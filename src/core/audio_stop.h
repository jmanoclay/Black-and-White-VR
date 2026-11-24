#pragma once

// Stopping a sound without stopping the game.
//
// The game's audio library (LHaudiodllR.dll) stops a playing sound by fading it out over 20 ms
// through QMixer, sleeping 20 ms on the calling thread (the game's) for the fade, then resetting the
// fade speed and flushing the channel. Each sleep lasts 20-30 ms or more, and a few sounds stopping
// together froze the game for 40-170 ms, several times a minute: SteamVR then reprojected at half
// rate for a while (player, 2026-10-28: "low frame rate").
//
// Here the sleep is skipped and the rest of the stop is done 25 ms later on a helper thread. Any
// call the library makes on that channel before then finishes the stop first, so QMixer sees the
// same order of calls as before. [audio] async_stop=0 leaves the library as it is.
namespace bw::audio_stop {

// From DllMain: starts a thread that patches the audio library's imports once it is loaded.
void install();

}  // namespace bw::audio_stop
