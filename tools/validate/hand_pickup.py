"""Scripted interaction test: pick up an object with the laser pointer, carry it, put it down.

Runs on the live game with stereo on and mock controllers. The rig stands near the target, the
head looks at it, and the right controller (held at hip height, offset from the eyes) is aimed at
the target's world position ("ctrl r aimat"), so the pick goes through the full VR path: controller
ray -> island ray cast -> left-eye cursor -> the game's own picking. Trigger = right button.
Checks the game's own state through the mod's handinfo log lines:
  1. grip over the object -> the hand holds it (CHand state 4, interface "holding")
  2. the carried object follows the laser as the controller moves
  3. grip released -> the hand is empty (a tree gets planted where it's dropped)

The default target is a tree on the start island of the test save (trees are pickable early in
the tutorial; villagers aren't yet).
Usage: python tools/validate/hand_pickup.py [x y z]
"""
import math
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402

LOG = bwctl.BWVR / "logs" / "bwvr.log"
DEFAULT_TARGET = (2222.8, 18.5, 3170.9)
HOLDING_STATE = 4
TUG_STATE = 3  # pulling a tree out of the ground comes first


def handinfo():
    """Request one handinfo; returns (state, holding, hand position)."""
    mark = LOG.stat().st_size
    bwctl.stereo("handinfo 1")
    deadline = time.time() + 5
    while time.time() < deadline:
        time.sleep(0.3)
        with open(LOG, encoding="utf-8", errors="replace") as f:
            f.seek(mark)
            text = f.read()
        m1 = re.search(r"handinfo: eye \S+ hand \S+ state (\d+) pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
        m2 = re.search(r"handinfo: holding (\d+)", text)
        if m1 and m2:
            return int(m1.group(1)), int(m2.group(1)), tuple(float(m1.group(i)) for i in (2, 3, 4))
    raise RuntimeError("no handinfo in the log")


def main(argv):
    target = tuple(float(v) for v in argv[1:4]) if len(argv) >= 4 else DEFAULT_TARGET
    # Stand 30 units back and 20 up, looking at the target.
    rig = (target[0] - 12, target[1] + 22, target[2] + 14)
    dx, dy, dz = target[0] - rig[0], target[1] - rig[1], target[2] - rig[2]
    yaw = math.degrees(math.atan2(dx, dz))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands laser 1", "ctrl r trigger 0", "ctrl r squeeze 0",
                 f"rig set {rig[0]} {rig[1]} {rig[2]} 0", f"pose {yaw:.2f} {pitch:.2f} 0 0 0 0",
                 "ctrl r pose 0 0 0 0.2 -0.35 0.15")
    time.sleep(1)
    bwctl.stereo(f"ctrl r aimat {target[0]} {target[1]} {target[2]}")
    time.sleep(2.5)  # the hand glides to the cursor
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1.5)
    state, holding, pos0 = handinfo()
    ok_pick = state in (TUG_STATE, HOLDING_STATE)
    print(f"{'PASS' if ok_pick else 'FAIL'}  grab: state {state} holding {holding}")
    # Carried along the laser: the controller moved 40 cm right and 20 forward swings the laser's
    # point. (Re-aiming with "ctrl r aimat" while holding no longer moved the game's hand, 2026-11-01;
    # a moved controller, as in the headset, does.)
    bwctl.stereo("ctrl r pose 0 0 0 0.6 -0.35 0.35")
    time.sleep(2)
    bwctl.stereo_capture("pickup_carry")
    state, holding, pos1 = handinfo()
    moved = math.hypot(pos1[0] - pos0[0], pos1[2] - pos0[2])
    ok_carry = holding == 1 and state == HOLDING_STATE and moved > 2.0
    print(f"{'PASS' if ok_carry else 'FAIL'}  carry: holding {holding}, hand moved {moved:.2f} units")
    bwctl.stereo("ctrl r squeeze 0")
    ok_drop = False
    for _ in range(8):  # putting it down takes a moment
        time.sleep(0.75)
        state, holding, _ = handinfo()
        ok_drop = holding == 0
        if ok_drop:
            break
    print(f"{'PASS' if ok_drop else 'FAIL'}  put down: state {state} holding {holding}")
    bwctl.stereo("ctrl r off")
    return 0 if ok_pick and ok_carry and ok_drop else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
