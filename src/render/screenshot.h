#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bw::screenshot {

// Saves the device's current render target as a 24-bit BMP. `device` is an IDirect3DDevice7*.
bool save_render_target(void* device, const std::wstring& path);

// Saves any 16/32-bit DirectDraw surface (an IDirectDrawSurface7*) as a 24-bit BMP.
bool save_surface(void* surface, const std::wstring& path);

// A 16/32-bit DirectDraw surface's pixels as RGBA rows (opaque), every `step`th pixel each way.
bool read_surface_rgba(void* surface, std::vector<uint8_t>& rgba, int& w, int& h, int step);

// Debug: locks the z-buffer attached to the current render target and logs value statistics.
void log_depth_stats(void* device);

}  // namespace bw::screenshot
