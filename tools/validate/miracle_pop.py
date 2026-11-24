"""Scripted test of a one-shot miracle's bubble (the user's save: the strength miracle by the temple).

In the flat game a quick click on the bubble pops it, and the miracle is then charged in the hand
(CHand state 8). A creature's miracle is cast by clicking with it on the creature; shaking the
hand drops it. In VR the trigger does the click.

Checks (SKIP when there is no bubble on the pedestal):
- the trigger on the bubble (the hand on it; with "grip", holding it first) pops it: state 8;
- "cast" (the default): the trigger with the palm on the creature applies the miracle to it, and
  the hand is free again;
- "shake": a quick shake drops the charged miracle (the hand leaves state 8).

Usage: python tools/validate/miracle_pop.py [cast|shake] [grip]
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from leash_shake import BASE, sweep

BUBBLE = (1757.0, 2523.8)  # the pedestal's bubble (x, z)


def state():
    return nh.hand()[0]


def trigger_tap():
    nh.bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.15)
    nh.bwctl.stereo("ctrl r trigger 0")


def main():
    mode = "shake" if "shake" in sys.argv[1:] else "cast"
    grip = "grip" in sys.argv[1:]
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {BUBBLE[0]} {BUBBLE[1]} 20")
    time.sleep(0.8)
    m = re.search(r"OneOffSpellSeed@@ at \(([-\d.]+), \+[-\d.]+, ([-\d.]+)\)", ld.tail(mark))
    if not m:
        print("SKIP  no miracle bubble on the pedestal in this save")
        return 0
    bx, bz = float(m.group(1)), float(m.group(2))  # (where it is in this boot: it isn't always at the same spot)
    g = nh.ground_at(bx, bz)
    ld.place_palm((bx + 1.5, g + 5.0, bz))  # (in its glow, off the potion inside it)
    time.sleep(0.6)
    mark = nh.LOG.stat().st_size
    if grip:
        nh.bwctl.stereo("ctrl r squeeze 1")
        time.sleep(1.5)
        results.append(nh.report(state() == 4, "grip: the bubble is held"))
    trigger_tap()
    if grip:
        time.sleep(0.3)
        nh.bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2)
    s = state()
    results.append(nh.report(s == 8 and "the bubble popped" in ld.tail(mark), f"trigger: the bubble popped, the miracle is charged (state {s})"))
    if s != 8:
        print("\n".join(l[12:200] for l in ld.tail(mark).splitlines() if re.search(r"bubble|game hand state", l)))
        return 1
    mark = nh.LOG.stat().st_size
    if mode == "shake":
        ld.send(f"ctrl r pose 0 0 0 {BASE[0]} {BASE[1]} {BASE[2]}")
        time.sleep(0.5)
        sweep(0.15, 0.08, 8)
        s = state()
        results.append(nh.report("shake given to the game" in ld.tail(mark) and s != 8, f"shake: the charged miracle is dropped (state {s})"))
    else:
        for _ in range(6):  # (the creature may be walking)
            c = ld.player_creature()
            if c:
                ld.place_palm((c[0], c[1] + 1.0, c[2]))
            time.sleep(0.4)
        nh.bwctl.stereo("hands follow_creature 1")
        time.sleep(1.0)
        trigger_tap()
        for _ in range(16):  # (the creature takes it in its own time)
            time.sleep(0.5)
            s = state()
            if s != 8:
                break
        nh.bwctl.stereo("hands follow_creature 0")
        text = ld.tail(mark)
        results.append(nh.report("applied to .?AVCreature@@" in text and s != 8, f"cast: applied to the creature, the hand is free (state {s})"))
    print("\n".join(l[12:200] for l in ld.tail(mark).splitlines() if re.search(r"bubble|miracle|magic hand|shake|game hand state", l)))
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
