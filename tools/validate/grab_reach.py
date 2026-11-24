"""Scripted test: a grip closed just short of a person takes them (playtest 2026-11-01: children
running about were taken about 1 in 3; they had moved out of the touch range by the time the grip
closed).

Villagers the game lets be picked up, near the start view: for each, the palm goes 2.2 units beside and
1.3 above its middle (2.6 units, 26 cm: outside the touch range of 20 cm + 5, inside [hands]
living_grip_cm, 35) and the grip closes. With living_grip on, the hand must come to hold a villager
nearly every time (they walk: with the catch on, a grip counts only if the mod logged a villager
within 33 cm as it closed); with it off ("compare"), it doesn't.

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/grab_reach.py [count] [compare]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from grab_near_building import objects, START

PEOPLE = ("Villager", "SpecialVillager")


def holding():
    """The hand holds something (CHand state 4, holding 1)."""
    state, held, _, _ = nh.hand()
    return state == 4 and held == 1


def trial(key, around):
    for _ in range(10):  # (a free hand to start: what the last trial took put down)
        if nh.hand()[0] in (0, 1):
            break
        bwctl.stereo("ctrl r squeeze 0")
        time.sleep(0.5)
    near = objects(*around, 15)
    if key not in near:
        return None
    c = near[key][1]
    t = (c[0] + 2.2, c[1] + 1.3, c[2])  # (2.6 units)
    ld.send(f"rig set {t[0] - ld.PALM[0] * 10:.2f} {t[1] - ld.PALM[1] * 10:.2f} {t[2] - ld.PALM[2] * 10:.2f} 0")
    time.sleep(1.0)  # (the hand needs a moment after the rig moves)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1.5)
    held = holding()
    text = ld.tail(mark)
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1.5)
    # (Where the villager was as the grip closed, from the mod's log: taken at d, or the nearest at d.)
    m = re.search(r"grip: nothing touched; \S+ ([\d.]+) m from the palm taken", text) or re.search(r"grip: no person.*?nearest \S+ ([\d.]+) m", text)
    touched = re.search(r"grip with the hand: touching 1 \(\.\?AV(\w+)@@\)", text)
    return held, float(m.group(1)) if m else None, touched.group(1) if touched else None


def main():
    count = int(next((a for a in sys.argv[1:] if a.isdigit()), 8))
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)
    objs = objects(*START, 200)
    picks = [(k, (v[1][0], v[1][2])) for k, v in objs.items() if v[0] in PEOPLE and v[3]][:count]
    print(f"      {len(picks)} villagers to try")
    results = []
    for grip in ([0, 0.35] if "compare" in sys.argv else [0.35]):
        ld.send(f"hands living_grip {grip}")
        got = [g for g in (trial(k, a) for k, a in picks) if g is not None]
        # (Counted: the grips with a villager within 33 cm by the log, or one touched; with the catch off
        # there is no log, so every grip counts.)
        if grip:
            got = [g for g in got if (g[1] is not None and g[1] < 0.33) or g[2] in PEOPLE]
        took = sum(1 for g in got if g[0])
        print(f"      living_grip {grip}: held {took} of {len(got)} ({[g for g in got]})")
        if grip:
            results.append(nh.report(len(got) >= 3 and took >= 0.75 * len(got), f"a grip 26 cm from a villager takes them ({took} of {len(got)})"))
    ld.send("hands living_grip 0.35", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
