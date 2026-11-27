#include "render/stereo.h"

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

#include "core/load_trace.h"
#include "core/log.h"
#include "core/mod.h"
#include "core/paths.h"
#include "game/engine.h"
#include "control/vr_hand.h"
#include "ui/ui.h"
#include "render/d3d7_trace.h"
#include "render/mipmaps.h"
#include "render/eye_target.h"
#include "render/screenshot.h"
#include "render/d3d9_bridge.h"
#include "vr/hmd_profile.h"
#include "vr/math.h"
#include "vr/openvr_backend.h"

namespace bw::stereo {

bool install_saving_hook();  // (below)
bool install_disciple_hook();  // (below)
extern float g_disciple_scale;  // (below)
extern int g_disciple_log;
extern int g_disciple_force;
extern float g_disciple_lift;
void autosave_tick();        // (below: the Developer tab's "Autosave now")
extern bool g_autosave_defer_test;  // (below)

namespace {

using vr::Basis;
using vr::Vec3;

// ---------------- settings ----------------
struct Settings {
    bool enabled = false;
    float world_scale = 10.0f;  // engine units per real meter (god size)
    float ipd_scale = 1.0f;     // 0 = both eyes at the head centre (identity test)
    bool level_base = false;    // false: tracking origin follows the game camera's full orientation
    bool zero_time = true;      // second eye renders with zero time increments
    int eye_height = 1865;      // rows of the final (cropped) eye image (the most the 2048 px render-target limit allows for the Index)
    bool frozen = false;        // test determinism: no game turns, zero time increments in every pass
    bool use_openvr = false;    // [vr] backend=openvr: SteamVR head pose + frame submission
    float near_m = 0.05f;       // near plane, metres from the eye (24-bit z-buffer only)
    bool anchor_hand = true;    // the laser-mode hand sits on the laser's land point
    float anchor_fwd = 0.5f, anchor_up = 0.15f;  // ... its wrist this far behind / above it, in hand lengths
    bool saving_cover = true;    // [vr] saving_cover: while the game saves, the last frame on the virtual screen and "Saving..."
    bool saving_screen = false;        // [vr] saving_screen: the last frame on the virtual screen while saving (the 2026-11-04 try)
    bool autosave_fade_first = false;  // [vr] autosave_fade_first: the view faded to black before an autosave (the 2026-11-03 try)
    bool cutscene_carry = true;  // [stereo] cutscene_carry: handle cutscenes at all (off: the game's camera is fought, as before)
    bool cutscene_screen = true; // [stereo] cutscene_screen: show them on a cinema screen (off: the camera carries the player)
    int texture_filter = 1;     // [stereo] texture_filter: 0 the game's (none), 1 linear + mipmaps, 2 also anisotropic
    int anisotropy = 8;         // [stereo] anisotropy
    float lod_bias = 0;         // [textures] lod_bias: mip level offset (negative: sharper)
    bool head_audio = true;     // [stereo] head_audio: the 3D sound's listener is the head
    int object_distance = 250;  // [stereo] object_distance_percent: the engine's object vanish distance in the eye passes
    bool miracle_on_palm = true;  // [stereo] miracle_on_palm: a miracle in the hand (food's horn, wood) centred on the palm
    // [stereo] miracle_grip: ... and set against the palm, on its side, rather than where the game held
    // it (beside its hand: the wood hung apart from the hand, the horn behind it; playtest 2026-10-31).
    // How far out, toward the fingers and up: the tuning panel ([hands] held_miracle_*).
    bool miracle_grip = true;
    float miracle_effect_m = 0.12f;   // [stereo] miracle_effect_cm: a miracle's effect at the palm no bigger round than this (radius)
    int detail_distance = 150;  // [stereo] detail_distance_percent: the distances objects go to less detail and fade out, in the eye passes
    // [stereo] adaptive_detail: while frames take longer than the headset's frame time, the detail
    // distance eases down (to the game's own at the least: 100%), and back up when there's room.
    // Busy views from above were the slow ones (the engine's own work).
    bool adaptive_detail = true;
    bool one_pass = true;       // [stereo] one_pass: the engine draws once, from between the eyes; its draws replayed into each eye
    bool share_eyes = true;     // [stereo] share_eyes: through dxwrapper, the eyes go to SteamVR on the GPU (no readback)
    int helper_spread = 70;     // [stereo] helper_spread_percent: the helper spirits round the middle of the view (100: the flat game's angles)
    bool other_views_on_screen = true;  // [stereo] other_views_on_screen: views other than the world (the temple) on that screen
    bool temple_4x3 = true;     // [stereo] temple_4x3: the temple shows as much height as at 4:3 (its buttons are laid out for it)
    bool temple_unpause = true;  // [stereo] temple_unpause: a save loaded in the temple doesn't stay paused
    bool save_pictures = true;   // [stereo] save_pictures: the save room's pictures from the game's frame, not the (pink) screen
    bool second_hand = true;    // [stereo] second_hand: draw the other controller's hand too
    bool unsquash_hand = true;  // [stereo] unsquash_hand: no squash of the hand as it picks up or lets go
    bool shadow_on_ground = true;  // [stereo] shadow_on_ground: the hand's shadow cast from just above the land below it
    bool same_trees = true;  // [stereo] same_trees: trees bend and fade the same in both eyes
    int mirror = 1;             // game window: 0 nothing, 1 one eye filling it (for recording), 2 both eyes side by side
    bool sync_game_camera = true;  // [stereo] sync_game_camera: game camera follows the head
    bool upright_hand = true;       // [stereo] upright_hand: keep the hovering hand upright
    bool eye_facing_clouds = true;  // [stereo] eye_facing_clouds: 3D clouds face the eye
    bool hide_mouse_hints = true;  // [stereo] hide_mouse_hints: mouse/camera hints and tricons off in VR
};
Settings g_set;
float g_one_pass_k = 1.0f;  // (debug: one_pass_k, the eyes' shift in the replay scaled)
vr::HmdProfile g_profile;
vr::Pose g_head;  // head pose used for rendering (tracking space, meters; mock or SteamVR)

// SteamVR head pose -> tracking space relative to a recentred reference: the reference position
// becomes the origin and its heading becomes "forward" (yaw only, so the horizon stays level).
bool g_openvr_failed = false, g_have_ref = false, g_recenter = false;
Vec3 g_ref_pos;
float g_ref_yaw_deg = 0;
std::string g_mirror_name;  // stereo.req "mirror_capture <name>": SteamVR's mirror of both eyes
std::string g_burst_name;   // stereo.req "burst <name> <frames>": left eye of N consecutive frames
int g_burst_every = 1, g_burst_skip = 0;  // (a feedback burst takes every 8th frame)
int g_burst_left = 0, g_burst_index = 0;
double g_readback_ms = 0;
// Where a VR frame's time goes (5-second stats): the wait for SteamVR, the setup before the left
// eye, each eye, each eye's readback and upload, the rest of the 3D pass, and the game's work
// outside it (turns, 2D, its window). A frame is the time from one 3D pass to the next; those
// over the headset's frame time (120 Hz: 8.3 ms; 90 Hz: 11.1 ms) are reprojected by SteamVR.
struct FrameTiming {
    double wait = 0, setup = 0, eye[2] = {}, read[2] = {}, after = 0, outside = 0, max_frame = 0;
    double max_outside = 0, max_eyes = 0, max_read = 0;  // (the worst frame's parts)
    unsigned outside_over_2ms = 0;
    unsigned frames = 0, over_120 = 0, over_90 = 0;
};
FrameTiming g_ft;
struct FrameMarks {
    LARGE_INTEGER entry{}, waited{}, eye_start[2]{}, eye_end[2]{}, read_end[2]{};
    bool read[2] = {};
};
FrameMarks g_fm;
LARGE_INTEGER g_ft_last_entry{}, g_ft_last_exit{};

vr::Pose tracking_relative(const vr::Pose& raw) {
    if (!g_have_ref || g_recenter) {
        g_ref_pos = raw.pos;
        g_ref_yaw_deg = std::atan2(raw.rot.fwd.x, raw.rot.fwd.z) / vr::kDegToRad;
        g_have_ref = true;
        g_recenter = false;
        BWLOG("stereo: recentred at (%.3f, %.3f, %.3f) m, heading %.1f deg", raw.pos.x, raw.pos.y, raw.pos.z,
              g_ref_yaw_deg);
    }
    const Basis inv_yaw = vr::basis_from_ypr(-g_ref_yaw_deg, 0, 0);
    vr::Pose rel;
    rel.pos = inv_yaw.apply(raw.pos - g_ref_pos);
    rel.rot = inv_yaw.compose(raw.rot);
    return rel;
}
// Another device's raw pose in the same recentred frame as the head.
vr::Pose relative_to_ref(const vr::Pose& raw) {
    const Basis inv_yaw = vr::basis_from_ypr(-g_ref_yaw_deg, 0, 0);
    return {inv_yaw.compose(raw.rot), inv_yaw.apply(raw.pos - g_ref_pos)};
}

// Mock head sway: yaw = base + amp * sin(2*pi*t/period), updated every stereo frame.
float g_sway_amp = 0, g_sway_period = 2;
float g_pose_ypr[3] = {0, 0, 0};

// ---------------- per-frame state ----------------
using Process3dFn = void(__thiscall*)(void* game);
Process3dFn o_process3d = nullptr;
using TurnTimerFn = int(__thiscall*)(void* game);
TurnTimerFn o_turn_timer = nullptr;

int __fastcall turn_timer_detour(void* game, void*) { return g_set.frozen ? 0 : o_turn_timer(game); }

// Index of the eye pass in progress (-1 outside stereo rendering).
int g_current_eye = -1;
int g_force_cutscene = -1;  // debug: 0/1 overrides the game's widescreen flag
bool g_cinema_pass = false;  // the game is drawing its own flat frame for the cinema screen
float g_vignette = 0;       // the comfort vignette's strength now (0..1): how much the rig is moving the player
bool g_cinema_on = false;    // the cinema screen is up (a cutscene or another view): the game's own camera is shown
bool g_cutscene_log = false;  // debug: log the camera and the rig during a cutscene

// --- Debug: which globals does one eye pass change? (stereo.req "statediff <name>") ---
// Snapshots runblack.exe's writable sections before the first pass and after each pass, and
// writes every changed dword to captures\<name>.statediff (analysed by tools/analysis/statediff.py).
struct Section {
    uintptr_t start;
    size_t size;
};
bool g_statediff_heap = false;  // also cover committed private RW memory (the heap)
std::vector<std::pair<uintptr_t, size_t>> g_snap_storage;  // our own snapshot buffers (excluded)

std::vector<Section> writable_sections() {
    std::vector<Section> out;
    if (g_statediff_heap) {
        // All committed private read/write regions, except our own snapshot buffers.
        MEMORY_BASIC_INFORMATION mbi;
        for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
             a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
            const DWORD rw = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
            if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & rw) || (mbi.Protect & PAGE_GUARD))
                continue;
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            bool ours = false;
            for (auto& s : g_snap_storage) ours |= base >= s.first && base < s.first + s.second;
            if (!ours) out.push_back({base, mbi.RegionSize});
        }
    }
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
            out.push_back({reinterpret_cast<uintptr_t>(base) + s->VirtualAddress, s->Misc.VirtualSize & ~size_t(3)});
    return out;
}
std::string g_statediff_name;

// State the engine mutates on every render pass: snapshotted before the first eye pass and
// restored before the second, so both eyes start from identical state ([stereo] rewind, or
// stereo.req "rewind <hex addr> <bytes>"). Found with the statediff probe.
struct Range {
    uintptr_t addr;
    size_t len;
};
std::vector<Range> g_rewind;
std::vector<uint8_t> g_rewind_buf;

void rewind_save() {
    g_rewind_buf.clear();
    for (const Range& r : g_rewind) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(r.addr);
        g_rewind_buf.insert(g_rewind_buf.end(), p, p + r.len);
    }
}
void rewind_restore() {
    size_t off = 0;
    for (const Range& r : g_rewind) {
        memcpy(reinterpret_cast<void*>(r.addr), g_rewind_buf.data() + off, r.len);
        off += r.len;
    }
}

// One flat VirtualAlloc'd buffer, so heap snapshots don't include themselves.
std::vector<Section> g_snap_secs;
uint32_t* g_snap_mem = nullptr;
size_t g_snap_bytes = 0;

void snapshot() {
    g_snap_secs = writable_sections();
    size_t total = 0;
    for (auto& s : g_snap_secs) total += s.size;
    if (total > g_snap_bytes) {
        if (g_snap_mem) VirtualFree(g_snap_mem, 0, MEM_RELEASE);
        g_snap_bytes = total + (total >> 3);
        g_snap_mem = static_cast<uint32_t*>(VirtualAlloc(nullptr, g_snap_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_snap_storage = {{reinterpret_cast<uintptr_t>(g_snap_mem), g_snap_bytes}};
    }
    size_t off = 0;
    for (auto& s : g_snap_secs) {
        memcpy(reinterpret_cast<uint8_t*>(g_snap_mem) + off, reinterpret_cast<void*>(s.start), s.size);
        off += s.size;
    }
}

// Compares one region against its snapshot; SEH-guarded because the pass may have freed it.
unsigned diff_region(FILE* f, const char* label, const Section& s, const uint32_t* was, unsigned budget) {
    unsigned n = 0;
    __try {
        const uint32_t* now = reinterpret_cast<const uint32_t*>(s.start);
        for (size_t w = 0; w < s.size / 4; ++w)
            if (now[w] != was[w]) {
                if (n < budget) fprintf(f, "%s %08X %08X %08X\n", label, unsigned(s.start + w * 4), was[w], now[w]);
                ++n;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void write_statediff(FILE* f, const char* label) {
    unsigned n = 0;
    size_t off = 0;
    for (const Section& s : g_snap_secs) {
        const auto* was = reinterpret_cast<const uint32_t*>(reinterpret_cast<uint8_t*>(g_snap_mem) + off);
        n += diff_region(f, label, s, was, n < 2000000 ? 2000000 - n : 0);
        off += s.size;
    }
    BWLOG("statediff %s: %u dwords changed", label, n);
}

// Some per-frame updates live inside Process3dEngine. They smooth or move state on every call,
// so running them in the second eye pass would move things between the eyes (seen as the god
// hand differing at IPD 0). They run in the first pass only; the second pass draws the same state.
// LH3DRender::StartFrame measures real time since its previous call into g_delta_time. Called
// once per eye pass, it would give the second eye ~1 ms of extra animation. For the second pass
// (and every pass while frozen) the measured time is discarded: g_delta_time = 0 and the timer's
// "previous call" is restored, so the next frame's first pass sees the whole frame time.
using StartFrameFn = void(__cdecl*)();
StartFrameFn o_start_frame = nullptr;
void __cdecl start_frame_detour() {
    const bool discard = g_current_eye == 1 || (g_current_eye >= 0 && g_set.frozen);
    uint32_t last = game::at<uint32_t>(game::kStartFrameLastMs);
    uint32_t fps[3];
    memcpy(fps, &game::at<uint32_t>(game::kStartFrameFps), sizeof(fps));
    o_start_frame();
    if (discard) {
        game::delta_time() = 0;
        if (g_current_eye == 1) {
            game::at<uint32_t>(game::kStartFrameLastMs) = last;
            memcpy(&game::at<uint32_t>(game::kStartFrameFps), fps, sizeof(fps));
        }
    }
}

// Debug: mark the landscape's draw in trace captures.
using LandscapeDrawFn = void(__thiscall*)(void* landscape);
LandscapeDrawFn o_landscape_draw = nullptr;
void __fastcall landscape_draw_detour(void* landscape, void*) {
    trace::mark("landscape-begin");
    o_landscape_draw(landscape);
    trace::mark("landscape-end");
}

// Trees bend away from points the game lists during GLandscape::Draw: fn_005DF1B0 (cdecl:
// point, slot, radius) puts the point in a 14-entry table (0xD19A48, 16 bytes each: x, y, z,
// radius) and tags the trees in the 3x3 map cells round it with the slot (Tree +0x5C bits 6-9).
// Tree::Draw bends a tagged tree away from its slot's point (unless the point is above its top),
// then clears the tag. Slot 1: the carried thing; slot 2: the game's hand; -1: the next of slots
// 3-13 in turn (things in physics, fn_00646FE0).
constexpr uintptr_t kTreePush = 0x5DF1B0;
using TreePushFn = void(__cdecl*)(const game::LHPoint* point, int slot, float radius);
TreePushFn o_tree_push = nullptr;
int g_tree_push_log = 0;  // debug: log N calls (stereo tree_push_log N)
void __cdecl tree_push_detour(const game::LHPoint* point, int slot, float radius) {
    if (g_tree_push_log > 0) {
        --g_tree_push_log;
        BWLOG("trees: push eye %d slot %d (next %d) at (%.2f, %.2f, %.2f) radius %.2f", g_current_eye, slot,
              *reinterpret_cast<const int*>(0xBF2AF8), point->x, point->y, point->z, radius);
    }
    // A thrown tree pushes the trees in the map cells round it away, itself among them: in flight
    // it was drawn bent and squashed away from its own centre, looking see-through and blinking
    // (player, 2026-10-20 and -21; big rocks too, which push the trees they fly past). The thing
    // just let go of doesn't push (the point is its Game3DObject's position, +0x38).
    if (g_set.same_trees && slot == -1) {
        const char* rel = static_cast<const char*>(hand::recent_release());
        if (rel && reinterpret_cast<const char*>(point) == rel + 0x38) return;
    }
    o_tree_push(point, slot, radius);
}
// The trees, the same in both eyes? Tree::Draw bends a tree's matrix (and sets its colour), hands it
// to Game3DObject::AddForDrawing (0x63B5D0, __fastcall(Game3DObject*, Object*); called from
// 0x74B0CC), then puts the matrix back. Each tree's matrix and colour as the left eye drew it are
// kept, and a tree the right eye draws otherwise is logged: the player saw trees lean differently in
// each eye while moving over the land (2026-10-20), which no harness flight reproduced.
constexpr uintptr_t kAddForDrawing = 0x63B5D0;
constexpr uintptr_t kTreeAddReturn = 0x74B0D1;
using AddForDrawingFn = void(__fastcall*)(void* g3d, void* object);
AddForDrawingFn o_add_for_drawing = nullptr;
struct TreeSeen {
    float m[12];
    uint32_t colour;
    uint16_t flags;
};
std::unordered_map<const void*, TreeSeen> g_trees_left;
unsigned g_trees_differ = 0, g_trees_compared = 0;  // since the last report
int g_tree_alpha_log = 0;  // debug (stereo tree_alpha_log N): N trees drawn faded, with their distance
int g_burst_both = 0;  // debug: bursts save the right eye too
float g_drop_test_offset = 0;  // debug (stereo drop_test_offset y): the held/released thing raised in the right eye at AddForDrawing
int g_drop_log = 0;  // debug (stereo drop_log N): for N frames, where each eye draws the held thing and the one let go of
void* held_object_3d();
// The carried thing and the one just let go of, drawn by the right eye where the left eye drew it.
// A thing let go of showed in the left eye all along but went missing from the right eye for a
// couple of frames (player, 2026-10-19 and -20): between the passes the game (its physics, the
// hand letting go) or the mod's own throw moves it.
struct LeftDraw {
    const void* g3d = nullptr;
    float m[12];
};
LeftDraw g_left_draw;
// A thing let go of "blinked" as it left the hand (player, 2026-10-20; not seen in the harness).
// For its first 1.5 s each frame's left-eye draw is checked: a frame without it, or a jump of
// more than 3 units from the frame before, is logged.
struct BlinkWatch {
    const void* obj = nullptr;
    ULONGLONG since = 0;
    bool drawn = false, had = false;
    float pos[3] = {}, last[3] = {};
    int logged = 0;
};
BlinkWatch g_blink;
void blink_frame_end() {  // (the left eye's pass is done)
    const void* rel = hand::recent_release();
    if (rel != g_blink.obj) {
        g_blink = {};
        g_blink.obj = rel, g_blink.since = GetTickCount64();
        return;
    }
    if (!rel || GetTickCount64() - g_blink.since > 1500) return;
    if (!g_blink.drawn) {
        if (g_blink.logged++ < 20) BWLOG("drops: the thing let go of wasn't drawn %llu ms after (left eye)", GetTickCount64() - g_blink.since);
    } else {
        const float d = std::sqrt((g_blink.pos[0] - g_blink.last[0]) * (g_blink.pos[0] - g_blink.last[0]) +
                                  (g_blink.pos[1] - g_blink.last[1]) * (g_blink.pos[1] - g_blink.last[1]) +
                                  (g_blink.pos[2] - g_blink.last[2]) * (g_blink.pos[2] - g_blink.last[2]));
        if (g_blink.had && d > 3.0f && g_blink.logged++ < 20)
            BWLOG("drops: the thing let go of jumped %.1f units %llu ms after: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)", d,
                  GetTickCount64() - g_blink.since, g_blink.last[0], g_blink.last[1], g_blink.last[2], g_blink.pos[0], g_blink.pos[1], g_blink.pos[2]);
        memcpy(g_blink.last, g_blink.pos, sizeof(g_blink.last));
        g_blink.had = true;
    }
    g_blink.drawn = false;
}
bool g_same_drops = true;  // (debug: stereo same_drops 0)
void log_near_wrist(const char* via, const void* obj, uint32_t mesh);
int g_wrist_draw_log = 0;  // debug: log the next N draws of anything within 8 units of the game's wrist (wrist_draws N)
// What the hand would act on, lit up: its light colour (ARGB) made brighter by k; or, with too little room to
// brighten (lit near full white, as buildings in daylight: FFE8FEEF), tinted: warm gold, or pale blue (`cool`:
// the creature under the teleport's aim, the line's colour). 0 when it has none (nothing to raise).
uint32_t glow_colour(uint32_t old, float k, bool cool) {
    if ((old & 0x00FFFFFFu) == 0) return 0;
    const float r = float((old >> 16) & 0xFF), g = float((old >> 8) & 0xFF), b = float(old & 0xFF);
    const float most = std::max({r, g, b});
    const float t = k - 1.0f;
    const auto pack = [&](float rr, float gg, float bb) {
        const auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 255.0f)); };
        return (old & 0xFF000000u) | (c(rr) << 16) | (c(gg) << 8) | c(bb);
    };
    if (!cool && std::min(255.0f, most * k) - most >= 24.0f) return pack(r * k, g * k * 0.96f, b * k * 0.85f);
    return cool ? pack(r * (1.0f - 0.9f * t), g * (1.0f - 0.4f * t), b) : pack(r, g * (1.0f - 0.25f * t), b * (1.0f - 1.3f * t));
}
// And light added on top (the colour beside it, +0x50, FF000000 as the lighting leaves it): the light colour
// multiplies the texture, so it could only make it deeper, not brighter (player, 2026-11-26). [stereo] glow_add
// (RRGGBB at the glow's strongest; 0: none).
// The lighting doesn't write it: put back after each eye's world (glow_add_undo), not built up frame on frame.
uint32_t g_glow_add = 0x584A2C;
int g_glow_add_log = 3;
struct GlowAdded {
    uint32_t* at;
    uint32_t was;
};
std::vector<GlowAdded> g_glow_added;
void glow_add_on(uint32_t* at, float k) {
    if (!g_glow_add || !at || IsBadWritePtr(at, 4)) return;
    const auto added = [&](uint32_t old) {
        const float t = std::clamp((k - 1.0f) / 0.63f, 0.0f, 1.0f);
        uint32_t out = old & 0xFF000000u;
        for (int s = 0; s < 24; s += 8) out |= uint32_t(std::min(255.0f, float((old >> s) & 0xFF) + float((g_glow_add >> s) & 0xFF) * t)) << s;
        return out;
    };
    for (const auto& g : g_glow_added)
        if (g.at == at) {
            *at = added(g.was);
            return;
        }
    if (g_glow_added.size() >= 16) return;
    g_glow_added.push_back({at, *at});
    *at = added(g_glow_added.back().was);
    if (g_glow_add_log > 0) --g_glow_add_log, BWLOG("stereo: glow added: %08X -> %08X", g_glow_added.back().was, *at);
}
void glow_add_undo() {
    for (auto it = g_glow_added.rbegin(); it != g_glow_added.rend(); ++it)
        if (!IsBadWritePtr(it->at, 4)) *it->at = it->was;
    g_glow_added.clear();
}
const void* g_lit_obj = nullptr;  // (lit up by the lighting just now: not again as it is drawn)
ULONGLONG g_lit_ms = 0;
// The game lights an object as it is drawn (fn_00801C90, cdecl(its place +0x38, its light colour +0x4C, +0x50):
// a building in MultiMapFixed::DrawBuilding, the creature in LH3DCreature::AddForDrawing, 48 callers): the
// colour it has just worked out raised there, for what the hand would act on. (Raised beforehand, as it is
// handed to the renderer, a building's and the creature's were worked out again after: not lit.)
using LightFn = int(__cdecl*)(float* place, uint32_t* colour, uint32_t* other);
LightFn o_light = nullptr;
int __cdecl light_detour(float* place, uint32_t* colour, uint32_t* other) {
    const int r = o_light(place, colour, other);
    if (g_current_eye >= 0 && place && colour) {
        const void* obj3d = reinterpret_cast<const char*>(place) - 0x38;
        if (const float k = hand::glow_scale(obj3d, nullptr); k > 0) {
            const uint32_t was = *colour, c = glow_colour(was, k, hand::glow_cool());
            if (c) g_lit_obj = obj3d, g_lit_ms = GetTickCount64();
            static const void* told = nullptr;
            if (told != obj3d) told = obj3d, BWLOG("stereo: glow by the lighting on %p: %08X -> %08X (other %08X)", obj3d, was, c, other ? *other : 0);
            if (c) *colour = c, hand::glow_seen();
            if (c) glow_add_on(other, k);
        }
    }
    return r;
}
void add_for_drawing_inner(void* g3d, void* object, void* ret);
void __fastcall add_for_drawing_detour(void* g3d, void* object) {
    // What the hand would act on, lit up: its light colour (+0x4C, the engine's lighting of it, ARGB;
    // a rock's about 70% white) raised as it is handed to the renderer, which takes it then, as it does
    // the matrix; put back after.
    const float k = g_current_eye >= 0 && g3d ? hand::glow_scale(g3d, object) : 0.0f;
    uint32_t* col = k > 0 && !IsBadWritePtr(static_cast<char*>(g3d) + 0x4C, 4) ? reinterpret_cast<uint32_t*>(static_cast<char*>(g3d) + 0x4C) : nullptr;
    const uint32_t old = col ? *col : 0;
    const uint32_t lit = col && !(g3d == g_lit_obj && GetTickCount64() - g_lit_ms < 50) ? glow_colour(old, k, hand::glow_cool()) : 0;
    {
        static const void* told = nullptr;
        if (k > 0 && told != g3d) told = g3d, BWLOG("stereo: glow as it is drawn on %p (thing %p): %08X -> %08X", g3d, object, old, lit);
    }
    if (!lit) col = nullptr;  // (none to raise here: perhaps the lighting's, light_detour)
    if (col) *col = lit, hand::glow_seen(), glow_add_on(reinterpret_cast<uint32_t*>(static_cast<char*>(g3d) + 0x50), k);
    add_for_drawing_inner(g3d, object, _ReturnAddress());  // (its caller: trees are told apart by it)
    if (col) *col = old;
}
void add_for_drawing_inner(void* g3d, void* object, void* ret) {
    if (g_wrist_draw_log > 0 && g_current_eye == 0 && g3d) log_near_wrist("AddForDrawing", g3d, 0);
    if (g_current_eye >= 0 && g3d) {
        float* pm = reinterpret_cast<float*>(static_cast<char*>(g3d) + 0x14);
        float pre[12];
        memcpy(pre, pm, sizeof(pre));
        if (hand::prehold_draw(g3d, pre)) {  // (a tree on its way to the hand: drawn at the palm, left where it is)
            float own[12];
            memcpy(own, pm, sizeof(own));
            memcpy(pm, pre, sizeof(pre));
            o_add_for_drawing(g3d, object);
            memcpy(pm, own, sizeof(own));
            return;
        }
        const void* rel = hand::recent_release();
        if (g3d == rel || g3d == held_object_3d()) {
            float* m = reinterpret_cast<float*>(static_cast<char*>(g3d) + 0x14);
            float own[12];
            memcpy(own, m, sizeof(own));
            bool replaced = g_current_eye > 0 && g_same_drops && g_left_draw.g3d == g3d && memcmp(g_left_draw.m, m, sizeof(own)) != 0;
            if (replaced) memcpy(m, g_left_draw.m, sizeof(own));
            // While it flies from the hand before the game launches it, it is drawn where its flight
            // has got to: once the game's turn has taken the throw, something in the game puts it
            // back each frame where it was at that turn, and it hung still for a few frames, then
            // jumped at the launch (player, 2026-10-23). Its turn too: with the game's, a rock blinked
            // a quarter turn round and back (player, 2026-10-23, later).
            float flight[12];
            if (g_current_eye == 0 && g3d == rel && hand::in_flight(g3d, flight) && memcmp(m, flight, sizeof(flight)) != 0) {
                memcpy(m, flight, sizeof(flight));
                replaced = true;
            }
            if (g_current_eye == 0) {
                g_left_draw.g3d = g3d;
                memcpy(g_left_draw.m, m, sizeof(g_left_draw.m));
            }
            if (g_current_eye == 0 && g3d == rel) g_blink.drawn = true, memcpy(g_blink.pos, m + 9, sizeof(g_blink.pos));
            if (g_current_eye == 1 && g_drop_test_offset != 0) m[10] += g_drop_test_offset, replaced = true;  // (debug)
            o_add_for_drawing(g3d, object);
            if (g_drop_log > 0)
                BWLOG("drops: eye %d draws %s %p at (%.2f, %.2f, %.2f): alpha %u%s", g_current_eye, g3d == rel ? "released" : "held",
                      g3d, m[9], m[10], m[11], *reinterpret_cast<const uint32_t*>(static_cast<char*>(g3d) + 0x4C) >> 24,
                      replaced ? " (the left eye's place)" : "");
            if (replaced) memcpy(m, own, sizeof(own));
            return;
        }
    }
    if (g_current_eye >= 0 && ret == reinterpret_cast<void*>(kTreeAddReturn) && g3d && object) {
        const char* o = static_cast<const char*>(g3d);
        TreeSeen t;
        memcpy(t.m, o + 0x14, sizeof(t.m));
        t.colour = *reinterpret_cast<const uint32_t*>(o + 0x4C);
        t.flags = *reinterpret_cast<const uint16_t*>(static_cast<const char*>(object) + 0x5C);
        if (g_current_eye == 0) {
            g_trees_left[object] = t;
            if (g_tree_alpha_log > 0 && (t.colour >> 24) < 255) {
                --g_tree_alpha_log;
                const auto& c = game::camera();
                const float dx = t.m[9] - c.pos.x, dy = t.m[10] - c.pos.y, dz = t.m[11] - c.pos.z;
                BWLOG("trees: tree %p alpha %u at %.1f m (flags %04X)", object, t.colour >> 24, std::sqrt(dx * dx + dy * dy + dz * dz) / 10.0f, t.flags);
            }
        } else if (auto it = g_trees_left.find(object); it != g_trees_left.end()) {
            ++g_trees_compared;
            const TreeSeen& l = it->second;
            float most = 0;
            for (int i = 0; i < 9; ++i) most = std::max(most, std::fabs(l.m[i] - t.m[i]));
            const bool alpha_differs = (l.colour >> 24) != (t.colour >> 24);
            if (most > 0.02f || alpha_differs) {
                static int logged = 0;
                if (logged++ < 60)
                    BWLOG("trees: the eyes differ: tree %p at (%.1f, %.1f, %.1f) up L (%.3f, %.3f, %.3f) R (%.3f, %.3f, %.3f) alpha %u/%u flags %04X/%04X",
                          object, t.m[9], t.m[10], t.m[11], l.m[3], l.m[4], l.m[5], t.m[3], t.m[4], t.m[5], l.colour >> 24, t.colour >> 24,
                          l.flags, t.flags);
                ++g_trees_differ;
            }
        }
    }
    if (g_current_eye == 1 && g_drop_test_offset != 0 && ret == reinterpret_cast<void*>(kTreeAddReturn) && g3d) {  // (debug)
        float* m = reinterpret_cast<float*>(static_cast<char*>(g3d) + 0x14);
        m[10] += g_drop_test_offset;
        o_add_for_drawing(g3d, object);
        m[10] -= g_drop_test_offset;
        return;
    }
    o_add_for_drawing(g3d, object);
}

using HandMethodFn = void(__thiscall*)(void* hand);
HandMethodFn o_hand_prepare = nullptr, o_hand_held = nullptr;
float g_engine_near = 0;  // inside an eye pass: the engine's own near plane (the pass renders with a nearer one)

void* g_hand = nullptr;  // the render CHand (debug logging)
// The game frees its hand when it restarts in place (a new game, another player) and prepares a new
// one later (hand_prepare_detour). Dropped once it no longer looks like the game's (its vtable in
// the game's image, its status readable): Start a New Game crashed in held_object_3d (2026-11-02).
bool hand_alive() {
    if (!g_hand) return false;
    const char* h = static_cast<const char*>(g_hand);
    const uintptr_t vt = IsBadReadPtr(h, 0x4880) ? 0 : *reinterpret_cast<const uintptr_t*>(h);
    const char* status = vt ? *reinterpret_cast<const char* const*>(h + 0x4850) : nullptr;
    if (vt >= 0x401000 && vt < 0x1000000 && status && !IsBadReadPtr(status, 0x130)) return true;
    BWLOG("stereo: the game's hand %p is gone (a restart?): left alone until the game prepares a new one", g_hand);
    g_hand = nullptr;
    return false;
}

// The hand is a skinned mesh: fn_0084C3C0 (mesh draw) transforms each sub-mesh by a bone matrix
// from the global array [0xE9FE48] (4x3, 48 bytes each; index = word at +2 of the mesh's bone
// list entries, list at mesh+0x24, count at mesh+0x20). The animation has already placed those
// bones in world space, so the hand's object matrix plays no part. To move or turn the drawn
// hand, transform the bones it uses while (only while) the hand draws.
constexpr uintptr_t kObjectDraw = 0x813BA0;   // __thiscall(LH3DObject*)
constexpr uintptr_t kMeshDraw = 0x84C3C0;     // __thiscall(mesh*)
constexpr uintptr_t kBoneArray = 0xE9FE48;    // float (*)[12]
constexpr uintptr_t kDetailScale = 0xC37EA0;    // float: the last checked object's size on the screen (CheckRegionOnScreen writes it)
float g_size_boost = 1.0f;  // in the eye passes: objects' sizes on the screen are taken as this much bigger
constexpr uintptr_t kDetailDistCap = 0xC37EC8;  // float: objects' detail distances (fn_00815A70: a size-based scale capped at this, 5)
constexpr uintptr_t kVanishObjectDist = 0xC37200;  // float: objects are drawn from land blocks nearer than this (SetVanishObjectDist)
using ThisFn = void(__thiscall*)(void*);
ThisFn o_obj_draw = nullptr, o_mesh_draw = nullptr;
bool g_drawing_hand = false;
int g_bone_dump = 0;  // debug: log the hand's meshes and bones for N hand draws
// The drawn hand is re-posed as a whole: a rotation about the wrist (bone 0) and a move of the
// wrist, applied to every bone it uses while (only while) it draws.
//  - Upright fix, hovering (state 1) and holding the land (state 2): B&W tips the hand by where
//    its cursor sits in the view; an eye's view reaches far past the flat game's, so in the lower
//    half of the headset's view the hand was drawn pitched over, fingers at the player (player
//    snapshot snap_002). The correct pose has the wrist's local y axis pointing straight down;
//    the smallest rotation that restores it is applied (heading and finger animation kept).
//  - Near hand (hand::near_hand): within reach the wrist goes to the controller, fingers along
//    its pointing direction, palm toward its underside, eased in and out over ~0.15 s.
struct HandXform {
    bool on = false;
    float r[3][3];        // world rotation, column-vector form (v' = r v)
    float from[3], to[3]; // the wrist moves from -> to
    float offset_scale = 1.0f;  // for a carried object: its offset from the wrist grows with the hand
    float u[3][3] = {};         // ... and is kept in the game wrist's own axes (unit rows)
};
HandXform g_hand_xf;
unsigned g_frame_counter = 0;   // stereo frames since the start (for per-frame bookkeeping)
HandXform g_last_hand_xf;       // the god hand's transform at its last draw in an eye pass
// (For finding what the game draws at its hand: the leash loop while a leash is only chosen, say.)
void log_near_wrist(const char* via, const void* obj, uint32_t mesh) {
    if (!g_last_hand_xf.on || IsBadReadPtr(obj, 0x40)) return;
    const float* m = reinterpret_cast<const float*>(static_cast<const char*>(obj) + 0x14);
    const HandXform& xf = g_last_hand_xf;
    const float d[3] = {m[9] - xf.from[0], m[10] - xf.from[1], m[11] - xf.from[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist > 8.0f) return;
    --g_wrist_draw_log;
    const void* vt = *reinterpret_cast<void* const*>(obj);
    const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float radius = -1;  // (its bounding sphere's, as drawn: info +0x30 x scale)
    using InfoFn = const float*(__thiscall*)(const void*);
    void* const* vtbl = static_cast<void* const*>(vt);
    if (vtbl && !IsBadReadPtr(vtbl + 0xF8 / 4, 4)) {
        const auto info_fn = reinterpret_cast<InfoFn>(vtbl[0xF8 / 4]);
        const float* f = info_fn ? info_fn(obj) : nullptr;
        if (f && !IsBadReadPtr(f, 0x34)) radius = f[12] * scale;
    }
    BWLOG("stereo: near the wrist (%s): %p vtable %p mesh %08X at (%.2f, %.2f, %.2f), %.2f from the game's wrist, scale %.3f, radius %.2f", via, obj, vt,
          mesh, m[9], m[10], m[11], dist, scale, radius);
}
HandXform g_held_xf;            // the near hand's transform, reused for the object it carries
float g_held_game_pos[3] = {}, g_held_moved_pos[3] = {};  // debug: where the game put it / where we moved it
bool g_hand_xf_wanted = false;  // the hand is drawing in an eye pass: decide at its first mesh
bool g_second_hand_pass = false;  // drawing the god hand again, as the other controller's hand
// This pass draws the game's (right-hand) mesh mirrored, as a left hand: the second hand's pass,
// or the god hand's when the main hand is the left (player, 2026-11-02: the models were swapped).
bool mirrored_pass() { return g_second_hand_pass != hand::main_hand_left(); }
uint32_t g_hand_state_drawing = 0;

void rotation_between(const float a[3], const float b[3], float q[3][3]) {
    // Rodrigues: rotation taking unit vector a onto unit vector b.
    const float v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const float k = 1.0f / std::max(1e-6f, 1.0f + c);
    const float vx[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) {
            float vx2 = 0;
            for (int i = 0; i < 3; ++i) vx2 += vx[r][i] * vx[i][col];
            q[r][col] = (r == col ? 1.0f : 0.0f) + vx[r][col] + vx2 * k;
        }
}

// Rotation by `t` (0..1) of the way along rotation m (axis-angle), column-vector form.
void partial_rotation(const float m[3][3], float t, float out[3][3]) {
    const float c = std::clamp((m[0][0] + m[1][1] + m[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    const float angle = std::acos(c);
    float axis[3] = {m[2][1] - m[1][2], m[0][2] - m[2][0], m[1][0] - m[0][1]};
    float n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (n < 1e-6f || t >= 1.0f) {
        if (t >= 1.0f || angle < 1e-4f) {
            memcpy(out, m, sizeof(float) * 9);
            if (t < 1.0f) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) out[i][j] = i == j ? 1.0f : 0.0f;
            return;
        }
        // 180 degrees: axis from the diagonal.
        for (int i = 0; i < 3; ++i) axis[i] = std::sqrt(std::max(0.0f, (m[i][i] + 1.0f) * 0.5f));
        if (m[0][1] < 0) axis[1] = -axis[1];
        if (m[0][2] < 0) axis[2] = -axis[2];
        n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    }
    for (float& v : axis) v /= n;
    const float th = angle * t, s = std::sin(th), k = 1.0f - std::cos(th);
    const float x = axis[0], y = axis[1], z = axis[2];
    const float r[3][3] = {{1 - k * (y * y + z * z), -s * z + k * x * y, s * y + k * x * z},
                           {s * z + k * x * y, 1 - k * (x * x + z * z), -s * x + k * y * z},
                           {-s * y + k * x * z, s * x + k * y * z, 1 - k * (x * x + y * y)}};
    memcpy(out, r, sizeof(r));
}

float det3(const float a[3], const float b[3], const float c[3]) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
}

// Decide this hand draw's transform from the wrist bone (bones are filled just before the hand's
// meshes draw).
int g_hand_xf_log = 0;  // debug: log the wrist bone's axes for N hand draws
// The hand's size, from its wrist bone: the longest of its axes. (Picking up a living thing, the
// game squashes the hand for a tenth of a second: two of the axes shrink to a quarter, one stays.
// Sized from the first axis, the drawn hand was blown up four times over meanwhile.)
float wrist_scale(const float* root) {
    float m = 0;
    for (int r = 0; r < 3; ++r) m = std::max(m, std::sqrt(root[r * 3] * root[r * 3] + root[r * 3 + 1] * root[r * 3 + 1] + root[r * 3 + 2] * root[r * 3 + 2]));
    return m;
}
// Finger poses. Both drawn hands use the game's one skeleton, posed by its animation, so the
// other hand copied whatever the god hand was doing, and a grip on nothing left the hand open.
// The open pose is recorded from the game's own animation, each bone relative to the wrist
// (bone 0): the hand hovering (state 1) for a second, its fingers relaxed. Recorded once and kept
// in bwvr\hand_poses.bin. The fist is that pose with the fingers curled: the game has no fist
// (holding the land, its fingers are splayed, less curled than hovering). A hand drawn with a
// pose has its finger bones rebuilt from the wrist it has now.
// The skeleton (wrist space: z along the fingers, y toward the palm): four fingers of three
// bones, knuckle to tip (8-10, 11-13, 14-16, 17-19), the thumb (20-21); the rest are the palm.
constexpr int kPoseBones = 22;
struct PoseLib {
    uint32_t version = 1;
    uint32_t have[3] = {};  // by hand::HandPose
    float rel[3][kPoseBones * 12] = {};
};
PoseLib g_poses;
bool g_poses_loaded = false;
hand::HandPose g_pose_now = hand::HandPose::kGame;  // this hand draw's fingers
int g_pose_log = 0;  // debug: log the pose of each hand for N frames (stereo "pose_log N")

std::wstring poses_file() { return paths::bwvr_dir() + L"hand_poses.bin"; }
void load_poses() {
    g_poses_loaded = true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, poses_file().c_str(), L"rb") || !f) return;
    PoseLib lib;
    if (fread(&lib, sizeof(lib), 1, f) == 1 && lib.version == g_poses.version) g_poses = lib;
    fclose(f);
    BWLOG("stereo: hand poses loaded (open %u, fist %u)", g_poses.have[1], g_poses.have[2]);
}
void save_poses() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, poses_file().c_str(), L"wb") || !f) return;
    fwrite(&g_poses, sizeof(g_poses), 1, f);
    fclose(f);
}
// Rows are a bone's axes in world space, row-vector form (world = local * M + t).
bool invert3(const float* m, float out[9]) {
    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) return false;
    const float k = 1.0f / det;
    const float r[9] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
                        (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
                        (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
    memcpy(out, r, sizeof(r));
    return true;
}
bool relative_pose(const float* bones, float* rel) {
    float winv[9];
    if (!invert3(bones, winv)) return false;
    for (int b = 0; b < kPoseBones; ++b) {
        const float* m = bones + b * 12;
        float* o = rel + b * 12;
        for (int r = 0; r < 4; ++r) {
            float v[3] = {m[r * 3], m[r * 3 + 1], m[r * 3 + 2]};
            if (r == 3) v[0] -= bones[9], v[1] -= bones[10], v[2] -= bones[11];
            for (int c = 0; c < 3; ++c) o[r * 3 + c] = v[0] * winv[c] + v[1] * winv[3 + c] + v[2] * winv[6 + c];
        }
    }
    return true;
}
void record_pose(hand::HandPose pose, const float* bones) {
    if (!relative_pose(bones, g_poses.rel[int(pose)])) return;
    g_poses.have[int(pose)] = 1;
    save_poses();
    BWLOG("stereo: hand pose recorded: %s", pose == hand::HandPose::kFist ? "fist" : "open");
}
// Watches the game's hand (its first draw of the frame) for a pose to record.
void watch_for_poses(uint32_t state, const float* bones) {
    if (!g_poses_loaded) load_poses();
    static uint32_t last = ~0u;
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    if (state != last) last = state, since = now;
    if (state == 1 && !g_poses.have[1] && now - since > 1000) record_pose(hand::HandPose::kOpen, bones);
}
// The fist: each finger joint curled toward the palm by these angles (knuckle, middle, tip),
// the thumb folded across it.
float g_fist_deg[4] = {60, 70, 50, 40};  // stereo "fist <knuckle> <middle> <tip> <thumb>"
float g_fist[kPoseBones * 12] = {};
bool g_fist_built = false;
void rotate_bone(float* m, const float q[3][3], const float* pivot) {
    for (int r = 0; r < 4; ++r) {
        float v[3] = {m[r * 3], m[r * 3 + 1], m[r * 3 + 2]};
        if (r == 3) v[0] -= pivot[0], v[1] -= pivot[1], v[2] -= pivot[2];
        for (int c = 0; c < 3; ++c) m[r * 3 + c] = q[c][0] * v[0] + q[c][1] * v[1] + q[c][2] * v[2] + (r == 3 ? pivot[c] : 0.0f);
    }
}
void build_fist() {
    g_fist_built = true;
    memcpy(g_fist, g_poses.rel[int(hand::HandPose::kOpen)], sizeof(g_fist));
    const auto curl = [](const int* chain, int n, const float* deg, int axis) {
        for (int j = 0; j < n; ++j) {
            // About the wrist's x axis, turning z toward +y (the palm); the thumb about z, x toward +y.
            const float a = deg[j] * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
            float q[3][3];
            if (axis == 0) {
                const float m[3][3] = {{1, 0, 0}, {0, c, sn}, {0, -sn, c}};
                memcpy(q, m, sizeof(q));
            } else {
                const float m[3][3] = {{c, -sn, 0}, {sn, c, 0}, {0, 0, 1}};
                memcpy(q, m, sizeof(q));
            }
            float pivot[3];
            memcpy(pivot, g_fist + chain[j] * 12 + 9, sizeof(pivot));
            for (int k = j; k < n; ++k) rotate_bone(g_fist + chain[k] * 12, q, pivot);
        }
    };
    static const int fingers[4][3] = {{8, 9, 10}, {11, 12, 13}, {14, 15, 16}, {17, 18, 19}};
    for (const auto& f : fingers) curl(f, 3, g_fist_deg, 0);
    static const int thumb[2] = {20, 21};
    const float thumb_deg[2] = {g_fist_deg[3], g_fist_deg[3]};
    curl(thumb, 2, thumb_deg, 1);
}
// Pointing (drawing a gesture): the fist with the index finger left out, a little bent.
float g_point[kPoseBones * 12] = {};
bool g_point_built = false;
int g_point_finger = 3;  // (which chain is the index finger: stereo point_finger <0-3>; 17-19 lies next to the thumb)
float g_point_deg[3] = {5, 10, 5};
void build_point() {
    g_point_built = true;
    if (!g_fist_built) build_fist();
    memcpy(g_point, g_fist, sizeof(g_point));
    static const int fingers[4][3] = {{8, 9, 10}, {11, 12, 13}, {14, 15, 16}, {17, 18, 19}};
    // The index finger as in the open hand, then bent a little at each joint.
    const int* chain = fingers[std::clamp(g_point_finger, 0, 3)];
    for (int j = 0; j < 3; ++j) memcpy(g_point + chain[j] * 12, g_poses.rel[int(hand::HandPose::kOpen)] + chain[j] * 12, 12 * sizeof(float));
    for (int j = 0; j < 3; ++j) {
        const float a = g_point_deg[j] * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
        const float q[3][3] = {{1, 0, 0}, {0, c, sn}, {0, -sn, c}};
        float pivot[3];
        memcpy(pivot, g_point + chain[j] * 12 + 9, sizeof(pivot));
        for (int k = j; k < 3; ++k) rotate_bone(g_point + chain[k] * 12, q, pivot);
    }
}
// The bone as the pose has it, on the wrist `w` (bone 0 as it is now).
// The game squashes its hand for a moment as it picks up or lets go (its axes 8% uneven for
// about 0.15 s, the fingers flicking): a cartoon touch that looked like a distortion of the hand
// at arm's length in the headset. While the wrist is squashed, it is drawn evenly scaled and the
// rest of the hand as it was just before.
float g_last_good[kPoseBones * 12] = {};
bool g_have_last_good = false, g_squashed = false;
float squash_ratio(const float* m) {
    float lo = 1e9f, hi = 0;
    for (int r = 0; r < 3; ++r) {
        const float l = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
        lo = std::min(lo, l), hi = std::max(hi, l);
    }
    return hi > 0 ? lo / hi : 1.0f;
}
void unsquash(float* m) {  // the axes scaled up to the longest
    float len[3], hi = 0;
    for (int r = 0; r < 3; ++r) len[r] = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]), hi = std::max(hi, len[r]);
    for (int r = 0; r < 3; ++r)
        if (len[r] > 1e-9f)
            for (int c = 0; c < 3; ++c) m[r * 3 + c] *= hi / len[r];
}
bool posed_bone(int idx, const float* w, float out[12]) {
    if (idx <= 0 || idx >= kPoseBones) return false;
    const float* rel = nullptr;
    if (g_pose_now == hand::HandPose::kGame) {
        if (!g_squashed || !g_have_last_good) return false;
        rel = g_last_good + idx * 12;
    } else {
        if (!g_poses.have[int(hand::HandPose::kOpen)]) return false;
        if (g_pose_now == hand::HandPose::kFist && !g_fist_built) build_fist();
        if (g_pose_now == hand::HandPose::kPoint && !g_point_built) build_point();
        rel = (g_pose_now == hand::HandPose::kFist    ? g_fist
               : g_pose_now == hand::HandPose::kPoint ? g_point
                                                      : g_poses.rel[int(hand::HandPose::kOpen)]) + idx * 12;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = rel[r * 3] * w[c] + rel[r * 3 + 1] * w[3 + c] + rel[r * 3 + 2] * w[6 + c] + (r == 3 ? w[9 + c] : 0.0f);
    return true;
}

// A left main hand: the god hand's transform, then mirrored across its own wrist plane (the
// wrist's x axis, across the palm, reversed): r' = r (I - 2 x x^T). The game's hand otherwise.
void mirror_main_hand(const float (&u)[3][3]) {
    if (!hand::main_hand_left()) return;
    if (!g_hand_xf.on) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) g_hand_xf.r[i][j] = i == j ? 1.0f : 0.0f;
        // (to = from: in place)
        g_hand_xf.on = true;
    }
    float m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float ru = g_hand_xf.r[i][0] * u[0][0] + g_hand_xf.r[i][1] * u[0][1] + g_hand_xf.r[i][2] * u[0][2];
            m[i][j] = g_hand_xf.r[i][j] - 2.0f * ru * u[0][j];
        }
    memcpy(g_hand_xf.r, m, sizeof(m));
}

void decide_hand_xform(const float* root) {
    g_hand_xf.on = false;
    g_pose_now = hand::HandPose::kGame;
    if (g_hand_xf_log > 0 && !g_second_hand_pass) {
        --g_hand_xf_log;
        const auto len = [&](int r) { return std::sqrt(root[r * 3] * root[r * 3] + root[r * 3 + 1] * root[r * 3 + 1] + root[r * 3 + 2] * root[r * 3 + 2]); };
        BWLOG("stereo: hand draw, state %u: wrist axes %.5f %.5f %.5f, wrist at (%.2f, %.2f, %.2f)", g_hand_state_drawing, len(0), len(1),
              len(2), root[9], root[10], root[11]);
    }
    float u[3][3];  // the wrist's unit axes (its rows)
    for (int r = 0; r < 3; ++r) {
        const float* v = root + r * 3;
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-6f) return;
        for (int c = 0; c < 3; ++c) u[r][c] = v[c] / len;
    }
    memcpy(g_hand_xf.from, root + 9, sizeof(g_hand_xf.from));
    memcpy(g_hand_xf.to, root + 9, sizeof(g_hand_xf.to));
    // The laser-mode hand (hovering, or holding the land): upright, and on the laser's land
    // point. The game hovers its hand part of the way along the line of sight, by an amount
    // that depends on where the cursor is in the view and on the world's scale; that made the
    // hand wander off the laser in the lower half of the view and when scaled in.
    float laser_r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float laser_to[3] = {root[9], root[10], root[11]};
    bool laser_on = false;
    if (g_hand_state_drawing == 1 || g_hand_state_drawing == 2) {
        if (g_set.upright_hand && u[1][1] > -0.999f && u[1][1] < 0.95f) {  // already upright: nothing; nearly reversed: leave
            const float down[3] = {0, -1, 0};
            rotation_between(u[1], down, laser_r);
            laser_on = true;
        }
        vr::Vec3 anchor;
        if (g_set.anchor_hand && hand::laser_anchor(anchor)) {
            // The mesh is about 380 of its own units long; the wrist bone's axes carry its scale.
            const float hand_len = 380.0f * wrist_scale(root);
            float fingers[3];
            for (int c = 0; c < 3; ++c) fingers[c] = laser_r[c][0] * u[2][0] + laser_r[c][1] * u[2][1] + laser_r[c][2] * u[2][2];
            laser_to[0] = anchor.x - fingers[0] * g_set.anchor_fwd * hand_len;
            laser_to[1] = anchor.y - fingers[1] * g_set.anchor_fwd * hand_len + g_set.anchor_up * hand_len;
            laser_to[2] = anchor.z - fingers[2] * g_set.anchor_fwd * hand_len;
            laser_on = true;
        }
    }
    hand::NearHand nh;
    if (g_second_hand_pass) {
        // The other hand: the same mesh, mirrored (so the transform is a rotation with a
        // reflection: the opposite handedness to the bones'), at the other controller.
        if (!hand::second_hand(nh)) return;
        g_pose_now = nh.pose;
        float t[3][3] = {{nh.x.x, nh.x.y, nh.x.z}, {nh.y.x, nh.y.y, nh.y.z}, {nh.z.x, nh.z.y, nh.z.z}};
        if (((det3(t[0], t[1], t[2]) < 0) == (det3(u[0], u[1], u[2]) < 0)) == mirrored_pass())
            for (float& v : t[0]) v = -v;
        const float len = wrist_scale(root);
        const float k = nh.axis_len > 0 && len > 1e-9f ? nh.axis_len / len : 1.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) g_hand_xf.r[i][j] = k * (t[0][i] * u[0][j] + t[1][i] * u[1][j] + t[2][i] * u[2][j]);
        g_hand_xf.to[0] = nh.pos.x, g_hand_xf.to[1] = nh.pos.y, g_hand_xf.to[2] = nh.pos.z;
        g_hand_xf.on = true;
        return;
    }
    if (hand::near_hand(nh)) {
        if (nh.blend >= 1.0f) g_pose_now = nh.pose;
        float t[3][3] = {{nh.x.x, nh.x.y, nh.x.z}, {nh.y.x, nh.y.y, nh.y.z}, {nh.z.x, nh.z.y, nh.z.z}};
        if ((det3(t[0], t[1], t[2]) < 0) != (det3(u[0], u[1], u[2]) < 0))
            for (float& v : t[0]) v = -v;  // match the bones' handedness: a proper rotation
        float full[3][3];  // maps each wrist axis u_i onto t_i: sum_i t_i u_i^T
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) full[i][j] = t[0][i] * u[0][j] + t[1][i] * u[1][j] + t[2][i] * u[2][j];
        partial_rotation(full, nh.blend, g_hand_xf.r);
        const float to[3] = {nh.pos.x, nh.pos.y, nh.pos.z};
        for (int i = 0; i < 3; ++i) g_hand_xf.to[i] = laser_to[i] + (to[i] - laser_to[i]) * nh.blend;
        g_hand_xf.on = true;
        g_held_xf = g_hand_xf;  // the carried object goes with the drawn hand
        memcpy(g_held_xf.u, u, sizeof(u));
        // A left god hand is the right one mirrored, which a thing in it can't be: made a proper
        // rotation by turning the hand's side axis, its "up" for the thing, it was held upside down
        // (playtest 2026-11-07: a villager, a tree). Turned about the fingers instead (the side
        // and the palm's normal both), it stays upright.
        if (mirrored_pass()) {
            float th[3][3];
            for (int c = 0; c < 3; ++c) th[0][c] = -t[0][c], th[1][c] = -t[1][c], th[2][c] = t[2][c];
            float fh[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) fh[i][j] = th[0][i] * u[0][j] + th[1][i] * u[1][j] + th[2][i] * u[2][j];
            partial_rotation(fh, nh.blend, g_held_xf.r);
        }
        // The hand itself also takes a constant size to the player, whatever the world's scale
        // and wherever the game's own hand is (the game sizes it by its distance from the camera).
        const float len = wrist_scale(root);
        if (nh.axis_len > 0 && len > 1e-9f) {
            const float k = 1.0f + (nh.axis_len / len - 1.0f) * nh.blend;
            for (auto& row : g_hand_xf.r)
                for (float& v : row) v *= k;
            g_held_xf.offset_scale = k;  // what it holds stays in the palm, not at the wrist
        }
        mirror_main_hand(u);
        return;
    }
    if (laser_on) {
        memcpy(g_hand_xf.r, laser_r, sizeof(laser_r));
        memcpy(g_hand_xf.to, laser_to, sizeof(laser_to));
        g_hand_xf.on = true;
    }
    mirror_main_hand(u);
}

// Moving the whole hand: its skeleton (CHand+0x47F0, the array the draw uses too) has 22 bones,
// the mesh uses 20 of them.
constexpr int kHandBones = 22;
using SavedBones = std::array<float, kHandBones * 12>;
void move_bones(float* bones, const HandXform& xf, SavedBones& saved) {
    memcpy(saved.data(), bones, sizeof(saved));
    const auto& q = xf.r;
    for (int i = 0; i < kHandBones; ++i) {
        const float* copy = saved.data() + i * 12;
        float* b = bones + i * 12;
        for (int r = 0; r < 4; ++r) {
            float v[3] = {copy[r * 3], copy[r * 3 + 1], copy[r * 3 + 2]};
            if (r == 3) v[0] -= xf.from[0], v[1] -= xf.from[1], v[2] -= xf.from[2];
            for (int c = 0; c < 3; ++c) b[r * 3 + c] = q[c][0] * v[0] + q[c][1] * v[1] + q[c][2] * v[2] + (r == 3 ? xf.to[c] : 0.0f);
        }
    }
}
float* hand_bones() {
    if (!g_hand) return nullptr;
    float* bones = *reinterpret_cast<float**>(static_cast<char*>(g_hand) + 0x47F0);
    return bones && !IsBadWritePtr(bones, sizeof(SavedBones)) ? bones : nullptr;
}

// The hand's shadow. CHand::PrepareForDrawing ends with the hand object's dynamic-shadow update
// (fn_00814FD0, __thiscall(LH3DObject*)), which places the shadow from the object's own position
// (+0x38, its matrix's translation). That is the game's hand, not the one drawn at the controller
// or on the laser point, so the shadow lay apart from the hand. While the hand's shadow is
// updated its position is the drawn hand's.
constexpr uintptr_t kShadowUpdate = 0x814FD0;
ThisFn o_shadow_update = nullptr;
bool g_hand_shadow = true;
int g_shadow_log = 0;  // debug (stereo shadow_log N): where the hand's shadow is cast from, N times
void __fastcall shadow_update_detour(void* obj, void*) {
    const bool is_hand = g_hand_shadow && g_hand && obj == *reinterpret_cast<void**>(static_cast<char*>(g_hand) + 0x482C);
    float* bones = is_hand ? hand_bones() : nullptr;
    if (!bones) {
        o_shadow_update(obj);
        return;
    }
    // (The shadow is built from the skeleton and the object's position, +0x38.)
    g_hand_state_drawing = *reinterpret_cast<const uint32_t*>(static_cast<char*>(g_hand) + 0x4878);
    const HandXform held = g_held_xf;
    decide_hand_xform(bones);
    g_held_xf = held;  // (that is for the draw to decide)
    // (Also the loop of a leash in the hand's transform, this frame's: taken at the hand's own draw,
    // the left eye's loop, drawn before the hand, used the frame before's and jiggled as the hand
    // moved; player, 2026-10-23.)
    if (g_hand_xf.on) g_last_hand_xf = g_hand_xf;
    if (!g_hand_xf.on) {
        if (g_shadow_log > 0) --g_shadow_log, BWLOG("stereo: hand shadow at the game's hand (no transform)");
        o_shadow_update(obj);
        return;
    }
    // Moved to the drawn hand, but not turned: the game sizes the shadow's texture for its own hand
    // (palm down), and the drawn hand, upright or pointing ahead, cast a shadow past the texture's
    // edges, which stretched into streaks across the ground round it (player, 2026-10-19).
    HandXform xf = g_hand_xf;
    g_hand_xf.on = false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) xf.r[r][c] = r == c ? 1.0f : 0.0f;
    SavedBones saved;
    move_bones(bones, xf, saved);
    float* pos = static_cast<float*>(obj) + 0x38 / 4;
    float pos_saved[3];
    memcpy(pos_saved, pos, sizeof(pos_saved));
    const float v[3] = {pos[0] - xf.from[0], pos[1] - xf.from[1], pos[2] - xf.from[2]};
    for (int c = 0; c < 3; ++c) pos[c] = xf.r[c][0] * v[0] + xf.r[c][1] * v[1] + xf.r[c][2] * v[2] + xf.to[c];
    // Not up in the air with the drawn hand, though: cast from high above the land, along a low
    // sun, the shadow fell far off and smeared across the ground in long streaks, the higher the
    // worse (player, 2026-10-20). It is cast from the game's own height above the land (its hand
    // hovers just over it), straight below the drawn hand, as in the flat game.
    const float lift = pos_saved[1] - hand::ground_at(pos_saved[0], pos_saved[2]);
    const float dy = std::clamp(lift, 0.0f, 3.0f) + hand::ground_at(pos[0], pos[2]) - pos[1];
    if (g_set.shadow_on_ground && dy < 0) {
        pos[1] += dy;
        for (int b = 0; b < kHandBones; ++b) bones[b * 12 + 10] += dy;
    }
    if (g_shadow_log > 0) {
        --g_shadow_log;
        float spread = 0, low = 1e9f, high = -1e9f;
        for (int k = 0; k < kHandBones; ++k) {
            const float* t = bones + k * 12 + 9;
            spread = std::max(spread, std::sqrt((t[0] - pos[0]) * (t[0] - pos[0]) + (t[1] - pos[1]) * (t[1] - pos[1]) + (t[2] - pos[2]) * (t[2] - pos[2])));
            low = std::min(low, t[1]), high = std::max(high, t[1]);
        }
        BWLOG("stereo: hand shadow cast from (%.2f, %.2f, %.2f) (the game's hand (%.2f, %.2f, %.2f), %.2f above the land; moved %.2f); "
              "bones within %.2f of it, heights %.2f..%.2f, the land below %.2f",
              pos[0], pos[1], pos[2], pos_saved[0], pos_saved[1], pos_saved[2], lift, g_set.shadow_on_ground && dy < 0 ? dy : 0.0f, spread, low,
              high, hand::ground_at(pos[0], pos[2]));
    }
    o_shadow_update(obj);
    memcpy(pos, pos_saved, sizeof(pos_saved));
    memcpy(bones, saved.data(), sizeof(saved));
}

// The object the hand carries: GInterfaceStatus (hand+0x4850) -> holding flag (+0x90) and
// LastPickedUpObject (+0x120) -> Object::Game3dObject (+0x40), an LH3DObject (matrix at +0x14).
void* held_object_3d() {
    if (!hand_alive()) return nullptr;
    return hand::held_object_3d_now();  // (not the game's "last picked up" alone: it is stale a moment after a pick-up)
}

// The 3D objects drawn in the last eye passes (LH3DObject's draw, fn_00813BA0): what the player can
// see, for the gravity gloves (player, 2026-11-07: they offered a sheep and a fence too far off to be
// drawn at all). Two frames' worth, swapped each frame.
std::vector<const void*> g_drawn_now, g_drawn_last;
uint32_t g_disciple_obj3d = 0;  // (this frame's disciple icon: the villager's 3D object, and the icon)
int g_disciple_type = -1;
ULONGLONG g_disciple_asked_ms = 0;  // (when the game last asked for the icon: Villager::ShowDiscipleIcon)
double g_drawn_swapped_at = 0;
void drawn_frame_begins() {
    g_drawn_last.swap(g_drawn_now);
    g_drawn_now.clear();
    std::sort(g_drawn_last.begin(), g_drawn_last.end());
    g_drawn_swapped_at = double(GetTickCount64()) / 1000.0;
}
void __fastcall obj_draw_detour(void* obj, void*) {
    if (g_current_eye >= 0 && g_drawn_now.size() < 20000) g_drawn_now.push_back(obj);
    const bool is_hand = g_hand && obj == *reinterpret_cast<void**>(static_cast<char*>(g_hand) + 0x482C);
    g_hand_xf.on = false;
    g_hand_state_drawing = is_hand ? *reinterpret_cast<const uint32_t*>(static_cast<char*>(g_hand) + 0x4878) : 0;
    g_hand_xf_wanted = is_hand && g_current_eye >= 0;
    g_drawing_hand = is_hand;
    // (Faded outside the player's influence, where it can't act: hand::hand_opacity.)
    const float opacity = is_hand && g_current_eye >= 0 ? hand::hand_opacity() : 1.0f;
    if (opacity < 0.99f) trace::set_ghost(opacity);
    const bool mirrored = is_hand && g_current_eye >= 0 && mirrored_pass();
    if (mirrored) trace::set_no_cull(true);  // (a left main hand: drawn mirrored)
    o_obj_draw(obj);
    if (mirrored) trace::set_no_cull(false);
    trace::set_ghost(0);
    // The other controller's hand: the god hand drawn once more, mirrored, at that controller.
    hand::NearHand second;
    if (is_hand && g_current_eye >= 0 && g_set.second_hand && hand::second_hand(second)) {
        g_second_hand_pass = true;
        g_hand_xf.on = false;
        g_hand_xf_wanted = true;
        const bool mirrored = mirrored_pass();
        if (mirrored) trace::set_no_cull(true);  // mirrored: its triangles wind the other way
        // (Faded with the main hand outside the influence: playtest 2026-11-04, only the main one did.)
        if (opacity < 0.99f) trace::set_ghost(opacity);
        o_obj_draw(obj);
        trace::set_ghost(0);
        if (mirrored) trace::set_no_cull(false);
        g_second_hand_pass = false;
    }
    g_drawing_hand = false;
    g_hand_xf.on = false;
    g_hand_xf_wanted = false;
    if (is_hand && g_bone_dump > 0) --g_bone_dump;
}
void __fastcall mesh_draw_detour(void* mesh, void*) {
    if (g_drawing_hand && g_bone_dump > 0) {
        const char* m = static_cast<const char*>(mesh);
        const int count = *reinterpret_cast<const int*>(m + 0x20);
        const char* list = *reinterpret_cast<const char* const*>(m + 0x24);
        const float* bones = *reinterpret_cast<float* const*>(kBoneArray);
        for (int i = 0; i < count && list && bones; ++i) {
            const int idx = *reinterpret_cast<const uint16_t*>(list + i * 4 + 2);
            const float* b = bones + idx * 12;
            BWLOG("bones: mesh %p part %d/%d bone %d [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.2f %.2f %.2f]", mesh, i,
                  count, idx, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
        }
    }
    if (g_drawing_hand && g_hand_xf_wanted) {
        if (!g_second_hand_pass) g_held_xf.on = false;
        if (const float* root = *reinterpret_cast<float* const*>(kBoneArray)) {
            if (!g_second_hand_pass && g_current_eye == 0) {
                watch_for_poses(g_hand_state_drawing, root);
                const bool was = g_squashed;
                static ULONGLONG since = 0;
                g_squashed = g_set.unsquash_hand && squash_ratio(root) < 0.97f;
                if (g_squashed && !was) since = GetTickCount64();
                if (!g_squashed && was) BWLOG("stereo: the hand's squash was drawn away (%llu ms, state %u)", GetTickCount64() - since, g_hand_state_drawing);
                if (!g_squashed) g_have_last_good = relative_pose(root, g_last_good);
            }
            decide_hand_xform(root);
            if (g_pose_log > 0 && g_current_eye == 0) {
                static const char* names[] = {"game", "open", "fist", "point"};
                BWLOG("stereo: hand pose: %s hand %s (state %u)", g_second_hand_pass ? "other" : "god", names[int(g_pose_now)], g_hand_state_drawing);
                if (g_second_hand_pass) --g_pose_log;
            }
        }
        g_hand_xf_wanted = false;  // decided once per hand draw
        if (!g_second_hand_pass) g_last_hand_xf = g_hand_xf;
    }
    const HandXform* xf = g_drawing_hand && g_hand_xf.on ? &g_hand_xf : nullptr;
    if (!xf) {
        o_mesh_draw(mesh);
        return;
    }
    const char* m = static_cast<const char*>(mesh);
    const int count = *reinterpret_cast<const int*>(m + 0x20);
    const char* list = *reinterpret_cast<const char* const*>(m + 0x24);
    float* bones = *reinterpret_cast<float**>(kBoneArray);
    std::vector<std::pair<int, std::array<float, 12>>> saved;
    float wrist[12] = {};  // (as it is before this draw moves it)
    if (bones) memcpy(wrist, bones, sizeof(wrist));
    if (g_squashed) unsquash(wrist);
    const auto& q = xf->r;
    for (int i = 0; i < count && bones && list; ++i) {
        const int idx = *reinterpret_cast<const uint16_t*>(list + i * 4 + 2);
        if (std::any_of(saved.begin(), saved.end(), [&](const auto& p) { return p.first == idx; })) continue;
        float* b = bones + idx * 12;
        std::array<float, 12> copy;
        memcpy(copy.data(), b, sizeof(float) * 12);
        saved.push_back({idx, copy});
        if (idx == 0 && g_squashed) memcpy(copy.data(), wrist, sizeof(wrist));
        posed_bone(idx, wrist, copy.data());  // (the fingers as the pose has them, if it has them)
        // Rows are local axes in world space (row-vector convention): rotate each, and the
        // translation about the wrist, then move the wrist.
        for (int r = 0; r < 4; ++r) {
            float v[3] = {copy[r * 3], copy[r * 3 + 1], copy[r * 3 + 2]};
            if (r == 3) v[0] -= xf->from[0], v[1] -= xf->from[1], v[2] -= xf->from[2];
            for (int c = 0; c < 3; ++c)
                b[r * 3 + c] = q[c][0] * v[0] + q[c][1] * v[1] + q[c][2] * v[2] + (r == 3 ? xf->to[c] : 0.0f);
        }
    }
    // fn_0084C3C0 culls triangles by screen winding unless bit 0 of the mesh's byte +5 is set
    // (double-sided). The mirrored second hand needs it set.
    uint8_t* flags = reinterpret_cast<uint8_t*>(mesh) + 5;
    const uint8_t saved_flags = *flags;
    if (mirrored_pass()) *flags |= 1;
    o_mesh_draw(mesh);
    *flags = saved_flags;
    for (const auto& p : saved) memcpy(bones + p.first * 12, p.second.data(), sizeof(float) * 12);
}
// The 3D clouds are small meshes drawn by fn_007FA300, which copies a global rotation
// (0xEA1C98, rows [right; -forward; up] of the *game* camera, set when the game camera updates)
// into each cloud so it faces that camera. In the headset that made them swing with the game
// camera (or show edge-on). During the eye passes each cloud instead faces the head (the middle
// of the two eyes, with world up, so no roll), so head rotation doesn't move them at all. (Facing
// each eye on its own, a billboard near the player, such as a tapped tutorial sign, was turned
// differently in each eye: double vision.)
constexpr uintptr_t kBillboardDraw = 0x7FA300;    // __thiscall(object*)
constexpr uintptr_t kBillboardRotation = 0xEA1C98;  // float[9]
using BillboardFn = void(__thiscall*)(void* obj);
BillboardFn o_billboard = nullptr;
int g_current_eye_for_billboards = -1;
int g_cloud_log = 0;  // debug (stereo cloud_log N): the next N billboards drawn (the clouds), their size and distance
Vec3 g_billboard_from;  // the middle of the eyes, this frame
struct Listener {        // the head in the world, for the sound (see listener_update_detour)
    bool valid = false;
    Vec3 pos, fwd, up;
};
Listener g_listener;
int g_bb_diag = 0;  // (the feedback snapshot: the next N billboards drawn outside the eye passes, logged)
void __fastcall billboard_detour(void* obj, void*) {
    if (g_current_eye_for_billboards < 0) {
        // (Effects tilting with the head in the headset, not in the harness (playtest 2026-11-07, 16:11):
        // is it billboards drawn outside the eye passes, facing the game camera?)
        if (g_bb_diag > 0) {
            --g_bb_diag;
            const float* r = reinterpret_cast<const float*>(kBillboardRotation);
            BWLOG("stereo: billboard %p drawn outside the eye passes (facing the game camera: right %.2f %.2f %.2f, up %.2f %.2f %.2f)", obj, r[0], r[1],
                  r[2], r[6], r[7], r[8]);
        }
        o_billboard(obj);
        return;
    }
    const auto& p = *reinterpret_cast<const game::LHPoint*>(static_cast<char*>(obj) + 0x38);
    // (Debug, stereo cloud_log N: the next N billboards drawn: the clouds, and other sprites drawn the
    // same way, all with the same 29-unit mesh, so its size isn't theirs. Looked at for the big low
    // cloud through the view (playtest 2026-11-01); a skip by that size hid a small sprite at the
    // village's worship site instead.)
    if (g_cloud_log > 0 && g_current_eye_for_billboards == 0) {
        --g_cloud_log;
        const float d = vr::length(Vec3{p.x - g_billboard_from.x, p.y - g_billboard_from.y, p.z - g_billboard_from.z});
        BWLOG("stereo: billboard %p at (%.0f, %.0f, %.0f), %.0f from the eye (%.0f, %.0f, %.0f)", obj, p.x, p.y, p.z, d, g_billboard_from.x,
              g_billboard_from.y, g_billboard_from.z);
    }
    float* rot = reinterpret_cast<float*>(kBillboardRotation);
    float saved[9];
    memcpy(saved, rot, sizeof(saved));
    const Vec3 fwd{p.x - g_billboard_from.x, p.y - g_billboard_from.y, p.z - g_billboard_from.z};  // up stays world up: no roll
    if (vr::length(fwd) > 1e-3f) {
        const Basis b = vr::basis_look(fwd);
        const float rows[9] = {b.right.x, b.right.y, b.right.z, -b.fwd.x, -b.fwd.y, -b.fwd.z, b.up.x, b.up.y, b.up.z};
        memcpy(rot, rows, sizeof(rows));
    }
    o_billboard(obj);
    memcpy(rot, saved, sizeof(saved));
}

// The helper spirits (the good and evil advisers) hover where the game puts them on its screen:
// a point in -1..1 of the view's half-width (in both directions), often near the edge, which
// fn_005BD2A0 turns into a point in the world in front of the camera. Done against the VR eye's
// view, that point came out far above and beside the view (in the headset, mostly their sparkles
// showed at the edge of sight; player, 2026-10-19). In VR the point is placed here instead: as
// far in front of the head as the game's (about 0.7 m), in the direction that hover has in the
// flat game's view (70 degrees wide), pulled toward the middle by helper_spread_percent.
constexpr uintptr_t kHelperHoverTo3D = 0x5BD2A0;  // __thiscall(HelpDude*, LHPoint* out, float hx, float hy, float z, bool) -> out
constexpr uintptr_t kEngineCamera = 0xEA1DB8;     // LH3DTech::g_camera: float position[3], focus[3]
using HoverTo3DFn = void*(__thiscall*)(void*, void*, float, float, float, int);
HoverTo3DFn o_hover_to_3d = nullptr;
int g_helper_log = 0;
void* __fastcall hover_to_3d_detour(void* dude, void*, void* out, float hx, float hy, float z, int flag) {
    void* r = o_hover_to_3d(dude, out, hx, hy, z, flag);
    auto& p = *static_cast<game::LHPoint*>(out);
    const float* cam = reinterpret_cast<const float*>(kEngineCamera);
    const Vec3 game_point{p.x, p.y, p.z};
    // (On the cinema screen the game's own frame is shown, through its scripted camera: the game's
    // point is right there. Placed at the VR head, the spirits were far off the scene and never seen
    // in the new game's opening; player, 2026-11-02.)
    if (g_set.enabled && g_set.helper_spread > 0 && g_listener.valid && !g_cinema_on) {
        // (The game's depth: its points lie on a plane this far in front of the camera.)
        const Vec3 cam_pos{cam[0], cam[1], cam[2]};
        const float depth = vr::dot(game_point - cam_pos, vr::normalize(Vec3{cam[3], cam[4], cam[5]} - cam_pos));
        const float t = 0.7002f * float(g_set.helper_spread) / 100.0f;  // (tan 35 degrees: the flat view's edge)
        const Vec3 fwd = g_listener.fwd, up = g_listener.up, right = vr::normalize(vr::cross(up, fwd));
        const Vec3 at = g_listener.pos + (fwd + right * (hx * t) - up * (hy * t)) * depth;
        p.x = at.x, p.y = at.y, p.z = at.z;
    }
    if (g_helper_log > 0) {  // debug: stereo helper_log N
        --g_helper_log;
        BWLOG("stereo: helper spirit %p at hover (%.2f, %.2f) z %.1f flag %d: the game's (%.1f, %.1f, %.1f), drawn at (%.1f, %.1f, %.1f); "
              "camera (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), eye %d, from %p",
              dude, hx, hy, z, flag & 0xFF, game_point.x, game_point.y, game_point.z, p.x, p.y, p.z, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5],
              g_current_eye, _ReturnAddress());
    }
    return r;
}

// The leash's loop at the hand (shown while the leash is in the hand) is an object of its own,
// drawn through the general object draw (fn_0080DB30) with one of the three leash meshes
// (0xC5E3D8/DC/E0, from Data\MISC\leash.l3d) at +0x7C, placed at the game's own wrist. In the
// eye passes it gets the god hand's transform (game wrist -> drawn wrist), so it is on the VR hand.
ThisFn o_generic_draw = nullptr;
bool g_loop_follows_hand = true;
// The loop in the hand (the leash mesh nearest the game's wrist last frame). The engine decides
// whether to draw it from where it really is, at the game's wrist on the land below the VR hand;
// in one eye that was often off the view, and the loop wasn't drawn there at all (player,
// 2026-10-24: the loop still not on the hand). It is kept on screen (region_detour).
const void* g_loop_obj = nullptr;
bool g_loop_active = false;  // the loop was moved onto the hand last frame (kept on screen this one)
// A leash only chosen (at the temple), not yet on the creature, also has its loop at the game's
// wrist, and the game doesn't count it as the leash in the hand (player, 2026-10-24: the loop sat
// near the ground then). A leash mesh near the wrist that moves with the wrist is that loop; the
// temple's spare leashes never move.
struct LoopTrack {
    const void* obj = nullptr;
    float pos[3] = {}, wrist[3] = {};
    bool follows = false;
};
LoopTrack g_loop_track;
int g_leash_draw_log = 0;  // debug: log the next N draws of a leash mesh
void __fastcall generic_draw_detour(void* obj, void*) {
    float* m = static_cast<float*>(obj) + 0x14 / 4;  // 3x3 rows, then the position at +0x38
    if (g_wrist_draw_log > 0 && g_current_eye == 0 && !IsBadReadPtr(obj, 0x80))
        log_near_wrist("generic draw", obj, *reinterpret_cast<const uint32_t*>(static_cast<char*>(obj) + 0x7C));
    bool moved = false;
    float saved[12];
    if (g_loop_follows_hand && g_current_eye >= 0 && g_last_hand_xf.on && !IsBadReadPtr(obj, 0x80)) {
        const uint32_t mesh = *reinterpret_cast<const uint32_t*>(static_cast<char*>(obj) + 0x7C);
        const uint32_t* loops = reinterpret_cast<const uint32_t*>(0xC5E3D8);
        if (mesh && (mesh == loops[0] || mesh == loops[1] || mesh == loops[2])) {
            const HandXform& xf = g_last_hand_xf;
            const float d[3] = {m[9] - xf.from[0], m[10] - xf.from[1], m[11] - xf.from[2]};
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (g_leash_draw_log > 0) {
                --g_leash_draw_log;
                BWLOG("stereo: leash mesh draw %p mesh %08X at (%.2f, %.2f, %.2f), %.2f from the game's wrist", obj, mesh, m[9], m[10],
                      m[11], dist);
            }
            // Only the loop in the hand, a fraction of a unit from the game's wrist: the temple's
            // spare leashes (LeashObj, tapped to choose another) use the same meshes, and so does
            // an icon drawn in camera space.
            // (And only the leash mesh nearest the game's wrist, while a leash is in the hand: the
            // temple's spare leashes within 5 units of the game's hand, which is on the ground below
            // the player's, were carried off with the hand. The nearest is the one found in the
            // frame before; a leather leash's loop sits further from the wrist than a rope's.
            // Player, 2026-10-23.)
            const void*& nearest = g_loop_obj;
            static const void* nearest_next = nullptr;
            static float nearest_d = 1e9f;
            static unsigned frame_seen = 0;
            if (g_frame_counter != frame_seen) frame_seen = g_frame_counter, nearest = nearest_next, nearest_next = nullptr, nearest_d = 1e9f;
            if (dist < nearest_d) nearest_d = dist, nearest_next = obj;
            // (Does it move with the wrist? Checked once a frame, on the left eye's draw.)
            if (obj == nearest && g_current_eye == 0) {
                LoopTrack& lt = g_loop_track;
                if (lt.obj != obj || dist > 8.0f) {
                    lt = {};
                    lt.obj = obj;
                } else {
                    float dp = 0, dw = 0, diff = 0;
                    for (int i = 0; i < 3; ++i) {
                        const float a = m[9 + i] - lt.pos[i], b = xf.from[i] - lt.wrist[i];
                        dp += a * a, dw += b * b, diff += (a - b) * (a - b);
                    }
                    if (dw > 0.05f * 0.05f && diff < 0.09f * dw + 0.0004f) {
                        if (!lt.follows) BWLOG("stereo: the leash mesh %p moves with the game's wrist: the loop of a leash in the hand", obj);
                        lt.follows = true;
                    } else if (dw > 0.05f * 0.05f && dp < 1e-8f) {
                        lt.follows = false;  // (the wrist moved and it stayed: a leash lying somewhere)
                    }
                }
                memcpy(lt.pos, &m[9], sizeof(lt.pos));
                memcpy(lt.wrist, xf.from, sizeof(lt.wrist));
            }
            const bool loop_in_hand = hand::leash_in_hand_now() || (g_loop_track.obj == obj && g_loop_track.follows);
            {   // (Why a leash mesh at the hand wasn't moved: for the player's sessions, now and then.)
                static double why_at = -10;
                if (dist < 5.0f && !(obj == nearest && loop_in_hand) && g_current_eye == 0 && (GetTickCount64() / 1000.0) - why_at > 5.0) {
                    why_at = (GetTickCount64() / 1000.0);
                    BWLOG("stereo: a leash mesh %.2f from the game's wrist is left where it is (nearest %d, leash in the hand %d, follows the wrist %d)",
                          dist, obj == nearest, hand::leash_in_hand_now(), g_loop_track.obj == obj && g_loop_track.follows);
                }
            }
            if (dist < 5.0f && obj == nearest && loop_in_hand) {
                memcpy(saved, m, sizeof(saved));
                for (int r = 0; r < 4; ++r) {
                    float v[3] = {saved[r * 3], saved[r * 3 + 1], saved[r * 3 + 2]};
                    if (r == 3) v[0] -= xf.from[0], v[1] -= xf.from[1], v[2] -= xf.from[2];
                    for (int c = 0; c < 3; ++c)
                        m[r * 3 + c] = xf.r[c][0] * v[0] + xf.r[c][1] * v[1] + xf.r[c][2] * v[2] + (r == 3 ? xf.to[c] : 0.0f);
                }
                moved = true;
            }
            if (obj == nearest && g_current_eye == 0) g_loop_active = moved;
            if (g_leash_draw_log > 0) {
                --g_leash_draw_log;
                BWLOG("stereo: leash loop %s: %p %.2f from the wrist, nearest last frame %p, leash in the hand %d, eye %d",
                      moved ? "moved" : "left", obj, dist, nearest, hand::leash_in_hand_now(), g_current_eye);
            }
        }
    }
    // A miracle's effect drawn at the palm while the hand is charged, bigger than a hand: the water's
    // cloud is a sphere 2.3 m across around the hand (the game's size, for its camera far off), and
    // held up in VR it filled the view (found 2026-11-01). Shrunk about its middle to 25 cm across.
    if (!moved && g_set.miracle_on_palm && g_current_eye >= 0 && g_hand && !IsBadReadPtr(obj, 0x80) &&
        *reinterpret_cast<const uint32_t*>(static_cast<char*>(g_hand) + 0x4878) == 8) {
        hand::NearHand nh;
        if (hand::near_hand(nh) && nh.blend >= 0.5f) {
            const float scale = hand::rig_scale();
            const float d = std::sqrt((m[9] - nh.palm.x) * (m[9] - nh.palm.x) + (m[10] - nh.palm.y) * (m[10] - nh.palm.y) +
                                      (m[11] - nh.palm.z) * (m[11] - nh.palm.z));
            using InfoFn = const float*(__thiscall*)(const void*);
            void* const* vtbl = *static_cast<void* const* const*>(obj);
            const auto info_fn = vtbl && !IsBadReadPtr(vtbl + 0xF8 / 4, 4) ? reinterpret_cast<InfoFn>(vtbl[0xF8 / 4]) : nullptr;
            const float* f = d < 0.1f * scale && info_fn ? info_fn(obj) : nullptr;
            const float row = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            const float radius = f && !IsBadReadPtr(f, 0x34) ? f[12] * row : 0.0f, cap = g_set.miracle_effect_m * scale;
            if (radius > cap && cap > 0) {
                memcpy(saved, m, sizeof(saved));
                const float k = cap / radius;
                // (About its bounding sphere's middle: info +0x18, in its own axes.)
                const float c[3] = {f[6] * m[0] + f[7] * m[3] + f[8] * m[6], f[6] * m[1] + f[7] * m[4] + f[8] * m[7], f[6] * m[2] + f[7] * m[5] + f[8] * m[8]};
                for (int i = 0; i < 9; ++i) m[i] *= k;
                // (Its middle on the palm, resting on it (the cloud had sat above the palm; playtest
                // 2026-11-01), with the panel's miracle offsets.)
                const hand::HeldPlace mp = hand::held_place(2);
                const vr::Vec3 ny = vr::normalize(nh.y), nz = vr::normalize(nh.z);
                const vr::Vec3 want = nh.palm + ny * (cap * std::max(0.0f, mp.out + 0.2f)) + nz * (mp.fwd * scale) + vr::Vec3{0, mp.up * scale, 0};
                m[9] = want.x - c[0] * k, m[10] = want.y - c[1] * k, m[11] = want.z - c[2] * k;
                moved = true;
                static const void* told = nullptr;
                if (told != obj) told = obj, BWLOG("stereo: a miracle's effect at the palm %.1f units round: drawn %.1f", radius, cap);
            }
        }
    }
    o_generic_draw(obj);
    if (moved) memcpy(m, saved, sizeof(saved));
}
int g_handinfo_pending = 0;  // debug: log hand state on the next N PrepareForDrawing calls
int g_handtrace_left = 0;    // debug: one compact line per frame (hand vs cursor) for N frames
// LH3DBoundingBox::CheckRegionOnScreen (0x868C80) and its SSE twin (0x7AC640) decide whether an
// object is on screen and under the cursor (the flags at 0xEA1AD0 / 0xEA1AF0 that gate picking).
// They compare against the near plane too, and with the eye pass's nearer plane almost nothing
// reached the picker (226 reports in 3 s instead of 11000). They get the engine's own value.
struct EngineNear {
    float pass_near;
    EngineNear() : pass_near(game::info_transform().near_clip) {
        if (g_engine_near > 0) game::info_transform().near_clip = g_engine_near;
    }
    ~EngineNear() { game::info_transform().near_clip = pass_near; }
};
constexpr uintptr_t kCheckRegionOnScreen = 0x868C80;     // __thiscall(LH3DBoundingBox*, LH3DObject*)
constexpr uintptr_t kCheckRegionOnScreenSse = 0x7AC640;  // __fastcall(LH3DBoundingBox*, LH3DObject*)
using RegionFn = uintptr_t(__thiscall*)(void* box, void* obj);
using RegionSseFn = uintptr_t(__fastcall*)(void* box, void* obj);
RegionFn o_region = nullptr;
RegionSseFn o_region_sse = nullptr;
// The detail level and fade of each object come from its size on the screen and its distance as
// CheckRegionOnScreen works them out (0xC37EA0, LH3DObject::g_last_distance 0xEA1AF4). The eyes
// see each object a little differently, so one near a threshold could take one level in one eye
// and another in the other: far trees looked different in each eye while the head turned (player,
// 2026-10-23). The right eye takes the left eye's values for each object it also drew.
constexpr uintptr_t kLastDistance = 0xEA1AF4;
// (By the object and where it is as it's checked: one 3D object can be drawn in several places in
// a pass. The villagers' carried things are one shared object per kind (CarriedObject, fn_0051BAF0),
// moved to each villager in turn: by the object alone, every plank in the right eye took the last
// plank's values, and one far away hid the near ones, which blinked; player, 2026-10-27.)
struct SeenSize {
    float size, distance;
};
std::unordered_map<uint64_t, SeenSize> g_sizes_left;
std::unordered_map<const void*, uint64_t> g_sizes_obj;  // (the left eye: each object's first place this frame)
unsigned g_sizes_shared = 0, g_sizes_missed = 0, g_sizes_far = 0;  // (per 5 s: checks of an object already checked elsewhere; right-eye checks without a left one)
bool g_size_by_place = true;  // (debug: stereo size_by_place 0, by the object alone as before)
unsigned g_draws_eye[2] = {};  // (per 5 s: each eye's draws)
uint64_t size_key(const void* obj) {
    if (!g_size_by_place) return uint64_t(reinterpret_cast<uintptr_t>(obj));
    const float* m = reinterpret_cast<const float*>(static_cast<const char*>(obj) + 0x14);
    const auto q = [](float v) { return uint32_t(int32_t(std::floor(v * 4.0f))); };  // (a quarter of a unit)
    const uint32_t h = q(m[9]) * 73856093u ^ q(m[10]) * 19349663u ^ q(m[11]) * 83492791u;
    return uint64_t(reinterpret_cast<uintptr_t>(obj)) | (uint64_t(h) << 32);
}
int g_size_log = 0;           // debug (stereo size_log N): the next N objects' size on the screen, distance, and the two's product
// Small things keep their detail and stay drawn further out (player, 2026-11-07: sheep and fences faded out
// close by, as the adaptive detail took everything down to the game's own 100% in busy views). By their size
// in the world (size on the screen x distance; measured 2026-11-22: sheep 90-140, people and cows 160-550,
// scaffolds 200-500, fences and the like 460-1850, most rocks about 1000; trees from 400, median 2100; houses
// 1700 and up): under [stereo] small_size (600), small_detail_percent (200%); under small_floor_size (1600),
// never below detail_distance_percent (150%), whatever the adaptive detail. Cost over the village from above:
// +0.25 ms for both eyes (1200 draws -> 1290).
float g_small_w = 600.0f, g_small_boost = 2.0f, g_small_floor_w = 1600.0f;
void after_region(const void* obj, uintptr_t r) {
    if (!r) return;
    float& size = *reinterpret_cast<float*>(kDetailScale);
    float& dist = *reinterpret_cast<float*>(kLastDistance);
    const float w = size * dist;  // (its size in the world, near enough)
    if (g_size_log > 0 && g_current_eye == 0) --g_size_log, BWLOG("size: obj %p size %.5f distance %.1f product %.3f", obj, size, dist, w);
    if (g_current_eye >= 0 && w < g_small_floor_w)
        size *= std::max(g_size_boost, w < g_small_w ? g_small_boost : float(g_set.detail_distance) / 100.0f);
    else if (g_size_boost != 1.0f) size *= g_size_boost;
    if (g_current_eye == 0) {
        const auto [it, added] = g_sizes_left.try_emplace(size_key(obj), SeenSize{size, dist});
        if (!added) it->second = {size, dist};
        if (!added) return;
        const auto [o, first] = g_sizes_obj.try_emplace(obj, it->first);
        if (!first && o->second != it->first) {  // (the same object checked in another place)
            ++g_sizes_shared;
            static int logged = 0;
            if (logged < 12) {  // (which: one of the villagers' carried things, CarriedObject's table at 0xC5E19C?)
                int kind = -1;
                for (int i = 0; i < 16; ++i)
                    if (reinterpret_cast<void* const*>(0xC5E19C)[i] == obj) kind = i;
                if (kind >= 0 || logged < 4) ++logged, BWLOG("stereo: object %p checked in several places in a pass (carried kind %d)", obj, kind);
            }
        }
    } else if (g_current_eye == 1 && g_set.same_trees) {
        if (auto it = g_sizes_left.find(size_key(obj)); it != g_sizes_left.end()) {
            if (std::fabs(it->second.distance - dist) > 2.0f + 0.1f * dist)  // (a carried thing given another place's values)
                for (int i = 0; i < 16; ++i)
                    if (reinterpret_cast<void* const*>(0xC5E19C)[i] == obj) ++g_sizes_far;
            size = it->second.size, dist = it->second.distance;
        }
        else
            ++g_sizes_missed;
    }
}
int g_region_loop_log = 0;  // debug: log the loop's region checks (leash_draws N also sets it)
uintptr_t loop_on_screen(const void* obj, uintptr_t r) {
    if (!obj || obj != g_loop_obj || g_current_eye < 0 || !g_loop_follows_hand || !(hand::leash_in_hand_now() || g_loop_active)) return r;
    if (g_region_loop_log > 0) --g_region_loop_log, BWLOG("stereo: the loop's region check in eye %d: %u%s", g_current_eye, unsigned(r), r ? "" : " (kept on screen)");
    return r ? r : 1;
}
uintptr_t __fastcall region_detour(void* box, void*, void* obj) {
    EngineNear near_plane;
    const uintptr_t r = o_region(box, obj);
    after_region(obj, r);
    return loop_on_screen(obj, r);
}
uintptr_t __fastcall region_sse_detour(void* box, void* obj) {
    EngineNear near_plane;
    const uintptr_t r = o_region_sse(box, obj);
    after_region(obj, r);
    return loop_on_screen(obj, r);
}

void __fastcall hand_prepare_detour(void* hand, void*) {
    g_hand = hand;
    hand::set_game_hand(hand);
    hand::offer_at_palm();
    if (g_current_eye <= 0) {
        // The game places its hand (and from it, what can be picked) with the near plane's
        // distance: it must see its own value, not the eye pass's nearer plane.
        EngineNear near_plane;
        o_hand_prepare(hand);
    }
    if (g_handtrace_left > 0 && g_current_eye <= 0) {
        --g_handtrace_left;
        const auto* b = static_cast<const char*>(hand);
        const auto& pos = *reinterpret_cast<const game::LHPoint*>(b + 0x78);
        const auto& sp = *reinterpret_cast<const game::LHCoord*>(b + 0x485C);
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        const float* m = nullptr;
        if (const char* obj = *reinterpret_cast<const char* const*>(b + 0x482C)) m = reinterpret_cast<const float*>(obj + 0x14);
        BWLOG("handtrace %.4f %d %d %.4f %.4f %.4f heading %.3f R0 %.4f %.4f %.4f R1 %.4f %.4f %.4f R2 %.4f %.4f %.4f",
              double(t.QuadPart) / double(f.QuadPart), sp.x, sp.y, pos.x, pos.y, pos.z, *reinterpret_cast<const float*>(b + 0x84),
              m ? m[0] : 0.f, m ? m[1] : 0.f, m ? m[2] : 0.f, m ? m[3] : 0.f, m ? m[4] : 0.f, m ? m[5] : 0.f, m ? m[6] : 0.f,
              m ? m[7] : 0.f, m ? m[8] : 0.f);
    }
    if (g_handinfo_pending > 0) {
        --g_handinfo_pending;
        const auto* b = static_cast<const char*>(hand);
        const auto& pos = *reinterpret_cast<const game::LHPoint*>(b + 0x78);
        const auto& sp = *reinterpret_cast<const game::LHCoord*>(b + 0x485C);
        const auto& c = game::camera();
        const auto& t = game::info_transform();
        BWLOG("handinfo: eye %d hand %p state %u pos (%.3f, %.3f, %.3f) screen (%d, %d) | cam (%.3f, %.3f, %.3f) foc (%.3f, %.3f, %.3f) res %dx%d",
              g_current_eye, hand, *reinterpret_cast<const uint32_t*>(b + 0x4878), pos.x, pos.y, pos.z, sp.x, sp.y, c.pos.x, c.pos.y, c.pos.z, c.foc.x, c.foc.y, c.foc.z,
              t.resolution.x, t.resolution.y);
        if (const char* st = *reinterpret_cast<const char* const*>(b + 0x4850)) {  // GInterfaceStatus
            const auto& tv = *reinterpret_cast<const game::LHPoint*>(st + 0x44);
            const auto& hv = *reinterpret_cast<const game::LHPoint*>(st + 0x10C);
            const auto& cp = *reinterpret_cast<const game::LHPoint*>(st + 0xB0);
            const auto& ha = *reinterpret_cast<const game::LHPoint*>(st + 0x68);
            const auto& hp = *reinterpret_cast<const game::LHPoint*>(st + 0x5C);
            if (const char* obj = *reinterpret_cast<const char* const*>(b + 0x482C)) {
                const float* m = reinterpret_cast<const float*>(obj + 0x14);
                const auto& w = game::world_to_camera().m;
                float rc[3][3];  // hand rows expressed in camera axes: R * C
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) rc[r][c] = m[r * 3] * w[0][c] + m[r * 3 + 1] * w[1][c] + m[r * 3 + 2] * w[2][c];
                BWLOG("handinfo: hand R [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f] R*C [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
                      m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], rc[0][0], rc[0][1], rc[0][2], rc[1][0], rc[1][1],
                      rc[1][2], rc[2][0], rc[2][1], rc[2][2]);
            }
            BWLOG("handinfo: hand_angles (%.4f, %.4f, %.4f) status_hand_pos (%.3f, %.3f, %.3f)", ha.x, ha.y, ha.z, hp.x, hp.y, hp.z);
            if (const char* h3d = static_cast<const char*>(held_object_3d())) {
                const float* hm = reinterpret_cast<const float*>(h3d + 0x14);
                BWLOG("handinfo: held object at (%.3f, %.3f, %.3f) | game put it at (%.3f, %.3f, %.3f), moved to (%.3f, %.3f, %.3f)",
                      hm[9], hm[10], hm[11], g_held_game_pos[0], g_held_game_pos[1], g_held_game_pos[2], g_held_moved_pos[0],
                      g_held_moved_pos[1], g_held_moved_pos[2]);
            }
            BWLOG("handinfo: holding %d throw_vel (%.3f, %.3f, %.3f) hand_vel (%.3f, %.3f, %.3f) iface_cam (%.2f, %.2f, %.2f) picked %p",
                  int(st[0x90]), tv.x, tv.y, tv.z, hv.x, hv.y, hv.z, cp.x, cp.y, cp.z, *reinterpret_cast<void* const*>(st + 0x120));
        }
    }
}
void* held_object_3d();
// In near mode the carried object goes with the drawn hand: after the game has put it at its own
// hand, apply the same transform as the hand's (about the game's wrist, to the drawn wrist). The
// game drops it from there too. (Trees and other objects have their own renderers; moving the
// object itself covers them all.)
// The carried thing, the same in both eyes. The game puts it at its own hand (on the ground below
// the player's) during the left eye's pass, before the trees are drawn, and it was moved to the
// player's hand later in that pass: the left eye's trees saw it on the ground, the right eye's in
// the hand. Trees between the eye and a carried thing turn see-through, so the trees below looked
// different in each eye (player, 2026-10-18, carrying a tree). The right eye's pass now repeats the
// left's: the thing back where the game put it at the start, then into the hand at the same point.
struct HeldPlaced {
    void* obj3d = nullptr;
    float game_m[12] = {}, m[12] = {};  // where the game put it, and where it went in the hand
    bool moved = false;
};
HeldPlaced g_held_placed;
void held_to_game_place() {  // (the right eye's pass begins)
    // (Only while it is in the hand. A thing thrown or let go of is still the game's held object
    // for a few frames while it flies from the hand: put back at the game's place, under the hand,
    // and not into the hand again, the right eye drew it there, out of sight; player, 2026-10-19.)
    if (!g_held_placed.moved) return;
    if (!g_held_placed.obj3d || static_cast<void*>(held_object_3d()) != g_held_placed.obj3d) return;
    memcpy(static_cast<char*>(g_held_placed.obj3d) + 0x14, g_held_placed.game_m, sizeof(g_held_placed.game_m));
}
bool g_held_follow = true;  // debug: the carried thing goes with the drawn hand (stereo "held_follow 0": stays at the game's)
// Each new hold (the hand's held object changes to another, or from none), counted once a frame
// whether or not the thing is drawn: its place in the hand is taken afresh only then. (It was taken
// afresh after 300 ms without being drawn, as when the hand turned far and the game's view lost
// it, and the game's offset then was another: the thing jumped on the palm; playtest 2026-10-31.)
void* g_held_now = nullptr;
unsigned g_held_pickups = 0;
void held_track() {
    void* h = held_object_3d();
    if (h != g_held_now) {
        g_held_now = h;
        if (h) ++g_held_pickups;
    }
}
int g_flight_log = 0;  // debug (stereo flight_log N): the gloves' pull, N frames of its flight
int g_held_log = 0;  // debug (stereo held_log N): the carried thing's place and axes in the palm's frame, N frames
void __fastcall hand_held_detour(void* hand, void*) {
    if (g_current_eye > 0) {  // (the right eye: into the hand where the left eye's pass put it there)
        if (g_held_placed.moved && g_held_placed.obj3d && static_cast<void*>(held_object_3d()) == g_held_placed.obj3d)
            memcpy(static_cast<char*>(g_held_placed.obj3d) + 0x14, g_held_placed.m, sizeof(g_held_placed.m));
        return;
    }
    o_hand_held(hand);
    if (g_current_eye == 0) g_held_placed = {};
    if (g_current_eye != 0 || !g_held_xf.on || !g_held_follow) return;
    if (hand::held_unnamed()) return;  // (the game's "held" is the last thing it held: left where it is)
    char* obj3d = static_cast<char*>(held_object_3d());
    if (!obj3d) return;
    float* m = reinterpret_cast<float*>(obj3d + 0x14);
    g_held_placed.obj3d = obj3d;
    memcpy(g_held_placed.game_m, m, sizeof(g_held_placed.game_m));
    g_held_game_pos[0] = m[9], g_held_game_pos[1] = m[10], g_held_game_pos[2] = m[11];
    if (hand::in_flight(obj3d, m)) return;  // thrown: already out of the hand
    const auto& q = g_held_xf.r;
    // Where the object sits in the hand: its offset from the game's wrist, in the wrist's axes.
    // The game moves the object and its hand at different moments, so while the hand travels
    // fast (the rig moving or scaling carries the game's hand along) the raw offset jumps
    // about. Keep a steady one: follow the game's quickly while its hand is still, slowly
    // otherwise.
    static void* held = nullptr;
    static float local[3] = {}, last_from[3] = {}, last_pos[3] = {};
    // (And the hand's size factor then: the game sizes its hand by its distance from its camera,
    // and the offset scaled by the factor of the moment slid the thing along the palm as the
    // game's hand moved; playtest 2026-10-31.)
    static float local_k = 1;
    const auto& u = g_held_xf.u;
    const float raw[3] = {m[9] - g_held_xf.from[0], m[10] - g_held_xf.from[1], m[11] - g_held_xf.from[2]};
    float now_local[3];
    for (int i = 0; i < 3; ++i) now_local[i] = u[i][0] * raw[0] + u[i][1] * raw[1] + u[i][2] * raw[2];
    float moved = 0;
    for (int i = 0; i < 3; ++i) moved = std::max({moved, std::fabs(g_held_xf.from[i] - last_from[i]), std::fabs(m[9 + i] - last_pos[i])});
    // The game holds a thing rigidly in its hand, so the offset is fixed: it is followed while
    // the game brings the thing into its hand (the first half second), then kept. (Followed for
    // good, slowly while the player moved, the thing lagged and drifted in the hand.)
    static ULONGLONG held_at = 0;
    static unsigned pickup = 0;
    const ULONGLONG now_ms = GetTickCount64();
    if (pickup != g_held_pickups) pickup = g_held_pickups, held = nullptr;  // (picked up afresh)
    if (obj3d != held) {
        held = obj3d, held_at = now_ms;
        memcpy(local, now_local, sizeof(local));
        local_k = g_held_xf.offset_scale;
    } else if (now_ms - held_at < 500) {
        const float a = moved < 0.02f ? 0.3f : 0.1f;
        for (int i = 0; i < 3; ++i) local[i] += (now_local[i] - local[i]) * a;
        local_k = g_held_xf.offset_scale;
    }
    memcpy(last_from, g_held_xf.from, sizeof(last_from));
    last_pos[0] = m[9], last_pos[1] = m[10], last_pos[2] = m[11];
    // The wrist's place this frame (the hand transform is from the last frame's hand draw).
    float to[3] = {g_held_xf.to[0], g_held_xf.to[1], g_held_xf.to[2]};
    hand::NearHand nh;
    if (hand::near_hand(nh)) {
        const float p[3] = {nh.pos.x, nh.pos.y, nh.pos.z};
        for (int i = 0; i < 3; ++i) to[i] = g_held_xf.from[i] + (p[i] - g_held_xf.from[i]) * nh.blend;
    }
    for (int r = 0; r < 4; ++r) {
        float v[3] = {m[r * 3], m[r * 3 + 1], m[r * 3 + 2]};
        if (r == 3)
            for (int c = 0; c < 3; ++c) v[c] = local[0] * u[0][c] + local[1] * u[1][c] + local[2] * u[2][c];
        for (int c = 0; c < 3; ++c) {
            const float rot = q[c][0] * v[0] + q[c][1] * v[1] + q[c][2] * v[2];
            m[r * 3 + c] = r == 3 ? rot * local_k + to[c] : rot;
        }
    }
    // A miracle in the hand (the food's horn, the wood): the game holds it beside its hand and its
    // wrist turns while casting, so the miracle drifted away and back (player, 2026-10-27). Its
    // place against the palm, in the controller's axes, is taken as it settles into the hand (the
    // game's look: the hand holding the horn's side) and then kept: it goes with the controller.
    // (Its middle put on the palm hid the hand inside the horn.)
    // (People and animals too, since 2026-11-01: the game's place had them in the fingers. Where each
    // sits, out from the palm, toward the fingers and up, is on the tuning panel: hand::held_place.)
    const int held_kind = hand::held_kind();
    const hand::HeldPlace hp = hand::held_place(held_kind);
    if (((held_kind == 2 && g_set.miracle_on_palm) || ((held_kind == 1 || held_kind == 3) && hp.on)) && g_hand && hand::near_hand(nh) && nh.blend >= 0.5f) {
        using InfoFn = const char*(__thiscall*)(void*);
        const auto info_fn = reinterpret_cast<InfoFn>((*reinterpret_cast<void***>(obj3d))[0xF8 / 4]);
        const char* info = info_fn ? info_fn(obj3d) : nullptr;
        float c[3] = {0, 0, 0};
        if (info && !IsBadReadPtr(info + 0x18, 12)) memcpy(c, info + 0x18, sizeof(c));
        float off[3];  // (its middle from its origin, as drawn)
        for (int k = 0; k < 3; ++k) off[k] = c[0] * m[k] + c[1] * m[3 + k] + c[2] * m[6 + k];
        const vr::Vec3 ax[3] = {vr::normalize(nh.x), vr::normalize(nh.y), vr::normalize(nh.z)};
        const vr::Vec3 mid{m[9] + off[0], m[10] + off[1], m[11] + off[2]};
        static void* spell_obj = nullptr;
        static ULONGLONG spell_at = 0;
        static unsigned spell_pickup = 0;
        static float spell_local[3] = {};
        static float spell_thick = -1;  // (its half-thickness along the palm's normal; < 0: not set against the palm)
        const ULONGLONG now_ms2 = GetTickCount64();
        if (spell_pickup != g_held_pickups) spell_pickup = g_held_pickups, spell_obj = nullptr;
        if (obj3d != spell_obj) spell_obj = obj3d, spell_at = now_ms2;
        // (People and animals go straight to their place: following the game's place while they
        // settled, a person jiggled and settled near the back of the hand; playtest 2026-11-01.)
        static float up_sign = 1;  // ("up": the hand's own up, the side of x that was up as it settled)
        const bool settling = now_ms2 - spell_at < 600;
        if (settling) {  // (settling into the hand: the game's place followed)
            for (int k = 0; k < 3; ++k) spell_local[k] = vr::dot(mid - nh.palm, ax[k]);
            spell_thick = -1;
            up_sign = ax[0].y >= 0 ? 1.0f : -1.0f;
            // Against the palm: its middle out along the palm's normal by its half-thickness that way
            // (its box, info +0x24 half-sizes, along its rows as drawn), less a little: the fingers
            // close on it. Its turn stays the game's.
            // (Only things of a hand's size, up to 50 cm across: the shield's bubble and the water's
            // sheet are big, around the hand, and were pushed off it.)
            if (g_set.miracle_grip && info && !IsBadReadPtr(info + 0x24, 12)) {
                const float* h = reinterpret_cast<const float*>(info + 0x24);
                float thick = 0, most = 0;
                for (int r = 0; r < 3; ++r) {
                    const vr::Vec3 row{m[r * 3], m[r * 3 + 1], m[r * 3 + 2]};
                    thick += std::fabs(vr::dot(ax[1], row)) * h[r];
                    most = std::max(most, vr::length(row) * h[r]);
                }
                // (A rock of any size: in the hand at once, sitting on the palm.)
                const bool hand_sized = most <= (held_kind == 3 ? 2.0f : 0.25f) * hand::rig_scale();
                if (hand_sized) spell_local[0] = 0, spell_local[1] = 0, spell_local[2] = 0, spell_thick = thick;
                static void* told = nullptr;
                if (told != obj3d)
                    told = obj3d, BWLOG("stereo: %s in the hand, %.2f units at most from its middle: %s", held_kind == 1 ? "a person or animal" : held_kind == 3 ? "a rock" : "a miracle",
                                        most, hand_sized ? "held against the palm" : "too big: where the game holds it");
            }
        }
        if (!settling || ((held_kind == 1 || held_kind == 3) && spell_thick >= 0)) {
            vr::Vec3 want = nh.palm + ax[0] * spell_local[0] + ax[1] * spell_local[1] + ax[2] * spell_local[2];
            if (spell_thick >= 0) {  // (the panel's values, live; "up" along the hand, so it stays put as the hand turns)
                const float scale = hand::rig_scale();
                want = want + ax[1] * (spell_thick * hp.out + hp.out_m * scale) + ax[2] * (hp.fwd * scale) + ax[0] * (up_sign * hp.up * scale);
            }
            static double logged_at = 0;
            const double t = double(now_ms2) / 1000.0;
            if (t - logged_at > 5.0) {
                logged_at = t;
                BWLOG("stereo: the miracle in the hand kept with the controller, %.2f units from the palm (the game's place %.2f from it)",
                      vr::length(want - nh.palm), vr::length(mid - want));
            }
            m[9] = want.x - off[0], m[10] = want.y - off[1], m[11] = want.z - off[2];
        }
    }
    if (g_held_log > 0 && hand::near_hand(nh)) {
        --g_held_log;
        const vr::Vec3 ax[3] = {vr::normalize(nh.x), vr::normalize(nh.y), vr::normalize(nh.z)};
        const vr::Vec3 d{m[9] - nh.palm.x, m[10] - nh.palm.y, m[11] - nh.palm.z};
        float rows[3][3], game_rows[3][3];  // (each of its rows in the hand's axes, unit; the game's in its wrist's)
        for (int r = 0; r < 3; ++r) {
            const vr::Vec3 v = vr::normalize(vr::Vec3{m[r * 3], m[r * 3 + 1], m[r * 3 + 2]});
            const float* gm = g_held_placed.game_m + r * 3;
            const float gl = std::max(1e-6f, std::sqrt(gm[0] * gm[0] + gm[1] * gm[1] + gm[2] * gm[2]));
            for (int k = 0; k < 3; ++k) {
                rows[r][k] = vr::dot(v, ax[k]);
                game_rows[r][k] = (u[k][0] * gm[0] + u[k][1] * gm[1] + u[k][2] * gm[2]) / gl;
            }
        }
        {
            using InfoFn = const float*(__thiscall*)(void*);
            const auto info_fn = reinterpret_cast<InfoFn>((*reinterpret_cast<void***>(obj3d))[0xF8 / 4]);
            const float* f = info_fn ? info_fn(obj3d) : nullptr;
            if (f && !IsBadReadPtr(f, 0x34))
                BWLOG("held: info %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | r %.3f; scale %.3f; rows %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f",
                      f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], *reinterpret_cast<const float*>(obj3d + 0x44),
                      m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        }
        BWLOG("held: pos (%.3f, %.3f, %.3f) up (%.3f, %.3f, %.3f) fwd (%.3f, %.3f, %.3f) in the hand; game up (%.3f, %.3f, %.3f) "
              "fwd (%.3f, %.3f, %.3f) local (%.3f, %.3f, %.3f) in its wrist; blend %.2f",
              vr::dot(d, ax[0]), vr::dot(d, ax[1]), vr::dot(d, ax[2]), rows[1][0], rows[1][1], rows[1][2], rows[2][0], rows[2][1], rows[2][2],
              game_rows[1][0], game_rows[1][1], game_rows[1][2], game_rows[2][0], game_rows[2][1], game_rows[2][2], now_local[0], now_local[1],
              now_local[2], nh.blend);
    }
    {   // The gravity gloves' pull: on its way from where it was to its place in the hand, in an arc.
        vr::Vec3 from;
        float t = 0;
        if (hand::gravity_flight(obj3d, from, t)) {
            const float e = t * t * (3.0f - 2.0f * t);
            const vr::Vec3 to_p{m[9], m[10], m[11]};
            const float lift = 0.2f * vr::length(to_p - from) * std::sin(3.14159265f * t);
            const vr::Vec3 p = from + (to_p - from) * e + vr::Vec3{0, lift, 0};
            m[9] = p.x, m[10] = p.y, m[11] = p.z;
            if (g_flight_log > 0)
                --g_flight_log, BWLOG("stereo: the gloves' flight: t %.2f at (%.1f, %.1f, %.1f), %.1f from the hand's place", t, p.x, p.y, p.z, vr::length(to_p - p));
        }
    }
    g_held_moved_pos[0] = m[9], g_held_moved_pos[1] = m[10], g_held_moved_pos[2] = m[11];
    g_held_placed.moved = true;
    memcpy(g_held_placed.m, m, sizeof(g_held_placed.m));
    hand::note_held(obj3d, m);
}

// The camera/hand control hints ("Move", "Plant", a mouse icon...) are 2D overlays drawn at the
// cursor. In the headset they hang head-locked over the hand and mean mouse buttons; hide them.
// static void __cdecl CameraHelp::DrawKeyOrMouse(CH_ANIMTYPE, int, int, unsigned short*, int, int,
//                                                int, KEYALIGN, LH3DColor*, LH3DColor*, int)
using DrawKeyOrMouseFn = void(__cdecl*)(int, int, int, unsigned short*, int, int, int, int, void*, void*, int);
DrawKeyOrMouseFn o_draw_key_or_mouse = nullptr;
void __cdecl draw_key_or_mouse_detour(int anim, int x, int y, unsigned short* text, int a, int b, int c, int align,
                                      void* col1, void* col2, int d) {
    // In VR the hand's action hints become words by the hand in the world (ui::hint); the mouse
    // picture and the camera hints are dropped.
    if (g_set.enabled && g_set.hide_mouse_hints && !g_cinema_pass) {
        struct Args {
            int anim, x, y;
            unsigned short* text;
            int a, b, c, align;
            void *col1, *col2;
            int d;
        } args{anim, x, y, text, a, b, c, align, col1, col2, d};
        const auto original = [](void* p) {
            const Args& g = *static_cast<Args*>(p);
            o_draw_key_or_mouse(g.anim, g.x, g.y, g.text, g.a, g.b, g.c, g.align, g.col1, g.col2, g.d);
        };
        if (g_current_eye < 0 || ui::hint(reinterpret_cast<const wchar_t*>(text), x, original, &args)) return;
    }
    o_draw_key_or_mouse(anim, x, y, text, a, b, c, align, col1, col2, d);
}

// The camera "tricons" (the yellow move/pitch/tilt arrows at the hand) are sprites placed by
// unprojecting the hand's screen position with the game camera at the flat screen's size; in the
// headset they float detached from the hand. They're mouse-camera guidance, so hide them too.
using VoidFn = void(__cdecl*)();
VoidFn o_hand_tricon = nullptr, o_tricon_draw = nullptr;
void __cdecl hand_tricon_detour() {
    if (g_set.enabled && g_set.hide_mouse_hints && !g_cinema_pass) return;
    o_hand_tricon();
}
void __cdecl tricon_draw_detour() {
    if (g_set.enabled && g_set.hide_mouse_hints && !g_cinema_pass) return;
    o_tricon_draw();
}

render::EyeTarget g_targets[2];
std::string g_capture_name;  // non-empty: capture the next stereo frame
unsigned g_stereo_frames = 0, g_mono_frames = 0;
double g_last_vr_frame_s = -10;
double g_eye_ms[2] = {0, 0};
LARGE_INTEGER g_freq;

constexpr uint32_t kClearColor = 0x00FF00FF;  // magenta: anything left this colour was never drawn
constexpr uint32_t kCinemaDark = 0x00080808;  // the dark around the cinema screen
render::FrameTexture g_cinema_texture;        // the game's own frame, for the cinema screen

void* game_device() { return *reinterpret_cast<void**>(sym::LH3DRender_Direct3DDevice7); }

struct EyeGeometry {
    int render_w = 0, render_h = 0;  // symmetric render target
    render::Rect crop;               // the asymmetric eye frustum inside it
    float focal_px = 0;              // pixels per unit tangent
    float fov = 0;                   // engine horizontal FOV (radians) of the symmetric frustum
};

// Both eyes share one symmetric render geometry (size, focal length, FOV) covering the union of
// their frustums; only the crop differs. Identical pixel grids make the eyes directly comparable
// (the IPD=0 identity test can demand pixel equality).
EyeGeometry eye_geometry(const vr::EyeOptics& o) {
    EyeGeometry g;
    float tx = 0, ty = 0, tan_h = 0;
    for (const vr::EyeOptics& e : g_profile.eye) {
        tx = std::max({tx, -e.tan_left, e.tan_right});
        ty = std::max({ty, -e.tan_top, e.tan_bottom});
        tan_h = std::max(tan_h, e.tan_bottom - e.tan_top);
    }
    g.focal_px = float(g_set.eye_height) / tan_h;
    // The D3D7 device refuses render targets larger than 2048 px on either side; shrink to fit.
    constexpr float kMaxTarget = 2048 - 8;
    const float needed = 2 * std::max(tx, ty) * g.focal_px;
    if (needed > kMaxTarget) {
        g.focal_px *= kMaxTarget / needed;
        static int logged_height = -1;
        if (logged_height != g_set.eye_height) {
            BWLOG("stereo: eye_height %d exceeds the 2048 px render-target limit; using %d", g_set.eye_height,
                  int(g.focal_px * tan_h));
            logged_height = g_set.eye_height;
        }
    }
    // Even sizes keep the projection centre on a pixel corner. The engine clips to (w-1, h-1),
    // so a 2 px margin keeps the last drawn row/column out of the crop.
    g.render_w = (int(std::ceil(2 * tx * g.focal_px)) + 4 + 1) & ~1;
    g.render_h = (int(std::ceil(2 * ty * g.focal_px)) + 4 + 1) & ~1;
    // The engine centres its projection on (w/2, h/2) with f = (w/2)/tan(fov/2).
    g.fov = 2.0f * std::atan(g.render_w * 0.5f / g.focal_px);
    const float cx = g.render_w * 0.5f, cy = g.render_h * 0.5f;
    g.crop.x = int(std::lround(cx + o.tan_left * g.focal_px));
    g.crop.y = int(std::lround(cy + o.tan_top * g.focal_px));
    g.crop.w = int(std::lround((o.tan_right - o.tan_left) * g.focal_px));
    g.crop.h = int(std::lround((o.tan_bottom - o.tan_top) * g.focal_px));
    g.crop.w = std::min(g.crop.w, g.render_w - g.crop.x);
    g.crop.h = std::min(g.crop.h, g.render_h - g.crop.y);
    return g;
}

// Engine camera state we overwrite per eye and restore afterwards.
struct EngineCamState {
    game::LH3DCamera cam;
    game::LHMatrix w2c, w2clip;
    game::InfoTransform info;
    float meter_w, meter_h;
    float max_screen[2];

    void save() {
        cam = game::camera();
        w2c = game::world_to_camera();
        w2clip = game::world_to_clipping();
        info = game::info_transform();
        meter_w = game::meter_width_on_2();
        meter_h = game::meter_height_on_2();
        memcpy(max_screen, game::max_screen(), sizeof(max_screen));
    }
    void restore() const {
        game::camera() = cam;
        game::world_to_camera() = w2c;
        game::world_to_clipping() = w2clip;
        game::info_transform() = info;
        game::meter_width_on_2() = meter_w;
        game::meter_height_on_2() = meter_h;
        memcpy(game::max_screen(), max_screen, sizeof(max_screen));
    }
};

Basis basis_of(const game::LHMatrix& w2c) {
    // Columns of the 3x3 part are the camera's right/up/forward axes in world space.
    return {{w2c.m[0][0], w2c.m[1][0], w2c.m[2][0]},
            {w2c.m[0][1], w2c.m[1][1], w2c.m[2][1]},
            {w2c.m[0][2], w2c.m[1][2], w2c.m[2][2]}};
}

// Writes the eye camera into the engine, after UpdateViewPort/ChangeFov set the projection.
bool g_use_update_camera = false;  // stereo.req "update_camera 0|1" (no measurable effect; off)

void set_engine_camera(Vec3 pos, const Basis& b, float focus_dist) {
    if (g_use_update_camera) {
        // Let the engine derive everything it derives from the camera (e.g. g_d3d_world2view),
        // then overwrite the matrices with the exact eye basis (UpdateCamera has no roll).
        game::LHPoint p{pos.x, pos.y, pos.z};
        game::LHPoint f{pos.x + b.fwd.x * focus_dist, pos.y + b.fwd.y * focus_dist, pos.z + b.fwd.z * focus_dist};
        game::update_camera(p, f);
    }
    game::LH3DCamera& cam = game::camera();
    cam.pos = {pos.x, pos.y, pos.z};
    cam.look_at = {b.fwd.x, b.fwd.y, b.fwd.z};
    cam.foc = {pos.x + b.fwd.x * focus_dist, pos.y + b.fwd.y * focus_dist, pos.z + b.fwd.z * focus_dist};

    const Vec3 axes[3] = {b.right, b.up, b.fwd};
    const game::InfoTransform& t = game::info_transform();
    // world_to_clipping = world_to_camera with X scaled by 1/tan(fov/2) and Y by aspect/tan(fov/2)
    // (field names in the decomp are swapped; see documentation/render-pass-map.md).
    const float clip_scale[3] = {t.inv_half_tan_fov_y, t.inv_half_tan_fov_x, 1.0f};
    game::LHMatrix& w2c = game::world_to_camera();
    game::LHMatrix& w2clip = game::world_to_clipping();
    for (int c = 0; c < 3; ++c) {
        w2c.m[0][c] = axes[c].x;
        w2c.m[1][c] = axes[c].y;
        w2c.m[2][c] = axes[c].z;
        w2c.m[3][c] = -vr::dot(pos, axes[c]);
        for (int r = 0; r < 4; ++r) w2clip.m[r][c] = w2c.m[r][c] * clip_scale[c];
    }
    // LH3DTech::UpdateCamera also keeps a copy of world_to_clipping at 0xEA9E40 and its inverse
    // at 0xEA9DE0 (fn_007FB290 inverts). Code that goes from camera space back to the world uses
    // that inverse: villagers' shadows are built from their foot bones through it (fn_0081FFF0).
    // Left as UpdateCamera made it (no roll, its own pitch), it didn't match the matrices above,
    // and those shadows slid about with the head's orientation.
    memcpy(reinterpret_cast<void*>(0xEA9E40), &w2clip, sizeof(game::LHMatrix));
    reinterpret_cast<void(__fastcall*)(void*, const void*)>(0x7FB290)(reinterpret_cast<void*>(0xEA9DE0),
                                                                     reinterpret_cast<const void*>(0xEA9E40));
}

// --- capture helpers ---
struct EyeCapture {
    EyeGeometry geo;
    Vec3 pos;
    Basis rot;
    game::LHMatrix w2c;
    std::vector<std::string> probes;
    bool color_ok = false, depth_ok = false;
};

void probe_points(const Vec3& focus, std::vector<std::string>& out) {
    // Engine projection of a grid around the base focus point, with the eye camera active.
    char buf[160];
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = 0; dy <= 1; ++dy) {
                game::LHPoint p{focus.x + dx * 10.0f, focus.y + dy * 10.0f, focus.z + dz * 10.0f};
                int x = -1, y = -1;
                float depth = -1;
                uint32_t vis = game::project_point(p, &x, &y, &depth);
                snprintf(buf, sizeof(buf), "{\"p\": [%.4f, %.4f, %.4f], \"vis\": %u, \"x\": %d, \"y\": %d, \"depth\": %.5f}",
                         p.x, p.y, p.z, vis, x, y, depth);
                out.push_back(buf);
            }
}

void write_vec(FILE* f, const char* k, const Vec3& v, const char* end = ",") {
    fprintf(f, "\"%s\": [%.6f, %.6f, %.6f]%s", k, v.x, v.y, v.z, end);
}

void write_capture_meta(const std::wstring& path, const EngineCamState& base, const EyeCapture caps[2]) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "{\n  \"profile\": \"%s\", \"world_scale\": %.6f, \"ipd_scale\": %.4f, \"level_base\": %d, \"zero_time\": %d,\n",
            g_profile.model.c_str(), g_set.world_scale, g_set.ipd_scale, g_set.level_base, g_set.zero_time);
    fprintf(f, "  \"head\": {");
    write_vec(f, "pos", g_head.pos);
    write_vec(f, "right", g_head.rot.right);
    write_vec(f, "up", g_head.rot.up);
    write_vec(f, "fwd", g_head.rot.fwd, "},\n");
    fprintf(f, "  \"base_camera\": {");
    write_vec(f, "pos", {base.cam.pos.x, base.cam.pos.y, base.cam.pos.z});
    write_vec(f, "foc", {base.cam.foc.x, base.cam.foc.y, base.cam.foc.z});
    fprintf(f, "\"fov\": %.7f, \"resolution\": [%d, %d]},\n", base.cam.fov, base.info.resolution.x, base.info.resolution.y);
    fprintf(f, "  \"eyes\": [\n");
    for (int e = 0; e < 2; ++e) {
        const EyeCapture& c = caps[e];
        const vr::EyeOptics& o = g_profile.eye[e];
        fprintf(f, "    {\"name\": \"%s\", \"render\": [%d, %d], \"crop\": [%d, %d, %d, %d], \"focal_px\": %.5f, \"fov\": %.7f,\n",
                e ? "R" : "L", c.geo.render_w, c.geo.render_h, c.geo.crop.x, c.geo.crop.y, c.geo.crop.w, c.geo.crop.h,
                c.geo.focal_px, c.geo.fov);
        fprintf(f, "     \"tan\": [%.7f, %.7f, %.7f, %.7f], ", o.tan_left, o.tan_right, o.tan_top, o.tan_bottom);
        write_vec(f, "offset_m", o.offset);
        fprintf(f, "\n     ");
        write_vec(f, "pos", c.pos);
        write_vec(f, "right", c.rot.right);
        write_vec(f, "up", c.rot.up);
        write_vec(f, "fwd", c.rot.fwd);
        fprintf(f, "\n     \"world_to_camera\": [");
        for (int r = 0; r < 4; ++r)
            fprintf(f, "%s[%.7f, %.7f, %.7f]", r ? ", " : "", c.w2c.m[r][0], c.w2c.m[r][1], c.w2c.m[r][2]);
        fprintf(f, "],\n     \"color\": %s, \"depth\": %s, \"depth_encoding\": \"z = 1 - 1/view_depth, float32 rows top-down\",\n",
                c.color_ok ? "true" : "false", c.depth_ok ? "true" : "false");
        fprintf(f, "     \"probes\": [");
        for (size_t i = 0; i < c.probes.size(); ++i) fprintf(f, "%s%s", i ? ", " : "", c.probes[i].c_str());
        fprintf(f, "]}%s\n", e ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

void save_eye_images(render::EyeTarget& t, const EyeGeometry& g, const std::wstring& stem, EyeCapture& c) {
    std::vector<uint8_t> bgr;
    int stride = 0;
    c.color_ok = t.read_color(g.crop, bgr, stride) && render::write_bmp(stem + L".bmp", g.crop.w, g.crop.h, bgr);
    std::vector<float> depth;
    if (t.read_depth(g.crop, depth)) {
        if (FILE* f = _wfopen((stem + L".depth").c_str(), L"wb")) {
            fwrite(depth.data(), sizeof(float), depth.size(), f);
            fclose(f);
            c.depth_ok = true;
        }
    }
}

void mirror(void* device, const EyeGeometry geo[2]) {
    const game::InfoTransform& t = game::info_transform();  // restored: window size
    if (g_set.mirror == 1) {
        // One eye, filling the window: the largest window-shaped piece of the left eye's image
        // around its optical centre (the middle of the symmetric render target).
        const render::Rect& c = geo[0].crop;
        const float aspect = float(t.resolution.x) / float(std::max(1, t.resolution.y));
        render::Rect src;
        src.w = std::min(c.w, int(c.h * aspect));
        src.h = std::min(c.h, int(src.w / aspect));
        src.x = std::clamp(geo[0].render_w / 2 - src.w / 2, c.x, c.x + c.w - src.w);
        src.y = std::clamp(geo[0].render_h / 2 - src.h / 2, c.y, c.y + c.h - src.h);
        render::Rect dst;
        dst.x = dst.y = 0;
        dst.w = t.resolution.x;
        dst.h = t.resolution.y;
        g_targets[0].blit_to(device, src, dst);
        return;
    }
    const int half_w = t.resolution.x / 2, h = t.resolution.y;
    for (int e = 0; e < 2; ++e) {
        const render::Rect& c = geo[e].crop;
        float s = std::min(float(half_w) / c.w, float(h) / c.h);
        render::Rect d;
        d.w = int(c.w * s);
        d.h = int(c.h * s);
        d.x = e * half_w + (half_w - d.w) / 2;
        d.y = (h - d.h) / 2;
        g_targets[e].blit_to(device, c, d);
    }
}

// Writes SteamVR's mirror of both eyes (what the compositor actually received) as BMPs.
void save_mirror() {
    for (int e = 0; e < 2; ++e) {
        std::vector<uint8_t> bgra;
        int w = 0, h = 0;
        if (!vr::openvr::read_mirror(e, bgra, w, h)) {
            BWLOG("stereo: mirror read failed (eye %d)", e);
            continue;
        }
        const int stride = (w * 3 + 3) & ~3;
        std::vector<uint8_t> bgr(size_t(stride) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                memcpy(&bgr[size_t(h - 1 - y) * stride + x * 3], &bgra[(size_t(y) * w + x) * 4], 3);
        std::wstring name(g_mirror_name.begin(), g_mirror_name.end());
        render::write_bmp(paths::bwvr_dir() + L"captures\\" + name + (e ? L"_R" : L"_L") + L".bmp", w, h, bgr);
    }
    BWLOG("stereo: mirror captured '%s'", g_mirror_name.c_str());
    g_mirror_name.clear();
}

// The game's view isn't the world (GAME_VIEW_MODE: inside the temple, for one): it draws that
// view with a renderer of its own, which the eye passes don't cover. It is shown on the cinema
// screen instead, the laser being the mouse ([stereo] other_views_on_screen).
bool other_view(void* game) { return game::game_field<int>(game, game::kGameViewMode) != 0; }
bool eligible(void* game) {
    return game_device() && (!other_view(game) || g_set.other_views_on_screen) &&
           game::game_field<void*>(game, game::kGameVideoPlayer) == nullptr;
}

// The temple's camera (InnerCamera::PreDraw) sets a 90-degree horizontal FOV every frame. Its rooms
// are laid out for 4:3; at 16:9 that FOV shows a quarter less height, and the save room's Load
// button falls off the bottom of the frame (the flat game too). In VR its FOV is widened to show
// the height it shows at 4:3. Its mouse rays (InnerCamera::RayCast, LH3DTech::Get3DPointFromScreen)
// use the same projection, so clicks still land where the picture shows.
using ChangeFovFn = void(__stdcall*)(float);
ChangeFovFn o_change_fov = nullptr;
constexpr uintptr_t kInnerCameraPreDrawFovReturn = 0x796930;  // after PreDraw's call to ChangeFov
void __stdcall change_fov_detour(float fov) {
    if (g_set.enabled && g_set.temple_4x3 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == kInnerCameraPreDrawFovReturn) {
        const auto& res = game::info_transform().resolution;
        const float aspect = res.y > 0 ? float(res.x) / float(res.y) : 4.0f / 3.0f;
        if (aspect > 4.0f / 3.0f) {
            const float wide = 2.0f * std::atan(std::tan(fov * 0.5f) * aspect * 0.75f);
            static bool logged = false;
            if (!logged) BWLOG("stereo: the temple's FOV %.1f widened to %.1f degrees (4:3 height at %ldx%ld)", fov / vr::kDegToRad, wide / vr::kDegToRad, res.x, res.y), logged = true;
            fov = wide;
        }
    }
    o_change_fov(fov);
}

// GGame::GoInsideCitadel keeps the pause state (GGame+0x2502C4) and pauses the game (PauseGame(1):
// bit 4 of the game's flags at +0x14 stops the game turns); LeaveInsideCitadel puts it back. A save
// loaded from the temple's save room never leaves the temple that way, so the world came back
// paused until the Esc menu was opened and closed (the flat game too). When the world is back
// after one second with the temple not left, its pause state is put back as leaving would.
using CitadelFn = void(__thiscall*)(void* game, unsigned long, int);
using LeaveCitadelFn = void(__thiscall*)(void* game);
CitadelFn o_go_inside = nullptr;
LeaveCitadelFn o_leave_inside = nullptr;
constexpr uintptr_t kGoInsideCitadel = 0x553E10, kLeaveInsideCitadel = 0x553A70, kPauseGame = 0x54AE20;
constexpr uintptr_t kGameFlags = 0x14, kGamePausedBeforeTemple = 0x2502C4;  // flags: bit 4 = paused
bool g_inside_temple = false;
void __fastcall go_inside_detour(void* game, void*, unsigned long a, int b) {
    o_go_inside(game, a, b);
    g_inside_temple = true;
}
void __fastcall leave_inside_detour(void* game, void*) {
    g_inside_temple = false;
    o_leave_inside(game);
}
void unpause_after_temple_load(void* game) {
    static int world_frames = 0;
    if (!g_inside_temple || other_view(game)) {
        world_frames = 0;
        return;
    }
    static LARGE_INTEGER since{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (world_frames++ == 0) since = now;
    if (double(now.QuadPart - since.QuadPart) < double(g_freq.QuadPart)) return;
    g_inside_temple = false;
    const uint32_t flags = game::game_field<uint32_t>(game, kGameFlags);
    const int before = game::game_field<int>(game, kGamePausedBeforeTemple);
    BWLOG("stereo: the world is back without leaving the temple (a save loaded there): game %s, paused before the temple %d%s",
          flags & 4 ? "paused" : "running", before, g_set.temple_unpause && (flags & 4) && !before ? ": unpaused" : "");
    if (g_set.temple_unpause && (flags & 4) && !before) reinterpret_cast<void(__cdecl*)(int)>(kPauseGame)(0);
}

// The save room's pictures: entering the temple, the game takes a picture of the screen
// (ScriptedScreenShot::Process, from GoInsideCitadel), which a save then keeps; it also takes
// pictures of the land now and then. It locks the primary surface (LHSys::TheSystem +0x40) and
// samples the screen's size from its top-left corner (the size at +0x18, two words), as a
// fullscreen game would. In a window the primary surface is the whole desktop, and on Windows
// 10/11 it reads back as nothing but magenta: every save's picture was a pink rectangle.
// In VR it reads a copy of the window's last frame instead (what the player saw: the mirrored
// eye; on the cinema screen, the game's own frame), kept every few frames. (The back buffer itself
// was no good: at the capture it sometimes held one flat colour, or was an eye target.)
using VoidCdeclFn = void(__cdecl*)();
VoidCdeclFn o_screenshot_process = nullptr;
constexpr uintptr_t kScriptedScreenShotProcess = 0x821750, kScreenShotPending = 0xEB99B4;
constexpr uintptr_t kSysPrimarySurface = 0xE85080, kSysScreenSize = 0xE85058;  // LHSys::TheSystem +0x40, +0x18
render::FrameTexture g_last_frame;  // the window's frame, for the save pictures
bool g_last_frame_ok = false;
void __cdecl screenshot_process_detour() {
    void* picture = nullptr;
    int w = 0, h = 0;
    if (g_set.enabled && g_set.save_pictures && *reinterpret_cast<int*>(kScreenShotPending) && g_last_frame_ok &&
        render::surface_size(g_last_frame.surface(), &w, &h))
        picture = g_last_frame.surface();
    if (!picture) {
        o_screenshot_process();
        return;
    }
    void*& primary = *reinterpret_cast<void**>(kSysPrimarySurface);
    uint16_t* size = reinterpret_cast<uint16_t*>(kSysScreenSize);
    void* const saved = primary;
    const uint16_t saved_w = size[0], saved_h = size[1];
    primary = picture, size[0] = uint16_t(w), size[1] = uint16_t(h);
    o_screenshot_process();
    primary = saved, size[0] = saved_w, size[1] = saved_h;
    BWLOG("stereo: a picture taken from the window's last frame (%dx%d) instead of the screen", w, h);
}

// The temple's rooms: Temple::GoToRoom(TempleRoomsEnum) on the Temple at GGame+0x59B4 (logged;
// "temple_room <n>" for the harness, which otherwise has to click its way in).
using GoToRoomFn = void(__thiscall*)(void* temple, int room);
GoToRoomFn o_go_to_room = nullptr;
constexpr uintptr_t kTempleGoToRoom = 0x793D90, kGameTemple = 0x59B4;
void __fastcall go_to_room_detour(void* temple, void*, int room) {
    BWLOG("stereo: the temple goes to room %d", room);
    o_go_to_room(temple, room);
}
// Debug (stereo temple_mouse_log N): the mouse messages the temple gets (Temple::ProcessMouse,
// thiscall(LHCoord {x, y}, INTERFACE_MESSAGE_TYPES), from CMouse::ProcessButtons).
struct Coord2 {
    int32_t x, y;
};
using TempleMouseFn = void(__thiscall*)(void* temple, Coord2 at, int type);
TempleMouseFn o_temple_mouse = nullptr;
int g_temple_mouse_log = 0;

void __fastcall temple_mouse_detour(void* temple, void*, Coord2 at, int type) {
    if (g_temple_mouse_log > 0) {
        --g_temple_mouse_log;
        BWLOG("stereo: the temple gets mouse message %d at (%d, %d)", type, at.x, at.y);
    }
    o_temple_mouse(temple, at, type);
}

// A sampling profiler (debug: stereo profile <ms>): another thread stops the game's thread every
// ~0.25 ms and notes where it is (the instruction and a few return addresses up the frame-pointer
// chain) and which part of the frame it is in. Written to captures\profile.txt for
// tools\perf\profile_report.py, which names the game's functions.
DWORD g_game_thread_id = 0;
// Debug: which code writes to an address (watch_write <hex> | status): a hardware data breakpoint
// (DR0) on the game's thread; each distinct writing instruction is logged once.
volatile uintptr_t g_watch_addr = 0;
PVOID g_watch_veh = nullptr;
LONG CALLBACK watch_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    static uintptr_t seen[32];
    static int n = 0;
    const uintptr_t at = ep->ContextRecord->Eip;
    bool known = false;
    for (int i = 0; i < n; ++i) known |= seen[i] == at;
    if (!known && n < 32) {
        seen[n++] = at;
        const float* v = reinterpret_cast<const float*>(g_watch_addr);
        const auto* sp = reinterpret_cast<const uint32_t*>(ep->ContextRecord->Esp);
        BWLOG("stereo: watch: %08X written by the code before %08X (now %g); stack %08X %08X %08X %08X %08X %08X", unsigned(g_watch_addr), unsigned(at),
              v ? *v : 0.0f, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void set_watch(uintptr_t addr) {
    if (!g_watch_veh) g_watch_veh = AddVectoredExceptionHandler(1, &watch_handler);
    g_watch_addr = addr;
    std::thread([addr] {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_game_thread_id);
        if (!th) return;
        SuspendThread(th);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(th, &c);
        c.Dr0 = addr;
        c.Dr7 = addr ? (1u | (1u << 16) | (3u << 18)) : 0;  // L0, write, 4 bytes
        SetThreadContext(th, &c);
        ResumeThread(th);
        CloseHandle(th);
        BWLOG("stereo: watch: %s %08X", addr ? "watching writes to" : "stopped", unsigned(addr));
    }).detach();
}
std::atomic<uint32_t> g_frame_no{0};  // counts VR frames (each sample notes its frame: the slow frames apart)
std::atomic<int> g_frame_part{0};  // 0 outside the 3D pass, 1 setup/after, 2 left eye, 3 right eye, 4 readback
std::atomic<int64_t> g_part_at{0};  // when the frame's part last changed (QueryPerformanceCounter)
void set_part(int part) {
    g_frame_part = part;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_part_at = t.QuadPart;
}
// One sample of the game's thread (suspended meanwhile): the part of the frame, the instruction and
// a few return addresses up the frame-pointer chain, the frame's number.
std::array<uint32_t, 11> sample_game_thread(HANDLE th) {
    std::array<uint32_t, 11> smp{};
    if (SuspendThread(th) == DWORD(-1)) return smp;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    smp[0] = uint32_t(g_frame_part.load());
    smp[10] = g_frame_no.load();
    if (GetThreadContext(th, &c)) {
        smp[1] = c.Eip;
        uint32_t bp = c.Ebp;
        for (int i = 2; i < 10 && bp && !IsBadReadPtr(reinterpret_cast<void*>(bp), 8); ++i) {
            const uint32_t* frame = reinterpret_cast<const uint32_t*>(bp);
            smp[i] = frame[1];
            if (frame[0] <= bp) break;
            bp = frame[0];
        }
    }
    ResumeThread(th);
    return smp;
}
void write_modules(FILE* out) {  // (module ranges, for naming addresses outside the exe)
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
        MODULEINFO mi{};
        wchar_t name[MAX_PATH];
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) && GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH))
            fwprintf(out, L"module %08X %08X %s\n", unsigned(uintptr_t(mi.lpBaseOfDll)), unsigned(mi.SizeOfImage), name);
    }
}
void write_samples(FILE* out, const std::vector<std::array<uint32_t, 11>>& samples) {
    for (const auto& smp : samples) {
        if (!smp[1]) continue;
        fprintf(out, "s %u f%u", smp[0], smp[10]);
        for (int i = 1; i < 10 && smp[i]; ++i) fprintf(out, " %08X", smp[i]);
        fprintf(out, "\n");
    }
}
// Spikes (the player's frames of 40-170 ms, a few a minute, that tip SteamVR into reprojecting;
// 2026-10-28): a watcher looks every millisecond at how long the frame's part has run, and past
// spike_ms samples the game's thread until the part ends, into captures/spikes.txt
// (tools\perf\profile_report.py reads it). Nothing is done in frames that aren't slow.
// (stereo spike_ms <n>; 0: off.) Off unless asked for: in play it once left the game's thread
// suspended for 80 s, the watcher stuck between a suspend and its resume after 12 samples (the
// player's game froze after a fireball, 2026-10-30; resuming the thread by hand freed both).
std::atomic<int> g_spike_ms{0};
std::atomic<bool> g_spike_sampling{false};
// The game exiting: the watcher stops first. (Exiting, the frame's part never ends, so the watcher
// sampled on; ExitProcess ended its thread between a suspend and its resume, and the game's own
// thread stayed suspended: the game never closed. Player, 2026-10-30.)
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn o_exit_process = nullptr;
void stop_spikes() {
    g_spike_ms = 0;
    for (int i = 0; i < 200 && g_spike_sampling.load(); ++i) Sleep(1);
}
void WINAPI exit_process_detour(UINT code) {
    stop_spikes();
    o_exit_process(code);
}
// (The game's C runtime exiting, _doexit: before its cleanup.)
// Quitting from the menu in VR crashed in the game's own cleanup (player, 2026-10-30): LHScreen's
// destructor (a static one, after the C runtime's exit) has dxwrapper restore the display mode,
// and Direct3D 9 then went into NVIDIA's Direct3D 11 driver and called freed memory: the eyes'
// textures shared between Direct3D 9 and 11 (and our Direct3D 11 device) were still alive. They
// are now let go of first, with SteamVR, as when VR is switched off. (Without VR it never crashed.)
using DoExitFn = void(__cdecl*)(int, int, int);
DoExitFn o_doexit = nullptr;
void __cdecl doexit_detour(int code, int quick, int retcaller) {
    stop_spikes();
    static bool done = false;
    if (!done && vr::openvr::active()) {
        done = true;
        BWLOG("stereo: exiting: SteamVR and the shared eye textures let go of before the game's cleanup");
        vr::openvr::shutdown();
    }
    o_doexit(code, quick, retcaller);
}
void spike_watch() {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_game_thread_id);
    if (!th) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    int spikes = 0;
    bool started = false;
    int64_t reported = 0;  // (each stalled part once: the game may stop drawing VR frames for a while)
    for (;;) {
        Sleep(1);
        const int ms = g_spike_ms.load();
        const int64_t at = g_part_at.load();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (ms <= 0 || !at || at == reported || spikes >= 300 || (now.QuadPart - at) * 1000 < int64_t(ms) * f.QuadPart) continue;
        std::vector<std::array<uint32_t, 11>> samples;
        samples.reserve(4000);
        const int part = g_frame_part.load();
        const uint32_t frame = g_frame_no.load();
        g_spike_sampling = true;
        while (g_part_at.load() == at && samples.size() < 4000 && g_spike_ms.load() > 0) {
            samples.push_back(sample_game_thread(th));
            LARGE_INTEGER t, w;
            QueryPerformanceCounter(&t);
            do QueryPerformanceCounter(&w); while ((w.QuadPart - t.QuadPart) * 4000 < f.QuadPart);
        }
        g_spike_sampling = false;
        QueryPerformanceCounter(&now);
        const double lasted = double(now.QuadPart - at) * 1000.0 / double(f.QuadPart);
        ++spikes, reported = at;
        if (FILE* out = _wfopen((paths::bwvr_dir() + L"captures/spikes.txt").c_str(), started ? L"a" : L"w")) {
            if (!started) write_modules(out);
            started = true;
            write_samples(out, samples);
            fclose(out);
        }
        BWLOG("stereo: spike: part %d of frame %u lasted %.1f ms (%zu samples from %d ms on; captures/spikes.txt)", part, frame, lasted,
              samples.size(), ms);
    }
}
void run_profiler(int ms) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_game_thread_id);
    if (!th) {
        BWLOG("stereo: profile: can't open the game's thread %lu (%lu)", g_game_thread_id, GetLastError());
        return;
    }
    BWLOG("stereo: profile: sampling thread %lu for %d ms", g_game_thread_id, ms);
    std::vector<std::array<uint32_t, 11>> samples;
    samples.reserve(size_t(ms) * 5);  // (nothing allocated while sampling)
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart >= ms) break;
        if (samples.size() < samples.capacity()) samples.push_back(sample_game_thread(th));
        LARGE_INTEGER w;  // (about 0.25 ms between samples)
        do QueryPerformanceCounter(&w); while ((w.QuadPart - t.QuadPart) * 4000 < f.QuadPart);
    }
    CloseHandle(th);
    if (FILE* out = _wfopen((paths::bwvr_dir() + L"captures/profile.txt").c_str(), L"w")) {
        write_modules(out);
        write_samples(out, samples);
        fclose(out);
    }
    BWLOG("stereo: profile written (%zu samples over %d ms)", samples.size(), ms);
}

// The adaptive detail distance: each frame's own work (from SteamVR's go to the frame's end, and
// the game's work outside the 3D pass before it), smoothed, against the headset's frame time.
float g_detail_k = 1.0f;  // (the share of detail_distance in use)
double g_cost_ema = 0, g_last_outside = 0;
float adaptive_detail_percent() {
    const float pct = float(g_set.detail_distance);
    if (!g_set.adaptive_detail || pct <= 100.0f) return pct;
    return std::max(100.0f, pct * g_detail_k);
}
void adapt_detail(double cost_ms) {
    g_cost_ema = g_cost_ema * 0.9 + cost_ms * 0.1;
    const double budget = 1000.0 / std::max(60.0, double(g_profile.refresh_hz)) - 0.5;
    const float k_min = g_set.detail_distance > 100 ? 100.0f / float(g_set.detail_distance) : 1.0f;
    if (g_cost_ema > budget) g_detail_k = std::max(k_min, g_detail_k - 0.004f);
    else if (g_cost_ema < budget - 1.2) g_detail_k = std::min(1.0f, g_detail_k + 0.002f);
}

void __fastcall process3d_detour(void* game, void*) {
    hand_alive();  // (drops a hand freed by a restart)
    if (g_set.enabled) unpause_after_temple_load(game);
    // Inside the temple the game turns are paused, and CMouse::ProcessPosition (0x61A110), which
    // copies the system's mouse position (TheSystem +0x280, which the laser drives) into the
    // interface's (GInterface +0x428) with GInterface::SendMessage, runs only with the turns. A
    // real mouse's move messages keep it current in the flat game; the laser sends none. So the
    // hall got every click and drag at (0, 0): no turning it, no clicking its doors (player,
    // 2026-10-21). In VR it is called once a frame while the view isn't the world.
    if (g_set.enabled && other_view(game)) reinterpret_cast<void(__thiscall*)(void*)>(0x61A110)(nullptr);
    ui::save_name_tick(g_set.enabled);
    ui::debug_menu_tick();
    if (!g_set.enabled || !eligible(game)) {
        ++g_mono_frames;
        hand::reset();
        uint32_t saved_inc = game::game_time_inc();
        int32_t saved_delta = game::delta_time();
        if (g_set.frozen) game::game_time_inc() = 0, game::delta_time() = 0;
        o_process3d(game);
        game::game_time_inc() = saved_inc;
        game::delta_time() = saved_delta;
        return;
    }
    void* device = game_device();
    g_game_thread_id = GetCurrentThreadId();
    static bool spike_watcher = false;
    if (!spike_watcher) spike_watcher = true, std::thread(spike_watch).detach();
    set_part(1);
    ++g_frame_no;
    QueryPerformanceCounter(&g_fm.entry);
    g_fm.read[0] = g_fm.read[1] = false;
    if (g_ft_last_entry.QuadPart) {
        const double frame = double(g_fm.entry.QuadPart - g_ft_last_entry.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        const double outside = double(g_fm.entry.QuadPart - g_ft_last_exit.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        g_ft.outside += outside;
        g_last_outside = outside;
        g_ft.max_outside = std::max(g_ft.max_outside, outside);
        if (outside > 2.0) ++g_ft.outside_over_2ms;
        g_ft.max_frame = std::max(g_ft.max_frame, frame);
        ++g_ft.frames;
        if (frame > 8.6) ++g_ft.over_120;
        if (frame > 11.4) ++g_ft.over_90;
    }
    g_ft_last_entry = g_fm.entry;
    if (g_sway_amp != 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double t = double(now.QuadPart) / double(g_freq.QuadPart);
        const float yaw = g_pose_ypr[0] + g_sway_amp * float(std::sin(6.283185307 * t / g_sway_period));
        g_head.rot = vr::basis_from_ypr(yaw, g_pose_ypr[1], g_pose_ypr[2]);
    }
    {   // A feedback snapshot, for diagnosing what the player sees: the other hand's B in the
        // headset, Ctrl+F11 at the keyboard. Both eyes (with the json of the cameras), the hand's
        // state in the log, the left eye over the next half second (every 8th frame: motion), a
        // marker in the log to line up with what the player says, and a copy of the log so far
        // (the next launch starts the log afresh). Named by the time, so sessions don't overwrite
        // each other: captureseedback_<date>_<time>*.
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        const bool asked = hand::take_feedback_request();
        if (((down && !was_down) || asked) && g_capture_name.empty()) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char name[48];
            snprintf(name, sizeof(name), "feedback_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            g_capture_name = name;
            g_handinfo_pending = 1;
            g_handtrace_left = 1;
            g_burst_name = std::string(name) + "_burst";
            g_burst_left = 8, g_burst_index = 0, g_burst_every = 8, g_burst_skip = 0;
            BWLOG("==== FEEDBACK %s (%s) ====", name, asked ? "the other hand's B" : "Ctrl+F11");
            g_shadow_log = 2;  // (where the hand's shadow is cast from, for streaks)
            g_cloud_log = 6, g_bb_diag = 6;  // (effects tilting with the head: see billboard_detour)
            {
                const game::LHMatrix& w2c = game::world_to_camera();
                const float* r = reinterpret_cast<const float*>(kBillboardRotation);
                BWLOG("stereo: feedback: the engine camera's up (%.2f, %.2f, %.2f); the game camera's billboard rows right (%.2f, %.2f, %.2f) up (%.2f, %.2f, %.2f); eye-facing clouds %d, one pass %d",
                      w2c.m[0][1], w2c.m[1][1], w2c.m[2][1], r[0], r[1], r[2], r[6], r[7], r[8], g_set.eye_facing_clouds, g_set.one_pass);
            }
            const std::wstring wname(name, name + strlen(name));
            CopyFileW((paths::bwvr_dir() + L"logs\\bwvr.log").c_str(), (paths::bwvr_dir() + L"captures\\" + wname + L".log").c_str(), FALSE);
        }
        was_down = down;
    }
    {   // Ctrl+F12: recenter the tracking origin on the current head pose.
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (down && !was_down) g_recenter = true;
        was_down = down;
    }
    autosave_tick();
    bool hmd_frame = false;
    if (g_set.use_openvr) {
        if (!vr::openvr::active() && !g_openvr_failed) {
            if (!vr::openvr::init()) {
                g_openvr_failed = true;
                BWLOG("stereo: SteamVR unavailable; using the mock head pose");
            }
        }
        // The headset's own optics, once SteamVR is up, whoever started it: the screen for the
        // videos starts it before the first 3D frame, and the eyes were then drawn with the
        // generic profile (90-degree square views: double vision in 0.2.2 on every install
        // without the development copy's hmd_profile.txt).
        static bool profile_from_openvr = false;
        if (vr::openvr::active() && !profile_from_openvr) {
            profile_from_openvr = true;
            g_profile = vr::openvr::profile();
            g_have_ref = false;
            BWLOG("stereo: the headset's optics from SteamVR (eye tangents L %.3f %.3f %.3f %.3f)", g_profile.eye[0].tan_left,
                  g_profile.eye[0].tan_right, g_profile.eye[0].tan_top, g_profile.eye[0].tan_bottom);
        }
        if (vr::openvr::active()) {
            vr::openvr::saving_tick();
            vr::Pose raw;
            if (vr::openvr::wait_frame(raw)) g_head = tracking_relative(raw);
            hmd_frame = true;
            if (g_sway_amp != 0) {  // test aid: sway on top of the live pose
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double t = double(now.QuadPart) / double(g_freq.QuadPart);
                const float yaw = g_sway_amp * float(std::sin(6.283185307 * t / g_sway_period));
                g_head.rot = vr::basis_from_ypr(yaw, 0, 0).compose(g_head.rot);
            }
        }
    }
    QueryPerformanceCounter(&g_fm.waited);
    EngineCamState base;
    base.save();

    const Basis game_rot = basis_of(base.w2c);
    // In the headset the tracking origin never pitches or rolls with the game camera.
    const bool level = g_set.level_base || hmd_frame;
    const Vec3 game_pos{base.cam.pos.x, base.cam.pos.y, base.cam.pos.z};
    const Vec3 base_foc{base.cam.foc.x, base.cam.foc.y, base.cam.foc.z};
    const float focus_dist = std::max(1.0f, vr::length(base_foc - game_pos));

    // Controllers (tracking-relative, like the head).
    vr::Controller ctl[2];
    if (hmd_frame) {
        vr::openvr::read_controllers(ctl);
        for (vr::Controller& c : ctl)
            if (c.active) {
                c.aim = relative_to_ref(c.aim), c.grip = relative_to_ref(c.grip);
                // (And the velocity turned the same way: unturned, a thrown fireball flew off at the
                // recentred heading's angle to the swing; player, 2026-10-30, heading 116 degrees.)
                c.grip_vel = vr::basis_from_ypr(-g_ref_yaw_deg, 0, 0).apply(c.grip_vel);
            }
    } else {
        for (int h = 0; h < 2; ++h) ctl[h] = hand::mock_controllers()[h];
    }
    // With a level base the player's rig places the tracking space (locomotion); otherwise (the
    // offline "base game" tests) the game camera does, as before.
    static LARGE_INTEGER last_frame{};
    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);
    const float dt = last_frame.QuadPart ? std::min(0.1f, float(double(now_qpc.QuadPart - last_frame.QuadPart) /
                                                                  double(g_freq.QuadPart)))
                                         : 0.0f;
    last_frame = now_qpc;
    hand::WorldFrame world{game_pos, level ? vr::basis_look({game_rot.fwd.x, 0, game_rot.fwd.z}) : game_rot,
                           g_set.world_scale};
    const bool rig_was_anchored = hand::rig_anchored();
    // A cutscene (the game's "widescreen" mode: GGame::help_system at +0x25005C, its WideScreen
    // flag at +0x45E8) moves the game's camera by script. The player is carried with it; outside
    // cutscenes the player's rig places the view and the game's camera is put at the head.
    static bool was_cutscene = false;
    bool cutscene = false;
    if (level && g_set.cutscene_carry) {
        const char* help = game::game_field<const char*>(game, 0x25005C);
        cutscene = g_force_cutscene >= 0 ? g_force_cutscene != 0 : help && *reinterpret_cast<const int*>(help + 0x45E8) != 0;
    }
    // Shown as the game's own flat frame on a cinema screen (the convention for scenes that take
    // the camera), or, with cutscene_screen off, by carrying the player with the camera.
    static bool was_other_view = false;
    const bool in_other_view = other_view(game);
    if (in_other_view != was_other_view)
        BWLOG("stereo: the game's view %s (view mode %d)", in_other_view ? "is no longer the world: shown on the screen" : "is the world again",
              game::game_field<int>(game, game::kGameViewMode));
    was_other_view = in_other_view;
    const bool cinema = (cutscene && g_set.cutscene_screen) || in_other_view;
    g_cinema_on = cinema;
    if (cutscene != was_cutscene)
        BWLOG("stereo: cutscene %s", !cutscene ? "ended" : cinema ? "started: on the cinema screen" : "started: the game's camera carries the player");
    ui::set_cinema(cinema);
    hand::set_cinema(cinema, cutscene && !in_other_view);
    if (cinema) {
        world = hand::hold(world);  // the player stays put, watching
    } else if (was_cutscene && !cutscene && g_set.cutscene_screen) {
        world = hand::carry(world, g_head, true);  // play resumes where the scene's camera ended
    } else if (cutscene) {
        const Vec3 cam = world.base_pos, cam_fwd = world.base_rot.fwd;
        world = hand::carry(world, g_head, !was_cutscene);
        static int tick = 0;
        if (g_cutscene_log && ++tick % 30 == 0)
            BWLOG("stereo: cutscene camera (%.2f, %.2f, %.2f) heading %.1f | rig (%.2f, %.2f, %.2f) heading %.1f", cam.x, cam.y, cam.z,
                  std::atan2(cam_fwd.x, cam_fwd.z) / vr::kDegToRad, world.base_pos.x, world.base_pos.y, world.base_pos.z,
                  std::atan2(world.base_rot.fwd.x, world.base_rot.fwd.z) / vr::kDegToRad);
    }
    else if (level) world = hand::locomotion(ctl, g_head, world, dt);
    else hand::rig_reset();
    const bool cutscene_ended = was_cutscene && !cutscene;
    was_cutscene = cutscene;
    const bool g_rig_anchored_now = level && !rig_was_anchored;
    const Basis base_rot = world.base_rot;
    const Vec3 base_pos = world.base_pos;
    const float scale = world.scale;
    // The comfort vignette (VR option): how much the player is being moved by the rig (the stick,
    // smooth turning, quick travel, the world grab), not by walking about: 0 still, 1 moving.
    // Jumps (snap turns, a recentre, a cutscene's end) don't count.
    {
        static Vec3 last_pos{};
        static float last_yaw = 0;
        static bool have = false;
        const float yaw = std::atan2(base_rot.fwd.x, base_rot.fwd.z) / vr::kDegToRad;
        float target = 0;
        if (have && dt > 0 && !cinema && level) {
            float dyaw = std::fabs(yaw - last_yaw);
            if (dyaw > 180.0f) dyaw = 360.0f - dyaw;
            const float moved_m = vr::length(base_pos - last_pos) / std::max(1e-3f, scale);
            if (dyaw < 10.0f && moved_m < 1.0f)  // (more in one frame: a jump)
                target = std::clamp(std::max((moved_m / dt - 0.2f) / 1.3f, (dyaw / dt - 5.0f) / 40.0f), 0.0f, 1.0f);
        }
        last_pos = base_pos, last_yaw = yaw, have = true;
        // In over 0.15 s, out over 0.4 s.
        const float rate = target > g_vignette ? dt / 0.15f : dt / 0.4f;
        g_vignette += std::clamp(target - g_vignette, -rate, rate);
    }

    // Keep the game's own camera where the player's head is: the game measures hand reach,
    // sound, and interface rays from it. Log how far the game moved it since we set it (its
    // camera modes, the tutorial's limits, scripted cameras).
    static Vec3 last_set{};
    static bool have_last = false;
    // The game's camera modes are a stack in GCamera (+0x28, top index at +0x58). While the hand
    // strokes or slaps the creature the game runs CameraModeCtrInteract (vtable 0x8C7740, started
    // by CHand::OperateInteractionCamera) and moves its camera itself; that must not be taken for
    // the tutorial's force field pushing the player back.
    bool creature_camera = false;
    {
        const char* cam = static_cast<const char*>(game::game_camera_object());
        const int top = cam ? *reinterpret_cast<const int*>(cam + 0x58) : -1;
        const void* const* mode = cam && top >= 0 && top < 12 ? *reinterpret_cast<const void* const* const*>(cam + 0x28 + top * 4) : nullptr;
        const void* vtable = mode ? *mode : nullptr;
        creature_camera = vtable == reinterpret_cast<const void*>(0x8C7740);
        static const void* last_vtable = nullptr;
        if (vtable != last_vtable) {
            BWLOG("stereo: game camera mode %p (stack depth %d)%s", vtable, top + 1, creature_camera ? ": creature interaction" : "");
            last_vtable = vtable;
        }
    }
    if (cutscene || cutscene_ended) have_last = false;  // the game moved its camera itself: nothing to compare against
    if (level && g_set.sync_game_camera && !cutscene) {
        const Vec3 moved = game_pos - last_set;
        hand::set_game_resisting(have_last && !creature_camera && vr::length(moved) > 0.05f);
        if (have_last && vr::length(moved) > 0.01f) {
            static int logged = 0;
            if (logged++ < 20)
                BWLOG("stereo: game moved its camera by (%.3f, %.3f, %.3f) since the last sync; now (%.3f, %.3f, %.3f) set (%.3f, %.3f, %.3f)",
                      moved.x, moved.y, moved.z, game_pos.x, game_pos.y, game_pos.z, last_set.x, last_set.y, last_set.z);
        }
        // Position follows the head; orientation stays the rig's level heading. The game hangs
        // some scenery off its camera's orientation (the 3D clouds), so a head-following
        // orientation would swing it around with every head turn.
        // Its direction follows the head's yaw and pitch, smoothed (~0.5 s), never its roll.
        // The game uses its camera's direction for more than the view: the 3D clouds are flat
        // sprites turned to face it (a fixed direction showed them edge-on when looking aside,
        // the raw head orientation swung them with every head motion), and a carried object is
        // kept in its camera's view (a camera looking elsewhere held it back).
        const Vec3 head_fwd = base_rot.apply(g_head.rot.fwd);
        const float head_yaw = std::atan2(head_fwd.x, head_fwd.z);
        const float head_pitch = std::asin(std::clamp(head_fwd.y, -1.0f, 1.0f));
        static float cam_yaw = 0, cam_pitch = 0;
        static bool have_dir = false;
        if (!have_dir || g_rig_anchored_now || cutscene_ended) cam_yaw = head_yaw, cam_pitch = head_pitch, have_dir = true;
        float dyaw = head_yaw - cam_yaw;
        while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
        while (dyaw < -3.14159265f) dyaw += 6.2831853f;
        const float k = 1.0f - std::exp(-dt / 0.5f);
        cam_yaw += dyaw * k;
        cam_pitch += (head_pitch - cam_pitch) * k;
        const Vec3 head = base_pos + base_rot.apply(g_head.pos * scale);
        const Vec3 fwd{std::sin(cam_yaw) * std::cos(cam_pitch), std::sin(cam_pitch),
                       std::cos(cam_yaw) * std::cos(cam_pitch)};
        game::set_game_camera({head.x, head.y, head.z},
                              {head.x + fwd.x * focus_dist, head.y + fwd.y * focus_dist, head.z + fwd.z * focus_dist});
        last_set = head;
        have_last = true;
    }

    auto eye_view = [&](int e) {
        const EyeGeometry g = eye_geometry(g_profile.eye[e]);
        Vec3 offset = g_profile.eye[e].offset;
        offset.x *= g_set.ipd_scale;
        hand::EyeView v;
        v.rot = base_rot.compose(g_head.rot);
        v.pos = base_pos + base_rot.apply((g_head.pos + g_head.rot.apply(offset)) * scale);
        v.f = g.focal_px;
        v.cx = g.render_w * 0.5f;
        v.cy = g.render_h * 0.5f;
        return v;
    };
    {   // the 2D layout is the window's size (the eye passes change the engine's resolution)
        const game::InfoTransform& t = game::info_transform();
        ui::begin_frame(world, g_head, t.resolution.x, t.resolution.y);
    }
    {   // (for the sound: where the head is in the world, and where it looks)
        const Basis r = base_rot.compose(g_head.rot);
        g_listener = {true, base_pos + base_rot.apply(g_head.pos * scale), r.fwd, r.up};
    }
    // (The picking eye: the one the engine draws from. With one pass, the point between the eyes:
    // what the cursor is over is worked out in that pass, and the left eye's view put the cursor
    // tens of pixels off for things by the hand.)
    hand::EyeView pick_view = eye_view(0);
    if (g_set.one_pass) {
        Vec3 mo{};
        for (const vr::EyeOptics& eo : g_profile.eye) mo = mo + eo.offset * 0.5f;
        mo.x *= g_set.ipd_scale;
        pick_view.pos = base_pos + base_rot.apply((g_head.pos + g_head.rot.apply(mo)) * scale);
    }
    hand::update(ctl, world, pick_view);
    ui::place_gesture_panel(world, g_head);  // (after the hands: this frame's left hand, this frame's rig)

    // Cinema: the game draws its own frame once, flat, to its own target (the window shows it as
    // the flat game would); that frame becomes the picture on the screen in the headset.
    bool cinema_frame = false;
    if (cinema) {
        g_cinema_pass = true;
        o_process3d(game);
        g_cinema_pass = false;
        cinema_frame = g_cinema_texture.update(device);
    }

    const bool capturing = !g_capture_name.empty();
    const std::wstring stem = capturing ? paths::bwvr_dir() + L"captures\\" +
                                              std::wstring(g_capture_name.begin(), g_capture_name.end())
                                        : L"";
    EyeCapture caps[2];
    EyeGeometry geo[2];
    bool ok = true;
    // One engine pass for both eyes ([stereo] one_pass): the engine draws once, from between the
    // eyes, into a recording (d3d7_trace), which is replayed into each eye's target with every
    // vertex moved to that eye. The eyes share one symmetric geometry, so the point between them
    // sees what both see (bar a sliver at the outer edges for things very near). Not for the
    // cinema screen or state diffs (those compare the eyes' own passes). (A capture's camera
    // notes are the recording camera's for both eyes.)
    const bool one_pass = g_set.one_pass && !cinema && g_statediff_name.empty();
    Vec3 mid_offset{};
    for (const vr::EyeOptics& eo : g_profile.eye) mid_offset = mid_offset + eo.offset * 0.5f;
    mid_offset.x *= g_set.ipd_scale;
    static double rec_ms = 0, replay_ms = 0;
    static unsigned rec_frames = 0;

    for (int e = 0; e < 2 && ok; ++e) {
        const vr::EyeOptics& o = g_profile.eye[e];
        geo[e] = eye_geometry(o);
        if (!g_targets[e].ensure(device, geo[e].render_w, geo[e].render_h) ||
            !g_targets[e].bind(device, cinema ? kCinemaDark : kClearColor)) {
            ok = false;
            break;
        }
        // Eye pose: base * head * eye offset, with tracking-space meters scaled to world units.
        Vec3 offset = o.offset;
        offset.x *= g_set.ipd_scale;
        if (one_pass) offset = mid_offset;  // (the engine's camera: between the eyes)
        const Basis eye_rot = base_rot.compose(g_head.rot);
        const Vec3 eye_pos = base_pos + base_rot.apply((g_head.pos + g_head.rot.apply(offset)) * scale);

        game::update_viewport(geo[e].render_w, geo[e].render_h);
        game::change_fov(geo[e].fov);
        // The engine's near plane (1.8 units: 18 cm at the default scale) follows the player's
        // scale, so hands can come as close to the face when scaled in. The depth buffer holds
        // z = 1 - near/Z, so a nearer plane costs depth precision far away: no less than a quarter.
        const float engine_near = game::info_transform().near_clip;
        g_engine_near = engine_near;
        // With a 24-bit z-buffer the plane sits 5 cm from the eye at any scale; with the game's
        // 16 bits it only follows the scale downwards, to a quarter.
        game::info_transform().near_clip = g_targets[e].depth_precision() >= 24
                                               ? std::clamp(g_set.near_m * scale, 0.05f, engine_near)
                                               : engine_near * std::clamp(scale / 10.0f, 0.25f, 1.0f);
        hand::set_depth_scale(game::info_transform().near_clip);
        game::fit_sea(geo[e].render_w, geo[e].render_h);
        set_engine_camera(eye_pos, eye_rot, focus_dist);

        if (capturing) {
            caps[e].geo = geo[e];
            caps[e].pos = eye_pos;
            caps[e].rot = eye_rot;
            caps[e].w2c = game::world_to_camera();
            probe_points(base_foc, caps[e].probes);
        }

        // Second eye: freeze everything time-driven so it renders the same instant.
        uint32_t saved_inc = game::game_time_inc();
        int32_t saved_delta = game::delta_time();
        int saved_liquid = game::game_field<int>(game, game::kGameLiquidTimeMs);
        if ((e == 1 && g_set.zero_time) || g_set.frozen) {
            game::game_time_inc() = 0;
            game::delta_time() = 0;
            game::game_field<int>(game, game::kGameLiquidTimeMs) = 0;
        }
        if (e == 0) {
            g_trees_left.clear();
            g_sizes_left.clear();
            g_sizes_obj.clear();
            g_left_draw = {};
            static ULONGLONG reported = 0;
            if (GetTickCount64() - reported > 5000) {
                static int quiet = 0;
                if (g_trees_differ || g_sizes_missed > 50 || !g_size_by_place || ++quiet >= 12) {
                    BWLOG("trees: %u of %u trees drawn differently in the right eye; detail checks of objects drawn in several places %u, right-eye checks "
                          "without the left eye's %u, carried things given a far place's %u; draws L %u R %u (5 s)", g_trees_differ, g_trees_compared, g_sizes_shared, g_sizes_missed, g_sizes_far, g_draws_eye[0], g_draws_eye[1]);
                    quiet = 0;
                }
                g_trees_differ = g_trees_compared = 0;
                g_sizes_shared = g_sizes_missed = g_sizes_far = 0;
                g_draws_eye[0] = g_draws_eye[1] = 0;
                reported = GetTickCount64();
            }
        }
        if (e == 0) held_track();
        if (e == 0 && g_drop_log > 0) {
            --g_drop_log;
            const void* r = hand::recent_release();
            const float* m = r ? reinterpret_cast<const float*>(static_cast<const char*>(r) + 0x14) : nullptr;
            BWLOG("drops: frame: held %p released %p at (%.2f, %.2f, %.2f)", held_object_3d(), r, m ? m[9] : 0.f, m ? m[10] : 0.f,
                  m ? m[11] : 0.f);
        }
        if (!one_pass) {  // (the second eye's pass undoing the first's side effects: one pass has none)
            if (e == 1) held_to_game_place();
            if (e == 0) rewind_save();
            else rewind_restore();
        }
        // The disciple icon over a villager held above a building (Villager::ShowDiscipleIcon sets
        // the villager at 0xEB99E4 each turn) is cleared by LH3DRender::FinishFrame, which ends
        // each eye pass: the right eye never drew it. It is put back for the right eye.
        {
            static uint32_t disciple_icon = 0;
            uint32_t& icon = *reinterpret_cast<uint32_t*>(0xEB99E4);
            // (debug: stereo disciple_force N: the held villager shown as disciple N, as the game
            // does over a field (Villager::ShowDiscipleIcon), for tests: the harness's hand doesn't
            // make the game offer it.)
            if (e == 0 && g_disciple_force > 0 && hand_alive()) {
                const char* status = *reinterpret_cast<const char* const*>(static_cast<char*>(g_hand) + 0x4850);
                void* held = status && status[0x90] ? *reinterpret_cast<void* const*>(status + 0x120) : nullptr;
                if (held) reinterpret_cast<void(__thiscall*)(void*, int)>(0x756170)(held, g_disciple_force);
            }
            if (e == 0) drawn_frame_begins();
            // (Only while the game keeps asking for it: put back for the right eye after the game had
            // cleared it, it stayed into the next frame, and so on for good: the carry ray kept the
            // colour of the last place the villager was held over; player, 2026-11-07.)
            if (e == 0 && GetTickCount64() - g_disciple_asked_ms > 300) icon = 0;
            if (e == 0) disciple_icon = icon, g_disciple_obj3d = icon, g_disciple_type = *reinterpret_cast<const int*>(0xEB99E8);
            else icon = disciple_icon;
        }
        const bool statediff = !g_statediff_name.empty();
        if (statediff) snapshot();
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        g_fm.eye_start[e] = t0;
        set_part(2 + e);
        g_current_eye = e;
        {
            hand::EyeView view = eye_view(e);
            if (one_pass) view.pos = eye_pos;  // (the engine draws from between the eyes)
            ui::set_eye_pass(e, &view);
        }
        g_current_eye_for_billboards = g_set.eye_facing_clouds ? e : -1;
        g_billboard_from = (eye_view(0).pos + eye_view(1).pos) * 0.5f;
        if (cinema) {
            // The screen: the frame's texture on the panel, as this eye sees it.
            const hand::EyeView view = eye_view(e);
            Vec3 corner[4];
            float quad[4][3];
            bool visible = cinema_frame && ui::screen_corners(corner);
            for (int i = 0; i < 4 && visible; ++i) {
                const Vec3 r = corner[i] - view.pos;
                const float Z = vr::dot(r, view.rot.fwd);
                if (Z < 0.2f * scale) visible = false;  // beside or behind the eye
                quad[i][0] = view.cx + view.f * vr::dot(r, view.rot.right) / Z;
                quad[i][1] = view.cy - view.f * vr::dot(r, view.rot.up) / Z;
                quad[i][2] = 1.0f / Z;
            }
            if (visible) render::draw_textured_quad(device, g_cinema_texture.surface(), quad);
        } else {
            {
                const char* what = render::set_texture_filtering(device, g_set.texture_filter, g_set.anisotropy, g_set.lod_bias);
                static int logged_mode = -1;
                if (logged_mode != g_set.texture_filter) logged_mode = g_set.texture_filter, BWLOG("stereo: texture filtering in the eye passes: %s", what);
            }
            // Objects are drawn only from the land blocks nearer the camera than the engine's vanish
            // distance (700 units: 70 m at the god's VR size), so in the headset, looking out across
            // the land, everything further away was missing (player, 2026-10-19). Longer in the
            // eye passes; the game's own value is kept for everything else.
            float& vanish = *reinterpret_cast<float*>(kVanishObjectDist);
            const float game_vanish = vanish;
            if (g_set.object_distance > 0) vanish = game_vanish * float(g_set.object_distance) / 100.0f;
            // Objects also go to less detailed meshes and then fade out with distance: the engine
            // compares the distance with multiples (23.3, 66.7, 86.7 and 173.3) of a scale: the
            // object's size on the screen, as CheckRegionOnScreen just worked it out (0xC37EA0),
            // times the game's level of detail, capped at 5 (fn_00815A70; animated things,
            // fn_00816AD0, without the cap). An eye has about half the flat game's pixels per
            // degree, so everything counted as half as big and lost detail and faded out at half
            // the distance: trees went plain close by, far things and fences faded away (player,
            // 2026-10-19 and -20). In the eye passes the size is scaled after each check
            // (region_detour) and the cap raised with it.
            // (Raising 0xC37EA0 here, as before, did nothing: every check overwrites it.)
            float& detail = *reinterpret_cast<float*>(kDetailDistCap);
            const float game_detail = detail;
            if (g_set.detail_distance > 0) {
                const float pct = adaptive_detail_percent();
                detail = game_detail * pct / 100.0f;
                g_size_boost = pct / 100.0f;
            }
            if (!one_pass || e == 0) {
                LARGE_INTEGER r0, r1;
                QueryPerformanceCounter(&r0);
                if (one_pass) trace::begin_record();
                trace::set_eye_pass(true);
                o_process3d(game);
                trace::set_eye_pass(false);
                if (one_pass) trace::end_record();
                QueryPerformanceCounter(&r1);
                if (one_pass) rec_ms += double(r1.QuadPart - r0.QuadPart) * 1000.0 / double(g_freq.QuadPart), ++rec_frames;
            }
            if (one_pass) {
                // This eye's offset from the recording camera, in the camera's axes (right, up,
                // forward) and world units.
                Vec3 off = o.offset;
                off.x *= g_set.ipd_scale;
                const Vec3 dv = (off - mid_offset) * (scale * g_one_pass_k);
                const hand::EyeView ev = eye_view(e);
                LARGE_INTEGER r0, r1;
                QueryPerformanceCounter(&r0);
                trace::replay(device, ev.f, ev.cx, ev.cy, dv.x, dv.y, dv.z, game::info_transform().near_clip);
                QueryPerformanceCounter(&r1);
                replay_ms += double(r1.QuadPart - r0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
                static ULONGLONG said = 0;
                if (e == 1 && GetTickCount64() - said > 5000 && rec_frames) {
                    BWLOG("stereo: one pass: recording %.2f ms, replay %.2f ms an eye (%u frames); last: %s", rec_ms / rec_frames,
                          replay_ms / (2.0 * rec_frames), rec_frames, trace::record_stats().c_str());
                    rec_ms = replay_ms = 0, rec_frames = 0;
                    said = GetTickCount64();
                }
            }
            if (e == 0) blink_frame_end();
            if (!one_pass) {
                // The two eyes should draw nearly the same (the right one a little more or less at
                // the edges). Far fewer in the right eye is things seen by the left eye only.
                static unsigned left_draws = 0;
                unsigned draws = 0, prims = 0;
                trace::pass_draws(draws, prims);
                g_draws_eye[e ? 1 : 0] += draws;
                if (e == 0) left_draws = draws;
                else if (left_draws > 50 && draws < left_draws * 8 / 10) {
                    static int logged = 0;
                    if (logged++ < 40) BWLOG("stereo: the right eye drew %u draws, the left %u", draws, left_draws);
                }
            }
            vanish = game_vanish;
            detail = game_detail;
            g_size_boost = 1.0f;
        }
        g_current_eye = -1;
        ui::set_eye_pass(-1, nullptr);
        g_current_eye_for_billboards = -1;
        glow_add_undo();  // (the glow's added light: drawn by now)
        hand::draw(device, eye_view(e));
        // The comfort vignette, over everything in the eye while the player is moved.
        if (const int level_v = hand::comfort_vignette(); level_v > 0 && g_vignette > 0.01f && !cinema) {
            const hand::EyeView v = eye_view(e);
            const float in_deg = level_v >= 2 ? 26.0f : 38.0f, out_deg = level_v >= 2 ? 48.0f : 60.0f;
            const float k = g_vignette;  // (it closes in as it comes on)
            const float r_in = v.f * std::tan((in_deg + (1.0f - k) * 25.0f) * vr::kDegToRad);
            const float r_out = v.f * std::tan(std::min(85.0f, out_deg + (1.0f - k) * 15.0f) * vr::kDegToRad);
            render::draw_vignette(device, v.cx, v.cy, r_in, r_out, k);
        }
        // Teleport movement: a moment of black over the jump.
        if (const float fade = hand::teleport_fade(); fade > 0.01f && !cinema) {
            const hand::EyeView v = eye_view(e);
            render::draw_vignette(device, v.cx, v.cy, 0.0f, 1.0f, fade);
        }
        game::info_transform().near_clip = engine_near;
        g_engine_near = 0;
        QueryPerformanceCounter(&t1);
        g_fm.eye_end[e] = t1;
        set_part(1);
        if (statediff) {
            std::wstring p = paths::bwvr_dir() + L"captures\\" +
                             std::wstring(g_statediff_name.begin(), g_statediff_name.end()) + L".statediff";
            if (FILE* f = _wfopen(p.c_str(), e == 0 ? L"w" : L"a")) {
                write_statediff(f, e == 0 ? "L" : "R");
                fclose(f);
            }
            if (e == 1) g_statediff_name.clear();
        }
        g_eye_ms[e] += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        game::game_time_inc() = saved_inc;
        game::delta_time() = saved_delta;
        game::game_field<int>(game, game::kGameLiquidTimeMs) = saved_liquid;

        if (capturing) save_eye_images(g_targets[e], geo[e], stem + (e ? L"_R" : L"_L"), caps[e]);
        // (Each frame's left eye as <name>_NNN.bmp, and with "burst_both 1" its right eye as
        // <name>_NNN_R.bmp too.)
        static bool burst_frame = false;
        if (e == 0) burst_frame = g_burst_left > 0 && (g_burst_skip++ % std::max(1, g_burst_every)) == 0;
        if (burst_frame && (e == 0 || g_burst_both)) {
            std::vector<uint8_t> bgr;
            int stride = 0;
            wchar_t idx[16];
            swprintf(idx, 16, e == 0 ? L"_%03d.bmp" : L"_%03d_R.bmp", e == 0 ? g_burst_index : g_burst_index - 1);
            if (e == 0) ++g_burst_index;
            if (g_targets[e].read_color(geo[e].crop, bgr, stride))
                render::write_bmp(paths::bwvr_dir() + L"captures\\" + std::wstring(g_burst_name.begin(), g_burst_name.end()) + idx,
                                  geo[e].crop.w, geo[e].crop.h, bgr);
        }
        if (e == 1 && burst_frame && --g_burst_left == 0) BWLOG("stereo: burst '%s' done", g_burst_name.c_str());
        g_targets[e].unbind(device);
    }
    // To the headset: each eye's picture read back from its render target and uploaded for
    // SteamVR, straight into the upload texture (one copy less than through a buffer).
    for (int e = 0; e < 2 && hmd_frame && ok; ++e) {
        set_part(4);
        LARGE_INTEGER r0, r1;
        QueryPerformanceCounter(&r0);
        // Through dxwrapper (the game on Direct3D 9) the eye goes to SteamVR on the GPU, without a
        // readback: the crop is copied into a texture shared with Direct3D 11.
        void *dev9 = nullptr, *surf9 = nullptr;
        if (g_set.share_eyes && render::d3d9::surface(g_targets[e].surface(), &dev9, &surf9) &&
            vr::openvr::share_eye(e, dev9, surf9, geo[e].crop.x, geo[e].crop.y, geo[e].crop.w, geo[e].crop.h)) {
            QueryPerformanceCounter(&r1);
            g_readback_ms += double(r1.QuadPart - r0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
            g_fm.read_end[e] = r1, g_fm.read[e] = true;
            continue;
        }
        uint8_t* dst = nullptr;
        int pitch = 0;
        if (vr::openvr::map_eye(e, geo[e].crop.w, geo[e].crop.h, dst, pitch)) {
            g_targets[e].read_into(geo[e].crop, dst, pitch);
            vr::openvr::submit_mapped_eye(e);
        }
        QueryPerformanceCounter(&r1);
        g_readback_ms += double(r1.QuadPart - r0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        g_fm.read_end[e] = r1, g_fm.read[e] = true;
    }

    base.restore();
    {
        const auto& res = game::info_transform().resolution;
        game::fit_sea(res.x, res.y);
    }
    if (!ok) {
        BWLOG("stereo: eye target setup failed; disabling stereo");
        g_set.enabled = false;
        o_process3d(game);
        return;
    }
    if (g_set.mirror && !cinema) mirror(device, geo);  // (cinema: the window already has the game's own frame)
    // A copy of the window's frame for the game's pictures (every tenth frame).
    static int frame_copy = 0;
    if (g_set.save_pictures && (g_set.mirror || cinema) && ++frame_copy % 10 == 0) g_last_frame_ok = g_last_frame.update(device);
    if (capturing) {
        write_capture_meta(stem + L".json", base, caps);
        BWLOG("stereo: captured '%s' (color %d/%d, depth %d/%d)", g_capture_name.c_str(), caps[0].color_ok,
              caps[1].color_ok, caps[0].depth_ok, caps[1].depth_ok);
        g_capture_name.clear();
    }
    if (hmd_frame && !g_mirror_name.empty()) save_mirror();
    ++g_stereo_frames;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_last_vr_frame_s = double(now.QuadPart) / double(g_freq.QuadPart);
    }
    ++g_frame_counter;
    {
        LARGE_INTEGER exit;
        QueryPerformanceCounter(&exit);
        auto ms = [](const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return double(b.QuadPart - a.QuadPart) * 1000.0 / double(g_freq.QuadPart); };
        g_ft.wait += ms(g_fm.entry, g_fm.waited);
        if (hmd_frame) adapt_detail(ms(g_fm.waited, exit) + g_last_outside);
        g_ft.setup += ms(g_fm.waited, g_fm.eye_start[0]);
        for (int e = 0; e < 2; ++e) g_ft.eye[e] += ms(g_fm.eye_start[e], g_fm.eye_end[e]);
        g_ft.max_eyes = std::max(g_ft.max_eyes, ms(g_fm.eye_start[0], g_fm.eye_end[0]) + ms(g_fm.eye_start[1], g_fm.eye_end[1]));
        if (g_fm.read[1]) g_ft.max_read = std::max(g_ft.max_read, ms(g_fm.eye_end[1], g_fm.read_end[1]));
        if (g_fm.read[0]) g_ft.read[0] += ms(g_fm.eye_end[1], g_fm.read_end[0]);
        if (g_fm.read[1]) g_ft.read[1] += ms(g_fm.read_end[0], g_fm.read_end[1]);
        g_ft.after += ms(g_fm.read[1] ? g_fm.read_end[1] : g_fm.eye_end[1], exit);
        g_ft_last_exit = exit;
    }
    set_part(0);
}

}  // namespace

void stop_spike_watch() { stop_spikes(); }

void configure() {
    QueryPerformanceFrequency(&g_freq);
    g_set.enabled = paths::ini_int(L"stereo", L"enabled", 1) != 0;  // (VR from launch; player, 2026-11-02)
    g_set.eye_height = paths::ini_int(L"stereo", L"eye_height", 1800);
    g_set.mirror = paths::ini_int(L"stereo", L"mirror", 1);
    g_set.second_hand = paths::ini_int(L"stereo", L"second_hand", 1) != 0;
    g_set.head_audio = paths::ini_int(L"stereo", L"head_audio", 1) != 0;
    g_set.texture_filter = paths::ini_int(L"stereo", L"texture_filter", 1);
    g_set.object_distance = paths::ini_int(L"stereo", L"object_distance_percent", 250);
    g_set.detail_distance = paths::ini_int(L"stereo", L"detail_distance_percent", 150);
    g_set.one_pass = paths::ini_int(L"stereo", L"one_pass", 1) != 0;
    g_set.adaptive_detail = paths::ini_int(L"stereo", L"adaptive_detail", 1) != 0;
    g_small_w = float(paths::ini_int(L"stereo", L"small_size", 600));
    g_small_boost = float(paths::ini_int(L"stereo", L"small_detail_percent", 200)) / 100.0f;
    g_small_floor_w = float(paths::ini_int(L"stereo", L"small_floor_size", 1600));
    g_set.miracle_on_palm = paths::ini_int(L"stereo", L"miracle_on_palm", 1) != 0;
    g_set.miracle_grip = paths::ini_int(L"stereo", L"miracle_grip", 1) != 0;
    g_set.miracle_effect_m = float(paths::ini_int(L"stereo", L"miracle_effect_cm", 12)) / 100.0f;
    g_set.shadow_on_ground = paths::ini_int(L"stereo", L"shadow_on_ground", 1) != 0;
    g_set.same_trees = paths::ini_int(L"stereo", L"same_trees", 1) != 0;
    g_set.helper_spread = paths::ini_int(L"stereo", L"helper_spread_percent", 70);
    g_set.share_eyes = paths::ini_int(L"stereo", L"share_eyes", 1) != 0;
    g_set.anisotropy = paths::ini_int(L"stereo", L"anisotropy", 8);
    {
        wchar_t lb[32];
        GetPrivateProfileStringW(L"textures", L"lod_bias", L"0", lb, 32, paths::ini_file().c_str());
        g_set.lod_bias = float(_wtof(lb));
    }
    g_set.unsquash_hand = paths::ini_int(L"stereo", L"unsquash_hand", 1) != 0;
    g_set.cutscene_carry = paths::ini_int(L"stereo", L"cutscene_carry", 1) != 0;
    g_set.saving_cover = paths::ini_int(L"vr", L"saving_cover", 1) != 0;
    g_set.autosave_fade_first = paths::ini_int(L"vr", L"autosave_fade_first", 0) != 0;
    g_set.saving_screen = paths::ini_int(L"vr", L"saving_screen", 0) != 0;
    g_set.cutscene_screen = paths::ini_int(L"stereo", L"cutscene_screen", 1) != 0;
    g_set.other_views_on_screen = paths::ini_int(L"stereo", L"other_views_on_screen", 1) != 0;
    g_set.temple_4x3 = paths::ini_int(L"stereo", L"temple_4x3", 1) != 0;
    g_set.temple_unpause = paths::ini_int(L"stereo", L"temple_unpause", 1) != 0;
    g_set.save_pictures = paths::ini_int(L"stereo", L"save_pictures", 1) != 0;
    g_set.zero_time = paths::ini_int(L"stereo", L"zero_time", 1) != 0;
    g_set.level_base = paths::ini_int(L"stereo", L"level_base", 0) != 0;
    g_set.sync_game_camera = paths::ini_int(L"stereo", L"sync_game_camera", 1) != 0;
    g_set.hide_mouse_hints = paths::ini_int(L"stereo", L"hide_mouse_hints", 1) != 0;
    wchar_t buf[64];
    GetPrivateProfileStringW(L"stereo", L"world_scale", L"10", buf, 64, paths::ini_file().c_str());
    g_set.world_scale = float(_wtof(buf));
    g_profile = vr::load_profile(paths::bwvr_dir() + L"hmd_profile.txt");
    wchar_t backend[32];
    GetPrivateProfileStringW(L"vr", L"backend", L"openvr", backend, 32, paths::ini_file().c_str());
    g_set.use_openvr = _wcsicmp(backend, L"openvr") == 0;
    // The test harness starts the game flat (BWVR_START=mono) or with the scripted head (mock),
    // so a test run never takes the headset; the player's game is in VR from its first frame.
    {
        wchar_t start[16] = {};
        if (GetEnvironmentVariableW(L"BWVR_START", start, 16) > 0) {
            if (_wcsicmp(start, L"mono") == 0) g_set.enabled = false, g_set.use_openvr = false;
            else if (_wcsicmp(start, L"mock") == 0) g_set.enabled = true, g_set.use_openvr = false;
            BWLOG("stereo: BWVR_START=%ls", start);
        }
    }
    hand::configure();
    ui::configure();
    BWLOG("stereo: enabled=%d (%s) world_scale=%.2f eye_height=%d level_base=%d zero_time=%d", g_set.enabled,
          g_set.use_openvr ? "openvr" : "mock", g_set.world_scale, g_set.eye_height, g_set.level_base, g_set.zero_time);
}

// The game's 3D sound: once a game turn GAudio (fn_004270D0) gives the audio system its listener
// (LH_AudioSystem::LHListenerUpdate(position, focus point, up), through the import table at
// 0x8A971C) from the game's camera. In VR that camera stays level and doesn't turn with the head,
// so sounds didn't move as the player looked round. The listener is now the head: its position,
// a focus point along its gaze (as far out as the game's), and its up.
using ListenerUpdateFn = void(__thiscall*)(void* audio, float* pos, float* focus, float* up);
ListenerUpdateFn o_listener_update = nullptr;
void __fastcall listener_update_detour(void* audio, void*, float* pos, float* focus, float* up) {
    static bool seen = false;
    if (!seen) seen = true, BWLOG("stereo: the game updates the sound listener (stereo %d)", int(g_set.enabled));
    if (!g_set.enabled || !g_set.head_audio || !g_listener.valid || !pos || !focus || !up) {
        o_listener_update(audio, pos, focus, up);
        return;
    }
    const Listener& l = g_listener;
    const float dist = std::max(1.0f, std::sqrt((focus[0] - pos[0]) * (focus[0] - pos[0]) + (focus[1] - pos[1]) * (focus[1] - pos[1]) +
                                                (focus[2] - pos[2]) * (focus[2] - pos[2])));
    float p[3] = {l.pos.x, l.pos.y, l.pos.z};
    float f[3] = {l.pos.x + l.fwd.x * dist, l.pos.y + l.fwd.y * dist, l.pos.z + l.fwd.z * dist};
    float u[3] = {l.up.x, l.up.y, l.up.z};
    static int logged = 0;
    if (logged < 1) {
        ++logged;
        BWLOG("stereo: sound listener from the head: (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f); the game's was (%.1f, %.1f, %.1f) up (%.2f, %.2f, %.2f)",
              p[0], p[1], p[2], l.fwd.x, l.fwd.y, l.fwd.z, pos[0], pos[1], pos[2], up[0], up[1], up[2]);
    }
    o_listener_update(audio, p, f, u);
}
bool patch_listener_import() {
    void** slot = reinterpret_cast<void**>(0x8A971C);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    o_listener_update = reinterpret_cast<ListenerUpdateFn>(*slot);
    *slot = reinterpret_cast<void*>(&listener_update_detour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return o_listener_update != nullptr;
}

bool install_hooks() {
    if (MH_CreateHookApi(L"kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&exit_process_detour), reinterpret_cast<void**>(&o_exit_process)) != MH_OK)
        BWLOG("stereo: ExitProcess not hooked: the spike watcher is off"), g_spike_ms = 0;
    if (MH_CreateHook(reinterpret_cast<void*>(0x7C9F21), reinterpret_cast<void*>(&doexit_detour), reinterpret_cast<void**>(&o_doexit)) != MH_OK)
        BWLOG("stereo: the game's exit not hooked: the spike watcher is off"), g_spike_ms = 0;
    if (!patch_listener_import()) BWLOG("stereo: could not take over the sound listener (sounds stay with the game's camera)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::GGame_Process3dEngine), reinterpret_cast<void*>(&process3d_detour),
                      reinterpret_cast<void**>(&o_process3d)) != MH_OK) {
        BWLOG("FATAL: failed to hook GGame::Process3dEngine");
        return false;
    }
    BWLOG("hooked GGame::Process3dEngine at %08X", unsigned(sym::GGame_Process3dEngine));
    if (MH_CreateHook(reinterpret_cast<void*>(sym::LH3DTech_ChangeFov), reinterpret_cast<void*>(&change_fov_detour),
                      reinterpret_cast<void**>(&o_change_fov)) != MH_OK)
        BWLOG("stereo: could not hook LH3DTech::ChangeFov (the temple keeps its 16:9 view)");
    if (MH_CreateHook(reinterpret_cast<void*>(kScriptedScreenShotProcess), reinterpret_cast<void*>(&screenshot_process_detour),
                      reinterpret_cast<void**>(&o_screenshot_process)) != MH_OK)
        BWLOG("stereo: could not hook ScriptedScreenShot::Process (save pictures stay pink)");
    MH_CreateHook(reinterpret_cast<void*>(0x7947C0), reinterpret_cast<void*>(&temple_mouse_detour), reinterpret_cast<void**>(&o_temple_mouse));
    MH_CreateHook(reinterpret_cast<void*>(kTempleGoToRoom), reinterpret_cast<void*>(&go_to_room_detour),
                  reinterpret_cast<void**>(&o_go_to_room));
    if (MH_CreateHook(reinterpret_cast<void*>(kGoInsideCitadel), reinterpret_cast<void*>(&go_inside_detour),
                      reinterpret_cast<void**>(&o_go_inside)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(kLeaveInsideCitadel), reinterpret_cast<void*>(&leave_inside_detour),
                      reinterpret_cast<void**>(&o_leave_inside)) != MH_OK)
        BWLOG("stereo: could not hook the temple's entry and exit (a save loaded there stays paused)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::GGame_LocalTimerSaysDoATurn),
                      reinterpret_cast<void*>(&turn_timer_detour), reinterpret_cast<void**>(&o_turn_timer)) != MH_OK) {
        BWLOG("FATAL: failed to hook GGame::LocalTimerSaysDoATurn");
        return false;
    }
    MH_CreateHook(reinterpret_cast<void*>(kCheckRegionOnScreen), reinterpret_cast<void*>(&region_detour), reinterpret_cast<void**>(&o_region));
    MH_CreateHook(reinterpret_cast<void*>(kCheckRegionOnScreenSse), reinterpret_cast<void*>(&region_sse_detour),
                  reinterpret_cast<void**>(&o_region_sse));
    if (MH_CreateHook(reinterpret_cast<void*>(sym::CHand_PrepareForDrawing), reinterpret_cast<void*>(&hand_prepare_detour),
                      reinterpret_cast<void**>(&o_hand_prepare)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(sym::CHand_UpdateHeldObject), reinterpret_cast<void*>(&hand_held_detour),
                      reinterpret_cast<void**>(&o_hand_held)) != MH_OK) {
        BWLOG("FATAL: failed to hook CHand per-frame updates");
        return false;
    }
    hand::install_hooks();
    ui::install_hooks();
    if (MH_CreateHook(reinterpret_cast<void*>(kObjectDraw), reinterpret_cast<void*>(&obj_draw_detour),
                      reinterpret_cast<void**>(&o_obj_draw)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(kMeshDraw), reinterpret_cast<void*>(&mesh_draw_detour),
                      reinterpret_cast<void**>(&o_mesh_draw)) != MH_OK)
        BWLOG("stereo: could not hook the object/mesh draw (hand pose control off)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x80DB30), reinterpret_cast<void*>(&generic_draw_detour),
                      reinterpret_cast<void**>(&o_generic_draw)) != MH_OK)
        BWLOG("stereo: could not hook the general object draw (the leash's loop stays at the game's hand)");
    if (MH_CreateHook(reinterpret_cast<void*>(kShadowUpdate), reinterpret_cast<void*>(&shadow_update_detour),
                      reinterpret_cast<void**>(&o_shadow_update)) != MH_OK)
        BWLOG("stereo: could not hook the dynamic shadow update (the hand's shadow stays at the game's hand)");
    if (MH_CreateHook(reinterpret_cast<void*>(kBillboardDraw), reinterpret_cast<void*>(&billboard_detour),
                      reinterpret_cast<void**>(&o_billboard)) != MH_OK)
        BWLOG("stereo: could not hook the cloud billboard draw");
    if (!install_disciple_hook()) BWLOG("stereo: could not hook the sprites' drawing (the disciple icon not enlarged)");
    if (!install_saving_hook()) BWLOG("stereo: could not hook the game's saving box (no cover in the headset while it saves)");
    if (MH_CreateHook(reinterpret_cast<void*>(kHelperHoverTo3D), reinterpret_cast<void*>(&hover_to_3d_detour),
                      reinterpret_cast<void**>(&o_hover_to_3d)) != MH_OK)
        BWLOG("stereo: could not hook the helper spirits' placement (they stay at the edge of the view)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::CameraModeNew3_DrawHandTricon), reinterpret_cast<void*>(&hand_tricon_detour),
                      reinterpret_cast<void**>(&o_hand_tricon)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(sym::CameraModeNew3_TriconDraw), reinterpret_cast<void*>(&tricon_draw_detour),
                      reinterpret_cast<void**>(&o_tricon_draw)) != MH_OK)
        BWLOG("stereo: could not hook the camera tricons (they stay)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::CameraHelp_DrawKeyOrMouse), reinterpret_cast<void*>(&draw_key_or_mouse_detour),
                      reinterpret_cast<void**>(&o_draw_key_or_mouse)) != MH_OK)
        BWLOG("stereo: could not hook CameraHelp::DrawKeyOrMouse (hints stay)");
    if (MH_CreateHook(reinterpret_cast<void*>(kTreePush), reinterpret_cast<void*>(&tree_push_detour),
                      reinterpret_cast<void**>(&o_tree_push)) != MH_OK)
        BWLOG("stereo: could not hook fn_005DF1B0 (the points trees bend away from)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x801C90), reinterpret_cast<void*>(&light_detour), reinterpret_cast<void**>(&o_light)) != MH_OK)
        BWLOG("stereo: the lighting hook failed (no glow on buildings and the creature)");
    if (MH_CreateHook(reinterpret_cast<void*>(kAddForDrawing), reinterpret_cast<void*>(&add_for_drawing_detour),
                      reinterpret_cast<void**>(&o_add_for_drawing)) != MH_OK)
        BWLOG("stereo: could not hook Game3DObject::AddForDrawing (the trees compared between the eyes)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::GLandscape_Draw), reinterpret_cast<void*>(&landscape_draw_detour),
                      reinterpret_cast<void**>(&o_landscape_draw)) != MH_OK)
        BWLOG("stereo: could not hook GLandscape::Draw (debug marker only)");
    if (MH_CreateHook(reinterpret_cast<void*>(sym::LH3DRender_StartFrame), reinterpret_cast<void*>(&start_frame_detour),
                      reinterpret_cast<void**>(&o_start_frame)) != MH_OK) {
        BWLOG("FATAL: failed to hook LH3DRender::StartFrame");
        return false;
    }
    return true;
}

void apply_command(const std::string& line) {
    std::istringstream in(line);
    std::string k;
    in >> k;
    if (k == "enabled") { int v; in >> v; g_set.enabled = v != 0; }
    else if (k == "world_scale") in >> g_set.world_scale;
    else if (k == "ipd_scale") in >> g_set.ipd_scale;
    else if (k == "base") { std::string v; in >> v; g_set.level_base = v == "level"; }
    else if (k == "zero_time") { int v; in >> v; g_set.zero_time = v != 0; }
    else if (k == "eye_height") in >> g_set.eye_height;
    else if (k == "mirror") { in >> g_set.mirror; }
    else if (k == "second_hand") in >> g_set.second_hand;
    else if (k == "head_audio") in >> g_set.head_audio;
    else if (k == "held_follow") in >> g_held_follow;
    else if (k == "held_log") in >> g_held_log;
    else if (k == "texture_filter") in >> g_set.texture_filter >> g_set.anisotropy;
    else if (k == "object_distance") in >> g_set.object_distance;
    else if (k == "detail_distance") in >> g_set.detail_distance;
    else if (k == "one_pass") in >> g_set.one_pass;
    else if (k == "adaptive_detail") in >> g_set.adaptive_detail;
    else if (k == "size_log") in >> g_size_log;
    else if (k == "small_detail") in >> g_small_w >> g_small_boost >> g_small_floor_w;  // (small_detail <smallest under> <their boost> <small under>; 0 0 0: off)
    else if (k == "one_pass_k") in >> g_one_pass_k;
    else if (k == "replay_group") { int on = 1; in >> on; trace::set_replay_group(on != 0); }
    else if (k == "replay_prefill") { int on = 1; in >> on; trace::set_replay_prefill(on != 0); }
    else if (k == "shadow_on_ground") in >> g_set.shadow_on_ground;
    else if (k == "same_trees") in >> g_set.same_trees;
    else if (k == "helper_spread") in >> g_set.helper_spread;
    else if (k == "share_eyes") in >> g_set.share_eyes;
    else if (k == "flip_every") { int n = 1; in >> n; mod::set_flip_every(n); }
    else if (k == "helper_log") in >> g_helper_log;
    else if (k == "tree_push_log") in >> g_tree_push_log;
    else if (k == "drop_log") in >> g_drop_log;
    else if (k == "burst_both") in >> g_burst_both;
    else if (k == "drop_test_offset") in >> g_drop_test_offset;
    else if (k == "temple_mouse_log") in >> g_temple_mouse_log;
    else if (k == "tree_alpha_log") in >> g_tree_alpha_log;
    else if (k == "same_drops") in >> g_same_drops;
    else if (k == "glow_add") in >> std::hex >> g_glow_add >> std::dec, g_glow_add_log = 3;
    else if (k == "size_by_place") in >> g_size_by_place;
    else if (k == "dxw_fast") {  // debug: dxwrapper's per-draw savings on or off (DxwSetFastDraw, our patch)
        int on = 1;
        in >> on;
        HMODULE m = GetModuleHandleW(L"dxwrapper.dll");
        using SetFastFn = void(WINAPI*)(BOOL);
        auto fn = m ? reinterpret_cast<SetFastFn>(GetProcAddress(m, "DxwSetFastDraw")) : nullptr;
        if (fn) fn(on);
        BWLOG("stereo: dxwrapper's per-draw savings %s%s", on ? "on" : "off", fn ? "" : " (not in this dxwrapper)");
    }
    else if (k == "point_finger") { in >> g_point_finger; g_point_built = false; }
    else if (k == "miracle_on_palm") in >> g_set.miracle_on_palm;
    else if (k == "miracle_grip") in >> g_set.miracle_grip;
    else if (k == "cloud_log") in >> g_cloud_log;
    else if (k == "miracle_effect") in >> g_set.miracle_effect_m;
    else if (k == "spike_ms") { int n = 15; in >> n; g_spike_ms = n; }
    else if (k == "load_trace") { int n = 200; in >> n; load_trace::trace(n); }
    else if (k == "profile") {  // debug: profile <ms>
        int ms = 3000;
        in >> ms;
        std::thread([ms] { run_profiler(ms); }).detach();
    }
    else if (k == "lod_bias") in >> g_set.lod_bias;
    else if (k == "mip_levels") { int v = 4; in >> v; mipmaps::set_levels(v); }
    else if (k == "unsquash_hand") in >> g_set.unsquash_hand;
    else if (k == "pose_log") in >> g_pose_log;
    else if (k == "fist") { for (float& d : g_fist_deg) in >> d; g_fist_built = false, g_point_built = false; }
    else if (k == "cutscene") in >> g_force_cutscene;
    else if (k == "cutscene_log") in >> g_cutscene_log;
    else if (k == "cutscene_screen") in >> g_set.cutscene_screen;
    else if (k == "temple_4x3") in >> g_set.temple_4x3;
    else if (k == "temple_unpause") in >> g_set.temple_unpause;
    else if (k == "save_pictures") in >> g_set.save_pictures;
    else if (k == "temple_room") {  // temple_room <n>: go to that room of the temple (harness; inside the temple only)
        int room = 0;
        in >> room;
        void* game = game::game_instance();
        void* temple = game ? game::game_field<void*>(game, kGameTemple) : nullptr;
        if (temple && o_go_to_room && other_view(game)) o_go_to_room(temple, room);
        else BWLOG("stereo: not in the temple");
    }
    else if (k == "near_m") in >> g_set.near_m;
    else if (k == "anchor_hand") in >> g_set.anchor_hand >> g_set.anchor_fwd >> g_set.anchor_up;
    else if (k == "hand_shadow") in >> g_hand_shadow;
    else if (k == "shadow_log") in >> g_shadow_log;
    else if (k == "loop_follows_hand") in >> g_loop_follows_hand;
    else if (k == "leash_draws") in >> g_leash_draw_log, g_region_loop_log = g_leash_draw_log;
    else if (k == "wrist_draws") in >> g_wrist_draw_log;
    else if (k == "watch_write") {
        std::string a;
        in >> a;
        uintptr_t addr = 0;
        if (a == "status" && g_hand && !IsBadReadPtr(static_cast<char*>(g_hand) + 0x4850, 4)) {
            int off = 0xA4;
            in >> std::hex >> off;
            addr = *reinterpret_cast<uintptr_t*>(static_cast<char*>(g_hand) + 0x4850) + off;
        } else if (!a.empty()) {
            addr = std::stoul(a, nullptr, 16);
        }
        set_watch(addr);
    }
    else if (k == "crash_test") { BWLOG("stereo: crash_test: crashing on purpose"); *static_cast<volatile int*>(nullptr) = 1; }
    else if (k == "freeze") { int v; in >> v; g_set.frozen = v != 0; }
    else if (k == "autosave_now") {  // debug: the game's autosave, now (GameOSFile::AutoSave(force 1))
        const int r = reinterpret_cast<int(__cdecl*)(int)>(0x564050)(1);
        BWLOG("stereo: autosave now: %d", r);
    } else if (k == "saving_cover") { int v = 1; in >> v; g_set.saving_cover = v != 0; }
    else if (k == "disciple_force") in >> g_disciple_force;
    else if (k == "disciple_lift") in >> g_disciple_lift;
    else if (k == "flight_log") in >> g_flight_log;
    else if (k == "disciple_scale") { int v = 300; in >> v; g_disciple_scale = float(v) / 100.0f; g_disciple_log = 3; }
    else if (k == "autosave_defer") { int v = 1; in >> v; g_autosave_defer_test = g_set.autosave_fade_first = v != 0; }
    else if (k == "vignette_info") BWLOG("stereo: comfort vignette %.2f (option %d)", g_vignette, hand::comfort_vignette());
    else if (k == "sway") in >> g_sway_amp >> g_sway_period;
    else if (k == "update_camera") { int v; in >> v; g_use_update_camera = v != 0; }
    else if (k == "statediff") {
        // statediff <name> [heap]: "heap" also covers committed private memory, not just the exe.
        std::string opt;
        in >> g_statediff_name >> opt;
        g_statediff_heap = opt == "heap";
    }
    else if (k == "peek") {
        // peek <hex addr> [dwords]: log raw memory (debug)
        std::string a;
        int n = 4;
        in >> a >> n;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(strtoul(a.c_str(), nullptr, 16));
        for (int i = 0; i < n; ++i) {
            float fv;
            memcpy(&fv, &p[i], 4);
            BWLOG("peek %08X = %08X (%g)", unsigned(reinterpret_cast<uintptr_t>(p + i)), p[i], fv);
        }
    }
    else if (k == "pokef") {
        // pokef <hex addr> <float>...: write floats (debug; e.g. the sun's position at EA9E90 for low-sun tests)
        std::string a;
        in >> a;
        float* p = reinterpret_cast<float*>(strtoul(a.c_str(), nullptr, 16));
        DWORD old = 0;
        float v;
        for (int i = 0; in >> v; ++i)
            if (VirtualProtect(p + i, 4, PAGE_READWRITE, &old)) p[i] = v, VirtualProtect(p + i, 4, old, &old);
    }
    else if (k == "snap") {
        // snap: remember the exe's writable data now (between frames). snapdiff <name>: write
        // every dword that changed since, to captures\<name>.statediff (pass label "S").
        g_statediff_heap = false;
        snapshot();
        BWLOG("stereo: snapshot taken");
    }
    else if (k == "snapdiff") {
        std::string name;
        in >> name;
        std::wstring p = paths::bwvr_dir() + L"captures\\" + std::wstring(name.begin(), name.end()) + L".statediff";
        if (FILE* f = _wfopen(p.c_str(), L"w")) {
            write_statediff(f, "S");
            fclose(f);
        }
    }
    else if (k == "rewind") {
        std::string a;
        size_t len = 4;
        in >> a >> len;
        if (a == "clear") g_rewind.clear();
        else g_rewind.push_back({uintptr_t(strtoul(a.c_str(), nullptr, 16)), len});
    }
    else if (k == "highlight") { int a = -1, b = -1; in >> a >> b; trace::set_highlight(a, b); }
    else if (k == "dump_draw") { int v = -1; in >> v; trace::set_dump_draw(v); }
    else if (k == "near_clip") { int v = 1; in >> v; trace::set_near_clip(v); }
    else if (k == "ui") { if (!ui::command(k, in)) { BWLOG("stereo: bad command '%s'", line.c_str()); return; } }
    else if (k == "ctrl" || k == "hands" || k == "pointerinfo" || k == "rig" || k == "ground") { if (!hand::command(k, in)) { BWLOG("stereo: bad command '%s'", line.c_str()); return; } }
    else if (k == "gamecam") {
        game::LHPoint p, f;
        in >> p.x >> p.y >> p.z >> f.x >> f.y >> f.z;
        game::set_game_camera(p, f);
    }
    else if (k == "upright_hand") { int v = 1; in >> v; g_set.upright_hand = v != 0; }
    else if (k == "bone_dump") in >> g_bone_dump;
    else if (k == "hand_xf_log") in >> g_hand_xf_log;
    else if (k == "eye_clouds") { int v = 1; in >> v; g_set.eye_facing_clouds = v != 0; }
    else if (k == "mouse_hints") { int v = 0; in >> v; g_set.hide_mouse_hints = v == 0; }
    else if (k == "sync_camera") { int v = 1; in >> v; g_set.sync_game_camera = v != 0; }
    else if (k == "handtrace") in >> g_handtrace_left;
    else if (k == "handinfo") { int n = 2; in >> n; g_handinfo_pending = n; }
    else if (k == "sea_world") { int on = 1; in >> on; trace::set_sea_world(on != 0); }
    else if (k == "sea_only") { int on = 0; in >> on; trace::set_sea_only(on != 0); }
    else if (k == "sea_solid") { int on = 0; in >> on; trace::sea_solid() = on != 0; }
    else if (k == "sea_solid_under_land") { int on = 1; in >> on; trace::sea_solid_under_land() = on != 0; }
    else if (k == "skip_draws") { int a = -1, b = -1; in >> a >> b; trace::set_skip(a, b); }
    else if (k == "log_draws") { int a = -1, b = -1; in >> a >> b; trace::set_log_draws(a, b); }
    else if (k == "pose") {
        float yaw = 0, pitch = 0, roll = 0;
        Vec3 p;
        in >> yaw >> pitch >> roll >> p.x >> p.y >> p.z;
        g_head.rot = vr::basis_from_ypr(yaw, pitch, roll);
        g_pose_ypr[0] = yaw, g_pose_ypr[1] = pitch, g_pose_ypr[2] = roll;
        g_head.pos = p;
    } else if (k == "vr") {
        std::string v;
        in >> v;
        g_set.use_openvr = v == "openvr";
        g_openvr_failed = false;
        if (!g_set.use_openvr) vr::openvr::shutdown();
    } else if (k == "recenter") {
        g_recenter = true;
    } else if (k == "burst") {
        in >> g_burst_name >> g_burst_left;
        g_burst_every = 1, g_burst_skip = 0;
        g_burst_index = 0;
    } else if (k == "mirror_capture") {
        in >> g_mirror_name;
    } else if (k == "capture") {
        // capture <name> [trace]: "trace" also records the D3D7 call trace of the same frame.
        std::string opt;
        in >> g_capture_name >> opt;
        if (opt == "trace") trace::request_capture(1);
    }
    else if (!k.empty()) { BWLOG("stereo: unknown command '%s'", line.c_str()); return; }
    if (!k.empty()) BWLOG("stereo: %s", line.c_str());
}

bool active() { return g_set.enabled; }
bool wants_headset() { return g_set.enabled && g_set.use_openvr; }
double last_vr_frame_s() { return g_last_vr_frame_s; }

std::string stats_line() {
    char buf[256];
    unsigned n = g_stereo_frames ? g_stereo_frames : 1;
    snprintf(buf, sizeof(buf), "stereo frames %u (mono %u), eye L %.3f ms, eye R %.3f ms", g_stereo_frames,
             g_mono_frames, g_eye_ms[0] / n, g_eye_ms[1] / n);
    std::string out = buf;
    out += " | " + hand::stats_line();
    if (vr::openvr::active()) {
        snprintf(buf, sizeof(buf), " | readback+upload %.2f ms/frame | ", g_readback_ms / n);
        out += buf + vr::openvr::stats_line();
    }
    if (g_ft.frames) {
        const double n = double(g_ft.frames);
        snprintf(buf, sizeof(buf), " | frame %.2f ms: wait %.2f, setup %.2f, eyes %.2f/%.2f, readback %.2f/%.2f, after %.2f, outside %.2f",
                 (g_ft.wait + g_ft.setup + g_ft.eye[0] + g_ft.eye[1] + g_ft.read[0] + g_ft.read[1] + g_ft.after + g_ft.outside) / n, g_ft.wait / n,
                 g_ft.setup / n, g_ft.eye[0] / n, g_ft.eye[1] / n, g_ft.read[0] / n, g_ft.read[1] / n, g_ft.after / n, g_ft.outside / n);
        out += buf;
        snprintf(buf, sizeof(buf), "; longest %.1f ms (most: eyes %.1f, readback %.1f, outside %.1f; outside over 2 ms %u), over 8.6 ms %u/%u, over 11.4 ms %u",
                 g_ft.max_frame, g_ft.max_eyes, g_ft.max_read, g_ft.max_outside, g_ft.outside_over_2ms, g_ft.over_120, g_ft.frames, g_ft.over_90);
        out += buf;
        snprintf(buf, sizeof(buf), " (readback: lock %.2f, copy %.2f)", render::g_lock_ms / n, render::g_copy_ms / n);
        out += buf;
        snprintf(buf, sizeof(buf), "; detail %.0f%% (its work %.2f ms a frame)", adaptive_detail_percent(), g_cost_ema);
        out += buf;
        render::g_lock_ms = render::g_copy_ms = 0;
    }
    g_ft = FrameTiming{};
    g_readback_ms = 0;
    g_stereo_frames = g_mono_frames = 0;
    g_eye_ms[0] = g_eye_ms[1] = 0;
    return out;
}

void watch_writes(uintptr_t addr) { set_watch(addr); }

// The game's saves (the autosave, the temple's): SaveAllGame shows its "Saving" box
// (GameSavingBox, 0x557FC0, cdecl(bool done): false as the save starts, true once it is over) and
// then draws nothing until it is done. In the flat
// game the box stays on the screen; in the headset the world froze for the save's length with no
// word why (player, 2026-11-03). While VR is up, SteamVR dims the view and shows "Saving..." (its
// compositor draws them while the game is held up), and the time taken is logged.
using SavingBoxFn = void(__cdecl*)(bool done);
bool g_saving_box_seen = false;  // (the game's saving box came up: a save began)
SavingBoxFn o_saving_box = nullptr;
LARGE_INTEGER g_saving_since{};
// While it saves, the last frame on the virtual screen (the SteamVR overlay the videos play on,
// drawn by the compositor while the game is held up), the world behind it only a little dimmed,
// "Saving..." under it (player, 2026-11-04: black was disorienting, the frozen world too; "let's
// try virtual screen with the background visible"). Without a frame kept: dimmed, as before.
bool g_saving_screen = false;
void saving_screen(bool on) {
    if (on && !g_set.saving_screen) {  // (the 2026-11-05 choice: the world as it is, "Saving..." ahead)
        vr::openvr::saving_cover(true, 0.0f, 0.1f);
        return;
    }
    if (on) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (g_last_frame_ok && screenshot::read_surface_rgba(g_last_frame.surface(), rgba, w, h, 2) && w > 0 && h > 0) {
            g_saving_screen = vr::openvr::show_video(rgba.data(), w, h, 3.0f, 3.0f);
            BWLOG("stereo: saving: the last frame (%dx%d) on the virtual screen", w, h);
        }
        vr::openvr::saving_cover(true, g_saving_screen ? 0.3f : 0.6f, 0.15f);
    } else {
        if (g_saving_screen) vr::openvr::hide_video();
        g_saving_screen = false;
        vr::openvr::saving_cover(false);
    }
}
void __cdecl saving_box_detour(bool done) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_set.enabled && g_set.saving_cover && vr::openvr::active()) saving_screen(!done);
    if (!done) g_saving_since = now, g_saving_box_seen = true;
    else if (g_saving_since.QuadPart)
        BWLOG("stereo: the game saved in %.2f s", double(now.QuadPart - g_saving_since.QuadPart) / double(g_freq.QuadPart));
    o_saving_box(done);
}
// The autosave put off until the view has faded out (player, 2026-11-03: dimmed, the frozen world
// behind "Saving..." still looked like a hang and was disorienting). GameOSFile::AutoSave (0x564050,
// cdecl(force)) is called from GGame::EndTurn each turn and saves when the interval is up. In VR it
// now first fades the view to black and shows "Saving..." while the game goes on drawing (0.3 s),
// then saves in the dark, and the view comes back when done: no frozen frame is seen.
using AutoSaveFn = int(__cdecl*)(int force);
AutoSaveFn o_autosave = nullptr;
bool g_autosave_ours = false;   // (the mod's own call, after the fade)
double g_autosave_due_at = -1;  // the fade began: the save follows
bool g_autosave_defer_test = false;  // (stereo autosave_defer 1: deferred without SteamVR too, for the harness)
double now_seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(g_freq.QuadPart);
}
// Would the game save now? (AutoSave's own checks: no cutscene, not multiplayer, not level 6, the
// interval since the last autosave turn up and the game not paused (flags +0x14 & 4), or forced.)
bool autosave_due(int force) {
    const char* game = *reinterpret_cast<const char* const*>(0xD0195C);
    if (!game || IsBadReadPtr(game + 0x25005C, 4)) return false;
    const char* help = *reinterpret_cast<const char* const*>(game + 0x25005C);
    if (help && !IsBadReadPtr(help + 0x45E8, 8) && *reinterpret_cast<const int*>(help + 0x45E8) && *reinterpret_cast<const int*>(help + 0x45EC))
        return false;
    if (*reinterpret_cast<const int*>(game + 0x205A08) == 6) return false;
    if (force) return true;
    const unsigned turn = *reinterpret_cast<const unsigned*>(game + 0x205A40), last = *reinterpret_cast<const unsigned*>(0xD01A80),
                   interval = *reinterpret_cast<const unsigned*>(0xBEC98C);
    return turn - last > interval && !(*reinterpret_cast<const uint8_t*>(game + 0x14) & 4);
}
int __cdecl autosave_detour(int force) {
    // (Off now that the save shows on the virtual screen: [vr] autosave_fade_first=1 brings it back.)
    const bool defer = g_set.enabled && g_set.saving_cover && g_set.autosave_fade_first && (vr::openvr::active() || g_autosave_defer_test);
    if (g_autosave_ours || !defer) return o_autosave(force);
    if (g_autosave_due_at >= 0) return 0;  // (the fade is on: the save comes from autosave_tick)
    if (!autosave_due(force)) return o_autosave(force);
    g_autosave_due_at = now_seconds();
    vr::openvr::saving_cover(true, 1.0f, 0.3f);
    BWLOG("stereo: autosave due: the view fades out first (the save in 0.35 s)");
    return 0;
}
// Each frame: the autosave once the fade is done. (If the game then doesn't save after all, a scene
// having begun, the view comes back.)
void autosave_deferred_tick() {
    if (g_autosave_due_at < 0 || now_seconds() - g_autosave_due_at < 0.35) return;
    g_autosave_due_at = -1;
    g_saving_box_seen = false;
    g_autosave_ours = true;
    const int r = o_autosave(1);
    g_autosave_ours = false;
    BWLOG("stereo: autosave after the fade: %d%s", r, g_saving_box_seen ? "" : " (it didn't save: the view comes back)");
    if (!g_saving_box_seen && vr::openvr::active()) vr::openvr::saving_cover(false);
}
bool install_saving_hook() {
    return MH_CreateHook(reinterpret_cast<void*>(0x557FC0), reinterpret_cast<void*>(&saving_box_detour),
                         reinterpret_cast<void**>(&o_saving_box)) == MH_OK &&
           MH_CreateHook(reinterpret_cast<void*>(0x564050), reinterpret_cast<void*>(&autosave_detour),
                         reinterpret_cast<void**>(&o_autosave)) == MH_OK;
}

// The disciple icon (the yellow figure over a villager held above a field, a building or a tree:
// what it will become when dropped) was tiny in the headset (playtests 2026-10-16, 2026-11-03).
// fn_008241B0 sizes its sprite (0xEB99D8) for the game's camera, a few pixels on the flat screen,
// then LH3DSprite::AddDrawing (0x840C70, thiscall) queues it. That sprite is enlarged there, about
// its own anchor, just above the villager's head ([ui] disciple_icon_scale_percent, 700: at the game's own
// size, about 5 mm across by the hand, it wasn't seen at all in the harness). The game sets its place
// (+0, +4, +8) and size (+0xC) again each frame.
using AddDrawingFn = void(__fastcall*)(void* sprite, void* edx);
// Villager::ShowDiscipleIcon (0x756170, thiscall(int role), ret 4): when the game last asked for the icon.
using ShowDiscipleIconFn = void(__fastcall*)(void* villager, void* edx, int role);
ShowDiscipleIconFn o_show_disciple_icon = nullptr;
void __fastcall show_disciple_icon_detour(void* villager, void* edx, int role) {
    g_disciple_asked_ms = GetTickCount64();
    o_show_disciple_icon(villager, edx, role);
}
AddDrawingFn o_add_drawing = nullptr;
float g_disciple_scale = 7.0f;
int g_disciple_log = 3;  // (the first few enlargements logged)
int g_disciple_force = 0;
float g_disciple_lift = 0.2f;  // (metres at the player's scale above the held villager's origin; stereo disciple_lift)
void __fastcall add_drawing_detour(void* sprite, void* edx) {
    void* disciple = *reinterpret_cast<void* const*>(0xEB99D8);
    if (sprite && sprite == disciple && g_current_eye >= 0 && g_disciple_scale != 1.0f && !IsBadWritePtr(static_cast<char*>(sprite) + 0xC, 0x14)) {
        float* f = reinterpret_cast<float*>(static_cast<char*>(sprite));
        if (g_disciple_log > 0) {
            --g_disciple_log;
            BWLOG("stereo: the disciple icon (sprite %p, icon %u) at (%.1f, %.1f, %.1f), size %.3f (%.3f x %.3f): drawn %.1fx", sprite,
                  *reinterpret_cast<const uint32_t*>(0xEB99E8), f[0], f[1], f[2], f[3], f[6], f[7], g_disciple_scale);
        }
        // The game lifts it high over the villager (for its camera far off); by the hand it hung a
        // third of the view above. Over the villager in the hand it is put just above its head.
        bool in_hand = false;
        if (const void* held = held_object_3d()) {
            const float* m = reinterpret_cast<const float*>(static_cast<const char*>(held) + 0x14);
            if (!IsBadReadPtr(m, 48) && std::fabs(m[9] - f[0]) < 2.0f && std::fabs(m[11] - f[2]) < 2.0f)
                f[1] = m[10] + g_disciple_lift * hand::rig_scale(), in_hand = true;
        }
        // Enlarged only near the eye: the game leaves the last one shown up for a while after a
        // villager was hovered, and from far off, enlarged, it stood huge on the land (playtest
        // 2026-11-04). Full size within 2 m (at the player's scale), the game's own from 4 m.
        // Not held: only while the hand is over that villager (within 1 m across, at the player's
        // scale). The game kept showing the last one hovered, forever, however far the player flew
        // (playtest 2026-11-04: "it should only be when I'm holding them, or my hand is over them").
        if (!in_hand) {
            bool over = false;
            if (hand_alive()) {
                const float* hp = reinterpret_cast<const float*>(static_cast<const char*>(g_hand) + 0x78);  // (the game's hand, on the land)
                const float dx = hp[0] - f[0], dz = hp[2] - f[2], m = std::max(1e-3f, hand::rig_scale());
                over = dx * dx + dz * dz < m * m;
            }
            if (!over) return;  // (not drawn)
        }
        float k = g_disciple_scale;
        if (!in_hand) {
            const float* cam = reinterpret_cast<const float*>(0xEA1DB8);  // (LH3DTech::g_camera's place)
            const float dx = f[0] - cam[0], dy = f[1] - cam[1], dz = f[2] - cam[2];
            const float m = std::max(1e-3f, hand::rig_scale());
            const float near_t = std::clamp((4.0f - std::sqrt(dx * dx + dy * dy + dz * dz) / m) / 2.0f, 0.0f, 1.0f);
            k = 1.0f + (g_disciple_scale - 1.0f) * near_t;
        }
        f[3] *= k;
    }
    o_add_drawing(sprite, edx);
}
bool install_disciple_hook() {
    g_disciple_scale = float(paths::ini_int(L"ui", L"disciple_icon_scale_percent", 700)) / 100.0f;
    if (MH_CreateHook(reinterpret_cast<void*>(0x756170), reinterpret_cast<void*>(&show_disciple_icon_detour),
                      reinterpret_cast<void**>(&o_show_disciple_icon)) != MH_OK)
        BWLOG("stereo: Villager::ShowDiscipleIcon hook failed");
    return MH_CreateHook(reinterpret_cast<void*>(0x840C70), reinterpret_cast<void*>(&add_drawing_detour),
                         reinterpret_cast<void**>(&o_add_drawing)) == MH_OK;
}

// Was this 3D object drawn in the last frame's eye passes? (-1: nothing at all was: not known.)
int drawn_recently(const void* obj3d) {
    if (g_drawn_last.empty()) return -1;
    return std::binary_search(g_drawn_last.begin(), g_drawn_last.end(), obj3d) ? 1 : 0;
}
// This frame's disciple icon: the villager's 3D object it is over (0: none), and which icon.
bool disciple_icon(const void*& obj3d, int& type) {
    obj3d = reinterpret_cast<const void*>(uintptr_t(g_disciple_obj3d)), type = g_disciple_type;
    return g_disciple_obj3d != 0;
}

// The player's "Recenter view" (VR Options, or the other hand's stick click held): the tracking
// origin taken again from where the head is now, its height and heading, as Ctrl+F12 does. (For a
// seated player: the view's height is the head's at the last recentre, sitting or standing.)
void request_recenter() {
    g_recenter = true;
    BWLOG("stereo: recenter requested by the player");
}

// The Developer tab's "Autosave now": the game's autosave once the menu has closed and play has
// gone on 2 s (to see the headset's saving cover as an autosave in play would show it).
double g_autosave_menu_closed = -1;
bool g_autosave_wanted = false;
void request_autosave() {
    g_autosave_wanted = true, g_autosave_menu_closed = -1;
    BWLOG("stereo: autosave requested from the menu (2 s after it closes)");
}
void autosave_tick() {
    autosave_deferred_tick();
    if (!g_autosave_wanted) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = double(now.QuadPart) / double(g_freq.QuadPart);
    if (*reinterpret_cast<void* const*>(0xC4CD20) != nullptr) {  // (SetupBox::CurrentActiveBox: a menu is up)
        g_autosave_menu_closed = -1;
        return;
    }
    if (g_autosave_menu_closed < 0) g_autosave_menu_closed = t;
    if (t - g_autosave_menu_closed < 2.0) return;
    g_autosave_wanted = false;
    const int r = autosave_detour(1);  // (as the game's own: faded out first in VR)
    BWLOG("stereo: autosave from the menu: %d", r);
}

}  // namespace bw::stereo
