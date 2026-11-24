"""Far-texture shimmer, with and without the mipmap levels ([textures], src/render/mipmaps.cpp).

From a fixed spot looking over the village to the hills (rig set 1700 45 2470 0, pose 0 -8 0, time
frozen), two stereo captures with the head turned 0.1793 degrees apart: about 2 px at the centre.
For each 48 px column block of the far band near the horizon (sky left out), the best whole-pixel
shift between the two is found; the mean grey difference left over is what changed rather than
moved, which is the shimmer. Without mipmaps is the same boot with lod_bias -10 (top level only).

Checks:
- with the levels, the residual is under 80% of the one without (a fresh boot: 3.35 against 5.15; after
  run_suite.sh's start, 2.87 against 4.26: the values depend on the session, the ratio hardly).

Usage: python tools/validate/texture_shimmer.py
"""
import pathlib
import sys
import time

import cv2
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402

YAW = 0.1793
Y0, Y1, BLOCK = 0.47, 0.64, 48


def capture(tag, bias):
    bwctl.stereo("enabled 1", "vr mock", "base game", "ctrl r off", "ctrl l off", "freeze 0", "rig set 1700 45 2470 0",
                 "pose 0 -8 0 0 0 0", f"lod_bias {bias}")
    time.sleep(3)
    bwctl.stereo("freeze 1")
    time.sleep(1)
    for i, yaw in enumerate((0.0, YAW)):
        bwctl.stereo(f"pose {yaw} -8 0 0 0 0")
        time.sleep(0.8)
        if not bwctl.stereo_capture(f"tsh_{tag}_{i}"):
            raise RuntimeError("stereo capture timed out")


def residual(tag):
    out = []
    for eye in "LR":
        ca = cv2.imread(str(bwctl.CAPTURES / f"tsh_{tag}_0_{eye}.bmp"))
        a = cv2.cvtColor(ca, cv2.COLOR_BGR2GRAY).astype(np.float32)
        b = cv2.imread(str(bwctl.CAPTURES / f"tsh_{tag}_1_{eye}.bmp"), cv2.IMREAD_GRAYSCALE).astype(np.float32)
        h, w = a.shape
        bl, g, r = [ca[..., k].astype(int) for k in range(3)]
        sky = ((bl > r + 40) & (bl > 140)) | ((r > 190) & (g > 190) & (bl > 190))  # blue sky, white clouds
        sky = cv2.dilate(sky.astype(np.uint8), np.ones((7, 7), np.uint8)).astype(bool)
        y0, y1 = int(h * Y0), int(h * Y1)
        total, n = 0.0, 0
        for x0 in range(8, w - BLOCK - 8, BLOCK):
            m = ~sky[y0:y1, x0:x0 + BLOCK]
            if m.sum() < 200:
                continue
            best = min(float(np.abs(a[y0:y1, x0:x0 + BLOCK] - b[y0:y1, x0 + s:x0 + s + BLOCK])[m].mean())
                       for s in range(-6, 7))
            total += best * m.sum()
            n += m.sum()
        out.append(total / n)
    return sum(out) / 2


def main():
    capture("mip", 0)
    capture("nomip", -10)
    bwctl.stereo("lod_bias 0", "freeze 0", "pose 0 0 0 0 0 0")
    with_levels, without = residual("mip"), residual("nomip")
    ok = with_levels < 0.8 * without
    print(f"{'PASS' if ok else 'FAIL'}  far shimmer residual {with_levels:.2f} with the levels, {without:.2f} without "
          f"({100 * (1 - with_levels / without):.0f}% less)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
