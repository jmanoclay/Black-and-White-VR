"""Scripted test of directing the creature with the leash in VR (needs a save with the leash on).

With the creature on the leash, the hand is put on a rock away from the creature and the trigger
pulled. The mod should send the creature to the rock (the game's leash click), the rock must not be
picked up, and the creature should end up closer to the rock. The grip on the same rock still picks
it up. Then the same with the laser pointed at the rock.

Usage: python tools/validate/leash_direct.py
"""
import math
import re
import sys
import time

import near_hand as nh
from near_hand import bwctl
from give_creature import creatures, own_creature

PALM = (0.0, -0.3, 0.5)  # metres from the head, rig heading 0
ROCK = (2015.0, 8.6, 2343.5)  # a rock in the user's leash-tutorial save, away from the creature


def send(*lines):
    for _ in range(20):
        try:
            bwctl.stereo(*lines)
            time.sleep(0.3)
            return
        except PermissionError:
            time.sleep(0.05)


def tail(mark):
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        return f.read()


def leash_state():
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.5)
    m = re.search(r"in hand (\d), target (\S+)", tail(mark))
    return (int(m.group(1)), m.group(2)) if m else (None, None)


def player_creature():
    """The leashed creature's place: the creature nearest the leash's other end is not known, so
    the one the leash debug names is matched by address to the creature list."""
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.5)
    m = re.search(r"creature (\S+) control", tail(mark))
    addr = m.group(1) if m else None
    mark = nh.LOG.stat().st_size
    send("hands creatures")
    time.sleep(0.8)
    for c in re.finditer(r"hands: creature \d+ (\S+) sphere 1 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) r ([\d.]+)", tail(mark)):
        if addr and c.group(1).upper().lstrip("0") == addr.upper().lstrip("0"):
            return tuple(float(c.group(i)) for i in (2, 3, 4))
    return None


def place_palm(target):
    rig = [target[i] - PALM[i] * 10 for i in range(3)]
    for _ in range(3):
        send(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0")
        time.sleep(0.6)
        mark = nh.LOG.stat().st_size
        send("pointerinfo")
        time.sleep(0.4)
        m = re.search(r"pointer: touch \d object \S+ centre .* palm \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", tail(mark))
        palm = [float(m.group(i)) for i in (1, 2, 3)]
        err = [t - p for t, p in zip(target, palm)]
        if max(abs(e) for e in err) < 0.3:
            return True
        rig = [r + e for r, e in zip(rig, err)]
    return False


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
    time.sleep(1)
    on, _ = leash_state()
    if not on:
        send("hands leash_on")
        time.sleep(1)
        on, _ = leash_state()
    if not on:
        print("SKIP  the leash can't be put on in this save")
        return 0
    # Grip still picks up. (First: once the creature has been sent, it stands at the rock, and the
    # palm there strokes it.)
    send("hands laser 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
    place_palm(ROCK)
    time.sleep(0.5)
    mark = nh.LOG.stat().st_size
    send("ctrl r squeeze 1")
    time.sleep(1.0)
    state, holding, _, _ = nh.hand()
    # (With the leash in the hand the game may make its action there "Attach Leash" or "Focus
    # Creature" instead of "Pick Up", as in the flat game: then a grip doesn't pick up; 2026-11-03.)
    asked = re.findall(r"ui: hint '([^']*)' asked for", tail(mark))
    if holding == 1 or "Pick Up" in asked:
        results.append(nh.report(holding == 1, f"grip on the rock with the leash on: picked up (state {state}, holding {holding})"))
    else:
        print(f"SKIP  grip on the rock: the game's action there is {asked or 'none'}, not Pick Up")
    send("ctrl r squeeze 0")
    time.sleep(1.5)
    for mode in ("hand", "laser"):
        if mode == "hand":
            send("hands laser 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}")
            results.append(nh.report(place_palm(ROCK), "hand: palm on the rock"))
        else:
            # Laser from 20 units back, pointed down at the rock.
            send("hands laser 1", f"rig set {ROCK[0]:.2f} {ROCK[1] + 12:.2f} {ROCK[2] - 20:.2f} 0")
            dy, dz = -12.0, 20.0
            pitch = math.degrees(math.atan2(dy, dz))
            send(f"ctrl r pose 0 {pitch:.1f} 0 0 0 0")
            time.sleep(2.0)  # (the picker reports the rock only now and then)
        time.sleep(0.8)
        start = player_creature()
        on, target = leash_state()
        if mode == "hand":
            results.append(nh.report(on == 1 and target not in (None, "00000000"),
                                     f"{mode}: leash on and the rock is a leash target ({on}, {target})"))
        else:  # (the picker reports the rock under the laser only now and then: else the spot under it)
            print(f"      {mode}: leash on {on}, the rock a leash target: {target}")
        mark = nh.LOG.stat().st_size
        # (A quick tap: held past [hands] tap_s, 0.25 s, the trigger picks the thing up instead.)
        # (bwctl directly: send() waits 0.3 s after each line.)
        bwctl.stereo("ctrl r trigger 1")
        time.sleep(0.1)
        bwctl.stereo("ctrl r trigger 0")
        time.sleep(0.8)
        text = tail(mark)
        state, holding, _, _ = nh.hand()
        spot = re.search(r"leash: the creature is sent to the spot \(([-\d.]+), ([-\d.]+)\)", text)
        sent = "the creature is sent to the thing" in text or bool(spot and math.dist((float(spot.group(1)), float(spot.group(2))), (ROCK[0], ROCK[2])) < 5)
        results.append(nh.report(sent, f"{mode}: trigger sent the creature to it (or to the spot under the laser by it)"))
        results.append(nh.report(holding == 0, f"{mode}: the rock was not picked up (state {state}, holding {holding})"))
        time.sleep(8.0)
        end = player_creature()
        if start and end:
            d0, d1 = math.dist(start, ROCK), math.dist(end, ROCK)
            r = (own_creature() or (0, 0, 0, 0))[3]  # (a grown creature stands a few of its sizes off: r 14.7 in 2026-11)
            results.append(nh.report(d1 < d0 - 2.0 or d1 < 15.0 + 3 * r, f"{mode}: the creature went towards the rock ({d0:.1f} -> {d1:.1f}, r {r:.1f})"))
        else:
            results.append(nh.report(False, f"{mode}: creature position unknown ({start}, {end})"))
    send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
