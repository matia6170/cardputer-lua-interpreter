#include <Arduino.h>

#include <vector>

#include <SD.h>
#include <SPI.h>

#include "M5Cardputer.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

// Lua parsing/recursion needs more than the default 8KB loop task stack.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// Cardputer microSD slot (SPI)
static constexpr int SD_SCK = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS = 12;

enum class Mode { MENU, BROWSER, REPL, SCRIPT };
static Mode mode = Mode::MENU;
static bool uiDirty = true;

// ---------------------------------------------------------------------------
// Terminal: scrollback rendered on the display, mirrored to Serial
// ---------------------------------------------------------------------------

static constexpr int CHAR_W = 6;  // fonts::Font0 is 6x8 px
static constexpr int CHAR_H = 8;
static constexpr size_t TERM_MAX_LINES = 200;

static M5Canvas canvas(&M5Cardputer.Display);
static int termCols, termRows;
static std::vector<String> termLines;  // wrapped scrollback
static String inputLine;
static bool termDirty = true;

static void termRender();

static void termNewLine() {
    termLines.push_back("");
    if (termLines.size() > TERM_MAX_LINES) termLines.erase(termLines.begin());
}

static void termClear() {
    termLines.clear();
    termDirty = true;
}

static void termWrite(const char* s) {
    Serial.print(s);
    if (termLines.empty()) termNewLine();
    for (const char* p = s; *p; ++p) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            termNewLine();
            continue;
        }
        if ((int)termLines.back().length() >= termCols) termNewLine();
        termLines.back() += *p;
    }
    termDirty = true;
    // Live output while Lua is blocking the main loop (print() from scripts).
    if (mode == Mode::REPL || mode == Mode::SCRIPT) termRender();
}

static void termRender() {
    if (!termDirty) return;
    termDirty = false;

    canvas.fillSprite(TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    bool showPrompt = (mode == Mode::REPL);
    String in = "> " + inputLine;
    int inputRows = showPrompt ? (int)in.length() / termCols + 1 : 0;
    int outRows = termRows - inputRows;

    int total = (int)termLines.size();
    int start = total > outRows ? total - outRows : 0;
    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
    int y = 0;
    for (int i = start; i < total; ++i, y += CHAR_H) {
        canvas.drawString(termLines[i], 0, y);
    }

    if (showPrompt) {
        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
        y = outRows * CHAR_H;
        for (int i = 0; i < inputRows; ++i, y += CHAR_H) {
            canvas.drawString(in.substring(i * termCols, min((i + 1) * termCols, (int)in.length())),
                              0, y);
        }
        canvas.fillRect(((int)in.length() % termCols) * CHAR_W, (termRows - 1) * CHAR_H, CHAR_W,
                        CHAR_H, TFT_WHITE);
    }

    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Lua
// ---------------------------------------------------------------------------

static lua_State* L = nullptr;

static int l_print(lua_State* Ls) {
    int n = lua_gettop(Ls);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) termWrite("\t");
        termWrite(luaL_tolstring(Ls, i, nullptr));
        lua_pop(Ls, 1);
    }
    termWrite("\n");
    return 0;
}

// io.write replacement: like print() but no newline and no tab separators.
static int l_write(lua_State* Ls) {
    int n = lua_gettop(Ls);
    for (int i = 1; i <= n; ++i) {
        termWrite(luaL_tolstring(Ls, i, nullptr));
        lua_pop(Ls, 1);
    }
    return 0;
}

static int l_millis(lua_State* Ls) {
    lua_pushinteger(Ls, (lua_Integer)millis());
    return 1;
}

static int l_delay(lua_State* Ls) {
    delay((uint32_t)luaL_checkinteger(Ls, 1));
    return 0;
}

// Runs every N VM instructions: keeps USB alive and lets the ` (esc) key
// abort runaway code like `while true do end`.
static void luaInterruptHook(lua_State* Ls, lua_Debug*) {
    yield();
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        luaL_error(Ls, "interrupted by user");
    }
}

static void luaInit() {
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, l_millis);
    lua_setglobal(L, "millis");
    lua_pushcfunction(L, l_delay);
    lua_setglobal(L, "delay");
    // point io.write at the terminal instead of the (invisible) C stdout
    lua_getglobal(L, "io");
    lua_pushcfunction(L, l_write);
    lua_setfield(L, -2, "write");
    lua_pop(L, 1);
    lua_sethook(L, luaInterruptHook, LUA_MASKCOUNT, 20000);
}

static void luaReportError() {
    termWrite(lua_tostring(L, -1));
    termWrite("\n");
    lua_pop(L, 1);
}

static void runLine(const String& code) {
    termWrite("> ");
    termWrite(code.c_str());
    termWrite("\n");

    int base = lua_gettop(L);

    // REPL convention: try the line as an expression first so `1+2` prints 3,
    // then fall back to running it as a statement.
    String expr = "return " + code;
    int status = luaL_loadbuffer(L, expr.c_str(), expr.length(), "=repl");
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadbuffer(L, code.c_str(), code.length(), "=repl");
    }
    if (status == LUA_OK) status = lua_pcall(L, 0, LUA_MULTRET, 0);

    if (status != LUA_OK) {
        luaReportError();
        return;
    }

    int nres = lua_gettop(L) - base;
    if (nres > 0) {
        lua_getglobal(L, "print");
        lua_insert(L, base + 1);
        if (lua_pcall(L, nres, 0, 0) != LUA_OK) luaReportError();
    }
}

// ---------------------------------------------------------------------------
// SD card + script browser
// ---------------------------------------------------------------------------

static bool sdMounted = false;
static std::vector<String> scripts;
static int browserSel = 0;

static bool sdMount() {
    if (sdMounted) return true;
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdMounted = SD.begin(SD_CS, SPI, 25000000);  // mounts VFS at /sd, so Lua io/dofile work
    return sdMounted;
}

static void scanScripts() {
    scripts.clear();
    browserSel = 0;
    if (!sdMount()) return;
    File root = SD.open("/");
    if (!root) return;
    File f;
    while ((f = root.openNextFile())) {
        String n = f.name();
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        if (!f.isDirectory() && n.endsWith(".lua")) scripts.push_back(n);
        f.close();
    }
    root.close();
}

static void runScript(const String& name) {
    mode = Mode::SCRIPT;
    termClear();
    termWrite(("=== " + name + " ===\n").c_str());

    String path = "/sd/" + name;
    if (luaL_dofile(L, path.c_str()) != LUA_OK) luaReportError();
    lua_settop(L, 0);

    termWrite("\n[done] esc: menu, enter: run again\n");
}

// ---------------------------------------------------------------------------
// REPL command history
// ---------------------------------------------------------------------------

static std::vector<String> history;
static int histPos = -1;  // -1 = editing a fresh line
static String liveLine;

static void histUp() {
    if (history.empty()) return;
    if (histPos == -1) {
        liveLine = inputLine;
        histPos = (int)history.size() - 1;
    } else if (histPos > 0) {
        --histPos;
    }
    inputLine = history[histPos];
    termDirty = true;
}

static void histDown() {
    if (histPos == -1) return;
    ++histPos;
    if (histPos >= (int)history.size()) {
        histPos = -1;
        inputLine = liveLine;
    } else {
        inputLine = history[histPos];
    }
    termDirty = true;
}

static void submitInput() {
    String cmd = inputLine;
    inputLine = "";
    if (cmd.length() && (history.empty() || history.back() != cmd)) history.push_back(cmd);
    if (history.size() > 50) history.erase(history.begin());
    histPos = -1;
    termDirty = true;
    runLine(cmd);
}

// ---------------------------------------------------------------------------
// Menu / browser rendering
// ---------------------------------------------------------------------------

static int menuSel = 0;

static void renderMenu() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Cardputer Lua", canvas.width() / 2, 12);

    const char* items[] = {"Run SD script", "Lua REPL"};
    for (int i = 0; i < 2; ++i) {
        bool sel = (i == menuSel);
        canvas.setTextColor(sel ? TFT_BLACK : TFT_GREEN, sel ? TFT_GREEN : TFT_BLACK);
        canvas.drawString(items[i], canvas.width() / 2, 58 + i * 26);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setTextDatum(bottom_center);
    canvas.drawString("fn+; fn+. move   enter: select", canvas.width() / 2, canvas.height() - 3);
    canvas.setTextDatum(top_left);
    canvas.pushSprite(0, 0);
}

static void renderBrowser() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("Scripts on /sd", 2, 2);

    int listTop = 14;
    int visible = (canvas.height() - listTop - 12) / (CHAR_H + 2);

    if (!sdMounted) {
        canvas.setTextColor(TFT_RED, TFT_BLACK);
        canvas.drawString("SD card not mounted", 2, listTop + 8);
    } else if (scripts.empty()) {
        canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
        canvas.drawString("no .lua files found", 2, listTop + 8);
    } else {
        int first = max(0, min(browserSel - visible / 2, (int)scripts.size() - visible));
        for (int i = first, row = 0; i < (int)scripts.size() && row < visible; ++i, ++row) {
            bool sel = (i == browserSel);
            canvas.setTextColor(sel ? TFT_BLACK : TFT_GREEN, sel ? TFT_GREEN : TFT_BLACK);
            canvas.drawString((sel ? "> " : "  ") + scripts[i], 2, listTop + row * (CHAR_H + 2));
        }
    }

    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setTextDatum(bottom_left);
    canvas.drawString("enter: run   esc: menu", 2, canvas.height() - 2);
    canvas.setTextDatum(top_left);
    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------

static bool wordHas(const Keyboard_Class::KeysState& st, char c) {
    for (auto k : st.word)
        if (k == c) return true;
    return false;
}

static void handleKeys() {
    if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
    auto st = M5Cardputer.Keyboard.keysState();

    bool esc = st.fn && wordHas(st, '`');
    bool up = st.fn && wordHas(st, ';');
    bool down = st.fn && wordHas(st, '.');

    switch (mode) {
        case Mode::MENU:
            if (up || down || wordHas(st, ';') || wordHas(st, '.')) {
                menuSel ^= 1;
                uiDirty = true;
            }
            if (st.enter) {
                if (menuSel == 0) {
                    sdMounted = false;  // re-probe so a freshly inserted card is found
                    scanScripts();
                    mode = Mode::BROWSER;
                } else {
                    mode = Mode::REPL;
                    termDirty = true;
                }
                uiDirty = true;
            }
            break;

        case Mode::BROWSER:
            if (esc) {
                mode = Mode::MENU;
                uiDirty = true;
                break;
            }
            if ((up || wordHas(st, ';')) && browserSel > 0) {
                --browserSel;
                uiDirty = true;
            }
            if ((down || wordHas(st, '.')) && browserSel + 1 < (int)scripts.size()) {
                ++browserSel;
                uiDirty = true;
            }
            if (st.enter && !scripts.empty()) {
                runScript(scripts[browserSel]);
            }
            break;

        case Mode::REPL:
            if (esc) {
                mode = Mode::MENU;
                uiDirty = true;
                break;
            }
            if (up) {
                histUp();
            } else if (down) {
                histDown();
            } else {
                if (!st.fn) {
                    for (auto c : st.word) {
                        inputLine += c;
                        termDirty = true;
                    }
                }
                if (st.del && inputLine.length()) {
                    inputLine.remove(inputLine.length() - 1);
                    termDirty = true;
                }
                if (st.enter) submitInput();
            }
            break;

        case Mode::SCRIPT:
            if (esc) {
                mode = Mode::MENU;
                uiDirty = true;
            } else if (st.enter && !scripts.empty()) {
                runScript(scripts[browserSel]);
            }
            break;
    }
}

// Serial is a second REPL input path in every mode, so functionality can be
// verified from the host with the PlatformIO CLI.
static void handleSerial() {
    static String serialLine;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialLine.length()) {
                String cmd = serialLine;
                serialLine = "";
                runLine(cmd);
            }
        } else if (isprint((unsigned char)c)) {
            serialLine += c;
        }
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // enableKeyboard
    Serial.begin(115200);

    M5Cardputer.Display.setRotation(1);
    canvas.setColorDepth(8);
    canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    termCols = canvas.width() / CHAR_W;
    termRows = canvas.height() / CHAR_H;

    luaInit();
    sdMount();

    termWrite(LUA_RELEASE " on M5 Cardputer\n");
    char buf[64];
    snprintf(buf, sizeof(buf), "free heap: %u, sd: %s\n", (unsigned)ESP.getFreeHeap(),
             sdMounted ? "mounted" : "not found");
    termWrite(buf);
    termWrite("Enter runs, ` interrupts, esc: menu\n");
}

void loop() {
    M5Cardputer.update();
    handleKeys();
    handleSerial();

    switch (mode) {
        case Mode::MENU:
            if (uiDirty) {
                renderMenu();
                uiDirty = false;
            }
            break;
        case Mode::BROWSER:
            if (uiDirty) {
                renderBrowser();
                uiDirty = false;
            }
            break;
        case Mode::REPL:
        case Mode::SCRIPT:
            if (uiDirty) {
                termDirty = true;
                uiDirty = false;
            }
            termRender();
            break;
    }
    delay(10);
}
