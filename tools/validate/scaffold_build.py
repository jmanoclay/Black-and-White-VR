"""Scripted test: scaffolds in the hand at the controller (player, 2026-10-27).

Runs on the player's save of 2026-10-27 (scaffolds by the workshop, near the start view). Takes a
scaffold with the hand, holds it high over the land, turns the wrist and lets go gently; then takes
another and lets go of it touching a third (beside it, not above it).

Checks:
- the blueprint holds still by itself and turns with the wrist (clockwise for clockwise, seen from
  above, twice as far);
- let go gently over its blueprint, the scaffold builds there (Scaffold::BuildBuilding, at the
  blueprint), not where a fall from the hand would take it;
- let go touching another scaffold, the two combine (Scaffold::Combine).

Usage: python tools/validate/scaffold_build.py
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state

HIGH = 12.0  # units above the ground the scaffold is held (1.2 m: the player held it up in the air)


def scaffolds_near(x, z, r=60):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x:.1f} {z:.1f} {r}")
    time.sleep(1.0)
    found = []
    for m in re.finditer(r"hands: object (\S+) \.\?AVScaffold@@ at \(([-\d.]+), \+([-\d.]+), ([-\d.]+)\)", ld.tail(mark)):
        found.append((m.group(1), float(m.group(2)), float(m.group(4))))
    return found


def grab(sc):
    """The palm on the scaffold, the grip closed: True once the hand holds a scaffold."""
    g = nh.ground_at(sc[1], sc[2])
    for dy in (0.5, 1.0, 1.5, 2.5, 0.2):
        ld.place_palm((sc[1], g + dy, sc[2]))
        time.sleep(0.6)
        mark = nh.LOG.stat().st_size
        bwctl.stereo("ctrl r squeeze 1")
        time.sleep(1.2)
        if state() == 4 and ".?AVScaffold@@" in ld.tail(mark):
            return True
        bwctl.stereo("ctrl r squeeze 0")
        time.sleep(1.2)
    return False


def blueprint(mark):
    """The last blueprint line logged since `mark`: (angle, twist, valid, x axis, z axis, at)."""
    ms = list(re.finditer(r"hands: blueprint (valid|none) at angle ([-\d.]+) \(twist ([-\d.]+)\): its axes x \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) "
                          r"z \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", ld.tail(mark)))
    if not ms:
        return None
    m = ms[-1]
    f = [float(m.group(i)) for i in range(2, 13)]
    return f[0], f[1], m.group(1) == "valid", f[2:5], f[5:8], f[8:11]


def pose(pitch, roll):
    bwctl.stereo(f"ctrl r pose 0 {pitch} {roll} {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    pose(0, 0)
    time.sleep(1.0)
    rx, _, rz = nh.rig_pos()
    found = scaffolds_near(rx, rz, 80)
    print(f"   rig at ({rx:.1f}, {rz:.1f}); scaffolds: {[(a, round(x, 1), round(z, 1)) for a, x, z in found]}")
    if len(found) < 3:
        print("SKIP  fewer than three scaffolds near the start view")
        return 0
    # (The two lying nearest each other are for combining; the build uses another.)
    pairs = sorted(((math.dist(a[1:], b[1:]), a, b) for i, a in enumerate(found) for b in found[i + 1:]), key=lambda p: p[0])
    _, s2, s3 = pairs[0]
    s1 = next(s for s in found if s not in (s2, s3))

    # --- The blueprint, turned by the wrist, and the build where it shows.
    if not grab(s1):
        results.append(nh.report(False, f"the hand takes a scaffold ({s1[0]})"))
        return 1
    print(f"   holding scaffold {s1[0]}")
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
    results.append(nh.report(bool(spot), f"held up high over the land, the scaffold shows a blueprint ({spot})"))
    if not spot:
        return 1
    mark = nh.LOG.stat().st_size
    ld.send("hands bp_log 2")
    time.sleep(1.2)
    a0 = blueprint(mark)
    time.sleep(2.0)
    mark = nh.LOG.stat().st_size
    ld.send("hands bp_log 2")
    time.sleep(1.2)
    a1 = blueprint(mark)
    still = a0 and a1 and abs(a1[0] - a0[0]) < 0.02
    results.append(nh.report(bool(still), f"by itself the blueprint holds still: angle {a0 and a0[0]} then {a1 and a1[0]} 2 s later"))
    for roll in (10, 20, 30):  # (a clockwise twist, as seen from behind the hand: 30 degrees)
        pose(-60, roll)
        time.sleep(0.3)
    mark = nh.LOG.stat().st_size
    ld.send("hands bp_log 2")
    time.sleep(1.2)
    a2 = blueprint(mark)
    if a1 and a2:
        d = (a2[0] - a1[0] + math.pi) % (2 * math.pi) - math.pi
        # Seen from above, clockwise: the blueprint's z axis turns toward +x (engine axes: +x right, +z ahead).
        h1, h2 = math.atan2(a1[4][0], a1[4][2]), math.atan2(a2[4][0], a2[4][2])
        dh = (h2 - h1 + math.pi) % (2 * math.pi) - math.pi
        results.append(nh.report(abs(abs(d) - math.radians(60)) < 0.1,
                                 f"a 30 degree twist turns the blueprint 60 degrees: angle {a1[0]:.2f} -> {a2[0]:.2f} ({math.degrees(d):.0f} degrees)"))
        # (The mock's roll and a real controller's twist differ in sign here: with the blueprint turning
        # the way the wrist does in the headset (player, 2026-10-27), it turns against the mock's roll.)
        results.append(nh.report(dh < -0.5, f"... the way a real wrist turns (against the mock's roll): its heading {math.degrees(h1):.0f} -> {math.degrees(h2):.0f} degrees"))
    else:
        results.append(nh.report(False, "the blueprint's angle was logged"))
    bp_at = a2[5] if a2 else None
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(4.0)
    t = ld.tail(mark)
    let = re.search(r"let go of a scaffold over its blueprint", t)
    built = re.search(r"a scaffold builds \(Scaffold::BuildBuilding: (\d+), force \d+\) at map \(([-\d.]+), ([-\d.]+)\), angle ([-\d.]+)", t)
    results.append(nh.report(bool(let), "let go gently, the game puts the scaffold down at its hand (the blueprint)"))
    ok = bool(built) and built.group(1) != "0"
    near = ok and bp_at and math.dist((float(built.group(2)), float(built.group(3))), (bp_at[0], bp_at[2])) < 3.0
    results.append(nh.report(bool(near), f"it builds at the blueprint: {built.group(0) if built else 'no build'}; blueprint at {bp_at}"))
    if ok and a2:
        results.append(nh.report(abs(float(built.group(4)) - a2[0]) < 0.05, f"... at the blueprint's angle ({built.group(4)}, blueprint {a2[0]:.2f})"))

    # --- Combining: a scaffold let go of touching another (beside it, not above it).
    pose(0, 0)
    time.sleep(0.5)
    if not grab(s2):
        results.append(nh.report(False, f"the hand takes a second scaffold ({s2[0]})"))
        return 1
    g3 = nh.ground_at(s3[1], s3[2])
    joined = False
    for side in (1.2, 0.8, 1.6, 0.4):
        mark = nh.LOG.stat().st_size
        ld.place_palm((s3[1] + side, g3 + 1.0, s3[2]))
        time.sleep(1.0)
        if re.search(r"Scaffold@@ in the hand touches \.\?AVScaffold@@", ld.tail(mark)):
            joined = True
            print(f"   touching the other scaffold with the palm {side} units to its side")
            break
    results.append(nh.report(joined, "the scaffold in the hand touching another, the hand knows they can join"))
    released = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(3.0)
    t = ld.tail(released)
    results.append(nh.report("scaffolds combine" in t, f"let go, they combine ({'Scaffold::Combine logged' if 'scaffolds combine' in t else 'no combine'}; hand state {state()})"))
    # (The hand is empty then: no click of its own may follow, it would take hold of what is under it.)
    stray = re.search(r"game hand state 1 -> [23]|leash: the creature is sent", t)
    results.append(nh.report(not stray, f"nothing else happens as the hand lets go ({stray.group(0) if stray else 'no click after'})"))
    # The one left has both in it: the empty hand on it, the game says its value. (A hint is logged
    # as the game first asks for it: right after the join, the palm still on it.)
    ld.place_palm((s3[1], g3 + 1.0, s3[2]))
    time.sleep(2.5)
    v = re.findall(r"hint 'Scaffold Value (\d+)'", ld.tail(released))
    results.append(nh.report("2" in v, f"the scaffold left is worth two (the game's hint on it: {v})"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
