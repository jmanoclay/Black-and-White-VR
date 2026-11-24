"""Probe: the game hand's interaction fields while the palm rests on, then rubs, the player's
creature (why the stroking state doesn't always start).

Usage: python tools/validate/pet_probe.py
"""
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from give_creature import own_creature

PALM = (0.0, -0.3, 0.5)


def main():
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", "ctrl r pose 0 0 0 0.6 0.3 0.5")
    time.sleep(1)
    c = own_creature()
    print("creature", c)
    for dy in (0.0, 0.0):  # (twice at the centre)
        bwctl.stereo(f"rig set {c[0]:.2f} {c[1] + 3 - dy * 10:.2f} {c[2] - 5:.2f} 0")
        time.sleep(0.3)
        mark = nh.LOG.stat().st_size
        for k in range(24):
            bwctl.stereo(f"ctrl r pose 0 0 0 {PALM[0] + (0.03 if k % 2 else -0.03)} {PALM[1]} {PALM[2]}")
            time.sleep(0.15)
            bwctl.stereo("hands hand_fields")
            time.sleep(0.1)
        t = ld.tail(mark)
        print(f"dy {dy}: state {nh.hand()[0]}")
        for line in t.splitlines():
            if "interface state" in line or "petting" in line or "camera mode" in line or "state" in line and "->" in line:
                print("  ", line[:200])
        bwctl.stereo("ctrl r pose 0 0 0 0.6 0.3 0.5")
        time.sleep(1.5)
    bwctl.stereo("ctrl r off")


if __name__ == "__main__":
    sys.exit(main())
