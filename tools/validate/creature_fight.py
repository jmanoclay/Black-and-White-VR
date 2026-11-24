"""Scripted test of a creature fight (the user's save: an autosave in the tutorial's fight with a
bear; the arena is round the two creatures).

In a fight the game takes its clicks as orders (GInterface's fight action: the left button going
down moves or strikes where the cursor is). The mod's petting, the leash's sending and toggling and
the grip's reach either clicked by themselves or swallowed the player's clicks (player,
2026-10-23). In a fight the trigger is now the game's left button.

Checks:
- the fight is recognised;
- laser: the trigger on a spot in the arena gives the game a fight order there, and the mod
  doesn't send the creature on the leash itself;
- hand mode: the trigger with the controller pointing into the arena gives an order there;
- hand mode: the palm resting inside the player's creature clicks nothing by itself.

Usage: python tools/validate/creature_fight.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl


def creatures():
    m = nh.LOG.stat().st_size
    bwctl.stereo("hands creatures")
    time.sleep(0.8)
    return [tuple(float(c.group(i)) for i in (1, 2, 3)) for c in
            re.finditer(r"hands: creature \d+ \S+ sphere 1 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", ld.tail(m))]


def trigger():
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.25)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off", "hands fight_log 2000")
    time.sleep(1)
    # (The save can open on the fight's introduction, a scene on the cinema screen: clicked through
    # until the fight starts.)
    for _ in range(20):
        if "creature fight started" in open(nh.LOG, encoding="utf-8", errors="replace").read():
            break
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.3)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(2.5)
    time.sleep(1)
    if "creature fight started" not in open(nh.LOG, encoding="utf-8", errors="replace").read():
        print("SKIP  no creature fight in this save")
        return 0
    cs = creatures()
    if len(cs) < 2:
        print("SKIP  not in the fight (fewer than two creatures)")
        return 0
    mid = ((cs[0][0] + cs[1][0]) / 2, (cs[0][2] + cs[1][2]) / 2)
    spot = (mid[0] + 4, mid[1] + 8)
    gy = nh.ground_at(*spot)
    # Laser.
    bwctl.stereo("hands laser 1", f"rig set {mid[0] + 30:.1f} {gy + 25:.1f} {mid[1] - 25:.1f} 0", "pose -50 -25 0 0 0 0")
    time.sleep(1.5)
    bwctl.stereo(f"ctrl r aimat {spot[0]:.1f} {gy:.1f} {spot[1]:.1f}")
    time.sleep(1.0)
    m = nh.LOG.stat().st_size
    trigger()
    t = ld.tail(m)
    results.append(nh.report("creature fight started" in open(nh.LOG, encoding="utf-8", errors="replace").read(), "the fight is recognised"))
    order = re.search(r"fight: (select|apply) at \(([-\d.]+), ([-\d.]+)\)", t)
    results.append(nh.report(bool(order) and "leash: sending" not in t,
                             f"laser: the trigger in the arena orders the fight there ({order.group(0) if order else 'no order'}; "
                             f"{'the mod also sent the creature' if 'leash: sending' in t else 'no sending of its own'})"))
    # Hand mode, the controller pointing into the arena.
    bwctl.stereo("hands laser 0")
    time.sleep(1.5)
    bwctl.stereo(f"ctrl r aimat {spot[0] - 6:.1f} {gy:.1f} {spot[1] - 4:.1f}")
    time.sleep(1.0)
    m = nh.LOG.stat().st_size
    trigger()
    t = ld.tail(m)
    order = re.search(r"fight: (select|apply) at \(([-\d.]+), ([-\d.]+)\)", t)
    results.append(nh.report(bool(order), f"hand: the trigger pointing into the arena orders the fight there ({order.group(0) if order else 'no order'})"))
    # The trigger held on the ground is one press, not a stream of them (player, 2026-10-24).
    for mode in ("hand", "laser"):
        bwctl.stereo(f"hands laser {1 if mode == 'laser' else 0}")
        time.sleep(1.5)
        bwctl.stereo(f"ctrl r aimat {spot[0] - 6:.1f} {gy:.1f} {spot[1] - 4:.1f}")
        time.sleep(1.0)
        m = nh.LOG.stat().st_size
        bwctl.stereo("ctrl r trigger 1")
        # (The hand drifts while held, as a real one does.)
        for k in range(6):
            time.sleep(0.4)
            bwctl.stereo(f"ctrl r aimat {spot[0] - 6 + k:.1f} {gy:.1f} {spot[1] - 4 + k * 0.5:.1f}")
        time.sleep(0.3)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.0)
        t = ld.tail(m)
        downs = re.findall(r"fight action: flags (0[123])", t)
        orders = re.findall(r"fight: (?:select|apply) at", t)
        results.append(nh.report(len(downs) == 1 and len(orders) <= 1,
                                 f"{mode}: the trigger held on the ground, the hand drifting, is one order ({len(downs)} presses, {len(orders)} orders)"))
    bwctl.stereo("hands laser 0")
    time.sleep(1.0)
    # Hand mode, the palm resting in the player's creature: no clicks by themselves.
    own = ld.player_creature() or cs[0]
    ld.place_palm(own)
    time.sleep(0.5)
    m = nh.LOG.stat().st_size
    time.sleep(3)
    t = ld.tail(m)
    clicks = len(re.findall(r"fight action: flags 0[12]", t)) + len(re.findall(r"petting: action button down", t))
    results.append(nh.report(clicks == 0, f"hand: the palm resting in the creature clicks nothing by itself ({clicks} presses in 3 s)"))
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
