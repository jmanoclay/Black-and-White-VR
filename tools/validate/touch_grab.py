"""Scripted test of touch grabbing: the hand at the controller takes the object it is inside,
far out of reach of the ground.

The palm is put in the test tree's crown, ~1.1 m above the ground (reach is 1 m):
1. the tree counts as touched (pointerinfo: touch 1) and the ground is out of reach (reach 0);
2. the grip plus a sideways pull uproots it (CHand state 4, holding);
3. with the palm in open air nothing is touched and the grip grabs nothing.

Usage: python tools/validate/touch_grab.py
"""
import sys
import time

import near_hand as nh
from near_hand import bwctl


def touch():
    m = nh.query("pointerinfo", [r"pointer: touch (\d) object \S+ centre \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) radius ([\d.]+) dist ([\d.]+)",
                                 r"reach (\d)", r"rmb (\d)"])
    return int(m[0].group(1)), float(m[0].group(5)), float(m[0].group(6)), int(m[1].group(1)), int(m[2].group(1))


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "hands touch 1", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
    time.sleep(1)
    # Open air first: 3 m above the ground, away from the tree.
    bwctl.stereo(f"rig set {nh.TREE[0] + 30:.1f} 50 {nh.TREE[1] - 34.9:.1f} 0", "pose 0 -30 0 0 0 0", "ctrl r pose 0 -20 0 0 -0.2 0.49")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1)
    touching, _, _, reach, rmb = touch()
    results.append(nh.report(touching == 0 and reach == 0 and rmb == 0, f"open air: nothing touched, the grip grabs nothing (touch {touching}, rmb {rmb})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.5)
    # The palm in the tree's crown.
    g = nh.ground_at(*nh.TREE)
    bwctl.stereo(f"rig set {nh.TREE[0]:.1f} {g + 13.5:.1f} {nh.TREE[1] - 4.9:.1f} 0", "ctrl r pose 0 -20 0 0 -0.2 0.49")
    time.sleep(2.5)
    touching, radius, dist, reach, _ = touch()
    results.append(nh.report(touching == 1 and reach == 0,
                             f"palm in the tree's crown, out of reach of the ground: touched (sphere radius {radius:.1f}, palm {dist:.1f} from its centre)"))
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (-0.1, -0.2, -0.3, -0.4, -0.5, -0.6):
        bwctl.stereo(f"ctrl r pose -20 -20 0 {x} -0.15 0.45")
        time.sleep(0.4)
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(state == 4 and holding == 1, f"gripped and uprooted from the crown (state {state}, holding {holding})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
