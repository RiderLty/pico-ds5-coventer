#ifndef LUA_SCRIPT_H
#define LUA_SCRIPT_H
#include "proto.h"

// SCRIPT (0x20): STATUS=0 BEGIN=1 WRITE=2 RUN=3 SAVE=4 READ=5
// DEFAULT=6 DELETE=7 ABORT=8 RELOAD_SAVED=9 ERROR=10. See docs/lua-scripts.md.
void lua_script_init(void);        // core0, before launching core1
void lua_script_boot(void);        // core1, before initializing USB host
void lua_script_process(void);     // core1, between host/Lua callbacks
void lua_script_task(void);        // core0, deliver completed replies
void lua_script_request(proto_chan_t *ch, uint16_t id, const uint8_t *data, uint16_t len);
#endif
