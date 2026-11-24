"""Scripted test: a charged miracle thrown from the hand (a fireball) leaves from the VR palm, not
from the game's hand on the land below it (player, 2026-10-24).

Spawns a fire bubble at the spot (hands spawn_bubble 2), pops it with the palm, raises the hand, swings it with the trigger held and lets go.

Checks:
- the bubble pops (hand state 8);
- the throw goes through the mod (logged) and leaves the hand (hand state back to free);
- the thrown miracle starts at the palm's height, not the ground's.

Usage: python tools/validate/fireball_throw.py [x z]
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import picked_class, state


def main():
    cx, cz = (float(sys.argv[1]), float(sys.argv[2])) if len(sys.argv) > 2 else (3242.0, 3164.0)
    results = []
    pts = sorted([(cx + dx, cz + dz) for dz in range(-30, 31, 6) for dx in range(-30, 31, 6)], key=lambda q: abs(q[0] - cx) + abs(q[1] - cz))
    gs = {p: nh.ground_at(*p) for p in pts}
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", f"rig set {cx} {max(gs.values()) + 50:.1f} {cz - 50} 0", "pose 0 -45 0 0 0 0")
    time.sleep(1.5)
    # A fire bubble spawned at the spot (hands spawn_bubble 2), so no save needs one nearby.
    ld.send(f"hands spawn_bubble 2 {cx} {cz}")
    time.sleep(1.5)
    bubble = (cx, nh.ground_at(cx, cz) + 2.0, cz)
    print(f"   bubble at {bubble}")
    bwctl.stereo("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    for dy in (0.0, 1.0, -1.0, 2.0, 3.0, 4.0):
        ld.place_palm((bubble[0], bubble[1] + dy, bubble[2]))
        time.sleep(0.8)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.12)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if state() == 8:
            break
    results.append(nh.report(state() == 8, f"the trigger pops the bubble (hand state {state()})"))
    if state() != 8:
        return 1
    g = nh.ground_at(bubble[0], bubble[2])
    high = float(sys.argv[3]) if len(sys.argv) > 3 else 100.0  # (the player throws from high up)
    palm = [bubble[0], g + high, bubble[2] - 6]
    ld.place_palm(tuple(palm))
    time.sleep(1.0)
    mark = nh.LOG.stat().st_size
    bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} 0.2")
    time.sleep(0.6)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.5)
    # A swing forward and up: 0.75 m in 0.6 s. (Requests closer than ~0.15 s apart get lost: the
    # release was, more than once.)
    for k in range(1, 4):
        bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1] + 0.05 * k:.2f} {0.2 + 0.25 * k:.2f}")
        time.sleep(0.2)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(0.25)
    if state() == 8:
        print("   (the release was lost: sent again)")
        bwctl.stereo("ctrl r trigger 0")
    time.sleep(0.2)
    bwctl.stereo("pose 60 -45 0 0 0 0")  # (looking off to the side: the fireball must not follow the look)
    for k in range(4):  # (each capture takes about 0.4 s)
        bwctl.stereo_capture(f"fireball_s{k}")
    bwctl.stereo_capture("fireball_03")
    time.sleep(0.6)
    bwctl.stereo_capture("fireball_12")
    time.sleep(1.0)
    t = ld.tail(mark)
    k = re.search(r"spell made by \S+ -> \d+( \(starts at the palm\))?: .*?spell \S+ (\S+); points in it: \+64 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) \+70 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)
    results.append(nh.report(bool(k) and state() != 8, f"the miracle is cast and leaves the hand (state {state()}; {k.group(0)[:200] if k else 'no spell made'})"))
    if k:
        sy = float(k.group(7))
        results.append(nh.report(bool(k.group(1)) and sy - g > high * 0.7, f"the spell starts at the palm's height: y {sy:.1f} (ground {g:.1f})"))
    # (The spell follows the game's hand point until it flies: it must stay up at the palm.)
    ys = [float(m.group(2)) for m in re.finditer(r"hands: spell \S+ at ([\d.]+) s:.*? \+70 \([-\d.]+, ([-\d.]+), [-\d.]+\)", t)
          if float(m.group(1)) < 0.45]
    results.append(nh.report(bool(ys) and min(ys) - g > high * 0.7, f"in its first 0.4 s the spell stays up at the hand: heights {[round(y, 1) for y in ys]} (ground {g:.1f})"))
    # The fireball itself (a particle atom) leaves from the palm and flies along the swing (+z here).
    time.sleep(2.0)
    t = ld.tail(mark)
    made = re.search(r"a fireball made \S+ at map .*?its atom \S+ at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)
    track = [(float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)))
             for m in re.finditer(r"hands: fireball \S+ at ([\d.]+) s: object .*?atom \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", t)]
    if made and track:
        z0, y0 = float(made.group(3)), float(made.group(2))
        late = [p for p in track if 0.5 <= p[0] <= 0.7]
        dz = (late[0][3] - z0) if late else 0.0
        results.append(nh.report(y0 - g > high * 0.7 and dz > 10.0, f"the fireball starts at the palm (y {y0:.1f}, ground {g:.1f}) and flies along the swing ({dz:.1f} units forward in 0.6 s)"))
    else:
        results.append(nh.report(False, "the fireball was made and tracked (no fireball logged)"))
    # It keeps the throw's heading (along +z): it doesn't curve toward where the head looks.
    if len(track) >= 8:
        early = [p for p in track if p[0] <= 0.35]
        late = [p for p in track if p[0] >= 1.0]
        if early and late:
            dx = (late[-1][1] - early[0][1]) / max(1e-3, late[-1][3] - early[0][3])
            results.append(nh.report(abs(dx) < 0.15, f"the fireball keeps the throw's heading while the head looks aside (sideways {dx:.2f} per unit forward)"))

    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
