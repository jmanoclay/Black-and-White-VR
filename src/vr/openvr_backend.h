#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "vr/controllers.h"
#include "vr/hmd_profile.h"
#include "vr/math.h"

// SteamVR output through OpenVR (in-process, 32-bit openvr_api.dll, delay-loaded).
// Eye images arrive as CPU pixels (read back from the D3D7 eye targets), are uploaded into
// D3D11 textures on our own device, and submitted to the compositor.
namespace bw::vr::openvr {

bool init();              // VR_Init (starts SteamVR if needed); false if unavailable
void shutdown();
bool active();

// Blocks until the compositor wants the next frame; returns the predicted head pose in engine
// axes (tracking space, meters). False if the pose isn't valid (headset not tracked).
bool wait_frame(Pose& head);

// Live optics: projection tangents and eye-to-head offsets (engine axes), recommended size.
HmdProfile profile();

// Uploads one eye (BGRX rows, top-down, `pitch` bytes per row) and submits both once both are in.
void submit_eye(int eye, const uint8_t* bgrx, int w, int h, int pitch);
// The eye's upload texture mapped for writing (w x h BGRX rows at dst, pitch bytes apart); then
// submit_mapped_eye. Both eyes submitted: the frame goes to SteamVR.
bool map_eye(int eye, int w, int h, uint8_t*& dst, int& pitch);
void submit_mapped_eye(int eye);
// The eye straight from the GPU: the rect (x, y, w, h) of a Direct3D 9 surface (the game drawing
// through dxwrapper) copied into a texture shared with Direct3D 11. False: not possible (use the
// readback).
bool share_eye(int eye, void* device9, void* surface9, int x, int y, int w, int h);

// Reads SteamVR's mirror of what it received for one eye into top-down BGRA (validation).
bool read_mirror(int eye, std::vector<uint8_t>& bgra, int& w, int& h);

// Controllers through SteamVR Input (action manifest in bwvr\inputctions.json, default
// bindings for the Index controllers). Call after wait_frame; poses are predicted for the same
// display time as the head pose. Raw tracking space, engine axes.
void read_controllers(Controller out[2]);
void haptic(int hand, float seconds, float frequency, float amplitude);

// SteamVR's own keyboard overlay (minimal mode: the app shows the text). poll_keyboard, once per
// frame while it is up, appends what was typed since (UTF-8; Backspace as the character 8) to `typed`, and
// returns 1 when the player pressed Done, -1 when it closed without Done (or isn't up), 0 while
// it is still open. (It reads the system's events.)
bool show_keyboard(const char* description, const char* existing, uint32_t max_chars);
int poll_keyboard(std::string& typed);
void hide_keyboard();

// A flat picture on a screen in the headset, as a SteamVR overlay (the game's videos, which play
// with no 3D frame: no eye passes). RGBA rows, top-down. The screen is placed ahead of the head,
// level, `distance_m` away and `width_m` wide, when it first shows; hide_video takes it away.
bool show_video(const uint8_t* rgba, int w, int h, float width_m, float distance_m);
void hide_video();

// While the game saves (its "Saving" box, then no frames until it is done): SteamVR dims the
// frozen view and shows a steady "Saving..." panel ahead, both drawn by the compositor, which goes
// on while the game is held up. Off: the view comes back.
// alpha, fade_s: how dark and how fast (an autosave fades to black first, while the game still
// draws: see stereo.cpp; the temple's saves dim as they start). Called on again while up: kept.
void saving_cover(bool on, float alpha = 0.6f, float fade_s = 0.25f);
void saving_tick();  // each frame: the panel goes once it has been up a second (a quick save's words can still be read)

std::string stats_line();  // compositor frame timing since the last call

}  // namespace bw::vr::openvr
