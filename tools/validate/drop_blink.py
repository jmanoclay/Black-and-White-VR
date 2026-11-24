"""Scripted test: a tree let go of up in the air leaves the hand smoothly (the user's save, the
tree near_hand.py uses).

The player saw a palm let go of leave the hand, blink, and then fall, in both eyes (2026-10-20).
With `stereo drop_log`, where the left eye draws it is logged frame by frame from the release.

Check: from frame to frame it never jumps by more than 3 units (a fall or a throw moves it well
under 1 unit a frame).

Usage: python tools/validate/drop_blink.py [throw]
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import tail


def main():
    throw = len(sys.argv) > 1 and sys.argv[1] == "throw"
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl r vel 0 0 0")
    time.sleep(1)
    g = nh.ground_at(*nh.TREE)
    tip = (nh.TREE[0], g + 6.0, nh.TREE[1] - 0.5)
    rig = tuple(t - c * nh.SCALE for t, c in zip(tip, nh.CTRL))
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0",
                 f"ctrl r pose 0 -70 0 {nh.CTRL[0]} {nh.CTRL[1]} {nh.CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(1)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1] + 40:.2f} {rig[2]:.2f} 0", "ctrl r pose 0 -30 0 0.1 -0.4 0.45")
    time.sleep(2.5)
    state, holding, _, _ = nh.hand()
    if not (state == 4 and holding == 1):
        print(f"SKIP  the tree wasn't picked up (state {state})")
        bwctl.stereo("ctrl r off", "hands reach 0.25")
        return 0
    mark = nh.LOG.stat().st_size
    bwctl.stereo("drop_log 1500")
    time.sleep(0.3)
    if throw:
        bwctl.stereo("ctrl r vel 0 1.0 1.5")
        time.sleep(0.15)
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.2)
    bwctl.stereo("ctrl r vel 0 0 0")
    time.sleep(2.5)
    bwctl.stereo("drop_log 0")
    text = tail(mark)
    pts = []
    for block in text.split("drops: frame:")[1:]:
        m = re.search(r"drops: eye 0 draws (held|released) \S+ at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", block)
        t = re.match(r"[^\n]*", block)
        if m:
            pts.append((m.group(1), tuple(float(m.group(i)) for i in (2, 3, 4))))
    jumps = []
    for i in range(1, len(pts)):
        d = math.dist(pts[i - 1][1], pts[i][1])
        if d > 3.0:
            jumps.append((i, round(d, 1), pts[i - 1], pts[i]))
    ok = len(pts) > 50 and not jumps
    print(("PASS" if ok else "FAIL") + f"  {'thrown' if throw else 'dropped'} from high: {len(pts)} frames drawn, jumps over 3 units: {len(jumps)}")
    for j in jumps[:6]:
        print("   ", j)
    bwctl.stereo("ctrl r off", "hands reach 0.25")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
