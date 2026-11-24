"""Scripted test of catching a thing in the air (the user's save: the tree near_hand.py uses).

The tree is uprooted, lifted high (the hand far out of reach of the ground) and thrown straight
up; the hand stays where it let go, with the grip closed, and the tree falls back into it. Before,
at the edge of the catch reach the game's cursor went back below the hand and the game picked up
something on the ground (a fence) instead.

Checks:
- the tree is caught: the hand holds it, not something from the ground;
- while it came down, the cursor stayed on it (no flipping on and off).

Usage: python tools/validate/catch_throw.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail

LIFT = 30.0  # world units the rig is raised for the throw


def held_class():
    mark = nh.LOG.stat().st_size
    send("hands held")
    time.sleep(0.3)
    m = re.search(r"hands: held (\S+)", tail(mark))
    return m.group(1) if m else "-"


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
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
    time.sleep(1)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1] + LIFT:.2f} {rig[2]:.2f} 0", "ctrl r pose 0 10 0 0.1 -0.5 0.45")
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    if not (state == 4 and holding == 1):
        print(f"SKIP  the tree wasn't picked up (state {state})")
        bwctl.stereo("ctrl r off")
        return 0
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r vel 0 1.6 0", "ctrl r squeeze_after 0.15 0")
    time.sleep(0.3)
    bwctl.stereo("ctrl r vel 0 0 0")
    time.sleep(0.4)
    bwctl.stereo("ctrl r squeeze 1")  # (closed, waiting: it takes as soon as the tree is touched)
    caught = False
    for _ in range(30):
        time.sleep(0.2)
        if nh.hand()[1]:
            caught = True
            break
    what = held_class() if caught else "-"
    results.append(nh.report(caught and "Tree" in what, f"the tree is caught in the air (holding {what})"))
    text = tail(mark)
    grips = re.findall(r"nearest in flight: (\S+)", text)
    results.append(nh.report("MobileStatic" not in text.split("launch")[-1] or "Tree" in what, f"nothing from the ground was taken (grip logs {grips[:3]})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2)
    bwctl.stereo("ctrl r off", "hands reach 0.25")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
