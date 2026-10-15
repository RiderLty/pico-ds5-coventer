// ---------------------------------------------------------------------------
// 滚动跟随的**纯决策逻辑**（不碰 DOM，故可单测；回归见 scripts/test-ai-scroll.mjs）
//
// 这里踩过两次同一个坑，两处都是"能不能自动滚到最新"的现场：
//   · 正文消息列表：早先只看"离底距离" → 内容一长距离天然超阈值，被误判成"用户在看
//     历史"从而永久锁死（实测卡在离底 719px）。
//   · 实时思考预览：`pinBottom` 里写的是"离底 >60px 就不滚"，同样把"内容长高"当成
//     "用户上翻"。思考是按 120ms 批量刷新的，一次刷新长高好几行（>60px）后就**永久
//     停止跟随**——实测离底冻结在 658px 而内容还在长，界面表现就是"不能自动滚到最新"。
//
// 试过、但都不够稳的两条路（别再退回去）：
//   ✗ 拿"离底距离"直接当判据 —— 内容长高时距离天然变大，必误判（上面两个 bug）。
//   ✗ 盯 scroll 事件 + "用户意图"（滚轮/触摸）标记再决定 —— 意图标记会被
//     "回到底部就恢复跟随"那条规则抢跑（实测：滚轮上翻后 250ms 内又被拽回底部），
//     而且真实滚轮会连发几十个事件，谁先到全看调度，行为不稳定。
//
// 现在的判据只有一句话：**"这次内容变化之前，用户是不是在底部？"**
//   · 是 → 跟随，贴到新的底部；
//   · 否 → 不跟随（用户正在往上看，别抢）。
// 关键在于用**上一次记录的 scrollHeight**（内容变化之前的高度）来算离底，
// 而不是用当前 scrollHeight —— 内容自己长高就骗不到这个判据了。
// 不需要意图事件、不需要 scroll 监听，也就没有竞态。
// ---------------------------------------------------------------------------

export const FOLLOW_BOTTOM_PX = 40;    // 离底 ≤ 这个距离算"在底部"

/**
 * 内容变化后、贴底之前调用：决定"这次还要不要跟着滚到最新"。
 *
 * @param {Object} m
 * @param {number} m.prevH    **上一次内容变化时**记录的 scrollHeight（0 = 还没有记录 → 视为跟随）
 * @param {number} m.top      当前 scrollTop
 * @param {number} m.clientH  当前 clientHeight
 * @param {number} [m.bottomPx] 离底多少像素内算"在底部"
 * @returns {boolean} true = 跟随（滚到最新），false = 保持用户当前视野
 */
export function shouldFollowAfterGrowth({ prevH, top, clientH, bottomPx = FOLLOW_BOTTOM_PX }) {
    if (!prevH || !clientH) return true;          // 首次渲染 / 空容器：跟随
    const gapBeforeGrowth = prevH - top - clientH;
    return gapBeforeGrowth <= bottomPx;
}
