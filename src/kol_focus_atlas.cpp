// kol_focus_atlas.cpp — units and props as cards: the sprite atlas the
// units are rebuilt into from their own sprites (build_sprite_atlas), the
// map's flames, the lifted props and walls (lift_prop_pixels), the flame
// cards of the burning town, and what grows over a covered card
// (grow_over_sprite).

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
// Per card that a layer covered: where its covering pixels start and the
// box they may grow in (grow_over_sprite, after the props are lifted).
struct OverSeeds { std::vector<std::uint32_t> px; int x0, y0, x1, y1, depth, foot, unit, block; };
std::vector<OverSeeds> g_over_seeds;
}  // namespace

namespace kol_focus_detail {
// ── Units from their own sprites ─────────────────────────────────────────
// With the perspective, every unit used to be drawn from the frame, where a
// unit in front hides part of the one behind: when the stick leaned the
// camera, two ghosts one cell apart (states/CF_ghosts_overlap.state) pulled
// apart and the back one had no lower body, so the mesh stretched them into
// one warped figure. The game has the whole of each sprite in its OAM and
// tiles, so each unit is rebuilt from its own entries into a square of an
// atlas and drawn as its own quads, and its pixels in the frame are lifted
// out: there the mesh places the scene as it is without units (the ground or
// sky behind), with the ground cloned in.
//
// A texel the frame shows under a background layer stays out: the game hides
// a unit's lower part in water or behind a wall by priority, and that is
// kept. One the frame shows under another sprite is rebuilt. Visible texels
// take the frame's own colours (kol_perspective.cpp copies them); hidden ones
// the latched palette, converted as the PPU converts. A semi-transparent unit
// (the deployment ghost) is rebuilt too: the frame's colours are already its
// blend with the ground, and a hidden texel is blended with the ground the
// compositor recorded under it, at BLDALPHA's weights. Left in the frame, its
// blended pixels were placed as an upright thing where no sprite was rebuilt,
// and the ground beside it tore along those rows (DM, placing a unit, stick
// down). Left in the frame as before: units larger than a slot, and every
// unit while BLDCNT blends or fades sprites.

// A new block's square in the atlas (the map flames, the props, the wall
// surfaces, the model pieces): the atlas grows by rows of squares as needed,
// and the square's opaque bytes start as `opaque` (its blur 0 when
// clear_blur). Returns the block's index, g_blocks.size(); the callers keep
// their own kMaxLiftBlocks checks.
int atlas_new_square(int* ax0, int* ay0, std::uint8_t opaque, bool clear_blur) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const int b = static_cast<int>(g_blocks.size());
    const int need_h = (b / g_atlas_per_row + 1) * S;
    if (need_h > g_atlas_h) {
        g_atlas_h = need_h;
        const std::size_t apx = static_cast<std::size_t>(g_atlas_w) * g_atlas_h;
        g_atlas_rgb.resize(apx * 3, 0);
        g_atlas_opaque.resize(apx, 0);
        g_atlas_blur.resize(apx, 0);
    }
    const auto [x0, y0] = kol::slot_origin(b, g_atlas_per_row);
    for (int y = 0; y < S; ++y) {
        std::fill_n(&g_atlas_opaque[static_cast<std::size_t>(y0 + y) * g_atlas_w + x0], S, opaque);
        if (clear_blur) std::fill_n(&g_atlas_blur[static_cast<std::size_t>(y0 + y) * g_atlas_w + x0], S, 0);
    }
    *ax0 = x0;
    *ay0 = y0;
    return b;
}
}  // namespace kol_focus_detail

namespace {
constexpr int kMaxSpriteBlocks = 32;
}  // namespace

namespace kol_focus_detail {
// BG1 props lifted off the ground they stand on (Options > Lifted Props, or
// KOL_PERSP_LIFT_PROPS=1; experimental): their pixels drawn from the atlas as upright cards, like
// units, and the scene there shows what the game drew behind them (the
// compositor's scene without BG1, gbarecomp g_ppu_scene_under_bg1) at the
// terrain's own place, so a camera move opens onto it instead of stretching.
// g_pre_*: the terrain pass's places before the props claim their pixels.
bool lift_props_on() {
    static const bool env = kol::env_true("KOL_PERSP_LIFT_PROPS");
    return env || kol::setting_on(kol::Setting::LiftProps);
}
}  // namespace kol_focus_detail

namespace {
// The same for BG1 terrain over BG0 (Options > Lifted Walls, or
// KOL_PERSP_LIFT_WALLS=1): the castle's pillar edges over the wall behind.
// The burning town (map 99): its BG1 is its flames, which stand as cards
// (flame_cards); there Lifted Walls lifts nothing and Lifted Props only them
// (the user, 2026-10-17, the fire map slowing the game, asked to skip
// lifted props and walls there; walls off saved 0.8 ms a job on the Mac;
// then said the archer map could keep them: 99 alone).
// KOL_FIRE_MAP_LIFT=1: as elsewhere.
bool fire_actor(int actor);   // (kFireActors)
bool fire_map_now() {
    static const bool off = std::getenv("KOL_FIRE_MAP_LIFT") != nullptr;
    if (off) return false;
    return current_map() == 99;
}
}  // namespace

namespace kol_focus_detail {
bool lift_walls_on() {
    static const bool env = kol::env_true("KOL_PERSP_LIFT_WALLS");
    return (env || kol::setting_on(kol::Setting::LiftWalls)) && !fire_map_now();
}

// What covered a card is a surface (the wall beside the door, a tree's card),
// and as the camera moves other pixels of it come over the unit: the whole
// surface near the card is drawn again after it (ScenePixels::over_sprite),
// from the covering pixels across any neighbour nearer than the unit (X +
// Z), its own place (a wall's or prop's card keeps it in the scene's
// arrays; under the unit's own card, the ground under it, g_under_*), that
// is over his ground (nothing under his feet hides him from a camera over
// them: GC, the front of the ledge a unit stood on came over his feet with
// the camera down): the wall and a sconce on it, a
// house's whole roof, lifted as many wall cards (states/thor/0048: the
// roof's edge, Y 128, covers Alphonse's feet at rest; kept to one card, or
// to one surface, a strip of it came over his legs with the camera down
// while the rest of the roof stayed behind him; asked for the whole roof to
// cover Alphonse). Within 64 of the card; not where another sprite is on
// top. The covering pixels themselves are marked 1 (drawn always: the card
// has the texels they hid); what grew from them 3, and 2 where the unit
// itself is on top (the game drew him over the roof's lower part, BG0, and
// only its edge, BG1, over him): both drawn only with the camera off its
// rest, so the picture at rest stays the game's. After the props and walls
// are lifted.
void grow_over_sprite(const std::uint8_t* layers, int w, int h) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (g_over_seeds.empty() || g_over_sprite.size() != n) return;
    const bool lifted = g_lift.size() == n;
    int depth = 0, foot = 0, unit = -1;
    // A card's lift (a wall's, a prop's): a unit's own is the ground under it.
    auto card = [&](std::size_t k) {
        const int l = lifted ? g_lift[k] : 0;
        return l > 0 && l <= static_cast<int>(g_blocks.size()) && (g_blocks[l - 1].surface || g_blocks[l - 1].prop) ? l : 0;
    };
    // Where a unit's card took the pixel, its place is the ground under it
    // (g_under_*), as the warp places it.
    const bool unders = g_under_xz.size() == n && g_under_y.size() == n && g_under_kind.size() == n;
    auto same = [&](std::size_t b) {
        const bool ub = unders && lifted && g_lift[b] && !card(b);
        const int xz = ub ? g_under_xz[b] : g_xz[b], y = ub ? g_under_y[b] : g_y[b];
        const std::uint8_t kd = ub ? g_under_kind[b] : g_kind[b];
        return kd != kol::kSceneNone && xz > depth && y > foot + 4;
    };
    // The covering pixels widened by kOverMargin over the scene's own (a
    // background on top), marked 1 too, drawn at rest as well: at rest the
    // card (at the unit's depth) and the wall (at its own) are scaled a little
    // apart by the perspective, and the card's edge, its dark outline, showed
    // round the redrawn pixels (states/thor/0052, a figure behind the left
    // wall: its outline showed through the wall). At rest they draw
    // what they drew.
    constexpr int kOverMargin = 3;
    for (OverSeeds& o : g_over_seeds) {
        // (The seeds as they came: the loop appends to o.px, so by index up
        // to their count, each read before it pushes.)
        const std::size_t seeds = o.px.size();
        for (std::size_t i = 0; i < seeds; ++i) {
            const std::uint32_t k = o.px[i];
            const int px = static_cast<int>(k % static_cast<std::uint32_t>(w)), py = static_cast<int>(k / static_cast<std::uint32_t>(w));
            for (int dy = -kOverMargin; dy <= kOverMargin; ++dy)
                for (int dx = -kOverMargin; dx <= kOverMargin; ++dx) {
                    const int qx = px + dx, qy = py + dy;
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    const std::size_t m = static_cast<std::size_t>(qy) * w + qx;
                    if (g_over_sprite[m] || (layers[m] != gba::kLayerNotComposed && scene_top(layers, m) > 3u)) continue;
                    if (g_kind[m] == kol::kSceneNone) continue;
                    g_over_sprite[m] = 1;
                    g_over_block[m] = static_cast<std::uint8_t>(o.block + 1);
                    o.px.push_back(static_cast<std::uint32_t>(m));
                }
        }
    }
    for (OverSeeds& o : g_over_seeds) {
        depth = o.depth;
        foot = o.foot;
        unit = o.unit;
        std::vector<std::uint32_t>& stack = o.px;
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            stack.pop_back();
            const int px = static_cast<int>(k % static_cast<std::size_t>(w)), py = static_cast<int>(k / static_cast<std::size_t>(w));
            const int nb[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
            for (const auto& q : nb) {
                if (q[0] < o.x0 || q[0] > o.x1 || q[1] < o.y0 || q[1] > o.y1) continue;
                const std::size_t m = static_cast<std::size_t>(q[1]) * w + q[0];
                if (g_over_sprite[m] || !same(m)) continue;
                std::uint8_t mark = 3;   // grown: needed only once the camera moves
                if (layers[m] != gba::kLayerNotComposed && scene_top(layers, m) > 3u) {
                    if (scene_top(layers, m) != kol::kLayerObj || g_obj_top[m] < 0 || g_entries[g_obj_top[m]].owner != unit)
                        continue;
                    mark = 2;   // the unit itself on top at rest
                }
                g_over_sprite[m] = mark;
                g_over_block[m] = static_cast<std::uint8_t>(o.block + 1);
                stack.push_back(static_cast<std::uint32_t>(m));
            }
        }
    }
}

// A prop nearer than a unit that never covered him at rest: the game draws
// sprites over BG1, and the props' cards go in with the map, before the
// units' (kol_perspective warp_buckets), so with the camera low a reed in
// the water in front of Ivanna stood over her legs and she was drawn whole
// over it (map 16, states/thor/0082, the user reported sprites drawn over
// the plant props instead of partly hidden by them). The prop's pixels within 64
// of the unit's card are drawn again after it (3: only off rest, so the
// picture at rest stays the game's), after the nearest such unit's card
// where two stand behind one prop (drawn after both). After the props are
// lifted. KOL_PROPS_UNDER_UNITS=1: as before.
// Lifted walls the same, and the terrain no card lifted: a pixel at a time
// (a wall square, a roof, spans depths), one nearer than the unit and above
// his ground, as grow_over_sprite judges a pixel. Units' cards are drawn
// after the whole map, so only these marks put nearer terrain over them,
// and the grow starts only where the game's own priority cut a unit at
// rest. Map 13 (states/thor/0096), the camera down: a unit on the upper
// street behind the front house (X+Z 544, at 80) stood over its roof
// (the drawn roof, at 118-128, X+Z ~706) from his knees down; at rest he is
// above the ridge, so nothing cut him and nothing grew (the user reported
// a regression: a sprite drawn over
// the house). The terrain searched: 64 round the card, a pixel no
// farther from it than a quarter of how much nearer it is, plus 8 (the
// camera lowered brings nearer terrain up, raised brings it down, a lean
// swings it sideways, all by its depth: leaning left as well, the roof that
// covered his feet lay 28 px left of his card, 162 nearer). Only with the
// camera off rest (marks 3 draw only then). KOL_WALLS_UNDER_UNITS=1: walls
// as before; KOL_TERRAIN_UNDER_UNITS=1: the terrain as before.
void props_over_units(const std::uint8_t* layers, int w, int h, bool props_on, bool walls_on) {
    static const bool off = std::getenv("KOL_PROPS_UNDER_UNITS") != nullptr;
    static const bool walls_off = std::getenv("KOL_WALLS_UNDER_UNITS") != nullptr;
    static const bool terrain_off = std::getenv("KOL_TERRAIN_UNDER_UNITS") != nullptr;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (g_lift.size() != n || g_over_sprite.size() != n || g_over_block.size() != n) return;
    constexpr int kReach = 64;
    const int nb = static_cast<int>(g_blocks.size());
    const int nc = static_cast<int>(std::min(g_casters.size(), g_blocks.size()));
    auto is_unit = [&](int ub) {
        const kol::ScenePixels::SpriteBlock& u = g_blocks[ub];
        return g_casters[ub].unit && !u.prop && !u.surface && !u.overlay && !u.scenery;
    };
    // Pixel k drawn again after unit ub's card, unless it is taken already:
    // by a nearer unit's card only if grown (3) after a unit's card farther
    // than this one.
    auto take = [&](std::size_t k, int ub) {
        if (g_over_sprite[k]) {
            const int ob = g_over_block[k] - 1;
            if (g_over_sprite[k] != 3 || ob < 0 || ob >= nc || !g_casters[ob].unit || g_blocks[ob].xz >= g_blocks[ub].xz)
                return;
        }
        g_over_sprite[k] = 3;
        g_over_block[k] = static_cast<std::uint8_t>(ub + 1);
        g_over_sprite_any = true;
    };
    for (int pb = 0; pb < nb && !off; ++pb) {
        const kol::ScenePixels::SpriteBlock& p = g_blocks[pb];
        const bool wall = p.surface && walls_on && !walls_off;
        if (!(p.prop && props_on) && !wall) continue;
        const int depth = p.xz + p.sort_extra;
        for (int ub = 0; ub < nc && ub < 255; ++ub) {
            if (!is_unit(ub)) continue;
            const kol::ScenePixels::SpriteBlock& u = g_blocks[ub];
            if (!wall && u.xz >= depth) continue;
            const int x0 = std::max({0, p.sx, u.sx - kReach}), x1 = std::min({w, p.sx + p.w, u.sx + u.w + kReach});
            const int y0 = std::max({0, p.sy, u.sy - kReach}), y1 = std::min({h, p.sy + p.h, u.sy + u.h + kReach});
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if (g_lift[k] != pb + 1) continue;
                    if (wall && (g_xz[k] <= u.xz || g_y[k] <= u.y + 4)) continue;
                    take(k, ub);
                }
        }
    }
    const bool off_rest =
        std::abs(kol::g_cam_cap_look(0)) + std::abs(kol::g_cam_cap_look(1)) > 0.01f || kol::g_cam_cap_strike() > 0.0f;
    if (terrain_off || !off_rest) return;
    for (int ub = 0; ub < nc && ub < 255; ++ub) {
        if (!is_unit(ub)) continue;
        const kol::ScenePixels::SpriteBlock& u = g_blocks[ub];
        const int x0 = std::max(0, u.sx - kReach), x1 = std::min(w, u.sx + u.w + kReach);
        const int y0 = std::max(0, u.sy - kReach), y1 = std::min(h, u.sy + u.h + kReach);
        for (int py = y0; py < y1; ++py) {
            const int dy = std::max({0, u.sy - py, py - (u.sy + u.h - 1)});
            for (int px = x0; px < x1; ++px) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                const int nearer = g_xz[k] - u.xz;
                if (nearer <= 0 || g_lift[k] || g_kind[k] == kol::kSceneNone || g_y[k] <= u.y + 4) continue;
                const int dx = std::max({0, u.sx - px, px - (u.sx + u.w - 1)});
                if (std::max(dx, dy) > nearer / 4 + 8) continue;
                if (layers[k] != gba::kLayerNotComposed && scene_top(layers, k) > 3u) continue;
                take(k, ub);
            }
        }
    }
}
}  // namespace kol_focus_detail

namespace {
// build_sprite_atlas' frame, for its phases: the view, the units, and the
// sprites' colour effects (see there).
struct AtlasFrame {
    const std::uint8_t* layers;
    int w, h, ox, oy, kx, ky;
    const Unit* units;
    int count, unit_count;
    bool cast;   // units cast a shadow of their own (the game's blob left out)
    const std::uint8_t* tiles;
    const std::uint8_t* pal;
    unsigned effect, evy;   // BLDCNT's effect, BLDY
    bool brightness;        // applied to the texels the frame does not show
    unsigned eva, evb;      // BLDALPHA
    const std::uint8_t* under;
    int per_row;
    unsigned effected(unsigned c) const {
        // As gbarecomp's brighten/darken_gba555: green at 6 bits
        // (palette bit 15), darkening rounded with +7.
        unsigned r5 = c & 31u, g6 = ((c >> 4) & 62u) | (c >> 15), b5 = (c >> 10) & 31u;
        if (effect == 2u) {
            r5 += ((31u - r5) * evy + 8u) >> 4;
            g6 += ((63u - g6) * evy + 8u) >> 4;
            b5 += ((31u - b5) * evy + 8u) >> 4;
        } else {
            r5 -= (r5 * evy + 7u) >> 4;
            g6 -= (g6 * evy + 7u) >> 4;
            b5 -= (b5 * evy + 7u) >> 4;
        }
        return (b5 << 10) | ((g6 >> 1) << 5) | r5;
    }
};

// Doors in their walls first (g_wall_px): their pieces stay in the scene
// at the wall's place, no card; and before any card is built, since what
// they cover of a unit is hidden (the one going in at JM ~930 went in
// behind the door's frame, as the game draws it).
// (wall_unit: the units placed so; hidden_decals: see SceneObjects::decals_from.)
void place_wall_objects(const AtlasFrame& af, bool* wall_unit, std::uint32_t& hidden_decals) {
    const std::uint8_t* const tiles = af.tiles;
    const Unit* const units = af.units;
    const int w = af.w, h = af.h, ox = af.ox, oy = af.oy, kx = af.kx, ky = af.ky, count = af.count;
    static const bool object_card = std::getenv("KOL_SCENE_OBJECT_CARD") != nullptr;
    if (!object_card && g_under_xz.size() == static_cast<std::size_t>(w) * h)
        for (int u = 0; u < count && u < kObjects; ++u) {
            if (!scene_object_in_wall(units[u]) || (u < 64 && g_unit_hanging[u])) continue;
            wall_unit[u] = true;
            const SceneObjects* so = scene_object_of(units[u]);
            if (so && so->units_in_front) g_wall_front_xz = units[u].x + units[u].z;
            // Its tile block: from its own lowest tile, a blob's left out
            // (the walker's behind map 67's door went to it every other frame).
            int tile0 = 0x400;
            for (int k = 0; k < g_entry_n; ++k)
                if (g_entries[k].owner == u && !blob_tile(g_entries[k]))
                    tile0 = std::min(tile0, static_cast<int>(g_entries[k].a2 & 0x3FFu));
            const bool block = so && so->plane_x >= 0.0f && tile0 < 0x400;
            // Its painted shadow (the game's light through the door, drawn
            // as sprites) gives way to ours where the light through its
            // plane is drawn (the user asked for the shadow sprite to be
            // hidden when the lighting is on). KOL_KEEP_OBJECT_DECALS=1 keeps it.
            static const bool keep_decals = std::getenv("KOL_KEEP_OBJECT_DECALS") != nullptr;
            if (block && so->decals_from > 0 && !keep_decals && map_has_windows() && godray_strength() > 0.0f &&
                (g_shadow_on || dark_windows()))
                hidden_decals = static_cast<std::uint32_t>(current_map()) << 22 |
                                static_cast<std::uint32_t>(tile0 + so->decals_from) << 11 |
                                static_cast<std::uint32_t>(tile0 + 0x40);
            // Its plane's cover (g_plane_mask), from every opaque texel of
            // its own pieces, seen or not (a unit in front hides some).
            PlaneMask pm;
            std::vector<std::pair<float, float>> texels;
            for (int k = 0; k < g_entry_n; ++k) {
                ObjEntry& e = g_entries[k];
                const int tile = static_cast<int>(e.a2 & 0x3FFu);
                const bool in_block = block && tile >= tile0 && tile < tile0 + 0x40;
                if (block ? !in_block : e.owner != u) continue;
                e.owner = u;
                const bool decal = in_block && so->decals_from > 0 && tile >= tile0 + so->decals_from;
                if (block && !decal && tiles)
                    for (int ty = 0; ty < e.h; ++ty)
                        for (int tx = 0; tx < e.w; ++tx) {
                            if (!obj_texel(e, tx, ty, tiles, g_map_1d)) continue;
                            const float X = so->plane_x;
                            const float Z = X - 2.0f * static_cast<float>(e.x + tx - kx);
                            const float Y = 2.0f * ((X + Z) * 0.25f + static_cast<float>(ky) - static_cast<float>(e.y + ty));
                            texels.emplace_back(Z, Y);
                        }
                for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py)
                    for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                        const std::size_t p = static_cast<std::size_t>(py) * w + px;
                        if (g_obj_top[p] != k) continue;
                        if (!g_wall_px_any) g_wall_px.assign(static_cast<std::size_t>(w) * h, 0);
                        if (so && so->plane_x >= 0.0f && !decal) {
                            // In its plane: X = plane_x, the point the pixel's
                            // view line meets it at.
                            const float X = so->plane_x;
                            const float Z = X - 2.0f * static_cast<float>(px - kx - ox);
                            const float Y = 2.0f * ((X + Z) * 0.25f + static_cast<float>(ky + oy) - static_cast<float>(py));
                            g_xz[p] = static_cast<std::int32_t>(std::lround(X + Z));
                            g_y[p] = static_cast<std::int32_t>(std::lround(Y));
                            g_kind[p] = kol::kSceneUpright;
                        } else {
                            g_xz[p] = g_under_xz[p];
                            g_y[p] = g_under_y[p];
                            g_kind[p] = g_under_kind[p];
                        }
                        g_iface[p] = 0;
                        g_wall_px[p] = so && so->units_in_front ? 2 : 1;
                        g_wall_px_any = true;
                    }
            }
            if (!texels.empty()) {
                float z0 = 1e9f, z1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (const auto& t : texels) {
                    z0 = std::min(z0, t.first); z1 = std::max(z1, t.first);
                    y0 = std::min(y0, t.second); y1 = std::max(y1, t.second);
                }
                pm.map = current_map();
                pm.plane = so->plane_x;
                pm.z0 = static_cast<int>(std::floor(z0)) - 2;
                pm.y0 = static_cast<int>(std::floor(y0)) - 2;
                pm.nz = static_cast<int>(std::ceil(z1)) + 3 - pm.z0;
                pm.ny = static_cast<int>(std::ceil(y1)) + 3 - pm.y0;
                pm.m.assign(static_cast<std::size_t>(pm.nz) * pm.ny, 0);
                // A texel's corner at (Z, Y): it covers Z - 2..Z and Y - 2..Y
                // (a pixel right is 2 less Z, a row down 2 less Y).
                for (const auto& t : texels) {
                    const int a = static_cast<int>(std::lround(t.first)) - pm.z0;
                    const int b = static_cast<int>(std::lround(t.second)) - pm.y0;
                    for (int db = -2; db < 0; ++db)
                        for (int da = -2; da < 0; ++da)
                            if (a + da >= 0 && a + da < pm.nz && b + db >= 0 && b + db < pm.ny)
                                pm.m[static_cast<std::size_t>(b + db) * pm.nz + a + da] = 1;
                }
                std::uint32_t hsh = kol::kFnvBasis;
                for (std::uint8_t v : pm.m) kol::fnv_mix(hsh, v);
                pm.hash = (hsh ^ static_cast<std::uint32_t>(pm.z0 * 131 + pm.y0)) * 16777619u;
                g_plane_mask = std::move(pm);
            }
        }
}

// A unit's OAM entries (mine, in OAM order) and their box; its blob's parts
// split off when they do not fit a slot with it.
void gather_unit_entries(const AtlasFrame& af, int u, bool blob_split, int (&mine)[16], int& nm, int& x0, int& y0,
                         int& x1, int& y1, bool& any, bool& plain, bool& semi) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const int w = af.w, h = af.h, ox = af.ox, oy = af.oy;
    for (int k = 0; k < g_entry_n; ++k) {
        const ObjEntry& e = g_entries[k];
        if (e.owner != u || blob_tile(e)) continue;
        if (nm < 16) mine[nm++] = k;
        any = true;
        if (((e.a0 >> 10) & 3u) > 1u) plain = false;
        if (((e.a0 >> 10) & 3u) != 1u) semi = false;
        x0 = std::min(x0, e.x); y0 = std::min(y0, e.y);
        x1 = std::max(x1, e.x + e.w); y1 = std::max(y1, e.y + e.h);
    }
    // Its blob's parts, when the unit with them still fits a slot; else
    // they are its blob out of its frame (g_blob_of), as on the frames
    // they fit no frame at all: the girl on the stairs was 35 rows with
    // her blob on the others, and every other frame she was left in the
    // frame, unshaded and with the game's blob (FE).
    for (int k = 0; k < g_entry_n; ++k) {
        const ObjEntry& e = g_entries[k];
        if (e.owner != u || !blob_tile(e)) continue;
        const int bx0 = std::min(x0, e.x), by0 = std::min(y0, e.y);
        const int bx1 = std::max(x1, e.x + e.w), by1 = std::max(y1, e.y + e.h);
        if (!any || (bx1 - bx0 <= S && by1 - by0 <= S) || !blob_split) {
            if (nm < 16) mine[nm++] = k;
            any = true;
            x0 = bx0; y0 = by0; x1 = bx1; y1 = by1;
            continue;
        }
        g_blob_of[k] = u;
        g_entries[k].owner = -1;
        for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py)
            for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                if (g_obj_top[p] != k || g_under_xz.size() != static_cast<std::size_t>(w) * h) continue;
                g_xz[p] = g_under_xz[p];
                g_y[p] = g_under_y[p];
                g_kind[p] = g_under_kind[p];
                g_iface[p] = 0;
            }
    }
}

// One card of unit u (rows ry0..ry1 of its box), texel by texel, in atlas
// square b at (ax0, ay0); what covered it into over_seeds.
void fill_unit_card(const AtlasFrame& af, int u, const int* mine, int nm, int x0, int x1, int ry0, int ry1, int b,
                    int ax0, int ay0, float cut_hy, float shade_k, std::vector<std::uint32_t>& over_seeds) {
    const std::uint8_t* const layers = af.layers;
    const int w = af.w, h = af.h, ox = af.ox, oy = af.oy, kx = af.kx, ky = af.ky, unit_count = af.unit_count;
    const Unit* const units = af.units;
    const bool cast = af.cast, brightness = af.brightness;
    const std::uint8_t* const tiles = af.tiles;
    const std::uint8_t* const pal = af.pal;
    const std::uint8_t* const under = af.under;
    const unsigned eva = af.eva, evb = af.evb;
    for (int ty = 0; ty < ry1 - ry0; ++ty) {
        for (int tx = 0; tx < x1 - x0; ++tx) {
            const int hx = x0 + tx, hy = ry0 + ty;
            // The unit's own entry on top here with an opaque texel.
            int best = -1;
            unsigned best_prio = 4u, index = 0u;
            for (int m = 0; m < nm; ++m) {
                const int k = mine[m];
                const ObjEntry& e = g_entries[k];
                if (hx < e.x || hx >= e.x + e.w || hy < e.y || hy >= e.y + e.h) continue;
                const unsigned prio = (e.a2 >> 10) & 3u;
                if (best >= 0 && prio >= best_prio) continue;
                const unsigned t = obj_texel(e, hx - e.x, hy - e.y, tiles, g_map_1d);
                if (!t) continue;
                best = k; best_prio = prio; index = t;
            }
            if (best < 0) continue;
            const int px = hx + ox, py = hy + oy;
            const std::uint8_t* ground = nullptr;   // under a hidden texel
            if (px >= 0 && py >= 0 && px < w && py < h) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                // Under a background layer: hidden by priority. The game
                // cut a unit standing in a doorway at the door's frame
                // (BG1 over the sprite: map 65, states/thor/0051); as a
                // card, the cut stayed where it was in the frame as the
                // camera moved (drawn whole over the wall with it low).
                // The card keeps the texel, and the pixel that covered
                // it is drawn again after the cards at its own place
                // (kol_perspective), so the frame cuts the unit wherever
                // the camera puts it. KOL_UNIT_PRIORITY_CUT=1: the texel
                // left out, as before. Only by an upright thing (a wall,
                // a prop, a model's side): the ground covering a unit's
                // feet at a ledge (states/thor/0048, Andy) grew over the
                // floor before him, which then came up over his legs with
                // the camera down; there the texel is left out, as before.
                static const bool priority_cut = std::getenv("KOL_UNIT_PRIORITY_CUT") != nullptr;
                if (layers[p] != gba::kLayerNotComposed && scene_top(layers, p) != kol::kLayerObj) {
                    if (priority_cut || scene_top(layers, p) > 3u || g_kind[p] != kol::kSceneUpright) continue;
                    g_over_sprite[p] = 1;
                    g_over_block[p] = static_cast<std::uint8_t>(b + 1);
                    g_over_sprite_any = true;
                    over_seeds.push_back(static_cast<std::uint32_t>(p));
                }
                if (g_wall_px_any && (g_wall_px[p] == 1 ||   // behind a door in its wall
                                      (g_wall_px[p] == 2 && units[u].x + units[u].z < g_wall_front_xz)))
                    continue;
                if (scene_top(layers, p) == kol::kLayerObj && g_obj_top[p] >= 0 &&
                    g_entries[g_obj_top[p]].owner == u)
                    g_lift[p] = static_cast<std::uint8_t>(b + 1);
                else if (under && scene_top(layers, p) == kol::kLayerObj)
                    ground = under + p * 3;
            }
            const ObjEntry& e = g_entries[best];
            const unsigned slot = (e.a0 & 0x2000u) ? index : ((e.a2 >> 12) & 0xFu) * 16u + index;
            unsigned c = pal[0x200u + slot * 2u] | (pal[0x201u + slot * 2u] << 8);
            if (brightness && ((e.a0 >> 10) & 3u) == 0u) c = af.effected(c);
            const unsigned r = c & 31u, g = (c >> 5) & 31u, bl = (c >> 10) & 31u;
            const std::size_t a = static_cast<std::size_t>(ay0 + ty) * g_atlas_w + ax0 + tx;
            std::uint8_t* out = &g_atlas_rgb[a * 3];
            out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
            out[1] = static_cast<std::uint8_t>((g << 3) | (g >> 2));
            out[2] = static_cast<std::uint8_t>((bl << 3) | (bl >> 2));
            // A hidden texel of a semi-transparent sprite: blended with
            // the ground under it, as the frame shows its visible ones.
            if (((e.a0 >> 10) & 3u) == 1u && ground)
                for (int ch = 0; ch < 3; ++ch)
                    out[ch] = static_cast<std::uint8_t>(
                        std::min(255u, (out[ch] * eva + ground[ch] * evb + 8u) >> 4));
            // Fading in (units[u].fade): toward the ground under the
            // sprite, the scene the compositor drew without sprites.
            if (units[u].fade < 1.0f && under && px >= 0 && py >= 0 && px < w && py < h) {
                const std::uint8_t* g = under + (static_cast<std::size_t>(py) * w + px) * 3;
                const float f = units[u].fade;
                for (int ch = 0; ch < 3; ++ch)
                    out[ch] = static_cast<std::uint8_t>(std::lround(out[ch] * f + g[ch] * (1.0f - f)));
            }
            {
                const float f = kol::ScenePixels::card_shade(static_cast<float>(hy), cut_hy, shade_k,
                                                             kol::card_shade_soft());
                if (f < 1.0f)
                    for (int ch = 0; ch < 3; ++ch)
                        out[ch] = static_cast<std::uint8_t>(std::lround(out[ch] * f));
            }
            // The game's dark blob under a unit: colour 1 (its outline's)
            // in the rows just above its feet (EK, EI: rows feet - 7 to
            // feet - 2; the legs' outline stops above). With the unit
            // casting a real shadow it is left out, and the ground the
            // game drew under it shows.
            // The blob is one oval in every unit's lower 16x16 (IH: all
            // six units the same bytes), rows feet - 8 to feet - 2, its
            // top row 8 wide (feet x - 4 to + 3). That top row was left:
            // on the fairy it showed as a dark line under her feet, and
            // the lift under it as a dark column below (IH, the user's
            // slot 3, AO off). KOL_BLOB_NO_TOP=1 leaves it, as before.
            static const bool blob_top = std::getenv("KOL_BLOB_NO_TOP") == nullptr;
            // Not a bird in the sky (over kSkyY: the gulls round the sail on
            // map 117, states/thor/0223, 290-330 high): it has no blob, and
            // its outline (colour 1 too) lies round its place: the rows the
            // rule takes were its outline, a washed-out gull (the user,
            // 2026-11-12, found their sprite looked off).
            if (cast && u < unit_count && index == 1u && !flies_high(u, units[u])) {
                const int feet_y = feet_sy(units[u]) + ky;
                const int feet_x = feet_sx(units[u]) + kx;
                if (hy >= feet_y - 7) continue;
                if (blob_top && hy == feet_y - 8 && hx >= feet_x - 4 && hx <= feet_x + 3) continue;
            }
            g_atlas_opaque[a] = 1;
        }
    }
}

// A campfire's card as the fire effect's square (block b, unit u's last).
void campfire_block(const AtlasFrame& af, int u, int b, int ax0, int ay0, int ry0) {
    const int w = af.w, h = af.h, ox = af.ox, oy = af.oy, kx = af.kx, ky = af.ky;
    const Unit* const units = af.units;
    kol::ScenePixels::SpriteBlock& fb = g_blocks.back();
    fb.effect = 1;
    fb.fire = true;
    fb.fx_cx = static_cast<float>(feet_sx(units[u]) + kx + ox);
    fb.foot_row = static_cast<float>((units[u].x + units[u].z) / 4 - units[u].y / 2 + ky + oy);
    fb.fx_top = static_cast<float>(ry0 + oy);
    fb.fx_seed = ((units[u].x + units[u].z) * 7 + units[u].y * 13) & 1023;
    fb.fx_over = true;   // a sprite's fire: BG1's brightening (the camp's glow) is not its fade
    // Its heat (KOL_FIRE_HEAT, mockups): the user's pick, 0.7.
    static const float heat = kol::env_num_clamp("KOL_FIRE_HEAT", 0.3f, 1.0f, 0.7f);
    fb.fx_heat = heat;
    // Its width (KOL_FIRE_WIDTH, mockups): narrower than a burning
    // cell's (the user asked for a narrower campfire).
    static const float width = kol::env_num_clamp("KOL_FIRE_WIDTH", 0.3f, 1.0f, 0.7f);
    fb.fx_width = width;
    static const int palette = [] {
        const char* e = std::getenv("KOL_FIRE_PALETTE");
        return e ? std::atoi(e) : 1;   // deeper (shots/camp63/campfire_contrast.png, D)
    }();
    fb.fx_palette = palette;
    // Its logs stay the game's, on the ground where they lie: only
    // its flame's texels are lifted onto the card, the logs left in
    // the scene. (On the card, in front of the effect, they came out
    // washed to white: an effect card is composited for a glow.) The
    // camp fire's flame is red, orange and a pale yellow (255,66,24 /
    // 247,148,0 / 239,247,132), its logs, wood and outline all under
    // 215 in red: a flame texel is >= 230. Every texel of the square
    // is the effect's.
    g_fire_cx = fb.fx_cx;
    g_fire_cy = fb.foot_row;
    g_fire_seen = true;
    for (int ty = 0; ty < fb.h; ++ty)
        for (int tx = 0; tx < fb.w; ++tx) {
            const std::size_t a = static_cast<std::size_t>(ay0 + ty) * g_atlas_w + ax0 + tx;
            const std::uint8_t* c = &g_atlas_rgb[a * 3];
            const bool log_px = g_atlas_opaque[a] && c[0] < 230;
            g_atlas_opaque[a] = 1;
            const int px = fb.sx + tx, py = fb.sy + ty;
            if (log_px && px >= 0 && py >= 0 && px < w && py < h) {
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                if (g_lift[k] == static_cast<std::uint8_t>(b + 1)) {
                    g_lift[k] = 0;
                    if (g_fire_logs.size() == static_cast<std::size_t>(w) * h) {
                        g_fire_logs[k] = 1;
                        g_fire_logs_any = true;
                    }
                }
            }
        }
}

// A cloud of vapor at each fall's foot (the user, map 73, asked for vapor at the
// bottom of the falls; the thick one of two): puffs in the world, born at
// the foot along the sheet, rising, swelling, drifting out over the pool and thinning,
// lighter on top, half size where a fall lands on the next one's cap. Worked out at half
// resolution every other picture, as the effects' squares are (fx_square), and drawn on
// see-through squares standing in the air a little out from the sheet, so it floats as
// the camera moves (painted on the surfaces, a lifted spike left a hole of its shape in
// it). A square only where the cloud is, its texels only where it reaches.
float vapor_hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}
float vapor_noise(float x, float y) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = vapor_hash(xi, yi), b = vapor_hash(xi + 1, yi), c = vapor_hash(xi, yi + 1), d = vapor_hash(xi + 1, yi + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
void vapor_cloud(int fi, float t, kol::FallVapor& v) {
    const kol::MapFall& f = kol::kMapFalls[fi];
    const float scale = f.foot > kol::kFallPool + 1.0f ? 0.5f : 1.0f;
    constexpr float kOut = 34.0f, kUp = 46.0f, kR = 21.6f;
    // Its box on the world's screen: the foot, each puff's farthest and its radius.
    const float a0 = f.on_x ? f.z0 : f.x0, a1 = f.on_x ? f.z1 : f.x1;
    const float plane = f.on_x ? f.x1 : f.z1;
    auto sx_of = [&](float along, float out) { return f.on_x ? (plane + out - along) * 0.5f : (along - plane - out) * 0.5f; };
    const float xs[4] = {sx_of(a0, 0.0f), sx_of(a1, 0.0f), sx_of(a0, kOut * scale), sx_of(a1, kOut * scale)};
    const float r = kR * scale + 6.0f;   // (the jitter's)
    const float wx0 = *std::min_element(xs, xs + 4) - r, wx1 = *std::max_element(xs, xs + 4) + r;
    const float wy0 = (a0 + plane) * 0.25f - (f.foot + kUp * scale) * 0.5f - r;
    const float wy1 = (a1 + plane + kOut * scale) * 0.25f - f.foot * 0.5f + r;
    v.wx0 = static_cast<int>(std::floor(wx0)) & ~1;
    v.wy0 = static_cast<int>(std::floor(wy0)) & ~1;
    v.w = (static_cast<int>(std::ceil(wx1)) - v.wx0) / 2 + 2;
    v.h = (static_cast<int>(std::ceil(wy1)) - v.wy0) / 2 + 2;
    v.cells.assign(static_cast<std::size_t>(v.w) * v.h * 2, 0.0f);
    for (float along = a0 + 4.0f; along < a1; along += 8.0f)
        for (int i = 0; i < 3; ++i) {
            const int id = fi * 97 + static_cast<int>(along) * 3 + i * 13;
            const float life = 2.2f + 1.6f * vapor_hash(id, 0);
            const float ph = t / life + vapor_hash(id, 1);
            const float q = ph - std::floor(ph);
            const int gen = static_cast<int>(std::floor(ph)) & 0xFFFF;
            const float jitter = (vapor_hash(id, 2 + gen) - 0.5f) * 10.0f + 3.0f * std::sin(t * 0.7f + static_cast<float>(id));
            const float out = (2.0f + (10.0f + 22.0f * vapor_hash(id + 1, 3 + gen)) * std::sqrt(q)) * scale;
            const float up = (2.0f + (18.0f + 26.0f * vapor_hash(id + 2, 4 + gen)) * q) * scale;
            const float rad = (9.0f + (9.0f + 9.0f * vapor_hash(id, 5)) * q) * scale * 1.2f;
            const float at = along + jitter;
            const float psx = sx_of(at, out);
            const float psy = (plane + out + at) * 0.25f - (f.foot + up) * 0.5f;
            const float strength = std::pow(std::sin(3.14159f * std::min(1.0f, q * 1.05f)), 0.6f) * (1.0f - 0.45f * q);
            const int c0 = std::max(0, static_cast<int>((psx - rad - v.wx0) / 2.0f)), c1 = std::min(v.w - 1, static_cast<int>((psx + rad - v.wx0) / 2.0f) + 1);
            const int r0 = std::max(0, static_cast<int>((psy - rad - v.wy0) / 2.0f)), r1 = std::min(v.h - 1, static_cast<int>((psy + rad - v.wy0) / 2.0f) + 1);
            for (int cy = r0; cy <= r1; ++cy)
                for (int cx = c0; cx <= c1; ++cx) {
                    const float wx = static_cast<float>(v.wx0 + cx * 2), wy = static_cast<float>(v.wy0 + cy * 2);
                    const float dx = (wx - psx) / rad, dy = (wy - psy) / rad, dd = dx * dx + dy * dy;
                    if (dd >= 1.0f) continue;
                    const float wisp = 0.55f + 0.9f * vapor_noise(wx * 0.14f + static_cast<float>(id & 255), (wy + 6.0f * t) * 0.14f);
                    const float al = std::min(1.0f, (1.0f - dd) * (1.0f - dd) * wisp * strength * 0.52f);
                    const float tone = 0.8f + 0.2f * std::clamp(0.5f - dy * 0.5f, 0.0f, 1.0f);
                    float* c = &v.cells[(static_cast<std::size_t>(cy) * v.w + cx) * 2];
                    c[0] += (1.0f - c[0]) * al;
                    c[1] += (tone - c[1]) * al;
                }
        }
}
void add_fall_vapor(const AtlasFrame& af) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    if (!map_falls_on()) return;
    const int w = af.w, h = af.h;
    const int fkx = af.kx + af.ox, fky = af.ky + af.oy;
    // (Every other picture, with the squares it fills: fx_square's key.)
    static unsigned long built = ~0ul;
    const unsigned long pair = g_cap.serial >> 1;
    if (built != pair) {
        built = pair;
        const float t = static_cast<float>((pair * 2u) % (60ul * 60ul * 24ul)) / 60.0f;
        for (int fi = 0; fi < kol::kFallCount; ++fi) vapor_cloud(fi, t, g_fall_vapor[fi]);
    }
    for (int fi = 0; fi < kol::kFallCount; ++fi) {
        const kol::MapFall& f = kol::kMapFalls[fi];
        const kol::FallVapor& v = g_fall_vapor[fi];
        const float scale = f.foot > kol::kFallPool + 1.0f ? 0.5f : 1.0f;
        // On the world's grid: a square keeps its texels as the camera pans.
        for (int ty = 0; ty < v.h * 2; ty += S)
            for (int tx = 0; tx < v.w * 2; tx += S) {
                const int sx = v.wx0 + tx + fkx, sy = v.wy0 + ty + fky;
                if (sx + S <= 0 || sy + S <= 0 || sx >= w || sy >= h) continue;
                if (static_cast<int>(g_blocks.size()) >= kMaxLiftBlocks) return;
                // Its cells: any cloud, then which texels.
                bool any = false;
                for (int cy = ty / 2; cy <= (ty + S) / 2 && cy < v.h && !any; ++cy)
                    for (int cx = tx / 2; cx <= (tx + S) / 2 && cx < v.w && !any; ++cx)
                        any = v.cells[(static_cast<std::size_t>(cy) * v.w + cx) * 2] > 0.004f;
                if (!any) continue;
                int ax0, ay0;
                const int b = atlas_new_square(&ax0, &ay0, 0);
                for (int y = 0; y < S; ++y)
                    for (int x = 0; x < S; ++x) {
                        const int cx = (tx + x) / 2, cy = (ty + y) / 2;
                        bool on = false;
                        for (int j = cy; j <= cy + 1 && j < v.h && !on; ++j)
                            for (int i = cx; i <= cx + 1 && i < v.w && !on; ++i)
                                on = v.cells[(static_cast<std::size_t>(j) * v.w + i) * 2] > 0.004f;
                        if (on) g_atlas_opaque[static_cast<std::size_t>(ay0 + y) * g_atlas_w + ax0 + x] = 1;
                    }
                // Standing in the air, this far out from the sheet, at the foot's height.
                const float wx = static_cast<float>(v.wx0 + tx + S / 2);
                const float out = 24.0f * scale;
                const float X = f.on_x ? f.x1 + out : 2.0f * wx + f.z1 + out;
                const float Z = f.on_x ? X - 2.0f * wx : f.z1 + out;
                kol::ScenePixels::SpriteBlock blk{sx, sy, S, S, b, static_cast<int>(std::lround(X + Z)),
                                                  static_cast<int>(std::lround(f.foot)), 1.0e9f, 1.0f};
                blk.prop = true;   // (goes in with the map: nearer rock covers it)
                blk.fire = true;   // (lit by itself)
                blk.effect = 3;
                blk.fx_seed = fi;
                blk.fx_cx = static_cast<float>(fkx);
                blk.fx_top = static_cast<float>(fky);
                blk.foot_row = (X + Z) * 0.25f - f.foot * 0.5f + static_cast<float>(fky);
                g_blocks.push_back(blk);
            }
    }
}

// The map's own flames (kMapFlames): a square each, filled by the fire
// effect, at the flame's place among the units' cards; their own light, the
// night leaves them. No caster (the shadow passes cast units only).
void add_map_flames(const AtlasFrame& af) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const int w = af.w, h = af.h, ox = af.ox, oy = af.oy, kx = af.kx, ky = af.ky;
    if (kol::setting_on(kol::Setting::SpecialEffects)) {
        const int map_now = current_map();
        // KOL_MAP_FLAME=<rows,width,heat>: every flame's (mockups).
        static float fe[3] = {0.0f, 0.0f, 0.0f};
        static const bool fe_on = [] {
            const char* e = std::getenv("KOL_MAP_FLAME");
            return e && std::sscanf(e, "%f,%f,%f", &fe[0], &fe[1], &fe[2]) == 3;
        }();
        for (MapFlame f : kMapFlames) {
            if (fe_on) { f.rows = static_cast<int>(fe[0]); f.width = fe[1]; f.heat = fe[2]; }
            if (f.map != map_now || static_cast<int>(g_blocks.size()) >= kMaxLiftBlocks) continue;
            const float cx = (f.x - f.z) * 0.5f + static_cast<float>(kx + ox);
            const float foot = (f.x + f.z) * 0.25f - f.y * 0.5f + static_cast<float>(ky + oy);
            const int sx = static_cast<int>(std::lround(cx)) - S / 2;
            const int sy = static_cast<int>(std::lround(foot)) + 5 - S;   // (the ragged base reaches 5 under the foot)
            if (sx + S <= 0 || sy + S <= 0 || sx >= w || sy >= h) continue;
            int ax0, ay0;
            const int b = atlas_new_square(&ax0, &ay0, 1);
            kol::ScenePixels::SpriteBlock blk{sx, sy, S, S, b, static_cast<int>(std::lround(f.x + f.z)),
                                              static_cast<int>(std::lround(f.y)), 1.0e9f, 1.0f};
            // With the units (after the map, among them by depth), not a prop:
            // as a prop it went before every unit, and one behind the wall
            // showed its outline over the flame (states/thor/0052).
            blk.fire = true;
            blk.effect = 1;
            blk.fx_cx = cx;
            blk.foot_row = foot;
            blk.fx_top = foot - static_cast<float>(f.rows);
            blk.fx_seed = static_cast<int>(f.x * 7.0f + f.z * 13.0f) & 1023;
            blk.fx_over = true;
            blk.fx_heat = f.heat;
            blk.fx_width = f.width;
            blk.fx_palette = f.palette;   // (65: the campfire's deeper red and orange, the wood behind the sconce lit yellow)
            g_blocks.push_back(blk);
        }
    }
}
}  // namespace

namespace kol_focus_detail {
bool map_falls_on() {
    static const bool off = std::getenv("KOL_NO_FALLS") != nullptr;
    return !off && current_map() == kol::kFallsMap && kol::setting_on(kol::Setting::SpecialEffects);
}
void build_sprite_atlas(const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky,
                        const Unit* units, int count, int unit_count) {
    g_atlas_on = false;
    g_blocks.clear();
    g_casters.clear();
    g_fire_spots.clear();
    if (fire_logs_shade() < 1.0f) g_fire_logs.assign(static_cast<std::size_t>(w) * h, 0);
    else g_fire_logs.clear();
    g_fire_logs_any = false;
    g_fire_seen = false;
    g_lift_ao_on = false;
    // No pixel is lifted until this frame says so: an early return below
    // left last frame's marks, and the props and walls were then lifted
    // against them with no blocks (FN: after View List the game keeps its
    // windows on; lifting died and a prop's sliver blinked every 9 frames).
    g_lift.assign(static_cast<std::size_t>(w) * h, 0);
    g_lift_copy.assign(static_cast<std::size_t>(w) * h, 0);
    g_over_sprite.assign(static_cast<std::size_t>(w) * h, 0);
    g_over_block.assign(static_cast<std::size_t>(w) * h, 0);
    g_over_sprite_any = false;
    g_over_seeds.clear();
    g_atlas_bailed = true;
    g_wall_px_any = false;
    // Whether units cast a shadow of their own (the sun's, or a dark map's
    // lamps': cast_lamp_shadows), so the game's dark blob under them is left
    // out (or a cone window's: cast_window_shadows). By the sun's alone, map 51 (indoors since 2026-10-14: no sun)
    // got the blobs back under the units the pots cast shadows from (the
    // user reported the black circles back under the units, GT).
    const bool cast = units_cast() || units_cast_from_lamps() || units_cast_from_windows();
    const Capture& cap = g_cap;
    if (!cap.valid) return;
    const std::uint8_t* tiles = cap.obj_ok ? cap.obj_tiles.data() : nullptr;
    const std::uint8_t* pal = cap.obj_ok ? cap.pal.data() : nullptr;
    if (!tiles || !pal) return;
    // Colour effects on sprites. Brightness (BLDCNT effect 2/3 with OBJ a
    // first target, BLDY) is applied to the texels the frame does not show,
    // as the PPU applies it (a menu darkens every sprite at 8/16:
    // states/BK_tree_glass_cage). Alpha blending needs what lies under a
    // hidden texel, and windows can gate the effect per pixel: with either,
    // units stay in the frame.
    const unsigned bld = cap.bldcnt;
    const unsigned effect = (bld >> 6) & 3u;
    const bool obj_target = (bld & 0x10u) != 0u;
    const unsigned evy = std::min(16u, static_cast<unsigned>(cap.bldy & 0x1Fu));
    // Windows can leave the effect off in some regions: outside (WINOUT bit
    // 5) and each enabled window (WIN0 WININ bit 5, WIN1 bit 13, the OBJ
    // window WINOUT bit 13). The atlas colours the units' texels from the
    // palette, so it takes the outside region's rule, where units stand.
    // FN: after View List the game darkens sprites 8/16 for its menu except
    // in WIN1 and the OBJ window (its box and cursor), and keeps those windows
    // on; bailing out there left the units in the frame for the rest of the
    // battle and, with last frame's marks, lifting dead and a prop's sliver
    // blinking every 9 frames.
    const unsigned dc = cap.dispcnt;
    const bool gated = (dc & 0xE000u) != 0u &&
                       (!(cap.winout & 0x20u) || ((dc & 0x2000u) && !(cap.winin & 0x20u)) ||
                        ((dc & 0x4000u) && !(cap.winin & 0x2000u)) ||
                        ((dc & 0x8000u) && !(cap.winout & 0x2000u)));
    if (obj_target && effect == 1u) return;
    const bool brightness = obj_target && effect >= 2u && evy != 0u && (!gated || (cap.winout & 0x20u));
    // A semi-transparent sprite (OBJ mode 1) always alpha-blends and never
    // takes brightness. At BLDALPHA 16/16 over 0/16 that blend is the sprite
    // itself: the snowfield draws every unit so in its menus
    // (states/BK_tree_glass_cage). At any other weight (the deployment
    // ghost's 8/8) the unit stays in the frame.
    const unsigned bla = cap.bldalpha;
    const unsigned eva = std::min(16u, bla & 0x1Fu), evb = std::min(16u, (bla >> 8) & 0x1Fu);
    const std::uint8_t* under = cap.under_ok ? cap.under.data() : nullptr;
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const int per_row = w / S;
    if (per_row <= 0) return;
    const int rows = (kMaxSpriteBlocks + per_row - 1) / per_row;
    g_atlas_w = w;
    g_atlas_h = rows * S;
    g_atlas_per_row = per_row;
    const std::size_t apx = static_cast<std::size_t>(g_atlas_w) * g_atlas_h;
    g_atlas_rgb.assign(apx * 3, 0);
    g_atlas_opaque.assign(apx, 0);
    g_atlas_blur.assign(apx, 0);
    g_atlas_bailed = false;   // (g_lift cleared at the top)
    const auto t0 = std::chrono::steady_clock::now();
    const bool blob_split = blob_join_on();   // for A/B runs
    const AtlasFrame af{layers, w, h, ox, oy, kx, ky, units, count, unit_count, cast, tiles, pal,
                        effect, evy, brightness, eva, evb, under, per_row};
    bool wall_unit[kObjects] = {};
    std::uint32_t hidden_decals = 0;
    struct PublishDecals {
        std::uint32_t& v;
        ~PublishDecals() { g_hidden_decals.store(v, std::memory_order_relaxed); }
    } publish_decals{hidden_decals};
    place_wall_objects(af, wall_unit, hidden_decals);
    for (int u = 0; u < count && static_cast<int>(g_blocks.size()) < kMaxSpriteBlocks; ++u) {
        if (u < kObjects && wall_unit[u]) continue;
        int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
        bool any = false, plain = true, semi = true;
        int mine[16], nm = 0;   // the unit's entries, in OAM order
        gather_unit_entries(af, u, blob_split, mine, nm, x0, y0, x1, y1, any, plain, semi);
        std::sort(mine, mine + nm);
        // Up to kBigCard (a rider on his horse, joined by join_parts) the
        // figure is cut into columns of at most a slot, every square at its
        // place, as a big prop's are.
        const bool big = x1 - x0 > S || y1 - y0 > kTallCard;
        if (!any || !plain || (big && (x1 - x0 > kBigCard || y1 - y0 > kBigCard))) continue;
        // Taller than a slot (kTallCard): its top rows a card of their own
        // first (`upper`), then the slot over its feet; both at its place.
        const int uy0 = y0, uy1 = y1, ux0 = x0, ux1 = x1;
        const int ncol = (ux1 - ux0 + S - 1) / S;
        for (int col = 0; col < ncol; ++col) {
        const int cx0 = ux0 + (ux1 - ux0) * col / ncol, cx1 = ux0 + (ux1 - ux0) * (col + 1) / ncol;
        for (int part = uy1 - uy0 > S ? 0 : 1; part < 2; ++part) {
        if (static_cast<int>(g_blocks.size()) >= kMaxSpriteBlocks) break;
        const bool upper = part == 0;
        const int ry0 = upper ? uy0 : std::max(uy0, uy1 - S), ry1 = upper ? uy1 - S : uy1;
        const int b = static_cast<int>(g_blocks.size());
        const auto [ax0, ay0] = kol::slot_origin(b, per_row);
        // The terrain's shadow over the unit, as over a card standing at its
        // place: its feet are at height y - 8 on hardware row feet_hy, and a
        // row up is 2 units higher, so the rows below cut_hy are under the
        // shade's height there and darken as the ground does.
        float cut_hy = 1.0e9f;
        float shade_k = 1.0f;
        {
            int top = shade_height(units[u].x, units[u].z);
            const int foot = units[u].y - 8;
            // Under an awning: shaded up to where its shadow ends.
            int c_lo = 0, c_hi = 0;
            if (canopy_range(units[u].x, units[u].z, &c_lo, &c_hi) && foot + 8 >= c_lo && foot + 8 < c_hi)
                top = std::max(top, c_hi);
            if (top != kNoShade && top > foot) {
                const float feet_hy = feet_sy(units[u]) + ky;
                cut_hy = feet_hy - 0.5f * static_cast<float>(top - foot);
                shade_k = 1.0f - shadow_strength();
            }
        }
        static thread_local std::vector<std::uint32_t> over_seeds;
        over_seeds.clear();
        fill_unit_card(af, u, mine, nm, cx0, cx1, ry0, ry1, b, ax0, ay0, cut_hy, shade_k, over_seeds);
        // What covered it, grown over its surface later (grow_over_sprite).
        if (!over_seeds.empty())
            g_over_seeds.push_back(OverSeeds{over_seeds, std::max(0, cx0 + ox - 64), std::max(0, ry0 + oy - 64),
                                             std::min(w - 1, cx1 + ox + 64), std::min(h - 1, ry1 + oy + 64),
                                             units[u].x + units[u].z, units[u].y, u, b});   // (y: the ground it stands on)
        // Its blob out of its frame (g_blob_of): with Unit Shadows on, lifted
        // with no texel, so the ground the game drew under it shows there, as
        // under the blob in its frame (below).
        if (cast && u < unit_count && !upper)
            for (int k = 0; k < g_entry_n; ++k) {
                if (g_blob_of[k] != u) continue;
                const ObjEntry& e = g_entries[k];
                for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py)
                    for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                        const std::size_t p = static_cast<std::size_t>(py) * w + px;
                        if (g_obj_top[p] == k && scene_top(layers, p) == kol::kLayerObj)
                            g_lift[p] = static_cast<std::uint8_t>(b + 1);
                    }
            }
        g_blocks.push_back({cx0 + ox, ry0 + oy, cx1 - cx0, ry1 - ry0, b,
                            units[u].x + units[u].z, units[u].y, cut_hy + oy, shade_k});
        g_blocks.back().fade = units[u].fade;
        if (nm > 0) g_blocks.back().oam_first = g_entries[mine[0]].oam;
        // A campfire the scene makes as an object (kFireActors): with
        // Options > Special Effects, drawn by the fire effect over its
        // square, as the burning town's flames, lit by itself.
        if (fire_actor(units[u].actor) && kol::setting_on(kol::Setting::SpecialEffects))
            campfire_block(af, u, b, ax0, ay0, ry0);
        g_blocks.back().scenery = scene_object(units[u]);
        // (A bird over kSkyY casts nothing: its shadow fell on the sail beside
        // it, grey smudges round the gulls on map 117.)
        g_casters.push_back({u < unit_count && !flies_high(u, units[u]), units[u].x, units[u].z, units[u].y - 8,
                             static_cast<float>(feet_sx(units[u]) + kx),
                             static_cast<float>(feet_sy(units[u]) + ky),
                             cx0, ry0,
                             u < static_cast<int>(g_unit_ground.size()) && g_unit_ground[u] != kNoGround
                                 ? g_unit_ground[u] : units[u].y,
                             (semi ? static_cast<float>(eva) / 16.0f : 1.0f) * units[u].fade, upper,
                             u < unit_count && u < 64 && g_unit_hanging[u] &&
                                 (u >= static_cast<int>(g_unit_ground.size()) || g_unit_ground[u] == kNoGround)});
        // A campfire (kFireActors) darkens nothing: no contact occlusion
        // under it, no shadow (the user, map 63, asked for no ambient occlusion
        // on the firepit).
        // And it is its own light: with Special Effects off, the game's
        // flame sprite took the dark map's darkness (no unit, so no unit's
        // lamp light either) and showed nearly black.
        if (fire_actor(units[u].actor)) {
            g_blocks.back().fire = true;
            g_casters.back().unit = false;   // (every shadow and occlusion pass casts units only)
            g_fire_spots.push_back({units[u].x, units[u].z});
        }
        }
        }   // (col)
    }
    add_map_flames(af);
    add_fall_vapor(af);
    g_atlas_on = !g_blocks.empty();
    // KOL_SPRITE_DEBUG=1: the units drawn from their own sprites, a frame.
    static const bool sprite_debug = kol::env_set("KOL_SPRITE_DEBUG");
    if (sprite_debug) {
        long lifted = 0;
        for (std::uint8_t v : g_lift) lifted += v != 0;
        std::fprintf(stderr, "[sprites] blocks=%zu lifted=%ld bldcnt=%04x %.0f us\n", g_blocks.size(), lifted, bld,
                     std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        for (const auto& b : g_blocks)
            std::fprintf(stderr, "   slot %d at (%d,%d) %dx%d xz=%d y=%d\n",
                         b.slot, b.sx, b.sy, b.w, b.h, b.xz, b.y);
    }
}
}  // namespace kol_focus_detail

namespace {
// lift_prop_pixels' props: what it lifted, for KOL_SPRITE_DEBUG.
struct PropLift { std::size_t props = 0; int lifted = 0; };

// The props' pixels lifted (lift_prop_pixels' first part).
PropLift lift_props(const std::uint8_t* layers, int w, int h, bool props_on, int ox, int oy, int kx, int ky) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    // Group by the prop's place (every pixel of a prop has its prop's).
    const bool fire_map = fire_map_now();
    struct Prop { int xz, y, x0, y0, x1, y1, cell; bool flame, smoke; };
    std::vector<Prop> props;
    // (Kept between frames: a fresh scene-sized buffer is mapped, faulted in
    // and unmapped every frame.)
    static thread_local std::vector<int> prop_of;
    prop_of.assign(n, -1);
    std::unordered_map<std::uint64_t, int> prop_index;
    int last_pi = -1;
    for (std::size_t k = 0; k < n; ++k) {
        // (BG1 on top; or under interface, see the pass after the claim.)
        const bool bg1 = scene_top(layers, k) == 1u ||
                         (g_iface[k] && g_scene_codes && (g_scene_codes[k] & 0x0Fu) == kol::kLayerObj &&
                          (g_scene_codes[k] >> 4) == 1u);
        const bool v_bg0 = g_spire_v.size() == n && g_spire_v[k] && scene_top(layers, k) == 0u;
        if (!props_on || g_prop_dist[k] == kUnreached || !(bg1 || v_bg0) || g_lift[k]) continue;
        if (fire_map && !is_flame(k)) continue;   // only the flames and smoke there (fire_map_now)
        // By place and by the cell that claimed it: trees on one diagonal at
        // one height share a place, and were one prop (a row of the
        // snowfield's firs, 160 px wide) whose plant flag one cell decided.
        static const bool by_column = std::getenv("KOL_PERSP_PLANT_BY_COLUMN") != nullptr;
        const int cell = !by_column && g_prop_cell.size() == n ? g_prop_cell[k] : -1;
        // The previous pixel's prop, else by hash (a search of every prop
        // per pixel cost the worker 1.5 ms on map 102's fifty).
        int pi = -1;
        if (last_pi >= 0 && props[last_pi].xz == g_xz[k] && props[last_pi].y == g_y[k] && props[last_pi].cell == cell)
            pi = last_pi;
        const std::uint64_t pkey = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(g_xz[k])) << 40) ^
                                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(g_y[k]) & 0xFFFFFu) << 20) ^
                                   static_cast<std::uint32_t>(cell + 1);
        if (pi < 0)
            if (auto it = prop_index.find(pkey); it != prop_index.end()) pi = it->second;
        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
        if (pi < 0) {
            pi = static_cast<int>(props.size());
            prop_index.emplace(pkey, pi);
            props.push_back({g_xz[k], g_y[k], px, py, px, py, cell, is_flame(k), is_smoke(k)});
        }
        Prop& p = props[pi];
        p.x0 = std::min(p.x0, px); p.y0 = std::min(p.y0, py);
        p.x1 = std::max(p.x1, px); p.y1 = std::max(p.y1, py);
        prop_of[k] = pi;
        last_pi = pi;
    }
    int lifted = 0;
    // Options > Special Effects: a flame or the smoke drawn by an effect of
    // our own (kol_perspective), in squares a whole slot wide about its
    // middle column, every texel the effect's; the game's pixels lifted as
    // before, so the scene shows what lies behind them.
    const bool effects = kol::setting_on(kol::Setting::SpecialEffects);
    // The art's flames our effect draws anew (MapFlame::art_half): screen boxes.
    // KOL_FX_ART_KEEP=1 keeps them.
    struct ArtBox { int x0, y0, x1, y1; };
    std::vector<ArtBox> art_boxes;
    static const bool keep_art = std::getenv("KOL_FX_ART_KEEP") != nullptr;
    if (effects && !keep_art) {
        const int map_now = current_map();
        for (const MapFlame& f : kMapFlames) {
            if (f.map != map_now || f.art_half <= 0) continue;
            const int cx = static_cast<int>(std::lround((f.x - f.z) * 0.5f)) + kx + ox;
            const int foot = static_cast<int>(std::lround((f.x + f.z) * 0.25f - f.y * 0.5f)) + ky + oy;
            art_boxes.push_back({cx - f.art_half, foot - f.rows - 2, cx + f.art_half, foot});
        }
    }
    auto art_flame = [&](std::size_t k) {
        if (art_boxes.empty() || scene_top(layers, k) != 1u) return false;
        const int x = static_cast<int>(k % w), y = static_cast<int>(k / w);
        for (const ArtBox& a : art_boxes)
            if (x >= a.x0 && x <= a.x1 && y >= a.y0 && y <= a.y1) return true;
        return false;
    };
    for (int pi = 0; pi < static_cast<int>(props.size()); ++pi) {
        const Prop& p = props[pi];
        const bool fx = effects && p.flame;
        // Its tiles' middle and top (g_flame_cx/top), steady; else its pixels'.
        const int fi = -2 - p.cell;
        const bool tiled = fx && fi >= 0 && static_cast<std::size_t>(fi) < g_flame_cx.size();
        const float fx_cx = tiled ? g_flame_cx[fi] : 0.5f * static_cast<float>(p.x0 + p.x1 + 1);
        const int fx_x0 = static_cast<int>(std::lround(fx_cx)) - S / 2;
        // An effect's tongues and wisps rise over the game's: its squares
        // start a little higher.
        const int top0 = tiled ? std::min(p.y0, static_cast<int>(g_flame_top[fi])) : p.y0;
        const int fx_y0 = fx ? std::max(0, top0 - (p.smoke ? 8 : 12)) : p.y0;
        // ... and a fire's ragged base reaches a few rows under its foot.
        const int fx_y1 = fx && !p.smoke ? std::min(h - 1, p.y1 + 5) : p.y1;
        for (int ty = fx_y0; ty <= fx_y1; ty += S)
            for (int tx = fx ? fx_x0 : p.x0; tx <= (fx ? fx_x0 : p.x1); tx += S) {
                if (static_cast<int>(g_blocks.size()) >= kMaxLiftBlocks) break;
                const int bw = fx ? S : std::min(S, p.x1 + 1 - tx), bh = std::min(S, fx_y1 + 1 - ty);
                bool any = false;
                for (int y = ty; y < ty + bh && !any; ++y)
                    for (int x = std::max(0, tx); x < std::min(w, tx + bw) && !any; ++x)
                        any = prop_of[static_cast<std::size_t>(y) * w + x] == pi;
                if (!any && !fx) continue;
                // The atlas grows by rows of squares as needed.
                int ax0, ay0;
                const int b = atlas_new_square(&ax0, &ay0, 0);
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        if (tx + x < 0 || tx + x >= w) continue;
                        if (fx) g_atlas_opaque[static_cast<std::size_t>(ay0 + y) * g_atlas_w + ax0 + x] = 1;
                        const std::size_t k = static_cast<std::size_t>(ty + y) * w + tx + x;
                        if (prop_of[k] != pi) continue;
                        // (An art flame ours replaces: lifted, the scene behind
                        // it showing, and not drawn.)
                        g_atlas_opaque[static_cast<std::size_t>(ay0 + y) * g_atlas_w + ax0 + x] =
                            fx || !art_flame(k) ? 1 : 0;
                        if (g_spire_v.size() == n && g_spire_v[k] && scene_top(layers, k) == 0u) {
                            // A spire's V of BG0: on the card, and still the
                            // terrain's (see g_lift_copy).
                            if (g_lift_copy.size() == n) g_lift_copy[k] = static_cast<std::uint8_t>(b + 1);
                            continue;
                        }
                        g_lift[k] = static_cast<std::uint8_t>(b + 1);
                        g_under_xz[k] = g_pre_xz[k];
                        g_under_y[k] = g_pre_y[k];
                        g_under_kind[k] = g_pre_kind[k];
                        ++lifted;
                    }
                kol::ScenePixels::SpriteBlock blk{tx, ty, bw, bh, b, p.xz, p.y, 1.0e9f, 1.0f};
                blk.prop = true;
                if (p.cell >= 0 && p.cell < kGridSide * kGridSide && g_crystal_cell[p.cell]) blk.crystal = p.cell;
                if (p.cell >= 0 && static_cast<std::size_t>(p.cell) < g_spire_past.size() && g_spire_past[p.cell])
                    blk.sort_extra = 32;
                if (p.cell >= 0 && static_cast<std::size_t>(p.cell) < g_card_bend.size()) {
                    blk.bend_sx = g_card_bend[p.cell].sx;
                    blk.bend_tip = g_card_bend[p.cell].tip;
                    blk.bend_dyl = g_card_bend[p.cell].dyl;
                    blk.bend_dyr = g_card_bend[p.cell].dyr;
                }
                blk.fire = p.flame;
                blk.glow_flame = !p.flame && kol::map_flag(kol::MapFlag::GlowFlame, current_map());
                if (fx) {
                    blk.effect = p.smoke ? 2 : 1;
                    blk.fx_cx = fx_cx;
                    blk.fx_top = static_cast<float>(fx_y0);
                    blk.fx_seed = (p.xz * 7 + p.y * 13) & 1023;
                }
                // Smoke the game blends over BG0 (BG1 first target, alpha
                // blending, EVA / 16 of the smoke plus EVB / 16 of what is
                // behind: map 100 8 and 11, more than one in all): see-through,
                // drawn premultiplied, what the warp puts behind it kept at
                // EVB / 16 (see_through = 1 - EVB / 16) and its texels the
                // smoke's EVA / 16 share (kol_perspective).
                if (p.smoke && (g_cap.bldcnt & 0x02u) && ((g_cap.bldcnt >> 6) & 3u) == 1u) {
                    const unsigned evb = std::min(16u, (g_cap.bldalpha >> 8) & 0x1Fu);
                    if (evb > 0u) blk.see_through = 1.0f - static_cast<float>(evb) / 16.0f;
                }
                // The terrain's shadow over it, as over a unit, at the
                // square's middle column (see card_shade_row).
                if (props_shaded() && !p.flame) {
                    const float dxz = 2.0f * (static_cast<float>(tx) + 0.5f * bw - ox - kx);
                    const int X = static_cast<int>(std::lround(0.5f * (p.xz + dxz)));
                    const int Z = static_cast<int>(std::lround(0.5f * (p.xz - dxz)));
                    const float foot_row = static_cast<float>(p.xz) / 4.0f - static_cast<float>(p.y) / 2.0f + ky + oy;
                    blk.shade_row = card_shade_row(X, Z, p.y, foot_row);
                    if (blk.shade_row < 1.0e8f) blk.shade_k = 1.0f - shadow_strength();
                }
                if (!p.flame) prop_base_params(&blk.base_rows, &blk.base_dark, &blk.base_raise);
                blk.foot_row = static_cast<float>(p.xz) / 4.0f - static_cast<float>(p.y) / 2.0f + ky + oy;
                {
                    // A plant sways (kol_perspective): by the cell that
                    // claimed the prop (g_prop_cell), and its place under its
                    // middle column for the wind's phase.
                    const float dxz = 2.0f * (0.5f * static_cast<float>(p.x0 + p.x1 + 1) - ox - kx);
                    const float X = 0.5f * (p.xz + dxz), Z = 0.5f * (p.xz - dxz);
                    const int cx = static_cast<int>(std::floor((X - 1.0f) / 32.0f));
                    const int cz = static_cast<int>(std::floor((Z - 1.0f) / 32.0f));
                    blk.plant = p.flame ? false
                              : p.cell <= -100 ? placed_plant(-100 - p.cell)
                              : p.cell < 0 ? in_grid(cx, cz) && g_plant_cell[gi(cx, cz)] : g_plant_cell[p.cell];
                    blk.wind_x = X;
                    blk.wind_z = Z;
                    // The statue carrying its flame on a dark map (see
                    // g_self_lamp_cell): by its claiming cell, else its place.
                    const int sc = p.flame ? -1 : p.cell >= 0 ? p.cell : in_grid(cx, cz) ? gi(cx, cz) : -1;
                    if (sc >= 0 && static_cast<std::size_t>(sc) < g_self_lamp_cell.size() && g_self_lamp_cell[sc] > 0) {
                        static const float self_k = kol::env_num_min("KOL_STATUE_SELF", 0.0f, 1.0f);
                        blk.self_lit = self_k;
                        // Its lifted texels' span (a row is 2 units): the
                        // part below (the pit, the vase) is the scene's, dark,
                        // so the fade ends at its lowest texel, not its foot.
                        blk.self_top = static_cast<float>(p.y) + 2.0f * (blk.foot_row - static_cast<float>(p.y0));
                        blk.self_low = static_cast<float>(p.y) + 2.0f * (blk.foot_row - static_cast<float>(p.y1 + 1));
                        blk.self_lamp = g_self_lamp_cell[sc] - 1;
                    }
                }
                g_blocks.push_back(blk);
            }
    }
    return {props.size(), lifted};
}

// BG1 terrain over BG0 art (the castle's wall pillars, the wall behind
// them on BG0): where the terrain pass drew over another surface there,
// the pixel is lifted onto its own patch (its own surface) and the scene
// shows BG0 at that surface behind. Tiles of 32 on the scene's grid.
// (lift_prop_pixels' second part.) The pixels lifted: surface_px.
int lift_wall_surfaces(int w, int h, bool walls_on) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    int surface_px = 0;
    // The occlusion: the lifted texels take the surface's (g_lift_ao), the
    // scene there the surface's behind (it was the front's).
    const bool swap_ao = g_ao.size() == n && g_back_ao.size() == n;
    if (walls_on && swap_ao && g_lift_ao.size() != n) g_lift_ao.assign(n, 255);
    g_fx_under_fix.clear();
    if (walls_on && g_scene_codes && g_back_kind.size() == n) {
        static thread_local std::vector<std::uint8_t> want;
        want.assign(n, 0);
        static const bool keep_fx_art = std::getenv("KOL_FX_ART_KEEP") != nullptr;
        const bool fx_art_on = !keep_fx_art && g_modeled.size() == n && kol::setting_on(kol::Setting::SpecialEffects);
        const int fx_map = current_map();
        static thread_local std::vector<std::size_t> fx_paint;
        fx_paint.clear();
        auto fx_px = [&](std::size_t k) { return fx_art_on && g_modeled[k] && model_part_fx_art(fx_map, g_modeled[k] - 1); };
        const bool wall_behind = g_modeled.size() == n && in_list(kModelWallBehindMaps, fx_map);
        for (std::size_t k = 0; k < n; ++k) {
            if (g_lift[k] || g_prop_dist[k] != kUnreached || g_kind[k] == kol::kSceneNone) continue;
            if (g_no_wall_lift.size() == n && g_no_wall_lift[k]) continue;
            // (An awning's BG0 cloth too: g_awning 2.)
            const bool bg0_cloth = g_awning.size() == n && g_awning[k] == 2;
            // (And a model's rope over its other parts: g_rope_air.)
            const bool rope_air = g_rope_air.size() == n && g_rope_air[k];
            // (And a scene actor's sprite the model stands for: g_model_actor_px.)
            const bool actor_px = g_model_actor_px.size() == n && g_model_actor_px[k];
            // An art flame the effect draws anew (kModelFxArt) lifts however
            // close to its wall (its card stands 7.6 out, under the 8 below)
            // and is not drawn (fx_art below): the wall shows behind the
            // effect's flame. Left, dimmed by the dark, it stood round the
            // effect's as a dark red rim (map 66). Over nothing on BG0 (the
            // wall on BG1 alone) the wall is painted in (fx_paint).
            if (fx_px(k) && (g_scene_codes[k] & 0x0Fu) == 1u && g_back_kind[k] != kol::kSceneNone) {
                if ((g_scene_codes[k] >> 4) == 0u) {
                    want[k] = 1;
                } else if (g_scene_rgb) {
                    want[k] = 1;
                    fx_paint.push_back(k);
                }
                continue;
            }
            // A model's hanging part before a wall the art drew on BG1 alone, nothing
            // on BG0 behind it (kModelWallBehindMaps: map 84's crystal tops before the
            // slab of cliff): lifted all the same, the wall behind it painted in from
            // its own column (fx_paint), as under an art flame. Left in the scene, the
            // spikes' tops stood a cluster's depth before the slab beside them and the
            // rows between stretched.
            if (wall_behind && g_modeled[k] && g_scene_codes[k] == 0x51u && g_back_kind[k] == kol::kSceneUpright &&
                g_scene_rgb && (std::abs(g_xz[k] - g_back_xz[k]) > 8 || std::abs(g_y[k] - g_back_y[k]) > 8)) {
                want[k] = 1;
                fx_paint.push_back(k);
                continue;
            }
            if (!bg0_cloth && !rope_air && !actor_px && ((g_scene_codes[k] & 0x0Fu) != 1u || (g_scene_codes[k] >> 4) != 0u)) continue;
            // (3, 4: a rope over the sky, nothing behind it: it opens onto the sky.)
            const bool rope_sky = rope_air && g_rope_air[k] >= 3;
            if (rope_sky) {
                want[k] = 1;
                continue;
            }
            if (g_back_kind[k] == kol::kSceneNone) continue;
            if (std::abs(g_xz[k] - g_back_xz[k]) <= 8 && std::abs(g_y[k] - g_back_y[k]) <= 8) continue;
            // Not a block's own art over the top one step down behind it: a
            // raised block's BG1 (map 16's stone slab and grass tops, a
            // step over the cell behind) is that block's art over BG0 that
            // is the block too, which was put at the top behind it and slid
            // with that as the camera dropped: the block tore and showed what
            // is behind (states/thor/0083, the user asked to see only the
            // raised terrain, not what is behind it). On map
            // 16 every lifted pixel was such (a flat back at most a step
            // down, 16, and a cell back, 32: 4329 of them); the castle's,
            // map 6's and map 39's are mostly farther (a step's edge over the
            // floor further down, a roof over the street), and still lift,
            // as over a wall (the castle's pillars) and a model's pixels.
            // KOL_WALL_LIFT_ANY_BACK=1: over any surface, as before.
            static const bool any_back = std::getenv("KOL_WALL_LIFT_ANY_BACK") != nullptr;
            static thread_local int step_map = -1;
            static thread_local bool step_lift = false;
            if (step_map != fx_map) { step_map = fx_map; step_lift = in_list(kWallLiftStepMaps, fx_map); }
            if (!any_back && !step_lift && g_back_kind[k] != kol::kSceneUpright && !(g_modeled.size() == n && g_modeled[k]) &&
                g_y[k] - g_back_y[k] <= 16 && g_xz[k] - g_back_xz[k] <= 32)
                continue;
            want[k] = 1;
        }
        // fx_paint: the wall under an art flame where BG0 has nothing (the sky):
        // the nearest pixel of the wall up or down its own column (the
        // pilaster's edges run on through), not an art flame's itself.
        for (const std::size_t k : fx_paint) {
            const int x = static_cast<int>(k % w), y = static_cast<int>(k / w);
            const std::uint8_t* c = nullptr;
            for (int d = 1; d <= 48 && !c; ++d)
                for (const int qy : {y - d, y + d}) {
                    if (qy < 0 || qy >= h) continue;
                    const std::size_t q = static_cast<std::size_t>(qy) * w + x;
                    if (fx_px(q) || (g_scene_codes[q] & 0x0Fu) != 1u || g_kind[q] != kol::kSceneUpright) continue;
                    if (wall_behind && g_modeled[q]) continue;   // (another spike: not the wall)
                    c = g_scene_rgb + q * 3;
                    break;
                }
            if (!c) { want[k] = 0; continue; }
            g_fx_under_fix.push_back({static_cast<std::uint32_t>(k), {c[0], c[1], c[2]}});
        }
        // A square holds one surface. A corner is the nearest of its pixels
        // touching it, so where a model's sail, yard, ropes and mast share a
        // square at depths far apart, a far pixel's quad stretched to a near
        // one's corner, 60 rows under the stick: a model's pixels are cut
        // into pieces of one part whose neighbours lie close (kPieceDy in
        // height, kPieceDxz in depth), each piece a square of its own. Two
        // ropes crossing in a square are two pieces. Terrain stays whole.
        // Whose place the layer under a lifted pixel takes, by what the lifted
        // pixels border (g_inlay: 1 the wall's own, 2 the ground behind's). A
        // wall's pixel beside pixels left in the scene that carry on its own
        // surface (a few BG1 pixels set into a parapet's top that BG0 draws too,
        // a block's face running down into the wall under it) keeps its place
        // under it as well; one beside the surface behind it (the floor past a
        // block's top edge, the wall either side of a pillar) lets that surface
        // run on under it, which is where the camera uncovers it. The pixels
        // inside take their nearest border's. Laid at the ground behind's with
        // nothing of that ground beside it, the layer under was a patch torn out
        // of the top round it, a dark wedge below the wall's pixels with the
        // stick held down (map 36, Thor state 0272). KOL_LIFT_NO_INLAY=1: every
        // pixel the ground behind's.
        static const bool no_inlay = kol::env_set("KOL_LIFT_NO_INLAY");
        static thread_local std::vector<std::uint8_t> g_inlay;
        static thread_local std::vector<std::size_t> inlay_q;
        g_inlay.assign(no_inlay ? 0 : n, 0);
        if (!no_inlay) {
            inlay_q.clear();
            constexpr int kR = 2, kDy = 6, kDxz = 16;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * w + x;
                    if (!want[k]) continue;
                    int own = 0, back = 0;
                    for (int yy = std::max(0, y - kR); yy <= std::min(h - 1, y + kR); ++yy)
                        for (int xx = std::max(0, x - kR); xx <= std::min(w - 1, x + kR); ++xx) {
                            const std::size_t q = static_cast<std::size_t>(yy) * w + xx;
                            if (want[q] || g_lift[q] || g_kind[q] == kol::kSceneNone) continue;
                            own += std::abs(g_y[q] - g_y[k]) <= kDy && std::abs(g_xz[q] - g_xz[k]) <= kDxz;
                            back += std::abs(g_y[q] - g_back_y[k]) <= kDy && std::abs(g_xz[q] - g_back_xz[k]) <= kDxz;
                        }
                    if (own + back == 0) continue;
                    g_inlay[k] = own > back ? 1 : 2;
                    inlay_q.push_back(k);
                }
            for (std::size_t i = 0; i < inlay_q.size(); ++i) {
                const std::size_t k = inlay_q[i];
                const int cx = static_cast<int>(k % w), cy = static_cast<int>(k / w);
                static constexpr int k4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : k4) {
                    const int nx = cx + d[0], ny = cy + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::size_t q = static_cast<std::size_t>(ny) * w + nx;
                    if (!want[q] || g_inlay[q]) continue;
                    g_inlay[q] = g_inlay[k];
                    inlay_q.push_back(q);
                }
            }
        }
        const bool by_part = g_modeled.size() == n;
        constexpr int kPieceDy = 8, kPieceDxz = 16;
        static thread_local std::vector<int> piece;
        static thread_local std::vector<std::size_t> stack;
        piece.assign(by_part ? n : 0, 0);
        for (int ty = 0; ty < h; ty += S)
            for (int tx = 0; tx < w; tx += S) {
                const int bw = std::min(S, w - tx), bh = std::min(S, h - ty);
                int pieces = 0;
                for (int y = ty; y < ty + bh; ++y)
                    for (int x = tx; x < tx + bw; ++x) {
                        const std::size_t k0 = static_cast<std::size_t>(y) * w + x;
                        if (!want[k0]) continue;
                        if (!by_part || !g_modeled[k0]) { if (by_part) piece[k0] = -1; continue; }
                        if (piece[k0]) continue;
                        piece[k0] = ++pieces;
                        stack.assign(1, k0);
                        while (!stack.empty()) {
                            const std::size_t k = stack.back();
                            stack.pop_back();
                            const int cx = static_cast<int>(k % w), cy = static_cast<int>(k / w);
                            static constexpr int k4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                            for (const auto& d : k4) {
                                const int nx = cx + d[0], ny = cy + d[1];
                                if (nx < tx || ny < ty || nx >= tx + bw || ny >= ty + bh) continue;
                                const std::size_t q = static_cast<std::size_t>(ny) * w + nx;
                                if (!want[q] || piece[q] || g_modeled[q] != g_modeled[k]) continue;
                                if (std::abs(g_y[q] - g_y[k]) > kPieceDy || std::abs(g_xz[q] - g_xz[k]) > kPieceDxz)
                                    continue;
                                piece[q] = pieces;
                                stack.push_back(q);
                            }
                        }
                    }
                // Piece 0: the terrain's (and every pixel without a model).
                for (int group = by_part ? -1 : 0; group <= (by_part ? pieces : 0); ++group) {
                if (by_part && group == 0) continue;
                auto in_group = [&](std::size_t k) {
                    if (!want[k]) return false;
                    // (Not a slit's floor and back wall, DrawnCells::split_z: lifted a pixel a
                    // quad, the wall's came apart into dots with the stick held down.)
                    if (g_span_px.size() == static_cast<std::size_t>(w) * h && g_span_px[k]) return false;
                    if (!by_part) return true;
                    return group == -1 ? piece[k] == -1 : piece[k] == group;
                };
                bool any = false;
                for (int y = ty; y < ty + bh && !any; ++y)
                    for (int x = tx; x < tx + bw && !any; ++x) any = in_group(static_cast<std::size_t>(y) * w + x);
                if (!any) continue;
                const bool under_ok = g_scene_rgb && g_cap.under_ok && g_cap.under.size() == static_cast<std::size_t>(w) * h * 3;
                auto same_under = [&](std::size_t k) {
                    return under_ok && std::equal(g_scene_rgb + k * 3, g_scene_rgb + k * 3 + 3, g_cap.under.data() + k * 3);
                };
                int xz_min = 1 << 30;
                for (int y = ty; y < ty + bh; ++y)
                    for (int x = tx; x < tx + bw; ++x) {
                        const std::size_t k = static_cast<std::size_t>(y) * w + x;
                        if (in_group(k)) xz_min = std::min(xz_min, g_xz[k]);
                    }
                if (static_cast<int>(g_blocks.size()) >= kMaxLiftBlocks) break;
                int ax0, ay0;
                const int b = atlas_new_square(&ax0, &ay0, 0);
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        const std::size_t k = static_cast<std::size_t>(ty + y) * w + tx + x;
                        if (!in_group(k)) continue;
                        g_lift[k] = static_cast<std::uint8_t>(b + 1);
                        // (An art flame under the effect's: lifted off and not drawn, the wall behind
                        // it showing. KOL_FX_ART_KEEP=1: as before.)
                        const bool fx_art = by_part && fx_px(k);
                        g_atlas_opaque[static_cast<std::size_t>(ay0 + y) * g_atlas_w + ax0 + x] = fx_art ? 0 : 1;
                        g_under_xz[k] = g_back_xz[k];
                        g_under_y[k] = g_back_y[k];
                        g_under_kind[k] = g_back_kind[k];
                        // A pixel with the same colour on the layer under it: that layer
                        // holds a copy of this wall there, not what is behind it (map 36's
                        // parapet tops, BG1 over a near copy on BG0). Laid at the ground
                        // behind's depth, the copy slid out under the wall's top with the
                        // stick held down, a speckled wedge (Thor state 0270). It keeps
                        // the wall's own place; a pixel with another colour under it shows
                        // what is behind, as ever. (First by piece, half its pixels the
                        // same: whole pieces were left unlifted, making it nearly useless.)
                        // KOL_LIFT_SAME_UNDER=1: every pixel at the ground behind's.
                        static const bool lift_same = kol::env_set("KOL_LIFT_SAME_UNDER");
                        // (Or most of the wall's pixels round it are so: the copy differs
                        // from the wall by a few pixels, round a slit, and those few slid
                        // out alone. Of the piece's pixels within 4, two in five the same.)
                        auto copy_round = [&](int px0, int py0) {
                            int same = 0, all = 0;
                            for (int yy = std::max(ty, py0 - 4); yy <= std::min(ty + bh - 1, py0 + 4); ++yy)
                                for (int xx = std::max(tx, px0 - 4); xx <= std::min(tx + bw - 1, px0 + 4); ++xx) {
                                    const std::size_t q = static_cast<std::size_t>(yy) * w + xx;
                                    if (!in_group(q)) continue;
                                    ++all;
                                    same += same_under(q);
                                }
                            return all > 0 && same * 5 >= all * 2;
                        };
                        if (!lift_same && ((!g_inlay.empty() && g_inlay[k] == 1) || same_under(k) || copy_round(tx + x, ty + y))) {
                            g_under_xz[k] = g_xz[k];
                            g_under_y[k] = g_y[k];
                            g_under_kind[k] = g_kind[k];
                            // (And the wall's own colours there: the other layer's showed
                            // at the lifted pixels' corners, single dark pixels on the stone.)
                            if (g_scene_rgb && !same_under(k)) {
                                const std::uint8_t* c = g_scene_rgb + k * 3;
                                g_fx_under_fix.push_back({static_cast<std::uint32_t>(k), {c[0], c[1], c[2]}});
                            }
                        }
                        if (swap_ao) {
                            g_lift_ao[k] = g_ao[k];
                            g_ao[k] = g_back_ao[k];
                        }
                        ++surface_px;
                    }
                kol::ScenePixels::SpriteBlock blk{tx, ty, bw, bh, b, xz_min, 0, 1.0e9f, 1.0f};
                blk.surface = true;
                g_blocks.push_back(blk);
                }
            }
        g_lift_ao_on = swap_ao && surface_px > 0;
    }
    return surface_px;
}
}  // namespace

namespace kol_focus_detail {
// The BG1 props' pixels (claimed by a prop, the scene's top BG1) lifted into
// atlas squares placed as upright cards at the prop's place; the scene keeps
// the terrain's place there (g_under_*), and kol_perspective puts the colour
// behind BG1 there (the under buffer, g_ppu_scene_under_bg1). After the
// units' atlas; its squares follow theirs.
void lift_prop_pixels(const std::uint8_t* layers, int w, int h, bool props_on, bool walls_on,
                      int ox, int oy, int kx, int ky) {
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (g_prop_dist.size() != n || g_under_xz.size() != n) return;
    if (g_pre_xz.size() != n) props_on = false;
    if (g_lift.size() != n) {
        g_lift.assign(n, 0);
        g_atlas_w = w;
        g_atlas_per_row = w / S;
        g_atlas_h = 0;
        g_atlas_rgb.clear(); g_atlas_opaque.clear(); g_atlas_blur.clear();
    }
    if (g_atlas_per_row <= 0) return;
    const PropLift pl = lift_props(layers, w, h, props_on, ox, oy, kx, ky);
    const int surface_px = lift_wall_surfaces(w, h, walls_on);
    if (!g_blocks.empty()) g_atlas_on = true;
    static const bool sprite_debug = kol::env_set("KOL_SPRITE_DEBUG");
    if (sprite_debug) std::fprintf(stderr, "[surfaces] %d px of BG1 terrain lifted\n", surface_px);
    if (sprite_debug)
        std::fprintf(stderr, "[props] %zu props, %d px lifted, blocks now %zu\n", pl.props, pl.lifted, g_blocks.size());
    if (sprite_debug)
        for (const auto& b : g_blocks)
            if (b.prop) std::fprintf(stderr, "   prop slot %d at (%d,%d) %dx%d xz=%d y=%d\n", b.slot, b.sx, b.sy, b.w, b.h, b.xz, b.y);
}
}  // namespace kol_focus_detail

namespace {
// Flames the scene paints over the map: map 98's town under the archers'
// fire arrows, and 99, the same town burning (states/HK = the user's slot 2,
// 2026-10-16, asked for the fire not to look stuck to the walls).
// The terrain pass laid the flames' pixels on the roof, wall or street behind
// them, and the perspective and the stick bent them with it. Each flame is
// now a prop of its own: an upright card on the cell the game put it on,
// lifted like the bushes, lit by itself (no shade, no torchlight, no shadow
// cast). KOL_NO_FLAME_CARDS=1 leaves them on the terrain.
//
// The game writes a flame into its composed map as a block of BG1 tiles in
// palette 15, 4x4 when grown (smaller while it grows, upward from the same
// bottom row), the block's bottom 8 rows under a cell's top middle, in its
// middle column (all of HK's, exactly). On these maps BG1 holds nothing
// else; palette 15 is not flames everywhere (maps 2, 3, 8, 9, 29, 32, 40, 50
// and 89 have BG1 art in it), hence the list. The tile numbers name no
// place in the block (the grown flames' sheet is 22 tiles wide, the growing
// ones' is not), so the blocks are found by where they can stand: the
// lowest flame tile left over belongs to the cell-aligned 4x4 block over it
// that holds the most flame tiles left over (map 99's 54 blocks overlap).
// (The maps: map_settings.cfg `flames`.)
// Campfires the scene makes as objects, by their actor (a unit record's
// +0x4E): drawn by the fire effect with Options > Special Effects (the
// user, map 63, asked for the fire pit's fire as a special effect, as on the
// burning town map). KOL_NO_FIRE_ACTORS=1: the game's sprite.
struct FireActor { int map, actor; };
constexpr FireActor kFireActors[] = {{63, 116}, {64, 116}};   // (64: the snowy camp, its fire the same actor)
bool fire_actor(int actor) {
    static const bool off = std::getenv("KOL_NO_FIRE_ACTORS") != nullptr;
    if (off) return false;
    const int map = current_map();
    for (const FireActor& f : kFireActors)
        if (f.map == map && f.actor == actor) return true;
    return false;
}
}  // namespace

namespace kol_focus_detail {
// The smoke scene after the burning town (map 100): its BG1 is smoke, in
// the same palette, as columns two tiles wide and up to twelve tall, each
// rising from a cell (see flame_cards).
// (The maps: map_settings.cfg `smoke`.)
void flame_cards(const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky) {
    g_flame.clear();
    g_flame_at.clear();
    static const bool off = std::getenv("KOL_NO_FLAME_CARDS") != nullptr;
    static const bool trace = std::getenv("KOL_FLAME_TRACE") != nullptr;
    const Capture& cap = g_cap;
    if (off || !cap.extent_ok || !cap.bg0_ok) return;
    const int map = current_map();
    const bool smoke = kol::map_flag(kol::MapFlag::Smoke, map);
    static const bool no_smoke = std::getenv("KOL_NO_SMOKE_CARDS") != nullptr;
    if (smoke && no_smoke) return;
    if (!smoke && !kol::map_flag(kol::MapFlag::Flames, map)) return;
    const std::uint8_t* ew = guest_ew();
    if (!ew) return;
    const kol::MapExtent& x = cap.extent;
    // The picture's scroll, as edge_fade_build's (scroll_lead). A cut (the
    // camera jumping, map 98 to 99) leads the picture by the jump: the blocks
    // would be placed by the next map's (one frame, at the wrong depths). A
    // pan leads it by a few pixels.
    const int lead_x = scroll_lead(static_cast<int>(cap.bg0_h), x.hofs);
    const int lead_y = scroll_lead(static_cast<int>(cap.bg0_v), x.vofs);
    if (std::abs(lead_x) > 16 || std::abs(lead_y) > 16) return;
    const int ax0 = x.origin_x + lead_x - ox;   // array x = px + ax0
    const int ay0 = x.origin_y + lead_y - oy;
    // The cells' middles on the array's tile grid: a block's left column is
    // (16 d + kx + ox - 16 + ax0) / 8 for d = cx - cz, its bottom row's top
    // (8 s + 8 - 8 height + ky + oy + 8 + ay0) / 8 for s = cx + cz: both
    // whole, or the camera is between tiles and no block stands.
    const int col0 = kx + ox - 16 + ax0, row0 = ky + oy + 16 + ay0;
    if ((col0 & 7) != 0 || (row0 & 7) != 0) return;
    constexpr int kC = kol::MapExtent::kCols;
    const int rows = x.rows;
    auto flame_tile = [&](int r, int c) {
        if (c < 0 || c >= kC || r < 0 || r >= rows) return false;
        const std::uint16_t e = x.bg1[r * kC + c];
        return (e & 0x3FFu) != 0u && (e >> 12) == 15u;
    };
    static thread_local std::vector<int> block_of;   // per array tile: its flame, -1 none
    block_of.assign(static_cast<std::size_t>(rows) * kC, -1);
    struct Flame { int r1, c0, xz, y; bool placed; int r0 = -1; };
    static thread_local std::vector<Flame> flames;
    flames.clear();
    // A block's cell: down its middle column (cx - cz = d), a cell whose top
    // middle is at scene row `foot` at the game's own height, the nearest
    // first: the first such (first), or, of several, the one the terrain
    // drew there (owner).
    auto place_on_cell = [&](Flame& f, int d, int foot, int owner, bool first) {
        for (int sxz = 2 * kGameSide - 2; sxz >= 0 && !(first && f.placed); --sxz) {   // the nearest first
            if (((sxz + d) & 1) != 0) continue;
            const int cx = (sxz + d) / 2, cz = (sxz - d) / 2;
            if (!in_game(cx, cz)) continue;
            const int hgt = ew[kGrid - 0x02000000u + game_cell(cx, cz) + 1];
            if (8 * sxz + 8 - 8 * hgt + ky + oy != foot) continue;
            if (!f.placed || owner == gi(cx, cz)) {
                f.xz = 32 * sxz + 32;
                f.y = 16 * hgt;
                f.placed = true;
            }
        }
    };
    // ... else the ground the terrain drew at scene pixel k.
    auto place_on_ground = [&](Flame& f, std::size_t k) {
        if (g_covered[k] && g_kind[k] != kol::kSceneNone) {
            f.xz = g_xz[k];
            f.y = g_y[k];
            f.placed = true;
        }
    };
    const int lpar = ((col0 >> 3) % 2 + 2) % 2;   // a block's left column, mod 2
    for (int r = rows - 1; r >= 0; --r)   // the lowest first
        for (int c = 0; c < kC; ++c) {
            if (!flame_tile(r, c) || block_of[r * kC + c] >= 0) continue;
            if (smoke) {
                // A column: this tile and its partner beside it, up while
                // either holds smoke; it stands on its bottom.
                const int c0 = flame_tile(r, c + 1) && block_of[r * kC + c + 1] < 0 ? c : c - 1;
                int top = r;
                while (top - 1 >= 0 && (flame_tile(top - 1, c0) || flame_tile(top - 1, c0 + 1))) --top;
                const int fi = static_cast<int>(flames.size());
                for (int rr = top; rr <= r; ++rr)
                    for (int cc = c0; cc <= c0 + 1; ++cc)
                        if (flame_tile(rr, cc) && block_of[rr * kC + cc] < 0) block_of[rr * kC + cc] = fi;
                Flame f{r, c0 - 1, 0, 0, false, top};   // c0 - 1: its middle is the block's (c0 + 2) * 8 less 8
                // Where it rises: a cell's top middle at its bottom edge (on
                // the middle of a cell's column), else the ground drawn at
                // its foot (8 px off the middles, on the cells' corners, as
                // some of map 99's flames stand).
                const int mid_x = (c0 + 1) * 8 - ax0, bottom = (r + 1) * 8 - ay0;
                if (((mid_x - kx - ox) % 16 + 16) % 16 == 0) place_on_cell(f, (mid_x - kx - ox) / 16, bottom, -1, true);
                const int foot = bottom - 1;
                if (!f.placed && mid_x >= 0 && mid_x < w && foot >= 0 && foot < h)
                    place_on_ground(f, static_cast<std::size_t>(foot) * w + mid_x);
                flames.push_back(f);
                continue;
            }
            int best_c0 = -1, best_n = -1;
            for (int c0 = c - 3; c0 <= c; ++c0) {
                if (((c0 % 2) + 2) % 2 != lpar) continue;
                int nl = 0;
                for (int rr = r - 3; rr <= r; ++rr)
                    for (int cc = c0; cc < c0 + 4; ++cc)
                        nl += flame_tile(rr, cc) && block_of[rr * kC + cc] < 0;
                if (nl > best_n) { best_n = nl; best_c0 = c0; }
            }
            const int fi = static_cast<int>(flames.size());
            Flame f{r, best_c0, 0, 0, false, r - 3};
            for (int rr = r - 3; rr <= r; ++rr)
                for (int cc = best_c0; cc < best_c0 + 4; ++cc)
                    if (flame_tile(rr, cc) && block_of[rr * kC + cc] < 0) block_of[rr * kC + cc] = fi;
            // Its cell: down the block's middle column (cx - cz = d), the one
            // whose top middle is 8 rows over the block's bottom (its anchor)
            // at the game's own height; of several, the one the terrain drew
            // there.
            const int mid_x = (best_c0 + 2) * 8 - ax0, foot = (r + 1) * 8 - 8 - ay0;   // scene pixels
            int owner = -1;
            if (mid_x >= 0 && mid_x < w && foot >= 0 && foot < h && g_cell_owner.size() == static_cast<std::size_t>(w) * h)
                owner = g_cell_owner[static_cast<std::size_t>(foot) * w + mid_x];
            place_on_cell(f, (mid_x - kx - ox) / 16, foot, owner, false);
            // Map 99's flames stand off the cells' middles too (on their
            // corners, 8 rows off): there, the terrain drawn at the anchor.
            if (!f.placed && owner >= 0) place_on_ground(f, static_cast<std::size_t>(foot) * w + mid_x);
            flames.push_back(f);
        }
    const std::size_t n = static_cast<std::size_t>(w) * h;
    g_flame_cx.resize(flames.size());
    g_flame_top.resize(flames.size());
    for (std::size_t i = 0; i < flames.size(); ++i) {
        g_flame_cx[i] = static_cast<float>((flames[i].c0 + 2) * 8 - ax0);
        g_flame_top[i] = static_cast<float>(flames[i].r0 * 8 - ay0);
        if (!smoke && flames[i].placed) {   // its foot: X - Z from its middle column
            const float dxz = 2.0f * (g_flame_cx[i] - static_cast<float>(kx + ox));
            g_flame_at.push_back({0.5f * (flames[i].xz + dxz), static_cast<float>(flames[i].y), 0.5f * (flames[i].xz - dxz)});
        }
    }
    // The whole scene, margins too: they are drawn from the composed map's
    // entries, the very ones the blocks come from. (Only the native screen
    // until 2026-10-17: a flame crossing its edge went from card to terrain,
    // and under the perspective the edge showed as a faint trapezoid that
    // moved with the camera, states/HQ. KOL_FLAME_NATIVE_ONLY=1: as before.)
    static const bool native_only = std::getenv("KOL_FLAME_NATIVE_ONLY") != nullptr;
    const int nx0 = native_only ? std::max(0, ox) : 0, nx1 = native_only ? std::min(w, ox + 240) : w;
    const int ny0 = native_only ? std::max(0, oy) : 0, ny1 = native_only ? std::min(h, oy + 160) : h;
    auto fdiv8 = [](int v) { return v >= 0 ? v / 8 : -((7 - v) / 8); };
    std::vector<bool> seen(trace ? flames.size() : 0, false);   // (KOL_FLAME_TRACE's)
    for (int py = ny0; py < ny1; ++py)
        for (int px = nx0; px < nx1; ++px) {
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            if (scene_top(layers, k) != 1u) continue;
            const int r = fdiv8(py + ay0), c = fdiv8(px + ax0);
            if (c < 0 || c >= kC || r < 0 || r >= rows) continue;
            const int fi = block_of[r * kC + c];
            if (fi < 0) continue;
            const Flame& f = flames[fi];
            if (trace && !seen[fi]) {
                seen[fi] = true;
                std::fprintf(stderr, "flame block r%d c%d (%d px tall at %d,%d): %s xz %d y %d\n", f.r1, f.c0,
                             32, (f.c0 + 2) * 8 - ax0, (f.r1 + 1) * 8 - ay0, f.placed ? "cell" : "NO CELL", f.xz, f.y);
            }
            if (!f.placed) continue;
            if (g_flame.empty()) g_flame.assign(n, 0);
            g_flame[k] = smoke ? 2 : 1;
            // A prop of its own (props group by place and this): flames on
            // one diagonal at one height share a place, and six of map 99's
            // were one prop 192 px wide, of which Special Effects drew the
            // middle 32. Negative: no cell.
            set_prop_px(k, f.xz, f.y, -2 - fi);
        }
}
}  // namespace kol_focus_detail
