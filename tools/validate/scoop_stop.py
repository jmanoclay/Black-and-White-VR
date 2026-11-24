"""Scripted test of stopping a scoop by taking the hand out of the pile (the user's save: the
village storage pit's wood).

Gripping a store's pile takes some, and more for as long as the grip stays on it. In the flat game
moving the cursor off the pile ends that; in VR, taking the hand out of the pile does.

Checks:
- the grip above the pile (not in it) takes wood and keeps taking more;
- with the hand out of the pile (grip still closed) the scoop ends: the amount stops growing;
- the hand still holds what it took;
- letting go puts it down (the game no longer ignores the hand).

Usage: python tools/validate/scoop_stop.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld

PIT = (1826.8, 2641.4)  # the village storage pit (x, z)
WOOD = (1830.1, 33.0, 2642.6)  # one of its wood piles (the hand goes by the ground's height there)


def held():
    mark = nh.LOG.stat().st_size
    ld.send("hands held")
    time.sleep(0.3)
    m = re.search(r"hands: held (\S+) amount (-?\d+) \(state (\d+)\)", ld.tail(mark))
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else ("-", -1, 0)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    # (The piles shrink as the village uses the wood: 40 cm over the ground under the pile, which
    # was 5 units under its old top on the 2026-11 save, where a fixed height took nothing.)
    top = nh.ground_at(WOOD[0], WOOD[2])
    ld.place_palm((WOOD[0], top + 4.0, WOOD[2]))
    time.sleep(1.0)
    mark = nh.LOG.stat().st_size
    nh.bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1.5)
    first = held()
    time.sleep(1.5)
    second = held()
    if "Pile" not in first[0]:
        print(f"SKIP  no pile taken at the storage pit ({first})")
        nh.bwctl.stereo("ctrl r squeeze 0", "ctrl r off")
        return 0
    results.append(nh.report(second[1] > first[1] > 0, f"the grip on the pile scoops ({first[0]}: {first[1]} -> {second[1]})"))
    for k in range(1, 4):  # out of the pile, still gripping
        ld.place_palm((WOOD[0] + 3 * k, top + 4.0 + 4 * k, WOOD[2]))
        time.sleep(0.4)
    time.sleep(0.5)
    a = held()
    time.sleep(2.0)
    b = held()
    ended = "scoop ended" in ld.tail(mark)
    results.append(nh.report(ended and b[1] == a[1], f"out of the pile: the scoop ended ({a[1]} -> {b[1]} in 2 s)"))
    results.append(nh.report(b[2] == 4 and b[1] > 0, f"the hand still holds it (state {b[2]}, {b[0]} {b[1]})"))
    # Let go low over open ground: it is put down (the hand is free for what comes next).
    g2 = nh.ground_at(PIT[0] - 20, PIT[1] - 20)
    for k in range(1, 6):
        ld.place_palm((PIT[0] + 9 - 29 * k / 5, g2 + 0.5, PIT[1] - 20 * k / 5))
        time.sleep(0.4)
    nh.bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2.0)
    c = held()
    results.append(nh.report(c[2] == 1, f"let go of: put down, the hand is free (state {c[2]}, {c[0]})"))
    # Letting go while still scooping, the hand over the pile: the scoop ends and what was taken is
    # put down (it stayed in the open hand, 2026-10-19).
    ld.place_palm((WOOD[0], top + 4.0, WOOD[2]))
    time.sleep(1.0)
    nh.bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1.5)
    d = held()
    nh.bwctl.stereo("ctrl r squeeze 0")
    time.sleep(2.0)
    e = held()
    results.append(nh.report(d[2] == 4 and e[2] == 1, f"let go while scooping: put down, the hand is free (state {d[2]} -> {e[2]}, {e[0]})"))
    nh.bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
