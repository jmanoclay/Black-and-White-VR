// (The project builds for Direct3D 7, DIRECT3D_VERSION 0x0700; this file is the one place with
// Direct3D 9's declarations, which clash with Direct3D 7's.)
#undef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0900
#include "render/d3d9_bridge.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>

#include "core/log.h"

namespace bw::render::d3d9 {

namespace {

using GetD3D9Fn = HRESULT(WINAPI*)(void* surface, IDirect3DDevice9** device9, IDirect3DSurface9** surface9);

GetD3D9Fn get_fn() {
    static const GetD3D9Fn f = [] {
        HMODULE m = GetModuleHandleW(L"dxwrapper.dll");
        auto fn = m ? reinterpret_cast<GetD3D9Fn>(GetProcAddress(m, "DxwGetD3D9Surface")) : nullptr;
        BWLOG("d3d9 bridge: %s", fn ? "dxwrapper found: pictures move on the GPU" : "no dxwrapper (the system's Direct3D 7)");
        return fn;
    }();
    return f;
}

}  // namespace

bool available() { return get_fn() != nullptr; }

bool surface(void* dd_surface, void** device9, void** surface9) {
    GetD3D9Fn f = get_fn();
    if (!f || !dd_surface) return false;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* s = nullptr;
    if (FAILED(f(dd_surface, &dev, &s)) || !dev || !s) return false;
    *device9 = dev, *surface9 = s;
    return true;
}

bool stretch(void* src_dd, int sx, int sy, int sw, int sh, void* dst_dd, int dx, int dy, int dw, int dh) {
    void *dev_a = nullptr, *src = nullptr, *dev_b = nullptr, *dst = nullptr;
    if (!surface(src_dd, &dev_a, &src) || !surface(dst_dd, &dev_b, &dst) || dev_a != dev_b) return false;
    const RECT s{sx, sy, sx + sw, sy + sh}, d{dx, dy, dx + dw, dy + dh};
    const HRESULT hr = static_cast<IDirect3DDevice9*>(dev_a)->StretchRect(static_cast<IDirect3DSurface9*>(src), &s,
                                                                           static_cast<IDirect3DSurface9*>(dst), &d, D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 3) BWLOG("d3d9 bridge: StretchRect failed %08lX", hr);
        return false;
    }
    return true;
}

// The frame kept for the window: the game's flip clears its back buffer, and dxwrapper puts its
// own (stale) picture in the window during the flip, so the frame is shown after it.
IDirect3DSurface9* g_held = nullptr;
IDirect3DDevice9* g_held_dev = nullptr;
UINT g_held_w = 0, g_held_h = 0;
bool g_have_held = false;

bool hold_unguarded(void* dd_surface) {
    g_have_held = false;
    void *dev = nullptr, *src = nullptr;
    if (!surface(dd_surface, &dev, &src)) return false;
    auto* d = static_cast<IDirect3DDevice9*>(dev);
    D3DSURFACE_DESC sd{};
    static_cast<IDirect3DSurface9*>(src)->GetDesc(&sd);
    if (!g_held || g_held_dev != d || g_held_w != sd.Width || g_held_h != sd.Height) {
        // (Another device: the old one, and its surface with it, is gone; released, it crashed the
        // game as it quit.)
        if (g_held && g_held_dev == d) g_held->Release();
        g_held = nullptr;
        if (FAILED(d->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_held, nullptr))) {
            static int logged = 0;
            if (logged++ < 3) BWLOG("d3d9 bridge: no surface to keep the window's frame in");
            return false;
        }
        g_held_dev = d, g_held_w = sd.Width, g_held_h = sd.Height;
    }
    g_have_held = SUCCEEDED(d->StretchRect(static_cast<IDirect3DSurface9*>(src), nullptr, g_held, nullptr, D3DTEXF_NONE));
    return g_have_held;
}

bool show_unguarded() {
    if (!g_have_held || !g_held_dev) return false;
    g_have_held = false;
    IDirect3DDevice9* d = g_held_dev;
    static HWND window = nullptr;  // (the device's window: the game's)
    if (!window) {
        IDirect3DSwapChain9* sc = nullptr;
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) sc->GetPresentParameters(&pp), sc->Release();
        window = pp.hDeviceWindow;
        if (!window) return false;
    }
    IDirect3DSurface9* back = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
    // (dxwrapper's back buffer covers the whole desktop and is shown scaled into the window: the
    // frame is put in its top-left corner at the window's size, and only that part is presented.)
    RECT client{};
    GetClientRect(window, &client);
    D3DSURFACE_DESC bd{};
    back->GetDesc(&bd);
    const RECT dst{0, 0, std::min<LONG>(client.right, LONG(bd.Width)), std::min<LONG>(client.bottom, LONG(bd.Height))};
    HRESULT hr = dst.right > 0 && dst.bottom > 0 ? d->StretchRect(g_held, nullptr, back, &dst, D3DTEXF_LINEAR) : E_FAIL;
    back->Release();
    if (SUCCEEDED(hr)) hr = d->Present(&dst, nullptr, window, nullptr);
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 3) BWLOG("d3d9 bridge: showing the frame in the window failed %08lX", hr);
        return false;
    }
    return true;
}

// (Guarded: as the game quits, dxwrapper tears its device down while the game still flips, and
// the surfaces here may be gone. Player, 2026-10-30: the game crashed on quitting, in here.)
bool hold_for_window(void* dd_surface) {
    __try {
        return hold_unguarded(dd_surface);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_have_held = false, g_held = nullptr, g_held_dev = nullptr;
        return false;
    }
}
bool show_held() {
    __try {
        return show_unguarded();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_have_held = false, g_held = nullptr, g_held_dev = nullptr;
        return false;
    }
}

}  // namespace bw::render::d3d9
