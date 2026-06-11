# How This Project Works

A Lua 5.4 environment for the M5Stack Cardputer: boot menu → run `.lua`
scripts from the microSD card, or drop into an interactive REPL using the
built-in keyboard and screen. USB serial doubles as a second REPL input, so
everything can also be driven from `pio device monitor`.

## File layout

```
platformio.ini            build configuration
src/main.cpp              the entire firmware
lib/lua/                  vendored Lua 5.4.8 (one small patch)
  library.json            tells PlatformIO to build it
  luaconf.h               patched: respects -DLUA_32BITS from build flags
  *.c, *.h                untouched upstream sources
docs/                     this documentation
```

### `platformio.ini`

- Board `m5stack-stamps3` (the Cardputer's module), Arduino framework.
- `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`: USB port works for
  flashing *and* serial monitor without pressing reset.
- `LUA_32BITS=1`: 32-bit Lua numbers. Must live here (globally) so
  `main.cpp` and the Lua library compile with the same ABI.
- `m5stack/M5Cardputer` library dependency (display, keyboard, M5Unified).

### `lib/lua/`

Stock Lua 5.4.8 sources minus `lua.c`/`luac.c`/`onelua.c` (host-only `main()`s).
The only modification is in `luaconf.h`, where `#define LUA_32BITS 0` is
wrapped in `#if !defined(LUA_32BITS)` so the build flag isn't silently
overridden. See `docs/porting-lua-to-esp32.md` for the full story.

### `src/main.cpp`

Single-file firmware, organized in sections top to bottom:

| Section | What it does |
|---|---|
| Mode state machine | `enum class Mode { MENU, BROWSER, REPL, SCRIPT }` — which screen is active |
| Terminal | Scrollback text console rendered to the display |
| Lua | Interpreter state, custom bindings, REPL line evaluation |
| SD + browser | Mounts the card, lists `*.lua` files, runs a script |
| History | Up/down command history for the REPL |
| Menu/browser rendering | The two non-terminal screens |
| Input handling | Keyboard dispatch per mode + the serial input path |
| `setup()` / `loop()` | Init and the per-frame dispatch |

## The pieces in detail

### Terminal (`termWrite` / `termRender`)

All output goes through `termWrite()`, which:

1. mirrors the text to `Serial`,
2. appends it to `termLines`, a vector of display-width-wrapped lines
   (capped at 200 — older lines fall off),
3. when in REPL/SCRIPT mode, immediately redraws — so `print()` from a
   long-running script appears live even though `loop()` is blocked.

`termRender()` draws into an 8-bit `M5Canvas` sprite (full screen, 240×135,
6×8 px font → 40 columns × 16 rows) and pushes it in one blit — no flicker.
In REPL mode the bottom rows show the `> ` prompt, the input line (wrapped if
long) and a block cursor; output gets whatever rows remain.

### Lua state

One global `lua_State`, created once at boot and **shared** by the REPL and
scripts — variables you define in the REPL are visible to scripts and vice
versa. Registered on top of the standard library:

- `print(...)` → terminal, tab-separated + newline (replaces stdout version)
- `io.write(...)` → terminal, no separators/newline
- `millis()` → `millis()` from Arduino
- `delay(ms)` → blocking delay
- a count hook every 20 000 VM instructions that calls `yield()` and aborts
  the running chunk with *"interrupted by user"* if `` ` `` is held — the
  escape hatch for infinite loops.

### REPL evaluation (`runLine`)

Tries `return <line>` first so expressions echo their value (`1+2` → `3`),
falls back to compiling the line as a statement. Runs under `lua_pcall`;
errors print to the terminal and never kill the firmware. Any returned values
are passed to `print`.

### SD card and scripts

`sdMount()` initializes the SPI bus on the Cardputer's SD pins
(SCK 40, MISO 39, MOSI 14, CS 12) and mounts the FAT card at `/sd` in the
ESP-IDF VFS. Because Lua's file functions use `fopen`, `dofile("/sd/x.lua")`
and `io.open("/sd/...")` work natively.

`scanScripts()` re-probes the card (so hot-inserting works) and collects the
`.lua` filenames in the card's root directory. `runScript()` switches to the
terminal view and executes the file via `luaL_dofile` under the interrupt
hook; afterwards **enter** re-runs it, **esc** returns to the menu.

### Input

`handleKeys()` reads the M5Cardputer keyboard state machine once per frame
and dispatches on the current mode. Key chords (from `keysState()`'s `fn`
flag):

| Key | Where | Action |
|---|---|---|
| `fn+;` / `fn+.` (↑/↓) | menu, browser | move selection (plain `;`/`.` also work) |
| `fn+;` / `fn+.` | REPL | older / newer command history (last 50, deduped) |
| `enter` | everywhere | select / run / submit |
| `fn+`` ` `` (esc) | browser, REPL, script view | back to menu |
| `` ` `` (held) | while Lua runs | interrupt the running chunk |
| `del` | REPL | backspace |

`handleSerial()` runs in **every** mode: complete lines arriving over USB
serial are evaluated as REPL input, with output mirrored back over serial.
This is the hook that lets the whole stack be exercised from the host
(`pio device monitor`, or scripted with pyserial) without touching the
keyboard.

### Main loop

```
M5Cardputer.update() → handleKeys() → handleSerial() → render current mode
```

Rendering is dirty-flag driven (`uiDirty` for menu/browser, `termDirty` for
the terminal), so idle frames cost nothing but a 10 ms sleep.

## Memory & footprint

- Flash: ~630 KB (18.9 % of the 3.3 MB app partition)
- Static RAM: ~23 KB; loop task stack raised to 32 KB for Lua's recursion
- Lua heap after `luaL_openlibs`: ~17 KB (grows on demand from the ESP32 heap)
