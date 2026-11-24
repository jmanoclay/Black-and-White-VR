#pragma once
#include <string>

// Scripted mouse input for the test harness, without touching the user's real cursor.
//
// The game reads the cursor position by polling GetCursorPosClient() from a mouse thread and
// gets buttons from window messages. We override the former and post the latter.
//
// Script commands (one per line; at most one per frame):
//   pos <x> <y>     override the cursor position (client pixels) until "release"
//   down [l|r]      post a button-down message at the override position
//   up [l|r]        post a button-up message
//   click <x> <y>   pos + wait 100 + down + wait 100 + up
//   key <k>         post key down+up; k = esc|enter|space|back or a virtual-key number
//   type <text>     post the rest of the line as typed characters (WM_CHAR)
//   wait <ms>       do nothing for N milliseconds
//   release         stop overriding; the real cursor takes over again
namespace bw::input {

bool install_hooks();               // call once at startup (MinHook must be initialized)
void queue_script(const std::string& text);
void tick();                        // call once per frame on the game thread
bool busy();                        // script still running
// Types into the game's window: `backspaces` Backspaces, then `text`, as typed characters (WM_CHAR),
// one per frame from tick().
void type_text(const std::wstring& text, int backspaces);
bool typing();       // characters still waiting
void stop_typing();  // drops them

// Live cursor/buttons from the VR controllers (game thread). The cursor override is shared with
// the script's "pos"; a script "release" doesn't stop the VR cursor.
void set_vr_cursor(bool on, long x, long y);
void vr_button(bool right, bool down);
// A click on a menu with the laser: is the game's window in front (if not, it is brought to the
// front: clicks on the menus sometimes did nothing, playtest 2026-11-04), and the cursor's state, logged.
void menu_click_check(long laser_x, long laser_y);

}  // namespace bw::input
