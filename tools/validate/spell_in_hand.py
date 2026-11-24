"""Scripted test: a charged miracle in the hand is drawn at the VR palm, not on the land below it
(player, 2026-10-24: the water miracle's rain cloud sat on the ground while the hand was up high).

Runs on the player's save of 2026-10-24 (a miracle bubble near the temple, around the given spot):
finds a bubble with the laser, pops it with the trigger, then holds the hand (hand mode) 15 units
above the ground and reads where the game drew the miracle (CHand +0x4964, logged by the mod).

Checks:
- the bubble pops (hand state 8);
- with the hand up high, the miracle is drawn at the palm's height, not the ground's.

Usage: python tools/validate/spell_in_hand.py [x z]
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl


def picked_class():
    m = nh.LOG.stat().st_size
    bwctl.stereo("pointerinfo")
    time.sleep(0.5)
    k = re.search(r"picked (\S+) (\S+)", ld.tail(m))
    return k.group(2) if k else "-"


def state():
    return nh.hand()[0]


def main():
    cx, cz = (float(sys.argv[1]), float(sys.argv[2])) if len(sys.argv) > 2 else (1872.0, 2518.0)
    results = []
    pts = sorted([(cx + dx, cz + dz) for dz in range(-24, 25, 6) for dx in range(-24, 25, 6)], key=lambda q: abs(q[0] - cx) + abs(q[1] - cz))
    gs = {p: nh.ground_at(*p) for p in pts}
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", f"rig set {cx} {max(gs.values()) + 50:.1f} {cz - 50} 0",
            "pose 0 -45 0 0 0 0")
    time.sleep(1.5)
    bubble = None
    for p in pts:
        for dy in (2.0, 5.0, 9.0, 14.0):
            bwctl.stereo(f"ctrl r aimat {p[0]} {gs[p] + dy:.1f} {p[1]}")
            time.sleep(0.3)
            if picked_class() == ".?AVOneOffSpellSeed@@":
                bubble = (p[0], gs[p] + dy, p[1])
                break
        if bubble:
            break
    if not bubble:
        print("SKIP  no miracle bubble found near the spot")
        return 0
    print(f"   bubble at {bubble}")
    # (Popped with the palm in it, as the player does: with the laser here the game's click went to
    # something lying under the bubble.)
    bwctl.stereo("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    # The trigger held on the bubble picks it up (a tap pops it; player, 2026-10-24).
    held = None
    for dy in (0.0, 1.0, -1.0):
        ld.place_palm((bubble[0], bubble[1] + dy, bubble[2]))
        time.sleep(0.8)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(1.2)
        held = state()
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if held == 4:
            break
    results.append(nh.report(held == 4, f"the trigger held on the bubble picks it up (hand state {held}), let go it leaves the hand (now {state()})"))
    if held == 4:  # (put down where it was let go of: look for it there)
        bubble = (bubble[0], nh.ground_at(bubble[0], bubble[2]) + 1.0, bubble[2])
    for dy in (0.0, 1.0, -1.0, 2.0, 3.0):
        ld.place_palm((bubble[0], bubble[1] + dy, bubble[2]))
        time.sleep(0.8)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.15)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if state() == 8:
            break
    results.append(nh.report(state() == 8, f"the trigger pops the bubble (hand state {state()})"))
    if state() != 8:
        return 1
    # Hand mode, the palm 15 units above the ground.
    bwctl.stereo("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    g = nh.ground_at(bubble[0], bubble[2])
    palm = (bubble[0], g + 15.0, bubble[2])
    ld.place_palm(palm)
    m = nh.LOG.stat().st_size
    time.sleep(3)
    t = ld.tail(m)
    k = re.findall(r"miracle in the hand drawn at the palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\); the game's hand \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)
    if not k:
        results.append(nh.report(False, "the miracle in the hand is drawn at the palm (no log line: not drawn there)"))
    else:
        at_y, game_y = float(k[-1][1]), float(k[-1][4])
        results.append(nh.report(abs(at_y - palm[1]) < 2.0 and at_y - g > 10.0,
                                 f"with the hand 15 up, the miracle is drawn at the palm: y {at_y:.1f} (palm {palm[1]:.1f}, ground {g:.1f}, "
                                 f"the game's hand {game_y:.1f})"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
