"""Scripted test of the game's info line by the hand (player, 2026-10-30: the scaffolds' "wood
needed" not seen in VR).

The flat game shows a second line under the hand's action about the thing under the hand: "Wood
Required: 7" over a scaffold, "Building Completed: 42%" over a building going up, a town's
population, a miracle's name. The mod drew only the action ("Pick Up"). The palm is held over each
of a few things near the start view; their line must be asked for and drawn by the hand
(`ui textlog`: the mod's own text with those words). A picture of each: info_hints_<kind>_L.bmp.

Boot first (with Windows audio stalled: BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/info_hints.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
import leash_direct as ld

# The info lines the game may give each kind (the save moves on: a scaffold at 0% asks for
# "Production Completed: 0%", a finished house its population; 2026-11-01).
KINDS = {"Scaffold": "Wood Required|Production Completed|Building Completed|Scaffold Value",
         "Abode": "Building Completed|Production Completed|Population|Wood Required",
         "OneOffSpellSeed": "Miracle"}


def main():
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
            "ctrl r pose 0 0 0 0 -0.3 0.5", "pose 0 -30 0 0 0 0", "ui info_hints 1")
    time.sleep(1.5)
    rx, _, rz = nh.rig_pos()
    # (No miracle bubble near the start any more: one made 20 units ahead, food.)
    ld.send(f"hands spawn_bubble 3 {rx:.1f} {rz + 20:.1f}")
    time.sleep(1.5)
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {rx:.1f} {rz:.1f} 150")
    time.sleep(1.2)
    found = {}
    for m in re.finditer(r"object (\S+) \.\?AV(\w+)@@ at \(([-\d.]+), \+?[-\d.]+, ([-\d.]+)\)", ld.tail(mark)):
        found.setdefault(m.group(2), []).append((float(m.group(3)), float(m.group(4))))
    results = []
    for kind, words in KINDS.items():
        shown, seen = None, set()
        for x, z in found.get(kind, [])[:12]:
            g = nh.ground_at(x, z)
            mark = nh.LOG.stat().st_size
            ld.place_palm((x, g + 4, z))
            time.sleep(1.8)
            ld.send("ui textlog 40")
            time.sleep(0.5)
            t = ld.tail(mark)
            seen.update(re.findall(r"ui: hint '([^']*)' asked for", t))
            asked = re.search(r"ui: hint '([^']*(?:" + words + r")[^']*)' asked for", t)
            if asked:
                shown = asked.group(1)
                bwctl.stereo_capture(f"info_hints_{kind}")
                break
        if kind not in found:
            print(f"      no {kind} near the start view")
            continue
        if not shown and seen and not any(":" in w or "%" in w for w in seen):
            # (The hand was over them (the game asked for its action words) but the game had no info
            # line for them: on the Island 2 save, 2026-11-03, only "Tap" over the houses there.)
            print(f"SKIP  over a {kind}: the game asked only for {sorted(seen)}, no info line")
            continue
        results.append(nh.report(bool(shown), f"over a {kind}: '{shown or words}' asked for by the hand"))
    ld.send("ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
