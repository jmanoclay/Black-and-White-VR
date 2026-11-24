"""Probe: which draw paints the fireball that flies from the ground, and which game code asks for it.

Does the fireball_throw throw (from 100 units up), freezes the game a moment after the release,
finds the fireball's bright pixels in the left eye, bisects the draws that change them, and logs
the game code asking for each (log_draws).

Usage: python tools/validate/fireball_draw.py
"""
import re
import sys
import time

import cv2
import numpy as np

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import picked_class, state


def grab(name, skip):
    bwctl.stereo(f"skip_draws {skip[0]} {skip[1]}")
    time.sleep(0.4)
    if not bwctl.stereo_capture(name):
        raise RuntimeError("capture failed")
    return cv2.imread(str(bwctl.CAPTURES / f"{name}_L.bmp")).astype(np.float32)


def main():
    cx, cz = 3242.0, 3164.0
    pts = sorted([(cx + dx, cz + dz) for dz in range(-30, 31, 6) for dx in range(-30, 31, 6)], key=lambda q: abs(q[0] - cx) + abs(q[1] - cz))
    gs = {p: nh.ground_at(*p) for p in pts}
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", f"rig set {cx} {max(gs.values()) + 50:.1f} {cz - 50} 0", "pose 0 -45 0 0 0 0")
    time.sleep(1.5)
    bubble = None
    for p in pts:
        for dy in (1.0, 2.5, 5.0, 9.0):
            bwctl.stereo(f"ctrl r aimat {p[0]} {gs[p] + dy:.1f} {p[1]}")
            time.sleep(0.3)
            if picked_class() == ".?AVOneOffSpellSeed@@":
                bubble = (p[0], gs[p] + dy, p[1])
                break
        if bubble:
            break
    if not bubble:
        print("SKIP  no bubble")
        return 0
    bwctl.stereo("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    for dy in (0.0, 1.0, -1.0, 2.0, 3.0):
        ld.place_palm((bubble[0], bubble[1] + dy, bubble[2]))
        time.sleep(0.8)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.12)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if state() == 8:
            break
    if state() != 8:
        print("SKIP  not charged")
        return 0
    g = nh.ground_at(bubble[0], bubble[2])
    ld.place_palm((bubble[0], g + 100.0, bubble[2] - 6))
    time.sleep(1.0)
    bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} 0.2")
    time.sleep(0.6)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.5)
    for k in range(1, 5):
        bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1] + 0.05 * k:.2f} {0.2 + 0.15 * k:.2f}")
        time.sleep(0.1)
    time.sleep(0.05)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(0.15)
    bwctl.stereo("freeze 1")
    time.sleep(0.5)
    ref = grab("fbd_ref", (-1, -1))
    # The fireball: bright orange-white pixels in the lower part of the view.
    h, w = ref.shape[:2]
    b, gch, r = ref[:, :, 0], ref[:, :, 1], ref[:, :, 2]
    mask = ((r > 245) & (gch > 200)).astype(np.uint8)
    mask[: h // 3, :] = 0
    n, lab, stats, cent = cv2.connectedComponentsWithStats(mask)
    blobs = [i for i in range(1, n) if 15 <= stats[i, cv2.CC_STAT_AREA] <= 4000 and stats[i, cv2.CC_STAT_WIDTH] < 120 and stats[i, cv2.CC_STAT_HEIGHT] < 160]
    if not blobs:
        print("SKIP  no fireball blob found")
        bwctl.stereo("freeze 0")
        return 0
    i = max(blobs, key=lambda k: stats[k, cv2.CC_STAT_AREA])
    bx, by, bw_, bh = stats[i, 0], stats[i, 1], stats[i, 2], stats[i, 3]
    x0, y0, x1, y1 = max(0, bx - 6), max(0, by - 6), min(w, bx + bw_ + 6), min(h, by + bh + 6)
    print(f"   fireball blob: {stats[i, cv2.CC_STAT_AREA]} px in ({x0}, {y0})-({x1}, {y1}) of {len(blobs)} blobs")
    region = ref[y0:y1, x0:x1]

    def effect(a, bb):
        img = grab("fbd", (a, bb))[y0:y1, x0:x1]
        return float(np.mean(np.abs(img - region)))

    total = 2600
    ranges, found = [(0, total)], []
    while ranges and len(found) < 6:
        a, bb = ranges.pop()
        e = effect(a, bb)
        if e < 2.0:
            continue
        if bb - a == 1:
            found.append(a)
            continue
        m = (a + bb) // 2
        ranges += [(m, bb), (a, m)]
    bwctl.stereo("skip_draws -1 -1")
    print(f"   draws painting it: {found}")
    for d in found:
        mark = nh.LOG.stat().st_size
        bwctl.stereo(f"log_draws {d} {d + 1}")
        time.sleep(0.6)
        for line in ld.tail(mark).splitlines():
            if "asked for by" in line:
                print("   " + line[11:])
    bwctl.stereo("freeze 0")
    print("SKIP  (probe)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
