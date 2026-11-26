#pragma once

// kol_focus_internal.h — what the kol_focus files share (kol_focus.cpp and
// kol_focus_*.cpp; not for the rest of the game, which uses kol_focus.h):
// the grid and the guest addresses, the present's capture, the scene's
// per-pixel buffers and the rest of the state more than one file reads or
// writes (defined here, as inline variables, with their comments), the
// types and small helpers they share, and the declarations of the functions
// they call across files ("// (file)" names the file that defines them,
// where their comments are). Everything is in namespace kol_focus_detail,
// opened for the including file as the anonymous namespace it came from
// was. The per-map data is in kol_focus_tables.h.

#include "kol_focus.h"
#include "kol_focus_tables.h"
#include "kol_env.h"
#include "kol_hash.h"
#include "kol_oam.h"
#include "kol_map_settings.h"
#include "kol_rumble.h"
#include "kol_gulls.h"
#include "kol_highlight.h"
#include "kol_map_source.h"
#include "kol_perspective.h"
#include "kol_actions.h"
#include "kol_settings.h"
#include "kol_tile_edit.h"
#include "kol_widescreen.h"   // kol::ui_field

#include "runtime.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus(), active_ppu()
#include "gba_bus.h"
#include "gba_ppu.h"                // latched sprites (claim_unit_sprites)

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include "kol_region.h"

// What the files share stays inside the program (hidden), as the anonymous
// namespace it came from kept it: reached directly, with no GOT or PLT on the
// way (a shared variable is a weak definition, which on the Mac and in the
// Android library would otherwise be reached through the GOT).
#pragma GCC visibility push(hidden)

namespace kol_focus_detail {}
using namespace kol_focus_detail;

namespace kol_focus_detail {
constexpr std::uint32_t kGrid        = kol::usa(0x02016440u);
// The game's height grid is 32 x 32 cells (kGameSide); the field the
// perspective, the light and the editor work over has kMargin more cells on
// every side, -kMargin .. kGameSide + kMargin - 1 (kGridSide across), so
// cells can be given heights where a map's art goes on past the game's grid
// (kol_tile_edit). Cells out of the game's grid exist only when the editor
// gives them a height. Arrays over the field are indexed by gi(); the
// game's grid in EWRAM by game_cell().
constexpr int kGameSide = 32;
constexpr int kMargin = kol::kTileMargin;
constexpr int kGridSide = kGameSide + 2 * kMargin;
inline int gi(int cx, int cy) { return (cy + kMargin) * kGridSide + cx + kMargin; }
inline int cell_x(int i) { return i % kGridSide - kMargin; }
inline int cell_y(int i) { return i / kGridSide - kMargin; }
inline bool in_grid(int cx, int cy) {
    return cx >= -kMargin && cy >= -kMargin && cx < kGameSide + kMargin && cy < kGameSide + kMargin;
}
inline bool in_game(int cx, int cy) { return cx >= 0 && cy >= 0 && cx < kGameSide && cy < kGameSide; }
inline std::uint32_t game_cell(int cx, int cy) { return static_cast<std::uint32_t>((cy * kGameSide + cx) * 4); }
// World units of the field's first place along X and Z (negative).
constexpr int kFieldOrigin = -32 * kMargin;
constexpr std::uint32_t kTerrainMapPtr = kol::usa(0x020042E4u);   // map byte -> terrain record
// A/B switches read in more than one place (their notes are where they act).
inline bool blob_join_on() {   // KOL_NO_BLOB_JOIN=1: off
    static const bool v = !kol::env_set("KOL_NO_BLOB_JOIN");
    return v;
}

// (kol_focus.cpp)
bool cache_keep(bool key_repeats, bool prev_hit);
// A cache's counters (PassCache, RoofCache, Bg1Cache). With its _CHECK
// switch each check is counted, and every 60th prints them.
struct CacheStats {
    long frames = 0, hits = 0, checked = 0, mismatched = 0;
    void count_check(bool same, const char* name) {
        ++checked;
        if (!same) ++mismatched;
        if (checked % 60 == 0)
            std::fprintf(stderr, "kol-focus: %s cache check frames=%ld hits=%ld checked=%ld mismatched=%ld\n", name,
                         frames, hits, checked, mismatched);
    }
};
inline bool model_bg1_art_any() {   // KOL_MODEL_BG1_ART_ANY=1 (the model pass)
    static const bool v = kol::env_set("KOL_MODEL_BG1_ART_ANY");
    return v;
}
inline bool spire_flat_front() {   // KOL_SPIRE_FLAT_FRONT=1 (the cut-outs)
    static const bool v = kol::env_set("KOL_SPIRE_FLAT_FRONT");
    return v;
}
inline bool stalagmite_flat_card() {   // KOL_STALAGMITE_FLAT_CARD=1 (the cut-outs)
    static const bool v = kol::env_set("KOL_STALAGMITE_FLAT_CARD");
    return v;
}

// (kol_focus.cpp)
// The map number now (kMapNumber; the capture's in a present), -1 with no
// guest memory.
int current_map();
bool placed_plant(int index);
const DrawnCutout* drawn_cutout(int map, int record, int cx, int cz);
bool middle_v_cell(int map, int x, int z);
constexpr std::uint32_t kTerrainTable = kol::usa(0x0861CA5Cu);   // 0x20 a record; +1 0x7F: no unit enters
// The battle's map number: the u16 the map header parser 0x08021B68 reads
// (header = 0x085C55C4 + 2 + (number - 1) * 0x6A).
constexpr std::uint32_t kMapNumber = kol::usa(0x02029D40u);
// The u32 pointing at BG0's composed map (kol_map_source kArrayPtrAddr; BG1's
// follows it), as an EWRAM offset (Japan: 0x02026C28).
constexpr std::uint32_t kEwBg0Array = kol::usa(0x02026CA8u) - 0x02000000u;
constexpr std::uint32_t kObjectsPtr  = kol::usa(0x0202A238u);
constexpr int           kObjects     = 26;
constexpr std::uint32_t kObjStride   = 0xD0u;
constexpr std::uint32_t kSlotArray   = kol::usa(0x03004150u);   // IWRAM, stride 0x20
constexpr int           kSlots       = 128;
constexpr std::uint32_t kSlotStride  = 0x20u;
constexpr std::uint32_t kCursorSlotFn = kol::usa(0x08057AD5u);
constexpr int           kMaxProps    = 8;
constexpr int           kRow         = 32;   // depth of one row of cells
// Terrain ids whose height includes a prop standing on the ground. Seen on
// the snowfield (tree 0x07, bush 0x08, rock 0x0A); other maps may add more.
// Rock spires (0x09) are props too; KOL_PERSP_NO_SPIRE_PROPS=1 keeps them
// as blocks.
inline bool spire_props() {
    static const bool on = !kol::env_true("KOL_PERSP_NO_SPIRE_PROPS");
    return on;
}
inline bool is_prop(std::uint8_t terrain) {
    return terrain == 0x07 || terrain == 0x08 || terrain == 0x0A ||
           (spire_props() && terrain == 0x09);
}

// Each pixel's place: row (X + Z) and height (Y), world units.
inline std::vector<std::int32_t> g_xz;
inline std::vector<std::int32_t> g_y;
// What the pixel is (kol::kScene*): nothing (sky), a surface facing up, or
// something upright (walls, props, units).
inline std::vector<std::uint8_t> g_kind;
// Interface sprites (menus, icons): never blurred, never moved.
inline std::vector<std::uint8_t> g_iface;
// Interface pixels the perspective draws flat where their place has gone:
// the cursor's floating hand (see the cursor in build_scene_now). Their
// g_xz/g_y are the ground behind them, which is what the mesh shows there.
inline std::vector<std::uint8_t> g_floating;
inline bool g_floating_any = false;
// The grid cursor's corner pieces laid on their cell's face (1): scene
// pixels, but the light and shading leave them as drawn (range_mask marks
// them 2). Empty when the cursor drew none.
inline std::vector<std::uint8_t> g_cursor_face;
inline std::vector<std::uint8_t> g_ovl_card;   // see ScenePixels::ovl_card
inline bool g_ovl_card_any = false;
// Prop depth spreading (see the props pass): distance from a seeded pixel.
constexpr std::uint8_t kUnreached = 0xFF;
constexpr std::uint8_t kPropReach = 24;
inline std::vector<std::uint8_t> g_prop_dist;
// Flames the scene paints over the map (see flame_cards): 1 per pixel of one,
// each flame a prop of its own; empty when there are none.
inline std::vector<std::uint8_t> g_flame;
// Each flame's (and smoke column's) middle column and top row in the scene,
// from its tiles' place on the grid (flame_cards; its pixels' prop cell is
// -2 - its index): Special Effects stands its effect there, steady, where
// the bounds of the game's own pixels wander as it animates.
inline std::vector<float> g_flame_cx, g_flame_top;
// The placed flames' world places this frame (flame_cards; not the smoke),
// for their light (fire_lights).
struct FlameAt { float x, y, z; };
inline std::vector<FlameAt> g_flame_at;
inline bool is_flame(std::size_t k) { return k < g_flame.size() && g_flame[k] != 0; }
inline bool is_smoke(std::size_t k) { return k < g_flame.size() && g_flame[k] == 2; }
inline std::vector<std::uint8_t> g_covered;   // pixels the terrain pass drew
inline std::vector<std::int16_t> g_cell_owner;   // the cell that drew each pixel
// The pixels of a split cell's drawn span, its top and the wall behind it (DrawnCells::split_z):
// what an as_drawn split cell leaves as drawn (kol_focus_present range_mask), not its walls.
inline std::vector<std::uint8_t> g_span_px;
// A rounded box's wall pixels' own normal (kDrawnRounds, the editor's types), x127: X then Z
// a pixel, both 0 elsewhere. The lamps light such a pixel by it (lamp_and_window_light), not
// by its +X / +Z face. Written by the terrain pass (draw_round), kept with its arrays.
inline std::vector<std::int8_t> g_round_n;
inline std::vector<std::uint8_t> g_water;        // ScenePixels::water
inline std::vector<std::uint8_t> g_face;         // ... and which face: 0 top, 1 +X wall, 2 +Z wall
// Ambient occlusion, per pixel the terrain pass drew: 255 open, less where
// higher ground around shuts the light out (top_ao in the terrain pass).
// Options > Ambient Occl.: off, or a reach of half a tile or a whole one, at
// strength 0.7 (the user's pick from the mockups, shots/ambient_occlusion).
// For tests KOL_PERSP_AO=<strength 0..1> and KOL_PERSP_AO_R=<reach, world
// units> override it.
inline std::vector<std::uint8_t> g_ao;
// Torchlight: the ruins' torches (terrain record 70) are warm point lights
// near their tops, and the castle's statue with a blue flame (record 86) a
// cold one; each terrain pixel within reach takes their light by
// distance and by how its face turns to the flame, unless the height grid
// stands between them. Per pixel 0..255, applied in kol_perspective as an
// orange tint. Options > Torchlight, at strength 1.0 (the user's pick from
// shots/torch_glow); KOL_PERSP_GLOW=<strength> and KOL_PERSP_GLOW_R=<reach,
// world units, default 128> override it.
inline std::vector<std::uint8_t> g_glow;
inline bool g_glow_any = false;
// Each terrain pixel's world X and Z, kept while the torchlight is on (the
// light is worked out after the pass, on the pixels that stayed).
inline std::vector<std::int16_t> g_wx, g_wz;
// Whether the terrain hides a torch from each 4-unit cube around it (0 not
// known yet, 1 seen, 2 hidden), filled as pixels ask. The terrain does not
// change in a battle, so this lasts until the heights or the torches do.
struct LampVis {
    int ix0, iz0, nx, nz, ny;
    std::vector<std::uint8_t> vis;
    // What the grid was filled for (gather_lamps keeps a lamp's grid while
    // these hold): the map's heights and model, the lamp's place and reach.
    std::uint32_t base = 0;
    float at[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool kept = false;   // this picture's grid came from the last one's
};
inline std::vector<LampVis> g_lamp_vis;
inline std::uint32_t g_lamp_key = 0;
// The light is worked out again only when the view moves (or the torches
// change); between, only the flicker changes, applied per torch in
// kol_perspective: g_glow is the light without it, g_glow_lamp the torch
// most of a pixel's light comes from.
inline std::vector<std::uint8_t> g_glow_lamp;
// A second colour in a pixel's lamp light: the hue (LampHue) of the lamps that add most to
// it after its brightest lamp's own hue (g_glow_hue2), and that hue's share of the two hues'
// light, 0..255 (g_glow_mix; 0 none). kol_perspective's torch_tint blends the two tints by it.
inline std::vector<std::uint8_t> g_glow_hue2, g_glow_mix;
// The lamps that light units (kMapLamps) this frame, and the light on each
// unit's own pixels (0 none .. 255), with the lamp it is from, as g_glow
// is the map's.
struct UnitLamp { float x, y, z, reach, power, halo; int index; };
inline std::vector<UnitLamp> g_unit_lamps;
// A light of this frame (build_scene_now's lamps): a torch, a fire pot, a
// map's lamp (kMapLamps), a ceiling lamp, a doorway's light.
struct Lamp { float x, y, z, flick; std::uint8_t hue; float reach = 0.0f, power = 1.0f, halo = 0.0f; bool units = false;
              float lean_x = 0.0f, lean_y = 0.0f;
              // a cone: light_cone; a doorway: door_nearest; shadow_k < 1 longer shadows
              float dir_x = 0.0f, dir_z = 0.0f, cos_in = -2.0f, cos_out = -2.0f, half_w = 0.0f, shadow_k = 1.0f;
              float ring = 0.0f;   // > 0: a ring of flames this wide (radius), for soft shadows (lamp_seen)
              bool fire = false;   // a fire's (CeilingLamp::fire)
              float wob_x = 0.0f, wob_z = 0.0f;   // a fire's sway, for the units' shadows only (g_frame_lamps)
              float feather = 0.0f; };   // MapLamp::feather (lamp_fall)
inline std::vector<std::uint8_t> g_unit_glow, g_unit_glow_lamp;
inline bool g_unit_glow_any = false;
// On a dark map (kDarkMaps), the fixed lamps' light on each unit's own
// pixels (0 none .. 255, the flicker in), each pixel a point on the unit's
// card, so a ledge between a pot and a unit shades part of it, as the sun's
// shadows do (unit_card_lamp_light). Empty when not worked out.
inline std::vector<std::uint8_t> g_unit_lamp;
inline bool g_unit_lamp_any = false;
inline std::uint32_t g_glow_view_key = 0;
constexpr int kMaxLamps = 32;
inline float g_glow_flick[kMaxLamps];
// Each lamp's flame on screen (frame pixels) and its lean this frame: the
// pool's light a little stronger toward (lean_x, lean_y), a little weaker
// away (kol_perspective lamp_lean), as if the flame moved. 0: still.
inline float g_glow_fx[kMaxLamps], g_glow_fy[kMaxLamps], g_glow_lx[kMaxLamps], g_glow_ly[kMaxLamps];
inline std::uint8_t g_glow_hue[kMaxLamps];   // 0 the torches' orange, 1 blue

// (kol_focus.cpp)
float glow_strength();
float glow_reach();
float ao_strength();

// (kol_focus_shadows.cpp)
float sun_degrees();

// (kol_focus.cpp)
float weather_amount();
bool indoor_now();
bool scene_shadows_keep();

// (kol_focus_shadows.cpp)
float shadow_strength();

// (kol_focus.cpp)
// Debug > L2 Moves Sun carries it along an arc over the battlefield: up
// from the pick's side of the horizon, overhead, down to the opposite side.
// Its place on the arc, degrees from the pick's horizon, is the pick's
// height plus sun_arc_now(); past 90 it shines from the other side.
float sun_arc_now();

// (kol_focus.cpp)
// The debug turn of the direction along the ground, degrees clockwise on
// screen (L2 + the stick's up / down).
float sun_turn_now();
const std::uint8_t* guest_ew();
// Where the rain rises (map_settings.cfg `rain_rises`, map 57) and the game's
// rain is on: the sun circles low (kol_focus_shadows). True then, with the
// turn (degrees) it has made in *turn.
bool circling_sun(float* turn);
bool map_entry_on(int map);
const EveningRoom* evening_room();   // kol_focus.cpp
const DarkMap* dark_map();
const DarkPool* dark_pool();
const DarkCover* dark_cover();
const std::int32_t* lamp_palette_lut(int map);
const std::int32_t* lamp_palette_lut_live(int map, const std::uint8_t* pal);
float lamp_palette_from();
float sun_bump_strength();
bool map_wet_floor();
// What a scene object standing in its plane covers of that plane (map 67's
// barred door): its opaque texels at the plane's (Z, Y), 1-unit cells, each
// texel 2 x 2 of them. A window in the same plane (on_x, plane_z ==
// plane_x) is shut where it is: the light through the door falls between
// its bars (window_aperture). Built with the sprite atlas, so the window
// light of the next picture uses it; `hash` keys the light's caches.
struct PlaneMask {
    int map = -1;
    float plane = 0.0f;
    int z0 = 0, y0 = 0, nz = 0, ny = 0;
    std::vector<std::uint8_t> m;
    std::uint32_t hash = 0;
    float cover(float z, float y) const {   // 0..1, bilinear between cell centres
        if (m.empty()) return 0.0f;
        const float fz = z - z0 - 0.5f, fy = y - y0 - 0.5f;
        const int iz = static_cast<int>(std::floor(fz)), iy = static_cast<int>(std::floor(fy));
        const float az = fz - iz, ay = fy - iy;
        auto at = [&](int a, int b) {
            return a < 0 || b < 0 || a >= nz || b >= ny ? 0.0f : static_cast<float>(m[static_cast<std::size_t>(b) * nz + a]);
        };
        return (1 - ay) * ((1 - az) * at(iz, iy) + az * at(iz + 1, iy)) + ay * ((1 - az) * at(iz, iy + 1) + az * at(iz + 1, iy + 1));
    }
    // Softened over about r units (the centre and four spots r away).
    float cover_soft(float z, float y, float r) const {
        if (r < 0.25f) return cover(z, y);
        return 0.2f * (cover(z, y) + cover(z - r, y - r) + cover(z + r, y - r) + cover(z - r, y + r) + cover(z + r, y + r));
    }
};
// The bars' shadow softer the further from the door it falls, as a light
// that is not a point casts it: r = kPlaneSoft0 + kPlaneSoftK x the way
// past the plane (the user asked for a slightly softer door shadow).
// KOL_PLANE_SOFT=<r0,k> (mockups).
inline float plane_soft(float t) {
    static float v[2] = {0.6f, 0.03f};
    static const bool read = [] {
        if (const char* e = std::getenv("KOL_PLANE_SOFT")) std::sscanf(e, "%f,%f", &v[0], &v[1]);
        return true;
    }();
    (void)read;
    return v[0] + v[1] * t;
}
inline PlaneMask g_plane_mask;
// The tiles of a scene object's painted shadow our light replaces, for the
// compositor (kol::object_decal_hidden): map << 22 | lo << 11 | hi, 0 none.
inline std::atomic<std::uint32_t> g_hidden_decals{0};

// (kol_focus_windows.cpp)
bool map_has_windows();
bool window_room_plain();
bool dark_windows();

// (kol_focus.cpp)
struct Unit;
const SceneObjects* scene_object_of(const Unit& u);
bool scene_object(const Unit& u);
bool scene_object_in_wall(const Unit& u);
const MapSunset* map_sunset();
const SceneSunset* scene_sunset();
float scene_sunset_now();   // 0..1 the way along, -1 none (the capture's in a present)
float scene_sunset_colour();

// (kol_focus_windows.cpp)
const float* map_window_tint(int map);
// The set of windows being built (MapWindow::own_deg): 0 the map's light,
// 1 those with their own; and the second set's light height (its tangent).
inline int g_win_set = 0;
inline float g_win_set_rise = 0.0f;
constexpr float kMoonTint[3] = {0.55f, 0.7f, 1.0f};   // map 65's moonlight

// (kol_focus_windows.cpp)
const std::vector<MapWindow>& map_windows_all();
float godray_strength();
// Per scene pixel: the sun through a window on it (0..255), and the haze in
// front of it (0..255).
inline std::vector<std::uint8_t> g_win_patch, g_win_haze;
// The second set's (MapWindow::own_deg: map 67's moonlight), and its strength.
inline std::vector<std::uint8_t> g_moon_patch, g_moon_haze;
inline std::vector<std::uint8_t> g_moon_patch_built;   // g_moon_patch before the units' shadows
inline bool g_moon_any = false;
inline float g_moon_gain = 0.0f;
// Its light's way (toward it along the ground) and rise, when the map has
// such a window (g_moon_on); its beams, kept between builds; and its light
// on the units' own pixels (unit_window_light run as the second set), as
// g_unit_win_* are the main set's.
inline bool g_moon_on = false;
inline float g_moon_dx = 0.0f, g_moon_dz = 0.0f, g_moon_rise = 0.0f;
inline std::vector<std::uint8_t> g_moon_unit_patch, g_moon_unit_haze, g_moon_unit_patch_a, g_moon_unit_haze_a;
inline bool g_moon_unit_any = false;
inline float g_win_haze_k = 0.05f;   // the beams' haze, the map's (map_window_haze), set as they are built
inline bool g_win_any = false;
// The window light as built for the view, kept: lift_window_light reworks
// g_win_patch / g_win_haze at the lifted pixels every frame (the light moves
// to the lifted texels, the surface behind gets its own), and the view's
// light is only rebuilt when its key changes. Reworked in place, the next
// frame's lifting took the light of the surface behind for the lifted
// pieces: the lifted part of a lit ledge went dark, and right again for one
// frame after each camera move (a flicker; map 55, tile 14,4, GX, asked
// 2026-10-15; since the godrays, 10-06). Each frame starts from this copy.
inline std::vector<std::uint8_t> g_win_patch_built, g_win_haze_built;
inline bool g_win_any_built = false;
// Per scene pixel, how much of the map's own colour it keeps, 255 all
// (g_edge_fade_on false: none this frame).
inline std::vector<std::uint8_t> g_edge_fade;
inline bool g_edge_fade_on = false;
inline int g_sky = kol::kSkyGame;   // the backdrop's ramp (kEdgeFadeMaps)
inline float g_sky_desat = 0.0f;
struct Capture;

constexpr float kArcLowest = 5.0f, kArcHighest = 175.0f;

// (kol_focus_shadows.cpp)
int battle_turn();
float sun_turn_drop();

// (kol_focus.cpp)
float evening_amount();
std::uint32_t g_cap_serial();

// (kol_focus_windows.cpp)
const WindowTorch* window_torch();
void window_torch_sway(const WindowTorch& w, float& a, float& b, float& f);
bool map_cone_windows();

// (kol_focus_shadows.cpp)
float sun_degrees_base();
float sun_arc_angle();
float shadow_rise();
void shadow_dir_pick(float& lx, float& lz);
void shadow_dir(float& lx, float& lz);
std::uint32_t sun_key(std::uint32_t key, float rise, float lx, float lz);
// The debug arc (degrees), the guest's thread's; the present path reads the
// capture's copy.
inline float g_sun_arc = 0.0f;
// The same for the turn; KOL_PERSP_SUN_TURN=<degrees> starts it there
// (with L2 Moves Sun on: off resets it), for headless checks.
inline float g_sun_turn = kol::env_num("KOL_PERSP_SUN_TURN", 0.0f);
// How high the shade stands over each spot of the ground: 8 x 8 spots a
// cell, the world height below which a point standing over that spot is in
// the shadow of the terrain toward the sun (kNoShade when none). A top face
// asks with its own height, a wall facing the sun with the height of its
// pixel over the spot just in front of it. The heights do not change in a
// battle, so it is worked out again only when they (or the sun) do.
constexpr int kShadowSub = 8;
constexpr std::int16_t kNoShade = -32768;
inline std::vector<std::int16_t> g_shadow;
// With it, for each place, how far toward the sun the terrain that sets its
// shadow stands (world units along the ray): the soft edge's blur grows
// with it (see shade_frac in build_scene_now).
inline std::vector<std::uint16_t> g_shadow_dist;
// And the places near an edge of the shadow on the ground (within the
// softest blur): only there does the soft edge sample around a point.
inline std::vector<std::uint8_t> g_shadow_edge;
// The terrain shadow's soft edge: the blur's radius, world units per unit
// of distance to the terrain that casts it (the user's pick, 0.11, from
// shots/soft_shadows/terrain_*). KOL_TERRAIN_SOFT=<rate>; 0: hard.
// The blur's radius at most (world units): half a cell.
constexpr float kTerrainSoftMax = 16.0f;

// (kol_focus_shadows.cpp)
float terrain_soft();
// The places it covers: the field's cells that exist (the game's grid, and
// beyond it the cells given a height), from place (g_shadow_x0, g_shadow_z0)
// of the field, g_shadow_w x g_shadow_h of them.
inline int g_shadow_x0 = 0, g_shadow_z0 = 0, g_shadow_w = 0, g_shadow_h = 0;
inline std::uint32_t g_shadow_key = 0;
inline bool g_shadow_on = false;   // g_shadow is this frame's (shadows on)
// The place over world (X, Z) in those maps (g_shadow's, the canopy's), -1
// off them or with the shadows off.
inline std::ptrdiff_t shadow_place(int X, int Z) {
    if (!g_shadow_on) return -1;
    if (X < kFieldOrigin || Z < kFieldOrigin) return -1;
    const int sx = (X - kFieldOrigin) * kShadowSub / 32 - g_shadow_x0;
    const int sz = (Z - kFieldOrigin) * kShadowSub / 32 - g_shadow_z0;
    if (sx < 0 || sz < 0 || sx >= g_shadow_w || sz >= g_shadow_h) return -1;
    return static_cast<std::ptrdiff_t>(sz) * g_shadow_w + sx;
}
// The height below which a point over world (X, Z) is in the terrain's
// shadow, kNoShade for none (or with the shadows off).
inline int shade_height(int X, int Z) {
    if (g_shadow.size() != static_cast<std::size_t>(g_shadow_w) * g_shadow_h) return kNoShade;
    const std::ptrdiff_t i = shadow_place(X, Z);
    return i < 0 ? kNoShade : g_shadow[static_cast<std::size_t>(i)];
}

// Open roofs (awnings, kol_tile_edit's `open`) float: the sun passes under
// their edges, so they are not in g_shadow's one height per place. For each
// place, the heights between which a point is in an awning's shadow: its ray
// toward the sun is under the awning at one step and over it at another, so
// it goes through it (kNoShade where no ray meets one). Empty with no open
// roof on the map.
inline std::vector<std::int16_t> g_canopy_lo, g_canopy_hi;

// (kol_focus_shadows.cpp)
float awning_skirt();
float awning_overhang();
float awning_sure();
inline bool canopy_range(int X, int Z, int* lo, int* hi) {
    if (g_canopy_hi.size() != static_cast<std::size_t>(g_shadow_w) * g_shadow_h) return false;
    const std::ptrdiff_t at = shadow_place(X, Z);
    if (at < 0) return false;
    const std::size_t i = static_cast<std::size_t>(at);
    if (g_canopy_hi[i] == kNoShade) return false;
    *lo = g_canopy_lo[i];
    *hi = g_canopy_hi[i];
    return true;
}

// (kol_focus_shadows.cpp)
float card_shade_row(int X, int Z, int foot, float foot_row);
void prop_base_params(float* rows, float* dark, float* raise);
bool props_shaded();
inline bool canopy_shaded(int X, int Z, float y) {
    int lo = 0, hi = 0;
    return canopy_range(X, Z, &lo, &hi) && y >= static_cast<float>(lo) && y + 0.5f < static_cast<float>(hi);
}
inline std::vector<std::uint32_t> g_queue;

inline std::uint32_t rd32(const std::uint8_t* m, std::uint32_t off) {
    return static_cast<std::uint32_t>(m[off]) | (m[off + 1] << 8) |
           (m[off + 2] << 16) | (static_cast<std::uint32_t>(m[off + 3]) << 24);
}
inline int rd16(const std::uint8_t* m, std::uint32_t off) {
    return static_cast<std::int16_t>(m[off] | (m[off + 1] << 8));
}
inline bool in_ewram(std::uint32_t a, std::uint32_t len) {
    return a >= 0x02000000u && a + len <= 0x02040000u;
}

// ── The present's capture ────────────────────────────────────────────────
// The present-time providers (scene_pixels for the perspective's warp, the
// focus provider) may run on gbarecomp's present worker, a frame behind the
// guest (runtime.h, g_present_pipeline). They read nothing live: at present,
// on the guest's thread, kol::present_capture() copies what they read (EWRAM,
// IWRAM, the PPU's latched records of the frame, the frame hook's scroll and
// view), and the present path reads the copy, inline or on the worker alike.
// The guest-state queries (kol_focus_queries.cpp: cursor_on_map...) serve
// both: on the present path (t_present, kol::PresentScope) they answer from
// the capture, elsewhere (the sheet, the sound) from live memory.
// A colour that gives a spell's light: bright and saturated, its brightest
// channel at 120 or more and saturation 0.3 or more (an element's orb, green
// earth's (47, 145, 34) and fire's red (186, 63, 63) among them; not the
// white of a damage number or the grey of dust).
inline bool spell_light_colour(unsigned r, unsigned g, unsigned b) {
    const unsigned mx = std::max({r, g, b}), mn = std::min({r, g, b});
    return mx >= 120u && (mx - mn) * 10u >= mx * 3u;
}

struct Capture {
    std::vector<std::uint8_t> ew, iw;                 // all of each
    std::vector<std::uint8_t> oam, obj_tiles, pal;   // the latched sprites (obj_ok)
    // Where the compositor drew each OAM entry (GbaPpu::latched_obj_drawn):
    // the panel's portraits, a menu's hand, moved from their OAM place.
    std::array<std::int16_t, 128> obj_x{}, obj_y{};
    std::array<bool, 128> obj_drawn{};
    std::vector<std::uint8_t> under, codes;          // the scene under sprites (under_ok)
    std::vector<std::uint8_t> row_backdrop;          // each scene row's backdrop (gbarecomp latched_row_backdrop)
    bool obj_ok = false, under_ok = false, bg0_ok = false;
    unsigned bg0_h = 0, bg0_v = 0;                   // the picture's BG0 scroll
    // ... BG1's, and whether it is shown (DISPCNT bit 9), from the latched IO.
    int bg1_h = 0, bg1_v = 0;
    bool bg1_on = true, bg1_ok = false;
    int scene_w = 0, scene_h = 0;                    // the latched scene's size
    std::uint16_t dispcnt = 0, bldcnt = 0, bldalpha = 0, bldy = 0;
    std::uint16_t winin = 0, winout = 0;            // the latched frame's WININ, WINOUT
    int view[4] = {0, 0, 0, 0};                      // g_present_frame_view
    int hofs = 0, vofs = 0;                          // focus_note_scroll's
    bool scroll_valid = false;
    unsigned long serial = 0;
    unsigned exclude_layers = 0;                     // g_ppu_scene_exclude_layers
    // BG2 was in the scene as a piece of the world (kol_widescreen's
    // world_bg2_frame; map 84's cliff fading into its stairs), and how much
    // of it shows in sixteenths (16: opaque, no blend yet).
    bool world_bg2 = false;
    int world_bg2_eva = 16;
    // BG2 is the sea's field (map 117's water, kol_widescreen sea_bg2_frame):
    // 1 where the scene's top was that water, 2 where it was under the top
    // (a rope, the wake), before it was read as BG0.
    std::vector<std::uint8_t> sea_px;
    bool perspective = false;                        // kol::perspective_enabled()
    bool flat = false;                               // kol::perspective_flat_enabled()
    bool combat_camera = false;                      // kol::perspective_combat_camera()
    bool spell_camera = false;                       // kol::perspective_spell_camera()
    bool spell_running = false;                      // kol::perspective_spell_running()
    float sun_arc = 0.0f;                            // g_sun_arc
    float sun_turn = 0.0f;                           // g_sun_turn
    bool weather = false;                            // g_weather_hold > 0
    float weather_amt = 0.0f;                        // weather eased in and out (weather_amount)
    // The range's tiles (kol_highlight's world cells wearing the highlight)
    // and the screen-to-cell mapping, per terrain layer; range_ok false: none.
    std::vector<std::uint64_t> range_lit[2];
    int range_rows[2] = {0, 0}, range_map[2][4] = {};
    bool range_ok[2] = {false, false};
    bool indoor = false;                             // map_indoor() at capture
    kol::MapExtent extent{};                         // the composed map's art
    bool extent_ok = false;
    // BG1's tiles (4bpp, from its char base) on the maps whose BG1 is props
    // (kBg1PropMaps): a prop past the picture's edge is read from the
    // composed map with them. Empty elsewhere.
    std::vector<std::uint8_t> bg1_chars;
    float scene_sunset = -1.0f;                      // g_scene_sunset_live at capture
    float rain = 0.0f;                               // rain_of(ew) at capture
    int rain_level = -1;                             // rain_level_of(ew) at capture
    int snow_level = -1;                             // snow_level_of(ew) at capture
    bool story = false;                              // story_event() at capture (EWRAM 0x020013D0 == 1)
    int map = -1;                                    // the map number at capture
    unsigned map_loads = 0;                          // kol::map_loads() at capture
    float scene_storm = 0.0f;                        // g_scene_storm_live at capture
    bool palette_lag = false;                        // a scene sunset's palette lag (see below)
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    int ui_field = -1;                               // kol::ui_field at capture
    std::uint8_t ui_field_rgb[3] = {0, 0, 0};
    // A spell's effect drawn on BG2 (spell_lights): its pixels that may give
    // light, frame coordinates, every other one each way.
    struct SpellSample { std::int16_t x, y; std::uint8_t r, g, b; };
    std::vector<SpellSample> spell_bg;
    std::vector<kol::ScenePixels::HudGlowPx> hud_glow;   // ScenePixels::hud_glow
    // Light-layer pixels over BG1 in a battle action, taken out of hud_glow:
    // the scene index (latched scene, scene_w x scene_h) and the frame's
    // colour there (kol::hud_bake_into).
    struct HudBake { std::uint32_t k; std::uint8_t rgb[3]; };
    std::vector<HudBake> hud_bake;
    unsigned hud_glow_layers = 0;
    bool hud_glow_apex_ok = false;
    bool hud_glow_add = false;   // ScenePixels::hud_glow_add
    int hud_glow_eva = 16;
    float hud_glow_apex[2] = {0.0f, 0.0f};
    kol::ScenePixels::WindowLight window_light;          // ScenePixels::window_light
    // A spell's circle the game draws on BG2 (map_settings.cfg `magic_circle`,
    // map 57's summoning circle; see circle_capture): BG2's 256 x 256
    // tilemap, 1 where it draws; frame pixel (x, y) shows its texel (x +
    // circle_u, y + circle_v); the colour it adds (its palette entry x EVA /
    // 16, the blend's EVB 16: added); its texels' box. circle_on false: none.
    std::vector<std::uint8_t> circle;
    int circle_u = 0, circle_v = 0;
    int circle_box[4] = {0, 0, 0, 0};
    std::uint8_t circle_rgb[3] = {0, 0, 0};
    int circle_eva = 0, circle_evb = 16;
    bool circle_on = false;
    // A spell's closing circle this frame (kol::spell_ring), native pixels.
    bool ring_on = false;
    float ring[3] = {0.0f, 0.0f, 0.0f};
    int bg23_v[2] = {0, 0};   // BG2's and BG3's vertical offsets, as the frame was drawn
    // A blue flame on BG1 in the frame (map_settings.cfg `glow_flame`: 40 or
    // more of its pixels, kol_perspective's glow_flame_px test): its lamp
    // (CeilingLamp::flame_shown) is lit.
    bool bg1_flame = false;
    bool valid = false;
};
inline Capture g_cap;
inline thread_local bool t_present = false;
// Steps of a scene's sunset ahead of now, for sun_degrees_base (the shadow
// map built ahead, ShadowAhead in kol_focus.cpp); 0 but there.
inline thread_local int t_sun_ahead_steps = 0;

// (kol_focus.cpp)
int scroll_lead(int now, int then);
float indoor_dark();
float indoor_lamp_gain();
const std::uint8_t* guest_iw();

struct Unit {
    int x, y, z, depth;   // depth: draw_depth, for draw order
    float fade = 1.0f;    // its card's opacity (a scene actor stepping out of a doorway: gulls_unit_fade)
    // Its sprite's tile block: the actor slot (object +0x08) of the game's
    // sprite pool, whose VRAM callback 0x08059000 gives slots 0-15 the 16
    // tiles from slot * 0x10 (claim_unit_sprites); -1: none such.
    int block = -1;
    // Its unit record's +0x4E: the scene's actor (a lamp's carrier,
    // kMapLamps); -1: not read.
    int actor = -1;
    // False: its feet are in the view and the game draws no sprite there this
    // frame (a scene's unit not on stage yet). It carries no lamp (kMapLamps).
    bool shown = true;
};
// A unit's feet on the screen, before the camera (callers add kx, then ox;
// ky, then oy): (X - Z) / 2 across, (X + Z) / 4 - (Y - 8) / 2 down, divided
// as integers.
inline int feet_sx(const Unit& u) { return (u.x - u.z) / 2; }
inline int feet_sy(const Unit& u) { return (u.x + u.z) / 4 - (u.y - 8) / 2; }

// Units high over the ground under them (kSkyRise: more than 8 steps, far
// over any flier's hover) and high in the world (over kSkyY: the ship's
// banners, units hanging from its yard 136-166 high, are over the grid's
// water): the gulls crossing the harbour in the opening of
// map 97's boat scene (states/HJ), 290-345 high over the water. Their parts
// are drawn flat, as interface: as cards they flickered (spread wings are
// 40 wide, no 32 x 32 slot: card and flat by turns) and went to a sailor on
// the quay whose feet project where theirs do (the user, 2026-10-16).
// Set by build_scene_now for claim_unit_sprites. KOL_NO_SKY_UNITS=1: off.
constexpr int kSkyRise = 128, kSkyY = 256;
inline bool g_unit_sky[64] = {};
// Units over kHangRise above the ground under them and not in the sky: the
// boat's flag (map 58, 168 high over cells at 2): nothing under their feet
// to stand on (no contact shade there). A flier with its blob drawn has its
// ground (g_unit_ground) and keeps it.
constexpr int kHangRise = 96;
inline bool g_unit_hanging[64] = {};
// Unit u (index as in g_unit_sky) a bird: over kHangRise above the ground
// under it and high in the world (over kSkyY: the gulls round the sail on
// map 117 fly 290-330 high over the boat; the ship's banners hang 136-166,
// a unit on the highest cells stands at most 320 but on them): no blob of
// its own, casting no shadow (build_sprite_atlas). KOL_HIGH_UNITS_CAST=1:
// as any unit.
inline bool flies_high(int u, const Unit& unit) {
    static const bool off = std::getenv("KOL_HIGH_UNITS_CAST") != nullptr;
    return !off && u >= 0 && u < 64 && (g_unit_sky[u] || g_unit_hanging[u]) && unit.y > kSkyY;
}

// The order KoL draws its units' sprites in, front to back: by X + Z, and
// between two level in it the higher first. Taken for X + Z + Y until
// 2026-10-14: Allos three steps up the ruins (map 51, Y 128) a cell behind
// the unit drawn just before him (Y 80) counted as in front of it, his parts
// broke the run's order and went to interface, so he took none of the
// map's darkness (states/GS_allos_bright_in_shadow). Over ten states' OAM
// (KOL_OAM_DEBUG) X + Z held for every pair of units, the four ties all
// higher first. KOL_DEPTH_XYZ=1: the old key (A/B runs).
constexpr int kDepthPerXZ = 1024;   // Y below this: only a tie-break
inline bool depth_xyz() {
    static const bool v = kol::env_set("KOL_DEPTH_XYZ");
    return v;
}
inline int draw_depth(int x, int y, int z) { return depth_xyz() ? x + z + y : (x + z) * kDepthPerXZ + y; }

// The combat camera's sprite shapes (see build_scene_now).
inline std::vector<std::uint8_t> g_shape;
inline std::vector<std::uint32_t> g_shape_piece;   // one piece of interface in a shape
inline std::vector<std::uint32_t> g_shape_members;
inline std::vector<std::uint32_t> g_shape_front;
// Interface pixels that joined a unit for the combat camera: they move with
// it but stay sharp, as interface always is. Only as mesh pixels (no atlas,
// or KOL_PERSP_OVERLAY_MESH=1): sized and cleared only when one is set
// (g_sharp_any), its readers test the flag.
inline std::vector<std::uint8_t> g_sharp;
inline bool g_sharp_any = false;

// The layer on top of a pixel of the scene frame. With the perspective the
// scene is composed without the HUD (gba::g_ppu_scene_exclude_layers, drawn
// flat over it), so where the HUD covers a unit the scene shows the unit.
// The "5 STEP" bubble over a knight's sword (CB_sprites_broken) left the
// sword out of the unit and behind in the scene, and under the unit bar
// (BK_tree_glass_cage) whole units stayed behind. gbarecomp records the
// scene's own codes with the ground under sprites (g_scene_codes); without
// them the frame's code tells only when one HUD layer is over the sprite.
// Without the perspective nothing is excluded and this is the frame's top.
inline const std::uint8_t* g_scene_codes = nullptr;
inline unsigned scene_top(const std::uint8_t* layers, std::size_t k) {
    if (g_scene_codes) return g_scene_codes[k] & 0x0Fu;
    const std::uint8_t code = layers[k];
    const unsigned top = code & 0x0Fu;
    return top < 4u && ((g_cap.exclude_layers >> top) & 1u) ? code >> 4 : top;
}
// The ground's layer at a scene pixel: scene_top's, under a sprite (4) the
// layer below it (the scene's own codes). The roofs', the boxes' and the
// model's passes take a pixel by it.
inline unsigned ground_layer(const std::uint8_t* layers, std::size_t k) {
    const unsigned t = scene_top(layers, k);
    return t == 4u && g_scene_codes ? static_cast<unsigned>(g_scene_codes[k] >> 4) : t;
}

// Which OAM entry drew each sprite pixel, and the unit that owns it (see
// claim_unit_sprites).
inline std::vector<std::int16_t> g_obj_top;
inline std::vector<std::uint8_t> g_obj_prio;

// The grid cursor's corner pieces among those entries (claim_unit_sprites),
// by index: build_scene_now lays these on the cell and floats the rest.
inline bool g_cursor_piece[128];
// The weather's particles among them: the rain's drops (tiles 0x164, 0x165,
// semi-transparent) and the snow's flakes (0x166-0x168, plain), interface by
// their place in OAM but neither the cursor's corner pieces nor its hand.
inline bool g_weather_piece[128];

// (kol_focus_sprites.cpp)
bool weather_tile(std::uint16_t a2);
// Per scene pixel: drawn by a weather particle (weather_tile's 8x8s), from the
// latched OAM itself, so it is known with no unit on the map too (view mode:
// claim_unit_sprites does not run, and g_obj_top is not this frame's).
inline std::vector<std::uint8_t> g_weather_px;

// (kol_focus_sprites.cpp)
void build_weather_px(const std::uint8_t* layers, int w, int h, int ox, int oy);
inline bool g_obj_top_valid = false;   // g_obj_top is this frame's

// KOL_PERSP_ONMAP_ONLY=1: the rebuild takes only cells a unit may enter.
inline const bool g_onmap_only = kol::env_true("KOL_PERSP_ONMAP_ONLY");

// The latched OAM's plain entries, as claim_unit_sprites read them (in OAM
// order, so index order is draw order), for the sprite atlas below.
struct ObjEntry { int x, y, w, h, owner; std::uint16_t a0, a1, a2; int oam = -1; };
// Per cell: a spire whose card sorts past its front neighbours' floor (see
// the spire rule); reset every frame.
inline std::vector<std::uint8_t> g_spire_past;
// Per cell: the scene column of a card's V tip and the tip's depth past the
// card's place, the card bent back along the cell's front edges from there
// (SpriteBlock::bend_sx, bend_tip): a spire's (at its front corner, tip 0)
// and a bush's (in the middle, tip 32); sx -1 a flat card. Reset every frame.
// dyl, dyr: height units added to the card's ground for the columns left
// and right of the tip (a V whose arms stand on floors a step apart:
// kVStepCutouts).
struct CardBend { int sx = -1, tip = 0, dyl = 0, dyr = 0; };
inline std::vector<CardBend> g_card_bend;
// Per cell: a pine's tip, its art's middle column and top row (sx, y0 by its
// stored top), for sharing out the pixels two pines' strips both hold; x
// INT_MIN not a pine. Reset every frame.
inline std::vector<std::pair<int, int>> g_pine_tip;
// Per scene pixel: in a spire's V under its strip (the V's BG0 too: lifted
// with the card, see lift_prop_pixels); reset every frame.
inline std::vector<std::uint8_t> g_spire_v;
// Per scene pixel: copied onto a card (its block + 1) and kept where it is
// in the scene too: a spire's V of BG0 (lifted, it left the backdrop, black,
// where it was: under BG0 there is nothing).
inline std::vector<std::uint8_t> g_lift_copy;
// Pixels Lifted Walls leaves in the scene: the pedestal under a cut-out on
// its block's top (kTopStandRecords), its BG1 half lifted as a wall square
// showed the carpet's refill through it with the camera turned (map 39).
inline std::vector<std::uint8_t> g_no_wall_lift;
// The ground's place under each sprite pixel (see build_scene_now).
inline std::vector<std::int32_t> g_under_xz, g_under_y;
inline std::vector<std::uint8_t> g_under_kind;
inline ObjEntry g_entries[128];
inline int g_entry_n = 0;
// The game's shadow blob under a unit is a pair of 8 x 8 parts (tiles
// 0x14B and 0x14C). On stairs it is drawn on the ground below the unit, out
// of its frame, and fit no unit on some frames: it went to interface and
// was drawn flat over the girl walking down them, a black oval at her waist
// every other frame (map 89's opening, states/FE_slot1_girl_flicker). Such a
// part now belongs to the unit drawn just before it, as its blob: it lies
// on the ground, and with Unit Shadows on it gives way to the ground as the
// blob in the unit's frame does (g_blob_of: that unit, -1 none).
// A flying unit's blob is a pair of 16 x 8 parts (tiles 0x153 and 0x155),
// drawn on the ground under it, 12 to 64 rows below its feet (the griffin
// over the snowfield, states/FF_slot1_flying_shadow_guard): it too belongs
// to that unit, and gives the ground under it (g_unit_ground, the height
// its occlusion spot lies at).
inline int g_blob_of[128];
// A unit up to this many rows tall, at most a slot wide, is two cards one
// over the other (its top rows, then the slot over its feet), both at its
// place: the boat's flag and its castle door on map 58 (and 97) are 32 x
// 40. Taller, or wider, it stays in the frame (flat).
constexpr int kTallCard = 48;
// The most a unit's figure may span joined (a rider on his horse): split
// into slots by the atlas (kol_focus_sprites join_parts).
constexpr int kBigCard = 64;

// (kol_focus_sprites.cpp)
bool blob_tile(const ObjEntry& e);
constexpr int kNoGround = -100000;
inline std::vector<int> g_unit_ground;   // by unit: the ground under a flying unit, else kNoGround
inline bool g_map_1d = true;

// The palette index of an entry's texel (tx, ty) from its top-left, flips
// applied; 0 is transparent.
inline unsigned obj_texel(const ObjEntry& e, int tx, int ty, const std::uint8_t* tiles, bool map_1d) {
    const bool bpp8 = (e.a0 & 0x2000u) != 0u;
    const int sx = (e.a1 & 0x1000u) ? e.w - 1 - tx : tx;
    const int sy = (e.a1 & 0x2000u) ? e.h - 1 - ty : ty;
    const unsigned row_tiles = map_1d ? static_cast<unsigned>(e.w / 8) * (bpp8 ? 2u : 1u) : 32u;
    return kol::obj_tile_texel(tiles, e.a2 & 0x3FFu, row_tiles, sx, sy, bpp8);
}

// (kol_focus_sprites.cpp)
void claim_unit_sprites(const std::uint8_t* layers, int w, int h, int ox, int oy,
                        int kx, int ky, const Unit* units, int count, int props,
                        bool cursor, int cursor_hx, int cursor_hy,
                        std::int32_t* xz_out, std::int32_t* y_out,
                        std::uint8_t* kind_out, std::uint8_t* iface_out);

// The units lifted into the atlas (kol_focus_atlas.cpp, "Units from their
// own sprites").
inline std::vector<std::uint8_t> g_lift;
// Scene pixels a background layer drew over a sprite's texel by priority
// (ScenePixels::over_sprite): the card keeps the texel, the warp draws these
// pixels again after the cards.
inline std::vector<std::uint8_t> g_over_sprite;
inline std::vector<std::uint8_t> g_over_block;   // ... whose card it covers (its block + 1, as g_lift)
inline bool g_over_sprite_any = false;
inline std::vector<kol::ScenePixels::SpriteBlock> g_blocks;
inline std::vector<std::uint8_t> g_atlas_rgb, g_atlas_opaque, g_atlas_blur;
inline int g_atlas_w = 0, g_atlas_h = 0, g_atlas_per_row = 0;
inline bool g_atlas_on = false;

// (kol_focus_atlas.cpp)
int atlas_new_square(int* ax0, int* ay0, std::uint8_t opaque, bool clear_blur = false);
// A scene object standing in a wall (kSceneObjects, not hanging: map 58's
// door, not its flag) is painted on the wall as a texture of the mesh, not
// stood up as a card: its pixels stay in the scene at the wall's place under
// them and are lit as the map's (ScenePixels::wall_px). As a card its square
// was lit by itself and stood out from the wall round it (the user,
// 2026-10-27, saw a cut-out round the door, states/JM ~860).
// KOL_SCENE_OBJECT_CARD=1: a card, as before.
inline std::vector<std::uint8_t> g_wall_px;
inline bool g_wall_px_any = false;
// A door in its wall with units in front (SceneObjects::units_in_front;
// g_wall_px 2): its feet's X + Z. A unit standing behind that (inside the
// church) stays hidden by it, as the game draws it; one at the door's place
// or before it is drawn over it.
inline int g_wall_front_xz = 0;
// A sea map's foam sprites laid on the sea (kol_focus_sprites, the blob's
// path): lit as the map, as wall_px is (ScenePixels::wall_px takes both).
inline std::vector<std::uint8_t> g_sea_decal_px;
inline bool g_sea_decal_any = false;
inline std::vector<kol::ScenePixels::SeaFoamPx> g_sea_foam;   // (ScenePixels::sea_foam)
// This frame's sprite atlas gave up (an effect it cannot reproduce): the
// units stay in the frame and nothing is lifted.
inline bool g_atlas_bailed = false;

// (kol_focus_atlas.cpp)
bool lift_props_on();
bool lift_walls_on();
constexpr int kMaxLiftBlocks = 254;   // g_lift is a byte: block + 1
inline std::vector<std::int32_t> g_pre_xz, g_pre_y;
// Per scene pixel: a prop's pixel that is lifted (BG1 on top, Lifted
// Props), so the scene shows the ground the game drew under it, at its
// terrain's place (g_pre_*). It keeps the terrain's light and takes the
// props' foot spots and cast shadows as the ground round it does; skipped as
// a prop's, it stayed bright as a strip under a bush whose card the camera
// raised off it (JE, the user's slot 3, map 40's rock at 8,13 with the
// camera up: the bush split horizontally). Empty when none.
// KOL_LIFT_GROUND_UNSHADED=1: skipped, as before.
inline std::vector<std::uint8_t> g_lift_ground;
// What each pixel showed before a nearer cell drew over it in the terrain
// pass (kind none: nothing): the surface behind, for lifted BG1 terrain.
inline std::vector<std::int32_t> g_back_xz, g_back_y;
inline std::vector<std::uint8_t> g_back_kind;
// The pixels a roof placed this frame (the house's own art, see the roofs).
inline std::vector<std::uint8_t> g_roofed;
// The pixels an awning (an open roof) placed: its cloth, which the props at
// its feet (its posts) must not take as theirs. Empty with no open roof.
inline std::vector<std::uint8_t> g_awning;
// (2 there: cloth the game drew on BG0. Lifted with the rest though nothing
// is recorded behind a BG0 pixel: the scene shows, at the surface behind,
// the colour of the nearest BG0 pixel that is not cloth (g_under_fix, laid
// over the under-BG1 picture for the warp in g_under_patched). Left in the
// scene's mesh at the awning's height, those pixels pulled the fishmonger
// beside them up into a streak.)
struct UnderFix { std::uint32_t k; std::uint8_t rgb[3]; };
inline std::vector<UnderFix> g_under_fix;
// A kModelOver model's rope pixels over its other parts: lifted, so that a rope
// is in the air to its ends (the user, map 90 and 117, asked for the ropes to
// touch the boat model only at their ends). 1: the art has what is
// behind (BG1 over the deck's BG0). 2: it has not (painted on BG0, or over a
// part on BG1): the colour behind is the nearest pixel of that surface
// (g_rope_under_fix, as g_under_fix). 3, 4: over the sky, nothing behind it;
// 4 nearer than the model's part beside it, 3 farther. Empty on other maps.
inline std::vector<std::uint8_t> g_rope_air;
inline std::vector<UnderFix> g_rope_under_fix;
// An art flame the effect draws anew (kModelFxArt) on a wall drawn on BG1
// alone (map 66's left wall, the sky on BG0 behind): lifted all the same, the
// wall there painted from its own column above or below the flame
// (lift_wall_surfaces), as g_under_fix.
inline std::vector<UnderFix> g_fx_under_fix;
// Options > Special Effects on map 73: its waterfalls (kol::kMapFalls). KOL_NO_FALLS=1: off.
bool model_part_fx_art(int map, int part);   // kol_focus_models.cpp
int model_far_part(int map, float* share);   // kol_focus_models.cpp (kFarParts): -1 none
bool map_falls_on();
inline kol::FallVapor g_fall_vapor[kol::kFallCount];
// The pixels of a scene actor's sprite that took a kModelActor part's place
// (kSceneObjects::model_actor): Lifted Walls lifts them as a BG1 part's.
// Empty on other maps.
inline std::vector<std::uint8_t> g_model_actor_px;
// They are a unit's (SceneObjects::body_tiles), and keep the sprite's colours.
inline bool g_model_actor_own_light = false;
inline bool g_plant_cell[kGridSide * kGridSide] = {};   // see kPlantRecords
inline bool g_crystal_cell[kGridSide * kGridSide] = {};
// Per grid cell (gi): a lamp whose flame its prop carries on a dark map
// (the castle's statue: record 86 burning as a pot), lamp index + 1; 0 none.
// Filled with the lamps.
inline std::vector<int> g_self_lamp_cell;
// The cell whose prop claimed each pixel (gi, -1 none), for the plant flag:
// taken under a card's middle column instead, the house's gable piece at 1,16
// on map 6, a card over its cell's left edge only, read the bush's cell 0,17
// beside it and swayed with the bushes (state GP).
// KOL_PERSP_PLANT_BY_COLUMN=1: by the middle column, as before.
inline std::vector<int> g_prop_cell;
// A prop's pixel claimed (a cut-out, a tree, a flame, a BG1 shape): upright
// at its card's place (xz, y), not spread from (g_prop_dist 0), the cell's
// (or group's) whose card it is.
inline void set_prop_px(std::size_t k, int xz, int y, int cell) {
    g_xz[k] = xz;
    g_y[k] = y;
    g_kind[k] = kol::kSceneUpright;
    g_prop_dist[k] = 0;
    g_prop_cell[k] = cell;
}
// On the maps whose BG1 is props (kBg1PropMaps): each scene pixel's BG1
// shape (-1 none; empty on other maps), and the shapes' pixels past the
// scene, which only their shadows take (bg1_shape_props, scene_props).
inline std::vector<int> g_bg1_shape;
struct PropOutPx {
    int shape, cell, px, py, xz, y;
    bool operator==(const PropOutPx&) const = default;
};
inline std::vector<PropOutPx> g_prop_out;
inline std::vector<std::uint8_t> g_under_patched;
// ... and its occlusion (with Ambient Occl. or Shadows on), which the scene
// shows there once the BG1 terrain over it is lifted; the lifted terrain's
// own texels keep theirs (g_lift_ao, valid when g_lift_ao_on).
// Debug > Tile Info (see the end of build_scene_now).
inline int g_tile_target = -1;
inline int g_tile_rect[4] = {-1, 0, 0, 0};
inline std::string g_tile_text;
inline std::vector<std::uint8_t> g_back_ao, g_lift_ao;
// A pass putting something in front of pixel k (a roof, a box, the model)
// keeps what it showed, for Lifted Walls to open onto (g_back_*), when
// `behind` (that is something), else marks nothing behind it.
inline void keep_behind(std::size_t k, bool behind, std::size_t npx) {
    if (behind) {
        g_back_xz[k] = g_xz[k];
        g_back_y[k] = g_y[k];
        g_back_kind[k] = g_kind[k];
        if (g_back_ao.size() == npx && g_ao.size() == npx) g_back_ao[k] = g_ao[k];
    } else {
        g_back_kind[k] = kol::kSceneNone;
    }
}
// The editor's roofs and boxes as the last picture whose inputs were the
// same left them (build_scene_now; asked 2026-10-26, map 6's 56 roofs and
// boxes: 1.85 ms a picture on the Mac, ~3 on the Thor, camera still or
// not). They ray-solve every pixel of every roof's screen box after the
// terrain pass, outside its PassCache. Keyed on the pass's key, the roofs
// and boxes, and the frame's layers, layer codes and colours over the
// roofs' screen rows (the awnings' cloth and the roofs' outline go by
// colour; a unit walking past changes them); taken once a key repeats.
// KOL_ROOF_CACHE=0 off; KOL_ROOF_CACHE_CHECK=1 builds them anyway and
// counts differences from the cache.
// The terrain pass's arrays as one set, as the caches keep them (PassCache,
// RoofResult) and the pass's check compares them: the places, kinds, cells
// and faces always; the optional parts the world X and Z (kWxz), the
// occlusion (kAo), and what each pixel showed before BG1 was lifted (kBack;
// its occlusion with kAo too). put() leaves a global the parts do not name
// as it is.
struct PassBuffers {
    static constexpr unsigned kWxz = 1, kAo = 2, kBack = 4, kAll = kWxz | kAo | kBack;
    std::vector<std::int32_t> xz, y;
    std::vector<std::uint8_t> covered, kind, face, ao, span;
    std::vector<std::int8_t> round_n;
    std::vector<std::int16_t> owner, wx, wz;
    std::vector<std::int32_t> back_xz, back_y;
    std::vector<std::uint8_t> back_kind, back_ao;
    void take(unsigned parts) {
        xz = g_xz; y = g_y; covered = g_covered; kind = g_kind; owner = g_cell_owner; face = g_face; span = g_span_px; round_n = g_round_n;
        if (parts & kWxz) { wx = g_wx; wz = g_wz; }
        if (parts & kAo) ao = g_ao;
        if (parts & kBack) {
            back_xz = g_back_xz; back_y = g_back_y; back_kind = g_back_kind;
            if (parts & kAo) back_ao = g_back_ao;
        }
    }
    void put(unsigned parts) const {
        g_xz = xz; g_y = y; g_covered = covered; g_kind = kind; g_cell_owner = owner; g_face = face; g_span_px = span; g_round_n = round_n;
        if (parts & kWxz) { g_wx = wx; g_wz = wz; }
        if (parts & kAo) g_ao = ao;
        if (parts & kBack) {
            g_back_xz = back_xz; g_back_y = back_y; g_back_kind = back_kind;
            if (parts & kAo) g_back_ao = back_ao;
        }
    }
    bool same(unsigned parts) const {
        return round_n == g_round_n && xz == g_xz && y == g_y && covered == g_covered && kind == g_kind && owner == g_cell_owner &&
               face == g_face && (!(parts & kWxz) || (wx == g_wx && wz == g_wz)) && (!(parts & kAo) || ao == g_ao) &&
               (!(parts & kBack) || (back_xz == g_back_xz && back_y == g_back_y && back_kind == g_back_kind &&
                                     (!(parts & kAo) || back_ao == g_back_ao)));
    }
    // All of them traded with the globals (the pass's check runs over these).
    void swap_globals() {
        xz.swap(g_xz); y.swap(g_y); covered.swap(g_covered); kind.swap(g_kind);
        face.swap(g_face); ao.swap(g_ao); owner.swap(g_cell_owner);
        wx.swap(g_wx); wz.swap(g_wz);
        round_n.swap(g_round_n);
        back_xz.swap(g_back_xz); back_y.swap(g_back_y);
        back_kind.swap(g_back_kind); back_ao.swap(g_back_ao);
    }
};
inline bool g_lift_ao_on = false;
inline std::vector<std::uint8_t> g_pre_kind;
// What casting a block's shadow needs: the unit's place (feet at height
// foot -- the sprite's anchor, 8 under the ground it stands on -- on
// hardware column feet_hx and row feet_hy) and the block's corner
// in hardware coordinates. Props (bags...) cast none.
struct BlockCaster {
    bool unit;
    int x, z, foot;
    float feet_hx, feet_hy;
    int hx0, hy0;
    int ground;   // the ground under it: foot + 8, or under a flying unit
    // Its shadow's strength: a semi-transparent unit (OBJ mode 1) casts
    // BLDALPHA's EVA / 16 of one (map 97's street soldier fading out,
    // kol_gulls; the deployment ghost at 8/8 casts half).
    float strength = 1.0f;
    // The top rows of a unit taller than a slot (kTallCard): the card over
    // the unit's own, its texels from 2 x (feet_hy - hy0) units up.
    bool upper = false;
    // Hanging high over the ground under it, with no blob there (the boat's
    // flag on map 58, 120 over its deck): nothing at its feet to stand on.
    bool hanging = false;
};
inline std::vector<BlockCaster> g_casters;
// A point on a unit's card (standing at its place, facing the camera: a
// pixel right on screen is +1 X, -1 Z, a row up 2 units up): scene pixel
// (px, py)'s, its middle.
inline void card_point(const BlockCaster& c, int px, int py, int ox, int oy, float* X, float* Y, float* Z) {
    const float col = static_cast<float>(px - ox) + 0.5f - c.feet_hx;
    const float up = 2.0f * (c.feet_hy - static_cast<float>(py - oy) - 0.5f);
    *X = c.x + col;
    *Y = static_cast<float>(c.foot) + 8.0f + up;
    *Z = c.z - col;
}
// The fixed lamps of this frame (torches, flames, fire pots; not a unit's
// own lamp), for the units' shadows from them (cast_lamp_shadows).
// A light's cone on the ground (a door light, kDoorLights): how much of it a
// point at (X, Z) takes, 1 inside cos_in of its direction, fading to 0 at
// cos_out; a light with no direction (cos_out < -1) lights all round.
// A door light is as wide as its doorway: a point takes it from the nearest
// point of the opening, the segment `half_w` either side of (lx, lz) across
// the light's direction. A point light: half_w 0.
inline void door_nearest(float lx, float lz, float dir_x, float dir_z, float half_w, float X, float Z, float* ox, float* oz) {
    *ox = lx; *oz = lz;
    if (half_w <= 0.0f) return;
    const float tx = -dir_z, tz = dir_x;   // along the wall
    const float along = std::clamp((X - lx) * tx + (Z - lz) * tz, -half_w, half_w);
    *ox = lx + tx * along;
    *oz = lz + tz * along;
}
// A door light's fall with distance (KOL_DOOR_FALLOFF=<power>, 3; 1 linear
// as a lamp's): steeper keeps it full by the doorway and lets it fade across the
// room, where the dark map's gain otherwise held it full.
inline float door_fall(float lin) {
    static const float p = [] {
        const char* e = std::getenv("KOL_DOOR_FALLOFF");
        return e ? std::max(0.1f, static_cast<float>(std::atof(e))) : 3.0f;   // cubed: the user's pick (Alphonse 5 tiles in, GZ)
    }();
    return lin <= 0.0f ? 0.0f : (p == 1.0f ? lin : std::pow(lin, p));
}
// A lamp's fall at distance d of its reach: a door light's (a cone,
// cos_out >= -1: door_fall), else linear.
// A fire's flicker (the cave's pots, a camp fire): three uneven rhythms,
// `depth` deep, phase p its own (0 for a camp fire's).
inline float fire_flicker(float t, float p, float depth) {
    return 1.0f + depth * (0.6f * std::sin(t * 0.53f + p) + 0.3f * std::sin(t * 1.7f + 2.0f * p) +
                           0.1f * std::sin(t * 4.1f + 3.0f * p));
}
// `feather` (MapLamp::feather, 0..1): toward a smoothstep of the linear
// fall, which meets the reach flat (linear, the pool's rim was a line where
// the light ran out: map 118's candle, the user asked for a bigger range
// with a feathered edge).
inline float lamp_fall(float cos_out, float d, float reach, float feather = 0.0f) {
    if (cos_out >= -1.0f) return door_fall(1.0f - d / reach);
    const float f = 1.0f - d / reach;
    return feather > 0.0f ? f + (f * f * (3.0f - 2.0f * f) - f) * feather : f;
}
inline float light_cone(float lx, float lz, float dir_x, float dir_z, float cos_in, float cos_out, float X, float Z) {
    if (cos_out < -1.0f) return 1.0f;
    const float vx = X - lx, vz = Z - lz;
    const float len = std::sqrt(vx * vx + vz * vz);
    if (len < 1.0f) return 1.0f;
    // Only past the door's plane (the light sits in the doorway): the wall
    // it is in and everything outside stay dark.
    if (vx * dir_x + vz * dir_z < 1.0f) return 0.0f;
    const float c = (vx * dir_x + vz * dir_z) / len;
    const float t = std::clamp((c - cos_out) / std::max(1e-3f, cos_in - cos_out), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
struct LampPt { float x, y, z, reach, power, flick; float dir_x = 0.0f, dir_z = 0.0f, cos_in = -2.0f, cos_out = -2.0f, half_w = 0.0f, shadow_k = 1.0f;
                float carrier = 0.0f; };   // a carried torch (kMapLamps): no shadow for a unit this near it, its carrier
inline std::vector<LampPt> g_frame_lamps;
inline std::vector<kol::SceneLamp> g_scene_lamps;   // the same lights, for the air (ScenePixels::lamps)
inline std::uint32_t g_cast_heights_key = 0;   // g_cast_heights' heights, for the caches' keys
// On a dark map: how much of the lamps' light each pixel loses to a unit's
// shadow (0 none .. 255 all), null when none (ScenePixels::lamp_shade).
inline std::vector<std::uint8_t> g_lamp_shade;
inline int g_cast_heights[kGridSide * kGridSide];   // the cells' heights this frame (gi)

// (kol_focus_shadows.cpp)
bool unit_shadows_on();
bool units_cast();

// (kol_focus_windows.cpp)
bool units_cast_from_windows();

// Props cast their shadows too (with the terrain's shadows on), as units do
// (below): each prop, the pixels the scene gave one place (its card), stands
// as a card turned to face the sun at that place, its pixels its silhouette,
// with the same soft edge. It replaced the props' contact occlusion, which
// never looked right (the user, 2026-10-13, asked to drop AO on props and
// shade them as units are). Kept per prop while
// nothing it depends on changes. KOL_NO_PROP_SHADOWS=1 leaves them out.
// The scene's props: their pixels (claimed or spread, upright) that touch
// and share one place (two props on one diagonal at one height share a
// place: as one, their card stood between them and its shadow lay on the
// grass apart from both), and come from one cell: two pines side by side on
// a diagonal at one height overlap, touching, and as one their card stood
// between them, a blob of a shadow in front of the pair (JD, the user's
// slot 3, map 40's pines by the cursor: the tree's shadows there looked
// wrong). KOL_PROP_GROUP_BY_PLACE=1: by place alone, as before.
// prop_of: each pixel's prop, -1 none.
// On a map whose BG1 is props (g_bg1_shape), a group is one BG1 shape on one
// foot, its box and `out` taking the shape's pixels past the scene (their
// shadow is whole while the camera pans the shape in).
struct PropGroup { int xz, y, x0, y0, x1, y1; int shape = -1, cell = -1; std::vector<std::pair<int, int>> out; };

// (kol_focus_shadows.cpp)
std::vector<PropGroup> scene_props(int w, int h, std::vector<int>& prop_of);
// The frame's colours while scene_pixels builds (the warp provider's scene),
// else null: a pedestal tells its stone from the floor around it by them.
inline const std::uint8_t* g_scene_rgb = nullptr;
// The last scene's colours, kept for KOL_FOCUS_DUMP's .rgb (perspective on).
inline std::vector<std::uint8_t> g_dump_rgb;

// (kol_focus_shadows.cpp)
void cast_unit_shadows(int w, int h, int ox, int oy, int kx, int ky);
void cover_shade(int w, int h, int ox, int oy, int kx, int ky);
void cast_window_shadows(int w, int h, int ox, int oy, int kx, int ky);
bool units_cast_from_lamps();
void cast_lamp_shadows(int w, int h, int ox, int oy, int kx, int ky);

// Where this frame's campfires (kFireActors) are, world X, Z: no unit's
// contact occlusion within kFireNoAo of one (the user, map 63, asked for no
// ambient occlusion on the firepit, then reported an AO blot left of
// the pit: Allos's, sitting on his bench, his oval at his feet by the fire).
inline std::vector<std::pair<int, int>> g_fire_spots;
inline std::vector<std::uint8_t> g_fire_logs;   // a campfire's logs, left in the scene (ScenePixels::fire_logs)
inline bool g_fire_logs_any = false;   // any of them marked

// (kol_focus_shadows.cpp)
float fire_logs_shade();
inline float g_fire_cx = 0.0f, g_fire_cy = 0.0f;   // its place on screen, scene px (ScenePixels::fire_x/y)
inline bool g_fire_seen = false;

// (kol_focus_shadows.cpp)
void unit_contact_ao(int w, int h, int ox, int oy, int kx, int ky);
void unit_lamp_glow(int w, int h, int ox, int oy, int kx, int ky);

// The fixed lamps on the units of a dark map, a pixel at a time: each
// pixel a point on the unit's card standing at its place, facing the
// camera (as unit_lamp_glow's), lit by each lamp by its distance and by
// how squarely the lamp faces the card (half from behind), and hidden
// where the terrain stands between it and the flame (lamp_seen, the
// ground's own test), so a unit below a ledge is lit at the head and
// dark at the legs (asked 2026-10-14 whether units could be partly shadowed
// as in the sun). Until then a unit took one light, the ground's
// at its feet, all of it lit or all dark. KOL_UNIT_LAMP_FEET=1: that.
// Whether a point sees the flame is a 4-unit cube's answer, 2 rows of
// the card: averaged over three cubes it came down in 2-row steps (the
// user found the cutoff harsh). Now the light is worked out at every
// point of the card (the unit's whole square), capped where the
// darkness's gain (kDarkMaps lamp_gain, 3 on map 51) makes it full, then
// blurred over the card, a box of KOL_UNIT_LAMP_SOFT px (3) each way,
// twice: an edge about 6 rows soft on screen (6: 12), across or down the
// unit alike.
// (Blurring whether the flame is seen and capping after, the cap took
// the ramp's top two thirds as full light: the edge stayed 2-3 rows.)
template <class Seen>
void unit_card_lamp_light(int w, int h, int ox, int oy, const std::vector<Lamp>& lamps, Seen&& lamp_seen) {
    static const bool feet = std::getenv("KOL_UNIT_LAMP_FEET") != nullptr;
    static const int soft = [] {
        const char* v = std::getenv("KOL_UNIT_LAMP_SOFT");
        return v ? std::clamp(std::atoi(v), 0, 16) : 3;   // the user's pick (6 first)
    }();
    g_unit_lamp_any = false;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (feet || indoor_dark() >= 1.0f || lamps.empty() || g_atlas_bailed || g_lift.size() != n) return;
    // Per texel of each unit's card, in the atlas's layout (as
    // g_unit_win_patch_a): a part another unit hides in the frame is lit
    // as its own card, not as that unit's.
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const std::size_t an = static_cast<std::size_t>(g_atlas_w) * g_atlas_h;
    g_unit_lamp.assign(an, 0);
    constexpr float kFace = 0.70710678f;   // the card's normal: toward the camera, (1, 0, 1) / sqrt 2
    static std::vector<float> vis, tmp;
    const float cap = 255.0f / std::max(1e-3f, indoor_lamp_gain());
    // A box blur of radius `soft` over a bw x bh grid, along rows then columns.
    auto blur = [&](int bw, int bh) {
        tmp.resize(vis.size());
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                float sum = 0.0f;
                int cnt = 0;
                for (int q = std::max(0, x - soft); q <= std::min(bw - 1, x + soft); ++q, ++cnt) sum += vis[y * bw + q];
                tmp[y * bw + x] = sum / cnt;
            }
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                float sum = 0.0f;
                int cnt = 0;
                for (int q = std::max(0, y - soft); q <= std::min(bh - 1, y + soft); ++q, ++cnt) sum += tmp[q * bw + x];
                vis[y * bw + x] = sum / cnt;
            }
    };
    for (std::size_t bi = 0; bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        // A unit's card, or a map object's drawn as one (a bag of loot: no battle unit,
        // so its card took no lamp's light at all and stood black in a lit hall on a
        // dark map; the user, map 56, states/thor/0287, asked why the bag was dark).
        // kol_perspective reads this light for every card that is no prop, wall,
        // scenery or overlay, so those are the ones lit here.
        // KOL_UNIT_LAMP_UNITS_ONLY=1: battle units alone, as before.
        static const bool units_only = std::getenv("KOL_UNIT_LAMP_UNITS_ONLY") != nullptr;
        const bool object_card = !units_only && !b.prop && !b.surface && !b.scenery && !b.overlay && b.effect == 0;
        if (!c.unit && !object_card) continue;
        if (b.w <= 0 || b.h <= 0) continue;
        const std::size_t cells = static_cast<std::size_t>(b.w) * b.h;
        vis.assign(cells, 0.0f);   // the light on the card
        // The card's point at (tx, ty) of the square.
        auto point = [&](int tx, int ty, float* X, float* Y, float* Z) {
            card_point(c, b.sx + tx, b.sy + ty, ox, oy, X, Y, Z);
        };
        // (A map's lamp, carried by a unit, too: without it the carrier
        // took the light at its feet, the water 70 under the gangplank
        // it stood on, and went dark under its own lamp, map 58 at
        // night. KOL_UNIT_LAMP_NO_MAP_LAMPS=1: as before.)
        static const bool no_map_lamps = std::getenv("KOL_UNIT_LAMP_NO_MAP_LAMPS") != nullptr;
        // A fire's light on the units: its share from behind the card
        // and a scale. 1.5: map 63's fire went 1.5 -> 1.0 for its ground
        // (the flame's contrast), and the units with it (Alphonse beside
        // it 103 -> 80, the user noticed Alphonse got darker); they keep
        // the 1.5 they had. KOL_FIRE_UNIT=<back,scale> (mockups: 0.8 and
        // 1.0 from behind came out as this, the card at its cap).
        static float fu[2] = {0.5f, 1.5f};
        static const bool fu_read = [] {
            if (const char* e = std::getenv("KOL_FIRE_UNIT")) std::sscanf(e, "%f,%f", &fu[0], &fu[1]);
            return true;
        }();
        (void)fu_read;
        for (std::size_t i = 0; i < lamps.size(); ++i) {
            const Lamp& L = lamps[i];
            const float back = L.fire ? fu[0] : 0.5f, scale = L.fire ? fu[1] : 1.0f;
            if (L.units && no_map_lamps) continue;
            {   // out of reach of the whole square: nothing to do
                float X, Y, Z;
                point(b.w / 2, b.h / 2, &X, &Y, &Z);
                const float d = std::sqrt((L.x - X) * (L.x - X) + (L.y - Y) * (L.y - Y) + (L.z - Z) * (L.z - Z));
                if (d >= L.reach + static_cast<float>(b.w + 2 * b.h)) continue;
            }
            for (int ty = 0; ty < b.h; ++ty)
                for (int tx = 0; tx < b.w; ++tx) {
                    float X, Y, Z;
                    point(tx, ty, &X, &Y, &Z);
                    float lx, lz;
                    door_nearest(L.x, L.z, L.dir_x, L.dir_z, L.half_w, X, Z, &lx, &lz);
                    const float dx = lx - X, dy = L.y - Y, dz = lz - Z;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= L.reach * L.reach) continue;
                    const float d = std::sqrt(d2) + 1e-3f;
                    const float facing = std::max(0.0f, (dx + dz) * kFace / d);
                    const float seen = lamp_seen(i, X, std::max(0.0f, Y), Z);
                    if (seen <= 0.0f) continue;
                    vis[static_cast<std::size_t>(ty) * b.w + tx] +=
                        255.0f * L.power * scale * lamp_fall(L.cos_out, d, L.reach, L.feather) *
                        (back + (1.0f - back) * facing) * L.flick *
                        light_cone(lx, lz, L.dir_x, L.dir_z, L.cos_in, L.cos_out, X, Z) * seen;
                }
        }
        for (float& v : vis) v = std::min(v, cap);
        if (soft > 0) blur(b.w, b.h);
        const int ax0 = g_atlas_per_row > 0 ? (b.slot % g_atlas_per_row) * S : 0;
        const int ay0 = g_atlas_per_row > 0 ? (b.slot / g_atlas_per_row) * S : 0;
        for (int ty = 0; ty < b.h; ++ty)
            for (int tx = 0; tx < b.w; ++tx) {
                const std::size_t a = static_cast<std::size_t>(ay0 + ty) * g_atlas_w + ax0 + tx;
                if (a >= an) continue;
                const float t = vis[static_cast<std::size_t>(ty) * b.w + tx];
                if (t <= 0.5f) continue;
                g_unit_lamp[a] = static_cast<std::uint8_t>(std::min(255.0f, t + 0.5f));
                g_unit_lamp_any = true;
            }
    }
}

// (kol_focus_atlas.cpp)
void grow_over_sprite(const std::uint8_t* layers, int w, int h);
void props_over_units(const std::uint8_t* layers, int w, int h, bool props_on, bool walls_on);   // (kol_focus_atlas.cpp)
void build_sprite_atlas(const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky,
                        const Unit* units, int count, int unit_count);

// Camera constants voted for by sprite slots, per map.
struct Vote { int cx = 0, cy = 0; unsigned n = 0; };

// Focus state across presents.
struct Focus {
    std::uint32_t list = 0;          // object array the positions below are for
    int pos[kObjects][3] = {};
    bool pos_valid[kObjects] = {};
    bool have = false;               // the focus below is set
    float xz = 0.0f, y = 0.0f;       // where the focus is
    std::uint32_t grid_hash = 0;     // the map the votes below are for
    Vote votes[16];
    bool have_camera = false;        // the last camera found, for unwrapping
    int kx = 0, ky = 0;
    int rest_kx = 0, rest_ky = 0;    // the game's camera, and frames it has rested
    int rest_frames = 0;
    int cut_hofs = 0, cut_vofs = 0;  // the last frame's scroll, to see a cut (a jump)
    bool cut_scroll_valid = false;
    int step_cx = 0, step_cy = 0;    // the game's camera plus the scroll, last frame
    bool step_valid = false;
};
inline Focus g_focus;
inline int g_pic_hofs = 0, g_pic_vofs = 0;
// Counts real frames (see focus_note_scroll); the scene is built once per
// frame even when the perspective and the depth of field both ask for it.
inline unsigned long g_frame_serial = 0;


// What the last build_scene() found, for the depth of field.
struct SceneInfo {
    int w = 0, h = 0, ox = 0, oy = 0, kx = 0, ky = 0;
    int vx = 0, vy = 0, vw = 0, vh = 0;   // the view in the frame
    Unit units[kObjects];
    int count = 0, moved = -1, moved_count = 0;
    bool cursor = false;
    int cursor_xz = 0, cursor_y = 0;
    bool fall_card = false;    // see ScenePixels::fall_card
    bool fall_ready = false;
    int fall_v[2] = {0, 0};
    unsigned fall_on = 0;
    int fall_fx[3] = {0, 0, 0}, fall_fy[3] = {0, 0, 0}, fall_xz[3] = {0, 0, 0}, fall_y = 0;
    bool spell_card = false;   // see ScenePixels::spell_card
    int spell_fx = 0, spell_fy = 0, spell_xz = 0, spell_y = 0;
};
inline SceneInfo g_info;

// The pixels a map model placed (see kModels): 0 none, else 1 + its part.
inline std::vector<std::uint8_t> g_modeled;
// Each model pixel's normal (x, y, z) times 127, smooth as the model's
// corners (a cylinder's): the lamps' facing on its round walls
// (kol_focus_lamps). Empty when no model placed pixels.
inline std::vector<std::int8_t> g_model_n;

// (kol_focus_atlas.cpp)
void lift_prop_pixels(const std::uint8_t* layers, int w, int h, bool props_on, bool walls_on,
                      int ox, int oy, int kx, int ky);
inline std::uint32_t g_win_sun_key = 0;   // the heights and the sun, for the beams indoors (no g_shadow_key there)
inline std::vector<std::uint8_t> g_win_glow;   // per scene pixel, the beams' radiated light (dark_windows), 0..255
// Sunlight through the map's windows into g_win_patch / g_win_haze.
//
// The beams: on a 4-unit grid of the world, how much sun through a window
// each cell holds (0 none .. 1, fading over KOL_GODRAYS_FADE units from the
// window). Traced from the windows outward: rays 2 units apart across each
// opening (and its soft edge), each marched away from the sun until the
// terrain stops it, marking the cells it passes. It depends on the sun and
// the heights only (g_shadow_key) and the map, not on the camera, so it is
// kept until those change.
struct WindowBeams {
    std::uint32_t key = 0;
    bool valid = false;
    std::vector<float> cell;
    // The view's direction is the same for every pixel, (+1, +1, +1): for
    // each cell, the sum of the cells from it along that diagonal toward
    // the camera, so a view line's haze is one (interpolated) read.
    std::vector<float> diag;
    // On a dark map (dark_windows): the cells blurred, the light the beams
    // radiate around them (window_glow_at). Empty otherwise.
    std::vector<float> glow;
    int gx0 = 0, gz0 = 0, gnx = 0, gny = 0, gnz = 0;
    float lx0 = 0, lx1 = -1, ly0 = 0, ly1 = -1, lz0 = 0, lz1 = -1;   // the lit cells' bounds (world)
    std::size_t lit = 0;
    float ymax = 0.0f;
};
inline WindowBeams g_beams;
inline WindowBeams g_moon_beams;   // the second set's (MapWindow::own_deg), set aside while the main set's are in g_beams
// A cone window's godrays (window_cone_spread): shafts of light from across
// its opening, fanning out within the cone down to the floor, each a soft
// tube widening and fading along its length; a view line's haze is the
// shafts it passes in front of its surface (window_haze_at). The cone's
// beam cells only add a faint fill (kConeVolume) under them.
// `len` where it ends (the floor, or a wall or the room's edge in its
// way); `fade` the length it fades over: its way to the floor, whatever
// stops it first (a shaft cut short by the room's open front faded twice as
// fast and the rug it crossed went dark).
struct ConeShaft { float ax, ay, az, dx, dy, dz, len, w0, grow, gain, fade; };
inline std::vector<ConeShaft> g_shafts;
// Map 71's shutter narrows its cone as it closes (g_shutter_spread: 0.4 of
// the spread shut, whole open, set per position in read_shutter).
inline float g_shutter_spread = 1.0f, g_shutter_shown = 1.0f;
// Map 71's lightning (the user asked for very bright strikes, with bounce
// light too): 1 on a flash's pictures, halving after
// (read_shutter).
inline float g_lightning = 0.0f;
// Light past a model that stands in for the grid's walls (map 101's arcade:
// the grid has the band under the arches as a wall to the top, the model
// has a floor there, the alcove walls behind it and the piers and arches in
// front). The chandelier's light and the windows' sun are stopped by the
// model's faces, 4-unit cubes each face passes through, and the cells under
// its floor are as high as the floor, not the grid's wall.
struct ModelOccluder {
    int map = -1;
    int ix0 = 0, iy0 = 0, iz0 = 0, nx = 0, ny = 0, nz = 0;   // in 4-unit cubes
    std::vector<std::uint8_t> solid;
    std::vector<std::int8_t> floor_h;   // per grid cell (gi): the model's floor height there, -1 none
    // The cube a point is in (its index in solid), -1 outside.
    std::ptrdiff_t cube(float X, float Y, float Z) const {
        const int ix = static_cast<int>(std::floor(X / 4.0f)) - ix0;
        const int iy = static_cast<int>(std::floor(Y / 4.0f)) - iy0;
        const int iz = static_cast<int>(std::floor(Z / 4.0f)) - iz0;
        if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz) return -1;
        return static_cast<std::ptrdiff_t>((static_cast<std::size_t>(iz) * ny + iy) * nx + ix);
    }
    bool solid_at(float X, float Y, float Z) const {
        const std::ptrdiff_t c = cube(X, Y, Z);
        return c >= 0 && solid[static_cast<std::size_t>(c)] != 0;
    }
    // ... not counting what light passes (a railing's cubes, 2).
    bool solid_hard_at(float X, float Y, float Z) const {
        const std::ptrdiff_t c = cube(X, Y, Z);
        return c >= 0 && solid[static_cast<std::size_t>(c)] == 1;
    }
    // The cell's height for light: the model's floor where it has one.
    template <class Height>
    int height(int cx, int cz, Height cell_height) const {
        if (in_grid(cx, cz))
            if (const int f = floor_h[gi(cx, cz)]; f >= 0) return f;
        return cell_height(cx, cz);
    }
};
// Points on a face sit in its own cubes: light is tested against the model
// only this far from the point lit and from the window it comes through.
constexpr float kOccluderSkip = 6.0f;

// (kol_focus_models.cpp)
bool ship_light_map(int map);
const ModelOccluder* model_occluder(int map);
bool model_actor_plane(float* n, float& d);
inline const ModelOccluder* g_occ = nullptr;   // this frame's (model_occluder)

// The cells' heights the window light reads (build_scene_now's cell_height):
// each worked out once a frame by `fill` into `memo` (gi, INT_MIN not yet),
// -1 off the grid; with `occ`, a model's floor first where it has one
// (g_occ->height: the beams and the patch, not the units or the lifted
// pieces). A plain type, so the window light is no template on the build's
// lambda.
struct CellHeights {
    int* memo = nullptr;
    int (*fill)(const void* ctx, int cx, int cz) = nullptr;
    const void* ctx = nullptr;
    bool occ = false;
    int raw(int cx, int cz) const {
        if (!in_grid(cx, cz)) return -1;
        int& m = memo[gi(cx, cz)];
        if (m == INT_MIN) m = fill(ctx, cx, cz);
        return m;
    }
    int operator()(int cx, int cz) const {
        if (occ && g_occ) return g_occ->height(cx, cz, [this](int x, int z) { return raw(x, z); });
        return raw(cx, cz);
    }
};

// (kol_focus_windows.cpp)
void build_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                        const CellHeights& cell_height);

// Sunlight through the windows on the units' own pixels: each a point on a
// card standing at its unit's place, facing the camera (as unit_lamp_glow:
// a pixel right is +1 X, -1 Z, a row up 2 units up), the sun on it through
// a window and the beams' haze in front of it. Units move, so every frame,
// but only their pixels within the beams' bounds on screen.
inline std::vector<std::uint8_t> g_unit_win_patch, g_unit_win_haze;
// ... and per texel of each unit's card, in the atlas's layout: the parts of
// a unit another one hides in the frame are drawn too when the camera moves
// (the stick), and took the light worked out at their screen pixel -- the
// other unit's, or none (2026-10-15, GX: sprites were lit by the light under
// other sprites). Read by the warp for unit cards.
inline std::vector<std::uint8_t> g_unit_win_patch_a, g_unit_win_haze_a;
inline bool g_unit_win_any = false;

// (kol_focus_windows.cpp)
void unit_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                       const CellHeights& cell_height);

// Lifted BG1 terrain and props (lift_prop_pixels) are drawn from the atlas
// over the scene, which shows the surface behind them there. The window
// light worked out at those pixels was the front's (the terrain pass drew
// the front): it goes to the lifted texels (g_lift_win_*), and the scene's
// is worked out again for the surface behind (g_under_xz / g_under_y, X - Z
// from the column). Without it the lifted pieces cut holes in the light
// (the castle's pillars, FM).
inline std::vector<std::uint8_t> g_lift_win_patch, g_lift_win_haze;
inline bool g_lift_win_any = false;

// (kol_focus_windows.cpp)
// `memo_key`: everything the light on a world point reads but the point
// itself, without the view (0: worked out every time; see its memo).
void lift_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                       const CellHeights& cell_height, std::uint32_t memo_key);

// The second set of windows (MapWindow::own_deg, map 67's moonlight) runs
// through the main set's functions: for as long as this lives, its beams
// and buffers are swapped in for the main set's globals (each pair: a
// main-set global, the second set's), g_win_set is 1 with the second set's
// rise, and `any` (the main set's flag) is set aside; `shafts`: the cone
// shafts set aside too (the beams' build makes the second set's, dropped).
// When it ends everything goes back, the second set's flag to `moon_any`,
// and the haze's strength is the main set's again (the build sets it).
// Written out by hand twice before, where one swap missed on the way back
// would have left the sun's light with the moon's buffers.
class WinSetScope {
public:
    using Buf = std::vector<std::uint8_t>;
    WinSetScope(float rise, WindowBeams& beams, bool& any, bool& moon_any, bool shafts,
                std::initializer_list<std::pair<Buf*, Buf*>> bufs)
        : beams_(beams), any_(any), moon_any_(moon_any), any_main_(any), haze_k_main_(g_win_haze_k), shafts_(shafts) {
        for (const auto& p : bufs) {
            pairs_[n_++] = p;
            std::swap(*p.first, *p.second);
        }
        std::swap(g_beams, beams_);
        if (shafts_) std::swap(g_shafts, shafts_main_);
        g_win_set = 1;
        g_win_set_rise = rise;
    }
    ~WinSetScope() {
        g_win_set = 0;
        moon_any_ = any_;
        any_ = any_main_;
        g_win_haze_k = haze_k_main_;
        if (shafts_) std::swap(g_shafts, shafts_main_);
        std::swap(g_beams, beams_);
        while (n_ > 0) {
            --n_;
            std::swap(*pairs_[n_].first, *pairs_[n_].second);
        }
    }
    WinSetScope(const WinSetScope&) = delete;
    WinSetScope& operator=(const WinSetScope&) = delete;

private:
    WindowBeams& beams_;
    bool& any_;
    bool& moon_any_;
    const bool any_main_;
    const float haze_k_main_;
    const bool shafts_;
    std::vector<ConeShaft> shafts_main_;
    std::pair<Buf*, Buf*> pairs_[4];
    int n_ = 0;
};

// (kol_focus_present.cpp)
bool water_cells(int map, std::uint8_t* out);

// (kol_focus.cpp)
bool build_scene(const std::uint8_t* layers, int w, int h);

// (kol_focus_atlas.cpp)
void flame_cards(const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky);

// build_scene_now's frame: its locals that more than one of its phases
// reads, its helpers (the cells' rules, the roofs', the shade's, the
// lamps') as member functions, and each phase, as its WorkerTimer laps
// name them, a member function, run in order by run().
struct SceneBuild {
    SceneBuild(const std::uint8_t* layers, int w, int h) : layers(layers), w(w), h(h) {}
    const std::uint8_t* const layers;
    const int w, h;
    kol::WorkerTimer wt{"scene.setup"};
    const Capture& cap = g_cap;
    const std::uint8_t* ew = nullptr;
    int map = -1;
    const std::uint8_t* iw = nullptr;
    static std::uint32_t e(std::uint32_t a) { return a - 0x02000000u; }
    // The units, and which of them moved since the last present.
    Unit units[kObjects];
    int count = 0;
    int moved = -1;
    int moved_count = 0;
    int vx = 0, vy = 0, vw = 0, vh = 0;   // the view in the frame
    int ox = 0, oy = 0;                   // hardware (0, 0) in the frame
    int kx = 0, ky = 0;                   // the camera
    bool cursor = false;
    int cursor_xz = 0, cursor_y = 0;
    int cursor_sx = 0, cursor_sy = 0;   // before the camera is added
    int cursor_x = 0, cursor_z = 0;     // its world place
    // The map's records (cell_rules): the ROM, the map's table, and each
    // byte's answers memoised a frame (record_of, record_rule, is_obstacle).
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    std::uint32_t terrain_map = 0;
    int record_memo[256];
    std::int8_t rule_memo[256];
    std::int8_t obstacle_memo[256];
    const bool byte_props = [] {   // (is_obstacle)
        static const bool v = kol::env_true("KOL_PERSP_BYTE_PROPS");
        return v;
    }();
    bool bg1_props = false;
    // The drawn heights by cell (-1: as stored), their key, and the field's
    // cells that exist; the rounded boxes by cell (-1: none).
    int* drawn = nullptr;
    int* drawn_lower = nullptr;   // DrawnCells::lower, split_z, split_z0, by cell
    int* drawn_split = nullptr;
    int* drawn_split0 = nullptr;
    int* drawn_split_x = nullptr;   // 1: the span is over X offsets (DrawnCells::split_x)
    std::uint32_t drawn_key = 0;
    int field_x0 = 0, field_z0 = 0, field_x1 = kGameSide - 1, field_z1 = kGameSide - 1;
    int* round_of = nullptr;
    static thread_local std::vector<DrawnRound> rounds;
    // Each cell's height (cell_height_of, memoised; cell_heights_occ with the
    // model's floors).
    CellHeights cell_heights, cell_heights_occ;
    // The light this frame (roofs_and_shade).
    bool glow_on = false, godrays_on = false;
    bool world_on = false;   // each pixel's world place kept (g_wx, g_wz, g_face)
    float ao_s = 0.0f, sh_s = 0.0f;
    bool shade_on = false;
    // The editor's roofs, and each one's extent (roof_extent, once a frame).
    std::vector<kol::TileRoof> roofs;
    struct RoofExtent { float X0, X1, Z0, Z1; int cx0, cx1, cz0, cz1; };
    std::vector<RoofExtent> roof_ext;
    // The roofs over each cell, in the roofs' order (roofs_and_shade): cell gi's
    // are roof_cell_list[roof_cell_at[gi] .. roof_cell_at[gi + 1]). roof_only_y
    // asks only those (null: every roof, as before).
    const std::uint32_t* roof_cell_at = nullptr;
    const std::uint16_t* roof_cell_list = nullptr;
    // The boxes over each cell, the same way (roof_y).
    const std::uint32_t* box_cell_at = nullptr;
    const std::uint16_t* box_cell_list = nullptr;
    // Boxes set with the editor (chimneys, dormers): footprint, walls to
    // `top`, a gable to `peak` with its ridge along X or Z.
    struct Box { float X0, X1, Z0, Z1, E, T; bool along_x; int x, z; };
    std::vector<Box> boxes;
    bool any_open = false;   // an awning among the roofs
    // The cells' drawn heights (once a frame, for the shadow map and the
    // torchlight) and their key.
    int* heights = nullptr;
    std::uint32_t heights_key = 0;
    bool win_dark = false, heights_ready = false;
    float sun_x = 0.0f, sun_z = 0.0f, sun_rise = 0.0f;   // the build thread's (see roofs_and_shade)
    // The shadow map as shaded_at and shade_frac read it.
    int sh_ox = 0, sh_oz = 0, sh_w = 0, sh_h = 0;
    const std::int16_t* sh_map = nullptr;
    float soft_t = 0.0f;
    const std::uint16_t* sh_dist = nullptr;
    const std::uint8_t* sh_edge = nullptr;
    // The torches and lamps as lights (gather_lamps), whether their light is
    // worked out again (glow_dirty) and the world places written (world_dirty);
    // lamp_seen's skip of the model's own faces.
    std::vector<Lamp> lamps;
    bool glow_dirty = false, world_dirty = false;
    float lamp_skip = 0.0f;
    // The terrain pass's key (PassCache) and the places' alone (the BG1
    // shapes' cache); the occlusion's reach.
    float ao_reach_v = 0.0f;
    std::uint32_t pass_key = 0, place_key = 0;
    // The same as pass_key but for where the camera is (kx, ky): the world,
    // its heights and the sun alone (terrain_pass's still_key).
    std::uint32_t pass_still_key = 0;
    // The roofs' and boxes' cache (RoofCache): checked, hit, its key.
    const bool roof_cache_check = [] {
        static const bool v = kol::env_true("KOL_ROOF_CACHE_CHECK");
        return v;
    }();
    bool roof_hit = false;
    bool roof_par_serial_now = false;   // KOL_ROOF_PAR_CHECK's second run: the roofs on one thread
    std::uint64_t roof_key = 0;
    // The props (prop_base): whether lifted (props, walls), the scene's size.
    std::size_t pixels = 0;
    bool lift_on = false, walls_on = false;
    // The units' sprites (prop_light_pass): whether they are drawn from their
    // own (sprite_atlas), KOL_FOCUS_BOX_ONLY=1 (box_only), and the units
    // followed by the bags on the map (all, props of them).
    bool sprite_atlas = false;
    const bool box_only = [] {
        static const bool v = std::getenv("KOL_FOCUS_BOX_ONLY") != nullptr;
        return v;
    }();
    Unit all[kObjects + kMaxProps];
    int props = 0;

    bool run();
    // The phases, in the order run() calls them, each after the helpers it
    // brings (the build's lambdas before; later phases use them too).
    bool camera_and_units();   // scene.setup: false, no scene
    int record_of(std::uint8_t local);
    int record_rule_of(std::uint8_t local);
    int record_rule(std::uint8_t local);
    bool is_obstacle(std::uint8_t local);
    int raw_cell(int cx, int cy, bool* prop);
    int ground_cell(int cx, int cy, bool* prop);
    bool is_spire(int cx, int cy);
    int spire_ground(int cx, int cy);
    int cell_height_of(int cx, int cy);
    int cell_height(int cx, int cy) const { return cell_heights.raw(cx, cy); }
    void cell_rules();         // scene.setup
    static float box_h(const Box& b, float X, float Z);
    bool roof_only_y(float X, float Z, float* y, bool open = false);
    bool roof_y(float X, float Z, float* y);
    bool shaded_at(float X, float Z, float y, bool awnings = true);
    int cell_h(float X, float Z);
    float shade_frac(float X, float Z, float y, float tdx = 0.0f, float tdz = 0.0f);
    void roofs_and_shade();    // scene.setup
    float lamp_seen(std::size_t i, float X, float Y, float Z);
    float lamp_seen_cube(std::size_t i, int ix, int iy, int iz) const;   // worked out, not read
    void gather_lamps();       // scene.setup
    void terrain_pass();       // its key (scene.setup), scene.terrain
    void lamp_and_window_light();
    void glow_pass();          // scene.glow (scene.windows inside)
    bool wall_lit(int face, float X, float Z, float hit, bool awnings);
    std::uint8_t lit_ao(bool lit) const { return static_cast<std::uint8_t>(std::lround(255.0f * (lit ? 1.0f : 1.0f - sh_s))); }
    void roofs_pass();         // scene.roofs
    void boxes_pass();         // scene.boxes
    bool prop_pixel(std::size_t k);
    template <class F> void mask_pixels(const char* const* rows, int count, int x0, int y0, F&& f);
    void flat_cell(std::size_t k, int* cx, int* cy);
    bool below_base(std::size_t k, int xz, int y);
    void model_pass();         // scene.model
    void prop_base();          // scene.propbase
    void cutouts();            // scene.cutouts
    void bg1_shapes();         // scene.bg1_shapes
    void spread();             // scene.spread
    void prop_light_pass();    // scene.proplight
    void atlas();              // scene.atlas (and its sub-laps)
    void rest();               // scene.rest
    void debug_tile_info();    // (rest)
};

// (SceneBuild::rounds is defined in kol_focus.cpp: an inline thread_local
// with a constructor gets one TLS init function per file from MinGW's GCC,
// and the Windows link refused the duplicates.)

// A map byte's terrain record (cell_rules), worked out once a frame.
inline int SceneBuild::record_of(std::uint8_t local) {
    int& memo = record_memo[local];
    if (memo != INT_MIN) return memo;
    const std::uint32_t at = terrain_map - 0x08000000u + local * 2u;
    if (!rom || terrain_map < 0x08000000u || at + 2u > rom_size) return memo = -1;
    return memo = rom[at] | (rom[at + 1] << 8);
}

// (Memoised: rule_memo.)
inline int SceneBuild::record_rule(std::uint8_t local) {
    std::int8_t& memo = rule_memo[local];
    if (memo == INT8_MIN) memo = static_cast<std::int8_t>(record_rule_of(local));
    return memo;
}

// A prop is something standing on a cell that no unit may enter: the
// record's +1 is 0x7F (trees 41, snow rocks 42, the water rock 50, torches
// 70, fire pots 76...). The bytes the prop rules go by are the map's own
// numbers, and on other maps they name plain terrain: 0x0A is ground
// (record 1) where the cursor stood in DP_cursor_not_terrain, a wall (61)
// on the ruins by the water, 0x07 ice (3) on CK and block tops (66) on BY.
// Those stood as props. Only a real obstacle's cell is one now; nothing
// new becomes a prop (the ruins' pillars, 60, and BT's church, 71, are
// obstacles too but have no prop byte). KOL_PERSP_BYTE_PROPS=1 goes by the
// bytes alone again.
inline bool SceneBuild::is_obstacle(std::uint8_t local) {
    if (byte_props) return true;
    std::int8_t& memo = obstacle_memo[local];
    if (memo >= 0) return memo != 0;
    const int r = record_of(local);
    const std::size_t at = kTerrainTable - 0x08000000u + static_cast<std::size_t>(r) * 0x20u + 1u;
    memo = r >= 0 && at < rom_size && rom[at] == 0x7Fu;
    return memo != 0;
}

// Height as stored (a prop's top), and the ground a prop stands on: the
// highest neighbour that is not a prop itself, never above the prop.
inline int SceneBuild::raw_cell(int cx, int cy, bool* prop) {
    if (!in_game(cx, cy)) {
        // Beyond the game's grid: a cell only where given a height.
        if (!in_grid(cx, cy)) return -1;
        const int d = drawn[gi(cx, cy)];
        if (d >= 0 && prop) *prop = false;
        return d < 0 ? -1 : d;
    }
    const std::uint32_t c = e(kGrid) + game_cell(cx, cy);
    // A cell no unit may enter (flag 0x80 clear) is still drawn when it
    // has a height: the tall pillars around the ruins' edge, height 22
    // against the map's 2-17 (states/CV_pillars_background). Left out,
    // they stayed behind as painted background while the map shrank.
    // KOL_PERSP_ONMAP_ONLY=1 leaves them out again.
    if (!(ew[c + 2] & 0x80u) && (g_onmap_only || ew[c + 1] == 0u)) {
        // Off the field (byte FF, height 0): not drawn, unless given a
        // height (Debug > Tile Info's editor, or the drawn-height
        // table), when it stands as terrain at it: the art the game
        // draws around a field (walls, roofs, cliffs) then has a shape.
        const int d = drawn[gi(cx, cy)];
        if (d < 0) return -1;
        if (prop) *prop = false;
        return d;
    }
    if (prop) {
        const int rule = record_rule(ew[c]);
        *prop = !bg1_props && (rule > 0 || (rule == 0 && is_prop(ew[c]) && is_obstacle(ew[c])));
    }
    return ew[c + 1];
}

// A neighbour as a prop stands on it: its drawn height where the map
// draws it otherwise (the gargoyle stands in its pool, not on the rim).
inline int SceneBuild::ground_cell(int cx, int cy, bool* prop) {
    const int h = raw_cell(cx, cy, prop);
    if (h < 0) return h;
    const int d = drawn[gi(cx, cy)];
    return d >= 0 ? d : h;
}

// Rock spires (0x09) and the compact props of 0x0A (the snowfield's
// rocks, the ruins' fire pots, CZ_fire_pot) go by the spire rule below.
// KOL_PERSP_NO_POT_PROPS=1 keeps 0x0A on the trees' rule.
inline bool SceneBuild::is_spire(int cx, int cy) {
    static const bool pots = !kol::env_true("KOL_PERSP_NO_POT_PROPS");
    if (!spire_props() || !in_game(cx, cy)) return false;
    const std::uint8_t t = ew[e(kGrid) + game_cell(cx, cy)];
    if (const int rule = record_rule(t)) return rule == 2;   // the record decides
    return is_prop(t) && is_obstacle(t) && (t == 0x09u || (pots && t == 0x0Au));
}

// A box's height over a spot inside its footprint.
inline float SceneBuild::box_h(const Box& b, float X, float Z) {
    if (b.T <= b.E) return b.E;
    const float a = b.along_x ? Z : X, a0 = b.along_x ? b.Z0 : b.X0, a1 = b.along_x ? b.Z1 : b.X1;
    const float half = 0.5f * (a1 - a0);
    return b.T - (b.T - b.E) * std::abs(a - (a0 + half)) / half;
}

// (See roofs_and_shade: whether a point is in the terrain's shadow.)
inline bool SceneBuild::shaded_at(float X, float Z, float y, bool awnings) {
    const int ssx = static_cast<int>(std::floor(X * kShadowSub / 32.0f)) - sh_ox;
    const int ssz = static_cast<int>(std::floor(Z * kShadowSub / 32.0f)) - sh_oz;
    if (ssx < 0 || ssz < 0 || ssx >= sh_w || ssz >= sh_h || !sh_map) return false;
    if (y + 0.5f < sh_map[static_cast<std::size_t>(ssz) * sh_w + ssx]) return true;
    return awnings && any_open &&
           canopy_shaded(static_cast<int>(std::floor(X)), static_cast<int>(std::floor(Z)), y);
}
}  // namespace kol_focus_detail

namespace kol {
// (kol_focus_windows.cpp)
float shutter_light(float open);
void read_shutter(const std::uint8_t* rgb, const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky);
}  // namespace kol

#pragma GCC visibility pop
