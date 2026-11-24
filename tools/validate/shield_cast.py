"""Scripted test: casting the shield from the hand (player, 2026-10-27/28).

Runs on the player's save by the temple (the worship site's floating miracle icons by (3321, 3234)).
Takes the physical shield from its icon with the laser, goes back to hand mode, then:
- holds the trigger a moment and lets go without drawing: the shield stays in the hand;
- holds the trigger (the hand turns into the laser), draws a circle on the land with it: as the
  game recognises the circle (action 4) the shield is cast, the trigger still held, on the circle.

The trigger holds the game's Action press (the right button), as when pouring a miracle: held, the
game starts the circle's recording itself ("Cast Circle").

Usage: python tools/validate/shield_cast.py
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state

ICON = (3321.1, 81.5, 3234.3)  # (the worship site's physical shield icon)


def near_on():
    (m,) = nh.query("pointerinfo", [r"pointer: near (\d) blend"])
    return m.group(1) == "1"


def take_shield():
    x, y, z = ICON
    ld.send("hands laser 1", f"rig set {x} 95 {z - 30} 0", "pose 0 -20 0 0 0 0")
    time.sleep(1.2)
    bwctl.stereo(f"ctrl r aimat {x} {y} {z}")
    time.sleep(0.5)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.12)
    bwctl.stereo("ctrl r trigger 0")
    for _ in range(12):
        time.sleep(1.0)
        if state() == 8:
            return True
    return False


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off")
    time.sleep(1.0)
    if state() != 8 and not take_shield():
        print("SKIP  no shield taken from the worship site (not the save by the temple?)")
        return 0
    m = nh.LOG.stat().st_size
    ld.send("hands seed")
    time.sleep(0.6)
    k = re.search(r"type (-?\d+)", ld.tail(m))
    if not k or k.group(1) not in ("4", "5"):
        print(f"SKIP  the hand holds seed type {k.group(1) if k else '?'}, not a shield")
        return 0
    cx, cz = 3150.0, 3350.0  # (open grass, where the player cast one: beside the temple it may be moved or hidden)
    g = nh.ground_at(cx, cz)
    r = 15.0
    ld.send("hands laser 0", f"rig set {cx} {g + 50:.1f} {cz - 45} 0", "pose 0 -45 0 0 0 0",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)
    results.append(nh.report(state() == 8, f"the shield in the hand, in hand mode (state {state()})"))

    # Held without drawing, then let go: the shield stays.
    bwctl.stereo(f"ctrl r aimat {cx:.1f} {g:.1f} {cz + r:.1f}")
    time.sleep(0.4)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.6)
    results.append(nh.report(not near_on(), "the trigger held turns the hand into the laser"))
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.2)
    results.append(nh.report(state() == 8, f"let go of without a circle: still in the hand (state {state()})"))

    # A circle, then let go: cast.
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.3)
    T = 3.0
    bwctl.stereo(f"ctrl r aimcircle {cx:.1f} {g:.1f} {cz:.1f} {r} {T} 1.15")
    time.sleep(T + 0.5)
    t = ld.tail(mark)
    results.append(nh.report("recognises action 4" in t, "the game recognises the circle"))
    t = ld.tail(mark)
    made = re.findall(r"spell made .*?spell [0-9A-F]+ ([^;]+);", t)
    results.append(nh.report(state() != 8 and any("Shield" in s for s in made) and "closed: cast" in t,
                             f"closing the circle casts the shield, the trigger still held (state {state()}; spells made {made})"))
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    t = ld.tail(mark)
    results.append(nh.report(near_on(), "the hand comes back"))
    # Where it went up: on the circle drawn (it went well off it, toward the player; 2026-10-28).
    cast = re.search(r"spell [0-9A-F]+ \.\?AVSpellShield@@; points in it: \+64 \(([-\d.]+), [-\d.]+, ([-\d.]+)\)", t)
    off = math.hypot(float(cast.group(1)) - cx, float(cast.group(2)) - cz) if cast else None  # (the circle aimed at)
    results.append(nh.report(off is not None and off < 4.0, f"the shield goes up on the circle drawn ({off if off is None else round(off, 1)} units off its middle)"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
