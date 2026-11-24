#pragma once
#include <string>

namespace bw::game {

// Writes the engine camera globals plus LH3DTech::ProjectPoint results for probe points around
// the camera focus. Used to pin down the engine's projection convention (tools/analysis/camfit.py).
void dump_camera(const std::wstring& path);

}  // namespace bw::game
