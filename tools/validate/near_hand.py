"""Scripted test of the near ("direct") hand with mock controllers on the live game.

1. The controller is held ~60 cm above the ground beside a tree (standing height): the god hand is at the
   controller (pointerinfo: near 1, blend 1) and the game's hand stays under it.
2. Grip + a sideways pull uproots the tree (CHand state 3 -> 4, holding).
3. Moving the controller while carrying moves the tree by the same amount as the drawn hand.
4. Grip released: the hand lets go (holding 0).
5. The hand stays at the controller however high it is raised; A toggles the laser on and off.
6. The other hand's stick moves the rig forward; a double trigger pull in laser mode travels to the pointed spot.

Usage: python tools/validate/near_hand.py
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
TREE = (2222.8, 3170.9)  # the test save's start-island tree (x, z)
CTRL = (0.1, -0.35, 0.25)  # mock controller offset from the head, metres
SCALE = 10.0


def query(cmd, patterns, timeout=5):
    mark = LOG.stat().st_size
    bwctl.stereo(cmd)
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.3)
        with open(LOG, encoding="utf-8", errors="replace") as f:
            f.seek(mark)
            text = f.read()
        found = [re.search(p, text) for p in patterns]
        if all(found):
            return found
    raise RuntimeError(f"no answer to {cmd!r}")


def num(m, i):
    return float(m.group(i))


def pointer():
    (m,) = query("pointerinfo", [r"pointer: near (\d) blend ([\d.]+) dragging \d wrist \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"])
    return int(m.group(1)), num(m, 2), (num(m, 3), num(m, 4), num(m, 5))


def hand():
    m1, m2 = query("handinfo 1", [r"handinfo: eye \S+ hand \S+ state (\d+) pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)",
                                  r"handinfo: holding (\d+)"])
    held = None
    with open(LOG, encoding="utf-8", errors="replace") as f:
        tail = f.read()[-4000:]
    m3 = list(re.finditer(r"handinfo: held object at .*moved to \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", tail))
    if m3:
        held = tuple(float(m3[-1].group(i)) for i in (1, 2, 3))
    return int(m1.group(1)), int(m2.group(1)), (num(m1, 2), num(m1, 3), num(m1, 4)), held


def ground_at(x, z):
    (m,) = query(f"rig set {x} 40 {z} 0\nrig info", [r"rig: ground under the rig ([-\d.]+)"])
    return float(m.group(1))


def rig_pos():
    (m,) = query("rig info", [r"rig: pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"])
    return num(m, 1), num(m, 2), num(m, 3)


def report(ok, text):
    print(f"{'PASS' if ok else 'FAIL'}  {text}")
    return ok


def main():
    results = []
    bwctl.stereo("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "hands reach 1.0", "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0")
    time.sleep(1)
    g = ground_at(*TREE)
    # Controller tip ~60 cm above the ground beside the tree (standing height), pointing down.
    tip = (TREE[0], g + 6.0, TREE[1] - 0.5)
    rig = (tip[0] - CTRL[0] * SCALE, tip[1] - CTRL[1] * SCALE, tip[2] - CTRL[2] * SCALE)
    bwctl.stereo(f"rig set {rig[0]:.2f} {rig[1]:.2f} {rig[2]:.2f} 0", "pose 0 -50 0 0 0 0",
                 f"ctrl r pose 0 -70 0 {CTRL[0]} {CTRL[1]} {CTRL[2]}")
    time.sleep(2.5)
    near, blend, wrist = pointer()
    state, holding, hand_pos, _ = hand()
    under = math.hypot(hand_pos[0] - tip[0], hand_pos[2] - tip[2])
    results.append(report(near == 1 and blend > 0.99, f"near mode on (near {near}, blend {blend:.2f})"))
    # The game hovers its hand partway along the line of sight to the ground point.
    results.append(report(under < 4.0, f"game hand under the controller ({under:.2f} units away horizontally)"))
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(2)
    for x in (0.0, -0.1, -0.2, -0.3):
        bwctl.stereo(f"ctrl r pose -20 -50 0 {x} -0.28 0.15")
        time.sleep(0.4)
    time.sleep(2)
    state, holding, _, _ = hand()
    results.append(report(state == 4 and holding == 1, f"uprooted and holding (state {state}, holding {holding})"))
    bwctl.stereo("ctrl r pose 0 10 0 0.05 -0.05 0.45")
    time.sleep(2.5)
    _, _, wrist0 = pointer()
    _, _, _, held0 = hand()
    bwctl.stereo("ctrl r pose 0 10 0 0.25 0.0 0.45")  # move the hand 20 cm right, 5 cm up
    time.sleep(2.5)
    _, _, wrist1 = pointer()
    state, holding, _, held1 = hand()
    if held0 and held1:
        hand_move = [b - a for a, b in zip(wrist0, wrist1)]
        tree_move = [b - a for a, b in zip(held0, held1)]
        err = math.dist(hand_move, tree_move)
        moved = math.dist(hand_move, (0, 0, 0))
    else:
        err, moved = float("inf"), 0
    # The game lets a carried object swing a little behind its hand; that lag stays. Its offset
    # from the wrist also follows the drawn hand's size, which the game varies with distance.
    results.append(report(holding == 1 and moved > 1.0 and err < 1.5,
                          f"carried tree follows the drawn hand (hand moved {moved:.2f}, tree off by {err:.2f} units)"))
    # A while carrying: the hand goes out along the laser with the tree, and comes back with it.
    bwctl.stereo("ctrl r pose 0 -40 0 0.25 0.0 0.45")
    time.sleep(1)
    switched = []
    for want in (0, 1):
        bwctl.stereo("ctrl r a 1")
        time.sleep(0.5)
        bwctl.stereo("ctrl r a 0")
        time.sleep(1.5)
        near, _, _ = pointer()
        _, holding, _, _ = hand()
        switched.append(near == want and holding == 1)
    results.append(report(all(switched), f"A while carrying sends the hand out and recalls it, still holding ({switched})"))
    bwctl.stereo("ctrl r squeeze 0")
    released = False
    for _ in range(8):
        time.sleep(0.75)
        state, holding, _, _ = hand()
        if holding == 0:
            released = True
            break
    results.append(report(released, f"let go (state {state}, holding {holding})"))
    bwctl.stereo("ctrl r pose 0 -10 0 0.1 0.7 0.3")  # raise the hand well above the ground
    time.sleep(2)
    near, blend, _ = pointer()
    results.append(report(near == 1, f"still the hand when raised (near {near}, blend {blend:.2f})"))
    # The grip grabs nothing there -- unless the raised hand touches something (the tree just
    # dropped): touch grabbing has its own test, touch_grab.py.
    (m,) = query("pointerinfo", [r"pointer: touch (\d)"])
    touching = m.group(1) == "1"
    bwctl.stereo("ctrl r squeeze 1")
    time.sleep(1)
    m = query("pointerinfo", [r"reach (\d)", r"rmb (\d)"])
    results.append(report(m[0].group(1) == "0" and (touching or m[1].group(1) == "0"),
                          "out of reach of the ground: the grip grabs nothing" + (" (skipped: touching an object)" if touching else "")))
    bwctl.stereo("ctrl r squeeze 0")
    time.sleep(0.5)
    bwctl.stereo("ctrl r a 1")
    time.sleep(0.5)
    bwctl.stereo("ctrl r a 0")
    time.sleep(1)
    near, blend, _ = pointer()
    results.append(report(near == 0 and blend < 0.01, f"A turns the laser on (near {near}, blend {blend:.2f})"))

    # Quick travel: double trigger pull while the laser points at the ground ahead.
    # From a known open spot: the laser lands on flat ground ~28 units ahead (+z).
    bwctl.stereo("rig set 2231 30 3135 0", "ctrl r pose 0 -25 0 0.1 -0.1 0.3")
    time.sleep(1)
    before = rig_pos()
    (m,) = query("rig info", [r"rig: ground under the rig ([-\d.]+)"])
    ground_before = float(m.group(1))
    for v in (1, 0, 1):
        bwctl.stereo(f"ctrl r trigger {v}")
        time.sleep(0.12)
    time.sleep(1.5)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(0.5)
    after = rig_pos()
    (m,) = query("rig info", [r"rig: ground under the rig ([-\d.]+)"])
    ground_after = float(m.group(1))
    went = math.hypot(after[0] - before[0], after[2] - before[2])
    # The glide stops 10 units short of the spot, no higher above the ground than before (it comes
    # down to at most travel_height there; on the current save the ground there is higher, so the
    # rig may rise with it).
    above = (before[1] - ground_before, after[1] - ground_after)
    results.append(report(went > 8.0 and after[2] > before[2] and above[1] < above[0] + 0.5,
                          f"double trigger pull travels along the laser ({went:.1f} units; {above[0]:.1f} -> {above[1]:.1f} above the ground)"))

    bwctl.stereo("ctrl r a 1")
    time.sleep(0.5)
    bwctl.stereo("ctrl r a 0")
    time.sleep(1)
    near, blend, _ = pointer()
    results.append(report(near == 1, f"A turns the laser off again (near {near}, blend {blend:.2f})"))

    # Stick movement: forward is where the head faces (+z here), at a speed set by scale and height.
    before = rig_pos()
    bwctl.stereo("ctrl l pose 0 0 0 -0.2 -0.3 0.3", "ctrl l stick 0 1")
    time.sleep(1.0)
    bwctl.stereo("ctrl l stick 0 0")
    time.sleep(0.5)
    after = rig_pos()
    dz, dx = after[2] - before[2], after[0] - before[0]
    results.append(report(dz > 5.0 and abs(dx) < 0.2 * dz, f"left stick moves forward ({dz:.1f} units in ~1 s, {dx:.1f} sideways)"))
    bwctl.stereo("ctrl l off")
    bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
