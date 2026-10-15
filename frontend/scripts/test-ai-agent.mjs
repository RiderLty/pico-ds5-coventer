// ---------------------------------------------------------------------------
// Agent 循环集成测试（零依赖，node 直接跑）：
//   node frontend/scripts/test-ai-agent.mjs
//
// 用假端点（stub fetch）跑真实的 runAgentTurn，逐条验证"省钱改造"在**发出去的请求体**上
// 真的生效——这些是单测覆盖不到的地方（aiContext 单测只管组装逻辑，这里管整条链路）：
//   ① 请求体里绝不出现 _meta / _compactNotice（旧版每个工具结果发两遍）；
//   ② 每轮都带 stream_options.include_usage 与 max_tokens（usage 可见 + 输出有上限）；
//   ③ 一轮里模型给多个工具调用 → 只发一次请求（旧版提示词强制"一轮一个编辑"）；
//   ④ 连续 3 次同参数调用 → 直接停（旧版会一路撞到 50 轮）；
//   ⑤ 任务 token 闸：累计输入超预算 → 停；
//   ⑥ 压缩事件：上下文超预算时压缩并写回会话，之后请求体变小。
// ---------------------------------------------------------------------------

import { pathToFileURL } from 'node:url';

// ── node 版本门（module.register 需要 node ≥ 20.6）──
// 用动态 import 探测：老 node 上 `import { register } from 'node:module'` 是**链接期**
// 报错，静态 import 会让整个脚本起不来，所以这里必须先探测再决定跑不跑。
let register;
try { ({ register } = await import('node:module')); } catch (e) { register = undefined; }
if (typeof register !== 'function') {
    console.log('[ai-agent] 跳过：当前 node 不支持 module.register（需 ≥ 20.6）');
    process.exit(0);
}

// ── 浏览器环境打桩（aiAgent 在模块加载时就会读 localStorage）──
const store = new Map();
globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: (k) => store.delete(k),
};

register('./ai-raw-loader.mjs', pathToFileURL(import.meta.dirname + '/'));

const { runAgentTurn, getSessionMessages, clearHistory, getLastPlan } =
    await import('../src/utils/aiAgent.js');
const { estimateTokens } = await import('../src/utils/aiContext.js');

// ── 断言 ────────────────────────────────────────────────────────────────────
let passed = 0;
const failures = [];
function ok(cond, name, detail) {
    if (cond) { passed++; return; }
    failures.push(detail ? `${name} —— ${detail}` : name);
}
function eq(a, b, name) { ok(a === b, name, `期望 ${JSON.stringify(b)}，实际 ${JSON.stringify(a)}`); }

// ── 假端点 ──────────────────────────────────────────────────────────────────
/** 把若干帧拼成 SSE 响应体 */
function sse(frames) {
    return frames.map((f) => `data: ${JSON.stringify(f)}\n\n`).join('') + 'data: [DONE]\n\n';
}
/** 一次"模型要调用工具"的响应（可一次给多个工具调用，模拟批量编辑）*/
function toolCallResponse(calls, usage) {
    const frames = [{ choices: [{ delta: { role: 'assistant', content: '' }, index: 0 }] }];
    calls.forEach((c, i) => {
        frames.push({ choices: [{ delta: { tool_calls: [{ index: i, id: c.id, type: 'function', function: { name: c.name, arguments: JSON.stringify(c.args || {}) } }] }, index: 0 }] });
    });
    frames.push({ choices: [{ delta: {}, finish_reason: 'tool_calls', index: 0 }] });
    if (usage) frames.push({ choices: [], usage });
    return sse(frames);
}
/** 一次"模型给出最终文本"的响应 */
function textResponse(text, usage) {
    const frames = [
        { choices: [{ delta: { role: 'assistant', content: text }, index: 0 }] },
        { choices: [{ delta: {}, finish_reason: 'stop', index: 0 }] },
    ];
    if (usage) frames.push({ choices: [], usage });
    return sse(frames);
}
/** 推理模型：先流思维链（reasoning_content）再流正文，finish_reason 可选 'length'（截断） */
function thinkingResponse(reasoning, content, finishReason, usage) {
    const frames = [];
    if (reasoning) frames.push({ choices: [{ delta: { role: 'assistant', reasoning_content: reasoning }, index: 0 }] });
    if (content) frames.push({ choices: [{ delta: { content }, index: 0 }] });
    frames.push({ choices: [{ delta: {}, finish_reason: finishReason || 'stop', index: 0 }] });
    if (usage) frames.push({ choices: [], usage });
    return sse(frames);
}

const requests = [];      // 捕获每个请求体
let scriptQueue = [];     // 模型脚本：每轮弹一个响应体
let usageQueue = [];      // 与脚本同序的 usage
let fetchCount = 0;

globalThis.fetch = async (url, init) => {
    fetchCount++;
    let body = null;
    try { body = JSON.parse(init.body); } catch (e) { /* 非 JSON 请求 */ }
    requests.push({ url: String(url), body });
    const payload = scriptQueue.shift();
    if (payload === undefined) throw new Error('假端点：脚本队列空了（说明循环比预期多跑了一轮）');
    return new Response(payload, { status: 200, headers: { 'Content-Type': 'text/event-stream' } });
};

/** 组装一次发送视图（用于断言"思维链不占上下文预算"这类性质） */
async function planOf(messages) {
    const { planApiMessages, estimateToolsTokens } = await import('../src/utils/aiContext.js');
    const { buildSystemPrompt } = await import('../src/utils/aiPrompt.js');
    return planApiMessages(messages, buildSystemPrompt('桩文档'), { toolsTokens: estimateToolsTokens([]) }).messages;
}

// ── 测试框架 ────────────────────────────────────────────────────────────────
const CFG = {
    baseUrl: 'https://fake.example', model: 'fake', apiKey: 'k', temperature: 0,
    contextBudgetTokens: 32000, maxRounds: 16, taskBudgetTokens: 150000, maxTokens: 4096,
};
const BIG_SCRIPT = Array.from({ length: 400 }, (_, i) => `-- 第 ${i} 行\nlocal x${i} = ${i}`).join('\n');

function makeCtx() {
    let script = BIG_SCRIPT;
    return {
        getScript: () => script,
        applyScript: (s) => { script = s; },
        sendCustomEvent: () => true,
        current: () => script,
    };
}

function resetQueue(scriptSeq) {
    requests.length = 0;
    fetchCount = 0;
    scriptQueue = scriptSeq;
}
function reset(scriptSeq) {
    clearHistory();
    requests.length = 0;
    fetchCount = 0;
    scriptQueue = scriptSeq;
}

function lastRequest() { return requests[requests.length - 1]; }
function bodyText(i) { return JSON.stringify(requests[i === undefined ? requests.length - 1 : i].body); }

/** 每个请求体都必须满足的公共约束 */
function assertRequestHygiene(label, maxTokens = 4096) {
    requests.forEach((r, i) => {
        const text = JSON.stringify(r.body);
        ok(!text.includes('_meta'), `${label} #${i + 1}：请求体不含 _meta`);
        ok(!text.includes('_compactNotice'), `${label} #${i + 1}：请求体不含 _compactNotice`);
        ok(!text.includes('_dropped'), `${label} #${i + 1}：请求体不含内部记账字段`);
        if (r.body.stream) {
            ok(r.body.stream_options && r.body.stream_options.include_usage === true,
                `${label} #${i + 1}：带 stream_options.include_usage`);
        }
        eq(r.body.max_tokens, maxTokens || undefined, `${label} #${i + 1}：输出配置一致`);
        ok(Array.isArray(r.body.tools) && r.body.tools.length > 0, `${label} #${i + 1}：带 tools`);
    });
}

// ── 1. 一轮多工具调用 = 一次请求；_meta 不外泄；usage 上抛 ──────────────────
{
    const ctx = makeCtx();
    reset([
        toolCallResponse([
            { id: 'c1', name: 'read_current_script', args: { startLine: 1, lines: 40 } },
            { id: 'c2', name: 'read_current_script', args: { startLine: 100, lines: 40 } },
        ], { prompt_tokens: 9000, completion_tokens: 120, total_tokens: 9120, prompt_cache_hit_tokens: 8000 }),
        textResponse('两段都看过了', { prompt_tokens: 9500, completion_tokens: 30, total_tokens: 9530, prompt_cache_hit_tokens: 9000 }),
    ]);
    const events = [];
    const res = await runAgentTurn('看看脚本', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });

    ok(res.ok, '一轮多工具调用：任务成功结束');
    eq(fetchCount, 2, '两个只读工具调用只花了 2 次请求（同一轮的多个调用合并）');
    assertRequestHygiene('多工具');

    const usageEvents = events.filter((e) => e.type === 'usage');
    eq(usageEvents.length, 2, '每轮都上抛 usage 事件');
    eq(usageEvents[0].usage.inputTokens, 1000, 'usage 把缓存命中从输入里扣掉（9000-8000）');
    eq(usageEvents[0].usage.cacheReadTokens, 8000, 'usage 单列缓存命中');
    eq(usageEvents[1].totalUsage.inputTokens, 1500, '累计输入跨轮相加');
    ok(events.some((e) => e.type === 'done'), '结束时发 done');

    // 会话里保留 _meta（UI 渲染要用），但发出去的没有
    ok(getSessionMessages().some((m) => m._meta), '会话内保留 _meta 供 UI 渲染');
}

// ── 2. 批量编辑：一次请求落多个编辑块 ───────────────────────────────────────
{
    const ctx = makeCtx();
    const before = ctx.getScript();
    reset([
        toolCallResponse([{ id: 'e0', name: 'read_current_script', args: { startLine: 1, lines: 10 } }]),
        toolCallResponse([{ id: 'e1', name: 'replace_in_file', args: {
            edits: [
                { search: 'local x0 = 0', replace: 'local x0 = 42' },
                { search: 'local x1 = 1', replace: 'local x1 = 43' },
                { search: 'local x2 = 2', replace: 'local x2 = 44' },
            ],
            explanation: '改三处',
        } }]),
        textResponse('已改三处并加载到内存'),
    ]);
    const events = [];
    const res = await runAgentTurn('把前三行常量改掉', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });
    ok(res.ok, '批量编辑任务成功');
    eq(fetchCount, 3, '读取上下文后一次编辑请求落三处，再一轮回答');
    ok(ctx.getScript().includes('local x0 = 42') && ctx.getScript().includes('local x2 = 44'), '三处改动都应用了');
    ok(ctx.getScript() !== before, '编辑生效');
    assertRequestHygiene('批量编辑');
}

// ── 3. 无进展：连续 3 次同参数 → 直接停 ─────────────────────────────────────
{
    const ctx = makeCtx();
    const same = [{ id: 'r', name: 'read_current_script', args: { startLine: 1, lines: 20 } }];
    // 4 个相同调用：第 3 次之后就该停，所以第 4 个响应不会被消费
    reset([
        toolCallResponse(same), toolCallResponse(same), toolCallResponse(same), toolCallResponse(same),
        textResponse('不该走到这里'),
    ]);
    const events = [];
    const res = await runAgentTurn('反复读同一段', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });
    ok(!res.ok, '无进展时任务被中止');
    ok(/没有进展|已停止/.test(res.error.message), '中止原因点明"没有进展"', res.error && res.error.message);
    eq(fetchCount, 3, '第 3 次之后就停，不再请求（旧版会一路撞到 50 轮）');
    ok(events.some((e) => e.type === 'error'), '发出 error 事件');
    assertRequestHygiene('无进展');
}

// ── 4. 任务 token 闸 ───────────────────────────────────────────────────────
{
    const ctx = makeCtx();
    const cfg = { ...CFG, taskBudgetTokens: 20000 };
    const big = { prompt_tokens: 12000, completion_tokens: 50, total_tokens: 12050 };  // 每次都 12k 输入
    reset([
        toolCallResponse([{ id: 'a', name: 'read_current_script', args: { startLine: 1, lines: 10 } }], big),
        toolCallResponse([{ id: 'b', name: 'read_current_script', args: { startLine: 20, lines: 10 } }], big),
        toolCallResponse([{ id: 'c', name: 'read_current_script', args: { startLine: 40, lines: 10 } }], big),
        textResponse('不该走到这里'),
    ]);
    const res = await runAgentTurn('烧钱测试', ctx, { cfg, onEvent: () => {} });
    ok(!res.ok, '超预算时任务被中止');
    ok(/预算/.test(res.error.message), '中止原因点明预算', res.error && res.error.message);
    eq(fetchCount, 2, '累计 24k > 20k 时停（第 2 轮之后）');
    assertRequestHygiene('token 闸');
}

// ── 5. 上下文压缩：会话被真正缩短，之后的请求体变小 ─────────────────────────
{
    const ctx = makeCtx();
    // 预算要小到几轮就顶线（测试里的 system prompt 是桩文档，固定开销比真实的小得多）
    const cfg = { ...CFG, contextBudgetTokens: 6000, maxTokens: 0 };
    // 每轮读一个 200 行窗口，读 6 轮必然越线
    const seq = [];
    for (let i = 0; i < 6; i++) {
        seq.push(toolCallResponse([{ id: `p${i}`, name: 'read_current_script', args: { startLine: i * 50 + 1, lines: 200 } }]));
    }
    seq.push(textResponse('读完了'));
    reset(seq);
    const events = [];
    const res = await runAgentTurn('把整份脚本读完', ctx, { cfg, onEvent: (e) => events.push(e) });
    ok(res.ok, '压缩场景任务成功结束');
    const compactEvents = events.filter((e) => e.type === 'compacted');
    ok(compactEvents.length >= 1, '触发了压缩事件', `实际 ${compactEvents.length} 次`);
    ok(compactEvents[0]?.droppedBlocks > 0, '压缩确实丢了块');

    // 压缩后请求体确实变小（不是越压越大）
    const sizes = requests.map((r) => JSON.stringify(r.body.messages).length);
    const peak = Math.max(...sizes);
    const tail = sizes[sizes.length - 1];
    ok(tail <= peak, '最后一个请求不比峰值大', `peak=${peak} tail=${tail}`);
    const plan = getLastPlan();
    ok(plan && plan.estimatedTokens <= cfg.contextBudgetTokens,
        '最终上下文在预算内', plan && `${plan.estimatedTokens} > ${cfg.contextBudgetTokens}`);
    ok(getSessionMessages().some((m) => m._compactNotice), '会话里留下一条压缩通知（UI 解释用）');
    assertRequestHygiene('压缩', 0);

    // 通知的正文会发给模型（这是有意的），但内部标记不会
    ok(bodyText().includes('已省略'), '通知正文发给模型，让模型知道记录被省了');
}

// ── 6. 空活动（模型直接回答，不调工具）──
{
    const ctx = makeCtx();
    reset([textResponse('你好，请告诉我你想改什么')]);
    const res = await runAgentTurn('你好', ctx, { cfg: CFG, onEvent: () => {} });
    ok(res.ok, '纯文本回答也算成功');
    eq(fetchCount, 1, '不调工具就只发一次请求');
    assertRequestHygiene('纯文本');
    // 会话 = user + assistant
    eq(getSessionMessages().length, 2, '历史里只有一问一答');
}

// ── 7. 多轮对话：第二轮的请求里必须带上第一轮 ───────────────────────────────
// 回归守卫：早先的实现把"当前任务锚点"当成了发送视图的起点（blocks.slice(anchor)），
// 于是从第二条用户消息起，请求里只剩「system + 最新一条用户消息」——真机上表现为
// "聊两句就失忆"。所有单轮测试都发现不了，必须专门测一轮"接着聊"。
{
    const ctx = makeCtx();
    reset([textResponse('第一轮回复：已经帮你写好了')]);
    const r1 = await runAgentTurn('第一轮：给 A 键加连点', ctx, { cfg: CFG, onEvent: () => {} });
    ok(r1.ok, '第一轮成功');

    resetQueue([textResponse('第二轮回复：间隔已改成 200ms')]);
    const r2 = await runAgentTurn('第二轮：间隔改成 200ms', ctx, { cfg: CFG, onEvent: () => {} });
    ok(r2.ok, '第二轮成功');
    eq(fetchCount, 1, '第二轮只发一次请求');

    const body = requests[0].body;
    const roles = body.messages.map((m) => m.role).join(',');
    eq(roles, 'system,user,assistant,user', '第二轮请求的消息序列 = system + 第一轮问答 + 新问题');
    const text = JSON.stringify(body.messages);
    ok(text.includes('第一轮：给 A 键加连点'), '第二轮请求里带着第一轮的需求');
    ok(text.includes('第一轮回复'), '第二轮请求里带着第一轮的回复（上文的助手输出没丢）');
    ok(text.includes('第二轮：间隔改成 200ms'), '第二轮的新需求也在');
    ok(JSON.stringify(body).length > 0 && !JSON.stringify(body).includes('_meta'), '多轮请求体仍不含 _meta');

    // 第三轮继续接着聊，前两轮都要在
    resetQueue([textResponse('第三轮回复')]);
    await runAgentTurn('第三轮：再加个 B 键', ctx, { cfg: CFG, onEvent: () => {} });
    const t3 = JSON.stringify(requests[0].body.messages);
    ok(t3.includes('第一轮：给 A 键加连点') && t3.includes('第二轮：间隔改成 200ms') && t3.includes('第三轮'),
        '第三轮请求里三轮内容都在（上下文没有被清空）');
    assertRequestHygiene('多轮对话');
}

// ── 8. 推理模型：思维链只做展示、绝不进请求；截断与空回答必须显式报错 ────────
// 真机 bug 现场：deepseek-flash 是推理模型，思维链与正文共用输出额度。max_tokens 设小 →
// 思考吃光额度 → content 为空但 finish_reason=length → 旧逻辑当成"正常答完" →
// 界面上"思考了一会儿然后什么都没有"。
{
    // 8a. 思维链：发出 reasoning 事件，但**不写进请求**（DeepSeek 要求不回传，且省 token）
    const ctx = makeCtx();
    reset([thinkingResponse('我先读一下脚本，再决定改哪里。', '这是最终回答', 'stop')]);
    const events = [];
    const res = await runAgentTurn('看看脚本', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });
    ok(res.ok, '带思维链的正常回答成功');
    const thinkEvents = events.filter((e) => e.type === 'reasoning');
    ok(thinkEvents.length > 0, '思维链以 reasoning 事件上抛（UI 可显示"思考中"进度）',
        `${thinkEvents.length} 个事件`);
    ok(thinkEvents.map((e) => e.content).join('').includes('先读一下脚本'), '思维链内容完整上抛');
    const bodyText8a = JSON.stringify(requests[requests.length - 1].body);
    ok(!bodyText8a.includes('先读一下脚本'), '思维链**没有**被写进请求体');
    ok(!bodyText8a.includes('_reasoning'), '请求体里连 _reasoning 字段都没有（sanitizeMessage 剥掉）');
    // 但会话里要留着（UI 展开回看用），且**不占上下文预算**
    const withReasoning = getSessionMessages().filter((m) => m.role === 'assistant' && m._reasoning);
    ok(withReasoning.length > 0, '思维链挂在 assistant._reasoning 上（历史里保留，供 UI 展开）');
    ok(!JSON.stringify(await planOf(getSessionMessages())).includes('先读一下脚本'),
        '思维链不占上下文预算（组装发送视图时不带它）');
    ok(JSON.stringify(getSessionMessages()).includes('这是最终回答'), '正文正常进历史');
}

{
    // 8a2. 超长思维链按上限截断（避免把 512KB 的 localStorage 存档挤爆）
    const ctx = makeCtx();
    const huge = '想'.repeat(6000);
    reset([thinkingResponse(huge, '答案', 'stop')]);
    await runAgentTurn('想久一点', ctx, { cfg: CFG, onEvent: () => {} });
    const msg = getSessionMessages().find((m) => m.role === 'assistant' && m._reasoning);
    ok(!!msg, '超长思维链也有 _reasoning');
    ok(msg._reasoning.length <= 2100, '思维链被截断到上限内', `${msg._reasoning.length} 字`);
    ok(msg._reasoning.includes('中间') && msg._reasoning.includes('省略'), '截断处留下标记');
    ok(msg._reasoning.startsWith('想') && msg._reasoning.endsWith('想'), '保留头尾（头尾最能说明当时在想什么）');
}

{
    // 8b. 被截断且正文为空 → 必须报错（不能静默变成空回答）
    const ctx = makeCtx();
    reset([thinkingResponse('用户要一个连点脚本，A 键 HID 码是 0x04，连点要注册 tick 回调，', '', 'length')]);
    const events = [];
    const res = await runAgentTurn('写一个连点脚本', ctx, { cfg: { ...CFG, maxTokens: 256 }, onEvent: (e) => events.push(e) });
    ok(!res.ok, '思考吃光额度、正文为空时任务判定为失败');
    ok(res.error && /截断/.test(res.error.message), '错误说明是"被截断"', res.error && res.error.message.slice(0, 60));
    ok(res.error && /回复长度上限/.test(res.error.message), '错误里给出可操作的修法');
    ok(events.some((e) => e.type === 'error'), '发出 error 事件（UI 会显示提示，不再静默）');
    eq(fetchCount, 1, '不会傻乎乎重试到轮数上限');
}

{
    // 8c. 被截断但正文有内容 → 保留内容 + 明确告知不完整
    const ctx = makeCtx();
    reset([thinkingResponse('想了一半……', 'function on_key(k, d)\n  -- 还没写完', 'length')]);
    const events = [];
    const res = await runAgentTurn('写脚本', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });
    ok(!res.ok, '截断（即使有部分内容）也判定为失败，避免用户以为写完了');
    ok(res.error && /内容不完整/.test(res.error.message), '错误里说明"内容不完整"');
    ok(events.some((e) => e.type === 'error'), '发出 error 事件（UI 显示提示）');
    ok(JSON.stringify(getSessionMessages()).includes('还没写完'), '已生成的部分内容保留在历史里（用户能看见）');
}

{
    // 8d. finish_reason=stop 但内容为空 → 同样必须报错
    const ctx = makeCtx();
    reset([thinkingResponse('思考了一下', '', 'stop')]);
    const events = [];
    const res = await runAgentTurn('你好', ctx, { cfg: CFG, onEvent: (e) => events.push(e) });
    ok(!res.ok, '空回答（finish_reason=stop）判定为失败');
    ok(res.error && /没有返回任何内容/.test(res.error.message), '错误说明是"模型没返回内容"');
    ok(events.some((e) => e.type === 'error'), '发出 error 事件');
    eq(getSessionMessages().filter((m) => m.role === 'assistant').length, 0, '不把空回答写进历史');
}

{
    // 8e. maxTokens=0（默认）时**不发送** max_tokens 字段
    const ctx = makeCtx();
    reset([thinkingResponse('想', '好的', 'stop')]);
    await runAgentTurn('你好', ctx, { cfg: { ...CFG, maxTokens: 0 }, onEvent: () => {} });
    ok(!('max_tokens' in requests[0].body), 'maxTokens=0 时请求体里没有 max_tokens（不限制输出）');
}

// 缺 usage 的预算与轮数耗尽总结
{
    reset([textResponse('不应发送')]);
    const res = await runAgentTurn('预算不足', makeCtx(), { cfg: { ...CFG, taskBudgetTokens: 10 } });
    ok(!res.ok && /预算/.test(res.error?.message), '请求前预算不足就停止');
    eq(fetchCount, 0, '缺 usage 也不能无预算发送');
}
{
    reset([toolCallResponse([{ id: 'end', name: 'read_current_script' }]), textResponse('总结')]);
    const events = [];
    const res = await runAgentTurn('目标必须保留', makeCtx(), { cfg: { ...CFG, maxRounds: 1 }, onEvent: (e) => events.push(e) });
    ok(res.ok, '耗尽后正常总结');
    ok(!requests[1].body.tools, '总结请求不再提供 tools');
    ok(events.some((e) => e.type === 'round_start' && e.round === 1), 'usage 缺失时仍能看轮数');
    ok(getSessionMessages().find((m) => m.content.includes('轮数已达上限'))?._notice, '收尾提醒不是任务锚点');
}
{
    reset([toolCallResponse([{ id: 'end', name: 'read_current_script' }]), textResponse('')]);
    const res = await runAgentTurn('总结为空', makeCtx(), { cfg: { ...CFG, maxRounds: 1 } });
    ok(!res.ok, '耗尽后的空总结不能判成功');
}

// 中止批次仍保留已经生效的编辑回执，并给未执行工具补齐配对
{
    const ctx = makeCtx(); const ctrl = new AbortController();
    reset([toolCallResponse([{ id: 'read-abort', name: 'read_current_script', args: { startLine: 1, lines: 10 } }]), toolCallResponse([
        { id: 'edit-abort', name: 'replace_in_file', args: { edits: [{ search: 'local x0 = 0', replace: 'local x0 = 99' }] } },
        { id: 'unused', name: 'read_current_script', args: { startLine: 10, lines: 10 } },
    ])]);
    const res = await runAgentTurn('中止批次', ctx, { cfg: CFG, signal: ctrl.signal, onEvent: (ev) => {
        if (ev.type === 'tool_result' && ev.call.id === 'edit-abort') ctrl.abort();
    } });
    ok(res.aborted && ctx.getScript().includes('local x0 = 99'), '中止不会假装已生效编辑被回滚');
    for (const id of ['read-abort', 'edit-abort', 'unused']) {
        eq(getSessionMessages().filter((m) => m.role === 'tool' && m.tool_call_id === id).length, 1, `${id} 完整配对`);
    }
}
{
    const calls = [{ id: 'a', name: 'read_current_script', args: { startLine: 1, lines: 10 } },
        { id: 'b', name: 'read_current_script', args: { startLine: 20, lines: 10 } }];
    reset([toolCallResponse(calls), toolCallResponse(calls), toolCallResponse(calls), textResponse('不会发送')]);
    const res = await runAgentTurn('重复多工具批次', makeCtx(), { cfg: CFG });
    ok(!res.ok && fetchCount === 3, '重复多工具批次也会中止');
}
{
    const cacheUsage = { prompt_tokens: 12000, prompt_cache_hit_tokens: 12000, completion_tokens: 1 };
    reset([toolCallResponse([{ id: 'a', name: 'read_current_script' }], cacheUsage), textResponse('预算超限', cacheUsage)]);
    const res = await runAgentTurn('缓存也计入预算', makeCtx(), { cfg: { ...CFG, taskBudgetTokens: 20000 } });
    ok(!res.ok && /预算/.test(res.error.message), '缓存 token 也触发预算闸');
}
{
    reset([toolCallResponse([{ id: 'partial', name: 'write_whole_file', args: { content: 'local changed = true' } }]).replace('"tool_calls","index"', '"length","index"')]);
    const ctx = makeCtx(), before = ctx.getScript();
    const res = await runAgentTurn('截断工具调用', ctx, { cfg: CFG });
    ok(!res.ok && /截断/.test(res.error.message) && ctx.getScript() === before, '截断工具调用不能执行');
}
{
    // 存档超过 512KB 时只裁存档；当前任务的真实目标不得因 debounce 保存丢失。
    reset([textResponse('第一轮')]);
    const target = '长期目标' + 'x'.repeat(530000);
    const cfg = { ...CFG, contextBudgetTokens: 1000000, requestBudgetTokens: 1000000, taskBudgetTokens: 1000000 };
    await runAgentTurn(target, makeCtx(), { cfg });
    ok(getSessionMessages().some((m) => m.content === target), '持久化上限不能裁运行中历史/当前目标');
}

// 同批次的读取结果尚未送回模型，不能给猜测的编辑授权
{
    const ctx = makeCtx(); const before = ctx.getScript();
    reset([toolCallResponse([
        { id: 'read-guess', name: 'read_current_script', args: { startLine: 1, lines: 10 } },
        { id: 'edit-guess', name: 'replace_in_file', args: { edits: [{ search: 'local x0 = 0', replace: 'local x0 = 123' }] } },
    ]), textResponse('需先分析读取结果')]);
    await runAgentTurn('读取后才能修改', ctx, { cfg: CFG });
    ok(ctx.getScript() === before, '同批次先读后猜测编辑必须拒绝');
}

// 网络断开可能已经生成/计费，不能盲重发；明确 HTTP 5xx 仍保留一次重试
{
    const original = globalThis.fetch;
    let count = 0;
    globalThis.fetch = async () => { count++; throw new Error('network lost'); };
    clearHistory();
    const failed = await runAgentTurn('网络失败', makeCtx(), { cfg: CFG });
    ok(!failed.ok && count === 1 && failed.error.kind === 'network', '网络断开不自动重发，避免重复费用');
    count = 0; clearHistory();
    globalThis.fetch = async () => ++count === 1 ? new Response('Unavailable', { status: 503 }) : new Response(textResponse('重试成功'));
    const recovered = await runAgentTurn('服务端暂时不可用', makeCtx(), { cfg: CFG });
    ok(recovered.ok && count === 2, '明确 HTTP 5xx 保留一次重试');
    globalThis.fetch = original;
}

// ── 结果 ────────────────────────────────────────────────────────────────────
if (failures.length) {
    console.error(`[ai-agent] 失败 ${failures.length} 项（通过 ${passed} 项）：`);
    for (const f of failures) console.error('   ✗ ' + f);
    process.exit(1);
}
console.log(`[ai-agent] OK：${passed} 项断言通过（请求体无 _meta / 多工具合并 / 无进展中止 / 预算闸 / 压缩生效 / 多轮上下文保留）`);
