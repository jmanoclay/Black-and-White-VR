"""Scripted test: the gesture panel stays up while the left palm is held up to be read, at any
natural tilt (playtest 2026-10-31: at some angles of the palm the icons vanished, at others came back).

With the leash in the hand (the game's three leash gestures), the left controller is held in front
and turned through palm angles: flat up, tilted toward the face, turned to face the eyes. At each
the panel's state is read 5 times ("ui gesture_state": up or down, the palm's up and toward-head
values, the icons drawn). Must hold: up with 3 icons at every reading for the reading angles; down
with the palm turned down. With "verbose" every reading is printed.

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/gesture_palm.py [verbose]
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail

STATE = re.compile(r"ui: gesture panel (up|down), palm up ([-\d.]+), toward the head ([-\d.]+), (\d+) icons")
# (name, left controller pose (yaw pitch roll x y z), the panel wanted up)
POSES = [("palm up", "0 0 -90 -0.15 -0.3 0.35", True),
         ("tilted toward the face 30", "0 30 -90 -0.15 -0.3 0.35", True),
         ("tilted toward the face 50", "0 50 -90 -0.15 -0.3 0.35", True),
         ("facing the eyes", "0 70 -90 -0.15 -0.2 0.35", True),
         ("rolled in 30", "0 20 -60 -0.15 -0.3 0.35", True),
         ("rolled out 30", "0 20 -120 -0.15 -0.3 0.35", True),
         ("palm up, nearer", "0 40 -90 -0.1 -0.25 0.25", True),
         ("palm down", "0 0 90 -0.15 -0.3 0.35", False),
         ("palm sideways", "0 0 0 -0.15 -0.3 0.35", False),
         ("then facing the eyes (up from down)", "0 70 -90 -0.15 -0.2 0.35", True)]


def readings(n=5):
    out = []
    for _ in range(n):
        mark = nh.LOG.stat().st_size
        send("ui gesture_state")
        time.sleep(0.25)
        m = STATE.search(tail(mark))
        if m:
            out.append((m.group(1) == "up", float(m.group(2)), float(m.group(3)), int(m.group(4))))
    return out


def main():
    verbose = "verbose" in sys.argv
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
         "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl r pose 0 0 0 0.2 -0.3 0.4", "pose 0 -15 0 0 0 0", "hands leash_on")
    time.sleep(2.0)
    results = []
    for name, pose, want in POSES:
        send(f"ctrl l pose {pose}")
        time.sleep(1.0)
        rs = readings()
        ok = bool(rs) and all(up == want and (n >= 3 if want else True) for up, _, _, n in rs)
        if verbose:
            for r in rs:
                print(f"      {name}: {'up' if r[0] else 'down'} palm up {r[1]:.2f} toward the head {r[2]:.2f}, {r[3]} icons")
        last = rs[-1] if rs else (False, 0, 0, 0)
        results.append(nh.report(ok, f"{name}: panel {'up' if want else 'down'} at every reading (palm up {last[1]:.2f}, toward the head {last[2]:.2f}, {last[3]} icons)"))
    send("hands leash_off", "ctrl l off", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
