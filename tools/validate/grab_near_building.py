"""Scripted test: by a building the grip takes the villager or animal the hand is over, not the
building (playtest 2026-10-31: near buildings the hand took the building instead).

The game offers the picker only what its cursor ray (eye to the ground below the hand) hits: beside a
building that is the building, and a villager's own sphere is 4-9 cm. The mod now looks for people,
animals and trees within [hands] living_reach_cm (20) of the palm itself and puts them ahead.

Finds villagers and animals standing inside a building's sphere near the start view (not the
creche's: its children are inside it), the ones the game lets be picked up now. For each, the palm
goes 1.7 units (17 cm) above its middle: outside its own sphere's old touch range (10 cm + 5), inside
the building's. The touch is read ("pointerinfo"), then where the villager is by then: a trial counts
only if it was still within the range (they walk about). Each twice, with living_reach_cm 0 (the old
range) and 20 (the default).

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/grab_near_building.py [count]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld

OBJ = re.compile(r"object (\S+) \.\?AV(\w+)@@ at \(([-\d.]+), \+?([-\d.]+), ([-\d.]+)\)(.*?), sphere \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) r ([-\d.]+)")
START = (2231.5, 3147.4)  # (where the save starts: the village below)
SMALL = ("Villager", "SpecialVillager", "Pig", "Sheep", "Cow", "Horse", "Chicken", "Goat")


import os as _os
# Where the save's village is (the tests' default: Island 2's, by the player's save). Another save:
# BWVR_VILLAGE="x z" (Land 1's main village: "1500 2500").
VILLAGE = tuple(float(v) for v in _os.environ.get("BWVR_VILLAGE", "3250 3290").split())


def objects(x, z, radius):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x:.1f} {z:.1f} {radius}")
    time.sleep(1.0)
    out = {}
    for m in OBJ.finditer(ld.tail(mark)):
        out[m.group(1).lower().lstrip("0x")] = (m.group(2), (float(m.group(7)), float(m.group(8)), float(m.group(9))), float(m.group(10)),
                                                "liftable" in m.group(6))
    return out


def touched():
    mark = nh.LOG.stat().st_size
    ld.send("pointerinfo")
    time.sleep(0.4)
    m = re.search(r"pointer: touch (\d) object \S+ .*palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) \.\?AV(\w+)@@", ld.tail(mark))
    if not m:
        return "nothing", None
    palm = tuple(float(m.group(i)) for i in (2, 3, 4))
    return (m.group(5) if m.group(1) == "1" else "nothing"), palm


def trial(key, around, reach_units):
    """The palm above that villager or animal: what is touched ('small', a class, 'nothing'), or None
    if it walked out of range meanwhile."""
    near = objects(*around, 20)
    if key not in near:
        return None
    c = near[key][1]
    t = (c[0], c[1] + 1.7, c[2])  # (one move of the rig, the mock controller's offset known: quicker than place_palm)
    ld.send(f"rig set {t[0] - ld.PALM[0] * 10:.2f} {t[1] - ld.PALM[1] * 10:.2f} {t[2] - ld.PALM[2] * 10:.2f} 0")
    time.sleep(0.35)
    kind, palm = touched()
    after = objects(c[0], c[2], 10).get(key)
    if not palm or not after or math.dist(after[1], palm) > reach_units:
        return None
    return "small" if kind in SMALL else kind


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -40 0 0 0 0",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)
    objs = objects(*START, 200)
    buildings = [v for v in objs.values() if 3 < v[2] <= 20 and v[0] not in SMALL and v[0] != "Creche"]
    picks = []
    for k, (kind, c, r, lift) in objs.items():
        if kind in SMALL and lift:
            inside = [b for b in buildings if math.dist(c, b[1]) < b[2] - 2]
            if inside:
                picks.append((k, kind, (c[0], c[2]), inside[0][0]))
    picks = picks[:count]
    print(f"      {len(picks)} villagers/animals inside a building's sphere: {[(p[1], p[3]) for p in picks]}")
    tally = {}
    for reach in (0, 20):
        ld.send(f"hands living_reach {reach / 100}")
        got = [trial(k, around, 2.3) for _ in range(2) for k, _, around, _ in picks]
        tally[reach] = [g for g in got if g is not None]
        print(f"      living_reach {reach} cm: {tally[reach]} ({got.count(None)} walked off)")
    share = lambda xs: sum(1 for g in xs if g == "small") / max(1, len(xs))
    old, new = tally[0], tally[20]
    ok = len(new) >= 4 and share(new) >= 0.8 and share(new) > share(old)
    ld.send("hands living_reach 0.2", "ctrl r off")
    nh.report(ok, f"above a villager or animal by a building the touch is on it ({share(new):.0%} with 20 cm, {share(old):.0%} with the old range)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
