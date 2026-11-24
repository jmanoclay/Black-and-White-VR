"""Scripted test of the shield's orange "ready" ring (player, 2026-10-28: drawn off the circle,
toward the player).

With [hands] shield_autocast=0 the ring stays up while the trigger is held after the circle closes.
A shield bubble is spawned and popped (miracle_catalogue.pop), a circle of radius 15 traced with the
laser round open grass, the trigger kept held. The mod moves each of the ring's points (the game casts
them from its own screen, round the recogniser's centre) as it moves the shield's centre (log), and
in the picture the ring's orange pixels must be round the circle drawn's middle (projected into the
left eye), not round the recogniser's centre.

Boot first (with Windows audio stalled: BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/shield_ring.py
"""
import json
import math
import re
import sys
import time

import cv2
import numpy as np

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state
from miracle_catalogue import pop
from circle_try import to_spot

CX, CZ, R, T = 3150.0, 3350.0, 15.0, 3.0  # (open grass by the temple, as circle_try.py)


def project(meta, p):
    e = meta["eyes"][0]
    r = [p[i] - e["pos"][i] for i in range(3)]
    z = sum(r[i] * e["fwd"][i] for i in range(3))
    x = sum(r[i] * e["right"][i] for i in range(3))
    y = sum(r[i] * e["up"][i] for i in range(3))
    w, h = e["render"]
    return w / 2 + e["focal_px"] * x / z - e["crop"][0], h / 2 - e["focal_px"] * y / z - e["crop"][1]


def main():
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0", "hands shield_autocast 0")
    time.sleep(1.0)
    results = []
    if not pop(4, CX + 40, CZ - 60):
        print(f"SKIP  the shield's bubble doesn't pop (state {state()})")
        return 0
    g = to_spot(CX, CZ)
    bwctl.stereo(f"ctrl r aimat {CX:.1f} {g:.1f} {CZ + R:.1f}")
    time.sleep(0.5)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.3)
    bwctl.stereo(f"ctrl r aimcircle {CX:.1f} {g:.1f} {CZ:.1f} {R} {T} 1.15")
    time.sleep(T + 1.0)
    bwctl.stereo("freeze 1")
    time.sleep(0.5)
    meta = json.loads(bwctl.stereo_capture("shield_ring").read_text())
    text = ld.tail(mark)
    moved = re.findall(r"ready ring moved onto the circle drawn: by \(([-\d.]+), ([-\d.]+)\)", text)
    shift = re.findall(r"shield's circle: the recogniser's centre \(([-\d.]+), ([-\d.]+)\)", text)
    if shift:
        print(f"      the recogniser's centre ({shift[-1][0]}, {shift[-1][1]}); the circle drawn round ({CX}, {CZ})")
    results.append(nh.report(bool(moved), f"the ring's points moved as the shield's centre ({moved[-1] if moved else 'no log line'})"))
    img = cv2.imread(str(bwctl.CAPTURES / "shield_ring_L.bmp")).astype(np.int16)
    b, gr, r = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    orange = (r > 180) & (gr > 60) & (gr < 170) & (b < 90) & (r - gr > 60)
    cx, cy = project(meta, (CX, g, CZ))
    ex, _ = project(meta, (CX + R, g, CZ))
    rad = abs(ex - cx)
    yy, xx = np.mgrid[0:orange.shape[0], 0:orange.shape[1]]

    def count_near(p):
        return int(np.count_nonzero(orange & ((xx - p[0]) ** 2 + (yy - p[1]) ** 2 < (0.6 * rad) ** 2)))

    on_drawn = count_near((cx, cy))
    if shift:
        old = project(meta, (float(shift[-1][0]), g, float(shift[-1][1])))
        on_old = count_near(old) if math.hypot(old[0] - cx, old[1] - cy) > 0.8 * rad else 0
    else:
        on_old = 0
    results.append(nh.report(on_drawn > 50 and on_drawn > 3 * on_old,
                             f"in the picture the ring is on the circle drawn ({on_drawn} orange pixels round its middle, {on_old} round the recogniser's)"))
    bwctl.stereo("ctrl r trigger 0", "freeze 0")
    time.sleep(1.5)
    ld.send("hands shield_autocast 1", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
