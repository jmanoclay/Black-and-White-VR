"""Scripted test of the two-handed world grab (both triggers, hand at the controller).

Run twice: with [hands] two_hand_scale on, then off (the default since 2026-10-31).
1. Bringing the hands together to half their separation doubles the rig's scale (the world shrinks);
   with scaling off the scale stays as it was.
2. Turning the hands a quarter turn about each other turns the rig a quarter turn.
3. Moving both hands together moves the rig the opposite way by the same amount.
With scaling on, the two world points taken hold of stay in the hands (checked through the wrist).

Usage: python tools/validate/two_hand.py
"""
import math
import sys
import time

import near_hand as nh
from near_hand import bwctl


def rig():
    (m,) = nh.query("rig info", [r"rig: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) yaw ([-\d.]+) scale ([\d.]+)"])
    return [float(m.group(i)) for i in range(1, 6)]


def hands(r, l):
    bwctl.stereo(f"ctrl r pose 0 0 0 {r[0]} {r[1]} {r[2]}", f"ctrl l pose 0 0 0 {l[0]} {l[1]} {l[2]}")
    time.sleep(1.0)


def palm_world():
    (m,) = nh.query("pointerinfo", [r"pointer: touch .* palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"])
    return tuple(float(m.group(i)) for i in (1, 2, 3))


def run(scaling, results):
    bwctl.stereo(f"hands two_hand_scale {int(scaling)}", "enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "world_scale 10", "ctrl r a 0", "ctrl r squeeze 0", "ctrl r trigger 0", "ctrl l trigger 0",
                 "pose 0 -30 0 0 0 0")
    time.sleep(1)
    bwctl.stereo("rig set 2231 40 3140 0")
    time.sleep(0.5)
    hands((0.4, -0.2, 0.4), (-0.4, -0.2, 0.4))
    time.sleep(1.5)
    bwctl.stereo("ctrl r trigger 1", "ctrl l trigger 1")
    time.sleep(1.0)
    held = palm_world()
    x0, y0, z0, yaw0, s0 = rig()
    hands((0.2, -0.2, 0.4), (-0.2, -0.2, 0.4))
    _, _, _, yaw1, s1 = rig()
    want_s = s0 * 2 if scaling else s0
    tag = "scaling on" if scaling else "scaling off"
    results.append(nh.report(abs(s1 - want_s) < 0.1 and abs(yaw1 - yaw0) < 0.5, f"{tag}: hands half as far apart: scale {s0:.2f} -> {s1:.2f}"))
    hands((0.0, -0.2, 0.2), (0.0, -0.2, 0.6))
    _, _, _, yaw2, s2 = rig()
    turn = (yaw2 - yaw1 + 540) % 360 - 180
    results.append(nh.report(abs(abs(turn) - 90) < 1.0 and abs(s2 - s1) < 0.1, f"{tag}: quarter turn of the hands: rig turned {turn:.1f} degrees"))
    if scaling:
        drift = math.dist(palm_world(), held)
        results.append(nh.report(drift < 0.2, f"{tag}: the held world point stayed in the hand ({drift:.2f} units of drift)"))
    xa, ya, za, _, sa = rig()
    hands((0.1, -0.1, 0.2), (0.1, -0.1, 0.6))
    xb, yb, zb, _, _ = rig()
    moved = math.dist((xa, ya, za), (xb, yb, zb))
    want = math.hypot(0.1, 0.1) * sa
    results.append(nh.report(abs(moved - want) < 0.1 and abs(yb - ya + 0.1 * sa) < 0.1,
                             f"{tag}: both hands moved together: rig moved {moved:.2f} units (expected {want:.2f})"))
    bwctl.stereo("ctrl r trigger 0", "ctrl l trigger 0")
    time.sleep(0.5)
    _, _, _, _, s3 = rig()
    results.append(nh.report(abs(s3 - sa) < 0.01, f"{tag}: scale kept after letting go ({s3:.2f})"))
    bwctl.stereo("ctrl l off", "ctrl r off", "rig reset")
    time.sleep(0.5)


def main():
    results = []
    run(True, results)
    run(False, results)
    bwctl.stereo("hands two_hand_scale 0")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
