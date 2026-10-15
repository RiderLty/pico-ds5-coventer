// ---------------------------------------------------------------------------
// AI 上下文管理（纯函数、零依赖 —— 可在 node 下直接单测：scripts/test-ai-context.mjs）
//
// 这一层把内存里的完整会话（消息带 _meta，供 UI 渲染工具卡片）编译成**发给 API 的 wire
// 消息**，并顺手解决旧版最费 token 的四件事：
//
//   ① sanitize —— 剥掉 _meta 等非 wire 字段。旧版把工具结果存了两份（content 里一份
//      JSON 字符串 + _meta.result 一份对象），组装请求时又原样 flat() 出去，等于每个
//      工具结果按**两倍字节**计费。
//   ② 估算 token —— CJK 按 1 token/字符、其余按 3.5 字符/token，比"4 字符/token"保守，
//      实际分词随模型变化，仍需保留余量并处理端点的容量拒绝。
//   ③ 滞回压缩 —— **只在超过高水位时压缩，且一次压到低水位**。于是绝大多数轮次的消息
//      序列逐字节不变 → 服务端前缀缓存持续命中；只有越线那一轮才发生一次前缀变动。
//      旧版每轮都从最旧滑窗丢弃（maxHistoryRounds 只管非 tool 块、还带一个 120k 字符
//      硬保险），等于每轮都把前缀打乱，缓存全废：实测稳态单次请求 6.8 万 token 全部按
//      miss 计费。
//   ④ 锚点 —— 永远保留"最近一条用户消息"所在的原子块（当前任务目标）；优先移除旧任务和当前任务较早的工具块。原子块 = assistant(tool_calls) 及其全部 tool 结果，整体丢弃，绝不拆散配对
//      （拆散会被服务端判为非法消息序列）。
//
// 工具结果的内容级裁剪（头/尾保留 + 中间标记）也在这里：pruneText / pruneToolResult。
// ---------------------------------------------------------------------------

/** 工具结果内容裁剪预算（照抄成熟实现的经验值：8192 阈值 / 4096 头 / 1024 尾）。 */
export const PRUNE_DEFAULTS = Object.freeze({
    thresholdChars: 8192,
    headChars: 4096,
    tailChars: 1024,
});

/** 默认上下文预算（估算 token）——**整次请求**的目标上限，含 system prompt 与 tools schema。
 *  实测固定开销就有 ~10.9k（内置 API 文档 8.8k + tools 1.7k），所以预算必须给历史留出额度。
 *  默认单轮输入上限跟随模型窗口；用户可以另设更小的值来控制费用和延迟。 */
export const DEFAULT_BUDGET_TOKENS = 1000000;
export const HIGH_WATER_RATIO = 0.80;
export const LOW_WATER_RATIO = 0.40;
/** 旧调用的兼容导出；不再用下限虚增模型真实容量。 */
export const HISTORY_MIN_TOKENS = 4000;
// 默认跟随 contextBudgetTokens；保留导出名兼容旧调用方和配置迁移。
export const DEFAULT_REQUEST_BUDGET_TOKENS = DEFAULT_BUDGET_TOKENS;
export const DEFAULT_OUTPUT_RESERVE_TOKENS = 8192;
export const MAX_REQUEST_CHARS = 2 * 1024 * 1024;

export function contextCapacity(value) {
    const n = Number(value);
    return Number.isFinite(n) && n > 0 ? Math.floor(n) : DEFAULT_BUDGET_TOKENS;
}

export function formatTokens(n) {
    if (!Number.isFinite(n) || n <= 0) return '0';
    if (n >= 1000000) return `${+(n / 1000000).toFixed(2)}M`;
    if (n >= 1000) return `${+(n / 1000).toFixed(1)}K`;
    return String(Math.round(n));
}

/** 压缩占位文本里引用的省略说明（中文，与提示词语言一致）。 */
const COMPACT_NOTE = (n) =>
    `[系统提示] 为控制上下文长度，已省略 ${n} 段较早的记录（更早的对话，或本次任务里已读过的`
    + `脚本/日志/编辑回执）。编辑器里的脚本始终是最新版本，需要时请重新读取；`
    + `也不要因为看不到旧记录就重复之前的操作。`;

// ── token 估算 ──────────────────────────────────────────────────────────────

/**
 * 保守估算一段文本的 token 数：CJK/全角按 1 token/字符，其余按 3.5 字符/token，
 * 另加每条消息的固定开销。宁可高估（提前压缩）也不要低估（撞端点上限被 400）。
 * @param {string} text
 * @returns {number}
 */
export function estimateTokens(text) {
    if (!text) return 0;
    let wide = 0;
    let narrow = 0;
    for (const ch of text) {
        // 0x2E80 起：CJK 部首/汉字、全角标点、假名、谚文 —— 这些在 BPE 里基本 1 字 1 token 起
        if (ch.codePointAt(0) >= 0x2e80) wide++;
        else narrow++;
    }
    return Math.ceil(wide + narrow / 3.5) + 4;
}

/** 单条消息的估算 token（含 tool_calls 的参数）。 */
export function estimateMessageTokens(msg) {
    if (!msg) return 0;
    let n = 6 + estimateTokens(typeof msg.content === 'string' ? msg.content : '');
    if (Array.isArray(msg.tool_calls)) {
        for (const tc of msg.tool_calls) {
            n += 8 + estimateTokens(tc?.id || '');
            n += estimateTokens(tc && tc.function ? tc.function.name : '');
            n += estimateTokens(tc && tc.function ? stringifyArgs(tc.function.arguments) : '');
        }
    }
    return n;
}

/** tools schema 的估算 token（每轮都要随请求发送，属于固定开销）。 */
export function estimateToolsTokens(tools) {
    if (!Array.isArray(tools) || !tools.length) return 0;
    try { return estimateTokens(JSON.stringify(tools)); } catch (e) { return 0; }
}

// ── wire 化 ────────────────────────────────────────────────────────────────

function stringifyArgs(args) {
    if (typeof args === 'string') return args;
    if (args === undefined || args === null) return '{}';
    try { return JSON.stringify(args); } catch (e) { return '{}'; }
}

/**
 * 把内存消息转成 API wire 格式：只保留 role/content/tool_calls/tool_call_id/name。
 * `_meta`（UI 渲染工具卡片用）以及任何自定义字段一律剥掉——它们不该计费。
 * @param {Object} m
 * @returns {Object}
 */
export function sanitizeMessage(m) {
    if (!m || typeof m !== 'object') return { role: 'user', content: '' };
    const out = { role: m.role };
    if (typeof m.content === 'string') out.content = m.content;
    else if (m.content === undefined || m.content === null) out.content = '';
    else out.content = String(m.content);
    if (m.role === 'tool' && m.tool_call_id) out.tool_call_id = m.tool_call_id;
    if (Array.isArray(m.tool_calls) && m.tool_calls.length) {
        out.tool_calls = m.tool_calls.map((tc) => ({
            id: tc && tc.id ? tc.id : '',
            type: 'function',
            function: {
                name: (tc && tc.function && tc.function.name) || '',
                arguments: stringifyArgs(tc && tc.function ? tc.function.arguments : '{}'),
            },
        }));
    } else if (m.role === 'assistant') {
        out.content = out.content || '';
    }
    return out;
}

// ── 原子块 ────────────────────────────────────────────────────────────────

/**
 * 把消息序列切成原子块：assistant(tool_calls) + 其后连续的 tool 消息算一块，其余单条一块。
 * 裁剪以块为单位，保证 tool 配对永不被拆散。
 * @param {Array} messages
 * @returns {Array<Array>}
 */
export function toBlocks(messages) {
    const blocks = [];
    let i = 0;
    while (i < messages.length) {
        const m = messages[i];
        if (m && m.role === 'assistant' && Array.isArray(m.tool_calls) && m.tool_calls.length) {
            const block = [m];
            i++;
            while (i < messages.length && messages[i].role === 'tool') { block.push(messages[i]); i++; }
            blocks.push(block);
        } else {
            blocks.push([m]);
            i++;
        }
    }
    return blocks;
}

function blockTokens(block) {
    let n = 0;
    for (const m of block) n += estimateMessageTokens(m);
    return n;
}

function blocksTokens(blocks) {
    let n = 0;
    for (const b of blocks) n += blockTokens(b);
    return n;
}

// ── 内容级裁剪 ─────────────────────────────────────────────────────────────

/**
 * 头/尾保留、中间换成标记。阈值内原样返回（保证"不超阈值就不动"）。
 * @param {string} text
 * @param {{thresholdChars:number,headChars:number,tailChars:number}} [cfg]
 * @param {string} [marker] 标记文本（JSON 字符串场景要传转义后的，保持 JSON 合法）
 * @returns {{text:string, removedChars:number}}
 */
export function pruneText(text, cfg, marker) {
    const c = cfg || PRUNE_DEFAULTS;
    const s = typeof text === 'string' ? text : '';
    if (s.length <= c.thresholdChars) return { text: s, removedChars: 0 };
    const mk = marker === undefined ? '\n\n[... 内容中间部分已省略 ...]\n\n' : marker;
    const head = s.slice(0, c.headChars);
    const tail = s.slice(Math.max(c.headChars, s.length - c.tailChars));
    return { text: head + mk + tail, removedChars: s.length - (head.length + mk.length + tail.length) };
}

/** 按工具名给内容裁剪预算：脚本读取是"要改的目标"，放宽；日志/诊断按默认值。 */
export function pruneConfigForTool(name) {
    if (name === 'read_current_script') {
        return { thresholdChars: 16384, headChars: 8192, tailChars: 2048 };
    }
    return PRUNE_DEFAULTS;
}

/**
 * 工具结果进上下文前的裁剪。保持返回值仍是**合法 JSON**：
 *   ① 日志类（带 lines 数组）→ 保留前 10 行 + 后 20 行，中间插一行省略标记；
 *   ② 其余 → 找最长的字符串字段做头/尾裁剪（标记用转义形式，JSON 依旧合法）。
 * 阈值内一律原样返回（不打扰模型）。
 * @param {string} name 工具名
 * @param {Object} result 工具返回值
 * @returns {{value:Object, content:string, removedChars:number}}
 */
export function pruneToolResult(name, result) {
    const cfg = pruneConfigForTool(name);
    let full;
    try { full = JSON.stringify(result); } catch (e) { full = '{"ok":false,"error":"结果无法序列化"}'; }
    if (full.length <= cfg.thresholdChars) return { value: result, content: full, removedChars: 0 };

    // ① 日志类：按行做结构化裁剪，比砍 JSON 字符串更可读
    if (result && Array.isArray(result.lines) && result.lines.length > 30) {
        const head = result.lines.slice(0, 10);
        const tail = result.lines.slice(-20);
        const v = {
            ...result,
            lines: [...head, `[... 中间省略 ${result.lines.length - 30} 行 ...]`, ...tail],
            truncated: true,
            truncateNote: '日志过长，仅保留前 10 行与后 20 行；看特定内容请用 get_device_logs 的 grep 参数',
        };
        const content = JSON.stringify(v);
        if (content.length < full.length) {
            return { value: v, content, removedChars: full.length - content.length };
        }
    }

    // ② 兜底：裁剪最长的字符串字段。标记用 \n 转义（在 JSON 字符串内即合法）
    const v = { ...result };
    let key = null;
    let len = 0;
    for (const [k, val] of Object.entries(v)) {
        if (typeof val === 'string' && val.length > len) { key = k; len = val.length; }
    }
    if (key && len > cfg.thresholdChars) {
        const r = pruneText(v[key], cfg, '\\n\\n[... 中间内容已省略（可用按行窗口重新读取） ...]\\n\\n');
        v[key] = r.text;
        v.truncated = true;
        v.truncateNote = `字段 ${key} 过长已截断（保留头尾）；需要中间内容请用窗口参数重新读取`;
        const content = JSON.stringify(v);
        return { value: v, content, removedChars: full.length - content.length };
    }
    return { value: result, content: full, removedChars: 0 };
}

// ── 无进展检测 ─────────────────────────────────────────────────────────────

function stableStringify(value) {
    if (value === null || typeof value !== 'object') return JSON.stringify(value);
    if (Array.isArray(value)) return `[${value.map(stableStringify).join(',')}]`;
    const keys = Object.keys(value).sort();
    return `{${keys.map((k) => `${JSON.stringify(k)}:${stableStringify(value[k])}`).join(',')}}`;
}

/**
 * 一次工具调用的规范化签名（工具名 + 参数），用于识别"同一件事反复做"。
 * 参数按 JSON 解析后按键排序，所以 `{"a":1,"b":2}` 与 `{ "b": 2, "a": 1 }` 视作同一次调用。
 * @param {Object} call OpenAI wire 格式的 tool_call
 * @returns {string}
 */
export function toolCallSignature(call) {
    const name = (call && call.function && call.function.name) || '';
    let args = call && call.function ? call.function.arguments : '{}';
    if (typeof args !== 'string') args = stringifyArgs(args);
    try { args = stableStringify(JSON.parse(args)); } catch (e) { /* 非法 JSON 就用原串 */ }
    return `${name}\u0000${args}`;
}

// ── usage 归一化 ───────────────────────────────────────────────────────────

/**
 * 把服务端 usage 归一成面板要显示的四个数。**幂等**：对已经归一化的对象再调用一次
 * 原样返回（aiClient 已归一化一次、aiAgent 记账时还会再过一道，不幂等会把数字清零——
 * 那会让"任务 token 闸"永远不触发）。
 * DeepSeek 的 prompt_tokens **包含**缓存命中；OpenAI 系把命中放在
 * prompt_tokens_details.cached_tokens。命中部分单独记账，才能分辨"量大"还是"没命中"。
 * @param {Object} raw
 * @returns {null|{inputTokens:number,cacheReadTokens:number,outputTokens:number,totalTokens:number}}
 */
export function normalizeUsage(raw) {
    if (!raw || typeof raw !== 'object') return null;
    const count = (n) => Number.isFinite(n) ? Math.max(0, n) : 0;
    if (Number.isFinite(raw.inputTokens) && !Number.isFinite(raw.prompt_tokens)) {
        const input = count(raw.inputTokens), cache = count(raw.cacheReadTokens), output = count(raw.outputTokens);
        return { inputTokens: input, cacheReadTokens: cache, outputTokens: output,
            totalTokens: Number.isFinite(raw.totalTokens) ? count(raw.totalTokens) : input + cache + output };
    }
    if (!Number.isFinite(raw.prompt_tokens) && !Number.isFinite(raw.input_tokens)) return null;
    const prompt = count(raw.prompt_tokens ?? raw.input_tokens);
    const completion = count(raw.completion_tokens ?? raw.output_tokens);
    const cacheRead = Math.min(prompt, count(raw.prompt_tokens_details?.cached_tokens
        ?? raw.input_tokens_details?.cached_tokens ?? raw.prompt_cache_hit_tokens));
    return {
        inputTokens: prompt - cacheRead,
        cacheReadTokens: cacheRead,
        outputTokens: completion,
        totalTokens: Number.isFinite(raw.total_tokens) ? count(raw.total_tokens) : prompt + completion,
    };
}

/** 累加两次 usage（都允许为 null）。 */
export function addUsage(a, b) {
    if (!a) return b || null;
    if (!b) return a;
    return {
        inputTokens: a.inputTokens + b.inputTokens,
        cacheReadTokens: a.cacheReadTokens + b.cacheReadTokens,
        outputTokens: a.outputTokens + b.outputTokens,
        totalTokens: a.totalTokens + b.totalTokens,
    };
}

// ── 组装发送视图 ───────────────────────────────────────────────────────────

/**
 * 最近一条**真实**用户消息所在的块下标（当前任务目标的锚点）；没有则 -1。
 * 必须跳过我们注入的系统提示（压缩通知、无进展提醒）——它们也是 user 角色，
 * 若被当成锚点，锚点就会一路往后漂，真正的任务描述会在下一次压缩时被丢掉。
 */
function findAnchor(blocks) {
    for (let i = blocks.length - 1; i >= 0; i--) {
        const first = blocks[i] && blocks[i][0];
        if (first && first.role === 'user' && !first._notice) return i;
    }
    return -1;
}

/** 工具轮原子块：assistant(tool_calls) + 其 tool 结果。最占地方，也是最该先丢的。 */
function isToolBlock(block) {
    return block.length > 1 || (block[0] && block[0].role === 'tool');
}

/** 上一次压缩留下的通知块（压缩时清掉旧的，只保留一条最新的）。 */
function isNoticeBlock(block) {
    return block.length === 1 && block[0] && block[0]._compactNotice === true;
}

function noticeMessage(totalDropped) {
    return {
        role: 'user',
        content: COMPACT_NOTE(totalDropped),
        _notice: true,          // 不是真实用户消息 → 不参与锚点选择
        _compactNotice: true,   // UI 渲染成居中小字条；压缩时会被合并成一条
        _dropped: totalDropped,
    };
}

/**
 * 一次**不可逆**压缩：返回 { blocks, dropped, totalDropped }。
 *
 * 不可逆是关键——早先的版本每轮都从完整历史重算裁剪结果，于是被丢掉的块下一轮又"复活"，
 * 导致从第 3 轮起**每轮都是压缩轮、每轮前缀都变**，缓存照样全废（实测 12 轮里压了 10 次）。
 * 现在压缩结果会被写回会话，只有真正长回来才会再压一次。
 *
 * 丢的顺序（**最旧优先**，这是多轮对话的正确语义）：
 *   ① 锚点之前的早期对话（上一轮任务、更早的问答）——最不该占着预算的就是它们；
 *   ② 当前任务里较早的**工具轮块**（读过的脚本页/日志都能重读，性价比最高）；
 *   ③ 最后才动当前任务的对话叙述。
 * **锚点块（最近一条真实用户消息）与最后一块永不丢**——前者是当前任务的题干，
 * 后者是模型正要消费的最新结果。
 */
function compactBlocks(blocks, anchor, allowanceTokens, fixedTokens) {
    const dropped = new Set();
    let droppedCount = 0;
    let total = fixedTokens + blocksTokens(blocks);

    // 历史遗留的压缩通知先一并清掉（重建成一条最新的汇总）
    for (let i = 0; i < blocks.length; i++) {
        if (isNoticeBlock(blocks[i])) { dropped.add(i); total -= blockTokens(blocks[i]); }
    }

    const dropAt = (i) => {
        total -= blockTokens(blocks[i]);
        dropped.add(i);
        if (!isNoticeBlock(blocks[i])) droppedCount++;
    };
    // ① 锚点之前的早期对话：由旧到新
    for (let i = 0; i < anchor && total > allowanceTokens; i++) {
        if (!dropped.has(i)) dropAt(i);
    }
    // ② 锚点之后（不含最后一块）的旧工具轮块
    for (let i = anchor + 1; i < blocks.length - 1 && total > allowanceTokens; i++) {
        if (!dropped.has(i) && isToolBlock(blocks[i])) dropAt(i);
    }
    // ③ 仍不够才丢锚点之后的对话叙述
    for (let i = anchor + 1; i < blocks.length - 1 && total > allowanceTokens; i++) {
        if (!dropped.has(i)) dropAt(i);
    }

    if (droppedCount === 0) return null;

    // 累计历史上被省掉的段数：读旧通知里的计数
    let prevDropped = 0;
    for (const b of blocks) {
        if (isNoticeBlock(b)) prevDropped = Math.max(prevDropped, b[0]._dropped || 0);
    }

    const out = [];
    let noticePlaced = false;
    for (let i = 0; i < blocks.length; i++) {
        if (dropped.has(i)) {
            if (!noticePlaced) { out.push([noticeMessage(prevDropped + droppedCount)]); noticePlaced = true; }
            continue;
        }
        out.push(blocks[i]);
    }
    return { blocks: out, dropped: droppedCount, totalDropped: prevDropped + droppedCount };
}

/** 兜底：单个块自身就超预算时的内容级收缩（工具层已限制结果长度，正常不可达）。 */
function shrinkBlocks(blocks, allowanceTokens, anchor) {
    const out = blocks.map((b) => b.map((m) => ({ ...m })));
    // 工具结果用完整 JSON 占位，不能直接砍序列化文本；目标和调用参数始终保留。
    for (let i = 0; i < out.length && blocksTokens(out) > allowanceTokens; i++) {
        if (i === anchor) continue;
        for (const m of out[i]) {
            if (m.role !== 'tool' || m.content.length < 1024) continue;
            m.content = JSON.stringify({ contextOmitted: true,
                hint: '工具结果因上下文限制已省略，状态未知；请按需重新读取目标区间或设备日志，不要重复编辑。' });
        }
    }
    return out;
}

/**
 * 组装一次请求的消息序列，并给出这次组装的实际决策。
 *
 * 返回值：
 *   - messages           发给 API 的 wire 数组（已剥 _meta、已按预算压缩）
 *   - compactedMessages  仅当本轮触发了压缩：应**写回会话**的新消息数组（不可逆），否则 null
 *   - meta               估算 token / 预算水位 / 是否压缩 / 丢了多少块（供 UI 显示）
 *
 * 调用方拿到 compactedMessages 后必须写回（agent 循环里就是这样），否则滞回失效：
 * 每轮都会从同一份旧历史重新压一次，前缀每轮都变，服务端前缀缓存一点都吃不到。
 * @param {Array} messages 内存会话（可带 _meta）
 * @param {string} systemPrompt
 * @param {Object} [opts] { budgetTokens, highWaterRatio, lowWaterRatio }
 * @returns {{messages:Array, compactedMessages:?Array, meta:Object}}
 */
export function planApiMessages(messages, systemPrompt, opts) {
    const o = opts || {};
    const budgetTokens = contextCapacity(o.budgetTokens);
    const toolsTokens = Number.isFinite(o.toolsTokens) && o.toolsTokens > 0 ? Math.floor(o.toolsTokens) : 0;
    const highRatio = Number.isFinite(o.highWaterRatio) ? o.highWaterRatio : HIGH_WATER_RATIO;
    const lowRatio = Number.isFinite(o.lowWaterRatio) ? o.lowWaterRatio : LOW_WATER_RATIO;

    const system = { role: 'system', content: systemPrompt || '' };
    const sysTokens = estimateMessageTokens(system);
    const fixed = sysTokens + toolsTokens;
    const outputReserveTokens = Number.isFinite(o.outputReserveTokens) && o.outputReserveTokens >= 0
        ? Math.floor(o.outputReserveTokens)
        : Math.min(DEFAULT_OUTPUT_RESERVE_TOKENS, Math.floor(budgetTokens * 0.1));
    const requestBudgetTokens = Number.isFinite(o.requestBudgetTokens) && o.requestBudgetTokens > 0
        ? Math.floor(o.requestBudgetTokens) : budgetTokens;
    const inputLimit = Math.max(0, Math.min(budgetTokens - outputReserveTokens, requestBudgetTokens));
    const historyBudget = Math.max(0, inputLimit - fixed);
    const highWater = Math.max(0, Math.floor(budgetTokens * highRatio) - outputReserveTokens);
    const compactionWater = Math.min(highWater, Math.floor(requestBudgetTokens * highRatio));
    // 低水位至少容纳固定开销，但不能虚构比输入硬上限更大的额度。
    const lowWater = Math.min(inputLimit, Math.max(fixed,
        Math.min(Math.floor(budgetTokens * lowRatio) - outputReserveTokens,
            Math.floor(requestBudgetTokens * lowRatio))));

    let blocks = toBlocks(messages || []);
    const anchor = findAnchor(blocks);

    const toolsChars = o.toolsChars || toolsTokens * 4;
    const fixedChars = JSON.stringify(system).length + toolsChars + 1024;
    const charsOf = (bs) => JSON.stringify([system, ...bs.flat().map(sanitizeMessage)]).length + toolsChars + 1024;
    let total = fixed + blocksTokens(blocks);
    const beforeTokens = total;
    let compacted = false;
    let droppedBlocks = 0;
    let compactedMessages = null;

    if (!o.preview && (total >= compactionWater || charsOf(blocks) > MAX_REQUEST_CHARS) && blocks.length > 1) {
        // 转义会使 JSON 比原文长，体积越线时也压到有余量的目标。
        const compactTarget = charsOf(blocks) > MAX_REQUEST_CHARS
            ? Math.min(lowWater, fixed + Math.max(0, Math.floor((MAX_REQUEST_CHARS - fixedChars) / 8))) : lowWater;
        const r = compactBlocks(blocks, anchor, compactTarget, fixed);
        if (r) {
            compacted = true;
            droppedBlocks = r.dropped;
            blocks = r.blocks;
            compactedMessages = blocks.flat();     // 写回会话用（保留 _meta，UI 照常渲染）
            total = fixed + blocksTokens(blocks);
        }
    }

    // 兜底：单个块自身就超预算（工具层已限制结果长度，正常不可达）
    if (!o.preview && (fixed + blocksTokens(blocks) > inputLimit || charsOf(blocks) > MAX_REQUEST_CHARS)) {
        const shrunk = shrinkBlocks(blocks, Math.max(0, inputLimit - fixed), findAnchor(blocks));
        if (JSON.stringify(shrunk) !== JSON.stringify(blocks)) {
            blocks = shrunk;
            compactedMessages = blocks.flat();
            compacted = true;
        }
    }

    // 发送视图 = **全部**块；是否变短只由预算（压缩）决定。
    // 早先的版本在这里做了 `blocks.slice(anchor)`，等于"只发最近一条用户消息之后的对话"——
    // 多轮对话从第二条用户消息起就只剩当前这一轮，模型表现为"聊两句就失忆"。
    // 锚点只用于压缩时划定"不许丢到哪"的下限，不是发送视图的起点。
    const wireBlocks = blocks;
    const wire = [system];
    for (const b of wireBlocks) for (const m of b) wire.push(sanitizeMessage(m));

    const requestChars = JSON.stringify(wire).length + toolsChars + 1024;
    return {
        messages: wire,
        compactedMessages,
        meta: {
            estimatedTokens: fixed + blocksTokens(wireBlocks),
            beforeTokens,
            outputReserveTokens,
            estimatedWindowTokens: fixed + blocksTokens(wireBlocks) + outputReserveTokens,
            requestBudgetTokens,
            inputLimit,
            compactionWater,
            requestChars,
            overBudget: fixed + blocksTokens(wireBlocks) > inputLimit || requestChars > MAX_REQUEST_CHARS,
            systemTokens: sysTokens,
            toolsTokens,
            fixedTokens: fixed,
            historyBudget,
            historyTokens: blocksTokens(wireBlocks),
            budgetTokens,
            highWater,
            lowWater,
            compacted,
            droppedBlocks,
            keptBlocks: wireBlocks.length,
        },
    };
}

/**
 * 兼容旧签名的薄封装：只返回消息数组（单测与既有调用点用）。
 * 注意第三个参数语义已变（旧版是 maxHistoryRounds，现在是 { budgetTokens, ... }）。
 */
export function buildApiMessages(messages, systemPrompt, opts) {
    return planApiMessages(messages, systemPrompt, opts).messages;
}
