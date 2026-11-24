"""Scripted test of the pause menu's VR Options tab (player, 2026-11-01: the standard player VR options,
move speed, snap turn degrees, ..., as a tab in the pause menu).

Clicked as in the headset, with the laser and trigger (debug_menu.py's aim):
- the VR Options tab opens its page: Turning, Snap turn, Smooth turn speed, Move speed, Main hand, ...;
- + on Snap turn: 45 degrees; Back; a flick of the right stick turns the view 45 degrees;
- + on Turning: smooth; the right stick held at full turns it smoothly, about 90 degrees a second;
- both set back as they were (the player's saved settings are left as they were).

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/vr_options.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
import debug_menu as dm

OPTIONS = (960, 270)  # (the VR Options tab; the menu's box at 560, 240 on the screen)
TURNING_PLUS, TURNING_MINUS = (1280, 372), (1210, 372)  # (the first line: y 110-154 in the box)
SNAP_PLUS, SNAP_MINUS = (1280, 424), (1210, 424)  # (the second: 162-206)
BACK = (1190, 808)


def yaw():
    (m,) = nh.query("rig info", [r"rig: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) yaw ([-\d.]+)"])
    return float(m.group(4))


def open_options():
    up, _ = dm.menu()
    if not (up and up[0] == up[1]):
        bwctl.request("input.req", "key esc\n")
        time.sleep(3)
    dm.press(OPTIONS)
    return dm.menu()


def close_menu():
    dm.press(BACK)
    ld.send("ctrl l a 1")
    time.sleep(0.3)
    ld.send("ctrl l a 0")
    time.sleep(1.5)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l pose 0 0 0 -0.2 -0.3 0.3", "pose 0 0 0 0 0 0", "ui screen 1")
    time.sleep(1.0)
    up, lines = open_options()
    results.append(nh.report(bool(up) and up[2] == "options" and any(l.startswith("Snap turn") for l in lines),
                             f"the VR Options tab: its page is up ({lines[:4]})"))
    dm.press(SNAP_PLUS)
    snap = dm.menu()[1][1] if len(dm.menu()[1]) > 1 else "?"
    close_menu()
    ld.send("ctrl r pose 0 -40 0 0.2 -0.3 0.3")
    time.sleep(0.5)
    y0 = yaw()
    ld.send("ctrl r stick 1 0")
    time.sleep(0.3)
    ld.send("ctrl r stick 0 0")
    time.sleep(0.5)
    turned = (yaw() - y0 + 540) % 360 - 180
    results.append(nh.report(snap.endswith("45 degrees") and abs(abs(turned) - 45) < 0.5, f"snap turn set to 45: '{snap}', a flick turns {turned:.1f} degrees"))
    open_options()
    dm.press(TURNING_PLUS)
    mode = dm.menu()[1][0] if dm.menu()[1] else "?"
    close_menu()
    ld.send("ctrl r pose 0 -40 0 0.2 -0.3 0.3")
    time.sleep(0.5)
    y0 = yaw()
    ld.send("ctrl r stick 1 0")
    time.sleep(1.0)
    ld.send("ctrl r stick 0 0")
    time.sleep(0.3)
    turned = (yaw() - y0 + 540) % 360 - 180
    results.append(nh.report(mode.endswith("smooth") and 60 < abs(turned) < 130, f"turning set to smooth: '{mode}', the stick held 1 s turns {turned:.0f} degrees"))
    # (Back as they were.)
    open_options()
    dm.press(TURNING_MINUS)
    dm.press(SNAP_MINUS)
    _, lines = dm.menu()
    results.append(nh.report(lines[:2] == ["Turning: snap", "Snap turn: 30 degrees"], f"set back: {lines[:2]}"))
    close_menu()
    ld.send("ctrl r off", "ctrl l off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
