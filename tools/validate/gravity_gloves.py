"""The gravity gloves (player, 2026-11-05: "mimic the Half-Life: Alyx gravity gloves").

From 3.5 m up (the teleport's bird's-eye height) and about 9 m off, with an empty hand at the
controller:
  1. the controller aimed at a villager: it is the gloves' target ("aiming");
  2. the grip: locked on;
  3. a flick up (the mock controller moving up at 2 m/s for 0.15 s): pulled ("pulling", then
     "flying"), and the hand holds the villager (hand state 4, the game's held object it);
  4. the grip let go: it drops.
Pictures of each step in the captures (gravity_*).

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot), on the player's Island 2 save.
Usage: python tools/validate/gravity_gloves.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
import grab_near_building as gb

HAND = (0.2, -0.3, 0.3)


def states(mark):
    return re.findall(r"gravity gloves: (\w[\w ]*?) -> (\w[\w ]*?) \((\S+) ", ld.tail(mark))


def holding():
    (m,) = nh.query("handinfo", [r"handinfo: holding (\d)"])
    return m.group(1) == "1"


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl l off",
            "ctrl r squeeze 0", "ctrl r trigger 0", "pose 0 -25 0 0 0 0", f"ctrl r pose 0 0 0 {HAND[0]} {HAND[1]} {HAND[2]}")
    time.sleep(1.0)
    objs = gb.objects(*gb.VILLAGE, 250)
    vill = [(k, v) for k, v in objs.items() if v[0] == "Villager" and v[3]]
    if not vill:
        print("SKIP  no villager to pull")
        return 0
    for key, (kind, c, r, lift) in vill[:5]:
        now = gb.objects(c[0], c[2], 10).get(key)
        if not now:
            continue
        c = now[1]
        x, z = c[0] - 90.0, c[2]  # (9 m off: people are offered to about 13 m)
        g = max(0.0, nh.ground_at(x, z))
        ld.send(f"rig set {x:.1f} {g + 35:.1f} {z:.1f} 90")  # (facing +x, toward the villager)
        time.sleep(0.8)
        now = gb.objects(c[0], c[2], 10).get(key)
        if not now:
            continue
        c = now[1]
        mark = nh.LOG.stat().st_size
        ld.send(f"ctrl r aimat {c[0]:.1f} {c[1]:.1f} {c[2]:.1f}")
        time.sleep(0.8)
        st = states(mark)
        text = ld.tail(mark)
        aimed = bool(re.search(r"gravity gloves: (idle -> aiming \(|aiming at )\.\?AVVillager@@", text))
        if not aimed:
            print(f"      villager {key}: not aimed at ({st[-3:]})")
            continue
        results.append(nh.report(True, f"aimed at a villager {abs(c[0] - x) / 10:.0f} m off: the gloves' target"))
        def follow():  # (villagers walk: aimed again where it is now, as a player keeps pointing)
            cur = gb.objects(c[0], c[2], 15).get(key)
            if cur:
                ld.send(f"ctrl r aimat {cur[1][0]:.1f} {cur[1][1]:.1f} {cur[1][2]:.1f}")
        bwctl.stereo_capture("gravity_aim")
        follow()
        time.sleep(0.2)
        bwctl.stereo("ctrl r squeeze 1")
        time.sleep(0.5)
        st = states(mark)
        results.append(nh.report(any(b == "locked on" for a, b, k in st), "the grip: locked on"))
        bwctl.stereo_capture("gravity_locked")
        bwctl.stereo("ctrl r vel 0 2 0")
        time.sleep(0.15)
        bwctl.stereo("ctrl r vel 0 0 0")
        time.sleep(0.12)
        bwctl.stereo_capture("gravity_flying")
        time.sleep(0.8)
        st = states(mark)
        pulled = any(b == "flying" for a, b, k in st)
        results.append(nh.report(pulled and holding(), f"a flick up: pulled into the hand ({[b for a, b, k in st]}, holding {holding()})"))
        bwctl.stereo_capture("gravity_held")
        bwctl.stereo("ctrl r squeeze 0")
        time.sleep(1.0)
        results.append(nh.report(not holding(), "the grip let go: dropped"))
        break
    else:
        results.append(nh.report(False, "no villager could be aimed at"))
    ld.send("ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
