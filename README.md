# Cardputer Lua

A Lua 5.4 environment for the [M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer). Run scripts from a microSD card or use an interactive REPL from the built-in keyboard and screen. USB serial doubles as a second REPL input.

## Features

- Full Lua 5.4.8 interpreter (32-bit mode, ~17 KB heap after stdlib)
- Boot menu: choose between the script browser or the REPL
- Script browser: lists all `.lua` files in the SD card root, run with one key
- Interactive REPL with command history (last 50 commands)
- `io.write()` for print-without-newline, works on screen
- Hardware bindings: `millis()`, `delay(ms)`
- Interrupt any runaway script by holding `` ` ``
- Full Lua file I/O (`io.open`, `dofile`) routed to `/sd`
- All output mirrored to USB serial

## Hardware

- M5Stack Cardputer (ESP32-S3)
- MicroSD card formatted as FAT32

## Build & Flash

Requires [PlatformIO](https://platformio.org).

```sh
pio run -t upload
```

## Using the Serial REPL

```sh
pio device monitor
```

Type Lua and press Enter. Use `--echo` if you want local keystroke echo. See `docs/how-it-works.md` for notes on why some serial monitors don't work out of the box.

## Keyboard Controls

| Key | Context | Action |
|---|---|---|
| `Enter` | everywhere | select / run / submit |
| `fn` + `;` / `.` | menu, browser | move selection up / down |
| `fn` + `;` / `.` | REPL | older / newer command in history |
| `fn` + `` ` `` (Esc) | browser, REPL, script view | back to menu |
| `` ` `` (hold) | while Lua runs | interrupt the running script |
| `Del` | REPL | backspace |

## Putting Scripts on the SD Card

Copy any `.lua` files to the **root** of the SD card. They will appear in the script browser on boot (the card is re-scanned each time you enter the browser, so hot-inserting works).

From the REPL, `io.open`, `dofile`, and `require` all use `/sd` as the filesystem root:

```lua
dofile("/sd/myscript.lua")
io.open("/sd/data.txt", "w")
```

## Lua Bindings

| Name | Description |
|---|---|
| `print(...)` | print to screen (tab-separated, newline) |
| `io.write(...)` | print to screen (no separator, no newline) |
| `millis()` | milliseconds since boot |
| `delay(ms)` | blocking delay |

All standard Lua 5.4 libraries are available (`math`, `string`, `table`, `io`, `os`, `coroutine`, `utf8`, `package`, `debug`).

## Documentation

- [`docs/porting-lua-to-esp32.md`](docs/porting-lua-to-esp32.md) — steps and pitfalls for compiling Lua on the ESP32-S3
- [`docs/how-it-works.md`](docs/how-it-works.md) — project architecture and file guide
