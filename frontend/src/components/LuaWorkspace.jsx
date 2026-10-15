import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Box, Button, IconButton, LinearProgress, Menu, MenuItem, Stack, TextField, Typography } from '@mui/material';
import CodeIcon from '@mui/icons-material/Code';
import PlayArrowIcon from '@mui/icons-material/PlayArrow';
import SaveIcon from '@mui/icons-material/Save';
import MoreHorizIcon from '@mui/icons-material/MoreHoriz';
import ContrastIcon from '@mui/icons-material/Contrast';
import ViewSidebarIcon from '@mui/icons-material/ViewSidebar';
import FolderOpenIcon from '@mui/icons-material/FolderOpen';
import CodeEditor from './CodeEditor';
import LogViewer from './LogViewer';
import { LuaScriptManager, LuaExportDialog } from './LuaScriptDialogs';
import AiChatPanel from './ai/AiChatPanel';
import { DialogShell, StatusPill, PANEL_LINE } from './PanelKit';
import { deviceSession, SCRIPT_MAX_BYTES } from '../utils/deviceSession';
import { WorkspaceModel } from '../utils/workspaceModel';
import { clearLogs, exportLogLines, usePicoLogs } from '../utils/logStore';
import { THEME_LABELS, useThemeMode } from '../utils/themeStore';
import '../utils/deviceLogs';
import defaultSource from '../../../scripts/default.lua?raw';

function useStore(store) {
    const [state, setState] = useState(store.getSnapshot);
    useEffect(() => store.subscribe(() => setState(store.getSnapshot())), [store]);
    return state;
}

// ── 日志面板高度：可拖动分割条 + 浏览器记忆 ──────────────────────────────
// 编辑器与日志面板之间加一条可上下拖动的分割条；松手后高度存 localStorage，
// 下次打开页面自动恢复。双击分割条回到默认高度（22vh，钳制在 110~240px）。
const LOG_HEIGHT_KEY = 'ds5_log_panel_height_v1';
const LOG_HEIGHT_MIN = 60;

function defaultLogHeight() {
    const vh = typeof window !== 'undefined' ? window.innerHeight : 800;
    return Math.max(110, Math.min(240, Math.round(vh * 0.22)));
}

function useLogPanelHeight(collapsed) {
    const [height, setHeight] = useState(() => {
        try {
            const stored = parseInt(localStorage.getItem(LOG_HEIGHT_KEY), 10);
            if (Number.isFinite(stored) && stored >= LOG_HEIGHT_MIN) {
                // 存的值可能来自更大的屏幕，加载时按当前视口钳一下上限
                const maxH = (window.innerHeight || 800) - 200;
                return Math.min(stored, maxH);
            }
        } catch { /* 隐私模式 / storage 不可用 */ }
        return defaultLogHeight();
    });
    const [dragging, setDragging] = useState(false);
    const heightRef = useRef(height);
    heightRef.current = height;

    const startDrag = useCallback((e) => {
        if (collapsed) return;
        e.preventDefault();
        const startY = e.clientY;
        const startH = heightRef.current;
        setDragging(true);
        // is-dragging：全站禁选文字（index.css 里有 !important 规则），防止拖动时把编辑器/日志刷蓝
        document.body.classList.add('is-dragging');

        const onMove = (ev) => {
            const delta = startY - ev.clientY;  // 向上拖 → delta 正 → 增高
            const maxH = (window.innerHeight || 800) - 200;  // 给编辑器至少留 200px
            setHeight(Math.max(LOG_HEIGHT_MIN, Math.min(maxH, startH + delta)));
        };
        const onUp = () => {
            setDragging(false);
            document.body.classList.remove('is-dragging');
            document.removeEventListener('pointermove', onMove);
            document.removeEventListener('pointerup', onUp);
            try { localStorage.setItem(LOG_HEIGHT_KEY, String(heightRef.current)); } catch { /* 存不下就只在本次会话生效 */ }
        };
        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
    }, [collapsed]);

    const reset = useCallback(() => {
        setHeight(defaultLogHeight());
        try { localStorage.removeItem(LOG_HEIGHT_KEY); } catch { /* ignore */ }
    }, []);

    return { height, dragging, startDrag, reset };
}

function LogDragHandle({ onStart, onReset, dragging }) {
    return (
        <Box
            onPointerDown={onStart}
            onDoubleClick={onReset}
            role="separator"
            aria-orientation="horizontal"
            aria-label="拖动调整日志面板高度（双击重置）"
            sx={{
                flexShrink: 0,
                height: 7,
                cursor: 'row-resize',
                touchAction: 'none',   // 防止触摸时浏览器把手势当滚动
                bgcolor: dragging ? 'var(--panel-accent-bg-hover)' : 'transparent',
                '&:hover': { bgcolor: 'var(--panel-accent-bg)' },
                position: 'relative',
                '&::after': {
                    content: '""',
                    position: 'absolute',
                    top: '50%', left: '50%',
                    transform: 'translate(-50%, -50%)',
                    width: 36, height: 3,
                    borderRadius: 2,
                    bgcolor: 'var(--panel-icon)',
                },
                '&:hover::after': { bgcolor: 'var(--panel-icon-hover)' },
            }}
        />
    );
}

// ── 左侧面板：可拖动宽度 + 展开状态，均存浏览器 ──────────────────────────
// 仿 VS Code 的左侧栏：编辑区左侧一条可左右拖动的面板，承载「脚本管理」等区段。
// 展开状态（open）与宽度（width）都写 localStorage，下次打开页面自动恢复。
// 双击分割条回到默认宽度。向右拖 → 面板变宽。
const LEFT_WIDTH_KEY = 'ds5_left_panel_width_v1';
const LEFT_OPEN_KEY = 'ds5_left_panel_open_v1';
const LEFT_WIDTH_MIN = 200;

function defaultLeftWidth() {
    return 300;
}

function maxLeftWidth() {
    const vw = typeof window !== 'undefined' ? window.innerWidth : 1200;
    // 上限 640px，且至少给编辑区 / AI 面板留 480px
    return Math.max(LEFT_WIDTH_MIN, Math.min(640, vw - 480));
}

function useLeftPanel() {
    const [open, setOpen] = useState(() => {
        try { return localStorage.getItem(LEFT_OPEN_KEY) === '1'; } catch { return false; }
    });
    const [width, setWidth] = useState(() => {
        try {
            const stored = parseInt(localStorage.getItem(LEFT_WIDTH_KEY), 10);
            if (Number.isFinite(stored) && stored >= LEFT_WIDTH_MIN) {
                return Math.min(stored, maxLeftWidth());
            }
        } catch { /* 隐私模式 / storage 不可用 */ }
        return defaultLeftWidth();
    });
    const [dragging, setDragging] = useState(false);
    const widthRef = useRef(width);
    widthRef.current = width;

    // 展开状态持久化（首帧也会写一次，把缺省值固化下来）
    useEffect(() => {
        try { localStorage.setItem(LEFT_OPEN_KEY, open ? '1' : '0'); } catch { /* ignore */ }
    }, [open]);

    // 视口变化时把宽度重新钳进合法区间
    useEffect(() => {
        const onResize = () => setWidth(w => Math.max(LEFT_WIDTH_MIN, Math.min(maxLeftWidth(), w)));
        window.addEventListener('resize', onResize);
        return () => window.removeEventListener('resize', onResize);
    }, []);

    const startDrag = useCallback((e) => {
        e.preventDefault();
        const startX = e.clientX;
        const startW = widthRef.current;
        setDragging(true);
        document.body.classList.add('is-dragging');

        const onMove = (ev) => {
            const delta = ev.clientX - startX;   // 向右拖 → delta 正 → 变宽
            setWidth(Math.max(LEFT_WIDTH_MIN, Math.min(maxLeftWidth(), startW + delta)));
        };
        const onUp = () => {
            setDragging(false);
            document.body.classList.remove('is-dragging');
            document.removeEventListener('pointermove', onMove);
            document.removeEventListener('pointerup', onUp);
            try { localStorage.setItem(LEFT_WIDTH_KEY, String(widthRef.current)); } catch { /* ignore */ }
        };
        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
    }, []);

    const reset = useCallback(() => {
        setWidth(defaultLeftWidth());
        try { localStorage.removeItem(LEFT_WIDTH_KEY); } catch { /* ignore */ }
    }, []);

    return { open, setOpen, width, dragging, startDrag, reset };
}

// 左侧面板（仿 VS Code）：区段标题 + 内容区。目前只有一个区段「脚本管理」。
// 后续要加区段，在标题栏下挂一组 tab、或按区段渲染不同内容即可。
function LeftPanel({ width, onApplyFromFile, onEdit, onApply }) {
    return (
        <Box component="aside" className="workspace-left" style={{ '--left-width': `${width}px` }}>
            <Box className="workspace-left-header">
                <FolderOpenIcon sx={{ fontSize: 16, color: 'var(--panel-accent)' }} />
                <Typography sx={{ fontSize: 12, fontWeight: 600, color: 'var(--panel-ink)' }}>脚本管理</Typography>
            </Box>
            <Box className="workspace-left-body">
                <LuaScriptManager onApplyFromFile={onApplyFromFile} onEdit={onEdit} onApply={onApply} />
            </Box>
        </Box>
    );
}

// ── AI 面板宽度：可拖动分割条 + 浏览器记忆 ──────────────────────────────
// 编辑区与 AI 面板之间加一条可左右拖动的分割条；松手后宽度存 localStorage，
// 下次打开页面自动恢复。双击分割条回到默认宽度（35vw，钳制在 360~600px）。
// 与日志面板高度同构，只是轴向相反：向左拖 → AI 面板变宽。
const AI_WIDTH_KEY = 'ds5_ai_panel_width_v1';
const AI_WIDTH_MIN = 300;

function defaultAiWidth() {
    const vw = typeof window !== 'undefined' ? window.innerWidth : 1200;
    return Math.max(360, Math.min(600, Math.round(vw * 0.35)));
}

function maxAiWidth() {
    const vw = typeof window !== 'undefined' ? window.innerWidth : 1200;
    return Math.max(AI_WIDTH_MIN, vw - 320);   // 给编辑器至少留 320px
}

function useAiPanelWidth(open) {
    const [width, setWidth] = useState(() => {
        try {
            const stored = parseInt(localStorage.getItem(AI_WIDTH_KEY), 10);
            if (Number.isFinite(stored) && stored >= AI_WIDTH_MIN) {
                // 存的值可能来自更宽的屏幕，加载时按当前视口钳一下上限
                return Math.min(stored, maxAiWidth());
            }
        } catch { /* 隐私模式 / storage 不可用 */ }
        return defaultAiWidth();
    });
    const [dragging, setDragging] = useState(false);
    const widthRef = useRef(width);
    widthRef.current = width;

    // 视口变化时把宽度重新钳进合法区间（宽度比高度更怕越界，会把编辑器挤没）
    useEffect(() => {
        const onResize = () => setWidth(w => Math.max(AI_WIDTH_MIN, Math.min(maxAiWidth(), w)));
        window.addEventListener('resize', onResize);
        return () => window.removeEventListener('resize', onResize);
    }, []);

    const startDrag = useCallback((e) => {
        if (!open) return;
        e.preventDefault();
        const startX = e.clientX;
        const startW = widthRef.current;
        setDragging(true);
        // is-dragging：全站禁选文字（index.css 里有 !important 规则），防止拖动时把编辑器/对话刷蓝
        document.body.classList.add('is-dragging');

        const onMove = (ev) => {
            const delta = startX - ev.clientX;   // 向左拖 → delta 正 → 变宽
            setWidth(Math.max(AI_WIDTH_MIN, Math.min(maxAiWidth(), startW + delta)));
        };
        const onUp = () => {
            setDragging(false);
            document.body.classList.remove('is-dragging');
            document.removeEventListener('pointermove', onMove);
            document.removeEventListener('pointerup', onUp);
            try { localStorage.setItem(AI_WIDTH_KEY, String(widthRef.current)); } catch { /* 存不下就只在本次会话生效 */ }
        };
        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
    }, [open]);

    const reset = useCallback(() => {
        setWidth(defaultAiWidth());
        try { localStorage.removeItem(AI_WIDTH_KEY); } catch { /* ignore */ }
    }, []);

    return { width, dragging, startDrag, reset };
}

// 竖直分割条（左右拖动）：左侧面板与 AI 面板共用。
// 7px 宽的透明条，中央一枚 3×36 的圆角 pill；双击回到默认宽度。
function ColResizeHandle({ onStart, onReset, dragging, className, label }) {
    return (
        <Box
            className={className}
            onPointerDown={onStart}
            onDoubleClick={onReset}
            role="separator"
            aria-orientation="vertical"
            aria-label={label}
            sx={{
                flexShrink: 0,
                width: 7,
                cursor: 'col-resize',
                touchAction: 'none',   // 防止触摸时浏览器把手势当滚动
                bgcolor: dragging ? 'var(--panel-accent-bg-hover)' : 'transparent',
                '&:hover': { bgcolor: 'var(--panel-accent-bg)' },
                position: 'relative',
                '&::after': {
                    content: '""',
                    position: 'absolute',
                    top: '50%', left: '50%',
                    transform: 'translate(-50%, -50%)',
                    width: 3, height: 36,
                    borderRadius: 2,
                    bgcolor: 'var(--panel-icon)',
                },
                '&:hover::after': { bgcolor: 'var(--panel-icon-hover)' },
            }}
        />
    );
}

function DeviceLogPanel() {
    const logs = usePicoLogs();
    const [collapsed, setCollapsed] = useState(false);
    const [luaOnly, setLuaOnly] = useState(false);
    const { height, dragging, startDrag, reset } = useLogPanelHeight(collapsed);
    const filter = luaOnly ? line => /\[lua(?: warn)?\]/.test(line) : undefined;
    return (
        <Box sx={{
            flexShrink: 0,
            display: 'flex',
            flexDirection: 'column',
            height: collapsed ? 38 : height,
            borderTop: `1px solid ${PANEL_LINE}`,
        }}>
            {!collapsed && <LogDragHandle onStart={startDrag} onReset={reset} dragging={dragging} />}
            <LogViewer logs={logs} filter={filter} collapsed={collapsed}
                onToggleCollapsed={() => setCollapsed(!collapsed)} onClear={clearLogs}
                onExport={() => exportLogLines(filter ? logs.filter(filter) : logs)}
                headerLeft={<Button onClick={() => setLuaOnly(!luaOnly)}>{luaOnly ? '仅 Lua' : '全部日志'}</Button>}
                sx={{ px: 1.5, pb: 1, pt: 0.5, flex: 1, minHeight: 0 }} />
        </Box>
    );
}

export default function LuaWorkspace() {
    const [model] = useState(() => new WorkspaceModel(deviceSession, defaultSource));
    const editor = useStore(model);
    const device = useStore(deviceSession);
    const editorRef = useRef();
    const [aiOpen, setAiOpen] = useState(false);
    const [aiRunning, setAiRunning] = useState(false);
    const [unread, setUnread] = useState(false);
    const { width: aiWidth, dragging: aiDragging, startDrag: aiStartDrag, reset: aiResetWidth } = useAiPanelWidth(aiOpen);
    const left = useLeftPanel();
    const [dialog, setDialog] = useState('');
    const [menu, setMenu] = useState(null);
    const [confirm, setConfirm] = useState(null);
    const [error, setError] = useState('');
    const [baud, setBaud] = useState('4000000');
    const theme = useThemeMode();
    const connected = device.state === 'connected';
    const locked = !!device.busy || aiRunning || device.state === 'connecting';
    const byteLength = new TextEncoder().encode(editor.script).length;
    const tooLarge = byteLength > SCRIPT_MAX_BYTES;
    const matchesRam = device.ramSource !== null && editor.script === device.ramSource;
    const canSave = connected && !locked && matchesRam && !tooLarge && device.status?.kvReady;
    const attempt = async fn => {
        setError('');
        try { await fn(); } catch (e) { setError(e.detail || e.message || String(e)); }
    };
    const ask = (title, text, action) => { setMenu(null); setConfirm({ title, text, action }); };
    const connect = kind => {
        // Start the browser picker in the original click's user activation.
        setError('');
        const pending = deviceSession.connect(kind, { baudRate: Number(baud) });
        attempt(async () => { await pending; await model.read({ automatic: true }); });
    };
    const importSource = source => model.edit(source, '已导入编辑器，尚未加载到设备。');
    // 窄屏（≤720px）三栏放不下：左侧面板与 AI 面板互斥，开一个就收另一个
    const isNarrow = () => typeof window !== 'undefined' && window.innerWidth <= 720;
    const toggleLeft = () => {
        const next = !left.open;
        left.setOpen(next);
        if (next && isNarrow()) setAiOpen(false);
    };
    const toggleAi = () => {
        const next = !aiOpen;
        setAiOpen(next);
        setUnread(false);
        if (next && isNarrow()) left.setOpen(false);
    };
    useEffect(() => {
        const handler = event => {
            if (model.getSnapshot().draftError) { event.preventDefault(); event.returnValue = ''; }
        };
        window.addEventListener('beforeunload', handler);
        return () => window.removeEventListener('beforeunload', handler);
    }, [model]);

    return <Box className="workspace">
        <Box component="header" className="workspace-header">
            <Stack direction="row" alignItems="center" spacing={1}>
                <IconButton size="small" className="workspace-left-toggle" aria-label="切换左侧面板"
                    title={left.open ? '收起左侧面板' : '展开左侧面板'} onClick={toggleLeft}
                    sx={{ color: left.open ? 'var(--panel-accent)' : 'var(--panel-muted)' }}>
                    <ViewSidebarIcon fontSize="small" />
                </IconButton>
                <CodeIcon color="primary" />
                <Typography component="h1" sx={{ fontSize: 16, fontWeight: 700 }}>Pico DS5 <Box component="span" sx={{ color: 'var(--panel-muted)', fontWeight: 400 }}> / Lua 工作台</Box></Typography>
                <StatusPill tone={connected ? 'accent' : 'muted'}>{connected ? device.transport === 'serial' ? 'UART 已连接' : 'USB 已连接' : device.state === 'connecting' ? '连接中' : '离线'}</StatusPill>
            </Stack>
            <Stack direction="row" spacing={0.75} alignItems="center" sx={{ flexWrap: 'wrap', gap: 0.5 }}>
                {connected ? <>
                    <Button disabled={locked} onClick={() => attempt(() => deviceSession.refresh())}>刷新状态</Button>
                    <Button onClick={() => attempt(() => deviceSession.disconnect())}>断开</Button>
                </> : <>
                    <Button variant="outlined" disabled={locked || !navigator.usb} onClick={() => connect('winusb')}>连接 USB</Button>
                    <Button variant="outlined" disabled={locked || !navigator.serial} onClick={() => connect('serial')}>连接 UART</Button>
                    <TextField size="small" label="波特率" inputProps={{ inputMode: 'numeric', 'aria-label': '波特率' }} value={baud} onChange={e => setBaud(e.target.value)} sx={{ width: 106 }} />
                </>}
                <IconButton title={`主题：${THEME_LABELS[theme.mode]}`} onClick={theme.cycle}><ContrastIcon fontSize="small" /></IconButton>
            </Stack>
        </Box>
        <Box className="workspace-toolbar">
            <Stack direction="row" spacing={0.5} alignItems="center">
                <Button variant={left.open ? 'contained' : 'outlined'} onClick={toggleLeft}>脚本管理</Button>
                <Button onClick={() => setDialog('export')}>导出</Button>
                <Button disabled={!connected || locked} onClick={() => ask('读取设备', '用设备当前运行脚本替换编辑器内容？', () => model.read())}>读取设备</Button>
                <IconButton title="更多操作" disabled={locked} onClick={e => setMenu(e.currentTarget)}><MoreHorizIcon fontSize="small" /></IconButton>
            </Stack>
            <Stack direction="row" spacing={0.75}>
                <Button variant={aiOpen ? 'contained' : 'outlined'} onClick={toggleAi}>AI 助手{aiRunning ? ' · 工作中' : unread ? ' · 新回复' : ''}</Button>
                <Button variant="outlined" startIcon={<PlayArrowIcon />} disabled={!connected || locked || tooLarge} onClick={() => attempt(() => model.run())}>加载到内存</Button>
                <Button variant="contained" startIcon={<SaveIcon />} disabled={!canSave} onClick={() => ask('写入存储', '将当前运行脚本保存到 KV，作为下次启动的脚本？', () => model.save())}>写入存储</Button>
            </Stack>
        </Box>
        {(error || device.error || editor.draftError || editor.notice) && <Box role={error || device.error || editor.draftError ? 'alert' : 'status'} className="workspace-notice" sx={{ color: error || device.error || editor.draftError ? 'var(--panel-danger)' : 'var(--panel-muted)' }}>
            {error || editor.draftError || device.error || editor.notice}
        </Box>}
        {!window.isSecureContext && <Box className="workspace-notice">设备连接需要 HTTPS 或 localhost；当前仍可离线编辑。</Box>}
        {device.busy && <LinearProgress sx={{ height: 2, flexShrink: 0 }} />}
        <Box className={`workspace-body${aiOpen ? ' ai-visible' : ''}${left.open ? ' left-visible' : ''}`}>
            {left.open && <>
                <LeftPanel width={left.width}
                    onApplyFromFile={importSource}
                    onEdit={item => importSource(item.script)}
                    onApply={item => { importSource(item.script); attempt(() => model.run(item.script)); }} />
                <ColResizeHandle className="workspace-left-resizer" label="拖动调整左侧面板宽度（双击重置）"
                    onStart={left.startDrag} onReset={left.reset} dragging={left.dragging} />
            </>}
            <Box className="workspace-editor-pane">
                <Box className="workspace-filebar">
                    <span>main.lua</span>
                    <span>{connected ? matchesRam ? device.status?.dirty ? '内存运行 · 尚未保存' : '与运行脚本一致' : '编辑器改动 · 尚未加载' : '本地草稿'}</span>
                    <span style={{ marginLeft: 'auto', color: tooLarge ? 'var(--panel-danger)' : undefined }}>{(byteLength / 1024).toFixed(1)} / 60 KiB · {editor.script.split('\n').length} 行</span>
                </Box>
                <Box className="workspace-code" data-testid="lua-editor"><CodeEditor ref={editorRef} value={editor.script} onChange={model.edit} disabled={aiRunning} placeholder="-- 编写 Lua 脚本" /></Box>
                <DeviceLogPanel />
            </Box>
            {aiOpen && <ColResizeHandle className="workspace-ai-resizer" label="拖动调整 AI 面板宽度（双击重置）"
                onStart={aiStartDrag} onReset={aiResetWidth} dragging={aiDragging} />}
            <Box component="aside" className="workspace-ai" style={{ '--ai-width': `${aiWidth}px` }} sx={{ display: aiOpen ? 'flex' : 'none' }}>
                <AiChatPanel open={aiOpen} getScript={model.getScript} applyScript={model.edit}
                    onRunningChange={setAiRunning} onUnread={setUnread} onFoundLines={lines => editorRef.current?.scrollToLine(lines[0])} />
            </Box>
        </Box>
        <Box component="footer" className="workspace-footer">
            <span>{device.busy || (aiRunning ? 'AI 正在处理脚本' : '浏览器自动保留草稿 · 运行不写存储')}</span>
            <span>{device.status ? `KV ${device.status.saved ? '有已存脚本' : '无已存脚本'} · 分区 ${Math.round(device.status.partitionSize / 1024)} KiB` : '脚本上限 60 KiB · KV 分区 512 KiB'}</span>
        </Box>
        <Menu anchorEl={menu} open={!!menu} onClose={() => setMenu(null)}>
            <MenuItem onClick={() => ask('载入预制模板', '用预制脚本替换编辑器内容？此操作只修改浏览器草稿。', () => importSource(defaultSource))}>预制模板 → 编辑器</MenuItem>
            <MenuItem disabled={!connected} onClick={() => ask('加载 KV', '从 KV 读取并运行脚本，同时替换编辑器内容？', () => model.switchRunning('kv'))}>从 KV 加载运行</MenuItem>
            <MenuItem disabled={!connected} onClick={() => ask('运行预制脚本', '切换到预制脚本并替换编辑器内容？已保存的 KV 脚本保留。', () => model.switchRunning('default'))}>运行预制脚本</MenuItem>
            <MenuItem disabled={!connected || !device.status?.saved} onClick={() => ask('删除 KV 脚本', '删除持久化脚本？当前脚本继续运行；下次启动运行预制脚本。', async () => { await deviceSession.deleteSaved(); model.notify('已删除 KV 脚本，当前运行脚本保持原样。'); })}>删除 KV 脚本</MenuItem>
            <MenuItem onClick={() => { setMenu(null); setDialog('info'); }}>设备与 API</MenuItem>
        </Menu>
        <LuaExportDialog open={dialog === 'export'} onClose={() => setDialog('')} script={editor.script} onNotify={model.notify} />
        <DialogShell open={!!confirm} onClose={() => setConfirm(null)} title={confirm?.title} maxWidth="xs"
            actions={<><Button onClick={() => setConfirm(null)}>取消</Button><Button variant="contained" onClick={() => { const action = confirm.action; setConfirm(null); attempt(action); }}>确认</Button></>}>
            <Typography sx={{ fontSize: 13 }}>{confirm?.text}</Typography>
        </DialogShell>
        <DialogShell open={dialog === 'info'} onClose={() => setDialog('')} title="设备与 API" actions={<Button onClick={() => setDialog('')}>关闭</Button>}>
            <Typography component="pre" className="selectable" sx={{ fontSize: 12, whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{typeof device.info === 'string' ? device.info || '连接设备后可刷新状态获取信息。' : JSON.stringify(device.info, null, 2)}</Typography>
            <Typography sx={{ fontSize: 12 }}>USB 使用 WebUSB，UART 使用 Web Serial。推荐使用桌面 Chromium 浏览器。Lua API 文档随 AI 助手内置。</Typography>
        </DialogShell>
    </Box>;
}
