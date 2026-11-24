"""Gestures drawn with the controller (player, 2026-10-27: the miracle tutorial's gesture couldn't be
drawn). The game reads gestures from the hand's free movement over the land (no button); in VR it
emptied the samples every turn because its camera (the head) always moves.

Runs on the player's save of 2026-10-27 (the gesture lesson). Laser mode: spirals drawn with the
laser on the land; `hands gesture_log` logs the game's samples, their emptying and the actions it
recognises.

Checks: an inward clockwise spiral drawn with the palm (hand mode, no button) opens the game's
Miracle selection (action 1), and it stays open: the spiral's reversals were taken for a shake,
which cancelled it (player, 2026-10-27). With --laser, drawn with the laser instead.

Usage: python tools/validate/gesture_probe.py [--all]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl


G = 0.0
RATE = 25


def circle(cx, cz, r, turns=1.5, steps=18):
    for k in range(int(steps * turns) + 1):
        a = 2 * math.pi * k / steps
        x, z = cx + r * math.sin(a), cz + r * math.cos(a)
        bwctl.stereo(f"ctrl r aimat {x:.1f} {G:.1f} {z:.1f}")
        time.sleep(0.16)


def spiral(cx, cz, shape, turns=2.5, steps=16):
    n = int(steps * turns)
    for k in range(n + 1):
        f = k / n
        r = (14.0 - 12.0 * f) if shape.startswith("in") else (2.0 + 12.0 * f)
        a = (1 if shape.endswith(" cw") else -1) * 2 * math.pi * k / steps
        x, z = cx + r * math.sin(a), cz + r * math.cos(a)
        bwctl.stereo(f"ctrl r aimat {x:.1f} {G:.1f} {z:.1f}")
        time.sleep(0.12)


def hand_spiral(shape="in cw"):
    for k in range(41):
        f = k / 40
        r = 0.20 - 0.17 * f
        a = (1 if shape.endswith(" cw") else -1) * 2 * math.pi * k / 16
        bwctl.stereo(f"ctrl r pose 0 0 0 {r * math.sin(a):.3f} {-0.30 + r * math.cos(a):.3f} 0.55")
        time.sleep(0.12)


def smooth_spiral(T=1.8, turns=2.5, shape="in cw", hold_b=True, cy=-0.30):
    """A spiral drawn with the palm moving every frame, as a real hand does (the mod moves the mock
    controller along it: ctrl r spiral)."""
    sgn = 1 if shape.endswith(" cw") else -1
    r0, r1 = 0.20, 0.03
    bwctl.stereo(f"ctrl r pose 0 0 0 0.000 {cy + r0:.3f} 0.55")
    time.sleep(0.3)
    if hold_b:
        bwctl.stereo("ctrl r b 1")
        time.sleep(0.15)
    bwctl.stereo(f"ctrl r spiral {T} {turns} {r0} {r1} {'ccw' if sgn < 0 else 'cw'}")
    time.sleep(T + 0.1)
    bwctl.stereo("ctrl r vel 0 0 0")
    if hold_b:
        time.sleep(0.15)
        bwctl.stereo("ctrl r b 0")


def main():
    if "--jerky" not in sys.argv:  # (the default: smooth, with B held; --jerky: the old palm spiral without B)
        ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
                "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -40 0 0 0 0")
        time.sleep(1.0)
        ld.send(f"hands gesture_rate {RATE}")
        results = []
        # Without B: nothing is drawn (the hand's own motion doesn't count).
        mark = nh.LOG.stat().st_size
        smooth_spiral(hold_b=False)
        time.sleep(1.5)
        results.append(nh.report("Miracle selection open" not in ld.tail(mark), "a spiral without B held does nothing"))
        for k in range(3):
            mark = nh.LOG.stat().st_size
            ld.send("hands gesture_log 12")
            smooth_spiral()
            time.sleep(1.5)
            t = ld.tail(mark)
            opened = "Miracle selection open" in t
            print(f"   samples: {max([int(b) for a, b in re.findall(r'samples (-?[0-9]+) -> (-?[0-9]+)', t)] or [-1])} at most")
            results.append(nh.report(opened, f"a spiral drawn smoothly over 1.8 s with B held opens Miracle selection (try {k + 1})"))
            if opened:  # (closed again: the game's own shake)
                for i in range(10):
                    bwctl.stereo(f"ctrl r pose 0 0 0 {0.15 if i % 2 else -0.15} -0.30 0.55")
                    time.sleep(0.16)
                ld.send(f"hands gesture_rate {RATE}")
                bwctl.stereo("ctrl r b 1")
                time.sleep(0.2)
                for i in range(10):  # (the game's shake, drawn with B)
                    bwctl.stereo(f"ctrl r pose 0 0 0 {0.15 if i % 2 else -0.15} -0.30 0.55")
                    time.sleep(0.16)
                bwctl.stereo("ctrl r b 0")
                time.sleep(1.5)
        # Wherever the player looks: drawn in front of the face, looking out level and up into the
        # sky (the game took no samples with no land behind the pen; player, 2026-11-03).
        for name, pose, cy in (("looking straight ahead, the hand at eye level", "pose 0 0 0 0 0 0", 0.0),
                               ("looking up 30 degrees, the hand raised (sky behind)", "pose 0 30 0 0 0 0", 0.30)):
            ld.send(pose)
            time.sleep(0.8)
            mark = nh.LOG.stat().st_size
            smooth_spiral(cy=cy)
            time.sleep(1.5)
            opened = "Miracle selection open" in ld.tail(mark)
            results.append(nh.report(opened, f"{name}: the spiral opens Miracle selection"))
            if opened:
                bwctl.stereo("ctrl r b 1")
                time.sleep(0.2)
                for i in range(10):
                    bwctl.stereo(f"ctrl r pose 0 0 0 {0.15 if i % 2 else -0.15} {cy:.2f} 0.55")
                    time.sleep(0.16)
                bwctl.stereo("ctrl r b 0")
                time.sleep(1.5)
        ld.send("pose 0 -40 0 0 0 0")
        return 0 if all(results) else 1
    if "--laser" not in sys.argv:
        ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
                "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -40 0 0 0 0")
        time.sleep(1.0)
        mark = nh.LOG.stat().st_size
        ld.send("hands gesture_log 12")
        hand_spiral()
        time.sleep(3.0)
        t = ld.tail(mark)
        opened = "Miracle selection open" in t
        closed = "Miracle selection closed" in t
        shakes = len(re.findall(r"shake given to the game", t))
        print(f"   recognised {sorted(set(re.findall(r'recognises action (\d+)', t)))}, shakes given {shakes}")
        r1 = nh.report(opened, "a spiral drawn with the palm opens Miracle selection")
        r2 = nh.report(opened and not closed and shakes == 0, f"... and it stays open 3 s later (closed: {closed}, shakes given to the game: {shakes})")
        return 0 if r1 and r2 else 1
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -40 0 0 0 0")
    time.sleep(1.0)
    rx, ry, rz = nh.rig_pos()
    cx, cz = rx, rz + 40
    g = nh.ground_at(cx, cz)
    ld.send(f"rig set {rx:.1f} {ry:.1f} {rz:.1f} 0")
    time.sleep(1.0)
    global G
    G = g
    print(f"   circles around ({cx:.0f}, {g:.0f}, {cz:.0f})")
    shapes = [("no button, in cw", None, None, "in cw")]
    if "--all" in sys.argv:
        shapes = [(f"{b[0]}, {sh}", b[1], b[2], sh) for sh in ("in cw", "in ccw", "out cw", "out ccw")
                  for b in (("no button", None, None), ("trigger held", "ctrl r trigger 1", "ctrl r trigger 0"))]
    found = {}
    for label, down, up, shape in shapes:
        bwctl.stereo(f"ctrl r aimat {cx:.1f} {g:.1f} {cz + 10:.1f}")
        time.sleep(0.5)
        mark = nh.LOG.stat().st_size
        ld.send("hands gesture_log 9")
        if down:
            bwctl.stereo(down)
            time.sleep(0.3)
        spiral(cx, cz, shape)
        if up:
            bwctl.stereo(up)
        time.sleep(1.0)
        t = ld.tail(mark)
        fed = re.findall(r"gesture: fed .*samples (-?\d+) -> (-?\d+)", t)
        emptied = len(re.findall(r"emptied by the turn", t))
        acts = re.findall(r"recognises action (\d+)", t)
        found[label] = acts
        print(f"   {label}: {len(fed)} feed lines, samples up to {max([int(b) for a, b in fed] or [-1])}, emptied {emptied}x, actions {sorted(set(acts))}")
        for line in t.splitlines():
            if "gesture:" in line:
                print("      " + line[11:160])
                break
    ok = "1" in found.get("no button, in cw", [])
    print(f"{'PASS' if ok else 'FAIL'}  an inward clockwise spiral drawn with the laser is the game's Miracle selection (action 1): {found}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
