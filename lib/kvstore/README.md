Minimal vendored pico-kvstore/logkvs and pico-vfs Flash blockdevice, copied from
../pico-hid-mapper/lib/pico-kvstore. Original copyright and BSD licenses are
preserved. Only the log store, global API, staging device and Flash device are
built; secure KV, filesystems and upstream default partition setup are omitted.

Sources: pico-kvstore e2629eeba5d7e1bfd1519d45d687e59675d0f72c;
pico-vfs 0ec53e84cf16b6ea0f97ead0c8bcc008847e4a50.
Local fixes: staging cache address comparison and standalone MIN/MAX macros;
avoid NULL pointer arithmetic during record scans; reserve the scanner key NUL;
propagate initialization/erase errors; restore bank version when only one bank
is valid and compare wrapping 16-bit bank versions; handle staging/context
allocation failures and free the staging device on initialization errors;
release staging caches even when the final sync fails. The staging wrapper
borrows the underlying blockdevice; the application retains its ownership.

Application partitioning and serialization live in src/lua_store.c. The Flash
device uses Pico SDK flash_safe_execute; both application cores initialize the
SDK's flash safety support. No application raw Flash write path is used.
