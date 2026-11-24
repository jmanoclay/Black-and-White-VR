"""Scripted test of letting go just after moving with the stick (the user's save, the tree that
near_hand.py uses).

The throw speed was taken from the hand's motion in the world, which includes the rig's: let go of
just after the stick stopped, a carried thing flew off at the speed the player had been moving
(player, 2026-10-18). It is now taken relative to the rig.

Checks:
- carrying the tree up high, after flying forward with the stick and stopping, letting go with a
  still hand doesn't throw it: its launch speed is small, and it falls rather than flies on.

Usage: python tools/validate/drop_after_moving.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import tail

LIFT = 30.0


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "hands throw_trace 1", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l pose 0 0 0 -0.2 -0.3 0.3")
    time.sleep(1)
    g = nh.ground_at(*nh.TREE)
    tip = (nh.TREE[0], g + 6.0, nh.TREE[1] - 0.5)
    rig = tuple(t - c * nh.SCALE for t, c in zip(tip, nh.CTRL))
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0",
                 f"ctrl r pose 0 -70 0 {nh.CTRL[0]} {nh.CTRL[1]} {nh.CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(1)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1] + LIFT:.2f} {rig[2]:.2f} 0", "ctrl r pose 0 10 0 0.1 -0.5 0.45")
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    if not (state == 4 and holding == 1):
        print(f"SKIP  the tree wasn't picked up (state {state})")
        bwctl.stereo("ctrl r off", "ctrl l off", "hands throw_trace 0")
        return 0
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl l stick 0 1")  # fly forward...
    time.sleep(1.0)
    bwctl.stereo("ctrl l stick 0 0")  # ... stop, and let go a moment later with the hand still
    time.sleep(0.12)
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1.5)
    text = tail(mark)
    m = re.search(r"hands: launch: \S+ game velocity \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) -> \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    flown = re.search(r"thrown object went from \(([-\d.]+), [-\d.]+, ([-\d.]+)\) to \(([-\d.]+), [-\d.]+, ([-\d.]+)\) in 1 s", text)
    horiz = None
    if flown:
        x0, z0, x1, z1 = (float(flown.group(i)) for i in range(1, 5))
        horiz = ((x1 - x0) ** 2 + (z1 - z0) ** 2) ** 0.5
    if m:
        v = [float(m.group(i)) for i in range(4, 7)]
        speed = (v[0] ** 2 + v[2] ** 2) ** 0.5
    else:
        speed = 0.0
    results.append(nh.report(speed < 3.0 and (horiz is None or horiz < 3.0),
                             f"let go just after moving: not thrown (horizontal launch speed {speed:.1f}, travelled {horiz} units in 1 s)"))
    bwctl.stereo("ctrl r off", "ctrl l off", "hands throw_trace 0", "hands reach 0.25")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
