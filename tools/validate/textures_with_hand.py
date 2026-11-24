"""The world keeps its textures while the mod draws its own things (the hand, the laser, markers).

The mod's draws set their own texture stages and must put the game's back. Through dxwrapper a
Direct3D 7 state block didn't (2026-10-19): once the hand was drawn, the whole world was drawn
without textures, white and grey. Checks, on a frozen view:
- with a controller (the hand and its marker drawn), the land is as colourful as without one;
- and the land is green, not white (saturation of the lower half of each eye).

Usage: python tools/validate/textures_with_hand.py
"""
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh


def saturation(name, eye):
    im = np.asarray(Image.open(nh.bwctl.CAPTURES / f"{name}_{eye}.bmp").convert("HSV"), dtype=float)
    h = im.shape[0]
    return im[h // 2:, :, 1].mean() / 255.0


def main():
    results = []
    nh.bwctl.stereo("enabled 1", "vr mock", "base level", "hands near 1", "hands always 1", "hands laser 0", "ctrl r off",
                    "pose 0 -30 0 0 0 0", "freeze 1")
    time.sleep(2)
    nh.bwctl.stereo_capture("tex_nohand")
    nh.bwctl.stereo("ctrl r pose 0 0 0 0.1 -0.3 0.4")  # (the hand appears, low in the view)
    time.sleep(2)
    nh.bwctl.stereo_capture("tex_hand")
    nh.bwctl.stereo("hands laser 1")  # (the laser's beam and diamond: the mod's own triangles)
    time.sleep(2)
    nh.bwctl.stereo_capture("tex_laser")
    time.sleep(0.5)
    nh.bwctl.stereo("hands laser 0", "ctrl r off", "freeze 0")
    for name, what in (("tex_hand", "the hand"), ("tex_laser", "the laser")):
        for eye in "LR":
            a, b = saturation("tex_nohand", eye), saturation(name, eye)
            results.append(nh.report(b > 0.25 and b > 0.7 * a, f"eye {eye}: the land keeps its colour with {what} drawn (saturation {a:.2f} without, {b:.2f} with)"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
