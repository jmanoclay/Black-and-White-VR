#pragma once
#include <cstddef>
#include <string>

namespace bw::trace {

void configure();                      // reads [trace] from bwvr.ini
void ensure_hooked(void* device);      // patches the IDirect3DDevice7 vtable once
void hook_surface_methods(void* surface);  // Lock/Unlock/Blt/BltFast/Restore of every IDirectDrawSurface7 (once)
void request_capture(int frames);      // trace the next N frames
void set_near_clip(int mode);           // 1 = clip at the near plane, 2 = drop (A/B), 0 = off
void mark(const char* label);           // writes a marker line into the current trace capture
void set_highlight(int from, int to);    // debug: draw [from, to) of each scene flat red
void set_dump_draw(int index);          // debug: dump vertices/indices of draw N in traced frames
void set_skip(int from, int to);
void set_log_draws(int from, int to);  // debug: log the game code asking for these draws        // debug: skip draws [from, to) of each scene (-1 -1 = none)
void set_sea_world(bool on);          // [fix] sea_world: in eye passes, draw the sea as world geometry
void set_hud_unclamped(bool on);     // the next HUD models' screen shift without the engine's clamp to the screen (the gesture icons)
void set_eye_pass(bool on);           // the stereo renderer is inside an eye's 3D pass
// One engine pass for both eyes: the eye pass's calls recorded instead of drawn, then replayed
// into each eye with every vertex moved to it (see the .cpp).
void begin_record();
void end_record();
bool record_ok();
void replay(void* device, float f, float cx, float cy, float dx, float dy, float dz, float near_z);
std::string record_stats();
// The next draws' real depth (world units ahead of the eye) when their vertices' is made up; 0: none.
void set_record_depth(float z);
// The replay's draws grouped by texture where they don't overlap (on by default).
void set_replay_group(bool on);
// The replay's vertices handed to dxwrapper once an eye (DxwRingFill; on by default).
void set_replay_prefill(bool on);
void pass_draws(unsigned& draws, unsigned& prims);  // the last eye pass's draw calls (since set_eye_pass(true))
// A filter for the pre-transformed draws of an eye pass (the 2D interface is re-projected with
// it). It may hand back replacement vertices (same count and layout, valid until its next call)
// or have the draw skipped.
enum class Filter { kKeep, kReplace, kSkip };
using VertexFilter = Filter (*)(unsigned long fvf, const void* verts, unsigned long count, size_t stride, const void** out);
void set_vertex_filter(VertexFilter filter);
void set_on_top(bool on);             // the mod's own text: drawn over whatever is nearer (depth test off)
bool& sea_solid();
bool& sea_solid_under_land();  // the sea under the land solid (the rivers show it), the open sea as it is             // the sea drawn opaque: the reflections below it don't show through (comfort)
void set_no_cull(bool on);            // draws are mirrored (the second hand): draw both faces
void set_ghost(float opacity);        // draws are faded to this opacity, cooled (0 or 1: as they are)
void set_sea_only(bool on);           // debug: draw only the sea (to measure it in isolation)
void end_frame(void* device, unsigned frame);
std::string stats_line();              // per-frame draw statistics for the periodic log line

}  // namespace bw::trace
