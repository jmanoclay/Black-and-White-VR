"""The other hand grabs too (player, 2026-11-05: the left hand grabbing, holding, throwing, dropping).

The game has one hand; it goes to whichever of the player's hands acts. With the main (right) hand
empty and away, the left palm put on a villager:
  1. the left grip: the god hand goes to the left hand and holds the villager;
  2. the right hand brought within 8 cm of the left and its grip closed: passed across; the left
     grip let go, the villager is still held (by the right);
  3. the right grip let go: dropped;
  4. the left grip on nothing: the god hand stays at the right (it goes to the other hand only to
     take something; player, 2026-11-07).

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot), on the player's Island 2 save.
Usage: python tools/validate/offhand.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
import grab_near_building as gb

LEFT = (0.0, -0.3, 0.5)    # the left controller where the tests' palm usually is
RIGHT = (0.45, -0.3, 0.3)  # the right one out of the way


def holding():
    (m,) = nh.query("handinfo", [r"handinfo: holding (\d)"])
    return m.group(1) == "1"


def said(mark, text):
    return text in ld.tail(mark)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands option offhand_grab 1",
            "ctrl r squeeze 0", "ctrl l squeeze 0", "pose 0 -40 0 0 0 0",
            f"ctrl r pose 0 0 0 {RIGHT[0]} {RIGHT[1]} {RIGHT[2]}", f"ctrl l pose 0 0 0 {LEFT[0]} {LEFT[1]} {LEFT[2]}")
    time.sleep(1.0)
    objs = gb.objects(*gb.VILLAGE, 250)
    vill = [(k, v) for k, v in objs.items() if v[0] == "Villager" and v[3]]
    got = False
    for key, (kind, c, r, lift) in vill[:8]:
        cur = gb.objects(c[0], c[2], 10).get(key)
        if not cur:
            continue
        c = cur[1]
        ld.send(f"rig set {c[0] - LEFT[0] * 10:.2f} {c[1] + 1.0 - LEFT[1] * 10:.2f} {c[2] - LEFT[2] * 10:.2f} 0")
        time.sleep(0.5)
        mark = nh.LOG.stat().st_size
        bwctl.stereo("ctrl l squeeze 1")
        time.sleep(1.0)
        if said(mark, "goes to the left hand") and holding():
            got = True
            break
        bwctl.stereo("ctrl l squeeze 0")
        time.sleep(0.8)
    results.append(nh.report(got, "the left grip on a villager: the god hand at the left, holding it"))
    if got:
        bwctl.stereo_capture("offhand_left")
        mark = nh.LOG.stat().st_size
        ld.send(f"ctrl r pose 0 0 0 {LEFT[0] + 0.08} {LEFT[1]} {LEFT[2]}")
        time.sleep(0.4)
        bwctl.stereo("ctrl r squeeze 1")
        time.sleep(0.5)
        bwctl.stereo("ctrl l squeeze 0")
        time.sleep(0.8)
        results.append(nh.report(said(mark, "passed across") and holding(), "the right grip by it: passed across, still held with the left let go"))
        bwctl.stereo_capture("offhand_passed")
        bwctl.stereo("ctrl r squeeze 0")
        time.sleep(1.0)
        results.append(nh.report(not holding(), "the right grip let go: dropped"))
    # The left grip on nothing, let go: the god hand back at the right.
    ld.send(f"ctrl r pose 0 0 0 {RIGHT[0]} {RIGHT[1]} {RIGHT[2]}", "pose 0 0 0 0 0 0")
    ld.send(f"rig set 3150 {nh.ground_at(3150, 3350) + 40:.1f} 3350 0")
    time.sleep(0.5)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl l squeeze 1")
    time.sleep(0.6)
    bwctl.stereo("ctrl l squeeze 0")
    time.sleep(0.8)
    results.append(nh.report(not said(mark, "goes to the left hand"), "the left grip on nothing: the god hand stays at the right"))
    ld.send("ctrl l off", "ctrl r off")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
