#include "core/paths.h"

namespace bw::paths {

static HINSTANCE g_module = nullptr;
static std::wstring g_game, g_bwvr, g_ini, g_real, g_system_ddraw;

void set_module(HINSTANCE inst) { g_module = inst; }

void init() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(g_module, buf, MAX_PATH);  // the proxy lives next to runblack.exe
    std::wstring dir = buf;
    g_game = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    g_bwvr = g_game + L"bwvr\\";
    g_ini = g_bwvr + L"bwvr.ini";
    CreateDirectoryW(g_bwvr.c_str(), nullptr);
    CreateDirectoryW((g_bwvr + L"logs").c_str(), nullptr);
    CreateDirectoryW((g_bwvr + L"captures").c_str(), nullptr);
    CreateDirectoryW((g_bwvr + L"control").c_str(), nullptr);

    wchar_t chain[MAX_PATH] = {};
    GetPrivateProfileStringW(L"proxy", L"chain", L"", chain, MAX_PATH, g_ini.c_str());
    // A 32-bit process asking for System32 is redirected to SysWOW64 by WOW64.
    GetSystemDirectoryW(buf, MAX_PATH);
    g_system_ddraw = std::wstring(buf) + L"\\ddraw.dll";
    g_real = g_system_ddraw;
    if (chain[0]) {
        // (relative to the game's folder unless it has a drive or starts with a slash)
        const bool absolute = (chain[0] && chain[1] == L':') || chain[0] == L'\\' || chain[0] == L'/';
        g_real = absolute ? std::wstring(chain) : g_game + chain;
    }
}

const std::wstring& system_ddraw() { return g_system_ddraw; }
const std::wstring& game_dir() { return g_game; }
const std::wstring& bwvr_dir() { return g_bwvr; }
const std::wstring& ini_file() { return g_ini; }
const std::wstring& real_ddraw() { return g_real; }

int ini_int(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini.c_str()));
}

}  // namespace bw::paths
