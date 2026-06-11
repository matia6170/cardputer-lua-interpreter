# Getting Lua Running on the ESP32-S3 (M5 Cardputer)

This documents the actual steps (and the pitfalls hit along the way) used to get a
full Lua 5.4 interpreter running inside an Arduino/PlatformIO firmware on the
Cardputer's ESP32-S3.

## 1. Vendor the official Lua source

Lua is plain ANSI C with no OS dependencies beyond `stdio`/`setjmp`, so the
official source compiles for the ESP32 almost unmodified.

```sh
curl -O https://www.lua.org/ftp/lua-5.4.8.tar.gz
tar xzf lua-5.4.8.tar.gz
mkdir -p lib/lua
cp lua-5.4.8/src/*.c lua-5.4.8/src/*.h lib/lua/
rm lib/lua/lua.c lib/lua/luac.c lib/lua/onelua.c
```

The three removed files each contain a `main()` for the standalone host
interpreter/compiler — they would collide with the Arduino `app_main` and are
useless on a microcontroller.

A minimal `lib/lua/library.json` makes PlatformIO treat the directory as a
library and compile every `.c` file in it.

## 2. Pick a 32-bit number ABI

By default Lua uses `long long` integers and `double` floats (64-bit). On a
32-bit MCU, `-DLUA_32BITS=1` (32-bit `int` integers, `float` numbers) halves the
size of every Lua value, which saves RAM and is faster on the FPU-less integer
side.

**Pitfall #1:** since Lua 5.4.x, `luaconf.h` *hardcodes* `#define LUA_32BITS 0`
— it silently overrides the compiler flag (upstream expects you to edit the
file). The vendored `luaconf.h` is patched with a guard so the flag works:

```c
#if !defined(LUA_32BITS)
#define LUA_32BITS	0
#endif
```

Without the patch the build fails confusingly: the default `long long` branch
trips `#error "Compiler does not support 'long long'..."` when the headers are
included from C++.

**Pitfall #2:** the define must be **global** (in `platformio.ini`
`build_flags`), not just in `library.json`. If only the Lua `.c` files see
`LUA_32BITS` but `main.cpp` doesn't, the application and the library disagree
about the size of `lua_Number`/`lua_Integer` — an ABI mismatch that corrupts
the stack at runtime.

```ini
build_flags =
    -D LUA_32BITS=1
```

## 3. Include the headers as C

Lua's headers have no `extern "C"` guards. From C++ they must be wrapped:

```cpp
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
```

## 4. Give the loop task a bigger stack

The Arduino `loopTask` has an 8 KB stack. Lua's parser and VM are recursive
(`LUAI_MAXCCALLS` levels of C recursion), which can blow through 8 KB. One line
fixes it:

```cpp
SET_LOOP_TASK_STACK_SIZE(32 * 1024);
```

## 5. Create the state and rewire I/O

`luaL_newstate()` + `luaL_openlibs()` work out of the box (the default
allocator is `realloc`, which lands in ESP32 internal heap; the whole stdlib
costs ~17 KB of Lua heap). The catch is **output**: Lua's `print` and
`io.write` go to C `stdout`, which is invisible on a device with a screen. Both
are replaced with C functions that write to the on-screen terminal (and mirror
to USB serial):

```cpp
lua_pushcfunction(L, l_print);
lua_setglobal(L, "print");

lua_getglobal(L, "io");
lua_pushcfunction(L, l_write);   // print without newline/tabs
lua_setfield(L, -2, "write");
lua_pop(L, 1);
```

Hardware helpers (`millis()`, `delay(ms)`) are registered the same way.

## 6. Make runaway scripts interruptible

`while true do end` would otherwise hang the device until a power cycle. A
count hook runs every 20 000 VM instructions, feeds the watchdog/USB stack, and
raises a Lua error if the `` ` `` (esc) key is held:

```cpp
static void luaInterruptHook(lua_State* Ls, lua_Debug*) {
    yield();
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isKeyPressed('`'))
        luaL_error(Ls, "interrupted by user");
}
lua_sethook(L, luaInterruptHook, LUA_MASKCOUNT, 20000);
```

`luaL_error` longjmps out of the running script back to the protected call —
the REPL survives, the script dies.

## 7. File access for free via the VFS

`SD.begin(CS, SPI, 25000000)` registers the FAT filesystem at `/sd` in
ESP-IDF's virtual file system. Lua's `io`, `loadfile` and `dofile` use newlib
`fopen`, which routes through the VFS — so `dofile("/sd/script.lua")` and
`io.open("/sd/data.txt", "w")` work with **zero** extra binding code.

## 8. The REPL trick for expression results

A line like `1+2` is not a valid Lua statement. The classic REPL approach: try
compiling `return <line>` first; if that fails to parse, compile the line
as-is. Any values returned are fed to `print`:

```cpp
int status = luaL_loadbuffer(L, ("return " + code).c_str(), ..., "=repl");
if (status != LUA_OK) {
    lua_pop(L, 1);
    status = luaL_loadbuffer(L, code.c_str(), ..., "=repl");
}
if (status == LUA_OK) status = lua_pcall(L, 0, LUA_MULTRET, 0);
```

## 9. Verify on real hardware over serial

The firmware accepts REPL input from USB serial in parallel with the keyboard,
so functionality can be tested from the host without touching the device:

```sh
pio run -t upload
pio device monitor   # then type Lua directly
```

The automated check used a few lines of pyserial to send
`print('hello')`, `1+2`, `dofile('/sd/selftest.lua')` etc. and assert on the
echoed results.

## Resulting footprint

| Metric | Value |
|---|---|
| Flash | ~630 KB total firmware (Lua adds ~250 KB) |
| Static RAM | ~23 KB (6.9 %) |
| Lua heap after stdlib | ~17 KB |
