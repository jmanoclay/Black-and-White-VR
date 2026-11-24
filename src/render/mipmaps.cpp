// Mipmaps for the game's textures (see mipmaps.h).
#define CINTERFACE
#include "render/mipmaps.h"

#include <windows.h>

#include <ddraw.h>

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <MinHook.h>

#include "core/log.h"
#include "core/paths.h"
#include "render/d3d7_trace.h"

namespace bw::mipmaps {

namespace {

using Surf = IDirectDrawSurface7;

bool g_enabled = false;   // [textures] mipmaps
int g_levels = 4;         // [textures] mip_levels: the top level included
int g_min_size = 16;      // [textures] min_size: smaller textures stay single-level
bool g_keep_coverage = true;  // [textures] keep_coverage: cut-out textures keep their solid share at every level

std::unordered_set<Surf*> g_ours;   // top levels created with a chain (pointers may be reused: checked again)
thread_local bool t_busy = false;   // inside our own Lock/Unlock calls
struct Pending {
    Surf* s;
    RECT r;
    bool whole;
    const BYTE* bits;               // the locked rectangle's memory, read just before Unlock
    LONG pitch;
    bool changed;
};
std::vector<Pending> g_locked;      // write locks of our textures, marked dirty on Unlock

// CPU copies of small textures' top levels. A few small textures are uploaded again in every eye
// pass; reading a texture back from the GPU is what the levels cost (~0.2 ms for 32x32). The
// copy is taken from the game's staging slot while it is locked, carried over by the Blt into the
// texture, and used instead; an upload of the same pixels again makes no levels at all.
constexpr size_t kMaxShadowBytes = 64 * 64 * 4;
struct Shadow {
    std::vector<BYTE> px;
    LONG w = 0, h = 0;
    int bpp = 0;
    bool valid = false;             // holds the whole top level
};
std::unordered_map<Surf*, Shadow> g_shadow;
// Written regions not yet made into the lower levels. They are made when the texture is next
// bound: the game uploads through a staging slot (Lock, Unlock, then Blt into the texture it
// draws with), which is never bound, and a few small textures are written once per eye pass.
std::unordered_map<Surf*, RECT> g_dirty;

// Textures written again and again (the game's dialogs redraw theirs every frame): their levels
// are read back from the GPU each time (about 0.8 ms), which in the save room's name dialog took a
// tenth of the frame time. Without a CPU copy to make them from, a texture's levels are made at
// most every kRemakeGap seconds; the writes in between wait (the levels lag that much at most).
constexpr double kRemakeGap = 0.25;
struct Remade {
    double at = -1e9;
    bool noted = false;
};
std::unordered_map<Surf*, Remade> g_remade;
unsigned g_deferred = 0;

double now_s() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

// --- statistics for the 5-second log line ---
unsigned g_created = 0, g_created_total = 0, g_skipped = 0, g_failed = 0;
unsigned g_regens = 0, g_writes = 0, g_same = 0, g_from_copy = 0;
double g_regen_ms = 0, g_regen_ms_max = 0, g_read_ms = 0;
unsigned g_by_size[10];  // levels made, by log2 of the width
unsigned g_by_alpha[4];  // levels made, by alpha bits: none, 1, 2-4, 8
unsigned g_cut_out = 0;  // ... of them cut out (their solid share kept)
unsigned g_holed_by_binary[10];  // debug: textures with clear texels, by tenths of texels fully clear or solid
LARGE_INTEGER g_freq;

HRESULT(WINAPI* o_DirectDrawCreate)(GUID*, LPDIRECTDRAW*, IUnknown*) = nullptr;
HRESULT(WINAPI* o_DirectDrawCreateEx)(GUID*, LPVOID*, REFIID, IUnknown*) = nullptr;
HRESULT(STDMETHODCALLTYPE* o_CreateSurface)(IDirectDraw7*, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*, IUnknown*) = nullptr;
HRESULT(STDMETHODCALLTYPE* o_RestoreAllSurfaces)(IDirectDraw7*) = nullptr;

bool from_game(void* ret) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    return a >= 0x00401000 && a < 0x008A9000;  // runblack.exe's code (not ours, not d3dim's)
}

bool pow2(DWORD v) { return v && !(v & (v - 1)); }
int log2i(DWORD v) {
    int n = 0;
    while (v > 1) v >>= 1, ++n;
    return n;
}

// How many levels a texture created with this description gets (1: left alone).
int levels_for(const DDSURFACEDESC2& sd) {
    const DWORD caps = sd.ddsCaps.dwCaps;
    if (!(caps & DDSCAPS_TEXTURE) || (caps & (DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY))) return 1;
    // The game asks for a mip chain of one level (LH3D's texture slots, 0x85DEB0); a chain of
    // more is someone's own.
    if ((caps & DDSCAPS_MIPMAP) && (!(sd.dwFlags & DDSD_MIPMAPCOUNT) || sd.dwMipMapCount > 1)) return 1;
    if (!(sd.dwFlags & DDSD_WIDTH) || !(sd.dwFlags & DDSD_HEIGHT)) return 1;
    if (!pow2(sd.dwWidth) || !pow2(sd.dwHeight)) return 1;
    const DWORD m = std::min(sd.dwWidth, sd.dwHeight);
    if (int(m) < g_min_size) return 1;
    if (sd.dwFlags & DDSD_PIXELFORMAT) {
        const DDPIXELFORMAT& pf = sd.ddpfPixelFormat;
        if (!(pf.dwFlags & DDPF_RGB) || (pf.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 |
                                                       DDPF_PALETTEINDEXED8 | DDPF_FOURCC)))
            return 1;
        if (pf.dwRGBBitCount != 16 && pf.dwRGBBitCount != 32) return 1;
    }
    return std::max(1, std::min(g_levels, log2i(m) - 1));  // the smallest level 4 texels or more
}

void hook_fn(void* target, void* detour, void** orig, const char* what) {
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) BWLOG("mipmaps: hooking %s failed: %s", what, MH_StatusToString(st));
}

HRESULT STDMETHODCALLTYPE h_CreateSurface(IDirectDraw7* dd, LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7* out,
                                          IUnknown* outer) {
    void* ret = _ReturnAddress();
    const bool game = from_game(ret);
    const bool texture = sd && (sd->ddsCaps.dwCaps & DDSCAPS_TEXTURE);
    const int levels = g_enabled && game && sd ? levels_for(*sd) : 1;
    HRESULT hr = E_FAIL;
    bool chained = false;
    if (levels > 1) {
        DDSURFACEDESC2 m = *sd;
        m.dwFlags |= DDSD_MIPMAPCOUNT;
        m.dwMipMapCount = DWORD(levels);
        m.ddsCaps.dwCaps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
        hr = o_CreateSurface(dd, &m, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            chained = true;
            g_ours.insert(*out);
            ++g_created, ++g_created_total;
        } else {
            ++g_failed;
            static int logged = 0;
            if (logged++ < 5) BWLOG("mipmaps: CreateSurface with %d levels failed %08lX; made without", levels, hr);
        }
    }
    if (!chained) {
        hr = o_CreateSurface(dd, sd, out, outer);
        if (texture && game && SUCCEEDED(hr)) ++g_skipped;
    }
    if (SUCCEEDED(hr) && out && *out) {
        // The pointer may be a released surface's: nothing of that one carries over.
        if (!chained) g_ours.erase(*out);
        g_shadow.erase(*out);
        g_dirty.erase(*out);
        g_remade.erase(*out);
    }
    if (SUCCEEDED(hr) && out && *out) trace::hook_surface_methods(*out);
    if (texture && game) {
        static int logged = 0;
        if (logged++ < 40) {
            const DDPIXELFORMAT& pf = sd->ddpfPixelFormat;
            BWLOG("mipmaps: game texture %lux%lu caps=%08lX caps2=%08lX fmt=%s %lubpp masks %04lX/%04lX/%04lX/%04lX "
                  "-> %d level(s) %s (hr %08lX, from %p)",
                  sd->dwWidth, sd->dwHeight, sd->ddsCaps.dwCaps, sd->ddsCaps.dwCaps2,
                  (sd->dwFlags & DDSD_PIXELFORMAT) ? "given" : "default", pf.dwRGBBitCount, pf.dwRBitMask,
                  pf.dwGBitMask, pf.dwBBitMask, pf.dwRGBAlphaBitMask, chained ? levels : 1,
                  (sd->dwFlags & DDSD_CKSRCBLT) ? "colour-keyed" : "", hr, ret);
        }
    }
    return hr;
}

// Restored surfaces have lost their contents: the game uploads them again, and the copies
// mustn't take that for the same pixels.
void forget_contents() {
    for (auto& [s, c] : g_shadow) c.valid = false;
    g_locked.clear();
}

HRESULT STDMETHODCALLTYPE h_RestoreAllSurfaces(IDirectDraw7* dd) {
    forget_contents();
    return o_RestoreAllSurfaces(dd);
}

void hook_dd7(IUnknown* obj) {
    if (o_CreateSurface || !obj) return;
    IDirectDraw7* dd = nullptr;
    if (FAILED(obj->lpVtbl->QueryInterface(obj, IID_IDirectDraw7, reinterpret_cast<void**>(&dd))) || !dd) return;
    hook_fn(reinterpret_cast<void*>(dd->lpVtbl->CreateSurface), reinterpret_cast<void*>(&h_CreateSurface),
            reinterpret_cast<void**>(&o_CreateSurface), "IDirectDraw7::CreateSurface");
    hook_fn(reinterpret_cast<void*>(dd->lpVtbl->RestoreAllSurfaces), reinterpret_cast<void*>(&h_RestoreAllSurfaces),
            reinterpret_cast<void**>(&o_RestoreAllSurfaces), "IDirectDraw7::RestoreAllSurfaces");
    dd->lpVtbl->Release(dd);
    BWLOG("mipmaps: game textures get %d mip levels (min size %d)", g_levels, g_min_size);
}

HRESULT WINAPI h_DirectDrawCreate(GUID* g, LPDIRECTDRAW* out, IUnknown* outer) {
    HRESULT hr = o_DirectDrawCreate(g, out, outer);
    if (SUCCEEDED(hr) && out && *out) hook_dd7(reinterpret_cast<IUnknown*>(*out));
    return hr;
}

HRESULT WINAPI h_DirectDrawCreateEx(GUID* g, LPVOID* out, REFIID iid, IUnknown* outer) {
    HRESULT hr = o_DirectDrawCreateEx(g, out, iid, outer);
    if (SUCCEEDED(hr) && out && *out) hook_dd7(static_cast<IUnknown*>(*out));
    return hr;
}

// --- pixel formats ---
struct Channel {
    DWORD mask = 0;
    int shift = 0, bits = 0;
    void set(DWORD m) {
        mask = m, shift = 0, bits = 0;
        if (!m) return;
        while (!((m >> shift) & 1)) ++shift;
        while ((m >> (shift + bits)) & 1) ++bits;
    }
    uint8_t get(uint32_t px) const {
        if (!bits) return 255;
        const uint32_t v = (px & mask) >> shift, max = (1u << bits) - 1;
        return uint8_t((v * 255 + max / 2) / max);
    }
    uint32_t put(uint8_t c) const {
        if (!bits) return 0;
        const uint32_t max = (1u << bits) - 1;
        return (((uint32_t(c) * max + 127) / 255) << shift) & mask;
    }
};
struct Format {
    Channel r, g, b, a;
    int bpp = 0;
    bool ok = false;
};

Format format_of(const DDPIXELFORMAT& pf) {
    Format f;
    if (!(pf.dwFlags & DDPF_RGB) || (pf.dwFlags & DDPF_PALETTEINDEXED8)) return f;
    if (pf.dwRGBBitCount != 16 && pf.dwRGBBitCount != 32) return f;
    f.bpp = int(pf.dwRGBBitCount) / 8;
    f.r.set(pf.dwRBitMask), f.g.set(pf.dwGBitMask), f.b.set(pf.dwBBitMask);
    f.a.set((pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0);
    f.ok = true;
    return f;
}

struct Rgba {
    uint8_t r, g, b, a;
};

// One 2x2 box: 1-bit alpha by majority (a tie is kept), the colour from the opaque texels; more
// alpha bits are averaged, the colour weighted by alpha (no dark fringes from transparent texels).
Rgba box(const Rgba& p0, const Rgba& p1, const Rgba& p2, const Rgba& p3, bool one_bit_alpha) {
    const Rgba* p[4] = {&p0, &p1, &p2, &p3};
    Rgba o{};
    if (one_bit_alpha) {
        unsigned n = 0, r = 0, g = 0, b = 0;
        for (const Rgba* q : p)
            if (q->a >= 128) ++n, r += q->r, g += q->g, b += q->b;
        if (!n) {
            for (const Rgba* q : p) r += q->r, g += q->g, b += q->b;
            n = 4;
        }
        o.r = uint8_t((r + n / 2) / n), o.g = uint8_t((g + n / 2) / n), o.b = uint8_t((b + n / 2) / n);
        unsigned opaque = 0;
        for (const Rgba* q : p) opaque += q->a >= 128;
        o.a = opaque >= 2 ? 255 : 0;
        return o;
    }
    unsigned wa = 0, r = 0, g = 0, b = 0;
    for (const Rgba* q : p) wa += q->a, r += q->r * q->a, g += q->g * q->a, b += q->b * q->a;
    if (wa) {
        o.r = uint8_t((r + wa / 2) / wa), o.g = uint8_t((g + wa / 2) / wa), o.b = uint8_t((b + wa / 2) / wa);
    } else {
        r = g = b = 0;
        for (const Rgba* q : p) r += q->r, g += q->g, b += q->b;
        o.r = uint8_t((r + 2) / 4), o.g = uint8_t((g + 2) / 4), o.b = uint8_t((b + 2) / 4);
    }
    o.a = uint8_t((wa + 2) / 4);
    return o;
}

bool is_our_top(Surf* s, DDSURFACEDESC2* out) {
    if (!s || !g_ours.count(s)) return false;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(s->lpVtbl->GetSurfaceDesc(s, &d))) return false;
    const bool ok = (d.ddsCaps.dwCaps & DDSCAPS_MIPMAP) && (d.ddsCaps.dwCaps & DDSCAPS_TEXTURE) &&
                    !(d.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL) && (d.dwFlags & DDSD_MIPMAPCOUNT) &&
                    d.dwMipMapCount > 1;
    if (!ok) {
        g_ours.erase(s);  // the pointer now belongs to another surface
        return false;
    }
    if (out) *out = d;
    return true;
}

// Makes the lower levels of the region `dirty` of the top level again.
void regenerate(Surf* top, const RECT* dirty) {
    DDSURFACEDESC2 d;
    if (!is_our_top(top, &d)) return;
    const Format f = format_of(d.ddpfPixelFormat);
    if (!f.ok) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    t_busy = true;
    const int levels = int(d.dwMipMapCount);
    const LONG block = 1L << (levels - 1);
    const LONG W = LONG(d.dwWidth), H = LONG(d.dwHeight);
    RECT r = dirty ? *dirty : RECT{0, 0, W, H};
    // Whole blocks, so that every lower level's texels in the region come from the region alone.
    r.left = std::max(0L, r.left & ~(block - 1));
    r.top = std::max(0L, r.top & ~(block - 1));
    r.right = std::min(W, (r.right + block - 1) & ~(block - 1));
    r.bottom = std::min(H, (r.bottom + block - 1) & ~(block - 1));
    if (r.right <= r.left || r.bottom <= r.top) {
        t_busy = false;
        return;
    }
    LONG w = r.right - r.left, h = r.bottom - r.top;
    std::vector<Rgba> cur(size_t(w) * h), next;
    auto sh = g_shadow.find(top);
    if (sh != g_shadow.end() && sh->second.valid && sh->second.w == W && sh->second.h == H && sh->second.bpp == f.bpp) {
        const Shadow& c = sh->second;
        for (LONG y = 0; y < h; ++y) {
            const BYTE* row = c.px.data() + (size_t(r.top + y) * W + r.left) * c.bpp;
            for (LONG x = 0; x < w; ++x) {
                const uint32_t px = f.bpp == 2 ? reinterpret_cast<const uint16_t*>(row)[x]
                                               : reinterpret_cast<const uint32_t*>(row)[x];
                cur[size_t(y) * w + x] = {f.r.get(px), f.g.get(px), f.b.get(px), f.a.get(px)};
            }
        }
        ++g_from_copy;
    } else {
        DDSURFACEDESC2 ld{};
        ld.dwSize = sizeof(ld);
        if (FAILED(top->lpVtbl->Lock(top, &r, &ld, DDLOCK_READONLY | DDLOCK_WAIT | DDLOCK_NOSYSLOCK, nullptr))) {
            t_busy = false;
            return;
        }
        for (LONG y = 0; y < h; ++y) {
            const BYTE* row = static_cast<const BYTE*>(ld.lpSurface) + size_t(y) * ld.lPitch;
            for (LONG x = 0; x < w; ++x) {
                const uint32_t px = f.bpp == 2 ? reinterpret_cast<const uint16_t*>(row)[x]
                                               : reinterpret_cast<const uint32_t*>(row)[x];
                cur[size_t(y) * w + x] = {f.r.get(px), f.g.get(px), f.b.get(px), f.a.get(px)};
            }
        }
        top->lpVtbl->Unlock(top, &r);
    }
    {
        LARGE_INTEGER tr;
        QueryPerformanceCounter(&tr);
        g_read_ms += double(tr.QuadPart - t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        ++g_by_size[std::min(9, log2i(DWORD(W)))];
    }
    const bool one_bit = f.a.bits == 1;
    ++g_by_alpha[f.a.bits == 0 ? 0 : one_bit ? 1 : f.a.bits <= 4 ? 2 : 3];
    // Cut-out textures (fences, foliage) are drawn with an alpha test. Averaged, their thin solid
    // parts fall below it level by level, and far away they thinned out and vanished (a fence
    // line: solid at 26 m, mostly gone by 35 m; player, 2026-10-20). The lower levels' alpha keeps
    // the top level's share of texels passing the test: `cov` is each texel's share of solid from
    // the top, and each level is cut where the same share passes. A texture counts as cut out when
    // it has holes and its alpha is mostly (80%) fully clear or solid (the game's are 4444; ones
    // with in-between alpha, such as smoke, are blended and keep the plain average).
    size_t clear = 0, binary = 0;
    for (const Rgba& c : cur) clear += c.a == 0, binary += c.a == 0 || c.a == 255;
    const bool cut_out = clear > 0 && (one_bit || binary * 100 >= cur.size() * 80);
    if (clear > 0) ++g_holed_by_binary[std::min<size_t>(9, binary * 10 / cur.size())];
    const bool keep_coverage = g_keep_coverage && f.a.bits > 0 && cut_out;
    std::vector<float> cov, cov_next, sorted;
    double solid_share = 1.0;
    if (keep_coverage) {
        ++g_cut_out;
        cov.resize(cur.size());
        size_t solid = 0;
        for (size_t i = 0; i < cur.size(); ++i) {
            cov[i] = cur[i].a >= 128 ? 1.0f : 0.0f;
            solid += cur[i].a >= 128;
        }
        solid_share = double(solid) / double(cur.size());
    }
    Surf* level = top;
    level->lpVtbl->AddRef(level);
    for (int k = 1; k < levels && w >= 2 && h >= 2; ++k) {
        DDSCAPS2 caps{};
        caps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        Surf* sub = nullptr;
        const HRESULT hr = level->lpVtbl->GetAttachedSurface(level, &caps, &sub);
        level->lpVtbl->Release(level);
        level = sub;
        if (FAILED(hr) || !sub) break;
        const LONG nw = w / 2, nh = h / 2;
        next.resize(size_t(nw) * nh);
        for (LONG y = 0; y < nh; ++y)
            for (LONG x = 0; x < nw; ++x) {
                const Rgba* a = &cur[size_t(2 * y) * w + 2 * x];
                const Rgba* b = a + w;
                next[size_t(y) * nw + x] = box(a[0], a[1], b[0], b[1], one_bit);
            }
        if (keep_coverage) {
            cov_next.resize(next.size());
            for (LONG y = 0; y < nh; ++y)
                for (LONG x = 0; x < nw; ++x) {
                    const float* a = &cov[size_t(2 * y) * w + 2 * x];
                    const float* b = a + w;
                    cov_next[size_t(y) * nw + x] = (a[0] + a[1] + b[0] + b[1]) * 0.25f;
                }
            cov.swap(cov_next);
            // The level at which the top level's share of texels is at or above it.
            const size_t n = cov.size(), pass = size_t(solid_share * double(n) + 0.5);
            float t = 2.0f;  // (none pass)
            if (pass > 0) {
                sorted = cov;
                std::nth_element(sorted.begin(), sorted.begin() + (n - pass), sorted.end());
                t = std::max(sorted[n - pass], 1e-3f);
            }
            for (size_t i = 0; i < n; ++i) next[i].a = cov[i] >= t ? 255 : 0;
        }
        cur.swap(next);
        w = nw, h = nh;
        RECT rk{r.left >> k, r.top >> k, (r.left >> k) + w, (r.top >> k) + h};
        DDSURFACEDESC2 ld{};
        ld.dwSize = sizeof(ld);
        if (FAILED(level->lpVtbl->Lock(level, &rk, &ld, DDLOCK_WRITEONLY | DDLOCK_WAIT | DDLOCK_NOSYSLOCK, nullptr)))
            break;
        for (LONG y = 0; y < h; ++y) {
            BYTE* row = static_cast<BYTE*>(ld.lpSurface) + size_t(y) * ld.lPitch;
            for (LONG x = 0; x < w; ++x) {
                const Rgba& c = cur[size_t(y) * w + x];
                const uint32_t px = f.r.put(c.r) | f.g.put(c.g) | f.b.put(c.b) | f.a.put(c.a);
                if (f.bpp == 2) reinterpret_cast<uint16_t*>(row)[x] = uint16_t(px);
                else reinterpret_cast<uint32_t*>(row)[x] = px;
            }
        }
        level->lpVtbl->Unlock(level, &rk);
    }
    if (level) level->lpVtbl->Release(level);
    t_busy = false;
    QueryPerformanceCounter(&t1);
    const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
    ++g_regens;
    g_regen_ms += ms;
    g_regen_ms_max = std::max(g_regen_ms_max, ms);
}

RECT surface_rect(Surf* s) {
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    s->lpVtbl->GetSurfaceDesc(s, &d);
    return RECT{0, 0, LONG(d.dwWidth), LONG(d.dwHeight)};
}

void mark_dirty(Surf* s, const RECT* rect) {
    if (t_busy || !g_ours.count(s)) return;
    ++g_writes;
    const RECT r = rect ? *rect : surface_rect(s);
    auto [it, fresh] = g_dirty.try_emplace(s, r);
    if (!fresh) {
        RECT& u = it->second;
        u.left = std::min(u.left, r.left), u.top = std::min(u.top, r.top);
        u.right = std::max(u.right, r.right), u.bottom = std::max(u.bottom, r.bottom);
    }
}

// The CPU copy of a small texture of ours (made on first use), or null.
Shadow* shadow_of(Surf* s, bool create) {
    auto it = g_shadow.find(s);
    if (it != g_shadow.end()) return &it->second;
    if (!create) return nullptr;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(s->lpVtbl->GetSurfaceDesc(s, &d))) return nullptr;
    const int bpp = int(d.ddpfPixelFormat.dwRGBBitCount) / 8;
    if ((bpp != 2 && bpp != 4) || size_t(d.dwWidth) * d.dwHeight * bpp > kMaxShadowBytes) return nullptr;
    Shadow& c = g_shadow[s];
    c.w = LONG(d.dwWidth), c.h = LONG(d.dwHeight), c.bpp = bpp;
    c.px.assign(size_t(c.w) * c.h * bpp, 0);
    return &c;
}

// Copies a rectangle into the copy; true if any byte differs (or the copy didn't hold it yet).
bool copy_rows(Shadow& c, const RECT& r, const BYTE* bits, LONG pitch) {
    if (r.left < 0 || r.top < 0 || r.right > c.w || r.bottom > c.h || r.right <= r.left || r.bottom <= r.top) {
        c.valid = false;
        return true;
    }
    const bool whole = r.left == 0 && r.top == 0 && r.right == c.w && r.bottom == c.h;
    bool changed = !c.valid;
    const size_t n = size_t(r.right - r.left) * c.bpp;
    for (LONG y = r.top; y < r.bottom; ++y) {
        BYTE* dst = c.px.data() + (size_t(y) * c.w + r.left) * c.bpp;
        const BYTE* src = bits + size_t(y - r.top) * pitch;
        if (!changed && memcmp(dst, src, n) != 0) changed = true;
        if (changed) memcpy(dst, src, n);
    }
    if (whole) c.valid = true;
    return changed;
}

// A Blt (or BltFast) wrote rectangle r of an our texture from rectangle sr of src.
void write_from(Surf* s, const RECT& r, Surf* src, const RECT& sr) {
    Shadow* c = shadow_of(s, true);
    auto from = src ? g_shadow.find(src) : g_shadow.end();
    const bool same_size = r.right - r.left == sr.right - sr.left && r.bottom - r.top == sr.bottom - sr.top;
    if (c && from != g_shadow.end() && from->second.valid && from->second.bpp == c->bpp && same_size &&
        sr.left >= 0 && sr.top >= 0 && sr.right <= from->second.w && sr.bottom <= from->second.h) {
        const Shadow& f = from->second;
        const BYTE* bits = f.px.data() + (size_t(sr.top) * f.w + sr.left) * f.bpp;
        if (!copy_rows(*c, r, bits, f.w * f.bpp)) {
            ++g_same;
            return;  // the same pixels again: the levels stand
        }
    } else if (c) {
        c->valid = false;
    }
    mark_dirty(s, &r);
}

}  // namespace

void configure() {
    QueryPerformanceFrequency(&g_freq);
    g_enabled = paths::ini_int(L"textures", L"mipmaps", 1) != 0;
    g_levels = std::clamp(paths::ini_int(L"textures", L"mip_levels", 4), 1, 9);
    g_min_size = std::max(4, paths::ini_int(L"textures", L"min_size", 16));
    g_keep_coverage = paths::ini_int(L"textures", L"keep_coverage", 1) != 0;
    if (!g_enabled || g_levels < 2) {
        g_enabled = false;
        BWLOG("mipmaps: off ([textures] mipmaps=0 or mip_levels<2)");
    }
}

void on_ddraw_loaded(FARPROC create, FARPROC create_ex) {
    if (!g_enabled) return;
    if (create)
        hook_fn(reinterpret_cast<void*>(create), reinterpret_cast<void*>(&h_DirectDrawCreate),
                reinterpret_cast<void**>(&o_DirectDrawCreate), "DirectDrawCreate");
    if (create_ex)
        hook_fn(reinterpret_cast<void*>(create_ex), reinterpret_cast<void*>(&h_DirectDrawCreateEx),
                reinterpret_cast<void**>(&o_DirectDrawCreateEx), "DirectDrawCreateEx");
}

void after_blt(void* dst, const RECT* dst_rect, void* src, const RECT* src_rect) {
    Surf* s = static_cast<Surf*>(dst);
    if (t_busy || !g_ours.count(s)) return;
    const RECT r = dst_rect ? *dst_rect : surface_rect(s);
    const RECT sr = src && src_rect ? *src_rect : (src ? surface_rect(static_cast<Surf*>(src)) : RECT{});
    write_from(s, r, static_cast<Surf*>(src), sr);
}

void after_blt_fast(void* dst, DWORD x, DWORD y, void* src, const RECT* src_rect) {
    Surf* s = static_cast<Surf*>(dst);
    if (t_busy || !g_ours.count(s) || !src) return;
    const RECT sr = src_rect ? *src_rect : surface_rect(static_cast<Surf*>(src));
    const RECT r{LONG(x), LONG(y), LONG(x) + (sr.right - sr.left), LONG(y) + (sr.bottom - sr.top)};
    write_from(s, r, static_cast<Surf*>(src), sr);
}

void after_lock(void* surface, const RECT* rect, DWORD flags, const void* desc) {
    Surf* s = static_cast<Surf*>(surface);
    if (t_busy || (flags & DDLOCK_READONLY) || !g_ours.count(s)) return;
    for (const Pending& p : g_locked)
        if (p.s == s) return;
    const auto* d = static_cast<const DDSURFACEDESC2*>(desc);
    g_locked.push_back({s, rect ? *rect : RECT{}, rect == nullptr, d ? static_cast<const BYTE*>(d->lpSurface) : nullptr,
                        d ? d->lPitch : 0, true});
}

void before_unlock(void* surface) {
    Surf* s = static_cast<Surf*>(surface);
    if (t_busy || g_locked.empty()) return;
    for (Pending& p : g_locked)
        if (p.s == s) {
            Shadow* c = shadow_of(s, true);
            if (!c) return;
            if (!p.bits) {
                c->valid = false;
                return;
            }
            const RECT r = p.whole ? RECT{0, 0, c->w, c->h} : p.r;
            p.changed = copy_rows(*c, r, p.bits, p.pitch);
            return;
        }
}

void after_unlock(void* surface) {
    Surf* s = static_cast<Surf*>(surface);
    if (t_busy || g_locked.empty()) return;
    for (size_t i = 0; i < g_locked.size(); ++i)
        if (g_locked[i].s == s) {
            const Pending p = g_locked[i];
            g_locked.erase(g_locked.begin() + ptrdiff_t(i));
            if (p.changed) mark_dirty(s, p.whole ? nullptr : &p.r);
            else ++g_same;
            return;
        }
}

void after_load(void* dst, const POINT* at, void* src, const RECT* src_rect) {
    Surf* s = static_cast<Surf*>(dst);
    if (t_busy || !g_ours.count(s)) return;
    if (Shadow* c = shadow_of(s, false)) c->valid = false;
    if (!src) return mark_dirty(s, nullptr);
    const RECT sr = src_rect ? *src_rect : surface_rect(static_cast<Surf*>(src));
    const LONG x = at ? at->x : sr.left, y = at ? at->y : sr.top;
    const RECT r{x, y, x + (sr.right - sr.left), y + (sr.bottom - sr.top)};
    mark_dirty(s, &r);
}

void after_restore(void* surface) {
    Surf* s = static_cast<Surf*>(surface);
    if (!g_ours.count(s)) return;
    if (Shadow* c = shadow_of(s, false)) c->valid = false;
    mark_dirty(s, nullptr);
}

void before_bind(void* texture) {
    if (g_dirty.empty() || !texture) return;
    Surf* s = static_cast<Surf*>(texture);
    auto it = g_dirty.find(s);
    if (it == g_dirty.end()) return;
    const auto sh = g_shadow.find(s);
    const bool from_copy = sh != g_shadow.end() && sh->second.valid;
    Remade& m = g_remade[s];
    const double now = now_s();
    if (!from_copy && now - m.at < kRemakeGap) {
        ++g_deferred;
        if (!m.noted) {
            m.noted = true;
            const RECT full = surface_rect(s);
            BWLOG("mipmaps: a %ldx%ld texture is written again within %.2f s: its levels are made at most every %.2f s",
                  full.right, full.bottom, now - m.at, kRemakeGap);
        }
        return;  // (stays dirty: made at a later bind)
    }
    m.at = now;
    const RECT r = it->second;
    g_dirty.erase(it);
    regenerate(s, &r);
}

void set_levels(int levels) {
    g_levels = std::clamp(levels, 1, 9);
    BWLOG("mipmaps: textures created from now on get %d level(s)", g_levels);
}

std::string stats_line() {
    char buf[768];
    snprintf(buf, sizeof(buf),
             "mipmaps: %u textures made with levels (%u in all, %u left single, %u failed), %u writes (%u the same "
             "again), %u levels made (%u from copies; avg %.3f ms, max %.3f ms, %.2f ms in all; %u put off) (5 s)",
             g_created, g_created_total, g_skipped, g_failed, g_writes, g_same, g_regens, g_from_copy, g_regens ? g_regen_ms / g_regens : 0.0,
             g_regen_ms_max, g_regen_ms, g_deferred);
    char more[200];
    snprintf(more, sizeof(more), " | reading %.2f ms; by width 16/32/64/128/256: %u %u %u %u %u; by alpha bits 0/1/2-4/8: %u %u %u %u (%u cut out)",
             g_read_ms, g_by_size[4], g_by_size[5], g_by_size[6], g_by_size[7], g_by_size[8], g_by_alpha[0], g_by_alpha[1],
             g_by_alpha[2], g_by_alpha[3], g_cut_out);
    strncat(buf, more, sizeof(buf) - strlen(buf) - 1);
    char holed[120];
    snprintf(holed, sizeof(holed), "; with holes, by on/off tenths: %u %u %u %u %u %u %u %u %u %u", g_holed_by_binary[0], g_holed_by_binary[1],
             g_holed_by_binary[2], g_holed_by_binary[3], g_holed_by_binary[4], g_holed_by_binary[5], g_holed_by_binary[6], g_holed_by_binary[7],
             g_holed_by_binary[8], g_holed_by_binary[9]);
    strncat(buf, holed, sizeof(buf) - strlen(buf) - 1);
    memset(g_holed_by_binary, 0, sizeof(g_holed_by_binary));
    g_read_ms = 0;
    memset(g_by_size, 0, sizeof(g_by_size));
    memset(g_by_alpha, 0, sizeof(g_by_alpha));
    g_cut_out = 0;
    g_created = g_regens = g_writes = g_same = g_from_copy = g_deferred = 0;
    g_regen_ms = g_regen_ms_max = 0;
    return buf;
}

}  // namespace bw::mipmaps
