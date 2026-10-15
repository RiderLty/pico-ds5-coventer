// ---------------------------------------------------------------------------
// AI 上下文层单测（零依赖，node 直接跑）：node frontend/scripts/test-ai-context.mjs
//
// 这里只测纯逻辑（utils/aiContext.js）——它不 import 任何浏览器/Vite 专有东西，
// 所以能在 node 里直接跑。build.sh 第 3c 步会调用它。
//
// 重点保障四件事（都是旧版真金白银踩过的坑）：
//   ① _meta 绝不出现在发出去的 wire 消息里（旧版把工具结果发了两遍）；
//   ② 未触发压缩时消息序列**纯追加**——前缀逐字节不变，服务端前缀缓存才能命中；
//   ③ 压缩是"越线才发生、一次压到低水位"的滞回事件，压完接着若干轮前缀稳定；
//   ④ 原子块整体丢弃，tool 配对被拆散（服务端会直接 400）。
// ---------------------------------------------------------------------------

import { liveReasoningWindow, REASONING_LIVE_TAIL_CHARS as liveWindowMax } from '../src/utils/aiText.js';
import {
    estimateTokens, estimateMessageTokens, estimateToolsTokens,
    sanitizeMessage, toBlocks, pruneText, pruneToolResult,
    toolCallSignature, normalizeUsage, addUsage,
    planApiMessages, buildApiMessages,
    DEFAULT_BUDGET_TOKENS, PRUNE_DEFAULTS, formatTokens,
} from '../src/utils/aiContext.js';

let passed = 0;
const failures = [];

function ok(cond, name, detail) {
    if (cond) { passed++; return; }
    failures.push(detail ? `${name} —— ${detail}` : name);
}
function eq(actual, expected, name) {
    ok(actual === expected, name, `期望 ${JSON.stringify(expected)}，实际 ${JSON.stringify(actual)}`);
}

// ── 测试夹具 ────────────────────────────────────────────────────────────────
const SYS = '你是 Lua 助手。' + '系统提示词内容。'.repeat(20);   // ≈ 300 字符
const TOOLS_TOKENS = 1700;   // 实测 tools schema 的估算量（9 个工具）

function userMsg(text) { return { role: 'user', content: text }; }
function assistantText(text) { return { role: 'assistant', content: text }; }
/** 一轮工具调用块：assistant(tool_calls) + 其 tool 结果（_meta 只给 UI 看） */
function toolRound(id, name, args, result, contentChars) {
    const resultObj = result !== undefined ? result : { ok: true, script: 'x'.repeat(contentChars || 0) };
    return [
        { role: 'assistant', content: '', tool_calls: [{ id, type: 'function', function: { name, arguments: args } }] },
        {
            role: 'tool',
            tool_call_id: id,
            content: JSON.stringify(resultObj),
            _meta: { name, result: resultObj },   // UI 用；绝不能发给 API
        },
    ];
}
/** 发出去的消息必须自洽：tool 结果一定有前置 assistant 的配对 id */
function pairingViolations(messages) {
    const seen = new Set();
    const bad = [];
    for (const m of messages) {
        if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
            for (const tc of m.tool_calls) seen.add(tc.id);
        } else if (m.role === 'tool') {
            if (!seen.has(m.tool_call_id)) bad.push(m.tool_call_id);
        }
    }
    return bad;
}
/** 相邻两次组装的前缀必须逐字节一致（纯追加），否则前缀缓存失效 */
function prefixStable(prev, next, name) {
    ok(next.length >= prev.length, `${name}：新序列不该比旧序列短`);
    const head = next.slice(0, prev.length);
    const same = JSON.stringify(head) === JSON.stringify(prev);
    ok(same, `${name}：前缀被改动（缓存会失效）`);
}

// ── 1. token 估算 ───────────────────────────────────────────────────────────
eq(estimateTokens(''), 0, '空串 0 token');
ok(estimateTokens('中文测试') > estimateTokens('abcdefgh'), '中文比等长 ASCII 贵（保守估算）');
ok(estimateTokens('a'.repeat(350)) >= 100, 'ASCII 350 字符 ≈ 100+ token');
ok(estimateMessageTokens({ role: 'tool', content: 'x'.repeat(3500) }) >= 1000, '单条消息估算含内容');
ok(estimateToolsTokens([{ type: 'function', function: { name: 'f', description: '中文描述'.repeat(50) } }]) > 0,
    'tools schema 估算 > 0');
eq(estimateToolsTokens([]), 0, '空 tools 估算 0');

// ── 2. sanitize：剥 _meta、补 type、参数转字符串 ────────────────────────────
{
    const wire = sanitizeMessage({
        role: 'tool', tool_call_id: 't1', content: '{"ok":true}',
        _meta: { name: 'read_current_script', result: { big: 'x'.repeat(100) } },
    });
    ok(!('_meta' in wire), 'sanitize 剥掉 _meta');
    eq(wire.tool_call_id, 't1', 'sanitize 保留 tool_call_id');

    const r = sanitizeMessage({ role: 'assistant', content: '答', _reasoning: '我在想…' });
    ok(!('_reasoning' in r), 'sanitize 剥掉 _reasoning（思维链只给 UI 看，绝不发给端点）');
    ok(r.content === '答', 'sanitize 保留正文');

    const a = sanitizeMessage({ role: 'assistant', content: '', tool_calls: [{ id: 'c1', function: { name: 'f', arguments: { a: 1 } } }] });
    eq(a.tool_calls[0].type, 'function', 'sanitize 补 tool_calls.type');
    eq(a.tool_calls[0].function.arguments, '{"a":1}', 'sanitize 把对象参数转成字符串');
}

// ── 3. 原子块 ───────────────────────────────────────────────────────────────
{
    const blocks = toBlocks([
        userMsg('a'),
        ...toolRound('t1', 'read_current_script', '{}', { ok: true, script: 'x' }),
        assistantText('好的'),
    ]);
    eq(blocks.length, 3, 'tool 轮合成一个原子块');
    eq(blocks[1].length, 2, '原子块含 assistant + tool');
}

// ── 4. 内容裁剪 ─────────────────────────────────────────────────────────────
{
    const short = 'y'.repeat(PRUNE_DEFAULTS.thresholdChars);
    eq(pruneText(short).removedChars, 0, '阈值内不裁剪');
    eq(pruneText(short).text.length, short.length, '阈值内原样返回');

    const long = 'H'.repeat(6000) + 'M'.repeat(6000) + 'T'.repeat(3000);
    const r = pruneText(long);
    ok(r.removedChars > 0, '超阈值裁剪掉中间');
    ok(r.text.startsWith('H'.repeat(100)), '裁剪保留头部');
    ok(r.text.endsWith('T'.repeat(100)), '裁剪保留尾部');
    ok(r.text.includes('已省略'), '裁剪插入标记');
    ok(r.text.length < long.length, '裁剪后更短');
}
{
    // 日志类：结构化保留前 10 + 后 20 行，且仍是合法 JSON
    const lines = Array.from({ length: 200 }, (_, i) => `[lua] 第 ${i} 行输出内容 padding padding padding`);
    const big = { ok: true, lines, total: 200 };
    const r = pruneToolResult('get_device_logs', big);
    ok(r.removedChars > 0, '长日志被裁剪');
    const parsed = JSON.parse(r.content);
    eq(parsed.lines.length, 31, '前 10 + 标记 1 + 后 20');
    eq(parsed.lines[0], lines[0], '保留第一行');
    eq(parsed.lines[parsed.lines.length - 1], lines[199], '保留最后一行');

    const small = { ok: true, lines: ['[lua] 一行'] };
    eq(pruneToolResult('get_device_logs', small).removedChars, 0, '短日志不动');
}

// ── 5. 组装：纯追加 + 锚点 + 无 _meta + 配对完整 ───────────────────────────
{
    const messages = [userMsg('给 A 键加连点')];
    const p1 = planApiMessages(messages, SYS, { budgetTokens: 24000 }).messages;
    eq(p1[0].role, 'system', '首条是 system');
    ok(!JSON.stringify(p1).includes('_meta'), '发出的消息里没有 _meta');

    messages.push(...toolRound('r1', 'read_current_script', '{"startLine":1}', undefined, 8000));
    const p2 = planApiMessages(messages, SYS, { budgetTokens: 24000 }).messages;
    prefixStable(p1, p2, '追加工具结果后');

    messages.push(...toolRound('r2', 'replace_in_file', '{"edits":[]}', { ok: true, applied: 1, foundAtLines: [12] }));
    const p3 = planApiMessages(messages, SYS, { budgetTokens: 24000 }).messages;
    prefixStable(p2, p3, '再追加一轮后');
    eq(pairingViolations(p3).length, 0, 'tool 配对完整');
    eq(p3[p3.length - 1].role, 'tool', '最后一条是刚拿到的工具结果');

    // 第二条用户消息成为新锚点：**新的要保留，旧的也不能丢**（预算是够的）
    messages.push(userMsg('再改成 200ms'));
    const p4 = planApiMessages(messages, SYS, { budgetTokens: 24000 }).messages;
    ok(JSON.stringify(p4).includes('再改成 200ms'), '最新用户消息（新锚点）一定保留');
    ok(JSON.stringify(p4).includes('给 A 键加连点'), '预算是够的：更早的任务描述也必须还在');
    eq(p4.length, p3.length + 1, '纯追加一条用户消息：消息数 +1（不是被裁到只剩最新一条）');
}

// ── 5b. 多轮对话：整段历史都要发出去（回归：曾把"锚点"误当发送视图起点）──
// 这条是**真机踩出来的 bug**：早先的实现 `blocks.slice(anchor)` 让第二条用户消息之后的请求
// 只剩「system + 最新一条用户消息」，模型聊两句就失忆。测试当时只断言了"最新消息还在"，
// 所以没拦住——断言要测"该保的东西还在"，不是"我实现的东西还在"。
{
    const turns = [
        userMsg('第一轮：帮我写个 A 键连点'),
        assistantText('写好了'),
        userMsg('第二轮：间隔改成 200ms'),
        assistantText('改好了'),
        userMsg('第三轮：再加个 B 键'),
    ];
    const p = planApiMessages(turns, SYS, { budgetTokens: 32000, toolsTokens: TOOLS_TOKENS });
    const text = JSON.stringify(p.messages);
    for (const key of ['第一轮', '写好了', '第二轮', '改好了', '第三轮']) {
        ok(text.includes(key), `多轮对话：${key} 仍在发送内容里`);
    }
    eq(p.messages.length, turns.length + 1, '多轮对话：system + 全部历史一条不少');
    ok(!p.meta.compacted, '预算充足时不触发压缩');

    // 真实长对话：给足预算 → 全程不压缩，消息数只增不减
    let long = [userMsg('第 1 轮需求')];
    for (let i = 2; i <= 6; i++) {
        long.push(assistantText(`第 ${i - 1} 轮回复`));
        long.push(...toolRound(`L${i}`, 'get_device_logs', `{"lines":30}`, { ok: true, lines: ['[lua] 一行'] }));
        long.push(userMsg(`第 ${i} 轮需求`));
    }
    const plan = planApiMessages(long, SYS, { budgetTokens: 32000, toolsTokens: TOOLS_TOKENS });
    ok(plan.messages.length === long.length + 1,
        '长对话（6 轮 + 每轮一次工具）在预算内也是全量发送',
        `实际 ${plan.messages.length} vs 期望 ${long.length + 1}`);
    ok(JSON.stringify(plan.messages).includes('第 1 轮需求'), '第一轮需求没有被吃掉');
}

// ── 6. 滞回压缩：越线才压缩、压到低水位、压完接着若干轮前缀稳定 ────────────
// 关键：压缩必须**写回会话**（不可逆）。不写回的话每轮都会从同一份旧历史重压一次，
// 于是每轮都是压缩轮、前缀每轮都变，缓存全废——这是第一版实现踩的第二个坑。
// 另一个坑见 5b：压缩要**最旧优先**，且锚点只是"不许丢到哪"的下限。
{
    // 6a. 现实量级：显式使用 32k 小窗口测试压缩机制；默认能力已提升到 1M。
    const budget = 32000;
    let messages = [userMsg('把连点间隔改成 100ms')];
    let last = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS }).messages;
    let compactions = 0;
    let appendOnlyRounds = 0;

    for (let i = 0; i < 14; i++) {
        messages.push(...toolRound(`t${i}`, 'read_current_script', `{"startLine":${i * 50 + 1}}`, undefined, 8000));
        const plan = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS });
        if (plan.meta.compacted) {
            compactions++;
            ok(Array.isArray(plan.compactedMessages), `第 ${i} 轮：压缩必须带回可写回会话的消息`);
            messages = plan.compactedMessages;      // ← 写回（不可逆）
        } else {
            appendOnlyRounds++;
            prefixStable(last, plan.messages, `第 ${i} 轮`);
        }
        eq(pairingViolations(plan.messages).length, 0, `第 ${i} 轮配对完整`);
        ok(plan.meta.estimatedTokens <= budget,
            `第 ${i} 轮估算不超预算`, `实际 ${plan.meta.estimatedTokens} > ${budget}`);
        ok(JSON.stringify(plan.messages).includes('把连点间隔改成 100ms'), `第 ${i} 轮保留任务目标`);
        last = plan.messages;
    }
    ok(compactions >= 1, '现实量级的长任务里也会触发压缩');
    ok(compactions <= 4, '压缩是偶发事件', `压缩了 ${compactions}/14 轮`);
    ok(appendOnlyRounds >= 10, '绝大多数轮次是纯追加（前缀缓存可命中）',
        `只有 ${appendOnlyRounds}/14 轮是纯追加`);
}

// 6b. 极小预算压力测试：机制不能卡死、配对不能破、预算不能爆
{
    const budget = 5000;
    let messages = [userMsg('目标：把连点改成 100ms')];
    let compacted = 0;
    for (let i = 0; i < 10; i++) {
        messages.push(...toolRound(`s${i}`, 'read_current_script', `{"startLine":${i + 1}}`, undefined, 8000));
        const plan = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS });
        if (plan.meta.compacted) { compacted++; messages = plan.compactedMessages; }
        eq(pairingViolations(plan.messages).length, 0, `压力第 ${i} 轮配对完整`);
        ok(plan.meta.estimatedTokens <= budget,
            `压力第 ${i} 轮不超预算`, `实际 ${plan.meta.estimatedTokens} > ${budget}`);
        eq(plan.messages[0].role, 'system', `压力第 ${i} 轮首条仍是 system`);
        ok(plan.messages.length >= 2, `压力第 ${i} 轮不会退化成空请求`);
    }
    ok(compacted > 0, '极小预算下压缩确实在干活');
}

// 6c. 压缩顺序：**最旧优先**，且当前任务（锚点及其后）绝不被动
{
    const budget = 16000;
    let messages = [];
    for (let i = 1; i <= 8; i++) {
        messages.push(userMsg(`第 ${i} 轮需求：把间隔调成 ${i * 100}ms`));
        messages.push(...toolRound(`t${i}`, 'read_current_script', `{"startLine":${i}}`, undefined, 8000));
        messages.push(assistantText(`第 ${i} 轮改好了`));
    }
    const plan = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS });
    const text = JSON.stringify(plan.messages);
    ok(plan.meta.compacted, '长多轮对话触发压缩');
    ok(plan.meta.estimatedTokens <= budget, '压缩后仍在预算内', String(plan.meta.estimatedTokens));
    ok(!text.includes('第 1 轮需求'), '最旧的对话先被丢掉');
    ok(text.includes('第 8 轮需求'), '最新的对话保留（不是被清空）');
    ok(text.includes('第 8 轮改好了') || text.includes('第 7 轮'), '最近的回复也保留');
    ok(text.includes('已省略'), '插入了压缩通知（告诉模型记录被省了）');
    eq(pairingViolations(plan.messages).length, 0, '压缩后 tool 配对完整');

    // 压缩后继续追加一轮：前缀必须稳定（缓存可命中），且新内容照常发出
    const before = plan.messages;
    let after = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS });
    messages = after.compactedMessages || messages;
    messages.push(userMsg('第 9 轮：再加个按键'));
    after = planApiMessages(messages, SYS, { budgetTokens: budget, toolsTokens: TOOLS_TOKENS });
    ok(JSON.stringify(after.messages).includes('第 9 轮'), '压缩后新的一轮照常发出');
    ok(JSON.stringify(after.messages).includes('第 8 轮'), '压缩后既有上下文没被再清一次');
    prefixStable(before, after.messages, '压缩后追加一轮');
}

// ── 7. 预算缺省与边界 ───────────────────────────────────────────────────────
{
    const p = planApiMessages([], SYS, {});
    eq(p.meta.budgetTokens, DEFAULT_BUDGET_TOKENS, '缺省预算生效');
    eq(p.messages.length, 1, '空会话只有 system');
    const arr = buildApiMessages([userMsg('hi')], SYS, { budgetTokens: 24000 });
    ok(Array.isArray(arr), 'buildApiMessages 薄封装仍返回数组');
}

// ── 8. 工具调用签名（无进展检测的判据）──────────────────────────────────────
{
    const a = { function: { name: 'get_device_logs', arguments: '{"lines":30}' } };
    const b = { function: { name: 'get_device_logs', arguments: '{ "lines" : 30 }' } };
    const c = { function: { name: 'get_device_logs', arguments: '{"lines":30,"scope":"all"}' } };
    const d = { function: { name: 'sleep', arguments: '{"lines":30}' } };
    ok(toolCallSignature(a) === toolCallSignature(b), '参数键序/空白差异视为同一次调用');
    ok(toolCallSignature(a) !== toolCallSignature(c), '参数不同则签名不同');
    ok(toolCallSignature(a) !== toolCallSignature(d), '工具不同则签名不同');
    ok(toolCallSignature(a) === toolCallSignature({ function: { name: 'get_device_logs', arguments: { lines: 30 } } }),
        '对象形态的参数也能规范化');
    ok(toolCallSignature({}) === '\u0000{}', '缺字段时不抛错');
}

// ── 9. usage 归一化 ─────────────────────────────────────────────────────────
{
    const ds = normalizeUsage({ prompt_tokens: 10000, completion_tokens: 500, total_tokens: 10500, prompt_cache_hit_tokens: 9000 });
    eq(ds.inputTokens, 1000, 'DeepSeek：命中部分从输入里扣掉');
    eq(ds.cacheReadTokens, 9000, 'DeepSeek：命中单独记账');
    eq(ds.outputTokens, 500, '输出 token');
    const oa = normalizeUsage({ prompt_tokens: 800, completion_tokens: 20, prompt_tokens_details: { cached_tokens: 512 } });
    eq(oa.inputTokens, 288, 'OpenAI：cached_tokens 也被扣除');
    ok(addUsage(ds, oa).inputTokens === 1288, 'usage 可累加');
    eq(normalizeUsage(null), null, '无 usage 返回 null');
    // 幂等：aiClient 归一化一次、aiAgent 记账时再过一道；不幂等会把数字清零，
    // 那会让"任务 token 闸"永远不触发（集成测试真的抓到过这个 bug）
    const twice = normalizeUsage(ds);
    eq(twice.inputTokens, ds.inputTokens, 'normalizeUsage 幂等（输入）');
    eq(twice.cacheReadTokens, ds.cacheReadTokens, 'normalizeUsage 幂等（缓存命中）');
    eq(twice.outputTokens, ds.outputTokens, 'normalizeUsage 幂等（输出）');
}

// ── 10. 实时思考块的尾部窗口（卡顿的直接防线）────────────────────────────────
// 实时渲染量必须有上界：整块 pre-wrap 文本每隔 ~120ms 重排一次，上万字时会明显拖帧。
{
    const short = '想'.repeat(200);
    const r1 = liveReasoningWindow(short, true);
    eq(r1.trimmed, false, '短思考不截尾');
    eq(r1.shown, short, '短思考原样渲染');
    eq(r1.total, 200, '总字数照实报（表头显示用）');

    const long = Array.from({ length: 9000 }, (_, i) => '想' + (i % 10)).join('');
    const r2 = liveReasoningWindow(long, true);
    eq(r2.trimmed, true, '超长思考在流式中被截尾');
    ok(r2.shown.length <= liveWindowMax + 2, '渲染长度被钳在尾部窗口内', `${r2.shown.length}`);
    ok(r2.shown.startsWith('…'), '截尾时以省略号开头（提示用户上面还有）');
    ok(r2.shown.endsWith(long.slice(-20)), '渲染的确实是**尾部**（最新思考）');
    eq(r2.total, long.length, '表头仍显示完整字数', `${r2.total}`);

    // 历史块不截尾：回看时要能翻到开头
    const r3 = liveReasoningWindow(long, false);
    eq(r3.trimmed, false, '历史块不截尾（与实时相反）');
    eq(r3.shown.length, long.length, '历史块渲染完整文本');

    // 边界：刚好等于窗口、空文本、非法 max
    eq(liveReasoningWindow('x'.repeat(liveWindowMax), true).trimmed, false, '刚好等于窗口不截');
    eq(liveReasoningWindow('x'.repeat(liveWindowMax + 1), true).trimmed, true, '多一个字就截');
    eq(liveReasoningWindow('', true).shown, '', '空文本安全');
    eq(liveReasoningWindow(null, true).shown, '', 'null 安全');
    ok(liveReasoningWindow(long, true, 0).shown.length > 0, '非法窗口值回退到默认值');
}

// 总窗口水位、输出预留和目标保护
{
    eq(DEFAULT_BUDGET_TOKENS, 1000000, '默认容量 1M');
    for (const [value, label] of [[128000, '128K'], [256000, '256K'], [1000000, '1M']]) eq(formatTokens(value), label, `容量显示 ${label}`);
    for (const budget of [128000, 256000, 1000000]) {
        const p = planApiMessages([], SYS, { budgetTokens: budget, toolsTokens: 1700, outputReserveTokens: 8192 });
        eq(p.meta.highWater, Math.floor(budget * 0.8) - 8192, `${budget} 水位按总窗口并预留输出`);
        eq(p.meta.fixedTokens, p.meta.systemTokens + 1700, '固定开销计入 system 和 tools');
    }
    const target = '不能丢失的目标' + '中'.repeat(15000);
    const p = planApiMessages([userMsg(target), ...toolRound('large', 'get_device_logs', '{}', { ok: true, script: 'x'.repeat(30000) })], SYS, { budgetTokens: 5000 });
    ok(p.messages.some((m) => m.content === target), '目标过大时完整保留，显式阻止发送');
    ok(p.meta.overBudget, '无法安全压缩时标记超限');
    for (const m of p.messages.filter((m) => m.role === 'tool')) {
        let valid = true;
        try { JSON.parse(m.content); } catch { valid = false; }
        ok(valid, '兜底裁剪保持工具 JSON 可解析');
    }
    const u = normalizeUsage({ input_tokens: 800, output_tokens: 20, input_tokens_details: { cached_tokens: 512 } });
    eq(u.inputTokens, 288, '兼容 input_tokens 格式');
    eq(u.outputTokens, 20, '兼容 output_tokens 格式');
    eq(normalizeUsage({ prompt_tokens: 10, prompt_cache_hit_tokens: 100 }).cacheReadTokens, 10, '异常缓存值钳到输入总量');
}

// 1M 配置仍有请求体硬保险；80% 边界与只读预览
{
    const followsWindow = planApiMessages([userMsg('目标')], SYS, { budgetTokens: 1000000, toolsTokens: 1700 });
    eq(followsWindow.meta.requestBudgetTokens, 1000000, '未指定单轮上限时跟随模型窗口');
    eq(followsWindow.meta.compactionWater, followsWindow.meta.highWater, '默认压缩线跟随模型窗口 80%');

    const capped = planApiMessages([userMsg('目标')], SYS, {
        budgetTokens: 1000000, requestBudgetTokens: 128000, toolsTokens: 1700,
    });
    eq(capped.meta.requestBudgetTokens, 128000, '手动单轮上限仍被保留');
    eq(capped.meta.compactionWater, Math.floor(128000 * 0.8), '手动单轮上限控制压缩线');

    const opts = { budgetTokens: 128000, requestBudgetTokens: 1000000, toolsTokens: 1700, outputReserveTokens: 0 };
    const base = planApiMessages([userMsg('目标')], SYS, opts).meta.estimatedTokens;
    const high = 128000 * 0.8;
    const below = [assistantText('x'.repeat(Math.floor((high - base - 20) * 3.5))), userMsg('目标')];
    const above = [assistantText('x'.repeat(Math.ceil((high - base + 20) * 3.5))), userMsg('目标')];
    ok(!planApiMessages(below, SYS, opts).meta.compacted, '低于 80% 不压缩');
    ok(planApiMessages(above, SYS, opts).meta.compacted, '达到 80% 压缩');
    const preview = planApiMessages(above, SYS, { ...opts, preview: true });
    ok(!preview.compactedMessages && preview.meta.estimatedTokens >= preview.meta.highWater, 'UI 预览保留真实超线占用，不预先压缩');
    const large = planApiMessages([assistantText('\\'.repeat(1200000)), userMsg('目标')], SYS,
        { budgetTokens: 1000000, requestBudgetTokens: 1000000 });
    ok(large.meta.compacted && large.meta.requestChars <= 2 * 1024 * 1024, '转义膨胀的 JSON 超过 2Mi 字符也压缩');
    eq(normalizeUsage({}), null, '空 usage 视为未知，用估算记账');
}

// ── 结果 ────────────────────────────────────────────────────────────────────
if (failures.length) {
    console.error(`[ai-context] 失败 ${failures.length} 项（通过 ${passed} 项）：`);
    for (const f of failures) console.error('   ✗ ' + f);
    process.exit(1);
}
console.log(`[ai-context] OK：${passed} 项断言全部通过（前缀稳定性 / 滞回压缩 / 配对完整性 / 裁剪 / 无进展检测）`);
