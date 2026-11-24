"""Scripted test of the gesture panel above the left hand (player, 2026-10-28: the pink shapes off to
the side of their icons, more so the further the hand is out of view).

With the leash in the hand the game shows its three leash gestures (icons, with their animated
shapes under them). The left palm is turned up so they go onto the panel above it, and the head is
turned so the panel is in the middle of the view, then toward the edges. For each view, from the
log (`ui gesture_log`):
- each shape's points are drawn where the mod puts them on the panel (the engine's projection of
  the sprite against the mod's, within 3 px);
- each icon is drawn in its own slot: its backing square's middle on the game's screen is its
  rectangle's middle (within 2 px). (The engine's clamp to the screen's edges squashed icons
  against an edge, away from their shapes: 4535b07.)
(On the flat eye image a shape can look off to the side of its icon near the edges: that is the
wide-angle perspective of a panel facing the head, gone in the headset's lenses.)

Boot first (with Windows audio stalled: BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/gesture_panel.py
"""
import re
import statistics
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail, leash_state

LEFT = "ctrl l pose 0 0 -90 -0.15 -0.3 0.35"  # the left palm turned up, in front and a little left
VIEWS = [("middle", "pose 0 0 0 0 0 0"), ("right 20", "pose 20 0 0 0 0 0"), ("right 35", "pose 35 0 0 0 0 0"),
         ("up 25", "pose 0 25 0 0 0 0"), ("up 35", "pose 0 35 0 0 0 0"), ("left 25", "pose -25 0 0 0 0 0"),
         ("rolled left 30", "pose 0 0 30 0 0 0"), ("rolled right 30", "pose 0 0 -30 0 0 0"),
         # (The player was turned 99 degrees from the play area's forward, playtest 2026-11-01; the
         # left hand turned with them, as it would be.)
         ("turned 99", "pose 99 0 0 0 0 0", "ctrl l pose 99 0 -90 0.29 -0.3 -0.2"),
         ("turned 99, hand moved", "pose 99 -15 0 0 0 0", "ctrl l pose 99 0 -90 0.33 -0.2 -0.15"),
         ("hand lower and nearer", "pose 0 -25 0 0 0 0", "ctrl l pose 0 0 -90 -0.1 -0.4 0.25"),
         ("hand out to the left", "pose -20 0 0 0 0 0", "ctrl l pose 0 0 -90 -0.35 -0.25 0.4")]  # (the HUD draws the shapes without the head's roll: 2026-11-01)

DRAW = re.compile(r"gesture draw \(eye 0\) \d+ verts fvf \S+: \(([-\d]+), [-\d]+\)-\(([-\d]+), [-\d]+\) z (\d+\.\d+)")
POINT = re.compile(r"gesture point \(eye 0\) item (\S+) frac \(([-\d.]+), ([-\d.]+)\) screen \(([-\d.]+), [-\d.]+\).*?engine \(([-\d]+), ([-\d]+)\) vis (\d+), ours \(([-\d.]+), ([-\d.]+)\)")


def icons():
    mark = nh.LOG.stat().st_size
    send("ui gesture_state")
    time.sleep(0.5)
    m = re.search(r"ui: gesture panel \w+, .*?, (\d+) icons", tail(mark))
    return int(m.group(1)) if m else 0


def measure():
    """From one frame of the left eye: the icons' backing squares' middles on the game's screen, and
    each item's points: its rectangle's middle on the screen (frac 0.5, 0.5), and the worst gap
    between the engine's projection of a point and the mod's."""
    mark = nh.LOG.stat().st_size
    send("ui gesture_log 1")
    time.sleep(1.0)
    text = tail(mark)
    squares = sorted((int(m.group(1)) + int(m.group(2))) / 2 for m in DRAW.finditer(text) if float(m.group(3)) > 0.99)
    middles, gap, points = [], 0.0, 0
    for m in POINT.finditer(text):
        if m.group(2) == "0.50" and m.group(3) == "0.50":
            middles.append(float(m.group(4)))
        if int(m.group(7)):  # (in the engine's view)
            points += 1
            gap = max(gap, abs(int(m.group(5)) - float(m.group(8))), abs(int(m.group(6)) - float(m.group(9))))
    return squares, sorted(set(middles)), gap, points


def main():
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r pose 0 0 0 0 -0.3 0.5", "pose 0 0 0 0 0 0")
    time.sleep(1.5)
    # (Whatever the game offers: the miracles' gestures in a save with miracles (the player's Island 2
    # autosave, 2026-11-03), else the leash's in the hand.)
    send(LEFT)
    time.sleep(1.0)
    if icons() < 2:
        send("hands leash_on")
        time.sleep(1.5)
    n = icons()
    print(f"      {n} gesture icons offered by the game")
    if n < 1:
        print("SKIP  no gesture icons in this save")
        return 0
    send(LEFT)
    time.sleep(1.0)
    results = []
    for name, *poses in VIEWS:
        send(*poses, *([] if len(poses) > 1 else [LEFT]))
        time.sleep(1.2)
        squares, middles, gap, points = measure()
        if len(squares) < max(1, n - 1) or len(middles) < n or not points:  # (an icon wholly beside the eye isn't drawn)
            results.append(nh.report(False, f"{name}: the panel isn't drawn ({len(squares)} icons, {len(middles)} items, {points} points in view)"))
            continue
        off = max(min(abs(s - m) for m in middles) for s in squares)
        results.append(nh.report(off <= 2, f"{name}: each icon in its own slot (worst {off:.0f} px from its rectangle's middle)"))
        results.append(nh.report(gap <= 3, f"{name}: the shapes drawn where placed ({points} points, worst {gap:.0f} px)"))
        bwctl.stereo_capture(f"gesture_panel_{name.replace(' ', '_')}")
    # Held up and still for 10 s: every frame looked at has every icon and its shape (the panel mustn't
    # drop out from under a steady hand).
    send("pose 0 -10 0 0 0 0", LEFT)
    time.sleep(1.2)
    gaps = []
    for k in range(10):
        squares, middles, gap, points = measure()
        if len(squares) < n or len(middles) < n or not points:
            gaps.append(f"{k}: {len(squares)} icons, {len(middles)} shapes")
    results.append(nh.report(not gaps, f"held still 10 s: all {n} icons and their shapes in each of 10 frames looked at" + (f" (missing: {gaps})" if gaps else "")))
    send("ctrl l off", "pose 0 0 0 0 0 0")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
