// Naming a save in VR. The temple's save room asks for a "Save Game Message" (DialogBoxSaveMessage:
// a SetupEdit text field, OK and Cancel), which needs a keyboard. In VR the field is filled in with
// a default name the player can accept with the laser, and SteamVR's own keyboard is opened over
// the scene; each key pressed there goes into the field (Backspace too) as it is pressed.
// Characters reach the field as the game's window messages (WM_CHAR), as typing would. The flat
// game is left alone.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

#include "control/input_inject.h"
#include "core/log.h"
#include "core/paths.h"
#include "game/engine.h"
#include "ui/ui.h"
#include "vr/openvr_backend.h"

namespace bw::ui {

namespace {

// DialogBoxSaveMessage: made once at startup (GGame::InitOneTimeOnly keeps it at GGame+0x25030C,
// and its Init makes the text field: a SetupEdit, id 0x7D2, with OK and
// Cancel, 0x7D0 / 0x7D1). It is up while it is the current SetupBox (DialogBoxBase::IsVisible).
constexpr uintptr_t kGameSaveMessageBox = 0x25030C, kDialogIsVisible = 0x513770;
constexpr int kSaveMaxChars = 24;  // (what the field took in testing; a save keeps all of it)
// NewProfileBox::Init keeps the box at 0xCD0B24 (its name field is a SetupEdit; OK and Cancel).
constexpr uintptr_t kNewProfileBox = 0xCD0B24;
constexpr int kPlayerMaxChars = 16;
using IsVisibleFn = unsigned(__thiscall*)(void* box);

// Which dialog asks for a name.
enum class Ask { kNone, kSave, kPlayer };
Ask g_ask = Ask::kNone;
int g_max_chars = kSaveMaxChars;

enum class State { kClosed, kOpened, kKeyboard, kDone };
State g_state = State::kClosed;
int g_typed = 0;  // characters in the field that we typed (Backspaces to replace them)
bool g_enabled = true;  // [ui] save_name

bool box_up(void* box) {
    return box && !IsBadReadPtr(box, 8) && reinterpret_cast<IsVisibleFn>(kDialogIsVisible)(box) != 0;
}

Ask dialog_up() {
    void* game = game::game_instance();
    if (box_up(game ? game::game_field<void*>(game, kGameSaveMessageBox) : nullptr)) return Ask::kSave;
    if (box_up(*reinterpret_cast<void* const*>(kNewProfileBox))) return Ask::kPlayer;
    return Ask::kNone;
}
const char* ask_name(Ask a) { return a == Ask::kPlayer ? "new player's name" : "save's name"; }

std::wstring default_name() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[40];
    wcsftime(buf, 40, L"VR save %d %b %H:%M", &tm);
    return buf;
}

// Replaces what we typed in the field with `text` (printable characters, the field's length).
void put_text(const std::wstring& text) {
    std::wstring clean;
    for (wchar_t c : text)
        if (c >= 32 && c != 127 && int(clean.size()) < g_max_chars) clean += c;
    input::type_text(clean, g_typed);
    g_typed = int(clean.size());
    char narrow[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, clean.c_str(), -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    BWLOG("ui: the %s typed: '%s'", ask_name(g_ask), narrow);
}

void keyboard_text(const std::string& utf8) {
    wchar_t wide[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide, 255);
    put_text(wide);
}

// Keys pressed on SteamVR's keyboard (minimal mode: as they are pressed): straight into the field.
// Backspace (the character 8) takes one character off; Enter is left out (Done ends it).
void keyboard_keys(const std::string& utf8) {
    wchar_t wide[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide, 63);
    std::wstring add;
    int back = 0;
    for (const wchar_t* c = wide; *c; ++c) {
        if (*c == 8) {
            if (!add.empty()) add.pop_back();
            else ++back;
        } else if (*c >= 32 && *c != 127 && g_typed - back + int(add.size()) < g_max_chars) {
            add += *c;
        }
    }
    input::type_text(add, back);
    g_typed = std::max(0, g_typed - back) + int(add.size());
    BWLOG("ui: keyboard keys: %d back, %d typed (the field has about %d)", back, int(add.size()), g_typed);
}

}  // namespace

void configure_save_name() { g_enabled = paths::ini_int(L"ui", L"save_name", 1) != 0; }

void save_name_tick(bool vr) {
    const Ask up = dialog_up();
    if (up != g_ask && g_state != State::kClosed) {  // (closed, or another one in its place)
        if (g_state == State::kKeyboard) vr::openvr::hide_keyboard();
        input::stop_typing();  // (nothing left over goes to the world)
        BWLOG("ui: the %s dialog closed", ask_name(g_ask));
        g_state = State::kClosed;
    }
    g_ask = up;
    if (up == Ask::kNone) return;
    if (g_state == State::kClosed) {
        g_state = State::kOpened;
        g_max_chars = up == Ask::kPlayer ? kPlayerMaxChars : kSaveMaxChars;
        BWLOG("ui: the %s dialog opened", ask_name(up));
    }
    if (!vr || !g_enabled) return;
    if (g_state == State::kOpened) {
        g_typed = 0;
        // A save gets a name to accept as it is; a new player's starts empty.
        const std::wstring name = up == Ask::kSave ? default_name() : std::wstring();
        if (!name.empty()) put_text(name);
        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        const char* title = up == Ask::kPlayer ? "Your name" : "Name this save";
        g_state = vr::openvr::show_keyboard(title, narrow, g_max_chars) ? State::kKeyboard : State::kDone;
    } else if (g_state == State::kKeyboard) {
        std::string keys;
        const int r = vr::openvr::poll_keyboard(keys);
        if (!keys.empty()) keyboard_keys(keys);
        if (r != 0) g_state = State::kDone;
        if (r > 0) BWLOG("ui: the keyboard's Done: the name is what is in the field");
        else if (r < 0) BWLOG("ui: the keyboard closed without Done: the name is what is in the field");
    }
}

bool save_name_command(const std::string& what, std::istringstream& in) {
    if (what == "save_name") in >> g_enabled;
    else if (what == "save_name_typed") {  // debug: as if the player typed this on SteamVR's keyboard and pressed Done
        std::string rest;
        std::getline(in >> std::ws, rest);
        if (g_state == State::kClosed) BWLOG("ui: no name dialog is open");
        else {
            if (g_state == State::kKeyboard) vr::openvr::hide_keyboard();
            g_state = State::kDone;
            keyboard_text(rest);
        }
    } else if (what == "save_name_keys") {  // debug: as if these keys were pressed on SteamVR's keyboard ("\b" = Backspace)
        std::string rest, keys;
        std::getline(in >> std::ws, rest);
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == '\\' && i + 1 < rest.size() && rest[i + 1] == 'b') keys += char(8), ++i;
            else keys += rest[i];
        }
        if (g_state == State::kClosed) BWLOG("ui: no name dialog is open");
        else keyboard_keys(keys);
    } else return false;
    return true;
}

}  // namespace bw::ui
