"""The Developer tab's "Autosave now" (player, 2026-11-03: a way to see the headset's saving cover
without waiting for the game's own autosave).

- Esc, the Developer tab: its first line is "Autosave now";
- + on it: the request is logged, and nothing is saved while the menu is up;
- Back, then Continue Game: about 2 s into play the game autosaves ("the game saved in ... s").
(The dim and "Saving..." panel themselves are SteamVR's: not seen in the harness.)

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot). The harness's kill restores the saves.
Usage: python tools/validate/autosave_button.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from debug_menu import menu, press, OPEN, BACK

PLUS1 = (1280, 428)  # (+ on the Developer page's first line)
CONTINUE = (960, 420)  # ("Continue Game" on the main page; Esc there doesn't reach it in the harness)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 0 0 0 0 0", "ui screen 1")
    time.sleep(1.0)
    up, _ = menu()
    if not (up and up[0] == up[1]):
        bwctl.request("input.req", "key esc\n")
        time.sleep(3)
    press(OPEN)
    up, lines = menu()
    first = lines[0] if lines else "?"
    results.append(nh.report(first.startswith("Autosave now"), f"the Developer page's first line: '{first}'"))
    mark = nh.LOG.stat().st_size
    press(PLUS1)
    time.sleep(3.0)
    text = ld.tail(mark)
    results.append(nh.report("autosave requested" in text and "the game saved" not in text,
                             "+ on it: requested, nothing saved while the menu is up"))
    press(BACK)
    for _ in range(3):  # (the first click can come while the main page is still being put back)
        time.sleep(1.0)
        press(CONTINUE)
        up, _ = menu()
        if up and up[1] != up[0]:
            break
    t0 = time.time()
    saved = None
    while time.time() - t0 < 15 and not saved:
        time.sleep(0.5)
        saved = re.search(r"the game saved in ([\d.]+) s", ld.tail(mark))
    after = time.time() - t0
    results.append(nh.report(bool(saved), f"back in play: the game autosaved ({saved.group(0) if saved else 'not saved'}; "
                                          f"seen {after:.1f} s after Continue)"))
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
