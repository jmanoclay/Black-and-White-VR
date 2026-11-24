"""Scripted test of a hand-over the creature doesn't finish (the user's save: a pig from the pen).

A carried thing held up to the creature is handed over (packet 0x1F), and the grip counts as let go
of. If the player then takes the hand away before the creature has it, the game drops the give,
but the thing stayed in the game's hand: it hung on the open hand until the grip closed and opened
again (player, 2026-10-17). It is now put down once the give has ended and the grip is open.

Checks (SKIP when no pig can be caught):
- the hand-over is sent, and the give ends without the creature finishing it;
- the pig is put down: the hand is free.

Usage: python tools/validate/give_pulled_away.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
import give_pig as gp


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    on, _ = ld.leash_state()
    if on:
        ld.send("hands leash_off")
        time.sleep(0.5)
    holding = 0
    for px, pz in gp.objects(gp.PIGS[0], gp.PIGS[1], 40, "Pig")[:6]:
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
    for _ in range(16):
        c = ld.player_creature()
        if c:
            ld.place_palm((c[0], c[1] + 1.0, c[2]))
        time.sleep(0.4)
        if "hands: handing" in ld.tail(mark):
            break
    c = ld.player_creature()  # (pulled away at once, up high, and let go)
    ld.place_palm((c[0] + 60, c[1] + 30, c[2] + 60))
    time.sleep(0.2)
    nh.bwctl.stereo("ctrl r squeeze 0")
    time.sleep(8)
    state, holding, _, _ = nh.hand()
    text = ld.tail(mark)
    ended = "hands: handing" in text and re.search(r"giving: (the game finished \(or dropped\)|gave up)", text) and "the creature has it" not in text
    results.append(nh.report(bool(ended), "the hand-over was sent and ended without the creature finishing it"))
    by = "the mod" if "not taken: put down" in text else "the game"
    results.append(nh.report(holding == 0, f"the pig is put down ({by}), the hand is free (state {state}, holding {holding})"))
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
