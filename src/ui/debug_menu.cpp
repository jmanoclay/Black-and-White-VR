// VR Options (and, in a development build, Developer) in the game's own pause menu, as tabs beside
// "Main Menu" and "Statistics" (player, 2026-10-31: use the game's menu, not the tuning panel;
// 2026-11-01: the standard player VR options, move speed, snap turn, ... "as a tab, in a more natural
// spot", the debug settings only in the development build).
//
// The pause menu (Esc) is MainMenu: a SetupBox with five tab buttons (ids 2300-2304; only "Main
// Menu" and "Statistics" are used by the game) and the buttons Continue Game (id 0), Start Skirmish
// Game, Join Online Game, Options and Quit (id 4) under a welcome line (a SetupStaticText, id 999).
// The game makes it once at startup (MainMenu::Init, 0x53F910, keeps the MainMenu at 0xCD0B60; its
// box at +4) and calls the box's callback (box +0xB0, here 0x540040: stdcall(action, box, control,
// x, y); action 1 a click, the control's id at +0x18) for what happens on it.
//
// The third tab (2302) becomes "VR Options", the fourth (2303) "Developer" (BWVR_DEVELOPER builds).
// Either turns the box into a page of the mod's own buttons (the game's buttons and welcome line
// hidden): a title, the settings as lines "name: value" with - and + (a click on the line is + too),
// and Previous, Next, Back. The VR Options page has the player's options (hand::player_option_*:
// turning, snap angle, smooth turn speed, move speed, main hand, the two-hand grab, throw strength);
// the Developer page gives a miracle ("< Lightning bolt >" steps to the next, "Give miracle" closes
// the menu as Continue does and the mod makes the bubble in front of the hand: hand::give_miracle)
// and has every tuning setting (hand::option_*, the left hand's tuning panel's list). Settings are
// saved to bwvr.ini on Back, "Main Menu", Esc (the left hand's A) or when the menu closes.
//
// Buttons are made the game's way (operator new 0x7DB790 cdecl(size, file, line);
// SetupButton::SetupButton 0x4098B0 thiscall(id, x, y, w, h, font 0xC4CD30, 0), which joins the box in
// SetupBox::CurrentInitBox, 0xC4CD1C). A control's words are its text, inline from +0x24; its rect at
// +8 (x0, y0, x1, y1); it is hidden while the byte at +0x229 is set (SetupControl::Hide, 0x409300).
// (The game lays its own buttons out again as the menu shows; while a page is up they are kept hidden.)
//
// A switch is the game's own checkbox (SetupCheckBox::SetupCheckBox 0x410F10 thiscall(id, x, y, radio,
// checked, text, size); checked at +0x248, a click flips it), an action a wide button, a value the
// line with - and +. The open tab is drawn as the selected one (SetupTabButton +0x244: the others are
// drawn fainter), as the game's own tabs (player, 2026-11-03: the tab didn't look open).
#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "control/vr_hand.h"
#include "core/log.h"
#include "core/paths.h"
#include "ui/ui.h"
#include "vr/openvr_backend.h"

#ifndef BWVR_DEVELOPER
#define BWVR_DEVELOPER 1
#endif

namespace bw::ui {

namespace {

constexpr uintptr_t kMainMenu = 0xCD0B60, kCurrentInitBox = 0xC4CD1C, kCurrentActiveBox = 0xC4CD20;
constexpr uintptr_t kGameNew = 0x7DB790, kSetupButtonCtor = 0x4098B0, kFindControl = 0x408160, kMainMenuCallback = 0x540040;
constexpr uintptr_t kButtonFont = 0xC4CD30;
// The tabs: the game's "Main Menu" and two of its unused ones.
constexpr int kTabMain = 2300, kTabOptions = 2302, kTabDeveloper = 2303;
// The mod's controls' ids.
constexpr int kIdTitle = 0x4C10, kIdMiracle = 0x4C00, kIdGive = 0x4C01;
constexpr int kIdLine = 0x4C20, kIdMinus = 0x4C30, kIdPlus = 0x4C40;  // + row
constexpr int kIdPrev = 0x4C50, kIdNext = 0x4C51, kIdBack = 0x4C52;
constexpr int kIdCheck = 0x4C60;  // + row
constexpr uintptr_t kCheckBoxCtor = 0x410F10;
constexpr int kRows = 8;
// The game's own controls on the main page, hidden while a page is up.
constexpr int kGameIds[] = {0, 1, 2, 3, 4, 999};
// The miracles offered, in this order (GSpellSeedInfo types; the creature's spells, 10 and up, aren't).
constexpr int kTypes[] = {6, 0, 9, 3, 8, 2, 7, 4, 5, 1};
constexpr int kTypeCount = int(sizeof(kTypes) / sizeof(kTypes[0]));

using NewFn = void*(__cdecl*)(size_t, const char*, int);
using ButtonCtorFn = void*(__thiscall*)(void*, int, int, int, int, int, void*, int);
using CheckBoxCtorFn = void*(__thiscall*)(void*, int, int, int, bool, int, const wchar_t*, int);
using FindControlFn = char*(__thiscall*)(void*, int);
using CallbackFn = int(__stdcall*)(int, void*, void*, int, int);

enum class Page { kNone, kOptions, kDeveloper, kSetup };

// The first-run comfort setup (a new player in the headset: how to turn and move, before playing):
// the first time the menu is up with SteamVR running, a page of these VR options comes up by itself
// ("Start playing" closes the menu as Continue Game). [ui] first_setup_done=1 once left.
//
// A new game in the headset: over the new game's choice (start normally, skip to the creature's
// choice, ...: a SetupBox with the callback 0x544480, made by FrontEnd::Init) the same page pops up
// first, as its own box (the pause menu's, its tabs hidden), made active by
// SetupBox::SetCurrentActiveBox (0x407A30, cdecl(box): the game's own zoom in); "Start playing" goes
// back to the choice (player, 2026-11-03: the welcome page belongs there, not in the main menu).
const wchar_t* const kSetupKeys[] = {L"smooth_turn", L"snap_turn_deg", L"move_teleport", L"comfort_vignette", L"dominant_left", L"move_speed_percent", L"recenter_view"};
std::vector<int> g_setup;  // (indices into the VR options)
bool g_setup_done = false, g_setup_tried = false;
constexpr uintptr_t kSetActiveBox = 0x407A30, kNewGameCallback = 0x544480;
void* g_return_box = nullptr;      // (the new game's choice, to go back to from the popped-up page)
void* g_newgame_seen = nullptr;    // (the choice the page last popped up over: once each time it shows)
bool g_newgame_test = false;       // (ui first_setup_newgame 1: also without SteamVR, for the harness)
void setup_list() {
    g_setup.clear();
    for (const wchar_t* k : kSetupKeys)
        for (int i = 0; i < hand::player_option_count(); ++i)
            if (hand::player_option_key(i) == k) g_setup.push_back(i);
}

bool g_enabled = true;  // [ui] debug_menu (the VR Options tab and, in a development build, Developer)
void* g_box = nullptr;  // the pause menu's box, once the buttons are in it
int g_pick = 0;         // index into kTypes
Page g_page_on = Page::kNone;
int g_page = 0;         // which settings the page shows (kRows, or kRows - 1 on Developer, to a page)
CallbackFn o_callback = nullptr;

void* main_menu_box() {
    const char* menu = *reinterpret_cast<const char* const*>(kMainMenu);
    if (!menu || IsBadReadPtr(menu + 4, 4)) return nullptr;
    void* box = *reinterpret_cast<void* const*>(menu + 4);
    return box && !IsBadReadPtr(box, 0xB4) ? box : nullptr;
}

char* control(int id) { return g_box ? reinterpret_cast<FindControlFn>(kFindControl)(g_box, id) : nullptr; }

std::wstring miracle_name(int type) {
    std::wstring out;
    const char* n = reinterpret_cast<const char*>(0xD9D678 + type * 0x190 + 0x18);
    if (IsBadReadPtr(n, 24)) return L"?";
    for (int i = 0; i < 24 && n[i]; ++i) {  // ("LIGHTNING_BOLT" -> "Lightning bolt")
        const char c = n[i] == '_' ? ' ' : n[i];
        out += wchar_t(out.empty() ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void set_text(void* c, const std::wstring& text) {
    if (!c || IsBadWritePtr(static_cast<char*>(c) + 0x24, 0x100)) return;
    wchar_t* t = reinterpret_cast<wchar_t*>(static_cast<char*>(c) + 0x24);
    const size_t n = std::min<size_t>(text.size(), 100);
    for (size_t i = 0; i < n; ++i) t[i] = text[i];
    t[n] = 0;
}

void hide(char* c, bool hidden) {
    if (c && !IsBadWritePtr(c + 0x229, 1)) c[0x229] = hidden ? 1 : 0;
}

void set_top(char* c, int y, int h) {  // (its rect's top and bottom)
    if (!c || IsBadWritePtr(c + 8, 16)) return;
    int* r = reinterpret_cast<int*>(c + 8);
    r[1] = y, r[3] = y + h;
}
void set_width(char* c, int x0, int x1) {
    if (!c || IsBadWritePtr(c + 8, 16)) return;
    int* r = reinterpret_cast<int*>(c + 8);
    r[0] = x0, r[2] = x1;
}
// The tabs: the open one drawn as selected. (With the game's own pages up, its "Main Menu" or
// "Statistics", the game's tabs are left as it set them; only the mod's are drawn unselected.)
void set_tab_selected(int id, bool on) {
    char* t = control(id);
    if (t && !IsBadWritePtr(t + 0x244, 4)) *reinterpret_cast<int*>(t + 0x244) = on ? 1 : 0;
}
void mark_tabs() {
    if (g_page_on == Page::kNone) {
        set_tab_selected(kTabOptions, false), set_tab_selected(kTabDeveloper, false);
        return;
    }
    const int open = g_page_on == Page::kDeveloper ? kTabDeveloper : kTabOptions;
    for (int id : {kTabMain, kTabMain + 1, kTabOptions, kTabDeveloper}) set_tab_selected(id, id == open);
}
void show_tabs();

// The page's list: its size, a line's text, a step.
int list_count() {
    return g_page_on == Page::kDeveloper ? hand::option_count() : g_page_on == Page::kSetup ? int(g_setup.size()) : hand::player_option_count();
}
int list_index(int i) { return g_page_on == Page::kSetup ? (i >= 0 && i < int(g_setup.size()) ? g_setup[size_t(i)] : -1) : i; }
std::wstring list_text(int i) { return g_page_on == Page::kDeveloper ? hand::option_text(i) : hand::player_option_text(list_index(i)); }
int list_kind(int i) { return g_page_on == Page::kDeveloper ? hand::option_kind(i) : hand::player_option_kind(list_index(i)); }
std::wstring list_label(int i) { return g_page_on == Page::kDeveloper ? hand::option_label(i) : hand::player_option_label(list_index(i)); }
bool list_on(int i) { return g_page_on == Page::kDeveloper ? hand::option_on(i) : hand::player_option_on(list_index(i)); }
void list_step(int i, int dir) {
    if (g_page_on == Page::kDeveloper) hand::option_step(i, dir);
    else hand::player_option_step(list_index(i), dir);
}
// (The Developer page has the miracle row first: one line fewer.)
int rows_here() { return g_page_on == Page::kDeveloper ? kRows - 1 : kRows; }
int first_row_y() { return g_page_on == Page::kDeveloper ? 166 : 110; }
int page_count() { return std::max(1, (list_count() + rows_here() - 1) / rows_here()); }

// The page's words and which lines show.
void fill_page() {
    const bool on = g_page_on != Page::kNone, dev = g_page_on == Page::kDeveloper;
    set_text(control(kIdMiracle), L"< " + miracle_name(kTypes[g_pick]) + L" >");
    for (int id : {kIdMiracle, kIdGive}) hide(control(id), !dev);
    for (int r = 0; r < kRows; ++r) {
        const int i = g_page * rows_here() + r;
        const bool used = on && r < rows_here() && i < list_count();
        const int y = first_row_y() + r * 52;
        const int kind = used ? list_kind(i) : 0;
        for (int id : {kIdLine + r, kIdMinus + r, kIdPlus + r}) {
            set_top(control(id), y, 44);
            hide(control(id), !used || kind == 1 || (kind == 2 && id != kIdLine + r));
        }
        // (A value's line beside its - and +; an action's button the row's width.)
        set_width(control(kIdLine + r), 50, kind == 2 ? 750 : 610);
        set_text(control(kIdLine + r), !used ? L"" : kind == 2 ? list_label(i) : list_text(i));
        char* check = control(kIdCheck + r);
        hide(check, !used || kind != 1);
        if (check && used && kind == 1) {
            set_text(check, list_label(i));
            // (A 36 px box at the row's left, its words to its right (+0x244 0; the game's 2 put them
            // small under the box), as large as the other lines' (+0x20).)
            int* cr = reinterpret_cast<int*>(check + 8);
            if (!IsBadWritePtr(cr, 16)) cr[0] = 56, cr[1] = y + 4, cr[2] = 92, cr[3] = y + 40;
            if (!IsBadWritePtr(check + 0x244, 4)) *reinterpret_cast<int*>(check + 0x244) = 0;
            const char* line = control(kIdLine + r);
            int size = line && !IsBadReadPtr(line + 0x20, 4) ? *reinterpret_cast<const int*>(line + 0x20) : 0;
            if (size <= 0 && line && !IsBadReadPtr(line + 0x234, 4)) {  // (the button's font's size)
                const char* font = *reinterpret_cast<const char* const*>(line + 0x234);
                if (font && !IsBadReadPtr(font + 0xAC, 4)) size = *reinterpret_cast<const int*>(font + 0xAC);
            }
            if (size > 0 && !IsBadWritePtr(check + 0x20, 4)) *reinterpret_cast<int*>(check + 0x20) = size;
            if (!IsBadWritePtr(check + 0x248, 4)) *reinterpret_cast<int*>(check + 0x248) = list_on(i) ? 1 : 0;
        }
    }
    wchar_t title[96];
    if (g_page_on == Page::kSetup) swprintf(title, 96, L"Welcome to VR: how you turn and move (more under VR Options)");
    else swprintf(title, 96, L"%s  (page %d of %d; saved on Back)", dev ? L"Developer" : L"VR Options", g_page + 1, page_count());
    set_text(control(kIdTitle), title);
    set_text(control(kIdBack), g_page_on != Page::kSetup ? L"Back" : g_return_box ? L"Continue" : L"Start playing");
    for (int id : {kIdPrev, kIdNext}) hide(control(id), !on || page_count() < 2);
    mark_tabs();
    // (The welcome page is a popup of its own: no tabs.)
    show_tabs();
    // (The welcome page's one button in the middle, a little up from the box's bottom edge: it sat
    // pressed against it, player 2026-11-04.)
    if (char* back = control(kIdBack)) {
        const bool popup = g_page_on == Page::kSetup;
        set_width(back, popup ? 280 : 510, popup ? 520 : 750);
        set_top(back, popup ? 535 : 545, 46);
    }
}

// The tabs: the welcome page is a popup of its own, with one tab, "Welcome to VR", over it (with
// none the box's top border had a gap where the tabs go: player, 2026-11-04); otherwise the game's
// two, "VR Options" and (in a development build) "Developer", every frame (after the new game's
// "skip to the creature" the VR Options tab could be clicked but wasn't drawn).
void show_tabs() {
    const bool popup = g_page_on == Page::kSetup;
    hide(control(kTabMain), popup);
    if (char* stats = control(kTabMain + 1)) hide(stats, popup);
    set_text(control(kTabOptions), popup ? L"Welcome to VR" : L"VR Options");
    hide(control(kTabOptions), false);
#if BWVR_DEVELOPER
    hide(control(kTabDeveloper), popup);
#endif
}

// Which page shows: the game's main page, or one of the mod's.
void show(Page page) {
    if (page == Page::kNone && g_page_on != Page::kNone) set_tab_selected(kTabMain, true);  // (back to the main page)
    g_page_on = page;
    const bool on = page != Page::kNone;
    for (int id : kGameIds) hide(control(id), on);
    for (int id : {kIdTitle, kIdBack}) hide(control(id), !on);
    fill_page();
}

void* add_button(int id, int x, int y, int w, int h, const wchar_t* text) {
    void* mem = reinterpret_cast<NewFn>(kGameNew)(0x244, "bwvr menu", __LINE__);
    if (!mem) return nullptr;
    void* c = reinterpret_cast<ButtonCtorFn>(kSetupButtonCtor)(mem, id, x, y, w, h, reinterpret_cast<void*>(kButtonFont), 0);
    set_text(c, text);
    return c;
}

// The tabs and the pages' buttons go into the pause menu's box once it exists (it is made at startup).
void add_buttons() {
    void* box = main_menu_box();
    if (!box || g_box) return;
    void** init_box = reinterpret_cast<void**>(kCurrentInitBox);
    void* saved = *init_box;
    *init_box = box;
    add_button(kIdTitle, 50, 62, 700, 40, L"");
    add_button(kIdMiracle, 50, 110, 420, 44, L"");
    add_button(kIdGive, 480, 110, 270, 44, L"Give miracle");
    for (int r = 0; r < kRows; ++r) {
        add_button(kIdLine + r, 50, 110, 560, 44, L"");
        add_button(kIdMinus + r, 620, 110, 60, 44, L"-");
        add_button(kIdPlus + r, 690, 110, 60, 44, L"+");
        if (void* mem = reinterpret_cast<NewFn>(kGameNew)(0x260, "bwvr menu", __LINE__))
            reinterpret_cast<CheckBoxCtorFn>(kCheckBoxCtor)(mem, kIdCheck + r, 60, 118, false, 0, L"", 28);
    }
    add_button(kIdPrev, 50, 545, 220, 46, L"< Previous");
    add_button(kIdNext, 280, 545, 220, 46, L"Next >");
    add_button(kIdBack, 510, 545, 240, 46, L"Back");
    *init_box = saved;
    g_box = box;
    // The tabs: the game's unused third and fourth.
    set_text(control(kTabOptions), L"VR Options");
    hide(control(kTabOptions), false);
#if BWVR_DEVELOPER
    set_text(control(kTabDeveloper), L"Developer");
    hide(control(kTabDeveloper), false);
#endif
    show(Page::kNone);
    BWLOG("ui: pause menu: the VR Options tab%s added (box %p; %d options%s)", BWVR_DEVELOPER ? " and the Developer tab" : "", box,
          hand::player_option_count(), BWVR_DEVELOPER ? ", Developer: give a miracle and the tuning settings" : "");
}

void setup_seen() {
    if (g_page_on != Page::kSetup || g_setup_done) return;
    g_setup_done = true;
    WritePrivateProfileStringW(L"ui", L"first_setup_done", L"1", paths::ini_file().c_str());
    BWLOG("ui: the first-run comfort setup done");
}

void leave_page(const char* why) {
    if (g_page_on == Page::kNone) return;
    hand::options_save();
    setup_seen();
    show(Page::kNone);
    BWLOG("ui: pause menu: the mod's page left (%s); settings saved", why);
}

int g_callback_log = 0;  // debug (ui debug_menu_log N): the next N calls of the pause menu's callback (not the per-frame 0, 6, 13, 14)
int __stdcall callback_detour(int action, void* box, void* ctl, int x, int y) {
    if (g_callback_log > 0 && action != 6 && action != 0 && action != 13 && action != 14) {
        --g_callback_log;
        BWLOG("ui: debug menu: callback action %d box %p control %p (id %d) %d %d", action, box, ctl,
              ctl && !IsBadReadPtr(static_cast<char*>(ctl) + 0x18, 4) ? *reinterpret_cast<const int*>(static_cast<char*>(ctl) + 0x18) : -1, x, y);
    }
    if (action == 1 && box == g_box && ctl && !IsBadReadPtr(static_cast<char*>(ctl) + 0x18, 4)) {
        const int id = *reinterpret_cast<const int*>(static_cast<char*>(ctl) + 0x18);
        if (id == kTabOptions || (BWVR_DEVELOPER && id == kTabDeveloper)) {
            if (g_page_on != Page::kNone) hand::options_save();
            setup_seen();
            g_page = 0;
            show(id == kTabOptions ? Page::kOptions : Page::kDeveloper);
            BWLOG("ui: pause menu: the %s tab", id == kTabOptions ? "VR Options" : "Developer");
            return 0;
        }
        if (id == kTabMain && g_page_on != Page::kNone) {
            leave_page("Main Menu");
            return 0;
        }
        if (g_page_on == Page::kNone) return o_callback(action, box, ctl, x, y);
        if (id == kIdBack) {
            const bool setup = g_page_on == Page::kSetup;
            leave_page(setup ? "Start playing" : "Back");
            if (setup && g_return_box) {  // (popped up over the new game's choice: back to it)
                void* back = g_return_box;
                g_return_box = nullptr;
                reinterpret_cast<void(__cdecl*)(void*)>(kSetActiveBox)(back);
                BWLOG("ui: the welcome page: back to the new game's choice");
                return 0;
            }
            if (setup) {  // (straight into the game, as Continue Game)
                char* cont = control(0);
                return cont ? o_callback(1, box, cont, x, y) : 0;
            }
            return 0;
        }
        if (id == kIdPrev || id == kIdNext) {
            g_page = (g_page + (id == kIdNext ? 1 : page_count() - 1)) % page_count();
            fill_page();
            BWLOG("ui: pause menu: page %d", g_page + 1);
            return 0;
        }
        if (id == kIdMiracle) {
            g_pick = (g_pick + 1) % kTypeCount;
            fill_page();
            BWLOG("ui: debug menu: miracle %d (%ls)", kTypes[g_pick], miracle_name(kTypes[g_pick]).c_str());
            return 0;
        }
        if (id >= kIdCheck && id < kIdCheck + kRows) {  // (a switch's checkbox: the switch flipped)
            const int i = g_page * rows_here() + (id - kIdCheck);
            list_step(i, 1);
            fill_page();
            return 0;
        }
        if (id >= kIdLine && id < kIdPlus + kRows) {
            const int r = (id - kIdLine) % 0x10, i = g_page * rows_here() + r;
            list_step(i, id >= kIdMinus && id < kIdMinus + kRows ? -1 : 1);
            fill_page();
            return 0;
        }
        if (id == kIdGive) {
            hand::give_miracle(kTypes[g_pick]);
            BWLOG("ui: debug menu: give %ls: the menu closes (as Continue Game)", miracle_name(kTypes[g_pick]).c_str());
            leave_page("Give miracle");
            char* cont = control(0);
            return cont ? o_callback(1, box, cont, x, y) : 0;
        }
    }
    return o_callback(action, box, ctl, x, y);
}

}  // namespace

bool debug_menu_install() {
    g_enabled = paths::ini_int(L"ui", L"debug_menu", 1) != 0;
    g_setup_done = paths::ini_int(L"ui", L"first_setup_done", 0) != 0;
    if (!g_enabled) return true;
    if (MH_CreateHook(reinterpret_cast<void*>(kMainMenuCallback), reinterpret_cast<void*>(&callback_detour),
                      reinterpret_cast<void**>(&o_callback)) != MH_OK) {  // (enabled with the others, at startup)
        BWLOG("ui: debug menu: could not hook the pause menu's callback");
        g_enabled = false;
        return false;
    }
    return true;
}

void debug_menu_tick() {
    if (!g_enabled) return;
    if (!g_box) {
        add_buttons();
        return;
    }
    // A new game's choice in the headset: the welcome page pops up over it first.
    {
        void* active = *reinterpret_cast<void* const*>(kCurrentActiveBox);
        const bool newgame = active && active != g_box && !IsBadReadPtr(static_cast<char*>(active) + 0xB0, 4) &&
                             *reinterpret_cast<const uintptr_t*>(static_cast<char*>(active) + 0xB0) == kNewGameCallback;
        if (!newgame && active != g_box) g_newgame_seen = nullptr;
        if (newgame && active != g_newgame_seen && g_page_on == Page::kNone && (vr::openvr::active() || g_newgame_test)) {
            g_newgame_seen = g_return_box = active;
            setup_list();
            g_page = 0;
            reinterpret_cast<void(__cdecl*)(void*)>(kSetActiveBox)(g_box);
            show(Page::kSetup);
            g_setup_tried = true;
            BWLOG("ui: the welcome page popped up over the new game's choice (%zu options)", g_setup.size());
            return;
        }
    }
    // The first-run setup: the first time the menu is up in the headset.
    if (!g_setup_done && !g_setup_tried && g_page_on == Page::kNone && vr::openvr::active() &&
        *reinterpret_cast<void* const*>(kCurrentActiveBox) == g_box) {
        g_setup_tried = true;
        setup_list();
        g_page = 0;
        show(Page::kSetup);
        BWLOG("ui: the first-run comfort setup shown (%zu options)", g_setup.size());
    }
    if (g_page_on != Page::kNone) {
        // (The menu closed with a page up: saved, and the main page again for next time. While it is
        // up, the game's own buttons stay hidden: it lays them out again itself.)
        if (*reinterpret_cast<void* const*>(kCurrentActiveBox) != g_box) {
            leave_page("the menu closed");
            g_return_box = nullptr;
        }
        else
            for (int id : kGameIds) hide(control(id), true);
    }
    if (*reinterpret_cast<void* const*>(kCurrentActiveBox) == g_box) {  // (the tabs, every frame the menu is up)
        show_tabs();
        mark_tabs();
    }
}

// Esc (the left hand's A) with a page up: the game's Esc goes by its Continue button, hidden there, and
// did nothing (playtest 2026-11-01). The page is left and the menu closed as Continue Game does. False
// when no page is up (Esc goes to the game as before).
bool debug_menu_escape() {
    if (!g_enabled || g_page_on == Page::kNone || !g_box || *reinterpret_cast<void* const*>(kCurrentActiveBox) != g_box) return false;
    if (g_page_on == Page::kSetup && g_return_box) {  // (the popup over the new game's choice: back to it)
        leave_page("Esc");
        void* back = g_return_box;
        g_return_box = nullptr;
        reinterpret_cast<void(__cdecl*)(void*)>(kSetActiveBox)(back);
        BWLOG("ui: the welcome page: Esc, back to the new game's choice");
        return true;
    }
    leave_page("Esc");
    char* cont = control(0);
    if (cont && o_callback) o_callback(1, g_box, cont, 0, 0);
    BWLOG("ui: debug menu: Esc on the mod's page: the menu closes (as Continue Game)");
    return true;
}

bool debug_menu_command(const std::string& what, std::istringstream& in) {
    if (what == "debug_menu_pick") {  // debug: ui debug_menu_pick <type>: the miracle the Developer page offers
        int t = -1;
        in >> t;
        for (int i = 0; i < kTypeCount; ++i)
            if (kTypes[i] == t) g_pick = i;
        fill_page();
        return true;
    }
    if (what == "debug_menu_escape") {  // debug: as the left hand's A
        debug_menu_escape();
        return true;
    }
    if (what == "first_setup_newgame") {  // debug: ui first_setup_newgame 1: the popup over a new game's choice without SteamVR
        in >> g_newgame_test;
        return true;
    }
    if (what == "first_setup") {  // debug: ui first_setup: the first-run comfort setup page, now (the menu must be up)
        setup_list();
        g_page = 0;
        show(Page::kSetup);
        BWLOG("ui: the first-run comfort setup shown (%zu options; forced)", g_setup.size());
        return true;
    }
    if (what == "debug_menu_log") {
        in >> g_callback_log;
        return true;
    }
    if (what == "debug_menu_state") {
        const char* names[] = {"off", "options", "developer", "setup"};
        BWLOG("ui: debug menu: box %p, active box %p, offers %d (%ls), page %s %d", g_box, *reinterpret_cast<void* const*>(kCurrentActiveBox), kTypes[g_pick],
              miracle_name(kTypes[g_pick]).c_str(), names[int(g_page_on)], g_page + 1);
        for (int r = 0; r < rows_here(); ++r) {
            const int i = g_page * rows_here() + r;
            if (g_page_on != Page::kNone && i < list_count()) BWLOG("ui: debug menu: line %d: %ls", r, list_text(i).c_str());
        }
        return true;
    }
    return false;
}

}  // namespace bw::ui
