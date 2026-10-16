// ---------------------------------------------------------------------------
// AI 助手聊天侧栏：消息列表 + 输入区 + 设置入口。
// 编辑动作经 props.applyScript（LuaEditorDialog.updateScript）落到编辑器。
// ---------------------------------------------------------------------------
import React, { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import {
    Box, Stack, TextField, IconButton, Button, Typography, Tooltip,
    LinearProgress,
} from '@mui/material';
import SendIcon from '@mui/icons-material/Send';
import StopIcon from '@mui/icons-material/Stop';
import SettingsIcon from '@mui/icons-material/Settings';
import UndoIcon from '@mui/icons-material/Undo';
import DeleteSweepIcon from '@mui/icons-material/DeleteSweep';
import AiMessageList, { ReasoningBlock } from './AiMessageList';
import AiSettingsDialog from './AiSettingsDialog';
import { deviceSession } from '../../utils/deviceSession';
import { loadAiCfg } from '../../utils/aiSettings';
import { getAiDocs } from '../../utils/aiDocs';
import {
    runAgentTurn, getSessionMessages, clearHistory, isAgentRunning,
    getLastPlan, getLastUsage, peekContextPlan,
} from '../../utils/aiAgent';
import { undoByTs, peekUndo } from '../../utils/aiTools';
import { formatTokens as fmtTokens } from '../../utils/aiContext';
import { shouldFollowAfterGrowth } from '../../utils/aiScroll';
import { Notice, PANEL_INK, PANEL_LINE, PANEL_MUTED, StatusPill } from '../PanelKit';

// 一行用量摘要：第几轮 + 本轮输入（含缓存命中拆分）+ 输出 + 任务累计。
// 口径刻意与服务端 Dashboard 对齐：**"累计"含缓存命中**（DeepSeek 的 prompt_tokens 就是这么
// 统计的），同时单独给出"非缓存输入"（缓存输入也可能收费）——否则用户会拿侧栏的小数字去对账单，
// 看到 Dashboard 上的大数字就以为助手在偷偷多花钱（真机实测踩到过这个观感问题）。
function usageLine(usage, round, maxRounds) {
    if (!usage) return '';
    const u = usage.usage || usage;              // 兼容 {usage} 与裸 usage
    const total = usage.total || usage.totalUsage;
    const parts = [];
    if (round) parts.push(`第 ${round}/${maxRounds} 轮`);
    parts.push(u.cacheReadTokens > 0
        ? `输入 ${fmtTokens(u.inputTokens)}（缓存命中 ${fmtTokens(u.cacheReadTokens)}）`
        : `输入 ${fmtTokens(u.inputTokens)}`);
    parts.push(`输出 ${usage.estimated ? '未知' : fmtTokens(u.outputTokens)}`);
    if (total) {
        const all = total.inputTokens + total.cacheReadTokens + total.outputTokens;
        const uncached = total.inputTokens;
        parts.push(`累计${usage.totalEstimated ? '≈' : ' ' }${fmtTokens(all)}（非缓存输入 ${fmtTokens(uncached)}）`);
    }
    if (usage.estimated) parts.push('输入估算，输出用量未知');
    return parts.join(' · ');
}

// 思考中动画：三个错相闪烁的圆点
const DOT_KEYFRAMES = {
    '@keyframes aiDotBlink': {
        '0%, 80%, 100%': { opacity: 0.15, transform: 'scale(0.85)' },
        '40%': { opacity: 1, transform: 'scale(1)' },
    },
};
function ThinkingDots() {
    return (
        <Stack direction="row" spacing={0.25} alignItems="center" sx={DOT_KEYFRAMES}>
            {[0, 1, 2].map((i) => (
                <Box key={i} sx={{
                    width: 5, height: 5, borderRadius: '50%', bgcolor: 'primary.main',
                    animation: 'aiDotBlink 1.2s infinite ease-in-out',
                    animationDelay: `${i * 0.2}s`,
                }} />
            ))}
        </Stack>
    );
}

// ── 流式文本的「前导 + 尾随」节流 ───────────────────────────────────────────
// 背景：DeepSeek 的增量帧又碎又快（一帧一两个字），逐帧 setState 会让整棵面板
// （含历史消息列表）每秒重渲染几十次，表现出来就是"思考过程刷得快但卡"。
// 这里的做法是：
//   · 距上次刷新已超过 intervalMs → 立刻刷（第一段马上出现，有即时反馈）；
//   · 否则把增量攒着，排到"上次刷新 + intervalMs"那一刻再刷（尾随）。
// 于是刷新频率被钳在 ~1/intervalMs，且**不会有定时器空转**。
// 单靠 100ms 的 setTimeout 是不够的：那只是把频率压到 10/s，
// 真正让界面卡的是每次刷新要做的事（见下面的滚动与折叠块实现）。
const STREAM_MIN_INTERVAL_MS = 120;
function useThrottledText(intervalMs = STREAM_MIN_INTERVAL_MS) {
    const [text, setText] = useState('');
    const bufRef = useRef('');
    const timerRef = useRef(null);
    const lastRef = useRef(0);

    const flush = useCallback(() => {
        if (timerRef.current) { clearTimeout(timerRef.current); timerRef.current = null; }
        if (!bufRef.current) return;
        const chunk = bufRef.current;
        bufRef.current = '';
        lastRef.current = Date.now();
        setText((t) => t + chunk);
    }, []);

    const push = useCallback((chunk) => {
        if (!chunk) return;
        bufRef.current += chunk;
        const since = Date.now() - lastRef.current;
        if (since >= intervalMs) flush();
        else if (!timerRef.current) timerRef.current = setTimeout(flush, intervalMs - since);
    }, [flush, intervalMs]);

    /** 丢掉未刷新的缓冲（内容作废时用，不改已有文本） */
    const drop = useCallback(() => {
        if (timerRef.current) { clearTimeout(timerRef.current); timerRef.current = null; }
        bufRef.current = '';
    }, []);

    /** 清空缓冲与已显示文本 */
    const reset = useCallback(() => {
        drop();
        lastRef.current = Date.now();
        setText('');
    }, [drop]);

    return { text, push, flush, drop, reset };
}

export default function AiChatPanel({ disabled = false, open, getScript, applyScript, onRunningChange, onUnread, onFoundLines }) {
    const [, force] = useState(0);
    const rerender = useCallback(() => force((n) => n + 1), []);
    const [input, setInput] = useState('');
    const [running, setRunning] = useState(false);
    useEffect(() => { onRunningChange?.(running); }, [running, onRunningChange]);
    const stream = useThrottledText();
    const reasoning = useThrottledText();
    const streamingText = stream.text;
    const reasoningText = reasoning.text;
    const streamingPreview = streamingText.length > 16000
        ? `[较早正文将在本轮结束后显示，这里显示最新片段]\n${streamingText.slice(-16000)}` : streamingText;
    const [toolStatus, setToolStatus] = useState('');
    const [roundInfo, setRoundInfo] = useState(null);
    const [activeModel, setActiveModel] = useState('');
    const [error, setError] = useState(null);       // {message, isAuth}
    const [settingsOpen, setSettingsOpen] = useState(false);
    const [undoMissed, setUndoMissed] = useState(false); // 撤销条目已随刷新失效提示
    // 成本可见性：本轮/累计 token（含缓存命中）+ 上下文占用 + 压缩提示。
    // 旧版把用量完全藏着，用户只能看着账单变快却不知道钱花在哪一步。
    const [usageInfo, setUsageInfo] = useState(() => getLastUsage());
    const [contextMeta, setContextMeta] = useState(() => getLastPlan());
    const [compactNote, setCompactNote] = useState('');
    // 推理模型的思考（运行中实时预览）：**只显示，不回传**——aiAgent 把它挂到
    // assistant._reasoning，组装请求时被 sanitizeMessage 剥掉，所以看不到也不花钱。
    // 文本由上面的 reasoning = useThrottledText() 提供（同样走前导+尾随节流）。
    const abortRef = useRef(null);
    useEffect(() => {
        const epoch = deviceSession.epoch;
        const stop = () => { if (deviceSession.epoch !== epoch || deviceSession.getSnapshot().state !== 'connected') abortRef.current?.abort(); };
        const unsubscribe = deviceSession.subscribe(stop);
        return () => { unsubscribe(); abortRef.current?.abort(); onRunningChange?.(false); };
    }, [onRunningChange]);
    const openRef = useRef(open);
    const scrollBoxRef = useRef(null);   // 消息列表滚动容器（自己控滚动，不用 scrollIntoView）
    openRef.current = open;

    const flushStream = stream.flush;
    const dropStream = stream.drop;

    const queueStreamDelta = stream.push;

    const flushReasoning = reasoning.flush;
    const queueReasoning = reasoning.push;
    // ── 滚动跟随 ───────────────────────────────────────────────────────────────
    // 三条纪律（前两条都是踩出来的）：
    //  ① **不要用 `scrollIntoView`**：它每次调用都强制整页重排，而原来那个 effect 没有依赖
    //     数组（每次渲染都跑，流式期间每秒十几次）——那是"刷得快但卡"的主因之一。
    //     改成对滚动容器做一次 `scrollTop = scrollHeight` 的廉价写。
    //  ② **判据只用 `shouldFollowAfterGrowth()`**（utils/aiScroll.js，有单测）：
    //     "这次内容变化**之前**用户是不是在底部"。**绝不能拿"离底距离"直接当"用户在看历史"**
    //     ——内容一长距离天然超阈值，会被误判从而**永久锁死**（实测列表卡在离底 719px；
    //     思考预览那处更狠，冻结在 658px 而内容还在长，表现就是"不能自动滚到最新"）。
    //     判据用**上一次的 scrollHeight** 算离底，所以"内容自己长高"骗不到它；也不监听
    //     scroll、不用意图事件——那版会被"回到底部就恢复"抢跑（实测滚轮上翻后又立刻被拽回）。
    //  ③ 触发时机要覆盖**所有内容变化**：消息条数（用户消息 / 工具卡片 / 助手回答 / 压缩
    //     通知）、两条流式文本、轮次开始与结束、面板展开。只依赖流式文本会漏掉"追加消息"这一类。
    const prevHRef = useRef(0);            // 上一次内容变化时的 scrollHeight
    const forceFollowRef = useRef(false);  // 用户主动发消息 → 强制恢复跟随一次

    const scrollToEnd = useCallback(() => {
        const el = scrollBoxRef.current;
        if (!el) return;
        const force = forceFollowRef.current;
        forceFollowRef.current = false;
        const follow = force || shouldFollowAfterGrowth({
            prevH: prevHRef.current, top: el.scrollTop, clientH: el.clientHeight,
        });
        prevHRef.current = el.scrollHeight;
        if (follow) el.scrollTop = el.scrollHeight;
    }, []);

    const cfg = loadAiCfg();
    const hasKey = !!cfg.apiKey;
    // 文档随面板内置（构建时写入），正常构建永远为 false；为 true 说明这次构建没拿到文档
    const docsMissing = getAiDocs().source === 'none';

    const handleSend = async () => {
        const text = input.trim();
        if (!text || running || disabled) return;
        if (!hasKey) { setSettingsOpen(true); return; }
        const taskEpoch = deviceSession.epoch;
        try { getScript(); } catch { return; }
        setInput('');
        setError(null);
        forceFollowRef.current = true;   // 用户主动发消息 → 强制恢复跟随最新
        stream.reset();
        reasoning.reset();
        setToolStatus('');
        setRoundInfo(null);
        setActiveModel(cfg.model);
        setUsageInfo(null);
        setRunning(true);
        abortRef.current = new AbortController();
        const res = await runAgentTurn(text, { getScript, applyScript }, {
            cfg,
            signal: abortRef.current.signal,
            onEvent: (ev) => {
                if (deviceSession.epoch !== taskEpoch || !deviceSession.device.connected) return;
                switch (ev.type) {
                    case 'round_start':
                        stream.reset();
                        reasoning.reset();
                        setRoundInfo({ round: ev.round, maxRounds: ev.maxRounds, summarizing: ev.summarizing });
                        setContextMeta(ev.plan);
                        break;
                    case 'text_delta':
                        queueStreamDelta(ev.content);
                        break;
                    case 'tool_start':
                        // 本轮文本已整体入历史，缓冲作废
                        stream.reset();
                        setToolStatus(`正在执行 ${ev.call.function?.name || ''}…`);
                        rerender();
                        break;
                    case 'tool_result':
                        flushStream();
                        setToolStatus('');
                        // 编辑成功 → 通知父组件滚动定位到改动行
                        if (onFoundLines && ev.result && Array.isArray(ev.result.foundAtLines) && ev.result.foundAtLines.length) {
                            onFoundLines(ev.result.foundAtLines);
                        }
                        rerender();
                        break;
                    case 'done':
                        flushStream();
                        break;
                    case 'usage':
                        // 每轮都报：输入/输出/缓存命中（DeepSeek 会单列 prompt_cache_hit_tokens）
                        setUsageInfo({ round: ev.round, maxRounds: ev.maxRounds, usage: ev.usage, total: ev.totalUsage, estimated: ev.estimated, totalEstimated: ev.totalEstimated });
                        setContextMeta(ev.plan);
                        rerender();
                        break;
                    case 'reasoning':
                        queueReasoning(ev.content || '');
                        break;
                    case 'compacted':
                        setCompactNote(`已压缩上下文：省略 ${ev.droppedBlocks} 段较早记录或缩减工具结果（控制 token 消耗）`);
                        rerender();
                        break;
                    case 'error':
                        flushStream();
                        setError({
                            message: ev.err.message || String(ev.err),
                            isAuth: ev.err.kind === 'auth',
                        });
                        break;
                    default: break;
                }
            },
        });
        if (deviceSession.epoch !== taskEpoch || !deviceSession.device.connected) return;
        // 收尾：中断或出错时把本轮未完成的流式文本丢弃，刷新历史
        stream.reset();
        setToolStatus('');
        setRunning(false);
        // 成功后思考过程已经进了历史（由消息列表渲染），清掉运行中的临时副本；
        // 失败时留着——排查"为什么没答出来"时，那段思考往往就是唯一线索。
        if (res && res.ok) reasoning.reset();
        else flushReasoning();
        rerender();
        if (openRef.current) requestAnimationFrame(scrollToEnd);
        if (!openRef.current && res && (res.ok || res.aborted)) {
            // 面板折叠期间完成 → 通知父组件亮未读标记
            if (onUnread) onUnread(true);
        }
    };

    const handleStop = () => {
        if (abortRef.current) abortRef.current.abort();
    };

    const applyScriptRef = useRef(applyScript);
    applyScriptRef.current = applyScript;
    const handleUndo = useCallback((ts) => {
        if (isAgentRunning() || disabled) return;
        const entry = undoByTs(ts);
        if (entry) {
            applyScriptRef.current(entry.before);
            rerender();
        } else {
            // 历史从 localStorage 恢复后，内存撤销栈里没有对应条目（页面刷新过）
            setUndoMissed(true);
            setTimeout(() => setUndoMissed(false), 4000);
        }
    }, [rerender, disabled]);

    const handleClear = () => {
        if (!running) rerender();
        clearHistory();
        setUsageInfo(null);
        setCompactNote('');
        setContextMeta(peekContextPlan(cfg));
        rerender();
    };

    const messages = getSessionMessages();
    const undoEntry = peekUndo();
    // 面板展开/收起时把"上次高度"清零：收起期间列表尺寸变了，旧高度算出来的离底没意义，
    // 清零等价于"当作第一次内容变化，直接跟随最新"。
    useEffect(() => {
        prevHRef.current = 0;
        forceFollowRef.current = true;
    }, [open]);
    useEffect(() => {
        if (!open) return;
        const id = requestAnimationFrame(() => scrollToEnd());
        return () => cancelAnimationFrame(id);
    }, [open, streamingText, reasoningText, messages.length, running, scrollToEnd]);
    // 上下文占用：优先用本次任务实测的 meta；没有就按当前历史估算一次。
    // 必须 memo——它要遍历整份历史做 token 估算，而流式期间每 100ms 就重渲染一次。
    const fallbackContext = useMemo(() => {
        try { return peekContextPlan(cfg); } catch (e) { return null; }
    }, [cfg.contextBudgetTokens, cfg.requestBudgetTokens, cfg.maxTokens, messages, messages.length, running]);
    const liveContext = running ? (contextMeta || fallbackContext) : fallbackContext;
    const usageText = running || usageInfo ? usageLine(usageInfo, usageInfo && usageInfo.round, usageInfo && usageInfo.maxRounds) : '';
    const ctxPct = liveContext && liveContext.budgetTokens
        ? Math.min(100, Math.round((liveContext.estimatedWindowTokens / liveContext.budgetTokens) * 100)) : 0;
    const requestPct = liveContext && liveContext.requestBudgetTokens
        ? Math.min(100, Math.round((liveContext.estimatedTokens / liveContext.requestBudgetTokens) * 100)) : 0;
    // 默认压缩线跟随模型窗口；用户手动设置更小的单轮上限时，以该上限为准。
    const nearLimit = !!liveContext && liveContext.estimatedTokens >= liveContext.compactionWater;
    const requestLimited = !!liveContext && liveContext.requestBudgetTokens < liveContext.budgetTokens;

    return (
        <Stack sx={{ height: '100%', minHeight: 0 }}>
            {/* 顶栏 */}
            <Stack direction="row" spacing={0.5} alignItems="center" sx={{
                px: 1, py: 0.5, flexShrink: 0,
                borderBottom: `1px solid ${PANEL_LINE}`, bgcolor: 'var(--panel-surface)',
            }}>
                <Typography sx={{ fontSize: 13, fontWeight: 600, color: PANEL_INK }}>AI 助手</Typography>
                <Tooltip title={`当前模型：${running ? activeModel : cfg.model}；声明窗口 ${fmtTokens(liveContext?.budgetTokens || cfg.contextBudgetTokens)}，每轮输入上限 ${fmtTokens(liveContext?.requestBudgetTokens || cfg.requestBudgetTokens)}`}>
                    <Typography sx={{ fontSize: 10.5, color: PANEL_MUTED, maxWidth: 140, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {running ? activeModel : cfg.model}
                    </Typography>
                </Tooltip>
                {running ? <StatusPill tone="accent">{roundInfo ? `${roundInfo.summarizing ? '总结' : '第'} ${roundInfo.round}/${roundInfo.maxRounds} 轮` : '准备中'}</StatusPill> : null}
                <Box sx={{ flex: 1 }} />
                <Tooltip title="撤销最近一次 AI 编辑">
                    <span>
                        <IconButton size="small" onClick={() => handleUndo(undoEntry.ts)} disabled={disabled || !undoEntry || running}>
                            <UndoIcon sx={{ fontSize: 18 }} />
                        </IconButton>
                    </span>
                </Tooltip>
                {/* 清空对话 = 成本归零最快的动作，所以给它文字按钮而不是一个不起眼的图标 */}
                <Tooltip title="清空对话：整段历史删掉，之后每次请求都从最小上下文重新开始（最省钱的操作）">
                    <span>
                        <Button size="small" onClick={handleClear} disabled={disabled || running}
                            startIcon={<DeleteSweepIcon sx={{ fontSize: 15 }} />}
                            sx={{
                                textTransform: 'none', minWidth: 0, px: 0.75, fontSize: 12,
                                color: nearLimit ? 'var(--panel-danger)' : PANEL_MUTED,
                            }}>
                            清空
                        </Button>
                    </span>
                </Tooltip>
                <Tooltip title="设置">
                    <IconButton size="small" disabled={disabled || running} onClick={() => setSettingsOpen(true)}>
                        <SettingsIcon sx={{ fontSize: 18 }} />
                    </IconButton>
                </Tooltip>
            </Stack>

            {/* 用量条：上下文占用 + 本轮/累计 token（含缓存命中）。
                这一行是"让用户看得见钱花在哪"的最小实现——旧版把 usage 完全丢掉。 */}
            {hasKey && (
                <Stack direction="row" spacing={1} alignItems="center" sx={{
                    px: 1, py: 0.25, flexShrink: 0,
                    borderBottom: `1px solid ${PANEL_LINE}`, bgcolor: 'var(--panel-sunken)',
                }}>
                    <Typography sx={{ fontSize: 10.5, color: nearLimit ? 'var(--panel-danger)' : PANEL_MUTED, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>
                        {liveContext
                            ? `上下文 ≈${fmtTokens(liveContext.estimatedTokens)}/${fmtTokens(liveContext.budgetTokens)}（窗口 ${ctxPct}%${requestLimited ? ` · 本轮 ${requestPct}%` : ''}）`
                            : ''}
                    </Typography>
                    <Tooltip title={usageText || '端点未返回用量时显示输入估算；费用请以供应商账单为准'}>
                    <Typography sx={{ fontSize: 10.5, color: PANEL_MUTED, fontVariantNumeric: 'tabular-nums', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {usageText}
                    </Typography>
                    </Tooltip>
                </Stack>
            )}

            {/* 未配置引导 */}
            {!hasKey ? (
                <Box sx={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1, p: 2 }}>
                    <Typography sx={{ fontSize: 12, color: PANEL_MUTED }}>尚未配置 AI API</Typography>
                    <Button variant="contained" disableElevation size="small"
                        onClick={() => setSettingsOpen(true)} sx={{ textTransform: 'none' }}>前往设置</Button>
                </Box>
            ) : (
                <React.Fragment>
                    {/* 撤销条目失效轻提示（历史刷新恢复后撤销栈为空） */}
                    {undoMissed && (
                        <Typography sx={{ fontSize: 11, color: PANEL_MUTED, px: 1, pt: 0.5 }}>
                            该编辑的撤销信息已随页面刷新失效
                        </Typography>
                    )}
                    {/* 文档未内置轻提示（正常构建不会出现；说明这次构建没拿到文档源） */}
                    {docsMissing && (
                        <Typography sx={{ fontSize: 11, color: PANEL_MUTED, px: 1, pt: 0.5 }}>
                            面板未内置 Lua API 文档，AI 可能不了解固件函数；请重新构建面板
                        </Typography>
                    )}
                    {/* 压缩提示：解释"早先的工具卡片为什么消失了" */}
                    {(compactNote || messages.some((m) => m._compactNotice)) && (
                        <Typography sx={{ fontSize: 11, color: PANEL_MUTED, px: 1, pt: 0.5 }}>
                            {compactNote || '本会话已压缩过；当前目标保留，需要的旧代码可按区间重新读取'}
                        </Typography>
                    )}
                    {/* 上下文接近上限：此刻"清空对话"是最省钱的建议，明说出来 */}
                    {nearLimit && (
                        <Notice tone="warn" sx={{ mx: 1, mt: 0.5 }} action={
                            <Button size="small" onClick={handleClear} disabled={disabled || running}
                                sx={{ textTransform: 'none', color: 'var(--panel-danger)', minWidth: 0 }}>清空</Button>
                        }>
                            {requestLimited
                                ? `本轮输入已达到 ${fmtTokens(liveContext.compactionWater)} 警戒线，接下来会自动压缩较早的记录。`
                                : '上下文已达到 80% 警戒线，接下来会自动压缩较早的记录。'}
                            {' '}开新对话比接着聊更省钱。
                        </Notice>
                    )}
                    {/* 消息列表 */}
                    {/* data-ai-scroll：给联调/自动化测试一个稳定的定位锚（否则小视口下
                        多个小容器都会溢出，脚本很容易量到别的元素上去——真踩过） */}
                    <Box ref={scrollBoxRef}
                        data-ai-scroll="list"
                        sx={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
                        {messages.length === 0 && !streamingText && (
                            <Box sx={{ p: 2, textAlign: 'center' }}>
                                <Typography sx={{ fontSize: 11.5, color: PANEL_MUTED, lineHeight: 1.8 }}>
                                    描述你想要的脚本功能或改动，例如：<br />「让 A 键按下时触屏连点」<br />「检查脚本为什么没有输出日志」
                                </Typography>
                            </Box>
                        )}
                        {/* rev 让 memo 能识别"历史真的变了"：session.messages 是原地 push 的，
                            只比引用会让消息列表永远不更新 */}
                        <AiMessageList messages={messages} rev={messages.length} onUndo={running || disabled ? undefined : handleUndo} />
                        {/* 思考中动画 + 阶段状态（流式文本直接跟在下方） */}
                        {running && (
                            <Box sx={{ mx: 1, mb: 1 }}>
                                <Stack direction="row" spacing={1} alignItems="center" sx={{ py: 0.5 }}>
                                    <ThinkingDots />
                                    <Typography sx={{ fontSize: 11, color: PANEL_MUTED, flex: 1 }}>
                                        {toolStatus || (streamingText ? '正在回复…'
                                            : (reasoningText.length > 0 ? `模型思考中…（已思考 ${reasoningText.length} 字）` : '思考中…'))}
                                    </Typography>
                                </Stack>
                                {reasoningText && (
                                    // selectable：实时思维链同样要能选中复制（历史里的那份在
                                    // AiMessageList 里，已带 selectable；这里是运行中的副本）
                                    <Box className="selectable" sx={{ mt: 0.5 }}>
                                        <ReasoningBlock text={reasoningText} live />
                                    </Box>
                                )}
                                {streamingText && (
                                    <Box sx={{ mt: 0.5, mx: -1 }}>
                                        <AiMessageList messages={[{ role: 'assistant', content: streamingPreview }]} />
                                    </Box>
                                )}
                            </Box>
                        )}
                    </Box>
                    {running && <LinearProgress sx={{ height: 2 }} />}

                    {/* 错误提示：报错原文要能复制去排查（全站默认不可选，故显式放行） */}
                    {error && (
                        <Notice tone="warn" className="selectable" sx={{ mx: 1, my: 0.5 }} action={
                            <Box sx={{ display: 'flex', gap: 0.5, flexShrink: 0 }}>
                                {error.isAuth && (
                                    <Button size="small" disabled={disabled || running} onClick={() => setSettingsOpen(true)}
                                        sx={{ textTransform: 'none', color: 'var(--panel-danger)', minWidth: 0 }}>设置</Button>
                                )}
                                <Button size="small" onClick={() => setError(null)}
                                    sx={{ textTransform: 'none', color: 'var(--panel-danger)', minWidth: 0 }}>知道了</Button>
                            </Box>
                        }>
                            {error.message}
                        </Notice>
                    )}

                    {/* 输入区 */}
                    <Stack direction="row" spacing={0.5} alignItems="flex-end" sx={{ p: 1, flexShrink: 0, borderTop: `1px solid ${PANEL_LINE}` }}>
                        <TextField
                            size="small" fullWidth multiline maxRows={4}
                            placeholder="向 AI 描述脚本需求…"
                            value={input}
                            onChange={(e) => setInput(e.target.value)}
                            onKeyDown={(e) => {
                                if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
                                    e.preventDefault();
                                    handleSend();
                                }
                            }}
                            disabled={disabled && !running}
                        />
                        {running ? (
                            <IconButton color="error" onClick={handleStop} title="停止">
                                <StopIcon />
                            </IconButton>
                        ) : (
                            <IconButton color="primary" onClick={handleSend} disabled={disabled || !input.trim()} title="发送（Enter 发送，Shift+Enter 换行）">
                                <SendIcon />
                            </IconButton>
                        )}
                    </Stack>
                </React.Fragment>
            )}

            <AiSettingsDialog open={settingsOpen} onClose={() => setSettingsOpen(false)} />
        </Stack>
    );
}
