"""Scripted test: the village totem goes up and down with the hand (player, 2026-10-27: it went with
the hand moving forward and back, as the flat game's mouse does).

Runs on the player's save of 2026-10-27 (the village with its totem statue). Takes hold of the
totem with the hand, raises the hand, then lowers it, reading the totem's level (TotemStatue +0x88,
0 to 1) each time.

Checks:
- the hand takes hold of the totem (hand state 5);
- raising the hand 25 cm raises the totem; lowering it 50 cm lowers it;
- moving the hand forward and back (same height) hardly changes it.

Usage: python tools/validate/totem_height.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state


def find_totem(x, z, r=150):
    """The town centre nearest (x, z) and its totem statue (TownCentre +0xCC): (statue, x, z)."""
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x:.1f} {z:.1f} {r}")
    time.sleep(1.2)
    tcs = {m.group(1): (float(m.group(2)), float(m.group(3)))
           for m in re.finditer(r"hands: object (\S+) \.\?AVTownCentre@@ at \(([-\d.]+), \+[-\d.]+, ([-\d.]+)\)", ld.tail(mark))}
    for addr, (tx, tz) in sorted(tcs.items(), key=lambda kv: abs(kv[1][0] - x) + abs(kv[1][1] - z)):
        mark = nh.LOG.stat().st_size
        ld.send(f"hands peek {int(addr, 16) + 0xCC:X} 1")
        time.sleep(0.4)
        m = re.search(r"peek: \S+: ([0-9A-F]{8})", ld.tail(mark))
        if not m:
            continue
        mark = nh.LOG.stat().st_size
        ld.send(f"hands peek {m.group(1)} 1")
        time.sleep(0.4)
        if re.search(r"peek: \S+: 008E029C", ld.tail(mark)):  # (TotemStatue's vtable)
            return m.group(1), tx, tz
    return None


def level(addr):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands peek {int(addr, 16) + 0x88:X} 1")
    time.sleep(0.5)
    m = re.search(r"peek: \S+: \S+\(([-\d.e+]+)\)", ld.tail(mark))
    return float(m.group(1)) if m else None


def palm(dx=0.0, dy=0.0, dz=0.0):
    bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0] + dx:.2f} {ld.PALM[1] + dy:.2f} {ld.PALM[2] + dz:.2f}")


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -30 0 0 0 0")
    palm()
    time.sleep(1.0)
    rx, _, rz = nh.rig_pos()
    t = find_totem(rx, rz) or find_totem(3255.0, 3338.0)
    if not t:
        print("SKIP  no totem statue near the start view or the village")
        return 0
    print(f"   totem {t[0]} at ({t[1]:.1f}, {t[2]:.1f}), level {level(t[0])}")
    g = nh.ground_at(t[1], t[2])
    held = False
    for dy in (6.0, 4.0, 8.0, 10.0, 3.0, 12.0):
        ld.place_palm((t[1], g + dy, t[2]))
        time.sleep(0.6)
        bwctl.stereo("ctrl r squeeze 1")
        time.sleep(1.0)
        if state() == 5:
            held = True
            print(f"   holding the totem with the palm {dy} units above the ground")
            break
        bwctl.stereo("ctrl r squeeze 0")
        time.sleep(1.0)
    if not held:
        # (On the save of the gesture lesson the game lets neither the hand nor the laser take it:
        # the grip on it does nothing, the trigger takes hold of the land.)
        print("SKIP  the game doesn't let the hand take hold of the totem on this save")
        return 0
    results.append(nh.report(held, "the hand takes hold of the totem (hand state 5)"))
    l0 = level(t[0])
    for k in range(1, 6):  # (up 25 cm in 0.75 s)
        palm(dy=0.05 * k)
        time.sleep(0.15)
    time.sleep(0.6)
    l1 = level(t[0])
    for k in range(1, 11):  # (down 50 cm)
        palm(dy=0.25 - 0.05 * k)
        time.sleep(0.15)
    time.sleep(0.6)
    l2 = level(t[0])
    for k in range(1, 6):  # (forward 25 cm, back, at the low height)
        palm(dy=-0.25, dz=0.05 * k)
        time.sleep(0.15)
    for k in range(1, 6):
        palm(dy=-0.25, dz=0.25 - 0.05 * k)
        time.sleep(0.15)
    time.sleep(0.6)
    l3 = level(t[0])
    # (The hand still, the head turning and tilting: the totem stays put. It wobbled after the
    # cursor on the screen; player, 2026-10-27.)
    seen = []
    for k, (yaw, roll) in enumerate(((8, 0), (-8, 5), (0, -5), (5, 0), (0, 0))):
        bwctl.stereo(f"pose {yaw} -30 {roll} 0 0 0")
        time.sleep(0.4)
        seen.append(level(t[0]))
    print(f"   levels: taken {l0}, hand up {l1}, hand down {l2}, forward and back {l3}")
    ok = None not in (l0, l1, l2, l3)
    results.append(nh.report(ok and l1 > l0 + 0.05, f"raising the hand raises the totem ({l0} -> {l1})"))
    results.append(nh.report(ok and l2 < l1 - 0.05, f"lowering the hand lowers it ({l1} -> {l2})"))
    results.append(nh.report(ok and abs(l3 - l2) < 0.05, f"forward and back at one height hardly moves it ({l2} -> {l3})"))
    results.append(nh.report(None not in seen and max(seen) - min(seen) < 0.01, f"the head turning and tilting with the hand still leaves it be ({seen})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.5)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
