r"""The spirits seen in the new game's opening scenes (player, 2026-11-02: not seen while they
talked on the cinema screen).

Start a New Game from the pause menu (Main Menu > Options > Players > Start a New Game > Yes, OK:
"Start the game normally"), clicked with the laser; then the trigger clicks through the scenes until
the spirits are placed ("We're your conscience"). Checks that each placement, while the cinema screen
is up, is the game's own (beside the scene's camera, within 20 units of it) and not moved to the
VR head. Saves a window shot (`opening_spirits.png`) to look at.

Runs on the player's "test" profile; set HKCU\...\LHMultiplayer\Profile to test first, and
back after. Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot); kill after (the new game
overwrites the profile's saves; `bwctl kill` restores them).
Usage: python tools/validate/opening_spirits.py
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
import debug_menu as dm
from near_hand import bwctl


def controls():
    m = nh.LOG.stat().st_size
    ld.send("ui menu_dump")
    time.sleep(0.8)
    out = []
    for line in ld.tail(m).splitlines():
        g = re.search(r"control \S+ (\S+) id (-?\d+) \| \S+ (\S+) (\S+) (\S+) (\S+) .* '(.*)'$", line)
        if g:
            x0, y0, x1, y1 = (int(g.group(i), 16) for i in range(3, 7))
            out.append((g.group(7).strip(), (560 + (x0 + x1) // 2, 240 + (y0 + y1) // 2)))
    return out


def click_label(label):
    for text, centre in controls():
        if text.lower().startswith(label.lower()):
            dm.press(centre)
            time.sleep(1.5)
            return True
    return False


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands laser 0", "ctrl r trigger 0", "ctrl l off", "pose 0 0 0 0 0 0", "ui screen 1")
    time.sleep(1.5)
    bwctl.request("input.req", "key esc\n")
    time.sleep(3)
    clicked = all(click_label(b) for b in ("Main Menu", "Options", "Players", "Start a New Game", "Yes."))
    time.sleep(8)
    clicked = clicked and click_label("OK")
    results.append(nh.report(clicked, "Start a New Game > Yes > OK, clicked with the laser"))
    mark = nh.LOG.stat().st_size
    found = []
    t0 = time.time()
    while time.time() - t0 < 600 and len(found) < 6:
        ld.send("ctrl r trigger 1")
        time.sleep(0.4)
        ld.send("ctrl r trigger 0", "helper_log 2")
        time.sleep(5)
        found = re.findall(r"helper spirit \S+ at hover .*?the game's \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), drawn at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\); "
                           r"camera \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", ld.tail(mark))
    bwctl.shot("opening_spirits")
    results.append(nh.report(bool(found), f"the spirits placed in the opening ({len(found)} placements logged)"))
    if found:
        worst = 0.0
        for f in found:
            v = [float(x) for x in f]
            moved = math.dist(v[0:3], v[3:6])
            worst = max(worst, moved, math.dist(v[3:6], v[6:9]) - 20)
        results.append(nh.report(worst < 0.01, f"each placed as the game put it, beside the scene's camera (worst {worst:.2f} units off)"))
    print(f"{sum(results)} of {len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
