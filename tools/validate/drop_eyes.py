"""Scripted test: a thing let go of is drawn in the same place by both eyes (the user's save, the
tree near_hand.py uses).

The player saw a dropped or thrown thing vanish from the right eye for a couple of frames as it
left the hand, the left eye showing it all along (2026-10-20). With `stereo drop_log`, each eye's
draw of the held thing and the one let go of is logged with its position, frame by frame.

Check: for the frames after the grip opens, every frame in which the left eye draws the thing,
the right eye draws it too, at the same place (within 0.05 units).

Usage: python tools/validate/drop_eyes.py [throw]   (throw: let go with the hand moving)
"""
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from leash_direct import tail


def frames(text):
    """[(left position or None, right position or None)] per logged frame."""
    out = []
    for block in text.split("drops: frame:")[1:]:
        pos = {0: None, 1: None}
        for m in re.finditer(r"drops: eye (\d) draws (?:held|released) \S+ at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", block):
            pos[int(m.group(1))] = tuple(float(m.group(i)) for i in (2, 3, 4))
        out.append((pos[0], pos[1]))
    return out


def main():
    throw = len(sys.argv) > 1 and sys.argv[1] == "throw"
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0",
                 "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
    time.sleep(1)
    g = nh.ground_at(*nh.TREE)
    tip = (nh.TREE[0], g + 6.0, nh.TREE[1] - 0.5)
    rig = tuple(t - c * nh.SCALE for t, c in zip(tip, nh.CTRL))
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0",
                 f"ctrl r pose 0 -70 0 {nh.CTRL[0]} {nh.CTRL[1]} {nh.CTRL[2]}")
    time.sleep(2.5)
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(1)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1] + 10:.2f} {rig[2]:.2f} 0", "ctrl r pose 0 -30 0 0.1 -0.4 0.45")
    time.sleep(2)
    state, holding, _, _ = nh.hand()
    if not (state == 4 and holding == 1):
        print(f"SKIP  the tree wasn't picked up (state {state})")
        bwctl.stereo("ctrl r off", "hands reach 0.25")
        return 0
    mark = nh.LOG.stat().st_size
    if throw:
        bwctl.stereo("ctrl r vel 0 1.5 2.5")
        time.sleep(0.15)
    bwctl.stereo("drop_log 90")
    time.sleep(0.1)
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(1.5)
    if throw:
        bwctl.stereo("ctrl r vel 0 0 0")
    fr = frames(tail(mark))
    seen_left = [f for f in fr if f[0]]
    missing = [i for i, f in enumerate(fr) if f[0] and not f[1]]
    apart = [i for i, f in enumerate(fr) if f[0] and f[1] and max(abs(a - b) for a, b in zip(f[0], f[1])) > 0.05]
    results.append(nh.report(seen_left and not missing and not apart,
                             f"{'thrown' if throw else 'dropped'}: of {len(fr)} frames, {len(seen_left)} drew it in the left eye; "
                             f"the right eye missed {len(missing)} ({missing[:8]}) and drew it elsewhere in {len(apart)} ({apart[:8]})"))
    for i in (missing + apart)[:4]:
        print("   frame", i, fr[i])
    bwctl.stereo("ctrl r off", "hands reach 0.25")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
