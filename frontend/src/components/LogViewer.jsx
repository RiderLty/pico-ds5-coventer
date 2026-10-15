import React, { useEffect, useRef, useState } from 'react';
import { Box, Stack, Typography, IconButton, Tooltip } from '@mui/material';
import { PANEL_INK, PANEL_LINE, PANEL_MUTED, StatusPill } from './PanelKit';
import DeleteSweepIcon from '@mui/icons-material/DeleteSweep';
import FileDownloadIcon from '@mui/icons-material/FileDownload';
import KeyboardArrowDownIcon from '@mui/icons-material/KeyboardArrowDown';
import KeyboardArrowUpIcon from '@mui/icons-material/KeyboardArrowUp';
import OpenInFullIcon from '@mui/icons-material/OpenInFull';

// 虚拟滚动：固定行高（12px 字号），只渲染可视区 ±OVERSCAN 行，
// 上方以垫片占位——数千行日志也只挂 ~40 个 DOM 节点。
const ROW_H = 19.2;    // px；与 fontSize 12 单行严格对应
const OVERSCAN = 10;

// 日志显示组件（纯展示 + 虚拟滚动）
// props:
//   logs      string[]                 日志数组（每项渲染为一行，长行横向滚动不折行）
//   filter    (line, i, arr) => bool   可选过滤函数，缺省显示全部
//   title     string                   面板标题
//   onClear   () => void               提供则显示清空按钮
//   onOpenFull () => void              提供则在清空按钮右侧显示「全屏日志面板」按钮
//                                      （打开 #LOG 全屏日志页，与悬浮面板顶栏同一入口）
//   onExport  () => void               提供则显示导出按钮（位于清空按钮左侧，导出过滤后的日志行）
//   collapsed bool                       收起态：隐藏日志列表仅保留头部
//   onToggleCollapsed () => void         提供则在头部最右侧显示收起/展开按钮
//   emptyText string                   空态占位文本
//   sx        object                   外层容器附加样式（供父级控制布局/比例）
export default function LogViewer({
    logs = [],
    filter,
    title = '设备日志',
    onClear,
    onOpenFull,
    onExport,
    collapsed,
    onToggleCollapsed,
    emptyText = '暂无日志',
    sx,
    headerLeft,
    headerBelow,
}) {
    const scrollRef = useRef(null);
    const stickRef = useRef(true);     // 是否吸附底部：用户上滚查看历史时暂停自动滚动
    const lastTopRef = useRef(0);
    const winRef = useRef({ s: 0, e: 0 });
    const [, setWin] = useState({ s: 0, e: 0 });

    const lines = filter ? logs.filter(filter) : logs;

    // 由 scrollTop/容器高度计算可视窗口；窗口无变化时不触发重渲染
    const computeWin = () => {
        const el = scrollRef.current;
        if (!el) return;
        const s = Math.max(0, Math.floor(el.scrollTop / ROW_H) - OVERSCAN);
        const e = Math.min(lines.length, Math.ceil((el.scrollTop + el.clientHeight) / ROW_H) + OVERSCAN);
        if (s !== winRef.current.s || e !== winRef.current.e) {
            winRef.current = { s, e };
            setWin(winRef.current);
        }
    };

    const onScroll = () => {
        const el = scrollRef.current;
        if (!el) return;
        const st = el.scrollTop;
        // 吸附判定按滚动方向：到底部 → 吸附；向上滚（拉历史）→ 解除。
        // 不能用「距底部 < N px」判定——那会把刚开始缓慢拖离底部的触摸也当成
        // 贴底，每次窗口更新都被拉回底部，触屏上表现为反复抽动。
        if (el.scrollHeight - st - el.clientHeight <= 2) stickRef.current = true;
        else if (st < lastTopRef.current - 1) stickRef.current = false;
        lastTopRef.current = st;
        computeWin();
    };

    // 每次渲染后：吸附底部 + 按最新几何重算窗口（窗口无变化时自然收敛，无死循环）
    const winFnRef = useRef(null);
    winFnRef.current = () => {
        const el = scrollRef.current;
        if (!el) return;
        if (stickRef.current) el.scrollTop = el.scrollHeight;
        computeWin();
    };
    useEffect(() => { winFnRef.current(); });

    // 旋转/分栏尺寸变化时重算窗口
    useEffect(() => {
        const onR = () => winFnRef.current();
        window.addEventListener('resize', onR);
        return () => window.removeEventListener('resize', onR);
    }, []);

    const s = Math.min(winRef.current.s, lines.length);
    const e = Math.min(winRef.current.e, lines.length);

    return (
        <Box sx={{ display: 'flex', flexDirection: 'column', minHeight: 0, ...sx }}>
            {/* 头部：左侧由父级给（脚本大小等），右侧标题 + 行数药丸 + 图标按钮；与面板同一套字号/间距 */}
            <Stack direction="row" alignItems="center" spacing={0.75} sx={{ flexShrink: 0, mb: 0.5, minHeight: 26 }}>
                {headerLeft}
                <Box sx={{ flexGrow: 1 }} />
                {title ? <Typography sx={{ fontSize: 11.5, color: PANEL_MUTED }}>{title}</Typography> : null}
                <StatusPill>{lines.length}</StatusPill>
                {onExport && (
                    <Tooltip title="导出日志">
                        <span>
                            <IconButton size="small" onClick={onExport} disabled={lines.length === 0}>
                                <FileDownloadIcon fontSize="small" />
                            </IconButton>
                        </span>
                    </Tooltip>
                )}
                {onClear && (
                    <Tooltip title="清空日志">
                        <span>
                            <IconButton size="small" onClick={onClear} disabled={logs.length === 0}>
                                <DeleteSweepIcon fontSize="small" />
                            </IconButton>
                        </span>
                    </Tooltip>
                )}
                {onOpenFull && (
                    <Tooltip title="全屏日志面板">
                        <IconButton size="small" onClick={onOpenFull}>
                            <OpenInFullIcon fontSize="small" />
                        </IconButton>
                    </Tooltip>
                )}
                {onToggleCollapsed && (
                    <Tooltip title={collapsed ? '展开日志面板' : '收起日志面板'}>
                        <IconButton size="small" onClick={onToggleCollapsed}>
                            {collapsed ? <KeyboardArrowUpIcon fontSize="small" /> : <KeyboardArrowDownIcon fontSize="small" />}
                        </IconButton>
                    </Tooltip>
                )}
            </Stack>
            {headerBelow && <Box sx={{ flexShrink: 0 }}>{headerBelow}</Box>}
            {!collapsed && (
            <Box
                ref={scrollRef}
                onScroll={onScroll}
                // selectable：日志要能选中 / 复制（全站默认不可选，见 index.css 的「文字选择策略」）。
                // 注意列表是虚拟滚动（只挂载可视 ±OVERSCAN 行），跨行拖选只能选到已挂载的部分，
                // 要整段日志请走「导出」。
                className="selectable"
                sx={{
                    flexGrow: 1,
                    minHeight: 0,
                    overflow: 'auto',
                    WebkitOverflowScrolling: 'touch',
                    overscrollBehavior: 'contain',
                    px: 1,                  // 垂直方向零内边距：scrollTop 与行序号严格对应
                    border: `1px solid ${PANEL_LINE}`,
                    borderRadius: '8px',
                    // 浅底控制台：与面板同底色（深色大色块在浅色弹窗里像外来元素），
                    // 等宽字体 + 固定行高仍然保证可读与对齐
                    bgcolor: 'var(--panel-content)',
                    color: PANEL_INK,
                    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
                    fontSize: 12,
                }}
            >
                {lines.length === 0
                    ? <Box sx={{ color: PANEL_MUTED, fontStyle: 'italic', p: 1 }}>{emptyText}</Box>
                    : (
                        <>
                            <Box sx={{ height: s * ROW_H }} />
                            {lines.slice(s, e).map((line, k) => (
                                <Box key={s + k} sx={{
                                    height: ROW_H,
                                    lineHeight: `${ROW_H}px`,
                                    whiteSpace: 'pre',      // 不折行：固定行高 + 横向滚动
                                }}>{line}</Box>
                            ))}
                            <Box sx={{ height: (lines.length - e) * ROW_H }} />
                        </>
                    )}
            </Box>
            )}
        </Box>
    );
}
