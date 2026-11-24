"""The warning when the hand leaves the player's influence (the mana path back to the ring, and its sound).

GVirtualInfluence keeps the last point the hand was inside the influence and an amount that fades
with the distance outside; while it is up and the hand is outside, its Draw shows the path and
plays the sound (flag +0x3C). The mod gives it the ground below the palm (hand mode) or the laser's
land hit (laser mode); the game's own point, from the game camera, was hundreds of units off.
In the test save the influence ends near x 2650 along z 2500 (inside to the west).
  1. hand mode: walking out from x 2450 to 2775, the warning comes on past the edge and is still
     up 100 units out (the game's point jumped away there and the warning died at x 2750);
  2. laser mode, the head looking off to the side: sweeping the laser out over the edge, the
     warning comes on and stays up to 75 units out.
Usage: python tools/validate/influence_warning.py [mod 0|1]   (0: the game's own point, to compare)
"""
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
sys.path.insert(0, str(ROOT / "tools" / "validate"))
import bwctl  # noqa: E402
import near_hand as nh  # noqa: E402

Z = 2500.0
EDGE_X = 2650.0


def vinfl():
    """(amount, warning on) from one 'hands vinfl' line."""
    (m,) = nh.query("hands vinfl 1", [r"hands: vinfl amount ([\d.]+) sound (\d)"])
    return float(m.group(1)), m.group(2) == "1"


def report(ok, text):
    print(("PASS " if ok else "FAIL ") + text)
    return ok


def main(argv):
    mod = argv[1] if len(argv) > 1 else "1"
    xs = list(range(2450, 2776, 25))
    gs = {x: nh.ground_at(x, Z) for x in xs}
    ok = True

    # 1. Hand mode: the rig walks out, the hand in front of it.
    bwctl.stereo(f"hands vinfl_mod {mod}", "hands laser 0", f"rig set 2450 {gs[2450] + 15:.1f} {Z} 0", "pose 90 0 0 0 0 0",
                 "ctrl r pose 0 -20 0 0 -0.4 0.45")
    time.sleep(3)
    first_on, amounts = None, {}
    for x in xs:
        bwctl.stereo(f"rig set {x} {gs[x] + 15:.1f} {Z} 0")
        time.sleep(0.4)
        amount, on = vinfl()
        amounts[x] = amount
        if on and first_on is None:
            first_on = x
    print(f"  hand: amounts {' '.join(f'{x}:{a:.2f}' for x, a in amounts.items())}; on from {first_on}")
    ok &= report(first_on is not None and EDGE_X - 25 <= first_on <= EDGE_X + 75,
                 f"hand mode: the warning comes on past the edge (x {first_on}, edge ~{EDGE_X:.0f})")
    ok &= report(amounts[2750] > 0.5, f"hand mode: still up 100 units out (amount {amounts[2750]:.2f} at x 2750)")

    # 2. Laser mode from inside, the head turned 90 degrees away from the laser.
    bwctl.stereo("hands laser 1", f"rig set 2450 {max(gs.values()) + 60:.1f} {Z} 0", "pose 0 -30 0 0 0 0", "ctrl r pose 0 0 0 0.2 -0.35 0.15")
    time.sleep(1.0)
    first_on, amounts = None, {}
    for x in xs[:-2]:
        bwctl.stereo(f"ctrl r aimat {x} {gs[x]:.1f} {Z}")
        time.sleep(0.4)
        amount, on = vinfl()
        amounts[x] = amount
        if on and first_on is None:
            first_on = x
    print(f"  laser: amounts {' '.join(f'{x}:{a:.2f}' for x, a in amounts.items())}; on from {first_on}")
    ok &= report(first_on is not None and EDGE_X - 25 <= first_on <= EDGE_X + 50,
                 f"laser mode, head turned away: the warning comes on past the edge (x {first_on})")
    ok &= report(amounts[2725] > 0.5, f"laser mode: still up 75 units out (amount {amounts[2725]:.2f})")
    bwctl.stereo("hands vinfl_mod 1", "hands laser 0")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
