#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bw::render {

extern double g_lock_ms, g_copy_ms;  // time read_into spent locking and copying (summed, for the stats)

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// An offscreen D3D7 render target (+ matching z-buffer) for one eye. All calls must be made on
// the game thread with the game's IDirect3DDevice7 (passed as void* to keep d3d.h out of headers).
class EyeTarget {
public:
    ~EyeTarget();
    // (Re)creates the surfaces if the size changed. Returns false if creation failed.
    bool ensure(void* device, int w, int h);
    void release();

    // Makes this the device's render target (remembers the previous one), and clears it.
    bool bind(void* device, uint32_t clear_color);
    // Restores the render target that was active before bind().
    void unbind(void* device);

    // Copies `crop` of the colour buffer into BGR (bottom-up rows, BMP layout). Returns false on failure.
    bool read_color(const Rect& crop, std::vector<uint8_t>& bgr_bottom_up, int& stride);
    // Copies `crop` of a 32-bit colour buffer as top-down BGRX rows (stride = w*4), the layout of
    // DXGI_FORMAT_B8G8R8A8_UNORM. Returns false for other pixel formats.
    bool read_bgrx(const Rect& crop, std::vector<uint8_t>& bgrx);
    bool read_into(const Rect& crop, uint8_t* dst, int dst_pitch);  // the crop's BGRX rows straight into dst
    // Copies `crop` of the z-buffer as floats in [0,1]. Returns false if the z-buffer can't be locked.
    bool read_depth(const Rect& crop, std::vector<float>& depth);
    // Stretches `crop` of the colour buffer onto `dst` of the device's original render target.
    bool blit_to(void* device, const Rect& crop, const Rect& dst);

    void* surface() const { return rt_; }  // the colour buffer (IDirectDrawSurface7*)
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void* rt_ = nullptr;     // IDirectDrawSurface7*
    void* z_ = nullptr;      // IDirectDrawSurface7*
    void* prev_rt_ = nullptr;
    int w_ = 0, h_ = 0;
    uint32_t z_bits_ = 0, z_mask_ = 0;

public:
    // Bits of depth precision (0 without a z-buffer).
    int depth_precision() const {
        int n = 0;
        for (uint32_t m = z_mask_; m; m >>= 1) n += int(m & 1);
        return n;
    }
};

// A copy of the device's current render target (the game's own frame) as a texture, and a quad
// to draw it with: the cinema screen for cutscenes.
class FrameTexture {
public:
    ~FrameTexture();
    // Copies the device's current render target into the texture, (re)creating it to match.
    bool update(void* device);
    void release();
    void* surface() const { return tex_; }

private:
    void* tex_ = nullptr;  // IDirectDrawSurface7*
    int w_ = 0, h_ = 0;
};
// Lines of text drawn into a texture, on a coloured background: the mod's panels in the world (a
// signpost's "Did you know...?"). The first title_lines are larger.
class TextTexture {
public:
    ~TextTexture();
    bool set(void* device, const std::vector<std::wstring>& lines, int title_lines, uint32_t background_rgb, uint32_t title_rgb,
             uint32_t text_rgb, uint8_t background_alpha);
    void release();
    void* surface() const { return tex_; }
    int width() const { return used_w_; }   // (pixels used, from the top-left)
    int height() const { return used_h_; }
    float u1() const { return tex_w_ ? float(used_w_) / tex_w_ : 1.0f; }
    float v1() const { return tex_h_ ? float(used_h_) / tex_h_ : 1.0f; }

private:
    void* tex_ = nullptr;  // IDirectDrawSurface7*
    int tex_w_ = 0, tex_h_ = 0, used_w_ = 0, used_h_ = 0;
};
// Draws `texture` on the quad `v` (top-left, top-right, bottom-right, bottom-left; each x, y in
// target pixels and 1/depth), unlit, without depth test. u1, v1: how much of the texture (from its
// top-left); blend: by the texture's alpha.
bool draw_textured_quad(void* device, void* texture, const float v[4][3], float u1 = 1.0f, float v1 = 1.0f, bool blend = false);
// The comfort vignette: black, clear inside r_in pixels of (cx, cy), fading to `alpha` at r_out and
// beyond (to the target's corners), over whatever is drawn.
bool draw_vignette(void* device, float cx, float cy, float r_in, float r_out, float alpha);
// Texture filtering for the eye passes: the game never sets it, so D3D7's defaults applied (point
// sampling, no mipmaps), and far textures crawled in the headset. mode 0 leaves the device as it
// is; 1 linear with mipmaps; 2 also anisotropic (up to `aniso`, as far as the device allows).
// Returns what was applied (for the log).
const char* set_texture_filtering(void* device, int mode, int aniso, float lod_bias);

// A surface's own size.
bool surface_size(void* surface, int* w, int* h);

bool write_bmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgr_bottom_up);

}  // namespace bw::render
