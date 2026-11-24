"""Scripted test: a circle miracle drawn after the laser swept in from elsewhere (2026-10-31: all the
points since the trigger went down were used, so the sweep pulled the centre up to 100 units off).
The trigger is pressed pointing ~90 units away, the laser swept over to the circle, the circle drawn
(radius 15, in T s): the shield (4) and the storm (0) must go up on the circle's middle.

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/circle_sweep.py [T]   (1.5: the sweep inside the last 3 s of points)
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from miracle_catalogue import pop
from circle_try import to_spot, OTHERS

cx, cz, r = 3150.0, 3350.0, 15.0
T = float(sys.argv[1]) if len(sys.argv) > 1 else 1.5
ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
        "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
time.sleep(1.0)
results = []
for t in (4, 0):
    if not pop(t, cx + 40, cz - 60):
        results.append(nh.report(False, f"type {t}: the bubble pops (boot fresh: the ring test leaves the controller off)"))
        continue
    g = to_spot(cx, cz)
    sx, sz = cx + 70, cz + 60
    bwctl.stereo(f"ctrl r aimat {sx:.1f} {g:.1f} {sz:.1f}")
    time.sleep(0.4)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.5)
    for k in range(1, 21):
        f = k / 20
        bwctl.stereo(f"ctrl r aimat {sx + (cx - sx) * f:.1f} {g:.1f} {sz + (cz + r - sz) * f:.1f}")
        time.sleep(0.04)
    bwctl.stereo(f"ctrl r aimcircle {cx:.1f} {g:.1f} {cz:.1f} {r} {T} 1.15")
    time.sleep(T + 0.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    text = ld.tail(mark)
    made = [(m.group(1), float(m.group(2)), float(m.group(3)))
            for m in re.finditer(r"spell made by \S+ -> \d+.*?spell \S+ (\S+); points in it: \+64 \(([-\d.]+), [-\d.]+, ([-\d.]+)\)", text)
            if m.group(1) not in OTHERS]
    print([l for l in text.splitlines() if "the drawn one's" in l][-1:])
    if made:
        off = math.hypot(made[0][1] - cx, made[0][2] - cz)
        results.append(nh.report(off < 4.0, f"type {t}: after a sweep in, on the circle's middle ({off:.1f} units off)"))
    else:
        results.append(nh.report(False, f"type {t}: cast after a sweep in"))
sys.exit(0 if all(results) else 1)
