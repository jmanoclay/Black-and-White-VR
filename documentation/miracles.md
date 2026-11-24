# Miracles in VR: catalogue

All 30 of the game's miracle types (`GSpellSeedInfo::Infos`, 0x190 each, the name at +0x18), how the
flat game casts each, and how each one does with the VR controls as of 2026-10-31 (commit after
7ddecae). Measured in the harness with `tools/validate/miracle_catalogue.py`: it spawns each type as
a bubble (`hands spawn_bubble <type>`), pops it into the hand with the trigger, then tries the VR
controls in turn and stops at the first one that casts it: the trigger held 3 s over the land and let
go slowly ("pour"), a swing let go at speed ("throw"), then a trigger pull with the palm on the
player's creature. Circles: `tools/validate/circle_try.py <type>`.

## The game's own data

Three fields of each type's info sort them by how the flat game casts them:

| Field | Value | Types | Meaning (from what they do) |
|---|---|---|---|
| +0x110 | 0 | food, wood, water, lightning | held: cast for as long as the Action button is held |
| | 1 | storm, fire, shield, physical shield, the two flocks | on letting go of the button (or a drawn circle) |
| | 2 | nature, heal, teleport, beam explosion, creature spells | at once, on the press |
| +0x10C | 4 | storm, shield, physical shield | cast through a drawn circle (the recogniser's action 4) |
| +0x118 | 1 | the 16 creature spells | cast onto a creature |

## The catalogue

| # | Miracle | Flat game | VR now | Notes |
|---|---|---|---|---|
| 0 | Storm | circle drawn with Action held | works (batch 1) | Was failing: pouring, throwing and the creature do nothing; the hand stays charged. Drawn as the shields are (`hands circle_type 0`, debug) it is recognised and the storm goes up on the circle's middle. Shaking it off throws it as a bubble. Now on the shields' path: `circle_try.py` (storm, shield, physical shield: 12 checks pass). |
| 1 | Nature (forest) | at once, where the hand is | works | Trigger: the trees are planted under the hand (laser: at the point). |
| 2 | Fire | thrown, or let go of slowly to drop it | works | Swing and let go throws it from the palm along the swing (`fireball_throw.py`); let go slowly it lands under the hand. |
| 3 | Food | poured, Action held | works | `miracle_hand.py food`. |
| 4 | Shield | circle drawn with Action held | works | Trigger held: the hand turns into the laser, draw a circle on the land; cast as it closes (`shield_cast.py`). |
| 5 | Physical shield | circle | works | As the shield. |
| 6 | Lightning bolt | held: bolts while held | works, aim only below the hand | Each trigger pull streams bolts (about 10 a second) onto the land under the hand while held, and the hand stays charged for more. In hand mode it can only strike straight down; the laser aims it. |
| 7 | Heal | at once | works | Cast under the hand on the press. |
| 8 | Wood | poured | works | `miracle_hand.py wood`. |
| 9 | Water | poured | works | Rain while held; it lasts several holds before it runs out. |
| 10 | Flock (flying) | on letting go | works | Cast under the hand when the trigger goes up. |
| 11 | Flock (ground) | on letting go | works | As the flying flock. |
| 12–27 | Creature spells: freeze, small, big, weak, strong, fat, thin, invisible, compassion, angry, hungry, frightened, tired, ill, thirsty, itchy | onto a creature | works (own creature) | Trigger with the palm on the player's creature: applied (poured onto it). All 16 checked. Poured on the land they do nothing and stay in the hand. Onto another player's creature: not tried (no other creature in the test save). |
| 28 | Teleport | at once | casts | A teleport point (`MagicTeleport`) is made under the hand on the press (not on the beach: there the hand stays charged). The points are for villagers and the creature, not the player: in the game's code only `Villager::ApplyThisToObject` (a villager dropped onto a point) and the creature's `MoveByTeleport` use them; a point picked up goes back into the hand as the miracle. They last a few minutes. Not confirmed: a villager let go over a point going through (2026-11-03: the points' sphere is r 0, the drop-onto's "held over" rule should take it; one villager let go on a point just walked off, and the harness couldn't grab one again to retry). |
| 29 | Beam explosion | at once | works | Cast under the hand on the press. |

## What fails or is weak, by cast style

- **Circle (storm):** the storm is a circle miracle like the shields but doesn't take the shields'
  path, so the trigger just holds the Action press and nothing is drawn. Adding it to the circle
  types is enough (tried with the debug switch).
- **Thrown, other than fire:** only the fireball takes the hand's swing. The game throws a miracle
  itself only from the cursor's speed, which a VR swing doesn't give, so the other "on letting go"
  miracles (storm, the flocks) can't be thrown from the hand, if the flat game allows it (not
  checked); the shake does throw them, as a bubble.
- **Aimed (lightning, also beam explosion, heal, nature in hand mode):** in hand mode a miracle is
  cast at the land straight under the hand; to strike something far away the player has to switch
  to the laser (A) or move. Lightning especially wants pointing.
- **Creature spells onto other creatures:** the target is the player's own creature (palm on it) or
  what the game's picker is on; other creatures are untested.
- **Teleport:** casts. The points carry villagers (dropped onto one) and the creature, not the player's view; a villager dropped onto a point in VR is not yet confirmed.

## The agreed controls (player, 2026-10-31)

| Style | Miracles | VR control | Status |
|---|---|---|---|
| Circle | storm, shield, physical shield | Trigger held: the hand turns into the laser; draw a circle on the land; cast as it closes. | storm added (batch 1), awaiting the headset |
| Thrown | fire | Swing with the trigger held and let go; let go still: it lands under the hand. | as before |
| Poured | food, wood, water | Trigger held pours under the hand (laser: at its point). | as before |
| Zap | lightning | Trigger held: bolts strike where the controller points (a beam from the hand to the land). | batch 2 done (`[hands] zap_aim`): the hand turns into the laser while held; each bolt goes to the laser's point (the game's own point, through its flat camera, fell 17 units short, in laser mode too). `lightning_aim.py`: 25 of 26 bolts within 6 units. Awaiting the headset |
| Placed | nature, heal, flocks, teleport, beam explosion | Trigger: cast on the land under the hand (laser: at its point). | as before |
| Creature | the 16 creature spells | Palm on your creature, then the trigger. | as before |
