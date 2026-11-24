"""Scripted test: the hand's own speed on the creature decides between a stroke and a slap
(player, 2026-10-30: however hard the hand swung, it only stroked).

The game decides once a second from its cursor's speed, and a real hand speeds up through the
stroke's band on its way to a slap. The mod now gives the game a cursor speed for the hand's
speed. The mock hand moves smoothly (ctrl r vel), side to side.

Checks, with the hand held on the creature (its stroking state, 7):
- held still, and a slow rub: never a slap (a resting hand gets the game's own slow strokes);
- a rub (0.6 m/s): the game strokes;
- a hard swing (2 m/s): the game slaps;
- a rub again: back to stroking.

Usage: python tools/validate/pet_slap.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from give_creature import creatures, own_creature

PALM = (0.0, -0.3, 0.5)  # metres from the head, rig heading 0


def state():
    s, _, _, _ = nh.hand()
    return s


def follow():
    cx, cy, cz, r = own_creature()  # (the player's: there can be others in the level)
    # (The palm at its centre: in the front of its loose sphere, the cursor was off its body.)
    bwctl.stereo(f"rig set {cx - PALM[0] * 10:.2f} {cy - PALM[1] * 10:.2f} {cz - PALM[2] * 10:.2f} 0")


def watch(speed, secs):
    """The game's strokes and slaps while the hand rubs side to side at this speed (m/s, smooth,
    as a real hand: ctrl r vel), 15 cm or more each way."""
    leg = max(0.15 / speed, 0.16) if speed > 0 else 0  # (requests closer than ~0.15 s get lost)
    follow()
    bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0] - speed * leg / 2} {PALM[1]} {PALM[2]}")
    time.sleep(0.4)
    mark = nh.LOG.stat().st_size
    t0 = time.time()
    sign = 1
    while time.time() - t0 < secs:
        if speed <= 0:
            time.sleep(0.5)
            continue
        bwctl.stereo(f"ctrl r vel {sign * speed} 0 0")
        time.sleep(leg)
        sign = -sign
    bwctl.stereo("ctrl r vel 0 0 0")
    time.sleep(0.2)
    return re.findall(r"hands: petting: the game (slapped|stroked) the creature", ld.tail(mark))


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", "ctrl r pose 0 0 0 0.6 0.3 0.5")
    time.sleep(1)
    if not own_creature():
        print("SKIP  no creature of the player's in this save")
        return 0
    def stroking(mark):  # (from the log: asking with handinfo meanwhile kept the presses from taking)
        t = ld.tail(mark)
        return t.rfind("-> 7 (") > max(t.rfind("7 -> "), -1)
    for attempt in range(3):  # (the hand taken away and put back, as a player would)
        mark = nh.LOG.stat().st_size
        follow()
        bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
        t0 = time.time()
        k = 0
        last_follow = time.time()
        while time.time() - t0 < 12.0 and not stroking(mark):  # (small rubs, as a hand resting on it)
            k += 1
            bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0] + (0.03 if k % 2 else -0.03)} {PALM[1]} {PALM[2]}")
            time.sleep(0.25)
            if time.time() - last_follow > 4.0:
                follow()
                last_follow = time.time()
        if stroking(mark):
            break
        bwctl.stereo("ctrl r pose 0 0 0 0.6 0.3 0.5")
        time.sleep(1.5)
    results.append(nh.report(state() == 7, f"the hand on the creature: stroking state ({state()})"))
    if state() != 7:
        bwctl.stereo("ctrl r off")
        return 1
    # (A hand resting on the creature also gets the game's own slow strokes: slow motion may stroke,
    # never slap.)
    checks = ((0.0, "no slap", "held still"), (0.15, "no slap", "a slow rub (0.15 m/s)"), (0.6, "stroke", "a rub (0.6 m/s)"),
              (2.0, "slap", "a hard swing (2 m/s)"), (0.6, "stroke", "a rub again"))
    for speed, want, label in checks:
        got = watch(speed, 3.5)
        ok = {"no slap": "slapped" not in got, "stroke": "stroked" in got and "slapped" not in got, "slap": "slapped" in got}[want]
        results.append(nh.report(ok, f"{label}: {want}; the game {', '.join(got) if got else 'did nothing'}"))
    bwctl.stereo("ctrl r vel 0 0 0", "ctrl r pose 0 0 0 0.6 0.3 0.5")
    time.sleep(1.5)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
