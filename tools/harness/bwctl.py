"""Control client for the running game, via the mod's control files in <game>/bwvr/control.

Usage:
  python tools/harness/bwctl.py launch               start the game (mod must be deployed)
  python tools/harness/bwctl.py boot                 launch and get to gameplay (closes the menu)
  python tools/harness/bwctl.py vr                   boot, then render to the headset through SteamVR
  python tools/harness/bwctl.py kill
  python tools/harness/bwctl.py status
  python tools/harness/bwctl.py shot [name]          screenshot -> bwvr/captures/<name>.png
  python tools/harness/bwctl.py input "<script>"     e.g. "click 960 418" (see src/control/input_inject.h)
  python tools/harness/bwctl.py capture <frames>     D3D7 call trace of the next N frames
  python tools/harness/bwctl.py camdump [name]       engine camera globals + ProjectPoint probes (JSON)
  python tools/harness/bwctl.py stereo "<cmd>;<cmd>" stereo renderer commands (see src/render/stereo.h)
  python tools/harness/bwctl.py stereo-capture <name> capture both eyes + depth + metadata
  python tools/harness/bwctl.py wait-frames <n>      block until n more frames have been presented

With BWVR_MUTE=1 in the environment the launched game opens no sound mixer (the mod stubs it): for
when Windows' audio service stalls, which hangs the game after loading (2026-10-31).
"""
import os
import pathlib
import subprocess
import sys
import time

GAME = pathlib.Path(r"D:\Games\Black & White")
BWVR = GAME / "bwvr"
CONTROL = BWVR / "control"
CAPTURES = BWVR / "captures"


def status():
    try:
        text = (CONTROL / "status.txt").read_text()
    except OSError:
        return {}
    return dict(line.split("=", 1) for line in text.split() if "=" in line)


def running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq runblack.exe", "/NH"], capture_output=True, text=True)
    return "runblack.exe" in out.stdout


PROFILES = GAME / "Profiles"
SAVE_SNAPSHOT = BWVR / "test_save_snapshot"


def snapshot_saves():
    """Copy the player's profiles aside before a test run (the game writes its autosave while running)."""
    import shutil
    if SAVE_SNAPSHOT.exists():
        return  # an earlier run's snapshot hasn't been restored yet; keep the older (pre-test) state
    shutil.copytree(PROFILES, SAVE_SNAPSHOT / "Profiles")
    (SAVE_SNAPSHOT / "taken").write_text(str(time.time()))


def restore_saves():
    """Undo whatever a test run did to the saves: copy the pre-launch snapshot back over them."""
    import shutil
    marker = SAVE_SNAPSHOT / "taken"
    if not marker.exists():
        return
    taken = float(marker.read_text())
    changed = [p for p in PROFILES.rglob("*") if p.is_file() and p.stat().st_mtime > taken]
    if changed:
        shutil.copytree(SAVE_SNAPSHOT / "Profiles", PROFILES, dirs_exist_ok=True)
        print(f"restored {len(changed)} save file(s) changed during the test run")
    # Files and folders the run created (a new save's slot) aren't in the snapshot: removed.
    snap = SAVE_SNAPSHOT / "Profiles"
    for p in sorted(PROFILES.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if not (snap / p.relative_to(PROFILES)).exists():
            shutil.rmtree(p) if p.is_dir() else p.unlink()
            print(f"removed {p.relative_to(PROFILES)} (made during the test run)")
    shutil.rmtree(SAVE_SNAPSHOT)


def launch(protect_saves=True):
    """protect_saves: snapshot the profiles so kill() can undo a test run's autosaves. Off for
    the player's own sessions (bwctl vr), whose progress must stick."""
    if running():
        print("already running")
        return
    if protect_saves:
        snapshot_saves()
    (CONTROL / "status.txt").unlink(missing_ok=True)
    # (The mod starts in VR through SteamVR; a test run starts flat unless BWVR_START says
    # otherwise, and turns on the scripted head itself. The player's session (bwctl vr): VR at once.)
    env = dict(os.environ)
    if protect_saves:
        env.setdefault("BWVR_START", "mono")
    subprocess.Popen([str(GAME / "runblack.exe")], cwd=str(GAME), env=env)
    print("launched")


def kill(timeout=15.0):
    subprocess.run(["taskkill", "/IM", "runblack.exe", "/F"], capture_output=True)
    # taskkill returns before the process is gone; wait so a following launch/deploy is safe.
    deadline = time.time() + timeout
    while running() and time.time() < deadline:
        time.sleep(0.2)
    if running():
        print("kill: still running")
        return
    restore_saves()
    print("killed")


def request(name, text):
    CONTROL.mkdir(parents=True, exist_ok=True)
    tmp = CONTROL / (name + ".tmp")
    tmp.write_text(text)
    for attempt in range(20):  # (the mod may hold the last request open while reading it)
        try:
            tmp.replace(CONTROL / name)  # atomic: the mod never sees a half-written request
            break
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.02)


def wait_frames(n, timeout=60.0):
    start = int(status().get("frame", 0))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if int(status().get("frame", 0)) >= start + n:
            return True
        time.sleep(0.1)
    return False


def shot(name="shot", timeout=10.0):
    bmp = CAPTURES / f"{name}.bmp"
    bmp.unlink(missing_ok=True)
    request("screenshot.req", name)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if bmp.exists():
            time.sleep(0.2)
            from PIL import Image

            png = CAPTURES / f"{name}.png"
            Image.open(bmp).save(png)
            return png
        time.sleep(0.1)
    return None


CONTINUE_BUTTON = (960, 418)  # "Continue Game" on the in-game menu at 1920x1080


def boot(menu_timeout=90.0, protect_saves=True):
    """Launch and get through the startup sequence to gameplay.

    Logos, loading and the splash need no input on this setup; the last save then loads with
    the in-game menu open, and Esc (or "Continue Game") closes it.
    """
    launch(protect_saves)
    deadline = time.time() + menu_timeout
    while time.time() < deadline and status().get("device") != "1":
        time.sleep(0.5)
    # The menu shows once the save has loaded; the device appears well before that. The mod logs
    # the menu's text starting ("ui: menu text starts"; through dxwrapper about 35 s in, natively
    # about 25): Esc then closes it (same as "Continue Game").
    log = BWVR / "logs" / "bwvr.log"
    t0 = time.time()
    while time.time() - t0 < 60:
        if "ui: menu text starts" in log.read_text(encoding="utf-8", errors="replace"):
            break
        time.sleep(1.0)
    time.sleep(2)
    request("input.req", "key esc\n")
    time.sleep(4)
    return shot("boot")


def stereo(*lines):
    """Send stereo renderer commands (see src/render/stereo.h)."""
    request("stereo.req", "\n".join(lines) + "\n")


def stereo_capture(name, timeout=15.0):
    """Capture the next stereo frame; returns the metadata path once all files exist."""
    meta = CAPTURES / f"{name}.json"
    for suffix in ("_L.bmp", "_R.bmp", "_L.depth", "_R.depth", ".json"):
        (CAPTURES / f"{name}{suffix}").unlink(missing_ok=True)
    stereo(f"capture {name}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if meta.exists():
            time.sleep(0.3)
            return meta
        time.sleep(0.1)
    return None


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    cmd, args = argv[0], argv[1:]
    if cmd == "launch":
        launch()
    elif cmd == "boot":
        print(boot() or "boot screenshot failed")
    elif cmd == "vr":
        # Play in the headset: boot to gameplay, then stereo through SteamVR (Ctrl+F12 recentres).
        restore_saves()  # a test run that wasn't cleaned up: undo it before the player's session
        boot(protect_saves=False)
        stereo("enabled 1", "vr openvr", "recenter")
        print("VR on: put the headset on; Ctrl+F12 recentres")
    elif cmd == "kill":
        kill()
    elif cmd == "status":
        print(status() if running() else "not running")
    elif cmd == "shot":
        print(shot(*(args[:1] or ["shot"])) or "screenshot failed")
    elif cmd == "input":
        request("input.req", "\n".join(args).replace(";", "\n") + "\n")
        print("queued")
    elif cmd == "stereo":
        stereo(*" ".join(args).split(";"))
        print("sent")
    elif cmd == "stereo-capture":
        print(stereo_capture(args[0] if args else "stereo") or "stereo capture failed")
    elif cmd == "camdump":
        name = args[0] if args else "camdump"
        out = CAPTURES / f"{name}.json"
        out.unlink(missing_ok=True)
        request("camdump.req", name)
        deadline = time.time() + 10
        while time.time() < deadline and not out.exists():
            time.sleep(0.1)
        time.sleep(0.2)
        print(out if out.exists() else "camdump failed")
    elif cmd == "capture":
        request("capture.req", args[0] if args else "1")
        print("requested")
    elif cmd == "wait-frames":
        print("ok" if wait_frames(int(args[0])) else "timeout")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
