// ---------------------------------------------------------------------------
// OpenAI 兼容 chat completions 客户端（流式 SSE + 非流式）
// 零依赖手写 SSE 解析；诊断信息一律走抛错/返回值，不依赖 console
// （生产构建 terser drop_console 会剥掉 console）。
//
// 成本相关的两件事在这里收口：
//   ① `stream_options:{include_usage:true}` —— 不带这个字段，很多端点（尤其各类中转）
//      在流式响应里根本不回 usage，面板就永远看不到 token 消耗；DeepSeek 还会在
//      usage 里给 prompt_cache_hit_tokens，缓存命中率据此可见。
//   ② `max_tokens` —— 输出不设上限时，一次跑偏的回答能把输出 token 顶到很高。
// 端点明确拒绝字段时精确降级重试；输出上限换用 max_completion_tokens，不能悄悄删除。
// ---------------------------------------------------------------------------
import { normalizeUsage, MAX_REQUEST_CHARS } from './aiContext';

// 结构化错误：kind 用于 UI 分类文案
// 'auth' | 'rate_limit' | 'context_length' | 'invalid_request' | 'timeout' | 'abort' | 'network' | 'server' | 'bad_response'
export class AiClientError extends Error {
    constructor(kind, status, message) {
        super(message);
        this.kind = kind;
        this.status = status || 0;
    }
}

const FIRST_BYTE_TIMEOUT_MS = 120000;   // 首字节与帧间等待分别计时，避免流永久卡住

// 请求体统一构造。relax 用于端点不认扩展字段时逐级降级（见 chatCompletionStream）。
function buildBody(opts, stream, relax) {
    const r = relax || {};
    const body = {
        model: opts.model,
        messages: opts.messages,
        ...(r.noTemperature ? {} : { temperature: opts.temperature }),
        stream,
    };
    if (opts.disableThinking) body.thinking = { type: 'disabled' };
    if (opts.tools && opts.tools.length) body.tools = opts.tools;
    // max_tokens 只在**明确大于 0** 时才发。默认 0 = 不发送，这不是省事而是必须：
    // 推理模型（deepseek-flash / reasoner 之类）的思维链也算在 max_tokens 里，设小了会
    // 出现"思考把额度吃光 → content 为空 → finish_reason=length"——实测 max_tokens=256
    // 时 reasoning 499 字符、content 0 字符。旧版不发送该字段，从不会这样截断。
    if (Number.isFinite(opts.maxTokens) && opts.maxTokens > 0) {
        body[r.completionTokens ? 'max_completion_tokens' : 'max_tokens'] = Math.floor(opts.maxTokens);
    }
    // 只有流式才需要（非流式的 usage 本来就在 JSON 里）
    if (stream && !r.noStreamOptions) body.stream_options = { include_usage: true };
    return body;
}

// HTTP 非 2xx → 分类错误
async function httpError(res) {
    const text = await res.text().catch(() => '');
    let kind = 'server';
    if (res.status === 401 || res.status === 403) kind = 'auth';
    else if (res.status === 429) kind = 'rate_limit';
    else if (/context_length_exceeded|context.*(?:length|window|limit).*(?:exceed|maximum|too)|maximum context|too many tokens/i.test(text)) kind = 'context_length';
    else if (res.status >= 400 && res.status < 500) kind = 'invalid_request';
    let msg;
    if (kind === 'auth') msg = 'API Key 无效或无权限，请检查设置';
    else if (kind === 'rate_limit') msg = '请求过快或额度不足';
    else if (kind === 'context_length') msg = '模型实际上下文容量不足。请在设置中填写该端点支持的真实窗口、降低每轮输入上限或回复长度上限，再继续任务。' + text.slice(0, 300);
    else if (/reasoning_content/i.test(text)) msg = '该模型的思考模式要求回传推理内容，与本助手仅展示思考的策略不兼容。请在设置中关闭模型思考模式（端点需支持 thinking 参数），或更换模型。' + text.slice(0, 200);
    else if (kind === 'invalid_request') msg = `请求被拒绝 (HTTP ${res.status})：${text.slice(0, 300)}。请检查模型名、工具调用支持和生成参数；未自动重发。`;
    else msg = `服务端错误 (HTTP ${res.status})${text ? `: ${text.slice(0, 200)}` : ''}`;
    return new AiClientError(kind, res.status, msg);
}

/**
 * 发起一次 chat completion。
 * @param {Object} opts
 * @param {string} opts.baseUrl  opts.apiKey  opts.model  opts.temperature
 * @param {Array}  opts.messages OpenAI 消息格式（system/user/assistant/tool）
 * @param {Array}  [opts.tools]  tool schema 数组
 * @param {boolean}[opts.stream=true]  false 走非流式 JSON 分支
 * @param {number} [opts.maxTokens]  输出 token 上限（端点不认该字段时自动降级重试）
 * @param {AbortSignal}[opts.signal]
 * @param {(delta:{content?:string})=>void}[opts.onDelta] 文本增量回调（仅流式）
 * @param {(delta:{content?:string})=>void}[opts.onReasoning] 推理模型的思考增量回调（仅流式，仅用于展示）
 * @returns {Promise<{content:string, reasoning:string, toolCalls:Array<{id,name,arguments}>, finishReason:string, usage?:{inputTokens:number,cacheReadTokens:number,outputTokens:number,totalTokens:number}}>}
 *          usage 已归一化：inputTokens 不含缓存命中，命中单独在 cacheReadTokens（DeepSeek 的
 *          prompt_tokens 把命中算在内，直接展示会让人误判"每一轮都在烧全价"）。
 */
export async function chatCompletionStream(opts) {
    let stream = opts.stream !== false;
    const ctrl = new AbortController();
    let timedOut = false;   // 区分超时中断与用户中断
    let timer = setTimeout(() => { timedOut = true; ctrl.abort(); }, FIRST_BYTE_TIMEOUT_MS);
    // 外部 signal（用户停止）转发到内部 ctrl
    let onOuterAbort;
    if (opts.signal) {
        onOuterAbort = () => ctrl.abort();
        if (opts.signal.aborted) ctrl.abort();
        else opts.signal.addEventListener('abort', onOuterAbort);
    }

    try {
        let res;
        let relax = {};
        for (let attempt = 0; ; attempt++) {
            try {
                const body = JSON.stringify(buildBody(opts, stream, relax));
                if (body.length > MAX_REQUEST_CHARS) throw new AiClientError('context_length', 0,
                    '请求体超过 2Mi 字符浏览器保护上限，未发送。请缩短需求或降低每轮输入上限。');
                res = await fetch(opts.url, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                        Authorization: `Bearer ${opts.apiKey}`,
                    },
                    body,
                    signal: ctrl.signal,
                });
            } catch (e) {
                clearTimeout(timer);
                if (e instanceof AiClientError) throw e;
                if (e.name === 'AbortError') {
                    if (timedOut) throw new AiClientError('timeout', 0, '请求超时（120s 无响应）');
                    throw new AiClientError('abort', 0, '已停止');
                }
                throw new AiClientError('network', 0, '网络错误（检查 API 地址，或该端点不支持浏览器跨域直连）');
            }
            if (res.ok) break;

            // 仅对明确拒绝的参数重试；未知 400、上下文超限和已开始生成的响应不重发。
            if (res.status === 400 && attempt < 4) {
                const peek = await res.clone().text().catch(() => '');
                const contextError = /context_length|maximum context|context.*exceed|too many tokens/i.test(peek);
                const rejected = /unknown|unrecognized|unsupported|not supported|not allowed|use .*instead/i.test(peek);
                let next = null;
                if (!contextError && rejected) {
                    if (/stream_options/i.test(peek) && stream && !relax.noStreamOptions) next = { ...relax, noStreamOptions: true };
                    else if (/max_tokens/i.test(peek) && opts.maxTokens > 0 && !relax.completionTokens) next = { ...relax, completionTokens: true };
                    else if (/temperature/i.test(peek) && !relax.noTemperature) next = { ...relax, noTemperature: true };
                    else if (/\bstream\b/i.test(peek) && stream) { stream = false; next = { ...relax, noStreamOptions: true }; }
                }
                if (next) {
                    relax = next;
                    clearTimeout(timer);
                    timer = setTimeout(() => { timedOut = true; ctrl.abort(); }, FIRST_BYTE_TIMEOUT_MS);
                    continue;
                }
            }
            clearTimeout(timer);
            throw await httpError(res);
        }

        try {
            if (!stream || /application\/json/i.test(res.headers.get('content-type') || '')) return await parseNonStream(res);
            return await parseStream(res, ctrl, opts, () => { clearTimeout(timer); });
        } catch (e) {
            if (e instanceof AiClientError) throw e;
            if (e.name === 'AbortError') {
                if (timedOut) throw new AiClientError('timeout', 0, '请求超时');
                throw new AiClientError('abort', 0, '已停止');
            }
            throw new AiClientError('bad_response', 0, `响应解析失败: ${e.message}`);
        }
    } finally {
        clearTimeout(timer);
        if (opts.signal && onOuterAbort) opts.signal.removeEventListener('abort', onOuterAbort);
    }
}

// ── 非流式分支 ──
async function parseNonStream(res) {
    let json;
    try { json = await res.json(); } catch (e) {
        throw new AiClientError('bad_response', 0, '响应不是有效 JSON');
    }
    if (json.error) throw new AiClientError('bad_response', 0, `模型返回错误：${json.error.message || JSON.stringify(json.error)}`);
    const choice = json.choices && json.choices[0];
    if (!choice?.message) throw new AiClientError('bad_response', 0, '响应缺少 choices/message，请检查端点是否兼容 Chat Completions');
    const msg = choice.message;
    return {
        content: (msg && msg.content) || '',
        reasoning: (msg && (msg.reasoning_content || msg.reasoning)) || '',
        // 归一化：个别端点（非流式透传/中转）可能缺 type 字段，补齐 'function'
        toolCalls: ((msg && msg.tool_calls) || []).map((tc) => ({ ...tc, type: tc.type || 'function' })),
        finishReason: (choice && choice.finish_reason) || '',
        usage: normalizeUsage(json.usage),
    };
}

// ── 流式分支：SSE 按行解析 ──
async function parseStream(res, ctrl, opts, onFirstByte) {
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    let content = '';
    let reasoning = '';      // 推理模型的思维链（reasoning_content / reasoning）
    const tcMap = new Map();    // index → {id, name, args}
    let finishReason = '';
    let usage;
    let gotFirstByte = false;
    let gotDone = false;
    let responseBytes = 0;
    let idleTimer;
    let idleExpired = false;

    const handleLine = (line) => {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith(':')) return;    // keep-alive 注释行
        if (!trimmed.startsWith('data:')) return;
        const payload = trimmed.slice(5).trim();
        if (payload === '[DONE]') { gotDone = true; return; }
        let frame;
        try { frame = JSON.parse(payload); } catch (e) { return; }   // 脏帧静默跳过
        if (frame.error) throw new AiClientError('bad_response', 0, `模型流返回错误：${frame.error.message || JSON.stringify(frame.error)}`);
        if (frame.usage) usage = frame.usage;
        const choice = frame.choices && frame.choices[0];
        if (!choice) return;
        if (choice.finish_reason) finishReason = choice.finish_reason;
        const delta = choice.delta || {};
        if (delta.content) {
            content += delta.content;
            if (opts.onDelta) opts.onDelta({ content: delta.content });
        }
        // 推理模型的思考流：**只用于展示进度，绝不回传**（DeepSeek 明确要求不要把
        // reasoning_content 放回下一轮请求；它也不该占我们的上下文预算）
        const think = delta.reasoning_content !== undefined ? delta.reasoning_content : delta.reasoning;
        if (think) {
            reasoning += think;
            if (opts.onReasoning) opts.onReasoning({ content: think });
        }
        if (delta.tool_calls) {
            for (const tc of delta.tool_calls) {
                // index 缺失的端点按 id 兜底归并
                const key = tc.index !== undefined && tc.index !== null ? tc.index : (tc.id || 0);
                let acc = tcMap.get(key);
                if (!acc) { acc = { id: '', name: '', args: '' }; tcMap.set(key, acc); }
                if (tc.id) acc.id = tc.id;
                if (tc.function && tc.function.name) acc.name = tc.function.name;
                if (tc.function && tc.function.arguments) acc.args += tc.function.arguments;
            }
        }
    };

    try {
        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            if (!gotFirstByte) { gotFirstByte = true; onFirstByte(); }
            clearTimeout(idleTimer);
            idleTimer = setTimeout(() => { idleExpired = true; ctrl.abort(); }, FIRST_BYTE_TIMEOUT_MS);
            responseBytes += value.byteLength;
            if (responseBytes > 8 * 1024 * 1024) throw new AiClientError('bad_response', 0,
                '响应流超过 8MiB 保护上限，未执行本轮工具。请拆分任务或设置合理的回复长度上限。');
            buffer += decoder.decode(value, { stream: true });
            let nl;
            while ((nl = buffer.indexOf('\n')) >= 0) {
                const line = buffer.slice(0, nl);
                buffer = buffer.slice(nl + 1);
                handleLine(line);
            }
        }
        buffer += decoder.decode();
        if (buffer) handleLine(buffer);   // 残尾
    } catch (e) {
        if (idleExpired) throw new AiClientError('timeout', 0, '模型响应流停滞 120s，请检查端点或网络后继续任务');
        throw e;
    } finally {
        clearTimeout(idleTimer);
        await reader.cancel().catch(() => {});
        reader.releaseLock();
    }

    if (!gotDone && !finishReason) throw new AiClientError('bad_response', 0, '响应流提前结束，内容可能不完整。请检查网络后重试；本轮未执行工具。');

    // 流式 delta 不携带 type/function 嵌套结构，重组为 OpenAI wire 格式回传：
    // {id, type:'function', function:{name, arguments}}，否则 DeepSeek 报
    // "missing field `type`"（反序列化只报首个缺失字段）
    const toolCalls = Array.from(tcMap.values())
        .filter((v) => v.name)
        .map((v) => ({ id: v.id, type: 'function', function: { name: v.name, arguments: v.args || '{}' } }));
    return { content, reasoning, toolCalls, finishReason, usage: normalizeUsage(usage) };
}
