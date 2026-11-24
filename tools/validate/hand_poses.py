"""Scripted test of each drawn hand's own fingers (a fist on the grip, the other hand open).

Both drawn hands use the game's one skeleton. The mod records the game's relaxed hand (hovering)
as the open pose and curls it into a fist. Checks, from the per-hand pose log:
- the open pose has been recorded (bwvr\\hand_poses.bin);
- nothing pressed: the other hand is drawn open, the god hand with the game's animation;
- the other hand's grip: it is a fist, the god hand isn't;
- the god hand's grip in open air (nothing to grab): a fist, and the game's hand stays free;
- the god hand carrying a tree: the game's animation (it holds it), the other hand still open.

Usage: python tools/validate/hand_poses.py
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import tail


def poses():
    """The last logged pose of each hand: {"god": ..., "other": ...}."""
    mark = nh.LOG.stat().st_size
    bwctl.stereo("pose_log 3")
    time.sleep(0.6)
    out = {}
    for m in re.finditer(r"hand pose: (god|other) hand (\w+)", tail(mark)):
        out[m.group(1)] = m.group(2)
    return out


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
                 "hands touch 1", "hands fist_on_grip 1", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0",
                 "ctrl l trigger 0", "ctrl l squeeze 0")
    time.sleep(1)
    # Open air, 3 m up: the game's hand hovers, so the open pose gets recorded.
    bwctl.stereo(f"rig set {nh.TREE[0] + 30:.1f} 50 {nh.TREE[1] - 34.9:.1f} 0", "pose 0 -30 0 0 0 0",
                 "ctrl r pose 0 -20 0 0.1 -0.2 0.49", "ctrl l pose 0 -20 0 -0.15 -0.2 0.45")
    time.sleep(2.5)
    recorded = (nh.LOG.parent.parent / "hand_poses.bin").exists()
    results.append(nh.report(recorded, "the open pose is recorded (hand_poses.bin)"))
    p = poses()
    results.append(nh.report(p.get("other") == "open" and p.get("god") == "game", f"nothing pressed: other hand open, god hand the game's ({p})"))
    bwctl.stereo("ctrl l squeeze 1")
    time.sleep(0.5)
    p = poses()
    results.append(nh.report(p.get("other") == "fist" and p.get("god") == "game", f"other hand's grip: a fist, the god hand not ({p})"))
    bwctl.stereo("ctrl l squeeze 0", "ctrl r squeeze 1")
    time.sleep(0.8)
    p = poses()
    state = nh.hand()[0]
    results.append(nh.report(p.get("god") == "fist" and p.get("other") == "open" and state == 1,
                             f"god hand's grip on nothing: a fist, the game's hand free (state {state}, {p})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.5)
    # Carrying a tree: the game's own hand holding it.
    g = nh.ground_at(*nh.TREE)
    bwctl.stereo(f"rig set {nh.TREE[0]:.1f} {g + 13.5:.1f} {nh.TREE[1] - 4.9:.1f} 0", "ctrl r pose 0 -20 0 0 -0.2 0.49")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (-0.1, -0.2, -0.3, -0.4, -0.5, -0.6):
        bwctl.stereo(f"ctrl r pose -20 -20 0 {x} -0.15 0.45")
        time.sleep(0.4)
    time.sleep(1.5)
    state = nh.hand()[0]
    p = poses()
    results.append(nh.report(state == 4 and p.get("god") == "game" and p.get("other") == "open",
                             f"carrying a tree: the god hand the game's, the other hand open (state {state}, {p})"))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2)
    bwctl.stereo("ctrl r off", "ctrl l off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
