#ifndef LUA_STORE_H
#define LUA_STORE_H
#include <stdbool.h>
#include <stddef.h>
#define LUA_SCRIPT_MAX (60u * 1024u)
#define LUA_KV_SIZE (512u * 1024u)
// All functions are core1 only. KV operations are serialized by lua_script.
bool lua_store_init(void);
void lua_store_deinit(void);
int lua_store_read(void *data, size_t capacity, size_t *size);
int lua_store_save(const void *data, size_t size);
int lua_store_delete(void);
#endif
