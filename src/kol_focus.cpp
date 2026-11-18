// kol_focus.cpp — depth of field on the battle map, following the cursor.
//
// The battle map is a heightfield the guest keeps in EWRAM, and every map
// object carries its world position, so the scene's depth can be rebuilt at
// present time without asking the PPU anything:
//
//   height grid  0x02016440, 32 x 32 cells, 4 bytes each: +0 terrain id,
//                +1 height, +2 flags (0x80 = the cell is on the map). Verified
//                against the terrain box's elevation on 37 cells of one map
//                and 43 of another, zero mismatches. A prop's cell (tree,
//                bush, rock) holds the prop's TOP: trees sit 3 above their
//                neighbours, rocks 2, bushes 1.
//   objects      u32 at 0x0202A238 -> 26 records, stride 0xD0: +0 the unit
//                record, +0x10 u16 world X, Y (up), Z.
//                X = cell_x * 32 + 16, Z = cell_y * 32 + 16, Y = height * 16.
//   projection   screen_x = (X - Z) / 2 + kx
//                screen_y = (X + Z) / 4 - Y / 2 + ky
//                fitted exactly on eight units. (kx, ky) is the camera.
//   camera       kx + BG0HOFS and ky + BG0VOFS are constant for a map, mod
//                the BG's size, 512 x 256: (192, 24), (208, 16) and
//                (352, 176) on three maps. (Taking Y mod 512 split one map's
//                constant into two that swapped places as the camera moved.) A
//                unit's sprite slot holds its world and screen position, so
//                each slot yields that constant -- but the slot list keeps
//                stale entries, and during the enemy turn's camera pans the
//                live ones lead or trail the scroll by a frame; taking the
//                first slot put the terrain 40 px off. So every slot votes,
//                the votes accumulate over frames (the true constant repeats,
//                stale entries do not) and the camera comes from the scroll of
//                the frame being drawn plus the winning constant.
//   cursor       the slot whose routine is 0x08057AD5 carries the grid
//                cursor's world position, gliding between cells. It is absent
//                on the enemy turn and while a command menu is up.
//
// Every pixel gets a place in the scene: its row, X + Z (32 a row of cells;
// constant along a screen row at one height, so things side by side on
// screen stay equally sharp), and its height, Y (16 a step). Terrain is
// rasterized back to front; sprites face the camera, so a unit's sprite takes
// one place, its own. Props are drawn the same way: their cell at its ground
// height, and their own pixels (BG1 -- trees and bushes are on the overlay
// layer) at the place of the cell they stand on. Drawing a prop's cell as a
// solid column instead boxed the tree in a sharp "glass cage".
//
// Blur grows with rows apart PLUS height apart. Camera depth (X + Z + Y) was
// tried first and lets the two cancel: a unit six rows in front and eight
// steps below the cursor came out two rows away, in focus.
//
// The blur grows with the distance from the focus: the cursor; with no
// cursor, the unit that is moving (an enemy walking on its turn); with
// neither, the tile at the centre of the screen, where the camera parks on
// whoever is about to act. It eases toward a new target over a few frames
// rather than jumping.
//
// The work is spread over files, kol_focus_internal.h holding what they
// share: this one has the capture (present_capture), the scene build's
// driver (SceneBuild: the camera and the units, the cells' rules, the
// shade, the terrain pass, the props' light, the atlas, the rest) and the
// sun's API; kol_focus_sprites.cpp whose sprite is whose,
// kol_focus_atlas.cpp the units and props as cards, kol_focus_shadows.cpp
// the sun and the shadows, kol_focus_windows.cpp the window light,
// kol_focus_lamps.cpp the lamps, kol_focus_roofs.cpp the roofs and boxes,
// kol_focus_cutouts.cpp the props' passes, kol_focus_models.cpp the map
// models, kol_focus_present.cpp the providers and the scene's pixels,
// kol_focus_queries.cpp the guest-state queries, and kol_focus_tables.h
// the per-map and per-record data.

#include "kol_focus.h"
#include "kol_focus_internal.h"
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

namespace {
bool cache_store_twice() {   // KOL_CACHE_STORE_TWICE=1 (PassCache)
    static const bool v = kol::env_set("KOL_CACHE_STORE_TWICE");
    return v;
}
}  // namespace

namespace kol_focus_detail {
// The caches' store rule (PassCache, Bg1Cache): an output is kept when its
// key repeats, or at once after a hit (see PassCache::prev_hit).
bool cache_keep(bool key_repeats, bool prev_hit) { return key_repeats || (prev_hit && !cache_store_twice()); }
// The `index`-th (from 1) of this map's placed cut-outs: a plant?
bool placed_plant(int index) {
    const int map = current_map();
    for (const PlacedCutout& pc : kPlacedCutouts)
        if (pc.map == map && --index == 0) return pc.plant;
    return false;
}
const DrawnCutout* drawn_cutout(int map, int record, int cx, int cz) {
    static const bool none = std::getenv("KOL_NO_DRAWN_CUTOUTS") != nullptr;
    if (none) return nullptr;
    for (const DrawnCutout& d : kDrawnCutouts)
        if (d.map == map && d.record == record && (d.cx < 0 || (d.cx == cx && d.cz == cz))) return &d;
    return nullptr;
}
}  // namespace kol_focus_detail

namespace {
// Maps whose BG1 is props and nothing else, which the game's grid does not
// know: map 102 (the opening's night field) is a scene map, its field an
// 11-row band along the river with record 0 on every cell but four, and its
// bushes, rocks and dead trees stand mostly off that band, on the ground the
// user drew past it. There every BG1 shape is a prop (bg1_shape_props), and
// the records' and bytes' rules are off. Its only other BG1 is the small
// triangles on ledge tops, which stay ground. KOL_NO_BG1_PROPS=1: as before.
// (map_settings.cfg `bg1_props`.)
bool bg1_prop_map(int map) {
    static const bool off = std::getenv("KOL_NO_BG1_PROPS") != nullptr;
    if (off) return false;
    return kol::map_flag(kol::MapFlag::Bg1Props, map);
}
}  // namespace

namespace kol_focus_detail {
bool middle_v_cell(int map, int x, int z) {
    static const bool off = std::getenv("KOL_NO_MIDDLE_V_CELLS") != nullptr;
    if (off) return false;
    for (const MiddleVCell& c : kMiddleVCells)
        if (c.map == map && c.x == x && c.z == z) return true;
    return false;
}
}  // namespace kol_focus_detail

namespace {
// +2 and +4 of it, u16: the map's screen offset in tiles, which the sprite
// projection adds (0x080769D0). And the game's camera, u16 x then y, which
// it subtracts (0x08076A28; the scroll target lies at 0x02026C80).
constexpr std::uint32_t kGameCamera = 0x02026C78u;
// A bag left on the map (an item to pick up): its own sprite slot, with a
// world place like a unit's but no map object (states/CP_spell_hand_merged:
// slot 0, owner 0x0202A26C, world 496,40,304 between Radam and Elrik).
constexpr std::uint32_t kBagSlotFn   = 0x0805AD9Du;
}  // namespace

namespace kol_focus_detail {
float glow_strength() {
    static const float env = kol::env_num_min("KOL_PERSP_GLOW", 0.0f, -1.0f);   // -1: not set
    if (env >= 0.0f) return env;
    return kol::setting_on(kol::Setting::Torchlight) ? 1.0f : 0.0f;
}
float glow_reach() {
    static const float v = kol::env_num_min("KOL_PERSP_GLOW_R", 8.0f, 128.0f);
    return v;
}
}  // namespace kol_focus_detail

namespace {
constexpr float kAoStrength = 0.5f;   // 0.7 first; 0.5 with the shadows (the user)
}  // namespace

namespace kol_focus_detail {
float ao_strength() {
    static const char* e = std::getenv("KOL_PERSP_AO");
    if (e) return std::max(0.0f, std::min(1.0f, static_cast<float>(std::atof(e))));
    return kol::ao_reach_units() > 0.0f ? kAoStrength : 0.0f;
}
}  // namespace kol_focus_detail

namespace {
// Maps indoors: no sun there, so no sun shadows (the terrain's, the units',
// the props') and no evening tint; the occlusion, torches and lamps stay
// (2026-10-14, asked for different sun and shadow behaviour indoors, set
// per map; the game keeps no indoor flag, see kol-weather in
// README "Only under a grey sky"). Set on the Thor with the editor's SUN tab,
// INDOOR (`indoor map` lines); copy a map here once settled. -1: none yet.
// KOL_INDOOR=0/1 forces it for a desktop run.
// 51: the ruins, lit by their blue fire pots; 55: the castle, lit by its
// statue's blue flame and the sun through its windows (kDarkMaps).
// 99: the burning town at night (asked 2026-10-17 for a night look, no
// sunlight, only the fires' light), kDarkMaps.
// 101: the intro's castle room, dark, lit through its two windows (HS).
// 58: the ship at night (states/JL), lit by the torch Rictor carries
// (kMapLamps), asked 2026-10-24/25.
// 67: the dungeon cell, lit through its barred door (kMapWindows); 68 the
// same cell flooded, by the moon through its small window.
// 39: the throne hall, lit through its two windows and by three chandeliers.
// (The maps: map_settings.cfg `indoor`.)
// Map 58 at night, everything it took in one switch (the user, 2026-10-26,
// was unsure whether the torches or the earlier look was better): the map
// dark (kIndoorMaps, kDarkMaps), its people's torches (kMapLamps), their
// light and shadows by the ship's model (kShipLightMaps). Off, map 58 is
// the game's own night again. KOL_NIGHT_SHIP=0/1 overrides it.
constexpr bool kNightShip = true;
bool night_ship() {
    static const bool on = [] {
        const char* v = std::getenv("KOL_NIGHT_SHIP");
        return v ? v[0] != '0' : kNightShip;
    }();
    return on;
}
}  // namespace

namespace kol_focus_detail {
// Whether a table's entry for `map` is in force (all but the night ship's
// with its switch off).
bool map_entry_on(int map) { return map != 58 || night_ship(); }
const DarkPool* dark_pool() {
    static DarkPool env{};
    static const bool env_on = [] {
        const char* e = std::getenv("KOL_DARK_POOL");
        if (!e) return false;
        env.ground = 1;
        return std::sscanf(e, "%f,%f,%f,%f,%f,%f", &env.x, &env.z, &env.rx, &env.rz, &env.level, &env.inner) == 6;
    }();
    if (env_on) return &env;
    const int map = current_map();
    if (map < 0) return nullptr;
    return by_map(kDarkPools, map);
}
// With Options > Shadows (the user asked for it to follow the shadow
// switch); off, the room is as dark everywhere.
const DarkCover* dark_cover() {
    const int map = current_map();
    if (map < 0 || kol::shadow_sun_degrees() <= 0.0f) return nullptr;
    const DarkCover* c = by_map(kDarkCovers, map);
    static float env[2] = {0.0f, 0.0f};
    static const bool env_on = [] {
        const char* e = std::getenv("KOL_DARK_COVER");
        return e && std::sscanf(e, "%f,%f", &env[0], &env[1]) == 2;
    }();
    if (!c || !env_on) return c;
    static DarkCover m{};
    m = *c;
    m.level = env[0];
    m.soft = env[1];
    return &m;
}
// The room's evening (kEveningRooms) when the game's time of day is 1-4.
const EveningRoom* evening_room() {
    static const bool off = std::getenv("KOL_NO_EVENING_ROOM") != nullptr;
    const int map = current_map();
    if (off || map < 0) return nullptr;
    const std::uint8_t* ew = guest_ew();
    if (!ew) return nullptr;
    const std::uint8_t tod = ew[0x21BC];
    if (tod < 1 || tod > 4) return nullptr;
    static EveningRoom env{};
    static const bool env_on = [] {
        const char* e = std::getenv("KOL_EVENING_ROOM");
        return e && std::sscanf(e, "%f,%f,%f,%f", &env.dark, &env.degrees, &env.x, &env.z) == 4;
    }();
    for (const EveningRoom& r : kEveningRooms)
        if (r.map == map) {
            if (!env_on) return &r;
            env.map = map;
            return &env;
        }
    return nullptr;
}
const DarkMap* dark_map() {
    const int map = current_map();
    if (map < 0) return nullptr;
    if (const EveningRoom* r = evening_room()) {
        static thread_local DarkMap d{};
        for (const DarkMap& base : kDarkMaps)
            if (base.map == map) d = base;
        d.map = map;
        d.dark = r->dark;
        if (d.lamp_gain == 0.0f) d.lamp_gain = 3.0f;
        return &d;
    }
    // KOL_DARK_MAP=<map,dark,lamp_gain,pot_reach>: one more (mockups).
    static DarkMap env{};
    static const bool env_on = [] {
        const char* e = std::getenv("KOL_DARK_MAP");
        return e && std::sscanf(e, "%d,%f,%f,%f", &env.map, &env.dark, &env.lamp_gain, &env.pot_reach) == 4;
    }();
    if (env_on && env.map == map) return &env;
    for (const DarkMap& d : kDarkMaps)
        if (d.map == map && map_entry_on(map)) return &d;
    return nullptr;
}
// The night colour (5 bits a channel, a scene pixel's top bits) to its
// daylight one in 8 bits a channel, -1 none, for `map`; null when it has
// none (or with the night ship's switch off).
const std::int32_t* lamp_palette_lut(int map) {
    static const bool off = [] {
        const char* e = std::getenv("KOL_LAMP_PALETTE");
        return e && e[0] == '0';
    }();
    static std::vector<std::int32_t> lut;
    static int lut_map = -1;
    if (off || !map_entry_on(map)) return nullptr;
    if (lut_map != map) {
        lut_map = map;
        lut.clear();
        auto x8 = [](unsigned c) { return static_cast<std::int32_t>((c << 3) | (c >> 2)); };
        for (const ColourPair& c : kLampPalette) {
            if (c.map != map) continue;
            if (lut.empty()) lut.assign(32768, -1);
            lut[c.night & 0x7FFFu] = x8(c.day & 31u) | x8((c.day >> 5) & 31u) << 8 | x8((c.day >> 10) & 31u) << 16;
        }
        // A firelit palette (the user's option B, 2026-11-09, map 64's
        // campfire): every colour's own warm grade, x (r, g, b) per channel.
        // KOL_FIRELIT=<map,r,g,b> (mockups).
        static float fl[4] = {-1.0f, 1.0f, 1.0f, 1.0f};
        static const bool fl_read = [] {
            if (const char* e = std::getenv("KOL_FIRELIT")) std::sscanf(e, "%f,%f,%f,%f", &fl[0], &fl[1], &fl[2], &fl[3]);
            return true;
        }();
        (void)fl_read;
        if (lut.empty() && static_cast<int>(fl[0]) == map) {
            lut.assign(32768, -1);
            for (unsigned c = 0; c < 32768u; ++c) {
                std::int32_t out = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    const float v = static_cast<float>(x8((c >> (5 * ch)) & 31u)) * fl[1 + ch];
                    out |= static_cast<std::int32_t>(std::clamp(std::lround(v), 0L, 255L)) << (8 * ch);
                }
                lut[c] = out;
            }
        }
    }
    return lut.empty() ? nullptr : lut.data();
}
// The table for the palette the picture was drawn with (`pal`, BG PAL RAM,
// 256 entries): the scenes darken the palette itself (map 118's narration,
// "In the year 232 ...": rows 0-3 at ~57%), and then no pixel is a night
// colour any more and the swap went off (the user, 2026-11-12). The BG
// entries holding the night colours are noted while the palette is
// undarkened; when they are the night colours times k (their channel sums
// against the night colours'), each one's live colour turns to its day
// colour times k. KOL_LAMP_PALETTE_NO_FADE=1: the plain table only (old).
const std::int32_t* lamp_palette_lut_live(int map, const std::uint8_t* pal) {
    const std::int32_t* base = lamp_palette_lut(map);
    static const bool no_fade = std::getenv("KOL_LAMP_PALETTE_NO_FADE") != nullptr;
    if (!base || !pal || no_fade) return base;
    struct Slot { int index; unsigned night, day; };
    static std::vector<Slot> slots;
    static int slots_map = -1;
    static std::vector<std::int32_t> lut;
    static std::vector<unsigned> lut_keys;
    static std::uint64_t lut_hash = 0;
    auto at = [pal](int i) { return static_cast<unsigned>(pal[i * 2] | (pal[i * 2 + 1] << 8)) & 0x7FFFu; };
    if (slots_map != map) { slots_map = map; slots.clear(); lut_hash = 0; }
    // Note the slots while the palette shows the night colours (the most
    // slots seen: a palette with more of them is the undarkened one).
    std::vector<Slot> now;
    for (int i = 1; i < 256; ++i) {
        if ((i & 15) == 0) continue;
        const unsigned c = at(i);
        for (const ColourPair& p : kLampPalette)
            if (p.map == map && (p.night & 0x7FFFu) == c && c) { now.push_back({i, c, p.day & 0x7FFFu}); break; }
    }
    if (now.size() >= slots.size()) slots = std::move(now);
    if (slots.empty()) return base;
    unsigned live = 0, night = 0;
    bool all_same = true;
    auto ch = [](unsigned c, int n) { return (c >> (5 * n)) & 31u; };
    for (const Slot& s : slots) {
        const unsigned c = at(s.index);
        all_same = all_same && c == s.night;
        for (int n = 0; n < 3; ++n) { live += ch(c, n); night += ch(s.night, n); }
    }
    if (all_same || night == 0u) return base;
    // The fade is not strictly downward: its first ten frames raise a few
    // channels a step while the rest fall (map 118, presents 156-165: the
    // swap went off there, a brief lighting change around the
    // lady). So the test is that the live colours are the night colours
    // times k within 3 steps (nine channels in ten), not "all darker".
    const float k = static_cast<float>(live) / static_cast<float>(night);
    unsigned near = 0;
    for (const Slot& s : slots)
        for (int n = 0; n < 3; ++n)
            near += std::fabs(static_cast<float>(ch(at(s.index), n)) - static_cast<float>(ch(s.night, n)) * k) <= 3.0f;
    if (k > 1.1f || near * 10u < static_cast<unsigned>(slots.size()) * 3u * 9u) return base;
    // Darkened: the table of the live colours, rebuilt when they change.
    std::uint64_t h = 1469598103934665603ull;
    for (const Slot& s : slots) h = (h ^ at(s.index)) * 1099511628211ull;
    if (h != lut_hash || lut.empty()) {
        if (lut.empty()) lut.assign(32768, -1);
        for (unsigned key : lut_keys) lut[key] = -1;
        lut_keys.clear();
        lut_hash = h;
        auto x8 = [](unsigned c) { return static_cast<std::int32_t>((c << 3) | (c >> 2)); };
        for (const Slot& s : slots) {
            const unsigned c = at(s.index);
            std::int32_t v = 0;
            for (int n = 0; n < 3; ++n)
                v |= x8(std::min(31u, static_cast<unsigned>(std::lround(static_cast<float>(ch(s.day, n)) * k)))) << (8 * n);
            if (lut[c] < 0) lut_keys.push_back(c);
            lut[c] = v;
        }
    }
    return lut.data();
}
float lamp_palette_from() {
    static const float v = kol::env_num_clamp("KOL_LAMP_PALETTE_FROM", 0.0f, 0.95f, kLampPaletteFrom);
    return v;
}
// The bump map in the sun (kol_perspective's apply_lighting), on no map:
// tried on map 89 (2026-10-14, 0.3, then softened 2 px and 1 px), the user
// had it turned off there as adding nothing. KOL_BUMP_SUN=<strength>
// puts it on every map in the sun (mockups).
float sun_bump_strength() {
    static const float v = kol::env_num_min("KOL_BUMP_SUN", 0.0f, 0.0f);
    return v;
}
// The maps whose floor is wet (Options > Bump Mapping: kol_perspective's
// stone bumps and slime glints, on a dark map). 51, the ruins; not 55, the
// castle, dark since 2026-10-15 (kept to map 51 for now; it looked poor on
// map 55).
// (map_settings.cfg `wet_floor`.)
bool map_wet_floor() {
    const int map = current_map();
    return map >= 0 && kol::map_flag(kol::MapFlag::WetFloor, map);
}
}  // namespace kol_focus_detail

namespace {
bool map_indoor() {
    static const char* forced = std::getenv("KOL_INDOOR");
    if (forced) return forced[0] != '0';
    const int map = current_map();
    if (map < 0) return false;
    if (window_room_plain()) return false;   // the game's own light (Shadows off)
    if (kol::tile_edit_indoor(map)) return true;
    return kol::map_flag(kol::MapFlag::Indoor, map) && map_entry_on(map);
}
}  // namespace

namespace kol_focus_detail {
const SceneObjects* scene_object_of(const Unit& u) {
    if (u.actor < 0) return nullptr;
    const int map = current_map();
    if (map < 0) return nullptr;
    static const bool bed_card = std::getenv("KOL_BED_CARD") != nullptr;   // =1: Alphonse in bed a card, as before
    for (const SceneObjects& o : kSceneObjects)
        if (o.map == map && u.actor >= o.actors_from && u.actor <= o.actors_to &&
            (o.only_y == kAnyY || (o.only_y == u.y && !bed_card)))
            return &o;
    return nullptr;
}
bool scene_object(const Unit& u) { return scene_object_of(u) != nullptr; }
bool scene_object_in_wall(const Unit& u) {
    const SceneObjects* o = scene_object_of(u);
    return o && o->in_wall;
}
const MapSunset* map_sunset() {
    return by_map(kMapSunsets, current_map());
}
const SceneSunset* scene_sunset() {
    static const bool off = std::getenv("KOL_NO_SCENE_SUNSET") != nullptr;
    if (off) return nullptr;
    return by_map(kSceneSunsets, current_map());
}
}  // namespace kol_focus_detail

namespace {
float g_scene_sunset_live = -1.0f;   // the guest thread's way along (-1 none)
// KOL_SUNSET_STORM_STEP=1: as before (shadows cut in rain, the colours
// greyed by the rain's steps, the evening by the grey sky at once).
bool sunset_storm_step() {
    static const bool v = std::getenv("KOL_SUNSET_STORM_STEP") != nullptr;
    return v;
}
}  // namespace

namespace kol_focus_detail {
bool scene_shadows_keep() {
    const SceneSunset* s = scene_sunset();
    return s && !s->shadow_weather && !sunset_storm_step();
}
}  // namespace kol_focus_detail

namespace {
float g_scene_storm_live = 0.0f;   // the guest thread's eased weather (scene_storm)
float scene_storm_now();
float rain_of(const std::uint8_t* ew);
int rain_level_of(const std::uint8_t* ew, bool on_is_1);
}  // namespace

namespace kol_focus_detail {
// The sunset's colours: its way along squared, greyed by the rain.
float scene_sunset_colour() {
    const float p = scene_sunset_now();
    if (p <= 0.0f) return 0.0f;
    return p * p * (1.0f - 0.7f * scene_storm_now());
}
}  // namespace kol_focus_detail

namespace {
void edge_fade_build(int w, int h, int ox, int oy, int map);
}  // namespace

namespace kol_focus_detail {
// The light warms toward evening as the turns go by (2026-10-12, asked for
// the lighting to turn slightly more orange over time):
// none through turn 6, then rising on an ease-in curve (the square of the
// way along) to the strength at turn 21, and held (asked to start at round
// 6 on a curve rather than linearly; it first rose evenly from
// turn 1). Light reddens fastest as the sun nears the horizon, hence slow
// first. Turn 11: 0.11, 16: 0.44, 21: 1. With Shadows on only.
// KOL_EVENING=<strength> (mockups; 0 off), KOL_EVENING_START=<turn> (6),
// KOL_EVENING_TURNS=<n> (15, the turns from the start to full).
float evening_amount() {
    if (kol::shadow_sun_degrees() <= 0.0f || indoor_now()) return 0.0f;
    static const float strength = kol::env_num_min("KOL_EVENING", 0.0f, 1.0f);
    static const float start = kol::env_num("KOL_EVENING_START", 6.0f);
    static const float turns = kol::env_num_min("KOL_EVENING_TURNS", 1.0f, 15.0f);
    // Under rain or snow the light stays grey: x0.3 (2026-10-12, asked for
    // less orange tint in the rain).
    constexpr float kWeatherEvening = 0.3f;
    const float t = std::max(0.0f, std::min(1.0f, (static_cast<float>(battle_turn()) - start) / turns));
    float a = strength * t * t;
    if (const MapSunset* m = map_sunset()) a = std::max(a, m->evening);
    if (const SceneSunset* s = scene_sunset()) {
        a = std::max(a, s->evening * scene_sunset_colour());   // (greyed there by the eased weather)
        if (!sunset_storm_step()) return a * (1.0f - (1.0f - kWeatherEvening) * scene_storm_now());
    }
    return a * (1.0f - (1.0f - kWeatherEvening) * weather_amount());
}
}  // namespace kol_focus_detail

namespace {
float ao_reach() {
    static const float env = kol::env_num_min("KOL_PERSP_AO_R", 2.0f, -1.0f);   // -1: not set
    if (env >= 0.0f) return env;
    const float r = kol::ao_reach_units();
    return r > 0.0f ? r : 16.0f;
}
}  // namespace

namespace kol_focus_detail {
std::uint32_t g_cap_serial() { return g_cap.serial; }
// The picture's BG0 scroll `now` against the map extent's `then`: the frame
// hook's lead, modulo 256 and folded to -128..127 (why modulo 256: see
// edge_fade_build).
int scroll_lead(int now, int then) {
    const int d = (now - then) & 255;
    return d >= 128 ? d - 256 : d;
}
}  // namespace kol_focus_detail

namespace {
void edge_fade_build(int w, int h, int ox, int oy, int map) {
    g_edge_fade_on = false;
    static const int env_width = kol::env_int("KOL_EDGE_FADE", -1);
    static const char* env_sky = std::getenv("KOL_SKY");
    static const char* env_desat = std::getenv("KOL_SKY_DESAT");
    int width = env_width;
    int sky = kol::kSkyGame;
    float desat = 0.0f;
    float rnd[3] = {-1.0f, 0.0f, 1.0f};   // round: shape, length, scale (MapEdgeFade)
    for (const MapEdgeFade& m : kEdgeFadeMaps)
        if (m.map == map) {
            if (width < 0) width = m.width;
            sky = m.sky;
            desat = m.desat;
            rnd[0] = static_cast<float>(m.round); rnd[1] = m.round_len; rnd[2] = m.round_scale;
        }
    static float rnd_env[3] = {-2.0f, 0.0f, 1.0f};
    static const bool rnd_env_read = [] {
        if (const char* e = std::getenv("KOL_EDGE_FADE_ROUND")) std::sscanf(e, "%f,%f,%f", &rnd_env[0], &rnd_env[1], &rnd_env[2]);
        return true;
    }();
    (void)rnd_env_read;
    if (rnd_env[0] > -2.0f) for (int i = 0; i < 3; ++i) rnd[i] = rnd_env[i];
    if (width < 0) width = 0;
    if (env_sky) sky = std::clamp(std::atoi(env_sky), 0, static_cast<int>(kol::kSkyMeadow));
    if (env_desat) desat = static_cast<float>(std::atof(env_desat));
    g_sky = sky;
    g_sky_desat = std::clamp(desat, 0.0f, 1.0f);
    const Capture& cap = g_cap;
    if (width <= 0 || !cap.extent_ok || !cap.bg0_ok) return;
    const kol::MapExtent& x = cap.extent;
    // The picture's own scroll, not the tick's: the frame hook's registers
    // can lead the presented picture by a frame while the camera moves. That
    // lead is a few pixels, taken modulo 256: every BG size is a multiple of
    // it, and the battle BG is 256 tall (modulo 512 the frame the vertical
    // scroll wrapped, 256 against the tick's 511, read -255: the fade fell
    // across the map for a frame, on map 89 after New Game).
    const int org_x = x.origin_x + scroll_lead(static_cast<int>(cap.bg0_h), x.hofs);
    const int org_y = x.origin_y + scroll_lead(static_cast<int>(cap.bg0_v), x.vofs);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    g_edge_fade.assign(n, 255);
    // The fade is worked out once per map, in the array's own pixels (the
    // cut does not move with the camera, so each frame only looks it up):
    // from each pixel's distance to the nearest pixel past the art (an empty
    // tile, or off the array), Euclidean, so the fade bends round a step in
    // the cut (measured per row first: a seam where a shorter row met a
    // longer one). Over the art's bounding box grown by `width` (worked out
    // per frame over the scene, 1.8 ms on the Mac each time the camera moved).
    constexpr int kC = kol::MapExtent::kCols;
    static std::vector<std::uint8_t> fade;   // 255 all of the map's colour
    static std::uint32_t fade_key = 0;
    static int fx0 = 0, fy0 = 0, fw = 0, fh = 0;
    std::uint32_t key = kol::kFnvBasis;
    kol::fnv_ints(key, {width, x.rows, static_cast<int>(rnd[0]), static_cast<int>(rnd[1] * 16.0f), static_cast<int>(rnd[2] * 1000.0f)});
    for (int i = 0; i < x.rows * kC; ++i) kol::fnv_mix(key, x.filled[i]);
    if (key != fade_key) {
        fade_key = key;
        int c0 = kC, c1 = -1, r0 = x.rows, r1 = -1;
        for (int r = 0; r < x.rows; ++r)
            for (int c = 0; c < kC; ++c)
                if (x.filled[r * kC + c]) {
                    c0 = std::min(c0, c); c1 = std::max(c1, c);
                    r0 = std::min(r0, r); r1 = std::max(r1, r);
                }
        const int m = width;
        fx0 = 8 * c0 - m;
        fy0 = 8 * r0 - m;
        fw = 8 * (c1 - c0 + 1) + 2 * m;
        fh = 8 * (r1 - r0 + 1) + 2 * m;
        auto filled = [&](int ax, int ay) {
            if (ax < 0 || ay < 0) return false;
            const int c = ax >> 3, r = ay >> 3;
            return r < x.rows && c < kC && x.filled[r * kC + c] != 0;
        };
        // Squared distance transform (Felzenszwalb and Huttenlocher), along
        // rows then columns, 0 past the art. In doubles, with "far" a large
        // finite value: at 1e12 in floats the q^2 terms vanish under the
        // rounding and the envelope breaks.
        constexpr double kFar = 1.0e9;
        std::vector<double> grid(static_cast<std::size_t>(fw) * fh, kFar);
        for (int gy = 0; gy < fh; ++gy)
            for (int gx = 0; gx < fw; ++gx)
                if (!filled(fx0 + gx, fy0 + gy)) grid[static_cast<std::size_t>(gy) * fw + gx] = 0.0;
        const std::size_t span = static_cast<std::size_t>(std::max(fw, fh));
        std::vector<double> f(span), d(span), z(span + 1);
        std::vector<int> v(span);
        auto pass = [&](int len) {
            int k = 0;
            v[0] = 0;
            z[0] = -1.0e30;
            z[1] = 1.0e30;
            for (int q = 1; q < len; ++q) {
                auto meet = [&](int p) {
                    return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * (q - p));
                };
                double sx = meet(v[k]);
                while (sx <= z[k]) {
                    --k;
                    sx = meet(v[k]);
                }
                ++k;
                v[k] = q;
                z[k] = sx;
                z[k + 1] = 1.0e30;
            }
            k = 0;
            for (int q = 0; q < len; ++q) {
                while (z[k + 1] < q) ++k;
                const double dq = q - v[k];
                d[q] = dq * dq + f[v[k]];
            }
        };
        for (int gy = 0; gy < fh; ++gy) {
            for (int gx = 0; gx < fw; ++gx) f[gx] = grid[static_cast<std::size_t>(gy) * fw + gx];
            pass(fw);
            for (int gx = 0; gx < fw; ++gx) grid[static_cast<std::size_t>(gy) * fw + gx] = d[gx];
        }
        for (int gx = 0; gx < fw; ++gx) {
            for (int gy = 0; gy < fh; ++gy) f[gy] = grid[static_cast<std::size_t>(gy) * fw + gx];
            pass(fh);
            for (int gy = 0; gy < fh; ++gy) grid[static_cast<std::size_t>(gy) * fw + gx] = d[gy];
        }
        // To the cut, which lies between pixels; smoothstep over `width`.
        // Past the art all faded (0): nothing of the map's is there, the sky
        // is left as it is (the fade passes skip the backdrop), and a sprite
        // over it is hidden in the sky (the user, 2026-11-13, map 93's birds
        // flying in over the green: asked for sprites under the fade to be
        // hidden by it). Until then 255 there, and past the grid
        // too: a sprite out there was drawn whole. KOL_EDGE_FADE_SPRITES_OUT=1:
        // as before.
        static const bool keep_out = std::getenv("KOL_EDGE_FADE_SPRITES_OUT") != nullptr;
        fade.assign(grid.size(), 255);
        const float inv = 1.0f / static_cast<float>(width);
        for (std::size_t i = 0; i < grid.size(); ++i) {
            if (grid[i] <= 0.0) {
                if (!keep_out) fade[i] = 0;
                continue;
            }
            const float dd = static_cast<float>(std::sqrt(grid[i])) - 0.5f;
            if (dd >= static_cast<float>(width)) continue;
            const float t = std::max(0.0f, dd) * inv;
            fade[i] = static_cast<std::uint8_t>(std::lround(255.0f * t * t * (3.0f - 2.0f * t)));
        }
        // A round fade (MapEdgeFade::round; the user 2026-11-10 on map 64
        // asked for a circular, smoother fade, accepting a longer one): by the distance from the art's middle, full
        // inside the radius less its length, none at the radius; the edge's
        // fade still bounds it. KOL_EDGE_FADE_ROUND=<shape,length,scale>
        // (mockups; shape -1 none).
        if (rnd[0] >= 0.0f && rnd[1] > 0.0f) {
            const float cx = 4.0f * (c0 + c1 + 1) - static_cast<float>(fx0), cy = 4.0f * (r0 + r1 + 1) - static_cast<float>(fy0);
            const float hw = 4.0f * (c1 - c0 + 1), hh = 4.0f * (r1 - r0 + 1);
            const float rx = (rnd[0] >= 1.0f ? hw : hh) * rnd[2], ry = hh * rnd[2];
            const float len = rnd[1];
            for (int gy = 0; gy < fh; ++gy)
                for (int gx = 0; gx < fw; ++gx) {
                    const float dx = (gx + 0.5f - cx) / rx, dy = (gy + 0.5f - cy) / ry;
                    const float r = std::sqrt(dx * dx + dy * dy) * ry;   // (in pixels along the short axis)
                    const float t = std::clamp((ry - r) / len, 0.0f, 1.0f);
                    const std::uint8_t v = static_cast<std::uint8_t>(std::lround(255.0f * t * t * (3.0f - 2.0f * t)));
                    std::uint8_t& f0 = fade[static_cast<std::size_t>(gy) * fw + gx];
                    f0 = std::min(f0, v);
                }
        }
    }
    bool any = false;
    // Past the fade's grid (the art and the edge's width round it) all of it
    // faded too, not left whole: first for a round fade (the snow's flakes
    // there, which read the fade, kol_snow, fell in the black at the view's
    // corners), now for every fade (a sprite over the sky, above).
    static const bool keep_out = std::getenv("KOL_EDGE_FADE_SPRITES_OUT") != nullptr;
    const bool round_out = (rnd[0] >= 0.0f && rnd[1] > 0.0f) || !keep_out;
    for (int py = 0; py < h; ++py) {
        const int gy = py - oy + org_y - fy0;
        if (gy < 0 || gy >= fh) {
            if (round_out) { std::fill_n(&g_edge_fade[static_cast<std::size_t>(py) * w], w, 0); any = true; }
            continue;
        }
        for (int px = 0; px < w; ++px) {
            const int gx = px - ox + org_x - fx0;
            if (gx < 0 || gx >= fw) {
                if (round_out) { g_edge_fade[static_cast<std::size_t>(py) * w + px] = 0; any = true; }
                continue;
            }
            const std::uint8_t f = fade[static_cast<std::size_t>(gy) * fw + gx];
            if (f == 255) continue;
            g_edge_fade[static_cast<std::size_t>(py) * w + px] = f;
            any = true;
        }
    }
    g_edge_fade_on = any;
}
}  // namespace

namespace kol_focus_detail {
float sun_arc_now() { return t_present ? g_cap.sun_arc : g_sun_arc; }
}  // namespace kol_focus_detail

namespace {
// Rain or snow falling: the game's weather particles (kol_obj_cull, ten
// 0x14-byte entries at 0x0202A0F8, +0x04 nonzero when live), held for
// kWeatherHold frames past the last drop, so the strength (a key of the
// shadow map's cache) does not flicker with the drops' count. Only under an
// overcast sky (2026-10-14, asked: map 6 rains under its blue sky too, GO,
// and there the shadows and sun colour should stay as normal; under
// its grey sky and darker palette, GN, as before). The game keeps no such
// flag: its weather block after the drops (0x0202A1C0) holds the drops'
// count and an intensity 0..2 (+0xCE), and map 6's heavier rain (V) is
// under the blue sky. The grey is the palette the battle loaded: the
// backdrop in the game's own palette buffer (0x02002400; the latched
// palette's entry 0 is the sky gradient's last line, light cyan in GO), a
// near-neutral grey, its 5-bit components within 4 (GN, BX 5,7,7; H 5,6,7).
// Blue skies (GO 1,8,31; the snowfield 0,21,31, so its snow keeps the
// normal light), DF's pale blue and FU's green are not. A near-black
// backdrop (a fade) keeps the last verdict. KOL_WEATHER_ANY_SKY=1: as before.
int g_weather_hold = 0;
float g_weather_amt = 0.0f;   // the weather look eased (weather_amount)
bool g_overcast = false;
constexpr std::uint32_t kWeatherBlock = 0x0202A1C0u;   // the drops' count and intensity (see above)
}  // namespace

namespace kol_focus_detail {
// The weather look's share, 0..1, eased in and out (the captured frame's).
// KOL_WEATHER=0/1 forces it off or whole.
float weather_amount() {
    static const char* forced = std::getenv("KOL_WEATHER");
    if (forced) return forced[0] != '0' ? 1.0f : 0.0f;
    return t_present ? g_cap.weather_amt : g_weather_amt;
}
}  // namespace kol_focus_detail

namespace {
bool g_indoor_live = false;   // map_indoor() at the last capture
}  // namespace

namespace kol_focus_detail {
bool indoor_now() { return t_present ? g_cap.indoor : g_indoor_live; }
// An indoor map's darkness where no lamp reaches (ScenePixels::dark), 1 none.
// KOL_INDOOR_DARK=<brightness 0..1> (mockups).
float indoor_dark() {
    static const float v = [] {
        const char* e = std::getenv("KOL_INDOOR_DARK");
        return e ? std::max(0.0f, std::min(1.0f, static_cast<float>(std::atof(e)))) : -1.0f;
    }();
    if (!indoor_now()) return 1.0f;
    if (v >= 0.0f) return v;
    const DarkMap* d = dark_map();
    // No light but the lamps (map 63): only with the lamps on (Options >
    // Torchlight); without them the map was black, units too.
    if (d && d->dark <= 0.0f && glow_strength() <= 0.0f) return 1.0f;
    return d ? d->dark : 1.0f;
}
float indoor_lamp_gain() {
    static const float v = kol::env_num_min("KOL_INDOOR_LAMP", 0.0f, -1.0f);
    if (v >= 0.0f) return v;
    const DarkMap* d = dark_map();
    return d ? d->lamp_gain : 1.5f;
}
float sun_turn_now() { return t_present ? g_cap.sun_turn : g_sun_turn; }
// The sun circling low where the rain rises (the user, 2026-11-12, map 57,
// asked for the shadows to circle with a low sun, about 20 degrees, while the
// upward rain runs): once round in
// KOL_CIRCLE_SUN_SECONDS (30), turned every second picture.
bool circling_sun(float* turn) {
    static const bool off = kol::env_set("KOL_NO_CIRCLE_SUN");
    if (off || !kol::map_flag(kol::MapFlag::RainRises, current_map())) return false;
    const int level = t_present ? g_cap.rain_level : rain_level_of(guest_ew(), false);
    if (level < 0) return false;
    static const float seconds = kol::env_num_min("KOL_CIRCLE_SUN_SECONDS", 1.0f, 30.0f);
    const unsigned long serial = (t_present ? g_cap.serial : g_frame_serial) & ~1ul;
    if (turn) *turn = std::fmod(static_cast<float>(serial) * 360.0f / (seconds * 60.0f), 360.0f);
    return true;
}
const std::uint8_t* guest_ew() {
    if (t_present) return g_cap.valid ? g_cap.ew.data() : nullptr;
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->ewram_ptr() : nullptr;
}
const std::uint8_t* guest_iw() {
    if (t_present) return g_cap.valid ? g_cap.iw.data() : nullptr;
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->iwram_ptr() : nullptr;
}
}  // namespace kol_focus_detail

namespace {
// The terrain pass's arrays, kept from the last frame whose inputs were the
// same (build_scene_now: the view, the camera, the grid, the lighting), so a
// still camera does not rasterise every cell again. Taken only once a key
// repeats, so a scrolling frame pays no copy. KOL_PASS_CACHE=0 turns it
// off; KOL_PASS_CACHE_CHECK=1 runs the pass anyway and counts differences.
struct PassCache : CacheStats {
    std::uint32_t key = 0;
    bool valid = false;
    // The last frame's was a hit: inputs that held still and changed once
    // (map 102's sunset moves the sun every 6th frame) are kept at once, not
    // worked out twice. KOL_CACHE_STORE_TWICE=1: kept only when they repeat.
    bool prev_hit = false;
    PassBuffers buf;
    unsigned parts = 0;   // the optional parts it holds (PassBuffers::kWxz...)
};
PassCache g_pass;

// BG0's scroll from the frame hook: the live registers, which pair with the
// sprite slot list the camera votes from. The picture on screen is the
// latched frame, drawn with the scroll before: a game scrolling from its
// VBlank handler has written the next one by the hook. g_pic_hofs/vofs is the
// picture's (gbarecomp latched_bg0_scroll), which turns the votes into the
// picture's camera. With the hook's, the scene was a scroll step ahead of the
// picture while the camera moved: the map's edge pixels went unplaced, and
// the perspective's backdrop carried them along their rows as streaks in the
// sky (states/CW_sky_streaks, Up). KOL_PERSP_LIVE_SCROLL=1 uses the hook's.
bool g_scroll_valid = false;
int g_hofs = 0, g_vofs = 0;

void add_vote(int cx, int cy, unsigned weight = 1u) {
    Vote* weakest = &g_focus.votes[0];
    for (Vote& v : g_focus.votes) {
        if (v.n && v.cx == cx && v.cy == cy) {
            if ((v.n += weight) >= 4096u)
                for (Vote& d : g_focus.votes) d.n >>= 1;   // stay responsive
            return;
        }
        if (v.n < weakest->n) weakest = &v;
    }
    *weakest = Vote{cx, cy, weight};
}

// The value congruent to `v` mod `period` that is nearest `near`.
int unwrap(int v, int period, int near) {
    return v + period * static_cast<int>(std::lround((near - v) / static_cast<double>(period)));
}
}  // namespace

namespace kol_focus_detail {
// (First written for the windows on the map now, kMapWindows.)
int current_map() {
    const std::uint8_t* ew = guest_ew();
    return ew ? rd16(ew, kMapNumber - 0x02000000u) : -1;
}
float scene_sunset_now() { return t_present ? g_cap.scene_sunset : g_scene_sunset_live; }
}  // namespace kol_focus_detail

namespace {
// The rain's strength in EWRAM's weather block (ScenePixels::rain): 0 none
// or snow, 1 the game's heaviest.
// The game's rain intensity 0..2 while its drops fall, -1 none (or snow).
// +4 is 3, not 1, for a while after some loads (JM: 72 frames, drops
// falling): any nonzero is rain here, as for hiding the game's drops
// (kol_obj_cull), or they showed until it turned 1. rain_of keeps 1 only.
int rain_level_of(const std::uint8_t* ew, bool on_is_1 = false) {
    if (!ew) return -1;
    const std::uint8_t* wb = ew + (kWeatherBlock - 0x02000000u);
    bool falling = false;   // a live drop (the drops' table before the block)
    for (int i = 0; i < 10 && !falling; ++i) falling = wb[-0xC8 + i * 0x14 + 4] != 0;
    if (falling && (on_is_1 ? wb[4] == 1u : wb[4] != 0u) && wb[7] != 3u && wb[0] >= 1u && wb[0] <= 10u && wb[6] <= 2u)
        return wb[6];
    return -1;
}
// The game's snow level 0..2 while its flakes fall (+7 3), -1 none (or
// rain): 0 five flakes falling slowly, 1 ten, 2 a blizzard of ten (kol_snow).
int snow_level_of(const std::uint8_t* ew) {
    if (!ew) return -1;
    const std::uint8_t* wb = ew + (kWeatherBlock - 0x02000000u);
    bool falling = false;
    for (int i = 0; i < 10 && !falling; ++i) falling = wb[-0xC8 + i * 0x14 + 4] != 0;
    if (falling && wb[4] != 0u && wb[7] == 3u && wb[0] >= 1u && wb[0] <= 10u && wb[6] <= 2u) return wb[6];
    return -1;
}
float rain_of(const std::uint8_t* ew) {
    const int level = rain_level_of(ew, true);
    return level < 0 ? 0.0f : static_cast<float>(level) / 2.0f;
}
float scene_storm_now() { return t_present ? g_cap.scene_storm : g_scene_storm_live; }

bool build_scene_now(const std::uint8_t* layers, int w, int h);

// Where the view is in the w x h picture being presented: all of it, unless
// it is a scene composed past the view (gbarecomp's g_present_frame_view).
void frame_view(int w, int h, int* vx, int* vy, int* vw, int* vh) {
    const int* v = g_cap.view;
    if (v[2] >= 240 && v[3] >= 160 && v[0] >= 0 && v[1] >= 0 &&
        v[0] + v[2] <= w && v[1] + v[3] <= h) {
        *vx = v[0]; *vy = v[1]; *vw = v[2]; *vh = v[3];
    } else {
        *vx = 0; *vy = 0; *vw = w; *vh = h;
    }
}
}  // namespace

namespace kol_focus_detail {
bool build_scene(const std::uint8_t* layers, int w, int h) {
    static unsigned long built_serial = ~0ul;
    static const std::uint8_t* built_layers = nullptr;
    static int built_w = 0, built_h = 0, built_vx = 0, built_vy = 0;
    static bool built_ok = false;
    int vx, vy, vw, vh;
    frame_view(w, h, &vx, &vy, &vw, &vh);
    if (built_serial == g_cap.serial && built_layers == layers &&
        built_w == w && built_h == h && built_vx == vx && built_vy == vy)
        return built_ok;
    built_ok = build_scene_now(layers, w, h);
    built_serial = g_cap.serial;
    built_layers = layers;
    built_w = w;
    built_h = h;
    built_vx = vx;
    built_vy = vy;
    return built_ok;
}
}  // namespace kol_focus_detail

namespace {
// While the combat camera is on (kol_perspective.cpp), every sprite
// belongs to the action: a shape of sprite pixels that a unit claimed
// any of takes that unit's place whole, and one no unit touches takes
// the nearest unit's. Otherwise text drawn over a unit -- GUARD on a
// counter-hit is wider than the unit's box -- tore where the box ended:
// half of it moved with the unit, half stayed flat, and the zoom pulled
// the halves apart ("GUARL", states/BV_attack_confirm.state). Shapes
// within 2 px of each other are one (the digits of a damage number are
// apart, and each went to a different unit). Pixels a unit claimed keep
// it -- two units side by side are one shape, and giving the whole shape
// the front one's place moved the other's sprite against the wall -- and
// the rest take the place of the nearest claimed pixel. A shape no unit
// touches takes the unit whose body is nearest, if it is near one (a
// damage number over a head, a slash beside a unit); farther ones stay
// interface, flat, as at rest. Raindrops joined too at first, and an
// overlay wins the corners it shares, so each drop pulled the terrain
// around it to the unit's place (the user reported the rain disturbing the
// terrain). At rest nothing changes here: menus and their text stay flat.
void combat_overlays(const Capture& cap, const std::uint8_t* layers, int w, int h, const Unit* units, int count,
                     int ox, int oy, int kx, int ky) {
    // (Both masks sized and cleared at their first mark: almost never set.)
    g_sharp_any = false;
    g_ovl_card_any = false;
    auto set_sharp = [&](std::size_t k) {
        if (!g_sharp_any) g_sharp.assign(static_cast<std::size_t>(w) * h, 0);
        g_sharp_any = true;
        g_sharp[k] = 1;
    };
    // Each piece of interface the combat camera gives a unit's place is a
    // card of its own (ScenePixels::SpriteBlock::overlay), not pixels of the
    // map's mesh: as mesh pixels an overlay won every corner it shared with
    // the ground around it, so that ground was pulled to the piece's place
    // and the number and the drops beside a unit stood tied to the ground
    // behind them, spikes of it at their edges under the mesh paint
    // (states/HZ_slot2_damage_30_broken.state, presents 96-170). The
    // pixels go back to the ground under them (g_under_*), which the scene
    // shows there. KOL_PERSP_OVERLAY_MESH=1: as mesh pixels, as before.
    static const bool overlay_cards_env = std::getenv("KOL_PERSP_OVERLAY_MESH") == nullptr;
    const bool overlay_cards = overlay_cards_env && g_atlas_per_row > 0 &&
                               g_lift.size() == static_cast<std::size_t>(w) * h &&
                               g_under_xz.size() == static_cast<std::size_t>(w) * h;
    auto overlay_card = [&](const std::vector<std::uint32_t>& px, int xz, int y) {
        constexpr int S = kol::ScenePixels::kSpriteSlot;
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        for (const std::uint32_t k : px) {
            const int qx = static_cast<int>(k % w), qy = static_cast<int>(k / w);
            x0 = std::min(x0, qx); x1 = std::max(x1, qx);
            y0 = std::min(y0, qy); y1 = std::max(y1, qy);
        }
        if (x1 < 0) return;
        for (int ty = y0; ty <= y1; ty += S)
            for (int tx = x0; tx <= x1; tx += S) {
                if (static_cast<int>(g_blocks.size()) >= kMaxLiftBlocks) return;   // (left as mesh pixels)
                const int bw = std::min(S, x1 + 1 - tx), bh = std::min(S, y1 + 1 - ty);
                int ax0, ay0;
                const int b = atlas_new_square(&ax0, &ay0, 0, true);
                bool any = false;
                for (const std::uint32_t k : px) {
                    const int qx = static_cast<int>(k % w), qy = static_cast<int>(k / w);
                    if (qx < tx || qx >= tx + bw || qy < ty || qy >= ty + bh) continue;
                    const std::size_t a = static_cast<std::size_t>(ay0 + qy - ty) * g_atlas_w + ax0 + qx - tx;
                    g_atlas_opaque[a] = 1;
                    any = true;
                    if (!g_lift[k]) {
                        g_lift[k] = static_cast<std::uint8_t>(b + 1);   // its colour: the scene's (kol_perspective)
                    } else {
                        // Over a lifted prop (the 3 over the stone): the prop
                        // keeps the pixel; the card's texel from the frame.
                        if (g_scene_rgb) std::copy_n(g_scene_rgb + static_cast<std::size_t>(k) * 3, 3, &g_atlas_rgb[a * 3]);
                        if (!g_ovl_card_any) g_ovl_card.assign(static_cast<std::size_t>(w) * h, 0);
                        g_ovl_card[k] = 1;
                        g_ovl_card_any = true;
                    }
                    g_xz[k] = g_under_xz[k];
                    g_y[k] = g_under_y[k];
                    g_kind[k] = g_under_kind[k];
                }
                if (!any) continue;
                kol::ScenePixels::SpriteBlock blk{tx, ty, bw, bh, b, xz, y, 1.0e9f, 1.0f};
                blk.overlay = true;
                g_blocks.push_back(blk);
                g_atlas_on = true;
            }
    };
    if (cap.combat_camera && count > 0) {
        const std::size_t pixels = static_cast<std::size_t>(w) * h;
        auto sprite = [&](std::size_t k) { return scene_top(layers, k) == kol::kLayerObj; };
        g_shape.assign(pixels, 0);
        std::vector<std::uint32_t>& members = g_shape_members;
        for (std::size_t k0 = 0; k0 < pixels; ++k0) {
            if (!sprite(k0) || g_shape[k0]) continue;
            members.clear();
            members.push_back(static_cast<std::uint32_t>(k0));
            g_shape[k0] = 1;
            int claimed = -1;
            int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
            for (std::size_t head = 0; head < members.size(); ++head) {
                const std::uint32_t k = members[head];
                const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
                if (!g_iface[k] && (claimed < 0 || g_xz[k] > g_xz[claimed]))
                    claimed = static_cast<int>(k);   // the frontmost claim
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
                for (int qy = std::max(0, py - 2); qy <= std::min(h - 1, py + 2); ++qy) {
                    for (int qx = std::max(0, px - 2); qx <= std::min(w - 1, px + 2); ++qx) {
                        const std::size_t m = static_cast<std::size_t>(qy) * w + qx;
                        if (g_shape[m] || !sprite(m)) continue;
                        g_shape[m] = 1;
                        members.push_back(static_cast<std::uint32_t>(m));
                    }
                }
            }
            int xz = 0, y = 0;
            if (claimed >= 0) {
                // Out from the claimed pixels through the shape, nearest first.
                std::vector<std::uint32_t>& front = g_shape_front;
                front.clear();
                for (const std::uint32_t k : members) {
                    g_shape[k] = g_iface[k] ? 2 : 3;   // 2: to place, 3: placed
                    if (!g_iface[k]) front.push_back(k);
                }
                for (std::size_t head = 0; head < front.size(); ++head) {
                    const std::uint32_t k = front[head];
                    const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
                    for (int qy = std::max(0, py - 2); qy <= std::min(h - 1, py + 2); ++qy) {
                        for (int qx = std::max(0, px - 2); qx <= std::min(w - 1, px + 2); ++qx) {
                            const std::size_t m = static_cast<std::size_t>(qy) * w + qx;
                            if (g_shape[m] != 2) continue;
                            g_shape[m] = 3;
                            g_xz[m] = g_xz[k];
                            g_y[m] = g_y[k];
                            g_kind[m] = kol::kSceneOverlay;
                            if (!overlay_cards) set_sharp(m);
                            g_iface[m] = 0;
                            front.push_back(static_cast<std::uint32_t>(m));
                        }
                    }
                }
                // Each piece of interface in the shape (a word, a number;
                // pixels within 2 px) is then one card, at the place of its
                // pixel nearest the unit (the first reached): pixel by pixel,
                // the letters of GUARD over the griffin took the depths of
                // the parts of its body nearest each and the warp bent them
                // (states/FF_slot1_flying_shadow_guard).
                std::vector<std::uint32_t>& piece = g_shape_piece;
                for (const std::uint32_t s0 : front) {
                    if (g_shape[s0] != 3 || g_kind[s0] != kol::kSceneOverlay) continue;
                    g_shape[s0] = 4;   // 4: set as one card
                    piece.clear();
                    piece.push_back(s0);
                    for (std::size_t head = 0; head < piece.size(); ++head) {
                        const std::uint32_t k = piece[head];
                        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
                        for (int qy = std::max(0, py - 2); qy <= std::min(h - 1, py + 2); ++qy)
                            for (int qx = std::max(0, px - 2); qx <= std::min(w - 1, px + 2); ++qx) {
                                const std::size_t m = static_cast<std::size_t>(qy) * w + qx;
                                if (g_shape[m] != 3 || g_kind[m] != kol::kSceneOverlay) continue;
                                g_shape[m] = 4;
                                g_xz[m] = g_xz[s0];
                                g_y[m] = g_y[s0];
                                piece.push_back(static_cast<std::uint32_t>(m));
                            }
                    }
                    if (overlay_cards) overlay_card(piece, g_xz[s0], g_y[s0]);
                }
            } else if (cap.spell_camera) {
                // In a spell, a shape no unit claims is the spell's own
                // drawing: it stays interface, and kol_perspective draws it
                // with the rest of the effect as one card on its target
                // (ScenePixels::spell_card). Joined to the nearest unit (in
                // reach, 64 rows over Alphonse's feet) Summon Darkness's
                // circle stood in the scene as a card at his depth on a
                // pillar of terrain, behind the card's own circle.
                continue;
            } else {
                // The unit whose body (16 px above its feet) is nearest the
                // bottom of the shape, if the shape lies within reach of it:
                // up to 32 px either side and 64 above its feet.
                const int cxp = (bx0 + bx1) / 2;
                long best = -1;
                xz = 0; y = 0;
                for (int i = 0; i < count; ++i) {
                    const Unit& u = units[i];
                    const int fx = feet_sx(u) + kx + ox;
                    const int feet = feet_sy(u) + ky + oy;
                    if (bx1 < fx - 32 || bx0 > fx + 32 || by1 < feet - 64 || by0 > feet + 8)
                        continue;
                    const int fy = feet - 16;
                    const long d = static_cast<long>(fx - cxp) * (fx - cxp) +
                                   static_cast<long>(fy - by1) * (fy - by1);
                    if (best < 0 || d < best) { best = d; xz = u.x + u.z; y = u.y; }
                }
                if (best < 0) continue;   // near no unit: stays interface
                // Interface stays in front: scenery nearer than the unit (a
                // torch's flame) must not pull a digit's corners.
                for (const std::uint32_t k : members) {
                    g_xz[k] = xz;
                    g_y[k] = y;
                    g_kind[k] = kol::kSceneOverlay;
                    if (!overlay_cards) set_sharp(k);
                    g_iface[k] = 0;
                }
                if (overlay_cards) overlay_card(members, xz, y);
            }
        }
    }
}
}  // namespace

namespace {
// A unit standing past the game's own screen until the scene walks it in:
// hidden while it waits there (in the wide view it stood in plain sight, off
// the board's edge, where the game's 240 x 160 never showed it), then faded
// in (a card fade) over the last kWaitFadeFrames frames of the scene's wait
// before its walk. Map 95 (states/thor/0244): the Shadow, actor 0x27, at
// (208, 304, 16) beyond the board; the main thread (ctx 0x02028ED0: +0 pc,
// +2 the frames left, +0x10 the script) waits at op 16 27 41 (0x41 frames),
// then 0A 27 00 12 walks it to the rider. The user, 2026-11-12, asked for the
// unit at the map's border to fade in before it walks to the
// rider. KOL_NO_WAIT_FADE=1: as before.
struct WaitFade { int map; std::uint32_t script; int wait_pc; std::uint8_t actor; };
constexpr WaitFade kWaitFades[] = {{95, 0x08771A20u, 0x7B1, 0x27}};
constexpr int kWaitFadeFrames = 30;
constexpr std::uint32_t kSceneCtx = 0x02028ED0u;
float wait_fade(const std::uint8_t* ew, int map, std::uint8_t actor) {
    static const bool off = kol::env_set("KOL_NO_WAIT_FADE");
    if (off || !ew) return 1.0f;
    for (const WaitFade& w : kWaitFades) {
        if (w.map != map || w.actor != actor) continue;
        const std::uint32_t c = kSceneCtx - 0x02000000u;
        const std::uint32_t script = static_cast<std::uint32_t>(ew[c + 0x10] | ew[c + 0x11] << 8 | ew[c + 0x12] << 16 |
                                                                static_cast<std::uint32_t>(ew[c + 0x13]) << 24);
        if (script != w.script) return 1.0f;   // (another script: the game's way)
        const int pc = static_cast<std::int16_t>(ew[c] | ew[c + 1] << 8);
        const int left = ew[c + 2] | ew[c + 3] << 8;
        if (pc < w.wait_pc) return 0.0f;
        if (pc == w.wait_pc) return std::clamp(1.0f - static_cast<float>(left) / kWaitFadeFrames, 0.0f, 1.0f);
        return 1.0f;
    }
    return 1.0f;
}
}  // namespace

namespace kol_focus_detail {
// The camera and the units (false: no scene this frame).
bool SceneBuild::camera_and_units() {
    // The scene's own layer codes, for the scene's layer map only (the depth
    // of field without the perspective passes the frame's).
    {
        // The layers given are the latched scene's when they are its size
        // (without overscan the scene's layer map is the frame's own).
        g_scene_codes = cap.under_ok && w == cap.scene_w && h == cap.scene_h
                            ? cap.codes.data() : nullptr;
        static const bool live = kol::env_true("KOL_PERSP_LIVE_SCROLL");
        if (!live && cap.bg0_ok) {
            g_pic_hofs = static_cast<int>(cap.bg0_h);
            g_pic_vofs = static_cast<int>(cap.bg0_v);
        } else {
            g_pic_hofs = cap.hofs;
            g_pic_vofs = cap.vofs;
        }
    }
    if (!cap.valid || w < 240 || h < 160) return false;
    ew = cap.ew.data();
    map = cap.map;   // (these bytes' map number, read at capture)
    iw = cap.iw.data();
    const bool scroll_valid = cap.scroll_valid;
    const int hofs = cap.hofs, vofs = cap.vofs;

    // Units, and which of them moved since the last present. A new map (a
    // different height grid) starts everything over.
    const std::uint32_t list = rd32(ew, e(kObjectsPtr));
    if (!in_ewram(list, kObjects * kObjStride)) return false;
    std::uint32_t grid_hash = kol::kFnvBasis;
    for (int c = 0; c < kGameSide * kGameSide; ++c)
        kol::fnv_mix(grid_hash, ew[e(kGrid) + c * 4 + 1]);
    if (g_focus.list != list || g_focus.grid_hash != grid_hash) {
        g_focus = Focus{};
        g_focus.list = list;
        g_focus.grid_hash = grid_hash;
    }
    for (int i = 0; i < kObjects; ++i) {
        const std::uint32_t o = e(list + i * kObjStride);
        const std::uint32_t rec = rd32(ew, o);
        if (!in_ewram(rec, 0x68)) {
            g_focus.pos_valid[i] = false;
            continue;
        }
        Unit u{rd16(ew, o + 0x10), rd16(ew, o + 0x12), rd16(ew, o + 0x14), 0};
        // On the model's deck (kUnitDecks), along the ray through its feet.
        {
            static const bool off = kol::env_true("KOL_NO_UNIT_DECK");
            if (!off)
                for (const UnitDeck& d : kUnitDecks)
                    if (d.map == map && u.y == d.from) {
                        const int up = d.to - d.from;
                        u.x += up;
                        u.y += up;
                        u.z += up;
                    }
        }
        u.depth = draw_depth(u.x, u.y, u.z);
        u.actor = ew[e(rec) + 0x4E];
        u.fade = kol::gulls_unit_fade(ew, ew[e(rec) + 0x4E], static_cast<std::int16_t>(u.x)) *
                 wait_fade(ew, map, ew[e(rec) + 0x4E]);
        int* prev = g_focus.pos[i];
        if (g_focus.pos_valid[i] && (prev[0] != u.x || prev[1] != u.y || prev[2] != u.z)) {
            moved = count;
            ++moved_count;
        }
        prev[0] = u.x; prev[1] = u.y; prev[2] = u.z;
        g_focus.pos_valid[i] = true;
        {
            // The actor pool: [0x02026F08] + 0x2020, 0x3C8 bytes an actor.
            const std::uint32_t pool = rd32(ew, e(0x02026F08u)) + 0x2020u;
            const std::uint32_t actor = rd32(ew, o + 0x08);
            if (in_ewram(pool, 0x3C8) && actor >= pool && (actor - pool) % 0x3C8u == 0u &&
                (actor - pool) / 0x3C8u < 16u)
                u.block = static_cast<int>((actor - pool) / 0x3C8u);
        }
        units[count++] = u;
    }

    // Output pixel (px, py) is hardware (px - ox, py - oy): KoL's margins are
    // symmetric about the view, which may sit inside a larger scene.
    frame_view(w, h, &vx, &vy, &vw, &vh);
    ox = vx + (vw - 240) / 2;
    oy = vy + (vh - 160) / 2;
    edge_fade_build(w, h, ox, oy, map);

    // The camera: every unit slot votes for the map's constant (see the top
    // of the file). The cursor comes from its own slot.
    //
    // Only a unit on screen votes (32 px of slack for its sprite). Slots are
    // handed out in call order, so on the frames a blinking unit (the one on
    // a deployment square) is not drawn, its slot holds another sprite that
    // names the same unit as owner: on BT_persp_crazy (56, 276) instead of
    // (120, 100), which implies a camera 176 rows off. With that unit the
    // only voter the two tied, the camera flipped each frame, and the
    // perspective and the depth of field jumped with it.
    // The game's own camera, as its sprite emitter projects (0x080769D0 then
    // 0x08076A28): screen x = ((X - Z) >> 1) + 8 + 8 * the map's x offset
    // (u16 0x02029D42) - the camera's x (u16 0x02026C78) + 256, and y =
    // ((X + Z) >> 2) - (Y >> 1) - 8 + 8 * the y offset (0x02029D44) - the
    // camera's y (0x02026C7A) + 256, as u16s.
    auto game_camera = [&](int* gkx, int* gky) {
        *gkx = static_cast<std::int16_t>(static_cast<std::uint16_t>(
            8 + 8 * rd16(ew, e(kMapNumber + 2)) - rd16(ew, e(kGameCamera)) + 256));
        *gky = static_cast<std::int16_t>(static_cast<std::uint16_t>(
            -8 + 8 * rd16(ew, e(kMapNumber + 4)) - rd16(ew, e(kGameCamera + 2)) + 256));
    };
    // Votes only while the camera rests. As it pans, the scroll registers
    // trail the game's camera by up to two frames and the cursor's slot is
    // a step behind it (the game emits it before it moves the camera), so
    // every vote cast then is off by a few pixels. A long pan (the left
    // stick across the map in view mode, state GH) piled them on a wrong
    // constant that kept winning after the pan: the scene sat 4-8 px off the
    // picture and the map's edge streaked into the sky. The constant learned
    // at rest holds through a pan; with none yet, the votes are taken anyway.
    // Still means the game's camera unchanged for 3 frames.
    // KOL_FOCUS_VOTE_WHILE_PANNING=1 votes on every frame, as before.
    static const bool no_rest_gate = kol::env_true("KOL_FOCUS_VOTE_WHILE_PANNING");
    bool vote_ok = scroll_valid;
    // A cut to another shot: the scroll jumps by more than a pan's step. The
    // game's camera reaches the new shot a few frames before the scroll
    // registers do, so the frames before the jump paired the new camera with
    // the old scroll, at rest, and voted for it. Map 3's church scene (Thor
    // state 0305): the gulls' sky shot at scroll (418, 256), the camera
    // already the church's (144, 232) for 4 frames, then the scroll (208,
    // 480) and a pan down: that vote won the whole pan (votes are held while
    // the camera moves), the scene lay 210 px and 33 rows off the picture,
    // the shadows on the wrong ground and the door's card off its wall. The
    // votes before a cut are dropped; the new shot's own sprites vote from
    // its first frame. KOL_FOCUS_VOTES_ACROSS_CUTS=1: as before.
    static const bool votes_across_cuts = kol::env_true("KOL_FOCUS_VOTES_ACROSS_CUTS");
    if (scroll_valid && !votes_across_cuts) {
        auto step = [](int a, int b, int m) { const int d = ((a - b) % m + m) % m; return std::min(d, m - d); };
        if (g_focus.cut_scroll_valid &&
            (step(hofs, g_focus.cut_hofs, 512) > 16 || step(vofs, g_focus.cut_vofs, 256) > 16)) {
            for (Vote& v : g_focus.votes) v = Vote{};
            g_focus.rest_frames = 0;
        }
        g_focus.cut_hofs = hofs;
        g_focus.cut_vofs = vofs;
        g_focus.cut_scroll_valid = true;
    }
    {
        int gkx = 0, gky = 0;
        game_camera(&gkx, &gky);
        if (gkx != g_focus.rest_kx || gky != g_focus.rest_ky) {
            g_focus.rest_kx = gkx;
            g_focus.rest_ky = gky;
            g_focus.rest_frames = 0;
        } else if (g_focus.rest_frames < 3) {
            ++g_focus.rest_frames;
        }
        bool any = false;
        for (const Vote& v : g_focus.votes) any = any || v.n != 0;
        if (g_focus.rest_frames < 3 && any && !no_rest_gate) vote_ok = false;
    }
    const int on_x0 = -(vw - 240) / 2 - 32, on_x1 = 240 + (vw - 240) / 2 + 32;
    const int on_y0 = -(vh - 160) / 2 - 32, on_y1 = 160 + (vh - 160) / 2 + 32;
    //
    // The cursor votes too, and outweighs any unit: its slot's world and
    // screen positions are always the same point. A unit's need not be. While
    // a move is being chosen the game draws a pale copy of the unit where it
    // would go, in the unit's own slot, which keeps the unit's world position
    // where it stands (states/BU_freelook_blurry.state: slot at (104, 80) on a
    // rock in the water, world 13 steps up at the back of the map). With that
    // copy the only unit on screen, it put the map 216 rows off: all of it
    // blurred, and the perspective warped the wrong shape.
    constexpr unsigned kCursorVote = 8u;
    //
    // And a unit votes only where a sprite is drawn: the frame's OAM has an
    // entry over its slot's screen place (the 16x32 above its feet). In
    // battle preparation, before anyone is deployed, the unit to be placed
    // keeps a slot at the screen's centre (120, 100) with its world place at
    // the map's back corner, and draws nothing there (DU_persp_broken, the
    // lava map, MEMBER 0/8). The only voter, it put the whole map in the
    // wrong place: the warp placed a stray triangle of it and left the rest
    // as painted background.
    const std::uint8_t* const vote_oam = cap.obj_ok ? cap.oam.data() : nullptr;
    auto drawn_at = [vote_oam](int sx, int sy) {
        // No record of the frame's sprites (the first frame after a load):
        // no vote. Recorded whenever the perspective or the depth of field,
        // the camera's users, is on (g_ppu_record_obj).
        if (!vote_oam) return false;
        for (int i = 0; i < 128; ++i) {
            const std::uint8_t* a = vote_oam + i * 8;
            const unsigned a0 = a[0] | (a[1] << 8), a1 = a[2] | (a[3] << 8);
            if ((a0 & 0x0300u) == 0x0200u || ((a0 >> 10) & 3u) == 2u) continue;   // disabled, window
            const unsigned shape = (a0 >> 14) & 3u, size = (a1 >> 14) & 3u;
            if (shape == 3u) continue;
            const int ew = kol::kObjW[shape][size], eh = kol::kObjH[shape][size];
            // The entry's reading nearest the unit's box (X wraps at 512, Y at 256).
            int ex = static_cast<int>(a1 & 0x1FFu), ey = static_cast<int>(a0 & 0xFFu);
            ex += 512 * static_cast<int>(std::lround((sx - ex) / 512.0));
            ey += 256 * static_cast<int>(std::lround((sy - 16 - ey) / 256.0));
            if (ex < sx + 8 && ex + ew > sx - 8 && ey < sy && ey + eh > sy - 32) return true;
        }
        return false;
    };
    bool camera = false;
    int cursor_kx = 0, cursor_ky = 0;
    for (int s = 0; s < kSlots; ++s) {
        const std::uint32_t o = kSlotArray - 0x03000000u + s * kSlotStride;
        if (!cursor && rd32(iw, o) == kCursorSlotFn) {
            // Only a cursor on screen. With a menu up (R, the command list)
            // its slot keeps its world place but goes to (-24, -300): its
            // weighted votes put the map 96 columns and 156 rows off, and
            // with the menu up long enough they outweighed the true ones, so
            // the map broke on closing it (DV_r_then_b).
            const int csx = rd16(iw, o + 0x1C), csy = rd16(iw, o + 0x1E);
            if (csx < on_x0 || csx >= on_x1 || csy < on_y0 || csy >= on_y1) continue;
            cursor = true;
            cursor_xz = rd16(iw, o + 0x08) + rd16(iw, o + 0x0C);
            cursor_x = rd16(iw, o + 0x08);
            cursor_z = rd16(iw, o + 0x0C);
            cursor_y = rd16(iw, o + 0x0A);

            // The game's own projection (0x080769D0), shifts and all: the
            // left stick puts the cursor between cells (kol_stick_cursor.cpp).
            cursor_sx = (rd16(iw, o + 0x08) - rd16(iw, o + 0x0C)) >> 1;
            cursor_sy = (cursor_xz >> 2) - (cursor_y >> 1);
            cursor_kx = rd16(iw, o + 0x1C) - cursor_sx;
            cursor_ky = rd16(iw, o + 0x1E) - cursor_sy;
            if (vote_ok)
                add_vote((cursor_kx + hofs) & 511, (cursor_ky + vofs) & 255, kCursorVote);
            continue;
        }
        const std::uint32_t owner = rd32(iw, o + 4);
        if (owner < list || owner >= list + kObjects * kObjStride ||
            (owner - list) % kObjStride != 0)
            continue;
        const int X = rd16(iw, o + 0x08), Y = rd16(iw, o + 0x0A), Z = rd16(iw, o + 0x0C);
        const int scr_x = rd16(iw, o + 0x1C), scr_y = rd16(iw, o + 0x1E);
        if (scr_x < on_x0 || scr_x >= on_x1 || scr_y < on_y0 || scr_y >= on_y1)
            continue;
        if (!drawn_at(scr_x, scr_y)) continue;
        // A slot left over from the scene before: its owner gone or another
        // object now, its place and screen spot the old scene's. Map 97's
        // boat flag (496, 128, 544, at (112, 60)) kept three slots into map
        // 98's town (states/HN_slot2_scene_change_shadows), something drawn
        // there, and they voted the boat's camera: the town lay 112 px and
        // 136 rows off its picture, its light and shadows over the wrong
        // ground (a fresh load of the town only tied, by luck). A slot votes
        // only for a live object standing within a step of its place (the
        // slot's Y is the object's less 8). KOL_FOCUS_STALE_SLOTS=1: as before.
        static const bool stale_slots = std::getenv("KOL_FOCUS_STALE_SLOTS") != nullptr;
        if (!stale_slots) {
            const std::uint32_t ob = e(owner);
            if (!in_ewram(rd32(ew, ob), 0x68)) continue;
            if (std::abs(rd16(ew, ob + 0x10) - X) > 16 || std::abs(rd16(ew, ob + 0x14) - Z) > 16 ||
                std::abs(rd16(ew, ob + 0x12) - 8 - Y) > 32)
                continue;
        }
        const int skx = scr_x - (X - Z) / 2;
        const int sky = scr_y - (X + Z) / 4 + Y / 2;
        if (!camera) { kx = skx; ky = sky; camera = true; }   // rough, for unwrapping
        if (vote_ok) add_vote((skx + hofs) & 511, (sky + vofs) & 255);
    }
    static const bool cam_check = kol::env_true("KOL_FOCUS_CAM_CHECK");
    if (cam_check) {
        int gkx = 0, gky = 0;
        game_camera(&gkx, &gky);
        std::fprintf(stderr, "focus_cam: voted %s (%d,%d) cursor %d (%d,%d) game (%d,%d) scroll %d,%d\n",
                     camera ? "unit" : "none", kx, ky, cursor ? 1 : 0, cursor_kx, cursor_ky,
                     gkx, gky, hofs, vofs);
    }
    // The cursor is the rough value to unwrap the votes near when there is
    // one. No unit and no cursor on screen this frame: the votes still hold,
    // and the last camera is.
    // With no unit and no cursor voting, the game's own camera votes, once
    // a frame like a unit (it pairs with the live scroll as the slots do).
    // Battle preparation before anyone is placed, with the cursor in the
    // roster, has no voter at all (states/GG_slot3_prep_no_shadows, MEMBER
    // 0/8): no camera, so no scene, and no perspective, shadows or occlusion
    // until a unit stood on the map. It agrees with the votes wherever they
    // exist (DF, B_rictor, S_move_highlight, BL_enemy_turn; KOL_FOCUS_CAM_CHECK=1
    // logs both). KOL_FOCUS_NO_GAME_CAMERA=1 leaves it out.
    static const bool no_game_camera = kol::env_true("KOL_FOCUS_NO_GAME_CAMERA");
    if (cursor) {
        kx = cursor_kx;
        ky = cursor_ky;
        camera = true;
    } else if (!camera && !no_game_camera) {
        int gkx = 0, gky = 0;
        game_camera(&gkx, &gky);
        // Only at rest, even with no votes yet: while a scene loads its camera
        // holds garbage for a few frames (map 97's boat into map 98's town,
        // states/HN_slot2_scene_change_shadows: (640, 624) for 3 frames after
        // the grid changed). Taken as the first votes, they won, and the
        // town's camera then panned the whole scene, so the units' votes were
        // never taken (votes only at rest once there are any): the scene lay
        // 112 px and 120 rows off the picture, its light and shadows over the
        // wrong ground. KOL_FOCUS_GAME_VOTE_ANY=1: as before.
        static const bool game_vote_any = std::getenv("KOL_FOCUS_GAME_VOTE_ANY") != nullptr;
        if (vote_ok && (g_focus.rest_frames >= 3 || no_rest_gate || game_vote_any))
            add_vote((gkx + hofs) & 511, (gky + vofs) & 255);
        // The votes are taken mod 512 and 256 (the camera plus the scroll),
        // the camera the copy nearest this. The last camera was that while a
        // scene loaded, (640, 624): the smoke scene after the burning town
        // (states/HQ, map 99 into the next) votes (176, -96), the same vote,
        // and took the copy nearest (640, 624), 512 px and 768 rows off: no
        // perspective or light where it belongs until a unit came on screen
        // and voted (~60 frames). Its own camera at rest is the nearer one.
        // KOL_FOCUS_LAST_NEAR=1: the last camera, as before.
        static const bool last_near = std::getenv("KOL_FOCUS_LAST_NEAR") != nullptr;
        // ... or moving, when it agrees with the votes (camera plus scroll,
        // within 2): a camera that is the scene's, not a loading one's
        // garbage. Map 57's death scene into map 37 (Thor state 0252): the
        // castle's camera pans from its first picture, never at rest, so the
        // copy came from map 57's last camera (296, -40), 256 rows off the
        // game's (152, 272): the mesh lay a BG height off the art until the
        // pan's scroll wrapped (~2 s). The vote itself was right (both 368,
        // 168). Not the loading placeholder near (640, 624), which moves with
        // the pan too and so agrees as well (HQ: (638, 623) to (618, 608) for
        // 16 frames before (144, 40)): a scene's camera is within 512 of 0.
        // Nor at a cut, the picture still the scene before's (its scroll more
        // than a pan's step from the tick's: HQ's 278,272 against 240,328).
        // KOL_FOCUS_GAME_AT_REST=1: only at rest, as before.
        static const bool at_rest_only = std::getenv("KOL_FOCUS_GAME_AT_REST") != nullptr;
        bool agrees = false;
        const bool pic_follows = std::abs(scroll_lead(hofs, g_pic_hofs)) <= 8 && std::abs(scroll_lead(vofs, g_pic_vofs)) <= 8;
        if (!at_rest_only && scroll_valid && pic_follows && std::abs(gkx) < 512 && std::abs(gky) < 512) {
            const Vote* best = &g_focus.votes[0];
            for (const Vote& v : g_focus.votes)
                if (v.n > best->n) best = &v;
            auto near = [](int a, int b, int m) { const int d = ((a - b) % m + m) % m; return d <= 2 || d >= m - 2; };
            agrees = best->n && near(best->cx, (gkx + hofs) & 511, 512) && near(best->cy, (gky + vofs) & 255, 256);
            // No votes at all (a cut dropped them, see above) and nobody on
            // screen to cast one: the camera moving in step with the scroll
            // (camera plus scroll unchanged since the last frame) is the
            // shot's own. Map 57 into map 37 (Thor state 0252): the castle
            // pans from its first picture with no unit in view; the old
            // scene's vote had agreed with it by chance. Within 2, as above:
            // the scroll trails the camera a frame, so the sum wobbles by 1.
            if (!best->n && !votes_across_cuts && g_focus.step_valid &&
                near(g_focus.step_cx, (gkx + hofs) & 511, 512) && near(g_focus.step_cy, (gky + vofs) & 255, 256))
                agrees = true;
        }
        g_focus.step_cx = (gkx + hofs) & 511;
        g_focus.step_cy = (gky + vofs) & 255;
        g_focus.step_valid = scroll_valid;
        const bool own = !g_focus.have_camera || (!last_near && g_focus.rest_frames >= 3) || agrees;
        kx = own ? gkx : g_focus.kx;
        ky = own ? gky : g_focus.ky;
        camera = true;
    } else if (!camera && g_focus.have_camera) {
        kx = g_focus.kx;
        ky = g_focus.ky;
        camera = true;
    }
    if (!camera) return false;
    if (scroll_valid) {
        const Vote* best = &g_focus.votes[0];
        for (const Vote& v : g_focus.votes)
            if (v.n > best->n) best = &v;
        if (best->n) {
            kx = unwrap(best->cx - g_pic_hofs, 512, kx);
            ky = unwrap(best->cy - g_pic_vofs, 256, ky);
        }
    }
    g_focus.have_camera = true;
    g_focus.kx = kx;
    g_focus.ky = ky;
    // Units on stage: one whose feet are in the view with no sprite drawn over
    // them is not (yet) shown. Map 52's opening scene (states/thor/0175) keeps
    // the soldiers who come in later in the map's objects, at their places, and
    // each carried a lantern there (the user, 2026-11-10, saw lanterns where
    // units arrive later in the cutscene). Off the view
    // a unit keeps its lamp (its pool reaches in as the camera scrolls); with
    // no record of the frame's sprites, every unit is shown.
    // KOL_LAMP_HIDDEN_UNITS=1: as before.
    static const bool hidden_lamps = kol::env_true("KOL_LAMP_HIDDEN_UNITS");
    if (vote_oam && !hidden_lamps) {
        const int vx0 = -(vw - 240) / 2, vx1 = 240 + (vw - 240) / 2;
        const int vy0 = -(vh - 160) / 2, vy1 = 160 + (vh - 160) / 2;
        for (int u = 0; u < count; ++u) {
            const int sx = feet_sx(units[u]) + kx, sy = feet_sy(units[u]) + ky;
            if (sx < vx0 || sx >= vx1 || sy < vy0 || sy >= vy1) continue;
            units[u].shown = drawn_at(sx, sy);
        }
    }
    if (cam_check) {
        int gkx = 0, gky = 0;
        game_camera(&gkx, &gky);
        std::fprintf(stderr, "focus_cam: chose (%d,%d) game (%d,%d) pic %d,%d\n", kx, ky, gkx, gky,
                     g_pic_hofs, g_pic_vofs);
    }
    g_iface.assign(static_cast<std::size_t>(w) * h, 0);
    g_obj_top_valid = false;
    return true;
}

// Props by the game's own record, where the map's byte does not say so.
// The same record is the same thing on every map; the bytes are the
// map's own numbers. The lists hold the records the byte rules already
// treat right somewhere (rocks and pots by the cut-out rule, trees and
// bushes by the trees' rule), so a map that gives one of them another
// byte gets the same rule: the torch on the ruins by the water (70, byte
// 0x07, the trees' byte there, which took the wall behind it for its
// ground), the rocks on DQ (50, byte 0x05, no prop byte at all). A
// listed record also chooses the rule over the byte.
// KOL_PERSP_NO_RECORD_PROPS=1 goes by the bytes alone.
// 0: not in the lists, 1: the trees' rule, 2: the cut-out rule, -1:
// never a prop (kTerrainRecords).
int SceneBuild::record_rule_of(std::uint8_t local) {
    static const bool record_props = [] {
        const char* v = std::getenv("KOL_PERSP_NO_RECORD_PROPS");
        const char* t = std::getenv("KOL_PERSP_NO_TORCH_PROPS");   // its older name
        return !(v && v[0] && v[0] != '0') && !(t && t[0] && t[0] != '0');
    }();
    if (!record_props) return 0;
    const int r = record_of(local);
    static const bool terrain_records = !kol::env_true("KOL_PERSP_NO_TERRAIN_RECORDS");
    if (terrain_records) {
        if (in_list(kTerrainRecords, r)) return -1;
        for (const MapTerrainRecord& c : kMapTerrainRecords)
            if (c.map == map && c.record == r) return -1;
    }
    if (in_list(kCutoutRecords, r)) return 2;
    if (in_list(kTreeRecords, r)) return 1;
    return 0;
}

// The ground a rock spire or a pot stands on: the highest of the two
// neighbours whose tops show in front of it (+X, +Z), else of all four.
// The highest of all four took the platform behind the spire in
// CY_spire_not_fixed and the fire pot in CZ_fire_pot (both taller than
// the rock) for their ground, so neither made a prop. -1: not a prop.
// One that does not rise above all four must stand at least 2 over the
// front: a stone block top of 0x0A on the ruins (BY_tile_sprites, 9 over
// a front of 8) is a block.
int SceneBuild::spire_ground(int cx, int cy) {
    static constexpr int kN[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    int front = -1, all = -1;
    for (int i = 0; i < 4; ++i) {
        bool nprop = false;
        const int nh = ground_cell(cx + kN[i][0], cy + kN[i][1], &nprop);
        if (nh < 0 || nprop) continue;
        if (i < 2) front = std::max(front, nh);
        all = std::max(all, nh);
    }
    const int top = raw_cell(cx, cy, nullptr);
    const int ground = front >= 0 ? front : all;
    if (ground < 0 || top <= ground) return -1;
    // Only with the bytes alone (KOL_PERSP_BYTE_PROPS): an obstacle's
    // record says it is a prop, and the block tops this guarded against
    // are not obstacles (BY: record 66). A low rock (DQ, 1 over its
    // front, lower than a neighbour) stands as a rock.
    if (byte_props && top <= all && top < ground + 2) return -1;
    return ground;
}

// A cell's height as the terrain stands (cell_height: memoised).
int SceneBuild::cell_height_of(int cx, int cy) {
    bool prop = false;
    const int h = raw_cell(cx, cy, &prop);
    if (h < 0) return h;
    if (const int d = drawn[gi(cx, cy)]; d >= 0) return d;
    if (!prop) return h;
    if (is_spire(cx, cy)) {
        const int ground = spire_ground(cx, cy);
        return ground < 0 ? h : std::min(h, ground);
    }
    int ground = -1;
    static constexpr int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& n : kN) {
        bool nprop = false;
        const int nh = ground_cell(cx + n[0], cy + n[1], &nprop);
        if (nh >= 0 && !nprop) ground = std::max(ground, nh);
    }
    return ground < 0 ? h : std::min(h, ground);
}

// The cells: their records, drawn heights and rounded boxes, the plants'
// and crystals' cells, each cell's height (cell_heights), the model.
void SceneBuild::cell_rules() {
    // The game's own terrain record of a cell. Byte +0 of a cell is the map's
    // own number; the map's table (u16 per number, ROM, pointer at
    // 0x020042E4, read by 0x08028774) gives the record of the terrain table
    // 0x0861CA5C. The same byte is a tree on the snowfield (41) and a torch
    // on the ruins (70). -1: unknown.
    rom = cap.rom;
    rom_size = cap.rom_size;
    terrain_map = rd32(ew, e(kTerrainMapPtr));
    // (Each byte's worked out once a frame, when first asked: raw_cell asks
    // per cell, and the props' spread per pixel. The same for record_rule
    // and is_obstacle below: each depends on the byte and the frame alone.)
    std::fill_n(record_memo, 256, INT_MIN);
    // The cells whose prop is a plant (trees, bushes: it sways in the wind).
    // (Past the game's grid only BG1 shapes set them: bg1_shape_props.)
    std::fill_n(g_plant_cell, kGridSide * kGridSide, false);
    std::fill_n(g_crystal_cell, kGridSide * kGridSide, false);
    for (int cy = 0; cy < kGameSide; ++cy)
        for (int cx = 0; cx < kGameSide; ++cx) {
            const int r = record_of(ew[e(kGrid) + game_cell(cx, cy)]);
            const bool veg = in_list(kPlantRecords, r), ice = in_list(kCrystalRecords, r);
            g_plant_cell[gi(cx, cy)] = veg;
            g_crystal_cell[gi(cx, cy)] = ice;
        }
    std::fill_n(rule_memo, 256, static_cast<std::int8_t>(INT8_MIN));
    bg1_props = bg1_prop_map(map);   // (kBg1PropMaps)
    std::fill_n(obstacle_memo, 256, static_cast<std::int8_t>(-1));

    // The drawn heights that differ from the stored ones on this map, by
    // cell (-1: as stored), and a key of them for the terrain pass's cache.
    static const bool drawn_heights = !kol::env_true("KOL_PERSP_NO_DRAWN_HEIGHTS");
    // (Over the whole field: 36 KB, off the stack of the present's worker.)
    static thread_local std::vector<int> drawn_store;
    drawn_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, -1);
    drawn = drawn_store.data();
    static thread_local std::vector<int> lower_store, split_store, split0_store, split_x_store;
    split_x_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    drawn_split_x = split_x_store.data();
    lower_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    split_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    split0_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    drawn_lower = lower_store.data(); drawn_split = split_store.data(); drawn_split0 = split0_store.data();
    if (drawn_heights) {
        auto cell_record = [&](int cx, int cy) {
            return in_game(cx, cy) ? record_of(ew[e(kGrid) + game_cell(cx, cy)]) : -1;
        };
        for (const DrawnCells& d : kDrawnCells) {
            if (d.map != map) continue;
            for (int cy = d.z0; cy <= d.z1; ++cy)
                for (int cx = d.x0; cx <= d.x1; ++cx)
                    if (in_grid(cx, cy) && (d.record < 0 || cell_record(cx, cy) == d.record)) {
                        drawn[gi(cx, cy)] = d.height;
                        drawn_lower[gi(cx, cy)] = d.lower;
                        drawn_split[gi(cx, cy)] = d.split_z;
                        drawn_split0[gi(cx, cy)] = d.split_z0;
                        drawn_split_x[gi(cx, cy)] = d.split_x ? 1 : 0;
                    }
        }
        // Heights set live with Debug > Tile Info (kol_tile_edit), over the
        // table's.
        {
            static thread_local std::vector<int> edits;
            edits.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
            if (kol::tile_edit_heights(map, edits.data()))
                for (int c = 0; c < kGridSide * kGridSide; ++c)
                    if (edits[c] >= 0) drawn[c] = edits[c];
        }
        // Water (or ice) tagged in the editor where the pass would place nothing
        // (off the field, or past the game's grid): flat, at 0, with
        // Options > Water on (off, the picture is as before). Map 89's
        // harbour lay on no cell, so it had nothing to be water on. Placed,
        // it takes the light like any ground (the quay's shadow).
        if (kol::setting_on(kol::Setting::Water)) {
            static thread_local std::vector<signed char> tags;
            tags.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
            if (kol::tile_edit_water(map, tags.data()))
                for (int c = 0; c < kGridSide * kGridSide; ++c) {
                    if ((tags[c] != kol::kWaterYes && tags[c] != kol::kWaterIce) || drawn[c] >= 0) continue;
                    const int cx = cell_x(c), cy = cell_y(c);
                    if (in_game(cx, cy)) {
                        const std::uint32_t gc = e(kGrid) + game_cell(cx, cy);
                        if ((ew[gc + 2] & 0x80u) || ew[gc + 1] != 0u) continue;   // on the field
                    }
                    drawn[c] = 0;
                }
        }
        // A map's floor past its field: every cell off it with no height of
        // its own (the editor's, the tables') at `height`. Map 63, the night
        // camp: its clearing is the field at 1, the woods round it off the
        // grid at 0, and the band of them at the top, a step down, stood
        // apart from the rest under the perspective (the user suggested
        // raising the rest of the drawn map to height 1 -- it
        // did; their editor heights there were 1 already).
        // KOL_NO_MAP_FLOORS=1: as before.
        static const bool no_floors = std::getenv("KOL_NO_MAP_FLOORS") != nullptr;
        for (const MapFloor& f : kMapFloors) {
            if (f.map != map || no_floors) continue;
            for (int c = 0; c < kGridSide * kGridSide; ++c) {
                if (drawn[c] >= 0) continue;
                const int cx = cell_x(c), cy = cell_y(c);
                if (in_game(cx, cy)) {
                    const std::uint32_t gc = e(kGrid) + game_cell(cx, cy);
                    if ((ew[gc + 2] & 0x80u) || ew[gc + 1] != 0u) continue;   // on the field
                }
                drawn[c] = f.height;
            }
        }
        for (int c = 0; c < kGridSide * kGridSide; ++c)
            if (drawn[c] >= 0)
                drawn_key = (drawn_key ^ static_cast<std::uint32_t>((c << 8) | drawn[c])) * 16777619u + 1u;
        // The field's cells that exist: the game's grid, and those the
        // editor or the table gave a height beyond it.
        for (int c = 0; c < kGridSide * kGridSide; ++c)
            if (drawn[c] >= 0 && !in_game(cell_x(c), cell_y(c))) {
                field_x0 = std::min(field_x0, cell_x(c)); field_x1 = std::max(field_x1, cell_x(c));
                field_z0 = std::min(field_z0, cell_y(c)); field_z1 = std::max(field_z1, cell_y(c));
            }
    }
    // This map's rounded boxes (see kDrawnRounds), by cell: -1 none. The
    // table's, then the editor's terrain types, a cell each.
    static thread_local std::vector<int> round_store;
    round_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, -1);
    rounds.clear();
    round_of = round_store.data();
    if (drawn_heights) {
        for (const DrawnRound& d : kDrawnRounds)
            if (d.map == map) rounds.push_back(d);
        static const int kCorner[kol::kTileTypes] = {0, kRoundLeft, kRoundFront, kRoundRight, kRoundBack};
        // Their cornice: the art's outline sits a column further out for
        // the wall's top 12 units under a rounded silhouette (map 88's 12,5,
        // JY: measured on the flat frame row by row, outline 231 against the
        // shape's 232 from height 352 down to 340). KOL_TYPE_CORNICE=<units>.
        static const int type_cornice = [] {
            const char* v = std::getenv("KOL_TYPE_CORNICE");
            return v ? std::max(0, std::atoi(v)) : 12;
        }();
        for (const kol::TileTyped& t : kol::tile_edit_types(map))
            rounds.push_back({map, t.x, t.z, t.x, t.z, 32, kCorner[t.type], type_cornice});
        for (int i = 0; i < static_cast<int>(rounds.size()); ++i) {
            const DrawnRound& d = rounds[i];
            for (int cy = d.z0; cy <= d.z1; ++cy)
                for (int cx = d.x0; cx <= d.x1; ++cx) if (in_grid(cx, cy)) round_of[gi(cx, cy)] = i;
            drawn_key = (drawn_key ^ static_cast<std::uint32_t>(0x10000 + (d.x0 + 64) * 4096 + (d.z0 + 64) * 16 +
                                                                d.corners)) * 16777619u + 1u;
        }
    }
    // Each cell's, worked out once a frame when first asked (all it reads is
    // the frame's, final by now): the terrain pass asks about 11 times a
    // cell, the window light at every step of its rays. Off the grid -1, as
    // raw_cell answers there.
    // The window light takes the same, as a CellHeights (cell_heights; with
    // the model's floors, cell_heights_occ).
    static thread_local std::vector<int> cell_h_store;
    cell_h_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, INT_MIN);
    cell_heights = CellHeights{cell_h_store.data(),
                               [](const void* c, int cx, int cy) {
                                   return static_cast<SceneBuild*>(const_cast<void*>(c))->cell_height_of(cx, cy);
                               },
                               this, false};
    cell_heights_occ = cell_heights;
    cell_heights_occ.occ = true;
    g_occ = model_occluder(map);
}

// Over the roofs with walls, or (open) the awnings.
bool SceneBuild::roof_only_y(float X, float Z, float* y, bool open) {
    // Only the roofs over the point's cell, in the roofs' order (the first
    // that holds the point answers, as when every roof was asked): the shadow
    // map asks at every step of every ray, and map 89 has 60 roofs: its first
    // picture took 151 ms on the Mac in the map, 335 on the Steam Deck
    // (2026-11-18, "loading stutter"). Off the grid, every roof.
    const int pcx = static_cast<int>(std::floor(X / 32.0f)), pcz = static_cast<int>(std::floor(Z / 32.0f));
    const bool indexed = roof_cell_at && in_grid(pcx, pcz);
    const std::size_t n_roofs = indexed ? roof_cell_at[gi(pcx, pcz) + 1] - roof_cell_at[gi(pcx, pcz)] : roofs.size();
    const std::uint16_t* const cell_roofs = indexed ? roof_cell_list + roof_cell_at[gi(pcx, pcz)] : nullptr;
    for (std::size_t li = 0; li < n_roofs; ++li) {
        const std::size_t ri = indexed ? cell_roofs[li] : li;
        const kol::TileRoof& r = roofs[ri];
        if (r.open != open) continue;
        const RoofExtent& ext = roof_ext[ri];
        const float X0 = ext.X0, X1 = ext.X1, Z0 = ext.Z0, Z1 = ext.Z1;
        if (X < X0 || X >= X1 || Z < Z0 || Z >= Z1) continue;
        if (r.shape == 11) {
            // A pyramid: the ridge height over the middle, the eave at
            // all four sides.
            const float u = std::abs(X - 0.5f * (X0 + X1)) / (0.5f * (X1 - X0));
            const float v = std::abs(Z - 0.5f * (Z0 + Z1)) / (0.5f * (Z1 - Z0));
            *y = 8.0f * r.ridge2 - 8.0f * (r.ridge2 - r.eave2) * std::max(u, v);
            return true;
        }
        if (r.shape >= 3) {
            // A peak over a corner (3-6): highest there, falling to the
            // eave along the two far sides (the larger of the two
            // fractions of the way across). A valley (7-10): lowest
            // there, rising to the ridge along them.
            const int ci = (r.shape - 3) % 4;
            const bool px = ci == 0 || ci == 1, pz = ci == 0 || ci == 3;
            const float u = (px ? X1 - X : X - X0) / (X1 - X0), v = (pz ? Z1 - Z : Z - Z0) / (Z1 - Z0);
            const float rise = 8.0f * (r.ridge2 - r.eave2) * std::max(u, v);
            *y = r.shape >= 7 ? 8.0f * r.eave2 + rise : 8.0f * r.ridge2 - rise;
            return true;
        }
        // Across the ridges: X when they run along Z.
        const float a = r.along_x ? Z : X;
        const float a0 = r.along_x ? Z0 : X0, a1 = r.along_x ? Z1 : X1;
        const float wg = (a1 - a0) / static_cast<float>(r.gables);
        const int i = std::clamp(static_cast<int>((a - a0) / wg), 0, r.gables - 1);
        const float aL = a0 + i * wg;
        const float T = 8.0f * r.ridge2, E = 8.0f * r.eave2;
        if (r.shape == 1) *y = T - (T - E) * (a - aL) / wg;              // falls toward +
        else if (r.shape == 2) *y = T - (T - E) * (aL + wg - a) / wg;    // falls toward -
        else *y = T - (T - E) * std::abs(a - (aL + 0.5f * wg)) / (0.5f * wg);
        return true;
    }
    return false;
}

// The roof's height there, or a box's standing over it (the higher).
bool SceneBuild::roof_y(float X, float Z, float* y) {
    bool any = roof_only_y(X, Z, y);
    // (Only the boxes over the point's cell, as roof_only_y its roofs; the
    // highest answers, whatever their order.)
    const int pcx = static_cast<int>(std::floor(X / 32.0f)), pcz = static_cast<int>(std::floor(Z / 32.0f));
    const bool indexed = box_cell_at && in_grid(pcx, pcz);
    const std::size_t n_boxes = indexed ? box_cell_at[gi(pcx, pcz) + 1] - box_cell_at[gi(pcx, pcz)] : boxes.size();
    const std::uint16_t* const cell_boxes = indexed ? box_cell_list + box_cell_at[gi(pcx, pcz)] : nullptr;
    for (std::size_t li = 0; li < n_boxes; ++li) {
        const Box& b = boxes[indexed ? cell_boxes[li] : li];
        if (X < b.X0 || X >= b.X1 || Z < b.Z0 || Z >= b.Z1) continue;
        const float v = box_h(b, X, Z);
        if (!any || v > *y) *y = v;
        any = true;
    }
    return any;
}

// Samples count only on cells as high as the point's own (one across a
// step stands inside the block beside it, whose shadow is not this
// point's: the pit's lit wall went dark); on a wall (tdx, tdz: along
// it) only along the wall, not into it.
int SceneBuild::cell_h(float X, float Z) {
    const int cx = static_cast<int>(std::floor(X / 32.0f)), cz = static_cast<int>(std::floor(Z / 32.0f));
    return in_grid(cx, cz) ? heights[gi(cx, cz)] : -1;
}

// (See roofs_and_shade: the share of a point in the terrain's shadow.)
float SceneBuild::shade_frac(float X, float Z, float y, float tdx, float tdz) {
    const bool c = shaded_at(X, Z, y);
    if (soft_t <= 0.0f || !sh_dist) return c ? 1.0f : 0.0f;
    const int ssx = static_cast<int>(std::floor(X * kShadowSub / 32.0f)) - sh_ox;
    const int ssz = static_cast<int>(std::floor(Z * kShadowSub / 32.0f)) - sh_oz;
    if (ssx < 0 || ssz < 0 || ssx >= sh_w || ssz >= sh_h) return c ? 1.0f : 0.0f;
    const bool wall0 = tdx != 0.0f || tdz != 0.0f;
    if (!wall0 && sh_edge && !sh_edge[static_cast<std::size_t>(ssz) * sh_w + ssx]) return c ? 1.0f : 0.0f;
    const float r = std::min(kTerrainSoftMax, soft_t * sh_dist[static_cast<std::size_t>(ssz) * sh_w + ssx]);
    if (r < 1.0f) return c ? 1.0f : 0.0f;
    const bool wall = tdx != 0.0f || tdz != 0.0f;
    const int h0 = wall ? 0 : cell_h(X, Z);
    int n = 0, m = 0;   // shaded, counted
    auto sample = [&](float sx, float sz) {
        if (!wall && cell_h(sx, sz) != h0) return;
        ++m;
        n += shaded_at(sx, sz, y) ? 1 : 0;
    };
    if (wall) {
        sample(X + r * tdx, Z + r * tdz);
        sample(X - r * tdx, Z - r * tdz);
    } else {
        sample(X + r, Z); sample(X - r, Z); sample(X, Z + r); sample(X, Z - r);
    }
    if (n == (c ? m : 0)) return c ? 1.0f : 0.0f;   // all agree
    n = 0; m = 0;
    if (wall)
        for (int i = -2; i <= 2; ++i) sample(X + 0.5f * r * i * tdx, Z + 0.5f * r * i * tdz);
    else
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) sample(X + 0.5f * r * i, Z + 0.5f * r * j);
    return m ? static_cast<float>(n) / m : (c ? 1.0f : 0.0f);
}

// The light's switches, the roofs and boxes, the cells' heights, the sun,
// the shadow map and how points read it.
void SceneBuild::roofs_and_shade() {
    glow_on = glow_strength() > 0.0f;
    // Each pixel's world place (g_wx, g_wz, g_face) is kept for the
    // torchlight and for the light through windows.
    godrays_on = godray_strength() > 0.0f && map_has_windows();
    world_on = glow_on || godrays_on;
    if (world_on) {
        g_wx.resize(static_cast<std::size_t>(w) * h);
        g_wz.resize(static_cast<std::size_t>(w) * h);
    }
    ao_s = ao_strength();
    sh_s = shadow_strength();
    shade_on = ao_s > 0.0f || sh_s > 0.0f;
    // Roofs set with Debug > Tile Info's editor (kol_tile_edit): over cells
    // x0..x1, z0..z1, `gables` prisms side by side, ridges along X or Z,
    // each falling from the ridge to the eave at its sides. roof_y gives the
    // roof's height (world units) over a spot inside one.
    roofs = kol::tile_edit_roofs(map);
    // A roof's extent in world units: its cells, shifted dx8, dz8 eighths of
    // a cell (a house the art draws off the grid), grown by `oh` on every
    // side (an awning's overhang); and the cells that extent touches.
    auto roof_extent = [](const kol::TileRoof& r, float oh) {
        RoofExtent x;
        x.X0 = 32.0f * r.x0 + 4.0f * r.dx8 - oh;
        x.X1 = 32.0f * (r.x1 + 1) + 4.0f * r.dx8 + oh;
        x.Z0 = 32.0f * r.z0 + 4.0f * r.dz8 - oh;
        x.Z1 = 32.0f * (r.z1 + 1) + 4.0f * r.dz8 + oh;
        x.cx0 = static_cast<int>(std::floor(x.X0 / 32.0f));
        x.cx1 = static_cast<int>(std::floor((x.X1 - 0.01f) / 32.0f));
        x.cz0 = static_cast<int>(std::floor(x.Z0 / 32.0f));
        x.cz1 = static_cast<int>(std::floor((x.Z1 - 0.01f) / 32.0f));
        return x;
    };
    // Each roof's, once a frame (roof_only_y asks per step of a shadow ray).
    roof_ext.reserve(roofs.size());
    for (const kol::TileRoof& r : roofs) roof_ext.push_back(roof_extent(r, r.open ? awning_overhang() : 0.0f));
    // Each cell's roofs (roof_only_y): every cell an extent reaches, its far
    // edge's cell included (a point just short of X1 may lie in it).
    // KOL_ROOF_NO_INDEX=1: every roof asked, as before.
    {
        static const bool no_index = kol::env_set("KOL_ROOF_NO_INDEX");
        static thread_local std::vector<std::uint32_t> at_store;
        static thread_local std::vector<std::uint16_t> list_store;
        roof_cell_at = nullptr;
        roof_cell_list = nullptr;
        if (!no_index && !roofs.empty() && roofs.size() < 65535) {
            constexpr int kCells = kGridSide * kGridSide;
            at_store.assign(kCells + 1, 0);
            auto each_cell = [&](const RoofExtent& x, auto&& fn) {
                const int z0 = static_cast<int>(std::floor(x.Z0 / 32.0f)), z1 = static_cast<int>(std::floor(x.Z1 / 32.0f));
                const int x0 = static_cast<int>(std::floor(x.X0 / 32.0f)), x1 = static_cast<int>(std::floor(x.X1 / 32.0f));
                for (int cz = z0; cz <= z1; ++cz)
                    for (int cx = x0; cx <= x1; ++cx)
                        if (in_grid(cx, cz)) fn(gi(cx, cz));
            };
            for (const RoofExtent& x : roof_ext) each_cell(x, [&](int c) { ++at_store[c + 1]; });
            for (int c = 0; c < kCells; ++c) at_store[c + 1] += at_store[c];
            list_store.assign(at_store[kCells], 0);
            std::vector<std::uint32_t> fill(at_store.begin(), at_store.end() - 1);
            for (std::size_t ri = 0; ri < roof_ext.size(); ++ri)
                each_cell(roof_ext[ri], [&](int c) { list_store[fill[c]++] = static_cast<std::uint16_t>(ri); });
            roof_cell_at = at_store.data();
            roof_cell_list = list_store.data();
        }
    }
    for (const kol::TileBox& b : kol::tile_edit_boxes(map)) {
        const float mx = 32.0f * b.x + 16.0f + 4.0f * b.dx8, mz = 32.0f * b.z + 16.0f + 4.0f * b.dz8;
        boxes.push_back({mx - 2.0f * b.size_x8, mx + 2.0f * b.size_x8, mz - 2.0f * b.size_z8, mz + 2.0f * b.size_z8,
                         8.0f * b.top2, 8.0f * std::max(b.top2, b.peak2), b.along_x, b.x, b.z});
    }
    {
        static const bool no_index = kol::env_set("KOL_ROOF_NO_INDEX");
        static thread_local std::vector<std::uint32_t> at_store;
        static thread_local std::vector<std::uint16_t> list_store;
        box_cell_at = nullptr;
        box_cell_list = nullptr;
        if (!no_index && !boxes.empty() && boxes.size() < 65535) {
            constexpr int kCells = kGridSide * kGridSide;
            at_store.assign(kCells + 1, 0);
            auto each_cell = [&](const Box& b, auto&& fn) {
                const int z0 = static_cast<int>(std::floor(b.Z0 / 32.0f)), z1 = static_cast<int>(std::floor(b.Z1 / 32.0f));
                const int x0 = static_cast<int>(std::floor(b.X0 / 32.0f)), x1 = static_cast<int>(std::floor(b.X1 / 32.0f));
                for (int cz = z0; cz <= z1; ++cz)
                    for (int cx = x0; cx <= x1; ++cx)
                        if (in_grid(cx, cz)) fn(gi(cx, cz));
            };
            for (const Box& b : boxes) each_cell(b, [&](int c) { ++at_store[c + 1]; });
            for (int c = 0; c < kCells; ++c) at_store[c + 1] += at_store[c];
            list_store.assign(at_store[kCells], 0);
            std::vector<std::uint32_t> fill(at_store.begin(), at_store.end() - 1);
            for (std::size_t bi = 0; bi < boxes.size(); ++bi)
                each_cell(boxes[bi], [&](int c) { list_store[fill[c]++] = static_cast<std::uint16_t>(bi); });
            box_cell_at = at_store.data();
            box_cell_list = list_store.data();
        }
    }
    for (const kol::TileRoof& r : roofs) any_open |= r.open;
    // The heights the cells are drawn at, once a frame, for the shadow map
    // and the torchlight (cell_height itself looks up records and props).
    static thread_local std::vector<int> heights_store;
    heights_store.assign(static_cast<std::size_t>(kGridSide) * kGridSide, -1);
    heights = heights_store.data();
    heights_key = kol::kFnvBasis;
    for (const kol::TileRoof& r : roofs)
        for (int v : {r.x0, r.z0, r.x1, r.z1, r.along_x ? 1 : 0, r.gables, r.eave2, r.ridge2, r.shape, r.open ? 1 : 0,
                      r.dx8 + 64, r.dz8 + 64})
            kol::fnv_mix(heights_key, static_cast<std::uint32_t>(v + 7));
    for (const Box& b : boxes)
        for (float v : {b.X0, b.X1, b.Z0, b.Z1, b.E, b.T, b.along_x ? 1.0f : 0.0f})
            kol::fnv_q4(heights_key, v);
    win_dark = dark_windows() && godray_strength() > 0.0f;
    heights_ready = sh_s > 0.0f || glow_on || win_dark;   // (field_x0..x1, field_z0..z1)
    if (heights_ready)
        for (int cy = field_z0; cy <= field_z1; ++cy)
            for (int cx = field_x0; cx <= field_x1; ++cx) {
                heights[gi(cx, cy)] = cell_height(cx, cy);
                kol::fnv_mix(heights_key, static_cast<std::uint32_t>(heights[gi(cx, cy)] + 1));
            }
    g_shadow_on = sh_s > 0.0f;
    // The sun's way along the ground and its rise, once a frame: each query
    // reads the map's sun (the editor's under its lock), the windows, the
    // torch and the turns, and the roof pass asked per pixel. Nothing they
    // read changes during the build (the capture's) but t_torch_at_rest,
    // which build_window_light sets only while it runs.
    shadow_dir(sun_x, sun_z);
    sun_rise = shadow_rise();
    if (win_dark) {
        // The beams' key indoors (dark_windows), where no shadow map is made:
        // the heights and the sun.
        g_win_sun_key = sun_key(heights_key, sun_rise, sun_x, sun_z);
    }
    // The heights the cast shadows bound their regions by: with the sun, and
    // with lamps (indoors, no sun: they were never refreshed there).
    // And over a model's floor (a table's top, map 65) as high as it: the
    // shadows' regions are bounded by these, and a unit's shadow in the
    // window light never reached the table's top (the user wanted the godrays
    // to cast sprite shadows, e.g. Alphonse in front of the window).
    if (sh_s > 0.0f || glow_on) {
        std::copy_n(heights, kGridSide * kGridSide, g_cast_heights);
        if (g_occ)
            for (int c = 0; c < kGridSide * kGridSide; ++c)
                if (g_occ->floor_h[static_cast<std::size_t>(c)] > g_cast_heights[c]) g_cast_heights[c] = g_occ->floor_h[static_cast<std::size_t>(c)];
        g_cast_heights_key = heights_key;
    }
    if (sh_s > 0.0f) {
        // The shadow map, from the heights the cells are drawn at.
        const float lx = sun_x, lz = sun_z;
        std::uint32_t key = sun_key(heights_key, sun_rise, lx, lz);
        // Its places: the field's cells that exist.
        const int sw = (field_x1 - field_x0 + 1) * kShadowSub, shh = (field_z1 - field_z0 + 1) * kShadowSub;
        const int sx0 = (field_x0 + kMargin) * kShadowSub, sz0 = (field_z0 + kMargin) * kShadowSub;
        kol::fnv_mix(key, static_cast<std::uint32_t>(sx0 * 65536 + sz0 + 1));
        kol::fnv_mix(key, static_cast<std::uint32_t>(sw * 65536 + shh + 1));
        if (key != g_shadow_key || g_shadow.size() != static_cast<std::size_t>(sw) * shh) {
            g_shadow_key = key;
            g_shadow_x0 = sx0; g_shadow_z0 = sz0; g_shadow_w = sw; g_shadow_h = shh;
            g_shadow.assign(static_cast<std::size_t>(sw) * shh, kNoShade);
            g_shadow_dist.assign(static_cast<std::size_t>(sw) * shh, 0);
            g_canopy_lo.clear();
            g_canopy_hi.clear();
            if (any_open) {
                g_canopy_lo.assign(static_cast<std::size_t>(sw) * shh, kNoShade);
                g_canopy_hi.assign(static_cast<std::size_t>(sw) * shh, kNoShade);
            }
            int top = 0;
            for (int i = 0; i < kGridSide * kGridSide; ++i) top = std::max(top, heights[i]);
            for (const kol::TileRoof& r : roofs) top = std::max(top, (r.ridge2 + 1) / 2);
            for (const Box& b : boxes) top = std::max(top, static_cast<int>(std::ceil(b.T / 16.0f)));
            const float rise = sun_rise;
            // The cells a roof covers: its height changes across them, so
            // every step there counts.
            static thread_local std::vector<std::uint8_t> under_roof;
            under_roof.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
            const int last = kGameSide + kMargin - 1;
            for (const RoofExtent& ext : roof_ext) {
                // Its cells, its overhang's and a shift's.
                for (int z = std::max(-kMargin, ext.cz0); z <= std::min(last, ext.cz1); ++z)
                    for (int x = std::max(-kMargin, ext.cx0); x <= std::min(last, ext.cx1); ++x)
                        under_roof[gi(x, z)] = 1;
            }
            for (const Box& b : boxes)
                for (int z = std::max(-kMargin, static_cast<int>(std::floor(b.Z0 / 32.0f)));
                     z <= std::min(last, static_cast<int>(std::floor((b.Z1 - 0.01f) / 32.0f))); ++z)
                    for (int x = std::max(-kMargin, static_cast<int>(std::floor(b.X0 / 32.0f)));
                         x <= std::min(last, static_cast<int>(std::floor((b.X1 - 0.01f) / 32.0f))); ++x)
                        under_roof[gi(x, z)] = 1;
            // A ray steps toward the sun 2 units at a time. Off a roof, the
            // first step into a cell stands highest of that cell's (the same
            // height, the shortest way), so only those count; and a ray
            // crosses the cells in the same pattern as any other leaving
            // the same place inside its own cell. So the pattern is worked
            // out once for each of the kShadowSub x kShadowSub places: the
            // cells crossed (from the ray's own) and the step entering each,
            // up to one off any grid. Every step was marched before (13-18
            // ms a frame on the Mac while the sun moved).
            struct Entry { int dx, dz; float t; };
            std::vector<Entry> paths[kShadowSub * kShadowSub];
            for (int iz = 0; iz < kShadowSub; ++iz)
                for (int ix = 0; ix < kShadowSub; ++ix) {
                    std::vector<Entry>& path = paths[iz * kShadowSub + ix];
                    const float ox = (ix + 0.5f) * (32.0f / kShadowSub);
                    const float oz = (iz + 0.5f) * (32.0f / kShadowSub);
                    for (float t = 2.0f;; t += 2.0f) {
                        const int dx = static_cast<int>(std::floor((ox + t * lx) / 32.0f));
                        const int dz = static_cast<int>(std::floor((oz + t * lz) / 32.0f));
                        if (path.empty() || path.back().dx != dx || path.back().dz != dz) path.push_back({dx, dz, t});
                        if (std::abs(dx) >= field_x1 - field_x0 + 1 || std::abs(dz) >= field_z1 - field_z0 + 1) break;
                    }
                }
            // Over the field's cells that exist (and the game's grid), a band
            // of rows on each of the row pool's threads (each place writes
            // its own; the sunset's step every 6th frame was a multi-ms frame
            // on the Thor). under_roof is this thread's: read through a pointer.
            const std::uint8_t* const roofed = under_roof.data();
            // The awnings' cells' bounds, and whether a ray at cell (qx, qz),
            // going the sun's way (lx, lz), has left them behind for good: it
            // moves one way along X and one along Z. Then no step further on
            // can be over an awning, and the ray may stop once nothing
            // higher is left, as on a map without one. (Map 89, one awning
            // among 60 roofs: every ray of the map crossed the whole field;
            // the map's first picture, 2026-11-18. KOL_SHADOW_OPEN_WHOLE=1:
            // as before.)
            static const bool open_whole = kol::env_set("KOL_SHADOW_OPEN_WHOLE");
            int ob_x0 = INT_MAX, ob_x1 = INT_MIN, ob_z0 = INT_MAX, ob_z1 = INT_MIN;
            for (std::size_t ri = 0; ri < roofs.size(); ++ri) {
                if (!roofs[ri].open) continue;
                const RoofExtent& x = roof_ext[ri];
                ob_x0 = std::min(ob_x0, static_cast<int>(std::floor(x.X0 / 32.0f)));
                ob_x1 = std::max(ob_x1, static_cast<int>(std::floor(x.X1 / 32.0f)));
                ob_z0 = std::min(ob_z0, static_cast<int>(std::floor(x.Z0 / 32.0f)));
                ob_z1 = std::max(ob_z1, static_cast<int>(std::floor(x.Z1 / 32.0f)));
            }
            auto open_past = [&](int qx, int qz) {
                if (open_whole) return false;
                const bool past_x = lx > 0.0f ? qx > ob_x1 : lx < 0.0f ? qx < ob_x0 : (qx < ob_x0 || qx > ob_x1);
                const bool past_z = lz > 0.0f ? qz > ob_z1 : lz < 0.0f ? qz < ob_z0 : (qz < ob_z0 || qz > ob_z1);
                return past_x || past_z;
            };
            wt.lap("shade.map");
            kol::parallel_rows(shh, 8, [&](int, int r0, int r1) {
            for (int sz = sz0 + r0; sz < sz0 + r1; ++sz)
                for (int sx = sx0; sx < sx0 + sw; ++sx) {
                    const float px = (sx + 0.5f) * (32.0f / kShadowSub) + kFieldOrigin;
                    const float pz = (sz + 0.5f) * (32.0f / kShadowSub) + kFieldOrigin;
                    const int cx = sx / kShadowSub - kMargin, cz = sz / kShadowSub - kMargin;
                    const std::vector<Entry>& path = paths[(sz % kShadowSub) * kShadowSub + sx % kShadowSub];
                    // The highest the terrain toward the sun stands above
                    // the ray that leaves this spot at ground level.
                    float shade = -1.0e9f, shade_t = 0.0f;   // and the step that set it
                    // An awning over the ray: the lowest and the highest
                    // start the ray would pass it at (see g_canopy_lo). With
                    // one on the map every ray runs its whole way: a step
                    // too low to raise `shade` can still be an awning's.
                    float c_lo = 1.0e9f, c_hi = -1.0e9f;
                    for (std::size_t e = 0; e < path.size(); ++e) {
                        const float t = path[e].t;
                        const int qx = cx + path[e].dx, qz = cz + path[e].dz;
                        // (With an awning on the map a ray went its whole way
                        // for the awning's sake. Past the awnings' cells, the
                        // way the ray travels, there is none left to meet:
                        // open_past.)
                        const bool no_awning_left = !any_open || open_past(qx, qz);
                        if (no_awning_left && 16.0f * top - t * rise <= shade) break;   // nothing higher left
                        // Past the cells that exist nothing casts a shadow.
                        if (qx < field_x0 || qz < field_z0 || qx > field_x1 || qz > field_z1) break;
                        const int hq = heights[gi(qx, qz)];
                        if (roofed[gi(qx, qz)]) {
                            // A roof's slope: every step in the cell.
                            for (float u = t; u < path[e + 1].t; u += 2.0f) {
                                if (no_awning_left && 16.0f * top - u * rise <= shade) break;
                                float ry = 0.0f;
                                if (any_open && roof_only_y(px + u * lx, pz + u * lz, &ry, true)) {
                                    c_lo = std::min(c_lo, ry - u * rise);
                                    c_hi = std::max(c_hi, ry - u * rise);
                                }
                                float v = -1.0e9f;
                                if (roof_y(px + u * lx, pz + u * lz, &ry)) v = ry - u * rise;
                                else if (hq >= 0) v = 16.0f * hq - u * rise;
                                if (v > shade) { shade = v; shade_t = u; }
                            }
                        } else if (hq >= 0 && 16.0f * hq - t * rise > shade) {
                            shade = 16.0f * hq - t * rise;
                            shade_t = t;
                        }
                    }
                    const std::size_t at = static_cast<std::size_t>(sz - sz0) * sw + (sx - sx0);
                    if (shade > -32000.0f) {
                        g_shadow[at] = static_cast<std::int16_t>(std::lround(shade));
                        g_shadow_dist[at] = static_cast<std::uint16_t>(std::min(65535.0f, shade_t));
                    }
                    if (c_hi > -32000.0f) {
                        g_canopy_lo[at] = static_cast<std::int16_t>(std::max(-32000.0f, std::floor(c_lo)));
                        g_canopy_hi[at] = static_cast<std::int16_t>(std::lround(c_hi));
                    }
                }
            });
            // The edge places: where a place's ground and its neighbour's (as
            // high) differ in shadow, grown by the softest blur (in places).
            wt.lap("shade.edge");
            g_shadow_edge.clear();
            if (terrain_soft() > 0.0f) {
                const std::size_t np = static_cast<std::size_t>(sw) * shh;
                std::vector<std::int16_t> ph(np, -1);   // the place's cell height
                std::vector<std::uint8_t> st(np, 0), e(np, 0);
                int max_d = 0;
                for (int z = 0; z < shh; ++z)
                    for (int x = 0; x < sw; ++x) {
                        const std::size_t at = static_cast<std::size_t>(z) * sw + x;
                        const int cx = (sx0 + x) / kShadowSub - kMargin, cz = (sz0 + z) / kShadowSub - kMargin;
                        const int hq = in_grid(cx, cz) ? heights[gi(cx, cz)] : -1;
                        ph[at] = static_cast<std::int16_t>(hq);
                        st[at] = hq >= 0 && 16.0f * hq + 0.5f < g_shadow[at];
                        max_d = std::max(max_d, static_cast<int>(g_shadow_dist[at]));
                    }
                for (int z = 0; z < shh; ++z)
                    for (int x = 0; x < sw; ++x) {
                        const std::size_t at = static_cast<std::size_t>(z) * sw + x;
                        if (ph[at] < 0) continue;
                        if ((x + 1 < sw && ph[at + 1] == ph[at] && st[at + 1] != st[at]) ||
                            (z + 1 < shh && ph[at + sw] == ph[at] && st[at + sw] != st[at])) {
                            e[at] = 1;
                            if (x + 1 < sw) e[at + 1] = 1;
                            if (z + 1 < shh) e[at + sw] = 1;
                        }
                    }
                const int R = static_cast<int>(std::ceil(
                                  std::min(kTerrainSoftMax, terrain_soft() * max_d) * kShadowSub / 32.0f)) + 1;
                std::vector<std::uint8_t> t(np, 0);
                for (int z = 0; z < shh; ++z) {   // rows, then columns: any edge within R
                    int run = -1000000;
                    for (int x = 0; x < sw; ++x) { if (e[static_cast<std::size_t>(z) * sw + x]) run = x; if (x - run <= R) t[static_cast<std::size_t>(z) * sw + x] = 1; }
                    run = 1000000;
                    for (int x = sw - 1; x >= 0; --x) { if (e[static_cast<std::size_t>(z) * sw + x]) run = x; if (run - x <= R) t[static_cast<std::size_t>(z) * sw + x] = 1; }
                }
                g_shadow_edge.assign(np, 0);
                for (int x = 0; x < sw; ++x) {
                    int run = -1000000;
                    for (int z = 0; z < shh; ++z) { if (t[static_cast<std::size_t>(z) * sw + x]) run = z; if (z - run <= R) g_shadow_edge[static_cast<std::size_t>(z) * sw + x] = 1; }
                    run = 1000000;
                    for (int z = shh - 1; z >= 0; --z) { if (t[static_cast<std::size_t>(z) * sw + x]) run = z; if (run - z <= R) g_shadow_edge[static_cast<std::size_t>(z) * sw + x] = 1; }
                }
            }
        }
    }
    // Whether a point `y` high over ground spot (X, Z) (world units) is in
    // the shadow of the terrain toward the sun.
    // (`awnings` false for a point on an awning itself: its own slopes say
    // which of them face away, and the map's places are too coarse for a
    // sheet's own surface: stripes.)
    // (The map's place and size read once: this runs per pixel. The field's
    // origin is a whole number of places, so it comes off after the floor.)
    wt.lap("setup.roofs+shade");
    sh_ox = g_shadow_x0 + kFieldOrigin * kShadowSub / 32;
    sh_oz = g_shadow_z0 + kFieldOrigin * kShadowSub / 32;
    sh_w = g_shadow_w;
    sh_h = g_shadow_h;
    sh_map = g_shadow.size() == static_cast<std::size_t>(sh_w) * sh_h ? g_shadow.data() : nullptr;
    // The share of a point in the terrain's shadow, its edge softened with
    // the distance to the terrain that casts it: the blur's radius is
    // soft_t x that distance (world units). Where the point and four around
    // it at that radius agree, it is that; else a 5 x 5 average over the
    // radius. KOL_TERRAIN_SOFT=<rate>; 0: the hard edge.
    soft_t = terrain_soft();
    sh_dist = g_shadow_dist.size() == g_shadow.size() && sh_map ? g_shadow_dist.data() : nullptr;
    sh_edge = g_shadow_edge.size() == g_shadow.size() ? g_shadow_edge.data() : nullptr;

    // KOL_PERSP_CELL_LOG=1: the cursor's cell, its map byte, terrain record
    // and height, logged when it changes (for "the thing under the cursor"
    // reports).
    static const bool cell_log = kol::env_true("KOL_PERSP_CELL_LOG");
    if (cell_log) {
        static int logged = -1;
        int ccx = 0, ccz = 0;
        if (kol::map_cursor_cell(&ccx, &ccz) && in_game(ccx, ccz) &&
            gi(ccx, ccz) != logged) {
            logged = gi(ccx, ccz);
            const std::uint32_t c = e(kGrid) + game_cell(ccx, ccz);
            std::fprintf(stderr, "kol-cell x=%d z=%d byte=0x%02x record=%d height=%d grid=%02x %02x %02x %02x\n",
                         ccx, ccz, ew[c], record_of(ew[c]), heights[logged],
                         ew[c], ew[c + 1], ew[c + 2], ew[c + 3]);
            const int rec = record_of(ew[c]);
            std::fprintf(stderr, "kol-cell record %d also at:", rec);
            for (int i = 0; i < kGameSide * kGameSide; ++i)
                if (i != ccz * kGameSide + ccx && record_of(ew[e(kGrid) + i * 4]) == rec)
                    std::fprintf(stderr, " %d,%d", i % kGameSide, i / kGameSide);
            std::fprintf(stderr, "\n");
        }
    }
}

// The terrain pass: every pixel's place, face, occlusion and shade from the
// cells (cached across still frames: PassCache).
void SceneBuild::terrain_pass() {
    // Everything the terrain pass reads, as a key (see PassCache): the
    // view and the camera, the grid's bytes, the map's record table and
    // drawn heights, the lighting and its maps, and whether the torchlight's world coordinates
    // are to be written this frame.
    ao_reach_v = ao_reach();
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    static const bool pass_cache = kol::env_not_zero("KOL_PASS_CACHE");
    static const bool pass_check = kol::env_true("KOL_PASS_CACHE_CHECK");
    pass_key = kol::kFnvBasis;
    auto mix = [this](std::uint32_t v) { kol::fnv_mix(pass_key, v); };
    auto mixf = [&mix](float f) {
        std::uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        mix(u);
    };
    kol::fnv_ints(pass_key, {w, h, ox, oy, kx, ky});
    for (std::size_t i = 0; i < static_cast<std::size_t>(kGameSide) * kGameSide * 4u; ++i)
        mix(ew[e(kGrid) + i]);
    mix(terrain_map);
    mix(drawn_key);   // the map's drawn heights: the grid alone does not name the map
    // Lifting BG1 (see lift_props_on) keeps what each pixel showed before.
    const bool record_back = lift_walls_on() && cap.perspective && cap.under_ok;
    mix(record_back ? 0x9E3779B9u : 0u);
    // The places alone (no light, no sun), for the BG1 shapes' cache.
    place_key = pass_key;
    mixf(ao_s); mixf(sh_s); mixf(ao_reach_v); mixf(sun_x); mixf(sun_z);
    mix(sh_s > 0.0f ? g_shadow_key : 0u);
    mix(1u | (world_on ? 2u : 0u) | (world_dirty ? 4u : 0u) | (shade_on ? 8u : 0u));
    // The same, but for where the camera is (kx, ky): two pictures with this
    // key in common show the same world, one moved over the other by whole
    // pixels (the pass's shift, below).
    std::uint32_t still_key = kol::kFnvBasis;
    {
        auto smix = [&still_key](std::uint32_t v) { kol::fnv_mix(still_key, v); };
        auto smixf = [&smix](float f) {
            std::uint32_t u;
            std::memcpy(&u, &f, sizeof u);
            smix(u);
        };
        kol::fnv_ints(still_key, {w, h, ox, oy});
        for (std::size_t i = 0; i < static_cast<std::size_t>(kGameSide) * kGameSide * 4u; ++i)
            smix(ew[e(kGrid) + i]);
        smix(terrain_map);
        smix(drawn_key);
        smix(record_back ? 0x9E3779B9u : 0u);
        smixf(ao_s); smixf(sh_s); smixf(ao_reach_v); smixf(sun_x); smixf(sun_z);
        smix(sh_s > 0.0f ? g_shadow_key : 0u);
        // (Not world_dirty: whether this pass writes the world places is the
        // shift's own business, below.)
        smix(1u | (world_on ? 2u : 0u) | (shade_on ? 8u : 0u));
    }
    wt.lap("scene.terrain");
    const bool pass_hit = pass_cache && g_pass.valid && g_pass.key == pass_key;
    ++g_pass.frames;
    if (pass_hit) {
        ++g_pass.hits;
        g_pass.buf.put(g_pass.parts);
    }
    // In check mode the pass runs over copies and is compared with what the
    // cache restored (above); the restored arrays are what the frame uses.
    PassBuffers chk;
    const bool checking = pass_hit && pass_check;
    if (checking) {
        chk.swap_globals();
        if (world_on) { g_wx.assign(chk.wx.begin(), chk.wx.end()); g_wz.assign(chk.wz.begin(), chk.wz.end()); }
    }
    // The pass. `clear`: every buffer emptied first, and all of the picture
    // drawn. Else (the shift, below) the buffers hold the last pass's pixels
    // moved to their new place in the rectangle [rx0, rx1) x [ry0, ry1), and
    // only the pixels outside it are drawn: each pixel is the last cell
    // drawn over it, whatever is drawn beside it.
    auto run_pass = [&](bool clear, int rx0, int rx1, int ry0, int ry1) {
    // A column with a pixel to draw, a pixel to draw.
    auto draw_col = [&](int px) {
        if (px < 0 || px >= w) return false;
        return clear || px < rx0 || px >= rx1 || ry0 > 0 || ry1 < h;
    };
    auto draw_at = [&](int px, int py) {
        if (py < 0 || py >= h) return false;
        return clear || px < rx0 || px >= rx1 || py < ry0 || py >= ry1;
    };
    if (clear) {
    // Each pixel's cell, face and occlusion start empty (on a hit the cache
    // restored them: a hit always holds the owners and faces, and the
    // occlusion exactly when shade_on, a bit of the key).
    g_cell_owner.assign(npix, -1);
    g_span_px.assign(npix, 0);
    g_round_n.assign(npix * 2, 0);
    g_face.assign(npix, 0);
    if (shade_on) g_ao.assign(npix, 255);
    g_xz.resize(npix);
    g_y.assign(npix, 0);
    g_covered.assign(npix, 0);
    g_kind.assign(npix, kol::kSceneNone);
    if (record_back) {
        g_back_xz.assign(npix, 0);
        g_back_y.assign(npix, 0);
        g_back_kind.assign(npix, kol::kSceneNone);
        if (shade_on) g_back_ao.assign(npix, 255);
    }
    // Nothing drawn: the ground plane, Y = 0, so sky above the map reads as
    // far behind and sky below it as in front. On any surface, a pixel's row
    // follows from its screen row and height: X + Z = 4 (sy - ky) + 2 Y.
    for (int py = 0; py < h; ++py) {
        const std::int32_t xz = 4 * (py - oy - ky);
        std::fill_n(g_xz.begin() + static_cast<std::size_t>(py) * w, w, xz);
    }
    }   // (clear)
    // The heights the pass draws with: a round tower's cells are the ground
    // at its foot (its lowest neighbour), its top the cells' stored height.
    static thread_local std::vector<int> round_top, round_base;
    static thread_local std::vector<std::uint8_t> round_edge;
    round_top.assign(rounds.size(), 0);
    round_base.assign(rounds.size(), 0);
    round_edge.assign(rounds.size(), 0);
    for (int i = 0; i < static_cast<int>(rounds.size()); ++i) {
        const DrawnRound& d = rounds[i];
        int top = -1, base = INT_MAX;
        bool edge = false;   // no cell beside a side the camera sees
        for (int cy = d.z0 - 1; cy <= d.z1 + 1; ++cy)
            for (int cx = d.x0 - 1; cx <= d.x1 + 1; ++cx) {
                const bool inside = cx >= d.x0 && cx <= d.x1 && cy >= d.z0 && cy <= d.z1;
                const int hh = inside ? raw_cell(cx, cy, nullptr) : cell_height(cx, cy);
                const bool seen_side = (cx == d.x1 + 1 && cy >= d.z0 && cy <= d.z1) ||
                                       (cy == d.z1 + 1 && cx >= d.x0 && cx <= d.x1);
                if (inside) top = std::max(top, hh);
                else if (hh >= 0) base = std::min(base, hh);
                else if (seen_side) edge = true;
            }
        // At the map's edge (no cell beside a side the camera sees) the
        // terrain pass runs the wall down to 0; so does the box. With the
        // foot at the lowest neighbour instead, a box there came out empty
        // (17, 2 on map 54's right corner, states/FS), or rounded only down
        // to a lower cell behind it and square below (13, 16, states/FT).
        if (edge) base = 0;
        round_edge[i] = edge ? 1 : 0;
        round_top[i] = top;
        round_base[i] = base == INT_MAX ? std::max(0, top) : std::min(base, std::max(0, top));
    }
    auto pass_height = [&](int cx, int cy) -> int {
        if (in_grid(cx, cy)) {
            const int i = round_of[gi(cx, cy)];
            if (i >= 0) return round_base[i];
        }
        return cell_height(cx, cy);
    };
    // The part of rounded box i over cell (ocx, ocy): its top and its front.
    // Each column of the front takes the shape's front-most point there (X - Z
    // from the column, X + Z the largest over the shape): on its +X or +Z
    // side, or on a rounded corner's arc.
    // A pixel the terrain pass draws (a cell's, a rounded box's): what it
    // showed kept for Lifted Walls (record_back), then its place (row r,
    // height y), kind, cell and face.
    auto put_px = [&](std::size_t k, int r, int y, bool flat, int face, int cell) {
        if (record_back && g_covered[k]) {
            g_back_xz[k] = g_xz[k]; g_back_y[k] = g_y[k]; g_back_kind[k] = g_kind[k];
            if (shade_on) g_back_ao[k] = g_ao[k];
        }
        g_xz[k] = 4 * (r - ky) + 2 * y;
        g_y[k] = y;
        g_covered[k] = 1;
        g_kind[k] = flat ? kol::kSceneFlat : kol::kSceneUpright;
        g_cell_owner[k] = static_cast<std::int16_t>(cell);
        g_face[k] = static_cast<std::uint8_t>(face);
        g_round_n[k * 2] = g_round_n[k * 2 + 1] = 0;
    };
    // A wall's occlusion, darker toward its foot (`above` it, in units).
    auto wall_occ = [&](float above) { return above < ao_reach_v ? 0.6f * (1.0f - above / ao_reach_v) : 0.0f; };
    // A pixel's occlusion byte, from its occlusion and its light.
    // (A kModelAoFloors map's occlusion is the model's alone: its grid's raised cells
    // are not what is drawn, and would add theirs on top.)
    bool grid_ao = true;
    for (const ModelAoFloor& f : kModelAoFloors)
        if (f.map == map) grid_ao = false;
    auto ao_byte = [&](float occ, float lit) {
        return static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - (grid_ao ? ao_s * occ : 0.0f)) * lit));
    };
    auto draw_round = [&](int i, int ocx, int ocy) {
        const DrawnRound& d = rounds[i];
        const int top = round_top[i], base = round_base[i];
        if (top <= base) return;
        const float R = static_cast<float>(d.r);
        const float X0 = 32.0f * d.x0, X1 = 32.0f * (d.x1 + 1), Z0 = 32.0f * d.z0, Z1 = 32.0f * (d.z1 + 1);
        struct Arc { float cx, cz, sx, sz; };
        Arc arcs[4];
        int narcs = 0;
        if (d.corners & kRoundLeft) arcs[narcs++] = {X0 + R, Z1 - R, -1.0f, 1.0f};
        if (d.corners & kRoundFront) arcs[narcs++] = {X1 - R, Z1 - R, 1.0f, 1.0f};
        if (d.corners & kRoundRight) arcs[narcs++] = {X1 - R, Z0 + R, 1.0f, -1.0f};
        // The back corner: only the top's outline (no column's front-most
        // point lies on it).
        const int front_arcs = narcs;
        if (d.corners & kRoundBack) arcs[narcs++] = {X0 + R, Z0 + R, -1.0f, -1.0f};
        auto inside = [&](float X, float Z) {
            if (X < X0 || X > X1 || Z < Z0 || Z > Z1) return false;
            for (int a = 0; a < narcs; ++a) {
                const Arc& c = arcs[a];
                if ((X - c.cx) * c.sx > 0.0f && (Z - c.cz) * c.sz > 0.0f &&
                    (X - c.cx) * (X - c.cx) + (Z - c.cz) * (Z - c.cz) > R * R)
                    return false;
            }
            return true;
        };
        // Past an arc on the top, the art's outline: its pixel beside the
        // shape (a column: X and Z one apart, a row: two) goes with the top,
        // as pass 1 below takes the front's. Left to the floor behind, it
        // came apart from the box as a thin dark curve when the camera moved
        // (JY's map 88, 12,5's left corner, the user reported rounded walls on
        // most maps missing a thin 1 px line). Only in the
        // box's square: its straight edges as before.
        // KOL_ROUND_NO_TOP_RING=1: as before.
        static const bool no_top_ring = std::getenv("KOL_ROUND_NO_TOP_RING") != nullptr;
        auto top_ring = [&](float X, float Z) {
            if (no_top_ring || X < X0 || X > X1 || Z < Z0 || Z > Z1) return false;
            return inside(X + 1.0f, Z - 1.0f) || inside(X - 1.0f, Z + 1.0f) || inside(X + 2.0f, Z + 2.0f) ||
                   inside(X - 2.0f, Z - 2.0f);
        };
        auto over_cell = [&](float X, float Z) {
            const int qx = std::min(d.x1, std::max(d.x0, static_cast<int>(std::floor(X / 32.0f))));
            const int qz = std::min(d.z1, std::max(d.z0, static_cast<int>(std::floor(Z / 32.0f))));
            return qx == ocx && qz == ocy;
        };
        const bool round_left = d.corners & kRoundLeft, round_front = d.corners & kRoundFront,
                   round_right = d.corners & kRoundRight;
        const int top_y = 16 * top, base_y = 16 * base;
        const float span = R * 1.41421356f;   // |(X - Z) - (cx - cz)| on an arc
        const int hx0 = static_cast<int>(std::floor((X0 - Z1) / 2.0f)) + kx - 3;
        const int hx1 = static_cast<int>(std::ceil((X1 - Z0) / 2.0f)) + kx + 3;
        auto put = [&](std::size_t k, float X, float Z, int y, bool flat, float nx, float nz, int r) {
            put_px(k, r, y, flat, flat ? 0 : nx >= nz ? 1 : 2, gi(ocx, ocy));
            if (!flat) {
                g_round_n[k * 2] = static_cast<std::int8_t>(std::lround(127.0f * std::clamp(nx, -1.0f, 1.0f)));
                g_round_n[k * 2 + 1] = static_cast<std::int8_t>(std::lround(127.0f * std::clamp(nz, -1.0f, 1.0f)));
            }
            if (world_dirty) {
                g_wx[k] = static_cast<std::int16_t>(std::lround(X));
                g_wz[k] = static_cast<std::int16_t>(std::lround(Z));
            }
            if (shade_on) {
                float occ = 0.0f, lit = 1.0f;
                if (flat) {
                    if (sh_s > 0.0f) lit = 1.0f - sh_s * shade_frac(X, Z, static_cast<float>(y));
                } else {
                    // Darker toward its foot, as a wall; turned away from the
                    // sun in its own shadow, else what the terrain casts on
                    // the spot in front of it: out of the box's square, as
                    // a square wall's spot is. On an arc, half a unit out
                    // was still inside the box's cells, which the shadow
                    // map holds at its top: the box shadowed itself, a
                    // sunlit rounded wall dark all over (JZ, the user's slot
                    // 3, map 86: a rounded right wall was shadowed while
                    // also lit).
                    // KOL_ROUND_SHADOW_NEAR=1: half a unit out, as before.
                    static const bool shadow_near = std::getenv("KOL_ROUND_SHADOW_NEAR") != nullptr;
                    occ = wall_occ(static_cast<float>(y - base_y));
                    float out = 0.5f;
                    if (!shadow_near) {
                        float t = 1.0e9f;
                        if (nx > 0.01f) t = std::min(t, (X1 - X) / nx);
                        if (nx < -0.01f) t = std::min(t, (X - X0) / -nx);
                        if (nz > 0.01f) t = std::min(t, (Z1 - Z) / nz);
                        if (nz < -0.01f) t = std::min(t, (Z - Z0) / -nz);
                        if (t < 1.0e8f) out += std::max(0.0f, t);
                    }
                    // The curve's own shadow comes in over the last 20
                    // degrees of turn before it faces away from the sun, not
                    // on one column (the user expected a more gradual shadow
                    // on a rounded left wall;
                    // picked 20 from 0, 20 and 40 on JZ).
                    // KOL_ROUND_TERMINATOR=<degrees>, 0: one column, as before.
                    static const float term = [] {
                        const char* v = std::getenv("KOL_ROUND_TERMINATOR");
                        const float deg = v ? static_cast<float>(std::atof(v)) : 20.0f;
                        return std::sin(std::clamp(deg, 0.0f, 89.0f) * 3.14159265f / 180.0f);
                    }();
                    const float facing = nx * sun_x + nz * sun_z;
                    if (sh_s > 0.0f) {
                        float self = facing <= 0.0f ? 1.0f : 0.0f;
                        if (term > 0.0f && facing > 0.0f && facing < term) {
                            const float f = facing / term;
                            self = 1.0f - f * f * (3.0f - 2.0f * f);
                        }
                        const float cast = facing <= 0.0f ? 0.0f
                                                          : shade_frac(X + nx * out, Z + nz * out, static_cast<float>(y),
                                                                       -nz, nx);
                        lit = 1.0f - sh_s * std::max(self, cast);
                    }
                }
                g_ao[k] = ao_byte(occ, lit);
            }
        };
        for (int hx = hx0; hx <= hx1; ++hx) {
            const int px = hx + ox;
            if (!draw_col(px)) continue;
            const float u = 2.0f * (hx - kx) + 1.0f;   // X - Z at the column's middle
            // The front-most point: on the +X side, the +Z side, or an arc.
            float best = -1.0e9f, bx = 0.0f, bz = 0.0f, bnx = 0.0f, bnz = 0.0f;
            {
                const float Z = X1 - u;   // the +X side
                if (Z >= (round_right ? Z0 + R : Z0) && Z <= (round_front ? Z1 - R : Z1) && 2.0f * X1 - u > best) {
                    best = 2.0f * X1 - u; bx = X1; bz = Z; bnx = 1.0f; bnz = 0.0f;
                }
                const float X = u + Z1;   // the +Z side
                if (X >= (round_left ? X0 + R : X0) && X <= (round_front ? X1 - R : X1) && u + 2.0f * Z1 > best) {
                    best = u + 2.0f * Z1; bx = X; bz = Z1; bnx = 0.0f; bnz = 1.0f;
                }
            }
            bool on_arc = false, outer = false;
            for (int pass = 0; pass < 2 && best < -1.0e8f + 1.0f; ++pass)
                for (int a = 0; a < front_arcs; ++a) {
                    // Pass 1, past an arc's silhouette: the art outlines the
                    // shape there, and the column of outline goes with it at
                    // the arc's edge depth (a second one along the cornice).
                    // Left to the wall behind, the outline column was the one
                    // stretched across the opening a camera move makes
                    // between the two, a dark band (stick down-left on EO).
                    // Two columns all the way down took the wall's next one,
                    // a light line beside the tower (ES, on the Thor).
                    const Arc& c = arcs[a];
                    const float du = u - (c.cx - c.cz);
                    const float reach = pass == 0 ? span : d.cornice > 0 ? span + 4.0f : span + 2.0f;
                    if (std::abs(du) >= reach) continue;
                    const float dv = std::sqrt(std::max(0.0f, 2.0f * R * R - du * du));
                    const float X = c.cx + 0.5f * (du + dv), Z = c.cz + 0.5f * (dv - du);
                    if ((X - c.cx) * c.sx < -0.01f || (Z - c.cz) * c.sz < -0.01f) continue;
                    if (c.cx + c.cz + dv > best) {
                        best = c.cx + c.cz + dv; bx = X; bz = Z;
                        const float nl = std::max(1.0f, std::hypot(X - c.cx, Z - c.cz));
                        bnx = (X - c.cx) / nl; bnz = (Z - c.cz) / nl;
                        on_arc = true;
                        outer = pass == 1 && std::abs(du) >= span + 2.0f;
                    }
                }
            for (int a = 0; a < front_arcs && !on_arc; ++a) {   // an arc in front of a side
                const Arc& c = arcs[a];
                const float du = u - (c.cx - c.cz);
                if (std::abs(du) >= span) continue;
                const float dv = std::sqrt(2.0f * R * R - du * du);
                const float X = c.cx + 0.5f * (du + dv), Z = c.cz + 0.5f * (dv - du);
                if ((X - c.cx) * c.sx < 0.0f || (Z - c.cz) * c.sz < 0.0f) continue;
                if (c.cx + c.cz + dv > best) {
                    best = c.cx + c.cz + dv; bx = X; bz = Z;
                    const float nl = std::max(1.0f, std::hypot(X - c.cx, Z - c.cz));
                    bnx = (X - c.cx) / nl; bnz = (Z - c.cz) / nl;
                }
            }
            if (best < -1.0e8f) continue;
            // The top: rows whose X + Z at the top's height falls inside.
            const int rt0 = static_cast<int>(std::floor((u + 2.0f * std::max(X0 - u, Z0) - 2.0f * top_y) / 4.0f)) + ky - 1;
            const int rt1 = static_cast<int>(std::ceil((best - 2.0f * top_y) / 4.0f)) + ky;
            for (int r = rt0; r <= rt1; ++r) {
                const int py = r + oy;
                if (!draw_at(px, py)) continue;
                const float v = 4.0f * (r - ky) + 2.0f * top_y + 2.0f;   // the row's middle
                const float X = 0.5f * (v + u), Z = 0.5f * (v - u);
                if (!(inside(X, Z) || top_ring(X, Z)) || !over_cell(X, Z)) continue;
                put(static_cast<std::size_t>(py) * w + px, X, Z, top_y, true, 0.0f, 0.0f, r);
            }
            // The front, from the top down to the foot.
            if (!over_cell(bx, bz)) continue;
            const int rs0 = static_cast<int>(std::ceil((best - 2.0f * top_y) / 4.0f)) + ky;
            const int rs1 = static_cast<int>(std::floor((best - 2.0f * base_y) / 4.0f)) + ky;
            for (int r = rs0; r < rs1; ++r) {
                const int py = r + oy;
                if (!draw_at(px, py)) continue;
                const int y = static_cast<int>(std::lround((best - 4.0f * (r - ky)) / 2.0f));
                if (outer && y < top_y - d.cornice) continue;   // only the cornice's rows
                put(static_cast<std::size_t>(py) * w + px, bx, bz, y, false, bnx, bnz, r);
            }
        }
    };
    // Back to front along the diagonals: a cell further down the diagonal
    // covers the ones behind it.
    for (int diag = field_x0 + field_z0; diag <= field_x1 + field_z1; ++diag) {
        for (int cx = std::max(field_x0, diag - field_z1);
             cx <= std::min(diag - field_z0, field_x1); ++cx) {
            const int cy = diag - cx;
            const int ch = pass_height(cx, cy);
            if (ch < 0) continue;
            // A rounded box at the map's edge stands on nothing: no ground
            // at its foot, which showed below its curved front (FT).
            if (in_grid(cx, cy))
                if (const int ri = round_of[gi(cx, cy)]; ri >= 0 && round_edge[ri]) {
                    draw_round(ri, cx, cy);
                    continue;
                }
            // A slit (DrawnCells::split_z, map 37's parapets): the cell in parts, back to
            // front: the stored top before the span, the span at the drawn height less
            // `lower`, the stored top after it; each part's pixels only (a top's by its Z
            // offset, the right wall's by its own, the front wall the last part's).
            auto lower_of = [&](int qx, int qz) { return drawn_lower && in_grid(qx, qz) ? drawn_lower[gi(qx, qz)] : 0; };
            const int cell_ch = ch;
            const int split = drawn_split && in_grid(cx, cy) ? drawn_split[gi(cx, cy)] : 0;
            const int split0 = split > 0 ? std::clamp(drawn_split0[gi(cx, cy)], 0, split) : 0;
            const int stored_h = split > 0 ? std::max(0, raw_cell(cx, cy, nullptr)) : cell_ch;
            const bool split_x = split > 0 && drawn_split_x[gi(cx, cy)] != 0;
            struct CellPart { int pz0, pz1; bool drawn; };
            static const float kSlitShade = kol::env_num_clamp("KOL_SLIT_SHADE", 0.0f, 1.0f, 0.5f);   // the shading inside a slit, of the map's
            CellPart parts_v[3];
            int nparts = 0;
            if (split <= 0 || (split0 <= 0 && split >= 32)) parts_v[nparts++] = {0, 32, true};
            else {
                if (split0 > 0) parts_v[nparts++] = {0, split0, false};
                parts_v[nparts++] = {split0, split, true};
                if (split < 32) parts_v[nparts++] = {split, 32, false};
            }
            for (int part = 0; part < nparts; ++part) {
            const CellPart& cpart = parts_v[part];
            const int ch = cpart.drawn ? cell_ch : stored_h;
            const int pz0 = cpart.pz0, pz1 = cpart.pz1;
            const int lw = cpart.drawn ? lower_of(cx, cy) : 0;
            // The top face's back corner, in hardware coordinates.
            const int sx = 16 * (cx - cy) + kx;
            const int sy = 8 * (cx + cy) - 8 * ch + ky + lw / 2;
            const int top_y = 16 * ch - lw;
            // Walls stop at the neighbour in front, which covers the rest. (A split
            // neighbour in front whose span starts inside it: its back edge is at its
            // stored height.)
            // (Any split neighbour: its edges are at its stored height but for the slit's
            // own width, which the wall behind it, drawn there, covers.)
            auto is_split = [&](int qx, int qz) {
                return drawn_split && in_grid(qx, qz) && drawn_split[gi(qx, qz)] > 0 &&
                       (drawn_split0[gi(qx, qz)] > 0 || drawn_split[gi(qx, qz)] < 32);
            };
            const bool left_stored = is_split(cx, cy + 1), right_stored = is_split(cx + 1, cy);
            const int right_floor = right_stored ? std::max(0, raw_cell(cx + 1, cy, nullptr)) : std::max(0, pass_height(cx + 1, cy));
            const int left_floor = left_stored ? std::max(0, raw_cell(cx, cy + 1, nullptr)) : std::max(0, pass_height(cx, cy + 1));
            const int right_lw = right_stored ? 0 : lower_of(cx + 1, cy), left_lw = left_stored ? 0 : lower_of(cx, cy + 1);
            // For the occlusion: how much higher each of the eight neighbours
            // stands, in world units (16 a step); none off the map.
            float rise[3][3] = {};
            if (shade_on)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nh = (dx || dy) ? cell_height(cx + dx, cy + dy) : -1;
                        rise[dx + 1][dy + 1] = nh < 0 ? 0.0f : std::max(0.0f, 16.0f * (nh - ch));
                    }
            // The occlusion at a point of the top face, (a, b) world units
            // from its back corner along X and Z (0..32): each higher
            // neighbour shades the side next to it, fading over the reach,
            // more the higher it stands (full at two steps); the shades
            // combine as independent ones do.
            // A rounded box among them (kDrawnRounds, the editor's types) shades by the
            // distance to its own shape, the arc at a rounded corner, not to its cells'
            // square: the shade at a round tower's foot lay along the square's sides and
            // stopped at its corner (map 36, Thor state 0278: asked for the AO at their
            // foot to be rounded too). The floor in the box's own cell, past its
            // arc, takes it too. KOL_ROUND_SQUARE_AO=1: as before.
            static const bool round_square_ao = kol::env_set("KOL_ROUND_SQUARE_AO");
            bool cell_round[3][3] = {};
            int near_rounds[9], n_near_rounds = 0;
            if (shade_on && !round_square_ao && !rounds.empty())
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!in_grid(cx + dx, cy + dy)) continue;
                        const int ri = round_of[gi(cx + dx, cy + dy)];
                        if (ri < 0 || round_top[ri] <= round_base[ri] || round_top[ri] <= ch) continue;
                        cell_round[dx + 1][dy + 1] = true;
                        if (std::find(near_rounds, near_rounds + n_near_rounds, ri) == near_rounds + n_near_rounds)
                            near_rounds[n_near_rounds++] = ri;
                    }
            auto round_dist = [&](int ri, float X, float Z) {
                const DrawnRound& d = rounds[ri];
                const float R = static_cast<float>(d.r);
                const float X0 = 32.0f * d.x0, X1 = 32.0f * (d.x1 + 1), Z0 = 32.0f * d.z0, Z1 = 32.0f * (d.z1 + 1);
                auto arc = [&](float ax, float az, float sx, float sz, float* out) {
                    if ((X - ax) * sx <= 0.0f || (Z - az) * sz <= 0.0f) return false;
                    *out = std::max(0.0f, std::sqrt((X - ax) * (X - ax) + (Z - az) * (Z - az)) - R);
                    return true;
                };
                float dist;
                if ((d.corners & kRoundLeft) && arc(X0 + R, Z1 - R, -1.0f, 1.0f, &dist)) return dist;
                if ((d.corners & kRoundFront) && arc(X1 - R, Z1 - R, 1.0f, 1.0f, &dist)) return dist;
                if ((d.corners & kRoundRight) && arc(X1 - R, Z0 + R, 1.0f, -1.0f, &dist)) return dist;
                if ((d.corners & kRoundBack) && arc(X0 + R, Z0 + R, -1.0f, -1.0f, &dist)) return dist;
                const float ex = std::max({X0 - X, X - X1, 0.0f}), ez = std::max({Z0 - Z, Z - Z1, 0.0f});
                return std::sqrt(ex * ex + ez * ez);
            };
            // (The cell behind a doorway set into it, kModelRecessAo: from the door's plane.)
            const float recess_back = shade_on ? recess_depth_at(current_map(), cx, cy - 1, false) : 0.0f;
            const float recess_left = shade_on ? recess_depth_at(current_map(), cx - 1, cy, true) : 0.0f;
            auto top_ao = [&](float a, float b) {
                const float reach = ao_reach_v;
                float open = 1.0f;
                auto shade = [&](float rise_units, float dist) {
                    if (rise_units <= 0.0f || dist >= reach) return;
                    const float t = std::min(1.0f, rise_units / 32.0f) * (1.0f - dist / reach);
                    open *= 1.0f - t;
                };
                const float da0 = a, da1 = 32.0f - a, db0 = b, db1 = 32.0f - b;
                if (!cell_round[0][1]) shade(rise[0][1], da0 + recess_left);
                if (!cell_round[2][1]) shade(rise[2][1], da1);
                if (!cell_round[1][0]) shade(rise[1][0], db0 + recess_back);
                if (!cell_round[1][2]) shade(rise[1][2], db1);
                if (!cell_round[0][0]) shade(rise[0][0], std::sqrt(da0 * da0 + db0 * db0));
                if (!cell_round[2][0]) shade(rise[2][0], std::sqrt(da1 * da1 + db0 * db0));
                if (!cell_round[0][2]) shade(rise[0][2], std::sqrt(da0 * da0 + db1 * db1));
                if (!cell_round[2][2]) shade(rise[2][2], std::sqrt(da1 * da1 + db1 * db1));
                for (int i = 0; i < n_near_rounds; ++i)
                    shade(16.0f * static_cast<float>(round_top[near_rounds[i]] - ch),
                          round_dist(near_rounds[i], 32.0f * cx + a, 32.0f * cy + b));
                return 1.0f - open;
            };
            for (int c = -16; c < 16; ++c) {
                const int px = sx + c + ox;
                if (!draw_col(px)) continue;
                const int half = (c >= 0 ? c : -c - 1) >> 1;
                const int face_top = sy + half;
                const int face_bottom = sy + 16 - half;
                const int floor = c >= 0 ? right_floor : left_floor;
                const int wall_bottom = face_bottom + std::max(0, 8 * (ch - floor) + ((c >= 0 ? right_lw : left_lw) - lw) / 2);
                for (int r = face_top; r < wall_bottom; ++r) {
                    const int py = r + oy;
                    if (!draw_at(px, py)) continue;
                    if (nparts > 1) {   // (this part's pixels only)
                        // (Along Z: a top by its Z offset, the right wall by its own, the
                        // front wall the last part's. Along X: mirrored, the right wall the
                        // last part's.)
                        const int zoff = split_x ? (r < face_bottom ? std::clamp(2 * (r - sy) + c, 0, 31) : c >= 0 ? 32 : std::clamp(33 + 2 * c, 0, 31))
                                                 : (r < face_bottom ? std::clamp(2 * (r - sy) - c, 0, 31) : c >= 0 ? 31 - 2 * c : 32);
                        if (zoff < pz0 || (zoff >= pz1 && !(zoff == 32 && pz1 == 32))) continue;
                    }
                    // Down a wall, one pixel is two units of height.
                    const int y = r < face_bottom ? top_y : top_y - 2 * (r - face_bottom);
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    put_px(k, r, y, r < face_bottom, r < face_bottom ? 0 : c >= 0 ? 1 : 2, gi(cx, cy));
                    g_span_px[k] = nparts > 1 && cpart.drawn && r < face_bottom ? 1 : 0;
                    if (world_dirty) {
                        const int rr2 = 2 * (r - sy);
                        if (r < face_bottom) {
                            g_wx[k] = static_cast<std::int16_t>(32 * cx + std::min(31, std::max(0, rr2 + c)));
                            g_wz[k] = static_cast<std::int16_t>(32 * cy + std::min(31, std::max(0, rr2 - c)));
                        } else if (c >= 0) {
                            g_wx[k] = static_cast<std::int16_t>(32 * (cx + 1));
                            g_wz[k] = static_cast<std::int16_t>(32 * cy + 31 - 2 * c);
                        } else {
                            g_wx[k] = static_cast<std::int16_t>(32 * cx + 33 + 2 * c);
                            g_wz[k] = static_cast<std::int16_t>(32 * (cy + 1));
                        }
                    }
                    if (shade_on) {
                        float occ;
                        float lit = 1.0f;
                        if (r < face_bottom) {
                            // Top face: (c, r - sy) back from the corner is
                            // a = 2 rr + c, b = 2 rr - c (the projection).
                            const float rr = static_cast<float>(r - sy);
                            const float fa = 2.0f * rr + c, fb = 2.0f * rr - c;
                            occ = top_ao(fa, fb);
                            if (sh_s > 0.0f)
                                lit = 1.0f - sh_s * shade_frac(32.0f * cx + std::min(31.9f, std::max(0.0f, fa)),
                                                               32.0f * cy + std::min(31.9f, std::max(0.0f, fb)),
                                                               static_cast<float>(top_y));
                        } else {
                            // A wall, darker toward its foot, where it meets
                            // the ground in front.
                            occ = wall_occ(static_cast<float>(y - 16 * floor));
                            if (sh_s > 0.0f) {
                                // The right wall faces +X, the left one +Z.
                                // Turned away from the sun it is in its own
                                // shadow; turned toward it, it takes what the
                                // terrain casts over the spot in front of it.
                                const bool right = c >= 0;
                                const float facing = right ? sun_x : sun_z;
                                const float along = right ? 32.0f - 2.0f * c - 1.0f : 32.0f + 2.0f * c + 1.0f;
                                const float dark = facing <= 0.0f ? 1.0f
                                    : right ? shade_frac(32.0f * (cx + 1) + 0.5f, 32.0f * cy + along, static_cast<float>(y), 0.0f, 1.0f)
                                            : shade_frac(32.0f * cx + along, 32.0f * (cy + 1) + 0.5f, static_cast<float>(y), 1.0f, 0.0f);
                                lit = 1.0f - sh_s * dark;
                            }
                        }
                        // (A slit's floor: its shading at kSlitShade, as its back wall's.)
                        if (nparts > 1 && cpart.drawn && r < face_bottom) {
                            occ *= kSlitShade;
                            lit = 1.0f - kSlitShade * (1.0f - lit);
                        }
                        g_ao[k] = ao_byte(occ, lit);
                    }
                }
            }
            // The stored part behind a span faces it with a wall (Z offset split0, facing
            // +Z) from its top down to the span's: the slit's back wall, the art's dark band.
            // After the span and before the part in front, whose top hides its lower rows
            // (drawn last, it painted over the near block's top: the slit's near side broke).
            if (split > 0 && split0 > 0 && cpart.drawn && nparts > 1) {
                // (Along X: the wall at X offset split0, facing +X, over the cell's Z.)
                const int Zf = split_x ? 32 * cx + split0 : 32 * cy + split0;
                const int y_top = 16 * stored_h, y_bot = 16 * cell_ch - lower_of(cx, cy);
                for (int X = split_x ? 32 * cy : 32 * cx; X < (split_x ? 32 * cy : 32 * cx) + 32; X += 2) {
                    const int px = (split_x ? Zf - X : X - Zf) / 2 + kx + ox;
                    if (!draw_col(px)) continue;
                    for (int y = y_top; y > y_bot; y -= 2) {
                        const int r = static_cast<int>(std::floor((X + Zf - 2 * y) / 4.0)) + ky;
                        const int py = r + oy;
                        if (!draw_at(px, py)) continue;
                        const std::size_t k = static_cast<std::size_t>(py) * w + px;
                        put_px(k, r, y, false, split_x ? 1 : 2, gi(cx, cy));
                        g_span_px[k] = 1;
                        if (world_dirty) {
                            g_wx[k] = static_cast<std::int16_t>(split_x ? Zf : X);
                            g_wz[k] = static_cast<std::int16_t>(split_x ? X : Zf);
                        }
                        // Lit as a wall down a narrow, deep gap (the user, 2026-11-13: real
                        // lighting inside the slits, on the walls as well as the
                        // floor): in the blocks' shadow whenever the sun casts any, and
                        // darker toward its foot as any wall is, a little more for the gap.
                        if (shade_on) {
                            const float depth = static_cast<float>(y_top - y) / static_cast<float>(std::max(2, y_top - y_bot));
                            // (At kSlitShade of it: the art's wall is dark already, and at
                            // full strength the inside came out near black; asked for it
                            // a little lighter in the slit.)
                            g_ao[k] = ao_byte(kSlitShade * std::max(wall_occ(static_cast<float>(y - y_bot)), 0.25f + 0.35f * depth),
                                              sh_s > 0.0f ? 1.0f - kSlitShade * sh_s : 1.0f);
                        }
                    }
                }
            }
            }   // (the cell's parts)
            if (const int ri = round_of[gi(cx, cy)]; ri >= 0) draw_round(ri, cx, cy);
        }
    }
    };   // run_pass

    // The shift. A camera that pans gives a new key every picture, so the
    // cache above never answers, and the pass drew the whole view again each
    // time: 3.8 ms a job on the Steam Deck in map 89's opening pan, the
    // largest piece of a job that ran over (2026-11-18). But a pan moves the
    // still world over the picture by a whole number of pixels: with the same
    // world, light and view size (still_key), every pixel the last pass drew
    // is this pass's pixel (kx - last kx, ky - last ky) further on, byte for
    // byte (a pixel's values come from its world point; its X + Z from its row
    // less ky). So the last pass's buffers are put back moved, and the pass
    // draws only the strips the move uncovered. The last pass's own buffers
    // are kept for it each time (g_shift: a copy, since the passes after this
    // one write into the globals).
    // KOL_PASS_SHIFT=0: off. KOL_PASS_SHIFT_CHECK=1: the whole pass drawn too
    // and compared (the world places only where a pixel is covered: the pass
    // leaves the rest as they were).
    static const bool shift_on = kol::env_not_zero("KOL_PASS_SHIFT");
    static const bool shift_check = kol::env_true("KOL_PASS_SHIFT_CHECK");
    struct ShiftKept {
        PassBuffers buf;
        unsigned parts = 0;
        std::uint32_t still_key = 0;
        int kx = 0, ky = 0;
        bool valid = false;
        // Its world places (wx, wz) are that pass's view's: it wrote them
        // (world_dirty). A pass that does not leaves the globals as they
        // were, and so does its shift.
        bool wx_valid = false;
        long shifts = 0, checked = 0, mismatched = 0;
    };
    static thread_local ShiftKept g_shift;
    const unsigned parts_now = (world_on ? PassBuffers::kWxz : 0u) | (shade_on ? PassBuffers::kAo : 0u) |
                               (record_back ? PassBuffers::kBack : 0u);
    const int dx = kx - g_shift.kx, dy = ky - g_shift.ky;
    const bool can_shift = shift_on && !pass_hit && g_shift.valid && g_shift.still_key == still_key &&
                           g_shift.parts == parts_now && g_shift.buf.xz.size() == npix &&
                           std::abs(dx) < w / 2 && std::abs(dy) < h / 2 &&
                           (!world_on || !world_dirty || g_shift.wx_valid);
    // (Not moved at all, dx and dy 0, is the same picture: the cache above
    // keeps a pass only from the second time its key comes, so a camera that
    // moves a pixel every other picture drew the whole pass every time.)
    // KOL_PASS_SHIFT_LOG=1: why a pass that missed the cache was not shifted.
    static const bool shift_log = kol::env_true("KOL_PASS_SHIFT_LOG");
    if (shift_log && shift_on && !pass_hit && !can_shift)
        std::fprintf(stderr, "pass-shift: no: kept %d, still key %s (%08x -> %08x), parts %u -> %u, moved %d, %d, world %d dirty %d kept's %d, shadow key %08x\n",
                     g_shift.valid ? 1 : 0, g_shift.still_key == still_key ? "same" : "CHANGED", g_shift.still_key,
                     still_key, g_shift.parts, parts_now, dx, dy, world_on ? 1 : 0, world_dirty ? 1 : 0,
                     g_shift.wx_valid ? 1 : 0, sh_s > 0.0f ? g_shadow_key : 0u);
    if (can_shift) {
        // Pixel (x, y) takes the last pass's (x - dx, y - dy); a pixel with
        // none starts as the pass's clear leaves it.
        const int rx0 = std::max(0, dx), rx1 = std::min(w, w + dx), ry0 = std::max(0, dy), ry1 = std::min(h, h + dy);
        auto moved = [&](auto& dst, const auto& src, int per, auto empty) {
            dst.resize(npix * per);
            for (int y = 0; y < h; ++y) {
                auto* row = dst.data() + static_cast<std::size_t>(y) * w * per;
                if (y < ry0 || y >= ry1) {
                    std::fill_n(row, static_cast<std::size_t>(w) * per, empty);
                    continue;
                }
                std::fill_n(row, static_cast<std::size_t>(rx0) * per, empty);
                std::copy_n(src.data() + (static_cast<std::size_t>(y - dy) * w + (rx0 - dx)) * per,
                            static_cast<std::size_t>(rx1 - rx0) * per, row + static_cast<std::size_t>(rx0) * per);
                std::fill_n(row + static_cast<std::size_t>(rx1) * per, static_cast<std::size_t>(w - rx1) * per, empty);
            }
        };
        const PassBuffers& B = g_shift.buf;
        moved(g_cell_owner, B.owner, 1, static_cast<std::int16_t>(-1));
        moved(g_span_px, B.span, 1, static_cast<std::uint8_t>(0));
        moved(g_round_n, B.round_n, 2, static_cast<std::int8_t>(0));
        moved(g_face, B.face, 1, static_cast<std::uint8_t>(0));
        if (shade_on) moved(g_ao, B.ao, 1, static_cast<std::uint8_t>(255));
        moved(g_xz, B.xz, 1, static_cast<std::int32_t>(0));
        moved(g_y, B.y, 1, static_cast<std::int32_t>(0));
        moved(g_covered, B.covered, 1, static_cast<std::uint8_t>(0));
        moved(g_kind, B.kind, 1, static_cast<std::uint8_t>(kol::kSceneNone));
        if (record_back) {
            moved(g_back_xz, B.back_xz, 1, static_cast<std::int32_t>(0));
            moved(g_back_y, B.back_y, 1, static_cast<std::int32_t>(0));
            moved(g_back_kind, B.back_kind, 1, static_cast<std::uint8_t>(kol::kSceneNone));
            if (shade_on) moved(g_back_ao, B.back_ao, 1, static_cast<std::uint8_t>(255));
        }
        if (world_on && world_dirty) {
            moved(g_wx, B.wx, 1, static_cast<std::int16_t>(0));
            moved(g_wz, B.wz, 1, static_cast<std::int16_t>(0));
        }
        // (The ground plane's X + Z where nothing is drawn, as the clear sets it.)
        for (int py = 0; py < h; ++py) {
            const std::int32_t xz = 4 * (py - oy - ky);
            std::int32_t* row = g_xz.data() + static_cast<std::size_t>(py) * w;
            if (py < ry0 || py >= ry1) { std::fill_n(row, w, xz); continue; }
            std::fill_n(row, rx0, xz);
            std::fill_n(row + rx1, w - rx1, xz);
        }
        if (dx != 0 || dy != 0) run_pass(false, rx0, rx1, ry0, ry1);
        ++g_shift.shifts;
        if (shift_check) {
            PassBuffers whole;
            const std::vector<std::uint8_t> span_shift = g_span_px;
            whole.swap_globals();   // (the shifted pass set aside; the globals empty)
            if (world_on) { g_wx.assign(whole.wx.begin(), whole.wx.end()); g_wz.assign(whole.wz.begin(), whole.wz.end()); }
            run_pass(true, 0, 0, 0, 0);
            bool same = whole.xz == g_xz && whole.y == g_y && whole.covered == g_covered && whole.kind == g_kind &&
                        whole.owner == g_cell_owner && whole.face == g_face && whole.round_n == g_round_n &&
                        span_shift == g_span_px && (!shade_on || whole.ao == g_ao) &&
                        (!record_back || (whole.back_xz == g_back_xz && whole.back_y == g_back_y &&
                                          whole.back_kind == g_back_kind && (!shade_on || whole.back_ao == g_back_ao)));
            if (same && world_on && world_dirty)
                for (std::size_t k = 0; k < npix && same; ++k)
                    same = !g_covered[k] || (whole.wx[k] == g_wx[k] && whole.wz[k] == g_wz[k]);
            ++g_shift.checked;
            if (!same) ++g_shift.mismatched;
            if (!same || g_shift.checked % 300 == 1)
                std::fprintf(stderr, "pass-shift-check: %s, moved by %d, %d (%ld shifts, %ld checked, %ld with a difference)\n",
                             same ? "the same" : "DIFFERENT", dx, dy, g_shift.shifts, g_shift.checked, g_shift.mismatched);
            // (The frame goes on with the whole pass's, which the check trusts.)
        }
    } else if (!pass_hit || checking) {
        run_pass(true, 0, 0, 0, 0);
    }
    // This pass's own pixels, for the next one's shift (not on a hit: the
    // cache's are this view's, and the last kept are still the last pass's).
    if (shift_on && !pass_hit && !checking) {
        g_shift.buf.take(parts_now);
        g_shift.parts = parts_now;
        g_shift.still_key = still_key;
        g_shift.kx = kx;
        g_shift.ky = ky;
        g_shift.valid = true;
        g_shift.wx_valid = world_on && world_dirty;
    }
    if (checking) {
        g_pass.count_check(chk.same(g_pass.parts), "pass");
        // The frame goes on with what the cache restored.
        chk.swap_globals();
    } else if (!pass_hit && pass_cache) {
        if (cache_keep(g_pass.key == pass_key, g_pass.prev_hit)) {
            // The same inputs twice: the camera is still. Keep the pass.
            g_pass.parts = (world_on ? PassBuffers::kWxz : 0u) | (shade_on ? PassBuffers::kAo : 0u) |
                           (record_back ? PassBuffers::kBack : 0u);
            g_pass.buf.take(g_pass.parts);
            g_pass.key = pass_key;
            g_pass.valid = true;
        } else {
            g_pass.key = pass_key;
            g_pass.valid = false;
        }
    }
    g_pass.prev_hit = pass_hit;
}

// A prop's own pixels (a tree, a rock, a torch) are not the ground the
// terrain pass shaded behind them: no occlusion, shadow or torchlight
// from it (EK_tree_shaded: the snowfield's trees took the dark band of
// the wall behind them).
// Lifted (BG1, see lift_props_on), the pixel shows the ground behind the
// prop, which keeps the terrain's light; the prop's own colour goes to
// the atlas first.
// The terrain's shadow over the prop, though, it takes, as a unit does
// (card_shade_row; lifted, through its block's shade_row).
void SceneBuild::prop_light_pass() {
    wt.lap("scene.proplight");
    const bool prop_shade = props_shaded() && g_ao.size() == pixels;
    const std::uint8_t prop_shade_ao =
        prop_shade ? static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - shadow_strength()))) : 255;
    float base_rows = 0.0f, base_dark = 0.0f, base_raise = 0.0f;
    if (g_ao.size() == pixels) prop_base_params(&base_rows, &base_dark, &base_raise);
    auto prop_light = [&](std::size_t k) -> std::uint8_t {
        if (!prop_shade && base_rows <= 0.0f) return 255;
        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
        const float foot_row = static_cast<float>(g_xz[k]) / 4.0f - static_cast<float>(g_y[k]) / 2.0f + ky + oy;
        std::uint8_t light = 255;
        if (prop_shade) {
            const int dxz = 2 * (px - ox - kx);
            const float row = card_shade_row((g_xz[k] + dxz) / 2, (g_xz[k] - dxz) / 2, g_y[k], foot_row);
            light = static_cast<std::uint8_t>(std::lround(
                255.0f * kol::ScenePixels::card_shade(static_cast<float>(py), row, prop_shade_ao / 255.0f,
                                                      kol::card_shade_soft())));
        }
        // Bent (g_card_bend), its foot is its cell's front edge there.
        float foot = foot_row;
        const int pc = g_prop_cell.size() == pixels ? g_prop_cell[k] : -1;
        if (pc >= 0 && static_cast<std::size_t>(pc) < g_card_bend.size() && g_card_bend[pc].sx >= 0) {
            const CardBend& cb = g_card_bend[pc];
            foot += 0.25f * (static_cast<float>(cb.tip) - 2.0f * std::min(16.0f, std::abs(px + 0.5f - cb.sx))) -
                    0.5f * static_cast<float>(px < cb.sx ? cb.dyl : cb.dyr);
        }
        const float f = kol::ScenePixels::prop_base_light(foot - static_cast<float>(py), base_rows, base_dark,
                                                          base_raise);
        return f < 1.0f ? static_cast<std::uint8_t>(std::lround(light * f)) : light;
    };
    static const bool lift_ground_unshaded = std::getenv("KOL_LIFT_GROUND_UNSHADED") != nullptr;
    if (lift_on && !lift_ground_unshaded && g_pre_kind.size() == pixels) g_lift_ground.assign(pixels, 0);
    else g_lift_ground.clear();
    for (std::size_t k = 0; k < pixels; ++k) {
        if (g_prop_dist[k] == kUnreached) continue;
        if (lift_on && scene_top(layers, k) == 1u) {
            if (!g_lift_ground.empty() && !is_flame(k)) g_lift_ground[k] = 1;
            continue;
        }
        if (is_flame(k)) {   // its own light: no shade, no torchlight
            if (g_ao.size() == pixels) g_ao[k] = 255;
            if (g_glow.size() == pixels) g_glow[k] = 0;
            continue;
        }
        if (g_ao.size() == pixels) g_ao[k] = prop_light(k);
        if (g_glow.size() == pixels) g_glow[k] = 0;
    }
    // ... and that shade goes on past the prop's foot onto the ground it
    // stands on, fading to normal (the user asked for the fade to continue
    // down across the tile it sits on): a round spot on the
    // ground at the prop's height, as dark as the prop's foot at its foot
    // point and easing out (smoothstep) with the distance from it, to normal
    // as far as the tile's front corner (16 sqrt 2 for a prop at the cell's
    // middle). A first version darkened only the prop's tile in front of its
    // foot line: hard edges along that line beside the prop and along the
    // tile's sides (the user reported hard edges in every prop's shadow, from
    // the added shade).
    if (base_rows > 0.0f && base_dark > 0.0f && g_ao.size() == pixels) {
        static thread_local std::vector<int> prop_of;
        constexpr float kReach = 22.627417f;   // 16 sqrt 2
        for (const PropGroup& p : scene_props(w, h, prop_of)) {
            // The foot point: under its pixels' middle column, at its place.
            const float dxz = 2.0f * (0.5f * static_cast<float>(p.x0 + p.x1 + 1) - ox - kx);
            const float X = 0.5f * (p.xz + dxz), Z = 0.5f * (p.xz - dxz);
            // The spot's box on screen: X - Z within 2 kReach, X + Z within it.
            const int cxs = static_cast<int>(std::lround(dxz / 2.0f)) + kx + ox;
            const int cys = p.xz / 4 - p.y / 2 + ky + oy;
            // Only a prop that reaches its foot: a flame's loose tongue (map 9's
            // braziers: their art's wisps, each a group of its own, its foot the
            // brazier's) put a spot of its own on the ground 30 rows under it,
            // there in some frames of the flame and not others, a dark patch
            // pulsing by the brazier (the user saw dark spots pulsing about
            // once a second). KOL_SPOT_ANY_GROUP=1: as before.
            static const bool spot_any = std::getenv("KOL_SPOT_ANY_GROUP") != nullptr;
            if (!spot_any && p.y1 < cys - 4) continue;
            // Across, a spot reaches 16 columns (its X - Z within 2 kReach is
            // 16 px either side of its middle column): 17 with the middle's
            // rounding. (24 was the box of X and Z each within kReach.)
            const int rx = 17, ry = static_cast<int>(kReach / 2.0f) + 2;
            for (int py = std::max(0, cys - ry); py <= std::min(h - 1, cys + ry); ++py)
                for (int px = std::max(0, cxs - rx); px <= std::min(w - 1, cxs + rx); ++px) {
                    const std::size_t q = static_cast<std::size_t>(py) * w + px;
                    const bool under = !g_lift_ground.empty() && g_lift_ground[q];   // the ground under a lifted prop
                    if (!g_covered[q] || (under ? g_pre_kind[q] : g_kind[q]) != kol::kSceneFlat ||
                        (g_prop_dist[q] != kUnreached && !under))
                        continue;
                    const int qxz = under ? g_pre_xz[q] : g_xz[q], qy = under ? g_pre_y[q] : g_y[q];
                    if (std::abs(qy - p.y) > 2) continue;   // the ground at the prop's height
                    const float qdxz = 2.0f * static_cast<float>(px - ox - kx);
                    const float qx = 0.5f * (qxz + qdxz), qz = 0.5f * (qxz - qdxz);
                    const float d2 = (qx - X) * (qx - X) + (qz - Z) * (qz - Z);
                    if (d2 > 1.01f * kReach * kReach) continue;   // well outside: no root
                    const float r = std::sqrt(d2) / kReach;
                    if (r >= 1.0f) continue;
                    const float t = 1.0f - r;
                    const float fade = t * t * (3.0f - 2.0f * t);
                    g_ao[q] = static_cast<std::uint8_t>(std::lround(g_ao[q] * (1.0f - base_dark * fade)));
                }
        }
    }

    // Sprites that are not units -- the command menu's text and icons, the
    // cursor, damage numbers -- are interface, not scenery: they stay sharp
    // unless a unit claims the pixel below.
    // With the perspective, the scene as it is without units, for the pixels
    // lifted out to draw units from their own sprites (build_sprite_atlas).
    // KOL_PERSP_NO_SPRITES=1 draws units from the frame as before.
    static const bool no_sprites = std::getenv("KOL_PERSP_NO_SPRITES") != nullptr;
    // Flat (the light without the perspective, kol_perspective's
    // flat_provider): the units' squares are still made, for their shadows
    // and to take the game's blob from under them; nothing is lifted.
    sprite_atlas = (cap.perspective || cap.flat) && !no_sprites;
    if (sprite_atlas) {
        // Only sprite pixels are ever lifted, so only theirs are kept.
        g_under_xz.resize(pixels);
        g_under_y.resize(pixels);
        g_under_kind.resize(pixels);
    }
    // (Both in one pass over the scene's sprite pixels.)
    std::uint8_t* const iface_px = g_iface.data();
    for (std::size_t k = 0; k < pixels; ++k) {
        const bool sprite = scene_top(layers, k) == kol::kLayerObj;
        iface_px[k] = sprite;
        if (sprite_atlas && sprite) {
            g_under_xz[k] = g_xz[k];
            g_under_y[k] = g_y[k];
            g_under_kind[k] = g_kind[k];
        }
    }

    // Sprites: a unit's visible OBJ pixels take its depth, front unit last.
    Unit sorted[kObjects];
    std::copy(units, units + count, sorted);
    std::sort(sorted, sorted + count,
              [](const Unit& a, const Unit& b) { return a.depth < b.depth; });
    for (int i = 0; i < count; ++i) {
        const Unit& u = sorted[i];
        // Feet: the slot convention stands a sprite 8 units below its object.
        const int fx = feet_sx(u) + kx + ox;
        const int fy = feet_sy(u) + ky + oy;
        // The 16x32 sprite, plus the shadow oval drawn under the feet.
        for (int py = std::max(0, fy - 32); py < std::min(h, fy + 8); ++py) {
            for (int px = std::max(0, fx - 12); px < std::min(w, fx + 12); ++px) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (scene_top(layers, k) != kol::kLayerObj) continue;
                g_xz[k] = u.x + u.z;
                g_y[k] = u.y;
                g_kind[k] = kol::kSceneUpright;
                g_iface[k] = 0;
            }
        }
    }
    // Then what each unit really drew, whatever its size. KOL_FOCUS_BOX_ONLY=1
    // keeps the box alone.
    // Bags: things standing on the map that are not units. They follow the
    // units in the array, claim only the sprites no unit's frame takes, and
    // are then drawn upright at their place like a unit. Left as they were,
    // they were interface except where a unit's box above covered them, and
    // came apart under the perspective (states/CP_spell_hand_merged: the bag
    // between Radam and Elrik).
    std::copy(units, units + count, all);
    // A unit on a modelled seat (kUnitSeats): slid along the line of sight.
    if (static const bool seats_off = std::getenv("KOL_NO_UNIT_SEATS") != nullptr; !seats_off) {
        const int map_now = current_map();
        for (const UnitSeat& s : kUnitSeats) {
            if (s.map != map_now) continue;
            for (int i = 0; i < count; ++i) {
                Unit& u = all[i];
                if (u.y != s.from_y || u.x < 0 || u.z < 0 || u.x / 32 != s.cx || u.z / 32 != s.cz) continue;
                const int t = s.seat_y - s.from_y;
                u.x += t;
                u.y += t;
                u.z += t;
            }
        }
    }
    for (int s = 0; s < kSlots && props < kMaxProps; ++s) {
        const std::uint32_t o = kSlotArray - 0x03000000u + s * kSlotStride;
        if (rd32(iw, o) != kBagSlotFn) continue;
        // The slot's Y is 8 below the object's, as for units.
        Unit b{rd16(iw, o + 0x08), rd16(iw, o + 0x0A) + 8, rd16(iw, o + 0x0C), 0};
        b.depth = draw_depth(b.x, b.y, b.z);
        bool dup = false;
        for (int j = 0; j < props; ++j)
            dup = dup || (all[count + j].x == b.x && all[count + j].y == b.y && all[count + j].z == b.z);
        if (!dup) all[count + props++] = b;
    }
    for (int u = 0; u < 64; ++u) g_unit_sky[u] = g_unit_hanging[u] = false;
    for (int u = 0; u < count && u < 64; ++u) {
        const int gh = cell_height(all[u].x >> 5, all[u].z >> 5);
        g_unit_sky[u] = all[u].y > kSkyY && all[u].y - 16 * std::max(0, gh) > kSkyRise;
        g_unit_hanging[u] = !g_unit_sky[u] && all[u].y - 16 * std::max(0, gh) > kHangRise;
    }
    if (!box_only)
        claim_unit_sprites(layers, w, h, ox, oy, kx, ky, all, count, props,
                           cursor, cursor_sx + kx, cursor_sy + ky,
                           g_xz.data(), g_y.data(), g_kind.data(), g_iface.data());
    // A prop under interface: where an interface sprite covers a prop's
    // BG1 pixels, the scene there shows them (the game's picture without
    // sprites) and they are the prop's. Placed as the ground around them,
    // the snow rock beside the command menu lost the corner under its last
    // icon and "Attack": it tore off as shards under the stick
    // (states/FG_slot1_icons_break_stone). They take the place of the prop
    // pixel they touch, and its light (none of the ground's), interface
    // only: through a unit's sprite the prop reached the ground under its
    // feet and took its occlusion and shadow away (BK). With the prop lifted
    // (lift_prop_pixels) they go into its card too: left in the scene, the
    // corner stayed behind as a floating piece of rock.
    if (g_scene_codes && g_prop_dist.size() == pixels) {
        const bool one_foot = kol::map_flag(kol::MapFlag::OneFoot, current_map());
        static thread_local std::vector<std::uint32_t> front;   // (kept: tens of thousands a frame)
        front.clear();
        for (std::size_t k = 0; k < pixels; ++k)
            if (g_prop_dist[k] != kUnreached && !g_iface[k]) front.push_back(static_cast<std::uint32_t>(k));
        for (std::size_t head = 0; head < front.size(); ++head) {
            const std::uint32_t k = front[head];
            const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
            for (const auto& d : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const int qx = px + d.first, qy = py + d.second;
                if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                const std::size_t m = static_cast<std::size_t>(qy) * w + qx;
                if (g_prop_dist[m] != kUnreached || !g_iface[m] || (g_scene_codes[m] & 0x0Fu) != kol::kLayerObj)
                    continue;
                // On a one-creature map (map_settings one_foot: Shaher), a
                // sprite no unit owns over the sky beside him, touching him,
                // is his art too (the tip of his mouth while he casts
                // Apocalypse, OAM 16 x 8 priority 1, states/thor/0238 ~395):
                // as interface it stayed flat at its place while he moved
                // and grew with the combat camera, a piece floating over his
                // head. Not priority 0 (the cursor's hand, the menus).
                // KOL_ONE_FOOT_NO_SPRITES=1: interface, as before.
                bool own_art = false;
                if ((g_scene_codes[m] >> 4) != 1u) {
                    static const bool no_sprites = std::getenv("KOL_ONE_FOOT_NO_SPRITES") != nullptr;
                    const int oi = g_obj_top_valid && g_obj_top.size() == pixels ? g_obj_top[m] : -1;
                    own_art = !no_sprites && one_foot && oi >= 0 && oi < 128 && g_entries[oi].owner < 0 &&
                              ((g_entries[oi].a2 >> 10) & 3u) != 0u;
                    if (!own_art) continue;
                    g_iface[m] = 0;
                }
                // (Every pixel of a prop has the prop's place.)
                g_prop_dist[m] = g_prop_dist[k];
                g_prop_cell[m] = g_prop_cell[k];   // the same prop (the chimney under the hand, GP)
                g_xz[m] = g_xz[k];
                g_y[m] = g_y[k];
                g_kind[m] = g_kind[k];
                // The prop's light (none), unless it is lifted: then the
                // pixel shows the ground behind and keeps its light, as the
                // prop's own lifted pixels do (above).
                if (!lift_on && g_ao.size() == pixels) g_ao[m] = prop_light(m);
                if (!lift_on && g_glow.size() == pixels) g_glow[m] = 0;
                front.push_back(static_cast<std::uint32_t>(m));
            }
        }
    }
}

// The units' and props' atlas squares, their shadows and light.
void SceneBuild::atlas() {
    wt.lap("scene.atlas");
    if (sprite_atlas && !box_only) {
        build_sprite_atlas(layers, w, h, ox, oy, kx, ky, all, count + props, count);
        // A hanging unit's card lifted with the perspective: what the scene
        // shows behind it is the ground the compositor drew there, at the
        // ground's place, not at the unit's. The boat's flag (map 58) hangs
        // 120 over its deck: the deck and the castle behind it stood at the
        // flag's 168, took its shadow and moved with it (states/HM). A unit
        // standing on the ground keeps its place there, as before.
        // And it hangs from the model at the depth of what it hangs from: the
        // game's place fixes it on the screen only up to the line of sight
        // (X, Y, Z) + t (1, 1, 1), and map 97's flag at 166 had its top 23
        // over the yard it touches (223): under the perspective it came off
        // the yard by a few pixels near the view's bottom (states/HJ, present
        // 320). The cards slide along the line of sight until their top row
        // is at the height of the model's pixels just over it (their median),
        // which leaves the flat picture as it was. KOL_NO_HANG_ANCHOR=1: off.
        static const bool hang_anchor = std::getenv("KOL_NO_HANG_ANCHOR") == nullptr;
        const std::size_t npx = static_cast<std::size_t>(w) * h;
        std::vector<int> hang_t(g_casters.size(), 0);
        for (std::size_t bi = 0; hang_anchor && g_modeled.size() == npx && bi < g_casters.size() &&
                                 bi < g_blocks.size(); ++bi) {
            const BlockCaster& c = g_casters[bi];
            if (!c.hanging || (bi > 0 && g_casters[bi - 1].upper && g_casters[bi - 1].hanging)) continue;
            const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];   // its top card (upper, if any)
            const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
            std::vector<int> ys;
            for (int tx = 0; tx < b.w; ++tx) {
                if (!g_atlas_opaque[static_cast<std::size_t>(ay0) * g_atlas_w + ax0 + tx]) continue;
                const int px = b.sx + tx, py = b.sy - 1;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (g_modeled[k] && g_covered[k]) ys.push_back(g_y[k]);
            }
            if (ys.empty()) continue;
            std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
            const float top_y = static_cast<float>(c.foot) + 8.0f +
                                2.0f * (c.feet_hy - static_cast<float>(b.sy - oy) - 0.5f);
            const int t = std::clamp(static_cast<int>(std::lround(ys[ys.size() / 2] - top_y)), -96, 96);
            for (std::size_t bj = bi; bj < g_casters.size() && bj < g_blocks.size(); ++bj) {
                BlockCaster& cj = g_casters[bj];
                if (cj.x != c.x || cj.z != c.z || cj.foot != c.foot) break;   // its cards, one after another
                hang_t[bj] = t;
            }
        }
        for (std::size_t bi = 0; bi < hang_t.size(); ++bi) {
            if (!hang_t[bi]) continue;
            const int t = hang_t[bi];
            BlockCaster& c = g_casters[bi];
            c.x += t; c.z += t; c.foot += t; c.ground += t;
            g_blocks[bi].xz += 2 * t;
            g_blocks[bi].y += t;
        }
        if (g_under_xz.size() == g_lift.size())
            for (std::size_t k = 0; k < g_lift.size(); ++k) {
                const int b = g_lift[k];
                if (!b || b > static_cast<int>(g_casters.size()) || !g_casters[b - 1].hanging) continue;
                if (cap.perspective) {
                    g_xz[k] = g_under_xz[k];
                    g_y[k] = g_under_y[k];
                    g_kind[k] = g_under_kind[k];
                } else {   // drawn in the frame (flat light): at the card's place
                    g_xz[k] += 2 * hang_t[b - 1];
                    g_y[k] += hang_t[b - 1];
                }
            }
        wt.lap("scene.sun_cast");
        cover_shade(w, h, ox, oy, kx, ky);
        cast_unit_shadows(w, h, ox, oy, kx, ky);
        wt.lap("scene.lamp_cast");
        cast_lamp_shadows(w, h, ox, oy, kx, ky);
        wt.lap("scene.unit_ao+glow+win");
        unit_contact_ao(w, h, ox, oy, kx, ky);
        unit_lamp_glow(w, h, ox, oy, kx, ky);
        unit_card_lamp_light(w, h, ox, oy, lamps,
                             [this](std::size_t i, float X, float Y, float Z) { return lamp_seen(i, X, Y, Z); });
        unit_window_light(w, h, ox, oy, kx, ky, sun_x, sun_z, cell_heights);
        // ... and the windows with a light of their own on them (map 67's
        // moonlight on the woman on the stairs; the user reported she did not
        // light up in it): the same, as the second set, into g_moon_unit_*.
        g_moon_unit_any = false;
        if (g_moon_on && g_moon_beams.valid) {
            WinSetScope moon(g_moon_rise, g_moon_beams, g_unit_win_any, g_moon_unit_any, false,
                             {{&g_unit_win_patch, &g_moon_unit_patch}, {&g_unit_win_haze, &g_moon_unit_haze},
                              {&g_unit_win_patch_a, &g_moon_unit_patch_a}, {&g_unit_win_haze_a, &g_moon_unit_haze_a}});
            unit_window_light(w, h, ox, oy, kx, ky, g_moon_dx, g_moon_dz, cell_heights);
        }
        cast_window_shadows(w, h, ox, oy, kx, ky);
        wt.lap("scene.lift");
        if ((lift_on || walls_on) && !g_atlas_bailed) lift_prop_pixels(layers, w, h, lift_on, walls_on, ox, oy, kx, ky);
        if (!g_atlas_bailed) grow_over_sprite(layers, w, h);
        if (!g_atlas_bailed) props_over_units(layers, w, h, lift_on, walls_on);
        if ((lift_on || walls_on) && !g_atlas_bailed)
            lift_window_light(w, h, ox, oy, kx, ky, sun_x, sun_z, cell_heights);
    }
    else g_atlas_on = g_wall_px_any = false;
}

// Debug > Tile Info: the cell under the game's cursor, else the one a
// third of the way down the middle of the view (a story scene has no
// cursor, its dialogue boxes sit lower; the D-pad moves its camera,
// kol_actions.cpp), and what is known of it: the heights as
// stored and as drawn, the map's byte and the game's record for it.
void SceneBuild::debug_tile_info() {
    g_tile_target = -1;
    g_tile_rect[0] = -1;
    // Debug > Scene Map's viewer has a tile cursor (kol_actions steps it with
    // the D-pad): set here from the view's middle, and the camera that puts
    // its cell at the middle of the game's window (hardware 120, 80) at its
    // drawn height, from kx = A - camera (the scroll follows the camera one
    // for one): goal = A - (120 - (X - Z) / 2), likewise for y.
    const bool viewer = kol::debug_scene_viewer_active();
    int view_cx = -1, view_cz = -1;
    if (viewer && !kol::debug_viewer_cell(&view_cx, &view_cz) &&
        g_cell_owner.size() == static_cast<std::size_t>(w) * h) {
        int cell = -1;
        for (int py = vy + vh / 2; py < std::min(h, vy + vh / 2 + 48) && cell < 0; ++py)
            cell = g_cell_owner[static_cast<std::size_t>(py) * w + vx + vw / 2];
        // KOL_VIEWER_CELL starts the cursor even with no cell at the middle
        // (maps 77, 78, 83: backdrop there at 384 and 480).
        if (cell >= 0) kol::debug_viewer_cell_init(cell_x(cell), cell_y(cell));
        else if (std::getenv("KOL_VIEWER_CELL")) kol::debug_viewer_cell_init(0, 0);
        kol::debug_viewer_cell(&view_cx, &view_cz);
    }
    // kx + camera is learned once, on a frame the camera stood still since
    // the last: kx comes from the sprite slots, which can trail the camera a
    // frame while it moves, and a goal from each frame's sum shook the
    // camera (it chased a goal that moved with it).
    static int last_cam_x = 0, last_cam_y = 0;
    const int cam_x = static_cast<std::int16_t>(rd16(ew, e(0x02026C78u)));
    const int cam_y = static_cast<std::int16_t>(rd16(ew, e(0x02026C7Au)));
    int ax = 0, ay = 0;
    if (viewer && !kol::debug_viewer_anchor(&ax, &ay) && cam_x == last_cam_x && cam_y == last_cam_y)
        kol::debug_viewer_anchor_set(kx + cam_x, ky + cam_y);
    last_cam_x = cam_x;
    last_cam_y = cam_y;
    if (viewer && in_grid(view_cx, view_cz) && kol::debug_viewer_anchor(&ax, &ay)) {
        const int cell = gi(view_cx, view_cz);
        const int stored = in_game(view_cx, view_cz) ? ew[e(kGrid) + game_cell(view_cx, view_cz) + 1] : 0;
        const int height = drawn[cell] >= 0 ? drawn[cell] : stored;
        const int X = 32 * view_cx + 16, Z = 32 * view_cz + 16, Y = 16 * height;
        kol::debug_viewer_camera_goal(ax - (120 - (X - Z) / 2), ay - (80 - (X + Z) / 4 + Y / 2));
    }
    if (kol::setting_on(kol::Setting::DebugTileInfo)) {
        int cell = -1;
        if (viewer && in_grid(view_cx, view_cz)) {
            cell = gi(view_cx, view_cz);
        } else if (cursor) {
            if (cursor_x >= 0 && cursor_z >= 0 && cursor_x < 32 * kGameSide && cursor_z < 32 * kGameSide)
                cell = gi(cursor_x / 32, cursor_z / 32);
        } else if (g_cell_owner.size() == static_cast<std::size_t>(w) * h) {
            // That point, or the nearest drawn pixel below it.
            for (int py = vy + vh / 3; py < std::min(h, vy + vh / 3 + 24) && cell < 0; ++py)
                cell = g_cell_owner[static_cast<std::size_t>(py) * w + vx + vw / 2];
        }
        // A cell picked on the second screen's editor page, until the game's
        // cursor moves.
        cell = kol::tile_edit_resolve_target(cell);
        {
            // The grid for that page: each cell's drawn height and flags.
            static thread_local std::vector<int> grid_drawn;
            static thread_local std::vector<unsigned char> grid_flags;
            static thread_local std::vector<std::uint8_t> grid_water;
            grid_drawn.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
            grid_flags.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
            grid_water.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
            water_cells(map, grid_water.data());
            for (int i = 0; i < kGridSide * kGridSide; ++i) {
                const bool game = in_game(cell_x(i), cell_y(i));
                const std::uint32_t gc = game ? e(kGrid) + game_cell(cell_x(i), cell_y(i)) : 0u;
                const int st = game ? ew[gc + 1] : 0;
                const bool walk = game && (ew[gc + 2] & 0x80u) != 0;
                grid_drawn[i] = drawn[i] >= 0 ? drawn[i] : st;
                grid_flags[i] = static_cast<unsigned char>((walk ? kol::kTileWalkable : 0) |
                                                           (!walk && st == 0 ? kol::kTileOffField : 0) |
                                                           (grid_water[i] ? kol::kTileWater : 0));
            }
            kol::tile_edit_publish(map, grid_drawn.data(), grid_flags.data(), cell);
        }
        if (cell >= 0) {
            const int cx = cell_x(cell), cz = cell_y(cell);
            // Beyond the game's grid a cell reads as off the field (byte FF).
            static const std::uint8_t kOff[4] = {0xFF, 0, 0, 0};
            const std::uint8_t* const cb = in_game(cx, cz) ? ew + e(kGrid) + game_cell(cx, cz) : kOff;
            bool prop = false;
            const int stored = cb[1];
            raw_cell(cx, cz, &prop);
            const int d = drawn[cell];
            const bool edited = kol::tile_edit_get(map, cell) >= 0;
            kol::tile_edit_set_target(map, cell, d >= 0 ? d : stored);
            // The editor: a roof under the target, the field the D-pad edits,
            // a roof's first corner waiting.
            static const char* const kField[] = {"HEIGHT", "EAVE", "RIDGE", "GABLES", "AXIS",
                                                 "SHAPE", "OPEN", "SHIFT X", "SHIFT Z"};
            const kol::TileRoof* roof = nullptr;
            for (const kol::TileRoof& r : roofs)
                if (cx >= r.x0 && cx <= r.x1 && cz >= r.z0 && cz <= r.z1) roof = &r;
            const int field = roof ? kol::tile_edit_field() % 9 : 0;
            int corner_cell = -1;
            const bool corner = kol::tile_edit_corner(map, &corner_cell);
            char roof_line[220] = "";
            if (roof) {
                // A single slope falls across the ridges' axis: along Z when
                // they run along X.
                const char across = roof->along_x ? 'Z' : 'X';
                char shape[40];
                static const char* const kCorner[] = {"FRONT +X+Z", "RIGHT +X-Z", "BACK -X-Z", "LEFT -X+Z"};
                if (roof->shape == 0) std::snprintf(shape, sizeof shape, "GABLE");
                else if (roof->shape <= 2)
                    std::snprintf(shape, sizeof shape, "SLOPE DOWN TO %c%c", roof->shape == 1 ? '+' : '-', across);
                else if (roof->shape <= 6) std::snprintf(shape, sizeof shape, "PEAK AT %s", kCorner[roof->shape - 3]);
                else if (roof->shape <= 10) std::snprintf(shape, sizeof shape, "LOW AT %s", kCorner[roof->shape - 7]);
                else std::snprintf(shape, sizeof shape, "PYRAMID");
                std::snprintf(roof_line, sizeof roof_line,
                              "\nROOF X %d-%d Z %d-%d  RIDGES ALONG %c  %s\nGABLES %d  EAVE %d%s  RIDGE %d%s  %s"
                              "  SHIFT X %d/8 Z %d/8",
                              roof->x0, roof->x1, roof->z0, roof->z1, roof->along_x ? 'X' : 'Z', shape, roof->gables,
                              roof->eave2 / 2, roof->eave2 % 2 ? ".5" : "", roof->ridge2 / 2, roof->ridge2 % 2 ? ".5" : "",
                              roof->open ? "OPEN (AWNING)" : "WALLS", roof->dx8, roof->dz8);
                g_tile_rect[0] = roof->x0; g_tile_rect[1] = roof->z0;
                g_tile_rect[2] = roof->x1; g_tile_rect[3] = roof->z1;
            } else if (corner) {
                std::snprintf(roof_line, sizeof roof_line, "\nROOF CORNER X %d Z %d  L2 + B AGAIN TO MAKE IT",
                              cell_x(corner_cell), cell_y(corner_cell));
                g_tile_rect[0] = std::min(cx, cell_x(corner_cell));
                g_tile_rect[1] = std::min(cz, cell_y(corner_cell));
                g_tile_rect[2] = std::max(cx, cell_x(corner_cell));
                g_tile_rect[3] = std::max(cz, cell_y(corner_cell));
            }
            // A box on the cell (Select makes one, and switches the
            // fields to it and back).
            static const char* const kBoxField[] = {"TOP", "PEAK", "SIZE X", "SIZE Z", "SHIFT X", "SHIFT Z", "AXIS"};
            const kol::TileBox* box = nullptr;
            const std::vector<kol::TileBox> cell_boxes = kol::tile_edit_boxes(map);
            for (const kol::TileBox& b : cell_boxes)
                if (b.x == cx && b.z == cz) box = &b;
            const bool box_focus = box && kol::tile_edit_box_focus();
            char box_line[200] = "";
            if (box) {
                auto halves = [](int v, char* out) { std::snprintf(out, 8, "%d%s", v / 2, v % 2 ? ".5" : ""); };
                char t[8], pk[8];
                halves(box->top2, t);
                halves(std::max(box->top2, box->peak2), pk);
                std::snprintf(box_line, sizeof box_line,
                              "\nBOX TOP %s  PEAK %s  RIDGE ALONG %c\nSIZE %d/8 X %d/8  SHIFT X %d/8 Z %d/8%s", t, pk,
                              box->along_x ? 'X' : 'Z', box->size_x8, box->size_z8, box->dx8, box->dz8,
                              box_focus ? "" : "  (L2 + SELECT  EDIT)");
                if (box_focus) {
                    g_tile_rect[0] = g_tile_rect[2] = cx;
                    g_tile_rect[1] = g_tile_rect[3] = cz;
                }
            }
            const char* field_name = box_focus ? kBoxField[kol::tile_edit_box_field() % 7]
                                     : field != 0 ? kField[field]
                                     : kol::tile_edit_terrain_field() == 1 ? "TYPE" : "HEIGHT";
            // Every target has two fields at least now: the terrain's height
            // and type.
            const char* hints = box_focus ? "   L2 + LEFT/RIGHT  FIELD\nL2 + A  DELETE BOX   L2 + SELECT  BACK"
                              : roof ? "   L2 + LEFT/RIGHT  FIELD\nL2 + A  DELETE ROOF"
                              : corner ? "   L2 + LEFT/RIGHT  FIELD\nL2 + B  OTHER CORNER   L2 + A  RESET"
                                       : "   L2 + LEFT/RIGHT  FIELD\nL2 + B TWICE  MAKE A ROOF   L2 + A  RESET";
            const int type = kol::tile_edit_type(map, cell);
            char type_text[32] = "";
            if (type != kol::kTypeSquare) std::snprintf(type_text, sizeof type_text, "  %s", kol::tile_type_name(type));
            else if (round_of[cell] >= 0) std::snprintf(type_text, sizeof type_text, "  ROUNDED");
            char text[768];
            std::snprintf(text, sizeof text,
                          "MAP %d  CELL X %d Z %d\nSTORED %d  DRAWN %d%s\nBYTE %02X  RECORD %d\n%s  %s%s%s%s\n"
                          "L2 + UP/DOWN  %s%s%s",
                          map, cx, cz, stored, d >= 0 ? d : stored,
                          edited ? "  (EDITED)" : d >= 0 ? "  (TABLE)" : "", cb[0], in_game(cx, cz) ? record_of(cb[0]) : -1,
                          (cb[2] & 0x80u) ? "WALKABLE" : stored == 0 ? "OFF FIELD" : "NOT WALKABLE",
                          !prop ? "TERRAIN" : is_spire(cx, cz) ? "CUT-OUT PROP" : "TREE PROP",
                          type_text, roof_line, box_line, field_name,
                          // On a roof or a box, left / right pick the field;
                          // elsewhere there is only the height (the readout
                          // used to offer left / right everywhere).
                          hints, box ? "" : "\nL2 + SELECT  ADD A BOX");
            g_tile_text = text;
            g_tile_target = cell;
        }
    }
}

// Interface, the cursor, the combat camera's overlays, Debug > Tile Info,
// and what the depth of field and the warp read (g_info).
void SceneBuild::rest() {
    wt.lap("scene.rest");
    // Under interface (the cursor's hand, a menu) the scene shows the ground
    // (kol_perspective.cpp fills it from the compositor's scene without
    // sprites), so it takes the ground's place, not the place of the unit
    // whose box it lies in. The hand at the bottom of its bob is within 32
    // rows of the feet of the unit on the cursor's cell; the ground under it
    // stood up at the unit's depth, and the mesh pulled a sheet of ground out
    // to it under the stick (states/CM_hand_distorts.state).
    if (sprite_atlas && !box_only) {
        const std::size_t n = static_cast<std::size_t>(w) * h;
        for (std::size_t k = 0; k < n; ++k)
            if (g_iface[k] && scene_top(layers, k) == kol::kLayerObj) {
                g_xz[k] = g_under_xz[k];
                g_y[k] = g_under_y[k];
                g_kind[k] = g_under_kind[k];
            }
    }
    // The grid cursor: the one sprite that is not a unit and still belongs to
    // the scene. (Its slot is absent while a command menu is up, so a menu is
    // never caught here.) Its slot's position is the centre of its cell's top
    // face. It draws two things (states/CB_sprites_broken.state):
    // - corner pieces around that centre, 9 rows above it to 8 below, lying ON
    //   the face. They take the place of the face under them, so they stay on
    //   their cell whatever the camera does. Standing upright at the centre,
    //   the front piece broke up under the right stick.
    // - a pointing hand floating above, 27-43 rows up. The ground seen around
    //   it is rows farther back, so in the mesh its corners dragged that ground
    //   along (the user reported the hand cursor warping the scenery).
    //   With the perspective it stays interface, filled under like a menu, and
    //   is drawn flat on top where its cell has gone (g_floating,
    //   kol_perspective.cpp). Without it nothing moves: it stands on the cell.
    g_floating.assign(static_cast<std::size_t>(w) * h, 0);
    g_floating_any = false;
    g_cursor_face.clear();
    if (cursor) build_weather_px(layers, w, h, ox, oy);
    // Only while the cursor is drawn. Its slot stays on screen under a menu
    // opened from the map, and the box took the menu's sprites: the System
    // menu's icons in its lower rows were laid on the cell, the rest of them
    // and the menu's hand floated, and under the stick part of the icons and
    // the hand stuck to the ground (states/thor/0099, map 41, no unit on the
    // field). The corner pieces are drawn whenever the cursor is out (CK,
    // CM, BT, DV, FI, FJ: four); without the latched OAM, the box as before.
    // KOL_CURSOR_BOX_UNDRAWN=1: the box regardless.
    static const bool box_undrawn = std::getenv("KOL_CURSOR_BOX_UNDRAWN") != nullptr;
    bool cursor_drawn = box_undrawn || !g_obj_top_valid;
    for (int k = 0; k < g_entry_n && k < 128 && !cursor_drawn; ++k) cursor_drawn = g_cursor_piece[k];
    if (cursor && cursor_drawn) {
        const bool float_hand = cap.perspective;
        const int fx = cursor_sx + kx + ox;
        const int fy = cursor_sy + ky + oy;
        for (int py = std::max(0, fy - 64); py < std::min(h, fy + 12); ++py) {
            for (int px = std::max(0, fx - 16); px < std::min(w, fx + 16); ++px) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (!g_iface[k]) continue;
                // By what drew the pixel, not by its row: the hand bobs, and
                // at the bottom of its bob its lower rows were taken for
                // corner pieces and laid on the cell's face, which pulled a
                // sheet of ground out to the hand under the stick
                // (states/CM_hand_distorts.state). Without the latched OAM,
                // the rows decide as before.
                const int top = g_obj_top_valid ? g_obj_top[k] : -1;
                // Not the weather falling through its box: the snow's flakes
                // (plain sprites, interface) joined the hand, and the hand
                // took a new shape every frame and moved oddly as the camera
                // turned (the user reported snow, unlike rain, affecting the
                // cursor and shadows, visible on the hand cursor with the
                // camera turned sideways).
                // KOL_WEATHER_IN_HAND=1: as before.
                static const bool weather_in_hand = std::getenv("KOL_WEATHER_IN_HAND") != nullptr;
                if (!weather_in_hand && ((top >= 0 && top < 128 && g_weather_piece[top]) ||
                                         (g_weather_px.size() == g_floating.size() && g_weather_px[k])))
                    continue;
                const bool on_face = top >= 0 ? g_cursor_piece[top] : py >= fy - 16;
                if (on_face) {                        // on the face
                    g_xz[k] = 4 * (py - oy - ky) + 2 * cursor_y;
                    g_y[k] = cursor_y;
                    g_kind[k] = kol::kSceneFlat;
                    g_iface[k] = 0;
                    // Unlit: on map 52 at 0.07 the white target square went
                    // nearly black (the user, 2026-11-10, states/thor/0178,
                    // asked for it to be unaffected by shadows).
                    if (g_cursor_face.empty()) g_cursor_face.assign(static_cast<std::size_t>(w) * h, 0);
                    g_cursor_face[k] = 1;
                } else if (float_hand) {              // in the air
                    g_floating[k] = 1;
                    g_floating_any = true;
                } else {
                    g_xz[k] = cursor_xz;
                    g_y[k] = cursor_y;
                    g_kind[k] = kol::kSceneUpright;
                    g_iface[k] = 0;
                }
            }
        }
    }

    combat_overlays(cap, layers, w, h, units, count, ox, oy, kx, ky);

    debug_tile_info();

    g_info.w = w; g_info.h = h; g_info.ox = ox; g_info.oy = oy;
    g_info.vx = vx; g_info.vy = vy; g_info.vw = vw; g_info.vh = vh;
    g_info.kx = kx; g_info.ky = ky;
    std::copy(units, units + count, g_info.units);
    g_info.count = count; g_info.moved = moved; g_info.moved_count = moved_count;
    g_info.cursor = cursor; g_info.cursor_xz = cursor_xz; g_info.cursor_y = cursor_y;
    // A spell's effect, as one card on the unit under it (ScenePixels::
    // spell_card): the picture drawn flat for it in the view -- the HUD
    // layers and interface sprites, which in a spell's effect are the
    // spell's -- and the unit whose feet lie below its middle, nearest its
    // column (Summon Darkness: its circle floats over the target and the
    // column comes down onto it).
    g_info.fall_card = false;
    g_info.fall_ready = false;
    static const bool no_fall = std::getenv("KOL_NO_FALL_CARD") != nullptr;
    for (const FallCard& f : kFallCards)
        if (!no_fall && f.map == map && cap.story) {
            g_info.fall_ready = true;
            g_info.fall_card = (cap.dispcnt & 0x8000u) && (cap.dispcnt & 0x0C00u);
            g_info.fall_v[0] = cap.bg23_v[0]; g_info.fall_v[1] = cap.bg23_v[1];
            g_info.fall_on = (cap.dispcnt >> 10) & 3u;
            const int ex[3] = {f.x0, f.x1, f.x2}, ez[3] = {f.z0, f.z1, f.z2};
            for (int i = 0; i < 3; ++i) {
                g_info.fall_fx[i] = (ex[i] - ez[i]) / 2 + kx + ox;
                g_info.fall_fy[i] = (ex[i] + ez[i]) / 4 - f.y / 2 + ky + oy;
                g_info.fall_xz[i] = ex[i] + ez[i];
            }
            g_info.fall_y = f.y;
        }
    g_info.spell_card = false;
    if (cap.spell_camera && count > 0) {
        int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
        for (int py = vy; py < vy + vh; ++py)
            for (int px = vx; px < vx + vw; ++px) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                const unsigned top = layers[k] & 0x0Fu, under = layers[k] >> 4;
                const bool effect = top == 2u || top == 3u || (top == 4u && (under == 2u || under == 3u)) ||
                                    (g_iface[k] && !(g_floating_any && g_floating[k]));
                if (!effect) continue;
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
            }
        if (bx1 >= bx0) {
            const int mx = (bx0 + bx1) / 2, my = (by0 + by1) / 2;
            int best = -1, best_dx = 0, best_dy = 0;
            for (int i = 0; i < count; ++i) {
                const int fx = feet_sx(units[i]) + kx + ox;
                const int fy = feet_sy(units[i]) + ky + oy;
                if (fy < my) continue;
                const int dx = std::abs(fx - mx), dy = std::abs(fy - by1);
                if (best < 0 || dx < best_dx || (dx == best_dx && dy < best_dy)) {
                    best = i; best_dx = dx; best_dy = dy;
                }
            }
            if (best >= 0 && best_dx <= 24) {
                g_info.spell_card = true;
                g_info.spell_fx = feet_sx(units[best]) + kx + ox;
                g_info.spell_fy = feet_sy(units[best]) + ky + oy;
                g_info.spell_xz = units[best].x + units[best].z;
                g_info.spell_y = units[best].y - 8;
            }
        }
    }
}

bool SceneBuild::run() {
    if (!camera_and_units()) return false;
    wt.lap("setup.cells");
    cell_rules();
    wt.lap("setup.roofs+shade");
    roofs_and_shade();
    wt.lap("setup.lamps");
    gather_lamps();
    wt.lap("setup.key");
    terrain_pass();
    glow_pass();
    roofs_pass();
    boxes_pass();
    model_pass();
    prop_base();
    cutouts();
    bg1_shapes();
    spread();
    prop_light_pass();
    atlas();
    rest();
    return true;
}
}  // namespace kol_focus_detail

namespace {
// Every pixel's place in the scene (g_xz, g_y, g_kind, g_iface, g_covered)
// for the frame being presented. False when this is not a battle map.
bool build_scene_now(const std::uint8_t* layers, int w, int h) {
    SceneBuild b(layers, w, h);
    return b.run();
}
}  // namespace

namespace kol {
PresentScope::PresentScope() : prev(t_present) { t_present = true; }
PresentScope::~PresentScope() { t_present = prev; }

const std::uint8_t* present_layers(const std::uint8_t* layers, int w, int h) {
    if (!layers || !g_cap.world_bg2 || w <= 0 || h <= 0) return layers;
    // One copy a picture, the same pointer for the focus and the warp:
    // build_scene builds once for both by it.
    static std::vector<std::uint8_t> c;
    static const std::uint8_t* of = nullptr;
    static unsigned long serial = ~0ul;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (of == layers && serial == g_cap.serial && c.size() == n) return c.data();
    of = layers;
    serial = g_cap.serial;
    c.assign(layers, layers + n);
    for (std::uint8_t& v : c) {
        if (v == gba::kLayerNotComposed) continue;
        if ((v & 0x0Fu) == 2u) v &= 0xF0u;
        if ((v >> 4) == 2u) v &= 0x0Fu;
    }
    return c.data();
}
unsigned present_exclude_layers() { return g_cap.exclude_layers; }

void sun_arc_move(float degrees_right) {
    // The pick's horizon is on the screen's right when its direction is
    // (screen right is X - Z): there, moving right is back toward it.
    float lx, lz;
    shadow_dir_pick(lx, lz);
    const float side = lx - lz >= 0.0f ? 1.0f : -1.0f;
    // Kept within the arc, so moving back answers at once.
    const float base = sun_degrees_base();
    g_sun_arc = std::max(kArcLowest - base, std::min(kArcHighest - base, g_sun_arc - degrees_right * side));
}
void sun_arc_reset() { g_sun_arc = g_sun_turn = 0.0f; }

// Maps where the right stick does not move the camera (asked 2026-10-15 for
// such maps, starting with 89).
// Map 71, the boat-builder's shed (the user, 2026-10-30, asked for the camera
// to be fixed on this map).
// (The maps: map_settings.cfg `no_look`.)
bool map_small_look() {
    const int map = current_map();
    return map >= 0 && kol::map_flag(kol::MapFlag::SmallLook, map);
}
bool map_no_look() {
    const int map = current_map();
    return map >= 0 && kol::map_flag(kol::MapFlag::NoLook, map);
}
void sun_turn_move(float degrees_clockwise) {
    g_sun_turn = std::remainder(g_sun_turn + degrees_clockwise, 360.0f);
}

namespace {
std::atomic<bool> g_sun_save{false}, g_sun_clear{false};
std::atomic<bool> g_sun_ok{false};
std::atomic<float> g_sun_now_deg{0.0f}, g_sun_now_x{0.0f}, g_sun_now_z{0.0f};
}  // namespace
void sun_save_request() { g_sun_save = true; }
void sun_clear_request() { g_sun_clear = true; }
void sun_tick() {
    const std::uint8_t* ew = guest_ew();
    const bool on = ew && kol::shadow_sun_degrees() > 0.0f;
    const int map = ew ? rd16(ew, kMapNumber - 0x02000000u) : 0;
    float lx = 0.0f, lz = 0.0f;
    if (on) shadow_dir(lx, lz);
    if (g_sun_save.exchange(false) && on) {
        // Where it is now becomes the map's own; the arc and the turn start
        // again from it, so the picture does not move. It is saved as the
        // first turn's height: the turns' drop is added back.
        kol::tile_edit_set_sun(map, {std::min(89.0f, sun_degrees() + sun_turn_drop()), lx, lz});
        g_sun_arc = g_sun_turn = 0.0f;
        shadow_dir(lx, lz);
    }
    if (g_sun_clear.exchange(false) && kol::tile_edit_clear_sun(map)) {
        g_sun_arc = g_sun_turn = 0.0f;
        if (on) shadow_dir(lx, lz);
    }
    g_sun_ok = on;
    if (on) {
        g_sun_now_deg = sun_degrees();
        g_sun_now_x = lx;
        g_sun_now_z = lz;
    }
}
bool sun_now(float* degrees, float* x, float* z) {
    if (!g_sun_ok.load()) return false;
    *degrees = g_sun_now_deg.load();
    *x = g_sun_now_x.load();
    *z = g_sun_now_z.load();
    return true;
}
void sun_arc_report() {
    float lx, lz;
    shadow_dir(lx, lz);
    std::fprintf(stderr, "sun: %.1f degrees along the arc from the Shadows pick's horizon, %.1f above the ground: KOL_PERSP_SHADOW_ELEV=%.1f KOL_PERSP_SHADOW_DIR=%.3f,%.3f\n",
                 sun_arc_angle(), sun_degrees(), sun_degrees(), lx, lz);
    std::fflush(stderr);
}

float g_cam_cap_strike();
float g_cam_cap_look(int i);
// A spell's circle on BG2 (map_settings.cfg `magic_circle`; asked
// 2026-11-11 on Thor state 0232, map 57, for Shaher's summoning spell and its
// red floor circle to look good). The
// game draws it in screen space: BG2, mode 0, one colour (an entry the game
// pulses), blended as the first target (EVB 16: added; fading in, less). Taken for a
// HUD layer it was light drawn flat (hud_glow), off the floor as soon as
// the camera moved, and faint over the lit floor. Here: BG2's whole tilemap
// (the circle is all in it, more than the GBA's 160 rows show), where frame
// pixels show it (by BG2's scroll and the frame's native origin, checked
// against the frame's BG2 pixels), and its colour (from the frame);
// kol_perspective
// (magic_circle) lays it on the floor by each scene pixel's place in the
// game's picture and draws it after the light. KOL_NO_MAGIC_CIRCLE=1: as
// before (hud_glow); KOL_CIRCLE_LOG=1.
void circle_capture(Capture& c, gba::GbaBus* bus, gba::GbaPpu* ppu) {
    c.circle_on = false;
    static const bool off = kol::env_set("KOL_NO_MAGIC_CIRCLE");
    static const bool log = kol::env_set("KOL_CIRCLE_LOG");
    const int map = rd16(c.ew.data(), kMapNumber - 0x02000000u);
    if (off || !c.perspective || !kol::map_flag(kol::MapFlag::MagicCircle, map) || !c.obj_ok) return;
    const std::uint8_t* io = ppu->latched_io();
    const std::uint8_t* vram = bus->vram_ptr();
    const int eva = std::min(16, c.bldalpha & 31), evb = std::min(16, (c.bldalpha >> 8) & 31);
    if (!io || !vram || (c.dispcnt & 7u) != 0u || !(c.dispcnt & 0x0400u) || !(c.bldcnt & 0x04u) ||
        ((c.bldcnt >> 6) & 3u) != 1u || eva <= 0)
        return;
    const unsigned cnt = io[0x0C] | (io[0x0D] << 8);
    if ((cnt & 0xC080u) != 0u) return;   // (256 x 256, 4 bits a texel only)
    const int hofs = (io[0x18] | (io[0x19] << 8)) & 0x1FF, vofs = (io[0x1A] | (io[0x1B] << 8)) & 0x1FF;
    const std::size_t chars = ((cnt >> 2) & 3u) * 0x4000u, screen = ((cnt >> 8) & 31u) * 0x800u;
    static thread_local std::vector<std::uint8_t> mask;
    mask.assign(256u * 256u, 0);
    int n = 0, x0 = 256, y0 = 256, x1 = -1, y1 = -1;
    unsigned slot = 0;
    for (int ty = 0; ty < 32; ++ty)
        for (int tx = 0; tx < 32; ++tx) {
            const unsigned e = vram[screen + 2u * (ty * 32 + tx)] | (vram[screen + 2u * (ty * 32 + tx) + 1] << 8);
            const std::size_t tile = chars + (e & 0x3FFu) * 32u;
            if (tile + 32u > 0x10000u) continue;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const unsigned b = vram[tile + y * 4 + x / 2], v = (x & 1) ? b >> 4 : b & 15u;
                    if (!v) continue;
                    const int X = tx * 8 + ((e & 0x400u) ? 7 - x : x), Y = ty * 8 + ((e & 0x800u) ? 7 - y : y);
                    mask[static_cast<std::size_t>(Y) * 256 + X] = 1;
                    slot = ((e >> 12) & 15u) * 16u + v;
                    ++n;
                    x0 = std::min(x0, X); y0 = std::min(y0, Y); x1 = std::max(x1, X); y1 = std::max(y1, Y);
                }
        }
    // A circle: wide, and lying (an ellipse twice as wide as high, the floor's).
    if (n < 1500 || x1 - x0 < 120 || 3 * (y1 - y0) > 2 * (x1 - x0)) {
        if (log) std::fprintf(stderr, "circle: none (n %d box %d,%d-%d,%d)\n", n, x0, y0, x1, y1);
        return;
    }
    // Where the frame shows it: frame (x, y) is native (x - nx0, y - ny0),
    // BG2's texel (native + scroll) & 255. The native origin from the view;
    // checked on the frame's own BG2 pixels (the top layer), else searched
    // near it.
    const std::uint8_t* lay = ppu->latched_layers();
    const int fw = static_cast<int>(ppu->render_width()), fh = static_cast<int>(ppu->render_height());
    if (!lay || fw <= 0 || fh <= 0) return;
    static thread_local std::vector<std::uint32_t> seen;
    seen.clear();
    // Only the game's own 240 x 160: past it the widescreen may have continued
    // BG2's rows as a bar (bands that are no part of the circle), and with them
    // the check failed and the circle fell back to flat light, bands and all.
    const int nx0 = std::max(0, (fw - 240) / 2), ny0 = std::max(0, (fh - 160) / 2);
    for (int y = ny0 + (ny0 & 1); y < std::min(fh, ny0 + 160); y += 2)
        for (int x = nx0 + (nx0 & 1); x < std::min(fw, nx0 + 240); x += 2)
            if ((lay[static_cast<std::size_t>(y) * fw + x] & 0x0Fu) == 2u) seen.push_back(static_cast<std::uint32_t>(y) << 16 | x);
    auto hits = [&](int du, int dv) {
        int h = 0;
        for (std::uint32_t p : seen) {
            const int u = (static_cast<int>(p & 0xFFFF) + du) & 255, v = (static_cast<int>(p >> 16) + dv) & 255;
            h += mask[static_cast<std::size_t>(v) * 256 + u];
        }
        return h;
    };
    int du = hofs - (fw - 240) / 2, dv = vofs - (fh - 160) / 2;
    int best = seen.empty() ? 0 : hits(du, dv);
    if (!seen.empty() && best * 10 < static_cast<int>(seen.size()) * 9) {
        const int du0 = du, dv0 = dv;
        for (int a = -64; a <= 64; ++a)
            for (int b = -64; b <= 64; ++b) {
                const int h = hits(du0 + a, dv0 + b);
                if (h > best) { best = h; du = du0 + a; dv = dv0 + b; }
            }
    }
    if (log)
        std::fprintf(stderr, "circle: n %d box %d,%d-%d,%d scroll %d,%d frame %dx%d view %d,%d,%d,%d seen %zu hits %d at %d,%d slot %u eva %d\n",
                     n, x0, y0, x1, y1, hofs, vofs, fw, fh, c.view[0], c.view[1], c.view[2], c.view[3], seen.size(), best, du, dv, slot, eva);
    if (seen.empty() || best * 10 < static_cast<int>(seen.size()) * 9) return;
    // One period of the tilemap (it wraps every 256): the one the frame
    // shows, its BG2 pixels' middle inside it.
    {
        long sx = 0, sy = 0;
        for (std::uint32_t p : seen) { sx += static_cast<long>(p & 0xFFFF); sy += static_cast<long>(p >> 16); }
        const int mx = static_cast<int>(sx / static_cast<long>(seen.size())), my = static_cast<int>(sy / static_cast<long>(seen.size()));
        du -= 256 * static_cast<int>(std::floor((mx + du) / 256.0));
        dv -= 256 * static_cast<int>(std::floor((my + dv) / 256.0));
    }
    c.circle.assign(mask.begin(), mask.end());
    c.circle_u = du;
    c.circle_v = dv;
    c.circle_box[0] = x0; c.circle_box[1] = y0; c.circle_box[2] = x1; c.circle_box[3] = y1;
    // Its colour, as the frame shows it: the frame's less the scene's (the
    // picture without it) at EVB, over EVA, on its pixels that did not run
    // into white; the mean. (Not the palette's: the latched entry reads
    // black while the frame draws it red-pink; the game sets it during the
    // frame.) Times EVA / 16: the share of it the frame shows.
    (void)slot;
    const std::uint8_t* fb = ppu->latched_framebuffer();
    const std::uint8_t* sc = ppu->latched_scene();
    const int sw = static_cast<int>(ppu->latched_scene_width()), sh = static_cast<int>(ppu->latched_scene_height());
    const int svx = static_cast<int>(ppu->latched_scene_view_x()), svy = static_cast<int>(ppu->latched_scene_view_y());
    if (!fb || !sc || svx + fw > sw || svy + fh > sh) return;
    // Per channel: a channel that ran into white on a pixel gives only a
    // floor for it there (the most of those floors is kept); at the pulse's
    // peak the red runs into white on every pixel (24 pictures of each 90),
    // and the circle fell back to hud_glow, flat and off the floor.
    double sum[3] = {0.0, 0.0, 0.0};
    int got[3] = {0, 0, 0}, floor_of[3] = {0, 0, 0};
    for (std::uint32_t p : seen) {
        const int x = static_cast<int>(p & 0xFFFF), y = static_cast<int>(p >> 16);
        const std::uint8_t* f = fb + (static_cast<std::size_t>(y) * fw + x) * 3;
        const std::uint8_t* b = sc + (static_cast<std::size_t>(y + svy) * sw + x + svx) * 3;
        for (int ch = 0; ch < 3; ++ch) {
            const int v = std::clamp((f[ch] * 16 - b[ch] * evb) / eva, 0, 255);
            if (f[ch] >= 248) {
                floor_of[ch] = std::max(floor_of[ch], v);
            } else {
                sum[ch] += v;
                ++got[ch];
            }
        }
    }
    if (static_cast<int>(seen.size()) < 16) return;
    for (int ch = 0; ch < 3; ++ch) {
        const double own = got[ch] >= 16 ? std::max(sum[ch] / got[ch], static_cast<double>(floor_of[ch]))
                                         : static_cast<double>(floor_of[ch]);
        sum[ch] = own;
        c.circle_rgb[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(own * eva / 16.0), 0L, 255L));
    }
    c.circle_eva = eva;
    c.circle_evb = evb;
    c.circle_on = true;
    if (log)
        std::fprintf(stderr, "circle: on rgb %u,%u,%u (own %.0f,%.0f,%.0f) eva %d evb %d\n", c.circle_rgb[0], c.circle_rgb[1],
                     c.circle_rgb[2], sum[0], sum[1], sum[2], eva, evb);
}

void present_capture() {
    Capture& c = g_cap;
    c.valid = false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!bus || !ppu) return;
    const std::uint8_t* ew = bus->ewram_ptr();
    const std::uint8_t* iw = bus->iwram_ptr();
    if (!ew || !iw) return;
    c.ew.assign(ew, ew + 0x40000u);
    kol::map_source_bg1_kept_into(c.ew.data(), static_cast<unsigned long>(c.ew.size()));
    c.iw.assign(iw, iw + 0x8000u);
    const std::uint8_t* oam = ppu->latched_oam();
    const std::uint8_t* tiles = ppu->latched_obj_tiles();
    const std::uint8_t* pal = ppu->latched_pal();
    c.obj_ok = oam && tiles && pal;
    if (c.obj_ok) {
        c.oam.assign(oam, oam + 0x400u);
        c.obj_tiles.assign(tiles, tiles + 0x8000u);
        c.pal.assign(pal, pal + 0x400u);
    }
    for (int i = 0; i < 128; ++i) {
        int x = 0, y = 0;
        c.obj_drawn[i] = c.obj_ok && ppu->latched_obj_drawn(i, &x, &y);
        c.obj_x[i] = static_cast<std::int16_t>(x);
        c.obj_y[i] = static_cast<std::int16_t>(y);
    }
    c.ui_field = kol::ui_field(c.ui_field_rgb);
    c.ring_on = kol::spell_ring(c.ring);
    c.dispcnt = ppu->latched_dispcnt();
    c.bldcnt = ppu->latched_bldcnt();
    c.bldalpha = ppu->latched_bldalpha();
    c.bldy = ppu->latched_bldy();
    if (const std::uint8_t* io = ppu->latched_io()) {
        c.winin = static_cast<std::uint16_t>(io[0x48] | io[0x49] << 8);
        c.winout = static_cast<std::uint16_t>(io[0x4A] | io[0x4B] << 8);
    } else {
        c.winin = c.winout = 0xFFFFu;
    }
    c.bg0_ok = ppu->latched_bg0_scroll(&c.bg0_h, &c.bg0_v);
    c.bg1_ok = false;
    if (const std::uint8_t* lio = ppu->latched_io()) {
        c.bg1_h = (lio[0x14] | (lio[0x15] << 8)) & 0x1FF;
        c.bg1_v = (lio[0x16] | (lio[0x17] << 8)) & 0x1FF;
        c.bg1_on = ((lio[0x00] | (lio[0x01] << 8)) & 0x0200u) != 0u;
        c.bg23_v[0] = (lio[0x1A] | (lio[0x1B] << 8)) & 0x1FF;
        c.bg23_v[1] = (lio[0x1E] | (lio[0x1F] << 8)) & 0x1FF;
        c.bg1_ok = true;
    }
    c.scene_w = static_cast<int>(ppu->latched_scene_width());
    c.scene_h = static_cast<int>(ppu->latched_scene_height());
    const std::uint8_t* under = ppu->latched_scene_under();
    const std::uint8_t* codes = ppu->latched_scene_own_layers();
    const std::size_t scene_px = static_cast<std::size_t>(c.scene_w) * c.scene_h;
    c.under_ok = under && codes && scene_px > 0;
    if (const std::uint8_t* rb = ppu->latched_row_backdrop(); rb && c.scene_h > 0)
        c.row_backdrop.assign(rb, rb + static_cast<std::size_t>(c.scene_h) * 3);
    else
        c.row_backdrop.clear();
    if (c.under_ok) {
        c.under.assign(under, under + scene_px * 3);
        c.codes.assign(codes, codes + scene_px);
    }
    for (int i = 0; i < 4; ++i) c.view[i] = gbarecomp::g_present_frame_view[i];
    c.hofs = g_hofs;
    c.vofs = g_vofs;
    c.scroll_valid = g_scroll_valid;
    // The serial of the frame whose picture this is. The frame hook counts
    // at a frame's start (line 228); a present the game yields to late, in
    // the next frame's visible lines (its VBlank handler ran past VBlank;
    // gbarecomp presents it as the VBlank's frame), found the count a frame
    // on, and the next present the same count: the snow, the wet look and
    // everything timed by the serial stepped 2, then 0 (~6% of pictures on
    // map 34: flakes standing still a picture, then jumping; the user still
    // found it not quite smooth with every picture two refreshes).
    // KOL_SERIAL_AT_PRESENT=1: the count at the present, as before.
    static const bool serial_at_present = std::getenv("KOL_SERIAL_AT_PRESENT") != nullptr;
    c.serial = g_frame_serial;
    if (!serial_at_present && ppu && ppu->vcount() < 160 && c.serial > 0) --c.serial;
    // The layers the latched scene left out (the HUD, BG2|BG3), as the
    // frame was composed: the live mask may already be the next frame's.
    c.exclude_layers = ppu->latched_scene_exclude();
    // BG2 in the scene: a piece of the world laid over BG0 (map 84's cliff
    // over its stairs). Everything after reads it as BG0, the ground it
    // covers: the scene's own codes here, the layer maps in present_layers.
    c.world_bg2 = c.exclude_layers == 0x8u && (c.dispcnt & 0x0400u) != 0u;
    c.world_bg2_eva = ((c.bldcnt >> 6) & 3u) == 1u && (c.bldcnt & 0x4u) != 0u
                          ? std::min(16, static_cast<int>(c.bldalpha & 31u)) : 16;
    c.sea_px.clear();
    if (c.world_bg2 && sea_plane_y(current_map()) >= 0) {
        c.sea_px.assign(c.codes.size(), 0);
        for (std::size_t i = 0; i < c.codes.size(); ++i)
            if (c.codes[i] == gba::kLayerNotComposed) continue;
            else if ((c.codes[i] & 0x0Fu) == 2u) c.sea_px[i] = 1;
            else if ((c.codes[i] >> 4) == 2u) c.sea_px[i] = 2;
            // A sprite over the boat layer's own painted sea at the hull's foot
            // (the game's foam over that dark paint): sea too. Hidden under our
            // foam (Special Effects), that paint showed as a dark band between
            // the hull and the foam (the user, 2026-11-11, reported the
            // darkening remained).
            else if ((c.codes[i] & 0x0Fu) == kol::kLayerObj && (c.codes[i] >> 4) == 0u && c.under_ok &&
                     c.under.size() >= (i + 1) * 3) {
                const std::uint8_t* u = c.under.data() + i * 3;
                if (u[2] > u[0] + 40 && u[2] > u[1] + 10) c.sea_px[i] = 2;
            }
    }
    if (c.world_bg2)
        for (std::uint8_t& v : c.codes) {
            if (v == gba::kLayerNotComposed) continue;
            if ((v & 0x0Fu) == 2u) v &= 0xF0u;
            if ((v >> 4) == 2u) v &= 0x0Fu;
        }
    c.perspective = perspective_enabled();
    c.flat = perspective_flat_enabled();
    c.combat_camera = perspective_combat_camera();
    c.spell_camera = perspective_spell_camera();
    c.spell_running = perspective_spell_running();
    // A spell's effect on the affine BG2 (mode 1: Ice Javelin's spike, states/
    // GD) is not sprites: its light comes from the frame's BG2 pixels.
    c.spell_bg.clear();
    if (c.spell_running && (c.dispcnt & 7u) == 1u && ppu->has_latched_framebuffer() && ppu->latched_layers()) {
        const int fw = static_cast<int>(ppu->render_width()), fh = static_cast<int>(ppu->render_height());
        const std::uint8_t* fb = ppu->latched_framebuffer();
        const std::uint8_t* lay = ppu->latched_layers();
        for (int y = 0; y < fh; y += 2)
            for (int x = 0; x < fw; x += 2) {
                const std::size_t k = static_cast<std::size_t>(y) * fw + x;
                if ((lay[k] & 0x0Fu) != 2u) continue;
                const std::uint8_t* p = fb + k * 3;
                if (spell_light_colour(p[0], p[1], p[2]))
                    c.spell_bg.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), p[0], p[1], p[2]});
            }
    }
    // The field through the game's windows (ScenePixels::window_light): mode
    // 0, the field layer on and an alpha blend's first target, WIN0 or WIN1 on
    // (no OBJ window) and the field not shown in every part of them alike.
    // The windows' lines from gbarecomp's latched_line_windows (the game
    // rewrites WIN0H/WIN1H in HBlank: the last line's alone say nothing).
    // KOL_NO_WINDOW_LIGHT=1: the field over every pixel, as before.
    c.window_light = {};
    {
        static const bool off = kol::env_set("KOL_NO_WINDOW_LIGHT");
        const int F = c.ui_field;
        const std::uint8_t* lw = ppu->latched_line_windows();
        const std::size_t LB = gba::GbaPpu::kLineWindowBytes;
        const unsigned effect = (c.bldcnt >> 6) & 3u;
        bool ok = !off && (c.perspective || c.flat) && lw && F >= 2 && (c.dispcnt & 7u) == 0u &&
                  (c.dispcnt & (0x100u << F)) != 0u && (c.dispcnt & 0x6000u) != 0u && (c.dispcnt & 0x8000u) == 0u &&
                  effect == 1u && ((c.bldcnt >> F) & 1u) != 0u;
        // One DISPCNT, WININ and WINOUT down the frame.
        for (int y = 0; ok && y < 160; ++y)
            ok = std::memcmp(lw + y * LB + 8, lw + 159 * LB + 8, 6) == 0 &&
                 (static_cast<unsigned>(lw[y * LB + 12] | lw[y * LB + 13] << 8)) == c.dispcnt;
        kol::ScenePixels::WindowLight wl;
        if (ok) {
            const unsigned winin = lw[159 * LB + 8] | lw[159 * LB + 9] << 8, winout = lw[159 * LB + 10] | lw[159 * LB + 11] << 8;
            const unsigned ctl[3] = {winin & 0x3Fu, (winin >> 8) & 0x3Fu, winout & 0x3Fu};
            for (int i = 0; i < 3; ++i)
                wl.show[i] = !((ctl[i] >> F) & 1u) ? 0 : (ctl[i] & 0x20u) ? 1 : 2;
            // A span by mGBA's rules (gbarecomp window_span_contains); false
            // when it wraps round the screen (not one run).
            auto span = [](int start, int end, int limit, int* a, int* b) {
                if (start > limit && start > end) start = 0;
                if (end > limit) { end = limit; if (start > limit) start = limit; }
                if (end < start) return false;
                *a = start; *b = end;
                return true;
            };
            for (int w = 0; ok && w < 2; ++w) {
                auto& win = wl.win[w];
                win.on = (c.dispcnt & (0x2000u << w)) != 0u;
                if (!win.on) continue;
                int lines = 0;
                for (int y = 0; ok && y < 160; ++y) {
                    const std::uint8_t* r = lw + y * LB;
                    int v0 = 0, v1 = 0, h0 = 0, h1 = 0;
                    const bool vin = span(r[5 + 2 * w], r[4 + 2 * w], 160, &v0, &v1) ? (y >= v0 && y < v1)
                                         : (y < r[4 + 2 * w] || y >= r[5 + 2 * w]);
                    if (!span(r[1 + 2 * w], r[2 * w], 240, &h0, &h1)) { ok = false; break; }
                    if (!vin) h0 = h1 = 0;
                    win.x0[y] = static_cast<std::uint8_t>(h0);
                    win.x1[y] = static_cast<std::uint8_t>(h1);
                    if (h1 > h0) ++lines;
                }
                // A disc: the lines that reach neither side give its centre's
                // column (their middles' mean) and, half width h at line
                // middle y: h^2 + y^2 = (r^2 - cy^2) + 2 cy y, least squares;
                // taken when every line's span is within 1.5 px of it.
                double sx = 0, sy = 0, syy = 0, sz = 0, szy = 0;
                int n = 0;
                for (int y = 0; y < 160; ++y) {
                    const int a0 = win.x0[y], a1 = win.x1[y];
                    if (a1 <= a0 || a0 <= 0 || a1 >= 240) continue;
                    const double h = 0.5 * (a1 - a0), yc = y + 0.5, z = h * h + yc * yc;
                    sx += 0.5 * (a0 + a1); sy += yc; syy += yc * yc; sz += z; szy += z * yc;
                    ++n;
                }
                const double den = n * syy - sy * sy;
                if (!ok || n < 6 || lines < 6 || den <= 0.0) continue;
                const double B = (n * szy - sy * sz) / den, A = (sz - B * sy) / n;
                const double cy = 0.5 * B, r2 = A + cy * cy, cx = sx / n;
                if (r2 <= 4.0) continue;
                bool fits = true;
                for (int y = 0; fits && y < 160; ++y) {
                    const double dy = y + 0.5 - cy, h2 = r2 - dy * dy;
                    const double e0 = h2 > 0.0 ? std::clamp(cx - std::sqrt(h2), 0.0, 240.0) : 0.0;
                    const double e1 = h2 > 0.0 ? std::clamp(cx + std::sqrt(h2), 0.0, 240.0) : 0.0;
                    const int a0 = win.x0[y], a1 = win.x1[y];
                    if (a1 <= a0) fits = e1 - e0 <= 3.0;
                    else fits = std::abs(a0 - e0) <= 1.5 && std::abs(a1 - e1) <= 1.5;
                }
                if (!fits) continue;
                win.disc = true;
                win.cx = static_cast<float>(cx); win.cy = static_cast<float>(cy); win.r = static_cast<float>(std::sqrt(r2));
            }
        }
        if (ok && wl.win[0].on != wl.win[1].on) wl.show[wl.win[0].on ? 1 : 0] = wl.show[2];   // (a window that is off: as outside)
        if (ok && !(wl.show[0] == wl.show[1] && wl.show[1] == wl.show[2] && wl.show[0] != 0)) {
            wl.on = true;
            std::copy_n(c.ui_field_rgb, 3, wl.rgb);
            // field * EVA + under * EVB. EVB 16: the field's light is added,
            // as the game adds it. Else one colour laid over at alpha a, under *
            // (1 - a) + colour * a: exact when the weights sum to 16.
            const int eva = std::min(16, c.bldalpha & 31), evb = std::min(16, (c.bldalpha >> 8) & 31);
            wl.eva = eva;
            wl.add = evb >= 16;
            const float a = wl.add ? 1.0f : std::clamp(1.0f - evb / 16.0f, 0.0f, 1.0f);
            for (int ch = 0; ch < 3; ++ch)
                wl.rgba[ch] = a > 0.0f ? static_cast<std::uint8_t>(std::clamp(std::lround(wl.rgb[ch] * eva / 16.0f / a), 0L, 255L)) : 0;
            wl.rgba[3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
            for (int w = 0; w < 2 && !wl.anchor; ++w)
                if (wl.win[w].on && wl.win[w].disc) { wl.anchor = true; wl.ax = wl.win[w].cx; wl.ay = wl.win[w].cy; }
            c.window_light = wl;
        }
    }
    circle_capture(c, bus, ppu);
    c.bg1_flame = false;
    if (c.bg1_on && kol::map_flag(kol::MapFlag::GlowFlame, rd16(c.ew.data(), kMapNumber - 0x02000000u)))
        if (const std::uint8_t* fb = ppu->latched_framebuffer(); fb && ppu->latched_layers()) {
            const std::uint8_t* lay = ppu->latched_layers();
            const std::size_t n = static_cast<std::size_t>(ppu->render_width()) * ppu->render_height();
            int seen = 0;
            for (std::size_t k = 0; k < n && seen < 40; ++k) {
                if ((lay[k] & 0x0Fu) != 1u) continue;
                const int r = fb[k * 3], g = fb[k * 3 + 1], b = fb[k * 3 + 2];
                // (kol_perspective glow_flame_px.)
                seen += std::max(g, b) >= 48 && 10 * g >= 16 * r && 10 * b >= 14 * r && 100 * g >= 45 * b;
            }
            c.bg1_flame = seen >= 40;
        }
    // HUD layers blended over the picture as light (ScenePixels::hud_glow):
    // mode 0, an alpha blend with BG2 or BG3 first and both weights on. Each
    // such pixel's own colour: the frame's less the scene's (the picture
    // without the HUD layers) at EVB, over EVA. KOL_NO_HUD_GLOW=1: flat HUD.
    c.hud_glow.clear();
    c.hud_bake.clear();
    c.hud_glow_add = false;
    c.hud_glow_eva = 16;
    c.hud_glow_layers = 0;
    c.hud_glow_apex_ok = false;
    // The last picture took its apex (the hold below): only pictures in a row.
    static bool g_glow_held = false;
    const bool glow_held = g_glow_held;
    g_glow_held = false;
    {
        static const bool off = kol::env_set("KOL_NO_HUD_GLOW");
        // (Not BG2 while it is a piece of the world, c.world_bg2: map 84's old
        // cliff cross-fading into its stairs is the world, not light; taken for
        // a HUD layer it stood as a broad see-through sheet to the view's edges,
        // states/thor/0183 frames ~460-680; the user, 2026-11-11, saw the ice
        // wall as a huge transparent sheet over the screen.)
        const unsigned first = c.bldcnt & 0x0Cu & (c.dispcnt >> 8) & (c.world_bg2 ? ~0x04u : ~0u);
        const int eva = std::min(16, c.bldalpha & 31), evb = std::min(16, (c.bldalpha >> 8) & 31);
        // Blended at EVA 0 over everything under it (BG0, BG1, the sprites
        // that are on, the backdrop all second targets), the layer shows
        // nothing: light of none, not interface. After Shaher's attack the
        // game leaves BG2 on over the whole screen at BLDALPHA 0x1000 (map 57,
        // states/thor/0235 from present ~240, the misses' numbers): taken for
        // the top, his pixels were not his card's but the far ground's, at
        // full blur (the user, 2026-11-12, reported him badly out of focus, as
        // if far in the background). KOL_ZERO_GLOW_HUD=1: as before.
        static const bool zero_hud = kol::env_set("KOL_ZERO_GLOW_HUD");
        if (!off && !zero_hud && c.perspective && (c.dispcnt & 7u) == 0u && ((c.bldcnt >> 6) & 3u) == 1u && first &&
            eva == 0 && evb > 0) {
            const unsigned below = (c.dispcnt >> 8 & 0x3u) | (c.dispcnt & 0x1000u ? 0x10u : 0u) | 0x20u;
            if ((below & ~(c.bldcnt >> 8)) == 0u) c.hud_glow_layers = first;
        }
        if (!off && c.perspective && (c.dispcnt & 7u) == 0u && ((c.bldcnt >> 6) & 3u) == 1u && first && eva > 0 && evb > 0 &&
            ppu->has_latched_framebuffer() && ppu->latched_layers() && ppu->latched_scene()) {
            const int fw = static_cast<int>(ppu->render_width()), fh = static_cast<int>(ppu->render_height());
            const int sw = static_cast<int>(ppu->latched_scene_width()), sh = static_cast<int>(ppu->latched_scene_height());
            const int svx = static_cast<int>(ppu->latched_scene_view_x()), svy = static_cast<int>(ppu->latched_scene_view_y());
            const std::uint8_t* fb = ppu->latched_framebuffer();
            const std::uint8_t* lay = ppu->latched_layers();
            const std::uint8_t* sc = ppu->latched_scene();
            if (svx + fw <= sw && svy + fh <= sh) {
                c.hud_glow_layers = first;
                static thread_local std::vector<std::size_t> sat;
                static thread_local std::vector<std::uint32_t> known;
                sat.clear();
                known.clear();
                static const bool bake_on = kol::env_not_zero("KOL_HUD_BAKE");
                const bool bake = bake_on && (c.spell_running || c.spell_camera || c.combat_camera);
                // In a battle action the rest is added as the game blends it
                // (ScenePixels::hud_glow_add): the fading whirlwind (0233 ~142)
                // came out red-brown stripes as light, its difference over a
                // small EVA magnifying the frame's rounding, then drawn at 0.75
                // of its brightest channel. KOL_HUD_GLOW_ADD=0: as light.
                static const bool add_on = kol::env_not_zero("KOL_HUD_GLOW_ADD");
                c.hud_glow_add = add_on && bake;
                c.hud_glow_eva = eva;
                for (int y = 0; y < fh; ++y)
                    for (int x = 0; x < fw; ++x) {
                        const std::size_t k = static_cast<std::size_t>(y) * fw + x;
                        const unsigned top = lay[k] & 0x0Fu;
                        if ((top != 2u && top != 3u) || !((first >> top) & 1u)) continue;
                        if (c.window_light.on && static_cast<int>(top) == c.ui_field) continue;   // (window_light draws it)
                        if (c.circle_on && top == 2u) continue;   // (on the floor: circle_capture)
                        const std::uint8_t* f = fb + k * 3;
                        const std::uint8_t* b = sc + (static_cast<std::size_t>(y + svy) * sw + x + svx) * 3;
                        // In a battle action, over BG1 (a prop: Shaher, map 57):
                        // the frame's own colour on the prop, not light. Taken
                        // as light, his Cataclysm's whirlwind over his body
                        // (states/thor/0233, presents ~120-145) came out tan, then
                        // pink stripes as it faded: the difference over EVA,
                        // small, magnified the frame's rounding; and the game's
                        // weights darken there (EVA + EVB under 16), which light
                        // cannot. KOL_HUD_BAKE=0: light, as before.
                        // (Not over the backdrop: baked into the warp's sky, a
                        // pillar of light (Apocalypse, states/thor/0238 ~400)
                        // was covered in pieces by the moved board and his
                        // card. Its colour there comes right as the game's
                        // blend instead: hud_glow_add.)
                        if (bake && (lay[k] >> 4) == 1u) {
                            c.hud_bake.push_back({static_cast<std::uint32_t>((y + svy) * sw + x + svx), {f[0], f[1], f[2]}});
                            continue;
                        }
                        std::uint8_t t[3];
                        bool clamped = false;
                        for (int ch = 0; ch < 3; ++ch) {
                            t[ch] = static_cast<std::uint8_t>(std::clamp((f[ch] * 16 - b[ch] * evb) / eva, 0, 255));
                            clamped = clamped || f[ch] >= 248;   // (the blend ran into white there: t is a floor)
                        }
                        if (std::max({t[0], t[1], t[2]}) < 8 && !clamped) continue;
                        c.hud_glow.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), t[0], t[1], t[2]});
                        if (clamped) { sat.push_back(c.hud_glow.size() - 1); continue; }
                        const std::uint32_t v = static_cast<std::uint32_t>(t[0]) << 16 | static_cast<std::uint32_t>(t[1]) << 8 | t[2];
                        if (known.size() < 64 && std::find(known.begin(), known.end(), v) == known.end()) known.push_back(v);
                    }
                // Where the blend saturated (a beam over the bright floor: the
                // difference came out dim and green), the layer's colour is one
                // of those seen whole elsewhere (over the void): the dimmest of
                // them no channel of which is under the floor found.
                static thread_local std::vector<std::int32_t> glow_at;   // a frame pixel's place in hud_glow, -1 none
                if (c.window_light.on) {
                    glow_at.assign(static_cast<std::size_t>(fw) * fh, -1);
                    for (std::size_t i = 0; i < c.hud_glow.size(); ++i)
                        glow_at[static_cast<std::size_t>(c.hud_glow[i].y) * fw + c.hud_glow[i].x] = static_cast<std::int32_t>(i);
                }
                for (const std::size_t i : sat) {
                    auto& g = c.hud_glow[i];
                    int best = -1, best_sum = 1 << 30;
                    if (c.window_light.on) {
                        // (With the windows' light the glow is added, so its
                        // colour counts in full: of the colours seen whole,
                        // the nearest on the channels that did not run into
                        // white, not under the floor on those that did.)
                        const std::size_t k = static_cast<std::size_t>(g.y) * fw + g.x;
                        const std::uint8_t* f = fb + k * 3;
                        const int t[3] = {g.r, g.g, g.b};
                        for (std::size_t n = 0; n < known.size(); ++n) {
                            const int kc[3] = {static_cast<int>(known[n] >> 16), static_cast<int>((known[n] >> 8) & 255), static_cast<int>(known[n] & 255)};
                            int dist = 0;
                            bool under = false;
                            for (int ch = 0; ch < 3; ++ch) {
                                if (f[ch] >= 248) under = under || kc[ch] + 8 < t[ch];
                                else dist += std::abs(kc[ch] - t[ch]) * 4;
                            }
                            if (under) continue;
                            dist += kc[0] + kc[1] + kc[2] > 0 ? 0 : 1;
                            const int key = dist * 1024 + kc[0] + kc[1] + kc[2];
                            if (key < best_sum) { best_sum = key; best = static_cast<int>(n); }
                        }
                        int est[3] = {g.r, g.g, g.b};
                        if (best >= 0) {
                            est[0] = static_cast<int>(known[best] >> 16); est[1] = static_cast<int>((known[best] >> 8) & 255);
                            est[2] = static_cast<int>(known[best] & 255);
                        }
                        // And not under the glow beside it (left, above: done
                        // before it): inside a glow that runs into white all
                        // over, the floor says little.
                        for (const std::size_t nb : {g.x > 0 ? k - 1 : k, g.y > 0 ? k - fw : k}) {
                            if (nb == k || glow_at[nb] < 0) continue;
                            const auto& o = c.hud_glow[static_cast<std::size_t>(glow_at[nb])];
                            est[0] = std::max<int>(est[0], o.r); est[1] = std::max<int>(est[1], o.g); est[2] = std::max<int>(est[2], o.b);
                        }
                        if (f[0] >= 248) g.r = static_cast<std::uint8_t>(std::max<int>(g.r, est[0]));
                        if (f[1] >= 248) g.g = static_cast<std::uint8_t>(std::max<int>(g.g, est[1]));
                        if (f[2] >= 248) g.b = static_cast<std::uint8_t>(std::max<int>(g.b, est[2]));
                        continue;
                    }
                    for (std::size_t n = 0; n < known.size(); ++n) {
                        const int r = known[n] >> 16, gg = (known[n] >> 8) & 255, bb = known[n] & 255;
                        if (r + 8 < g.r || gg + 8 < g.g || bb + 8 < g.b) continue;
                        if (r + gg + bb < best_sum) { best_sum = r + gg + bb; best = static_cast<int>(n); }
                    }
                    if (best >= 0) {
                        g.r = static_cast<std::uint8_t>(known[best] >> 16);
                        g.g = static_cast<std::uint8_t>(known[best] >> 8);
                        g.b = static_cast<std::uint8_t>(known[best]);
                    }
                }
                // In a battle action, a sprite over the light layer (Apocalypse's
                // rings over its pillar, states/thor/0238 ~395): the layer's light
                // under it too, from the nearest light pixel up or down its
                // column (16 rows). Left out, the pillar was cut black where
                // each ring crossed it (the scene has no light layer).
                if (c.hud_glow_add) {
                    static thread_local std::vector<std::int32_t> at;
                    at.assign(static_cast<std::size_t>(fw) * fh, -1);
                    const std::size_t n0 = c.hud_glow.size();
                    for (std::size_t i = 0; i < n0; ++i)
                        at[static_cast<std::size_t>(c.hud_glow[i].y) * fw + c.hud_glow[i].x] = static_cast<std::int32_t>(i);
                    for (int y = 0; y < fh; ++y)
                        for (int x = 0; x < fw; ++x) {
                            const std::size_t k = static_cast<std::size_t>(y) * fw + x;
                            const unsigned under = lay[k] >> 4;
                            if ((lay[k] & 0x0Fu) != kol::kLayerObj || (under != 2u && under != 3u) || !((first >> under) & 1u)) continue;
                            std::int32_t src = -1;
                            for (int d = 1; d <= 16 && src < 0; ++d) {
                                if (y - d >= 0 && at[k - static_cast<std::size_t>(d) * fw] >= 0) src = at[k - static_cast<std::size_t>(d) * fw];
                                else if (y + d < fh && at[k + static_cast<std::size_t>(d) * fw] >= 0) src = at[k + static_cast<std::size_t>(d) * fw];
                            }
                            if (src < 0) continue;
                            const auto g = c.hud_glow[static_cast<std::size_t>(src)];
                            c.hud_glow.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), g.r, g.g, g.b});
                        }
                    // (Back in reading order: hud_glow_over joins runs along a row.)
                    std::stable_sort(c.hud_glow.begin(), c.hud_glow.end(), [](const auto& p, const auto& q) {
                        return p.y != q.y ? p.y < q.y : p.x < q.x;
                    });
                }
                // The point the light fans out from: beams from one point are a
                // star about it (the way from the point to any lit pixel is lit),
                // so the point that sees the most of a sample of the lit pixels
                // through lit ones (tested a quarter and half the way back from
                // each), searched every 4 px, then every 1. Taken when it sees
                // 85% of them; kept from the last picture when within 3 px (the
                // carried-on part must not shake). KOL_NO_HUD_GLOW_REACH=1: none.
                static const bool no_reach = kol::env_set("KOL_NO_HUD_GLOW_REACH");
                const std::size_t n = c.hud_glow.size();
                // Not in a battle's action: a spell's light is no beams (Shaher's
                // Cataclysm, map 57, states/thor/0233: its whirlwind on BG2 passed
                // as a star about a point inside it, and its pixels carried on,
                // scaled up to 4, stood as a blocky band over him and a patch on
                // the floor among the units). KOL_HUD_GLOW_REACH_IN_ACTION=1: as
                // before.
                static const bool reach_in_action = kol::env_set("KOL_HUD_GLOW_REACH_IN_ACTION");
                const bool in_action = c.spell_running || c.spell_camera || c.combat_camera;
                if (!no_reach && n >= 200 && !c.window_light.on && (reach_in_action || !in_action)) {
                    static thread_local std::vector<std::uint8_t> mask;
                    mask.assign(static_cast<std::size_t>(fw) * fh, 0);
                    for (const auto& g : c.hud_glow) mask[static_cast<std::size_t>(g.y) * fw + g.x] = 1;
                    const std::size_t step = std::max<std::size_t>(1, n / 256);
                    int samples = 0;
                    for (std::size_t i = 0; i < n; i += step) ++samples;
                    auto score = [&](int ax, int ay) {
                        int s = 0;
                        for (std::size_t i = 0; i < n; i += step) {
                            const auto& g = c.hud_glow[i];
                            s += mask[static_cast<std::size_t>((g.y + ay) / 2) * fw + (g.x + ax) / 2];
                            s += mask[static_cast<std::size_t>((3 * g.y + ay) / 4) * fw + (3 * g.x + ax) / 4];
                        }
                        return s;
                    };
                    int bx = 0, by = 0, bs = -1;
                    for (int ay = 0; ay < fh; ay += 4)
                        for (int ax = 0; ax < fw; ax += 4) {
                            const int s = score(ax, ay);
                            if (s > bs) { bs = s; bx = ax; by = ay; }
                        }
                    const int cx0 = bx, cy0 = by;
                    for (int ay = std::max(0, cy0 - 4); ay <= std::min(fh - 1, cy0 + 4); ++ay)
                        for (int ax = std::max(0, cx0 - 4); ax <= std::min(fw - 1, cx0 + 4); ++ax) {
                            const int s = score(ax, ay);
                            if (s > bs) { bs = s; bx = ax; by = ay; }
                        }
                    static float last[2] = {-100.0f, -100.0f};
                    static const bool glow_log = kol::env_set("KOL_HUD_GLOW_LOG");
                    if (glow_log)
                        std::fprintf(stderr, "hud_glow: n %zu apex %d,%d score %d%% (last %.0f,%.0f)\n", n, bx, by,
                                     samples ? bs * 50 / samples : 0, last[0], last[1]);
                    // Once taken, the same apex (within 3 px) is kept down to
                    // 60%: Shaher's death rays (map 57, states/
                    // final_battle_won_route_b.state ~470-670, apex 192,100 at
                    // 384x216) animate outward and score 69-88% in a cycle, so
                    // at 85% they were carried to the view's edges on a third
                    // of the pictures and boxed to 240x160 on the rest (the
                    // user, 2026-11-12, wanted the rays to fill the screen as
                    // they did on some frames). A new apex still
                    // needs 85%. KOL_HUD_GLOW_NO_HOLD=1: as before.
                    static const bool no_hold = kol::env_set("KOL_HUD_GLOW_NO_HOLD");
                    const bool same = std::abs(bx - last[0]) <= 3.0f && std::abs(by - last[1]) <= 3.0f;
                    const bool hold = !no_hold && glow_held && same && bs * 100 >= samples * 2 * 60;
                    if (bs * 100 >= samples * 2 * 85 || hold) {
                        if (!same) { last[0] = static_cast<float>(bx); last[1] = static_cast<float>(by); }
                        c.hud_glow_apex_ok = true;
                        c.hud_glow_apex[0] = last[0];
                        c.hud_glow_apex[1] = last[1];
                    }
                    g_glow_held = c.hud_glow_apex_ok;
                }
            }
        }
    }
    if (c.window_light.on) c.hud_glow_layers |= 1u << c.ui_field;   // (not HUD: hud_pixel)
    c.sun_arc = g_sun_arc;
    c.sun_turn = g_sun_turn;
    for (int b = 0; b < 2; ++b) {
        int rows = 0;
        const std::uint64_t* lit = kol::highlight_world_lit(b, &rows);
        int* m = c.range_map[b];
        c.range_ok[b] = lit && rows > 0 && kol::map_source_cell_mapping(b, &m[0], &m[1], &m[2], &m[3]);
        if (c.range_ok[b]) {
            c.range_lit[b].assign(lit, lit + static_cast<std::size_t>(rows) * 128u);
            c.range_rows[b] = rows;
        }
    }
    // A new map, or a state loaded (the frame count gone back, or on by
    // more than 600): what eases over the frames (the weather look, a
    // scene's sunset and its storm, below) takes it as it is. `steps`: the
    // frames since the last capture.
    static int seen_map = -1;
    static std::uint64_t seen_frame = 0;
    const std::uint64_t fr = ppu->frame_count();
    const int map_now = rd16(ew, kMapNumber - 0x02000000u);
    const bool fresh = map_now != seen_map || fr < seen_frame || fr > seen_frame + 600;
    const std::uint64_t steps = fr - seen_frame;
    seen_map = map_now;
    seen_frame = fr;
    {
        constexpr std::uint32_t kWeatherParticles = 0x0202A0F8u;
        constexpr int kWeatherHold = 240;
        bool live = false;
        for (unsigned i = 0; i < 10u && !live; ++i)
            live = ew[kWeatherParticles - 0x02000000u + i * 0x14u + 4u] != 0;
        static const bool any_sky = std::getenv("KOL_WEATHER_ANY_SKY") != nullptr;
        {
            constexpr std::uint32_t kPaletteBuffer = 0x02002400u;   // the game's BG palette, entry 0
            const unsigned v = ew[kPaletteBuffer - 0x02000000u] | (ew[kPaletteBuffer - 0x02000000u + 1u] << 8);
            const int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            if (r + g + b >= 6)
                g_overcast = std::max({r, g, b}) - std::min({r, g, b}) <= 4;
        }
        live = live && (any_sky || g_overcast);
        c.indoor = g_indoor_live = map_indoor();
        live = live && !c.indoor;   // no weather look indoors either
        g_weather_hold = live ? kWeatherHold : std::max(0, g_weather_hold - 1);
        c.weather = g_weather_hold > 0;
        // Eased over kWeatherEase frames (the user, 2026-10-26, reported rain
        // and overcast arriving instantly); a new map or a state
        // loaded takes it as it is.
        constexpr float kWeatherEase = 150.0f;
        const float w_target = c.weather ? 1.0f : 0.0f;
        if (fresh) g_weather_amt = w_target;
        else g_weather_amt += std::clamp(w_target - g_weather_amt,
                                         -static_cast<float>(steps) / kWeatherEase,
                                         static_cast<float>(steps) / kWeatherEase);
        c.weather_amt = g_weather_amt;
    }
    c.extent_ok = kol::map_source_extent(&c.extent);
    // A scene's sunset (scene_sunset): frames since the map came, held set
    // once its heavy rain has; a jump in the frame count (a state loaded)
    // starts it again.
    c.rain = rain_of(c.ew.data());
    c.rain_level = rain_level_of(c.ew.data());
    c.snow_level = snow_level_of(c.ew.data());
    c.story = c.ew[0x020013D0u - 0x02000000u] == 1u;
    c.map = rd16(c.ew.data(), kMapNumber - 0x02000000u);
    c.map_loads = kol::map_loads();
    {
        static std::uint64_t since = 0;
        static bool stormed = false;
        if (fresh) {   // (a new map, or a state loaded)
            since = fr;
            stormed = false;
        }
        static const int frames_env = kol::env_int("KOL_SUNSET_FRAMES", 0);
        g_scene_sunset_live = -1.0f;
        {
            // The weather eased (scene_storm): toward the rain's strength,
            // or 1 under the grey sky, by 1 / grey_frames a frame.
            static float storm = 0.0f;
            const SceneSunset* s = scene_sunset();
            const float target = std::max(c.rain, c.weather ? 1.0f : 0.0f);
            if (!s || fresh || sunset_storm_step()) storm = sunset_storm_step() ? c.rain : target;   // (a new map or a load: as it is)
            else storm += std::clamp(target - storm, -1.0f / s->grey_frames, 1.0f / s->grey_frames);
            g_scene_storm_live = storm;
            c.scene_storm = storm;
        }
        c.palette_lag = false;
        if (const SceneSunset* s = scene_sunset()) {
            stormed = stormed || c.rain >= 1.0f;
            // Not while the actor it waits for is on the map.
            const std::uint32_t list = rd32(c.ew.data(), kObjectsPtr - 0x02000000u);
            if (s->wait_for >= 0 && in_ewram(list, kObjects * kObjStride))
                for (int i = 0; i < kObjects; ++i) {
                    const std::uint32_t rec = rd32(c.ew.data(), list - 0x02000000u + static_cast<std::uint32_t>(i) * kObjStride);
                    if (in_ewram(rec, 0x68) && c.ew[rec - 0x02000000u + 0x4E] == s->wait_for) since = fr;
                }
            const int n = std::max(1, frames_env > 0 ? frames_env : s->frames);
            g_scene_sunset_live = stormed ? 1.0f : std::min(1.0f, static_cast<float>(fr - since) / static_cast<float>(n));
        }
        c.scene_sunset = g_scene_sunset_live;
        // The palette lag (ScenePixels::lag_pal): from the sunset's start
        // (after the actor it waits for) until 600 frames after the sun has
        // set (map 102: 10 s past the heavy rain, before the thunder; the
        // later fades, to white and to black, as the game draws them). Not
        // from the map's first frame: the shown palette then started from
        // the one the game had while still loading the map and crept from it
        // to the map's own, wrong colours for seconds after the fade in from
        // map 101 (JK, the user's slot 2: the transition looked wrong).
        static std::uint64_t set_at = 0;
        if (fresh || g_scene_sunset_live < 1.0f) set_at = fr;
        c.palette_lag = g_scene_sunset_live > 0.0f && !sunset_storm_step() && fr < set_at + 600;
    }
    c.bg1_chars.clear();
    if (c.extent_ok && bg1_prop_map(c.map))
        if (const std::uint8_t* io = ppu->latched_io()) {
            const unsigned bg1cnt = io[0x0A] | (io[0x0B] << 8);
            const std::uint8_t* vram = bus->vram_ptr();
            const std::size_t base = ((bg1cnt >> 2) & 3u) * 0x4000u;
            if (vram && !(bg1cnt & 0x80u)) c.bg1_chars.assign(vram + base, vram + std::min<std::size_t>(base + 0x8000u, 0x10000u));
        }
    c.rom = bus->rom_ptr();
    c.rom_size = bus->rom_size();
    perspective_capture();
    c.valid = true;
    // KOL_PERSP_HASH=1 (kol_perspective): the capture's hash too, so a
    // present that differs between two runs can be traced to its inputs.
    if (kol::persp_hash_trace()) {
        std::uint32_t h = kol::kFnvBasis;
        for (int v : {c.hofs, c.vofs, static_cast<int>(c.bg0_h), static_cast<int>(c.bg0_v), c.view[0], c.view[1], c.view[2], c.view[3], c.scene_w, c.scene_h})
            kol::fnv_mix(h, static_cast<std::uint32_t>(v + 65536));
        std::fprintf(stderr, "kol-capture serial=%lu ew=%08x iw=%08x oam=%08x tiles=%08x under=%08x codes=%08x scalars=%08x strike=%.3f look=%.3f,%.3f\n",
                     c.serial, kol::fnv_bytes(c.ew), kol::fnv_bytes(c.iw), kol::fnv_bytes(c.oam), kol::fnv_bytes(c.obj_tiles),
                     kol::fnv_bytes(c.under), kol::fnv_bytes(c.codes), h,
                     g_cam_cap_strike(), g_cam_cap_look(0), g_cam_cap_look(1));
    }
}

void focus_note_scroll(unsigned bg0_hofs, unsigned bg0_vofs) {
    g_hofs = static_cast<int>(bg0_hofs & 0x1FFu);
    g_vofs = static_cast<int>(bg0_vofs & 0x1FFu);
    g_scroll_valid = true;
    ++g_frame_serial;
}
}  // namespace kol
