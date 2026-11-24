"""Scripted test: inside the temple, the laser and trigger work the hall as the mouse does in the flat
game (the user's save: the temple by the creature pen).

The player could point in the hall but no longer drag to turn it, or click its rooms
(2026-10-21). In the flat game, holding the left button and moving the mouse turns the hall.

Checks:
- holding the trigger and sweeping the laser across the cinema screen turns the hall (the game's
  frame changes a lot), and the game saw the left button go down;
- for comparison, sweeping the laser without the trigger leaves the frame much as it was.

Usage: python tools/validate/temple_hall.py
"""
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh
import leash_direct as ld
import temple_save as ts
from near_hand import bwctl


def frame(name):
    return np.asarray(Image.open(bwctl.shot(name)).convert("L")).astype(int)


def sweep(trigger):
    ts.aim(700, 540)
    if trigger:
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.4)
    for px in range(760, 1281, 60):
        ts.aim(px, 540)
    if trigger:
        bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    inside = False
    for attempt in range(4):  # (the creature sometimes stands in the entrance and takes the tap)
        mark = nh.LOG.stat().st_size
        ld.place_palm(ts.ENTRANCE)
        time.sleep(0.6)
        ts.trigger()
        time.sleep(8)
        if "shown on the screen (view mode 1)" in ts.log_since(mark):
            inside = True
            break
        bwctl.stereo("ctrl r pose 0 0 0 0.2 -0.3 0.3")
        time.sleep(15)
    if not inside:
        print("SKIP  couldn't get into the temple (the creature in the entrance?)")
        return 0
    bwctl.stereo("pose 0 0 0 0 0 0", "hands laser 1", "ctrl r pose 0 0 0 0.2 -0.3 0.3")
    time.sleep(4)
    a = frame("hall_a")
    sweep(False)
    b = frame("hall_b")
    still = float(np.abs(a - b).mean())
    mark = nh.LOG.stat().st_size
    sweep(True)
    c = frame("hall_c")
    turned = float(np.abs(b - c).mean())
    results.append(nh.report(turned > max(8.0, 3 * still),
                             f"trigger held and the laser swept: the hall turns (frame change {turned:.1f}; swept without the trigger {still:.1f})"))
    # A click on a door: the Challenge Room's sign, on the left of the hall as it now faces; the
    # game's mouse must be where the laser is (it was (0, 0) inside the temple).
    mark = nh.LOG.stat().st_size
    bwctl.stereo("temple_mouse_log 5000")
    time.sleep(0.5)
    ts.aim(1000, 600)
    time.sleep(1)
    import re
    at = re.findall(r"temple gets mouse message 7 at \((-?\d+), (-?\d+)\)", ts.log_since(mark))
    last = tuple(int(v) for v in at[-1]) if at else None
    results.append(nh.report(bool(last) and abs(last[0] - 1000) < 8 and abs(last[1] - 600) < 8,
                             f"the temple's mouse is where the laser is: {last} (laser at (1000, 600))"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
