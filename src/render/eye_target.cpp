#define CINTERFACE
#include "render/eye_target.h"

#include <windows.h>

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdio>
#include <thread>

#include "core/log.h"
#include "render/d3d9_bridge.h"
#include "render/state_saver.h"

namespace bw::render {

namespace {

IDirectDrawSurface7* surf(void* p) { return static_cast<IDirectDrawSurface7*>(p); }
IDirect3DDevice7* dev(void* p) { return static_cast<IDirect3DDevice7*>(p); }

int mask_shift(DWORD m) {
    int s = 0;
    while (m && !((m >> s) & 1)) ++s;
    return s;
}

// Copies rows with a few helper threads: a picture read back for the headset is 12 MB an eye,
// written into memory the CPU can only write to slowly (an upload texture), about 0.7 ms on one
// thread. The helpers wait on an event between copies.
class RowCopier {
public:
    static constexpr int kHelpers = 3;
    RowCopier() {
        for (int i = 0; i < kHelpers; ++i) {
            go_[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            std::thread([this, i] { run(i); }).detach();
        }
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    void copy(uint8_t* dst, int dst_pitch, const uint8_t* src, int src_pitch, int row_bytes, int rows) {
        dst_ = dst, src_ = src, dst_pitch_ = dst_pitch, src_pitch_ = src_pitch, row_bytes_ = row_bytes, rows_ = rows;
        left_ = kHelpers;
        ResetEvent(done_);
        for (int i = 0; i < kHelpers; ++i) SetEvent(go_[i]);
        part(kHelpers);  // (this thread takes a share too)
        WaitForSingleObject(done_, INFINITE);
    }

private:
    void part(int k) {
        const int n = kHelpers + 1, y0 = rows_ * k / n, y1 = rows_ * (k + 1) / n;
        for (int y = y0; y < y1; ++y) memcpy(dst_ + size_t(y) * dst_pitch_, src_ + size_t(y) * src_pitch_, size_t(row_bytes_));
    }
    void run(int i) {
        for (;;) {
            WaitForSingleObject(go_[i], INFINITE);
            part(i);
            if (--left_ == 0) SetEvent(done_);
        }
    }
    HANDLE go_[kHelpers] = {}, done_ = nullptr;
    std::atomic<int> left_{0};
    uint8_t* dst_ = nullptr;
    const uint8_t* src_ = nullptr;
    int dst_pitch_ = 0, src_pitch_ = 0, row_bytes_ = 0, rows_ = 0;
};

RowCopier& copier() {
    static RowCopier* c = new RowCopier();  // (never torn down: its threads end with the process)
    return *c;
}

}  // namespace

EyeTarget::~EyeTarget() { release(); }

void EyeTarget::release() {
    if (z_) surf(z_)->lpVtbl->Release(surf(z_));
    if (rt_) surf(rt_)->lpVtbl->Release(surf(rt_));
    z_ = rt_ = nullptr;
    w_ = h_ = 0;
}

bool EyeTarget::ensure(void* device, int w, int h) {
    if (rt_ && w == w_ && h == h_) {
        if (surf(rt_)->lpVtbl->IsLost(surf(rt_)) == DD_OK) return true;
        surf(rt_)->lpVtbl->Restore(surf(rt_));
        if (z_) surf(z_)->lpVtbl->Restore(surf(z_));
        return true;
    }
    release();

    IDirectDrawSurface7* back = nullptr;
    if (FAILED(dev(device)->lpVtbl->GetRenderTarget(dev(device), &back)) || !back) return false;
    DDSURFACEDESC2 bd{};
    bd.dwSize = sizeof(bd);
    back->lpVtbl->GetSurfaceDesc(back, &bd);

    IUnknown* unk = nullptr;
    IDirectDraw7* dd = nullptr;
    back->lpVtbl->GetDDInterface(back, reinterpret_cast<void**>(&unk));
    if (unk) {
        unk->lpVtbl->QueryInterface(unk, IID_IDirectDraw7, reinterpret_cast<void**>(&dd));
        unk->lpVtbl->Release(unk);
    }
    DDSCAPS2 zcaps{};
    zcaps.dwCaps = DDSCAPS_ZBUFFER;
    IDirectDrawSurface7* back_z = nullptr;
    back->lpVtbl->GetAttachedSurface(back, &zcaps, &back_z);
    back->lpVtbl->Release(back);
    if (!dd) {
        BWLOG("eye target: no IDirectDraw7");
        if (back_z) back_z->lpVtbl->Release(back_z);
        return false;
    }

    DDSURFACEDESC2 sd{};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = DWORD(w);
    sd.dwHeight = DWORD(h);
    sd.ddpfPixelFormat = bd.ddpfPixelFormat;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    IDirectDrawSurface7* rt = nullptr;
    HRESULT hr = dd->lpVtbl->CreateSurface(dd, &sd, &rt, nullptr);
    if (FAILED(hr)) {
        BWLOG("eye target: CreateSurface(colour %dx%d) failed %08lX", w, h, hr);
        dd->lpVtbl->Release(dd);
        if (back_z) back_z->lpVtbl->Release(back_z);
        return false;
    }

    IDirectDrawSurface7* z = nullptr;
    if (back_z) {
        DDSURFACEDESC2 zd{};
        zd.dwSize = sizeof(zd);
        back_z->lpVtbl->GetSurfaceDesc(back_z, &zd);
        back_z->lpVtbl->Release(back_z);
        DDSURFACEDESC2 zs{};
        zs.dwSize = sizeof(zs);
        zs.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        zs.dwWidth = DWORD(w);
        zs.dwHeight = DWORD(h);
        zs.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        // The game's own z-buffer is 16-bit, which with depth z = 1 - near/Z forces a far near
        // plane. Prefer a 24-bit one for the eye targets (near plane a few cm from the eye),
        // and fall back to the game's format.
        struct ZFormat {
            DWORD bits, mask;
        };
        const ZFormat tries[] = {{32, 0x00FFFFFF}, {32, 0xFFFFFF00}, {24, 0x00FFFFFF}, {32, 0xFFFFFFFF}};
        hr = E_FAIL;
        for (const ZFormat& f : tries) {
            zs.ddpfPixelFormat = {};
            zs.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            zs.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            zs.ddpfPixelFormat.dwZBufferBitDepth = f.bits;
            zs.ddpfPixelFormat.dwZBitMask = f.mask;
            hr = dd->lpVtbl->CreateSurface(dd, &zs, &z, nullptr);
            if (SUCCEEDED(hr)) hr = rt->lpVtbl->AddAttachedSurface(rt, z);
            if (SUCCEEDED(hr)) {
                zd.ddpfPixelFormat = zs.ddpfPixelFormat;
                break;
            }
            if (z) z->lpVtbl->Release(z);
            z = nullptr;
        }
        if (FAILED(hr)) {
            zs.ddpfPixelFormat = zd.ddpfPixelFormat;
            hr = dd->lpVtbl->CreateSurface(dd, &zs, &z, nullptr);
            if (SUCCEEDED(hr)) hr = rt->lpVtbl->AddAttachedSurface(rt, z);
        }
        if (FAILED(hr)) {
            BWLOG("eye target: z-buffer creation/attach failed %08lX", hr);
            if (z) z->lpVtbl->Release(z);
            z = nullptr;
        } else {
            z_bits_ = zd.ddpfPixelFormat.dwZBufferBitDepth;
            z_mask_ = zd.ddpfPixelFormat.dwZBitMask;
        }
    }
    dd->lpVtbl->Release(dd);
    rt_ = rt;
    z_ = z;
    w_ = w;
    h_ = h;
    BWLOG("eye target: %dx%d created (z %s, %lu bits, mask %08lX)", w, h, z ? "yes" : "NO", z_bits_, z_mask_);
    return true;
}

FrameTexture::~FrameTexture() { release(); }

void FrameTexture::release() {
    if (tex_) surf(tex_)->lpVtbl->Release(surf(tex_));
    tex_ = nullptr;
    w_ = h_ = 0;
}

bool FrameTexture::update(void* device) {
    IDirectDrawSurface7* back = nullptr;
    if (FAILED(dev(device)->lpVtbl->GetRenderTarget(dev(device), &back)) || !back) return false;
    DDSURFACEDESC2 bd{};
    bd.dwSize = sizeof(bd);
    back->lpVtbl->GetSurfaceDesc(back, &bd);
    if (tex_ && surf(tex_)->lpVtbl->IsLost(surf(tex_)) != DD_OK) surf(tex_)->lpVtbl->Restore(surf(tex_));
    if (!tex_ || w_ != int(bd.dwWidth) || h_ != int(bd.dwHeight)) {
        release();
        IUnknown* unk = nullptr;
        IDirectDraw7* dd = nullptr;
        back->lpVtbl->GetDDInterface(back, reinterpret_cast<void**>(&unk));
        if (unk) {
            unk->lpVtbl->QueryInterface(unk, IID_IDirectDraw7, reinterpret_cast<void**>(&dd));
            unk->lpVtbl->Release(unk);
        }
        if (dd) {
            // The frame's own size first; a power of two if the driver insists (the copy stretches).
            const DWORD sizes[2][2] = {{bd.dwWidth, bd.dwHeight}, {2048, 1024}};
            for (const auto& size : sizes) {
                DDSURFACEDESC2 sd{};
                sd.dwSize = sizeof(sd);
                sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                sd.dwWidth = size[0];
                sd.dwHeight = size[1];
                sd.ddpfPixelFormat = bd.ddpfPixelFormat;
                sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
                IDirectDrawSurface7* t = nullptr;
                const HRESULT hr = dd->lpVtbl->CreateSurface(dd, &sd, &t, nullptr);
                if (SUCCEEDED(hr) && t) {
                    tex_ = t;
                    BWLOG("frame texture: %lux%lu created for a %lux%lu frame", size[0], size[1], bd.dwWidth, bd.dwHeight);
                    break;
                }
                BWLOG("frame texture: CreateSurface(%lux%lu) failed %08lX", size[0], size[1], hr);
            }
            dd->lpVtbl->Release(dd);
        }
        w_ = int(bd.dwWidth), h_ = int(bd.dwHeight);  // (also when it failed: don't retry every frame)
    }
    bool ok = false;
    if (tex_) {
        const HRESULT hr = surf(tex_)->lpVtbl->Blt(surf(tex_), nullptr, back, nullptr, DDBLT_WAIT, nullptr);
        ok = SUCCEEDED(hr);
        static bool logged = false;
        if (!ok && !logged) BWLOG("frame texture: copy failed %08lX", hr), logged = true;
    }
    back->lpVtbl->Release(back);
    return ok;
}

const char* set_texture_filtering(void* device, int mode, int aniso, float lod_bias) {
    if (!device || mode <= 0) return "unchanged";
    IDirect3DDevice7* d = dev(device);
    D3DDEVICEDESC7 caps{};
    d->lpVtbl->GetCaps(d, &caps);
    const DWORD filter = caps.dpcTriCaps.dwTextureFilterCaps;
    const bool can_aniso = mode >= 2 && (filter & D3DPTFILTERCAPS_MINFANISOTROPIC) && caps.dwMaxAnisotropy > 1;
    for (DWORD stage = 0; stage < 2; ++stage) {
        d->lpVtbl->SetTextureStageState(d, stage, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
        d->lpVtbl->SetTextureStageState(d, stage, D3DTSS_MINFILTER, can_aniso ? D3DTFN_ANISOTROPIC : D3DTFN_LINEAR);
        d->lpVtbl->SetTextureStageState(d, stage, D3DTSS_MIPFILTER, D3DTFP_LINEAR);
        DWORD bias;
        memcpy(&bias, &lod_bias, sizeof(bias));
        d->lpVtbl->SetTextureStageState(d, stage, D3DTSS_MIPMAPLODBIAS, bias);
        if (can_aniso)
            d->lpVtbl->SetTextureStageState(d, stage, D3DTSS_MAXANISOTROPY, DWORD(std::min<int>(aniso, int(caps.dwMaxAnisotropy))));
    }
    return can_aniso ? "linear, mipmaps, anisotropic" : "linear, mipmaps";
}

bool surface_size(void* surface, int* w, int* h) {
    if (!surface) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(surf(surface)->lpVtbl->GetSurfaceDesc(surf(surface), &d))) return false;
    *w = int(d.dwWidth), *h = int(d.dwHeight);
    return true;
}

TextTexture::~TextTexture() { release(); }

void TextTexture::release() {
    if (tex_) surf(tex_)->lpVtbl->Release(surf(tex_));
    tex_ = nullptr;
    tex_w_ = tex_h_ = used_w_ = used_h_ = 0;
}

bool TextTexture::set(void* device, const std::vector<std::wstring>& lines, int title_lines, uint32_t background_rgb, uint32_t title_rgb,
                      uint32_t text_rgb, uint8_t background_alpha) {
    if (!device || lines.empty()) return false;
    // Laid out with GDI in a bitmap first: 44 px a line (the title 54), centred, with a margin.
    constexpr int kLine = 44, kTitle = 54, kMargin = 28;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT body = CreateFontW(-kLine * 3 / 4, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Georgia");
    HFONT title = CreateFontW(-kTitle * 3 / 4, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Georgia");
    int w = 0, h = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        SelectObject(dc, int(i) < title_lines ? title : body);
        SIZE sz{};
        GetTextExtentPoint32W(dc, lines[i].c_str(), int(lines[i].size()), &sz);
        w = std::max(w, int(sz.cx));
        h += int(i) < title_lines ? kTitle : kLine;
    }
    w = std::min(w + 2 * kMargin, 2048), h = std::min(h + 2 * kMargin, 1024);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // (top-down)
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        auto bgr = [](uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); };
        HBRUSH brush = CreateSolidBrush(bgr(background_rgb));
        RECT all{0, 0, w, h};
        FillRect(dc, &all, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        int y = kMargin;
        for (size_t i = 0; i < lines.size(); ++i) {
            const bool is_title = int(i) < title_lines;
            SelectObject(dc, is_title ? title : body);
            SetTextColor(dc, bgr(is_title ? title_rgb : text_rgb));
            RECT r{kMargin, y, w - kMargin, y + (is_title ? kTitle : kLine)};
            DrawTextW(dc, lines[i].c_str(), int(lines[i].size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            y += is_title ? kTitle : kLine;
        }
        GdiFlush();
        SelectObject(dc, old);
        // Into a texture with alpha (a power of two in each direction): the background
        // translucent, the letters (wherever GDI changed the background) opaque.
        int tw = 64, th = 64;
        while (tw < w) tw *= 2;
        while (th < h) th *= 2;
        if (!tex_ || tex_w_ != tw || tex_h_ != th) {
            release();
            IDirectDrawSurface7* back = nullptr;
            if (SUCCEEDED(dev(device)->lpVtbl->GetRenderTarget(dev(device), &back)) && back) {
                IUnknown* unk = nullptr;
                IDirectDraw7* dd = nullptr;
                back->lpVtbl->GetDDInterface(back, reinterpret_cast<void**>(&unk));
                if (unk) unk->lpVtbl->QueryInterface(unk, IID_IDirectDraw7, reinterpret_cast<void**>(&dd)), unk->lpVtbl->Release(unk);
                back->lpVtbl->Release(back);
                if (dd) {
                    DDSURFACEDESC2 sd{};
                    sd.dwSize = sizeof(sd);
                    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                    sd.dwWidth = DWORD(tw), sd.dwHeight = DWORD(th);
                    sd.ddpfPixelFormat.dwSize = sizeof(sd.ddpfPixelFormat);
                    sd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
                    sd.ddpfPixelFormat.dwRGBBitCount = 32;
                    sd.ddpfPixelFormat.dwRBitMask = 0x00FF0000, sd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
                    sd.ddpfPixelFormat.dwBBitMask = 0x000000FF, sd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
                    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
                    IDirectDrawSurface7* t = nullptr;
                    const HRESULT hr = dd->lpVtbl->CreateSurface(dd, &sd, &t, nullptr);
                    if (SUCCEEDED(hr) && t) tex_ = t, tex_w_ = tw, tex_h_ = th;
                    else BWLOG("text texture: CreateSurface(%dx%d) failed %08lX", tw, th, hr);
                    dd->lpVtbl->Release(dd);
                }
            }
        }
        if (tex_) {
            DDSURFACEDESC2 d{};
            d.dwSize = sizeof(d);
            if (SUCCEEDED(surf(tex_)->lpVtbl->Lock(surf(tex_), nullptr, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
                const uint32_t bg = background_rgb & 0xFFFFFF;
                for (int y2 = 0; y2 < th; ++y2) {
                    uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(d.lpSurface) + size_t(y2) * d.lPitch);
                    for (int x = 0; x < tw; ++x) {
                        if (x >= w || y2 >= h) {
                            row[x] = 0;
                            continue;
                        }
                        const uint32_t px = static_cast<const uint32_t*>(bits)[size_t(y2) * w + x] & 0xFFFFFF;
                        row[x] = px | (px == bg ? uint32_t(background_alpha) << 24 : 0xFF000000u);
                    }
                }
                surf(tex_)->lpVtbl->Unlock(surf(tex_), nullptr);
                used_w_ = w, used_h_ = h;
                ok = true;
            }
        }
        DeleteObject(bmp);
    }
    DeleteObject(body);
    DeleteObject(title);
    DeleteDC(dc);
    return ok;
}

bool draw_textured_quad(void* device, void* texture, const float v[4][3], float u1, float v1, bool blend) {
    if (!device || !texture) return false;
    IDirect3DDevice7* d = dev(device);
    struct Vtx {
        float x, y, z, rhw, u, w;
    };
    const Vtx q[4] = {{v[0][0], v[0][1], 0.5f, v[0][2], 0, 0},
                      {v[1][0], v[1][1], 0.5f, v[1][2], u1, 0},
                      {v[2][0], v[2][1], 0.5f, v[2][2], u1, v1},
                      {v[3][0], v[3][1], 0.5f, v[3][2], 0, v1}};
    const HRESULT scene = d->lpVtbl->BeginScene(d);
    HRESULT hr;
    {
        StateSaver st(d);  // (the states below are put back afterwards)
        st.texture(0, surf(texture));
        st.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        st.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        st.tss(0, D3DTSS_ALPHAOP, blend ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
        st.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        st.tss(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
        st.tss(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
        st.tss(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
        st.tss(0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP);
        st.tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        st.rs(D3DRENDERSTATE_ZENABLE, FALSE);
        st.rs(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        st.rs(D3DRENDERSTATE_ALPHABLENDENABLE, blend ? TRUE : FALSE);
        if (blend) st.rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA), st.rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        st.rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        st.rs(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        st.rs(D3DRENDERSTATE_FOGENABLE, FALSE);
        st.rs(D3DRENDERSTATE_SPECULARENABLE, FALSE);
        hr = d->lpVtbl->DrawPrimitive(d, D3DPT_TRIANGLEFAN, D3DFVF_XYZRHW | D3DFVF_TEX1, const_cast<Vtx*>(q), 4, 0);
    }
    if (SUCCEEDED(scene)) d->lpVtbl->EndScene(d);
    return SUCCEEDED(hr);
}

bool draw_vignette(void* device, float cx, float cy, float r_in, float r_out, float alpha) {
    if (!device || alpha <= 0.0f || r_out <= r_in) return false;
    IDirect3DDevice7* d = dev(device);
    struct Vtx {
        float x, y, z, rhw;
        DWORD c;
    };
    // Three rings: clear at r_in, `alpha` at r_out, `alpha` far out (past any corner); a strip
    // between each pair.
    constexpr int kSeg = 48;
    const DWORD a8 = DWORD(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
    const float radii[3] = {r_in, r_out, r_out * 4.0f + 4000.0f};
    const DWORD cols[3] = {0x00000000, a8, a8};
    std::vector<Vtx> strip;
    strip.reserve((kSeg + 1) * 2);
    HRESULT scene = d->lpVtbl->BeginScene(d);
    HRESULT hr = S_OK;
    {
        StateSaver st(d);
        st.texture(0, nullptr);
        st.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        st.tss(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        st.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        st.tss(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        st.tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        st.rs(D3DRENDERSTATE_ZENABLE, FALSE);
        st.rs(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        st.rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        st.rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        st.rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        st.rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        st.rs(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        st.rs(D3DRENDERSTATE_FOGENABLE, FALSE);
        st.rs(D3DRENDERSTATE_SPECULARENABLE, FALSE);
        st.rs(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
        for (int ring = 0; ring < 2 && SUCCEEDED(hr); ++ring) {
            strip.clear();
            for (int i = 0; i <= kSeg; ++i) {
                const float t = 6.2831853f * float(i) / float(kSeg), c = std::cos(t), s = std::sin(t);
                for (int k = ring; k <= ring + 1; ++k)
                    strip.push_back({cx + c * radii[k], cy + s * radii[k], 0.0f, 1.0f, cols[k]});
            }
            hr = d->lpVtbl->DrawPrimitive(d, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, strip.data(), DWORD(strip.size()), 0);
        }
    }
    if (SUCCEEDED(scene)) d->lpVtbl->EndScene(d);
    return SUCCEEDED(hr);
}

bool EyeTarget::bind(void* device, uint32_t clear_color) {
    if (!rt_) return false;
    IDirectDrawSurface7* prev = nullptr;
    dev(device)->lpVtbl->GetRenderTarget(dev(device), &prev);
    prev_rt_ = prev;  // keeps the reference until unbind()
    if (FAILED(dev(device)->lpVtbl->SetRenderTarget(dev(device), surf(rt_), 0))) {
        BWLOG("eye target: SetRenderTarget failed");
        unbind(device);
        return false;
    }
    D3DVIEWPORT7 vp{0, 0, DWORD(w_), DWORD(h_), 0.0f, 1.0f};
    dev(device)->lpVtbl->SetViewport(dev(device), &vp);
    dev(device)->lpVtbl->Clear(dev(device), 0, nullptr, D3DCLEAR_TARGET | (z_ ? D3DCLEAR_ZBUFFER : 0),
                               clear_color, 1.0f, 0);
    return true;
}

void EyeTarget::unbind(void* device) {
    if (!prev_rt_) return;
    IDirectDrawSurface7* prev = surf(prev_rt_);
    dev(device)->lpVtbl->SetRenderTarget(dev(device), prev, 0);
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    prev->lpVtbl->GetSurfaceDesc(prev, &d);
    D3DVIEWPORT7 vp{0, 0, d.dwWidth, d.dwHeight, 0.0f, 1.0f};
    dev(device)->lpVtbl->SetViewport(dev(device), &vp);
    prev->lpVtbl->Release(prev);
    prev_rt_ = nullptr;
}

bool EyeTarget::read_color(const Rect& c, std::vector<uint8_t>& out, int& stride) {
    if (!rt_) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(surf(rt_)->lpVtbl->Lock(surf(rt_), nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
        return false;
    const int bpp = int(d.ddpfPixelFormat.dwRGBBitCount);
    const DWORD rm = d.ddpfPixelFormat.dwRBitMask, gm = d.ddpfPixelFormat.dwGBitMask, bm = d.ddpfPixelFormat.dwBBitMask;
    const int rs = mask_shift(rm), gs = mask_shift(gm), bs = mask_shift(bm);
    const DWORD rmax = rm >> rs, gmax = gm >> gs, bmax = bm >> bs;
    stride = (c.w * 3 + 3) & ~3;
    out.assign(size_t(stride) * c.h, 0);
    for (int y = 0; y < c.h; ++y) {
        const BYTE* row = static_cast<const BYTE*>(d.lpSurface) + size_t(c.y + y) * d.lPitch;
        BYTE* dst = &out[size_t(c.h - 1 - y) * stride];
        for (int x = 0; x < c.w; ++x) {
            DWORD px = bpp == 32 ? reinterpret_cast<const DWORD*>(row)[c.x + x]
                                 : reinterpret_cast<const WORD*>(row)[c.x + x];
            dst[x * 3 + 0] = BYTE(((px & bm) >> bs) * 255 / bmax);
            dst[x * 3 + 1] = BYTE(((px & gm) >> gs) * 255 / gmax);
            dst[x * 3 + 2] = BYTE(((px & rm) >> rs) * 255 / rmax);
        }
    }
    surf(rt_)->lpVtbl->Unlock(surf(rt_), nullptr);
    return bpp == 16 || bpp == 32;
}

double g_lock_ms = 0, g_copy_ms = 0;
bool EyeTarget::read_bgrx(const Rect& c, std::vector<uint8_t>& out) {
    if (!rt_) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(surf(rt_)->lpVtbl->Lock(surf(rt_), nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
        return false;
    const bool ok = d.ddpfPixelFormat.dwRGBBitCount == 32 && d.ddpfPixelFormat.dwBBitMask == 0xFF;
    if (ok) {
        out.resize(size_t(c.w) * c.h * 4);
        for (int y = 0; y < c.h; ++y)
            memcpy(&out[size_t(y) * c.w * 4],
                   static_cast<const BYTE*>(d.lpSurface) + size_t(c.y + y) * d.lPitch + size_t(c.x) * 4, size_t(c.w) * 4);
    }
    surf(rt_)->lpVtbl->Unlock(surf(rt_), nullptr);
    return ok;
}

bool EyeTarget::read_into(const Rect& c, uint8_t* dst, int dst_pitch) {
    if (!rt_) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    RECT r{c.x, c.y, c.x + c.w, c.y + c.h};  // (only the crop: less to bring back from the GPU)
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (FAILED(surf(rt_)->lpVtbl->Lock(surf(rt_), &r, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return false;
    QueryPerformanceCounter(&t1);
    const bool ok = d.ddpfPixelFormat.dwRGBBitCount == 32 && d.ddpfPixelFormat.dwBBitMask == 0xFF;
    if (ok) {
        copier().copy(dst, dst_pitch, static_cast<const uint8_t*>(d.lpSurface), int(d.lPitch), c.w * 4, c.h);
    }
    surf(rt_)->lpVtbl->Unlock(surf(rt_), &r);
    QueryPerformanceCounter(&t2);
    g_lock_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    g_copy_ms += double(t2.QuadPart - t1.QuadPart) * 1000.0 / double(f.QuadPart);
    return ok;
}

bool EyeTarget::read_depth(const Rect& c, std::vector<float>& out) {
    if (!z_ || !z_mask_) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    HRESULT hr = surf(z_)->lpVtbl->Lock(surf(z_), nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) BWLOG("eye target: z-buffer Lock failed %08lX (depth capture unavailable)", hr);
        logged = true;
        return false;
    }
    const int shift = mask_shift(z_mask_);
    const double maxv = double(z_mask_ >> shift);
    const int bytes = int(d.ddpfPixelFormat.dwZBufferBitDepth) / 8;
    out.assign(size_t(c.w) * c.h, 0.f);
    for (int y = 0; y < c.h; ++y) {
        const BYTE* row = static_cast<const BYTE*>(d.lpSurface) + size_t(c.y + y) * d.lPitch;
        for (int x = 0; x < c.w; ++x) {
            DWORD v = bytes == 4 ? reinterpret_cast<const DWORD*>(row)[c.x + x]
                                 : reinterpret_cast<const WORD*>(row)[c.x + x];
            out[size_t(y) * c.w + x] = float(double((v & z_mask_) >> shift) / maxv);
        }
    }
    surf(z_)->lpVtbl->Unlock(surf(z_), nullptr);
    return true;
}

bool EyeTarget::blit_to(void* device, const Rect& c, const Rect& dst) {
    if (!rt_) return false;
    IDirectDrawSurface7* back = nullptr;
    if (FAILED(dev(device)->lpVtbl->GetRenderTarget(dev(device), &back)) || !back) return false;
    // (Through dxwrapper a stretched Blt is slow, about 1 ms: a Direct3D 9 StretchRect instead.)
    if (d3d9::stretch(rt_, c.x, c.y, c.w, c.h, back, dst.x, dst.y, dst.w, dst.h)) {
        back->lpVtbl->Release(back);
        return true;
    }
    RECT s{c.x, c.y, c.x + c.w, c.y + c.h};
    RECT t{dst.x, dst.y, dst.x + dst.w, dst.y + dst.h};
    HRESULT hr = back->lpVtbl->Blt(back, &t, surf(rt_), &s, DDBLT_WAIT, nullptr);
    back->lpVtbl->Release(back);
    return SUCCEEDED(hr);
}

bool write_bmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgr) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = DWORD(fh.bfOffBits + stride * h);
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgr.data(), 1, bgr.size(), f);
    fclose(f);
    return true;
}

}  // namespace bw::render
