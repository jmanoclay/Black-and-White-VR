"""Scripted test of shaking the hand to drop the leash in VR (needs a save with the leash).

The flat game drops the leash (and a miracle held in the hand) when the mouse is shaken left and
right. With the leash on: a slow sweep of the hand left and right must not count; a quick shake
must, and the leash must come off.

Usage: python tools/validate/leash_shake.py
"""
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import send, tail
from leash_tie import status

BASE = (0.2, 0.3, 0.45)  # (held up: the leashed creature comes to the hand, and a stroke is not shaken)


def sweep(amplitude, step_s, count):
    for i in range(count):
        dx = amplitude if i % 2 == 0 else -amplitude
        bwctl.stereo(f"ctrl r pose 0 0 0 {BASE[0] + dx:.3f} {BASE[1]} {BASE[2]}")
        time.sleep(step_s)
    bwctl.stereo(f"ctrl r pose 0 0 0 {BASE[0]} {BASE[1]} {BASE[2]}")
    time.sleep(1.5)


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -30 0 0 0 0", f"ctrl r pose 0 0 0 {BASE[0]} {BASE[1]} {BASE[2]}")
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
    # Slow: 10 cm each way, 0.4 s per stroke (0.5 m/s).
    mark = nh.LOG.stat().st_size
    sweep(0.10, 0.4, 6)
    on, _ = status()
    results.append(nh.report("hands: shake" not in tail(mark) and on == 1, f"slow sweep: no shake, the leash stays on (on {on})"))
    # Quick: 15 cm each way, 0.08 s per stroke.
    mark = nh.LOG.stat().st_size
    sweep(0.15, 0.08, 8)
    text = tail(mark)
    on, _ = status()
    results.append(nh.report("hands: shake given to the game" in text, "quick shake: detected and given to the game"))
    results.append(nh.report(on == 0, f"quick shake: the leash came off (on {on})"))
    send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
