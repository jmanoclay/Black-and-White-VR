"""Phase 1 test suite: stereo correctness with the mock head pose (no headset needed).

Boots the game (unless --no-boot), freezes the simulation, captures stereo frames for a set of
head poses and runs tools/validate/stereo_checks.py on them. Writes captures/phase1_report.json.

Usage: python tools/validate/run_phase1.py [--no-boot] [--keep-running]
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "harness"))
import bwctl  # noqa: E402

PY = sys.executable
CHECKS = ROOT / "tools" / "validate" / "stereo_checks.py"

POSES = {  # name: (yaw, pitch, roll, x, y, z) in degrees / meters, engine axes
    "p_base": (0, 0, 0, 0, 0, 0),
    "p_yaw10": (10, 0, 0, 0, 0, 0),
    "p_pitch10": (0, 10, 0, 0, 0, 0),
    "p_roll15": (0, 0, 15, 0, 0, 0),
    "p_move": (0, 0, 0, 0.3, 0.1, 0.2),
}


# Failures that are understood and tracked. They are still reported,
# but don't fail the run; anything else failing does.
KNOWN_ISSUES = {
    "p_yaw10": "sea surface renders ~0.7 px beyond infinity when in view (negative disparity)",
    "p_base": "sea surface renders ~0.7 px beyond infinity when in view (negative disparity)",
    "identity": "citadel volume light (and some sky state) differs between eye passes after a camera change",
    "identity_motion": "citadel volume light + sky keep per-pass state; eyes differ under head motion "
                       "(workaround: CitadelVolumeLight=0 in the game's graphics setup)",
}


def capture(name, pose, ipd_scale=1.0):
    bwctl.stereo(f"ipd_scale {ipd_scale}", "pose " + " ".join(str(v) for v in pose))
    bwctl.wait_frames(10)
    meta = bwctl.stereo_capture(name)
    if not meta:
        raise RuntimeError(f"capture {name} failed")
    return name


def run_check(*args):
    out = subprocess.run([PY, str(CHECKS), *args], capture_output=True, text=True)
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return {"pass": False, "error": out.stdout[-2000:] + out.stderr[-2000:]}


def main(argv):
    if "--no-boot" not in argv:
        bwctl.kill()
        time.sleep(1)
        bwctl.boot()
        time.sleep(3)
    # Pin the game's cursor: the god hand follows the real mouse, which would otherwise move it
    # between (and during) captures whenever the user touches their mouse.
    bwctl.request("input.req", "pos 960 540\n")
    bwctl.stereo("enabled 1", "freeze 1", "base game")
    bwctl.wait_frames(30)

    results = {}
    for name, pose in POSES.items():
        capture(name, pose)
    capture("p_identity", POSES["p_base"], ipd_scale=0.0)
    bwctl.stereo("ipd_scale 1", "pose 0 0 0 0 0 0")

    for name in POSES:
        results[name] = run_check("single", name)
    results["identity"] = run_check("identity", "p_identity")
    for name in ("p_yaw10", "p_pitch10", "p_roll15", "p_move"):
        results[f"worldlock_{name}"] = run_check("worldlock", "p_base", name)

    # Eye identity under continuous head motion (IPD 0 + sway).
    sway = subprocess.run([PY, str(ROOT / "tools/validate/sway_identity.py"), "6"], capture_output=True, text=True)
    results["identity_motion"] = {"pass": sway.returncode == 0, "output": sway.stdout[-1500:]}
    bwctl.stereo("ipd_scale 1", "pose 0 0 0 0 0 0")

    summary = {k: v.get("pass") for k, v in results.items()}
    report = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "summary": summary,
              "known_issues": KNOWN_ISSUES, "results": results}
    out = bwctl.CAPTURES / "phase1_report.json"
    out.write_text(json.dumps(report, indent=2))
    for k, v in summary.items():
        note = f"   (known issue: {KNOWN_ISSUES[k]})" if not v and k in KNOWN_ISSUES else ""
        print(f"{'PASS' if v else 'FAIL'}  {k}{note}")
    print(f"report: {out}")
    if "--keep-running" not in argv:
        bwctl.stereo("freeze 0")
    unexpected = [k for k, v in summary.items() if not v and k not in KNOWN_ISSUES]
    return 0 if not unexpected else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
