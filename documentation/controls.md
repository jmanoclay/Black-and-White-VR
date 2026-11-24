# Controls (as built, 2026-10-23)

**The trigger does everything; the grip is a shortcut.** Most controllers have only a trigger, so a quick tap interacts and the trigger held past a quarter second picks up and carries (let go: drop or throw). On controllers with a grip (Index), the grip picks up at once, without the wait. `[hands] trigger_carries=0` turns the trigger's pick-up off.

The dominant hand is the right hand (`[hands] dominant_left=1` swaps them). "The other hand" is the left.

## Moving around
| Control | Does |
|---|---|
| Other hand's stick | Move, relative to where you face. Faster the higher up you are. You keep your height above the land as you go (up over hills, down into valleys), as set with the dominant stick or by a teleport (`[hands] follow_ground=0` turns it off). |
| Dominant stick left/right | Turn: in snaps (30 degrees), or smoothly; both set in the pause menu's VR Options. |
| Dominant stick up/down | Rise / descend. |
| Both triggers (hand mode) | Off by default; on in VR Options: grab the world with two hands, move it and turn it (scaling: its own switch). |
| Laser: trigger held | Grab the land and drag it. |
| Laser: two quick trigger pulls on land | Quick travel to the spot. |

## The god hand
The hand is at your controller (hand mode). **Dominant A** switches to the laser and back; with **Teleport movement** on (VR Options) it teleports instead: hold A to aim from that hand at the land (a line and a ring; red past its reach, 30 m at your scale plus twice your height), let go to be there, your head 3.5 m above the ground (a bird's-eye view, as the flat game's; take things from there with the gravity gloves). **Aim it at your creature** and the line turns pale blue, and the creature too, with a soft glow round it: let go and you are in front of it, facing it, at its middle's height and within reach to pet or slap it (further off as it grows; `[hands] teleport_to_creature=0` turns it off). Teleport anywhere else to be back at the usual height. The laser stays off then; everything else is the same. **Dominant B** held draws a gesture (`[hands] gesture_button_b=0`: B gives the laser while held, as before).

| Control | Hand mode | Laser mode |
|---|---|---|
| Trigger held (or the grip) | Pick up what your palm touches, or something on the ground just below the hand. Things in flight count within 25 cm (catching). Things bigger than 2 m (the temple) can't be touched. Hold to carry. | Pick up what the laser is on. |
| **Gravity gloves**: point an empty, open hand at something liftable out of reach and in sight (people, animals, rocks, trees (anywhere on the trunk), small things; a thing still flying, and the one you just threw; not fields, piles or buildings), up to 20 m off (people and animals about 13 m). The main hand only | Warm motes drift up about it (a light tick as it's picked out); only things in your influence and in view (within 40° of where you look), not with a menu up, and not while the hand is over a field or pile (it scoops). **Grip**: locked on (the motes brighten and stream to your palm, a steady buzz). **Flick your hand back or up**: it flies to your hand in an arc; keep the grip closed to hold it (opened by the time it arrives, it drops from your hand). Either hand. VR Options: "Gravity gloves" (on by default). | — |
| Let go | Put it down. Higher than 0.5 m above the ground it falls from the hand (a tree falls straight down and is planted there). Moving faster than 1 m/s as you let go: throw it, at your hand's speed (a hand that has stopped doesn't throw). A quick grab and let go counts too: let go before the game has the thing in its hand (a walking villager can take a moment), it drops or throws once the game has it, at your hand's speed as you let go; a quick grip is never a tap. A grown villager held over a field, a tree or a building becomes its disciple (motes drift down from it to a soft pool of light on the ground, in the role's colour). Up to 5 m above the ground a person or animal let go of is set down on the ground there, a tree planted (a scaffold: up to 20 m, built at its blueprint); higher, anything falls. | Drop or throw, at the laser point's speed. |
| A while carrying | — | Send the hand out along the laser, and recall it with what it holds. |
| Touching something with an action of its own (a rock too heavy to lift, a scroll, a pile) | It lights up (a little brighter, breathing; a building lit by daylight turns golden), instead of the old ring (`[hands] glow_models=0`: the ring for all). | — |
| Trigger, a quick tap | Tap what the hand touches, or what the game shows a tap word for (story scrolls, signs, the temple door). | Tap what the laser is on. |
| Hand on your creature, no button | Pet it (slow rubbing). A fast swipe slaps. | — |
| Carried thing held up to your creature | After 0.35 s, given if the creature can take it (also with the leash on). It comes and takes it from you. | — |
| Trigger on your creature | Put the leash on, or take it off. | The same. |
| Shake the hand left and right | Drops the leash, or a charged miracle. | — |
| Trigger held or grip on nothing | The hand closes into a fist. | Trigger held on land: drag the land. |
| Holding the village totem | Raise and lower your hand to raise and lower it (`totem_gain_x10`; 0 = forward and back, as the flat game's mouse). | Forward and back, as the flat game. |
| Left palm turned up | The game's gesture icons (its corner display in the flat game) float just above the left hand (`[ui] gesture_panel`). | The same. |
| Back of the left wrist turned to your eyes (as you'd look at a watch) | **Wrist controls** over the forearm: **Pause** / **Resume** (the game stops, you can still move about), **Slower** · speed · **Faster** (½×, 1×, 2×, as the flat game's Alt+1 / Alt+2; tap the speed for 1×), **Info** on/off (every villager's info above them). Poke one with the right index fingertip (a tick). `[hands] wrist_menu=0` turns them off. | — |
| A miracle's gesture | Hold **B** and draw the shape in the air with your fingertip; a pink trail follows it. Only what is drawn with B held counts. A spiral opens Miracle selection; then draw the miracle's shape (B held again). | Hold B and draw it with the laser on the land. |
| Carrying a scaffold | Its blueprint shows on the land below the hand and holds still; **turn your hand** left or right (as seen from above) to turn it the same way, twice as far (`blueprint_twist_x10`, 0 = it turns by itself). Let go gently and it is built there, up to 20 m above the ground (`[hands] scaffold_build_max_cm`); higher, it falls. Let go with the scaffold touching another one and they join (a thrown one still flies). | The blueprint turns with the wrist the same way; let go to build at the laser point. |
| Trigger held (or grip) on, or above (up to 3.5 m over the ground: from the teleport's height; a light tick as the hand comes over it), a store's food or wood heap or a field, then the hand taken out of it | Scoops more for as long as the hand is in the pile; taking it out ends the scoop and you carry what you took. Let go to put it down. | — |
| Other hand's grip | **Grabs too** (VR Options: "The other hand grabs too", on): with the main hand empty, it picks up (what it touches, or a person or animal just below it), carries, drops and throws as the main hand does: people, animals, rocks, trees, piles and fields, scaffolds, onto the altar, to your creature. Not miracles (it doesn't pop their bubbles), the laser, gestures, the leash or taps. The game has one hand, so one thing at a time: holding something, a grip of the other hand by it (within 20 cm) passes it across. On nothing, it closes into a fist (the god hand stays where it was). | — |
| Trigger on a miracle's bubble | Pops it: the miracle is charged in the hand. Holding the bubble (trigger held or grip): it is let go of, then popped. | The same. |
| Trigger with a charged miracle, the palm on your creature | Pours a creature's miracle (strength and the like) onto it. It takes a few seconds. | — |
| Trigger with a charged area miracle (food, wood, water…) | Pours it where the hand is. | Pours it where the laser is. |
| Trigger held with the shield, physical shield or storm | The hand turns into the laser: draw a circle on the land round the place. It is cast as the circle closes, on its middle. Let go without a circle: it stays in the hand. | The same. |
| In a creature fight | The trigger is the game's action button: tap the enemy to queue strikes, hold to block or maneuver. The hand never grabs during a fight. | The same. |

## With the creature on the leash
| Control | Does |
|---|---|
| Trigger tap on a thing (hand on it, or the laser) | Sends the creature to it. Held past a tap instead, it picks the thing up. |
| Two quick trigger pulls on a thing | Ties the leash to it. |
| Quick trigger tap on the land | Sends the creature there. Laser: the spot it's on. Hand: the ground below the hand when it's within reach of the ground, else where the controller points. |
| Trigger held (laser) | Still drags the land. |
| Grip | Never directs the creature: it only picks things up at once (with the hand, it does nothing on bare land). |
| Trigger on your creature | Unties a tied leash, else takes it off. |

## Menus, cutscenes, the tuning panel
| Control | Does |
|---|---|
| First time in the headset | The game's menu opens on a **Welcome to VR** page: turning, snap angle, movement, comfort vignette, main hand, move speed; "Start playing" goes into the game ([ui] first_setup_done). |
| Other hand's A | The game's menu (Esc). Its **VR Options** tab: smooth turning (a checkbox; off: snaps), snap angle, smooth turn speed, the comfort vignette, teleport movement (a checkbox), move speed, left-handed, the two-hand grab, throw strength, Recenter view (a button); saved to `bwvr.ini`. In a development build also a **Developer** tab: give yourself any miracle, "Autosave now" (the game autosaves 2 s after the menu closes), and every tuning setting. |
| Other hand's B | Feedback snapshot: a buzz, then both eyes, half a second of frames, the state and a copy of the log are saved (`bwvr\captureseedback_*`), with a marker in the log. Press it when something looks wrong. |
| Laser on a menu, the cinema screen or the temple interior (shown on that screen) | The mouse. The trigger clicks. |
| In a cutscene | The game moves the hand; you can't. Only the trigger's click passes through (to skip). |
| Other hand's stick click | Held 1 s: **recenter the view** (where you are, sitting or standing, becomes the view's height and forward; also on VR Options). (Development builds: a short click opens the tuning panel above the other hand; click again to close it and save the values to `bwvr.ini`.) |
| Panel open: other stick up/down | Pick a setting. Hold to scroll. |
| Panel open: other stick left/right | Change it. Hold to repeat. Moving with that stick is paused while the panel is open. |

The panel covers:
- **Throwing:** throw strength, and the hand speed that counts as a throw.
- **Buttons:** where the grip and the trigger engage and let go, and how long a trigger pull is still a tap.
- **Touch:** the margin, the smallest size, the largest size, and the reach below the hand.
- **Petting:** hand-to-cursor; higher means slaps come sooner.
- **Handing over:** how long to hold it up, how close to the creature, and the earliest the mod finishes it.
- **Dropping:** the height above the ground from which things fall from the hand.
- **Shake:** speed and number of reversals.
- **The hand:** its size and the finger angle.
- **Catching:** the reach for things in flight, and how fast they must move to count.
- **Scoop:** how far above piles and fields the hand reaches them, and how far out of a pile the hand goes before a scoop ends.
- **Laser:** the diamond's size under the laser hand.
- **Moving:** stick speed.
