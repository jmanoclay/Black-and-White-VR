// The game's videos (Bink: the Lionhead opening, the new game's intro, ...) in the headset: they
// play with no 3D frame, so none of the eye passes run and SteamVR showed nothing. Each frame the
// game copies out of Bink is copied once more for a screen ahead of the player (a SteamVR overlay).
#pragma once

namespace bw::video_screen {

void configure();       // [vr] video_screen, video_screen_width_m, video_screen_distance_m
bool install_hooks();   // binkw32's BinkCopyToBuffer and BinkClose (MinHook initialised)
// Each frame the game presents (LHScreen::Flip), with its back buffer (IDirectDrawSurface7*): when
// no VR frame was made for it (the opening splash, loading screens), it goes on the same screen.
void presented(void* back_buffer);

}  // namespace bw::video_screen
