"""Probe: which of the tutorial's bubbles is the shield, and how the game casts it (the action
button held, a circle drawn on the land with the laser).

Usage: python tools/validate/shield_probe.py
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state, picked_class


def seeds_near(x, z, r=200):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x} {z} {r}")
    time.sleep(1.5)
    out = {}
    for m in re.finditer(r"hands: object (\S+) \.\?AVOneOffSpellSeed@@ at \(([-\d.]+), \+([-\d.]+), ([-\d.]+)\)", ld.tail(mark)):
        out[m.group(1)] = (float(m.group(2)), float(m.group(4)))
    return out


def pop(x, z):
    """Laser on the bubble, a trigger tap: True once the hand holds the charged miracle (state 8)."""
    g = nh.ground_at(x, z)
    ld.send("hands laser 1", f"rig set {x} {g + 45:.1f} {z - 18} 0", "pose 0 -65 0 0 0 0")
    time.sleep(1.5)
    for dy in (1.0, 2.5, 5.0, 9.0, 13.0, 18.0, 0.3):
        bwctl.stereo(f"ctrl r aimat {x} {g + dy:.1f} {z}")
        time.sleep(0.4)
        if picked_class() == ".?AVOneOffSpellSeed@@":
            bwctl.stereo("ctrl r trigger 1")
            time.sleep(0.12)
            bwctl.stereo("ctrl r trigger 0")
            time.sleep(1.5)
            return state() == 8
    return False


def laser_circle(cx, cz, r=15.0, steps=20, turns=1.15):
    g = nh.ground_at(cx, cz)
    ld.send("hands laser 1", f"rig set {cx} {g + 50:.1f} {cz - 45} 0", "pose 0 -45 0 0 0 0")
    time.sleep(1.2)
    bwctl.stereo(f"ctrl r aimat {cx:.1f} {g:.1f} {cz + r:.1f}")
    time.sleep(0.5)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.3)
    for k in range(int(steps * turns) + 1):
        a = 2 * math.pi * k / steps
        x, z = cx + r * math.sin(a), cz + r * math.cos(a)
        bwctl.stereo(f"ctrl r aimat {x:.1f} {nh.ground_at(x, z) if False else g:.1f} {z:.1f}")
        time.sleep(0.16)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(2.0)


def main():
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off")
    rx, _, rz = nh.rig_pos()
    seeds = seeds_near(rx, rz)
    print("   bubbles:", seeds)
    for addr, (x, z) in sorted(seeds.items(), key=lambda kv: kv[1]):
        mark = nh.LOG.stat().st_size
        if not pop(x, z):
            print(f"   {addr}: not popped (state {state()})")
            continue
        m2 = nh.LOG.stat().st_size
        ld.send("hands seed")
        time.sleep(0.6)
        k = re.search(r"hands: seed: (.*)", ld.tail(m2))
        print(f"   {addr} at ({x:.0f}, {z:.0f}): {k.group(1) if k else '?'}")
        m3 = nh.LOG.stat().st_size
        laser_circle(x + 25, z + 10)
        made = re.findall(r"spell made .*?spell [0-9A-F]+ ([^;]+);", ld.tail(m3))
        print(f"      after a laser circle with the trigger held: state {state()}, spells made {made}")
        ld.send("hands laser 0")
        bwctl.stereo("ctrl r pose 0 0 0 0 -0.30 0.55")
        time.sleep(0.5)
        for i in range(12):  # (shaken off)
            bwctl.stereo(f"ctrl r pose 0 0 0 {0.15 if i % 2 else -0.15} -0.30 0.55")
            time.sleep(0.16)
        time.sleep(1.5)
        print(f"      shaken off: state {state()}")
    print("SKIP  (probe)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
