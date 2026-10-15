// 面板/弹窗共用的视觉基元（"检查器"风格）
// ═══════════════════════════════════════════════════════════════
// 这里只放**纯展示**的零件，不放业务逻辑。所有面板与弹窗都从这里取，目的是：
//   1) 视觉一致：分组一律扁平描边、阴影只留给浮层；字号/间距/颜色只有一个来源；
//   2) 语义正确：二选一用分段按钮（ChoiceToggle），只有真·开/关才用 Switch；
//   3) 文案一致：标题 + 一行说明 + 状态药丸的表达方式到处一样。
//
// 约定（改 UI 前先读）：
//   - 新的"分组"一律 PanelGroup；新的"设置行"一律 PanelRow / PanelValueRow / PanelSliderRow；
//   - 弹窗外壳一律 DialogShell（统一标题排版、内边距、底栏按钮顺序）；
//   - 不要再引入第二套卡片样式，也不要在按钮文案里放 emoji（图标 + 短句）。
import {
    Box, Button, Dialog, DialogActions, DialogContent, DialogTitle, FormControl, Select,
    Slider, TextField, Tooltip, ToggleButton, ToggleButtonGroup, Typography,
} from "@mui/material";
import { forwardRef, useEffect, useState } from "react";
import ChevronRightIcon from '@mui/icons-material/ChevronRight';

// ── 设计令牌 ────────────────────────────────────────────────────
// 色值本身在 src/index.css 的两组 CSS 变量里（亮色 / 暗色），这里只做**取用**：
// 常量值写成 `var(--panel-xxx)`，在 sx、内联 style、模板字符串里都直接生效，
// 切主题时不需要 React 重渲染（`<html data-theme>` 一变，整棵树立刻跟着换色）。
// 需要真实色值的场合（canvas 2D 绘制、把颜色算成 rgba）**不要**用这些常量。
export const PANEL_WIDTH = 384
export const PANEL_INK = 'var(--panel-ink)'                 // 正文
export const PANEL_MUTED = 'var(--panel-muted)'             // 次要说明
export const PANEL_LINE = 'var(--panel-line)'               // 描边/分隔线
export const PANEL_LINE_STRONG = 'var(--panel-line-strong)' // 描边控件（chip 等可点按的框）暗色下更清晰
export const PANEL_SUNKEN = 'var(--panel-sunken)'           // 下沉底（分组标题条、底栏）
export const PANEL_SURFACE = 'var(--panel-surface)'         // 卡片/弹窗纸面
export const PANEL_CONTENT = 'var(--panel-content)'         // 弹窗内容区/终端底
export const PANEL_ACCENT = 'var(--panel-accent)'           // 选中态文字
export const PANEL_ACCENT_BG = 'var(--panel-accent-bg)'
export const PANEL_ACCENT_BG_HOVER = 'var(--panel-accent-bg-hover)'
export const PANEL_ACCENT_LINE = 'var(--panel-accent-line)'
export const PANEL_SELECTED_BG = 'var(--panel-accent-bg)'
export const PANEL_DANGER = 'var(--panel-danger)'           // 提示/错误文字
export const PANEL_DANGER_BG = 'var(--panel-danger-bg)'
export const PANEL_DANGER_LINE = 'var(--panel-danger-line)'

// ── 文本 ────────────────────────────────────────────────────────
// 一行说明（分组标题下、设置行下、弹窗标题下都用它）
export const Hint = forwardRef(function Hint({ children, sx }, ref) {
    if (!children) return null
    return (
        <Typography ref={ref} sx={{ fontSize: 11, color: PANEL_MUTED, lineHeight: 1.6, ...sx }}>
            {children}
        </Typography>
    )
})

// 状态药丸（"已启用""未保存""通讯中…"）
//
// **必须透传其余 props（...rest）**：它经常被放进 `<Tooltip>` 当直接子元素，而 Tooltip 是把
// onMouseEnter/onFocus/ref 等**塞给子元素**的；只解构 children/tone/sx 的话这些会被吞掉
// ——tooltip 永远不弹（静默失效，dev/生产都不报错）。透传之后 `<Tooltip><StatusPill/></Tooltip>`
// 直接可用，**不要再在外面套一层 `<Box component="span">`**：那层包装会让药丸退回行内元素，
// padding/border 不进高度，药丸会比同排其它药丸矮一截（实测 13.6px vs 20.5px）。
export const StatusPill = forwardRef(function StatusPill({ children, tone = 'muted', sx, ...rest }, ref) {
    if (!children) return null
    const tones = {
        muted: { bgcolor: PANEL_SUNKEN, color: PANEL_MUTED, border: `1px solid ${PANEL_LINE}` },
        accent: { bgcolor: PANEL_ACCENT_BG, color: PANEL_ACCENT, border: `1px solid ${PANEL_ACCENT_LINE}` },
        warn: { bgcolor: 'var(--panel-warn-bg)', color: PANEL_DANGER, border: '1px solid var(--panel-warn-line)' },
    }
    return (
        <Box ref={ref} component="span" {...rest} sx={{
            px: 0.75, borderRadius: '999px', whiteSpace: 'nowrap',
            fontSize: 10.5, lineHeight: 1.8, ...(tones[tone] || tones.muted), ...sx,
        }}>{children}</Box>
    )
})

// 提示条：浅底 + 描边的一行说明（替代 MUI Alert —— 弹窗里不再出现第二套配色）。
// tone='accent' 用于中性/成功说明，tone='warn' 用于错误与风险提示。
// 透传 `...rest`（与 StatusPill 同理）：Tooltip 会往子元素塞 onMouseEnter/ref，
// 而且错误原文常常需要 `className="selectable"` 放行复制——解构漏了就静默失效。
export const Notice = forwardRef(function Notice({ tone = 'accent', children, action, sx, ...rest }, ref) {
    if (!children) return null
    const accent = tone === 'accent'
    return (
        <Box ref={ref} {...rest} sx={{
            display: 'flex', alignItems: 'center', gap: 1, px: 1.25, py: 0.75,
            borderRadius: '6px', border: `1px solid ${accent ? PANEL_ACCENT_LINE : PANEL_DANGER_LINE}`,
            bgcolor: accent ? PANEL_ACCENT_BG : PANEL_DANGER_BG, ...sx,
        }}>
            <Typography sx={{ fontSize: 11.5, lineHeight: 1.6, flex: 1, color: accent ? PANEL_ACCENT : PANEL_DANGER }}>
                {children}
            </Typography>
            {action}
        </Box>
    )
})

// ── 分组与设置行 ────────────────────────────────────────────────
// 分组卡片：扁平（1px 描边 + 8px 圆角，无阴影），标题条下沉
// bodySx：内容区（标题条之下的内边距盒）的附加样式。需要"标题固定、内容自己滚动"时
// （如左侧面板里拉满高度的脚本库列表）传 `{ flex: 1, minHeight: 0, overflow: 'auto' }`。
export const PanelGroup = forwardRef(function PanelGroup({ title, hint, children, sx, bodySx, dense }, ref) {
    return (
        <Box ref={ref} sx={{ border: `1px solid ${PANEL_LINE}`, borderRadius: '8px', overflow: 'hidden', bgcolor: PANEL_SURFACE, ...sx }}>
            {title && (
                <Box sx={{ px: 1.5, py: 0.85, bgcolor: PANEL_SUNKEN, borderBottom: `1px solid ${PANEL_LINE}` }}>
                    <Typography sx={{ fontSize: 12, fontWeight: 600, color: PANEL_INK, lineHeight: 1.4 }}>
                        {title}
                    </Typography>
                    <Hint sx={{ mt: 0.25 }}>{hint}</Hint>
                </Box>
            )}
            <Box sx={{ p: dense ? 1 : 1.25, ...bodySx }}>{children}</Box>
        </Box>
    )
})

// 参数行：左名称（+当前状态弱化跟在后面）／右控件
export const PanelRow = forwardRef(function PanelRow({ label, state, hint, children, alignItems = 'center' }, ref) {
    return (
        <Box ref={ref} sx={{ display: 'flex', alignItems, gap: 1.5, py: 0.4, '& + &': { mt: 0.4 } }}>
            <Box sx={{ minWidth: 0, flex: 1 }}>
                <Box sx={{ display: 'flex', alignItems: 'center', flexWrap: 'wrap', gap: 0.6 }}>
                    <Typography sx={{ fontSize: 13, color: PANEL_INK, lineHeight: 1.45 }}>{label}</Typography>
                    {state ? <StatusPill>{state}</StatusPill> : null}
                </Box>
                <Hint sx={{ mt: 0.25 }}>{hint}</Hint>
            </Box>
            <Box sx={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 0.5 }}>{children}</Box>
        </Box>
    )
})

// 只读数值行（画布上拖动/滚轮调的量，面板里只显示当前值）
// 需要"能被复制"的值（设备序列号这类）传 `className="selectable"`——全站默认不可选，
// 只有 .selectable 白名单里的文字才选得中（见 index.css 的「文字选择策略」）。
export const PanelValueRow = forwardRef(function PanelValueRow({ label, mono, children, ...rest }, ref) {
    return (
        <Box ref={ref} {...rest} sx={{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between', gap: 1, py: 0.4, '& + &': { mt: 0.4 } }}>
            <Typography sx={{ fontSize: 13, color: PANEL_INK }}>{label}</Typography>
            <Typography sx={{
                fontSize: 12, color: PANEL_MUTED,
                fontVariantNumeric: mono === false ? undefined : 'tabular-nums',
            }}>{children}</Typography>
        </Box>
    )
})

// 滑条行：标题与数值一行，滑条独占一行（窄面板里塞在右侧会短到没法拖）
export const PanelSliderRow = forwardRef(function PanelSliderRow({ label, value, hint, children }, ref) {
    return (
        <Box ref={ref} sx={{ pt: 0.75, '& + &': { mt: 0.25 } }}>
            <Box sx={{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between', gap: 1 }}>
                <Typography sx={{ fontSize: 13, color: PANEL_INK }}>{label}</Typography>
                {value !== undefined && (
                    <Typography sx={{ fontSize: 12, color: PANEL_MUTED, fontVariantNumeric: 'tabular-nums' }}>{value}</Typography>
                )}
            </Box>
            <Hint sx={{ mt: 0.25 }}>{hint}</Hint>
            <Box sx={{ px: 0.25, mt: -0.25 }}>{children}</Box>
        </Box>
    )
})

// 设备入口行：图标 + 名称 + 一句话说明（比一排裸按钮更好认）
export const DeviceEntry = forwardRef(function DeviceEntry({ icon, title, hint, onClick, disabled }, ref) {
    return (
        <Box ref={ref}
            onClick={disabled ? undefined : onClick}
            role="button"
            tabIndex={disabled ? -1 : 0}
            onKeyDown={(e) => { if (!disabled && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); onClick() } }}
            sx={{
                display: 'flex', alignItems: 'center', gap: 1.25, px: 1.25, py: 1,
                borderRadius: '6px', cursor: disabled ? 'not-allowed' : 'pointer',
                opacity: disabled ? 0.45 : 1,
                '&:hover': disabled ? undefined : { bgcolor: 'var(--panel-hover-bg)' },
                '&:focus-visible': { outline: '2px solid var(--panel-focus-ring)', outlineOffset: -2 },
            }}
        >
            <Box sx={{ color: PANEL_MUTED, display: 'flex', flexShrink: 0 }}>{icon}</Box>
            <Box sx={{ minWidth: 0, flex: 1 }}>
                <Typography sx={{ fontSize: 13, color: PANEL_INK, lineHeight: 1.4 }} noWrap>{title}</Typography>
                <Hint>{hint}</Hint>
            </Box>
            <ChevronRightIcon sx={{ fontSize: 18, color: 'var(--panel-chevron)', flexShrink: 0 }} />
        </Box>
    )
})

// 鼠标键等"调色板"小按钮
export const PaletteChip = forwardRef(function PaletteChip({ label, active, onClick, dense }, ref) {
    return (
        <Button ref={ref}
            size="small"
            onClick={onClick}
            variant={active ? 'contained' : 'outlined'}
            disableElevation
            sx={{
                minWidth: 0, px: dense ? 0.75 : 1, py: 0.1, fontSize: 11.5, lineHeight: 1.9,
                textTransform: 'none',
                borderColor: active ? undefined : PANEL_LINE_STRONG,
                color: active ? undefined : PANEL_INK,
            }}
        >{label}</Button>
    )
})

// 标题栏里的图标按钮（深色条上用）
export function BarIconButton({ title, onClick, children }) {
    return (
        <Tooltip title={title} arrow>
            <Box
                component="button"
                onClick={onClick}
                aria-label={title}
                sx={{
                    display: 'flex', alignItems: 'center', justifyContent: 'center',
                    p: 0.5, borderRadius: '50%', border: 0, cursor: 'pointer',
                    bgcolor: 'transparent', color: 'rgba(255,255,255,.72)',
                    '&:hover': { color: '#fff', bgcolor: 'rgba(255,255,255,.08)' },
                }}
            >{children}</Box>
        </Tooltip>
    )
}

// ── 二选一（或多选一）用分段按钮，不要用开关 ─────────────────────
// 开关的语义是"开 / 关"，而"疾跑触发＝长按还是切换""越界释放＝交替还是单点"这类是
// **在几个模式之间选一个**：两个选项都该被看见，当前生效的高亮。用 Switch 只能看到
// 当前状态、看不到另一个选项是什么，而且"关"这个词对模式选择毫无意义。
// 视觉与「参数配置 → 光标显示方式」的分段按钮保持同一套语言。
export const ChoiceToggle = forwardRef(function ChoiceToggle({ value, options, onChange, disabled, size = 'small' }, ref) {
    return (
        <ToggleButtonGroup ref={ref}
            exclusive
            size={size}
            value={value}
            onChange={(_, v) => { if (v !== null && v !== undefined) onChange(v) }}
            disabled={disabled}
            sx={{
                '& .MuiToggleButton-root': {
                    px: 1, py: 0.1, fontSize: 11.5, lineHeight: 1.9,
                    textTransform: 'none', whiteSpace: 'nowrap',
                    borderColor: PANEL_LINE, color: PANEL_MUTED,
                    '& + .MuiToggleButton-root': { borderLeftColor: PANEL_LINE },
                },
                '& .MuiToggleButton-root.Mui-selected': {
                    bgcolor: PANEL_ACCENT_BG, color: PANEL_ACCENT, fontWeight: 600,
                    '&:hover': { bgcolor: PANEL_ACCENT_BG_HOVER },
                },
            }}
        >
            {options.map((o) => (
                <ToggleButton key={String(o.value)} value={o.value}>{o.label}</ToggleButton>
            ))}
        </ToggleButtonGroup>
    )
})

// ── 弹窗外壳 ────────────────────────────────────────────────────
// 统一：标题（+状态药丸）／一行说明／内容区（浅底、统一内边距）／底栏按钮顺序。
// actions 里请按「左侧次要（恢复默认/刷新）→ 右侧主操作（保存）→ 关闭」排列，
// 主操作用 variant="contained"，其余 outlined / text。
export const DialogShell = forwardRef(function DialogShell({
    open, onClose, title, icon, titleExtra, hint, status, actions, children,
    maxWidth = 'sm', busy = false, contentSx, fullScreen = false, paperSx, zIndex,
}, ref) {
    return (
        <Dialog ref={ref}
            open={open}
            onClose={busy ? undefined : onClose}
            maxWidth={maxWidth}
            fullWidth
            fullScreen={fullScreen}
            PaperProps={{ sx: { bgcolor: PANEL_SURFACE, backgroundImage: 'none', ...paperSx } }}
            // 画布上的标记圆点/编辑浮层用的是 10⁴ 量级的手写 z-index（见 UIcomponents），
            // 比 MUI 默认的 modal(1300) 高得多。凡是"从标记编辑浮层里打开"的弹窗都要显式抬高，
            // 否则会被浮层盖住（类型下拉菜单同理，见 ConfigManager 的 MenuProps）。
            sx={zIndex ? { zIndex } : undefined}
        >
            <DialogTitle sx={{ pb: hint ? 1 : 1.5, pt: 2 }}>
                <Box sx={{ display: 'flex', alignItems: 'center', gap: 1, pr: 3, flexWrap: 'wrap' }}>
                    {icon ? <Box sx={{ display: 'flex', flexShrink: 0 }}>{icon}</Box> : null}
                    <Typography sx={{ fontSize: 15, fontWeight: 600, color: PANEL_INK, whiteSpace: 'nowrap' }}>
                        {title}
                    </Typography>
                    {/* 状态药丸紧跟标题（用户第一眼要看到"现在什么状态"），
                        标题后缀（分页/入口等附加信息）靠右，不与状态抢位置 */}
                    {status ? <StatusPill tone={status.tone || 'muted'}>{status.text}</StatusPill> : null}
                    <Box sx={{ flex: 1, minWidth: 8 }} />
                    {titleExtra}
                </Box>
                <Hint sx={{ mt: 0.5, fontSize: 11.5 }}>{hint}</Hint>
            </DialogTitle>
            <DialogContent
                dividers
                sx={{ bgcolor: PANEL_CONTENT, px: 2, py: 2, ...contentSx }}
            >
                {children}
            </DialogContent>
            <DialogActions sx={{ px: 2, py: 1.25, gap: 1 }}>
                {actions}
            </DialogActions>
        </Dialog>
    )
})

// 弹窗底栏按钮的统一样式（次要 / 主要）
export const dialogActionSx = {
    secondary: { textTransform: 'none', color: PANEL_MUTED },
    outline: { textTransform: 'none' },
    primary: { textTransform: 'none', minWidth: 88 },
}

// ── 数值输入与滑条行 ────────────────────────────────────────────
// 轮盘/手柄/参数三个弹窗的参数行以前各写一份（Grid+Paper 三栏布局，窄弹窗里滑条被压到没法拖）。
// 统一到这里：名称 + 可直接输入的数值一行、说明一行、滑条独占一行。
//
// 输入框带本地缓冲：拖滑条时 value 每帧都变，直接回填会让正在输入的光标跳动，
// 所以编辑期间显示用户输入、blur / Enter 时再提交并钳制到 [min, max]。
export const NumberField = forwardRef(function NumberField({ value, onChange, min, max, step, isFloat, unit, width, disabled, placeholder }, ref) {
    const [edit, setEdit] = useState(null);
    useEffect(() => { setEdit(null); }, [value]);
    const num = Number(value);
    const display = edit !== null ? edit : (isFloat ? num.toFixed(6) : String(Math.round(num)));

    const commit = () => {
        if (edit === null) return;
        const v = isFloat ? parseFloat(edit) : parseInt(edit, 10);
        if (!isNaN(v)) {
            const lo = min === undefined ? -Infinity : min;
            const hi = max === undefined ? Infinity : max;
            const c = Math.max(lo, Math.min(hi, v));
            onChange(isFloat ? c : Math.round(c));
        }
        setEdit(null);
    };

    return (
        <Box ref={ref} sx={{ display: 'flex', alignItems: 'baseline', gap: 0.5, flexShrink: 0 }}>
            <TextField
                size="small" value={display} disabled={disabled} placeholder={placeholder}
                onChange={(e) => setEdit(e.target.value)}
                onBlur={commit}
                onKeyDown={(e) => { if (e.key === 'Enter') commit(); }}
                inputProps={{ min, max, step: isFloat ? 'any' : step }}
                sx={{ width: width || (isFloat ? 104 : 92), '& input': { fontSize: 12, py: 0.5, textAlign: 'right' } }}
            />
            {unit ? <Typography sx={{ fontSize: 11, color: PANEL_MUTED, minWidth: 16 }}>{unit}</Typography> : null}
        </Box>
    );
})

export const NumberSliderRow = forwardRef(function NumberSliderRow({ label, hint, value, onChange, min, max, step, unit, isFloat, disabled }, ref) {
    return (
        <Box ref={ref} sx={{ pt: 0.75, '& + &': { mt: 1.25 } }}>
            <Box sx={{ display: 'flex', alignItems: 'center', gap: 1.5 }}>
                <Typography sx={{ fontSize: 13, color: PANEL_INK, flex: 1 }}>{label}</Typography>
                <NumberField value={value} onChange={onChange} min={min} max={max} step={step}
                    unit={unit} isFloat={isFloat} disabled={disabled} />
            </Box>
            <Hint sx={{ mt: 0.25 }}>{hint}</Hint>
            <Box sx={{ px: 0.25, mt: -0.25 }}>
                <Slider size="small" min={min} max={max} step={step} value={value} disabled={disabled}
                    valueLabelDisplay="auto"
                    onChange={(_, v) => onChange(isFloat ? v : Math.round(v))} />
            </Box>
        </Box>
    );
})

// ── 表单控件对（映射卡片顶部那一行）─────────────────────────────
// 左边"是哪个键"、右边"什么类型"必须长得一模一样：同样的高度 / 圆角 / 描边 / 字号 /
// 左内边距。MUI 的 outlined Button 与 outlined Select 默认并不一致——Button 无描边色
// （中性灰）、Select 用 rgba(0,0,0,.23) 且有 40px 的默认高度，并排一眼就能看出是
// 两套零件拼的。故这里成对定义，谁也别单独调。
export const FIELD_H = 36

export const FieldButton = forwardRef(function FieldButton({ onClick, children, startIcon, endIcon, disabled, sx }, ref) {
    return (
        <Button ref={ref}
            variant="outlined" fullWidth onClick={onClick} disabled={disabled}
            startIcon={startIcon} endIcon={endIcon}
            sx={{
                height: FIELD_H, justifyContent: 'flex-start', textTransform: 'none',
                px: 1.25, fontSize: 13, lineHeight: 1, borderRadius: '6px',
                borderColor: PANEL_LINE, color: PANEL_INK, bgcolor: PANEL_SURFACE,
                '& .MuiButton-startIcon': { mr: 0.75, ml: 0, '& > *:nth-of-type(1)': { fontSize: 18 } },
                '&:hover': { borderColor: 'var(--panel-field-line-hover)', bgcolor: PANEL_SURFACE },
                ...sx,
            }}
        >{children}</Button>
    )
})

export const FieldSelect = forwardRef(function FieldSelect({ value, onChange, children, menuZIndex, disabled, sx }, ref) {
    return (
        <FormControl ref={ref} fullWidth>
            <Select
                value={value} onChange={onChange} disabled={disabled}
                MenuProps={menuZIndex ? { style: { zIndex: menuZIndex } } : undefined}
                sx={{
                    height: FIELD_H, borderRadius: '6px', bgcolor: PANEL_SURFACE,
                    '& .MuiOutlinedInput-notchedOutline': { borderColor: PANEL_LINE },
                    '&:hover .MuiOutlinedInput-notchedOutline': { borderColor: 'var(--panel-field-line-hover)' },
                    '&.Mui-focused .MuiOutlinedInput-notchedOutline': { borderColor: PANEL_ACCENT, borderWidth: '1px' },
                    // 文本与「改键」按钮的左内边距一致（10px），高度撑满 34px 内容区并垂直居中
                    '& .MuiSelect-select': {
                        py: 0, pl: 1.25, pr: '30px !important', fontSize: 13,
                        height: '100%', boxSizing: 'border-box', display: 'flex', alignItems: 'center',
                    },
                    '& .MuiSelect-icon': { fontSize: 18, right: 6, color: PANEL_MUTED },
                    ...sx,
                }}
            >{children}</Select>
        </FormControl>
    )
})
