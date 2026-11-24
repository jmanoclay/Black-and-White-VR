"""The island tour: frame spikes as new land is first travelled (player, 2026-10-30).

Run on a fresh boot (the land not yet seen this session). Sets the rig down in ten places around
the island, 4 s apart, looking down and across, and counts the spike catcher's lines
("stereo: spike") and the file opens / profile reads the mod logs. Copies captures/spikes.txt to
captures/tour_<tag>.txt for tools/perf/profile_report.py.

(TOUR_OPENVR=1: through SteamVR, the headset on the desk; else the mock headset.)
Usage: python tools/perf/island_tour.py <tag> [wait_s] [rounds] [pitch] [sway degrees]
"""
import os
import pathlib
import re
import shutil
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "validate"))
import near_hand as nh  # noqa: E402
import leash_direct as ld  # noqa: E402

PLACES = [(3250, 3310), (2723, 2061), (2222, 3170), (1759, 2964), (2015, 2343), (2660, 1970), (3000, 2600), (2500, 2700), (1900, 2600), (2400, 3300)]


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "tour"
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
    spikes = nh.bwctl.CAPTURES / "spikes.txt"
    if spikes.exists():
        spikes.unlink()
    ld.send("enabled 1", "vr openvr" if os.environ.get("TOUR_OPENVR") else "vr mock", "freeze 0", "hands near 1", "ctrl l off", "ctrl r off", f"pose 0 {sys.argv[4] if len(sys.argv) > 4 else -30} 0 0 0 0", "spike_ms 15", f"sway {sys.argv[5] if len(sys.argv) > 5 else 0} 3")
    time.sleep(2.0)
    mark = nh.LOG.stat().st_size
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    for r in range(rounds):
        m0 = nh.LOG.stat().st_size
        for x, z in PLACES:
            ld.send(f"rig set {x} 90 {z} 0")
            time.sleep(wait)
        time.sleep(5.5)  # (the last 5-second stats)
        if rounds > 1:
            st = re.findall(r"stereo frames (\d+) .*?eyes ([\d.]+)/([\d.]+).*?longest ([\d.]+) ms", ld.tail(m0))
            print(f"round {r + 1}: frames per 5 s {[int(a) for a, *_ in st]}; left eye {[float(b) for _, b, *_ in st]}; longest {[float(d) for *_, d in st]}")
    t = ld.tail(mark)
    sp = re.findall(r"stereo: spike: part (\d+) of frame \d+ lasted ([\d.]+) ms", t)
    by_part = {}
    for p, ms in sp:
        by_part.setdefault(p, []).append(float(ms))
    opens = re.findall(r"load: (\d+) file opens \(([\d.]+) ms\), (\d+) profile reads \(([\d.]+) ms\)", t)
    reproj = re.findall(r"(\d+) reprojected", t)
    print(f"{tag}: {len(sp)} spikes over 15 ms; by part: " + ", ".join(f"{p}: {len(v)} (max {max(v):.0f} ms)" for p, v in sorted(by_part.items())))
    print(f"   file opens {sum(int(o[0]) for o in opens)} ({sum(float(o[1]) for o in opens):.0f} ms), "
          f"profile reads {sum(int(o[2]) for o in opens)} ({sum(float(o[3]) for o in opens):.0f} ms)")
    slow = re.findall(r"load: (open|profile \w+) (\S+): ([\d.]+) ms", t)
    for w, n, ms in slow[:15]:
        print(f"   slow {w} {n}: {ms} ms")
    longest = re.findall(r"longest ([\d.]+) ms", t)
    print(f"   longest frame per 5 s: {longest}")
    if spikes.exists():
        shutil.copy(spikes, nh.bwctl.CAPTURES / f"tour_{tag}.txt")


if __name__ == "__main__":
    main()
