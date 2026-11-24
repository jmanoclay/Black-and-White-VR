"""Scripted test: a circle miracle (storm 0, shield 4, physical shield 5) cast by drawing a circle
(2026-10-31: the storm couldn't be cast in VR). Spawns the bubble, pops it, then in hand mode:
- holds the trigger a moment and lets go without drawing: it stays in the hand;
- holds the trigger (the hand turns into the laser) and traces a circle on the land: as the game
  recognises it (action 4) the miracle is cast, on the circle's middle.

Usage: python tools/validate/circle_try.py [type ...] [x z] [debug]
  (default: 0 4 5. debug: hands circle_type <type> sends any type down the circle's path, to try it.)
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state
from miracle_catalogue import pop, NAMES

OTHERS = (".?AVSpellHeal@@", ".?AVSpellResource@@")  # (the villages' own, made all the time)


def to_spot(cx, cz):
    g = nh.ground_at(cx, cz)
    ld.send("hands laser 0", f"rig set {cx} {g + 50:.1f} {cz - 45} 0", "pose 0 -45 0 0 0 0",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)
    return g


def try_type(t, cx, cz, results, r=15.0, T=3.0):
    if not pop(t, cx + 40, cz - 60):
        results.append(nh.report(False, f"{NAMES[t]}: the bubble pops (state {state()})"))
        return
    g = to_spot(cx, cz)
    # Held without drawing, then let go: still in the hand.
    bwctl.stereo(f"ctrl r aimat {cx:.1f} {g:.1f} {cz + r:.1f}")
    time.sleep(0.4)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.6)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.2)
    results.append(nh.report(state() == 8, f"{NAMES[t]}: let go of without a circle, still in the hand (state {state()})"))
    # A circle: cast as it closes.
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.3)
    bwctl.stereo(f"ctrl r aimcircle {cx:.1f} {g:.1f} {cz:.1f} {r} {T} 1.15")
    time.sleep(T + 0.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    text = ld.tail(mark)
    rec = sorted(set(re.findall(r"recognises action (\d+)", text)))
    made = [(m.group(1), float(m.group(2)), float(m.group(3)))
            for m in re.finditer(r"spell made by \S+ -> \d+.*?spell \S+ (\S+); points in it: \+64 \(([-\d.]+), [-\d.]+, ([-\d.]+)\)", text)
            if m.group(1) not in OTHERS]
    results.append(nh.report("4" in rec, f"{NAMES[t]}: the game recognises the circle (actions {rec})"))
    results.append(nh.report(state() != 8 and bool(made), f"{NAMES[t]}: the circle casts it (state {state()}; {[m[0] for m in made]})"))
    if made:
        off = math.hypot(made[0][1] - cx, made[0][2] - cz)
        results.append(nh.report(off < 4.0, f"{NAMES[t]}: it goes up on the circle's middle ({off:.1f} units off)"))


def main():
    args = [a for a in sys.argv[1:] if a != "debug"]
    nums = [float(a) for a in args]
    cx, cz = 3150.0, 3350.0  # (open grass by the temple, where the player cast a shield)
    if len(nums) >= 2 and nums[-1] > 100:
        cx, cz = nums[-2], nums[-1]
        nums = nums[:-2]
    types = [int(n) for n in nums] or [0, 4, 5]
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    time.sleep(1.0)
    results = []
    for t in types:
        ld.send(f"hands circle_type {t if 'debug' in sys.argv else -1}")
        try_type(t, cx, cz, results)
    ld.send("hands circle_type -1", "ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
