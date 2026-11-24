#pragma once
#include <string>

// Engine-level stereo: GGame::Process3dEngine is run once per eye with that eye's camera,
// viewport and FOV, into an offscreen render target per eye. See
// documentation/render-pass-map.md for why this has to happen inside the engine.
//
// Runtime control (bwvr\control\stereo.req, one "key value" per line):
//   enabled 0|1        world_scale <units per meter>   ipd_scale <x>   base game|level
//   zero_time 0|1      eye_height <px>                  mirror 0|1
//   freeze 0|1         stop game turns and all time increments (deterministic test captures)
//   pose <yaw> <pitch> <roll> <x> <y> <z>   mock head pose (degrees, meters; engine axes)
//   sway <amp_deg> <period_s>               mock head yaw oscillation (0 = off)
//   vr openvr|mock     head pose + output: SteamVR or the mock pose   recenter   mirror_capture <name>
//   capture <name>     write <name>_L/_R .bmp/.depth + <name>.json for the next stereo frame
//   temple_4x3 0|1     the temple shows the height it shows at 4:3 (its FOV widened at 16:9)
//   temple_unpause 0|1 a save loaded in the temple's save room doesn't stay paused
//   save_pictures 0|1  the save room's pictures from the game's back buffer (the screen reads as magenta)
//   lod_bias <x>       mip level offset in the eye passes   mip_levels <n>  for textures created from now on
namespace bw::stereo {

void configure();       // [stereo] section of bwvr.ini + the HMD profile
bool install_hooks();   // detours GGame::Process3dEngine (MinHook must be initialized)
bool active();          // the eye passes are on (VR)
bool wants_headset();   // on, and through SteamVR (not the scripted head)
double last_vr_frame_s();  // when the last VR (eye pass) frame was made (QueryPerformanceCounter seconds)
void apply_command(const std::string& line);
// The spike watcher stops (and finishes a sample under way): the game is crashing or exiting, and a
// thread ended between its suspend and resume would leave the game's thread suspended for good.
void stop_spike_watch();
std::string stats_line();

}  // namespace bw::stereo
