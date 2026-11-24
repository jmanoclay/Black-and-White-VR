"""Scripted test of handing an object to a creature in VR (needs a save with a creature).

The hand uproots the test tree, the rig is put next to the creature, and the carried tree is held up
to it. If the game names the creature as the one to give to (CHand+0x49B4), the mod hands the object
over and the hand ends up empty without the grip opening. If it doesn't (the object is too big for
the creature), nothing happens and the hand keeps it.

Usage: python tools/validate/give_creature.py
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl


def creatures():
    mark = nh.LOG.stat().st_size
    bwctl.stereo("hands creatures")
    time.sleep(1.0)
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        text = f.read()
    return [tuple(float(v) for v in m) for m in
            re.findall(r"hands: creature \d+ \S+ sphere 1 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) r ([\d.]+)", text)]


def own_creature():
    """The player's creature (x, y, z, r), or None."""
    mark = nh.LOG.stat().st_size
    bwctl.stereo("hands creatures")
    time.sleep(1.0)
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        text = f.read()
    m = re.search(r"hands: creature \d+ \S+ sphere 1 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) r ([\d.]+) \(the player's\)", text)
    return tuple(float(v) for v in m.groups()) if m else None


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "hands reach 1.0", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
    time.sleep(1)
    found = creatures()
    if not found:
        print("SKIP  no creature in this save")
        return 0
    # Uproot the test tree (as near_hand.py does).
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
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(state == 4 and holding == 1, f"carrying the tree (state {state}, holding {holding})"))
    # Next to the smallest creature (the player's; the other one here is far bigger), hand well away from it.
    cx, cy, cz, r = own_creature()  # the player's (the game's own mark)
    palm = (0.0, -0.3, 0.5)  # metres from the head, rig heading 0
    bwctl.stereo("ctrl r pose 0 0 0 0.6 0.3 0.5", f"rig set {cx:.2f} {cy + 3.0 + 10.0:.2f} {cz - 5.0:.2f} 0", "pose 0 -40 0 0 0 0")
    time.sleep(2.0)
    state, holding, _, _ = nh.hand()
    results.append(nh.report(holding == 1, f"still carrying beside the creature, hand away from it (holding {holding})"))
    mark = nh.LOG.stat().st_size
    # Hold it up to the creature: the palm at the creature's centre (it may wander: look it up again).
    cx, cy, cz, r = own_creature()  # the player's (the game's own mark)
    bwctl.stereo(f"rig set {cx - palm[0] * 10:.2f} {cy - palm[1] * 10:.2f} {cz - palm[2] * 10:.2f} 0",
                 f"ctrl r pose 0 0 0 {palm[0]} {palm[1]} {palm[2]}")
    time.sleep(3.0)
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        text = f.read()
    handed = "hands: handing what the hand holds to the creature" in text
    state, holding, _, _ = nh.hand()
    if handed:
        results.append(nh.report(holding == 0, f"the game named the creature as the one to give to; handed over, the hand is empty "
                                               f"with the grip still closed (state {state}, holding {holding})"))
    else:
        # The game only names a creature to give to for things it can take (a small creature can't
        # take a full-grown tree). Then nothing must happen: no click, the object stays in the hand.
        results.append(nh.report(holding == 1, f"the game named no creature to give to (too big for it?): the hand keeps the object "
                                               f"(state {state}, holding {holding})"))
    for line in text.splitlines():
        if "game hand state" in line or "launch" in line or "camera mode" in line:
            print("      " + line[12:150])
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1)
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
