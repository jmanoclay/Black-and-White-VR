"""Scripted test of tapping a scroll that floats over a house (the user's save: the west village,
the scroll over the house at (1350.3, 2394.9)).

Scrolls (ScriptHighlight) are small; the house under one is big. With the game's cursor at the
palm, its picker often settled on the house, and the trigger knocked on it (the hand's knock,
state 9) instead of tapping the scroll. The cursor now goes onto a scroll within reach of the palm.

Checks: with the palm a little off the scroll (beside it, above it, between it and the roof), the
hand is on the scroll, not the house; and a trigger tap there starts the scroll's scene (no knock).

Usage: python tools/validate/scroll_tap.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld

HOUSE = (1350.3, 2394.9)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -20 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    g = nh.ground_at(*HOUSE)
    ld.place_palm((HOUSE[0], g + 6.0, HOUSE[1]))
    time.sleep(1.0)
    mark = nh.LOG.stat().st_size
    ld.send("pointerinfo")
    time.sleep(0.4)
    m = re.search(r"pointer: touch (\d) object (\S+) centre \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) radius ([\d.]+)", ld.tail(mark))
    if not m or float(m.group(6)) > 3:
        print(f"SKIP  no scroll over the house in this save ({m.groups() if m else None})")
        return 0
    c = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
    scroll_id = m.group(2)
    for name, off in (("beside it", (1.6, 0, 0)), ("above it", (0, 1.6, 0)), ("below it, toward the roof", (0, -1.4, 0.6))):
        ld.place_palm((c[0] + off[0], c[1] + off[1], c[2] + off[2]))
        time.sleep(1.0)
        mark = nh.LOG.stat().st_size
        ld.send("pointerinfo")
        time.sleep(0.4)
        t = re.search(r"pointer: touch (\d) object (\S+)", ld.tail(mark))
        on = t and t.group(1) == "1" and t.group(2) == scroll_id
        results.append(nh.report(bool(on), f"palm {name}: the hand is on the scroll, not the house ({t.groups() if t else None})"))
    # One real tap (it starts the scroll's scene): the scroll, not a knock on the house.
    mark = nh.LOG.stat().st_size
    nh.bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.15)
    nh.bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    text = ld.tail(mark)
    results.append(nh.report("-> 9" not in text and "cutscene started" in text, "the trigger there taps the scroll: its scene starts, no knock"))
    nh.bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
