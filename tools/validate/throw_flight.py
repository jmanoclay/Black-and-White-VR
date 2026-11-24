"""Scripted upward throw from the hand at the controller, held well above the ground.

The hand uproots the test tree (as in hand_throw.py), the rig is lifted 4 m, and the controller
moves up at a steady 3 m/s (mock velocity, so no step is lost); the grip opens 0.2 s into the
swing and the hand keeps going. `hands throw_trace 1` logs every frame from the grip opening.

Checks:
- before the game's launch the tree is already on its way: it moves up from where it was in the hand;
- the launch starts at the hand, not on the ground below it (the game's AdjustToGroundLevel);
- the frame after the launch the tree is drawn near where its flight had got to (no jump);
- a second later it is higher than where it left the hand.

Usage: python tools/validate/throw_flight.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl

LIFT = 40.0  # world units the rig is raised before the throw


def flight_lines(text):
    out = []
    for m in re.finditer(r"hands: flight ([\d.]+)( launched)? state \d+ held -?\d+ palm \([^)]*\) object \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text):
        out.append((float(m.group(1)), m.group(2) is not None, float(m.group(4))))
    return out


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "hands throw_trace 1", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
    time.sleep(1)
    g = nh.ground_at(*nh.TREE)
    tip = (nh.TREE[0], g + 6.0, nh.TREE[1] - 0.5)
    rig = tuple(t - c * nh.SCALE for t, c in zip(tip, nh.CTRL))
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0",
                 f"ctrl r pose 0 -70 0 {nh.CTRL[0]} {nh.CTRL[1]} {nh.CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(1)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1] + LIFT:.2f} {rig[2]:.2f} 0", "ctrl r pose 0 10 0 0.1 -0.5 0.45")
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(state == 4 and holding == 1, f"holding the tree up high (state {state}, holding {holding})"))
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r vel 0 3 0", "ctrl r squeeze_after 0.2 0")
    time.sleep(0.6)
    bwctl.stereo("ctrl r vel 0 0 0")
    time.sleep(2.5)
    bwctl.stereo("ctrl r off", "hands throw_trace 0")
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        text = f.read()
    lines = flight_lines(text)
    launch = re.search(r"hands: launch: .* -> \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) \[hand, in flight\] from \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    went = re.search(r"hands: thrown object went from \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) to \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    before = [l for l in lines if not l[1]]
    after = [l for l in lines if l[1]]
    if not (lines and launch and went and before and after):
        print(f"FAIL  no complete trace (flight lines {len(lines)}, launch {launch is not None}, 1 s later {went is not None})")
        return 1
    start_y = before[0][2]
    gap = after[0][0]
    rise = before[-1][2] - start_y
    results.append(nh.report(rise > 2.0, f"on its way before the launch: up {rise:.1f} units in the {gap:.2f} s the game took"))
    from_y = float(launch.group(5))
    results.append(nh.report(from_y > start_y, f"launched from the hand's height ({from_y:.1f}; it was in the hand at {start_y:.1f}, "
                                               f"ground {g:.1f})"))
    # The first frames after the launch: drawn near where the flight had got to. (The trace reads
    # the matrix between the game's turn and its draw, so the frame of a turn can show the turn's
    # own position, a turn ahead; the closest of the first few frames is the one drawn.)
    jump = min(abs(l[2] - before[-1][2]) for l in after[:3])
    results.append(nh.report(jump < 3.0, f"no jump at the launch ({jump:.2f} units in the first frames)"))
    y0, y1 = float(went.group(2)), float(went.group(5))
    results.append(nh.report(y1 > y0 + 10.0, f"still going up a second later ({y0:.1f} -> {y1:.1f})"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
