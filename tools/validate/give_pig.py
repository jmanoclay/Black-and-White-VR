"""Scripted hand-over of a pig to the player's creature, with the leash in the hand (the user's save).

A pig is picked up, then held up to the creature (the palm put on it again every half second: on
the leash it follows the hand). After the hold time the mod asks the creature whether it can take
the pig and sends the game's give (packet 0x1F); the creature comes and reaches for the game's
hand, which goes to it, and the game finishes the give (or, failing that, the mod: packet 0x52).

Checks (SKIP when no pig could be caught):
- the hand-over is sent;
- the creature reached for it and the give was finished;
- the hand is empty afterwards, without the pig having been dropped (no launch).

Usage: python tools/validate/give_pig.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld

PIGS = (1645.0, 2615.0)  # where the save's pigs graze (x, z)


def objects(x, z, r, cls):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands objects {x} {z} {r}")
    time.sleep(0.8)
    return [(float(m.group(1)), float(m.group(2))) for m in
            re.finditer(r"hands: object \S+ \.\?AV" + cls + r"@@ at \(([-\d.]+), \+[-\d.]+, ([-\d.]+)\)", ld.tail(mark))]


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}", "hands give_log 1")
    time.sleep(1)
    on, _ = ld.leash_state()
    if not on:
        ld.send("hands leash_on")
        time.sleep(1)
    holding = 0
    for px, pz in objects(PIGS[0], PIGS[1], 40, "Pig")[:6]:
        g = nh.ground_at(px, pz)
        ld.place_palm((px, g + 1.0, pz))
        nh.bwctl.stereo("ctrl r squeeze 1")
        time.sleep(1.2)
        holding = nh.hand()[1]
        if holding:
            break
        nh.bwctl.stereo("ctrl r squeeze 0")
        time.sleep(0.5)
    if not holding:
        print("SKIP  no pig could be caught")
        ld.send("ctrl r off")
        return 0
    mark = nh.LOG.stat().st_size
    for _ in range(16):  # (on the leash the creature follows the hand: keep putting the palm on it)
        c = ld.player_creature()
        if c:
            ld.place_palm((c[0], c[1] + 1.0, c[2]))
        time.sleep(0.5)
        if re.search(r"hands: giving: (the creature has it: finished|the game finished)", ld.tail(mark)):
            break
    time.sleep(1.5)
    state, holding, _, _ = nh.hand()
    text = ld.tail(mark)
    results.append(nh.report("hands: handing .?AVPig@@ to the creature" in text, "the hand-over was sent (packet 0x1F)"))
    results.append(nh.report(re.search(r"hands: giving: (the creature has it: finished|the game finished)", text) is not None,
                             "the give was finished (by the game, or by the mod once the creature reached for it)"))
    results.append(nh.report(holding == 0 and "hands: launch:" not in text, f"the hand is empty and the pig wasn't dropped (state {state}, "
                                                                           f"holding {holding})"))
    nh.bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.5)
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
