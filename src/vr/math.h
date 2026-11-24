// Minimal vector/rotation math for the stereo camera.
// Engine camera space is left-handed: +X right, +Y up, +Z forward (see documentation/render-pass-map.md).
// All rotations here are expressed in that convention.
#pragma once
#include <cmath>

namespace bw::vr {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 0 ? a * (1.0f / l) : a;
}

// Rotation stored as three world-space axes (the columns of the rotation matrix).
struct Basis {
    Vec3 right{1, 0, 0}, up{0, 1, 0}, fwd{0, 0, 1};

    // Local -> parent.
    Vec3 apply(Vec3 v) const { return right * v.x + up * v.y + fwd * v.z; }
    // this * other: other's axes expressed in this basis' parent frame.
    Basis compose(const Basis& o) const { return {apply(o.right), apply(o.up), apply(o.fwd)}; }
};

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// yaw: + turns right (about +Y); pitch: + looks up (about -X); roll: + tilts the head
// clockwise as seen from behind (about -Z). Applied in yaw, pitch, roll order (head-style).
inline Basis basis_from_ypr(float yaw_deg, float pitch_deg, float roll_deg) {
    const float y = yaw_deg * kDegToRad, p = pitch_deg * kDegToRad, r = roll_deg * kDegToRad;
    Basis yaw{{std::cos(y), 0, -std::sin(y)}, {0, 1, 0}, {std::sin(y), 0, std::cos(y)}};
    Basis pitch{{1, 0, 0}, {0, std::cos(p), -std::sin(p)}, {0, std::sin(p), std::cos(p)}};
    Basis roll{{std::cos(r), -std::sin(r), 0}, {std::sin(r), std::cos(r), 0}, {0, 0, 1}};
    return yaw.compose(pitch).compose(roll);
}

// Basis looking along `fwd` with no roll (world up = +Y).
inline Basis basis_look(Vec3 fwd) {
    fwd = normalize(fwd);
    Vec3 right = normalize(cross(Vec3{0, 1, 0}, fwd));
    if (length(right) < 1e-6f) right = {1, 0, 0};
    return {right, cross(fwd, right), fwd};
}

struct Pose {
    Basis rot;
    Vec3 pos;  // meters, tracking space
};

}  // namespace bw::vr
