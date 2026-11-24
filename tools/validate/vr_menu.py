"""Scripted test of the game's menu on the virtual screen in VR.

1. Esc opens the menu: its panels are drawn on the virtual screen, inside the window's layout.
2. The screen's centre is at the same height in both eyes, shifted by the disparity of a panel 2 m
   away, and the eye images change there.
3. A controller aimed at "Continue Game" makes the laser the mouse there (cursor at the button).
4. A trigger click closes the menu, and the laser goes back to the world.

Usage: python tools/validate/vr_menu.py
"""
import json
import math
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh
from near_hand import bwctl

CAP = bwctl.CAPTURES
BUTTON = (960, 420)  # "Continue Game" in the 1920x1080 layout
DIST, WIDTH, DROP = 2.0, 2.6, 0.1  # the virtual screen: metres


def screen():
    (m,) = nh.query("ui screeninfo", [r"ui: screen (\d+)x(\d+) .* box \(([-\d.e+]+), ([-\d.e+]+)\)-\(([-\d.e+]+), ([-\d.e+]+)\) drawn ([\d.]+) s ago"])
    return int(m.group(1)), int(m.group(2)), [float(m.group(i)) for i in (3, 4, 5, 6)], float(m.group(7))


def cursor():
    (m,) = nh.query("pointerinfo", [r"pointer: valid 1 .* land (\d) cursor (\d) \((-?\d+), (-?\d+)\) lmb"])
    return int(m.group(1)), int(m.group(3)), int(m.group(4))


def changed(name, eye, meta, index, centre):
    """Fraction of pixels that changed in a 300 px box around `centre` (render pixels)."""
    a = np.asarray(Image.open(CAP / f"{name}_{eye}.bmp").convert("RGB")).astype(int)
    off = np.asarray(Image.open(CAP / f"menu_off_{eye}.bmp").convert("RGB")).astype(int)
    crop = meta["eyes"][index]["crop"]
    x, y = int(centre[0] - crop[0]), int(centre[1] - crop[1])
    d = np.abs(a - off).sum(axis=2)[max(0, y - 150):y + 150, max(0, x - 150):x + 150]
    return float((d > 60).mean()) if d.size else 0.0


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 0 0 0 0 0", "ui screen 1")
    time.sleep(1.5)
    bwctl.stereo("ctrl r pose 0 -40 0 0.2 -0.3 0.3", "freeze 1")
    time.sleep(1)
    bwctl.stereo_capture("menu_off")
    bwctl.stereo("freeze 0")
    bwctl.request("input.req", "key esc\n")
    time.sleep(3)
    w, h, box, ago = screen()
    results.append(nh.report(ago < 0.2 and 0 <= box[0] < box[2] <= w and 0 <= box[1] < box[3] <= h,
                             f"menu drawn on the virtual screen, panels at ({box[0]:.0f}, {box[1]:.0f})-({box[2]:.0f}, {box[3]:.0f}) of {w}x{h}"))
    bwctl.stereo_capture("menu_on")
    meta = json.load(open(CAP / "menu_on.json"))
    (m,) = nh.query("ui screeninfo", [r"ui: screen centre in the eyes L \(([-\d.]+), ([-\d.]+)\) R \(([-\d.]+), ([-\d.]+)\)"])
    left, right = (float(m.group(1)), float(m.group(2))), (float(m.group(3)), float(m.group(4)))
    f = meta["eyes"][0]["focal_px"]
    ipd = meta["eyes"][1]["offset_m"][0] - meta["eyes"][0]["offset_m"][0]
    want = f * ipd / DIST
    disparity = left[0] - right[0]
    results.append(nh.report(abs(left[1] - right[1]) < 1 and abs(disparity - want) < 1,
                             f"screen centre in the eyes: vertical offset {abs(left[1] - right[1]):.1f} px, disparity {disparity:.1f} px "
                             f"(a panel {DIST} m away gives {want:.1f})"))
    seen = [changed("menu_on", e, meta, i, c) for i, (e, c) in enumerate(zip("LR", (left, right)))]
    results.append(nh.report(min(seen) > 0.5, f"the menu is drawn there in both eye images ({seen[0]:.0%} and {seen[1]:.0%} of the pixels around it changed)"))
    # Aim the controller at the button: the panel faces the head 2 m ahead, its centre 10 cm below eye level.
    unit = WIDTH / w
    target = ((BUTTON[0] - w / 2) * unit, -DROP + (h / 2 - BUTTON[1]) * unit, DIST)
    hand_pos = (0.2, -0.3, 0.3)
    d = [t - p for t, p in zip(target, hand_pos)]
    yaw = math.degrees(math.atan2(d[0], d[2]))
    pitch = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
    bwctl.stereo(f"ctrl r pose {yaw:.2f} {pitch:.2f} 0 {hand_pos[0]} {hand_pos[1]} {hand_pos[2]}")
    time.sleep(1.5)
    land, cx, cy = cursor()
    results.append(nh.report(land == 0 and abs(cx - BUTTON[0]) < 6 and abs(cy - BUTTON[1]) < 6,
                             f"the laser is the mouse on the menu: cursor ({cx}, {cy}), button at {BUTTON}"))
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(3)
    _, _, box, _ = screen()
    results.append(nh.report(box[2] < box[0], "a trigger click on Continue Game closed the menu"))
    bwctl.stereo("ctrl r pose 0 -40 0 0.2 -0.3 0.3")
    time.sleep(1.5)
    land, cx, cy = cursor()
    results.append(nh.report(land == 1, "the laser is back on the world"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
