// ---------------------------------------------------------------------------
// Lua 脚本的浏览器本地脚本库（localStorage）。
// 设备上只能存一份板载脚本；这里给浏览器侧一个多份存档，供「导入」面板
// 一键应用 / 下载。与 LuaEditorDialog 的断电备份（'lua_backup'）互不相干：
// 备份是"最后一次加载到内存的内容"（自动、只留一份），这里是用户主动
// 命名保存的多份存档。
// 注意：面板可能跑在明文 HTTP（非安全上下文），只能用 localStorage 这类
// 无安全上下文要求的 API；所有读写都包 try/catch（隐私模式/配额满可能抛错）。
// ---------------------------------------------------------------------------
const LUA_LIB_KEY = 'ds5_lua_library_v1';   // { [name]: { name, script, byteLen, timestamp } }
const LAST_NAME_KEY = 'ds5_lua_export_name';

// 脚本库变更通知：左侧「脚本管理」面板订阅它来实时刷新列表。
// 这样从「导出」面板保存、或在别的入口删除后，列表不用手动刷新就能跟上。
export const LUA_LIBRARY_EVENT = 'ds5-lua-library-changed';
function notifyLibraryChanged() {
    try { window.dispatchEvent(new CustomEvent(LUA_LIBRARY_EVENT)); } catch (e) { /* 非浏览器环境忽略 */ }
}

function readMap() {
    try {
        return JSON.parse(localStorage.getItem(LUA_LIB_KEY) || '{}') || {};
    } catch (e) {
        return {};
    }
}

function writeMap(map) {
    try {
        localStorage.setItem(LUA_LIB_KEY, JSON.stringify(map));
    } catch (e) {
        // 5MB 配额用尽（单份脚本 ≤60 KiB，正常要几十份才会到）；或隐私模式直接禁写
        throw new Error('浏览器存储空间不足，保存失败（可删掉几份不用的脚本后重试）');
    }
}

// 全部存档，按保存时间倒序
export function listLuaScripts() {
    return Object.values(readMap()).sort((a, b) => b.timestamp - a.timestamp);
}

export function hasLuaScript(name) {
    return !!readMap()[String(name || '').trim()];
}

// 保存（同名覆盖），返回是否覆盖了同名存档。名字为空抛错。
export function saveLuaScript(name, script) {
    const trimmed = String(name || '').trim();
    if (!trimmed) throw new Error('请先填写脚本名称');
    const map = readMap();
    const existed = !!map[trimmed];
    map[trimmed] = {
        name: trimmed,
        script,
        byteLen: new TextEncoder().encode(script).length,
        timestamp: Date.now(),
    };
    writeMap(map);
    try { localStorage.setItem(LAST_NAME_KEY, trimmed); } catch (e) { /* 仅记忆，失败无所谓 */ }
    notifyLibraryChanged();
    return existed;
}

export function deleteLuaScript(name) {
    const map = readMap();
    delete map[name];
    try {
        localStorage.setItem(LUA_LIB_KEY, JSON.stringify(map));
    } catch (e) { /* 删除只会变小，写失败忽略 */ }
    notifyLibraryChanged();
}

// 「导出」面板的名称输入框预填：记住上次用的名字
export function lastExportName() {
    try { return localStorage.getItem(LAST_NAME_KEY) || ''; } catch (e) { return ''; }
}
