#pragma once

// Keeps the game's sound even between left and right.
//
// Windows remembers each program's volume per output device, left and right separately. For
// Black & White on the Index it had saved left 100%, right 0% (player, 2026-10-26/27: the sound
// mostly in the left ear, in the game only, on the Index only; another headset was fine). The
// game keeps that going: its audio library reads the volume as it starts and writes the same
// value back as it closes.
namespace bw::audio {

// From DllMain: hooks waveOutSetVolume (left and right are made equal) and starts a thread that
// evens the game's audio session on the default output device, then keeps it even.
// [audio] even_balance=0 leaves both alone.
void install();

}  // namespace bw::audio
