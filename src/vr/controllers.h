// Motion-controller state shared by the OpenVR input layer, the mock (scripted) controllers and
// the hand/pointer logic. Poses are in engine axes, tracking space, meters (like the head pose).
#pragma once
#include "vr/math.h"

namespace bw::vr {

enum HandSide { kLeft = 0, kRight = 1 };

struct Controller {
    bool active = false;      // tracked this frame
    Pose aim;                 // pointing pose: origin at the tip, fwd along the pointing direction
    Pose grip;                // where the hand holds the controller
    Vec3 grip_vel;            // the grip's velocity (tracking space, m/s), from SteamVR's tracking
    bool has_vel = false;     // ... when SteamVR gave one (not the mock)
    float trigger = 0;        // 0..1
    float squeeze = 0;        // grip squeeze 0..1
    bool a = false, b = false;
    float stick_x = 0, stick_y = 0;
    bool stick_click = false;
    float curl[5] = {};       // finger curl 0..1 (thumb..pinky), from skeletal input
};

}  // namespace bw::vr
