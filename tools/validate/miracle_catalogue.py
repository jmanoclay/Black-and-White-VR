"""Catalogue: how each of the game's 30 miracles fares with the current VR controls.

For each type (or those given): a bubble is spawned (hands spawn_bubble) on open land, popped into
the hand with the trigger, then tried with the VR controls in turn, stopping at the first that
casts it (the hand no longer charged):
  1. pour: the hand held 1 m over the land, the trigger held 3 s, then let go slowly;
  2. throw: a swing forward with the trigger held, let go at speed;
  3. creature: the palm on the player's creature, a trigger pull;
  4. otherwise it is shaken off.
What the game did along the way (spells made, applied, hand state, the interface's state) is
printed and written, per type, to <out>/type_NN.log.

Usage: python tools/validate/miracle_catalogue.py [out dir] [types...] [x z]
"""
import pathlib
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state
from give_creature import own_creature

NAMES = ("STORM NATURE FIRE FOOD SHIELD PHYSICAL_SHIELD LIGHTNING_BOLT HEAL WOOD WATER FLYING_FLOCK GROUND_FLOCK "
         "C_FREEZE C_SMALL C_BIG C_WEAK C_STRONG C_FAT C_THIN C_INVISIBLE C_COMPASSION C_ANGRY C_HUNGRY C_FRIGHTENED "
         "C_TIRED C_ILL C_THIRSTY C_ITCHY TELEPORT BEAM_EXPLOSION").split()
KEEP = re.compile(r"hands: (trigger|magic hand|spell made|a miracle|a charged|the bubble|shake|the shield)|gesture:|spell \S+ ->|seed:")


def interface():
    m = nh.LOG.stat().st_size
    bwctl.stereo("hands seed")
    time.sleep(0.5)
    k = re.search(r"hand state (\d+), interface flags (\S+)", ld.tail(m))
    return k.groups() if k else ("?", "?")


def pop(t, x, z):
    g = nh.ground_at(x, z)
    ld.send(f"hands spawn_bubble {t} {x} {z}")
    time.sleep(1.5)
    ld.send("hands laser 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(0.4)
    for dy in (2.0, 3.0, 1.0, 4.0):
        ld.place_palm((x, g + dy, z))
        time.sleep(0.7)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.15)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(1.4)
        if state() == 8:
            return True
    return False


def pour(x, z):
    g = nh.ground_at(x, z)
    ld.place_palm((x, g + 10.0, z - 20.0))
    time.sleep(0.6)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(1.5)
    during = interface()
    time.sleep(1.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    return during


def throw(x, z):
    g = nh.ground_at(x, z)
    ld.place_palm((x, g + 30.0, z - 40.0))
    time.sleep(0.6)
    bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} 0.2")
    time.sleep(0.5)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.5)
    for k in range(1, 4):
        bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1] + 0.05 * k:.2f} {0.2 + 0.25 * k:.2f}")
        time.sleep(0.2)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(0.3)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(2.0)


def on_creature():
    c = own_creature()
    if not c:
        return False
    ld.place_palm((c[0], c[1] + c[3] * 0.6, c[2]))
    ld.send("hands follow_creature 1")  # (it may walk off before the trigger)
    time.sleep(1.0)
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(3.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.5)
    ld.send("hands follow_creature 0")
    return True


def shake():
    base = (0.2, 0.3, 0.45)
    for i in range(10):
        dx = 0.15 if i % 2 == 0 else -0.15
        bwctl.stereo(f"ctrl r pose 0 0 0 {base[0] + dx:.3f} {base[1]} {base[2]}")
        time.sleep(0.08)
    bwctl.stereo(f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1.5)


def main():
    args = sys.argv[1:]
    out = pathlib.Path(args.pop(0)) if args and not re.match(r"^-?\d", args[0]) else pathlib.Path("miracle_catalogue")
    nums = [float(a) for a in args]
    x, z = 3262.0, 3300.0
    if len(nums) >= 2 and nums[-1] > 100:
        x, z = nums[-2], nums[-1]
        nums = nums[:-2]
    types = [int(n) for n in nums] or list(range(30))
    out.mkdir(parents=True, exist_ok=True)
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "ctrl r a 0", "ctrl r trigger 0",
            "ctrl r squeeze 0", "ctrl l off", "pose 0 -45 0 0 0 0")
    time.sleep(1.0)
    summary = []
    for t in types:
        mark = nh.LOG.stat().st_size
        if state() == 8:
            shake()
        # (Each type on its own spot: one shaken off falls back to the land as a bubble.)
        tx, tz = x + 25.0 * (t % 6), z + 25.0 * (t // 6)
        if not pop(t, tx, tz):
            row = f"{t:2d} {NAMES[t]:16s} bubble didn't pop (state {state()})"
        else:
            how = "nothing"
            steps = [("pour", lambda: pour(tx, tz)), ("throw", lambda: throw(tx, tz)), ("creature", on_creature)]
            notes = []
            for name, fn in steps:
                r = fn()
                if name == "pour":
                    notes.append(f"while held: state {r[0]} flags {r[1]}")
                if state() != 8:
                    how = name
                    break
            if how == "nothing":
                shake()
                notes.append(f"shaken off: state {state()}")
            text = ld.tail(mark)
            made = re.findall(r"spell made by \S+ -> \d+ \(starts at the palm\).*?spell \S+ (\S+);", text)  # (the hand's own)
            applied = len(re.findall(r"a miracle applied where the hand is", text))
            row = f"{t:2d} {NAMES[t]:16s} cast by: {how:8s} spells made {made}; applied-at-map {applied}; {'; '.join(notes)}"
        text = ld.tail(mark)
        (out / f"type_{t:02d}.log").write_text("\n".join(l for l in text.splitlines() if KEEP.search(l)), encoding="utf-8")
        print(row, flush=True)
        summary.append(row)
    (out / "summary.txt").write_text("\n".join(summary) + "\n")
    bwctl.stereo("ctrl r off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
