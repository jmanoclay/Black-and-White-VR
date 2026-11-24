"""Scripted test: the trees below look the same in both eyes while the hand carries a tree (the
user's save: the tree near_hand.py uses, carried over the village's edge).

The game puts a carried thing at its own hand (on the ground below the player's) during the left
eye's pass, before drawing the trees, and the mod moved it into the player's hand later in that
pass. Trees between the eye and a carried thing turn see-through, so the trees below looked
different in each eye (player, 2026-10-18). The right eye's pass now repeats the left's.

Check (coarse): around the trees below, the right eye matches the left (after the best horizontal
shift) about as well as with the carried thing left where the game puts it (stereo "held_follow
0"). The measure is noisy (the carried tree's crown moves into the area); the captures
carry_eyes_L/_R are the thing to look at.

Usage: python tools/validate/carry_eyes.py
"""
import json
import sys
import time

import near_hand as nh
from near_hand import bwctl
from PIL import Image, ImageChops, ImageStat

SPOT = "rig set 1733.1 47.2 2598.8 0", "pose 99.8 -74.6 0 0 0 0"  # looking down at a fir and a palm


def mismatch(name):
    """Mean difference between the eyes over the trees, at the best horizontal shift."""
    d = json.load(open(bwctl.CAPTURES / f"{name}.json"))
    cl, cr = d["eyes"][0]["crop"][0], d["eyes"][1]["crop"][0]
    a = Image.open(bwctl.CAPTURES / f"{name}_L.bmp").convert("L").crop((650, 1000, 1150, 1250))
    best = 1e9
    for dx in range(-60, 61, 2):
        x = 650 + cl - cr + dx
        b = Image.open(bwctl.CAPTURES / f"{name}_R.bmp").convert("L").crop((x, 1000, x + 500, 1250))
        best = min(best, ImageStat.Stat(ImageChops.difference(a, b)).mean[0])
    return best


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "held_follow 1")
    time.sleep(1)
    g = nh.ground_at(*nh.TREE)
    tip = (nh.TREE[0], g + 6.0, nh.TREE[1] - 0.5)
    rig = tuple(t - c * nh.SCALE for t, c in zip(tip, nh.CTRL))
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0", f"ctrl r pose 0 -70 0 {nh.CTRL[0]} {nh.CTRL[1]} {nh.CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(1)
    if nh.hand()[0] != 4:
        print("SKIP  the tree wasn't picked up")
        bwctl.stereo("ctrl r off", "hands reach 0.25")
        return 0
    bwctl.stereo("ctrl r pose 0 -60 0 0.25 -0.4 0.2", *SPOT)
    time.sleep(2.5)
    bwctl.stereo_capture("carry_eyes")
    carried = mismatch("carry_eyes")
    bwctl.stereo("held_follow 0")
    time.sleep(1.5)
    bwctl.stereo_capture("carry_eyes_base")
    base = mismatch("carry_eyes_base")
    bwctl.stereo("held_follow 1")
    results.append(nh.report(carried <= base * 1.3 + 1.0,
                             f"carrying, the trees below match between the eyes (mismatch {carried:.1f}; with the thing left at the game's place {base:.1f})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1.5)
    bwctl.stereo("ctrl r off", "hands reach 0.25")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
