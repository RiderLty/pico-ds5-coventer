// ---------------------------------------------------------------------------
// AI 面板的文本展示辅助（纯函数，零依赖 —— 可在 node 下直接单测）
//
// 这里放的是"流式显示"的取舍规则。抽出来的原因很实际：这些规则决定每次刷新要渲染多少
// 字符，而**渲染成本是卡顿的直接来源**（DeepSeek 流得快时尤其明显）；同时它们又必须能在
// 没有浏览器的情况下被断言——所以不能留在 .jsx 里。
// ---------------------------------------------------------------------------

/** 运行中实时思考块只渲染最后这么多字符（人看的时候也只关心最新一段）。
 *  整块 pre-wrap 文本每隔一百多毫秒重排一次，上万字时会明显拖帧，所以实时必须截。 */
export const REASONING_LIVE_TAIL_CHARS = 1500;

/**
 * 计算实时思考块该渲染什么。
 * 规则：**只有流式中（live）才截尾**——历史块显示完整（已由 capReasoning 截断到 ~2000 字），
 * 这样回看时能翻到开头，而流式时渲染量有上界。
 * @param {string} text 完整文本（调用方给的是已 cap 过的）
 * @param {boolean} live 是否正在流式
 * @param {number} [max] 尾部窗口大小
 * @returns {{shown:string, trimmed:boolean, total:number}}
 */
export function liveReasoningWindow(text, live, max = REASONING_LIVE_TAIL_CHARS) {
    const full = typeof text === 'string' ? text : '';
    const limit = Number.isFinite(max) && max > 0 ? Math.floor(max) : REASONING_LIVE_TAIL_CHARS;
    if (!live || full.length <= limit) return { shown: full, trimmed: false, total: full.length };
    return { shown: `… ${full.slice(-limit)}`, trimmed: true, total: full.length };
}
