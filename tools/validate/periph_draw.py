"""Probe: what draws the small glowing ball fixed at the right edge of the right eye (player,
2026-10-27: seen only in the periphery). Bisects the frame's draws over its pixels and logs the
game code asking for each (log_draws).

Usage: python tools/validate/periph_draw.py
"""
import sys
import time

import cv2
import numpy as np

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl


def grab(name, skip):
    bwctl.stereo(f"skip_draws {skip[0]} {skip[1]}")
    time.sleep(0.4)
    bwctl.stereo_capture(name)
    return cv2.imread(str(bwctl.CAPTURES / f"{name}_R.bmp")).astype(np.float32)


def main():
    bwctl.stereo("freeze 1")
    time.sleep(0.5)
    ref = grab("pd_ref", (-1, -1))
    h, w = ref.shape[:2]
    # The ball: around (89%, 54%) of the right eye's image.
    x0, y0, x1, y1 = int(w * 0.84), int(h * 0.48), int(w * 0.95), int(h * 0.60)
    region = ref[y0:y1, x0:x1]
    found, ranges = [], [(0, 3000)]
    while ranges and len(found) < 8:
        a, b = ranges.pop()
        e = float(np.mean(np.abs(grab("pd", (a, b))[y0:y1, x0:x1] - region)))
        if e < 1.0:
            continue
        if b - a == 1:
            found.append((a, e))
            continue
        m = (a + b) // 2
        ranges += [(m, b), (a, m)]
    bwctl.stereo("skip_draws -1 -1")
    print(f"   draws painting it: {found}")
    for d, _ in found:
        mark = nh.LOG.stat().st_size
        bwctl.stereo(f"log_draws {d} {d + 1}")
        time.sleep(0.6)
        for line in ld.tail(mark).splitlines():
            if "asked for by" in line or "draw" in line:
                print("   " + line[11:240])
    bwctl.stereo("freeze 0")
    print("SKIP  (probe)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
