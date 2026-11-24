"""Scripted test: quitting from the game's own menu doesn't crash (player, 2026-10-30).

The game must be running (bwctl boot). Optionally turns on VR through SteamVR first (the crash
was in VR), then Esc -> "Quit Black & White" -> "Yes" (the 1920x1080 layout's places, which the
injected mouse uses in VR too), and waits for the game to end. Checks the log for a CRASH line.

Usage: python tools/validate/quit_menu.py [openvr|mock|mono]
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "openvr"
    if mode != "mono":
        bwctl.stereo("enabled 1", f"vr {mode}", "recenter")
        time.sleep(8)
    mark = nh.LOG.stat().st_size
    bwctl.request("input.req", "key esc\n")
    time.sleep(3)
    bwctl.request("input.req", "click 960 740\n")  # Quit Black & White
    time.sleep(3)
    bwctl.request("input.req", "click 790 593\n")  # Yes
    for _ in range(40):
        time.sleep(1)
        if not bwctl.running():
            break
    ended = not bwctl.running()
    t = nh.LOG.read_bytes()[mark:].decode("utf-8", "replace")
    crash = [l for l in t.splitlines() if "CRASH" in l]
    for l in crash:
        print("   " + l[:300])
    ok = [nh.report(ended, f"the game ends after Quit -> Yes ({mode})"), nh.report(not crash, "without a crash")]
    bwctl.restore_saves()
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
