"""Temporal flicker under head motion: burst-capture consecutive frames while the mock head sways.

A pixel flickers in frame i when it differs strongly from both neighbours while the neighbours
agree with each other: smooth motion doesn't do that, popping geometry does.

Usage: python tools/validate/flicker_check.py [frames] [pitch_deg] [sway_amp] [sway_period] [--live]
"""
import glob
import pathlib
import sys
import time

import cv2
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402


def main(argv):
    argv_n = [a for a in argv if not a.startswith("--")]
    frames = int(argv_n[0]) if argv_n else 60
    pitch = float(argv_n[1]) if len(argv_n) > 1 else 0.0
    amp = float(argv_n[2]) if len(argv_n) > 2 else 15.0
    period = float(argv_n[3]) if len(argv_n) > 3 else 3.0
    for f in glob.glob(str(bwctl.CAPTURES / "fl_*.bmp")):
        pathlib.Path(f).unlink()
    live = "--live" in argv  # use SteamVR's real head pose (sway, if given, is added on top)
    amp_given = len(argv_n) > 2
    bwctl.request("input.req", "pos 960 540\n")
    if live:
        bwctl.stereo("enabled 1", "vr openvr", "ipd_scale 1", f"sway {amp if amp_given else 0} {period}")
        time.sleep(15)
    else:
        bwctl.stereo("enabled 1", "vr mock", "ipd_scale 1", f"pose 0 {pitch} 0 0 0 0", f"sway {amp} {period}")
        time.sleep(2)
    bwctl.stereo(f"burst fl {frames}")
    deadline = time.time() + 30
    while time.time() < deadline and len(glob.glob(str(bwctl.CAPTURES / "fl_*.bmp"))) < frames:
        time.sleep(0.2)
    time.sleep(0.5)
    bwctl.stereo("sway 0 3", "pose 0 0 0 0 0 0")
    files = sorted(glob.glob(str(bwctl.CAPTURES / "fl_*.bmp")))
    imgs = [cv2.cvtColor(cv2.imread(f), cv2.COLOR_BGR2GRAY).astype(np.float32) for f in files]
    worst, worst_i, per = 0.0, -1, []
    for i in range(1, len(imgs) - 1):
        a, b, c = imgs[i - 1], imgs[i], imgs[i + 1]
        flick = (np.abs(b - (a + c) / 2) > 50) & (np.abs(a - c) < 15)
        frac = float(flick.mean())
        per.append(frac)
        if frac > worst:
            worst, worst_i = frac, i
    print(f"{len(imgs)} frames, flicker fraction: median {np.median(per):.4%}, worst {worst:.4%} (frame {worst_i})")
    if worst_i > 0:
        a, b, c = imgs[worst_i - 1], imgs[worst_i], imgs[worst_i + 1]
        mask = ((np.abs(b - (a + c) / 2) > 50) & (np.abs(a - c) < 15)).astype(np.uint8) * 255
        sheet = np.concatenate([b.astype(np.uint8), mask], 1)
        cv2.imwrite(str(bwctl.CAPTURES / "flicker_sheet.png"), cv2.resize(sheet, (sheet.shape[1] // 2, sheet.shape[0] // 2)))
    ok = worst < 0.001
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
