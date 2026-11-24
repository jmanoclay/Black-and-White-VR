# VR interaction design: mechanics not yet covered

Written 2026-10-14, before implementation, as a proposal for the player to react to. Sources: the decomp's symbol map (`third_party/bw1-decomp/symbols_BW1W120.txt`) for which systems exist, and general knowledge of how the PC game plays. Details marked *(verify)* are from memory of the PC game and need checking on a save before building on them.

## Guiding rules

1. **Design for a trigger alone; the grip is a shortcut.** (Revised 2026-10-23 by the player: most controllers have only a trigger.) A quick trigger tap activates, directs, casts and taps; the trigger held past a tap (0.25 s) picks up, carries and grabs, and letting go drops or throws. The grip, where a controller has one, picks up at once: a natural extra for Index controllers, never the only way to do something. Hints name the trigger ("hold trigger" for picking up).
2. **Prefer a real motion over a button** where the game has an analogue: a throw for a throw, a pull for a tug, a rub for petting, a wrist twist for rotating.
3. **The laser is the same hand at a distance.** Everything that works by touch in hand mode should work by pointing in laser mode, with the same buttons.
4. **The left hand is the off hand.** It handles navigation, menus and spellcasting setup, so the right hand stays the god hand. Left-hand grabbing (still open) would make either hand the god hand, but only one at a time, because the game has one hand.
5. **Hints name the control**, as they do for scrolls now.

## Controller budget

| Input | Right hand (now) | Left hand (now) | Free |
|---|---|---|---|
| Grip | shortcut: pick up at once | (two-hand world grab, with the right) | left grip alone |
| Trigger | tap; held: pick up / carry (laser on land: drag); double: travel | (two-hand world grab) | left trigger alone |
| A | laser toggle | game menu (Esc) | |
| B | | | **both** |
| Stick | | move | **right stick** |
| Stick click | | | **both** |

## Mechanics not yet translated

### 1. Miracles (the biggest one)

**How the game does it.** Townsfolk pray at the Worship Site. Its spell icons (`WorshipSpellIcon`) are tapped to charge a miracle into the hand (`SpellCastInHand`, `SpellInHand*`, `SpellChargingFraction`). The hand then casts:
- by throwing it (fireball, lightning bolt);
- by holding the action button over the target (water, heal *(verify)*);
- by placing it (food, wood, forest, shield).

One-shot miracles come from floating seeds and dispensers (`SpellDispenser`, `SpellSeed`). Gestures (`GestureSystem`) are a shortcut that casts a known miracle without visiting the worship site.

**Player feedback (2026-10-14).** Miracles differ too much for one scheme; they get their own audit, one row per miracle, done when the save reaches the worship site. Starting points from the player:
- **Miracle balls** (one-shot seeds): grip to pick up, **trigger** to activate.
- **Fireball:** cast by letting go of the grip once it is in the hand (a throw).
- **Lightning:** hold the **trigger** to keep it going.
- Worship-site miracles are taken from the icons; how each is then cast is per miracle.

**Audit to do (per miracle):** how the PC game casts it (throw, hold, click, place), what charging does, how it is cancelled, then a VR control that follows the rules above.

### 2. Gestures

**How the game does it.** Hold a button and draw a glyph on the screen. The recogniser (`GestureSystem`) works on a 2D mouse path.

**VR proposal.** Hold the **right B** and draw in the air with the right hand. The mod projects the path onto a plane facing the head and feeds it to the game's recogniser.
- A faint trail shows the glyph as it is drawn. Haptic ticks follow the speed of the stroke.
- Using B, not the trigger, keeps casting and drawing apart: you can draw while holding nothing, without tapping something.
- Optional later: the left index finger draws instead (Index finger tracking), which feels more like writing in the air.
- The game also has camera gestures (go to the creature, go to the temple *(verify which exist)*). In VR, those move to the left-hand menu (§8).

### 3. Leash (the current tutorial)

**How the game does it.** Leashes hang outside the temple (`LeashObj`). Picking one up attaches it to the creature. With it held:
- clicking a target directs the creature to it (`InterfaceTap@LeashObj`, `ValidAsInterfaceLeashTarget`);
- clicking the ground walks the creature there;
- pulling back tugs the creature away (`HandStateTug`).

There are other leash types (learning, compassion, aggression), and a creature can be tied to a post (`LeashToObject`).

**VR proposal.**
- **Take the leash:** grip it.
- **Direct the creature:** **trigger** on an object, villager or ground, in both hand and laser mode. Grip keeps meaning "pick up", so a cow under the hand is no longer ambiguous.
- **Tug the creature back:** pull the controller back sharply while holding the grip. This maps directly to a physical leash. Fall back to the game's own drag if the pull is unreliable.
- **Let go:** open the grip.
- **Leash types:** these are separate leashes, so they need no new controls. Hints say which type you hold.
- Already in the plan: the leash end follows the drawn hand.

### 4. Creature training extras

- **Petting and slapping:** done, by touch. One idea for later: speed decides between a stroke and a slap (a fast swing slaps, a slow rub strokes), with haptics scaled to strength. That is more physical than relying on the game's interpretation.
- **Feeding and handing over:** done (hold the object up to the creature).
- **Creature room in the temple:** see §6.

### 5. Creature fights

**How the game does it.** Fights are their own mode (`Fight*`, the queued hits in `FightQueueHits`). The player issues attacks and blocks with mouse clicks and gestures *(verify the exact PC scheme on a save that reaches a fight)*.

**Player's recollection (2026-10-14):** during a fight you tap the enemy creature or your own to direct them, and they act on their own. So fights need no new controls: the **trigger** taps either creature, by touch or laser, like scrolls. Confirm on a save that reaches a fight.

### 6. Temple interior (Citadel)

**How the game does it.** The temple has its own camera and hand modes (`CameraModeCitadel`, `HandStateCitadel`). Its rooms (creature, miracles, save/load, options, help *(verify list)*) are 3D spaces with clickable hotspots.

**VR proposal.**
- Show it as a real 3D room at a comfortable scale, not on the cinema screen.
- Point the laser at a hotspot and press the **trigger** to activate it. The hand can also touch a hotspot when it is in reach.
- Moving between rooms uses the game's own transitions, faded to avoid sliding the camera through walls.
- The temple's save room is the in-world alternative to the Esc menu.

### 7. Building and resources

- **Scaffolds** (from the Workshop, `Scaffold`):
  - grip (or trigger held) to take one;
  - a **wrist twist** turns the blueprint while held (built 2026-10-27: the game's own turning is held off; twice the twist);
  - let go gently to build at the blueprint, from any height (the game puts it down at its hand, as in the flat game); faster than the throw speed it is thrown;
  - let go touching another scaffold to join them (the game's apply, called directly: the scaffold on the ground doesn't report to the picker while the hand holds one).
- **Stores and piles** (wood, grain, `HandStateMultiPickUp`): in the game, holding longer takes more, and the only way to finish is moving the hand away (player, 2026-10-14). In VR: hold the grip on the store to keep scooping (a haptic tick per unit); pulling the hand out of the store ends it, which is also the natural motion.
- **Feeding a construction:** drop wood on the site. This already works as dropping.
- **Disciples:** drop a villager on a workplace (farm, forest, fishery, breeder, worship). This works already, but a hint should show the disciple type before you let go.

### 8. Camera shortcuts and navigation

**How the game does it.** It has keys and gestures for going to the creature, the temple and saved camera bookmarks (`Bookmark`), plus a follow-creature camera (`CameraModeFollow`).

**VR proposal: a left-hand wrist menu.**
- Turn the left palm toward your face to show a small ring of buttons: Creature, Temple, Bookmarks 1–n, and Set bookmark.
- Touch a button with the right index finger, or point at it and press the trigger.
- The rig travels there with the existing quick-travel glide, never with a camera cut.
- Pressing the left stick in follows the creature: the rig glides along beside it, never locked to its head.
- Snap turn on the **right stick** left/right, for players who can't turn their body, in 30° steps by default.
- Right stick up/down scales the world, as a one-handed alternative to the two-hand grab.

### 9. Smaller items

| Mechanic | Proposal |
|---|---|
| Gold and silver scrolls | Done (trigger). |
| Throwing rocks or villagers at gates or targets | Done (throws). |
| Football, totems, puzzle objects | Generic grip and throw; check each when reached. |
| Influence ring | Already drawn by the game; no change. |
| Tribute / challenge tally | Show on the left wrist when the palm is turned up. |

## What the left hand does, in summary

1. **Navigation:** moving with the stick, two-hand grab, the wrist menu (§8), and following the creature.
2. **Menus:** A for the game menu; the wrist menu for shortcuts.
3. **Spell support:** B cancels a charged miracle. As an option, the left index finger draws gestures.
4. **Grabbing (still open):** whichever hand grips first becomes the god hand until it lets go. The other hand stays a passive visible hand. This keeps one game hand while letting either hand work.

## Suggested order

1. The leash fixes, already planned (unblocks the tutorial).
2. Miracles §1, when the save reaches the worship site: this is the core loop of the game.
3. Gestures §2, as a shortcut on top of §1.
4. The wrist menu and right stick §8: small, and helps everything.
5. Building and stores §7: as they come up in play.
6. Temple interior §6.
7. Fights §5: the trigger taps on creatures; check on a save that reaches one.
