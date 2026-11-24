"""Eye identity under continuous head motion: IPD 0 + mock head sway, N captures.

Both eyes then share one camera, so any pixel difference is engine state that one render pass
changes for the next. Optional extra stereo commands (e.g. rewind ranges) are applied first.

Usage: python tools/validate/sway_identity.py [count] ["<stereo cmd>;<stereo cmd>"]
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402


def main(argv):
    count = int(argv[0]) if argv else 6
    extra = [c for c in (argv[1].split(";") if len(argv) > 1 else []) if c.strip()]
    bwctl.request("input.req", "pos 960 540\n")
    bwctl.stereo("enabled 1", "freeze 1", "ipd_scale 0", "pose 0 0 0 0 0 0", "sway 15 3", *extra)
    time.sleep(3)
    worst = 0.0
    for i in range(count):
        name = f"swayid{i}"
        bwctl.stereo_capture(name)
        out = subprocess.run([sys.executable, str(ROOT / "tools/validate/stereo_checks.py"), "identity", name],
                             capture_output=True, text=True).stdout
        r = json.loads(out)["identity"]
        worst = max(worst, r["fraction_diff_gt_8"])
        print(f"{name}: diff>8 {r['fraction_diff_gt_8']:.4%}  max {r['max_abs_diff']}  psnr {r['psnr_db']:.1f}")
        time.sleep(0.6)
    bwctl.stereo("sway 0 3")
    print(f"worst: {worst:.4%}")
    return 0 if worst < 1e-4 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
