"""Find which draw(s) produce a region of the image, by skipping draw ranges and re-capturing.

Assumes the game runs in gameplay with stereo enabled and frozen. Measures how much the given
region of the left eye changes when draws [a, b) are skipped, and bisects down to single draws.

Usage: python tools/validate/bisect_draws.py x0 y0 x1 y1 [total_draws]
"""
import pathlib
import sys
import time

import cv2
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402


def grab(name, skip):
    bwctl.stereo(f"skip_draws {skip[0]} {skip[1]}")
    time.sleep(0.4)
    if not bwctl.stereo_capture(name):
        raise RuntimeError("capture failed")
    return cv2.imread(str(bwctl.CAPTURES / f"{name}_L.bmp")).astype(np.float32)


def main(argv):
    x0, y0, x1, y1 = map(int, argv[:4])
    total = int(argv[4]) if len(argv) > 4 else 300
    ref = grab("bis_ref", (-1, -1))[y0:y1, x0:x1]

    def effect(a, b):
        img = grab("bis", (a, b))[y0:y1, x0:x1]
        return float(np.mean(np.abs(img - ref)))

    ranges = [(0, total)]
    found = []
    while ranges:
        a, b = ranges.pop()
        e = effect(a, b)
        print(f"skip [{a},{b}) -> region change {e:.2f}")
        if e < 1.0:
            continue
        if b - a == 1:
            found.append((a, e))
            continue
        m = (a + b) // 2
        ranges += [(m, b), (a, m)]
    bwctl.stereo("skip_draws -1 -1")
    print("draws affecting the region:", found)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
