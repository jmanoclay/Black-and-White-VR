"""Reproduce the transient eye mismatch after a head-pose jump, with a D3D trace of the same frame.

Assumes the game is running in gameplay with stereo enabled. Jumps the mock head pose from an
offset pose back to identity with IPD 0, traces and captures that very frame, then reports which
draws differ between the two eye passes (by full vertex-array hash) and the identity metric.

Usage: python tools/validate/repro_pose_jump.py [name] [delay_seconds]
"""
import json
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
sys.path.insert(0, str(ROOT / "tools" / "analysis"))
import bwctl  # noqa: E402
from trace_report import Symbols, first_game_frames  # noqa: E402


def main(argv):
    name = argv[0] if argv else "jump"
    delay = float(argv[1]) if len(argv) > 1 else 0.0  # seconds between the pose jump and the capture
    bwctl.stereo("enabled 1", "freeze 1", "ipd_scale 1", "pose 0 0 0 0.3 0.1 0.2")
    time.sleep(2)
    before = set(bwctl.CAPTURES.glob("trace_*.txt"))
    if delay > 0:
        bwctl.stereo("ipd_scale 0", "pose 0 0 0 0 0 0")
        time.sleep(delay)
        bwctl.stereo(f"capture {name} trace")
    else:
        bwctl.stereo("ipd_scale 0", "pose 0 0 0 0 0 0", f"capture {name} trace")
    deadline = time.time() + 10
    while time.time() < deadline and not (bwctl.CAPTURES / f"{name}.json").exists():
        time.sleep(0.1)
    time.sleep(0.5)
    new = sorted(set(bwctl.CAPTURES.glob("trace_*.txt")) - before)
    ident = subprocess.run([sys.executable, str(ROOT / "tools/validate/stereo_checks.py"), "identity", name],
                           capture_output=True, text=True).stdout
    print("identity:", json.dumps(json.loads(ident)["identity"]) if ident else "n/a")
    if not new:
        print("no trace captured")
        return 1
    syms = Symbols()
    passes, p = [[], [], []], 0
    for line in new[-1].read_text().splitlines():
        if line.startswith("BeginScene"):
            p += 1
            continue
        m = re.match(r"^(DIP|DP)\S* (\S+) (\S+) (.*?)\|(.*)$", line)
        if m and p < 3:
            site = " < ".join(first_game_frames(m.group(5), syms, 4))
            passes[p].append(f"{site} :: {m.group(3)} {m.group(4).strip()}")
        elif p < 3 and line.startswith(("Lock", "Blt", "BltFast", "Load")):
            head, _, stack = line.partition("|")
            passes[p].append(f"{' < '.join(first_game_frames(stack, syms, 4))} :: {head.strip()}")
    a, b = passes[1], passes[2]
    print(f"trace {new[-1].name}: {len(a)} / {len(b)} draws")
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
    print(f"{len(diffs)} differing draws")
    for i, x, y in diffs[:20]:
        print(f"#{i}\n  L: {x[:260]}\n  R: {y[:260]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
