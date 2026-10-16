import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Box, Button, IconButton, LinearProgress, Snackbar, Stack, TextField, Typography } from '@mui/material';
import PlayArrowIcon from '@mui/icons-material/PlayArrow';
import SaveIcon from '@mui/icons-material/Save';
import ContrastIcon from '@mui/icons-material/Contrast';
import ViewSidebarIcon from '@mui/icons-material/ViewSidebar';
import FolderOpenIcon from '@mui/icons-material/FolderOpen';
import UsbIcon from '@mui/icons-material/Usb';
import UploadIcon from '@mui/icons-material/Upload';
import DescriptionIcon from '@mui/icons-material/Description';
import StorageIcon from '@mui/icons-material/Storage';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import InfoOutlinedIcon from '@mui/icons-material/InfoOutlined';
import CodeEditor from './CodeEditor';
import LogViewer from './LogViewer';
import { LuaScriptManager, LuaExportDialog } from './LuaScriptDialogs';
import AiChatPanel from './ai/AiChatPanel';
import { DialogShell, StatusPill, PANEL_LINE, PanelGroup, PanelRow, DeviceEntry } from './PanelKit';
import { deviceSession, SCRIPT_MAX_BYTES } from '../utils/deviceSession';
import { initAiCfg } from '../utils/aiSettings';
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

// ── 左侧面板：可拖动宽度 + 展开状态 + 当前区段，均存浏览器 ────────────────
// 仿 VS Code：最左一列活动栏（图标）切换区段，右侧是区段内容。
// 展开状态 / 宽度 / 当前区段都写 localStorage，下次打开页面自动恢复。
const LEFT_WIDTH_KEY = 'ds5_left_panel_width_v1';
const LEFT_OPEN_KEY = 'ds5_left_panel_open_v1';
const LEFT_SECTION_KEY = 'ds5_left_panel_section_v1';
const LEFT_WIDTH_MIN = 200;

function defaultLeftWidth() {
    return 320;
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
    const [section, setSection] = useState(() => {
        try { return localStorage.getItem(LEFT_SECTION_KEY) || 'device'; } catch { return 'device'; }
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

    // 展开状态 / 区段持久化（首帧也会写一次，把缺省值固化下来）
    useEffect(() => {
        try { localStorage.setItem(LEFT_OPEN_KEY, open ? '1' : '0'); } catch { /* ignore */ }
    }, [open]);
    useEffect(() => {
        try { localStorage.setItem(LEFT_SECTION_KEY, section); } catch { /* ignore */ }
    }, [section]);

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

    return { open, setOpen, section, setSection, width, dragging, startDrag, reset };
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

// ── AI 面板：可拖动宽度 + 展开状态，均存浏览器 ──────────────────────────
// 与左侧面板同构（键名各自独立，互不干扰）：
//   · 编辑区与 AI 面板之间的分割条可左右拖动，松手后宽度存 localStorage；
//   · 展开 / 收起状态同样落盘，下次打开页面自动恢复。
// 双击分割条回到默认宽度（35vw，钳制在 360~600px）。
const AI_WIDTH_KEY = 'ds5_ai_panel_width_v1';
const AI_OPEN_KEY = 'ds5_ai_panel_open_v1';
const AI_WIDTH_MIN = 300;

// 首次进入（localStorage 里还没有记录）时的缺省：宽屏开、窄屏关。
// 窄屏三栏放不下，AI 面板铺满整屏会直接盖住编辑器，一进来就看不到代码。
function defaultAiOpen() {
    return typeof window === 'undefined' || window.innerWidth > 720;
}

function defaultAiWidth() {
    const vw = typeof window !== 'undefined' ? window.innerWidth : 1200;
    return Math.max(360, Math.min(600, Math.round(vw * 0.35)));
}

function maxAiWidth() {
    const vw = typeof window !== 'undefined' ? window.innerWidth : 1200;
    return Math.max(AI_WIDTH_MIN, vw - 320);   // 给编辑器至少留 320px
}

function useAiPanel() {
    const [open, setOpen] = useState(() => {
        try {
            const stored = localStorage.getItem(AI_OPEN_KEY);
            if (stored === '1') return true;
            if (stored === '0') return false;
        } catch { /* 隐私模式 / storage 不可用 */ }
        return defaultAiOpen();
    });
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

    // 展开状态持久化（首帧也会写一次，把缺省值固化下来）
    useEffect(() => {
        try { localStorage.setItem(AI_OPEN_KEY, open ? '1' : '0'); } catch { /* ignore */ }
    }, [open]);

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
            try { localStorage.setItem(AI_WIDTH_KEY, String(widthRef.current)); } catch { /* ignore */ }
        };
        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
    }, [open]);

    const reset = useCallback(() => {
        setWidth(defaultAiWidth());
        try { localStorage.removeItem(AI_WIDTH_KEY); } catch { /* ignore */ }
    }, []);

    return { open, setOpen, width, dragging, startDrag, reset };
}

// 弹窗底栏按钮的统一样式（小号、无大写、紧凑）
const compactBtnSx = { textTransform: 'none', minWidth: 0, px: 1.25 };

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

/*--------------------------------------------------------------------+
 * 左侧面板：活动栏 + 区段内容
 *--------------------------------------------------------------------*/

// 区段表：加新区段只需在这里加一项，并补一个渲染分支。
// 顺序即活动栏自上而下的顺序 —— 设备在前（连接是第一步操作），脚本管理在后。
const LEFT_SECTIONS = [
    { id: 'device', label: '设备', Icon: UsbIcon },
    { id: 'scripts', label: '脚本管理', Icon: FolderOpenIcon },
];

// 活动栏按钮（VS Code 风格）：图标竖排，选中项左侧一条强调色竖条
function ActivityButton({ active, label, Icon, onClick }) {
    return (
        <Box
            component="button"
            type="button"
            onClick={onClick}
            aria-label={label}
            aria-pressed={active}
            title={label}
            sx={{
                position: 'relative', width: '100%', height: 44, border: 0, p: 0,
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                cursor: 'pointer', bgcolor: 'transparent',
                color: active ? 'var(--panel-accent)' : 'var(--panel-muted)',
                transition: 'color .15s, background-color .15s',
                '&:hover': { color: active ? 'var(--panel-accent)' : 'var(--panel-ink)', bgcolor: 'var(--panel-hover-bg)' },
                '&::before': active ? {
                    content: '""', position: 'absolute', left: 0, top: 8, bottom: 8,
                    width: 2, borderRadius: '0 2px 2px 0', bgcolor: 'var(--panel-accent)',
                } : undefined,
            }}
        >
            <Icon sx={{ fontSize: 20 }} />
        </Box>
    );
}

// ── 区段：脚本管理 ──────────────────────────────────────────────────────
function ScriptsSection({ onApplyFromFile, onEdit, onApply, onExport, disabled, canExport }) {
    return <LuaScriptManager canExport={canExport} disabled={disabled} onApplyFromFile={onApplyFromFile} onEdit={onEdit}
        onApply={onApply} onExport={onExport} />;
}

// ── 区段：设备（连接 + 板载脚本操作 + 信息）──────────────────────────────
function DeviceSection({ connected, transport, locked, baud, onBaud, reconnectStatus,
    onConnect, onRefresh, onDisconnect, actions, onInfo }) {
    return (
        <Stack spacing={1.25} sx={{ flex: 1, minHeight: 0 }}>
            <PanelGroup dense title="连接"
                hint={connected ? undefined : 'USB 走 WebUSB，UART 走 Web Serial'}>
                {connected ? (
                    <PanelRow label={transport === 'serial' ? 'UART 已连接' : 'USB 已连接'}>
                        <Button size="small" variant="outlined" disableElevation disabled={locked}
                            onClick={onRefresh} sx={compactBtnSx}>刷新</Button>
                        <Button size="small" variant="outlined" disableElevation
                            onClick={onDisconnect} sx={compactBtnSx}>断开</Button>
                    </PanelRow>
                ) : (
                    <Stack spacing={1}>
                        <Button variant="outlined" fullWidth disableElevation
                            disabled={locked || !navigator.usb}
                            onClick={() => onConnect('winusb')} sx={compactBtnSx}>连接 USB</Button>
                        <Button variant="outlined" fullWidth disableElevation
                            disabled={locked || !navigator.serial}
                            onClick={() => onConnect('serial')} sx={compactBtnSx}>连接 UART</Button>
                        <TextField size="small" fullWidth label="波特率" value={baud}
                            inputProps={{ inputMode: 'numeric', 'aria-label': '波特率' }}
                            onChange={e => onBaud(e.target.value)} />
                        {reconnectStatus && <Typography variant="caption" role="status">{reconnectStatus}</Typography>}
                    </Stack>
                )}
            </PanelGroup>

            <PanelGroup dense title="板载脚本" hint="设备只存一份运行脚本">
                {actions.map(a => <DeviceEntry key={a.title} {...a} />)}
            </PanelGroup>

            <PanelGroup dense title="关于">
                <DeviceEntry icon={<InfoOutlinedIcon sx={{ fontSize: 18 }} />}
                    title="设备与 API" hint="接口说明与设备信息" onClick={onInfo} />
            </PanelGroup>
        </Stack>
    );
}

// ── 左侧面板外壳：活动栏 + 当前区段 ──────────────────────────────────────
function LeftPanel({ width, section, onSection, scripts, device }) {
    const current = LEFT_SECTIONS.find(s => s.id === section) || LEFT_SECTIONS[0];
    return (
        <Box component="aside" className="workspace-left" style={{ '--left-width': `${width}px` }}>
            <Box className="workspace-activity">
                {LEFT_SECTIONS.map(({ id, label, Icon }) => (
                    <ActivityButton key={id} active={current.id === id} label={label} Icon={Icon}
                        onClick={() => onSection(id)} />
                ))}
            </Box>
            <Box className="workspace-left-main">
                <Box className="workspace-left-header">
                    <current.Icon sx={{ fontSize: 16, color: 'var(--panel-accent)' }} />
                    <Typography sx={{ fontSize: 12, fontWeight: 600, color: 'var(--panel-ink)' }}>
                        {current.label}
                    </Typography>
                </Box>
                <Box className="workspace-left-body">
                    {current.id === 'scripts'
                        ? <ScriptsSection {...scripts} />
                        : <DeviceSection {...device} />}
                </Box>
            </Box>
        </Box>
    );
}

export default function LuaWorkspace() {
    const [model] = useState(() => new WorkspaceModel(deviceSession));
    const editor = useStore(model);
    const device = useStore(deviceSession);
    const editorRef = useRef();
    // AI 面板：展开状态与宽度都记忆在浏览器里（与左侧面板同构）
    const { open: aiOpen, setOpen: setAiOpen, width: aiWidth, dragging: aiDragging,
        startDrag: aiStartDrag, reset: aiResetWidth } = useAiPanel();
    const [aiRunning, setAiRunning] = useState(false);
    const [unread, setUnread] = useState(false);
    const left = useLeftPanel();
    const [dialog, setDialog] = useState('');
    const [confirm, setConfirm] = useState(null);
    const [error, setError] = useState('');
    const [toastClosed, setToastClosed] = useState(true);
    const [baud, setBaud] = useState('4000000');
    const theme = useThemeMode();
    const connected = device.state === 'connected';
    const connecting = device.state === 'connecting';
    const ready = connected && editor.ready;
    const aiReady = ready && device.aiReady;
    const epoch = deviceSession.epoch;
    useEffect(() => () => model.dispose(), [model]);
    useEffect(() => { setConfirm(null); setDialog(''); setError(''); setAiRunning(false); }, [epoch]);
    useEffect(() => {
        const warnUnsaved = event => {
            if (model.getSnapshot().hasEdits) { event.preventDefault(); event.returnValue = ''; }
        };
        window.addEventListener('beforeunload', warnUnsaved);
        return () => window.removeEventListener('beforeunload', warnUnsaved);
    }, [model]);
    const locked = !!device.busy || aiRunning || connecting || device.state === 'disconnecting';
    const byteLength = new TextEncoder().encode(editor.script).length;
    const tooLarge = byteLength > SCRIPT_MAX_BYTES;
    const matchesRam = device.ramSource !== null && editor.script === device.ramSource;
    const canSave = ready && !locked && matchesRam && !tooLarge && device.status?.kvReady;

    // 操作反馈以浮动提示呈现（不占版面）；noticeSeq 变化即视为"又来了一条"，重置计时
    useEffect(() => { setToastClosed(!editor.notice); }, [editor.noticeSeq, editor.notice]);

    const attempt = async fn => {
        setError('');
        const attemptEpoch = deviceSession.epoch;
        try { await fn(); } catch (e) {
            if (attemptEpoch === deviceSession.epoch) setError(e.detail || e.message || String(e));
        }
    };
    const ask = (title, text, action) => setConfirm({ title, text, action: () => { deviceSession.assertSession(epoch); return action(); } });
    const connect = kind => {
        // Start the browser picker in the original click's user activation.
        setError('');
        const pending = deviceSession.connect(kind, { baudRate: Number(baud) });
        attempt(() => pending);
    };
    // All connections (manual, page restore, hotplug) initialize through one path.
    useEffect(() => {
        if (!connected) return;
        let current = true;
        (async () => {
            try {
                await model.read({ automatic: true });
                deviceSession.assertSession(epoch);
                await initAiCfg();
            } catch (e) {
                if (current && epoch === deviceSession.epoch) setError(e.detail || e.message || String(e));
            }
        })();
        return () => { current = false; };
    }, [connected, epoch, model]);
    const importSource = source => model.edit(source, '已导入编辑器，尚未加载到设备。', epoch);

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

    const editorStatus = ready
        ? (matchesRam
            ? (device.status?.dirty ? '内存运行 · 尚未保存' : '与运行脚本一致')
            : '编辑内容与设备运行脚本不同')
        : (editor.retained ? '编辑内容已保留 · 等待重连读取（只读）' : connected ? '正在读取设备脚本 · 未就绪' : '请先连接设备');

    // 板载脚本操作（左侧「设备」区段）
    const deviceActions = [
        {
            icon: <UploadIcon sx={{ fontSize: 18 }} />,
            title: '读取设备', hint: '用设备当前运行脚本替换编辑器',
            disabled: !connected || locked,
            onClick: () => ask('读取设备', '用设备当前运行脚本替换编辑器内容？', () => model.read()),
        },
        {
            icon: <StorageIcon sx={{ fontSize: 18 }} />,
            title: '从 KV 加载运行', hint: '运行设备上已持久化的脚本',
            disabled: !ready || locked,
            onClick: () => ask('加载 KV', '从 KV 读取并运行脚本，同时替换编辑器内容？', () => model.switchRunning('kv')),
        },
        {
            icon: <DescriptionIcon sx={{ fontSize: 18 }} />,
            title: '运行预制脚本', hint: '切回出厂脚本，已存 KV 保留',
            disabled: !ready || locked,
            onClick: () => ask('运行预制脚本', '切换到预制脚本并替换编辑器内容？已保存的 KV 脚本保留。', () => model.switchRunning('default')),
        },
        {
            icon: <FolderOpenIcon sx={{ fontSize: 18 }} />,
            title: '预制模板 → 编辑器', hint: '替换编辑器，尚未加载到设备',
            disabled: !ready || locked,
            onClick: () => ask('载入预制模板', '用预制脚本替换编辑器内容？此操作只修改编辑器。', () => importSource(defaultSource)),
        },
        {
            icon: <DeleteOutlineIcon sx={{ fontSize: 18 }} />,
            title: '删除 KV 脚本', hint: '下次启动将运行预制脚本',
            disabled: !ready || locked || !device.status?.saved,
            onClick: () => ask('删除 KV 脚本', '删除持久化脚本？当前脚本继续运行；下次启动运行预制脚本。',
                async () => { await deviceSession.deleteSaved(); model.notify('已删除 KV 脚本，当前运行脚本保持原样。'); }),
        },
    ];

    return <Box className="workspace">
        <Box component="header" className="workspace-header">
            <Stack direction="row" alignItems="center" spacing={1.25} sx={{ minWidth: 0 }}>
                <IconButton size="small" className="workspace-left-toggle" aria-label="切换左侧面板"
                    title={left.open ? '收起左侧面板' : '展开左侧面板'} onClick={toggleLeft}
                    sx={{ color: left.open ? 'var(--panel-accent)' : 'var(--panel-muted)' }}>
                    <ViewSidebarIcon fontSize="small" />
                </IconButton>
                <Typography component="h1" noWrap sx={{ fontSize: 15, fontWeight: 700 }}>
                    Pico DS5 <Box component="span" sx={{ color: 'var(--panel-muted)', fontWeight: 400 }}>/ Lua 工作台</Box>
                </Typography>
                <StatusPill tone={connected ? 'accent' : 'muted'}>
                    {connected ? (device.transport === 'serial' ? 'UART 已连接' : 'USB 已连接') : (connecting ? '连接中' : '离线')}
                </StatusPill>
            </Stack>
            <Stack direction="row" spacing={0.75} alignItems="center" sx={{ flexShrink: 0 }}>
                <Button size="small" variant={aiOpen ? 'contained' : 'outlined'} disableElevation
                    disabled={!aiReady} onClick={toggleAi} sx={{ textTransform: 'none' }}>
                    AI 助手{aiRunning ? ' · 工作中' : unread ? ' · 新回复' : ''}
                </Button>
                <IconButton size="small" title={`主题：${THEME_LABELS[theme.mode]}`} onClick={theme.cycle}>
                    <ContrastIcon fontSize="small" />
                </IconButton>
            </Stack>
        </Box>

        {device.busy && <LinearProgress sx={{ height: 2, flexShrink: 0 }} />}
        {(error || device.error) && <Box role="alert" className="workspace-notice" sx={{ color: 'var(--panel-danger)' }}>
            {error || device.error}
        </Box>}
        {!window.isSecureContext && <Box className="workspace-notice">设备连接需要 HTTPS 或 localhost；请连接设备后读取并编辑脚本。</Box>}

        <Box className={`workspace-body${aiOpen ? ' ai-visible' : ''}${left.open ? ' left-visible' : ''}`}>
            {left.open && <>
                <LeftPanel width={left.width} section={left.section} onSection={left.setSection}
                    scripts={{
                        disabled: !ready || locked,
                        canExport: editor.ready || editor.retained,
                        onApplyFromFile: source => attempt(() => importSource(source)),
                        onEdit: item => attempt(() => importSource(item.script)),
                        onApply: item => attempt(async () => { importSource(item.script); await model.run(item.script); }),
                        onExport: () => setDialog('export'),
                    }}
                    device={{
                        connected, transport: device.transport, locked, baud, onBaud: setBaud, reconnectStatus: device.reconnectStatus,
                        onConnect: connect,
                        onRefresh: () => attempt(async () => { if (!editor.ready) await model.read({ automatic: true }); deviceSession.assertSession(epoch); if (!device.aiReady) await initAiCfg(); deviceSession.assertSession(epoch); await deviceSession.refresh(); }),
                        onDisconnect: () => attempt(() => deviceSession.disconnect()),
                        actions: deviceActions,
                        onInfo: () => setDialog('info'),
                    }} />
                <ColResizeHandle className="workspace-left-resizer" label="拖动调整左侧面板宽度（双击重置）"
                    onStart={left.startDrag} onReset={left.reset} dragging={left.dragging} />
            </>}
            <Box className="workspace-editor-pane">
                <Box className="workspace-editor-bar">
                    <span className="workspace-editor-name">main.lua</span>
                    <span className="workspace-editor-status">{editorStatus}</span>
                    <span className="workspace-editor-meta"
                        style={{ color: tooLarge ? 'var(--panel-danger)' : undefined }}>
                        {(byteLength / 1024).toFixed(1)} / 60 KiB · {editor.script.split('\n').length} 行
                    </span>
                    <Stack direction="row" spacing={0.75} sx={{ ml: 'auto', flexShrink: 0 }}>
                        <Button size="small" variant="outlined" disableElevation startIcon={<PlayArrowIcon />}
                            disabled={!ready || locked || tooLarge}
                            onClick={() => attempt(() => model.run())} sx={compactBtnSx}>加载到内存</Button>
                        <Button size="small" variant="contained" disableElevation startIcon={<SaveIcon />}
                            disabled={!canSave}
                            onClick={() => ask('写入存储', '将当前运行脚本保存到 KV，作为下次启动的脚本？', () => model.save())}
                            sx={compactBtnSx}>写入存储</Button>
                    </Stack>
                </Box>
                <Box className="workspace-code" data-testid="lua-editor"><CodeEditor ref={editorRef} value={editor.script} onChange={script => model.edit(script, '', epoch)} disabled={!ready || locked} placeholder={connected ? "正在读取设备脚本…" : "请先连接设备"} /></Box>
                <DeviceLogPanel />
            </Box>
            {aiOpen && <ColResizeHandle className="workspace-ai-resizer" label="拖动调整 AI 面板宽度（双击重置）"
                onStart={aiStartDrag} onReset={aiResetWidth} dragging={aiDragging} />}
            <Box component="aside" className="workspace-ai" style={{ '--ai-width': `${aiWidth}px` }} sx={{ display: aiOpen ? 'flex' : 'none' }}>
                {aiReady ? <AiChatPanel key={epoch} disabled={!!device.busy} open={aiOpen} getScript={() => { model.assertReady(epoch); return model.getScript(); }} applyScript={script => model.edit(script, '', epoch)}
                    onRunningChange={setAiRunning} onUnread={setUnread} onFoundLines={lines => editorRef.current?.scrollToLine(lines[0])} /> : <Typography sx={{ p: 2, color: 'var(--panel-muted)', fontSize: 13 }}>请连接设备并读取脚本与 AI 配置后使用 AI 助手。</Typography>}
            </Box>
        </Box>

        <Box component="footer" className="workspace-footer">
            <span>{device.busy || (aiRunning ? 'AI 正在处理脚本' : '断线保留编辑 · 加载到内存不会写入存储')}</span>
            <span>{device.status ? `KV ${device.status.saved ? '有已存脚本' : '无已存脚本'} · 分区 ${Math.round(device.status.partitionSize / 1024)} KiB` : '脚本上限 60 KiB · KV 分区 512 KiB'}</span>
        </Box>

        <Snackbar open={!toastClosed && !!editor.notice} message={editor.notice}
            autoHideDuration={4200} onClose={() => setToastClosed(true)}
            anchorOrigin={{ vertical: 'bottom', horizontal: 'center' }}
            ContentProps={{ sx: { bgcolor: 'var(--panel-titlebar-bg)', color: 'var(--panel-titlebar-ink)', fontSize: 12.5 } }} />

        <LuaExportDialog open={(editor.ready || editor.retained) && dialog === 'export'} onClose={() => setDialog('')} script={editor.script} onNotify={model.notify} />
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
