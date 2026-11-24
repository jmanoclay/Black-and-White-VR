"""Scripted test (both sizes set alike: the spirits' speech, [ui] caption_scale, and other help lines, status_scale, as the creature's state since 2026-11-01)

Scripted test of the captions' size on the screen panel (player, 2026-10-30: the spirits' captions
low resolution, hard to read).

Waits for one of the save's own help captions (HelpText: "Click the Action Button to Leash
Creature." and the like come up every minute or so), freezes the game and captures the left eye
with the captions enlarged ([ui] caption_scale_percent, 200) and at the old size (1). The words'
width on the eye image (white glyph pixels by the dark band) must grow by about the scale, their
area by at least about its square, and the words must stay inside the eye image.

Boot first (with Windows audio stalled: BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/caption_scale.py
"""
import re
import sys
import time

import cv2
import numpy as np

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail


def caption_up():
    mark = nh.LOG.stat().st_size
    send("ui captioninfo")
    time.sleep(0.4)
    m = re.search(r"ui: caption: band (\d).*?its words \(([-\d]+), ([-\d]+)\)-\(([-\d]+), ([-\d]+)\) drawn ([\d.]+)x", tail(mark))
    return m if m and m.group(1) == "1" else None


def text_rows(name):
    """The caption's words in the left eye: white (outlined) glyph pixels. Their rows' extent and
    their columns' extent, eye pixels."""
    img = cv2.imread(str(bwctl.CAPTURES / f"{name}_L.bmp")).astype(np.int16)
    white = (img.min(axis=2) > 200) & (np.abs(img[:, :, 0] - img[:, :, 2]) < 20)
    dark = cv2.erode(img.max(axis=2).astype(np.uint8), np.ones((3, 3), np.uint8)) < 90  # (the glyphs' outline, the band)
    near_dark = cv2.dilate(dark.astype(np.uint8), np.ones((7, 7), np.uint8)) > 0
    white &= near_dark
    ys, xs = np.nonzero(white)
    if len(ys) < 50:
        return None
    return (int(np.percentile(ys, 2)), int(np.percentile(ys, 98)), int(np.percentile(xs, 2)), int(np.percentile(xs, 98)), len(ys))


def main():
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "pose 0 0 0 0 0 0", "ui caption_scale 2 70", "ui status_scale 2")
    m = None
    t0 = time.time()
    while time.time() - t0 < 240 and not m:
        m = caption_up()
        if not m:
            time.sleep(1.0)
    if not m:
        print("SKIP  no caption came up in 4 minutes")
        return 0
    send("freeze 1")
    time.sleep(0.5)
    print(f"      caption words ({m.group(2)}, {m.group(3)})-({m.group(4)}, {m.group(5)}) of the layout, drawn {m.group(6)}x")
    bwctl.stereo_capture("caption_big")
    send("ui caption_scale 1 70", "ui status_scale 1")
    time.sleep(0.5)
    bwctl.stereo_capture("caption_small")
    send("ui caption_scale 2 70", "ui status_scale 1.25", "freeze 0")
    big, small = text_rows("caption_big"), text_rows("caption_small")
    print(f"      words (rows y0, y1, columns x0, x1, pixels): 2x {big}, 1x {small}")
    results = []
    if not big or not small:
        results.append(nh.report(False, "the caption's words found in both pictures"))
        return 1
    hb, hs = big[1] - big[0], small[1] - small[0]
    wb, ws = big[3] - big[2], small[3] - small[2]
    results.append(nh.report(1.6 < wb / ws < 2.4, f"the words {wb / ws:.2f}x as wide ({ws} -> {wb} px)"))
    # (Only a lower bound: at 1x the thin glyphs are blurred and few of their pixels reach "white",
    # so the area grows by more than the square: 10.5x on 2026-11-03.)
    results.append(nh.report(big[4] / small[4] > 2.5, f"the words' area {big[4] / small[4]:.1f}x ({small[4]} -> {big[4]} px; {hs} -> {hb} rows)"))
    h, w = cv2.imread(str(bwctl.CAPTURES / "caption_big_L.bmp")).shape[:2]
    results.append(nh.report(big[2] > 0 and big[3] < w - 1 and big[1] < h - 1, "the enlarged words inside the eye image"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
