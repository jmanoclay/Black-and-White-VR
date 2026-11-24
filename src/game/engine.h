// Typed views of Black & White engine data (layouts from bw1-decomp, BW1W120).
#pragma once
#include <cstdint>

#include "game/bw_symbols.h"

namespace bw::game {

struct LHPoint {
    float x, y, z;
};

struct LHCoord {
    int32_t x, y;
};

// 4x3 row-vector matrix: rows 0..2 are the basis, row 3 the translation (D3DMATRIX-style names).
struct LHMatrix {
    float m[4][3];
};

struct LH3DCamera {
    LHPoint pos;  // 0x00
    LHPoint foc;  // 0x0C
    float fov;    // 0x18 (radians, horizontal)
    LHPoint look_at;  // 0x1C
};
static_assert(sizeof(LH3DCamera) == 0x28);

struct InfoTransform {
    float near_clip;
    LHCoord resolution;
    float aspect_x_over_y;
    float half_res[2];
    float inv_half_res[2];
    float inv_half_tan_fov_y;
    float inv_half_tan_fov_x;
    float cos_half_fov_sqr;
    float field_2c;
    float cos_half_fov;
    float field_34;
    float inv_aspect_sqr_hypo_inv_times_inv_aspect;
    float inv_aspect_sqr_hypo_inv;
};
static_assert(sizeof(InfoTransform) == 0x40);

template <class T>
T& at(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}

inline LH3DCamera& camera() { return at<LH3DCamera>(sym::LH3DTech_g_camera); }
inline LHMatrix& world_to_camera() { return at<LHMatrix>(sym::LH3DTech_g_world_to_camera); }
inline LHMatrix& world_to_clipping() { return at<LHMatrix>(sym::LH3DTech_g_world_to_clipping); }
inline InfoTransform& info_transform() { return at<InfoTransform>(sym::LH3DTech_g_info_transform); }
inline float& meter_width_on_2() { return at<float>(sym::LH3DTech_g_meter_width_screen_on_2); }
inline float& meter_height_on_2() { return at<float>(sym::LH3DTech_g_meter_height_screen_on_2); }
inline int32_t& delta_time() { return at<int32_t>(sym::LH3DTech_g_delta_time); }
// Screen clip bounds {width-1, height-1}, set by UpdateViewPort.
inline float* max_screen() { return &at<float>(sym::g_MaxScreen); }
inline uint32_t& game_time_inc() { return at<uint32_t>(sym::LH3DTech_g_game_time_inc); }

// uint32_t __fastcall LH3DTech::ProjectPoint(LHPoint* point, int* x, int* y, float* depth)
using ProjectPointFn = uint32_t(__fastcall*)(LHPoint* point, int* x, int* y, float* depth);
inline uint32_t project_point(LHPoint p, int* x, int* y, float* depth) {
    return reinterpret_cast<ProjectPointFn>(sym::LH3DTech_ProjectPoint)(&p, x, y, depth);
}

// void __fastcall LH3DTech::UpdateViewPort(long width, long height): resolution, projection
// centre and clip bounds; resets the FOV to 70 degrees via ChangeFov.
inline void update_viewport(long w, long h) {
    reinterpret_cast<void(__fastcall*)(long, long)>(sym::LH3DTech_UpdateViewPort)(w, h);
}
// void __fastcall LH3DTech::UpdateCamera(const LHPoint& position, const LHPoint& focus): sets
// g_camera, world_to_camera/clipping (no roll) and derived copies such as g_d3d_world2view.
inline void update_camera(const LHPoint& pos, const LHPoint& foc) {
    reinterpret_cast<void(__fastcall*)(const LHPoint*, const LHPoint*)>(sym::LH3DTech_UpdateCamera)(&pos, &foc);
}
// LH3DTech::ChangeFov(float horizontal_fov_radians): declared as a member but ignores `this`;
// takes its argument on the stack and pops it (ret 4).
inline void change_fov(float fov) { reinterpret_cast<void(__stdcall*)(float)>(sym::LH3DTech_ChangeFov)(fov); }

// LH3DRender::StartFrame timing state (unnamed in the decomp; see the disassembly at 0x82F14E):
// g_delta_time = now - last_frame_ms (min 1), then last_frame_ms = now; plus a smoothed FPS.
constexpr uintptr_t kStartFrameLastMs = 0xEA9EAC;  // uint32 ms of the previous StartFrame
constexpr uintptr_t kStartFrameNowMs = 0xEA9EB0;   // uint32 ms of the current StartFrame
constexpr uintptr_t kStartFrameFps = 0xEC7FC0;     // 3 floats/ints: smoothed fps, fps, frame counter


// GGame fields used by the stereo renderer (offsets confirmed in Process3dEngine's code).
constexpr uintptr_t kGameViewMode = 0x205A28;      // GAME_VIEW_MODE (0 = world)
constexpr uintptr_t kGameLiquidTimeMs = 0x205D48;  // field_0x205d48: ms for UpdateLiquidParticles
constexpr uintptr_t kGameVideoPlayer = 0x250188;   // LHVideoPlayer*

template <class T>
T& game_field(void* game, uintptr_t off) {
    return *reinterpret_cast<T*>(static_cast<char*>(game) + off);
}

// GGame::g_game and its GCamera (GGame::GetCamera reads [game + 0x2502C0]).
constexpr uintptr_t kGGameInstance = 0xD0195C;
constexpr uintptr_t kGameCameraField = 0x2502C0;
constexpr uintptr_t kGCameraSetPositionAndFocus = 0x4438C0;  // __thiscall(const LHPoint&, const LHPoint&)
inline void* game_instance() { return at<void*>(kGGameInstance); }
inline void* game_camera_object() {
    void* g = game_instance();
    return g ? *reinterpret_cast<void**>(static_cast<char*>(g) + kGameCameraField) : nullptr;
}
inline void set_game_camera(const LHPoint& pos, const LHPoint& foc) {
    if (void* cam = game_camera_object())
        reinterpret_cast<void(__thiscall*)(void*, const LHPoint*, const LHPoint*)>(kGCameraSetPositionAndFocus)(cam, &pos,
                                                                                                            &foc);
}

// The sea is drawn as screen-space strips, one quad row per 2 pixels (fn_00879930, called from
// GLandscape::Draw). Its vertex and index arrays are allocated once by LH3DRender::Open for the
// screen height (fn_008792E0, __fastcall(height)), and the strips' right edge is fixed to the
// screen width. A taller render target makes the engine write vertices past the array and read
// garbage indices (a flickering sheet across the sky), so fit_sea() re-sizes them first.
constexpr uintptr_t kSeaInit = 0x8792E0;         // void __fastcall(int height): rows = height / 2
constexpr uintptr_t kSeaFree = 0x8794A0;         // void(): frees both arrays
constexpr uintptr_t kSeaInitialized = 0xFA9354;  // int
constexpr uintptr_t kSeaVerts = 0xFA9358;        // 32-byte XYZRHW|DIFFUSE|SPECULAR|TEX1 vertices
constexpr uintptr_t kSeaRows = 0xFA9360;         // int: quad rows the arrays hold
constexpr uintptr_t kSeaVertCount = 0xFA9364;    // int: 2 * (rows + 1); odd vertices are the right edge

inline void fit_sea(int width, int height) {
    if (!at<int>(kSeaInitialized)) return;
    if (at<int>(kSeaRows) < height / 2 + 2) {
        reinterpret_cast<void (*)()>(kSeaFree)();
        reinterpret_cast<void(__fastcall*)(int)>(kSeaInit)(height + 8);
    }
    auto* v = at<unsigned char*>(kSeaVerts);
    const int n = at<int>(kSeaVertCount);
    for (int i = 1; v && i < n; i += 2) *reinterpret_cast<float*>(v + i * 32) = float(width);
}

}  // namespace bw::game
