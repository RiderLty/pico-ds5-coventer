// ---------------------------------------------------------------------------
// Lua 面板的脚本库 UI。
// 设备上只能存一份板载脚本，浏览器侧的脚本库（localStorage，utils/luaLibrary.js）
// 提供多份命名存档：
//   · 导出 = 命名后"保存到浏览器"或"导出为文件"（弹窗）；
//   · 脚本管理 = 拖拽/点击选文件进编辑器，或从脚本库一键"应用"到编辑器并加载到内存
//     —— 这块原来是「导入」弹窗，现在直接嵌进左侧面板（LuaScriptManager），拉满高度。
// 组件必须定义在模块作用域（见 CLAUDE.md「面板组件必须定义在模块作用域」）。
// ---------------------------------------------------------------------------
import React, { useState, useEffect, useRef, useCallback } from 'react';
import { Box, Button, IconButton, Stack, TextField, Tooltip, Typography } from '@mui/material';
import CodeIcon from '@mui/icons-material/Code';
import DownloadIcon from '@mui/icons-material/Download';
import SaveIcon from '@mui/icons-material/Save';
import UploadFileIcon from '@mui/icons-material/UploadFile';
import EditIcon from '@mui/icons-material/Edit';
import PlayArrowIcon from '@mui/icons-material/PlayArrow';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import {
    DialogShell, Hint, Notice, PanelGroup,
    PANEL_ACCENT, PANEL_ACCENT_BG, PANEL_ACCENT_LINE, PANEL_DANGER, PANEL_DANGER_BG,
    PANEL_DANGER_LINE, PANEL_INK, PANEL_LINE, PANEL_MUTED,
    dialogActionSx,
} from './PanelKit';
import { downloadTextFile } from '../utils/download';
import {
    listLuaScripts, saveLuaScript, deleteLuaScript, hasLuaScript, lastExportName,
    LUA_LIBRARY_EVENT,
} from '../utils/luaLibrary';

const encoder = new TextEncoder();
const fmtBytes = (n) => (n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`);
const fmtTime = (ts) => {
    const d = new Date(ts);
    const pad = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
};
// 文件名只取安全子集（download 属性里的 / 等会被浏览器静默改名，不如自己换掉）
const safeFileStem = (name) => String(name).replace(/[\\/:*?"<>|]/g, '_').trim() || 'onboard';

// 脚本库行内的小图标按钮：单图标、描边（outline）、鼠标悬浮出标签（Tooltip）。
// danger = 删除的确认态（红描边）；确认态下 label 换成"再点一次"提示。
function RowIconBtn({ label, onClick, danger, disabled, children }) {
    return (
        <Tooltip title={label} arrow placement="top">
            <IconButton size="small" disableRipple aria-label={label} disabled={disabled} onClick={onClick}
                sx={{
                    width: 30, height: 30, borderRadius: '6px', p: 0.5,
                    border: `1px solid ${danger ? PANEL_DANGER_LINE : PANEL_LINE}`,
                    color: danger ? PANEL_DANGER : PANEL_MUTED,
                    transition: 'color .15s, border-color .15s, background-color .15s',
                    '&:hover': {
                        color: danger ? PANEL_DANGER : PANEL_ACCENT,
                        borderColor: danger ? PANEL_DANGER_LINE : PANEL_ACCENT_LINE,
                        bgcolor: danger ? PANEL_DANGER_BG : PANEL_ACCENT_BG,
                    },
                }}>
                {children}
            </IconButton>
        </Tooltip>
    );
}

// ── 导出：命名 → 保存到浏览器 / 导出为文件 ──────────────────────
export function LuaExportDialog({ open, onClose, script, onNotify }) {
    const [name, setName] = useState('');
    const [errMsg, setErrMsg] = useState(null);
    useEffect(() => {
        if (open) { setName(lastExportName() || 'onboard'); setErrMsg(null); }
    }, [open]);
    const trimmed = name.trim();
    const exists = hasLuaScript(trimmed);

    const handleSaveBrowser = () => {
        setErrMsg(null);
        try {
            const existed = saveLuaScript(trimmed, script);
            onNotify?.(`✓ 已保存到浏览器：${trimmed}${existed ? '（覆盖同名存档）' : ''}`);
            onClose();
        } catch (e) {
            setErrMsg(e.message);
        }
    };

    return (
        <DialogShell
            open={open}
            onClose={onClose}
            maxWidth="xs"
            icon={<CodeIcon sx={{ fontSize: 18, color: PANEL_ACCENT }} />}
            title="导出脚本"
            hint="当前编辑器里的整份脚本：留在浏览器里随时取用，或存成文件带走。"
            status={exists ? { text: '同名将覆盖', tone: 'warn' } : null}
            actions={<>
                <Button size="small" startIcon={<DownloadIcon />} sx={dialogActionSx.outline}
                    onClick={() => downloadTextFile(`${safeFileStem(trimmed)}.lua`, script)}>
                    导出为文件
                </Button>
                <Box sx={{ flex: 1 }} />
                <Button size="small" startIcon={<SaveIcon />} variant="contained" disableElevation
                    sx={dialogActionSx.primary} onClick={handleSaveBrowser}>
                    保存到浏览器
                </Button>
            </>}
        >
            <Stack spacing={1.25}>
                <TextField size="small" label="脚本名称" value={name} autoFocus fullWidth
                    onChange={(e) => setName(e.target.value)}
                    onKeyDown={(e) => { if (e.key === 'Enter') handleSaveBrowser(); }} />
                <Hint>按名称管理、同名覆盖；导出的文件名为「名称.lua」。</Hint>
                <Notice tone="warn">{errMsg}</Notice>
            </Stack>
        </DialogShell>
    );
}

// ── 脚本管理（左侧面板内容）：拖拽/点击选文件，或从浏览器脚本库应用 ──────
// 原「导入脚本」弹窗的内容，现直接嵌进左侧面板，拉满高度：
// 顶部拖拽区固定，下方脚本库列表撑满剩余高度并自行滚动。
// active：面板是否可见（重新打开时刷新列表）；列表增删也会经 LUA_LIBRARY_EVENT 实时刷新。
// onApplyFromFile(text, fname)：文件内容放进编辑器（尚未保存）
// onEdit(item)：脚本库存档 → 只放进编辑器（不加载到内存）
// onApply(item)：脚本库存档 → 编辑器 + 加载到内存
// onExport()：导出编辑器当前脚本（打开「导出脚本」弹窗）
export function LuaScriptManager({ onApplyFromFile, onEdit, onApply, onExport, disabled = false, canExport = !disabled, active = true }) {
    const [saved, setSaved] = useState([]);
    const [dragOver, setDragOver] = useState(false);
    const [pendingDelete, setPendingDelete] = useState(null);  // 两步确认删除
    const fileRef = useRef(null);
    const usable = useRef(!disabled);
    usable.current = !disabled;
    useEffect(() => () => { usable.current = false; }, []);

    const refresh = useCallback(() => setSaved(listLuaScripts()), []);
    useEffect(() => {
        if (active) { refresh(); setDragOver(false); setPendingDelete(null); }
    }, [active, refresh]);
    // 脚本库有增删（导出面板保存 / 行内删除）时实时刷新
    useEffect(() => {
        const onChange = () => refresh();
        window.addEventListener(LUA_LIBRARY_EVENT, onChange);
        return () => window.removeEventListener(LUA_LIBRARY_EVENT, onChange);
    }, [refresh]);

    const readFirstFile = (files) => {
        const file = files?.[0];
        if (!file || disabled) return;
        const reader = new FileReader();
        reader.onload = () => { if (usable.current) onApplyFromFile(String(reader.result), file.name); };
        reader.readAsText(file);
    };

    const handleDelete = (item) => {
        if (pendingDelete !== item.name) {
            setPendingDelete(item.name);   // 第一次点：进入确认态
            return;
        }
        deleteLuaScript(item.name);
        setPendingDelete(null);
        refresh();
    };

    return (
        <Box sx={{ display: 'flex', flexDirection: 'column', gap: 1.25, flex: 1, minHeight: 0 }}>
            {/* 拖拽区：点击同样弹出文件选择框（固定高度，不随列表滚动） */}
            <Box
                aria-disabled={disabled} onClick={() => { if (!disabled) fileRef.current?.click(); }}
                onDragOver={(e) => { e.preventDefault(); if (!disabled) setDragOver(true); }}
                onDragLeave={() => setDragOver(false)}
                onDrop={(e) => { e.preventDefault(); setDragOver(false); readFirstFile(e.dataTransfer?.files); }}
                sx={{
                    flexShrink: 0,
                    border: `1.5px dashed ${dragOver ? PANEL_ACCENT_LINE : PANEL_LINE}`,
                    borderRadius: '8px', px: 1.5, py: 2, textAlign: 'center', cursor: 'pointer',
                    bgcolor: dragOver ? PANEL_ACCENT_BG : 'transparent',
                    transition: 'border-color .15s, background-color .15s',
                }}
            >
                <UploadFileIcon sx={{ fontSize: 22, color: dragOver ? PANEL_ACCENT : PANEL_MUTED }} />
                <Typography sx={{ fontSize: 12.5, color: dragOver ? PANEL_ACCENT : PANEL_MUTED, mt: 0.5 }}>
                    {disabled ? '连接并读取设备脚本后可导入' : dragOver ? '松开即导入' : '点击选择或拖入 .lua 文件'}
                </Typography>
            </Box>
            <input ref={fileRef} type="file" disabled={disabled} accept=".lua,text/plain" hidden
                onChange={(e) => { readFirstFile(e.target.files); e.target.value = ''; }} />

            {onExport && (
                <Button size="small" variant="outlined" fullWidth disableElevation
                    startIcon={<DownloadIcon sx={{ fontSize: 16 }} />}
                    disabled={!canExport} onClick={onExport} sx={{ flexShrink: 0, textTransform: 'none' }}>
                    导出当前脚本
                </Button>
            )}

            <PanelGroup dense title={`浏览器脚本库（${saved.length}）`}
                hint="用「导出」保存到这里，换浏览器或清了站点数据就没了"
                sx={{ flex: 1, minHeight: 160, display: 'flex', flexDirection: 'column' }}
                bodySx={{ flex: 1, minHeight: 0, overflow: 'auto' }}>
                {saved.length === 0 ? (
                    <Hint>暂无存档。</Hint>
                ) : (
                    <Stack divider={<Box sx={{ height: 1, bgcolor: PANEL_LINE }} />}>
                        {saved.map((item) => (
                            <Box key={item.name} sx={{ py: 0.75, display: 'flex', alignItems: 'center', gap: 0.75 }}>
                                <Box sx={{ minWidth: 0, flex: 1 }}>
                                    <Typography sx={{
                                        fontSize: 13, color: PANEL_INK, lineHeight: 1.4,
                                        overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                                    }}>
                                        {item.name}
                                    </Typography>
                                    <Typography sx={{ fontSize: 11, color: PANEL_MUTED }}>
                                        {fmtBytes(item.byteLen)} · {fmtTime(item.timestamp)}
                                    </Typography>
                                </Box>
                                <Box sx={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 0.5 }}>
                                    <RowIconBtn disabled={disabled} label="编辑（仅放入编辑器）" onClick={() => onEdit(item)}>
                                        <EditIcon sx={{ fontSize: 15 }} />
                                    </RowIconBtn>
                                    <RowIconBtn disabled={disabled} label="应用（编辑器 + 加载到内存）" onClick={() => onApply(item)}>
                                        <PlayArrowIcon sx={{ fontSize: 15 }} />
                                    </RowIconBtn>
                                    <RowIconBtn label="下载为文件"
                                        onClick={() => downloadTextFile(`${safeFileStem(item.name)}.lua`, item.script)}>
                                        <DownloadIcon sx={{ fontSize: 15 }} />
                                    </RowIconBtn>
                                    <RowIconBtn label={pendingDelete === item.name ? '再点一次确认删除' : '删除'}
                                        danger={pendingDelete === item.name}
                                        onClick={() => handleDelete(item)}>
                                        <DeleteOutlineIcon sx={{ fontSize: 15 }} />
                                    </RowIconBtn>
                                </Box>
                            </Box>
                        ))}
                    </Stack>
                )}
            </PanelGroup>
        </Box>
    );
}
