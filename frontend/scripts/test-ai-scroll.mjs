// ---------------------------------------------------------------------------
// 滚动跟随判据单测（零依赖，node 直接跑）：node frontend/scripts/test-ai-scroll.mjs
// 挂在 `npm run test:ai` 里。
//
// 只测纯逻辑（utils/aiScroll.js 的 shouldFollowAfterGrowth）。这个判据**踩过两次同一个
// 坑**："拿离底距离当用户意图"，于是内容一长高就被误判成"用户在看历史"，从此永久停止
// 跟随——列表卡在离底 719px、思考预览冻结在 658px（都是真机实测值，表现就是"不能自动
// 滚到最新"）。所以核心断言是：**内容持续长高时，绝不允许出现"不跟随"**。
// ---------------------------------------------------------------------------

import { readFileSync } from 'node:fs';
import { shouldFollowAfterGrowth, FOLLOW_BOTTOM_PX } from '../src/utils/aiScroll.js';

let passed = 0;
const failures = [];

function ok(cond, name, detail) {
    if (cond) { passed++; return; }
    failures.push(detail ? `${name} —— ${detail}` : name);
}
function eq(actual, expected, name) {
    ok(actual === expected, name, `期望 ${String(expected)}，实际 ${String(actual)}`);
}

const CH = 200;   // 容器可视高度

// ── 在底部（含首次渲染）→ 跟随 ──────────────────────────────────────────────
eq(shouldFollowAfterGrowth({ prevH: 0, top: 0, clientH: CH }), true, '还没有记录高度（首次渲染）→ 跟随');
eq(shouldFollowAfterGrowth({ prevH: 900, top: 700, clientH: CH }), true, '长高前贴底（离底 0）→ 跟随');
eq(shouldFollowAfterGrowth({ prevH: 900, top: 900 - CH - FOLLOW_BOTTOM_PX, clientH: CH }), true, '离底正好等于阈值 → 跟随');
eq(shouldFollowAfterGrowth({ prevH: 900, top: 900 - CH - FOLLOW_BOTTOM_PX - 1, clientH: CH }), false, '离底刚超阈值 1px → 不跟随');

// ── 用户在看历史 → 不跟随（关键：内容再长高也不许把他拽回去）────────────────
{
    let prevH = 2000;
    const top = 500;                              // 离底 1300，明显在看历史
    eq(shouldFollowAfterGrowth({ prevH, top, clientH: CH }), false, '在看历史 → 不跟随');
    // 内容继续长高（prevH 每轮更新成新高度），判据必须**持续**不跟随
    let stuck = null;
    for (let i = 0; i < 100; i++) {
        prevH += 300;
        if (shouldFollowAfterGrowth({ prevH, top, clientH: CH })) { stuck = `第 ${i} 次长高后被拽回跟随（prevH=${prevH}）`; break; }
    }
    ok(!stuck, '看历史期间连续长高 100 次都不抢', stuck || '');
}

// ── 用户自己滑回底部 → 恢复跟随 ────────────────────────────────────────────
eq(shouldFollowAfterGrowth({ prevH: 4000, top: 4000 - CH, clientH: CH }), true, '滑回底部 → 恢复跟随');

// ── 内容变矮（压缩 / 思考窗口重置）不能误判 ────────────────────────────────
{
    // 压缩前在底部：top = prevH - CH；压缩后高度变小，浏览器已把 top 钳到新底部。
    // 此时"跟不跟随"都不影响结果（怎么算都是贴底），断言结果而不是标志位。
    const prevH = 5000;
    const newH = 3000;
    const topClamped = newH - CH;
    const follow = shouldFollowAfterGrowth({ prevH, top: topClamped, clientH: CH });
    const topAfter = follow ? newH - CH : topClamped;
    eq(newH - topAfter - CH, 0, '压缩后变矮：视野仍在底部（跟随与否都不跳走）');
}

// ── **真实流式序列回归**：连续长高 + 自动贴底，一次都不许"停止跟随" ─────────
{
    let prevH = 0;
    let followed = 0;
    let bad = null;
    for (let i = 0; i < 300; i++) {
        // 每次刷新长高 0~400px（思考/正文按 120ms 批量刷新的真实量级）
        const newH = prevH + (i % 5) * 100;
        const top = Math.max(0, prevH - CH);      // 跟随状态下：位置停在"上一次的底部"
        const follow = shouldFollowAfterGrowth({ prevH, top, clientH: CH });
        if (!follow) { bad = `第 ${i} 次刷新判定为不跟随（prevH=${prevH} top=${top} newH=${newH}）`; break; }
        followed++;
        prevH = newH;
    }
    ok(!bad, '流式连续长高 300 次：从不停止跟随', bad || '');
    eq(followed, 300, '全部 300 次都跟随');
}

// ── 旧 bug 的定点复现：单次长高远超阈值也不许停止跟随（思考预览冻结 658px）──
{
    const prevH = 800;                            // 内容变化前高度
    const top = prevH - CH;                       // 变化前贴底
    // 一次刷新长高 700px（旧实现里"离底 >60px"就在这里永久锁死）
    ok(shouldFollowAfterGrowth({ prevH, top, clientH: CH }), '单次长高 700px 仍然跟随');
    // 再长高一次，此时"上一次高度"已经是新高，位置仍在上一次的底部 → 继续跟随
    ok(shouldFollowAfterGrowth({ prevH: 1500, top: 1500 - CH, clientH: CH }), '紧接着再长高一次仍然跟随');
}

// ── 非法输入安全 ────────────────────────────────────────────────────────────
eq(shouldFollowAfterGrowth({ prevH: 0, top: 0, clientH: 0 }), true, '空容器（clientH=0）→ 跟随（空操作）');

// ── 静态守卫：两处滚动容器必须走同一个判据，且不许再出现"离底距离闸门" ──────
// 上面测的是纯函数；这一节保证**组件真的在用**它。旧 bug 的写法就是组件里自己写
// `离底距离 > 60` 这种闸门——纯函数测得再对也拦不住，所以这里静态查一遍。
// 守卫本身也要被验证：先拿两段**历史写法**（真实踩过的代码）确认它抓得住，
// 否则这守卫就只是摆设。
{
    const DISTANCE_GATES = [
        /scrollHeight[^;\n]{0,80}[<>]=?\s*\d/,
        /clientHeight[^;\n]{0,80}[<>]=?\s*\d/,
        /\bgap\s*[<>]=?\s*\d/,
    ];
    const OLD_BAD = [
        'if (el.scrollHeight - el.scrollTop - el.clientHeight > 60) return;',                 // 思考预览冻结 658px 的写法
        'const gap = el.scrollHeight - el.scrollTop - el.clientHeight;\nif (gap < 40) stick = true; else if (!grew && movedUp) stick = false;',   // 列表锁死 719px 的写法
    ];
    for (const [i, bad] of OLD_BAD.entries()) {
        ok(DISTANCE_GATES.some((re) => re.test(bad)), `静态守卫能抓到历史写法 #${i + 1}`, bad.split('\n')[0].slice(0, 60));
    }

    for (const f of ['../src/components/ai/AiChatPanel.jsx', '../src/components/ai/AiMessageList.jsx']) {
        const src = readFileSync(new URL(f, import.meta.url), 'utf8');
        ok(src.includes('shouldFollowAfterGrowth('), `${f} 使用 shouldFollowAfterGrowth()`);
        const hit = DISTANCE_GATES.map((re) => re.exec(src)).find(Boolean);
        ok(!hit, `${f} 没有"离底距离闸门"（不许用距离判断用户是否上翻）`, hit ? `发现：${hit[0].trim().slice(0, 60)}` : '');
    }
}

// ── 结果 ────────────────────────────────────────────────────────────────────
if (failures.length) {
    console.error(`[ai-scroll] 失败 ${failures.length} 项（通过 ${passed} 项）：`);
    for (const f of failures) console.error('   ✗ ' + f);
    process.exit(1);
}
console.log(`[ai-scroll] OK：${passed} 项断言全部通过（贴底才跟随 / 看历史不抢 / 长高绝不误判）`);
