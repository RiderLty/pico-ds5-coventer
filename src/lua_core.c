/*
 * Lua 引擎单例封装实现（由 pico-hid-mapper 的 src/lua_core.c 精简移植）。
 *
 * 与 mapper 版的差异：计步看门狗的墙钟直接用 Pico SDK 的 time_us_64()
 * （mapper 经 core_get_time_us() 间接取）。
 *
 * C→Lua:  push_int / push_bool → pcall(name, nargs, nrets) → pop_* 取值
 * Lua→C:  register("name", func) 暴露 C 函数给 Lua 直接调用
 */
#define LUA_CORE_DEBUG  // 注释掉以关闭调试输出

#include "lua_core.h"
#include "pico/time.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

//--------------------------------------------------------------------+
// 调试输出
//--------------------------------------------------------------------+

#ifdef LUA_CORE_DEBUG
static void __debugf(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lua_core_debug_cb(buf);
}
#define debugf __debugf
#else
#define debugf(...) ((void)0)
#endif

//--------------------------------------------------------------------+
// 单例状态
//--------------------------------------------------------------------+

static lua_State *g_L = NULL;

//--------------------------------------------------------------------+
// 执行步数看门狗（防脚本死循环拖垮实时性）
//
// 脚本顶层代码和回调全部在 core1 USB host 线程同步执行 —— 一个
// `while true do end` 会阻塞 tuh_task。机制：LUA_MASKCOUNT 钩子按固定
// 指令数轮询，每次轮询用墙钟判定"本次进入 VM"是否超预算——未超时直接
// return（Lua 自动重置计数继续执行），超时才摘钩子抛错。pcall 捕获后
// 本次调用返回 false，VM 与固件都继续存活。
//--------------------------------------------------------------------+
#define LUA_STEP_POLL_COUNT  20000     // 钩子轮询间隔（条指令）
#define LUA_STEP_BUDGET_US   10000     // 单次进入 VM 的墙钟预算（µs）

static uint64_t g_step_t0;             // 本次进入 VM 的时刻（arm 时采样）

static void _step_limit_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (time_us_64() - g_step_t0 <= LUA_STEP_BUDGET_US)
        return;                        // 未超时：重置轮询计数，继续执行
    lua_sethook(L, NULL, 0, 0);   // 超时：先摘钩子，避免错误处理路径再次触发
    luaL_error(L, "script exceeded %dus step budget (infinite loop?)",
               LUA_STEP_BUDGET_US);
}

static inline void arm_step_limit(void) {
    g_step_t0 = time_us_64();
    lua_sethook(g_L, _step_limit_hook, LUA_MASKCOUNT, LUA_STEP_POLL_COUNT);
}
static inline void disarm_step_limit(void) {
    lua_sethook(g_L, NULL, 0, 0);
}

// 最近一次脚本加载错误串（dostring/dobuffer 失败时填充，成功清空）
static char g_last_err[160] = {0};

//--------------------------------------------------------------------+
// 生命周期
//--------------------------------------------------------------------+

bool lua_core_init(const char *script, size_t len) {
    // 已初始化 → 销毁重建（调用者须重新 lua_core_find 获取新 id）
    if (g_L) {
        debugf("[lua_core] re-init: destroying old VM...");
        lua_core_deinit();
    }

    g_L = luaL_newstate();
    if (!g_L) {
        snprintf(g_last_err, sizeof(g_last_err), "out of memory creating Lua VM");
        debugf("[lua_core] luaL_newstate failed");
        return false;
    }

    luaL_openlibs(g_L);
    g_last_err[0] = '\0';

    // 加载外部传入的脚本
    if (script && len > 0) {
        if (!lua_core_dobuffer(script, len)) {
            debugf("[lua_core] script load failed");
        }
    }

    debugf("[lua_core] initialized");
    return true;
}

void lua_core_deinit(void) {
    if (g_L) {
        lua_close(g_L);
        g_L = NULL;
    }
}

bool lua_core_is_ready(void) {
    return g_L != NULL;
}

//--------------------------------------------------------------------+
// 代码加载 & 注册
//--------------------------------------------------------------------+

bool lua_core_dostring(const char *code) {
    if (!g_L || !code) return false;
    arm_step_limit();   // 顶层代码同样可能死循环
    if (luaL_dostring(g_L, code) != LUA_OK) {
        disarm_step_limit();
        const char *msg = lua_tostring(g_L, -1);
        snprintf(g_last_err, sizeof(g_last_err), "%s", msg ? msg : "(non-string error)");
        debugf("[lua_core] dostring error: %s", g_last_err);
        lua_pop(g_L, 1);
        return false;
    }
    disarm_step_limit();
    g_last_err[0] = '\0';
    return true;
}

// 长度感知加载：脚本非 '\0' 结尾时（如 flash 常量后跟填充）必须用它
bool lua_core_dobuffer(const char *code, size_t len) {
    if (!g_L || !code) return false;
    arm_step_limit();
    if (luaL_loadbuffer(g_L, code, len, "script") != LUA_OK ||
        lua_pcall(g_L, 0, LUA_MULTRET, 0) != LUA_OK) {
        disarm_step_limit();
        const char *msg = lua_tostring(g_L, -1);
        snprintf(g_last_err, sizeof(g_last_err), "%s", msg ? msg : "(non-string error)");
        debugf("[lua_core] dobuffer error: %s", g_last_err);
        lua_pop(g_L, 1);
        return false;
    }
    disarm_step_limit();
    g_last_err[0] = '\0';
    return true;
}

bool lua_core_register(const char *name, void *func) {
    if (!g_L || !name || !func) return false;
    lua_register(g_L, name, (lua_CFunction)func);
    return true;
}

bool lua_core_register_in(const char *table, const char *name, void *func) {
    if (!g_L || !table || !name || !func) return false;
    lua_getglobal(g_L, table);
    if (!lua_istable(g_L, -1)) {
        lua_pop(g_L, 1);
        lua_newtable(g_L);
    }
    lua_pushcfunction(g_L, (lua_CFunction)func);
    lua_setfield(g_L, -2, name);
    lua_setglobal(g_L, table);
    return true;
}

bool lua_core_table_int(const char *table, const char *key, int64_t v) {
    if (!g_L || !table || !key) return false;
    lua_getglobal(g_L, table);
    if (!lua_istable(g_L, -1)) {
        lua_pop(g_L, 1);
        lua_newtable(g_L);
    }
    lua_pushinteger(g_L, v);
    lua_setfield(g_L, -2, key);
    lua_setglobal(g_L, table);
    return true;
}

//--------------------------------------------------------------------+
// C → Lua: 函数名 → id 预解析
//--------------------------------------------------------------------+

int lua_core_find(const char *name) {
    if (!g_L || !name) return -1;

    lua_getglobal(g_L, name);                  // push function from _G
    if (!lua_isfunction(g_L, -1)) {
        lua_pop(g_L, 1);
        return -1;
    }
    // luaL_ref: pop from stack, store in registry at free integer slot, return index
    int ref = luaL_ref(g_L, LUA_REGISTRYINDEX);
    return ref;
}

void lua_core_unref(int id) {
    if (g_L && id >= 0) {
        luaL_unref(g_L, LUA_REGISTRYINDEX, id);
    }
}

//--------------------------------------------------------------------+
// C → Lua: push 参数（零分配栈操作）
//--------------------------------------------------------------------+

bool lua_core_checkstack(int extra) {
    return g_L && extra >= 0 && lua_checkstack(g_L, extra);
}

bool lua_core_push_int(int64_t v) {
    if (!g_L) return false;
    lua_pushinteger(g_L, v);
    return true;
}

bool lua_core_push_bool(bool v) {
    if (!g_L) return false;
    lua_pushboolean(g_L, (int)v);
    return true;
}

bool lua_core_push_str(const char *s, size_t len) {
    if (!g_L || !s) return false;
    lua_pushlstring(g_L, s, len);
    return true;
}

//--------------------------------------------------------------------+
// C → Lua: pcall（通用）
//--------------------------------------------------------------------+

bool lua_core_pcall(const char *name, int nargs, int nrets) {
    if (!g_L || !name) return false;

    // 栈: [args...] ← 调用者已 push；需要: [func][args...]
    lua_getglobal(g_L, name);                    // push function
    if (!lua_isfunction(g_L, -1)) {
        debugf("[lua_core] pcall('%s'): not a function", name);
        lua_pop(g_L, nargs + 1);
        return false;
    }

    if (nargs > 0) {
        lua_insert(g_L, -(nargs + 1));
    }

    arm_step_limit();
    int status = lua_pcall(g_L, nargs, nrets, 0);
    disarm_step_limit();
    if (status != LUA_OK) {
        debugf("[lua_core] pcall('%s') error: %s", name, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }
    // 成功: 栈顶有 nrets 个返回值，留给调用者 pop
    return true;
}

bool lua_core_call(const char *name) {
    return lua_core_pcall(name, 0, 0);
}

bool lua_core_pcall_id(int id, int nargs, int nrets) {
    if (!g_L || id < 0) return false;

    lua_rawgeti(g_L, LUA_REGISTRYINDEX, id);    // push function（整数键 O(1)）
    if (!lua_isfunction(g_L, -1)) {
        debugf("[lua_core] pcall_id(%d): invalid ref", id);
        lua_pop(g_L, nargs + 1);
        return false;
    }

    if (nargs > 0) {
        lua_insert(g_L, -(nargs + 1));
    }

    arm_step_limit();
    int status = lua_pcall(g_L, nargs, nrets, 0);
    disarm_step_limit();
    if (status != LUA_OK) {
        debugf("[lua_core] pcall_id(%d) error: %s", id, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

//--------------------------------------------------------------------+
// C → Lua: pop 返回值
//--------------------------------------------------------------------+

bool lua_core_pop_int(int64_t *out) {
    if (!g_L) return false;
    if (out) {
        if (lua_isinteger(g_L, -1))
            *out = lua_tointeger(g_L, -1);
        else if (lua_isnumber(g_L, -1))
            *out = (int64_t)lua_tonumber(g_L, -1);
        else
            *out = 0;
    }
    lua_pop(g_L, 1);
    return true;
}

bool lua_core_pop_bool(bool *out) {
    if (!g_L) return false;
    if (out) *out = (bool)lua_toboolean(g_L, -1);
    lua_pop(g_L, 1);
    return true;
}

void lua_core_pop(int n) {
    if (g_L && n > 0) lua_pop(g_L, n);
}

//--------------------------------------------------------------------+
// 诊断
//--------------------------------------------------------------------+

const char *lua_core_last_error(void) {
    return g_last_err;
}
