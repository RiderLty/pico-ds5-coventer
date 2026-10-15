// ---------------------------------------------------------------------------
// AI 消息列表渲染：用户/助手气泡、tool call 卡片（编辑 diff、工具摘要）、
// 代码块正则高亮。不引 markdown/diff 库，控制单文件体积。
// ---------------------------------------------------------------------------
import React, { useState, useEffect, useRef, useCallback } from 'react';
import {
    Box, Stack, Typography, Collapse, IconButton,
} from '@mui/material';
import ExpandMoreIcon from '@mui/icons-material/ExpandMore';
import UndoIcon from '@mui/icons-material/Undo';
import { marked } from 'marked';
import DOMPurify from 'dompurify';
import { PANEL_ACCENT, PANEL_INK, PANEL_LINE, PANEL_MUTED, PANEL_SUNKEN } from '../PanelKit';
import { liveReasoningWindow, REASONING_LIVE_TAIL_CHARS } from '../../utils/aiText';
import { shouldFollowAfterGrowth } from '../../utils/aiScroll';

// GFM：表格/删除线/任务列表；breaks：单换行成 <br>（聊天场景更符合直觉）
marked.setOptions({ gfm: true, breaks: true });

// ── markdown 渲染（marked + DOMPurify 消毒；流式部分文本可安全增量解析）──
function AiText({ text }) {
    const html = React.useMemo(
        () => DOMPurify.sanitize(marked.parse(text || '', { async: false })),
        [text]
    );
    return (
        <Box sx={{
            '& p': { m: 0, my: 0.5, wordBreak: 'break-word' },
            '& h1, & h2, & h3, & h4, & h5, & h6': { m: '8px 0 4px', fontWeight: 700, lineHeight: 1.35 },
            '& h1': { fontSize: 15 }, '& h2': { fontSize: 14 }, '& h3, & h4, & h5, & h6': { fontSize: 13.5 },
            '& :first-child': { mt: 0 }, '& :last-child': { mb: 0 },
            '& pre': {
                m: 0, my: 0.5, p: 1, bgcolor: PANEL_SUNKEN, border: `1px solid ${PANEL_LINE}`, borderRadius: '6px',
                overflowX: 'auto', fontSize: 12,
                fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
                whiteSpace: 'pre-wrap', wordBreak: 'break-word',
            },
            '& code': {
                fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
                fontSize: '0.9em', px: 0.5, borderRadius: 0.5, bgcolor: 'action.hover',
            },
            '& pre code': { p: 0, bgcolor: 'transparent', fontSize: 12 },
            '& ul, & ol': { m: 0, my: 0.5, pl: 3.5 },
            '& li': { my: 0.25, wordBreak: 'break-word' },
            '& table': { borderCollapse: 'collapse', my: 0.5, maxWidth: '100%', display: 'block', overflowX: 'auto' },
            '& th, & td': { border: '1px solid', borderColor: 'divider', px: 1, py: 0.25, fontSize: 12, textAlign: 'left' },
            '& th': { bgcolor: 'action.hover', fontWeight: 700 },
            '& blockquote': { m: 0, my: 0.5, pl: 1.5, borderLeft: '3px solid', borderColor: 'divider', color: 'text.secondary' },
            '& a': { color: 'primary.main', wordBreak: 'break-all' },
            '& hr': { border: 'none', borderTop: '1px solid', borderColor: 'divider', my: 1 },
        }} dangerouslySetInnerHTML={{ __html: html }} />
    );
}

// ── mini diff：search/replace 各自拆行红绿并列 ──
function MiniDiff({ search, replace }) {
    const delLines = search.split('\n');
    const addLines = replace.split('\n');
    return (
        <Box sx={{ fontFamily: 'monospace', fontSize: 11, borderRadius: 1, overflow: 'hidden', border: '1px solid', borderColor: 'divider' }}>
            {delLines.map((l, i) => (
                <Box key={`d${i}`} sx={{ px: 1, py: '1px', bgcolor: 'var(--ai-diff-del-bg)', color: 'var(--ai-diff-del-ink)', whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                    - {l || ' '}
                </Box>
            ))}
            {addLines.map((l, i) => (
                <Box key={`a${i}`} sx={{ px: 1, py: '1px', bgcolor: 'var(--ai-diff-add-bg)', color: 'var(--ai-diff-add-ink)', whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                    + {l || ' '}
                </Box>
            ))}
        </Box>
    );
}

const TOOL_LABELS = {
    read_current_script: '读取脚本',
    replace_in_file: '编辑脚本',
    write_whole_file: '整体重写脚本',
    get_device_logs: '读取设备日志',
    clear_device_logs: '清空设备日志',
    sleep: '等待',
    wait_for_log: '等待日志',
    reload_script: '热重载到设备',
};

// tool result 一行摘要
function resultSummary(name, result) {
    if (!result) return '';
    if (name === 'read_current_script') {
        const total = result.totalBytes != null ? ` · 共 ${result.totalBytes}B` : '';
        return `已读取${result.offset ? ` offset=${result.offset}` : ''}${total}${result.truncated ? ' · 未完' : ''}`;
    }
    if (name === 'get_device_logs') {
        const total = result.total != null ? ` / 共 ${result.total}` : '';
        return `日志 ${result.lines ? result.lines.length : 0} 行${total}${result.olderAvailable ? ' · 有更早' : ''}${result.state ? ` · loaded=${result.state.loaded}` : ''}`;
    }
    if (name === 'reload_script') {
        return result.ok ? '✓ 已热重载' : `✗ ${result.error || '失败'}`;
    }
    if (name === 'sleep') {
        return result.ok ? `✓ 已等待 ${result.waited}s` : '已中断';
    }
    if (name === 'wait_for_log') {
        if (result.ok) return `✓ 命中 · 等了 ${result.waited}s`;
        if (result.aborted) return '已中断';
        if (result.timeout) return `✗ 超时（${result.waited}s）`;
        return '✗ 失败';
    }
    return result.ok ? '✓' : `✗ ${result.error || '失败'}`;
}

// ── 编辑类 tool call 卡片（含 diff + 结果状态 + 撤销）──
function EditCard({ call, result, onUndo }) {
    let edits = [];
    let explanation = '';
    const name = call.function && call.function.name;
    try {
        const args = JSON.parse((call.function && call.function.arguments) || '{}');
        edits = args.edits || [];
        explanation = args.explanation || '';
    } catch (e) { /* 参数损坏，显示原始 arguments */ }
    const applied = result && result.ok;
    return (
        <Box sx={{ p: 1, borderRadius: '6px', border: `1px solid ${PANEL_LINE}`, bgcolor: 'var(--panel-surface)' }}>
            <Stack direction="row" spacing={1} alignItems="center" sx={{ mb: edits.length ? 1 : 0 }}>
                <Typography sx={{ fontSize: 11.5, fontWeight: 600, color: PANEL_INK }}>
                    {applied === true ? '✓' : applied === false ? '✗' : '…'} {TOOL_LABELS[name] || name}
                    {edits.length ? ` · ${edits.length} 处修改` : ''}
                </Typography>
                {explanation && (
                    <Typography sx={{ fontSize: 11, color: PANEL_MUTED, flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {explanation}
                    </Typography>
                )}
                {applied && result.ts && onUndo && (
                    <IconButton size="small" title="撤销此编辑（其后编辑一并撤销）" onClick={() => onUndo(result.ts)}>
                        <UndoIcon sx={{ fontSize: 16 }} />
                    </IconButton>
                )}
            </Stack>
            {edits.map((e, i) => (
                <Box key={i} sx={{ mb: i < edits.length - 1 ? 1 : 0 }}>
                    <MiniDiff search={e.search || ''} replace={e.replace || ''} />
                </Box>
            ))}
            {applied === true && result.foundAtLines && result.foundAtLines.length > 0 && (
                <Typography sx={{ fontSize: 11, color: PANEL_MUTED, mt: 0.5, display: 'block' }}>
                    已应用 · 第 {result.foundAtLines.join('、')} 行
                </Typography>
            )}
            {applied === false && (
                <Typography sx={{ fontSize: 11, color: 'var(--panel-danger)', mt: 0.5, display: 'block', whiteSpace: 'pre-wrap' }}>
                    {result.error}
                </Typography>
            )}
        </Box>
    );
}

// ── 折叠的工具结果卡片 ──
function ToolResultCard({ call, result }) {
    const [open, setOpen] = React.useState(false);
    const name = call.function && call.function.name;
    if (name === 'replace_in_file' || name === 'write_whole_file') {
        return <EditCard call={call} result={result} />;
    }
    return (
        <Box sx={{ borderRadius: '6px', border: `1px solid ${PANEL_LINE}`, bgcolor: 'var(--panel-surface)' }}>
            <Stack direction="row" spacing={0.5} alignItems="center"
                onClick={() => setOpen((o) => !o)} sx={{ px: 1, py: 0.5, cursor: 'pointer' }}>
                <ExpandMoreIcon sx={{ fontSize: 16, transform: open ? 'rotate(180deg)' : 'none', transition: '0.15s', color: 'text.secondary' }} />
                <Typography sx={{ fontSize: 11.5, fontWeight: 600, color: PANEL_INK, flex: 1 }}>
                    {TOOL_LABELS[name] || name}
                </Typography>
                <Typography sx={{ fontSize: 11, color: result && result.ok === false ? 'var(--panel-danger)' : PANEL_MUTED }}>
                    {resultSummary(call.name, result)}
                </Typography>
            </Stack>
            <Collapse in={open}>
                <Box sx={{ px: 1, pb: 1 }}>
                    <Box component="pre"
                        sx={{ fontFamily: 'ui-monospace, Menlo, monospace', fontSize: 11, m: 0, p: 1, bgcolor: PANEL_SUNKEN, border: `1px solid ${PANEL_LINE}`, borderRadius: '6px', maxHeight: 200, overflow: 'auto', whiteSpace: 'pre-wrap', wordBreak: 'break-word' }}>
                        {result ? JSON.stringify(result, null, 1) : '执行中…'}
                    </Box>
                </Box>
            </Collapse>
        </Box>
    );
}

// ── 思考过程折叠块（面板运行中与消息列表共用）───────────────────────────────
// 推理模型的思维链只做展示：**默认收起**（不占版面），点一下展开回看"当时在想什么"。
// 它不在发给端点的消息里（aiAgent 把它挂在 assistant._reasoning 上，sanitizeMessage 会剥掉），
// 也不计入上下文预算，所以展开看纯属免费。
export function ReasoningBlock({ text, live }) {
    // 默认展开：用户要的就是"不用点也能看见它在想什么"。
    const [open, setOpen] = useState(true);
    const bodyRef = useRef(null);
    const full = text || '';

    // 实时只渲染**尾部窗口**（规则与阈值见 utils/aiText.js，那里有单测）：
    // 思维链可能上万字，整块 pre-wrap 每隔一百多毫秒重排一次会明显拖帧。
    // 历史块不截尾——回看时应该能翻到开头。
    const LIVE_TAIL_CHARS = REASONING_LIVE_TAIL_CHARS;
    const { shown, trimmed } = liveReasoningWindow(full, live);

    // 折叠块自己滚到底（廉价：只写 scrollTop，不做 scrollIntoView 那种强制重排）；
    // 用户自己往上翻了就不抢。
    //
    // **判据必须用 shouldFollowAfterGrowth()，不能拿"离底距离"当闸门**（2026-09-26 修）：
    // 旧实现是 `if (离底 > 60) return;`——思考按 120ms 批量刷新，一次刷新就长高好几行，
    // 离底超过 60px 后被当成"用户上翻"，**永久停止跟随**（实测离底冻结在 658px 而内容
    // 还在长，用户看到的就是"不能自动滚到最新"）。现在判据是"这次长高**之前**在不在底部"，
    // 用上一次的 scrollHeight 算，内容自己长高骗不到它。
    const prevHRef = useRef(0);   // 上一次内容变化时的 scrollHeight
    const pinBottom = useCallback(() => {
        const el = bodyRef.current;
        if (!el) return;
        const follow = shouldFollowAfterGrowth({ prevH: prevHRef.current, top: el.scrollTop, clientH: el.clientHeight });
        prevHRef.current = el.scrollHeight;
        if (follow) el.scrollTop = el.scrollHeight;
    }, []);
    useEffect(() => {
        if (!open) return;
        const id = requestAnimationFrame(pinBottom);
        return () => cancelAnimationFrame(id);
    }, [open, shown, pinBottom]);

    if (!full) return null;
    return (
        <Box sx={{ border: `1px dashed ${PANEL_LINE}`, borderRadius: '6px', overflow: 'hidden' }}>
            <Box
                onClick={() => setOpen((o) => !o)}
                sx={{
                    display: 'flex', alignItems: 'center', gap: 0.5, px: 1, py: 0.25,
                    cursor: 'pointer', bgcolor: 'var(--panel-sunken)', userSelect: 'none',
                }}
            >
                <ExpandMoreIcon sx={{
                    fontSize: 14, color: PANEL_MUTED,
                    transform: open ? 'rotate(180deg)' : 'none', transition: 'transform .15s',
                }} />
                <Typography sx={{ fontSize: 10.5, color: PANEL_MUTED }}>
                    {live ? '正在思考' : '思考过程'}（{full.length} 字）
                    {open ? '' : ' · 点击展开'}
                    {open && trimmed ? ` · 仅显示最新 ${LIVE_TAIL_CHARS} 字` : ''}
                </Typography>
            </Box>
            {open && (
                <Box
                    ref={bodyRef}
                    data-ai-scroll="reasoning"
                    sx={{
                        px: 1, py: 0.5, maxHeight: 200, overflowY: 'auto',
                        bgcolor: 'var(--panel-surface)', borderTop: `1px solid ${PANEL_LINE}`,
                    }}
                >
                    <Typography sx={{
                        fontSize: 10.5, lineHeight: 1.65, color: PANEL_MUTED,
                        whiteSpace: 'pre-wrap', wordBreak: 'break-word',
                    }}>
                        {shown}
                    </Typography>
                </Box>
            )}
        </Box>
    );
}

// ── 主渲染 ──
// messages: aiAgent 的 session.messages（tool 消息含 _meta）
// rev: 历史版本号（父级传 messages.length）——session.messages 是**原地 push** 的，
//      memo 只比引用会导致历史永远不刷新，所以必须给一个会变的标量。
function AiMessageList({ messages, onUndo }) {
    // 把 assistant(tool_calls) + 后续 tool 消息聚合渲染
    const items = [];
    let i = 0;
    while (i < messages.length) {
        const m = messages[i];
        if (m.role === 'user' && m._compactNotice) {
            // 压缩提示不是用户说的话：渲染成居中小字条，别装成用户气泡
            items.push(
                <Box key={i} sx={{ display: 'flex', justifyContent: 'center' }}>
                    <Typography sx={{
                        fontSize: 10.5, lineHeight: 1.6, color: PANEL_MUTED, textAlign: 'center',
                        px: 1, py: 0.25, maxWidth: '92%',
                        border: `1px dashed ${PANEL_LINE}`, borderRadius: '6px',
                    }}>
                        {m.content}
                    </Typography>
                </Box>
            );
            i++;
        } else if (m.role === 'user') {
            items.push(
                <Box key={i} sx={{ display: 'flex', justifyContent: 'flex-end' }}>
                    <Box sx={{ px: 1.5, py: 0.75, borderRadius: '10px', bgcolor: PANEL_ACCENT, color: '#fff', maxWidth: '85%' }}>
                        <Typography sx={{ fontSize: 12.5, lineHeight: 1.65, whiteSpace: 'pre-wrap', wordBreak: 'break-word' }}>{m.content}</Typography>
                    </Box>
                </Box>
            );
            i++;
        } else if (m.role === 'assistant') {
            // tool call 卡片由其后续的 tool 消息渲染（那里才有执行结果）
            items.push(
                <Stack key={i} spacing={0.5} sx={{ alignItems: 'stretch' }}>
                    {m._reasoning && <ReasoningBlock text={m._reasoning} />}
                    {m.content && <AiText text={m.content} />}
                </Stack>
            );
            i++;
        } else if (m.role === 'tool') {
            // 找到对应的 tool call（向前找最近的 assistant 消息）
            let call = null;
            for (let k = i - 1; k >= 0; k--) {
                const pm = messages[k];
                if (pm.role === 'tool') continue;
                if (pm.role === 'assistant' && pm.tool_calls) {
                    call = pm.tool_calls.find((tc) => tc.id === m.tool_call_id);
                }
                break;
            }
            if (call && (call.function?.name === 'replace_in_file' || call.function?.name === 'write_whole_file')) {
                items.push(<EditCard key={i} call={call} result={m._meta ? m._meta.result : null} onUndo={onUndo} />);
            } else if (call) {
                items.push(<ToolResultCard key={i} call={call} result={m._meta ? m._meta.result : null} />);
            }
            i++;
        } else {
            i++;
        }
    }
    return (
        // selectable：AI 的回复 / 思维链 / 工具结果 / 代码块 / diff 都要能选中复制
        //（全站默认不可选，见 index.css 的「文字选择策略」）。
        <Stack spacing={1} className="selectable" sx={{ p: 1 }}>
            {items}
        </Stack>
    );
}

// 流式正文/思考每秒刷新好几次，而历史消息（含 markdown）跟这些刷新无关：
// memo 掉之后每次刷新只重渲染正在流的那一小块，长对话时差别很明显。
export default React.memo(AiMessageList, (a, b) => (
    a.messages === b.messages && a.rev === b.rev && a.onUndo === b.onUndo
));
