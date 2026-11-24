#define CINTERFACE
#include "render/screenshot.h"

#include <windows.h>

#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <vector>

#include "core/log.h"

namespace bw::screenshot {

namespace {

struct Channel {
    DWORD mask = 0;
    int shift = 0;
    DWORD max = 1;
    explicit Channel(DWORD m) : mask(m) {
        if (!m) return;
        while (!((m >> shift) & 1)) ++shift;
        max = m >> shift;
    }
    BYTE get(DWORD px) const { return mask ? BYTE(((px & mask) >> shift) * 255 / max) : 0; }
};

bool write_bmp(const std::wstring& path, int w, int h, const std::vector<BYTE>& bgr_bottom_up) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgr_bottom_up.data(), 1, bgr_bottom_up.size(), f);
    fclose(f);
    return true;
}

bool save_surface_impl(IDirectDrawSurface7* surf, const std::wstring& path) {
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(surf->lpVtbl->Lock(surf, nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR,
                                  nullptr)))
        return false;
    const int w = int(d.dwWidth), h = int(d.dwHeight);
    const int bpp = int(d.ddpfPixelFormat.dwRGBBitCount);
    const Channel r(d.ddpfPixelFormat.dwRBitMask), g(d.ddpfPixelFormat.dwGBitMask), b(d.ddpfPixelFormat.dwBBitMask);
    const int stride = (w * 3 + 3) & ~3;
    std::vector<BYTE> out(size_t(stride) * h);
    bool ok = bpp == 16 || bpp == 32;
    for (int y = 0; ok && y < h; ++y) {
        const BYTE* row = static_cast<const BYTE*>(d.lpSurface) + size_t(y) * d.lPitch;
        BYTE* dst = &out[size_t(h - 1 - y) * stride];
        for (int x = 0; x < w; ++x) {
            DWORD px = bpp == 32 ? reinterpret_cast<const DWORD*>(row)[x] : reinterpret_cast<const WORD*>(row)[x];
            dst[x * 3 + 0] = b.get(px);
            dst[x * 3 + 1] = g.get(px);
            dst[x * 3 + 2] = r.get(px);
        }
    }
    surf->lpVtbl->Unlock(surf, nullptr);
    return ok && write_bmp(path, w, h, out);
}

}  // namespace

bool save_surface(void* surface, const std::wstring& path) {
    return save_surface_impl(static_cast<IDirectDrawSurface7*>(surface), path);
}

bool read_surface_rgba(void* surface, std::vector<uint8_t>& rgba, int& w, int& h, int step) {
    auto* surf = static_cast<IDirectDrawSurface7*>(surface);
    if (!surf || step < 1) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(surf->lpVtbl->Lock(surf, nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR, nullptr))) return false;
    const int bpp = int(d.ddpfPixelFormat.dwRGBBitCount);
    const Channel r(d.ddpfPixelFormat.dwRBitMask), g(d.ddpfPixelFormat.dwGBitMask), b(d.ddpfPixelFormat.dwBBitMask);
    const bool ok = bpp == 16 || bpp == 32;
    w = int(d.dwWidth) / step, h = int(d.dwHeight) / step;
    if (ok) {
        rgba.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const BYTE* row = static_cast<const BYTE*>(d.lpSurface) + size_t(y) * step * d.lPitch;
            uint8_t* dst = &rgba[size_t(y) * w * 4];
            for (int x = 0; x < w; ++x) {
                const DWORD px = bpp == 32 ? reinterpret_cast<const DWORD*>(row)[x * step] : reinterpret_cast<const WORD*>(row)[x * step];
                dst[x * 4] = BYTE(r.get(px)), dst[x * 4 + 1] = BYTE(g.get(px)), dst[x * 4 + 2] = BYTE(b.get(px)), dst[x * 4 + 3] = 255;
            }
        }
    }
    surf->lpVtbl->Unlock(surf, nullptr);
    return ok;
}

void log_depth_stats(void* device) {
    auto* dev = static_cast<IDirect3DDevice7*>(device);
    DWORD zenable = 99, zwrite = 99, zfunc = 99;
    dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZENABLE, &zenable);
    dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZWRITEENABLE, &zwrite);
    dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZFUNC, &zfunc);
    BWLOG("zstats: ZENABLE=%lu ZWRITEENABLE=%lu ZFUNC=%lu", zenable, zwrite, zfunc);
    IDirectDrawSurface7* rt = nullptr;
    if (FAILED(dev->lpVtbl->GetRenderTarget(dev, &rt)) || !rt) return;
    DDSCAPS2 caps{};
    caps.dwCaps = DDSCAPS_ZBUFFER;
    IDirectDrawSurface7* z = nullptr;
    rt->lpVtbl->GetAttachedSurface(rt, &caps, &z);
    rt->lpVtbl->Release(rt);
    if (!z) {
        BWLOG("zstats: no z-buffer attached");
        return;
    }
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    HRESULT hr = z->lpVtbl->Lock(z, nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr)) {
        BWLOG("zstats: Lock failed %08lX", hr);
    } else {
        const DWORD mask = d.ddpfPixelFormat.dwZBitMask;
        const int bytes = int(d.ddpfPixelFormat.dwZBufferBitDepth) / 8;
        DWORD lo = 0xFFFFFFFF, hi = 0;
        unsigned long long n_max = 0, n = 0;
        for (DWORD y = 0; y < d.dwHeight; y += 4) {
            const BYTE* row = static_cast<const BYTE*>(d.lpSurface) + size_t(y) * d.lPitch;
            for (DWORD x = 0; x < d.dwWidth; x += 4) {
                DWORD v = (bytes == 4 ? reinterpret_cast<const DWORD*>(row)[x] : reinterpret_cast<const WORD*>(row)[x]) & mask;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                n_max += v == mask;
                ++n;
            }
        }
        z->lpVtbl->Unlock(z, nullptr);
        BWLOG("zstats: %lux%lu, %lu bits, mask %08lX, min %08lX max %08lX, %.1f%% at max", d.dwWidth, d.dwHeight,
              d.ddpfPixelFormat.dwZBufferBitDepth, mask, lo, hi, 100.0 * double(n_max) / double(n ? n : 1));
    }
    z->lpVtbl->Release(z);
}

bool save_render_target(void* device, const std::wstring& path) {
    auto* dev = static_cast<IDirect3DDevice7*>(device);
    IDirectDrawSurface7* rt = nullptr;
    if (FAILED(dev->lpVtbl->GetRenderTarget(dev, &rt)) || !rt) return false;
    bool ok = save_surface_impl(rt, path);
    rt->lpVtbl->Release(rt);
    return ok;
}

}  // namespace bw::screenshot
