"""Scripted test: lightning held with the trigger strikes where the controller points (the agreed
"zap" control, documentation/miracles.md; before, in hand mode it struck only straight down under the hand).

Spawns and pops a lightning bubble into the hand, points the controller at a spot on open grass
about 40 units ahead ("ctrl r aimat"), holds the trigger 2.5 s and reads where the game strikes its
bolts (each "a miracle applied where the hand is (x, z)": about 10 a second). With [hands] zap_aim on:
most bolts within 6 units of the spot pointed at (the game took its point through its own flat camera,
17 units short); then the hand comes back (near mode) once the trigger is let go of.
With "compare", also run with zap_aim off first, for the record (the bolts under the hand).

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/lightning_aim.py [compare]
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from miracle_catalogue import pop
from spell_in_hand import state

SPOT = (3150.0, 3350.0)  # (open grass by the temple, as circle_try.py)
BOLT = re.compile(r"a miracle applied where the hand is \(([-\d.]+), ([-\d.]+)\): \.\?AVSpellSeed@@")


def run(aim):
    ld.send(f"hands zap_aim {int(aim)}")
    if not pop(6, SPOT[0] + 40, SPOT[1] - 60):
        return None, f"the lightning bubble pops (state {state()})"
    g = nh.ground_at(*SPOT)
    # Standing 40 units short of the spot, the hand held out in front.
    ld.send("hands laser 0", f"rig set {SPOT[0]} {g + 14} {SPOT[1] - 40} 0", "pose 0 -15 0 0 0 0",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)
    hand = nh.query("pointerinfo", [r"pointer: touch \d object \S+ centre .* palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"])[0]
    palm = (float(hand.group(1)), float(hand.group(3)))
    bwctl.stereo(f"ctrl r aimat {SPOT[0]:.1f} {g:.1f} {SPOT[1]:.1f}")
    time.sleep(0.8)
    mark = nh.LOG.stat().st_size
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(2.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    text = ld.tail(mark)
    bolts = [(float(m.group(1)), float(m.group(2))) for m in BOLT.finditer(text)]
    near = nh.query("pointerinfo", [r"pointer: near (\d)"])[0].group(1) == "1"
    return (bolts, palm, near), None


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    time.sleep(1)
    for aim in ([False, True] if "compare" in sys.argv else [True]):
        got, err = run(aim)
        if err:
            results.append(nh.report(False, err))
            continue
        bolts, palm, near = got
        on_spot = sum(1 for b in bolts if math.dist(b, SPOT) < 6)
        under = sum(1 for b in bolts if math.dist(b, palm) < 10)
        print(f"      zap_aim {int(aim)}: {len(bolts)} bolts, {on_spot} by the spot pointed at, {under} under the hand")
        if aim:
            results.append(nh.report(len(bolts) >= 3 and on_spot >= 0.7 * len(bolts),
                                     f"the bolts strike where the controller points ({on_spot} of {len(bolts)} within 6 units of the spot)"))
            results.append(nh.report(near, "the hand comes back once the trigger is let go of"))
    ld.send("hands zap_aim 1", "ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
