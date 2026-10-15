// Actual script state machine, Lua VM and KV; only the hardware is mocked.
#include <assert.h>
#include "../src/lua_script.c"
#include "lua_core.h"
#include "blockdevice/flash.h"
#include <lua.h>

static uint8_t flash[LUA_KV_SIZE];
static unsigned erases, programs, releases;
static unsigned flash_devices;
static int fail_program_after = -1;
static uint64_t now;
uint64_t time_us_64(void) { return now += 1000; }
void lua_core_debug_cb(const char *s) { (void)s; }
void input_app_release_outputs(void) { ++releases; }
static int noop(lua_State *L) { (void)L; return 0; }
void lua_binder_rebind(void) {
    lua_core_register("print", noop);
    lua_core_register("ability_enable", noop);
    lua_core_register_in("hidkit", "info", noop);
}
static int bd_init(blockdevice_t *bd) { bd->is_initialized=true; return 0; }
static int bd_sync(blockdevice_t *bd) { (void)bd; return 0; }
static bd_size_t bd_size(blockdevice_t *bd) { (void)bd; return sizeof flash; }
static int bd_read(blockdevice_t *bd,const void *buf,bd_size_t addr,bd_size_t n) {
    (void)bd; assert(addr+n<=sizeof flash); memcpy((void *)buf,flash+addr,n); return 0;
}
static int bd_erase(blockdevice_t *bd,bd_size_t addr,bd_size_t n) {
    (void)bd; assert(addr%4096==0 && n%4096==0 && addr+n<=sizeof flash);
    ++erases; memset(flash+addr,255,n); return 0;
}
static int bd_program(blockdevice_t *bd,const void *buf,bd_size_t addr,bd_size_t n) {
    (void)bd; assert(addr%256==0 && n%256==0 && addr+n<=sizeof flash);
    if (fail_program_after==0) return -4001;
    if (fail_program_after>0) --fail_program_after;
    ++programs;
    for (size_t i=0;i<n;++i) {
        uint8_t b=((const uint8_t *)buf)[i];
        assert((flash[addr+i]&b)==b); // NOR cannot change a 0 to a 1 without erase
        flash[addr+i]&=b;
    }
    return 0;
}
blockdevice_t *blockdevice_flash_create(uint32_t start,size_t len) {
    assert(start==4u*1024u*1024u-LUA_KV_SIZE && len==LUA_KV_SIZE);
    blockdevice_t *bd=calloc(1,sizeof *bd);
    ++flash_devices;
    *bd=(blockdevice_t){.init=bd_init,.deinit=bd_sync,.sync=bd_sync,.read=bd_read,
        .program=bd_program,.erase=bd_erase,.trim=bd_erase,.size=bd_size,
        .read_size=1,.program_size=256,.erase_size=4096,.is_initialized=true};
    return bd;
}
void blockdevice_flash_free(blockdevice_t *bd) { --flash_devices; free(bd); }

static proto_chan_t channel={.active=true}, other={.active=true};
static uint8_t reply[512], reply_cmd;
static uint16_t reply_size;
static unsigned replies;
bool proto_chan_submit(proto_chan_t *ch,uint8_t cmd,uint16_t id,const uint8_t *p,uint16_t n) {
    (void)ch; assert(id==1); assert(n<=512);
    memcpy(reply,p,n); reply_size=n; reply_cmd=cmd; ++replies; return true;
}
static uint8_t request(const uint8_t *p, unsigned n) {
    unsigned old=replies;
    lua_script_request(&channel,1,p,n); lua_script_process(); lua_script_task();
    assert(replies==old+1);
    return reply_cmd==PROTO_CMD_ERROR ? reply[1] : 0;
}
static uint8_t op(uint8_t code) { return request(&code,1); }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i) p[i]=n>>(i*8); }
static void upload(const char *source,unsigned n) {
    uint8_t body[512]={1}; put32(body+1,n); put32(body+5,checksum(source,n));
    assert(!request(body,9));
    for(unsigned off=0;off<n;) {
        unsigned count=n-off; if(count>507) count=507;
        body[0]=2;put32(body+1,off);memcpy(body+5,source+off,count);
        assert(!request(body,count+5));off+=count;
    }
}
static void run(const char *s) { upload(s,strlen(s)); assert(!op(3)); }

int main(void) {
    memset(flash,255,sizeof flash);
    fail_program_after=0;
    assert(!lua_store_init() && flash_devices==0);
    assert(lua_store_save("a=1",3)==KVSTORE_ERROR_FAILED_OPERATION);
    fail_program_after=-1;
    lua_script_init(); lua_script_boot();
    assert(lua_store_init() && flash_devices==1); // repeated init retains the live store
    assert(lua_binder_is_ready() && s_image==s_default_image && !s_saved && s_dirty && !s_error[0]);
    unsigned initial_programs=programs, initial_erases=erases;
    const char good[]="enable_listen_keys(4); function on_key(k,d) seen=k end; marker=42";
    upload(good,strlen(good));
    assert(s_image==s_default_image && programs==initial_programs);
    assert(!op(3)); assert(s_size==strlen(good) && !memcmp(s_image,good,s_size) && s_dirty);
    lua_binder_on_key(4,true);
    assert(lua_core_dostring("assert(marker==42 and seen==4)"));
    assert(programs==initial_programs && erases==initial_erases);
    assert(!op(4) && s_saved && !s_dirty);
    unsigned small_erases=erases;
    for (unsigned i=0;i<5;++i) assert(!op(4));
    assert(erases==small_erases); // small records append within the erased sector
    run("marker=99"); assert(!op(9));
    assert(s_size==strlen(good) && !memcmp(s_image,good,s_size)); // unsaved run never replaced persisted source
    upload("function invalid(",17);
    assert(op(3)==PROTO_ERR_SCRIPT && s_image==s_default_image);
    assert(strstr(s_error,"script"));
    assert(!op(9));
    upload("error('bad startup')",20);
    assert(op(3)==PROTO_ERR_SCRIPT && s_image==s_default_image);
    assert(strstr(s_error,"bad startup"));
    upload("while true do end",17);
    assert(op(3)==PROTO_ERR_SCRIPT && strstr(s_error,"step budget"));
    assert(!op(9));
    // BEGIN/WRITE validation and CRC protect the old running image.
    uint8_t begin[9]={1};put32(begin+1,LUA_SCRIPT_MAX+1);assert(request(begin,9)==PROTO_ERR_LENGTH);
    put32(begin+1,3);assert(!request(begin,9));
    uint8_t part[]={2,1,0,0,0,'a'};assert(request(part,sizeof part)==PROTO_ERR_LENGTH);
    part[1]=0;assert(!request(part,sizeof part));assert(op(3)==PROTO_ERR_LENGTH);
    assert(!op(8));
    upload("a=1",3);s_crc^=1;assert(op(3)==PROTO_ERR_CRC);assert(!op(8));
    // Different channels cannot steal uploads; old sessions cannot continue them.
    upload("a=1",3);
    uint8_t reset=6;
    lua_script_request(&other,1,&reset,1);lua_script_process();lua_script_task();
    assert(reply_cmd==PROTO_CMD_ERROR && reply[1]==PROTO_ERR_BUSY);
    channel.generation++;assert(op(3)==PROTO_ERR_BUSY); // also frees stale upload
    assert(!op(9));
    // A reply produced for an old connection is never delivered to a new one.
    unsigned count=replies;uint8_t status=0;
    lua_script_request(&channel,1,&status,1);lua_script_process();channel.generation++;
    lua_script_task();assert(replies==count);
    // RESET before the worker starts also cancels side effects, not just replies.
    uint8_t save=4;unsigned writes=programs;
    lua_script_request(&channel,1,&save,1);channel.generation++;
    lua_script_process();lua_script_task();
    assert(programs==writes && replies==count && !op(0));
    upload("a=1",3);now+=31000000;
    lua_script_process();assert(!s_upload); // timeout releases RAM without another request
    // Maximum-sized script, repeated saves and garbage collection.
    char *large=malloc(LUA_SCRIPT_MAX);memset(large,' ',LUA_SCRIPT_MAX);memcpy(large,"marker=123",10);
    upload(large,LUA_SCRIPT_MAX);assert(!op(3));
    for(unsigned i=0;i<12;++i) assert(!op(4));
    assert(erases>initial_erases && !op(9) && s_size==LUA_SCRIPT_MAX);
    assert(lua_core_dostring("assert(marker==123)"));
    free(large);
    // Reopen the actual KV with a power-cut snapshot: either old or new complete record.
    static uint8_t baseline[LUA_KV_SIZE];memcpy(baseline,flash,sizeof flash);
    for(int cut=0;cut<5;++cut) {
        lua_store_deinit();
        memcpy(flash,baseline,sizeof flash);assert(lua_store_init());
        fail_program_after=cut;
        lua_store_save("marker=456",10);
        lua_store_deinit(); // release failed-write caches while the failure is still active
        fail_program_after=-1;
        assert(lua_store_init());
        char saved[LUA_SCRIPT_MAX];size_t size=0;
        assert(!lua_store_read(saved,sizeof saved,&size));
        assert((size==10 && !memcmp(saved,"marker=456",10)) || size==LUA_SCRIPT_MAX);
    }
    assert(!op(7) && !s_saved);
    assert(op(9)==PROTO_ERR_SCRIPT && s_image==s_default_image);
    // Persist a syntactically bad script directly to simulate a bad boot KV value.
    assert(!lua_store_save("function invalid(",17));
    lua_store_deinit();
    assert(flash_devices==0);
    lua_script_boot();assert(s_image==s_default_image && lua_binder_is_ready());
    assert(s_saved && s_dirty && strstr(s_error,"script"));
    assert(releases>10);
    lua_binder_deinit();discard_upload();free(s_owned);
    lua_store_deinit();
    assert(flash_devices==0);
    puts("PASS: real Lua/KV RAM-only run, explicit save/reload/delete, boot/default fallback, watchdog, 60KiB, GC, interrupted writes, CRC, sessions");
}
