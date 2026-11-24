// The game's 2D interface in VR.
//
// All of the game's text goes through GatheringText::DrawTextRaw(text, count, x, y, z, size,
// colour, ...): a screen position in pixels, a view depth in world units (the flat game passes a
// little more than the near plane) and a glyph size in pixels. Drawing the same text in each eye
// pass at the projection of one world point, with that point's depth, puts it in the world.
#include "ui/ui.h"

#include <windows.h>
#include <intrin.h>

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/log.h"
#include "core/paths.h"
#include "game/engine.h"
#include "render/d3d7_trace.h"
#include "render/eye_target.h"

namespace bw::ui {

// When the game last asked for its "Pick Up" word (QueryPerformanceCounter seconds): the thing under
// its cursor can be picked up.
double g_pick_up_asked = -10;
double seconds_since_pick_up_word() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) / double(f.QuadPart) - g_pick_up_asked;
}
namespace {

using vr::Vec3;

constexpr uintptr_t kDrawTextRaw = 0x832C60;  // GatheringText::DrawTextRaw
using DrawTextRawFn = void(__thiscall*)(void* font, const wchar_t* text, int count, float x, float y, float z, float size,
                                        void* colour, int flags, void* colour2, float a, float b);
DrawTextRawFn o_draw_text_raw = nullptr;

struct Settings {
    bool hand_hints = true;        // [ui] hand_hints: the hand's action hints, as words by the hand
    float hint_height_m = 0.022f;  // letter size at the hand, metres at the player's scale
    float hint_above_m = 0.11f;    // how far above the hand the first line sits
    float hint_min_px = 20.0f, hint_max_px = 56.0f;
    // [ui] spinner_at_hand: the game's counters (ValueSpinner: wood or food in the hand, a store's,
    // a scaffold's wood) drawn at the hand go above it with the hand's words (at the game's hand,
    // inside the VR hand, they were hidden); spinner_scale: the others drawn this much larger.
    bool spinner_at_hand = true;
    bool held_count = true;  // [ui] held_count: how much food or wood the hand carries, by the hand
    float spinner_scale = 2.0f;
    float label_before_hand_m = 0.15f;  // [ui] label_before_hand_cm: labels by the hand at least this much nearer the eye than the palm
    float world_text_scale = 2.0f;  // [ui] world_text_scale_percent: other text the game draws in the world this much larger
    float help_height_m = 0.06f, help_above_m = 0.0f, help_dist_m = 1.5f;  // the signposts' help lines: size, offset up, distance
    // A word appears only once the game has asked for it steadily for hint_delay seconds, then
    // fades in over hint_fade: it stays out of the way while things are moving.
    float hint_delay = 1.0f, hint_fade = 0.3f;
    // [ui] hint_controls: add which control does it ("Activate - trigger"). Things that are
    // tapped show their word sooner (tap_delay): it is the only sign of how to use them.
    bool hint_controls = true;
    float tap_delay = 0.25f;
    // Words never shown: whether letting go throws or drops is decided by the arm at that
    // moment, so the game's guess beforehand only misleads. ([ui] hidden_hints, comma separated.)
    std::vector<std::wstring> hidden = {L"Throw", L"Drop"};
    bool show_hidden = false;  // debug/tests: show those too
    float hint_depth = 0;  // debug: >0 = draw at this fraction of the real depth
    bool screen = true;            // [ui] screen: the game's 2D on a virtual screen
    float screen_dist_m = 2.0f;    // how far away it floats
    float screen_width_m = 2.6f;   // how wide the whole 2D layout is there
    float screen_drop_m = 0.1f;    // its centre this far below eye height
    float screen_follow_deg = 40;  // it swings round once the head has turned this far from it
    // [ui] screen_follow_cm: the panel stays where it was set out, still in the room as the head
    // moves and leans (it moved with every movement of the head: the pause menu wiggled; player,
    // 2026-10-29), and follows once the head is this far from that place (0: with the head, as before).
    float screen_follow_m = 0.30f;
    // [ui] menu_width_cm: the menus' panel this wide (at screen_dist_m): about one menu pixel to one
    // eye pixel. At screen_width_m their text was shrunk to half and shimmered as the head moved
    // (player, 2026-10-30: fuzzy and jittery).
    float menu_width_m = 5.0f;
    // [ui] caption_scale_percent: the spirits' captions (HelpText) and their band drawn up to this
    // much larger on the panel, about the band's middle. At screen_width_m the game's 36 px words
    // came out about 16 eye pixels tall, its glyphs shrunk to 0.43: blurred and hard to read
    // (player, 2026-10-30). Smaller for a long line, so it stays within caption_view_deg.
    float caption_scale = 2.0f;
    float status_scale = 1.25f;  // [ui] status_scale_percent: help lines with no spirit out (the creature's state)
    float stats_scale = 2.0f;           // [ui] stats_scale_percent: the creature's behaviour panel this much larger
    float stats_toward_middle = 0.5f;   // [ui] stats_toward_middle_percent: ... and moved this part of the way to the middle
    float caption_view_deg = 70.0f;
    float cinema_dist_m = 3.0f, cinema_width_m = 4.2f;  // the screen as a cinema screen (cutscenes)
    bool info_hints = true;        // [ui] info_hints: the game's line about the thing under the hand, by the hand too
    bool gesture_panel = true;     // [ui] gesture_panel: the game's gesture icons above the left hand, palm up
    float gesture_panel_m = 0.30f; // ... this wide (the icons are a row)
    bool gesture_match = true;     // [ui] gesture_match: the shapes moved to where the engine's own projection shows them on the panel
    bool sprites_upright = true;   // [ui] sprites_upright: the engine's sprites turned to stand upright in the world (sprite_upright_begin)
    bool gesture_remap = true;     // [ui] gesture_remap: the shapes' corners laid on the panel as drawn (gesture_sprite_begin); 0: placed in the world
};
Settings g_set;

int g_eye = -1;
hand::EyeView g_view;

// ---- The virtual screen ----
// The game lays its 2D out in window pixels and draws it at the end of every eye pass at those
// pixels: the same place in both eyes (so at infinity), in a corner of the much larger eye image.
// Here every 2D vertex is instead put on a panel floating in front of the player and projected
// with the eye's camera. The panel is kept in tracking space: it stays put when the head turns a
// little, comes along when the player moves, and swings round when the head has turned far.
struct Screen {
    bool valid = false;        // begin_frame has run
    int w = 1920, h = 1080;    // the 2D layout, pixels
    float yaw_deg = 0;         // panel direction from the head, tracking space
    bool placed = false, following = false;
    Vec3 anchor;               // the head's place it's set out from (tracking space): still in the room
    bool moving = false;       // ... following the head's place
    double last_drawn = -10;   // when 2D was last drawn
    double last_frame = 0;
    Vec3 centre, right, up, normal;  // world; right/up are unit vectors
    float unit = 1;                  // world units per layout pixel
    // Where panels and buttons (not just text) were drawn, layout pixels: this frame so far, and
    // the last whole frame. The laser acts as the mouse inside it.
    float box[4] = {1e9f, 1e9f, -1e9f, -1e9f}, last_box[4] = {1e9f, 1e9f, -1e9f, -1e9f};
    float text[4] = {1e9f, 1e9f, -1e9f, -1e9f}, last_text[4] = {1e9f, 1e9f, -1e9f, -1e9f};  // in-game text, as laid out
    float centre_px[2][2] = {};  // where the layout's centre lands in each eye's image (for the tests)
};
Screen g_screen;
bool g_in_text = false;      // inside the game's DrawTextRaw with a "front" depth: its glyph draws are 2D
// The captions (HelpText's words, and the band behind them): enlarged about the band's middle.
constexpr uintptr_t kCaptionReturn = 0x5CBE84;  // (HelpText's call of DrawTextRaw, fn_005CB590)
bool g_in_caption = false;
bool g_drawing_prompt = false;  // the continue prompt, drawn with the caption (left out of its box)
unsigned g_frame_serial = 0;
float g_caption_text[4] = {1e9f, 1e9f, -1e9f, -1e9f}, g_caption_last[4] = {1e9f, 1e9f, -1e9f, -1e9f};  // its words, layout pixels
// The creature's behaviour panel (its desires' bars while stroking it): small, blurry and off to the
// side on the screen panel (playtest 2026-10-31). Drawn by DrawCreatureStats (0x517080, cdecl(float,
// float, float, float, int), from fn_005178D0; the symbol map's 0x517265 is inside it): while it
// draws, its pieces are drawn [ui] stats_scale_percent larger about the panel's middle (the last
// frame's), moved part of the way to the screen's middle (stats_toward_middle_percent).
constexpr uintptr_t kDrawCreatureStats = 0x517080;
using DrawStatsFn = void(__cdecl*)(float, float, float, float, int);
DrawStatsFn o_draw_stats = nullptr;
bool g_in_stats = false;
float g_stats_box[4] = {1e9f, 1e9f, -1e9f, -1e9f}, g_stats_last[4] = {1e9f, 1e9f, -1e9f, -1e9f};  // layout pixels
void __cdecl draw_stats_detour(float a, float b, float c, float d, int e) {
    g_in_stats = true;
    o_draw_stats(a, b, c, d, e);
    g_in_stats = false;
}
float g_caption_mid_y = -1;  // the band's middle (layout pixels; < 0 none seen yet)
// Two layouts. The front-end menus (SetupThing) lay themselves out in window pixels. The in-game
// 2D (speech text, the letterbox bars, fades) uses the engine's current resolution, which in an
// eye pass is the eye image's: much taller than the window. Menu mode = a menu drew text lately.
double g_menu_text_at = -10;
bool g_cinema = false;
bool g_passthrough = false;  // our own text (the hand's hints): already placed, leave it
std::vector<uint8_t> g_scratch;

double now_s() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}
// The words drawn so far in this eye pass. The game asks for the same hint more than once in a
// frame, and (while carrying) only the later request's text reaches the screen: every request is
// drawn, a repeated word on the line it already has.
std::vector<std::wstring> g_hints_drawn;
int g_textlog = 0;    // debug: log this many text draws

// A caption left up after a scene. In the flat game a speech caption can stay until clicked, with a
// prompt the headset doesn't show, so it looked like a caption floating there for no reason
// (player, 2026-10-24). When a speech band has been up outside a scene with the same words for a
// while, "trigger - continue" is shown under it. The words are told apart by a hash of this
// frame's 2D text (left eye).
uint64_t g_caption_hash = 0, g_caption_hash_last = 0;
bool g_band_seen = false, g_band_seen_last = false;
double g_caption_since = -10;
float g_continue_after_s = 3.0f;  // [ui] continue_prompt_s (0: never)
bool g_continue_up = false;  // the prompt is drawn: the trigger clicks (the game's Continue), whatever the hand is on
double g_continue_seen_at = -10;  // (drawn in the eye passes: only while they run)

// How the game draws a hint's word, learned from one flat draw: the font, and each of its passes
// (it draws the word three times, a pixel apart, for an outline).
struct Pass {
    float dx, dy;
    uint32_t colour, colour2;
    bool has_colour2;
    int flags;
    float a, b;
};
struct HintStyle {
    void* font = nullptr;
    float size = 28;
    std::vector<Pass> passes;
};
HintStyle g_style;
HintStyle g_fallback;  // the first in-game text's font, one white pass
bool g_learning = false;
float g_learn_x = 0, g_learn_y = 0;

// The tutorial's "?" signposts: tapped, a helper sprite by the sign says something in a speech
// bubble ("Did you know...?"), drawn by fn_005767D0 with GatheringText::DrawBubble (called at
// 0x5768A7) and its lines with GatheringText::DrawTextA (at 0x576D49). Their screen positions are
// worked out once per frame from the game's camera, and the bubble and its text are laid out
// differently in an eye pass (the bubble scaled to the tall eye image, the text not), so in the
// headset the bubble hung at the same place in both eyes (no depth) at the top edge, and the text
// went elsewhere. In the eye passes the bubble isn't drawn and its lines are drawn by the mod
// instead, on the virtual screen with the hints' outlined style. (DrawTextA is called three
// times a line: from 0x576D49 to measure it, then its shadow from 0x576DEF and the line itself
// from 0x576E3E.)
constexpr uintptr_t kGtDrawBubble = 0x8334C0, kGtDrawTextA = 0x8315B0;
constexpr uintptr_t kHelpBubbleReturn = 0x5768AC, kHelpShadowReturn = 0x576DF4, kHelpTextReturn = 0x576E43;
// (DrawBubble is static, whatever its symbol says: plain ret, and the caller takes 13 arguments
// off the stack. They are passed through untouched.)
using GtDrawBubbleFn = void(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                      uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using GtDrawTextAFn = float(__thiscall*)(void*, wchar_t*, float, float, float, float, float, float, float, float, float, void*, int, int,
                                         int);
GtDrawBubbleFn o_gt_draw_bubble = nullptr;
GtDrawTextAFn o_gt_draw_text_a = nullptr;
bool g_in_help_text = false;  // inside the help bubble's DrawTextA: its glyphs are recorded, not drawn
struct HelpLine {
    std::wstring text;
    float y;
};
std::vector<HelpLine> g_help_lines, g_help_shown;  // this frame's so far / the last whole frame's
double g_help_at = -10;                            // when the help bubble was last drawn
double g_help_started = -10;                       // ... when it was first drawn after a gap
bool g_help_panel = true;                          // [ui] help_panel
int g_help_log = 0;                                // debug: log the help bubble's arguments (ui help_log N)

void __cdecl gt_draw_bubble_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h,
                                   uintptr_t i, uintptr_t j, uintptr_t k, uintptr_t l, uintptr_t m) {
    if (g_eye >= 0 && g_help_panel && reinterpret_cast<uintptr_t>(_ReturnAddress()) == kHelpBubbleReturn) {
        if (g_help_log > 0) {
            --g_help_log;
            const uintptr_t v[13] = {a, b, c, d, e, f, g, h, i, j, k, l, m};
            std::string out;
            for (uintptr_t x : v) {
                float fv;
                memcpy(&fv, &x, 4);
                char buf[48];
                snprintf(buf, sizeof(buf), " %08X(%g)", unsigned(x), fv);
                out += buf;
            }
            BWLOG("ui: help bubble args:%s", out.c_str());
        }
        if (now_s() - g_help_at > 0.3) {
            g_help_started = now_s();
            BWLOG("ui: the game starts a help bubble");  // (a sign that showed nothing: did the game draw one?)
        }
        g_help_at = now_s();
        return;
    }
    o_gt_draw_bubble(a, b, c, d, e, f, g, h, i, j, k, l, m);
}
float __fastcall gt_draw_text_a_detour(void* gt, void*, wchar_t* text, float a, float b, float c, float d, float e, float f, float g,
                                       float h, float i, void* colour, int j, int k, int l) {
    const auto from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool help = g_eye >= 0 && g_help_panel && (from == kHelpTextReturn || from == kHelpShadowReturn);
    g_in_help_text = help;
    const float r = o_gt_draw_text_a(gt, text, a, b, c, d, e, f, g, h, i, colour, j, k, l);
    g_in_help_text = false;
    return r;
}

void draw_hint(const wchar_t* text, float alpha);  // (below)
// ValueSpinner::Draw's two calls of DrawTextRaw: its dark shadow, then the number. (Only the shadow
// was known: it alone was enlarged, a big black number beside a small yellow one; player, 2026-10-31.)
constexpr uintptr_t kSpinnerShadowReturn = 0x833C65, kSpinnerReturn = 0x833CA5;
void __fastcall draw_text_raw_detour(void* font, void*, const wchar_t* text, int count, float x, float y, float z, float size,
                                     void* colour, int flags, void* colour2, float a, float b) {
    // The game's counters (ValueSpinner::Draw), drawn at a depth in the world (z: world units
    // ahead of the eye): at the hand, with the hand's words; elsewhere, larger, centred on the same place.
    if (g_eye >= 0 && !g_passthrough && text && count > 0 && (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kSpinnerReturn || reinterpret_cast<uintptr_t>(_ReturnAddress()) == kSpinnerShadowReturn) &&
        z > game::info_transform().near_clip * 2.0f) {
        const Vec3 P = g_view.pos + g_view.rot.right * ((x - g_view.cx) * z / g_view.f) + g_view.rot.up * ((g_view.cy - y) * z / g_view.f) +
                       g_view.rot.fwd * z;
        hand::NearHand nh;
        const float scale = hand::rig_scale();
        if (g_set.spinner_at_hand && hand::near_hand(nh) && nh.blend > 0.5f && vr::length(P - (nh.pos + nh.z * (0.08f * scale))) < 1.0f * scale) {
            std::wstring t(text, size_t(count));
            t.erase(0, t.find_first_not_of(L' '));
            while (!t.empty() && t.back() == L' ') t.pop_back();
            static int logged = 0;
            if (logged < 5) ++logged, BWLOG("ui: a counter at the hand ('%ls') shown above it", t.c_str());
            if (!t.empty()) draw_hint(t.c_str(), 1.0f);
            return;
        }
        if (g_set.spinner_scale > 1.0f) {
            const float k = g_set.spinner_scale, width = 0.5f * size * float(count);
            x -= width * (k - 1.0f) * 0.5f, y -= size * (k - 1.0f) * 0.5f, size *= k;
        }
    } else if (g_eye >= 0 && !g_passthrough && !g_in_help_text && text && count > 0 && z > game::info_transform().near_clip * 2.0f &&
               g_set.world_text_scale > 1.0f && now_s() - g_menu_text_at > 0.3) {
        // Other text at a depth in the world (the creature's learning percentage over its head while
        // teaching: small and blurry, playtest 2026-10-31): larger, centred on the same place.
        const float k = g_set.world_text_scale, width = 0.5f * size * float(count);
        x -= width * (k - 1.0f) * 0.5f, y -= size * (k - 1.0f) * 0.5f, size *= k;
        static std::vector<uintptr_t> told;
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (told.size() < 16 && std::find(told.begin(), told.end(), from) == told.end()) {
            told.push_back(from);
            char narrow[32] = {};
            for (int i = 0; i < 31 && i < count && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
            BWLOG("ui: world text '%s' (from %p, z %.1f) drawn %.1fx", narrow, reinterpret_cast<void*>(from), z, k);
        }
    }
    if (g_in_help_text) {  // (a help bubble's line: kept for the mod's own panel; each is drawn twice, a shadow first)
        if (text && count > 0 && g_eye == 0) {
            std::wstring line(text, size_t(count));
            while (!line.empty() && line.back() == L' ') line.pop_back();
            const bool seen = std::any_of(g_help_lines.begin(), g_help_lines.end(), [&](const HelpLine& h) { return h.text == line; });
            if (!line.empty() && !seen) g_help_lines.push_back({line, y});
        }
        return;
    }
    if (g_textlog > 0 && text) {
        --g_textlog;
        char narrow[64] = {};
        for (int i = 0; i < 63 && i < count && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
        BWLOG("ui: text eye %d '%s' at (%.1f, %.1f) z %.3f size %.2f flags %d a %.2f b %.2f font %p colour %08X from %p", g_eye, narrow, x, y,
              z, size, flags, a, b, font, colour ? *static_cast<uint32_t*>(colour) : 0u, _ReturnAddress());
    }
    if (g_learning) {
        if (g_style.passes.empty()) {
            g_style.font = font;
            g_style.size = size;
            g_learn_x = x, g_learn_y = y;
        }
        if (g_style.passes.size() < 8)
            g_style.passes.push_back({x - g_learn_x, y - g_learn_y, colour ? *static_cast<uint32_t*>(colour) : 0xFFFFFFFFu,
                                      colour2 ? *static_cast<uint32_t*>(colour2) : 0u, colour2 != nullptr, flags, a, b});
        return;  // the flat hint itself isn't wanted
    }
    if (g_eye == 0 && text && now_s() - g_menu_text_at > 0.3) {  // for the log: each in-game text once
        static std::vector<std::wstring> seen;
        const std::wstring key(text, size_t(std::min(count, 24)));
        if (seen.size() < 150 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            char narrow[48] = {};
            for (int i = 0; i < 47 && i < count && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
            BWLOG("ui: in-game text '%s' at (%.0f, %.0f) z %.2f size %.0f (near %.2f, eye image %.0fx%.0f) from %p", narrow, x, y, z, size,
                  game::info_transform().near_clip, g_view.cx * 2, g_view.cy * 2, _ReturnAddress());
        }
    }
    if (!g_fallback.font && font && text && count > 0 && !g_passthrough) {  // (a style for our own text until a hint is learned)
        // A dark outline (two offset passes), then white.
        const uint32_t c2 = colour2 ? *static_cast<uint32_t*>(colour2) : 0u;
        g_fallback = {font, size, {{1.5f, 1.5f, 0xFF000000u, c2, colour2 != nullptr, flags, a, b},
                                   {-1.5f, -1.5f, 0xFF000000u, c2, colour2 != nullptr, flags, a, b},
                                   {0, 0, 0xFFFFFFFFu, c2, colour2 != nullptr, flags, a, b}}};
    }
    // Text at a real depth (a label on something in the world) is already placed per eye.
    g_in_text = z <= game::info_transform().near_clip * 2.0f;
    g_in_caption = g_in_text && reinterpret_cast<uintptr_t>(_ReturnAddress()) == kCaptionReturn;
    // (Only the caption's own words: the hints and counters on the screen changed it, so a caption
    // left up took a minute to be seen as one; player, 2026-11-02.)
    if (g_in_caption && g_eye == 0 && text && !g_passthrough && !g_learning) {
        for (int i = 0; i < count && text[i]; ++i) g_caption_hash = (g_caption_hash ^ uint64_t(text[i])) * 1099511628211ull;
        g_caption_hash = (g_caption_hash ^ uint64_t(std::lround(y))) * 1099511628211ull;
    }
    o_draw_text_raw(font, text, count, x, y, z, size, colour, flags, colour2, a, b);
    // "trigger - continue" under a caption left up, drawn as one more of the caption's words: on the
    // screen with it, in front, level with it (as text of the mod's own at a point it turned with the
    // head's roll and lay behind the caption; player, 2026-11-02). Once an eye pass.
    if (g_in_caption && !g_passthrough && continue_prompt_up() && g_caption_last[2] > g_caption_last[0]) {
        static unsigned drawn_serial = ~0u;
        static int drawn_eye = -9;
        if (drawn_serial != g_frame_serial || drawn_eye != g_eye) {
            drawn_serial = g_frame_serial, drawn_eye = g_eye;
            static const wchar_t kPrompt[] = L"trigger - continue";
            const int n = int(wcslen(kPrompt));
            const float ps = size * 0.85f, width = 0.5f * ps * float(n);
            const float px = (g_caption_last[0] + g_caption_last[2]) * 0.5f - width * 0.5f, py = g_caption_last[3] + size * 0.7f;
            g_drawing_prompt = true;
            o_draw_text_raw(font, kPrompt, n, px, py, z, ps, colour, flags, colour2, a, b);
            g_drawing_prompt = false;
        }
    }
    g_in_text = false, g_in_caption = false;
}

// The front-end menus' text functions, SetupThing::DrawTextA and DrawTextWrap (static, __cdecl,
// returning float): they mark menu mode. Their argument lists in the symbol map don't match the
// code exactly (DrawTextA reads its text from the fifth slot, not the fourth), so the detours
// don't name the arguments: they pass on more stack slots than either takes, which __cdecl allows
// (the callee ignores the extra ones and the caller cleans up its own).
constexpr uintptr_t kSetupDrawText = 0x4119B0, kSetupDrawTextWrap = 0x411750;
using SetupTextFn = float(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
SetupTextFn o_setup_draw_text = nullptr, o_setup_draw_text_wrap = nullptr;
// Not every user of those functions is a menu: the creature's stats panel, shown while the hand
// strokes it (DrawCreatureStats, 0x517265-0x5178C0), uses them too, and the laser must not take
// over the hand for it.
void note_menu_text(void* caller) {
    const auto at = reinterpret_cast<uintptr_t>(caller);
    if (at >= 0x517265 && at < 0x5178C0) return;
    // Nor a tab button the game draws with no menu box up (SetupTabButton::Draw, 0x40F3A0-0x40F5E0):
    // one stays on the screen in the tutorial, and as "menu text" it kept the caption's continue
    // prompt off (player, 2026-11-02: "I'll leave you two alone..." could only be waited out).
    if (at >= 0x40F3A0 && at < 0x40F5E0 && !*reinterpret_cast<void* const*>(0xC4CD20)) return;
    if (now_s() - g_menu_text_at > 1.0) BWLOG("ui: menu text starts (from %p)", caller);
    g_menu_text_at = now_s();
}
float __cdecl setup_draw_text_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                                     uintptr_t h, uintptr_t i, uintptr_t j, uintptr_t k, uintptr_t l, uintptr_t m, uintptr_t n) {
    note_menu_text(_ReturnAddress());
    return o_setup_draw_text(a, b, c, d, e, f, g, h, i, j, k, l, m, n);
}
float __cdecl setup_draw_text_wrap_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                                          uintptr_t h, uintptr_t i, uintptr_t j, uintptr_t k, uintptr_t l, uintptr_t m, uintptr_t n) {
    note_menu_text(_ReturnAddress());
    return o_setup_draw_text_wrap(a, b, c, d, e, f, g, h, i, j, k, l, m, n);
}

// For the log: each distinct piece of in-game 2D once (where the game lays out speech text and
// its bands can't be seen from the test harness; the player's sessions show it).
void note_in_game_2d(bool wide, float x0, float y0, float x1, float y1) {
    if (!wide) return;
    static std::vector<uint32_t> seen;
    const uint32_t key = (uint32_t(std::lround(y0 / 8.0f)) & 0xFFFF) << 16 | (uint32_t(std::lround(y1 / 8.0f)) & 0xFFFF);
    if (seen.size() >= 40 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    BWLOG("ui: in-game band (%.0f, %.0f)-(%.0f, %.0f) of eye image %.0fx%.0f; text last frame (%.0f, %.0f)-(%.0f, %.0f)", x0, y0, x1, y1,
          g_view.cx * 2, g_view.cy * 2, g_screen.last_text[0], g_screen.last_text[1], g_screen.last_text[2], g_screen.last_text[3]);
}

// ---- The gestures, above the left hand ----
// The flat game shows the player's gestures as small 3D icons at the right edge of the screen (each
// DisplayGesture item drawn by fn_0068B480, with a screen shift: 0xFA9398/9C/A0). In VR that edge is
// only in the right eye's far periphery: a ghost seen out of the corner of the eye, never where the
// eye looked (player, 2026-10-27). Its draws go onto a small panel above the left hand while that
// palm is turned up, the player's book of gestures, and aren't drawn otherwise.
struct GesturePanel {
    bool up = false;          // the left palm is up (with some hysteresis)
    float palm_up = -1, palm_face = -1;  // (as last seen: the palm's normal up, and toward the head)
    Vec3 centre, right, upv;  // world; right/upv unit vectors
    Vec3 toward;              // from the panel toward the head (unit)
    float size = 1;           // world units: the row's longer side
    float src[4] = {1e9f, 1e9f, -1e9f, -1e9f}, last_src[4] = {1e9f, 1e9f, -1e9f, -1e9f};  // the icons, eye-image pixels (left eye)
    bool logged = false;
};
GesturePanel g_gp;
bool g_in_gesture_hud = false;
int g_dbg_gesture_draws = 0;  // debug: log the gesture icons' draws for N eye passes (ui gesture_log N)
using GestureItemDrawFn = void(__fastcall*)(void* item, void* edx);
GestureItemDrawFn o_gesture_item_draw = nullptr;
// Each item (an icon with its gesture) gets a slot on the panel, left to right in the order the
// game lays them out, and is drawn there relative to its own screen rectangle (+0x44 x0, +0x48
// y0, +0x50 x1, +0x54 y1). Where the game puts the row on the screen doesn't matter. (Mapped from
// the screen, the panel jumped and the shapes split from their icons when the game laid the row out
// differently: with the hand to the left of the view; player, 2026-10-27.)
struct ItemSlot {
    const void* item;
    float cx;
};
std::vector<ItemSlot> g_items_now, g_items_last;  // this frame's items (left eye), the last frame's
const void* g_cur_item = nullptr;
void __fastcall gesture_item_draw_detour(void* item, void* edx) {
    if (g_eye == 0 && item && !IsBadReadPtr(static_cast<char*>(item) + 0x44, 0x14)) {
        const float* r = reinterpret_cast<const float*>(static_cast<const char*>(item) + 0x44);
        g_items_now.push_back({item, (r[0] + r[3]) * 0.5f});
    }
    g_in_gesture_hud = true, g_cur_item = item;
    // (The game shifts each icon from the screen's middle to its place and clamps it to the screen's
    // edges; its gesture's shape isn't clamped. With the row near or past an edge (as the hand went
    // out of view) the icons were squashed against it and the shapes beside them; player,
    // 2026-10-28. On the panel the icons are taken unclamped.)
    const bool unclamped = g_eye >= 0 && g_set.gesture_panel;
    if (unclamped) trace::set_hud_unclamped(true);
    // (On the panel the icons keep the game's HUD depth, 0.999, the back of the depth range: the land
    // and even the sky in front of it hid them, and only the shapes showed, with no icons, in the
    // headset; playtest 2026-11-01. Drawn over whatever is nearer, as the labels by the hand.)
    const bool on_top = unclamped && g_gp.up;
    if (on_top) trace::set_on_top(true);
    o_gesture_item_draw(item, edx);
    if (on_top) trace::set_on_top(false);
    if (unclamped) trace::set_hud_unclamped(false);
    g_in_gesture_hud = false, g_cur_item = nullptr;
}
float slot_width() { return g_gp.size / 3.0f; }
// Where a screen pixel of this item goes on the panel; false for an item not laid out yet.
bool item_point(const void* item, float sx, float sy, Vec3& out) {
    int rank = -1, n = int(g_items_last.size());
    for (const ItemSlot& a : g_items_last)
        if (a.item == item) {
            rank = 0;
            for (const ItemSlot& b : g_items_last) rank += b.cx < a.cx ? 1 : 0;
        }
    if (rank < 0 || !item || IsBadReadPtr(static_cast<const char*>(item) + 0x44, 0x14)) return false;
    const float* r = reinterpret_cast<const float*>(static_cast<const char*>(item) + 0x44);
    const float rcx = (r[0] + r[3]) * 0.5f, rcy = (r[1] + r[4]) * 0.5f;
    const float k = slot_width() * 0.85f / std::max(1.0f, std::max(r[3] - r[0], r[4] - r[1]));
    const float x = (float(rank) - float(n - 1) * 0.5f) * slot_width();
    out = g_gp.centre + g_gp.right * (x + (sx - rcx) * k) - g_gp.upv * ((sy - rcy) * k);
    return true;
}
// Each icon's gesture (the pink, animated shape under it) is an LH3DSprite placed in the world:
// fn_0068B420 (item, fractions, out) takes a point of the icon's screen rectangle (+0x44..+0x54)
// and puts it 10 units out along the camera's ray through it (Get3DPointFromScreen). In an eye pass
// that point was nowhere in view and no shape was drawn (player: no gestures on the icons). In VR
// it goes to the same place on the panel as the icon's pixels, a little toward the eye; with the
// palm down, behind the eye (not drawn).
const void* g_glyph_sprite[8] = {};  // the gestures' sprites placed this frame, and their scale
float g_glyph_scale[8] = {};
const void* g_glyph_item[8] = {};     // (and the item and its screen point each was placed for: placed
float g_glyph_sx[8] = {}, g_glyph_sy[8] = {};  // again when drawn, gesture_sprite_drawing)
int g_glyph_n = 0;
struct GlyphPoint {
    Vec3 p;
    const void* item = nullptr;
    float sx = 0, sy = 0, k = 1;
};
constexpr int kGlyphPoints = 16;
GlyphPoint g_glyph_points[kGlyphPoints];
int g_glyph_points_next = 0;
unsigned g_glyph_by_place = 0;
// Where a point of an item's gesture goes (sx, sy: on the item's screen rectangle): on the panel,
// a little toward the eye, moved so the engine's projection, with the camera current now, puts it
// where the eye sees the panel's point. k: the sprite's size factor there. False: not on the panel.
bool glyph_place(const void* item, float sx, float sy, Vec3& p, float& k, Vec3& target) {
    if (!g_gp.up || !item_point(item, sx, sy, p)) return false;
    // (A little in front of the icons: the same point for both eyes.)
    p = p + g_gp.toward * (0.01f * g_gp.size / std::max(1e-3f, g_set.gesture_panel_m));  // (1 cm)
    // (Sized for 10 units out; nearer, it is drawn smaller in proportion, then a little larger.)
    k = 1.4f * vr::length(p - g_view.pos) / 10.0f;
    // The shapes are drawn through the engine's camera as the game's HUD sets it again (no head
    // roll): with the head rolled they stayed level on the screen, off the panel, and went with
    // the head (playtest 2026-11-01). The point is moved until the engine's own projection
    // (LH3DTech::ProjectPoint) puts it where the eye sees the panel's point.
    target = p;  // (where the eye should see it: the log compares the engine's projection with this)
    if (g_set.gesture_match) {
        const Vec3 rr = p - g_view.pos;
        const float Z = vr::dot(rr, g_view.rot.fwd);
        if (Z > 1e-3f) {
            const float ox = g_view.cx + g_view.f * vr::dot(rr, g_view.rot.right) / Z, oy = g_view.cy - g_view.f * vr::dot(rr, g_view.rot.up) / Z;
            // (The engine's screen is turned against the eye's by the roll: how the engine's
            // projection moves for a step right and a step up is measured, and solved for.)
            const auto proj = [](const Vec3& v, float& x, float& y) {
                int ix = 0, iy = 0;
                float depth = 0;
                if (!game::project_point({v.x, v.y, v.z}, &ix, &iy, &depth)) return false;
                x = float(ix), y = float(iy);
                return true;
            };
            const float e = 40.0f * Z / g_view.f;  // (a step of about 40 pixels)
            Vec3 q = p;
            float miss = 1e9f;  // (pixels between the engine's projection of q and the eye's of the panel's point)
            for (int it = 0; it < 4; ++it) {
                float x0, y0, xr, yr, xu, yu;
                if (!proj(q, x0, y0)) break;
                const float dx = ox - x0, dy = oy - y0;
                miss = std::max(std::fabs(dx), std::fabs(dy));
                if (miss < 0.5f || !proj(q + g_view.rot.right * e, xr, yr) || !proj(q + g_view.rot.up * e, xu, yu)) break;
                const float a = xr - x0, b = xu - x0, c = yr - y0, d = yu - y0, det = a * d - b * c;  // (pixels per step)
                if (std::fabs(det) < 1.0f) break;
                const float sr = (d * dx - b * dy) / det, su = (a * dy - c * dx) / det;  // (steps right, up)
                q = q + g_view.rot.right * (sr * e) + g_view.rot.up * (su * e);
                float x1, y1;
                miss = proj(q, x1, y1) ? std::max(std::fabs(ox - x1), std::fabs(oy - y1)) : 1e9f;
            }
            // Taken only once checked: the engine's own projection of the moved point lands
            // where the eye sees the panel's (within 4 px), and it moved under half the way
            // (with the head rolled 30 degrees, points near the panel's ends move 37%). Before,
            // the move was taken unchecked, and in the headset the shapes stood far off to the
            // side of their icons, at the horizon, going with the head (playtest 2026-11-03,
            // palm up toward the left of the view; not reproduced in the harness). Otherwise
            // the point is left as it is (off by the head's roll at most), and the log says so.
            const bool good = miss <= 4.0f && vr::length(q - p) < 0.5f * vr::length(rr);
            if (good) p = q;
            else {
                static double logged_at = -10;
                if (now_s() - logged_at > 5.0) {
                    logged_at = now_s();
                    BWLOG("ui: gesture shape: the engine's projection not matched (eye %d, %.0f px off after solving, moved %.2f of %.2f): "
                          "left on the panel unsolved", g_eye, miss, vr::length(q - p), vr::length(rr));
                }
            }
        }
    }
    return true;
}
using GesturePointFn = void(__fastcall*)(void* item, void* edx, const float* frac, float* out);
GesturePointFn o_gesture_point = nullptr;
unsigned g_gesture_points[2] = {};  // (debug: the shapes' placements in an eye pass, and outside one)
void __fastcall gesture_point_detour(void* item, void* edx, const float* frac, float* out) {
    o_gesture_point(item, edx, frac, out);
    ++g_gesture_points[g_eye < 0 ? 1 : 0];
    // (Outside an eye pass too: left there, the game's own place, 10 units out along its camera (the
    // head's), stayed, and the shapes went with the head, tilting as it rolled, out by the horizon;
    // playtest 2026-11-01.)
    if (!g_set.gesture_panel || !frac || !out) return;
    const float* r = reinterpret_cast<const float*>(static_cast<const char*>(item) + 0x44);  // x0, y0, (z), x1, y1
    const float sx = r[0] + (r[3] - r[0]) * frac[0], sy = r[1] + (r[4] - r[1]) * frac[2];
    Vec3 p, target;
    float k = 1;
    if (glyph_place(item, sx, sy, p, k, target)) {
        // (Every point handed out is remembered, by where it was put: the preview dot running round a
        // shape is placed through a value on the stack, not a sprite, and its sprite is known only by
        // being drawn at that place; see glyph_begin.)
        g_glyph_points[g_glyph_points_next] = {p, item, sx, sy, k};
        g_glyph_points_next = (g_glyph_points_next + 1) % kGlyphPoints;
        int i = 0;
        while (i < g_glyph_n && g_glyph_sprite[i] != out) ++i;
        if (i < 8) {
            g_glyph_sprite[i] = out, g_glyph_scale[i] = k, g_glyph_n = std::max(g_glyph_n, i + 1);
            g_glyph_item[i] = item, g_glyph_sx[i] = sx, g_glyph_sy[i] = sy;
        }
        if (g_dbg_gesture_draws > 0) {
            int ex = 0, ey = 0;
            float depth = 0;
            const uint32_t vis = game::project_point({p.x, p.y, p.z}, &ex, &ey, &depth);
            const Vec3 r = target - g_view.pos;
            const float Z = vr::dot(r, g_view.rot.fwd);
            BWLOG("ui: gesture point (eye %d) item %p frac (%.2f, %.2f) screen (%.0f, %.0f) out %p: engine (%d, %d) vis %u, ours (%.0f, %.0f)", g_eye, item,
                  frac[0], frac[2], sx, sy, static_cast<void*>(out), ex, ey, vis, g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z,
                  g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z);
        }
    } else {
        p = g_view.pos - g_view.rot.fwd * 1000.0f;
    }
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}
trace::Filter gesture_filter(unsigned long fvf, const void* verts, unsigned long count, size_t stride, const void** out) {
    using trace::Filter;
    if (g_eye < 0 || !g_set.gesture_panel) return Filter::kKeep;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || stride < 16) {
        if (g_dbg_gesture_draws > 0) BWLOG("ui: gesture draw (eye %d) not pre-transformed (fvf %lX): skipped", g_eye, fvf);
        return Filter::kSkip;
    }
    const auto* base = static_cast<const uint8_t*>(verts);
    // (The icons come through the HUD's own camera, at depths 0.95-1. The gestures' sprites are
    // drawn in the world (placed on the panel by gesture_point_detour): left as they are.)
    for (unsigned long i = 0; i < count; ++i)
        if (reinterpret_cast<const float*>(base + i * stride)[2] < 0.9f) return Filter::kKeep;
    if (g_eye == 0)
        for (unsigned long i = 0; i < count; ++i) {
            const float* v = reinterpret_cast<const float*>(base + i * stride);
            g_gp.src[0] = std::min(g_gp.src[0], v[0]), g_gp.src[1] = std::min(g_gp.src[1], v[1]);
            g_gp.src[2] = std::max(g_gp.src[2], v[0]), g_gp.src[3] = std::max(g_gp.src[3], v[1]);
        }
    const float* b = g_gp.last_src;
    if (!g_gp.up || !g_cur_item) return Filter::kSkip;
    if (!g_gp.logged) {
        g_gp.logged = true;
        BWLOG("ui: the gesture icons shown above the left hand (as drawn: (%.0f, %.0f)-(%.0f, %.0f) px)", b[0], b[1], b[2], b[3]);
    }
    // (In front of everything, in the game's order: its depths 0.95-1 brought to just in front of
    // the eye.)
    g_scratch.assign(base, base + size_t(count) * stride);
    const float near_clip = game::info_transform().near_clip;
    for (unsigned long i = 0; i < count; ++i) {
        float* v = reinterpret_cast<float*>(g_scratch.data() + i * stride);
        Vec3 p;
        if (!item_point(g_cur_item, v[0], v[1], p)) return Filter::kSkip;
        const Vec3 r = p - g_view.pos;
        const float Z = vr::dot(r, g_view.rot.fwd);
        if (Z < near_clip * 2.0f) return Filter::kSkip;
        v[0] = g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z;
        v[1] = g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z;
        v[3] = near_clip / Z;  // (the engine's rhw: near/Z; the one-pass replay moves vertices by it)
        v[2] = 0.001f + std::clamp(v[2] - 0.94f, 0.0f, 0.06f) * 0.1f;
    }
    if (g_dbg_gesture_draws > 0) {
        float a[4] = {1e9f, 1e9f, -1e9f, -1e9f}, o[4] = {1e9f, 1e9f, -1e9f, -1e9f}, zr[2] = {1e9f, -1e9f};
        for (unsigned long i = 0; i < count; ++i) {
            const float* v = reinterpret_cast<const float*>(base + i * stride);
            const float* w = reinterpret_cast<const float*>(g_scratch.data() + i * stride);
            a[0] = std::min(a[0], v[0]), a[1] = std::min(a[1], v[1]), a[2] = std::max(a[2], v[0]), a[3] = std::max(a[3], v[1]);
            o[0] = std::min(o[0], w[0]), o[1] = std::min(o[1], w[1]), o[2] = std::max(o[2], w[0]), o[3] = std::max(o[3], w[1]);
            zr[0] = std::min(zr[0], v[2]), zr[1] = std::max(zr[1], v[2]);
        }
        unsigned amin = 255, amax = 0;  // (the diffuse colour's alpha, right after x, y, z, rhw)
        if (fvf & D3DFVF_DIFFUSE)
            for (unsigned long i = 0; i < count; ++i) {
                const unsigned a8 = reinterpret_cast<const uint32_t*>(base + i * stride)[4] >> 24;
                amin = std::min(amin, a8), amax = std::max(amax, a8);
            }
        BWLOG("ui: gesture draw (eye %d) %lu verts fvf %lX: (%.0f, %.0f)-(%.0f, %.0f) z %.3f..%.3f -> (%.0f, %.0f)-(%.0f, %.0f) alpha %u..%u", g_eye,
              count, fvf, a[0], a[1], a[2], a[3], zr[0], zr[1], o[0], o[1], o[2], o[3], amin, amax);
    }
    *out = g_scratch.data();
    return Filter::kReplace;
}

// Is one of the spirits out, speaking? HelpSystem (GGame +0x25005C) keeps them at +8 and +0xC;
// HelpSystem::IsSpiritEjected (0x5C6550, thiscall(type 0 or 1)). Their captions are the spirits'
// speech; without one out, a help line is the game's own note, as the creature's state ("Your
// Creature is hungry..."), which comes often.
bool spirit_out() {
    void* game = game::game_instance();
    const char* hs = game ? game::game_field<const char*>(game, 0x25005C) : nullptr;
    if (!hs || IsBadReadPtr(hs, 0x10)) return true;
    if (!*reinterpret_cast<void* const*>(hs + 8) || !*reinterpret_cast<void* const*>(hs + 0xC)) return true;
    using EjectedFn = int(__thiscall*)(const void*, int);
    const auto ejected = reinterpret_cast<EjectedFn>(0x5C6550);
    return ejected(hs, 0) != 0 || ejected(hs, 1) != 0;
}

// Does the game take a click on the line it shows now? HelpSystem's text is shown in a mode
// (fn_005C6100: 1 kept longer, 2 not to be clicked away: +0x580 set), and ProcessInterface
// (0x5C69B0) ignores clicks while +0x580 is set. The spirits' idle banter showed "trigger -
// continue" with nothing the trigger could do (playtest 2026-11-03).
// A script waiting for the line to be read: GScript::TextRead (0x6F8260, cdecl, no arguments) is
// what a script's "wait until read" polls each turn. Only then does a click move anything on: a rival
// god's speech ("I'm fighting for the land you see below you") is clickable but nobody waits on it,
// and it goes by itself; it showed the prompt (playtest 2026-11-04).
using TextReadFn = void(__cdecl*)();
TextReadFn o_text_read = nullptr;
double g_text_read_polled = -10;
void __cdecl text_read_detour() {
    g_text_read_polled = now_s();
    o_text_read();
}
bool script_waits_for_read() { return now_s() - g_text_read_polled < 1.0; }
bool help_line_clickable() {
    void* game = game::game_instance();
    const char* hs = game ? game::game_field<const char*>(game, 0x25005C) : nullptr;
    if (!hs || IsBadReadPtr(hs + 0x57C, 8)) return true;
    return *reinterpret_cast<const int*>(hs + 0x580) == 0;
}
int help_line_mode() {
    void* game = game::game_instance();
    const char* hs = game ? game::game_field<const char*>(game, 0x25005C) : nullptr;
    if (!hs || IsBadReadPtr(hs + 0x57C, 8)) return -1;
    return *reinterpret_cast<const int*>(hs + 0x580) ? 2 : *reinterpret_cast<const int*>(hs + 0x57C) ? 1 : 0;
}

// How much larger the captions are drawn: [ui] caption_scale_percent, less for a line too long to
// stay within caption_view_deg of view (the last frame's words' width). A help line with no spirit
// out (the creature's state, coming often) only [ui] status_scale_percent: enlarged with the spirits'
// speech, it was in the way (playtest 2026-11-01 evening).
float caption_scale() {
    static double checked_at = -10;
    static bool spirit = true;
    if (now_s() - checked_at > 0.25) {
        checked_at = now_s();
        const bool now = spirit_out();
        if (now != spirit) BWLOG("ui: captions: %s", now ? "a spirit is out: its speech enlarged" : "no spirit out: help lines smaller");
        spirit = now;
    }
    float k = std::max(1.0f, spirit ? g_set.caption_scale : g_set.status_scale);
    const float w = g_caption_last[2] - g_caption_last[0];
    if (w > 0 && g_set.screen_width_m > 0) {
        const float fit = 2.0f * std::tan(g_set.caption_view_deg * 0.5f * vr::kDegToRad) * g_set.screen_dist_m / g_set.screen_width_m *
                          float(g_screen.w);  // (layout pixels within that angle)
        k = std::clamp(fit / (w + 100.0f), 1.0f, k);  // (the band reaches 50 px past the words)
    }
    return k;
}

// The gesture shapes, drawn onto the panel by their own corners (player, 2026-11-08: "still flying
// away from the icon", after every way of placing the sprite in the world so that the engine's camera
// would draw it there: the camera that draws it isn't the eye's, and isn't the same from one moment to
// the next). Now the sprite is put straight ahead of whatever camera the engine has as it draws it
// (so it is always drawn), and its corners, as the engine outputs them, are taken relative to where
// its middle came out and laid on the panel about the icon's point, a pixel of the engine's being so
// many world units at that distance, then projected for the eye as the icons are. The engine's camera
// no longer matters.
struct GlyphDraw {
    bool on = false;
    Vec3 anchor;           // the panel point the sprite's middle goes to
    float ex = 0, ey = 0;  // where the engine projects the sprite's middle (its pixels)
    float unit = 1;        // world units on the panel per engine pixel
};
GlyphDraw g_glyph_draw;
unsigned g_glyph_remapped = 0, g_glyph_failed = 0;  // (for the log)
bool glyph_begin(void* sprite, float saved[3]) {
    g_glyph_draw.on = false;
    if (!g_set.gesture_remap || !g_gp.up || g_eye < 0 || !g_set.gesture_panel || !sprite || IsBadWritePtr(sprite, 12)) return false;
    int i = 0;
    while (i < g_glyph_n && g_glyph_sprite[i] != sprite) ++i;
    const void* item = nullptr;
    float gsx = 0, gsy = 0;
    if (i < g_glyph_n) {
        item = g_glyph_item[i], gsx = g_glyph_sx[i], gsy = g_glyph_sy[i];
    } else {
        // (A sprite drawn where a point was just handed out: the preview dot. It flickered on the panel
        // with the hand to the left, placed in the world for the engine's camera; player, 2026-11-08.)
        const float* sp = static_cast<const float*>(sprite);
        const Vec3 at{sp[0], sp[1], sp[2]};
        for (const GlyphPoint& q : g_glyph_points)
            if (q.item && vr::length(q.p - at) < 1e-3f) {
                item = q.item, gsx = q.sx, gsy = q.sy;
                ++g_glyph_by_place;
                break;
            }
        if (!item) return false;
    }
    Vec3 P;
    if (!item || IsBadReadPtr(static_cast<const char*>(item) + 0x44, 0x14) || !item_point(item, gsx, gsy, P)) return false;
    P = P + g_gp.toward * (0.01f * g_gp.size / std::max(1e-3f, g_set.gesture_panel_m));  // (1 cm in front of the icons)
    const float dist = vr::length(P - g_view.pos);
    // The engine's camera as it is now: world_to_camera's columns are its right, up and forward; its
    // last row the translation.
    const game::LHMatrix& w = game::world_to_camera();
    const Vec3 a0{w.m[0][0], w.m[1][0], w.m[2][0]}, a1{w.m[0][1], w.m[1][1], w.m[2][1]}, a2{w.m[0][2], w.m[1][2], w.m[2][2]};
    const Vec3 cam = (a0 * w.m[3][0] + a1 * w.m[3][1] + a2 * w.m[3][2]) * -1.0f;
    const Vec3 Q = cam + a2 * dist;
    const auto proj = [](const Vec3& v, float& x, float& y) {
        int ix = 0, iy = 0;
        float depth = 0;
        if (!game::project_point({v.x, v.y, v.z}, &ix, &iy, &depth)) return false;
        x = float(ix), y = float(iy);
        return true;
    };
    const float step = 0.05f * dist;
    float ex, ey, xr, yr;
    if (!proj(Q, ex, ey) || !proj(Q + a0 * step, xr, yr)) {
        ++g_glyph_failed;
        return false;
    }
    const float px_per_unit = std::sqrt((xr - ex) * (xr - ex) + (yr - ey) * (yr - ey)) / step;
    if (px_per_unit < 1e-3f) {
        ++g_glyph_failed;
        return false;
    }
    float* sp = static_cast<float*>(sprite);
    saved[0] = sp[0], saved[1] = sp[1], saved[2] = sp[2];
    sp[0] = Q.x, sp[1] = Q.y, sp[2] = Q.z;
    g_glyph_draw = {true, P, ex, ey, 1.0f / px_per_unit};
    return true;
}
void glyph_end(void* sprite, const float saved[3]) {
    if (!g_glyph_draw.on) return;
    g_glyph_draw.on = false;
    float* sp = static_cast<float*>(sprite);
    sp[0] = saved[0], sp[1] = saved[1], sp[2] = saved[2];
}
trace::Filter glyph_filter(unsigned long fvf, const void* verts, unsigned long count, size_t stride, const void** out) {
    using trace::Filter;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || stride < 16) return Filter::kKeep;
    const auto* base = static_cast<const uint8_t*>(verts);
    g_scratch.assign(base, base + size_t(count) * stride);
    const float near_clip = game::info_transform().near_clip;
    const GlyphDraw& g = g_glyph_draw;
    for (unsigned long i = 0; i < count; ++i) {
        float* v = reinterpret_cast<float*>(g_scratch.data() + i * stride);
        const Vec3 W = g.anchor + g_gp.right * ((v[0] - g.ex) * g.unit) - g_gp.upv * ((v[1] - g.ey) * g.unit);
        const Vec3 r = W - g_view.pos;
        const float Z = vr::dot(r, g_view.rot.fwd);
        if (Z < near_clip * 2.0f) return Filter::kSkip;
        v[0] = g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z;
        v[1] = g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z;
        v[3] = near_clip / Z;  // (the one-pass replay moves vertices by it)
        v[2] = 0.0005f;        // (in front of the icons)
    }
    ++g_glyph_remapped;
    *out = g_scratch.data();
    return Filter::kReplace;
}

// Sprites (LH3DSprite: the sun's glow, glows about villagers and miracles, some clouds) are laid out
// by the engine on its screen, lined up with the screen's edges: upright in the flat game, whose
// screen never rolls. In the headset the screen rolls with the head, and they turned with it
// (player, 2026-11-08: "the sun always rotates with the headset"; effects "anchored to the rotation
// of my headset"). Each is turned about its middle by the angle between the screen's up and the
// world's up where it is (the engine's own projection of a step up from it).
struct SpriteUpright {
    bool on = false;
    float cx = 0, cy = 0, c = 1, s = 0;
};
SpriteUpright g_sprite_upright;
float g_glare_sign = 1.0f;  // (ui glare_sign: which way the glare is turned, found in the harness)
int g_dbg_far_sprites = 0;
unsigned g_sprites_turned = 0;
trace::Filter upright_filter(unsigned long fvf, const void* verts, unsigned long count, size_t stride, const void** out) {
    using trace::Filter;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || stride < 16) return Filter::kKeep;
    const auto* base = static_cast<const uint8_t*>(verts);
    g_scratch.assign(base, base + size_t(count) * stride);
    const SpriteUpright& u = g_sprite_upright;
    for (unsigned long i = 0; i < count; ++i) {
        float* v = reinterpret_cast<float*>(g_scratch.data() + i * stride);
        const float dx = v[0] - u.cx, dy = v[1] - u.cy;
        v[0] = u.cx + u.c * dx - u.s * dy;
        v[1] = u.cy + u.s * dx + u.c * dy;
    }
    ++g_sprites_turned;
    *out = g_scratch.data();
    return Filter::kReplace;
}

// 2D draws: the panels and buttons are pre-transformed quads with z = 0 and rhw = 1; text glyphs
// are whatever the game's text function draws. Everything else in an eye pass is the world.
trace::Filter screen_filter(unsigned long fvf, const void* verts, unsigned long count, size_t stride, const void** out) {
    using trace::Filter;
    if (g_glyph_draw.on) return glyph_filter(fvf, verts, count, stride, out);
    if (g_sprite_upright.on) return upright_filter(fvf, verts, count, stride, out);
    if (g_in_gesture_hud) return gesture_filter(fvf, verts, count, stride, out);
    if (g_eye < 0 || !g_set.screen || !g_screen.valid || g_passthrough) return Filter::kKeep;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || stride < 16) return Filter::kKeep;
    const auto* base = static_cast<const uint8_t*>(verts);
    bool sprite = false;
    if (!g_in_text) {
        for (unsigned long i = 0; i < count; ++i) {
            const float* v = reinterpret_cast<const float*>(base + i * stride);
            if (v[2] != 0.0f || v[3] != 1.0f) return Filter::kKeep;
        }
        sprite = true;
    }
    g_scratch.assign(base, base + size_t(count) * stride);
    const Screen& sc = g_screen;
    // Which layout is this draw in? The window's, unless it reaches beyond the window: then the
    // eye image's, brought into the window's shape by keeping its upper half against the top and
    // its lower half against the bottom. (Seen in a player session: speech text and its band
    // are laid out in window pixels, x up to 1920, like the menus; the letterbox bars and fades
    // span the eye image, x up to 1971.)
    const float eye_w = g_view.cx * 2.0f, eye_h = g_view.cy * 2.0f;
    bool eye_layout = false;
    float y_min = 1e9f, y_max = -1e9f;
    for (unsigned long i = 0; i < count; ++i) {
        const float* v = reinterpret_cast<const float*>(base + i * stride);
        if (v[0] > float(sc.w) + 2.0f || v[1] > float(sc.h) + 2.0f) eye_layout = true;
        y_min = std::min(y_min, v[1]), y_max = std::max(y_max, v[1]);
    }
    const float fit = float(sc.w) / std::max(1.0f, eye_w);
    const bool whole_lower = (y_min + y_max) * 0.5f > eye_h * 0.5f;
    const bool tall = y_max - y_min > eye_h / 3.0f;  // spans the screen: each vertex to its own edge
    // Strips across the whole width. One that is also tall is a fade over the whole view: it stays
    // on the view (on the panel it would be a black rectangle in the middle of the scene). A
    // shallow one is the band behind speech text: it goes on the panel with its text, and only as
    // wide as that text (the game makes it the width of the screen).
    float x_min = 1e9f, x_max = -1e9f;
    for (unsigned long i = 0; i < count; ++i) {
        const float x = reinterpret_cast<const float*>(base + i * stride)[0];
        x_min = std::min(x_min, x), x_max = std::max(x_max, x);
    }
    const bool wide = sprite && x_max - x_min > std::min(eye_w, float(sc.w)) * 0.9f;
    if (wide && tall) return Filter::kKeep;
    // A shallow strip across the whole eye image is a letterbox bar sliding in or out around a
    // cutscene (which itself plays on the cinema screen): not drawn.
    if (wide && eye_layout) return Filter::kSkip;
    float band_left = x_min, band_right = x_max;
    if (wide) {
        const float* tb = g_screen.last_text;
        if (tb[2] > tb[0] && tb[3] > y_min - 30.0f && tb[1] < y_max + 30.0f)
            band_left = tb[0] - 50.0f, band_right = tb[2] + 50.0f;
    }
    if (g_eye == 0) note_in_game_2d(wide, x_min, y_min, x_max, y_max);
    if (g_eye == 0 && wide && !eye_layout) g_band_seen = true;
    const float near_clip = game::info_transform().near_clip;
    const bool in_menu = now_s() - g_menu_text_at < 0.3;
    // A caption (its words, or the band behind them): larger, about the band's middle.
    const bool band = wide && !eye_layout;
    if (band) g_caption_mid_y = (y_min + y_max) * 0.5f;
    const float cap_k = (g_in_caption || band) && !in_menu ? caption_scale() : 1.0f;
    const float cap_y = g_caption_mid_y >= 0 ? g_caption_mid_y : float(sc.h) * 0.9f;
    for (unsigned long i = 0; i < count; ++i) {
        float* v = reinterpret_cast<float*>(g_scratch.data() + i * stride);
        float lx = v[0], ly = v[1];
        if (g_in_text && g_eye == 0 && !g_drawing_prompt) {  // where this frame's text is, as the game laid it out
            float* tb = g_screen.text;
            tb[0] = std::min(tb[0], lx), tb[1] = std::min(tb[1], ly), tb[2] = std::max(tb[2], lx), tb[3] = std::max(tb[3], ly);
            if (g_in_caption) {
                float* cb = g_caption_text;
                cb[0] = std::min(cb[0], lx), cb[1] = std::min(cb[1], ly), cb[2] = std::max(cb[2], lx), cb[3] = std::max(cb[3], ly);
            }
        }
        if (wide) lx = band_left + (lx - x_min) / (x_max - x_min) * (band_right - band_left);
        if (cap_k != 1.0f) lx = float(sc.w) * 0.5f + (lx - float(sc.w) * 0.5f) * cap_k, ly = cap_y + (ly - cap_y) * cap_k;
        if (g_in_stats && !in_menu) {
            if (g_eye == 0) {
                float* sb = g_stats_box;
                sb[0] = std::min(sb[0], lx), sb[1] = std::min(sb[1], ly), sb[2] = std::max(sb[2], lx), sb[3] = std::max(sb[3], ly);
            }
            const float* lb = g_stats_last[2] > g_stats_last[0] ? g_stats_last : g_stats_box;
            if (lb[2] > lb[0]) {
                const float cx = (lb[0] + lb[2]) * 0.5f, cy = (lb[1] + lb[3]) * 0.5f;
                const float tx = cx + (float(sc.w) * 0.5f - cx) * g_set.stats_toward_middle;
                lx = tx + (lx - cx) * g_set.stats_scale, ly = cy + (ly - cy) * g_set.stats_scale;
            }
        }
        if (eye_layout) {
            const bool lower = tall ? ly > eye_h * 0.5f : whole_lower;
            lx *= fit;
            ly = lower ? float(sc.h) - (eye_h - ly) * fit : ly * fit;
        }
        // Menu panels and buttons: where the laser is the mouse. Only in a menu: the help text and
        // speech bands of play are on the panel too, and a hand pointed low in front (petting at a
        // low angle) would land on them and be turned into the laser.
        if (sprite && !eye_layout && g_eye == 0 && in_menu) {
            g_screen.box[0] = std::min(sc.box[0], lx), g_screen.box[1] = std::min(sc.box[1], ly);
            g_screen.box[2] = std::max(sc.box[2], lx), g_screen.box[3] = std::max(sc.box[3], ly);
        }
        const Vec3 p = sc.centre + sc.right * ((lx - sc.w * 0.5f) * sc.unit) - sc.up * ((ly - sc.h * 0.5f) * sc.unit);
        const Vec3 r = p - g_view.pos;
        const float Z = vr::dot(r, g_view.rot.fwd);
        if (Z < near_clip * 2.0f) return Filter::kSkip;  // the panel is beside or behind the eye
        v[0] = g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z;
        v[1] = g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z;
        v[3] = near_clip / Z;  // perspective-correct textures (the engine's near/Z); z stays the game's "in front of everything"
    }
    g_screen.last_drawn = now_s();
    *out = g_scratch.data();
    return Filter::kReplace;
}

// Where the hand's hints go (world): above the hand at the controller, or above the laser's
// point. `unit` is one metre at the player's scale there; for the laser hand it grows with the
// distance, so the words keep a readable size.
bool hint_anchor(Vec3& anchor, float& unit, float& above) {
    hand::NearHand nh;
    const float scale = hand::rig_scale();
    Vec3 p;
    if (hand::near_hand(nh) && nh.blend > 0.5f) {
        unit = scale;
        above = g_set.hint_above_m;
        anchor = nh.pos + nh.z * (0.08f * scale);  // the palm
        return true;
    }
    if (hand::laser_anchor(p)) {
        const float dist = vr::length(p - g_view.pos);
        unit = std::max(scale, dist / 0.6f);  // as if the hand were 60 cm away
        above = g_set.hint_above_m * 0.5f;    // the laser hand lies flat on its point
        anchor = p;
        return true;
    }
    return false;
}

// Draws `text` centred at the world point `p` in this eye pass, `height_m` tall at `unit` world
// units per metre (clamped to readable pixel sizes), in front of everything.
void draw_text_at(const wchar_t* text, const Vec3& p, float unit, float height_m, float alpha) {
    const HintStyle& style = g_style.font ? g_style : g_fallback;
    if (!style.font || style.passes.empty()) return;
    const Vec3 r = p - g_view.pos;
    const float Z = vr::dot(r, g_view.rot.fwd);
    const float near_clip = game::info_transform().near_clip;
    if (Z < near_clip * 1.2f) return;
    const float sx = g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z;
    const float sy = g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z;
    // The letter size is rounded to whole pixels from the left eye's depth in both eyes, so the
    // word is the same size in each (only its position differs).
    const float size = std::round(std::clamp(g_view.f * height_m * unit / Z, g_set.hint_min_px, g_set.hint_max_px));
    const int count = int(wcslen(text));
    const float width = 0.5f * size * float(count);  // roughly: the font is proportional
    const float k = size / std::max(1.0f, style.size);
    if (g_textlog > 0) {
        --g_textlog;
        BWLOG("ui: own text eye %d at (%.1f, %.1f) Z %.2f size %.0f unit %.1f point (%.1f, %.1f, %.1f) colour %08X flags %d", g_eye, sx,
              sy, Z, size, unit, p.x, p.y, p.z, style.passes[0].colour, style.passes[0].flags);
    }
    // In front of everything: by the hand a label is otherwise buried in whatever the hand holds.
    const float depth = g_set.hint_depth > 0 ? Z * g_set.hint_depth : near_clip * 1.1f;
    trace::set_record_depth(Z);  // (drawn at a fake depth: the one-pass replay moves it by the real one)
    trace::set_on_top(true);
    for (const Pass& pass : style.passes) {
        // (The style was learned while the game's hint was fading in: its own alpha is not used.)
        const uint32_t opacity = uint32_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24;
        uint32_t c = (pass.colour & 0x00FFFFFFu) | opacity, c2 = (pass.colour2 & 0x00FFFFFFu) | opacity;
        g_passthrough = true;
        o_draw_text_raw(style.font, text, count, sx - width * 0.5f + pass.dx * k, sy - size * 0.5f + pass.dy * k, depth, size, &c,
                        pass.flags, pass.has_colour2 ? &c2 : nullptr, pass.a, pass.b);
        g_passthrough = false;
    }
    trace::set_record_depth(0);
    trace::set_on_top(false);
}

// A label by the hand at the controller, brought nearer the eye than the hand (along the same line
// of sight, so it stays where it was on screen): with the hand low and the head looking down at it,
// a label above the palm was drawn over the hand but lay behind it in depth, and the eyes saw two of
// it (playtest 2026-11-01: the carried count). At least [ui] label_before_hand_cm nearer than the palm.
Vec3 before_hand(const Vec3& p) {
    hand::NearHand nh;
    if (!hand::near_hand(nh) || nh.blend < 0.5f || g_set.label_before_hand_m <= 0) return p;
    const float d_palm = vr::length(nh.palm - g_view.pos), d = vr::length(p - g_view.pos);
    const float most = d_palm - g_set.label_before_hand_m * hand::rig_scale();
    if (d <= most || most <= 0.1f * hand::rig_scale()) return p;
    return g_view.pos + (p - g_view.pos) * (most / d);
}

void draw_hint(const wchar_t* text, float alpha) {
    Vec3 anchor;
    float unit = 1, above = 0;
    if (!g_style.font || g_style.passes.empty() || !hint_anchor(anchor, unit, above)) return;
    auto it = std::find(g_hints_drawn.begin(), g_hints_drawn.end(), text);
    if (it == g_hints_drawn.end()) it = g_hints_drawn.insert(g_hints_drawn.end(), text);
    const int hint_line = int(it - g_hints_drawn.begin());
    const float line = g_set.hint_height_m * 1.5f * unit;
    const Vec3 at = anchor + Vec3{0, above * unit + line * float(hint_line), 0};
    const Vec3 moved = before_hand(at);
    // (Nearer, it is drawn as large to the eye as before: the unit goes down with the distance.)
    const float k = vr::length(moved - g_view.pos) / std::max(1e-3f, vr::length(at - g_view.pos));
    draw_text_at(text, moved, unit * k, g_set.hint_height_m, alpha);
}

}  // namespace

void configure() {
    configure_save_name();
    g_set.hand_hints = paths::ini_int(L"ui", L"hand_hints", 1) != 0;
    g_set.spinner_at_hand = paths::ini_int(L"ui", L"spinner_at_hand", 1) != 0;
    g_set.held_count = paths::ini_int(L"ui", L"held_count", 1) != 0;
    g_set.screen_follow_m = float(paths::ini_int(L"ui", L"screen_follow_cm", 30)) / 100.0f;
    g_set.menu_width_m = float(paths::ini_int(L"ui", L"menu_width_cm", 500)) / 100.0f;
    g_set.caption_scale = float(paths::ini_int(L"ui", L"caption_scale_percent", 200)) / 100.0f;
    g_set.status_scale = float(paths::ini_int(L"ui", L"status_scale_percent", 125)) / 100.0f;
    g_set.stats_scale = float(paths::ini_int(L"ui", L"stats_scale_percent", 200)) / 100.0f;
    g_set.stats_toward_middle = float(paths::ini_int(L"ui", L"stats_toward_middle_percent", 50)) / 100.0f;
    g_set.caption_view_deg = float(paths::ini_int(L"ui", L"caption_view_deg", 70));
    g_set.spinner_scale = float(paths::ini_int(L"ui", L"spinner_scale_percent", 200)) / 100.0f;
    g_set.world_text_scale = float(paths::ini_int(L"ui", L"world_text_scale_percent", 200)) / 100.0f;
    g_set.label_before_hand_m = float(paths::ini_int(L"ui", L"label_before_hand_cm", 15)) / 100.0f;
    g_help_panel = paths::ini_int(L"ui", L"help_panel", 1) != 0;
    g_set.screen = paths::ini_int(L"ui", L"screen", 1) != 0;
    g_set.gesture_panel = paths::ini_int(L"ui", L"gesture_panel", 1) != 0;
    g_set.gesture_panel_m = float(paths::ini_int(L"ui", L"gesture_panel_cm", 30)) / 100.0f;
    g_set.gesture_remap = paths::ini_int(L"ui", L"gesture_remap", 1) != 0;
    g_set.sprites_upright = paths::ini_int(L"ui", L"sprites_upright", 1) != 0;
    g_set.hint_delay = float(paths::ini_int(L"ui", L"hint_delay_ms", 1000)) / 1000.0f;
    g_set.hint_controls = paths::ini_int(L"ui", L"hint_controls", 1) != 0;
    g_set.info_hints = paths::ini_int(L"ui", L"info_hints", 1) != 0;
    g_continue_after_s = float(paths::ini_int(L"ui", L"continue_prompt_s", 3));
    wchar_t buf[256];
    GetPrivateProfileStringW(L"ui", L"hidden_hints", L"Throw,Drop", buf, 256, paths::ini_file().c_str());
    g_set.hidden.clear();
    std::wstring word;
    for (const wchar_t* c = buf;; ++c) {
        if (*c == L',' || !*c) {
            while (!word.empty() && word.back() == L' ') word.pop_back();
            if (!word.empty()) g_set.hidden.push_back(word);
            word.clear();
            if (!*c) break;
        } else if (*c != L' ' || !word.empty()) {
            word += *c;
        }
    }
}

// (debug: how often the menus' tick sounds, RussClickNoise 0x4082F0, played when the cursor goes
// onto another button; ui click_noises logs the count)
using VoidFn = void(__cdecl*)();
VoidFn o_click_noise = nullptr;
unsigned g_click_noises = 0;
void __cdecl click_noise_detour() {
    ++g_click_noises;
    o_click_noise();
}
// The sun's glare (fn_0086BB60, from LH3DRender::FinishFrame: the yellow glow about the sun, drawn
// flat on the screen) is placed where the sun would be for the camera without its roll: with the
// head tilted it stood off the sun's disc, and went "all over the place" as the head turned and nodded
// (player, 2026-11-08; the disc itself, the sky's own draw, is right). Rolling a camera only turns its
// picture about the middle, so the glare's corners are turned about the screen's middle by the angle
// the world's up makes there.
VoidFn o_sun_glare = nullptr;
bool g_glare_fix = true;  // (ui glare_fix 0: as the game draws it)
unsigned g_glare_calls[2] = {}, g_glare_turned = 0;
void __cdecl sun_glare_detour() {
    g_sprite_upright.on = false;
    ++g_glare_calls[g_eye >= 0 ? 0 : 1];
    if (g_glare_fix && g_eye >= 0) {
        const game::LHMatrix& w = game::world_to_camera();
        const Vec3 a0{w.m[0][0], w.m[1][0], w.m[2][0]}, a1{w.m[0][1], w.m[1][1], w.m[2][1]}, a2{w.m[0][2], w.m[1][2], w.m[2][2]};
        const Vec3 cam = (a0 * w.m[3][0] + a1 * w.m[3][1] + a2 * w.m[3][2]) * -1.0f;
        const Vec3 ahead = cam + a2 * 100.0f;
        int cx = 0, cy = 0;
        float depth = 0;
        // (World up in the eye's axes, its right and up parts, on the screen with y down: the engine's
        // camera by now is the game's own again, without the roll, which is why the glare is placed as
        // it is.)
        const float ux = g_view.rot.right.y, uy = -g_view.rot.up.y, len = std::sqrt(ux * ux + uy * uy);
        if (len > 1e-3f && game::project_point({ahead.x, ahead.y, ahead.z}, &cx, &cy, &depth)) {
            const float sn = ux / len, cs = -uy / len;
            if (std::fabs(sn) > 0.005f || cs < 0) g_sprite_upright = {true, float(cx), float(cy), cs, g_glare_sign * sn};
        }
    }
    const unsigned before = g_sprites_turned;
    o_sun_glare();
    g_glare_turned += g_sprites_turned - before;
    g_sprite_upright.on = false;
}
bool install_hooks() {
    if (MH_CreateHook(reinterpret_cast<void*>(0x86BB60), reinterpret_cast<void*>(&sun_glare_detour),
                      reinterpret_cast<void**>(&o_sun_glare)) != MH_OK)
        BWLOG("ui: could not hook the sun's glare (it stays off the sun with the head tilted)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x4082F0), reinterpret_cast<void*>(&click_noise_detour),
                      reinterpret_cast<void**>(&o_click_noise)) != MH_OK)
        BWLOG("ui: could not hook the menus' tick sound (only counted)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x6F8260), reinterpret_cast<void*>(&text_read_detour),
                      reinterpret_cast<void**>(&o_text_read)) != MH_OK)
        BWLOG("ui: could not hook GScript::TextRead (the continue prompt shows on every caption left up)");
    debug_menu_install();
    if (MH_CreateHook(reinterpret_cast<void*>(kDrawTextRaw), reinterpret_cast<void*>(&draw_text_raw_detour),
                      reinterpret_cast<void**>(&o_draw_text_raw)) != MH_OK) {
        BWLOG("ui: could not hook GatheringText::DrawTextRaw");
        return false;
    }
    trace::set_vertex_filter(&screen_filter);
    if (MH_CreateHook(reinterpret_cast<void*>(kGtDrawBubble), reinterpret_cast<void*>(&gt_draw_bubble_detour),
                      reinterpret_cast<void**>(&o_gt_draw_bubble)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(kGtDrawTextA), reinterpret_cast<void*>(&gt_draw_text_a_detour),
                      reinterpret_cast<void**>(&o_gt_draw_text_a)) != MH_OK)
        BWLOG("ui: could not hook the help bubble's drawing (the signposts' panel stays flat)");
    if (MH_CreateHook(reinterpret_cast<void*>(kSetupDrawText), reinterpret_cast<void*>(&setup_draw_text_detour),
                      reinterpret_cast<void**>(&o_setup_draw_text)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(kSetupDrawTextWrap), reinterpret_cast<void*>(&setup_draw_text_wrap_detour),
                      reinterpret_cast<void**>(&o_setup_draw_text_wrap)) != MH_OK)
        BWLOG("ui: could not hook the menus' text functions (menus will be laid out as in-game 2D)");
    if (MH_CreateHook(reinterpret_cast<void*>(kDrawCreatureStats), reinterpret_cast<void*>(&draw_stats_detour),
                      reinterpret_cast<void**>(&o_draw_stats)) != MH_OK)
        BWLOG("ui: could not hook DrawCreatureStats (the creature's behaviour panel stays as drawn)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x68B420), reinterpret_cast<void*>(&gesture_point_detour),
                      reinterpret_cast<void**>(&o_gesture_point)) != MH_OK)
        BWLOG("ui: could not hook the gestures' place (their shapes aren't shown)");
    if (MH_CreateHook(reinterpret_cast<void*>(0x68B480), reinterpret_cast<void*>(&gesture_item_draw_detour),
                      reinterpret_cast<void**>(&o_gesture_item_draw)) != MH_OK)
        BWLOG("ui: could not hook the gesture icons' drawing (they stay at the screen's edge)");
    return true;
}

void set_eye_pass(int eye, const hand::EyeView* view) {
    if (g_dbg_gesture_draws > 0 && g_eye >= 0) --g_dbg_gesture_draws;
    if (eye == 0) {  // (the lines are drawn after the overlays: the last frame's are shown)
        g_help_shown = g_help_lines;
        std::stable_sort(g_help_shown.begin(), g_help_shown.end(), [](const HelpLine& a, const HelpLine& b) { return a.y < b.y; });
        g_help_lines.clear();
    }
    g_eye = eye;
    if (view && eye >= 0 && eye < 2 && g_screen.valid) {
        const Vec3 r = g_screen.centre - view->pos;
        const float Z = std::max(1e-3f, vr::dot(r, view->rot.fwd));
        g_screen.centre_px[eye][0] = view->cx + view->f * vr::dot(r, view->rot.right) / Z;
        g_screen.centre_px[eye][1] = view->cy - view->f * vr::dot(r, view->rot.up) / Z;
    }
    g_hints_drawn.clear();
    if (view) g_view = *view;
}

void begin_frame(const hand::WorldFrame& world, const vr::Pose& head, int screen_w, int screen_h) {
    ++g_frame_serial;
    Screen& sc = g_screen;
    const double now = now_s();
    const float dt = float(std::min(0.1, now - sc.last_frame));
    sc.last_frame = now;
    sc.w = std::max(1, screen_w), sc.h = std::max(1, screen_h);
    memcpy(sc.last_box, sc.box, sizeof(sc.box));
    memcpy(sc.last_text, sc.text, sizeof(sc.text));
    memcpy(g_caption_last, g_caption_text, sizeof(g_caption_text));
    g_caption_text[0] = g_caption_text[1] = 1e9f, g_caption_text[2] = g_caption_text[3] = -1e9f;
    memcpy(g_stats_last, g_stats_box, sizeof(g_stats_box));
    g_stats_box[0] = g_stats_box[1] = 1e9f, g_stats_box[2] = g_stats_box[3] = -1e9f;
    {   // (the caption's words: the same as last frame, since when)
        const bool up = g_band_seen && g_caption_hash != 0 && !g_cinema && now - g_menu_text_at > 1.0;
        // (Also when it was already there while it didn't count, as in a menu: the same words then
        // never started the wait; player, 2026-11-02: a caption up at a save's load never got the prompt.)
        if (!up) g_caption_since = -10;
        else if (g_caption_hash != g_caption_hash_last || g_caption_since < 0) g_caption_since = now;
        g_caption_hash_last = g_caption_hash, g_band_seen_last = up;
        g_caption_hash = 0, g_band_seen = false;
    }
    sc.text[0] = sc.text[1] = 1e9f, sc.text[2] = sc.text[3] = -1e9f;
    sc.box[0] = sc.box[1] = 1e9f, sc.box[2] = sc.box[3] = -1e9f;
    // In front of where the head faces when 2D shows up after a pause; afterwards it only swings
    // round once the head has turned well away, and then all the way.
    const float head_yaw = std::atan2(head.rot.fwd.x, head.rot.fwd.z) / vr::kDegToRad;
    float off = head_yaw - sc.yaw_deg;
    off -= 360.0f * std::round(off / 360.0f);
    if (g_cinema) sc.last_drawn = now;
    if (!sc.placed || now - sc.last_drawn > 1.0) {
        sc.yaw_deg = head_yaw;
        sc.placed = true;
        sc.following = false;
        sc.anchor = head.pos, sc.moving = false;
    } else {
        if (std::fabs(off) > g_set.screen_follow_deg) sc.following = true;
        if (sc.following) {
            sc.yaw_deg += off * (1.0f - std::exp(-dt * 4.0f));
            if (std::fabs(off) < 3.0f) sc.following = false;
        }
        const Vec3 d = head.pos - sc.anchor;
        const float away = vr::length(d);
        if (away > g_set.screen_follow_m) sc.moving = true;
        if (sc.moving || sc.following) {
            sc.anchor = sc.anchor + d * (1.0f - std::exp(-dt * 4.0f));
            if (away < 0.05f) sc.moving = false;
        }
    }
    const Vec3 from = g_set.screen_follow_m > 0 ? sc.anchor : head.pos;
    const vr::Basis facing = vr::basis_from_ypr(sc.yaw_deg, 0, 0);
    Vec3 centre_t = from + facing.fwd * (g_cinema ? g_set.cinema_dist_m : g_set.screen_dist_m);
    centre_t.y = from.y - g_set.screen_drop_m;
    sc.centre = world.base_pos + world.base_rot.apply(centre_t * world.scale);
    sc.right = world.base_rot.apply(facing.right);
    sc.up = world.base_rot.apply(facing.up);
    sc.normal = world.base_rot.apply(facing.fwd);
    const bool menu = now - g_menu_text_at < 0.5;
    sc.unit = (g_cinema ? g_set.cinema_width_m : menu ? std::max(g_set.screen_width_m, g_set.menu_width_m) : g_set.screen_width_m) * world.scale / float(sc.w);
    sc.valid = true;
}

bool pointer(Vec3 origin, Vec3 dir, Vec3& hit, long& x, long& y) {
    const Screen& sc = g_screen;
    if (!sc.valid) return false;
    if (!g_cinema && (!g_set.screen || sc.last_box[2] < sc.last_box[0])) return false;  // no menu panels or buttons up
    const float facing = vr::dot(dir, sc.normal);
    if (facing < 1e-4f) return false;
    const float t = vr::dot(sc.centre - origin, sc.normal) / facing;
    if (t <= 0) return false;
    hit = origin + dir * t;
    const float u = vr::dot(hit - sc.centre, sc.right) / sc.unit + sc.w * 0.5f;
    const float v = sc.h * 0.5f - vr::dot(hit - sc.centre, sc.up) / sc.unit;
    const float margin = 40.0f;
    if (g_cinema) {  // the whole frame
        if (u < 0 || u > float(sc.w) || v < 0 || v > float(sc.h)) return false;
    } else if (u < sc.last_box[0] - margin || u > sc.last_box[2] + margin || v < sc.last_box[1] - margin || v > sc.last_box[3] + margin)
        return false;
    x = std::lround(std::clamp(u, 0.0f, float(sc.w - 1)));
    y = std::lround(std::clamp(v, 0.0f, float(sc.h - 1)));
    return true;
}

void place_gesture_panel(const hand::WorldFrame& world, const vr::Pose& head) {
{   // The gesture panel: above the left hand while its palm is turned up, facing the head.
    GesturePanel& gp = g_gp;
    memcpy(gp.last_src, gp.src, sizeof(gp.src));
    gp.src[0] = gp.src[1] = 1e9f, gp.src[2] = gp.src[3] = -1e9f;
    g_items_last.swap(g_items_now);
    g_items_now.clear();
    hand::NearHand nh;
    const bool tracked = !g_cinema && hand::second_hand(nh);
    const float palm_up = tracked ? vr::normalize(nh.y).y : -1.0f;
    // The palm turned toward the eyes counts as up too: held up to be read, a palm tilted toward the
    // face was near the thresholds, and the panel went and came back with small turns of the hand
    // (playtest 2026-10-31).
    const Vec3 head_now = world.base_pos + world.base_rot.apply(head.pos * world.scale);
    const float palm_face = tracked ? vr::dot(vr::normalize(nh.y), vr::normalize(head_now - nh.pos)) : -1.0f;
    gp.palm_up = palm_up, gp.palm_face = palm_face;
    const float palm = std::max(palm_up, palm_face);
    // (Shown above 0.6, hidden below 0.25, and only once the palm has stayed so for 0.15 s: a hand
    // half turned flipped it on and off, and the panel flickered and doubled; player, 2026-10-27.)
    const bool want = tracked && palm > (gp.up ? 0.25f : 0.6f);
    static double since = 0;
    if (want == gp.up) since = now_s();
    else if (now_s() - since > 0.15) gp.up = want, since = now_s();
    if (!gp.up) gp.logged = false;
    if (tracked) {
        const float m = world.scale;
        const Vec3 head_w = world.base_pos + world.base_rot.apply(head.pos * m);
        gp.size = g_set.gesture_panel_m * m;
        // (The gestures' shapes hang under their icons, as in the flat game: room for them above the hand.)
        gp.centre = nh.pos + vr::normalize(nh.z) * (0.07f * m) + Vec3{0, 1, 0} * (0.08f * m + slot_width() * 1.4f);
        const Vec3 fwd = vr::normalize(gp.centre - head_w);
        gp.right = vr::normalize(vr::cross(Vec3{0, 1, 0}, fwd));
        gp.upv = vr::cross(fwd, gp.right);
        gp.toward = fwd * -1.0f;
    }
}
}

// A gesture's sprite about to be drawn: placed again now, with the camera that draws it and the
// panel as it is now. (Placed only when the game asked, the shapes stood far off to the side of
// their icons in the headset, more so the further the head had turned since: playtests 2026-11-03
// and 04; the placement and the drawing came at different times, with different cameras.)
void gesture_sprite_drawing(void* sprite) {
    if (!g_gp.up || g_eye < 0 || !g_set.gesture_panel) return;
    for (int i = 0; i < g_glyph_n; ++i) {
        if (g_glyph_sprite[i] != sprite) continue;
        Vec3 p, target;
        float k = 1;
        if (!g_glyph_item[i] || IsBadReadPtr(static_cast<const char*>(g_glyph_item[i]) + 0x44, 0x14)) return;
        if (glyph_place(g_glyph_item[i], g_glyph_sx[i], g_glyph_sy[i], p, k, target)) {
            float* out = static_cast<float*>(sprite);
            out[0] = p.x, out[1] = p.y, out[2] = p.z;
            g_glyph_scale[i] = k;
        }
        return;
    }
}

bool gesture_sprite_begin(void* sprite, float saved[3]) { return glyph_begin(sprite, saved); }
bool sprite_upright_begin(const float* world) {
    g_sprite_upright.on = false;
    if (!g_set.sprites_upright || g_eye < 0 || !world || IsBadReadPtr(world, 12)) return false;
    const Vec3 P{world[0], world[1], world[2]};
    const float d = vr::length(P - g_view.pos);
    if (d < 1e-3f) return false;
    if (g_dbg_far_sprites > 0 && d > 600.0f) {  // (debug, ui far_sprites N: the sun's place, as a sprite?)
        --g_dbg_far_sprites;
        const game::LHMatrix& w = game::world_to_camera();
        const Vec3 a0{w.m[0][0], w.m[1][0], w.m[2][0]}, a1{w.m[0][1], w.m[1][1], w.m[2][1]}, a2{w.m[0][2], w.m[1][2], w.m[2][2]};
        const Vec3 cam = (a0 * w.m[3][0] + a1 * w.m[3][1] + a2 * w.m[3][2]) * -1.0f;
        const game::LH3DCamera& gc = game::camera();
        BWLOG("ui: far sprite %p at (%.0f, %.0f, %.0f), %.0f from the eye (%.1f, %.1f, %.1f); engine camera (%.1f, %.1f, %.1f); game camera (%.1f, %.1f, %.1f); size %.1f",
              world, P.x, P.y, P.z, d, g_view.pos.x, g_view.pos.y, g_view.pos.z, cam.x, cam.y, cam.z, gc.pos.x, gc.pos.y, gc.pos.z, world[3]);
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float depth = 0;
    const float e = 0.02f * d;
    if (!game::project_point({P.x, P.y, P.z}, &x0, &y0, &depth) || !game::project_point({P.x, P.y + e, P.z}, &x1, &y1, &depth)) return false;
    // (Measured in whole pixels: a step of 2% of the distance is tens of them.)
    const float ux = float(x1 - x0), uy = float(y1 - y0), len = std::sqrt(ux * ux + uy * uy);
    if (len < 4.0f) return false;  // (looking straight up or down at it: no up to speak of)
    const float sn = ux / len, cs = -uy / len;  // (turns the screen's up, (0, -1), onto the world's)
    if (std::fabs(sn) < 0.01f && cs > 0) return false;  // (upright already)
    g_sprite_upright = {true, float(x0), float(y0), cs, sn};
    return true;
}
void sprite_upright_end() { g_sprite_upright.on = false; }
void gesture_sprite_end(void* sprite, const float saved[3]) { glyph_end(sprite, saved); }
float gesture_sprite_scale(const void* sprite) {
    if (!g_gp.up) return 1.0f;
    for (int i = 0; i < g_glyph_n; ++i)
        if (g_glyph_sprite[i] == sprite) return g_glyph_scale[i];
    return 1.0f;
}

void set_cinema(bool on) {
    if (on != g_cinema) g_screen.placed = false;  // a different screen: in front of the head again
    g_cinema = on;
}

bool screen_corners(Vec3 out[4]) {
    const Screen& sc = g_screen;
    if (!sc.valid) return false;
    const Vec3 half_w = sc.right * (sc.w * 0.5f * sc.unit), half_h = sc.up * (sc.h * 0.5f * sc.unit);
    out[0] = sc.centre - half_w + half_h, out[1] = sc.centre + half_w + half_h;
    out[2] = sc.centre + half_w - half_h, out[3] = sc.centre - half_w - half_h;
    return true;
}

bool hint(const wchar_t* text, int group, void (*draw_original)(void*), void* ctx) {
    if (g_eye < 0) return false;                       // not in VR: the game's own
    if (text && group != 2) {  // (for the log: each other text once)
        static std::vector<std::wstring> seen;
        if (seen.size() < 60 && std::find(seen.begin(), seen.end(), std::wstring(text)) == seen.end()) {
            seen.push_back(text);
            char narrow[64] = {};
            for (int i = 0; i < 63 && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
            BWLOG("ui: hint (group %d) '%s' not shown", group, narrow);
        }
    }
    // Group 2: the hand's action ("Pick Up"); 0: the line the game shows under it about the thing
    // under the hand ("Wood Required: 7", "Building Completed: 42%", "Population: 76/60", a
    // miracle's name), all left out before (player, 2026-10-30: the scaffolds' wood needed not
    // seen); 1: the mouse camera's moves, meaning nothing here.
    const bool info = group == 0 && g_set.info_hints;
    if (!g_set.hand_hints || (group != 2 && !info) || !text) return true;
    static ULONGLONG learn_tried = 0;
    static bool learn_failed_logged = false;
    if (!g_style.font && group == 2 && GetTickCount64() - learn_tried > 1000) {
        // Once: let the game draw it, to learn the font and the outline passes (the draws
        // themselves are swallowed; the mouse picture shows for this one frame). (Some hints draw
        // no text, as in a creature fight: then tried again at most once a second.)
        learn_tried = GetTickCount64();
        g_learning = true;
        draw_original(ctx);
        g_learning = false;
        if (g_style.font || !learn_failed_logged)
            BWLOG("ui: hint style learned: font %p, size %.1f, %zu passes", g_style.font, g_style.size, g_style.passes.size());
        learn_failed_logged |= !g_style.font;
    }
    const bool hidden = !g_set.show_hidden && std::find(g_set.hidden.begin(), g_set.hidden.end(), text) != g_set.hidden.end();
    // How long the game has been asking for this word without a break.
    struct Asked {
        std::wstring word;
        double first, last;
    };
    static std::vector<Asked> asked;
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const double now = double(t.QuadPart) / double(freq.QuadPart);
    asked.erase(std::remove_if(asked.begin(), asked.end(), [&](const Asked& a) { return now - a.last > 0.25; }), asked.end());
    // (Asked for steadily: the same words, whatever their numbers; "42%" turning "46%" is the same line.)
    std::wstring key;
    for (const wchar_t* c = text; *c; ++c)
        if (!iswdigit(*c)) key += *c;
    auto it = std::find_if(asked.begin(), asked.end(), [&](const Asked& a) { return a.word == key; });
    if (it == asked.end()) {
        it = asked.insert(asked.end(), {key, now, now});
        char narrow[48] = {};
        for (int i = 0; i < 47 && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
        BWLOG("ui: hint '%s' asked for%s", narrow, hidden ? " (hidden)" : "");
    }
    it->last = now;
    if (text == L"Pick Up") g_pick_up_asked = now;
    const hand::HintControl control = info ? hand::HintControl::kNone : hand::hint_control(text);  // (an info line is no action)
    const float delay = control == hand::HintControl::kTrigger || control == hand::HintControl::kDoubleTrigger ? std::min(g_set.tap_delay, g_set.hint_delay) : g_set.hint_delay;
    const float alpha = g_set.hint_fade > 0 ? float((now - it->first - delay) / g_set.hint_fade) : 1.0f;
    if (alpha > 0 && !hidden) {
        std::wstring label = text;
        if (g_set.hint_controls) {
            switch (control) {
            case hand::HintControl::kTrigger: label += L" - trigger"; break;
            case hand::HintControl::kDoubleTrigger: label += L" - double trigger"; break;
            case hand::HintControl::kGrip: label += L" - grip"; break;
            case hand::HintControl::kHoldTrigger: label += L" - hold trigger"; break;
            case hand::HintControl::kLetGo: label += L" - let go"; break;
            default: break;
            }
        }
        draw_hint(label.c_str(), alpha);
    }
    return true;
}

// The help bubble's lines. Where the player looks when it opens: 1.5 m along the gaze (they are
// usually looking down at the sign; on the virtual screen, at eye height, the lines were above the
// view in the headset). They stay there in the world, one under another as the game laid them
// out, on the game's own blue bubble.
// The game stops drawing its bubble as soon as the helper sprite by the sign leaves the view (in
// the headset, looking at the text was enough), and doesn't bring it back. So the lines stay up
// for a reading time after that (3 s and 0.05 s a letter), unless the trigger taps something (a
// tap on the sign is the game's way of closing it).
struct HelpAnchor {
    bool on = false;
    Vec3 pos, up;
    double opened = 0, game_gone = -1, closed = -10;
    std::vector<HelpLine> lines;
};
HelpAnchor g_help_anchor;
// The panel as a picture in the world: its lines drawn into a texture (rebuilt when they change) on
// a quad that stays where it opened, upright and facing the player as they were then. (The game's
// own text is flat on the view: it turned with the head, and the panel with it; player,
// 2026-10-19: these should float still in the air.)
render::TextTexture g_help_tex;
std::vector<std::wstring> g_help_tex_lines;
Vec3 g_help_right;
bool draw_help_picture(const HelpAnchor& h, float scale) {
    void* device = hand::panel_device();
    if (!device) return false;
    std::vector<std::wstring> lines;
    for (const HelpLine& l : h.lines) lines.push_back(l.text);
    if (lines != g_help_tex_lines || !g_help_tex.surface()) {
        g_help_tex_lines = lines;
        if (!g_help_tex.set(device, lines, 1, 0x1D5E76, 0xFFF3A0, 0xF4E27A, 0xC8)) {
            g_help_tex_lines.clear();
            return false;
        }
    }
    // Size: a line of the texture (44 px) is a line of the panel (help_height_m x 1.7).
    const float m_per_px = g_set.help_height_m * 1.7f / 44.0f * scale;
    const float w = g_help_tex.width() * m_per_px, ht = g_help_tex.height() * m_per_px;
    const Vec3 top = h.pos + h.up * (55.0f * m_per_px);  // (the title's middle at the panel's point)
    const Vec3 corner[4] = {top - g_help_right * (w * 0.5f), top + g_help_right * (w * 0.5f), top + g_help_right * (w * 0.5f) - h.up * ht,
                            top - g_help_right * (w * 0.5f) - h.up * ht};
    float q[4][3];
    const float near_clip = game::info_transform().near_clip;
    for (int i = 0; i < 4; ++i) {
        const Vec3 r = corner[i] - g_view.pos;
        const float Z = vr::dot(r, g_view.rot.fwd);
        if (Z < near_clip * 1.2f) return true;  // (a corner beside or behind the eye: not drawn this frame)
        q[i][0] = g_view.cx + g_view.f * vr::dot(r, g_view.rot.right) / Z;
        q[i][1] = g_view.cy - g_view.f * vr::dot(r, g_view.rot.up) / Z;
        q[i][2] = near_clip / Z;  // (the engine's rhw)
    }
    render::draw_textured_quad(device, g_help_tex.surface(), q, g_help_tex.u1(), g_help_tex.v1(), true);
    return true;
}

void draw_help_panel() {
    const double now = now_s();
    const bool game_draws = !g_help_shown.empty() && now - g_help_at < 0.3;
    HelpAnchor& h = g_help_anchor;
    if (h.on && g_eye == 0) {
        if (game_draws) {
            h.game_gone = -1, h.lines = g_help_shown;
        } else if (h.game_gone < 0) {
            h.game_gone = now;
        }
        size_t letters = 0;
        for (const HelpLine& l : h.lines) letters += l.text.size();
        const bool read = h.game_gone >= 0 && now - h.game_gone > 3.0 + 0.05 * double(letters);
        const bool tapped = hand::last_tap_at() > h.opened + 0.3;
        if (read || (tapped && !game_draws)) {
            h.on = false, h.closed = now;
            BWLOG("ui: help panel closed (%s, %.1f s open)", read ? "read" : "tapped", now - h.opened);
        }
    }
    // (Opened by a bubble the game starts afresh: not by the end of one a tap has just closed.)
    if (!h.on && game_draws && g_eye == 0 && g_screen.valid && g_help_started > h.closed) {
        const Screen& sc = g_screen;
        const float scale = sc.unit * float(sc.w) / std::max(0.1f, g_set.screen_width_m);
        const float line = g_set.help_height_m * 1.7f;
        const Vec3 fwd = g_view.rot.fwd, world_up{0, 1, 0};
        Vec3 up = world_up - fwd * vr::dot(world_up, fwd);  // (upright text, whatever the head's pitch)
        up = vr::length(up) > 0.1f ? vr::normalize(up) : g_view.rot.up;
        const float block = (line * float(g_help_shown.size()) + line * 0.3f) * scale;
        h = {true, g_view.pos + fwd * (g_set.help_dist_m * scale) + up * (block * 0.5f + g_set.help_above_m * scale), up, now, -1, h.closed, g_help_shown};
        // (facing the player: right is level, across the line of sight)
        const Vec3 across = vr::cross(up, fwd);
        g_help_right = vr::length(across) > 1e-3f ? vr::normalize(across) : g_view.rot.right;
        BWLOG("ui: help panel: %zu lines, placed %.1f m along the gaze at (%.1f, %.1f, %.1f)", h.lines.size(), g_set.help_dist_m, h.pos.x,
              h.pos.y, h.pos.z);
    }
    if (!h.on || h.lines.empty() || !g_screen.valid) return;
    const Screen& sc = g_screen;
    const float scale = sc.unit * float(sc.w) / std::max(0.1f, g_set.screen_width_m);
    if (draw_help_picture(h, scale)) return;
    const float height = g_set.help_height_m;
    // (Without the picture:) laid out in this eye's pixels round the panel's point: the game's text is flat on the view
    // (it turns with the head's roll) and never smaller than hint_min_px, so a panel fixed in the
    // world tilted against it and, from far off, the text spilled out of it (player, 2026-10-19).
    // The panel and the lines are the same picture now, at the point's depth.
    const Vec3 r = h.pos - g_view.pos;
    const float Z = vr::dot(r, g_view.rot.fwd);
    if (Z < game::info_transform().near_clip * 1.2f) return;
    auto px = [&](float height_m) { return std::round(std::clamp(g_view.f * height_m * scale / Z, g_set.hint_min_px, g_set.hint_max_px)); };
    const float first = px(height * 1.2f), size = px(height), line = size * 1.7f;
    const float k = Z / g_view.f;  // (world units per pixel at the panel's depth)
    const Vec3 right = g_view.rot.right * k, up = g_view.rot.up * k;
    // A translucent blue panel behind them, as the game's bubble was (its colour, more opaque).
    {
        float width = 0;  // (draw_text_at's estimate of a line's width: half its height a letter)
        for (size_t i = 0; i < h.lines.size(); ++i) width = std::max(width, 0.5f * (i == 0 ? first : size) * float(h.lines[i].text.size()));
        const float margin = size * 1.2f;
        const float top = line * 0.6f + margin, bottom = line * (0.3f + float(h.lines.size() - 1)) + line * 0.6f + margin;
        const float w = width * 0.5f + margin;
        const Vec3 q[4] = {h.pos + up * top - right * w, h.pos + up * top + right * w, h.pos - up * bottom + right * w, h.pos - up * bottom - right * w};
        hand::draw_panel_quad(g_view, q, 0xA01D5E76u);
    }
    Vec3 p = h.pos;
    for (size_t i = 0; i < h.lines.size(); ++i) {
        draw_text_at(h.lines[i].text.c_str(), p, scale, i == 0 ? height * 1.2f : height, 1.0f);
        p = p - up * (line * (i == 0 ? 1.3f : 1.0f));
    }
}

void draw_overlays() {
    if (g_eye < 0) return;
    if (g_help_panel) draw_help_panel();
    // How much the hand carries ("160 food"), with the hand's words: the flat game's count by the
    // cursor isn't there in VR (player, 2026-10-30).
    {
        // (Above the top of what it carries, larger than the hand's words: by the palm it was behind
        // the bundle of wood and hard to see; player, 2026-10-30.)
        const wchar_t* kind = nullptr;
        int amount = 0;
        Vec3 top;
        if (g_set.held_count && hand::held_resource(kind, amount, &top)) {
            wchar_t label[32];
            swprintf(label, 32, L"%d %ls", amount, kind);
            const float scale = hand::rig_scale();
            const float unit = std::max(scale, vr::length(top - g_view.pos) / 0.6f);  // (far out on the laser: still readable)
            const Vec3 at = top + Vec3{0, 0.06f * unit, 0}, moved = before_hand(at);
            const float k = vr::length(moved - g_view.pos) / std::max(1e-3f, vr::length(at - g_view.pos));
            draw_text_at(label, moved, unit * k, g_set.hint_height_m * 1.4f, 1.0f);
        }
    }
    // "trigger - continue" under a caption left up (see g_caption_hash).
    const Screen& sc = g_screen;
    const bool left_up = g_continue_after_s > 0 && g_band_seen_last && g_caption_since > 0 && now_s() - g_caption_since > g_continue_after_s &&
                         sc.valid && sc.last_text[2] > sc.last_text[0];
    g_continue_up = left_up && help_line_clickable() && script_waits_for_read();
    if (left_up) {
        static double logged_mode_since = -1;
        if (logged_mode_since != g_caption_since) {
            logged_mode_since = g_caption_since;
            BWLOG("ui: a caption up %.0f s: the game's line mode %d, a script %s for it to be read (%s)", g_continue_after_s, help_line_mode(),
                  script_waits_for_read() ? "waits" : "doesn't wait", g_continue_up ? "the prompt" : "no prompt");
        }
    }
    if (g_continue_up) g_continue_seen_at = now_s();
    if (g_continue_up) {
        static double logged_since = -1;
        if (logged_since != g_caption_since) {
            logged_since = g_caption_since;
            BWLOG("ui: a caption has stayed up %.0f s outside a scene: the continue prompt is shown", g_continue_after_s);
        }
        // (Drawn with the caption's words: see draw_text_raw_detour.)
    }
    hand::WristView wv;
    if (hand::wrist(wv))
        for (const auto& b : wv.buttons) draw_text_at(b.text.c_str(), b.pos, wv.unit, b.lit ? 0.016f : 0.013f, b.lit ? 1.0f : 0.8f);
    hand::TuningView tv;
    if (!hand::tuning(tv)) return;
    // The panel above the other hand: its title, then a window of lines around the selected one.
    const float line = 0.022f * tv.unit;
    const int shown = 7, n = int(tv.lines.size()) - 1;
    const int first = std::clamp(tv.selected - 1 - shown / 2, 0, std::max(0, n - shown));
    std::vector<const std::wstring*> rows = {&tv.lines[0]};
    for (int i = first; i < std::min(n, first + shown); ++i) rows.push_back(&tv.lines[size_t(i) + 1]);
    for (size_t i = 0; i < rows.size(); ++i)
        draw_text_at(rows[i]->c_str(), tv.anchor + Vec3{0, line * float(rows.size() - i), 0}, tv.unit, i == 0 ? 0.011f : 0.014f, 1.0f);
}

bool continue_prompt_up() { return g_continue_up && now_s() - g_continue_seen_at < 0.2; }

bool command(const std::string& k, std::istringstream& in) {
    if (k != "ui") return false;
    std::string what;
    in >> what;
    if (what == "textlog") in >> g_textlog;
    else if (what == "help_panel") in >> g_help_panel;
    else if (what == "help_log") in >> g_help_log;
    else if (what == "help_size") in >> g_set.help_height_m >> g_set.help_above_m >> g_set.help_dist_m;
    else if (what == "hand_hints") in >> g_set.hand_hints;
    else if (what == "hint_size") in >> g_set.hint_height_m >> g_set.hint_above_m;
    else if (what == "spinner") in >> g_set.spinner_at_hand >> g_set.spinner_scale;
    else if (what == "world_text") in >> g_set.world_text_scale;
    else if (what == "label_before_hand") in >> g_set.label_before_hand_m;
    else if (what == "screen_follow") in >> g_set.screen_follow_m;
    else if (what == "menu_width") in >> g_set.menu_width_m;
    else if (what == "screen_where")  // debug: where the screen panel's centre lands in each eye
        BWLOG("ui: the screen panel's centre at (%.1f, %.1f) left, (%.1f, %.1f) right; 2D drawn %.2f s ago", g_screen.centre_px[0][0],
              g_screen.centre_px[0][1], g_screen.centre_px[1][0], g_screen.centre_px[1][1], now_s() - g_screen.last_drawn);
    else if (what == "hint_depth") in >> g_set.hint_depth;
    else if (what == "show_hidden") in >> g_set.show_hidden;
    else if (what == "hint_controls") in >> g_set.hint_controls;
    else if (what == "screen") in >> g_set.screen;
    else if (what == "gesture_panel") in >> g_set.gesture_panel;
    else if (what == "gesture_log") in >> g_dbg_gesture_draws;
    else if (what == "gesture_match") in >> g_set.gesture_match;
    else if (what == "gesture_remap") in >> g_set.gesture_remap;
    else if (what == "sprites_upright") in >> g_set.sprites_upright;
    else if (what == "far_sprites") in >> g_dbg_far_sprites;
    else if (what == "glare_fix") in >> g_glare_fix;
    else if (what == "glare_stats") BWLOG("ui: sun glare: %u calls in eye passes, %u outside; %u draws turned", g_glare_calls[0], g_glare_calls[1], g_glare_turned);
    else if (what == "glare_sign") in >> g_glare_sign;
    else if (what == "sprites_turned") BWLOG("ui: sprites turned upright: %u draws", g_sprites_turned);
    else if (what == "gesture_remap_stats") BWLOG("ui: gesture shapes laid on the panel: %u draws, %u not (no projection); %u found by their place", g_glyph_remapped, g_glyph_failed, g_glyph_by_place);
    else if (what == "stats") in >> g_set.stats_scale >> g_set.stats_toward_middle;
    else if (what == "menu_dump") {
        // debug: the open dialog (SetupBox::CurrentActiveBox, 0xC4CD20) and its controls (two lists,
        // +0x6C and +0x68; each control's id at +0x18, the next at +0x230), with their classes.
        const auto rtti = [](const void* o) -> const char* {
            const char* const* vt = o && !IsBadReadPtr(o, 4) ? *static_cast<const char* const* const*>(o) : nullptr;
            if (!vt || IsBadReadPtr(vt - 1, 4)) return "?";
            const char* col = vt[-1];
            const char* td = col && !IsBadReadPtr(col + 0xC, 4) ? *reinterpret_cast<const char* const*>(col + 0xC) : nullptr;
            return td && !IsBadReadPtr(td + 8, 4) ? td + 8 : "?";
        };
        const char* box = *reinterpret_cast<const char* const*>(0xC4CD20);
        BWLOG("ui: menu: the active box %p %s", box, box ? rtti(box) : "-");
        if (box && !IsBadReadPtr(box, 0x80)) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(box);
            BWLOG("ui: menu: box words %08X %08X %08X %08X %08X %08X %08X %08X | +0x60 %08X %08X %08X %08X %08X %08X %08X %08X", w[0], w[1], w[2], w[3],
                  w[4], w[5], w[6], w[7], w[24], w[25], w[26], w[27], w[28], w[29], w[30], w[31]);
            for (int off = 0; off < 0x400; off += 4) {  // (code pointers in it: its callback)
                if (IsBadReadPtr(box + off, 4)) break;
                const uint32_t v = *reinterpret_cast<const uint32_t*>(box + off);
                if (v >= 0x401000 && v < 0x8C0000) BWLOG("ui: menu: box +0x%X -> %08X", off, v);
            }
            for (int list = 0; list < 2; ++list) {
                int left = 64;
                for (const char* c = *reinterpret_cast<const char* const*>(box + (list ? 0x68 : 0x6C)); c && !IsBadReadPtr(c, 0x234) && left-- > 0;
                     c = *reinterpret_cast<const char* const*>(c + 0x230)) {
                    const uint32_t* cw = reinterpret_cast<const uint32_t*>(c);
                    // (Its text: inline from +0x24.)
                    char label[48] = {};
                    const wchar_t* t = reinterpret_cast<const wchar_t*>(c + 0x24);
                    for (int k = 0; k < 47 && t[k] >= 32 && t[k] < 127; ++k) label[k] = char(t[k]);
                    BWLOG("ui: menu: list %d control %p %s id %d | %08X %08X %08X %08X %08X %08X %08X %08X '%s'", list, c, rtti(c), int(cw[6]), cw[1],
                          cw[2], cw[3], cw[4], cw[5], cw[7], cw[8], cw[9], label);
                }
            }
        }
    }
    else if (what == "stats_box")
        BWLOG("ui: the creature's behaviour panel last drawn at (%.0f, %.0f)-(%.0f, %.0f) layout px", g_stats_last[0], g_stats_last[1], g_stats_last[2],
              g_stats_last[3]);
    else if (what == "gesture_state")
        BWLOG("ui: gesture panel %s, palm up %.2f, toward the head %.2f, %zu icons; shapes placed %u in eye passes, %u outside", g_gp.up ? "up" : "down",
              g_gp.palm_up, g_gp.palm_face, g_items_last.size(), g_gesture_points[0], g_gesture_points[1]);
    else if (what == "screen_size") in >> g_set.screen_dist_m >> g_set.screen_width_m >> g_set.screen_drop_m;
    else if (what == "screeninfo")
        BWLOG("ui: screen centre in the eyes L (%.1f, %.1f) R (%.1f, %.1f)", g_screen.centre_px[0][0], g_screen.centre_px[0][1],
              g_screen.centre_px[1][0], g_screen.centre_px[1][1]),
        BWLOG("ui: screen %dx%d centre (%.1f, %.1f, %.1f) yaw %.1f unit %.4f box (%.0f, %.0f)-(%.0f, %.0f) drawn %.2f s ago", g_screen.w,
              g_screen.h, g_screen.centre.x, g_screen.centre.y, g_screen.centre.z, g_screen.yaw_deg, g_screen.unit, g_screen.last_box[0],
              g_screen.last_box[1], g_screen.last_box[2], g_screen.last_box[3], now_s() - g_screen.last_drawn);
    else if (what == "hint_delay") in >> g_set.hint_delay >> g_set.hint_fade;
    else if (what == "click_noises") BWLOG("ui: the menus' tick sound played %u times", g_click_noises);
    else if (what == "continue_prompt") in >> g_continue_after_s;
    else if (what == "info_hints") in >> g_set.info_hints;
    else if (what == "caption_scale") in >> g_set.caption_scale >> g_set.caption_view_deg;
    else if (what == "status_scale") in >> g_set.status_scale;
    else if (what == "captioninfo")
        BWLOG("ui: caption: band %d, hash %016llX, the same for %.1f s, text (%.0f, %.0f)-(%.0f, %.0f); its words (%.0f, %.0f)-(%.0f, %.0f) "
              "drawn %.2fx about y %.0f",
              g_band_seen_last, (unsigned long long)g_caption_hash_last, g_caption_since > 0 ? now_s() - g_caption_since : -1.0,
              g_screen.last_text[0], g_screen.last_text[1], g_screen.last_text[2], g_screen.last_text[3], g_caption_last[0], g_caption_last[1],
              g_caption_last[2], g_caption_last[3], caption_scale(), g_caption_mid_y);
    else if (debug_menu_command(what, in)) return true;
    else return save_name_command(what, in);
    return true;
}

}  // namespace bw::ui
