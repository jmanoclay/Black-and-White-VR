"""Check: one engine pass for both eyes ([stereo] one_pass) draws what the two passes draw.

Freezes the game in a busy view (the mock headset), captures both eyes with one_pass off and
on, and compares: the mean difference, and the horizontal shift that best lines the pictures up
near the bottom (the ground close by: the largest parallax) and in the middle. With "k", also
tries scales of the eyes' shift in the replay (stereo one_pass_k).

Usage: python tools/validate/one_pass_check.py [k]
"""
import sys
import time

import cv2
import numpy as np

import near_hand as nh
from near_hand import bwctl


def grab(name):
    bwctl.stereo_capture(name)
    return [cv2.imread(str(bwctl.CAPTURES / f"{name}_{e}.bmp"), 0).astype(np.float32) for e in "LR"]


def compare(a, b):
    out = []
    h = a.shape[0]
    for rows in (slice(int(h * 0.75), int(h * 0.95)), slice(int(h * 0.5), int(h * 0.7))):
        best = min((np.abs(np.roll(b[rows], s, axis=1)[:, 40:-40] - a[rows][:, 40:-40]).mean(), s) for s in range(-6, 7))
        out.append(best[1])
    return float(np.abs(a - b).mean()), out


def main():
    bwctl.stereo("enabled 1", "vr mock", "base level", "pose 0 -30 0 0 0 0", "rig set 2723 90 2061 0")
    time.sleep(5)
    bwctl.stereo("freeze 1")
    time.sleep(1)
    bwctl.stereo("one_pass 0")
    time.sleep(1)
    ref = grab("op_ref")
    ks = [1.0, 1.5, 2.0, 2.5, 3.0] if "k" in sys.argv[1:] else [1.0]
    results = []
    for k in ks:
        bwctl.stereo("one_pass 1", f"one_pass_k {k}")
        time.sleep(1)
        got = grab("op_try")
        for e, name in ((0, "left"), (1, "right")):
            mean, shifts = compare(ref[e], got[e])
            print(f"k {k}: {name} eye: mean difference {mean:.2f}, best shift near {shifts[0]} px, middle {shifts[1]} px")
            if k == 1.0:
                results.append(nh.report(mean < 3.0 and all(abs(s) <= 1 for s in shifts), f"one pass, {name} eye, lines up with the two passes"))
    bwctl.stereo("one_pass_k 1", "one_pass 1", "freeze 0")  # (on: the default)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
