// ---------------------------------------------------------------------------
// PICO 设备日志全局存储
// 协议 PRINT 收到的日志行统一 push 到全局数组 picoLogs；组件通过 usePicoLogs()
// 订阅实时更新（React 17，无 useSyncExternalStore，故自建订阅通知）。
// ---------------------------------------------------------------------------
import { useEffect, useState } from 'react';
import { downloadTextFile } from './download';

const MAX_LOGS = 2000;            // 上限：超出丢弃最旧，防止无限增长

export const picoLogs = [];       // 全局日志数组（每个元素为一行字符串）

const listeners = new Set();
function emit() {
    for (const fn of listeners) fn();
}

// 追加一行日志（由设备 PRINT 通道调用）
export function pushLog(line) {
    picoLogs.push(line);
    if (picoLogs.length > MAX_LOGS) {
        picoLogs.splice(0, picoLogs.length - MAX_LOGS);
    }
    emit();
}

// 清空日志
export function clearLogs() {
    picoLogs.length = 0;
    emit();
}

// 导出日志行为 .log 文件（文件名：basename.日期_时间.log，精确到秒）
export function exportLogLines(lines, basename = 'pico-ds5') {
    if (lines.length === 0) return;
    const d = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    const fname = `${basename}.${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
        + `_${pad(d.getHours())}-${pad(d.getMinutes())}-${pad(d.getSeconds())}.log`;
    // 走统一出口：iOS 主屏独立窗口下 blob 下载会静默失败，见 utils/download.js
    downloadTextFile(fname, lines.join('\n') + '\n');
}

// 订阅日志变化，返回取消订阅函数
export function subscribeLogs(fn) {
    listeners.add(fn);
    return () => { listeners.delete(fn); };
}

// React Hook：返回实时日志数组，并在更新时触发订阅组件重渲染
export function usePicoLogs() {
    const [, force] = useState(0);
    useEffect(() => subscribeLogs(() => force((n) => n + 1)), []);
    return picoLogs;
}
