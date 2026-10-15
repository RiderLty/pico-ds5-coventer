#include "lua_script.h"
#include "lua_store.h"
#include "lua_binder.h"
#include "lua_default_script.h"
#include "input_app.h"
#include "kvstore.h"
#include "pico/critical_section.h"
#include "pico/time.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static critical_section_t s_lock;
// One bounded mailbox. No locks are held during Lua, KV or USB calls.
static struct {
    unsigned state; // 0 idle, 1 request, 2 executing, 3 reply
    proto_chan_t *ch;
    uint32_t generation;
    uint16_t id, len;
    uint8_t data[512], error;
} s_job;

static char s_default_image[sizeof(LUA_DEFAULT_SCRIPT)-1];
static const char *s_image = s_default_image;
static size_t s_size = sizeof(LUA_DEFAULT_SCRIPT) - 1;
static char *s_owned, *s_upload;
static uint32_t s_expected, s_received, s_crc;
static proto_chan_t *s_owner;
static uint32_t s_owner_generation;
static uint64_t s_upload_time;
static bool s_kv_ok, s_saved, s_dirty;
static char s_error[160];

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t checksum(const void *data, size_t size) {
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    while (size--) {
        crc ^= *p++;
        for (unsigned b=0; b<8; ++b) crc = (crc>>1) ^ (0xEDB88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static void discard_upload(void) {
    free(s_upload); s_upload = NULL; s_expected = s_received = 0; s_owner = NULL;
}
static bool owns_upload(void) {
    return s_upload && s_owner == s_job.ch && s_owner_generation == s_job.generation;
}
static bool load_default(void) {
    input_app_release_outputs();
    lua_binder_deinit();
    free(s_owned); s_owned = NULL;
    memcpy(s_default_image, LUA_DEFAULT_SCRIPT, sizeof s_default_image);
    s_image = s_default_image; s_size = sizeof s_default_image;
    s_dirty = true;
    return lua_binder_init(s_image, s_size);
}
static bool run_upload(void) {
    input_app_release_outputs();
    lua_binder_deinit();
    if (!lua_binder_init(s_upload, s_received)) {
        snprintf(s_error, sizeof s_error, "%s", lua_binder_last_error());
        discard_upload();
        load_default();
        return false;
    }
    free(s_owned); s_owned = s_upload; s_image = s_owned; s_size = s_received;
    s_upload = NULL; discard_upload(); s_dirty = true; s_error[0] = 0;
    return true;
}
static bool read_saved(bool requested) {
    discard_upload();
    s_saved = false;
    s_upload = malloc(LUA_SCRIPT_MAX);
    if (!s_upload) { snprintf(s_error, sizeof s_error, "out of memory"); load_default(); return false; }
    size_t size = 0;
    int rc = lua_store_read(s_upload, LUA_SCRIPT_MAX, &size);
    if (rc) {
        if (!requested && rc == KVSTORE_ERROR_ITEM_NOT_FOUND) s_error[0] = 0;
        else snprintf(s_error, sizeof s_error, "KV: %s", kvs_strerror(rc));
        discard_upload(); load_default(); return false;
    }
    s_received = size; s_saved = true;
    if (!run_upload()) return false;
    s_dirty = false;
    return true;
}

void lua_script_init(void) {
    critical_section_init_with_lock_num(&s_lock, next_striped_spin_lock_num());
}
void lua_script_boot(void) {
    s_kv_ok = lua_store_init();
    read_saved(false); // absent, corrupt or invalid script always falls back to embedded
}
void lua_script_request(proto_chan_t *ch, uint16_t id, const uint8_t *data, uint16_t len) {
    if (len > sizeof s_job.data) {
        uint8_t error[] = {PROTO_CMD_SCRIPT, PROTO_ERR_LENGTH};
        proto_chan_submit(ch, PROTO_CMD_ERROR, id, error, sizeof error);
        return;
    }
    critical_section_enter_blocking(&s_lock);
    bool busy = s_job.state != 0;
    if (!busy) {
        s_job.ch=ch; s_job.id=id;
        s_job.generation=__atomic_load_n(&ch->generation, __ATOMIC_ACQUIRE);
        s_job.len=len; memcpy(s_job.data,data,len); s_job.state=1;
    }
    critical_section_exit(&s_lock);
    if (busy) {
        uint8_t error[] = {PROTO_CMD_SCRIPT, PROTO_ERR_BUSY};
        proto_chan_submit(ch, PROTO_CMD_ERROR, id, error, sizeof error);
    }
}

static uint8_t execute(void) {
    uint8_t *p = s_job.data;
    uint16_t n = s_job.len;
    if (!n) return PROTO_ERR_LENGTH;
    unsigned op = p[0];
    if (op != 1 && op != 2 && op != 5 && n != 1) return PROTO_ERR_LENGTH;
    s_job.len = 0;
    if (s_upload && !owns_upload() && op != 0 && op != 5 && op != 10) return PROTO_ERR_BUSY;
    switch (op) {
    case 0: {
        // Error text travels separately on load failures, keeping status valid JSON.
        int len = snprintf((char *)p, 512,
            "{\"maxSize\":%u,\"partitionSize\":%u,\"size\":%u,\"crc32\":%lu,"
            "\"saved\":%s,\"dirty\":%s,\"default\":%s,\"kvReady\":%s,\"uploadSize\":%lu,\"received\":%lu}",
            LUA_SCRIPT_MAX, LUA_KV_SIZE, (unsigned)s_size, (unsigned long)checksum(s_image,s_size),
            s_saved?"true":"false", s_dirty?"true":"false", s_owned?"false":"true",
            s_kv_ok?"true":"false", (unsigned long)s_expected,(unsigned long)s_received);
        s_job.len = len; break;
    }
    case 1:
        if (n != 9 || !get32(p+1) || get32(p+1)>LUA_SCRIPT_MAX) return PROTO_ERR_LENGTH;
        if (s_upload && !owns_upload()) return PROTO_ERR_BUSY;
        discard_upload(); s_expected=get32(p+1); s_crc=get32(p+5);
        s_upload=malloc(s_expected);
        if (!s_upload) { s_expected=0; snprintf(s_error,sizeof s_error,"out of memory staging script"); return PROTO_ERR_SCRIPT; }
        s_owner=s_job.ch; s_owner_generation=s_job.generation; s_upload_time=time_us_64(); break;
    case 2:
        if (n < 6) return PROTO_ERR_LENGTH;
        if (!owns_upload()) return PROTO_ERR_BUSY;
        if (get32(p+1)!=s_received || n-5u>s_expected-s_received) return PROTO_ERR_LENGTH;
        memcpy(s_upload+s_received,p+5,n-5u); s_received+=n-5u; s_upload_time=time_us_64(); break;
    case 3:
        if (!owns_upload()) return PROTO_ERR_BUSY;
        if (s_received!=s_expected) return PROTO_ERR_LENGTH;
        if (checksum(s_upload,s_received)!=s_crc) return PROTO_ERR_CRC;
        if (!run_upload()) return PROTO_ERR_SCRIPT;
        break;
    case 4: {
        int rc = lua_store_save(s_image,s_size);
        if (rc) { snprintf(s_error,sizeof s_error,"KV: %s",kvs_strerror(rc)); return PROTO_ERR_STORAGE; }
        s_saved=true; s_dirty=false; break;
    }
    case 5: {
        // READ [op][offset:u32][count:u16]; reads current RAM image, up to 512B.
        if (n!=7) return PROTO_ERR_LENGTH;
        uint32_t offset=get32(p+1); unsigned count=p[5]|(unsigned)p[6]<<8;
        if (offset>s_size || count>512 || count>s_size-offset) return PROTO_ERR_LENGTH;
        memcpy(p,s_image+offset,count); s_job.len=count; break;
    }
    case 6:
        discard_upload(); s_error[0]=0;
        if (!load_default()) return PROTO_ERR_SCRIPT;
        break;
    case 7: {
        int rc=lua_store_delete();
        if (rc) { snprintf(s_error,sizeof s_error,"KV: %s",kvs_strerror(rc)); return PROTO_ERR_STORAGE; }
        s_saved=false; s_dirty=true; break;
    }
    case 8:
        if (s_upload && !owns_upload()) return PROTO_ERR_BUSY;
        discard_upload(); break;
    case 9:
        if (!read_saved(true)) return PROTO_ERR_SCRIPT;
        break;
    case 10:
        s_job.len=strlen(s_error); memcpy(p,s_error,s_job.len); break;
    default: return PROTO_ERR_COMMAND;
    }
    return 0;
}

void lua_script_process(void) {
    // Expire abandoned uploads even if the owner never sends another request.
    if (s_upload && s_owner && (time_us_64()-s_upload_time > 30000000u ||
        __atomic_load_n(&s_owner->generation, __ATOMIC_ACQUIRE) != s_owner_generation)) discard_upload();
    critical_section_enter_blocking(&s_lock);
    bool pending=s_job.state==1;
    if (pending && __atomic_load_n(&s_job.ch->generation, __ATOMIC_ACQUIRE) != s_job.generation) {
        s_job.state=0;
        pending=false; // reset before execution: do not run a stale SAVE/RUN
    }
    if (pending) s_job.state=2;
    critical_section_exit(&s_lock);
    if (!pending) return;
    s_job.error=execute();
    if (s_job.error==PROTO_ERR_SCRIPT || s_job.error==PROTO_ERR_STORAGE) {
        // Extended ERROR body: [cmd][code][UTF-8 diagnostic].
        s_job.data[0]=PROTO_CMD_SCRIPT; s_job.data[1]=s_job.error;
        size_t len=strlen(s_error); memcpy(s_job.data+2,s_error,len); s_job.len=2+len;
    } else if (s_job.error) {
        s_job.data[0]=PROTO_CMD_SCRIPT; s_job.data[1]=s_job.error; s_job.len=2;
    }
    critical_section_enter_blocking(&s_lock); s_job.state=3; critical_section_exit(&s_lock);
}
void lua_script_task(void) {
    critical_section_enter_blocking(&s_lock);
    bool ready=s_job.state==3;
    critical_section_exit(&s_lock);
    if (!ready) return;
    if (s_job.ch->active && s_job.ch->generation==s_job.generation &&
        !proto_chan_submit(s_job.ch,s_job.error?PROTO_CMD_ERROR:PROTO_CMD_SCRIPT,
                           s_job.id,s_job.data,s_job.len)) return;
    critical_section_enter_blocking(&s_lock); s_job.state=0; critical_section_exit(&s_lock);
}
