"""Scripted test: an area miracle (food) cast with the hand at the controller (the user's save after
the food tutorial: a pile of food miracles by the village store).

In hand mode a food miracle in the hand couldn't be cast: the trigger only applied one-shot
miracles to a target, and the grip presses only near the ground (player, 2026-10-23). Now the
trigger holds the game's action button while it is held: the miracle pours on the land (or store)
below the hand.

Checks:
- the trigger with the laser on a bubble pops it into the hand (hand state 8);
- then with the hand at the controller (hand mode):
- with the hand raised above the store, the hint names the trigger, and holding the trigger
  pours the miracle: the hand leaves state 8.

Usage: python tools/validate/food_miracle.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl

BUBBLE = (2460.0, 2600.0)  # where the laser found a food bubble in the player's save
STORE = (2480.0, 2570.0)


def state():
    return nh.hand()[0]


def main():
    results = []
    gb = nh.ground_at(*BUBBLE)
    gs = nh.ground_at(*STORE)
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", "ctrl l off",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    popped = False
    bwctl.stereo("hands laser 1", f"rig set {BUBBLE[0]} {gb + 60:.1f} {BUBBLE[1] - 40} 0", "pose 0 -50 0 0 0 0")
    time.sleep(1.5)
    for dx, dy in ((0, 2), (0, 4), (0, 6), (4, 3), (-4, 3), (0, 0)):
        bwctl.stereo(f"ctrl r aimat {BUBBLE[0] + dx} {gb + dy:.1f} {BUBBLE[1]}")
        time.sleep(1.0)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.3)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if state() == 8:
            popped = True
            break
    bwctl.stereo("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    results.append(nh.report(popped, f"the trigger on the bubble: the miracle in the hand (state {state()})"))
    if not popped:
        return 1
    ld.place_palm((STORE[0], gs + 15.0, STORE[1]))  # 1.5 m above the store
    time.sleep(1.5)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("pose 0 -70 0 0 0 0")
    time.sleep(0.5)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(3.0)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    text = ld.tail(mark)
    after = state()
    results.append(nh.report("pouring while held" in text and after != 8,
                             f"held trigger above the store pours it: hand state 8 -> {after} "
                             f"({'pouring logged' if 'pouring while held' in text else 'no pouring logged'})"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
