#include "control/input_inject.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <deque>
#include <sstream>

#include "core/log.h"
#include "game/bw_symbols.h"

namespace bw::input {

namespace {

// Read by the game's mouse thread, written by the game thread.
volatile LONG g_override = 0;
volatile LONG g_vr_override = 0;
volatile LONG g_x = 0, g_y = 0;

using GetCursorPosClientFn = int(__cdecl*)(int arg, POINT* pt);
GetCursorPosClientFn o_get_cursor = nullptr;

int __cdecl get_cursor_detour(int arg, POINT* pt) {
    int r = o_get_cursor(arg, pt);
    if ((g_override || g_vr_override) && pt) {
        pt->x = g_x;
        pt->y = g_y;
        return 1;
    }
    return r;
}

std::deque<std::string> g_queue;
ULONGLONG g_wait_until = 0;  // GetTickCount64 deadline for "wait"

HWND game_window() {
    struct Ctx {
        DWORD pid;
        HWND found;
    } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void post_button(bool down, bool right) {
    HWND h = game_window();
    if (!h) {
        BWLOG("input: no game window");
        return;
    }
    LPARAM lp = MAKELPARAM(g_x, g_y);
    UINT msg = right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    WPARAM wp = down ? (right ? MK_RBUTTON : MK_LBUTTON) : 0;
    PostMessageW(h, msg, wp, lp);
}

void run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "pos") {
        LONG x = 0, y = 0;
        in >> x >> y;
        InterlockedExchange(&g_x, x);
        InterlockedExchange(&g_y, y);
        InterlockedExchange(&g_override, 1);
    } else if (cmd == "down" || cmd == "up") {
        std::string b;
        in >> b;
        post_button(cmd == "down", b == "r");
    } else if (cmd == "click") {
        int x = 0, y = 0;
        std::string b;
        in >> x >> y >> b;
        char buf[64];
        snprintf(buf, sizeof(buf), "pos %d %d", x, y);
        // Expand in place, ahead of the rest of the queue.
        g_queue.push_front("up " + b);
        g_queue.push_front("wait 100");
        g_queue.push_front("down " + b);
        g_queue.push_front("wait 100");
        g_queue.push_front(buf);
        return;
    } else if (cmd == "key") {
        // key <name|vk-number>: posts WM_KEYDOWN + WM_KEYUP (e.g. "key esc" closes the menu).
        std::string k;
        in >> k;
        UINT vk = k == "esc" ? VK_ESCAPE : k == "enter" ? VK_RETURN : k == "space" ? VK_SPACE : k == "back" ? VK_BACK
                : UINT(strtoul(k.c_str(), nullptr, 0));
        if (HWND h = game_window()) {
            UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            PostMessageW(h, WM_KEYDOWN, vk, 1 | (LPARAM(scan) << 16));
            PostMessageW(h, WM_KEYUP, vk, 1 | (LPARAM(scan) << 16) | (1u << 30) | (1u << 31));
        }
    } else if (cmd == "type") {
        // type <text>: the rest of the line, as typed characters (WM_CHAR)
        std::string rest;
        std::getline(in >> std::ws, rest);
        type_text(std::wstring(rest.begin(), rest.end()), 0);
    } else if (cmd == "wait") {
        ULONGLONG ms = 0;
        in >> ms;
        g_wait_until = GetTickCount64() + ms;
    } else if (cmd == "release") {
        InterlockedExchange(&g_override, 0);
    } else if (!cmd.empty() && cmd[0] != '#') {
        BWLOG("input: unknown command '%s'", line.c_str());
        return;
    }
    if (!cmd.empty() && cmd[0] != '#') BWLOG("input: %s", line.c_str());
}

}  // namespace

// Typed characters waiting to be posted: one per frame (a burst of them came out as nothing).
std::deque<wchar_t> g_chars;

void type_text(const std::wstring& text, int backspaces) {
    // (The game's text fields take Backspace as a typed character: SetupEdit::Char handles 8.)
    for (int i = 0; i < backspaces; ++i) g_chars.push_back(8);
    for (wchar_t c : text) g_chars.push_back(c);
}

bool typing() { return !g_chars.empty(); }
void stop_typing() { g_chars.clear(); }

bool install_hooks() {
    if (MH_CreateHook(reinterpret_cast<void*>(sym::GetCursorPosClient), reinterpret_cast<void*>(&get_cursor_detour),
                      reinterpret_cast<void**>(&o_get_cursor)) != MH_OK) {
        BWLOG("FATAL: failed to hook GetCursorPosClient");
        return false;
    }
    BWLOG("hooked GetCursorPosClient at %08X", unsigned(sym::GetCursorPosClient));
    return true;
}

void queue_script(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        const auto last = line.find_last_not_of(" \t\r");
        if (first != std::string::npos) g_queue.push_back(line.substr(first, last - first + 1));
    }
}

void tick() {
    if (!g_chars.empty()) {
        if (HWND h = game_window()) PostMessageW(h, WM_CHAR, g_chars.front(), 1);
        g_chars.pop_front();
    }
    if (GetTickCount64() < g_wait_until) return;
    // One command per frame, so the mouse thread and window procedure see them in order.
    if (!g_queue.empty()) {
        std::string line = g_queue.front();
        g_queue.pop_front();
        run(line);
    }
}

void set_vr_cursor(bool on, long x, long y) {
    if (on) {
        InterlockedExchange(&g_x, x);
        InterlockedExchange(&g_y, y);
    }
    InterlockedExchange(&g_vr_override, on ? 1 : 0);
}

void vr_button(bool right, bool down) { post_button(down, right); }

void menu_click_check(long laser_x, long laser_y) {
    HWND h = game_window();
    HWND fg = GetForegroundWindow();
    POINT real{};
    GetCursorPos(&real);
    if (h) ScreenToClient(h, &real);
    const bool front = h && fg == h;
    bool brought = false;
    if (h && !front) {
        brought = SetForegroundWindow(h) != FALSE;
        if (!brought) BringWindowToTop(h);
    }
    static ULONGLONG logged_at = 0;
    if (!front || GetTickCount64() - logged_at > 5000) {
        logged_at = GetTickCount64();
        wchar_t title[64] = L"";
        if (fg) GetWindowTextW(fg, title, 64);
        BWLOG("input: a menu click: the game's window %s%s (in front: '%ls'); cursor override %ld at (%ld, %ld), the laser at (%ld, %ld), "
              "the real mouse at (%ld, %ld)", front ? "in front" : "NOT in front", front ? "" : brought ? ", brought to the front" : ", could not be brought to the front",
              title, long(g_vr_override), long(g_x), long(g_y), laser_x, laser_y, long(real.x), long(real.y));
    }
}

bool busy() { return GetTickCount64() < g_wait_until || !g_queue.empty(); }

}  // namespace bw::input
