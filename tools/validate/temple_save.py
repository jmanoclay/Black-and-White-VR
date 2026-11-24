"""Scripted test of the temple's save room in VR: the Load button on the cinema screen, and play
resuming after a load (the user's save: the temple by the creature pen).

The temple's camera has a 90-degree horizontal FOV, laid out for 4:3; at 16:9 the save room's
green Load button was cut off at the bottom of the frame. And a save loaded inside the temple left
the game paused (the temple pauses it; only leaving it the normal way unpauses).

1. A trigger tap on the entrance takes the player inside (view mode 1, on the cinema screen).
2. The save room (`temple_room 5`: harness navigation only).
3. With the laser on the cinema screen: the trigger on a save's picture selects it; the green Load
   button under it is wholly inside the frame; the trigger on it asks "load this game?"; the
   trigger on "Yes" loads it.
4. Back in the world, the game turns again by itself.

The run loads a save; bwctl snapshots and restores the profiles, but the game's state is the
loaded save's afterwards (run it last, or reboot). Finds the save's picture by its colour: the
thumbnails are drawn pink (magenta) in this setup; SKIP if none is found.

Usage: python tools/validate/temple_save.py [control]
  control: with the mod's unpause off ([stereo] temple_unpause 0), the game must stay paused (the
           game's own behaviour, also in the flat game)
"""
import math
import re
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl

ENTRANCE = (1905.0, 30.0, 2515.0)  # a palm here and the trigger enter the temple
DIST, DROP, WIDTH = 3.0, 0.1, 4.2  # the cinema screen: metres ahead, centre below eye level, width
HAND = (0.2, -0.3, 0.3)  # the controller, metres from the head


def log_since(mark):
    with open(nh.LOG, encoding="utf-8", errors="replace") as f:
        f.seek(mark)
        return f.read()


def enter(tries=4):
    """A trigger tap with the palm on the entrance, again while it doesn't take: the creature
    wanders by the temple, and with it under the palm the tap goes to the creature (2026-11-03).
    True once inside (view mode 1)."""
    for _ in range(tries):
        ld.send("hands leash_off")  # (a tap that met the creature puts the leash in the hand)
        time.sleep(1.0)
        mark = nh.LOG.stat().st_size
        ld.place_palm(ENTRANCE)
        time.sleep(0.6)
        trigger(hold=0.1)  # (a tap on the world)
        time.sleep(8)
        if "shown on the screen (view mode 1)" in log_since(mark):
            return True
    return False


def frame(name):
    """The game's own frame (the window shows it while the cinema screen is up)."""
    return np.asarray(Image.open(bwctl.shot(name)).convert("RGB")).astype(int)


def aim(px, py, w=1920, h=1080):
    """Points the laser at layout pixel (px, py) of the cinema screen; returns the game's cursor."""
    unit = WIDTH / w
    t = ((px - w / 2) * unit, -DROP - (py - h / 2) * unit, DIST)
    d = [t[i] - HAND[i] for i in range(3)]
    yaw = math.degrees(math.atan2(d[0], d[2]))
    pitch = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
    bwctl.stereo(f"ctrl r pose {yaw:.2f} {pitch:.2f} 0 {HAND[0]} {HAND[1]} {HAND[2]}")
    time.sleep(1.2)
    (m,) = nh.query("pointerinfo", [r"pointer: valid 1 .* cursor (\d) \((-?\d+), (-?\d+)\)"])
    return int(m.group(2)), int(m.group(3))


def trigger(hold=0.5):
    """A click on the screen (the temple's views): 0.5 s (0.3 s clicks were missed in the save room,
    2026-11-03). A tap on the world needs hold=0.1: held
    past [hands] tap_s, 0.25 s, the trigger is a hold there (picks up, or does nothing)."""
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(hold)
    bwctl.stereo("ctrl r trigger 0")


def blob(mask):
    ys, xs = np.nonzero(mask)
    if len(xs) < 200:
        return None
    return xs.min(), ys.min(), xs.max(), ys.max(), float(np.median(xs)), float(np.median(ys))


def main():
    control = "control" in sys.argv[1:]
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "temple_4x3 1", f"temple_unpause {0 if control else 1}", "pose 0 -40 0 0 0 0", "hands leash_off",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    inside = enter()
    results.append(nh.report(inside, "a trigger tap on the entrance: inside the temple, on the cinema screen"))
    if not inside:
        return 1
    widened = re.search(r"the temple's FOV ([\d.]+) widened to ([\d.]+) degrees", open(nh.LOG, encoding="utf-8", errors="replace").read())
    results.append(nh.report(bool(widened), f"the temple's FOV widened to the 4:3 height ({widened.group(1)} -> {widened.group(2)} degrees)"
                             if widened else "the temple's FOV widened to the 4:3 height"))
    # Harness navigation: straight to the save room (Temple::GoToRoom, room 5). Then the laser, on the
    # cinema screen (looking straight ahead; the screen is placed anew).
    bwctl.stereo("pose 0 0 0 0 0 0", "hands laser 1", "ctrl r pose 60 -40 0 0.2 -0.3 0.3", "temple_room 5")
    time.sleep(10)
    a = frame("temple_room")
    h, w = a.shape[:2]
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    pic = blob((r > 220) & (g < 60) & (b > 150))  # (an old save's magenta picture: 254, 0, 186)
    if not pic:
        print("SKIP  no save picture (magenta thumbnail) found in the save room")
        return 0
    cur = aim(pic[4], pic[5], w, h)
    results.append(nh.report(math.dist(cur, pic[4:6]) < 8, f"the laser on a save's picture: cursor {cur}, picture at ({pic[4]:.0f}, {pic[5]:.0f})"))
    # (While the spirits introduce the room a click on a picture may not zoom in: again, up to 3 times.)
    for _ in range(3):
        trigger()
        time.sleep(5)
        a = frame("temple_save")
        r, g, b = a[..., 0], a[..., 1], a[..., 2]
        green = (g > 120) & (g > r + 40) & (g > b + 40)
        green[: int(h * 0.55)] = False  # (the buttons are under the picture)
        btn = blob(green)
        if btn:
            break
        aim(pic[4], pic[5], w, h)
    whole = bool(btn) and btn[3] < h - 4
    results.append(nh.report(whole, f"the green Load button is wholly inside the frame (rows {btn[1]}-{btn[3]} of {h})" if btn else
                             "the green Load button is in the frame"))
    if not btn:
        return 1
    cur = aim(btn[4], btn[5], w, h)
    results.append(nh.report(math.dist(cur, btn[4:6]) < 8, f"the laser on the Load button: cursor {cur}, button at ({btn[4]:.0f}, {btn[5]:.0f})"))
    trigger()
    time.sleep(4)
    a = frame("temple_confirm")
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    box = blob((abs(r - 192) < 8) & (abs(g - 93) < 8) & (abs(b - 192) < 8))  # the purple "load this game?" box (its exact colour: stray pixels widened a looser match)
    results.append(nh.report(bool(box), "the Load button asks to confirm"))
    if not box:
        return 1
    yes = (box[0] + 0.16 * (box[2] - box[0]), box[1] + 0.74 * (box[3] - box[1]))
    aim(*yes, w, h)
    mark = nh.LOG.stat().st_size
    trigger()
    deadline = time.time() + 90
    text = ""
    while time.time() < deadline:
        time.sleep(2)
        if not bwctl.running():
            break
        text = log_since(mark)
        turns = [float(t) for t in re.findall(r"game turns: ([\d.]+)/s", text.split("without leaving the temple")[-1])]
        if "without leaving the temple" in text and len(turns) >= 2:
            break
    results.append(nh.report(bwctl.running(), "the game is still running after the load"))
    results.append(nh.report("is the world again" in text, "after the load: the world, off the cinema screen"))
    m = re.search(r"the world is back without leaving the temple .*", text)
    print("      " + (m.group(0) if m else "(no unpause line)"))
    turns = [float(t) for t in re.findall(r"game turns: ([\d.]+)/s", text.split("without leaving the temple")[-1])] if m else []
    if control:
        results.append(nh.report(len(turns) >= 2 and turns[-1] == 0, f"control (unpause off): the game stays paused: game turns {turns[-2:]} /s"))
    else:
        results.append(nh.report(len(turns) >= 2 and turns[-1] > 5, f"play resumes by itself: game turns {turns[-2:]} /s"))
    bwctl.stereo("ctrl r off", "hands laser 0")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
