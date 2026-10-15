// ---------------------------------------------------------------------------
// 浏览器端"把文本存成文件"的统一出口。
//
// 为什么不能直接用 <a download> + blob: URL（历史上三处导出各自手写、又各自失效）：
//   · 设备面板是明文 HTTP（非安全上下文），showSaveFilePicker / navigator.share /
//     navigator.clipboard 这些**仅安全上下文**的 API 一律不可用，不能拿它们当替代；
//   · iOS「添加到主屏幕」的独立窗口（standalone PWA）里，blob: URL 的 a.download
//     会**静默失败**——无报错、无下载提示，同一代码在 Safari 普通标签页里却正常
//     （WebKit 已知平台限制，bug 263608 / 275288，Apple 归为 Safari UI 问题）。
//     症状恰好是"导入可用（file input 在独立窗口正常）、导出点了没反应"；
//   · iOS Safari 普通标签页还有另一层：text/plain / application/json 这类
//     **可显示类型**不会触发下载，而是被"打开"在当前页（同 bug 263608）。
//
// 统一修法：blob 一律重包成 application/octet-stream——不可显示的类型没有"打开"
// 分支，只能走下载通道；独立窗口下实测有效（263608 现场确认）。文件名仍由
// download 属性决定，文件内容不受 type 影响。
// ---------------------------------------------------------------------------

// 是否运行在主屏独立窗口里（iOS 用 navigator.standalone 标记，其余读 media query）
export function isStandaloneDisplay() {
    return (typeof window.matchMedia === 'function'
        && window.matchMedia('(display-mode: standalone)').matches)
        || window.navigator?.standalone === true;
}

// 把文本下载为文件。仅依赖非安全上下文可用的 API，任何入口（直连 HTTP / vPointer
// 转发 / 桌面 / 手机浏览器 / 主屏独立窗口）都走这一条路。
export function downloadTextFile(filename, text) {
    const blob = new Blob([text], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
}
