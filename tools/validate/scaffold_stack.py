"""Scripted test: a scaffold held over another joins it (player, 2026-10-30).

Runs on the player's autosave by the workshop (two scaffolds lying close together). Every release
of a scaffold held above the other one was "onto nothing" and built a blueprint instead.

Modes:
  over  (default) take one scaffold, hold it over the other at a few heights, let go: they combine.
  site  put one scaffold down on its blueprint (the game builds there), then take the other and
        let go of it over that building: the game applies it there (its wood to the building).

Usage: python tools/validate/scaffold_stack.py [over|site]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state
from scaffold_build import scaffolds_near, grab, blueprint, pose

HIGH = 12.0


def objects(x, z, r=15):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x:.1f} {z:.1f} {r}")
    time.sleep(1.0)
    return [l for l in ld.tail(mark).splitlines() if "hands: object" in l]


def sphere_of(addr, lines):
    for l in lines:
        if addr in l:
            m = re.search(r"sphere \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) r ([-\d.]+)", l)
            return tuple(float(m.group(i)) for i in range(1, 5)), " applies" in l
    return None, False


def over(results, held, target):
    if not grab(held):
        results.append(nh.report(False, f"the hand takes a scaffold ({held[0]})"))
        return
    pose(0, 0)
    sph, applies = sphere_of(target[0], objects(target[1], target[2]))
    print(f"   target sphere {sph}, the game would apply the held one to it: {applies}")
    results.append(nh.report(applies, "the game would apply the held scaffold to the other"))
    if not sph:
        return
    cx, cy, cz, r = sph
    touched = None
    # Held over it: straight above its sphere's top, and a little to one side, from high to low.
    for gap, side in ((8.0, 0.0), (5.0, 0.5), (3.0, 0.0), (1.5, 1.0)):
        mark = nh.LOG.stat().st_size
        ld.place_palm((cx + side, cy + r + gap, cz))
        time.sleep(1.0)
        if re.search(r"Scaffold@@ in the hand touches \.\?AVScaffold@@", ld.tail(mark)):
            touched = (gap, side)
            break
    results.append(nh.report(bool(touched), f"held over the other scaffold, the hand knows they can join ({touched} units above its sphere's top, to the side)"))
    released = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(3.0)
    t = ld.tail(released)
    let = re.search(r"let go of .*", t)
    print(f"   {let.group(0) if let else 'no release line'}")
    results.append(nh.report("scaffolds combine" in t, f"let go over it, they combine ({'Scaffold::Combine logged' if 'scaffolds combine' in t else 'no combine'})"))
    results.append(nh.report("BuildBuilding: 1" not in t, "... and nothing is built from a blueprint instead"))


def site(results, s1, s2):
    if not grab(s1):
        results.append(nh.report(False, f"the hand takes a scaffold ({s1[0]})"))
        return
    pose(-60, 0)
    spot = None
    for ang in range(0, 360, 45):
        for dist in (25.0, 40.0):
            x, z = s1[1] + dist * math.sin(math.radians(ang)), s1[2] + dist * math.cos(math.radians(ang))
            g = nh.ground_at(x, z)
            ld.place_palm((x, g + HIGH, z))
            mark = nh.LOG.stat().st_size
            ld.send("hands bp_log 2")
            time.sleep(1.6)
            b = blueprint(mark)
            if b and b[2]:
                spot = (x, g, z)
                break
        if spot:
            break
    results.append(nh.report(bool(spot), f"a blueprint shows ({spot})"))
    if not spot:
        return
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(4.0)
    built = re.search(r"a scaffold builds \(Scaffold::BuildBuilding: (\d+).*at map \(([-\d.]+), ([-\d.]+)\)", ld.tail(mark))
    results.append(nh.report(bool(built), f"put down on its blueprint, it builds ({built.group(0) if built else 'no build'})"))
    if not built:
        return
    bx, bz = float(built.group(2)), float(built.group(3))
    # (Quickly: the game joins a scaffold to one on a building site only until the building has
    # started, Scaffold::ValidToApplyThisToObject: the site's building's GetPercentBuilt 0.)
    pose(0, 0)
    if not grab(s2):
        results.append(nh.report(False, f"the hand takes the other scaffold ({s2[0]})"))
        return
    lines = objects(bx, bz, 3)
    for l in lines:
        print("   " + l.split("hands: ")[-1])
    cand = [l for l in lines if " applies" in l]
    results.append(nh.report(bool(cand), "something at the blueprint takes the held scaffold"))
    touched = None
    g = nh.ground_at(bx, bz)
    for gap in (6.0, 3.0):
        mark = nh.LOG.stat().st_size
        ld.place_palm((bx, g + gap, bz))
        time.sleep(0.3)
        m = re.search(r"Scaffold@@ in the hand touches (\S+)", ld.tail(mark))
        if m:
            touched = (gap, m.group(1))
            break
    results.append(nh.report(bool(touched), f"held over the building put down, the hand knows it goes onto it ({touched})"))
    released = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(3.0)
    t = ld.tail(released)
    for l in t.splitlines():
        if "let go" in l or "combine" in l or "builds" in l:
            print("   " + l.split("] ")[-1])
    results.append(nh.report("applied to it" in t, "let go over it, the game applies it there"))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "over"
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    pose(0, 0)
    time.sleep(1.0)
    rx, _, rz = nh.rig_pos()
    found = scaffolds_near(rx, rz, 80)
    print(f"   rig at ({rx:.1f}, {rz:.1f}); scaffolds: {[(a, round(x, 1), round(z, 1)) for a, x, z in found]}")
    if len(found) < 2:
        print("SKIP  fewer than two scaffolds near the start view")
        return 0
    pairs = sorted(((math.dist(a[1:], b[1:]), a, b) for i, a in enumerate(found) for b in found[i + 1:]), key=lambda p: p[0])
    _, a, b = pairs[0]
    (over if mode == "over" else site)(results, a, b)
    bwctl.stereo("ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
