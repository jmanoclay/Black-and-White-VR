"""Scripted test of the hand's action hints in VR (words by the hand, placed in the world).

For each situation the scene is frozen and captured with the hints on and off; the difference is
the label. Checks: a label appears, in both eyes, at the same height, and shifted between the
eyes by the disparity of the hand's depth (so it sits at the hand, not on a flat screen).

Situations: laser on a tree ("Pick Up"), carrying with the laser ("Plant"), carrying in the hand at
the controller ("Throw": hidden by default; the placement checks run with hidden words shown). Last, the delay: no
word before the game has asked for it long enough.

Usage: python tools/validate/hand_hints.py [tree x z]
"""
import json
import sys
import time

import numpy as np
from PIL import Image, ImageChops

import near_hand as nh
from near_hand import bwctl

CAP = bwctl.CAPTURES


def label_box(name_on, name_off, eye):
    a = Image.open(CAP / f"{name_on}_{eye}.bmp").convert("RGB")
    b = Image.open(CAP / f"{name_off}_{eye}.bmp").convert("RGB")
    d = np.asarray(ImageChops.difference(a, b)).sum(axis=2)
    # Only the label's own colour (bright yellow letters): other things can differ between the two
    # captures too (the right eye showed unrelated differences in some runs).
    on = np.asarray(a).astype(int)
    yellow = (on[:, :, 0] > 170) & (on[:, :, 1] > 160) & (on[:, :, 2] < 130)
    ys, xs = np.nonzero((d > 60) & yellow)
    if len(xs) < 20:
        return None
    return xs.min(), xs.max(), ys.min(), ys.max()


def check(results, what):
    bwctl.stereo("freeze 1", "ui hand_hints 1")
    time.sleep(1.0)
    bwctl.stereo_capture("hint_on")
    bwctl.stereo("ui hand_hints 0")
    time.sleep(1.0)
    bwctl.stereo_capture("hint_off")
    bwctl.stereo("ui hand_hints 1", "freeze 0")
    meta = json.load(open(CAP / "hint_on.json"))
    boxes = [label_box("hint_on", "hint_off", e) for e in "LR"]
    if not all(boxes):
        results.append(nh.report(False, f"{what}: no label in {'both eyes' if not any(boxes) else 'one eye'}"))
        return
    # Captures are each eye's crop of the shared render grid: back to render pixels.
    cx = [(b[0] + b[1]) / 2 + meta["eyes"][i]["crop"][0] for i, b in enumerate(boxes)]
    cy = [(b[2] + b[3]) / 2 + meta["eyes"][i]["crop"][1] for i, b in enumerate(boxes)]
    disparity = cx[0] - cx[1]
    results.append(nh.report(abs(cy[0] - cy[1]) < 3 and disparity > 1.0,
                             f"{what}: label in both eyes, {boxes[0][1] - boxes[0][0] + 1} px wide, vertical offset "
                             f"{abs(cy[0] - cy[1]):.1f} px, disparity {disparity:.1f} px"))
    return disparity


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -30 0 0 0 0", "ui hint_delay 0 0", "ui show_hidden 1", "hands beam_still 1",
                 "hands leash_off")  # (with the creature on the leash the game asks for other words)
    time.sleep(1)
    # (The tree: the test save's, or one given as "x z" (a save where that one is outside the influence,
    # where the game offers no words: the Island 2 autosave, 2026-11-03).)
    tx, tz = (float(sys.argv[1]), float(sys.argv[2])) if len(sys.argv) > 2 else nh.TREE
    gy = nh.ground_at(tx, tz)
    bwctl.stereo(f"rig set {tx:.1f} {gy + 11.5 + (12 if len(sys.argv) > 2 else 0):.1f} {tz - 20.9:.1f} 0", "ctrl r pose 0 0 0 0.2 -0.2 0.3")
    time.sleep(1)
    bwctl.stereo(f"ctrl r aimat {tx} {gy} {tz}")
    time.sleep(4)
    far = check(results, "laser on a tree")
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(3)
    bwctl.stereo(f"ctrl r aimat {tx + 3.2} {nh.ground_at(tx + 3.2, tz - 8.9)} {tz - 8.9}")
    time.sleep(3)
    check(results, "carrying with the laser")
    bwctl.stereo("ctrl r a 1")
    time.sleep(1)
    bwctl.stereo("ctrl r a 0", "ctrl r pose 20 -10 0 0.15 -0.25 0.45")
    time.sleep(3)
    # (Which word the game asks for here varies between "Throw", "Drop" and "Plant"; the first two
    # are hidden by default, so the placement checks run with hidden words shown.)
    near = check(results, "carrying in the hand at the controller")
    if far and near:
        results.append(nh.report(near > far * 2, f"the label at the controller is much nearer than the one on the laser "
                                                 f"({near:.1f} against {far:.1f} px of disparity)"))
    # With the delay on, a word the game has only just started asking for isn't drawn yet.
    bwctl.stereo("ctrl r a 1")
    time.sleep(0.5)
    bwctl.stereo("ctrl r a 0", "ui hint_delay 30 0.3")  # nothing can have been asked for 30 s
    time.sleep(1.5)
    bwctl.stereo("freeze 1")
    time.sleep(0.5)
    bwctl.stereo_capture("hint_on")
    bwctl.stereo("ui hand_hints 0")
    time.sleep(0.5)
    bwctl.stereo_capture("hint_off")
    bwctl.stereo("ui hand_hints 1", "freeze 0", "ui hint_delay 1 0.3", "ui show_hidden 0")
    results.append(nh.report(label_box("hint_on", "hint_off", "L") is None, "no word before the delay has passed"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
