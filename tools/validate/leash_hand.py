"""Scripted test of where the held leash starts in VR (needs a save with the leash on).

The player stands near the ground looking level, with the hand held out in front (a low angle:
the eye's ray through the hand meets the ground far off). The hand must stay the hand (not be
turned into the laser by the help text on the virtual screen in front). The leash must start at the drawn hand
(the palm at the controller; with the laser, the laser hand on its point), not at the game's own
hand. Screenshots of both (leash_to_hand 1 and 0) are saved for a look.

Usage: python tools/validate/leash_hand.py
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail, leash_state, player_creature

PALM = (0.0, -0.25, 0.45)


def start_points():
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.5)
    m = re.search(r"leash start: game \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) used \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) "
                  r"palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", tail(mark))
    v = [float(m.group(i)) for i in range(1, 10)]
    return tuple(v[0:3]), tuple(v[3:6]), tuple(v[6:9])


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 0 0 0 0 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
    time.sleep(1)
    on, _ = leash_state()
    if not on:
        send("hands leash_on")
        time.sleep(1)
        on, _ = leash_state()
    if not on:
        print("SKIP  the leash can't be put on in this save")
        return 0
    c = player_creature()
    if not c:
        print("SKIP  creature not found")
        return 0
    # 40 units south of the creature, the head 17 units (1.7 m) above the ground, looking at it.
    g = nh.ground_at(c[0], c[2] - 40)
    send(f"rig set {c[0]:.2f} {g + 17:.2f} {c[2] - 40:.2f} 0", "hands leash_to_hand 1")
    time.sleep(1.5)
    # (The tutorial's help text is up here, on the virtual screen in front: a hand held out level
    # used to land on it and be turned into the laser.)
    near, blend, _ = nh.pointer()
    results.append(nh.report(near == 1 and blend >= 1.0, f"hand held out level in front, help text up: still the hand (near {near}, blend {blend})"))
    game, used, palm = start_points()
    results.append(nh.report(math.dist(used, palm) < 0.5, f"hand: the leash starts at the palm (used {used}, palm {palm})"))
    print(f"      (the game's own start point is {math.dist(game, palm):.1f} units from the palm: {game})")
    bwctl.shot("leash_hand_on")
    send("hands leash_to_hand 0")
    time.sleep(1.0)
    bwctl.shot("leash_hand_off")
    send("hands leash_to_hand 1", "hands laser 1", "ctrl r pose 0 -25 0 0.1 -0.3 0.4")
    time.sleep(1.5)
    mark = nh.LOG.stat().st_size
    send("pointerinfo")
    time.sleep(0.4)
    m = re.search(r"pointer: valid 1 .* hit \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) land 1", tail(mark))
    game, used, palm = start_points()
    if m:
        hit = tuple(float(m.group(i)) for i in (1, 2, 3))
        results.append(nh.report(math.dist(used, hit) < 3.0, f"laser: the leash starts at the laser hand (used {used}, laser point {hit})"))
    else:
        results.append(nh.report(False, "laser: the laser doesn't land"))
    bwctl.shot("leash_laser")
    send("hands laser 0", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
