"""Scripted test of saving in the temple in VR: the save's picture and its name.

1. Entering the temple, the game takes the picture a save keeps. It read the (windowed) screen,
   which reads back as magenta; in VR it now reads the game's frame: the temporary picture
   (Profiles/.../Saved Games/Pictures/save_game_picture_temporary.raw, DXT1) is not magenta.
2. In the save room, the laser on an empty frame and then on its (yellow) save button opens the
   name dialog ("Save Game Message"). In VR it is filled in with a default name ("VR save 18 Oct 20:49")
   and SteamVR's keyboard is opened (mock VR: none; `ui save_name_keys` stands in for keys pressed
   on it, which go into the field one by one; a backslash and a b stand for Backspace). While the dialog is up, the
   mipmaps take little time (the dialog's textures are redrawn every frame).
3. The laser on OK saves: a save's SaveGame.inf with that name, and its picture not magenta.

Saves a game: bwctl restores the profiles when the game is killed. Run it alone or last.
Usage: python tools/validate/temple_save_name.py
"""
import io
import re
import sys
import time

import numpy as np
from PIL import Image

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
import temple_save as ts

SAVES = bwctl.GAME / "Profiles"
# Frames in the save room's first view (window pixels), tried in turn for an empty one (no Load
# button under it): the lower right, the middle one up, the upper right.
FRAMES = [(1600, 700), (980, 385), (1380, 280)]
OK_BUTTON = (860, 614)  # the dialog's OK (it is centred in the window)
NAME = "Harness save test"


def picture(path, box=(0, 0, 256, 128)):
    """Fraction of magenta pixels, of see-through pixels (DXT1's 1-bit alpha), and the number of
    distinct colours, in `box` (x0, y0, x1, y1) of a save picture file."""
    d = path.read_bytes()
    a = np.asarray(Image.open(io.BytesIO(b"DDS " + d[4:])).convert("RGBA")).astype(int)[box[1]:box[3], box[0]:box[2]]
    magenta = ((a[..., 0] > 230) & (a[..., 1] < 40) & (a[..., 2] > 230)).mean()
    clear = (a[..., 3] < 128).mean()
    return float(magenta), float(clear), len(np.unique(a[..., :3].reshape(-1, 3), axis=0))


def main():
    results = []
    start = time.time()
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "save_pictures 1", "ui save_name 1", "pose 0 -40 0 0 0 0", "hands leash_off",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    mark = nh.LOG.stat().st_size
    results.append(nh.report(ts.enter(), "a trigger tap on the entrance: inside the temple"))
    text = ts.log_since(mark)
    results.append(nh.report("a picture taken from the window's last frame" in text, "entering, the save picture is taken from the game's frame"))
    temp = next(SAVES.rglob("save_game_picture_temporary.raw"))
    magenta, clear, colours = picture(temp)
    results.append(nh.report(temp.stat().st_mtime > start and magenta < 0.05 and clear < 0.05 and colours > 50,
                             f"the picture is the world, not magenta ({magenta:.0%} magenta, {clear:.0%} see-through, {colours} colours)"))
    # Harness navigation: straight to the save room (Temple::GoToRoom, room 5). Then the laser, on the
    # cinema screen (looking straight ahead; the screen is placed anew).
    bwctl.stereo("pose 0 0 0 0 0 0", "hands laser 1", "ctrl r pose 60 -40 0 0.2 -0.3 0.3", "temple_room 5")
    time.sleep(10)
    yellow = None
    for i, spot in enumerate(FRAMES):
        if i:  # back to the room's first view
            bwctl.stereo("temple_room 6")
            time.sleep(7)
            bwctl.stereo("temple_room 5")
            time.sleep(9)
        ts.aim(*spot)
        ts.trigger()
        time.sleep(5)
        a = ts.frame("temple_empty_slot")
        h = a.shape[0]
        r, g, b = a[..., 0], a[..., 1], a[..., 2]
        low = np.zeros(r.shape, bool)
        low[int(h * 0.6):] = True
        yellow = ts.blob(low & (r > 240) & (g > 150) & (b < 80))  # the save button (under the frame)
        if yellow:  # a Load button under it: a slot already in use
            below = np.zeros(r.shape, bool)
            below[yellow[3]:, max(0, int(yellow[4]) - 120):int(yellow[4]) + 120] = True
            if np.count_nonzero(below & (g > 120) & (g > r + 40) & (g > b + 40)) > 1000:  # (the wall: a few hundred)
                yellow = None
        if yellow:
            break
    if not yellow:
        print("SKIP  no empty frame found in the save room")
        return 0 if all(results) else 1
    mark = nh.LOG.stat().st_size
    ts.aim(yellow[4], yellow[5])
    ts.trigger()
    time.sleep(4)
    text = ts.log_since(mark)
    results.append(nh.report("the save's name dialog opened" in text, "the save button opens the name dialog"))
    m = re.search(r"the save's name typed: '([^']*)'", text)
    results.append(nh.report(bool(m) and m.group(1).startswith("VR save "), f"a default name is filled in ({m.group(1) if m else None!r})"))
    results.append(nh.report("openvr: keyboard" not in text, "(mock VR: no SteamVR keyboard to show)"))
    ts.frame("temple_save_name")
    default = m.group(1) if m else ""
    # The game's dialog redraws its textures every frame: their mip levels mustn't cost much.
    mark = nh.LOG.stat().st_size
    time.sleep(11)
    costs = [float(c) for c in re.findall(r"mipmaps: .*?levels made .*?, ([\d.]+) ms in all", ts.log_since(mark))]
    results.append(nh.report(bool(costs) and max(costs) < 100, f"with the dialog up, mip levels take {costs} ms per 5 s (was ~480)"))
    # Keys on the keyboard: Backspace over the default, then a name.
    bwctl.stereo("ui save_name_keys " + r"\b" * 3)
    time.sleep(1.5)
    ts.frame("temple_save_backspaced")
    bwctl.stereo("ui save_name_keys " + r"\b" * (len(default) - 3) + NAME)
    time.sleep(3)  # (one character a frame)
    ts.frame("temple_save_named")
    mark = nh.LOG.stat().st_size
    saved_after = time.time()
    ts.aim(*OK_BUTTON)
    ts.trigger()
    infos = []
    for _ in range(20):
        time.sleep(2)
        infos = [p for p in SAVES.rglob("SaveGame.inf") if p.stat().st_mtime > saved_after]
        if infos and "dialog closed" in ts.log_since(mark):
            break
    # (SaveGame.inf: the date, then the name at byte 132, UTF-16, alone)
    named = [p for p in infos if p.read_bytes()[132:].decode("utf-16-le", "replace").split(chr(0))[0] == NAME]
    results.append(nh.report(bool(named), f"OK saves the game under that name ({named[0].parent.name if named else [p.parent.name for p in infos]})"))
    # The slot's picture: two slots to a file (save_game_picture_hires_<slot / 2>.raw), one above
    # the other, 256x128 each. (The small copies, _lores_, hold eight: two across, four down.)
    if named and named[0].parent.name.isdigit():
        slot = int(named[0].parent.name)
        pic = next(SAVES.rglob(f"save_game_picture_hires_{slot // 2}.raw"))
        y0 = (slot % 2) * 128
        magenta, clear, colours = picture(pic, (0, y0, 256, y0 + 128))
        results.append(nh.report(pic.stat().st_mtime > saved_after and magenta < 0.05 and clear < 0.05 and colours > 50,
                                 f"its picture is the world (slot {slot} of {pic.name}: {magenta:.0%} magenta, {clear:.0%} see-through, {colours} colours)"))
    else:
        results.append(nh.report(False, "its picture is written"))
    bwctl.stereo("ctrl r off", "hands laser 0")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
