#pragma once
// The game drawing through dxwrapper (Direct3D 7 on Direct3D 9, [proxy] chain): reaching the
// Direct3D 9 objects behind the game's DirectDraw surfaces, to move pictures on the GPU instead of
// through the CPU. Without dxwrapper everything here answers false.

namespace bw::render::d3d9 {

// dxwrapper is loaded and offers its surfaces (its DxwGetD3D9Surface export).
bool available();
// The Direct3D 9 device and surface behind a DirectDraw surface (IDirectDrawSurface7*). Not
// AddRef'd: valid while the surface lives.
bool surface(void* dd_surface, void** device9, void** surface9);
// Copies (scaling, filtered) the rect of one DirectDraw surface onto the rect of another, on the GPU.
bool stretch(void* src_dd, int sx, int sy, int sw, int sh, void* dst_dd, int dx, int dy, int dw, int dh);
// The game's window through dxwrapper: the frame (the game's back buffer) is kept before the
// game's flip, and shown in the window after it.
bool hold_for_window(void* dd_surface);
bool show_held();

}  // namespace bw::render::d3d9
