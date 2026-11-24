"""Scripted test: the trigger picks up and carries, for controllers without a grip (player,
2026-10-23: design for a trigger alone; the grip is a shortcut).

Runs on whatever save is loaded: it looks for a tree with the laser near the given spot (default:
by the village, where the player's saves have trees), afresh before each try (things moved by
earlier tries move). The grip on the same tree is the control: where the grip can't pick a tree
up (a scene, a fight), the trigger isn't expected to either.

Checks:
- laser: a quick trigger tap on a tree doesn't carry it;
- laser: the trigger held picks a tree up wherever the grip does (3 tries each);
- hand: the trigger held with the palm in a tree's crown picks it up; let go, it leaves the hand.

Usage: python tools/validate/trigger_carry.py [x z]
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl


def state():
    return nh.hand()[0]


def picked_class():
    m = nh.LOG.stat().st_size
    bwctl.stereo("pointerinfo")
    time.sleep(0.5)
    k = re.search(r"picked (\S+) (\S+)", ld.tail(m))
    return k.group(2) if k else "-"


def find_tree(pts, gs):
    for p in pts:
        for dy in (3, 6):
            bwctl.stereo(f"ctrl r aimat {p[0]} {gs[p] + dy:.1f} {p[1]}")
            time.sleep(0.35)
            if picked_class() == ".?AVTree@@":
                return (p[0], gs[p] + dy, p[1])
    return None


def hold(btn, tree):
    bwctl.stereo(f"ctrl r aimat {tree[0]} {tree[1]:.1f} {tree[2]}")
    time.sleep(1.0)
    bwctl.stereo(f"ctrl r {btn} 1")
    time.sleep(1.5)
    held = state()
    bwctl.stereo(f"ctrl r {btn} 0")
    time.sleep(1.5)
    return held, state()


def main():
    cx, cz = (float(sys.argv[1]), float(sys.argv[2])) if len(sys.argv) > 2 else (2460.0, 2590.0)
    results = []
    pts = [(cx + dx, cz + dz) for dx in range(-100, 101, 20) for dz in range(-60, 61, 20)]
    gs = {p: nh.ground_at(*p) for p in pts}
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 1", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "hands fight_mode 0", "hands trigger_carries 1",
            f"rig set {cx} {max(gs.values()) + 40:.1f} {cz - 60} 0", "pose 0 -40 0 0 0 0")
    time.sleep(1.5)
    tree = find_tree(pts, gs)
    if not tree:
        print("SKIP  no tree found near the spot")
        return 0
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.12)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.0)
    results.append(nh.report(state() != 4, f"laser: a quick trigger tap on the tree doesn't carry it (hand state {state()})"))
    grip_ok = trig_ok = 0
    tries = []
    for _ in range(3):
        tree = find_tree(pts, gs)
        if not tree:
            break
        t = hold("trigger", tree)
        tree = find_tree(pts, gs)
        if not tree:
            tries.append(f"trigger {t}, no tree left for the grip")
            trig_ok += t[0] == 4
            break
        g = hold("squeeze", tree)
        grip_ok += g[0] == 4
        trig_ok += t[0] == 4
        tries.append(f"grip {g}, trigger {t}")
    detail = "; ".join(tries) or "no tree found again"
    if grip_ok == 0 and trig_ok == 0:
        # The grip can't lift anything here either (a fight or a scene locks the hand).
        print(f"SKIP  laser: neither the grip nor the trigger can pick a tree up here ({detail})")
    else:
        results.append(nh.report(trig_ok >= grip_ok,
                                 f"laser: the trigger held picks a tree up as often as the grip ({trig_ok} and {grip_ok}: {detail})"))
    # Hand mode: palm in the crown, trigger held.
    tree = find_tree(pts, gs)
    if not tree:
        print("SKIP  hand: no tree found again for the hand")
        bwctl.stereo("ctrl r off", "hands reach 0.25", "hands fight_mode 1", "hands laser 0")
        return 0 if all(results) else 1
    bwctl.stereo("hands laser 0", "hands reach 1.0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)

    def in_crown(btn):
        for dy in (4.0, 6.0, 2.0):
            ld.place_palm((tree[0], tree[1] - 3 + dy, tree[2]))
            time.sleep(0.6)
            bwctl.stereo(f"ctrl r {btn} 1")
            time.sleep(1.5)
            took = state() == 4
            bwctl.stereo(f"ctrl r {btn} 0")
            time.sleep(1.5)
            if took:
                return dy
        return None

    dy = in_crown("trigger")
    if dy is not None:
        results.append(nh.report(state() != 4, f"hand: the trigger held in the crown picks the tree up (palm +{dy}), let go it leaves the hand (state {state()})"))
    elif in_crown("squeeze") is None:
        print(f"SKIP  hand: neither the trigger nor the grip picks the tree at {tree} up")
    else:
        results.append(nh.report(False, "hand: the grip picks the tree up in the crown but the trigger doesn't"))
    bwctl.stereo("ctrl r off", "hands reach 0.25", "hands fight_mode 1", "hands laser 0")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
