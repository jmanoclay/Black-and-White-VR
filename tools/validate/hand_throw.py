"""Scripted throw from the hand at the controller, with mock controllers on the live game.

The hand uproots the test tree (as in near_hand.py), swings sideways and up in steps, and the grip
opens mid-swing. The mod's log then shows the velocity it gave the game and where the object was
one second later.

Checks: the throw used the hand's velocity taken at release, and the object travelled.

Usage: python tools/validate/hand_throw.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0", "hands throw_trace 1",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
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
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(state == 4 and holding == 1, f"holding the tree (state {state}, holding {holding})"))
    mark = nh.LOG.stat().st_size
    # Swing to the right and up, ~0.3 m per step, and let go mid-swing.
    for i in range(4):
        bwctl.stereo(f"ctrl r pose 0 10 0 {-0.3 + 0.3 * i:.2f} {-0.2 + 0.1 * i:.2f} 0.45")
        time.sleep(0.12)
    bwctl.stereo("ctrl r squeeze 0", "ctrl r pose 0 10 0 0.9 0.2 0.45")
    time.sleep(4)
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        text = f.read()
    thrown = re.search(r"hands: launch: .* -> \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) \[hand[^]]*\]", text)
    speed = sum(float(thrown.group(i)) ** 2 for i in (1, 2, 3)) ** 0.5 if thrown else 0.0
    went = re.search(r"hands: thrown object went from .* in 1 s: ([\d.]+) units", text)
    results.append(nh.report(thrown is not None and speed > 5.0 and float(thrown.group(1)) > 0,
                             f"the launch used the hand's velocity at release (game velocity {speed:.1f})"))
    dist = float(went.group(1)) if went else 0.0
    results.append(nh.report(dist > 5.0, f"the object travelled ({dist:.1f} units in 1 s)"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
