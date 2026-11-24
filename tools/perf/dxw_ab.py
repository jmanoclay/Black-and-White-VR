"""A/B of a switchable saving (default stereo dxw_fast; or: merge) in the same views, the same boot.

Through SteamVR with TOUR_OPENVR=1. At each place: on, off, on, off, 10 s each; each eye's pass
averaged over the two 5-second stats lines.

Usage: python tools/perf/dxw_ab.py [switch [on off]]
"""
import os
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "validate"))
import near_hand as nh  # noqa: E402
import leash_direct as ld  # noqa: E402

SWITCH = sys.argv[1] if len(sys.argv) > 1 else "dxw_fast"
ON, OFF = (sys.argv[2], sys.argv[3]) if len(sys.argv) > 3 else ("1", "0")  # (the values compared)
PLACES = [(2400, 3300), (2723, 2061), (3250, 3310)]


def measure():
    m = nh.LOG.stat().st_size
    time.sleep(10.5)
    st = re.findall(r"stereo frames (\d+) .*?eye L ([\d.]+) ms, eye R ([\d.]+) ms.*?openvr: \d+ presents, \d+ dropped, (\d+) reprojected", ld.tail(m))
    dr = re.findall(r"last frame: (\d+) draws", ld.tail(m))
    if not st:
        return None
    n = len(st)
    return (sum(float(s[1]) for s in st) / n, sum(float(s[2]) for s in st) / n, sum(int(s[3]) for s in st) / n, dr[-1] if dr else "?")


def main():
    ld.send("enabled 1", "vr openvr" if os.environ.get("TOUR_OPENVR") else "vr mock", "freeze 0", "hands near 1", "ctrl l off",
            "ctrl r off", "pose 0 -30 0 0 0 0", "sway 0 3")
    time.sleep(2.0)
    for x, z in PLACES:
        ld.send(f"rig set {x} 90 {z} 0")
        time.sleep(5.0)
        for on in (ON, OFF, ON, OFF):
            ld.send(f"{SWITCH} {on}")
            time.sleep(1.0)
            r = measure()
            mg = re.findall(r"merged: (\d+) draws drawn as (\d+)", ld.tail(nh.LOG.stat().st_size - 4000))
            if r:
                print(f"({x}, {z}) {'on ' if on == ON else 'off'}: eyes {r[0]:.2f}/{r[1]:.2f} ms, reprojected {r[2]:.0f}/5 s, draws {r[3]}")
    ld.send(f"{SWITCH} {ON}")


if __name__ == "__main__":
    main()
