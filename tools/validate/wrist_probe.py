"""Probe: what the game draws at its hand (the leash loop while a leash is only chosen, say).
Logs the leash status and every draw within 8 units of the game's wrist, the hand held up.

Usage: python tools/validate/wrist_probe.py
"""
import re
import sys
import time

import near_hand as nh
from leash_direct import send, tail
from leash_toggle import AWAY


def main():
    send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0",
         "ctrl r a 0", "ctrl r trigger 0", "ctrl r squeeze 0", "pose 0 -40 0 0 0 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1] + 0.3} {AWAY[2]}")
    time.sleep(2)
    # Choose a leash: the trigger on one of the temple's spare leashes (LeashObj), with the laser.
    spare = (1902.88, 34.86, 2528.11) if len(sys.argv) < 4 else tuple(float(v) for v in sys.argv[1:4])
    send("hands laser 1", f"rig set {spare[0] - 8:.1f} {spare[1] + 12:.1f} {spare[2] - 8:.1f} 0", "pose 45 -40 0 0 0 0")
    time.sleep(1.5)
    for dy in (0.0, 0.5, 1.0):
        send(f"ctrl r aimat {spare[0]} {spare[1] + dy:.2f} {spare[2]}")
        time.sleep(0.8)
        m0 = nh.LOG.stat().st_size
        send("ctrl r trigger 1")
        time.sleep(0.15)
        send("ctrl r trigger 0")
        time.sleep(1.5)
        t0 = tail(m0)
        print("   " + " | ".join(l[11:120] for l in t0.splitlines() if "hands:" in l)[:600])
    send("hands laser 0", f"ctrl r pose 0 0 0 {AWAY[0]} {AWAY[1] + 0.3} {AWAY[2]}")
    time.sleep(1.5)
    mark = nh.LOG.stat().st_size
    send("hands leash")
    time.sleep(0.6)
    send("wrist_draws 80", "leash_draws 20")
    time.sleep(1.5)
    t = tail(mark)
    for line in t.splitlines():
        if re.search(r"leash status|leash interface|near the wrist|leash mesh draw|leash loop", line):
            print("   " + line[:200])
    print("SKIP  (probe)")
    send("ctrl r off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
