// ---------------------------------------------------------------------------
// 主题模式：亮色 / 暗色 / 自适应（跟随系统）
// ---------------------------------------------------------------------------
// 一个按钮三态循环，选择记在 localStorage（纯前端偏好，不进设备配置——它跟这台机器
// 当前的显示环境有关，换台电脑/换个手机不该被设备上的配置带着走）。
//
// 颜色本身**不在这里**：真正的色值集中在 index.css 的两组 CSS 变量里（`:root` 亮色、
// `:root[data-theme="dark"]` 暗色），本模块只负责决定把 `data-theme` 设成哪个，
// 以及通知订阅者重渲染（React 17 无 useSyncExternalStore，沿用 logStore 的订阅写法）。
import { useEffect, useState } from 'react';

export const THEME_MODES = ['light', 'dark', 'auto'];
export const THEME_LABELS = { light: '亮色', dark: '暗色', auto: '自适应' };

const STORAGE_KEY = 'ds5_theme_mode';

function systemPrefersDark() {
    try {
        return !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
    } catch {
        return false;   // 老内核没有 matchMedia：按亮色处理
    }
}

function loadMode() {
    try {
        const v = localStorage.getItem(STORAGE_KEY);
        if (THEME_MODES.includes(v)) return v;
    } catch { /* 隐私模式/禁用存储：按默认亮色 */ }
    // 默认亮色（与加这个设置之前的外观一致），想要跟随系统就切到"自适应"
    return 'light';
}

let mode = loadMode();
const listeners = new Set();

function emit() {
    for (const fn of listeners) fn();
}

/** 当前模式：'light' | 'dark' | 'auto' */
export function getThemeMode() {
    return mode;
}

/** 解析后的实际主题：auto 时跟随系统 */
export function getResolvedTheme(m = mode) {
    return m === 'auto' ? (systemPrefersDark() ? 'dark' : 'light') : m;
}

/** 把主题写到 <html data-theme>：CSS 变量与原生控件配色（color-scheme）都靠它切换 */
export function applyThemeToDocument(m = mode) {
    const resolved = getResolvedTheme(m);
    try {
        document.documentElement.setAttribute('data-theme', resolved);
        // 让浏览器把滚动条/日期选择器等原生控件也画成对应色调
        document.documentElement.style.colorScheme = resolved;
    } catch { /* SSR/极端环境忽略 */ }
    return resolved;
}

export function setThemeMode(next) {
    if (!THEME_MODES.includes(next) || next === mode) return;
    mode = next;
    try { localStorage.setItem(STORAGE_KEY, next); } catch { /* 存不下就只在本次会话生效 */ }
    applyThemeToDocument(next);
    emit();
}

/** 循环切换：亮色 → 暗色 → 自适应 → 亮色 */
export function cycleThemeMode() {
    const i = THEME_MODES.indexOf(mode);
    setThemeMode(THEME_MODES[(i + 1) % THEME_MODES.length]);
    return mode;
}

export function subscribeTheme(fn) {
    listeners.add(fn);
    return () => { listeners.delete(fn); };
}

/** React Hook：拿到当前模式、解析后的主题与切换函数 */
export function useThemeMode() {
    const [, force] = useState(0);
    useEffect(() => subscribeTheme(() => force((n) => n + 1)), []);
    return { mode, resolved: getResolvedTheme(mode), setMode: setThemeMode, cycle: cycleThemeMode };
}

/** 跟随系统变化：auto 模式下系统切换深浅色时立即生效（在 App 里挂一次） */
export function watchSystemTheme() {
    if (!window.matchMedia) return () => {};
    const mq = window.matchMedia('(prefers-color-scheme: dark)');
    const onChange = () => { if (mode === 'auto') { applyThemeToDocument(mode); emit(); } };
    if (mq.addEventListener) mq.addEventListener('change', onChange);
    else if (mq.addListener) mq.addListener(onChange);   // 老 Safari
    return () => {
        if (mq.removeEventListener) mq.removeEventListener('change', onChange);
        else if (mq.removeListener) mq.removeListener(onChange);
    };
}

// 模块加载即应用一次：App 挂载前就把 <html data-theme> 定下来，避免首帧闪白/闪黑。
// （CSS 变量到位后，第一帧渲染出来的就是正确主题。）
if (typeof document !== 'undefined') applyThemeToDocument();
