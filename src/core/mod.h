#pragma once

namespace bw::mod {

// Called once, on the game's first ddraw call. Verifies the exe and installs hooks.
void init();
void set_flip_every(int n);  // experiment: the game's window shown every Nth frame

}  // namespace bw::mod
