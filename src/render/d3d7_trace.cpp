// Logs what the game sends to IDirect3DDevice7 by patching the device's vtable.
// Always: cheap per-frame counters. On request: a full per-call text trace of N frames plus a
// screenshot of each traced frame, written to <game>\bwvr\captures\trace_<frame>.txt/.bmp.
#define CINTERFACE
#include "render/d3d7_trace.h"

#include <windows.h>

#include <ddraw.h>
#include <d3d.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/log.h"
#include "core/paths.h"
#include "game/engine.h"
#include "render/mipmaps.h"
#include "render/screenshot.h"

namespace bw::trace {

namespace {

using Dev = IDirect3DDevice7;

// --- capture state (game thread only) ---
FILE* g_cap = nullptr;
int g_cap_pending = 0;   // frames requested, not yet started
int g_stack_depth = 6;   // [trace] stack_depth

// --- counters for the current frame and the last completed frame ---
struct Counters {
    unsigned draws = 0, draws_xyz = 0, draws_rhw = 0, draws_vb = 0, prims = 0, set_transform = 0, clears = 0;
};
Counters g_cur, g_last;

// --- original vtable entries ---
HRESULT(STDMETHODCALLTYPE* o_BeginScene)(Dev*);
HRESULT(STDMETHODCALLTYPE* o_EndScene)(Dev*);
HRESULT(STDMETHODCALLTYPE* o_SetRenderTarget)(Dev*, LPDIRECTDRAWSURFACE7, DWORD);
HRESULT(STDMETHODCALLTYPE* o_Clear)(Dev*, DWORD, LPD3DRECT, DWORD, D3DCOLOR, D3DVALUE, DWORD);
HRESULT(STDMETHODCALLTYPE* o_SetTransform)(Dev*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
HRESULT(STDMETHODCALLTYPE* o_SetViewport)(Dev*, LPD3DVIEWPORT7);
HRESULT(STDMETHODCALLTYPE* o_SetRenderState)(Dev*, D3DRENDERSTATETYPE, DWORD);
HRESULT(STDMETHODCALLTYPE* o_DrawPrimitiveStrided)(Dev*, D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD);
HRESULT(STDMETHODCALLTYPE* o_DrawIndexedPrimitiveStrided)(Dev*, D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD);
HRESULT(STDMETHODCALLTYPE* o_DrawPrimitiveVB)(Dev*, D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER7, DWORD, DWORD, DWORD);
HRESULT(STDMETHODCALLTYPE* o_DrawIndexedPrimitiveVB)(Dev*, D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER7, DWORD, DWORD, LPWORD, DWORD, DWORD);
HRESULT(STDMETHODCALLTYPE* o_SetTexture)(Dev*, DWORD, LPDIRECTDRAWSURFACE7);
HRESULT(STDMETHODCALLTYPE* o_SetTextureStageState)(Dev*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);

const char* prim_name(D3DPRIMITIVETYPE t) {
    switch (t) {
        case D3DPT_POINTLIST: return "POINTS";
        case D3DPT_LINELIST: return "LINES";
        case D3DPT_LINESTRIP: return "LINESTRIP";
        case D3DPT_TRIANGLELIST: return "TRILIST";
        case D3DPT_TRIANGLESTRIP: return "TRISTRIP";
        case D3DPT_TRIANGLEFAN: return "TRIFAN";
        default: return "?";
    }
}

unsigned prim_count(D3DPRIMITIVETYPE t, DWORD n) {
    switch (t) {
        case D3DPT_POINTLIST: return n;
        case D3DPT_LINELIST: return n / 2;
        case D3DPT_LINESTRIP: return n ? n - 1 : 0;
        case D3DPT_TRIANGLELIST: return n / 3;
        default: return n > 2 ? n - 2 : 0;
    }
}

bool is_rhw(DWORD fvf) { return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW; }

void count_draw(DWORD fvf, D3DPRIMITIVETYPE t, DWORD n, bool vb) {
    ++g_cur.draws;
    g_cur.prims += prim_count(t, n);
    if (vb) ++g_cur.draws_vb;
    if (is_rhw(fvf)) ++g_cur.draws_rhw;
    else ++g_cur.draws_xyz;
}

// "XYZ,N,D,S,T2" style FVF description.
std::string fvf_desc(DWORD fvf) {
    std::string s;
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: s = "XYZ"; break;
        case D3DFVF_XYZRHW: s = "XYZRHW"; break;
        case 0: s = "NOPOS"; break;
        default: s = "XYZB" + std::to_string(((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZ) / 2); break;
    }
    if (fvf & D3DFVF_NORMAL) s += ",N";
    if (fvf & D3DFVF_DIFFUSE) s += ",D";
    if (fvf & D3DFVF_SPECULAR) s += ",S";
    s += ",T" + std::to_string((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    return s;
}

// The game and d3dim700 are built without frame pointers, so RtlCaptureStackBackTrace finds
// nothing. Instead scan the stack for values that point into runblack.exe's code right after a
// CALL instruction: a heuristic, but reliable enough to attribute draws to engine functions.
bool looks_like_return_address(uintptr_t a) {
    constexpr uintptr_t kTextBegin = 0x00401000, kTextEnd = 0x008A9000;
    if (a < kTextBegin + 8 || a >= kTextEnd) return false;
    const BYTE* p = reinterpret_cast<const BYTE*>(a);
    if (p[-5] == 0xE8) return true;                                  // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;        // call [disp32] / [reg+disp32]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;        // call [reg+disp8]
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;        // call reg / [reg]
    return false;
}

__declspec(noinline) void cap_stack() {
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    fputs(" |", g_cap);
    int found = 0;
    for (int i = 0; i < 512 && found < g_stack_depth; ++i) {
        uintptr_t v = sp[i];
        if (looks_like_return_address(v)) {
            fprintf(g_cap, " %08X", unsigned(v));
            ++found;
        }
    }
    fputc('\n', g_cap);
}

// Bytes per vertex for the FVF formats the game uses (position, normal, colours, 2D texcoords).
size_t fvf_stride(DWORD fvf) {
    size_t s = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: s = 12; break;
        case D3DFVF_XYZRHW: s = 16; break;
        default: s = 12 + 4 * (((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZ) / 2); break;
    }
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    s += 8 * ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    return s;
}

// FNV-1a over the whole vertex array, so passes can be compared beyond the first vertex.
void cap_hash(DWORD fvf, const void* verts, DWORD n) {
    if (!verts) return;
    const BYTE* p = static_cast<const BYTE*>(verts);
    const size_t len = fvf_stride(fvf) * n;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
    fprintf(g_cap, " vh=%08X", h);
}

// Screen bounding box and smallest/largest rhw of the vertices actually referenced by the draw.
void cap_bbox(DWORD fvf, const void* verts, DWORD nverts, const WORD* idx, DWORD ni) {
    if (!verts || !is_rhw(fvf)) return;
    const size_t stride = fvf_stride(fvf);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
    const DWORD count = idx ? ni : nverts;
    for (DWORD k = 0; k < count; ++k) {
        const DWORD i = idx ? idx[k] : k;
        const float* p = reinterpret_cast<const float*>(static_cast<const BYTE*>(verts) + size_t(i) * stride);
        x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
        w0 = std::min(w0, p[3]), w1 = std::max(w1, p[3]);
    }
    fprintf(g_cap, " bbox=(%.0f,%.0f)-(%.0f,%.0f) rhw=[%.4g,%.4g]", x0, y0, x1, y1, w0, w1);
}

void cap_vertex0(DWORD fvf, const void* verts) {
    if (!verts) return;
    const float* v = static_cast<const float*>(verts);
    if (is_rhw(fvf)) fprintf(g_cap, " v0=(%.1f,%.1f,%.4f,%.4f)", v[0], v[1], v[2], v[3]);
    else fprintf(g_cap, " v0=(%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    // First texture coordinate pair of vertex 0 (it follows the position, normal and colours).
    if ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) {
        const size_t uv_off = fvf_stride(fvf) - 8 * ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
        const float* uv = reinterpret_cast<const float*>(static_cast<const BYTE*>(verts) + uv_off);
        fprintf(g_cap, " uv0=(%.4f,%.4f)", uv[0], uv[1]);
    }
}

DWORD vb_fvf(LPDIRECT3DVERTEXBUFFER7 vb) {
    D3DVERTEXBUFFERDESC d{};
    d.dwSize = sizeof(d);
    return vb && SUCCEEDED(vb->lpVtbl->GetVertexBufferDesc(vb, &d)) ? d.dwFVF : 0;
}

// ---------------- draw batching analysis ----------------
// How many eye-pass draws follow the previous one with the same vertex format and type and no
// real state change in between (a state set to the value it already has doesn't count): the
// draws that could be merged into one call.
struct Batching {
    bool dirty = true;
    DWORD fvf = 0;
    D3DPRIMITIVETYPE type = D3DPT_POINTLIST;
    unsigned draws = 0, mergeable = 0;
    DWORD rs[256] = {}, tss[8][32] = {};
    bool rs_known[256] = {}, tss_known[8][32] = {};
    void* tex[8] = {};
    bool tex_known[8] = {};
} g_batch;
void state_changed() { g_batch.dirty = true; }
void note_rs(D3DRENDERSTATETYPE st, DWORD v) {
    const unsigned i = unsigned(st);
    if (i >= 256 || !g_batch.rs_known[i] || g_batch.rs[i] != v) g_batch.dirty = true;
    if (i < 256) g_batch.rs[i] = v, g_batch.rs_known[i] = true;
}
void note_tss(DWORD stage, D3DTEXTURESTAGESTATETYPE st, DWORD v) {
    const unsigned i = unsigned(st);
    if (stage >= 8 || i >= 32 || !g_batch.tss_known[stage][i] || g_batch.tss[stage][i] != v) g_batch.dirty = true;
    if (stage < 8 && i < 32) g_batch.tss[stage][i] = v, g_batch.tss_known[stage][i] = true;
}
void note_tex(DWORD stage, void* t) {
    if (stage >= 8 || !g_batch.tex_known[stage] || g_batch.tex[stage] != t) g_batch.dirty = true;
    if (stage < 8) g_batch.tex[stage] = t, g_batch.tex_known[stage] = true;
}
extern bool g_in_eye_pass;
unsigned g_pass_draws = 0, g_pass_prims = 0;  // (this eye pass's draws, and their triangles)
void note_draw(DWORD fvf, D3DPRIMITIVETYPE t) {
    if (g_in_eye_pass) ++g_pass_draws;
    if (!g_in_eye_pass) {
        g_batch.dirty = true;
        return;
    }
    ++g_batch.draws;
    if (!g_batch.dirty && fvf == g_batch.fvf && t == D3DPT_TRIANGLELIST && g_batch.type == D3DPT_TRIANGLELIST) ++g_batch.mergeable;
    g_batch.dirty = false, g_batch.fvf = fvf, g_batch.type = t;
}

// ---------------- detours ----------------
extern int g_draw_in_scene;
extern IDirectDrawSurface7* g_last_tex0;
bool skip_this_draw();

bool recording();
void rec_state_fwd(int op, DWORD a, DWORD b, DWORD c, void* ptr);
void rec_misc(int op, const void* data);
struct XformArgs {
    DWORD t;
    const D3DMATRIX* m;
};
struct ClearArgs {
    DWORD n;
    LPD3DRECT r;
    DWORD flags;
    D3DCOLOR c;
    D3DVALUE z;
    DWORD st;
};
HRESULT STDMETHODCALLTYPE h_BeginScene(Dev* d) {
    g_draw_in_scene = 0;
    state_changed();
    if (recording()) rec_state_fwd(6, 0, 0, 0, nullptr);
    if (g_cap) fputs("BeginScene\n", g_cap);
    return o_BeginScene(d);
}
HRESULT STDMETHODCALLTYPE h_EndScene(Dev* d) {
    state_changed();
    if (recording()) rec_state_fwd(7, 0, 0, 0, nullptr);
    if (g_cap) fputs("EndScene\n", g_cap);
    return o_EndScene(d);
}
HRESULT STDMETHODCALLTYPE h_SetRenderTarget(Dev* d, LPDIRECTDRAWSURFACE7 s, DWORD f) {
    state_changed();
    if (recording()) rec_state_fwd(-1, 0, 0, 0, nullptr);  // (not replayable: noted)
    if (g_cap) {
        DDSURFACEDESC2 sd{};
        sd.dwSize = sizeof(sd);
        if (s) s->lpVtbl->GetSurfaceDesc(s, &sd);
        fprintf(g_cap, "SetRenderTarget %p %lux%lu caps=%08lX", s, sd.dwWidth, sd.dwHeight, sd.ddsCaps.dwCaps);
        cap_stack();
    }
    return o_SetRenderTarget(d, s, f);
}
HRESULT STDMETHODCALLTYPE h_Clear(Dev* d, DWORD n, LPD3DRECT r, DWORD flags, D3DCOLOR c, D3DVALUE z, DWORD st) {
    ++g_cur.clears;
    state_changed();
    if (recording()) {  // (recorded, not done: the replay clears each eye)
        const ClearArgs args{n, r, flags, c, z, st};
        rec_misc(5, &args);
        return D3D_OK;
    }
    if (g_cap) {
        fprintf(g_cap, "Clear n=%lu flags=%lX color=%08lX z=%.3f", n, flags, c, z);
        cap_stack();
    }
    return o_Clear(d, n, r, flags, c, z, st);
}
HRESULT STDMETHODCALLTYPE h_SetTransform(Dev* d, D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) {
    ++g_cur.set_transform;
    state_changed();
    if (recording() && m) {
        const XformArgs args{DWORD(t), m};
        rec_misc(3, &args);
    }
    if (g_cap && m) {
        fprintf(g_cap, "SetTransform %d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                int(t), m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33,
                m->_34, m->_41, m->_42, m->_43, m->_44);
        cap_stack();
    }
    return o_SetTransform(d, t, m);
}
HRESULT STDMETHODCALLTYPE h_SetViewport(Dev* d, LPD3DVIEWPORT7 v) {
    state_changed();
    if (recording() && v) rec_misc(4, v);
    if (g_cap && v)
        fprintf(g_cap, "SetViewport %lu,%lu %lux%lu z=[%.3f,%.3f]\n", v->dwX, v->dwY, v->dwWidth, v->dwHeight,
                v->dvMinZ, v->dvMaxZ);
    return o_SetViewport(d, v);
}
HRESULT STDMETHODCALLTYPE h_SetRenderState(Dev* d, D3DRENDERSTATETYPE s, DWORD v) {
    note_rs(s, v);
    if (recording()) rec_state_fwd(0, DWORD(s), v, 0, nullptr);
    if (g_cap) fprintf(g_cap, "RS %d=%lX\n", int(s), v);
    return o_SetRenderState(d, s, v);
}
HRESULT STDMETHODCALLTYPE h_SetTexture(Dev* d, DWORD stage, LPDIRECTDRAWSURFACE7 s) {
    if (stage == 0) g_last_tex0 = s;
    if (recording()) rec_state_fwd(2, stage, 0, 0, s);
    note_tex(stage, s);
    mipmaps::before_bind(s);
    if (g_cap) {
        DWORD mips = 0, w = 0, h = 0;
        if (s) {
            DDSURFACEDESC2 sd{};
            sd.dwSize = sizeof(sd);
            if (SUCCEEDED(s->lpVtbl->GetSurfaceDesc(s, &sd))) mips = (sd.dwFlags & DDSD_MIPMAPCOUNT) ? sd.dwMipMapCount : 1, w = sd.dwWidth, h = sd.dwHeight;
        }
        fprintf(g_cap, "Tex %lu=%p (%lux%lu, %lu mip levels)\n", stage, s, w, h, mips);
    }
    return o_SetTexture(d, stage, s);
}
HRESULT STDMETHODCALLTYPE h_SetTextureStageState(Dev* d, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    note_tss(stage, t, v);
    if (recording()) rec_state_fwd(1, stage, DWORD(t), v, nullptr);
    if (g_cap) fprintf(g_cap, "TSS %lu.%d=%lX\n", stage, int(t), v);
    return o_SetTextureStageState(d, stage, t, v);
}
// --- Draw-call implementation variants ---
// d3dim700 rewrites the DrawPrimitive/DrawIndexedPrimitive slots of the device vtable on the fly
// (different implementations for different render states). Hooking only the function that was in
// the slot at startup missed ~40% of the draws -- including landscape and sea, which therefore
// bypassed the near-plane clip. Every implementation that ever appears in a slot gets its own
// detour and trampoline (track_draw_variants, polled every frame and before engine draws).
extern int g_draw_depth;
using DpFn = HRESULT(STDMETHODCALLTYPE*)(Dev*, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD);
using DipFn = HRESULT(STDMETHODCALLTYPE*)(Dev*, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD);
constexpr int kMaxVariants = 8;
DpFn g_dp_orig[kMaxVariants];
DipFn g_dip_orig[kMaxVariants];
void* g_dp_target[kMaxVariants];
void* g_dip_target[kMaxVariants];
int g_dp_variants = 0, g_dip_variants = 0;

HRESULT dp_impl(DpFn orig, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, DWORD f);
HRESULT dip_impl(DipFn orig, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, LPWORD idx, DWORD ni, DWORD f);

template <int I>
HRESULT STDMETHODCALLTYPE h_dp_v(Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, DWORD f) {
    return dp_impl(g_dp_orig[I], d, t, fvf, v, n, f);
}
template <int I>
HRESULT STDMETHODCALLTYPE h_dip_v(Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, LPWORD idx, DWORD ni,
                                  DWORD f) {
    return dip_impl(g_dip_orig[I], d, t, fvf, v, n, idx, ni, f);
}
constexpr DpFn kDpDetours[kMaxVariants] = {h_dp_v<0>, h_dp_v<1>, h_dp_v<2>, h_dp_v<3>,
                                           h_dp_v<4>, h_dp_v<5>, h_dp_v<6>, h_dp_v<7>};
constexpr DipFn kDipDetours[kMaxVariants] = {h_dip_v<0>, h_dip_v<1>, h_dip_v<2>, h_dip_v<3>,
                                             h_dip_v<4>, h_dip_v<5>, h_dip_v<6>, h_dip_v<7>};

template <class Fn>
void hook_variant(void* target, const Fn* detours, Fn* origs, void** targets, int& count, const char* what) {
    if (!target) return;
    for (int i = 0; i < count; ++i)
        if (targets[i] == target) return;
    for (const Fn* d = detours; d < detours + kMaxVariants; ++d)
        if (reinterpret_cast<void*>(*d) == target) return;  // the slot already points at our detour
    if (count == kMaxVariants) return;
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detours[count]), reinterpret_cast<void**>(&origs[count]));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        BWLOG("trace: hooking %s variant %p failed: %s", what, target, MH_StatusToString(st));
        return;
    }
    targets[count++] = target;
    BWLOG("trace: hooked %s implementation #%d at %p", what, count, target);
}

void track_draw_variants(Dev* dev) {
    if (!dev) return;
    hook_variant(reinterpret_cast<void*>(dev->lpVtbl->DrawPrimitive), kDpDetours, g_dp_orig, g_dp_target, g_dp_variants,
                 "DrawPrimitive");
    hook_variant(reinterpret_cast<void*>(dev->lpVtbl->DrawIndexedPrimitive), kDipDetours, g_dip_orig, g_dip_target,
                 g_dip_variants, "DrawIndexedPrimitive");
}

// One d3dim implementation can call another (both are hooked): only the outermost call is the
// engine's draw. Nested calls pass straight through -- clipping again would read g_vtx_scratch
// while rebuilding it.
int g_draw_depth = 0;
struct DrawDepth {
    DrawDepth() { ++g_draw_depth; }
    ~DrawDepth() { --g_draw_depth; }
};

// A mirrored mesh has its triangles wound the other way, so winding-based culling would keep
// its back faces. While it draws, D3D culls nothing (the stereo renderer also sets the mesh's
// double-sided flag, which turns off the engine's own software culling) and the z-buffer sorts
// the faces out.
bool g_no_cull = false;
// The mod's own text (labels by the hand): drawn whatever is nearer, still writing its depth, so a
// hand drawn before it doesn't cover it and one drawn after stays behind it (playtest 2026-10-31:
// "256 food" partly behind the fingers).
bool g_on_top = false;
struct OnTop {
    Dev* d = nullptr;
    DWORD old = 0;
    explicit OnTop(Dev* dev) {
        if (!g_on_top || FAILED(dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZFUNC, &old))) return;
        d = dev;
        o_SetRenderState(d, D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
    }
    ~OnTop() {
        if (d) o_SetRenderState(d, D3DRENDERSTATE_ZFUNC, old);
    }
};
struct FlipCull {
    Dev* d = nullptr;
    DWORD old = 0;
    explicit FlipCull(Dev* dev) {
        if (!g_no_cull || FAILED(dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_CULLMODE, &old))) return;
        if (old == D3DCULL_NONE) return;
        d = dev;
        o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    }
    ~FlipCull() {
        if (d) o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, old);
    }
};

// A faded draw (the god hand outside the player's influence, where it can do nothing): blended at
// g_ghost's opacity, its colour cooled through the second texture stage. 0 or 1: drawn as it is.
float g_ghost = 0;
struct Ghost {
    Dev* d = nullptr;
    DWORD blend = 0, src = 0, dst = 0, test = 0, tf = 0, op = 0, a1 = 0, a2 = 0, aop = 0, aa1 = 0, aa2 = 0;
    explicit Ghost(Dev* dev) {
        if (g_ghost <= 0 || g_ghost >= 1) return;
        d = dev;
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, &blend);
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_SRCBLEND, &src);
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_DESTBLEND, &dst);
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_ALPHATESTENABLE, &test);
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, &tf);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_COLOROP, &op);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_COLORARG1, &a1);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_COLORARG2, &a2);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_ALPHAOP, &aop);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_ALPHAARG1, &aa1);
        d->lpVtbl->GetTextureStageState(d, 1, D3DTSS_ALPHAARG2, &aa2);
        const DWORD alpha = DWORD(std::clamp(g_ghost, 0.0f, 1.0f) * 255.0f + 0.5f);
        o_SetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, (alpha << 24) | 0xA0B8E8);
        o_SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        o_SetRenderState(d, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        o_SetRenderState(d, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        o_SetRenderState(d, D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        o_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        o_SetTextureStageState(d, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        o_SetTextureStageState(d, 1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    }
    ~Ghost() {
        if (!d) return;
        o_SetTextureStageState(d, 1, D3DTSS_COLOROP, op);
        o_SetTextureStageState(d, 1, D3DTSS_COLORARG1, a1);
        o_SetTextureStageState(d, 1, D3DTSS_COLORARG2, a2);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, aop);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAARG1, aa1);
        o_SetTextureStageState(d, 1, D3DTSS_ALPHAARG2, aa2);
        o_SetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, tf);
        o_SetRenderState(d, D3DRENDERSTATE_ALPHATESTENABLE, test);
        o_SetRenderState(d, D3DRENDERSTATE_DESTBLEND, dst);
        o_SetRenderState(d, D3DRENDERSTATE_SRCBLEND, src);
        o_SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, blend);
    }
};

VertexFilter g_vertex_filter = nullptr;
extern bool g_in_eye_pass;
// Runs the eye-pass vertex filter; false when the draw is to be skipped.
bool filter_vertices(DWORD fvf, LPVOID& v, DWORD n) {
    if (!g_in_eye_pass || !g_vertex_filter || !v || !n) return true;
    const void* out = nullptr;
    switch (g_vertex_filter(fvf, v, n, fvf_stride(fvf), &out)) {
    case Filter::kSkip: return false;
    case Filter::kReplace: v = const_cast<void*>(out); break;
    default: break;
    }
    return true;
}

// ---------------- one engine pass for both eyes: record, then replay per eye ----------------
// The engine's eye pass is mostly its own CPU work (landscape, objects), and the second eye
// repeated all of it. With one_pass, the engine runs once with a camera between the eyes (the
// same symmetric frustum both eyes render with); its calls are recorded here instead of drawn
// (state calls also go to the device, so what the game reads back stays true), then replayed
// into each eye's target with every vertex moved to that eye: the vertices are pre-transformed
// (sx = cx + f X/Z, sy = cy - f Y/Z, rhw = near/Z, z = 1 - near/Z: rhw is 1 at the near plane,
// found by matching the two passes' pictures), so the point is known exactly.
enum class RecOp : uint8_t { kRS, kTSS, kTex, kXform, kViewport, kClear, kBegin, kEnd, kDIP, kDP };
struct RecCmd {
    RecOp op;
    bool no_cull = false;               // (a mirrored mesh: drawn without culling)
    bool on_top = false;                // (the mod's text: drawn over whatever is nearer)
    float ghost = 0;                    // (the god hand outside the influence: drawn faded, as Ghost)
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    DWORD a = 0, b = 0, c = 0;          // state/stage/value, or fvf/flags; Clear: count, flags, colour
    float zf = 0;                        // Clear's z
    void* ptr = nullptr;                 // a texture, or the draw implementation
    uint32_t vofs = 0, vcount = 0, iofs = 0, icount = 0, xofs = 0;  // into the arenas below
};
bool g_recording = false;
bool g_rec_bad = false;  // (something the replay can't do was met: logged)
Dev* g_rec_dev = nullptr;
std::vector<RecCmd> g_rec;
std::vector<BYTE> g_rec_vtx;
std::vector<WORD> g_rec_idx;
std::vector<D3DMATRIX> g_rec_mats;
std::vector<D3DVIEWPORT7> g_rec_vps;
std::vector<D3DRECT> g_rec_rects;
std::vector<BYTE> g_rep_vtx;  // (one draw's vertices, moved to the eye)
unsigned g_rec_draws = 0, g_rec_unsupported = 0;
bool g_plan_valid = false;  // (the replay's grouping: planned at the first eye's replay)
float g_rec_true_z = 0;  // (the mod's text drawn at a fake depth "in front of everything": its real depth)
bool recording() { return g_recording && g_in_eye_pass && g_draw_depth <= 1; }
void rec_state(RecOp op, DWORD a, DWORD b, DWORD c = 0, void* ptr = nullptr) {
    RecCmd r{op};
    r.a = a, r.b = b, r.c = c, r.ptr = ptr;
    g_rec.push_back(r);
}
// A draw: the vertices its indices use (rebased) and the indices; or all n vertices (DrawPrimitive).
void rec_draw(RecOp op, void* fn, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni, DWORD f) {
    const size_t stride = fvf_stride(fvf);
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || !v || !n) {  // (a transformed draw: the replay can't move it)
        ++g_rec_unsupported;
        return;
    }
    WORD lo = 0, hi = WORD(n - 1);
    if (idx && ni) {
        lo = 0xFFFF, hi = 0;
        for (DWORD k = 0; k < ni; ++k) lo = std::min(lo, idx[k]), hi = std::max(hi, idx[k]);
        if (hi >= n) return;  // (bad indices: as the device would, nothing)
    }
    RecCmd r{op};
    r.no_cull = g_no_cull;
    r.on_top = g_on_top;
    // (The fade was set around the live draw only, and the replay drew the hand solid: outside the
    // influence it no longer faded since one pass; playtest 2026-11-03.)
    r.ghost = g_ghost;
    r.zf = g_rec_true_z;  // (a draw's real depth when its vertices' is fake; 0: theirs)
    r.type = t, r.a = fvf, r.b = f, r.ptr = fn;
    r.vofs = uint32_t(g_rec_vtx.size()), r.vcount = uint32_t(hi - lo + 1);
    const BYTE* src = static_cast<const BYTE*>(v) + size_t(lo) * stride;
    g_rec_vtx.insert(g_rec_vtx.end(), src, src + size_t(r.vcount) * stride);
    if (idx && ni) {
        r.iofs = uint32_t(g_rec_idx.size()), r.icount = ni;
        g_rec_idx.resize(g_rec_idx.size() + ni);
        WORD* out = g_rec_idx.data() + r.iofs;
        for (DWORD k = 0; k < ni; ++k) out[k] = WORD(idx[k] - lo);
    }
    g_rec_dev = d;
    g_rec.push_back(r);
    ++g_rec_draws;
}

HRESULT dp_impl(DpFn o_DrawPrimitive, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, DWORD f) {
    if (g_draw_depth) return o_DrawPrimitive(d, t, fvf, v, n, f);
    DrawDepth depth;
    FlipCull flip(d);
    OnTop on_top(d);
    if (!filter_vertices(fvf, v, n)) return D3D_OK;
    if (skip_this_draw()) return D3D_OK;
    count_draw(fvf, t, n, false);
    if (g_cap) {
        fprintf(g_cap, "DP %s %s n=%lu", prim_name(t), fvf_desc(fvf).c_str(), n);
        cap_vertex0(fvf, v);
        cap_bbox(fvf, v, n, nullptr, 0);
        cap_hash(fvf, v, n);
        cap_stack();
    }
    note_draw(fvf, t);
    if (recording()) {
        rec_draw(RecOp::kDP, reinterpret_cast<void*>(o_DrawPrimitive), d, t, fvf, v, n, nullptr, 0, f);
        return D3D_OK;
    }
    return o_DrawPrimitive(d, t, fvf, v, n, f);
}
// --- Fix: near-plane clipping of pre-transformed triangles ---
// The engine sometimes emits XYZRHW triangles with a vertex on or behind the camera plane
// (rhw <= 0). They can't be rasterized correctly: at the game's 70 degree FOV the garbage falls
// off-screen, but at VR field of view it sweeps across the image. Big planes such as the sea
// keep a corner behind the eye most of the time, so dropping those triangles made the sea
// flicker as the head moved. Instead, rebuild camera-space positions from the screen position
// and 1/w (sx = cx + f*X/Z, sy = cy - f*Y/Z, rhw = 1/Z), clip each triangle against Z = zc,
// interpolate colours/fog/UVs, and re-project the new vertices with the engine's convention.
int g_near_clip = 1;  // [fix] near_clip: 1 = clip, 2 = drop (old behaviour, A/B), 3 = clip + tint new vertices red (debug), 0 = off
unsigned g_clipped_tris = 0;
unsigned g_bad_index_tris = 0;  // triangles dropped for referencing a vertex past the array
std::vector<WORD> g_idx_scratch;
std::vector<BYTE> g_vtx_scratch;

struct Proj {
    float cx, cy, f, zc;
};

Proj current_projection() {
    const auto& t = game::info_transform();
    // Clip a little before the engine's own near plane: only vertices it failed to clip are affected.
    return {t.half_res[0], t.half_res[1], t.half_res[0] * t.inv_half_tan_fov_y, t.near_clip * 0.5f};
}

struct Cam {
    float x, y, z;
};

Cam camera_space(const float* v, const Proj& p) {
    const float z = 1.0f / v[3];  // rhw = 1/Z; negative behind the eye
    return {(v[0] - p.cx) * z / p.f, (p.cy - v[1]) * z / p.f, z};
}

// (rhw = 1/Z: Z >= zc is rhw <= 1/zc, for rhw > 0; no division)
bool in_front(const float* v, float zc) { return v[3] > 0.0f && v[3] * zc <= 1.0f; }

// The clip plane for one triangle. World geometry uses a fixed plane just inside the engine's own
// near plane. The sky is a tiny dome centred on the eye (radius ~0.01 units), so a fixed plane
// would slice all of it away; for small triangles the plane scales with their distance (0.25 r
// is ~75 degrees off-axis, beyond the Index's field of view), which also keeps the projected
// coordinates of new vertices sane.
float triangle_zc(const float* v0, const float* v1, const float* v2, const Proj& p) {
    float r = 0;
    for (const float* v : {v0, v1, v2}) {
        const Cam c = camera_space(v, p);
        r = std::max(r, std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z));
    }
    return std::min(p.zc, 0.25f * r);
}

// Appends the vertex at fraction t from a to b (clipped exactly onto Z = zc); returns its index.
WORD add_clipped_vertex(DWORD fvf, size_t stride, const BYTE* a, const BYTE* b, const Proj& p, float zc) {
    const Cam ca = camera_space(reinterpret_cast<const float*>(a), p);
    const Cam cb = camera_space(reinterpret_cast<const float*>(b), p);
    const float t = (zc - ca.z) / (cb.z - ca.z);
    const size_t at = g_vtx_scratch.size();
    g_vtx_scratch.insert(g_vtx_scratch.end(), a, a + stride);
    BYTE* out = &g_vtx_scratch[at];
    const float x = ca.x + (cb.x - ca.x) * t, y = ca.y + (cb.y - ca.y) * t;
    float* pos = reinterpret_cast<float*>(out);
    pos[0] = p.cx + p.f * x / zc;
    pos[1] = p.cy - p.f * y / zc;
    pos[3] = 1.0f / zc;
    // World geometry uses z = 1 - 1/Z; the sky writes its own (far) z. Follow whichever convention
    // the vertex in front of the plane uses.
    const float* inside = reinterpret_cast<const float*>(ca.z >= zc ? a : b);
    pos[2] = std::fabs(inside[2] - (1.0f - inside[3])) < 1e-3f ? 1.0f - 1.0f / zc : inside[2];
    size_t off = 16;
    for (DWORD bit : {DWORD(D3DFVF_DIFFUSE), DWORD(D3DFVF_SPECULAR)}) {
        if (!(fvf & bit)) continue;
        for (int c = 0; c < 4; ++c)
            out[off + c] = BYTE(a[off + c] + (int(b[off + c]) - int(a[off + c])) * t + 0.5f);
        off += 4;
    }
    for (; off + 4 <= stride; off += 4) {
        const float fa = *reinterpret_cast<const float*>(a + off), fb = *reinterpret_cast<const float*>(b + off);
        *reinterpret_cast<float*>(out + off) = fa + (fb - fa) * t;
    }
    if (g_near_clip == 3 && (fvf & D3DFVF_DIFFUSE)) *reinterpret_cast<DWORD*>(out + 16) = 0xFFFF0000;  // debug tint
    return WORD(at / stride);
}

// Clips an indexed XYZRHW triangle list. Returns false (draw unchanged) if nothing needs clipping;
// otherwise the vertices/indices to draw are in g_vtx_scratch / g_idx_scratch.
bool g_vtx_copied = false;  // (clip_near: the vertices to draw are g_vtx_scratch, else the engine's own)
bool clip_near(DWORD fvf, const void* verts, DWORD nverts, const WORD* idx, DWORD ni) {
    if (!g_near_clip || !is_rhw(fvf) || (fvf & D3DFVF_NORMAL) || !verts || !idx) return false;
    const size_t stride = fvf_stride(fvf);
    const BYTE* base = static_cast<const BYTE*>(verts);
    const Proj p = current_projection();
    auto vert = [&](DWORD i) { return reinterpret_cast<const float*>(base + size_t(i) * stride); };
    // Each vertex once: in front of the plane or not. In VR many big draws (landscape blocks)
    // have a vertex behind the head, so this runs often: kept cheap.
    static std::vector<uint8_t> front;
    front.resize(nverts);
    bool any = false;
    for (DWORD i = 0; i < nverts; ++i) any |= !(front[i] = uint8_t(in_front(vert(i), p.zc)));
    // Some engine draws (the sea) carry index 0xFFFF for vertices the engine rejected. D3D reads
    // whatever lies past the vertex array for them and draws a flickering sheet across the sky.
    for (DWORD i = 0; i < ni && !any; ++i) any = idx[i] >= nverts;
    if (!any) return false;

    // The engine's vertices are copied only once a clipped vertex has to be added after them.
    g_vtx_copied = false;
    auto copy_vertices = [&] {
        if (!g_vtx_copied) g_vtx_scratch.assign(base, base + size_t(nverts) * stride), g_vtx_copied = true;
    };
    // (written in place and trimmed at the end; a clipped triangle can become two, so room for twice
    // as many as come in)
    g_idx_scratch.resize(size_t(ni) * 2 + 6);
    WORD* out = g_idx_scratch.data();
    size_t n_out = 0;
    for (DWORD i = 0; i + 2 < ni; i += 3) {
        const WORD tri[3] = {idx[i], idx[i + 1], idx[i + 2]};
        if (tri[0] >= nverts || tri[1] >= nverts || tri[2] >= nverts) {
            ++g_bad_index_tris;
            continue;
        }
        // The per-triangle plane is never further than p.zc, so triangles wholly in front of p.zc
        // need no further work (most of them).
        if (front[tri[0]] & front[tri[1]] & front[tri[2]]) {
            out[n_out] = tri[0], out[n_out + 1] = tri[1], out[n_out + 2] = tri[2];
            n_out += 3;
            continue;
        }
        const float zc = triangle_zc(vert(tri[0]), vert(tri[1]), vert(tri[2]), p);
        const bool inside[3] = {in_front(vert(tri[0]), zc), in_front(vert(tri[1]), zc), in_front(vert(tri[2]), zc)};
        if (inside[0] && inside[1] && inside[2]) {
            out[n_out] = tri[0], out[n_out + 1] = tri[1], out[n_out + 2] = tri[2];
            n_out += 3;
            continue;
        }
        ++g_clipped_tris;
        if (g_near_clip == 2 || (!inside[0] && !inside[1] && !inside[2])) continue;
        // Sutherland-Hodgman against Z >= zc; keeps the original winding.
        copy_vertices();
        WORD poly[4];
        int n = 0;
        for (int k = 0; k < 3; ++k) {
            const WORD a = tri[k], b = tri[(k + 1) % 3];
            if (inside[k]) poly[n++] = a;
            if (inside[k] != inside[(k + 1) % 3] && g_vtx_scratch.size() / stride < 0xFFFF)
                poly[n++] = add_clipped_vertex(fvf, stride, base + size_t(a) * stride, base + size_t(b) * stride, p, zc);
        }
        for (int k = 1; k + 1 < n; ++k) {
            out[n_out] = poly[0], out[n_out + 1] = poly[k], out[n_out + 2] = poly[k + 1];
            n_out += 3;
        }
    }
    g_idx_scratch.resize(n_out);
    return true;
}

// clip_near behind an SEH guard: a fault while reading the engine's vertices is logged with the
// draw's arguments and the draw goes through unclipped instead of taking the game down.
bool clip_near_seh(DWORD fvf, const void* verts, DWORD nverts, const WORD* idx, DWORD ni, bool* ok) {
    __try {
        *ok = true;
        return clip_near(fvf, verts, nverts, idx, ni);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return false;
    }
}
bool clip_near_guarded(DWORD fvf, const void* verts, DWORD nverts, const WORD* idx, DWORD ni) {
    bool ok = true;
    const bool r = clip_near_seh(fvf, verts, nverts, idx, ni, &ok);
    if (!ok) {
        static int logged = 0;
        if (logged++ < 8) {
            WORD hi = 0;
            for (DWORD i = 0; idx && i < ni; ++i) hi = std::max(hi, idx[i]);
            BWLOG("near clip: fault on draw fvf=%lX v=%p n=%lu idx=%p ni=%lu max index %u, scratch %zu bytes", fvf,
                  verts, nverts, idx, ni, hi, g_vtx_scratch.size());
        }
    }
    return r;
}


// --- Fix: the sea as world geometry in VR ---
// The engine draws the sea as a screen-space backdrop (fn_00879930): horizontal screen rows,
// unprojected onto the plane y = 0, with texture coordinates interpolated linearly along each row
// and a wave offset along the camera's look direction. That only holds for a camera that never
// rolls and never turns independently of the game: in the headset the texture swims (measured
// with tools/validate/sea_lock.py: ~90 px of slip for 4 degrees of roll, ~2-8 px for yaw).
// During eye passes the sea draw is replaced by a polar grid on y = 0 around the camera, with the
// engine's texture, scroll offsets, colour and distance fade, projected with the eye's camera.
bool g_sea_world_cfg = true;  // [fix] sea_world
// The sea is see-through: under it the game draws the sky and the islands mirrored, below the
// water, so in the headset the eyes see two depths at once (the waves on the surface and the
// reflected world beneath). The player found the water nauseating (2026-11-02); drawn solid, only
// the surface is seen. A VR option ([hands] sea_solid).
bool g_sea_solid = false;
bool g_in_eye_pass = false;   // set by the stereo renderer around each eye's 3D pass (declared above for the filter)
std::vector<BYTE> g_sea_vtx;
std::vector<WORD> g_sea_idx;
// The sea's triangles under the land, drawn solid even while the sea is see-through: the rivers are
// holes in the land's texture the sea shows through, and under a mountain stream the see-through
// sea showed the mirrored world beneath it ("the inside of the mountain"; player, 2026-11-02, who
// wants the open sea see-through and the rivers not). [fix] sea_solid_under_land.
std::vector<WORD> g_sea_idx_land;
bool g_sea_solid_under_land = true;
bool g_sea_main_solid = false;  // this frame's sea draw is all under the land
std::vector<uint8_t> g_sea_on_land;  // per grid vertex: the land is above the sea there
float land_altitude(float x, float z) {
    struct MapCoords { int32_t x, z; float altitude; } c{int32_t(x * 65536.0f / 10.0f), int32_t(z * 65536.0f / 10.0f), 0.0f};
    return reinterpret_cast<float(__fastcall*)(const MapCoords*)>(0x803090)(&c);
}
// Land or sea at (x, z), by 10-unit cells, each asked of the game once (the land doesn't move; the
// cache is dropped when the island's altitude at a fixed point changes, as at a new land).
bool on_land(float x, float z) {
    static std::vector<uint8_t> cells(512 * 512, 0);  // 0 unknown, 1 sea, 2 land
    static float probe = -1e9f;
    static int calls = 0;
    if ((++calls & 4095) == 0) {
        const float now = land_altitude(2560.0f, 2560.0f);
        if (now != probe) std::fill(cells.begin(), cells.end(), uint8_t(0)), probe = now;
    }
    const int cx = int(x / 10.0f), cz = int(z / 10.0f);
    if (cx < 0 || cz < 0 || cx >= 512 || cz >= 512) return false;
    uint8_t& c = cells[size_t(cx) * 512 + cz];
    if (!c) c = land_altitude(cx * 10.0f + 5.0f, cz * 10.0f + 5.0f) > 0.5f ? 2 : 1;
    return c == 2;
}

bool build_world_sea(DWORD fvf, const BYTE* engine_v) {
    constexpr DWORD kSeaFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
    if (fvf != kSeaFvf || !engine_v) return false;
    const auto& m = game::world_to_camera().m;
    const game::LHPoint cam = game::camera().pos;
    if (cam.y <= 0.01f) return false;  // camera at or below sea level: leave the engine's sea
    // Texture period and scroll, as fn_00879930 computes them (constants read from the binary).
    const float period = game::at<float>(0x8AB248) - game::at<float>(0xC38228) * game::at<float>(0x9A3C28);
    if (period <= 1.0f) return false;
    const float uv_scale = game::at<float>(0x8AA390) / period;
    const float scroll_u = game::at<float>(0xFA9370), scroll_v = game::at<float>(0xFA9374);
    const DWORD rgb = game::at<DWORD>(0xC398FC) & 0xFFFFFF;
    const float fade_far = game::at<float>(0xC39900), fade_near = game::at<float>(0xC39904);
    const int far_alpha = game::at<int>(0xC39908);
    const DWORD specular = *reinterpret_cast<const DWORD*>(engine_v + 20);
    const float zval = reinterpret_cast<const float*>(engine_v)[2];
    const Proj p = current_projection();
    const float near_z = game::info_transform().near_clip > 0 ? game::info_transform().near_clip : 1.0f;

    constexpr int kRings = 64, kSegs = 128;
    const float r0 = 0.25f * cam.y, r_max = 400000.0f;
    static float cos_t[kSegs], sin_t[kSegs];
    static bool tables = false;
    if (!tables) {
        for (int s = 0; s < kSegs; ++s) cos_t[s] = std::cos(6.2831853f * s / kSegs), sin_t[s] = std::sin(6.2831853f * s / kSegs);
        tables = true;
    }
    g_sea_vtx.resize(size_t(1 + kRings * kSegs) * 32);
    g_sea_on_land.assign(size_t(1 + kRings * kSegs), 0);
    const bool mark_land = g_sea_solid_under_land && !g_sea_solid;
    auto put = [&](int i, float wx, float wz, int alpha) {
        float c[3];
        for (int j = 0; j < 3; ++j) c[j] = wx * m[0][j] + wz * m[2][j] + m[3][j];
        if (std::fabs(c[2]) < 1e-4f) c[2] = c[2] < 0 ? -1e-4f : 1e-4f;
        float* f = reinterpret_cast<float*>(&g_sea_vtx[size_t(i) * 32]);
        f[0] = p.cx + p.f * c[0] / c[2];
        f[1] = p.cy - p.f * c[1] / c[2];
        // (The engine's pre-transformed convention: rhw = near/Z. The one-pass replay finds each
        // vertex's distance as near/rhw, so with 1/Z the sea was put at near (0.3-0.5) times its
        // distance in the eyes: in front of the coast it meets; player, 2026-11-02, nauseating there.)
        f[2] = zval;
        f[3] = near_z / c[2];
        const float dx = wx - cam.x, dz = wz - cam.z;
        const float dist = std::sqrt(dx * dx + dz * dz + cam.y * cam.y);
        if (alpha < 0) {
            float a = dist <= fade_near ? 255.0f
                      : dist >= fade_far ? float(far_alpha)
                      : 255.0f - (dist - fade_near) * (255.0f - far_alpha) / (fade_far - fade_near);
            alpha = int(a + 0.5f);
        }
        reinterpret_cast<DWORD*>(f)[4] = (DWORD(alpha) << 24) | rgb;
        reinterpret_cast<DWORD*>(f)[5] = specular;
        f[6] = (scroll_u + wx) * uv_scale;
        f[7] = (scroll_v + wz) * uv_scale;
        // (The land is 5120 units across: further out, open sea.)
        if (mark_land && wx > 0 && wz > 0 && wx < 5120.0f && wz < 5120.0f) g_sea_on_land[size_t(i)] = on_land(wx, wz);
    };
    put(0, cam.x, cam.z, -1);
    for (int k = 0; k < kRings; ++k) {
        const float r = r0 * std::pow(r_max / r0, float(k) / float(kRings - 1));
        for (int s = 0; s < kSegs; ++s)
            put(1 + k * kSegs + s, cam.x + r * cos_t[s], cam.z + r * sin_t[s], k == kRings - 1 ? 0x20 : -1);
    }
    // Emit only triangles that can reach the screen: not entirely behind the eye, and (when
    // entirely in front) not entirely off one side of the screen. The rest is most of the grid.
    const float w = 2.0f * p.cx, h = 2.0f * p.cy;
    auto vf = [&](WORD i) { return reinterpret_cast<const float*>(&g_sea_vtx[size_t(i) * 32]); };
    g_sea_idx.clear();
    g_sea_idx_land.clear();
    auto tri = [&](WORD a, WORD b, WORD c) {
        const float *A = vf(a), *B = vf(b), *C = vf(c);
        const int front = (A[3] > 0) + (B[3] > 0) + (C[3] > 0);
        if (!front) return;
        if (front == 3 && ((A[0] < 0 && B[0] < 0 && C[0] < 0) || (A[0] > w && B[0] > w && C[0] > w) ||
                           (A[1] < 0 && B[1] < 0 && C[1] < 0) || (A[1] > h && B[1] > h && C[1] > h)))
            return;
        if (g_sea_on_land[a] && g_sea_on_land[b] && g_sea_on_land[c]) g_sea_idx_land.insert(g_sea_idx_land.end(), {a, b, c});
        else g_sea_idx.insert(g_sea_idx.end(), {a, b, c});
    };
    auto ring = [](int k, int s) { return WORD(1 + k * kSegs + (s % kSegs)); };
    for (int s = 0; s < kSegs; ++s) {
        tri(0, ring(0, s), ring(0, s + 1));
        for (int k = 0; k + 1 < kRings; ++k) {
            tri(ring(k, s), ring(k + 1, s), ring(k, s + 1));
            tri(ring(k, s + 1), ring(k + 1, s), ring(k + 1, s + 1));
        }
    }
    // Keep only referenced vertices (the clipper copies the whole vertex array).
    static std::vector<int> remap;
    static std::vector<BYTE> packed;
    remap.assign(g_sea_vtx.size() / 32, -1);
    packed.resize(g_sea_vtx.size());
    size_t used = 0;
    for (auto* list : {&g_sea_idx, &g_sea_idx_land})
        for (WORD& i : *list) {
            if (remap[i] < 0) {
                remap[i] = int(used);
                memcpy(&packed[used * 32], &g_sea_vtx[size_t(i) * 32], 32);
                ++used;
            }
            i = WORD(remap[i]);
        }
    packed.resize(used * 32);
    g_sea_vtx.swap(packed);
    if (g_sea_idx.empty() && g_sea_idx_land.empty()) return false;
    return true;
}

// Debug bisection: skip draws [g_skip_from, g_skip_to) of every scene (BeginScene resets the count).
int g_skip_from = -1, g_skip_to = -1, g_draw_in_scene = 0;
bool g_sea_only = false;
// Debug: who asks for draws [g_log_from, g_log_to): the game's code addresses on the stack, once.
int g_log_from = -1, g_log_to = -1, g_log_left = 0;
void log_draw_callers(int i) {
    static uintptr_t lo = 0, hi = 0;
    if (!hi) {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        lo = uintptr_t(base), hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    const auto* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    std::string out;
    int found = 0;
    for (int k = 0; k < 600 && found < 16 && !IsBadReadPtr(sp + k, 4); ++k)
        if (sp[k] >= lo + 0x1000 && sp[k] < 0x8A0000) {
            char buf[16];
            snprintf(buf, sizeof(buf), " %06X", unsigned(sp[k]));
            out += buf;
            ++found;
        }
    BWLOG("trace: draw %d asked for by:%s", i, out.c_str());
}
bool skip_this_draw() {
    int i = g_draw_in_scene++;
    if (g_log_left > 0 && i >= g_log_from && i < g_log_to) --g_log_left, log_draw_callers(i);
    return i >= g_skip_from && i < g_skip_to;
}

// Engine-level submission counter: LH3DRender::DrawTriangle calls per frame, to compare with the
// D3D draws seen by the device hooks (anything that reaches the GPU around them shows up here).
unsigned g_engine_tris_calls = 0;
// __fastcall in the binary (verts in ecx, count in edx), despite the decomp's mangled name.
using DrawTriangleFn = void(__fastcall*)(void*, DWORD, WORD*, DWORD);
DrawTriangleFn o_draw_triangle = nullptr;
bool g_hud_unclamped = false;
void __fastcall draw_triangle_detour(void* verts, DWORD nverts, WORD* idx, DWORD ni) {
    ++g_engine_tris_calls;
    // A HUD model's screen shift (0xFA9398 on, by 0xFA939C/A0 pixels) without the engine's clamp to
    // the screen's edges (see set_hud_unclamped).
    auto* shift_on = reinterpret_cast<int*>(0xFA9398);
    bool shifted = false;
    if (g_hud_unclamped && *shift_on && verts) {
        const float dx = *reinterpret_cast<const float*>(0xFA939C), dy = *reinterpret_cast<const float*>(0xFA93A0);
        const float w = float(*reinterpret_cast<const int*>(0xE839E4)), h = float(*reinterpret_cast<const int*>(0xE839E8));
        float past = 0;  // (how far past the screen's edges the engine would have clamped it)
        for (DWORD i = 0; i < nverts; ++i) {
            float* v = reinterpret_cast<float*>(static_cast<char*>(verts) + size_t(i) * 0x20);  // (Vertex3D: x, y, ...)
            v[0] += dx, v[1] += dy;
            past = std::max({past, -v[0], v[0] - (w - 1), -v[1], v[1] - (h - 1)});
        }
        static int logged = 0;
        if (past > 0.5f && logged < 20) ++logged, BWLOG("ui: a gesture icon reached %.0f px past the screen's edge (%.0fx%.0f; the engine would have squashed it there)", past, w, h);
        *shift_on = 0, shifted = true;
    }
    // Catch a newly swapped-in DrawIndexedPrimitive implementation before this draw uses it.
    track_draw_variants(*reinterpret_cast<IDirect3DDevice7**>(sym::LH3DRender_Direct3DDevice7));
    if (g_cap) {
        // Which device and which DrawIndexedPrimitive implementation will this call reach?
        auto* dev = *reinterpret_cast<IDirect3DDevice7**>(sym::LH3DRender_Direct3DDevice7);
        void* fn = dev ? reinterpret_cast<void*>(dev->lpVtbl->DrawIndexedPrimitive) : nullptr;
        fprintf(g_cap, "DT v=%lu i=%lu dev=%p vtbl=%p dip=%p\n", nverts, ni, static_cast<void*>(dev),
                dev ? static_cast<void*>(dev->lpVtbl) : nullptr, fn);
    }
    o_draw_triangle(verts, nverts, idx, ni);
    if (shifted) *shift_on = 1;
}

// Debug: during a trace capture, dump every vertex and index of draw number g_dump_draw.
int g_dump_draw = -1;
IDirectDrawSurface7* g_last_tex0 = nullptr;  // texture bound to stage 0 (for dumping)
IDirectDrawSurface7* g_dumped_tex = nullptr;
void dump_draw(DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni) {
    if (!g_cap || g_dump_draw != g_draw_in_scene - 1 || !is_rhw(fvf)) return;
    // Save the stage-0 texture too, as captures\dumptex_<ptr>.bmp (once per texture).
    if (g_last_tex0 && g_last_tex0 != g_dumped_tex) {
        wchar_t name[64];
        static int dump_seq = 0;
        swprintf(name, 64, L"captures\\dumptex_%p_%d.bmp", static_cast<void*>(g_last_tex0), dump_seq++);
        fprintf(g_cap, "DUMPTEX %p %s\n", static_cast<void*>(g_last_tex0),
                screenshot::save_surface(g_last_tex0, paths::bwvr_dir() + name) ? "saved" : "FAILED");
        g_dumped_tex = g_last_tex0;
    }
    const size_t stride = fvf_stride(fvf);
    fprintf(g_cap, "DUMP v=%lu i=%lu stride=%u\n", n, ni, unsigned(stride));
    for (DWORD k = 0; k < n; ++k) {
        const float* p = reinterpret_cast<const float*>(static_cast<const BYTE*>(v) + k * stride);
        const DWORD* c = reinterpret_cast<const DWORD*>(p + 4);
        fprintf(g_cap, "V %lu %.6g %.6g %.6g %.6g %08lX %08lX", k, p[0], p[1], p[2], p[3], c[0], c[1]);
        if (stride >= 32) fprintf(g_cap, " %.5g %.5g", p[6], p[7]);
        fputc('\n', g_cap);
    }
    fputs("I", g_cap);
    for (DWORD k = 0; k < ni; ++k) fprintf(g_cap, " %u", idx[k]);
    fputc('\n', g_cap);
}

// Debug: draws [g_hl_from, g_hl_to) of each scene are drawn flat semi-transparent red, to see
// visually which draw produces what (unlike skipping, this leaves all other state untouched).
int g_hl_from = -1, g_hl_to = -1;

HRESULT draw_highlighted(DipFn o_DrawIndexedPrimitive, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n,
                         LPWORD idx, DWORD ni, DWORD f) {
    DWORD op = 0, a1 = 0, aop = 0, aa1 = 0, tf = 0;
    d->lpVtbl->GetTextureStageState(d, 0, D3DTSS_COLOROP, &op);
    d->lpVtbl->GetTextureStageState(d, 0, D3DTSS_COLORARG1, &a1);
    d->lpVtbl->GetTextureStageState(d, 0, D3DTSS_ALPHAOP, &aop);
    d->lpVtbl->GetTextureStageState(d, 0, D3DTSS_ALPHAARG1, &aa1);
    d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, &tf);
    o_SetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, 0xC0FF0000);
    o_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    o_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    o_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    o_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    HRESULT hr = o_DrawIndexedPrimitive(d, t, fvf, v, n, idx, ni, f);
    o_SetTextureStageState(d, 0, D3DTSS_COLOROP, op);
    o_SetTextureStageState(d, 0, D3DTSS_COLORARG1, a1);
    o_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, aop);
    o_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, aa1);
    o_SetRenderState(d, D3DRENDERSTATE_TEXTUREFACTOR, tf);
    return hr;
}

HRESULT dip_impl(DipFn o_DrawIndexedPrimitive, Dev* d, D3DPRIMITIVETYPE t, DWORD fvf, LPVOID v, DWORD n, LPWORD idx,
                 DWORD ni, DWORD f) {
    if (g_draw_depth) return o_DrawIndexedPrimitive(d, t, fvf, v, n, idx, ni, f);
    DrawDepth depth;
    FlipCull flip(d);
    OnTop on_top(d);
    Ghost ghost(d);
    // (Not the sea: its screen-space strips have the 2D interface's z = 0, rhw = 1 signature.)
    if (v != *reinterpret_cast<void**>(game::kSeaVerts) && !filter_vertices(fvf, v, n)) return D3D_OK;
    const int draw_index = g_draw_in_scene;
    if (skip_this_draw()) return D3D_OK;
    const bool is_sea = v == *reinterpret_cast<void**>(game::kSeaVerts);
    if (g_sea_only && !is_sea) return D3D_OK;
    bool world_sea = false;
    if (is_sea && g_sea_world_cfg && g_in_eye_pass && t == D3DPT_TRIANGLELIST &&
        build_world_sea(fvf, static_cast<const BYTE*>(v))) {
        world_sea = true;
        // (All of it under the land: drawn as the main part, solid.)
        if (g_sea_idx.empty()) g_sea_idx.swap(g_sea_idx_land), g_sea_main_solid = true;
        else g_sea_main_solid = false;
        v = g_sea_vtx.data();
        n = DWORD(g_sea_vtx.size() / 32);
        idx = g_sea_idx.data();
        ni = DWORD(g_sea_idx.size());
    }
    dump_draw(fvf, v, n, idx, ni);
    const bool clipped = t == D3DPT_TRIANGLELIST && clip_near_guarded(fvf, v, n, idx, ni);
    if (clipped) {
        if (g_idx_scratch.empty()) return D3D_OK;
        if (g_vtx_copied) {
            v = g_vtx_scratch.data();
            n = DWORD(g_vtx_scratch.size() / fvf_stride(fvf));
        }
        idx = g_idx_scratch.data();
        ni = DWORD(g_idx_scratch.size());
    }
    count_draw(fvf, t, ni, false);
    if (g_cap) {
        fprintf(g_cap, "DIP %s %s v=%lu i=%lu", prim_name(t), fvf_desc(fvf).c_str(), n, ni);
        cap_vertex0(fvf, v);
        cap_bbox(fvf, v, n, idx, ni);
        cap_hash(fvf, v, n);
        cap_stack();
    }
    if (draw_index >= g_hl_from && draw_index < g_hl_to)
        return draw_highlighted(o_DrawIndexedPrimitive, d, t, fvf, v, n, idx, ni, f);
    if (world_sea) {  // the grid's winding isn't the engine's; draw it double-sided
        DWORD cull = D3DCULL_CCW;
        d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_CULLMODE, &cull);
        d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        DWORD blend = FALSE;
        const bool main_solid = g_sea_solid || g_sea_main_solid;
        if (main_solid) {
            d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, &blend);
            d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        }
        HRESULT hr = D3D_OK;
        if (recording()) rec_draw(RecOp::kDIP, reinterpret_cast<void*>(o_DrawIndexedPrimitive), d, t, fvf, v, n, idx, ni, f);
        else hr = o_DrawIndexedPrimitive(d, t, fvf, v, n, idx, ni, f);
        if (main_solid) d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, blend);
        if (!g_sea_idx_land.empty()) {  // (under the land: solid)
            const WORD* li = g_sea_idx_land.data();
            DWORD lni = DWORD(g_sea_idx_land.size()), ln = DWORD(g_sea_vtx.size() / 32);
            const void* lv = g_sea_vtx.data();
            bool ok = true;
            if (clip_near_guarded(fvf, lv, ln, li, lni)) {
                ok = !g_idx_scratch.empty();
                if (g_vtx_copied) lv = g_vtx_scratch.data(), ln = DWORD(g_vtx_scratch.size() / fvf_stride(fvf));
                li = g_idx_scratch.data(), lni = DWORD(g_idx_scratch.size());
            }
            if (ok) {
                DWORD b2 = FALSE;
                d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, &b2);
                d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
                if (recording()) rec_draw(RecOp::kDIP, reinterpret_cast<void*>(o_DrawIndexedPrimitive), d, t, fvf, const_cast<void*>(lv), ln, const_cast<WORD*>(li), lni, f);
                else o_DrawIndexedPrimitive(d, t, fvf, const_cast<void*>(lv), ln, const_cast<WORD*>(li), lni, f);
                d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_ALPHABLENDENABLE, b2);
            }
        }
        d->lpVtbl->SetRenderState(d, D3DRENDERSTATE_CULLMODE, cull);
        state_changed();
        return hr;
    }
    note_draw(fvf, t);
    if (recording()) {
        rec_draw(RecOp::kDIP, reinterpret_cast<void*>(o_DrawIndexedPrimitive), d, t, fvf, v, n, idx, ni, f);
        return D3D_OK;
    }
    return o_DrawIndexedPrimitive(d, t, fvf, v, n, idx, ni, f);
}
HRESULT STDMETHODCALLTYPE h_DrawPrimitiveStrided(Dev* d, D3DPRIMITIVETYPE t, DWORD fvf,
                                                 LPD3DDRAWPRIMITIVESTRIDEDDATA s, DWORD n, DWORD f) {
    if (recording()) ++g_rec_unsupported;
    count_draw(fvf, t, n, false);
    if (g_cap) {
        fprintf(g_cap, "DPS %s %s n=%lu", prim_name(t), fvf_desc(fvf).c_str(), n);
        cap_stack();
    }
    return o_DrawPrimitiveStrided(d, t, fvf, s, n, f);
}
HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitiveStrided(Dev* d, D3DPRIMITIVETYPE t, DWORD fvf,
                                                        LPD3DDRAWPRIMITIVESTRIDEDDATA s, DWORD n, LPWORD idx,
                                                        DWORD ni, DWORD f) {
    if (recording()) ++g_rec_unsupported;
    count_draw(fvf, t, ni, false);
    if (g_cap) {
        fprintf(g_cap, "DIPS %s %s v=%lu i=%lu", prim_name(t), fvf_desc(fvf).c_str(), n, ni);
        cap_stack();
    }
    return o_DrawIndexedPrimitiveStrided(d, t, fvf, s, n, idx, ni, f);
}
HRESULT STDMETHODCALLTYPE h_DrawPrimitiveVB(Dev* d, D3DPRIMITIVETYPE t, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start,
                                            DWORD n, DWORD f) {
    if (recording()) ++g_rec_unsupported;
    DWORD fvf = vb_fvf(vb);
    count_draw(fvf, t, n, true);
    if (g_cap) {
        fprintf(g_cap, "DPVB %s %s vb=%p start=%lu n=%lu", prim_name(t), fvf_desc(fvf).c_str(), vb, start, n);
        cap_stack();
    }
    return o_DrawPrimitiveVB(d, t, vb, start, n, f);
}
HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitiveVB(Dev* d, D3DPRIMITIVETYPE t, LPDIRECT3DVERTEXBUFFER7 vb,
                                                   DWORD start, DWORD n, LPWORD idx, DWORD ni, DWORD f) {
    if (recording()) ++g_rec_unsupported;
    DWORD fvf = vb_fvf(vb);
    count_draw(fvf, t, ni, true);
    if (g_cap) {
        fprintf(g_cap, "DIPVB %s %s vb=%p start=%lu v=%lu i=%lu", prim_name(t), fvf_desc(fvf).c_str(), vb, start,
                n, ni);
        cap_stack();
    }
    return o_DrawIndexedPrimitiveVB(d, t, vb, start, n, idx, ni, f);
}

// --- surface writes (texture updates) ---
using Surf = IDirectDrawSurface7;
HRESULT(STDMETHODCALLTYPE* o_Lock)(Surf*, LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE);
HRESULT(STDMETHODCALLTYPE* o_Unlock)(Surf*, LPRECT);
HRESULT(STDMETHODCALLTYPE* o_Restore)(Surf*);
HRESULT(STDMETHODCALLTYPE* o_Blt)(Surf*, LPRECT, Surf*, LPRECT, DWORD, LPDDBLTFX);
HRESULT(STDMETHODCALLTYPE* o_BltFast)(Surf*, DWORD, DWORD, Surf*, LPRECT, DWORD);
HRESULT(STDMETHODCALLTYPE* o_Load)(Dev*, LPDIRECTDRAWSURFACE7, LPPOINT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);

HRESULT STDMETHODCALLTYPE h_Lock(Surf* s, LPRECT r, LPDDSURFACEDESC2 d, DWORD f, HANDLE h) {
    if (g_cap && !(f & DDLOCK_READONLY)) {
        fprintf(g_cap, "Lock %p flags=%lX", s, f);
        cap_stack();
    }
    const HRESULT hr = o_Lock(s, r, d, f, h);
    if (SUCCEEDED(hr)) mipmaps::after_lock(s, r, f, d);
    return hr;
}
HRESULT STDMETHODCALLTYPE h_Restore(Surf* s) {
    const HRESULT hr = o_Restore(s);
    if (SUCCEEDED(hr)) mipmaps::after_restore(s);
    return hr;
}
HRESULT STDMETHODCALLTYPE h_Unlock(Surf* s, LPRECT r) {
    mipmaps::before_unlock(s);
    const HRESULT hr = o_Unlock(s, r);
    if (SUCCEEDED(hr)) mipmaps::after_unlock(s);
    return hr;
}
HRESULT STDMETHODCALLTYPE h_Blt(Surf* s, LPRECT dr, Surf* src, LPRECT sr, DWORD f, LPDDBLTFX fx) {
    if (g_cap) {
        fprintf(g_cap, "Blt %p <- %p flags=%lX", s, src, f);
        cap_stack();
    }
    const HRESULT hr = o_Blt(s, dr, src, sr, f, fx);
    if (SUCCEEDED(hr)) mipmaps::after_blt(s, dr, src, sr);
    return hr;
}
HRESULT STDMETHODCALLTYPE h_BltFast(Surf* s, DWORD x, DWORD y, Surf* src, LPRECT sr, DWORD f) {
    if (g_cap) {
        fprintf(g_cap, "BltFast %p <- %p at %lu,%lu", s, src, x, y);
        cap_stack();
    }
    const HRESULT hr = o_BltFast(s, x, y, src, sr, f);
    if (SUCCEEDED(hr)) mipmaps::after_blt_fast(s, x, y, src, sr);
    return hr;
}
HRESULT STDMETHODCALLTYPE h_Load(Dev* d, LPDIRECTDRAWSURFACE7 dst, LPPOINT p, LPDIRECTDRAWSURFACE7 src, LPRECT r,
                                 DWORD f) {
    if (g_cap) {
        fprintf(g_cap, "Load %p <- %p", dst, src);
        cap_stack();
    }
    const HRESULT hr = o_Load(d, dst, p, src, r, f);
    if (SUCCEEDED(hr)) mipmaps::after_load(dst, p, src, r);
    return hr;
}

// Detour the functions themselves rather than the vtable slots: the engine caches some method
// pointers before we get a chance to patch the table, so slot patching misses its draw calls.
template <class Fn>
void patch(void** slot, Fn detour, Fn* orig) {
    void* target = *slot;
    if (target == reinterpret_cast<void*>(detour) || *orig) return;
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(orig));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) BWLOG("trace: hooking %p failed: %s", target, MH_StatusToString(st));
}

IDirect3DDevice7Vtbl* g_hooked_vtbl = nullptr;

}  // namespace

void configure() {
    g_stack_depth = paths::ini_int(L"trace", L"stack_depth", 6);
    if (g_stack_depth < 1) g_stack_depth = 1;
    if (g_stack_depth > 16) g_stack_depth = 16;
    g_near_clip = paths::ini_int(L"fix", L"near_clip", 1);
    g_sea_world_cfg = paths::ini_int(L"fix", L"sea_world", 1) != 0;
    g_sea_solid_under_land = paths::ini_int(L"fix", L"sea_solid_under_land", 1) != 0;
    int at_start = paths::ini_int(L"trace", L"capture_frames_at_device", 0);
    if (at_start > 0) g_cap_pending = at_start;
}

void hook_surface_methods(void* surface) {
    if (o_Lock && o_Unlock && o_Blt && o_BltFast && o_Restore) return;
    IDirectDrawSurface7Vtbl* sv = static_cast<IDirectDrawSurface7*>(surface)->lpVtbl;
    patch(reinterpret_cast<void**>(&sv->Lock), &h_Lock, &o_Lock);
    patch(reinterpret_cast<void**>(&sv->Unlock), &h_Unlock, &o_Unlock);
    patch(reinterpret_cast<void**>(&sv->Restore), &h_Restore, &o_Restore);
    patch(reinterpret_cast<void**>(&sv->Blt), &h_Blt, &o_Blt);
    patch(reinterpret_cast<void**>(&sv->BltFast), &h_BltFast, &o_BltFast);
}

void ensure_hooked(void* device) {
    auto* dev = static_cast<Dev*>(device);
    IDirect3DDevice7Vtbl* vt = dev->lpVtbl;
    track_draw_variants(dev);  // the draw slots change at runtime; catch every implementation
    if (vt == g_hooked_vtbl) return;
    if (g_hooked_vtbl) BWLOG("trace: device vtable changed %p -> %p, re-patching", g_hooked_vtbl, vt);
    patch(reinterpret_cast<void**>(&vt->Load), &h_Load, &o_Load);
    // All DirectDraw surfaces share one implementation; hook it through the render target's vtable.
    IDirectDrawSurface7* rt = nullptr;
    if (SUCCEEDED(vt->GetRenderTarget(dev, &rt)) && rt) {
        hook_surface_methods(rt);
        rt->lpVtbl->Release(rt);
    }
    patch(reinterpret_cast<void**>(&vt->BeginScene), &h_BeginScene, &o_BeginScene);
    patch(reinterpret_cast<void**>(&vt->EndScene), &h_EndScene, &o_EndScene);
    patch(reinterpret_cast<void**>(&vt->SetRenderTarget), &h_SetRenderTarget, &o_SetRenderTarget);
    patch(reinterpret_cast<void**>(&vt->Clear), &h_Clear, &o_Clear);
    patch(reinterpret_cast<void**>(&vt->SetTransform), &h_SetTransform, &o_SetTransform);
    patch(reinterpret_cast<void**>(&vt->SetViewport), &h_SetViewport, &o_SetViewport);
    patch(reinterpret_cast<void**>(&vt->SetRenderState), &h_SetRenderState, &o_SetRenderState);
    patch(reinterpret_cast<void**>(&vt->DrawPrimitiveStrided), &h_DrawPrimitiveStrided, &o_DrawPrimitiveStrided);
    patch(reinterpret_cast<void**>(&vt->DrawIndexedPrimitiveStrided), &h_DrawIndexedPrimitiveStrided,
          &o_DrawIndexedPrimitiveStrided);
    patch(reinterpret_cast<void**>(&vt->DrawPrimitiveVB), &h_DrawPrimitiveVB, &o_DrawPrimitiveVB);
    patch(reinterpret_cast<void**>(&vt->DrawIndexedPrimitiveVB), &h_DrawIndexedPrimitiveVB, &o_DrawIndexedPrimitiveVB);
    patch(reinterpret_cast<void**>(&vt->SetTexture), &h_SetTexture, &o_SetTexture);
    patch(reinterpret_cast<void**>(&vt->SetTextureStageState), &h_SetTextureStageState, &o_SetTextureStageState);
    g_hooked_vtbl = vt;
    BWLOG("trace: hooked IDirect3DDevice7 %p (vtable %p)", dev, vt);
    if (!o_draw_triangle) {
        // LH3DRender::DrawTriangle(Vertex3D*, ulong, ushort*, ulong): the engine's submission point.
        void* target = reinterpret_cast<void*>(sym::LH3DRender_DrawTriangle);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&draw_triangle_detour),
                          reinterpret_cast<void**>(&o_draw_triangle)) == MH_OK)
            MH_EnableHook(target);
    }
}

void mark(const char* label) {
    if (g_cap) fprintf(g_cap, "MARK %s\n", label);
}

void set_highlight(int from, int to) {
    g_hl_from = from;
    g_hl_to = to;
}

void set_dump_draw(int index) {
    g_dump_draw = index;
    g_dumped_tex = nullptr;  // allow the texture to be dumped again
}

void set_near_clip(int mode) {
    g_near_clip = mode;
    BWLOG("trace: near_clip mode %d", mode);
}

void set_log_draws(int from, int to) {
    g_log_from = from, g_log_to = to, g_log_left = std::max(0, to - from) * 2;
    BWLOG("trace: logging who asks for draws [%d, %d)", from, to);
}
void set_skip(int from, int to) {
    g_skip_from = from;
    g_skip_to = to;
    BWLOG("trace: skipping draws [%d, %d) of each scene", from, to);
}

void set_hud_unclamped(bool on) { g_hud_unclamped = on; }

namespace {
void rec_state_fwd(int op, DWORD a, DWORD b, DWORD c, void* ptr) {
    if (op < 0) {
        g_rec_bad = true;
        return;
    }
    rec_state(RecOp(op), a, b, c, ptr);
}
void rec_misc(int op, const void* data) {
    RecCmd r{RecOp(op)};
    if (RecOp(op) == RecOp::kXform) {
        const auto* x = static_cast<const XformArgs*>(data);
        r.a = x->t, r.xofs = uint32_t(g_rec_mats.size());
        g_rec_mats.push_back(*x->m);
    } else if (RecOp(op) == RecOp::kViewport) {
        r.xofs = uint32_t(g_rec_vps.size());
        g_rec_vps.push_back(*static_cast<const D3DVIEWPORT7*>(data));
    } else if (RecOp(op) == RecOp::kClear) {
        const auto* x = static_cast<const ClearArgs*>(data);
        r.a = x->r ? x->n : 0, r.b = x->flags, r.c = x->c, r.zf = x->z, r.vcount = x->st;
        r.xofs = uint32_t(g_rec_rects.size());
        for (DWORD i = 0; x->r && i < x->n; ++i) g_rec_rects.push_back(x->r[i]);
    }
    g_rec.push_back(r);
}

}  // namespace

void set_record_depth(float z) { g_rec_true_z = z; }
void begin_record() {
    g_rec.clear(), g_rec_vtx.clear(), g_rec_idx.clear(), g_rec_mats.clear(), g_rec_vps.clear(), g_rec_rects.clear();
    g_rec_bad = false, g_rec_draws = 0, g_rec_unsupported = 0, g_rec_dev = nullptr;
    g_plan_valid = false;
    g_recording = true;
}
void end_record() { g_recording = false; }
bool record_ok() { return !g_rec_bad; }

// One eye: the recorded calls on the device, every vertex moved by the eye's offset from the
// recording camera (d: right, up, forward in world units) and drawn with the same focal length
// and centre; near: the engine's near plane in the pass. A vertex with z = 0, rhw = 1 (the 2D
// interface's) stays where it is.
// The replay's draws grouped by texture (planned once a frame, at the first eye's replay; both
// eyes use the plan). The engine binds a texture before nearly every draw (the landscape: each
// block once per texture on it), and each draw costs ~1.5 us in dxwrapper, Direct3D 9 and the
// driver. A window is a run of the recording where only stage 0's texture changes (a state set
// to the value it has doesn't count); in it a draw joins its texture's last group unless a later
// group overlaps it on the screen (it would then be drawn before that one: most of these draws
// are blended), else it starts a new group. Screen boxes are widened by the most either eye moves
// the draw. The groups are drawn when the window ends, each as one draw. (stereo replay_group 0|1.)
bool g_group = true;
struct PlanItem {
    int32_t cmd = -1;    // a recorded command, done as it is; or
    int32_t group = -1;  // a group drawn; or
    void* tex0 = nullptr;  // (cmd and group -1: stage 0 bound to this, as the recording left it)
};
struct PlanGroup {
    void* tex = nullptr;
    std::vector<uint32_t> draws;  // (indices into g_rec)
    uint32_t verts = 0, idx = 0;
    float x0, y0, x1, y1;
};
std::vector<PlanItem> g_plan;
std::vector<PlanGroup> g_groups;
unsigned g_plan_in = 0, g_plan_out = 0;  // (draws grouped, draws made, last plan)

void build_plan(float f, float adx, float ady, float near_z) {
    g_plan.clear(), g_groups.clear();
    g_plan_in = g_plan_out = 0;
    static DWORD rs[256], tss[8][32];
    static bool rs_known[256], tss_known[8][32];
    memset(rs_known, 0, sizeof(rs_known)), memset(tss_known, 0, sizeof(tss_known));
    void* tex0 = nullptr;
    bool open = false;
    size_t first = 0;  // (the window's first group)
    const RecCmd* key = nullptr;
    const auto close = [&] {
        if (!open) return;
        for (size_t gi = first; gi < g_groups.size(); ++gi) g_plan.push_back({-1, int32_t(gi), nullptr});
        g_plan.push_back({-1, -1, tex0});
        open = false;
    };
    const float mx = f * adx / near_z, my = f * ady / near_z;  // (the most a vertex moves, per unit of rhw)
    for (size_t i = 0; i < g_rec.size(); ++i) {
        const RecCmd& r = g_rec[i];
        switch (r.op) {
        case RecOp::kRS:
            if (r.a < 256 && rs_known[r.a] && rs[r.a] == r.b) { g_plan.push_back({int32_t(i)}); break; }
            close();
            if (r.a < 256) rs[r.a] = r.b, rs_known[r.a] = true;
            g_plan.push_back({int32_t(i)});
            break;
        case RecOp::kTSS:
            if (r.a < 8 && r.b < 32 && tss_known[r.a][r.b] && tss[r.a][r.b] == r.c) { g_plan.push_back({int32_t(i)}); break; }
            close();
            if (r.a < 8 && r.b < 32) tss[r.a][r.b] = r.c, tss_known[r.a][r.b] = true;
            g_plan.push_back({int32_t(i)});
            break;
        case RecOp::kTex:
            if (r.a == 0) {
                tex0 = r.ptr;
                if (!open) g_plan.push_back({int32_t(i)});
                break;
            }
            close();
            g_plan.push_back({int32_t(i)});
            break;
        case RecOp::kDIP: {
            const bool ok = r.type == D3DPT_TRIANGLELIST && r.zf == 0 && r.icount > 0 && r.ghost == 0;
            if (!ok) {
                close();
                g_plan.push_back({int32_t(i)});
                break;
            }
            if (open && (key->a != r.a || key->b != r.b || key->ptr != r.ptr || key->no_cull != r.no_cull || key->on_top != r.on_top)) close();
            if (!open) open = true, first = g_groups.size(), key = &r;
            // The draw's screen box, widened by the most an eye moves it.
            const size_t stride = fvf_stride(r.a);
            const BYTE* v = g_rec_vtx.data() + r.vofs;
            const WORD* ix = g_rec_idx.data() + r.iofs;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, wmax = 0;
            for (uint32_t k = 0; k < r.icount; ++k) {
                const float* q = reinterpret_cast<const float*>(v + size_t(ix[k]) * stride);
                x0 = std::min(x0, q[0]), x1 = std::max(x1, q[0]), y0 = std::min(y0, q[1]), y1 = std::max(y1, q[1]);
                wmax = std::max(wmax, q[3]);
            }
            const float ex = mx * wmax + 1.0f, ey = my * wmax + 1.0f;
            x0 -= ex, x1 += ex, y0 -= ey, y1 += ey;
            PlanGroup* g = nullptr;
            for (size_t gi = g_groups.size(); gi-- > first;) {
                PlanGroup& c = g_groups[gi];
                if (c.tex == tex0) {
                    if (c.verts + r.vcount <= 65535) g = &c;
                    break;
                }
                if (!(c.x1 < x0 || x1 < c.x0 || c.y1 < y0 || y1 < c.y0)) break;  // (a later group overlaps it)
            }
            if (!g) {
                g_groups.push_back({tex0, {}, 0, 0, x0, y0, x1, y1});
                g = &g_groups.back();
            } else {
                g->x0 = std::min(g->x0, x0), g->y0 = std::min(g->y0, y0), g->x1 = std::max(g->x1, x1), g->y1 = std::max(g->y1, y1);
            }
            g->draws.push_back(uint32_t(i));
            g->verts += r.vcount, g->idx += r.icount;
            ++g_plan_in;
            break;
        }
        default:  // (other draws, clears, scenes, viewports, transforms: in order, between windows)
            close();
            g_plan.push_back({int32_t(i)});
            break;
        }
    }
    close();
    g_plan_out = unsigned(g_groups.size());
    g_plan_valid = true;
}

// Copies a recorded draw's vertices to dst, moved to the eye.
void move_vertices(const RecCmd& r, BYTE* dst, float f, float cx, float cy, float dx, float dy, float dz, float near_z) {
    const size_t stride = fvf_stride(r.a);
    memcpy(dst, g_rec_vtx.data() + r.vofs, size_t(r.vcount) * stride);
    const float kx = f * dx / near_z, ky = f * dy / near_z;
    for (uint32_t i = 0; i < r.vcount; ++i) {
        float* p = reinterpret_cast<float*>(dst + i * stride);
        if (r.zf > 0) {  // (moved by its real depth; its own z and rhw kept)
            p[0] -= f * dx / r.zf;
            p[1] += f * dy / r.zf;
            continue;
        }
        const float rhw = p[3];
        if (rhw <= 0 || (p[2] == 0.0f && rhw == 1.0f)) continue;
        if (dz == 0.0f) {
            p[0] -= kx * rhw;
            p[1] += ky * rhw;
        } else {
            const float Z = near_z / rhw, Ze = Z - dz;
            if (Ze <= 0) continue;
            const float X = (p[0] - cx) * Z / f - dx, Y = (cy - p[1]) * Z / f - dy, w = near_z / Ze;
            p[0] = cx + f * X * w;
            p[1] = cy - f * Y * w;
            p[2] = 1.0f - (1.0f - p[2]) * w / rhw;
            p[3] = w;
        }
    }
}
std::vector<WORD> g_rep_idx;
void replay_cmd(Dev* d, const RecCmd& r, float f, float cx, float cy, float dx, float dy, float dz, float near_z) {
    switch (r.op) {
    case RecOp::kRS: o_SetRenderState(d, D3DRENDERSTATETYPE(r.a), r.b); break;
    case RecOp::kTSS: o_SetTextureStageState(d, r.a, D3DTEXTURESTAGESTATETYPE(r.b), r.c); break;
    case RecOp::kTex: o_SetTexture(d, r.a, static_cast<LPDIRECTDRAWSURFACE7>(r.ptr)); break;
    case RecOp::kXform: o_SetTransform(d, D3DTRANSFORMSTATETYPE(r.a), &g_rec_mats[r.xofs]); break;
    case RecOp::kViewport: o_SetViewport(d, &g_rec_vps[r.xofs]); break;  // (both eyes render the same geometry)
    case RecOp::kClear: o_Clear(d, r.a, r.a ? &g_rec_rects[r.xofs] : nullptr, r.b, r.c, r.zf, r.vcount); break;
    case RecOp::kBegin: o_BeginScene(d); break;
    case RecOp::kEnd: o_EndScene(d); break;
    case RecOp::kDIP:
    case RecOp::kDP: {
        const size_t bytes = size_t(r.vcount) * fvf_stride(r.a);
        if (g_rep_vtx.size() < bytes) g_rep_vtx.resize(bytes);
        BYTE* dst = g_rep_vtx.data();
        move_vertices(r, dst, f, cx, cy, dx, dy, dz, near_z);
        DWORD old = 0, old_z = 0;
        if (r.no_cull) d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_CULLMODE, &old), o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        if (r.on_top) d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_ZFUNC, &old_z), o_SetRenderState(d, D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
        {
            const float live = g_ghost;
            g_ghost = r.ghost;
            Ghost ghost(d);
            g_ghost = live;
            if (r.op == RecOp::kDIP)
                reinterpret_cast<DipFn>(r.ptr)(d, r.type, r.a, dst, r.vcount, g_rec_idx.data() + r.iofs, r.icount, r.b);
            else
                reinterpret_cast<DpFn>(r.ptr)(d, r.type, r.a, dst, r.vcount, r.b);
        }
        if (r.on_top) o_SetRenderState(d, D3DRENDERSTATE_ZFUNC, old_z);
        if (r.no_cull) o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, old);
        break;
    }
    }
}

// One eye: the recorded calls on the device, every vertex moved by the eye's offset from the
// recording camera (d: right, up, forward in world units) and drawn with the same focal length
// and centre; near_z: the engine's near plane in the pass. A vertex with z = 0, rhw = 1 (the 2D
// interface's) stays where it is.
// (dxwrapper's DxwRingFill, from our patch: an eye's vertices and indices put in its draw buffers
// in one lock each, so the draws inside them don't copy or lock; see the dxwrapper patch.)
using RingFillFn = BOOL(WINAPI*)(const void*, UINT, UINT, const void*, UINT);
RingFillFn ring_fill() {
    static const RingFillFn f = [] {
        HMODULE m = GetModuleHandleW(L"dxwrapper.dll");
        return m ? reinterpret_cast<RingFillFn>(GetProcAddress(m, "DxwRingFill")) : nullptr;
    }();
    return f;
}
bool g_prefill = true;  // (debug: stereo replay_prefill 0|1)
std::vector<BYTE> g_eye_vtx;
std::vector<WORD> g_eye_idx;
struct GroupSpan {
    uint32_t v = 0, i = 0, nv = 0, ni = 0;  // (in vertices and indices into the eye's arrays)
};
std::vector<GroupSpan> g_spans;

void replay(void* device, float f, float cx, float cy, float dx, float dy, float dz, float near_z) {
    Dev* d = static_cast<Dev*>(device);
    if (!d) return;
    DrawDepth depth;
    if (!g_group) {
        for (const RecCmd& r : g_rec) replay_cmd(d, r, f, cx, cy, dx, dy, dz, near_z);
        return;
    }
    if (!g_plan_valid) build_plan(f, std::fabs(dx), std::fabs(dy), near_z);
    // The groups' vertices (all of one stride, 32: nearly every draw) moved to this eye, and their
    // indices, written once into two arrays and handed to dxwrapper in one go.
    constexpr size_t kStride = 32;
    g_spans.assign(g_groups.size(), {});
    size_t nv = 0, ni = 0;
    for (const PlanGroup& g : g_groups)
        if (fvf_stride(g_rec[g.draws[0]].a) == kStride) nv += g.verts, ni += g.idx;
    if (g_eye_vtx.size() < nv * kStride) g_eye_vtx.resize(nv * kStride);
    if (g_eye_idx.size() < ni) g_eye_idx.resize(ni);
    uint32_t vat = 0, iat = 0;
    for (size_t gi = 0; gi < g_groups.size(); ++gi) {
        const PlanGroup& g = g_groups[gi];
        if (fvf_stride(g_rec[g.draws[0]].a) != kStride) continue;
        GroupSpan& sp = g_spans[gi];
        sp.v = vat, sp.i = iat;
        uint32_t vb = 0;
        for (uint32_t di : g.draws) {
            const RecCmd& r = g_rec[di];
            move_vertices(r, g_eye_vtx.data() + size_t(vat + vb) * kStride, f, cx, cy, dx, dy, dz, near_z);
            const WORD* src = g_rec_idx.data() + r.iofs;
            WORD* out = g_eye_idx.data() + iat;
            for (uint32_t q = 0; q < r.icount; ++q) out[q] = WORD(src[q] + vb);
            vb += r.vcount, iat += r.icount;
        }
        sp.nv = vb, sp.ni = iat - sp.i;
        vat += vb;
    }
    const RingFillFn fill = g_prefill ? ring_fill() : nullptr;
    if (fill && nv) fill(g_eye_vtx.data(), UINT(nv * kStride), UINT(kStride), g_eye_idx.data(), UINT(ni * sizeof(WORD)));
    void* bound0 = reinterpret_cast<void*>(1);  // (unknown: the first group binds its own)
    for (const PlanItem& it : g_plan) {
        if (it.cmd >= 0) {
            const RecCmd& r = g_rec[it.cmd];
            if (r.op == RecOp::kTex && r.a == 0) bound0 = r.ptr;
            replay_cmd(d, r, f, cx, cy, dx, dy, dz, near_z);
        } else if (it.group >= 0) {
            const PlanGroup& g = g_groups[it.group];
            const RecCmd& k = g_rec[g.draws[0]];
            const GroupSpan& sp = g_spans[it.group];
            if (bound0 != g.tex) o_SetTexture(d, 0, static_cast<LPDIRECTDRAWSURFACE7>(g.tex)), bound0 = g.tex;
            if (!sp.nv) {  // (another stride: drawn as before)
                replay_cmd(d, k, f, cx, cy, dx, dy, dz, near_z);
                for (size_t q = 1; q < g.draws.size(); ++q) replay_cmd(d, g_rec[g.draws[q]], f, cx, cy, dx, dy, dz, near_z);
                continue;
            }
            DWORD old = 0;
            if (k.no_cull) d->lpVtbl->GetRenderState(d, D3DRENDERSTATE_CULLMODE, &old), o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            reinterpret_cast<DipFn>(k.ptr)(d, D3DPT_TRIANGLELIST, k.a, g_eye_vtx.data() + size_t(sp.v) * kStride, sp.nv, g_eye_idx.data() + sp.i, sp.ni, k.b);
            if (k.no_cull) o_SetRenderState(d, D3DRENDERSTATE_CULLMODE, old);
        } else if (bound0 != it.tex0) {
            o_SetTexture(d, 0, static_cast<LPDIRECTDRAWSURFACE7>(it.tex0)), bound0 = it.tex0;
        }
    }
    if (fill) fill(nullptr, 0, 0, nullptr, 0);  // (the arrays are rewritten for the next eye)
}
void set_replay_prefill(bool on) { g_prefill = on; }
void set_replay_group(bool on) { g_group = on, g_plan_valid = false; }
std::string record_stats() {
    char buf[200];
    snprintf(buf, sizeof(buf), "%zu calls, %u draws (%u grouped into %u), %zu KB of vertices%s%s", g_rec.size(), g_rec_draws, g_plan_in, g_plan_out, g_rec_vtx.size() / 1024,
             g_rec_unsupported ? " (some draws not replayable)" : "", g_rec_bad ? " (a render target set mid-pass)" : "");
    return buf;
}
void set_eye_pass(bool on) {
    if (on) g_pass_draws = g_pass_prims = 0;
    g_in_eye_pass = on;
}
void pass_draws(unsigned& draws, unsigned& prims) { draws = g_pass_draws, prims = g_pass_prims; }
void set_no_cull(bool on) { g_no_cull = on; }
bool& sea_solid() { return g_sea_solid; }
bool& sea_solid_under_land() { return g_sea_solid_under_land; }
void set_on_top(bool on) { g_on_top = on; }
void set_ghost(float opacity) { g_ghost = opacity; }
void set_vertex_filter(VertexFilter filter) { g_vertex_filter = filter; }

void set_sea_world(bool on) {
    g_sea_world_cfg = on;
    BWLOG("trace: sea_world %d", int(on));
}

void set_sea_only(bool on) {
    g_sea_only = on;
    BWLOG("trace: sea_only %d", int(on));
}

void request_capture(int frames) {
    g_cap_pending += frames;
    BWLOG("trace: capture of %d frame(s) requested", frames);
}

void end_frame(void* device, unsigned frame) {
    if (g_cap) {
        fclose(g_cap);
        g_cap = nullptr;
        // The back buffer still holds the frame we just traced (Flip hasn't run yet).
        if (device) {
            wchar_t name[64];
            swprintf(name, 64, L"captures\\trace_%06u.bmp", frame);
            screenshot::save_render_target(device, paths::bwvr_dir() + name);
        }
        BWLOG("trace: frame %u done (%u draws: %u xyz, %u rhw, %u vb; %u prims)", frame, g_cur.draws,
              g_cur.draws_xyz, g_cur.draws_rhw, g_cur.draws_vb, g_cur.prims);
    }
    g_last = g_cur;
    g_cur = Counters{};

    if (g_cap_pending > 0 && device) {
        --g_cap_pending;
        wchar_t name[64];
        swprintf(name, 64, L"captures\\trace_%06u.txt", frame + 1);
        g_cap = _wfopen((paths::bwvr_dir() + name).c_str(), L"w");
        if (g_cap) fprintf(g_cap, "# frame %u\n", frame + 1);
    }
}

std::string stats_line() {
    char buf[256];
    snprintf(buf, sizeof(buf), "last frame: %u draws (%u xyz, %u rhw, %u vb), %u prims, %u SetTransform, %u Clear, %u tris near-clipped, %u bad-index tris dropped, %u engine DrawTriangle calls (5 s)",
             g_last.draws, g_last.draws_xyz, g_last.draws_rhw, g_last.draws_vb, g_last.prims, g_last.set_transform,
             g_last.clears, g_clipped_tris, g_bad_index_tris, g_engine_tris_calls);
    std::string out = buf;
    if (g_batch.draws) {
        snprintf(buf, sizeof(buf), " | eye-pass draws %u, %u (%.0f%%) could merge with the one before", g_batch.draws, g_batch.mergeable,
                 100.0 * g_batch.mergeable / g_batch.draws);
        out += buf;
    }
    g_batch.draws = g_batch.mergeable = 0;
    g_engine_tris_calls = 0;
    g_clipped_tris = 0;
    g_bad_index_tris = 0;
    return out;
}

}  // namespace bw::trace
