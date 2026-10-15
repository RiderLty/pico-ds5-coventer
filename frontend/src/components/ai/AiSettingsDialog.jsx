// ---------------------------------------------------------------------------
// AI 设置对话框：API 地址 / 模型 / API Key / 温度 / 历史轮数 + 测试连接
// ---------------------------------------------------------------------------
import React, { useState, useEffect } from 'react';
import { Button, IconButton, Box, Stack, TextField, InputAdornment, CircularProgress, Checkbox, FormControlLabel } from '@mui/material';
import VisibilityIcon from '@mui/icons-material/Visibility';
import VisibilityOffIcon from '@mui/icons-material/VisibilityOff';
import { loadAiCfg, saveAiCfg, testAiConnection } from '../../utils/aiSettings';
import {
    dialogActionSx, DialogShell, Notice, NumberField, NumberSliderRow,
    PaletteChip, PanelGroup, PanelRow,
} from '../PanelKit';

const MODEL_SUGGESTIONS = ['deepseek-flash'];

export default function AiSettingsDialog({ open, onClose }) {
    const [cfg, setCfg] = useState(loadAiCfg());
    const [showKey, setShowKey] = useState(false);
    const [testing, setTesting] = useState(false);
    const [testResult, setTestResult] = useState(null);   // {ok, message, latencyMs}
    const [saving, setSaving] = useState(false);
    const [saveError, setSaveError] = useState('');

    useEffect(() => {
        if (open) {
            setCfg(loadAiCfg());
            setTestResult(null);
        }
    }, [open]);

    const set = (k) => (e) => setCfg((c) => ({ ...c, [k]: e.target.value }));

    const handleSave = async () => {
        setSaving(true);
        setSaveError('');
        const ok = await saveAiCfg(cfg);
        setSaving(false);
        if (ok) {
            onClose();
        } else {
            setSaveError('保存到浏览器失败，配置只在当前页面生效，请检查浏览器存储权限');
        }
    };

    const handleTest = async () => {
        setTesting(true);
        setTestResult(null);
        const r = await testAiConnection(cfg);
        setTestResult(r);
        setTesting(false);
    };

    // 标题右侧状态：保存中 / 测试中 / 测试结果
    const status = saving ? { text: '保存中…' }
        : testing ? { text: '测试中…' }
        : (testResult ? { text: testResult.message, tone: testResult.ok ? 'accent' : 'warn' } : null);

    return (
        <DialogShell
            open={open}
            onClose={onClose}
            title="AI 助手设置"
            hint="面板里的 AI 助手用这个 OpenAI 兼容端点直连（浏览器发起）。Key 仅存此浏览器，不写入设备，也不包含在导出的 Lua 文件中。"
            status={status}
            busy={saving}
            actions={<>
                <Button onClick={onClose} sx={dialogActionSx.secondary}>取消</Button>
                <Box sx={{ flex: 1 }} />
                <Button variant="outlined" onClick={handleTest}
                    disabled={testing || !cfg.apiKey || !cfg.baseUrl} sx={{ ...dialogActionSx.outline, minWidth: 96 }}>
                    {testing ? <CircularProgress size={14} /> : '测试连接'}
                </Button>
                <Button variant="contained" disableElevation onClick={handleSave} disabled={saving}
                    sx={dialogActionSx.primary}>
                    {saving ? <CircularProgress size={14} /> : '保存'}
                </Button>
            </>}
        >
            <Box sx={{ display: 'flex', flexDirection: 'column', gap: 1.5 }}>
                <PanelGroup title="接口" hint="OpenAI 兼容的 /chat/completions 端点">
                    <TextField size="small" fullWidth label="API 地址（OpenAI 兼容）"
                        placeholder="https://api.deepseek.com"
                        value={cfg.baseUrl} onChange={set('baseUrl')}
                        helperText="需支持浏览器跨域直连的端点" />
                    <Box sx={{ mt: 1 }}>
                        <TextField size="small" fullWidth label="模型"
                            value={cfg.model} onChange={set('model')} />
                        <Stack direction="row" spacing={0.5} sx={{ mt: 0.75 }}>
                            {MODEL_SUGGESTIONS.map((m) => (
                                <PaletteChip key={m} label={m} dense
                                    onClick={() => setCfg((c) => ({ ...c, model: m }))} />
                            ))}
                        </Stack>
                    </Box>
                    <Box sx={{ mt: 1 }}>
                        <TextField size="small" fullWidth label="API Key"
                            type={showKey ? 'text' : 'password'}
                            value={cfg.apiKey} onChange={set('apiKey')}
                            autoComplete="off"
                            InputProps={{
                                endAdornment: (
                                    <InputAdornment position="end">
                                        <IconButton size="small" title={showKey ? '隐藏' : '显示'}
                                            onClick={() => setShowKey((s) => !s)}>
                                            {showKey ? <VisibilityOffIcon fontSize="small" /> : <VisibilityIcon fontSize="small" />}
                                        </IconButton>
                                    </InputAdornment>
                                ),
                            }} />
                    </Box>
                    {testResult && (
                        <Notice sx={{ mt: 1 }} tone={testResult.ok ? 'accent' : 'warn'}>
                            {testResult.message}{testResult.ok && testResult.latencyMs ? ` · ${testResult.latencyMs}ms` : ''}
                        </Notice>
                    )}
                    {saveError && <Notice sx={{ mt: 1 }} tone="warn">{saveError}</Notice>}
                </PanelGroup>

                <FormControlLabel control={<Checkbox checked={cfg.stream !== false}
                    onChange={(e) => setCfg((c) => ({ ...c, stream: e.target.checked }))} />} label="流式回复（端点不支持时可关闭）" />
                <FormControlLabel control={<Checkbox checked={!!cfg.disableThinking}
                    onChange={(e) => setCfg((c) => ({ ...c, disableThinking: e.target.checked }))} />} label="关闭模型思考模式（仅支持 thinking 参数的端点）" />
                <PanelGroup title="生成参数" hint="温度和历史上限只影响 AI 助手的回复" dense>
                    <NumberSliderRow
                        label="温度（创造性，低 = 稳定）"
                        min={0} max={1} step={0.1} isFloat
                        value={cfg.temperature}
                        onChange={(v) => setCfg((c) => ({ ...c, temperature: v }))} />
                    <PanelRow
                        label="模型上下文窗口（token）"
                        hint="填写模型声明支持的上下文容量，包含输入、系统提示词、工具表和输出。达到 80% 时开始警告并自动压缩较早记录；默认 1M 是容量声明，请按供应商实际支持值填写">
                        <NumberField isFloat={false} min={8000} max={2000000} step={1000}
                            value={cfg.contextBudgetTokens}
                            onChange={(v) => setCfg((c) => ({
                                ...c,
                                contextBudgetTokens: v,
                                requestBudgetTokens: c.requestBudgetAuto ? v : c.requestBudgetTokens,
                            }))} />
                    </PanelRow>
                    <PanelRow
                        label="每轮输入上限（token）"
                        hint="默认跟随模型窗口；关闭自动后可设更小值来控制单次请求体、延迟和费用。到此额度 80% 时提前压缩，任务目标完整保留">
                        <Stack direction="row" spacing={0.5} alignItems="center">
                            <FormControlLabel
                                sx={{ m: 0, '& .MuiFormControlLabel-label': { fontSize: 11, color: 'var(--panel-muted)' } }}
                                control={<Checkbox size="small" checked={cfg.requestBudgetAuto !== false}
                                    onChange={(e) => setCfg((c) => ({
                                        ...c,
                                        requestBudgetAuto: e.target.checked,
                                        requestBudgetTokens: e.target.checked ? c.contextBudgetTokens : c.requestBudgetTokens,
                                    }))} />}
                                label="跟随" />
                            <NumberField isFloat={false} min={8000} max={2000000} step={1000}
                                value={cfg.requestBudgetTokens}
                                disabled={cfg.requestBudgetAuto !== false}
                                onChange={(v) => setCfg((c) => ({ ...c, requestBudgetTokens: v, requestBudgetAuto: false }))} />
                        </Stack>
                    </PanelRow>
                    <PanelRow
                        label="单次任务轮数上限"
                        hint="模型最多来回几轮工具调用；正常任务 3~9 轮，超过它会强制收口总结">
                        <NumberField isFloat={false} min={2} max={40}
                            value={cfg.maxRounds}
                            onChange={(v) => setCfg((c) => ({ ...c, maxRounds: v }))} />
                    </PanelRow>
                    <PanelRow
                        label="单次任务 token 闸"
                        hint="累计输入（含缓存命中，缺 usage 时估算）即将超过这个数就停下（防跑飞）。按你的额度习惯设">
                        <NumberField isFloat={false} min={10000} max={2000000} step={10000}
                            value={cfg.taskBudgetTokens}
                            onChange={(v) => setCfg((c) => ({ ...c, taskBudgetTokens: v }))} />
                    </PanelRow>
                    <PanelRow
                        label="回复长度上限（0 = 不限制）"
                        hint="0 不发送输出上限，但上下文会按窗口大小预留输出空间（最大 8192 token）；推理模型的思维链也占这个额度，设小了会出现「思考完却什么都没输出」。要限制就设 8192 以上">
                        <NumberField isFloat={false} min={0} max={32768} step={256}
                            value={cfg.maxTokens}
                            onChange={(v) => setCfg((c) => ({ ...c, maxTokens: v }))} />
                    </PanelRow>
                </PanelGroup>
            </Box>
        </DialogShell>
    );
}
