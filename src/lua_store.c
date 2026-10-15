#include "lua_store.h"
#include "kvstore.h"
#include "kvstore_logkvs.h"
#include "blockdevice/flash.h"
#include "pico.h"

static const char script_key[] = "lua.script";
static kvs_t *s_kv;
// The staging device borrows this device; it does not free it.
static blockdevice_t *s_flash;

void lua_store_deinit(void) {
    kvs_assign(NULL);
    if (s_kv) kvs_logkvs_free(s_kv);
    if (s_flash) blockdevice_flash_free(s_flash);
    s_kv = NULL;
    s_flash = NULL;
}

bool lua_store_init(void) {
    _Static_assert(PICO_FLASH_SIZE_BYTES > LUA_KV_SIZE, "KV partition exceeds Flash");
    if (s_kv) return true;
    blockdevice_t *bd = blockdevice_flash_create(PICO_FLASH_SIZE_BYTES - LUA_KV_SIZE, LUA_KV_SIZE);
    if (!bd) return false;
    s_kv = kvs_logkvs_create(bd);
    if (!s_kv) { blockdevice_flash_free(bd); return false; }
    s_flash = bd;
    kvs_assign(s_kv);
    return true;
}

int lua_store_read(void *data, size_t capacity, size_t *size) {
    if (!s_kv) return KVSTORE_ERROR_FAILED_OPERATION;
    kvs_info_t info;
    int rc = s_kv->get_info(s_kv, script_key, &info);
    if (rc) return rc;
    if (!info.size || info.size > capacity) return KVSTORE_ERROR_INVALID_SIZE;
    return kvs_get(script_key, data, capacity, size);
}

int lua_store_save(const void *data, size_t size) {
    if (!s_kv) return KVSTORE_ERROR_FAILED_OPERATION;
    if (!size || size > LUA_SCRIPT_MAX) return KVSTORE_ERROR_INVALID_SIZE;
    return kvs_set(script_key, data, size);
}

int lua_store_delete(void) {
    if (!s_kv) return KVSTORE_ERROR_FAILED_OPERATION;
    int rc = kvs_delete(script_key);
    return rc == KVSTORE_ERROR_ITEM_NOT_FOUND ? KVSTORE_SUCCESS : rc;
}
