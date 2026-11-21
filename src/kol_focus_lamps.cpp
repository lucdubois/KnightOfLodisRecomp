// kol_focus_lamps.cpp — the lamps: the maps' own (kMapLamps, the
// chandeliers, the door lights), the frame's lights gathered from the
// terrain and the units (SceneBuild::gather_lamps) with their sight
// (lamp_seen), and the lamps' and the windows' light on each pixel
// (lamp_and_window_light, glow_pass).

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
// The lamps above and KOL_MAP_LAMP=<map,anchor_x,anchor_z,dx,dy,dz,reach,
// power,halo,hue[,actor]>[;...] (mockups: a lantern carried by a scene's unit).
const std::vector<MapLamp>& map_lamps() {
    static const std::vector<MapLamp> v = [] {
        std::vector<MapLamp> out(std::begin(kMapLamps), std::end(kMapLamps));
        kol::env_items("KOL_MAP_LAMP", [&](const char* p) {
            MapLamp m{};
            int hue = 0, actor = -1;
            if (std::sscanf(p, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%d,%d", &m.map, &m.anchor_x, &m.anchor_z, &m.dx, &m.dy, &m.dz,
                            &m.reach, &m.power, &m.halo, &hue, &actor) >= 10) {
                m.actor = actor;
                // Every unit (actor -2, kEveryActor): every actor id, a torch's flicker.
                if (actor == kEveryActor) { m.actors_below = 0x10000; m.flicker = 0.4f; }
                m.hue = static_cast<std::uint8_t>(hue);
                out.push_back(m);
            }
        });
        return out;
    }();
    return v;
}
// A chandelier's candles stand on a ring, not at a point: its shadows are
// soft, each point lit by the share of the ring it sees (lamp_seen). The
// ring's radius, world units; KOL_CHANDELIER_RING=<r> (0: a point).
// KOL_CHANDELIER_POWER=<k>: every ceiling lamp's power times k (mockups).
float chandelier_power_scale() {
    static const float k = kol::env_num_min("KOL_CHANDELIER_POWER", 0.0f, 1.0f);
    return k;
}
// KOL_CHANDELIER_REACH=<k>: every ceiling lamp's reach times k (mockups).
float chandelier_reach_scale() {
    static const float k = kol::env_num_min("KOL_CHANDELIER_REACH", 0.0f, 1.0f);
    return k;
}
float chandelier_ring() {
    static const float r = [] {
        const char* v = std::getenv("KOL_CHANDELIER_RING");
        return v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 40.0f;   // the user's pick, C (24: B)
    }();
    return r;
}
// Map 71's door (kDoorLights): open from the game's door-open sound (song
// 203) to its door-shut sound (200), eased over a few pictures; 1 on other
// maps (their door lights are always on).
float door_open_71(int map) {
    static unsigned seen_open = 0, seen_shut = 0;
    static float target = 0.0f, now = 0.0f;
    const unsigned o = kol::song_starts(203), c = kol::song_starts(200);
    if (map != 71) {
        seen_open = o; seen_shut = c;
        target = now = 0.0f;
        return 1.0f;
    }
    if (o != seen_open) { seen_open = o; target = 1.0f; }
    if (c != seen_shut) { seen_shut = c; target = 0.0f; }
    now += (target - now) * 0.25f;
    if (std::fabs(target - now) < 0.01f) now = target;
    return now;
}
std::vector<DoorLight> map_door_lights(int map) {
    static const std::vector<DoorLight> env = [] {
        std::vector<DoorLight> v;
        kol::env_items("KOL_DOOR_LIGHT", [&](const char* p) {
            DoorLight d{};
            d.shadow = 1.0f;
            d.hue = 2.0f;
            if (std::sscanf(p, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &d.map, &d.x, &d.y, &d.z, &d.dir_x, &d.dir_z, &d.reach,
                            &d.power, &d.spread, &d.width, &d.shadow, &d.hue) >= 9)
                v.push_back(d);
        });
        return v;
    }();
    std::vector<DoorLight> out;
    static const bool none = std::getenv("KOL_NO_DOOR_LIGHTS") != nullptr;   // (A/B)
    if (none) return out;
    for (const DoorLight& D : kDoorLights)
        if (D.map == map) out.push_back(D);
    for (const DoorLight& D : env)
        if (D.map == map) out.push_back(D);
    return out;
}
}  // namespace

namespace kol_focus_detail {
// The frame's lights (lamps), their sight grids (g_lamp_vis) and keys.
void SceneBuild::gather_lamps() {
    // The torches, as lights: record 70 the ruins' torches (orange), 86 the
    // castle's statue with the blue flame (states/EN_blue_torch.state).
    g_self_lamp_cell.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    if (glow_on) {
        std::uint32_t key = heights_key;
        int top_all = 0;
        for (int c = 0; c < kGridSide * kGridSide; ++c) top_all = std::max(top_all, heights[c]);
        // A lamp in (but the torches): its height raises the lamps' sight
        // grid (top_all), and key_fields go into the key, in their order.
        auto add_lamp = [&](const Lamp& L, std::initializer_list<float> key_fields) {
            top_all = std::max(top_all, static_cast<int>(L.y / 16.0f) + 1);
            lamps.push_back(L);
            for (float v : key_fields) kol::fnv_q4(key, v);
        };
        for (int cy = 0; cy < kGameSide; ++cy)
            for (int cx = 0; cx < kGameSide; ++cx) {
                const int rec = record_of(ew[e(kGrid) + game_cell(cx, cy)]);
                // Torches (70), the castle statue's blue flame (86), and the
                // ruins' blue fire pots (76; asked 2026-10-14 for map 51 to be lit
                // by the blue fire pots). KOL_NO_POT_LAMPS=1 leaves the pots out.
                static const bool no_pots = std::getenv("KOL_NO_POT_LAMPS") != nullptr;
                // Map 56's torch stands (record 79, blue flames: five of them, up
                // the left stair and by the throne) burn as the pots do. Only there
                // for now: the record's other maps were not looked at.
                const bool stand56 = rec == 79 && map == 56;
                if (rec != 70 && rec != 86 && (rec != 76 || no_pots) && !stand56) continue;
                // Map 88's statue is record 86 too, with no flame: it glowed
                // blue (JY, the user pointed out it has no torch).
                // KOL_FLAMELESS_STATUE_LIT=1: lit, as before.
                static const bool flameless_lit = std::getenv("KOL_FLAMELESS_STATUE_LIT") != nullptr;
                if (rec == 86 && !flameless_lit && map == 88) continue;
                const int top = raw_cell(cx, cy, nullptr);
                if (top < 0) continue;
                top_all = std::max(top_all, top);
                // A slow, uneven flicker, each torch its own.
                const float t = static_cast<float>(cap.serial);
                const float flick = 0.9f + 0.06f * std::sin(t * 0.31f + 1.7f * cx) +
                                    0.04f * std::sin(t * 0.83f + 2.3f * cy);
                const std::uint8_t hue = rec == 86 || rec == 76 || stand56 ? 1 : 0;
                // A fire pot's light reaches the map's kDarkMaps pot_reach (x the
                // torches') far; KOL_POT_REACH overrides it.
                static const float env_reach = kol::env_num_min("KOL_POT_REACH", 0.1f, -1.0f);
                const DarkMap* dm = dark_map();
                const float pot_reach = env_reach > 0.0f ? env_reach : dm ? dm->pot_reach : 1.0f;
                // On a dark map the statue's blue flame burns as a fire pot
                // does: its reach, its flicker and its lean (the castle, 55).
                const bool pot = rec == 76 || stand56 || (rec == 86 && dm && indoor_now());
                // There its light comes from higher up, so it clears the
                // statue and the pit's rim and reaches the units behind
                // (asked 2026-10-15). KOL_STATUE_LAMP_RISE=<units>.
                static const float statue_rise = kol::env_num("KOL_STATUE_LAMP_RISE", 32.0f);
                const float rise_y = rec == 86 && pot ? statue_rise : 0.0f;
                lamps.push_back({32.0f * cx + 16.0f, 16.0f * top - 8.0f + rise_y, 32.0f * cy + 16.0f, flick, hue,
                                 glow_reach() * (pot ? pot_reach : 1.0f)});
                // (Map 56's torch stands too: the user, 2026-11-16, asked for the torches
                // themselves to take their own light.)
                if ((rec == 86 || stand56) && pot) g_self_lamp_cell[gi(cx, cy)] = static_cast<int>(lamps.size());
                if (pot) {
                    // A fire pot's flame: a livelier, uneven flicker than a
                    // torch's (three rhythms), and a lean that wanders, so its
                    // pool of light and the shadows from it sway (asked
                    // 2026-10-14 for a slight flicker and movement, like
                    // a fire). KOL_POT_FLICKER=<depth>, KOL_POT_LEAN=<amount>.
                    static const float depth = kol::env_num_min("KOL_POT_FLICKER", 0.0f, 0.08f);
                    static const float lean = kol::env_num_min("KOL_POT_LEAN", 0.0f, kol::kPotLeanDefault);
                    const float p = 1.3f * cx + 2.1f * cy;   // each pot its own phase
                    Lamp& P = lamps.back();
                    P.flick = fire_flicker(t, p, depth);
                    // The lean wanders slowly (KOL_POT_LEAN_SPEED, 0.35: its
                    // first rhythms, a turn in about half a second, drifted
                    // too fast).
                    static const float speed = kol::env_num_min("KOL_POT_LEAN_SPEED", 0.0f, 0.35f);
                    const float tl = t * speed;
                    P.lean_x = lean * (0.7f * std::sin(tl * 0.21f + p) + 0.3f * std::sin(tl * 0.67f + 1.7f * p));
                    P.lean_y = lean * (0.7f * std::sin(tl * 0.17f + 2.3f * p) + 0.3f * std::sin(tl * 0.59f + p));
                }
                kol::fnv_mix(key, static_cast<std::uint32_t>(cy * kGameSide + cx + 1));
                kol::fnv_mix(key, static_cast<std::uint32_t>(rec));
                kol::fnv_mix(key, static_cast<std::uint32_t>(top + 1));
            }
        // Torch light from where no torch is drawn (kPhantomTorches): a blue pot's.
        for (const PhantomTorch& ph : kPhantomTorches) {
            if (ph.map != map) continue;
            const float t = static_cast<float>(cap.serial);
            const DarkMap* dm = dark_map();
            static const float env_reach = kol::env_num_min("KOL_POT_REACH", 0.1f, -1.0f);
            const float pot_reach = env_reach > 0.0f ? env_reach : dm ? dm->pot_reach : 1.0f;
            static const float depth = kol::env_num_min("KOL_POT_FLICKER", 0.0f, 0.08f);
            static const float lean = kol::env_num_min("KOL_POT_LEAN", 0.0f, kol::kPotLeanDefault);
            static const float speed = kol::env_num_min("KOL_POT_LEAN_SPEED", 0.0f, 0.35f);
            const float p = 1.3f * ph.x + 2.1f * ph.z, tl = t * speed;
            Lamp L{32.0f * ph.x + 16.0f, 16.0f * ph.top - 8.0f, 32.0f * ph.z + 16.0f, fire_flicker(t, p, depth),
                   static_cast<std::uint8_t>(kol::kHueBlue), glow_reach() * pot_reach};
            L.lean_x = lean * (0.7f * std::sin(tl * 0.21f + p) + 0.3f * std::sin(tl * 0.67f + 1.7f * p));
            L.lean_y = lean * (0.7f * std::sin(tl * 0.17f + 2.3f * p) + 0.3f * std::sin(tl * 0.59f + p));
            add_lamp(L, {L.x, L.y, L.z, L.reach});
        }
        // The map's own lamps (kMapLamps), on the units that carry them.
        // (None in a window room with Shadows off: window_room_plain.)
        const bool plain_room = window_room_plain();
        for (const MapLamp& m : map_lamps()) {
            if (m.map != map || !map_entry_on(m.map) || plain_room) continue;
            // The units carrying it.
            int carriers[kObjects];
            int carried = 0;
            if (m.actor == kEveryActor) {
                for (int u = 0; u < count; ++u)
                    if (units[u].shown && units[u].actor >= 0 && units[u].actor < m.actors_below && units[u].actor != m.no_actor)
                        carriers[carried++] = u;
            } else {
                int best = -1;
                float best_d = 12.0f;
                for (int u = 0; u < count; ++u) {
                    if (m.actor >= 0) {
                        if (units[u].actor == m.actor) { best = u; break; }
                        continue;
                    }
                    const float d = std::hypot(units[u].x - m.anchor_x, units[u].z - m.anchor_z);
                    if (d <= best_d) { best_d = d; best = u; }
                }
                if (best >= 0 && units[best].shown) carriers[carried++] = best;
            }
            for (int ci = 0; ci < carried; ++ci) {
                const int best = carriers[ci];
                // A candle's flicker: quicker and a little deeper than a torch's
                // (each carrier's out of step with the others').
                // (MapLamp::flicker_rate slows it: map 118's candle.)
                const float t = (static_cast<float>(cap.serial) + 37.0f * static_cast<float>(ci)) * m.flicker_rate;
                const float flick = 1.0f - m.flicker * (0.12f - 0.08f * std::sin(t * 0.9f) - 0.04f * std::sin(t * 2.3f + 1.1f));
                // KOL_LAMP=<reach>,<power>,<halo> overrides them all, for mockups.
                static float env_lamp[3] = {-1.0f, -1.0f, -1.0f};
                static const bool env_on_lamp = [] {
                    const char* v = std::getenv("KOL_LAMP");
                    return v && std::sscanf(v, "%f,%f,%f", &env_lamp[0], &env_lamp[1], &env_lamp[2]) == 3;
                }();
                Lamp L{units[best].x + m.dx, units[best].y + m.dy, units[best].z + m.dz, flick, m.hue,
                       env_on_lamp ? env_lamp[0] : m.reach, env_on_lamp ? env_lamp[1] : m.power,
                       env_on_lamp ? env_lamp[2] : m.halo, true};
                L.feather = m.feather;
                add_lamp(L, {L.x, L.y, L.z, L.reach, L.power, L.halo, L.feather});
            }
        }
        // A chandelier (kCeilingLamps): a point lamp, a candle's flicker.
        {
            static float env_c[6] = {0, 0, 0, 0, 0, 0};
            static const bool env_on_c = [] {
                const char* v = std::getenv("KOL_CHANDELIER");
                return v && std::sscanf(v, "%f,%f,%f,%f,%f,%f", &env_c[0], &env_c[1], &env_c[2], &env_c[3], &env_c[4],
                                        &env_c[5]) == 6;
            }();
            for (const CeilingLamp& c : kCeilingLamps) {
                if (c.map != map || plain_room || (c.flame_shown && !cap.bg1_flame)) continue;
                const float t = static_cast<float>(cap.serial);
                // A fire (map 63's camp, the user asked for less flicker and
                // shadows dancing a little, as on the cave map): the
                // cave's pots' flicker at 0.05 deep (theirs 0.08), their lean
                // (0.06, wandering at 0.35), and its place swaying by up to
                // kFireSway for the units' shadows (3, then 1.5: the user
                // asked for a gentler sway).
                constexpr float kFireDepth = 0.05f, kFireLean = 0.06f, kFireLeanSpeed = 0.35f, kFireSway = 1.5f;
                const float flick = c.steady ? 1.0f
                                    : c.fire ? fire_flicker(t, 0.0f, kFireDepth)
                                             : 0.96f + 0.025f * std::sin(t * 0.7f) + 0.015f * std::sin(t * 1.9f + 0.6f);
                Lamp L{env_on_c ? env_c[0] : c.x, env_on_c ? env_c[1] : c.y, env_on_c ? env_c[2] : c.z, flick,
                       static_cast<std::uint8_t>(std::clamp(env_on_c ? static_cast<int>(env_c[5]) : c.hue, 0, kol::kHueCount - 1)),
                       (env_on_c ? env_c[3] : c.reach) * chandelier_reach_scale(),
                       (env_on_c ? env_c[4] : c.power) * chandelier_power_scale(), 0.0f, false};
                // KOL_CEIL_RING_ALL=<r>: every ceiling lamp's ring (mockups).
                static const float ring_all = [] {
                    const char* v = std::getenv("KOL_CEIL_RING_ALL");
                    return v ? std::max(0.0f, static_cast<float>(std::atof(v))) : -1.0f;
                }();
                L.ring = ring_all >= 0.0f ? ring_all : c.ring >= 0.0f ? c.ring : chandelier_ring();
                L.fire = c.fire;
                // Map 71's lamp is the window's light bounced (the user asked
                // for the bounce light's brightness to change too): it follows the
                // shutter. Through the flicker, applied each picture, so the
                // cached light is not rebuilt.
                // A lightning flash: the bounce up to 4 times the open shutter's.
                if (map == 71) L.flick *= g_shutter_shown + 3.0f * g_lightning;
                // KOL_FIRE_LAMP=<power,hue>: a fire's light (mockups).
                static float fl[2] = {0.0f, 0.0f};
                static const bool fl_on = [] {
                    const char* e = std::getenv("KOL_FIRE_LAMP");
                    return e && std::sscanf(e, "%f,%f", &fl[0], &fl[1]) == 2;
                }();
                if (c.fire && fl_on) {
                    L.power = fl[0];
                    L.hue = static_cast<std::uint8_t>(std::clamp(static_cast<int>(fl[1]), 0, kol::kHueCount - 1));
                }
                if (c.fire && !c.steady) {
                    const float tl = t * kFireLeanSpeed;
                    const float wx = 0.7f * std::sin(tl * 0.21f) + 0.3f * std::sin(tl * 0.67f + 1.7f);
                    const float wz = 0.7f * std::sin(tl * 0.17f + 2.3f) + 0.3f * std::sin(tl * 0.59f);
                    L.lean_x = kFireLean * wx;
                    L.lean_y = kFireLean * wz;
                    L.wob_x = kFireSway * wx;
                    L.wob_z = kFireSway * wz;
                }
                add_lamp(L, {L.x, L.y, L.z, L.reach, L.power, static_cast<float>(L.hue), L.ring});
            }
        }
        // KOL_CEIL_LAMPS=<x,y,z,reach,power,hue,steady,ring>[;...]: more
        // fixed lamps on the map now (mockups: map 63's fire and moon).
        {
            static const std::vector<std::array<float, 8>> extra = [] {
                std::vector<std::array<float, 8>> v;
                kol::env_items("KOL_CEIL_LAMPS", [&](const char* p) {
                    std::array<float, 8> a{};
                    if (std::sscanf(p, "%f,%f,%f,%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) == 8)
                        v.push_back(a);
                });
                return v;
            }();
            for (const auto& a : extra) {
                const float t = static_cast<float>(cap.serial);
                const float flick = a[6] > 0.5f ? 1.0f : 0.92f + 0.05f * std::sin(t * 0.9f) + 0.03f * std::sin(t * 2.3f + 0.6f);
                Lamp L{a[0], a[1], a[2], flick, static_cast<std::uint8_t>(std::clamp(static_cast<int>(a[5]), 0, kol::kHueCount - 1)), a[3], a[4],
                       0.0f, false};
                L.ring = a[7];
                add_lamp(L, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
            }
        }
        // Daylight in through a door out of sight (kDoorLights): a light
        // in a cone, steady, warm white (hue 2).
        const float door_open = door_open_71(map);
        for (const DoorLight& D : plain_room ? std::vector<DoorLight>{} : map_door_lights(map)) {
            // (Map 71's opens and shuts: through the flicker, applied each
            // picture, so the cached light is not rebuilt.)
            Lamp L{D.x, D.y, D.z, door_open, static_cast<std::uint8_t>(std::clamp(static_cast<int>(D.hue), 0, 3)), D.reach, D.power, 0.0f, false};
            const float n = std::max(1e-3f, std::hypot(D.dir_x, D.dir_z));
            L.dir_x = D.dir_x / n;
            L.dir_z = D.dir_z / n;
            L.cos_out = std::cos(D.spread * 3.14159265f / 180.0f);
            L.cos_in = std::cos(D.spread * 0.45f * 3.14159265f / 180.0f);
            L.half_w = 0.5f * D.width;
            L.shadow_k = D.shadow > 0.0f ? D.shadow : D.shadow < 0.0f ? -1.0f : 0.3f;   // (the slope, see cast_lamp_shadows; < 0 none)
            add_lamp(L, {L.x, L.y, L.z, L.reach, L.power, D.dir_x, D.dir_z, D.spread, D.width, D.shadow, D.hue});
        }
        kol::fnv_mix(key, static_cast<std::uint32_t>(glow_reach()));
        // The sight grid up to the map's model's top too: above the grid's
        // heights and the lamps every point counted as seeing the lamp, and
        // map 83's stairwell walls and newel over Y 144 took the torches'
        // light from below through the treads (the reviewer, 2026-11-03).
        // KOL_LAMP_GRID_BY_HEIGHTS=1: the grid's heights and lamps only.
        static const bool by_heights = std::getenv("KOL_LAMP_GRID_BY_HEIGHTS") != nullptr;
        if (g_occ && !by_heights) top_all = std::max(top_all, (g_occ->iy0 + g_occ->ny) / 4 + 1);
        if (key != g_lamp_key || g_lamp_vis.size() != lamps.size()) {
            g_lamp_key = key;
            // A lamp that stands where it stood keeps its grid: what a cube
            // sees of it depends on the lamp's place, the map's heights and
            // its model alone (lamp_seen), and on map 58 at night, where
            // every character carries a torch, one of them walking emptied
            // every other's grid each step, all of them filled again by the
            // light's rebuild. Not a lamp with a ring: a cube's first read
            // gives its exact share and the later ones the byte's (lamp_seen's
            // note), so a kept grid would light differently from a fresh one.
            // KOL_LAMP_VIS_KEEP=0: every grid emptied, as before.
            // KOL_LAMP_VIS_CHECK=1: each kept cube worked out again and compared.
            static const bool keep_on = kol::env_not_zero("KOL_LAMP_VIS_KEEP");
            std::vector<LampVis> old;
            old.swap(g_lamp_vis);
            g_lamp_vis.assign(lamps.size(), LampVis{});
            std::uint32_t base = heights_key;
            kol::fnv_mix(base, static_cast<std::uint32_t>(map));
            kol::fnv_mix(base, static_cast<std::uint32_t>(top_all));
            kol::fnv_mix(base, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_occ)));
            for (std::size_t i = 0; i < lamps.size(); ++i) {
                const int r4 = static_cast<int>(std::ceil(lamps[i].reach / 4.0f)) + 1;
                LampVis& v = g_lamp_vis[i];
                v.ix0 = static_cast<int>(std::floor(lamps[i].x / 4.0f)) - r4;
                v.iz0 = static_cast<int>(std::floor(lamps[i].z / 4.0f)) - r4;
                v.nx = v.nz = 2 * r4 + 1;
                v.ny = 4 * (top_all + 2);
                v.base = base;
                const float at[4] = {lamps[i].x, lamps[i].y, lamps[i].z, lamps[i].reach};
                std::memcpy(v.at, at, sizeof at);
                const std::size_t n = static_cast<std::size_t>(v.nx) * v.nz * v.ny;
                if (keep_on && !(lamps[i].ring > 0.0f))
                    for (LampVis& o : old)
                        if (o.vis.size() == n && o.base == base && std::memcmp(o.at, at, sizeof at) == 0 && o.ix0 == v.ix0 &&
                            o.iz0 == v.iz0 && o.nx == v.nx && o.ny == v.ny) {
                            v.vis.swap(o.vis);
                            o.vis.clear();
                            v.kept = true;
                            break;
                        }
                if (!v.kept) v.vis.assign(n, 0);
            }
        }
    }
    g_scene_lamps.clear();
    for (const Lamp& L : lamps)
        g_scene_lamps.push_back({L.x, L.y, L.z, L.reach, L.power, L.flick, static_cast<int>(L.hue)});
    g_frame_lamps.clear();
    for (const Lamp& L : lamps)   // (at rest: a lean of 0.06 would move a shadow a fifth of a pixel)
        // (A map's lamps too, carried by its units: without them map 58's
        // people at night, lit by nothing else, cast no shadow and kept the
        // game's dark blob, the user, 2026-10-26.)
        g_frame_lamps.push_back({L.x + L.wob_x, L.y, L.z + L.wob_z, L.reach, L.power, L.flick, L.dir_x, L.dir_z, L.cos_in, L.cos_out, L.half_w, L.shadow_k,
                                 L.units ? 16.0f : 0.0f});
    if (lamps.empty()) {
        g_glow_any = false;
        g_glow_view_key = 0;
    } else {
        std::uint32_t vk = g_lamp_key;
        kol::fnv_ints(vk, {ox, oy, kx, ky, w, h}, 4096);
        // And which pixels of the scene show the backdrop, sprites left out
        // (the compositor's own layer codes, Capture::codes): a hole in the
        // map's art is no surface, so its light (and where its pixels lie)
        // follow it. A tent's door on map 63 is a hole in BG1 while open
        // (the backdrop shows through): it opened while Ivanna came out
        // (states/thor/0039, ~frame 30) and closed with the camera still,
        // by its tiles' graphics (the tilemap stays as it is), and its
        // light, cached with the doorway taken as the sky, stayed dark (the
        // user reported the door only closed behind her once the
        // camera moved). Only the backdrop's pixels: the game's firelight on BG1
        // flickers between the layers every few frames, and keyed on every
        // layer the light was rebuilt as often (units walking, the stream's
        // tiles, the dialogue's box change nothing). KOL_GLOW_LAYER_KEY=0:
        // the view alone, as before.
        static const bool layer_key = kol::env_not_zero("KOL_GLOW_LAYER_KEY");
        const std::size_t npx = static_cast<std::size_t>(w) * h;
        if (layer_key && cap.under_ok && cap.codes.size() == npx) {
            // Inside the view only: past it, the side margins are composed
            // only on the rows the camera's tilt needs, the rest left as
            // backdrop, so with the stick moving the key changed every frame
            // and the light was built again each time (Thor state 0302, map
            // 56: 8 -> 15 ms on the Mac). KOL_GLOW_KEY_WHOLE=1: every pixel.
            static const bool whole = std::getenv("KOL_GLOW_KEY_WHOLE") != nullptr;
            int vx = 0, vy = 0, vw = w, vh = h;   // the view in the scene, as frame_view finds it
            const int* v = cap.view;
            if (!whole && v[2] >= 240 && v[3] >= 160 && v[0] >= 0 && v[1] >= 0 && v[0] + v[2] <= w &&
                v[1] + v[3] <= h) {
                vx = v[0]; vy = v[1]; vw = v[2]; vh = v[3];
            }
            const int x0 = std::clamp(vx, 0, w), x1 = std::clamp(vx + vw, 0, w);
            const int y0 = std::clamp(vy, 0, h), y1 = std::clamp(vy + vh, 0, h);
            std::uint32_t lk = kol::kFnvBasis;
            const std::uint8_t* own = cap.codes.data();
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    kol::fnv_mix(lk, (own[static_cast<std::size_t>(y) * w + x] & 0x0Fu) == kol::kLayerBackdrop ? 0x5Bu : 0u);
            kol::fnv_mix(vk, lk);
        }
        glow_dirty = vk != g_glow_view_key || g_glow.size() != static_cast<std::size_t>(w) * h;
        g_glow_view_key = vk;
        for (std::size_t i = 0; i < lamps.size() && i < static_cast<std::size_t>(kMaxLamps); ++i) {
            g_glow_flick[i] = lamps[i].flick;
            g_glow_hue[i] = lamps[i].hue;
            g_glow_fx[i] = (lamps[i].x - lamps[i].z) / 2.0f + ox + kx;
            g_glow_fy[i] = (lamps[i].x + lamps[i].z) / 4.0f - lamps[i].y / 2.0f + oy + ky;
            // The lean on screen: X - Z across, (X + Z) / 2 down.
            g_glow_lx[i] = lamps[i].lean_x - lamps[i].lean_y;
            g_glow_ly[i] = 0.5f * (lamps[i].lean_x + lamps[i].lean_y);
        }
    }
    // The world places are written when the torchlight's are (its view key)
    // or, without lamps, when the godrays' view key changes.
    world_dirty = glow_dirty;
    {
        static std::uint32_t win_view_key = 0;
        if (godrays_on && lamps.empty()) {
            std::uint32_t vk = 0x51ED270Bu;
            kol::fnv_ints(vk, {ox, oy, kx, ky, w, h}, 4096);
            world_dirty = vk != win_view_key || g_wx.size() != static_cast<std::size_t>(w) * h;
            win_view_key = vk;
        } else {
            win_view_key = 0;
        }
    }
    // The model's own faces stay out of a lamp's rays for their first
    // kOccluderSkip units; on map 63 for 12. The tents' roofs slope, and a
    // ray from a roof cube toward the fire ran on through the roof's own
    // cubes past 6: the faces turned to the fire came out in speckles along
    // the eaves (the user found the shadows on the two front tents' tops
    // looked off). KOL_LAMP_SKIP=<units> sets it on any map.
    static const float env_skip = kol::env_num("KOL_LAMP_SKIP", -1.0f);
    lamp_skip = env_skip >= 0.0f ? env_skip : (map == 63 || map == 64) ? 12.0f : kOccluderSkip;
    // KOL_LAMP_VIS_CHECK=1: the kept grids' cubes against a fresh answer.
    static const bool vis_check = kol::env_true("KOL_LAMP_VIS_CHECK");
    static const bool vis_log = kol::env_true("KOL_LAMP_VIS_LOG");
    if ((vis_check || vis_log) && glow_on) {
        static long kept = 0, fresh = 0, cubes = 0, bad = 0, pictures = 0;
        for (std::size_t i = 0; i < g_lamp_vis.size() && i < lamps.size(); ++i) {
            LampVis& v = g_lamp_vis[i];
            if (!v.kept) continue;
            if (vis_check)
                for (int iy = 0; iy < v.ny; ++iy)
                    for (int iz = 0; iz < v.nz; ++iz)
                        for (int ix = 0; ix < v.nx; ++ix) {
                            const std::uint8_t m = v.vis[(static_cast<std::size_t>(iy) * v.nz + iz) * v.nx + ix];
                            if (!m) continue;
                            ++cubes;
                            const float seen = lamp_seen_cube(i, ix, iy, iz);
                            if (m != static_cast<std::uint8_t>(1 + std::lround(254.0f * seen)) || (m != 1 && m != 255)) ++bad;
                        }
        }
        for (LampVis& v : g_lamp_vis) {
            (v.kept ? kept : fresh) += 1;
            v.kept = false;   // (counted once: the grids stay while the key does)
        }
        if (++pictures % 300 == 0)
            std::fprintf(stderr, "[lamp-vis] %ld pictures: %ld grids kept, %ld emptied; check %ld cubes, %ld bad\n", pictures,
                         kept, fresh, cubes, bad);
    }
}

// How much of lamp `i` the 4-unit cube holding world point (X, Y, Z)
// sees, 0..1: whether the line from the cube's centre to the flame passes
// under a cell's top, other than the cube's own cell and the torch's, or
// through the map model's faces (model_occluder). A lamp with a ring
// (a chandelier) is its centre and 8 flames round it, the share seen.
// Worked out once a cube.
float SceneBuild::lamp_seen(std::size_t i, float X, float Y, float Z) {
    LampVis& v = g_lamp_vis[i];
    const int ix = static_cast<int>(std::floor(X / 4.0f)) - v.ix0;
    const int iz = static_cast<int>(std::floor(Z / 4.0f)) - v.iz0;
    const int iy = static_cast<int>(std::floor(Y / 4.0f));
    if (ix < 0 || iz < 0 || iy < 0 || ix >= v.nx || iz >= v.nz || iy >= v.ny) return 1.0f;
    std::uint8_t& m = v.vis[(static_cast<std::size_t>(iy) * v.nz + iz) * v.nx + ix];
    if (m) return static_cast<float>(m - 1) / 254.0f;
    const float seen = lamp_seen_cube(i, ix, iy, iz);
    m = static_cast<std::uint8_t>(1 + std::lround(254.0f * seen));
    // (The read that works a cube out returns the exact share, every later
    // one the byte's: with a ring, a share between 0 and 1, a pixel's light
    // then depends on whether it was the first to ask about its cube, that
    // is on the order the pixels are taken in. So a map with a ring lamp
    // keeps its lamps' rows on one thread: lamp_and_window_light.)
    return seen;
}

// lamp_seen's answer for cube (ix, iy, iz) of lamp i's grid, worked out.
float SceneBuild::lamp_seen_cube(std::size_t i, int ix, int iy, int iz) const {
    const LampVis& v = g_lamp_vis[i];
    const Lamp& L = lamps[i];
    const float px = 4.0f * (ix + v.ix0) + 2.0f, pz = 4.0f * (iz + v.iz0) + 2.0f, py = 4.0f * iy + 2.0f;
    const int own_x = static_cast<int>(std::floor(px / 32.0f)), own_z = static_cast<int>(std::floor(pz / 32.0f));
    bool model_only = false;
    if (g_occ)
        for (const int m : kLampModelOnlyMaps)
            model_only = model_only || m == current_map();
    auto hidden_from = [&](float fx, float fy, float fz) {
        const int lcx = static_cast<int>(std::floor(fx / 32.0f)), lcz = static_cast<int>(std::floor(fz / 32.0f));
        const float dx = fx - px, dy = fy - py, dz = fz - pz;
        const int steps = static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 4.0f);
        for (int s = 1; s < steps; ++s) {
            const float f = static_cast<float>(s) / steps;
            const int qx = static_cast<int>(std::floor((px + dx * f) / 32.0f));
            const int qz = static_cast<int>(std::floor((pz + dz * f) / 32.0f));
            if (!in_grid(qx, qz)) continue;
            if (g_occ && 4.0f * s > lamp_skip && 4.0f * (steps - s) > kOccluderSkip &&
                g_occ->solid_at(px + dx * f, py + dy * f, pz + dz * f))
                return true;
            if ((qx == own_x && qz == own_z) || (qx == lcx && qz == lcz)) continue;
            // (kLampModelOnlyMaps: the model alone stops its lamps.)
            if (model_only) continue;
            const int hq = g_occ ? g_occ->height(qx, qz, [&](int x, int z) { return heights[gi(x, z)]; })
                                 : heights[gi(qx, qz)];
            if (16.0f * hq > py + dy * f) return true;
        }
        return false;
    };
    float seen = hidden_from(L.x, L.y, L.z) ? 0.0f : 1.0f;
    if (L.ring > 0.0f) {
        for (int a = 0; a < 8; ++a) {
            const float t = 0.78539816f * a;
            if (!hidden_from(L.x + L.ring * std::cos(t), L.y, L.z + L.ring * std::sin(t))) seen += 1.0f;
        }
        seen /= 9.0f;
    }
    return seen;
}

// The lamps' and the windows' light on each pixel's world place (called
// by glow_pass, or after the model on a map with an occluder: model_pass).
void SceneBuild::lamp_and_window_light() {
    // The torchlight, on the pixels the pass left, within each torch's reach
    // on screen. Pixels it did not draw are the map's water (height 0, no
    // unit may enter it, so not modelled) at the torches' feet: ground at
    // height 0 where the screen puts it.
    if (glow_dirty) {
        g_glow_any = false;
        g_glow.assign(static_cast<std::size_t>(w) * h, 0);
        g_glow_lamp.assign(static_cast<std::size_t>(w) * h, 0);
        std::vector<std::uint8_t> best(static_cast<std::size_t>(w) * h, 0);
        // Each hue's light on each pixel, for the lamps' colours to mix where two reach
        // (g_glow_mix): a pixel took its brightest lamp's colour alone, and a warm
        // chandelier's light met the blue torches' along a line (map 56, the user expected
        // the torches' blue and the chandelier's warmth to blend more). KOL_LAMP_NO_MIX=1: as before.
        static const bool no_mix = kol::env_set("KOL_LAMP_NO_MIX");
        // (This thread's buffer, through a reference: a row's helper thread
        // naming a thread_local reaches its own, empty one, and the rows it
        // took had no hue sums. Found by KOL_LAMP_PAR_CHECK.)
        static thread_local std::vector<std::uint8_t> hue_store;
        std::vector<std::uint8_t>& hue_sum = hue_store;
        hue_sum.assign(no_mix ? 0 : static_cast<std::size_t>(w) * h * kol::kHueCount, 0);
        // Lamp i's light on pixel k: added to g_glow (to_glow), and the
        // pixel's brightest lamp kept (g_glow_lamp).
        std::atomic<bool> glow_any{false};
        auto glow_add = [&](std::size_t k, long add, std::size_t i, bool to_glow) {
            if (to_glow) g_glow[k] = static_cast<std::uint8_t>(std::min(255L, g_glow[k] + add));
            if (!hue_sum.empty() && add > 0 && i < lamps.size()) {
                std::uint8_t& hs = hue_sum[k * kol::kHueCount + std::min<std::size_t>(lamps[i].hue, kol::kHueCount - 1)];
                hs = static_cast<std::uint8_t>(std::min(255L, hs + add));
            }
            if (add > best[k]) {
                best[k] = static_cast<std::uint8_t>(std::min(255L, add));
                g_glow_lamp[k] = static_cast<std::uint8_t>(std::min(i, static_cast<std::size_t>(kMaxLamps - 1)));
            }
            glow_any.store(true, std::memory_order_relaxed);
        };
        // On a ship lit by its model (ship_light_map), the shadows its faces
        // cast from the torches softened on the screen (the user, 2026-10-26,
        // asked for softer torch-cast environment shadows; a ring of flames per torch did it at 9 times the rays,
        // 6 ms): each pixel's light both as lit (lit_f) and as if nothing
        // stood in the way (full_f); the share that gets through is blurred
        // over `soft` pixels each way among pixels at about its place, then
        // multiplies the full light. KOL_TORCH_SOFT=<px> (0: hard).
        static const int torch_soft = kol::env_int_clamp("KOL_TORCH_SOFT", 0, 8, 3);
        const int soft = ship_light_map(map) ? torch_soft : 0;
        static const bool coarse_face = std::getenv("KOL_LAMP_COARSE_FACE") != nullptr;
        const bool round_light = !coarse_face && kol::map_flag(kol::MapFlag::RoundLight, map);
        static const bool fx_light_card = std::getenv("KOL_FX_ART_LIGHT_CARD") != nullptr;
        const std::size_t npx_l = static_cast<std::size_t>(w) * h;
        const bool fx_light_wall = !fx_light_card && g_modeled.size() == npx_l && g_back_kind.size() == npx_l &&
                                   g_back_xz.size() == npx_l && g_back_y.size() == npx_l;
        const int fx_light_map = map;
        static std::vector<float> lit_f, full_f, share, share_tmp;
        if (soft) {
            lit_f.assign(static_cast<std::size_t>(w) * h, 0.0f);
            full_f.assign(static_cast<std::size_t>(w) * h, 0.0f);
        }
        // Each lamp's pixels over the row pool (2026-11-18: with the camera
        // panning this is worked out again every picture, 1.7 ms a job on the
        // Mac's map 55). The lamps stay in order, so each pixel still takes
        // them in order (its brightest lamp, the first of equals); a pixel's
        // sums are its own. What a point's read fills as it goes: lamp_seen's
        // cubes (g_lamp_vis: each a byte that depends on the cube alone, so
        // two rows that meet one write the same value; with no ring lamp, 0
        // or 1 whoever asks first) and
        // model_part_fx_art's table for the map (asked once here first). A
        // helper takes this thread's t_present for the run.
        // KOL_LAMP_SERIAL=1: on this thread alone, as before.
        // KOL_LAMP_PAR_CHECK=1: both ways, compared.
        static const bool lamp_serial = kol::env_set("KOL_LAMP_SERIAL");
        static const bool lamp_check = kol::env_set("KOL_LAMP_PAR_CHECK");
        (void)model_part_fx_art(fx_light_map, 0);
        const bool present = t_present;
        auto lamps_loop = [&](bool split) {
        for (std::size_t i = 0; i < lamps.size(); ++i) {
            const Lamp& L = lamps[i];
            const float reach = L.reach;
            const int R = static_cast<int>(std::ceil(reach)) + 2;
            // The flame's place on screen; everything in reach lies within
            // the reach of it either way (sx = (X - Z) / 2, sy = (X + Z) / 4
            // - Y / 2).
            const int cxs = static_cast<int>((L.x - L.z) / 2.0f) + ox + kx;
            const int cys = static_cast<int>((L.x + L.z) / 4.0f - L.y / 2.0f) + oy + ky;
            const int x0 = std::max(0, cxs - R), x1 = std::min(w - 1, cxs + R);
            const int y0 = std::max(0, cys - R), y1 = std::min(h - 1, cys + R);
            auto lamp_rows = [&](int row0, int row1) {
            for (int py = row0; py < row1; ++py)
                for (int px = x0; px <= x1; ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    float X, Y, Z, nx = 0.0f, ny = 0.0f, nz = 0.0f;
                    bool round_n = false;
                    if (g_covered[k]) {
                        X = g_wx[k];
                        Z = g_wz[k];
                        Y = static_cast<float>(g_y[k]);
                        // An art flame's card (kModelFxArt) at the wall behind
                        // it: 7.6 out, a torch's lamp only just in front of it
                        // lit it edge-on, a third of the wall's light, and the
                        // card's box stood dark round the flame on map 66 (the
                        // user saw a dark outline round the flames, with or
                        // without the effect). KOL_FX_ART_LIGHT_CARD=1: at the card.
                        if (fx_light_wall && g_modeled[k] && g_back_kind[k] != kol::kSceneNone &&
                            model_part_fx_art(fx_light_map, g_modeled[k] - 1)) {
                            const float a = 2.0f * static_cast<float>(px - ox - kx);   // X - Z
                            const float b = static_cast<float>(g_back_xz[k]);          // X + Z
                            X = 0.5f * (a + b);
                            Z = 0.5f * (b - a);
                            Y = static_cast<float>(g_back_y[k]);
                        }
                        // A wall's point a little out in front of it, in the
                        // cell it faces. On a round_light map a model's
                        // upright pixel by its own normal (round_n): on map
                        // 83's newel, by the +X / +Z face, the torches'
                        // light split down the column in two flat halves
                        // (the user reported the central post's shadow split
                        // vertically instead of grading).
                        // KOL_LAMP_COARSE_FACE=1: by the face, as before.
                        // Only a surface turned between the axes (a round
                        // one): a square wall's face is its normal anyway,
                        // and the torches' flame cards, 3 in front of the
                        // wall, would stand past their own lamp.
                        const bool has_n = round_light && g_face[k] != 0 &&
                                           g_model_n.size() == 3 * static_cast<std::size_t>(w) * h && g_modeled[k];
                        // A rounded box's wall (the terrain's own: g_round_n) on any map,
                        // the same way: by its face a round tower took the lamps' light in
                        // two flat halves, cut down its middle (map 56's hall, the user reported
                        // the rounded walls lit as if flat, their shadows cutting off
                        // abruptly).
                        // KOL_ROUND_TERRAIN_FACE=1: by the face, as before.
                        static const bool terrain_face = kol::env_set("KOL_ROUND_TERRAIN_FACE");
                        const bool terr_n = !terrain_face && !coarse_face && g_face[k] != 0 && !(has_n) &&
                                            g_round_n.size() == 2 * static_cast<std::size_t>(w) * h &&
                                            std::abs(g_round_n[k * 2]) > 10 && std::abs(g_round_n[k * 2 + 1]) > 10;
                        if (terr_n || (has_n && std::abs(g_model_n[k * 3]) > 10 && std::abs(g_model_n[k * 3 + 2]) > 10)) {
                            round_n = true;
                            nx = terr_n ? g_round_n[k * 2] / 127.0f : g_model_n[k * 3] / 127.0f;
                            ny = terr_n ? 0.0f : g_model_n[k * 3 + 1] / 127.0f;
                            nz = terr_n ? g_round_n[k * 2 + 1] / 127.0f : g_model_n[k * 3 + 2] / 127.0f;
                            // Its sight of the lamp from round_out along the
                            // normal: from half a unit, a grazing line ran
                            // through the column's own 4-unit cubes past the
                            // skip and cut the light off in a hard vertical
                            // line before the surface turned away.
                            // KOL_ROUND_OUT=<units> (mockups).
                            static const float round_out = kol::env_num_clamp("KOL_ROUND_OUT", 0.0f, 16.0f, 4.0f);
                            X += round_out * nx;
                            Y += round_out * ny;
                            Z += round_out * nz;
                        }
                        else if (g_face[k] == 1) { nx = 1.0f; X += 0.5f; }
                        else if (g_face[k] == 2) { nz = 1.0f; Z += 0.5f; }
                        else ny = 1.0f;
                    } else {
                        const float a = 2.0f * static_cast<float>(px - ox - kx);   // X - Z
                        const float b = 4.0f * static_cast<float>(py - oy - ky);   // X + Z
                        X = 0.5f * (a + b);
                        Z = 0.5f * (b - a);
                        Y = 0.0f;
                        ny = 1.0f;
                        if (X < kFieldOrigin || Z < kFieldOrigin || X >= 32.0f * (kGameSide + kMargin) ||
                            Z >= 32.0f * (kGameSide + kMargin))
                            continue;
                    }
                    float lx, lz;
                    door_nearest(L.x, L.z, L.dir_x, L.dir_z, L.half_w, X, Z, &lx, &lz);
                    const float dx = lx - X, dy = L.y - Y, dz = lz - Z;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= reach * reach) continue;
                    const float d = std::sqrt(d2) + 1e-3f;
                    float facing = (nx * dx + ny * dy + nz * dz) / d;
                    if (facing <= 0.0f) continue;
                    // By its own normal, the light fades in from the side
                    // turned away (0.35 of it at once made a hard line where
                    // the surface turns toward the lamp; kRoundFade of the
                    // way, the rest as before).
                    constexpr float kRoundFade = 0.35f;
                    const float fade_in = round_n ? std::min(1.0f, facing / kRoundFade) : 1.0f;
                    const float seen = lamp_seen(i, X, Y, Z);
                    if (seen <= 0.0f && !soft) continue;
                    // Linear, so the water six steps below the ruins' flames
                    // still takes some (squared, it took next to nothing).
                    const float fall = lamp_fall(L.cos_out, d, reach, L.feather);
                    const float full = 255.0f * L.power * fall * (0.35f + 0.65f * facing) * fade_in *
                                       light_cone(lx, lz, L.dir_x, L.dir_z, L.cos_in, L.cos_out, X, Z);
                    if (soft && full > 0.0f) {
                        full_f[k] += full;
                        lit_f[k] += full * seen;
                        glow_any.store(true, std::memory_order_relaxed);
                    }
                    const long add = std::lround(full * seen);
                    if (add <= 0) continue;
                    glow_add(k, add, i, !soft);
                }
            };
            if (y1 >= y0 && x1 >= x0) {
                if (!split) lamp_rows(y0, y1 + 1);
                else
                    kol::parallel_rows(y1 - y0 + 1, 4, [&](int, int r0, int r1) {
                        const bool was = t_present;
                        t_present = present;
                        lamp_rows(y0 + r0, y0 + r1);
                        t_present = was;
                    });
            }
            // Its glow on screen: a soft halo round the flame, on whatever
            // the map shows there (sprites take theirs with the units' light).
            if (L.halo > 0.0f) {
                const float fx = (L.x - L.z) / 2.0f + ox + kx, fy = (L.x + L.z) / 4.0f - L.y / 2.0f + oy + ky;
                kol::for_halo(fx, fy, L.halo, w, h, 0.5f, [&](int px, int py, float r) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    glow_add(k, std::lround(255.0f * (1.0f - r) * (1.0f - r)), i, true);
                });
            }
        }
        };   // lamps_loop
        // Not with a ring lamp on the map (lamp_seen's note): its light would
        // differ by the rows' order, a 254th of a share on a few pixels.
        bool any_ring = false;
        for (const Lamp& L : lamps) any_ring = any_ring || L.ring > 0.0f;
        const bool lamp_split = !lamp_serial && !any_ring;
        lamps_loop(lamp_split);
        if (lamp_check && lamp_split) {
            // The same again on this thread alone, into the same buffers set
            // back to their start, against what the split left.
            const std::vector<std::uint8_t> glow_p = g_glow, lamp_p = g_glow_lamp, best_p = best, hue_p = hue_sum;
            const std::vector<float> lit_p = lit_f, full_p = full_f;
            std::fill(g_glow.begin(), g_glow.end(), 0);
            std::fill(g_glow_lamp.begin(), g_glow_lamp.end(), 0);
            std::fill(best.begin(), best.end(), 0);
            std::fill(hue_sum.begin(), hue_sum.end(), 0);
            if (soft) {
                std::fill(lit_f.begin(), lit_f.end(), 0.0f);
                std::fill(full_f.begin(), full_f.end(), 0.0f);
            }
            lamps_loop(false);
            const bool same = glow_p == g_glow && lamp_p == g_glow_lamp && best_p == best && hue_p == hue_sum &&
                              (!soft || (lit_p == lit_f && full_p == full_f));
            static long checks = 0, fails = 0;
            ++checks;
            if (!same) ++fails;
            if (!same || checks % 300 == 1)
                std::fprintf(stderr, "lamp-par-check: %s (%ld checks, %ld with a difference)\n",
                             same ? "the same" : "DIFFERENT", checks, fails);
        }
        g_glow_any = glow_any.load();
        if (soft) {
            const std::size_t n = static_cast<std::size_t>(w) * h;
            share.assign(n, 1.0f);
            share_tmp.assign(n, 1.0f);
            for (std::size_t k = 0; k < n; ++k)
                if (full_f[k] > 0.0f) share[k] = lit_f[k] / full_f[k];
            // About the same place: both placed or both not, and near in
            // depth and height (the deck by the far wall's foot is not the
            // wall's).
            auto near_place = [&](std::size_t a, std::size_t b) {
                if (full_f[b] <= 0.0f || g_covered[a] != g_covered[b]) return false;
                return !g_covered[a] || (std::abs(g_xz[a] - g_xz[b]) <= 12 && std::abs(g_y[a] - g_y[b]) <= 8);
            };
            // Where no pixel in the window is in any shadow (most of the
            // light), the mean is 1 and is not worked out: running counts of
            // shadowed pixels along each row, then each column.
            static std::vector<int> shadowed;
            auto blur_line = [&](const std::vector<float>& in, std::vector<float>& out, std::size_t first,
                                 std::size_t step, int len) {
                shadowed.assign(static_cast<std::size_t>(len) + 1, 0);
                for (int i = 0; i < len; ++i) shadowed[i + 1] = shadowed[i] + (in[first + i * step] < 1.0f);
                for (int i = 0; i < len; ++i) {
                    const std::size_t k = first + i * step;
                    if (full_f[k] <= 0.0f) { out[k] = in[k]; continue; }
                    const int a0 = std::max(0, i - soft), a1 = std::min(len, i + soft + 1);
                    if (shadowed[a1] == shadowed[a0]) { out[k] = 1.0f; continue; }
                    float sum = 0.0f;
                    int cnt = 0;
                    for (int j = a0; j < a1; ++j) {
                        const std::size_t q = first + j * step;
                        if (q != k && !near_place(k, q)) continue;
                        sum += in[q];
                        ++cnt;
                    }
                    out[k] = sum / static_cast<float>(cnt);
                }
            };
            for (int y = 0; y < h; ++y) blur_line(share, share_tmp, static_cast<std::size_t>(y) * w, 1, w);
            for (int x = 0; x < w; ++x) blur_line(share_tmp, share, static_cast<std::size_t>(x), static_cast<std::size_t>(w), h);
            for (std::size_t k = 0; k < n; ++k)
                if (full_f[k] > 0.0f)
                    g_glow[k] = static_cast<std::uint8_t>(
                        std::min(255L, g_glow[k] + std::lround(full_f[k] * share[k])));
        }
        {
            const std::size_t n = static_cast<std::size_t>(w) * h;
            g_glow_hue2.assign(n, 0);
            g_glow_mix.assign(n, 0);
            if (!hue_sum.empty())
                for (std::size_t k = 0; k < n; ++k) {
                    if (!g_glow[k] || g_glow_lamp[k] >= lamps.size()) continue;
                    const std::uint8_t* hs = &hue_sum[k * kol::kHueCount];
                    const int own = std::min<int>(lamps[g_glow_lamp[k]].hue, kol::kHueCount - 1);
                    int other = -1;
                    for (int hh = 0; hh < kol::kHueCount; ++hh)
                        if (hh != own && hs[hh] && (other < 0 || hs[hh] > hs[other])) other = hh;
                    if (other < 0 || !hs[own]) continue;
                    g_glow_hue2[k] = static_cast<std::uint8_t>(other);
                    g_glow_mix[k] = static_cast<std::uint8_t>(255 * hs[other] / (hs[own] + hs[other]));
                }
        }
    }
    wt.lap("scene.windows");
    // Light through the map's windows (kMapWindows, KOL_GODRAYS). It
    // depends only on what the terrain pass drew and on the sun, so it is
    // worked out again only when the pass's key or the sun's height change.
    {
        static std::uint32_t win_key = 0;
        static bool win_valid = false;
        std::uint32_t key = pass_key;
        {
            const float r = sun_rise;
            std::uint32_t u;
            std::memcpy(&u, &r, sizeof u);
            kol::fnv_mix(key, u);
        }
        if (win_dark) kol::fnv_mix(key, g_win_sun_key);
        kol::fnv_mix(key, g_plane_mask.hash);   // the door's bars (moving as it opens)
        kol::fnv_mix(key, static_cast<std::uint32_t>(g_shutter_spread * 1000.0f));   // map 71's shutter
        const bool want = godray_strength() > 0.0f && g_wx.size() == static_cast<std::size_t>(w) * h &&
                          map_has_windows();
        if (!want) {
            g_win_any = false;
            win_valid = false;
        } else if (!win_valid || key != win_key || g_win_patch.size() != static_cast<std::size_t>(w) * h) {
            win_key = key;
            win_valid = true;
            // The last few builds kept whole (the user, on map 71, suggested
            // caching the 4 shadow states without the easing): its
            // shutter cycles four cone widths, and the pass key flips with
            // world_dirty at each change, so the light was built again ~25
            // times in 100 pictures (the grid ~45 ms, patch and haze ~15 on
            // the Mac each). A hit restores everything the build leaves.
            // KOL_WIN_CACHE=0: build every time.
            struct WinBuilt {
                std::uint32_t key = 0;
                unsigned long used = 0;
                WindowBeams beams;
                std::vector<ConeShaft> shafts;
                std::vector<std::uint8_t> patch, haze, glow;
                bool any = false;
                float haze_k = 0.0f;
                // Everything the build leaves, back in place (as stored).
                void put() const {
                    g_beams = beams;
                    g_shafts = shafts;
                    g_win_patch = patch;
                    g_win_haze = haze;
                    g_win_glow = glow;
                    g_win_any = any;
                    g_win_haze_k = haze_k;
                }
            };
            static std::array<WinBuilt, 8> built;
            static unsigned long tick = 0;
            static const bool cache_on = kol::env_not_zero("KOL_WIN_CACHE");
            WinBuilt* hit = nullptr;
            if (cache_on)
                for (WinBuilt& b : built)
                    if (b.used && b.key == key && b.patch.size() == static_cast<std::size_t>(w) * h) hit = &b;
            ++tick;
            if (hit) {
                hit->used = tick;
                hit->put();
            } else {
                build_window_light(w, h, ox, oy, kx, ky, sun_x, sun_z, cell_heights_occ);
                // Kept only from the second time its key is built: a camera
                // that pans has a new key every picture, none of them seen
                // again, and keeping each (the beams' grids and five buffers
                // copied) cost more than its build: 1.3 of the 1.9 ms this
                // section took in a pan on the Mac's map 55, with the build
                // itself on the row pool (2026-11-18, the Steam Deck's pans).
                // Map 71's four states are each built twice, then kept.
                // KOL_WIN_CACHE_FIRST=1: from the first, as before.
                static const bool keep_first = kol::env_set("KOL_WIN_CACHE_FIRST");
                static std::array<std::uint32_t, 16> missed{};
                static unsigned missed_at = 0;
                const bool seen = keep_first || std::find(missed.begin(), missed.end(), key) != missed.end();
                if (!seen) missed[missed_at++ % missed.size()] = key;
                if (cache_on && seen) {
                    WinBuilt* b = &built[0];
                    for (WinBuilt& c : built)
                        if (c.used < b->used) b = &c;
                    *b = {key, tick, g_beams, g_shafts, g_win_patch, g_win_haze, g_win_glow, g_win_any, g_win_haze_k};
                }
            }
            g_win_patch_built = g_win_patch;
            g_win_haze_built = g_win_haze;
            g_win_any_built = g_win_any;
        } else {
            // As built for the view (see g_win_patch_built).
            static const bool rework_in_place = std::getenv("KOL_GODRAYS_LIFT_IN_PLACE") != nullptr;
            if (!rework_in_place && g_win_patch_built.size() == g_win_patch.size()) {
                std::copy(g_win_patch_built.begin(), g_win_patch_built.end(), g_win_patch.begin());
                std::copy(g_win_haze_built.begin(), g_win_haze_built.end(), g_win_haze.begin());
                g_win_any = g_win_any_built;
            }
        }
    }
    // The windows with a light of their own (MapWindow::own_deg), the
    // second set: built the same way with the main set's state set aside,
    // into g_moon_patch / g_moon_haze, kept while the pass and the heights
    // are the same.
    {
        static std::uint32_t moon_key = 0;
        static bool moon_valid = false;
        WindowBeams& moon_beams = g_moon_beams;
        const int map = current_map();
        static const bool off = std::getenv("KOL_NO_OWN_LIGHT_WINDOWS") != nullptr;   // A/B
        const MapWindow* first = nullptr;
        for (const MapWindow& W : map_windows_all())
            if (!off && W.map == map && W.own_deg > 0.0f) { first = &W; break; }
        const std::size_t n = static_cast<std::size_t>(w) * h;
        g_moon_on = false;
        if (!first || godray_strength() <= 0.0f || g_wx.size() != n) {
            g_moon_any = false;
            moon_valid = false;
        } else {
            {
                const float l = std::hypot(first->own_x, first->own_z);
                g_moon_on = true;
                g_moon_dx = first->own_x / l;
                g_moon_dz = first->own_z / l;
                g_moon_rise = std::tan(first->own_deg * 3.14159265f / 180.0f);
            }
            const std::uint32_t key = ((pass_key ^ g_cast_heights_key) * 16777619u ^ static_cast<std::uint32_t>(map)) * 16777619u;
            if (!moon_valid || key != moon_key || g_moon_patch.size() != n) {
                moon_key = key;
                moon_valid = true;
                std::vector<std::uint8_t> moon_glow;   // (its glow and shafts are not kept)
                WinSetScope moon(g_moon_rise, moon_beams, g_win_any, g_moon_any, true,
                                 {{&g_win_patch, &g_moon_patch}, {&g_win_haze, &g_moon_haze}, {&g_win_glow, &moon_glow}});
                build_window_light(w, h, ox, oy, kx, ky, g_moon_dx, g_moon_dz, cell_heights_occ);
                g_moon_patch_built = g_moon_patch;
            }
            g_moon_gain = first->own_gain;
        }
    }
}

// The torchlight and the windows' light, and the units the map's lamps
// light.
void SceneBuild::glow_pass() {
    wt.lap("scene.glow");
    // The lamps' and the windows' light on each pixel's world place: here,
    // or, on a map whose model stands in for the grid's walls (g_occ, map
    // 101's arcade), once the model has placed its pixels (below).
    if (!g_occ) lamp_and_window_light();

    // The units the map's lamps light (Lamp::units), worked out once their
    // sprites are claimed (below, unit_lamp_glow).
    g_unit_lamps.clear();
    for (std::size_t i = 0; i < lamps.size() && i < static_cast<std::size_t>(kMaxLamps); ++i)
        if (lamps[i].units)
            g_unit_lamps.push_back({lamps[i].x, lamps[i].y, lamps[i].z, lamps[i].reach, lamps[i].power,
                                    lamps[i].halo, static_cast<int>(i)});
}
}  // namespace kol_focus_detail
