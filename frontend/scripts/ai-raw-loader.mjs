// 测试用 loader：把 Vite 专有的 `?raw` 导入换成桩模块。
// 这样 node 才能直接 import aiAgent / aiTools / aiPrompt（它们最终会 import
// '../generated/lua-api.ai.md?raw'），从而对真实的 agent 循环做集成测试。
// 只在 test-ai-agent.mjs 里通过 module.register() 注册，不参与生产构建。
//
// 环境变量 AI_RAW_STUB_DOC=<路径> 时改读该文件（用于"拿真实 17.7k 字符文档去打真端点"
// 的验证脚本）；不设时用下面这段极短桩，好让预算相关的断言能用小预算触发。
import { readFileSync } from 'node:fs';

const REAL_DOC = process.env.AI_RAW_STUB_DOC;
const STUB_DOC = 'export default "## 假 API 文档\\n- lua_print(s) —— 打印一行\\n- on_key(key, down) —— 按键回调\\n";';

export async function resolve(specifier, context, next) {
    if (specifier.endsWith('?raw')) {
        return { url: `raw-stub:${specifier}`, format: 'module', shortCircuit: true };
    }
    // 应用代码用无扩展名相对导入（'./aiClient'），那是 Vite 的解析习惯；
    // node ESM 要求显式扩展名，这里补上（找不到就原样交回，让 node 报它自己的错）。
    if (specifier.startsWith('.') && !/\.[cm]?jsx?$/i.test(specifier)) {
        for (const ext of ['.js', '.jsx']) {
            try { return await next(specifier + ext, context); } catch (e) { /* 换下一个 */ }
        }
    }
    return next(specifier, context);
}

export async function load(url, context, next) {
    if (url.startsWith('raw-stub:')) {
        let source = STUB_DOC;
        if (REAL_DOC) {
            const text = readFileSync(REAL_DOC, 'utf8');
            source = `export default ${JSON.stringify(text)};`;
        }
        return { format: 'module', shortCircuit: true, source };
    }
    return next(url, context);
}
