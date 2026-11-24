#pragma once
#include <windows.h>

#include <string>

// Mipmaps for the game's textures. The game creates every texture as a single level, so far
// textures alias and crawl in the headset. The game's CreateSurface calls for textures get a short
// mip chain ([textures] mip_levels), and the lower levels are made again (a box filter) from the
// top level written by the game (Blt, BltFast, Lock/Unlock), when the texture is next bound.
namespace bw::mipmaps {

void configure();                                    // reads [textures]; after MH_Initialize
void on_ddraw_loaded(FARPROC create, FARPROC create_ex);  // hooks DirectDrawCreate(Ex) of the real ddraw

// Called by the surface detours (render/d3d7_trace.cpp) after the call went through.
void after_blt(void* dst, const RECT* dst_rect, void* src, const RECT* src_rect);
void after_blt_fast(void* dst, DWORD x, DWORD y, void* src, const RECT* src_rect);
void after_lock(void* surface, const RECT* rect, DWORD flags, const void* desc);  // desc: DDSURFACEDESC2
void before_unlock(void* surface);
void after_unlock(void* surface);
void after_load(void* dst, const POINT* at, void* src, const RECT* src_rect);
void after_restore(void* surface);
// Called by the SetTexture detour: a texture written since it was last bound gets its levels now.
void before_bind(void* texture);

void set_levels(int levels);                         // runtime: textures created from now on
std::string stats_line();                            // for the 5-second log line

}  // namespace bw::mipmaps
