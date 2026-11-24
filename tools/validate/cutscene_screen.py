"""Scripted test of cutscenes on the cinema screen in VR.

A cutscene is forced on (`cutscene 1` overrides the game's widescreen flag).
1. The eye images show a picture on a screen with dark around it, in both eyes.
2. The player stays put while the game's camera is moved as a script would move it.
3. A controller aimed at the middle of the screen makes the laser the mouse there (cursor at the
   middle of the game's frame); the trigger is the left button.
4. Aimed off the screen, the controller does nothing.
5. When the cutscene ends, play resumes at the game's camera.

Usage: python tools/validate/cutscene_screen.py
"""
import json
import math
import re
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh
from near_hand import bwctl

CAP = bwctl.CAPTURES
DIST, DROP = 3.0, 0.1  # the cinema screen: metres ahead, and its centre below eye level


def rig():
    (m,) = nh.query("rig info", [r"rig: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) yaw ([-\d.]+) scale ([\d.]+)"])
    return [float(m.group(i)) for i in range(1, 6)]


def pointer():
    (m,) = nh.query("pointerinfo", [r"pointer: valid 1 .* land (\d) cursor (\d) \((-?\d+), (-?\d+)\) lmb (\d) rmb (\d)"])
    return int(m.group(3)), int(m.group(4)), int(m.group(5))


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "world_scale 10", "cutscene -1", "cutscene_screen 1",
                 "hands laser 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 0 0 0 0 0")
    time.sleep(2)
    x0, y0, z0, yaw0, _ = rig()
    bwctl.stereo("cutscene 1", "ctrl r pose 60 -40 0 0.2 -0.3 0.3")  # controller aimed well off the screen
    time.sleep(2)
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        log = f.read()
    results.append(nh.report("cutscene started: on the cinema screen" in log, "the cutscene goes to the cinema screen"))
    bwctl.stereo_capture("cinema")
    meta = json.load(open(CAP / "cinema.json"))
    (m,) = nh.query("ui screeninfo", [r"ui: screen centre in the eyes L \(([-\d.]+), ([-\d.]+)\) R \(([-\d.]+), ([-\d.]+)\)"])
    centres = ((float(m.group(1)), float(m.group(2))), (float(m.group(3)), float(m.group(4))))
    lit, dark = [], []
    for i, eye in enumerate("LR"):
        a = np.asarray(Image.open(CAP / f"cinema_{eye}.bmp").convert("RGB")).astype(int).sum(axis=2)
        crop = meta["eyes"][i]["crop"]
        cx, cy = int(centres[i][0] - crop[0]), int(centres[i][1] - crop[1])
        # (The game follows the real clock: at night the picture is dim, so "lit" only means brighter than the surround.)
        lit.append(float((a[cy - 100:cy + 100, cx - 100:cx + 100] > 30).mean()))
        dark.append(float((a[20:120, 20:120] < 40).mean()))  # a corner of the eye image
    # (The picture is whatever the game shows; at dusk parts of it are dark too.)
    results.append(nh.report(min(lit) > 0.3 and min(dark) > 0.99,
                             f"a picture on the screen with dark around it, in both eyes (screen centre {lit[0]:.0%}/{lit[1]:.0%} lit, corner {dark[0]:.0%}/{dark[1]:.0%} dark)"))
    f = meta["eyes"][0]["focal_px"]
    ipd = meta["eyes"][1]["offset_m"][0] - meta["eyes"][0]["offset_m"][0]
    disparity = centres[0][0] - centres[1][0]
    results.append(nh.report(abs(disparity - f * ipd / DIST) < 1, f"the screen is {DIST} m away ({disparity:.1f} px of disparity; {f * ipd / DIST:.1f} expected)"))
    # The script moves the game's camera: the player stays put.
    bwctl.stereo(f"gamecam {x0 + 40} {y0 + 12} {z0 + 25} {x0 + 90} {y0 + 2} {z0 + 25}")
    time.sleep(1.5)
    x, y, z, yaw, _ = rig()
    moved = math.dist((x, y, z), (x0, y0, z0))
    results.append(nh.report(moved < 0.05 and abs(yaw - yaw0) < 0.1, f"the player stays put while the game's camera moves ({moved:.2f} units)"))
    # Off the screen: nothing.
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.8)
    _, _, lmb = pointer()
    results.append(nh.report(lmb == 0, "aimed off the screen, the trigger does nothing"))
    bwctl.stereo("ctrl r trigger 0")
    # At the middle of the screen: the laser is the mouse at the middle of the game's frame.
    hand_pos = (0.2, -0.3, 0.3)
    d = (0 - hand_pos[0], -DROP - hand_pos[1], DIST - hand_pos[2])
    yaw_c = math.degrees(math.atan2(d[0], d[2]))
    pitch_c = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
    bwctl.stereo(f"ctrl r pose {yaw_c:.2f} {pitch_c:.2f} 0 {hand_pos[0]} {hand_pos[1]} {hand_pos[2]}")
    time.sleep(1.5)
    w, h = [int(v) for v in re.search(r"ui: screen (\d+)x(\d+) ", open(nh.LOG, encoding="utf-8", errors="replace").read()[-20000:]).groups()]
    cx, cy, _ = pointer()
    results.append(nh.report(abs(cx - w / 2) < 6 and abs(cy - h / 2) < 6, f"aimed at the middle of the screen: cursor ({cx}, {cy}) of {w}x{h}"))
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.8)
    _, _, lmb = pointer()
    results.append(nh.report(lmb == 1, "on the screen, the trigger is the left button"))
    bwctl.stereo("ctrl r trigger 0", "cutscene_log 1")
    time.sleep(0.5)
    # End: play resumes at the game's camera (wherever the game has it by now).
    bwctl.stereo("cutscene 0", "cutscene_screen 0", "cutscene 1")  # one carried frame logs the camera
    time.sleep(1.2)
    cams = re.findall(r"stereo: cutscene camera \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", open(nh.LOG, encoding="utf-8", errors="replace").read())
    bwctl.stereo("cutscene_screen 1", "cutscene 0")
    time.sleep(1.0)
    x, y, z, _, _ = rig()
    cam = [float(v) for v in cams[-1]] if cams else (0, 0, 0)
    off = math.hypot(x - cam[0], z - cam[2])
    results.append(nh.report(bool(cams) and off < 3.0, f"after the cutscene play resumes at the game's camera ({off:.2f} units from it)"))
    bwctl.stereo("cutscene -1", "cutscene_log 0", "ctrl r off", "rig reset")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
