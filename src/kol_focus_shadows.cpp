// kol_focus_shadows.cpp — the sun and the shadows: the sun's height and
// direction (the map's own, the turns' drop, the debug arc), the terrain's
// shadow map and the awnings' canopy, the shadows units and props cast
// (from the sun, the windows' cones and the lamps), the units' contact
// shade and their light from the lamps.

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
// Shadows cast by a sun at the screen's top right (the light the tiles are
// painted with): the direction to it in the world is (-1, -3) along (X, Z)
// (screen right is X - Z, screen down X + Z), its height an angle above the
// ground, from Options > Shadows (High Sun 45, Low and Side Sun 30, Low Side
// Sun 20 degrees).
// KOL_PERSP_SHADOW=<strength 0..1> and KOL_PERSP_SHADOW_ELEV=<degrees>
// override it for mockups. The strength is the user's pick, 0.5, from
// shots/shadow_strength (2026-10-10; it was 0.35): the terrain's, units'
// own and the shade over units alike.
constexpr float kShadowStrength = 0.5f;
// Since 2026-10-12 that is the strength at 36 degrees (where it was picked):
// a higher sun casts darker shadows and a low one fainter (asked for darker
// shadows under a high sun and less contrast under a low one), as the sun on the ground goes with the sine of its height
// while the sky's light in the shade does not: 0.5 * sqrt(sin h / sin 36),
// 0.15..0.65 (60: 0.61, 36: 0.5, 12.5: 0.30, 5: 0.19). And rain or snow
// falling cuts it to kWeatherShadow (asked for much less shadow contrast in
// bad weather and snow). KOL_SHADOW_BY_HEIGHT=0
// keeps the 0.5; KOL_WEATHER=0/1 stands in for the weather.
constexpr float kWeatherShadow = 0.35f;
}  // namespace

namespace kol_focus_detail {
float shadow_strength() {
    static const char* e = std::getenv("KOL_PERSP_SHADOW");
    if (e) return std::max(0.0f, std::min(1.0f, static_cast<float>(std::atof(e))));
    if (kol::shadow_sun_degrees() <= 0.0f || indoor_now()) return 0.0f;
    static const bool by_height = kol::env_not_zero("KOL_SHADOW_BY_HEIGHT");
    float s = kShadowStrength;
    if (by_height) {
        constexpr float kRad = 3.14159265f / 180.0f;
        const float r = std::sin(sun_degrees() * kRad) / std::sin(36.0f * kRad);
        s = std::max(0.15f, std::min(0.65f, kShadowStrength * std::sqrt(std::max(0.0f, r))));
        // Below 36 degrees the curve is lifted to reach 0.28 at the lowest
        // sun (5 degrees), not 0.19 (2026-10-14, asked for more visible
        // shadows at the lowest sun, the sine curve not dropping as low; 0.28 the user's pick from 0.28 / 0.35,
        // shots/low_sun_strength): 10 degrees 0.34, 20 0.42, 30 0.47, as
        // before from 36 up. KOL_SHADOW_LOWEST=<strength> for others, a
        // negative one the curve as before.
        static const float lowest = kol::env_num("KOL_SHADOW_LOWEST", 0.28f);
        if (lowest >= 0.0f && r < 1.0f) {
            const float q5 = std::sqrt(std::sin(5.0f * kRad) / std::sin(36.0f * kRad));
            const float q = std::sqrt(std::max(0.0f, r));
            s = lowest + (kShadowStrength - lowest) * std::max(0.0f, (q - q5) / (1.0f - q5));
        }
    }
    if (scene_shadows_keep()) return s;   // (a scene's sunset, SceneSunset::shadow_weather)
    if (circling_sun(nullptr)) return s;   // (the rising rain's own: kept whole)
    return s * (1.0f - (1.0f - kWeatherShadow) * weather_amount());
}
}  // namespace kol_focus_detail

namespace {
const MapSun* map_sun() {
    const int map = current_map();
    if (map < 0) return nullptr;
    // One set in the editor (the SUN tab) first.
    static thread_local MapSun edited{};
    if (kol::TileSun s{}; kol::tile_edit_sun(map, &s)) {
        edited = {map, s.degrees, s.x, s.z};
        return &edited;
    }
    if (const EveningRoom* r = evening_room()) {   // (its evening's, kEveningRooms)
        static thread_local MapSun ev{};
        ev = {map, r->degrees, r->x, r->z};
        return &ev;
    }
    return by_map(kMapSuns, map);
}
// The sun sinks as a battle goes on (2026-10-11, asked for the sun to drop
// about 1 degree per turn; then, 10-12, for a higher start on turn 1 and a
// faster drop, 2 or 3 degrees per turn): 2.5 degrees a turn after the first, from 60
// (kol_settings kShadowSunDegrees) to 12.5 at turn 20, never below the
// arc's lowest (turn 23). The turn is the game's,
// the byte at 0x020021CC: 1 in battle preparation and the first turn, +1
// when the enemy phase ends (BL_enemy_turn 1 -> 2 at its frame 3920; CN 3,
// CO 5 in the same battle). KOL_SUN_DROP=<degrees a turn> overrides the 1
// (0 off); KOL_SUN_TURN=<n> stands in for the game's turn.
// (The 1 is 2.5 since 2026-10-12.)
constexpr std::uint32_t kBattleTurn = 0x020021CCu;
}  // namespace

namespace kol_focus_detail {
int battle_turn() {   // 1.., the game's turn (1 when unknown)
    static const char* forced = std::getenv("KOL_SUN_TURN");
    int turn = 1;
    if (forced) turn = std::atoi(forced);
    else if (const std::uint8_t* ew = guest_ew()) turn = rd16(ew, kBattleTurn - 0x02000000u);
    return std::max(1, std::min(turn, 999));
}
float sun_turn_drop() {
    static const float per = kol::env_num_min("KOL_SUN_DROP", 0.0f, 2.5f);
    return per * static_cast<float>(battle_turn() - 1);
}
// The sun's height before the debug arc: the map's own or the default, less
// the turns' drop. An explicit KOL_PERSP_SHADOW_ELEV is taken as it is.
float sun_degrees_base() {
    static const char* e = std::getenv("KOL_PERSP_SHADOW_ELEV");
    if (e) return static_cast<float>(std::atof(e));
    if (kol::shadow_sun_degrees() <= 0.0f) return 45.0f;
    // Circling where the rain rises (circling_sun): 40 degrees up (20 first; the
    // user, 2026-11-12, asked for a smaller angle, about 40 degrees).
    static const float circle_deg = kol::env_num("KOL_CIRCLE_SUN_DEG", 40.0f);
    if (circling_sun(nullptr)) return circle_deg;
    const MapSun* m = map_sun();
    // A room lit through cone windows (map 62) keeps its own sun: sunk by
    // the turns of the battle before it, the light came in low and the
    // cone's beams ran across the whole room (seconds to build).
    // So does a torch out of view (window_torch, map 67): no turns sink it.
    // And a map set so (map_settings.cfg `fixed_sun`).
    if (m && (map_cone_windows() || window_torch() || kol::map_flag(kol::MapFlag::FixedSun, current_map()))) return m->degrees;
    float d = m ? m->degrees : kol::shadow_sun_degrees();
    if (const SceneSunset* s = scene_sunset())
        if (const float p = scene_sunset_now(); p > 0.0f) {
            // Stepped 10 times a second (a 6th of the way's frames): each
            // move of the sun rebuilds the terrain's shadows, every frame
            // the sunset cost the worker most of its time.
            float n = std::floor(p * s->frames / 6.0f);
            if (t_sun_ahead_steps) n += static_cast<float>(t_sun_ahead_steps);
            const float q = std::min(1.0f, n * 6.0f / s->frames);
            d += (std::min(d, s->to_degrees) - d) * q;
        }
    return std::max(std::min(d, kArcLowest), d - sun_turn_drop());
}
float sun_arc_angle() {
    return std::max(kArcLowest, std::min(kArcHighest, sun_degrees_base() + sun_arc_now()));
}
float sun_degrees() {   // 5..85 (the arc: up to 89, about overhead)
    const float a = sun_arc_angle();
    float d = std::min(a, 180.0f - a);
    if (const WindowTorch* w = window_torch()) {
        float sa, sb, sf;
        window_torch_sway(*w, sa, sb, sf);
        d += w->pitch * sb;
    }
    return sun_arc_now() == 0.0f ? std::min(85.0f, d) : std::min(89.0f, d);
}
float shadow_rise() {   // world units the sun's ray climbs per unit across
    return std::tan(sun_degrees() * 3.14159265f / 180.0f);
}
// The direction toward the sun along the ground, (X, Z), unit length:
// (0.585, -0.811), right of the battlefield and toward its far side, unless
// the map has its own sun (kMapSuns, the editor's `sun` lines). It is the
// sun the user had on the Thor on map 9 (2026-10-11, 36 degrees up,
// kol_settings kShadowSunDegrees), made the default when the menu's five
// angle picks went. Those were the screen's top right (-1, -3) at 45 or 30
// degrees, and (2, -1), the side the game's art lights (its walls facing
// down-right are the bright ones), at 30 or 20.
// (1, -2) read as from behind on the Thor, and (1, -1), straight from the
// screen's right, still as "deeper". KOL_PERSP_SHADOW_DIR=<x,z> overrides it
// for mockups.
void shadow_dir_pick(float& lx, float& lz) {
    static const char* e = std::getenv("KOL_PERSP_SHADOW_DIR");
    float v[2] = {0.585f, -0.811f};
    const MapSun* m = e || kol::shadow_sun_degrees() <= 0.0f ? nullptr : map_sun();
    if (e) std::sscanf(e, "%f,%f", &v[0], &v[1]);
    else if (m) { v[0] = m->x; v[1] = m->z; }
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    lx = n > 0.0f ? v[0] / n : 0.0f;
    lz = n > 0.0f ? v[1] / n : -1.0f;
    // Turned in the (X, Z) plane: +X (screen down-right) toward +Z (down-
    // left) is clockwise on screen. A torch out of view sways (window_torch).
    float turn = sun_turn_now();
    if (float c = 0.0f; !e && circling_sun(&c)) turn += c;   // (round and round, circling_sun)
    if (const WindowTorch* w = window_torch()) {
        float sa, sb, sf;
        window_torch_sway(*w, sa, sb, sf);
        turn += w->yaw * sa;
    }
    if (const float t = turn; t != 0.0f) {
        const float a = t * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        const float x = lx * c - lz * s, z = lx * s + lz * c;
        lx = x;
        lz = z;
    }
}
void shadow_dir(float& lx, float& lz) {
    shadow_dir_pick(lx, lz);
    if (sun_arc_angle() > 90.0f) { lx = -lx; lz = -lz; }   // over the top
}
// A key on the sun: `key` with its rise and way along the ground folded in
// (the shadow map's, and indoors the window beams', dark_windows).
std::uint32_t sun_key(std::uint32_t key, float rise, float lx, float lz) {
    kol::fnv_mix(key, static_cast<std::uint32_t>(rise * 1000.0f));
    kol::fnv_mix(key, static_cast<std::uint32_t>(lx * 1000.0f + 5000.0f));
    kol::fnv_mix(key, static_cast<std::uint32_t>(lz * 1000.0f + 5000.0f));
    return key;
}
float terrain_soft() {
    static const float v = kol::env_num("KOL_TERRAIN_SOFT", 0.11f);
    return v;
}
// How far under an awning's edge its cloth hangs (world units): pixels whose
// ray comes in that close under the edge are the awning's, upright.
float awning_skirt() {   // KOL_AWNING_SKIRT=<units>, for fitting
    static const float v = kol::env_num("KOL_AWNING_SKIRT", 4.0f);
    return v;
}
// An awning's cloth reaches past the cells its posts stand at, by this much
// on every side (world units; map 89's kiosk, fitted to its art).
float awning_overhang() {
    static const float v = kol::env_num("KOL_AWNING_OVERHANG", 4.0f);
    return v;
}
// The share of an awning's rise above which a pixel on its slope is taken
// to be its cloth, for the colours that tell the cloth from other art.
float awning_sure() {   // KOL_AWNING_SURE=<0..1>, for fitting
    static const float v = kol::env_num("KOL_AWNING_SURE", 0.15f);
    return v;
}

// The terrain's shadow over a prop standing as a card at world (X, Z), its
// foot at height `foot` on scene row foot_row (a row up is 2 units up), as
// over a unit (build_sprite_atlas): the rows below the one returned are
// under the shade's height there. 1e9: none. An awning's shadow too.
float card_shade_row(int X, int Z, int foot, float foot_row) {
    int top = shade_height(X, Z);
    int c_lo = 0, c_hi = 0;
    if (canopy_range(X, Z, &c_lo, &c_hi) && foot >= c_lo && foot < c_hi) top = std::max(top, c_hi);
    if (top == kNoShade || top <= foot) return 1.0e9f;
    return foot_row - 0.5f * static_cast<float>(top - foot);
}
// A prop shades its own foot, darkest where it meets the ground and easing
// out over its lowest rows (the user, 2026-10-14, asked for props darker at
// the bottom, as if shadowing themselves), then
// a few rows higher than where it touches the ground (the user asked for it
// a bit higher). With Ambient Occl. on: base_dark at the default strength (0.5),
// in proportion to it. KOL_PROP_BASE_ROWS / KOL_PROP_BASE_DARK /
// KOL_PROP_BASE_RAISE for trying values; rows 0 turns it off.
void prop_base_params(float* rows, float* dark, float* raise) {
    static const float up = kol::env_num("KOL_PROP_BASE_RAISE", 4.0f);
    *raise = up;
    static const float r = kol::env_num("KOL_PROP_BASE_ROWS", 10.0f);
    static const float d = kol::env_num("KOL_PROP_BASE_DARK", 0.5f);
    const float ao_s = ao_strength();
    *rows = ao_s > 0.0f && !kol::map_flag(kol::MapFlag::NoPropFoot, current_map()) ? r : 0.0f;
    *dark = std::min(0.9f, d * ao_s / 0.5f);
}
}  // namespace kol_focus_detail

namespace kol {
float card_shade_soft() {
    static const float v = [] {
        const char* e = std::getenv("KOL_CARD_SHADE_SOFT");
        return e ? static_cast<float>(std::atof(e)) : 4.0f;   // the user's pick, 2026-10-14
    }();
    return v;
}
}  // namespace kol

namespace kol_focus_detail {
// Props take it (KOL_NO_PROP_SHADE=1: their pixels keep the full light).
bool props_shaded() {
    static const bool off = std::getenv("KOL_NO_PROP_SHADE") != nullptr;
    return !off && g_shadow_on && shadow_strength() > 0.0f;
}
}  // namespace kol_focus_detail

namespace {
// A point `y` high over world (X, Z) out of the sun: in the terrain's shadow
// (shade_height) or an awning's (canopy_shaded), the place found once.
bool sun_blocked(int X, int Z, float y) {
    const std::ptrdiff_t at = shadow_place(X, Z);
    if (at < 0) return false;
    const std::size_t i = static_cast<std::size_t>(at), n = static_cast<std::size_t>(g_shadow_w) * g_shadow_h;
    if (g_shadow.size() == n) {
        const int shade = g_shadow[i];
        if (shade != kNoShade && y + 0.5f < shade) return true;
    }
    return g_canopy_hi.size() == n && g_canopy_hi[i] != kNoShade && y >= static_cast<float>(g_canopy_lo[i]) &&
           y + 0.5f < static_cast<float>(g_canopy_hi[i]);
}
// Each unit's shadow from each lamp (cast_lamp_shadows), kept while nothing
// it depends on changes (the unit's place and sprite, the lamp, the view,
// the heights), as the sun's are (CastCache): its pixels at full strength;
// the lamp's strength on the unit, with its flicker, scales them each frame.
struct LampCast { std::uint32_t key; float lit; std::vector<std::uint32_t> px; };
std::vector<LampCast> g_lamp_cast_cache, g_lamp_cast_next;
std::vector<std::uint8_t> g_cast_mark;   // pixels already in a unit's shadow
// Each unit's shadow, as the pixels it covers (not those in the terrain's
// shadow already), kept while nothing it depends on changes: the unit's
// place and sprite frame, the view, the sun and the heights.
struct CastCache {
    std::uint32_t key;
    std::vector<std::uint32_t> px;
    // A prop's (cast_prop_shadows): the camera (kx + ox, ky + oy) its px are
    // placed for, and the scene it was worked out over, in the camera's
    // place (scene - camera): kept as the camera pans.
    int camx = 0, camy = 0, wx0 = 0, wy0 = 0, wx1 = -1, wy1 = -1;
    // A prop's: its list already handed to this frame's (a still camera
    // swaps it out), so an empty list is still an answer.
    bool taken = false;
};
std::vector<CastCache> g_cast_cache, g_cast_next;
}  // namespace

namespace kol_focus_detail {
// Options > Unit Shadows: units cast shadows (and lose the game's dark blob
// under them) with the terrain's shadows on, and sit in occlusion with
// Ambient Occl. on.
bool unit_shadows_on() { return kol::setting_on(kol::Setting::UnitShadows); }
bool units_cast() { return unit_shadows_on() && g_shadow_on && shadow_strength() > 0.0f; }
namespace {
std::vector<PropGroup> scene_props_make(int w, int h, std::vector<int>& prop_of);
// Counts the times scene_props gave a new answer on this thread: while it
// stands, each call's groups and prop_of are the last call's (cast_prop_shadows
// keeps its keys by it). 0: not kept (KOL_SCENE_PROPS_KEEP=0, the check).
thread_local std::uint64_t t_props_gen = 0;
thread_local bool t_props_gen_ok = false;
}
// The props' groups, kept while what they are made from stands (2026-11-20,
// Thor state 0311: map 102's narration asked twice a picture, the foot spots
// and the shadows, and each time every pixel was grouped again through a
// hash map, 0.7 ms of the worker's job on the Mac, on a scene that had not
// changed). What the grouping reads is listed exactly: each pixel a prop
// reached (g_prop_dist) with its place, kind, flame, cell and shape, and the
// props' pixels past the scene; the same list gives the same groups, copied
// out. KOL_SCENE_PROPS_KEEP=0: grouped every time. KOL_SCENE_PROPS_CHECK=1:
// grouped every time and compared with the kept.
std::vector<PropGroup> scene_props(int w, int h, std::vector<int>& prop_of) {
    static const bool keep_on = kol::env_not_zero("KOL_SCENE_PROPS_KEEP");
    static const bool check = kol::env_set("KOL_SCENE_PROPS_CHECK");
    t_props_gen_ok = keep_on && !check;
    if (!keep_on) return scene_props_make(w, h, prop_of);
    struct Id {
        std::uint32_t q, kind;
        std::int32_t xz, y, cell, shape;
        bool operator==(const Id&) const = default;
    };
    struct Kept {
        bool valid = false;
        int w = 0, h = 0;
        bool has_cell = false, has_shape = false;
        std::vector<Id> id;
        std::vector<PropOutPx> out;
        std::vector<PropGroup> props;
        std::vector<int> prop_of;
    };
    static thread_local Kept kept;
    static thread_local std::vector<Id> id;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const bool has_cell = g_prop_cell.size() == n, has_shape = g_bg1_shape.size() == n;
    id.clear();
    for (std::size_t q = 0; q < n; ++q) {
        if (g_prop_dist[q] == kUnreached) continue;
        id.push_back({static_cast<std::uint32_t>(q), static_cast<std::uint32_t>(g_kind[q]) | (is_flame(q) ? 256u : 0u),
                      g_xz[q], g_y[q], has_cell ? g_prop_cell[q] : 0, has_shape ? g_bg1_shape[q] : 0});
    }
    const bool same = kept.valid && kept.w == w && kept.h == h && kept.has_cell == has_cell &&
                      kept.has_shape == has_shape && kept.id == id && kept.out == g_prop_out;
    static thread_local long asked = 0, taken = 0, bad = 0;
    ++asked;
    if (same && !check) {
        ++taken;
        prop_of = kept.prop_of;
        return kept.props;
    }
    std::vector<PropGroup> props = scene_props_make(w, h, prop_of);
    if (same && check) {
        ++taken;
        bool ok = props.size() == kept.props.size() && prop_of == kept.prop_of;
        for (std::size_t i = 0; ok && i < props.size(); ++i) {
            const PropGroup& a = props[i];
            const PropGroup& b = kept.props[i];
            ok = a.xz == b.xz && a.y == b.y && a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1 &&
                 a.shape == b.shape && a.cell == b.cell && a.out == b.out;
        }
        if (!ok) ++bad;
    }
    if (check && asked % 600 == 0)
        std::fprintf(stderr, "kol-focus: scene props check: asked %ld, kept %ld, bad %ld\n", asked, taken, bad);
    ++t_props_gen;
    kept.valid = true;
    kept.w = w;
    kept.h = h;
    kept.has_cell = has_cell;
    kept.has_shape = has_shape;
    kept.id.swap(id);
    kept.out = g_prop_out;
    kept.props = props;
    kept.prop_of = prop_of;
    return props;
}
namespace {
std::vector<PropGroup> scene_props_make(int w, int h, std::vector<int>& prop_of) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<PropGroup> props;
    prop_of.assign(n, -1);
    auto is_prop = [&](std::size_t q) {   // (a flame casts no shadow, has no shaded foot)
        return g_prop_dist[q] != kUnreached && g_kind[q] == kol::kSceneUpright && !is_flame(q);
    };
    static const bool by_place = std::getenv("KOL_PROP_GROUP_BY_PLACE") != nullptr;
    const bool by_cell = !by_place && g_prop_cell.size() == n;
    static thread_local std::vector<std::size_t> stack;
    const bool by_shape = g_bg1_shape.size() == n && g_prop_cell.size() == n;
    if (by_shape) {
        // (shape, cell) -> group, found by hash: a linear search per pixel
        // was a third of the worker's time on map 102.
        std::unordered_map<std::uint64_t, int> group_of;
        auto key_of = [](int sh, int cell) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(sh)) << 32 | static_cast<std::uint32_t>(cell); };
        // A row's run of one shape and cell asks the map once.
        std::uint64_t last_key = ~0ull;
        int last_pi = -1;
        for (std::size_t q = 0; q < n; ++q) {
            const int sh = g_bg1_shape[q];
            if (sh < 0 || !is_prop(q)) continue;
            const int px = static_cast<int>(q % w), py = static_cast<int>(q / w);
            const std::uint64_t key = key_of(sh, g_prop_cell[q]);
            int pi = last_pi;
            if (key != last_key) {
                auto [it, fresh] = group_of.try_emplace(key, static_cast<int>(props.size()));
                pi = it->second;
                if (fresh) props.push_back({g_xz[q], g_y[q], px, py, px, py, sh, g_prop_cell[q], {}});
                last_key = key;
                last_pi = pi;
            }
            PropGroup& p = props[pi];
            p.x0 = std::min(p.x0, px); p.y0 = std::min(p.y0, py);
            p.x1 = std::max(p.x1, px); p.y1 = std::max(p.y1, py);
            prop_of[q] = pi;
        }
        for (const PropOutPx& o : g_prop_out) {
            const std::uint64_t key = key_of(o.shape, o.cell);
            if (key != last_key) {
                auto [it, fresh] = group_of.try_emplace(key, static_cast<int>(props.size()));
                if (fresh) props.push_back({o.xz, o.y, o.px, o.py, o.px, o.py, o.shape, o.cell, {}});   // wholly past the scene: its shadow only
                last_key = key;
                last_pi = it->second;
            }
            PropGroup* g = &props[static_cast<std::size_t>(last_pi)];
            g->x0 = std::min(g->x0, o.px); g->y0 = std::min(g->y0, o.py);
            g->x1 = std::max(g->x1, o.px); g->y1 = std::max(g->y1, o.py);
            g->out.emplace_back(o.px, o.py);
        }
    }
    for (std::size_t q0 = 0; q0 < n; ++q0) {
        if (prop_of[q0] >= 0 || !is_prop(q0)) continue;
        const int pi = static_cast<int>(props.size());
        const int px0 = static_cast<int>(q0 % w), py0 = static_cast<int>(q0 / w);
        props.push_back({g_xz[q0], g_y[q0], px0, py0, px0, py0});
        stack.assign(1, q0);
        prop_of[q0] = pi;
        while (!stack.empty()) {
            const std::size_t q = stack.back();
            stack.pop_back();
            const int px = static_cast<int>(q % w), py = static_cast<int>(q / w);
            PropGroup& p = props[pi];
            p.x0 = std::min(p.x0, px); p.y0 = std::min(p.y0, py);
            p.x1 = std::max(p.x1, px); p.y1 = std::max(p.y1, py);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = px + dx, ny = py + dy;
                    if ((dx | dy) == 0 || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::size_t m = static_cast<std::size_t>(ny) * w + nx;
                    if (prop_of[m] >= 0 || !is_prop(m) || g_xz[m] != g_xz[q0] || g_y[m] != g_y[q0]) continue;
                    if (by_cell && g_prop_cell[m] != g_prop_cell[q0]) continue;
                    prop_of[m] = pi;
                    stack.push_back(m);
                }
        }
    }
    return props;
}
}  // namespace
}  // namespace kol_focus_detail

namespace {
std::vector<CastCache> g_prop_cast_cache, g_prop_cast_next;
bool props_cast() {
    static const bool off = std::getenv("KOL_NO_PROP_SHADOWS") != nullptr;
    return !off && g_shadow_on && shadow_strength() > 0.0f;
}
// A card's solid texels as running sums (s[(y+1)*(w+1)+x+1]: the count
// over [0,x]x[0,y]), for a soft shadow edge's box average (card_shadow,
// cast_prop_shadows). solid(i, j): texel i, j's 0 or 1.
struct BoxSum {
    std::vector<int> s;
    int w = 0, h = 0;
    template <class Solid>
    void build(int bw, int bh, Solid solid) {
        w = bw;
        h = bh;
        s.assign(static_cast<std::size_t>(w + 1) * (h + 1), 0);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                s[static_cast<std::size_t>(j + 1) * (w + 1) + i + 1] =
                    solid(i, j) +
                    s[static_cast<std::size_t>(j) * (w + 1) + i + 1] +
                    s[static_cast<std::size_t>(j + 1) * (w + 1) + i] -
                    s[static_cast<std::size_t>(j) * (w + 1) + i];
    }
    int count(int tx0, int ty0, int tx1, int ty1) const {   // solid texels in a box
        tx0 = std::max(tx0, 0); ty0 = std::max(ty0, 0);
        tx1 = std::min(tx1, w - 1); ty1 = std::min(ty1, h - 1);
        if (tx0 > tx1 || ty0 > ty1) return 0;
        const int W = w + 1;
        return s[static_cast<std::size_t>(ty1 + 1) * W + tx1 + 1] - s[static_cast<std::size_t>(ty0) * W + tx1 + 1] -
               s[static_cast<std::size_t>(ty1 + 1) * W + tx0] + s[static_cast<std::size_t>(ty0) * W + tx0];
    }
};
// Where a card's shadow (card_shadow, cast_prop_shadows) can lie, on the
// ground: within `half` across the light (lx, lz) of the card's place (cx,
// cz), and out along it from the card as far as a ray from the card's top
// (`top`, its foot at `foot`) reaches the lowest ground there (heights of
// the cells under it; water and holes at 0), found in two steps. (With a
// soft edge, a third: wider by its blur at the far end, r_far(far_d).)
struct ShadowRegion {
    float cu[4], cv[4];   // the region's corners, as X + Z and X - Z
    float lo, hi;         // the ground's heights under it
    int y0, y1;           // its rows
};
template <class FarBlur>
ShadowRegion shadow_region(float cx, float cz, float foot, float top, float half, float lx, float lz, float rise,
                           int passes, FarBlur r_far, int h, int oy, int ky) {
    constexpr float kRoot2 = 1.41421356f;
    const float ax = -lz, az = lx;   // across the light
    ShadowRegion R;
    float near_d = 0.0f, far_d = (top - foot) / rise;
    R.lo = R.hi = foot;
    for (int pass = 0; pass < passes; ++pass) {
        if (pass == 2) {   // the soft edge's blur at the far end, once that is known
            const float rf = r_far(far_d);
            half += rf * kRoot2;
            far_d += 2.0f * rf / rise;
        }
        float wx0 = 1e9f, wx1 = -1e9f, wz0 = 1e9f, wz1 = -1e9f;
        for (int i = 0; i < 4; ++i) {
            const float sa = (i & 1) ? half : -half, sl = (i & 2) ? far_d : near_d;
            const float X = cx + ax * sa - lx * sl, Z = cz + az * sa - lz * sl;
            wx0 = std::min(wx0, X); wx1 = std::max(wx1, X);
            wz0 = std::min(wz0, Z); wz1 = std::max(wz1, Z);
            R.cu[i] = X + Z;
            R.cv[i] = X - Z;
        }
        const int last = kGameSide + kMargin - 1;
        for (int gz = std::max(-kMargin, static_cast<int>(std::floor(wz0 / 32.0f)));
             gz <= std::min(last, static_cast<int>(std::floor(wz1 / 32.0f))); ++gz)
            for (int gx = std::max(-kMargin, static_cast<int>(std::floor(wx0 / 32.0f)));
                 gx <= std::min(last, static_cast<int>(std::floor(wx1 / 32.0f))); ++gx) {
                const int hc = g_cast_heights[gi(gx, gz)];
                if (hc < 0 && !in_game(gx, gz)) continue;   // no cell there
                R.lo = std::min(R.lo, hc < 0 ? 0.0f : 16.0f * hc);
                R.hi = std::max(R.hi, hc < 0 ? 0.0f : 16.0f * hc);
            }
        if (pass < 2) {
            near_d = std::max(0.0f, (foot - R.hi) / rise);
            far_d = (top - R.lo) / rise;
        }
    }
    // Rows: a ground point's row is (X + Z) / 4 - Y / 2.
    float umin = 1e9f, umax = -1e9f;
    for (int i = 0; i < 4; ++i) { umin = std::min(umin, R.cu[i]); umax = std::max(umax, R.cu[i]); }
    R.y0 = std::max(0, static_cast<int>(std::floor(umin / 4.0f - R.hi / 2.0f)) + ky + oy - 1);
    R.y1 = std::min(h - 1, static_cast<int>(std::ceil(umax / 4.0f - R.lo / 2.0f)) + ky + oy + 1);
    return R;
}
// The region's pixels, a row at a time: row(py, x0, x1) for each row with
// any. This row's ground points have X + Z in [u0, u1] (heights lo to hi):
// the columns are the region's X - Z over that strip.
template <class Row>
void for_region_rows(const ShadowRegion& R, int w, int ox, int oy, int kx, int ky, Row&& row) {
    // The region's edges, corner to corner around it.
    static constexpr int kRing[4] = {0, 1, 3, 2};
    for (int py = R.y0; py <= R.y1; ++py) {
        const float u0 = 4.0f * (py - oy - ky) + 2.0f * R.lo, u1 = 4.0f * (py - oy - ky) + 2.0f * R.hi;
        float vmin = 1e9f, vmax = -1e9f;
        for (int e = 0; e < 4; ++e) {
            const int i = kRing[e], j = kRing[(e + 1) % 4];
            if (R.cu[i] >= u0 && R.cu[i] <= u1) { vmin = std::min(vmin, R.cv[i]); vmax = std::max(vmax, R.cv[i]); }
            for (const float uu : {u0, u1}) {
                const float du = R.cu[j] - R.cu[i];
                if (du == 0.0f) continue;
                const float t = (uu - R.cu[i]) / du;
                if (t < 0.0f || t > 1.0f) continue;
                const float vv = R.cv[i] + t * (R.cv[j] - R.cv[i]);
                vmin = std::min(vmin, vv); vmax = std::max(vmax, vv);
            }
        }
        if (vmin > vmax) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(vmin / 2.0f)) + kx + ox - 1);
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(vmax / 2.0f)) + kx + ox + 1);
        row(py, x0, x1);
    }
}
// A prop's shadow edge is blurred by at least this many texels: a unit's
// sharpens toward its feet, which on a short prop left a hard-edged dark
// shape under every bush (GK; the user reported hard edges still in every
// prop's shadow).
constexpr int kPropMinBlur = 6;
// (`view_key` is read only by KOL_PROP_SHADOW_BY_VIEW's old keying, below.)
void cast_prop_shadows(int w, int h, int ox, int oy, int kx, int ky, std::uint32_t view_key, float soft) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    g_prop_cast_next.clear();
    if (g_prop_dist.size() != n || g_kind.size() != n) {
        g_prop_cast_cache.clear();
        return;
    }
    float lx, lz;
    shadow_dir(lx, lz);
    const float rise = shadow_rise();
    const float ax = -lz, az = lx;   // across the sun
    constexpr float kRoot2 = 1.41421356f;
    static thread_local std::vector<int> prop_of;
    const std::vector<PropGroup> props = scene_props(w, h, prop_of);
    // Keyed on the prop's place in the camera's frame, not the camera:
    // a pan moves a prop and its shadow the same whole pixels, so the
    // shadow is kept and moved with it (map 102's fifty props recast
    // every frame of the narration's pan were a tenth of the Thor's
    // worker). KOL_PROP_SHADOW_BY_VIEW=1: by the view, as before.
    static const bool by_view = std::getenv("KOL_PROP_SHADOW_BY_VIEW") != nullptr;
    // A shadow that falls on nothing (empty) is kept too: on map 102 in
    // the rain most props' are, and they were cast again every frame.
    // KOL_PROP_SHADOW_EMPTY_RECAST=1: as before.
    static const bool empty_recast = std::getenv("KOL_PROP_SHADOW_EMPTY_RECAST") != nullptr;
    static std::atomic<long> n_still{0}, n_moved{0}, n_cast{0};   // KOL_PROP_SHADOW_LOG=1
    static const bool cast_log = std::getenv("KOL_PROP_SHADOW_LOG") != nullptr;
    // In three steps (2026-11-20, Thor state 0311: map 102's seventy-odd
    // props were all cast again on one thread at each step of the sunset,
    // 2 to 3 ms of that picture, and every other picture each one's mask was
    // made and hashed for a key that had not changed, 0.3 ms):
    // 1. each prop's key, from its mask: on the row pool; none of it while
    //    the groups (scene_props' count), the sun, the heights, the view and
    //    the camera are the last picture's, when the keys are too;
    // 2. on this thread, in order: each prop's shadow of the last picture
    //    found by its key (a still camera takes it whole);
    // 3. the rest moved with the camera or cast again, on the row pool: a
    //    prop's cast writes its own list and reads the scene.
    // KOL_PROP_SHADOW_SERIAL=1: on this thread alone, a key every picture.
    static const bool serial = kol::env_set("KOL_PROP_SHADOW_SERIAL");
    const int np = static_cast<int>(props.size());
    const int camx = kx + ox, camy = ky + oy;
    static thread_local std::vector<std::vector<std::uint8_t>> masks_tl;
    static thread_local std::vector<std::uint32_t> keys_tl;
    static thread_local std::vector<std::uint8_t> mask_made_tl;
    struct KeysFor {
        bool valid = false;
        std::uint64_t gen = 0;
        std::uint32_t base = 0, stale = 0;
        int w = 0, h = 0, camx = 0, camy = 0, np = 0;
    };
    static thread_local KeysFor keys_for;
    // (This thread's, by reference: a helper naming a thread_local reaches its own.)
    std::vector<std::vector<std::uint8_t>>& masks = masks_tl;
    std::vector<std::uint32_t>& keys = keys_tl;
    std::vector<std::uint8_t>& mask_made = mask_made_tl;
    const int* const pof = prop_of.data();
    if (static_cast<int>(masks.size()) < np) masks.resize(np);
    const std::uint32_t key_base = by_view ? view_key : g_shadow_key;
    auto make_mask = [&](int pi) {
        const PropGroup& p = props[pi];
        const int bw = p.x1 - p.x0 + 1, bh = p.y1 - p.y0 + 1;
        std::vector<std::uint8_t>& mask = masks[pi];
        mask.assign(static_cast<std::size_t>(bw) * bh, 0);
        for (int j = 0; j < bh; ++j)
            for (int i = 0; i < bw; ++i) {
                const int px = p.x0 + i, py = p.y0 + j;   // (past the scene: p.out)
                if (px >= 0 && py >= 0 && px < w && py < h)
                    mask[static_cast<std::size_t>(j) * bw + i] = pof[static_cast<std::size_t>(py) * w + px] == pi;
            }
        for (const auto& [px, py] : p.out) mask[static_cast<std::size_t>(py - p.y0) * bw + (px - p.x0)] = 1;
        mask_made[pi] = 1;
    };
    // A step of the sun recasts every prop's shadow at once (1.3 ms of that
    // picture on the Mac, map 102's sunset, 2026-11-20). Spread instead over
    // `spread` pictures: a share of them cast each picture, the rest keep their
    // shadow from the sun before (found by the key it was made with, `stale`)
    // until their turn. Only when the sun alone moved (the heights are the
    // same) and the props and camera are as they were: a prop keeps a shadow
    // only for the very same mask at the very same place. For a picture or
    // two, part of the props' shadows are a step of the sun behind the
    // terrain's (a few pixels at the sunset's end, at most).
    // KOL_PROP_SHADOW_SPREAD=<pictures> (3); 0 or 1: all at once, as before.
    static const int spread = std::max(0, static_cast<int>(kol::env_num_or("KOL_PROP_SHADOW_SPREAD", 3.0f)));
    struct Before {
        bool valid = false;
        std::uint32_t base = 0, heights = 0;   // the sun's key the shadows were all made with, and the heights'
    };
    static thread_local Before before;
    const bool spreading = spread > 1 && !by_view && before.valid && before.base != key_base &&
                           before.heights == g_cast_heights_key;
    const std::uint32_t stale_base = spreading ? before.base : 0u;
    static thread_local std::vector<std::uint32_t> stale_tl;
    std::vector<std::uint32_t>& stale = stale_tl;
    const bool keys_same = !serial && t_props_gen_ok && keys_for.valid && keys_for.gen == t_props_gen &&
                           keys_for.base == key_base && keys_for.stale == stale_base && keys_for.w == w &&
                           keys_for.h == h && keys_for.camx == camx && keys_for.camy == camy && keys_for.np == np &&
                           static_cast<int>(keys.size()) == np && static_cast<int>(stale.size()) == np;
    mask_made.assign(np, 0);
    if (!keys_same) {
        keys.assign(np, 0);
        stale.assign(np, 0);
        auto key_range = [&](int p0, int p1) {
            for (int pi = p0; pi < p1; ++pi) {
                const PropGroup& p = props[pi];
                const int bw = p.x1 - p.x0 + 1, bh = p.y1 - p.y0 + 1;
                make_mask(pi);
                auto key_of = [&](std::uint32_t base) {
                    std::uint32_t key = base;
                    if (!by_view)
                        kol::fnv_ints(key, {w, h});
                    kol::fnv_ints(key, {p.xz, p.y, by_view ? p.x0 : p.x0 - camx, by_view ? p.y0 : p.y0 - camy, bw, bh});
                    for (std::uint8_t v : masks[pi]) kol::fnv_mix(key, v);
                    return key;
                };
                keys[pi] = key_of(key_base);
                if (spreading) stale[pi] = key_of(stale_base);
            }
        };
        if (serial || np < 8) key_range(0, np);
        else kol::parallel_rows(np, 4, [&](int, int p0, int p1) { key_range(p0, p1); });
        keys_for.valid = t_props_gen_ok;
        keys_for.gen = t_props_gen;
        keys_for.base = key_base;
        keys_for.stale = stale_base;
        keys_for.w = w;
        keys_for.h = h;
        keys_for.camx = camx;
        keys_for.camy = camy;
        keys_for.np = np;
    }
    g_prop_cast_next.resize(np);
    static thread_local std::vector<CastCache*> old_of_tl;
    static thread_local std::vector<int> work_tl;
    std::vector<CastCache*>& old_of = old_of_tl;
    std::vector<int>& work = work_tl;
    old_of.assign(np, nullptr);
    work.clear();
    // While spreading: this picture casts at most `budget` (the misses
    // beyond it keep their old shadow when they have one).
    int budget = spreading ? (np + spread - 1) / spread : np;
    int kept_stale = 0;
    for (int pi = 0; pi < np; ++pi) {
        const std::uint32_t key = keys[pi];
        g_prop_cast_next[pi] = CastCache{key, {}};
        CastCache& cur = g_prop_cast_next[pi];
        cur.camx = camx;
        cur.camy = camy;
        cur.wx0 = -camx; cur.wy0 = -camy; cur.wx1 = w - 1 - camx; cur.wy1 = h - 1 - camy;
        CastCache* old_hit = nullptr;
        for (CastCache& old : g_prop_cast_cache)
            if (old.key == key && !old.taken && !(empty_recast && old.px.empty())) { old_hit = &old; break; }
        if (cast_log && pi == 0 && (n_still + n_moved + n_cast) > 0 && (n_still + n_moved + n_cast) % 2000 < 60)
            std::fprintf(stderr, "prop_shadows: still %ld moved %ld cast %ld\n", n_still.load(), n_moved.load(), n_cast.load());
        if (old_hit && old_hit->camx == camx && old_hit->camy == camy) {   // a still camera
            ++n_still;
            cur.px.swap(old_hit->px);
            old_hit->taken = true;
            cur.wx0 = old_hit->wx0; cur.wy0 = old_hit->wy0; cur.wx1 = old_hit->wx1; cur.wy1 = old_hit->wy1;
            continue;
        }
        if (spreading && budget <= 0) {
            CastCache* old_stale = nullptr;
            for (CastCache& old : g_prop_cast_cache)
                if (old.key == stale[pi] && !old.taken && !(empty_recast && old.px.empty())) { old_stale = &old; break; }
            if (old_stale && old_stale->camx == camx && old_stale->camy == camy) {
                cur.key = stale[pi];   // (found by it again next picture)
                cur.px.swap(old_stale->px);
                old_stale->taken = true;
                cur.wx0 = old_stale->wx0; cur.wy0 = old_stale->wy0; cur.wx1 = old_stale->wx1; cur.wy1 = old_stale->wy1;
                ++kept_stale;
                continue;
            }
        }
        old_of[pi] = old_hit;
        work.push_back(pi);
        --budget;
    }
    static const bool spread_log = kol::env_true("KOL_PROP_SHADOW_SPREAD_LOG");
    if (spread_log && (spreading || !work.empty()))
        std::fprintf(stderr, "prop-spread: props %d cast %zu kept-old %d spreading %d\n", np, work.size(), kept_stale,
                     spreading ? 1 : 0);
    // Every shadow is the sun's now: the next step spreads from here.
    if (!kept_stale) {
        before.valid = !by_view;
        before.base = key_base;
        before.heights = g_cast_heights_key;
    }
    auto cast_one = [&](int pi) {
        const PropGroup& p = props[pi];
        const int bw = p.x1 - p.x0 + 1, bh = p.y1 - p.y0 + 1;
        if (!mask_made[pi]) make_mask(pi);
        const std::vector<std::uint8_t>& mask = masks[pi];
        CastCache& cur = g_prop_cast_next[pi];
        std::vector<std::uint32_t>& list = cur.px;
        CastCache* const old_hit = old_of[pi];
        // The card: at the prop's place, across its pixels' middle column;
        // its foot the prop's ground (its pixels' place) on its foot row.
        const float feet_hx = 0.5f * static_cast<float>(p.x0 + p.x1 + 1);
        const float feet_hy = static_cast<float>(p.xz) / 4.0f - static_cast<float>(p.y) / 2.0f + ky + oy;
        const float cdx = 2.0f * (feet_hx - ox - kx);   // X - Z
        const float cx = 0.5f * (p.xz + cdx), cz = 0.5f * (p.xz - cdx);
        const float foot = static_cast<float>(p.y);
        const float card_h = 2.0f * (feet_hy - p.y0);   // world units up to its top
        if (card_h <= 0.0f) return;
        static thread_local BoxSum box;
        box.build(bw, bh, [&](int i, int j) { return mask[static_cast<std::size_t>(j) * bw + i]; });
        // Where the shadow can lie, as for a unit.
        const ShadowRegion R = shadow_region(
            cx, cz, foot, foot + card_h, std::max(feet_hx - p.x0, p.x1 + 1 - feet_hx) * kRoot2, lx, lz, rise, 3,
            [&](float far_d) { return std::max(static_cast<float>(kPropMinBlur), soft * far_d); }, h, oy, ky);
        const int y0 = R.y0, y1 = R.y1;
        if (old_hit) {
            // Moved with the camera, if all of it the scene shows now lay in
            // the scene it was worked out over (in the camera's frame).
            float vlo = 1e9f, vhi = -1e9f;
            for (int i = 0; i < 4; ++i) { vlo = std::min(vlo, R.cv[i]); vhi = std::max(vhi, R.cv[i]); }
            const int nx0 = std::max(static_cast<int>(std::floor(vlo / 2.0f)) - 1 + kx + ox, 0) - camx;
            const int nx1 = std::min(static_cast<int>(std::ceil(vhi / 2.0f)) + 1 + kx + ox, w - 1) - camx;
            const int ny0 = y0 - camy, ny1 = y1 - camy;
            if (nx0 > nx1 || ny0 > ny1 ||
                (nx0 >= old_hit->wx0 && nx1 <= old_hit->wx1 && ny0 >= old_hit->wy0 && ny1 <= old_hit->wy1)) {
                const int dx = camx - old_hit->camx, dy = camy - old_hit->camy;
                list.clear();
                for (const std::uint32_t v : old_hit->px) {
                    const int q = static_cast<int>(v & 0xFFFFFFu);
                    const int x = q % w + dx, y = q / w + dy;
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    list.push_back(static_cast<std::uint32_t>(y * w + x) | (v & 0xFF000000u));
                }
                cur.wx0 = old_hit->wx0; cur.wy0 = old_hit->wy0; cur.wx1 = old_hit->wx1; cur.wy1 = old_hit->wy1;
                ++n_moved;
                return;
            }
        }
        ++n_cast;
        for_region_rows(R, w, ox, oy, kx, ky, [&](int py, int x0, int x1) {
            for (int px = x0; px <= x1; ++px) {
                const std::size_t q = static_cast<std::size_t>(py) * w + px;
                const bool under = g_lift_ground.size() == n && g_lift_ground[q];   // the ground under a lifted prop
                if (g_prop_dist[q] != kUnreached && !under) continue;
                if (g_covered[q] && g_face.size() == n &&
                    ((g_face[q] == 1 && lx <= 0.0f) || (g_face[q] == 2 && lz <= 0.0f)))
                    continue;
                const float Y = g_covered[q] ? static_cast<float>(under ? g_pre_y[q] : g_y[q]) : 0.0f;
                const float dxz = 2.0f * static_cast<float>(px - ox - kx);
                const float sxz = static_cast<float>(g_covered[q] ? (under ? g_pre_xz[q] : g_xz[q]) : 4 * (py - oy - ky));
                const float X = 0.5f * (sxz + dxz), Z = 0.5f * (sxz - dxz);
                const float rx = X - cx, rz = Z - cz;
                const float d = -(rx * lx + rz * lz);
                if (d < 0.0f) continue;
                const float up = Y + d * rise - foot;
                const float col = (rx * ax + rz * az) / kRoot2;
                const int tx = static_cast<int>(std::floor(feet_hx + col)) - p.x0;
                const int ty = static_cast<int>(std::floor(feet_hy - up / 2.0f)) - p.y0;
                std::uint32_t alpha = 255;
                const int rr = std::max(kPropMinBlur, soft > 0.0f ? static_cast<int>(std::lround(soft * d)) : 0);   // never sharper than kPropMinBlur
                if (rr > 0) {
                    if (up < -2.0f * rr || up > card_h + 2.0f * rr) continue;
                    const int m = box.count(tx - rr, ty - rr, tx + rr, ty + rr);
                    if (m == 0) continue;
                    alpha = static_cast<std::uint32_t>(255 * m / ((2 * rr + 1) * (2 * rr + 1)));
                    if (alpha == 0) continue;
                } else {
                    if (up < 0.0f || up > card_h) continue;
                    if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
                    if (!mask[static_cast<std::size_t>(ty) * bw + tx]) continue;
                }
                if (sun_blocked(static_cast<int>(X), static_cast<int>(Z), Y)) continue;   // the terrain's, an awning's
                list.push_back(static_cast<std::uint32_t>(q) | (alpha << 24));
            }
        });
    };
    const int nwork = static_cast<int>(work.size());
    if (serial || nwork < 4) {
        for (const int pi : work) cast_one(pi);
    } else {
        kol::parallel_rows(nwork, 2, [&](int, int i0, int i1) {
            for (int i = i0; i < i1; ++i) cast_one(work[i]);
        });
    }
    g_prop_cast_cache.swap(g_prop_cast_next);
    g_prop_cast_next.clear();
}

// A unit's card shadow toward a light along the ground (lx, lz), climbing
// `rise` a unit across (see cast_unit_shadows): the pixels it covers, with
// their alpha, into `list`. `sun`: not where the terrain's shadow or an
// awning's is already.
// `fade` (> 0): the shadow pales along its length, gone `fade` units from
// the card (a door light's; 0 none).
void card_shadow(const BlockCaster& c, const kol::ScenePixels::SpriteBlock& b, int ax0, int ay0, float lx, float lz,
                 float rise, float soft, bool sun, int w, int h, int ox, int oy, int kx, int ky,
                 std::vector<std::uint32_t>& list, float fade = 0.0f) {
    constexpr float kRoot2 = 1.41421356f;
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const float ax = -lz, az = lx;   // across the light
    // The sprite's solid texels as running sums, for the soft edge's box
    // average (BoxSum).
    static thread_local BoxSum box;
    if (soft > 0.0f)
        box.build(b.w, b.h, [&](int i, int j) {
            return g_atlas_opaque[static_cast<std::size_t>(ay0 + j) * g_atlas_w + ax0 + i];
        });
    // Where the shadow can lie, on the ground (shadow_region; with a soft
    // edge, a third pass widens it by its blur at the far end).
    // The card's top, in units over its foot: a slot's, or for the top
    // rows of a tall unit (c.upper) its own top row's.
    const float card_up = c.upper ? 2.0f * (c.feet_hy - static_cast<float>(c.hy0)) : 2.0f * S;
    const float top = static_cast<float>(c.foot) + card_up;
    const ShadowRegion R =
        shadow_region(static_cast<float>(c.x), static_cast<float>(c.z), static_cast<float>(c.foot), top,
                      0.5f * S * kRoot2, lx, lz, rise, soft > 0.0f ? 3 : 2, [&](float far_d) { return soft * far_d; },
                      h, oy, ky);
    // (The faces were tested only while g_cast_mark, the sun pass's mark
    // set AFTER this, was scene-sized: never on a session's first sun
    // frame or after a size change, and on a dark map never at all unless
    // a lit map at the same size came first. KOL_CAST_FACE_BY_MARK=1: so.)
    static const bool face_by_mark = std::getenv("KOL_CAST_FACE_BY_MARK") != nullptr;
    const bool faces = face_by_mark ? g_face.size() == g_cast_mark.size()
                                    : g_face.size() == static_cast<std::size_t>(w) * h;
    for_region_rows(R, w, ox, oy, kx, ky, [&](int py, int x0, int x1) {
        for (int px = x0; px <= x1; ++px) {
            const std::size_t q = static_cast<std::size_t>(py) * w + px;
            if (g_prop_dist[q] != kUnreached) continue;
            // A wall turned away from the sun is in its own shadow
            // already (the terrain pass darkened it).
            if (g_covered[q] && faces &&
                ((g_face[q] == 1 && lx <= 0.0f) || (g_face[q] == 2 && lz <= 0.0f)))
                continue;
            const float Y = g_covered[q] ? static_cast<float>(g_y[q]) : 0.0f;
            const float dxz = 2.0f * static_cast<float>(px - ox - kx);
            const float sxz = static_cast<float>(g_covered[q] ? g_xz[q] : 4 * (py - oy - ky));
            const float X = 0.5f * (sxz + dxz), Z = 0.5f * (sxz - dxz);
            const float rx = X - c.x, rz = Z - c.z;
            const float d = -(rx * lx + rz * lz);   // toward the sun to the card
            if (d < 0.0f) continue;
            const float up = Y + d * rise - c.foot;   // height on the card
            const float col = (rx * ax + rz * az) / kRoot2;
            const int tx = static_cast<int>(std::floor(c.feet_hx + col)) - c.hx0;
            const int ty = static_cast<int>(std::floor(c.feet_hy - up / 2.0f)) - c.hy0;
            std::uint32_t alpha = 255;
            // The edge blurs with the distance from the card (d): the
            // share of solid texels in a box that grows with it (whole
            // texels: the radius steps every 1 / soft units).
            const int rr = soft > 0.0f ? static_cast<int>(std::lround(soft * d)) : 0;
            if (rr > 0) {
                if (up < -2.0f * rr || up > card_up + 2.0f * rr) continue;
                const int n = box.count(tx - rr, ty - rr, tx + rr, ty + rr);
                if (n == 0) continue;
                alpha = static_cast<std::uint32_t>(255 * n / ((2 * rr + 1) * (2 * rr + 1)));
                if (alpha == 0) continue;
            } else {
                if (up < 0.0f || up > card_up) continue;
                if (tx < 0 || ty < 0 || tx >= b.w || ty >= b.h) continue;
                if (!g_atlas_opaque[static_cast<std::size_t>(ay0 + ty) * g_atlas_w + ax0 + tx]) continue;
            }
            // In the terrain's already, or an awning's.
            if (sun && sun_blocked(static_cast<int>(X), static_cast<int>(Z), Y)) continue;
            if (fade > 0.0f) {
                alpha = static_cast<std::uint32_t>(std::lround(alpha * std::max(0.0f, 1.0f - d / fade)));
                if (alpha == 0) continue;
            }
            list.push_back(static_cast<std::uint32_t>(q) | (alpha << 24));   // (q < 2^24)
        }
    });
}

// Units cast their shadows on the ground (with the terrain's shadows on).
// Each unit stands as a card turned to face the sun (turned to the camera,
// a sun from the side would see it edge on and cast a sliver): a point of
// the scene is in the unit's shadow when the line from it toward the sun
// meets the card on one of the unit's solid texels. The card's width is the
// sprite's columns (one column, sqrt 2 world units across the sun), its
// height the sprite's rows (one row, 2 units up). Darkens g_ao as the
// terrain's shadow does, once, and not where that shadow already is.
// The shadow's edge softens with the distance from the unit: the blur's
// radius in sprite texels per world unit along the sun (the user's pick,
// 0.06, from shots/soft_shadows). KOL_SHADOW_SOFT=<rate>; 0: hard. (The
// lamps' unit shadows too, cast_lamp_shadows.)
float unit_shadow_soft() {
    static const float v = kol::env_num("KOL_SHADOW_SOFT", 0.06f);
    return v;
}
// Whether any of block bi's pixels is in the picture (g_lift); true when
// that is not known (g_lift not scene-sized).
bool card_in_picture(std::size_t bi, const kol::ScenePixels::SpriteBlock& b, int w, int h) {
    if (g_lift.size() != static_cast<std::size_t>(w) * h) return true;
    for (int j = 0; j < b.h; ++j)
        for (int i = 0; i < b.w; ++i) {
            const int px = b.sx + i, py = b.sy + j;
            if (px >= 0 && py >= 0 && px < w && py < h && g_lift[static_cast<std::size_t>(py) * w + px] == bi + 1)
                return true;
        }
    return false;
}
// A unit's card shadow's key, from `key`: the card's place and size, then
// `extra`, then its opaque texels (in the atlas at ax0, ay0).
std::uint32_t card_key(std::uint32_t key, const BlockCaster& c, const kol::ScenePixels::SpriteBlock& b, int ax0, int ay0,
                       std::initializer_list<int> extra) {
    kol::fnv_ints(key, {c.x, c.z, c.foot, c.hx0, c.hy0, b.w, b.h});
    kol::fnv_ints(key, extra);
    for (int j = 0; j < b.h; ++j) {
        const std::uint8_t* op = &g_atlas_opaque[static_cast<std::size_t>(ay0 + j) * g_atlas_w + ax0];
        for (int i = 0; i < b.w; ++i) kol::fnv_mix(key, op[i]);
    }
    return key;
}
// Last frame's shadow with the same key, moved into `list` (a list left
// empty is not kept: cast again). False when there is none.
template <class C>
bool take_cached(std::vector<C>& cache, std::uint32_t key, std::vector<std::uint32_t>& list) {
    for (C& old : cache)
        if (old.key == key && !old.px.empty()) {
            list.swap(old.px);
            return true;
        }
    return false;
}
}  // namespace

namespace kol_focus_detail {
void cast_unit_shadows(int w, int h, int ox, int oy, int kx, int ky) {
    const bool units = units_cast() && !g_casters.empty();
    const bool props = props_cast();
    if ((!units && !props) || g_ao.size() != static_cast<std::size_t>(w) * h) return;
    float lx, lz;
    shadow_dir(lx, lz);
    const float rise = shadow_rise();
    const float k = 1.0f - shadow_strength();
    const float soft = unit_shadow_soft();
    std::uint32_t view_key = g_shadow_key;   // the heights and the sun
    kol::fnv_ints(view_key, {w, h, ox, oy, kx, ky});
    g_cast_next.clear();
    for (std::size_t bi = 0; units && bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        if (!c.unit) continue;
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        if (b.scenery) continue;   // a door in its wall casts none (map 3's church door, 0x70)
        // A unit with none of its pixels in the picture casts none: map 3's
        // two waiting inside the church after the rainy battle (behind its
        // door by their sprites' priority, at 2,2 and 2,3) cast their whole
        // sprites' shadows on the ground before the door, which the user
        // took for the gulls' (not following the sun's
        // path). One partly hidden (behind a wall) still casts it whole.
        // KOL_HIDDEN_UNITS_CAST=1: as before.
        static const bool hidden_cast = std::getenv("KOL_HIDDEN_UNITS_CAST") != nullptr;
        if (!hidden_cast && !card_in_picture(bi, b, w, h)) continue;
        const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
        const std::uint32_t key = card_key(view_key, c, b, ax0, ay0, {static_cast<int>(c.strength * 256.0f)});
        g_cast_next.push_back({key, {}});
        std::vector<std::uint32_t>& list = g_cast_next.back().px;
        if (take_cached(g_cast_cache, key, list)) continue;
        card_shadow(c, b, ax0, ay0, lx, lz, rise, soft, true, w, h, ox, oy, kx, ky, list);
        if (c.strength < 1.0f)
            for (std::uint32_t& v : list) {
                const std::uint32_t a = static_cast<std::uint32_t>(std::lround((v >> 24) * c.strength));
                v = (v & 0xFFFFFFu) | (a << 24);
            }
    }
    g_cast_cache.swap(g_cast_next);
    g_cast_next.clear();
    if (props) cast_prop_shadows(w, h, ox, oy, kx, ky, view_key, soft);
    else g_prop_cast_cache.clear();
    // Applied every frame; a pixel in two shadows (units', props') darkens
    // once, by the stronger. Only the map's pixels take it in the end
    // (kol_perspective), not the sky's.
    g_cast_mark.assign(static_cast<std::size_t>(w) * h, 0);
    for (const auto* cache : {&g_cast_cache, &g_prop_cast_cache})
        for (const CastCache& cc : *cache)
            for (const std::uint32_t v : cc.px) {
                const std::uint32_t q = v & 0xFFFFFFu, a = v >> 24;
                if (a > g_cast_mark[q]) g_cast_mark[q] = static_cast<std::uint8_t>(a);
            }
    for (std::size_t q = 0; q < g_cast_mark.size(); ++q) {
        const unsigned a = g_cast_mark[q];
        if (!a) continue;
        g_ao[q] = static_cast<std::uint8_t>(std::lround(
            a == 255u ? g_ao[q] * k : g_ao[q] * (1.0f - (1.0f - k) * a / 255.0f)));
    }
}

// Indoors, units cast shadows from the window light (asked 2026-10-29 on
// map 62, with the light through its window, for units to cast shadows):
// each unit's card shadow along the light's way (the map's sun, which the
// windows let in: card_shadow), taking that share of the window's light
// where it falls, so the shadow is a dark shape in the lit pool and
// nothing outside it. The game's blob stays under them (out of the light
// they would have none). With Unit Shadows on. KOL_NO_WINDOW_SHADOWS=1: off.
namespace {
// One window light's unit shadows: what falls in them is taken off `patch`.
void window_light_shadows(std::vector<std::uint8_t>& patch, float lx, float lz, float rise, int w, int h, int ox, int oy, int kx, int ky) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    // A little softer than the sun's (0.06): the window is an opening, not
    // a point (the user asked for slightly softer).
    static const float soft = kol::env_num("KOL_WINDOW_SHADOW_SOFT", 0.09f);
    // (The marks kept per thread, all 0 between calls: each pixel marked is
    // listed once, applied and cleared, not a scene-sized buffer zeroed and
    // scanned every frame.)
    static thread_local std::vector<std::uint32_t> list, touched;
    static thread_local std::vector<std::uint8_t> mark;
    if (mark.size() != n) mark.assign(n, 0);
    touched.clear();
    for (std::size_t bi = 0; bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        if (!c.unit) continue;
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        if (b.scenery) continue;
        if (!card_in_picture(bi, b, w, h)) continue;   // (as cast_unit_shadows: a unit not in the picture casts none)
        list.clear();
        const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
        card_shadow(c, b, ax0, ay0, lx, lz, rise, soft, false, w, h, ox, oy, kx, ky, list);
        for (const std::uint32_t v : list) {
            const std::uint32_t q = v & 0xFFFFFFu, a = static_cast<std::uint32_t>(std::lround((v >> 24) * c.strength));
            if (q < n && a > mark[q]) {
                if (!mark[q]) touched.push_back(q);
                mark[q] = static_cast<std::uint8_t>(a);
            }
        }
    }
    // A unit that is part of the model (Alphonse in bed, g_model_actor_own_light) has
    // no card: a lit point is in his shadow if its ray toward the light meets his
    // plane at one of his pixels. A few spots round that, wider the further the
    // shadow falls, for a soft edge.
    float pn[3], pd = 0.0f;
    if (g_model_actor_own_light && g_model_actor_px.size() == n && g_wx.size() == n && model_actor_plane(pn, pd)) {
        const float den = pn[0] * lx + pn[1] * rise + pn[2] * lz;
        if (std::fabs(den) > 1e-4f)
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px) {
                    const std::size_t q = static_cast<std::size_t>(py) * w + px;
                    if (!patch[q] || !g_covered[q] || g_model_actor_px[q]) continue;
                    const float X = g_wx[q], Y = static_cast<float>(g_y[q]), Z = g_wz[q];
                    const float t = (pd - pn[0] * X - pn[1] * Y - pn[2] * Z) / den;
                    if (t <= 1.0f) continue;
                    const float QX = X + t * lx, QY = Y + t * rise, QZ = Z + t * lz;
                    const float sx = (QX - QZ) * 0.5f + static_cast<float>(ox + kx);
                    const float sy = (QX + QZ) * 0.25f - QY * 0.5f + static_cast<float>(oy + ky);
                    const float r = std::min(4.0f, 0.5f + soft * t);
                    int hits = 0;
                    static constexpr float kSpot[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    for (const auto& o : kSpot) {
                        const int ax = static_cast<int>(std::floor(sx + o[0] * r)), ay = static_cast<int>(std::floor(sy + o[1] * r));
                        if (ax >= 0 && ay >= 0 && ax < w && ay < h && g_model_actor_px[static_cast<std::size_t>(ay) * w + ax]) ++hits;
                    }
                    const std::uint32_t a = static_cast<std::uint32_t>(hits * 51);
                    if (a > mark[q]) {
                        if (!mark[q]) touched.push_back(static_cast<std::uint32_t>(q));
                        mark[q] = static_cast<std::uint8_t>(a);
                    }
                }
    }
    for (const std::uint32_t q : touched) {
        if (patch[q]) patch[q] = static_cast<std::uint8_t>(std::lround(patch[q] * (1.0f - mark[q] / 255.0f)));
        mark[q] = 0;
    }
}
}  // namespace

void cast_window_shadows(int w, int h, int ox, int oy, int kx, int ky) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (!units_cast_from_windows()) return;
    if (g_win_any && g_win_patch.size() == n) {
        float lx, lz;
        shadow_dir(lx, lz);
        window_light_shadows(g_win_patch, lx, lz, shadow_rise(), w, h, ox, oy, kx, ky);
    }
    // The second light through the window, where it casts them too
    // (map_settings.cfg `window_unit_shadows`; map 67's moon casts none). It is kept
    // between frames: from the patch as built, each time.
    if (g_moon_on && g_moon_any && g_moon_patch_built.size() == n && kol::map_flag(kol::MapFlag::WindowUnitShadows, current_map())) {
        g_moon_patch = g_moon_patch_built;
        window_light_shadows(g_moon_patch, g_moon_dx, g_moon_dz, g_moon_rise, w, h, ox, oy, kx, ky);
    }
}

// Ambient occlusion around each unit's feet, to stand it on the ground: the
// ground's top within kContactR of the unit's place, at its height, darkens
// toward the feet, 1.4 x the occlusion's strength x (1 - r / R)^0.7, at most
// 0.7. With
// Ambient Occl. on, shadows or not.

// On a dark map (kDarkMaps), units cast shadows from its lamps (asked
// 2026-10-14 on map 51 to get shadows working there): for each
// unit and each lamp in reach, the unit's card shadow as from the sun
// (card_shadow), toward the lamp along the ground, climbing as the flame
// stands over the unit's feet, and as strong as the lamp lights the unit
// (its power and fall-off, as g_glow's). A shadowed pixel loses that share
// of the lamps' light (g_lamp_shade), so it falls back toward the dark.
// KOL_NO_LAMP_SHADOWS=1 leaves them out.
bool units_cast_from_lamps() {
    static const bool off = std::getenv("KOL_NO_LAMP_SHADOWS") != nullptr;
    return !off && indoor_dark() < 1.0f && unit_shadows_on() && !g_frame_lamps.empty();
}
// The props' shadows from the map's lamps (kLampPropShadowMaps), into the
// lamp shade (a pixel keeps the strongest): each prop a card as for the sun
// (cast_prop_shadows), along the ground away from each lamp, climbing as the
// lamp stands over it, at the lamp's strength on the prop as a unit's is. Not
// kept between frames: a few props on the maps that have it.
void cast_prop_lamp_shadows(int w, int h, int ox, int oy, int kx, int ky, float soft) {
    static const bool off = std::getenv("KOL_NO_PROP_LAMP_SHADOWS") != nullptr;
    const int map = current_map();
    bool listed = false;
    for (int m : kLampPropShadowMaps) listed = listed || m == map;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (off || !listed || g_frame_lamps.empty() || g_prop_dist.size() != n || g_kind.size() != n ||
        g_lamp_shade.size() != n)
        return;
    constexpr float kRoot2 = 1.41421356f;
    static thread_local std::vector<int> prop_of;
    const std::vector<PropGroup> props = scene_props(w, h, prop_of);
    for (int pi = 0; pi < static_cast<int>(props.size()); ++pi) {
        const PropGroup& p = props[pi];
        const int bw = p.x1 - p.x0 + 1, bh = p.y1 - p.y0 + 1;
        static thread_local std::vector<std::uint8_t> mask;
        mask.assign(static_cast<std::size_t>(bw) * bh, 0);
        for (int j = 0; j < bh; ++j)
            for (int i = 0; i < bw; ++i) {
                const int px = p.x0 + i, py = p.y0 + j;
                if (px >= 0 && py >= 0 && px < w && py < h)
                    mask[static_cast<std::size_t>(j) * bw + i] = prop_of[static_cast<std::size_t>(py) * w + px] == pi;
            }
        for (const auto& [px, py] : p.out) mask[static_cast<std::size_t>(py - p.y0) * bw + (px - p.x0)] = 1;
        const float feet_hx = 0.5f * static_cast<float>(p.x0 + p.x1 + 1);
        const float feet_hy = static_cast<float>(p.xz) / 4.0f - static_cast<float>(p.y) / 2.0f + ky + oy;
        const float cdx = 2.0f * (feet_hx - ox - kx);   // X - Z
        const float cx = 0.5f * (p.xz + cdx), cz = 0.5f * (p.xz - cdx);
        const float foot = static_cast<float>(p.y);
        const float card_h = 2.0f * (feet_hy - p.y0);
        if (card_h <= 0.0f) continue;
        static thread_local BoxSum box;
        box.build(bw, bh, [&](int i, int j) { return mask[static_cast<std::size_t>(j) * bw + i]; });
        for (const LampPt& L : g_frame_lamps) {
            const float dx = L.x - cx, dz = L.z - cz;
            const float D = std::hypot(dx, dz);
            const float dy = L.y - (foot + 0.5f * card_h);
            const float d3 = std::sqrt(D * D + dy * dy);
            if (D < 4.0f || d3 >= L.reach || L.cos_out >= -1.0f) continue;   // (not a door's light)
            // At the lamp's strength on the prop, as a unit's, made stronger:
            // at that strength the crystals' shadows did not show on the Thor
            // (the user could not see the shadow).
            // KOL_PROP_LAMP_SHADOW_K=<k> (1: a unit's).
            static const float strength_k = kol::env_num_min("KOL_PROP_LAMP_SHADOW_K", 0.0f, 2.0f);
            const float lit = std::min(1.0f, strength_k * L.power * (1.0f - d3 / L.reach) * L.flick);
            if (lit <= 0.02f) continue;
            const float lx = dx / D, lz = dz / D;
            const float ax = -lz, az = lx;
            const float rise = std::max(0.2f, (L.y - foot) / D);
            const ShadowRegion R = shadow_region(
                cx, cz, foot, foot + card_h, std::max(feet_hx - p.x0, p.x1 + 1 - feet_hx) * kRoot2, lx, lz, rise, 3,
                [&](float far_d) { return std::max(static_cast<float>(kPropMinBlur), soft * far_d); }, h, oy, ky);
            for_region_rows(R, w, ox, oy, kx, ky, [&](int py, int x0, int x1) {
                for (int px = x0; px <= x1; ++px) {
                    const std::size_t q = static_cast<std::size_t>(py) * w + px;
                    const bool under = g_lift_ground.size() == n && g_lift_ground[q];
                    if (g_prop_dist[q] != kUnreached && !under) continue;
                    if (g_covered[q] && g_face.size() == n &&
                        ((g_face[q] == 1 && lx <= 0.0f) || (g_face[q] == 2 && lz <= 0.0f)))
                        continue;
                    const float Y = g_covered[q] ? static_cast<float>(under ? g_pre_y[q] : g_y[q]) : 0.0f;
                    const float dxz = 2.0f * static_cast<float>(px - ox - kx);
                    const float sxz = static_cast<float>(g_covered[q] ? (under ? g_pre_xz[q] : g_xz[q]) : 4 * (py - oy - ky));
                    const float X = 0.5f * (sxz + dxz), Z = 0.5f * (sxz - dxz);
                    const float rx = X - cx, rz = Z - cz;
                    const float d = -(rx * lx + rz * lz);
                    if (d < 0.0f) continue;
                    const float up = Y + d * rise - foot;
                    const float col = (rx * ax + rz * az) / kRoot2;
                    const int tx = static_cast<int>(std::floor(feet_hx + col)) - p.x0;
                    const int ty = static_cast<int>(std::floor(feet_hy - up / 2.0f)) - p.y0;
                    const int rr = std::max(kPropMinBlur, soft > 0.0f ? static_cast<int>(std::lround(soft * d)) : 0);
                    if (up < -2.0f * rr || up > card_h + 2.0f * rr) continue;
                    const int m = box.count(tx - rr, ty - rr, tx + rr, ty + rr);
                    if (m == 0) continue;
                    const std::uint32_t a = static_cast<std::uint32_t>(std::lround(255.0f * lit * m / ((2 * rr + 1) * (2 * rr + 1))));
                    if (a > g_lamp_shade[q]) g_lamp_shade[q] = static_cast<std::uint8_t>(std::min(255u, a));
                }
            });
        }
    }
}

void cast_lamp_shadows(int w, int h, int ox, int oy, int kx, int ky) {
    g_lamp_shade.clear();
    g_lamp_cast_next.clear();
    // (No units: the props' may still fall, kLampPropShadowMaps.)
    if (!units_cast_from_lamps() || g_atlas_bailed) {
        g_lamp_cast_cache.clear();
        return;
    }
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const float soft = unit_shadow_soft();
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    std::uint32_t view_key = g_cast_heights_key;
    kol::fnv_ints(view_key, {w, h, ox, oy, kx, ky});
    // A door light's shadows also pale with the unit's distance from
    // the doorway (its light there weaker), gone at shadow_reach, and
    // along their length, gone shadow_fade units from the unit
    // (asked 2026-10-15 for both to fade with distance).
    // KOL_DOOR_SHADOW_REACH / KOL_DOOR_SHADOW_FADE (mockups; 0 off).
    static const float door_reach = [] {
        const char* e = std::getenv("KOL_DOOR_SHADOW_REACH");
        return e ? static_cast<float>(std::atof(e)) : 224.0f;   // the user's mix of the current value and gentle
    }();
    static const float door_fade = kol::env_num("KOL_DOOR_SHADOW_FADE", 240.0f);
    static const float door_full = [] {   // full strength this near the doorway (KOL_DOOR_SHADOW_FULL)
        const char* e = std::getenv("KOL_DOOR_SHADOW_FULL");
        return e ? static_cast<float>(std::atof(e)) : 64.0f;   // dark on the first two rows
    }();
    // A unit carrying a lamp of its own (map 52: every unit a lantern) lights
    // the shadow it casts from another carried lamp itself: such a shadow at
    // this share of its strength (the user, 2026-11-10, states/thor/0177,
    // objected that each torch-carrying unit cast shadows from the other
    // units' torches, which is not how the light should behave).
    // Fixed lamps' shadows (pots, chandeliers) are kept whole.
    // KOL_CARRIER_SHADOW=<0..1> (1: as before).
    static const float carrier_shadow = kol::env_num_clamp("KOL_CARRIER_SHADOW", 0.0f, 1.0f, 0.3f);
    for (std::size_t bi = 0; bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        if (!c.unit || g_blocks[bi].scenery) continue;   // (a door in its wall: no shadow of its own)
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
        const std::uint32_t unit_key = card_key(view_key, c, b, ax0, ay0, {});
        bool carries = false;
        for (const LampPt& L : g_frame_lamps)
            carries = carries || (L.carrier > 0.0f && std::hypot(L.x - static_cast<float>(c.x), L.z - static_cast<float>(c.z)) < L.carrier);
        for (std::size_t li = 0; li < g_frame_lamps.size(); ++li) {
            const LampPt& L = g_frame_lamps[li];
            const float share = carries && L.carrier > 0.0f ? carrier_shadow : 1.0f;
            if (share <= 0.0f) continue;
            // (From a doorway, its nearest point: the shadows fall straight in.)
            float lxn, lzn;
            door_nearest(L.x, L.z, L.dir_x, L.dir_z, L.half_w, static_cast<float>(c.x), static_cast<float>(c.z), &lxn, &lzn);
            const float dx = lxn - c.x, dz = lzn - c.z;
            const float D = std::hypot(dx, dz);
            const float dy = L.y - static_cast<float>(c.foot + 8 + S);   // to the unit's middle
            const float d3 = std::sqrt(D * D + dy * dy);
            if (D < std::max(4.0f, L.carrier) || d3 >= L.reach) continue;
            // (In a doorway's cone as seen from its nearest point, as the
            // floor and the units' own light are: from its middle, the units
            // toward its ends fell outside the cone and cast none, GY.)
            const float lit0 = std::min(1.0f, L.power * (1.0f - d3 / L.reach)) *
                               light_cone(lxn, lzn, L.dir_x, L.dir_z, L.cos_in, L.cos_out, static_cast<float>(c.x), static_cast<float>(c.z));
            if (lit0 <= 0.02f) continue;
            std::uint32_t key = unit_key;
            for (float v : {L.x, L.y, L.z, L.reach, static_cast<float>(li), L.shadow_k, lxn, lzn})
                kol::fnv_q4(key, v);
            // A door light's shadow takes all of its light (a unit in the
            // doorway's light blocks it); a lamp's, as much as it lights the
            // unit (a far flame casts a faint one). A door light's pales with
            // the distance too (door_reach, door_fade, above).
            const bool door = L.cos_out >= -1.0f;
            if (door && L.shadow_k < 0.0f) continue;   // a light with no shadows (a faint window)
            const float door_lit = door && door_reach > door_full
                                       ? std::clamp(1.0f - (D - door_full) / (door_reach - door_full), 0.0f, 1.0f) : 1.0f;
            if (door && door_lit <= 0.02f) continue;
            g_lamp_cast_next.push_back({key, share * (door ? door_lit : std::min(1.0f, lit0 * L.flick)), {}});
            std::vector<std::uint32_t>& list = g_lamp_cast_next.back().px;
            if (take_cached(g_lamp_cast_cache, key, list)) continue;
            // A door light's shadows are parallel and all as long (daylight
            // through the opening: the slope is the door light's `shadow`);
            // from the flame's height they were short right by the doorway
            // (GY: the row next to it had none to see).
            const float rise = L.cos_out >= -1.0f ? std::max(0.05f, L.shadow_k)
                                                  : std::max(0.2f, (L.y - static_cast<float>(c.foot)) / D);
            // (A door light's longer shadow keeps its edge: the blur grows
            // with the distance, and would wash the long end out.)
            card_shadow(c, b, ax0, ay0, dx / D, dz / D, rise, soft * std::min(1.0f, L.shadow_k), false, w, h, ox, oy, kx, ky, list,
                        L.cos_out >= -1.0f ? door_fade : 0.0f);
        }
    }
    g_lamp_cast_cache.swap(g_lamp_cast_next);
    g_lamp_cast_next.clear();
    // Applied every frame, each shadow at its lamp's strength on the unit
    // now; a pixel keeps the strongest.
    g_lamp_shade.assign(n, 0);
    for (const LampCast& lc : g_lamp_cast_cache)
        for (const std::uint32_t v : lc.px) {
            const std::uint32_t q = v & 0xFFFFFFu;
            const std::uint32_t a = static_cast<std::uint32_t>(std::lround((v >> 24) * lc.lit));
            if (q < n && a > g_lamp_shade[q]) g_lamp_shade[q] = static_cast<std::uint8_t>(a);
        }
    cast_prop_lamp_shadows(w, h, ox, oy, kx, ky, soft);
}
// How dark the logs are drawn (ScenePixels::fire_logs_shade): 1, as the art
// (the user, after D's 0.45). KOL_FIRE_LOGS_SHADE=<0..1>. At 1 nothing reads
// the mask, so it is not kept.
float fire_logs_shade() {
    static const float shade = [] {
        const char* e = std::getenv("KOL_FIRE_LOGS_SHADE");
        return e ? std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.0f) : 1.0f;
    }();
    return shade;
}
}  // namespace kol_focus_detail

namespace {
constexpr int kFireNoAo = 40;
}  // namespace

namespace kol_focus_detail {
void unit_contact_ao(int w, int h, int ox, int oy, int kx, int ky) {
    const float ao_s = ao_strength();
    if (!unit_shadows_on() || ao_s <= 0.0f || g_casters.empty() ||
        g_ao.size() != static_cast<std::size_t>(w) * h) return;
    constexpr float kContactR = 18.0f;
    for (const BlockCaster& c : g_casters) {
        if (!c.unit || c.upper || c.hanging) continue;   // (once a unit; none in the air)
        bool by_fire = false;
        for (const auto& f : g_fire_spots)
            by_fire = by_fire || (std::abs(c.x - f.first) <= kFireNoAo && std::abs(c.z - f.second) <= kFireNoAo);
        if (by_fire) continue;
        // The ground the unit stands on is 8 over `foot` (the sprite's
        // anchor, 4 rows under the cell's ground row, where the blob is); a
        // flying unit's is the ground under it, where the game drew its blob.
        const int ground = c.ground;
        // The circle's box on screen: X - Z within 2R, X + Z within 2R.
        const int cxs = (c.x - c.z) / 2 + kx + ox;
        const int cys = (c.x + c.z) / 4 - ground / 2 + ky + oy;
        const int R = static_cast<int>(kContactR) + 1;
        for (int py = std::max(0, cys - R / 2 - 1); py <= std::min(h - 1, cys + R / 2 + 1); ++py)
            for (int px = std::max(0, cxs - R); px <= std::min(w - 1, cxs + R); ++px) {
                const std::size_t q = static_cast<std::size_t>(py) * w + px;
                float Y;
                if (g_covered[q]) {
                    if (g_face.size() == g_ao.size() && g_face[q] != 0) continue;   // walls
                    Y = static_cast<float>(g_y[q]);
                } else {
                    Y = 0.0f;   // the water, not modelled
                }
                if (std::abs(Y - ground) > 2.0f || g_prop_dist[q] != kUnreached) continue;
                const float dxz = 2.0f * static_cast<float>(px - ox - kx);
                const float sxz = static_cast<float>(g_covered[q] ? g_xz[q] : 4 * (py - oy - ky));
                const float rx = 0.5f * (sxz + dxz) - c.x, rz = 0.5f * (sxz - dxz) - c.z;
                const float r = std::sqrt(rx * rx + rz * rz);
                if (r >= kContactR) continue;
                // Flat near the feet (mostly under the sprite), so the ring
                // around them carries it.
                const float t = 1.0f - r / kContactR;
                const float occ = c.strength * std::min(0.7f, 1.4f * ao_s * std::pow(t, 0.7f));
                g_ao[q] = static_cast<std::uint8_t>(std::lround(g_ao[q] * (1.0f - occ)));
            }
    }
}

// The map's lamps on the units (g_unit_lamps): each unit's own pixels as
// points on a card standing at its place, facing the camera (a pixel right
// on screen is +1 X, -1 Z; a row up 2 units up), lit by distance, and the
// flame's halo by distance on screen.
void unit_lamp_glow(int w, int h, int ox, int oy, int kx, int ky) {
    g_unit_glow_any = false;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (g_unit_lamps.empty() || glow_strength() <= 0.0f || g_lift.size() != n) return;
    g_unit_glow.assign(n, 0);
    g_unit_glow_lamp.assign(n, 0);
    for (std::size_t bi = 0; bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        if (!c.unit) continue;
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        for (int ty = 0; ty < b.h; ++ty)
            for (int tx = 0; tx < b.w; ++tx) {
                const int px = b.sx + tx, py = b.sy + ty;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (g_lift[k] != bi + 1) continue;
                float X, Y, Z;
                card_point(c, px, py, ox, oy, &X, &Y, &Z);
                int total = 0, strongest = 0;
                std::uint8_t lamp = 0;
                for (const UnitLamp& L : g_unit_lamps) {
                    int add = 0;
                    const float d = std::sqrt((L.x - X) * (L.x - X) + (L.y - Y) * (L.y - Y) + (L.z - Z) * (L.z - Z));
                    if (d < L.reach) add += static_cast<int>(std::lround(255.0f * L.power * 0.8f * (1.0f - d / L.reach)));
                    if (L.halo > 0.0f) {
                        const float fx = (L.x - L.z) / 2.0f + ox + kx, fy = (L.x + L.z) / 4.0f - L.y / 2.0f + oy + ky;
                        const float r = std::hypot(px + 0.5f - fx, py + 0.5f - fy) / L.halo;
                        if (r < 1.0f) add += static_cast<int>(std::lround(255.0f * (1.0f - r) * (1.0f - r)));
                    }
                    if (add <= 0) continue;
                    total += add;
                    if (add > strongest) { strongest = add; lamp = static_cast<std::uint8_t>(L.index); }
                }
                if (total <= 0) continue;
                g_unit_glow[k] = static_cast<std::uint8_t>(std::min(255, total));
                g_unit_glow_lamp[k] = lamp;
                g_unit_glow_any = true;
            }
    }
}

// kCoverShades: the hollow under a cover, darker than the sun's shadow leaves it.
void cover_shade(int w, int h, int ox, int oy, int kx, int ky) {
    static const bool off = std::getenv("KOL_NO_COVER_SHADE") != nullptr;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (off || g_ao.size() != n || g_xz.size() != n || g_y.size() != n) return;
    const int map = current_map();
    for (const CoverShade& c : kCoverShades) {
        if (c.map != map) continue;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * w + x;
                if (g_covered.size() == n && !g_covered[k]) continue;
                if (static_cast<float>(g_y[k]) > c.y1) continue;
                const float dxz = 2.0f * static_cast<float>(x - ox - kx);
                const float X = 0.5f * (static_cast<float>(g_xz[k]) + dxz), Z = 0.5f * (static_cast<float>(g_xz[k]) - dxz);
                if (X < c.x0 || X > c.x1 || Z < c.z0 || Z > c.z1) continue;
                float t = std::clamp((Z - c.z0) / (c.z1 - c.z0), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);   // eased at both ends: no edge at the mouth
                g_ao[k] = static_cast<std::uint8_t>(std::lround(g_ao[k] * (c.keep_in + (c.keep_out - c.keep_in) * t)));
            }
    }
}
}  // namespace kol_focus_detail
