import React, { useRef, useEffect } from 'react';
import { Box } from '@mui/material';
import { EditorView, keymap, Decoration, placeholder as editorPlaceholder } from '@codemirror/view';
import { Compartment, EditorState, StateEffect, StateField } from '@codemirror/state';
import { basicSetup } from 'codemirror';
import { HighlightStyle, StreamLanguage, syntaxHighlighting } from '@codemirror/language';
import { tags as t } from '@lezer/highlight';
import { lua } from '@codemirror/legacy-modes/mode/lua';
import { indentWithTab } from '@codemirror/commands';
import { autocompletion } from '@codemirror/autocomplete';
import { luaCompletions } from '../utils/luaCompletions';
import { useThemeMode } from '../utils/themeStore';

// ── AI 编辑高亮：StateEffect 驱动的临时 mark 装饰 ──
const setAiHighlight = StateEffect.define();
const aiHighlightField = StateField.define({
    create: () => Decoration.none,
    update(value, tr) {
        value = value.map(tr.changes);
        for (const e of tr.effects) if (e.is(setAiHighlight)) value = e.value;
        return value;
    },
    provide: (f) => EditorView.decorations.from(f),
});
const aiMarkDeco = Decoration.mark({ class: 'cm-ai-highlight' });

// ── 暗色编辑器主题 ──────────────────────────────────────────────
// CodeMirror 的 basicSetup 自带的是浅色主题；暗色模式下如果不换，代码区会是刺眼的白块。
// 这里给出与面板同色系的暗色皮肤（背景/侧栏/选中/光标都用 CSS 变量，跟随主题开关），
// 并配一套高对比的语法高亮（默认高亮风格是给浅色底设计的，暗底上关键词几乎看不清）。
const darkEditorTheme = [
    EditorView.theme({
        '&': { backgroundColor: 'var(--panel-content)', color: 'var(--panel-ink)' },
        '.cm-content': { caretColor: 'var(--panel-ink)' },
        '.cm-cursor, .cm-dropCursor': { borderLeftColor: 'var(--panel-ink)' },
        '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection': {
            backgroundColor: 'var(--panel-accent-bg-hover)',
        },
        '.cm-gutters': {
            backgroundColor: 'var(--panel-sunken)', color: 'var(--panel-muted)',
            borderRight: `1px solid var(--panel-line)`,
        },
        '.cm-activeLine': { backgroundColor: 'rgba(255,255,255,.045)' },
        '.cm-activeLineGutter': { backgroundColor: 'transparent', color: 'var(--panel-ink)' },
        '.cm-lineNumbers .cm-gutterElement': { padding: '0 6px 0 8px' },
        '.cm-matchingBracket, &.cm-focused .cm-matchingBracket': {
            backgroundColor: 'var(--panel-accent-bg)', outline: 'none',
        },
        '.cm-tooltip': {
            backgroundColor: 'var(--panel-surface)', border: '1px solid var(--panel-line)',
            color: 'var(--panel-ink)',
        },
        '.cm-tooltip-autocomplete > ul > li[aria-selected]': {
            backgroundColor: 'var(--panel-accent-bg)', color: 'var(--panel-accent)',
        },
        '.cm-panels': { backgroundColor: 'var(--panel-sunken)', color: 'var(--panel-ink)' },
    }, { dark: true }),
    syntaxHighlighting(HighlightStyle.define([
        { tag: [t.keyword, t.moduleKeyword, t.controlKeyword], color: '#C792EA' },
        { tag: [t.string, t.special(t.string)], color: '#C3E88D' },
        { tag: [t.number, t.bool, t.null], color: '#F78C6C' },
        { tag: [t.comment, t.lineComment, t.blockComment], color: '#7C8B9E', fontStyle: 'italic' },
        { tag: [t.variableName, t.propertyName], color: '#B2CCD6' },
        { tag: [t.function(t.variableName), t.labelName], color: '#82AAFF' },
        { tag: [t.typeName, t.className, t.namespace], color: '#FFCB6B' },
        { tag: [t.operator, t.punctuation, t.bracket], color: '#89DDFF' },
        { tag: [t.definition(t.variableName), t.definition(t.propertyName)], color: '#E4E9F0' },
        { tag: [t.heading, t.strong], color: '#E4E9F0', fontWeight: 'bold' },
        { tag: t.emphasis, fontStyle: 'italic' },
        { tag: t.invalid, color: '#FF8F8A' },
    ])),
];

// ── CodeMirror 6 Lua 编辑器 ──
const CodeEditor = React.forwardRef(function CodeEditor({ value, onChange, disabled, placeholder }, ref) {
    // 跟随全局主题：亮色用 basicSetup 的默认浅色皮肤，暗色换成上面那套
    const { resolved } = useThemeMode();
    const containerRef = useRef(null);
    const viewRef = useRef(null);
    const skipSync = useRef(false);  // 我们自己的 setValue → 跳过 onChange 回传
    const editableCompartment = useRef(new Compartment()).current;  // editable 需经 Compartment 动态切换
    const themeCompartment = useRef(new Compartment()).current;     // 亮/暗皮肤同理，切主题不重建编辑器
    const onChangeRef = useRef(onChange); onChangeRef.current = onChange;

    // 初始化 & 销毁 EditorView
    useEffect(() => {
        const view = new EditorView({
            doc: value || '',
            extensions: [
                basicSetup,
                editorPlaceholder(placeholder || ''),
                StreamLanguage.define(lua),
                keymap.of([indentWithTab]),
                autocompletion({ override: [luaCompletions] }),
                aiHighlightField,
                editableCompartment.of([EditorView.editable.of(!disabled), EditorState.readOnly.of(!!disabled)]),
                themeCompartment.of(resolved === 'dark' ? darkEditorTheme : []),
                EditorView.updateListener.of((update) => {
                    if (update.docChanged && !skipSync.current) {
                        const text = update.state.doc.toString();
                        onChangeRef.current(text);
                    }
                }),
            ],
            parent: containerRef.current,
        });
        viewRef.current = view;
        return () => { clearTimeout(highlightTimer.current); viewRef.current = null; view.destroy(); };
    }, []); // 仅挂载/卸载

    // 对外能力：滚动定位到指定行并短暂高亮（AI 编辑后定位改动点用）
    const highlightTimer = useRef(null);
    React.useImperativeHandle(ref, () => ({
        scrollToLine(line, highlightMs = 3000) {
            const view = viewRef.current;
            if (!view || !line) return;
            const l = view.state.doc.line(Math.max(1, Math.min(line, view.state.doc.lines)));
            const deco = Decoration.set([aiMarkDeco.range(l.from, l.to)]);
            view.dispatch({ effects: [EditorView.scrollIntoView(l.from, { y: 'center' }), setAiHighlight.of(deco)] });
            if (highlightTimer.current) clearTimeout(highlightTimer.current);
            highlightTimer.current = setTimeout(() => {
                const v = viewRef.current;
                if (v) v.dispatch({ effects: setAiHighlight.of(Decoration.none) });
            }, highlightMs);
        },
    }), []);

    // 主题切换 → 只重配皮肤（不动文档、不重建编辑器，光标与滚动位置都保留）
    useEffect(() => {
        const view = viewRef.current;
        if (!view) return;
        view.dispatch({ effects: themeCompartment.reconfigure(resolved === 'dark' ? darkEditorTheme : []) });
    }, [resolved]);

    // 外部 disabled 变化 → 重配 editable（授权状态变化时即时切换只读）
    useEffect(() => {
        const view = viewRef.current;
        if (!view) return;
        view.dispatch({ effects: editableCompartment.reconfigure([EditorView.editable.of(!disabled), EditorState.readOnly.of(!!disabled)]) });
    }, [disabled]);

    // 外部 value 变化 → 同步进编辑器
    useEffect(() => {
        const view = viewRef.current;
        if (!view) return;
        const current = view.state.doc.toString();
        if (value !== current) {
            skipSync.current = true;
            view.dispatch({
                changes: { from: 0, to: current.length, insert: value || '' }
            });
            skipSync.current = false;
        }
    }, [value]);

    return (
        <Box
            ref={containerRef}
            sx={{
                flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column',
                '& .cm-editor': { flex: 1, minHeight: 0, outline: 'none' },
                '& .cm-scroller': { overflow: 'auto' },
                '& .cm-content': { fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace', fontSize: 13 },
                '& .cm-gutters': { fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace' },
                '& .cm-ai-highlight': { backgroundColor: 'rgba(255, 213, 0, 0.35)' },
            }}
        />
    );
});


export default CodeEditor;
