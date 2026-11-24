"""Compare what SteamVR received (its mirror texture) with the eye images we rendered.

Assumes the game runs with stereo through SteamVR (bwctl.py vr / stereo.req "vr openvr").
Takes N paired captures: our eye crops ("capture") and the compositor mirror ("mirror_capture")
of the same frame. Reports alignment, correlation and colour agreement, and how often the mirror
shows content our capture doesn't (e.g. a glitch in the previous frame), with a sheet per pair.

Usage: python tools/validate/mirror_check.py [count]
"""
import json
import pathlib
import sys
import time

import cv2
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402


def compare(ours, mirror):
    m = cv2.resize(mirror, (ours.shape[1], ours.shape[0]), interpolation=cv2.INTER_AREA).astype(np.float32)
    o = ours.astype(np.float32)
    g1, g2 = cv2.cvtColor(o, cv2.COLOR_BGR2GRAY), cv2.cvtColor(m, cv2.COLOR_BGR2GRAY)
    (dx, dy), _ = cv2.phaseCorrelate(g1, g2)
    diff = np.abs(m - o).max(axis=2)
    return {"correlation": float(np.corrcoef(g1.ravel(), g2.ravel())[0, 1]), "shift_px": [float(dx), float(dy)],
            "mean_abs_diff": float(np.abs(m - o).mean()), "fraction_diff_gt_40": float(np.mean(diff > 40)),
            "mean_bgr_ours": [float(v) for v in o.reshape(-1, 3).mean(0)],
            "mean_bgr_mirror": [float(v) for v in m.reshape(-1, 3).mean(0)]}, m.astype(np.uint8)


def main(argv):
    count = int(argv[0]) if argv else 5
    results = []
    for i in range(count):
        name = f"mc{i}"
        for s in ("_L.bmp", "_R.bmp"):
            (bwctl.CAPTURES / f"{name}m{s}").unlink(missing_ok=True)
        bwctl.stereo(f"capture {name}", f"mirror_capture {name}m")
        deadline = time.time() + 10
        while time.time() < deadline and not (bwctl.CAPTURES / f"{name}m_R.bmp").exists():
            time.sleep(0.1)
        time.sleep(0.3)
        for e in "LR":
            ours = cv2.imread(str(bwctl.CAPTURES / f"{name}_{e}.bmp"))
            mir = cv2.imread(str(bwctl.CAPTURES / f"{name}m_{e}.bmp"))
            if ours is None or mir is None:
                results.append({"capture": name, "eye": e, "error": "missing image"})
                continue
            r, m = compare(ours, mir)
            r.update({"capture": name, "eye": e})
            results.append(r)
            sheet = np.concatenate([cv2.resize(ours, (600, 652)), cv2.resize(m, (600, 652))], 1)
            cv2.imwrite(str(bwctl.CAPTURES / f"{name}_{e}_mirror_sheet.png"), sheet)
        time.sleep(0.5)
    for r in results:
        if "error" in r:
            print(r)
            continue
        print(f"{r['capture']} {r['eye']}: corr {r['correlation']:.4f}  shift ({r['shift_px'][0]:.2f},{r['shift_px'][1]:.2f})"
              f"  mean|d| {r['mean_abs_diff']:.2f}  >40: {r['fraction_diff_gt_40']:.2%}")
    (bwctl.CAPTURES / "mirror_check.json").write_text(json.dumps(results, indent=2))
    # The mirror can hold a slightly stale frame (e.g. right after a capture stalls the game), so
    # require most pairs - not all - to match. Geometry must always align within a pixel.
    good = [r for r in results if "error" not in r and r["correlation"] > 0.95]
    aligned = all("error" not in r and max(map(abs, r["shift_px"])) < 1 for r in results)
    ok = aligned and len(good) >= 0.75 * len(results)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
