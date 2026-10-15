// ---------------------------------------------------------------------------
// AI 工具集：schema 定义 + 执行器 + 撤销栈
// 执行器永不 throw，失败统一 {ok:false, error, hint} JSON 回传给模型。
// 故意不暴露 luaSave/写入 flash——持久化永远由用户手动「写入存储」。
// ---------------------------------------------------------------------------
import { picoLogs, clearLogs, subscribeLogs } from './logStore';
import { luaSet, luaState } from './picoApi';

const SCRIPT_MAX_BYTES = 60 * 1024;
// 小脚本直接整份返回（一次读完最省轮数）；超过这个大小才先给大纲、按窗口读。
const SCRIPT_INLINE_MAX_CHARS = 8000;
// 行窗口：默认 200 行、上限 300 行（≈ 8~12KB 字符，单次结果受控）
const READ_WINDOW_LINES = 200;
const READ_WINDOW_MAX_LINES = 300;
// 日志：默认 30 行（旧版 60 行，实测大部分场景只需最后几行 + grep）
const LOG_LINES_DEFAULT = 30;
const LOG_LINES_MAX = 200;
// 超过这个行数就做"前 10 + 后 20"折叠（避免一次把 200 行原文全塞进上下文）
const LOG_COLLAPSE_AT = 40;

// ── 工具 schema（发给 API 的 tools 数组）──
export const AI_TOOLS = [
    {
        type: 'function',
        function: {
            name: 'read_current_script',
            description: '读取编辑器中当前 Lua 脚本。三种用法：① 不带参数——脚本 ≤8KB 时整份返回；超过 8KB 时返回**大纲**（每行行号 + 内容首行，函数定义全给）并提示按窗口读；② outline=true——强制只要大纲；③ startLine+lines——按行窗口读取（默认 200 行，上限 300）。**先看大纲、再只读要改的那几行**，不要整份读完（整份读会长期占用上下文预算）。修改前必须先读取相关内容。',
            parameters: {
                type: 'object',
                properties: {
                    outline: { type: 'boolean', description: 'true = 只要大纲（行号 + 结构），不看正文' },
                    startLine: { type: 'integer', description: '窗口起始行（1 起）；缺省 1' },
                    lines: { type: 'integer', description: '窗口行数，缺省 200、上限 300' },
                },
                required: [],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'replace_in_file',
            description: '对当前脚本应用一组精确的搜索/替换块。search 必须与脚本内容逐字符匹配（含缩进与行尾），多个块按顺序依次应用（前一块的替换结果对后一块可见）。**一次尽量把所有改动放进同一个 edits 数组**——一轮里发多个编辑块不额外花钱，拆成多次调用才贵。适用于局部修改；新建脚本或改动范围极大时用 write_whole_file。',
            parameters: {
                type: 'object',
                properties: {
                    edits: {
                        type: 'array',
                        description: '按应用顺序排列的编辑块（可一次给多个）',
                        items: {
                            type: 'object',
                            properties: {
                                search: { type: 'string', description: '要查找的原文片段，必须唯一且逐字符匹配' },
                                replace: { type: 'string', description: '替换后的内容；删除内容时传空串' },
                            },
                            required: ['search', 'replace'],
                        },
                    },
                    explanation: { type: 'string', description: '一句话说明本次改动目的（展示给用户）' },
                },
                required: ['edits'],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'write_whole_file',
            description: '整体替换当前脚本内容。仅在新建脚本或改动范围极大时使用。',
            parameters: {
                type: 'object',
                properties: {
                    content: { type: 'string', description: '完整的 Lua 脚本内容' },
                    explanation: { type: 'string', description: '一句话说明本次改动目的' },
                },
                required: ['content'],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'get_device_logs',
            description: '读取设备运行日志（脚本 print 输出与错误），附脚本运行状态。单次默认只返回最近 30 行（上限 200）。**找特定输出请用 grep**（浏览器过滤，只回匹配行，比拉 200 行省得多）。返回带 olderAvailable 与 nextFromEnd 时，可带 fromEnd=nextFromEnd 读更早日志。',
            parameters: {
                type: 'object',
                properties: {
                    afterCursor: { type: 'integer', description: '只看 reload_script 返回的 logBaselineCursor 之后的新日志；缓冲溢出也能正确区分' },
                    grep: { type: 'string', description: '只看匹配该正则的日志行（浏览器过滤）。例如 "AUTO_FIRE|error"。含特殊字符想按字面匹配时可能失效，优先给简单词' },
                    lines: { type: 'integer', description: '本次返回行数，缺省 30，上限 200（用 grep 时指最多返回多少条匹配）' },
                    fromEnd: { type: 'integer', description: '从末尾往前跳过的行数（分页游标），缺省 0；传上一次返回的 nextFromEnd 读取更早日志' },
                    scope: { type: 'string', enum: ['lua', 'all'], description: '日志范围：lua=仅 [lua 行（缺省），all=全部设备日志（含按键/手柄事件行）' },
                },
                required: [],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'clear_device_logs',
            description: '清空设备日志缓冲（全部历史，清空后不可恢复，用户可能仍需要其中的信息）。严禁例行调用——不要在每次修改或 reload_script 前默认清空；仅当日志混杂导致确实无法区分新旧输出时才使用，且清空前先向用户说明原因。清空后 get_device_logs 只会返回新产生的日志。',
            parameters: { type: 'object', properties: {}, required: [] },
        },
    },
    {
        type: 'function',
        function: {
            name: 'sleep',
            description: '等待指定秒数（1-30，缺省 1）。仅用于用户明确要求的等待；观察输出优先 wait_for_log。不要用它无意义地拖延。',
            parameters: {
                type: 'object',
                properties: {
                    seconds: { type: 'number', description: '等待秒数，1-30，缺省 1' },
                },
                required: [],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'wait_for_log',
            description: '等待设备日志中出现匹配的新行，出现即返回（只匹配等待开始后新产生的日志，历史日志不计）。主要用于等待用户操作触发的脚本输出（如"等用户按下按键后脚本打印确认"）。匹配方式：contains（包含，缺省）/ exact（整行完全一致）/ regex（正则）。超时未命中返回失败，可用 get_device_logs 查看当前日志排查。',
            parameters: {
                type: 'object',
                properties: {
                    afterCursor: { type: 'integer', description: '从 reload_script 的 logBaselineCursor 开始匹配，避免错过加载后立即产生的输出；缺省仅等待调用后新日志' },
                    pattern: { type: 'string', description: '匹配内容（按 match 方式解释）' },
                    match: { type: 'string', enum: ['contains', 'exact', 'regex'], description: '匹配方式，缺省 contains' },
                    timeout: { type: 'number', description: '超时秒数，1-120，缺省 30' },
                    scope: { type: 'string', enum: ['lua', 'all'], description: '日志范围：lua=仅 [lua 行（缺省），all=全部设备日志（含按键/手柄事件行）' },
                },
                required: ['pattern'],
            },
        },
    },
    {
        type: 'function',
        function: {
            name: 'reload_script',
            description: '把编辑器当前内容热重载到设备（等效"加载到内存"，不写 flash）。返回运行状态和 logBaselineCursor；按需用 get_device_logs 或 wait_for_log 的 afterCursor 验证本次输出，不必 sleep。若有 Lua 报错会原样返回，请修复后重试。',
            parameters: { type: 'object', properties: {}, required: [] },
        },
    },
];

// ── 撤销栈：每次 AI 编辑前压栈 ──
const undoStack = [];   // [{ before, after, ts, tool, explanation }]
const UNDO_MAX = 20;

// 单调递增时间戳：同一毫秒内连续多次编辑会使 Date.now() 碰撞，
// 导致 undoByTs 按 ts 定位串位，故强制严格递增
let lastUndoTs = 0;
function nextUndoTs() {
    const now = Date.now();
    lastUndoTs = now > lastUndoTs ? now : lastUndoTs + 1;
    return lastUndoTs;
}

export function pushUndo(entry) {
    undoStack.push(entry);
    if (undoStack.length > UNDO_MAX) undoStack.shift();
}

export function peekUndo() {
    return undoStack.length ? undoStack[undoStack.length - 1] : null;
}

// 撤销最近一次 AI 编辑：恢复 before 并移除该条目及其后所有条目
export function popUndo() {
    return undoStack.length ? undoStack.pop() : null;
}

// 撤销指定条目（按 ts 定位，通常来自消息卡片上的「撤销此编辑」按钮）：
// 恢复该次编辑前的内容，并移除该条目及其后所有条目（后续编辑基于旧版本，一并失效）。
// 返回被撤销的条目，找不到返回 null。
export function undoByTs(ts) {
    const idx = undoStack.findIndex((e) => e.ts === ts);
    if (idx < 0) return null;
    const entry = undoStack[idx];
    undoStack.splice(idx);
    return entry;
}

export function undoCount() { return undoStack.length; }

// ── 辅助 ──
const encoder = new TextEncoder();
const byteLen = (s) => encoder.encode(s).length;
function lineOf(script, idx) {
    return script.slice(0, idx).split('\n').length;
}

// 失配块的模糊定位：取 search 首个含字母/数字的行，在脚本行里做包含匹配，
// 命中行 ±5 行作为上下文回给 AI
function findNearestContent(script, search) {
    const lines = script.split('\n');
    const searchLines = search.split('\n');
    const probeLine = searchLines.find((l) => /[A-Za-z0-9_]/.test(l));
    if (!probeLine) return '';
    // 取探针行的核心词（去缩进/符号），截 20 字符防过长
    const probe = probeLine.trim().replace(/[^A-Za-z0-9_]+/g, ' ').trim().split(/\s+/)
        .filter((w) => w.length > 2)[0] || probeLine.trim().slice(0, 20);
    for (let i = 0; i < lines.length; i++) {
        if (lines[i].includes(probe)) {
            const from = Math.max(0, i - 5);
            const to = Math.min(lines.length, i + 6);
            return lines.slice(from, to).map((l, j) => `${from + j + 1}: ${l}`).join('\n');
        }
    }
    return '';
}

// 逐块顺序应用搜索/替换，返回 {ok, script, applied, failedIndex, error, nearestContent, foundAt[]}
function applyEdits(script, edits) {
    let cur = script;
    const foundAt = [];
    for (let i = 0; i < edits.length; i++) {
        const { search, replace } = edits[i] || {};
        if (typeof search !== 'string' || typeof replace !== 'string') {
            return { ok: false, applied: i, error: `第 ${i + 1} 个块的 search/replace 必须是字符串` };
        }
        if (!search) {
            return { ok: false, script: cur, applied: i, failedIndex: i,
                error: `第 ${i + 1} 个搜索块为空`, nearestContent: '', foundAt };
        }
        let idx = cur.indexOf(search);
        if (idx >= 0) {
            // 唯一性检查：多处命中时提示 AI 补上下文（避免改错位置）
            if (cur.indexOf(search, idx + 1) >= 0) {
                return { ok: false, script: cur, applied: i, failedIndex: i,
                    error: `第 ${i + 1} 个搜索块在脚本中出现多处，请扩大上下文使其唯一`,
                    nearestContent: findNearestContent(cur, search), foundAt };
            }
            cur = cur.slice(0, idx) + replace + cur.slice(idx + search.length);
            foundAt.push(lineOf(cur, idx));
            continue;
        }
        return {
            ok: false, script: cur, applied: i, failedIndex: i,
            error: `第 ${i + 1} 个搜索块未找到（大小写与空白必须完全一致）`,
            nearestContent: findNearestContent(cur, search),
            foundAt,
        };
    }
    return { ok: true, script: cur, applied: edits.length, foundAt };
}

// Lua 长字符串可跨行，行尾空白也可能是字符串内容。
// 搜索失配时返回原文诊断交给模型修正，不用文本归一化猜测编辑位置。
// 日志长度在环形缓冲满后不再增长，使用订阅事件生成单调游标。
// 与 logStore 同寿命，最多保留其 2000 条容量，不占 API 历史。
let logCursor = 0;
const logRecords = picoLogs.map((line) => ({ cursor: ++logCursor, line }));
subscribeLogs(() => {
    if (!picoLogs.length) { logRecords.length = 0; return; }
    logRecords.push({ cursor: ++logCursor, line: picoLogs[picoLogs.length - 1] });
    if (logRecords.length > 2000) logRecords.splice(0, logRecords.length - 2000);
});

function editReadError(ctx, before, edits) {
    const state = ctx.toolState;
    if (!state) return null; // 独立工具调用仍兼容；Agent 始终提供任务状态。
    const knownWindows = state.editWindows || state.readWindows;
    if (state.readScript !== before || !knownWindows?.length) {
        return { ok: false, applied: 0, error: '尚未读取当前脚本的代码上下文，或脚本已被修改', hint: '先 read_current_script 读取目标区间；大纲不能代替正文' };
    }
    if (edits) {
        let windows = knownWindows.slice();
        for (const edit of edits) {
            if (!edit || typeof edit.search !== 'string' || !windows.some((window) => window.includes(edit.search))) {
                return { ok: false, applied: 0, error: '搜索块不在已读取的代码区间中', hint: '读取包含目标搜索块的行窗口后重试' };
            }
            // 保留顺序编辑语义：后续块可以搜索前一个块刚插入的内容。
            windows = windows.map((window) => window.replace(edit.search, () => edit.replace));
        }
    }
    return null;
}

/**
 * 执行一个工具调用（call 为 OpenAI wire 格式 {id, type, function:{name, arguments}}）。
 * @param {Object} call
 * @param {Object} ctx { getScript: ()=>string, applyScript: (text)=>void }
 * @returns {Promise<Object>} 结果对象（回传给模型）
 */
export async function executeTool(call, ctx) {
    const name = call.function && call.function.name;
    let args = {};
    try { args = call.function && call.function.arguments ? JSON.parse(call.function.arguments) : {}; } catch (e) {
        return { ok: false, error: `参数不是有效 JSON: ${e.message}`, hint: '请重新调用并确保 arguments 是合法 JSON' };
    }
    if (!args || typeof args !== 'object' || Array.isArray(args)) return { ok: false, error: '参数必须是 JSON 对象', hint: '请用对象形式重新调用' };
    if (ctx.signal?.aborted) return { ok: false, aborted: true, error: '用户已停止，工具未执行' };
    try {
        switch (name) {
            case 'read_current_script': {
                const result = toolReadScript(ctx, args);
                if (ctx.toolState && result.ok && typeof result.script === 'string' && result.script) {
                    const snapshot = ctx.getScript();
                    if (ctx.toolState.readScript !== snapshot) ctx.toolState.readWindows = [];
                    ctx.toolState.readScript = snapshot;
                    ctx.toolState.readWindows.push(result.script);
                }
                return result;
            }
            case 'replace_in_file':
            case 'write_whole_file': {
                const result = name === 'replace_in_file' ? toolReplaceInFile(args, ctx) : toolWriteWholeFile(args, ctx);
                if (ctx.toolState) {
                    ctx.toolState.editFailed = !result.ok;
                    if (result.ok && !result.unchanged) ctx.toolState.editedScript = ctx.getScript();
                }
                return result;
            }
            case 'get_device_logs': return await toolGetLogs(args);
            case 'clear_device_logs': return toolClearLogs();
            case 'sleep': return toolSleep(args, ctx);
            case 'wait_for_log': return toolWaitForLog(args, ctx);
            case 'reload_script':
                if (ctx.toolState?.editFailed) return { ok: false, error: '最近一次编辑失败，未加载', hint: '先修复编辑；编辑工具成功后再加载到内存' };
                {
                    const snapshot = ctx.getScript();
                    const result = await toolReload(ctx);
                    if (result.ok && ctx.toolState) ctx.toolState.loadedScript = snapshot;
                    return result;
                }
            default: return { ok: false, error: `未知工具: ${name}` };
        }
    } catch (e) {
        return { ok: false, error: `工具执行异常: ${e.message}` };
    }
}

// ── 脚本大纲 ────────────────────────────────────────────────────────────────
// 大脚本的省 token 核心：先给"行号 + 每处开头"，模型自己决定读哪几行。
// 结构行（函数定义）全给，普通顶层语句给一部分——足矣定位，又只花几百字符。
const OUTLINE_MAX_ENTRIES = 120;
const OUTLINE_TOP_MAX = 40;
function buildScriptOutline(lines) {
    const fnRe = /^\s*(?:local\s+)?function\s*[\w.:]*\s*\(/;
    const assignFnRe = /^\s*[\w.:\[\]"']+\s*=\s*function\s*\(/;
    const fns = [];
    const tops = [];
    for (let i = 0; i < lines.length; i++) {
        const raw = lines[i];
        const t = raw.trim();
        if (!t || t.startsWith('--')) continue;
        if (fnRe.test(raw) || assignFnRe.test(raw)) {
            fns.push({ line: i + 1, text: t.slice(0, 100) });
            continue;
        }
        if (raw.length - raw.trimStart().length === 0) tops.push({ line: i + 1, text: t.slice(0, 100) });
    }
    // 函数定义优先；顶层语句按余量补，超出就丢（大纲只是"目录"，不是正文）
    const room = Math.max(0, OUTLINE_MAX_ENTRIES - fns.length);
    const picked = [...fns, ...tops.slice(0, Math.min(OUTLINE_TOP_MAX, room))]
        .sort((a, b) => a.line - b.line);
    return {
        outline: picked.map((e) => `${e.line}: ${e.text}`).join('\n'),
        entryCount: picked.length,
        fnCount: fns.length,
        omitted: Math.max(0, tops.length - Math.min(OUTLINE_TOP_MAX, room)),
    };
}

function toolReadScript(ctx, args) {
    const script = ctx.getScript();
    if (!script) {
        return { ok: false, error: '编辑器当前为空，可建议用户先加载脚本或用 write_whole_file 新建' };
    }
    const lines = script.split('\n');
    const totalLines = lines.length;
    const base = { totalLines, totalChars: script.length, totalBytes: byteLen(script) };

    // ① 大纲模式（显式 outline，或"大脚本且没指定窗口"）
    const wantOutline = args.outline === true || args.outline === 'true';
    const bigScript = script.length > SCRIPT_INLINE_MAX_CHARS;
    if (wantOutline) {
        const o = buildScriptOutline(lines);
        return {
            ok: true, mode: 'outline', ...base, ...o,
            note: `脚本共 ${totalLines} 行，以上是大纲（行号: 内容首行）。要看正文请用 startLine + lines 只读需要的区间，不要整份读。`,
        };
    }
    if (args.startLine === undefined && args.offset === undefined && bigScript) {
        const o = buildScriptOutline(lines);
        return {
            ok: true, mode: 'outline', ...base, ...o,
            note: `脚本较长（${script.length} 字符 / ${totalLines} 行），先给你大纲（行号: 内容首行）。请用 startLine + lines 只读要修改的区间（默认 200 行），不要整份读。`,
        };
    }
    if (args.startLine === undefined && args.offset === undefined) {
        // 小脚本：整份给（一次读完最省轮数）
        return { ok: true, mode: 'full', ...base, startLine: 1, endLine: totalLines, script };
    }

    // ② 行窗口模式
    let startLine = parseInt(args.startLine, 10);
    if (!Number.isFinite(startLine) && args.offset !== undefined) {
        // 旧参数（字符下标）兼容：换算成行号，避免老习惯的调用直接失败
        const co = parseInt(args.offset, 10);
        if (Number.isFinite(co) && co > 0) startLine = script.slice(0, co).split('\n').length;
    }
    if (!Number.isFinite(startLine) || startLine < 1) startLine = 1;
    let want = parseInt(args.lines, 10);
    if (!Number.isFinite(want) || want <= 0) want = READ_WINDOW_LINES;
    want = Math.min(want, READ_WINDOW_MAX_LINES);
    if (startLine > totalLines) {
        return { ok: false, mode: 'window', ...base, startLine, script: '', error: 'startLine 超过脚本行数', hint: `有效行号为 1~${totalLines}，请按大纲选择目标区间` };
    }
    const endLine = Math.min(totalLines, startLine + want - 1);
    const slice = lines.slice(startLine - 1, endLine).join('\n');
    const r = { ok: true, mode: 'window', ...base, startLine, endLine, script: slice };
    if (endLine < totalLines) {
        r.hasMore = true;
        r.nextStartLine = endLine + 1;
        r.note = `本次返回第 ${startLine}~${endLine} 行（共 ${totalLines} 行）；需要后续内容带 startLine=${endLine + 1} 继续读。`;
    }
    return r;
}

function toolReplaceInFile(args, ctx) {
    const edits = args.edits;
    if (!Array.isArray(edits) || !edits.length) {
        return { ok: false, error: 'edits 不能为空', hint: '提供 edits: [{search, replace}] 数组' };
    }
    const before = ctx.getScript();
    const guard = editReadError(ctx, before, edits);
    if (guard) return guard;
    const r = applyEdits(before, edits);
    if (!r.ok) {
        const diag = { ok: false, error: r.error, applied: 0, matchedBeforeFailure: r.applied, failedIndex: r.failedIndex };
        const nearestContent = findNearestContent(before, edits[r.failedIndex]?.search || '');
        if (nearestContent) {
            diag.nearestContent = nearestContent;
            diag.hint = '以上 nearestContent 是失败搜索块附近（±5 行）的实际脚本内容（带行号），请据此修正 search 后重试，或调用 read_current_script 重新读取';
        } else {
            diag.hint = '未找到相似内容，请调用 read_current_script 重新读取脚本';
        }
        // 注意：失配时已应用的块不回写（cur 只在内存），避免半套编辑造成用户困惑——
        // 全部块成功才一次性 applyScript
        return diag;
    }
    if (byteLen(r.script) > SCRIPT_MAX_BYTES) {
        return { ok: false, error: `修改后脚本 ${byteLen(r.script)} 字节，超出设备 60 KiB 上限，未应用`, applied: 0 };
    }
    if (r.script === before) return { ok: true, unchanged: true, applied: 0, note: '脚本没有变化，不产生撤销记录' };
    ctx.applyScript(r.script);
    const ts = nextUndoTs();
    pushUndo({ before, after: r.script, ts, tool: 'replace_in_file', explanation: args.explanation || '' });
    return { ok: true, applied: r.applied, byteLen: byteLen(r.script), foundAtLines: r.foundAt, explanation: args.explanation || '', ts };
}

function toolWriteWholeFile(args, ctx) {
    const content = args.content;
    if (typeof content !== 'string' || !content.trim()) {
        return { ok: false, error: 'content 不能为空' };
    }
    const bytes = byteLen(content);
    if (bytes > SCRIPT_MAX_BYTES) {
        return { ok: false, error: `脚本 ${bytes} 字节超出设备 60 KiB 上限`, hint: '精简脚本后重试' };
    }
    const before = ctx.getScript();
    const guard = before ? editReadError(ctx, before) : null;
    if (guard) return guard;
    if (content === before) return { ok: true, unchanged: true, note: '脚本没有变化' };
    ctx.applyScript(content);
    const ts = nextUndoTs();
    pushUndo({ before, after: content, ts, tool: 'write_whole_file', explanation: args.explanation || '' });
    return { ok: true, byteLen: bytes, lineCount: content.split('\n').length, explanation: args.explanation || '', ts };
}

function toolClearLogs() {
    const cleared = picoLogs.length;
    clearLogs();
    return { ok: true, cleared, note: '日志已清空，之后的 get_device_logs 只会返回新产生的日志' };
}

// 等待：给 AI 实现延时序列的能力（如"10s 后发事件"）。单次上限 30s 防拖延；
// 挂到 AbortSignal——用户点「停止」立即中断等待，不傻等满时长。
function toolSleep(args, ctx) {
    let s = parseFloat(args.seconds);
    if (!Number.isFinite(s) || s <= 0) s = 1;
    s = Math.min(s, 30);
    return new Promise((resolve) => {
        const done = (result) => {
            clearTimeout(timer);
            ctx.signal?.removeEventListener('abort', onAbort);
            resolve(result);
        };
        const onAbort = () => done({ ok: false, aborted: true, error: '等待被用户中断' });
        const timer = setTimeout(() => done({ ok: true, waited: s, note: '等待结束，可继续后续动作' }), s * 1000);
        ctx.signal?.addEventListener('abort', onAbort, { once: true });
        if (ctx.signal?.aborted) onAbort();
    });
}

// 等待日志出现：只匹配等待开始后新产生的行（历史不计，防旧输出假命中）。
// 基线索引随环形缓冲溢出同步前移；300ms 轮询；支持超时与用户中断。
function toolWaitForLog(args, ctx) {
    const pattern = typeof args.pattern === 'string' ? args.pattern : '';
    if (!pattern) return { ok: false, error: 'pattern 不能为空' };
    const match = ['exact', 'contains', 'regex'].includes(args.match) ? args.match : 'contains';
    let re = null;
    if (match === 'regex') {
        try { re = new RegExp(pattern); } catch (e) {
            return { ok: false, error: `正则无效: ${e.message}`, hint: '检查 pattern 语法' };
        }
    }
    let timeout = parseFloat(args.timeout);
    if (!Number.isFinite(timeout) || timeout <= 0) timeout = 30;
    timeout = Math.min(timeout, 120);
    const scope = args.scope === 'all' ? 'all' : 'lua';

    const testLine = (line) => (match === 'exact'
        ? line === pattern
        : match === 'regex' ? re.test(line) : line.includes(pattern));
    const baseline = Number.isFinite(args.afterCursor) ? args.afterCursor : logCursor;
    const t0 = Date.now();
    return new Promise((resolve) => {
        let timer, unsubscribe;
        const done = (result) => {
            clearTimeout(timer);
            unsubscribe?.();
            ctx.signal?.removeEventListener('abort', onAbort);
            resolve(result);
        };
        const onAbort = () => done({ ok: false, aborted: true, error: '等待被用户中断' });
        const scan = () => {
            const record = logRecords.find((r) => r.cursor > baseline
                && (scope === 'all' || r.line.startsWith('[lua')) && testLine(r.line));
            if (record) done({ ok: true, line: record.line, cursor: record.cursor,
                waited: +((Date.now() - t0) / 1000).toFixed(1) });
        };
        unsubscribe = subscribeLogs(scan);
        timer = setTimeout(() => done({ ok: false, timeout: true,
            waited: timeout, error: `等待超时：${timeout}s 内未出现匹配日志`,
            hint: '用 get_device_logs 查看状态；确认脚本含输出点、设备连接及触发条件，不要盲目重复等待' }), timeout * 1000);
        ctx.signal?.addEventListener('abort', onAbort, { once: true });
        if (ctx.signal?.aborted) onAbort();
        else scan();
    });
}

// 日志行数折叠：超过 LOG_COLLAPSE_AT 时只留头 10 + 尾 20，中间一行标记。
// 日志的价值集中在"最新几行"与"最早的启动/报错几行"，中间大段重复输出是纯浪费。
function collapseLogLines(arr) {
    if (!Array.isArray(arr) || arr.length <= LOG_COLLAPSE_AT) return { lines: arr, collapsed: 0 };
    const head = arr.slice(0, 10);
    const tail = arr.slice(-20);
    return {
        lines: [...head, `[... 中间省略 ${arr.length - 30} 行 ...]`, ...tail],
        collapsed: arr.length - 30,
    };
}

async function toolGetLogs(args) {
    let lines = parseInt(args.lines, 10);
    if (!Number.isFinite(lines) || lines <= 0) lines = LOG_LINES_DEFAULT;
    lines = Math.min(lines, LOG_LINES_MAX);
    let fromEnd = parseInt(args.fromEnd, 10);
    if (!Number.isFinite(fromEnd) || fromEnd < 0) fromEnd = 0;
    const scope = args.scope === 'all' ? 'all' : 'lua';
    const available = Number.isFinite(args.afterCursor)
        ? logRecords.filter((r) => r.cursor > args.afterCursor).map((r) => r.line) : picoLogs;
    const filtered = scope === 'all' ? available.slice() : available.filter((l) => l.startsWith('[lua'));

    let state = null;
    try { state = await luaState(); } catch (e) { /* 设备不在线时日志仍可返回 */ }

    // ── grep：浏览器过滤，只把匹配行发回上下文（拉 200 行只看 3 行是纯浪费）──
    const grep = typeof args.grep === 'string' ? args.grep.trim() : '';
    if (grep) {
        let re = null;
        let literal = false;
        try { re = new RegExp(grep); } catch (e) { literal = true; }   // 正则语法错 → 按字面包含匹配
        const test = literal ? (l) => l.includes(grep) : (l) => re.test(l);
        const matched = filtered.filter(test);
        const pick = matched.slice(-lines);
        const col = collapseLogLines(pick);
        const r = {
            ok: true, grep, matchCount: matched.length, lines: col.lines,
            total: filtered.length, scope, state,
            note: matched.length === 0
                ? `没有匹配 "${grep}" 的日志（[lua] 共 ${filtered.length} 行）。可换个关键词，或不带 grep 看最近几行。`
                : `匹配 ${matched.length} 行${matched.length > pick.length ? `，本次返回最后 ${pick.length} 条` : ''}。`,
        };
        if (col.collapsed) r.collapsedLines = col.collapsed;
        return r;
    }

    // ── 分页：从末尾往前取 [fromEnd, fromEnd+lines) 区间 ──
    const end = filtered.length - fromEnd;
    if (end <= 0) {
        return {
            ok: true, lines: [], total: filtered.length, fromEnd,
            note: fromEnd > 0 ? '该区间已无更早日志' : '暂无日志',
            state,
        };
    }
    const start = Math.max(0, end - lines);
    const col = collapseLogLines(filtered.slice(start, end));
    const r = {
        ok: true, lines: col.lines, total: filtered.length, fromEnd, scope, state,
    };
    if (col.collapsed) r.collapsedLines = col.collapsed;
    if (start > 0) {
        r.olderAvailable = true;
        r.nextFromEnd = filtered.length - start;
        r.note = `本次返回倒数第 ${fromEnd + 1}~${fromEnd + filtered.slice(start, end).length} 行（[lua] 共 ${filtered.length} 行）；如需更早日志，带 fromEnd=${r.nextFromEnd} 继续调用`;
    }
    return r;
}

async function toolReload(ctx) {
    const script = ctx.getScript();
    if (!script) return { ok: false, error: '编辑器为空，无可重载内容' };
    // 记录重载前的 [lua 日志行数：重载后 get_device_logs 的 total 超出该基线的
    // 部分即新脚本的输出，AI 据此区分新旧日志而无需清空
    const logBaselineCursor = logCursor;
    if (byteLen(script) > SCRIPT_MAX_BYTES) return { ok: false, error: '脚本超过 60 KiB，未加载', hint: '先精简脚本' };
    const logBaseline = picoLogs.filter((l) => l.startsWith('[lua')).length;
    let res;
    try { res = await luaSet(script, { signal: ctx.signal }); }
    catch (error) {
        let state = null;
        try { state = await luaState(); } catch { /* disconnected */ }
        return { ok: false, error: error.detail || error.message, state, logBaselineCursor,
            hint: '加载失败会恢复预制脚本。保留编辑器内容，按错误行号修复或检查设备连接。' };
    }
    const state = await luaState();
    return { ok: true, state, logBaseline, logBaselineCursor, message: res.message,
        note: `已加载到内存，未保存。用 afterCursor=${logBaselineCursor} 查询本次日志；写入存储须用户手动操作。` };
}
