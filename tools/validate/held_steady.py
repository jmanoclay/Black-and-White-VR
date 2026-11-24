"""Scripted test: a thing held in the hand stays in one place on the palm as the hand turns
(playtest 2026-10-31: people, trees, animals and scooped piles shifted and drifted on the palm as the
hand turned a lot).

Takes the near_hand.py tree with the grip, then turns the controller through a set of poses (yaw,
pitch, roll up to 90 degrees) and moves it about, and at each reads the held thing's place and axes in the palm's frame
("stereo held_log", from the drawn place). They must stay what they were in the first pose: place
within 0.3 units, axes within 5 degrees.

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/held_steady.py [verbose]
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl, TREE, CTRL, SCALE

PAT = re.compile(r"held: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) up \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) fwd \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
                 r" in the hand; game up \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) fwd \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) local \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")
# (yaw, pitch, roll, x, y, z): turns, then the hand moved about (the game's own hand then moves
# nearer to or farther from its camera, and sizes itself by that)
POSES = [(0, 10, 0, 0.05, -0.05, 0.45), (60, 10, 0, 0.05, -0.05, 0.45), (-60, 10, 0, 0.05, -0.05, 0.45), (0, 60, 0, 0.05, -0.05, 0.45),
         (0, -50, 0, 0.05, -0.05, 0.45), (0, 10, 80, 0.05, -0.05, 0.45), (0, 10, -80, 0.05, -0.05, 0.45), (90, 40, 45, 0.05, -0.05, 0.45),
         (0, 10, 0, 0.05, -0.05, 0.45), (0, 10, 0, 0.3, -0.3, 0.2), (0, 10, 0, -0.2, 0.2, 0.6), (-45, -30, 30, 0.1, -0.4, 0.3)]


def held():
    (m,) = nh.query("held_log 1", [PAT])
    v = [float(m.group(i)) for i in range(1, 19)]
    return v[0:3], v[3:6], v[6:9], v[9:12], v[12:15], v[15:18]


def angle(a, b):
    d = sum(x * y for x, y in zip(a, b)) / max(1e-6, math.hypot(*a) * math.hypot(*b))
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


def main():
    verbose = "verbose" in sys.argv
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off")
    time.sleep(1)
    g = nh.ground_at(*TREE)
    tip = (TREE[0], g + 6.0, TREE[1] - 0.5)
    rig = (tip[0] - CTRL[0] * SCALE, tip[1] - CTRL[1] * SCALE, tip[2] - CTRL[2] * SCALE)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0", f"ctrl r pose 0 -70 0 {CTRL[0]} {CTRL[1]} {CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(state == 4 and holding == 1, f"holding the tree (state {state}, holding {holding})"))
    if not results[-1]:
        return 1
    first = None
    worst_p = worst_a = 0.0
    for yaw, pitch, roll, x, y, z in POSES:
        bwctl.stereo(f"ctrl r pose {yaw} {pitch} {roll} {x} {y} {z}")
        time.sleep(2.0)
        pos, up, fwd, gup, gfwd, local = held()
        if first is None:
            first = pos, up, fwd
        dp, du, df = math.dist(pos, first[0]), angle(up, first[1]), angle(fwd, first[2])
        worst_p, worst_a = max(worst_p, dp), max(worst_a, du, df)
        if verbose:
            print(f"  pose {yaw:4} {pitch:4} {roll:4} at ({x}, {y}, {z}): pos {pos} ({dp:.2f} off) up {du:.1f} fwd {df:.1f} deg off; "
                  f"game up {gup} local {local}")
    results.append(nh.report(worst_p < 0.3, f"the tree's place on the palm kept ({worst_p:.2f} units at most)"))
    results.append(nh.report(worst_a < 5.0, f"the tree's turn in the hand kept ({worst_a:.1f} degrees at most)"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1.5)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
