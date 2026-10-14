#ifndef LUA_CORE_H
#define LUA_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lua_core — Lua 引擎单例封装（由 pico-hid-mapper 的 src/lua_core.c 精简移植）。
 *
 * 模式：C→Lua 走 push+pcall，Lua→C 走 register 注册的 C 函数。
 * 所有操作纯栈传递。调试走 lua_core_debug_cb。
 */

// 外部回调（应用层必须实现；本工程在 main.c 里转发到调试串口）
extern void lua_core_debug_cb(const char *msg);

// 初始化 VM：创建 state → 打开标准库 → 加载脚本（可 NULL/0 跳过）。
// 可重复调用：VM 已存在则销毁重建（缓存的 find id 随之失效）。
bool lua_core_init(const char *script, size_t len);
void lua_core_deinit(void);
bool lua_core_is_ready(void);

// 代码加载 & C 函数注册
bool lua_core_dostring(const char *code);
bool lua_core_dobuffer(const char *code, size_t len);   // 脚本非 '\0' 结尾时（flash）必须用它
bool lua_core_register(const char *name, void *func);   // func: lua_CFunction

// 注册到全局表 `table`（表不存在则创建）：函数 / 整型常量
bool lua_core_register_in(const char *table, const char *name, void *func);
bool lua_core_table_int(const char *table, const char *key, int64_t v);

// C → Lua: push 参数（零分配栈操作）
bool lua_core_push_int(int64_t v);
bool lua_core_push_bool(bool v);
bool lua_core_push_str(const char *s, size_t len);

// 函数名 → registry 引用 id 预解析（热路径免字符串查找；失败返回 -1）
int  lua_core_find(const char *name);
void lua_core_unref(int id);

// 调用：按名称（冷路径）/ 按 id（热路径）/ 无参便捷
bool lua_core_pcall(const char *name, int nargs, int nrets);
bool lua_core_pcall_id(int id, int nargs, int nrets);
bool lua_core_call(const char *name);

// 取回返回值
bool lua_core_pop_int(int64_t *out);
bool lua_core_pop_bool(bool *out);
void lua_core_pop(int n);

// 最近一次脚本加载错误串；无错返回 ""
const char *lua_core_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_CORE_H */
