"""Scripted test: a food or wood miracle in the hand sits on the palm, and stays there while it is
cast (player, 2026-10-27: it started off the palm and drifted away and back while casting).

Needs no bubble in the save: spawns one (hands spawn_bubble), pops it into the hand with the
trigger, holds the hand up over open land, and reads where the miracle's object is drawn against
the palm (handinfo's "moved to", pointerinfo's palm), still and while the trigger pours it.
With "old", only the old way ([stereo] miracle_on_palm off), for the record.

Checks:
- the bubble pops (hand state 8);
- held still, it is kept with the controller where it settled (the game's look: the hand holding it);
- while pouring, it moves with the palm by less than a unit (it doesn't bob away).

Usage: python tools/validate/miracle_hand.py [food|wood] [x z] [old]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import picked_class, state

TYPES = {"food": 3, "wood": 8}


def pop(x, z):
    g = nh.ground_at(x, z)
    ld.send(f"hands spawn_bubble {TYPES[KIND]} {x} {z}")
    time.sleep(1.5)
    ld.send("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(0.5)
    for dy in (2.0, 3.0, 1.0, 4.0, 5.0):
        ld.place_palm((x, g + dy, z))
        time.sleep(0.8)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.15)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if state() == 8:
            return True
    return False


def held_vs_palm():
    """(held object's drawn position, palm) now."""
    m = nh.LOG.stat().st_size
    bwctl.stereo("handinfo 1", "pointerinfo")
    time.sleep(0.6)
    t = ld.tail(m)
    a = re.findall(r"handinfo: held object at .*moved to \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)
    p = re.findall(r"pointer: touch \d object \S+ centre .* palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)
    if not a or not p:
        return None
    return [float(v) for v in a[-1]], [float(v) for v in p[-1]]


def run(results, x, z, on):
    ld.send(f"miracle_on_palm {1 if on else 0}")
    if not pop(x, z):
        results.append(nh.report(False, f"the {KIND} bubble pops into the hand (hand state {state()})"))
        return
    results.append(nh.report(True, f"the {KIND} bubble pops into the hand (state 8)"))
    g = nh.ground_at(x, z)
    ld.place_palm((x, g + 10.0, z))
    m = nh.LOG.stat().st_size
    time.sleep(5.5)
    off = re.findall(r"miracle in the hand kept with the controller, ([\d.]+) units from the palm \(the game's place ([\d.]+) from it\)", ld.tail(m))
    still = [held_vs_palm() for _ in range(3)]
    rel = [[a[i] - p[i] for i in range(3)] for a, p in filter(None, still)]
    # Pouring: the trigger held over open land.
    bwctl.stereo("ctrl r trigger 1")
    pour = []
    for _ in range(6):
        time.sleep(0.4)
        s = held_vs_palm()
        if s:
            pour.append([s[0][i] - s[1][i] for i in range(3)])
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.0)
    spread = max((math.dist(a, b) for a in pour for b in pour), default=-1)
    label = "on the palm" if on else "the old way (miracle_on_palm 0)"
    print(f"   {label}: object origin minus palm while still {[[round(v, 2) for v in r] for r in rel]}; while pouring {[[round(v, 2) for v in r] for r in pour]}")
    if on:
        results.append(nh.report(bool(off), f"held, it is kept with the controller ({off[-1][0] if off else '?'} units from the palm; the game's place {off[-1][1] if off else '?'} from that)"))
        results.append(nh.report(0 <= spread < 1.0, f"while pouring it moves with the palm (spread {spread:.2f} units)"))
    else:
        print(f"   (old way: spread while pouring {spread:.2f} units)")


def main():
    global KIND
    args = [a for a in sys.argv[1:] if a != "old"]
    KIND = args[0] if args and args[0] in TYPES else "food"
    nums = [float(a) for a in args if re.match(r"^-?[\d.]+$", a)]
    x, z = (nums[0], nums[1]) if len(nums) >= 2 else (3262.0, 3300.0)
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    time.sleep(1.0)
    if "old" in sys.argv:
        run([], x, z, False)
        return 0
    run(results, x, z, True)
    bwctl.stereo("ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
