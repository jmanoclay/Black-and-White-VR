#include "core/mod.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <string>

#include "control/input_inject.h"
#include "core/audio_balance.h"
#include "core/audio_stop.h"
#include "core/load_trace.h"
#include "core/exe_check.h"
#include "core/log.h"
#include "core/paths.h"
#include "ui/ui.h"
#include "game/bw_symbols.h"
#include "game/camdump.h"
#include "render/d3d7_trace.h"
#include "render/d3d9_bridge.h"
#include "render/mipmaps.h"
#include "render/screenshot.h"
#include "render/stereo.h"
#include "render/video_screen.h"

namespace bw::mod {

namespace {

using FlipFn = int(__thiscall*)(void* screen, int arg);
FlipFn o_flip = nullptr;

unsigned g_frame = 0;
int g_live_ms = 0;  // [debug] live_screenshot_ms: periodic bwvr\captures\live.bmp
LARGE_INTEGER g_freq, g_last_live, g_last_stats;
unsigned g_frames_at_stats = 0;

// Measurement (Phase 0.7): cost of one render pass, simulation turn rate.
double g_draw_ms_sum = 0, g_draw_ms_max = 0;
unsigned g_draw_calls = 0, g_turns = 0;
double g_gfx_ms_sum = 0, g_gfx_ms_max = 0;
unsigned g_gfx_calls = 0;

double seconds_since(const LARGE_INTEGER& t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - t.QuadPart) / double(g_freq.QuadPart);
}

void* game_device() { return *reinterpret_cast<void**>(sym::LH3DRender_Direct3DDevice7); }

// Control files let the test harness talk to the running game without any IPC setup.
// capture.req: "<frames>"           -> trace the next N frames to bwvr\captures\trace_*
// screenshot.req: "<name>"          -> save bwvr\captures\<name>.bmp at the end of this frame
bool take_request(const wchar_t* name, char* out, size_t out_size) {
    std::wstring path = paths::bwvr_dir() + L"control\\" + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    out[0] = 0;
    if (FILE* f = _wfopen(path.c_str(), L"r")) {
        if (!fgets(out, int(out_size), f)) out[0] = 0;
        fclose(f);
    }
    DeleteFileW(path.c_str());
    for (char* p = out; *p; ++p)
        if (*p == '\r' || *p == '\n') *p = 0;
    return true;
}

std::string take_request_text(const wchar_t* name) {
    std::wstring path = paths::bwvr_dir() + L"control\\" + name;
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        DeleteFileW(path.c_str());
    }
    return text;
}

void poll_control(void* device) {
    // input.req: an input script (see control/input_inject.h)
    std::string script = take_request_text(L"input.req");
    if (!script.empty()) input::queue_script(script);

    // stereo.req: stereo renderer commands (see render/stereo.h), one per line
    std::string stereo_cmds = take_request_text(L"stereo.req");
    size_t start = 0;
    while (start < stereo_cmds.size()) {
        size_t end = stereo_cmds.find('\n', start);
        if (end == std::string::npos) end = stereo_cmds.size();
        std::string line = stereo_cmds.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        stereo::apply_command(line);
        start = end + 1;
    }

    char arg[260];
    if (take_request(L"zstats.req", arg, sizeof(arg)) && device) screenshot::log_depth_stats(device);
    if (take_request(L"camdump.req", arg, sizeof(arg))) {
        std::wstring name = arg[0] ? std::wstring(arg, arg + strlen(arg)) : L"camdump";
        game::dump_camera(paths::bwvr_dir() + L"captures\\" + name + L".json");
    }
    if (take_request(L"capture.req", arg, sizeof(arg))) {
        int frames = atoi(arg);
        trace::request_capture(frames > 0 ? frames : 1);
    }
    if (take_request(L"screenshot.req", arg, sizeof(arg)) && device) {
        std::wstring name = arg[0] ? std::wstring(arg, arg + strlen(arg)) : L"shot";
        std::wstring path = paths::bwvr_dir() + L"captures\\" + name + L".bmp";
        BWLOG("screenshot %ls: %s", path.c_str(), screenshot::save_render_target(device, path) ? "ok" : "FAILED");
    }
}

void write_status(double fps, void* device) {
    std::wstring path = paths::bwvr_dir() + L"control\\status.txt";
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        fprintf(f, "frame=%u\nfps=%.1f\ndevice=%d\ntick=%lu\n", g_frame, fps, device ? 1 : 0, GetTickCount());
        fclose(f);
    }
}

void on_frame_end() {
    ++g_frame;
    void* device = game_device();
    if (device) trace::ensure_hooked(device);
    trace::end_frame(device, g_frame);

    if (g_frame % 10 == 0) poll_control(device);
    input::tick();

    if (g_live_ms > 0 && device && seconds_since(g_last_live) * 1000.0 >= g_live_ms) {
        QueryPerformanceCounter(&g_last_live);
        screenshot::save_render_target(device, paths::bwvr_dir() + L"captures\\live.bmp");
    }

    double dt = seconds_since(g_last_stats);
    if (dt >= 1.0) {
        double fps = (g_frame - g_frames_at_stats) / dt;
        write_status(fps, device);
        static int s_n = 0;
        static double s_window = 0;
        s_window += dt;
        if (++s_n % 5 == 0) {
            BWLOG("frame %u, %.1f fps, %s", g_frame, fps, trace::stats_line().c_str());
            BWLOG("  GGame::Draw: %u calls, avg %.3f ms, max %.3f ms | game turns: %.1f/s", g_draw_calls,
                  g_draw_calls ? g_draw_ms_sum / g_draw_calls : 0.0, g_draw_ms_max, g_turns / s_window);
            BWLOG("  %s", stereo::stats_line().c_str());
            BWLOG("  %s", mipmaps::stats_line().c_str());
            load_trace::report();
            BWLOG("  GGame::ProcessGraphicsEngine: %u calls, avg %.3f ms, max %.3f ms", g_gfx_calls,
                  g_gfx_calls ? g_gfx_ms_sum / g_gfx_calls : 0.0, g_gfx_ms_max);
            g_gfx_ms_sum = g_gfx_ms_max = 0;
            g_gfx_calls = 0;
            g_draw_ms_sum = g_draw_ms_max = 0;
            g_draw_calls = g_turns = 0;
            s_window = 0;
        }
        g_frames_at_stats = g_frame;
        QueryPerformanceCounter(&g_last_stats);
    }
}

int g_flip_every = 1;  // (experiment: show the game's window only every Nth frame)
// In a window the game shows its frame by blitting the back buffer onto the primary surface at the
// window's place on the desktop (LHScreen::LHFlip; LHScreen +0x30 the primary, +0x34 the back
// buffer, +0x58/+0x5C the client area's position, +0x64 windowed). Through dxwrapper that never
// reached the window unless it sat in the desktop's corner (it kept the loading screen). So
// through dxwrapper the mod shows the frame itself: the back buffer onto dxwrapper's Direct3D 9
// window, before the game's flip (which clears the back buffer afterwards).
int __fastcall flip_detour(void* screen, void* /*edx*/, int arg) {
    on_frame_end();
    if (screen) video_screen::presented(*reinterpret_cast<void**>(static_cast<char*>(screen) + 0x34));  // (no VR frame: on the headset's screen)
    static unsigned n = 0;
    if (g_flip_every > 1 && (n++ % unsigned(g_flip_every)) != 0) return 1;
    if (!screen || !render::d3d9::available() || !*reinterpret_cast<int*>(static_cast<char*>(screen) + 0x64)) return o_flip(screen, arg);
    // (The game's own blit is sent off the primary surface: dxwrapper rejects it at once and puts
    // nothing in the window, which cost a present every frame. The mod's frame goes to the window
    // every other frame in VR (60 a second at 120 Hz: the window is for watching and recording),
    // every frame otherwise.)
    static unsigned frame = 0;
    const bool show = !stereo::active() || (frame++ & 1) == 0;
    const bool own = show && render::d3d9::hold_for_window(*reinterpret_cast<void**>(static_cast<char*>(screen) + 0x34));
    int* at = reinterpret_cast<int*>(static_cast<char*>(screen) + 0x58);
    const int x = at[0], y = at[1];
    at[0] = at[1] = 1 << 20;
    const int r = o_flip(screen, arg);
    at[0] = x, at[1] = y;
    if (own) render::d3d9::show_held();
    return r;
}

// The game refuses to start unless DSETUP reports DirectX >= 7. DSETUP reads
// HKLM\Software\Microsoft\DirectX with KEY_ALL_ACCESS, which fails without admin rights, and its
// fallback probe tops out at "6" on modern Windows. Report what that registry key actually holds.
using GetDxVersionFn = int(WINAPI*)(DWORD* version, DWORD* revision);
GetDxVersionFn o_get_dx_version = nullptr;

int WINAPI get_dx_version_detour(DWORD* version, DWORD* revision) {
    DWORD v = 0, r = 0;
    int ok = o_get_dx_version(&v, &r);
    if (v < 0x00040007) {
        BWLOG("DirectXSetupGetVersion reported %08lX.%08lX; reporting 4.09.00.0904 instead", v, r);
        v = 0x00040009;
        r = 0x00000904;
        ok = 1;
    }
    if (version) *version = v;
    if (revision) *revision = r;
    return ok;
}

// --- Measurement hooks (Phase 0.7): cost of one render pass, simulation turn rate ---
using GameMethodFn = void(__thiscall*)(void* game);
GameMethodFn o_game_draw = nullptr, o_game_turn = nullptr;

void __fastcall game_draw_detour(void* game, void*) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    o_game_draw(game);
    ui::draw_overlays();  // (the mod's own panels, in each eye pass, with the game's 2D)
    QueryPerformanceCounter(&b);
    double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(g_freq.QuadPart);
    g_draw_ms_sum += ms;
    if (ms > g_draw_ms_max) g_draw_ms_max = ms;
    ++g_draw_calls;
}

using GraphicsEngineFn = void(__thiscall*)(void* game, unsigned a, unsigned b);
GraphicsEngineFn o_graphics = nullptr;

void __fastcall graphics_detour(void* game, void*, unsigned a, unsigned b) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    o_graphics(game, a, b);
    QueryPerformanceCounter(&t1);
    double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
    g_gfx_ms_sum += ms;
    if (ms > g_gfx_ms_max) g_gfx_ms_max = ms;
    ++g_gfx_calls;
}

void __fastcall game_turn_detour(void* game, void*) {
    ++g_turns;
    o_game_turn(game);
}

bool hook(uintptr_t target, void* detour, void** original, const char* name) {
    if (MH_CreateHook(reinterpret_cast<void*>(target), detour, original) != MH_OK) {
        BWLOG("FATAL: failed to hook %s at %08X", name, unsigned(target));
        return false;
    }
    BWLOG("hooked %s at %08X", name, unsigned(target));
    return true;
}

// A crash: where, and the code addresses on the stack (the game's and the mod's), into the log.
// Windows' own record only had "address 0" for a crash as the portal's scene ended (player,
// 2026-10-24): which call got there was lost.
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
bool in_module(uintptr_t a, HMODULE m) {
    if (!m) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return a >= uintptr_t(base) && a < uintptr_t(base) + nt->OptionalHeader.SizeOfImage;
}
// Not in a module: one unloaded already? Windows keeps the last unloaded ones (ntdll's
// RtlGetUnloadEventTraceEx: entries of base, size, sequence, time stamp, checksum, name, ...).
void log_unloaded(uintptr_t a) {
    struct Entry {
        void* base;
        SIZE_T size;
        ULONG sequence, time_stamp, checksum;
        WCHAR name[32];
    };
    using TraceExFn = void(NTAPI*)(ULONG**, ULONG**, void**);
    const auto fn = reinterpret_cast<TraceExFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetUnloadEventTraceEx"));
    ULONG *size_p = nullptr, *count_p = nullptr;
    void* trace_p = nullptr;
    if (fn) fn(&size_p, &count_p, &trace_p);
    if (!size_p || !count_p || !trace_p || IsBadReadPtr(trace_p, sizeof(void*))) return;
    const auto* arr = *static_cast<const char* const*>(trace_p);
    const ULONG size = *size_p, count = *count_p;
    if (!arr || size < sizeof(Entry) || IsBadReadPtr(arr, size_t(size) * count)) return;
    for (ULONG i = 0; i < count; ++i) {
        const auto* e = reinterpret_cast<const Entry*>(arr + size_t(i) * size);
        if (!e->base) continue;
        const uintptr_t b = reinterpret_cast<uintptr_t>(e->base);
        WCHAR n[33] = {};
        memcpy(n, e->name, sizeof(e->name));
        BWLOG("CRASH: %s %ls at %p, %lu bytes (unload %lu)", a >= b && a < b + e->size ? "... in, unloaded already:" : "unloaded earlier:", n, e->base,
              static_cast<unsigned long>(e->size), e->sequence);
    }
}
LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    static bool once = false;
    if (!once && ep && ep->ExceptionRecord && ep->ContextRecord) {
        once = true;
        stereo::stop_spike_watch();
        const CONTEXT& c = *ep->ContextRecord;
        HMODULE game = GetModuleHandleW(nullptr), mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&crash_filter), &mod);
        BWLOG("CRASH: code %08X at %p (eip %08X esp %08X ebp %08X eax %08X ecx %08X edx %08X); the mod is loaded at %p",
              unsigned(ep->ExceptionRecord->ExceptionCode), ep->ExceptionRecord->ExceptionAddress, unsigned(c.Eip), unsigned(c.Esp),
              unsigned(c.Ebp), unsigned(c.Eax), unsigned(c.Ecx), unsigned(c.Edx), static_cast<void*>(mod));
        {   // (Which module the crash is in, and where in it: the crash on quitting was in one loaded late.)
            HMODULE at = nullptr;
            wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &at) && at)
                GetModuleFileNameW(at, name, MAX_PATH);
            BWLOG("CRASH: in %ls +%08X", name, unsigned(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(at)));
            if (!at) log_unloaded(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
        }
        std::string chain;
        const auto* sp = reinterpret_cast<const uint32_t*>(c.Esp);
        int found = 0;
        for (int i = 0; i < 512 && found < 24 && !IsBadReadPtr(sp + i, 4); ++i) {
            const uintptr_t a = sp[i];
            const char* where = in_module(a, game) ? "game" : in_module(a, mod) ? "mod" : nullptr;
            char buf[96];
            if (!where) {  // (another module's code: its name and offset, e.g. dxwrapper's)
                HMODULE other = nullptr;
                char path[MAX_PATH];
                if (a < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                       reinterpret_cast<LPCSTR>(a), &other) || !other || !GetModuleFileNameA(other, path, MAX_PATH))
                    continue;
                const char* base_name = strrchr(path, 92) ? strrchr(path, 92) + 1 : path;  // (92: a backslash)
                snprintf(buf, sizeof(buf), " %s+%X@%d", base_name, unsigned(a - reinterpret_cast<uintptr_t>(other)), i);
                chain += buf;
                ++found;
                continue;
            }
            snprintf(buf, sizeof(buf), " %s:%08X@%d", where, unsigned(a - (where[0] == 'g' ? 0 : uintptr_t(mod)) + (where[0] == 'g' ? 0 : 0x10000000)), i);
            chain += buf;
            ++found;
        }
        BWLOG("CRASH: code addresses on the stack (mod ones as at image base 10000000):%s", chain.c_str());
    }
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void set_flip_every(int n) { g_flip_every = n < 1 ? 1 : n; }

void init() {
    BWLOG("bwvr init, game dir %ls", paths::game_dir().c_str());
    if (!paths::ini_int(L"mod", L"enabled", 1)) {
        BWLOG("[mod] enabled=0: pass-through only");
        return;
    }
    if (!exe::matches_v120()) {
        BWLOG("unsupported runblack.exe: pass-through only, no hooks installed");
        return;
    }
    // BWVR_PASSTHROUGH=dx (in the environment): the game exactly as it is, but for the DirectX version
    // check, without which it won't start on Windows 10 and 11 without administrator rights. For playing
    // or testing the unmodified game (a save made with the mod, loaded without it: 2026-11-14).
    wchar_t pass[8] = {};
    if (GetEnvironmentVariableW(L"BWVR_PASSTHROUGH", pass, 8) && wcscmp(pass, L"dx") == 0) {
        const bool ok = MH_Initialize() == MH_OK &&
                        hook(sym::DSETUP_DirectXSetupGetVersion, reinterpret_cast<void*>(&get_dx_version_detour),
                             reinterpret_cast<void**>(&o_get_dx_version), "DirectXSetupGetVersion thunk") &&
                        MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
        BWLOG("BWVR_PASSTHROUGH=dx: the game unmodified but for the DirectX version check (%s)", ok ? "hooked" : "FAILED");
        return;
    }
    g_prev_filter = SetUnhandledExceptionFilter(&crash_filter);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last_live);
    QueryPerformanceCounter(&g_last_stats);
    g_live_ms = paths::ini_int(L"debug", L"live_screenshot_ms", 0);
    trace::configure();
    stereo::configure();

    if (MH_Initialize() != MH_OK) {
        BWLOG("FATAL: MinHook init failed");
        return;
    }
    mipmaps::configure();
    audio::install();
    audio_stop::install();
    load_trace::install();
    bool ok = hook(sym::DSETUP_DirectXSetupGetVersion, reinterpret_cast<void*>(&get_dx_version_detour),
                   reinterpret_cast<void**>(&o_get_dx_version), "DirectXSetupGetVersion thunk");
    ok &= input::install_hooks();
    ok &= stereo::install_hooks();
    video_screen::configure();
    ok &= video_screen::install_hooks();
    ok &= hook(sym::GGame_Draw, reinterpret_cast<void*>(&game_draw_detour), reinterpret_cast<void**>(&o_game_draw),
               "GGame::Draw");
    ok &= hook(sym::GGame_ProcessGraphicsEngine, reinterpret_cast<void*>(&graphics_detour),
               reinterpret_cast<void**>(&o_graphics), "GGame::ProcessGraphicsEngine");
    ok &= hook(sym::GGame_ProcessOneGameTurn, reinterpret_cast<void*>(&game_turn_detour),
               reinterpret_cast<void**>(&o_game_turn), "GGame::ProcessOneGameTurn");
    ok &= hook(sym::LHScreen_Flip, reinterpret_cast<void*>(&flip_detour), reinterpret_cast<void**>(&o_flip),
               "LHScreen::Flip");
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        BWLOG("FATAL: enabling hooks failed; removing all hooks");
        MH_Uninitialize();
    }
}

}  // namespace bw::mod
