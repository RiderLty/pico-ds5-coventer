import { register } from 'node:module';
register('./ai-raw-loader.mjs', import.meta.url);
const store = new Map();
globalThis.localStorage = { getItem: (k) => store.get(k), setItem: (k, v) => store.set(k, v), removeItem: (k) => store.delete(k) };
const { executeTool, undoByTs, undoCount } = await import('../src/utils/aiTools.js');
const { deviceSession } = await import('../src/utils/deviceSession.js');
const { chatCompletionStream } = await import('../src/utils/aiClient.js');
const { normalizeAiCfg, loadAiCfg, initAiCfg } = await import('../src/utils/aiSettings.js');
const { pushLog, clearLogs } = await import('../src/utils/logStore.js');
let passed = 0;
const failures = [];
function ok(condition, name) { if (condition) passed++; else failures.push(name); }
function ctx(script) { return { getScript: () => script, applyScript: (s) => { script = s; }, toolState: {} }; }
function call(name, args = {}) { return { id: 't', function: { name, arguments: JSON.stringify(args) } }; }
async function read(c) { return executeTool(call('read_current_script'), c); }
{
    const c = ctx('local a = 1');
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 2' }] }), c);
    ok(!result.ok && c.getScript() === 'local a = 1', '未读取代码不能编辑');
    await read(c);
    c.applyScript('local a = 3');
    ok(!(await executeTool(call('write_whole_file', { content: 'local a = 2' }), c)).ok, '人工改动后旧读取失效');
}
{
    const c = ctx('local a = "a  b"');
    await read(c);
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = "a b"', replace: 'local a = "wrong"' }] }), c);
    ok(!result.ok && c.getScript().includes('a  b'), '不能归一化字符串内的空白');
}
{
    const c = ctx('local a = 1  \nlocal a = 1\t');
    await read(c);
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 2' }] }), c);
    ok(!result.ok && c.getScript().includes('local a = 1'), '宽松匹配也必须唯一');
}
{
    const c = ctx('local a = 1'); await read(c);
    const edit = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 2' }] }), c);
    await read(c);
    const edit2 = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 2', replace: 'local a = 3' }] }), c);
    ok(edit.ok && edit2.ok && edit2.ts > edit.ts, '多次编辑撤销标识唯一');
    const entry = undoByTs(edit.ts); c.applyScript(entry.before);
    ok(c.getScript() === 'local a = 1' && !undoByTs(edit2.ts), '撤销早期编辑同时废弃后续编辑');
    const count = undoCount(); await read(c);
    await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 1' }] }), c);
    ok(undoCount() === count, '无变化编辑不产生撤销记录');
}
{
    const c = ctx('local a = 1'); await read(c);
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 2' }, { search: '不存在', replace: '' }] }), c);
    ok(!result.ok && result.applied === 0 && c.getScript() === 'local a = 1', '批量失败回执明确实际应用零块');
}
{
    clearLogs(); for (let i = 0; i < 2000; i++) pushLog('[lua] old');
    const promise = executeTool(call('wait_for_log', { pattern: 'new', timeout: 1 }), ctx(''));
    pushLog('[lua] new');
    const r = await promise;
    ok(r.ok && r.line === '[lua] new', '满环形缓冲仍可匹配新日志');
}
{
    const bodies = [];
    globalThis.fetch = async (url, init) => {
        bodies.push(JSON.parse(init.body));
        return bodies.length === 1
            ? new Response('{"error":{"message":"Unsupported parameter max_tokens. Use max_completion_tokens instead."}}', { status: 400 })
            : new Response(JSON.stringify({ choices: [{ message: { content: 'ok' }, finish_reason: 'stop' }] }), { status: 200 });
    };
    await chatCompletionStream({ url: 'https://fake', stream: false, maxTokens: 8192 });
    ok(bodies.length === 2 && bodies[1].max_completion_tokens === 8192 && !bodies[1].max_tokens,
        '输出字段降级保留输出额度且不先移除无关 stream_options');
    let count = 0;
    globalThis.fetch = async () => { count++; return new Response('{"error":{"code":"context_length_exceeded","message":"maximum context length exceeded"}}', { status: 400 }); };
    let error;
    try { await chatCompletionStream({ url: 'https://fake' }); } catch (e) { error = e; }
    ok(error?.kind === 'context_length' && /上下文|窗口/.test(error.message) && count === 1, '上下文超限分类并且不盲重试');
    globalThis.fetch = async () => new Response('data: {"error":{"message":"provider failed"}}\n\n', { status: 200 });
    error = null;
    try { await chatCompletionStream({ url: 'https://fake' }); } catch (e) { error = e; }
    ok(!!error && /provider failed/.test(error.message), 'SSE 内 error 不能被当成空成功');
    globalThis.fetch = async () => new Response('data: {"choices":[{"delta":{"content":"half"}}]}\n\n', { status: 200 });
    error = null;
    try { await chatCompletionStream({ url: 'https://fake' }); } catch (e) { error = e; }
    ok(!!error, '流中断缺少结束标记必须失败');
}
{
    ok(loadAiCfg().contextBudgetTokens === 1000000, '旧配置缺容量默认 1M');
    for (const value of [undefined, null, 0, -1, 'bad']) {
        ok(normalizeAiCfg({ contextBudgetTokens: value }).contextBudgetTokens === 1000000, `非法容量 ${value} 安全回落`);
    }
    ok(normalizeAiCfg({ contextBudgetTokens: '128000' }).contextBudgetTokens === 128000, '数字字符串旧配置可兼容');
    const migrated = normalizeAiCfg({ contextBudgetTokens: 1000000, requestBudgetTokens: 128000 });
    ok(migrated.requestBudgetAuto && migrated.requestBudgetTokens === 1000000, '旧的固定 128K 配置迁移为跟随模型窗口');
    const preserved = normalizeAiCfg({ contextBudgetTokens: 1000000, requestBudgetTokens: 256000 });
    ok(!preserved.requestBudgetAuto && preserved.requestBudgetTokens === 256000, '旧的自定义单轮上限继续保留');
    const manual = normalizeAiCfg({ contextBudgetTokens: 1000000, requestBudgetTokens: 128000, requestBudgetAuto: false });
    ok(!manual.requestBudgetAuto && manual.requestBudgetTokens === 128000, '明确关闭跟随后保留手动单轮上限');
    store.set('pico_ai_cfg_v1', JSON.stringify({ apiKey: 'legacy' }));
    globalThis.fetch = async (url, init) => init.method === 'GET'
        ? new Response('{}', { status: 200 }) : new Response('', { status: 500 });
    await initAiCfg();
    ok(store.has('pico_ai_cfg_v1'), '迁移写入失败保留旧配置');
}
// Lua 长字符串中的行尾空白具有语义，不能靠文本归一化猜测匹配
{
    const c = ctx('local s = [[hello  \nworld]]'); delete c.toolState;
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local s = [[hello\nworld]]', replace: 'local s = "wrong"' }] }), c);
    ok(!result.ok && c.getScript().includes('hello  '), 'Lua 长字符串行尾空白不能容错匹配');
}
// 明确参数降级、普通 400 与已开始生成的失败不能乱重试
{
    for (const field of ['stream_options', 'temperature', 'stream']) {
        const bodies = [];
        globalThis.fetch = async (url, init) => {
            bodies.push(JSON.parse(init.body));
            if (bodies.length === 1) return new Response(`Unsupported parameter: ${field}`, { status: 400 });
            return field === 'stream'
                ? new Response(JSON.stringify({ choices: [{ message: { content: 'ok' }, finish_reason: 'stop' }] }))
                : new Response('data: {"choices":[{"delta":{"content":"ok"},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n');
        };
        const r = await chatCompletionStream({ url: 'https://fake', temperature: 0.2 });
        ok(r.content === 'ok' && bodies.length === 2, `${field} 明确拒绝仅降级一次`);
        ok(field === 'stream' ? bodies[1].stream === false : !Object.hasOwn(bodies[1], field), `${field} 精确降级`);
    }
    let count = 0;
    globalThis.fetch = async () => { count++; return new Response('invalid request: tools are not supported', { status: 400 }); };
    let error;
    try { await chatCompletionStream({ url: 'https://fake', tools: [call('read_current_script')] }); } catch (e) { error = e; }
    ok(count === 1 && error?.kind === 'invalid_request', '不支持 tools 时不能悄悄删工具并重试');
}
// reload/新日志游标/错误状态的组合流程
{
    const c = ctx('print("new")');
    const oldRun = deviceSession.run;
    const oldSnapshot = deviceSession.getSnapshot;
    deviceSession.run = async () => { pushLog('[lua] new'); return { size: 12 }; };
    deviceSession.getSnapshot = () => ({ state: 'connected', status: { loaded: true, error: '' }, error: '' });
    const r = await executeTool(call('reload_script'), c);
    const found = await executeTool(call('wait_for_log', { pattern: 'new', afterCursor: r.logBaselineCursor, timeout: 1 }), c);
    const logs = await executeTool(call('get_device_logs', { afterCursor: r.logBaselineCursor }), c);
    ok(r.ok && found.ok && logs.lines.includes('[lua] new'), '游标能找到加载期间立即产生的日志');
    deviceSession.run = async () => { throw new Error('runtime error'); };
    deviceSession.getSnapshot = () => ({ state: 'connected', status: { loaded: true, error: 'runtime error' }, error: 'runtime error' });
    const failed = await executeTool(call('reload_script'), c);
    ok(!failed.ok && failed.error === 'runtime error', 'HTTP 200 但 Lua 状态有错误不能判成功');
    const endpoints = [];
    deviceSession.run = async () => { throw new Error('syntax error'); };
    await executeTool(call('reload_script'), c);
    ok(!endpoints.includes('/lua/save'), '加载工具始终不写 Flash');
    deviceSession.run = oldRun;
    deviceSession.getSnapshot = oldSnapshot;
}
{
    const c = ctx('local a = 1'); await read(c);
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local b = 2' }, { search: 'local b = 2', replace: 'local c = 3' }] }), c);
    ok(result.ok && c.getScript() === 'local c = 3', '批量顺序编辑支持前一块插入的内容');
}
{
    const c = ctx('local a = 1'); delete c.toolState;
    const result = await executeTool(call('replace_in_file', { edits: [{ search: 'local a = 1', replace: 'local a = 2' }, { search: 'local a = 3', replace: '' }] }), c);
    ok(!result.ok && result.nearestContent?.includes('local a = 1') && !result.nearestContent.includes('local a = 2'), '原子编辑失败诊断必须来自尚未改动的编辑器原文');
}
{
    globalThis.fetch = async () => new Response(JSON.stringify({ choices: [{ message: { content: 'endpoint ignores stream' }, finish_reason: 'stop' }], usage: { input_tokens: 100, output_tokens: 5 } }), { headers: { 'Content-Type': 'application/json' } });
    let response;
    try { response = await chatCompletionStream({ url: 'https://fake' }); } catch {}
    ok(response?.content === 'endpoint ignores stream' && response?.usage.inputTokens === 100, '端点忽略 stream 返回 JSON 时仍可解析并计量');
}
// 刷新恢复时不允许存档中的孤立结果和未完成调用污染下一轮请求
{
    store.set('ds5_ai_chat_v1', JSON.stringify([
        { role: 'user', content: '保留目标' },
        { role: 'tool', tool_call_id: 'orphan', content: '{}' },
        { role: 'assistant', content: '', tool_calls: [call('read_current_script')] },
    ]));
    const { getSessionMessages } = await import('../src/utils/aiAgent.js');
    ok(getSessionMessages().length === 1 && getSessionMessages()[0].content === '保留目标', '刷新恢复过滤孤立和未完成工具块');
}
if (failures.length) { console.error(`[ai-safety] ${failures.length} failures (${passed} passed):\n${failures.join('\n')}`); process.exit(1); }
console.log(`[ai-safety] OK: ${passed} assertions`);
