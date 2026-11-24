// The game's 2D interface in VR: text, icons, menus.
#pragma once
#include <sstream>
#include <string>

#include "control/vr_hand.h"

namespace bw::ui {

// Seconds since the game last asked for its "Pick Up" word (the thing under its cursor can be picked up).
double seconds_since_pick_up_word();
bool continue_prompt_up();  // the "trigger - continue" prompt under a caption left up

void configure();
bool install_hooks();  // MinHook initialised; enabled by the caller
// The stereo renderer enters eye pass `eye` (0 left, 1 right) with this view, or leaves the eye
// passes (eye -1, view ignored).
void set_eye_pass(int eye, const hand::EyeView* view);

// A control hint the game wants at the cursor (CameraHelp::DrawKeyOrMouse: a mouse picture and a
// word such as "Pick Up"). `group` is its second argument: 2 for what the hand can do, 1 for
// mouse camera moves. `draw_original` draws the game's own flat version.
// Returns true when the hint was dealt with here (drawn by the hand in the world, or dropped).
bool hint(const wchar_t* text, int group, void (*draw_original)(void*), void* ctx);

// Once per stereo frame, before the eye passes: where tracking space is in the world, the head
// in tracking space, and the size of the game's 2D layout (the window).
void begin_frame(const hand::WorldFrame& world, const vr::Pose& head, int screen_w, int screen_h);

// The game's 2D (menus, panels, text) is shown on a virtual screen floating in front of the
// player. If the ray (world) lands on it where something is drawn, returns the point hit and the
// 2D position there: the game's mouse position for it.
bool pointer(vr::Vec3 origin, vr::Vec3 dir, vr::Vec3& hit, long& x, long& y);

// A cutscene is playing and is shown as the game's own flat frame on the virtual screen (a
// cinema screen): the screen is larger, and the laser is the mouse anywhere on it.
void set_cinema(bool on);
// A gesture's shape placed on the panel above the left hand: the factor its size is drawn at there
// (the game sizes it for 10 units from the eye); 1 for any other sprite.
float gesture_sprite_scale(const void* sprite);
void gesture_sprite_drawing(void* sprite);  // (a gesture sprite about to be drawn: placed again now)
// A gesture sprite drawn now: if it is one, it is put where the engine draws it in view, and its
// corners laid on the panel (true; gesture_sprite_end after the draw puts its place back).
bool gesture_sprite_begin(void* sprite, float saved[3]);
void gesture_sprite_end(void* sprite, const float saved[3]);
// An engine sprite (its world place) drawn now: turned about its middle to stand upright in the world
// (true; sprite_upright_end after the draw).
bool sprite_upright_begin(const float* world);
void sprite_upright_end();
// Each frame after the hands are updated: the gesture panel above the left hand (palm up).
void place_gesture_panel(const hand::WorldFrame& world, const vr::Pose& head);
// The virtual screen's corners in the world: top-left, top-right, bottom-right, bottom-left.
bool screen_corners(vr::Vec3 out[4]);

// In each eye pass, after the game's own 2D: the mod's own panels (the tuning panel).
void draw_overlays();

bool command(const std::string& k, std::istringstream& in);  // "ui ..."

// Naming a save in VR (save_name.cpp): the save room's name field gets a default name and SteamVR's
// keyboard. Once per frame; `vr` = the stereo renderer is on.
void configure_save_name();
void save_name_tick(bool vr);
// VR debug options in the game's pause menu (debug_menu.cpp).
bool debug_menu_install();
void debug_menu_tick();
bool debug_menu_escape();  // Esc with the VR options page up: closes the menu (false: not up)
bool debug_menu_command(const std::string& what, std::istringstream& in);
bool save_name_command(const std::string& what, std::istringstream& in);

}  // namespace bw::ui
