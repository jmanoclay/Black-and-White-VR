"""Scripted test of cutscene carrying in VR (the alternative to the cinema screen: cutscene_screen=0).

A cutscene is forced on (`cutscene 1` overrides the game's widescreen flag) and the game's camera is
moved as a script would move it.
1. In normal play the game is not in a cutscene.
2. The rig goes with the game's camera: position and heading.
3. The head still looks around: a head offset stays an offset from the camera.
4. When the cutscene ends the rig stays where the camera left it (no easing back).

Usage: python tools/validate/cutscene_carry.py
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl

SCALE = 10.0


def rig():
    (m,) = nh.query("rig info", [r"rig: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) yaw ([-\d.]+) scale ([\d.]+)"])
    return [float(m.group(i)) for i in range(1, 6)]


def log_tail():
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        return f.read()


def carried():
    """The game's camera and the rig as last logged during the cutscene: (cam xyz, cam heading, rig xyz, rig heading)."""
    time.sleep(0.8)
    m = re.findall(r"stereo: cutscene camera \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) heading ([-\d.]+) \| rig \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) heading ([-\d.]+)",
                   log_tail())
    v = [float(t) for t in m[-1]]
    return v[0:3], v[3], v[4:7], v[7]


def main():
    results = []
    mark = nh.LOG.stat().st_size
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "world_scale 10", "cutscene -1", "cutscene_screen 0", "cutscene_log 1", "pose 0 0 0 0 0 0")
    time.sleep(2)
    results.append(nh.report("stereo: cutscene started" not in log_tail()[mark:], "normal play is not taken for a cutscene"))
    x0, y0, z0, yaw0, _ = rig()
    # Push the game's camera somewhere else, as a script would (the game then moves it on by itself;
    # what counts is that the rig is wherever the game's camera actually is).
    cam = (x0 + 40.0, y0 + 12.0, z0 + 25.0)
    bwctl.stereo("cutscene 1", f"gamecam {cam[0]} {cam[1]} {cam[2]} {cam[0] + 50} {cam[1] - 10} {cam[2]}")
    time.sleep(1.5)
    c, ch, r, rh = carried()
    moved = math.dist(c, (x0, y0, z0))
    turn = (rh - ch + 540) % 360 - 180
    results.append(nh.report(moved > 5 and math.dist(c, r) < 0.05 and abs(turn) < 0.5,
                             f"the rig is at the game's camera ({math.dist(c, r):.2f} units apart, headings {rh:.1f} and {ch:.1f}), "
                             f"{moved:.0f} units from where play was"))
    # The head 30 cm to the side and turned: an offset from the camera, not something the rig chases.
    bwctl.stereo("pose 40 0 0 0.3 0 0", f"gamecam {cam[0] + 15} {cam[1]} {cam[2] - 20} {cam[0] + 15} {cam[1] - 10} {cam[2] - 70}")
    time.sleep(1.5)
    c, ch, r, rh = carried()
    turn = (rh - ch + 540) % 360 - 180
    results.append(nh.report(math.dist(c, r) < 0.05 and abs(turn) < 0.5,
                             f"head moved and turned: the rig is still at the camera ({math.dist(c, r):.2f} units apart, headings {rh:.1f} and {ch:.1f})"))
    x, y, z, _, _ = rig()
    bwctl.stereo("cutscene 0", "pose 0 0 0 0 0 0")
    time.sleep(2.0)
    x2, y2, z2, _, _ = rig()
    drift = math.hypot(x2 - x, z2 - z)
    results.append(nh.report(drift < 0.5, f"after the cutscene the rig stays where it was left ({drift:.2f} units of drift in 2 s)"))
    bwctl.stereo("cutscene -1", "cutscene_log 0", "cutscene_screen 1", "rig reset")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
