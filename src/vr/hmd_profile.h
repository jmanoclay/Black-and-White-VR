#pragma once
#include <string>

#include "vr/math.h"

namespace bw::vr {

// Per-eye optics in OpenVR terms: projection tangents (left/top negative) and the eye's offset
// from the head origin, already converted to engine axes (meters).
struct EyeOptics {
    float tan_left = -1, tan_right = 1, tan_top = -1, tan_bottom = 1;
    Vec3 offset;
};

struct HmdProfile {
    std::string model = "default";
    int render_w = 1440, render_h = 1600;  // recommended per-eye size
    float refresh_hz = 90;
    float ipd_m = 0.063f;
    EyeOptics eye[2];  // 0 = left, 1 = right
};

// Reads the key/value text written by tools/deploy.ps1 from profiles/index.json.
// Falls back to a generic profile (and logs it) if the file is missing.
HmdProfile load_profile(const std::wstring& path);

}  // namespace bw::vr
