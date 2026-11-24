"""Scripted test of tying the leash to a tree in VR (needs a save with the leash available).

With the creature on the leash, the hand on a tree and two quick trigger pulls: the leash must
end up tied to the tree (GLeashStatus+0x24), the tree must not be picked up, and no quick travel
may happen. (In the tutorial a scene follows at once: "Your Creature is tied to the tree now";
it is clicked through.) Then a trigger on the creature unties it (the flat game's click on it),
and the same is done with the laser, on a rock further off.

Usage: python tools/validate/leash_tie.py
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail, leash_state, place_palm, PALM

TREE = (1962.1, 34.6, 2523.3)  # a tree by the temple in the user's leash-tutorial save (crown centre)
TREE2 = (2015.0, 8.6, 2343.5)  # for the laser: a rock further off (the creature stands at the tree once tied to it)
AWAY = (0.6, 0.3, 0.5)


def status():
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.5)
    m = re.search(r"leash status \S+: (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) \| (\S+) (\S+)", tail(mark))
    if not m:
        return None, None
    return int(m.group(6), 16), int(m.group(10), 16)  # on (+0x14), tied object (+0x24)


def scenes_running():
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return text.count("stereo: cutscene started") > text.count("stereo: cutscene ended")


def through_scenes(laser):
    """Click through whatever tutorial scene is on (aimed at the cinema screen)."""
    if not scenes_running():
        return
    print("      (a tutorial scene is on; clicking through it)")
    send("hands laser 0", "pose 0 0 0 0 0 0", "ctrl r pose 0 0 0 0.1 -0.3 0.4")
    for _ in range(40):
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.3)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.5)
        if not scenes_running():
            break
    time.sleep(2.0)
    send("pose 0 -40 0 0 0 0", f"hands laser {int(laser)}")


def double_pull():
    for _ in range(2):
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.1)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(0.1)
    time.sleep(1.0)


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
    time.sleep(1)
    on, tied = status()
    if on is None:
        print("SKIP  no leash status in this save")
        return 0
    if not on or tied:
        send("hands leash_on")
        time.sleep(1.5)
        on, tied = status()
    if not on:
        print("SKIP  the leash can't be put on in this save")
        return 0
    for mode in ("hand", "laser"):
        if mode == "hand":
            send("hands laser 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
            results.append(nh.report(place_palm(TREE), "hand: palm in the tree"))
        else:
            send("hands laser 1", f"rig set {TREE2[0]:.2f} {TREE2[1] + 25:.2f} {TREE2[2] - 3:.2f} 0", "ctrl r pose 0 -80 0 0.1 -0.3 0.1")
            time.sleep(1.5)
        time.sleep(1.2)  # (for the hint's delay)
        through_scenes(mode == "laser")
        rig_before = nh.rig_pos()
        mark = nh.LOG.stat().st_size
        double_pull()
        on, tied = status()
        text = tail(mark)
        time.sleep(1.0)
        through_scenes(mode == "laser")
        state, holding, _, _ = nh.hand()
        rig_after = nh.rig_pos()
        results.append(nh.report("hands: leash: tying it to" in text and tied not in (None, 0),
                                 f"{mode}: two quick pulls tied the leash (tied object {tied:#x})"))
        results.append(nh.report(holding == 0, f"{mode}: the tree was not picked up (state {state}, holding {holding})"))
        results.append(nh.report(math.dist(rig_before, rig_after) < 1.0, f"{mode}: no quick travel ({math.dist(rig_before, rig_after):.1f} units)"))
        if mode == "hand":
            hints = re.findall(r"ui: hint '([^']*)' asked for", text)
            print(f"      (hints asked for: {sorted(set(hints))})")
            # Back to the hand: the trigger on the creature (it is at the tree now).
            send(f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
            time.sleep(2.0)
            through_scenes(False)
            send(f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}", "hands follow_creature 1")  # (it walks about the tree)
            time.sleep(0.5)
            send("ctrl r trigger 1")
            time.sleep(0.3)
            send("ctrl r trigger 0", "hands follow_creature 0")
            time.sleep(1.5)
            on, tied = status()
            results.append(nh.report(on == 1 and tied == 0, f"trigger on the creature: untied, back in the hand (on {on}, tied {tied:#x})"))
            send(f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
            time.sleep(1.5)
    send("hands laser 0", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
