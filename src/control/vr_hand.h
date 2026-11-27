// Motion controllers -> the god hand.
//
// Laser pointer: the dominant controller's ray is cast against the island with the game's own
// LH3DIsland::RayCast. The game picks (hand position, objects under the hand) from the mouse
// cursor through the camera of the first 3D pass of the frame, which in VR is the left eye, at the
// left eye's render resolution. So the hit point is projected into the left eye and that pixel
// becomes the cursor: the game's own picking then lands exactly under the laser.
// Trigger = right mouse button (pick up / act / throw), grip = left mouse button (hold the land).
#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <string>

#include "vr/controllers.h"
#include "vr/math.h"

namespace bw::hand {

// A camera as the engine projects with it: sx = cx + f * X/Z, sy = cy - f * Y/Z (camera axes `rot`).
struct EyeView {
    vr::Vec3 pos;
    vr::Basis rot;
    float f = 1, cx = 0, cy = 0;
};

// Tracking space -> world: world = base_pos + base_rot * (tracking_m * scale).
struct WorldFrame {
    vr::Vec3 base_pos;
    vr::Basis base_rot;
    float scale = 10;
};

void configure();       // [hands] in bwvr.ini
bool install_hooks();   // CHand::ThrowObject (VR throw velocity); MinHook initialised, enabled by the caller

// Scripted controllers for tests (used when SteamVR isn't driving the controllers).
const vr::Controller* mock_controllers();

// The VR rig: where the tracking space sits in the world. It starts at the game camera (level) and
// from then on only the player moves it -- grabbing land with the grip drags the world under
// the laser, the thumbstick snap-turns (x) and rises/descends (y). The game's own camera motion is
// ignored while the rig is active (B&W's mouse drag can't follow a 6-DoF pointer and moves with
// momentum, which is uncomfortable in a headset).
// Returns the frame to render this frame. `game` is the level game camera (for (re)initialising).
WorldFrame locomotion(const vr::Controller ctl[2], const vr::Pose& head, const WorldFrame& game, float dt);
void rig_reset();                    // next frame re-anchors on the game camera
// During a cutscene the game's scripted camera carries the rig: the view starts exactly at that
// camera (level, facing its heading) and then goes where it goes; the head still looks around.
// `start` on the first frame of a cutscene.
WorldFrame carry(const WorldFrame& game, const vr::Pose& head, bool start);
// The rig as it is, untouched (a cutscene on the cinema screen: the player stays put).
WorldFrame hold(const WorldFrame& game);
// A cutscene is on the cinema screen: the world isn't shown, the controller only points at the screen.
void set_cinema(bool on, bool cutscene);  // cutscene: a scene (not the temple or another view on the screen)
// Which control does what the game's hint at the hand names ("Activate", "Pick Up", "Plant"...)?
enum class HintControl { kNone, kTrigger, kDoubleTrigger, kGrip, kLetGo, kHoldTrigger };
HintControl hint_control(const wchar_t* word);
bool rig_anchored();
float rig_scale();  // world units per metre
// The game is fighting the camera position we give it (B&W shakes its camera when it's outside
// an allowed zone, e.g. the tutorial's force field). The rig then eases back to where it last
// wasn't resisted and drags are held off.
void set_game_resisting(bool resisting);
void set_game_hand(void* chand);     // the render CHand (its state tells us when it holds land)

// Once per stereo frame, before the first eye pass. Controllers in tracking-relative space.
void update(const vr::Controller ctl[2], const WorldFrame& wf, const EyeView& pick_eye);

// Near ("direct") hand: within reach the god hand is drawn at the controller instead of at the
// laser point. `blend` eases 0 (laser) -> 1 (at the controller) over ~0.15 s. The wrist frame is
// in world space: z = fingers (the controller's pointing direction), y = palm normal (the
// controller's underside), x completes it; `pos` is where the wrist goes.
// The fingers: the game's own animation, or a pose recorded from it (open: the hand hovering;
// fist: the hand holding the land), drawn whatever the game's hand is doing.
enum class HandPose { kGame, kOpen, kFist, kPoint };  // (kPoint: the index finger out, drawing a gesture)
struct NearHand {
    HandPose pose = HandPose::kGame;
    float blend = 0;
    vr::Vec3 pos, x, y, z;
    vr::Vec3 palm;       // where the hand holds the controller (world): what touches things
    float axis_len = 0;  // length the wrist bone's axes should have: a hand of constant size to the player
};
void set_depth_scale(float k);  // the eye pass's depth buffer holds z = 1 - k/Z (k = the engine's near plane)
bool near_hand(NearHand& out);
// What the hand holds, for where it sits in the hand: 1 a person or animal, 2 a miracle, 0 else.
int held_kind();
struct HeldPlace {
    bool on = true;
    float out = 0.3f;  // its middle out from the palm, a part of its half-thickness that way
    float fwd = 0, up = 0;  // then toward the fingers and up (the hand's up, as it was when taken), metres
    float out_m = 0;        // ... and out from the palm, metres (people and animals)
};
HeldPlace held_place(int kind);
// A miracle given from the debug menu: its bubble is made below the hand once the menu has closed.
void give_miracle(int type);
// The tuning settings, for the pause menu's VR options page: how many, each line's text ("name: value"),
// a step down or up (a switch: flipped), and saving them to bwvr.ini.
// The player's VR options (comfort, main hand), for the pause menu's VR Options tab.
int player_option_count();
std::wstring player_option_key(int i);  // (its bwvr.ini key)
std::wstring player_option_text(int i);
void player_option_step(int i, int dir);
// How a line shows in the menu: 0 a value (- and +), 1 a switch (a checkbox: its label and whether
// it is on), 2 an action (a button with its label).
int player_option_kind(int i);
std::wstring player_option_label(int i);
bool player_option_on(int i);
int option_count();
std::wstring option_text(int i);
void option_step(int i, int dir);
int option_kind(int i);
std::wstring option_label(int i);
bool option_on(int i);
void options_save();
// What the hand carries when it's a pile of food or wood: "food"/"wood" and the amount.
bool held_resource(const wchar_t*& kind, int& amount, vr::Vec3* top = nullptr);  // false when the hand is fully in laser mode
// The other controller's hand (drawn as a mirrored copy of the god hand); false when it isn't tracked.
bool second_hand(NearHand& out);
// The main (god) hand is the left controller: the game's right-hand mesh is drawn mirrored for it,
// and the other hand's copy unmirrored.
bool main_hand_left();
// Once a frame, from the game's hand preparing to draw: what is by the palm offered to the game's
// picker, even when no object reports itself (over open ground).
void offer_at_palm();

// The carried object as just drawn (moved to the drawn hand), from stereo's carry: its matrix
// (LH3DObject +0x14: three rows, then the position).
void note_held(void* obj3d, const float* matrix);
// A carried object let go of with a throw flies from the hand before the game launches it:
// writes its matrix now; false when this object isn't in such a flight.
bool in_flight(const void* obj3d, float* matrix);
// A tree on its way to the hand (the game still taking it): drawn at the palm (its origin into the matrix).
bool prehold_draw(const void* obj3d, float* matrix);
// The game holding a thing it hasn't named yet (it names the last thing it held meanwhile): its "held" is stale.
bool held_unnamed();
// What the hand would act on is lit up: how much brighter its 3D object is drawn (0: not this one); and
// that it was, this frame.
float glow_scale(const void* obj3d, const void* object);
void glow_seen();
bool glow_cool();  // (the creature under the teleport's aim: tinted pale blue)
// The held thing's 3D object (nothing while the game hasn't named a thing it has just taken).
void* held_object_3d_now();
// The gravity gloves' flight to the hand: true while the thing pulled (obj3d) is on its way; t 0..1,
// from: where it was (its origin) when pulled.
bool gravity_flight(const void* obj3d, vr::Vec3& from, float& t);
// How opaque the god hand is drawn: below 1 while it is outside the player's influence (it can't act there).
float hand_opacity();
// The leash is in the god hand (its loop shows at the wrist).
bool leash_in_hand_now();
// The land's height at world x, z (LH3DIsland::GetAltitude).
float ground_at(float x, float z);
// The object last let go of, while it flies from the hand or for 1.5 s after its launch; else null.
const void* recent_release();

// The tuning panel (the other hand's stick click opens and closes it): its lines, the selected
// one, and where it floats (world, above the other hand; `unit` world units per metre).
struct TuningView {
    std::vector<std::wstring> lines;
    int selected = 0;
    vr::Vec3 anchor;
    float unit = 10;
};
bool tuning(TuningView& out);  // false while it is closed
bool object_centre(const void* object, vr::Vec3& centre);  // a thing's middle (its bounding sphere's)
// A villager's info as the game asks for it (VillagerName::Add) in the eye pass `eye`: drawn as a label
// standing in the world at `at` (the controls on the back of the left wrist are drawn the same way).
void villager_label(const vr::Vec3& at, const wchar_t* text, uint32_t colour, int eye, const void* villager);
// What the gravity gloves are on (aiming or locked: the motes about it), else null: the villagers' info is shown
// for it alone.
const void* gloves_target();
// Holding a tree, the hand too high above the ground for it to be planted if let go of (it would fall).
bool tree_falls_if_let_go();

// Where the laser-mode hand belongs (world): the land point held in a drag, else where the laser
// lands on the land; false when the laser isn't on the land.
bool laser_anchor(vr::Vec3& point);
double last_tap_at();  // when the trigger last tapped something (now_s() clock)
bool take_feedback_request();
// The game object the grip would take now (to light it up), and by how much; null: nothing.
void* highlight_target(float& amount);  // the other hand's B was pressed since the last call (a feedback snapshot)
// A flat quad in the world (corners in order around it), drawn in this eye pass in front of the
// world (for the UI's panels).
void draw_panel_quad(const EyeView& eye, const vr::Vec3 (&q)[4], uint32_t colour);
void* panel_device();  // the eye passes' Direct3D 7 device (IDirect3DDevice7*), for panels drawn from the UI

// Where the laser lands this frame (world); false without a pointer ray.
bool pointer_target(vr::Vec3& hit);

// The VR option "Comfort vignette": 0 off, 1 gentle, 2 strong.
int comfort_vignette();
// Teleport movement's fade to black (1 just after a teleport, 0 after 0.3 s).
float teleport_fade();

// After an eye's 3D pass, with that eye's render target bound.
void draw(void* device, const EyeView& eye);

// Called when stereo stops: releases buttons and the cursor.
void reset();

// stereo.req commands: "ctrl <l|r> ..." (mock state), "hands ..." settings. Returns false if unknown.
bool command(const std::string& k, std::istringstream& in);

std::string stats_line();

}  // namespace bw::hand
