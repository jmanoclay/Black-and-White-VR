"""Scripted test: the leash's loop is drawn on the VR hand while the leash is in the hand (player,
2026-10-24: the loop still wasn't on the hand).

Puts the leash on (the palm on the player's creature, the trigger), holds the hand up, and logs the
leash meshes drawn (stereo leash_draws): one of them must be moved onto the drawn hand.

Usage: python tools/validate/leash_loop.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail
from leash_toggle import leash_on, hand_on_creature, trigger, AWAY


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
    time.sleep(1)
    state = leash_on()
    if state is None:
        print("SKIP  no leash status in this save")
        return 0
    for _ in range(3):
        if state == 1:
            break
        hand_on_creature()
        trigger()
        state = leash_on()
        send(f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
        time.sleep(1.5)
    if state != 1:
        print("SKIP  the leash couldn't be put on")
        return 0
    # The hand up (0.3 m above the default), still.
    send(f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1] + 0.3} {AWAY[2]}")
    time.sleep(1.5)
    mark = nh.LOG.stat().st_size
    send("leash_draws 60")
    time.sleep(1.5)
    t = tail(mark)
    draws = re.findall(r"leash mesh draw (\S+) mesh (\S+) at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), ([-\d.]+) from the game's wrist", t)
    moved = re.findall(r"leash loop moved: .* eye (\d)", t)
    near = sorted({(d[0], round(float(d[5]), 1)) for d in draws}, key=lambda x: x[1])
    print(f"   leash meshes drawn (object, distance from the game's wrist): {near[:6]}")
    results.append(nh.report(bool(draws), f"a leash mesh is drawn while the leash is in the hand ({len(draws)} draws)"))
    eyes = sorted(set(moved))
    results.append(nh.report(eyes == ["0", "1"], f"the loop is moved onto the drawn hand in both eyes ({len(moved)} moved draws, eyes {eyes})"))
    send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
