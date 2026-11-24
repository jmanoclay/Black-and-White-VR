"""Scripted test of putting the leash on the creature and taking it off in VR (needs a save with
the player's creature and the leash available).

- The hand on the creature and the trigger: the leash comes off; again: it goes back on.
- The same with the laser on the creature.
- A hand brushing the creature (on it long enough for a stroke to begin, then away) must leave
  the leash as it was (the flat game's click on the creature toggles it, and a stroke let go
  too soon was such a click).

Usage: python tools/validate/leash_toggle.py
"""
import math
import re
import sys
import time

import near_hand as nh
from leash_direct import send, tail, leash_state, player_creature, place_palm, PALM
from leash_tie import through_scenes

AWAY = (0.6, 0.3, 0.5)


def leash_on():
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.5)
    m = re.search(r"leash status \S+: (\S+ ){5}(\S+)", tail(mark))
    return int(m.group(2), 16) if m else None


def hand_on_creature():
    """The palm onto the creature, kept there (it may be walking)."""
    through_scenes(False)
    send("hands laser 0", f"ctrl r pose 0 0 0 {PALM[0]} {PALM[1]} {PALM[2]}", "hands follow_creature 1")
    time.sleep(0.5)
    return True


def hand_away():
    send("hands follow_creature 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
    time.sleep(2.0)


def trigger():
    send("ctrl r trigger 1")
    time.sleep(0.3)
    send("ctrl r trigger 0")
    time.sleep(1.0)


def main():
    results = []
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
    time.sleep(1)
    start = leash_on()
    if start is None or not player_creature():
        print("SKIP  no leash status or creature in this save")
        return 0
    state = start
    for attempt in (1, 2):
        ok = hand_on_creature()
        mark = nh.LOG.stat().st_size
        trigger()
        now = leash_on()
        toggled = "hands: leash: toggled on the creature" in tail(mark) or now != state
        results.append(nh.report(ok and now == (0 if state else 1),
                                 f"hand on the creature, trigger: leash {'off' if state else 'on'} ({state} -> {now}, palm placed {ok})"))
        state = now
        hand_away()
    # The laser on the creature.
    through_scenes(False)
    c = player_creature()
    # (From above: the creature may stand in the temple's yard, behind its walls from the side.)
    send("hands laser 1", f"rig set {c[0]:.2f} {c[1] + 30:.2f} {c[2] - 3:.2f} 0")
    time.sleep(0.8)
    send("ctrl r pose 0 -80 0 0.1 -0.3 0.1")
    time.sleep(0.8)
    send("hands laser_to_creature")  # (it walks after the leash's end: aim, and pull at once)
    trigger()
    now = leash_on()
    results.append(nh.report(now == (0 if state else 1), f"laser on the creature, trigger: leash {'off' if state else 'on'} ({state} -> {now})"))
    state = now
    send("hands laser 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
    time.sleep(1.5)
    # Brushing past: on the creature for 0.4 s (a stroke's press comes after 0.25 s), then away.
    for attempt in (1, 2):
        hand_on_creature()
        time.sleep(0.4)
        send("hands follow_creature 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1]} {AWAY[2]}")
        time.sleep(2.5)
        now = leash_on()
        results.append(nh.report(now == state, f"hand brushing the creature (attempt {attempt}): leash unchanged ({state} -> {now})"))
        state = now
    send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
