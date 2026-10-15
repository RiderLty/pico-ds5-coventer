// AI preferences are browser-local; they never write to the device KV.
import { contextCapacity, DEFAULT_REQUEST_BUDGET_TOKENS } from './aiContext';
const STORAGE_KEY = 'ds5_ai_cfg_v1';

const DEFAULT_CFG = {
    baseUrl: 'https://api.deepseek.com',
    model: 'deepseek-flash',
    apiKey: '',
    temperature: 0.2,
    contextBudgetTokens: 1000000,
    requestBudgetTokens: DEFAULT_REQUEST_BUDGET_TOKENS,
    requestBudgetAuto: true,
    maxRounds: 16,
    taskBudgetTokens: 150000,
    maxTokens: 0,
};

let cache = null;
let inited = false;

export function normalizeAiCfg(cfg) {
    const merged = { ...DEFAULT_CFG, ...(cfg || {}) };
    merged.contextBudgetTokens = contextCapacity(merged.contextBudgetTokens);
    const hasRequestBudgetAuto = !!cfg && Object.prototype.hasOwnProperty.call(cfg, 'requestBudgetAuto');
    const legacyRequestBudget = Number(cfg?.requestBudgetTokens);
    // 旧版本默认 128K 没有“跟随”字段；只迁移这个默认值，保留用户主动设置的其他上限。
    merged.requestBudgetAuto = hasRequestBudgetAuto
        ? merged.requestBudgetAuto !== false
        : (!Number.isFinite(legacyRequestBudget) || legacyRequestBudget === 128000);
    for (const key of ['requestBudgetTokens', 'maxRounds', 'taskBudgetTokens', 'maxTokens', 'temperature']) {
        const n = Number(merged[key]);
        merged[key] = Number.isFinite(n) && n >= 0 && (n > 0 || key === 'maxTokens' || key === 'temperature')
            ? n : DEFAULT_CFG[key];
    }
    if (merged.requestBudgetAuto) merged.requestBudgetTokens = merged.contextBudgetTokens;
    merged.baseUrl = (merged.baseUrl || DEFAULT_CFG.baseUrl).replace(/\/+$/, '');
    return merged;
}

export function loadAiCfg() {
    if (!inited) {
        inited = true;
        try { cache = JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null'); } catch { /* private mode */ }
    }
    return normalizeAiCfg(cache);
}

export async function saveAiCfg(cfg) {
    cache = normalizeAiCfg(cfg);
    inited = true;
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(cache)); return true; }
    catch { return false; }
}

export async function initAiCfg() { loadAiCfg(); }

// 拼请求 URL：兼容 baseUrl 带/不带 /v1、带/不带 /chat/completions 的写法
export function buildChatUrl(baseUrl) {
    const base = (baseUrl || DEFAULT_CFG.baseUrl).replace(/\/+$/, '');
    if (base.endsWith('/chat/completions')) return base;
    return `${base}/chat/completions`;
}

// 测试连接：发一个 max_tokens=1 的最小请求，返回 {ok, status, message, latencyMs}
export async function testAiConnection(cfg) {
    const t0 = Date.now();
    try {
        const r = await fetch(buildChatUrl(cfg.baseUrl), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                Authorization: `Bearer ${cfg.apiKey}`,
            },
            body: JSON.stringify({
                model: cfg.model,
                messages: [{ role: 'user', content: 'hi' }],
                max_tokens: 1,
                stream: false,
            }),
        });
        const latencyMs = Date.now() - t0;
        if (r.ok) return { ok: true, status: r.status, message: '连接成功', latencyMs };
        const text = await r.text().catch(() => '');
        let msg = `HTTP ${r.status}`;
        if (r.status === 401 || r.status === 403) msg += '：API Key 无效或无权限';
        else if (r.status === 429) msg += '：请求过快或额度不足';
        else if (r.status >= 500) msg += '：服务端错误';
        if (text) msg += ` · ${text.slice(0, 120)}`;
        return { ok: false, status: r.status, message: msg, latencyMs };
    } catch (e) {
        return {
            ok: false, status: 0,
            message: '网络错误（检查地址是否可达、该端点是否支持浏览器跨域直连）',
            latencyMs: Date.now() - t0,
        };
    }
}
