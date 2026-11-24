"""A/B timing in fixed heavy views (through SteamVR with TOUR_OPENVR=1, else the mock headset).

Sets the rig down at each place, looking down and across, waits for two 5-second stats lines, and
prints each eye's pass, the frame, draws and SteamVR's reprojected count. Run the same views before
and after a change, in the same boot state.

Usage: python tools/perf/heavy_view.py <tag> [pitch]
"""
import os
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "validate"))
import near_hand as nh  # noqa: E402
import leash_direct as ld  # noqa: E402

PLACES = [(2400, 3300), (2723, 2061), (3250, 3310)]  # (heavy: across the island; over the village; the temple)


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "view"
    pitch = sys.argv[2] if len(sys.argv) > 2 else "-30"
    ld.send("enabled 1", "vr openvr" if os.environ.get("TOUR_OPENVR") else "vr mock", "freeze 0", "hands near 1", "ctrl l off",
            "ctrl r off", f"pose 0 {pitch} 0 0 0 0", "sway 0 3")
    time.sleep(2.0)
    for x, z in PLACES:
        ld.send(f"rig set {x} 90 {z} 0")
        time.sleep(6.0)  # (settle: textures, the first stats line)
        m = nh.LOG.stat().st_size
        time.sleep(10.5)
        t = ld.tail(m)
        st = re.findall(r"stereo frames (\d+) .*?eye L ([\d.]+) ms, eye R ([\d.]+) ms.*?openvr: \d+ presents, \d+ dropped, (\d+) reprojected", t)
        fr = re.findall(r"\| frame ([\d.]+) ms: wait ([\d.]+)", t)
        dr = re.findall(r"last frame: (\d+) draws", t)
        if not st:
            print(f"{tag} ({x}, {z}): no stats")
            continue
        n = len(st)
        print(f"{tag} ({x}, {z}): eyes {sum(float(s[1]) for s in st) / n:.2f}/{sum(float(s[2]) for s in st) / n:.2f} ms, "
              f"frames {sum(int(s[0]) for s in st) / n:.0f}/5 s, reprojected {sum(int(s[3]) for s in st) / n:.0f}/5 s, "
              f"frame {[f[0] for f in fr]} wait {[f[1] for f in fr]}, draws {dr}")


if __name__ == "__main__":
    main()
