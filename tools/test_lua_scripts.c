// Actual script state machine, Lua VM and KV; only the hardware is mocked.
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include "../src/lua_script.c"
#include "lua_core.h"
#include "blockdevice/flash.h"
#include "kbd_out.h"
#include "mouse_queue.h"
#include "usb_io.h"
#include <lua.h>
#include <lauxlib.h>

// Real usb_km.c applies requests; mock only its hardware/output dependencies.
static bool device_inited, keyboard_enabled;
static unsigned applied, applies, disconnects, connects, slept;
static uint8_t keyboard_instance, io_instance;
void debug_log(const char *fmt, ...) { (void)fmt; }
void kbd_out_init(void) { keyboard_enabled=false; keyboard_instance=KBD_HID_INST_NONE; }
void mouse_queue_init(void) {}
void usb_io_init(void) { io_instance=USB_IO_INST_NONE; }
bool tud_inited(void) { return device_inited; }
bool tud_mounted(void) { return device_inited; }
bool tud_disconnect(void) { ++disconnects; return true; }
bool tud_connect(void) { ++connects; return true; }
void sleep_ms(uint32_t ms) { slept+=ms; }
void usb_km_apply(bool mouse, bool kbd, bool io) {
    applied=(kbd ? USB_KM_BIT_KBD : 0) | (mouse ? USB_KM_BIT_MOUSE : 0) | (io ? USB_KM_BIT_IO : 0);
    ++applies;
}
void kbd_hid_set_instance(uint8_t instance) { keyboard_instance=instance; }
void usb_io_set_instance(uint8_t instance) { io_instance=instance; }
void kbd_out_set_enabled(bool on) { keyboard_enabled=on; }
void mouse_queue_clear(void) {}
bool kbd_hid_enabled(void) { return keyboard_enabled; }
bool kbd_out_peek(kbd_out_state_t *out) { (void)out; return false; }
bool kbd_hid_send_nkro(uint8_t modifier, const uint8_t bitmap[KBD_HID_NKRO_BYTES]) {
    (void)modifier; (void)bitmap; return false;
}
void kbd_out_commit(void) {}
bool mouse_queue_pop(mouse_report_t *out) { (void)out; return false; }
bool mouse_hid_send(const mouse_report_t *rep) { (void)rep; return false; }
void mouse_queue_push(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons) {
    (void)dx; (void)dy; (void)wheel; (void)buttons;
}

static uint8_t flash[LUA_KV_SIZE];
static unsigned erases, programs, releases;
static unsigned flash_devices;
static int fail_program_after = -1;
static uint64_t now;
static uint64_t time_step = 1000;
uint64_t time_us_64(void) { return now += time_step; }
void lua_core_debug_cb(const char *s) { (void)s; }
void input_app_release_outputs(void) { ++releases; }
static int noop(lua_State *L) { (void)L; return 0; }
static int enable(lua_State *L) {
    usb_km_request((int)luaL_optinteger(L,1,-1), (int)luaL_optinteger(L,2,-1), (int)luaL_optinteger(L,3,-1));
    return 0;
}
void lua_binder_rebind(void) {
    lua_core_register("print", noop);
    lua_core_register("dev_enable", enable);
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
static bool block_reply;
bool proto_chan_submit(proto_chan_t *ch,uint8_t cmd,uint16_t id,const uint8_t *p,uint16_t n) {
    (void)ch; assert(id==1); assert(n<=512);
    if (block_reply) return false;
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
static void upload_kind(const char *source,unsigned n,uint8_t kind) {
    uint8_t body[512]={0}; body[0]=kind; put32(body+1,n); put32(body+5,checksum(source,n));
    assert(!request(body,9));
    for(unsigned off=0;off<n;) {
        unsigned count=n-off; if(count>507) count=507;
        body[0]=2;put32(body+1,off);memcpy(body+5,source+off,count);
        assert(!request(body,count+5));off+=count;
    }
}
static void upload(const char *s,unsigned n) { upload_kind(s,n,1); }
static void run(const char *s) { upload(s,strlen(s)); assert(!op(3)); }

// 真正的双线程运行 mailbox，检查发布/复用 512 字节缓冲的所有权。
static unsigned worker_stop;
static void *mailbox_worker(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&worker_stop, __ATOMIC_ACQUIRE)) {
        lua_script_process();
        sched_yield();
    }
    return NULL;
}
static void test_concurrent_mailbox(void) {
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, mailbox_worker, NULL));
    for (unsigned i=0; i<20000; ++i) {
        // 每次偏移不同，检查不会把上次回复或未发布完的字段当成本次请求。
        unsigned offset=i%(s_size-512u+1u);
        uint8_t body[7]={5}; put32(body+1, offset); body[5]=0; body[6]=2;
        unsigned old=replies;
        lua_script_request(&channel, 1, body, sizeof body);
        while (replies==old) { lua_script_task(); sched_yield(); }
        assert(reply_cmd==PROTO_CMD_SCRIPT && reply_size==512);
        assert(!memcmp(reply, s_image+offset, 512));
    }
    __atomic_store_n(&worker_stop, 1u, __ATOMIC_RELEASE);
    assert(!pthread_join(worker, NULL));
}

int main(void) {
    usb_km_init();
    memset(flash,255,sizeof flash);
    fail_program_after=0;
    assert(!lua_store_init() && flash_devices==0);
    assert(lua_store_save("a=1",3)==KVSTORE_ERROR_FAILED_OPERATION);
    fail_program_after=-1;
    lua_script_init(); lua_script_boot();
    assert(lua_store_init() && flash_devices==1); // repeated init retains the live store
    assert(lua_binder_is_ready() && s_image==s_default_image && !s_saved && s_dirty && !s_error[0]);
    unsigned initial_programs=programs, initial_erases=erases;
    // USB and Bluetooth expose only the two DS5 touch points (six arguments),
    // including immediately after VM replacement and garbage collection.
    for (unsigned reload=0; reload<3; ++reload) {
        run("touch_calls=0; function on_touchpad(...) touch_calls=touch_calls+1; touch_args=table.pack(...) end");
        uint16_t x[10], y[10];
        for (unsigned i=0; i<10; ++i) { x[i]=100+i; y[i]=200+i; }
        lua_binder_on_touchpad(3, 1920u|(1080u<<16), x, y);
        assert(lua_core_dostring("assert(touch_calls==1 and touch_args.n==6 and touch_args[1]==3 and touch_args[2]==(1920|(1080<<16))); for i=0,1 do assert(touch_args[3+2*i]==100+i and touch_args[4+2*i]==200+i) end; collectgarbage('collect')"));
        x[2]++; y[9]++; // Unexposed coordinates must not trigger a callback.
        lua_binder_on_touchpad(3, 1920u|(1080u<<16), x, y);
        assert(lua_core_dostring("assert(touch_calls==1)"));
        y[1]++;
        lua_binder_on_touchpad(3, 1920u|(1080u<<16), x, y);
        assert(lua_core_dostring("assert(touch_calls==2 and touch_args.n==6 and touch_args[6]==202)"));
        lua_binder_on_touchpad(0, 1920u|(1080u<<16), x, y);
        assert(lua_core_dostring("assert(touch_calls==3 and touch_args.n==6 and touch_args[1]==0)"));
    }
    assert(!op(6)); // restore default before the remaining lifecycle tests
    // A controlled clock verifies the boundary, real dt, no catch-up, and reload reset.
    time_step=0;
    for (unsigned reload=0; reload<2; ++reload) {
        run("ticks={}; function tick(dt) ticks[#ticks+1]=dt end");
        assert(!lua_binder_process());
        now+=LUA_TICK_INTERVAL_US-1;
        assert(!lua_binder_process());
        assert(lua_core_dostring("assert(#ticks==0)"));
        ++now;
        assert(lua_binder_process());
        assert(lua_core_dostring("assert(#ticks==1 and ticks[1]==0)"));
        now+=LUA_TICK_INTERVAL_US-1;
        assert(!lua_binder_process());
        ++now;
        assert(lua_binder_process());
        assert(lua_core_dostring("assert(#ticks==2)"));
        char interval_assertion[80];
        snprintf(interval_assertion,sizeof interval_assertion,"expected_interval=%llu",(unsigned long long)LUA_TICK_INTERVAL_US);
        assert(lua_core_dostring(interval_assertion));
        assert(lua_core_dostring("assert(ticks[2]==expected_interval)"));
        now+=5u*LUA_TICK_INTERVAL_US;
        assert(lua_binder_process());
        assert(!lua_binder_process());
        assert(lua_core_dostring("assert(#ticks==3 and ticks[3]==5*expected_interval)"));
    }
    time_step=1000;
    assert(!op(6));
    const char good[]="function on_key(k,d) seen=k end; function on_boot_button(p) boot=p; boots=(boots or 0)+1; return p end; marker=42";
    upload(good,strlen(good));
    assert(s_image==s_default_image && programs==initial_programs);
    assert(!op(3)); assert(s_size==strlen(good) && !memcmp(s_image,good,s_size) && s_dirty);
    lua_binder_on_key(4,true);
    assert(lua_core_dostring("assert(marker==42 and seen==4)"));
    // BOOTSEL 事件走同一条转发路径（core0 消抖后跨核投递，这里直接调转发口）
    assert(lua_binder_on_boot_button(true));
    assert(!lua_binder_on_boot_button(false));
    assert(lua_core_dostring("assert(boots==2 and boot==false)"));
    assert(programs==initial_programs && erases==initial_erases);
    assert(!op(4) && s_saved && !s_dirty);
    // AI uses its own bounded KV record and must never execute/swap the Lua VM.
    assert(!op(11) && strstr((char *)reply, "\"size\":0"));
    const char ai[]="{\"apiKey\":\"secret\",\"model\":\"test\"}";
    upload_kind(ai,strlen(ai),13);
    assert(op(3)==PROTO_ERR_COMMAND);
    s_crc^=1; assert(op(14)==PROTO_ERR_CRC); s_crc^=1;
    assert(!op(14));
    assert(s_size==strlen(good) && !memcmp(s_image,good,s_size) && !s_dirty);
    lua_store_deinit(); assert(lua_store_init());
    assert(!op(11));
    uint8_t ai_read[7]={12}; ai_read[5]=strlen(ai);
    assert(!request(ai_read,sizeof ai_read) && reply_size==strlen(ai) && !memcmp(reply,ai,reply_size));
    uint8_t ai_begin[9]={13}; put32(ai_begin+1,AI_CFG_MAX+1);
    assert(request(ai_begin,sizeof ai_begin)==PROTO_ERR_LENGTH);
    upload("a=1",3); assert(op(14)==PROTO_ERR_COMMAND); assert(!op(8));
    unsigned small_erases=erases;
    for (unsigned i=0;i<5;++i) assert(!op(4));
    assert(erases==small_erases); // small records append within the erased sector
    run("marker=99"); assert(!op(9));
    assert(s_size==strlen(good) && !memcmp(s_image,good,s_size)); // unsaved run never replaced persisted source
    run("function on_boot_button(p) end");
    assert(!lua_binder_on_boot_button(false)); // nil/no return does not intercept
    run("marker=1");
    assert(!lua_binder_on_boot_button(false)); // unbound callback does not intercept
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
    // 出口暂忙时保留回复和 job 所有权；新请求必须 BUSY，重试回复不被覆盖。
    lua_script_request(&channel,1,&status,1); lua_script_process();
    block_reply=true; lua_script_task(); block_reply=false;
    assert(__atomic_load_n(&s_job.state,__ATOMIC_ACQUIRE)==3u);
    lua_script_request(&other,1,&reset,1);
    assert(reply_cmd==PROTO_CMD_ERROR && reply[1]==PROTO_ERR_BUSY);
    lua_script_task(); assert(reply_cmd==PROTO_CMD_SCRIPT);
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
    assert(!request(ai_read,sizeof ai_read) && reply_size==strlen(ai) && !memcmp(reply,ai,reply_size));
    assert(op(9)==PROTO_ERR_SCRIPT && s_image==s_default_image);
    // Persist a syntactically bad script directly to simulate a bad boot KV value.
    assert(!lua_store_save("function invalid(",17));
    lua_store_deinit();
    assert(flash_devices==0);
    puts("PASS: mailbox reply backpressure/BUSY and 20000 concurrent 512-byte replies");
    lua_script_boot();assert(s_image==s_default_image && lua_binder_is_ready());
    assert(s_saved && s_dirty && strstr(s_error,"script"));
    // Boot applies the final top-level target before Device initialization.
    const char startup[]="dev_enable(1,0,0); dev_enable(-1,1,1); dev_enable(0,-1,-1)";
    assert(!lua_store_save(startup,strlen(startup)));
    usb_km_init(); lua_script_boot();
    assert(!usb_km_mouse_enabled() && !usb_km_io_enabled()); // still pending
    usb_km_task();
    assert(applied==(USB_KM_BIT_MOUSE|USB_KM_BIT_IO) && applies==1);
    assert(keyboard_instance==KBD_HID_INST_NONE && io_instance==2);
    assert(!disconnects && !connects && !slept);
    device_inited=true; // first enumeration uses this already-applied configuration
    assert(lua_core_dostring("dev_enable(0,1,1)")); usb_km_task();
    assert(applies==1 && !disconnects); // same state, no reload
    assert(lua_core_dostring("dev_enable(1,-1,-1)")); usb_km_task();
    assert(applied==7 && applies==2 && keyboard_instance==2 && io_instance==3);
    assert(disconnects==1 && connects==1 && slept==500);
    // Defining tick at boot must not execute it until the Device is ready.
    const char delayed[]="function tick(dt) dev_enable(1,0,1) end";
    assert(!lua_store_save(delayed,strlen(delayed)));
    device_inited=false; usb_km_init(); lua_script_boot();
    unsigned old_applies=applies; usb_km_task();
    assert(applies==old_applies && !usb_km_kbd_enabled() && !usb_km_io_enabled());
    device_inited=true; lua_binder_process(); usb_km_task();
    assert(applied==(USB_KM_BIT_KBD|USB_KM_BIT_IO) && keyboard_instance==1 && io_instance==2);
    assert(disconnects==2 && connects==2 && slept==1000);
    // A failed boot must discard partial requests before the default is run.
    const char *failed_boot[]={"dev_enable(1,1,1); error('bad boot')", "dev_enable(1,1,1); while true do end"};
    for(unsigned i=0;i<2;++i) {
        assert(!lua_store_save(failed_boot[i],strlen(failed_boot[i])));
        device_inited=false; usb_km_init(); lua_script_boot();
        assert(s_image==s_default_image && s_dirty && s_error[0]);
        old_applies=applies; usb_km_task();
        assert(applies==old_applies && !usb_km_kbd_enabled() && !usb_km_mouse_enabled() && !usb_km_io_enabled());
        assert(disconnects==2 && connects==2 && slept==1000);
    }
    assert(releases>10);
    test_concurrent_mailbox();
    lua_binder_deinit();discard_upload();free(s_owned);
    lua_store_deinit();
    assert(flash_devices==0);
    puts("PASS: real Lua/KV RAM-only run, explicit save/reload/delete, boot/default fallback, watchdog, 60KiB, GC, interrupted writes, CRC, sessions, first-enumeration capabilities, runtime reload and six-argument touch callbacks with Lua API checks");
}
