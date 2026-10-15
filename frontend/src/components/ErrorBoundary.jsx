import React from 'react';
import { PANEL_ACCENT, PANEL_DANGER, PANEL_INK, PANEL_LINE, PANEL_MUTED, PANEL_SUNKEN } from './PanelKit';

// 顶层渲染错误兜底；重新挂载不会操作设备或清空浏览器草稿。
class ErrorBoundary extends React.Component {
    constructor(props) {
        super(props);
        this.state = { error: null, resetKey: 0 };
    }

    static getDerivedStateFromError(error) {
        return { error };
    }

    componentDidCatch(error, info) {
        console.error('[ErrorBoundary] 渲染错误:', error, info && info.componentStack);
        // 挂到全局，便于在控制台 / 日志页进一步取用
        try {
            window.__lastRenderError = {
                message: String(error),
                stack: (error && error.stack) || '',
                componentStack: (info && info.componentStack) || '',
            };
        } catch { /* 存储失败不影响兜底渲染 */ }
    }

    // 恢复默认配置：跳过自动加载 + 子树重挂载（config 状态归零）
    handleReset = () => {
        this.setState(prev => ({ error: null, resetKey: prev.resetKey + 1 }));
    }

    handleCopy = () => {
        const { error } = this.state;
        const text = `${String(error)}\n${(error && error.stack) || ''}`;
        // 设备面板多为 http 非 localhost（非安全上下文），navigator.clipboard 可能不可用
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).catch(() => { });
        } else {
            const ta = document.createElement('textarea');
            ta.value = text;
            document.body.appendChild(ta);
            ta.select();
            try { document.execCommand('copy'); } catch { /* 忽略 */ }
            document.body.removeChild(ta);
        }
    }

    render() {
        const { error, resetKey } = this.state;
        if (!error) {
            // resetKey 变更 -> 整棵子树重挂载，所有组件状态（含损坏的 config）归零
            return <React.Fragment key={resetKey}>{this.props.children}</React.Fragment>;
        }
        // 崩溃页也用面板那套语言（浅底卡片 + 描边 + 字号层级），不再是另一套深色终端风。
        // 这里刻意不依赖 MUI 组件：兜底 UI 要尽可能少的前提条件才能画出来。
        const btn = {
            padding: '7px 14px', borderRadius: 6, fontSize: 13, cursor: 'pointer',
            textTransform: 'none', fontFamily: 'inherit',
        };
        return (
            <div style={{
                position: 'fixed', inset: 0, zIndex: 99999,
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                backgroundColor: 'rgba(26,31,38,.35)', padding: '16px', boxSizing: 'border-box',
            }}>
                <div style={{
                    maxWidth: '92vw', width: 680, maxHeight: '88vh', overflow: 'auto',
                    display: 'flex', flexDirection: 'column', gap: 12,
                    background: 'var(--panel-surface)', border: `1px solid ${PANEL_LINE}`, borderRadius: 10,
                    boxShadow: '0 16px 48px rgba(16,24,40,.22)', padding: 20, boxSizing: 'border-box',
                }}>
                    <div style={{ fontSize: 15, fontWeight: 600, color: PANEL_INK }}>
                        页面渲染出错
                        <span style={{
                            marginLeft: 8, padding: '1px 7px', borderRadius: 999, fontSize: 10.5,
                            color: PANEL_DANGER, background: 'var(--panel-danger-bg)', border: '1px solid var(--panel-danger-line)',
                        }}>渲染异常</span>
                    </div>
                    <div style={{ fontSize: 12, color: PANEL_MUTED, lineHeight: 1.7 }}>
                        浏览器草稿会在重新打开编辑器后恢复。也可以复制错误信息以便排查，或刷新页面重试。
                    </div>
                    <pre className="selectable" style={{
                        margin: 0, padding: 12, overflow: 'auto', maxHeight: '46vh',
                        background: PANEL_SUNKEN, border: `1px solid ${PANEL_LINE}`,
                        borderRadius: 8, fontSize: 12, lineHeight: 1.55, color: PANEL_INK,
                        whiteSpace: 'pre-wrap', wordBreak: 'break-all',
                        fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
                    }}>
                        {String(error)}
                        {'\n\n'}
                        {(error && error.stack) || '(无堆栈)'}
                    </pre>
                    <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                        <button onClick={this.handleReset} style={{
                            ...btn, border: 'none', background: PANEL_ACCENT, color: '#fff',
                        }}>
                            重新打开编辑器
                        </button>
                        <button onClick={() => window.location.reload()} style={{
                            ...btn, background: 'var(--panel-surface)', color: PANEL_INK, border: `1px solid ${PANEL_LINE}`,
                        }}>
                            重新加载
                        </button>
                        <button onClick={this.handleCopy} style={{
                            ...btn, background: 'var(--panel-surface)', color: PANEL_MUTED, border: `1px solid ${PANEL_LINE}`,
                        }}>
                            复制错误信息
                        </button>
                    </div>
                </div>
            </div>
        );
    }
}

export default ErrorBoundary;
