"""Scripted test of a tutorial "?" signpost's panel in VR (the user's save: the sign at the
fish farms, (1751.2, 2466.4)).

Tapped, the sign's helper says something in a speech bubble ("Did you know...?"). The game lays
the bubble out once per frame for its own camera (in the headset it hung at the top of the view
with no depth) and its text elsewhere. In the eye passes the mod drops the bubble and draws its
lines itself, 1.5 m along the player's gaze when it opens, staying there in the world.

Checks:
- the trigger on the sign taps it;
- the panel's lines are drawn by the mod (the log), title first;
- looking down at the sign (as a player does), the lines are where the player looks, in both eyes:
  near the middle of each eye's view and level between them, still there after looking away and
  back (the game drops its bubble meanwhile).

Usage: python tools/validate/help_sign.py
"""
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from PIL import Image

SIGN = (1751.2, 2466.4)


def text_box(path):
    """Bounding box of the yellow text pixels in an eye image (the game's yellow, or the paler one of
    the mod's own panel picture)."""
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            if r > 200 and g > 180 and b < 175 and r - b > 60:
                xs.append(x), ys.append(y)
    return (min(xs), min(ys), max(xs), max(ys), len(xs)) if xs else None


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ui help_panel 1", "pose 0 -45 0 0 0 0",
            f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    time.sleep(1)
    g = nh.ground_at(*SIGN)
    ld.place_palm((SIGN[0], g + 2.0, SIGN[1]))
    time.sleep(1.2)
    mark = nh.LOG.stat().st_size
    nh.bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.15)
    nh.bwctl.stereo("ctrl r trigger 0")
    time.sleep(2.5)
    nh.bwctl.stereo("ctrl r pose 0 0 0 0.3 -0.5 0.1")  # (the hand out of the view)
    time.sleep(0.5)
    # Looking away (the game drops its bubble once its helper sprite is out of view), then back:
    # the lines stay for a reading time.
    nh.bwctl.stereo("pose 90 -20 0 0 0 0")
    time.sleep(2.0)
    nh.bwctl.stereo("pose 0 -45 0 0 0 0")
    time.sleep(1.0)
    text = ld.tail(mark)
    results.append(nh.report("hands: tap (hand)" in text, "the trigger taps the sign"))
    m = re.search(r"help panel: (\d+) lines", text)
    results.append(nh.report(m and int(m.group(1)) >= 2, f"the panel's lines are drawn by the mod ({m.group(1) if m else 0} lines)"))
    nh.bwctl.stereo_capture("help_sign")
    import json
    eyes = json.load(open(nh.bwctl.CAPTURES / "help_sign.json"))["eyes"]
    boxes = [text_box(nh.bwctl.CAPTURES / f"help_sign_{e}.bmp") for e in "LR"]
    # Where the player looks: near the middle of each eye's view (the eye camera's axis is the
    # middle of its render, before the crop), level between the eyes.
    ok, where = all(boxes), []
    for bx, e in zip(boxes, eyes):
        if not bx:
            continue
        cx = e["render"][0] / 2 - e["crop"][0]
        cy = e["render"][1] / 2 - e["crop"][1]
        mx, my = (bx[0] + bx[2]) / 2 - cx, (bx[1] + bx[3]) / 2 - cy
        where.append((round(mx), round(my)))
        ok = ok and abs(mx) < 300 and abs(my) < 300
    ok = ok and len(where) == 2 and abs(boxes[0][1] - boxes[1][1]) < 12
    results.append(nh.report(ok, f"the lines are where the player looks, in both eyes (text centre from each eye's centre: {where})"))
    nh.bwctl.stereo("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
