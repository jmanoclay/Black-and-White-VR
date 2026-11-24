#define CINTERFACE
#include "control/vr_hand.h"

#include <windows.h>

#include <intrin.h>

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <deque>
#include <cmath>
#include <cstdio>
#include <vector>

#include <MinHook.h>

#include "control/input_inject.h"
#include "render/d3d7_trace.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/paths.h"
#include "game/bw_symbols.h"
#include "game/engine.h"
#include "render/state_saver.h"
#include "vr/openvr_backend.h"

namespace bw::stereo { void watch_writes(uintptr_t addr); void request_recenter(); void request_autosave(); int drawn_recently(const void* obj3d); bool disciple_icon(const void*& obj3d, int& type); }
namespace bw::hand {
bool g_watch_spell_start = false;  // debug: hands watch_spell 1: watch writes to a new spell's start (its height)
bool fireball_launch(void* object, game::LHPoint& v);  // (below, outside the unnamed namespace)

bool disciple_too_high();  // (a villager held too high up to become a disciple; below)
void* held_adult_villager();
void* held_villager();
namespace {

using vr::Basis;
using vr::Controller;
using vr::Vec3;

// The game's mouse polling thread feeds LHMouse::UpdateCurrentPos (this = LHSys::TheSystem.mouse)
// only every ~40 ms, so a cursor driven from the headset would move the hand in 25 Hz steps.
// The pointer pushes its cursor through the same call every frame.
constexpr uintptr_t kTheMouse = 0xE85204;
struct Coord {
    long x, y;
};
void push_cursor(long x, long y) {
    reinterpret_cast<int(__thiscall*)(void*, Coord)>(sym::LHMouse_UpdateCurrentPos)(reinterpret_cast<void*>(kTheMouse), {x, y});
}

// bool32_t __fastcall LH3DIsland::RayCast(const LHPoint& from, const LHPoint& to, float* x, float* z)
constexpr uintptr_t kIslandRayCast = 0x802550;
using RayCastFn = int(__fastcall*)(const game::LHPoint* from, const game::LHPoint* to, float* x, float* z);

struct Settings {
    int dominant = vr::kRight;
    bool pointer = true;
    float max_ray = 20000.0f;     // world units
    float beam_width_m = 0.004f;  // tracking meters
    float trigger_down = 0.75f, trigger_up = 0.55f;
    float squeeze_down = 0.7f, squeeze_up = 0.4f;
    float head_clearance = 4.0f;  // world units the head stays above the ground, at 10 units per metre (scales with the rig)
    float drag_speed = 3.0f;      // max drag speed, in heights-above-ground per second  // world units the head stays above the ground
    bool beam_ztest = true;  // debug
    float depth_k = 1.8f;    // depth buffer scale in the eye passes (z = 1 - k/Z): the engine's near plane
    bool vr_throw = true;    // [hands] vr_throw: throws from the hand at the controller use its real velocity
    // Near hand: enter when the laser lands within near_in metres of the controller tip, leave
    // beyond near_out (the gap stops it flickering at the edge).
    bool near_enabled = true;
    float near_in = 0.70f, near_out = 0.85f;  // [hands] near_in_cm / near_out_cm
    // [hands] always_hand: the god hand is always at the controller; A toggles the laser (B held
    // also gives it). Off: the hand switches by its height above the ground (near_in / near_out).
    bool always_hand = true;
    bool grip_never_directs = true;  // [hands] grip_never_directs: with the leash on, the hand's grip is never a click (that directs)
    bool fist_on_grip = true;  // [hands] fist_on_grip: each drawn hand's fingers follow its own grip (fist / open)
    bool pop_with_trigger = true;  // [hands] pop_with_trigger: the trigger pops a miracle's bubble, and casts the charged miracle
    bool leash_land_tap = true;  // [hands] leash_land_tap: with the leash on, a quick trigger tap on the land sends the creature there
    float give_finish_s = 0.3f;  // the give is finished (packet 0x52) once the creature reaches for it, at least this long after it started  // ... at most this long (then it is let go, as the game does)
    bool throw_tell_game = true;  // debug: the game's throw-or-drop check is given the throw's velocity
    bool throw_flight = true;    // [hands] throw_flight: a thrown object leaves the hand as the grip opens
    float throw_min_speed_m_s = 1.0f;
    float drop_fall_m = 0.5f;    // [hands] drop_fall_cm: let go of higher above the ground than this, a thing falls from the hand  // [hands] throw_min_speed_cm_s: slower, letting go is a drop (the game puts it down)
    float hand_throw_gain = 1.5f;  // [hands] hand_throw_gain_x10: throws from the hand at the controller, x the hand's real velocity
    // A held scaffold's blueprint turns this many times as far as the wrist twists ([hands]
    // blueprint_twist_x10; 0: it turns by itself, as in the flat game).
    float bp_twist_gain = 2.0f;
    float totem_range_m = 0.6f;      // the village totem goes up and down with the hand: this much hand travel for its whole height ([hands] totem_range_cm; 0: forward and back, as the flat game's mouse)
    bool gesture_b = true;           // [hands] gesture_button_b: holding B draws a gesture (the fingertip, or the laser's point); only that counts. 0: B gives the laser while held, and any motion of the empty hand counts as drawing
    bool build_at_blueprint = true;  // [hands] build_at_blueprint: a scaffold let go of gently over its blueprint is set down there (the game builds)
    bool zap_aim = true;             // [hands] zap_aim: lightning held with the trigger strikes where the controller points (g_zap)
    bool shield_autocast = true;     // [hands] shield_autocast: the shield is cast as its circle closes, the trigger still held (0: on letting go)
    bool point_to_draw = true;       // [hands] point_to_draw: the hand points (index finger out) while B draws a gesture
    bool combine_by_touch = true;    // [hands] combine_by_touch: a held scaffold touching another it can join: letting go combines them
    float join_over_m = 1.0f;        // [hands] join_over_cm: ... or held over it, at most this far above its top
    float finger_pitch_deg = 15.0f;  // the fingers point this far below the controller's axis (towards the little finger)
    bool near_drag = false;     // 1:1 world drag with the hand at the controller (the player didn't want it)
    float travel_stop = 10.0f;  // quick travel: stop this far short of the spot (world units at 10 per metre) ...
    float travel_height = 20.0f;  // ... and come down to at most this height above the ground there
    bool touch_grab = true;      // [hands] touch_grab: the hand grabs the object it is touching, wherever that is
    float touch_margin_m = 0.05f;  // ... counting as touching this far outside the object's bounding sphere
    float touch_min_m = 0.10f;     // ... and a small object's sphere counts as at least this big (a scroll's is 8 cm)
    float touch_max_m = 2.0f;
    float highlight = 0.35f;       // what the grip would take is lit up by this much ([hands] highlight_percent; 0: off)
    bool trigger_carries = true;   // the trigger picks up and carries, as the grip ([hands] trigger_carries)
    float tap_s = 0.25f;          // a trigger pull let go within this is a tap
    bool fight_mode = true;        // in a creature fight the trigger is the game's click (debug: hands fight_mode 0)
    bool fight_pin = true;         // in a fight the held button's cursor stays where it went down (debug: hands fight_pin 0)
    bool launch_catch_up = true;   // a thrown thing keeps moving with its throw until the game's physics takes it on (debug: hands launch_catch_up 0)
    float outside_alpha = 0.4f;    // outside the influence, where it can't act, the hand is drawn this opaque ([hands] outside_alpha_percent; 100: off)
    float tip_under_hand = 0.5f;   // the laser's diamond under the laser hand, as a part of its size ([hands] tip_under_hand_percent)
    float scoop_leave_m = 0.2f;
    // A field or a pile is reached from this high above the ground over it: from the teleport's
    // bird's-eye height the hand scoops from it, as by the ground (player, 2026-11-07: "if you're right
    // over the field, grab it from the air as if you were close to it on the ground").
    float scoop_height_m = 3.5f;
    // A villager let go of with the hand higher than this above the ground falls, as anything does,
    // and becomes nothing: no disciple from far up in the sky (player, 2026-11-07: the teleport's
    // height, "village management height", and a little more).
    float disciple_max_m = 5.0f;
    float pile_reach_m = 0.5f;     // stores' piles and fields are reached from this much further ([hands] pile_reach_cm)
    // People, animals and trees are touched within at least this of their middle ([hands]
    // living_reach_cm): a villager's sphere is 4-9 cm, so beside a building (2 m across) the
    // palm was only on the building, and the grip took it (playtest 2026-10-31).
    float living_reach_m = 0.2f;
    float living_grip_m = 0.35f;
    // Where a held person or animal, and a miracle, sits in the hand (playtest 2026-11-01: people in
    // the fingers, the food horn through the thumb, the wood too far back; tune them on the panel):
    // its middle out from the palm by this part of its half-thickness, then this far toward the
    // fingers and up (metres). [hands] held_*_out_percent, held_*_fwd_cm, held_*_up_cm.
    // (People and animals: out in metres; their boxes are far smaller than they look, and a part of
    // that put a villager inside the closed hand.)
    float held_living_out = 0.06f, held_living_fwd = 0.0f, held_living_up = 0.0f;
    float held_miracle_out = 0.3f, held_miracle_fwd = 0.06f, held_miracle_up = 0.0f;  // (fwd: the player's pick, 2026-11-01)
    bool held_living_place = true;  // [hands] held_living_place: people and animals set against the palm (0: where the game holds them)  // [hands] living_grip_cm: ... and taken from this far when nothing is touched as the grip closes
    bool scoop_end = true;         // debug: end scoops when the hand leaves the pile    // a scoop ends when the hand is this far outside the pile ([hands] scoop_leave_cm)
    float catch_reach_m = 0.25f;
    float catch_speed_m_s = 1.5f;  // ... if moving faster than this ([hands] catch_speed_cm_s)   // ... and things in flight further than that ([hands] catch_reach_cm)      // ... and bigger ones (the temple) aren't touched by the hand at all
    bool two_hand = false;       // [hands] two_hand: both triggers grab the world (move, turn, scale); off: turning and moving that way was nauseating (playtest 2026-11-01)
    // [hands] two_hand_scale: pulling the hands apart or together scales the world. Off (player,
    // 2026-10-31: held things came out the wrong size at other scales): the grab moves and turns
    // only, and the scale stays the starting one.
    bool two_hand_scale = false;
    // World units per metre the player can scale between. (The near plane shrinks with the scale,
    // down to a quarter of the engine's 1.8 units; below that distant depth gets too coarse.)
    float scale_min = 2.5f, scale_max = 150.0f;
    // Size of the drawn hand: the wrist bone's axis length per world-unit-per-metre. The game
    // sizes its hand by its distance from the camera (0.005-0.006 at 10 units per metre).
    float hand_size = 0.00045f;  // (90% of 0.0005: the player's pick)
    float pet_gain = 0.75f;      // petting: how much of the hand's motion over the creature the game's cursor makes (the game
                                // slaps on fast cursor motion, and a hand near the eyes crosses many pixels)
    // [hands] give: an object held up to a creature for give_dwell seconds is handed to it (the
    // game's action button on the creature while carrying; no need to let go).
    bool give = true;
    float give_dwell = 0.35f;
    float give_margin_m = 0.10f;  // how far outside the creature's bounding sphere still counts
    bool tap = true;            // [hands] tap: touching a tappable thing (a scroll, a door) taps it; so does the laser's trigger
    bool leash_to_hand = true;  // [hands] leash_to_hand: the held leash starts at the drawn VR hand, not the game's hand
    // [hands] shake_speed_cm_s, shake_reversals: the hand shaken left and right (this many
    // reversals, each faster than this, in quick succession) is the game's shake: it drops the
    // leash, or a miracle held in the hand.
    float shake_speed_m_s = 1.0f;
    int shake_reversals = 4;
    float reach_m = 0.25f;      // [hands] reach_cm: the hand also grabs what is just below it, this close to the ground
    bool ground_marker = false; // [hands] ground_marker: mark the ground point below the hand (from before touch grabbing)
    // [hands] pet: a hand on a creature strokes or slaps it without any button (the game's action
    // button is held for it; slow rubbing strokes, a fast swipe slaps, as with the mouse).
    bool pet = true;
    // [hands] slap_speed_cm_s / stroke_speed_cm_s: the hand's own speed on the creature that slaps
    // it, and the slower one that strokes it (held a moment, so a swing on its way to a slap isn't
    // taken for a stroke). The game itself decides on the cursor's speed once a second.
    bool pet_by_speed = true;
    float slap_m_s = 1.2f, stroke_m_s = 0.3f;
    // Player comfort settings (the pause menu's VR Options tab; 2026-11-01).
    bool turn_smooth = false;        // [hands] smooth_turn: the stick turns smoothly (off: in snaps)
    float snap_deg = 30.0f;          // [hands] snap_turn_deg
    float smooth_turn_dps = 90.0f;   // [hands] smooth_turn_deg_s: at full stick
    bool move_teleport = false;      // [hands] move_teleport: the main hand's A aims a teleport (held; let go: there at once) instead of switching the laser
    float teleport_reach_m = 30.0f;  // [hands] teleport_reach_m: how far a teleport reaches (metres at the player's scale, plus twice the height above the ground)
    float teleport_height_m = 3.5f;  // (bird's-eye, as the flat game's view: the gravity gloves reach the ground from there; 2026-11-05)
    bool gravity = true;               // [hands] gravity_gloves: point at a thing, grip to lock on, flick back to pull it to the hand
    float gravity_range_m = 20.0f;     // [hands] gravity_reach_m: how far (metres at the player's scale; 40 reached things not even drawn)
    float gravity_min_m = 0.6f;        // ... nearer than this, the hand's own grab
    float gravity_size_reach = 30.0f;  // ... and no further than this many times the thing's own radius (small things fade from view)
    float gravity_cone_deg = 9.0f;     // [hands] gravity_cone_deg: how near the aim a thing must be
    float gravity_max_size_m = 1.5f;   // ... and no bigger than this (its bounding sphere's radius)
    float gravity_flick_m_s = 1.1f;    // [hands] gravity_flick_cm_s: the hand's speed back or up that pulls
    float gravity_flight_s = 0.4f;     // the flight to the hand
    bool carry_ray = true;             // [hands] carry_ray: a grown villager carried: a ray to the ground, tinted by the disciple it would become
    bool offhand = true;               // [hands] offhand_grab: the other hand grabs, carries, drops and throws too (not miracles)
    float offhand_pass_m = 0.2f;       // ... a grip of the other hand this near the hand holding something passes it across  // [hands] teleport_height_cm: the head this far above the ground after a teleport (the hand reaches it without stooping)
    int comfort_vignette = 0;        // [hands] comfort_vignette: the view's edges darkened while moving or turning smoothly: 0 off, 1 gentle, 2 strong
    float move_scale = 1.0f;         // [hands] move_speed_percent: the left stick's speed, times this
    bool dominant_left = false;      // [hands] dominant_left: the main hand (the god hand) is the left
    float move_speed = 1.5f;    // left stick: heights-above-ground per second ...
    float move_min_mps = 1.5f;  // ... and at least this many (player-scale) metres per second
    // [hands] follow_ground: moving with the stick, the head keeps its height above the land (as set with
    // the other stick, or by a teleport), over hills and down into valleys (player, 2026-11-13: it went
    // into a hill, then over it).
    bool follow_ground = true;
    // [hands] glow_models: what the hand would act on is lit up itself, not ringed (the ring only for what
    // isn't drawn as a model).
    bool glow_models = true;
    // [hands] teleport_to_creature: the teleport aimed at the creature puts the player in front of it to
    // pet it (teleport_creature_front 0: on the player's side of it, not its front).
    bool teleport_to_creature = true;
    bool teleport_creature_front = true;
    // [hands] scaffold_build_max_cm: a scaffold let go of up to this high above the ground is still built
    // at its blueprint (people and animals: disciple_max_m); higher, it falls. (Player, 2026-11-13:
    // buildings get big, placed from further out.)
    float scaffold_max_m = 20.0f;
    float wrist_back_m = 0.08f;  // the wrist sits this far behind the controller along the fingers
    float blend_time = 0.25f;
};
Settings g_set;

Controller g_mock[2];

struct PointerState {
    bool valid = false;  // a ray this frame
    Vec3 origin, dir;    // world
    Vec3 hit;            // world (end of the beam)
    bool hit_land = false;
    long cursor_x = 0, cursor_y = 0;
    bool cursor_ok = false;
};
PointerState g_ptr;

struct Near {
    bool on = false;      // near mode (target of the blend)
    float blend = 0;      // 0 laser .. 1 at the controller
    Vec3 pos, x, y, z;    // target wrist frame (world)
    bool dragging = false;
    Vec3 grab_world;      // world point held by the hand during a near drag
    Vec3 ground;          // the ground straight below the controller tip: what the trigger grabs
    bool in_reach = false;  // ... when the hand is this close to it
    Vec3 palm;              // where the hand holds the controller (world): what touches things
};
Near g_near;
bool g_laser = false;  // always_hand: the laser is toggled on (A)
int g_hand = vr::kRight;  // the controller the god hand follows: the main hand, or the other while it acts (choose_hand)
double g_hand_switched_at = -10;
// The laser is on the game's 2D (a menu on the virtual screen): it is the mouse there.
bool g_ui_pointing = false;
long g_ui_x = 0, g_ui_y = 0;
bool g_cinema = false;  // a cutscene on the cinema screen
struct LeashTap {
    void* target = nullptr;
    double at = 0;
};
LeashTap g_leash_tap;      // a trigger pull on a thing with the leash: a tap sends the creature, a hold picks it up
bool g_trigger_grab = false;  // the trigger is carrying (held past a tap, on something the hand can lift)
bool g_trigger_past_tap = false;  // the trigger has been held past a tap (laser: only then the land drag starts)
bool g_fight = false;  // the player's creature is in a fight (GInterface fn_005D2990): the trigger is the game's click
bool g_pouring = false;  // the trigger holds the game's action button to pour an area miracle
// The shield (player, 2026-10-27): in the flat game it is cast by holding the action button with it
// in the hand and drawing a circle on the land round the place to shield (the game reads the
// circle from its gesture samples, like any gesture). With the shield in the hand, the trigger held
// draws: the hand turns into the laser (the circle goes where it points), the samples start clean
// and take the laser point's path (with the pink trail), and when the shield is cast (the hand no
// longer charged) or the trigger is let go of, the hand comes back.
bool g_shield_draw = false;
// Lightning in the hand, the trigger held: the game's Action press is held and the bolts strike where
// the controller points, not only straight down under the hand (the agreed "zap" control,
// documentation/miracles.md), from the fingertip, the hand staying at the controller and pointing (it turned
// into the laser at first: the bolts came from out there; playtest 2026-11-01). ([hands] zap_aim)
bool g_zap = false;
bool g_zap_from_finger = true;  // (debug: hands zap_from_finger 0: lightning from the land below the hand, as the game has it)
Vec3 g_shield_pen{};  // the laser's point on the land while the shield's circle is drawn
std::vector<Vec3> g_shield_pts;  // all of them since the trigger went down (from 0.15 s after: the hand turning into the laser)
double g_shield_down_at = -10;
double g_shield_tail_until = -10;  // after the button goes up: the laser and the pen kept a moment (the game casts on the release)
// The shields' spell seed types (GSpellSeedInfo::Infos, 0x190 each, the name at +0x18): 4 SHIELD,
// 5 PHYSICAL_SHIELD. (Both are cast "after the recognition system": info +0x110 = 1.)
// The storm is cast the same way (info +0x10C = 4, as the shields'): with the Action press held a
// circle drawn round the place, the storm going up at its middle. Without the circle's path the
// trigger only held the press and nothing could cast it (2026-10-31, documentation/miracles.md).
int g_dbg_circle_type = -1;  // debug: hands circle_type <type>: another miracle drawn as a circle, to try it
bool is_shield_type(int t) { return t == 0 || t == 4 || t == 5 || (t >= 0 && t == g_dbg_circle_type); }
bool g_cutscene = false;  // ... and it is a cutscene (not the temple or another view shown there)
// Laser quick travel (double squeeze): the rig glides to the pointed spot.
struct Travel {
    bool on = false;
    Vec3 from, to;
    float t = 0;
    float dur = 0.35f;
};
Travel g_travel;
// Teleport movement (the VR option "Teleport movement"): the main hand's A, held, aims from that hand
// at the land; let go, the player is there at once, behind a short fade to black, the head
// teleport_height_m above the ground there (3.5 m: a bird's-eye view, as the flat game's, with the
// gravity gloves to take things from there; it was 1.1 m, low enough to reach the ground, which lost
// the view: player, 2026-11-05). A switches the laser otherwise; with teleport on the laser stays off, and every other
// control is as it was (player, 2026-11-04: "the only thing turning on teleport should do is replace
// the laser pointer (A) with the teleport"). Its reach is capped: a teleport far across the island
// felt odd (2026-11-03); quick travel is for that.
struct TeleportAim {
    bool aiming = false, valid = false;
    Vec3 from, target;
    // Aimed at the player's creature (player, 2026-11-13: petting and slapping from the teleport's
    // height): it lights up, the line turns pale blue, and the teleport puts the player in front of it,
    // facing it, close enough to reach it, at its middle's height (all by its size, which grows).
    bool creature = false;
    Vec3 creature_c;
    float creature_r = 0;
    Vec3 local_pos, local_dir{0, 0, -1};  // (the controller's tip and aim in the play area: drawn from as the rig is when drawn)
};
TeleportAim g_tp;
// Gravity gloves (player, 2026-11-05: "mimic the Half-Life: Alyx gravity gloves as much as
// possible"), for things out of the hand's reach: with an empty hand at the controller, what the
// hand points at (the nearest liftable thing to the aim within a narrow cone, out to
// gravity_range_m) glows; the grip locks on to it (a thread of energy from the palm, a steady buzz);
// a flick of the hand back toward the player, or up, pulls it: the game's hand takes it there (the
// cursor and the game's pick put on it) and it is drawn flying to the palm in an arc, into the hand
// the grip still holds. Let go, and it drops.
struct GravityGlove {
    enum State { kIdle, kTarget, kLocked, kPulling, kFlying } st = kIdle;
    void* target = nullptr;
    Vec3 c;            // the target's middle (its bounding sphere), and its radius
    float r = 0;
    bool in_flight = false;  // (a thing flying: its ring is drawn where it is drawn)
    void* c_for = nullptr;     // (the target c_off was taken for)
    Vec3 c_off;                // its middle from its drawn origin, as first seen: the glow follows where it is drawn
    double at = -10;   // when the state began
    double tick_at = -10;
    Vec3 fly_from;     // where it was when the pull began (its origin), and when it was taken
    void* fly_obj3d = nullptr;
    double fly_at = -10;
};
GravityGlove g_gg;

double g_tp_at = -10;  // the last teleport (the fade)
struct NearDebug {
    float beam_m, above_m;
    bool b, busy;
};
NearDebug g_near_dbg{};  // inputs of the last near/far decision, for the stats line

// One-Euro filter (Casiez et al.): heavy smoothing when the ray is still, little lag when it
// moves fast. Far targets magnify hand tremor, so the aim ray is filtered before the cast.
struct OneEuro {
    float min_cutoff = 1.5f, beta = 4.0f, d_cutoff = 1.0f;
    bool init = false;
    Vec3 x, dx;
    static float alpha(float cutoff, float dt) {
        const float tau = 1.0f / (6.2831853f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }
    Vec3 filter(Vec3 v, float dt) {
        if (!init || dt <= 0) {
            init = true;
            x = v;
            dx = {};
            return v;
        }
        const Vec3 d = (v - x) * (1.0f / dt);
        dx = dx + (d - dx) * alpha(d_cutoff, dt);
        const float cutoff = min_cutoff + beta * vr::length(dx);
        x = x + (v - x) * alpha(cutoff, dt);
        return x;
    }
};
OneEuro g_aim_dir_filter, g_aim_pos_filter;

// Recent laser hit points, for the throw velocity (world units per second).
struct HitSample {
    double t;
    Vec3 p;
};
std::vector<HitSample> g_hit_history;
double now_s() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}
// Throw velocity: the fastest ~60 ms of laser-point motion in the last 150 ms (people slow down
// a little just before letting go, and the game's own estimate catches that slowdown).
bool hit_velocity(Vec3& v, double window = 0.15) {
    if (g_hit_history.size() < 2 || now_s() - g_hit_history.back().t > 0.1) return false;
    const double end = g_hit_history.back().t;
    float best = -1;
    for (size_t j = g_hit_history.size(); j-- > 0;) {
        const HitSample& b = g_hit_history[j];
        if (b.t < end - window) break;
        const HitSample* a = nullptr;
        for (size_t i = j; i-- > 0;)
            if (g_hit_history[i].t <= b.t - 0.06) {
                a = &g_hit_history[i];
                break;
            }
        if (!a || b.t - a->t < 0.01) continue;
        const Vec3 w = (b.p - a->p) * float(1.0 / (b.t - a->t));
        if (vr::length(w) > best) best = vr::length(w), v = w;
    }
    return best >= 0;
}
// The hand's velocity as it lets go (the hand at the controller): the fastest stretch of the last
// 0.35 s (the grip reads open a little after the fingers start to), but only while the hand is
// still moving. A hand swept, stopped and let go of half a second later still threw at the sweep's
// speed (player, 2026-11-07: a villager put down gently by a tree was tossed). Slower than half
// that peak and than the throw speed over the last 80 ms, it is the hand's speed now.
bool g_dbg_release_log = true;
bool release_velocity(Vec3& v, float throw_min) {
    Vec3 peak;
    if (!hit_velocity(peak, 0.35)) return false;
    v = peak;
    const double end = g_hit_history.back().t;
    const HitSample* a = nullptr;
    for (size_t i = g_hit_history.size() - 1; i-- > 0;)
        if (g_hit_history[i].t <= end - 0.08) {
            a = &g_hit_history[i];
            break;
        }
    if (!a || end - a->t < 0.01) return true;
    const Vec3 now_v = (g_hit_history.back().p - a->p) * float(1.0 / (end - a->t));
    if (vr::length(now_v) < 0.5f * vr::length(peak) && vr::length(now_v) < throw_min) {
        if (g_dbg_release_log && vr::length(peak) > throw_min)
            BWLOG("hands: let go: the hand had stopped (%.2f now, %.2f at its fastest): not thrown", vr::length(now_v), vr::length(peak));
        v = now_v;
    }
    return true;
}

// A grip let go of before the game had hold of what it was taking: the release is carried out once it
// has (see the press logic), with the hand's speed as the grip opened.
struct PendingRelease {
    bool on = false;
    double t = -10;
    Vec3 v;
    bool has_v = false;
};
PendingRelease g_pending_release;
double g_holding_since = -1;  // since when the game's hand has held what it holds
bool g_hold_through_taking = true;  // (hands prehold <0|1> [hold 0|1])
// The hand's velocity for a release: the one kept from when the grip opened, if the release waited
// for the game; else as it is now.
bool release_velocity_now(Vec3& v, float throw_min) {
    if (g_pending_release.on) {
        v = g_pending_release.v;
        return g_pending_release.has_v;
    }
    return release_velocity(v, throw_min);
}

// The throw velocity as the grip opened.
struct Release {
    double t = -10;
    Vec3 v;
    bool hand = false;  // thrown from the hand at the controller
    Vec3 pos;           // ... which was here
    void* obj3d = nullptr;  // ... the thing let go of (a scaffold a builder threw then took the hand's speed: 2026-11-07)
    // A tree let go of, not thrown: launched still, from the ground below where it was let go of. (A
    // tree launched upright is planted where it is launched: let go of with some speed left in the
    // hand, it drifted and was planted in the air; player, 2026-11-07. Given to the game instead, it
    // fell over, uprooted.)
    bool plant = false;
    bool high = false;  // let go of high up: the game is told it was thrown, so it falls (see add_object_detour)
};
Release g_release;
float ground_height(float x, float z);  // (below)

// A throw from the hand at the controller leaves the hand as the grip opens. The game only
// launches it on its next turn after the drop click (0.15-0.2 s later); until then it would stay
// with the hand, then hang still, then start from where the hand was when the grip opened, now
// well behind the hand. So from the grip opening the object flies on its own: from where it was
// drawn, with the launch velocity. The launch then starts it from that point of its path.
struct Flight {
    const char* obj3d = nullptr;  // null: no flight
    double t0 = 0;
    Vec3 start, v;  // world units, units per second
    float rot[9] = {};  // the matrix rows it had in the hand (kept: something stands it upright meanwhile,
                        // and a tree launched upright is planted again where it is)
    float last[12] = {};  // the matrix as last written here
    bool wrote = false;
};
Flight g_flight;
// After the launch the game moves the thrown thing only on its turns (10 a second): it hung still
// in the air for a few frames after leaving the hand, then jumped ahead (player, 2026-10-23: "it
// stops moving, then a couple frames later jumps forward"). For a short while after the launch it
// is drawn where its throw has carried it, until the game's own position catches up.
struct AfterLaunch {
    const char* obj3d = nullptr;
    double t0 = 0;
    Vec3 start, v;
    bool logged = false;
};
AfterLaunch g_after_launch;
// A pick-up on its way (playtests 2026-11-07: "a half second to a second between the grip and the
// thing appearing in the hand"; a rock caught in the air flew on, then jumped into the hand): from the
// grip the game's hand takes a moment to have it (its state 3, "taking"; measured in the player's
// logs: usually 0.03-0.17 s, a walking villager up to 1.8 s, a tree pulled up 0.5-0.9 s), and until
// then the thing stays where it is, or flies on. Meanwhile it is drawn at the palm (its matrix written
// each frame, as the throws' catch-up does); if the game doesn't take it, it is put back where it was.
struct PreHold {
    void* object = nullptr;
    char* obj3d = nullptr;
    double t0 = -10;
    Vec3 off;              // its middle from its origin
    float r = 0;
    float saved[12] = {};  // its matrix as the pick-up began
    float last[12] = {};   // as last written here
    bool wrote = false;
    double held_wait = -1;  // (the game holding, not yet naming what)
    // A tree is only drawn at the palm, its matrix left alone: the game makes the hole it was pulled
    // from where the tree then is, and it was left in the air at the palm (player, 2026-11-13).
    bool draw_only = false;
    bool has_draw = false;
    Vec3 draw_at;  // (its origin, as drawn)
    // The game holding it, not yet naming it: it names what it holds at its next turn (0.1 s on in the
    // harness, 0.34 s in a playtest), and meanwhile names the last thing it held, or nothing. A rock
    // goes straight to holding (no state 3), and stayed on the ground till then (player, 2026-11-13).
    bool unnamed = false;
};
PreHold g_prehold;
bool g_prehold_on = true;  // (hands prehold 0: as the game does it)
bool g_prehold_tree_draw_only = true;  // (hands prehold 1 1 0: a tree's matrix written too, as before 2026-11-13)
Vec3 flight_pos(const Flight& f, double t) { return f.start + f.v * float(t - f.t0); }

// The carried object as last drawn (stereo's carry moves it to the drawn hand).
struct HeldSeen {
    const char* obj3d = nullptr;
    Vec3 pos;
    float rot[9] = {};  // its matrix rows
};
HeldSeen g_held_seen;

// Debug ([hands] throw_trace): every frame from the grip opening to a second after the launch,
// where the palm and the released object are.
struct FlightTrace {
    bool on = false;
    double t0 = -10, launched = -10;
    const char* obj3d = nullptr;
    const char* object = nullptr;  // the game object, after the launch
    const char* phys = nullptr;    // its PhysicsObject
};
FlightTrace g_flight_trace;

// Where a thrown object is (Game3dObject matrix row 3), for the log: at the throw and a second later.
// (Also the steps of its first half second, and who takes it out of physics: a pig thrown by the
// player near the temple came straight down where it was launched, which no test reproduced.)
struct ThrownWatch {
    const char* obj3d = nullptr;
    double t = 0;
    Vec3 start;
    void* object = nullptr;  // the game object, while it is in its first seconds
    int steps_logged = 0;
    bool went_logged = false;
};
ThrownWatch g_thrown;
bool object_pos(const char* obj3d, Vec3& p) {
    if (!obj3d || IsBadReadPtr(obj3d + 0x14, 48)) return false;
    const float* m = reinterpret_cast<const float*>(obj3d + 0x14);
    p = {m[9], m[10], m[11]};
    return true;
}

// static PhysicsObject* PhysicsObject::AddObject(Object*, const LHPoint& velocity,
//     const LHPoint& angular, Object* thrower, GInterfaceStatus*): where a released object
// actually enters physics. (CHand::ThrowObject only lets go of it visually, afterwards.) The game
// launches it from its own hand position with its own velocity estimate; a throw from the hand
// at the controller instead leaves from that hand, with the hand's velocity as the grip opened.
// Object::InitialisePhysicsFromHand follows the launch with PhysOb::AdjustToGroundLevel
// (0x7FCB80, __thiscall(PhysOb*, bool only_raise, bool align_to_slope)), which moves the object
// until its lowest point sits on the ground; only up when only_raise. The game asks for
// only_raise when its own throw velocity is more than ~1.4 units/s across the ground. Its hand
// hardly moves when the player's does, so a throw from the hand at the controller (from wherever
// it is) started on the ground below it. For our own launches it may only lift an object out of
// the ground, and doesn't tip it to the slope.
struct HandLaunch {
    void* physob = nullptr;  // the launched object's PhysOb (PhysicsObject + 0x28)
    double t = -10;
};
HandLaunch g_hand_launch;
// static void PhysicsObject::RemoveObject(Object*, bool, bool): logged for a just-thrown object.
constexpr uintptr_t kPhysicsRemoveObject = 0x646A00;
using RemoveObjectFn = void(__cdecl*)(void* object, bool a, bool b);
RemoveObjectFn o_remove_object = nullptr;
void __cdecl remove_object_detour(void* object, bool a, bool b) {
    if (object && object == g_thrown.object) {
        Vec3 p;
        object_pos(g_thrown.obj3d, p);
        BWLOG("hands: thrown object out of physics after %.2f s at (%.1f, %.1f, %.1f) (%d %d, from %p)", now_s() - g_thrown.t, p.x, p.y,
              p.z, a, b, _ReturnAddress());
        g_thrown.object = nullptr;
    }
    o_remove_object(object, a, b);
}
constexpr uintptr_t kAdjustToGroundLevel = 0x7FCB80;
using AdjustFn = void(__fastcall*)(void* physob, void* edx, bool only_raise, bool align);
AdjustFn o_adjust = nullptr;
void __fastcall adjust_detour(void* physob, void* edx, bool only_raise, bool align) {
    if (physob == g_hand_launch.physob && now_s() - g_hand_launch.t < 0.5) {
        g_hand_launch.physob = nullptr;
        only_raise = true, align = false;
    }
    o_adjust(physob, edx, only_raise, align);
}
// An object's class from its RTTI (vtable[-1]: complete object locator, +0xC type descriptor,
// the name at +8), e.g. ".?AVPig@@"; "?" if unreadable.
bool g_dbg_still_game_hand = false;  // debug: the game's own throw velocity zeroed, as with a real headset
const char* class_name(const void* object) {
    const char* const* vt = object && !IsBadReadPtr(object, 4) ? *static_cast<const char* const* const*>(object) : nullptr;
    if (!vt || IsBadReadPtr(vt - 1, 4)) return "?";
    const char* col = vt[-1];
    const char* td = col && !IsBadReadPtr(col + 0xC, 4) ? *reinterpret_cast<const char* const*>(col + 0xC) : nullptr;
    return td && !IsBadReadPtr(td + 8, 4) ? td + 8 : "?";
}
// Is the object of that class or one derived from it (".?AVLiving@@")? From its RTTI: the
// complete object locator's class hierarchy (+0x10), its base classes (+8 count, +0xC array of
// descriptors, each with its type descriptor at +0).
bool is_a(const void* object, const char* name) {
    const char* const* vt = object && !IsBadReadPtr(object, 4) ? *static_cast<const char* const* const*>(object) : nullptr;
    if (!vt || IsBadReadPtr(vt - 1, 4)) return false;
    const char* col = vt[-1];
    const char* chd = col && !IsBadReadPtr(col + 0x10, 4) ? *reinterpret_cast<const char* const*>(col + 0x10) : nullptr;
    if (!chd || IsBadReadPtr(chd + 8, 8)) return false;
    const uint32_t n = *reinterpret_cast<const uint32_t*>(chd + 8);
    const char* const* bases = *reinterpret_cast<const char* const* const*>(chd + 0xC);
    if (n > 64 || !bases || IsBadReadPtr(bases, n * 4)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const char* bcd = bases[i];
        const char* td = bcd && !IsBadReadPtr(bcd, 4) ? *reinterpret_cast<const char* const*>(bcd) : nullptr;
        if (td && !IsBadReadPtr(td + 8, 4) && strcmp(td + 8, name) == 0) return true;
    }
    return false;
}
// What a person, animal or tree the palm is within reach of claims to the picker: ahead of
// buildings, fields, piles and rocks (whose claims are 100 x their size above -1e6), behind a
// miracle's bubble; among them the nearest, a big tree (its sphere more than the range) a little
// behind a villager by it.
float living_claim(float touch_r, float range, float dist) { return -1.0e6f + touch_r / std::max(range, 1e-3f) + dist / std::max(touch_r, 1e-3f); }
// People, animals, trees: what the hand most often means to take, small beside buildings.
bool is_a(const void* object, const char* name);
bool is_tree(const void* object) {  // (by vtable, remembered)
    static std::vector<std::pair<const void*, bool>> seen;
    const void* vt = object && !IsBadReadPtr(object, 4) ? *static_cast<const void* const*>(object) : nullptr;
    if (!vt) return false;
    for (const auto& k : seen)
        if (k.first == vt) return k.second;
    const bool yes = is_a(object, ".?AVTree@@") || is_a(object, ".?AVDeadTree@@");
    seen.push_back({vt, yes});
    return yes;
}
bool living_or_tree(const void* object) {
    struct Seen {
        const void* vt;
        bool yes;
    };
    static std::vector<Seen> seen;  // (by vtable)
    const void* vt = object && !IsBadReadPtr(object, 4) ? *static_cast<const void* const*>(object) : nullptr;
    if (!vt) return false;
    for (const Seen& k : seen)
        if (k.vt == vt) return k.yes;
    const bool yes = is_a(object, ".?AVLiving@@") || is_a(object, ".?AVTree@@");
    seen.push_back({vt, yes});
    return yes;
}
constexpr uintptr_t kPhysicsAddObject = 0x6443A0;
using AddObjectFn = void*(__cdecl*)(void* object, const game::LHPoint* velocity, const game::LHPoint* angular, void* thrower,
                                    void* status);
AddObjectFn o_add_object = nullptr;
void* __cdecl add_object_detour(void* object, const game::LHPoint* velocity, const game::LHPoint* angular, void* thrower,
                                void* status) {
    if (object && velocity && strcmp(class_name(object), ".?AVMagicFireBall@@") == 0) {
        game::LHPoint v = *velocity;
        const bool ours = fireball_launch(object, v);
        char* o3 = *reinterpret_cast<char**>(static_cast<char*>(object) + 0x40);
        const float* m = o3 && !IsBadReadPtr(o3 + 0x14, 0x30) ? reinterpret_cast<const float*>(o3 + 0x14) : nullptr;
        BWLOG("hands: launch: a fireball, game velocity (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)%s from (%.1f, %.1f, %.1f), thrower %p status %p",
              velocity->x, velocity->y, velocity->z, v.x, v.y, v.z, ours ? " [hand]" : "", m ? m[9] : 0.0f, m ? m[10] : 0.0f, m ? m[11] : 0.0f,
              thrower, status);
        return o_add_object(object, &v, angular, thrower, status);
    }
    if (!status || thrower || !object || !velocity) return o_add_object(object, velocity, angular, thrower, status);
    if (g_dbg_still_game_hand) *const_cast<game::LHPoint*>(velocity) = {0, 0, 0};  // (debug: as in the headset)
    // Laser throws keep the game's own velocity (it follows the laser point already, and the
    // player was happy with it); measured, the game's unit is world units per second.
    char* obj3d = *reinterpret_cast<char**>(static_cast<char*>(object) + 0x40);
    const bool ours = g_set.vr_throw && g_release.hand && now_s() - g_release.t < 1.0 && (!g_release.obj3d || g_release.obj3d == obj3d);
    game::LHPoint v = *velocity;
    float* m = obj3d ? reinterpret_cast<float*>(obj3d + 0x14) : nullptr;
    // A throw already on its way leaves from where its flight has got to, as it was in the hand
    // (the physics starts from the matrix: PhysOb::SetUpPos).
    const bool flying = ours && g_flight.obj3d && g_flight.obj3d == obj3d;
    if (ours) {
        const float k = g_set.hand_throw_gain;
        v = {g_release.v.x * k, g_release.v.y * k, g_release.v.z * k};
        Vec3 p = g_release.pos;
        if (flying) {
            p = flight_pos(g_flight, now_s()), v = {g_flight.v.x, g_flight.v.y, g_flight.v.z};
            if (m) memcpy(m, g_flight.rot, sizeof(g_flight.rot));
        }
        if (g_release.plant) {
            p.y = std::max(0.0f, ground_height(p.x, p.z));
            v = {0, 0, 0};
        }
        if (m) m[9] = p.x, m[10] = p.y, m[11] = p.z;
        if (m && g_set.launch_catch_up) g_after_launch = {obj3d, now_s(), p, Vec3{v.x, v.y, v.z}, false};
    }
    g_flight.obj3d = nullptr;
    BWLOG("hands: launch: %s game velocity (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)%s from (%.1f, %.1f, %.1f)", class_name(object),
          velocity->x, velocity->y, velocity->z, v.x, v.y, v.z, flying ? " [hand, in flight]" : ours ? " [hand]" : " [game's]",
          m ? m[9] : 0.0f, m ? m[10] : 0.0f, m ? m[11] : 0.0f);
    // After the launch, Object::InitialisePhysicsFromHand decides from the velocity it was given
    // (this one: the game's own, all but still when the player's hand moves) whether the hand
    // threw or dropped. Under ~1.4 units/s across the ground it is a drop: the object is put on
    // the ground, and a living thing (a villager, an animal) is taken out of physics there and
    // then (PhysicsObject::RemoveObject from 0x6373C5); trees aren't. So for a throw it is told
    // the throw's velocity, at least that fast across the ground. (Physics itself gets `v`.)
    // (Measured, 2026-11-08: the game's line is 2 units/s across the ground (vx^2 + vz^2 over 4.0 is a
    // throw, at 0x6370AB), not 1.4; told 1.5, a thing let go of gently, high up, was "set down": a
    // person or animal stood on the ground at once.) Let go of gently below the high line, it is still
    // told 1.5: set down, as a comfort. High up, or thrown, it is told a throw: it falls.
    if (flying && g_set.throw_tell_game) {
        game::LHPoint seen = v;
        const float want = g_release.high || std::sqrt(v.x * v.x + v.z * v.z) > 2.0f ? 2.2f : 1.5f;
        const float across = std::sqrt(seen.x * seen.x + seen.z * seen.z);
        if (across < want) {
            if (across > 1e-3f) seen.x *= want / across, seen.z *= want / across;
            else seen.x = want;
        }
        *const_cast<game::LHPoint*>(velocity) = seen;
    }
    if (obj3d) g_thrown = {obj3d, now_s(), m ? Vec3{m[9], m[10], m[11]} : Vec3{}, object, 0, false};
    g_release.t = -10;  // one object per release
    void* phys = o_add_object(object, &v, angular, thrower, status);
    if (flying && phys) g_hand_launch = {static_cast<char*>(phys) + 0x28, now_s()};
    if (g_flight_trace.on) {
        g_flight_trace.launched = now_s(), g_flight_trace.obj3d = obj3d;
        g_flight_trace.object = static_cast<const char*>(object), g_flight_trace.phys = static_cast<const char*>(phys);
    }
    return phys;
}

// Picking. The game picks while it draws: every pickable object that is queued for drawing goes
// through GInterface::SendObjectDrawCollision(object, distance, lh3d). With distance 0 the game
// tests the object against the mouse ray itself; with any other distance it takes the caller's
// word. Either way the object with the smallest distance is what a click acts on.
// For the hand at the controller, an object whose bounding sphere the palm is inside is reported
// with a distance below anything the game computes, so the grip takes what the hand touches --
// in a tree's crown, on a roof, in the air -- before whatever is under the cursor.
float ground_height(float x, float z);
constexpr uintptr_t kSendObjectDrawCollision = 0x5D56C0;
using CollideFn = int(__thiscall*)(void* iface, void* object, float distance, void* lh3d);
CollideFn o_collide = nullptr;
struct Touch {
    double t = -10;       // when an object last counted as touched
    void* object = nullptr;
    Vec3 centre;
    float radius = 0, dist = 0;
};
Touch g_touch;
float g_rig_scale = 10.0f;
// The top of the column over a pile or a field in which the hand reaches it (see scoop_height_m).
float scoop_top(const Vec3& centre, float radius) {
    const float ground = ground_height(centre.x, centre.z);
    return std::max({centre.y + radius, ground + 1.5f * g_rig_scale, ground + g_set.scoop_height_m * g_rig_scale}) + g_set.pile_reach_m * g_rig_scale;
}
bool g_touch_armed = false;  // the hand is at the controller, empty and ready to grab
double g_on_own_at = -10;  // ... the player's own creature's
double g_on_creature_at = -10;  // when the palm was last inside a creature's bounding sphere
Vec3 g_creature_centre;         // ... and that sphere's centre
bool g_petting = false;         // ... and so the action button is held for the player
bool g_tappable = false;        // the thing under the game's hand can be tapped (this frame)
bool g_leash_target = false;    // ... or the creature on the leash can be sent to it
bool g_on_own_creature = false; // ... or it is the player's creature (the trigger puts the leash on or off)
int g_touchlog = 0;  // debug (hands touchlog N): the next N objects the touch looks at near the palm
bool g_hand_picks = false;   // ... so only things at the hand may be picked
// People, animals and trees by the palm, offered to the picker by the mod once a frame: the game
// only offers what its cursor ray (eye to the ground below the hand) hits, and beside a building
// that was the building, never the villager under the palm (playtest 2026-10-31).
bool g_living_offered = false;
bool g_scroll_by_palm = false;  // a story scroll within the hand's reach this frame: it, not a tree beside it
// (Scrolls aren't in the map cells: they report to the picker from their own draw, 0x519AA4. When one
// does with the palm on it, noted here; a tree beside it has a sphere of 8 m around it.)
double g_scroll_at_palm_s = -10;
// A small thing (not a person, animal or tree: a gate stone) offered with the palm inside it this frame:
// it beats a field or pile the palm is over (a field goes by its whole footprint; player, 2026-11-02).
bool g_small_by_palm = false;
int g_give_type = -1;  // a miracle given from the pause menu's debug buttons, to be made (give_miracle)
// The object touched when the grip closed. The game acts on the click up to a turn later (and
// the object may have flown on by then), so it stays the only candidate until the hand has it.
struct Latch {
    void* object = nullptr;
    double t = -10;
    bool offer = false;  // taken by the grip's catch (the game may not offer it): the mod offers it
};
Latch g_latch;
Near g_second;  // the other controller's hand frame (pos, x, y, z), drawn only
bool g_second_valid = false;
// Each hand's grip closed, for the drawn fingers (closes at squeeze_down, opens at squeeze_up).
bool g_fist[2] = {};
void update_fist(int side, float squeeze) {
    g_fist[side] = g_fist[side] ? squeeze > g_set.squeeze_up : squeeze > g_set.squeeze_down;
}

// A game object's virtual function at vtable offset `off`, or null if the object, its vtable or
// the function isn't where the game's own are (inside runblack.exe). Objects the mod remembers or
// reads from the game's lists can be freed under it (a land being unloaded): calling through a
// cleared vtable jumped to address 0 and crashed the game as the portal's scene ended (player,
// 2026-10-24).
bool in_game_image(const void* p) {
    static uintptr_t lo = 0, hi = 0;
    if (!hi) {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        lo = uintptr_t(base), hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    return uintptr_t(p) >= lo && uintptr_t(p) < hi;
}
void* game_method(const void* obj, unsigned off) {
    if (!obj || IsBadReadPtr(obj, 4)) return nullptr;
    void* const* vt = *static_cast<void* const* const*>(obj);
    if (!in_game_image(vt) || IsBadReadPtr(vt + off / 4, 4)) return nullptr;
    void* fn = vt[off / 4];
    return in_game_image(fn) ? fn : nullptr;
}

// The object's bounding sphere, as SendObjectDrawCollision itself derives it.
bool bounding_sphere(void* object, void* lh3d, Vec3& centre, float& radius) {
    if (!lh3d && (!object || IsBadReadPtr(static_cast<char*>(object) + 0x40, 4))) return false;
    char* o3 = static_cast<char*>(lh3d ? lh3d : *reinterpret_cast<void**>(static_cast<char*>(object) + 0x40));
    if (!o3 || IsBadReadPtr(o3, 0x48)) return false;
    const float* m = reinterpret_cast<const float*>(o3 + 0x14);  // 4x3, rows = local axes, row 3 = position
    const float scale = *reinterpret_cast<const float*>(o3 + 0x44);
    using InfoFn = char*(__thiscall*)(void*);
    const auto info_fn = reinterpret_cast<InfoFn>(game_method(o3, 0xF8));
    if (!info_fn) return false;
    char* info = info_fn(o3);
    if (info && IsBadReadPtr(info, 0x34)) return false;
    if (!info) {
        centre = {m[9], m[10], m[11]};
        radius = 1.0f;
        return true;
    }
    const float* c = reinterpret_cast<const float*>(info + 0x18);
    centre = {c[0] * m[0] + c[1] * m[3] + c[2] * m[6] + m[9], c[0] * m[1] + c[1] * m[4] + c[2] * m[7] + m[10],
              c[0] * m[2] + c[1] * m[5] + c[2] * m[8] + m[11]};
    radius = *reinterpret_cast<const float*>(info + 0x30) * scale;
    return true;
}

unsigned g_dbg_calls = 0, g_dbg_inside = 0, g_dbg_taken = 0;
int g_picklog = 0;
int g_dbg_vinfl = 0;  // debug: log the influence-ring warning's state for N frames
extern void* g_game_hand;
void* game_interface();  // (below)
// The object the picker settled on: each accepted report is nearer than the one before, so the
// last accepted one of a frame is what the hand is over. (This frame so far / the last frame.)
void* g_picked_now = nullptr;
void* g_picked = nullptr;
bool g_picked_now_creature = false, g_picked_creature = false;  // ... and whether it is a creature
int collide_report(void* iface, void* object, float distance, void* lh3d, void* caller);
void* g_apply_target = nullptr;  // a scaffold the held one touches and can join (this frame)
void* own_creature();  // (below)
// Can the hand lift it (Object::CanBePickedUp, vtable 0x628)? `strict`: also not too heavy for the
// hand (ValidForPlaceInHand, 0x6FC: a Rock above a weight; it answers no for most other things,
// trees among them, so it only makes sense together with a tap).
bool liftable(void* o, bool strict = false) {
    void* status = g_game_hand ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850) : nullptr;
    if (!o || !status) return false;
    const auto can = reinterpret_cast<int(__thiscall*)(void*)>(game_method(o, 0x628));
    const auto valid = reinterpret_cast<int(__thiscall*)(void*, void*)>(game_method(o, 0x6FC));
    if (!can || !valid || !can(o)) return false;
    return !strict || valid(o, status);
}
float ground_height(float x, float z);  // (below)
// Villager::SetVillagerDisciple (0x756000, thiscall(GameThing* at, VILLAGER_DISCIPLE role, int), ret
// 0xC): the game makes a held villager a disciple as soon as it is held near a field, a tree, a
// building (Object::DiscipleInHandNear, by the distance across the ground only), at any height. Held
// too high up (disciple_too_high), it isn't: let go, it falls, and stays what it was.
double g_shaken_at = -10;  // (the last shake given to the game)
using SetDiscipleFn = unsigned(__fastcall*)(void* villager, void* edx, void* at, int role, int flag);
SetDiscipleFn o_set_disciple = nullptr;
unsigned __fastcall set_disciple_detour(void* villager, void* edx, void* at, int role, int flag) {
    if (role > 0 && disciple_too_high() && villager == held_adult_villager()) {
        static double logged = -10;
        if (now_s() - logged > 2.0) logged = now_s(), BWLOG("hands: a disciple (role %d) not made: the villager is held too high up", role);
        // (As if held over nothing: a role taken lower down is let go of as the hand rises, its icon too.)
        return o_set_disciple(villager, edx, nullptr, 0, flag);
    }
    static void* last_villager = nullptr;  // (logged when the held villager's role changes, or a shaken one's)
    static int last_role = -99;
    if ((villager == held_villager() || now_s() - g_shaken_at < 3.0) && (villager != last_villager || role != last_role))
        last_villager = villager, last_role = role,
        BWLOG("hands: villager %p: disciple role %d (by %s, from %p)", villager, role, at ? class_name(at) : "nothing", _ReturnAddress());
    return o_set_disciple(villager, edx, at, role, flag);
}
int g_pick_change_log = 0;  // debug (hands pick_changes N): each change of the game's pick in the picker
int __fastcall collide_detour(void* iface, void*, void* object, float distance, void* lh3d) {
    void* const before = iface ? *reinterpret_cast<void* const*>(static_cast<char*>(iface) + 0x400) : nullptr;
    const int taken = collide_report(iface, object, distance, lh3d, _ReturnAddress());
    if (g_pick_change_log > 0 && iface) {
        void* const after = *reinterpret_cast<void* const*>(static_cast<char*>(iface) + 0x400);
        if (after != before) {
            --g_pick_change_log;
            BWLOG("picker: the pick %p -> %p %s (%.1f) on %s %p's report (distance %.1f, from %p)", before, after, after ? class_name(after) : "-",
                  *reinterpret_cast<const float*>(static_cast<char*>(iface) + 0x404) + 1.0e6f, object ? class_name(object) : "-", object, distance,
                  _ReturnAddress());
        }
    }
    if (taken && object) g_picked_now = object, g_picked_now_creature = _ReturnAddress() == reinterpret_cast<void*>(0x517B09);
    return taken;
}
// The objects in flight (the PhysicsObject array at 0xD47814, count at 0xD47818, 0x1DC each, the
// object at +0x18), refreshed every frame. Their bounding spheres are tiny (a thrown villager's is
// 4 cm), so the hand reaches further for them: catching them in the air needed the palm within
// 15 cm of the centre.
// The game's objects in the map cells around (x, z): GGame +0x59FC holds the cells ({first
// mobile, first fixed}, 8 bytes each, by x cell * +0x59C4 + z cell; +0x59C8 is the number of x
// cells; a cell is 10 units), and an object's next in its cell is at +0x20.
template <typename Fn>
void for_each_object_near(float x, float z, float radius, Fn fn) {
    const auto* game = static_cast<const char*>(game::game_instance());
    if (!game) return;
    const int n = int(radius / 10.0f) + 1, cx = int(x / 10.0f), cz = int(z / 10.0f);
    const uint32_t nx = *reinterpret_cast<const uint32_t*>(game + 0x59C8), nz = *reinterpret_cast<const uint32_t*>(game + 0x59C4);
    int left = 2000;  // (in case a list loops)
    for (int i = cx - n; i <= cx + n; ++i)
        for (int j = cz - n; j <= cz + n; ++j) {
            if (i < 0 || j < 0 || uint32_t(i) >= nx || uint32_t(j) >= nz) continue;
            for (int list = 0; list < 2; ++list)
                for (const char* o = *reinterpret_cast<const char* const*>(game + 0x59FC + (size_t(i) * nz + j) * 8 + list * 4);
                     o && !IsBadReadPtr(o, 0x44) && left-- > 0; o = *reinterpret_cast<const char* const*>(o + 0x20))
                    fn(const_cast<char*>(o));
        }
}

// Things scooped from: a store's piles (PileResource: PileWood, PileFood) and fields.
// (A store (StoragePit) too: its food heap isn't a thing of its own to the game's picker, the store
// is, and taking from it gives food.)
bool scoopable(const void* object) {
    const char* name = class_name(object);
    return strncmp(name, ".?AVPile", 8) == 0 || strcmp(name, ".?AVField@@") == 0 || strcmp(name, ".?AVStoragePit@@") == 0;
}
// Where a scoopable thing is, to choose between those under the hand: its own centre, or for a store
// its food heap's (StoragePit +0xC4.., read by StoragePit::GetResourcePile; PileFood's vtable).
bool is_store(const void* object) { return object && *reinterpret_cast<void* const*>(object) == reinterpret_cast<void*>(0x8F7030); }
// A store's food heap (its sphere), if it has one.
bool store_heap(void* store, Vec3& c, float& r) {
    for (int off = 0xC0; off < 0x100; off += 4) {
        void* pile = *reinterpret_cast<void**>(static_cast<char*>(store) + off);
        if (!pile || IsBadReadPtr(pile, 0x44) || *reinterpret_cast<void**>(pile) != reinterpret_cast<void*>(0x93305C)) continue;
        if (bounding_sphere(pile, nullptr, c, r) && r > 0) return true;
    }
    return false;
}
Vec3 scoop_centre(void* object, const Vec3& own) {
    Vec3 c;
    float r = 0;
    return is_store(object) && store_heap(object, c, r) ? c : own;
}

// Things in flight, for catching: the objects in the game's physics (PhysicsObject array) that
// are moving. Something dropped stays in that array for a few seconds while it settles on the
// ground; counted as in flight, it drew the game's cursor (and hand) onto it from the catch
// reach. Only objects faster than catch_speed count, and for a moment after they were (the top of
// a throw's arc is slow).
std::vector<const void*> g_in_flight;
void refresh_in_flight() {
    struct Track {
        const void* object;
        Vec3 pos;
        double t, fast_at;
    };
    static std::vector<Track> tracks;
    std::vector<Track> next;
    g_in_flight.clear();
    const double now = now_s();
    const char* list = *reinterpret_cast<const char* const*>(0xD47814);
    const int n = *reinterpret_cast<const int*>(0xD47818);
    for (int i = 0; list && i < n && i < 512 && !IsBadReadPtr(list + i * 0x1DC, 0x1DC); ++i) {
        const void* o = *reinterpret_cast<const void* const*>(list + i * 0x1DC + 0x18);
        Vec3 c;
        float r = 0;
        if (!o || !bounding_sphere(const_cast<void*>(o), nullptr, c, r)) continue;
        Track t{o, c, now, -10};
        const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& k) { return k.object == o; });
        if (it != tracks.end()) {
            t.fast_at = it->fast_at;
            const double dt = now - it->t;
            if (dt > 1e-4 && vr::length(c - it->pos) / float(dt) > g_set.catch_speed_m_s * g_rig_scale) t.fast_at = now;
            if (dt < 0.004) t.pos = it->pos, t.t = it->t;  // (too soon to measure: keep the older sample)
        }
        if (now - t.fast_at < 0.3) g_in_flight.push_back(o);
        next.push_back(t);
    }
    tracks.swap(next);
}
bool in_flight_object(const void* object) { return std::find(g_in_flight.begin(), g_in_flight.end(), object) != g_in_flight.end(); }

// The nearest person, animal or tree the game lets be picked up, its middle within `range` of the
// palm (world units), from the game's map cells; null if none.
void* nearest_living(float range, float* dist_out = nullptr) {
    void* best = nullptr;
    float best_d = 1e9f;
    for_each_object_near(g_near.palm.x, g_near.palm.z, range + 10.0f, [&](void* o) {
        if (!living_or_tree(o) || o == own_creature()) return;
        Vec3 c;
        float r = 0;
        if (!bounding_sphere(o, nullptr, c, r) || r <= 0 || r > range) return;
        const float d = vr::length(g_near.palm - c);
        if (d < range && d < best_d && liftable(o)) best = o, best_d = d;
    });
    if (dist_out) *dist_out = best_d;
    return best;
}
// People, animals, trees and small liftable things by the palm, offered to the picker once a frame
// (from the first object reporting, or failing that from the hand's preparation: over open ground
// nothing reports, so the gate stone lying there was never offered; player, 2026-11-02).
void* g_pick_iface = nullptr;  // (the picker, as the last report passed it)
// Small things that reported to the picker lately: some aren't in the map cells (the tutorial's gate
// stone, until it is first moved, is in none), so the palm's offer looks at these too.
struct RecentThing {
    void* object;
    double at;
};
std::vector<RecentThing> g_recent_things;
void note_reported(void* object) {
    if (!object || IsBadReadPtr(object, 0x44)) return;
    const double now = now_s();
    for (RecentThing& r : g_recent_things)
        if (r.object == object) {
            r.at = now;
            return;
        }
    if (living_or_tree(object) || scoopable(object) || is_store(object)) return;
    Vec3 c;
    float rad = 0;
    if (!bounding_sphere(object, nullptr, c, rad) || rad <= 0 || rad > g_set.touch_max_m * g_rig_scale || !liftable(object)) return;
    g_recent_things.erase(std::remove_if(g_recent_things.begin(), g_recent_things.end(), [&](const RecentThing& r) { return now - r.at > 3.0; }),
                          g_recent_things.end());
    if (g_recent_things.size() < 32) g_recent_things.push_back({object, now});
}
const char* gg_name(int s) {
    static const char* const n[] = {"idle", "aiming", "locked on", "pulling", "flying"};
    return s >= 0 && s < 5 ? n[s] : "?";
}
// (The game's hand's held object's 3D object, as the renderer's held_object_3d reads it.)
uint32_t game_hand_state();
// The first thing in the game's hand (GInterfaceStatus::GetFirstObjectInCurrentHand, on the interface's status).
void* first_in_hand() {
    void* gi = game_interface();
    void* status = gi && !IsBadReadPtr(static_cast<char*>(gi) + 0x39C, 4) ? *reinterpret_cast<void**>(static_cast<char*>(gi) + 0x39C) : nullptr;
    return status ? reinterpret_cast<void*(__thiscall*)(void*)>(0x5DC130)(status) : nullptr;
}
// The held thing's 3D object. The game's "last picked up" (GInterfaceStatus +0x120) names the last thing
// it held until its next turn after a new one is taken (0.1 s in the harness, 0.34 s in a playtest: a
// rock went straight to holding and was taken for the tree held before it; 2026-11-13); the first thing
// in the hand (GetFirstObjectInCurrentHand) is nothing until then. So that one, and nothing while it
// is nothing in the first 0.6 s of holding; after that +0x120, as before.
// The held thing itself, by the same rule.
void* held_game_object() {
    if (!g_game_hand || IsBadReadPtr(static_cast<char*>(g_game_hand) + 0x4850, 4)) return nullptr;
    const char* status = *reinterpret_cast<const char* const*>(static_cast<char*>(g_game_hand) + 0x4850);
    if (!status || IsBadReadPtr(status, 0x124)) return nullptr;
    if (void* first = first_in_hand(); first && !IsBadReadPtr(first, 0x44)) return first;
    if (game_hand_state() == 4 /* holding */ && g_holding_since > 0 && now_s() - g_holding_since < 0.6) return nullptr;
    return *reinterpret_cast<void* const*>(status + 0x120);
}
void* held_object_3d_hand() {
    if (!g_game_hand) return nullptr;
    const char* status = *reinterpret_cast<const char* const*>(static_cast<char*>(g_game_hand) + 0x4850);
    if (!status || IsBadReadPtr(status, 0x124) || !status[0x90]) return nullptr;
    if (const char* first = static_cast<const char*>(first_in_hand()); first && !IsBadReadPtr(first, 0x44))
        return *reinterpret_cast<void* const*>(first + 0x40);
    if (game_hand_state() == 4 /* holding */ && g_holding_since > 0 && now_s() - g_holding_since < 0.6) return nullptr;
    const char* object = *reinterpret_cast<const char* const*>(status + 0x120);
    return object && !IsBadReadPtr(object, 0x44) ? *reinterpret_cast<void* const*>(object + 0x40) : nullptr;
}

int g_offer_log = 0;  // debug (hands offer_log N)
// What the palm's offer found from the hand's preparation, before the world is drawn: during the
// drawing a placed (fixed) thing's sphere doesn't come out where it lies, so the offer in the
// picking found nothing by the palm (the gate stone; 2026-11-02) and takes this instead.
struct PalmBest {
    void* object = nullptr;
    float d = 0, r = 0;
    Vec3 c;
    double at = -10;
};
PalmBest g_palm_best;
bool g_offer_preparing = false;
void offer_by_palm(void* iface) {
    if (g_offer_log > 0) --g_offer_log, BWLOG("hands: offer: picks %d, offered %d, armed %d, the pick %p", g_hand_picks, g_living_offered, g_touch_armed,
                                         iface ? *reinterpret_cast<void* const*>(static_cast<char*>(iface) + 0x400) : nullptr);
    if (g_gg.st == GravityGlove::kPulling && g_gg.target && iface && !IsBadReadPtr(g_gg.target, 0x44)) {
        // (The gravity gloves' pull: the target offered ahead of everything, and made the game's object.)
        if (void* o3 = *reinterpret_cast<void**>(static_cast<char*>(g_gg.target) + 0x40)) o_collide(iface, g_gg.target, -1.0e6f, o3);
        if (char* gi = static_cast<char*>(iface); !IsBadWritePtr(gi + 0x400, 4)) *reinterpret_cast<void**>(gi + 0x400) = g_gg.target;
        g_touch = {now_s(), g_gg.target, g_gg.c, g_gg.r, 0};
        return;
    }
    if (!g_hand_picks || !iface) return;
    if (!g_living_offered && g_touch_armed && g_set.living_reach_m > 0) {
        g_living_offered = true;
        const float range = std::max(g_set.touch_min_m, g_set.living_reach_m) * g_rig_scale;
        // (Not with a scroll in reach: the tutorial's scrolls sit among trees and over houses,
        // and the nearest tree, offered ahead of everything, took the tap; player, 2026-11-02.)
        bool& scroll_near = g_scroll_by_palm;
        scroll_near = now_s() - g_scroll_at_palm_s < 0.15;
        for_each_object_near(g_near.palm.x, g_near.palm.z, range + 10.0f, [&](void* o) {
            if (scroll_near || IsBadReadPtr(o, 4) || *static_cast<void**>(o) != reinterpret_cast<void*>(0x94228C)) return;
            Vec3 c;
            float r = 0;
            if (bounding_sphere(o, nullptr, c, r) && r > 0 && r < g_set.touch_min_m * 1.5f * g_rig_scale &&
                vr::length(g_near.palm - c) < r + (g_set.touch_margin_m + 0.15f) * g_rig_scale)
                scroll_near = true;
        });
        if (scroll_near) {
            static double logged_at = -10;
            if (now_s() - logged_at > 5) logged_at = now_s(), BWLOG("hands: a scroll by the palm: no person, animal or tree offered ahead of it");
        }
        {  // (with a scroll in reach only people, animals and trees are left out: a small thing still is offered)
        void* best = nullptr;
        float best_d = 1e9f;
        Vec3 best_c;
        float best_r = 0;
        auto consider = [&](void* o) {
            if (o == own_creature()) return;
            // (Also the other small things the hand can lift, a gate stone, a rock: the game
            // offers only what its cursor ray from the eye meets, so the stone lying by the
            // hand was taken only from some angles; player, 2026-11-02. By the palm inside
            // their sphere.)
            const bool living = living_or_tree(o);
            if (living && scroll_near) return;
            Vec3 c;
            float r = 0;
            const bool sphere_ok = bounding_sphere(o, nullptr, c, r);
            if (g_offer_log > 0 && !living && strcmp(class_name(o), ".?AVMobileStatic@@") == 0 && vr::length(g_near.palm - c) < 8.0f)
                BWLOG("hands: offer: candidate %p sphere %d (%.1f, %.1f, %.1f) r %.2f, d %.2f, liftable %d", o, sphere_ok, c.x, c.y, c.z, r,
                      vr::length(g_near.palm - c), liftable(o));
            if (!sphere_ok || r <= 0) return;
            if (living ? r > range : (r > g_set.touch_max_m * g_rig_scale || scoopable(o) || is_store(o))) return;  // (bigger: the game's own offer does)
            const float d = vr::length(g_near.palm - c);
            // (A small thing is reached a little further, as scrolls are: the gate stone is drawn larger
            // than its sphere, and the palm on top of it was 40 cm from its middle, its sphere 27.5 cm.)
            const float small_reach = (g_set.touch_margin_m + 0.15f) * g_rig_scale;
            if (!living && d > r + small_reach) return;
            if (g_touchlog > 0 && d < range + 3.0f * g_rig_scale) {
                --g_touchlog;
                BWLOG("touch: by the palm: %s %p r %.2f dist %.2f (range %.2f) liftable %d", class_name(o), o, r, d, range, liftable(o));
            }
            if (d < (living ? range + g_set.touch_margin_m * g_rig_scale : r + small_reach) && d < best_d && liftable(o)) best = o, best_d = d, best_c = c, best_r = r;
        };
        for_each_object_near(g_near.palm.x, g_near.palm.z, range + 10.0f, consider);
        for (const RecentThing& t : g_recent_things)
            if (now_s() - t.at < 3.0 && !IsBadReadPtr(t.object, 0x44)) consider(t.object);
        if (g_offer_preparing) g_palm_best = {best, best_d, best_r, best_c, now_s()};
        else if (!best && g_palm_best.object && now_s() - g_palm_best.at < 0.1 && !IsBadReadPtr(g_palm_best.object, 0x44))
            best = g_palm_best.object, best_d = g_palm_best.d, best_r = g_palm_best.r, best_c = g_palm_best.c;
        if (!best && g_offer_log > 0) BWLOG("hands: offer: nothing found by the palm (%.1f, %.1f, %.1f), %zu reported lately", g_near.palm.x,
                                            g_near.palm.y, g_near.palm.z, g_recent_things.size());
        if (best) {
            void* o3 = *reinterpret_cast<void**>(static_cast<char*>(best) + 0x40);
            const float claimed = living_claim(range, range, best_d);
            ++g_dbg_inside;
            const int took = o3 ? o_collide(iface, best, claimed, o3) : 0;
            if (g_offer_log > 0) BWLOG("hands: offer: best %s %p, taken %d", class_name(best), best, took);
            if (!living_or_tree(best) && g_offer_log > 0) {  // (debug, hands offer_log N: does the game take the offer of a small thing?)
                static double logged_at = -10;
                if (now_s() - logged_at > 1.0) {
                    logged_at = now_s();
                    const auto pickable = reinterpret_cast<int(__thiscall*)(void*)>(game_method(best, 0x190));
                    BWLOG("hands: offered %s %p (claim %.1f): taken %d, its +0x190 %d, flags +0x24 %04X, the pick now %p (%.1f)", class_name(best), best,
                          claimed + 1.0e6f, took, pickable ? pickable(best) : -1, *reinterpret_cast<const uint16_t*>(static_cast<char*>(best) + 0x24),
                          *reinterpret_cast<void* const*>(static_cast<char*>(iface) + 0x400), *reinterpret_cast<const float*>(static_cast<char*>(iface) + 0x404) + 1.0e6f);
                }
            }
            if (took) {
                ++g_dbg_taken;
                g_touch = {now_s(), best, best_c, best_r, best_d};
                g_picked_now = best, g_picked_now_creature = false;
                if (!living_or_tree(best)) {
                    g_small_by_palm = true;
                    // (The game takes from the ground under the hand when its own object under
                    // the hand (GInterface +0x400) is empty: by a field, its food, whatever the
                    // picker chose; player, 2026-11-02: the gate stone gave food. The palm's
                    // thing goes there.)
                    if (char* gi = static_cast<char*>(game_interface()); gi && !IsBadWritePtr(gi + 0x400, 4) && !*reinterpret_cast<void**>(gi + 0x400)) {
                        *reinterpret_cast<void**>(gi + 0x400) = best;
                        static int logged = 0;
                        if (logged++ < 5) BWLOG("hands: %s %p by the palm made the game's object under the hand", class_name(best), best);
                    }
                }
            }
        }
        }
    }
}

int collide_report(void* iface, void* object, float distance, void* lh3d, void* caller) {
    ++g_dbg_calls;
    g_pick_iface = iface;
    // (The game picks in more than one round a frame, each starting with nothing picked (GInterface
    // +0x400 empty), and the last round's pick is the one used: the palm's offer is made in each.
    // Once a frame, the house beside the gate stone won the round that counted; 2026-11-02.)
    if (iface && !IsBadReadPtr(static_cast<char*>(iface) + 0x400, 4) && !*reinterpret_cast<void* const*>(static_cast<char*>(iface) + 0x400))
        g_living_offered = false, g_scroll_by_palm = false, g_small_by_palm = false;
    note_reported(object);
    if (g_picklog > 0 && object) {  // debug: what reports to the picker, each object once
        static std::vector<void*> seen;
        if (std::find(seen.begin(), seen.end(), object) == seen.end()) {
            seen.push_back(object);
            --g_picklog;
            Vec3 c{};
            float r = 0;
            const bool ok = bounding_sphere(object, lh3d, c, r);
            char* status = g_game_hand ? *reinterpret_cast<char**>(static_cast<char*>(g_game_hand) + 0x4850) : nullptr;
            using ValidFn = unsigned(__thiscall*)(void*, void*);
            const auto tap_fn = reinterpret_cast<ValidFn>(game_method(object, 0x740));
            const unsigned tap = status && tap_fn ? tap_fn(object, status) : 0;
            BWLOG("picker: object %p vtable %p from %p distance %g sphere %d (%.1f, %.1f, %.1f) r %.2f tappable %u", object, *static_cast<void**>(object),
                  caller, distance, ok, c.x, c.y, c.z, r, tap);
        }
    }
    // Creatures report themselves from Creature::Draw (the call at 0x517B04). Is the palm on one?
    // (Only the player's own: with the leash in hand, the palm inside a big wild creature's sphere
    // (a bear) held the action button as petting does, and the game took it as a leash order;
    // player, 2026-10-20.)
    if (object && g_near.on && caller == reinterpret_cast<void*>(0x517B09) && object != own_creature()) {
        static int logged = 0;
        if (logged++ < 5) BWLOG("hands: a creature reports that isn't the player's own: %p (own %p, %s)", object, own_creature(), class_name(object));
    }
    if (object && g_near.on && caller == reinterpret_cast<void*>(0x517B09) && object == own_creature()) {
        Vec3 c;
        float r = 0;
        if (bounding_sphere(object, lh3d, c, r) && vr::length(g_near.palm - c) < std::max(r, g_set.touch_min_m * g_rig_scale) + g_set.touch_margin_m * g_rig_scale)
            g_on_creature_at = now_s(), g_creature_centre = c;
    }
    // While the hand pets a creature, that creature is what the hand is over, whatever else the
    // rules below would make of it. (Petting starts from the creature list, the same frame the
    // palm arrives; without this the creature was only let through if it had happened to be
    // accepted as "touched" a frame earlier, and otherwise petting never took.)
    if (g_petting && object && caller == reinterpret_cast<void*>(0x517B09)) {
        Vec3 c;
        float r = 0;
        if (bounding_sphere(object, lh3d, c, r) &&
            vr::length(g_near.palm - c) < std::max(r, g_set.touch_min_m * g_rig_scale) + g_set.touch_margin_m * g_rig_scale)
            return o_collide(iface, object, -3.0e6f, lh3d);  // ahead of anything latched or touched
    }
    if (g_hand_picks && object) {
        if (g_latch.object) {
            // (A person or animal latched at the grip that the game doesn't offer itself (it is off
            // its cursor ray): offered once a frame, ahead of everything.)
            if (!g_living_offered && g_latch.offer && object != g_latch.object) {
                g_living_offered = true;
                void* o3 = !IsBadReadPtr(static_cast<char*>(g_latch.object) + 0x40, 4) ? *reinterpret_cast<void**>(static_cast<char*>(g_latch.object) + 0x40) : nullptr;
                if (o3 && o_collide(iface, g_latch.object, -2.0e6f, o3)) g_picked_now = g_latch.object, g_picked_now_creature = false;
            }
            return object == g_latch.object ? o_collide(iface, object, -2.0e6f, lh3d) : 0;
        }
        offer_by_palm(iface);
        if (object && *static_cast<void**>(object) == reinterpret_cast<void*>(0x94228C)) {
            Vec3 c;
            float r = 0;
            if (bounding_sphere(object, lh3d, c, r) && r > 0 && r < g_set.touch_min_m * 1.5f * g_rig_scale &&
                vr::length(g_near.palm - c) < r + (g_set.touch_margin_m + 0.15f) * g_rig_scale)
                g_scroll_at_palm_s = now_s();
        }
        if ((g_scroll_by_palm || now_s() - g_scroll_at_palm_s < 0.15) && living_or_tree(object)) return 0;
        if (g_small_by_palm && (scoopable(object) || is_store(object))) return 0;
        Vec3 centre;
        float radius = 0;
        bool sphere = bounding_sphere(object, lh3d, centre, radius) && radius > 0;
        // A store is touched at its food heap, not anywhere in its sphere (2 m across: the hand
        // well above it was "on" it, ringed it, and its grip took nothing; player, 2026-10-19).
        // A store without one has nothing to take.
        if (sphere && is_store(object) && !store_heap(object, centre, radius)) return 0;
        const float dist = sphere ? vr::length(g_near.palm - centre) : 0.0f;
        // (Scrolls, small and often over a building, and things in flight are reached from further;
        // so are a store's piles and fields, which lie low: the hand scoops from above them.)
        // (Small scrolls only: the "?" signposts are ScriptHighlights too, 30 cm across, and with
        // the extra reach one beside the miracle bubble took the hand off the bubble.)
        const bool scroll = *reinterpret_cast<void**>(object) == reinterpret_cast<void*>(0x94228C) && sphere &&
                            radius < g_set.touch_min_m * 1.5f * g_rig_scale;
        const float reach = g_set.touch_margin_m + (in_flight_object(object) ? g_set.catch_reach_m : 0.0f) + (scroll ? 0.15f : 0.0f) +
                            (scoopable(object) ? g_set.pile_reach_m : 0.0f);  // (metres)
        // Is the palm on it? Inside its sphere (with the reach). A store's piles and fields go by a
        // column instead: over its footprint, and below a height the same for all of them (at
        // least 1.5 m above the ground, and its own top, plus the scooping reach). By their spheres
        // the food heap (1.2 m across, and over the nearest wood pile) was reached from far
        // higher than the wood beside it, so the hand above the wood took food (player,
        // 2026-10-19).
        const bool living = sphere && living_or_tree(object);
        const float touch_r = std::max(radius, (living ? std::max(g_set.touch_min_m, g_set.living_reach_m) : g_set.touch_min_m) * g_rig_scale);
        static std::vector<void*> touch_logged;
        if (g_touchlog <= 0) touch_logged.clear();
        if (g_touchlog > 0 && sphere && dist < radius + 3.0f * g_rig_scale &&
            std::find(touch_logged.begin(), touch_logged.end(), object) == touch_logged.end()) {
            --g_touchlog, touch_logged.push_back(object);
            BWLOG("touch: %s %p r %.2f dist %.2f (touch range %.2f) living %d, in reach %d, armed %d", class_name(object), object, radius, dist, touch_r,
                  living, g_near.in_reach, g_touch_armed);
        }
        auto on_it = [&] {
            if (!scoopable(object)) return dist < touch_r + reach * g_rig_scale;
            const float flat = std::sqrt((g_near.palm.x - centre.x) * (g_near.palm.x - centre.x) + (g_near.palm.z - centre.z) * (g_near.palm.z - centre.z));
            const float ground = ground_height(centre.x, centre.z);
            const float top = scoop_top(centre, radius);
            return flat < radius + g_set.touch_margin_m * g_rig_scale && g_near.palm.y < top && g_near.palm.y > ground - radius;
        };
        // Anything else is picked by the game's cursor ray, which runs from the eye through the
        // ground point below the hand and can land on things far away along it. With the hand at
        // the controller only things within reach of the hand itself count.
        if (!g_near.in_reach || (sphere && dist - radius > g_set.reach_m * g_rig_scale)) {
            if (!(g_touch_armed && sphere && (radius <= g_set.touch_max_m * g_rig_scale || scoopable(object)) && on_it())) return 0;
        }
        // Huge things (the temple: a sphere 51 units across, holding everything near it) aren't
        // touched with the hand: the palm anywhere near them was "on" them, so the grip, the
        // trigger and the leash all went to the temple. The laser still reaches them.
        if (g_touch_armed && sphere && (radius <= g_set.touch_max_m * g_rig_scale || scoopable(object))) {
            if (on_it()) {
                // The smallest thing the palm is inside wins: a villager under a tree beats the
                // tree, and a scroll or the creature beats the temple (whose sphere, 51 units
                // across here, holds everything near it; by depth-relative-to-size it used to win
                // over anything the palm wasn't near the middle of).
                // (A miracle's bubble beats its pedestal: the pedestal's sphere is the smaller,
                // but the bubble is what the hand is for.)
                // (A store's piles go by the nearest centre across the ground, not the smallest
                // sphere: the food pile's sphere is a metre across and held most of the store,
                // which should split between its food and its wood by where they lie.)
                const bool seed = strcmp(class_name(object), ".?AVOneOffSpellSeed@@") == 0;
                const Vec3 sc = scoopable(object) ? scoop_centre(object, centre) : centre;
                const float flat = std::sqrt((g_near.palm.x - sc.x) * (g_near.palm.x - sc.x) + (g_near.palm.z - sc.z) * (g_near.palm.z - sc.z));
                // (The store stands in for its food heap only when the heap doesn't report itself:
                // a little behind it.)
                const bool store = *reinterpret_cast<void**>(object) == reinterpret_cast<void*>(0x8F7030);
                const float size = seed ? 0.0f : scoopable(object) ? flat + (store ? 0.5f : 0.0f) : std::min(radius, 5000.0f);
                // (A pile or a field goes by how near its middle the palm is across the ground: right over
                // it, its claim came down to a person's, and a villager sitting by a store's food heap
                // couldn't be taken: the food came instead; player, 2026-11-08. So it is kept behind
                // anyone the palm is on.)
                const float claimed = living ? living_claim(touch_r, std::max(g_set.touch_min_m, g_set.living_reach_m) * g_rig_scale, dist)
                                             : -1.0e6f + 100.0f * size + dist / std::max(radius, 1e-3f) + (scoopable(object) ? 5.0f : 0.0f);
                ++g_dbg_inside;
                if (o_collide(iface, object, claimed, lh3d)) {
                    ++g_dbg_taken;
                    g_touch = {now_s(), object, centre, radius, dist};
                    return 1;
                }
                return 0;
            }
        }
    }
    return o_collide(iface, object, distance, lh3d);
}

struct Rig {
    bool init = false;
    Vec3 pos;
    float yaw_deg = 0;
    float scale = 10;
};
Rig g_rig;
void* g_game_hand = nullptr;
// The game frees its hand when it restarts in place (another player, a new game) and makes a new
// one a little later (hand_prepare sets it again). Until then the old one must not be read: it is
// dropped once it no longer looks like the game's (player, 2026-11-02: a crash on Start a New Game).
bool game_hand_alive() {
    const char* h = static_cast<const char*>(g_game_hand);
    if (!h) return false;
    bool ok = !IsBadReadPtr(h, 0x4880) && in_game_image(*reinterpret_cast<void* const*>(h));
    if (ok) {
        const char* status = *reinterpret_cast<const char* const*>(h + 0x4850);
        ok = status && !IsBadReadPtr(status, 0x140);
    }
    if (!ok) {
        BWLOG("hands: the game's hand %p is gone (a restart?): left alone until the game makes a new one", g_game_hand);
        g_game_hand = nullptr;
    }
    return ok;
}
constexpr uint32_t kHandStateCamera = 2;   // CHand::CurrentState while it holds the land
constexpr uint32_t kHandStateHolding = 4;  // ... while it carries an object
constexpr uint32_t kHandStateCharged = 8;  // ... while a miracle is charged in it (HandStateGrain)

// B&W picks up with a right click and keeps the object until the next click. In VR the trigger
// works hold-to-carry: releasing it while the game still holds something sends that second click.
enum class DropClick { kIdle, kDown, kUp };
DropClick g_drop = DropClick::kIdle;
ULONGLONG g_drop_at = 0;

// Two-handed world grab: the two world points taken hold of stay in the two hands.
struct TwoHand {
    bool on = false;
    Vec3 a, b;  // world points held by the dominant and the other hand
};
TwoHand g_two;
float g_base_scale = 0;  // [stereo] world_scale as last seen: a change there resets the rig's scale

struct Drag {
    bool on = false;
    Vec3 grab;  // world point held under the laser
};
Drag g_drag;
bool g_resisting = false;
float g_calm_time = 0;  // seconds since the game last resisted
Vec3 g_last_good;       // rig position the game accepted (from a moment ago: detection lags a frame or two)
bool g_have_good = false;
struct TimedPos {
    float t;
    Vec3 p;
};
std::vector<TimedPos> g_rig_history;  // recent calm positions
float g_clock = 0;
bool g_turn_latched = false;

WorldFrame rig_frame() { return {g_rig.pos, vr::basis_from_ypr(g_rig.yaw_deg, 0, 0), g_rig.scale}; }

uint32_t game_hand_state() {
    return g_game_hand ? *reinterpret_cast<const uint32_t*>(static_cast<const char*>(g_game_hand) + 0x4878) : 0;
}
bool g_lmb = false, g_rmb = false;
bool g_cursor_on = false;
unsigned g_updates = 0, g_land_hits = 0, g_clicks = 0;

Vec3 to_world(const WorldFrame& wf, Vec3 p_m) { return wf.base_pos + wf.base_rot.apply(p_m * wf.scale); }

// float __fastcall LH3DIsland::GetAltitude(const LH3DMapCoords& coords); coords are 16.16 fixed
// point in cells of 10 world units.
constexpr uintptr_t kIslandGetAltitude = 0x803090;
struct MapCoords {
    int32_t x, z;
    float altitude;
};
float ground_height(float x, float z) {
    const MapCoords c{int32_t(x * 65536.0f / 10.0f), int32_t(z * 65536.0f / 10.0f), 0.0f};
    return reinterpret_cast<float(__fastcall*)(const MapCoords*)>(kIslandGetAltitude)(&c);
}

int g_cast_log = 0;  // debug (hands cast_log N): the land rays cast, N of them
bool cast_island(Vec3 o, Vec3 d, float len, Vec3& hit) {
    // LH3DIsland::RayCast never returns for a ray all but along an axis across the ground (from
    // (2460, 111.3, 2565) to (2459.999, 111.292, 22565) hung the game; exactly along it is fine):
    // such a component is made exactly 0.
    if (std::fabs(d.x) < 2e-3f) d.x = 0;
    if (std::fabs(d.z) < 2e-3f) d.z = 0;
    const game::LHPoint from{o.x, o.y, o.z};
    const game::LHPoint to{o.x + d.x * len, o.y + d.y * len, o.z + d.z * len};
    float x = 0, z = 0;
    if (g_cast_log > 0) {
        --g_cast_log;
        BWLOG("hands: land ray from (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f)", from.x, from.y, from.z, to.x, to.y, to.z);
    }
    if (!reinterpret_cast<RayCastFn>(kIslandRayCast)(&from, &to, &x, &z)) return false;
    // The cast reports x/z only: recover the point on the ray.
    const float hl = d.x * d.x + d.z * d.z;
    const float t = hl > 1e-8f ? ((x - o.x) * d.x + (z - o.z) * d.z) / hl : 0.0f;
    if (t <= 0) return false;
    hit = o + d * t;
    return true;
}

// Engine projection of a world point with a camera; false if behind it.
bool project(const EyeView& e, Vec3 p, float& sx, float& sy, float& Z) {
    const Vec3 r = p - e.pos;
    const float X = vr::dot(r, e.rot.right), Y = vr::dot(r, e.rot.up);
    Z = vr::dot(r, e.rot.fwd);
    if (Z <= 1e-3f) return false;
    sx = e.cx + e.f * X / Z;
    sy = e.cy - e.f * Y / Z;
    return true;
}

void set_lmb(bool down) {
    if (down == g_lmb) return;
    g_lmb = down;
    input::vr_button(false, down);
    if (down) ++g_clicks;
}
uint32_t game_hand_state();

void set_rmb(bool down);

// The game's hand is free: 1 is its normal state; 0 is seen (in player sessions) while it hovers
// over things such as the tutorial's scrolls.
bool hand_free(uint32_t st) { return st == 0 || st == 1; }

// Is the thing under the game's hand something it can tap (a scroll, a door, the town centre)?
// (The object the picker settled on last frame; Object::InterfaceValidToTap(GInterfaceStatus*)
// is virtual slot 0x740.)
bool tappable_under_hand() {
    if (!g_game_hand) return false;
    char* status = *reinterpret_cast<char**>(static_cast<char*>(g_game_hand) + 0x4850);
    void* object = g_picked;
    if (!object || IsBadReadPtr(object, 4)) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    if (IsBadReadPtr(vtable + 0x740 / 4, 4)) return false;
    // The tutorial's and story scrolls (ScriptHighlight, vtable 0x94228C) answer no to that, yet
    // the action button activates them (seen: the game's hint says "Activate" and a click starts
    // their scene).
    if (vtable == reinterpret_cast<void**>(0x94228C)) return true;
    // So do the tutorial's singing stones (MobileStatic, vtable 0x8E80B4): its script asks whether
    // the stone was the last thing the action button clicked (GAME_THING_CLICKED: GInterface
    // +0x45C), and the hand can't lift them (player, 2026-10-19: the trigger on one did nothing).
    if (vtable == reinterpret_cast<void**>(0x8E80B4)) return true;
    // A creature the game offers to "Pick Up": only the creature choice does that (the tutorial's
    // pens). Choosing is a click, so the trigger taps it (player, 2026-11-02: only the grip chose;
    // a tap on something liftable otherwise does nothing).
    // (Not only while the game asks for its "Pick Up" word: at the choice it didn't, and the trigger
    // went by the land button first, then the action button as a pick-up, and nothing was chosen;
    // the grip chose. A creature not the player's own is tapped with the action button at once.)
    if (is_a(object, ".?AVCreature@@") && object != own_creature()) return true;
    using ValidToTapFn = unsigned(__thiscall*)(void* object, void* status);
    return reinterpret_cast<ValidToTapFn>(vtable[0x740 / 4])(object, status) != 0;
}

// The leash. In the flat game a quick click (the action button let go within about a quarter of a
// second) on an object, with the creature on the leash, sends the creature to it; held longer
// the button picks the object up (GInterface 0x5D5250 -> 0x5D3930). A grip is always held, so
// in VR the trigger directs: the mod makes the game's own call, fn 0x5D3930(GInterface, object),
// which sends the packet (0x5D) that has the creature act on the object (Creature::ForceActOnObject).
// GInterface is GInterfaceStatus+0x2C; its player (+0x39C) has the leash status (+0x12C): on at
// +0x14, the object the leash is tied to at +0x24.
void* game_interface() {
    if (!g_game_hand) return nullptr;
    const char* status = *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850);
    if (!status || IsBadReadPtr(status + 0x2C, 4)) return nullptr;
    void* gi = *reinterpret_cast<void* const*>(status + 0x2C);
    // (While the game restarts for another player's profile the hand's status points at freed
    // memory: only an object whose methods are the game's own is used; player, 2026-11-02.)
    if (game_method(gi, 0x1C)) return gi;
    static int logged = 0;
    if (gi && logged < 3) ++logged, BWLOG("hands: the hand's game interface %p isn't the game's (a restart?): left alone", gi);
    return nullptr;
}
// The tutorial's camera lessons (HandDemos.txt: TeachRotate, TeachPitch, TeachZoom) count the help
// events the game's camera controls send (GET_TOTAL_EVENTS 25 rotate, 28 pitch, 29 zoom: the
// camera's action codes 0x300 + n go to HelpProfile::Trigger, 0x5C46E0, on GGame +0x250060) and
// end when there are enough (rotate 50 more, zoom 15). VR's turns and rises don't go through those
// controls, so the lessons never ended (player, 2026-11-02: stuck on "rotate"). They send them here:
// a turn one rotate event a degree, a rise or descent a zoom event a tenth of a metre, the head's
// nod a pitch event every two degrees.
void help_events(int type, int count) {
    const char* game = *reinterpret_cast<const char* const*>(0xD0195C);
    if (!game || IsBadReadPtr(game + 0x250060, 4) || count <= 0) return;
    void* profile = *reinterpret_cast<void* const*>(game + 0x250060);
    if (!profile || IsBadReadPtr(profile, 0x3300)) return;
    // (The game counts one a game turn: a flag in the counter, cleared each turn. Once through the
    // game, for its other tallies, then the rest straight into the count the script reads.)
    int* total = reinterpret_cast<int*>(static_cast<char*>(profile) + 8 + type * 0x10C);
    const int before = *total;
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x5C46E0)(profile, type);
    *total = before + std::min(count, 90);
}
struct LessonAmounts {
    float turned_deg = 0, risen_m = 0, nodded_deg = 0;
};
LessonAmounts g_lesson;
int g_lesson_log = 3;
void lesson_amounts(float turn_deg, float rise_m, float nod_deg) {
    const auto send = [](float& acc, float add, float per, int type, const char* what) {
        acc += std::fabs(add);
        const int n = int(acc / per);
        if (n <= 0) return;
        acc -= float(n) * per;
        help_events(type, n);
        if (g_lesson_log > 0) --g_lesson_log, BWLOG("hands: %d %s help events for the camera lessons", n, what);
    };
    send(g_lesson.turned_deg, turn_deg, 1.0f, 25, "rotate");
    send(g_lesson.risen_m, rise_m, 0.1f, 29, "zoom");
    send(g_lesson.nodded_deg, nod_deg, 2.0f, 28, "pitch");
}
// Is the player's creature in a fight (the game's own check, 0x5D2990)? It reads the game object.
bool creature_fighting(void* gi) {
    const char* game = *reinterpret_cast<const char* const*>(0xD0195C);
    if (!gi || !game || IsBadReadPtr(game + 0x250538, 4)) return false;
    return reinterpret_cast<int(__thiscall*)(void*)>(0x5D2990)(gi) != 0;
}
// The player whose influence the hand is in: GInterfaceStatus vtable +0x1C (as GVirtualInfluence asks it).
void* influence_player() {
    const char* status = g_game_hand ? *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850) : nullptr;
    if (!status) return nullptr;
    using PlayerFn = void*(__thiscall*)(const void*);
    return reinterpret_cast<PlayerFn>((*reinterpret_cast<void* const* const*>(status))[0x1C / 4])(status);
}
bool leash_in_hand() {
    const char* gi = static_cast<const char*>(game_interface());
    if (!gi || IsBadReadPtr(gi + 0x39C, 4)) return false;
    const char* player = *reinterpret_cast<const char* const*>(gi + 0x39C);
    if (!player || IsBadReadPtr(player + 0x12C, 4)) return false;
    const char* leash = *reinterpret_cast<const char* const*>(player + 0x12C);
    if (!leash || IsBadReadPtr(leash + 0x24, 4)) return false;
    return *reinterpret_cast<const uint32_t*>(leash + 0x14) != 0 && *reinterpret_cast<void* const*>(leash + 0x24) == nullptr;
}
// The leash is tied to an object (or there is no leash status at all).
bool leash_tied() {
    const char* gi = static_cast<const char*>(game_interface());
    if (!gi || IsBadReadPtr(gi + 0x39C, 4)) return true;
    const char* player = *reinterpret_cast<const char* const*>(gi + 0x39C);
    if (!player || IsBadReadPtr(player + 0x12C, 4)) return true;
    const char* leash = *reinterpret_cast<const char* const*>(player + 0x12C);
    if (!leash || IsBadReadPtr(leash + 0x24, 4)) return true;
    return *reinterpret_cast<void* const*>(leash + 0x24) != nullptr;
}
// The thing under the game's hand that the creature on the leash can be sent to, if any
// (Object::ValidAsInterfaceTarget, slot 0x6F0, and ValidAsInterfaceLeashTarget, slot 0x6F4).
void* leash_target_under_hand() {
    void* object = g_picked;
    if (!object || g_picked_creature || IsBadReadPtr(object, 4) || !leash_in_hand()) return nullptr;
    void** vtable = *reinterpret_cast<void***>(object);
    if (IsBadReadPtr(vtable + 0x6F4 / 4, 4)) return nullptr;
    using CheckFn = int(__thiscall*)(void*);
    if (reinterpret_cast<CheckFn>(vtable[0x6F0 / 4])(object) != 1 || !reinterpret_cast<CheckFn>(vtable[0x6F4 / 4])(object)) return nullptr;
    return object;
}
// Where the leash leaves the hand. GInterface::GetLeashStartPoint (0x5D8EA0, thiscall (LHPoint*
// out, bool)) gives, for a leash held in the hand, a bone of the game's own hand mesh; the game's
// hand is wherever the eye's ray through the cursor meets the ground, which is not where the VR
// hand is drawn (and at a low angle that ray meets the ground far off, or not at all, and the
// leash flew off into the distance). For the player's own leash it is the drawn hand: the palm at
// the controller, or the laser hand on its point. (The other flag, true, asks for the player's
// stored point instead; that is left to the game.)
using LeashStartFn = void(__fastcall*)(void* gi, void* edx, Vec3* out, int flag);
LeashStartFn o_leash_start = nullptr;
Vec3 g_leash_game{}, g_leash_drawn{};  // (debug: the last start point the game gave, and the one used)
int g_leash_start_log = 0;  // debug: log the next N calls
bool g_follow_creature = false;  // debug: the rig follows the player's creature so the palm stays on it
void __fastcall leash_start_detour(void* gi, void* edx, Vec3* out, int flag) {
    o_leash_start(gi, edx, out, flag);
    if (g_leash_start_log > 0 && out) {
        --g_leash_start_log;
        BWLOG("hands: leash start asked by %p, flag %d, mine %d, tied %d: (%.2f, %.2f, %.2f)", _ReturnAddress(), flag & 0xFF,
              gi == game_interface(), leash_tied(), out->x, out->y, out->z);
    }
    // (Also while the leash is only in the hand, not on the creature: the end then hangs from the
    // hand as the leash symbol.)
    if (!out || (flag & 0xFF) || gi != game_interface() || leash_tied() || !g_ptr.valid) return;
    g_leash_game = *out;
    Vec3 p;
    if (!g_set.leash_to_hand) {
    } else if (g_near.blend > 0.5f) {
        *out = g_near.palm;
    } else if (laser_anchor(p)) {
        *out = p;
    }
    g_leash_drawn = *out;
}

// Tying the leash to the thing (the flat game's double click on it, its hint "Attach Leash"):
// packet 0x5C with the object's id (GInterface 0x5D4210 sends it), which ties the leash there
// (GLeashStatus 0x5E6BD0) and sends the creature to it.
void leash_tie(void* object) {
    void* game = game::game_instance();
    if (!game || !object) return;
    using IdFn = uint32_t(__thiscall*)(void*);
    const uint32_t id = reinterpret_cast<IdFn>(0x436A70)(object);
    BWLOG("hands: leash: tying it to %p (id %u)", object, id);
    using SetPacketIdFn = void(__thiscall*)(void*, int, uint32_t);
    reinterpret_cast<SetPacketIdFn>(0x550C80)(game, 0x5C, id);
}
// While the hand's grip holds the action button with the leash on, the game's own sending is
// swallowed: the grip doesn't direct (the mod's own, from the trigger, goes through).
double g_grip_no_send_until = -10;
struct PendingPop {
    void* object = nullptr;  // a miracle's bubble to pop once the hand is free
    double t = 0;
};
PendingPop g_pop_after_release;
bool g_let_go_for_pop = false;
int g_dbg_pile_log = 0;  // debug: log the piles and fields the cursor may go onto (hands pile_log N)
double g_last_tap_at = -10;
bool g_feedback_requested = false;  // the other hand's B was pressed: a feedback snapshot
double g_give_check_at = -1;  // a give ended unfinished: then, if the hand still holds it, it is put down  // the trigger's last tap (the help panel closes on one)
bool g_scoop_done = false;  // a scoop was ended by the hand leaving the pile: the grip doesn't count until it opens
PendingPop g_pop_click;          // a click on a bubble was made: watching whether it popped
void* g_pop_aim = nullptr;       // the cursor goes onto this bubble for the click
double g_dbg_lmb_until = -10;  // debug: hold the game's left button until then (hands lclick ms)
double g_no_drop_click_until = -10;  // a hand-over was just sent: letting go isn't a drop
// The grip let go while scooping: the game's button going up ends the scoop (the hand keeps what it
// took, as in the flat game), and the click that puts it down has to come after the game has
// taken that in (a click on the same turn was lost, and the wood stayed in the open hand).
double g_scoop_drop_at = -1;
int g_scoop_drop_tries = 0;
void* scoop_pile() {  // (the store, pile or field the game's hand is scooping from, if any)
    void* gi = game_interface();
    void* status = gi ? *reinterpret_cast<void**>(static_cast<char*>(gi) + 0x39C) : nullptr;
    void* it = status ? *reinterpret_cast<void**>(static_cast<char*>(status) + 0x3C) : nullptr;
    // (Only what is scooped from: the interaction is also a creature being stroked, and that ended
    // a "scoop" each frame; player's log, 2026-10-24.)
    return it && !IsBadReadPtr(it, 4) && scoopable(it) ? it : nullptr;
}
struct GiveFinish {
    uint32_t id = 0;  // the creature's (0: nothing to finish)
    double sent_at = -10, started_at = -1;
};
GiveFinish g_give_finish;
bool g_dbg_give_log = false;  // debug: log the game's give state after the packet
bool g_own_send = false;
void leash_direct(void* object) {
    void* gi = game_interface();
    if (!gi || !object) return;
    BWLOG("hands: leash: sending the creature to %p", object);
    using DirectFn = void(__thiscall*)(void*, void*);
    g_own_send = true;
    reinterpret_cast<DirectFn>(0x5D3930)(gi, object);
    g_own_send = false;
}

void freeze_watch_sent(const Vec3& to);
double g_walk_check_at = -1;  // debug: when to log the creature's command after sending it somewhere
// The game's own "force finish" of the creature (the scripts' CREATURE_FORCE_FINISH, GScript::CreatureForceFinish
// 0x6F5500): its animations ended at once (LH3DCreature::EndAnyAnimsRapidly 0x48F750, on Creature +0x160 -> +0x58)
// and what it was doing finished as not done (Creature::FinishActionUnsuccessfully 0x475730, with the script's
// reason string 0xC0D0D0).
bool creature_force_finish(void* cr, const char* why) {
    if (!cr || IsBadReadPtr(static_cast<char*>(cr) + 0x164, 4)) return false;
    const char* a = *reinterpret_cast<const char* const*>(static_cast<char*>(cr) + 0x160);
    void* lh = a && !IsBadReadPtr(a + 0x58, 4) ? *reinterpret_cast<void* const*>(a + 0x58) : nullptr;
    if (lh && !IsBadReadPtr(lh, 4)) reinterpret_cast<void(__thiscall*)(void*)>(0x48F750)(lh);
    reinterpret_cast<void(__thiscall*)(void*, const char*, int, int)>(0x475730)(cr, reinterpret_cast<const char*>(0xC0D0D0), 1, 1);
    BWLOG("hands: the creature force-finished (%s): animations ended, its action finished as not done", why);
    return true;
}
int creature_action(const void* creature);
int creature_command(const void* creature);
int creature_control(const void* creature);
// A frozen creature (deaf to the leash: playtests 2026-11-08, action 0x9B, and 2026-11-13, 0x4E; a save and a
// load cleared it): sent somewhere 5 units or more away with the leash, 3 s on it hasn't moved a unit and is
// in the same action, and isn't finishing one. Twice within 30 s: the game's force finish, once a minute at most.
struct FreezeWatch {
    double check_at = -1;
    Vec3 from;
    int action = -1;
    int strikes = 0;
    double first_strike = -100, finished_at = -100;
};
FreezeWatch g_freeze;
bool g_freeze_watch = true;  // (hands freeze_watch 0|1)
void freeze_watch_sent(const Vec3& to) {
    void* cr = own_creature();
    Vec3 c;
    float r = 0;
    if (!g_freeze_watch || !cr || g_freeze.check_at > 0 || !bounding_sphere(cr, nullptr, c, r)) return;
    if (std::hypot(to.x - c.x, to.z - c.z) < 5.0f) return;
    g_freeze.check_at = now_s() + 3.0, g_freeze.from = c, g_freeze.action = creature_action(cr);
}
void freeze_watch_tick() {
    if (g_freeze.check_at < 0 || now_s() < g_freeze.check_at) return;
    g_freeze.check_at = -1;
    void* cr = own_creature();
    Vec3 c;
    float r = 0;
    if (!cr || !bounding_sphere(cr, nullptr, c, r)) return;
    const float moved = std::hypot(c.x - g_freeze.from.x, c.z - g_freeze.from.z);
    const int act = creature_action(cr);
    const bool finishing = !IsBadReadPtr(static_cast<char*>(cr) + 0x3CC, 4) && *reinterpret_cast<const uint32_t*>(static_cast<char*>(cr) + 0x3CC) != 0;
    if (moved >= 1.0f || act != g_freeze.action || finishing) {
        g_freeze.strikes = 0;
        return;
    }
    if (g_freeze.strikes == 0 || now_s() - g_freeze.first_strike > 30.0) g_freeze.strikes = 0, g_freeze.first_strike = now_s();
    ++g_freeze.strikes;
    BWLOG("hands: leash: the creature didn't go (%.1f units in 3 s, action 0x%X, command 0x%X, control %d): %d of 2", moved, act,
          creature_command(cr), creature_control(cr), g_freeze.strikes);
    if (g_freeze.strikes >= 2 && now_s() - g_freeze.finished_at > 60.0) {
        g_freeze.strikes = 0, g_freeze.finished_at = now_s();
        creature_force_finish(cr, "deaf to the leash twice");
    }
}
// Sending the creature on the leash to a spot on the land: fn_005D3D10 (GInterface, thiscall
// (const MapCoords*)) sends packet 0x5E with the spot, if the leash is on and not tied (and the
// creature isn't already acting on a 0x5D); its handler calls Creature::ForceActOnMapCoord. The
// flat game does it for a click on the land (GInterface +0x3F0 holds the spot).
// The creature's current command (CreatureMental at +0x164: entries of 0x60 bytes from +0xFB4's
// index, the command at +0xFD8), as fn_005D3D10 reads it; -1 if unreadable.
int creature_command(const void* creature) {
    const char* mental = creature && !IsBadReadPtr(static_cast<const char*>(creature) + 0x164, 4)
                             ? *reinterpret_cast<const char* const*>(static_cast<const char*>(creature) + 0x164) : nullptr;
    if (!mental || IsBadReadPtr(mental + 0xFB4, 4)) return -1;
    const int i = *reinterpret_cast<const int*>(mental + 0xFB4);
    const char* cmd = mental + i * 0x60 + 0xFD8;
    return i >= 0 && i < 64 && !IsBadReadPtr(cmd, 4) ? *reinterpret_cast<const int*>(cmd) : -1;
}
void* own_creature();
// The creature's current action (CreatureMental +0xF60) and the leash's control of it (Creature +0x380),
// for the log; -1 if unreadable. (A creature frozen in action 0x9B, deaf to the leash and to hand-overs,
// playtest 2026-11-08: these say when it began.)
int creature_action(const void* creature) {
    const char* mental = creature && !IsBadReadPtr(static_cast<const char*>(creature) + 0x164, 4)
                             ? *reinterpret_cast<const char* const*>(static_cast<const char*>(creature) + 0x164) : nullptr;
    return mental && !IsBadReadPtr(mental + 0xF60, 4) ? *reinterpret_cast<const int*>(mental + 0xF60) : -1;
}
int creature_control(const void* creature) {
    return creature && !IsBadReadPtr(static_cast<const char*>(creature) + 0x380, 4) ? *reinterpret_cast<const int*>(static_cast<const char*>(creature) + 0x380) : -1;
}
void leash_walk_to(const Vec3& p) {
    void* gi = game_interface();
    if (!gi) return;
    BWLOG("hands: leash: sending the creature to (%.1f, %.1f, %.1f) (its command was 0x%X)", p.x, p.y, p.z, creature_command(own_creature()));
    const MapCoords spot{int32_t(p.x * 65536.0f / 10.0f), int32_t(p.z * 65536.0f / 10.0f), 0.0f};
    using WalkFn = int(__thiscall*)(void*, const MapCoords*);
    g_own_send = true;
    reinterpret_cast<WalkFn>(0x5D3D10)(gi, &spot);
    g_own_send = false;
    g_walk_check_at = now_s() + 0.5;
    freeze_watch_sent(p);
}

// (Logged: every time the creature on the leash is sent to a thing or a spot, by the mod or the game.)
using SendToFn = int(__fastcall*)(void* gi, void* edx, const void* what);
SendToFn o_send_to_thing = nullptr, o_send_to_spot = nullptr;
// (Not swallowed while a bubble is popped: with the leash in the hand the game sends the creature
// to the bubble as it pops it, and without the sending it wasn't popped at all; player, 2026-10-24.)
bool g_pet_pressing = false;  // (the mod's petting press is down: it never sends the creature)
int __fastcall send_to_thing_detour(void* gi, void* edx, const void* object) {
    // (Petting with the leash in the hand: the game took the press as an order, sending the creature to
    // itself, or to a spot; player's log, 2026-11-08.)
    if (!g_own_send && (now_s() < g_grip_no_send_until || g_pet_pressing)) return 0;
    BWLOG("hands: leash: the creature is sent to the thing %p (%s)", object, class_name(object));
    Vec3 oc;
    float orad = 0;
    if (object && object != own_creature() && bounding_sphere(const_cast<void*>(object), nullptr, oc, orad)) freeze_watch_sent(oc);
    return o_send_to_thing(gi, edx, object);
}
int __fastcall send_to_spot_detour(void* gi, void* edx, const void* spot) {
    if (!g_own_send && (now_s() < g_grip_no_send_until || g_pet_pressing)) return 0;
    static double logged_at = -10;  // (the game sends it again every frame while the land is held: log a few)
    const int32_t* c = static_cast<const int32_t*>(spot);
    if (g_own_send || now_s() - logged_at > 0.5)
        BWLOG("hands: leash: the creature is sent to the spot (%.1f, %.1f)", c ? float(c[0]) * 10.0f / 65536.0f : 0.0f,
              c ? float(c[1]) * 10.0f / 65536.0f : 0.0f), logged_at = now_s();
    return o_send_to_spot(gi, edx, spot);
}

// Putting the leash on and taking it off. In the flat game a click on the player's own creature
// does it: the action button starts the creature-interaction camera (stroking), and when the
// camera ends (CHand::OperateInteractionCamera, 0x46CB60) after less than 450 ms of it (CHand+0x4900
// counts the milliseconds; +0x49C0 is set while it runs), packet 0x5F toggles the leash
// (GInterface 0x5D06E0: a leash tied to something is untied, otherwise on <-> off).
void* own_creature() {
    const auto* game = static_cast<const uint8_t*>(game::game_instance());
    if (!game) return nullptr;
    void* creature = *reinterpret_cast<void* const*>(game + 0xA64 + 0xA60 * game[0x205A59]);
    return creature && !IsBadReadPtr(creature, 0x44) ? creature : nullptr;
}
bool interaction_running(uint32_t& ms) {
    if (!g_game_hand) return false;
    const char* h = static_cast<const char*>(g_game_hand);
    ms = *reinterpret_cast<const uint32_t*>(h + 0x4900);
    return *reinterpret_cast<const uint32_t*>(h + 0x49C0) != 0;
}
constexpr uint32_t kLeashClickMs = 0x1C2;
// Is the leash on the creature (GLeashStatus+0x14)? -1: no leash status.
int leash_on_flag() {
    const char* gi = static_cast<const char*>(game_interface());
    if (!gi || IsBadReadPtr(gi + 0x39C, 4)) return -1;
    const char* player = *reinterpret_cast<const char* const*>(gi + 0x39C);
    if (!player || IsBadReadPtr(player + 0x12C, 4)) return -1;
    const char* leash = *reinterpret_cast<const char* const*>(player + 0x12C);
    if (!leash || IsBadReadPtr(leash + 0x14, 4)) return -1;
    return *reinterpret_cast<const uint32_t*>(leash + 0x14) != 0;
}
void leash_toggle() {
    void* game = game::game_instance();
    if (!game) return;
    BWLOG("hands: leash: toggled on the creature");
    using SetPacketFn = void(__thiscall*)(void*, int);
    reinterpret_cast<SetPacketFn>(0x550DD0)(game, 0x5F);
}

// The creature nearest `p` among those whose bounding sphere, grown by `extra`, holds it.
// Creature::CreatureList (0xC5FCF8) is a linked list: {first node, count}; a node is {next, creature}.
bool creature_at(Vec3 p, float extra, Vec3& centre, float& radius) {
    const char* node = *reinterpret_cast<const char* const*>(0xC5FCF8);
    bool found = false;
    float best = 0;
    for (int i = 0; node && i < 16; ++i) {
        if (IsBadReadPtr(node, 8)) break;
        void* creature = *reinterpret_cast<void* const*>(node + 4);
        node = *reinterpret_cast<const char* const*>(node);
        if (!creature || IsBadReadPtr(creature, 0x44)) continue;
        void* obj3d = *reinterpret_cast<void**>(static_cast<char*>(creature) + 0x40);
        if (!obj3d || IsBadReadPtr(obj3d, 0x48)) continue;
        Vec3 c;
        float r = 0;
        if (!bounding_sphere(creature, nullptr, c, r)) continue;
        const float d = vr::length(p - c);
        if (d < r + extra && (!found || d < best)) found = true, best = d, centre = c, radius = r;
    }
    return found;
}

void run_drop_click() {
    if (g_drop == DropClick::kIdle || GetTickCount64() < g_drop_at) return;
    if (g_rmb) {  // the trigger went down again: the player took over
        g_drop = DropClick::kIdle;
        return;
    }
    input::vr_button(true, g_drop == DropClick::kDown);
    g_drop = g_drop == DropClick::kDown ? DropClick::kUp : DropClick::kIdle;
    g_drop_at = GetTickCount64() + 60;
}

void set_rmb(bool down) {
    if (down == g_rmb) return;
    g_rmb = down;
    input::vr_button(true, down);
    if (down) ++g_clicks;
}

// ---------------- drawing ----------------
struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

// Appends a world-space quad (a, b, c, d in order around it), projected with the eye. Corners
// closer than `near_z` are skipped as a whole quad (the beam's start is clipped beforehand).
void add_quad(std::vector<Vtx>& out, const EyeView& e, const Vec3 (&q)[4], const DWORD (&col)[4], float bias = 0.998f) {
    Vtx v[4];
    for (int i = 0; i < 4; ++i) {
        float sx, sy, Z;
        if (!project(e, q[i], sx, sy, Z) || Z < 1.0f) return;
        // The eye passes' depth buffer holds z = 1 - k/Z with k = 1.8 (measured from captured
        // depth against known ground points; the same at any world scale or view pitch). Nudged
        // towards the eye so the marker isn't buried.
        v[i] = {sx, sy, std::max(0.0f, 1.0f - g_set.depth_k / (Z * bias)), 1.0f / Z, col[i]};
    }
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (int i : tri) out.push_back(v[i]);
}

bool g_tris_over_all = false;  // draw_tris for a panel: in front of the world, no depth test
bool g_tris_additive = false;  // draw_tris adding light (a glow), not covering
void draw_tris(IDirect3DDevice7* dev, std::vector<Vtx>& v) {
    if (v.empty()) return;
    const HRESULT scene = dev->lpVtbl->BeginScene(dev);
    {
        render::StateSaver st(dev);  // (the states below are put back afterwards)
        st.texture(0, nullptr);
        st.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        st.tss(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        st.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        st.tss(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        st.tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        // (The 2D panels are drawn in front of everything: the beam onto them can't be depth tested.)
        st.rs(D3DRENDERSTATE_ZENABLE, g_set.beam_ztest && !g_ui_pointing && !g_tris_over_all ? TRUE : FALSE);
        st.rs(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        st.rs(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        st.rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        st.rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        st.rs(D3DRENDERSTATE_DESTBLEND, g_tris_additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        st.rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        st.rs(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        st.rs(D3DRENDERSTATE_FOGENABLE, FALSE);
        st.rs(D3DRENDERSTATE_SPECULARENABLE, FALSE);
        dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v.data(), DWORD(v.size()), 0);
    }
    if (SUCCEEDED(scene)) dev->lpVtbl->EndScene(dev);
}

}  // namespace

// ---- The tuning panel ----
// Values the player dials in by feel in the headset. The other hand's stick click opens it; its
// stick then picks a line (up/down) and changes it (left/right; held, it repeats). Closing it
// writes the values to bwvr.ini ([hands], each under its key, as an integer: value x ini_scale).
struct Tunable {
    const wchar_t* label;
    float* value = nullptr;  // (or flag / count)
    bool* flag = nullptr;
    int* count = nullptr;
    float step = 0.1f, lo = 0, hi = 1;
    const wchar_t* key = nullptr;
    float ini_scale = 1;
    const wchar_t* format = L"%.2f";  // how the value shows (after display_scale)
    float display_scale = 1;
    const wchar_t* off_word = L"off";  // (a switch's words)
    const wchar_t* on_word = L"on";
    const wchar_t* const* names = nullptr;  // (a count's words, from lo)
    void (*action)() = nullptr;             // (an action line: a click does it; nothing saved)
};
void autosave_action() { stereo::request_autosave(); }
const std::vector<Tunable>& tunables() {
    static const std::vector<Tunable> list = {
        {L"Autosave now (2 s after the menu closes)", nullptr, nullptr, nullptr, 1, 0, 1,
         L"autosave_now", 1, L"", 1, L"off", L"on", nullptr, &autosave_action},
        // (Water: see-through for players always (player, 2026-11-03); solid only as a debug setting.)
        {L"Water", nullptr, &trace::sea_solid(), nullptr, 1, 0, 1, L"sea_solid", 1, L"", 1, L"see-through", L"solid"},
        {L"Teleport reach (plus twice the height)", &g_set.teleport_reach_m, nullptr, nullptr, 5.0f, 10.0f, 200.0f, L"teleport_reach_m", 1, L"%.0f m"},
        {L"Gravity gloves: reach", &g_set.gravity_range_m, nullptr, nullptr, 5.0f, 5.0f, 150.0f, L"gravity_reach_m", 1, L"%.0f m"},
        {L"Gravity gloves: aim cone", &g_set.gravity_cone_deg, nullptr, nullptr, 1.0f, 2.0f, 30.0f, L"gravity_cone_deg", 1, L"%.0f degrees"},
        {L"Gravity gloves: flick speed", &g_set.gravity_flick_m_s, nullptr, nullptr, 0.1f, 0.3f, 4.0f, L"gravity_flick_cm_s", 100, L"%.1f m/s"},
        {L"Teleport: the head above the ground", &g_set.teleport_height_m, nullptr, nullptr, 0.25f, 0.5f, 10.0f, L"teleport_view_height_cm", 100, L"%.2f m"},
        {L"Held person/animal: out from the palm", &g_set.held_living_out, nullptr, nullptr, 0.01f, -0.3f, 0.3f, L"held_living_out_cm", 100, L"%.0f cm", 100},
        {L"Held person/animal: toward the fingers", &g_set.held_living_fwd, nullptr, nullptr, 0.01f, -0.3f, 0.3f, L"held_living_fwd_cm", 100, L"%.0f cm", 100},
        {L"Held person/animal: up", &g_set.held_living_up, nullptr, nullptr, 0.01f, -0.3f, 0.3f, L"held_living_up_cm", 100, L"%.0f cm", 100},
        {L"Held miracle: out from the palm", &g_set.held_miracle_out, nullptr, nullptr, 0.1f, -1.0f, 2.0f, L"held_miracle_out_percent", 100, L"%.0f%%", 100},
        {L"Held miracle: toward the fingers", &g_set.held_miracle_fwd, nullptr, nullptr, 0.01f, -0.3f, 0.3f, L"held_miracle_fwd_cm", 100, L"%.0f cm", 100},
        {L"Held miracle: up", &g_set.held_miracle_up, nullptr, nullptr, 0.01f, -0.3f, 0.3f, L"held_miracle_up_cm", 100, L"%.0f cm", 100},
        {L"Two-hand grab moves and turns the world", nullptr, &g_set.two_hand, nullptr, 1, 0, 1, L"two_hand", 1},
        {L"... and pulling the hands apart scales it", nullptr, &g_set.two_hand_scale, nullptr, 1, 0, 1, L"two_hand_scale", 1},
        {L"Throw strength", &g_set.hand_throw_gain, nullptr, nullptr, 0.1f, 0.3f, 6.0f, L"hand_throw_gain_x10", 10, L"x %.1f"},
        {L"Throw: least hand speed", &g_set.throw_min_speed_m_s, nullptr, nullptr, 0.1f, 0.2f, 4.0f, L"throw_min_speed_cm_s", 100, L"%.1f m/s"},
        {L"Drop: falls from the hand above", &g_set.drop_fall_m, nullptr, nullptr, 0.1f, 0.1f, 5.0f, L"drop_fall_cm", 100, L"%.1f m"},
        {L"Totem: hand travel for its whole height (0: forward and back)", &g_set.totem_range_m, nullptr, nullptr, 0.1f, 0.0f, 2.0f, L"totem_range_cm", 100, L"%.1f m"},
        {L"Blueprint: turns per wrist turn (0: by itself)", &g_set.bp_twist_gain, nullptr, nullptr, 0.5f, 0.0f, 4.0f, L"blueprint_twist_x10", 10, L"x %.1f"},
        {L"Grip: closes at", &g_set.squeeze_down, nullptr, nullptr, 0.05f, 0.2f, 0.95f, L"grip_down_percent", 100, L"%.0f%%", 100},
        {L"Grip: opens below", &g_set.squeeze_up, nullptr, nullptr, 0.05f, 0.05f, 0.9f, L"grip_up_percent", 100, L"%.0f%%", 100},
        {L"Trigger: pulls at", &g_set.trigger_down, nullptr, nullptr, 0.05f, 0.2f, 0.95f, L"trigger_down_percent", 100, L"%.0f%%", 100},
        {L"Trigger: lets go below", &g_set.trigger_up, nullptr, nullptr, 0.05f, 0.05f, 0.9f, L"trigger_up_percent", 100, L"%.0f%%", 100},
        {L"Touch: margin", &g_set.touch_margin_m, nullptr, nullptr, 0.01f, 0.0f, 0.5f, L"touch_margin_cm", 100, L"%.0f cm", 100},
        {L"Touch: small things at least", &g_set.touch_min_m, nullptr, nullptr, 0.01f, 0.02f, 0.5f, L"touch_min_cm", 100, L"%.0f cm", 100},
        {L"Touch: things bigger than this aren't touched", &g_set.touch_max_m, nullptr, nullptr, 0.25f, 0.5f, 10.0f, L"touch_max_cm", 100, L"%.2f m"},
        {L"Highlight: what the hand would take", &g_set.highlight, nullptr, nullptr, 0.05f, 0.0f, 1.0f, L"highlight_percent", 100, L"%.0f%%", 100},
        {L"Outside the influence: hand opacity", &g_set.outside_alpha, nullptr, nullptr, 0.05f, 0.1f, 1.0f, L"outside_alpha_percent", 100, L"%.0f%%", 100},
        {L"Laser: diamond under the hand", &g_set.tip_under_hand, nullptr, nullptr, 0.1f, 0.0f, 1.0f, L"tip_under_hand_percent", 100, L"%.0f%%", 100},
        {L"Scoop: reach above piles and fields", &g_set.pile_reach_m, nullptr, nullptr, 0.05f, 0.0f, 1.0f, L"pile_reach_cm", 100, L"%.0f cm", 100},
        {L"Touch: people, animals, trees within", &g_set.living_reach_m, nullptr, nullptr, 0.05f, 0.0f, 0.5f, L"living_reach_cm", 100, L"%.0f cm", 100},
        {L"Grip: people, animals, trees from", &g_set.living_grip_m, nullptr, nullptr, 0.05f, 0.0f, 0.8f, L"living_grip_cm", 100, L"%.0f cm", 100},
        {L"Scoop: reach from above (over the ground)", &g_set.scoop_height_m, nullptr, nullptr, 0.25f, 1.5f, 10.0f, L"scoop_height_cm", 100, L"%.2f m"},
        {L"Set down and disciples below (higher: things fall)", &g_set.disciple_max_m, nullptr, nullptr, 0.5f, 1.0f, 30.0f, L"disciple_max_height_cm", 100, L"%.1f m"},
        {L"Scoop: ends this far out of the pile", &g_set.scoop_leave_m, nullptr, nullptr, 0.05f, 0.0f, 1.0f, L"scoop_leave_cm", 100, L"%.0f cm", 100},
        {L"Catch: only things faster than", &g_set.catch_speed_m_s, nullptr, nullptr, 0.25f, 0.0f, 10.0f, L"catch_speed_cm_s", 100, L"%.2f m/s"},
        {L"Catch: reach for things in flight", &g_set.catch_reach_m, nullptr, nullptr, 0.05f, 0.0f, 1.0f, L"catch_reach_cm", 100, L"%.0f cm", 100},
        {L"Grab below the hand: reach", &g_set.reach_m, nullptr, nullptr, 0.05f, 0.0f, 1.5f, L"reach_cm", 100, L"%.0f cm", 100},
        {L"Petting: a swing this fast slaps", &g_set.slap_m_s, nullptr, nullptr, 0.1f, 0.3f, 5.0f, L"slap_speed_cm_s", 100, L"%.1f m/s"},
        {L"Petting: a rub this fast strokes", &g_set.stroke_m_s, nullptr, nullptr, 0.05f, 0.05f, 2.0f, L"stroke_speed_cm_s", 100, L"%.2f m/s"},
        {L"Hand-over: hold up for", &g_set.give_dwell, nullptr, nullptr, 0.05f, 0.0f, 2.0f, L"give_dwell_ms", 1000, L"%.2f s"},
        {L"Hand-over: reach around the creature", &g_set.give_margin_m, nullptr, nullptr, 0.02f, 0.0f, 1.0f, L"give_margin_cm", 100, L"%.0f cm", 100},
        {L"Hand-over: creature takes it after at least", &g_set.give_finish_s, nullptr, nullptr, 0.1f, 0.0f, 3.0f, L"give_finish_ms", 1000, L"%.2f s"},
        {L"Shake: speed", &g_set.shake_speed_m_s, nullptr, nullptr, 0.1f, 0.3f, 4.0f, L"shake_speed_cm_s", 100, L"%.1f m/s"},
        {L"Shake: reversals", nullptr, nullptr, &g_set.shake_reversals, 1, 2, 10, L"shake_reversals", 1},
        {L"Hand size", &g_set.hand_size, nullptr, nullptr, 0.00005f, 0.0002f, 0.002f, L"hand_size_x1e6", 1000000, L"%.0f%%", 200000},
        {L"Fingers below the controller", &g_set.finger_pitch_deg, nullptr, nullptr, 1, -45, 60, L"finger_pitch_deg", 1, L"%.0f deg"},
        {L"Stick movement speed", &g_set.move_speed, nullptr, nullptr, 0.1f, 0.2f, 6.0f, L"move_speed_x10", 10, L"%.1f"},
    };
    return list;
}
struct TuningPanel {
    bool open = false;
    int selected = 0;
    double next_y = 0, next_x = 0;  // when a held stick repeats
};
TuningPanel g_tuning;

std::wstring held_kind_name();
std::wstring tunable_text(const Tunable& t) {
    // (The held person/animal lines edit the kind in the hand: they say which.)
    if (t.value == &g_set.held_living_out || t.value == &g_set.held_living_fwd || t.value == &g_set.held_living_up) {
        std::wstring label = t.label;
        const size_t at = label.find(L"person/animal");
        if (at != std::wstring::npos) label.replace(at, 13, held_kind_name());
        wchar_t v[32];
        swprintf(v, 32, t.format, *t.value * t.display_scale);
        return label + L": " + v;
    }
    if (t.action) return t.label;
    wchar_t buf[64];
    if (t.flag) swprintf(buf, 64, L"%s", *t.flag ? t.on_word : t.off_word);
    else if (t.count && t.names) swprintf(buf, 64, L"%s", t.names[*t.count - int(t.lo)]);
    else if (t.count) swprintf(buf, 64, L"%d", *t.count);
    else swprintf(buf, 64, t.format, *t.value * t.display_scale);
    return std::wstring(t.label) + L": " + buf;
}
void tunable_step(const Tunable& t, int dir) {
    if (t.action) t.action();
    else if (t.flag) *t.flag = !*t.flag;
    else if (t.count) *t.count = std::clamp(*t.count + dir, int(t.lo), int(t.hi));
    else *t.value = std::clamp(std::round((*t.value + t.step * float(dir)) / t.step) * t.step, t.lo, t.hi);
}
// The player's VR options (the pause menu's VR Options tab): comfort and handedness, saved in
// [hands] with the tuning settings.
const wchar_t* const kVignetteNames[] = {L"off", L"gentle", L"strong"};
void recenter_action() {
    stereo::request_recenter();
    vr::openvr::haptic(0, 0.05f, 160.0f, 0.6f);
    vr::openvr::haptic(1, 0.05f, 160.0f, 0.6f);
}
const std::vector<Tunable>& player_options() {
    static const std::vector<Tunable> list = {
        {L"Turning", nullptr, &g_set.turn_smooth, nullptr, 1, 0, 1, L"smooth_turn", 1, L"", 1, L"snap", L"smooth"},
        {L"Snap turn", &g_set.snap_deg, nullptr, nullptr, 15.0f, 15.0f, 90.0f, L"snap_turn_deg", 1, L"%.0f degrees"},
        {L"Smooth turn speed", &g_set.smooth_turn_dps, nullptr, nullptr, 15.0f, 30.0f, 240.0f, L"smooth_turn_deg_s", 1, L"%.0f degrees a second"},
        {L"Comfort vignette (moving, turning)", nullptr, nullptr, &g_set.comfort_vignette, 1, 0, 2, L"comfort_vignette", 1, L"", 1, L"off", L"on", kVignetteNames},
        {L"Movement", nullptr, &g_set.move_teleport, nullptr, 1, 0, 1, L"move_teleport", 1, L"", 1, L"smooth", L"teleport"},
        {L"Move speed", &g_set.move_scale, nullptr, nullptr, 0.25f, 0.25f, 3.0f, L"move_speed_percent", 100, L"%.0f%%", 100},
        {L"Main hand", nullptr, &g_set.dominant_left, nullptr, 1, 0, 1, L"dominant_left", 1, L"", 1, L"right", L"left"},
        {L"Both triggers grab the world (move, turn)", nullptr, &g_set.two_hand, nullptr, 1, 0, 1, L"two_hand", 1},
        {L"... and pulling the hands apart scales it", nullptr, &g_set.two_hand_scale, nullptr, 1, 0, 1, L"two_hand_scale", 1},
        {L"Throw strength", &g_set.hand_throw_gain, nullptr, nullptr, 0.1f, 0.3f, 6.0f, L"hand_throw_gain_x10", 10, L"x %.1f"},
        {L"Gravity gloves: point, grip, flick to pull", nullptr, &g_set.gravity, nullptr, 1, 0, 1, L"gravity_gloves", 1, L"", 1, L"off", L"on"},
        {L"The other hand grabs too", nullptr, &g_set.offhand, nullptr, 1, 0, 1, L"offhand_grab", 1, L"", 1, L"off", L"on"},
        {L"Recenter view (sit or stand as you'll play)", nullptr, nullptr, nullptr, 1, 0, 1,
         L"recenter_view", 1, L"", 1, L"off", L"on", nullptr, &recenter_action},
    };
    return list;
}
int player_option_count() { return int(player_options().size()); }
std::wstring player_option_key(int i) {
    const wchar_t* k = i >= 0 && i < player_option_count() ? player_options()[size_t(i)].key : nullptr;
    return k ? k : L"";
}
std::wstring player_option_text(int i) { return i >= 0 && i < player_option_count() ? tunable_text(player_options()[size_t(i)]) : L""; }
void player_option_step(int i, int dir) {
    if (i < 0 || i >= player_option_count()) return;
    tunable_step(player_options()[size_t(i)], dir);
    BWLOG("hands: VR option: %ls", tunable_text(player_options()[size_t(i)]).c_str());
}
// How a line shows in the menu (a checkbox for a switch, a button for an action; player, 2026-11-03:
// "checkboxes for the things that are toggles", Recenter view a button, not - and +). A switch whose
// words are a choice (snap / smooth) is named for its "on" side.
int tunable_kind(const Tunable& t) { return t.action ? 2 : t.flag ? 1 : 0; }
std::wstring tunable_label(const Tunable& t) {
    const std::wstring key = t.key ? t.key : L"";
    if (key == L"smooth_turn") return L"Smooth turning (off: snap turns)";
    if (key == L"move_teleport") return t.flag && *t.flag ? L"Teleport with A" : L"Teleport with A (off: A is the laser pointer)";
    if (key == L"dominant_left") return L"Left-handed (the god hand on the left)";
    if (key == L"sea_solid") return L"Solid water (off: see-through)";
    return t.label;
}
int player_option_kind(int i) { return i >= 0 && i < player_option_count() ? tunable_kind(player_options()[size_t(i)]) : 0; }
std::wstring player_option_label(int i) { return i >= 0 && i < player_option_count() ? tunable_label(player_options()[size_t(i)]) : L""; }
bool player_option_on(int i) {
    return i >= 0 && i < player_option_count() && player_options()[size_t(i)].flag && *player_options()[size_t(i)].flag;
}
int option_kind(int i) { return i >= 0 && i < option_count() ? tunable_kind(tunables()[size_t(i)]) : 0; }
std::wstring option_label(int i) { return i >= 0 && i < option_count() ? tunable_label(tunables()[size_t(i)]) : L""; }
bool option_on(int i) { return i >= 0 && i < option_count() && tunables()[size_t(i)].flag && *tunables()[size_t(i)].flag; }
// The tuning list for the pause menu's Developer tab (ui/debug_menu.cpp).
int option_count() { return int(tunables().size()); }
std::wstring option_text(int i) { return i >= 0 && i < option_count() ? tunable_text(tunables()[size_t(i)]) : L""; }
void option_step(int i, int dir) {
    if (i < 0 || i >= option_count()) return;
    tunable_step(tunables()[size_t(i)], dir);
    BWLOG("hands: option: %ls", tunable_text(tunables()[size_t(i)]).c_str());
}
void tuning_save();
void options_save() { tuning_save(); }
void held_kinds_save();
void tuning_save() {
    held_kinds_save();
    for (const auto* list : {&tunables(), &player_options()})
        for (const Tunable& t : *list) {
            if (t.action) continue;
            const int v = t.flag ? int(*t.flag) : t.count ? *t.count : int(std::lround(*t.value * t.ini_scale));
            WritePrivateProfileStringW(L"hands", t.key, std::to_wstring(v).c_str(), paths::ini_file().c_str());
        }
    BWLOG("hands: tuning saved to bwvr.ini");
}
// The other hand: its stick click opens and closes the panel; while open its stick works the
// panel. Returns true while the panel has the stick.
// The other hand's stick click: held a second, the view is recentred (a seated player's "Recenter
// view" without the menu); a short press (let go within 0.6 s) is the tuning panel's, in a
// development build.
bool g_click_short = false;
void stick_click_update(const Controller& o) {
    static double down_at = -10;
    static bool was = false, fired = false;
    const bool click = o.active && o.stick_click;
    g_click_short = false;
    if (click && !was) down_at = now_s(), fired = false;
    if (click && !fired && now_s() - down_at >= 1.0) {
        fired = true;
        BWLOG("hands: the other hand's stick held in: recenter");
        recenter_action();
    }
    if (!click && was && !fired && now_s() - down_at < 0.6) g_click_short = true;
    was = click;
}
bool tuning_input(const Controller& o) {
#if !BWVR_DEVELOPER
    (void)o;
    return false;  // (a player build has no tuning panel: the pause menu's VR Options instead)
#endif
    if (g_click_short) {
        g_tuning.open = !g_tuning.open;
        BWLOG("hands: tuning panel %s", g_tuning.open ? "open" : "closed");
        if (!g_tuning.open) tuning_save();
        vr::openvr::haptic(1 - g_set.dominant, 0.02f, 160.0f, 0.4f);
    }
    if (!g_tuning.open || !o.active) return g_tuning.open;
    const double now = now_s();
    const auto& list = tunables();
    const int dy = o.stick_y > 0.6f ? -1 : o.stick_y < -0.6f ? 1 : 0;  // up = the line above
    const int dx = o.stick_x > 0.6f ? 1 : o.stick_x < -0.6f ? -1 : 0;
    static int dy_was = 0, dx_was = 0;
    if (dy && (dy != dy_was || now >= g_tuning.next_y)) {
        g_tuning.selected = (g_tuning.selected + dy + int(list.size())) % int(list.size());
        g_tuning.next_y = now + (dy != dy_was ? 0.4 : 0.15);
    }
    if (dx && (dx != dx_was || now >= g_tuning.next_x)) {
        const Tunable& t = list[size_t(g_tuning.selected)];
        tunable_step(t, dx);
        BWLOG("hands: tuning: %ls", tunable_text(t).c_str());
        g_tuning.next_x = now + (dx != dx_was ? 0.4 : 0.12);
    }
    dy_was = dy, dx_was = dx;
    return true;
}

void held_kinds_load();  // (below)
void configure() {
    g_set.dominant_left = paths::ini_int(L"hands", L"dominant_left", 0) != 0;
    g_set.dominant = g_set.dominant_left ? vr::kLeft : vr::kRight;
    g_set.pointer = paths::ini_int(L"hands", L"pointer", 1) != 0;
    g_set.vr_throw = paths::ini_int(L"hands", L"vr_throw", 1) != 0;
    g_set.near_enabled = paths::ini_int(L"hands", L"near_hand", 1) != 0;
    g_set.always_hand = paths::ini_int(L"hands", L"always_hand", 1) != 0;
    g_set.hand_throw_gain = float(paths::ini_int(L"hands", L"hand_throw_gain_x10", 15)) / 10.0f;
    g_set.finger_pitch_deg = float(paths::ini_int(L"hands", L"finger_pitch_deg", 15));
    g_set.two_hand = paths::ini_int(L"hands", L"two_hand", 0) != 0;
    g_set.two_hand_scale = paths::ini_int(L"hands", L"two_hand_scale", 0) != 0;
    trace::sea_solid() = paths::ini_int(L"hands", L"sea_solid", 0) != 0;
    g_set.trigger_carries = paths::ini_int(L"hands", L"trigger_carries", 1) != 0;
    g_set.touch_grab = paths::ini_int(L"hands", L"touch_grab", 1) != 0;
    g_set.reach_m = float(paths::ini_int(L"hands", L"reach_cm", 25)) / 100.0f;
    g_set.ground_marker = paths::ini_int(L"hands", L"ground_marker", 0) != 0;
    g_set.pet = paths::ini_int(L"hands", L"pet", 1) != 0;
    g_set.tap = paths::ini_int(L"hands", L"tap", 1) != 0;
    g_set.give = paths::ini_int(L"hands", L"give", 1) != 0;
    g_set.leash_to_hand = paths::ini_int(L"hands", L"leash_to_hand", 1) != 0;
    g_set.shake_speed_m_s = float(paths::ini_int(L"hands", L"shake_speed_cm_s", 100)) / 100.0f;
    g_set.follow_ground = paths::ini_int(L"hands", L"follow_ground", 1) != 0;
    extern bool g_wrist_on;
    g_wrist_on = paths::ini_int(L"hands", L"wrist_menu", 1) != 0;
    g_set.glow_models = paths::ini_int(L"hands", L"glow_models", 1) != 0;
    g_set.teleport_to_creature = paths::ini_int(L"hands", L"teleport_to_creature", 1) != 0;
    g_set.teleport_creature_front = paths::ini_int(L"hands", L"teleport_creature_front", 1) != 0;
    g_set.scaffold_max_m = float(paths::ini_int(L"hands", L"scaffold_build_max_cm", 2000)) / 100.0f;
    g_set.throw_flight = paths::ini_int(L"hands", L"throw_flight", 1) != 0;
    g_set.build_at_blueprint = paths::ini_int(L"hands", L"build_at_blueprint", 1) != 0;
    g_set.gesture_b = paths::ini_int(L"hands", L"gesture_button_b", 1) != 0;
    g_set.shield_autocast = paths::ini_int(L"hands", L"shield_autocast", 1) != 0;
    g_set.zap_aim = paths::ini_int(L"hands", L"zap_aim", 1) != 0;
    g_set.held_living_place = paths::ini_int(L"hands", L"held_living_place", 1) != 0;
    held_kinds_load();
    g_set.point_to_draw = paths::ini_int(L"hands", L"point_to_draw", 1) != 0;
    g_set.combine_by_touch = paths::ini_int(L"hands", L"combine_by_touch", 1) != 0;
    g_set.join_over_m = float(paths::ini_int(L"hands", L"join_over_cm", 100)) / 100.0f;
    g_set.leash_land_tap = paths::ini_int(L"hands", L"leash_land_tap", 1) != 0;
    g_set.grip_never_directs = paths::ini_int(L"hands", L"grip_never_directs", 1) != 0;
    g_set.fist_on_grip = paths::ini_int(L"hands", L"fist_on_grip", 1) != 0;
    g_set.throw_min_speed_m_s = float(paths::ini_int(L"hands", L"throw_min_speed_cm_s", 100)) / 100.0f;
    g_set.shake_reversals = paths::ini_int(L"hands", L"shake_reversals", 4);
    g_set.pet_gain = float(paths::ini_int(L"hands", L"pet_gain_percent", 75)) / 100.0f;
    g_set.pet_by_speed = paths::ini_int(L"hands", L"pet_by_speed", 1) != 0;
    g_set.slap_m_s = float(paths::ini_int(L"hands", L"slap_speed_cm_s", 120)) / 100.0f;
    g_set.stroke_m_s = float(paths::ini_int(L"hands", L"stroke_speed_cm_s", 30)) / 100.0f;
    g_set.near_in = float(paths::ini_int(L"hands", L"near_in_cm", 70)) / 100.0f;
    g_set.near_out = float(paths::ini_int(L"hands", L"near_out_cm", 85)) / 100.0f;
    for (const Tunable& t : player_options())  // (the VR Options' values)
        if (t.value) *t.value = float(paths::ini_int(L"hands", t.key, int(std::lround(*t.value * t.ini_scale)))) / t.ini_scale;
        else if (t.flag) *t.flag = paths::ini_int(L"hands", t.key, int(*t.flag)) != 0;
        else if (t.count) *t.count = std::clamp(paths::ini_int(L"hands", t.key, *t.count), int(t.lo), int(t.hi));
    for (const Tunable& t : tunables())  // (the tuning panel's values, saved by it)
        if (t.value) *t.value = float(paths::ini_int(L"hands", t.key, int(std::lround(*t.value * t.ini_scale)))) / t.ini_scale;
}

float g_mock_sweep[2] = {0, 0};  // deg/s of yaw added to a mock controller's aim (smooth-motion tests)
Vec3 g_mock_vel[2];              // m/s added to a mock controller's position (throw tests)
// A spiral traced by a mock controller, moved every frame as a real hand (gesture tests):
// ctrl <l|r> spiral seconds turns r0 r1 [ccw]: about where the controller is, in its x/y plane.
struct MockSpiral {
    double t0 = -1;
    float T = 1.8f, turns = 2.5f, r0 = 0.2f, r1 = 0.03f, sgn = 1;
    Vec3 centre;
};
MockSpiral g_mock_spiral[2];
// A circle traced on the land by a mock controller's laser, aimed afresh every frame (shield tests):
// ctrl <l|r> aimcircle x y z radius seconds turns.
struct MockAimCircle {
    double t0 = -1;
    Vec3 c;
    float r = 10, T = 2, turns = 1.1f;
};
MockAimCircle g_mock_aim_circle[2];
struct MockSqueeze {
    double at = -1;  // when (now_s) the squeeze changes to `value`
    float value = 0;
};
MockSqueeze g_mock_squeeze_at[2];

// Shaking the hand. The flat game drops the leash (and a miracle held in the hand) when the
// player shakes the mouse left and right quickly: its control map reports an action (BINDABLE
// action 5, asked through GInterface 0x5CF220), and the leash and power-up code act on it. In VR
// the controller's sideways speed (along its own right axis) is watched: enough fast reversals
// in quick succession make the mod answer yes to action 5, once to each place that asks.
double g_shake_at = -10;
double g_shake_leash_at = -10;  // (a shake given with the leash in the hand: see shake_leash_check)
// Drawing a gesture with B held (player, 2026-10-27: drawing with every motion of the hand, the
// game matched whatever the hand had done in the last seconds, and moving the hand into place
// spoiled the shape). While B is held the game is fed the fingertip's (or the laser point's) path,
// from a clean start, and a pink trail follows the pen; nothing else is fed.
struct GestureDraw {
    bool on = false;
    std::deque<std::pair<double, Vec3>> trail;  // (time, world point)
};
GestureDraw g_gd;
double g_dbg_gesture_until = -1;  // hands gesture_log N: log the game's gesture samples and answers for N seconds
// A charged miracle (a fireball) let go of with the hand moving fast (logged). Answering the game's
// action 5 then (its throw-what-the-magic-hand-holds at 0x5CF6E5) was wrong: action 5 is the
// shake that dismisses the miracle, and the fireball vanished (player, 2026-10-25). The game casts
// a fireball through SpellSeed::ApplyThisToMapCoord on the release.
struct CtlVel {
    double t;
    Vec3 v;
};
std::deque<CtlVel> g_ctl_vel;  // the dominant controller's velocity in the world (SteamVR's), the last 0.3 s

// The gravity gloves' choice of a target along an aim (origin o, direction d): the liftable thing
// nearest the aim within the cone, out to the reach, drawn (seen) and in sight; or something big to
// take from (a field, a store's pile, a workshop's scaffolds) the aim passes through. `current`, the
// target already shown, is held on to. (Playtest 2026-11-07: things too far off to be drawn, and
// behind a hill, were offered; the choice flickered between things, and a grip in a flicker found
// none; trees wanted; a thing just thrown, still in flight, wanted back.)
// (The drawn check: LH3DObject's draw doesn't see people or rocks, which are drawn otherwise; off.)
bool g_gg_check_drawn = false, g_gg_check_sight = true;  // (hands gg_checks <drawn> <sight>)
int g_gg_log = 0;  // (hands gg_log N: why things were left out)
struct GGChoice {
    void* target = nullptr;
    Vec3 c;
    float r = 0;
    bool in_flight = false;
};
bool gg_big(const void* o) { return scoopable(o) || is_store(o) || strcmp(class_name(o), ".?AVWorkshop@@") == 0; }
// Is a point in the player's influence? Influence::CalculatePlayerInfluence (0x5CD170, cdecl(const
// MapCoords*, GPlayer*, int, INFL_CALC_TYPE, int)), as the game asks it for its hand's point each turn
// (fn_005D1120, with 0, 1, 1; the player from GInterface's vtable +0x1C): over 0 is inside.
bool in_player_influence(const Vec3& p) {
    void* gi = game_interface();
    if (!gi || IsBadReadPtr(gi, 4)) return true;
    using PlayerFn = void*(__thiscall*)(void*);
    void* player = reinterpret_cast<PlayerFn>((*static_cast<void***>(gi))[0x1C / 4])(gi);
    if (!player) return true;
    const MapCoords c{int32_t(p.x * 6553.6f), int32_t(p.z * 6553.6f), 0.0f};
    using InflFn = float(__cdecl*)(const MapCoords*, void*, int, int, int);
    return reinterpret_cast<InflFn>(0x5CD170)(&c, player, 0, 1, 1) > 0.0f;
}
bool hand_cannot_act();  // (below)
EyeView g_gg_view;  // (the head's view this frame: the gloves offer only what is in it)
bool g_gg_view_ok = false;
float g_gg_view_deg = 40.0f;  // (hands gg_view_deg: how far from where the head looks)
GGChoice gravity_find(const Vec3& o, const Vec3& d, void* current) {
    const float range = g_set.gravity_range_m * g_rig_scale, near_m = g_set.gravity_min_m * g_rig_scale;
    const float cone = g_set.gravity_cone_deg * vr::kDegToRad;
    struct Cand {
        void* obj;
        float score;
        Vec3 c;
        float r;
        bool in_flight;
    };
    std::vector<Cand> cands;
    std::vector<void*> seen;
    void* const recent = now_s() - g_thrown.t < 4.0 ? g_thrown.object : nullptr;  // (the thing just thrown)
    const auto consider = [&](void* obj, bool in_flight) {
        if (std::find(seen.begin(), seen.end(), obj) != seen.end()) return;
        seen.push_back(obj);
        if (!obj || obj == own_creature() || IsBadReadPtr(obj, 0x44)) return;
        const void* o3 = *reinterpret_cast<void* const*>(static_cast<char*>(obj) + 0x40);
        if (!o3) return;
        Vec3 cc;
        float rr = 0;
        if (!bounding_sphere(obj, nullptr, cc, rr) || rr <= 0) return;
        const Vec3 v = cc - o;
        const float dist = vr::length(v);
        if (dist < near_m || dist > range) return;
        // (Small things fade from view by how big they look: a sheep or a fence out at 20 m wasn't drawn
        // at all. Within gravity_size_reach times its own radius.)
        // (People's and animals' spheres are a few centimetres: each counted at least 45 cm across.)
        if (!in_flight && dist > std::max(rr, 0.45f * g_rig_scale) * g_set.gravity_size_reach) {
            if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p too small to be seen at %.1f m", class_name(obj), obj, dist / g_rig_scale);
            return;
        }
        // (Not fields, piles, stores or workshops: what came of them couldn't be chosen, a handful of
        // food from a field, and a store's ring was huge; player, 2026-11-07. From above, the hand
        // scoops from them instead.)
        if (gg_big(obj)) return;
        if (rr > g_set.gravity_max_size_m * g_rig_scale && !is_tree(obj)) return;
        // (A tree is pointed at anywhere on its trunk, from its foot up to its middle: a tall palm's
        // middle is high in its crown, and an aim at its trunk missed it.)
        Vec3 aim_at = cc;
        if (is_tree(obj)) {
            Vec3 foot;
            if (object_pos(static_cast<const char*>(o3), foot) && cc.y > foot.y) {
                float best = -2.0f;
                for (int k = 0; k <= 8; ++k) {
                    const Vec3 q = foot + (cc - foot) * (float(k) / 8.0f);
                    const float cosq = vr::dot(vr::normalize(q - o), d);
                    if (cosq > best) best = cosq, aim_at = q;
                }
            }
        }
        const Vec3 va = aim_at - o;
        const float ang = std::acos(std::clamp(vr::dot(va, d) / std::max(1e-3f, vr::length(va)), -1.0f, 1.0f)) - std::atan2(std::min(rr, 0.5f * g_rig_scale), dist);
        const float lim = cone * ((obj == current || obj == recent) ? 1.6f : 1.0f);
        if (ang > lim) {
            if (g_gg_log > 0 && ang < 3.0f * lim) --g_gg_log, BWLOG("hands: gravity gloves: %s %p %.1f degrees off the aim", class_name(obj), obj, ang / vr::kDegToRad);
            return;
        }
        float score = std::max(0.0f, ang) / cone + 0.25f * dist / range + (is_tree(obj) ? 0.35f : 0.0f) - (obj == recent ? 0.5f : 0.0f);
        // (In view: within gg_view_deg of where the head looks. A tree at the edge of the view, or out of
        // it, was pulled while the player looked down at a field to scoop; player, 2026-11-08.)
        if (g_gg_view_ok && g_gg_view_deg < 180.0f) {
            const Vec3 to = vr::normalize(cc - g_gg_view.pos);
            if (vr::dot(to, g_gg_view.rot.fwd) < std::cos(g_gg_view_deg * vr::kDegToRad)) {
                if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p out of view", class_name(obj), obj);
                return;
            }
        }
        // (In the player's influence: out of it the game's hand can't take it, and the pull came to
        // nothing; player, 2026-11-08.)
        if (!in_player_influence(cc)) {
            if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p out of the player's influence", class_name(obj), obj);
            return;
        }
        if (!in_flight && !liftable(obj, true)) {
            if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p can't be lifted now", class_name(obj), obj);
            return;
        }
        // (Seen: drawn in the last frame's eye passes. Not known when nothing was recorded.)
        if (g_gg_check_drawn && stereo::drawn_recently(o3) == 0 && !in_flight) {
            if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p not drawn: left out", class_name(obj), obj);
            return;
        }
        if (obj == current) score -= 0.25f;  // (held on to: a new one must be clearly better)
        cands.push_back({obj, score, cc, rr, in_flight});
    };
    {   // Things in flight (the physics' list): a thing thrown can be called back before it lands.
        // (First: found first among the things on the ground, a thing in flight was judged as one
        // standing, and came and went from frame to frame; player, 2026-11-07: the ring flickered.)
        const char* list = *reinterpret_cast<const char* const*>(0xD47814);
        const int n = *reinterpret_cast<const int*>(0xD47818);
        for (int i = 0; list && i < n && i < 256 && !IsBadReadPtr(list + i * 0x1DC, 0x1DC); ++i)
            consider(*reinterpret_cast<void* const*>(list + i * 0x1DC + 0x18), true);
    }
    const float step = 12.0f;
    for (float t = near_m; t <= range; t += step) {
        const Vec3 p = o + d * t;
        for_each_object_near(p.x, p.z, std::max(6.0f, t * std::tan(cone) + 4.0f), [&](void* obj) { consider(obj, false); });
        if (seen.size() > 600) break;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    // In sight: no land between the hand and it (the best few looked at).
    for (size_t i = 0; i < cands.size() && i < 4; ++i) {
        // (Land well short of it, both on the line to its middle and on the line to a point a metre
        // above it: a hill between. Land just by it is the ground it stands on, grazed.)
        const auto blocked = [&](const Vec3& to) {
            const Vec3 v = to - o;
            const float dist = vr::length(v);
            Vec3 hit;
            // (The game's cast reports the first land along the whole ray, past its end too.)
            const bool b = dist > 1e-3f && cast_island(o, v * (1.0f / dist), dist * 0.85f, hit) && vr::length(hit - o) < dist * 0.85f;
            if (b && g_gg_log > 0) BWLOG("hands: gravity gloves: sight to (%.1f, %.1f, %.1f) from (%.1f, %.1f, %.1f): land at %.1f of %.1f (at %.1f, %.1f, %.1f)",
                                         to.x, to.y, to.z, o.x, o.y, o.z, vr::length(hit - o), dist, hit.x, hit.y, hit.z);
            return b;
        };
        const float dist = vr::length(cands[i].c - o);
        const Vec3 hit = cands[i].c;
        if (g_gg_check_sight && blocked(cands[i].c) && blocked(cands[i].c + Vec3{0, 1.0f * g_rig_scale, 0})) {
            if (g_gg_log > 0) --g_gg_log, BWLOG("hands: gravity gloves: %s %p out of sight (land at %.1f of %.1f)", class_name(cands[i].obj), cands[i].obj,
                                                vr::length(hit - o), dist);
            continue;
        }
        return {cands[i].obj, cands[i].c, cands[i].r, cands[i].in_flight};
    }
    return {};
}
// The other hand's aim, while the main hand does nothing: its own target, shown (dimmer), taken by
// its grip (the god hand goes there, locked on).
double g_gg_soft_until = -10;  // (a pulled thing arrived with the grip open: let go of it, not thrown)
// Gravity gloves: the frame's update (see GravityGlove). st: the game's hand state.
double g_gg_updated_at = -10;  // (the ring is drawn only while the gloves are being updated)
void* scoop_below_palm();  // (below)
void gravity_update(const Controller& c, uint32_t st) {
    GravityGlove& g = g_gg;
    const double now = now_s();
    g_gg_updated_at = now;
    const auto to = [&](GravityGlove::State s) {
        if (g.st != s) BWLOG("hands: gravity gloves: %s -> %s (%s %p)", gg_name(g.st), gg_name(s), g.target ? class_name(g.target) : "-", g.target);
        g.st = s, g.at = now;
    };
    const bool squeezed = c.active && c.squeeze > g_set.squeeze_down, open = !c.active || c.squeeze < g_set.squeeze_up;
    // (Not with a menu up: the glow and the fingertips showed over the pause menu, the hand hidden;
    // nor with the hand out of the player's influence, where it can't take anything; player, 2026-11-08.)
    const bool menu_up = *reinterpret_cast<void* const*>(0xC4CD20) != nullptr;  // (SetupBox::CurrentActiveBox)
    const bool hand_mode = g_near.on && g_near.blend >= 1.0f && !g_ui_pointing && !g_cinema && !g_cutscene && !g_fight && !menu_up &&
                           !hand_cannot_act();
    const auto alive = [&](void* o) { return o && !IsBadReadPtr(o, 0x44) && *reinterpret_cast<void* const*>(static_cast<char*>(o) + 0x40); };
    static double target_seen_at = -10;  // (the grace: a target lost for a moment is still shown and taken)
    static bool squeezed_was = false;
    const bool squeeze_edge = squeezed && !squeezed_was;
    squeezed_was = squeezed;
    // (The main hand's only: one ring at a time; player, 2026-11-07: with the other hand's too, "the
    // game is all circles now".)
    if (!g_set.gravity || (g.st <= GravityGlove::kLocked && (!hand_mode || g_hand != g_set.dominant))) {
        if (g.st != GravityGlove::kIdle) to(GravityGlove::kIdle);
        g.target = nullptr;
        return;
    }
    switch (g.st) {
    case GravityGlove::kIdle:
    case GravityGlove::kTarget: {
        // The grip on the target (or one shown a moment ago): locked on.
        const bool free = hand_free(st);
        if (squeeze_edge) {
            if (g.target && free && alive(g.target) && now - target_seen_at < 0.35) {
                to(GravityGlove::kLocked);
                vr::openvr::haptic(g_hand, 0.04f, 120.0f, 0.6f);
                return;
            }
            // (Why a grip with a ring shown, or one a moment ago, didn't lock on: for the log, and
            // the feedback snapshot that copies it.)
            if (g.target || now - target_seen_at < 1.0)
                BWLOG("hands: gravity gloves: the grip didn't lock on: target %s %p (alive %d), shown %.2f s ago, hand free %d (state %u), touched %.2f s ago, squeeze %.2f",
                      g.target ? class_name(g.target) : "-", g.target, alive(g.target), now - target_seen_at, free, st, now - g_touch.t, c.squeeze);
        }
        // (Only an empty hand, not touching something by it: the hand's own grab comes first.)
        if (!free || now - g_touch.t < 0.15) {
            if (g.st != GravityGlove::kIdle) to(GravityGlove::kIdle), g.target = nullptr;
            return;
        }
        // Over a field or a pile (in its column, high up too): the hand scoops from it; the gloves stand
        // aside, whichever way the hand points (a tree by the field was locked on to, and the scoop
        // didn't happen; and again pointing ahead over a field, 2026-11-08).
        if (scoop_below_palm()) {
            if (g.st != GravityGlove::kIdle) to(GravityGlove::kIdle), g.target = nullptr;
            return;
        }
        // The grip closing: what is shown is held on to until it counts as closed (squeeze_down).
        // (It was let go of as soon as the grip left "open": a slow squeeze lost the target before it
        // counted, a quick one didn't; player, 2026-11-07: "the circle disappears" on the grip.)
        if (!open) {
            if (!squeezed && g.target && alive(g.target)) target_seen_at = now;
            else if (g.st != GravityGlove::kIdle && now - target_seen_at > 0.35) to(GravityGlove::kIdle), g.target = nullptr;
            return;
        }
        const GGChoice best = gravity_find(g_near.palm, g_ptr.dir, g.target);
        if (best.target) {
            if (best.target != g.target) {
                if (g.target) BWLOG("hands: gravity gloves: aiming at %s %p", class_name(best.target), best.target);
                if (now - g.tick_at > 0.08) vr::openvr::haptic(g_hand, 0.012f, 260.0f, 0.25f), g.tick_at = now;
            }
            g.target = best.target, g.c = best.c, g.r = best.r, g.in_flight = best.in_flight, target_seen_at = now;
            if (g.c_for != g.target) {
                Vec3 o;
                const char* o3 = *reinterpret_cast<const char* const*>(static_cast<const char*>(g.target) + 0x40);
                g.c_for = g.target, g.c_off = object_pos(o3, o) ? g.c - o : Vec3{};
            }
            to(GravityGlove::kTarget);
        } else if (now - target_seen_at > 0.35 || !alive(g.target)) {
            g.target = nullptr;
            to(GravityGlove::kIdle);
        }
        return;
    }
    case GravityGlove::kLocked: {
        if (!alive(g.target) || !hand_free(st) || open) {
            to(GravityGlove::kIdle), g.target = nullptr;
            return;
        }
        Vec3 cc;
        float rr = 0;
        if (bounding_sphere(g.target, nullptr, cc, rr)) g.c = cc, g.r = rr;
        if (now - g.tick_at > 0.12) vr::openvr::haptic(g_hand, 0.02f, 90.0f, 0.15f), g.tick_at = now;  // (the steady buzz)
        // The flick: the hand's speed back toward the player (away from the target) and up, over
        // the last 0.12 s.
        const Vec3 away = vr::normalize(g_near.palm - g.c);
        float best = 0;
        for (const auto& s : g_ctl_vel)
            if (now - s.t < 0.12) best = std::max(best, vr::dot(s.v, away) + 0.8f * s.v.y);
        if (best > g_set.gravity_flick_m_s * g_rig_scale && now - g.at > 0.1) {
            const char* o3 = *reinterpret_cast<const char* const*>(static_cast<char*>(g.target) + 0x40);
            const float* m = reinterpret_cast<const float*>(o3 + 0x14);
            g.fly_from = gg_big(g.target) ? g.c : Vec3{m[9], m[10], m[11]};
            g.fly_obj3d = const_cast<char*>(o3);
            BWLOG("hands: gravity gloves: flicked (%.1f m/s): pulling %s from %.1f m", best / g_rig_scale, class_name(g.target),
                  vr::length(g.c - g_near.palm) / g_rig_scale);
            vr::openvr::haptic(g_hand, 0.06f, 70.0f, 0.9f);
            to(GravityGlove::kPulling);
        }
        return;
    }
    case GravityGlove::kPulling:
        // The game takes it: its cursor and its pick are on the target, its button held down (not
        // let go early: a quick press and release was a tap, and a rock broke in two). A tree is
        // first pulled out of the ground (hand state 3). Whatever comes into the hand (from a field,
        // a pile or a workshop it is a new thing) flies from where the target was.
        // (Once the game names what it holds: in the first frame of holding it names nothing yet, and the
        // flight then followed nothing; the thing was simply in the hand; player, 2026-11-07: "objects
        // just teleport to your hand".)
        if (st == kHandStateHolding && held_object_3d_hand()) {
            void* held = held_object_3d_hand();
            if (held != g.fly_obj3d) g.fly_from = g.c;
            g.fly_obj3d = held;
            g.fly_at = now;
            to(GravityGlove::kFlying);
        } else if (now - g.at > 2.0 || !alive(g.target)) {
            BWLOG("hands: gravity gloves: the pull didn't take (hand state %u)", st);
            to(GravityGlove::kIdle), g.target = nullptr;
        }
        return;
    case GravityGlove::kFlying:
        // All the way into the hand, the grip open or not; arrived with the grip open, it is let go
        // of there, not thrown (it flew past the player with the flick's speed; 2026-11-07).
        if (now - g.fly_at > g_set.gravity_flight_s || st != kHandStateHolding) {
            if (st == kHandStateHolding && !open) vr::openvr::haptic(g_hand, 0.05f, 160.0f, 0.7f);  // (caught)
            if (open) g_gg_soft_until = now + 0.6, g_ctl_vel.clear();
            to(GravityGlove::kIdle), g.target = nullptr;
        }
        return;
    }
}
struct Fling {
    double t = -10;
    Vec3 v, from;  // the hand's velocity, and where it let go (the palm)
};
Fling g_fling;
bool g_spell_at_palm = true;  // (debug: hands spell_at_palm 0)
// A fireball thrown from the hand (hand mode, within 2 s of letting go of a charged miracle at
// speed): it leaves from the palm at the hand's velocity, as a thrown rock does (player,
// 2026-10-25: it didn't come out of the hand, nor go where the hand swung).
bool fireball_launch(void* object, game::LHPoint& v) {
    if (!g_spell_at_palm || !g_set.vr_throw || now_s() - g_fling.t > 2.0 || g_near.blend < 0.5f) return false;
    const float k = g_set.hand_throw_gain;
    v = {g_fling.v.x * k, g_fling.v.y * k, g_fling.v.z * k};
    char* o3 = *reinterpret_cast<char**>(static_cast<char*>(object) + 0x40);
    if (o3 && !IsBadWritePtr(o3 + 0x14, 0x30)) {
        float* m = reinterpret_cast<float*>(o3 + 0x14);
        m[9] = g_near.palm.x, m[10] = g_near.palm.y, m[11] = g_near.palm.z;
    }
    return true;
}
void detect_shake(const Controller& c) {
    static Vec3 prev;
    static double prev_t = 0, last_rev = -10;
    static int dir = 0, count = 0;
    const double now = now_s();
    const float dt = float(now - prev_t);
    if (dt > 0.004f && dt < 0.1f && game_hand_state() != 7) {  // (not while stroking: a rub is not a shake)
        const float v = vr::dot(c.grip.pos - prev, c.grip.rot.right) / dt;
        const int d = v > g_set.shake_speed_m_s ? 1 : v < -g_set.shake_speed_m_s ? -1 : 0;
        if (d && d != dir) {
            count = now - last_rev < 0.35 ? count + 1 : 1;
            last_rev = now, dir = d;
            // (Only with something to shake off: the leash, or a charged miracle (hand state 8). A
            // spiral drawn with the empty hand for a gesture has the same reversals, and the shake
            // cancelled the Miracle selection it had just opened; player, 2026-10-27.)
            // (And a villager in the hand: shaken, the game drops it and takes its disciple's role away;
            // player, 2026-11-13: lost since the shake needed something to shake off.)
            const bool something = (game_hand_state() == 8 || leash_in_hand() || held_villager()) && !g_gd.on && !g_shield_draw &&
                                   now_s() >= g_shield_tail_until;
            if (count >= g_set.shake_reversals && now - g_shake_at > 1.0 && !something) {
                static double logged = -10;
                if (now - logged > 5.0) logged = now, BWLOG("hands: a shake with nothing to shake off: not given to the game");
                count = 0;
            } else if (count >= g_set.shake_reversals && now - g_shake_at > 1.0) {  // (one shake, however long it goes on)
                g_shake_at = now, count = 0;
                BWLOG("hands: shake");
                if (leash_in_hand()) g_shake_leash_at = now;
                vr::openvr::haptic(g_hand, 0.04f, 160.0f, 0.5f);
            }
        }
    }
    if (dt > 0.004f || dt <= 0) prev = c.grip.pos, prev_t = now;
}
using ActionFn = int(__fastcall*)(void* gi, void* edx, int action);
ActionFn o_action = nullptr;
int __fastcall action_detour(void* gi, void* edx, int action) {
    // (It is asked every frame, by each place that acts on it: each gets one yes per shake.)
    if (false && action == 5 && now_s() - g_fling.t < 0.25) {  // (action 5 is the shake: the miracle vanished)
        static double answered_for = -1;
        static std::vector<void*> answered;
        if (answered_for != g_fling.t) answered.clear(), answered_for = g_fling.t;
        void* caller = _ReturnAddress();
        if (std::find(answered.begin(), answered.end(), caller) == answered.end()) {
            answered.push_back(caller);
            BWLOG("hands: fling given to the game (asked by %p)", caller);
            return 1;
        }
    }
    if (action == 5 && now_s() - g_shake_at < 0.5) {
        static std::vector<void*> answered;
        static double answered_for = -1;
        if (answered_for != g_shake_at) answered.clear(), answered_for = g_shake_at;
        void* caller = _ReturnAddress();
        if (std::find(answered.begin(), answered.end(), caller) == answered.end()) {
            answered.push_back(caller);
            BWLOG("hands: shake given to the game (asked by %p)", caller);
            g_shaken_at = now_s();
            return 1;
        }
    }
    const int r = o_action(gi, edx, action);
    if (now_s() < g_dbg_gesture_until) {
        static uint64_t asked = 0;  // (each action asked about, logged once)
        if (action >= 0 && action < 64 && !(asked >> action & 1)) asked |= 1ull << action, BWLOG("gesture: the game asks about action %d (from %p)", action, _ReturnAddress());
    }
    if (r) {  // (always logged: rare, and what a player's gesture did)
        static double logged_at = -10;
        static int logged_action = -1;
        if (action != logged_action || now_s() - logged_at > 1.0)
            BWLOG("gesture: the game recognises action %d (asked by %p)", action, _ReturnAddress());
        logged_at = now_s(), logged_action = action;
    }
    return r;
}

// The warning when the hand leaves the player's influence (the mana path flowing back to the ring,
// and its sound). GVirtualInfluence (GInterfaceStatus+0x130) is updated each game turn
// (0x76CD40): it reads the status's hand point (GInterfaceStatus+0x14, map coordinates), keeps
// the last point inside the player's raw influence (+8) and ramps an amount (+0x30) up there and
// down outside, the faster the further the point is from the kept one. Its Draw (0x76D000, from
// CHand::Draw) shows the path, and the sound, while that amount is up, the kept point is inside and
// the game hand (GInterface+0x3A0 -> CHand, position +0x78) is outside. The status's point is the
// game's ground point under the cursor seen from the game camera (at the head, its direction
// smoothed); for a cursor low in the view (the hand at the controller) or a head not facing the
// laser it lands hundreds of units off, and the warning died at once. So both calls get the mod's
// point: the ground below the palm in hand mode, the laser's land hit in laser mode.
using VirtualInfluenceFn = void(__fastcall*)(void* vi, void* edx);
VirtualInfluenceFn o_vinfl_update = nullptr, o_vinfl_draw = nullptr;
bool g_vinfl_mod_point = true;  // (debug: hands vinfl_mod 0 leaves both to the game)
bool vinfl_point(const void* vi, Vec3& p) {
    if (!g_vinfl_mod_point || !g_game_hand || !g_ptr.valid) return false;
    // (Is this the player's warning? GVirtualInfluence +0x38 is its GInterfaceStatus. The hand's
    // status pointer isn't followed: it was garbage for a moment after a jump of the rig, and
    // reading its +0x130 crashed the game.)
    if (IsBadReadPtr(static_cast<const char*>(g_game_hand) + 0x4850, 4) || IsBadReadPtr(static_cast<const char*>(vi) + 0x38, 4)) return false;
    const char* status = *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850);
    if (!status || *reinterpret_cast<const char* const*>(static_cast<const char*>(vi) + 0x38) != status) return false;
    if (g_near.blend >= 0.5f) p = g_near.palm;
    else if (g_ptr.hit_land) p = g_ptr.hit;
    else return false;
    return true;
}
void __fastcall vinfl_update_detour(void* vi, void* edx) {
    Vec3 p;
    if (!vinfl_point(vi, p)) return o_vinfl_update(vi, edx);
    char* status = *reinterpret_cast<char**>(static_cast<char*>(vi) + 0x38);
    int32_t* mc = reinterpret_cast<int32_t*>(status + 0x14);
    const int32_t saved[2] = {mc[0], mc[1]};
    mc[0] = int32_t(p.x * 6553.6f), mc[1] = int32_t(p.z * 6553.6f);
    o_vinfl_update(vi, edx);
    mc[0] = saved[0], mc[1] = saved[1];
}
// Outside the player's influence the game lets the hand do nothing: GInterface +0x48 is set each
// turn (fn_005D1120) from Influence::CalculatePlayerInfluence at the hand's point (+0x3F0), and
// nothing is picked up or tapped while it is 0. Only the warning (the path back to the ring, which
// fades out further on) said so, and nothing on the hand (player, 2026-10-20). Now the god hand
// fades and cools while the game says so (to [hands] outside_alpha_percent), and the controller
// gives a dull bump as the hand crosses out and a light tick as it comes back in.
float g_outside = 0;  // 0 inside .. 1 outside, eased
bool hand_cannot_act() {
    if (g_cinema || !g_game_hand) return false;
    const char* gi = static_cast<const char*>(game_interface());
    return gi && !IsBadReadPtr(gi + 0x48, 4) && *reinterpret_cast<const int*>(gi + 0x48) == 0;
}
void update_outside_cue() {
    static double last = now_s();
    static bool was = false;
    const double now = now_s();
    const float dt = float(std::clamp(now - last, 0.0, 0.1));
    last = now;
    const bool out = g_set.outside_alpha < 0.99f && hand_cannot_act();
    if (out != was) {
        if (out) vr::openvr::haptic(g_hand, 0.07f, 70.0f, 0.8f);
        else vr::openvr::haptic(g_hand, 0.015f, 220.0f, 0.3f);
        const Vec3 p = g_near.blend >= 0.5f ? g_near.palm : g_ptr.hit;
        BWLOG("hands: the hand %s the player's influence at (%.1f, %.1f)", out ? "left (it can't act)" : "is back in", p.x, p.z);
        was = out;
    }
    g_outside = std::clamp(g_outside + (out ? 4.0f : -4.0f) * dt, 0.0f, 1.0f);
}
// The path itself is particles (PSysGlobal::AddManaPathSprite: a SpellPointInf each, flying from a
// point on the ring's edge to the hand's point on the land). In hand mode they flowed to the land
// below the hand rather than to the hand (player, 2026-10-20): while the warning draws, the
// particles' end is the palm (SpellPointInf's constructor, 0x69AC10: from, speed, to, colour).
const Vec3* g_mana_path_end = nullptr;
// In hand mode the stream is the mod's own (drawn in hand::draw): glowing motes leaving the palm
// and arcing back to the influence's edge. The game's particles are sized for its camera far off;
// at the hand they were big soft blobs on the land below it, nothing seen to leave the hand
// (player, 2026-11-03). PSysGlobal::AddManaPathSprite (0x68FB20) is skipped then, its edge kept.
struct ManaStream {
    double at = -10;  // last asked for
    Vec3 ring;        // the influence's edge end
    uint32_t colour = 0;
};
ManaStream g_mana;
bool g_beam_still = false;  // (hands beam_still 1: the pointer's animation held, for tests that compare pictures)
bool g_mana_own = true;  // (hands mana_own 0: the game's particles, as before)
// (A cdecl function: from, to (MapCoords), the player's colour, a strength; the caller cleans up.)
using AddManaPathFn = void(__cdecl*)(const int32_t* a, const int32_t* b, uint32_t colour, float f);
AddManaPathFn o_add_mana_path = nullptr;
void __cdecl add_mana_path_detour(const int32_t* a, const int32_t* b, uint32_t colour, float f) {
    if (!g_mana_path_end || !g_mana_own || !a || !b || IsBadReadPtr(a, 8) || IsBadReadPtr(b, 8)) return o_add_mana_path(a, b, colour, f);
    const float ax = float(a[0]) / 6553.6f, az = float(a[1]) / 6553.6f, bx = float(b[0]) / 6553.6f, bz = float(b[1]) / 6553.6f;
    const Vec3& palm = *g_mana_path_end;
    const bool a_ring = std::hypot(ax - palm.x, az - palm.z) > std::hypot(bx - palm.x, bz - palm.z);
    const float rx = a_ring ? ax : bx, rz = a_ring ? az : bz;
    g_mana.ring = {rx, std::max(0.0f, ground_height(rx, rz)), rz};
    g_mana.colour = colour;
    if (now_s() - g_mana.at > 1.0)
        BWLOG("hands: the influence warning: the mod's stream from the palm to the edge (%.1f, %.1f, %.1f), colour %08X", rx, g_mana.ring.y, rz, colour);
    g_mana.at = now_s();
}
using SpellPointInfCtor = void*(__fastcall*)(void* self, void* edx, const game::LHPoint* from, float speed, const game::LHPoint* to,
                                             uint32_t colour);
SpellPointInfCtor o_spell_point_inf = nullptr;
void* __fastcall spell_point_inf_detour(void* self, void* edx, const game::LHPoint* from, float speed, const game::LHPoint* to,
                                        uint32_t colour) {
    if (g_mana_path_end && to && from) {
        // (Whichever end lies under the hand: the particles flow from the hand back to the ring.)
        const game::LHPoint palm{g_mana_path_end->x, g_mana_path_end->y, g_mana_path_end->z};
        const auto across = [&](const game::LHPoint* q) { return std::hypot(q->x - palm.x, q->z - palm.z); };
        static double logged_at = -10;
        if (now_s() - logged_at > 1.0) {
            logged_at = now_s();
            BWLOG("hands: the warning's path: from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f): now from the palm (%.1f, %.1f, %.1f) to the %s", from->x,
                  from->y, from->z, to->x, to->y, to->z, palm.x, palm.y, palm.z, across(from) <= across(to) ? "game's end" : "game's start");
        }
        // (They leave the palm and stream back to the ring: the game's start, the ring's edge, is often
        // nearly under the hand, and the stream seemed to rise from the land below it rather than
        // come from the hand; player, 2026-11-03.)
        const game::LHPoint* ring = across(from) <= across(to) ? to : from;
        return o_spell_point_inf(self, edx, &palm, speed, ring, colour);
    }
    return o_spell_point_inf(self, edx, from, speed, to, colour);
}
// A charged miracle in the hand (the water miracle's rain cloud, say) is drawn by
// CHand::DrawSpellInHand (0x46E680) at CHand +0x4964: a bone of the game's hand mesh (when byte
// +0x4954 is set) or the hand's own point (+0x78). Both are the game's hand on the land below the
// VR hand, so up high the cloud sat on the ground (player, 2026-10-24). In hand mode it is the palm.
using DrawSpellInHandFn = void(__fastcall*)(void* hand, void* edx);
DrawSpellInHandFn o_draw_spell_in_hand = nullptr;
void __fastcall draw_spell_in_hand_detour(void* hand, void* edx) {
    if (!g_spell_at_palm || hand != g_game_hand || g_near.blend < 0.5f || g_cinema) return o_draw_spell_in_hand(hand, edx);
    char* h = static_cast<char*>(hand);
    auto& hp = *reinterpret_cast<game::LHPoint*>(h + 0x78);
    const game::LHPoint saved = hp;
    const uint8_t saved_bone = *reinterpret_cast<uint8_t*>(h + 0x4954);
    hp = {g_near.palm.x, g_near.palm.y, g_near.palm.z};
    *reinterpret_cast<uint8_t*>(h + 0x4954) = 0;
    o_draw_spell_in_hand(hand, edx);
    hp = saved;
    *reinterpret_cast<uint8_t*>(h + 0x4954) = saved_bone;
    static double logged_at = -10;
    if (now_s() - logged_at > 2.0 && *reinterpret_cast<void**>(h + 0x494C)) {
        logged_at = now_s();
        const auto& at = *reinterpret_cast<const game::LHPoint*>(h + 0x4964);
        BWLOG("hands: the miracle in the hand drawn at the palm (%.1f, %.1f, %.1f); the game's hand (%.1f, %.1f, %.1f), bone %d", at.x, at.y,
              at.z, saved.x, saved.y, saved.z, saved_bone);
    }
}

// A charged miracle thrown from the hand (a fireball): SpellSeed::ThrowObjectFromHand (0x72ACD0,
// thiscall(status, int)) goes to Object::ThrowObjectFromHand (0x6385E0), which puts the seed at
// GInterfaceStatus +0x5C (the game's hand: on the land below the VR hand) and launches it from
// there with the status's velocity. A fireball left from the ground, not the hand (player,
// 2026-10-24). In hand mode it leaves from the palm.
using SeedThrowFn = int(__fastcall*)(void* seed, void* edx, void* status, int arg);
SeedThrowFn o_seed_throw = nullptr;
int __fastcall seed_throw_detour(void* seed, void* edx, void* status, int arg) {
    if (!g_spell_at_palm || !status || IsBadWritePtr(static_cast<char*>(status) + 0x44, 0x30) || g_near.blend < 0.5f || g_cinema)
        return o_seed_throw(seed, edx, status, arg);
    float* f = reinterpret_cast<float*>(static_cast<char*>(status) + 0x44);  // +0x44 .. +0x70
    float saved[3];
    memcpy(saved, f + (0x5C - 0x44) / 4, sizeof(saved));
    BWLOG("hands: a miracle thrown from the hand: %s; the game's point (%.1f, %.1f, %.1f), the palm (%.1f, %.1f, %.1f); status +44 (%.2f, %.2f, %.2f) "
          "+50 (%.2f, %.2f, %.2f) +68 (%.2f, %.2f, %.2f)",
          class_name(seed), saved[0], saved[1], saved[2], g_near.palm.x, g_near.palm.y, g_near.palm.z, f[0], f[1], f[2], f[3], f[4], f[5], f[9], f[10],
          f[11]);
    float* p = f + (0x5C - 0x44) / 4;
    p[0] = g_near.palm.x, p[1] = g_near.palm.y, p[2] = g_near.palm.z;
    // (+0x44 is the velocity handed on to the launch: Object::ThrowObjectFromHand passes it first
    // to vtable 0x780, as PhysicsObject::AddObject gets it.)
    float saved_v[3];
    memcpy(saved_v, f, sizeof(saved_v));
    if (now_s() - g_fling.t < 1.0) {
        const float k = g_set.hand_throw_gain;
        f[0] = g_fling.v.x * k, f[1] = g_fling.v.y * k, f[2] = g_fling.v.z * k;
    }
    const int r = o_seed_throw(seed, edx, status, arg);
    memcpy(p, saved, sizeof(saved));
    memcpy(f, saved_v, sizeof(saved_v));
    return r;
}

// (Logged: a miracle let go of slowly is applied where the hand is, SpellSeed::ApplyThisToMapCoord
// (0x728E20, thiscall(status, MapCoords*, gesture data)), not thrown.)
using SeedApplyMapFn = int(__fastcall*)(void* seed, void* edx, void* status, const int32_t* mc, void* gesture);
SeedApplyMapFn o_seed_apply_map = nullptr;
// Lightning aimed with the laser (the zap, or the laser hand): the game takes its point from the
// cursor's place on its flat screen, through its own camera, and in VR that fell short of where the
// laser points (17 units toward the player at 40 out; the shield's circle had the same). Each bolt
// is applied at the laser's point on the land instead.
int __fastcall seed_apply_map_detour(void* seed, void* edx, void* status, const int32_t* mc, void* gesture) {
    int32_t aimed[3];
    if (mc && (g_zap || !g_near.on) && g_ptr.hit_land && g_set.zap_aim) {
        using InfoFn = void*(__thiscall*)(void*);
        void* info = reinterpret_cast<InfoFn>(0x7290E0)(seed);
        const int type = info ? reinterpret_cast<int(__cdecl*)(void*)>(0x72B200)(info) : -1;
        if (type == 6) {
            memcpy(aimed, mc, sizeof(aimed));
            aimed[0] = int32_t(std::lround(g_ptr.hit.x * 6553.6f)), aimed[1] = int32_t(std::lround(g_ptr.hit.z * 6553.6f));
            mc = aimed;
        }
    }
    BWLOG("hands: a miracle applied where the hand is (%.1f, %.1f): %s", mc ? mc[0] / 6553.6f : 0.0f, mc ? mc[1] / 6553.6f : 0.0f, class_name(seed));
    return o_seed_apply_map(seed, edx, status, mc, gesture);
}

// A miracle cast from the hand: SpellSeed's cast makes the spell with fn_005FB490 (thiscall:
// point, player, ?, Spell** out, ?; ret 14h). A thrown one (a fireball: Spell) holds where it
// starts at +0x70, the game's hand (CHand +0x78) on the land below the VR hand, and its target at
// +0x64: the fireball flew from the ground (player, 2026-10-24). In hand mode it starts at the palm.
// (Debug: the spell's points for a moment after the cast, to see which of them is its flight.)
struct SpellWatch {
    void* spell = nullptr;
    double t = -10, last = -10;
};
SpellWatch g_spell_watch;
// (Debug: every copy in memory of a point, and the object it is in where that can be told: the
// nearest vtable of runblack.exe at or before it, within 0x600 bytes.)
struct PointCopy {
    const float* at;
    const void* owner;
    int off;
};
std::vector<PointCopy> g_point_copies;
void scan_memory_for(const float p[3], float tol, const char* only_class = nullptr, int only_off = -1) {
    g_point_copies.clear();
    MEMORY_BASIC_INFORMATION mbi;
    for (const char* a = reinterpret_cast<const char*>(0x10000); a < reinterpret_cast<const char*>(0x7FFE0000) && VirtualQuery(a, &mbi, sizeof(mbi));
         a = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
            continue;
        const float* f = static_cast<const float*>(mbi.BaseAddress);
        const size_t n = mbi.RegionSize / 4;
        for (size_t i = 0; i + 2 < n && g_point_copies.size() < 200; ++i) {
            if (!(std::fabs(f[i] - p[0]) <= tol && std::fabs(f[i + 2] - p[2]) <= tol && std::fabs(f[i + 1] - p[1]) <= tol)) continue;  // (NaN fails)
            const void* owner = nullptr;
            int off = 0;
            for (size_t k = 0; k <= 0x600 / 4 && k <= i; ++k) {
                const void* v = *reinterpret_cast<void* const*>(f + i - k);
                if (in_game_image(v) && reinterpret_cast<uintptr_t>(v) > 0x8A0000 && !IsBadReadPtr(v, 4) &&
                    in_game_image(*static_cast<void* const*>(v)) && reinterpret_cast<uintptr_t>(*static_cast<void* const*>(v)) < 0x8A0000) {
                    owner = f + i - k, off = int(k * 4);
                    break;
                }
            }
            if (only_class && (!owner || off != only_off || strcmp(class_name(owner), only_class) != 0)) continue;
            g_point_copies.push_back({f + i, owner, off});
        }
    }
    std::string out;
    for (const PointCopy& c : g_point_copies) {
        char buf[96];
        snprintf(buf, sizeof(buf), " %p(%s+%X)", static_cast<const void*>(c.at), c.owner ? class_name(c.owner) : "-", c.off);
        out += buf;
    }
    BWLOG("hands: copies of (%.1f, %.1f, %.1f): %zu:%s", p[0], p[1], p[2], g_point_copies.size(), out.c_str());
}
void recheck_point_copies(const float p[3]) {
    std::string out;
    for (const PointCopy& c : g_point_copies) {
        if (IsBadReadPtr(c.at, 12)) continue;
        if (std::fabs(c.at[0] - p[0]) < 0.5f && std::fabs(c.at[1] - p[1]) < 0.5f && std::fabs(c.at[2] - p[2]) < 0.5f) continue;
        char buf[128];
        snprintf(buf, sizeof(buf), " %p(%s+%X)=(%.1f, %.1f, %.1f)", static_cast<const void*>(c.at), c.owner ? class_name(c.owner) : "-", c.off, c.at[0],
                 c.at[1], c.at[2]);
        out += buf;
    }
    BWLOG("hands: of those copies, moved since:%s", out.empty() ? " none" : out.c_str());
}
bool g_dbg_scan = false;  // debug: hands scan 1: after a cast, every copy of the ground hand's point
// (Debug, hands atom_track 1: at a cast, the particles (AtomCore, position +0x80, velocity +0x34)
// at the ground hand's point; then where they are every 0.1 s for a second.)
bool g_dbg_atom_track = false;
// (Debug, hands liquid_log 1: the liquid particle groups (list at [0xEF7538], next at +0xB010,
// origin +0xB004, count +0xB000, callback +0xB018) for a second after a cast.)
bool g_dbg_liquid = false;
void liquid_log_tick() {
    static double last = -10;
    if (!g_dbg_liquid || now_s() - g_spell_watch.t > 1.2 || now_s() - last < 0.1) return;
    last = now_s();
    const char* grp = *reinterpret_cast<const char* const*>(0xEF7538);
    std::string out;
    for (int n = 0; grp && n < 12 && !IsBadReadPtr(grp + 0xB000, 0x20); ++n, grp = *reinterpret_cast<const char* const*>(grp + 0xB010)) {
        const float* o = reinterpret_cast<const float*>(grp + 0xB004);
        const float* p0 = reinterpret_cast<const float*>(grp);
        char buf[220];
        snprintf(buf, sizeof(buf), " [%p n %d cb %p origin (%.1f, %.1f, %.1f) first %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g]", static_cast<const void*>(grp),
                 *reinterpret_cast<const int*>(grp + 0xB000), *reinterpret_cast<void* const*>(grp + 0xB018), o[0], o[1], o[2], p0[0], p0[1], p0[2], p0[3], p0[4],
                 p0[5], p0[6], p0[7]);
        out += buf;
    }
    BWLOG("hands: liquid groups at %.2f s:%s", now_s() - g_spell_watch.t, out.c_str());
}
struct AtomTrack {
    std::vector<const char*> atoms;
    double t = -10, last = -10;
};
AtomTrack g_atom_track;
void atom_track_start() {
    if (!g_dbg_atom_track || !g_game_hand) return;
    float at[3];
    memcpy(at, static_cast<char*>(g_game_hand) + 0x78, sizeof(at));
    scan_memory_for(at, 3.0f, ".?AVAtomCore@@", 0x80);
    g_atom_track = {};
    for (const PointCopy& c : g_point_copies)
        if (c.owner && c.off == 0x80 && strcmp(class_name(c.owner), ".?AVAtomCore@@") == 0) g_atom_track.atoms.push_back(static_cast<const char*>(c.owner));
    g_atom_track.t = now_s();
}
void atom_track_tick() {
    AtomTrack& a = g_atom_track;
    if (a.atoms.empty() || now_s() - a.t > 1.1 || now_s() - a.last < 0.1) return;
    a.last = now_s();
    std::string out;
    for (const char* atom : a.atoms) {
        if (IsBadReadPtr(atom, 0xC0)) continue;
        const float* p = reinterpret_cast<const float*>(atom + 0x80);
        const float* v = reinterpret_cast<const float*>(atom + 0x34);
        char buf[128];
        snprintf(buf, sizeof(buf), " %p (%.1f, %.1f, %.1f) v (%.1f, %.1f, %.1f);", static_cast<const void*>(atom), p[0], p[1], p[2], v[0], v[1], v[2]);
        out += buf;
    }
    BWLOG("hands: atoms at %.2f s:%s", now_s() - a.t, out.c_str());
}
int g_dbg_scan_watch = 0;   // debug: hands scan_watch 1: with a miracle charged, find the ground point's copies, watch writes to a particle system's
void scan_watch_charged() {
    static double charged_at = -1;
    static bool done = false;
    if (!g_dbg_scan_watch || !g_game_hand) return;
    if (game_hand_state() != 8) {
        charged_at = -1;
        return;
    }
    if (charged_at < 0) charged_at = now_s(), done = false;
    if (done || now_s() - charged_at < 1.0) return;
    done = true;
    float at[3];
    memcpy(at, static_cast<char*>(g_game_hand) + 0x78, sizeof(at));
    scan_memory_for(at, 0.05f);
    for (const PointCopy& c : g_point_copies)
        if (c.owner && strcmp(class_name(c.owner), ".?AVPSysManager@@") == 0) {
            BWLOG("hands: watching the particle system point %p (%s+%X)", static_cast<const void*>(c.at), class_name(c.owner), c.off);
            stereo::watch_writes(reinterpret_cast<uintptr_t>(c.at));
            return;
        }
}
void watch_spell() {
    SpellWatch& w = g_spell_watch;
    if (!w.spell || now_s() - w.t > 1.6) return;
    if (now_s() - w.last < 0.1) return;
    w.last = now_s();
    {
        static double scanned_for = -1, rechecked_for = -1;
        static float scanned_at[3];
        if (g_dbg_scan && g_game_hand && scanned_for != w.t) {
            scanned_for = w.t;
            memcpy(scanned_at, static_cast<char*>(g_game_hand) + 0x78, sizeof(scanned_at));
            scan_memory_for(scanned_at, 0.05f);
        } else if (g_dbg_scan && rechecked_for != w.t && now_s() - w.t > 0.12) {
            rechecked_for = w.t;
            recheck_point_copies(scanned_at);
        }
    }
    if (IsBadReadPtr(w.spell, 0x400)) {
        w.spell = nullptr;
        return;
    }
    const float* f = static_cast<const float*>(w.spell);
    std::string out;
    for (int i = 1; i + 2 < 0x400 / 4; ++i)
        if (std::fabs(f[i]) > 500.0f && std::fabs(f[i]) < 6000.0f && std::fabs(f[i + 2]) > 500.0f && std::fabs(f[i + 2]) < 6000.0f &&
            std::fabs(f[i + 1]) < 1000.0f && f[i] == f[i] && f[i + 1] == f[i + 1]) {
            char buf[64];
            snprintf(buf, sizeof(buf), " +%X (%.1f, %.1f, %.1f)", i * 4, f[i], f[i + 1], f[i + 2]);
            out += buf;
        }
    BWLOG("hands: spell %p at %.2f s:%s", w.spell, now_s() - w.t, out.c_str());
    // (Which object, one or two pointers away, holds the ground hand's point (CHand +0x78): what
    // takes the hand on the land as its place.)
    float ground_pt[3] = {};
    if (g_game_hand) memcpy(ground_pt, static_cast<char*>(g_game_hand) + 0x78, sizeof(ground_pt));
    const float* cur = ground_pt;
    std::string found;
    auto near_cur = [&](const float* q) { return std::fabs(q[0] - cur[0]) < 3.0f && std::fabs(q[1] - cur[1]) < 3.0f && std::fabs(q[2] - cur[2]) < 3.0f; };
    const auto* words = static_cast<const uintptr_t*>(w.spell);
    for (int i = 0; i < 0x400 / 4 && found.size() < 600; ++i) {
        const uintptr_t p1 = words[i];
        if (p1 < 0x10000 || p1 == uintptr_t(w.spell) || IsBadReadPtr(reinterpret_cast<void*>(p1), 0x200)) continue;
        const float* g = reinterpret_cast<const float*>(p1);
        for (int j = 0; j + 2 < 0x200 / 4; ++j)
            if (near_cur(g + j)) {
                char buf[96];
                snprintf(buf, sizeof(buf), " [+%X]->%s +%X", i * 4, class_name(reinterpret_cast<void*>(p1)), j * 4);
                found += buf;
            }
        const auto* w2 = reinterpret_cast<const uintptr_t*>(p1);
        for (int k = 0; k < 0x100 / 4; ++k) {
            const uintptr_t p2 = w2[k];
            if (p2 < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(p2), 0x200)) continue;
            const float* h = reinterpret_cast<const float*>(p2);
            for (int j = 0; j + 2 < 0x200 / 4; ++j)
                if (near_cur(h + j)) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), " [+%X][+%X]->%s +%X", i * 4, k * 4, class_name(reinterpret_cast<void*>(p2)), j * 4);
                    found += buf;
                }
        }
    }
    BWLOG("hands: spell %p: the ground hand's point (%.1f, %.1f, %.1f) is in:%s", w.spell, cur[0], cur[1], cur[2], found.empty() ? " nothing" : found.c_str());
}
// A cast spell follows the hand until it flies, from its GInterfaceStatus (+0xA8): the status's
// 3D hand point (+0xA4; the one before at +0xC8, +0xD4), set each turn from the hand's packet by
// fn_005DBFB0 (thiscall: map point -> +0x14, 3D point -> +0xA4, +0xB0, +0xBC, flag; ret 14h;
// found with watch_write). That is the game's hand on the land below the VR hand: a fireball left
// from the ground (player, 2026-10-25). In hand mode, with a miracle charged in the hand and for a
// moment after one is cast, the 3D point is the palm.
using SetHandInfoFn = void(__fastcall*)(void* status, void* edx, const void* map, const game::LHPoint* p3, const void* c, const void* d, int flag);
SetHandInfoFn o_set_hand_info = nullptr;
void __fastcall set_hand_info_detour(void* status, void* edx, const void* map, const game::LHPoint* p3, const void* c, const void* d, int flag) {
    void* mine = game_hand_alive() ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850)
                                                                                         : nullptr;
    if (g_spell_at_palm && status == mine && p3 && g_near.blend >= 0.5f && !g_cinema &&
        (game_hand_state() == 8 || now_s() - g_spell_watch.t < 1.6)) {
        const game::LHPoint palm{g_near.palm.x, g_near.palm.y, g_near.palm.z};
        return o_set_hand_info(status, edx, map, &palm, c, d, flag);
    }
    o_set_hand_info(status, edx, map, p3, c, d, flag);
}
// Each turn GInterfaceStatus::UpdateSpellInfo (0x5DC8F0, thiscall(Spell*, PSysProcessInfo*); ret 8)
// gives the spell the status's +0xC8 as where it is (found with hands watch_spell 1): that is set
// back to the game's hand within the turn, so a fireball's first turn was on the land. For the
// hand's status in hand mode (a miracle charged, or just cast), +0xC8 is the palm for that call.
using UpdateSpellInfoFn = void(__fastcall*)(void* status, void* edx, void* spell, void* psys);
UpdateSpellInfoFn o_update_spell_info = nullptr;
void __fastcall update_spell_info_detour(void* status, void* edx, void* spell, void* psys) {
    void* mine = game_hand_alive() ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850)
                                                                                         : nullptr;
    if (!g_spell_at_palm || status != mine || g_near.blend < 0.5f || g_cinema || (game_hand_state() != 8 && now_s() - g_spell_watch.t > 1.6))
        return o_update_spell_info(status, edx, spell, psys);
    float* q = reinterpret_cast<float*>(static_cast<char*>(status) + 0xC8);
    float saved[3];
    memcpy(saved, q, sizeof(saved));
    q[0] = g_near.palm.x, q[1] = g_near.palm.y, q[2] = g_near.palm.z;
    o_update_spell_info(status, edx, spell, psys);
    memcpy(q, saved, sizeof(saved));
    // (The spell's info here is its own +0x64: +0x0 the target, set along the swing elsewhere; +0xC the start.)
    // Where it flies: the spell's target (+0x64) is the game's cursor on the land, which in hand
    // mode has nothing to do with the swing: a fireball pitched forward went off to the right or
    // the left (player, 2026-10-25). After a release at speed, the target is where the swing points:
    // where its line meets the land, or for a level or upward throw ahead along it, the further the
    // faster.
    if (spell && now_s() - g_fling.t < 2.0 && !IsBadWritePtr(static_cast<char*>(spell) + 0x64, 12)) {
        const Vec3 v = g_fling.v;
        const float speed = vr::length(v);
        if (speed > 1e-3f) {
            const Vec3 d = v * (1.0f / speed);
            const Vec3 from = g_fling.from;
            Vec3 aim;
            const float down = from.y - std::max(0.0f, ground_height(from.x, from.z));
            if (d.y < -0.1f && down > 0) {
                aim = from + d * (down / -d.y);
            } else {
                Vec3 flat{d.x, 0, d.z};
                const float fl = vr::length(flat);
                flat = fl > 1e-3f ? flat * (1.0f / fl) : Vec3{0, 0, 1};
                const float reach = std::clamp(speed * g_set.hand_throw_gain * (1.0f + std::max(0.0f, d.y) * 2.0f), 30.0f, 600.0f);
                aim = from + flat * reach;
            }
            aim.y = std::max(0.0f, ground_height(aim.x, aim.z));
            float* t = reinterpret_cast<float*>(static_cast<char*>(spell) + 0x64);
            t[0] = aim.x, t[1] = aim.y, t[2] = aim.z;
            static double logged_for = -1;
            if (logged_for != g_fling.t) {
                logged_for = g_fling.t;
                BWLOG("hands: the thrown spell aims along the swing: from (%.1f, %.1f, %.1f) at (%.1f, %.1f, %.1f)", from.x, from.y, from.z, aim.x, aim.y,
                      aim.z);
            }
        }
    }
}
// (Debug: the fireballs themselves, MagicFireBall::Create (0x682970, cdecl(MapCoords*, info,
// atom); static): where each is made, then where it is every 0.1 s for 2 s.)
bool g_dbg_atoms = false;  // debug: hands atoms 1
void dump_atom(const char* when, const void* atom) {
    if (!atom || IsBadReadPtr(atom, 0x100)) return;
    const float* f = static_cast<const float*>(atom);
    std::string out;
    for (int i = 0; i < 0x100 / 4; ++i) {
        char buf[40];
        snprintf(buf, sizeof(buf), " %X:%.3g", i * 4, f[i]);
        out += buf;
    }
    BWLOG("hands: atom %p %s:%s", atom, when, out.c_str());
}
using FireBallCreateFn = void*(__cdecl*)(const int32_t* mc, void* info, void* atom);
FireBallCreateFn o_fireball_create = nullptr;
struct FireTrack {
    void* ball = nullptr;
    void* atom = nullptr;
    double t = -10, last = -10;
};
FireTrack g_fire_track;
void* __cdecl fireball_create_detour(const int32_t* mc, void* info, void* atom) {
    void* ball = o_fireball_create(mc, info, atom);
    // The fireball is a particle atom flown by its particle system: position at +0x80, velocity at
    // +0x34 (units/s; gravity pulls it down). It was given the game's hand's own motion, all but
    // still: it dropped to the ground from where it was made (player, 2026-10-25). Thrown from the
    // hand at speed in hand mode, it gets the hand's velocity, as a thrown rock does.
    bool given = false;
    if (g_spell_at_palm && g_set.vr_throw && atom && now_s() - g_fling.t < 2.0 && g_near.blend >= 0.5f &&
        !IsBadWritePtr(static_cast<char*>(atom) + 0x34, 12)) {
        float* av = reinterpret_cast<float*>(static_cast<char*>(atom) + 0x34);
        const float k = g_set.hand_throw_gain;
        BWLOG("hands: the fireball gets the hand's velocity (%.1f, %.1f, %.1f) (the game's (%.1f, %.1f, %.1f))", g_fling.v.x * k, g_fling.v.y * k,
              g_fling.v.z * k, av[0], av[1], av[2]);
        av[0] = g_fling.v.x * k, av[1] = g_fling.v.y * k, av[2] = g_fling.v.z * k;
        // (And where it is: the particle may have been flying a moment already with the game's
        // velocity, which a jump of the hand point to the palm made huge (once 190 units/s up: it
        // appeared far off). It is where the palm let go, moved on by the time since.)
        if (!IsBadWritePtr(static_cast<char*>(atom) + 0x80, 12)) {
            float* ap = reinterpret_cast<float*>(static_cast<char*>(atom) + 0x80);
            const float dt = float(now_s() - g_fling.t);
            ap[0] = g_fling.from.x + av[0] * dt, ap[1] = g_fling.from.y + av[1] * dt, ap[2] = g_fling.from.z + av[2] * dt;
            // (And its two other copies: the particle is drawn from +0xD0's matrix, the position at
            // +0xF4, and with only +0x80 set it was drawn rising from the land.)
            for (int off : {0xBC, 0xF4})
                if (!IsBadWritePtr(static_cast<char*>(atom) + off, 12)) memcpy(static_cast<char*>(atom) + off, ap, 12);
        }
        given = true;
    }
    (void)given;
    const float* ap = atom && !IsBadReadPtr(static_cast<char*>(atom) + 0x80, 12) ? reinterpret_cast<const float*>(static_cast<char*>(atom) + 0x80) : nullptr;
    BWLOG("hands: a fireball made %p at map (%.1f, %.1f); its atom %p at (%.1f, %.1f, %.1f); the palm (%.1f, %.1f, %.1f)", ball,
          mc ? mc[0] / 6553.6f : 0.0f, mc ? mc[1] / 6553.6f : 0.0f, atom, ap ? ap[0] : 0.0f, ap ? ap[1] : 0.0f, ap ? ap[2] : 0.0f, g_near.palm.x,
          g_near.palm.y, g_near.palm.z);
    if (!g_fire_track.ball || now_s() - g_fire_track.t > 2.0) g_fire_track = {ball, atom, now_s(), -10};
    if (g_dbg_atoms) dump_atom("made", atom);
    return ball;
}
// The thrown fireball's particle carries a DrawOffsetDecay (AtomCore +0x124; the offset at +0x2C,
// shrinking each turn): it is drawn from where the game's own throw would start, the game's hand on
// the land, blending into its real path. With the path starting at the palm, it was drawn from
// the land below the hand, 95 units down, closing in over a second or two (found by hooking
// Particle3DSprite::DrawAt). The offset is held at zero: drawn on its path from the palm.
void fireball_no_decay() {
    const FireTrack& f = g_fire_track;
    if (!g_spell_at_palm || !f.atom || now_s() - f.t > 3.0 || IsBadReadPtr(static_cast<char*>(f.atom) + 0x124, 4)) return;
    char* off = *reinterpret_cast<char**>(static_cast<char*>(f.atom) + 0x124);
    if (!off || IsBadWritePtr(off + 0x2C, 12) || *reinterpret_cast<void**>(off) != reinterpret_cast<void*>(0x9390E8)) return;  // (DrawOffsetDecay)
    float* o = reinterpret_cast<float*>(off + 0x2C);
    if (o[0] != 0 || o[1] != 0 || o[2] != 0) {
        static double logged_for = -1;
        if (logged_for != f.t) logged_for = f.t, BWLOG("hands: the thrown fireball's draw offset (%.1f, %.1f, %.1f) held at zero", o[0], o[1], o[2]);
        o[0] = o[1] = o[2] = 0;
    }
}
// (Held at zero where it is read, too: DrawOffsetDecay::GetOffset (0x6C7960, thiscall(LHPoint* out);
// ret 4). Its stored value is worked out again each turn, and zeroing it once a frame didn't hold.)
using DecayOffsetFn = void(__fastcall*)(void* off, void* edx, float* out);
DecayOffsetFn o_decay_offset = nullptr;
void __fastcall decay_offset_detour(void* off, void* edx, float* out) {
    const FireTrack& f = g_fire_track;
    if (g_spell_at_palm && out && f.atom && now_s() - f.t < 3.0 && !IsBadReadPtr(static_cast<char*>(f.atom) + 0x124, 4) &&
        *reinterpret_cast<void**>(static_cast<char*>(f.atom) + 0x124) == off) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    o_decay_offset(off, edx, out);
}
// Stroking or slapping the creature: HandStateCreature::Update (0x5B17B0, thiscall(float dt,
// LHMatrix*); ret 8). Once its wait (+0xDC, ms) passes 1000, the first update in which the cursor's
// speed is over 5 r strokes, and over 9 r slaps (r = 7.5 x the creature's scale, +0x90 of its 3D
// creature at hand +0x49B8). The speed is the horizontal distance between the points at the hand's
// depth (+0x4854) under the cursor now (+0x485C, screen x y) and before (+0x4864), over dt. A
// mouse jumps to its speed; a real hand speeds up smoothly through the stroke's band on its way
// to a slap, so it only ever stroked (player, 2026-10-30). With the hand at the controller, the
// hand's own speed decides: the cursor's step "before" is scaled for that update so the game reads
// a slap (13 r) for a fast swing, a stroke (7 r) for a rub held a moment, and nothing (under 5 r)
// otherwise. (Restored after: the game sets it itself.)
using PetUpdateFn = void(__fastcall*)(void* self, void* edx, float dt, void* m);
PetUpdateFn o_pet_update = nullptr;
using ScreenToPointFn = void(__fastcall*)(const int32_t* coord, float* out, float depth);  // fn_0074CAF0; ret 4
bool g_pet_log = false;  // debug: hands pet_log 1
float g_pet_speed_test = -1;  // debug: hands pet_speed <m/s> (the hand's speed taken as this; -1 measured)
struct PalmSample {
    double t;
    Vec3 p;
};
std::deque<PalmSample> g_pet_palm;
float pet_hand_speed_m_s() {
    const double now = now_s();
    if (g_pet_speed_test >= 0) return g_pet_speed_test;
    // (SteamVR's controller velocity where there is one: the fastest of the last 60 ms; else the
    // palm's track.)
    if (!g_ctl_vel.empty() && now - g_ctl_vel.back().t < 0.05) {
        float best = 0;
        for (const auto& s : g_ctl_vel)
            if (now - s.t < 0.06) best = std::max(best, vr::length(s.v));
        return best / std::max(1e-3f, g_rig_scale);
    }
    // (Relative to the rig: walking with the stick, or the rig set down elsewhere, isn't the hand's speed.)
    g_pet_palm.push_back({now, g_near.palm - g_rig.pos});
    while (g_pet_palm.size() > 2 && now - g_pet_palm.front().t > 0.08) g_pet_palm.pop_front();
    const double dt = g_pet_palm.back().t - g_pet_palm.front().t;
    if (dt < 0.02) return 0;
    return vr::length(g_pet_palm.back().p - g_pet_palm.front().p) / float(dt) / std::max(1e-3f, g_rig_scale);
}
void __fastcall pet_update_detour(void* self, void* edx, float dt, void* m) {
    char* hand = self ? *reinterpret_cast<char**>(static_cast<char*>(self) + 4) : nullptr;
    if (!g_set.pet_by_speed || !g_petting || !hand || dt <= 1e-5f || IsBadReadPtr(hand + 0x4854, 0x18) || IsBadReadPtr(hand + 0x49B8, 4))
        return o_pet_update(self, edx, dt, m);
    const char* lh = *reinterpret_cast<char* const*>(hand + 0x49B8);
    if (!lh || IsBadReadPtr(lh + 0x90, 4)) return o_pet_update(self, edx, dt, m);
    const float r = *reinterpret_cast<const float*>(lh + 0x90) * 7.5f;
    const float depth = *reinterpret_cast<const float*>(hand + 0x4854);
    int32_t* cur = reinterpret_cast<int32_t*>(hand + 0x485C);
    int32_t* prev = reinterpret_cast<int32_t*>(hand + 0x4864);
    const auto conv = reinterpret_cast<ScreenToPointFn>(0x74CAF0);
    const auto game_speed = [&](int32_t dx, int32_t dy) {
        const int32_t a[2] = {cur[0], cur[1]}, b[2] = {cur[0] - dx, cur[1] - dy};
        float pa[3], pb[3];
        conv(a, pa, depth), conv(b, pb, depth);
        return std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[2] - pb[2]) * (pa[2] - pb[2])) / dt;
    };
    // The hand's own speed, and how long it has been in the stroke's band.
    const double now = now_s();
    const float sp = pet_hand_speed_m_s();
    static double band_since = -1;
    if (sp < g_set.stroke_m_s || sp >= g_set.slap_m_s) band_since = -1;
    else if (band_since < 0) band_since = now;
    const bool slap = sp >= g_set.slap_m_s;
    const bool stroke = !slap && band_since >= 0 && now - band_since > 0.15;
    int32_t dx = cur[0] - prev[0], dy = cur[1] - prev[1];
    if (dx == 0 && dy == 0) dx = 1;
    const float s0 = game_speed(dx, dy);
    const float target = slap ? 13.0f * r : stroke ? 7.0f * r : std::min(s0, 4.0f * r);
    const float k = s0 > 1e-4f ? std::clamp(target / s0, 0.0f, 400.0f) : 0.0f;
    const int32_t saved[2] = {prev[0], prev[1]};
    int32_t mine[2] = {cur[0] - int32_t(std::lround(dx * k)), cur[1] - int32_t(std::lround(dy * k))};
    if ((slap || stroke) && mine[0] == cur[0]) mine[0] -= 1;  // (see below: the step must have a sideways part)
    prev[0] = mine[0], prev[1] = mine[1];
    // The speed is only looked at when the cursor crossed the screen's middle (x = width / 2, from
    // TheSystem +0x18's low word) since the last update; otherwise, with the hand on the body, the
    // game strokes once a second whatever the speed. The flat game's interaction camera centres
    // the creature, so a swipe across it crosses the middle; the VR view seldom does: it only ever
    // stroked (player, 2026-10-30). For a stroke or slap, the middle is put between the two
    // places for this update.
    uint32_t* sys_w = reinterpret_cast<uint32_t*>(0xE85058);
    const uint32_t saved_w = *sys_w;
    if ((slap || stroke) && cur[0] + mine[0] > 0 && cur[0] + mine[0] < 0xFFFF)
        *sys_w = (saved_w & 0xFFFF0000u) | uint32_t(cur[0] + mine[0]);
    // The game also strokes or slaps only with its hand between the ground and 1.1 r above it (and
    // picks the stroke's height on the body from where in that range it is: under 0.4 r low,
    // 0.7 r middle, else high). The VR hand on the creature's back or head was higher: nothing
    // happened, however it moved. For this update the hand's height is the palm's place on the
    // creature, from the ground to its top, brought into that range.
    float* hy = IsBadWritePtr(hand + 0x78, 12) ? nullptr : reinterpret_cast<float*>(hand + 0x7C);
    const float saved_y = hy ? *hy : 0.0f;
    float ratio = -1, made_ratio = -1, mine_y = saved_y;
    if (hy) {
        const float* hx = reinterpret_cast<const float*>(hand + 0x78);
        const float ground = std::max(0.0f, ground_height(hx[0], hx[2]));
        ratio = (*hy - ground) / std::max(1e-3f, r);
        Vec3 cc;
        float cr = 0;
        void* own = own_creature();
        if (own && bounding_sphere(own, nullptr, cc, cr) && cr > 0) {
            const float cground = std::max(0.0f, ground_height(cc.x, cc.z));
            const float top = std::max(cground + 1.0f, cc.y + cr);
            const float on_body = std::clamp((g_near.palm.y - cground) / (top - cground), 0.05f, 1.0f);
            *hy = mine_y = ground + on_body * 1.0f * r;
            made_ratio = on_body;
        }
    }
    const float made = game_speed(cur[0] - mine[0], cur[1] - mine[1]);
    const uint32_t wait = IsBadReadPtr(static_cast<char*>(self) + 0xDC, 4) ? 0 : *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + 0xDC);
    // (Both ways of acting set the timer +0xD0, which otherwise counts up, to 1000; the resting
    // stroke also resets +0xDC.)
    const int32_t* const d0 = IsBadReadPtr(static_cast<char*>(self) + 0xD0, 4) ? nullptr : reinterpret_cast<const int32_t*>(static_cast<char*>(self) + 0xD0);
    const int32_t d0_before = d0 ? *d0 : 0;
    o_pet_update(self, edx, dt, m);
    const bool acted = d0 && *d0 == 1000 && (d0_before > 1000 || d0_before < 960);
    if (hy && *hy == mine_y) *hy = saved_y;  // (unless the game moved it itself)
    *sys_w = saved_w;
    const uint32_t wait_after = IsBadReadPtr(static_cast<char*>(self) + 0xDC, 4) ? 0 : *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + 0xDC);
    const bool resting = wait > 1000 && wait_after < wait, by_speed = acted && !resting;
    if (by_speed || resting)
        BWLOG("hands: petting: the game %s the creature (the hand at %.2f m/s; the cursor's speed made %.0f, slap over %.0f; %s)",
              by_speed && made >= 9 * r ? "slapped" : "stroked", sp, made, 9 * r, by_speed ? "by its speed" : "resting");
    if (prev[0] == mine[0] && prev[1] == mine[1]) prev[0] = saved[0], prev[1] = saved[1];
    if (g_pet_log || ((slap || stroke) && wait > 1000)) {
        // (And whether the creature is in a body action, which holds a stroke or slap back while
        // under 35% through.)
        using IsActFn = bool(__fastcall*)(const void*, void*);
        const bool acting = reinterpret_cast<IsActFn>(0x4842D0)(lh, nullptr);
        static double logged_at = -10;
        if (g_pet_log || now - logged_at > 0.5)
            logged_at = now, BWLOG("hands: petting: the hand at %.2f m/s -> %s (the game's cursor %.0f, made %.0f; stroke over %.0f, slap over %.0f; wait %u ms; "
                                   "the game's hand %.2f r above the ground, made %.2f; in a body action %d)", sp, slap ? "slap" : stroke ? "stroke" : "nothing", s0,
                                   made, 5 * r, 9 * r, wait, ratio, made_ratio, acting);
    }
}
// A thrown fireball curves: UR_SideSpin::ModifyAtomCore (0x69E300, thiscall(AtomCore*); ret 4) takes a
// spin from its particle system at the start and turns the velocity sideways by it each turn (a
// curveball, in the flat game from the mouse's sideways motion). In VR the spin came from the
// head or the hand turning: a fireball pitched forward veered off left or right (player,
// 2026-10-26; found with a write watch on its velocity, hands watch_steer 1). The fireball thrown
// from the hand flies straight, as a rock does.
using SideSpinFn = void(__fastcall*)(void* rule, void* edx, void* atom);
SideSpinFn o_side_spin = nullptr;
bool g_no_side_spin = true;  // (debug: hands side_spin 1 puts it back)
void __fastcall side_spin_detour(void* rule, void* edx, void* atom) {
    if (g_no_side_spin && atom && atom == g_fire_track.atom && now_s() - g_fire_track.t < 10.0) return;
    o_side_spin(rule, edx, atom);
}
bool g_dbg_watch_steer = false;  // debug: hands watch_steer 1: who changes the thrown fireball's velocity in flight
void track_fireball() {
    FireTrack& f = g_fire_track;
    {
        static double watched_for = -1;
        if (g_dbg_watch_steer && f.atom && watched_for != f.t && now_s() - f.t > 0.3 && now_s() - f.t < 1.0) {
            watched_for = f.t;
            stereo::watch_writes(reinterpret_cast<uintptr_t>(f.atom) + 0x34);
        }
    }
    if (!f.ball || now_s() - f.t > 2.0 || now_s() - f.last < 0.1) return;
    f.last = now_s();
    char* o3 = !IsBadReadPtr(static_cast<char*>(f.ball) + 0x40, 4) ? *reinterpret_cast<char**>(static_cast<char*>(f.ball) + 0x40) : nullptr;
    const float* m = o3 && !IsBadReadPtr(o3 + 0x14, 0x30) ? reinterpret_cast<const float*>(o3 + 0x14) : nullptr;
    const float* ap = f.atom && !IsBadReadPtr(static_cast<char*>(f.atom) + 0x80, 24) ? reinterpret_cast<const float*>(static_cast<char*>(f.atom) + 0x80) : nullptr;
    if (g_dbg_atoms && now_s() - f.t < 0.35) dump_atom("later", f.atom);
    BWLOG("hands: fireball %p at %.2f s: object (%.1f, %.1f, %.1f), atom (%.1f, %.1f, %.1f) next (%.2f, %.2f, %.2f)", f.ball, now_s() - f.t,
          m ? m[9] : 0.0f, m ? m[10] : 0.0f, m ? m[11] : 0.0f, ap ? ap[0] : 0.0f, ap ? ap[1] : 0.0f, ap ? ap[2] : 0.0f, ap ? ap[3] : 0.0f,
          ap ? ap[4] : 0.0f, ap ? ap[5] : 0.0f);
}
// The hand's own particle systems (a charged miracle's glow, and the fireball it throws: its
// particles start where the system is) take where they are from a PSysProcessInfo in the hand's
// GInterfaceStatus (+0x138; its point at +0xC): the hand on the land. Each turn
// fn_00673340 (PSysManager, thiscall(info, ?); ret 8) copies the info (0x3C bytes) into the system
// (+0x24): found by finding every copy of the ground hand's point in memory and watching the
// writes to a particle system's one (hands scan_watch 1). The fireball came from the land below the
// VR hand (player, 2026-10-26). In hand mode the hand's systems are given the palm.
using PSysProcessFn = void(__fastcall*)(void* mgr, void* edx, const void* info, void* b);
PSysProcessFn o_psys_process = nullptr;
bool g_psys_at_palm = true;  // (debug: hands psys_at_palm 0)
void __fastcall psys_process_detour(void* mgr, void* edx, const void* info, void* b) {
    void* mine = game_hand_alive() ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850)
                                                                                         : nullptr;
    if (g_psys_at_palm && mine && info == static_cast<char*>(mine) + 0x138 && g_near.blend >= 0.5f && !g_cinema && !IsBadReadPtr(info, 0x3C)) {
        alignas(4) uint8_t copy[0x3C];
        memcpy(copy, info, sizeof(copy));
        float* p = reinterpret_cast<float*>(copy + 0xC);
        p[0] = g_near.palm.x, p[1] = g_near.palm.y, p[2] = g_near.palm.z;
        return o_psys_process(mgr, edx, copy, b);
    }
    if (g_game_hand && info && !IsBadReadPtr(info, 0x3C)) {  // (debug: a system at the ground hand that isn't the status's)
        const float* ip = reinterpret_cast<const float*>(static_cast<const char*>(info) + 0xC);
        const float* hp = reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78);
        static double logged_at = -10;
        if (std::fabs(ip[0] - hp[0]) < 1.0f && std::fabs(ip[1] - hp[1]) < 1.0f && std::fabs(ip[2] - hp[2]) < 1.0f && now_s() - logged_at > 1.0) {
            logged_at = now_s();
            BWLOG("hands: a particle system %p at the ground hand from info %p (the hand's status %p, +0x138 %p; blend %.2f) a %p", mgr, info, mine,
                  mine ? static_cast<char*>(mine) + 0x138 : nullptr, g_near.blend, b);
        }
    }
    o_psys_process(mgr, edx, info, b);
}
// Any particle system processed where the game's hand is on the land (within 2 units across of
// CHand +0x78, 10 up or down): GJPSysInterface::Process_ (0x6736B0, thiscall(PSysProcessInfo*, n); ret 8) is given a
// copy of its info placed at the palm, in hand mode. Most such infos are built on the stack each
// turn from the hand's point (the fireball that visibly flew from the ground was one; the
// status's own info is another). Each place that asks is logged once.
using GjProcessFn = unsigned(__fastcall*)(void* self, void* edx, const void* info, unsigned n);
GjProcessFn o_gj_process = nullptr;
unsigned __fastcall gj_process_detour(void* self, void* edx, const void* info, unsigned n) {
    if (g_psys_at_palm && g_game_hand && g_near.blend >= 0.5f && !g_cinema && info && !IsBadReadPtr(info, 0x3C)) {
        const float* ip = reinterpret_cast<const float*>(static_cast<const char*>(info) + 0xC);
        const float* hp = reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78);
        // (Over or under the hand too: some sit on the land below it, a few units lower.)
        if (std::fabs(ip[0] - hp[0]) < 2.0f && std::fabs(ip[2] - hp[2]) < 2.0f && std::fabs(ip[1] - hp[1]) < 10.0f) {
            static std::vector<void*> seen;
            void* from = _ReturnAddress();
            if (seen.size() < 32 && std::find(seen.begin(), seen.end(), from) == seen.end()) {
                seen.push_back(from);
                BWLOG("hands: a particle system at the game's hand moved to the palm (asked by %p, info %p)", from, info);
            }
            alignas(4) uint8_t copy[0x3C];
            memcpy(copy, info, sizeof(copy));
            float* p = reinterpret_cast<float*>(copy + 0xC);
            p[0] = g_near.palm.x, p[1] = g_near.palm.y + (ip[1] - hp[1]), p[2] = g_near.palm.z;
            return o_gj_process(self, edx, copy, n);
        }
    }
    return o_gj_process(self, edx, info, n);
}
// The fireball seen flying is a sprite drawn by sparklecbstub (0x5CDFC0, stdcall(LiquidParticleGroup*,
// GInterface*)) from DrawLiquidParticles, at the group's +0xB004 (found by bisecting the draws that
// paint it, log_draws). (Debug, hands sparkle_log 1: the group's fields around it after a cast.)
// (Debug, hands sprite_log 1: sprites (LH3DSprite::Draw, 0x840530; position at +0) drawn near
// the ground hand for a second after a cast, and who draws them.)
using SpriteDrawFn = void(__fastcall*)(void* sprite, void* edx);
SpriteDrawFn o_sprite_draw = nullptr;
bool g_dbg_sprite = false;
void __fastcall sprite_draw_detour(void* sprite, void* edx) {
    if (g_dbg_sprite && g_game_hand && sprite && (now_s() - g_spell_watch.t < 1.2 || game_hand_state() == 8) && !IsBadReadPtr(sprite, 12)) {
        const float* p = static_cast<const float*>(sprite);
        // (the land below the palm: where the game's hand is)
        const float gy = std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
        const float hp[3] = {g_near.palm.x, gy + 5.0f, g_near.palm.z};
        const float d = std::sqrt((p[0] - hp[0]) * (p[0] - hp[0]) + (p[1] - hp[1]) * (p[1] - hp[1]) + (p[2] - hp[2]) * (p[2] - hp[2]));
        static double last = -10;
        static std::vector<void*> seen_from;
        void* from = _ReturnAddress();
        const bool new_from = std::find(seen_from.begin(), seen_from.end(), from) == seen_from.end();
        if (d < 15.0f && (now_s() - last > 0.25 || new_from)) {
            if (new_from && seen_from.size() < 32) seen_from.push_back(from);
            // (The sprite queued at the ground hand at a cast: watch who writes its height.)
            static bool watched = false;
            if (!watched && from == reinterpret_cast<void*>(0x82F2A8) && now_s() - g_spell_watch.t < 0.5) {
                watched = true;
                stereo::watch_writes(reinterpret_cast<uintptr_t>(sprite) + 4);
            }
            last = now_s();
            BWLOG("hands: sprite %p at (%.1f, %.1f, %.1f), %.1f from the ground hand, at %.2f s, drawn by %p", sprite, p[0], p[1], p[2], d,
                  now_s() - g_spell_watch.t, _ReturnAddress());
        }
    }
    // (A gesture's shape on the panel above the left hand: placed again with the camera drawing it,
    // and drawn at the size it has there.)
    // (Its corners laid on the panel as drawn: ui::gesture_sprite_begin. Otherwise placed in the world.)
    float saved_pos[3];
    const bool laid = ui::gesture_sprite_begin(sprite, saved_pos);
    if (!laid) ui::gesture_sprite_drawing(sprite);
    // (Any other sprite: stood upright in the world, not the rolled screen.)
    const bool turned = !laid && ui::sprite_upright_begin(static_cast<const float*>(sprite));
    const float k = ui::gesture_sprite_scale(sprite);
    if (k != 1.0f && !IsBadWritePtr(static_cast<char*>(sprite) + 0xC, 4)) {
        float& size = *reinterpret_cast<float*>(static_cast<char*>(sprite) + 0xC);
        const float saved = size;
        size *= k;
        o_sprite_draw(sprite, edx);
        size = saved;
    } else {
        o_sprite_draw(sprite, edx);
    }
    if (laid) ui::gesture_sprite_end(sprite, saved_pos);
    if (turned) ui::sprite_upright_end();
}
// (Debug, hands drawat_log 1: Particle3DSprite::DrawAt (0x67AE80, thiscall(DrawData&); ret 4)
// draws a particle at [data+4] +0x24: for those near the land below the palm around a cast, what
// that is.)
using DrawAtFn = void(__fastcall*)(void* sprite, void* edx, const void* data);
DrawAtFn o_draw_at = nullptr;
bool g_dbg_drawat = false;
void __fastcall draw_at_detour(void* sprite, void* edx, const void* data) {
    if (g_dbg_drawat && data && now_s() - g_spell_watch.t < 1.5 && !IsBadReadPtr(data, 8)) {
        const char* o = *reinterpret_cast<const char* const*>(static_cast<const char*>(data) + 4);
        if (o && !IsBadReadPtr(o, 0x30)) {
            const float* p = reinterpret_cast<const float*>(o + 0x24);
            const float gy = std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
            static double last = -10;
            if (std::fabs(p[0] - g_near.palm.x) < 60.0f && std::fabs(p[2] - g_near.palm.z) < 80.0f && p[1] < g_near.palm.y + 40.0f + 0.0f * gy && now_s() - last > 0.1) {
                last = now_s();
                const char* atom = *reinterpret_cast<const char* const*>(data);
                const char* doff = atom && !IsBadReadPtr(atom + 0x128, 4) ? *reinterpret_cast<const char* const*>(atom + 0x128) : nullptr;
                const float* d = doff && !IsBadReadPtr(doff, 0x38) ? reinterpret_cast<const float*>(doff) : nullptr;
                BWLOG("hands: drawn particle at (%.1f, %.1f, %.1f) at %.2f s: atom %p (%s) at (%.1f, %.1f, %.1f); its draw offset %p (%s): +18 %.2f +1C (%.1f, %.1f, %.1f) +2C (%.1f, %.1f, %.1f)",
                      p[0], p[1], p[2], now_s() - g_spell_watch.t, static_cast<const void*>(atom), atom ? class_name(atom) : "-",
                      atom ? reinterpret_cast<const float*>(atom + 0x80)[0] : 0.0f, atom ? reinterpret_cast<const float*>(atom + 0x80)[1] : 0.0f,
                      atom ? reinterpret_cast<const float*>(atom + 0x80)[2] : 0.0f, static_cast<const void*>(doff), doff ? class_name(doff) : "-",
                      d ? d[6] : 0.0f, d ? d[7] : 0.0f, d ? d[8] : 0.0f, d ? d[9] : 0.0f, d ? d[11] : 0.0f, d ? d[12] : 0.0f, d ? d[13] : 0.0f);
            }
        }
    }
    o_draw_at(sprite, edx, data);
}
using SparkleFn = void(__stdcall*)(void* group, void* gi);
SparkleFn o_sparkle = nullptr;
bool g_dbg_sparkle = false;
void __stdcall sparkle_detour(void* group, void* gi) {
    if (g_dbg_sparkle && group && now_s() - g_spell_watch.t < 1.2 && !IsBadReadPtr(static_cast<char*>(group) + 0xAFE0, 0x80)) {
        static double last = -10;
        static void* last_group = nullptr;
        if (now_s() - last > 0.1 || group != last_group) {
            last = now_s(), last_group = group;
            const float* f = reinterpret_cast<const float*>(static_cast<char*>(group) + 0xAFE0);
            std::string out;
            for (int i = 0; i < 0x80 / 4; ++i) {
                char buf[32];
                snprintf(buf, sizeof(buf), " %X:%.4g", 0xAFE0 + i * 4, f[i]);
                out += buf;
            }
            BWLOG("hands: sparkle group %p at %.2f s:%s", group, now_s() - g_spell_watch.t, out.c_str());
        }
    }
    o_sparkle(group, gi);
}
// Effects that go with the hand are drawn with a DrawOffsetLT: DrawOffsetLT::GetOffset (0x6C7690,
// thiscall(LHPoint* out); ret 4) gives (the game's hand, CHand +0x78, minus the reference point
// +0x1C) times +0x18, and SetRefPos (0x6C7600, thiscall(const LHPoint*, float); ret 8) sets the
// reference where the effect starts. So the hand's particles followed the hand on the land below
// the VR hand (found by hooking Particle3DSprite::DrawAt: fn_00679920 draws a particle at its
// place plus the offset). In hand mode both use the palm instead of the game's hand.
using GetOffsetFn = void(__fastcall*)(void* off, void* edx, void* out);
using SetRefPosFn = void(__fastcall*)(void* off, void* edx, const float* p, float f);
GetOffsetFn o_get_offset = nullptr;
SetRefPosFn o_set_ref_pos = nullptr;
bool g_atoms_at_palm = true;  // (debug: hands atoms_at_palm 0: the hand's effects and particles on the land)
bool g_offsets_at_palm = false;  // (debug: hands offsets_at_palm 1. Off: with the reference at the palm, a thrown effect's frozen offset dragged it
// back to the land, a particle at the palm drawn 95 units below it; the particles are moved at birth instead.)
// (Lightning held: its bolts are particles kept as offsets from such a reference, the game's hand on
// the land, so they rose from below the view (playtest 2026-11-01). While it is held, with the hand at
// the controller, the reference is the fingertip.)
bool zap_from_finger() { return g_zap && g_zap_from_finger && g_atoms_at_palm && game_hand_alive() && g_near.on && g_near.blend >= 1.0f && !g_cinema; }
Vec3 zap_tip() { return g_near.palm + g_near.z * (0.09f * g_rig_scale); }
int comfort_vignette() { return g_set.comfort_vignette; }
float teleport_fade() {  // (1 just after a teleport, gone in 0.3 s)
    const double t = now_s() - g_tp_at;
    return t < 0 || t > 0.3 ? 0.0f : float(1.0 - t / 0.3);
}
bool hand_effects_at_palm() { return (g_offsets_at_palm && g_atoms_at_palm && game_hand_alive() && g_near.blend >= 0.5f && !g_cinema) || zap_from_finger(); }
Vec3 hand_effects_point() { return zap_from_finger() ? zap_tip() : g_near.palm; }
void __fastcall get_offset_detour(void* off, void* edx, void* out) {
    if (!hand_effects_at_palm()) return o_get_offset(off, edx, out);
    float* hp = reinterpret_cast<float*>(static_cast<char*>(g_game_hand) + 0x78);
    float saved[3];
    memcpy(saved, hp, sizeof(saved));
    const Vec3 at = hand_effects_point();
    hp[0] = at.x, hp[1] = at.y, hp[2] = at.z;
    o_get_offset(off, edx, out);
    memcpy(hp, saved, sizeof(saved));
}
void __fastcall set_ref_pos_detour(void* off, void* edx, const float* p, float f) {
    if (hand_effects_at_palm() && p) {
        const float* hp = reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78);
        if (std::fabs(p[0] - hp[0]) < 2.0f && std::fabs(p[2] - hp[2]) < 2.0f && std::fabs(p[1] - hp[1]) < 10.0f) {
            const Vec3 at = hand_effects_point();
            const float q[3] = {at.x, at.y + (p[1] - hp[1]), at.z};
            return o_set_ref_pos(off, edx, q, f);
        }
    }
    o_set_ref_pos(off, edx, p, f);
}
// Every new particle: AtomCollection::CommonInitNewAtom (0x674C60, thiscall(AtomCore*); ret 4). One
// born at the game's hand on the land (within 2 units across of CHand +0x78, 10 up or down) in
// hand mode is moved up to the palm, its position (+0x80) and the two copies kept with it (+0xBC,
// +0xF4) together: the fireball seen flying was such a particle, let out at the land below the VR
// hand (player, 2026-10-26). The first of each kind are logged with their collection.
using InitAtomFn = void(__fastcall*)(void* coll, void* edx, void* atom);
InitAtomFn o_init_atom = nullptr;
int g_zap_atom_log = 0;  // debug (hands zap_atoms N): the next N particles born while lightning is held
void __fastcall init_atom_detour(void* coll, void* edx, void* atom) {
    o_init_atom(coll, edx, atom);
    if (g_dbg_atoms && atom && !IsBadReadPtr(static_cast<char*>(atom) + 0x80, 12)) {  // (debug, hands atoms 1: every new particle)
        const float* p = reinterpret_cast<const float*>(static_cast<char*>(atom) + 0x80);
        BWLOG("hands: new particle (%.1f, %.1f, %.1f) in %p %s from %p; the palm (%.1f, %.1f, %.1f)", p[0], p[1], p[2], coll, class_name(coll),
              _ReturnAddress(), g_near.palm.x, g_near.palm.y, g_near.palm.z);
    }
    if (g_zap && g_zap_atom_log > 0 && atom && g_game_hand && !IsBadReadPtr(static_cast<char*>(atom) + 0x80, 12)) {
        --g_zap_atom_log;
        const float* p = reinterpret_cast<const float*>(static_cast<char*>(atom) + 0x80);
        const float* hp = reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78);
        BWLOG("hands: zap particle (%.1f, %.1f, %.1f) in %p %s from %p; the game's hand (%.1f, %.1f, %.1f), the palm (%.1f, %.1f, %.1f)", p[0], p[1], p[2],
              coll, class_name(coll), _ReturnAddress(), hp[0], hp[1], hp[2], g_near.palm.x, g_near.palm.y, g_near.palm.z);
    }
    if (!g_atoms_at_palm || !atom || !game_hand_alive() || g_near.blend < 0.5f || g_cinema || IsBadWritePtr(static_cast<char*>(atom) + 0x80, 0x80)) return;
    float* p = reinterpret_cast<float*>(static_cast<char*>(atom) + 0x80);
    const float* hp = reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78);
    if (!(std::fabs(p[0] - hp[0]) < 2.0f && std::fabs(p[2] - hp[2]) < 2.0f && std::fabs(p[1] - hp[1]) < 10.0f)) return;
    const float d[3] = {g_near.palm.x - hp[0], g_near.palm.y - hp[1], g_near.palm.z - hp[2]};
    for (int off : {0x80, 0xBC, 0xF4}) {
        float* q = reinterpret_cast<float*>(static_cast<char*>(atom) + off);
        q[0] += d[0], q[1] += d[1], q[2] += d[2];
    }
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        BWLOG("hands: a particle born at the game's hand moved up to the palm (collection %p %s, from %p)", coll, class_name(coll), _ReturnAddress());
    }
}
// (The hand's packet comes only when the hand moves: a fireball let go of from a still hand took
// the point from before. So while a miracle is charged, the points are the palm every frame too.)
void spell_points_at_palm() {
    void* mine = game_hand_alive() ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850)
                                                                                         : nullptr;
    if (!g_spell_at_palm || !mine || g_near.blend < 0.5f || g_cinema || IsBadWritePtr(static_cast<char*>(mine) + 0x5C, 0x88)) return;
    if (game_hand_state() != 8 && now_s() - g_spell_watch.t > 1.6) return;
    // (Lightning held with the trigger: from the fingertip, the hand pointing; playtest 2026-11-01:
    // the hand flew out as the laser and the bolts came from there.)
    const Vec3 at = g_zap ? g_near.palm + g_near.z * (0.09f * g_rig_scale) : g_near.palm;
    for (int off : {0x5C, 0xA4, 0xC8, 0xD4}) {
        float* q = reinterpret_cast<float*>(static_cast<char*>(mine) + off);
        q[0] = at.x, q[1] = at.y, q[2] = at.z;
    }
    // (The lightning's spell keeps where its bolts start at +0x70, from when it was made: the palm.)
    if (g_zap && g_spell_watch.spell && !IsBadWritePtr(static_cast<char*>(g_spell_watch.spell) + 0x70, 12)) {
        float* start = reinterpret_cast<float*>(static_cast<char*>(g_spell_watch.spell) + 0x70);
        start[0] = at.x, start[1] = at.y, start[2] = at.z;
    }
}
using SpellMakeFn = int(__fastcall*)(void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
SpellMakeFn o_spell_make = nullptr;
int __fastcall spell_make_detour(void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    const int r = o_spell_make(self, edx, a, b, c, d, e);
    char at[128] = "-";
    if (a > 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(a), 12)) {
        const float* q = reinterpret_cast<const float*>(a);
        const int32_t* qi = reinterpret_cast<const int32_t*>(a);
        snprintf(at, sizeof(at), "floats (%g, %g, %g) ints (%d, %d, %d)", q[0], q[1], q[2], qi[0], qi[1], qi[2]);
    }
    void* spell = d > 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(d), 4) ? *reinterpret_cast<void**>(d) : nullptr;
    const float* hp = g_game_hand ? reinterpret_cast<const float*>(static_cast<char*>(g_game_hand) + 0x78) : nullptr;
    bool moved = false;
    if (spell && strcmp(class_name(spell), ".?AVSpell@@") == 0) {
        g_spell_watch = {spell, now_s(), -10};
        atom_track_start();
        if (g_watch_spell_start) stereo::watch_writes(reinterpret_cast<uintptr_t>(spell) + 0x74);
    }
    if (g_spell_at_palm && spell && hp && g_near.blend >= 0.5f && !g_cinema && !IsBadWritePtr(static_cast<char*>(spell) + 0x70, 12)) {
        float* start = reinterpret_cast<float*>(static_cast<char*>(spell) + 0x70);
        // (Near the game's hand, not on it: it moves during a swing; 2 units off in the player's throw.)
        if (std::fabs(start[0] - hp[0]) < 10.0f && std::fabs(start[1] - hp[1]) < 10.0f && std::fabs(start[2] - hp[2]) < 10.0f) {
            start[0] = g_near.palm.x, start[1] = g_near.palm.y, start[2] = g_near.palm.z;
            moved = true;
        }
    }
    std::string fields;
    if (spell && !IsBadReadPtr(spell, 0x200)) {
        const float* f = static_cast<const float*>(spell);
        for (int i = 1; i + 2 < 0x200 / 4; ++i)
            if (std::fabs(f[i] - g_near.palm.x) < 300.0f && std::fabs(f[i + 2] - g_near.palm.z) < 300.0f && std::fabs(f[i + 1]) < 2000.0f) {
                char buf[64];
                snprintf(buf, sizeof(buf), " +%X (%.1f, %.1f, %.1f)", i * 4, f[i], f[i + 1], f[i + 2]);
                fields += buf;
            }
    }
    BWLOG("hands: spell made by %p -> %d%s: at %s; spell %p %s; points in it:%s; the game's hand (%.1f, %.1f, %.1f), the palm (%.1f, %.1f, %.1f)", self,
          r, moved ? " (starts at the palm)" : "", at, spell, spell ? class_name(spell) : "-", fields.c_str(), hp ? hp[0] : 0.0f, hp ? hp[1] : 0.0f, hp ? hp[2] : 0.0f, g_near.palm.x,
          g_near.palm.y, g_near.palm.z);
    return r;
}

void __fastcall vinfl_draw_detour(void* vi, void* edx) {
    Vec3 p;
    if (!vinfl_point(vi, p)) return o_vinfl_draw(vi, edx);
    auto& hp = *reinterpret_cast<game::LHPoint*>(static_cast<char*>(g_game_hand) + 0x78);
    const game::LHPoint saved = hp;
    hp.x = p.x, hp.z = p.z;
    if (g_near.blend >= 0.5f) g_mana_path_end = &p;
    o_vinfl_draw(vi, edx);
    g_mana_path_end = nullptr;
    hp = saved;
}

// Debug (hands fight_log N): the creature fight's interface calls (Creature: select / apply a fight
// move to a map spot, fight this to an object; LH3DCreature::FightClickGround).
int g_fight_log = 0;
using FightMapFn = unsigned(__fastcall*)(void* c, void* edx, void* status, const void* coords);
FightMapFn o_fight_select = nullptr, o_fight_apply = nullptr;
using FightObjFn = unsigned(__fastcall*)(void* c, void* edx, void* status, void* object);
FightObjFn o_fight_obj = nullptr;
unsigned __fastcall fight_select_detour(void* c, void* edx, void* status, const void* coords) {
    const unsigned r = o_fight_select(c, edx, status, coords);
    if (g_fight_log > 0) --g_fight_log, BWLOG("hands: fight: select at (%.1f, %.1f) -> %u (lmb %d rmb %d)", static_cast<const int32_t*>(coords)[0] / 6553.6f,
                                             static_cast<const int32_t*>(coords)[1] / 6553.6f, r, g_lmb, g_rmb);
    return r;
}
unsigned __fastcall fight_apply_detour(void* c, void* edx, void* status, const void* coords) {
    const unsigned r = o_fight_apply(c, edx, status, coords);
    if (g_fight_log > 0) --g_fight_log, BWLOG("hands: fight: apply at (%.1f, %.1f) -> %u (lmb %d rmb %d)", static_cast<const int32_t*>(coords)[0] / 6553.6f,
                                             static_cast<const int32_t*>(coords)[1] / 6553.6f, r, g_lmb, g_rmb);
    return r;
}
unsigned __fastcall fight_obj_detour(void* c, void* edx, void* status, void* object) {
    const unsigned r = o_fight_obj(c, edx, status, object);
    if (g_fight_log > 0) --g_fight_log, BWLOG("hands: fight: to object %s -> %u (lmb %d rmb %d)", object ? class_name(object) : "-", r, g_lmb, g_rmb);
    return r;
}
using FightActionFn = void(__fastcall*)(void* gi, void* edx);
FightActionFn o_fight_action = nullptr;
void __fastcall fight_action_detour(void* gi, void* edx) {
    if (g_fight_log > 0) {
        const uint8_t flags = static_cast<const uint8_t*>(gi)[0x39];
        void* picked = *reinterpret_cast<void* const*>(static_cast<const char*>(gi) + 0x400);
        static uint8_t last = 0xFF;
        if (flags != last) {
            --g_fight_log;
            void* game = game::game_instance();
            const char* cam = game ? reinterpret_cast<const char*(__thiscall*)(void*)>(0x54C180)(game) : nullptr;
            const int top = cam ? *reinterpret_cast<const int*>(cam + 0x58) : -1;
            const char* mode = cam && top >= 0 ? *reinterpret_cast<const char* const*>(cam + 0x28 + top * 4) : nullptr;
            const int fighting = creature_fighting(gi) ? 1 : 0;
            {
                const char* g = static_cast<const char*>(gi);
                const int32_t* mc = reinterpret_cast<const int32_t*>(g + 0x3F0);
                void* creature = reinterpret_cast<void*(__thiscall*)(void*)>(0x5D2C20)(gi);
                int valid = -1;
                float dist = -1, radius = -1;
                int c4b30 = -1;
                if (creature) {
                    void* status = *reinterpret_cast<void* const*>(g + 0x39C);
                    if (const auto fn = reinterpret_cast<int(__thiscall*)(void*, void*, const void*)>(game_method(creature, 0x75C)))
                        valid = fn(creature, status, mc);
                    const char* c3d = reinterpret_cast<const char*(__thiscall*)(void*)>(0x477850)(creature);
                    if (c3d) {
                        const float x = mc[0] / 6553.6f, z = mc[1] / 6553.6f;
                        const float dx = x - *reinterpret_cast<const float*>(c3d + 0x4A98), dz = z - *reinterpret_cast<const float*>(c3d + 0x4AA0);
                        dist = std::sqrt(dx * dx + dz * dz), radius = *reinterpret_cast<const float*>(c3d + 0x4AA4);
                        c4b30 = *reinterpret_cast<const int*>(c3d + 0x4B30);
                    }
                }
                BWLOG("hands: fight select check: +0x444 %d, spot (%.1f, %.1f), creature %p, valid %d, %.1f from the arena centre (radius %.1f), +0x4B30 %d",
                      *reinterpret_cast<const int*>(g + 0x444), mc[0] / 6553.6f, mc[1] / 6553.6f, creature, valid, dist, radius, c4b30);
            }
            BWLOG("hands: fight action: flags %02X picked %s, lmb %d rmb %d; camera mode %s +0x44 %p, fighting %d", flags,
                  picked ? class_name(picked) : "-", g_lmb, g_rmb, mode ? class_name(mode) : "-",
                  mode ? *reinterpret_cast<void* const*>(mode + 0x44) : nullptr, fighting);
            last = flags;
        }
    }
    o_fight_action(gi, edx);
}
// --- Scaffolds (player, 2026-10-27). Held over the land, a scaffold shows the building it would
// make (its blueprint) on the land below the game's hand, slowly turning; let go, the game builds
// it there. Dropped onto another scaffold, the two combine.
//  - Scaffold +0x80: the blueprint's angle (radians). Scaffold::ProcessInHand (0x6E8570, each game
//    turn) adds the scaffold info's turning speed (+0x28 -> +0x114) unless +0x88 has 0x200 (which
//    also keeps a planned abode's angle off it); Scaffold::DrawInHand (0x5181A0) draws it at the
//    angle plus the part of a turn gone by ((GGame +0x25053C) % 100 x 0.01 x the speed).
//  - fn_006EA950(0) = 1: it has a building to make (a planned abode at +0x6C; the "Build" hint).
//  - The building is made as the dropped scaffold comes to rest (Scaffold::EndPhysics ->
//    Scaffold::BuildBuilding), if the game's hand dropped it rather than threw it. Launched from the
//    palm (as the mod does for anything let go of in the air) it fell, rolled and built off the
//    blueprint, or not at all.
//  - Combining: as the hand lets go the game applies what it holds to the object under its cursor
//    (fn_005DA1A0: the held thing's ValidToApplyThisToObject, vtable +0x71C, then
//    ApplyThisToObject -> Scaffold::Combine). A hand that carries keeps the cursor on the ground
//    below it, so the other scaffold had to be right under the palm.
const void* const kScaffoldVtable = reinterpret_cast<const void*>(0x8E552C);
bool is_scaffold(const void* o) { return o && !IsBadReadPtr(o, 4) && *static_cast<const void* const*>(o) == kScaffoldVtable; }
void* game_status() {
    void* gi = game_interface();
    return gi && !IsBadReadPtr(static_cast<char*>(gi) + 0x39C, 4) ? *reinterpret_cast<void**>(static_cast<char*>(gi) + 0x39C) : nullptr;
}
void* object_in_hand() {  // (GInterfaceStatus::GetFirstObjectInCurrentHand)
    void* status = game_status();
    return status ? reinterpret_cast<void*(__thiscall*)(void*)>(0x5DC130)(status) : nullptr;
}
bool blueprint_valid(void* scaffold) {
    return is_scaffold(scaffold) && reinterpret_cast<int(__thiscall*)(void*, int)>(0x6EA950)(scaffold, 0) == 1;
}
// An object still alive: its vtable is one of the game's (in the exe's read-only data). A freed
// object's memory often stays readable, and its old vtable pointer is overwritten.
bool live_object(const void* o) {
    if (!o || IsBadReadPtr(o, 8)) return false;
    const auto vt = *static_cast<const uintptr_t*>(o);
    return vt >= 0x8A0000 && vt < 0xA00000;
}
bool applies_to(void* held, void* target) {  // would the game apply the held thing to `target`, dropped onto it?
    // (Both checked alive: a pile grabbed from was merged away, and the game's check called its freed
    // vtable: a crash, 2026-10-30.)
    if (!live_object(held) || !live_object(target)) return false;
    void* status = game_status();
    using ValidFn = int(__thiscall*)(void*, void*, void*);
    const auto fn = reinterpret_cast<ValidFn>(game_method(held, 0x71C));
    return status && fn && fn(held, status, target) == 1;
}

// The blueprint turns with the wrist: the twist about the controller's axis since the scaffold was
// taken, times bp_twist_gain, added to the angle it had then. The game's own turning is held off.
struct Blueprint {
    void* scaffold = nullptr;
    float start = 0;    // the game's angle as the hand took over
    float twist = 0;    // the hand's turn about the vertical since (radians, + to the right, seen from above)
    double seen = -10;  // last game turn it was in the hand
};
Blueprint g_bp;
double g_twist_at = -10;  // the controller's twist was last measured then
const void* g_glow_obj3d = nullptr;  // what the hand would act on, lit up (its 3D object), as of g_glow_at
double g_glow_hit_at = -10;           // (its draws were lit up then)
bool g_glow_cool = false;             // (pale blue: the creature under the teleport's aim)
const void* g_glow_object = nullptr;  // (... the thing itself)
float g_glow_amount = 0;
double g_glow_at = -10;
float g_follow_above = -1;  // the head's height above the land kept while the stick moves it (-1: not moving)
float g_bp_sign = -1.0f;  // which way the blueprint turns for the hand's turn (hands bp_sign): the game's angle
                          // goes round to the left seen from above (+90 degrees took its x axis from +X to +Z;
                          // harness 2026-11-13), the hand's turn is + to the right.
int g_dbg_bp_log = 0;     // debug: log the blueprint's angle and drawn axes (hands bp_log N)
bool bp_steered(const void* s) {
    return g_set.bp_twist_gain > 0 && s && s == g_bp.scaffold && now_s() - g_bp.seen < 0.5 && now_s() - g_twist_at < 0.5;
}
float bp_angle() {
    const float two_pi = 6.2831853f;
    float a = std::fmod(g_bp.start + g_bp_sign * g_set.bp_twist_gain * g_bp.twist, two_pi);
    return a < 0 ? a + two_pi : a;
}
using ScaffoldProcessFn = unsigned(__fastcall*)(void* scaffold, void* edx);
ScaffoldProcessFn o_scaffold_process = nullptr;
unsigned __fastcall scaffold_process_detour(void* s, void* edx) {
    const double t = now_s();
    if (g_set.bp_twist_gain <= 0 || t - g_twist_at > 0.5 || IsBadWritePtr(static_cast<char*>(s) + 0x80, 0xC))
        return o_scaffold_process(s, edx);
    float& angle = *reinterpret_cast<float*>(static_cast<char*>(s) + 0x80);
    uint16_t& flags = *reinterpret_cast<uint16_t*>(static_cast<char*>(s) + 0x88);
    if (s != g_bp.scaffold || t - g_bp.seen > 0.5) {
        g_bp = {s, angle, 0, t};
        BWLOG("hands: a scaffold in the hand: its blueprint turns with the wrist (from %.2f rad, x %.1f)", angle, g_set.bp_twist_gain);
    }
    g_bp.seen = t;
    const uint16_t keep = flags & 0x200;
    flags |= 0x200;
    angle = bp_angle();
    const unsigned r = o_scaffold_process(s, edx);
    flags = uint16_t((flags & ~0x200) | keep);
    angle = bp_angle();
    return r;
}
using ScaffoldDrawFn = void(__fastcall*)(void* scaffold, void* edx, void* status);
ScaffoldDrawFn o_scaffold_draw = nullptr;
void __fastcall scaffold_draw_detour(void* s, void* edx, void* status) {
    if (!bp_steered(s)) return o_scaffold_draw(s, edx, status);
    char* p = static_cast<char*>(s);
    float& angle = *reinterpret_cast<float*>(p + 0x80);
    // (The game adds the part of a turn gone by at its turning speed: taken off again.)
    const char* info = *reinterpret_cast<const char* const*>(p + 0x28);
    const auto* game = static_cast<const char*>(game::game_instance());
    float lead = 0;
    if (info && game && !IsBadReadPtr(info + 0x114, 4))
        lead = float(*reinterpret_cast<const uint32_t*>(game + 0x25053C) % 100) * 0.01f * *reinterpret_cast<const float*>(info + 0x114);
    angle = bp_angle() - lead;
    o_scaffold_draw(s, edx, status);
    angle = bp_angle();
    if (g_dbg_bp_log > 0) {
        static double last = 0;
        const char* ph = *reinterpret_cast<const char* const*>(p + 0x74);
        if (now_s() - last > 0.5 && ph && !IsBadReadPtr(ph + 0x14, 0x30)) {
            last = now_s();
            --g_dbg_bp_log;
            const float* m = reinterpret_cast<const float*>(ph + 0x14);
            BWLOG("hands: blueprint %s at angle %.2f (twist %.2f): its axes x (%.2f, %.2f, %.2f) z (%.2f, %.2f, %.2f), at (%.1f, %.1f, %.1f)",
                  blueprint_valid(s) ? "valid" : "none", angle, g_bp.twist, m[0], m[1], m[2], m[6], m[7], m[8], m[9], m[10], m[11]);
        }
    }
}
// (Logged: what the game does with scaffolds.)
using ScaffoldCombineFn = void(__fastcall*)(void* scaffold, void* edx, void* other);
ScaffoldCombineFn o_scaffold_combine = nullptr;
void __fastcall scaffold_combine_detour(void* s, void* edx, void* other) {
    BWLOG("hands: scaffolds combine (Scaffold::Combine %p with %p)", s, other);
    o_scaffold_combine(s, edx, other);
}
using ScaffoldBuildFn = int(__fastcall*)(void* scaffold, void* edx, void* player, int force);
ScaffoldBuildFn o_scaffold_build = nullptr;
int __fastcall scaffold_build_detour(void* s, void* edx, void* player, int force) {
    const int r = o_scaffold_build(s, edx, player, force);
    const int32_t* at = reinterpret_cast<const int32_t*>(static_cast<char*>(s) + 0x14);  // (MapCoords: units of 10/65536)
    BWLOG("hands: a scaffold builds (Scaffold::BuildBuilding: %d, force %d) at map (%.1f, %.1f), angle %.2f", r, force,
          at[0] * (10.0f / 65536.0f), at[1] * (10.0f / 65536.0f), *reinterpret_cast<const float*>(static_cast<char*>(s) + 0x80));
    return r;
}

// --- Gestures (debug, 2026-10-27: the miracle tutorial's gesture couldn't be drawn in VR).
// The game keeps the land points under the cursor in a ring of 80 samples (GGame +0x25006C:
// count byte +0xC88, next +0xC90; sample 0x28 bytes from +8: screen x, y, then the land point at
// +0x14), fed through fn_005CEAD0 (GInterface, the screen point) -> fn_0057BBC0. Each turn
// GInterface::ProcessPowerUpSystem (0x5CF300) empties it while the camera moves (unless the camera
// mode lets the player gesture then) and matches it against Gestures.jty (fn_005CF220 per action).
int gesture_samples() {
    const auto* game = static_cast<const char*>(game::game_instance());
    const char* buf = game ? *reinterpret_cast<const char* const*>(game + 0x25006C) : nullptr;
    return buf && !IsBadReadPtr(buf + 0xC88, 1) ? *reinterpret_cast<const uint8_t*>(buf + 0xC88) : -1;
}
using GestureFeedFn = void(__fastcall*)(void* gi, void* edx, const int32_t* screen);
GestureFeedFn o_gesture_feed = nullptr;
// The ring holds 80 samples, one per frame: in VR the game runs at 120 frames a second, so it held
// the last 0.7 s of a gesture, and a spiral drawn at a natural pace never fitted (the flat game
// runs much slower). In VR a sample goes in at most gesture_rate_hz times a second (2 s of drawing).
float g_gesture_rate_hz = 25.0f;  // (hands gesture_rate; 0: every frame)
bool g_gesture_anywhere = true;   // (hands gesture_anywhere 0: the game finds the land under the cursor itself, as before)
void __fastcall gesture_feed_detour(void* gi, void* edx, const int32_t* screen) {
    static double last_fed = -10;
    const double t = now_s();
    if (t < g_dbg_gesture_until) {  // (debug: is the game asking at all, and is the pen down?)
        static double counted_at = 0;
        static int calls = 0;
        ++calls;
        if (t - counted_at > 1.0) {
            BWLOG("gesture: %d mouse-move samples offered in %.1f s (pen %s, from %p)", calls, t - counted_at, g_gd.on ? "down" : "up", _ReturnAddress());
            counted_at = t, calls = 0;
        }
    }
    if (g_set.gesture_b && t - g_twist_at < 0.5 && !g_gd.on && !g_shield_draw && t >= g_shield_tail_until) return;  // (in VR only what is drawn with B held)
    if (g_gesture_rate_hz > 0 && t - g_twist_at < 0.5 && t - last_fed < 1.0 / g_gesture_rate_hz) return;
    last_fed = t;
    const int before = gesture_samples();
    // The shield's circle: each sample's point on the land is the laser's. The game finds it from
    // the cursor's place on the screen through its own camera (fn_005E5620), which isn't what the
    // eyes see in VR, and the shield went up well off the circle drawn, toward the player (player,
    // 2026-10-28; the circle's centre and size come from these points). The rest as fn_005CEAD0.
    if ((g_shield_draw || t < g_shield_tail_until) && t - g_twist_at < 0.5 && screen) {
        const auto* game = static_cast<const char*>(game::game_instance());
        if (!game || (game[0x14] & 4) || *reinterpret_cast<const float*>(static_cast<char*>(gi) + 0x378) > 0.0f) return;
        void* ring = *reinterpret_cast<void* const*>(game + 0x25006C);
        game::LHPoint world{g_shield_pen.x, g_shield_pen.y, g_shield_pen.z};
        if (ring) reinterpret_cast<void(__thiscall*)(void*, game::LHPoint*, const int32_t*)>(0x57BBC0)(ring, &world, screen);
        return;
    }
    // Drawing with B: each sample is put in by the mod, its land point the ground under the pen. The
    // game takes the land under the cursor through its camera (the head), and over the sky there is
    // none: a gesture drawn in front of the face, looking up or out over the sea, was never taken
    // (player, 2026-11-03: it shouldn't matter where the player looks).
    if (g_gd.on && g_gesture_anywhere && t - g_twist_at < 0.5 && screen && !g_gd.trail.empty()) {
        const auto* game = static_cast<const char*>(game::game_instance());
        if (!game || (game[0x14] & 4) || *reinterpret_cast<const float*>(static_cast<char*>(gi) + 0x378) > 0.0f) return;
        void* ring = *reinterpret_cast<void* const*>(game + 0x25006C);
        const Vec3 pen = g_gd.trail.back().second;
        game::LHPoint world{pen.x, std::max(0.0f, ground_height(pen.x, pen.z)), pen.z};
        if (ring) reinterpret_cast<void(__thiscall*)(void*, game::LHPoint*, const int32_t*)>(0x57BBC0)(ring, &world, screen);
    } else {
        o_gesture_feed(gi, edx, screen);
    }
    if (now_s() < g_dbg_gesture_until) {
        static double last = 0;
        if (now_s() - last > 0.1) {
            last = now_s();
            BWLOG("gesture: fed (%d, %d) from %p: samples %d -> %d", screen ? screen[0] : -1, screen ? screen[1] : -1, _ReturnAddress(), before, gesture_samples());
        }
    }
}
// fn_005CF2D0 (only asked there): may the player gesture while the camera moves (the camera mode's
// CanPlayerGestureWhenCameraMoving)? In VR the game's camera follows the head and is always moving,
// so the samples were emptied every turn and no gesture could ever be drawn: yes, while the VR
// controller is in use.
using GestureMovingFn = int(__fastcall*)(void* gi, void* edx);
GestureMovingFn o_gesture_moving = nullptr;
int __fastcall gesture_moving_detour(void* gi, void* edx) {
    if (now_s() - g_twist_at < 0.5) return 1;
    return o_gesture_moving(gi, edx);
}
// The shield's circle: the game places it, and its "ready" ring, at the middle of the circle on
// the screen, found on the land through its own camera (fn_0057A5E0, the recogniser ->
// fn_005E5620), well off the circle drawn in VR (toward the player; 2026-10-28). The recogniser's
// answer (its last argument: the centre at +8) is set to the middle of the circle the laser drew on
// the land (a circle fitted to its last loop's points), before the game copies it anywhere
// (GInterface +0x180).
using RecogniseFn = int(__fastcall*)(void* sys, void* edx, void* ring, void* a2, void* a3, char* out);
RecogniseFn o_recognise = nullptr;
// The orange "ready" ring shown on the land as the circle is recognised is placed by other game code
// (UR_GesturingRecognised, from fn_00689F20): the gesture's template laid over the samples' box on
// the game's own screen and cast onto the land through its camera, so at the recogniser's own
// centre: off the circle drawn, toward the player (player, 2026-10-28). Each of its points is moved
// as the shield's centre is (the recogniser's -> the drawn circle's), on the land there; its size is
// the game's (the shield's).
struct RingMove {
    Vec3 from, to;
    double at = -10;
};
RingMove g_ring_move;
// A circle fitted to pts[first, n) on the land (x, z; least squares, Kasa's method), then fitted
// again without the points off it, the cut tightening from half the radius to a fifth (a sweep in
// from elsewhere pulls the first fit toward it). Leaves cx, cz as they are (the
// caller's fallback) if the points don't make a circle 2-200 units across.
bool circle_fit(const std::vector<Vec3>& pts, size_t first, size_t n, float& cx, float& cz, float& radius, size_t& kept) {
    std::vector<char> use(n - first, 1);
    double ux = 0, uz = 0, rr = 0;
    bool ok = false;
    static const double kCut[] = {0.5, 0.33, 0.25, 0.2, 0.2};
    for (int pass = 0; pass < 6; ++pass) {
        double mx = 0, mz = 0;
        size_t m = 0;
        for (size_t i = first; i < n; ++i)
            if (use[i - first]) mx += pts[i].x, mz += pts[i].z, ++m;
        if (m < 8) break;
        mx /= double(m), mz /= double(m);
        double suu = 0, svv = 0, suv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
        for (size_t i = first; i < n; ++i) {
            if (!use[i - first]) continue;
            const double u = pts[i].x - mx, v = pts[i].z - mz;
            suu += u * u, svv += v * v, suv += u * v;
            suuu += u * u * u, svvv += v * v * v, suvv += u * v * v, svuu += v * u * u;
        }
        const double det = suu * svv - suv * suv;
        if (std::abs(det) < 1e-9) break;
        const double bu = 0.5 * (suuu + suvv), bv = 0.5 * (svvv + svuu);
        const double uc = (bu * svv - bv * suv) / det, vc = (bv * suu - bu * suv) / det;
        const double r2 = uc * uc + vc * vc + (suu + svv) / double(m);
        if (r2 < 1.0 || r2 > 100.0 * 100.0) break;
        ux = mx + uc, uz = mz + vc, rr = std::sqrt(r2), kept = m, ok = true;
        if (pass == 5) break;
        for (size_t i = first; i < n; ++i) {
            const double d = std::hypot(pts[i].x - ux, pts[i].z - uz);
            use[i - first] = std::abs(d - rr) < rr * kCut[pass];
        }
    }
    if (ok) cx = float(ux), cz = float(uz), radius = float(rr);
    return ok;
}
int __fastcall recognise_detour(void* sys, void* edx, void* ring, void* a2, void* a3, char* out) {
    const int r = o_recognise(sys, edx, ring, a2, a3, out);
    if ((g_shield_draw || now_s() < g_shield_tail_until) && out && g_shield_pts.size() >= 8) {
        // Only the last loop drawn: back from the newest point until the path, having gone out,
        // comes back to it. (All points since the trigger went down took in the laser's sweep over
        // to where the circle began: the centre went up to 100 units off; 2026-10-31.) No loop
        // closed: the last 3 s of points (about what the game's 80 samples hold).
        const size_t n = g_shield_pts.size();
        const Vec3& last = g_shield_pts.back();
        size_t first = n > 360 ? n - 360 : 0;
        float reach = 0;
        for (size_t i = n - 1; i-- > 0;) {
            const float d = vr::length(Vec3{g_shield_pts[i].x - last.x, 0, g_shield_pts[i].z - last.z});
            reach = std::max(reach, d);
            if (reach > 3.0f && d < 0.25f * reach) {
                first = i;
                break;
            }
        }
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, ysum = 0;
        for (size_t i = first; i < n; ++i) {
            const Vec3& w = g_shield_pts[i];
            x0 = std::min(x0, w.x), x1 = std::max(x1, w.x), z0 = std::min(z0, w.z), z1 = std::max(z1, w.z), ysum += w.y;
        }
        const float used = float(n - first);
        float* c = reinterpret_cast<float*>(out + 8);
        float nx = 0.5f * (x0 + x1), nz = 0.5f * (z0 + z1);
        // The game often recognises the circle before it closes, so the points are an arc and their
        // box's middle is off toward it. A circle fitted to them finds the middle of an arc too;
        // points far off the fitted circle (a sweep in from elsewhere) are dropped and it is fitted again.
        float fr = 0;
        size_t kept = 0;
        circle_fit(g_shield_pts, first, n, nx, nz, fr, kept);
        static double logged_at = -10;
        if (now_s() - logged_at > 0.5) logged_at = now_s(),
        BWLOG("hands: the shield's circle: the recogniser's centre (%.1f, %.1f) size %.1f -> the drawn one's (%.1f, %.1f) radius %.1f, %zu of %zu points, box %.1f x %.1f",
              c[0], c[2], *reinterpret_cast<const float*>(out + 0x14), nx, nz, fr, kept, size_t(used), x1 - x0, z1 - z0);
        g_ring_move = {{c[0], c[1], c[2]}, {nx, ysum / used, nz}, now_s()};
        c[0] = nx, c[1] = ysum / used, c[2] = nz;
    }
    return r;
}
// fn_00689F20 (cdecl: the screen box, a template point (fractions at +0 and +8), out: the land
// point): one of the ring's points.
using RingPointFn = int(__cdecl*)(const float* box, const float* frac, float* out);
RingPointFn o_ring_point = nullptr;
int __cdecl ring_point_detour(const float* box, const float* frac, float* out) {
    const int r = o_ring_point(box, frac, out);
    const RingMove& m = g_ring_move;
    if (out && (g_shield_draw || now_s() < g_shield_tail_until) && now_s() - m.at < 1.0) {
        out[0] += m.to.x - m.from.x, out[2] += m.to.z - m.from.z;
        out[1] = ground_height(out[0], out[2]);
        static double logged_at = -10;
        if (now_s() - logged_at > 0.5)
            logged_at = now_s(), BWLOG("hands: the shield's ready ring moved onto the circle drawn: by (%.1f, %.1f), a point now at (%.1f, %.1f)",
                                       m.to.x - m.from.x, m.to.z - m.from.z, out[0], out[2]);
    }
    return r;
}
using PowerUpFn = void(__fastcall*)(void* gi, void* edx);
PowerUpFn o_power_up = nullptr;
void __fastcall power_up_detour(void* gi, void* edx) {
    const int before = gesture_samples();
    o_power_up(gi, edx);
    const int after = gesture_samples();
    if (now_s() < g_dbg_gesture_until && before > 0 && after < before)
        BWLOG("gesture: the samples were emptied by the turn's processing (%d -> %d)", before, after);
    // (Logged always: Miracle selection (GInterface +0x364, opened by the spiral) opening and closing.)
    static uint8_t selecting = 0;
    const uint8_t now_selecting = gi && !IsBadReadPtr(static_cast<char*>(gi) + 0x364, 1) ? *(static_cast<uint8_t*>(gi) + 0x364) : 0;
    if (now_selecting != selecting) BWLOG("gesture: Miracle selection %s", now_selecting ? "open" : "closed");
    selecting = now_selecting;
}
// The village totem (TotemStatue, +0x88: its level, 0 to 1). The flat game raises it by chasing the
// cursor: each turn HandStateTotem::Update (0x5B77A0) puts the game's hand at the totem's top,
// projects it to the screen and moves the level by how far the cursor is from it there
// (TotemStatue::NetworkUnfriendlyLockedSelect, 0x7386A0: level += delta x 0.1). In VR every head
// motion moved both on the screen, and the totem wobbled. Instead the level is the one it had when
// taken plus the hand's rise over totem_range_m.
struct TotemSteer {
    int grab = 0;     // counts the takes (the cursor code starts a new one)
    float rise_m = 0; // how far the hand has risen since (metres)
    double t = -10;   // last set (the hand is on it)
};
TotemSteer g_totem;
float g_totem_sign = 1.0f;  // which way the level goes for a rising hand (hands totem_sign)
using TotemSelectFn = unsigned(__fastcall*)(void* totem, void* edx, void* info);
TotemSelectFn o_totem_select = nullptr;
unsigned __fastcall totem_select_detour(void* totem, void* edx, void* info) {
    if (now_s() - g_totem.t > 0.3 || g_set.totem_range_m <= 0) return o_totem_select(totem, edx, info);
    return 1;  // (the level is the hand's: steer_totem)
}
// Each frame while the hand holds the totem: its level from the hand. (The game calls the locked
// select only while the cursor moves on the screen; the totem held is GInterface +0x3C8.)
void steer_totem() {
    static int grab = -1;
    static float level0 = 0;
    char* gi = static_cast<char*>(game_interface());
    char* totem = gi && !IsBadReadPtr(gi + 0x3C8, 4) ? *reinterpret_cast<char**>(gi + 0x3C8) : nullptr;
    if (!totem || IsBadWritePtr(totem + 0x88, 4) || *reinterpret_cast<void**>(totem) != reinterpret_cast<void*>(0x8E029C)) return;
    float& level = *reinterpret_cast<float*>(totem + 0x88);
    if (grab != g_totem.grab) grab = g_totem.grab, level0 = level;
    level = std::clamp(level0 + g_totem_sign * g_totem.rise_m / g_set.totem_range_m, 0.0f, 1.0f);
}

bool install_hooks() {
    if (MH_CreateHook(reinterpret_cast<void*>(0x7386A0), reinterpret_cast<void*>(&totem_select_detour), reinterpret_cast<void**>(&o_totem_select)) != MH_OK)
        BWLOG("hands: could not hook TotemStatue's locked select (the totem follows the cursor)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5CEAD0), reinterpret_cast<void*>(&gesture_feed_detour), reinterpret_cast<void**>(&o_gesture_feed)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x5CF300), reinterpret_cast<void*>(&power_up_detour), reinterpret_cast<void**>(&o_power_up)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x57A5E0), reinterpret_cast<void*>(&recognise_detour), reinterpret_cast<void**>(&o_recognise)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x689F20), reinterpret_cast<void*>(&ring_point_detour), reinterpret_cast<void**>(&o_ring_point)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x5CF2D0), reinterpret_cast<void*>(&gesture_moving_detour), reinterpret_cast<void**>(&o_gesture_moving)) != MH_OK)
        BWLOG("hands: could not hook the gesture samples (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6E8570), reinterpret_cast<void*>(&scaffold_process_detour),
                      reinterpret_cast<void**>(&o_scaffold_process)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x5181A0), reinterpret_cast<void*>(&scaffold_draw_detour),
                      reinterpret_cast<void**>(&o_scaffold_draw)) != MH_OK)
        BWLOG("hands: could not hook the scaffold in the hand (its blueprint turns by itself)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6E9B60), reinterpret_cast<void*>(&scaffold_combine_detour),
                      reinterpret_cast<void**>(&o_scaffold_combine)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x6E9250), reinterpret_cast<void*>(&scaffold_build_detour),
                      reinterpret_cast<void**>(&o_scaffold_build)) != MH_OK)
        BWLOG("hands: could not hook Scaffold::Combine / BuildBuilding (log only)");
    MH_CreateHook(reinterpret_cast<void*>(0x5D4AC0), reinterpret_cast<void*>(&fight_action_detour), reinterpret_cast<void**>(&o_fight_action));
    MH_CreateHook(reinterpret_cast<void*>(0x4C6460), reinterpret_cast<void*>(&fight_select_detour), reinterpret_cast<void**>(&o_fight_select));
    MH_CreateHook(reinterpret_cast<void*>(0x4C64D0), reinterpret_cast<void*>(&fight_apply_detour), reinterpret_cast<void**>(&o_fight_apply));
    MH_CreateHook(reinterpret_cast<void*>(0x4C5FE0), reinterpret_cast<void*>(&fight_obj_detour), reinterpret_cast<void**>(&o_fight_obj));
    if (MH_CreateHook(reinterpret_cast<void*>(0x756000), reinterpret_cast<void*>(&set_disciple_detour),
                      reinterpret_cast<void**>(&o_set_disciple)) != MH_OK)
        BWLOG("hands: could not hook Villager::SetVillagerDisciple (no height limit for disciples)");
    if (MH_CreateHook(reinterpret_cast<void*>(kSendObjectDrawCollision), reinterpret_cast<void*>(&collide_detour),
                      reinterpret_cast<void**>(&o_collide)) != MH_OK)
        BWLOG("hands: could not hook GInterface::SendObjectDrawCollision (no touch grabbing)");
    if (MH_CreateHook(reinterpret_cast<void*>(kPhysicsAddObject), reinterpret_cast<void*>(&add_object_detour),
                      reinterpret_cast<void**>(&o_add_object)) != MH_OK) {
        BWLOG("hands: could not hook PhysicsObject::AddObject (throws keep the game's velocity)");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5D3930), reinterpret_cast<void*>(&send_to_thing_detour),
                      reinterpret_cast<void**>(&o_send_to_thing)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x5D3D10), reinterpret_cast<void*>(&send_to_spot_detour),
                      reinterpret_cast<void**>(&o_send_to_spot)) != MH_OK)
        BWLOG("hands: could not hook the leash's sending (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(kPhysicsRemoveObject), reinterpret_cast<void*>(&remove_object_detour),
                      reinterpret_cast<void**>(&o_remove_object)) != MH_OK)
        BWLOG("hands: could not hook PhysicsObject::RemoveObject (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(kAdjustToGroundLevel), reinterpret_cast<void*>(&adjust_detour),
                      reinterpret_cast<void**>(&o_adjust)) != MH_OK)
        BWLOG("hands: could not hook PhysOb::AdjustToGroundLevel (throws start from the ground)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5CF220), reinterpret_cast<void*>(&action_detour), reinterpret_cast<void**>(&o_action)) != MH_OK)
        BWLOG("hands: could not hook GInterface's action check (no shaking)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5D8EA0), reinterpret_cast<void*>(&leash_start_detour),
                      reinterpret_cast<void**>(&o_leash_start)) != MH_OK)
        BWLOG("hands: could not hook GInterface::GetLeashStartPoint (the leash starts at the game's hand)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x76CD40), reinterpret_cast<void*>(&vinfl_update_detour),
                      reinterpret_cast<void**>(&o_vinfl_update)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x76D000), reinterpret_cast<void*>(&vinfl_draw_detour),
                      reinterpret_cast<void**>(&o_vinfl_draw)) != MH_OK)
        BWLOG("hands: could not hook GVirtualInfluence (the warning leaving the influence uses the game camera's point)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x69E300), reinterpret_cast<void*>(&side_spin_detour), reinterpret_cast<void**>(&o_side_spin)) != MH_OK)
        BWLOG("hands: could not hook UR_SideSpin (a thrown fireball curves)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6C7960), reinterpret_cast<void*>(&decay_offset_detour), reinterpret_cast<void**>(&o_decay_offset)) != MH_OK)
        BWLOG("hands: could not hook DrawOffsetDecay::GetOffset (a thrown fireball is drawn from the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6C7690), reinterpret_cast<void*>(&get_offset_detour), reinterpret_cast<void**>(&o_get_offset)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(0x6C7600), reinterpret_cast<void*>(&set_ref_pos_detour), reinterpret_cast<void**>(&o_set_ref_pos)) != MH_OK)
        BWLOG("hands: could not hook DrawOffsetLT (the hand's effects follow the hand on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x67AE80), reinterpret_cast<void*>(&draw_at_detour), reinterpret_cast<void**>(&o_draw_at)) != MH_OK)
        BWLOG("hands: could not hook Particle3DSprite::DrawAt (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x674C60), reinterpret_cast<void*>(&init_atom_detour), reinterpret_cast<void**>(&o_init_atom)) != MH_OK)
        BWLOG("hands: could not hook AtomCollection::CommonInitNewAtom (particles born at the hand stay on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x840530), reinterpret_cast<void*>(&sprite_draw_detour), reinterpret_cast<void**>(&o_sprite_draw)) != MH_OK)
        BWLOG("hands: could not hook LH3DSprite::Draw (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5CDFC0), reinterpret_cast<void*>(&sparkle_detour), reinterpret_cast<void**>(&o_sparkle)) != MH_OK)
        BWLOG("hands: could not hook sparklecbstub (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6736B0), reinterpret_cast<void*>(&gj_process_detour), reinterpret_cast<void**>(&o_gj_process)) != MH_OK)
        BWLOG("hands: could not hook GJPSysInterface::Process_ (particle systems at the hand stay on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x673340), reinterpret_cast<void*>(&psys_process_detour), reinterpret_cast<void**>(&o_psys_process)) !=
        MH_OK)
        BWLOG("hands: could not hook the particle systems' process (the hand's effects stay on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B17B0), reinterpret_cast<void*>(&pet_update_detour), reinterpret_cast<void**>(&o_pet_update)) != MH_OK)
        BWLOG("hands: could not hook HandStateCreature::Update (strokes and slaps by the cursor's speed only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x682970), reinterpret_cast<void*>(&fireball_create_detour),
                      reinterpret_cast<void**>(&o_fireball_create)) != MH_OK)
        BWLOG("hands: could not hook MagicFireBall::Create (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5DC8F0), reinterpret_cast<void*>(&update_spell_info_detour),
                      reinterpret_cast<void**>(&o_update_spell_info)) != MH_OK)
        BWLOG("hands: could not hook GInterfaceStatus::UpdateSpellInfo (a cast spell's first turn is on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5DBFB0), reinterpret_cast<void*>(&set_hand_info_detour),
                      reinterpret_cast<void**>(&o_set_hand_info)) != MH_OK)
        BWLOG("hands: could not hook the status's hand info (a cast spell follows the game's hand on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x5FB490), reinterpret_cast<void*>(&spell_make_detour), reinterpret_cast<void**>(&o_spell_make)) !=
        MH_OK)
        BWLOG("hands: could not hook the spell making (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x728E20), reinterpret_cast<void*>(&seed_apply_map_detour),
                      reinterpret_cast<void**>(&o_seed_apply_map)) != MH_OK)
        BWLOG("hands: could not hook SpellSeed::ApplyThisToMapCoord (log only)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x72ACD0), reinterpret_cast<void*>(&seed_throw_detour), reinterpret_cast<void**>(&o_seed_throw)) !=
        MH_OK)
        BWLOG("hands: could not hook SpellSeed::ThrowObjectFromHand (a thrown miracle leaves from the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x46E680), reinterpret_cast<void*>(&draw_spell_in_hand_detour),
                      reinterpret_cast<void**>(&o_draw_spell_in_hand)) != MH_OK)
        BWLOG("hands: could not hook CHand::DrawSpellInHand (a miracle in the hand sits on the land)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x68FB20), reinterpret_cast<void*>(&add_mana_path_detour),
                      reinterpret_cast<void**>(&o_add_mana_path)) != MH_OK)
        BWLOG("hands: could not hook AddManaPathSprite (the influence warning's stream is the game's)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x69AC10), reinterpret_cast<void*>(&spell_point_inf_detour),
                      reinterpret_cast<void**>(&o_spell_point_inf)) != MH_OK)
        BWLOG("hands: could not hook SpellPointInf (the influence warning's path ends on the land)");
    return true;
}

const Controller* mock_controllers() {
    static LARGE_INTEGER last{}, freq{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const float dt = last.QuadPart ? float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart)) : 0.0f;
    last = now;
    for (int h = 0; h < 2; ++h)
        if (g_mock_sweep[h] != 0 && g_mock[h].active)
            g_mock[h].aim.rot = vr::basis_from_ypr(g_mock_sweep[h] * dt, 0, 0).compose(g_mock[h].aim.rot);
    for (int h = 0; h < 2; ++h) {
        if (g_mock[h].active) g_mock[h].aim.pos = g_mock[h].grip.pos = g_mock[h].aim.pos + g_mock_vel[h] * dt;
        // (a mock moving at a speed reports it, as SteamVR does: the gravity gloves' flick in tests)
        if (g_mock[h].active) g_mock[h].grip_vel = g_mock_vel[h], g_mock[h].has_vel = vr::length(g_mock_vel[h]) > 0;
        MockAimCircle& ac = g_mock_aim_circle[h];
        if (ac.t0 >= 0 && g_mock[h].active && g_rig.init) {
            const float f = float(std::min(1.0, (now_s() - ac.t0) / ac.T)), a = 6.2831853f * ac.turns * f;
            const Vec3 t = ac.c + Vec3{ac.r * std::sin(a), 0, ac.r * std::cos(a)};
            const WorldFrame wf = rig_frame();
            const Vec3 d = vr::basis_from_ypr(-g_rig.yaw_deg, 0, 0).apply(vr::normalize(t - to_world(wf, g_mock[h].aim.pos)));
            const float yaw = std::atan2(d.x, d.z) / vr::kDegToRad, pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / vr::kDegToRad;
            g_mock[h].aim.rot = g_mock[h].grip.rot = vr::basis_from_ypr(yaw, pitch, 0);
            if (f >= 1.0f) ac.t0 = -1;
        }
        MockSpiral& sp = g_mock_spiral[h];
        if (sp.t0 >= 0 && g_mock[h].active) {
            const float f = float(std::min(1.0, (now_s() - sp.t0) / sp.T));
            const float r = sp.r0 + (sp.r1 - sp.r0) * f, a = sp.sgn * 6.2831853f * sp.turns * f;
            g_mock[h].aim.pos = g_mock[h].grip.pos = sp.centre + Vec3{r * std::sin(a), r * std::cos(a), 0};
            if (f >= 1.0f) sp.t0 = -1;
        }
        if (g_mock_squeeze_at[h].at >= 0 && now_s() >= g_mock_squeeze_at[h].at)
            g_mock[h].squeeze = g_mock_squeeze_at[h].value, g_mock_squeeze_at[h].at = -1;
    }
    return g_mock;
}


// The other hand grabs too (player, 2026-11-05: "let the left hand grab, hold, throw, drop"). The
// game has one hand; it goes to whichever of the player's hands is acting (g_hand: the controller the
// god hand follows; the other is drawn as the second hand, and the two look alike, mirrored). The
// other hand takes over as its grip closes while the main hand does nothing (empty, open, no button,
// no gesture, no gravity glove), and gives it back once it is empty and open again. Holding
// something, a grip of the other hand by it (within 20 cm) passes it across. The other hand only
// grabs: no laser, gestures, miracles (it doesn't pop their bubbles or take a miracle across), leash,
// taps (its A, B and trigger aren't the god hand's while it acts).
// Would a grip of a hand here take something: its palm in a liftable thing's sphere, or a pile's or
// a field's? (The other hand takes the god hand only then: a grip on nothing took it over, and the
// main hand's next grab had to wait for it to come back; player, 2026-11-07.)
bool palm_would_take(const Vec3& palm) {
    bool yes = false;
    for_each_object_near(palm.x, palm.z, 3.0f * g_rig_scale, [&](void* o) {
        if (yes || o == own_creature() || IsBadReadPtr(o, 0x44)) return;
        Vec3 c;
        float r = 0;
        if (!bounding_sphere(o, nullptr, c, r) || r <= 0) return;
        const bool big = scoopable(o) || is_store(o);
        const bool living = living_or_tree(o);
        if (r > g_set.touch_max_m * g_rig_scale && !big && !living) return;
        // (A person or animal by the palm or just below it counts as the main hand's grab takes it:
        // within the grip's reach for the living, and the reach below the hand; player, 2026-11-07: a
        // sheep under the other hand wasn't taken.)
        Vec3 d = palm - c;
        if (d.y > 0) d.y = std::max(0.0f, d.y - g_set.reach_m * g_rig_scale);
        const float extra = living ? std::max(g_set.living_grip_m, g_set.touch_margin_m + 0.15f) : g_set.touch_margin_m + 0.15f;
        if (vr::length(d) > r + extra * g_rig_scale) return;
        if (big || liftable(o)) yes = true;
    });
    return yes;
}
// A field or a pile whose column the palm is in (see scoop_top), the nearest across the ground.
void* scoop_below_palm() {
    void* best = nullptr;
    float best_flat = 1e9f;
    const auto consider = [&](void* o, const Vec3& c, float r) {
        const float flat = std::sqrt((g_near.palm.x - c.x) * (g_near.palm.x - c.x) + (g_near.palm.z - c.z) * (g_near.palm.z - c.z));
        if (flat < r + g_set.touch_margin_m * g_rig_scale && g_near.palm.y < scoop_top(c, r) && flat < best_flat) best = o, best_flat = flat;
    };
    for_each_object_near(g_near.palm.x, g_near.palm.z, 3.0f * g_rig_scale, [&](void* o) {
        if (!scoopable(o)) return;
        Vec3 cc;
        float r = 0;
        if (!bounding_sphere(o, nullptr, cc, r) || r <= 0) return;
        // (A store: each of its heaps, food and wood, by its own place. Taken as the store, by its food
        // heap's middle, it gave food from over the wood too; player, 2026-11-08.)
        if (is_store(o)) {
            bool any = false;
            for (int off = 0xC0; off < 0x100; off += 4) {
                void* pile = *reinterpret_cast<void**>(static_cast<char*>(o) + off);
                if (!pile || IsBadReadPtr(pile, 0x44) || strncmp(class_name(pile), ".?AVPile", 8) != 0) continue;
                Vec3 pc;
                float pr = 0;
                if (bounding_sphere(pile, nullptr, pc, pr) && pr > 0) consider(pile, pc, std::max(pr, 0.5f * g_rig_scale)), any = true;
            }
            if (any) return;
        }
        consider(o, scoop_centre(o, cc), r);
    });
    return best;
}
void choose_hand(const Controller ctl[2], const WorldFrame& wf) {
    const int dom = g_set.dominant, off = 1 - dom;
    const auto sq = [&](const Controller& k) { return k.active && k.squeeze > g_set.squeeze_down; };
    const auto open = [&](const Controller& k) { return !k.active || k.squeeze < g_set.squeeze_up; };
    static bool was[2] = {};
    const bool edge[2] = {sq(ctl[0]) && !was[0], sq(ctl[1]) && !was[1]};
    was[0] = sq(ctl[0]), was[1] = sq(ctl[1]);
    if (!g_set.offhand || g_cinema || g_cutscene || g_fight) {
        if (g_hand != dom && hand_free(game_hand_state())) g_hand = dom;
        if (!g_set.offhand) g_hand = dom;
        return;
    }
    const uint32_t st = game_hand_state();
    const int cur = g_hand, other = 1 - cur;
    const Controller& cc = ctl[cur];
    const Controller& oc = ctl[other];
    const double now = now_s();
    const auto set_hand = [&](int h, const char* why) {
        if (h == g_hand) return;
        g_hand = h, g_hand_switched_at = now;
        g_aim_dir_filter.init = false, g_aim_pos_filter.init = false;
        if (h != dom) g_laser = false;
        g_ctl_vel.clear();  // (a throw takes the speed of the hand it leaves)
        // (And the gloves' aim is the hand's it was: the other hand's grip on a sheep below it locked on
        // to the sheep far off the main hand pointed at; player, 2026-11-07.)
        if (g_gg.st <= GravityGlove::kTarget) g_gg.st = GravityGlove::kIdle, g_gg.target = nullptr;
        BWLOG("hands: the god hand goes to the %s hand (%s)", h == vr::kLeft ? "left" : "right", why);
    };
    const bool cur_idle = hand_free(st) && open(cc) && !g_rmb && !g_lmb && !g_gd.on && g_gg.st <= GravityGlove::kTarget &&
                          !(cc.active && cc.trigger > g_set.trigger_up);
    if (edge[other] && oc.active && !g_ui_pointing && now - g_hand_switched_at > 0.2) {
        const float apart = vr::length(to_world(wf, cc.grip.pos) - to_world(wf, oc.grip.pos)) / std::max(1e-3f, wf.scale);
        if (cur_idle && other == dom) {
            set_hand(other, "its grip");
        } else if (cur_idle && palm_would_take(to_world(wf, oc.grip.pos))) {
            set_hand(other, "its grip on something, the other hand empty");
        } else if (st == kHandStateHolding && held_kind() != 2 && apart < g_set.offhand_pass_m) {
            set_hand(other, "passed across");
            vr::openvr::haptic(other, 0.03f, 180.0f, 0.6f);  // (as a pick-up's)
        }
    }
    // (The other hand empty and open again for a moment: the main hand's again.)
    static double idle_since = -1;
    if (g_hand != dom && hand_free(st) && open(ctl[g_hand]) && !g_rmb) {
        if (idle_since < 0) idle_since = now;
        if (now - idle_since > 0.3) set_hand(dom, "the other hand let go");
    } else {
        idle_since = -1;
    }
}
void prehold_end(const char* why) {
    PreHold& h = g_prehold;
    if (!h.object) return;
    float* m = h.obj3d && !IsBadWritePtr(h.obj3d + 0x14, 48) ? reinterpret_cast<float*>(h.obj3d + 0x14) : nullptr;
    // (Not taken, and the game hasn't moved it since: back where it was. A thing lying still isn't
    // written again by the game, and would have stayed at the palm.)
    const bool restore = m && h.wrote && game_hand_state() != kHandStateHolding && memcmp(m, h.last, sizeof(h.last)) == 0;
    if (restore) memcpy(m, h.saved, sizeof(h.saved));
    BWLOG("hands: pick-up drawn at the palm for %.2f s: %s%s", now_s() - h.t0, why, restore ? " (put back where it was)" : "");
    h = {};
}
void prehold_update() {
    PreHold& h = g_prehold;
    const uint32_t st = game_hand_state();
    if (!h.object) {
        // (What the game's hand is taking: its interface's object (GInterface +0x400), else what the hand
        // at the controller touched or reached for.)
        const char* gi = static_cast<const char*>(game_interface());
        void* o = gi && !IsBadReadPtr(gi + 0x400, 4) ? *reinterpret_cast<void* const*>(gi + 0x400) : nullptr;
        if (!o) o = g_latch.object ? g_latch.object : (now_s() - g_touch.t < 0.3 ? g_touch.object : nullptr);
        // (Taking it, or just holding it, the game not yet naming it.)
        const bool just_held = st == kHandStateHolding && g_holding_since > 0 && now_s() - g_holding_since < 0.3 && !object_in_hand();
        if (!g_prehold_on || !(st == 3 || just_held) || !g_rmb || !g_near.on || g_near.blend < 1.0f || !o || IsBadReadPtr(o, 0x44)) return;
        // (Not a gravity-gloves pull: it flies to the hand in its own arc.)
        if (g_gg.st == GravityGlove::kPulling || g_gg.st == GravityGlove::kFlying) return;
        if (o == own_creature() || scoopable(o) || is_store(o)) return;
        char* o3 = *reinterpret_cast<char**>(static_cast<char*>(o) + 0x40);
        Vec3 c;
        float r = 0;
        if (!o3 || IsBadWritePtr(o3 + 0x14, 48) || !bounding_sphere(o, nullptr, c, r)) return;
        const float* m = reinterpret_cast<const float*>(o3 + 0x14);
        h.object = o, h.obj3d = o3, h.t0 = now_s(), h.r = r, h.off = c - Vec3{m[9], m[10], m[11]};
        h.draw_only = g_prehold_tree_draw_only && is_tree(o);
        memcpy(h.saved, m, sizeof(h.saved));
        if (st == kHandStateHolding) BWLOG("hands: %s held, the game not yet naming it: drawn at the palm meanwhile", class_name(o));
        return;
    }
    if (st == kHandStateHolding) {
        // Once the game names what it holds: this one, it has it; another, this one is put back where it
        // was. Until then (nothing named, or the last thing it held), it is drawn at the palm, up to 0.6 s.
        const char* first = static_cast<const char*>(object_in_hand());
        void* held = first && !IsBadReadPtr(first, 0x44) ? *reinterpret_cast<void* const*>(first + 0x40) : nullptr;
        if (held == h.obj3d) {
            h.unnamed = false;
            return prehold_end("the game has it");
        }
        if (!h.unnamed) h.unnamed = true, h.held_wait = now_s();
        if (held || now_s() - h.held_wait > 0.6) {
            float* m = !IsBadWritePtr(h.obj3d + 0x14, 48) ? reinterpret_cast<float*>(h.obj3d + 0x14) : nullptr;
            if (m && h.wrote && memcmp(m, h.last, sizeof(h.last)) == 0) memcpy(m, h.saved, sizeof(h.saved));
            BWLOG("hands: pick-up drawn at the palm: the game holds %s: put back", held ? class_name(first) : "nothing named");
            h = {};
            return;
        }
    } else {
        h.unnamed = false;
        if (st != 3 || !g_rmb) return prehold_end(st != 3 ? "the game stopped taking it" : "let go");
    }
    if (now_s() - h.t0 > 2.5) return prehold_end("timed out");
    float* m = IsBadWritePtr(h.obj3d + 0x14, 48) ? nullptr : reinterpret_cast<float*>(h.obj3d + 0x14);
    if (!m) return prehold_end("gone");
    // Its middle just out from the palm, as it will sit in the hand.
    const Vec3 n = vr::normalize(g_near.y);
    const Vec3 want = g_near.palm + n * std::min(h.r, 0.12f * g_rig_scale) - h.off;
    if (h.draw_only) {
        h.draw_at = want, h.has_draw = true;
        return;
    }
    m[9] = want.x, m[10] = want.y, m[11] = want.z;
    memcpy(h.last, m, sizeof(h.last));
    h.wrote = true;
}
// A shake with the leash in the hand that the game didn't take (a second on, the leash still in the
// hand): the leash is dropped as the game's own drop would (packet 0x5B). (Player, 2026-11-08: a frozen
// creature's leash wouldn't shake off; the game's drop packet did take it off.)
void shake_leash_check() {
    if (g_shake_leash_at < 0 || now_s() - g_shake_leash_at < 1.0) return;
    g_shake_leash_at = -10;
    if (!leash_in_hand()) return;
    using SetPacketIdFn = void(__thiscall*)(void*, int, uint32_t);
    if (void* game = game::game_instance()) reinterpret_cast<SetPacketIdFn>(0x550C80)(game, 0x5B, 0);
    BWLOG("hands: the shake didn't drop the leash: dropped by the mod (packet 0x5B)");
}
// The controls on the back of the left wrist (player, 2026-11-22: "use the wrist on the back of the left
// hand for those kinds of interactions"): shown while the back of the other hand faces the eyes (as a
// watch is looked at), poked with the main hand's fingertip. The game's own: its pause (PauseGame 0x54AE20,
// as the menu does: the world stops, the player can still move about), its speed (GGame::SetSpeed 0x5537F0,
// GGame +0x205B74; the flat game's Alt+1 / Alt+2 halve and double it, from 0.5x to 2x), and the villagers'
// info over all of them (GGame +0x14 bit 0x200000, as the flat game's info key toggles it with nothing under
// the hand).
struct WristPanel {
    bool shown = false;
    double since = 0;
    Vec3 centre, right, up, fwd;
    struct Button {
        int id;
        Vec3 pos;
        float half_w;
        bool lit;
    };
    std::vector<Button> buttons;
    int armed = -1;  // the button the fingertip is in (pressed as it went in; again only once it has left)
};
WristPanel g_wrist;
bool g_wrist_on = true;  // [hands] wrist_menu
int g_wrist_log = 0;
uint32_t game_flags() {
    const char* game = static_cast<const char*>(game::game_instance());
    return game && !IsBadReadPtr(game + 0x14, 4) ? *reinterpret_cast<const uint32_t*>(game + 0x14) : 0;
}
float game_speed() {
    const char* game = static_cast<const char*>(game::game_instance());
    return game && !IsBadReadPtr(game + 0x205B74, 4) ? *reinterpret_cast<const float*>(game + 0x205B74) : 1.0f;
}
std::wstring speed_text(float s) {
    if (s < 0.75f) return L"1/2x";
    if (s < 1.5f) return L"1x";
    wchar_t b[16];
    swprintf(b, 16, L"%.0fx", s);
    return b;
}
void wrist_press(int id) {
    void* game = game::game_instance();
    if (!game) return;
    const float s = game_speed();
    switch (id) {
        case 0: {  // pause / resume
            const bool paused = game_flags() & 4;
            reinterpret_cast<void(__cdecl*)(int)>(0x54AE20)(paused ? 0 : 1);
            BWLOG("hands: wrist: %s (the game's pause %d)", paused ? "resumed" : "paused", (game_flags() & 4) != 0);
            break;
        }
        case 1:
        case 2:
        case 3: {  // slower, normal, faster
            const float to = id == 1 ? std::max(0.5f, s * 0.5f) : id == 3 ? std::min(2.0f, s * 2.0f) : 1.0f;
            reinterpret_cast<void(__thiscall*)(void*, float)>(0x5537F0)(game, to);
            BWLOG("hands: wrist: game speed %.2f -> %.2f (now %.2f)", s, to, game_speed());
            break;
        }
        case 4: {  // the villagers' info over all of them
            uint32_t& f = *reinterpret_cast<uint32_t*>(static_cast<char*>(game) + 0x14);
            f ^= 0x200000;
            BWLOG("hands: wrist: villagers' info %s", (f & 0x200000) ? "shown" : "hidden");
            break;
        }
    }
    vr::openvr::haptic(g_set.dominant, 0.02f, 220.0f, 0.6f);
}
void wrist_update(const Vec3& eye) {
    WristPanel& w = g_wrist;
    NearHand nh;
    const bool tracked = g_wrist_on && !g_cinema && !g_ui_pointing && game::game_instance() && second_hand(nh);
    // (The back of the other hand toward the eyes: shown above 0.6, hidden below 0.3, after 0.15 s.)
    const float back = tracked ? vr::dot(vr::normalize(nh.y) * -1.0f, vr::normalize(eye - nh.pos)) : -1.0f;
    const bool want = tracked && back > (w.shown ? 0.3f : 0.6f) && vr::length(eye - nh.pos) < 0.8f * g_rig_scale;
    if (want == w.shown) w.since = now_s();
    else if (now_s() - w.since > 0.15) {
        w.shown = want, w.since = now_s(), w.armed = -1;
        static int told = 0;
        if (told++ < 3) BWLOG("hands: wrist: the controls %s", want ? "shown" : "hidden");
    }
    w.buttons.clear();
    if (!w.shown) return;
    const float m = g_rig_scale;
    // Over the forearm just behind the wrist, as a watch's face, facing the eyes; the rows upward. (Over the
    // back of the hand the hand, drawn after them, hid the words it was in front of.)
    w.centre = nh.pos - vr::normalize(nh.y) * (0.02f * m) - vr::normalize(nh.z) * (0.09f * m);
    w.fwd = vr::normalize(w.centre - eye);
    w.right = vr::normalize(vr::cross(Vec3{0, 1, 0}, w.fwd));
    w.up = vr::cross(w.fwd, w.right);
    const float row = 0.032f * m, col = 0.065f * m;
    const Vec3 c = w.centre;
    w.buttons = {{0, c + w.up * row, 0.035f * m, false},
                 {1, c - w.right * col, 0.03f * m, false}, {2, c, 0.018f * m, false}, {3, c + w.right * col, 0.03f * m, false},
                 {4, c - w.up * row, 0.04f * m, false}};
    if (g_wrist_log > 0) {  // (debug: hands wrist_log N)
        --g_wrist_log;
        for (const auto& b : w.buttons) BWLOG("hands: wrist: button %d at (%.2f, %.2f, %.2f)", b.id, b.pos.x, b.pos.y, b.pos.z);
    }
    // The main hand's fingertip (as for drawing gestures).
    if (!g_near.on || g_near.blend < 1.0f) return;
    const Vec3 tip = g_near.palm + g_near.z * (0.09f * m);
    int in = -1;
    for (auto& b : w.buttons) {
        const Vec3 d = tip - b.pos;
        const float h = vr::dot(d, w.right), v = vr::dot(d, w.up), z = vr::dot(d, w.fwd);
        b.lit = std::fabs(h) < b.half_w && std::fabs(v) < 0.014f * m && std::fabs(z) < 0.03f * m;
        if (b.lit) in = b.id;
    }
    if (in >= 0 && in != w.armed) wrist_press(in);
    w.armed = in;
}
bool wrist(WristView& out) {
    if (!g_wrist.shown || g_wrist.buttons.empty()) return false;
    out.unit = g_rig_scale;
    out.buttons.clear();
    const bool paused = game_flags() & 4, info = game_flags() & 0x200000;
    for (const auto& b : g_wrist.buttons) {
        std::wstring t = b.id == 0 ? (paused ? L"Resume" : L"Pause") : b.id == 1 ? L"Slower" : b.id == 2 ? speed_text(game_speed()) : b.id == 3 ? L"Faster"
                                                                                                                       : (info ? L"Info: on" : L"Info: off");
        out.buttons.push_back({t, b.pos, b.lit});
    }
    return true;
}

void update(const Controller ctl[2], const WorldFrame& wf, const EyeView& pick_eye) {
    g_gg_view = pick_eye, g_gg_view_ok = true;
    shake_leash_check();
    game_hand_alive();  // (drops a hand freed by a restart)
    ++g_updates;
    // A miracle given from the pause menu's debug buttons: its bubble below the hand, once the menu
    // has closed (SetupBox::CurrentActiveBox, 0xC4CD20, empty) and the hand is back. A little in
    // front of the palm, so it doesn't land in the hand.
    if (g_give_type >= 0 && *reinterpret_cast<void* const*>(0xC4CD20) == nullptr && g_near.blend >= 1.0f) {
        const Vec3 at = g_near.palm + g_near.z * (0.25f * g_rig_scale);
        const MapCoords c{int32_t(at.x * 65536.0f / 10.0f), int32_t(at.z * 65536.0f / 10.0f), 0.0f};
        using CreateFn = void*(__cdecl*)(const MapCoords*, int, int, float);
        void* seed = reinterpret_cast<CreateFn>(0x72A2F0)(&c, g_give_type, 0, 1.0f);
        BWLOG("hands: debug menu: a miracle's bubble, type %d (%.20s), made at (%.1f, %.1f): %p", g_give_type,
              reinterpret_cast<const char*>(0xD9D678 + g_give_type * 0x190 + 0x18), at.x, at.z, seed);
        g_give_type = -1;
    }
    g_picked = g_picked_now, g_picked_creature = g_picked_now_creature;
    g_picked_now = nullptr, g_picked_now_creature = false;
    g_living_offered = false, g_scroll_by_palm = false, g_small_by_palm = false;
    g_set.dominant = g_set.dominant_left ? vr::kLeft : vr::kRight;  // (the VR Options' main hand, live)
    {   // The other hand's A opens and closes the game's menu (Esc).
        static bool menu_was = false;
        const Controller& o = ctl[1 - g_set.dominant];
        if (o.active && o.a && !menu_was && !ui::debug_menu_escape()) input::queue_script("key esc\n");
        menu_was = o.active && o.a;
        // The other hand's B: a feedback snapshot (the stereo renderer takes it: both eyes, a burst,
        // the state and a copy of the log), with a buzz to say it was taken.
        static bool feedback_was = false;
        if (o.active && o.b && !feedback_was) {
            g_feedback_requested = true;
            vr::openvr::haptic(1 - g_set.dominant, 0.08f, 200.0f, 0.8f);
        }
        feedback_was = o.active && o.b;
    }
    g_ptr = PointerState{};
    if (g_hand != vr::kLeft && g_hand != vr::kRight) g_hand = g_set.dominant;
    choose_hand(ctl, wf);
    Controller c = ctl[g_hand];
    if (g_hand != g_set.dominant) c.a = c.b = false, c.trigger = 0, c.stick_click = false;  // (the other hand only grabs)
    if (c.active) {
        // The wrist's twist about the controller's axis since the last frame (+ clockwise as seen
        // from behind the hand): it turns a held scaffold's blueprint. (In the controller's own
        // tracking space: the rig turning isn't a twist.)
        static vr::Basis prev;
        const double t = now_s();
        if (t - g_twist_at < 0.2 && g_bp.scaffold && t - g_bp.seen < 0.5) {
            // The hand's turn about the vertical since the last frame (+ to the right, seen from above),
            // as the blueprint turns on the ground. (The twist about the controller's own axis turned it
            // one way with the hand pointing a little down and the other way pointing a little up: most
            // of the time against the hand; player, 2026-11-13.) The twist part of the rotation from the
            // last frame's axes to these: 2 atan2(D_xz - D_zx, 1 + trace D), D = now * before^T.
            const vr::Basis& q = c.grip.rot;
            const Vec3 cn[3] = {q.right, q.up, q.fwd}, pv[3] = {prev.right, prev.up, prev.fwd};
            float dxz = 0, dzx = 0, tr = 0;
            for (int k = 0; k < 3; ++k) dxz += cn[k].x * pv[k].z, dzx += cn[k].z * pv[k].x, tr += vr::dot(cn[k], pv[k]);
            if (1.0f + tr > 1e-3f) g_bp.twist += 2.0f * std::atan2(dxz - dzx, 1.0f + tr);
        }
        prev = c.grip.rot;
        g_twist_at = t;
    }
    {   // The controller's own velocity in the world (SteamVR's, smoothed by its tracking), kept a moment.
        if (c.has_vel) g_ctl_vel.push_back({now_s(), wf.base_rot.apply(c.grip_vel * wf.scale)});
        while (!g_ctl_vel.empty() && now_s() - g_ctl_vel.front().t > 0.3) g_ctl_vel.pop_front();
    }
    if (g_cutscene) c.squeeze = 0;
    // (The trigger carrying: from the frame after it started, it is a fully closed grip and no
    // trigger, so everything the grip does applies unchanged, until the trigger is let go.)
    if (g_trigger_grab && c.trigger < g_set.trigger_up) g_trigger_grab = false;
    if (g_trigger_grab) c.squeeze = 1.0f, c.trigger = 0.0f;
    // A creature fight (the player's creature fighting, with the fight camera): the game takes its
    // clicks as the fight's orders (GInterface: the left button going down moves or attacks where
    // the cursor is, the right one too). The mod's own uses of those buttons (petting, the leash's
    // sending and toggling, the grip's reach) kept clicking by themselves or swallowed the player's
    // (player, 2026-10-23); in a fight the trigger is simply the game's left button.
    {
        const bool was = g_fight;
        void* gi = game_interface();
        g_fight = g_set.fight_mode && gi && !g_cinema && creature_fighting(gi);
        if (g_fight != was) BWLOG("hands: creature fight %s", g_fight ? "started: the trigger is the game's click" : "over");
    }  // (the hand is the game's in a cutscene: no grabbing)
    if (!g_set.pointer || !c.active) {
        set_lmb(false);
        set_rmb(false);
        return;
    }
    detect_shake(c);
    g_ptr.valid = true;
    {   // filtered in tracking space, so locomotion doesn't smear it
        static LARGE_INTEGER last{}, freq{};
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        const float dt = last.QuadPart ? std::min(0.1f, float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart))) : 0.0f;
        last = now;
        const Vec3 dir_t = vr::normalize(g_aim_dir_filter.filter(c.aim.rot.fwd, dt));
        const Vec3 pos_t = g_aim_pos_filter.filter(c.aim.pos, dt);
        g_ptr.origin = to_world(wf, pos_t);
        g_ptr.dir = vr::normalize(wf.base_rot.apply(dir_t));
    }
    if (cast_island(g_ptr.origin, g_ptr.dir, g_set.max_ray, g_ptr.hit)) {
        g_ptr.hit_land = true;
        ++g_land_hits;
    } else if (g_ptr.dir.y < -1e-4f && g_ptr.origin.y > 0) {  // the sea (y = 0)
        g_ptr.hit = g_ptr.origin + g_ptr.dir * (-g_ptr.origin.y / g_ptr.dir.y);
    } else {
        g_ptr.hit = g_ptr.origin + g_ptr.dir * g_set.max_ray;
    }
    {
        Vec3 on_screen;
        g_ui_pointing = ui::pointer(g_ptr.origin, g_ptr.dir, on_screen, g_ui_x, g_ui_y);
        // (In a fight the speech boxes are drawn like a menu's panels, and a hand raised toward the
        // caption turned into the laser; player, 2026-10-23. Only a real menu then: the game's hand
        // is put away (state 0) while one is open.)
        if (g_fight && game_hand_state() != 0) g_ui_pointing = false;
        if (g_ui_pointing) {
            g_ptr.hit = on_screen;
            g_ptr.hit_land = false;
        }
    }
    if (g_cinema) {
        // Only the screen exists: on it the laser is the mouse (trigger left button, grip right),
        // off it the controller does nothing.
        g_near.on = false;
        g_near.blend = 0;
        g_near.dragging = false;
        if (!g_ui_pointing) {
            set_lmb(false);
            set_rmb(false);
            g_ptr.hit = g_ptr.origin + g_ptr.dir * (3.0f * wf.scale);
            return;
        }
    }
    // Near or far? Only switch between actions: never while holding, tugging or dragging.
    {
        // "Within reach" is about where the hand is, not where the beam lands: the controller
        // tip's height above the ground (at or below the surface counts -- the head is kept
        // only a little above the terrain, so a lowered hand easily dips under it, and from
        // there a downward ray finds no land at all), or a beam that lands right at the tip.
        const float beam_m = vr::length(g_ptr.hit - g_ptr.origin) / std::max(1e-3f, wf.scale);
        const float above_m = (g_ptr.origin.y - std::max(0.0f, ground_height(g_ptr.origin.x, g_ptr.origin.z))) /
                              std::max(1e-3f, wf.scale);
        const float dist_m = std::min(beam_m, above_m);
        g_near_dbg = {beam_m, above_m, c.b, 0};
        const uint32_t st = game_hand_state();
        const bool busy = g_lmb || g_rmb || g_near.dragging || g_drop != DropClick::kIdle || st == 2 || st == 3 || st == 4;
        g_near_dbg.busy = busy;
        const bool was = g_near.on;
        static bool a_was = false;
        // While carrying, the switch is allowed: A recalls the hand, with what it holds, to the
        // controller, or sends it back out along the laser.
        const bool carrying = st == 4 && !g_lmb && !g_near.dragging && g_drop == DropClick::kIdle;
        const bool can_switch = !busy || carrying;
        if (g_set.move_teleport) g_laser = false;  // (A teleports instead)
        else if (c.a && !a_was && g_set.always_hand && can_switch) g_laser = !g_laser;
        a_was = c.a;
        g_near.in_reach = above_m < g_set.reach_m;
        // (B draws a gesture instead of giving the laser, unless gesture_button_b is off.)
        const bool b_laser = c.b && !g_set.gesture_b;
        {
            const bool draw = g_set.gesture_b && c.b && !g_cinema && !g_ui_pointing && hand_free(st);
            if (draw && !g_gd.on) {
                // A clean start: the game's samples emptied (as its own reset does).
                const auto* game = static_cast<const char*>(game::game_instance());
                char* buf = game ? *reinterpret_cast<char* const*>(game + 0x25006C) : nullptr;
                if (buf && !IsBadWritePtr(buf, 0xC94)) {
                    buf[0xC88] = 0, buf[0xC90] = 0, *reinterpret_cast<uint32_t*>(buf + 0xC8C) = 0;
                    memset(buf + 8, 0, 0x320 * 4);
                }
                g_gd.trail.clear();
                vr::openvr::haptic(g_hand, 0.02f, 200.0f, 0.4f);
                BWLOG("hands: drawing a gesture (B held, %s)", g_near.on ? "the fingertip" : "the laser's point");
            }
            g_gd.on = draw;
        }
        if (!g_set.near_enabled || g_shield_draw || now_s() < g_shield_tail_until) g_near.on = false;
        else if (g_set.always_hand) {
            if (can_switch) g_near.on = !(g_laser || b_laser || g_ui_pointing);
        } else if (b_laser) g_near.on = false;
        else if (!busy) g_near.on = g_near.on ? dist_m < g_set.near_out : dist_m < g_set.near_in;
        if (g_near.on != was) vr::openvr::haptic(g_hand, 0.015f, 200.0f, 0.3f);
        static double last_t = 0;
        const double t = now_s();
        const float dt = last_t ? float(std::min(0.1, t - last_t)) : 0.0f;
        last_t = t;
        const float step = g_set.blend_time > 0 ? dt / g_set.blend_time : 1.0f;
        g_near.blend = std::clamp(g_near.blend + (g_near.on ? step : -step), 0.0f, 1.0f);
        // Target wrist frame from the controller's grip pose.
        const Basis gr = wf.base_rot.compose(c.grip.rot);
        // Fingers: a little below the controller's axis, as a hand holds it.
        const float fp = g_set.finger_pitch_deg * vr::kDegToRad;
        g_near.z = vr::normalize(gr.fwd * std::cos(fp) - gr.up * std::sin(fp));
        // The palm faces inwards on a held controller: left for a right hand, right for a left.
        g_near.y = vr::normalize(gr.right * (g_hand == vr::kRight ? -1.0f : 1.0f));
        g_near.x = vr::normalize(vr::cross(g_near.y, g_near.z));
        g_near.pos = to_world(wf, c.grip.pos) - g_near.z * (g_set.wrist_back_m * wf.scale);
        g_near.palm = to_world(wf, c.grip.pos);
        g_rig_scale = wf.scale;
        g_hand_picks = g_set.touch_grab && g_near.on && g_near.blend >= 1.0f && hand_free(st);
        g_touch_armed = g_hand_picks && !g_rmb;
        gravity_update(c, st);
        // ... or for as long as the grip stays closed on it (stroking a creature is the action
        // button held on it while the hand rubs).
        if (!g_hand_picks || (!g_rmb && now_s() - g_latch.t > 0.6)) g_latch.object = nullptr;
        // The other hand: the same frame rules, mirrored (its palm faces the other way).
        const Controller& oc = ctl[1 - g_hand];
        g_second_valid = oc.active;
        update_fist(1 - g_hand, oc.active ? oc.squeeze : 0.0f);
        update_fist(g_hand, ctl[g_hand].active ? ctl[g_hand].squeeze : 0.0f);
        if (oc.active) {
            const Basis og = wf.base_rot.compose(oc.grip.rot);
            g_second.z = vr::normalize(og.fwd * std::cos(fp) - og.up * std::sin(fp));
            g_second.y = vr::normalize(og.right * (g_hand == vr::kRight ? 1.0f : -1.0f));
            g_second.x = vr::normalize(vr::cross(g_second.y, g_second.z));
            g_second.pos = to_world(wf, oc.grip.pos) - g_second.z * (g_set.wrist_back_m * wf.scale);
        }
    }
    // The point the game's hand goes to: the laser hit, or in near mode the ground straight
    // below the controller tip, so the game's hand -- and where it drops things -- stays under
    // the player's hand (through the eye, the tip would land on whatever is behind it).
    // On a recall the cursor stays at the laser point until the drawn hand has arrived at the
    // controller, so the hand (and what it carries) comes back along the laser, not up from the
    // ground below.
    // A hand on a creature pets it: the game's hand goes where the player's is (the cursor is the
    // palm as the picking eye sees it), and the action button is held while the hand stays on it.
    // Handing over: an object carried in the hand and held up to a creature for a moment is given
    // to it. In the flat game the player carries the object over the creature, the game's hand
    // logic (ObtainRequiredHandPosition, HandStateHolding::Update) notes it as the creature to
    // give to (CHand+0x49B4, read by CHand::GetCreatureToGiveTo), and the action button then gives
    // instead of dropping. So while the object is held up to the creature the cursor goes onto
    // the creature; once the game has its creature to give to, the hold-to-carry grip is let go
    // for the player, and the click that follows a release is the action button on the creature.
    // The grip counts as open until it really is.
    static double near_creature_since = -1, giving_until = -10;
    static bool grip_spent = false, pet_wait = false;
    static Vec3 give_centre;
    bool offering = false;
    {
        const uint32_t hs = game_hand_state();
        const double now = now_s();
        const bool hand_mode = g_near.on && g_near.blend >= 1.0f && !g_ui_pointing;
        Vec3 centre;
        float radius = 0;
        // On a creature: from its place in the game's creature list (it only reports to the
        // picker while the cursor is on it).
        const bool on_creature = hand_mode && creature_at(g_near.palm, g_set.touch_margin_m * wf.scale, centre, radius);
        if (on_creature) g_on_creature_at = now, g_creature_centre = centre;
        // (Petting is for the player's own creature only: any creature's sphere counted, and with
        // the leash in hand by a bear the hand kept clicking by itself; player, 2026-10-23.)
        {
            Vec3 oc;
            float orad = 0;
            void* own = own_creature();
            if (on_creature && own && bounding_sphere(own, nullptr, oc, orad) && vr::length(oc - centre) < 0.5f) g_on_own_at = now;
        }
        const bool offered = g_set.give && hand_mode && hs == kHandStateHolding && g_rmb &&
                             creature_at(g_near.palm, g_set.give_margin_m * wf.scale, centre, radius);
        if (!offered) near_creature_since = -1;
        else if (near_creature_since < 0) near_creature_since = now;
        offering = offered;
        if (offered) give_centre = centre;
        // Held up to the player's own creature for the hold time, the object is given to it: the
        // mod asks the creature the game's own question (Creature::InterfaceValidToGiveObject,
        // vtable +0x748: can it take this?) and, if yes, sends the game's give: packet 0x1F with the
        // creature's id (its handler, fn_005DA2D0, starts the hand-over: fn_0046C080). The grip
        // is let go for the game, without the drop click that would drop it.
        // (The game's own way, a click with its hand's creature-to-give-to set, needs the creature
        // under the game's cursor: with the leash on the picker never reports it, and moving the
        // cursor onto the creature put the leash's end behind it, so it turned away.)
        if (offered && now - near_creature_since > g_set.give_dwell && now > giving_until && g_game_hand) {
            void* own = own_creature();
            Vec3 oc;
            float orad = 0;
            void* status = *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850);
            void* held = status ? *reinterpret_cast<void**>(static_cast<char*>(status) + 0x120) : nullptr;
            void* game = game::game_instance();
            if (own && held && game && bounding_sphere(own, nullptr, oc, orad) && vr::length(oc - centre) < 0.5f) {
                using ValidFn = int(__thiscall*)(void*, void*, void*);
                const auto valid_fn = reinterpret_cast<ValidFn>(game_method(own, 0x748));
                const int valid = valid_fn ? valid_fn(own, status, held) : 0;
                if (valid != 1) {
                    static double logged_at = -10;
                    if (now - logged_at > 2.0) BWLOG("hands: offering: the creature can't take %s (%d)", class_name(held), valid), logged_at = now;
                } else {
                    giving_until = now + 0.7;
                    give_centre = centre;
                    grip_spent = true;
                    pet_wait = true;  // and no petting until the hand has left the creature once
                    g_no_drop_click_until = now + 1.0;
                    using IdFn = uint32_t(__thiscall*)(void*);
                    const uint32_t id = reinterpret_cast<IdFn>(0x436A70)(own);
                    using SetPacketIdFn = void(__thiscall*)(void*, int, uint32_t);
                    reinterpret_cast<SetPacketIdFn>(0x550C80)(game, 0x1F, id);
                    g_give_finish = {id, now, -1};
                    BWLOG("hands: handing %s to the creature at (%.1f, %.1f, %.1f) (packet 0x1F, id %u)", class_name(held), centre.x, centre.y,
                          centre.z, id);
                    if (g_flight_trace.on)
                        g_flight_trace.t0 = now, g_flight_trace.launched = -10, g_flight_trace.obj3d = g_held_seen.obj3d,
                        g_flight_trace.object = g_flight_trace.phys = nullptr;
                    vr::openvr::haptic(g_hand, 0.03f, 140.0f, 0.5f);
                }
            }
        }
        if (pet_wait && now - g_on_creature_at > 0.5 && now > giving_until) pet_wait = false;
        g_petting = !g_fight && g_set.pet && hand_mode && !pet_wait && (hand_free(hs) || hs == 7) && now - g_on_own_at < 0.25;
    }
    const bool giving = now_s() < giving_until;
    Vec3 cursor_point = g_ptr.hit;
    if (g_petting) {
        cursor_point = g_creature_centre + (g_near.palm - g_creature_centre) * g_set.pet_gain;
        g_near.ground = g_ptr.origin;
        g_near.ground.y = std::max(0.0f, ground_height(g_near.ground.x, g_near.ground.z));
    } else if (g_near.on) {
        g_near.ground = g_ptr.origin;
        g_near.ground.y = std::max(0.0f, ground_height(g_near.ground.x, g_near.ground.z));
        // An object only reports to the picker while the cursor is inside its outline on screen
        // (LH3DBoundingBox::CheckRegionOnScreen). So an empty hand's cursor is the palm itself, as
        // the picking eye sees it: whatever the palm is inside then reports, and can be touched.
        // (With the cursor on the ground below the hand, a scroll floating in the air never did,
        // and a creature only when the hand happened to be over its feet.) A hand that carries
        // something keeps the ground point: that is where the game puts things down.
        if (g_near.blend >= 1.0f) cursor_point = hand_free(game_hand_state()) ? g_near.palm : g_near.ground;
        // (In a fight: the palm on a creature (to strike it), else where the controller points on
        // the land (to send the creature there), as the laser would.)
        if (g_fight && g_near.blend >= 1.0f && !(now_s() - g_on_creature_at < 0.25) && g_ptr.hit_land) cursor_point = g_ptr.hit;
        // A thing in flight within the catch reach of an empty hand: the cursor on it, so that it
        // reports to the picker (only while the cursor is inside its outline) and can be caught.
        //  - Once on it, the cursor stays until it is 15 cm further out. (Flipping on and off it
        //    at the edge of the reach, the game's hand flipped between its poses: the fingers
        //    twitched.)
        //  - While the grip is closing on the thing it latched, the cursor stays on that thing,
        //    however far it has flown on: the game clicks where the cursor is, and below a hand up
        //    in the air that was something on the ground (a fence was picked up instead).
        static const void* catch_target = nullptr;
        if (g_near.blend >= 1.0f && hand_free(game_hand_state())) {
            const void* latched = g_rmb && g_latch.object && in_flight_object(g_latch.object) ? g_latch.object : nullptr;
            float best = 1e9f;
            const void* pick = nullptr;
            for (const void* o : g_in_flight) {
                Vec3 c;
                float r = 0;
                if (!bounding_sphere(const_cast<void*>(o), nullptr, c, r) || r > g_set.touch_max_m * wf.scale) continue;
                const float d = vr::length(g_near.palm - c) - std::max(r, g_set.touch_min_m * wf.scale);
                const float reach = g_set.touch_margin_m + g_set.catch_reach_m + (o == catch_target ? 0.15f : 0.0f);
                if (o == latched) best = -1e9f, pick = o, cursor_point = c;
                else if (d < reach * wf.scale && d < best) best = d, pick = o, cursor_point = c;
            }
            catch_target = pick;
        } else {
            catch_target = nullptr;
        }
    }
    // A story or tutorial scroll (ScriptHighlight) the palm is on: the cursor on it. They float
    // over buildings, and with the cursor at the palm the game's picker often settled on the
    // building behind (a tap then knocked on the house). Their sphere is small (under 10 cm), so
    // they are reached from 15 cm further away than other things.
    if (g_near.on && g_near.blend >= 1.0f && hand_free(game_hand_state())) {
        float best = 1e9f;
        // (So does a miracle's bubble the palm is in: it reports only with the cursor over the
        // small potion inside it, not anywhere in its glow.)
        for_each_object_near(g_near.palm.x, g_near.palm.z, 20.0f, [&](void* o) {
            void* vt = *reinterpret_cast<void**>(o);
            const bool bubble = vt == reinterpret_cast<void*>(0x8F3774);
            if (vt != reinterpret_cast<void*>(0x94228C) && !bubble) return;
            Vec3 c;
            float r = 0;
            if (!bounding_sphere(o, nullptr, c, r)) return;
            if (!bubble && r >= g_set.touch_min_m * 1.5f * wf.scale) return;  // (small scrolls, not the signposts)
            const float d = vr::length(g_near.palm - c) - std::max(r, g_set.touch_min_m * wf.scale);
            if (d < (g_set.touch_margin_m + (bubble ? 0.0f : 0.15f)) * wf.scale && d < best) best = d, cursor_point = c;
        });
    }
    // A small liftable thing the palm is on (the gate stone): the cursor on it, so it reports to the
    // picker (the line from the eye through the palm on top of it passed over it to the ground),
    // and ahead of a field's or pile's cursor below: the stone lies at a field's edge, and the field
    // under the hand took the grip, which scooped food (player, 2026-11-02).
    bool small_cursor = false;
    if (g_near.on && g_near.blend >= 1.0f && hand_free(game_hand_state())) {
        float best = 1e9f;
        auto look = [&](void* o) {
            if (IsBadReadPtr(o, 0x44) || o == own_creature() || living_or_tree(o) || scoopable(o) || is_store(o) ||
                strcmp(class_name(o), ".?AVMagicFood@@") == 0)
                return;
            Vec3 c;
            float r = 0;
            if (!bounding_sphere(o, nullptr, c, r) || r <= 0 || r > g_set.touch_max_m * wf.scale) return;
            const float d = vr::length(g_near.palm - c) - r;
            if (d < (g_set.touch_margin_m + 0.15f) * wf.scale && d < best && liftable(o)) {
                best = d, cursor_point = c, small_cursor = true;
                static const void* logged = nullptr;
                if (o != logged) logged = o, BWLOG("hands: the cursor on %s %p by the palm (%.2f m out of its sphere)", class_name(o), o, d / wf.scale);
            }
        };
        for_each_object_near(g_near.palm.x, g_near.palm.z, 20.0f, look);
        for (const RecentThing& t : g_recent_things)
            if (now_s() - t.at < 3.0) look(t.object);
    }
    // A store's pile or a field within the scooping reach of the palm (above it): the cursor on
    // it. They lie low, so the line from the eye through the palm passed beyond them and the game
    // never saw them under the hand. Fields are in the map; a store's piles are its own
    // (StoragePit +0xC4.., read by StoragePit::GetResourcePile).
    if (g_near.on && g_near.blend >= 1.0f && hand_free(game_hand_state()) && g_set.pile_reach_m > 0 && !small_cursor) {
        // (The smallest one the palm is inside wins, as for touch, else the one most under the
        // hand: horizontally nearest for its size, piles lie side by side in a store. The cursor
        // goes to its side toward the palm: a big pile's centre can be under the ground.)
        float best = 1e9f;
        const auto consider = [&](void* o) {
            Vec3 c;
            float r = 0;
            if (!o || IsBadReadPtr(o, 0x44) || !bounding_sphere(o, nullptr, c, r)) return;
            const float d = vr::length(g_near.palm - c) - std::max(r, g_set.touch_min_m * wf.scale);
            if (g_dbg_pile_log > 0) --g_dbg_pile_log, BWLOG("hands: scoopable %p %s at (%.1f, %.1f, %.1f) r %.2f, palm %.2f outside", o, class_name(o), c.x, c.y, c.z, r, d);
            if (d >= (g_set.touch_margin_m + g_set.pile_reach_m) * wf.scale) return;
            const float flat = std::sqrt((g_near.palm.x - c.x) * (g_near.palm.x - c.x) + (g_near.palm.z - c.z) * (g_near.palm.z - c.z));
            const float score = flat;  // (the nearest across the ground: a store splits between its piles by where they lie)
            if (score >= best) return;
            best = score;
            const Vec3 to = g_near.palm - c;
            const float len = vr::length(to);
            cursor_point = len > 1e-3f ? c + to * (std::min(len, r * 0.6f) / len) : c;
            // (Over the pile, the ground straight below the palm: a big pile's sphere is much larger
            // than its mound, and a point on the sphere floated above it, where the game saw nothing.)
            if (flat < r * 0.8f) cursor_point = {g_near.palm.x, ground_height(g_near.palm.x, g_near.palm.z), g_near.palm.z};
        };
        for_each_object_near(g_near.palm.x, g_near.palm.z, 30.0f, [&](void* o) {
            void* vt = *reinterpret_cast<void**>(o);
            if (vt == reinterpret_cast<void*>(0x8D9B7C)) consider(o);  // a field
            if (vt != reinterpret_cast<void*>(0x8F7030)) return;       // a store
            for (int off = 0xC0; off < 0x100; off += 4) {
                void* pile = *reinterpret_cast<void**>(static_cast<char*>(o) + off);
                if (!pile || IsBadReadPtr(pile, 4)) continue;
                void* pvt = *reinterpret_cast<void**>(pile);
                if (pvt == reinterpret_cast<void*>(0x8E5D8C) || pvt == reinterpret_cast<void*>(0x93305C) || pvt == reinterpret_cast<void*>(0x9338D4))
                    consider(pile);
            }
        });
    }
    // A scaffold carried in the hand touching another one it can join (the game's own test): letting
    // go joins them (see the release), as dropping one onto the other does in the flat game. The
    // cursor goes onto that one meanwhile, and the game's hand with it. (A hand that carries
    // otherwise keeps the cursor on the ground below the palm.)
    g_apply_target = nullptr;
    static void* apply_last = nullptr;  // (see below)
    static double apply_last_at = -10;
    if (g_set.combine_by_touch && g_near.on && g_near.blend >= 1.0f && game_hand_state() == kHandStateHolding) {
        void* held = object_in_hand();
        Vec3 hc;
        float hr = 0;
        // (Not only scaffolds: anything the game would apply the held thing to, dropped onto it, such
        // as a sheep or a villager onto the temple's altar, the worship site, to be turned into
        // power; player, 2026-10-28. Not the creature: given things go by the giving, below.)
        if (held && bounding_sphere(held, nullptr, hc, hr)) {
            float best = 1e9f;
            const bool scaffold = is_scaffold(held);
            const auto consider = [&](void* o) {
                if (!o || o == held || IsBadReadPtr(o, 4) || strstr(class_name(o), "Creature")) return;
                Vec3 oc;
                float r = 0;
                if (!bounding_sphere(o, nullptr, oc, r)) return;
                // (The held one is drawn at the palm: touching when the spheres about meet.)
                float d = vr::length(g_near.palm - oc) - r - hr * 0.5f;
                // Held over it counts too, as the flat game's cursor over it: the palm above its
                // middle, horizontally within the two radii, at most join_over above its top. (A
                // scaffold held over another was let go "onto nothing" each time; player, 2026-10-30.)
                const float flat = std::sqrt((g_near.palm.x - oc.x) * (g_near.palm.x - oc.x) + (g_near.palm.z - oc.z) * (g_near.palm.z - oc.z));
                if (flat < r + hr && g_near.palm.y > oc.y && g_near.palm.y < oc.y + r + hr + g_set.join_over_m * wf.scale)
                    d = std::min(d, flat - r - hr);
                if (d >= g_set.touch_margin_m * wf.scale) return;
                // (A held scaffold: another scaffold first, then anything else it goes onto, such as a
                // building being built from a blueprint put down earlier, which takes its wood.)
                const float key = d + (scaffold && !is_scaffold(o) ? 1000.0f : 0.0f);
                if (key >= best || !applies_to(held, o)) return;
                best = key, g_apply_target = o;
                if (scaffold && is_scaffold(o)) cursor_point = oc;  // (a big target's centre, such as the worship site's, may be far off)
            };
            for_each_object_near(g_near.palm.x, g_near.palm.z, scaffold ? 20.0f : 60.0f, [&](void* o) {
                consider(o);
                // (The altar, a WorshipTotem, isn't in the map's cells: its worship site holds it, +0xDC.)
                if (!scaffold && strcmp(class_name(o), ".?AVWorshipSite@@") == 0 && !IsBadReadPtr(static_cast<char*>(o) + 0xDC, 4))
                    consider(*reinterpret_cast<void**>(static_cast<char*>(o) + 0xDC));
            });
        }
        // (Kept half a second after the touch ends: a hand drifts a few centimetres as it lets go,
        // and two scaffolds the player had touched together were thrown apart instead; 2026-10-28.)
        if (g_apply_target) {
            if (g_apply_target != apply_last) {  // (a light buzz: letting go now joins them, or puts it on the altar)
                BWLOG("hands: %s in the hand touches %s, which it applies to (%p)", class_name(held), class_name(g_apply_target), g_apply_target);
                vr::openvr::haptic(g_hand, 0.015f, 150.0f, 0.35f);
            }
            apply_last = g_apply_target, apply_last_at = now_s();
        }
    }
    if (!g_apply_target && apply_last) {
        void* held = game_hand_state() == kHandStateHolding ? object_in_hand() : nullptr;
        if (now_s() - apply_last_at < 0.5 && held && !IsBadReadPtr(apply_last, 4) && applies_to(held, apply_last)) {
            g_apply_target = apply_last;
        } else {
            const auto at = [](void* o, int off) { return o && !IsBadReadPtr(static_cast<char*>(o) + off, 4) ? *reinterpret_cast<int*>(static_cast<char*>(o) + off) : -2; };
            BWLOG("hands: %s in the hand no longer touches %s (near %d, blend %.2f, hand state %u, %.2f s; +0x90 %d / %d, +0x68 %x / %x)", held ? class_name(held) : "-",
                  class_name(apply_last), g_near.on, g_near.blend, game_hand_state(), now_s() - apply_last_at, at(held, 0x90), at(apply_last, 0x90),
                  at(held, 0x68), at(apply_last, 0x68));
            apply_last = nullptr;
        }
    }
    {   // A bubble being clicked to pop it: the cursor on it.
        Vec3 c;
        float r = 0;
        if (g_pop_aim && bounding_sphere(g_pop_aim, nullptr, c, r)) cursor_point = c;
    }
    {   // In a fight, while the action button is held, the cursor stays where the press began: the
        // game gives a new order wherever the held cursor moves, and a real hand always drifts (the
        // trigger held on the ground tapped it over and over; player, 2026-10-24). A hold still is
        // a hold, only in one place.
        static bool pinned = false;
        static Vec3 pin;
        if (g_fight && g_rmb && g_set.fight_pin) {
            if (!pinned) pin = cursor_point, pinned = true;
            cursor_point = pin;
        } else {
            pinned = false;
        }
    }
    {   // The village totem in the hand (hand state 5) goes up and down with the hand: its level is
        // set from how far the hand has risen since it was taken (see totem_select_detour). The
        // cursor stays where it was meanwhile. (On the ground below the palm, as for anything
        // held, only moving the hand forward and back did it; then, with the cursor raised with the
        // hand, it wobbled after it: the game chases the cursor on the screen; player, 2026-10-27.)
        static bool on = false;
        static Vec3 anchor;
        static float palm_y0 = 0;
        if (g_near.on && g_near.blend >= 1.0f && game_hand_state() == 5 && g_set.totem_range_m > 0) {
            if (!on) anchor = cursor_point, palm_y0 = g_near.palm.y, on = true, ++g_totem.grab;
            cursor_point = anchor;
            g_totem.rise_m = (g_near.palm.y - palm_y0) / std::max(1e-3f, wf.scale);
            g_totem.t = now_s();
            steer_totem();
        } else {
            on = false;
        }
    }
    if (g_shield_draw) g_shield_pen = cursor_point;
    if (g_shield_draw && now_s() - g_shield_down_at > 0.15) g_shield_pts.push_back(cursor_point);
    if (g_shield_draw) g_gd.trail.push_back({now_s(), cursor_point});  // (the shield's circle: the laser's point)
    if (g_gd.on) {  // drawing a gesture: the pen is the fingertip, or the laser's point
        Vec3 pen = cursor_point;
        if (g_near.on && g_near.blend >= 1.0f) pen = cursor_point = g_near.palm + g_near.z * (0.09f * wf.scale);
        g_gd.trail.push_back({now_s(), pen});
    }
    while (!g_gd.trail.empty() && now_s() - g_gd.trail.front().first > 2.0) g_gd.trail.pop_front();
    {   // The game takes the cursor's moves (for gestures: GInterface's mouse-move message, which
        // feeds the samples) only from its mouse handler, on the real mouse's events
        // (GGame::MouseHandler -> CMouse::ProcessPosition, 0x61A110, every 28 ms of them). The VR
        // cursor makes none: gestures worked only while the desk mouse happened to give the game
        // events, and stopped for whole sessions (player, 2026-10-27). In VR the mod sends the
        // position the same way, 35 times a second.
        static double last = -10;
        if (now_s() - last > 1.0 / 35.0 && game::game_instance()) {
            last = now_s();
            reinterpret_cast<void(__thiscall*)(void*)>(0x61A110)(static_cast<char*>(game::game_instance()) + 0x2502B8);
        }
    }
    // Throws: the motion of the laser point, or in hand mode of the hand itself (the ground
    // point below it has no vertical motion).
    // (Relative to the rig: moving with the stick isn't the hand's motion. Taken from the world,
    // a thing let go of just after the stick stopped flew off with the rig's speed.)
    g_hit_history.push_back({now_s(), (g_near.on ? g_ptr.origin : cursor_point) - g_rig.pos});
    while (!g_hit_history.empty() && g_hit_history.front().t < g_hit_history.back().t - 0.5)
        g_hit_history.erase(g_hit_history.begin());
    if (g_gg.st == GravityGlove::kPulling) cursor_point = g_gg.c;  // (the gravity gloves' pull: the game's hand goes to it)
    float sx, sy, Z;
    // In a cutscene the hand is the game's: the player has no control of it at all (player,
    // 2026-10-21). When the hand shows in a scene, the game is moving it to demonstrate a tutorial
    // action (a silver scroll's throwing lesson); the laser had fed the game's mouse against it,
    // then while the grip was held, then while the laser moved. Now neither the laser on the
    // screen nor the hand in the world moves the game's mouse, and the grip is ignored (see
    // `c`); the trigger still clicks where the game's mouse is, to click through the scene's text.
    // The temple and other views shown on the screen are the player's throughout.
    const bool cinema_idle = g_cutscene;
    if (g_ui_pointing && cinema_idle) {
        g_ptr.cursor_x = g_ui_x, g_ptr.cursor_y = g_ui_y;  // (the laser's spot, for drawing; the game's mouse stays)
    } else if (g_ui_pointing) {
        // 2D takes the mouse position in its own layout pixels.
        g_ptr.cursor_x = g_ui_x, g_ptr.cursor_y = g_ui_y;
        g_ptr.cursor_ok = true;
        input::set_vr_cursor(true, g_ptr.cursor_x, g_ptr.cursor_y);
        push_cursor(g_ptr.cursor_x, g_ptr.cursor_y);
        g_cursor_on = true;
    } else if (!g_cutscene && project(pick_eye, cursor_point, sx, sy, Z)) {
        g_ptr.cursor_x = std::lround(sx);
        g_ptr.cursor_y = std::lround(sy);
        g_ptr.cursor_ok = true;
        input::set_vr_cursor(true, g_ptr.cursor_x, g_ptr.cursor_y);
        push_cursor(g_ptr.cursor_x, g_ptr.cursor_y);
        g_cursor_on = true;
    }
    // Buttons, with hysteresis on the analog inputs.
    // B&W: the left button holds the land (move), the right button picks up / acts / throws.
    // Grip = right button, in both modes (hold to carry, let go to drop or throw). Trigger = left
    // button, with the laser only. The hand at the controller grabs only within reach of the
    // ground, and not while it is still arriving.
    // Tapping (the game's action-button click on things like the tutorial's scrolls) is the
    // trigger, in both modes: with the laser on the thing, or with the hand touching it. (Touch
    // alone doesn't tap: the player didn't want scrolls set off by brushing them.)
    // With the creature on the leash, the trigger on a thing sends the creature to it instead
    // (rocks, for one, are tappable: a tap would pick the rock up).
    void* const leash_target = !g_fight && !g_ui_pointing && hand_free(game_hand_state()) ? leash_target_under_hand() : nullptr;
    const bool tappable = leash_target || (!g_fight && g_set.tap && !g_ui_pointing && hand_free(game_hand_state()) && tappable_under_hand());
    g_tappable = tappable;
    g_leash_target = leash_target != nullptr;
    // The trigger on the player's own creature (the palm on it, or the laser) puts the leash on
    // or takes it off: the flat game's click on it. (The hand on it alone strokes, as before.)
    bool on_own_creature = false;
    if (!g_fight && !g_ui_pointing && (hand_free(game_hand_state()) || game_hand_state() == 7)) {
        void* own = own_creature();
        Vec3 cc;
        float cr = 0;
        if (g_near.on)
            on_own_creature = own && bounding_sphere(own, nullptr, cc, cr) &&
                              vr::length(g_near.palm - cc) < std::max(cr, g_set.touch_min_m * g_rig_scale) + g_set.give_margin_m * g_rig_scale;
        else if (own) {
            // The laser: the picker's choice, or the beam passing through the creature's sphere
            // before it reaches the land (with the leash on the picker doesn't report it).
            on_own_creature = g_picked == own;
            if (!on_own_creature && bounding_sphere(own, nullptr, cc, cr)) {
                const float t = vr::dot(cc - g_ptr.origin, g_ptr.dir);
                const float to_hit = vr::length(g_ptr.hit - g_ptr.origin);
                on_own_creature = t > 0 && t < to_hit + cr && vr::length(g_ptr.origin + g_ptr.dir * t - cc) < cr;
            }
        }
    }
    if (g_follow_creature) {  // (debug; takes effect next frame)
        Vec3 fc;
        float fr = 0;
        if (void* own = own_creature(); own && bounding_sphere(own, nullptr, fc, fr)) g_rig.pos = g_rig.pos + (fc - g_near.palm);
    }
    g_on_own_creature = on_own_creature;
    static bool trigger = false, trigger_taps = false, trigger_leash = false, trigger_creature = false;
    bool creature_click = false;
    const bool trigger_was = trigger;
    const bool pulled = trigger ? c.trigger > g_set.trigger_up : c.trigger > g_set.trigger_down;
    // (With the hand at the controller the trigger exists only for tapping what the hand touches.)
    {
        static bool pulled_was = false;
        if (pulled && !pulled_was) {
            Vec3 dc{};
            float dr = 0;
            if (void* own = own_creature()) bounding_sphere(own, nullptr, dc, dr);
            BWLOG("hands: trigger pulled: near %d, ui %d, hand state %u, own creature %d (palm %.1f from its centre, r %.1f), tappable %d, touch %.2f s ago",
                  g_near.on, g_ui_pointing, game_hand_state(), on_own_creature, vr::length(g_near.palm - dc), dr, tappable, now_s() - g_touch.t);
            if (g_ui_pointing) input::menu_click_check(g_ui_x, g_ui_y);
        }
        pulled_was = pulled;
    }
    // (A tappable thing the game's picker has settled on taps, touched or not: the game shows its
    // word and the trigger's name by the hand then, so the trigger has to do it. Brushing it
    // still doesn't.)
    // Miracles. A one-shot miracle comes as a bubble (OneOffSpellSeed). In the flat game a quick
    // click of the pick-up button on the bubble pops it: the hand goes to its charged state
    // (CHand state 8, HandStateGrain: CHand::GetRequiredState picks it when the hand holds a
    // thing with a spell seed), and the miracle is cast from there.
    //  - The trigger on a bubble the hand is on (or the laser) makes that quick click.
    //  - The trigger while holding a bubble lets go of it, then clicks it.
    //  - The trigger while charged casts at what the hand is on: the player's creature for a
    //    creature's miracle (strength and the like are poured onto it).
    // A pull that did one of these is spent.
    static bool pull_spent = false;
    {
        static PendingPop bubble_press;  // a trigger press on a bubble: a tap pops it, a hold picks it up
        static bool pulled_before = false;
        const uint32_t hs_now = game_hand_state();
        if (pulled && !pulled_before && (hs_now == kHandStateHolding || hs_now == kHandStateCharged) && g_game_hand &&
            g_set.pop_with_trigger) {
            void* status = *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850);
            void* held = status ? *reinterpret_cast<void**>(static_cast<char*>(status) + 0x120) : nullptr;
            void* gi = game_interface();
            if (hs_now == kHandStateCharged && gi) {
                // A miracle in the magic hand, poured (applied) onto what the hand is on: the
                // player's creature for a creature's miracle. fn_005D30D0 applies the power-up in
                // the hand to GInterface +0x400 (the picker's object) if it can be (packet 0x11);
                // the creature isn't the picker's (the cursor is below a full hand, and a creature
                // on the leash never reports), so it is put there for the call.
                void* own = own_creature();
                Vec3 oc;
                float orad = 0;
                const bool on_creature = own && bounding_sphere(own, nullptr, oc, orad) &&
                                         vr::length(g_near.palm - oc) < orad + g_set.give_margin_m * g_rig_scale;
                void* target = on_creature ? own : g_picked;
                int valid = -1, seed_type = -1;
                {   // (Logged: what the game holds in the magic hand, and whether it would apply to the target.)
                    using FirstFn = void*(__thiscall*)(void*);
                    void* in_hand = reinterpret_cast<FirstFn>(0x5DC130)(status);
                    using SeedFn = void*(__thiscall*)(void*);
                    const auto seed_fn = reinterpret_cast<SeedFn>(game_method(in_hand, 0x4C4));
                    void* seed = seed_fn ? seed_fn(in_hand) : nullptr;
                    int type = -1;
                    if (seed) {
                        using InfoFn = void*(__thiscall*)(void*);
                        void* info = reinterpret_cast<InfoFn>(0x7290E0)(seed);
                        type = reinterpret_cast<int(__cdecl*)(void*)>(0x72B200)(info);
                    }
                    seed_type = type;
                    if (in_hand && target) {
                        using ValidFn = int(__thiscall*)(void*, void*, void*);
                        if (const auto fn = reinterpret_cast<ValidFn>(game_method(in_hand, 0x71C))) valid = fn(in_hand, status, target);
                    }
                    BWLOG("hands: magic hand holds %s (seed %s, type %d), applies to %s: %d", in_hand ? class_name(in_hand) : "-",
                          seed ? class_name(seed) : "-", type, target ? class_name(target) : "-", valid);
                }
                if (is_shield_type(seed_type)) {
                    g_pouring = true, g_shield_draw = true;
                    const auto* game = static_cast<const char*>(game::game_instance());
                    char* buf = game ? *reinterpret_cast<char* const*>(game + 0x25006C) : nullptr;
                    if (buf && !IsBadWritePtr(buf, 0xC94)) {
                        buf[0xC88] = 0, buf[0xC90] = 0, *reinterpret_cast<uint32_t*>(buf + 0xC8C) = 0;
                        memset(buf + 8, 0, 0x320 * 4);
                    }
                    g_gd.trail.clear(), g_shield_pts.clear(), g_shield_down_at = now_s();
                    BWLOG("hands: trigger with the shield in the hand: drawing its circle with the laser while held");
                } else if (seed_type == 6 && g_set.zap_aim) {
                    g_pouring = true, g_zap = true;
                    BWLOG("hands: trigger with lightning in the hand: the bolts go where the controller points while held");
                } else if (target && valid == 1) {
                    void*& picked = *reinterpret_cast<void**>(static_cast<char*>(gi) + 0x400);
                    void* saved = picked;
                    picked = target;
                    using ApplyFn = void(__thiscall*)(void*);
                    reinterpret_cast<ApplyFn>(0x5D30D0)(gi);
                    picked = saved;
                    BWLOG("hands: trigger with a miracle in the hand: applied to %s", class_name(target));
                    vr::openvr::haptic(g_hand, 0.03f, 200.0f, 0.6f);
                } else {
                    // An area miracle (food, wood, water: poured on the land or a store while the
                    // action button is held, as the grip did with the laser): the trigger holds the
                    // game's button while it is held, with the game's cursor on the land below the
                    // hand. In hand mode the grip couldn't (it presses only near the ground), so a
                    // food miracle couldn't be cast (player, 2026-10-23).
                    g_pouring = true;
                    BWLOG("hands: trigger with a miracle in the hand: pouring while held (%s)", target ? class_name(target) : "the land");
                }
                pull_spent = true;
            } else if (held && gi && strcmp(class_name(held), ".?AVOneOffSpellSeed@@") == 0) {
                // (The magic hand takes it only from an empty hand: GInterface::PlaceObjectInMagicHand
                // checks IsSpaceInHands. So the hand lets go of it first, and pops it once free.)
                g_pop_after_release = {held, now_s()};
                g_let_go_for_pop = true;
                pull_spent = true;
                BWLOG("hands: trigger while holding a miracle's bubble: letting go of it to pop it");
            }
        } else if (pulled && !pulled_before && hand_free(game_hand_state()) && g_set.pop_with_trigger) {
            // A bubble the hand is on (or the laser): popped straight into the hand. (The tap's
            // press would start picking it up instead: the game picks up on the button going down.)
            void* on = now_s() - g_touch.t < 0.1 ? g_touch.object : g_picked;
            if (on && strcmp(class_name(on), ".?AVOneOffSpellSeed@@") == 0 && g_drop == DropClick::kIdle && !g_rmb) {
                pull_spent = true;
                if (g_set.trigger_carries) {
                    // (A tap pops it, a hold picks it up, as for everything else: decided below.)
                    bubble_press = {on, now_s()};
                } else {
                    g_drop = DropClick::kDown, g_drop_at = GetTickCount64();
                    g_pop_click = {on, now_s()};
                    BWLOG("hands: trigger on a miracle's bubble: clicking it");
                }
            }
        }
        if (bubble_press.object) {
            if (!pulled && now_s() - bubble_press.t < g_set.tap_s) {
                if (g_drop == DropClick::kIdle && !g_rmb) {
                    g_drop = DropClick::kDown, g_drop_at = GetTickCount64();
                    g_pop_click = {bubble_press.object, now_s()};
                    BWLOG("hands: trigger tapped on a miracle's bubble: clicking it");
                }
                bubble_press = {};
            } else if (pulled && now_s() - bubble_press.t >= g_set.tap_s) {
                g_trigger_grab = true;
                BWLOG("hands: trigger held on a miracle's bubble: it is picked up");
                bubble_press = {};
            } else if (!pulled) {
                bubble_press = {};
            }
        }
        if (!pulled) pull_spent = false, g_pouring = false;
        if (g_zap && (!pulled || game_hand_state() != kHandStateCharged)) {
            BWLOG("hands: lightning: %s", pulled ? "spent" : "the trigger let go of");
            g_zap = false, g_pouring = false;
        }
        // (The shield: the trigger holds the right button, the game's Action press, as for pouring.
        // Held, the game starts the circle's recording itself (interface state 8, the 'Cast Circle'
        // hint), recognises the circle (action 4) and casts it on the release.)
        // (The circle closed, the game having recognised it: the button goes up and the game casts
        // it, the trigger still held, as the flat game's shield goes up as the circle is closed.
        // The trigger then does nothing more until let go of.)
        const char* gi_now = static_cast<const char*>(game_interface());
        const bool closed = g_shield_draw && g_set.shield_autocast && gi_now && gi_now[0x17C];
        if (g_shield_draw && (!pulled || closed || game_hand_state() != kHandStateCharged)) {
            BWLOG("hands: the shield's circle ends (%s)", game_hand_state() != kHandStateCharged ? "cast" : closed ? "closed: cast" : "let go of");
            if (closed && pulled) pull_spent = true;
            if (const char* gib = static_cast<const char*>(game_interface())) {  // (where the game will cast it, against the laser's trail)
                Vec3 c{};
                for (const auto& p : g_gd.trail) c = c + p.second;
                if (!g_gd.trail.empty()) c = c * (1.0f / float(g_gd.trail.size()));
                float r = 0;
                for (const auto& p : g_gd.trail) r += vr::length(Vec3{p.second.x - c.x, 0, p.second.z - c.z});
                if (!g_gd.trail.empty()) r /= float(g_gd.trail.size());
                const float* gc = reinterpret_cast<const float*>(gib + 0x180);
                BWLOG("hands: the shield's circle: the game's (recognised %d) centre (%.1f, %.1f, %.1f) size %.1f; the laser's trail "
                      "(%zu points, 2 s) centre (%.1f, %.1f, %.1f) radius %.1f; the hand's point (%.1f, %.1f, %.1f)",
                      gib[0x17C], gc[0], gc[1], gc[2], *reinterpret_cast<const float*>(gib + 0x18C), g_gd.trail.size(), c.x, c.y, c.z, r,
                      g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z);
            }
            g_shield_draw = false, g_pouring = false;
            if (game_hand_state() == kHandStateCharged) g_shield_tail_until = now_s() + 0.6;
        }
        if (g_shield_tail_until > 0 && game_hand_state() != kHandStateCharged) {
            BWLOG("hands: the shield is cast (%.2f s after the button went up)", 0.6 - (g_shield_tail_until - now_s()));
            g_shield_tail_until = -10;
        } else if (g_shield_tail_until > 0 && now_s() > g_shield_tail_until) {
            g_shield_tail_until = -10;
        }
        pulled_before = pulled;
    }
    // (A caption left up with the "trigger - continue" prompt: the trigger is the game's click, to
    // continue, even with the hand on the creature; player, 2026-11-02: it toggled the leash.)
    // (Not with the palm on something to tap: at the creature choice the game asks to tap the one
    // chosen again to confirm, with its words left up, and the prompt took that second tap;
    // 2026-11-02. On the player's own creature it still continues: there it was the leash.)
    const bool to_continue = ui::continue_prompt_up() && !g_fight && !tappable;
    trigger = pulled && !pull_spent &&
              (g_fight || !g_near.on || to_continue || trigger_taps || trigger_leash || trigger_creature || on_own_creature || tappable);
    if (trigger && !trigger_was) {
        trigger_leash = !to_continue && leash_target != nullptr;
        trigger_creature = !to_continue && !trigger_leash && on_own_creature;
        trigger_taps = !to_continue && tappable && !trigger_leash && !trigger_creature;
        if (to_continue) BWLOG("hands: the trigger continues (the caption's prompt)");
        if (trigger_taps) BWLOG("hands: tap (%s) on %s", g_near.on ? "hand" : "laser", g_picked ? class_name(g_picked) : "-"), g_last_tap_at = now_s();
        // A singing stone: its script asks for the last thing clicked (GInterface +0x45C), which
        // the tap's click didn't set on a stone; the game's own setter (fn_005D36D0) records it.
        if (trigger_taps && g_picked && !IsBadReadPtr(g_picked, 4) && *reinterpret_cast<void**>(g_picked) == reinterpret_cast<void*>(0x8E80B4)) {
            if (void* gi = game_interface()) {
                reinterpret_cast<void(__thiscall*)(void*, void*)>(0x5D36D0)(gi, g_picked);
                BWLOG("hands: tap: %p recorded as the thing clicked", g_picked);
            }
        }
        // (With the leash: a quick tap sends the creature to the thing, held it picks the thing up:
        // the sending waits for the trigger's release, below.)
        if (trigger_leash) g_leash_tap = {leash_target, now_s()};
        creature_click = trigger_creature;
        Vec3 dc{};
        float dr = 0;
        if (void* own = own_creature()) bounding_sphere(own, nullptr, dc, dr);
        BWLOG("hands: trigger (%s): own creature %d, leash target %p, tappable %d, picked %p; creature (%.1f, %.1f, %.1f) r %.1f, "
              "beam (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)", g_near.on ? "hand" : "laser", on_own_creature, leash_target, tappable, g_picked,
              dc.x, dc.y, dc.z, dr, g_ptr.origin.x, g_ptr.origin.y, g_ptr.origin.z, g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z);
    }
    // With the creature on the leash, a quick trigger tap on the land (let go within a quarter of
    // a second) sends the creature there.
    //  - The laser: the spot it is on (and the beam must not have moved: the pull also takes hold
    //    of the land, and holding it drags as before).
    //  - The hand at the controller, on nothing it could tap: the ground below it when it is
    //    within reach of the ground, else where the controller points on the land.
    {
        static bool land_tap = false, pulled_before = false;
        static double tap_at = 0;
        static Vec3 tap_point, tap_dir;
        const bool hand = g_near.on && g_near.blend >= 1.0f;
        if (pulled && !pulled_before) {
            land_tap = false;
            if (!g_fight && g_set.leash_land_tap && !g_ui_pointing && !trigger_taps && !trigger_leash && !trigger_creature && leash_in_hand()) {
                if (!g_near.on && g_ptr.hit_land) land_tap = true, tap_point = g_ptr.hit;
                else if (hand && !trigger && g_near.in_reach) land_tap = true, tap_point = g_near.ground;
                else if (hand && !trigger && g_ptr.hit_land) land_tap = true, tap_point = g_ptr.hit;
            }
            tap_at = now_s(), tap_dir = g_ptr.dir;
        }
        if (!pulled && pulled_before && land_tap) {
            if (now_s() - tap_at < 0.25 && (g_near.on || vr::dot(tap_dir, g_ptr.dir) > 0.9986f))  // (laser: within 3 degrees)
                leash_walk_to(tap_point);
            land_tap = false;
        }
        pulled_before = pulled;
    }
    // The leash's tap: let go within a quarter of a second (and not turned into a pick-up).
    if (!pulled && g_leash_tap.target) {
        if (now_s() - g_leash_tap.at < g_set.tap_s && !g_trigger_grab) {
            // A second tap on the same thing within half a second ties the leash to it.
            static void* last_target = nullptr;
            static double last_at = -10;
            const double t = now_s();
            if (g_leash_tap.target == last_target && t - last_at < 0.5 + g_set.tap_s) {
                leash_tie(g_leash_tap.target);
                last_target = nullptr;
            } else {
                leash_direct(g_leash_tap.target);
                last_target = g_leash_tap.target, last_at = t;
            }
        }
        g_leash_tap = {};
    }
    if (!trigger) trigger_taps = trigger_leash = trigger_creature = false;
    // (In a fight the trigger is the game's action button, as the grip: pressed again and again on
    // the enemy it queues strikes, which the other button didn't; player, 2026-10-23.)
    // The trigger picks up and carries, as the grip does on controllers that have one (player,
    // 2026-10-23: the controls are designed for a trigger alone; the grip is a shortcut). It is the
    // game's action button, which itself tells a click (tap) from a hold (pick up):
    //  - the hand at the controller, where the grip could press (touching something, or within
    //    reach of the ground);
    //  - the laser on something the hand can lift (on bare land it still drags the land);
    //  - with the leash, held on the thing past a quick tap (a tap sends the creature).
    // Not with both triggers held (the two-handed world grab), on the menus or screen, or while
    // the trigger does something else (a miracle, the creature, a fight).
    {
        const Controller& o = ctl[1 - g_set.dominant];
        const bool other_trigger = o.active && o.trigger > g_set.trigger_down;
        // (The laser: something liftable under it now or a moment ago, by the mod's picker or the
        // game's own interface object (GInterface +0x400): the picker reports only now and then,
        // and a press between reports dragged the land instead.)
        static double liftable_at = -10;
        if (!g_near.on) {
            void* gi_obj = nullptr;
            if (const char* gi = static_cast<const char*>(game_interface())) gi_obj = *reinterpret_cast<void* const*>(gi + 0x400);
            if ((g_picked && liftable(g_picked)) || (gi_obj && liftable(gi_obj))) liftable_at = now_s();
        }
        const bool hand_ok = g_near.on && g_near.blend >= 1.0f && (g_near.in_reach || now_s() - g_touch.t < 0.1);
        // (With the laser: something liftable under it now or a moment ago, by the mod's picker or
        // the game's interface object, or the game asking for its "Pick Up" word: the picker
        // reports only now and then.)
        const bool laser_ok = !g_near.on && !g_ui_pointing && (now_s() - liftable_at < 0.25 || ui::seconds_since_pick_up_word() < 0.3);
        const bool held_long = g_leash_tap.target && now_s() - g_leash_tap.at >= g_set.tap_s && liftable(g_leash_tap.target);
        // (Only once the trigger has been held past a tap: the game picks a tree up the moment its
        // button goes down, so a quick tap would uproot it. A tap on such a thing does nothing.)
        static double pull_since = -10;
        static bool pulled_was = false;
        if (pulled && !pulled_was) pull_since = now_s();
        pulled_was = pulled;
        const bool past_tap = now_s() - pull_since >= g_set.tap_s;
        g_trigger_past_tap = past_tap;
        const bool can = g_set.trigger_carries && pulled && !pull_spent && !g_ui_pointing && !g_fight && !other_trigger && !to_continue &&
                         !trigger_creature && !trigger_taps && !g_cinema && past_tap && (g_leash_tap.target ? held_long : (hand_ok || laser_ok));
        if (!g_trigger_grab && can && game_hand_state() != kHandStateHolding) {
            g_trigger_grab = true;
            BWLOG("hands: the trigger picks up (%s)", g_near.on ? "hand" : "laser");
        }

    }
    set_lmb((trigger && !g_fight && !g_trigger_grab && !trigger_taps && !trigger_leash && !trigger_creature &&
             // (The wait for a tap to pass is for the laser on the world only: on a menu, the
             // cinema screen or in a scene the trigger clicks at once. A cutscene's "continue"
             // couldn't be clicked; player, 2026-10-24.)
             (g_near.on || g_ui_pointing || g_cinema || g_cutscene || !g_set.trigger_carries || g_trigger_past_tap)) ||
            now_s() < g_dbg_lmb_until);
    const bool was_rmb = g_rmb;
    // The hand at the controller grabs what it touches, or failing that what is below it within
    // reach of the ground.
    const bool touching = g_near.on && now_s() - g_touch.t < 0.1;
    static bool was_touching = false;
    if (touching && !was_touching && !g_rmb) vr::openvr::haptic(g_hand, 0.01f, 220.0f, 0.25f);
    was_touching = touching;
    {   // (Over a field or a pile high up, in reach of a scoop from above: the same light tick, since
        // nothing else says so; player, 2026-11-08: "not always clear when I'm close enough".)
        static bool was_over = false;
        static double checked = -10;
        static bool over = false;
        if (now_s() - checked > 0.1) {
            checked = now_s();
            const float above = g_near.palm.y - std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
            over = g_hand_picks && !touching && !g_rmb && above > 1.0f * g_rig_scale && scoop_below_palm() != nullptr;
        }
        if (over && !was_over) vr::openvr::haptic(g_hand, 0.01f, 220.0f, 0.25f);
        was_over = over;
    }
    // (A person, animal or tree within the grip's wider reach counts as something to press on: see
    // the grip's catch below. Looked for only as the grip closes.)
    bool living_near = false;
    void* scoop_above = nullptr;  // (a field or a pile whose column the palm is in, high over it)
    {
        static bool squeezed_before = false;
        const bool squeezed_now = c.squeeze > g_set.squeeze_down;
        if (squeezed_now && !squeezed_before && !(touching && living_or_tree(g_touch.object)) && g_hand_picks && g_set.living_grip_m > 0)
            living_near = nearest_living(g_set.living_grip_m * g_rig_scale) != nullptr;
        if (squeezed_now && !squeezed_before && !touching && !living_near && g_hand_picks) scoop_above = scoop_below_palm();
        squeezed_before = squeezed_now;
    }
    const bool can_press = g_fight || !g_near.on || ((g_near.in_reach || touching || living_near || scoop_above) && g_near.blend >= 1.0f) ||
                           game_hand_state() == kHandStateHolding;
    static bool grip = false;
    {   // (Logged: grips with the hand at the controller, against the nearest thing in flight, to see
        // why catching things in motion is hard.)
        static bool squeezed_was = false;
        const bool squeezed = c.squeeze > g_set.squeeze_down;
        if (squeezed && !squeezed_was && g_near.on && game_hand_state() != kHandStateHolding) {
            const char* list = *reinterpret_cast<const char* const*>(0xD47814);  // PhysicsObject array, 0x1DC each
            const int n = *reinterpret_cast<const int*>(0xD47818);
            float best = 1e9f;
            const char* best_obj = nullptr;
            float best_r = 0;
            for (int i = 0; list && i < n && i < 256 && !IsBadReadPtr(list + i * 0x1DC, 0x1DC); ++i) {
                void* obj = *reinterpret_cast<void* const*>(list + i * 0x1DC + 0x18);
                Vec3 cc;
                float r = 0;
                if (obj && bounding_sphere(obj, nullptr, cc, r) && vr::length(g_near.palm - cc) - r < best)
                    best = vr::length(g_near.palm - cc) - r, best_obj = static_cast<const char*>(obj), best_r = r;
            }
            BWLOG("hands: grip with the hand: touching %d (%s), can press %d, in reach %d; nearest in flight: %s %.2f m outside its sphere (r %.2f m)",
                  touching, touching ? class_name(g_touch.object) : "-", can_press, g_near.in_reach, best_obj ? class_name(best_obj) : "none",
                  best / g_rig_scale, best_r / g_rig_scale);
            {   // (what the game would act on now: its picker's choice, and the interface's object, GInterface +0x400)
                const char* gi = static_cast<const char*>(game_interface());
                const void* under = gi && !IsBadReadPtr(gi + 0x400, 4) ? *reinterpret_cast<const void* const*>(gi + 0x400) : nullptr;
                BWLOG("hands: grip: leash in the hand %d (tied %d)", leash_in_hand(), leash_tied());
                BWLOG("hands: grip: the picker's choice %s %p, the game's object %s %p, armed %d", g_picked ? class_name(g_picked) : "-", g_picked,
                      under ? class_name(under) : "-", under, g_touch_armed);
            }
        }
        squeezed_was = squeezed;
    }
    grip = grip ? c.squeeze > g_set.squeeze_up : can_press && c.squeeze > g_set.squeeze_down;
    if (g_gg.st == GravityGlove::kLocked) grip = false;  // (the gravity gloves' lock: no grab here)
    if (g_hand != g_set.dominant && !g_rmb && touching && g_touch.object && strstr(class_name(g_touch.object), "SpellSeed")) grip = false;  // (the other hand: no miracles)
    if (g_gg.st == GravityGlove::kPulling || g_gg.st == GravityGlove::kFlying) grip = true;  // (held through the pull and the flight)
    if (g_trigger_grab) grip = true;  // (the trigger carrying: as the grip, until it is let go)
    const bool grab_open = c.squeeze < g_set.squeeze_up && !(g_trigger_grab || c.trigger > g_set.trigger_up);
    if (g_scoop_done) {  // (a scoop ended: the game's button is up, the hand carries what it took until the grip opens)
        if (grab_open) {
            g_scoop_done = false;
            if (game_hand_state() == kHandStateHolding && g_drop == DropClick::kIdle)
                g_drop = DropClick::kDown, g_drop_at = GetTickCount64() + 40;  // (put down, as any let go of)
        }
        grip = false;
    }
    if (g_let_go_for_pop) {  // (the grip counts as open until it really is: the bubble is being popped)
        if (grab_open) g_let_go_for_pop = false;
        grip = false;
    }
    if (grip_spent) {  // handed over: the grip is open as far as the game goes, until it really opens
        if (grab_open) grip_spent = false;
        grip = false;
    }
    // Petting's action button. The game starts stroking on the button going down with the
    // creature under its hand, so the press waits until the picker has settled on the creature
    // (the cursor only went onto it when the palm arrived); pressed a frame too soon it is a click
    // on nothing, and held from then on it never becomes one on the creature. If a press didn't
    // take, it is let go and made again.
    static bool pet_press = false;
    static double pet_press_at = -10, pet_release_at = -10;
    {
        const uint32_t hs = game_hand_state();
        const double now = now_s();
        uint32_t ms = 0;
        const bool interacting = interaction_running(ms);
        // The trigger on the creature: stroking stops until the hand has left it, and the leash
        // toggles. If a stroke had begun, letting go of it may itself be the game's click (when
        // the game sees the release within its click time, which depends on when its next turn
        // comes): so the toggle waits for the stroke to end, and is sent only if the leash
        // hasn't changed by then.
        static int toggle_from = -2;  // the leash's state at the trigger, while waiting
        static double toggle_at = 0;
        // A petting press is never let go of within the game's click time, nor before the game has
        // started on it: let go then, the game took it as a click on the creature, its interaction
        // started and ended within a frame or two, and the creature froze (twice, each time just
        // after a petting press cut short by the trigger on it: 2026-11-08, action 0x9B; 2026-11-13,
        // 17 ms after the press, action 0x4E; deaf to the leash until the game was saved and loaded
        // again). It is held until the interaction has run past the click time (or 0.8 s, if the
        // game never starts it), then let go; the leash is toggled by the mod (below).
        const bool press_young = pet_press && (interacting ? ms < kLeashClickMs + 80 : now - pet_press_at < 0.8);
        if (creature_click) {
            pet_wait = true;
            // (A press already down counts, even before the game's camera has started.)
            const bool stroking = pet_press || interacting;
            if (press_young) BWLOG("hands: petting: the trigger on the creature %.2f s into the press: held until the game's interaction is past a click", now - pet_press_at);
            else pet_press = false, pet_release_at = now;
            if (stroking) toggle_from = leash_on_flag(), toggle_at = now;
            else leash_toggle();
        }
        if (toggle_from != -2 && ((!interacting && now - toggle_at > 0.6) || now - toggle_at > 1.5)) {
            if (leash_on_flag() == toggle_from) leash_toggle();
            toggle_from = -2;
        }
        if (creature_click) {
        } else if (!g_petting) {
            // A stroke is never let go within the game's click time: that would put the leash on
            // or take it off (a hand brushing past the creature did).
            if (pet_press && !press_young) pet_press = false, pet_release_at = now;
        } else if (hs == 7) {
            if (!pet_press) pet_press_at = now;
            pet_press = true;
        } else if (!pet_press) {
            // (... and has been for a moment: the game also takes a moment to notice its hand is
            // over the creature, which shows as its "Interact" hint and interaction camera.)
            static double on_since = -1;
            if (!g_picked_creature) on_since = -1;
            else if (on_since < 0) on_since = now;
            if (on_since >= 0 && now - on_since > 0.25 && now - pet_release_at > 0.15) pet_press = true, pet_press_at = now;
            if (pet_press) BWLOG("hands: petting: action button down");
        } else {
            // A press that hasn't become stroking is let go and made again after 1.5 s. But the
            // creature first finishes what it was doing (Creature::GoIntoStateOfFinishingAction
            // sets its +0x3CC; fn_0046CF80 refuses stroking until it clears): letting go then
            // started it over, and stroking took several tries (player, 2026-10-30). While the
            // creature is finishing, the press is held, up to 6 s.
            const char* cr = static_cast<const char*>(own_creature());
            const bool finishing = cr && !IsBadReadPtr(cr + 0x3CC, 4) && *reinterpret_cast<const uint32_t*>(cr + 0x3CC) != 0;
            static bool said = false;
            if (finishing && now - pet_press_at > 1.5 && !said) said = true, BWLOG("hands: petting: the creature is finishing what it was doing; the press is held");
            if (now - pet_press_at > (finishing ? 6.0 : 1.5)) pet_press = false, pet_release_at = now, said = false;
        }
    }
    {
        // With the creature on the leash, the action button sends the creature: a click (let go
        // within the game's ~0.22 s) on a thing sends it to the thing, and a press on bare land
        // sends it there. With the hand at the controller that is the trigger's job (the player
        // didn't want the grip to direct): while a grip there holds the button, the game's own
        // sending is swallowed (send_to_*_detour), and the button is held long enough never to
        // be a click. (Holding back the press itself on "nothing under the hand" also stopped
        // grabbing what lies on the ground just below the hand, which isn't touched.)
        static double rmb_down_at = -10;
        static bool from_grip = false;  // the press now down is the grip's
        const bool leash_hand = g_set.grip_never_directs && g_near.on && leash_in_hand();
        static bool pour_was = false;
        if (pour_was && !g_pouring && game_hand_state() == 8) {
            Vec3 rv;
            // (Aimed by the controller's own velocity where SteamVR gives one: the fastest of the last
            // 0.15 s. The palm's track over the swing pointed off to the side of a forward pitch: the
            // fireball veered left or right; player, 2026-10-26.)
            bool got = false;
            if (g_near.on && !g_ctl_vel.empty()) {
                float best = 0;
                for (const auto& s : g_ctl_vel)
                    if (now_s() - s.t < 0.15 && vr::length(s.v) > best) best = vr::length(s.v), rv = s.v;
                got = best > 0;
            }
            if (!got) got = hit_velocity(rv, g_near.on ? 0.35 : 0.15);
            if (got && vr::length(rv) > g_set.throw_min_speed_m_s * g_rig_scale) {
                g_fling = {now_s(), rv, g_near.blend >= 0.5f ? g_near.palm : g_ptr.hit};
                BWLOG("hands: a charged miracle let go of at speed (%.1f, %.1f, %.1f)", rv.x, rv.y, rv.z);
            }
        }
        pour_was = g_pouring;
        const bool flinging = false;  // (the flick isn't the throw: see Fling)
        bool want = grip || pet_press || (trigger && trigger_taps) || g_pouring || (trigger && g_fight) || flinging;
        g_pet_pressing = pet_press || now_s() - pet_release_at < 0.25;  // (and its release: no sending either)
        if (want && !g_rmb) rmb_down_at = now_s(), from_grip = grip;
        if (!want && g_rmb && from_grip && leash_hand && now_s() - rmb_down_at < 0.3)
            want = true;
        // Let go of too soon: within a click's time of the press, or while the game's hand is still
        // taking what the grip closed on (its state 3; a walking villager took up to 1.8 s). Let go
        // then, the game took it anyway and it stayed in the open hand, a throw lost; under the game's
        // click time (~0.22 s) it was a click, a tap (a rock broke). The button is held until the game
        // has it, then let go, with the hand's speed as the grip opened: a quick catch-and-throw
        // throws, a quick grab-and-drop drops. (Player, 2026-11-13.)
        if (g_pending_release.on && (grip || !g_rmb)) g_pending_release = {};  // (gripped again; or done)
        const uint32_t hs = game_hand_state();
        if (hs != kHandStateHolding) g_holding_since = -1;
        else if (g_holding_since < 0) g_holding_since = now_s();
        if (!want && g_rmb && from_grip && g_near.on && g_hold_through_taking) {
            const double down = now_s() - rmb_down_at;
            // And once the game holds it, the button stays down a moment more (0.1 s): let up in the
            // frame the game took hold, the put-down click after it was ignored and it stayed in the
            // hand; in its first frames, the thing thrown wasn't drawn for 30-50 ms as it left, a
            // blink (harness, 2026-11-13; the player saw it 2026-10-20).
            const bool just_held = hs == kHandStateHolding && (now_s() - g_holding_since < 0.1 || g_prehold.unnamed) && down < 3.0;
            if ((hs == 1 && down < 0.25) || (hs == 3 && down < 3.0) || just_held) {  // (not with a miracle in the hand)
                if (!g_pending_release.on) {
                    g_pending_release.on = true, g_pending_release.t = now_s();
                    g_pending_release.has_v = release_velocity(g_pending_release.v, g_set.throw_min_speed_m_s * g_rig_scale);
                    BWLOG("hands: let go %.2f s after the grip, the game's hand %s: the button held until it has it (hand speed %.2f)", down,
                          hs == 3 ? "still taking it" : hs == kHandStateHolding ? "only just holding it" : "not taking yet",
                          vr::length(g_pending_release.v) / g_rig_scale);
                }
                want = true;
            }
        }
        set_rmb(want);
        {   // (Logged: the first frames of a pick-up by the trigger.)
            static int frames = 0;
            static bool was_grab = false;
            if (g_trigger_grab && !was_grab) frames = 6;
            was_grab = g_trigger_grab;
            if (frames > 0) {
                --frames;
                BWLOG("hands: trigger pick-up: rmb %d lmb %d grip %d want %d, game hand state %u, cursor %d (%ld, %ld)", g_rmb, g_lmb, grip, want,
                      game_hand_state(), g_ptr.cursor_ok, g_ptr.cursor_x, g_ptr.cursor_y);
            }
        }
        // (Until a moment after it is let go: the game acts on the release on its next turn.)
        // (For any grip with the hand, whatever leash_in_hand says: a villager by a store couldn't be picked
        // up with the leash on, every grip sent the creature to the spot instead; player, 2026-11-08. The
        // game only sends with the leash on, so this is the grip never directing.)
        if (g_rmb && from_grip && g_set.grip_never_directs && g_near.on) g_grip_no_send_until = now_s() + 0.25;
    }
    // (Not for petting's own press: the "touched" object then is whatever else the palm is in,
    // near the temple the temple itself, and latching that shut the creature out.)
    if (g_rmb && !was_rmb && touching && g_hand_picks && !g_petting) g_latch = {g_touch.object, now_s()};
    // Nothing touched as the grip closes: the nearest person, animal or tree within a wider reach is
    // taken (playtest 2026-11-01: children running about were taken about 1 in 3; they had moved
    // out of the touch range by the time the grip closed). [hands] living_grip_cm.
    // (And when what is touched is a building, a rock or a field: a person by it is what the hand means.)
    const bool touching_living = touching && living_or_tree(g_touch.object);
    if (g_rmb && !was_rmb && !touching_living && g_hand_picks && !g_petting && g_set.living_grip_m > 0) {
        float d = 0;
        // (On a building, a person a little further off still counts: one sitting by a house 43 cm
        // from the palm wasn't taken, and the house was tapped; player, 2026-11-08.)
        const float reach = touching && !scoopable(g_touch.object) ? std::max(g_set.living_grip_m, 0.5f) : g_set.living_grip_m;
        if (void* o = nearest_living(reach * g_rig_scale, &d)) {
            Vec3 c;
            float r = 0;
            bounding_sphere(o, nullptr, c, r);
            g_latch = {o, now_s(), true};
            g_touch = {now_s(), o, c, r, d};
            BWLOG("hands: grip: nothing touched; %s %.2f m from the palm taken%s", class_name(o), d / g_rig_scale, touching ? " (over what was touched)" : "");
        } else if (void* nearby = nearest_living(1.0f * g_rig_scale, &d)) {
            BWLOG("hands: grip: no person, animal or tree within %.0f cm (the nearest %s %.2f m)", g_set.living_grip_m * 100.0f, class_name(nearby),
                  d / g_rig_scale);
        }
    }
    // Nothing taken as the grip closes, the palm high over a field or a pile (within scoop_height_m
    // of the ground): it is offered to the picker, which doesn't hear of it from up there (the
    // teleport's bird's-eye height; player, 2026-11-07).
    if (g_rmb && !was_rmb && !touching && !g_latch.object && g_hand_picks && !g_petting && (scoop_above || (scoop_above = scoop_below_palm()))) {
        g_latch = {scoop_above, now_s(), true};
        BWLOG("hands: grip: nothing touched; the %s below taken from above (%.1f m over the ground)", class_name(scoop_above),
              (g_near.palm.y - std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z))) / g_rig_scale);
    }
    if (g_petting) g_latch.object = nullptr;
    // (Not when the grip opens for a hand-over: the scoop's "put it down" then dropped the pile the
    // creature was taking, and the game crashed in the creature's state some time later, a bad_cast
    // in Creature::ProcessState; player, 2026-10-31.)
    const bool giving_now = now_s() < g_no_drop_click_until;
    if (giving_now) g_scoop_drop_at = -1;
    const bool let_go_scooping = was_rmb && !g_rmb && game_hand_state() == kHandStateHolding && !g_scoop_done && !giving_now && scoop_pile();
    if (let_go_scooping) {
        g_scoop_drop_at = now_s() + 0.35, g_scoop_drop_tries = 0;
        BWLOG("hands: let go while scooping from the %s: the scoop ends, then it is put down", class_name(scoop_pile()));
    } else if (was_rmb && !g_rmb && game_hand_state() == kHandStateHolding) {
        // The fastest stretch of the swing: the grip reads open a little after the fingers
        // start to, so look further back for the hand than for a laser point.
        Vec3 rv;
        // A scaffold let go of gently over its blueprint: the game puts it down from its own hand,
        // as in the flat game, and builds there. Let go touching another scaffold it joins: the
        // game's own "apply to what the hand is over" with that one (the scaffold on the ground
        // doesn't report to the game's picker while the hand holds another, so the release alone
        // only dropped it). Thrown, it flies from the hand as anything does.
        bool to_game = false, applied = false, plant_tree = false;
        // Let go of high up (above disciple_max_m, the "village management" height): anything falls, as
        // thrown; below it a person, an animal, a tree or a scaffold is set down on the ground there
        // (player, 2026-11-08: setting down near the ground is a VR comfort; from high in the sky
        // things should fall).
        const float above_now = g_near.palm.y - std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
        bool high = g_near.on && g_set.disciple_max_m > 0 && above_now > g_set.disciple_max_m * wf.scale;
        if (high && is_scaffold(object_in_hand()) && above_now <= g_set.scaffold_max_m * wf.scale) high = false;  // (a scaffold: higher)
        if (g_near.on && !giving && !g_scoop_done) {
            void* held = object_in_hand();
            Vec3 v0;
            const bool onto = g_apply_target != nullptr;
            // (Touching something it goes onto, it takes a real throw to throw it: the hand moves
            // a little as it opens, and scaffolds touched together were thrown apart; 2026-10-28.)
            const bool thrown = release_velocity_now(v0, g_set.throw_min_speed_m_s * wf.scale) &&
                                vr::length(v0) > g_set.throw_min_speed_m_s * wf.scale * (onto ? 2.0f : 1.0f);
            const bool build = g_set.build_at_blueprint && blueprint_valid(held) && !high;
            if (onto || is_scaffold(held))
                BWLOG("hands: let go of %s: onto %s, thrown %d (%.2f), blueprint %d", class_name(held), onto ? class_name(g_apply_target) : "nothing",
                      thrown, vr::length(v0), build);
            if (!thrown && onto) {
                // (fn_005DA1A0, GInterface: the held thing's ApplyThisToObject on the target, then
                // out of the hand; the release packet's handler, called without the packet.)
                void* gi = game_interface();
                void* target = g_apply_target;
                if (gi && applies_to(held, target)) {
                    using ApplyFn = int(__thiscall*)(void*, void*, void*);
                    // (It answers what the apply did, 0 for nothing; the hand lets go of the scaffold a
                    // moment later.)
                    const int r = reinterpret_cast<ApplyFn>(0x5DA1A0)(gi, target, nullptr);
                    applied = r != 0;
                    BWLOG("hands: let go of %s touching %s: %s (%d)", class_name(held), class_name(target), applied ? "applied to it" : "not applied", r);
                    if (applied) vr::openvr::haptic(g_hand, 0.03f, 200.0f, 0.6f);
                }
            }
            // (A tree let go of, not thrown, is planted on the ground below the hand: see Release::plant.)
            plant_tree = !thrown && is_tree(held) && !high;
            if (!thrown && (applied || build)) {
                to_game = true;
                g_release.t = -10, g_flight.obj3d = nullptr;  // (nothing of the mod's own launch)
                if (!applied) BWLOG("hands: let go of a scaffold over its blueprint: the game puts it down at its hand");
            }
        }
        if (!to_game && !giving && !g_scoop_done && now_s() > g_gg_soft_until &&
            (g_near.on ? release_velocity_now(rv, g_set.throw_min_speed_m_s * wf.scale) : hit_velocity(rv, 0.15))) {
            if (g_pending_release.on)
                BWLOG("hands: the release waited %.2f s for the game to have it: carried out now, at the hand's speed then (%.2f)",
                      now_s() - g_pending_release.t, vr::length(rv) / wf.scale);
            g_release = {now_s(), rv, g_near.on, g_near.palm, held_object_3d_hand()};
            g_release.plant = plant_tree && g_near.on;
            g_release.high = high;
            if (high && vr::length(rv) <= g_set.throw_min_speed_m_s * wf.scale) BWLOG("hands: let go %.1f m up: it falls", above_now / wf.scale);
            // A throw (not a drop) leaves the hand now, not when the game gets round to it. So
            // does anything let go of high above the ground: it falls from the hand (the game
            // would put it straight down on the ground, as it does for a drop near the ground,
            // which is how things are set down and planted).
            const float above = g_near.palm.y - std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
            if (g_set.vr_throw && g_set.throw_flight && g_near.on && g_held_seen.obj3d &&
                (vr::length(rv) > g_set.throw_min_speed_m_s * wf.scale || above > g_set.drop_fall_m * wf.scale)) {
                g_flight = {g_held_seen.obj3d, now_s(), g_held_seen.pos, rv * g_set.hand_throw_gain};
                memcpy(g_flight.rot, g_held_seen.rot, sizeof(g_flight.rot));
                // (A tree set down: straight down to the ground in the moment before the game launches
                // it, at no speed of its own.)
                if (g_release.plant) g_flight.v = Vec3{0, -above / 0.2f, 0};
            }
            if (g_release.plant) BWLOG("hands: let go of a tree, not thrown: planted on the ground below the hand (%.1f m below)", above / wf.scale);
        }
        // (Not after a hand-over, nor at the end of a scoop, nor once joined: the hand is empty, and
        // a click would take hold of what is under it.)
        if (now_s() >= g_no_drop_click_until && !g_scoop_done && !applied) {
            g_drop = DropClick::kDown;
            g_drop_at = GetTickCount64() + 40;
        }
        if (g_flight_trace.on)
            g_flight_trace.t0 = now_s(), g_flight_trace.launched = -10, g_flight_trace.obj3d = g_held_seen.obj3d,
            g_flight_trace.object = g_flight_trace.phys = nullptr;
    }
    if (g_scoop_drop_at > 0 && now_s() >= g_scoop_drop_at) {
        // (Still holding it, the hand open: the click that puts it down; once more if the first
        // didn't take.)
        if (game_hand_state() != kHandStateHolding || g_rmb || g_scoop_drop_tries >= 2) {
            if (g_scoop_drop_tries >= 2 && game_hand_state() == kHandStateHolding) BWLOG("hands: what was scooped wasn't put down");
            g_scoop_drop_at = -1;
        } else if (g_drop == DropClick::kIdle) {
            g_drop = DropClick::kDown, g_drop_at = GetTickCount64() + 10;
            ++g_scoop_drop_tries;
            g_scoop_drop_at = now_s() + 0.5;
        }
    }
    run_drop_click();
    if (g_pop_after_release.object) {
        if (now_s() - g_pop_after_release.t > 1.5) {
            BWLOG("hands: the bubble wasn't popped (the hand didn't come free)");
            g_pop_after_release = {};
        } else if (hand_free(game_hand_state()) && g_drop == DropClick::kIdle && !g_rmb) {
            // (The cursor is put on the bubble for the click: it has fallen from the hand.)
            g_pop_aim = g_pop_after_release.object;
            g_drop = DropClick::kDown, g_drop_at = GetTickCount64() + 30;
            g_pop_click = {g_pop_after_release.object, now_s()};
            BWLOG("hands: the bubble is let go of: clicking it");
            g_pop_after_release = {};
        }
    }
    if (g_pop_click.object) {  // (did the click pop it? the hand goes to its charged state)
        if (game_hand_state() == kHandStateCharged) {
            BWLOG("hands: the bubble popped: the miracle is charged in the hand");
            vr::openvr::haptic(g_hand, 0.03f, 200.0f, 0.6f);
            g_pop_click = {}, g_pop_aim = nullptr;
        } else if (now_s() - g_pop_click.t > 1.0) {
            BWLOG("hands: the bubble wasn't popped (hand state %u)", game_hand_state());
            g_pop_click = {}, g_pop_aim = nullptr;
        }
    }
    // Scooping from a store, a pile or a field: the game keeps adding to the hand for as long as
    // the action button is held on the pile (its interaction, GInterfaceStatus +0x3C; each frame
    // the pile's ProcessInInteract adds more). Letting go of the button ends it, and the hand
    // carries what it took until the next click puts it down. In VR the grip holds the button, so
    // letting go also puts it down at once: there was no way to stop part way and keep it. Now the
    // hand (or the laser point) leaving the pile by more than scoop_leave ends the scoop: the
    // game's button goes up while the grip stays closed, and opening the grip puts it down.
    // (Ending it with the game's own packet, 0x1C, left GInterface deaf to the hand afterwards.)
    {
        void* pile = scoop_pile();
        Vec3 c;
        float r = 0;
        if (pile && g_rmb && !g_scoop_done && g_set.scoop_end && g_ptr.valid && bounding_sphere(pile, nullptr, c, r)) {
            const Vec3 at = g_near.on && g_near.blend >= 1.0f ? g_near.palm : g_ptr.hit;
            // (Out of its column, the way it was reached: from above, the hand is far from its middle.)
            const float flat = std::sqrt((at.x - c.x) * (at.x - c.x) + (at.z - c.z) * (at.z - c.z));
            const float out = std::max(flat - r, at.y - scoop_top(c, r));
            if (out > (g_set.scoop_leave_m + g_set.pile_reach_m) * wf.scale) {  // (it is reached from that far too)
                g_scoop_done = true;
                BWLOG("hands: scoop ended: the hand left the %s (%.2f m outside it)", class_name(pile), out / wf.scale);
                vr::openvr::haptic(g_hand, 0.02f, 120.0f, 0.4f);
            }
        }
    }
    refresh_in_flight();
    watch_spell();
    atom_track_tick();
    liquid_log_tick();
    scan_watch_charged();
    spell_points_at_palm();
    fireball_no_decay();
    track_fireball();
    freeze_watch_tick();
    wrist_update(pick_eye.pos);
    if (g_walk_check_at > 0 && now_s() > g_walk_check_at) {
        BWLOG("hands: leash: the creature's command is now 0x%X, its action 0x%X (control %d)", creature_command(own_creature()),
              creature_action(own_creature()), creature_control(own_creature()));
        g_walk_check_at = -1;
    }
    // The give started (CHand +0x49BC: the creature being given to). The game finishes it once
    // its hand has come within reach of the creature (ObtainRequiredHandPosition: then packet
    // 0x52 with the creature's id); the game's hand is where the player's is, so the mod finishes
    // it, a moment after the give has started (the creature turns to it meanwhile).
    // (Packet 0x52 is taken only while the creature is reaching for it: CreatureMental +0xF60
    // action 0x8E with the current command 0x56; otherwise the creature gives up the action.)
    if (g_give_finish.id && g_game_hand) {
        const char* h = static_cast<const char*>(g_game_hand);
        const bool started = *reinterpret_cast<void* const*>(h + 0x49BC) != nullptr;
        void* own = own_creature();
        const char* mental = own ? *reinterpret_cast<const char* const*>(static_cast<const char*>(own) + 0x164) : nullptr;
        const int action = mental && !IsBadReadPtr(mental + 0xF60, 4) ? *reinterpret_cast<const int*>(mental + 0xF60) : -1;
        // (... and it has to have hold of it by then: +0x160 -> +0x28, or the take counts as failed.)
        const char* grasp = own ? *reinterpret_cast<const char* const*>(static_cast<const char*>(own) + 0x160) : nullptr;
        const bool has_it = grasp && !IsBadReadPtr(grasp + 0x28, 4) && *reinterpret_cast<void* const*>(grasp + 0x28) != nullptr;
        const bool reaching = action == 0x8E && creature_command(own) == 0x56 && has_it;
        if (started && g_give_finish.started_at < 0) g_give_finish.started_at = now_s();
        if (started && reaching && now_s() - g_give_finish.started_at > g_set.give_finish_s) {
            // (The game would, once its hand reached the creature's; the game's hand is the player's,
            // at the controller. Putting the game's cursor on the creature instead ended the give.)
            using SetPacketIdFn = void(__thiscall*)(void*, int, uint32_t);
            if (void* game = game::game_instance()) reinterpret_cast<SetPacketIdFn>(0x550C80)(game, 0x52, g_give_finish.id);
            BWLOG("hands: giving: the creature has it: finished (packet 0x52) %.2f s after the give started",
                  now_s() - g_give_finish.started_at);
            g_give_finish = {};
        } else if (!started && g_give_finish.started_at >= 0) {
            BWLOG("hands: giving: the game finished (or dropped) the give itself");
            g_give_finish = {}, g_give_check_at = now_s() + 0.4;
        } else if (now_s() - g_give_finish.sent_at > 6.0) {
            BWLOG("hands: giving: gave up (started %d, the creature's action 0x%X, command 0x%X): finished as not taken (packet 0x52)", started, action,
                  creature_command(own));
            // (The game's finish of a give: with the creature not reaching for it, the creature's action
            // is ended as failed and the hand's give cleared. Left as it was, a creature stood frozen in
            // action 0x9B, taking nothing more from the hand, and the leash wouldn't shake off; player,
            // 2026-11-08.)
            using SetPacketIdFn52 = void(__thiscall*)(void*, int, uint32_t);
            if (void* game = game::game_instance()) reinterpret_cast<SetPacketIdFn52>(0x550C80)(game, 0x52, g_give_finish.id);
            g_give_finish = {}, g_give_check_at = now_s() + 0.4;
        }
    }
    // A give that ended without the creature taking it left the thing in the game's hand, though
    // the grip had let go of it (the hand-over counts as letting go): it stayed in the hand until
    // the grip closed and opened again. It is put down now, once the grip is open.
    if (g_give_check_at > 0 && now_s() > g_give_check_at) {
        if (game_hand_state() != kHandStateHolding || g_give_finish.id) {
            g_give_check_at = -1;
        } else if (c.squeeze < g_set.squeeze_up && !g_rmb && g_drop == DropClick::kIdle) {
            g_drop = DropClick::kDown, g_drop_at = GetTickCount64() + 40;
            g_give_check_at = -1;
            BWLOG("hands: giving: not taken: put down");
        }
    }
    static double give_log_until = -10;
    if (g_give_finish.id) give_log_until = now_s() + 3.0;
    if (now_s() < give_log_until && g_game_hand && g_dbg_give_log) {  // (debug: the game's give, after the packet)
        static double logged_at = -10;
        if (now_s() - logged_at > 0.1) {
            const char* h = static_cast<const char*>(g_game_hand);
            const auto& hp = *reinterpret_cast<const game::LHPoint*>(h + 0x78);
            void* own = own_creature();
            const char* mental = own ? *reinterpret_cast<const char* const*>(static_cast<const char*>(own) + 0x164) : nullptr;
            const char* grasp = own ? *reinterpret_cast<const char* const*>(static_cast<const char*>(own) + 0x160) : nullptr;
            BWLOG("hands: giving: +0x49BC %p, state %u, game hand (%.1f, %.1f, %.1f), the creature's action 0x%X command 0x%X, holding %p",
                  *reinterpret_cast<void* const*>(h + 0x49BC), game_hand_state(), hp.x, hp.y, hp.z,
                  mental ? *reinterpret_cast<const int*>(mental + 0xF60) : -1, creature_command(own),
                  grasp && !IsBadReadPtr(grasp + 0x28, 4) ? *reinterpret_cast<void* const*>(grasp + 0x28) : nullptr);
            logged_at = now_s();
        }
    }
    prehold_update();
    if (g_after_launch.obj3d) {
        const double age = now_s() - g_after_launch.t0;
        float* m = IsBadWritePtr(const_cast<char*>(g_after_launch.obj3d) + 0x14, 48) ? nullptr
                                                                                    : reinterpret_cast<float*>(const_cast<char*>(g_after_launch.obj3d) + 0x14);
        const float speed = vr::length(g_after_launch.v);
        if (!m || age > 0.35 || speed < 1e-3f) {
            g_after_launch.obj3d = nullptr;
        } else {
            const Vec3 want = g_after_launch.start + g_after_launch.v * float(age);
            const Vec3 at{m[9], m[10], m[11]};
            const float behind = vr::dot(want - at, g_after_launch.v) / speed;  // how far the game's place lags, along the throw
            if (behind > 0) {
                m[9] = want.x, m[10] = want.y, m[11] = want.z;
                if (!g_after_launch.logged) g_after_launch.logged = true, BWLOG("hands: thrown thing kept moving after the launch (the game's place %.2f behind)", behind);
            } else {
                g_after_launch.obj3d = nullptr;  // (caught up)
            }
        }
    }
    if (g_flight.obj3d) {
        float* m = IsBadWritePtr(const_cast<char*>(g_flight.obj3d) + 0x14, 48) ? nullptr
                                                                             : reinterpret_cast<float*>(const_cast<char*>(g_flight.obj3d) + 0x14);
        // The game may put the object somewhere itself instead of launching it (placing or
        // planting it: 5 of 55 releases in one session). Once its hand has let go, a matrix that
        // isn't the one written here last is the game's: the flight stops, or the object would
        // stay frozen where the flight had got to.
        const bool game_moved_it = m && g_flight.wrote && game_hand_state() != kHandStateHolding && memcmp(m, g_flight.last, sizeof(g_flight.last)) != 0;
        if (!m || now_s() - g_flight.t0 > 0.6 || game_moved_it) {
            if (m && (game_moved_it || now_s() - g_flight.t0 > 0.6))
                BWLOG("hands: thrown object not launched (%s): left where the game has it", game_moved_it ? "the game placed it" : "timed out");
            g_flight.obj3d = nullptr;
        } else {
            const Vec3 p = flight_pos(g_flight, now_s());
            memcpy(m, g_flight.rot, sizeof(g_flight.rot));
            m[9] = p.x, m[10] = p.y, m[11] = p.z;
            memcpy(g_flight.last, m, sizeof(g_flight.last)), g_flight.wrote = true;
        }
    }
    if (g_flight_trace.on && now_s() - g_flight_trace.t0 < 3.0 &&
        (g_flight_trace.launched < 0 || now_s() - g_flight_trace.launched < 1.0)) {
        Vec3 p;
        const bool have = object_pos(g_flight_trace.obj3d, p);
        Vec3 own;  // the object's own position (map coordinates and height above the ground)
        if (const char* o = g_flight_trace.object) {
            const int32_t* mc = reinterpret_cast<const int32_t*>(o + 0x14);
            own = {float(mc[0]) * 10.0f / 65536.0f, reinterpret_cast<const float*>(mc)[2], float(mc[1]) * 10.0f / 65536.0f};
        }
        float ph[9] = {};  // PhysicsObject: position (+0xC8), previous position (+0xF8), velocity (+0x104)
        if (const char* q = g_flight_trace.phys; q && !IsBadReadPtr(q, 0x110)) {
            memcpy(ph, q + 0xC8, 12), memcpy(ph + 3, q + 0xF8, 12), memcpy(ph + 6, q + 0x104, 12);
        }
        const char* st = g_game_hand ? *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850) : nullptr;
        BWLOG("hands: flight %.3f%s state %u held %d palm (%.2f, %.2f, %.2f) object (%.2f, %.2f, %.2f)%s own (%.2f, +%.2f, %.2f) phys "
              "(%.2f, %.2f, %.2f) (%.2f, %.2f, %.2f) v (%.1f, %.1f, %.1f)",
              now_s() - g_flight_trace.t0, g_flight_trace.launched >= 0 ? " launched" : "", game_hand_state(), st ? int(st[0x90]) : -1, g_near.palm.x,
              g_near.palm.y, g_near.palm.z, p.x, p.y, p.z, have ? "" : " (gone)", own.x, own.y, own.z, ph[0], ph[1], ph[2], ph[3],
              ph[4], ph[5], ph[6], ph[7], ph[8]);
    }
    if (g_dbg_vinfl > 0 && g_game_hand) {  // (debug: the influence-ring warning's state, hands vinfl N)
        --g_dbg_vinfl;
        const char* h = static_cast<const char*>(g_game_hand);
        const char* st = *reinterpret_cast<const char* const*>(h + 0x4850);
        const char* vi = st ? *reinterpret_cast<const char* const*>(st + 0x130) : nullptr;
        const char* gi = static_cast<const char*>(game_interface());
        const char* gh = gi ? *reinterpret_cast<const char* const*>(gi + 0x3A0) : nullptr;
        if (vi && gh) {
            const int32_t* in = reinterpret_cast<const int32_t*>(vi + 8);
            const int32_t* cur = reinterpret_cast<const int32_t*>(st + 0x14);
            const auto& hp = *reinterpret_cast<const game::LHPoint*>(gh + 0x78);
            BWLOG("hands: vinfl amount %.2f sound %d off %d inside (%.1f, %.1f) status (%.1f, %.1f) gi hand %p (mine %p) (%.1f, %.1f, %.1f) cursor %d %ld,%ld; can act %d at (%.1f, %.1f)",
                  *reinterpret_cast<const float*>(vi + 0x30), int(vi[0x3C]), *reinterpret_cast<const int*>(vi + 0x44),
                  in[0] / 6553.6f, in[1] / 6553.6f, cur[0] / 6553.6f, cur[1] / 6553.6f, gh, h, hp.x, hp.y, hp.z,
                  int(g_ptr.cursor_ok), g_ptr.cursor_x, g_ptr.cursor_y, *reinterpret_cast<const int*>(gi + 0x48),
                  reinterpret_cast<const int32_t*>(gi + 0x3F0)[0] / 6553.6f, reinterpret_cast<const int32_t*>(gi + 0x3F0)[1] / 6553.6f);
        }
    }
    update_outside_cue();
    if (g_thrown.obj3d && g_thrown.steps_logged < 7) {
        static const double kSteps[7] = {0.1, 0.2, 0.3, 0.5, 1.5, 2.5, 4.0};
        Vec3 p;
        if (now_s() - g_thrown.t > kSteps[g_thrown.steps_logged] && object_pos(g_thrown.obj3d, p))
            BWLOG("hands: thrown object at %.1f s: (%.1f, %.1f, %.1f)", kSteps[g_thrown.steps_logged++], p.x, p.y, p.z);
    }
    if (g_thrown.obj3d && !g_thrown.went_logged && now_s() - g_thrown.t > 1.0) {
        Vec3 p;
        if (object_pos(g_thrown.obj3d, p))
            BWLOG("hands: thrown object went from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) in 1 s: %.1f units", g_thrown.start.x,
                  g_thrown.start.y, g_thrown.start.z, p.x, p.y, p.z, vr::length(p - g_thrown.start));
        g_thrown.went_logged = true;
    }
    if (g_thrown.obj3d && now_s() - g_thrown.t > 4.1) g_thrown.obj3d = nullptr, g_thrown.object = nullptr;
    // Haptics on the game's own hand-state changes: a firm tick when something is grabbed
    // (pick-up or tug), a light one when the land is taken hold of.
    static uint32_t last_state = 0;
    const uint32_t st = game_hand_state();
    if (st != last_state) {
        // (Logged: what the game's hand does with a creature can't be reached from the tests.)
        BWLOG("hands: game hand state %u -> %u (%s, grip %d, trigger %d, touching %d, petting %d, tappable %d)", last_state, st,
              g_near.on ? "hand at the controller" : "laser", g_rmb, g_lmb, now_s() - g_touch.t < 0.1, g_petting, tappable_under_hand());
        if (st == kHandStateHolding && g_game_hand) {  // (what it holds: GInterfaceStatus +0x120, LastPickedUpObject)
            const char* status = *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850);
            const void* held = status && !IsBadReadPtr(status + 0x120, 4) ? *reinterpret_cast<const void* const*>(status + 0x120) : nullptr;
            BWLOG("hands: the hand holds %s %p (touched %s %p)", held ? class_name(held) : "-", held,
                  g_touch.object ? class_name(g_touch.object) : "-", g_touch.object);
        }
        if (st == kHandStateHolding || st == 3) vr::openvr::haptic(g_hand, 0.04f, 160.0f, 0.8f);
        else if (st == kHandStateCamera) vr::openvr::haptic(g_hand, 0.02f, 120.0f, 0.4f);
        last_state = st;
    }
    {   // (Logged, the first 0.6 s held: when the game's two answers to "what is in the hand" change.)
        static double held_at = -10;
        static const void* last_first = nullptr;
        static const void* last_picked = nullptr;
        if (st == kHandStateHolding && held_at < 0) held_at = now_s(), last_first = last_picked = reinterpret_cast<const void*>(1);
        if (st != kHandStateHolding) held_at = -10;
        if (held_at > 0 && now_s() - held_at < 0.6 && g_game_hand) {
            const void* first = object_in_hand();
            const char* status = *reinterpret_cast<const char* const*>(static_cast<const char*>(g_game_hand) + 0x4850);
            const void* picked = status && !IsBadReadPtr(status + 0x120, 4) ? *reinterpret_cast<const void* const*>(status + 0x120) : nullptr;
            if (first != last_first || picked != last_picked)
                BWLOG("hands: held %.3f s: first in the hand %s %p, last picked up %s %p", now_s() - held_at, first ? class_name(first) : "-", first,
                      picked ? class_name(picked) : "-", picked),
                    last_first = first, last_picked = picked;
        }
    }
}

bool near_hand(NearHand& out) {
    if (g_near.blend <= 0 || !g_ptr.valid) return false;
    out.blend = g_near.blend;
    out.pos = g_near.pos, out.x = g_near.x, out.y = g_near.y, out.z = g_near.z;
    out.palm = g_near.palm;
    // A grip closed on nothing (the game's hand free, no press made): a fist.
    out.pose = g_set.fist_on_grip && g_near.blend >= 1.0f && g_fist[g_hand] && !g_rmb && hand_free(game_hand_state())
                   ? HandPose::kFist : HandPose::kGame;
    // Drawing a gesture with B held: the hand points, the pen at the fingertip (player, 2026-10-27).
    if (g_set.point_to_draw && g_gd.on && g_near.blend >= 1.0f) out.pose = HandPose::kPoint;
    if (g_zap && g_near.blend >= 1.0f) out.pose = HandPose::kPoint;  // (lightning: pointed with the finger)
    out.axis_len = g_set.hand_size * g_rig.scale;
    return true;
}

// What the hand carries, when it's a pile of food or wood (its amount at +0x70; the game shows no
// count of it in VR).
bool held_resource(const wchar_t*& kind, int& amount, Vec3* top) {
    // (Only while the game's hand holds: after a hand-over its status still names the pile, and the
    // count stayed by an empty hand, at the palm, behind the hand: double vision; player, 2026-10-31.)
    if (game_hand_state() != kHandStateHolding) return false;
    void* status = game_hand_alive() ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850)
                                                                                              : nullptr;
    void* held = status && !IsBadReadPtr(static_cast<char*>(status) + 0x120, 4) ? *reinterpret_cast<void**>(static_cast<char*>(status) + 0x120) : nullptr;
    if (!held || IsBadReadPtr(static_cast<char*>(held) + 0x70, 4)) return false;
    const char* name = class_name(held);
    if (strcmp(name, ".?AVPileFood@@") == 0) kind = L"food";
    else if (strcmp(name, ".?AVPileWood@@") == 0) kind = L"wood";
    else return false;
    amount = *reinterpret_cast<const int*>(static_cast<char*>(held) + 0x70);
    if (top) {  // (above what it carries, where its label goes: its sphere's top, 8-30 cm above the palm; a
                // pile's sphere can be far bigger than the bundle in the hand)
        Vec3 c;
        float r = 0;
        const float lo = 0.08f * g_rig_scale, hi = 0.30f * g_rig_scale;
        const float up = bounding_sphere(held, nullptr, c, r) ? std::clamp(c.y + r - g_near.palm.y, lo, hi) : lo;
        *top = g_near.palm + Vec3{0, up, 0};
    }
    return amount > 0;
}
void set_depth_scale(float k) {
    if (k > 0) g_set.depth_k = k;
}

bool main_hand_left() { return g_hand == vr::kLeft; }  // (the hand the god hand is drawn at: the main one, or the other while it acts)
// From the game's hand preparing to draw, once a frame: the palm's offer even when no object reports.
void offer_at_palm() {
    {
        static double logged_at = -10;
        if (g_offer_log > 0 && now_s() - logged_at > 0.5)
            logged_at = now_s(), BWLOG("hands: offer before drawing: near %d, picker %p (%d), found before %p %.2f s ago", g_near.on, g_pick_iface,
                                       g_pick_iface && game_method(g_pick_iface, 0) ? 1 : 0, g_palm_best.object, now_s() - g_palm_best.at);
    }
    if (!g_near.on || !g_pick_iface || !game_method(g_pick_iface, 0)) return;
    // (Not counted as this frame's offer: the hand is prepared outside the picking, which is reset
    // when the world is drawn; marked, it kept the offer from the picking itself, and the house
    // beside the gate stone won there; 2026-11-02.)
    g_offer_preparing = true;
    g_living_offered = false;
    offer_by_palm(g_pick_iface);
    g_offer_preparing = false;
    g_living_offered = false, g_scroll_by_palm = false, g_small_by_palm = false;
}
bool second_hand(NearHand& out) {
    if (!g_second_valid || !g_ptr.valid) return false;
    out.blend = 1.0f;
    out.pos = g_second.pos, out.x = g_second.x, out.y = g_second.y, out.z = g_second.z;
    // Its own fingers, not the god hand's: a fist while its grip is closed, else open.
    out.pose = !g_set.fist_on_grip ? HandPose::kGame : g_fist[1 - g_hand] ? HandPose::kFist : HandPose::kOpen;
    out.axis_len = g_set.hand_size * g_rig.scale;
    return true;
}

double last_tap_at() { return g_last_tap_at; }
// What the grip would take now, to light it up: the thing the palm is on (or a pile or field below
// it), or what the laser is on. Only things with an interaction of their own (player, 2026-10-19:
// "less gamey" for what the hand simply picks up): a store's piles and fields, and what is tapped
// (houses, scrolls, signposts, rocks too heavy to lift) but not what the hand can lift; not
// villagers, trees, small rocks or the like. Not the player's
// creature, and only with the game's hand free.
int g_glow_any = 0;  // debug (hands glow_any 1): whatever the palm touches is lit up
void* highlight_target(float& amount) {
    amount = g_set.highlight;
    if (g_set.highlight <= 0 || g_ui_pointing || !hand_free(game_hand_state())) return nullptr;
    void* o = nullptr;
    if (g_near.on && g_near.blend >= 1.0f) {
        if (now_s() - g_touch.t < 0.15) o = g_touch.object;
    } else if (g_ptr.valid) {
        o = g_picked;
    }
    if (g_glow_any && o && !IsBadReadPtr(o, 0x44)) return o;
    if (!o || o == own_creature() || IsBadReadPtr(o, 0x44)) return nullptr;
    if (scoopable(o) || *reinterpret_cast<void**>(o) == reinterpret_cast<void*>(0x94228C)) return o;
    if (void* status = g_game_hand ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850) : nullptr) {
        // Tappable, but not what the hand would simply pick up (player, 2026-10-20: small rocks
        // had the ring; a rock too heavy to lift, which a tap splits, should have it).
        // Object::CanBePickedUp is vtable 0x628, ValidForPlaceInHand(GInterfaceStatus*) 0x6FC
        // (a Rock: too heavy above a weight).
        const auto tap_fn = reinterpret_cast<unsigned(__thiscall*)(void*, void*)>(game_method(o, 0x740));
        if (tap_fn && !liftable(o, true) && tap_fn(o, status)) return o;  // (Object::InterfaceValidToTap)
    }
    return nullptr;
}

bool take_feedback_request() {
    const bool r = g_feedback_requested;
    g_feedback_requested = false;
    return r;
}

bool laser_anchor(Vec3& point) {
    if (!g_ptr.valid) return false;
    if (g_drag.on) {
        point = g_drag.grab;
        return true;
    }
    if (!g_ptr.hit_land) return false;
    point = g_ptr.hit;
    return true;
}

bool pointer_target(Vec3& hit) {
    if (!g_ptr.valid) return false;
    hit = g_ptr.hit;
    return true;
}

void* g_device = nullptr;  // (the eye passes' device, for panels drawn from the UI)
void* panel_device() { return g_device; }
void draw_panel_quad(const EyeView& eye, const Vec3 (&q)[4], uint32_t colour) {
    if (!g_device) return;
    std::vector<Vtx> v;
    const DWORD col[4] = {colour, colour, colour, colour};
    add_quad(v, eye, q, col);
    g_tris_over_all = true;
    draw_tris(static_cast<IDirect3DDevice7*>(g_device), v);
    g_tris_over_all = false;
}

// The influence warning's stream (see add_mana_path_detour): motes leave the palm and arc back to
// the influence's edge, glowing in the game's colour, while the game asks for it.
void draw_mana_stream(IDirect3DDevice7* dev, const EyeView& eye) {
    const double now = now_s();
    const float vis = float(std::clamp(1.0 - (now - g_mana.at - 0.15) / 0.3, 0.0, 1.0));  // (fades once the game stops asking)
    if (vis <= 0 || g_near.blend < 0.5f) return;
    const Vec3 a = g_near.palm, b = g_mana.ring;
    const float len = vr::length(b - a);
    if (len < 0.05f * g_rig.scale) return;
    // An arc: out from the palm, up a little, and down onto the edge.
    const Vec3 ctrl = (a + b) * 0.5f + Vec3{0, std::min(0.25f * len, 3.0f * g_rig.scale), 0};
    const auto at = [&](float t) {
        const float u = 1.0f - t;
        return a * (u * u) + ctrl * (2.0f * u * t) + b * (t * t);
    };
    // Its colour: the game's (LH3DColor, ARGB); a warm red if it gives none.
    DWORD rgb = g_mana.colour & 0xFFFFFF;
    if (!rgb) rgb = 0xFF6040;
    const auto argb = [](float al, DWORD c) { return (DWORD(std::clamp(al, 0.0f, 1.0f) * 255.0f) << 24) | c; };
    std::vector<Vtx> glow;
    const float metres = len / std::max(1e-3f, g_rig.scale);
    const int n = std::clamp(int(metres * 8.0f), 10, 80);  // (about eight motes a metre)
    const double speed = 0.9 / std::max(0.5, double(metres));  // (along the arc, about 0.9 m a second)
    for (int i = 0; i < n; ++i) {
        const float t = float(std::fmod(now * speed + double(i) / n, 1.0));
        const Vec3 p = at(t);
        const float dist = vr::length(eye.pos - p);
        // About 2.5 cm at the hand, never under half a degree, so it reads far off too.
        const float r = std::max(0.025f * g_rig.scale, 0.009f * dist);
        const float al = vis * std::min(1.0f, t * 8.0f) * std::min(1.0f, (1.0f - t) * 4.0f);
        const Vec3 to_e = vr::normalize(eye.pos - p);
        const Vec3 sd = vr::normalize(vr::cross(to_e, Vec3{0, 1, 0.001f}));
        const Vec3 up = vr::normalize(vr::cross(to_e, sd));
        const int kRim = 8;
        for (int k = 0; k < kRim; ++k) {  // (a soft disc)
            const float a0 = 6.2831853f * float(k) / kRim, a1 = 6.2831853f * float(k + 1) / kRim;
            const Vec3 d0 = sd * std::cos(a0) + up * std::sin(a0), d1 = sd * std::cos(a1) + up * std::sin(a1);
            const Vec3 q[4] = {p, p + d0 * r, p + d1 * r, p};
            const DWORD c[4] = {argb(0.8f * al, rgb), argb(0, rgb), argb(0, rgb), argb(0.8f * al, rgb)};
            add_quad(glow, eye, q, c, 0.99f);
        }
        const float s = r * 0.35f;  // (a white-hot centre)
        const Vec3 q[4] = {p - sd * s, p - up * s, p + sd * s, p + up * s};
        const DWORD cw = argb(0.9f * al, 0xFFFFF0);
        const DWORD c[4] = {cw, cw, cw, cw};
        add_quad(glow, eye, q, c, 0.99f);
    }
    // A faint thread along the arc.
    const int kSegs = 24;
    for (int i = 0; i < kSegs; ++i) {
        const float t0 = float(i) / kSegs, t1 = float(i + 1) / kSegs;
        const Vec3 p0 = at(t0), p1 = at(t1);
        const Vec3 sd = vr::normalize(vr::cross(p1 - p0, (p0 + p1) * 0.5f - eye.pos));
        const float w = std::max(0.008f * g_rig.scale, 0.002f * vr::length(eye.pos - p0));
        const Vec3 q[4] = {p0 - sd * w, p0 + sd * w, p1 + sd * w, p1 - sd * w};
        const float a0 = 0.35f * vis * (1.0f - 0.6f * t0), a1 = 0.35f * vis * (1.0f - 0.6f * t1);
        const DWORD c[4] = {argb(a0, rgb), argb(a0, rgb), argb(a1, rgb), argb(a1, rgb)};
        add_quad(glow, eye, q, c, 0.99f);
    }
    g_tris_additive = true;
    draw_tris(dev, glow);
    g_tris_additive = false;
}
// The teleport's aim, drawn as the pointer's beam is (player, 2026-11-04: polish it like the laser,
// give it a curve, and keep it on the hand: it trailed behind while moving with the stick). From the
// controller's tip as the rig is now (the aim was taken before the frame's movement), arcing up and
// down onto the spot (a quadratic curve); a glowing ring there. Gold on land,
// red where there is nothing to stand on.
void draw_teleport_aim(IDirect3DDevice7* dev, const EyeView& eye) {
    if (!g_tp.aiming) return;
    const WorldFrame wf = rig_frame();
    const Vec3 from = to_world(wf, g_tp.local_pos);
    const Vec3 dir = vr::normalize(wf.base_rot.apply(g_tp.local_dir));
    const Vec3 end = g_tp.valid ? g_tp.target : from + dir * std::min(vr::length(g_tp.target - from), 3.0f * g_rig.scale);
    const float dist = vr::length(end - from);
    if (dist < 1e-3f) return;
    // (The spot is on the straight aim: the curve bows up over it, as a thrown arc, and comes down
    // onto the spot; more so the flatter the aim.)
    const Vec3 ctrl = from + (end - from) * 0.5f + Vec3{0, 1, 0} * (dist * 0.22f * (1.0f - std::fabs(dir.y)));
    const auto at = [&](float t) { const float u = 1.0f - t; return from * (u * u) + ctrl * (2.0f * u * t) + end * (t * t); };
    const double now = g_beam_still ? 0.25 : now_s();
    const auto argb = [](float al, DWORD rgb) { return (DWORD(std::clamp(al, 0.0f, 1.0f) * 255.0f) << 24) | rgb; };
    const DWORD kHalo = g_tp.creature ? 0x80D0FF : g_tp.valid ? 0xFFC860 : 0xFF5A40, kCore = g_tp.creature ? 0xE8F6FF : g_tp.valid ? 0xFFF8E8 : 0xFFD8D0;
    if (g_tp.creature) {  // (the creature lit up: its drawn model, LH3DCreature (Creature +0x160 -> +0x58) +0x482C)
        if (void* cr = own_creature(); cr && !IsBadReadPtr(static_cast<char*>(cr) + 0x164, 4)) {
            const char* a = *reinterpret_cast<const char* const*>(static_cast<char*>(cr) + 0x160);
            const char* lh = a && !IsBadReadPtr(a + 0x58, 4) ? *reinterpret_cast<const char* const*>(a + 0x58) : nullptr;
            const void* model = lh && !IsBadReadPtr(lh + 0x482C, 4) ? *reinterpret_cast<const void* const*>(lh + 0x482C) : nullptr;
            g_glow_obj3d = model, g_glow_object = cr, g_glow_amount = 0.5f, g_glow_at = now_s(), g_glow_cool = true;
        }
    }
    std::vector<Vtx> glow;
    glow.reserve(1024);
    const float w0 = g_set.beam_width_m * g_rig.scale * 0.5f;
    const int kSegs = 40;
    for (int i = 0; i < kSegs; ++i) {
        const float t0 = float(i) / kSegs, t1 = float(i + 1) / kSegs;
        const Vec3 p0 = at(t0), p1 = at(t1);
        const Vec3 side = vr::normalize(vr::cross(p1 - p0, (p0 + p1) * 0.5f - eye.pos));
        const float d0 = vr::length(p0 - from), d1 = vr::length(p1 - from);
        const float h0 = w0 * (1.0f + 0.004f * d0), h1 = w0 * (1.0f + 0.004f * d1);
        const auto pulse = [&](float d) {  // (pulses running out along it, two a second)
            const float ph = float(std::fmod(now * 2.0 - double(d / std::max(1e-3f, g_rig.scale)) * 0.6, 1.0));
            return 0.5f + 0.5f * std::pow(std::max(0.0f, std::cos(ph * 6.2831853f)), 8.0f);
        };
        const auto env = [&](float d) { return std::clamp(d / (0.04f * g_rig.scale), 0.0f, 1.0f); };
        const float e0 = env(d0), e1 = env(d1), k0 = pulse(d0), k1 = pulse(d1);
        for (const auto& [wk, al] : {std::pair<float, float>{5.0f, 0.50f}, std::pair<float, float>{12.0f, 0.14f}}) {
            const float g0 = h0 * wk, g1 = h1 * wk;
            const float m0 = al * e0 * (0.35f + 0.65f * k0), m1 = al * e1 * (0.35f + 0.65f * k1);
            const Vec3 l[4] = {p0 - side * g0, p0, p1, p1 - side * g1}, r[4] = {p0, p0 + side * g0, p1 + side * g1, p1};
            const DWORD lc[4] = {argb(0, kHalo), argb(m0, kHalo), argb(m1, kHalo), argb(0, kHalo)};
            const DWORD rc[4] = {argb(m0, kHalo), argb(0, kHalo), argb(0, kHalo), argb(m1, kHalo)};
            add_quad(glow, eye, l, lc, 0.995f);
            add_quad(glow, eye, r, rc, 0.995f);
        }
        const float c0 = 0.9f * e0 * (0.45f + 0.55f * k0), c1 = 0.9f * e1 * (0.45f + 0.55f * k1);
        const Vec3 c[4] = {p0 - side * (h0 * 0.5f), p0 + side * (h0 * 0.5f), p1 + side * (h1 * 0.5f), p1 - side * (h1 * 0.5f)};
        const DWORD cc[4] = {argb(c0, kCore), argb(c0, kCore), argb(c1, kCore), argb(c1, kCore)};
        add_quad(glow, eye, c, cc, 0.995f);
    }
    if (g_tp.creature) {
        // Aimed at the creature: a soft pale-blue glow round it, facing the eye, breathing (its model can't
        // be lit up: it is drawn full white already).
        const float r = g_tp.creature_r * 1.25f;
        const float breathe = 0.8f + 0.2f * std::sin(float(now * 3.0));
        const Vec3 cen = g_tp.creature_c;
        const int kSegs = 40;
        for (int i = 0; i < kSegs; ++i) {
            const float a0 = 6.2831853f * float(i) / kSegs, a1 = 6.2831853f * float(i + 1) / kSegs;
            const Vec3 d0 = eye.rot.right * std::cos(a0) + eye.rot.up * std::sin(a0), d1 = eye.rot.right * std::cos(a1) + eye.rot.up * std::sin(a1);
            const auto band = [&](float ri, float ro, float ai, float ao) {
                const Vec3 q[4] = {cen + d0 * ri, cen + d0 * ro, cen + d1 * ro, cen + d1 * ri};
                const DWORD c[4] = {argb(ai, kHalo), argb(ao, kHalo), argb(ao, kHalo), argb(ai, kHalo)};
                add_quad(glow, eye, q, c, 0.99f);
            };
            band(0, r * 0.75f, 0.10f * breathe, 0.22f * breathe);
            band(r * 0.75f, r, 0.22f * breathe, 0);
        }
    }
    if (g_tp.valid && !g_tp.creature) {
        // The ring on the ground: 40 cm across at the player's scale, larger far off (about 3 degrees),
        // a soft glow inside and out of a bright band, breathing slowly; a faint disc in the middle.
        const float r = std::max(0.2f * g_rig.scale, 0.025f * vr::length(end - eye.pos));
        const float breathe = 0.8f + 0.2f * std::sin(float(now * 3.0));
        const Vec3 cen = end + Vec3{0, 0.02f * g_rig.scale, 0};
        const int kRing = 48;
        for (int i = 0; i < kRing; ++i) {
            const float a0 = 6.2831853f * float(i) / kRing, a1 = 6.2831853f * float(i + 1) / kRing;
            const Vec3 d0{std::cos(a0), 0, std::sin(a0)}, d1{std::cos(a1), 0, std::sin(a1)};
            const float band = r * 0.08f, soft = r * 0.35f;
            const auto quad = [&](float ri0, float ro0, float ai, float ao) {
                const Vec3 q[4] = {cen + d0 * ri0, cen + d0 * ro0, cen + d1 * ro0, cen + d1 * ri0};
                const DWORD c[4] = {argb(ai, kHalo), argb(ao, kHalo), argb(ao, kHalo), argb(ai, kHalo)};
                add_quad(glow, eye, q, c, 0.99f);
            };
            quad(r - band - soft, r - band, 0, 0.45f * breathe);   // (glow inside)
            quad(r - band, r + band, 0.45f * breathe, 0.45f * breathe);
            quad(r + band, r + band + soft, 0.45f * breathe, 0);   // (glow outside)
            {   // (the bright band's core)
                const Vec3 q[4] = {cen + d0 * (r - band * 0.4f), cen + d0 * (r + band * 0.4f), cen + d1 * (r + band * 0.4f), cen + d1 * (r - band * 0.4f)};
                const DWORD c[4] = {argb(0.8f, kCore), argb(0.8f, kCore), argb(0.8f, kCore), argb(0.8f, kCore)};
                add_quad(glow, eye, q, c, 0.99f);
            }
            quad(0, r - band - soft, 0.12f * breathe, 0.04f);  // (a faint disc)
        }
    }
    g_tris_additive = true;
    draw_tris(dev, glow);
    g_tris_additive = false;
}

// Soft light, no shapes (player, 2026-11-07: "stop adding UI elements"; the rings and the beam read as
// in-world UI). A mote: a small round glow facing the eye, brightest in the middle, fading to nothing
// at its edge. Drawn adding light.
uint32_t mote_hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return x;
}
float mote_rand(uint32_t seed, uint32_t k) { return float(mote_hash(seed * 7919u + k) & 0xFFFF) / 65535.0f; }
void add_mote(std::vector<Vtx>& out, const EyeView& eye, const Vec3& c, float r, float alpha, DWORD rgb) {
    if (alpha <= 0.003f || r <= 0) return;
    const Vec3 to_eye = vr::normalize(eye.pos - c);
    const Vec3 sd = vr::normalize(vr::cross(to_eye, Vec3{0, 1, 0.001f}));
    const Vec3 up = vr::normalize(vr::cross(sd, to_eye));
    const DWORD mid = (DWORD(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24) | rgb, rim = rgb;
    const int kSegs = 8;
    for (int i = 0; i < kSegs; ++i) {
        const float a0 = 6.2831853f * float(i) / kSegs, a1 = 6.2831853f * float(i + 1) / kSegs;
        const Vec3 q[4] = {c, c + (sd * std::cos(a0) + up * std::sin(a0)) * r, c + (sd * std::cos(a1) + up * std::sin(a1)) * r, c};
        const DWORD cc[4] = {mid, rim, rim, mid};
        add_quad(out, eye, q, cc, 0.995f);
    }
}
// A pool of light lying on the ground: brightest in the middle, no edge.
void add_pool(std::vector<Vtx>& out, const EyeView& eye, const Vec3& c, float r, float alpha, DWORD rgb) {
    if (alpha <= 0.003f || r <= 0) return;
    const DWORD mid = (DWORD(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24) | rgb;
    const DWORD half = (DWORD(std::clamp(alpha * 0.35f, 0.0f, 1.0f) * 255.0f) << 24) | rgb, rim = rgb;
    const int kSegs = 32;
    for (int i = 0; i < kSegs; ++i) {
        const float a0 = 6.2831853f * float(i) / kSegs, a1 = 6.2831853f * float(i + 1) / kSegs;
        const Vec3 d0{std::cos(a0), 0, std::sin(a0)}, d1{std::cos(a1), 0, std::sin(a1)};
        const auto at = [&](const Vec3& d, float k) {
            const Vec3 p = c + d * (r * k);
            return Vec3{p.x, std::max(0.0f, ground_height(p.x, p.z)) + 0.03f * g_rig_scale, p.z};
        };
        const Vec3 q0[4] = {c, at(d0, 0.45f), at(d1, 0.45f), c};
        const DWORD c0[4] = {mid, half, half, mid};
        add_quad(out, eye, q0, c0, 0.99f);
        const Vec3 q1[4] = {at(d0, 0.45f), at(d0, 1.0f), at(d1, 1.0f), at(d1, 0.45f)};
        const DWORD c1[4] = {half, rim, rim, half};
        add_quad(out, eye, q1, c1, 0.99f);
    }
}
// The gravity gloves, as in Half-Life: Alyx (the thing pointed at lights up, the glove shows it's
// ready): a few warm motes drift up about the thing picked out and fade; locked on, they brighten and
// a loose stream of them flows from it to the palm. (Was: a ring about it and a thread of energy;
// player, 2026-11-07.)
void draw_gravity_one(IDirect3DDevice7* dev, const EyeView& eye, const GravityGlove& g, float dim);
void draw_gravity(IDirect3DDevice7* dev, const EyeView& eye) {
    // (Not in a cutscene or on the cinema screen: the ring stayed on through one; player, 2026-11-07.)
    if (g_cutscene || g_cinema || now_s() - g_gg_updated_at > 0.25) return;
    draw_gravity_one(dev, eye, g_gg, 1.0f);
}
void draw_gravity_one(IDirect3DDevice7* dev, const EyeView& eye, const GravityGlove& g, float dim) {
    if ((g.st != GravityGlove::kTarget && g.st != GravityGlove::kLocked && g.st != GravityGlove::kPulling) || !g.target) return;
    const bool locked = g.st != GravityGlove::kTarget;
    const double now = g_beam_still ? 0.25 : now_s();
    constexpr DWORD kWarm = 0xFFB050, kHot = 0xFFE0A0;
    // (A thing in flight: where it is drawn, its matrix; the game's own place for it moves on its turns,
    // ten a second, and the ring jittered about it; player, 2026-11-07.)
    // (Anything else: where it is drawn too, its middle kept at the offset first seen: a person walking
    // moves on the game's turns, and the motes stepped with it.)
    Vec3 c = g.c;
    if (!IsBadReadPtr(g.target, 0x44)) {
        Vec3 p;
        if (object_pos(*reinterpret_cast<const char* const*>(static_cast<const char*>(g.target) + 0x40), p))
            c = g.in_flight || g.c_for != g.target ? p : p + g.c_off;
    }
    std::vector<Vtx> v;
    v.reserve(1024);
    const float dist = vr::length(eye.pos - c);
    // (Each mote never smaller than about half a degree across: a sheep far off still shows them.
    // Made larger, 2026-11-08, they looked low-resolution: back to this size.)
    const float mote_r = std::max(0.03f * g_rig_scale, 0.005f * dist);
    const float spread = std::clamp(g.r, 0.25f * g_rig_scale, 0.7f * g_rig_scale) * 1.1f;
    const uint32_t seed = uint32_t(reinterpret_cast<uintptr_t>(g.target));
    const int kMotes = locked ? 14 : 9;
    for (int i = 0; i < kMotes; ++i) {
        // (Each its own slow cycle: rises 40 cm, fading in and out.)
        const float period = 1.4f + 0.8f * mote_rand(seed, i * 5);
        const float ph = float(std::fmod(now / period + mote_rand(seed, i * 5 + 1), 1.0));
        const float ang = 6.2831853f * mote_rand(seed, i * 5 + 2) + 0.6f * ph;
        const float rad = spread * (0.5f + 0.5f * mote_rand(seed, i * 5 + 3));
        const float y0 = (mote_rand(seed, i * 5 + 4) - 0.6f) * spread;
        const Vec3 p = c + Vec3{std::cos(ang) * rad, y0 + ph * 0.4f * g_rig_scale, std::sin(ang) * rad};
        const float life = std::sin(3.14159265f * ph);
        add_mote(v, eye, p, mote_r * (0.7f + 0.5f * life), (locked ? 0.8f : 0.6f) * life * dim, locked ? kHot : kWarm);
    }
    // (A faint warmth about it, to carry the motes: not a shape.)
    add_mote(v, eye, c, std::max(spread * 1.2f, 0.012f * dist), (locked ? 0.18f : 0.12f) * dim, kWarm);
    if (locked) {  // the stream: motes flowing from it to the palm, along a gentle sag, wavering
        const Vec3 a = c, b = g_near.palm;
        const float len = vr::length(b - a);
        const Vec3 mid = (a + b) * 0.5f - Vec3{0, 1, 0} * (0.06f * len);
        const auto at = [&](float t) { const float u = 1.0f - t; return a * (u * u) + mid * (2.0f * u * t) + b * (t * t); };
        const int kStream = std::clamp(int(len / (0.25f * g_rig_scale)), 8, 40);
        const Vec3 side = vr::normalize(vr::cross(b - a, Vec3{0, 1, 0.001f}));
        for (int i = 0; i < kStream; ++i) {
            const float t = float(std::fmod(now * 0.9 + double(i) / kStream + 0.03 * mote_rand(seed, 100 + i), 1.0));
            const float wob = 0.05f * g_rig_scale * std::sin(float(now) * 3.0f + float(i) * 1.7f) * std::sin(3.14159265f * t);
            const Vec3 p = at(t) + side * wob + Vec3{0, wob * 0.6f, 0};
            const float r = std::max(0.02f * g_rig_scale, 0.004f * vr::length(eye.pos - p));
            add_mote(v, eye, p, r, 0.45f * std::sin(3.14159265f * t) * dim, kHot);
        }
    }
    // (A glow at the fingertips was tried: four dots by the fingers, not on them, and not missed;
    // player, 2026-11-08. Not worth it unless it rides the finger bones.)
    // (Over everything: a target behind a wall, still to be pulled, is seen.)
    g_tris_additive = true, g_tris_over_all = true;
    draw_tris(dev, v);
    g_tris_additive = false, g_tris_over_all = false;
}

// Carrying a grown villager: a faint ray from it to the ground below and a ring there, tinted by
// what it would become if put down now (the game's disciple icon for it this frame: a forester's
// green over trees, a breeder's pink, a farmer's yellow at a field, ...); white otherwise (player,
// 2026-11-07: from the teleport's height it was hard to tell where it would land and what it would
// do). Not for children: they can't be disciples.
// (By the game's icon, 0xEB99E8, which Villager::ShowDiscipleIcon (0x756170) sets from the role: farmer
// (role 1) icon 1, forester (2) 3, fisherman (3) 6, builder (4) 4, breeder (5) 2, role 6 and 9 icon 5,
// role 7 icon 0, role 8 icon 7, role 10 icon 8. The roles by the town's disciple counts, +0x6D8 + role.
// Over a field it was green and over a tree orange, read the wrong way about; player, 2026-11-07.)
const DWORD kDiscipleColour[9] = {0xC0A0FF, 0xF0D050, 0xFF80C8, 0x60D860, 0xF09050, 0xD8A858, 0x60B0FF, 0xB0F0F0, 0xB090FF};
// The grown villager the hand holds, if it holds one.
void* held_villager() {
    if (game_hand_state() != kHandStateHolding || !g_game_hand || IsBadReadPtr(static_cast<char*>(g_game_hand) + 0x4850, 4)) return nullptr;
    void* held = held_game_object();
    return held && is_a(held, ".?AVVillager@@") ? held : nullptr;
}
void* held_adult_villager() {
    if (game_hand_state() != kHandStateHolding || !g_game_hand || IsBadReadPtr(static_cast<char*>(g_game_hand) + 0x4850, 4)) return nullptr;
    void* held = held_game_object();
    if (!held || strcmp(class_name(held), ".?AVVillager@@") != 0) return nullptr;
    if (reinterpret_cast<int(__thiscall*)(void*)>(0x55C970)(held)) return nullptr;  // (Villager::IsChild)
    return held;
}
bool disciple_too_high() {
    if (!g_near.on || g_near.blend < 1.0f || g_set.disciple_max_m <= 0) return false;
    const float above = g_near.palm.y - std::max(0.0f, ground_height(g_near.palm.x, g_near.palm.z));
    return above > g_set.disciple_max_m * g_rig_scale && held_adult_villager();
}
void draw_carry_ray(IDirect3DDevice7* dev, const EyeView& eye) {
    if (!g_set.carry_ray) return;
    void* held = held_adult_villager();
    if (!held) return;
    const char* o3 = *reinterpret_cast<const char* const*>(static_cast<char*>(held) + 0x40);
    if (!o3 || IsBadReadPtr(o3 + 0x14, 48)) return;
    const float* m = reinterpret_cast<const float*>(o3 + 0x14);
    // (Where it is drawn in the hand this frame: the game's own place for it moves on its turns, ten a
    // second, and the motes falling from it jumped along; player, 2026-11-07: "a jittery frame rate".)
    const Vec3 top = g_held_seen.obj3d == o3 ? g_held_seen.pos : Vec3{m[9], m[10], m[11]};
    const Vec3 ground{top.x, std::max(0.0f, ground_height(top.x, top.z)), top.z};
    if (top.y - ground.y < 0.3f * g_rig_scale) return;  // (by the ground: no need)
    const void* icon_o3 = nullptr;
    int type = -1;
    const bool disciple = stereo::disciple_icon(icon_o3, type) && icon_o3 == o3 && type >= 0 && type < 9;
    if (disciple_too_high()) return;  // (nothing for it to become from up there: nothing shown)
    // A slow trickle of soft motes drifting down from it, and a pool of light where it would land, in
    // the colour of what it would become; over nowhere in particular only a faint white pool, to see
    // where it lands from up high. (Was: a ray and a ring; player, 2026-11-07: "softer".)
    const DWORD rgb = disciple ? kDiscipleColour[type] : 0xFFFFFF;
    const double now = g_beam_still ? 0.25 : now_s();
    std::vector<Vtx> v;
    const float drop = top.y - ground.y;
    if (disciple) {
        const int kMotes = std::clamp(int(drop / (0.3f * g_rig_scale)), 4, 16);
        for (int i = 0; i < kMotes; ++i) {
            const float t = float(std::fmod(now * 0.35 + double(i) / kMotes + 0.05 * mote_rand(7, i), 1.0));  // (0 at the villager, 1 at the ground)
            const float ang = 6.2831853f * mote_rand(11, i) + float(now) * 0.7f;
            const float rad = 0.08f * g_rig_scale * (0.4f + t);
            const Vec3 p = top + (ground - top) * t + Vec3{std::cos(ang) * rad, 0, std::sin(ang) * rad};
            const float r = std::max(0.02f * g_rig_scale, 0.004f * vr::length(eye.pos - p));
            add_mote(v, eye, p, r * 1.3f, 0.7f * std::sin(3.14159265f * t), rgb);
        }
    }
    const float pulse = 0.85f + 0.15f * std::sin(float(now) * 2.2f);
    const float pool_r = std::max(0.45f * g_rig_scale, 0.03f * vr::length(eye.pos - ground));
    add_pool(v, eye, ground, pool_r * (disciple ? 1.0f : 0.8f), (disciple ? 0.5f : 0.15f) * pulse, rgb);
    g_tris_additive = true;
    draw_tris(dev, v);
    g_tris_additive = false;
}
void draw(void* device, const EyeView& eye) {
    g_device = device;
    if (device) draw_teleport_aim(static_cast<IDirect3DDevice7*>(device), eye);
    if (device) draw_gravity(static_cast<IDirect3DDevice7*>(device), eye);
    if (device) draw_carry_ray(static_cast<IDirect3DDevice7*>(device), eye);
    if (device) draw_mana_stream(static_cast<IDirect3DDevice7*>(device), eye);
    if (!g_ptr.valid || !device) return;
    const float fade = 1.0f - g_near.blend;
    auto* dev = static_cast<IDirect3DDevice7*>(device);
    std::vector<Vtx> v;
    v.reserve(64);
    if (g_set.ground_marker && g_near.blend > 0 && g_near.on && now_s() - g_touch.t >= 0.1 && (g_near.in_reach || game_hand_state() == kHandStateHolding)) {
        // Near mode: the hand hovers above what it targets, so mark the ground straight below it
        // (where the game's hand is, and what the trigger grabs) with a diamond and a faint
        // plumb line down from the controller tip.
        const Vec3 g = g_near.ground, top = g_ptr.origin;
        const Vec3 to_eye_g = vr::normalize(eye.pos - g);
        Vec3 sd = vr::normalize(vr::cross(Vec3{0, 1, 0}, to_eye_g));
        const Vec3 upg = vr::normalize(vr::cross(to_eye_g, sd));
        const float rg = 0.012f * vr::length(eye.pos - g);
        const DWORD mc = (DWORD(224.0f * g_near.blend) << 24) | 0xFFFFFF;
        const Vec3 qd[4] = {g + sd * rg, g + upg * rg, g - sd * rg, g - upg * rg};
        const DWORD cd[4] = {mc, mc, mc, mc};
        add_quad(v, eye, qd, cd, 0.97f);
        if (top.y - g.y > 0.3f) {
            const float hw = g_set.beam_width_m * g_rig.scale * 0.35f;
            const DWORD lc = (DWORD(90.0f * g_near.blend) << 24) | 0xFFE89A;
            const Vec3 ql[4] = {top - sd * hw, top + sd * hw, g + sd * hw, g - sd * hw};
            const DWORD cl[4] = {lc, lc, lc, lc};
            add_quad(v, eye, ql, cl);
        }
    }
    // What the grip would take: a soft ring round it, facing the eye (its bounding sphere's
    // outline), so the player sees what a grip there takes: a pile in a store, a field, the thing
    // in flight, rather than only feeling the buzz.
    {
        float amount = 0;
        if (void* target = highlight_target(amount)) {
            Vec3 c;
            float r = 0;
            if (is_store(target) ? store_heap(target, c, r) : bounding_sphere(target, nullptr, c, r) && r > 0) {  // (a store: round its food heap)
                // (A thin line, its size capped: a big pile's sphere is over a metre across, centred
                // under the ground, and a ring that big covered the view.)
                r = std::clamp(r, g_set.touch_min_m * g_rig_scale, 0.4f * g_rig_scale);
                const Vec3 to = g_near.on && g_near.blend >= 1.0f ? g_near.palm - c : Vec3{};
                if (vr::length(to) > r) c = c + vr::normalize(to) * (vr::length(to) - r * 0.9f);  // (still round the palm's side of it)
                const float w = 0.002f * g_rig_scale;
                const Vec3 right = eye.rot.right, up = eye.rot.up;
                const DWORD col = (DWORD(std::clamp(amount, 0.0f, 1.0f) * 200.0f) << 24) | 0xFFF2C4;
                const DWORD cols[4] = {col, col, col, col};
                const int kSegs = 40;
                std::vector<Vtx> ring;  // (drawn in front: the thing itself would hide half of it)
                for (int i = 0; i < kSegs; ++i) {
                    const float a0 = 6.2831853f * float(i) / kSegs, a1 = 6.2831853f * float(i + 1) / kSegs;
                    const Vec3 d0 = right * std::cos(a0) + up * std::sin(a0), d1 = right * std::cos(a1) + up * std::sin(a1);
                    const Vec3 q[4] = {c + d0 * (r - w), c + d0 * (r + w), c + d1 * (r + w), c + d1 * (r - w)};
                    add_quad(ring, eye, q, cols);
                }
                // (The thing's own model lit up instead, when it is drawn as one: the ring only for what
                // isn't, as a field. Player, 2026-11-13.)
                const char* o3 = !IsBadReadPtr(target, 0x44) ? *reinterpret_cast<const char* const*>(static_cast<const char*>(target) + 0x40) : nullptr;
                // (Lit up if its model's draws were, a moment ago: else the ring.)
                if (g_glow_obj3d != o3) g_glow_hit_at = -10;
                if (g_set.glow_models) g_glow_obj3d = o3, g_glow_object = target, g_glow_amount = amount, g_glow_at = now_s(), g_glow_cool = false;
                static const void* told = nullptr;
                if (told != target && now_s() - g_glow_hit_at < 0.2)
                    told = target, BWLOG("hands: lit up: %s %p (its model %p)", class_name(target), target, o3);

                if (!g_set.glow_models || now_s() - g_glow_hit_at > 0.2) {
                    g_tris_over_all = true;
                    draw_tris(dev, ring);
                    g_tris_over_all = false;
                }
            }
        }
    }
    // The gesture's trail: a golden glow along the pen's path, fading over 1.5 s: a soft halo
    // fading to nothing at its edges, a white-hot core, and sparkles twinkling along it, all adding
    // light like the game's own magic. (It was a flat pink ribbon, "debug pink"; player, 2026-10-27.)
    if (g_gd.trail.size() > 1) {
        std::vector<Vtx> glow;
        const double now = now_s();
        const auto fade_at = [&](double t) { return float(std::clamp(1.0 - (now - t) / 1.5, 0.0, 1.0)); };
        const auto argb = [](float a, DWORD rgb) { return (DWORD(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24) | rgb; };
        constexpr DWORD kGold = 0xFFC24A, kCore = 0xFFF6DC;
        // (Points closer than the core's width to the last one used are skipped: where the pen
        // stood still they piled up, and the added light made a bright blob.)
        size_t last = 0, used = 0;
        for (size_t i = 1; i < g_gd.trail.size(); ++i) {
            const auto& [t0, p0] = g_gd.trail[last];
            const auto& [t1, p1] = g_gd.trail[i];
            const Vec3 d = p1 - p0;
            const float hw = 0.004f * g_rig.scale * std::max(1.0f, 0.15f * vr::length(eye.pos - p0) / g_rig.scale);
            if (vr::length(d) < hw * 1.2f) continue;
            last = i, ++used;
            const Vec3 side = vr::normalize(vr::cross(d, (p0 + p1) * 0.5f - eye.pos));
            const float f0 = fade_at(t0), f1 = fade_at(t1);
            // (The halo, three times the core's width: brightest at the middle, nothing at its edges.)
            const float g = hw * 3.5f;
            const Vec3 l[4] = {p0 - side * g, p0, p1, p1 - side * g}, r[4] = {p0, p0 + side * g, p1 + side * g, p1};
            const DWORD lc[4] = {argb(0, kGold), argb(0.45f * f0, kGold), argb(0.45f * f1, kGold), argb(0, kGold)};
            const DWORD rc[4] = {argb(0.45f * f0, kGold), argb(0, kGold), argb(0, kGold), argb(0.45f * f1, kGold)};
            add_quad(glow, eye, l, lc, 0.99f);
            add_quad(glow, eye, r, rc, 0.99f);
            const Vec3 c[4] = {p0 - side * (hw * 0.6f), p0 + side * (hw * 0.6f), p1 + side * (hw * 0.6f), p1 - side * (hw * 0.6f)};
            const DWORD cc[4] = {argb(0.9f * f0, kCore), argb(0.9f * f0, kCore), argb(0.9f * f1, kCore), argb(0.9f * f1, kCore)};
            add_quad(glow, eye, c, cc, 0.99f);
            // Sparkles: on every third point, a small star a little off the path, twinkling.
            if (used % 3 == 0) {
                const uint32_t h = uint32_t(t1 * 1000.0) * 2654435761u;
                const float jitter = (float(h >> 24) / 255.0f - 0.5f) * g * 1.6f;
                const float tw = 0.5f + 0.5f * std::sin(float(now * 9.0) + float(h & 0xFF) * 0.1f);
                const float a = f1 * tw;
                if (a > 0.05f) {
                    const Vec3 sp = p1 + side * jitter;
                    const Vec3 to_eye = vr::normalize(eye.pos - sp);
                    const Vec3 up = vr::normalize(vr::cross(to_eye, side));
                    const float s1 = hw * (1.5f + 2.0f * tw), s2 = s1 * 0.25f;
                    const DWORD sc[4] = {argb(0, kCore), argb(a, kCore), argb(0, kCore), argb(a, kCore)};
                    const Vec3 h1[4] = {sp - side * s1, sp - up * s2, sp + side * s1, sp + up * s2};  // (across)
                    const Vec3 h2[4] = {sp - up * s1, sp + side * s2, sp + up * s1, sp - side * s2};  // (up and down)
                    const DWORD sc2[4] = {argb(0, kCore), argb(a, kCore), argb(0, kCore), argb(a, kCore)};
                    add_quad(glow, eye, h1, sc, 0.99f);
                    add_quad(glow, eye, h2, sc2, 0.99f);
                }
            }
        }
        g_tris_additive = true;
        draw_tris(dev, glow);
        g_tris_additive = false;
    }
    if (fade <= 0) {
        draw_tris(dev, v);
        return;
    }
    draw_tris(dev, v);  // (the ground marker and the ring, covering)
    // The pointer: a beam of energy from the controller's tip to where it lands (player, 2026-11-03:
    // it looked like a debug line). A soft halo and a bright core, adding light; pulses running out
    // from the hand; a few sparks riding it; a glow where it lands.
    const Vec3 a = g_ptr.origin, b = g_ptr.hit;
    const float len = vr::length(b - a);
    if (len < 1e-3f) return;
    const double now = g_beam_still ? 0.25 : now_s();  // (tests comparing pictures hold the pulses and sparks still)
    const auto argb = [](float al, DWORD rgb) { return (DWORD(std::clamp(al, 0.0f, 1.0f) * 255.0f) << 24) | rgb; };
    constexpr DWORD kHalo = 0xFFC860, kCore = 0xFFF8E8;
    std::vector<Vtx> glow;
    glow.reserve(512);
    const Vec3 mid = (a + b) * 0.5f;
    const Vec3 side = vr::normalize(vr::cross(g_ptr.dir, mid - eye.pos));
    // Width in world units: constant near the hand, growing slowly so it stays visible far away.
    const float w0 = g_set.beam_width_m * g_rig.scale * 0.5f;
    const int kSegs = 32;
    for (int i = 0; i < kSegs; ++i) {
        const float t0 = float(i) / kSegs, t1 = float(i + 1) / kSegs;
        const float u0 = t0 * t0, u1 = t1 * t1;  // (bunched near the controller, where depth changes fastest)
        const Vec3 p0 = a + g_ptr.dir * (len * u0), p1 = a + g_ptr.dir * (len * u1);
        const float h0 = w0 * (1.0f + 0.004f * len * u0), h1 = w0 * (1.0f + 0.004f * len * u1);
        // Pulses running out along the beam (two a second).
        const auto pulse = [&](float u) {
            const float d = u * len / std::max(1e-3f, g_rig.scale);  // (metres out)
            const float ph = float(std::fmod(now * 2.0 - double(d) * 0.6, 1.0));
            return 0.5f + 0.5f * std::pow(std::max(0.0f, std::cos(ph * 6.2831853f)), 8.0f);
        };
        // Fading in from the tip, and out a little toward the far end.
        const auto env = [&](float u) { return std::clamp(u * len / (0.04f * g_rig.scale), 0.0f, 1.0f) * (1.0f - 0.45f * u); };
        const float e0 = env(u0) * fade, e1 = env(u1) * fade, k0 = pulse(u0), k1 = pulse(u1);
        // Two halos, nothing at their edges: a gold one five times the core's width, and a faint wide
        // one (twelve times) that the pulses light up.
        for (const auto& [wk, al] : {std::pair<float, float>{5.0f, 0.50f}, std::pair<float, float>{12.0f, 0.14f}}) {
            const float g0 = h0 * wk, g1 = h1 * wk;
            const float m0 = al * e0 * (0.35f + 0.65f * k0), m1 = al * e1 * (0.35f + 0.65f * k1);
            const Vec3 l[4] = {p0 - side * g0, p0, p1, p1 - side * g1}, r[4] = {p0, p0 + side * g0, p1 + side * g1, p1};
            const DWORD lc[4] = {argb(0, kHalo), argb(m0, kHalo), argb(m1, kHalo), argb(0, kHalo)};
            const DWORD rc[4] = {argb(m0, kHalo), argb(0, kHalo), argb(0, kHalo), argb(m1, kHalo)};
            add_quad(glow, eye, l, lc, 0.995f);
            add_quad(glow, eye, r, rc, 0.995f);
        }
        const float c0 = 0.9f * e0 * (0.45f + 0.55f * k0), c1 = 0.9f * e1 * (0.45f + 0.55f * k1);
        const Vec3 c[4] = {p0 - side * (h0 * 0.5f), p0 + side * (h0 * 0.5f), p1 + side * (h1 * 0.5f), p1 - side * (h1 * 0.5f)};
        const DWORD cc[4] = {argb(c0, kCore), argb(c0, kCore), argb(c1, kCore), argb(c1, kCore)};
        add_quad(glow, eye, c, cc, 0.995f);
    }
    // Sparks riding the beam out, each a small twinkling star.
    const auto star = [&](const Vec3& p, float size, float al, DWORD rgb) {
        if (al < 0.03f) return;
        const Vec3 to_e = vr::normalize(eye.pos - p);
        const Vec3 sd = vr::normalize(vr::cross(to_e, Vec3{0, 1, 0.001f}));
        const Vec3 up = vr::normalize(vr::cross(to_e, sd));
        const float s2 = size * 0.22f;
        const DWORD sc[4] = {argb(0, rgb), argb(al, rgb), argb(0, rgb), argb(al, rgb)};
        const Vec3 h1[4] = {p - sd * size, p - up * s2, p + sd * size, p + up * s2};
        const Vec3 h2[4] = {p - up * size, p + sd * s2, p + up * size, p - sd * s2};
        add_quad(glow, eye, h1, sc, 0.99f);
        add_quad(glow, eye, h2, sc, 0.99f);
    };
    const int kSparks = 8;
    for (int i = 0; i < kSparks; ++i) {
        const float ph = float(std::fmod(now * 0.7 + double(i) / kSparks, 1.0));  // (out to the end in about 1.4 s)
        const float u = ph * ph;
        const Vec3 p = a + g_ptr.dir * (len * u);
        const float w = w0 * (1.0f + 0.004f * len * u);
        const float tw = 0.6f + 0.4f * std::sin(float(now * 11.0) + float(i) * 2.1f);
        star(p, w * 6.0f, fade * tw * std::sin(ph * 3.14159f), kCore);
    }
    // Where it lands: a soft glow and a twinkle, constant angular size. The laser hand sits on that
    // spot: there it is smaller, so the hand covers it.
    const bool hand_on_it = g_near.blend <= 0 && g_ptr.hit_land && !g_ui_pointing;
    {
        const Vec3 to_eye = vr::normalize(eye.pos - b);
        const Vec3 up = vr::normalize(vr::cross(to_eye, side));
        const float r = 0.010f * vr::length(eye.pos - b) * (hand_on_it ? g_set.tip_under_hand : 1.0f);
        const float strength = (g_ptr.hit_land || g_ui_pointing ? 1.0f : 0.45f) * fade;
        const int kRim = 16;
        for (int i = 0; i < kRim; ++i) {  // (a disc, bright in the middle, nothing at the rim)
            const float a0 = 6.2831853f * float(i) / kRim, a1 = 6.2831853f * float(i + 1) / kRim;
            const Vec3 d0 = side * std::cos(a0) + up * std::sin(a0), d1 = side * std::cos(a1) + up * std::sin(a1);
            // (Small: the hand's words sit by this spot, and a wide glow over them washed them out.)
            const Vec3 q[4] = {b, b + d0 * (r * 1.8f), b + d1 * (r * 1.8f), b};
            const DWORD col[4] = {argb(0.5f * strength, kHalo), argb(0, kHalo), argb(0, kHalo), argb(0.5f * strength, kHalo)};
            add_quad(glow, eye, q, col, hand_on_it ? 0.995f : 0.97f);
        }
        const float tw = 0.75f + 0.25f * std::sin(float(now * 7.0));
        star(b, r * 1.4f * tw, 0.8f * strength, kCore);
    }
    g_tris_additive = true;
    draw_tris(dev, glow);
    g_tris_additive = false;
}

WorldFrame locomotion(const Controller ctl[2], const vr::Pose& head, const WorldFrame& game, float dt) {
    if (!g_rig.init) {
        g_rig.pos = game.base_pos;
        g_rig.yaw_deg = std::atan2(game.base_rot.fwd.x, game.base_rot.fwd.z) / vr::kDegToRad;
        g_rig.scale = g_base_scale = game.scale;
        g_rig.init = true;
        g_drag.on = false;
        BWLOG("hands: rig anchored at (%.2f, %.2f, %.2f), heading %.1f", g_rig.pos.x, g_rig.pos.y, g_rig.pos.z, g_rig.yaw_deg);
    }
    if (game.scale != g_base_scale) g_rig.scale = g_base_scale = game.scale;  // [stereo] world_scale changed
    const float rel = g_rig.scale / 10.0f;  // distances tuned at 10 units per metre follow the rig's scale
    // With one of the game's menus up (SetupBox::CurrentActiveBox), the player stays where they are:
    // with the new game's choices up, the sticks flew them about the island (playtest 2026-11-04).
    // (The stick click's recentre still works.)
    Controller locked[2];
    if (*reinterpret_cast<void* const*>(0xC4CD20) != nullptr) {
        for (int i = 0; i < 2; ++i) {
            locked[i] = ctl[i];
            locked[i].stick_x = locked[i].stick_y = 0, locked[i].a = false, locked[i].trigger = locked[i].squeeze = 0;
        }
        ctl = locked;
        g_tp.aiming = false;
    }
    const Controller& c = ctl[g_set.dominant];
    // Ray-drag: while the game's hand holds the land, keep the grabbed point under the laser.
    // This frame's trigger (update() turns it into the left button after locomotion runs).
    const bool grip = c.active && (g_lmb ? c.trigger > g_set.trigger_up : c.trigger > g_set.trigger_down);
    const bool holding_land = grip && g_lmb && game_hand_state() == kHandStateCamera;
    // Near drag: the world point under the hand stays under the hand (1:1).
    const bool near_grip = g_set.near_drag && g_near.on && c.active &&
                           (g_near.dragging ? c.squeeze > g_set.squeeze_up : c.squeeze > g_set.squeeze_down);
    if (near_grip) {
        const Vec3 hand_world = to_world(rig_frame(), c.grip.pos);
        if (!g_near.dragging) {
            g_near.dragging = true;
            g_near.grab_world = hand_world;
            vr::openvr::haptic(g_hand, 0.02f, 120.0f, 0.4f);
        } else if (!g_resisting) {
            g_rig.pos = g_rig.pos + (g_near.grab_world - hand_world);
        }
    } else {
        g_near.dragging = false;
    }
    // Laser quick travel, like the game's double click: a second trigger pull within 0.5 s of
    // the last glides the rig to the pointed spot. The head stops a little short of it and comes
    // down to standing height over the ground there (or keeps its height, if already lower).
    {
        static bool trig = false, grip_was = false;
        static float last_press = -10.0f;
        trig = c.active && (trig ? c.trigger > g_set.trigger_up : c.trigger > g_set.trigger_down);
        const uint32_t st = game_hand_state();
        const bool laser_grip = trig && !g_near.on && st != 3 && st != kHandStateHolding;
        if (laser_grip && !grip_was) {
            const bool second = g_clock - last_press < 0.5f;
            last_press = second ? -10.0f : g_clock;
            // (Not on a thing the leash can be tied to, where two pulls tie the leash, nor on the
            // player's creature, where each pull puts the leash on or off.)
            if (second && g_ptr.valid && g_ptr.hit_land && !g_travel.on && !g_leash_target && !g_on_own_creature) {
                const Vec3 h = to_world(rig_frame(), head.pos);
                const float above = std::max(g_set.head_clearance * rel, h.y - std::max(0.0f, ground_height(h.x, h.z)));
                Vec3 flat = g_ptr.hit - h;
                flat.y = 0;
                const float dist = vr::length(flat);
                const float go = std::max(0.0f, dist - g_set.travel_stop * rel);
                const Vec3 head_to = dist > 1e-3f ? h + flat * (go / dist) : h;
                Vec3 move = head_to - h;
                move.y = std::max(0.0f, ground_height(head_to.x, head_to.z)) + std::min(above, g_set.travel_height * rel) - h.y;
                const float len = vr::length(move);
                if (len > 1.0f && g_set.move_teleport) {  // (teleport movement: there at once)
                    g_rig.pos = g_rig.pos + move, g_tp_at = now_s();
                    BWLOG("hands: quick travel by teleport %.1f units to (%.1f, %.1f, %.1f)", len, g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z);
                    vr::openvr::haptic(g_hand, 0.03f, 180.0f, 0.6f);
                } else if (len > 1.0f) {
                    g_travel = {true, g_rig.pos, g_rig.pos + move, 0.0f, std::clamp(len / 300.0f, 0.35f, 0.8f)};
                    BWLOG("hands: quick travel %.1f units towards (%.1f, %.1f, %.1f)", len, g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z);
                    vr::openvr::haptic(g_hand, 0.03f, 180.0f, 0.6f);
                }
            }
        }
        grip_was = trig;
        if (g_travel.on) {
            g_travel.t = std::min(1.0f, g_travel.t + dt / g_travel.dur);
            const float e = g_travel.t * g_travel.t * (3.0f - 2.0f * g_travel.t);
            g_rig.pos = g_travel.from + (g_travel.to - g_travel.from) * e;
            if (g_travel.t >= 1.0f || g_resisting) g_travel.on = false;
        }
    }
    // Two-handed world grab (both triggers, with the hand at the controller): the two world points
    // taken hold of stay in the hands. Moving the hands together moves the world, turning them
    // about each other turns it, and pulling them apart or together scales it.
    {
        const Controller& o = ctl[1 - g_set.dominant];
        static bool mine = false, other = false;
        mine = c.active && (mine ? c.trigger > g_set.trigger_up : c.trigger > g_set.trigger_down);
        other = o.active && (other ? o.trigger > g_set.trigger_up : o.trigger > g_set.trigger_down);
        const bool both = g_set.two_hand && mine && other && g_near.on && !g_travel.on;
        if (!both) {
            g_two.on = false;
        } else if (!g_two.on) {
            const WorldFrame wf = rig_frame();
            g_two = {true, to_world(wf, c.grip.pos), to_world(wf, o.grip.pos)};
            vr::openvr::haptic(vr::kLeft, 0.02f, 120.0f, 0.4f);
            vr::openvr::haptic(vr::kRight, 0.02f, 120.0f, 0.4f);
        } else if (!g_resisting) {
            const Vec3 dt_ = o.grip.pos - c.grip.pos, dw = g_two.b - g_two.a;
            const float lt = vr::length(dt_), lw = vr::length(dw);
            if (lt > 0.05f && lw > 1e-3f) {
                g_rig.scale = g_set.two_hand_scale ? std::clamp(lw / lt, g_set.scale_min, g_set.scale_max) : g_base_scale;
                // Turn only about the vertical, and only when the hands are apart sideways enough
                // for their direction to mean something.
                if (dt_.x * dt_.x + dt_.z * dt_.z > 0.01f && dw.x * dw.x + dw.z * dw.z > 1e-4f)
                    g_rig.yaw_deg = (std::atan2(dw.x, dw.z) - std::atan2(dt_.x, dt_.z)) / vr::kDegToRad;
                const Vec3 mid = (c.grip.pos + o.grip.pos) * 0.5f;
                g_rig.pos = (g_two.a + g_two.b) * 0.5f - vr::basis_from_ypr(g_rig.yaw_deg, 0, 0).apply(mid * g_rig.scale);
            }
        }
    }
    // Lateral movement: the other hand's stick, relative to where the head faces. Speed follows
    // the player's scale and height: slow and precise low down, fast high up.
    {
        const Controller& o = ctl[1 - g_set.dominant];
        stick_click_update(o);
        const bool panel = tuning_input(o);  // (while the tuning panel is open the stick is the panel's)
        const float mx = o.active && !panel ? o.stick_x : 0.0f, my = o.active && !panel ? o.stick_y : 0.0f;
        const float mag = std::sqrt(mx * mx + my * my);
        if (g_set.move_teleport) {
            // The main hand's A held: aim from that hand; let go: there.
            const bool push = c.active && c.a && !g_resisting && !g_travel.on && !g_two.on && !g_cinema && !g_ui_pointing;
            if (push) {
                const WorldFrame wf = rig_frame();
                g_tp.from = to_world(wf, c.aim.pos);
                g_tp.local_pos = c.aim.pos, g_tp.local_dir = c.aim.rot.fwd;
                const Vec3 d = vr::normalize(wf.base_rot.apply(c.aim.rot.fwd));
                const Vec3 h = to_world(wf, head.pos);
                const float above = std::max(0.0f, h.y - std::max(0.0f, ground_height(h.x, h.z)));
                const float reach = g_set.teleport_reach_m * g_rig.scale + 2.0f * above;
                Vec3 hit;
                g_tp.valid = cast_island(g_tp.from, d, reach, hit);
                g_tp.target = g_tp.valid ? hit : g_tp.from + d * reach;
                // (The creature on the aim, before the land: its sphere, a little wider, to be easy to hit.)
                const bool was_creature = g_tp.creature;
                g_tp.creature = false;
                if (void* cr = own_creature(); cr && g_set.teleport_to_creature) {
                    Vec3 cc;
                    float rr = 0;
                    if (bounding_sphere(cr, nullptr, cc, rr) && rr > 0) {
                        const Vec3 oc = cc - g_tp.from;
                        const float t = vr::dot(oc, d);
                        const float land_t = g_tp.valid ? vr::length(g_tp.target - g_tp.from) : 1e9f;
                        if (t > 0 && t < reach + rr && vr::length(oc - d * t) < rr * 1.15f && t - rr < land_t)
                            g_tp.creature = true, g_tp.creature_c = cc, g_tp.creature_r = rr, g_tp.target = cc, g_tp.valid = true;
                    }
                }
                if (g_tp.creature && !was_creature) {
                    BWLOG("hands: teleport: aimed at the creature (r %.1f)", g_tp.creature_r);
                    vr::openvr::haptic(c.active ? g_set.dominant : g_hand, 0.02f, 150.0f, 0.4f);
                }
                if (!g_tp.aiming) BWLOG("hands: teleport: aiming (reach %.0f units)", reach);
                g_tp.aiming = true;
            } else if (g_tp.aiming) {
                g_tp.aiming = false;
                if (g_tp.valid && !g_cinema && g_tp.creature) {
                    // In front of the creature (its facing, its 3D object's z row; else from where the player
                    // is), its middle's height, 0.6 of its size and an arm's length (45 cm) from its middle,
                    // turned to face it. Its size grows over the game: so does the distance, and the head
                    // stays out of it.
                    void* cr = own_creature();
                    const Vec3 cc = g_tp.creature_c;
                    const float rr = g_tp.creature_r;
                    WorldFrame wf = rig_frame();
                    const Vec3 h = to_world(wf, head.pos);
                    Vec3 front = h - cc;
                    const char* o3 = cr && !IsBadReadPtr(static_cast<char*>(cr) + 0x44, 4) ? *reinterpret_cast<const char* const*>(static_cast<char*>(cr) + 0x40) : nullptr;
                    if (o3 && !IsBadReadPtr(o3 + 0x14, 48) && g_set.teleport_creature_front) {
                        const float* m = reinterpret_cast<const float*>(o3 + 0x14);
                        front = Vec3{m[6], 0, m[8]};
                    }
                    front.y = 0;
                    front = vr::length(front) > 1e-3f ? vr::normalize(front) : Vec3{0, 0, 1};
                    Vec3 want = cc + front * (0.6f * rr + 0.45f * g_rig.scale);
                    want.y = std::max(cc.y, std::max(0.0f, ground_height(want.x, want.z)) + g_set.head_clearance * g_rig.scale / 10.0f * 2.0f);
                    g_rig.pos = g_rig.pos + (want - h);
                    // Turned to face its middle: the head's heading (+ to the right) to the one toward it.
                    wf = rig_frame();
                    Vec3 hf = wf.base_rot.apply(head.rot.fwd);
                    hf.y = 0;
                    const Vec3 to = cc - want;
                    if (vr::length(hf) > 1e-3f && vr::length(Vec3{to.x, 0, to.z}) > 1e-3f) {
                        const float turn = std::atan2(to.x, to.z) - std::atan2(hf.x, hf.z);
                        float deg = turn / vr::kDegToRad;
                        while (deg > 180.0f) deg -= 360.0f;
                        while (deg < -180.0f) deg += 360.0f;
                        const Vec3 head_world = to_world(wf, head.pos);
                        g_rig.yaw_deg += deg;
                        const WorldFrame after = rig_frame();
                        g_rig.pos = head_world - after.base_rot.apply(head.pos * g_rig.scale);
                    }
                    g_tp_at = now_s();
                    BWLOG("hands: teleport: to the creature (r %.1f): %.1f units from its middle, at its middle's height %.1f", rr,
                          vr::length(Vec3{want.x - cc.x, 0, want.z - cc.z}), want.y);
                    vr::openvr::haptic(g_hand, 0.03f, 180.0f, 0.6f);
                } else if (g_tp.valid && !g_cinema) {
                    // The head over the spot, teleport_height_m above the ground there.
                    const WorldFrame wf = rig_frame();
                    const Vec3 h = to_world(wf, head.pos);
                    Vec3 move = g_tp.target - h;
                    move.y = std::max(0.0f, ground_height(g_tp.target.x, g_tp.target.z)) + g_set.teleport_height_m * g_rig.scale - h.y;
                    g_rig.pos = g_rig.pos + move, g_tp_at = now_s();
                    BWLOG("hands: teleport: %.1f units to (%.1f, %.1f, %.1f)", vr::length(move), g_tp.target.x, g_tp.target.y, g_tp.target.z);
                    vr::openvr::haptic(g_hand, 0.03f, 180.0f, 0.6f);
                }
            }
        }
        const bool moving = mag > 0.15f && !g_resisting && !g_travel.on && !g_two.on;
        if (!moving) g_follow_above = -1;
        if (moving) {
            const WorldFrame wf = rig_frame();
            Vec3 f = wf.base_rot.apply(head.rot.fwd);
            f.y = 0;
            if (vr::length(f) > 1e-3f) {
                f = vr::normalize(f);
                const Vec3 r = vr::cross(Vec3{0, 1, 0}, f);
                const Vec3 h = to_world(wf, head.pos);
                const float ground = std::max(0.0f, ground_height(h.x, h.z));
                const float above = std::max(0.0f, h.y - ground);
                const float speed = std::max(g_set.move_min_mps * g_rig.scale, g_set.move_speed * above) * g_set.move_scale;
                const float k = std::min(1.0f, (mag - 0.15f) / 0.85f) / mag;  // deadzone, then analog
                const Vec3 step = (f * my + r * mx) * (k * speed * dt);
                g_rig.pos = g_rig.pos + step;
                // Over the land at the height above it the head had as the move began (or as the other
                // stick set it): the ground a quarter second ahead counts too, so a hill is risen over as
                // it comes, not run into; eased (a quarter second) so the head doesn't jolt.
                if (g_set.follow_ground) {
                    if (g_follow_above < 0) g_follow_above = above;
                    const Vec3 ahead = h + step * (0.25f / std::max(dt, 1e-3f));
                    const float g_here = std::max(0.0f, ground_height(h.x + step.x, h.z + step.z));
                    const float g_ahead = std::max(0.0f, ground_height(ahead.x, ahead.z));
                    const float want = std::max(g_here, g_ahead) + g_follow_above;
                    const float a = 1.0f - std::exp(-dt / 0.25f);
                    g_rig.pos.y += (want - h.y) * a;
                }
            }
        }
    }
    if (holding_land && !g_travel.on) {
        const WorldFrame wf = rig_frame();
        const Vec3 o = to_world(wf, c.aim.pos);
        const Vec3 d = vr::normalize(wf.base_rot.apply(c.aim.rot.fwd));
        if (g_resisting) {
            g_drag.on = false;  // re-grab once the game accepts the position again
        } else if (!g_drag.on) {
            g_drag.on = true;
            g_drag.grab = g_ptr.hit_land ? g_ptr.hit : o + d * 50.0f;
        } else if (d.y < -0.05f) {
            const Vec3 p = o + d * ((g_drag.grab.y - o.y) / d.y);
            Vec3 t = g_drag.grab - p;
            t.y = 0;
            // A far grab turns a small wrist movement into a long trip. Cap the speed to a few
            // times the height above the ground (low down stays precise, high up covers the
            // island) so the world glides instead of jumping; the grabbed point catches up.
            const float above = std::max(1.0f, g_rig.pos.y - std::max(0.0f, ground_height(g_rig.pos.x, g_rig.pos.z)));
            const float cap = std::clamp(g_set.drag_speed * above, 40.0f, 800.0f) * std::max(dt, 1e-3f);
            const float tl = vr::length(t);
            if (tl > cap) t = t * (cap / tl);
            g_rig.pos = g_rig.pos + t;
        }
    } else {
        g_drag.on = false;
    }
    // Turning about the head (thumbstick x): in snaps (g_set.snap_deg, re-armed below 0.3), or
    // smoothly (g_set.smooth_turn_dps at full stick, past a 0.2 deadzone).
    const float sx = c.active ? c.stick_x : 0.0f;
    const auto turn_by = [&](float deg) {
        const WorldFrame before = rig_frame();
        const Vec3 head_world = to_world(before, head.pos);
        g_rig.yaw_deg += deg;
        lesson_amounts(deg, 0, 0);
        const WorldFrame after = rig_frame();
        g_rig.pos = head_world - after.base_rot.apply(head.pos * g_rig.scale);
    };
    if (g_set.turn_smooth) {
        if (std::fabs(sx) > 0.2f) turn_by((sx > 0 ? 1.0f : -1.0f) * (std::fabs(sx) - 0.2f) / 0.8f * g_set.smooth_turn_dps * dt);
        g_turn_latched = false;
    } else if (!g_turn_latched && std::fabs(sx) > 0.7f) {
        g_turn_latched = true;
        turn_by(sx > 0 ? g_set.snap_deg : -g_set.snap_deg);
    } else if (std::fabs(sx) < 0.3f) {
        g_turn_latched = false;
    }
    // Rise/descend (thumbstick y), faster when higher up.
    const float sy = c.active ? c.stick_y : 0.0f;
    if (std::fabs(sy) > 0.2f) {
        // ... and no slower than a metre a second at the player's own scale.
        const float speed = std::max(g_rig.scale, 10.0f + 0.8f * std::max(0.0f, g_rig.pos.y));
        const float y0 = g_rig.pos.y;
        g_rig.pos.y = std::max(1.0f, g_rig.pos.y + sy * speed * dt);
        lesson_amounts(0, (g_rig.pos.y - y0) / std::max(1e-3f, g_rig.scale), 0);
        if (g_follow_above >= 0) g_follow_above = std::max(0.0f, g_follow_above + (g_rig.pos.y - y0));  // (the height kept moving)
    }
    {   // (the head's nod: the pitch lesson)
        static float pitch0 = 0;
        static bool have = false;
        const float pitch = std::asin(std::clamp(head.rot.fwd.y, -1.0f, 1.0f)) / vr::kDegToRad;
        if (have) lesson_amounts(0, 0, pitch - pitch0);
        pitch0 = pitch, have = true;
    }
    // Allowed zone: remember positions the game accepts; ease back when it resists.
    g_clock += dt;
    if (g_resisting) {
        g_calm_time = 0;
        g_rig_history.clear();
        if (g_have_good) {
            const float k = 1.0f - std::exp(-dt * 6.0f);
            g_rig.pos = g_rig.pos + (g_last_good - g_rig.pos) * k;
        }
    } else {
        g_calm_time += dt;
        g_rig_history.push_back({g_clock, g_rig.pos});
        // The newest position at least 0.3 s old becomes the fallback.
        size_t keep = 0;
        while (keep + 1 < g_rig_history.size() && g_rig_history[keep + 1].t <= g_clock - 0.3f) ++keep;
        if (g_rig_history[keep].t <= g_clock - 0.3f) {
            g_last_good = g_rig_history[keep].p;
            g_have_good = true;
        }
        g_rig_history.erase(g_rig_history.begin(), g_rig_history.begin() + keep);
    }
    // Keep the head out of the hills: a drag can pull the rig into rising ground.
    {
        const WorldFrame wf = rig_frame();
        const Vec3 h = to_world(wf, head.pos);
        const float lift = std::max(0.0f, ground_height(h.x, h.z)) + g_set.head_clearance * rel - h.y;
        if (lift > 0) g_rig.pos.y += lift;
    }
    return rig_frame();
}

WorldFrame carry(const WorldFrame& game, const vr::Pose& head, bool start) {
    static Vec3 head0;
    static float head_yaw0 = 0;
    if (!g_rig.init) {
        g_rig.scale = g_base_scale = game.scale;
        g_rig.init = true;
        start = true;
    }
    if (start) {
        head0 = head.pos;
        head_yaw0 = std::atan2(head.rot.fwd.x, head.rot.fwd.z) / vr::kDegToRad;
    }
    g_rig.yaw_deg = std::atan2(game.base_rot.fwd.x, game.base_rot.fwd.z) / vr::kDegToRad - head_yaw0;
    g_rig.pos = game.base_pos - vr::basis_from_ypr(g_rig.yaw_deg, 0, 0).apply(head0 * g_rig.scale);
    // Nothing of the player's own movement carries over, and where the cutscene leaves the rig is
    // where play resumes: no easing back to an earlier position.
    g_drag.on = false;
    g_travel.on = false;
    g_two.on = false;
    g_near.dragging = false;
    g_resisting = false;
    g_have_good = false;
    g_rig_history.clear();
    return rig_frame();
}

WorldFrame hold(const WorldFrame& game) {
    if (!g_rig.init) {
        g_rig.pos = game.base_pos;
        g_rig.yaw_deg = std::atan2(game.base_rot.fwd.x, game.base_rot.fwd.z) / vr::kDegToRad;
        g_rig.scale = g_base_scale = game.scale;
        g_rig.init = true;
    }
    g_drag.on = false;
    g_travel.on = false;
    g_two.on = false;
    g_resisting = false;
    return rig_frame();
}
void set_cinema(bool on, bool cutscene) { g_cinema = on, g_cutscene = on && cutscene; }

HintControl hint_control(const wchar_t* word) {
    // With the leash, the game alternates two words on a thing: the click's ("Attach Leash",
    // now the trigger) and the held button's ("Pick Up", still the grip).
    if (g_leash_target) {
        if (word && wcscmp(word, L"Pick Up") == 0) return g_set.trigger_carries ? HintControl::kHoldTrigger : HintControl::kGrip;
        if (word && wcscmp(word, L"Attach Leash") == 0) return HintControl::kDoubleTrigger;
        return HintControl::kTrigger;
    }
    if (g_tappable || g_fight) return HintControl::kTrigger;
    if (g_set.trigger_carries && word && wcscmp(word, L"Pick Up") == 0) return HintControl::kHoldTrigger;
    // With the leash on, the game's word for its land click (sending the creature there) is now
    // the trigger's job.
    if (word && wcscmp(word, L"Focus Creature") == 0) return HintControl::kTrigger;
    if (game_hand_state() == kHandStateHolding) return HintControl::kLetGo;
    if (game_hand_state() == kHandStateCharged) return HintControl::kTrigger;  // (a miracle in the hand: the trigger casts or pours it)
    if (g_near.on && now_s() - g_on_creature_at < 0.25) return HintControl::kNone;  // a hand on the creature pets by itself
    return g_set.trigger_carries ? HintControl::kTrigger : HintControl::kGrip;
}

void rig_reset() {
    g_rig.init = false;
    g_have_good = false;
    g_rig_history.clear();
}
void set_game_resisting(bool resisting) { g_resisting = resisting; }
bool rig_anchored() { return g_rig.init; }
float rig_scale() { return g_rig.scale; }
// Where each kind of person or animal sits in the hand (playtest 2026-11-01 evening, dialled in by
// the player: a person 1 cm out from the palm and 5 toward the fingers, a sheep 2 / 10, a cow 0 / 4,
// a horse 5 / 14 and 5 up). The tuning panel's "Held ..." lines edit the kind in the hand (its values
// are g_set.held_living_* while it is held); each kind is kept in bwvr.ini's [held], "out,fwd,up" in cm.
struct KindPlace {
    std::wstring name;
    float out, fwd, up;  // metres
};
std::vector<KindPlace> g_kind_places = {
    {L"Villager", 0.01f, 0.05f, 0.0f}, {L"Sheep", 0.02f, 0.10f, 0.0f}, {L"Cow", 0.0f, 0.04f, 0.0f}, {L"Horse", 0.05f, 0.14f, 0.05f},
    {L"Rock", 0.0f, 0.06f, 0.0f},  // (2026-11-13: a rock caught up with the hand over half a second, jumping about)
};
constexpr KindPlace kAnimalDefault{L"", 0.02f, 0.08f, 0.0f};
std::wstring g_cur_kind;  // the kind whose values g_set.held_living_* hold now ("" none yet)
KindPlace& kind_place(const std::wstring& name) {
    for (KindPlace& k : g_kind_places)
        if (k.name == name) return k;
    KindPlace k = kAnimalDefault;
    k.name = name;
    wchar_t buf[64] = {};  // (one the defaults don't name: from bwvr.ini, if it was saved)
    GetPrivateProfileStringW(L"held", name.c_str(), L"", buf, 64, paths::ini_file().c_str());
    float o = 0, f = 0, u = 0;
    if (swscanf_s(buf, L"%f,%f,%f", &o, &f, &u) == 3) k.out = o / 100.0f, k.fwd = f / 100.0f, k.up = u / 100.0f;
    g_kind_places.push_back(k);
    return g_kind_places.back();
}
void held_kinds_load() {
    for (KindPlace& k : g_kind_places) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"held", k.name.c_str(), L"", buf, 64, paths::ini_file().c_str());
        float o = 0, f = 0, u = 0;
        if (swscanf_s(buf, L"%f,%f,%f", &o, &f, &u) == 3) k.out = o / 100.0f, k.fwd = f / 100.0f, k.up = u / 100.0f;
    }
}
void held_kinds_save() {
    if (!g_cur_kind.empty()) {
        KindPlace& k = kind_place(g_cur_kind);
        k.out = g_set.held_living_out, k.fwd = g_set.held_living_fwd, k.up = g_set.held_living_up;
    }
    for (const KindPlace& k : g_kind_places) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.0f,%.0f,%.0f", k.out * 100.0f, k.fwd * 100.0f, k.up * 100.0f);
        WritePrivateProfileStringW(L"held", k.name.c_str(), buf, paths::ini_file().c_str());
    }
}
// The held thing's kind: its class without RTTI's decoration ("Sheep"); any villager "Villager".
std::wstring kind_of(const void* held) {
    if (is_a(held, ".?AVVillager@@")) return L"Villager";
    std::string n = class_name(held);
    if (n.rfind(".?AV", 0) == 0) n = n.substr(4);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "@@") == 0) n.resize(n.size() - 2);
    return std::wstring(n.begin(), n.end());
}
// The kind in the hand now: its values into g_set.held_living_* (the last kind's kept first).
void sync_held_kind(const void* held) {
    const std::wstring kind = kind_of(held);
    if (kind == g_cur_kind) return;
    if (!g_cur_kind.empty()) {
        KindPlace& old = kind_place(g_cur_kind);
        old.out = g_set.held_living_out, old.fwd = g_set.held_living_fwd, old.up = g_set.held_living_up;
    }
    const KindPlace& k = kind_place(kind);
    g_set.held_living_out = k.out, g_set.held_living_fwd = k.fwd, g_set.held_living_up = k.up;
    g_cur_kind = kind;
    BWLOG("hands: held: a %ls (%.0f cm out, %.0f toward the fingers, %.0f up)", kind.c_str(), k.out * 100.0f, k.fwd * 100.0f, k.up * 100.0f);
}
std::wstring held_kind_name() {
    if (g_cur_kind.empty()) return L"person/animal";
    return g_cur_kind == L"Villager" ? L"person" : g_cur_kind;
}
int held_kind() {
    if (!g_game_hand || IsBadReadPtr(static_cast<char*>(g_game_hand) + 0x4850, 4)) return 0;
    if (game_hand_state() == 8) return 2;
    void* held = held_game_object();
    if (held && strcmp(class_name(held), ".?AVRock@@") == 0) {
        sync_held_kind(held);
        return 3;
    }
    if (!held || !is_a(held, ".?AVLiving@@")) return 0;
    sync_held_kind(held);
    return 1;
}
void give_miracle(int type) {
    if (type >= 0 && type < 30) g_give_type = type;
}
HeldPlace held_place(int kind) {
    if (kind == 1) return {g_set.held_living_place, 0.0f, g_set.held_living_fwd, g_set.held_living_up, g_set.held_living_out};
    // (A rock: its middle out by half its half-thickness, the fingers closed on it.)
    if (kind == 3) return {g_set.held_living_place, 0.5f, g_set.held_living_fwd, g_set.held_living_up, g_set.held_living_out};
    return {true, g_set.held_miracle_out, g_set.held_miracle_fwd, g_set.held_miracle_up};
}
void set_game_hand(void* chand) { g_game_hand = chand; }

void reset() {
    set_lmb(false);
    set_rmb(false);
    if (g_cursor_on) input::set_vr_cursor(false, 0, 0);
    g_cursor_on = false;
    g_ptr = PointerState{};
    g_rig.init = false;
    g_near = Near{};
}

bool command(const std::string& k, std::istringstream& in) {
    if (k == "ctrl") {
        // ctrl <l|r> pose yaw pitch roll x y z | trigger v | squeeze v | a 0|1 | b 0|1 | stick x y | off
        std::string side, what;
        in >> side >> what;
        Controller& c = g_mock[side == "l" ? vr::kLeft : vr::kRight];
        if (what == "pose") {
            float yaw = 0, pitch = 0, roll = 0;
            Vec3 p;
            in >> yaw >> pitch >> roll >> p.x >> p.y >> p.z;
            c.active = true;
            c.aim.rot = c.grip.rot = vr::basis_from_ypr(yaw, pitch, roll);
            c.aim.pos = c.grip.pos = p;
        } else if (what == "aimat") {
            // Point at a world point from the controller's current position (rig as it is now).
            Vec3 t;
            in >> t.x >> t.y >> t.z;
            if (!g_rig.init) return false;
            const WorldFrame wf = rig_frame();
            const Vec3 d = vr::basis_from_ypr(-g_rig.yaw_deg, 0, 0).apply(vr::normalize(t - to_world(wf, c.aim.pos)));
            const float yaw = std::atan2(d.x, d.z) / vr::kDegToRad, pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / vr::kDegToRad;
            c.active = true;
            c.aim.rot = c.grip.rot = vr::basis_from_ypr(yaw, pitch, 0);
        } else if (what == "trigger") in >> c.trigger;
        else if (what == "squeeze") in >> c.squeeze;
        else if (what == "sweep") in >> g_mock_sweep[side == "l" ? vr::kLeft : vr::kRight];
        else if (what == "aimcircle") {
            MockAimCircle& ac = g_mock_aim_circle[side == "l" ? vr::kLeft : vr::kRight];
            in >> ac.c.x >> ac.c.y >> ac.c.z >> ac.r >> ac.T >> ac.turns;
            ac.t0 = now_s();
        } else if (what == "spiral") {
            MockSpiral& sp = g_mock_spiral[side == "l" ? vr::kLeft : vr::kRight];
            std::string dir;
            in >> sp.T >> sp.turns >> sp.r0 >> sp.r1 >> dir;
            sp.sgn = dir == "ccw" ? -1.0f : 1.0f;
            sp.centre = c.grip.pos - Vec3{0, sp.r0, 0};
            sp.t0 = now_s();
        } else if (what == "vel") {  // ctrl <l|r> vel x y z: moves the controller at that speed (m/s, tracking space)
            Vec3& v = g_mock_vel[side == "l" ? vr::kLeft : vr::kRight];
            in >> v.x >> v.y >> v.z;
        } else if (what == "squeeze_after") {  // ctrl <l|r> squeeze_after seconds value
            MockSqueeze& m = g_mock_squeeze_at[side == "l" ? vr::kLeft : vr::kRight];
            double after = 0;
            in >> after >> m.value;
            m.at = now_s() + after;
        }
        else if (what == "a") in >> c.a;
        else if (what == "b") in >> c.b;
        else if (what == "feedback") g_feedback_requested = true;  // (as the other hand's B)
        else if (what == "stick") in >> c.stick_x >> c.stick_y;
        else if (what == "click") in >> c.stick_click;
        else if (what == "off") c = Controller{}, g_mock_vel[side == "l" ? vr::kLeft : vr::kRight] = {};
        else return false;
        return true;
    }
    if (k == "hands") {
        // hands pointer 0|1 | dominant l|r
        std::string what;
        in >> what;
        // (Kept out of the long chain below: the compiler's limit on nested blocks.)
        if (what == "mana_own") { in >> g_mana_own; return true; }
        if (what == "beam_still") { in >> g_beam_still; return true; }
        if (what == "gg_checks") { in >> g_gg_check_drawn >> g_gg_check_sight; return true; }
        if (what == "gg_log") { in >> g_gg_log; return true; }
        if (what == "gesture_anywhere") { in >> g_gesture_anywhere; return true; }
        if (what == "pointer") in >> g_set.pointer;
        else if (what == "vr_throw") in >> g_set.vr_throw;
        else if (what == "drag_speed") in >> g_set.drag_speed;
        else if (what == "beam_ztest") in >> g_set.beam_ztest;
        else if (what == "depth_k") in >> g_set.depth_k;
        else if (what == "near") in >> g_set.near_enabled;
        else if (what == "always") in >> g_set.always_hand;
        else if (what == "reach") in >> g_set.reach_m;
        else if (what == "living_reach") in >> g_set.living_reach_m;
        else if (what == "living_grip") in >> g_set.living_grip_m;
        else if (what == "gg_view_deg" || what == "prehold") {  // (prehold 0|1: a pick-up on its way drawn at the palm)
            if (what == "prehold") {
                in >> g_prehold_on;
                int hold = -1, tree = -1;
                if (in >> hold) g_hold_through_taking = hold != 0;
                if (in >> tree) g_prehold_tree_draw_only = tree != 0;
            } else {
                in >> g_gg_view_deg;
            }
        }
        else if (what == "held_living") in >> g_set.held_living_place >> g_set.held_living_out >> g_set.held_living_fwd >> g_set.held_living_up;
        else if (what == "held_miracle") in >> g_set.held_miracle_out >> g_set.held_miracle_fwd >> g_set.held_miracle_up;
        else if (what == "touch") in >> g_set.touch_grab;
        else if (what == "pet") in >> g_set.pet;
        else if (what == "give") in >> g_set.give;
        else if (what == "creatures") {  // debug: where the game's creatures are
            const char* node = *reinterpret_cast<const char* const*>(0xC5FCF8);
            for (int i = 0; node && i < 16 && !IsBadReadPtr(node, 8); ++i) {
                void* creature = *reinterpret_cast<void* const*>(node + 4);
                node = *reinterpret_cast<const char* const*>(node);
                Vec3 c{};
                float r = 0;
                const bool ok = creature && !IsBadReadPtr(creature, 0x44) && bounding_sphere(creature, nullptr, c, r);
                BWLOG("hands: creature %d %p sphere %d (%.2f, %.2f, %.2f) r %.2f%s", i, creature, ok, c.x, c.y, c.z, r, creature == own_creature() ? " (the player's)" : "");
            }
        }
        else if (what == "in_flight") {  // debug: what counts as in flight (catchable) now
            std::string names;
            for (const void* o : g_in_flight) names += std::string(class_name(o)) + " ";
            BWLOG("hands: in flight: %zu %s", g_in_flight.size(), names.c_str());
        }
        else if (what == "held") {  // debug: what the game's hand holds (a pot's amount: +0x70)
            void* status = g_game_hand ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850) : nullptr;
            void* held = status ? *reinterpret_cast<void**>(static_cast<char*>(status) + 0x120) : nullptr;
            BWLOG("hands: held %s amount %d (state %u)", held ? class_name(held) : "-",
                  held ? *reinterpret_cast<const int*>(static_cast<char*>(held) + 0x70) : -1, game_hand_state());
        }
        else if (what == "clicked") {  // debug: the game's last clicked object (what GAME_THING_CLICKED asks about)
            void* gi = game_interface();
            using LastFn = void*(__thiscall*)(void*);
            void* o = gi ? reinterpret_cast<LastFn>(0x5D0730)(gi) : nullptr;
            BWLOG("hands: last clicked %p (%s)", o, o && !IsBadReadPtr(o, 4) ? class_name(o) : "-");
        }
        else if (what == "lit") {  // debug: what the ring is round now
            float amount = 0;
            void* t = highlight_target(amount);
            BWLOG("hands: lit %s (touch %s)", t ? class_name(t) : "-", g_touch.object && !IsBadReadPtr(g_touch.object, 4) ? class_name(g_touch.object) : "-");
        }
        else if (what == "objects") {
            // debug: hands objects x z radius: the objects in the map cells around (x, z). A
            // map cell is 10 units; GGame +0x59FC holds the cells ({first mobile, first fixed},
            // 8 bytes each, by x cell * +0x59C4 + z cell; +0x59C8 is the number of x cells), and
            // an object's next in its cell is at +0x20.
            float x = 0, z = 0, radius = 30;
            in >> x >> z >> radius;
            const auto* game = static_cast<const char*>(game::game_instance());
            const int n = int(radius / 10.0f) + 1, cx = int(x / 10.0f), cz = int(z / 10.0f);
            const uint32_t nx = game ? *reinterpret_cast<const uint32_t*>(game + 0x59C8) : 0;
            const uint32_t nz = game ? *reinterpret_cast<const uint32_t*>(game + 0x59C4) : 0;
            int k_left = 2000;  // (in case a list loops)
            for (int i = cx - n; i <= cx + n; ++i)
                for (int j = cz - n; j <= cz + n; ++j) {
                    if (i < 0 || j < 0 || uint32_t(i) >= nx || uint32_t(j) >= nz) continue;
                    for (int list = 0; list < 2; ++list)  // mobile, then fixed
                    for (const char* o = *reinterpret_cast<const char* const*>(game + 0x59FC + (size_t(i) * nz + j) * 8 + list * 4);
                         o && !IsBadReadPtr(o, 0x44) && k_left-- > 0; o = *reinterpret_cast<const char* const*>(o + 0x20)) {
                        const int32_t* c = reinterpret_cast<const int32_t*>(o + 0x14);
                        const float ox = float(c[0]) * 10.0f / 65536.0f, oz = float(c[1]) * 10.0f / 65536.0f;
                        const char* name = class_name(o);
                        unsigned tap = 0;  // (Object::InterfaceValidToTap, vtable 0x740)
                        if (void* status = g_game_hand ? *reinterpret_cast<void**>(static_cast<char*>(g_game_hand) + 0x4850) : nullptr) {
                            if (const auto fn = reinterpret_cast<unsigned(__thiscall*)(const void*, void*)>(game_method(o, 0x740))) tap = fn(o, status);
                        }
                        void* held = object_in_hand();  // (would the game apply what the hand holds to it, dropped onto it?)
                        const bool applies = held && held != o && applies_to(held, const_cast<char*>(o));
                        Vec3 bc{};
                        float br = -1;
                        bounding_sphere(const_cast<char*>(o), nullptr, bc, br);
                        BWLOG("hands: object %p %s at (%.1f, +%.1f, %.1f)%s%s%s%s, sphere (%.1f, %.1f, %.1f) r %.1f", o, name, ox,
                              reinterpret_cast<const float*>(c)[2], oz, list ? " fixed" : "", tap ? " tappable" : "", applies ? " applies" : "",
                              liftable(const_cast<char*>(o)) ? " liftable" : "",
                              bc.x, bc.y, bc.z, br);
                    }
                }
        }
        else if (what == "findptr") {  // debug: hands findptr <object hex> <bytes hex> <vtable hex>: pointers in it to objects of that class
            std::string o, n, vt;
            in >> o >> n >> vt;
            const char* obj = reinterpret_cast<const char*>(uintptr_t(strtoul(o.c_str(), nullptr, 16)));
            const size_t bytes = strtoul(n.c_str(), nullptr, 16);
            const uintptr_t want = strtoul(vt.c_str(), nullptr, 16);
            int found = 0;
            for (size_t off = 0; obj && off + 4 <= bytes && !IsBadReadPtr(obj + off, 4); off += 4) {
                const char* q = *reinterpret_cast<const char* const*>(obj + off);
                if (q && !IsBadReadPtr(q, 4) && *reinterpret_cast<const uintptr_t*>(q) == want)
                {
                    Vec3 c{};
                    float r = -1;
                    bounding_sphere(const_cast<char*>(q), nullptr, c, r);
                    void* held = object_in_hand();
                    BWLOG("hands: findptr: %p +0x%zX -> %p %s, sphere (%.1f, %.1f, %.1f) r %.1f%s", obj, off, q, class_name(q), c.x, c.y, c.z, r,
                          held && applies_to(held, const_cast<char*>(q)) ? ", what the hand holds applies to it" : "");
                    ++found;
                }
            }
            BWLOG("hands: findptr: %d found", found);
        }
        else if (what == "apply_to") {  // debug: hands apply_to <hex>: the game applies what the hand holds to that object (fn_005DA1A0)
            std::string hex;
            in >> hex;
            void* target = reinterpret_cast<void*>(uintptr_t(strtoul(hex.c_str(), nullptr, 16)));
            void* gi = game_interface();
            void* held = object_in_hand();
            const int valid = held && target ? int(applies_to(held, target)) : -1;
            const int r = gi && valid == 1 ? reinterpret_cast<int(__thiscall*)(void*, void*, void*)>(0x5DA1A0)(gi, target, nullptr) : -1;
            BWLOG("hands: apply_to: %s onto %p %s: valid %d, applied %d", held ? class_name(held) : "-", target, target ? class_name(target) : "-", valid, r);
        }
        else if (what == "leash") {  // debug: the player's leash (GLeashStatus, see leash_in_hand)
            const char* gi = static_cast<const char*>(game_interface());
            const char* player = gi && !IsBadReadPtr(gi + 0x39C, 4) ? *reinterpret_cast<const char* const*>(gi + 0x39C) : nullptr;
            const uint32_t* ls = player && !IsBadReadPtr(player + 0x12C, 4) ? *reinterpret_cast<const uint32_t* const*>(player + 0x12C) : nullptr;
            const auto* game = static_cast<const uint8_t*>(game::game_instance());
            const uint8_t* creature = game ? *reinterpret_cast<const uint8_t* const*>(game + 0xA64 + 0xA60 * game[0x205A59]) : nullptr;
            if (ls && !IsBadReadPtr(ls, 0x40))
                BWLOG("hands: leash status %p: %08X %08X %08X %08X %08X %08X %08X %08X | %08X %08X %08X %08X %08X %08X %08X %08X", ls,
                      ls[0], ls[1], ls[2], ls[3], ls[4], ls[5], ls[6], ls[7], ls[8], ls[9], ls[10], ls[11], ls[12], ls[13], ls[14], ls[15]);
            BWLOG("hands: leash interface %p player %p creature %p, in hand %d, target %p", gi, player, creature, leash_in_hand(),
                  leash_target_under_hand());
            BWLOG("hands: leash start: game (%.2f, %.2f, %.2f) used (%.2f, %.2f, %.2f) palm (%.2f, %.2f, %.2f)", g_leash_game.x, g_leash_game.y,
                  g_leash_game.z, g_leash_drawn.x, g_leash_drawn.y, g_leash_drawn.z, g_near.palm.x, g_near.palm.y, g_near.palm.z);
            if (creature && !IsBadReadPtr(creature + 0x380, 4))
                BWLOG("hands: leash creature %p control %u, hand state %u, picked %p", creature,
                      *reinterpret_cast<const uint32_t*>(creature + 0x380), game_hand_state(), g_picked);
        }
        else if (what == "leash_to_hand") in >> g_set.leash_to_hand;
        else if (what == "leash_start_log") in >> g_leash_start_log;
        else if (what == "laser_to_creature") {  // debug: shift the rig so the laser's point is on the player's creature now
            Vec3 c;
            float r = 0;
            void* own = own_creature();
            if (own && bounding_sphere(own, nullptr, c, r)) g_rig.pos.x += c.x - g_ptr.hit.x, g_rig.pos.z += c.z - g_ptr.hit.z;
        }
        else if (what == "follow_creature") in >> g_follow_creature;  // debug: keep the palm on the creature (it may walk)
        else if (what == "to_creature") {  // debug: move the rig so the palm is on the player's creature now
            Vec3 c;
            float r = 0;
            void* own = own_creature();
            if (own && bounding_sphere(own, nullptr, c, r)) {
                g_rig.pos = g_rig.pos + (c - g_near.palm);
                BWLOG("hands: rig moved by (%.2f, %.2f, %.2f) to put the palm on the creature", c.x - g_near.palm.x, c.y - g_near.palm.y,
                      c.z - g_near.palm.z);
            }
        }
        else if (what == "leash_type") {  // debug: take a leash as tapping a temple leash does (packet 0x65)
            int type = 1;
            in >> type;
            using SetPacketFn = void(__thiscall*)(void*, int, uint16_t, uint16_t);
            if (void* game = game::game_instance()) reinterpret_cast<SetPacketFn>(0x550C50)(game, 0x65, uint16_t(type), 0);
            BWLOG("hands: leash type %d requested", type);
        }
        else if (what == "leash_on" || what == "leash_off" || what == "send_to_self" || what == "give_packet" || what == "give_finish_packet") {  // debug: the leash to the hand (packet 0x5C, no object) or dropped (0x5B)
            // (give_packet / give_finish_packet, debug: the game's give (0x1F) or its finish (0x52) with the creature's id, as the hand-over sends them)
            if (what == "give_packet" || what == "give_finish_packet") {
                void* own = own_creature();
                using IdFn = uint32_t(__thiscall*)(void*);
                using SetPacketIdFn2 = void(__thiscall*)(void*, int, uint32_t);
                if (own) {
                    const uint32_t id = reinterpret_cast<IdFn>(0x436A70)(own);
                    if (void* game = game::game_instance()) reinterpret_cast<SetPacketIdFn2>(0x550C80)(game, what == "give_packet" ? 0x1F : 0x52, id);
                    BWLOG("hands: debug: packet 0x%X with the creature's id %u", what == "give_packet" ? 0x1F : 0x52, id);
                }
            } else
            // (send_to_self, debug: the game's send-to-a-thing with the creature itself, as a petting press with the leash did)
            if (what == "send_to_self" && in.peek() != EOF) {  // (send_to_self finish: the game's force finish, as the watch makes it; send_to_self watch 0|1)
                std::string sub;
                in >> sub;
                if (sub == "finish") creature_force_finish(own_creature(), "debug");
                else if (sub == "watch") in >> g_freeze_watch;
            } else if (what == "send_to_self") {
                void* own = own_creature();
                void* gi = game_interface();
                BWLOG("hands: debug: sending the creature to itself");
                if (own && gi) reinterpret_cast<SendToFn>(0x5D3930)(gi, nullptr, own);
            } else {
                using SetPacketIdFn = void(__thiscall*)(void*, int, uint32_t);
                if (what == "leash_on" && leash_on_flag() == 0) leash_toggle();  // (0x5C only re-takes a leash that is on)
                else if (void* game = game::game_instance()) reinterpret_cast<SetPacketIdFn>(0x550C80)(game, what == "leash_on" ? 0x5C : 0x5B, 0);
                BWLOG("hands: %s requested", what == "leash_on" ? "leash to the hand" : "leash dropped");
            }
        }
        else if (what == "pet_gain") in >> g_set.pet_gain;
        else if (what == "pet_by_speed") in >> g_set.pet_by_speed;
        else if (what == "slap_speed") in >> g_set.slap_m_s;
        else if (what == "stroke_speed") in >> g_set.stroke_m_s;
        else if (what == "pet_log") in >> g_pet_log;
        else if (what == "pet_speed") in >> g_pet_speed_test;
        else if (what == "hand_fields") {  // debug: the game hand's interaction fields
            const char* h = static_cast<const char*>(g_game_hand);
            if (h && !IsBadReadPtr(h + 0x4900, 0xC0)) {
                const void* o = *reinterpret_cast<void* const*>(h + 0x4904);
                BWLOG("hands: game hand +4904 %p %s +4908 %p +49B4 %p +49B8 %p +49BC %p; picked creature %d, own %p", o, o ? class_name(o) : "-",
                      *reinterpret_cast<void* const*>(h + 0x4908), *reinterpret_cast<void* const*>(h + 0x49B4), *reinterpret_cast<void* const*>(h + 0x49B8),
                      *reinterpret_cast<void* const*>(h + 0x49BC), g_picked_creature, own_creature());
                using MyIfFn = char*(__thiscall*)(void*);
                void* game = game::game_instance();
                char* gi = game ? reinterpret_cast<MyIfFn>(0x555850)(game) : nullptr;
                if (gi && !IsBadReadPtr(gi + 0x39C, 0x14)) BWLOG("hands: interface state +3AC %d (19: with the creature)", *reinterpret_cast<int*>(gi + 0x3AC));
                // (Stroking starts only with the creature's +0x24 bit 0x10 set and its +0x3CC empty: fn_0046CF80.)
                const char* cr = static_cast<const char*>(own_creature());
                if (cr && !IsBadReadPtr(cr, 0x3D0)) {
                    void* busy = *reinterpret_cast<void* const*>(cr + 0x3CC);
                    BWLOG("hands: the creature's +24 %08X (bit 10: %d), +3CC %p %s", *reinterpret_cast<const uint32_t*>(cr + 0x24),
                          (*reinterpret_cast<const uint32_t*>(cr + 0x24) & 0x10) != 0, busy, busy && !IsBadReadPtr(busy, 4) ? class_name(busy) : "");
                }
            }
        }
        else if (what == "pet_hook") {  // debug: hands pet_hook 0|1 (the game's own Update, untouched)
            int on = 1;
            in >> on;
            BWLOG("hands: HandStateCreature::Update hook %s: %d", on ? "on" : "off",
                  on ? MH_EnableHook(reinterpret_cast<void*>(0x5B17B0)) : MH_DisableHook(reinterpret_cast<void*>(0x5B17B0)));
        }
        else if (what == "tap") in >> g_set.tap;
        else if (what == "option") {  // hands option <bwvr.ini key> <value as saved>: a VR option, as the menu sets it
            std::string key;
            float v = 0;
            in >> key >> v;
            for (const Tunable& t : player_options()) {
                if (!t.key || std::wstring(key.begin(), key.end()) != t.key) continue;
                if (t.action) t.action();
                if (t.flag) *t.flag = v != 0;
                else if (t.count) *t.count = std::clamp(int(v), int(t.lo), int(t.hi));
                else if (t.value) *t.value = v / t.ini_scale;
                BWLOG("hands: VR option: %ls", tunable_text(t).c_str());
            }
        }
        else if (what == "ground_marker") in >> g_set.ground_marker;
        else if (what == "two_hand") in >> g_set.two_hand;
        else if (what == "two_hand_scale") {
            in >> g_set.two_hand_scale;
            if (!g_set.two_hand_scale) g_rig.scale = g_base_scale;
        }
        else if (what == "hand_size") in >> g_set.hand_size;
        else if (what == "scale") in >> g_rig.scale;
        else if (what == "touch_margin") in >> g_set.touch_margin_m;
        else if (what == "hand_throw_gain") in >> g_set.hand_throw_gain;
        else if (what == "throw_trace") in >> g_flight_trace.on;
        else if (what == "throw_flight") in >> g_set.throw_flight;
        else if (what == "bp_twist") in >> g_set.bp_twist_gain;
        else if (what == "totem_range") in >> g_set.totem_range_m;
        else if (what == "gesture_rate") in >> g_gesture_rate_hz;
        else if (what == "spell_types") {  // debug: the miracles' types and names (GSpellSeedInfo::Infos, 0x190 each, the name at +0x18)
            std::string line;
            for (int t = 0; t < 30; ++t) {
                const char* n = reinterpret_cast<const char*>(0xD9D678 + t * 0x190 + 0x18);
                char b[48];
                snprintf(b, sizeof(b), " %d %.20s", t, IsBadReadPtr(n, 20) ? "?" : n);
                line += b;
            }
            BWLOG("hands: spell types:%s", line.c_str());
        }
        else if (what == "spawn_bubble") {
            // debug: hands spawn_bubble <type> [x z]: a miracle bubble (OneOffSpellSeed::Create, 0x72A2F0:
            // static in effect, cdecl (coords, type 0-29, power-up, 1.0); the reward and the spell
            // dispenser make theirs so) on the land at x z, or below the palm. For tests without a
            // bubble in the save.
            int type = -1;
            float x = g_near.palm.x, z = g_near.palm.z;
            in >> type;
            if (in >> x) in >> z;
            if (type < 0 || type >= 30) {
                BWLOG("hands: spawn_bubble: type 0-29 (hands spell_types names them)");
            } else {
                const MapCoords c{int32_t(x * 65536.0f / 10.0f), int32_t(z * 65536.0f / 10.0f), 0.0f};
                using CreateFn = void*(__cdecl*)(const MapCoords*, int, int, float);
                void* seed = reinterpret_cast<CreateFn>(0x72A2F0)(&c, type, 0, 1.0f);
                BWLOG("hands: spawn_bubble: type %d (%.20s) at (%.1f, %.1f): %p %s", type,
                      reinterpret_cast<const char*>(0xD9D678 + type * 0x190 + 0x18), x, z, seed, seed ? class_name(seed) : "-");
            }
        }
        else if (what == "seed") {  // debug: what the magic hand holds (seed class and type, the spell it makes)
            void* in_hand = object_in_hand();
            using SeedFn = void*(__thiscall*)(void*);
            const auto seed_fn = reinterpret_cast<SeedFn>(game_method(in_hand, 0x4C4));
            void* seed = seed_fn ? seed_fn(in_hand) : nullptr;
            int type = -1;
            void* info = nullptr;
            if (seed) info = reinterpret_cast<void*(__thiscall*)(void*)>(0x7290E0)(seed), type = reinterpret_cast<int(__cdecl*)(void*)>(0x72B200)(info);
            const auto* gi = static_cast<const uint8_t*>(game_interface());
            const int32_t* mc = gi ? reinterpret_cast<const int32_t*>(gi + 0x3F0) : nullptr;
            BWLOG("hands: seed: in hand %s, seed %s, type %d, info %p, hand state %u, interface flags %08X/%02X, lmb %d rmb %d, +0x44 %d +0x48 %d, picked %p (%s), land (%.1f, %.1f)",
                  in_hand ? class_name(in_hand) : "-", seed ? class_name(seed) : "-", type, info, game_hand_state(), gi ? *reinterpret_cast<const unsigned*>(gi + 0x38) : 0u, gi ? unsigned(gi[0x40]) : 0u, int(g_lmb), int(g_rmb),
                  gi ? *reinterpret_cast<const int*>(gi + 0x44) : -1, gi ? *reinterpret_cast<const int*>(gi + 0x48) : -1,
                  gi ? *reinterpret_cast<void* const*>(gi + 0x400) : nullptr, gi && *reinterpret_cast<void* const*>(gi + 0x400) ? class_name(*reinterpret_cast<void* const*>(gi + 0x400)) : "-",
                  mc ? mc[0] * 10.0f / 65536.0f : 0.0f, mc ? mc[1] * 10.0f / 65536.0f : 0.0f);
        }
        else if (what == "totem_sign") in >> g_totem_sign;
        else if (what == "peek") {  // debug: hands peek <hex address> [dwords]: the memory there, as hex and as floats
            std::string a;
            int n = 4;
            in >> a >> n;
            const auto* p = reinterpret_cast<const uint32_t*>(std::strtoul(a.c_str(), nullptr, 16));
            n = std::clamp(n, 1, 32);
            if (IsBadReadPtr(p, n * 4)) {
                BWLOG("peek: %p unreadable", static_cast<const void*>(p));
            } else {
                std::string line;
                for (int i = 0; i < n; ++i) {
                    char b[48];
                    snprintf(b, sizeof(b), " %08X(%g)", p[i], *reinterpret_cast<const float*>(p + i));
                    line += b;
                }
                BWLOG("peek: %p:%s", static_cast<const void*>(p), line.c_str());
            }
        }
        else if (what == "bp_sign") in >> g_bp_sign;
        else if (what == "bp_log") in >> g_dbg_bp_log;
        else if (what == "gesture_log") {
            float sec = 10;
            in >> sec;
            g_dbg_gesture_until = now_s() + sec;
            BWLOG("gesture: logging for %.0f s (samples now %d)", sec, gesture_samples());
        }
        else if (what == "build_at_blueprint") in >> g_set.build_at_blueprint;
        else if (what == "point_to_draw") in >> g_set.point_to_draw;
        else if (what == "combine_by_touch") in >> g_set.combine_by_touch;
        else if (what == "join_over_cm") { float cm = 0; in >> cm; g_set.join_over_m = cm / 100.0f; }
        else if (what == "shield_autocast") in >> g_set.shield_autocast;
        else if (what == "zap_aim") in >> g_set.zap_aim;
        else if (what == "zap_atoms") in >> g_zap_atom_log;
        else if (what == "zap_from_finger") in >> g_zap_from_finger;
        else if (what == "circle_type") in >> g_dbg_circle_type;
        else if (what == "throw_tell_game") in >> g_set.throw_tell_game;
        else if (what == "still_game_hand") in >> g_dbg_still_game_hand;
        else if (what == "give_finish") in >> g_set.give_finish_s;
        else if (what == "give_log") in >> g_dbg_give_log;
        else if (what == "leash_land_tap") in >> g_set.leash_land_tap;
        else if (what == "pop_with_trigger") in >> g_set.pop_with_trigger;
        else if (what == "fist_on_grip") in >> g_set.fist_on_grip;
        else if (what == "tip_under_hand") in >> g_set.tip_under_hand;
        else if (what == "highlight") in >> g_set.highlight;
        else if (what == "cast_log") in >> g_cast_log;
        else if (what == "fight_log") in >> g_fight_log;
        else if (what == "fight_mode") in >> g_set.fight_mode;
        else if (what == "fight_pin") in >> g_set.fight_pin;
        else if (what == "spell_at_palm") in >> g_spell_at_palm;
        else if (what == "watch_spell") in >> g_watch_spell_start;
        else if (what == "atoms") in >> g_dbg_atoms;
        else if (what == "scan") in >> g_dbg_scan;
        else if (what == "scan_watch") in >> g_dbg_scan_watch;
        else if (what == "atom_track") in >> g_dbg_atom_track;
        else if (what == "sparkle_log") in >> g_dbg_sparkle;
        else if (what == "liquid_log") in >> g_dbg_liquid;
        else if (what == "sprite_log") in >> g_dbg_sprite;
        else if (what == "atoms_at_palm") in >> g_atoms_at_palm;
        else if (what == "offsets_at_palm") in >> g_offsets_at_palm;
        else if (what == "drawat_log") in >> g_dbg_drawat;
        else if (what == "watch_steer") in >> g_dbg_watch_steer;
        else if (what == "side_spin") { int v = 0; in >> v; g_no_side_spin = v == 0; }
        else if (what == "psys_at_palm") in >> g_psys_at_palm;
        else if (what == "trigger_carries") in >> g_set.trigger_carries;
        else if (what == "launch_catch_up") in >> g_set.launch_catch_up;
        else if (what == "scoop_end") in >> g_set.scoop_end;
        else if (what == "pile_log") in >> g_dbg_pile_log;
        else if (what == "vinfl") in >> g_dbg_vinfl;
        else if (what == "vinfl_mod") in >> g_vinfl_mod_point;
        else if (what == "influence") {  // debug: hands influence x0 z0 x1 z1 step: the game's inside/outside test on a grid
            float x0 = 0, z0 = 0, x1 = 0, z1 = 0, step = 20;
            in >> x0 >> z0 >> x1 >> z1 >> step;
            void* player = influence_player();
            if (!player || step <= 0) return false;
            using InflFn = int(__cdecl*)(const int32_t*, void*);
            for (float z = z0; z <= z1; z += step) {
                std::string row;
                for (float x = x0; x <= x1; x += step) {
                    const int32_t mc[3] = {int32_t(x * 6553.6f), int32_t(z * 6553.6f), 0};
                    row += reinterpret_cast<InflFn>(0x5CD490)(mc, player) ? 'A' : reinterpret_cast<InflFn>(0x5CD460)(mc, player) ? '#' : '.';
                }
                BWLOG("hands: influence z %.0f x %.0f..: %s", z, x0, row.c_str());
            }
        }
        else if (what == "touchlog") in >> g_touchlog;
        else if (what == "picklog") in >> g_picklog;
        else if (what == "pick_changes") in >> g_pick_change_log;
        else if (what == "offer_log") in >> g_offer_log;  // debug: what reports to the picker, each object once
        else if (what == "lclick") {
            int ms = 120;
            in >> ms;
            g_dbg_lmb_until = now_s() + ms / 1000.0;
        }
        else if (what == "grip_never_directs") in >> g_set.grip_never_directs;
        else if (what == "throw_min_speed") in >> g_set.throw_min_speed_m_s;
        else if (what == "finger_pitch") in >> g_set.finger_pitch_deg;
        else if (what == "near_drag") in >> g_set.near_drag;
        else if (what == "laser") in >> g_laser;
        else if (what == "move_speed" || what == "follow_ground" || what == "glow_models" || what == "wrist_press" || what == "wrist_log") {  // (follow_ground 0|1, glow_models 0|1)
            // (Plain ifs: the chain of else-ifs is at the compiler's nesting limit.)
            if (what == "move_speed") in >> g_set.move_speed;
            if (what == "glow_models") in >> g_set.glow_models >> g_glow_any;
            if (what == "follow_ground") in >> g_set.follow_ground;
            int id = -1;  // (debug: hands wrist_press <0 pause, 1 slower, 2 normal, 3 faster, 4 info>)
            if (what == "wrist_press" && in >> id) wrist_press(id);
            if (what == "wrist_log") in >> g_wrist_log;
        }
        else if (what == "near_in") in >> g_set.near_in;
        else if (what == "near_out") in >> g_set.near_out;
        else if (what == "wrist_back") in >> g_set.wrist_back_m;
        else if (what == "dominant") {
            std::string s;
            in >> s;
            g_set.dominant = s == "l" ? vr::kLeft : vr::kRight;
            g_set.dominant_left = g_set.dominant == vr::kLeft;
        } else return false;
        return true;
    }
    if (k == "rig") {
        // rig reset | rig info
        std::string what;
        in >> what;
        if (what == "reset") rig_reset();
        else if (what == "set") {
            in >> g_rig.pos.x >> g_rig.pos.y >> g_rig.pos.z >> g_rig.yaw_deg;
            g_rig.init = true;
            g_drag.on = false;
        }
        BWLOG("rig: grab (%.3f, %.3f, %.3f) pointer hit (%.3f, %.3f, %.3f)", g_drag.grab.x, g_drag.grab.y, g_drag.grab.z,
              g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z);
        BWLOG("rig: ground under the rig %.3f", ground_height(g_rig.pos.x, g_rig.pos.z));
        BWLOG("rig: pos (%.3f, %.3f, %.3f) yaw %.1f scale %.2f drag %d hand state %u", g_rig.pos.x, g_rig.pos.y, g_rig.pos.z,
              g_rig.yaw_deg, g_rig.scale, g_drag.on, game_hand_state());
        return true;
    }
    if (k == "ground") {  // ground x z: the altitude lookup against a steep ray cast
        float x = 0, z = 0;
        in >> x >> z;
        const Vec3 d = vr::normalize(Vec3{0.02f, -1.0f, 0.02f});
        Vec3 hit{};
        const bool ok = cast_island(Vec3{x, 0, z} - d * 200.0f, d, 400.0f, hit);
        BWLOG("ground: (%.2f, %.2f) altitude %.3f, ray cast %d (%.2f, %.3f, %.2f)", x, z, ground_height(x, z), ok, hit.x, hit.y, hit.z);
        return true;
    }
    if (k == "pointerinfo") {
        BWLOG("pointer: picker reports %u, palm inside %u, taken %u (since the last pointerinfo)", g_dbg_calls, g_dbg_inside, g_dbg_taken);
        g_dbg_calls = g_dbg_inside = g_dbg_taken = 0;
        BWLOG("pointer: touch %d object %p centre (%.2f, %.2f, %.2f) radius %.2f dist %.2f palm (%.2f, %.2f, %.2f) %s",
              now_s() - g_touch.t < 0.1, g_touch.object, g_touch.centre.x, g_touch.centre.y, g_touch.centre.z, g_touch.radius,
              g_touch.dist, g_near.palm.x, g_near.palm.y, g_near.palm.z, g_touch.object ? class_name(g_touch.object) : "-");
        BWLOG("pointer: near %d blend %.2f dragging %d wrist (%.2f, %.2f, %.2f) fingers (%.3f, %.3f, %.3f) palm (%.3f, %.3f, %.3f) reach %d", g_near.on,
              g_near.blend, g_near.dragging, g_near.pos.x, g_near.pos.y, g_near.pos.z, g_near.z.x, g_near.z.y, g_near.z.z, g_near.y.x, g_near.y.y,
              g_near.y.z, g_near.in_reach);
        BWLOG("pointer: valid %d origin (%.3f, %.3f, %.3f) dir (%.4f, %.4f, %.4f) hit (%.3f, %.3f, %.3f) land %d cursor %d (%ld, %ld) lmb %d rmb %d picked %p %s",
              g_ptr.valid, g_ptr.origin.x, g_ptr.origin.y, g_ptr.origin.z, g_ptr.dir.x, g_ptr.dir.y, g_ptr.dir.z,
              g_ptr.hit.x, g_ptr.hit.y, g_ptr.hit.z, g_ptr.hit_land, g_ptr.cursor_ok, g_ptr.cursor_x, g_ptr.cursor_y,
              g_lmb, g_rmb, g_picked, g_picked ? class_name(g_picked) : "-");
        return true;
    }
    return false;
}

std::string stats_line() {
    char buf[256];
    snprintf(buf, sizeof(buf), "hands: %u updates, %u land hits, %u clicks, near %d (beam %.2f m, above ground %.2f m, busy %d, B %d, state %u)",
             g_updates, g_land_hits, g_clicks, g_near.on, g_near_dbg.beam_m, g_near_dbg.above_m, g_near_dbg.busy, g_near_dbg.b,
             game_hand_state());
    g_updates = g_land_hits = g_clicks = 0;
    return buf;
}

bool tuning(TuningView& out) {
    if (!g_tuning.open) return false;
    NearHand other;
    if (!second_hand(other)) return false;
    out.lines.clear();
    out.lines.push_back(L"TUNING   stick: up/down picks, left/right changes   click: close and save");
    const auto& list = tunables();
    for (size_t i = 0; i < list.size(); ++i)
        out.lines.push_back((int(i) == g_tuning.selected ? L"> " : L"   ") + tunable_text(list[i]));
    out.selected = g_tuning.selected + 1;
    out.unit = g_rig.scale;
    out.anchor = other.pos + vr::Vec3{0, 0.08f * g_rig.scale, 0};
    return true;
}

void note_held(void* obj3d, const float* matrix) {
    g_held_seen.obj3d = static_cast<const char*>(obj3d);
    g_held_seen.pos = {matrix[9], matrix[10], matrix[11]};
    memcpy(g_held_seen.rot, matrix, sizeof(g_held_seen.rot));
}

bool held_unnamed() { return g_prehold.object && g_prehold.unnamed; }
float glow_scale(const void* obj3d, const void* object) {
    // (Its 3D object, or the thing itself as the renderer is handed it: a building's +0x40 isn't its model.)
    if (!obj3d || now_s() - g_glow_at > 0.1 || (obj3d != g_glow_obj3d && (!object || object != g_glow_object))) return 0;
    static const void* told = nullptr;
    if (object == g_glow_object && obj3d != g_glow_obj3d && told != object) told = object, BWLOG("hands: lit up by the thing: its model %p", obj3d);
    // Brighter by the highlight's amount (35%: x1.35), breathing a little (0.8 Hz).
    return 1.0f + g_glow_amount * (0.8f + 0.2f * std::sin(float(now_s()) * 5.0f));
}
void glow_seen() { g_glow_hit_at = now_s(); }
bool glow_cool() { return g_glow_cool; }
void* held_object_3d_now() { return held_object_3d_hand(); }
bool prehold_draw(const void* obj3d, float* matrix) {
    const PreHold& h = g_prehold;
    if (!h.object || !h.draw_only || !h.has_draw || obj3d != h.obj3d) return false;
    matrix[9] = h.draw_at.x, matrix[10] = h.draw_at.y, matrix[11] = h.draw_at.z;
    return true;
}

bool in_flight(const void* obj3d, float* matrix) {
    if (!g_flight.obj3d || g_flight.obj3d != obj3d) return false;
    const Vec3 p = flight_pos(g_flight, now_s());
    memcpy(matrix, g_flight.rot, sizeof(g_flight.rot));
    matrix[9] = p.x, matrix[10] = p.y, matrix[11] = p.z;
    return true;
}
float ground_at(float x, float z) { return ground_height(x, z); }
bool leash_in_hand_now() { return leash_in_hand(); }

bool gravity_flight(const void* obj3d, Vec3& from, float& t) {
    if (g_gg.st != GravityGlove::kFlying || obj3d != g_gg.fly_obj3d) return false;
    t = float((now_s() - g_gg.fly_at) / std::max(0.05f, g_set.gravity_flight_s));
    from = g_gg.fly_from;
    return t < 1.0f;
}
float hand_opacity() { return 1.0f - g_outside * (1.0f - std::clamp(g_set.outside_alpha, 0.0f, 1.0f)); }
const void* recent_release() {
    if (g_flight.obj3d) return g_flight.obj3d;
    return g_thrown.obj3d && now_s() - g_thrown.t < 1.5 ? g_thrown.obj3d : nullptr;
}

}  // namespace bw::hand
