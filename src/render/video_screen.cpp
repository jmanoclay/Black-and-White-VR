#include "render/video_screen.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "control/input_inject.h"
#include "core/paths.h"
#include "render/screenshot.h"
#include "render/stereo.h"
#include "vr/openvr_backend.h"

namespace bw::video_screen {

namespace {

// binkw32.dll (RAD's Bink, as the game ships it). The BINK handle starts with the video's width
// and height. BinkCopyToBuffer(bink, dest, pitch, height, x, y, flags): flags' low bits are the
// pixel format (3: 32-bit B, G, R, x), 0x80000000 copies the whole frame.
using CopyFn = int(__stdcall*)(void* bink, void* dest, int pitch, unsigned height, unsigned x, unsigned y, unsigned flags);
using CloseFn = void(__stdcall*)(void* bink);
CopyFn o_copy = nullptr;
CloseFn o_close = nullptr;

struct Settings {
    bool on = true;
    float width_m = 3.0f, distance_m = 3.0f;
} g_set;
bool g_capture = false;  // BWVR_VIDEO_CAPTURE=1: frames 0 and 30 of each video to bwvr\captures/video_<n>_<frame>.bmp (tests)
std::vector<uint8_t> g_rgba;
void* g_playing = nullptr;
int g_frames = 0, g_videos = 0, g_calls = 0;
bool g_vr_failed = false;
double now_s() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}
std::vector<uint8_t> g_flat;
bool g_flat_on = false;
int g_flat_frames = 0;
double g_last_video_s = -10;


void save_bmp(const std::wstring& path, const uint8_t* rgba, int w, int h) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const int row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
    BITMAPFILEHEADER fh{0x4D42, DWORD(54 + size), 0, 0, 54};
    BITMAPINFOHEADER ih{sizeof(ih), w, h, 1, 24, BI_RGB, DWORD(size), 0, 0, 0, 0};
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(size_t(row + pad));
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* s = rgba + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) line[x * 3] = s[x * 4 + 2], line[x * 3 + 1] = s[x * 4 + 1], line[x * 3 + 2] = s[x * 4];
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
}

int __stdcall copy_detour(void* bink, void* dest, int pitch, unsigned height, unsigned x, unsigned y, unsigned flags) {
    const int r = o_copy(bink, dest, pitch, height, x, y, flags);
    ++g_calls;
    if (!g_set.on || !bink || IsBadReadPtr(bink, 8)) return r;
    const int w = *static_cast<const int*>(bink), h = static_cast<const int*>(bink)[1];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return r;
    const bool vr = stereo::wants_headset();
    if (!vr && !g_capture) return r;
    if (bink != g_playing) {
        g_playing = bink, g_frames = 0, ++g_videos;
        BWLOG("video: a %dx%d video plays (the game copies it %dx%d at %u, %u, format %u)%s", w, h, pitch, height, x, y, flags & 0xFF,
              vr ? ": on the screen in the headset" : "");
    }
    g_rgba.resize(size_t(w) * h * 4);
    o_copy(bink, g_rgba.data(), w * 4, unsigned(h), 0, 0, 3u | 0x80000000u);
    for (size_t i = 0; i < g_rgba.size(); i += 4) {  // (B, G, R, x -> R, G, B, opaque)
        const uint8_t b = g_rgba[i];
        g_rgba[i] = g_rgba[i + 2], g_rgba[i + 2] = b, g_rgba[i + 3] = 255;
    }
    if (g_capture && (g_frames == 0 || g_frames == 30)) {
        const std::wstring path = paths::bwvr_dir() + L"captures/video_" + std::to_wstring(g_videos) + L"_" + std::to_wstring(g_frames) + L".bmp";
        save_bmp(path, g_rgba.data(), w, h);
        BWLOG("video: frame %d of video %d saved", g_frames, g_videos);
    }
    ++g_frames;
    g_last_video_s = now_s();
    if (vr && !g_vr_failed) {
        // (Before the first 3D frame SteamVR isn't started yet: start it here.)
        if (!vr::openvr::active() && !vr::openvr::init()) g_vr_failed = true, BWLOG("video: SteamVR could not be started");
        else vr::openvr::show_video(g_rgba.data(), w, h, g_set.width_m, g_set.distance_m);
    }
    return r;
}

void __stdcall close_detour(void* bink) {
    BWLOG("video: closed (%d copies by the game so far)", g_calls);
    if (bink == g_playing) {
        BWLOG("video: ended after %d frames", g_frames);
        g_playing = nullptr;
        vr::openvr::hide_video();
    }
    o_close(bink);
}

}  // namespace

void presented(void* back_buffer) {
    const bool vr = stereo::wants_headset() && !g_vr_failed;
    if (!g_set.on || !(vr || g_capture)) return;
    const double now = now_s();
    // (A VR frame or a video frame lately: nothing to do here; the screen goes once VR is back.)
    const bool flat = now - stereo::last_vr_frame_s() > 0.3 && now - g_last_video_s > 0.3;
    if (!flat) {
        if (g_flat_on && now - stereo::last_vr_frame_s() < 0.3) {
            g_flat_on = false;
            input::vr_button(false, false);
            input::set_vr_cursor(false, 0, 0);
            BWLOG("video: VR frames again after %d flat ones: the screen goes", g_flat_frames);
            vr::openvr::hide_video();
        }
        return;
    }
    // (At most 45 a second, a readback of the window's picture, half size each way: the splash
    // and the menus before the first 3D frame run at hundreds of frames a second.)
    static double read_at = -10;
    ++g_flat_frames;
    if (now - read_at < 1.0 / 45.0) return;
    read_at = now;
    int w = 0, h = 0;
    if (!back_buffer || !screenshot::read_surface_rgba(back_buffer, g_flat, w, h, 2)) return;
    if (g_capture && (g_flat_frames == 1 || g_flat_frames % 600 < 2)) {  // (tests: what would go on the screen)
        save_bmp(paths::bwvr_dir() + L"captures/flat_" + std::to_wstring(g_flat_frames) + L".bmp", g_flat.data(), w, h);
        BWLOG("video: flat frame %d saved (%dx%d)", g_flat_frames, w, h);
    }
    if (!vr) return;
    if (!vr::openvr::active() && !vr::openvr::init()) {
        g_vr_failed = true;
        BWLOG("video: SteamVR could not be started");
        return;
    }
    if (!g_flat_on) {
        g_flat_on = true, g_flat_frames = 1;
        BWLOG("video: frames with no VR frame (%dx%d): on the screen in the headset", w * 2, h * 2);
    }
    vr::openvr::show_video(g_flat.data(), w, h, g_set.width_m, g_set.distance_m);
    // Either trigger clicks the middle of the screen, as a click in the window does (it skips the
    // opening splash; player, 2026-11-02).
    vr::Controller c[2];
    vr::openvr::read_controllers(c);
    static bool down = false;
    const bool pulled = (c[0].active && c[0].trigger > 0.75f) || (c[1].active && c[1].trigger > 0.75f);
    const bool released = !(c[0].active && c[0].trigger > 0.55f) && !(c[1].active && c[1].trigger > 0.55f);
    if (pulled && !down) {
        down = true;
        input::set_vr_cursor(true, w, h);  // (the middle of the full-size window)
        input::vr_button(false, true);
        BWLOG("video: the trigger clicks the screen");
    } else if (down && released) {
        down = false;
        input::vr_button(false, false);
    }
}

void configure() {
    g_set.on = paths::ini_int(L"vr", L"video_screen", 1) != 0;
    g_set.width_m = float(paths::ini_int(L"vr", L"video_screen_width_cm", 300)) / 100.0f;
    g_set.distance_m = float(paths::ini_int(L"vr", L"video_screen_distance_cm", 300)) / 100.0f;
    wchar_t v[8] = {};
    g_capture = GetEnvironmentVariableW(L"BWVR_VIDEO_CAPTURE", v, 8) > 0 && v[0] == L'1';
    if (GetEnvironmentVariableW(L"BWVR_VIDEO_SCREEN", v, 8) > 0) g_set.on = v[0] == L'1';  // (tests)
}

bool install_hooks() {
    HMODULE bink = GetModuleHandleW(L"binkw32.dll");
    if (!bink) bink = LoadLibraryW(L"binkw32.dll");
    void* copy = bink ? reinterpret_cast<void*>(GetProcAddress(bink, "_BinkCopyToBuffer@28")) : nullptr;
    void* close = bink ? reinterpret_cast<void*>(GetProcAddress(bink, "_BinkClose@4")) : nullptr;
    if (!copy || !close) {
        BWLOG("video: binkw32's BinkCopyToBuffer/BinkClose not found: the videos stay on the desktop");
        return true;  // (not fatal)
    }
    if (MH_CreateHook(copy, reinterpret_cast<void*>(&copy_detour), reinterpret_cast<void**>(&o_copy)) != MH_OK ||
        MH_CreateHook(close, reinterpret_cast<void*>(&close_detour), reinterpret_cast<void**>(&o_close)) != MH_OK) {
        BWLOG("video: the Bink hooks could not be made: the videos stay on the desktop");
        return true;
    }
    BWLOG("video: Bink hooked (screen %s, %.1f m wide, %.1f m ahead)", g_set.on ? "on" : "off", g_set.width_m, g_set.distance_m);
    return true;
}

}  // namespace bw::video_screen
