"""Scripted check: with the creature on the leash, a quick trigger tap on the land sends it there.

The leash is put in the hand and a spot of open land is picked 30 units to the creature's side (away
from the temple). The mod should send the creature there (packet 0x5E, Creature::ForceActOnMapCoord)
and the creature should end up there:
- laser, pointed at the spot: a held pull (0.8 s) only holds the land, nothing is sent; a tap (0.12 s) sends it;
- the hand at the controller, 30 cm above the spot (within reach of the ground): a tap sends it;
- the hand at the controller held high (out of reach), pointing at the spot: a tap sends it.

Usage: python tools/validate/leash_land.py (run it on its own: the creature walks off afterwards)
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -30 0 0 0 0")
    time.sleep(1)
    on, _ = ld.leash_state()
    if not on:
        ld.send("hands leash_on")
        time.sleep(1)
    on, tied = ld.leash_state()
    c = ld.player_creature()
    if not (on and c and tied in ("00000000", "0")):
        print(f"SKIP  the leash isn't in the hand with the creature on it (on {on}, tied {tied}, creature {c})")
        return 0
    cases = (("laser", 0.8, False), ("laser", 0.12, True), ("hand low", 0.12, True), ("hand pointing", 0.12, True))
    for mode, hold, should_send in cases:
        c = ld.player_creature()
        tx, tz = c[0] - 30, c[2] + 15
        ty = nh.ground_at(tx, tz)
        if mode == "hand low":
            # A spot where the palm touches nothing (on a thing, the trigger sends the creature to the thing).
            ld.send("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
            time.sleep(1.0)
            free = False
            for ox, oz in ((-30, 15), (-30, -15), (30, -20), (25, 25), (-40, 0)):
                tx, tz = c[0] + ox, c[2] + oz
                ty = nh.ground_at(tx, tz)
                if not ld.place_palm((tx, ty + 2.0, tz)):
                    continue
                time.sleep(0.5)
                mark = nh.LOG.stat().st_size
                ld.send("pointerinfo")
                if re.search(r"pointer: touch 0 ", ld.tail(mark)):
                    free = True
                    break
            if not free:
                print(f"SKIP  {mode}: no spot near the creature where the palm touches nothing")
                continue
        else:
            rx, rz = c[0] - 5, c[2] + 40
            g = nh.ground_at(rx, rz)
            ld.send(f"hands laser {1 if mode == 'laser' else 0}", f"rig set {rx:.1f} {g + 15:.1f} {rz:.1f} 0",
                    "ctrl r pose 0 0 0 0.1 -0.3 0.4")
            time.sleep(1)
            ld.send(f"ctrl r aimat {tx:.1f} {ty:.1f} {tz:.1f}")
            time.sleep(1)
        mark = nh.LOG.stat().st_size
        nh.bwctl.stereo("ctrl r trigger 1")
        time.sleep(hold)
        nh.bwctl.stereo("ctrl r trigger 0")
        time.sleep(0.5)
        t = ld.tail(mark)
        m = re.search(r"hands: leash: sending the creature to \(([-\d.]+), [-\d.]+, ([-\d.]+)\)", t)
        if should_send and not m and "hands: leash: sending the creature to " in t:
            print(f"SKIP  {mode}: there was a thing under the hand, the creature was sent to it")
            continue
        d = 1e9
        for _ in range(8):  # (up to 6 s)
            time.sleep(0.75)
            c1 = ld.player_creature()
            d = min(d, math.dist((c1[0], c1[2]), (tx, tz)))
        if should_send:
            off = math.dist((float(m.group(1)), float(m.group(2))), (tx, tz)) if m else 1e9
            results.append(nh.report(m is not None and off < 3.0, f"{mode}, tap ({hold} s): the creature was sent to the spot "
                                                                  f"({off:.1f} units from it)"))
            results.append(nh.report(d < 8.0, f"{mode}, tap: the creature went there (as near as {d:.1f} units within 6 s)"))
        else:
            results.append(nh.report("hands: leash: sending the creature to" not in t, f"{mode}, held pull ({hold} s): only holds the land, nothing sent"))
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
