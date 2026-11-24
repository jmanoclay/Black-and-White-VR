"""Scripted test of the VR options and debug page in the game's pause menu (player, 2026-10-31: the
tuning panel is awkward, use the game's menu; 2026-11-01: a debug menu to give oneself a miracle).

Esc opens the pause menu; its "Developer" tab (development builds) has "< Lightning bolt >" (a
click steps to the next miracle) and "Give miracle", then the tuning settings, seven to a page, with -
and +, and Previous, Next, Back. Clicked as in the headset: the right controller aimed at the button
on the menu's panel (the laser is the mouse there; the aim is corrected from where the cursor lands)
and the trigger pulled.
- the Developer tab opens the page;
- + on the third line (held person: out from the palm) steps it up, - back down (the saved value
  stays as it was);
- Next shows page 2; Back goes back to the main page (settings saved);
- the page again; one click on the picker: it offers the storm;
- "Give miracle": the menu closes and the storm's bubble is made in front of the hand;
- the trigger on the bubble pops it into the hand (hand state 8, its seed the storm's, type 0).
(Injected mouse clicks don't reach the menu in the harness, VR on or off.)

Boot first (BWVR_MUTE=1 python tools/harness/bwctl.py boot).
Usage: python tools/validate/debug_menu.py
"""
import math
import re
import sys
import time

import near_hand as nh
import leash_direct as ld
from near_hand import bwctl
from spell_in_hand import state

# The buttons' middles in the 1920x1080 layout (the menu's box at 560, 240 on the screen).
OPEN = (1116, 270)  # (the Developer tab)
OPTIONS = (960, 270)  # (the VR Options tab)
PICK, GIVE = (820, 372), (1175, 372)
PLUS3, MINUS3 = (1280, 532), (1210, 532)
NEXT, BACK = (950, 808), (1190, 808)
DIST, WIDTH, DROP = 2.0, 5.0, 0.1  # the menus' panel: 2 m ahead, 5 m wide, its middle 10 cm below the eyes
HAND = (0.2, -0.3, 0.3)


def cursor():
    (m,) = nh.query("pointerinfo", [r"pointer: valid 1 .* land (\d) cursor (\d) \((-?\d+), (-?\d+)\) lmb"])
    return int(m.group(1)), int(m.group(3)), int(m.group(4))


def aim(button):
    """The controller aimed at a button on the panel; True once the cursor is on it."""
    unit = WIDTH / 1920
    tx, ty = button
    for _ in range(5):
        target = ((tx - 960) * unit, -DROP + (540 - ty) * unit, DIST)
        d = [t - p for t, p in zip(target, HAND)]
        yaw = math.degrees(math.atan2(d[0], d[2]))
        pitch = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
        bwctl.stereo(f"ctrl r pose {yaw:.2f} {pitch:.2f} 0 {HAND[0]} {HAND[1]} {HAND[2]}")
        time.sleep(0.8)
        land, cx, cy = cursor()
        if land == 0 and abs(cx - button[0]) < 8 and abs(cy - button[1]) < 8:
            return True
        if land == 0:  # (landed short or past: aim further the other way)
            tx += button[0] - cx
            ty += button[1] - cy
    return False


def click():
    bwctl.stereo("ctrl r trigger 1")
    time.sleep(0.4)
    bwctl.stereo("ctrl r trigger 0")
    time.sleep(1.2)


def press(button):
    aimed = aim(button)
    click()
    return aimed


def menu():
    """(box, active box, page on/off, page number) and the page's setting lines."""
    m = nh.LOG.stat().st_size
    ld.send("ui debug_menu_state")
    time.sleep(0.5)
    t = ld.tail(m)
    found = re.search(r"debug menu: box (\S+), active box (\S+?),.*page (\w+) (\d+)", t)
    lines = re.findall(r"debug menu: line \d+: (.*)", t)
    return (found.groups() if found else None), lines


def main():
    results = []
    ld.send("enabled 1", "vr mock", "base level", "freeze 0", "hands near 1", "hands always 1", "hands laser 0", "ctrl r a 0",
            "ctrl r trigger 0", "ctrl r squeeze 0", "ctrl l off", "pose 0 0 0 0 0 0", "ui screen 1")
    time.sleep(1.0)
    mark = nh.LOG.stat().st_size
    up, _ = menu()
    if not (up and up[0] == up[1]):  # (open it, unless it already is)
        bwctl.request("input.req", "key esc\n")
        time.sleep(3)
        up, _ = menu()
    results.append(nh.report(bool(up) and up[0] == up[1], f"Esc: the pause menu is up ({up[:2] if up else '-'})"))
    press(OPEN)
    up, lines = menu()
    results.append(nh.report(bool(up) and up[2] == "developer", f"the Developer tab: its page is up (page {up[3] if up else '?'}, {len(lines)} lines)"))
    before = lines[2] if len(lines) > 2 else "?"
    press(PLUS3)
    plus = (menu()[1] or ["?"] * 3)[2]
    press(MINUS3)
    minus = (menu()[1] or ["?"] * 3)[2]
    results.append(nh.report(plus != before and minus == before, f"+ and - on a setting: '{before}' -> '{plus}' -> '{minus}'"))
    press(NEXT)
    up, _ = menu()
    results.append(nh.report(bool(up) and up[3] == "2", f"Next: page {up[3] if up else '?'}"))
    press(BACK)
    up, _ = menu()
    saved = "settings saved" in ld.tail(mark)
    results.append(nh.report(bool(up) and up[2] == "off" and saved, f"Back: the main page again, settings saved ({saved})"))
    press(OPEN)
    on = press(PICK)
    picked = re.search(r"debug menu: miracle (\d+) \((.*?)\)", ld.tail(mark))
    results.append(nh.report(bool(picked) and picked.group(1) == "0", f"a click on the picker: {picked.group(2) if picked else 'nothing'} (aimed {on})"))
    on = press(GIVE)
    time.sleep(1.5)
    # (Back in the game: the hand at the controller, held out over open grass.)
    x, z = 3150.0, 3350.0
    ld.send("pose 0 -45 0 0 0 0", f"ctrl r pose 0 0 0 {ld.PALM[0]} {ld.PALM[1]} {ld.PALM[2]}")
    ld.place_palm((x, nh.ground_at(x, z) + 3.0, z))
    time.sleep(1.5)
    text = ld.tail(mark)
    made = re.search(r"debug menu: a miracle's bubble, type (\d+) \((.*?)\), made at \(([-\d.]+), ([-\d.]+)\)", text)
    results.append(nh.report(bool(made) and made.group(1) == "0", f"Give miracle: the menu closes and the bubble is made ({made.group(0)[11:] if made else 'not made'}; aimed {on})"))
    if made:
        bx, bz = float(made.group(3)), float(made.group(4))
        for dy in (2.0, 3.0, 1.0, 4.0):
            ld.place_palm((bx, nh.ground_at(bx, bz) + dy, bz))
            time.sleep(0.8)
            bwctl.stereo("ctrl r trigger 1")
            time.sleep(0.15)
            bwctl.stereo("ctrl r trigger 0")
            time.sleep(1.5)
            if state() == 8:
                break
        m2 = nh.LOG.stat().st_size
        ld.send("hands seed")
        time.sleep(0.5)
        seed = re.search(r"hands: seed: in hand \S+, seed \S+, type (-?\d+)", ld.tail(m2))
        results.append(nh.report(state() == 8 and bool(seed) and seed.group(1) == "0",
                                 f"the trigger pops it into the hand (state {state()}, type {seed.group(1) if seed else '?'})"))
    ld.send("ctrl r off")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
