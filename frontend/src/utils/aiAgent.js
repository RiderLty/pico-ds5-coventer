// ---------------------------------------------------------------------------
// Agent 循环：多轮 tool call、消息历史（模块级单例）、上下文预算与压缩。
//
// 与旧版的关键差别（都是"效果没变但 token 大降"的改动）：
//   ① 发出去的消息统一经 aiContext.planApiMessages 组装：剥掉 UI 用的 _meta
//      （旧版把工具结果按两份字节发出去），并按预算做**不可逆**压缩（旧版每轮滑窗，
//      前缀每轮都变 → 服务端前缀缓存全废）。
//   ② 轮数从 50 收到 16，并有任务级 token 预算闸：跑飞之前先停。
//   ③ 无进展检测：连续两次"同工具同参数"先注入提醒、再犯直接停（旧版会一路撞到 50 轮）。
//   ④ usage 逐轮回传 UI（含缓存命中），用户第一次能看见钱花在哪。
// 工具结果进历史前做内容级裁剪（日志类头尾保留），脚本读取窗口由工具自己限长，不动。
// ---------------------------------------------------------------------------
import { chatCompletionStream, AiClientError } from './aiClient';
import { buildChatUrl, normalizeAiCfg } from './aiSettings';
import { buildSystemPromptWithDocs } from './aiPrompt';
import { AI_TOOLS, executeTool } from './aiTools';
import {
    planApiMessages, toolCallSignature, normalizeUsage, addUsage,
    pruneToolResult, estimateToolsTokens,
} from './aiContext';

// 单次任务的最大工具调用轮数。旧版 50 轮配合"每轮重发全量上下文"，一次跑飞能烧掉几百万
// token；收到 16 轮（正常任务 3~9 轮跑完）足够用，超了强制模型收口总结。
const MAX_ROUNDS_DEFAULT = 16;
// 单次任务的输入 token 预算（估算值累计）。超了就停——宁可少做一步，也别把额度烧穿。
const TASK_BUDGET_DEFAULT = 150000;
// 无进展检测：连续第 2 次同工具同参数 → 注入提醒；第 3 次 → 直接停。
const REPEAT_WARN_AT = 2;
const REPEAT_STOP_AT = 3;
const CHAT_KEY = 'ds5_ai_chat_v1';         // 聊天历史 localStorage key
const CHAT_MAX_CHARS = 512 * 1024;          // 历史持久化体积上限（超出按原子块丢最旧）

// 模块级单例：折叠侧栏/关闭对话框后历史保留，刷新页面后从 localStorage 恢复
const session = {
    messages: loadPersisted(),   // {role, content?, tool_calls?, tool_call_id?, _meta?}
    running: false,
    // 上一次请求的估算上下文占用（UI 直接读，不必自己重算）
    lastPlan: null,
    // 最近一次任务的用量（含缓存命中）
    lastUsage: null,
};

// ── 聊天历史 localStorage 持久化 ──
function loadPersisted() {
    try {
        const raw = localStorage.getItem(CHAT_KEY);
        if (!raw) return [];
        const arr = JSON.parse(raw);
        if (!Array.isArray(arr)) return [];
        // 结构过滤：只保留合法消息，坏档静默清空
        return toBlocksLocal(arr.filter((m) => m && typeof m === 'object'
            && (m.role === 'user' || m.role === 'assistant' || m.role === 'tool'))).flat();
    } catch (e) {
        return [];
    }
}

let saveTimer = null;

// 历史变更后 debounce 落盘；turn 收尾用 persistNow 强制立即保存
function scheduleSave() {
    if (saveTimer) clearTimeout(saveTimer);
    saveTimer = setTimeout(persistNow, 300);
}

// 持久化的体积上限：从最旧按原子块丢，直到装得下（至少保留一块）。
// 注意这里只管"本地存档"大小；发给 API 的大小由 aiContext 的 token 预算单独管。
function persistNow() {
    if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
    try {
        const blocks = toBlocksLocal(session.messages);
        const sizes = blocks.map((block) => block.reduce((n, m) => n + JSON.stringify(m).length + 1, 0));
        let size = sizes.reduce((a, b) => a + b, 1);
        let anchor = -1;
        for (let i = blocks.length - 1; i >= 0; i--) {
            if (blocks[i][0]?.role === 'user' && !blocks[i][0]._notice) { anchor = i; break; }
        }
        const kept = blocks.filter((block, i) => {
            if (size <= CHAT_MAX_CHARS || i === anchor || i === blocks.length - 1) return true;
            size -= sizes[i];
            return false;
        });
        const json = JSON.stringify(kept.flat());
        if (json.length > CHAT_MAX_CHARS) return; // 大目标不静默砍掉；本轮仍在内存中可继续。
        localStorage.setItem(CHAT_KEY, json);
    } catch (e) { /* localStorage 不可用/quota 超限，静默退化为内存历史 */ }
}

// 本地原子块切分（仅用于持久化裁剪；发送视图的切分在 aiContext 里，带更多规则）
function toBlocksLocal(messages) {
    const blocks = [];
    let i = 0;
    while (i < messages.length) {
        const m = messages[i];
        if (m && m.role === 'assistant' && Array.isArray(m.tool_calls) && m.tool_calls.length) {
            const block = [m];
            i++;
            while (i < messages.length && messages[i].role === 'tool') { block.push(messages[i]); i++; }
            const ids = m.tool_calls.map((call) => call.id);
            const results = block.slice(1).map((result) => result.tool_call_id);
            if (ids.every(Boolean) && new Set(ids).size === ids.length && results.length === ids.length
                && ids.every((id) => results.filter((r) => r === id).length === 1)) blocks.push(block);
        } else {
            if (m.role !== 'tool') blocks.push([m]);
            i++;
        }
    }
    return blocks;
}

export function getSessionMessages() { return session.messages; }
export function isAgentRunning() { return session.running; }
export function getLastPlan() { return session.lastPlan; }
export function getLastUsage() { return session.lastUsage; }
export function clearHistory() {
    if (session.running) return;
    session.messages = [];
    session.lastPlan = null;
    session.lastUsage = null;
    persistNow();
}

/** 只读地算一次"现在发出去会有多大"（UI 显示上下文占用；不产生任何副作用）。 */
export function peekContextPlan(cfg) {
    const { prompt } = buildSystemPromptWithDocs();
    const plan = planApiMessages(session.messages, prompt, {
        budgetTokens: cfg && cfg.contextBudgetTokens,
        toolsTokens: estimateToolsTokens(AI_TOOLS),
        toolsChars: JSON.stringify(AI_TOOLS).length,
        requestBudgetTokens: cfg?.requestBudgetTokens,
        outputReserveTokens: cfg?.maxTokens > 0 ? cfg.maxTokens : undefined,
        preview: true,
    });
    return plan.meta;
}

// 单次请求：仅收到明确 HTTP 5xx 拒绝时自动重试一次。
// 网络断开无法证明服务端未生成，避免重发造成重复费用。
async function requestWithRetry(cfg, apiMessages, signal, onDelta, onReasoning, allowTools = true) {
    let lastErr;
    for (let attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) await new Promise((r) => setTimeout(r, 1000));
        try {
            return await chatCompletionStream({
                url: buildChatUrl(cfg.baseUrl),
                apiKey: cfg.apiKey,
                model: cfg.model,
                temperature: cfg.temperature,
                disableThinking: cfg.disableThinking,
                maxTokens: cfg.maxTokens,
                messages: apiMessages,
                tools: allowTools ? AI_TOOLS : undefined,
                stream: cfg.stream !== false,
                signal,
                onDelta,
                onReasoning,
            });
        } catch (e) {
            lastErr = e;
            if (e instanceof AiClientError && e.kind === 'server' && e.status >= 500) continue;
            throw e;
        }
    }
    throw lastErr;
}

// ── 思维链（reasoning_content）的处置 ──────────────────────────────────────
// 只用于**展示**：挂在 assistant 消息的 `_reasoning` 字段上。sanitizeMessage 只保留
// role/content/tool_calls/tool_call_id，所以它永远不会被发回端点（需要推理回传的模型
// 应关闭 thinking 模式或换模型）；estimateMessageTokens 也不看这个字段，
// 因此它同样不占上下文预算。
// 持久化上限：思维链动辄几千上万字，而聊天存档上限只有 512KB（超了从最旧丢块），
// 不截断就会挤掉真正的对话内容。头 1400 + 尾 500 足够回看"当时在想什么"。
const REASONING_KEEP_CHARS = 2000;
export function capReasoning(text) {
    const s = (text || '').trim();
    if (s.length <= REASONING_KEEP_CHARS) return s;
    return `${s.slice(0, 1400)}\n\n[... 中间 ${s.length - 1900} 字思考已省略 ...]\n\n${s.slice(-500)}`;
}

/** 统一的 assistant 消息构造：正文 + 工具调用 +（只用于展示的）思维链。 */
function assistantMessage(resp) {
    const m = { role: 'assistant', content: resp.content || '' };
    if (resp.toolCalls && resp.toolCalls.length) m.tool_calls = resp.toolCalls;
    if (resp.reasoning) m._reasoning = capReasoning(resp.reasoning);
    return m;
}

/** 输出被截断时的统一说明（区分"完全没出内容"与"出到一半"）。 */
function truncatedError(hadContent) {
    return new Error(
        `回复被输出上限截断（finish_reason=length）${hadContent ? '，内容不完整' : '，一个字都没生成出来'}。`
        + `常见原因是"回复长度上限"设得太小——推理模型的思维链也算在这个额度里，`
        + `写脚本这类任务光思考就可能超过 4096。请到「AI 助手设置 → 回复长度上限」改成 0（不限制）。`);
}

/**
 * 跑一轮用户输入（多轮 tool call 直到模型给出最终回答）。
 * @param {string} userText
 * @param {Object} ctx 工具执行上下文 { getScript, applyScript }
 * @param {Object} opts { cfg, signal, onEvent }
 *   onEvent(ev): ev.type =
 *     'text_delta'{content} | 'tool_start'{call} | 'tool_result'{call,result}
 *     | 'compacted'{droppedBlocks,estimatedTokens} | 'usage'{round,maxRounds,usage,totalUsage,plan}
 *     | 'done' | 'error'{err}
 * @returns {Promise<{ok:boolean, aborted?:boolean, error?:Error}>}
 */
export async function runAgentTurn(userText, ctx, opts) {
    const { signal, onEvent } = opts;
    const cfg = normalizeAiCfg(opts.cfg);
    if (session.running) return { ok: false, error: new Error('已有任务在运行') };
    session.running = true;
    const maxRounds = Number.isFinite(cfg.maxRounds) && cfg.maxRounds > 0
        ? Math.min(40, Math.floor(cfg.maxRounds)) : MAX_ROUNDS_DEFAULT;
    const taskBudget = Number.isFinite(cfg.taskBudgetTokens) && cfg.taskBudgetTokens > 0
        ? Math.floor(cfg.taskBudgetTokens) : TASK_BUDGET_DEFAULT;
    let totalUsage = null;
    let spentInput = 0;
    let hadEstimatedUsage = false;
    const toolState = {};
    // 无进展检测的计数放在**循环状态**里，不从历史反推：我们往历史注入的提醒本身就是
    // 一条 user 消息，从历史反推会把连续计数打断，于是提醒永远无法升级成中止。
    let lastSig = null;
    let repeatCount = 0;
    let repeatName = '';

    try {
        // 先入历史再组装 prompt，避免用户消息在准备阶段"发出去就不见了"
        session.lastUsage = null;
        session.messages.push({ role: 'user', content: userText });
        scheduleSave();
        // system prompt 每轮开始时组装（文档已内置进面板，同步取，无网络开销）
        const { prompt: systemPrompt } = buildSystemPromptWithDocs();
        const toolsTokens = estimateToolsTokens(AI_TOOLS);

        for (let round = 0; round < maxRounds; round++) {
            if (signal?.aborted) return { ok: false, aborted: true };
            // ── 无进展检测：同一工具同一参数反复调用 = 纯烧钱 ──
            // 必须排在组装之前：这样注入的提醒能**跟着这一次请求**发出去，而不是晚一轮。
            if (repeatCount >= REPEAT_STOP_AT) {
                const err = new Error(
                    `已停止：连续 ${repeatCount} 次用相同参数调用 ${repeatName} 且没有进展。`
                    + `请换个思路，或直接说明卡在哪里。`);
                onEvent && onEvent({ type: 'error', err });
                return { ok: false, error: err };
            }
            if (repeatCount >= REPEAT_WARN_AT) {
                session.messages.push({
                    role: 'user',
                    content: `[系统提示] 你刚刚连续 ${repeatCount} 次用完全相同的参数调用了 ${repeatName}，`
                        + `结果没有变化。请不要再用同样的参数调用它：要么换参数/换工具，要么直接根据已有结果作答。`,
                    _notice: true,   // 不是真实用户消息 → 不参与压缩锚点选择
                });
            }

            // ── 组装发送视图：剥 _meta + 按预算压缩（压缩结果必须写回会话）──
            const plan = planApiMessages(session.messages, systemPrompt, {
                budgetTokens: cfg.contextBudgetTokens,
                toolsTokens, toolsChars: JSON.stringify(AI_TOOLS).length,
                requestBudgetTokens: cfg.requestBudgetTokens,
                outputReserveTokens: cfg.maxTokens > 0 ? cfg.maxTokens : undefined,
            });
            if (plan.compactedMessages) {
                session.messages = plan.compactedMessages;
                scheduleSave();
                onEvent && onEvent({
                    type: 'compacted',
                    droppedBlocks: plan.meta.droppedBlocks,
                    estimatedTokens: plan.meta.estimatedTokens,
                });
            }
            session.lastPlan = plan.meta;

            if (signal?.aborted) return { ok: false, aborted: true };
            if (plan.meta.overBudget) throw new Error('上下文无法安全缩减：当前目标、系统文档或工具参数超过输入容量。请提高每轮输入上限/模型窗口，或缩短本次需求；目标未被截断。');
            if (spentInput + plan.meta.estimatedTokens > taskBudget) {
                throw new Error(`已停止：下一轮预计输入 ${plan.meta.estimatedTokens} token，将超过任务预算 ${taskBudget}（已用 ${spentInput}）。请缩小每轮输入上限、拆分需求或提高任务预算。`);
            }
            onEvent && onEvent({ type: 'round_start', round: round + 1, maxRounds, plan: plan.meta });
            const resp = await requestWithRetry(cfg, plan.messages, signal,
                onEvent ? (d) => onEvent({ type: 'text_delta', content: d.content }) : undefined,
                onEvent ? (d) => onEvent({ type: 'reasoning', content: d.content }) : undefined);

            // 中断：半截响应不入历史
            if (signal && signal.aborted) return { ok: false, aborted: true };

            // ── 用量记账（含缓存命中）──
            const actualUsage = normalizeUsage(resp.usage);
            const usage = actualUsage || { inputTokens: plan.meta.estimatedTokens, cacheReadTokens: 0,
                outputTokens: 0, totalTokens: plan.meta.estimatedTokens };
            spentInput += usage.inputTokens + usage.cacheReadTokens;
            hadEstimatedUsage ||= !actualUsage;
            if (usage) {
                totalUsage = addUsage(totalUsage, usage);
                session.lastUsage = { round: round + 1, usage, total: totalUsage, estimated: !actualUsage, totalEstimated: hadEstimatedUsage, maxRounds, plan: plan.meta };
                onEvent && onEvent({
                    type: 'usage', round: round + 1, maxRounds, usage, totalUsage, estimated: !actualUsage, totalEstimated: hadEstimatedUsage, plan: plan.meta,
                });
                if (spentInput > taskBudget) {
                    const err = new Error(
                        `已停止：本次任务输入已达 ${spentInput} token，超过预算 ${taskBudget}。`
                        + `可以调小"上下文预算"、清空对话后重开，或在设置里提高任务预算。`);
                    onEvent && onEvent({ type: 'error', err });
                    return { ok: false, error: err };
                }
            }

            // ── 截断（finish_reason=length）必须显式处理，不能当成"答完了" ──
            // 推理模型的思维链与正文共用输出额度，额度被思考吃光时 content 会是空串、
            // 而 finish_reason 仍是 length。旧版直接把它当正常结束 → 界面上什么都不显示
            // （用户看到的现象就是"思考了一会儿然后直接停了，啥也没有"）。
            const truncated = resp.finishReason === 'length';
            const hasToolCalls = !!(resp.toolCalls && resp.toolCalls.length);
            if (truncated) {
                if (!resp.content || hasToolCalls) {
                    const err = truncatedError(false);
                    onEvent && onEvent({ type: 'error', err });
                    return { ok: false, error: err };
                }
                // 有内容但不完整：把已生成的部分留在历史里给用户看，同时明确告知被截断
                session.messages.push(assistantMessage(resp));
                const err = truncatedError(true);
                onEvent && onEvent({ type: 'error', err });
                return { ok: false, error: err };
            }


            const assistant = assistantMessage(resp);

            // ── 收尾：模型不再调工具 ──
            if (!resp.toolCalls || !resp.toolCalls.length) {
                // 空回答**绝不静默通过**：界面上会表现为"助手气泡是空的、什么提示都没有"，
                // 用户完全无从判断发生了什么（真机踩过）。
                if (!assistant.content.trim()) {
                    const err = new Error('模型没有返回任何内容（也没有工具调用）。'
                        + '可能原因：该端点不支持这个模型名、输出上限被思考吃光、或服务端返回了空响应。'
                        + '可在设置里确认模型名与「回复长度上限」（建议 0=不限制）后重试。');
                    onEvent && onEvent({ type: 'error', err });
                    return { ok: false, error: err };
                }
                session.messages.push(assistant);
                scheduleSave();
                onEvent && onEvent({ type: 'done' });
                return { ok: true };
            }

            const ids = resp.toolCalls.map((call) => call.id);
            if (ids.some((id) => !id) || new Set(ids).size !== ids.length) {
                throw new Error('端点返回的工具调用 ID 缺失或重复，未执行。请更换支持标准 tool_calls 的模型或端点。');
            }
            // 本轮新读取的结果要到下一次请求才会被模型看到，不能授权本轮猜测的编辑。
            toolState.editWindows = toolState.readScript === ctx.getScript() ? [...(toolState.readWindows || [])] : [];
            const progressResults = [];
            const mark = session.messages.length;
            session.messages.push(assistant);
            scheduleSave();

            // 顺序执行 tool calls（并行会竞争 script 快照）。
            // 一轮里模型给多个工具调用是**免费**的（同一次请求），所以提示词鼓励批量编辑。
            for (const call of resp.toolCalls) {
                if (signal && signal.aborted) {
                    // 保留已生效操作的回执，未执行的调用补齐中止结果。
                    for (const pending of resp.toolCalls.slice(session.messages.length - mark - 1)) {
                        session.messages.push({ role: 'tool', tool_call_id: pending.id,
                            content: JSON.stringify({ ok: false, aborted: true, error: '用户停止，未执行此调用' }) });
                    }
                    return { ok: false, aborted: true };
                }
                onEvent && onEvent({ type: 'tool_start', call });
                const result = await executeTool(call, { ...ctx, signal, toolState });
                // 进上下文前裁剪（_meta 给 UI，content 给 API；两者分开，避免重复计费）
                const toolName = (call.function && call.function.name) || '';
                const pruned = pruneToolResult(toolName, result);
                progressResults.push(pruned.content);
                if (toolName === 'read_current_script' && result.ok && pruned.removedChars > 0 && typeof pruned.value.script === 'string') {
                    toolState.readWindows.pop();
                    toolState.readWindows.push(pruned.value.script);
                }
                session.messages.push({
                    role: 'tool', tool_call_id: call.id,
                    content: pruned.content,
                    _meta: { name: toolName, result: result, prunedChars: pruned.removedChars },
                });
                scheduleSave();
                onEvent && onEvent({ type: 'tool_result', call, result: result });
            }

            // 多工具批次也参与检测，不能靠重复调用两个工具绕开保护。
            const sig = resp.toolCalls.map(toolCallSignature).join('\n') + JSON.stringify(progressResults);
            if (sig === lastSig) repeatCount++;
            else { lastSig = sig; repeatCount = 1; }
            repeatName = resp.toolCalls.map((c) => c.function.name).join(', ');
        }

        // 轮次上限：追加收口请求（不再允许调用工具）
        session.messages.push({
            role: 'user',
            _notice: true,
            content: `[系统提示] 工具调用轮数已达上限（${maxRounds} 轮），请直接总结当前改动状态，不要再调用工具。`,
        });
        const plan = planApiMessages(session.messages, systemPrompt, {
            budgetTokens: cfg.contextBudgetTokens, toolsTokens: 0,
            requestBudgetTokens: cfg.requestBudgetTokens,
            outputReserveTokens: cfg.maxTokens > 0 ? cfg.maxTokens : undefined,
        });
        if (plan.compactedMessages) {
            session.messages = plan.compactedMessages;
            onEvent && onEvent({ type: 'compacted', droppedBlocks: plan.meta.droppedBlocks, estimatedTokens: plan.meta.estimatedTokens });
        }
        session.lastPlan = plan.meta;
        if (signal?.aborted) return { ok: false, aborted: true };
        if (plan.meta.overBudget || spentInput + plan.meta.estimatedTokens > taskBudget) {
            throw new Error('工具轮数已耗尽，剩余预算不足以总结。已完成的编辑和工具回执保留，请查看编辑器并拆分后续需求。');
        }
        onEvent && onEvent({ type: 'round_start', round: maxRounds + 1, maxRounds: maxRounds + 1, plan: plan.meta, summarizing: true });
        const resp = await requestWithRetry(cfg, plan.messages, signal,
            onEvent ? (d) => onEvent({ type: 'text_delta', content: d.content }) : undefined,
            onEvent ? (d) => onEvent({ type: 'reasoning', content: d.content }) : undefined, false);
        const actualUsage = normalizeUsage(resp.usage);
        const usage = actualUsage || { inputTokens: plan.meta.estimatedTokens, cacheReadTokens: 0,
            outputTokens: 0, totalTokens: plan.meta.estimatedTokens };
        if (usage) {
            totalUsage = addUsage(totalUsage, usage);
            hadEstimatedUsage ||= !actualUsage;
            session.lastUsage = { round: maxRounds + 1, maxRounds: maxRounds + 1, usage, total: totalUsage, estimated: !actualUsage, totalEstimated: hadEstimatedUsage, plan: plan.meta };
            onEvent && onEvent({ type: 'usage', round: maxRounds + 1, maxRounds: maxRounds + 1, usage, totalUsage, estimated: !actualUsage, totalEstimated: hadEstimatedUsage, plan: plan.meta });
        }
        if (signal?.aborted) return { ok: false, aborted: true };
        if (resp.finishReason === 'length') {
            if (resp.content && !resp.toolCalls?.length) session.messages.push(assistantMessage(resp));
            throw truncatedError(!!resp.content);
        }
        if (resp.toolCalls?.length) throw new Error('总结阶段模型仍返回工具调用，未执行。请选择支持工具规划的模型后继续。');
        if (!resp.content.trim()) throw new Error('模型没有返回任何内容：工具轮数耗尽后的总结为空。请检查模型与输出上限后继续。');
        session.messages.push(assistantMessage(resp));
        if (spentInput + usage.inputTokens + usage.cacheReadTokens > taskBudget) throw new Error('总结已返回，但累计输入超过任务预算，请提高预算或拆分后续需求。');
        onEvent && onEvent({ type: 'done' });
        return { ok: true };
    } catch (e) {
        const aborted = e instanceof AiClientError && e.kind === 'abort';
        onEvent && onEvent({ type: 'error', err: e });
        return { ok: false, aborted, error: e };
    } finally {
        session.running = false;
        persistNow();   // turn 收尾强制落盘，防止用户随即关页面丢最后几条
    }
}
