// kol_perspective.cpp — perspective on the battle map, drawn at present.
//
// The farther a thing is, the smaller it is drawn, and everything upright
// stays upright: the picture a camera would take looking level at the map
// rather than tilted down onto it. A tilted camera's warp (the whole picture
// squeezed toward its top edge) was tried first, on 2026-09-25, and rejected:
// every wall leaned in, and the HUD, cut out and refilled, left smudges.
//
// So the scale is not a function of the screen row but of the WORLD: each
// pixel's place on the map comes from kol_focus.cpp's scene rebuild (the
// height grid, the objects, the camera), and a place at depth D (X + Z, world
// units, larger is nearer) is drawn at
//
//   s = K / (K + Dref - D)
//
// times its size, toward the column at the middle of the screen. Heights
// scale by s about Yref, depth offsets from Dref by s, then the game's own
// projection applies: a projective map of the world, so straight lines stay
// straight, and since s does not change along a vertical line, verticals stay
// vertical. Dref is the depth under the middle of the screen, so the map
// there keeps its size and nothing moves under the cursor as the camera pans.
//
// Drawing: gbarecomp draws the frame WITHOUT the HUD (BG2|BG3, the scene
// frame) through a mesh: one vertex per pixel corner, placed by the frontmost
// of the up to four pixels around it, so a near unit keeps its shape and the
// background stretches up to its edge instead of leaving a gap. Runs of
// pixels whose corners move linearly (a cell's top across a row) share one
// quad. The sky is not in the mesh: it is drawn as it is, in the backdrop,
// and where the map moves away the backdrop shows that row's sky colour.
// The HUD and interface sprites are then drawn flat from the real frame.

#include "kol_perspective.h"

#include "kol_env.h"
#include "kol_hash.h"
#include "kol_focus.h"
#include "kol_rain.h"
#include "kol_snow.h"
#include "kol_map_settings.h"
#include "kol_tile_edit.h"
#include "kol_settings.h"
#include "runtime.h"
#include "runtime_arm.h"          // g_runtime_fn_entry_hook
#include "gba_ppu.h"

#include <algorithm>
#include <tuple>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <functional>

namespace {

// The row pool (kol_workers.h).
using kol::chunk_rows;
using kol::fine_rows;
using kol::row_pool;

// A pixel's place on the ground, X and Z, from its depth (ScenePixels::xz,
// X + Z) and its column (x = (X - Z) / 2 + kx + ox; why ox too: see
// water_effect).
inline void world_xz(const kol::ScenePixels& sp, int x, std::size_t k, float* X, float* Z) {
    const float u = static_cast<float>(sp.xz[k]), c2 = 2.0f * static_cast<float>(x - sp.kx - sp.ox);
    *X = 0.5f * (u + c2);
    *Z = 0.5f * (u - c2);
}
// The sky: the backdrop by the scene's own layer code (5), else (no codes)
// a pixel the terrain pass left unplaced (see ice_effect on why not that
// first).
inline bool sky_px(const kol::ScenePixels& sp, std::size_t q) {
    if (sp.scene_codes) return (sp.scene_codes[q] & 0x0Fu) == kol::kLayerBackdrop;
    return sp.kind && sp.kind[q] == kol::kSceneNone;
}
// A sprite on top (layer code 4) whose no-sprite colour is known
// (ScenePixels::under_rgb): the game's own colour there, without it.
inline bool obj_over(const kol::ScenePixels& sp, std::size_t k) {
    return sp.under_rgb && sp.scene_codes && (sp.scene_codes[k] & 0x0Fu) == kol::kLayerObj;
}
// The clock the effects run on: seconds (60 frames) into the frame count's
// day.
inline float scene_secs(const kol::ScenePixels& sp) { return static_cast<float>(sp.serial % (60ul * 60ul * 24ul)) / 60.0f; }
// Each texel of block `bi`'s atlas square that lies over the scene (w x h),
// row by row: f(x, y, px, py, k, a), (x, y) in the square, (px, py) and k
// the scene's pixel under it, `a` the atlas texel. With `own_only`, only
// those whose scene pixel is lifted onto this block (ScenePixels::lift, the
// block's index + 1).
template <class F>
void for_card_texels(const kol::ScenePixels& sp, int w, int h, int bi, bool own_only, F&& f) {
    const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
    const auto [ax0, ay0] = kol::slot_origin(b.slot, sp.atlas_per_row);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const int px = b.sx + x, py = b.sy + y;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            if (own_only && sp.lift[k] != bi + 1) continue;
            const std::size_t a = static_cast<std::size_t>(ay0 + y) * sp.atlas_w + ax0 + x;
            f(x, y, px, py, k, a);
        }
}

// Strength: the depth, in world units, at which the scale has come down to
// one half behind the reference. 3600 draws terrain 17 rows (one screen at
// 480x270) behind the middle at 0.87, and the bottom of the screen at 1.18 --
// between the 0.9 mockups of the ruins (K 2718) and the snowfield (K 4428).
float strength() {
    static const float k = kol::env_num_or("KOL_PERSP_K", 3600.0f);
    return k;
}

// The combat camera. While the game plays an attack on the map, eased in
// and out over 14 frames:
// - the perspective deepens (K down to KOL_PERSP_STRIKE_K of itself,
//   default 0.4);
// - the camera drops (KOL_PERSP_STRIKE_PITCH, default 0.75): depth takes
//   that much of its rows about the middle of the view, heights all of
//   theirs, so the ground flattens while walls and units stand as tall;
// - the view zooms in about its centre, by KOL_PERSP_STRIKE_ZOOM (1.45) or
//   by what the two above need, whichever is more: the combat camera never
//   needs more overscan than the camera at rest (the user's condition). At
//   0.4 and 0.75 that is 1.63 at 480x270; eased alike, the need is largest
//   at rest (44 px a side, 32 rows on top) and falls from there.
//
// The attack is the guest function 0x08088912: called once a frame, every
// frame from the strike's start until its "LP earned" message is up (299
// frames on states/BV_attack_confirm.state), and never otherwise -- not at
// rest, in menus, on the enemy phase banner, or in a spell
// (states/BW_spell_confirm.state; a spell's animation is a scene of its own).
// Watched through the function-entry hook, so the guest is not changed.
constexpr std::uint32_t kStrikeFn = 0x08088912u;
// A spell takes the same camera (the user, 2026-10-15). 0x080AC608, in the
// spell-effect code, is called once a frame from the effect's start to its
// end and never otherwise: every frame of Summon Darkness (786-1274 after
// the cast state's load), Fireball, Ice Javelin and Air Blade, and a breath
// (which also strikes); never at rest, in menus or in an attack
// (states/BV_attack_confirm.state, 299 strike frames). Found with a
// per-frame function-entry record. KOL_PERSP_NO_SPELL_CAMERA=1: attacks only.
constexpr std::uint32_t kSpellFn = 0x080AC608u;
// The spell before its effect: 0x080AC5C8 is called once a frame while the
// caster charges the spell's orb (Crag Crush 69 frames, then kSpellFn takes
// over; Ice Javelin too), never at rest, in menus or in an attack
// (states/BV_attack_confirm.state). With kSpellFn, "a spell is running"
// (perspective_spell_running: a spell's light, kol_focus spell_lights).
// Found by the function entries of the charges of states/IF and GD against
// the frames before them and against BK and GN.
constexpr std::uint32_t kSpellChargeFn = 0x080AC5C8u;
constexpr float kStrikeStep = 1.0f / 14.0f;
constexpr int kStrikeHold = 8;   // frames a strike stays on after its last call
void (*g_prev_entry_hook)(std::uint32_t) = nullptr;
bool g_strike_seen = false;
bool g_spell_seen = false;   // the camera is on for a spell (kSpellFn)
int g_spell_left = 0;        // frames the camera may still be a spell's
bool g_charge_seen = false;  // a spell's charge (kSpellChargeFn) this frame
int g_charge_left = 0;       // frames a spell may still be charging
bool g_strike_active = false;
int g_strike_hold = 0;
float g_strike_t = 0.0f;
void strike_entry(std::uint32_t pc) {
    if (g_prev_entry_hook) g_prev_entry_hook(pc);
    if (pc == kStrikeFn) g_strike_seen = true;
    static const bool spells = std::getenv("KOL_PERSP_NO_SPELL_CAMERA") == nullptr;
    if (pc == kSpellFn && spells) g_strike_seen = g_spell_seen = true;
    if (pc == kSpellChargeFn) g_charge_seen = true;
}
// A spell's camera is milder (the user, 2026-10-15): its effect stands over
// the target as a card (the floating circle of Summon Darkness, its column),
// and at the attack's 0.4 / 0.75 / 1.45 the circle went off the top of the
// view. KOL_PERSP_SPELL_K, _PITCH, _ZOOM.
float strike_k_ratio() {
    static const float v = std::clamp(kol::env_num_or("KOL_PERSP_STRIKE_K", 0.4f), 0.2f, 1.0f);
    static const float sv = std::clamp(kol::env_num_or("KOL_PERSP_SPELL_K", 0.7f), 0.2f, 1.0f);
    return g_spell_left > 0 ? sv : v;
}
float strike_zoom() {
    static const float v = std::clamp(kol::env_num_or("KOL_PERSP_STRIKE_ZOOM", 1.45f), 1.0f, 2.5f);
    static const float sv = std::clamp(kol::env_num_or("KOL_PERSP_SPELL_ZOOM", 1.2f), 1.0f, 2.5f);
    return g_spell_left > 0 ? sv : v;
}
float strike_pitch() {
    static const float v = std::clamp(kol::env_num_or("KOL_PERSP_STRIKE_PITCH", 0.75f), 0.4f, 1.0f);
    static const float sv = std::clamp(kol::env_num_or("KOL_PERSP_SPELL_PITCH", 0.9f), 0.4f, 1.0f);
    return g_spell_left > 0 ? sv : v;
}
float ease(float t) { return t * t * (3.0f - 2.0f * t); }
// The right stick's look (see perspective_strike_tick): x leans the camera
// (the perspective's centre shifts that way, by up to kLookShift of half the
// view), y drops it (down: a deeper perspective and flatter ground, like the
// combat camera's) or raises it (up). Eased toward the stick, and back to
// exactly nothing when it is let go.
float g_look_x = 0.0f, g_look_y = 0.0f;     // the look drawn
float g_look_tx = 0.0f, g_look_ty = 0.0f;   // where the stick is
constexpr float kLookShift = 1.2f;
constexpr float kLookEase = 0.2f;           // of the way to the stick, a frame
// The stick at full tilt, in looks (the user, 2026-10-05: twice as far up and
// down, half again sideways; the zoom grows to cover the lean).
constexpr float kLookRangeX = 1.5f;
constexpr float kLookRangeY = 2.0f;
constexpr float kSmallLook = 0.3f;   // the stick's reach on a `small_look` map
constexpr float kLookUpMore = 1.3f;  // the stick's reach upward in a battle, of kLookRangeY

// The camera for the combat camera's t and the look: K, the pitch f (depth's
// share of its rows), the zoom Z about the view's centre, and the shift dx of
// the perspective's centre (view px). The zoom is the combat camera's, or
// the least that keeps the overscan within the camera's at rest (the user's
// condition), whichever is more. With the perspective's centre dx off the
// view's C = half_w, the view's top row needs C (1/(Z s) - 1) + |dx| (1/s -
// 1) beyond a side, against C (1/s0 - 1) at rest; the top shows the row u
// where Z f u s = H: u = H K / (Z f K - 4H), no farther than at rest when
// Z >= (1 + 4H/K - 4H/K0) / f. At rest, exactly K0, 1, 1, 0.
struct Cam { float K, f, Z, dx; };
// The zoom is held to 2.5: past it the view would show beyond the overscan.
// The stick's full range with the combat camera on top asks up to ~4 (low
// and leaning in an attack), so there the look is drawn back, the least that
// keeps the zoom within it. Every look the stick reached before the range
// grew (2026-10-05) stays under 2.5 and is unchanged.
constexpr float kMaxZoom = 2.5f;
Cam camera(float t, float lx, float ly, float half_w, float half_h) {
    const float e = ease(t);
    const float K0 = strength();
    const float strike = 1.0f + (strike_zoom() - 1.0f) * e;
    auto make = [&](float lx, float ly) {
        Cam c;
        c.K = K0 * (1.0f + (strike_k_ratio() - 1.0f) * e) * std::exp(-0.4f * ly);
        c.f = (1.0f + (strike_pitch() - 1.0f) * e) * (1.0f - 0.25f * ly);
        c.dx = lx * kLookShift * half_w;
        const float s = c.K / (c.K + 4.0f * half_h), s0 = K0 / (K0 + 4.0f * half_h);
        const float room = half_w / s0 - std::abs(c.dx) * (1.0f / s - 1.0f);
        const float side = room > 0.0f ? (half_w / s) / room : 1.0e9f;
        const float top = (1.0f + 4.0f * half_h / c.K - 4.0f * half_h / K0) / c.f;
        c.Z = std::max(strike, std::max(side, top));
        return c;
    };
    Cam c = make(lx, ly);
    if (c.Z > kMaxZoom && (lx != 0.0f || ly != 0.0f)) {
        float lo = 0.0f, hi = 1.0f;   // the share of the look that fits
        for (int i = 0; i < 12; ++i) {
            const float mid = 0.5f * (lo + hi);
            (make(lx * mid, ly * mid).Z > kMaxZoom ? hi : lo) = mid;
        }
        c = make(lx * lo, ly * lo);
    }
    c.Z = std::clamp(c.Z, 1.0f, kMaxZoom);
    if (c.Z < 1.0f + 1e-5f) c.Z = 1.0f;   // at rest, exactly
    return c;
}
bool look_on() {
    return g_look_x != 0.0f || g_look_y != 0.0f || g_look_tx != 0.0f || g_look_ty != 0.0f;
}
// A frame composed now is drawn a frame or two later: the cameras it may be
// drawn with are the combat camera's two steps either way, with the look as
// it is and where the stick is taking it. At rest, exactly the camera's own.
int plan_cameras(float half_w, float half_h, Cam out[10]) {
    if (g_strike_t == 0.0f && !g_strike_active && !look_on()) {
        out[0] = camera(0.0f, 0.0f, 0.0f, half_w, half_h);
        return 1;
    }
    int n = 0;
    for (int i = 0; i < 5; ++i) {
        const float t = std::clamp(g_strike_t + (i - 2) * kStrikeStep, 0.0f, 1.0f);
        out[n++] = camera(t, g_look_x, g_look_y, half_w, half_h);
        out[n++] = camera(t, g_look_tx, g_look_ty, half_w, half_h);
    }
    return n;
}

// A per-pixel step over the scene on the row pool, by rows (each pixel's
// steps read and write that pixel alone: the same result in any split).
// The light's steps ran on the worker alone while its helpers idled (asked
// 2026-10-26 whether the perspective could be optimised).
template <class F>
void par_px(const kol::ScenePixels& sp, std::size_t pixels, F f) {
    const int w = sp.w > 0 ? sp.w : 1;
    const int h = static_cast<int>(pixels / static_cast<std::size_t>(w));
    if (static_cast<std::size_t>(w) * h != pixels) {   // (not the scene's rows: in order, here)
        for (std::size_t k = 0; k < pixels; ++k) f(k);
        return;
    }
    row_pool().run(h, chunk_rows(), [&](int, int y0, int y1) {
        for (std::size_t k = static_cast<std::size_t>(y0) * w, e = static_cast<std::size_t>(y1) * w; k < e; ++k) f(k);
    });
}
// ... the same split, handed a run of pixels at a time (k0 to k1), for a
// step that keeps locals of its own over the run.
template <class F>
void par_runs(const kol::ScenePixels& sp, std::size_t pixels, F f) {
    const int w = sp.w > 0 ? sp.w : 1;
    const int h = static_cast<int>(pixels / static_cast<std::size_t>(w));
    if (static_cast<std::size_t>(w) * h != pixels) {   // (not the scene's rows: in order, here)
        f(std::size_t{0}, pixels);
        return;
    }
    row_pool().run(h, chunk_rows(), [&](int, int y0, int y1) {
        f(static_cast<std::size_t>(y0) * w, static_cast<std::size_t>(y1) * w);
    });
}

// This frame's places (kol::ScenePixels, plus the map's outline).
std::vector<std::int32_t> g_xz;
std::vector<std::int32_t> g_y;
std::vector<std::uint8_t> g_kind;
std::vector<std::uint8_t> g_dist;
std::vector<std::uint8_t> g_cand;
std::vector<std::uint32_t> g_queue;
std::vector<float> g_xy;
std::vector<std::uint8_t> g_alpha;
std::vector<std::uint8_t> g_fx_alpha;   // Special Effects: each atlas slot's corners' alpha, from the fill
// ... and each square's texels and corners kept for the next frame (30 a
// second), by what they were made from (see the fill).
struct FxCache { unsigned long serial = 0; std::vector<std::uint8_t> rgb, alpha; };
std::unordered_map<std::uint32_t, FxCache> g_fx_cache;   // per corner, with see-through squares only (PresentWarpMesh::alpha)
std::vector<float> g_uv;
std::vector<int> g_idx;
// Per chunk of runs, its triangles' indices (see the mesh): a buffer that
// keeps its size, written through a pointer, since the vector's push_back
// was the warp's hottest line on the Thor.
struct IdxPart { std::vector<int> buf; std::size_t n = 0; };
std::vector<IdxPart> g_idx_parts;
std::vector<std::uint8_t> g_flat;
std::vector<std::uint8_t> g_card;   // PresentWarpMesh::card (a spell's effect)
// The cursor's floating hand: its pixels (scene) and colours, and where they
// are drawn (view-sized mask and RGB).
struct FloatPx { int x, y; std::uint8_t r, g, b; };
std::vector<FloatPx> g_float_px;
// Debug > Tile Info's readout: a 3 x 5 font (rows top first, 3 bits each,
// the high bit the left column) for what it writes.
struct Glyph { char c; std::uint16_t bits; };
constexpr Glyph kGlyphs[] = {
    {' ', 0x0000},
    {'+', 0x05D0},
    {'/', 0x12A4},
    {'.', 0x0002},
    {'(', 0x2922},
    {')', 0x224A},
    {'-', 0x01C0},
    {'0', 0x7B6F},
    {'1', 0x2C97},
    {'2', 0x73E7},
    {'3', 0x73CF},
    {'4', 0x5BC9},
    {'5', 0x79CF},
    {'6', 0x79EF},
    {'7', 0x7249},
    {'8', 0x7BEF},
    {'9', 0x7BCF},
    {'A', 0x2BED},
    {'B', 0x6BAE},
    {'C', 0x3923},
    {'D', 0x6B6E},
    {'E', 0x79A7},
    {'F', 0x79A4},
    {'G', 0x396B},
    {'H', 0x5BED},
    {'I', 0x7497},
    {'J', 0x126A},
    {'K', 0x5BAD},
    {'L', 0x4927},
    {'M', 0x5FED},
    {'N', 0x6B6D},
    {'O', 0x2B6A},
    {'P', 0x6BA4},
    {'Q', 0x2B73},
    {'R', 0x6BAD},
    {'S', 0x388E},
    {'T', 0x7492},
    {'U', 0x5B6F},
    {'V', 0x5B6A},
    {'W', 0x5BFD},
    {'X', 0x5AAD},
    {'Y', 0x5A92},
    {'Z', 0x72A7},
};
std::vector<std::uint8_t> g_moved;
std::vector<std::uint8_t> g_moved_rgb;
std::vector<std::uint8_t> g_backdrop;
std::vector<int> g_row_sky;   // per row, the first sky pixel, or -1
int g_uv_w = 0;
int g_uv_h = 0;
int g_uv_atlas = 0;   // the atlas vertices the uv grid was built with
std::uint64_t g_uv_gen = 0;   // counts the grid's builds (PresentWarpMesh::uv_gen)
// Runs bucketed by depth, drawn far to near where a fold overlaps them.
// A run of a pixel row; `slot` >= 0: a run of a unit's sprite in that
// square of the atlas (row and columns then local to the square).
struct Quad { int a, b, row, slot = -1, key = -1; };   // key: a prop square's bucket
std::vector<Quad> g_quads;
// A scene pixel drawn again after the cards (ScenePixels::over_sprite): a
// Quad of the scene's (a run of one pixel) with this slot.
constexpr int kOverSpriteSlot = -4;
std::vector<std::uint8_t> g_prop_lift;   // nonzero: a pixel lifted off for a prop
std::vector<std::uint8_t> g_hole_filled;   // a prop's hole under interface, filled from the right
std::vector<int> g_quad_key;
std::vector<int> g_bucket;
std::vector<Quad> g_sorted;
// Corner rows: surface cuts (per pixel row) and the corners kept as cuts.
std::vector<std::uint8_t> g_cut;
// A corner row two surfaces share, for the strips of the one behind (see
// "Strips behind end at their own edge"): its own corner there (g_own_xy),
// the pixel that places it (g_own_px, -1 none), which side of the corner its
// strips lie (g_own_dir: +1 under, -1 over), the vertex they use (g_own_vtx,
// -1 the shared corner).
std::vector<float> g_own_xy;
std::vector<int> g_own_px, g_own_vtx;
std::vector<std::int8_t> g_own_dir;
std::vector<std::uint8_t> g_keep, g_keep_own;
std::vector<std::uint8_t> g_used;   // per corner: touches a scene pixel
std::vector<std::vector<int>> g_pairs;   // per chunk, two rows: see the vertices
// The reference height, eased: the mean height of the map on screen. Written
// by the warp (gbarecomp's present worker), read by perspective_overscan_rows
// on the guest's thread: atomic, relaxed.
std::atomic<bool> g_have_ref{false};
std::atomic<float> g_yref{0.0f};
// The camera as captured for the frame the warp draws (perspective_capture):
// the live values move on with the guest while the worker runs.
struct CamCapture { float strike_t = 0.0f, look_x = 0.0f, look_y = 0.0f; };
CamCapture g_cam_cap;

// KOL_PERSP_TRIM_CHECK=1: compose every side column, and count the places
// perspective_overscan_rows would have left out that the warp draws inside
// the view. It should find none.
bool trim_check() {
    static const bool on = kol::env_true("KOL_PERSP_TRIM_CHECK");
    return on;
}
std::vector<unsigned char> g_check_rows;
long g_check_frames = 0, g_check_bad = 0;
float g_check_clear = 1e9f;

// Debug > Tile Info's readout at the top left of a view_w x view_h picture,
// white on a dark box, into g_moved / g_moved_rgb (drawn flat on top).
void draw_tile_text(const char* text, int view_w, int view_h) {
    auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= view_w || y >= view_h) return;
        const std::size_t q = static_cast<std::size_t>(y) * view_w + x;
        g_moved[q] = 1;
        g_moved_rgb[q * 3] = r; g_moved_rgb[q * 3 + 1] = g; g_moved_rgb[q * 3 + 2] = b;
    };
    int lines = 1, longest = 0, len = 0;
    for (const char* t = text; *t; ++t) {
        if (*t == '\n') { ++lines; len = 0; } else longest = std::max(longest, ++len);
    }
    const int x0 = 4, y0 = 4;
    for (int y = y0 - 2; y < y0 + lines * 7 + 1; ++y)
        for (int x = x0 - 2; x < x0 + longest * 4 + 1; ++x) put(x, y, 16, 16, 24);
    int col = 0, line = 0;
    for (const char* t = text; *t; ++t) {
        if (*t == '\n') { ++line; col = 0; continue; }
        std::uint16_t bits = 0;
        for (const Glyph& gl : kGlyphs) if (gl.c == *t) bits = gl.bits;
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 3; ++c)
                if (bits & (1u << (14 - (r * 3 + c))))
                    put(x0 + col * 4 + c, y0 + line * 7 + r, 255, 240, 120);
        ++col;
    }
}

// KOL_PERSP_KIND=1, or Debug > Perspective Mesh: the scene painted by what
// each pixel is taken for (see the warp's step for the colours). The warp
// and the flat light both.
// `keep` (null: none) marks pixels left as they are: the flat light's units,
// which keep their colours as the warp's squares do.
bool mesh_paint_on() {
    static const bool paint_kind_env = kol::env_true("KOL_PERSP_KIND");
    return paint_kind_env || kol::setting_on(kol::Setting::DebugMesh);
}
// One pixel's paint: green flat, red upright, light and dark bands every 8
// units of depth, blue for an overlay (see the call in the warp provider).
inline void mesh_paint_px(std::uint8_t* c, unsigned kd, std::int32_t xz, bool overlay, bool iface) {
    const bool band = ((xz >> 3) & 1) != 0;
    c[0] = kd == kol::kSceneUpright ? (band ? 255 : 160) : (band ? 40 : 0);
    c[1] = kd == kol::kSceneFlat ? (band ? 255 : 160) : 0;
    c[2] = overlay ? 255 : iface ? 128 : 0;
}
void paint_mesh_kind(const std::uint8_t* kind, const std::int32_t* xz, const std::uint8_t* iface,
                     const std::uint8_t* floating, std::uint8_t* scene, std::size_t pixels,
                     const std::uint8_t* keep = nullptr) {
    if (mesh_paint_on())
        for (std::size_t k = 0; k < pixels; ++k) {
            if (keep && keep[k]) continue;
            mesh_paint_px(scene + k * 3, kind[k], xz[k],
                          kind[k] == kol::kSceneOverlay || (floating && floating[k]), iface[k] != 0);
        }
}

// A map's cut edge faded (kol_focus, kEdgeFadeMaps): the colour the backdrop
// shows on each scene row (its first backdrop pixel, or the nearest row's),
// and a colour pulled toward it by the pixel's edge_fade.
void edge_sky_rows(const std::uint8_t* layers, const std::uint8_t* iface, const std::uint8_t* scene,
                   int w, int h, std::vector<std::uint8_t>& rows) {
    rows.assign(static_cast<std::size_t>(h) * 3, 0);
    std::vector<int> have(static_cast<std::size_t>(h), -1);
    for (int py = 0; py < h; ++py)
        for (int px = 0; px < w; ++px) {
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            if ((layers[k] & 0x0Fu) != kol::kLayerBackdrop || iface[k]) continue;
            have[py] = static_cast<int>(k);
            break;
        }
    for (int py = 0; py < h; ++py) {
        int src = have[py];
        for (int d = 1; src < 0 && d < h; ++d) {
            if (py - d >= 0 && have[py - d] >= 0) src = have[py - d];
            else if (py + d < h && have[py + d] >= 0) src = have[py + d];
        }
        if (src >= 0) std::copy_n(scene + static_cast<std::size_t>(src) * 3, 3, &rows[static_cast<std::size_t>(py) * 3]);
    }
}
// A map's backdrop recoloured (kol_focus, kEdgeFadeMaps sky): each
// backdrop pixel by its brightness through a ramp, its first colour at 64
// (map 89's sky is 70 at its darkest, 187 at its brightest, 8,66,255 to
// 90,239,255), its last at 192, and below 64 the first colour darkened
// toward black (a screen fade), then pulled `desat` toward its own grey.
// Done first, so everything after (the edge fade, the warp's sky filling)
// reads it.
//   grey: black to 96 (the user's pick of 64/96/128, 2026-10-08).
//   dusk: deep indigo high up, mauve, dusty rose, a peach horizon.
//   black: black (map 63, the night camp).
//   meadow: the pale sage of map 93's far hills (its art's own haze, 195,242,198
//   at the top of the hills, 150,215,135 lower), deeper high up, paler low.
void recolour_backdrop(const kol::ScenePixels& sp, const std::uint8_t* layers, const std::uint8_t* iface,
                       std::uint8_t* scene, std::size_t pixels, int sky, float desat, float mix = 1.0f) {
    if (mix <= 0.0f) return;
    struct Stop { float t; float c[3]; };
    static constexpr Stop kGrey[] = {{0.0f, {0, 0, 0}}, {1.0f, {96, 96, 96}}};
    static constexpr Stop kDusk[] = {{0.0f, {30, 26, 58}},   {0.35f, {74, 58, 104}},
                                     {0.65f, {150, 96, 120}}, {0.85f, {206, 128, 108}},
                                     {1.0f, {230, 166, 120}}};
    static constexpr Stop kBlack[] = {{0.0f, {0, 0, 0}}, {1.0f, {0, 0, 0}}};
    static constexpr Stop kMeadow[] = {{0.0f, {172, 224, 168}}, {1.0f, {212, 246, 216}}};
    // KOL_SKY_RGB=r,g,b: any recoloured sky one flat colour (mockups).
    static Stop flat[2] = {{0.0f, {0, 0, 0}}, {1.0f, {0, 0, 0}}};
    static const bool flat_on = [] {
        const char* e = std::getenv("KOL_SKY_RGB");
        if (!e) return false;
        float c[3] = {0, 0, 0};
        if (std::sscanf(e, "%f,%f,%f", &c[0], &c[1], &c[2]) != 3) return false;
        for (int i = 0; i < 3; ++i) flat[0].c[i] = flat[1].c[i] = c[i];
        return true;
    }();
    const Stop* ramp = flat_on ? flat : sky == kol::kSkyDusk ? kDusk : sky == kol::kSkyBlack ? kBlack
                     : sky == kol::kSkyMeadow ? kMeadow : kGrey;
    const int stops = !flat_on && sky == kol::kSkyDusk ? 5 : 2;
    // By brightness, 0..255, worked out once per frame.
    std::uint8_t lut[256][3];
    for (int lum = 0; lum < 256; ++lum) {
        const float t = std::clamp((lum - 64) / 128.0f, 0.0f, 1.0f);
        int i = 0;
        while (i + 2 < stops && t > ramp[i + 1].t) ++i;
        const float u = std::clamp((t - ramp[i].t) / (ramp[i + 1].t - ramp[i].t), 0.0f, 1.0f);
        // (sky_dim: the map's own palette darkened, kol_focus map_palette_dim.)
        const float dark = (lum < 64 ? lum / 64.0f : 1.0f) * std::clamp(sp.sky_dim, 0.0f, 1.0f);
        float c[3];
        for (int ch = 0; ch < 3; ++ch) c[ch] = (ramp[i].c[ch] + (ramp[i + 1].c[ch] - ramp[i].c[ch]) * u) * dark;
        const float g = 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];
        for (int ch = 0; ch < 3; ++ch)
            lut[lum][ch] = static_cast<std::uint8_t>(std::lround(std::clamp(c[ch] + (g - c[ch]) * desat, 0.0f, 255.0f)));
    }
    par_px(sp, pixels, [&](std::size_t k) {
        if ((layers[k] & 0x0Fu) != kol::kLayerBackdrop || iface[k]) return;
        std::uint8_t* c = scene + k * 3;
        const int lum = (77 * c[0] + 150 * c[1] + 29 * c[2]) >> 8;
        if (mix >= 1.0f) { std::copy_n(lut[lum], 3, c); return; }
        for (int ch = 0; ch < 3; ++ch) c[ch] = static_cast<std::uint8_t>(std::lround(c[ch] + (lut[lum][ch] - c[ch]) * mix));
    });
}
// Options > Graphics > Smooth Sky: a sky gradient without its steps. The
// game draws a gradient a line at a time (an HBlank DMA walks palette entry
// 0 down a table of 160 colours), in the GBA's 32 levels a channel: map 16's
// blue-to-green is 41 colours, a band every few lines, a few stretches
// dithered a line apart (stripes), and the widened view's rows past the
// game's 160 repeat its first and last colours (37 flat rows at the top and
// the bottom at 384 x 216). The user (2026-11-02) asked for smoother
// gradients with more colours, and picked D of four stills
// (shots/sky_gradient/): the game's 160 lines (ScenePixels::row_backdrop,
// as composed, with any fade) blurred at 8 bits a channel (three box passes
// of 25 lines, the ends carried on their slope: within 6 of the game's
// colour on any line), then spread over the view's rows, top to bottom, so
// no colour the game does not have and no flat band. Only a gradient: a
// backdrop that moves more than one 5-bit level (9) between lines anywhere
// keeps the game's colours, and one colour has nothing to smooth. The
// rows' colours stay in g_sky_rows for the warp's backdrop under the map
// (warp_backdrop); empty when not smoothed. KOL_SKY_SMOOTH_W=<lines>: the
// box's half width (12).
std::vector<std::uint8_t> g_sky_rows;
void smooth_sky(const kol::ScenePixels& sp, const std::uint8_t* layers, const std::uint8_t* iface,
                std::uint8_t* scene, int w, int h) {
    g_sky_rows.clear();
    constexpr int kLines = 160;
    if (!kol::setting_on(kol::Setting::SmoothSky) || sp.sky != kol::kSkyGame || !sp.row_backdrop ||
        sp.oy < 0 || sp.oy + kLines > h || sp.vh < 2)
        return;
    const std::uint8_t* rb = sp.row_backdrop + static_cast<std::size_t>(sp.oy) * 3;
    // Map 84's scene (states/thor/0183): the game's sky goes grey, bright blue
    // (around its white flash), then blue to green, some 500 pictures before
    // the terrain's palette turns orange. The user (2026-11-10) asked for the sky
    // to go from grey to blue-green at the same time as the palette turns
    // orange. The grey ramp is kept while it shows, and shown
    // until the palette warms (ScenePixels::pal_warm, the ice's two palette rows:
    // -0.13 to -0.17 blue, +0.04 the dark purple at picture ~790, +0.13 the orange
    // from ~850, brightening to ~950): from kSkyWarm0 to kSkyWarm1 it crosses to
    // the game's ramp. The grey ramp is told by its top line (41, 57, 57; it ends
    // pale blue at the bottom; the bright blue's top is 0, 173, 255). By the palette, not by time: the scene's
    // pace is the player's. No grey seen since the map came up (a state loaded
    // later): the game's sky. KOL_NO_SKY_HOLD=1: the game's sky; KOL_SKY_HOLD_LOG=1.
    static const bool no_hold = kol::env_set("KOL_NO_SKY_HOLD");
    static const bool hold_log = kol::env_set("KOL_SKY_HOLD_LOG");
    constexpr float kSkyWarm0 = -0.06f, kSkyWarm1 = 0.125f;
    static std::uint8_t held[kLines * 3], mixed[kLines * 3];
    static bool have_held = false;
    if (sp.map == 84 && !no_hold) {
        // (Whole, and with no jump: the game changes the ramp a part at a time, the
        // bottom lines bright, 198, 255, 255, while the top is still grey.)
        bool grey = std::max({rb[0], rb[1], rb[2]}) - std::min({rb[0], rb[1], rb[2]}) <= 32 && rb[2] < 128 &&
                    rb[(kLines - 1) * 3 + 1] < 200;
        for (int i = 3; i < kLines * 3 && grey; ++i) grey = std::abs(rb[i] - rb[i - 3]) <= 9;
        const float t = std::clamp((sp.pal_warm - kSkyWarm0) / (kSkyWarm1 - kSkyWarm0), 0.0f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t);
        if (hold_log) std::fprintf(stderr, "sky_hold: serial %u warm %.3f grey %d have %d s %.2f\n", static_cast<unsigned>(sp.serial), sp.pal_warm, grey, have_held, s);
        if (grey && s <= 0.0f) {
            std::copy_n(rb, kLines * 3, held);
            have_held = true;
        } else if (have_held && s < 1.0f) {
            for (int i = 0; i < kLines * 3; ++i)
                mixed[i] = static_cast<std::uint8_t>(std::lround(held[i] + (static_cast<float>(rb[i]) - held[i]) * s));
            rb = mixed;
        }
    } else {
        have_held = false;
    }
    bool moves = false;
    for (int i = 1; i < kLines; ++i)
        for (int ch = 0; ch < 3; ++ch) {
            const int d = std::abs(rb[i * 3 + ch] - rb[(i - 1) * 3 + ch]);
            if (d > 9) return;
            if (d) moves = true;
        }
    if (!moves) return;
    static const int half = static_cast<int>(kol::env_num_clamp("KOL_SKY_SMOOTH_W", 1.0f, 60.0f, 12.0f));
    float line[3][kLines];
    for (int ch = 0; ch < 3; ++ch) {
        float* v = line[ch];
        for (int i = 0; i < kLines; ++i) v[i] = rb[i * 3 + ch];
        for (int pass = 0; pass < 3; ++pass) {
            // Past the ends, the line carried on its slope (mirrored about
            // the end), so the ends keep their trend rather than flatten.
            float ext[kLines + 2 * 60];
            for (int i = 0; i < half; ++i) {
                ext[half - 1 - i] = 2.0f * v[0] - v[std::min(kLines - 1, i + 1)];
                ext[half + kLines + i] = 2.0f * v[kLines - 1] - v[std::max(0, kLines - 2 - i)];
            }
            std::copy_n(v, kLines, ext + half);
            float sum = 0.0f;
            for (int i = 0; i < 2 * half + 1; ++i) sum += ext[i];
            for (int i = 0; i < kLines; ++i) {
                v[i] = sum / static_cast<float>(2 * half + 1);
                if (i + 1 < kLines) sum += ext[i + 2 * half + 1] - ext[i];
            }
        }
    }
    // Every scene row: its place in the view, top to bottom, as a game line
    // (the overscan past the view held at the ends).
    g_sky_rows.resize(static_cast<std::size_t>(h) * 3);
    const float per_row = static_cast<float>(kLines - 1) / static_cast<float>(sp.vh - 1);
    for (int y = 0; y < h; ++y) {
        const float t = std::clamp(static_cast<float>(y - sp.vy) * per_row, 0.0f, static_cast<float>(kLines - 1));
        const int i = std::min(static_cast<int>(t), kLines - 2);
        const float f = t - static_cast<float>(i);
        for (int ch = 0; ch < 3; ++ch)
            g_sky_rows[static_cast<std::size_t>(y) * 3 + ch] = static_cast<std::uint8_t>(
                std::lround(std::clamp(line[ch][i] + (line[ch][i + 1] - line[ch][i]) * f, 0.0f, 255.0f)));
    }
    par_px(sp, static_cast<std::size_t>(w) * h, [&](std::size_t k) {
        if ((layers[k] & 0x0Fu) != kol::kLayerBackdrop || iface[k]) return;
        std::copy_n(&g_sky_rows[(k / static_cast<std::size_t>(w)) * 3], 3, scene + k * 3);
    });
}
inline void edge_fade_px(std::uint8_t* c, const std::uint8_t* sky, unsigned f) {
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = static_cast<std::uint8_t>((sky[ch] * (255u - f) + c[ch] * f + 127u) / 255u);
}

// Torchlight on one pixel (kol_focus): `g` of 255 from lamp `lamp`, at its
// flicker. Orange light on the ruins' green stone comes out yellow by
// multiplication alone, so the pixel is also pulled toward its own
// brightness in orange; the art's texture stays. A blue flame's light (the
// castle's statue) the same way in blue. KOL_PERSP_GLOW_BLUE=<strength>
// scales the blue alone (mockups).
// Colours swapped on listed model parts before the light (ScenePixels
// recolour_*, kol_focus kModelRecolours).
void model_recolour(const kol::ScenePixels& sp, std::uint8_t* scene, std::size_t pixels) {
    if (!sp.recolour_mask || !sp.recolour_pairs || sp.recolour_count <= 0) return;
    for (std::size_t k = 0; k < pixels; ++k) {
        if (!sp.recolour_mask[k]) continue;
        std::uint8_t* px = scene + k * 3;
        const std::uint32_t c = static_cast<std::uint32_t>(px[0]) << 16 | static_cast<std::uint32_t>(px[1]) << 8 | px[2];
        for (int i = 0; i < sp.recolour_count; ++i)
            if (sp.recolour_pairs[2 * i] == c) {
                const std::uint32_t t = sp.recolour_pairs[2 * i + 1];
                px[0] = static_cast<std::uint8_t>(t >> 16); px[1] = static_cast<std::uint8_t>(t >> 8); px[2] = static_cast<std::uint8_t>(t);
                break;
            }
    }
}

// (Read once at start, not a lit pixel at a time.)
const float torch_s_blue = kol::env_num_min("KOL_PERSP_GLOW_BLUE", 0.0f, 1.0f);
void torch_tint(std::uint8_t* px, const kol::ScenePixels& sp, unsigned g, unsigned lamp, float true_share = 0.0f,
                long mix_k = -1) {
    const float s = sp.glow_s;   // (kol::glow_strength(), once a picture)
    const float s_blue = torch_s_blue;
    // 0 the torches' orange, 1 the blue flames, 2 daylight through a door
    // (kol_focus kDoorLights): warm white.
    // 3: cool skylight through a window away from the sun (kDoorLights).
    // 4: embers' deep red-orange (map 63's campfire).
    // 5: green (map 83's torches): {0.3, 1, 0.15} and {0.55, 1.5, 0.35} at
    // 60% saturation, toward their means (the user asked for a less saturated
    // green, 2026-11-03).
    // 6: a lantern, halfway from 0 to 2 (map 52: asked for slightly more neutral
    // but still a little warm, 2026-11-10).
    static constexpr float kTint[kol::kHueCount][3] = {{1.0f, 0.5f, 0.12f}, {0.15f, 0.45f, 1.0f}, {0.85f, 0.72f, 0.45f},
                                                       {0.55f, 0.7f, 0.95f}, {1.0f, 0.28f, 0.05f},
                                                       {0.3733f, 0.7933f, 0.2833f}, {0.925f, 0.61f, 0.285f}};
    static constexpr float kHue[kol::kHueCount][3] = {{1.45f, 0.78f, 0.28f}, {0.35f, 0.8f, 1.6f}, {1.15f, 1.0f, 0.78f},
                                                      {0.85f, 0.97f, 1.2f}, {1.5f, 0.55f, 0.18f},
                                                      {0.65f, 1.22f, 0.53f}, {1.3f, 0.89f, 0.53f}};
    // The green's saturation, further: its rows pulled toward their own
    // mean (KOL_GREEN_SAT=<0..1>, mockups; 1 the table's).
    static float kTintGreen[3], kHueGreen[3];
    static const bool green_ready = [] {
        const float sat = kol::env_num_clamp("KOL_GREEN_SAT", 0.0f, 1.0f, 1.0f);
        for (const auto& [src, dst] : {std::pair{kTint[kol::kHueGreen], kTintGreen}, std::pair{kHue[kol::kHueGreen], kHueGreen}}) {
            const float mean = (src[0] + src[1] + src[2]) / 3.0f;
            for (int ch = 0; ch < 3; ++ch) dst[ch] = mean + sat * (src[ch] - mean);
        }
        return true;
    }();
    (void)green_ready;
    const int hue = sp.glow_hue ? std::min<int>(kol::kHueCount - 1, sp.glow_hue[lamp]) : 0;
    const float amount = std::min(1.0f, (hue == kol::kHueBlue ? s * s_blue : s) * static_cast<float>(g) / 255.0f *
                                            sp.glow_flick[lamp]);
    // (true_share: near a campfire its light shows the art's own colours, its
    // tint gone; see tint_px.)
    const float amount_t = amount * (1.0f - std::clamp(true_share, 0.0f, 1.0f));
    const float* kTintH = hue == kol::kHueGreen ? kTintGreen : kTint[hue];
    const float* kOrange = hue == kol::kHueGreen ? kHueGreen : kHue[hue];
    // Where lamps of another colour light the pixel too (mix_k: its scene pixel;
    // ScenePixels::glow_mix), the two colours by their shares of the light.
    float tint_m[3], hue_m[3];
    if (mix_k >= 0 && sp.glow_mix && sp.glow_mix[mix_k]) {
        const int h2 = std::min<int>(kol::kHueCount - 1, sp.glow_hue2[mix_k]);
        // The other colour's share, sharpened about a half (KOL_LAMP_MIX_SHARP=<p>, 2; 1 the
        // plain share): by the plain share map 56's hall went nearly all blue (five torches
        // against one chandelier), and a part of it left a seam where the brightest lamp
        // changes. m^p / (m^p + (1-m)^p) meets itself there (the two sides' shares sum to
        // 1), so each light keeps its colour near it and they blend between.
        static const float sharp = kol::env_num_clamp("KOL_LAMP_MIX_SHARP", 0.25f, 8.0f, 2.0f);
        const float m0 = sp.glow_mix[mix_k] / 255.0f;
        const float ma = std::pow(m0, sharp), mb = std::pow(1.0f - m0, sharp);
        const float m = ma / std::max(1.0e-6f, ma + mb);
        const float* t2 = h2 == kol::kHueGreen ? kTintGreen : kTint[h2];
        const float* o2 = h2 == kol::kHueGreen ? kHueGreen : kHue[h2];
        for (int ch = 0; ch < 3; ++ch) {
            tint_m[ch] = kTintH[ch] + (t2[ch] - kTintH[ch]) * m;
            hue_m[ch] = kOrange[ch] + (o2[ch] - kOrange[ch]) * m;
        }
        kTintH = tint_m;
        kOrange = hue_m;
    }
    const float lum = 0.3f * px[0] + 0.55f * px[1] + 0.15f * px[2];
    for (int ch = 0; ch < 3; ++ch) {
        const float lit = px[ch] * (1.0f + 0.6f * amount_t * kTintH[ch]);
        const float warm = lum * kOrange[ch] * (1.0f + 0.4f * amount_t);
        const float v = lit + (warm - lit) * 0.45f * amount_t;
        px[ch] = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, v)));
    }
}

// On the range's tiles (ScenePixels::range) a shading factor keeps only
// KOL_RANGE_LIGHT of its effect (0.4): the movement and action tiles stay
// readable in shadows and the dark (asked 2026-10-14).
float range_keep_share() {
    static const float keep = kol::env_num_clamp("KOL_RANGE_LIGHT", 0.0f, 1.0f, 0.4f);
    return keep;
}
float range_keep(const kol::ScenePixels& sp, std::size_t k, float f) {
    // (2: the grid cursor's corner pieces, left as drawn: kol_focus g_cursor_face.)
    if (!sp.range || !sp.range[k]) return f;
    return sp.range[k] == 2 ? 1.0f : 1.0f - range_keep_share() * (1.0f - f);
}
// The range's tiles take none of the water's, the ice's or the wet look's
// effects (asked 2026-11-02 to keep puddle, water and ice effects off
// the movement/attack tiles; states/thor/0098, the move range on
// map 41's ice, the sheen, the mirror and the glints through the tiles):
// those pixels keep the game's art with the highlight on it. Lighting and
// shading still take a share there (range_keep). KOL_RANGE_EFFECTS=1: the
// effects on the tiles too, as before.
bool range_plain(const kol::ScenePixels& sp, std::size_t k) {
    static const bool off = kol::env_set("KOL_RANGE_EFFECTS");
    return !off && sp.range && sp.range[k];
}

// The lamps' light on scene pixel k this frame, 0..255 (ScenePixels::glow,
// the pool cached per view). With the flame's lean (kol_focus g_glow_lx):
// the pool read a little away from the lean, up to KOL_POT_SHIFT px (1) at
// full lean 48 px or more from the flame, between pixels (the four around,
// weighted), so the pool and the terrain's shadows in it glide as a light's
// do when it moves; and swayed by it, a little stronger toward the lean,
// weaker away (within about 64 px, 0.6..1.4). The user reported the
// environment shadows not moving with the flame, then asked for slower,
// smaller, sub-pixel motion; the pool is cached per view, and recomputing it for a
// moving flame each frame costs too much. Not a unit's shadow (lamp_shade):
// the callers take that off after the darkness's cap (dark_light).
// 0 where the pool is 0 (it fades to nothing at its reach, so a read past
// its edge is nothing too). Read for every pixel of the scene twice a
// frame, a square root each, this was the torchlight's largest cost at
// 480x270 (the user, 2026-10-14, found it very slow): now once (apply_lighting
// keeps it), the slide's ramp near the flame by the distance squared (no
// root), the sway by a dot product, and the read a plain weighted one (the
// pool holds every lamp's light summed, so a neighbour's is the same light,
// whichever lamp it is mostly from).
unsigned lamp_light(const kol::ScenePixels& sp, std::size_t k) {
    if (!sp.glow) return 0u;
    const unsigned g0 = sp.glow[k];
    if (!g0 || !sp.glow_lamp || !sp.glow_fx || sp.w <= 0 || sp.h <= 0) return g0;
    const unsigned i = sp.glow_lamp[k];
    const float lx = sp.glow_lx[i], ly = sp.glow_ly[i];
    if (lx == 0.0f && ly == 0.0f) return g0;
    static const float shift_over_full = [] {   // KOL_POT_SHIFT / KOL_POT_LEAN (kol_focus)
        const float shift = kol::env_num_min("KOL_POT_SHIFT", 0.0f, 1.0f);
        return shift / kol::env_num_min("KOL_POT_LEAN", 1e-3f, kol::kPotLeanDefault);
    }();
    const int w = sp.w, h = sp.h;
    const int x = static_cast<int>(k % static_cast<std::size_t>(w)), y = static_cast<int>(k / static_cast<std::size_t>(w));
    const float dx = static_cast<float>(x) - sp.glow_fx[i], dy = static_cast<float>(y) - sp.glow_fy[i];
    const float s = shift_over_full * std::min(1.0f, (dx * dx + dy * dy) * (1.0f / 2304.0f));
    const float fx = std::clamp(static_cast<float>(x) - lx * s, 0.0f, static_cast<float>(w - 1));
    const float fy = std::clamp(static_cast<float>(y) - ly * s, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
    const std::uint8_t* G = sp.glow;
    const std::size_t r0 = static_cast<std::size_t>(y0) * w, r1 = static_cast<std::size_t>(y1) * w;
    const float top = G[r0 + x0] * (1.0f - ax) + G[r0 + x1] * ax;
    const float bot = G[r1 + x0] * (1.0f - ax) + G[r1 + x1] * ax;
    float g = top * (1.0f - ay) + bot * ay;
    g *= std::clamp(1.0f + (dx * lx + dy * ly) * (1.0f / 64.0f), 0.6f, 1.4f);
    return static_cast<unsigned>(std::min(255.0f, g + 0.5f));
}
// ... less a unit's shadow from the lamps (ScenePixels::lamp_shade).
unsigned lamp_light_shaded(const kol::ScenePixels& sp, std::size_t k) {
    const unsigned g = lamp_light(sp, k);
    return g && sp.lamp_shade ? g * (255u - sp.lamp_shade[k]) / 255u : g;
}

// The torchlight brings a dark map's daylight colours (ScenePixels::lamp_pal,
// kol_focus kLampPalette): a pixel in a night colour turns toward its
// daylight one past lamp_pal_from of `t`, the lamps' share of its light
// (0 the darkness, 1 full), all the way at 1. Before the darkness and the
// torch's tint, which then fall on it as on any colour.
void lamp_palette(const kol::ScenePixels& sp, std::uint8_t* px, float t) {
    const float from = sp.lamp_pal_from;
    t = (t - from) / (1.0f - from);
    if (t <= 0.0f) return;
    const std::int32_t v = sp.lamp_pal[(px[0] >> 3) | (px[1] >> 3) << 5 | (px[2] >> 3) << 10];
    if (v < 0) return;
    t = std::min(1.0f, t);
    for (int ch = 0; ch < 3; ++ch) {
        const float day = static_cast<float>((v >> (8 * ch)) & 255);
        px[ch] = static_cast<std::uint8_t>(std::lround(px[ch] + (day - px[ch]) * t));
    }
}

// An indoor map's light on a pixel lit `g` (0..255) by the lamps: from the
// darkness (ScenePixels::dark) to full, the light scaled by dark_gain (the
// map's kDarkMaps, KOL_INDOOR_LAMP), then less a unit's shadow (`shade`
// 0..255, ScenePixels::lamp_shade): after the gain's cap, or near a lamp
// the shadow took nothing visible.
// `win` (0..255): the light from the windows on a dark map with them (the
// sun through a window on it, or the beams' radiated glow), which brings the
// darkness back up as the lamps' does (no gain: a sunlit patch is full); 0
// none, and then the lamps' alone, exactly as before.
// The share of a dark map's darkness kept at scene pixel k under its roofed
// space (kol_focus kDarkCovers), 1 outside: by the pixel's own world place
// (its depth and height), so the floor inside, the inner curtain and the
// back wall take it and the curtains' outer faces do not.
float cover_at(const kol::ScenePixels& sp, std::size_t k) {
    if (!sp.xz || !sp.y || sp.w <= 0 || (sp.kind && sp.kind[k] == kol::kSceneNone)) return 1.0f;
    const float hx = static_cast<float>(static_cast<int>(k % static_cast<std::size_t>(sp.w)) - sp.kx - sp.ox);
    const float s = static_cast<float>(sp.xz[k]);
    const float X = 0.5f * s + hx, Z = 0.5f * s - hx, Y = static_cast<float>(sp.y[k]);
    if (X <= sp.cover_x0 || X >= sp.cover_x1 || Z <= sp.cover_z0 || Z >= sp.cover_z1 || Y >= sp.cover_y1) return 1.0f;
    const float t = std::clamp((sp.cover_z1 - Z) / std::max(1.0f, sp.cover_soft), 0.0f, 1.0f);
    return 1.0f - (1.0f - sp.cover_level) * t * t * (3.0f - 2.0f * t);
}
// A dark map's darkness at scene pixel k: sp.dark, or more inside its pool
// of faint light (kol_focus kDarkPools: the pixel taken at the floor's
// height, so the ellipse lies on the ground and moves with the map), times
// its roofed space's (cover_at).
float dark_at(const kol::ScenePixels& sp, std::size_t k) {
    const float cover = sp.cover_level < 1.0f ? cover_at(sp, k) : 1.0f;
    if (sp.pool_level <= 0.0f || sp.w <= 0) return sp.dark * cover;
    const float ax = static_cast<float>(static_cast<int>(k % static_cast<std::size_t>(sp.w)) - sp.kx - sp.ox);
    const float ay = static_cast<float>(static_cast<int>(k / static_cast<std::size_t>(sp.w)) - sp.ky - sp.oy);
    const float d = 2.0f * ax, s = 4.0f * (ay + sp.pool_y * 0.5f);
    const float X = 0.5f * (s + d) - sp.pool_x, Z = 0.5f * (s - d) - sp.pool_z;
    const float r = std::sqrt(X * X / (sp.pool_rx * sp.pool_rx) + Z * Z / (sp.pool_rz * sp.pool_rz));
    if (r >= 1.0f) return sp.dark * cover;
    const float t = std::clamp((1.0f - r) / std::max(1e-3f, 1.0f - sp.pool_inner), 0.0f, 1.0f);
    return std::max(sp.dark, sp.pool_level * t * t * (3.0f - 2.0f * t)) * cover;
}
float dark_light(const kol::ScenePixels& sp, unsigned g, unsigned shade = 0u, unsigned win = 0u, float base = -1.0f) {
    if (base < 0.0f) base = sp.dark;
    float lit = std::min(1.0f, sp.dark_gain * static_cast<float>(g) / 255.0f) *
                (1.0f - static_cast<float>(shade) / 255.0f);
    if (win) lit = 1.0f - (1.0f - lit) * (1.0f - static_cast<float>(win) / 255.0f);
    return base + (1.0f - base) * lit;
}

// The late day's light on one pixel (ScenePixels::evening, 0..1 and past it
// for mockups): red up a little, blue down, and pulled toward its own
// brightness in orange, as the torchlight is, so the art's texture stays.
void evening_tint(std::uint8_t* px, float a) {
    static constexpr float kMul[3] = {0.10f, -0.04f, -0.28f};
    static constexpr float kHue[3] = {1.30f, 0.94f, 0.55f};
    const float lum = 0.3f * px[0] + 0.55f * px[1] + 0.15f * px[2];
    for (int ch = 0; ch < 3; ++ch) {
        const float lit = px[ch] * (1.0f + a * kMul[ch]);
        const float v = lit + (lum * kHue[ch] - lit) * 0.3f * std::min(1.0f, a);
        px[ch] = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, v)));
    }
}

// A map at sunset's grade on one pixel: toward a golden warmth where it is
// lit (`lit` 1) and a cool purple grey in the shade (0), at strength g.
void sunset_grade(std::uint8_t* px, float g, float lit) {
    static constexpr float kSun[3] = {1.12f, 0.95f, 0.74f}, kShade[3] = {0.80f, 0.80f, 1.02f};
    for (int ch = 0; ch < 3; ++ch) {
        const float m = 1.0f + g * ((kShade[ch] + (kSun[ch] - kShade[ch]) * lit) - 1.0f);
        px[ch] = static_cast<std::uint8_t>(std::clamp(px[ch] * m, 0.0f, 255.0f));
    }
}

// A light's place in the world (L.x, L.y, L.z) on the scene's pixels
// (x = (X - Z) / 2, y = (X + Z) / 4 - Y / 2, from kx + ox, ky + oy).
inline void light_screen(const kol::ScenePixels& sp, float x, float y, float z, float* sx, float* sy) {
    *sx = (x - z) / 2.0f + sp.kx + sp.ox;
    *sy = (x + z) / 4.0f - y / 2.0f + sp.ky + sp.oy;
}
// A light's (a FireLight's, a SpellLight's) distance squared from world
// point (X, Y, Z).
template <class Light>
inline float light_d2(const Light& L, float X, float Y, float Z) {
    return (X - L.x) * (X - L.x) + (Y - L.y) * (Y - L.y) + (Z - L.z) * (Z - L.z);
}
// Every scene pixel within a light's reach, by its world place, in a box
// about the light on screen: visit(k, f0), f0 = 1 - d / reach, d the
// distance in 3D. `down`: how far the box reaches down from the light
// (before its rounding up), 0.62 x reach for a sphere (below).
template <class Light, class F>
void light_box(const kol::ScenePixels& sp, const Light& L, float down, F&& visit) {
    const int w = sp.w, h = sp.h;
    float sx, sy;
    light_screen(sp, L.x, L.y, L.z, &sx, &sy);
    // A sphere of `reach` covers at most 0.71 of it across the screen
    // and 0.61 of it down (x = (X - Z) / 2, y = (X + Z) / 4 - Y / 2).
    const int rx = static_cast<int>(std::ceil(0.71f * L.reach)) + 1, ry = static_cast<int>(std::ceil(down)) + 1;
    const int x0 = std::max(0, static_cast<int>(sx) - rx), x1 = std::min(w - 1, static_cast<int>(sx) + rx);
    const int y0 = std::max(0, static_cast<int>(sy) - ry), y1 = std::min(h - 1, static_cast<int>(sy) + ry);
    const float r2 = L.reach * L.reach;
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            if (sp.kind[k] == kol::kSceneNone) continue;
            float X, Z;
            world_xz(sp, px, k, &X, &Z);
            const float Y = static_cast<float>(sp.y[k]);
            const float d2 = light_d2(L, X, Y, Z);
            if (d2 >= r2) continue;
            visit(k, 1.0f - std::sqrt(d2) / L.reach);
        }
}

// Fire light (ScenePixels::fire_lights) at world point (X, Y, Z): the sum of
// each light's pool, (1 - d / reach)^2 x power, d the distance in 3D.
float fire_light_at(const kol::ScenePixels& sp, float X, float Y, float Z) {
    float I = 0.0f;
    for (int i = 0; i < sp.fire_light_count; ++i) {
        const auto& L = sp.fire_lights[i];
        const float d = std::sqrt(light_d2(L, X, Y, Z));
        if (d < L.reach) I += L.power * (1.0f - d / L.reach) * (1.0f - d / L.reach);
    }
    return I;
}
// ... on one pixel: warmer and brighter, orange, the art's texture kept.
void fire_light_px(std::uint8_t* px, float I) {
    if (I <= 0.0f) return;
    I = std::min(I, 1.5f);
    static constexpr float kMul[3] = {0.95f, 0.5f, 0.12f}, kAdd[3] = {30.0f, 11.0f, 0.0f};
    for (int ch = 0; ch < 3; ++ch)
        px[ch] = static_cast<std::uint8_t>(std::clamp(px[ch] * (1.0f + I * kMul[ch]) + I * kAdd[ch], 0.0f, 255.0f));
}
// The fire light's pools this frame, per scene pixel (by its world place),
// each light's in a box about it; null when there are none.
const float* fire_light_pools(const kol::ScenePixels& sp) {
    if (!sp.fire_light_count || !sp.xz || !sp.y || !sp.kind) return nullptr;
    const int w = sp.w, h = sp.h;
    static thread_local std::vector<float> acc;
    // The same lights over the same view: the same pools (the burning town
    // holds its camera; worked out again only when one moves).
    static thread_local std::uint32_t acc_key = 0;
    std::uint32_t key = kol::kFnvBasis;
    auto mix = [&key](std::uint32_t v) { kol::fnv_mix(key, v); };
    kol::fnv_ints(key, {w, h, sp.ox, sp.oy, sp.kx, sp.ky});
    for (int i = 0; i < sp.fire_light_count; ++i) {
        const auto& L = sp.fire_lights[i];
        for (float f : {L.x, L.y, L.z, L.reach, L.power}) mix(static_cast<std::uint32_t>(std::lround(f * 16.0f)));
    }
    if (key == acc_key && acc.size() == static_cast<std::size_t>(w) * h) return acc.data();
    acc_key = key;
    acc.assign(static_cast<std::size_t>(w) * h, 0.0f);
    for (int i = 0; i < sp.fire_light_count; ++i) {
        const auto& L = sp.fire_lights[i];
        light_box(sp, L, 0.62f * L.reach, [&](std::size_t k, float f) { acc[k] += L.power * f * f; });
    }
    return acc.data();
}
// A spell's light (ScenePixels::spell_lights) at world point (X, Y, Z): its
// intensity, as the fire's ((1 - d / reach)^2 x power), and in `tint` the
// lights' colours weighted by it.
float spell_light_at(const kol::ScenePixels& sp, float X, float Y, float Z, float* tint) {
    float I = 0.0f;
    tint[0] = tint[1] = tint[2] = 0.0f;
    for (int i = 0; i < sp.spell_light_count; ++i) {
        const auto& L = sp.spell_lights[i];
        const float d = std::sqrt(light_d2(L, X, Y, Z));
        if (d >= L.reach) continue;
        const float f = L.power * (1.0f - d / L.reach) * (1.0f - d / L.reach);
        I += f;
        tint[0] += f * L.r; tint[1] += f * L.g; tint[2] += f * L.b;
    }
    if (I > 0.0f) for (int ch = 0; ch < 3; ++ch) tint[ch] /= I;
    return I;
}
// ... on one pixel: as a lamp's light (torch_tint), in the light's colour:
// brighter by it, and toward its hue, the art's texture kept (the mockup's).
void spell_light_px(std::uint8_t* px, float I, const float* tint) {
    const float a = std::min(1.0f, I);
    if (a <= 0.0f) return;
    const float lum = 0.3f * px[0] + 0.55f * px[1] + 0.15f * px[2];
    for (int ch = 0; ch < 3; ++ch) {
        const float lit = px[ch] * (1.0f + 0.6f * a * tint[ch]);
        const float hue = lum * (0.6f + 0.7f * tint[ch]) * (1.0f + 0.4f * a);
        px[ch] = static_cast<std::uint8_t>(std::clamp(lit + (hue - lit) * 0.45f * a, 0.0f, 255.0f));
    }
}
// The spell lights' pools this frame, per scene pixel by its world place, and
// their colour (3 a pixel, in *tint_out); null when there are none.
const float* spell_light_pools(const kol::ScenePixels& sp, const float** tint_out) {
    *tint_out = nullptr;
    if (!sp.spell_light_count || !sp.xz || !sp.y || !sp.kind) return nullptr;
    const int w = sp.w, h = sp.h;
    static thread_local std::vector<float> acc, tint;
    acc.assign(static_cast<std::size_t>(w) * h, 0.0f);
    tint.assign(static_cast<std::size_t>(w) * h * 3, 0.0f);
    for (int i = 0; i < sp.spell_light_count; ++i) {
        const auto& L = sp.spell_lights[i];
        // (A sphere of `reach`, as fire_light_pools: down to its foot too.)
        light_box(sp, L, 0.62f * L.reach + L.y / 2.0f, [&](std::size_t k, float f0) {
            const float f = L.power * f0 * f0;
            acc[k] += f;
            tint[k * 3] += f * L.r; tint[k * 3 + 1] += f * L.g; tint[k * 3 + 2] += f * L.b;
        });
    }
    *tint_out = tint.data();
    return acc.data();
}
// ... over the scene (not the interface).
void spell_light_scene(const kol::ScenePixels& sp, const std::uint8_t* iface, std::uint8_t* scene, const float* acc,
                       const float* tint) {
    if (!acc) return;
    par_px(sp, static_cast<std::size_t>(sp.w) * sp.h, [&](std::size_t k) {
        if (acc[k] <= 0.0f || iface[k]) return;
        const float t[3] = {tint[k * 3] / acc[k], tint[k * 3 + 1] / acc[k], tint[k * 3 + 2] / acc[k]};
        spell_light_px(scene + k * 3, acc[k], t);
    });
}

// The fire light over the scene: the pools (on a dark map, the night,
// they have brought the darkness back up already: here only the warmth,
// at half), and an arrow's halo about it on screen.
void fire_light_scene(const kol::ScenePixels& sp, const std::uint8_t* iface, std::uint8_t* scene, const float* acc) {
    if (!acc) return;
    const int w = sp.w, h = sp.h;
    for (int i = 0; i < sp.fire_light_count; ++i) {
        const auto& L = sp.fire_lights[i];
        float sx, sy;
        light_screen(sp, L.x, L.y, L.z, &sx, &sy);
        if (L.halo > 0.0f)   // the arrow's halo, on everything about it
            kol::for_halo(sx, sy, L.halo, w, h, 0.0f, [&](int px, int py, float rr) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (iface[k]) return;
                const float a = 0.55f * (1.0f - rr) * (1.0f - rr);
                std::uint8_t* q = scene + k * 3;
                q[0] = static_cast<std::uint8_t>(std::min(255.0f, q[0] + a * 255.0f));
                q[1] = static_cast<std::uint8_t>(std::min(255.0f, q[1] + a * 150.0f));
                q[2] = static_cast<std::uint8_t>(std::min(255.0f, q[2] + a * 45.0f));
            });
    }
    const float warm = sp.dark < 1.0f ? 0.5f : 1.0f;
    par_px(sp, static_cast<std::size_t>(w) * h, [&](std::size_t k) {
        if (acc[k] > 0.0f && !iface[k]) fire_light_px(scene + k * 3, acc[k] * warm);
    });
}

// How far plants lean in the wind, in scene pixels 32 rows over their foot:
// 2 with Options > Plant Sway (the user's pick between the 1.5 and 3
// previews, 2026-10-14), 0 off. KOL_PROP_SWAY=<px> overrides either way.
// Options > Special Effects: the game's fire and smoke drawn by effects of
// our own (kol_focus makes their squares, SpriteBlock::effect), a texel at a
// time from the scene pixel (x, y) its centre or corner lies at, the time t
// in seconds: premultiplied colour (0..1) and alpha, drawn see-through
// (PresentWarpMesh::alpha). Fire: tongues of noise rising from the foot,
// narrowing as they go, on a ramp from dark red through orange and yellow to
// a white core, flickering. Smoke: a plume widening and drifting as it
// rises, its density noise rolling up, fading in over its foot and out at
// its top, a warm grey.
float fx_hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}
float fx_noise(float x, float y) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = fx_hash(xi, yi), b = fx_hash(xi + 1, yi), c = fx_hash(xi, yi + 1), d = fx_hash(xi + 1, yi + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
float fx_fbm(float x, float y) {   // two octaves (three cost the Thor's frame in the burning town)
    return (0.5f * fx_noise(x, y) + 0.25f * fx_noise(x * 2.03f, y * 2.03f)) / 0.75f;
}
float fx_smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// rgb (premultiplied, 0..1) and alpha of block b's effect at scene (x, y).
void effect_texel(const kol::ScenePixels::SpriteBlock& b, float x, float y, float t, float rgb[3], float* alpha) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    *alpha = 0.0f;
    const float seed = static_cast<float>(b.fx_seed) * 0.37f;
    const float u = (x - b.fx_cx) / (16.0f * b.fx_width);
    if (b.effect == 1) {   // fire
        const float hgt = std::max(16.0f, b.foot_row - b.fx_top);
        const float v = (b.foot_row - y) / hgt;
        if (v < -0.25f || v > 1.0f) return;
        // Its base: not a line but ragged, flickering along it, and lower in
        // the middle (a flame on the diamond of a cell's top).
        const float base = 0.12f * std::fabs(u) - 0.1f +
                           0.16f * (fx_noise(u * 5.0f + seed, t * 1.8f) - 0.5f) +
                           0.08f * (fx_noise(u * 11.0f - seed, t * 3.5f) - 0.5f);
        // Tongues: the shape narrowing upward, eaten by two layers of noise
        // rolling up at different speeds, swaying.
        const float width = 0.95f * std::pow(std::max(0.0f, 1.0f - v), 0.75f) + 0.03f;
        if (std::fabs(u) > width + 0.2f) return;   // past the widest sway: nothing (no noise worked out)
        const float sway = 0.12f * v * std::sin(v * 4.0f - t * 2.7f + seed) +
                           0.08f * (fx_noise(v * 3.0f - t * 1.7f, seed) - 0.5f);
        const float d = std::fabs(u + sway) / width;
        if (d >= 1.0f) return;
        const float n1 = fx_fbm(u * 4.2f + seed, v * 4.4f - t * 3.0f);
        const float n2 = fx_noise(u * 9.0f - seed, v * 10.0f - t * 5.2f);
        const float shape = (1.0f - d * d) * (1.15f - v);
        float f = shape * 1.45f - (1.0f - n1) * 0.8f - (1.0f - n2) * 0.35f * (0.3f + v);
        f *= fx_smooth(base - 0.03f, base + 0.09f, v);
        f *= 0.92f + 0.08f * std::sin(t * 17.0f + seed * 5.0f + u * 3.0f);
        f = std::clamp(f, 0.0f, 1.0f);
        f *= b.fx_heat;
        static const float kRamp[5][3] = {{0.3f, 0.04f, 0.0f}, {0.92f, 0.22f, 0.02f}, {1.0f, 0.5f, 0.05f},
                                          {1.0f, 0.76f, 0.22f}, {1.0f, 0.92f, 0.6f}};
        // Deeper (fx_palette 1): a campfire on ground its own light makes
        // yellow (map 63) stands out red and orange, its pale tip small, and
        // its body opaque sooner.
        static const float kDeep[5][3] = {{0.22f, 0.02f, 0.0f}, {0.7f, 0.08f, 0.02f}, {0.95f, 0.25f, 0.03f},
                                          {1.0f, 0.5f, 0.06f}, {1.0f, 0.78f, 0.3f}};
        // Green (fx_palette 2): map 83's torches, their flames green in the
        // art ((156, 222, 0), (247, 239, 16), (247, 255, 222)), opaque as
        // soon as the campfire's.
        static const float kGreen[5][3] = {{0.02f, 0.14f, 0.0f}, {0.22f, 0.55f, 0.02f}, {0.55f, 0.86f, 0.0f},
                                           {0.9f, 0.93f, 0.08f}, {0.97f, 1.0f, 0.85f}};
        // Blue (fx_palette 3): map 56's torch stands, from their art's flame ((33, 173, 255),
        // (90, 247, 255), (239, 239, 255), (255, 255, 239)).
        static const float kBlue[5][3] = {{0.0f, 0.12f, 0.38f}, {0.06f, 0.45f, 0.92f}, {0.13f, 0.68f, 1.0f},
                                          {0.35f, 0.97f, 1.0f}, {0.96f, 0.98f, 1.0f}};
        const auto& ramp = b.fx_palette == 3 ? kBlue : b.fx_palette == 2 ? kGreen : b.fx_palette == 1 ? kDeep : kRamp;
        const float r = std::min(3.999f, f * 4.6f);
        const int i = static_cast<int>(r);
        const float k = r - static_cast<float>(i);
        const float a = b.fx_palette >= 1 ? fx_smooth(0.03f, 0.12f, f) : fx_smooth(0.05f, 0.18f, f);
        for (int ch = 0; ch < 3; ++ch) rgb[ch] = (ramp[i][ch] + (ramp[i + 1][ch] - ramp[i][ch]) * k) * a;
        *alpha = a * 0.95f;
    } else {   // smoke
        const float hgt = std::max(24.0f, b.foot_row - b.fx_top);
        const float v = (b.foot_row - y) / hgt;
        if (v < 0.0f || v > 1.0f) return;
        // Wisps: a thin plume widening and drifting, its density noise
        // stretched along the rise and rolling up, thinned by a threshold.
        if (std::fabs(u) > 0.2f + 0.7f * v + 0.3f * v + 0.06f) return;   // past the widest drift
        // Its drift: no swing of the whole plume, only slow turbulence
        // carried up with it (the noise's phase is the height less the
        // time), leaning the higher wisps a little.
        const float off = 0.32f * v * (fx_fbm(seed + 3.0f, v * 1.6f - t * 0.35f) - 0.5f) + 0.06f * v;
        const float width = 0.2f + 0.7f * v;
        const float d = std::fabs(u - off) / width;
        if (d >= 1.0f) return;
        const float n = fx_fbm(u * 2.4f + seed, v * 2.2f - t * 0.5f);
        const float wisp = fx_smooth(0.25f, 0.65f, n);
        float dens = std::max(0.0f, 1.0f - d * d) * fx_smooth(0.0f, 0.1f, v) * (1.0f - fx_smooth(0.6f, 1.0f, v));
        dens *= 0.25f + 0.85f * wisp;
        const float a = std::clamp(dens * 0.75f, 0.0f, 0.7f);
        const float shade = 0.5f + 0.18f * wisp;   // a warm grey, lighter where thick
        rgb[0] = shade * a;
        rgb[1] = shade * 0.94f * a;
        rgb[2] = shade * 0.84f * a;
        *alpha = a;
    }
}

float prop_sway() {
    static const float env = kol::env_num("KOL_PROP_SWAY", -1.0f);
    if (env >= 0.0f) return env;
    return kol::setting_on(kol::Setting::PlantSway) ? 2.0f : 0.0f;
}
// In rain the wind blows harder (asked 2026-10-23 for plant props to move a
// bit more in heavy rain): at the game's heaviest rain
// (ScenePixels::rain 1) plants lean kRainLean times as far and the wind's
// swing and gust come kRainQuick times as fast; between, by the rain's
// strength (its intensity 1 halfway). Eased over about a second each way, so
// it neither jumps when the rain starts nor flickers with the drops.
// KOL_RAIN_SWAY=<lean>,<quick> (1,1 none).
float g_rain_ease = 0.0f;
void rain_sway(float rain, float* lean, float* quick) {
    static float kRainLean = 1.75f, kRainQuick = 1.6f;   // (quick 1.3 first, then a little faster)
    static const bool read = [] {
        if (const char* e = std::getenv("KOL_RAIN_SWAY")) std::sscanf(e, "%f,%f", &kRainLean, &kRainQuick);
        return true;
    }();
    (void)read;
    g_rain_ease += std::clamp(rain - g_rain_ease, -1.0f / 60.0f, 1.0f / 60.0f);
    *lean = 1.0f + (kRainLean - 1.0f) * g_rain_ease;
    *quick = 1.0f + (kRainQuick - 1.0f) * g_rain_ease;
}

// Sunlight through the map's windows on one pixel (kol_focus, KOL_GODRAYS):
// `patch` 0..255 the sun on it through a window, taken back from the
// wall's shade a little warm; `haze` 0..255 the beams in front of it, added.
// `bright`: the scene's fade (ScenePixels::scene_bright). The sun on the
// art scales its colours and fades with them; the haze is added, so it is
// faded here (it stayed lit while the castle room went to black).
// `keep_hue` (a unit's own pixels): a warm light brightens the three
// channels alike and adds a warm cast by the pixel's brightness, scaled
// down as a whole where it would pass full: the warm tint multiplied in
// took red up 1.87x and blue 1.39x, and Nichart's purple armour went
// magenta in map 67's torchlight (the user, states/thor/0055).
// KOL_UNIT_WARM_TINT=1: tinted as the ground (as before).
// `keep_cool`: a cool light (the moon's on a unit) keeps the hue too.
// A map's own windows' light on units keeps it too when cool (map 68's
// moon: the girl's purple dress went white in it, the cool light pulling
// her colours to its own pale blue; the user reported the girl looked overblown).
// KOL_UNIT_COOL_PULL=1: as before.
const bool unit_cool_keep = std::getenv("KOL_UNIT_COOL_PULL") == nullptr;
void window_light(std::uint8_t* c, unsigned patch, unsigned haze, float s, float bright, const float* tint,
                  bool keep_hue = false, bool keep_cool = false) {
    // The map's (ScenePixels::win_tint): yellow-orange, the user's pick (C in
    // shots/godrays/sheet_tame_warm.png; first a pale 1, 0.93, 0.78), or
    // moonlight (map 65). KOL_GODRAYS_TINT=<r,g,b> for mockups.
    static float env[3] = {-1.0f, 0.0f, 0.0f};
    static const bool tint_read = [] {
        if (const char* e = std::getenv("KOL_GODRAYS_TINT"))
            std::sscanf(e, "%f,%f,%f", &env[0], &env[1], &env[2]);
        return true;
    }();
    (void)tint_read;
    const float* kSun = env[0] >= 0.0f ? env : tint;
    if (patch) {
        const float a = std::min(1.0f, s * static_cast<float>(patch) / 255.0f);
        // A cool light (moonlight, map 65): the patch also pulled toward its
        // own brightness in the light's colour, as the torches warm what they
        // light; brightened alone, the floor's yellow art stayed yellow under
        // a blue moon (the user chose a blue tint). KOL_MOON_PATCH=<0..1>.
        static const float kMoonPull = kol::env_num_clamp("KOL_MOON_PATCH", 0.0f, 1.0f, 0.7f);
        const bool cool = kSun[2] > kSun[0];
        const float lum = 0.3f * c[0] + 0.55f * c[1] + 0.15f * c[2];
        static const bool unit_warm_tint = std::getenv("KOL_UNIT_WARM_TINT") != nullptr;
        if (keep_hue && (!cool || keep_cool) && !unit_warm_tint) {
            const float mean = (kSun[0] + kSun[1] + kSun[2]) / 3.0f;
            const float gain = 1.0f + 0.75f * a * mean;
            float v[3], top = 0.0f;
            for (int ch = 0; ch < 3; ++ch) {
                v[ch] = std::max(0.0f, c[ch] * gain + lum * 0.75f * a * (kSun[ch] - mean) * 0.5f);
                top = std::max(top, v[ch]);
            }
            const float k = top > 255.0f ? 255.0f / top : 1.0f;
            for (int ch = 0; ch < 3; ++ch) c[ch] = static_cast<std::uint8_t>(std::lround(std::min(255.0f, v[ch] * k)));
        } else
        for (int ch = 0; ch < 3; ++ch) {
            float v = c[ch] * (1.0f + 0.75f * a * kSun[ch]);
            if (cool) v += (lum * (1.0f + 0.75f * a) * kSun[ch] * 1.25f - v) * kMoonPull * a;
            c[ch] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
        }
    }
    if (haze) {
        const float a = std::min(1.0f, s * static_cast<float>(haze) / 255.0f) * bright;
        for (int ch = 0; ch < 3; ++ch)
            c[ch] = static_cast<std::uint8_t>(std::min(255.0f, c[ch] + a * 150.0f * kSun[ch]));
    }
}

// The game's palette turned more slowly (ScenePixels::lag_pal; asked
// 2026-10-23 on map 102's narration, for a less abrupt change from orange
// to overcast). The game turns the map's palette (BG rows 0-3)
// from the day's browns to the storm's greys in about 50 frames at 39600
// (JJ), most of the picture's warmth gone in one second whatever our light
// does. Here a shown palette follows the game's by kPaletteLag of the way a
// frame (about 1.5 s to two thirds, 5 s to all of it), and every pixel in a
// colour of the game's palette is drawn in that entry's shown colour: the
// map's (BG0, BG1, the backdrop), its lifted props' and walls' squares and
// the ground under them. A colour two entries share takes the first's.
// Before anything is lit, so the light goes on the shown colours.
// KOL_PALETTE_LAG=<a frame's fraction> (0: none).
// Once the shown palette has caught up after the sun has set
// (ScenePixels::lag_sun_set) it is done: the lightning's flash before the
// first thunder brightens the palette for 40 frames, and lagged the map
// barely lit while its sprites flashed (JS, the user's slot 2; JJ too).
// KOL_PALETTE_LAG_FLASH=1: lagged until the window ends (old).
void palette_lag(kol::ScenePixels& sp, const std::uint8_t* layers, std::uint8_t* scene, std::size_t pixels) {
    static float shown[256][3];
    static bool have = false, done = false;
    static const float kPaletteLag = kol::env_num("KOL_PALETTE_LAG", 1.0f / 90.0f);
    static const bool lag_flash = std::getenv("KOL_PALETTE_LAG_FLASH") != nullptr;
    if (!sp.lag_pal || kPaletteLag <= 0.0f) { have = done = false; return; }
    if (done) return;
    float game[256][3];
    for (int i = 0; i < 256; ++i) {
        const unsigned c = sp.lag_pal[i * 2] | (sp.lag_pal[i * 2 + 1] << 8);
        game[i][0] = static_cast<float>(c & 31u);
        game[i][1] = static_cast<float>((c >> 5) & 31u);
        game[i][2] = static_cast<float>((c >> 10) & 31u);
    }
    if (!have) {
        std::memcpy(shown, game, sizeof shown);
        have = true;
        return;
    }
    bool lagging = false;
    for (int i = 0; i < 256; ++i)
        for (int ch = 0; ch < 3; ++ch) {
            float& v = shown[i][ch];
            v += (game[i][ch] - v) * std::min(1.0f, kPaletteLag);
            if (std::fabs(game[i][ch] - v) < 0.05f) v = game[i][ch];
            else lagging = true;
        }
    if (!lagging) {
        done = sp.lag_sun_set && !lag_flash;
        return;
    }
    // The game's colour (5 bits a channel, the top of the scene's 8) to the
    // shown one, -1 where no entry has it.
    // (Stamped by the frame, not cleared: 32768 entries a frame.)
    static std::vector<std::int32_t> lut(32768);
    static std::vector<std::uint32_t> stamp(32768, 0);
    static std::uint32_t frame = 0;
    if (++frame == 0) { std::fill(stamp.begin(), stamp.end(), 0u); frame = 1; }
    auto expand = [](float v) { return static_cast<std::int32_t>(std::lround(std::clamp(v, 0.0f, 31.0f) * 255.0f / 31.0f)); };
    for (int i = 0; i < 256; ++i) {
        const unsigned c = (sp.lag_pal[i * 2] | (sp.lag_pal[i * 2 + 1] << 8)) & 0x7FFFu;
        if (stamp[c] == frame) continue;
        stamp[c] = frame;
        lut[c] = expand(shown[i][0]) | expand(shown[i][1]) << 8 | expand(shown[i][2]) << 16;
    }
    auto remap = [&](std::uint8_t* px) {
        const unsigned key = (px[0] >> 3) | (px[1] >> 3) << 5 | (px[2] >> 3) << 10;
        if (stamp[key] != frame) return;
        const std::int32_t v = lut[key];
        px[0] = static_cast<std::uint8_t>(v & 255);
        px[1] = static_cast<std::uint8_t>((v >> 8) & 255);
        px[2] = static_cast<std::uint8_t>((v >> 16) & 255);
    };
    par_px(sp, pixels, [&](std::size_t k) {
        const unsigned top = layers[k] & 0x0Fu;
        if (top <= 1u || top == 5u) remap(scene + k * 3);
    });
    if (sp.under_rgb) {
        static std::vector<std::uint8_t> under;
        under.assign(sp.under_rgb, sp.under_rgb + pixels * 3);
        par_px(sp, pixels, [&](std::size_t k) { remap(&under[k * 3]); });
        sp.under_rgb = under.data();
    }
    if (sp.atlas_rgb && sp.atlas_per_row > 0) {
        constexpr int S = kol::ScenePixels::kSpriteSlot;
        for (int bi = 0; bi < sp.block_count; ++bi) {
            const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
            if (!b.prop && !b.surface) continue;   // (units' squares: the sprites' own palette)
            const auto [ax0, ay0] = kol::slot_origin(b.slot, sp.atlas_per_row);
            for (int j = 0; j < std::min(b.h, S); ++j)
                for (int i = 0; i < std::min(b.w, S); ++i)
                    remap(sp.atlas_rgb + (static_cast<std::size_t>(ay0 + j) * sp.atlas_w + ax0 + i) * 3);
        }
    }
}

// The light under the game's whitening (ScenePixels::scene_white, y 0..1).
// The frame's colour c is the art's c0 whitened, c = c0 + (1 - c0) y, and the
// light was laid on c: lit = c L. Laid on c0 before the whitening instead it
// is y + (c - y) L, which is c - (c - y) (c - lit) / c: the same change of
// colour, by (c - y) / c of it. At y = 1 the pixel is c (white); at 0, lit.
// Without it the shadows and the occlusion stayed on the map as it faded to
// white (states/HW_slot3_cutscene_thunder, the fade after the thunder).
void under_white(std::uint8_t* px, const std::uint8_t* orig, float y) {
    const float yy = 255.0f * y;
    for (int ch = 0; ch < 3; ++ch) {
        const float c = orig[ch], lit = px[ch];
        if (c <= 0.0f) continue;
        const float v = c - std::max(0.0f, c - yy) * (c - lit) / c;
        px[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    }
}

// The flash's field over a pixel of layer `layer` (0-3 BG, 4 OBJ, 5 the
// backdrop), blended as the game blends it (ScenePixels::field_*): drawn
// flat from the frame, the whole picture lost its perspective for the
// flash's frames (states/HW_slot3_cutscene_thunder).
void field_px(const kol::ScenePixels& sp, std::uint8_t* px, unsigned layer) {
    const bool second = layer < 6u && ((sp.field_second >> layer) & 1u);
    for (int ch = 0; ch < 3; ++ch) {
        const int v = second ? (sp.field_eva * sp.field_rgb[ch] + sp.field_evb * px[ch]) / 16
                             : sp.field_rgb[ch];
        px[ch] = static_cast<std::uint8_t>(std::min(255, v));
    }
}

void field_all(const kol::ScenePixels& sp, std::uint8_t* scene, std::size_t pixels, bool backdrop) {
    if (sp.field_layer < 2) return;
    par_px(sp, pixels, [&](std::size_t k) {
        const unsigned layer = backdrop ? 5u : sp.scene_codes ? (sp.scene_codes[k] & 0x0Fu) : 0u;
        field_px(sp, scene + k * 3, layer);
    });
}

// A pixel of the frame that is HUD: BG2 or BG3 on top (or a sprite over
// them), but not the flash's field (ScenePixels::field_layer).
// With a spell's card (ScenePixels::spell_card) the light layers are its too:
// the effect goes up and grows with the warp as one picture. Apocalypse
// (map 57, states/thor/0238 ~395): its pillar on BG3 was on the card, its
// rings on BG2 drawn as light at their place, so the moved pillar had a black
// gap at each ring's rows. KOL_SPELL_CARD_NO_LIGHT=1: as before.
bool spell_card_takes_light(const kol::ScenePixels& sp) {
    static const bool off = std::getenv("KOL_SPELL_CARD_NO_LIGHT") != nullptr;
    static const bool spell_flat = std::getenv("KOL_PERSP_SPELL_FLAT") != nullptr;
    return sp.spell_card && !off && !spell_flat;
}
bool hud_pixel(const kol::ScenePixels& sp, std::uint8_t code) {
    const unsigned top = code & 0x0Fu, under = code >> 4;
    const bool card_light = spell_card_takes_light(sp);
    // A glow layer's pixel over a layer that is not a second target is not
    // blended by the game: drawn whole, HUD. Map 102's narration (Thor state
    // 0257): "Produced by QUEST" fades in and out on BG2 at BLDCNT 0x1344 (its
    // second targets BG0, BG1 and the sprites, not the backdrop), and its
    // letterbox bars, on BG2 over the backdrop, were taken for light too: black
    // light adds nothing, the moved picture showed through them for each fade
    // (the user, 2026-11-13, saw the black borders sometimes disappear).
    // Shaher's rays (0x334C) have the backdrop as a target: unchanged.
    // KOL_GLOW_OVER_ANY=1: as before.
    static const bool over_any = std::getenv("KOL_GLOW_OVER_ANY") != nullptr;
    auto blended = [&](unsigned l) {
        if (over_any || l > 5u) return true;
        const unsigned bit = l == 5u ? 0x20u : l == 4u ? 0x10u : (1u << l);
        return (sp.hud_glow_second & bit) != 0u;
    };
    auto hud = [&](unsigned l, unsigned below) {
        return (l == 2u || l == 3u) && static_cast<int>(l) != sp.field_layer &&
               (card_light || !((sp.hud_glow_layers >> l) & 1u) || !blended(below));
    };
    return hud(top, under) || (top == 4u && hud(under, 6u));   // (6: not known, blended)
}
// A sprite drawn flat as interface but the map's (map 93's birds, units in
// the sky drawn flat as map 97's gulls are) under the map's faded edge
// (kEdgeFadeMaps): faded as the map is, not drawn whole from the frame (the
// user, 2026-11-13, asked for sprites under the fade to be hidden by it). Not the HUD's sprites (a portrait over its box), not the hand.
// The scene keeps its colour (no ground under it) and the fade passes take
// it; past the art, over the sky, the mesh draws nothing there: hidden.
// KOL_EDGE_FADE_IFACE_FLAT=1: drawn whole, as before.
bool faded_iface(const kol::ScenePixels& sp, const std::uint8_t* layers, const std::uint8_t* floating, std::size_t k) {
    static const bool off = std::getenv("KOL_EDGE_FADE_IFACE_FLAT") != nullptr;
    return !off && sp.edge_fade && sp.edge_fade[k] < 255u && sp.iface && sp.iface[k] && !(floating && floating[k]) &&
           (layers[k] & 0x0Fu) == kol::kLayerObj && !hud_pixel(sp, layers[k]);
}

// The HUD layers' light (ScenePixels::hud_glow) over the moved picture: a
// see-through quad a run of pixels of one colour, after the rain's and the
// snow's triangles (PresentWarpMesh::over_*, straight alpha: the game adds
// the light, which this has no way to; at kHudGlowAlpha of its strongest
// channel a beam is nearly as bright over the black and lets a unit show
// through). KOL_HUD_GLOW_ALPHA=<0..1>.
std::vector<float> g_glow_xy;
std::vector<std::uint8_t> g_glow_rgba;
std::vector<int> g_glow_idx;
std::vector<int> g_glow_add_idx;   // the triangles that are added (PresentWarpMesh::over_add_from)
// A quad of the light, laid over or added. An added one has a corner of its
// own for each triangle: SDL's software renderer draws two triangles that
// share an edge as one rectangle, blended whatever the blend mode asked.
void glow_quad(float x0, float y0, float x1, float y1, const std::uint8_t* c, bool add) {
    const int v0 = static_cast<int>(g_glow_xy.size() / 2);
    if (!add) {
        g_glow_xy.insert(g_glow_xy.end(), {x0, y0, x1, y0, x1, y1, x0, y1});
        for (int v = 0; v < 4; ++v) g_glow_rgba.insert(g_glow_rgba.end(), c, c + 4);
        g_glow_idx.insert(g_glow_idx.end(), {v0, v0 + 1, v0 + 2, v0, v0 + 2, v0 + 3});
        return;
    }
    g_glow_xy.insert(g_glow_xy.end(), {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1});
    for (int v = 0; v < 6; ++v) g_glow_rgba.insert(g_glow_rgba.end(), c, c + 4);
    g_glow_add_idx.insert(g_glow_add_idx.end(), {v0, v0 + 1, v0 + 2, v0 + 3, v0 + 4, v0 + 5});
}

// Where the warp puts the point a window's light is about (ScenePixels::
// WindowLight::anchor, a disc's centre), and its scale there: the point of
// the picture under it, standing over the ground first seen at or below it
// (a unit's pixel has no height of its own: its middle is that far over the
// ground at its feet), placed as kol_rain places a point of the world. The light's shapes
// and the glow inside them are moved and scaled about it, so a ring the game
// draws round a unit stays round that unit, at its size, when the camera
// looks elsewhere. Frame pixels of the view; the flat view: where it is.
struct LightAnchor {
    bool on = false;
    float fx = 0.0f, fy = 0.0f;   // in the frame
    float px = 0.0f, py = 0.0f, s = 1.0f;   // in the picture
};
LightAnchor light_anchor(const kol::ScenePixels& sp, int w, int h, const kol::RainCam& cam) {
    LightAnchor la;
    const auto& wl = sp.window_light;
    if (!wl.on || !wl.anchor) return la;
    static const bool fixed = kol::env_set("KOL_WINDOW_LIGHT_FIXED");   // where the frame has it
    la.on = true;
    la.fx = 0.5f * (sp.vw - 240) + wl.ax;
    la.fy = 0.5f * (sp.vh - 160) + wl.ay;
    la.px = la.fx; la.py = la.fy;
    if (cam.flat || fixed) return la;
    const float col = la.fx + static_cast<float>(cam.vx), row = la.fy + static_cast<float>(cam.vy);
    float Y = cam.yref;
    if (sp.y && sp.kind) {
        const int ci = std::clamp(static_cast<int>(col), 0, w - 1);
        for (int r = std::max(0, static_cast<int>(row)); r < std::min(h, static_cast<int>(row) + 48); ++r) {
            const std::size_t k = static_cast<std::size_t>(r) * w + ci;
            if (sp.kind[k] != kol::kSceneFlat || (sp.lift && sp.lift[k])) continue;
            // (That ground, and the point as far over it as it is drawn: a
            // unit's middle over the ground at its feet.)
            Y = static_cast<float>(sp.y[k]) + 2.0f * static_cast<float>(r - std::max(0, static_cast<int>(row)));
            break;
        }
    }
    const float d = 4.0f * (row - cam.base) + 2.0f * Y;
    const float den = cam.K + cam.dref - d;
    float sc = den > 0.25f * cam.K ? cam.K / den : 4.0f;
    float x = cam.cxp + (col - cam.cxp) * sc;
    float y = cam.base + (cam.dref + (d - cam.dref) * sc * cam.pitch) * 0.25f - (cam.yref + (Y - cam.yref) * sc) * 0.5f;
    if (cam.zoom != 1.0f) {
        x = cam.cx + (x - cam.cx) * cam.zoom;
        y = cam.cy + (y - cam.cy) * cam.zoom;
        sc *= cam.zoom;
    }
    la.px = x - static_cast<float>(cam.vx);
    la.py = y - static_cast<float>(cam.vy);
    la.s = std::clamp(sc, 0.25f, 4.0f);
    return la;
}

// The field through the game's windows (ScenePixels::window_light) over the
// moved picture: a quad a run of each row of the view, where the row's place
// in the game's picture (back through the anchor) is in a part of the windows
// that shows the field. A disc is carried on past the game's 240 x 160 as the
// circle it is; a window of plain spans ends there (outside both).
void window_light_runs(const kol::ScenePixels& sp, const LightAnchor& la) {
    const auto& wl = sp.window_light;
    if (!wl.on || !(wl.show[0] | wl.show[1] | wl.show[2])) return;
    const float nx0 = 0.5f * (sp.vw - 240), ny0 = 0.5f * (sp.vh - 160);
    const float fx = la.on ? la.fx : 0.0f, fy = la.on ? la.fy : 0.0f, px = la.on ? la.px : 0.0f, py = la.on ? la.py : 0.0f;
    const float sc = la.on ? la.s : 1.0f;
    const std::uint8_t opaque[4] = {wl.rgb[0], wl.rgb[1], wl.rgb[2], 255};
    for (int y = 0; y < sp.vh; ++y) {
        const float ny = fy + (y + 0.5f - py) / sc - ny0;   // the row in the game's picture
        float iv[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};      // each window's run of this row, view pixels
        for (int wi = 0; wi < 2; ++wi) {
            const auto& win = wl.win[wi];
            if (!win.on) continue;
            float a = 0.0f, b = 0.0f;   // native columns
            // The game's line this row shows: as hud_glow_over rounds a line's
            // quad, so the glow inside a window meets the window's edge.
            int line = static_cast<int>(std::floor(ny));
            auto top = [&](int l) { return std::round(py + (l + ny0 - fy) * sc); };
            while (line > -2 && static_cast<float>(y) < top(line)) --line;
            while (line < 161 && static_cast<float>(y) >= top(line + 1)) ++line;
            const bool native = line >= 0 && line < 160;
            if (win.disc) {
                // In the game's lines its own spans (the disc where one runs
                // off the side); past them the circle.
                const float dy = (native ? line + 0.5f : ny) - win.cy, h2 = win.r * win.r - dy * dy;
                const bool span = native && win.x1[line] > win.x0[line];
                if (native && !span) continue;
                if (!native && h2 <= 0.0f) continue;
                const float hw = h2 > 0.0f ? std::sqrt(h2) : 0.0f;
                a = span && win.x0[line] > 0 ? win.x0[line] : win.cx - hw;
                b = span && win.x1[line] < 240 ? win.x1[line] : win.cx + hw;
                if (span && win.x0[line] == 0) a = std::min(a, 0.0f);
                if (span && win.x1[line] == 240) b = std::max(b, 240.0f);
            } else {
                if (!native || win.x1[line] <= win.x0[line]) continue;
                a = win.x0[line]; b = win.x1[line];
            }
            iv[wi][0] = std::clamp(std::round(px + (a + nx0 - fx) * sc), 0.0f, static_cast<float>(sp.vw));
            iv[wi][1] = std::clamp(std::round(px + (b + nx0 - fx) * sc), 0.0f, static_cast<float>(sp.vw));
        }
        float cut[6] = {0.0f, iv[0][0], iv[0][1], iv[1][0], iv[1][1], static_cast<float>(sp.vw)};
        std::sort(cut, cut + 6);
        float run0 = 0.0f;
        int run_show = -1;
        auto flush = [&](float x1) {
            if (run_show > 0 && x1 > run0) {
                const std::uint8_t* c = run_show == 2 ? opaque : wl.rgba;
                glow_quad(run0, static_cast<float>(y), x1, static_cast<float>(y) + 1.0f, c, run_show == 1 && wl.add);
            }
        };
        for (int i = 0; i + 1 < 6; ++i) {
            if (cut[i + 1] <= cut[i]) continue;
            const float m = 0.5f * (cut[i] + cut[i + 1]);
            const int part = m >= iv[0][0] && m < iv[0][1] ? 0 : m >= iv[1][0] && m < iv[1][1] ? 1 : 2;
            const int show = wl.show[part];
            if (show != run_show) { flush(cut[i]); run0 = cut[i]; run_show = show; }
        }
        flush(static_cast<float>(sp.vw));
    }
}

void hud_glow_over(const kol::ScenePixels& sp, gbarecomp::PresentWarpMesh* mesh, int w, int h, const kol::RainCam& cam) {
    const bool glow = sp.hud_glow && sp.hud_glow_n > 0 && !spell_card_takes_light(sp);   // (on the card)
    if (!glow && !sp.window_light.on) return;
    static const float kHudGlowAlpha = kol::env_num_clamp("KOL_HUD_GLOW_ALPHA", 0.0f, 1.0f, 0.75f);
    g_glow_xy.assign(mesh->over_xy, mesh->over_xy + (mesh->over_xy ? mesh->over_vertices * 2 : 0));
    g_glow_rgba.assign(mesh->over_rgba, mesh->over_rgba + (mesh->over_rgba ? mesh->over_vertices * 4 : 0));
    g_glow_idx.assign(mesh->over_indices, mesh->over_indices + (mesh->over_indices ? mesh->over_index_count : 0));
    g_glow_add_idx.clear();
    const LightAnchor la = light_anchor(sp, w, h, cam);
    window_light_runs(sp, la);
    // With the windows' light (the same blend), the glow is added as the
    // game adds it: its colour at EVA, whole.
    const bool glow_add = sp.window_light.on && sp.window_light.add;
    // The game's own area in the frame, and how far to carry the light on.
    const float nx0 = 0.5f * (sp.vw - 240), nx1 = nx0 + 240.0f, ny0 = 0.5f * (sp.vh - 160), ny1 = ny0 + 160.0f;
    float reach = 1.0f;
    if (sp.hud_glow_apex_ok) {
        const float ax = std::clamp(sp.hud_glow_apex[0], nx0 + 8.0f, nx1 - 8.0f), ay = std::clamp(sp.hud_glow_apex[1], ny0 + 8.0f, ny1 - 8.0f);
        reach = std::max({ax / (ax - nx0), (sp.vw - ax) / (nx1 - ax), ay / (ay - ny0), (sp.vh - ay) / (ny1 - ay)});
        reach = std::clamp(reach, 1.0f, 4.0f);
    }
    for (int i = 0; glow && i < sp.hud_glow_n;) {
        const kol::ScenePixels::HudGlowPx& p = sp.hud_glow[i];
        int j = i + 1;
        while (j < sp.hud_glow_n && sp.hud_glow[j].y == p.y && sp.hud_glow[j].x == p.x + (j - i) &&
               sp.hud_glow[j].r == p.r && sp.hud_glow[j].g == p.g && sp.hud_glow[j].b == p.b)
            ++j;
        const int mx = std::max({p.r, p.g, p.b});
        if (mx <= 0) { i = j; continue; }
        const std::uint8_t a = static_cast<std::uint8_t>(std::lround(kHudGlowAlpha * mx));
        std::uint8_t c[4] = {static_cast<std::uint8_t>(p.r * 255 / mx), static_cast<std::uint8_t>(p.g * 255 / mx),
                             static_cast<std::uint8_t>(p.b * 255 / mx), a};
        if (sp.hud_glow_add && !glow_add) {   // (a battle action: as the game adds it)
            c[0] = static_cast<std::uint8_t>(std::min(255, p.r * sp.hud_glow_eva / 16));
            c[1] = static_cast<std::uint8_t>(std::min(255, p.g * sp.hud_glow_eva / 16));
            c[2] = static_cast<std::uint8_t>(std::min(255, p.b * sp.hud_glow_eva / 16));
            c[3] = 255;
        }
        if (glow_add) {
            const int eva = sp.window_light.eva;
            c[0] = static_cast<std::uint8_t>(p.r * eva / 16); c[1] = static_cast<std::uint8_t>(p.g * eva / 16);
            c[2] = static_cast<std::uint8_t>(p.b * eva / 16); c[3] = 255;
        }
        float x0 = p.x, x1 = static_cast<float>(p.x + (j - i)), y0 = p.y, y1 = p.y + 1.0f;
        if (la.on) {
            // With the windows' light, about its anchor; on whole pixels (the
            // software renderer left gaps between quads at fractions).
            x0 = std::round(la.px + (x0 - la.fx) * la.s); x1 = std::round(la.px + (x1 - la.fx) * la.s);
            y0 = std::round(la.py + (y0 - la.fy) * la.s); y1 = std::round(la.py + (y1 - la.fy) * la.s);
            if (x1 <= x0 || y1 <= y0) { i = j; continue; }
        }
        glow_quad(x0, y0, x1, y1, c, glow_add || sp.hud_glow_add);
        // Carried on past the game's 240 x 160 (it draws these layers there
        // only; the user, 2026-11-10, asked for the beams to reach the screen's
        // end): the same run scaled about the point the light
        // fans out from, which leaves a beam from that point the same beam,
        // longer; drawn where it falls outside the game's area. The scale takes
        // the area's nearest side to the view's (at most 4).
        if (sp.hud_glow_apex_ok && reach > 1.01f) {
            const float ax = sp.hud_glow_apex[0], ay = sp.hud_glow_apex[1];
            const float sx0 = ax + (x0 - ax) * reach, sx1 = ax + (x1 - ax) * reach;
            const float sy0 = ay + (y0 - ay) * reach, sy1 = ay + (y1 - ay) * reach;
            const float mx2 = 0.5f * (sx0 + sx1), my2 = 0.5f * (sy0 + sy1);
            if (mx2 < nx0 || mx2 >= nx1 || my2 < ny0 || my2 >= ny1) {
                const int w0 = static_cast<int>(g_glow_xy.size() / 2);
                g_glow_xy.insert(g_glow_xy.end(), {sx0, sy0, sx1, sy0, sx1, sy1, sx0, sy1});
                for (int v = 0; v < 4; ++v) g_glow_rgba.insert(g_glow_rgba.end(), c, c + 4);
                g_glow_idx.insert(g_glow_idx.end(), {w0, w0 + 1, w0 + 2, w0, w0 + 2, w0 + 3});
            }
        }
        i = j;
    }
    mesh->over_xy = g_glow_xy.data();
    mesh->over_rgba = g_glow_rgba.data();
    mesh->over_vertices = static_cast<int>(g_glow_xy.size() / 2);
    if (!g_glow_add_idx.empty()) {
        mesh->over_add_from = static_cast<int>(g_glow_idx.size());
        g_glow_idx.insert(g_glow_idx.end(), g_glow_add_idx.begin(), g_glow_add_idx.end());
    }
    mesh->over_indices = g_glow_idx.data();
    mesh->over_index_count = static_cast<int>(g_glow_idx.size());
}

void under_white_all(const kol::ScenePixels& sp, std::uint8_t* scene, const std::vector<std::uint8_t>& orig,
                     std::size_t pixels, float y) {
    if (y <= 0.0f || orig.size() < pixels * 3) return;
    par_px(sp, pixels, [&](std::size_t k) { under_white(scene + k * 3, &orig[k * 3], y); });
}

// A dark map's wet floor and the sun's bumps (apply_lighting): the terms
// its steps read a pixel at a time, wet_bump and wet_spec.
// Worked out once a view (the pools' key, sp.glow_key: the pools are
// cached the same way), where a lamp's pool reaches (sp.glow), from the
// art's own colours, read here before any light changes them: per pixel
// the stone's bump term (wet_bump: N.L - L.z, the light's change from a
// flat floor's) and the slime's highlight before the light (wet_spec).
// Done every frame, the bumps' normals and the flame's direction cost
// about 1 ms a job at 480x270 (46k lit pixels on map 51, GS).
// `sun_l`: the sun's direction on screen (with `sun_on`).
void wet_floor_terms(const kol::ScenePixels& sp, const std::uint8_t* scene, std::size_t pixels, bool slime_on,
                     bool sun_on, const float (&sun_l)[3], std::vector<float>& wet_bump, std::vector<float>& wet_spec) {
    static std::uint32_t wet_key = 0;
    static std::size_t wet_pixels = 0;
    static const float bump_depth = kol::env_num("KOL_SLIME_DEPTH", 4.0f);
    static const float shin = kol::env_num_min("KOL_SLIME_SHINE", 1.0f, 24.0f);
    std::uint32_t art_key = sp.glow_key;
    if (sun_on)
        for (float v : sun_l) {
            std::uint32_t u;
            std::memcpy(&u, &v, sizeof u);
            kol::fnv_mix(art_key, u);
        }
    // (On a sample of the art itself too, every 13th pixel: the range's
    // tiles are drawn into the map, and bumps worked out under them stayed
    // on the floor after them.)
    if (slime_on || sun_on)
        for (std::size_t k = 0; k < pixels; k += 13)
            kol::fnv_mix(art_key, scene[k * 3] | scene[k * 3 + 1] << 8 | scene[k * 3 + 2] << 16);
    if (!(slime_on || sun_on) || (art_key == wet_key && wet_pixels == pixels)) return;
    wet_key = art_key;
    const bool same_size = wet_pixels == pixels && wet_bump.size() == pixels;
    wet_pixels = pixels;
    const int W = sp.w;
    auto lum = [&](std::size_t q) {
        const std::uint8_t* c = scene + q * 3;
        return (0.3f * c[0] + 0.55f * c[1] + 0.15f * c[2]) * (1.0f / 255.0f);
    };
    // The heights softened first, a box of KOL_BUMP_BLUR px (1) each
    // way, twice, so the slopes follow shapes (a cobble), not single
    // pixels: from the art's brightness unsoftened, the bump was an
    // emboss on every pixel (the user said it looked like a sharpening
    // pass; measured on map 89's street, the change was two
    // thirds pixel-to-pixel contrast; softened by 1, about half, the
    // user's pick after 2). The slopes x KOL_BUMP_DEPTH (4 + 4 a px of
    // softening: 8; 12 at 2, 4 unsoftened) to keep the relief. The
    // slime's glints keep the sharp bumps (the unsoftened heights x
    // KOL_SLIME_DEPTH, 4): they are what sparkles. Box sums by running
    // totals: the cost does not grow with the radius.
    // (A panel's bumps, ScenePixels::bump_boxed, from the heights as they are, not
    // softened: softened, a carving's fine lines ran together into blobs as its strength
    // went up; the user, map 56, noted the relief lost detail.)
    static const int hblur_env = kol::env_int_clamp("KOL_BUMP_BLUR", 0, 8, 1);
    static const float soft_depth_env = kol::env_num("KOL_BUMP_DEPTH", -1.0f);
    const int hblur = sp.bump_boxed ? 0 : hblur_env;
    const float soft_depth = soft_depth_env >= 0.0f ? soft_depth_env : 4.0f + 4.0f * static_cast<float>(hblur);
    // Only the rows a lamp's pool reaches (with the blur's reach round
    // them) on a dark map; every row in the sun.
    int ry0 = 0, ry1 = sp.h - 1;
    if (!sun_on) {
        ry0 = sp.h; ry1 = -1;
        for (int y = 0; y < sp.h; ++y) {
            const std::uint8_t* gl = sp.glow + static_cast<std::size_t>(y) * W;
            for (int x = 0; x < W; ++x)
                if (gl[x]) { ry0 = std::min(ry0, y); ry1 = y; break; }
        }
        ry0 = std::max(0, ry0 - 2 * hblur - 2);
        ry1 = std::min(sp.h - 1, ry1 + 2 * hblur + 2);
    }
    // The wet floor's stones by their shape, not their brightness
    // (asked 2026-10-15 for alternatives to the bump map; of mockups A
    // today, B stone shapes, C shapes and relief shadows, the user took
    // C at a lower strength; shots/bump_alternatives/): a pixel darker
    // than its neighbourhood (x KOL_BUMP_MORTAR, 0.82, of a 5x5 mean) is
    // mortar or a crack, height 0; a stone rises as a rounded dome with
    // the distance to it, KOL_BUMP_CAP px (2.5) at most, its slopes x
    // KOL_BUMP_SHAPE (1.5). Brightness alone made dark moss a crack and
    // embossed every pixel. KOL_RELIEF (0.8): each pixel marched toward
    // its flame, up to 10 px; a stone higher than the ray climbing to the
    // flame (20 px up over the distance) shades it, so the shadows grow
    // away from the pot. KOL_BUMP_SHAPE=0 KOL_RELIEF=0: the brightness
    // bump of before. Dark maps only (not the sun's bump).
    static const float shape_depth_env = kol::env_num("KOL_BUMP_SHAPE", 1.5f);
    static const float relief_amt_env = kol::env_num("KOL_RELIEF", 0.8f);
    // (A plain bump map, kPlainBumpMaps: the brightness alone.)
    const float shape_depth = sp.bump_plain ? 0.0f : shape_depth_env;
    const float relief_amt = sp.bump_plain ? 0.0f : relief_amt_env;
    static std::vector<float> shp;
    const bool shape_on = (shape_depth > 0.0f || relief_amt > 0.0f) && !sun_on && ry1 >= ry0;
    // Heights as integers (brightness x 64, 0..16320), so the box sums
    // are integer adds.
    static std::vector<std::int32_t> raw, hgt, htmp, colsum;
    raw.resize(pixels);
    for (int y = ry0; y <= ry1; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t q = static_cast<std::size_t>(y) * W + x;
            const std::uint8_t* c = scene + q * 3;
            raw[q] = (77 * c[0] + 141 * c[1] + 38 * c[2]) >> 2;   // (0.3, 0.55, 0.15) x 256 / 4
        }
    const std::int32_t* soft = raw.data();
    float soft_scale = 1.0f / (64.0f * 255.0f);
    if (hblur > 0 && ry1 >= ry0 && !(shape_on && shape_depth > 0.0f)) {
        const int r = hblur;
        const int n = 2 * r + 1;
        hgt.resize(pixels);
        htmp.resize(pixels);
        colsum.resize(static_cast<std::size_t>(W));
        const std::int32_t* src = raw.data();
        for (int pass = 0; pass < 2; ++pass) {
            // Along the rows: src -> htmp, the edges' missing pixels as
            // the edge pixel (sums of n, no division until the end).
            for (int y = ry0; y <= ry1; ++y) {
                const std::int32_t* row = src + static_cast<std::size_t>(y) * W;
                std::int32_t* out = &htmp[static_cast<std::size_t>(y) * W];
                std::int32_t sum = row[0] * (r + 1);
                for (int q = 1; q <= r; ++q) sum += row[std::min(W - 1, q)];
                for (int x = 0; x < W; ++x) {
                    out[x] = sum;
                    sum += row[std::min(W - 1, x + r + 1)] - row[std::max(0, x - r)];
                }
            }
            // Down the columns: htmp -> hgt, a running sum per column.
            std::int32_t* dst = hgt.data();
            auto row_of = [&](int y) { return &htmp[static_cast<std::size_t>(std::clamp(y, ry0, ry1)) * W]; };
            for (int x = 0; x < W; ++x) colsum[x] = 0;
            for (int q = ry0 - r; q <= ry0 + r; ++q) {
                const std::int32_t* rw = row_of(q);
                for (int x = 0; x < W; ++x) colsum[x] += rw[x];
            }
            for (int y = ry0; y <= ry1; ++y) {
                std::int32_t* out = dst + static_cast<std::size_t>(y) * W;
                // (Two passes of n x n: divided back each pass, so the
                // sums stay in range.)
                for (int x = 0; x < W; ++x) out[x] = colsum[x] / (n * n);
                const std::int32_t* add = row_of(y + r + 1);
                const std::int32_t* sub = row_of(y - r);
                for (int x = 0; x < W; ++x) colsum[x] += add[x] - sub[x];
            }
            src = dst;
        }
        soft = hgt.data();
    }
    int cx0 = W, cx1 = -1;
    std::function<void(int, int, int, int, int, int, int, int)> heights;
    float cap = 2.5f;
    if (shape_on) {
        static const float mortar_k = kol::env_num("KOL_BUMP_MORTAR", 0.82f);
        static const float cap_env = kol::env_num("KOL_BUMP_CAP", 2.5f);
        cap = cap_env;
        // Only the columns the pools reach (and 6 round them), in the
        // rows found above: worked out again every frame the view moves.
        for (int y = ry0; y <= ry1; ++y) {
            const std::uint8_t* gl = sp.glow + static_cast<std::size_t>(y) * W;
            int x = 0;
            while (x < cx0 && !gl[x]) ++x;
            cx0 = std::min(cx0, x);
            x = W - 1;
            while (x > cx1 && !gl[x]) --x;
            cx1 = std::max(cx1, x);
        }
        cx0 = std::max(0, cx0 - 6);
        cx1 = std::min(W - 1, cx1 + 6);
        static std::vector<std::int32_t> rsum;
        static std::vector<std::uint16_t> dist;   // chamfer, 2 a step, 3 a diagonal
        static std::vector<float> tmp;
        if (shp.size() != pixels) shp.assign(pixels, 0.0f);
        rsum.resize(pixels);
        dist.resize(pixels);
        tmp.resize(pixels);
        // The dome by distance (half px), a table: cap (1 - (1 - t)^2).
        float dome[16];
        for (int i = 0; i < 16; ++i) {
            const float t = std::min(i * 0.5f / cap, 1.0f);
            dome[i] = cap * (1.0f - (1.0f - t) * (1.0f - t));
        }
        static std::vector<float> sft;
        sft.resize(pixels);
        // The heights of the rect (oX0..oX1, oY0..oY1), worked out over
        // the context (X0..X1, Y0..Y1) round it: the mean reaches 2 px,
        // the dome 3, the softening 1.
        heights = [&, dome](int X0, int X1, int Y0, int Y1, int oX0, int oX1, int oY0, int oY1) {
            // The 5x5 mean: along the rows, then down the columns (edges
            // clamped); darker than x KOL_BUMP_MORTAR of it: mortar.
            for (int y = Y0; y <= Y1; ++y) {   // running sums along the rows
                const std::int32_t* row = &raw[static_cast<std::size_t>(y) * W];
                std::int32_t* out = &rsum[static_cast<std::size_t>(y) * W];
                std::int32_t a = 3 * row[X0] + row[std::min(X1, X0 + 1)] + row[std::min(X1, X0 + 2)];
                for (int x = X0; x <= X1; ++x) {
                    out[x] = a;
                    a += row[std::min(X1, x + 3)] - row[std::max(X0, x - 2)];
                }
            }
            // Down the columns, a running sum per column; mortar where
            // 25 raw < KOL_BUMP_MORTAR x the sum (x 1024, integers).
            const std::int32_t thr = static_cast<std::int32_t>(mortar_k * 1024.0f);
            constexpr std::uint16_t kFar = 60000;
            static std::vector<std::int32_t> csum;
            csum.assign(static_cast<std::size_t>(W), 0);
            auto rrow = [&](int y) { return &rsum[static_cast<std::size_t>(std::clamp(y, Y0, Y1)) * W]; };
            for (int dy = -2; dy <= 2; ++dy) {
                const std::int32_t* rw = rrow(Y0 + dy);
                for (int x = X0; x <= X1; ++x) csum[x] += rw[x];
            }
            for (int y = Y0; y <= Y1; ++y) {
                const std::size_t o = static_cast<std::size_t>(y) * W;
                for (int x = X0; x <= X1; ++x)
                    dist[o + x] = raw[o + x] * 25 * 1024 < thr * csum[x] ? 0 : kFar;
                const std::int32_t* add = rrow(y + 3);
                const std::int32_t* sub = rrow(y - 2);
                for (int x = X0; x <= X1; ++x) csum[x] += add[x] - sub[x];
            }
            // Chamfer distance, forward and back.
            auto relax = [](std::uint16_t& d, std::uint16_t o, int w) {
                if (o + w < d) d = static_cast<std::uint16_t>(o + w);
            };
            for (int y = Y0; y <= Y1; ++y)
                for (int x = X0; x <= X1; ++x) {
                    const std::size_t q = static_cast<std::size_t>(y) * W + x;
                    std::uint16_t d = dist[q];
                    if (x > X0) relax(d, dist[q - 1], 2);
                    if (y > Y0) {
                        relax(d, dist[q - W], 2);
                        if (x > X0) relax(d, dist[q - W - 1], 3);
                        if (x < X1) relax(d, dist[q - W + 1], 3);
                    }
                    dist[q] = d;
                }
            for (int y = Y1; y >= Y0; --y)
                for (int x = X1; x >= X0; --x) {
                    const std::size_t q = static_cast<std::size_t>(y) * W + x;
                    std::uint16_t d = dist[q];
                    if (x < X1) relax(d, dist[q + 1], 2);
                    if (y < Y1) {
                        relax(d, dist[q + W], 2);
                        if (x < X1) relax(d, dist[q + W + 1], 3);
                        if (x > X0) relax(d, dist[q + W - 1], 3);
                    }
                    dist[q] = d;
                    tmp[q] = dome[std::min<int>(d, 15)];
                }
            // Softened once (3x3, by rows then columns), so the domes'
            // slopes are not stairs; the context's edges as they are.
            for (int y = Y0; y <= Y1; ++y) {
                const std::size_t o = static_cast<std::size_t>(y) * W;
                sft[o + X0] = tmp[o + X0];
                sft[o + X1] = tmp[o + X1];
                for (int x = X0 + 1; x < X1; ++x) sft[o + x] = tmp[o + x - 1] + tmp[o + x] + tmp[o + x + 1];
            }
            for (int x = X0 + 1; x < X1; ++x) {
                float up = sft[static_cast<std::size_t>(Y0) * W + x], mid = sft[static_cast<std::size_t>(std::min(Y0 + 1, Y1)) * W + x];
                sft[static_cast<std::size_t>(Y0) * W + x] = tmp[static_cast<std::size_t>(Y0) * W + x];
                for (int y = Y0 + 1; y < Y1; ++y) {
                    const std::size_t q = static_cast<std::size_t>(y) * W + x;
                    const float down = sft[q + W];
                    sft[q] = (up + mid + down) * (1.0f / 9.0f);
                    up = mid; mid = down;
                }
                sft[static_cast<std::size_t>(Y1) * W + x] = tmp[static_cast<std::size_t>(Y1) * W + x];
            }
            for (int y = oY0; y <= oY1; ++y)
                std::memcpy(&shp[static_cast<std::size_t>(y) * W + oX0], &sft[static_cast<std::size_t>(y) * W + oX0],
                            sizeof(float) * static_cast<std::size_t>(oX1 - oX0 + 1));
        };
    }
    auto normal = [&](const std::int32_t* hh, std::size_t k, float depth, float* n) {
        const float d = depth * soft_scale;
        float nx = -static_cast<float>(hh[k + 1] - hh[k - 1]) * d, ny = -static_cast<float>(hh[k + W] - hh[k - W]) * d;
        const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
        n[0] = nx * inv; n[1] = ny * inv; n[2] = inv;
    };
    // A pixel's terms: its normal, the light's change from a flat floor's
    // (and its relief shadow), the slime's glint.
    auto terms = [&](int x, int y) {
        const std::size_t k = static_cast<std::size_t>(y) * W + x;
        wet_bump[k] = 0.0f;
        wet_spec[k] = 0.0f;
        if (!sun_on && !sp.glow[k]) return;
        float n[3];
        if (shape_on && shape_depth > 0.0f) {
            float nx = -(shp[k + 1] - shp[k - 1]) * 0.5f * shape_depth;
            float ny = -(shp[k + W] - shp[k - W]) * 0.5f * shape_depth;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
            n[0] = nx * inv; n[1] = ny * inv; n[2] = inv;
        } else {
            normal(soft, k, shape_on ? 0.0f : soft_depth, n);
        }
        if (sun_on) {
            wet_bump[k] = n[0] * sun_l[0] + n[1] * sun_l[1] + n[2] * sun_l[2] - sun_l[2];
            return;
        }
        // The flame its light is mostly from: its place on screen,
        // 20 px up out of the floor (at rest; the lean is a pixel).
        const unsigned li = sp.glow_lamp[k];
        float lx = sp.glow_fx[li] - x, ly = sp.glow_fy[li] - y, lz = 20.0f;
        const float ll = 1.0f / std::sqrt(lx * lx + ly * ly + lz * lz);
        lx *= ll; ly *= ll; lz *= ll;
        wet_bump[k] = n[0] * lx + n[1] * ly + n[2] * lz - lz;
        if (shape_on && relief_amt > 0.0f) {
            // March toward the flame on screen; the ray climbs
            // 20 px over the whole distance to it.
            const float sx = sp.glow_fx[li] - x, sy = sp.glow_fy[li] - y;
            const float dl = std::sqrt(sx * sx + sy * sy);
            if (dl > 1.0f) {
                const float ux = sx / dl, uy = sy / dl, climb = 20.0f / dl;
                const float h0 = shp[k];
                float occ = 0.0f;
                for (int t = 1; t <= 10 && t < dl; ++t) {
                    // (The step rounded alone, so a pixel slid by a pan
                    // samples as it would worked out afresh.)
                    const int xx = x + static_cast<int>(ux * t + 0.5f + 1024.0f) - 1024;
                    const int yy = y + static_cast<int>(uy * t + 0.5f + 1024.0f) - 1024;
                    if (xx < cx0 || xx > cx1 || yy < ry0 || yy > ry1) break;
                    const float ray = h0 + climb * t;
                    if (ray >= cap) break;   // over every stone now
                    const float over = shp[static_cast<std::size_t>(yy) * W + xx] - ray;
                    occ = std::max(occ, std::min(1.0f, over));
                }
                wet_bump[k] -= relief_amt * occ;
            }
        }
        const std::uint8_t* c = scene + k * 3;
        // A plain bump map (kPlainBumpMaps): not the grey stone, light and
        // pale (spread under 48, brightness over 110: map 79's walkway, gate
        // and pillars); the green rock and moss, their dark crevices too
        // (the user asked to skip the grey and do only the green walls and
        // floor).
        // A panel's box alone (ScenePixels::bump_boxed): the pixel's world place from its
        // column and its X + Z; outside it no bump, inside it every colour.
        if (sp.bump_boxed) {
            const float a = 2.0f * static_cast<float>(static_cast<int>(k % static_cast<std::size_t>(W)) - sp.ox - sp.kx);
            const float bxz = static_cast<float>(sp.xz[k]);
            const float X = 0.5f * (a + bxz), Z = 0.5f * (bxz - a), Y = static_cast<float>(sp.y[k]);
            if (X < sp.bump_box[0] || X > sp.bump_box[3] || Y < sp.bump_box[1] || Y > sp.bump_box[4] ||
                Z < sp.bump_box[2] || Z > sp.bump_box[5])
                wet_bump[k] = 0.0f;
            return;
        }
        if (sp.bump_plain) {
            const int hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
            if (hi - lo < 48 && (77 * c[0] + 150 * c[1] + 29 * c[2]) / 256 > 110) wet_bump[k] = 0.0f;
            return;
        }
        if (!(c[1] >= c[0] + 30 && c[1] >= c[2])) return;   // not the slime
        // The glint, by the sharp bumps. N.H with H = L + V,
        // V = (0, 0, 1): |H| = sqrt(2 + 2 lz).
        float g[3];
        normal(raw.data(), k, bump_depth, g);
        const float nh = std::max(0.0f, (g[0] * lx + g[1] * ly + g[2] * lz + g[2]) / std::sqrt(2.0f + 2.0f * lz));
        // (Lighter slime is wetter.)
        wet_spec[k] = std::pow(nh, shin) * (0.4f + 0.6f * lum(k));
    };
    auto dump_heights = [&] {
        if (const char* dbg = std::getenv("KOL_BUMP_DEBUG")) {   // the heights, a PGM
            if (FILE* f = std::fopen(dbg, "wb")) {
                std::fprintf(f, "P5 %d %d 255\n", W, sp.h);
                for (std::size_t q = 0; q < pixels; ++q) std::fputc(static_cast<int>(shp[q] / cap * 255.0f), f);
                std::fclose(f);
            }
        }
    };
    // The stones and their shadows slide with the camera (asked
    // 2026-10-15 after the Thor lost ~6 fps panning map 51 with them,
    // 55 -> 50, every frame worked out again from scratch): a pan moves
    // the art by whole pixels, the change of (kx + ox, ky + oy), and the
    // pots with it, so the kept heights and terms are moved too, and
    // only 16 px blocks whose art, pool or flame changed (the strip that
    // came into view, a unit's animation) are worked out again: their
    // heights over them and one block round, their terms two blocks
    // round (the relief looks 10 px toward the flame).
    // KOL_BUMP_SLIDE=0: everything every time.
    static const bool slide_env = [] { const char* v = std::getenv("KOL_BUMP_SLIDE"); return !v || std::atoi(v) != 0; }();
    static std::vector<std::uint8_t> prev_art, prev_glow, prev_lamp;
    static float prev_fx[32], prev_fy[32];
    static int prev_bx = 0, prev_by = 0, prev_x0 = 0, prev_x1 = -1, prev_y0 = 0, prev_y1 = -1, prev_w = 0;
    static bool prev_ok = false;
    const int bx = sp.kx + sp.ox, by = sp.ky + sp.oy;
    const int dxs = bx - prev_bx, dys = by - prev_by;
    const bool slide = slide_env && shape_on && prev_ok && same_size && prev_w == W &&
                       std::abs(dxs) < W / 2 && std::abs(dys) < sp.h / 2;
    // Everything worked out from scratch: the heights over the band, the
    // terms inside the frame's edge rows and columns.
    auto rebuild_all = [&] {
        wet_bump.assign(pixels, 0.0f);
        wet_spec.assign(pixels, 0.0f);
        if (shape_on) heights(cx0, cx1, ry0, ry1, cx0, cx1, ry0, ry1);
        for (int y = std::max(1, ry0); y < std::min(sp.h - 1, ry1 + 1); ++y)
            for (int x = 1; x < W - 1; ++x) terms(x, y);
    };
    if (!slide) {
        rebuild_all();
    } else {
        // Move what is kept by (dxs, dys); what has no source is zero.
        // (The kept values swapped into a scratch buffer, not copied:
        // the loop writes every value of `a`, so what the swap leaves
        // in it never shows. Copied, it was three frame-sized arrays a
        // panning frame.)
        auto shift = [&](std::vector<float>& a) {
            static std::vector<float> t;
            t.swap(a);
            a.resize(t.size());
            for (int y = 0; y < sp.h; ++y) {
                float* d = &a[static_cast<std::size_t>(y) * W];
                const int ys = y - dys;
                if (ys < 0 || ys >= sp.h) { std::fill(d, d + W, 0.0f); continue; }
                const float* src = &t[static_cast<std::size_t>(ys) * W];
                for (int x = 0; x < W; ++x) {
                    const int xs = x - dxs;
                    d[x] = xs >= 0 && xs < W ? src[xs] : 0.0f;
                }
            }
        };
        if (dxs || dys) {
            shift(shp); shift(wet_bump); shift(wet_spec);
            // The frame's edge rows and columns take no terms (the full
            // pass leaves them out): none may be carried into them.
            for (std::vector<float>* a : {&wet_bump, &wet_spec}) {
                std::fill_n(a->begin(), W, 0.0f);
                std::fill_n(a->begin() + static_cast<std::ptrdiff_t>(sp.h - 1) * W, W, 0.0f);
                for (int y = 0; y < sp.h; ++y) {
                    (*a)[static_cast<std::size_t>(y) * W] = 0.0f;
                    (*a)[static_cast<std::size_t>(y) * W + W - 1] = 0.0f;
                }
            }
        }
        constexpr int B = 16;
        const int nbx = (W + B - 1) / B, nby = (sp.h + B - 1) / B;
        static std::vector<std::uint8_t> dirty, dil1, dil2;
        dirty.assign(static_cast<std::size_t>(nbx) * nby, 0);
        // A kept height near the band's edge was worked out against the
        // clamped edge: still right only where it stands as far from
        // each edge (up to 8 px) as it did.
        auto e8 = [](int d) { return std::min(d, 8); };
        for (int y = ry0; y <= ry1; ++y) {
            const int ys = y - dys;
            for (int x = cx0; x <= cx1; ++x) {
                std::uint8_t& db = dirty[static_cast<std::size_t>(y / B) * nbx + x / B];
                if (db) { x = (x / B + 1) * B - 1; continue; }
                const int xs = x - dxs;
                bool bad = xs < prev_x0 || xs > prev_x1 || ys < prev_y0 || ys > prev_y1 ||
                           e8(x - cx0) != e8(xs - prev_x0) || e8(cx1 - x) != e8(prev_x1 - xs) ||
                           e8(y - ry0) != e8(ys - prev_y0) || e8(ry1 - y) != e8(prev_y1 - ys);
                if (!bad) {
                    const std::size_t k = static_cast<std::size_t>(y) * W + x;
                    const std::size_t q = static_cast<std::size_t>(ys) * W + xs;
                    const std::uint8_t* a = scene + k * 3;
                    const std::uint8_t* b = &prev_art[q * 3];
                    bad = a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || (sp.glow[k] != 0) != (prev_glow[q] != 0);
                    if (!bad && sp.glow[k]) {
                        const unsigned li = sp.glow_lamp[k], pl = prev_lamp[q];
                        bad = li >= 32u || pl >= 32u ||
                              std::fabs((sp.glow_fx[li] - x) - (prev_fx[pl] - xs)) > 0.5f ||
                              std::fabs((sp.glow_fy[li] - y) - (prev_fy[pl] - ys)) > 0.5f;
                    }
                }
                if (bad) db = 1;
            }
        }
        auto dilate = [&](const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
            out.assign(in.size(), 0);
            for (int j = 0; j < nby; ++j)
                for (int i = 0; i < nbx; ++i) {
                    if (!in[static_cast<std::size_t>(j) * nbx + i]) continue;
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di) {
                            const int jj = j + dj, ii = i + di;
                            if (jj >= 0 && jj < nby && ii >= 0 && ii < nbx) out[static_cast<std::size_t>(jj) * nbx + ii] = 1;
                        }
                }
        };
        dilate(dirty, dil1);
        dilate(dil1, dil2);
        // Heights over the dirty blocks and one round, a run of blocks
        // along a row at a time, 8 px of context round it.
        for (int j = 0; j < nby; ++j)
            for (int i = 0; i < nbx; ++i) {
                if (!dil1[static_cast<std::size_t>(j) * nbx + i]) continue;
                int e = i;
                while (e + 1 < nbx && dil1[static_cast<std::size_t>(j) * nbx + e + 1]) ++e;
                const int ox0 = std::max(cx0, i * B), ox1 = std::min(cx1, (e + 1) * B - 1);
                const int oy0 = std::max(ry0, j * B), oy1 = std::min(ry1, (j + 1) * B - 1);
                if (ox0 <= ox1 && oy0 <= oy1)
                    heights(std::max(cx0, ox0 - 8), std::min(cx1, ox1 + 8), std::max(ry0, oy0 - 8), std::min(ry1, oy1 + 8),
                            ox0, ox1, oy0, oy1);
                i = e;
            }
        for (int j = 0; j < nby; ++j)
            for (int i = 0; i < nbx; ++i) {
                if (!dil2[static_cast<std::size_t>(j) * nbx + i]) continue;
                for (int y = std::max(1, j * B); y < std::min({sp.h - 1, (j + 1) * B}); ++y)
                    for (int x = std::max(1, i * B); x < std::min({W - 1, (i + 1) * B}); ++x) {
                        if (y < ry0 || y > ry1) {
                            const std::size_t k = static_cast<std::size_t>(y) * W + x;
                            wet_bump[k] = wet_spec[k] = 0.0f;
                            continue;
                        }
                        terms(x, y);
                    }
            }
        // Outside the band nothing is lit: no kept term may stay there.
        for (int y = 0; y < sp.h; ++y) {
            if (y >= ry0 && y <= ry1) continue;
            std::fill_n(&wet_bump[static_cast<std::size_t>(y) * W], W, 0.0f);
            std::fill_n(&wet_spec[static_cast<std::size_t>(y) * W], W, 0.0f);
        }
        // KOL_BUMP_SLIDE_CHECK=1: worked out again from scratch and
        // compared; the differences logged (the scratch result kept).
        static const bool slide_check = std::getenv("KOL_BUMP_SLIDE_CHECK") != nullptr;
        if (slide_check) {
            const std::vector<float> s_shp = shp, s_bump = wet_bump, s_spec = wet_spec;
            rebuild_all();   // (shape_on: a slide is only with it)
            int nh = 0, nb = 0, shown = 0;
            for (int y = ry0; y <= ry1; ++y)
                for (int x = cx0; x <= cx1; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * W + x;
                    const bool hd = std::fabs(s_shp[k] - shp[k]) > 1e-4f;
                    const bool bd = std::fabs(s_bump[k] - wet_bump[k]) > 1e-4f || std::fabs(s_spec[k] - wet_spec[k]) > 1e-4f;
                    nh += hd; nb += bd;
                    if ((hd || bd) && shown < 4) {
                        ++shown;
                        std::fprintf(stderr, "slide-check: (%d,%d) h %.3f/%.3f bump %.3f/%.3f band x %d..%d y %d..%d dirty %d dil1 %d dil2 %d shift %d,%d\n",
                                     x, y, s_shp[k], shp[k], s_bump[k], wet_bump[k], cx0, cx1, ry0, ry1,
                                     dirty[static_cast<std::size_t>(y / B) * nbx + x / B], dil1[static_cast<std::size_t>(y / B) * nbx + x / B],
                                     dil2[static_cast<std::size_t>(y / B) * nbx + x / B], dxs, dys);
                    }
                }
            std::fprintf(stderr, "slide-check: shift %d,%d heights differ %d, terms differ %d\n", dxs, dys, nh, nb);
        }
    }
    if (shape_on) {
        dump_heights();
        prev_art.resize(pixels * 3);
        for (int y = ry0; y <= ry1; ++y)
            std::memcpy(&prev_art[static_cast<std::size_t>(y) * W * 3], scene + static_cast<std::size_t>(y) * W * 3,
                        static_cast<std::size_t>(W) * 3);
        prev_glow.assign(sp.glow, sp.glow + pixels);
        prev_lamp.assign(sp.glow_lamp, sp.glow_lamp + pixels);
        std::memcpy(prev_fx, sp.glow_fx, sizeof prev_fx);
        std::memcpy(prev_fy, sp.glow_fy, sizeof prev_fy);
        prev_bx = bx; prev_by = by; prev_x0 = cx0; prev_x1 = cx1; prev_y0 = ry0; prev_y1 = ry1; prev_w = W;
        prev_ok = true;
    } else {
        prev_ok = false;
    }
}

// The light kol_focus worked out, on the scene before anything samples it:
// the occlusion and shadows, the torchlight, and Debug > Tile Info's tint.
// It falls on the map's pixels (top layer BG0 or BG1) and on those
// `ground_extra` marks (the ground under a unit drawn from its sprite; null:
// none), never on interface. The perspective's warp and the flat shading
// (flat_provider) both.
// Its per-pixel steps run in one pass on the row pool (par_px): each
// reads and writes its own pixel alone, so a pixel goes through them in
// the same order as when each was a pass of its own over the scene, and
// the scene is swept once, not up to a dozen times (one pool run, the
// map's layer and the ground tests worked out once a pixel).
// Special Effects on a sea map: the foam round the boat drawn anew (the user's
// pick B, 2026-11-11), before the light so the sail's shadow takes it too. The
// game's foam sprites show what is under them; each listed sea pixel (kol_focus
// sea_foam_list: its world place, the ring's and the wake's weights) takes
// bubbles from a noise in the world, the wake's carried off with the water
// (+Z, ScenePixels::sea_foam_flow a frame), the ring's churning slowly in place,
// over a soft glow along the hull. KOL_SEA_FOAM_*: the mockup's numbers.
// Before the light: the game's foam shows the sea under it, and the listed
// pixels' colours are kept (their light, after, is that over this).
std::vector<std::uint8_t> g_sea_foam_unlit;
void sea_foam_hide(const kol::ScenePixels& sp, std::uint8_t* scene, std::size_t pixels) {
    if (!sp.sea_foam || sp.sea_foam_count <= 0) return;
    if (sp.sea_foam_hide && sp.under_rgb)
        for (std::size_t k = 0; k < pixels; ++k)
            if (sp.sea_foam_hide[k]) std::copy_n(sp.under_rgb + k * 3, 3, scene + k * 3);
    g_sea_foam_unlit.resize(static_cast<std::size_t>(sp.sea_foam_count));
    for (int i = 0; i < sp.sea_foam_count; ++i) {
        const std::size_t k = sp.sea_foam[i].k;
        const std::uint8_t* p = scene + (k < pixels ? k : 0) * 3;
        g_sea_foam_unlit[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(std::min(255L, std::lround(0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2])));
    }
}
// After the light: the foam over the lit sea, as bright as the light on the
// water there allows, gently (white foam dims less than the sea in shade: the
// occlusion along the hull greyed it to lavender, lit before).
// A blue flame's texel (SpriteBlock::glow_flame): cyan to blue, its green
// and blue well over its red (map 57's Shaher: 20b0f8, 48f0f8 and their
// darker steps; not the purples of his wings, green under red there).
bool glow_flame_px(const std::uint8_t* c) {
    const int r = c[0], g = c[1], b = c[2];
    return std::max(g, b) >= 48 && 10 * g >= 16 * r && 10 * b >= 14 * r && 100 * g >= 45 * b;
}

// A spell's circle on the floor (map_settings.cfg `magic_circle`; map 57's
// summoning circle, Thor state 0232; the user, 2026-11-11, asked for its red
// floor circle to look good). The game draws it on BG2
// in screen space, blended over the floor: a dark circle fading in (its
// colour black, EVA rising), then a red pulse added (EVB 12: the floor
// under its lines at 3/4). Drawn here on the scene, after the light, so it
// lies on the floor (the warp moves it with the floor: it stays there when
// the camera moves) and keeps its colour in the shade: the floor under its
// lines dimmed as the game dims it, its lines its colour and a little
// white at the pulse's height, and a soft halo of its colour round them
// (blurred once a circle). KOL_CIRCLE=<lines,white,halo,dim,ember>.
void magic_circle(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h) {
    if (!sp.circle) return;
    // (Pick E of five, shots/map57_circle/: saturated, the floor under its
    // lines at half, so it reads red, not pink.)
    static float k[5] = {1.4f, 0.5f, 0.5f, 2.0f, 0.5f};
    static const bool env = [] {
        const char* v = std::getenv("KOL_CIRCLE");
        return v && std::sscanf(v, "%f,%f,%f,%f,%f", &k[0], &k[1], &k[2], &k[3], &k[4]) == 5;
    }();
    (void)env;
    constexpr int kR = 4;   // the halo's box blur, twice
    static std::vector<float> halo;
    static std::uint32_t halo_key = 0;
    std::uint32_t key = kol::kFnvBasis;
    for (int i = 0; i < 4; ++i) kol::fnv_mix(key, static_cast<std::uint32_t>(sp.circle_box[i]));
    for (int v = sp.circle_box[1]; v <= sp.circle_box[3]; ++v)
        for (int u = sp.circle_box[0]; u <= sp.circle_box[2]; u += 4) {
            const std::uint8_t* m = sp.circle + v * 256 + u;
            kol::fnv_mix(key, static_cast<std::uint32_t>(m[0] | m[1] << 1 | (u + 2 < 256 ? m[2] << 2 : 0) | (u + 3 < 256 ? m[3] << 3 : 0)));
        }
    if (key != halo_key || halo.size() != 256u * 256u) {
        halo_key = key;
        std::vector<float> a(256u * 256u), b(256u * 256u);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = sp.circle[i] ? 1.0f : 0.0f;
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    float t = 0.0f;
                    for (int d = -kR; d <= kR; ++d) t += a[static_cast<std::size_t>(y) * 256 + std::clamp(x + d, 0, 255)];
                    b[static_cast<std::size_t>(y) * 256 + x] = t / (2 * kR + 1);
                }
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    float t = 0.0f;
                    for (int d = -kR; d <= kR; ++d) t += b[static_cast<std::size_t>(std::clamp(y + d, 0, 255)) * 256 + x];
                    a[static_cast<std::size_t>(y) * 256 + x] = t / (2 * kR + 1);
                }
        }
        // (So a ring's middle reaches about 1: lines are a texel or two wide.)
        for (float& f : a) f = std::min(1.0f, 3.0f * f);
        halo.swap(a);
    }
    const float c[3] = {static_cast<float>(sp.circle_rgb[0]), static_cast<float>(sp.circle_rgb[1]),
                        static_cast<float>(sp.circle_rgb[2])};
    const float peak = std::max({c[0], c[1], c[2]});
    const float dim = k[3] * (1.0f - static_cast<float>(sp.circle_evb) / 16.0f);
    // While it fades in black, a faint ember of red in its lines (by EVA).
    const float ember = peak < 8.0f ? k[4] * static_cast<float>(sp.circle_eva) / 16.0f : 0.0f;
    const int u0 = std::max(0, sp.circle_box[0] - 2 * kR), u1 = std::min(255, sp.circle_box[2] + 2 * kR);
    const int v0 = std::max(0, sp.circle_box[1] - 2 * kR), v1 = std::min(255, sp.circle_box[3] + 2 * kR);
    for (int y = std::max(0, v0 - sp.circle_sv); y <= std::min(h - 1, v1 - sp.circle_sv); ++y) {
        const int v = y + sp.circle_sv;
        for (int x = std::max(0, u0 - sp.circle_su); x <= std::min(w - 1, u1 - sp.circle_su); ++x) {
            const int u = x + sp.circle_su;
            const std::size_t k2 = static_cast<std::size_t>(y) * w + x;
            if (sp.kind && sp.kind[k2] == kol::kSceneNone) continue;
            const std::size_t t = static_cast<std::size_t>(v) * 256 + u;
            const float m = sp.circle[t] ? 1.0f : 0.0f, hl = halo[t];
            if (m == 0.0f && hl < 0.01f) continue;
            std::uint8_t* q = scene + k2 * 3;
            for (int ch = 0; ch < 3; ++ch) {
                float f = q[ch] * std::max(0.0f, 1.0f - dim * m);
                f += m * (k[0] * c[ch] + k[1] * peak) + hl * k[2] * c[ch];
                f += m * ember * (ch == 0 ? 70.0f : 10.0f) + hl * ember * (ch == 0 ? 30.0f : 3.0f);
                q[ch] = static_cast<std::uint8_t>(std::min(255.0f, f));
            }
        }
    }
}

void sea_foam(const kol::ScenePixels& sp, std::uint8_t* scene, std::size_t pixels) {
    if (!sp.sea_foam || sp.sea_foam_count <= 0 || g_sea_foam_unlit.size() != static_cast<std::size_t>(sp.sea_foam_count))
        return;
    static const float glow_s = kol::env_num("KOL_SEA_FOAM_GLOW", 0.7f);
    static const float scale = kol::env_num("KOL_SEA_FOAM_SCALE", 5.0f);
    auto hsh = [](int x, int y, int z) {
        std::uint32_t n = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u +
                          static_cast<std::uint32_t>(z) * 2147483647u;
        n = (n ^ (n >> 13)) * 1274126177u;
        return static_cast<float>(n ^ (n >> 16)) / 4294967296.0f;
    };
    auto vnoise = [&](float x, float y, int z) {
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const int xi = static_cast<int>(fx0), yi = static_cast<int>(fy0);
        float fx = x - fx0, fy = y - fy0;
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        const float a = hsh(xi, yi, z), b = hsh(xi + 1, yi, z), c = hsh(xi, yi + 1, z), d = hsh(xi + 1, yi + 1, z);
        return (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;
    };
    auto smooth = [](float e0, float e1, float v) {
        const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float t = static_cast<float>(sp.serial % 100000u);
    const float run = sp.sea_foam_flow * t;
    // The ring churns: two slices of noise a few seconds apart, blended.
    const float slice_t = t / 40.0f;
    const int s0 = static_cast<int>(std::floor(slice_t));
    const float sf = slice_t - static_cast<float>(s0);
    static const std::uint8_t kFoam[3] = {232, 248, 255};
    for (int i = 0; i < sp.sea_foam_count; ++i) {
        const kol::ScenePixels::SeaFoamPx& e = sp.sea_foam[i];
        if (e.k >= pixels) continue;
        const float ring = e.ring / 255.0f, wake = e.wake / 255.0f;
        const float X = e.x / scale, Z = e.z / scale;
        float a = 0.0f;
        if (ring > 0.0f) {
            const float n = vnoise(X, Z - run * 0.6f / scale, s0) * (1.0f - sf) + vnoise(X, Z - run * 0.6f / scale, s0 + 1) * sf;
            const float th = 1.0f - ring * 1.1f;
            a = std::max(a, glow_s * std::pow(ring, 1.3f) + smooth(th - 0.08f, th + 0.04f, n));
        }
        if (wake > 0.0f) {
            // Carried with the water: formed at the hull, it stays where it
            // formed in the sea and is left behind, breaking up as it ages.
            const float n = 0.65f * vnoise(X, Z - run / scale, 7) + 0.35f * vnoise(X * 2.3f, (Z - run / scale) * 2.3f, 9);
            const float th = 1.0f - wake * 0.8f;
            a = std::max(a, smooth(th - 0.06f, th + 0.05f, n) * (0.45f + 0.55f * wake));
        }
        a = std::min(1.0f, a);
        if (a <= 0.0f) continue;
        std::uint8_t* p = scene + static_cast<std::size_t>(e.k) * 3;
        // The light: the scene's own shade there (its occlusion and shadows,
        // ScenePixels::ao), gently; read off the lit sea's brightness, the
        // water's texture speckled it with dark spots.
        const float shade = sp.ao ? sp.ao[e.k] / 255.0f : 1.0f;
        const float light = 0.62f + 0.38f * shade;
        for (int ch = 0; ch < 3; ++ch)
            p[ch] = static_cast<std::uint8_t>(std::lround(p[ch] * (1.0f - a) + std::min(255.0f, kFoam[ch] * light) * a));
    }
}

void apply_lighting(const kol::ScenePixels& sp, const std::uint8_t* layers,
                    const std::uint8_t* ground_extra, std::uint8_t* scene, std::size_t pixels,
                    bool under_iface = false) {
    const std::uint8_t* iface = sp.iface;
    // With `under_iface` (the warp, after it filled them): under an
    // interface sprite the scene holds the ground the game drew there
    // without sprites, and it is lit as the map. Left unlit, the ground
    // behind the command menu's icons showed without its shadow and
    // occlusion once the stick moved the map against them
    // (states/FG_slot1_icons_break_stone).
    // (The floating hand's spot too: filled the same way once the hand is
    // moved off it, it showed as a light hand-shaped hole in a shadow, FI.)
    auto ground_under_iface = [&](std::size_t k) {
        return under_iface && (iface[k] || (sp.floating && sp.floating[k])) && obj_over(sp, k) &&
               (sp.scene_codes[k] >> 4) <= 1u;
    };
    // The map's own pixels: the top layer, or under a HUD layer (the frame's
    // dialog box, BG2/BG3) the layer below it. Judged by the frame's top
    // alone, the map under a dialog box stayed unlit, and since the warp
    // moves the map against the box drawn flat, it showed as a band along
    // the box's edges in a shaded place (dialog boxes seemed to cast a shadow).
    // Under two HUD layers (a dialog box's BG3 over its text on BG2) the
    // frame's code knows neither map layer: the scene's own code does (the
    // compositor's, sp.scene_codes). Taken from the frame's, the text's
    // pixels were BG2, not the map: the shadows and the occlusion left them
    // out, and with the camera turned the map slid from under the box with
    // the letters on it, lighter than the shade around them, like
    // graffiti on the walls (the user, 2026-10-28; JZ). KOL_HUD_MAP_FRAME=1:
    // the frame's, as before.
    static const bool hud_map_frame = std::getenv("KOL_HUD_MAP_FRAME") != nullptr;
    const unsigned hud = kol::present_exclude_layers();
    auto map_layer = [&](std::size_t k) {
        const unsigned top = layers[k] & 0x0Fu;
        if (!(top < 4u && ((hud >> top) & 1u))) return top;
        const unsigned under = layers[k] >> 4;
        if (!hud_map_frame && sp.scene_codes && under < 4u && ((hud >> under) & 1u))
            return static_cast<unsigned>(sp.scene_codes[k] & 0x0Fu);
        return under;
    };
    // Which pixels the light falls on, for every step below: the map's own
    // (BG0 or BG1 by map_layer) and those `ground_extra` marks (`map_px`);
    // the battlefield, not interface but the ground under it
    // (ground_under_iface; `battlefield`); and the map's own of those
    // (`map_lit`: the occlusion, the torches' tint, the bumps).
    auto map_px = [&](std::size_t k) { return (ground_extra && ground_extra[k]) || map_layer(k) <= 1u; };
    auto battlefield = [&](std::size_t k) { return !iface[k] || ground_under_iface(k); };
    auto map_lit = [&](std::size_t k) { return (map_px(k) && !iface[k]) || ground_under_iface(k); };
    // A dark map's wet floor (map 51's slime; asked 2026-10-14: the light
    // green torch reflections to flicker a little so it looks slimy, then
    // for a bump map from the existing textures; of three mockups the user took glints on the slime
    // and a faint bump on the stone): the unlit art's brightness (the bump
    // map's heights) and whether a pixel is the slime (teal and green: green
    // over red by 30, not under blue; the map's pale blue slabs are not),
    // before any light. KOL_SLIME=<glints> (1, 0 none), KOL_SLIME_BUMP=<the
    // stone's bump> (0.5, 0 none).
    static const float slime_env = kol::env_num("KOL_SLIME", 1.0f);
    const float slime_amt = sp.bump_plain ? 0.0f : slime_env;   // (a plain bump map: no glints)
    static const float stone_bump_env = kol::env_num("KOL_SLIME_BUMP", 0.5f);
    // A plain bump map's (kPlainBumpMaps) its own: 0.6 (the user's pick E of
    // shots/map79/normal_map_options.png was 1.0, then a little less intense: 0.6
    // of shots/map79/normal_map_strength.png). KOL_PLAIN_BUMP=<strength>.
    static const float plain_bump = kol::env_num("KOL_PLAIN_BUMP", 0.6f);
    const float stone_bump = sp.bump_boxed ? plain_bump * sp.bump_box_strength   // (a panel's: x its own)
                             : sp.bump_plain ? plain_bump : stone_bump_env;
    // The wet floor's terms (wet_floor_terms), worked out once a view.
    static std::vector<float> wet_bump, wet_spec;
    // (Options > Bump Mapping, asked 2026-10-14 for a switch for the bump
    // mapping.)
    const bool slime_on = kol::setting_on(kol::Setting::BumpMap) && (slime_amt > 0.0f || stone_bump > 0.0f) &&
                          sp.wet_floor && sp.dark < 1.0f && sp.glow && sp.glow_fx &&
                          sp.glow_lamp && sp.w > 2 && sp.h > 2 && static_cast<std::size_t>(sp.w) * sp.h == pixels;
    // The same bumps on a map in the sun (asked 2026-10-14 to try bump
    // mapping on map 89; the user took 0.3 of 0 / 0.4 / 0.8): lit
    // from the sun's direction (sun_now: the direction toward it and its
    // height), weaker where the occlusion and the shadows take the sun
    // (sp.ao, squared). Every map pixel, roofs and props too. On no map now
    // (the user, after trying it on 89, found it added nothing); KOL_BUMP_SUN
    // (kol_focus sun_bump_strength, sp.sun_bump) for mockups.
    const float sun_bump = sp.sun_bump;
    float sun_deg = 0.0f, sun_dx = 0.0f, sun_dz = 0.0f;
    const bool sun_on = !slime_on && sun_bump > 0.0f && kol::setting_on(kol::Setting::BumpMap) &&
                        sp.dark >= 1.0f && kol::sun_now(&sun_deg, &sun_dx, &sun_dz) && sp.w > 2 && sp.h > 2 &&
                        static_cast<std::size_t>(sp.w) * sp.h == pixels;
    // The sun's direction on screen: X - Z across, (X + Z) / 2 down, its
    // height out of the picture.
    float sun_l[3] = {0.0f, 0.0f, 1.0f};
    if (sun_on) {
        const float e = sun_deg * (3.14159265f / 180.0f);
        float ax = sun_dx - sun_dz, ay = 0.5f * (sun_dx + sun_dz);
        const float al = std::sqrt(ax * ax + ay * ay);
        if (al > 1e-4f) { ax /= al; ay /= al; }
        sun_l[0] = ax * std::cos(e);
        sun_l[1] = ay * std::cos(e);
        sun_l[2] = std::sin(e);
    }
    wet_floor_terms(sp, scene, pixels, slime_on, sun_on, sun_l, wet_bump, wet_spec);
    kol::WorkerTimer lt("light.pools");
    // The lamps' light this frame, once a pixel (lamp_light), for the
    // darkness and the tint both: worked out in the pass below, first in
    // each run of rows (it reads the pools, not the scene).
    const bool lamps = sp.glow != nullptr;
    // An indoor map's darkness (ScenePixels::dark): every pixel of the
    // battlefield (the map and the units in the scene, not the interface)
    // down to `dark` where no lamp reaches, back up with a lamp's light
    // (the flame's flicker too; the tint has its own), less a unit's shadow.
    // The factor a pixel, in 8.8 fixed point (three float roundings a pixel
    // over the whole scene were a third of this pass).
    const float* fire = fire_light_pools(sp);   // the flames' and fire arrows' light
    const float* spell_tint = nullptr;
    const float* spell = spell_light_pools(sp, &spell_tint);   // a spell's (spell_lights)
    const unsigned dark256 = static_cast<unsigned>(std::lround(256.0f * sp.dark));   // where no lamp reaches
    // The windows' light not on the background (the backdrop, layer 5): the
    // beams' glow lit it as a pale column left of map 67's cell, over the
    // corridor the grid has and the art does not show (the user asked for
    // that light in the upper left corner to be masked).
    // KOL_GLOW_ON_BACKDROP=1: as before.
    static const bool glow_backdrop = std::getenv("KOL_GLOW_ON_BACKDROP") != nullptr;
    // (`leff`: the lamps' light on pixel k, lamp_light, worked out first in
    // the pass below.)
    auto dark_f256 = [&](std::size_t k, unsigned leff) {
        unsigned g = leff;
        // The fire light brings the night back up as a lamp's does; so does a
        // spell's.
        if (fire && fire[k] > 0.0f) g = std::max(g, static_cast<unsigned>(std::min(255.0f, fire[k] * 255.0f)));
        if (spell && spell[k] > 0.0f) g = std::max(g, static_cast<unsigned>(std::min(255.0f, spell[k] * 255.0f)));
        // The windows' light (a dark map with windows): the sun through
        // one on the map's own pixels, or the beams' glow on any.
        // Not on the background (glow_backdrop above).
        unsigned wl = 0u;
        if (sp.win_glow && (glow_backdrop || map_layer(k) != kol::kLayerBackdrop)) {
            wl = sp.win_glow[k];
            if (sp.win_patch && sp.win_patch[k] > wl && (map_px(k) || ground_under_iface(k)))
                wl = sp.win_patch[k];
        }
        // ... and a window's own light (the moon's) at its strength: left
        // out, the dark took it down before it brightened (map 67: the
        // moonlight was barely seen).
        if (sp.moon_patch && sp.moon_patch[k] && (map_px(k) || ground_under_iface(k)))
            wl = std::max(wl, static_cast<unsigned>(std::lround(std::min(1.0f, sp.moon_strength) * sp.moon_patch[k])));
        unsigned f256;
        const float base = dark_at(sp, k);
        if (!g && !wl && !(sp.range && sp.range[k])) {
            f256 = base == sp.dark ? dark256 : static_cast<unsigned>(std::lround(256.0f * base));
        } else {
            if (g && sp.glow_lamp && sp.glow_flick)
                g = static_cast<unsigned>(std::min(255.0f, g * sp.glow_flick[sp.glow_lamp[k]] + 0.5f));
            f256 = static_cast<unsigned>(std::lround(
                256.0f * range_keep(sp, k, dark_light(sp, g, sp.lamp_shade ? sp.lamp_shade[k] : 0u, wl, base))));
        }
        return f256;
    };
    // The torchlight's daylight colours (ScenePixels::lamp_pal) on the map's
    // pixels, looked up by their colour before the occlusion and shadows
    // below change it (recoloured then shaded is the colour shaded then
    // recoloured by as much, but a shaded pixel no longer has its palette
    // colour to look up).
    const bool lamp_pal_on = sp.dark < 1.0f && sp.lamp_pal;
    // (`lit`: not interface, or the ground under it; `f256`: dark_f256, worked
    // out once a pixel for this and the darkness below.)
    auto lamp_pal_px = [&](std::size_t k, bool lit, unsigned f256) {
        if (!lit) return;
        if (!map_px(k)) return;
        if (f256 > dark256)
            lamp_palette(sp, scene + k * 3, static_cast<float>(f256 - dark256) / static_cast<float>(256u - std::min(255u, dark256)));
    };
    // Ambient occlusion (kol_focus's terrain pass): the map's own pixels
    // darkened where higher ground around them shuts the light out, before
    // the mesh samples them. The ground under a unit too; not sprites.
    auto ao_px = [&](std::size_t k) {
        unsigned a = sp.ao[k];
        if (a >= 255u) return;
        if (!map_lit(k)) return;
        if (sp.range && sp.range[k]) a = static_cast<unsigned>(std::lround(255.0f * range_keep(sp, k, a / 255.0f)));
        for (int ch = 0; ch < 3; ++ch)
            scene[k * 3 + ch] = static_cast<std::uint8_t>((scene[k * 3 + ch] * a + 127u) / 255u);
    };

    const bool dark_on = sp.dark < 1.0f;
    // The game's flash whitening the backdrop (ScenePixels::
    // backdrop_white): the night does not take it back down (a grey
    // flash, the user reported something still looked wrong).
    const unsigned bd_keep = static_cast<unsigned>(std::lround(256.0f * std::clamp(sp.backdrop_white, 0.0f, 1.0f)));
    auto dark_px = [&](std::size_t k, bool lit, unsigned f256) {
        if (!lit) return;
        if (bd_keep && map_layer(k) == kol::kLayerBackdrop) f256 += ((256u - std::min(256u, f256)) * bd_keep) >> 8;
        std::uint8_t* px = scene + k * 3;
        px[0] = static_cast<std::uint8_t>((px[0] * f256 + 128u) >> 8);
        px[1] = static_cast<std::uint8_t>((px[1] * f256 + 128u) >> 8);
        px[2] = static_cast<std::uint8_t>((px[2] * f256 + 128u) >> 8);
    };

    // Torchlight (kol_focus): the map's pixels near a torch warmed. Orange
    // light on the ruins' green stone comes out yellow by multiplication
    // alone, so the pixel is also pulled toward its own brightness in
    // orange; the art's texture stays. The ground under a unit too; not
    // sprites. A blue flame's light (the castle's statue) the same way in
    // blue; a pixel takes the colour of the lamp most of its light is from.
    // KOL_PERSP_GLOW_BLUE=<strength> scales the blue alone (mockups).
    // A campfire's logs darkened (ScenePixels::fire_logs): the flame's base.
    auto logs_px = [&](std::size_t k) {
        if (sp.fire_logs[k])
            for (int ch = 0; ch < 3; ++ch)
                scene[k * 3 + ch] = static_cast<std::uint8_t>(std::lround(scene[k * 3 + ch] * sp.fire_logs_shade));
    };
    auto tint_px = [&](std::size_t k, unsigned leff) {
        unsigned g = leff;
        if (!g) return;
        if (sp.lamp_shade) g = g * (255u - sp.lamp_shade[k]) / 255u;
        if (!g) return;
        if (!map_lit(k)) return;
        // Near a campfire the art's own colours (the user's option A,
        // 2026-11-09: like the palette updated round the boat map's
        // lanterns, something similar for the
        // campfire): its tint fades out toward the fire, full inside
        // KOL_FIRE_TRUE=<radius px,strength> (mockups; the ellipse as the
        // fire ground's, half as tall).
        float true_share = 0.0f;
        static float ft[2] = {0.0f, 0.0f};
        static const bool ft_read = [] {
            if (const char* e = std::getenv("KOL_FIRE_TRUE")) std::sscanf(e, "%f,%f", &ft[0], &ft[1]);
            return true;
        }();
        (void)ft_read;
        if (ft[0] > 0.0f && sp.fire_ground && sp.w > 0) {
            const float x = static_cast<float>(k % static_cast<std::size_t>(sp.w)), y = static_cast<float>(k / static_cast<std::size_t>(sp.w));
            const float dx = (x - sp.fire_x) / ft[0], dy = 2.0f * (y - sp.fire_y) / ft[0];
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < 1.0f) true_share = ft[1] * (1.0f - fx_smooth(0.35f, 1.0f, d));
        }
        torch_tint(&scene[k * 3], sp, g, sp.glow_lamp[k], true_share, static_cast<long>(k));
    };
    // (The scorched ground round the campfire, ScenePixels::fire_ground:
    // the box of rows and columns its ellipse reaches.)
    const bool fire_ground_on = sp.fire_ground && sp.fire_ground_r > 0.0f && sp.w > 0;
    int fg_x0 = 0, fg_x1 = -1, fg_y0 = 0, fg_y1 = -1;
    if (fire_ground_on) {
        const int w = sp.w, h = sp.h;
        const int r = static_cast<int>(std::ceil(sp.fire_ground_r));
        const int cx = static_cast<int>(std::lround(sp.fire_x)), cy = static_cast<int>(std::lround(sp.fire_y));
        fg_y0 = std::max(0, cy - r / 2 - 1);
        fg_y1 = std::min(h - 1, cy + r / 2 + 1);
        fg_x0 = std::max(0, cx - r);
        fg_x1 = std::min(w - 1, cx + r);
    }
    auto fire_ground_px = [&](int x, int y, std::size_t k) {   // (x, y in the box)
        if (iface[k]) return;
        const float dx = (x - sp.fire_x) / sp.fire_ground_r, dy = 2.0f * (y - sp.fire_y) / sp.fire_ground_r;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d >= 1.0f) return;
        const float e = 1.0f - fx_smooth(0.0f, 1.0f, d);
        const float f = 1.0f - sp.fire_ground_depth * e;
        const float red = sp.fire_ground_red * e;
        std::uint8_t* px = scene + k * 3;
        const float m[3] = {f, f * (1.0f - 0.45f * red), f * (1.0f - 0.8f * red)};
        for (int ch = 0; ch < 3; ++ch) px[ch] = static_cast<std::uint8_t>(std::lround(px[ch] * m[ch]));
    };

    // The wet floor's light. A bump map from the art's brightness (a light
    // pixel stands up, a dark one is a crack), its slope by the neighbours;
    // each pixel lit by the flame its light is mostly from (its place on
    // screen, 20 px up out of the floor, leaning with it): the floor's light
    // follows its bumps, faintly (KOL_SLIME_BUMP), and the slime takes a
    // sharp highlight (Blinn, KOL_SLIME_SHINE 24, KOL_SLIME_DEPTH 4 the
    // bumps' depth) in the pots' pale blue, scaled by the lamp's light there
    // and its flicker, with a slow shimmer of its own a pixel (a pulse in
    // about 1.5 s), so the glints twinkle as on a wet surface.
    // Not on the movement / action range's tiles (sp.range): the stones'
    // bumps and the slime's glints showed through the highlight (IT, the
    // user's slot 3, map 51, asked that the bump mapping leave the
    // movement and attack selection tiles alone). KOL_BUMP_ON_RANGE=1: as before.
    static const bool bump_on_range = std::getenv("KOL_BUMP_ON_RANGE") != nullptr;
    const std::uint8_t* no_bump = bump_on_range ? nullptr : sp.range;
    const bool slime_px_on = slime_on && lamps;
    // The shimmer, a sine by table: each pixel its own phase (a hash),
    // all turning 0.21 rad a frame.
    static float shimmer_lut[256];
    unsigned turn = 0u;
    float gain = 0.0f;
    static constexpr float kGlint[3] = {170.0f, 225.0f, 255.0f};   // the pots' blue, pale
    if (slime_px_on) {
        static const bool lut_ready = [] {
            for (int i = 0; i < 256; ++i) shimmer_lut[i] = 0.55f + 0.45f * std::sin(i * (6.2831853f / 256.0f));
            return true;
        }();
        (void)lut_ready;
        turn = static_cast<unsigned>(static_cast<float>(sp.serial) * 0.21f * (256.0f / 6.2831853f));
        gain = sp.dark_gain / 255.0f;
    }
    auto slime_px = [&](std::size_t k, unsigned leff) {
        const unsigned g = leff;
        if (!g) return;
        const float q = wet_bump[k], s0 = wet_spec[k];
        if ((q == 0.0f && s0 == 0.0f) || (no_bump && no_bump[k])) return;
        if (!map_lit(k)) return;
        const unsigned li = sp.glow_lamp[k];
        const float light = std::min(1.0f, gain * g) * (sp.glow_flick ? sp.glow_flick[li] : 1.0f);
        std::uint8_t* px = scene + k * 3;
        if (stone_bump > 0.0f && q != 0.0f) {   // (8.8 fixed point)
            // (A panel's, ScenePixels::bump_boxed, may swing further: 0.3 to 1.9.)
            // (A panel's light may swing further up, 1.9, but not down past 0.7: darker,
            // the shaded side of each ridge crushed the carving's lines.)
            static const float panel_lo = kol::env_num("KOL_PANEL_BUMP_LO", 0.7f);
            static const float panel_hi = kol::env_num("KOL_PANEL_BUMP_HI", 1.9f);
            const int f256 = static_cast<int>(256.0f * std::clamp(1.0f + stone_bump * q * light, sp.bump_boxed ? panel_lo : 0.6f,
                                                                  sp.bump_boxed ? panel_hi : 1.4f));
            for (int ch = 0; ch < 3; ++ch)
                px[ch] = static_cast<std::uint8_t>(std::min(255, (px[ch] * f256 + 128) >> 8));
        }
        if (slime_amt <= 0.0f || s0 <= 0.002f) return;
        const std::uint32_t hsh = static_cast<std::uint32_t>(k) * 2654435761u;
        const float spec = s0 * light * shimmer_lut[((hsh >> 24) + turn) & 255u] * slime_amt;
        if (spec <= 0.004f) return;
        for (int ch = 0; ch < 3; ++ch)
            px[ch] = static_cast<std::uint8_t>(std::min(255.0f, px[ch] + kGlint[ch] * spec));
    };

    auto sun_px = [&](std::size_t k) {
        const float q = wet_bump[k];
        if (q == 0.0f || (no_bump && no_bump[k])) return;
        if (!map_lit(k)) return;
        const float light = sp.ao ? sp.ao[k] / 255.0f : 1.0f;
        const int f256 = static_cast<int>(256.0f * std::clamp(1.0f + sun_bump * q * light * light, 0.6f, 1.4f));
        std::uint8_t* px = scene + k * 3;
        for (int ch = 0; ch < 3; ++ch) px[ch] = static_cast<std::uint8_t>(std::min(255, (px[ch] * f256 + 128) >> 8));
    };

    // Sunlight through the map's windows (kol_focus, KOL_GODRAYS): on the
    // map's pixels the sun the wall's shade took back, a little warm; then
    // the beams' haze over everything but the interface. The ground under
    // an interface sprite too (the floating hand's spot: left out, it
    // showed as a hand-shaped hole in the light).
    // (`patch`, `haze`: the windows' or the moon's, below.)
    auto window_px = [&](std::size_t k, const std::uint8_t* patch, const std::uint8_t* haze, float strength,
                         const float* tint) {
        // (No sun and no haze here: window_light would change nothing.)
        if (!patch[k] && !haze[k]) return;
        const bool under = ground_under_iface(k);
        if (iface[k] && !under) return;
        const bool ground = under || map_px(k);
        window_light(&scene[k * 3], ground ? patch[k] : 0u, haze[k], strength, sp.scene_bright, tint);
    };
    const bool win_on = sp.win_patch && sp.win_haze;
    // ... and the windows with a light of their own (map 67's moonlight).
    const bool moon_on = sp.moon_patch && sp.moon_haze;
    // Near-white art under the windows' light keeps its folds (ScenePixels::win_white_cap):
    // no brighter than its own colour times the cap. `white_art`: the scene as it
    // came in, before any of the steps below.
    const float white_cap = sp.win_white_cap;
    static std::vector<std::uint8_t> white_art;
    if (white_cap > 0.0f && (win_on || moon_on)) white_art.assign(scene, scene + pixels * 3);
    auto white_cap_px = [&](std::size_t k) {
        const bool lit = (win_on && (sp.win_patch[k] || sp.win_haze[k])) || (moon_on && (sp.moon_patch[k] || sp.moon_haze[k]));
        if (!lit) return;
        const std::uint8_t* a = &white_art[k * 3];
        const int low = std::min<int>({a[0], a[1], a[2]});
        if (low <= 110) return;   // only what is near white: floors and wood keep the whole light
        const float share = std::min(1.0f, (low - 110) / 60.0f);
        for (int ch = 0; ch < 3; ++ch) {
            const float cap = a[ch] * white_cap;
            const float v = scene[k * 3 + ch];
            if (v > cap) scene[k * 3 + ch] = static_cast<std::uint8_t>(std::lround(v + (cap - v) * share));
        }
    };

    // The late day (ScenePixels::evening): every pixel of the battlefield,
    // the map and the units in the scene, not the interface. The atlas's
    // pieces (lifted props, units drawn from their sprites) take it as
    // they are copied.
    // A map at sunset (ScenePixels::sunset_grade): warm where the sun falls,
    // cool in the shade, by the light there (ao, 255 lit).

    // The steps in one pass on the row pool, in the order they were passes
    // of their own: daylight colours, occlusion, darkness, the logs, the
    // torches' tint, the scorched ground, the wet floor, the sun's bumps,
    // the windows, the moon, the evening, the sunset. Each step over a run
    // of rows (16) at a time, then the next over the same run, still in the
    // caches: a pixel goes through them in that order, as before. (A step a
    // pixel at a time, every pixel tested a dozen steps' switches: on a map
    // in the sun, the occlusion alone, that took twice what its own pass did.)
    lt.lap("light.px");
    // (The steps on, as bits of a local in each run: the scene's byte
    // stores may alias anything reached through a reference, so a flag
    // read through one was read again at every pixel.)
    enum : unsigned {
        kLampPal = 1u << 0, kAo = 1u << 1, kDark = 1u << 2, kLogs = 1u << 3, kLamps = 1u << 4, kFireGround = 1u << 5,
        kSlime = 1u << 6, kSun = 1u << 7, kWin = 1u << 8, kMoon = 1u << 9, kEvening = 1u << 10, kSunset = 1u << 11,
    };
    const unsigned steps = (lamp_pal_on ? kLampPal : 0u) | (sp.ao ? kAo : 0u) | (dark_on ? kDark : 0u) |
                           (sp.fire_logs ? kLogs : 0u) | (lamps ? kLamps : 0u) | (fire_ground_on ? kFireGround : 0u) |
                           (slime_px_on ? kSlime : 0u) | (sun_on ? kSun : 0u) | (win_on ? kWin : 0u) |
                           (moon_on ? kMoon : 0u) | (sp.evening > 0.0f ? kEvening : 0u) |
                           (sp.sunset_grade > 0.0f ? kSunset : 0u);
    // The lamps' light, a run at a time (each run its own pixels).
    static std::vector<std::uint8_t> lamp_eff;
    if (lamps) lamp_eff.resize(pixels);
    std::uint8_t* const leff = lamps ? lamp_eff.data() : nullptr;
    if (steps)
        par_runs(sp, pixels, [&](std::size_t k0, std::size_t k1) {
            const unsigned on = steps;
            if (on & kLamps)
                for (std::size_t k = k0; k < k1; ++k) leff[k] = static_cast<std::uint8_t>(lamp_light(sp, k));
            if (on & kDark) {
                // A dark map: the daylight colours, the occlusion and the
                // darkness a pixel at a time, the darkness's factor
                // (dark_f256) worked out once for the first and the last.
                for (std::size_t k = k0; k < k1; ++k) {
                    const bool lit = battlefield(k);
                    const unsigned f256 = lit ? dark_f256(k, leff ? leff[k] : 0u) : 0u;
                    if (on & kLampPal) lamp_pal_px(k, lit, f256);
                    if (on & kAo) ao_px(k);
                    dark_px(k, lit, f256);
                }
            } else if (on & kAo) {
                for (std::size_t k = k0; k < k1; ++k) ao_px(k);
            }
            if (on & kLogs)
                for (std::size_t k = k0; k < k1; ++k) logs_px(k);
            if (on & kLamps)
                for (std::size_t k = k0; k < k1; ++k) tint_px(k, leff[k]);
            if ((on & kFireGround) && k1 > k0) {   // (its box's rows in the run)
                const std::size_t W = static_cast<std::size_t>(sp.w);
                const int y0 = std::max(fg_y0, static_cast<int>(k0 / W)), y1 = std::min(fg_y1, static_cast<int>((k1 - 1) / W));
                for (int y = y0; y <= y1; ++y)
                    for (int x = fg_x0; x <= fg_x1; ++x) {
                        const std::size_t k = static_cast<std::size_t>(y) * W + x;
                        if (k >= k0 && k < k1) fire_ground_px(x, y, k);
                    }
            }
            if (on & kSlime)
                for (std::size_t k = k0; k < k1; ++k) slime_px(k, leff[k]);
            if (on & kSun)
                for (std::size_t k = k0; k < k1; ++k) sun_px(k);
            if (on & kWin)
                for (std::size_t k = k0; k < k1; ++k)
                    window_px(k, sp.win_patch, sp.win_haze, sp.win_strength, sp.win_tint);
            if (on & kMoon)
                for (std::size_t k = k0; k < k1; ++k)
                    window_px(k, sp.moon_patch, sp.moon_haze, sp.moon_strength, sp.moon_tint);
            if (white_cap > 0.0f && (on & (kWin | kMoon)))
                for (std::size_t k = k0; k < k1; ++k) white_cap_px(k);
            if (on & kEvening)
                for (std::size_t k = k0; k < k1; ++k)
                    if (battlefield(k)) evening_tint(&scene[k * 3], sp.evening);
            if (on & kSunset)
                for (std::size_t k = k0; k < k1; ++k)
                    if (battlefield(k))
                        sunset_grade(&scene[k * 3], sp.sunset_grade, sp.ao ? sp.ao[k] / 255.0f : 1.0f);
        });

    lt.lap("light.rest");
    // The flames' and fire arrows' light (Torchlight; ScenePixels::fire_lights).
    fire_light_scene(sp, iface, scene, fire);
    // A spell's light (Torchlight; ScenePixels::spell_lights).
    spell_light_scene(sp, iface, scene, spell, spell_tint);

    // Debug > Tile Info: the target cell tinted yellow, its top and its
    // walls, warped with the map.
    if (sp.tile_target >= 0 && sp.cell_owner && sp.tile_rect[0] >= 0)
        for (std::size_t k = 0; k < pixels; ++k) {
            const int c = sp.cell_owner[k];
            if (c < 0 || c == sp.tile_target || iface[k]) continue;
            const int cx = kol::tile_x(c), cz = kol::tile_z(c);
            if (cx < sp.tile_rect[0] || cx > sp.tile_rect[2] || cz < sp.tile_rect[1] || cz > sp.tile_rect[3]) continue;
            static constexpr int kRect[3] = {120, 220, 255};
            for (int ch = 0; ch < 3; ++ch)
                scene[k * 3 + ch] = static_cast<std::uint8_t>((scene[k * 3 + ch] * 3 + kRect[ch]) / 4);
        }
    if (sp.tile_target >= 0 && sp.cell_owner)
        for (std::size_t k = 0; k < pixels; ++k) {
            if (sp.cell_owner[k] != sp.tile_target || iface[k]) continue;
            static constexpr int kTint[3] = {255, 220, 40};
            for (int ch = 0; ch < 3; ++ch)
                scene[k * 3 + ch] = static_cast<std::uint8_t>((scene[k * 3 + ch] * 5 + kTint[ch] * 4) / 9);
        }
}

// Options > Water, on the lit scene before anything samples it: the
// water's pixels (ScenePixels::water) ripple a pixel sideways in slow
// bands, mirror what stands above their shore, and foam where they meet
// it. The user's pick from mockups (shots/water, "C", reflection 0.4).
// The game cycles the water's colours itself; this rides on top of that.
// The mirror is about each column's waterline (the top of its run of
// water), past the art's one-pixel shore line, fading out 60 rows down;
// the game's own sparkles keep a quarter of it. Upright things stand
// upright in the scene (the perspective keeps verticals), so a column's
// mirror is theirs. Units drawn from their own sprites (the warp) are not
// in the scene, so they cast none.
// KOL_WATER_REFL=<strength> (mockups) overrides the 0.4.
// The warp's camera (warp_provider), and for a view-dependent reflection
// (see ice_effect): its rows are base + (dref + (d - dref) s pitch) / 4 -
// (yref + (y - yref) s) / 2, s = K / (K + dref - d), its columns
// cxp + (x - cxp) s; then the combat camera's zoom about (cx, cy). Null: the
// game's own view (no perspective). `base`: the frame row of the place
// (0, 0), row = base + D / 4 - Y / 2.
struct WarpCam {
    float base = 0.0f, dref = 0.0f, yref = 0.0f, K = 1.0f, pitch = 1.0f, zoom = 1.0f;
    float cx = 0.0f, cy = 0.0f, cxp = 0.0f;
    // s at depth d, 4 where the perspective's denominator is clamped.
    float scale(float d) const {
        const float den = K + dref - d;
        return den > 0.25f * K ? K / den : 4.0f;
    }
    float row_of(float d, float y) const { return base + d * 0.25f - y * 0.5f; }
    // The combat camera's pitch takes that much of depth's rows (about the
    // reference), none of height's.
    float row_drawn(float d, float y, float s) const {
        return base + (dref + (d - dref) * s * pitch) * 0.25f - (yref + (y - yref) * s) * 0.5f;
    }
    // Where point (px, py) of an upright thing standing at (d, y) is drawn,
    // at its scale s (v: x, y): its own height times s, about cxp.
    void upright(float d, float y, float s, float px, float py, float* v) const {
        v[0] = cxp + (px - cxp) * s;
        v[1] = row_drawn(d, y, s) + (py - row_of(d, y)) * s;
    }
    // The combat camera's zoom, about the view's centre.
    void zoomed(float* v) const {
        if (zoom != 1.0f) {
            v[0] = cx + (v[0] - cx) * zoom;
            v[1] = cy + (v[1] - cy) * zoom;
        }
    }
    // Corner (i, j) as scene pixel `best` places it, by the places xz, yy,
    // kind of a scene w wide.
    // Far scenery (ScenePixels::far_px, kol_focus kFarParts): its corners are
    // moved far_share of the way from where the camera at rest (no look, the
    // same combat camera) draws them to where this one does. far null: none.
    const std::uint8_t* far = nullptr;
    float far_share = 1.0f;
    float rest_K = 1.0f, rest_pitch = 1.0f, rest_zoom = 1.0f, rest_cxp = 0.0f;
    void corner(int w, const std::int32_t* xz, const std::int32_t* yy, const std::uint8_t* kind, int best, int i, int j,
                float* v) const {
        if (far && far[best]) {
            WarpCam rest = *this, now = *this;
            rest.far = now.far = nullptr;
            rest.K = rest_K; rest.pitch = rest_pitch; rest.zoom = rest_zoom; rest.cxp = rest_cxp;
            float v0[2];
            rest.corner(w, xz, yy, kind, best, i, j, v0);
            now.corner(w, xz, yy, kind, best, i, j, v);
            v[0] = v0[0] + (v[0] - v0[0]) * far_share;
            v[1] = v0[1] + (v[1] - v0[1]) * far_share;
            return;
        }
        const float d = static_cast<float>(xz[best]);
        const float y = static_cast<float>(yy[best]);
        if (kind[best] == kol::kSceneFlat) {
            // Down a surface facing up, a pixel is 4 units of depth.
            const float dc = d + 4.0f * static_cast<float>(j - best / w);
            const float s = scale(dc);
            v[0] = cxp + (static_cast<float>(i) - cxp) * s;
            v[1] = row_drawn(dc, y, s);
        } else {
            // Down an upright thing, a pixel is its own height times s.
            upright(d, y, scale(d), static_cast<float>(i), static_cast<float>(j), v);
        }
        zoomed(v);
    }
};
// (The reflections' name for it: IceCam.)
using IceCam = WarpCam;
// The reflected ray off a flat surface (the ice, the water's), from its pixel
// (x, y). In this projection a ray off it goes straight up its column, a
// height unit and 2 X + Z back a pixel (a row is (X + Z) / 4 - Y / 2): it
// reflects the first thing it reaches standing at or in front of it, so each
// thing is mirrored about its own foot (the user reported a plant near a
// patch of ice reflecting as a chevron, which reflections should not do:
// a column mirrored about the surface's top row bent it along the
// surface's diagonal edge). Flat ground behind lies on its plane and
// reflects nothing; the sky (the backdrop by its layer code) ends it; the
// weather's particles are passed through; a lifted prop's colour is its
// square's (the scene holds the ground under it), its depth its card's.
// A unit's card is met only where the ray crosses its depth (within a
// step): an angel flying over the ice stands on a card at its place on the
// ground (its shadow), its sprite high over it, and every ice pixel between
// the two, behind the card, took its colour as a long streak (JF, the
// user's slot 3, map 40: a flying angel's reflection
// made streaks); now it is mirrored under its shadow, at its height.
// KOL_ICE_UNIT_IN_FRONT=1: met anywhere at or in front, as before.
// With the camera, the view's slope there (k: X + Z a height unit along a
// line of sight; 2 in the game's view, more as the stick lowers the camera)
// makes the ray climb 1 / (k / 4 + 1/2) a pixel and go back k times that,
// and the warp's columns (cxp + (x - cxp) s(d)) make it drift sideways with
// depth (the user asked for the reflection to follow camera changes, and
// found side to side motion odd). `wobble` moves the column it samples (the water's
// ripple). `surface` holds 1 on the surface's own pixels (passed over).
// KOL_ICE_FIXED_VIEW=1: the game's view always.
struct RayHit {
    const std::uint8_t* col = nullptr;   // the thing met, or null
    float below = 0.0f;                  // its foot's distance up from the pixel (rows of the game's view)
    const std::uint8_t* sky = nullptr;   // the sky pixel it left at, or null
    int last_ry = 0;                     // the last row it reached
};
RayHit reflect_ray(const kol::ScenePixels& sp, const std::uint8_t* src, const std::uint8_t* surface, int w, int h,
                   int x, int y, const IceCam* cam, int wobble = 0) {
    static const bool fixed_view = std::getenv("KOL_ICE_FIXED_VIEW") != nullptr;
    RayHit out;
    out.last_ry = y;
    const std::size_t k = static_cast<std::size_t>(y) * w + x;
    const int uw = sp.xz[k];
    float kv = 2.0f;
    if (cam && !fixed_view && sp.y) {
        const float dd = static_cast<float>(uw), yy = static_cast<float>(sp.y[k]);
        const float den = cam->K + cam->dref - dd;
        const float sc = den > 0.25f * cam->K ? cam->K / den : 4.0f;
        const float ds = den > 0.25f * cam->K ? sc * sc / cam->K : 0.0f;
        const float dr_dd = sc * cam->pitch * 0.25f +
                            ds * ((dd - cam->dref) * cam->pitch * 0.25f - (yy - cam->yref) * 0.5f);
        if (dr_dd > 1e-4f) kv = std::clamp(0.5f * sc / dr_dd, 0.5f, 12.0f);
    }
    const bool drift = cam && !fixed_view;
    const float s0 = drift ? cam->scale(static_cast<float>(uw)) : 1.0f;
    const float climb = 1.0f / (kv * 0.25f + 0.5f);   // height a pixel up
    const float back = kv * climb;                     // X + Z a pixel up (2 in the game's view)
    const int reach = std::min(200, static_cast<int>(240.0f / back));
    // Past the reach a thing has faded out (its foot's distance up is the
    // fade's length there), but the sky does not: a ray that has passed over
    // everything goes on up to the picture's top, looking for the sky. It
    // stopped at the reach with nothing, so water under tall land (whose
    // rays climb over it for 120 rows) took no sky while its neighbours did:
    // a dark band in the land's shape that grew with the fade (the user,
    // states/thor/0105, map 19, reported odd reflections there that
    // looked like a bug; 122 of the 342 water pixels in the circled area).
    // A thing met past the reach still blocks it (nothing).
    // KOL_REFLECT_REACH_ENDS=1: the ray ends at the reach, as before.
    static const bool reach_ends = std::getenv("KOL_REFLECT_REACH_ENDS") != nullptr;
    const int limit = reach_ends ? reach : std::max(reach, y);
    // The drift's column, a step at a time: s0 / scale(uw - back t) is
    // (D0 + back t) / D0 with D0 = K + dref - uw, linear in t (no division a
    // step; the ray on map 102's river was a fifth of the Thor's worker).
    // Where a scale is clamped (den <= K / 4) the division as before.
    const float d0 = drift ? cam->K + cam->dref - static_cast<float>(uw) : 1.0f;
    const bool drift_linear = drift && d0 > 0.25f * cam->K;
    const float drift_step = drift_linear ? (static_cast<float>(x) - cam->cxp) * back / d0 : 0.0f;
    // The column `t` pixels up, with the ripple's wobble: by the linear
    // step where `linear` asks for it and the scale allows, else by the
    // scale's division. (The sky's search behind a unit, below, takes the
    // division at every step, as it always has: the linear step can round
    // to the column beside it, a change of picture.)
    auto column_at = [&](int t, bool linear) {
        int c = x;
        if (linear && drift_linear) {
            c = static_cast<int>(std::lround(static_cast<float>(x) + drift_step * static_cast<float>(t)));
        } else if (drift) {
            const float u = static_cast<float>(uw) - back * static_cast<float>(t);
            c = static_cast<int>(std::lround(cam->cxp + (static_cast<float>(x) - cam->cxp) * s0 / cam->scale(u)));
        }
        return c + wobble;
    };
    static const bool unit_in_front = std::getenv("KOL_ICE_UNIT_IN_FRONT") != nullptr;
    static const bool sky_at_hit = std::getenv("KOL_ICE_SKY_AT_HIT") != nullptr;
    static const bool iface_stops = std::getenv("KOL_ICE_IFACE_STOPS") != nullptr;
    const bool pass_hanging = kol::map_flag(kol::MapFlag::PolishedFloor, sp.map);
    for (int tt = 1; tt <= limit; ++tt) {
        const int ry = y - tt;
        if (ry < 0) break;
        out.last_ry = ry;
        const int rxc = column_at(tt, true);
        if (rxc < 0 || rxc >= w) break;
        const std::size_t q = static_cast<std::size_t>(ry) * w + rxc;
        if (surface[q] == 1u) continue;
        // Through the falling snow: a flake is not a thing standing in the
        // column (each made a streak down the ice under it).
        if (sp.weather && sp.weather[q]) continue;
        // Interface (a flying unit's blob shadow, the cursor) is judged as
        // the ground the scene holds under it. Stopped there, the ice under
        // an angel's shadow took the sky of the shadow's own low row, the
        // light fade by the horizon, down a whole column (JF).
        // KOL_ICE_IFACE_STOPS=1: it stops the ray, as before.
        if (sp.iface[q] && iface_stops) break;
        int us = sp.xz[q];
        bool unit_card = false;
        const std::uint8_t* col = &src[q * 3];
        if (sp.lift && sp.lift[q] && sp.lift[q] <= sp.block_count && sp.atlas_rgb && sp.atlas_opaque) {
            const kol::ScenePixels::SpriteBlock& b = sp.blocks[sp.lift[q] - 1];
            const auto [ax0, ay0] = kol::slot_origin(b.slot, sp.atlas_per_row);
            const int ax = ax0 + (rxc - b.sx);
            const int ay = ay0 + (ry - b.sy);
            const std::size_t ai = static_cast<std::size_t>(ay) * sp.atlas_w + ax;
            if (rxc >= b.sx && rxc < b.sx + b.w && ry >= b.sy && ry < b.sy + b.h && sp.atlas_opaque[ai]) {
                us = b.xz;
                col = sp.atlas_rgb + ai * 3;
                unit_card = !b.prop && !b.surface && !unit_in_front;
            } else if (sp.under_xz && sp.under_kind && !unit_in_front) {
                // Not its card's texel: the scene shows the ground under it
                // there, at that ground's place (the angel's square over
                // the ice blocked the ray at its own depth and mirrored
                // the ice, a pale streak, JF).
                if (sp.under_kind[q] == kol::kSceneNone) {
                    out.sky = col;
                    break;
                }
                us = sp.under_xz[q];
            }
        } else if (sky_px(sp, q)) {
            out.sky = col;
            break;
        }
        const float ray = static_cast<float>(uw) - back * static_cast<float>(tt);
        if (static_cast<float>(us) + 2.0f < ray) continue;   // behind the ray: on, past it
        if (unit_card && static_cast<float>(us) > ray + back + 2.0f) continue;   // crossed its depth already
        // On a polished floor a thing far in front of the ray hangs over it
        // (a grounded one there would hide the floor's pixel itself): the
        // ray passes behind it, as behind a unit's card. Met, map 91's
        // colonnade lintel over the aisle gave each floor pixel under it its
        // own lowest pixel's colour, a comb of light and dark columns (the same
        // at -1). The ice and the water keep theirs.
        if (pass_hanging && static_cast<float>(us) > ray + back + 2.0f) continue;
        if (tt > reach) break;   // past the reach: blocked, faded out
        out.col = col;
        out.below = back * static_cast<float>(tt) * 0.25f;   // the depth gap / 4
        if (sky_at_hit || !unit_card) break;
        // A unit's: the sky it would have reached, behind it. Taken at the
        // unit's row, a ray stopped low took the light fade by the horizon,
        // and under an angel's mirror the ice went pale cyan in its shape
        // (JF). Terrain and props keep their row's (their look, approved on
        // maps 12 and 17). KOL_ICE_SKY_AT_HIT=1: the unit's row, as before.
        for (int t2 = tt + 1; t2 <= reach; ++t2) {
            const int sy = y - t2;
            if (sy < 0) break;
            const int sx = column_at(t2, false);
            if (sx < 0 || sx >= w) break;
            out.last_ry = sy;
            const std::size_t q2 = static_cast<std::size_t>(sy) * w + sx;
            if (sp.iface[q2] && iface_stops) break;
            if (!(sp.lift && sp.lift[q2]) && sky_px(sp, q2)) {
                out.sky = &src[q2 * 3];
                break;
            }
        }
        break;
    }
    return out;
}

// The reflections at half the resolution (asked 2026-10-26 to make
// reflections cheaper, perhaps at a lower resolution, then for the
// water and ice reflections to be lowered as
// well): one ray a 2x2 block, from its first pixel that
// reflects (`on`), shared by the block's; `wobble` the water's ripple at that
// pixel. Traced on the row pool. The mirrors are faint and soft (the water's
// 0.4, the ice's 0.35, the puddles'): a quarter of the rays.
// Then back to a ray a pixel (the user, the same day, asked for full-resolution
// reflections again): a block of one, the rays still on the row pool.
// KOL_REFL_HALF=1: 2x2 blocks.
int refl_block() {
    static const int bs = std::getenv("KOL_REFL_HALF") ? 2 : 1;
    return bs;
}
// The rays at 30 a second (asked 2026-10-26, map 102 in the rain on the
// Thor, the reflections ~6 ms of the worker's ~20, the user suggested updating the
// reflections at 30 fps): each picture traces every other row, by the
// picture's parity, and a pixel of the other rows takes the hit its place had
// one picture ago -- at the same place in the world: the scene moves with the
// camera's pan (kx + ox, ky + oy) by whole pixels, so the place one picture
// ago is (x - dx, y - dy). A hit kept from a picture before that is not used
// again (every pixel is at most one picture old), nor one from before a
// change of view size or the stick's look (the rays' slant), nor where the
// surface was not (a puddle grown). Its colours are copied (the hit points
// into the picture it was traced in). Only with a ray a pixel
// (refl_block() 1). KOL_REFL_EVERY_FRAME=1: every ray every picture.
struct RayKeep {
    struct Hit {
        std::uint8_t col[3], sky[3];
        std::uint8_t has_col = 0, has_sky = 0;
        float below = 0.0f;
        int last_ry = 0;
    };
    std::vector<Hit> now, before;   // this picture's (the hits point into it), the last's
    // A pixel's: 1 on the surface, 2 traced this picture (not kept).
    std::vector<std::uint8_t> now_f, before_f;
    int w = 0, h = 0, camx = 0, camy = 0;
    float pitch = 0.0f, cxp = 0.0f, K = 0.0f;
    unsigned long serial = 0;
    // The pictures worked out, for the rows' parity: by the guest's frame,
    // two pictures a shown picture apart on the Thor (a late one is shown
    // again, the guest goes on) kept the same rows traced every time.
    unsigned long jobs = 0;
    bool have = false;
    long traced = 0, kept = 0;   // KOL_REFL_KEEP_LOG=1
};
bool refl_every_frame() {
    static const bool v = std::getenv("KOL_REFL_EVERY_FRAME") != nullptr;
    return v;
}
template <class On, class Wob>
void block_rays(const kol::ScenePixels& sp, const std::uint8_t* src, const std::uint8_t* surface, int w, int h,
                const IceCam* cam, std::vector<RayHit>& out, On on, Wob wobble, RayKeep* keep = nullptr) {
    const int bs = refl_block();
    const int bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
    // Not cleared: a hit is read only on the surface's own pixels, and each
    // of those is written below (the clear, 5.6 MB a call on one thread, was
    // most of what the rays at 30 a second saved on the Thor).
    out.resize(static_cast<std::size_t>(bw) * bh);
    if (bs != 1 || refl_every_frame()) keep = nullptr;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const int camx = sp.kx + sp.ox, camy = sp.ky + sp.oy;
    bool reuse = false;
    int dx = 0, dy = 0;
    if (keep) {
        dx = camx - keep->camx;
        dy = camy - keep->camy;
        const unsigned long gap = sp.serial - keep->serial;
        reuse = keep->have && keep->w == w && keep->h == h && gap >= 1 && gap <= 4 &&
                std::abs(dx) < w / 4 && std::abs(dy) < h / 4 && keep->now.size() == n &&
                (!cam || (cam->pitch == keep->pitch && cam->cxp == keep->cxp && cam->K == keep->K));
        keep->before.swap(keep->now);
        keep->before_f.swap(keep->now_f);
        keep->now.resize(n);   // (only where now_f says)
        keep->now_f.assign(n, 0);
    }
    const int parity = keep ? static_cast<int>(++keep->jobs & 1u) : 0;
    row_pool().run(bh, std::max(1, fine_rows() / bs), [&](int, int b0, int b1) {
        for (int by = b0; by < b1; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                int ox = -1, oy = -1;
                for (int j = 0; j < bs && oy < 0; ++j)
                    for (int i = 0; i < bs; ++i) {
                        const int x = bx * bs + i, y = by * bs + j;
                        if (x < w && y < h && on(static_cast<std::size_t>(y) * w + x)) { ox = x; oy = y; break; }
                    }
                if (oy < 0) continue;
                const std::size_t k = static_cast<std::size_t>(oy) * w + ox;
                RayHit& r = out[static_cast<std::size_t>(by) * bw + bx];
                if (!keep) {
                    r = reflect_ray(sp, src, surface, w, h, ox, oy, cam, wobble(ox, oy, k));
                    continue;
                }
                RayKeep::Hit& kh = keep->now[k];
                const int px = ox - dx, py = oy - dy;
                bool reused = false;
                if (reuse && ((oy + parity) & 1) && px >= 0 && py >= 0 && px < w && py < h) {
                    const std::size_t pk = static_cast<std::size_t>(py) * w + px;
                    if (keep->before_f[pk] == 3u) {
                        kh = keep->before[pk];
                        kh.last_ry += dy;
                        keep->now_f[k] = 1;
                        reused = true;
                    }
                }
                if (!reused) {
                    const RayHit t = reflect_ray(sp, src, surface, w, h, ox, oy, cam, wobble(ox, oy, k));
                    keep->now_f[k] = 3;
                    kh.has_col = t.col != nullptr;
                    kh.has_sky = t.sky != nullptr;
                    if (t.col) std::copy_n(t.col, 3, kh.col);
                    if (t.sky) std::copy_n(t.sky, 3, kh.sky);
                    kh.below = t.below;
                    kh.last_ry = t.last_ry;
                }
                // The hit, from the kept one (which stays put until the next picture).
                r.col = kh.has_col ? kh.col : nullptr;
                r.sky = kh.has_sky ? kh.sky : nullptr;
                r.below = kh.below;
                r.last_ry = kh.last_ry;
            }
    });
    if (!keep) return;
    static const bool log = std::getenv("KOL_REFL_KEEP_LOG") != nullptr;
    long traced = 0, kept = 0;
    if (log)
        for (std::size_t k = 0; k < n; ++k)
            if (keep->now_f[k]) (keep->now_f[k] == 3u ? traced : kept) += 1;
    keep->w = w;
    keep->h = h;
    keep->camx = camx;
    keep->camy = camy;
    keep->serial = sp.serial;
    if (cam) {
        keep->pitch = cam->pitch;
        keep->cxp = cam->cxp;
        keep->K = cam->K;
    }
    keep->have = true;
    keep->traced += traced;
    keep->kept += kept;
    if (log && (dx || dy))
        std::fprintf(stderr, "refl_keep: serial %lu pan %d,%d reuse %d\n", sp.serial, dx, dy, reuse ? 1 : 0);
    if (log && (keep->jobs % 120) == 0)
        std::fprintf(stderr, "refl_keep: traced %ld kept %ld (this picture %ld / %ld)\n", keep->traced, keep->kept,
                     traced, kept);
}
inline const RayHit& block_ray(const std::vector<RayHit>& rays, int w, int x, int y) {
    const int bs = refl_block();
    if (bs == 1) return rays[static_cast<std::size_t>(y) * w + x];   // (a ray a pixel: no divisions)
    return rays[static_cast<std::size_t>(y / bs) * ((w + bs - 1) / bs) + x / bs];
}

// A vivid sky calmed for a reflection: past a saturation of 0.4 it goes
// toward its own grey, up to 0.75 of the way by 0.9 (the user, of map 12's
// blue sky in the ice: too saturated, then at 0.6 still slightly
// too much; map 17's night, 0.28-0.34, unchanged).
// KOL_ICE_SKY_DESAT=<0..1>: the most of the way (0.75).
void calm_sky(float* sk) {
    static const float k_desat = kol::env_num("KOL_ICE_SKY_DESAT", 0.75f);
    const float mx = std::max({sk[0], sk[1], sk[2]}), mn = std::min({sk[0], sk[1], sk[2]});
    const float sat = mx > 1.0f ? (mx - mn) / mx : 0.0f;
    const float pull = k_desat * std::clamp((sat - 0.4f) / 0.5f, 0.0f, 1.0f);
    if (pull <= 0.0f) return;
    const float g = 0.3f * sk[0] + 0.55f * sk[1] + 0.15f * sk[2];
    for (int ch = 0; ch < 3; ++ch) sk[ch] += (g - sk[ch]) * pull;
}

// `unlit`: the scene before apply_lighting (null: the foam unlit, as before).
// Map 73's waterfalls (Options > Special Effects; the user's pick D of the stills, with
// the thick vapor, after asking for a waterfall special effect on the
// waterfalls). The game only cycles a zigzag's colours on them. On the scene, after
// the light and the water, phased on the world: a sheet is a plane, so its across is the
// column from kx + ox and its down the height. A blue body in long falling bands, thin
// fast white strands, a broken bright lip, the cap's water sliding to it, foam at the foot
// and spreading on the pool, a few drops of spray (the vapor: FallVapor, on its squares).
// Its colours are the falls' own brightest and mean this picture, so the sepia
// flashback and the fades keep them; each pixel in its own shade.
float g_fall_tint[3] = {0.83f, 1.0f, 1.0f};   // the vapor's colour, the falls' brightest (fx_square, the next picture)
void falls_effect(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h, bool paint_vapor = false) {
    if (!sp.fall_parts || !sp.xz || !sp.y || !sp.kind) return;
    using kol::kMapFalls;
    using kol::MapFall;
    const std::uint8_t* const parts = sp.fall_parts;
    constexpr int kFirst = kol::kFallsFirstPart, kCount = kol::kFallCount;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    auto is_fall = [&](std::size_t k) { return parts[k] >= kFirst && parts[k] < kFirst + kCount; };
    int hi_l = -1;
    std::uint8_t hi[3] = {255, 255, 255};
    long sum_rgb[3] = {0, 0, 0}, sum_n = 0;
    for (std::size_t k = 0; k < pixels; ++k) {
        if (!is_fall(k)) continue;
        const std::uint8_t* c = scene + k * 3;
        const int l = c[0] * 2 + c[1] * 5 + c[2];
        if (l > hi_l) { hi_l = l; std::copy_n(c, 3, hi); }
        if ((!sp.ao || sp.ao[k] >= 250) && sp.kind[k] != kol::kSceneFlat) {
            for (int ch = 0; ch < 3; ++ch) sum_rgb[ch] += c[ch];
            ++sum_n;
        }
    }
    if (hi_l < 0) return;
    float body_rgb[3];
    for (int ch = 0; ch < 3; ++ch) body_rgb[ch] = sum_n ? static_cast<float>(sum_rgb[ch]) / sum_n : 0.6f * hi[ch];
    const float top_ch = static_cast<float>(std::max({hi[0], hi[1], hi[2], std::uint8_t{1}}));
    for (int ch = 0; ch < 3; ++ch) g_fall_tint[ch] = hi[ch] / top_ch;
    const float t = scene_secs(sp);
    const int cx = sp.kx + sp.ox;
    auto mix = [&](std::size_t k, const std::uint8_t* to, float a) {
        a = std::clamp(a, 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c)
            scene[k * 3 + c] = static_cast<std::uint8_t>(std::lround(scene[k * 3 + c] + (to[c] - scene[k * 3 + c]) * a));
    };
    auto frac = [](float v) { return v - std::floor(v); };
    // A strand per few columns: falling bright heads with tails above them, each its
    // own speed; two layers.
    auto strands = [&](float u, float v) {
        float sum = 0.0f;
        for (int layer = 0; layer < 2; ++layer) {
            const float wid = layer ? 2.0f : 3.5f;
            const float uu = u + 0.8f * std::sin(v * 0.04f + layer);
            const int id = static_cast<int>(std::floor(uu / wid)) + 31 * layer;
            const float speed = 95.0f * (0.8f + 0.5f * fx_hash(id, 1));
            const float len = 40.0f + 50.0f * fx_hash(id, 2);
            const float f = 1.0f - frac((v + speed * t) / len + fx_hash(id, 3));   // 1 head .. 0 tail (height grows upward)
            const float f2 = f * f, f4 = f2 * f2;
            sum += layer ? f4 * f4 * f4 * 0.6f : f4 * f2 * f;
        }
        return std::min(1.0f, sum);
    };
    row_pool().run(h, 8, [&](int, int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * w + x;
                const float u = static_cast<float>(x - cx);
                if (is_fall(k)) {
                    const MapFall& f = kMapFalls[parts[k] - kFirst];
                    const float Y = static_cast<float>(sp.y[k]);
                    if (sp.kind[k] == kol::kSceneFlat) {
                        // The cap: water sliding to the lip, paler near it.
                        const float sum = static_cast<float>(sp.xz[k]), d = u * 2.0f;
                        const float along = f.on_x ? (sum + d) * 0.5f : (sum - d) * 0.5f;
                        const float to_lip = (f.on_x ? f.x1 : f.z1) - along;
                        const float across = f.on_x ? (sum - d) * 0.5f : (sum + d) * 0.5f;
                        const int id = static_cast<int>(std::floor(across / 3.0f));
                        const float q = 1.0f - frac((to_lip + 30.0f * t) / (16.0f + 14.0f * fx_hash(id, 4)) + fx_hash(id, 5));
                        mix(k, hi, 0.45f * q * q * q + 0.4f * std::max(0.0f, 1.0f - to_lip / 6.0f));
                        continue;
                    }
                    const float below = Y - f.foot, from_top = f.top - Y;
                    // The body: the water's own colour, darker and paler in long bands
                    // falling down it, in the pixel's shade; a little of the art stays.
                    const float shade = sp.ao ? 0.35f + 0.65f * sp.ao[k] / 255.0f : 1.0f;
                    const float bands = 0.72f + 0.5f * fx_noise(u * 0.3f, (Y + 80.0f * t) * 0.016f);
                    std::uint8_t body[3], white[3];
                    for (int c = 0; c < 3; ++c) {
                        body[c] = static_cast<std::uint8_t>(std::clamp(body_rgb[c] * shade * bands, 0.0f, 255.0f));
                        white[c] = static_cast<std::uint8_t>(hi[c] * shade);
                    }
                    mix(k, body, 0.82f);
                    mix(k, white, 0.9f * strands(u, Y));
                    // The lip: bright, broken, where the water curls over.
                    if (from_top < 6.0f) mix(k, white, (1.0f - from_top / 6.0f) * (0.45f + 0.5f * fx_noise(u * 0.5f, t * 3.0f)));
                    // Foam where it lands.
                    if (below < 24.0f) {
                        const float n = fx_noise(u * 0.5f + 40.0f, (Y + 80.0f * t) * 0.28f);
                        const float b = 1.0f - std::max(0.0f, below) / 24.0f;
                        mix(k, white, b * std::sqrt(b) * std::clamp(n * 1.8f - 0.1f, 0.0f, 1.0f));
                    }
                } else if (sp.water && sp.water[k] && sp.kind[k] == kol::kSceneFlat) {
                    // Foam on the pool, spreading out from each foot.
                    const float sum = static_cast<float>(sp.xz[k]), d = u * 2.0f;
                    const float X = (sum + d) * 0.5f, Z = (sum - d) * 0.5f;
                    float a = 0.0f;
                    for (const MapFall& f : kMapFalls) {
                        if (f.foot > kol::kFallPool + 1.0f) continue;
                        const float out = f.on_x ? X - f.x1 : Z - f.z1;
                        if (out < -2.0f || out > 40.0f) continue;
                        const float across = f.on_x ? Z : X, lo_a = f.on_x ? f.z0 : f.x0, hi_a = f.on_x ? f.z1 : f.x1;
                        const float side = std::min(across - lo_a, hi_a - across);
                        if (side < -8.0f) continue;
                        const float edge = std::clamp((side + 8.0f) / 12.0f, 0.0f, 1.0f);
                        const float n0 = 1.0f - std::max(0.0f, out) / 40.0f;
                        const float near = n0 * std::sqrt(n0);
                        const float n = 0.6f * fx_noise(across * 0.22f, (out - 16.0f * t) * 0.25f) + 0.4f * fx_noise(across * 0.5f + 9.0f, (out - 22.0f * t) * 0.5f);
                        a = std::max(a, edge * near * std::clamp((n - 0.28f + 0.5f * near) * 2.2f, 0.0f, 1.0f));
                    }
                    if (a > 0.0f) mix(k, hi, a);
                }
            }
    });
    // Spray: a few drops thrown up at the feet on the pool, each on its own short arc.
    for (const MapFall& f : kMapFalls) {
        if (f.foot > kol::kFallPool + 1.0f) continue;
        const int x0 = cx + static_cast<int>(f.on_x ? (f.x1 - f.z1) * 0.5f : (f.x0 - f.z1) * 0.5f);
        const int x1 = cx + static_cast<int>(f.on_x ? (f.x1 - f.z0) * 0.5f : (f.x1 - f.z1) * 0.5f);
        for (int px = std::max(0, x0 - 8); px <= std::min(w - 1, x1 + 8); ++px) {
            const float wx = static_cast<float>(px - cx);
            const float a = std::clamp(f.on_x ? f.x1 - 2.0f * wx : f.z1 + 2.0f * wx, f.on_x ? f.z0 : f.x0, f.on_x ? f.z1 : f.x1);
            const float sum = f.on_x ? f.x1 + a : a + f.z1;
            const int foot_row = static_cast<int>(sum * 0.25f - f.foot * 0.5f) + sp.ky + sp.oy;
            const float edge = std::clamp(1.0f - std::max(static_cast<float>(x0 - px), static_cast<float>(px - x1)) / 8.0f, 0.0f, 1.0f);
            for (int i = 0; i < 2; ++i) {
                const int id = px - cx + i * 131 + static_cast<int>(f.x0 + f.z0) * 7;
                const float ph = t / (0.7f + 0.6f * fx_hash(id, 6)) + fx_hash(id, 7);
                const float q = frac(ph);
                const float height = (10.0f + 18.0f * fx_hash(id, 8)) * 4.0f * q * (1.0f - q);
                const int dy = foot_row - static_cast<int>(height);
                if (fx_hash(id, 9 + (static_cast<int>(std::floor(ph)) & 0xFFFF)) > 0.3f || dy < 0 || dy >= h) continue;
                const std::size_t k = static_cast<std::size_t>(dy) * w + px;
                if (static_cast<float>(sp.xz[k]) <= sum + 20.0f) mix(k, hi, 0.9f * edge * (1.0f - q * 0.5f));
            }
        }
    }
    // Without the perspective no squares are drawn: the vapor goes on the picture, what
    // stands in front of it (nearer, and not the pool's flat water) clear of it.
    if (!paint_vapor || !sp.fall_vapor) return;
    for (int fi = 0; fi < kCount; ++fi) {
        const kol::FallVapor& v = sp.fall_vapor[fi];
        const MapFall& f = kMapFalls[fi];
        const float out = (f.foot > kol::kFallPool + 1.0f ? 0.5f : 1.0f) * 24.0f;
        const int fy = sp.ky + sp.oy;
        for (int cy = 0; cy + 1 < v.h; ++cy)
            for (int cx2 = 0; cx2 + 1 < v.w; ++cx2) {
                const float* c = &v.cells[(static_cast<std::size_t>(cy) * v.w + cx2) * 2];
                const float* d = c + static_cast<std::size_t>(v.w) * 2;
                if (c[0] + c[2] + d[0] + d[2] <= 0.0f) continue;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        const int px = v.wx0 + cx2 * 2 + i + cx, py = v.wy0 + cy * 2 + j + fy;
                        if (px < 0 || py < 0 || px >= w || py >= h) continue;
                        const std::size_t k = static_cast<std::size_t>(py) * w + px;
                        const float wx = static_cast<float>(px - cx);
                        const float plane = f.on_x ? 2.0f * (f.x1 + out) - 2.0f * wx : 2.0f * (f.z1 + out) + 2.0f * wx;
                        const bool pool = sp.kind[k] == kol::kSceneFlat && sp.water && sp.water[k];
                        const float fade = pool ? 1.0f : 1.0f - fx_smooth(-8.0f, 48.0f, static_cast<float>(sp.xz[k]) - plane);
                        if (fade <= 0.0f) continue;
                        const float tx = i * 0.5f, ty = j * 0.5f;
                        const float a = (c[0] * (1 - tx) + c[2] * tx) * (1 - ty) + (d[0] * (1 - tx) + d[2] * tx) * ty;
                        const float tone = (c[1] * (1 - tx) + c[3] * tx) * (1 - ty) + (d[1] * (1 - tx) + d[3] * tx) * ty;
                        std::uint8_t to[3];
                        for (int ch = 0; ch < 3; ++ch) to[ch] = static_cast<std::uint8_t>(std::min(255.0f, top_ch * g_fall_tint[ch] * tone));
                        mix(k, to, a * fade);
                    }
            }
    }
}

void water_effect(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h, const IceCam* cam = nullptr,
                  const std::uint8_t* unlit = nullptr) {
    const std::uint8_t* water = sp.water;
    if (!water) return;
    static const float kRefl = kol::env_num_or("KOL_WATER_REFL", 0.4f);
    // The foam's strength at the shore (0.7 before 2026-10-12, when it made
    // bright one-pixel dots along the jagged shore line).
    constexpr float kFoam = 0.45f, kTwoPi = 6.2831853f;
    // How far (rows) a reflection fades out: 30 (the user picked C, 20, of
    // 60, 30, 20, 12 on states/thor/0105, map 19, 2026-11-02: the user found them too
    // long, then after seeing 20 rows asked for 30). At
    // 60 a thing's reflection stayed strong far from its foot and the sky's
    // reached 60 rows into the water, so water whose ray met nothing (in
    // front of the shore's land) stood out as long dark bands.
    // KOL_WATER_FADE=<rows>: another length (60 as before).
    static const float kFade = kol::env_num_or("KOL_WATER_FADE", 30.0f);
    // A sine by table: a turn in 256 steps.
    static const auto sine = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[i] = std::sin(kTwoPi * i / 256.0f);
        return t;
    }();
    auto sin_turns = [&](float turns) { return sine[static_cast<unsigned>(static_cast<int>(std::floor(turns * 256.0f))) & 255u]; };
    const float t = scene_secs(sp);
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    static std::vector<std::uint8_t> src;
    src.assign(scene, scene + pixels * 3);
    auto wet = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && water[static_cast<std::size_t>(y) * w + x] == 1u; };
    // The waves are phased on the world, not on the scene's pixels: the
    // scene is the screen (a column is (X - Z) / 2 + kx, kx following the
    // guest's scroll), so a pattern on its pixels stood still while the
    // water slid under it whenever the camera moved (2026-10-12, the user reported
    // the water effect looked wrong on the Z axis: the waves' small white dots
    // on the shore looked unnatural as the camera moved). On the water's flat
    // top, X + Z is the scene's depth (ScenePixels::xz) and X - Z twice the
    // column from kx + ox (the scene's overscan: ox shrinks by 32 as the
    // stick lowers the camera, and from kx alone the waves, the ice's sheen
    // and the wet look's puddles, glints and drips slid 32 columns: the user,
    // 2026-10-26, saw a shift to the right with the right stick down,
    // even in the dripping water). (world_xz, shared by
    // the ice and the wet look.)
    // The first row of each water pixel's run down its column, worked out
    // first, so the pixels can go in rows on the row pool (each reads only
    // `src`; one thread, column by column, it was 5 of the worker's 9.5 ms
    // on map 58 at night, the Thor's worker at 97%, 2026-10-26).
    static std::vector<std::int16_t> run_top;
    static std::vector<int> col_top;
    run_top.resize(pixels);
    col_top.assign(static_cast<std::size_t>(w), -1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            if (water[k] != 1u) { col_top[x] = -1; continue; }
            if (col_top[x] < 0) col_top[x] = y;
            run_top[k] = static_cast<std::int16_t>(col_top[x]);
        }
    // The reflected rays, a 2x2 block's at a time (block_rays).
    auto wobble_at = [&](int x, std::size_t k) {
        float X, Z;
        world_xz(sp, x, k, &X, &Z);
        return static_cast<int>(std::lround(1.6f * sin_turns((X + Z) / 20.0f - t / 1.2f + (X - Z) / 110.0f)));
    };
    static std::vector<RayHit> rays;
    static RayKeep water_keep;
    static const bool column_mirror = kol::env_set("KOL_WATER_COLUMN_MIRROR");   // (see the reflection below)
    if (!column_mirror)
        block_rays(sp, src.data(), water, w, h, cam, rays, [&](std::size_t k) { return water[k] == 1u; },
                   [&](int x, int, std::size_t k) { return wobble_at(x, k); }, &water_keep);
    // No shore foam on kNoFoamMaps: map 16's marsh, puddles in the grass
    // (the user, 2026-11-01, states/thor/0072, asked to drop the white
    // border effect on the water there).
    const bool no_foam = kol::map_flag(kol::MapFlag::NoFoam, sp.map);   // (map_settings.cfg `no_foam`)
    row_pool().run(h, fine_rows(), [&](int, int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * w + x;
                if (water[k] != 1u || range_plain(sp, k)) continue;
                const int top = run_top[k];
                float X, Z;
                world_xz(sp, x, k, &X, &Z);
                // The ripple: the colour of the water a pixel to the side, where
                // two slow waves crossing (along X and along Z) both crest.
                const float rip = sin_turns(X / 22.0f - t / 1.5f) + sin_turns(Z / 30.0f + t / 2.0f);
                const int dx = rip > 1.1f ? 1 : rip < -1.1f ? -1 : 0;
                const std::uint8_t* s = wet(x + dx, y) ? &src[(k + dx) * 3] : &src[k * 3];
                float c[3] = {s[0] * 0.82f, s[1] * 0.82f, s[2] * 0.82f};
                const float lum = 0.3f * s[0] + 0.55f * s[1] + 0.15f * s[2];
                // The reflection, wobbling, bluer and darker through the water:
                // the reflected ray (reflect_ray, as the ice's: each thing
                // mirrored about its own foot and following the camera; the
                // user noted the new reflection pattern (the chevron fix and
                // camera movement) was missing on the water), its
                // column moved by the ripple's wobble. A thing fades by its
                // foot's distance up, the sky by the pixel's below the water's
                // edge, as the column mirror did. KOL_WATER_COLUMN_MIRROR=1: the
                // column mirrored about the water's top row, as before.
                // (The mirrored row and the wobbled column, the column mirror's
                // alone: the ray's wobble is block_rays', wobble_at.)
                const int d = y - top;
                static constexpr float kTint[3] = {0.72f, 0.86f, 0.98f};
                if (column_mirror) {
                    const int ry = top - 2 - d;
                    const int wob = static_cast<int>(std::lround(1.6f * sin_turns((X + Z) / 20.0f - t / 1.2f + (X - Z) / 110.0f)));
                    const int rx = x + wob;
                    if (ry >= 0 && rx >= 0 && rx < w && !wet(rx, ry) && !sp.iface[static_cast<std::size_t>(ry) * w + rx]) {
                        float a = kRefl * std::max(0.0f, 1.0f - d / kFade);
                        if (lum > 200.0f) a *= 0.25f;
                        const std::uint8_t* r = &src[(static_cast<std::size_t>(ry) * w + rx) * 3];
                        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - a) + r[ch] * kTint[ch] * a;
                    }
                } else {
                    const RayHit& hit = block_ray(rays, w, x, y);
                    const std::uint8_t* r = hit.col ? hit.col : hit.sky;
                    // Where the ray met no thing, the sky's gradient mirrored
                    // over the view: water low on the screen (near) takes the
                    // sky's top, water higher up its lower rows, as a mirror
                    // shows it. The sky where each ray left the land came in
                    // patches in the land's shape, and a ray that met nothing
                    // took none (the user, 2026-11-02, asked for reflections
                    // of terrain and objects only, the rest the sky's
                    // gradient; B of four: where each ray leaves,
                    // mirrored, the same row, one colour).
                    // KOL_WATER_SKY_AT_EXIT=1: where each ray leaves, as before.
                    static const bool sky_at_exit = std::getenv("KOL_WATER_SKY_AT_EXIT") != nullptr;
                    std::uint8_t grad[3];
                    if (!hit.col && !sky_at_exit && sp.row_backdrop && sp.vh > 0) {
                        const std::uint8_t* rows = g_sky_rows.empty() ? sp.row_backdrop : g_sky_rows.data();
                        const int ry = std::clamp((sp.vy + sp.vh - 1) - (y - sp.vy), sp.vy, sp.vy + sp.vh - 1);
                        std::copy_n(rows + static_cast<std::size_t>(ry) * 3, 3, grad);
                        r = grad;
                    }
                    if (r) {
                        float a = kRefl * std::max(0.0f, 1.0f - (hit.col ? hit.below : static_cast<float>(d)) / kFade);
                        // The sky at one strength over all the water: faded
                        // from the water's top edge over kFade rows, it was
                        // strongest right under every shore and rock, and on
                        // map 19, whose sky calms to a dark blue, each one had
                        // a dark band going down into the water (the user,
                        // states/thor/0109, reported a dark reflection going down
                        // from each part touching the water; C of
                        // now / even 0.15 / 0.25 / 0.25 greyed). Things still
                        // fade from their foot. KOL_WATER_SKY_REFL=<a>: another
                        // strength; KOL_WATER_SKY_FADE=1: faded from the edge,
                        // as before.
                        static const float sky_even = kol::env_num_or("KOL_WATER_SKY_REFL", 0.25f);
                        static const bool sky_fade = std::getenv("KOL_WATER_SKY_FADE") != nullptr;
                        if (!hit.col && !sky_fade) a = sky_even;
                        if (lum > 200.0f) a *= 0.25f;
                        float rr[3] = {static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2])};
                        if (!hit.col) calm_sky(rr);   // the sky calmed (DQ's vivid blue)
                        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - a) + rr[ch] * kTint[ch] * a;
                    }
                }
                // The shore's foam: a soft band along the water's edge (a
                // neighbour on the shore; not the sky, a sprite or the scene's
                // border), fading over three pixels in, that swells and ebbs in
                // long stretches of the shore (phased on the world). It was
                // near-white at 0.7 with a short screen-space pattern and the
                // diagonal neighbours too, which read as dots.
                auto shore = [&](int ox, int oy) {
                    const int nx = x + ox, ny = y + oy;
                    return nx >= 0 && ny >= 0 && nx < w && ny < h && water[static_cast<std::size_t>(ny) * w + nx] == 2u;
                };
                auto ring = [&](int r) { return shore(r, 0) || shore(-r, 0) || shore(0, r) || shore(0, -r); };
                // (None on kNoFoamMaps: no_foam above.)
                const float edge = no_foam ? 0.0f : ring(1) ? 1.0f : ring(2) ? 0.5f : ring(3) ? 0.2f : 0.0f;
                if (edge > 0.0f) {
                    const float surge = 0.5f + 0.5f * sin_turns((X + Z) / 90.0f + (X - Z) / 140.0f - t / 3.0f);
                    const float f = std::clamp(surge * 1.4f - 0.3f, 0.0f, 1.0f) * kFoam * edge;
                    // Its colour is ours, so the scene's fade darkens it as it
                    // does the art (ScenePixels::scene_bright): through the fade
                    // from map 101 to map 102 the shore stood out in near-white
                    // lines on the black screen and over the fade in (JK).
                    // And the light on the water under it, as the lighting
                    // changed that pixel channel by channel (asked 2026-10-26 for
                    // the water effect's borders to take the
                    // light too): dark at night, warm by a torch, darker in a
                    // shadow. Painted after the light, it was near-white on
                    // the night ship's black water.
                    static constexpr float kFoamRgb[3] = {215.0f, 232.0f, 235.0f};
                    const float fb = std::clamp(sp.scene_bright, 0.0f, 1.0f);
                    float lit[3] = {1.0f, 1.0f, 1.0f};
                    if (unlit)
                        for (int ch = 0; ch < 3; ++ch)
                            lit[ch] = std::clamp(static_cast<float>(src[k * 3 + ch]) /
                                                     std::max(24.0f, static_cast<float>(unlit[k * 3 + ch])),
                                                 0.0f, 1.6f);
                    for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - f) + kFoamRgb[ch] * fb * lit[ch] * f;
                }
                for (int ch = 0; ch < 3; ++ch)
                    scene[k * 3 + ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0l, 255l));
            }
        }
    });
}

// The wet look's strength (wet_effect): 3 with Options > Wet Rain (the
// user's pick of the three, 2026-10-26: the strongest), 0 off.
// KOL_WET=0..3 sets it and holds the wetness full (the mockups).
int wet_env() {
    static const int lv = [] {
        const char* v = std::getenv("KOL_WET");
        return v && *v ? std::clamp(std::atoi(v), 0, 3) : -1;
    }();
    return lv;
}
int wet_level() {
    const int env = wet_env();
    if (env >= 0) return env;
    return kol::setting_on(kol::Setting::WetRain) ? 3 : 0;
}

// Everything wet in the rain (asked 2026-10-26 for everything to look
// wet, with water seeming to drip down vertical
// surfaces; Options > Wet Rain, strong: wet_level). The
// map's own surfaces (units and lifted props are cards drawn over this):
// darker and a little more saturated, as wet stone and wood are; on the
// floors, puddles (a noise fixed to the world) mirroring what stands over
// them by the water's ray, and glints by the light the pixel takes (lit over
// unlit: the lamps at night); on the walls, drips running down, a pixel
// wide, in columns fixed to the world (a column from kx is (X - Z) / 2),
// a bright head and a trail catching the light.
// How wet, 0..1 (the user asked for puddles in battles to grow
// the longer it rains and to shrink after the rain): in a battle it
// builds up while it rains, to full in kWetGrow seconds (sooner in heavy
// rain), and dries in kWetDry after; a story scene's rain finds it wet. A
// map loaded (a new one, or the same one again), or a jump in the frame
// count (a state loaded), starts it again.
// The surfaces darken and shine first, the puddles come once they are wet
// and spread as it climbs (lower on the same noise: each grows from where
// it began, and shrinks back to it), and the walls stop dripping kDripStop
// seconds after the rain.
struct WetV { float dark, sat, puddle, refl, glint, drips, drip_dark, drip_head; };
// A drip's colour by day: the sky's, its saturation ((max - min) / max) held
// to 0.15 (drip_max_sat; the user, 2026-10-31, after trying the rain's colour,
// asked to return to the sky's colour with a low maximum
// saturation): a blue or dusk sky made coloured streaks.
// KOL_DRIP_MAX_SAT=<0..1> sets it (1: as before), for mockups.
inline float drip_max_sat() {
    static const float v = kol::env_num_clamp("KOL_DRIP_MAX_SAT", 0.0f, 1.0f, 0.15f);
    return v;
}
inline void drip_sky(const float* sky, float* out) {
    const float kDripMaxSat = drip_max_sat();
    const float hi = std::max(sky[0], std::max(sky[1], sky[2]));
    const float lo = std::min(sky[0], std::min(sky[1], sky[2]));
    const float sat = hi > 0.0f ? (hi - lo) / hi : 0.0f;
    const float m = 0.3f * sky[0] + 0.55f * sky[1] + 0.15f * sky[2];
    const float k = sat > kDripMaxSat ? kDripMaxSat / sat : 1.0f;
    for (int ch = 0; ch < 3; ++ch) out[ch] = m + (sky[ch] - m) * k;
}
// The wet look this picture, for the wet pass and the atlas's lifted walls
// and props (filled before it): the strengths scaled by how wet it is, off
// when `ok` is false. The wetness moves by the frames since the last call,
// so the second call of a picture changes nothing. `sky`: the frame's sky as
// the wet pass last measured it (the atlas's drips catch it).
struct WetNow { bool ok = false; WetV v{}; float sky[3] = {0.0f, 0.0f, 0.0f}; };
float g_wet_sky[3] = {150.0f, 160.0f, 170.0f};
WetNow wet_now(const kol::ScenePixels& sp) {
    WetNow out;
    const int lv = wet_level();
    static float wetness = 0.0f, dry_for = 1.0e9f;
    static unsigned long last_serial = 0;
    static int last_map = -2;
    static unsigned last_loads = 0;
    {
        // Rain in a story scene builds up over kWetGrowStory (map 102's
        // opening went wet in a frame: rain and overcast arrived
        // instantly).
        constexpr float kWetGrow = 150.0f, kWetGrowStory = 30.0f, kWetDry = 240.0f, kJump = 600.0f;
        // (Not where the rain rises, map_settings.cfg `rain_rises`: it never lands.)
        const bool raining = sp.rain_level >= 0 && !kol::map_flag(kol::MapFlag::RainRises, sp.map);
        static const float speed = [] {   // KOL_WET_SPEED=<x>: build up and dry x times faster (tests)
            const char* e = std::getenv("KOL_WET_SPEED");
            return e && *e ? std::max(0.01f, static_cast<float>(std::atof(e))) : 1.0f;
        }();
        const float gap = static_cast<float>(sp.serial) - static_cast<float>(last_serial);
        const float dt = gap * speed;
        // A map the game loads (again: the same number reopened after it
        // was closed, the user, 2026-10-27, asked for the wetness to reset
        // whenever a map closes) starts dry, a story scene's in
        // the rain too (it started wet: after the rainy battle, maps 3 and 59
        // came in dripping wet; the user, 2026-10-28, expected a new map
        // to reset the wetness). KOL_WET_STORY_STARTS_WET=1:
        // as before.
        static const bool story_wet = std::getenv("KOL_WET_STORY_STARTS_WET") != nullptr;
        if (sp.map != last_map || sp.map_loads != last_loads || sp.serial < last_serial || gap > kJump) {
            wetness = story_wet && sp.story && raining ? 1.0f : 0.0f;
            dry_for = raining ? 0.0f : 1.0e9f;
        } else if (raining) {
            const float heavy = 0.6f + 0.4f * static_cast<float>(sp.rain_level);   // 0..2: 0.6, 1, 1.4
            wetness = std::min(1.0f, wetness + dt / 60.0f * heavy / (sp.story ? kWetGrowStory : kWetGrow));
            dry_for = 0.0f;
        } else {
            wetness = std::max(0.0f, wetness - dt / 60.0f / kWetDry);
            dry_for += dt / 60.0f;
        }
        last_serial = sp.serial;
        last_map = sp.map;
        last_loads = sp.map_loads;
    }
    const bool held = wet_env() > 0;   // a mockup: full
    const float wet = held ? 1.0f : wetness;
    if (lv == 0 || wet <= 0.0f) return out;
    constexpr float kDripStop = 30.0f;
    const float surf = std::min(1.0f, wet * 2.0f);                          // darkening, sheen, glints
    const float pools = std::clamp((wet - 0.15f) / 0.85f, 0.0f, 1.0f);      // the puddles' share
    const float drip = held ? 1.0f : std::min(1.0f, wet * 3.0f) * std::max(0.0f, 1.0f - dry_for / kDripStop);
    static constexpr WetV kV[3] = {{0.10f, 0.10f, 0.22f, 0.35f, 0.5f, 0.05f, 0.12f, 0.45f},
                                   {0.16f, 0.16f, 0.30f, 0.45f, 0.8f, 0.09f, 0.18f, 0.65f},
                                   {0.22f, 0.22f, 0.38f, 0.55f, 1.0f, 0.14f, 0.25f, 0.85f}};
    WetV v = kV[std::clamp(lv, 1, 3) - 1];
    v.dark *= surf;
    v.sat *= surf;
    v.glint *= surf;
    v.puddle *= pools;
    // No puddles on kNoPuddleMaps: map 16's marsh has water of its own (the
    // user, 2026-11-01, states/thor/0072, asked for no puddles on this map).
    if (kol::map_flag(kol::MapFlag::NoPuddles, sp.map)) v.puddle = 0.0f;   // (map_settings.cfg `no_puddles`)
    v.drips *= drip;
    // The drips at 0.6 of their strength (the user, 2026-11-02, found the rain
    // drips a bit too opaque, picked from 1.0 / 0.7 / 0.5 / 0.35 stills,
    // shots/wet_drips/): their trail and bright head both.
    // KOL_DRIP_OPACITY=<0..2>: another (1: as before).
    static const float drip_opacity = kol::env_num_clamp("KOL_DRIP_OPACITY", 0.0f, 2.0f, 0.6f);
    v.drip_dark *= drip_opacity;
    v.drip_head *= drip_opacity;
    out.ok = true;
    out.v = v;
    std::copy_n(g_wet_sky, 3, out.sky);
    return out;
}
// The scene before its light, for the water's foam (water_effect's
// `unlit`) and the wet look's light; null without either. KOL_FOAM_UNLIT=1: the foam unlit, as before.
const std::uint8_t* foam_unlit(const kol::ScenePixels& sp, const std::uint8_t* scene, std::size_t pixels) {
    static const bool off = std::getenv("KOL_FOAM_UNLIT") != nullptr;
    static std::vector<std::uint8_t> unlit;
    if ((off || !sp.water) && wet_level() == 0) return nullptr;
    // Nor on a map with no water while it is dry (no rain, or after a map
    // loads): the wet look leaves at once then and reads none of it, and
    // with Wet Rain on this copied the whole scene every picture on every
    // map. (wet_now: the second call of a picture changes nothing, and
    // both providers call it again in wet_effect.)
    if (!sp.water && !wet_now(sp).ok) return nullptr;
    unlit.assign(scene, scene + pixels * 3);
    return unlit.data();
}
inline std::uint32_t wet_hash(std::uint32_t a) {
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}
// Wet: darker, more saturated (the scene's surfaces, the atlas's lifted
// walls and props).
inline void wet_tone(float* c, const WetV& v) {
    const float m = (c[0] + c[1] + c[2]) / 3.0f;
    for (int ch = 0; ch < 3; ++ch) c[ch] = (m + (c[ch] - m) * (1.0f + v.sat)) * (1.0f - v.dark);
}
// Drips down the walls (the scene's, and lifted walls' texels): a column
// (X - Z) / 2 fixed to the world, `col` its pixel column from kx + ox, a
// drop running down it every period; at height Y, at time t, `s1` how far
// below the last drop's head it is. False where the column has no drops.
inline bool wall_drip(int col, float Y, float t, float drips, float* s1) {
    const std::uint32_t hc = wet_hash(static_cast<std::uint32_t>(col) * 2654435761u);
    if ((hc & 1023u) >= static_cast<std::uint32_t>(drips * 1024.0f)) return false;
    const float speed = 18.0f + ((hc >> 10) & 31u);           // height units a second
    const float period = 50.0f + ((hc >> 15) & 63u);          // between drops down a column
    const float phase = ((hc >> 21) & 1023u) * 0.37f;
    *s1 = std::fmod(Y + speed * t + phase, period);
    if (*s1 < 0.0f) *s1 += period;
    return true;
}
// A drip's streak on colour c, `s1` behind its head (0 .. tail): a trail
// toward trail_mul x rgb fading over `tail`, and over the first `head` its
// bright head toward head_mul x rgb.
inline void drip_streak(float* c, float s1, float tail, float head, const WetV& v, const float* rgb, float trail_mul,
                        float head_mul) {
    const float trail = 1.0f - s1 / tail;
    const float a = v.drip_dark * 1.6f * trail * trail;
    for (int ch = 0; ch < 3; ++ch) c[ch] += (trail_mul * rgb[ch] - c[ch]) * a;
    if (s1 < head) {
        const float b = v.drip_head * (1.0f - s1 / head);
        for (int ch = 0; ch < 3; ++ch) c[ch] += (head_mul * rgb[ch] - c[ch]) * b;
    }
}
// A puddle's mirror at its pixel's ray (`hit`): in rr the colour it mirrors
// (the thing met, or the sky: the one it left at, else the frame's), and the
// fade with the thing's foot's distance up. The sky held to a saturation of
// 0.25 about its own grey, as the drips' (drip_sky, 0.15): a vivid sky
// calmed by calm_sky still came out navy (map 19's pure blue backdrop in the
// rain, about (40,42,83) on the Thor; the user, 2026-11-02, found the puddles
// odd in blue, then 0.25 of A-D: now / 0.3 / 0.15 / 0).
// KOL_PUDDLE_SKY_SAT=<0..1>: another saturation; negative: calm_sky, as
// before.
inline float puddle_mirror(const RayHit& hit, const float* sky, float* rr) {
    static const float max_sat = kol::env_num_or("KOL_PUDDLE_SKY_SAT", 0.25f);
    const std::uint8_t* r = hit.col ? hit.col : hit.sky;
    rr[0] = sky[0];
    rr[1] = sky[1];
    rr[2] = sky[2];
    if (r) {
        rr[0] = r[0];
        rr[1] = r[1];
        rr[2] = r[2];
        if (!hit.col && max_sat < 0.0f) calm_sky(rr);
    }
    if (!hit.col && max_sat >= 0.0f) {
        const float hi = std::max({rr[0], rr[1], rr[2]}), lo = std::min({rr[0], rr[1], rr[2]});
        const float sat = hi > 0.0f ? (hi - lo) / hi : 0.0f;
        const float m = 0.3f * rr[0] + 0.55f * rr[1] + 0.15f * rr[2];
        const float k = sat > max_sat ? max_sat / sat : 1.0f;
        for (int ch = 0; ch < 3; ++ch) rr[ch] = m + (rr[ch] - m) * k;
    }
    return std::max(0.0f, 1.0f - (hit.col ? hit.below : 0.0f) / 60.0f);
}

void wet_effect(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h, const IceCam* cam,
                const std::uint8_t* unlit) {
    const WetNow wn = wet_now(sp);
    if (!wn.ok || !sp.kind || !sp.xz || !sp.y || !unlit) return;
    WetV v = wn.v;
    // The scene's fade (ScenePixels::scene_bright): the streaks and glints
    // take a light of their own (the sky's, the lamps', with a floor), so
    // they stayed lit while the battle's end faded to black (the user,
    // states/thor/0011, A on "Congratulations!": BLDY 1 -> 16 over BG0-3).
    // KOL_WET_NO_FADE=1: as before.
    static const bool no_fade = std::getenv("KOL_WET_NO_FADE") != nullptr;
    const float fb = no_fade ? 1.0f : std::clamp(sp.scene_bright, 0.0f, 1.0f);
    const float t = scene_secs(sp);
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    static const bool dbg = std::getenv("KOL_WET_DEBUG") != nullptr;   // (the puddles' rays, below)
    auto h2 = [&](int a, int b) { return wet_hash(static_cast<std::uint32_t>(a) * 73856093u ^ static_cast<std::uint32_t>(b) * 19349663u); };
    // Value noise on the world's X, Z (cells of `s` units), 0..1.
    auto noise = [&](float X, float Z, float s) {
        const float fx = X / s, fz = Z / s;
        const int ix = static_cast<int>(std::floor(fx)), iz = static_cast<int>(std::floor(fz));
        float ux = fx - ix, uz = fz - iz;
        ux = ux * ux * (3.0f - 2.0f * ux);
        uz = uz * uz * (3.0f - 2.0f * uz);
        auto at = [&](int a, int b) { return (h2(a, b) & 0xFFFFu) / 65535.0f; };
        const float a = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * ux;
        const float b = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * ux;
        return a + (b - a) * uz;
    };
    auto surface = [&](std::size_t k) {
        const unsigned kd = sp.kind[k];
        return (kd == kol::kSceneFlat || kd == kol::kSceneUpright) && !(sp.water && sp.water[k] == 1u) &&
               !(sp.iface && sp.iface[k]) && !(sp.ice && sp.ice[k]) && !range_plain(sp, k);
    };
    kol::WorkerTimer lt("wet.sky");
    // The sky's colour, for what a wet floor mirrors where nothing stands
    // over it: the mean of the backdrop's pixels in the frame.
    float sky[3] = {0.0f, 0.0f, 0.0f};
    {
        double acc[3] = {0.0, 0.0, 0.0};
        std::size_t n = 0;
        double ground = 0.0;
        std::size_t ng = 0;
        for (std::size_t k = 0; k < pixels; k += 3) {
            if (sp.iface && sp.iface[k]) continue;
            const std::uint8_t* c = scene + k * 3;
            const bool bd = sp.scene_codes ? (sp.scene_codes[k] & 0x0Fu) == kol::kLayerBackdrop : sp.kind[k] == kol::kSceneNone;
            if (!bd) {
                if (sp.kind[k] != kol::kSceneNone) { ground += 0.3 * c[0] + 0.55 * c[1] + 0.15 * c[2]; ++ng; }
                continue;
            }
            if (c[0] + c[1] + c[2] < 24) continue;   // a letterbox's black (map 102's narration), not a sky
            for (int ch = 0; ch < 3; ++ch) acc[ch] += c[ch];
            ++n;
        }
        if (n > 64) {
            for (int ch = 0; ch < 3; ++ch) sky[ch] = static_cast<float>(acc[ch] / n);
        } else {
            // No sky in view: an overcast grey a third brighter than the
            // ground it lights. (The black letterbox made every puddle
            // black; a fixed grey was too light in map 102's dusk.)
            static constexpr float kTint[3] = {0.95f, 1.0f, 1.07f};
            const float l = ng ? static_cast<float>(ground / static_cast<double>(ng)) : 100.0f;
            for (int ch = 0; ch < 3; ++ch) sky[ch] = std::min(255.0f, 1.35f * l * kTint[ch]);
        }
        std::copy_n(sky, 3, g_wet_sky);
    }
    // The drips' colour by day (drip_sky), once a picture: the sky's.
    float dsky[3];
    drip_sky(sky, dsky);
    // A level floor: its pixel's neighbours up and down its column on the
    // same height (a roof's slope changes it a row; puddles stand on level
    // ground only).
    auto level = [&](int x, int y) {
        const std::size_t k = static_cast<std::size_t>(y) * w + x;
        for (int dy : {-1, 1}) {
            const int yy = y + dy;
            if (yy < 0 || yy >= h) continue;
            const std::size_t q = static_cast<std::size_t>(yy) * w + x;
            if (sp.kind[q] == kol::kSceneFlat && sp.y[q] != sp.y[k]) return false;
        }
        return true;
    };
    lt.lap("wet.mask");
    // A slope's way down (a roof's): the plane through the pixel and two
    // neighbours 2 px right and 2 down on the same surface (flat kind, the
    // height within 12), its gradient in the world's X, Z; false where it is
    // level or not a plane there. (fx, fz): the unit vector downhill.
    auto downhill = [&](int x, int y, float* fx, float* fz) {
        constexpr int d = 2;
        if (x + d >= w || y + d >= h) return false;
        const std::size_t k = static_cast<std::size_t>(y) * w + x;
        const std::size_t kr = k + d, kd = k + static_cast<std::size_t>(d) * w;
        if (sp.kind[kr] != kol::kSceneFlat || sp.kind[kd] != kol::kSceneFlat) return false;
        const float y0 = static_cast<float>(sp.y[k]);
        const float dy1 = static_cast<float>(sp.y[kr]) - y0, dy2 = static_cast<float>(sp.y[kd]) - y0;
        if (std::abs(dy1) > 12.0f || std::abs(dy2) > 12.0f) return false;
        float X0, Z0, X1, Z1, X2, Z2;
        world_xz(sp, x, k, &X0, &Z0);
        world_xz(sp, x + d, kr, &X1, &Z1);
        world_xz(sp, x, kd, &X2, &Z2);
        const float a = X1 - X0, b = Z1 - Z0, c = X2 - X0, e = Z2 - Z0;
        const float det = a * e - b * c;
        if (std::abs(det) < 1e-3f) return false;
        const float gx = (dy1 * e - b * dy2) / det, gz = (a * dy2 - dy1 * c) / det;
        const float g = std::sqrt(gx * gx + gz * gz);
        if (g < 0.2f) return false;
        *fx = -gx / g;
        *fz = -gz / g;
        return true;
    };
    // The puddles first (the ray passes over them).
    // (None while v.puddle is 0, the rain's first moments and the
    // no-puddle maps: the mask is then neither cleared nor read. pud is
    // read only where the mask is set, and set with it: not cleared.)
    static std::vector<std::uint8_t> puddle, src;
    static std::vector<float> pud;
    const bool has_puddles = v.puddle > 0.0f;
    if (has_puddles) {
        puddle.assign(pixels, 0);
        pud.resize(pixels);
    }
    src.assign(scene, scene + pixels * 3);
    const float th = 1.0f - v.puddle;
    if (has_puddles)
        row_pool().run(h, fine_rows(), [&](int, int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * w + x;
                    if (!surface(k) || sp.kind[k] != kol::kSceneFlat || !level(x, y)) continue;
                    float X, Z;
                    world_xz(sp, x, k, &X, &Z);
                    const float n = 0.65f * noise(X, Z, 56.0f) + 0.35f * noise(X + 311.0f, Z - 97.0f, 21.0f);
                    const float p = std::clamp((n - th) / 0.06f, 0.0f, 1.0f);
                    if (p > 0.0f) {
                        puddle[k] = 1;
                        pud[k] = p;
                    }
                }
        });
    lt.lap("wet.rays");
    // The puddles' reflected rays, a 2x2 block's at a time (block_rays).
    static std::vector<RayHit> rays;
    static RayKeep wet_keep;
    if (has_puddles)
        block_rays(sp, src.data(), puddle.data(), w, h, cam, rays, [&](std::size_t k) { return puddle[k] != 0u; },
                   [](int, int, std::size_t) { return 0; }, &wet_keep);
    lt.lap("wet.pixels");
    row_pool().run(h, fine_rows(), [&](int, int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * w + x;
                if (!surface(k)) continue;
                const std::uint8_t* s = &src[k * 3];
                float c[3] = {static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2])};
                // The light this pixel takes, channel by channel (lit / unlit),
                // worked out only for a glint or running water (three
                // divisions a pixel were most of this pass).
                float lit[3] = {1.0f, 1.0f, 1.0f}, light = -1.0f;
                auto need_light = [&] {
                    if (light >= 0.0f) return;
                    for (int ch = 0; ch < 3; ++ch)
                        lit[ch] = std::clamp(static_cast<float>(s[ch]) / std::max(16.0f, static_cast<float>(unlit[k * 3 + ch])),
                                             0.0f, 2.0f);
                    light = (lit[0] + lit[1] + lit[2]) / 3.0f;
                };
                // Wet: darker, more saturated.
                wet_tone(c, v);
                float X, Z;
                world_xz(sp, x, k, &X, &Z);
                if (sp.kind[k] == kol::kSceneFlat) {
                    if (has_puddles && puddle[k]) {
                        const RayHit& hit = block_ray(rays, w, x, y);
                        if (dbg) {   // magenta: a thing mirrored, cyan: the sky it left at, blue: neither
                            scene[k * 3] = hit.col ? 255 : 0;
                            scene[k * 3 + 1] = hit.sky ? 255 : 0;
                            scene[k * 3 + 2] = 255;
                            continue;
                        }
                        float rr[3];   // (nothing over it: the sky)
                        const float a = v.refl * pud[k] * puddle_mirror(hit, sky, rr);
                        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - a) + rr[ch] * a;
                    }
                    // A wet floor's sheen: a little of the sky over all of it.
                    else if (level(x, y))
                        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - 0.5f * v.dark) + sky[ch] * 0.5f * v.dark;
                    // A slope (a roof; asked 2026-10-26 for dripping water and a
                    // wet look on roof slopes too): more of the
                    // sky's sheen, and water running down it.
                    else {
                        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - 0.8f * v.dark) + sky[ch] * 0.8f * v.dark;
                        float fx, fz;
                        if (v.drips > 0.0f && downhill(x, y, &fx, &fz)) {
                            // Lanes fixed to the world across the fall, each
                            // with water running down it: a bright front and
                            // a trail catching the light, as the walls' drips.
                            const float along = fx * X + fz * Z, across = -fz * X + fx * Z;
                            const int lane = static_cast<int>(std::floor(across / 2.5f));
                            const std::uint32_t hl_ = wet_hash(static_cast<std::uint32_t>(lane) * 2246822519u + 0x5bd1e995u);
                            if ((hl_ & 1023u) < static_cast<std::uint32_t>(std::min(1.0f, 1.6f * v.drips) * 1024.0f)) {
                                const float speed = 26.0f + ((hl_ >> 10) & 31u);   // units a second, down the slope
                                const float period = 40.0f + ((hl_ >> 15) & 63u);
                                const float phase = ((hl_ >> 21) & 1023u) * 0.37f;
                                float s1 = std::fmod(speed * t + phase - along, period);
                                if (s1 < 0.0f) s1 += period;
                                constexpr float kTail = 22.0f;
                                if (s1 < kTail) {
                                    need_light();
                                    float hl[3];
                                    for (int ch = 0; ch < 3; ++ch)
                                        hl[ch] = fb * std::max(0.9f * dsky[ch],
                                                               170.0f * std::min(1.4f, lit[ch]) * std::clamp(light, 0.2f, 1.2f));
                                    drip_streak(c, s1, kTail, 2.5f, v, hl, 1.0f, 1.25f);
                                }
                            }
                        }
                    }
                    // Glints: sparse, twinkling, by the light there.
                    const std::uint32_t g = h2(static_cast<int>(std::floor(X / 2.0f)) * 7 + static_cast<int>(t * 3.0f),
                                               static_cast<int>(std::floor(Z / 2.0f)));
                    if ((g & 1023u) < 18u) {
                        need_light();
                        const float a = v.glint * std::clamp(light - 0.35f, 0.0f, 1.0f) * ((g >> 10) & 255u) / 255.0f;
                        for (int ch = 0; ch < 3; ++ch) c[ch] += fb * a * 150.0f * std::min(1.4f, lit[ch]);
                    }
                } else {
                    // Drips down the walls: a column (X - Z) / 2 fixed to the world.
                    float s1;
                    constexpr float kTail = 34.0f;
                    if (wall_drip(x - sp.kx - sp.ox, static_cast<float>(sp.y[k]), t, v.drips, &s1) && s1 < kTail) {
                        // A wet streak catches the light: the sky's by day,
                        // the lamps' at night.
                        need_light();
                        float hl[3];
                        for (int ch = 0; ch < 3; ++ch)
                            hl[ch] = fb * std::max(0.9f * dsky[ch], 170.0f * std::min(1.4f, lit[ch]) * std::clamp(light, 0.2f, 1.2f));
                        drip_streak(c, s1, kTail, 3.0f, v, hl, 1.0f, 1.25f);
                    }
                }
                for (int ch = 0; ch < 3; ++ch)
                    scene[k * 3 + ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0l, 255l));
            }
    });
    // A lifted wall's texels over a puddle take it too (map 102's ledge-top
    // triangle beside the water stood dry in the puddle: the user reported a BG1
    // triangle left out of the puddle). Lifted, its pixel keeps the
    // wall's own kind and place, so the mask and the rays above are its; the
    // card, filled before this pass, is drawn over the scene.
    if (has_puddles && sp.lift && sp.blocks && sp.atlas_rgb && sp.atlas_per_row > 0) {
        const float field_keep = sp.field_layer >= 2 && ((sp.field_second >> 1) & 1u)
                                     ? std::clamp(static_cast<float>(sp.field_evb) / 16.0f, 0.0f, 1.0f)
                                     : sp.field_layer >= 2 ? 0.0f : 1.0f;
        for (int bi = 0; bi < sp.block_count; ++bi) {
            if (!sp.blocks[bi].surface) continue;
            for_card_texels(sp, w, h, bi, true, [&](int, int, int px, int py, std::size_t k, std::size_t a) {
                if (!puddle[k]) return;
                const RayHit& hit = block_ray(rays, w, px, py);
                float rr[3];
                const float mirror = puddle_mirror(hit, sky, rr);
                // (The card is past its white fade here, and past the
                // lightning's field, which leaves field_evb / 16 of a
                // BG1 pixel showing: at full strength the reflection
                // put dark patches over the flash, map 2 after the
                // rainy battle, states/thor/0013.)
                const float al = v.refl * pud[k] * mirror * (1.0f - std::clamp(sp.scene_white, 0.0f, 1.0f)) * field_keep;
                std::uint8_t* t = sp.atlas_rgb + a * 3;
                for (int ch = 0; ch < 3; ++ch)
                    t[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(t[ch] * (1.0f - al) + rr[ch] * al), 0L, 255L));
            });
        }
    }
}

// The ice (Options > Water; asked 2026-10-20 on JB, map 17, for an effect
// to make the ice look more realistic, the user's pick C of
// shots/ice/JB_ice_mockups.png, all three): a whiter, colder base and broad
// glossy streaks fixed to the world (they slide over it as the camera
// turns), a faint blurred mirror of what stands over it, and sparse glints
// that twinkle, each phased on the world. KOL_ICE=<sheen>,<reflection>,
// <glints> (each 0..1, 1,1,1) for trying values.
void ice_effect(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h, const IceCam* cam = nullptr) {
    const std::uint8_t* ice = sp.ice;
    // A lifted card's ice (ScenePixels::ice_lift): only in the warp, where
    // the card is drawn over the scene (flat, the scene keeps its art).
    const std::uint8_t* ice_lift = cam && sp.lift && sp.atlas_rgb ? sp.ice_lift : nullptr;
    if (!ice && !ice_lift) return;
    static float e_sheen = 1.0f, e_refl = 1.0f, e_glint = 1.0f;
    static const bool parsed = [] {
        if (const char* v = std::getenv("KOL_ICE")) std::sscanf(v, "%f,%f,%f", &e_sheen, &e_refl, &e_glint);
        return true;
    }();
    (void)parsed;
    // A polished stone floor (map_settings.cfg `polished_floor`, its cells
    // kPolishedCells): the ice's mirror alone, twice as strong, no sheen,
    // glints or sky (the user, map 91, asked for the floor outside the blue and
    // red carpet to reflect; pick 6 of
    // rooms/r91/light/m91_mirror.png, 2026-11-03).
    const bool polished = kol::map_flag(kol::MapFlag::PolishedFloor, sp.map);
    const float k_sheen = polished ? 0.0f : e_sheen, k_refl = polished ? 2.0f : e_refl, k_glint = polished ? 0.0f : e_glint;
    static const auto sine = [] {
        std::array<float, 256> tb{};
        for (int i = 0; i < 256; ++i) tb[i] = std::sin(6.2831853f * i / 256.0f);
        return tb;
    }();
    const float t = scene_secs(sp);
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    static std::vector<std::uint8_t> src;
    src.assign(scene, scene + pixels * 3);
    // The sky in the ice: the art's blue streaks give way to a dark
    // reflection of the backdrop's fade where the ray leaves into the sky
    // (the user suggested replacing the blue streak texture with a
    // darkish reflection of the background fade), with `detail` of the
    // art's own light and dark kept; the user's pick from
    // shots/ice/sky_*_mockups.png, C but lighter: the sky at 0.85
    // (C 0.7), 0.35 of the art; then a bluer tint and lighter, the
    // darker version judged a mistake: the sky
    // at 1.1, tinted blue (kSkyTint). KOL_ICE_SKY=<amount>,<brightness>,
    // <detail> (1,1.1,0.35; 0 none) for trying values.
    static float e_sky = 1.0f, k_dark = 1.1f, k_detail = 0.35f;
    static const bool sky_parsed = [] {
        if (const char* v = std::getenv("KOL_ICE_SKY")) std::sscanf(v, "%f,%f,%f", &e_sky, &k_dark, &k_detail);
        return true;
    }();
    (void)sky_parsed;
    const float k_sky = polished ? 0.0f : e_sky;   // (indoors no sky in the floor)
    // The backdrop's colour on each row (its fade is by row), from its own
    // pixels in the scene; a row with none takes the nearest that has some.
    // The sky is the backdrop by the scene's own layer code (5), not any
    // pixel the terrain pass left unplaced: map 17's dirt faces are those
    // too, and the ice took their brown in bands (the user asked where the
    // colour streaks in the reflection came from). (sky_px)
    static std::vector<float> sky_row;
    if (k_sky > 0.0f) {
        sky_row.assign(static_cast<std::size_t>(h) * 3, -1.0f);
        for (int y = 0; y < h; ++y) {
            double acc[3] = {0, 0, 0};
            int n = 0;
            for (int x = 0; x < w; x += 2) {
                const std::size_t q = static_cast<std::size_t>(y) * w + x;
                if (!sky_px(sp, q) || sp.iface[q]) continue;
                for (int ch = 0; ch < 3; ++ch) acc[ch] += src[q * 3 + ch];
                ++n;
            }
            if (n)
                for (int ch = 0; ch < 3; ++ch) sky_row[static_cast<std::size_t>(y) * 3 + ch] = static_cast<float>(acc[ch] / n);
        }
        int last = -1;
        for (int y = 0; y < h; ++y) {
            if (sky_row[static_cast<std::size_t>(y) * 3] >= 0.0f) { last = y; continue; }
            if (last >= 0) std::copy_n(&sky_row[static_cast<std::size_t>(last) * 3], 3, &sky_row[static_cast<std::size_t>(y) * 3]);
        }
        last = -1;
        for (int y = h - 1; y >= 0; --y) {
            if (sky_row[static_cast<std::size_t>(y) * 3] >= 0.0f) { last = y; continue; }
            if (last >= 0) std::copy_n(&sky_row[static_cast<std::size_t>(last) * 3], 3, &sky_row[static_cast<std::size_t>(y) * 3]);
            else for (int ch = 0; ch < 3; ++ch) sky_row[static_cast<std::size_t>(y) * 3 + ch] = 40.0f;
        }
    }
    // The reflected rays, a 2x2 block's at a time (block_rays).
    static std::vector<RayHit> rays;
    static RayKeep ice_keep;
    if (k_refl > 0.0f || k_sky > 0.0f)
        block_rays(sp, src.data(), ice, w, h, cam, rays,
                   [&](std::size_t k) { return (ice && ice[k] != 0u) || (ice_lift && ice_lift[k] != 0u); },
                   [](int, int, std::size_t) { return 0; }, &ice_keep);
    // Its own colours (the cold tint, the sheen, the glints) fade with the
    // scene (ScenePixels::scene_bright), as the water's foam: added toward
    // white, they stayed on the black screen through a scene's fade out
    // (the user, 2026-10-27, map 85 after the dialogue, BLDY 1 -> 16), and
    // too bright through a fade in (JF's opening).
    // KOL_ICE_NO_FADE=1: unfaded (old).
    static const bool no_fade = std::getenv("KOL_ICE_NO_FADE") != nullptr;
    const float fb = no_fade ? 1.0f : std::clamp(sp.scene_bright, 0.0f, 1.0f);
    const float white = 255.0f * fb;
    // The ice's colour at pixel (x, y) of the art s0 into d.
    auto ice_px = [&](int x, int y, std::size_t k, const std::uint8_t* s0, std::uint8_t* d) {
        if (range_plain(sp, k)) return;   // (d keeps the art)
        float X, Z;
        world_xz(sp, x, k, &X, &Z);
        // The mirror: the reflected ray (reflect_ray), its things at 0.35
        // fading over 60 px below their foot (back to 0.35 on request),
        // the sky it leaves into (or, off the top, out of reach or met
        // by a thing, the fade's row it left at) under them.
        float obj_a = 0.0f;
        const std::uint8_t* obj_col = nullptr;
        float sky_c[3] = {-1.0f, -1.0f, -1.0f};
        if (k_refl > 0.0f || k_sky > 0.0f) {
            const RayHit& hit = block_ray(rays, w, x, y);
            if (hit.col) {
                obj_col = hit.col;
                obj_a = k_refl * 0.35f * std::max(0.0f, 1.0f - hit.below / 60.0f);
            }
            if (hit.sky)
                for (int ch = 0; ch < 3; ++ch) sky_c[ch] = hit.sky[ch];
            else if (k_sky > 0.0f)
                std::copy_n(&sky_row[static_cast<std::size_t>(std::max(0, hit.last_ry)) * 3], 3, sky_c);
        }
        float c[3] = {static_cast<float>(s0[0]), static_cast<float>(s0[1]), static_cast<float>(s0[2])};
        // The sky in the ice: the dark fade, with a little of the art.
        if (k_sky > 0.0f && sky_c[0] >= 0.0f) {
            const float lum = 0.3f * c[0] + 0.55f * c[1] + 0.15f * c[2];
            const float det = k_detail * (lum - 150.0f) * 0.6f;
            // A vivid sky calmed first (calm_sky).
            float sk[3] = {sky_c[0], sky_c[1], sky_c[2]};
            calm_sky(sk);
            static constexpr float kSkyTint[3] = {0.88f, 1.0f, 1.18f};
            for (int ch = 0; ch < 3; ++ch) c[ch] += (sk[ch] * k_dark * kSkyTint[ch] + det - c[ch]) * k_sky;
        }
        // Colder and a little clearer: toward a pale blue-white.
        static constexpr float kCold[3] = {200.0f, 228.0f, 245.0f};
        for (int ch = 0; ch < 3; ++ch) c[ch] += (kCold[ch] * fb - c[ch]) * 0.12f * k_sheen * (1.0f - k_sky);
        // The sheen: broad diagonal streaks on the world, sharpened.
        const float ph = (0.7f * X + Z) / 70.0f;
        const float sw = 0.5f + 0.5f * sine[static_cast<unsigned>(static_cast<int>(std::floor(ph * 256.0f))) & 255u];
        const float sw2 = sw * sw;
        const float sheen = k_sheen * 0.35f * sw2 * sw2 * sw2;
        for (int ch = 0; ch < 3; ++ch) c[ch] += (white - c[ch]) * sheen;
        if (obj_col) {
            static constexpr float kTint[3] = {0.85f, 0.95f, 1.05f};
            for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1.0f - obj_a) + obj_col[ch] * kTint[ch] * obj_a;
        }
        // The glints: one world point in ~180 on a 2-unit grid, each its
        // own slow twinkle.
        if (k_glint > 0.0f) {
            const int gx = static_cast<int>(std::floor(X / 2.0f)), gz = static_cast<int>(std::floor(Z / 2.0f));
            const std::uint32_t hsh = (static_cast<std::uint32_t>(gx) * 73856093u) ^ (static_cast<std::uint32_t>(gz) * 19349663u);
            if ((hsh % 180u) == 0u) {
                const float tw = 0.5f + 0.5f * std::sin(t * 2.3f + static_cast<float>(hsh >> 8) * 0.001f);
                const float tw2 = tw * tw;
                const float g = k_glint * tw2 * tw2;
                for (int ch = 0; ch < 3; ++ch) c[ch] += (white - c[ch]) * g;
            }
        }
        for (int ch = 0; ch < 3; ++ch)
            d[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0l, 255l));
    };
    // The scene's ice in rows on the row pool, as the water (each pixel
    // reads `src`, the rays and the sky's rows, and writes its own; on one
    // thread, column by column, it walked the frame across its rows).
    if (ice)
        row_pool().run(h, fine_rows(), [&](int, int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * w + x;
                    if (ice[k]) ice_px(x, y, k, &src[k * 3], &scene[k * 3]);
                }
        });
    // A lifted square of ice (a wall's, lifted with Lifted Walls): its
    // card's own texels, lit already in the card pass, as the scene's ice.
    if (ice_lift)
        for (int bi = 0; bi < sp.block_count; ++bi) {
            if (!sp.blocks[bi].surface) continue;
            for_card_texels(sp, w, h, bi, true, [&](int, int, int px, int py, std::size_t k, std::size_t a) {
                if (!ice_lift[k]) return;
                std::uint8_t* t = sp.atlas_rgb + a * 3;
                const std::uint8_t art[3] = {t[0], t[1], t[2]};
                ice_px(px, py, k, art, t);
            });
        }
    // The flakes' own reflections: each flake's pixels mirrored about the row
    // of the ground it falls to, as spots on the ice (only where they land
    // on ice), pale and fading as the flake is higher.
    // KOL_ICE_NO_FLAKES=1: none.
    static const bool no_flakes = std::getenv("KOL_ICE_NO_FLAKES") != nullptr;
    if (sp.flakes && sp.weather && k_refl > 0.0f && !no_flakes)
        for (int f = 0; f < sp.flake_count; ++f) {
            const kol::ScenePixels::Flake& fl = sp.flakes[f];
            for (int ty = 0; ty < 8; ++ty)
                for (int tx = 0; tx < 8; ++tx) {
                    const int px = fl.x + tx, py = fl.y + ty;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    const std::size_t q = static_cast<std::size_t>(py) * w + px;
                    if (!sp.weather[q]) continue;
                    const int my = 2 * fl.ground - py;   // mirrored about the ground
                    if (my < 0 || my >= h) continue;
                    const std::size_t m = static_cast<std::size_t>(my) * w + px;
                    if (!ice || !ice[m]) continue;
                    const float above = static_cast<float>(fl.ground - py);
                    const float a = k_refl * 0.6f * std::max(0.0f, 1.0f - above / 60.0f);
                    for (int ch = 0; ch < 3; ++ch)
                        scene[m * 3 + ch] = static_cast<std::uint8_t>(std::clamp(
                            std::lround(scene[m * 3 + ch] * (1.0f - a) + src[q * 3 + ch] * a), 0l, 255l));
                }
        }
}

// Smooth pans (the user, 2026-10-28, found the panning not as smooth
// as it could be). A scene's camera steps in whole pixels, and a
// diagonal pan in a staircase: map 3's pan down to the shore (states/thor/
// 0013) goes, every 4 frames, nowhere, 1 down, 1 sideways, 1 down (never
// both at once), so the picture stopped 15 times a second and zig-zagged.
// The scene (the mesh, units' cards with it; not the HUD) is drawn where
// the average of the picture's last kPanSmooth BG0 scrolls puts it: a
// staircase repeating every 4 frames becomes a straight line at its speed,
// a pan eases in and out over 4 pictures, at most ~1.5 px behind the game.
// Story scenes only (on battle maps the cursor's hand is drawn flat and
// would leave its cell); a step over kPanCut, a new map or none of that
// starts it again (a cut stays a cut). KOL_NO_PAN_SMOOTH=1: as before.
constexpr int kPanSmooth = 4, kPanCut = 6, kShakeHold = 12;
void pan_smooth(const kol::ScenePixels& sp, float* ox, float* oy) {
    *ox = *oy = 0.0f;
    static const bool off = std::getenv("KOL_NO_PAN_SMOOTH") != nullptr;
    static int n = 0, at = 0, map = -1;
    static unsigned loads = 0, last_h = 0, last_v = 0;
    static long ux = 0, uy = 0, hx[kPanSmooth] = {}, hy[kPanSmooth] = {};
    static int sgn_x = 0, sgn_y = 0, rev_x = kShakeHold, rev_y = kShakeHold;
    if (off || !sp.story || !sp.bg0_ok) {
        n = 0;
        return;
    }
    if (n > 0) {   // the scroll wraps at 512
        const int dx = ((static_cast<int>(sp.bg0_h) - static_cast<int>(last_h) + 256) & 511) - 256;
        const int dy = ((static_cast<int>(sp.bg0_v) - static_cast<int>(last_v) + 256) & 511) - 256;
        if (map != sp.map || loads != sp.map_loads || std::abs(dx) > kPanCut || std::abs(dy) > kPanCut) {
            n = 0;
        } else {
            ux += dx;
            uy += dy;
            // A shake, not a pan: the scroll turned back on itself.
            if (dx) {
                if (sgn_x && (dx > 0) != (sgn_x > 0)) rev_x = 0;
                sgn_x = dx > 0 ? 1 : -1;
            }
            if (dy) {
                if (sgn_y && (dy > 0) != (sgn_y > 0)) rev_y = 0;
                sgn_y = dy > 0 ? 1 : -1;
            }
        }
    }
    if (n == 0) {
        ux = sp.bg0_h;
        uy = sp.bg0_v;
        at = 0;
        sgn_x = sgn_y = 0;
        rev_x = rev_y = kShakeHold;
    }
    rev_x = std::min(rev_x + 1, kShakeHold);
    rev_y = std::min(rev_y + 1, kShakeHold);
    last_h = sp.bg0_h;
    last_v = sp.bg0_v;
    map = sp.map;
    loads = sp.map_loads;
    hx[at] = ux;
    hy[at] = uy;
    at = (at + 1) % kPanSmooth;
    n = std::min(n + 1, kPanSmooth);
    long sx = 0, sy = 0;
    for (int i = 0; i < n; ++i) {
        sx += hx[(at + kPanSmooth - 1 - i) % kPanSmooth];
        sy += hy[(at + kPanSmooth - 1 - i) % kPanSmooth];
    }
    // Drawn as if scrolled to the average: the scene moves by what the
    // scroll is past it.
    *ox = static_cast<float>(ux) - static_cast<float>(sx) / n;
    *oy = static_cast<float>(uy) - static_cast<float>(sy) / n;
    // The game shakes the ground by stepping the scroll back and forth (map
    // 86, Thor state 0216: X +-1 every 2 frames, exactly this window's
    // length, so the average held the ground still and the units, placed
    // by the real camera, shook instead). Either axis turned back within
    // the last kShakeHold pictures: the scene is drawn where the game put it.
    // KOL_PAN_SMOOTH_SHAKES=1: as before.
    static const bool smooth_shakes = std::getenv("KOL_PAN_SMOOTH_SHAKES") != nullptr;
    if (!smooth_shakes) {
        if (rev_x < kShakeHold || rev_y < kShakeHold) *ox = *oy = 0.0f;   // its jumps on the other axis too
    }
    static const bool log = std::getenv("KOL_CAM_LOG") != nullptr;   // the camera log's (kol_widescreen)
    if (log)
        std::fprintf(stderr, "pan: serial %lu scroll %ld,%ld shown %.2f,%.2f (moved %.2f,%.2f)\n",
                     static_cast<unsigned long>(sp.serial), ux, uy, ux - *ox, uy - *oy, *ox, *oy);
}

// The pixels drawn flat over the mesh (PresentWarpMesh::flat), the view's
// (x0, y0, vw x vh, in the scene w wide): the HUD (BG2|BG3, and sprites over
// them) and interface sprites, but not the cursor's floating hand
// (`floating`: it is moved with the map, the warp's step 3b; null: none).
void flat_mask(const kol::ScenePixels& sp, const std::uint8_t* layers, const std::uint8_t* floating, int w, int x0,
               int y0, int vw, int vh) {
    g_flat.resize(static_cast<std::size_t>(vw) * vh);
    for (int py = 0; py < vh; ++py) {
        const std::size_t o = static_cast<std::size_t>(y0 + py) * w + x0;
        for (int px = 0; px < vw; ++px) {
            const std::size_t k = o + px;
            g_flat[static_cast<std::size_t>(py) * vw + px] =
                hud_pixel(sp, layers[k]) || (sp.iface[k] && !(floating && floating[k]) && !faded_iface(sp, layers, floating, k));
        }
    }
}
// The mesh's pictures beside its triangles: the flat pixels (g_flat), the
// backdrop, the moved pixels (with `moved`) and the atlas (`atlas`'s, null:
// none).
void mesh_pictures(gbarecomp::PresentWarpMesh* mesh, bool moved, const kol::ScenePixels* atlas) {
    mesh->flat = g_flat.data();
    mesh->backdrop = g_backdrop.data();
    mesh->moved = moved ? g_moved.data() : nullptr;
    mesh->moved_rgb = moved ? g_moved_rgb.data() : nullptr;
    mesh->atlas_h = atlas ? atlas->atlas_h : 0;
    mesh->atlas_rgb = atlas ? atlas->atlas_rgb : nullptr;
    mesh->atlas_blur = atlas ? atlas->atlas_blur : nullptr;
}
// Our rain's view of the picture (kol_rain): the scene row and column of the
// place (0, 0) and the view in the scene. The warp adds its camera; the
// flat view is scale 1.
kol::RainCam rain_cam(const kol::ScenePixels& sp) {
    kol::RainCam rc;
    rc.base = static_cast<float>(sp.oy + sp.ky);
    rc.col0 = static_cast<float>(sp.ox + sp.kx);
    rc.vx = sp.vx; rc.vy = sp.vy; rc.vw = sp.vw; rc.vh = sp.vh;
    return rc;
}

// The light without the perspective (perspective_set_flat): the scene as
// the game drew it, lit, drawn back onto itself pixel for pixel, the HUD on
// top as the warp has it. Units keep their own pixels; with Unit Shadows
// the game's blob under them gives way to the ground the game drew there
// (as the warp draws units from their squares without it), and a unit in
// the terrain's shadow is shaded as its square would be.
int flat_provider(const std::uint8_t* layers, std::uint8_t* scene, int w, int h,
                  gbarecomp::PresentWarpMesh* mesh) {
    kol::PresentScope present;
    kol::RainFrame rain_frame;
    kol::SnowFrame snow_frame;
    if (w < 240 || h < 160) return 0;
    layers = kol::present_layers(layers, w, h);
    kol::ScenePixels sp;
    kol::hud_bake_into(scene, w, h);
    if (!kol::scene_pixels(layers, w, h, &sp, scene)) return 0;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    palette_lag(sp, layers, scene, pixels);
    model_recolour(sp, scene, pixels);
    static std::vector<std::uint8_t> unwhite;   // the scene before its light (under_white)
    if (sp.scene_white > 0.0f) unwhite.assign(scene, scene + pixels * 3);
    if (sp.sky != kol::kSkyGame) recolour_backdrop(sp, layers, sp.iface, scene, pixels, sp.sky, sp.sky_desat, sp.sky_mix);
    smooth_sky(sp, layers, sp.iface, scene, w, h);
    static std::vector<std::uint8_t> ground;
    const std::uint8_t* ground_extra = nullptr;
    if (sp.lift && sp.atlas_opaque) {
        ground.assign(pixels, 0);
        const float shade_soft = kol::card_shade_soft();   // (once, not a texel at a time)
        for (int bi = 0; bi < sp.block_count; ++bi) {
            const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
            if (b.prop || b.surface) continue;
            for_card_texels(sp, w, h, bi, true, [&](int, int, int, int py, std::size_t k, std::size_t a) {
                if (!sp.atlas_opaque[a]) {
                    if (sp.under_rgb) {
                        std::copy_n(sp.under_rgb + k * 3, 3, scene + k * 3);
                        ground[k] = 1;
                        ground_extra = ground.data();
                    }
                    return;
                }
                const float f = kol::ScenePixels::card_shade(static_cast<float>(py), b.shade_row, b.shade_k,
                                                             shade_soft);
                if (f < 1.0f)
                    for (int ch = 0; ch < 3; ++ch)
                        scene[k * 3 + ch] = static_cast<std::uint8_t>(std::lround(scene[k * 3 + ch] * f));
            });
        }
    }
    // A door painted on its wall (ScenePixels::wall_px) is lit as the map.
    if (sp.wall_px) {
        if (!(sp.lift && sp.atlas_opaque)) ground.assign(pixels, 0);
        for (std::size_t k = 0; k < pixels; ++k)
            if (sp.wall_px[k]) ground[k] = 1;
        ground_extra = ground.data();
    }
    sea_foam_hide(sp, scene, pixels);
    const std::uint8_t* unlit = foam_unlit(sp, scene, pixels);
    apply_lighting(sp, layers, ground_extra, scene, pixels);
    sea_foam(sp, scene, pixels);
    magic_circle(sp, scene, w, h);
    water_effect(sp, scene, w, h, nullptr, unlit);
    falls_effect(sp, scene, w, h, true);
    ice_effect(sp, scene, w, h);
    wet_effect(sp, scene, w, h, nullptr, unlit);
    // The units' own pixels (not the ground under their blob), in one pass:
    // a map's lamp on them (they keep their colours, but not in its light);
    // the light through the windows; and the moon's (windows with a light
    // of their own), hue kept.
    const bool unit_glow = sp.unit_glow != nullptr;
    const bool unit_win = sp.unit_win_patch && sp.unit_win_haze;
    const bool unit_moon = sp.moon_unit_patch && sp.moon_unit_haze;
    if (unit_glow || unit_win || unit_moon)
        for (std::size_t k = 0; k < pixels; ++k) {
            if (ground_extra && ground_extra[k]) continue;
            if (unit_glow && sp.unit_glow[k]) torch_tint(&scene[k * 3], sp, sp.unit_glow[k], sp.unit_glow_lamp[k]);
            if (unit_win && (sp.unit_win_patch[k] || sp.unit_win_haze[k]))
                window_light(&scene[k * 3], sp.unit_win_patch[k], sp.unit_win_haze[k], sp.win_strength, sp.scene_bright, sp.win_tint,
                             true, unit_cool_keep);
            if (unit_moon && (sp.moon_unit_patch[k] || sp.moon_unit_haze[k]))
                window_light(&scene[k * 3], sp.moon_unit_patch[k], sp.moon_unit_haze[k], sp.moon_strength, sp.scene_bright,
                             sp.moon_tint, true, true);
        }
    // The map's cut edge faded: the map's pixels and its units, not the HUD
    // or the interface.
    if (sp.edge_fade) {
        static std::vector<std::uint8_t> sky_rows;
        edge_sky_rows(layers, sp.iface, scene, w, h, sky_rows);
        for (std::size_t k = 0; k < pixels; ++k) {
            const unsigned f = sp.edge_fade[k];
            if (f >= 255u || (sp.iface[k] && !faded_iface(sp, layers, nullptr, k))) continue;
            const unsigned top = layers[k] & 0x0Fu, under = layers[k] >> 4;
            // (Not hud_pixel: without its field_layer exception a flash's
            // field counts as HUD here and is left unfaded, as it always was
            // on the flat view.)
            const bool hud = top == 2u || top == 3u || (top == kol::kLayerObj && (under == 2u || under == 3u));
            if (hud || top == kol::kLayerBackdrop) continue;
            edge_fade_px(scene + k * 3, &sky_rows[(k / static_cast<std::size_t>(w)) * 3], f);
        }
    }
    // Units keep their colours (as the warp draws them from their squares),
    // but not the ground under their blob, which is ground.
    static std::vector<std::uint8_t> unit_px;
    const std::uint8_t* keep = nullptr;
    if (sp.lift) {
        unit_px.assign(sp.lift, sp.lift + pixels);
        for (std::size_t k = 0; k < pixels; ++k) {
            const int b = static_cast<int>(unit_px[k]) - 1;
            if (b < 0) continue;
            if (b >= sp.block_count || sp.blocks[b].prop || sp.blocks[b].surface ||
                (ground_extra && ground_extra[k]))
                unit_px[k] = 0;
        }
        keep = unit_px.data();
    }
    // The ground under a blob is painted as the ground it is (its place
    // under the unit), as the warp has it.
    const std::uint8_t* paint_kind = sp.kind;
    const std::int32_t* paint_xz = sp.xz;
    static std::vector<std::uint8_t> kind_copy;
    static std::vector<std::int32_t> xz_copy;
    if (ground_extra && sp.under_kind && sp.under_xz && mesh_paint_on()) {
        kind_copy.assign(sp.kind, sp.kind + pixels);
        xz_copy.assign(sp.xz, sp.xz + pixels);
        for (std::size_t k = 0; k < pixels; ++k)
            if (ground_extra[k]) {
                kind_copy[k] = sp.under_kind[k];
                xz_copy[k] = sp.under_xz[k];
            }
        paint_kind = kind_copy.data();
        paint_xz = xz_copy.data();
    }
    paint_mesh_kind(paint_kind, paint_xz, sp.iface, nullptr, scene, pixels, keep);
    // The HUD (BG2|BG3, and sprites over them) and interface, from the frame.
    flat_mask(sp, layers, nullptr, w, 0, 0, w, h);
    // Debug > Tile Info's readout, as the warp writes it.
    const bool moved = sp.tile_text != nullptr;
    if (moved) {
        g_moved.assign(pixels, 0);
        g_moved_rgb.resize(pixels * 3);
        draw_tile_text(sp.tile_text, w, h);
    }
    g_backdrop.assign(scene, scene + pixels * 3);
    // The scene onto itself, a quad a pixel (as the warp's mesh is): one quad
    // over the whole picture drifted on SDL's software renderer (its texture
    // coordinates stepped with too little precision over a long triangle,
    // the snowfield's texture a pixel off toward the far corner), and so did
    // squares of 8. Built once for the picture's size. KOL_FLAT_GRID=<px>
    // sets the squares' size.
    static const int cell = [] {
        const char* v = std::getenv("KOL_FLAT_GRID");
        return v && *v ? std::max(1, std::atoi(v)) : 1;
    }();
    static std::vector<float> flat_xy;
    static std::vector<int> flat_idx;
    static int flat_w = 0, flat_h = 0;
    const int nx = (w + cell - 1) / cell, ny = (h + cell - 1) / cell;
    if (flat_w != w || flat_h != h) {
        flat_w = w;
        flat_h = h;
        flat_xy.resize(static_cast<std::size_t>(nx + 1) * (ny + 1) * 2);
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i <= nx; ++i) {
                const std::size_t v = (static_cast<std::size_t>(j) * (nx + 1) + i) * 2;
                flat_xy[v] = static_cast<float>(std::min(w, i * cell));
                flat_xy[v + 1] = static_cast<float>(std::min(h, j * cell));
            }
        flat_idx.clear();
        flat_idx.reserve(static_cast<std::size_t>(nx) * ny * 6);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
                flat_idx.insert(flat_idx.end(), {a, b, c, b, d, c});
            }
    }
    under_white_all(sp, scene, unwhite, pixels, sp.scene_white);
    field_all(sp, scene, pixels, false);
    field_all(sp, g_backdrop.data(), g_backdrop.size() / 3, true);
    mesh->alpha = nullptr;
    mesh->xy = flat_xy.data();
    mesh->uv = flat_xy.data();   // the same places: pixel for pixel
    float pan_x = 0.0f, pan_y = 0.0f;
    pan_smooth(sp, &pan_x, &pan_y);
    if (pan_x != 0.0f || pan_y != 0.0f) {   // the edge's squares stretch over what the move leaves
        static std::vector<float> pan_xy;
        pan_xy.resize(flat_xy.size());
        for (std::size_t v = 0; v < flat_xy.size(); v += 2) {
            pan_xy[v] = std::clamp(flat_xy[v] + pan_x, 0.0f, static_cast<float>(w));
            pan_xy[v + 1] = std::clamp(flat_xy[v + 1] + pan_y, 0.0f, static_cast<float>(h));
        }
        mesh->xy = pan_xy.data();
    }
    mesh->vertices = (nx + 1) * (ny + 1);
    mesh->indices = flat_idx.data();
    mesh->index_count = static_cast<int>(flat_idx.size());
    mesh->card = nullptr;
    mesh_pictures(mesh, moved, nullptr);
    kol::RainCam rc = rain_cam(sp);   // the picture as it is: scale 1
    rc.flat = true;
    kol::rain_build(sp, w, h, rc, mesh);
    kol::snow_build(sp, w, h, rc, mesh);
    hud_glow_over(sp, mesh, w, h, rc);
    return 1;
}

// An ice crystal's extent and its three brightest texels (warp_cards,
// SpriteBlock::crystal), the scene pixels of the glints.
struct CrystalInfo { int cell, x0, y0, x1, y1; std::array<std::size_t, 3> glints; };

// One picture of the warp (warp_provider): what its phases share. Each
// phase takes the names it reads from here and leaves what it works out.
struct WarpFrame {
    const kol::ScenePixels* sp = nullptr;
    const std::uint8_t* layers = nullptr;
    std::uint8_t* scene = nullptr;
    int w = 0, h = 0;
    std::size_t pixels = 0;
    const std::uint8_t* iface = nullptr;   // sp.iface
    const std::uint8_t* lift = nullptr;    // sp.lift
    // The view in the scene (see warp_provider).
    int view_x = 0, view_y = 0, view_w = 0, view_h = 0;
    std::size_t view_pixels = 0;
    // Each scene pixel's place, the map's outline spread (warp_places: g_xz,
    // g_y, g_kind).
    const std::int32_t* xz = nullptr;
    const std::int32_t* yy = nullptr;
    const std::uint8_t* kind = nullptr;
    // The camera (warp_camera), and whether the trim check runs this picture.
    WarpCam cam;
    bool check = false;
    // The backdrop's colour per row, for the map's cut edge (warp_cards).
    const std::uint8_t* edge_rows = nullptr;
    // The cursor's floating hand (warp_hand): sp.floating, and its box.
    const std::uint8_t* floating = nullptr;
    int fb_x0 = 0, fb_x1 = -1, fb_y0 = 0, fb_y1 = -1;
    bool moved = false;   // g_moved holds the moved hand or Tile Info's readout (warp_flat)
    // The mesh (warp_corners on): a vertex per pixel corner (vw x vh, verts),
    // then each atlas square's (`slots` of them; all_verts in all), then the
    // strips' own corners (mesh_verts in all, warp_zip).
    int vw = 0, vh = 0, slots = 0;
    std::size_t verts = 0, all_verts = 0, mesh_verts = 0;
    const std::uint8_t* prop_lift = nullptr;   // g_prop_lift, or null (see front() in warp_corners)
    int dmin = 0, dmax = 0;                    // the scene runs' depths (warp_runs)
    bool overlay_lifted = false;               // KOL_PERSP_OVERLAY_LIFTED (moved_overlay)

    // A lifted prop's pixel under interface the combat camera moved with a
    // unit (an overlay card draws it, kol_focus ScenePixels::ovl_card): the
    // prop's card keeps the prop's own colour there, as under a menu. Taken
    // as the frame's, the 3 of a damage number over a lifted stone stayed
    // in the stone's card and came apart from its 0
    // (states/HZ_slot2_damage_30_broken.state, presents 124-166).
    // KOL_PERSP_OVERLAY_LIFTED=1: as before.
    bool moved_overlay(std::size_t k) const {
        return !overlay_lifted && lift && lift[k] && sp->ovl_card && sp->ovl_card[k];
    }
    // A lifted prop's pixel under interface: the no-sprite buffer there holds
    // the prop's own colour (it goes to the card), not what lies behind, so
    // its hole takes what lies behind the prop's nearest pixel to the right
    // on its row (the prop's own hole there). From the left, the menu's
    // side, it took the snow under the menu where the wall behind the rock
    // shows (FG); with none to the right, step 2 fills it from the left.
    // And interface the combat camera moved with a unit (an overlay, drawn
    // in front of everything at the unit's place): the same, so the prop's
    // card keeps the prop's colour under it and the overlay keeps its own.
    // Taken by the lifted stone it lay over, the 3 of a damage number came
    // apart under the combat camera: the stone's card took the digit's
    // colours, the overlay the stone's (states/HZ_slot2_damage_30_broken,
    // presents 124-166). KOL_PERSP_OVERLAY_LIFTED=1: as before.
    bool prop_under_menu(std::size_t k) const {
        return lift && lift[k] && lift[k] <= sp->block_count && sp->blocks[lift[k] - 1].prop &&
               (iface[k] || moved_overlay(k)) && obj_over(*sp, k);
    }
};

// Two scene pixels on one surface (the mesh's cuts and the strips' own
// corners: warp_corners, warp_zip): both placed, both overlays or neither,
// within 8 of depth and 4 of height.
struct SameSurface {
    const std::uint8_t* kind;
    const std::int32_t* xz;
    const std::int32_t* yy;
    bool operator()(std::size_t k, std::size_t n) const {
        return kind[k] != kol::kSceneNone && kind[n] != kol::kSceneNone &&
               (kind[k] == kol::kSceneOverlay) == (kind[n] == kol::kSceneOverlay) &&
               std::abs(xz[k] - xz[n]) <= 8 && std::abs(yy[k] - yy[n]) <= 4;
    }
};

// KOL_PERSP_HASH's picture count (warp_corners, warp_keep, warp_output).
long g_trace_frame = 0;

// The places the mesh is built from (warp_provider's first phase): each
// scene pixel's (g_xz, g_y, g_kind), and the map's outline spread onto the
// art just past the cells.
void warp_places(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const layers = wf.layers;
    const int w = wf.w, h = wf.h, view_w = wf.view_w;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const lift = wf.lift;
    // The map's art reaches a little past the cells the rebuild draws (the
    // dark outline along the map's edge): map-layer pixels (BG0|BG1) within
    // kReach of a placed pixel take its place, so they move with the map
    // instead of staying behind in the sky. Anything farther is left as
    // backdrop -- a painted background stays where it is.
    constexpr std::uint8_t kReach = 3;
    g_xz.assign(sp.xz, sp.xz + pixels);
    g_y.assign(sp.y, sp.y + pixels);
    g_kind.assign(sp.kind, sp.kind + pixels);
    // Units drawn from their own sprites: their pixels in the frame show the
    // scene behind them (kol_focus_atlas.cpp, "Units from their own sprites").
    // Side columns the PPU was told to leave out (perspective_overscan_rows)
    // are not there: nothing is placed from them.
    // A lifted unit pixel shows the ground under the unit: its layer is the
    // scene's second there. (Read as the sprite, the map's outline under
    // Alphonse's hair stayed behind as sky, and the backdrop carried it
    // along the row: CG_alphonse_stretch.)
    // g_cand: the pixels the outline below may claim, worked out in the same
    // pass (the spread used to ask four neighbours of every scene pixel); a
    // claimed pixel is placed, so it stops being one.
    // Which lifted pixels are a prop's (see front() in warp_corners).
    g_prop_lift.clear();
    if (lift && sp.blocks) {
        bool any = false;
        for (int bi = 0; bi < sp.block_count && !any; ++bi) any = sp.blocks[bi].prop || sp.blocks[bi].surface;
        if (any) {
            g_prop_lift.assign(pixels, 0);
            for (std::size_t k = 0; k < pixels; ++k)
                // (Not behind a model's rope: that is the hull or sail itself, an
                // ordinary pixel of it. As a prop's ground its corners gave way to
                // the sea beside it, and it stretched out over the sea in a streak.)
                if (lift[k] && lift[k] <= sp.block_count &&
                    (sp.blocks[lift[k] - 1].prop || sp.blocks[lift[k] - 1].surface) && !(sp.rope_air && sp.rope_air[k]))
                    g_prop_lift[k] = 1;
        }
    }
    const bool trimmed = w > view_w;
    g_cand.resize(pixels);
    for (std::size_t k = 0; k < pixels; ++k) {
        const bool lifted = lift && lift[k];
        if (lifted) {
            g_xz[k] = sp.under_xz[k];
            g_y[k] = sp.under_y[k];
            g_kind[k] = sp.under_kind[k];
        }
        if (trimmed && layers[k] == gba::kLayerNotComposed) g_kind[k] = kol::kSceneNone;
        const unsigned layer = lifted && sp.scene_codes ? sp.scene_codes[k] >> 4
                                                        : layers[k] & 0x0Fu;
        g_cand[k] = g_kind[k] == kol::kSceneNone && layer <= 1u;
        // (A rope over the sky leaves the sky: no art to place. As art with no
        // place it took its neighbours', and showed as marks below the sail.)
        if (lifted && sp.rope_air && sp.rope_air[k]) g_cand[k] = false;
    }
    g_dist.assign(pixels, 0);
    g_queue.clear();
    const std::uint8_t* const cand = g_cand.data();
    for (int py = 0; py < h; ++py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        for (int px = 0; px < w; ++px) {
            const std::size_t k = o + px;
            if (g_kind[k] == kol::kSceneNone) continue;
            if ((px > 0 && cand[k - 1]) || (px + 1 < w && cand[k + 1]) ||
                (py > 0 && cand[k - w]) || (py + 1 < h && cand[k + w]))
                g_queue.push_back(static_cast<std::uint32_t>(k));
        }
    }
    for (std::size_t head = 0; head < g_queue.size(); ++head) {
        const std::uint32_t k = g_queue[head];
        if (g_dist[k] >= kReach) continue;
        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
        const int nx[4] = {px - 1, px + 1, px, px};
        const int ny[4] = {py, py, py - 1, py + 1};
        for (int m = 0; m < 4; ++m) {
            if (nx[m] < 0 || ny[m] < 0 || nx[m] >= w || ny[m] >= h) continue;
            const std::size_t n = static_cast<std::size_t>(ny[m]) * w + nx[m];
            if (!g_cand[n]) continue;
            g_cand[n] = 0;
            g_kind[n] = g_kind[k];
            g_xz[n] = g_xz[k];
            g_y[n] = g_y[k];
            g_dist[n] = static_cast<std::uint8_t>(g_dist[k] + 1);
            g_queue.push_back(static_cast<std::uint32_t>(n));
        }
    }
    wf.xz = g_xz.data();
    wf.yy = g_y.data();
    wf.kind = g_kind.data();
}

// The reference and the camera (warp_provider): the height the warp scales
// about, eased, the depth that keeps its size, and the combat camera and
// the stick's look (camera()), into wf.cam.
void warp_camera(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const int w = wf.w, h = wf.h;
    const int view_x = wf.view_x, view_y = wf.view_y, view_w = wf.view_w, view_h = wf.view_h;
    const std::int32_t* const yy = wf.yy;
    const std::uint8_t* const kind = wf.kind;
    // The frame row of the place (0, 0): row = base + D / 4 - Y / 2.
    const float base = static_cast<float>(sp.oy + sp.ky);

    // The reference: heights scale about the map's mean height on screen, and
    // the depth under the middle row at that height keeps its size.
    double sum_y = 0.0;
    long n = 0;
    for (int py = view_y; py < view_y + view_h; ++py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        for (int px = view_x; px < view_x + view_w; ++px)
            if (kind[o + px] != kol::kSceneNone) { sum_y += yy[o + px]; ++n; }
    }
    const float target_y = n ? static_cast<float>(sum_y / n) : 0.0f;
    // The check judges the rows as they were chosen, before this ease.
    const bool check = trim_check() && g_have_ref.load(std::memory_order_relaxed) && w > view_w;
    if (check) {
        g_check_rows.resize(static_cast<std::size_t>(h));
        const unsigned ov[4] = {static_cast<unsigned>(view_x),
                                static_cast<unsigned>(w - view_w - view_x),
                                static_cast<unsigned>(view_y),
                                static_cast<unsigned>(h - view_h - view_y)};
        kol::perspective_overscan_rows(view_w, view_h, ov, g_check_rows.data(), h);
    }
    if (!g_have_ref.load(std::memory_order_relaxed)) {
        g_yref.store(target_y, std::memory_order_relaxed);
        g_have_ref.store(true, std::memory_order_relaxed);
    }
    // Held within kYrefHold of its target: rain hides and shows scene pixels
    // (the target moves ~0.02 a frame on map 58's boat), and an ease that
    // never stopped moved every point far from a unit's anchor by a hair; a
    // row on a pixel's edge, the castle door's bottom row 4 over its anchor,
    // came and went with the rain (states/HM_slot2_map58_flag).
    // KOL_YREF_HOLD=0: always eased, as before.
    static const float kYrefHold = kol::env_num("KOL_YREF_HOLD", 0.5f);
    const float yref_now = g_yref.load(std::memory_order_relaxed);
    const float yref = std::abs(target_y - yref_now) < kYrefHold ? yref_now
                                                                 : yref_now + (target_y - yref_now) * 0.05f;
    g_yref.store(yref, std::memory_order_relaxed);
    const float dref = 4.0f * (view_y + view_h * 0.5f - base) + 2.0f * yref;
    const Cam lens = camera(g_cam_cap.strike_t, g_cam_cap.look_x, g_cam_cap.look_y,
                            view_w * 0.5f, view_h * 0.5f);
    WarpCam cam;   // (scale, row_of, row_drawn: WarpCam)
    cam.base = base;
    cam.dref = dref;
    cam.yref = yref;
    cam.K = lens.K;
    cam.zoom = lens.Z;
    cam.pitch = lens.f;
    cam.cx = view_x + view_w * 0.5f;
    cam.cy = view_y + view_h * 0.5f;
    cam.cxp = cam.cx + lens.dx;   // the perspective's centre (the look)
    if (sp.far_px) {
        const Cam rest = camera(g_cam_cap.strike_t, 0.0f, 0.0f, view_w * 0.5f, view_h * 0.5f);
        cam.far = sp.far_px;
        cam.far_share = sp.far_share;
        cam.rest_K = rest.K;
        cam.rest_pitch = rest.f;
        cam.rest_zoom = rest.Z;
        cam.rest_cxp = cam.cx + rest.dx;
    }
    wf.cam = cam;
    wf.check = check;
}

// A square of a fall's vapor (SpriteBlock::effect 3): its cloud at scene (x, y).
void vapor_texel(const kol::ScenePixels& sp, const kol::ScenePixels::SpriteBlock& b, float x, float y, float rgb[3], float* alpha) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    *alpha = 0.0f;
    if (!sp.fall_vapor) return;
    const kol::FallVapor& v = sp.fall_vapor[b.fx_seed];
    const float fx = (x - b.fx_cx - v.wx0) * 0.5f, fy = (y - b.fx_top - v.wy0) * 0.5f;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    if (ix < 0 || iy < 0 || ix + 1 >= v.w || iy + 1 >= v.h) return;
    const float tx = fx - ix, ty = fy - iy;
    const float* c = &v.cells[(static_cast<std::size_t>(iy) * v.w + ix) * 2];
    const float* d = c + static_cast<std::size_t>(v.w) * 2;
    float a = (c[0] * (1 - tx) + c[2] * tx) * (1 - ty) + (d[0] * (1 - tx) + d[2] * tx) * ty;
    const float tone = (c[1] * (1 - tx) + c[3] * tx) * (1 - ty) + (d[1] * (1 - tx) + d[3] * tx) * ty;
    // Soft against what stands near its plane: a flat plane through a rounded rock is
    // crossed only by the rock's nearest columns, and in the depth order alone those cut
    // the cloud in a hard vertical strip (the user, on the Thor, saw vertical tears). So
    // the quads go in late enough to cover what is near, and the cloud thins here as a
    // thing comes nearer than the plane. Not by the pool's flat water, which lies under
    // it, nor a lifted thing's own pixels (with the camera moved they lie elsewhere: the
    // hole of a spike's shape), only by what they stand on.
    const int ipx = static_cast<int>(x), ipy = static_cast<int>(y);
    if (a > 0.0f && ipx >= 0 && ipy >= 0 && ipx < sp.w && ipy < sp.h && sp.xz) {
        const std::size_t k = static_cast<std::size_t>(ipy) * sp.w + ipx;
        if (!(sp.kind && sp.kind[k] == kol::kSceneFlat && sp.water && sp.water[k])) {
            const bool lifted = sp.lift && sp.lift[k] && sp.under_xz;
            const float there = static_cast<float>(lifted ? sp.under_xz[k] : sp.xz[k]);
            const kol::MapFall& f = kol::kMapFalls[b.fx_seed];
            const float along = 2.0f * (x - static_cast<float>(b.sx) - 0.5f * b.w);
            const float plane = static_cast<float>(b.xz) + (f.on_x ? -along : along);
            a *= 1.0f - fx_smooth(-8.0f, 48.0f, there - plane);
        }
    }
    *alpha = a;
    // Its colour is the falls' hue at full brightness, so the scene's fades
    // (the game darkening the map's palette: states/thor/0255, rows 0, 2, 3 to
    // ~57%) left it bright over a dimmed cave (the user, 2026-11-13, saw the
    // waterfall effect not fade with the screen): times the map's dim
    // (ScenePixels::map_dim). The falls' body and strands take the art's own
    // colours and fade with it. KOL_VAPOR_NO_DIM=1: as before.
    static const bool no_dim = std::getenv("KOL_VAPOR_NO_DIM") != nullptr;
    const float dim = no_dim ? 1.0f : std::clamp(sp.map_dim, 0.0f, 1.0f);
    for (int ch = 0; ch < 3; ++ch) rgb[ch] = g_fall_tint[ch] * tone * a * dim;
}

// An effect's square (Options > Special Effects; fill_card): every texel
// the effect's, lit by itself.
void fx_square(const kol::ScenePixels& sp, const kol::ScenePixels::SpriteBlock& b, int ax0, int ay0, float card_t) {
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    // Worked out once a corner (its alpha the mesh's, g_fx_alpha);
    // a texel the mean of its four.
    constexpr int kC = kSlot + 1;
    static thread_local std::vector<float> cc;
    cc.assign(static_cast<std::size_t>(kC) * kC * 3, 0.0f);
    const std::size_t abase = static_cast<std::size_t>(b.slot) * kC * kC;
    if (g_fx_alpha.size() < abase + kC * kC) g_fx_alpha.resize(abase + kC * kC, 0);
    // At 30 a second: a square made from the same part of the same
    // flame (its place relative to the flame, not on screen: the
    // camera pans) in this frame pair takes the texels made then.
    std::uint32_t fkey = kol::kFnvBasis;
    for (float f : {static_cast<float>(b.fx_seed), static_cast<float>(b.effect), b.fx_heat, b.fx_width,
                    static_cast<float>(b.fx_palette), static_cast<float>(b.w),
                    static_cast<float>(b.h), static_cast<float>(b.sx) - b.fx_cx, static_cast<float>(b.sy) - b.foot_row,
                    b.fx_top - b.foot_row, sp.fx_dark, sp.fx_white, sp.fx_obj_dark, sp.fx_obj_white,
                    b.effect == 3 ? sp.map_dim : 1.0f, static_cast<float>(sp.serial >> 1)})
        kol::fnv_mix(fkey, static_cast<std::uint32_t>(std::lround(f * 64.0f)));
    FxCache& fc = g_fx_cache[fkey];
    const bool hit = fc.serial + 2u > sp.serial && !fc.rgb.empty();
    fc.serial = sp.serial;
    if (hit) {
        for (int y = 0; y < b.h; ++y)
            std::copy_n(&fc.rgb[static_cast<std::size_t>(y) * b.w * 3], b.w * 3,
                        &sp.atlas_rgb[(static_cast<std::size_t>(ay0 + y) * sp.atlas_w + ax0) * 3]);
        std::copy_n(fc.alpha.data(), kC * kC, &g_fx_alpha[abase]);
        return;
    }
    fc.rgb.assign(static_cast<std::size_t>(b.w) * b.h * 3, 0);
    fc.alpha.assign(static_cast<std::size_t>(kC) * kC, 0);
    for (int j = 0; j <= b.h; ++j)
        for (int i = 0; i <= b.w; ++i) {
            float c[3], al = 0.0f;
            if (b.effect == 3) vapor_texel(sp, b, static_cast<float>(b.sx + i), static_cast<float>(b.sy + j), c, &al);
            else effect_texel(b, static_cast<float>(b.sx + i), static_cast<float>(b.sy + j), card_t, c, &al);
            // The game's fade, as on its own BG1 (premultiplied);
            // a sprite's fire (fx_over: map 63's campfire, where
            // BG1's own brightening is its firelight glow) by the
            // sprite layer's instead (the camp's fade to black).
            const float fw = b.fx_over ? sp.fx_obj_white : sp.fx_white,
                        fd = b.fx_over ? sp.fx_obj_dark : sp.fx_dark;
            for (int ch = 0; ch < 3; ++ch)
                cc[(static_cast<std::size_t>(j) * kC + i) * 3 + ch] = (c[ch] + (al - c[ch]) * fw) * (1.0f - fd);
            g_fx_alpha[abase + static_cast<std::size_t>(j) * kC + i] =
                static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(al, 0.0f, 1.0f)));
        }
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const std::size_t a = static_cast<std::size_t>(ay0 + y) * sp.atlas_w + ax0 + x;
            const std::size_t q = (static_cast<std::size_t>(y) * kC + x) * 3;
            for (int ch = 0; ch < 3; ++ch) {
                const float m = 0.25f * (cc[q + ch] + cc[q + 3 + ch] + cc[q + kC * 3 + ch] + cc[q + kC * 3 + 3 + ch]);
                sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(m, 0.0f, 1.0f)));
                fc.rgb[(static_cast<std::size_t>(y) * b.w + x) * 3 + ch] = sp.atlas_rgb[a * 3 + ch];
            }
        }
    std::copy_n(&g_fx_alpha[abase], kC * kC, fc.alpha.data());
}

// An ice crystal's texel as frosted glass (fill_card; see the crystal
// there): texel t at scene pixel (px, py) of a w x h scene, `unlit` its
// colour before its light, (gx0..gx1, gy0..gy1) the crystal's extent,
// fb the scene's fade.
void crystal_glass(const kol::ScenePixels& sp, int w, int h, int px, int py, const std::uint8_t* unlit, int gx0, int gx1,
                   int gy0, int gy1, float fb, std::uint8_t* t) {
    // What lies behind, blurred (a 5 x 5 Gaussian, sigma 2),
    // in the texel's own light.
    float bh[3] = {0.0f, 0.0f, 0.0f}, wsum = 0.0f;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int qx = px + dx, qy = py + dy;
            if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
            const float wgt = std::exp(-static_cast<float>(dx * dx + dy * dy) / 8.0f);
            const std::uint8_t* u = sp.under_rgb + (static_cast<std::size_t>(qy) * w + qx) * 3;
            for (int ch = 0; ch < 3; ++ch) bh[ch] += wgt * u[ch];
            wsum += wgt;
        }
    const float lit_k = std::clamp((t[0] + t[1] + t[2] + 1.0f) / (unlit[0] + unlit[1] + unlit[2] + 1.0f),
                                   0.0f, 1.5f);
    // Fading out over its bottom quarter: its V's floor
    // (white snow took the sheen, JY).
    const float span_y = std::max(1.0f, 0.25f * (gy1 - gy0));
    const float on = std::clamp((gy1 - py) / span_y, 0.0f, 1.0f);
    float c[3];
    for (int ch = 0; ch < 3; ++ch) c[ch] = (1.0f - 0.3f * on) * t[ch] + 0.3f * on * lit_k * bh[ch] / wsum;
    // The sheen: a band across the crystal, light from the
    // upper right, its place by the camera's turn.
    const float u = static_cast<float>(px - gx0) / (gx1 - gx0 + 1) -
                    0.8f * static_cast<float>(py - gy0) / (gy1 - gy0 + 1);
    const float centre = 0.35f + 0.3f * g_cam_cap.look_x - 0.2f * g_cam_cap.look_y;
    const float band = std::exp(-((u - centre) / 0.12f) * ((u - centre) / 0.12f));
    const float f = (unlit[0] + unlit[1] + unlit[2]) / 765.0f;
    const float sh = 0.55f * on * band * (0.4f + 0.6f * f) * std::min(1.0f, lit_k);
    const float white = 255.0f * fb;
    for (int ch = 0; ch < 3; ++ch) c[ch] += (white - c[ch]) * sh;
    c[0] *= 1.0f - 0.03f * on; c[1] *= 1.0f - 0.01f * on; c[2] *= 1.0f + 0.04f * on;
    for (int ch = 0; ch < 3; ++ch) t[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0L, 255L));
}

// The glints: a small cross on its three brightest texels
// (over the whole crystal), here on this square's (fill_card: block bi's).
void crystal_glints(const kol::ScenePixels& sp, int w, int h, int bi, const CrystalInfo& glass, int ax0, int ay0, float fb) {
    const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
    const std::uint8_t* const lift = sp.lift;
    static constexpr int kGlint[9][3] = {{0, 0, 100}, {1, 0, 50}, {-1, 0, 50}, {0, 1, 50}, {0, -1, 50},
                                         {2, 0, 20}, {-2, 0, 20}, {0, 2, 20}, {0, -2, 20}};
    const float white = 255.0f * fb;
    for (const std::size_t g : glass.glints) {
        if (g == SIZE_MAX) continue;
        for (const auto& d : kGlint) {
            const int px = static_cast<int>(g % w) + d[0], py = static_cast<int>(g / w) + d[1];
            const int x = px - b.sx, y = py - b.sy;
            if (x < 0 || y < 0 || x >= b.w || y >= b.h || px < 0 || py < 0 || px >= w || py >= h) continue;
            if (lift[static_cast<std::size_t>(py) * w + px] != bi + 1) continue;
            std::uint8_t* t = sp.atlas_rgb + (static_cast<std::size_t>(ay0 + y) * sp.atlas_w + ax0 + x) * 3;
            const float kk = d[2] / 100.0f;
            for (int ch = 0; ch < 3; ++ch)
                t[ch] = static_cast<std::uint8_t>(std::lround(t[ch] + (white - t[ch]) * kk));
        }
    }
}

// The fill under the cursor's floating hand (fill_card, KOL_HAND_ROW_FILL):
// each of the square's texels the hand covered (`hand_tex`, atlas texels)
// takes the prop's nearest texel on its row.
void fill_hand_rows(const kol::ScenePixels& sp, int ax0, int ay0, const std::vector<std::uint32_t>& hand_tex) {
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    static thread_local std::vector<std::uint8_t> hand_mark;   // (each pool thread its own)
    hand_mark.assign(static_cast<std::size_t>(sp.atlas_w) * kSlot, 0);
    const std::size_t row0 = static_cast<std::size_t>(ay0) * sp.atlas_w;
    for (std::uint32_t a : hand_tex) hand_mark[a - row0] = 1;
    for (std::uint32_t a : hand_tex) {
        const int ay = static_cast<int>(a / sp.atlas_w), ax = static_cast<int>(a % sp.atlas_w);
        for (int d = 1; d < kSlot; ++d) {
            int found = -1;
            for (int side = -1; side <= 1 && found < 0; side += 2) {
                const int nx = ax + side * d;
                if (nx < ax0 || nx >= ax0 + kSlot) continue;
                const std::size_t na = static_cast<std::size_t>(ay) * sp.atlas_w + nx;
                if (sp.atlas_opaque[na] && !hand_mark[na - row0]) found = static_cast<int>(na);
            }
            if (found < 0) continue;
            std::copy_n(sp.atlas_rgb + static_cast<std::size_t>(found) * 3, 3, sp.atlas_rgb + a * 3);
            break;
        }
    }
}

// A lifted prop's or wall's square (fill_card): each empty texel next to
// one of its pixels takes that pixel's colour.
void pad_card_edges(const kol::ScenePixels& sp, int ax0, int ay0) {
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    const std::uint8_t* op = sp.atlas_opaque;
    for (int y = 0; y < kSlot; ++y)
        for (int x = 0; x < kSlot; ++x) {
            const std::size_t a = static_cast<std::size_t>(ay0 + y) * sp.atlas_w + ax0 + x;
            if (op[a]) continue;
            static constexpr int kN[4][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};
            for (const auto& d : kN) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= kSlot || ny >= kSlot) continue;
                const std::size_t na = static_cast<std::size_t>(ay0 + ny) * sp.atlas_w + ax0 + nx;
                if (!op[na]) continue;
                std::copy_n(sp.atlas_rgb + na * 3, 3, sp.atlas_rgb + a * 3);
                break;
            }
        }
}

// What every card's square reads as warp_cards fills them (fill_card), the
// same for all: filled before the pool's run, only read in it.
struct CardFill {
    const WarpFrame* wf;
    float card_t;                                   // the effects' and the wet look's clock
    const float* fire_of;                           // block_fire: per card, the fire light at its foot
    const float* spell_of;                          // block_spell: per card, a spell's light there and its colour
    WetNow wet_card;                                // the wet look on lifted walls and props
    float card_dsky[3];                             // its drips' colour (drip_sky)
    const std::vector<CrystalInfo>* crystals_of;    // the crystals
    bool no_glass, hidden_unlit, shade_twice;       // KOL_NO_CRYSTAL_GLASS, KOL_PERSP_HIDDEN_UNLIT, _SHADE_TWICE
    float shade_soft;                               // card_shade_soft()
};

// One card's square of the atlas, block bi (warp_cards: the effects' one
// after another, the rest on the pool's threads).
void fill_card(const CardFill& cf, int bi) {
    const WarpFrame& wf = *cf.wf;
    const kol::ScenePixels& sp = *wf.sp;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::uint8_t* const iface = wf.iface;
    const std::uint8_t* const lift = wf.lift;
    const std::int32_t* const xz = wf.xz;
    const std::uint8_t* const kind = wf.kind;
    const std::uint8_t* const edge_rows = wf.edge_rows;
    const float card_t = cf.card_t;
    const float* const fire_of = cf.fire_of;
    const float* const spell_of = cf.spell_of;
    const WetNow& wet_card = cf.wet_card;
    const float* const card_dsky = cf.card_dsky;
    const std::vector<CrystalInfo>* const crystals_of = cf.crystals_of;
    const bool no_glass = cf.no_glass, hidden_unlit = cf.hidden_unlit, shade_twice = cf.shade_twice;
    const float shade_soft = cf.shade_soft;
    // A prop's texels under the cursor's floating hand: once (with the
    // props lifted) the compositor's colour under a sprite left BG1 out,
    // so they took the wall behind the statue, a dark hand-shaped hole
    // through it under the stick (FX), and were filled from the prop's
    // nearest texel on the row (see below: no longer by default).
    static thread_local std::vector<std::uint32_t> hand_tex;   // (each pool thread its own)
    const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
    const auto [ax0, ay0] = kol::slot_origin(b.slot, sp.atlas_per_row);
    if (b.effect) {   // Options > Special Effects: every texel the effect's, lit by itself
        fx_square(sp, b, ax0, ay0, card_t);
        return;
    }
    hand_tex.clear();
    // An ice crystal (SpriteBlock::crystal; the user's pick F of the
    // stills, 2026-10-28, after asking for slightly transparent crystals with
    // light reflections, then for the transparency to be a little
    // blurry): inside its dark outline, 30% of what the game drew behind
    // it (under BG1), blurred over 2 px, as frosted glass; a cool
    // sheen across it from the sun's side (upper right) that slides as
    // the camera turns; a glint on its three brightest texels.
    // KOL_NO_CRYSTAL_GLASS=1: as before (no_glass).
    const CrystalInfo* glass = nullptr;
    if (b.crystal >= 0 && b.prop && !no_glass && sp.under_rgb && sp.scene_codes)
        for (const CrystalInfo& c : *crystals_of)
            if (c.cell == b.crystal) glass = &c;
    const int gx0 = glass ? glass->x0 : 0, gx1 = glass ? glass->x1 : -1;
    const int gy0 = glass ? glass->y0 : 0, gy1 = glass ? glass->y1 : -1;
    const float fb = std::clamp(sp.scene_bright, 0.0f, 1.0f);
    // On a dark map (the darkness below), what is the same for every
    // texel of the square, worked out once: whether it is lit (a
    // flame is its own light: the night leaves it), the fire's and a
    // spell's light at its foot, and for a unit or a prop the ground's
    // light at its foot (fk below: its lamps' light and darkness).
    const bool night = sp.dark < 1.0f && !b.fire;
    const bool by_foot = !b.surface && !b.scenery;   // (a door in its wall: by its own pixel's, as the wall)
    const bool card = sp.unit_lamp && !b.surface && !b.prop && !b.scenery;
    const unsigned fire_g = fire_of[bi] > 0.0f ? static_cast<unsigned>(std::min(255.0f, 255.0f * fire_of[bi])) : 0u;
    const unsigned spell_g =
        spell_of[bi * 4] > 0.0f ? static_cast<unsigned>(std::min(255.0f, 255.0f * spell_of[bi * 4])) : 0u;
    auto with_fire = [&](unsigned g) {   // the fire light at its foot, and a spell's
        if (fire_of[bi] > 0.0f) g = std::max(g, fire_g);
        if (spell_of[bi * 4] > 0.0f) g = std::max(g, spell_g);
        return g;
    };
    std::size_t foot_k = 0;
    unsigned foot_g = 0u;
    float foot_dark = sp.dark;
    if (night && by_foot) {
        const int fx = std::clamp(b.sx + b.w / 2, 0, w - 1), fy = std::clamp(b.sy + b.h, 0, h - 1);
        foot_k = static_cast<std::size_t>(fy) * w + fx;
        // A prop whose square's foot pixel no lamp reaches: by its own
        // lowest pixels instead (the ground behind them, lifted off), the
        // brightest of its bottom three rows. The square's foot can be
        // anything under it: map 79's crystal clump on its terrace took a
        // floor pixel the lamp never reaches and went black (the user
        // reported one stone prop as dark). Only then (any lit foot
        // as before: map 51's pots and rocks unchanged).
        // KOL_PROP_FOOT_SQUARE=1: by the square's foot always, as before.
        static const bool foot_square = std::getenv("KOL_PROP_FOOT_SQUARE") != nullptr;
        if (b.prop && !foot_square && lift && lamp_light(sp, foot_k) == 0u) {
            int low = -1;
            for (int j = b.h - 1; j >= 0 && low < 0; --j)
                for (int i = 0; i < b.w; ++i) {
                    const int px = b.sx + i, py = b.sy + j;
                    if (px >= 0 && py >= 0 && px < w && py < h && lift[static_cast<std::size_t>(py) * w + px] == bi + 1) {
                        low = j;
                        break;
                    }
                }
            unsigned best = 0u;
            std::size_t best_k = foot_k;
            for (int j = std::max(0, low - 2); low >= 0 && j <= low; ++j)
                for (int i = 0; i < b.w; ++i) {
                    const int px = b.sx + i, py = b.sy + j;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if (lift[k] != bi + 1) continue;
                    const unsigned g = lamp_light(sp, k);
                    if (g > best || best_k == foot_k) { best = g; best_k = k; }
                }
            foot_k = best_k;
        }
        if (!card) foot_g = with_fire(lamp_light(sp, foot_k));   // (a unit on a card: the card's own)
        foot_dark = dark_at(sp, foot_k);
    }
    for_card_texels(sp, w, h, bi, false, [&](int, int, int px, int py, std::size_t k, std::size_t a) {
        // A unit's texel another unit hides in the frame: the
        // camera moved by the stick shows it beside that unit.
        // It keeps the colour its own sprite gives it (the atlas,
        // from OAM) and takes the same light as the rest of its
        // card; left out, it showed unlit -- lit up -- under the
        // other units (GX, asked 2026-10-15).
        // KOL_PERSP_HIDDEN_UNLIT=1: the old way (hidden_unlit).
        const bool own = lift[k] == bi + 1 || (sp.lift_copy && sp.lift_copy[k] == bi + 1);
        // Interface moved with a unit (kol_focus overlay_card):
        // the colours the game drew, unlit (its texels over a
        // lifted prop came from the frame in kol_focus).
        if (b.overlay) {
            if (own) std::copy_n(scene + k * 3, 3, sp.atlas_rgb + a * 3);
            return;
        }
        if (!own && (hidden_unlit || b.prop || b.surface || !sp.atlas_opaque || !sp.atlas_opaque[a]))
            return;
        if (own) {
            // A prop's pixel under interface (the rock under the
            // command menu's corner, FG): the game's own colour
            // there, without the sprite.
            const bool under_menu = b.prop && (iface[k] || wf.moved_overlay(k)) && obj_over(sp, k);
            std::copy_n((under_menu ? sp.under_rgb : scene) + k * 3, 3, sp.atlas_rgb + a * 3);
            if (under_menu && sp.floating && sp.floating[k]) hand_tex.push_back(static_cast<std::uint32_t>(a));
            // See-through (map 100's smoke, b.see_through): the
            // frame's colour less the ground the game blended in
            // (behind BG1, under_rgb, at 1 - see_through), so drawn
            // premultiplied over that ground it is the frame's
            // pixel again, and over another the smoke on that.
            if (b.see_through < 1.0f && sp.under_rgb) {
                const float keep = 1.0f - b.see_through;
                for (int ch = 0; ch < 3; ++ch) {
                    const float f = sp.atlas_rgb[a * 3 + ch], g = sp.under_rgb[k * 3 + ch];
                    sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(f - keep * g), 0L, 255L));
                }
            }
            // Fading in: toward the ground the compositor drew
            // under the sprite (the hidden texels are mixed so
            // in kol_focus).
            if (b.fade < 1.0f && sp.under_rgb)
                for (int ch = 0; ch < 3; ++ch)
                    sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(std::lround(
                        sp.atlas_rgb[a * 3 + ch] * b.fade + sp.under_rgb[k * 3 + ch] * (1.0f - b.fade)));
        }
        std::uint8_t unlit[3];   // the texel before its light (under_white)
        std::copy_n(sp.atlas_rgb + a * 3, 3, unlit);
        // An indoor map's darkness. Lifted terrain (a wall's
        // square) by its own pixel's light, and tinted by it as
        // the map is: by one value a square, from the ground at
        // its foot, whole squares of wall went light or dark as
        // the camera moved (states/GR, the cursor stepping
        // down). A unit or a prop by the light on the ground at
        // its foot (its square's bottom row, middle column).
        // Worked out here, before the wet look, the occlusion
        // and the shade below change the texel: the torchlight's
        // daylight colours (lamp_palette) look the texel's
        // colour up, and a changed one is no longer in the table
        // (lifted BG1 by the torches on map 58 kept the night's
        // colours: the user, 2026-10-27, the ground where the
        // units arrive, states/JM ~550). As on the map's pixels
        // (apply_lighting: looked up before the occlusion).
        // (night: above, once a square.)
        float f = 1.0f;
        unsigned self_g = 0u;
        if (night) {
            // (Its foot's pixel for a unit or a prop: foot_k.)
            const std::size_t fk = by_foot ? foot_k : k;
            // (A unit's own shadow lies at its feet: it takes the
            // light there without it.) A unit on a card of its
            // own light, when kol_focus worked it out
            // (unit_lamp: a ledge shades part of it): `card`, above.
            // The windows' light: a wall's square by its own
            // pixel's, a unit or prop by the glow at its foot
            // and the sun through a window on its own texel.
            unsigned wl = 0u;
            if (sp.win_glow) {
                wl = sp.win_glow[fk];
                if (b.surface || b.prop) {
                    if (sp.lift_win_patch) wl = std::max<unsigned>(wl, sp.lift_win_patch[k]);
                } else if (sp.unit_win_patch_a && a < sp.unit_win_a_n) {
                    wl = std::max<unsigned>(wl, sp.unit_win_patch_a[a]);
                }
            }
            // The moon's on a unit (as on the ground above).
            if (!b.surface && !b.prop && sp.moon_unit_patch_a && a < sp.moon_unit_a_n && sp.moon_unit_patch_a[a])
                wl = std::max(wl, static_cast<unsigned>(std::lround(std::min(1.0f, sp.moon_strength) *
                                                                     sp.moon_unit_patch_a[a])));
            // A statue carrying its flame (b.self_lit): lit by
            // it, full over the upper part, fading to dark toward
            // its foot by the texel's height (a row is 2 units).
            if (b.self_lit > 0.0f && b.self_top > b.self_low) {
                static const float fade = [] {
                    const char* e = std::getenv("KOL_STATUE_FADE");
                    // The lowest 35%: the user's pick B (statue_fade.png).
                    return e ? std::clamp(static_cast<float>(std::atof(e)), 0.05f, 1.0f) : 0.35f;
                }();
                const float Y = static_cast<float>(b.y) + 2.0f * (b.foot_row - static_cast<float>(py) - 0.5f);
                const float t = std::clamp((Y - b.self_low) / ((b.self_top - b.self_low) * fade), 0.0f, 1.0f);
                self_g = static_cast<unsigned>(std::lround(255.0f * std::min(1.0f, b.self_lit * t * t * (3.0f - 2.0f * t))));
                wl = std::max(wl, self_g);
            }
            // The light at its foot (with the fire's and a spell's:
            // with_fire): a unit on its card by the card's texel,
            // a unit or prop by the ground's (foot_g), a wall's
            // square by its own pixel's.
            const unsigned g = card ? with_fire(a < sp.unit_lamp_n ? sp.unit_lamp[a] : 0u)
                               : by_foot ? foot_g
                                         : with_fire(lamp_light(sp, fk));
            f = dark_light(sp, g, (b.surface || b.scenery) && sp.lamp_shade ? sp.lamp_shade[fk] : 0u, wl,
                           by_foot ? foot_dark : dark_at(sp, fk));
            if (sp.lamp_pal && (b.surface || b.scenery || b.prop) && f > sp.dark)   // (a unit: its own palette)
                lamp_palette(sp, sp.atlas_rgb + a * 3, (f - sp.dark) / (1.0f - sp.dark));
        }
        // Wet (wet_now), after the texel's copy before its light: the
        // white fade gives it back as drawn (before it, every prop on
        // map 102's "An Excerpt from the Ogre Battle Saga" stayed a
        // grey shape on the white). Darker and more saturated, as the scene's
        // surfaces; a lifted wall's texels take the drips down its
        // column (x - kx - ox) as the scene's walls do, by its own
        // height (the scene keeps the wall's; the ground behind it
        // is kept apart, g_under_*).
        if (wet_card.ok && (b.surface || b.prop) && !b.fire && !b.effect) {
            const WetV& v = wet_card.v;
            float c[3] = {static_cast<float>(sp.atlas_rgb[a * 3]), static_cast<float>(sp.atlas_rgb[a * 3 + 1]),
                          static_cast<float>(sp.atlas_rgb[a * 3 + 2])};
            wet_tone(c, v);
            float s1;
            constexpr float kTail = 34.0f;
            if (b.surface && v.drips > 0.0f && sp.y &&
                wall_drip(px - sp.kx - sp.ox, static_cast<float>(sp.y[k]), card_t, v.drips, &s1) && s1 < kTail)
                drip_streak(c, s1, kTail, 3.0f, v, card_dsky, 0.9f, 1.15f);
            for (int ch = 0; ch < 3; ++ch)
                sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0L, 255L));
        }
        // Lifted terrain is terrain: its texels take its own
        // occlusion here (the scene's pixel there is the surface
        // behind, and takes that one's below).
        if (b.surface && sp.lift_ao && sp.lift_ao[k] < 255u)
            for (int ch = 0; ch < 3; ++ch)
                sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(
                    (sp.atlas_rgb[a * 3 + ch] * sp.lift_ao[k] + 127u) / 255u);
        // In the terrain's shadow, as the atlas's own texels. A
        // hidden texel keeps the atlas's colour, which kol_focus
        // shaded already: shaded again, a unit behind another
        // sprite in the frame went twice as dark there (Eleanor
        // behind the church door's sprite as she shuts it, a
        // door-shaped shadow over her: the user, states/thor/0013).
        // KOL_PERSP_HIDDEN_SHADE_TWICE=1: as before (shade_twice).
        // (The shade's factor once a texel: the sunset below takes
        // it too.)
        const bool shade_sunset = sp.sunset_grade > 0.0f;
        const float card_f = own || shade_twice || shade_sunset
                                 ? kol::ScenePixels::card_shade(static_cast<float>(py), b.shade_row, b.shade_k,
                                                                shade_soft)
                                 : 1.0f;
        if (own || shade_twice) {
            const float f = card_f;
            if (f < 1.0f)
                for (int ch = 0; ch < 3; ++ch)
                    sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(
                        std::lround(sp.atlas_rgb[a * 3 + ch] * f));
        }
        // A prop's shade over its own foot (kol_focus).
        if (b.prop && b.base_rows > 0.0f) {
            // Bent, its foot is its cell's front edge there.
            const float foot = b.bend_sx < 0 ? b.foot_row
                : b.foot_row + 0.25f * (static_cast<float>(b.bend_tip) -
                                        2.0f * std::min(16.0f, std::abs(px + 0.5f - b.bend_sx))) -
                      0.5f * static_cast<float>(px < b.bend_sx ? b.bend_dyl : b.bend_dyr);
            const float f = kol::ScenePixels::prop_base_light(
                foot - static_cast<float>(py), b.base_rows, b.base_dark, b.base_raise);
            if (f < 1.0f)
                for (int ch = 0; ch < 3; ++ch)
                    sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(
                        std::lround(sp.atlas_rgb[a * 3 + ch] * f));
        }
        // The mesh paint: a lifted patch of terrain is terrain,
        // painted as its own surface (units and props keep their
        // colours). Left in its art's colours it read as not
        // terrain (the snowfield's raised blocks, FH).
        if (b.surface && mesh_paint_on())
            mesh_paint_px(sp.atlas_rgb + a * 3, kind[k], xz[k], false, false);
        // The late day, as on the scene (apply_lighting).
        if (sp.evening > 0.0f) evening_tint(sp.atlas_rgb + a * 3, sp.evening);
        // The fire light at its foot (a unit, a prop; not a flame).
        if (fire_of[bi] > 0.0f) fire_light_px(sp.atlas_rgb + a * 3, (sp.dark < 1.0f ? 0.5f : 1.0f) * fire_of[bi]);
        // A spell's light at its foot: a unit keeps a little more
        // of its own colours (the mockup paled them).
        if (spell_of[bi * 4] > 0.0f)
            spell_light_px(sp.atlas_rgb + a * 3, 0.75f * spell_of[bi * 4], &spell_of[bi * 4 + 1]);
        // At sunset, by its own shade (the terrain's shadow over it).
        if (shade_sunset) {
            const float f = card_f;
            const float lit = b.shade_k < 1.0f ? std::clamp((f - b.shade_k) / (1.0f - b.shade_k), 0.0f, 1.0f) : 1.0f;
            sunset_grade(sp.atlas_rgb + a * 3, sp.sunset_grade, lit);
        }
        // The darkness (f, worked out above).
        if (night) {
            for (int ch = 0; ch < 3; ++ch)
                sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(std::lround(sp.atlas_rgb[a * 3 + ch] * f));
            if ((b.surface || b.scenery) && sp.glow_lamp)
                if (const unsigned lg = lamp_light_shaded(sp, k)) torch_tint(sp.atlas_rgb + a * 3, sp, lg, sp.glow_lamp[k], 0.0f, static_cast<long>(k));
            // ... and tinted by its flame, as the pit's wall is.
            if (self_g) torch_tint(sp.atlas_rgb + a * 3, sp, self_g, static_cast<unsigned>(b.self_lamp));
        }
        // A map's lamp on a unit (kMapLamps; worked out on its
        // own pixels in the frame only).
        if (own && sp.unit_glow && !b.prop && !b.surface && !b.scenery && sp.unit_glow[k])
            torch_tint(sp.atlas_rgb + a * 3, sp, sp.unit_glow[k], sp.unit_glow_lamp[k]);
        // The light through the windows on lifted terrain and props.
        if (sp.lift_win_patch && (b.prop || b.surface) &&
            (sp.lift_win_patch[k] || sp.lift_win_haze[k]))
            window_light(sp.atlas_rgb + a * 3, sp.lift_win_patch[k], sp.lift_win_haze[k],
                         sp.win_strength, sp.scene_bright, sp.win_tint);
        // The light through the windows on a unit.
        if (sp.unit_win_patch_a && !b.prop && !b.surface && a < sp.unit_win_a_n &&
            (sp.unit_win_patch_a[a] || sp.unit_win_haze_a[a]))
            window_light(sp.atlas_rgb + a * 3, sp.unit_win_patch_a[a], sp.unit_win_haze_a[a],
                         sp.win_strength, sp.scene_bright, sp.win_tint, true, unit_cool_keep);
        if (sp.moon_unit_patch_a && !b.prop && !b.surface && a < sp.moon_unit_a_n &&
            (sp.moon_unit_patch_a[a] || sp.moon_unit_haze_a[a]))
            window_light(sp.atlas_rgb + a * 3, sp.moon_unit_patch_a[a], sp.moon_unit_haze_a[a],
                         sp.moon_strength, sp.scene_bright, sp.moon_tint, true, true);
        // A unit, a lifted prop or wall at the map's cut edge
        // fades as the map does.
        if (sp.edge_fade && sp.edge_fade[k] < 255u)
            edge_fade_px(sp.atlas_rgb + a * 3, &edge_rows[static_cast<std::size_t>(py) * 3],
                         sp.edge_fade[k]);
        if (sp.scene_white > 0.0f) under_white(sp.atlas_rgb + a * 3, unlit, sp.scene_white);
        if (sp.field_layer >= 2) field_px(sp, sp.atlas_rgb + a * 3, b.prop || b.surface ? 1u : 4u);
        if (glass && own && unlit[0] + unlit[1] + unlit[2] >= 170 && gx1 >= gx0)
            crystal_glass(sp, w, h, px, py, unlit, gx0, gx1, gy0, gy1, fb, sp.atlas_rgb + a * 3);
        // A blue flame on a prop (b.glow_flame, map 57's Shaher; the user,
        // 2026-11-11: the AO darkened Shaher's blue flame; asked for it
        // to be bright and to emit light): its texels as the game drew them,
        // a little brighter, over all the light above (the shade, the foot's
        // occlusion, the evening); the white fade still takes them.
        // KOL_FLAME_GLOW=<gain> (1.12).
        if (b.glow_flame && own && glow_flame_px(unlit)) {
            static const float gain = kol::env_num_min("KOL_FLAME_GLOW", 0.0f, 1.12f);
            for (int ch = 0; ch < 3; ++ch)
                sp.atlas_rgb[a * 3 + ch] = static_cast<std::uint8_t>(std::min(255.0f, unlit[ch] * gain));
            if (sp.scene_white > 0.0f) under_white(sp.atlas_rgb + a * 3, unlit, sp.scene_white);
        }
    });
    if (glass) crystal_glints(sp, w, h, bi, *glass, ax0, ay0, fb);
    // (Now the compositor's colour under the hand is the prop's own,
    // BG1 kept: 0x0C, the HUD's layers, is all the scene leaves out.
    // The fill smeared a bush's branches into a pale band once the
    // stick moved the hand off it, JC, the user's slot 3: the hand
    // affected the bush behind it; FX's statue keeps its own
    // texture under it, no hole. KOL_HAND_ROW_FILL=1: the fill.)
    static const bool hand_row_fill = std::getenv("KOL_HAND_ROW_FILL") != nullptr;
    if (!hand_tex.empty() && hand_row_fill) fill_hand_rows(sp, ax0, ay0, hand_tex);
    // A lifted prop's square: the texels round its pixels take their
    // colours, so a run's edge sampled a texel past it reads the prop,
    // not the square's black (EK, the snowfield's trees: black seams).
    if (b.prop || b.surface) pad_card_edges(sp, ax0, ay0);
}

// Each unit's square takes the frame's own colours where the frame shows
// it; then its pixels in the frame get the ground under them. First of
// all: everything below reads the scene. Done after the backdrop, a unit
// standing against the sky (Alphonse, CG_alphonse_stretch) had sky under
// him, so the backdrop copied his colours there -- a second Alphonse in
// the sky -- and carried them along the row under the map as streaks.
void warp_cards(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const layers = wf.layers;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const iface = wf.iface;
    const std::uint8_t* const lift = wf.lift;
    // The backdrop's colour per row, for the map's cut edge (sp.edge_fade).
    static std::vector<std::uint8_t> edge_rows;
    if (sp.edge_fade) edge_sky_rows(layers, iface, scene, w, h, edge_rows);
    wf.edge_rows = edge_rows.data();
    if (lift) {
        const float card_t = scene_secs(sp);   // the effects' and the wet look's clock
        for (auto it = g_fx_cache.begin(); it != g_fx_cache.end();)   // the effects' squares of frames gone by
            it = it->second.serial + 4u < sp.serial ? g_fx_cache.erase(it) : std::next(it);
        // The fire light at each card's foot, once a card (not a texel); and
        // a spell's light there, and its colour (spell_lights).
        static thread_local std::vector<float> block_fire, block_spell;
        block_fire.assign(static_cast<std::size_t>(sp.block_count), 0.0f);
        block_spell.assign(static_cast<std::size_t>(sp.block_count) * 4, 0.0f);
        if (sp.fire_light_count || sp.spell_light_count)
            for (int bi = 0; bi < sp.block_count; ++bi) {
                const auto& b = sp.blocks[bi];
                if (b.fire || b.effect) continue;
                const float dxz = 2.0f * (static_cast<float>(b.sx) + 0.5f * b.w - sp.kx - sp.ox);
                if (sp.fire_light_count)
                    block_fire[bi] = fire_light_at(sp, 0.5f * (b.xz + dxz), static_cast<float>(b.y) + 8.0f, 0.5f * (b.xz - dxz));
                if (sp.spell_light_count)
                    block_spell[bi * 4] = spell_light_at(sp, 0.5f * (b.xz + dxz), static_cast<float>(b.y) + 8.0f,
                                                         0.5f * (b.xz - dxz), &block_spell[bi * 4 + 1]);
            }
        // The wet look on lifted walls and props (wet_now; the user noticed
        // BG1 did not get the wet effect: they are cards, drawn from the
        // atlas, and the wet pass runs on the scene only). Units stay as they are.
        const WetNow wet_card = wet_now(sp);
        // Its drips' colour (drip_sky), once a picture, not a texel.
        float card_dsky[3];
        drip_sky(wet_card.sky, card_dsky);
        // Each card's square on its own (they share no texel): the effects'
        // first, one after another (their cache, g_fx_cache), then the rest
        // on the row pool (map 102's fifty-odd props and walls were 0.9 ms
        // of the worker on the Mac, one after another).
        // (block_fire and block_spell are this thread's: the pool's threads
        // read them here.)
        const float* const fire_of = block_fire.data();
        const float* const spell_of = block_spell.data();
        // The switches the squares' texels test, and the rows the terrain's
        // shadow eases over on a card (card_shade_soft), read once here, not
        // a texel at a time on the pool's threads (each its own guard, and
        // card_shade_soft a call across files): see each below.
        static const bool no_glass = std::getenv("KOL_NO_CRYSTAL_GLASS") != nullptr;
        static const bool hidden_unlit = std::getenv("KOL_PERSP_HIDDEN_UNLIT") != nullptr;
        static const bool shade_twice = std::getenv("KOL_PERSP_HIDDEN_SHADE_TWICE") != nullptr;
        const float shade_soft = kol::card_shade_soft();
        // Each ice crystal's extent and its three brightest texels in its
        // upper three quarters (not its V's floor: a white snow texel at its
        // foot took one, JY), over all its squares (a crystal taller than a
        // square spans two; per square, the lower one's took its foot for a
        // top). See SpriteBlock::crystal and the frosted glass below.
        static thread_local std::vector<CrystalInfo> crystals;
        crystals.clear();
        for (int bi = 0; bi < sp.block_count; ++bi) {
            const auto& b = sp.blocks[bi];
            if (b.crystal < 0 || !b.prop) continue;
            CrystalInfo* c = nullptr;
            for (CrystalInfo& q : crystals)
                if (q.cell == b.crystal) c = &q;
            if (!c) {
                crystals.push_back({b.crystal, INT_MAX, INT_MAX, INT_MIN, INT_MIN, {SIZE_MAX, SIZE_MAX, SIZE_MAX}});
                c = &crystals.back();
            }
            for_card_texels(sp, w, h, bi, true, [&](int, int, int px, int py, std::size_t, std::size_t) {
                c->x0 = std::min(c->x0, px); c->x1 = std::max(c->x1, px);
                c->y0 = std::min(c->y0, py); c->y1 = std::max(c->y1, py);
            });
        }
        for (CrystalInfo& c : crystals) {
            std::array<int, 3> lum{-1, -1, -1};
            for (int bi = 0; bi < sp.block_count; ++bi) {
                const auto& b = sp.blocks[bi];
                if (b.crystal != c.cell || !b.prop) continue;
                for_card_texels(sp, w, h, bi, true, [&](int, int, int, int py, std::size_t k, std::size_t) {
                    if (py > c.y0 + 3 * (c.y1 - c.y0) / 4) return;
                    const int l = scene[k * 3] + scene[k * 3 + 1] + scene[k * 3 + 2];
                    if (l <= lum[2]) return;
                    int i = 2;
                    while (i > 0 && l > lum[i - 1]) { lum[i] = lum[i - 1]; c.glints[i] = c.glints[i - 1]; --i; }
                    lum[i] = l;
                    c.glints[i] = k;
                });
            }
        }
        // (crystals is this thread's: the pool's threads read it here.)
        const std::vector<CrystalInfo>* const crystals_of = &crystals;
        const CardFill fill{&wf, card_t, fire_of, spell_of, wet_card, {card_dsky[0], card_dsky[1], card_dsky[2]},
                            crystals_of, no_glass, hidden_unlit, shade_twice, shade_soft};
        for (int bi = 0; bi < sp.block_count; ++bi)
            if (sp.blocks[bi].effect) fill_card(fill, bi);
        row_pool().run(sp.block_count, 2, [&](int, int b0, int b1) {
            for (int bi = b0; bi < b1; ++bi)
                if (!sp.blocks[bi].effect) fill_card(fill, bi);
        });
        // Each pixel takes the ground the game drew under the unit: the
        // compositor's scene without sprites there (gbarecomp
        // g_ppu_scene_under_obj). Guessed from beside the unit, the fill
        // copied whatever lay there into the gap the unit leaves under the
        // stick: another unit's sprite, the dark face of the cliff behind a
        // griffin (CB_sprites_broken).
        // (Not the cursor's floating hand: it keeps its colours for step
        // 3b and fills under itself below. Over a lifted statue its pixels
        // are the statue's too, and it showed as the wall behind, a dark
        // hand-shaped hole, FX.)
        if (sp.under_rgb) {
            for (std::size_t k = 0; k < pixels; ++k)
                if (lift[k] && !wf.prop_under_menu(k) && !(sp.floating && sp.floating[k])) {
                    // (The sky under it: the smoothed sky's row (smooth_sky), as
                    // the sky beside it. The game's own row colour left specks
                    // in an art flame's shape where it was lifted out, map 9's
                    // brazier on the ruins' top, states/thor/0156.)
                    const bool sky_under = sp.scene_codes && (sp.scene_codes[k] >> 4) == kol::kLayerBackdrop &&
                                           g_sky_rows.size() == static_cast<std::size_t>(h) * 3;
                    std::copy_n(sky_under ? &g_sky_rows[(k / w) * 3] : sp.under_rgb + k * 3, 3, scene + k * 3);
                }
            g_hole_filled.assign(pixels, 0);
            for (int py = 0; py < h; ++py) {
                const std::size_t o = static_cast<std::size_t>(py) * w;
                int right = -1;   // the prop's nearest own hole to the right
                for (int px = w - 1; px >= 0; --px) {
                    const std::size_t k = o + px;
                    if (wf.prop_under_menu(k)) {
                        // The floating hand keeps its colours here too (it
                        // was drawn in the wall's from the hole beside it,
                        // the hand-shaped hole still seen on the Thor, FX).
                        if (sp.floating && sp.floating[k]) continue;
                        if (right >= 0 && lift[o + right] == lift[k]) {
                            std::copy_n(scene + (o + right) * 3, 3, scene + k * 3);
                            g_hole_filled[k] = 1;
                        }
                    } else {
                        right = lift[k] ? px : -1;
                    }
                }
            }
        } else
        // Without it (KOL_PERSP_MIRROR_FILL=1), each pixel of a run takes the
        // ground from its nearer side, mirrored across the run's edge, so the
        // ground reads as going on from both sides. (A copy of the run to the
        // left drew a block of whatever lay there: a dark rock beside the
        // ghosts in CF_ghosts_overlap.)
        for (int py = 0; py < h; ++py) {
            const std::size_t o = static_cast<std::size_t>(py) * w;
            auto ground = [&](int x) {
                return x >= 0 && x < w && !lift[o + x] && !iface[o + x];
            };
            for (int px = 0; px < w; ++px) {
                if (!lift[o + px]) continue;
                int end = px;
                while (end + 1 < w && lift[o + end + 1]) ++end;
                for (int x = px; x <= end; ++x) {
                    const int dl = x - px + 1, dr = end - x + 1;
                    const int l = px - dl, r = end + dr;
                    const int src = dl <= dr ? (ground(l) ? l : ground(r) ? r : -1)
                                             : (ground(r) ? r : ground(l) ? l : -1);
                    if (src >= 0) std::copy_n(scene + (o + src) * 3, 3, scene + (o + x) * 3);
                }
                px = end;
            }
        }
    }
}

// 1. The backdrop: the sky as it is; under the map, the nearest sky on the
// same row, or on the nearest row that has any, so what the map uncovers
// is sky. Map art the interface covers is not: the scene has it (it
// leaves the HUD out) but nothing is placed from it, since the frame shows
// the HUD. The preparation list lies over the map's left edge (DM), and
// the map under it was carried along its rows into what the map
// uncovered, as dark and green streaks in the sky. The sky under a HUD
// panel stays sky; an interface sprite is not. Only the backdrop under a
// HUD pixel is known to be sky: a dialog box's text and frame (BG2 over
// BG3) record no third layer, and the map under the box was carried to
// the island's far edge as gold and dark streaks (state 109, map 19).
void warp_backdrop(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const layers = wf.layers;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w;
    const int view_x = wf.view_x, view_y = wf.view_y, view_w = wf.view_w, view_h = wf.view_h;
    const std::size_t view_pixels = wf.view_pixels;
    const std::uint8_t* const iface = wf.iface;
    const std::uint8_t* const kind = wf.kind;
    const unsigned hud_layers = kol::present_exclude_layers();
    // Where a rope over the sky was lifted (ScenePixels::rope_air): no sky of
    // its own, it takes its row's, as the map's pixels do. Copied as it was, it
    // showed black (nothing recorded under BG0), then as faint marks.
    auto rope_hole = [&](std::size_t k) { return sp.rope_air && sp.rope_air[k] >= 3; };
    // A card's hole over the sky (a unit lifted off it) where the sky is
    // recoloured: the scene there is the game's sky under the sprite, not the
    // recoloured one, so it is not a row's sky and takes the row's (map 93's
    // white birds left a blue spot of their shape in the green; with kSkyGame
    // the colour is the same).
    auto card_hole = [&](std::size_t k) {
        return sp.sky != kol::kSkyGame && wf.lift && wf.lift[k] && (layers[k] >> 4) == kol::kLayerBackdrop;
    };
    auto sky = [&](std::size_t k) {
        const unsigned top = layers[k] & 0x0Fu, under = layers[k] >> 4;
        return kind[k] == kol::kSceneNone && !iface[k] &&
               !(top < 4u && ((hud_layers >> top) & 1u) && under != kol::kLayerBackdrop) &&
               !rope_hole(k) && !card_hole(k);
    };
    g_backdrop.resize(view_pixels * 3);
    g_row_sky.assign(static_cast<std::size_t>(view_h), -1);
    for (int py = 0; py < view_h; ++py) {
        const std::size_t o = static_cast<std::size_t>(view_y + py) * w + view_x;
        for (int px = 0; px < view_w; ++px)
            if (sky(o + px)) {
                g_row_sky[py] = static_cast<int>(o + px);
                break;
            }
    }
    for (int py = 0; py < view_h; ++py) {
        int src = g_row_sky[py];
        for (int d = 1; src < 0 && d < view_h; ++d) {
            if (py - d >= 0 && g_row_sky[py - d] >= 0) src = g_row_sky[py - d];
            else if (py + d < view_h && g_row_sky[py + d] >= 0) src = g_row_sky[py + d];
        }
        static const std::uint8_t kBlack[3] = {0, 0, 0};
        // KOL_PERSP_HOLES=1 paints the backdrop under the map magenta, so a
        // gap in the mesh shows.
        static const std::uint8_t kMagenta[3] = {255, 0, 255};
        static const bool show_holes = std::getenv("KOL_PERSP_HOLES") != nullptr;
        const std::uint8_t* fill = src >= 0 ? scene + static_cast<std::size_t>(src) * 3 : kBlack;
        // A row with no sky of its own: the backdrop the game set for that
        // line (sp.row_backdrop), not the nearest row's sky. The castle's
        // sky gradient (map 86, JZ: an HBlank DMA walks palette entry 0 down
        // a table) showed only at the top and bottom of the screen, and the
        // rows between took one or the other: flat green, then flat magenta
        // (the user, 2026-10-28, noticed a gradient seemed intended but
        // missing). Not where the sky is recoloured (kSkyGame
        // only). KOL_SKY_NEAREST_ROW=1: the nearest row's, as before.
        static const bool nearest_row = std::getenv("KOL_SKY_NEAREST_ROW") != nullptr;
        if (g_row_sky[py] < 0 && !nearest_row && sp.row_backdrop && sp.sky == kol::kSkyGame)
            fill = (g_sky_rows.empty() ? sp.row_backdrop : g_sky_rows.data()) + static_cast<std::size_t>(view_y + py) * 3;
        const std::size_t o = static_cast<std::size_t>(view_y + py) * w + view_x;
        for (int px = 0; px < view_w; ++px) {
            const std::size_t k = o + px;
            std::uint8_t* dst = &g_backdrop[(static_cast<std::size_t>(py) * view_w + px) * 3];
            if (kind[k] == kol::kSceneNone && !rope_hole(k)) {
                // Interface over the sky (the cursor's hand, moved to its
                // place by step 3b): the sky the game drew there, without
                // sprites. Copied as it was, the game's own hand stayed in
                // the backdrop beside the moved one under the stick
                // (states/FJ_slot1_duplicate_hand).
                const bool over = (iface[k] || (sp.floating && sp.floating[k])) && obj_over(sp, k);
                // (A map's sprite faded out over the sky, faded_iface: the
                // row's sky. The game's sky under it is its own colour, not a
                // recoloured one, kEdgeFadeMaps: map 93's birds left a blue
                // spot of their shape in the green.)
                // ... and a card's hole over a recoloured sky (card_hole).
                if ((over && faded_iface(sp, layers, sp.floating, k)) || card_hole(k)) std::copy_n(fill, 3, dst);
                else std::copy_n((over ? sp.under_rgb : scene) + k * 3, 3, dst);
                if (sky(k)) fill = scene + k * 3;
            } else {
                std::copy_n(show_holes ? kMagenta : fill, 3, dst);
            }
        }
    }

    // A dark map's backdrop, its sky at night too (map 99, the burning town;
    // the light below runs on the scene, after this copy).
    // Not under the game's flash whitening it (backdrop_white: map 58's
    // lightning in the opening, a white sky behind the black mast).
    if (sp.dark < 1.0f) {
        const float keep = std::clamp(sp.backdrop_white, 0.0f, 1.0f);
        const unsigned d256 = static_cast<unsigned>(std::lround(256.0f * (sp.dark + (1.0f - sp.dark) * keep)));
        for (std::uint8_t& c : g_backdrop) c = static_cast<std::uint8_t>((c * d256 + 128u) >> 8);
    }
}

// The cursor's floating hand (kol_focus.cpp) keeps its colours for step
// 3b before step 2 fills under it.
void warp_hand(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const iface = wf.iface;
    const std::uint8_t* floating = sp.floating;
    g_float_px.clear();
    int fb_x0 = w, fb_x1 = -1, fb_y0 = h, fb_y1 = -1;
    if (floating)
        for (std::size_t k = 0; k < pixels; ++k) {
            if (!floating[k]) continue;
            const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
            g_float_px.push_back({px, py, scene[k * 3], scene[k * 3 + 1], scene[k * 3 + 2]});
            fb_x0 = std::min(fb_x0, px); fb_x1 = std::max(fb_x1, px);
            fb_y0 = std::min(fb_y0, py); fb_y1 = std::max(fb_y1, py);
        }

    // Under the floating hand the ground shows once the hand has moved off
    // it: the ground the game drew there (the compositor's scene without
    // sprites). Without that, each run of its pixels on a row is a copy of
    // the run of ground just left of it, so the ground's texture goes on.
    // (Filled with the one colour to its left, as under a menu, it showed as
    // a dark smear beside the hand.)
    if (floating && sp.under_rgb && sp.scene_codes) {
        for (int py = fb_y0; py <= fb_y1; ++py)
            for (int px = fb_x0; px <= fb_x1; ++px) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (floating[k] && (sp.scene_codes[k] & 0x0Fu) == kol::kLayerObj)
                    std::copy_n(sp.under_rgb + k * 3, 3, scene + k * 3);
            }
    } else if (floating)
        for (int py = fb_y0; py <= fb_y1; ++py) {
            const std::size_t o = static_cast<std::size_t>(py) * w;
            for (int px = fb_x0; px <= fb_x1; ++px) {
                if (!floating[o + px]) continue;
                int end = px;
                while (end + 1 < w && floating[o + end + 1]) ++end;
                const int len = end - px + 1;
                const int src = px - len >= 0 ? px - len : end + 1;
                if (src + len <= w)
                    for (int i = 0; i < len; ++i)
                        if (!iface[o + src + i])
                            std::copy_n(scene + (o + src + i) * 3, 3, scene + (o + px + i) * 3);
                px = end;
            }
        }
    wf.floating = floating;
    wf.fb_x0 = fb_x0;
    wf.fb_x1 = fb_x1;
    wf.fb_y0 = fb_y0;
    wf.fb_y1 = fb_y1;
}

// 2. What the mesh samples: interface sprites (menus, icons) are drawn
// flat on top, so under them the scene takes the ground the game drew
// there (the compositor's scene without sprites), or, without it, the
// colour to their left. Filled from the left, a command menu's rows
// carried a wing's dark grey across, and leaning the camera showed it
// as streaks past the menu's end (states/CL_menu_embedded.state).
void warp_under(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const iface = wf.iface;
    const std::uint8_t* const lift = wf.lift;
    const std::uint8_t* const floating = wf.floating;
    const std::uint8_t* const under_rgb = sp.under_rgb && sp.scene_codes ? sp.under_rgb : nullptr;
    for (int py = 0; py < h; ++py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        if (under_rgb)
            for (int px = 0; px < w; ++px) {
                const std::size_t k = o + px;
                if (iface[k] && !(floating && floating[k]) && !(lift && lift[k]) &&
                    (sp.scene_codes[k] & 0x0Fu) == kol::kLayerObj && !faded_iface(sp, wf.layers, floating, k))
                    std::copy_n(under_rgb + k * 3, 3, scene + k * 3);
            }
        int last = -1;
        auto filled = [&](int px) {   // not interface, or the hand's, cloned above
            const std::size_t k = o + px;
            if (wf.prop_under_menu(k)) return g_hole_filled.size() == pixels && g_hole_filled[k];
            return !iface[k] || (floating && floating[k]) || (lift && lift[k]) ||
                   (under_rgb && (sp.scene_codes[k] & 0x0Fu) == kol::kLayerObj) || faded_iface(sp, wf.layers, floating, k);
        };
        for (int px = 0; px < w; ++px) {
            if (filled(px)) { last = px; continue; }
            if (last >= 0) std::copy_n(scene + (o + last) * 3, 3, scene + (o + px) * 3);
        }
        int next = -1;
        for (int px = 0; px < w; ++px) {   // a row that starts with interface
            if (filled(px)) { next = px; break; }
        }
        if (next > 0)
            for (int px = 0; px < next; ++px)
                std::copy_n(scene + (o + next) * 3, 3, scene + (o + px) * 3);
    }
}

// The light on the scene (apply_lighting), then the water, the ice and the
// wet look by the warp's camera, the white fade and the flash's field.
float g_fallcard_tint[3] = {1.0f, 1.0f, 1.0f};   // warp_light's, for warp_flat
// The light on a fall card's piece while it was the map's (warp_light): `mul` 3 bytes a
// pixel, 128 = 1, 0 = none; (x0, y0) its corner from the card's first anchor; v0 each
// interface layer's vertical offset when it first showed as the card (-1: not yet).
struct FallLight {
    bool ok = false;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    int v0[2] = {-1, -1};
    unsigned on = 0;   // the interface layers on at the last card (bit 0 BG2, bit 1 BG3)
    std::vector<std::uint8_t> mul;
};
FallLight g_fall_light;
std::vector<std::uint8_t> g_card_mul;   // PresentWarpMesh::card_mul
void warp_light(WarpFrame& wf, kol::WorkerTimer& wt) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const layers = wf.layers;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const lift = wf.lift;
    // The light a fall card takes (ScenePixels::fall_card).
    // - While its piece is still the map's (fall_ready, not yet fall_card): what the
    //   light makes of every pixel round its anchors (lit over unlit, per channel,
    //   128 = 1), kept by its place from the first anchor: g_fall_light. The card then
    //   wears it, each layer's pixels by where they were before they fell: the shadows
    //   and the occlusion the bridge had stay on it (the user found the shadows
    //   vanishing suddenly odd).
    // - As a card: one tint for what the record does not cover (the rubble's sprites, a
    //   state loaded after the switch): the ground's tops within 48 px of the join's
    //   middle, lit sum over unlit.
    static std::vector<std::uint8_t> fall_before;
    int fb[4] = {0, 0, -1, -1};
    g_fallcard_tint[0] = g_fallcard_tint[1] = g_fallcard_tint[2] = 1.0f;
    if (!sp.fall_ready) g_fall_light.ok = false;
    if (sp.fall_ready) {
        fb[0] = std::max(0, sp.fall_fx[0] - 300); fb[2] = std::min(w - 1, sp.fall_fx[0] + 140);
        fb[1] = std::max(0, sp.fall_fy[0] - 80); fb[3] = std::min(h - 1, sp.fall_fy[0] + 140);
        if (fb[2] >= fb[0] && fb[3] >= fb[1]) {
            fall_before.clear();
            for (int y = fb[1]; y <= fb[3]; ++y)
                fall_before.insert(fall_before.end(), scene + (static_cast<std::size_t>(y) * w + fb[0]) * 3,
                                   scene + (static_cast<std::size_t>(y) * w + fb[2] + 1) * 3);
        }
    }
    struct FallTint {
        const WarpFrame& wf; const int* fb; const std::vector<std::uint8_t>& before;
        ~FallTint() {
            if (fb[2] < fb[0] || fb[3] < fb[1]) return;
            const kol::ScenePixels& sp = *wf.sp;
            const int bw = fb[2] - fb[0] + 1, bh = fb[3] - fb[1] + 1;
            const int mx = (sp.fall_fx[0] + sp.fall_fx[1]) / 2, my = (sp.fall_fy[0] + sp.fall_fy[1]) / 2;
            const bool record = !sp.fall_card;
            if (record) {
                g_fall_light.ok = true;
                g_fall_light.x0 = fb[0] - sp.fall_fx[0]; g_fall_light.y0 = fb[1] - sp.fall_fy[0];
                g_fall_light.w = bw; g_fall_light.h = bh;
                g_fall_light.mul.assign(static_cast<std::size_t>(bw) * bh * 3, 0);
                g_fall_light.v0[0] = g_fall_light.v0[1] = -1;
                g_fall_light.on = 0;
            }
            // (Tops only for the tint: with the walls round the place the card came out
            // darker than the floor it lies level with; with only the brighter half of
            // the tops, out of the towers' shade that the bridge stood in, far too bright.)
            double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            for (int y = fb[1]; y <= fb[3]; ++y)
                for (int x = fb[0]; x <= fb[2]; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * wf.w + x;
                    if (wf.kind[k] == kol::kSceneNone || wf.iface[k]) continue;
                    const std::size_t bi = (static_cast<std::size_t>(y - fb[1]) * bw + (x - fb[0])) * 3;
                    const std::uint8_t* p = &before[bi];
                    // (Not where something is lifted off the scene, a wall or a prop: the
                    // scene there is the ground under it, with that ground's light. The
                    // tower's foot beside the join left a dark rectangle on the card.)
                    if (record && !(wf.lift && wf.lift[k])) {
                        const int us = p[0] + p[1] + p[2], ls = wf.scene[k * 3] + wf.scene[k * 3 + 1] + wf.scene[k * 3 + 2];
                        for (int c = 0; c < 3; ++c) {
                            // (A dark channel's own ratio is noise: the pixel's whole.)
                            const float r = p[c] >= 24 ? static_cast<float>(wf.scene[k * 3 + c]) / p[c]
                                                       : us > 0 ? static_cast<float>(ls) / us : 1.0f;
                            g_fall_light.mul[bi + c] = static_cast<std::uint8_t>(std::clamp(r * 128.0f + 0.5f, 1.0f, 255.0f));
                        }
                    }
                    if (wf.kind[k] != kol::kSceneFlat || std::abs(x - mx) > 48 || std::abs(y - my) > 48) continue;
                    for (int c = 0; c < 3; ++c) { b[c] += p[c]; a[c] += wf.scene[k * 3 + c]; }
                }
            for (int c = 0; c < 3; ++c)
                if (b[c] > 1000.0) g_fallcard_tint[c] = static_cast<float>(std::min(1.5, std::max(0.2, a[c] / b[c])));
        }
    } fall_tint{wf, fb, fall_before};
    static std::vector<std::uint8_t> unwhite;   // the scene before its light (under_white)
    if (sp.scene_white > 0.0f) unwhite.assign(scene, scene + pixels * 3);
    sea_foam_hide(sp, scene, pixels);
    const std::uint8_t* unlit = foam_unlit(sp, scene, pixels);
    // A door painted on its wall (ScenePixels::wall_px): lit as the map, as
    // the lifted pixels' ground is.
    const std::uint8_t* lit_as_map = lift;
    static std::vector<std::uint8_t> wall_or_lift;
    if (sp.wall_px) {
        wall_or_lift.assign(sp.wall_px, sp.wall_px + pixels);
        if (lift)
            for (std::size_t k = 0; k < pixels; ++k) wall_or_lift[k] |= lift[k] ? 1u : 0u;
        lit_as_map = wall_or_lift.data();
    }
    apply_lighting(sp, layers, lit_as_map, scene, pixels, true);
    sea_foam(sp, scene, pixels);
    magic_circle(sp, scene, w, h);
    wt.lap("warp.water");

    {
        water_effect(sp, scene, w, h, &wf.cam, unlit);
        falls_effect(sp, scene, w, h);
        ice_effect(sp, scene, w, h, &wf.cam);
        wt.lap("warp.wet");
        wet_effect(sp, scene, w, h, &wf.cam, unlit);
    }
    under_white_all(sp, scene, unwhite, pixels, sp.scene_white);
    field_all(sp, scene, pixels, false);
}

// Special Effects: a spell's closing circle bends the picture along its rim, a glass
// ring closing on the caster (asked 2026-11-16 for it to feel like the whole environment
// warps toward the caster; the user's pick A of four stills, shots/spell_circle/
// warp_mock_*.png). Within kBand of the rim a pixel shows the scene from further out,
// by up to kPull of its distance, (1 - t^2)^2 across the band; the scene's own lit
// pixels, bilinear. The cards (units) and the interface are not the scene's and stay
// straight. KOL_NO_RING_LENS=1 off; KOL_RING_LENS=<pull>,<band> to try others.
void ring_lens(const kol::ScenePixels& sp, std::uint8_t* scene, int w, int h) {
    static const bool off = std::getenv("KOL_NO_RING_LENS") != nullptr;
    if (!sp.ring_on || off) return;
    static float kPull = 0.22f, kBand = 16.0f;
    static const bool env = [] {
        if (const char* e = std::getenv("KOL_RING_LENS")) std::sscanf(e, "%f,%f", &kPull, &kBand);
        return true;
    }();
    (void)env;
    const float cx = sp.ring[0], cy = sp.ring[1], r = sp.ring[2];
    if (r < 2.0f) return;
    const float pull = kPull * std::min(1.0f, r / 24.0f);   // eased out as it shuts
    const int y0 = std::max(0, static_cast<int>(cy - r - kBand)), y1 = std::min(h - 1, static_cast<int>(cy + r + kBand) + 1);
    const int x0 = std::max(0, static_cast<int>(cx - r - kBand)), x1 = std::min(w - 1, static_cast<int>(cx + r + kBand) + 1);
    if (y0 > y1 || x0 > x1) return;
    static std::vector<std::uint8_t> src;
    src.assign(scene, scene + static_cast<std::size_t>(w) * h * 3);
    const float in2 = std::max(0.0f, r - kBand) * std::max(0.0f, r - kBand), out2 = (r + kBand) * (r + kBand);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 <= in2 || d2 >= out2) continue;
            const float t = (std::sqrt(d2) - r) / kBand, q = 1.0f - t * t, k = pull * q * q;
            const float u = std::min(w - 1.001f, std::max(0.0f, x + dx * k)), v = std::min(h - 1.001f, std::max(0.0f, y + dy * k));
            const int ui = static_cast<int>(u), vi = static_cast<int>(v);
            const float fu = u - ui, fv = v - vi;
            const std::uint8_t* a = &src[(static_cast<std::size_t>(vi) * w + ui) * 3];
            const std::uint8_t* b = a + static_cast<std::size_t>(w) * 3;
            std::uint8_t* o = scene + (static_cast<std::size_t>(y) * w + x) * 3;
            for (int ch = 0; ch < 3; ++ch)
                o[ch] = static_cast<std::uint8_t>((a[ch] * (1 - fu) + a[ch + 3] * fu) * (1 - fv) + (b[ch] * (1 - fu) + b[ch + 3] * fu) * fv + 0.5f);
        }
    }
}

// The map's cut edge faded, and the mesh's debug paint (KOL_PERSP_KIND).
void warp_edge_paint(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    std::uint8_t* const scene = wf.scene;
    const int w = wf.w;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const iface = wf.iface;
    const std::int32_t* const xz = wf.xz;
    const std::uint8_t* const kind = wf.kind;
    const std::uint8_t* const floating = wf.floating;
    const std::uint8_t* const edge_rows = wf.edge_rows;
    // The map's cut edge faded toward the backdrop's colour on its row: the
    // pixels the mesh places (a unit's place holds the ground under it now).
    if (sp.edge_fade)
        for (std::size_t k = 0; k < pixels; ++k) {
            const unsigned f = sp.edge_fade[k];
            if (f >= 255u || kind[k] == kol::kSceneNone || (iface[k] && !faded_iface(sp, wf.layers, floating, k))) continue;
            edge_fade_px(scene + k * 3, &edge_rows[(k / static_cast<std::size_t>(w)) * 3], f);
        }

    // KOL_PERSP_KIND=1, or BACK menu "Debug: perspective mesh", paints the
    // scene by what the mesh thinks each pixel
    // is, before it is warped, so the geometry shows under the stick:
    // - green: ground lying flat; red: upright (walls, props, units' places);
    // - light and dark bands alternate every 8 units of depth (two rows of
    //   flat ground), so on level ground they must run parallel;
    // - blue: an overlay; blue added: the floating hand (magenta on a wall),
    //   and half blue: other interface (purple on a wall);
    // - black: nothing placed (the backdrop shows there).
    // Units drawn from their own sprites, the flat HUD and the moved hand keep
    // their colours. Contours bending or bunching on level ground found the
    // sheared ice (states/CK_chevrons_ice.state) and the hand's ground sheet
    // (states/CM_hand_distorts.state).
    paint_mesh_kind(kind, xz, iface, floating, scene, pixels);
    ring_lens(sp, scene, w, wf.h);
}

// 3. Flat: the HUD (BG2|BG3 and sprites over them) and interface sprites.
// Then the spell's card (3a), the floating hand moved (3b) and Tile Info's
// readout (3c).
void warp_flat(WarpFrame& wf, gbarecomp::PresentWarpMesh* mesh) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const layers = wf.layers;
    const int w = wf.w;
    const int view_x = wf.view_x, view_y = wf.view_y, view_w = wf.view_w, view_h = wf.view_h;
    const std::size_t view_pixels = wf.view_pixels;
    const std::uint8_t* const floating = wf.floating;
    const int fb_x0 = wf.fb_x0, fb_x1 = wf.fb_x1, fb_y0 = wf.fb_y0, fb_y1 = wf.fb_y1;
    const WarpCam& cam = wf.cam;
    flat_mask(sp, layers, floating, w, view_x, view_y, view_w, view_h);
    // 3a. A spell's effect as one card on the unit under it (kol_focus,
    // ScenePixels::spell_card): its flat pixels go up and grow with the warp
    // as an upright thing at that unit's feet does -- the floating hand's
    // rule, scaled too. Drawn flat, Summon Darkness's circle and column kept
    // their size and place while the camera zoomed onto the target.
    // KOL_PERSP_SPELL_FLAT=1 leaves it flat.
    static const bool spell_flat = std::getenv("KOL_PERSP_SPELL_FLAT") != nullptr;
    mesh->card = nullptr;
    if (sp.spell_card && !spell_flat) {
        const float d = static_cast<float>(sp.spell_xz);
        const float y = static_cast<float>(sp.spell_y);
        const float s = cam.scale(d);
        const float fx = static_cast<float>(sp.spell_fx), fy = static_cast<float>(sp.spell_fy);
        float t[2];
        cam.upright(d, y, s, fx, fy, t);
        cam.zoomed(t);
        g_card.assign(view_pixels, 0);
        bool any = false;
        // A sprite's pixel is 2: drawn from the frame whatever the scene
        // shows there. The scene holds the sprites too, so as 1 (a pixel
        // the effect's layer adds nothing to where frame and scene agree)
        // every interface sprite on the card was left out: the heal's
        // number over its unit (states/thor/0081, the user reported the numbers
        // over the head vanishing much too quickly; it showed only once the
        // camera had let go), Crag Crush's rocks, dust and number (IF).
        // KOL_SPELL_CARD_SPRITES_AS_BG=1: as before.
        static const bool sprites_as_bg = std::getenv("KOL_SPELL_CARD_SPRITES_AS_BG") != nullptr;
        for (std::size_t i = 0; i < view_pixels; ++i) {
            if (!g_flat[i]) continue;
            const std::size_t k = (i / view_w + view_y) * static_cast<std::size_t>(w) + i % view_w + view_x;
            g_card[i] = !sprites_as_bg && (layers[k] & 0x0Fu) == kol::kLayerObj ? 2 : 1;
            g_flat[i] = 0;
            any = true;
        }
        if (any) {
            mesh->card = g_card.data();
            mesh->card_from[0] = fx - view_x; mesh->card_from[1] = fy - view_y;
            mesh->card_to[0] = t[0] - view_x;   mesh->card_to[1] = t[1] - view_y;
            mesh->card_scale = s * cam.zoom;
        }
    }

    // 3a'. A piece of the map on the interface layers (ScenePixels::fall_card): one card
    // at its world place, in the ground's light. The HUD layers past the native screen
    // are not drawn at all: there the game's tilemap wraps (a row of the bridge's tiles
    // ran through the wide view's margins).
    mesh->card_tint[0] = mesh->card_tint[1] = mesh->card_tint[2] = 1.0f;
    mesh->card_affine_on = false;
    mesh->card_col_x = nullptr;
    mesh->card_mul = nullptr;
    if (sp.fall_card && !mesh->card) {
        // Its three anchors, where the warp puts them.
        float f[3][2], t[3][2], s = 0.0f;
        for (int e = 0; e < 3; ++e) {
            const float d = static_cast<float>(sp.fall_xz[e]), se = cam.scale(d);
            f[e][0] = static_cast<float>(sp.fall_fx[e]); f[e][1] = static_cast<float>(sp.fall_fy[e]);
            cam.upright(d, static_cast<float>(sp.fall_y), se, f[e][0], f[e][1], t[e]);
            cam.zoomed(t[e]);
            s += se / 3.0f;
        }
        const float fx = f[0][0], fy = f[0][1];
        g_card.assign(view_pixels, 0);
        bool any = false;
        const int nx0 = sp.ox - view_x, ny0 = sp.oy - view_y;
        // Each pixel's light from the record of the piece as the map's (g_fall_light),
        // by where the pixel was before its layer fell; the one tint elsewhere.
        // KOL_FALL_CARD_ONE_TINT=1: the one tint everywhere.
        static const bool one_tint = std::getenv("KOL_FALL_CARD_ONE_TINT") != nullptr;
        const bool use_mul = g_fall_light.ok && !one_tint;
        int fallen[2] = {0, 0};
        std::uint8_t flat_mul[3];
        // The scene's fade too (ScenePixels::fx_dark, BLDY on BG0, BG1, the sprites and the
        // backdrop: 5/16 through map 88's collapse). The interface layers are no target
        // of it, so on the cartridge itself the bridge turns brighter the frame it
        // moves onto them; here it keeps the fade the map has.
        const float fade = 1.0f - std::clamp(sp.fx_dark, 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) flat_mul[c] = static_cast<std::uint8_t>(std::clamp(g_fallcard_tint[c] * fade * 128.0f + 0.5f, 1.0f, 255.0f));
        if (use_mul) {
            g_card_mul.assign(view_pixels * 3, 128);
            // A layer coming on is the game laying the piece out anew, on both (map 88:
            // BG2 alone at offset 440, then BG2 at 400 with BG3 at 432, the art in the
            // same place): the offsets then are the piece at rest again.
            if (sp.fall_on & ~g_fall_light.on) g_fall_light.v0[0] = g_fall_light.v0[1] = -1;
            g_fall_light.on = sp.fall_on;
            for (int l = 0; l < 2; ++l) {
                if (g_fall_light.v0[l] < 0) g_fall_light.v0[l] = sp.fall_v[l];
                fallen[l] = ((g_fall_light.v0[l] - sp.fall_v[l] + 256) & 511) - 256;   // a lower offset = further down
            }
        }
        int sky_row = -1, sky_px = -1;
        std::uint8_t sky_rgb[3] = {0, 0, 0};
        for (std::size_t i = 0; i < view_pixels; ++i) {
            if (!g_flat[i]) continue;
            const int px = static_cast<int>(i % view_w), py = static_cast<int>(i / view_w);
            const std::size_t k = static_cast<std::size_t>(py + view_y) * w + px + view_x;
            const unsigned top = layers[k] & 0x0Fu;
            g_flat[i] = 0;
            // (Sky behind it: its row's sky as the view's backdrop has it beside the
            // card. Under these layers the scene holds the game's sky without the
            // scene's fade, a brighter patch of the card's shape once it moved.)
            if (wf.kind[k] == kol::kSceneNone) {
                if (sky_row != py) {
                    sky_row = py;
                    sky_px = -1;
                    for (int x = 0; x < view_w && sky_px < 0; ++x) {
                        const std::size_t j = static_cast<std::size_t>(py) * view_w + x;
                        if (!g_flat[j] && !g_card[j] && wf.kind[static_cast<std::size_t>(py + view_y) * w + x + view_x] == kol::kSceneNone)
                            sky_px = x;
                    }
                    if (sky_px >= 0) std::copy_n(&g_backdrop[(static_cast<std::size_t>(py) * view_w + sky_px) * 3], 3, sky_rgb);
                }
                if (sky_px >= 0) {
                    std::copy_n(sky_rgb, 3, &g_backdrop[i * 3]);
                    std::copy_n(sky_rgb, 3, wf.scene + k * 3);
                }
            }
            if (use_mul) {
                std::uint8_t* m = &g_card_mul[i * 3];
                for (int c = 0; c < 3; ++c) m[c] = flat_mul[c];
                if (top == 2u || top == 3u) {
                    const int lx = px + view_x - sp.fall_fx[0] - g_fall_light.x0;
                    const int ly = py + view_y - fallen[top - 2u] - sp.fall_fy[0] - g_fall_light.y0;
                    if (lx >= 0 && lx < g_fall_light.w && ly >= 0 && ly < g_fall_light.h) {
                        const std::uint8_t* r = &g_fall_light.mul[(static_cast<std::size_t>(ly) * g_fall_light.w + lx) * 3];
                        if (r[0])
                            for (int c = 0; c < 3; ++c) m[c] = static_cast<std::uint8_t>(std::max(1.0f, r[c] * fade + 0.5f));
                    }
                }
            }
            if (top != 2u && top != 3u) { g_card[i] = 2; any = true; continue; }
            // The game drew its layers for its own screen alone: their cut at that
            // screen's edge, in the middle of the wide view, fades over kFade px.
            constexpr int kFade = 12;
            const int edge = std::min(std::min(px - nx0, nx0 + 239 - px), std::min(py - ny0, ny0 + 159 - py));
            if (edge < 0) continue;
            g_card[i] = edge >= kFade ? 2 : static_cast<std::uint8_t>(std::max(16, 255 * (edge + 1) / (kFade + 1)));
            any = true;
        }
        if (any) {
            mesh->card = g_card.data();
            mesh->card_from[0] = fx - view_x; mesh->card_from[1] = fy - view_y;
            mesh->card_to[0] = t[0][0] - view_x;   mesh->card_to[1] = t[0][1] - view_y;
            mesh->card_scale = s * cam.zoom;
            // The affine map taking the three anchors to their places: T = M F, with
            // F and T the two edges from the first anchor as columns.
            const float ax = f[1][0] - f[0][0], ay = f[1][1] - f[0][1], bx = f[2][0] - f[0][0], by = f[2][1] - f[0][1];
            const float det = ax * by - bx * ay;
            if (std::abs(det) >= 64.0f) {
                const float px = t[1][0] - t[0][0], py = t[1][1] - t[0][1], qx = t[2][0] - t[0][0], qy = t[2][1] - t[0][1];
                mesh->card_affine_on = true;
                mesh->card_affine[0] = (px * by - qx * ay) / det;
                mesh->card_affine[1] = (qx * ax - px * bx) / det;
                mesh->card_affine[2] = (py * by - qy * ay) / det;
                mesh->card_affine[3] = (qy * ax - py * bx) / det;
                // Verticals stay vertical (the user asked for vertical lines
                // to stay vertical): in the plane a pixel's column moves with its row (its
                // depth), which leans the parapets' edges, a lot. Each column goes
                // instead where the plane puts ONE point of it, whatever the row: its
                // point on the join line (so the join stays on what the piece is joined
                // to, exactly), and past the join's first end its point on the piece's
                // edge from there, along the piece (so its length follows the map).
                // The first try took the piece's middle line for every column: the
                // join's ends then slid along the terrace by the plane's shear times
                // their 40 rows from that line (the bridge no longer looked anchored to the
                // castle). KOL_FALL_CARD_PLANE=1: the plane's own columns.
                static const bool plane = std::getenv("KOL_FALL_CARD_PLANE") != nullptr;
                if (!plane && std::abs(ax) >= 8.0f && std::abs(bx) >= 8.0f) {
                    static std::vector<float> col_x;
                    col_x.resize(static_cast<std::size_t>(view_w) + 1);
                    const float kj = ay / ax, kb = by / bx;
                    const bool join_right = ax > 0.0f;   // the join runs right of the first anchor
                    for (int c = 0; c <= view_w; ++c) {
                        const float dx = static_cast<float>(c) - mesh->card_from[0];
                        const bool on_join = join_right ? dx >= 0.0f : dx <= 0.0f;
                        const float dy = dx * (on_join ? kj : kb);
                        col_x[c] = mesh->card_to[0] + mesh->card_affine[0] * dx + mesh->card_affine[1] * dy;
                    }
                    mesh->card_col_x = col_x.data();
                }
            }
            for (int c = 0; c < 3; ++c) mesh->card_tint[c] = g_fallcard_tint[c] * fade;
            mesh->card_mul = use_mul ? g_card_mul.data() : nullptr;
        }
    }

    // 3b. The floating hand, flat and whole, moved as the point at its middle
    // moves: it stands at the cursor's place like a unit, so that point goes
    // where an upright thing there goes. It keeps its size, as interface does.
    const bool moved = !g_float_px.empty() || sp.tile_text;
    if (moved) {
        g_moved.assign(view_pixels, 0);
        g_moved_rgb.resize(view_pixels * 3);
    }
    if (!g_float_px.empty()) {
        const float d = static_cast<float>(sp.cursor_xz);
        const float y = static_cast<float>(sp.cursor_y);
        const float s = cam.scale(d);
        const float ic = 0.5f * static_cast<float>(fb_x0 + fb_x1 + 1);
        const float jc = 0.5f * static_cast<float>(fb_y0 + fb_y1 + 1);
        float m[2];
        cam.upright(d, y, s, ic, jc, m);
        cam.zoomed(m);
        const int dx = static_cast<int>(std::lround(m[0] - ic)) - view_x;
        const int dy = static_cast<int>(std::lround(m[1] - jc)) - view_y;
        for (const FloatPx& f : g_float_px) {
            const int qx = f.x + dx, qy = f.y + dy;
            if (qx < 0 || qy < 0 || qx >= view_w || qy >= view_h) continue;
            const std::size_t q = static_cast<std::size_t>(qy) * view_w + qx;
            g_moved[q] = 1;
            g_moved_rgb[q * 3] = f.r;
            g_moved_rgb[q * 3 + 1] = f.g;
            g_moved_rgb[q * 3 + 2] = f.b;
        }
    }
    // 3c. Debug > Tile Info's readout (draw_tile_text).
    if (sp.tile_text) draw_tile_text(sp.tile_text, view_w, view_h);
    wf.moved = moved;
}

// 4. The mesh: a vertex per pixel corner.
// The corners placed, the surface cuts, the strips' own corners (pass 1).
void warp_corners(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const lift = wf.lift;
    const int view_x = wf.view_x, view_w = wf.view_w;
    const std::int32_t* const xz = wf.xz;
    const std::int32_t* const yy = wf.yy;
    const std::uint8_t* const kind = wf.kind;
    const WarpCam cam = wf.cam;
    const bool check = wf.check;
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    const int vw = w + 1;
    const int vh = h + 1;
    const std::size_t verts = static_cast<std::size_t>(vw) * vh;
    // Each atlas square has its own grid of (kSlot + 1)^2 corners after the
    // scene's, its uv fixed in the atlas under the scene (v from h on), so
    // the uv never change from frame to frame; only the squares in use are
    // placed.
    const int slots = lift ? sp.atlas_per_row * (sp.atlas_h / kSlot) : 0;
    constexpr int kSlotCorners = (kSlot + 1) * (kSlot + 1);
    const std::size_t all_verts = verts + static_cast<std::size_t>(slots) * kSlotCorners;
    g_xy.resize(all_verts * 2);
    if (g_uv_w != w || g_uv_h != h || g_uv_atlas != slots) {
        ++g_uv_gen;
        g_uv.resize(all_verts * 2);
        for (int j = 0; j < vh; ++j)
            for (int i = 0; i < vw; ++i) {
                g_uv[(static_cast<std::size_t>(j) * vw + i) * 2] = static_cast<float>(i);
                g_uv[(static_cast<std::size_t>(j) * vw + i) * 2 + 1] = static_cast<float>(j);
            }
        for (int sl = 0; sl < slots; ++sl) {
            const auto [ax0, ay0] = kol::slot_origin(sl, sp.atlas_per_row);
            for (int j = 0; j <= kSlot; ++j)
                for (int i = 0; i <= kSlot; ++i) {
                    const std::size_t v = verts + static_cast<std::size_t>(sl) * kSlotCorners +
                                          static_cast<std::size_t>(j) * (kSlot + 1) + i;
                    g_uv[v * 2] = static_cast<float>(ax0 + i);
                    g_uv[v * 2 + 1] = static_cast<float>(h + ay0 + j);
                }
        }
        g_uv_w = w;
        g_uv_h = h;
        g_uv_atlas = slots;
    }
    g_used.assign(verts, 0);
    // cut[j * vw + i]: a surface boundary at corner i of pixel row j.
    g_cut.assign(static_cast<std::size_t>(vh) * vw, 0);
    // The frontmost pixel touching a corner places it: nearer, then higher;
    // on a tie the first of top-left, top-right, bottom-left, bottom-right.
    // Taken in two steps: per pixel row, the better of the two pixels either
    // side of each corner column (left wins a tie), then the better of the
    // rows above and below (top wins). -1: none placed.
    // A pixel lifted off for a prop (KOL_PERSP_LIFT_PROPS) shows what the
    // prop hid, so it is behind anything beside it that is not: the ground
    // behind the castle's statue, a hair "nearer" by row (548 against the
    // card's 544), took the vase's top corners and dropped its top edge a
    // row below the body's card, a band of wall between them.
    const std::uint8_t* const prop_lift = g_prop_lift.empty() ? nullptr : g_prop_lift.data();
    auto front = [xz, yy, kind, prop_lift](int a, int b) {   // b only if strictly in front of a
        if (a < 0) return b;
        if (b < 0) return a;
        const bool oa = kind[a] == kol::kSceneOverlay, ob = kind[b] == kol::kSceneOverlay;
        if (oa != ob) return ob ? b : a;   // an overlay is in front of anything
        if (prop_lift && prop_lift[a] != prop_lift[b]) return prop_lift[a] ? b : a;
        return (xz[b] > xz[a] || (xz[b] == xz[a] && yy[b] > yy[a])) ? b : a;
    };
    auto pairs_of_row = [&](int py, int* pair) {
        if (py < 0 || py >= h) {
            std::fill(pair, pair + vw, -1);
            return;
        }
        const int o = py * w;
        int left = -1;
        for (int i = 0; i < w; ++i) {
            const int right = kind[o + i] != kol::kSceneNone ? o + i : -1;
            pair[i] = front(left, right);
            left = right;
        }
        pair[w] = left;
    };
    // The row functions copy what they use into locals: a byte store (used,
    // keep) may alias anything reached through a reference, which would be
    // reloaded after every one.
    // Corner (i, j) as pixel `best` places it (WarpCam::corner).
    auto corner_at = [w, cam, kind, xz, yy](int best, int i, int j, float* v) {
        cam.corner(w, xz, yy, kind, best, i, j, v);
    };
    auto place_corners = [&, vw](int j, const int* above, const int* below) {
        float* const xy = g_xy.data() + static_cast<std::size_t>(j) * vw * 2;
        std::uint8_t* const used_row = g_used.data() + static_cast<std::size_t>(j) * vw;
        for (int i = 0; i < vw; ++i) {
            const int best = front(above[i], below[i]);
            float* v = xy + static_cast<std::size_t>(i) * 2;
            if (best < 0) {
                v[0] = static_cast<float>(i);
                v[1] = static_cast<float>(j);
                continue;
            }
            used_row[i] = 1;
            corner_at(best, i, j, v);
        }
    };
    // Surface boundaries, per pixel row: where a row's pixels change surface
    // (and so draw order) or meet the sky, the merging below must cut.
    const SameSurface same_surface{kind, xz, yy};
    auto cut_row = [&, w, vw, kind, prop_lift](int py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        std::uint8_t* cut = g_cut.data() + static_cast<std::size_t>(py) * vw;
        for (int i = 0; i <= w; ++i) {
            const bool left = i > 0 && kind[o + i - 1] != kol::kSceneNone;
            const bool right = i < w && kind[o + i] != kol::kSceneNone;
            if (left != right || (left && right && !same_surface(o + i - 1, o + i)))
                cut[i] = 1;
            // The ground a prop hid is its own run (drawn before the prop's
            // card, see the buckets).
            else if (prop_lift && left && right && prop_lift[o + i - 1] != prop_lift[o + i])
                cut[i] = 1;
        }
    };
    // Strips behind end at their own edge. A corner row two surfaces share
    // (the deck's last row under the near rail's top, ground under a cliff's
    // edge) is placed by the one in front, and the strip of the one behind
    // is stretched to it. With the camera low the front one's corner lies
    // inside it on screen (the rail's top above where the deck's own edge
    // is): the stretched strip folded back over the rows before it and,
    // drawn after them, painted saw-teeth over them (map 58's near rail,
    // states/JL with the stick down; the user, 2026-10-25, reported a stretch behind
    // something else showing on top of it).
    // There the strip behind now ends at its own corner, under the front
    // surface drawn after it; where its own corner falls short of the shared
    // one (a gap: the camera high) it still stretches to fill it.
    // KOL_PERSP_STRETCH_OVER=1: always the shared corner, as before.
    // (Found from the same two rows' pairs as the corners, in the same pass:
    // a second pass over every row worked the pairs out again.)
    static const bool stretch_over = std::getenv("KOL_PERSP_STRETCH_OVER") != nullptr;
    g_own_px.assign(stretch_over ? 0 : verts, -1);
    if (!stretch_over) {
        g_own_xy.resize(verts * 2);
        g_own_dir.resize(verts);
    }
    // And sideways: where two surfaces meet along a column (a doorway's back and the
    // wall beside its opening), the one behind was stretched to the front one's corner
    // too, so when the camera uncovered more of it the filled gap ran from its own rows
    // to the front edge's, across the two depths: a door's crossbar turned up toward the
    // wall's edge (map 36's door, the user's drawing, asking for the
    // stretching to follow the view angle). There the strip behind gets a corner of its
    // own (g_own_dir 0: for the rows above and below alike): its own where that reaches
    // under the front surface, else the front corner's column on the behind surface's own
    // line (its next corner along the row gives the direction), so the gap is filled
    // along the surface behind. KOL_PERSP_SIDE_STRETCH_SHARED=1: as before.
    static const bool side_shared = std::getenv("KOL_PERSP_SIDE_STRETCH_SHARED") != nullptr;
    auto side_corner = [&, vw, w, h](int i, int j) {
        if (side_shared || i < 1 || i >= w || j < 1 || j >= h) return;
        const int ul = (j - 1) * w + i - 1, ur = ul + 1, dl = j * w + i - 1, dr = dl + 1;
        for (const int p : {ul, ur, dl, dr})
            if (kind[p] == kol::kSceneNone || kind[p] == kol::kSceneOverlay) return;
        if (same_surface(static_cast<std::size_t>(dl), static_cast<std::size_t>(dr))) return;
        const bool left_behind = front(dl, dr) == dr;
        const int behind = left_behind ? dl : dr, behind_up = left_behind ? ul : ur, other_up = left_behind ? ur : ul;
        if (prop_lift && (prop_lift[dl] || prop_lift[dr])) return;
        if (!same_surface(static_cast<std::size_t>(behind), static_cast<std::size_t>(behind_up)) ||
            same_surface(static_cast<std::size_t>(behind_up), static_cast<std::size_t>(other_up)))
            return;
        const std::size_t c = static_cast<std::size_t>(j) * vw + i;
        float own[2], next[2];
        corner_at(behind, i, j, own);
        const float sx = g_xy[c * 2];
        const float gap = left_behind ? sx - own[0] : own[0] - sx;
        if (gap > 0.0f) {
            // The direction: on a wall, along its own level line. The picture's rows
            // climb a wall as they cross it (a level line drops a row every two
            // columns on a +Z face, rises one on a +X face), so the pixel two columns
            // in and a row up or down at the same height gives it; along the picture's
            // row the fill ran level on screen and a level line in the art (the door's
            // crossbar) lost its slope there (the user asked for it to step down in 2x1
            // pixel shifts). On the ground, and where no such pixel is, along the row.
            float dir[2] = {0.0f, 0.0f};
            bool have = false;
            if (kind[behind] == kol::kSceneUpright) {
                const int side = left_behind ? -1 : 1;
                const int bc = behind % w, br = behind / w;
                for (const int dr2 : {-1, 1}) {
                    const int qc = bc + 2 * side, qr = br + (left_behind ? dr2 : -dr2);
                    if (qc < 0 || qc >= w || qr < 0 || qr >= h) continue;
                    const int q = qr * w + qc;
                    if (kind[q] != kol::kSceneUpright || std::abs(yy[q] - yy[behind]) > 1 ||
                        !same_surface(static_cast<std::size_t>(q), static_cast<std::size_t>(behind)))
                        continue;
                    corner_at(q, i + 2 * side, j + (qr - br), next);
                    dir[0] = left_behind ? own[0] - next[0] : next[0] - own[0];
                    dir[1] = left_behind ? own[1] - next[1] : next[1] - own[1];
                    have = true;
                    break;
                }
            }
            if (!have) {
                corner_at(behind, i + (left_behind ? 1 : -1), j, next);
                dir[0] = left_behind ? next[0] - own[0] : own[0] - next[0];
                dir[1] = left_behind ? next[1] - own[1] : own[1] - next[1];
            }
            // (dir: toward +x. Left behind: the fill runs +x; right behind: -x.)
            if (dir[0] < 0.05f || gap > 48.0f * dir[0]) return;   // (edge on, or far: the shared corner)
            const float t = gap / dir[0];
            own[1] += (left_behind ? dir[1] : -dir[1]) * t;
            own[0] = sx;
        }
        g_own_xy[c * 2] = own[0];
        g_own_xy[c * 2 + 1] = own[1];
        g_own_px[c] = behind;
        g_own_dir[c] = 0;
    };
    auto own_corners = [&, vw](int j, const int* above, const int* below) {
        for (int i = 0; i < vw; ++i) {
            const int a = above[i], b = below[i];
            if (a >= 0 && b >= 0 && same_surface(static_cast<std::size_t>(a), static_cast<std::size_t>(b))) {
                side_corner(i, j);
                continue;
            }
            if (a < 0 || b < 0 || kind[a] == kol::kSceneOverlay || kind[b] == kol::kSceneOverlay ||
                same_surface(static_cast<std::size_t>(a), static_cast<std::size_t>(b)))
                continue;
            const int behind = front(a, b) == a ? b : a;
            const std::size_t c = static_cast<std::size_t>(j) * vw + i;
            corner_at(behind, i, j, &g_own_xy[c * 2]);
            g_own_px[c] = behind;
            g_own_dir[c] = behind == b ? 1 : -1;
        }
    };
    const int kChunkRows = chunk_rows();
    const int chunks = (vh + kChunkRows - 1) / kChunkRows;
    if (g_pairs.size() < static_cast<std::size_t>(chunks) * 2)
        g_pairs.resize(static_cast<std::size_t>(chunks) * 2);
    row_pool().run(vh, kChunkRows, [&](int chunk, int j0, int j1) {
        std::vector<int>& above = g_pairs[static_cast<std::size_t>(chunk) * 2];
        std::vector<int>& below = g_pairs[static_cast<std::size_t>(chunk) * 2 + 1];
        above.resize(static_cast<std::size_t>(vw));
        below.resize(static_cast<std::size_t>(vw));
        pairs_of_row(j0 - 1, above.data());
        for (int j = j0; j < j1; ++j) {
            pairs_of_row(j, below.data());
            place_corners(j, above.data(), below.data());
            if (!stretch_over) own_corners(j, above.data(), below.data());
            above.swap(below);
        }
        for (int j = j0; j < std::min(j1, h); ++j) cut_row(j);
    });

    if (kol::persp_hash_trace())
        std::fprintf(stderr, "kol-persp-hash f=%ld in: kind=%08x xz=%08x y=%08x scene=%08x | pass1: xy=%08x used=%08x cut=%08x\n",
                     g_trace_frame, kol::fnv_bytes(g_kind), kol::fnv_bytes(g_xz), kol::fnv_bytes(g_y),
                     kol::fnv_bytes(std::vector<std::uint8_t>(scene, scene + pixels * 3)),
                     kol::fnv_bytes(g_xy), kol::fnv_bytes(g_used), kol::fnv_bytes(g_cut));
    if (check) {
        // A left-out place must land wholly outside the view: every corner of
        // it on the far side of the view's edge it came from.
        const float left = static_cast<float>(view_x);
        const float right = static_cast<float>(view_x + view_w);
        for (int py = 0; py < h; ++py) {
            const int keep = g_check_rows[py];
            for (int px = 0; px < w; ++px) {
                const bool l = px < view_x - keep;
                const bool r = px >= view_x + view_w + keep;
                if ((!l && !r) || kind[static_cast<std::size_t>(py) * w + px] == kol::kSceneNone)
                    continue;
                for (int c = 0; c < 4; ++c) {
                    const std::size_t v = static_cast<std::size_t>(py + (c >> 1)) * vw + px + (c & 1);
                    const float clear = l ? left - g_xy[v * 2] : g_xy[v * 2] - right;
                    g_check_clear = std::min(g_check_clear, clear);
                    if (clear < 0.0f) ++g_check_bad;
                }
            }
        }
        if (++g_check_frames % 60 == 0)
            std::fprintf(stderr, "kol-persp: trim check frames=%ld corners_inside=%ld "
                                 "min_clearance=%.1f px yref=%.1f\n",
                         g_check_frames, g_check_bad, g_check_clear,
                         g_yref.load(std::memory_order_relaxed));
    }
    wf.vw = vw;
    wf.vh = vh;
    wf.verts = verts;
    wf.slots = slots;
    wf.all_verts = all_verts;
    wf.prop_lift = prop_lift;
}

// Merging. Each corner row is cut into straight pieces ONCE -- a piece
// runs as far as its corners stay within kTolerance of the chord -- and
// both strips of pixels that share that row use the same cuts, so the
// mesh has no T-junctions and no hairline gaps. Cuts are forced at the
// surface boundaries of the pixel rows above and below.
// (Pass 2, and the kept corners spread: pass 3.)
void warp_keep(WarpFrame& wf) {
    const int w = wf.w, h = wf.h, vw = wf.vw, vh = wf.vh;
    const int kChunkRows = chunk_rows();
    // A tenth of a frame pixel: under half a panel pixel at 4x.
    constexpr float kTolerance = 0.1f;
    g_keep.assign(static_cast<std::size_t>(vh) * vw, 0);
    auto keep_row = [w, h, vw](int j) {
        const float* v = g_xy.data() + static_cast<std::size_t>(j) * vw * 2;
        std::uint8_t* keep = g_keep.data() + static_cast<std::size_t>(j) * vw;
        const std::uint8_t* used_row = g_used.data() + static_cast<std::size_t>(j) * vw;
        // The cuts of the pixel rows either side of this corner row, and of
        // the rows beyond those. A surface's edge runs diagonally, so a
        // corner the next row places on the surface beyond (the nearer one
        // wins a shared corner) lies a column or two inside this row's
        // piece: with no corner here to meet it, the strip between the two
        // rows was zipped as a fan from far along the piece to that corner,
        // and sheared across its whole length -- ice beside a raised block
        // stretched and leaned under the stick (states/CK_chevrons_ice.state).
        auto cut_row_at = [h, vw](int r) -> const std::uint8_t* {
            return r >= 0 && r < h ? g_cut.data() + static_cast<std::size_t>(r) * vw : nullptr;
        };
        const std::uint8_t* cut_above = cut_row_at(j - 1);
        const std::uint8_t* cut_below = cut_row_at(j);
        const std::uint8_t* cut_above2 = cut_row_at(j - 2);
        const std::uint8_t* cut_below2 = cut_row_at(j + 1);
        auto used = [used_row, w](int, int i) { return i <= w && used_row[i] != 0; };
        auto forced = [cut_above, cut_below, cut_above2, cut_below2](int i) {
            return (cut_above && cut_above[i]) || (cut_below && cut_below[i]) ||
                   (cut_above2 && cut_above2[i]) || (cut_below2 && cut_below2[i]);
        };
        int start = -1;
        float lo[2], hi[2];
        // Whether corner i lies on a piece from `start` within the tolerance
        // (and the slopes that keep it so), as the loop below asks.
        auto fits_piece = [&](int i, float* nlo, float* nhi) {
            const float t = static_cast<float>(i - start);
            bool fits = true;
            for (int c = 0; c < 2; ++c) {
                const float d = v[i * 2 + c] - v[start * 2 + c];
                if (d / t < lo[c] || d / t > hi[c]) fits = false;
                nlo[c] = std::max(lo[c], (d - kTolerance) / t);
                nhi[c] = std::min(hi[c], (d + kTolerance) / t);
            }
            return fits;
        };
        for (int i = 0; i <= w; ++i) {
            if (!used(j, i)) { start = -1; continue; }
            if (start < 0 || forced(i)) {
                // A cut ends the piece, but the piece must still reach the
                // cut's corner, which the surface beyond may have placed: it
                // then ends at the corner before instead.
                float nlo[2], nhi[2];
                if (start >= 0 && i - 1 > start && !keep[i - 1] && !fits_piece(i, nlo, nhi))
                    keep[i - 1] = 1;
                keep[i] = 1;
                start = i;
                lo[0] = lo[1] = -1e30f;
                hi[0] = hi[1] = 1e30f;
                continue;
            }
            // Slopes from `start` that keep every corner so far within the
            // tolerance; the chord to i must be one of them.
            float nlo[2], nhi[2];
            const bool fits = fits_piece(i, nlo, nhi);
            if (fits) {
                for (int c = 0; c < 2; ++c) { lo[c] = nlo[c]; hi[c] = nhi[c]; }
                continue;
            }
            // Cut at the corner before, and start the next piece there.
            keep[i - 1] = 1;
            start = i - 1;
            const float d0 = v[i * 2] - v[start * 2];
            const float d1 = v[i * 2 + 1] - v[start * 2 + 1];
            lo[0] = d0 - kTolerance; hi[0] = d0 + kTolerance;
            lo[1] = d1 - kTolerance; hi[1] = d1 + kTolerance;
        }
        // The last used corner of every span ends a piece.
        for (int i = 0; i <= w; ++i)
            if (used(j, i) && (i == w || !used(j, i + 1))) keep[i] = 1;
    };
    row_pool().run(vh, kChunkRows, [&](int, int j0, int j1) {
        for (int j = j0; j < j1; ++j) keep_row(j);
    });
    if (kol::persp_hash_trace()) std::fprintf(stderr, "kol-persp-hash f=%ld pass2: keep=%08x\n", g_trace_frame, kol::fnv_bytes(g_keep));
    // A corner kept in one corner row is kept in the rows either side too
    // (where they place one), so both edges of every strip have a corner at
    // each column either edge keeps. Zipped with a corner on one edge only,
    // a strip fanned it to the far end of the other edge: a corner an upright
    // thing had pulled 6 px up (the half-transparent unit being placed, left
    // in the frame) made a wedge from the unit to the row's far end, and the
    // row's texture was painted over the rows above it, tapering away (DM,
    // placing a unit, stick down: the water beside the unit repeated in a
    // band). Every strip sharing a corner row still uses that row's one set
    // of corners, so there are no T-junctions.
    static const bool no_keep_spread = std::getenv("KOL_PERSP_NO_KEEP_SPREAD") != nullptr;
    if (!no_keep_spread) {
        g_keep_own.swap(g_keep);
        g_keep.assign(g_keep_own.size(), 0);
        row_pool().run(vh, kChunkRows, [&](int, int j0, int j1) {
            for (int j = j0; j < j1; ++j) {
                const std::size_t o = static_cast<std::size_t>(j) * vw;
                const std::uint8_t* own = g_keep_own.data() + o;
                const std::uint8_t* up = j > 0 ? own - vw : nullptr;
                const std::uint8_t* down = j + 1 < vh ? own + vw : nullptr;
                const std::uint8_t* used_row = g_used.data() + o;
                std::uint8_t* keep = g_keep.data() + o;
                for (int i = 0; i < vw; ++i)
                    keep[i] = own[i] || (used_row[i] && ((up && up[i]) || (down && down[i])));
            }
        });
    }

    if (kol::persp_hash_trace()) std::fprintf(stderr, "kol-persp-hash f=%ld pass3: keep=%08x\n", g_trace_frame, kol::fnv_bytes(g_keep));
}

// Runs: a pixel row's scene pixels between surface cuts, each one strip
// zipped between the kept corners above and below it.
void warp_runs(WarpFrame& wf) {
    const int w = wf.w, h = wf.h, vw = wf.vw;
    const std::int32_t* const xz = wf.xz;
    const std::uint8_t* const kind = wf.kind;
    g_quads.clear();
    int dmin = INT32_MAX, dmax = INT32_MIN;
    // A run goes in by its first pixel's depth, and one surface may climb
    // across depths a step at a time (SameSurface: 8 in depth a pixel): on
    // map 16 a row ran from the ground plane at the map's foot (676) up its
    // front cliff onto the slab's top and wall (772), all drawn at 676, so
    // the grass three cells behind (716) went over the slab (states/thor/
    // 0083 with the camera low, the user: nothing behind the terrain should
    // show). A run is split where it passes between flat and upright
    // with its depth moved more than kRunDrift from its piece's first pixel,
    // each piece in by its own; the split corner kept in both corner rows,
    // so the strips above and below share it (no T-junction). (Split at any
    // such drift, every slanted wall's rows were cut too: the extra corners
    // shifted wall textures by a fraction of a pixel at rest, 2500-7200
    // pixels a picture.) KOL_PERSP_RUN_WHOLE=1: whole runs, as before.
    static const bool whole_runs = std::getenv("KOL_PERSP_RUN_WHOLE") != nullptr;
    constexpr int kRunDrift = 16;
    const std::uint8_t* const run_split = wf.sp->run_split;
    static const bool no_model_split = std::getenv("KOL_MODEL_RUN_WHOLE") != nullptr;
    static const int run_drift_env = static_cast<int>(kol::env_num_or("KOL_MODEL_RUN_DRIFT", 0.0f));
    const int run_drift = run_drift_env > 0 ? run_drift_env : wf.sp->run_split_drift > 0 ? wf.sp->run_split_drift : 48;
    for (int py = 0; py < h; ++py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        const std::uint8_t* cut = &g_cut[static_cast<std::size_t>(py) * vw];
        int px = 0;
        while (px < w) {
            if (kind[o + px] == kol::kSceneNone) { ++px; continue; }
            int b = px + 1;
            while (b < w && !cut[b]) ++b;
            int s = px;
            if (!whole_runs)
                for (int i = px + 1; i < b; ++i) {
                    // (A model's own long runs too, on the maps that ask: kModelRunSplitMaps.)
                    const bool model_drift = run_split && !no_model_split && run_split[o + i] &&
                                             std::abs(xz[o + i] - xz[o + s]) > run_drift;
                    if (!model_drift && (kind[o + i] == kind[o + i - 1] || std::abs(xz[o + i] - xz[o + s]) <= kRunDrift)) continue;
                    g_quads.push_back(Quad{s, i, py});
                    dmin = std::min(dmin, xz[o + s]);
                    dmax = std::max(dmax, xz[o + s]);
                    g_keep[static_cast<std::size_t>(py) * vw + i] = 1;
                    g_keep[static_cast<std::size_t>(py + 1) * vw + i] = 1;
                    s = i;
                }
            g_quads.push_back(Quad{s, b, py});
            dmin = std::min(dmin, xz[o + s]);
            dmax = std::max(dmax, xz[o + s]);
            px = b;
        }
    }
    wf.dmin = dmin;
    wf.dmax = dmax;
}

// Each unit's sprite: its square's corners placed as an upright thing at
// the unit's place (as the unit's pixels were), and its opaque runs.
void warp_card_runs(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const int w = wf.w, h = wf.h;
    const std::uint8_t* const lift = wf.lift;
    const std::uint8_t* const kind = wf.kind;
    const WarpCam& cam = wf.cam;
    const std::size_t verts = wf.verts;
    const int dmin = wf.dmin;
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    constexpr int kSlotCorners = (kSlot + 1) * (kSlot + 1);
    // The wind's time for the plants' sway: the presents' 60ths, quicker in
    // rain (rain_sway), carried on so a change of speed never jumps the
    // swing.
    // (Only what the rain adds is carried: with none it is the clock alone.)
    static float wind_extra = 0.0f;
    static unsigned long wind_serial = 0;
    float rain_lean = 1.0f, rain_quick = 1.0f;
    rain_sway(sp.rain, &rain_lean, &rain_quick);
    {
        const unsigned long dn = sp.serial - wind_serial;
        if (wind_serial != 0 && dn <= 600)
            wind_extra = std::fmod(wind_extra + (rain_quick - 1.0f) * static_cast<float>(dn) / 60.0f, 41.6f);   // 32 x 1.3, 13 x 3.2
        wind_serial = sp.serial;
    }
    const float rain_t = scene_secs(sp) + wind_extra;
    // The pixels the game drew over a sprite by priority (a door's frame over
    // a unit in its doorway, a roof's edge over a unit's feet) and what they
    // belong to (kol_focus grow_over_sprite): again right after that unit's
    // card, a quad each from its own corners, so they cover the unit
    // wherever the warp puts them, and nearer cards still go over them. At
    // rest they draw what they drew. A pixel lifted onto a wall's or a prop's
    // card (Lifted Walls: the door's frame, a roof on BG1; a tree) is that
    // card's quad; the scene there holds what is behind it. Only those that
    // covered the unit at rest (marked 1) with the camera at rest, the rest
    // (grown, 3; the unit on top there, 2) once it moves: the picture at rest
    // stays the game's. A pixel on the unit's own card is the scene's (it
    // holds the ground under him there).
    static std::vector<int> over_start, over_px;
    const bool cam_off_rest = std::abs(g_cam_cap.look_x) + std::abs(g_cam_cap.look_y) > 0.01f || g_cam_cap.strike_t > 0.0f;
    over_start.assign(static_cast<std::size_t>(sp.block_count) + 2, 0);
    over_px.clear();
    if (lift && sp.over_sprite && sp.over_block) {
        // Pixel k's card (its over_block), or 0 when it is not drawn again.
        auto over_of = [&](int k) {
            const std::uint8_t mark = sp.over_sprite[k];
            const int ob = sp.over_block[k];
            return !mark || (mark != 1 && !cam_off_rest) || ob <= 0 || ob > sp.block_count ? 0 : ob;
        };
        for (int k = 0; k < w * h; ++k) {
            const int ob = over_of(k);
            if (!ob) continue;
            ++over_start[static_cast<std::size_t>(ob) + 1];
        }
        for (std::size_t i = 1; i < over_start.size(); ++i) over_start[i] += over_start[i - 1];
        over_px.resize(static_cast<std::size_t>(over_start.back()));
        static std::vector<int> fill;   // (kept: no allocation a picture)
        fill.assign(over_start.begin(), over_start.end());
        for (int k = 0; k < w * h; ++k) {
            const int ob = over_of(k);
            if (!ob) continue;
            over_px[static_cast<std::size_t>(fill[static_cast<std::size_t>(ob)]++)] = k;
        }
    }
    // Where a model has ropes in the air (ScenePixels::rope_air), a redraw is
    // the model's own parts seen again over a unit's card, so it goes far to near,
    // and takes the ropes with it. In row order the deck came over the nearer
    // rail, and the ropes, drawn with the map, lay under whatever was redrawn: they
    // stopped short of their feet with the camera moved (the user's map 90 and
    // 117). Other maps keep their order.
    if (sp.rope_air && !over_px.empty()) {
        auto place_xz = [&](int k) {
            const int lb = lift[k];
            return lb > 0 && lb <= sp.block_count && (sp.blocks[lb - 1].surface || sp.blocks[lb - 1].prop) ? sp.xz[k] : wf.xz[k];
        };
        std::vector<int> ropes;
        for (int k = 0; k < w * h; ++k)
            // (Not a rope farther than the part beside it, 3: in the map's order
            // it stays behind that part, 117's forestay behind the sail.)
            if (sp.rope_air[k] != 0 && sp.rope_air[k] != 3 && lift[k]) ropes.push_back(k);
        std::vector<int> merged, starts(over_start.size(), 0);
        merged.reserve(over_px.size() + ropes.size());
        for (int bi = 0; bi < sp.block_count; ++bi) {
            const int a0 = over_start[static_cast<std::size_t>(bi) + 1], a1 = over_start[static_cast<std::size_t>(bi) + 2];
            starts[static_cast<std::size_t>(bi) + 1] = static_cast<int>(merged.size());
            if (a1 > a0) {
                merged.insert(merged.end(), over_px.begin() + a0, over_px.begin() + a1);
                merged.insert(merged.end(), ropes.begin(), ropes.end());
                std::stable_sort(merged.begin() + starts[static_cast<std::size_t>(bi) + 1], merged.end(),
                                 [&](int x, int y) { return place_xz(x) < place_xz(y); });
            }
        }
        starts[static_cast<std::size_t>(sp.block_count) + 1] = static_cast<int>(merged.size());
        over_px.swap(merged);
        over_start.swap(starts);
    }
    auto push_over = [&](int bi) {   // after card bi's quads
        for (int i = over_start[static_cast<std::size_t>(bi) + 1]; i < over_start[static_cast<std::size_t>(bi) + 2]; ++i) {
            const int k = over_px[static_cast<std::size_t>(i)];
            const int px = k % w, py = k / w;
            const int lb = lift[k];
            if (lb > 0 && lb <= sp.block_count && (sp.blocks[lb - 1].surface || sp.blocks[lb - 1].prop)) {
                const kol::ScenePixels::SpriteBlock& c = sp.blocks[lb - 1];
                g_quads.push_back(Quad{px - c.sx, px - c.sx + 1, py - c.sy, c.slot, -1});
            } else if (kind[k] != kol::kSceneNone) {
                g_quads.push_back(Quad{px, px + 1, py, kOverSpriteSlot});
            }
        }
    };
    if (lift) {
        // Back to front: they share one bucket, drawn in this order.
        static std::vector<int> order;
        const int nb = sp.block_count;
        order.resize(static_cast<std::size_t>(nb));
        for (int i = 0; i < nb; ++i) order[static_cast<std::size_t>(i)] = i;
        // A scene object (a door in its wall: SpriteBlock::scenery) sorts a
        // cell back, behind a unit in its doorway: map 3's Eleanor stepped
        // out to the church door's own place (80, 112) and, tied, the door
        // closing drew over her (the user, states/thor/0013). KOL_SCENERY_SORT_TIE=1:
        // as before.
        static const bool scenery_tie = std::getenv("KOL_SCENERY_SORT_TIE") != nullptr;
        static const bool tie_any = std::getenv("KOL_CARD_TIE_ANY") != nullptr;   // (the tie below)
        auto sort_xz = [&](const kol::ScenePixels::SpriteBlock& B) { return B.xz - (B.scenery && !scenery_tie ? 32 : 0); };
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const auto& A = sp.blocks[a];
            const auto& B = sp.blocks[b];
            const int ka = sort_xz(A), kb = sort_xz(B);
            if (ka != kb) return ka < kb;
            if (A.y != B.y) return A.y < B.y;
            // Tied: the game's later sprite first (SpriteBlock::oam_first).
            // KOL_CARD_TIE_ANY=1: as before (std::sort's own order; tie_any).
            return !tie_any && A.oam_first > B.oam_first;
        });
        for (int oi = 0; oi < nb; ++oi) {
            const int bi = order[static_cast<std::size_t>(oi)];
            const kol::ScenePixels::SpriteBlock& b = sp.blocks[bi];
            const std::size_t base = verts + static_cast<std::size_t>(b.slot) * kSlotCorners;
            if (b.surface) {
                // Lifted BG1 terrain: each corner from the nearest of the
                // square's own pixels touching it, at their own places (the
                // terrain's, sp.xz: the scene's arrays hold what is behind),
                // as the scene's corners are placed; each pixel its own quad,
                // in with the map by its own depth.
                const std::int32_t* fxz = sp.xz;
                const std::int32_t* fy = sp.y;
                const std::uint8_t* fk = sp.kind;
                auto own = [&](int px, int py) -> int {
                    if (px < b.sx || py < b.sy || px >= b.sx + b.w || py >= b.sy + b.h) return -1;
                    const int k = py * w + px;
                    return lift[k] == bi + 1 ? k : -1;
                };
                auto nearer = [&](int a, int c) {
                    if (a < 0) return c;
                    if (c < 0) return a;
                    return (fxz[c] > fxz[a] || (fxz[c] == fxz[a] && fy[c] > fy[a])) ? c : a;
                };
                // Each row's own pixels' first and last column (none: lo > hi):
                // a corner touches its own pixels only within the rows above and
                // below it, between those (2026-11-18: every corner of the square
                // was tested, 33 x 33 for a few pixels, on the Steam Deck).
                int row_lo[kSlot], row_hi[kSlot];
                for (int j = 0; j < b.h; ++j) {
                    row_lo[j] = b.w;
                    row_hi[j] = -1;
                    const std::uint8_t* lr = lift + static_cast<std::size_t>(b.sy + j) * w + b.sx;
                    for (int i = 0; i < b.w; ++i)
                        if (lr[i] == bi + 1) {
                            row_lo[j] = std::min(row_lo[j], i);
                            row_hi[j] = i;
                        }
                }
                for (int j = 0; j <= b.h; ++j) {
                    // The corners' columns that can touch one: lo - 0 .. hi + 1
                    // of the rows above (j - 1) and below (j).
                    int c0 = b.w + 1, c1 = -1;
                    for (const int r : {j - 1, j})
                        if (r >= 0 && r < b.h && row_lo[r] <= row_hi[r]) {
                            c0 = std::min(c0, row_lo[r]);
                            c1 = std::max(c1, row_hi[r] + 1);
                        }
                    for (int i = 0; i <= b.w; ++i) {
                        const int X = b.sx + i, Y = b.sy + j;
                        float* v = &g_xy[(base + static_cast<std::size_t>(j) * (kSlot + 1) + i) * 2];
                        const int best = i < c0 || i > c1 ? -1
                                                          : nearer(nearer(own(X - 1, Y - 1), own(X, Y - 1)),
                                                                   nearer(own(X - 1, Y), own(X, Y)));
                        if (best < 0) { v[0] = static_cast<float>(X); v[1] = static_cast<float>(Y); continue; }
                        cam.corner(w, fxz, fy, fk, best, X, Y, v);
                    }
                }
                const auto [sax0, say0] = kol::slot_origin(b.slot, sp.atlas_per_row);
                for (int j = 0; j < b.h; ++j)
                    for (int i = 0; i < b.w; ++i) {
                        const int k = (b.sy + j) * w + b.sx + i;
                        if (lift[k] != bi + 1) continue;
                        // (A texel left out: an art flame the effect draws anew,
                        // kol_focus kModelFxArt; the scene shows the wall there.)
                        if (sp.atlas_opaque && !sp.atlas_opaque[static_cast<std::size_t>(say0 + j) * sp.atlas_w + sax0 + i])
                            continue;
                        g_quads.push_back(Quad{i, i + 1, j, b.slot, std::max(0, (fxz[k] - dmin) / 4)});
                    }
                continue;
            }
            const float d = static_cast<float>(b.xz), y = static_cast<float>(b.y);
            const float s = cam.scale(d);
            const float rd = cam.row_drawn(d, y, s), ro = cam.row_of(d, y);
            // A plant sways with the wind (prop_sway(): how far a point
            // 32 rows over its foot leans, 0 off): its foot stays, each
            // row leans by (rows over the foot / 32)^1.5 of it, so it bends.
            // The wind is a slow swing with a quicker gust, both rolling over
            // the map along X + Z (a gust reaches the plants in turn).
            float lean = 0.0f;
            const float sway = prop_sway() * rain_lean;
            if (b.prop && b.plant && sway > 0.0f) {
                const float t = rain_t;
                const float u = b.wind_x + b.wind_z;
                constexpr float kTwoPi = 6.2831853f;
                lean = sway * (0.65f * std::sin(kTwoPi * (t / 3.2f - u / 384.0f)) +
                                      0.35f * std::sin(kTwoPi * (t / 1.3f - u / 160.0f) + 1.7f));
            }
            for (int j = 0; j <= b.h; ++j)
                for (int i = 0; i <= b.w; ++i) {
                    float* v = &g_xy[(base + static_cast<std::size_t>(j) * (kSlot + 1) + i) * 2];
                    float bend = 0.0f;
                    if (lean != 0.0f) {
                        const float up = b.foot_row - static_cast<float>(b.sy + j);
                        if (up > 0.0f) bend = lean * std::pow(std::min(1.5f, up / 32.0f), 1.5f);
                    }
                    if (b.effect == 3) {
                        // A fall's vapor: its plane runs across the square, each column
                        // at its own depth. At one depth a square scaled apart from the
                        // next, with a gap between them (the user, on the Thor, saw vertical
                        // tears); this way neighbours share their edge exactly.
                        const float along = 2.0f * static_cast<float>(i - b.w / 2);
                        const float dc = d + (kol::kMapFalls[b.fx_seed].on_x ? -along : along);
                        cam.upright(dc, y, cam.scale(dc), static_cast<float>(b.sx + i), static_cast<float>(b.sy + j), v);
                    } else if (b.bend_sx >= 0) {
                        // Bent along its cell's front edges (a spire's V),
                        // each arm on its own floor (bend_dyl / bend_dyr).
                        const float dc = d + static_cast<float>(b.bend_tip) -
                                         2.0f * static_cast<float>(std::min(16, std::abs(b.sx + i - b.bend_sx)));
                        const int col = b.sx + i;
                        const float yc = y + (col < b.bend_sx ? static_cast<float>(b.bend_dyl)
                                              : col > b.bend_sx ? static_cast<float>(b.bend_dyr)
                                              : 0.5f * static_cast<float>(b.bend_dyl + b.bend_dyr));
                        cam.upright(dc, yc, cam.scale(dc), static_cast<float>(b.sx + i) + bend, static_cast<float>(b.sy + j), v);
                    } else {
                        v[0] = cam.cxp + (static_cast<float>(b.sx + i) + bend - cam.cxp) * s;
                        v[1] = rd + (static_cast<float>(b.sy + j) - ro) * s;
                    }
                    cam.zoomed(v);
                }
            const auto [ax0, ay0] = kol::slot_origin(b.slot, sp.atlas_per_row);
            for (int j = 0; j < b.h; ++j) {
                const std::uint8_t* op = sp.atlas_opaque + static_cast<std::size_t>(ay0 + j) * sp.atlas_w + ax0;
                for (int i = 0; i < b.w;) {
                    if (!op[i]) { ++i; continue; }
                    int e = i + 1;
                    // (Bent, a quad a pixel: a run's quad takes only its
                    // ends' corners, and rows' runs ending apart tore. A fall's
                    // vapor: its alpha is per corner, and across a run it was the
                    // ends' alone, white bands with hard edges; two pixels a quad on even
                    // columns, the cloud's own half resolution, every row the same.)
                    while (e < b.w && op[e] && b.bend_sx < 0 && (b.effect != 3 || (e & 1))) ++e;
                    // A prop's square goes in with the map, just past its
                    // cell's front corner (its own cell's top must not cover
                    // its foot; nearer terrain still covers it).
                    // An overlay card (interface moved with a unit) goes last.
                    int key = b.prop ? std::max(0, (b.xz + 32 + b.sort_extra - dmin) / 4 + 1) : b.overlay ? -2 : -1;
                    // A fall's vapor stands on a plane running across the square, so
                    // each of its quads goes in at its own column's depth, and past
                    // the pool's water at its own row (nearer than the plane, it went
                    // in after it, over the cloud's lower half). One depth for the
                    // square cut it in a straight line anywhere across a rock.
                    if (b.effect == 3) {
                        const int along = 2 * (i - b.w / 2);
                        const int col_xz = b.xz + (kol::kMapFalls[b.fx_seed].on_x ? -along : along);
                        const int water_xz = 4 * (b.sy + j + 1 - sp.ky - sp.oy) + 2 * static_cast<int>(kol::kFallPool);
                        key = std::max(0, (std::max(col_xz + 64, water_xz + 4) - dmin) / 4 + 1);   // (+ 64: past the rocks beside the falls; vapor_texel fades over them)
                    }
                    g_quads.push_back(Quad{i, e, j, b.slot, key});
                    i = e;
                }
            }
            push_over(bi);
        }
    }
}

// Far to near (a counting sort on depth, 4 units a bucket).
// Two buckets on top: units drawn from their own sprites, then overlays.
// Units come after the whole map, as the game layers them: it draws
// sprites over the background except where priority hides them, and
// those texels are out of the atlas already. Sorted by depth among the
// map, terrain the height grid puts nearer and higher slid over units the
// game shows whole (states/BO_grimal_alphonse: the walkway's units under
// the upper castle), at rest too.
void warp_buckets(WarpFrame& wf) {
    const kol::ScenePixels& sp = *wf.sp;
    const int w = wf.w;
    const std::uint8_t* const lift = wf.lift;
    const std::int32_t* const xz = wf.xz;
    const std::uint8_t* const kind = wf.kind;
    const std::uint8_t* const prop_lift = wf.prop_lift;
    const int dmin = wf.dmin, dmax = wf.dmax;
    const int buckets = (dmax - dmin) / 4 + 3;
    g_bucket.assign(static_cast<std::size_t>(buckets) + 1, 0);
    g_quad_key.resize(g_quads.size());
    for (std::size_t q = 0; q < g_quads.size(); ++q) {
        const Quad& Q = g_quads[q];
        if (Q.slot == kOverSpriteSlot) {
            g_quad_key[q] = buckets - 2;   // with the units' cards, after them (pushed later)
        } else if (Q.slot >= 0) {
            g_quad_key[q] = Q.key >= 0 ? std::min(Q.key, buckets - 3) : Q.key == -2 ? buckets - 1 : buckets - 2;
        } else {
            const std::size_t first = static_cast<std::size_t>(Q.row) * w + Q.a;
            // Overlays are drawn last, in front of everything.
            g_quad_key[q] = kind[first] == kol::kSceneOverlay ? buckets - 1
                                                              : (xz[first] - dmin) / 4;
            // The ground a prop hid, before the prop's card: by its own row
            // place the castle statue's ground (the pool's back wall) went
            // after the card and painted a band across the gargoyle.
            if (prop_lift && prop_lift[first] && lift[first] <= sp.block_count &&
                sp.blocks[lift[first] - 1].prop)
                g_quad_key[q] = std::min(g_quad_key[q],
                                         std::max(0, (sp.blocks[lift[first] - 1].xz + 32 - dmin) / 4));
            // And the scene under a lifted wall's square, before every pixel of
            // that square (its nearest's bucket, less one). Where it keeps the
            // wall's own place (kol_focus g_inlay) it tied with the wall's pixel
            // and came after it, and a strip stretched from there to the ground
            // behind went in at the near end's depth, over the wall's farther
            // pixels: the other layer's colours smeared on the wall with the
            // stick held down (map 36, Thor state 0272).
            if (lift && lift[first] && lift[first] <= sp.block_count && sp.blocks[lift[first] - 1].surface)
                g_quad_key[q] = std::min(g_quad_key[q],
                                         std::max(0, (sp.blocks[lift[first] - 1].xz - dmin) / 4 - 1));
        }
        ++g_bucket[g_quad_key[q] + 1];
    }

    for (int bkt = 1; bkt <= buckets; ++bkt) g_bucket[bkt] += g_bucket[bkt - 1];
    g_sorted.resize(g_quads.size());
    for (std::size_t q = 0; q < g_quads.size(); ++q)
        g_sorted[g_bucket[g_quad_key[q]]++] = g_quads[q];
}

// The strips' own vertices, then every run zipped into triangles (the
// mesh's index list).
void warp_zip(WarpFrame& wf) {
    const int w = wf.w, vw = wf.vw;
    const std::size_t verts = wf.verts, all_verts = wf.all_verts;
    const SameSurface same_surface{wf.kind, wf.xz, wf.yy};
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    constexpr int kSlotCorners = (kSlot + 1) * (kSlot + 1);
    // The strips behind whose own corner reaches under the surface in front
    // (see "Strips behind end at their own edge"): a vertex of their own.
    std::size_t own_n = 0;
    g_own_vtx.assign(g_own_px.size(), -1);
    for (std::size_t c = 0; c < g_own_px.size(); ++c) {
        if (g_own_px[c] < 0) continue;
        const float shared = g_xy[c * 2 + 1], own = g_own_xy[c * 2 + 1];
        if (g_own_dir[c] != 0 && (g_own_dir[c] > 0 ? own >= shared : own <= shared)) continue;   // a gap: stretch to fill it
        g_own_vtx[c] = static_cast<int>(all_verts + own_n);
        ++own_n;
    }
    g_xy.resize((all_verts + own_n) * 2);
    g_uv.resize((all_verts + own_n) * 2);
    for (std::size_t c = 0; c < g_own_vtx.size(); ++c) {
        const int v = g_own_vtx[c];
        if (v < 0) continue;
        g_xy[static_cast<std::size_t>(v) * 2] = g_own_xy[c * 2];
        g_xy[static_cast<std::size_t>(v) * 2 + 1] = g_own_xy[c * 2 + 1];
        g_uv[static_cast<std::size_t>(v) * 2] = static_cast<float>(c % vw);
        g_uv[static_cast<std::size_t>(v) * 2 + 1] = static_cast<float>(c / vw);
    }
    const std::size_t mesh_verts = all_verts + own_n;
    // Each run is zipped into triangles on its own, so the sorted runs are
    // split into chunks, each chunk's triangles kept apart, and the chunks
    // joined in order: the same list as one pass would make.
    const int quads = static_cast<int>(g_sorted.size());
    constexpr int kQuadChunks = 8;
    const int quad_chunk = std::max(1, (quads + kQuadChunks - 1) / kQuadChunks);
    if (g_idx_parts.size() < static_cast<std::size_t>(kQuadChunks))
        g_idx_parts.resize(kQuadChunks);
    const int* const own_vtx = own_n ? g_own_vtx.data() : nullptr;
    const int* const own_px = g_own_px.data();
    const std::int8_t* const own_dir = g_own_dir.data();
    row_pool().run(quads, quad_chunk, [vw, verts, w, own_vtx, own_px, own_dir, same_surface](int chunk, int q0, int q1) {
        IdxPart& part = g_idx_parts[static_cast<std::size_t>(chunk)];
        // At most a triangle per corner advanced on either edge of a run
        // (two per column), six indices for a sprite run.
        std::size_t bound = 0;
        for (int q = q0; q < q1; ++q) {
            const Quad& Q = g_sorted[static_cast<std::size_t>(q)];
            bound += Q.slot >= 0 ? 6u : 6u * static_cast<std::size_t>(Q.b - Q.a);
        }
        if (part.buf.size() < bound) part.buf.resize(bound);
        int* const out = part.buf.data();
        std::size_t n = 0;
        for (int q = q0; q < q1; ++q) {
            const Quad& Q = g_sorted[static_cast<std::size_t>(q)];
            if (Q.slot >= 0) {   // a sprite run: one straight quad
                const int base = static_cast<int>(verts) + Q.slot * kSlotCorners;
                const int t0 = base + Q.row * (kSlot + 1), t1 = t0 + (kSlot + 1);
                out[n++] = t0 + Q.a; out[n++] = t0 + Q.b; out[n++] = t1 + Q.a;
                out[n++] = t0 + Q.b; out[n++] = t1 + Q.b; out[n++] = t1 + Q.a;
                continue;
            }
            // The kept corners of the two edges, both ends included.
            const std::uint8_t* keep_t = g_keep.data() + static_cast<std::size_t>(Q.row) * vw;
            const std::uint8_t* keep_b = keep_t + vw;
            const int top = Q.row * vw, bot = top + vw;
            const int end = Q.b;
            // A corner's vertex for this run: its own where the run is the
            // surface behind there and has one (see "Strips behind end at
            // their own edge"), else the shared corner.
            auto vtx = [&](int c, int col, int dir) {
                if (!own_vtx || own_vtx[c] < 0 || (own_dir[c] != 0 && own_dir[c] != dir)) return c;
                const std::size_t p = static_cast<std::size_t>(Q.row) * w + std::clamp(col, Q.a, Q.b - 1);
                return same_surface(p, static_cast<std::size_t>(own_px[c])) ? own_vtx[c] : c;
            };
            int i = Q.a, k = Q.a;
            auto next = [end](const std::uint8_t* keep, int from) {
                int n = from + 1;
                while (n < end && !keep[n]) ++n;
                return n;
            };
            // Each edge's next kept corner, found again only when that edge
            // advances (rescanning the edge that stays was most of the zip).
            int ni = i < end ? next(keep_t, i) : end;
            int nk = k < end ? next(keep_b, k) : end;
            while (i < end || k < end) {
                if (k >= end || (i < end && ni <= nk)) {
                    out[n++] = vtx(top + i, i, 1);
                    out[n++] = vtx(top + ni, ni, 1);
                    out[n++] = vtx(bot + k, k, -1);
                    i = ni;
                    ni = i < end ? next(keep_t, i) : end;
                } else {
                    out[n++] = vtx(top + i, i, 1);
                    out[n++] = vtx(bot + nk, nk, -1);
                    out[n++] = vtx(bot + k, k, -1);
                    k = nk;
                    nk = k < end ? next(keep_b, k) : end;
                }
            }
        }
        part.n = n;
    });
    const int used_chunks = (quads + quad_chunk - 1) / quad_chunk;
    std::size_t total = 0;
    for (int c = 0; c < used_chunks; ++c) total += g_idx_parts[static_cast<std::size_t>(c)].n;
    g_idx.resize(total);
    std::size_t at = 0;
    for (int c = 0; c < used_chunks; ++c) {
        const IdxPart& part = g_idx_parts[static_cast<std::size_t>(c)];
        std::copy(part.buf.begin(), part.buf.begin() + static_cast<std::ptrdiff_t>(part.n),
                  g_idx.begin() + static_cast<std::ptrdiff_t>(at));
        at += part.n;
    }
    wf.mesh_verts = mesh_verts;
}

// The mesh handed over: the traces and dumps, the smoothed pan, the
// see-through squares' alpha, the pictures and our rain.
void warp_output(WarpFrame& wf, gbarecomp::PresentWarpMesh* mesh) {
    const kol::ScenePixels& sp = *wf.sp;
    const std::uint8_t* const scene = wf.scene;
    const int w = wf.w, h = wf.h;
    const std::size_t pixels = wf.pixels;
    const std::uint8_t* const lift = wf.lift;
    const int view_x = wf.view_x, view_y = wf.view_y, view_w = wf.view_w, view_h = wf.view_h;
    const WarpCam& cam = wf.cam;
    const bool moved = wf.moved;
    const int slots = wf.slots;
    const std::size_t verts = wf.verts, all_verts = wf.all_verts, mesh_verts = wf.mesh_verts;
    constexpr int kSlot = kol::ScenePixels::kSpriteSlot;
    constexpr int kSlotCorners = (kSlot + 1) * (kSlot + 1);
    if (kol::persp_hash_trace()) {
        const std::size_t atlas_bytes = lift ? static_cast<std::size_t>(sp.atlas_w) * sp.atlas_h * 3 : 0;
        std::fprintf(stderr, "kol-persp-hash f=%ld pass4: idx=%08x xy=%08x backdrop=%08x flat=%08x atlas=%08x moved=%08x blocks=%d\n",
                     g_trace_frame, kol::fnv_bytes(g_idx), kol::fnv_bytes(g_xy), kol::fnv_bytes(g_backdrop), kol::fnv_bytes(g_flat),
                     kol::fnv_bytes(std::vector<std::uint8_t>(sp.atlas_rgb, sp.atlas_rgb + atlas_bytes)),
                     moved ? kol::fnv_bytes(g_moved) : 0u, sp.block_count);
        ++g_trace_frame;
    }
    // KOL_PERSP_MESH_DUMP=<path>: the 40th (KOL_PERSP_MESH_DUMP_AT) present's mesh, raw (header w, h,
    // view x/y/w/h, index count, vertex count; then xy, uv, indices, the scene
    // texels, the atlas rows and the atlas), to trace a pixel offline to the
    // triangle and texel that drew it.
    if (static const char* dump_path = std::getenv("KOL_PERSP_MESH_DUMP"); dump_path) {
        static long dump_n = 0;
        static const long dump_at = [] {   // KOL_PERSP_MESH_DUMP_AT=<n>, default 40
            const char* v = std::getenv("KOL_PERSP_MESH_DUMP_AT");
            return v && *v ? std::atol(v) : 40L;
        }();
        if (++dump_n == dump_at)
            if (FILE* f = std::fopen(dump_path, "wb")) {
                const int hdr[8] = {w, h, view_x, view_y, view_w, view_h, static_cast<int>(g_idx.size()),
                                    static_cast<int>(mesh_verts)};
                std::fwrite(hdr, sizeof hdr, 1, f);
                std::fwrite(g_xy.data(), 4, mesh_verts * 2, f);
                std::fwrite(g_uv.data(), 4, mesh_verts * 2, f);
                std::fwrite(g_idx.data(), 4, g_idx.size(), f);
                std::fwrite(scene, 1, pixels * 3, f);
                const int ah = lift ? sp.atlas_h : 0;
                std::fwrite(&ah, 4, 1, f);
                if (ah) std::fwrite(sp.atlas_rgb, 1, static_cast<std::size_t>(sp.atlas_w) * ah * 3, f);
                std::fclose(f);
            }
    }
    // Placed in scene pixels; drawn in the view's (and where a smoothed pan
    // puts the scene).
    float pan_x = 0.0f, pan_y = 0.0f;
    pan_smooth(sp, &pan_x, &pan_y);
    if (view_x || view_y || pan_x != 0.0f || pan_y != 0.0f)
        for (std::size_t v = 0; v < mesh_verts; ++v) {
            g_xy[v * 2] -= static_cast<float>(view_x) - pan_x;
            g_xy[v * 2 + 1] -= static_cast<float>(view_y) - pan_y;
        }
    // See-through squares (smoke): their corners at their alpha, the rest
    // opaque; no alpha at all when there are none (drawn as before).
    bool see_through = false;
    for (int bi = 0; bi < sp.block_count && lift; ++bi)
        see_through = see_through || sp.blocks[bi].see_through < 1.0f || sp.blocks[bi].effect;
    if (see_through) {
        g_alpha.assign(mesh_verts, 255);
        for (int bi = 0; bi < sp.block_count; ++bi) {
            const auto& b = sp.blocks[bi];
            if ((b.see_through >= 1.0f && !b.effect) || b.slot < 0 || b.slot >= slots) continue;
            const std::size_t base = verts + static_cast<std::size_t>(b.slot) * kSlotCorners;
            if (b.effect) {   // each corner the effect's own alpha there (worked out by the fill)
                const std::size_t abase = static_cast<std::size_t>(b.slot) * kSlotCorners;
                if (g_fx_alpha.size() >= abase + kSlotCorners)
                    std::copy_n(&g_fx_alpha[abase], kSlotCorners, &g_alpha[base]);
                continue;
            }
            const auto al = static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(b.see_through, 0.0f, 1.0f)));
            std::fill_n(&g_alpha[base], kSlotCorners, al);
        }
    }
    mesh->alpha = see_through ? g_alpha.data() : nullptr;
    mesh->xy = g_xy.data();
    mesh->uv = g_uv.data();
    mesh->uv_gen = g_uv_gen;   // the grid's uv only change with it; the own corners' after it do
    mesh->uv_fixed = static_cast<int>(all_verts);
    mesh->vertices = static_cast<int>(mesh_verts);
    mesh->indices = g_idx.data();
    mesh->index_count = static_cast<int>(g_idx.size());
    field_all(sp, g_backdrop.data(), g_backdrop.size() / 3, true);   // the flash over the sky too
    mesh_pictures(mesh, moved, lift ? &sp : nullptr);
    kol::RainCam rc = rain_cam(sp);
    rc.dref = cam.dref; rc.yref = cam.yref; rc.K = cam.K; rc.pitch = cam.pitch; rc.zoom = cam.zoom;
    rc.cx = cam.cx; rc.cy = cam.cy; rc.cxp = cam.cxp;
    kol::rain_build(sp, w, h, rc, mesh);
    kol::snow_build(sp, w, h, rc, mesh);
    hud_glow_over(sp, mesh, w, h, rc);
}

// The perspective's warp (see the top of this file), in its phases, each
// taking and leaving what they share in a WarpFrame: the places, the camera,
// the atlas's cards, the backdrop, the floating hand, the fill under the
// interface, the light, the edge and the debug paint, the flat pixels, then
// the mesh (corners, kept corners, runs, cards' runs, buckets, the zip) and
// its hand-over.
int warp_provider(const std::uint8_t* layers, std::uint8_t* scene, int w, int h,
                  gbarecomp::PresentWarpMesh* mesh) {
    kol::PresentScope present;   // guest state from the capture
    kol::RainFrame rain_frame;
    kol::SnowFrame snow_frame;
    if (w <= 240 || h < 160) return 0;
    layers = kol::present_layers(layers, w, h);
    kol::worker_time_job_done();
    kol::WorkerTimer wt("warp.scene");
    kol::ScenePixels sp;
    kol::hud_bake_into(scene, w, h);
    if (!kol::scene_pixels(layers, w, h, &sp, scene)) {
        g_have_ref.store(false, std::memory_order_relaxed);
        return 0;
    }
    wt.lap("warp.atlasfill");
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    palette_lag(sp, layers, scene, pixels);
    model_recolour(sp, scene, pixels);
    const std::uint8_t* iface = sp.iface;
    if (sp.sky != kol::kSkyGame) recolour_backdrop(sp, layers, iface, scene, pixels, sp.sky, sp.sky_desat, sp.sky_mix);
    smooth_sky(sp, layers, iface, scene, w, h);
    // The view in the scene: the scene may go on past it (the overscan, see
    // perspective_overscan), so that what the far side shrinks away from is
    // filled from real map. The projection, the backdrop and the flat pixels
    // belong to the view; the mesh covers the whole scene.
    const int view_x = sp.vx, view_y = sp.vy, view_w = sp.vw, view_h = sp.vh;
    const std::size_t view_pixels = static_cast<std::size_t>(view_w) * view_h;
    WarpFrame wf;
    wf.sp = &sp;
    wf.layers = layers;
    wf.scene = scene;
    wf.w = w;
    wf.h = h;
    wf.pixels = pixels;
    wf.iface = iface;
    wf.lift = sp.lift;
    wf.view_x = view_x;
    wf.view_y = view_y;
    wf.view_w = view_w;
    wf.view_h = view_h;
    wf.view_pixels = view_pixels;
    // (KOL_PERSP_OVERLAY_LIFTED=1: WarpFrame::moved_overlay as before.)
    static const bool overlay_lifted = std::getenv("KOL_PERSP_OVERLAY_LIFTED") != nullptr;
    wf.overlay_lifted = overlay_lifted;
    warp_places(wf);
    warp_camera(wf);
    warp_cards(wf);

    wt.lap("warp.backdrop");
    warp_backdrop(wf);
    warp_hand(wf);

    wt.lap("warp.under");
    warp_under(wf);

    wt.lap("warp.lighting");
    warp_light(wf, wt);
    wt.lap("warp.edge+paint");
    warp_edge_paint(wf);

    wt.lap("warp.flat");
    warp_flat(wf, mesh);

    // (The mesh by step, 2026-11-18: as one "warp.mesh" it was half the job
    // on the Steam Deck's map 55 and said nothing of where.)
    wt.lap("mesh.corners");
    warp_corners(wf);
    wt.lap("mesh.keep");
    warp_keep(wf);
    wt.lap("mesh.runs");
    warp_runs(wf);
    wt.lap("mesh.card_runs");
    warp_card_runs(wf);
    if (g_quads.empty()) return 0;
    wt.lap("mesh.buckets");
    warp_buckets(wf);
    wt.lap("mesh.zip");
    warp_zip(wf);
    wt.lap("mesh.output");
    warp_output(wf, mesh);
    return 1;
}

}  // namespace

namespace kol {

void perspective_overscan(int view_w, int view_h, unsigned out[4]) {
    // On the ground at the view's top row, depth is 4 * (view_h / 2) units
    // behind the middle, so the scale there is s = K / (K + 2 view_h). The
    // top corners then show the scene from (view_w / 2) (1/s - 1) beyond each
    // side, and the top row the ground row u above the middle where
    // u s = view_h / 2 with s = K / (K + 4u): u = (view_h/2) K / (K - 2 view_h).
    // Heights and the map's outline reach a little further: 8 more.
    // With the combat camera's zoom Z about the view's centre, a place is
    // drawn Z s times its size: the sides need (view_w/2)(1/(Z s) - 1), and
    // the top row shows u where Z u s = view_h/2: u = (view_h/2) K / (Z K -
    // 2 view_h). At Z = 1 these are the camera's own.
    constexpr float kSlack = 8.0f;
    // The combat camera's pitch f draws depth's rows at f: the top then
    // shows u where Z f u s = view_h/2.
    // The look's shift dx of the perspective's centre adds |dx| (1/s - 1)
    // on one side (see camera()).
    const float half_h = view_h * 0.5f, half_w = view_w * 0.5f;
    Cam cams[10];
    const int cameras = plan_cameras(half_w, half_h, cams);
    float side = 0.0f, top = 0.0f;
    for (int i = 0; i < cameras; ++i) {
        const float K = cams[i].K, Z = cams[i].Z;
        const float s_top = K / (K + 4.0f * half_h);
        side = std::max(side, half_w * (1.0f / (Z * s_top) - 1.0f) +
                              std::abs(cams[i].dx) * (1.0f / s_top - 1.0f) + kSlack);
        const float den = Z * cams[i].f * K - 4.0f * half_h;
        top = std::max(top, den > 0.0f ? half_h * K / den - half_h + kSlack : 1e9f);
    }
    auto px = [](float v) {
        return static_cast<unsigned>(std::min(1000.0f, std::ceil(std::max(0.0f, v))));
    };
    // Held steady while the camera moves: the scene's size and place are keys
    // of every per-view cache (the window light, the lamps, the AO), so a
    // margin creeping a pixel a frame with the stick rebuilt them all every
    // frame (Thor state 0309, map 62: the worker 6 -> 28 ms on the Mac). The
    // margin grows at once, in steps of kStep, and shrinks only after
    // kShrinkFrames of needing less. KOL_OVERSCAN_EXACT=1: as before.
    static const bool exact = kol::env_true("KOL_OVERSCAN_EXACT");
    if (exact) {
        out[0] = out[1] = px(side);
        out[2] = px(top);
        out[3] = 0u;
        return;
    }
    constexpr unsigned kStep = 16;
    constexpr int kShrinkFrames = 60;
    auto up = [](unsigned v) { return (v + kStep - 1) / kStep * kStep; };
    struct Held { int w = 0, h = 0; unsigned side = 0, top = 0; int lower_side = 0, lower_top = 0; };
    static Held held;
    const unsigned want_side = up(px(side)), want_top = up(px(top));
    if (held.w != view_w || held.h != view_h) held = {view_w, view_h, want_side, want_top, 0, 0};
    auto settle = [](unsigned want, unsigned& have, int& lower) {
        if (want >= have) { have = want; lower = 0; return; }
        if (++lower >= kShrinkFrames) { have = want; lower = 0; }
    };
    settle(want_side, held.side, held.lower_side);
    settle(want_top, held.top, held.lower_top);
    out[0] = out[1] = held.side;
    out[2] = held.top;
    out[3] = 0u;
}

bool perspective_overscan_rows(int view_w, int view_h, const unsigned overscan[4],
                               unsigned char* rows, int n) {
    if (!g_have_ref.load(std::memory_order_relaxed)) return false;
    // A place at view row r and height Y lies dref - D = 4 (view_h/2 - r) +
    // 2 (Yref - Y) behind the reference (see warp_provider), is scaled by
    // s = K / (K + dref - D), and so is drawn (view_w/2)(1/s - 1) =
    // (view_w/2)(dref - D)/K farther in from the side than it is. Nothing on
    // the map is below Y = 0, and Yref moves 5% of the way to its target a
    // frame, so a frame from now it is within kDrift of where it is. Rows
    // with nothing behind the reference are drawn larger: they need none.
    // The same 8 of slack as perspective_overscan, for the corners and the
    // map's outline.
    // With the combat camera's zoom Z (see perspective_overscan) a place is
    // drawn Z s times its size: (view_w/2)(1/(Z s) - 1) in.
    constexpr float kSlack = 8.0f;
    constexpr float kDrift = 32.0f;
    Cam cams[10];
    const int cameras = plan_cameras(view_w * 0.5f, view_h * 0.5f, cams);
    const float yb = std::max(0.0f, g_yref.load(std::memory_order_relaxed)) + kDrift;
    const float side = static_cast<float>(std::max(overscan[0], overscan[1]));
    for (int c = 0; c < n; ++c) {
        const float r = static_cast<float>(c) - static_cast<float>(overscan[2]);
        const float behind = 4.0f * (view_h * 0.5f - r) + 2.0f * yb;
        // In view px: (view_w/2)((1 + b/K)/Z - 1), plus |dx| b/K for the
        // look's shift (see perspective_overscan).
        float need = 0.0f;
        for (int i = 0; i < cameras; ++i)
            need = std::max(need, view_w * 0.5f * ((1.0f + behind / cams[i].K) / cams[i].Z - 1.0f) +
                                      std::abs(cams[i].dx) * behind / cams[i].K);
        need = need > 0.0f ? need + kSlack : 0.0f;
        rows[c] = static_cast<unsigned char>(std::min(side, std::ceil(need)));
    }
    return true;
}

void perspective_strike_install() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_prev_entry_hook = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = strike_entry;
}

void perspective_strike_tick(bool perspective) {
    // KOL_PERSP_NO_STRIKE=1 keeps the camera still; KOL_PERSP_STRIKE_FORCE=1
    // holds the combat camera on (for testing it anywhere).
    static const bool off = kol::env_true("KOL_PERSP_NO_STRIKE");
    static const bool force = kol::env_true("KOL_PERSP_STRIKE_FORCE");
    if (g_strike_seen) g_strike_hold = kStrikeHold;
    else if (g_strike_hold > 0) --g_strike_hold;
    g_strike_seen = false;
    // A spell's camera, through its hold and the ease back out.
    if (g_spell_seen) g_spell_left = kStrikeHold + static_cast<int>(1.0f / kStrikeStep) + 1;
    else if (g_spell_left > 0) --g_spell_left;
    g_spell_seen = false;
    if (g_charge_seen) g_charge_left = kStrikeHold;
    else if (g_charge_left > 0) --g_charge_left;
    g_charge_seen = false;
    g_strike_active = perspective && !off && (force || g_strike_hold > 0);
    g_strike_t = !perspective ? 0.0f
        : g_strike_active ? std::min(1.0f, g_strike_t + kStrikeStep)
                          : std::max(0.0f, g_strike_t - kStrikeStep);

    // The look, from the right stick (15% dead zone); KOL_PERSP_LOOK=x,y
    // holds it (testing without a stick), KOL_PERSP_NO_LOOK=1 ignores it.
    static const bool no_look = kol::env_true("KOL_PERSP_NO_LOOK");
    static float forced[2] = {0.0f, 0.0f};
    static const bool force_look = [] {
        const char* e = std::getenv("KOL_PERSP_LOOK");
        return e && std::sscanf(e, "%f,%f", &forced[0], &forced[1]) == 2;
    }();
    auto dead = [](float v) {
        constexpr float kDead = 0.15f;
        const float a = std::abs(v);
        return a <= kDead ? 0.0f : std::copysign(std::min(1.0f, (a - kDead) / (1.0f - kDead)), v);
    };
    // Debug > L2 Moves Sun: while L2 is held the stick's x moves the sun
    // along its arc over the battlefield, toward the side pushed, 45
    // degrees a second at full tilt, its y turns the sun's direction about
    // the field (up clockwise on screen, 60 degrees a second), and the look
    // lets go; where it is is logged when the stick lets go. Off, the sun
    // is the Shadows pick (or the map's) again.
    constexpr float kSunArc = 0.75f;   // degrees a frame
    constexpr float kSunTurn = 1.0f;
    static bool sun_moving = false;
    const bool sun_stick = perspective && kol::setting_on(kol::Setting::DebugSunStick);
    const bool sun_held = sun_stick && gbarecomp::g_host_pad_left_trigger > 0.5f;
    const float sun_x = sun_held ? dead(gbarecomp::g_host_pad_right_stick[0]) : 0.0f;
    const float sun_y = sun_held ? dead(gbarecomp::g_host_pad_right_stick[1]) : 0.0f;
    if (!kol::setting_on(kol::Setting::DebugSunStick)) kol::sun_arc_reset();
    if (sun_x != 0.0f) kol::sun_arc_move(sun_x * kSunArc);
    if (sun_y != 0.0f) kol::sun_turn_move(-sun_y * kSunTurn);
    // KOL_PERSP_SUN_SPIN=<degrees a frame>: the sun moved every frame as by
    // the stick, back and forth over 60 degrees (headless cost runs).
    static const float spin = kol::env_num("KOL_PERSP_SUN_SPIN", 0.0f);
    if (spin != 0.0f) {
        static int t = 0;
        const int period = std::max(2, static_cast<int>(60.0f / std::abs(spin)));
        kol::sun_arc_move(((t++ / period) % 2) ? -spin : spin);
    }
    if (sun_moving && sun_x == 0.0f && sun_y == 0.0f) kol::sun_arc_report();
    sun_moving = sun_x != 0.0f || sun_y != 0.0f;
    kol::sun_tick();
    // (Not on the maps of map_settings.cfg `no_look`. The stick still turns
    // the sun there with Debug > L2 Moves Sun. KOL_PERSP_LOOK still holds a
    // look there, to check a map's model from the side.)
    const bool map_still = kol::map_no_look() && !force_look;
    // The stick's whole range in a battle; a little only (kSmallLook of it) in a story
    // event (EWRAM 0x020013D0 == 1, kol::story_event(): story scenes, pre-battle talk,
    // scripted preparations) and on the maps of map_settings.cfg `small_look` (89, 71,
    // 70). Asked 2026-11-16 for scene maps to have a more restricted camera shift,
    // then, with the decompilation's table of the battle's flags, for a wider
    // shift in battle and a small look otherwise. The look eases between
    // the two as it does toward the stick. KOL_SMALL_LOOK=<0..1> tries another reach
    // (1: the whole range everywhere). A held KOL_PERSP_LOOK is not scaled: 0.3,0.3 is
    // the furthest the stick goes on a small look.
    static const float small = std::clamp(kol::env_num("KOL_SMALL_LOOK", kSmallLook), 0.0f, 1.0f);
    // (Preparation was small too at first, my reading of in battle; the user, on
    // Thor state 0297, a Training battle's preparation, asked for the battle
    // camera there. A preparation whose NAVI script runs is a story event: small.)
    const bool in_battle = !kol::story_event();
    const float reach = force_look ? 1.0f : !in_battle || kol::map_small_look() ? small : 1.0f;
    // (KOL_PERSP_STICK=x,y: the stick held there, through the reach, for headless checks.)
    static float held[2] = {0.0f, 0.0f};
    static const bool hold_stick = [] {
        const char* e = std::getenv("KOL_PERSP_STICK");
        return e && std::sscanf(e, "%f,%f", &held[0], &held[1]) == 2;
    }();
    // (KOL_PERSP_STICK_SWEEP=<frames a turn>: the stick goes round a circle, so
    // the camera moves every frame, for headless cost runs.)
    static const float sweep = kol::env_num("KOL_PERSP_STICK_SWEEP", 0.0f);
    static int sweep_t = 0;
    const float sweep_a = sweep > 0.0f ? 6.2831853f * static_cast<float>(sweep_t++) / sweep : 0.0f;
    const float stick_x = sweep > 0.0f ? std::sin(sweep_a)
                        : hold_stick ? held[0] : dead(gbarecomp::g_host_pad_right_stick[0]);
    const float stick_y = sweep > 0.0f ? std::cos(sweep_a)
                        : hold_stick ? held[1] : dead(gbarecomp::g_host_pad_right_stick[1]);
    g_look_tx = !perspective || no_look || sun_held || map_still ? 0.0f
        : reach * kLookRangeX * (force_look ? forced[0] : stick_x);
    // Up (the camera higher) goes kLookUpMore further in a battle (asked 2026-11-16
    // for the stick's upward camera movement to go a bit further in battle): to
    // 2.6 where down stops at 2. Not on a small look, nor a held KOL_PERSP_LOOK.
    // KOL_LOOK_UP_MORE=<factor> tries another (1: as before).
    static const float up_more = std::max(1.0f, kol::env_num("KOL_LOOK_UP_MORE", kLookUpMore));
    const float up = !force_look && reach == 1.0f && stick_y < 0.0f ? up_more : 1.0f;
    g_look_ty = !perspective || no_look || sun_held || map_still ? 0.0f
        : reach * up * kLookRangeY * (force_look ? forced[1] : stick_y);
    g_look_x += (g_look_tx - g_look_x) * kLookEase;
    g_look_y += (g_look_ty - g_look_y) * kLookEase;
    if (std::abs(g_look_x - g_look_tx) < 0.003f) g_look_x = g_look_tx;
    if (std::abs(g_look_y - g_look_ty) < 0.003f) g_look_y = g_look_ty;
    if (!perspective) g_look_x = g_look_y = 0.0f;
}

bool perspective_combat_camera() { return g_strike_t > 0.0f || g_strike_active; }
bool perspective_spell_camera() { return perspective_combat_camera() && g_spell_left > 0; }
bool perspective_spell_running() { return g_charge_left > 0 || g_spell_left > 0; }

void perspective_set_enabled(bool on) {
    gbarecomp::g_present_warp_provider = on ? &warp_provider : nullptr;
    if (!on) g_have_ref.store(false, std::memory_order_relaxed);
}

void perspective_set_flat(bool on) {
    if (on) gbarecomp::g_present_warp_provider = &flat_provider;
    else if (gbarecomp::g_present_warp_provider == &flat_provider) gbarecomp::g_present_warp_provider = nullptr;
}

bool perspective_flat_enabled() {
    return gbarecomp::g_present_warp_provider == &flat_provider;
}

void perspective_capture() {
    g_cam_cap.strike_t = g_strike_t;
    g_cam_cap.look_x = g_look_x;
    g_cam_cap.look_y = g_look_y;
}
float g_cam_cap_strike() { return g_cam_cap.strike_t; }
float g_cam_cap_look(int i) { return i ? g_cam_cap.look_y : g_cam_cap.look_x; }

bool perspective_enabled() {
    return gbarecomp::g_present_warp_provider == &warp_provider;
}

}  // namespace kol
