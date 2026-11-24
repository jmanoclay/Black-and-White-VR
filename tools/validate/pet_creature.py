"""Scripted test of petting in VR (needs a save with a creature).

The scripted hand is put straight onto the creature (no approach, no earlier touch) and rubbed.
The game's hand must go into its stroking state (7) without any button, stay there while the hand
rubs, and leave it when the hand is taken away. Then once more, to check it works a second time.

Usage: python tools/validate/pet_creature.py
"""
import sys
import time

import near_hand as nh
from near_hand import bwctl
from give_creature import creatures, own_creature

PALM = (0.0, -0.3, 0.5)  # metres from the head, rig heading 0


def state():
    s, _, _, _ = nh.hand()
    return s


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", "ctrl r pose 0 0 0 0.6 0.3 0.5")
    time.sleep(1)
    found = creatures()
    if not found:
        print("SKIP  no creature in this save")
        return 0
    def follow(away=0.0):
        """Put the palm on the creature where it is now (it wanders), or `away` units past its size
        to the side. The player is moved, not the hand: a jump of the controller is a fast swing,
        a slap (the creature grows: r 14.7 on the 2026-11 save, so a hand 60 cm off was still on it)."""
        cx, cy, cz, r = own_creature()  # the player's (the game's own mark, not the smallest: 2026-11-01)
        dx = (r + away) if away else 0.0
        bwctl.stereo(f"rig set {cx + dx - PALM[0] * 10:.2f} {cy - PALM[1] * 10:.2f} {cz - PALM[2] * 10:.2f} 0")

    for attempt in (1, 2):
        bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
        follow(away=15.0)
        time.sleep(1.5)
        results.append(nh.report(state() in (0, 1), f"attempt {attempt}: hand away from the creature: free"))
        follow()
        s0, t0 = 0, time.time()
        # (The creature first finishes what it is doing: the mod holds the press up to 6 s.)
        while time.time() - t0 < 7.0:
            time.sleep(0.5)
            s0 = state()
            if s0 == 7:
                break
        took = time.time() - t0
        for _ in range(2):
            for dx in (0.05, 0.10, 0.05, 0.0, -0.05, -0.10, -0.05, 0.0):
                bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0] + dx} {PALM[1]} {PALM[2]}")
                time.sleep(0.15)
        s1 = state()
        results.append(nh.report(s0 == 7 and s1 == 7, f"attempt {attempt}: hand on the creature, no button: stroking state after {took:.1f} s and still after rubbing ({s0}, {s1})"))
        follow(away=15.0)  # (flying away, the player's case)
        # (The mod lets go 0.25 s after the palm leaves; the game ends its stroke in its own time,
        # up to about 2 s. The player: it ends quickly, 2026-11-01.)
        t1, s2 = time.time(), 7
        while time.time() - t1 < 4.0 and s2 not in (0, 1):
            time.sleep(0.25)
            s2 = state()
        results.append(nh.report(s2 in (0, 1), f"attempt {attempt}: hand taken away: free again after {time.time() - t1:.1f} s ({s2})"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
