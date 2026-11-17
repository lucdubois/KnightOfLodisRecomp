// kol_focus_present.cpp — the output stage: the depth-of-field provider
// (focus_provider) and the scene's pixels for the perspective's warp
// (kol::scene_pixels), with the masks they hand over (water, ice, the
// range, the burnt houses' holes) and the lights of the moment (fire,
// spells, the fades).

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
// The field's cells (gi) a terrain rule takes, into out: each game cell's
// terrain record index (the terrain map through the ROM) for rec_ok(index),
// then the editor's WATER tab tags over it (kol_tile_edit): `set` takes a
// cell, `clear_a` and `clear_b` drop it. False when none is.
template <class RecOk>
bool cells_from_terrain(int map, std::uint8_t* out, RecOk rec_ok, signed char set, signed char clear_a,
                        signed char clear_b) {
    std::fill_n(out, kGridSide * kGridSide, 0);
    bool any = false;
    const Capture& cap = g_cap;
    const std::uint8_t* ew = cap.ew.data();
    const std::uint32_t terrain_map = rd32(ew, kTerrainMapPtr - 0x02000000u);
    if (cap.rom && terrain_map >= 0x08000000u)
        for (int cy = 0; cy < kGameSide; ++cy)
            for (int cx = 0; cx < kGameSide; ++cx) {
                const std::uint32_t at =
                    terrain_map - 0x08000000u + ew[kGrid - 0x02000000u + game_cell(cx, cy)] * 2u;
                if (at + 2u > cap.rom_size) continue;
                if (rec_ok(cap.rom[at] | (cap.rom[at + 1] << 8))) { out[gi(cx, cy)] = 1; any = true; }
            }
    static thread_local std::vector<signed char> tags(static_cast<std::size_t>(kGridSide) * kGridSide);
    if (kol::tile_edit_water(map, tags.data()))
        for (int c = 0; c < kGridSide * kGridSide; ++c) {
            if (tags[c] == set) { out[c] = 1; any = true; }
            else if (tags[c] == clear_a || tags[c] == clear_b) out[c] = 0;
        }
    if (!any) return false;
    for (int c = 0; c < kGridSide * kGridSide; ++c)
        if (out[c]) return true;
    return false;
}
}  // namespace

namespace kol_focus_detail {
// Which of the field's cells (gi) count as water, into out: the game's
// terrain class 2 (the terrain table's first byte; records 10, 11, 12),
// then the editor's water tags over it (kol_tile_edit, the WATER tab: a
// story scene's map has no terrain records). False when none is.
bool water_cells(int map, std::uint8_t* out) {
    const Capture& cap = g_cap;
    return cells_from_terrain(
        map, out,
        [&](int index) {
            const std::uint32_t rec = kTerrainTable - 0x08000000u + index * 0x20u;
            return rec < cap.rom_size && cap.rom[rec] == 2u;
        },
        kol::kWaterYes, kol::kWaterDry, kol::kWaterIce);
}
}  // namespace kol_focus_detail

namespace {
// Which cells are ice, into out: the ice's terrain records (kIceRecords; the
// snowfields' striped blue, map 17 and 12: record 22), and the cells the
// editor's WATER tab tags ICE (a water or dry tag is not ice). False when
// none is.
constexpr int kIceRecords[] = {22};
bool ice_cells(std::uint8_t* out) {
    const int map = rd16(g_cap.ew.data(), kMapNumber - 0x02000000u);
    bool any = cells_from_terrain(
        map, out, [](int rec) { return in_list(kIceRecords, rec); }, kol::kWaterIce, kol::kWaterYes, kol::kWaterDry);
    // A polished floor's cells (kPolishedCells): its mirror is the ice's.
    if (kol::map_flag(kol::MapFlag::PolishedFloor, map))
        for (const PolishedCells& p : kPolishedCells) {
            if (p.map != map) continue;
            for (int z = p.z0; z <= p.z1; ++z)
                for (int x = p.x0; x <= p.x1; ++x)
                    if (in_grid(x, z)) { out[gi(x, z)] = 1; any = true; }
        }
    return any;
}


int focus_provider(const std::uint8_t* layers, int w, int h, std::uint8_t* blur) {
    kol::PresentScope scope;
    layers = kol::present_layers(layers, w, h);
    if (!build_scene(layers, w, h)) return 0;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    const Unit* units = g_info.units;
    const int moved = g_info.moved;
    const int moved_count = g_info.moved_count;
    const bool cursor = g_info.cursor;
    const int cursor_xz = g_info.cursor_xz;
    const int cursor_y = g_info.cursor_y;

    // The target: the cursor; else the one unit that moved (several at once
    // is a reload or a scene change, not a walk); else the scenery at the
    // centre of the screen.
    const int vx = g_info.vx, vy = g_info.vy, vw = g_info.vw, vh = g_info.vh;
    const std::size_t centre =
        static_cast<std::size_t>(vy + vh / 2) * w + vx + vw / 2;
    const bool walker = !cursor && moved >= 0 && moved_count <= 2;
    // Else a unit at the centre of the screen: the one whose body (12 rows
    // over its feet) is nearest it, within kCentreUnitR px. The centre pixel
    // alone fell beside Alphonse onto the sky over a cliff's edge (map 50,
    // states/thor/0162, a dialogue: no cursor, nobody walking), read as ground
    // far behind, and the focus went to the background (the user, 2026-11-10,
    // wanted it on the character in the middle of the screen).
    // KOL_FOCUS_CENTRE_PIXEL=1: the centre's scenery alone, as before.
    static const bool centre_pixel = kol::env_set("KOL_FOCUS_CENTRE_PIXEL");
    constexpr int kCentreUnitR = 48;
    int centre_unit = -1;
    if (!cursor && !walker && !centre_pixel) {
        const int cx = vx + vw / 2, cy = vy + vh / 2;
        int best = kCentreUnitR * kCentreUnitR + 1;
        for (int u = 0; u < g_info.count; ++u) {
            if (!units[u].shown) continue;
            const int sx = feet_sx(units[u]) + g_info.kx + g_info.ox;
            const int sy = feet_sy(units[u]) + g_info.ky + g_info.oy - 12;
            const int d = (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy);
            if (d < best) { best = d; centre_unit = u; }
        }
    }
    const float target_xz = static_cast<float>(
        cursor ? cursor_xz : walker ? units[moved].x + units[moved].z
                           : centre_unit >= 0 ? units[centre_unit].x + units[centre_unit].z : g_xz[centre]);
    const float target_y = static_cast<float>(
        cursor ? cursor_y : walker ? units[moved].y : centre_unit >= 0 ? units[centre_unit].y : g_y[centre]);
    if (!g_focus.have) {
        g_focus.xz = target_xz;
        g_focus.y = target_y;
        g_focus.have = true;
    }
    g_focus.xz += (target_xz - g_focus.xz) * 0.25f;
    g_focus.y += (target_y - g_focus.y) * 0.25f;

    // Distance from the focus: rows apart plus height apart, a height step
    // counting KOL_FOCUS_HEIGHT rows (default 1: the player counts both as
    // steps). Sharp within `near` rows. Beyond that the blur ramps evenly to
    // full at the most distant map on screen on that side -- behind the focus
    // (the top edge) or in front of it (the bottom edge) -- so only the very
    // edges get the heaviest blur, whatever the camera takes in. Only terrain
    // sets that range: the empty sky reads as the ground plane, far beyond the
    // map, and letting it count left the whole map barely blurred.
    static const float near_rows = kol::env_num_or("KOL_FOCUS_NEAR", 1.5f);
    static const float height_weight = 2.0f * kol::env_num_or("KOL_FOCUS_HEIGHT", 1.0f);
    const float near_units = near_rows * kRow;
    auto distance_at = [&](float xz, float y) {
        return std::abs(xz - g_focus.xz) + height_weight * std::abs(y - g_focus.y);
    };
    // A pixel lifted out for a unit drawn from its own sprite shows the scene
    // behind it, and blurs as that does.
    const bool lifted = g_atlas_on && g_lift.size() == pixels;
    auto distance = [&](std::size_t k) {
        if (lifted && g_lift[k])
            return distance_at(static_cast<float>(g_under_xz[k]), static_cast<float>(g_under_y[k]));
        return distance_at(static_cast<float>(g_xz[k]), static_cast<float>(g_y[k]));
    };
    // Over the view only: what lies past it (a scene's overscan) is not on
    // screen, and must not stretch the ramp.
    float far_behind = 0.0f, far_front = 0.0f;
    const std::uint8_t* const sharp = g_sharp_any ? g_sharp.data() : nullptr;   // (none: g_sharp not kept)
    for (int py = vy; py < vy + vh; ++py) {
        const std::size_t o = static_cast<std::size_t>(py) * w;
        for (std::size_t k = o + vx; k < o + vx + vw; ++k) {
            if (!g_covered[k] || g_iface[k] || (sharp && sharp[k])) continue;
            const float d = distance(k);
            if (g_xz[k] < g_focus.xz) far_behind = std::max(far_behind, d);
            else far_front = std::max(far_front, d);
        }
    }
    // At least one row of ramp, so a shallow view never snaps to full blur.
    const float behind = 255.0f / std::max(static_cast<float>(kRow), far_behind - near_units);
    const float front = 255.0f / std::max(static_cast<float>(kRow), far_front - near_units);
    auto blur_of = [&](float d, bool behind_focus) -> std::uint8_t {
        const float off = d - near_units;
        if (off <= 0.0f) return 0;
        return static_cast<std::uint8_t>(std::min(255.0f, off * (behind_focus ? behind : front)));
    };
    // Everything the loop reads, in locals: `blur` is bytes, and a byte store
    // may alias anything, so the globals were reloaded for every pixel.
    const float focus_xz = g_focus.xz, focus_y = g_focus.y, weight = height_weight;
    const std::uint8_t* const iface = g_iface.data();
    const std::uint8_t* const floating = g_floating.data();
    const std::uint8_t* const lift = lifted ? g_lift.data() : nullptr;
    const std::int32_t* const xz_at = g_xz.data();
    const std::int32_t* const y_at = g_y.data();
    const std::int32_t* const under_xz = g_under_xz.data();
    const std::int32_t* const under_y = g_under_y.data();
    for (std::size_t k = 0; k < pixels; ++k) {
        // A floating hand is not drawn here: the ground behind it is, filled
        // from beside it, and blurs as that ground does.
        if ((iface[k] && !floating[k]) || (sharp && sharp[k])) { blur[k] = 0; continue; }
        const bool under = lift && lift[k];
        const float xz = static_cast<float>(under ? under_xz[k] : xz_at[k]);
        const float y = static_cast<float>(under ? under_y[k] : y_at[k]);
        const float d = std::abs(xz - focus_xz) + weight * std::abs(y - focus_y);
        const float off = d - near_units;
        blur[k] = off <= 0.0f ? 0
            : static_cast<std::uint8_t>(std::min(255.0f, off * (xz < focus_xz ? behind : front)));
    }
    // Each unit drawn from its own sprite blurs at its place, in its square
    // of the atlas (read by gbarecomp after this, PresentWarpMesh::atlas_blur).
    // Lifted BG1 terrain has no one place: each texel blurs at its own
    // pixel's (the block's xz is its nearest pixel's and its y 0, which
    // blurred the castle's pillar edges as the ground far off).
    if (lifted) {
        for (std::size_t bi = 0; bi < g_blocks.size(); ++bi) {
            const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
            if (b.surface) {
                const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
                for (int y = 0; y < b.h; ++y)
                    for (int x = 0; x < b.w; ++x) {
                        const std::size_t k = static_cast<std::size_t>(b.sy + y) * w + b.sx + x;
                        if (g_lift[k] != bi + 1) continue;
                        g_atlas_blur[static_cast<std::size_t>(ay0 + y) * g_atlas_w + ax0 + x] =
                            blur_of(distance_at(static_cast<float>(g_xz[k]), static_cast<float>(g_y[k])),
                                    static_cast<float>(g_xz[k]) < g_focus.xz);
                    }
                continue;
            }
            const std::uint8_t v = blur_of(distance_at(static_cast<float>(b.xz), static_cast<float>(b.y)),
                                           static_cast<float>(b.xz) < g_focus.xz);
            const auto [ax0, ay0] = kol::slot_origin(b.slot, g_atlas_per_row);
            for (int y = 0; y < b.h; ++y)
                std::fill_n(g_atlas_blur.begin() + static_cast<std::ptrdiff_t>(ay0 + y) * g_atlas_w + ax0,
                            b.w, v);
            // A flame the effect draws (effect 1) is see-through at its edges, and
            // gbarecomp draws the blur map through the mesh with the same blend as the
            // colours: a see-through texel keeps part of the blur behind it. Over a sky
            // at full blur a flame in focus came out soft (map 36's brazier on its
            // plinth, Thor state 0274: blurred as if far away). The scene
            // behind its column takes no more blur than the flame, and the sky (which
            // stays where it is while the flame moves with the camera, and looks the
            // same sharp) for 32 round it. KOL_FLAME_BLUR_BEHIND=1: as before.
            static const bool flame_behind = kol::env_set("KOL_FLAME_BLUR_BEHIND");
            if (b.effect == 1 && !flame_behind) {
                const int cx = static_cast<int>(b.fx_cx), top = static_cast<int>(b.fx_top),
                          foot = static_cast<int>(b.foot_row);
                for (int py = std::max(0, top - 32); py <= std::min(h - 1, foot + 16); ++py)
                    for (int px = std::max(0, cx - 32); px <= std::min(w - 1, cx + 32); ++px) {
                        const std::size_t k = static_cast<std::size_t>(py) * w + px;
                        const bool near_col = std::abs(px - cx) <= 10 && py >= top - 2 && py <= foot + 2;
                        if (near_col || g_kind[k] == kol::kSceneNone) blur[k] = std::min(blur[k], v);
                    }
            }
        }
    }
    // KOL_FOCUS_DUMP=<prefix> writes this frame's layer map and blur, raw
    // w*h bytes each, once, after KOL_FOCUS_DUMP_AT presents (default 200).
    if (static const char* dump = std::getenv("KOL_FOCUS_DUMP"); dump) {
        static long presents = 0;
        static const long at = [] {
            const char* v = std::getenv("KOL_FOCUS_DUMP_AT");
            return v && *v ? std::atol(v) : 200L;
        }();
        if (++presents == at) {
            const std::string base(dump);
            if (FILE* f = std::fopen((base + ".layers").c_str(), "wb")) {
                std::fwrite(layers, 1, pixels, f);
                std::fclose(f);
            }
            if (FILE* f = std::fopen((base + ".blur").c_str(), "wb")) {
                std::fwrite(blur, 1, pixels, f);
                std::fclose(f);
            }
            // Each pixel's world place, for placing things in the world from
            // the picture (mockups): X and Z (int16, with the torchlight on),
            // height (int32) and face (0 top, 1 +X wall, 2 +Z wall).
            auto put = [&](const char* ext, const void* p, std::size_t bytes) {
                if (FILE* f = std::fopen((base + ext).c_str(), "wb")) {
                    std::fwrite(p, 1, bytes, f);
                    std::fclose(f);
                }
            };
            if (g_wx.size() == pixels) put(".wx", g_wx.data(), pixels * 2);
            if (g_wz.size() == pixels) put(".wz", g_wz.data(), pixels * 2);
            if (g_y.size() == pixels) put(".y", g_y.data(), pixels * 4);
            if (g_face.size() == pixels) put(".face", g_face.data(), pixels);
            if (g_modeled.size() == pixels) put(".model", g_modeled.data(), pixels);
            if (g_lift.size() == pixels) put(".lift", g_lift.data(), pixels);   // lifted: its patch + 1
            if (g_back_kind.size() == pixels) put(".back", g_back_kind.data(), pixels);   // the kind behind
            if (g_back_xz.size() == pixels) put(".bxz", g_back_xz.data(), pixels * 4);   // ... its depth (X + Z), int32
            if (g_back_y.size() == pixels) put(".by", g_back_y.data(), pixels * 4);       // ... and height, int32
            if (g_xz.size() == pixels) put(".xz", g_xz.data(), pixels * 4);               // each pixel's own depth, int32
            if (g_ao.size() == pixels) put(".ao", g_ao.data(), pixels);   // the light (255 lit)
            if (g_glow.size() == pixels) put(".glow", g_glow.data(), pixels);   // the lamps' light (torchlight)
            if (g_dump_rgb.size() == pixels * 3) put(".rgb", g_dump_rgb.data(), pixels * 3);   // the scene's colours
            if (g_cap.under_ok && g_cap.under.size() == pixels * 3) put(".under", g_cap.under.data(), pixels * 3);   // with no sprite
            // The projection, the units' places and the grid (cell x z byte
            // height flags), for fitting a model to the art (tools/models).
            if (FILE* f = std::fopen((base + ".info").c_str(), "w")) {
                std::fprintf(f, "w %d h %d ox %d oy %d kx %d ky %d vx %d vy %d vw %d vh %d hofs %d vofs %d\n", w, h,
                             g_info.ox, g_info.oy, g_info.kx, g_info.ky, g_info.vx, g_info.vy, g_info.vw, g_info.vh,
                             g_pic_hofs, g_pic_vofs);
                {
                    float sx_ = 0.0f, sz_ = 0.0f;
                    shadow_dir_pick(sx_, sz_);
                    std::fprintf(f, "sun %.3f %.3f %.3f\n", sx_, shadow_rise(), sz_);   // toward the sun: X, rise per unit, Z
                }
                for (int i = 0; i < g_info.count; ++i)
                    std::fprintf(f, "unit %d %d %d\n", g_info.units[i].x, g_info.units[i].y, g_info.units[i].z);
                if (g_cap.ew.size() > kGrid - 0x02000000u + 4096)
                    for (int c = 0; c < 1024; ++c) {
                        const std::uint8_t* p = g_cap.ew.data() + (kGrid - 0x02000000u) + c * 4;
                        std::fprintf(f, "cell %d %d %02x %d %02x\n", c % 32, c / 32, p[0], p[1], p[2]);
                    }
                std::fclose(f);
            }
            std::fprintf(stderr, "kol_focus: dumped %dx%d to %s.{layers,blur,wx,wz,y,face}\n",
                         w, h, dump);
        }
    }
    return 1;
}
}  // namespace

namespace kol {
float glow_strength() { return ::glow_strength(); }
bool lighting_on() {
    return ::ao_strength() > 0.0f || ::shadow_strength() > 0.0f || ::glow_strength() > 0.0f;
}
bool perspective_lift_props() { return lift_props_on() || lift_walls_on(); }

namespace {
// Options > Water: the water's surface in the scene just built (see
// ScenePixels::water), on the cells water_cells() counts. The terrain
// table's first byte is the terrain's class: 0 ground, 1 an obstacle, 2
// water (records 10, 11, 12: BN's pool and DQ's streams are 12, BK's
// channels 11); CK's ice is ground. A story scene's map (89) has no
// records: there the editor's water tags say.
// Interface over the map (a sprite on top, BG0/BG1 under it), not the
// cursor's floating hand: the warp shows the ground the game drew there
// and lights it as ground (kol_perspective ground_under_iface), so the ice
// and the water take it too. Left out, a dialog's portrait (sprites) left
// the game's raw ice beside it as the stick moved the map against it (JG).
bool ground_under_iface(std::size_t k) {
    return g_iface[k] && g_scene_codes && (g_scene_codes[k] & 0x0Fu) == kol::kLayerObj && (g_scene_codes[k] >> 4) <= 1u &&
           !(g_floating.size() > k && g_floating[k]);
}
// The map-layer rule the water and the ice share (water_mask, ice_mask):
// false where pixel k does not show the map's ground (no cell, the
// interface, a layer over BG1). A lifted pixel (lift: g_lift when the atlas
// is on and scene-sized, else null) counts by the ground under it when
// under_ok, else is skipped. frame_top: by the frame's top layer, not the
// scene's. *kind: the ground's scene kind.
inline bool map_ground_px(const std::uint8_t* layers, std::size_t k, int c, const std::uint8_t* lift, bool under_ok,
                          bool frame_top, bool* lifted_out, std::uint8_t* kind) {
    const bool lifted = lift && lift[k];
    if (lifted && !under_ok) return false;
    const bool ground_iface = !frame_top && ground_under_iface(k);
    const unsigned layer = lifted ? static_cast<unsigned>(g_scene_codes[k] >> 4)
                           : ground_iface ? 0u : frame_top ? layers[k] & 0x0Fu : scene_top(layers, k);
    if (c < 0 || (g_iface[k] && !lifted && !ground_iface) || layer > 1u) return false;
    *lifted_out = lifted;
    *kind = lifted ? g_under_kind[k] : g_kind[k];
    return true;
}
const std::uint8_t* water_mask(const std::uint8_t* layers, int w, int h) {
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (!kol::setting_on(kol::Setting::Water) || g_cell_owner.size() != pixels) return nullptr;
    static thread_local std::vector<std::uint8_t> cell_water(static_cast<std::size_t>(kGridSide) * kGridSide);
    if (!water_cells(g_cap.map, cell_water.data())) return nullptr;
    bool any = false;
    g_water.assign(pixels, 0);
    // By the scene's layer (scene_top: under a dialog box, the map's), and
    // under a lifted pixel by the ground it shows, as the ice (ice_mask).
    static const bool frame_top = std::getenv("KOL_WATER_FRAME_TOP") != nullptr;
    const bool under_ok = !frame_top && g_under_kind.size() == pixels && g_scene_codes;
    const std::uint8_t* lift = g_atlas_on && g_lift.size() == pixels ? g_lift.data() : nullptr;
    // (A model's flat pixels at a recess's water height, kModelRecessAo::water_y.)
    int recess_water_y = -1;
    for (const ModelRecessAo& r : kModelRecessAo)
        if (r.map == g_cap.map && r.water_y >= 0) recess_water_y = r.water_y;
    const bool model_px = recess_water_y >= 0 && g_modeled.size() == pixels;
    for (std::size_t k = 0; k < pixels; ++k) {
        const int c = g_cell_owner[k];
        bool lifted;
        std::uint8_t kind;
        if (model_px && g_modeled[k] && g_kind[k] == kol::kSceneFlat && g_y[k] == recess_water_y && !g_iface[k] &&
            !(lift && lift[k])) {
            g_water[k] = 1;
            any = true;
            continue;
        }
        if (!map_ground_px(layers, k, c, lift, under_ok, frame_top, &lifted, &kind)) continue;
        const bool wet = cell_water[c] != 0;
        // The surface; anything else the terrain pass placed is the shore
        // (a step's falling water too).
        g_water[k] = wet && kind == kol::kSceneFlat ? 1 : 2;
        any = any || g_water[k] == 1;
    }
    return any ? g_water.data() : nullptr;
}
// The weather's particles for the ice's reflections (ScenePixels::flakes):
// the game moves them in screen space only (kol_obj_cull: ten 0x14-byte
// entries at 0x0202A0F8, +0x04 live, +0x08 / +0x0A x / y in 8.8, drawn as an
// 8x8 at (x - 4, y - 4)), so how high one is has no answer in the game. A
// flake gets its ground the first time it falls within 56 rows over ice in
// its column: that ice and 0 to 8 rows more, kept for the rest of its fall.
// Its reflection is mirrored about that row, a spot where it lands on ice,
// and rises to meet the flake as it falls (the user asked for snow to
// reflect as spots, not streaks). (A ground set when it appeared,
// the first ice below or 8-32 rows down, almost never put the mirror on the
// ice.) A new flake is a slot coming alive or jumping up the screen.
// The renderer's copies in the margins too (see build_weather_px).
std::vector<kol::ScenePixels::Flake> g_flakes;
std::vector<std::uint8_t> g_ice;
std::vector<std::uint8_t> g_ice_lift;   // (ScenePixels::ice_lift, ice_mask)
bool g_ice_lift_any = false;
void weather_reflections(int w, int h, int ox, int oy) {
    static int ground0[10], last_y[10], extra[10];
    static bool live_before[10];
    g_flakes.clear();
    if (g_cap.ew.empty() || g_ice.size() != static_cast<std::size_t>(w) * h) return;
    const std::uint8_t* ew = g_cap.ew.data();
    static constexpr int kCopy[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    auto ice_at = [&](int px, int py) {
        return px >= 0 && py >= 0 && px < w && py < h && g_ice[static_cast<std::size_t>(py) * w + px];
    };
    for (int i = 0; i < 10; ++i) {
        const std::uint8_t* p = ew + (0x0202A0F8u - 0x02000000u) + i * 0x14u;
        if (p[4] == 0) { live_before[i] = false; continue; }
        const int x = ((p[8] | (p[9] << 8)) >> 8) - 4;
        const int y = ((p[10] | (p[11] << 8)) >> 8) - 4;
        if (!live_before[i] || y < last_y[i] - 8) {   // a new fall: no ground yet
            const std::uint32_t hsh = (static_cast<std::uint32_t>(i) * 2654435761u) ^ (static_cast<std::uint32_t>(x) * 40503u);
            ground0[i] = -1;
            extra[i] = static_cast<int>((hsh >> 7) % 9u);
        }
        live_before[i] = true;
        last_y[i] = y;
        // Its ground, the first time it falls within 56 rows over ice (in
        // the view or a margin copy): that ice, and 0 to 8 rows more.
        if (ground0[i] < 0)
            for (const auto& cp : kCopy) {
                const int col = x + cp[0] * 240 + ox + 4, fy = y + cp[1] * 160 - cp[0] * 80 + oy;
                for (int yy = fy + 16; yy < fy + 8 + 56 && ground0[i] < 0; ++yy)
                    if (ice_at(col, yy)) ground0[i] = y + (yy - fy) + extra[i];
                if (ground0[i] >= 0) break;
            }
        if (ground0[i] < 0 || ground0[i] <= y + 8) continue;   // none yet, or landed
        for (const auto& cp : kCopy)
            g_flakes.push_back({x + cp[0] * 240 + ox, y + cp[1] * 160 - cp[0] * 80 + oy,
                                ground0[i] + cp[1] * 160 - cp[0] * 80 + oy});
    }
}

// Options > Water, the ice too: its flat top in the scene just built
// (ScenePixels::ice, see kol_perspective ice_effect): 1 the ice, 0 else
// (g_ice, above).
const std::uint8_t* ice_mask(const std::uint8_t* layers, int w, int h, const std::uint8_t* rgb) {
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (!kol::setting_on(kol::Setting::Water) || g_cell_owner.size() != pixels) return nullptr;
    static thread_local std::vector<std::uint8_t> cell_ice(static_cast<std::size_t>(kGridSide) * kGridSide);
    if (!ice_cells(cell_ice.data())) return nullptr;
    bool any = false;
    g_ice.assign(pixels, 0);
    g_ice_lift_any = false;
    // A lifted pixel (a unit's, a prop's) shows the ground under it, lit
    // and iced as the ground: left out, the ice where the angel's blob gave
    // way to it (JF, map 40) was the game's own pale striped art, an oval
    // on the ice. KOL_ICE_NOT_UNDER_LIFTED=1: lifted pixels left out.
    // By the scene's layer (scene_top): by the frame's, the ice under a
    // dialog box (BG2/BG3 on top) was left out, and the warp, moving the
    // map against the box drawn flat, showed it as the game's raw ice by
    // the box's edge (JG, the user's slot 3, map 116: dialog boxes disturbed
    // the ice and water effects). KOL_ICE_FRAME_TOP=1: by the frame's.
    static const bool not_under_lifted = std::getenv("KOL_ICE_NOT_UNDER_LIFTED") != nullptr;
    static const bool ice_frame_top = std::getenv("KOL_ICE_FRAME_TOP") != nullptr;
    // The lifted pixel's own surface (its card's texel, drawn over the
    // ground above): the ice's top too, iced on the card. Left out, a
    // cell tagged ICE whose top the game drew on BG1 (map 85's 11,11,
    // lifted with the walls) kept the raw art over the iced ground (the
    // user, 2026-10-27, reported the ice effect missing on BG1).
    // KOL_ICE_NO_LIFTED_TOP=1: left out.
    static const bool no_lifted_top = std::getenv("KOL_ICE_NO_LIFTED_TOP") != nullptr;
    const bool under_ok = !not_under_lifted && g_under_kind.size() == pixels && g_scene_codes;
    const std::uint8_t* lift = g_atlas_on && g_lift.size() == pixels ? g_lift.data() : nullptr;
    // A polished floor's stone only, by the art's colour: on map 91 a cell
    // is partly carpet (the red carpet's edges and gold trim, the pool's
    // blues), and the user asked for the tile floors only, not the
    // carpets. The marble is grey-blue to pale blue (green and blue at
    // least red, saturation at most 91); the pool is saturated (141 up),
    // the carpet, its purple shading and its gold have red over green or
    // blue.
    const int map = rd16(g_cap.ew.data(), kMapNumber - 0x02000000u);
    const bool polished = rgb && kol::map_flag(kol::MapFlag::PolishedFloor, map);
    auto stone = [&](std::size_t k) {
        const std::uint8_t* q = rgb + k * 3;
        const int r = q[0], g = q[1], b = q[2];
        return g >= r && b >= r && std::max({r, g, b}) - std::min({r, g, b}) <= 100;
    };
    // The model's floor (kPolishedModelFloor): a model pixel has no cell,
    // so it passes as one when it is flat at the floor's height.
    int model_floor_y = INT_MIN;
    if (polished && g_modeled.size() == pixels && g_y.size() == pixels)
        for (const PolishedModelFloor& f : kPolishedModelFloor)
            if (f.map == map) model_floor_y = f.y;
    for (std::size_t k = 0; k < pixels; ++k) {
        const int c = g_cell_owner[k];
        const bool model_floor = c < 0 && model_floor_y != INT_MIN && g_modeled[k] && g_kind[k] == kol::kSceneFlat &&
                                 g_y[k] == model_floor_y;
        bool lifted;
        std::uint8_t kind;
        if (!map_ground_px(layers, k, model_floor ? 0 : c, lift, under_ok, ice_frame_top, &lifted, &kind)) continue;
        if (polished && !lifted && !stone(k)) continue;
        if ((model_floor || cell_ice[c]) && kind == kol::kSceneFlat && (g_face.size() != pixels || g_face[k] == 0)) {
            g_ice[k] = 1;
            any = true;
        }
        // The lifted pixel's own surface: the ice's top too (no_lifted_top).
        if (lifted && !no_lifted_top && c >= 0 && cell_ice[c] && g_kind[k] == kol::kSceneFlat &&
            (g_face.size() != pixels || g_face[k] == 0)) {
            if (!g_ice_lift_any) g_ice_lift.assign(pixels, 0);
            g_ice_lift[k] = 1;
            g_ice_lift_any = true;
        }
    }
    return any ? g_ice.data() : nullptr;
}
}  // namespace
// The movement / action range's tiles on screen (ScenePixels::range): the
// map's own pixels (BG0, BG1) on a composed-map cell wearing the highlight
// (kol_highlight), by the margins' mapping from the frame (hardware column
// = frame column - ox, row = frame row - oy). Asked 2026-10-14 for the
// movement/action tiles to be less affected by shadows. Null when none.
// Their black lines too: the outline round the range and the dotted lines
// between its tiles are another colour of the same tiles, not the
// highlight's, and with the range now left plain by the water, the ice and
// the wet look (kol_perspective range_plain), the ice's sheen still painted
// them light blue (states/thor/0100, map 41: the range's black lines and
// dots vanished on ice). A dark pixel (each
// channel at most 64: the dots are 24,33,49; the scene's colour `rgb`) of a
// tile that carries the highlight is the range's: a tile with at least 10 of
// the highlight's pixels (a range's tiles have 12, 44 or 60; a scene's own
// tiles that happen to use the highlight's colour index, 1 to 8: map 89's
// fortune teller, FC). KOL_RANGE_NO_LINES=1: the highlight's pixels alone,
// as before.
std::vector<std::uint8_t> g_range;
const std::uint8_t* range_mask(const std::uint8_t* layers, int w, int h, const std::uint8_t* rgb) {
    static const bool no_lines = std::getenv("KOL_RANGE_NO_LINES") != nullptr;
    const Capture& c = g_cap;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    // The grid cursor's corner pieces (g_cursor_face) are 2: no light or
    // shading at all (range_keep). KOL_CURSOR_LIT=1: lit as the ground, as before.
    static const bool cursor_lit = kol::env_set("KOL_CURSOR_LIT");
    const bool cursor_px = !cursor_lit && g_cursor_face.size() == pixels;
    // A drawn-height spot lit as drawn (DrawnCells::as_drawn): its pixels are 2 too; of a
    // split cell only its span (g_span_px: its walls and stored tops are lit as their
    // neighbours, or its stretch of the outer wall stands apart as a band).
    // KOL_AS_DRAWN_LIT=1: lit as the ground.
    static const bool as_drawn_lit = kol::env_set("KOL_AS_DRAWN_LIT");
    static thread_local std::vector<std::uint8_t> as_drawn;
    bool as_drawn_any = false;
    if (!as_drawn_lit && g_cell_owner.size() == pixels && g_span_px.size() == pixels) {
        as_drawn.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
        for (const DrawnCells& d : kDrawnCells)
            if (d.as_drawn && d.map == current_map())
                for (int cz = d.z0; cz <= d.z1; ++cz)
                    for (int cx = d.x0; cx <= d.x1; ++cx)
                        if (in_grid(cx, cz)) { as_drawn[static_cast<std::size_t>(gi(cx, cz))] = d.split_z > 0 ? 2 : 1; as_drawn_any = true; }
    }
    if (!c.range_ok[0] && !c.range_ok[1] && !cursor_px && !as_drawn_any) return nullptr;
    g_range.assign(pixels, 0);
    bool any = false;
    if (cursor_px)
        for (std::size_t k = 0; k < pixels; ++k)
            if (g_cursor_face[k]) { g_range[k] = 2; any = true; }
    if (as_drawn_any)
        for (std::size_t k = 0; k < pixels; ++k) {
            const int o = g_cell_owner[k];
            if (o < 0 || static_cast<std::size_t>(o) >= as_drawn.size() || !as_drawn[static_cast<std::size_t>(o)]) continue;
            if (as_drawn[static_cast<std::size_t>(o)] == 2 && !g_span_px[k]) continue;
            g_range[k] = 2;
            any = true;
        }
    if (!c.range_ok[0] && !c.range_ok[1]) return any ? g_range.data() : nullptr;
    const int ox = g_info.ox, oy = g_info.oy;
    auto div8 = [](int v) { return v >= 0 ? v / 8 : -((7 - v) / 8); };
    for (int py = 0; py < h; ++py)
        for (int px = 0; px < w; ++px) {
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            const unsigned top = layers[k] & 0x0Fu;
            if (g_range[k] || top > 1u || !c.range_ok[top] || (g_iface.size() == pixels && g_iface[k])) continue;
            const int* m = c.range_map[top];
            const int hx = px - ox + m[2], hy = py - oy + m[3];
            const int col = m[0] + div8(hx), row = m[1] + div8(hy);
            if (row < 0 || row >= c.range_rows[top] || col < 0 || col >= 128) continue;
            // The pixel's own bit: the highlight's pixels, not the floor
            // beside them in an edge tile.
            const std::uint64_t ink = c.range_lit[top][static_cast<std::size_t>(row) * 128u + col];
            if (ink >> ((hy & 7) * 8 + (hx & 7)) & 1u) { g_range[k] = 1; any = true; }
            else if (rgb && !no_lines && __builtin_popcountll(ink) >= 10 && rgb[k * 3] <= 64u && rgb[k * 3 + 1] <= 64u &&
                     rgb[k * 3 + 2] <= 64u) {
                g_range[k] = 1;
                any = true;
            }
        }
    return any ? g_range.data() : nullptr;
}

// The burnt houses' holes made deeper (map 100, the smoke scene after the
// burning town; the user, 2026-10-17, asked for deeper-looking holes in the
// houses, perhaps with AO blobs, then picked mockup D, stronger,
// shots/holes/HR_holes_mockup_lit.png). The holes (broken roofs, doorways)
// are drawn in two dark blue greys, which also draw the roofs' outlines and
// the walls' mortar shadows: a hole pixel is one of them with 4/5 of its
// 5x5 around them too, grown 2 px over them. In the light (the occlusion,
// on a copy: a cached scene keeps its own), each hole pixel darkens by the
// far rim's shadow (its run of hole pixels from above, fading over 3 rows)
// and toward its middle (its distance in, full at 4 px), each at
// kHoleDepth. Only with the occlusion or shadows on. KOL_NO_HOLE_DEPTH=1 off.
struct HoleMap { int map; std::uint8_t c[2][3]; };
constexpr HoleMap kHoleMaps[] = {{100, {{57, 66, 82}, {33, 49, 66}}}};
constexpr float kHoleDepth = 0.75f;
std::vector<std::uint8_t> g_hole_ao;
const std::uint8_t* hole_depth(const std::uint8_t* layers, const std::uint8_t* rgb, int w, int h) {
    static const bool off = std::getenv("KOL_NO_HOLE_DEPTH") != nullptr;
    if (off || !rgb || g_ao.size() != static_cast<std::size_t>(w) * h) return nullptr;
    const HoleMap* hm = nullptr;
    const int map = current_map();
    for (const HoleMap& e : kHoleMaps)
        if (e.map == map) hm = &e;
    if (!hm) return nullptr;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    static thread_local std::vector<std::uint8_t> dark, core, m;
    dark.assign(n, 0);
    bool any = false;
    // The map's own colours: under a sprite or the smoke (BG1), the scene
    // the compositor drew without them, so the scouts and the drifting
    // smoke do not change the holes (and the camera holds still here: the
    // mask is worked out again only when it changes).
    const std::uint8_t* under = g_cap.under_ok && g_cap.under.size() == n * 3 ? g_cap.under.data() : nullptr;
    std::uint32_t key = kol::kFnvBasis;
    for (std::size_t k = 0; k < n; ++k) {
        const unsigned top = scene_top(layers, k);
        const std::uint8_t* c = under && (top == 1u || top == 4u) ? under + k * 3 : rgb + k * 3;
        for (const auto& hc : hm->c)
            if (c[0] == hc[0] && c[1] == hc[1] && c[2] == hc[2]) dark[k] = 1;
        any = any || dark[k];
        if (dark[k]) kol::fnv_mix(key, static_cast<std::uint32_t>(k));
    }
    if (!any) return nullptr;
    static thread_local std::vector<std::uint8_t> factor;   // per pixel, 255 = unchanged
    static thread_local std::uint32_t factor_key = 0;
    static thread_local std::size_t factor_n = 0;
    if (factor_key == key && factor_n == n) {
        g_hole_ao = g_ao;
        for (std::size_t k = 0; k < n; ++k)
            if (factor[k] != 255u) g_hole_ao[k] = static_cast<std::uint8_t>((g_hole_ao[k] * factor[k] + 127u) / 255u);
        return g_hole_ao.data();
    }
    factor_key = key;
    factor_n = n;
    factor.assign(n, 255);
    core.assign(n, 0);
    std::vector<int> I(static_cast<std::size_t>(w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = 0; x < w; ++x) {
            sum += dark[static_cast<std::size_t>(y) * w + x];
            I[static_cast<std::size_t>(y + 1) * (w + 1) + x + 1] = I[static_cast<std::size_t>(y) * (w + 1) + x + 1] + sum;
        }
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            if (!dark[k]) continue;
            const int x0 = std::max(0, x - 2), y0 = std::max(0, y - 2), x1 = std::min(w, x + 3), y1 = std::min(h, y + 3);
            const int cnt = I[static_cast<std::size_t>(y1) * (w + 1) + x1] - I[static_cast<std::size_t>(y0) * (w + 1) + x1] -
                            I[static_cast<std::size_t>(y1) * (w + 1) + x0] + I[static_cast<std::size_t>(y0) * (w + 1) + x0];
            core[k] = cnt * 5 >= 4 * (x1 - x0) * (y1 - y0);
        }
    m = core;
    for (int g = 0; g < 2; ++g) {
        std::vector<std::uint8_t> nm = m;   // (twice a frame, only on map 100)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * w + x;
                if (m[k] || !dark[k]) continue;
                for (int dy = -1; dy <= 1 && !nm[k]; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < w && yy < h && m[static_cast<std::size_t>(yy) * w + xx]) { nm[k] = 1; break; }
                    }
            }
        m.swap(nm);
    }
    auto chamfer = [&](auto inside) {
        std::vector<float> d(n);
        for (std::size_t k = 0; k < n; ++k) d[k] = inside(k) ? 99.0f : 0.0f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float& v = d[static_cast<std::size_t>(y) * w + x];
                if (v == 0.0f) continue;
                if (x > 0) v = std::min(v, d[static_cast<std::size_t>(y) * w + x - 1] + 1.0f);
                if (y > 0) v = std::min(v, d[static_cast<std::size_t>(y - 1) * w + x] + 1.0f);
                if (x > 0 && y > 0) v = std::min(v, d[static_cast<std::size_t>(y - 1) * w + x - 1] + 1.4f);
                if (x + 1 < w && y > 0) v = std::min(v, d[static_cast<std::size_t>(y - 1) * w + x + 1] + 1.4f);
            }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) {
                float& v = d[static_cast<std::size_t>(y) * w + x];
                if (v == 0.0f) continue;
                if (x + 1 < w) v = std::min(v, d[static_cast<std::size_t>(y) * w + x + 1] + 1.0f);
                if (y + 1 < h) v = std::min(v, d[static_cast<std::size_t>(y + 1) * w + x] + 1.0f);
                if (x + 1 < w && y + 1 < h) v = std::min(v, d[static_cast<std::size_t>(y + 1) * w + x + 1] + 1.4f);
                if (x > 0 && y + 1 < h) v = std::min(v, d[static_cast<std::size_t>(y + 1) * w + x - 1] + 1.4f);
            }
        return d;
    };
    const std::vector<float> din = chamfer([&](std::size_t k) { return m[k] != 0; });
    g_hole_ao = g_ao;
    for (int x = 0; x < w; ++x) {
        int run = 0;
        for (int y = 0; y < h; ++y) {
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            run = m[k] ? run + 1 : 0;
            if (!m[k]) continue;
            const float f = (1.0f - kHoleDepth * std::exp(-(run - 1) / 3.0f)) *
                            (1.0f - kHoleDepth * std::min(1.0f, (din[k] - 1.0f) / 4.0f));
            factor[k] = static_cast<std::uint8_t>(std::lround(255.0f * f));
            g_hole_ao[k] = static_cast<std::uint8_t>((g_hole_ao[k] * factor[k] + 127u) / 255u);
        }
    }
    return g_hole_ao.data();
}

namespace {
// Fire light (ScenePixels::fire_lights): the flames, and the fire arrows of
// the archers' scene (actors 0x69-0x6D on map 98), with Torchlight.
// KOL_FIRE_GLOW=<flame power>,<flame reach>,<arrow power>,<arrow reach>,
// <arrow halo> (mockups).
void fire_lights_now(ScenePixels* out) {
    // The user's picks from shots/sunset/HK_fire_light_mockups.png: the
    // flames B (1.1, 72), the arrows A (0.9, 48, a 10 px halo).
    static float fg[5] = {1.1f, 72.0f, 0.9f, 48.0f, 10.0f};
    static const bool fg_env = [] {
        const char* v = std::getenv("KOL_FIRE_GLOW");
        return v && std::sscanf(v, "%f,%f,%f,%f,%f", &fg[0], &fg[1], &fg[2], &fg[3], &fg[4]) == 5;
    }();
    (void)fg_env;
    static std::vector<kol::ScenePixels::FireLight> lights;
    lights.clear();
    if (glow_strength() > 0.0f) {
        for (const FlameAt& f : g_flame_at) lights.push_back({f.x, f.y + 16.0f, f.z, fg[1], fg[0], 0.0f});
        // A map's own flame that throws its light (MapFlame::light): from 10 over its foot.
        for (const MapFlame& f : kMapFlames)
            if (f.light > 0.0f && f.map == current_map()) lights.push_back({f.x, f.y + 10.0f, f.z, f.light_reach, f.light, 0.0f});
        const std::uint8_t* ew = g_cap.ew.size() ? g_cap.ew.data() : nullptr;
        if (ew && rd16(ew, kMapNumber - 0x02000000u) == 98) {
            const std::uint32_t list = rd32(ew, kObjectsPtr - 0x02000000u);
            for (int i = 0; i < kObjects && in_ewram(list, kObjects * kObjStride); ++i) {
                const std::uint32_t o = list - 0x02000000u + static_cast<std::uint32_t>(i) * kObjStride;
                const std::uint32_t rec = rd32(ew, o);
                if (!in_ewram(rec, 0x68)) continue;
                const std::uint8_t actor = ew[rec - 0x02000000u + 0x4E];
                if (actor < 0x69 || actor > 0x6D) continue;   // (until the script deletes it: landed, it burns on)
                lights.push_back({static_cast<float>(static_cast<std::int16_t>(rd16(ew, o + 0x10))),
                                  static_cast<float>(static_cast<std::int16_t>(rd16(ew, o + 0x12))),
                                  static_cast<float>(static_cast<std::int16_t>(rd16(ew, o + 0x14))), fg[3], fg[2],
                                  fg[4]});
            }
        }
    }
    out->fire_lights = lights.empty() ? nullptr : lights.data();
    out->fire_light_count = static_cast<int>(lights.size());
}

// A spell's light (ScenePixels::spell_lights; asked 2026-10-27, states/JW,
// for the spell projectile to cast light of its colour, the user's pick C
// of three mockups, shots in the session's scratchpad spell/mock). While
// a spell runs (its charge or its effect: perspective_spell_running), its
// pixels that are bright
// and saturated (spell_light_colour): the OAM pieces no unit owns (the
// charge's orb over the caster, the sparks; not the weather's), and the
// affine BG2 of an effect drawn there (Ice Javelin's spike, the capture's
// spell_bg); not priority 0, the interface's and Crag Crush's rocks and
// dust (a muddy ochre light). Gathered in 24-pixel cells, touching cells one cluster, each
// cluster a light of its colour from over the unit under it, at the
// effect's height (a row is 2 units): the orb over the caster lights the
// ground round the caster's feet, the impact the target. Survey (GD, ID,
// IF, JQ): every charge is one priority-1 piece in the element's colour.
// With Torchlight. KOL_SPELL_LIGHT=<reach>,<power> (155, 1:
// C's 110 px across), KOL_NO_SPELL_LIGHT=1 none, KOL_SPELL_LIGHT_LOG=1.
void spell_lights_now(int w, int h, ScenePixels* out) {
    static float sl[2] = {155.0f, 1.0f};
    static const bool sl_env = [] {
        const char* v = std::getenv("KOL_SPELL_LIGHT");
        return v && std::sscanf(v, "%f,%f", &sl[0], &sl[1]) == 2;
    }();
    (void)sl_env;
    static const bool off = std::getenv("KOL_NO_SPELL_LIGHT") != nullptr;
    static const bool log = std::getenv("KOL_SPELL_LIGHT_LOG") != nullptr;
    static std::vector<kol::ScenePixels::SpellLight> spells;
    spells.clear();
    if (!off && g_cap.spell_running && glow_strength() > 0.0f) {
        struct Sample { int x, y; unsigned r, g, b; };
        static std::vector<Sample> samples;
        samples.clear();
        const std::size_t pixels = static_cast<std::size_t>(w) * h;
        if (g_cap.obj_ok && g_obj_top_valid && g_obj_top.size() == pixels) {
            const std::uint8_t* tiles = g_cap.obj_tiles.data();
            const std::uint8_t* pal = g_cap.pal.data();
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px) {
                    const int k = g_obj_top[static_cast<std::size_t>(py) * w + px];
                    if (k < 0 || k >= g_entry_n) continue;
                    const ObjEntry& e = g_entries[k];
                    // Priority 0 is the interface's, and Crag Crush's
                    // rocks and dust (ochre and grey: a muddy light);
                    // the charge's orb and the sparks are priority 1.
                    if (e.owner >= 0 || weather_tile(e.a2) || ((e.a2 >> 10) & 3u) == 0u) continue;
                    const unsigned t = obj_texel(e, px - g_info.ox - e.x, py - g_info.oy - e.y, tiles, g_map_1d);
                    if (!t) continue;
                    const unsigned slot = (e.a0 & 0x2000u) ? t : ((e.a2 >> 12) & 0xFu) * 16u + t;
                    const unsigned c = pal[0x200u + slot * 2u] | (pal[0x201u + slot * 2u] << 8);
                    const unsigned r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
                    if (spell_light_colour(r, g, b)) samples.push_back({px, py, r, g, b});
                }
        }
        for (const Capture::SpellSample& s : g_cap.spell_bg)
            samples.push_back({s.x + g_info.vx, s.y + g_info.vy, s.r, s.g, s.b});
        // Cells, then clusters of touching cells.
        constexpr int kCell = 24;
        const int cw = (w + kCell - 1) / kCell, ch = (h + kCell - 1) / kCell;
        struct Acc { int n = 0; double x = 0, y = 0, r = 0, g = 0, b = 0; };
        static std::vector<Acc> cells;
        cells.assign(static_cast<std::size_t>(cw) * ch, Acc{});
        for (const Sample& s : samples) {
            if (s.x < 0 || s.y < 0 || s.x >= w || s.y >= h) continue;
            Acc& a = cells[static_cast<std::size_t>(s.y / kCell) * cw + s.x / kCell];
            ++a.n; a.x += s.x; a.y += s.y; a.r += s.r; a.g += s.g; a.b += s.b;
        }
        static std::vector<int> stack;
        for (int c0 = 0; c0 < cw * ch && spells.size() < 8; ++c0) {
            if (cells[c0].n == 0) continue;
            Acc sum;
            stack.assign(1, c0);
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                Acc& a = cells[c];
                if (a.n == 0) continue;
                sum.n += a.n; sum.x += a.x; sum.y += a.y; sum.r += a.r; sum.g += a.g; sum.b += a.b;
                a.n = 0;
                const int cx = c % cw, cy = c / cw;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx >= 0 && ny >= 0 && nx < cw && ny < ch && cells[ny * cw + nx].n) stack.push_back(ny * cw + nx);
                    }
            }
            if (sum.n < 6) continue;   // a speck
            const float sx = static_cast<float>(sum.x / sum.n), sy = static_cast<float>(sum.y / sum.n);
            // The unit under it: within 40 px across, its feet at or below.
            int best = -1;
            float best_d = 1e9f;
            for (int i = 0; i < g_info.count; ++i) {
                const Unit& u = g_info.units[i];
                const float fx = static_cast<float>(feet_sx(u) + g_info.kx + g_info.ox);
                const float fy = static_cast<float>(feet_sy(u) + g_info.ky + g_info.oy);
                if (std::abs(fx - sx) > 40.0f || fy < sy - 8.0f) continue;
                const float d = std::abs(fx - sx) + 0.5f * (fy - sy);
                if (d < best_d) { best_d = d; best = i; }
            }
            if (best < 0) continue;
            const Unit& u = g_info.units[best];
            const float fy = static_cast<float>(feet_sy(u) + g_info.ky + g_info.oy);
            const float Y = static_cast<float>(u.y - 8) + 2.0f * std::max(0.0f, fy - sy);
            const float r = static_cast<float>(sum.r / sum.n), g = static_cast<float>(sum.g / sum.n),
                        b = static_cast<float>(sum.b / sum.n);
            const float mx = std::max({r, g, b, 1.0f});
            spells.push_back({static_cast<float>(u.x), Y, static_cast<float>(u.z), sl[0], sl[1], r / mx, g / mx, b / mx});
            if (log)
                std::fprintf(stderr, "spell_light: %d px at (%.0f,%.0f) over unit %d (%d,%d,%d) Y %.0f colour %.0f,%.0f,%.0f\n",
                             sum.n, sx, sy, best, u.x, u.y, u.z, Y, r, g, b);
        }
    }
    // The circle on the floor (circle_capture): its light, a pool of its
    // colour from over its middle, as wide as it, as strong as it pulses.
    // The spell's closing circle, in the scene's pixels (Special Effects: its rim bends
    // the picture, kol_perspective ring_lens).
    out->ring_on = g_cap.ring_on && kol::setting_on(kol::Setting::SpecialEffects);
    if (out->ring_on) {
        out->ring[0] = g_cap.ring[0] + static_cast<float>(g_info.ox);
        out->ring[1] = g_cap.ring[1] + static_cast<float>(g_info.oy);
        out->ring[2] = g_cap.ring[2];
    }
    out->circle = nullptr;
    if (g_cap.circle_on && g_cap.circle.size() == 256u * 256u) {
        out->circle = g_cap.circle.data();
        out->circle_su = g_cap.circle_u - g_info.vx;
        out->circle_sv = g_cap.circle_v - g_info.vy;
        std::copy_n(g_cap.circle_box, 4, out->circle_box);
        std::copy_n(g_cap.circle_rgb, 3, out->circle_rgb);
        out->circle_eva = g_cap.circle_eva;
        out->circle_evb = g_cap.circle_evb;
        static const float k_light = kol::env_num_min("KOL_CIRCLE_LIGHT", 0.0f, 0.4f);
        const float peak = static_cast<float>(std::max({g_cap.circle_rgb[0], g_cap.circle_rgb[1], g_cap.circle_rgb[2]}));
        const int cx = (g_cap.circle_box[0] + g_cap.circle_box[2]) / 2 - out->circle_su;
        const int cy = (g_cap.circle_box[1] + g_cap.circle_box[3]) / 2 - out->circle_sv;
        if (k_light > 0.0f && peak >= 8.0f && glow_strength() > 0.0f && cx >= 0 && cy >= 0 && cx < w && cy < h) {
            const std::size_t k = static_cast<std::size_t>(cy) * w + cx;
            const float xz = static_cast<float>(g_xz[k]), dxz = 2.0f * static_cast<float>(cx - g_info.kx - g_info.ox);
            // (Its half width on the screen is 0.71 of its radius in the world.)
            const float reach = 1.41f * 0.5f * static_cast<float>(g_cap.circle_box[2] - g_cap.circle_box[0]);
            spells.push_back({0.5f * (xz + dxz), static_cast<float>(g_y[k]) + 24.0f, 0.5f * (xz - dxz), reach,
                              k_light * peak / 174.0f, g_cap.circle_rgb[0] / peak, g_cap.circle_rgb[1] / peak,
                              g_cap.circle_rgb[2] / peak});
        }
    }
    out->spell_lights = spells.empty() ? nullptr : spells.data();
    out->spell_light_count = static_cast<int>(spells.size());
}

// The fades on the scene (ScenePixels::fx_dark, fx_white, fx_obj_*,
// scene_bright, scene_white, backdrop_white): BLDY on the map's layers and
// the sprites, and the palette fades of the scenes.
void scene_fades(ScenePixels* out) {
    const unsigned effect = (g_cap.bldcnt >> 6) & 3u, evy = std::min(16u, g_cap.bldy & 0x1Fu);
    const bool bg1 = (g_cap.bldcnt & 0x02u) != 0u;
    out->fx_dark = bg1 && effect == 3u ? static_cast<float>(evy) / 16.0f : 0.0f;
    out->fx_white = bg1 && effect == 2u ? static_cast<float>(evy) / 16.0f : 0.0f;
    // And the sprites' (map 63's campfire stayed lit through the camp's
    // fade to black: BLDCNT 00ff, BLDY 1 -> 16; the user, states/thor/0040).
    const bool obj = (g_cap.bldcnt & 0x10u) != 0u;
    out->fx_obj_dark = obj && effect == 3u ? static_cast<float>(evy) / 16.0f : 0.0f;
    out->fx_obj_white = obj && effect == 2u ? static_cast<float>(evy) / 16.0f : 0.0f;
    // The scenes fade by the palette (their 5e op: BLDY stays 0): PAL RAM
    // and the palette buffer (0x02002400) both go dark, from the scene's
    // full palette at 0x0202D90C (found mid-fade; maps 99 and 100). The
    // flames' and smoke's own palette (BG 15) as it is against that one,
    // darker or whiter.
    constexpr std::uint32_t kFullPal = 0x2D90Cu;   // EWRAM offset
    // (Only where BG 15 is the flames' or the smoke's: on map 63 it is
    // other art, and its campfire's effect, kFireActors, came out
    // whitened.)
    const int fx_map = current_map();
    const bool fx_pal_map = kol::map_flag(kol::MapFlag::Flames, fx_map) || kol::map_flag(kol::MapFlag::Smoke, fx_map);
    if (fx_pal_map && g_cap.pal.size() >= 0x200u && g_cap.ew.size() > kFullPal + 0x200u) {
        unsigned now = 0, full = 0;
        for (unsigned i = 15u * 16u + 1u; i < 16u * 16u; ++i) {
            const unsigned c = g_cap.pal[i * 2] | (g_cap.pal[i * 2 + 1] << 8);
            const unsigned f = g_cap.ew[kFullPal + i * 2] | (g_cap.ew[kFullPal + i * 2 + 1] << 8);
            now += (c & 31u) + ((c >> 5) & 31u) + ((c >> 10) & 31u);
            full += (f & 31u) + ((f >> 5) & 31u) + ((f >> 10) & 31u);
        }
        if (full > 0u && now < full)
            out->fx_dark = 1.0f - (1.0f - out->fx_dark) * static_cast<float>(now) / static_cast<float>(full);
        else if (full > 0u && now > full)
            out->fx_white = std::max(out->fx_white, std::min(1.0f, static_cast<float>(now - full) /
                                                                 static_cast<float>(std::max(1u, 15u * 93u - full))));
    }
    // The whole BG palette against the full one, for scene_bright: only
    // where the live palette is that one darkened (nine entries in ten
    // no brighter in any channel; a battle map's 0x0202D90C is something
    // else), full from 0.95 up (a few entries differ: HS reads 0.98).
    out->scene_bright = 1.0f;
    if (g_cap.pal.size() >= 0x200u && g_cap.ew.size() > kFullPal + 0x200u) {
        unsigned now = 0, full = 0, used = 0, darker = 0;
        for (unsigned i = 1u; i < 256u; ++i) {
            const unsigned c = g_cap.pal[i * 2] | (g_cap.pal[i * 2 + 1] << 8);
            const unsigned f = g_cap.ew[kFullPal + i * 2] | (g_cap.ew[kFullPal + i * 2 + 1] << 8);
            if (!f) continue;
            ++used;
            if ((c & 31u) <= (f & 31u) && ((c >> 5) & 31u) <= ((f >> 5) & 31u) &&
                ((c >> 10) & 31u) <= ((f >> 10) & 31u))
                ++darker;
            now += (c & 31u) + ((c >> 5) & 31u) + ((c >> 10) & 31u);
            full += (f & 31u) + ((f >> 5) & 31u) + ((f >> 10) & 31u);
        }
        if (used >= 32u && darker * 10u >= used * 9u && full > 0u) {
            const float r = static_cast<float>(now) / static_cast<float>(full);
            out->scene_bright = std::min(1.0f, r / 0.95f);
        }
    }
    {
        // BLDY darkening the map's layers (the castle room's fade out:
        // BLDCNT 00f3, BG0 BG1 OBJ and backdrop, BLDY 1 -> 16 over 50 frames).
        const unsigned effect = (g_cap.bldcnt >> 6) & 3u;
        if (effect == 3u && (g_cap.bldcnt & 0x03u) == 0x03u)
            out->scene_bright *= 1.0f - static_cast<float>(std::min(16u, g_cap.bldy & 0x1Fu)) / 16.0f;
        // And BLDY whitening them (the opening's fade to white on map 102:
        // BLDCNT 00b3, BLDY to 16; states/HW_slot3_cutscene_thunder).
        // KOL_NO_UNDER_WHITE=1: the light laid over the whitened frame (old).
        static const bool no_white = std::getenv("KOL_NO_UNDER_WHITE") != nullptr;
        out->scene_white = !no_white && effect == 2u && (g_cap.bldcnt & 0x03u) == 0x03u
            ? static_cast<float>(std::min(16u, g_cap.bldy & 0x1Fu)) / 16.0f : 0.0f;
        out->backdrop_white = !no_white && effect == 2u && (g_cap.bldcnt & 0x20u)
            ? static_cast<float>(std::min(16u, g_cap.bldy & 0x1Fu)) / 16.0f : 0.0f;
        // Or the map's own palette set to white: the "An Excerpt from the
        // Ogre Battle Saga" title over map 102 turns the BG palette rows
        // the map uses to 0x7FFF (PAL RAM and the buffer at 0x02002400
        // alike: the colours are kept nowhere), blends its text over that
        // (BLDCNT 1344) and then fades it all to black (00ff). The rows
        // the map uses are read from its composed tilemap (0x02004440,
        // BG0 then BG1 at +0x6000, 128 x 96 entries); nine in ten of
        // their colours white is a map whited out (a map's own whites
        // are a few).
        // (Nine in ten of the used rows' colours white needs a row with
        // 14 or 15 of its 15 white: with every row at 13 or under, no set
        // of rows comes to 135 in 150, so the tilemap is not walked.)
        auto white_in_row = [&](unsigned r) {
            unsigned n = 0;
            for (unsigned i = 1; i < 16u; ++i)
                n += ((g_cap.pal[(r * 16u + i) * 2] | (g_cap.pal[(r * 16u + i) * 2 + 1] << 8)) & 0x7FFFu) == 0x7FFFu;
            return n;
        };
        bool whitish = false;
        if (!no_white && out->scene_white < 1.0f && g_cap.pal.size() >= 0x200u)
            for (unsigned r = 0; r < 16u && !whitish; ++r) whitish = white_in_row(r) >= 14u;
        if (whitish && g_cap.ew.size() >= 0x40000u) {
            const std::uint32_t arr = rd32(g_cap.ew.data(), 0x26CA8u);
            if (arr >= 0x02000000u && arr + 0xC000u <= 0x02040000u) {
                std::uint16_t rows = 0;
                const std::uint8_t* m = g_cap.ew.data() + (arr - 0x02000000u);
                for (std::size_t i = 0; i < 2u * 96u * 128u; ++i) {
                    const unsigned e = m[i * 2] | (m[i * 2 + 1] << 8);
                    if (e & 0x3FFu) rows = static_cast<std::uint16_t>(rows | (1u << (e >> 12)));
                }
                unsigned used = 0, white = 0;
                for (unsigned r = 0; r < 16u; ++r) {
                    if (!(rows & (1u << r))) continue;
                    for (unsigned i = 1; i < 16u; ++i) {
                        const unsigned c = (g_cap.pal[(r * 16u + i) * 2] | (g_cap.pal[(r * 16u + i) * 2 + 1] << 8)) & 0x7FFFu;
                        ++used;
                        white += c == 0x7FFFu;
                    }
                }
                if (used >= 15u && white * 10u >= used * 9u) out->scene_white = 1.0f;
            }
        }
    }
}
}  // namespace
void hud_bake_into(std::uint8_t* rgb, int w, int h) {
    if (!rgb || g_cap.hud_bake.empty() || w != g_cap.scene_w || h != g_cap.scene_h) return;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (const auto& b : g_cap.hud_bake)
        if (b.k < n) std::copy_n(b.rgb, 3, rgb + static_cast<std::size_t>(b.k) * 3);
}
// How dark the map's own palette is against its brightest since the map came
// up: the BG palette rows its composed tilemap uses ([0x02026CA8], BG0 then
// BG1, 128 x 96 entries each), their 5-bit channel sums now over each row's
// most since the map changed. Map 93's ending (states/ending_map93) darkens
// rows 0-3 to ~58% under "And, that was the last..." while the backdrop keeps
// its colour; the sky recoloured green stayed bright around a dimmed map (the
// user, 2026-11-13, asked for the green to dim with the rest of the
// screen). Not raised by a flash (fx_white, scene_white). A row's reference is
// its first settled sum, raised only to a brighter one held kHold pictures: map
// 73's flash (states/thor/0255, rows 0, 2, 3 brightened ~25% for ~90 frames,
// not a white blend) took the most up, and the cave stayed 20% dim after it.
// What the sky's recolouring (map 93) and our effects' colours (map 73's
// falls, ScenePixels::map_dim) follow. KOL_NO_SKY_DIM=1: 1.
float map_palette_dim(const ScenePixels& sp) {
    static const bool off = std::getenv("KOL_NO_SKY_DIM") != nullptr;
    static int map_seen = -1;
    static std::array<unsigned, 16> ref{}, cand{};
    static std::array<int, 16> held{};
    constexpr int kHold = 120;
    if (off || g_cap.pal.size() < 0x200u || g_cap.ew.size() < 0x40000u) return 1.0f;
    const std::uint32_t arr = rd32(g_cap.ew.data(), 0x26CA8u);
    if (arr < 0x02000000u || arr + 0xC000u > 0x02040000u) return 1.0f;
    static int settle = 0;   // pictures since the map changed
    if (sp.map != map_seen) { map_seen = sp.map; ref.fill(0u); cand.fill(0u); held.fill(0); settle = 0; }
    std::uint16_t rows = 0;
    const std::uint8_t* m = g_cap.ew.data() + (arr - 0x02000000u);
    for (std::size_t i = 0; i < 2u * 96u * 128u; ++i) {
        const unsigned e = m[i * 2] | (m[i * 2 + 1] << 8);
        if (e & 0x3FFu) rows = static_cast<std::uint16_t>(rows | (1u << (e >> 12)));
    }
    // The map's first pictures (a state loaded: the scene before's palette
    // for a picture, two of map 93's rows empty) set no most: none for
    // kSettle pictures, and none while a used row is all black.
    constexpr int kSettle = 8;
    std::array<unsigned, 16> sums{};
    bool empty_row = false;
    for (unsigned r = 0; r < 16u; ++r) {
        if (!(rows & (1u << r))) continue;
        for (unsigned i = 1; i < 16u; ++i) {
            const unsigned c = g_cap.pal[(r * 16u + i) * 2] | (g_cap.pal[(r * 16u + i) * 2 + 1] << 8);
            sums[r] += (c & 31u) + ((c >> 5) & 31u) + ((c >> 10) & 31u);
        }
        empty_row = empty_row || sums[r] == 0u;
    }
    if (empty_row) return 1.0f;
    if (settle < kSettle) { ++settle; return 1.0f; }
    const bool flash = sp.fx_white > 0.0f || sp.scene_white > 0.0f;
    unsigned now_sum = 0, ref_sum = 0;
    for (unsigned r = 0; r < 16u; ++r) {
        if (!(rows & (1u << r))) continue;
        const unsigned s = sums[r];
        if (ref[r] == 0u) ref[r] = s;   // (the first settled sum)
        else if (s > ref[r] && !flash) {
            if (cand[r] == s) ++held[r];
            else { cand[r] = s; held[r] = 1; }
            if (held[r] >= kHold) ref[r] = s;
        } else {
            held[r] = 0;
        }
        now_sum += s;
        ref_sum += ref[r];
    }
    // KOL_SKY_DIM_LOG=1: the rows used, each row's sum and most, a picture.
    static const bool log = std::getenv("KOL_SKY_DIM_LOG") != nullptr;
    if (log) {
        std::fprintf(stderr, "sky_dim: rows %04x", rows);
        for (unsigned r = 0; r < 16u; ++r) {
            if (!(rows & (1u << r))) continue;
            std::fprintf(stderr, " %u:%u/%u", r, sums[r], ref[r]);
        }
        std::fprintf(stderr, "\n");
    }
    return ref_sum ? std::min(1.0f, static_cast<float>(now_sum) / static_cast<float>(ref_sum)) : 1.0f;
}
bool scene_pixels(const std::uint8_t* layers, int w, int h, ScenePixels* out,
                  const std::uint8_t* rgb) {
    PresentScope scope;
    g_scene_rgb = rgb;
    static const bool dump = std::getenv("KOL_FOCUS_DUMP") != nullptr;
    if (dump && rgb) g_dump_rgb.assign(rgb, rgb + static_cast<std::size_t>(w) * h * 3);
    const bool built = build_scene(layers, w, h);
    g_scene_rgb = nullptr;
    if (!built) return false;
    out->w = w; out->h = h;
    out->ox = g_info.ox; out->oy = g_info.oy;
    out->vx = g_info.vx; out->vy = g_info.vy;
    out->vw = g_info.vw; out->vh = g_info.vh;
    out->kx = g_info.kx; out->ky = g_info.ky;
    out->xz = g_xz.data(); out->y = g_y.data();
    read_shutter(rgb, layers, w, h, g_info.ox, g_info.oy, g_info.kx, g_info.ky);
    fire_lights_now(out);
    spell_lights_now(w, h, out);
    scene_fades(out);
    out->hud_glow_layers = g_cap.hud_glow_layers;
    out->hud_glow_second = (g_cap.bldcnt >> 8) & 0x3Fu;
    out->hud_glow = g_cap.hud_glow.empty() ? nullptr : g_cap.hud_glow.data();
    out->hud_glow_n = static_cast<int>(g_cap.hud_glow.size());
    // Over a map whited out (scene_white 1: map 102's title cards, the map's
    // palette rows set to white), a blended HUD layer is the game's text
    // fading in or out, darker than what it is laid on: as light (the frame
    // less the scene) it was nothing, and "An Excerpt from the Ogre Battle
    // Saga" was missing while it faded (Thor state 0257; the user,
    // 2026-11-13, reported the text over the white screens disappearing).
    // Drawn flat from the frame, as the game blends it; the white
    // under it does not move. KOL_GLOW_OVER_WHITE=1: as before.
    static const bool glow_over_white = std::getenv("KOL_GLOW_OVER_WHITE") != nullptr;
    if (!glow_over_white && out->scene_white >= 1.0f) {
        out->hud_glow_layers = 0;
        out->hud_glow = nullptr;
        out->hud_glow_n = 0;
    }
    out->hud_glow_apex_ok = g_cap.hud_glow_apex_ok;
    out->hud_glow_add = g_cap.hud_glow_add;
    out->hud_glow_eva = g_cap.hud_glow_eva;
    std::copy_n(g_cap.hud_glow_apex, 2, out->hud_glow_apex);
    // The flash's field (kol::ui_field), as the game blends it.
    // (Only when the picture shows the layer: the frame before the flash has
    // its white tiles loaded and BG3 off, and drew all white.)
    out->field_layer = g_cap.ui_field >= 2 && (g_cap.dispcnt & (0x100u << g_cap.ui_field)) ? g_cap.ui_field : -1;
    // Shown through windows: drawn over the moved picture where they show it
    // (ScenePixels::window_light), not blended over every pixel.
    out->window_light = g_cap.window_light;
    if (out->window_light.on) out->field_layer = -1;
    if (out->field_layer >= 2) {
        std::copy_n(g_cap.ui_field_rgb, 3, out->field_rgb);
        const unsigned effect = (g_cap.bldcnt >> 6) & 3u;
        const bool first = (g_cap.bldcnt >> out->field_layer) & 1u;
        if (effect == 1u && first) {
            out->field_eva = static_cast<int>(std::min(16u, g_cap.bldalpha & 0x1Fu));
            out->field_evb = static_cast<int>(std::min(16u, (g_cap.bldalpha >> 8) & 0x1Fu));
            out->field_second = (g_cap.bldcnt >> 8) & 0x3Fu;
        } else {
            out->field_eva = 16; out->field_evb = 0; out->field_second = 0;   // drawn over all
        }
        // A fade the field is a target of takes its colour too (BLDY toward
        // white or black): map 57's scene ends on a black BG2 under BLDCNT
        // 0x00BF, BLDY 16 down to 0, the game's picture white going to
        // black, and the field's own black was laid over it all the way
        // (states/thor/0209, frames 1014-1144). KOL_FIELD_NO_FADE=1 old.
        static const bool no_fade = std::getenv("KOL_FIELD_NO_FADE") != nullptr;
        if (!no_fade && first && (effect == 2u || effect == 3u)) {
            const int y = static_cast<int>(std::min(16u, g_cap.bldy & 0x1Fu));
            for (int ch = 0; ch < 3; ++ch) {
                const int v = out->field_rgb[ch];
                out->field_rgb[ch] = static_cast<std::uint8_t>(effect == 2u ? v + (255 - v) * y / 16 : v - v * y / 16);
            }
        }
    }
    out->kind = g_kind.data(); out->iface = g_iface.data();
    out->ao = (ao_strength() > 0.0f || shadow_strength() > 0.0f) &&
                      g_ao.size() == static_cast<std::size_t>(w) * h ? g_ao.data() : nullptr;
    if (out->ao)
        if (const std::uint8_t* deeper = hole_depth(layers, rgb, w, h)) out->ao = deeper;   // map 100's holes
    out->glow = g_glow_any && g_glow.size() == static_cast<std::size_t>(w) * h ? g_glow.data() : nullptr;
    out->glow_lamp = g_glow_lamp.data();
    const bool mix_ok = g_glow_mix.size() == g_glow_lamp.size() && g_glow_hue2.size() == g_glow_lamp.size();
    out->glow_hue2 = mix_ok ? g_glow_hue2.data() : nullptr;
    out->glow_mix = mix_ok ? g_glow_mix.data() : nullptr;
    out->glow_flick = g_glow_flick;
    out->glow_fx = g_glow_fx;
    out->glow_key = g_glow_view_key;
    out->glow_fy = g_glow_fy;
    out->glow_lx = g_glow_lx;
    out->glow_ly = g_glow_ly;
    out->glow_hue = g_glow_hue;
    out->glow_s = glow_strength();
    const bool win = g_win_any && g_win_patch.size() == static_cast<std::size_t>(w) * h;
    out->win_patch = win ? g_win_patch.data() : nullptr;
    out->win_haze = win ? g_win_haze.data() : nullptr;
    // A flash adds kFlashLight to the window's light (open is 0.5).
    constexpr float kFlashLight = 1.5f;
    out->win_strength = godray_strength() * (shutter_light(g_shutter_shown) + kFlashLight * g_lightning);
    if (const WindowTorch* w = window_torch()) {
        float sa, sb, sf;
        window_torch_sway(*w, sa, sb, sf);
        out->win_strength *= 1.0f + w->flicker * sf;
    }
    {
        const float* t = map_window_tint(current_map());
        for (int ch = 0; ch < 3; ++ch) out->win_tint[ch] = t[ch];
    }
    out->win_glow = win && g_win_glow.size() == static_cast<std::size_t>(w) * h ? g_win_glow.data() : nullptr;
    const bool moon = g_moon_any && g_moon_patch.size() == static_cast<std::size_t>(w) * h;
    out->moon_patch = moon ? g_moon_patch.data() : nullptr;
    out->moon_haze = moon ? g_moon_haze.data() : nullptr;
    out->moon_strength = godray_strength() * g_moon_gain;
    {
        const float* mt = kMoonTint;
        for (const OwnLightTint& t : kOwnLightTints)
            if (t.map == current_map()) mt = &t.r;
        for (int ch = 0; ch < 3; ++ch) out->moon_tint[ch] = mt[ch];
    }
    {
        static const float env = kol::env_num("KOL_WIN_WHITE_CAP", -1.0f);   // (mockups)
        out->win_white_cap = 0.0f;
        for (const WindowWhiteCap& c : kWindowWhiteCaps)
            if (c.map == current_map()) out->win_white_cap = c.cap;
        if (env >= 0.0f) out->win_white_cap = env;
    }
    out->evening = evening_amount();
    {
        const MapSunset* m = out->evening > 0.0f ? map_sunset() : nullptr;   // (with Shadows on)
        static const char* env = std::getenv("KOL_SUNSET_GRADE");
        out->sunset_grade = env ? static_cast<float>(std::atof(env)) : m ? m->grade : 0.0f;
        if (const SceneSunset* s = env ? nullptr : scene_sunset())
            out->sunset_grade = std::max(out->sunset_grade, s->grade * scene_sunset_colour());
    }
    out->dark = indoor_dark();
    {
        const float shade = fire_logs_shade();   // as the art (the user, after D's 0.45)
        out->fire_logs = g_fire_logs_any && shade < 1.0f && g_fire_logs.size() == static_cast<std::size_t>(w) * h
                             ? g_fire_logs.data() : nullptr;
        out->fire_logs_shade = shade;
        // The ground round a campfire (kFireActors) darkened and reddened
        // in a small ellipse, so the flame stands out from the ground its
        // light made pale yellow: 18 px across the half width, 0.4 darker
        // and 0.4 redder at the middle (the user's pick P, 2026-10-29).
        // KOL_FIRE_GROUND=<radius,depth,red> (radius 0: none).
        static float fg[3] = {18.0f, 0.4f, 0.4f};
        static const bool fg_read = [] {
            if (const char* e = std::getenv("KOL_FIRE_GROUND")) std::sscanf(e, "%f,%f,%f", &fg[0], &fg[1], &fg[2]);
            return true;
        }();
        (void)fg_read;
        if (g_fire_seen && fg[0] > 0.0f) {
            out->fire_ground = true;
            out->fire_x = g_fire_cx;
            out->fire_y = g_fire_cy;
            out->fire_ground_r = fg[0];
            out->fire_ground_depth = fg[1];
            out->fire_ground_red = fg[2];
        }
    }
    if (out->dark < 1.0f)
        if (const DarkPool* dp = dark_pool()) {
            out->pool_level = dp->level;
            out->pool_x = dp->x; out->pool_z = dp->z;
            out->pool_rx = dp->rx; out->pool_rz = dp->rz;
            out->pool_inner = dp->inner;
            out->pool_y = 16.0f * dp->ground;
        }
    if (out->dark < 1.0f)
        if (const DarkCover* dc = dark_cover()) {
            out->cover_level = dc->level;
            out->cover_x0 = dc->x0; out->cover_x1 = dc->x1;
            out->cover_z0 = dc->z0; out->cover_z1 = dc->z1;
            out->cover_y1 = dc->y1;
            out->cover_soft = dc->soft;
        }
    // The palette the picture was drawn with (the scenes darken it:
    // lamp_palette_lut_live).
    out->lamp_pal = out->dark < 1.0f
        ? lamp_palette_lut_live(g_cap.map, g_cap.pal.size() >= 0x200u ? g_cap.pal.data() : nullptr) : nullptr;
    out->lamp_pal_from = lamp_palette_from();
    out->lamps = g_scene_lamps.empty() ? nullptr : g_scene_lamps.data();
    out->lamp_count = static_cast<int>(g_scene_lamps.size());
    out->sun_bump = sun_bump_strength();
    out->bump_plain = false;
    for (int m : kPlainBumpMaps) out->bump_plain = out->bump_plain || m == current_map();
    out->bump_boxed = false;
    for (const BumpPanel& bp : kBumpPanels)
        if (bp.map == current_map()) {
            out->bump_plain = out->bump_boxed = true;
            const float box[6] = {bp.x0, bp.y0, bp.z0, bp.x1, bp.y1, bp.z1};
            std::copy(box, box + 6, out->bump_box);
            out->bump_box_strength = bp.strength;
        }
    out->wet_floor = map_wet_floor() || out->bump_plain;
    out->range = range_mask(layers, w, h, rgb);
    out->lamp_shade = g_lamp_shade.size() == static_cast<std::size_t>(w) * h ? g_lamp_shade.data() : nullptr;
    out->dark_gain = indoor_lamp_gain();
    const bool uwin = g_atlas_on && g_unit_win_any && g_unit_win_patch.size() == static_cast<std::size_t>(w) * h;
    out->unit_win_patch = uwin ? g_unit_win_patch.data() : nullptr;
    out->unit_win_haze = uwin ? g_unit_win_haze.data() : nullptr;
    const bool lwin = g_atlas_on && g_lift_win_any && g_lift_win_patch.size() == static_cast<std::size_t>(w) * h;
    out->lift_win_patch = lwin ? g_lift_win_patch.data() : nullptr;
    out->lift_win_haze = lwin ? g_lift_win_haze.data() : nullptr;
    out->unit_glow = g_atlas_on && g_unit_glow_any && g_unit_glow.size() == static_cast<std::size_t>(w) * h
                         ? g_unit_glow.data() : nullptr;
    out->unit_glow_lamp = g_unit_glow_lamp.data();
    out->unit_lamp = g_atlas_on && g_unit_lamp_any ? g_unit_lamp.data() : nullptr;
    out->unit_lamp_n = out->unit_lamp ? g_unit_lamp.size() : 0;
    out->unit_win_patch_a = uwin && !g_unit_win_patch_a.empty() ? g_unit_win_patch_a.data() : nullptr;
    out->unit_win_haze_a = out->unit_win_patch_a ? g_unit_win_haze_a.data() : nullptr;
    out->unit_win_a_n = out->unit_win_patch_a ? g_unit_win_patch_a.size() : 0;
    const bool mwin = g_atlas_on && g_moon_unit_any && g_moon_unit_patch.size() == static_cast<std::size_t>(w) * h;
    out->moon_unit_patch = mwin ? g_moon_unit_patch.data() : nullptr;
    out->moon_unit_haze = mwin ? g_moon_unit_haze.data() : nullptr;
    out->moon_unit_patch_a = mwin && !g_moon_unit_patch_a.empty() ? g_moon_unit_patch_a.data() : nullptr;
    out->moon_unit_haze_a = out->moon_unit_patch_a ? g_moon_unit_haze_a.data() : nullptr;
    out->moon_unit_a_n = out->moon_unit_patch_a ? g_moon_unit_patch_a.size() : 0;
    out->floating = g_floating_any ? g_floating.data() : nullptr;
    out->ovl_card = g_ovl_card_any ? g_ovl_card.data() : nullptr;
    out->water = water_mask(layers, w, h);
    g_ice_lift_any = false;
    out->ice = ice_mask(layers, w, h, rgb);
    out->ice_lift = g_ice_lift_any ? g_ice_lift.data() : nullptr;
    out->weather = nullptr;
    out->flakes = nullptr;
    out->flake_count = 0;
    if (out->ice) {
        build_weather_px(layers, w, h, g_info.ox, g_info.oy);
        if (g_weather_px.size() == static_cast<std::size_t>(w) * h) out->weather = g_weather_px.data();
        weather_reflections(w, h, g_info.ox, g_info.oy);
        out->flakes = g_flakes.data();
        out->flake_count = static_cast<int>(g_flakes.size());
    }
    out->serial = g_cap.serial;
    out->edge_fade = g_edge_fade_on && g_edge_fade.size() == static_cast<std::size_t>(w) * h
                         ? g_edge_fade.data() : nullptr;
    out->sky = g_sky;
    out->sky_desat = g_sky_desat;
    out->rain = g_cap.rain;
    out->rain_level = g_cap.rain_level;
    out->snow_level = g_cap.snow_level;
    out->story = g_cap.story;
    out->map = g_cap.map;
    out->map_loads = g_cap.map_loads;
    out->bg0_ok = g_cap.bg0_ok;
    out->bg0_h = g_cap.bg0_h;
    out->bg0_v = g_cap.bg0_v;
    out->lag_pal = g_cap.palette_lag && g_cap.pal.size() >= 0x200u ? g_cap.pal.data() : nullptr;
    out->lag_sun_set = g_cap.scene_sunset >= 1.0f;
    out->sky_mix = 1.0f;
    out->pal_warm = 0.0f;
    if (current_map() == 84 && g_cap.pal.size() >= 0x200u) {
        unsigned r = 0, g = 0, b = 0;
        for (unsigned row : {0u, 3u})   // (map 84's ice: the two rows its scene recolours)
            for (unsigned i = 1; i < 16u; ++i) {
                const unsigned c = g_cap.pal[(row * 16u + i) * 2] | (g_cap.pal[(row * 16u + i) * 2 + 1] << 8);
                r += c & 31u; g += (c >> 5) & 31u; b += (c >> 10) & 31u;
            }
        out->pal_warm = (static_cast<float>(r) - static_cast<float>(b)) / static_cast<float>(r + g + b + 1u);
    }
    out->map_dim = map_palette_dim(*out);
    out->sky_dim = out->sky != kol::kSkyGame ? out->map_dim : 1.0f;
    if (scene_sunset()) {   // the dusk sky as the sun goes down (scene_sunset)
        out->sky = kol::kSkyDusk;
        out->sky_mix = scene_sunset()->sky * scene_sunset_colour();
    }
    out->wall_px = g_wall_px_any && g_wall_px.size() == static_cast<std::size_t>(w) * h ? g_wall_px.data() : nullptr;
    // A sea map's foam laid on the sea (g_sea_decal_px): lit as the map too (the
    // user, 2026-11-11, asked for the foam to take the lighting too).
    // KOL_SEA_FOAM_UNLIT=1: not.
    {
        static const bool unlit = std::getenv("KOL_SEA_FOAM_UNLIT") != nullptr;
        const std::size_t np = static_cast<std::size_t>(w) * h;
        if (!unlit && g_sea_decal_any && g_sea_decal_px.size() == np) {
            static thread_local std::vector<std::uint8_t> lit;
            if (out->wall_px) {
                lit.assign(out->wall_px, out->wall_px + np);
                for (std::size_t k = 0; k < np; ++k) lit[k] |= g_sea_decal_px[k];
                out->wall_px = lit.data();
            } else {
                out->wall_px = g_sea_decal_px.data();
            }
        }
    }
    // Special Effects' foam round the boat on a sea map (kol_focus_models
    // sea_foam_list): the list, the game's own foam to hide, the water's run.
    // (BG2's scroll, +0.67 px across and -0.33 down a frame, is 1.33 world
    // units a frame along +Z on the sea's plane: X - Z = 2 dx, X + Z = 4 dy.)
    out->sea_foam = g_sea_foam.empty() ? nullptr : g_sea_foam.data();
    out->sea_foam_count = static_cast<int>(g_sea_foam.size());
    out->sea_foam_hide = out->sea_foam && g_sea_decal_any && g_sea_decal_px.size() == static_cast<std::size_t>(w) * h
                             ? g_sea_decal_px.data() : nullptr;
    out->sea_foam_flow = 1.333f;
    out->fall_parts = map_falls_on() && g_modeled.size() == static_cast<std::size_t>(w) * h ? g_modeled.data() : nullptr;
    out->fall_vapor = out->fall_parts ? g_fall_vapor : nullptr;
    {
        // (kFarParts: the map's far part's pixels.)
        static std::vector<std::uint8_t> far;
        float share = 1.0f;
        const int fp = model_far_part(current_map(), &share);
        const std::size_t np = static_cast<std::size_t>(w) * h;
        if (fp >= 0 && g_modeled.size() == np) {
            far.assign(np, 0);
            bool any = false;
            for (std::size_t k = 0; k < np; ++k)
                if (g_modeled[k] == fp + 1) { far[k] = 1; any = true; }
            if (any) { out->far_px = far.data(); out->far_share = share; }
            // The map's cut edge fades (kEdgeFadeMaps) over a third of its
            // width on the far part: map 93's hills are some 30 rows under the
            // art's top cut, and the whole fade (32) took them.
            static std::vector<std::uint8_t> fade;
            if (any && out->edge_fade) {
                fade.assign(out->edge_fade, out->edge_fade + np);
                for (std::size_t k = 0; k < np; ++k)
                    if (far[k]) fade[k] = static_cast<std::uint8_t>(std::min(255u, 3u * fade[k]));
                out->edge_fade = fade.data();
            }
        }
    }
    out->row_backdrop = g_cap.row_backdrop.size() == static_cast<std::size_t>(h) * 3 ? g_cap.row_backdrop.data() : nullptr;
    if (g_atlas_on) {
        out->lift = g_lift.data();
        out->over_sprite = g_over_sprite_any && g_over_sprite.size() == static_cast<std::size_t>(w) * h
                               ? g_over_sprite.data() : nullptr;
        out->over_block = out->over_sprite ? g_over_block.data() : nullptr;
        out->lift_copy = g_lift_copy.size() == static_cast<std::size_t>(w) * h ? g_lift_copy.data() : nullptr;
        out->rope_air = g_rope_air.size() == static_cast<std::size_t>(w) * h ? g_rope_air.data() : nullptr;
        out->run_split = in_list(kModelRunSplitMaps, current_map()) && g_modeled.size() == static_cast<std::size_t>(w) * h
                             ? g_modeled.data()
                             : nullptr;
        out->run_split_drift = in_list(kModelRunFineMaps, current_map()) ? 8 : 0;
        {
            // (kModelRecolours: the mask of the map's listed parts' pixels.)
            static std::vector<std::uint8_t> mask;
            static std::vector<std::uint32_t> pairs;
            static const bool off = std::getenv("KOL_NO_MODEL_RECOLOUR") != nullptr;
            pairs.clear();
            std::uint32_t parts = 0;
            for (const ModelRecolour& r : kModelRecolours)
                if (r.map == current_map()) { pairs.push_back(r.from); pairs.push_back(r.to); parts |= r.parts; }
            const std::size_t np = static_cast<std::size_t>(w) * h;
            if (!off && !pairs.empty() && g_modeled.size() == np) {
                mask.assign(np, 0);
                for (std::size_t k = 0; k < np; ++k)
                    if (g_modeled[k] && g_modeled[k] <= 32 && ((parts >> (g_modeled[k] - 1)) & 1u)) mask[k] = 1;
                out->recolour_mask = mask.data();
                out->recolour_pairs = pairs.data();
                out->recolour_count = static_cast<int>(pairs.size() / 2);
            }
        }
        out->under_xz = g_under_xz.data();
        out->under_y = g_under_y.data();
        out->under_kind = g_under_kind.data();
        out->lift_ao = out->ao && g_lift_ao_on ? g_lift_ao.data() : nullptr;
        out->under_rgb = g_cap.under_ok ? g_cap.under.data() : nullptr;
        if (out->under_rgb && (!g_under_fix.empty() || !g_rope_under_fix.empty() || !g_fx_under_fix.empty())) {
            g_under_patched.assign(g_cap.under.begin(), g_cap.under.end());
            for (const std::vector<UnderFix>* fixes : {&g_under_fix, &g_rope_under_fix, &g_fx_under_fix})
                for (const UnderFix& f : *fixes)
                    if (static_cast<std::size_t>(f.k) * 3 + 2 < g_under_patched.size())
                        std::copy_n(f.rgb, 3, &g_under_patched[static_cast<std::size_t>(f.k) * 3]);
            out->under_rgb = g_under_patched.data();
        }
        out->scene_codes = out->under_rgb ? g_scene_codes : nullptr;
        // A wall painted in behind a model's lifted pixel (kModelWallBehindMaps) is
        // the layer under it now, not the backdrop: the warp fills the backdrop
        // under a lifted pixel with the sky's row (smooth_sky), and did here.
        // A lifted rope's fill (g_rope_under_fix: the boat's colour beside it) is
        // the layer under it too: a rope painted on BG0 has the backdrop under
        // it, so the warp put the sky's row there, blue streaks along map 90's
        // ropes over the deck (the user, 2026-11-11, Thor state 221).
        // KOL_ROPE_FILL_SKY=1: old.
        static const bool rope_fill_sky = std::getenv("KOL_ROPE_FILL_SKY") != nullptr;
        const bool fx_codes = !g_fx_under_fix.empty() && in_list(kModelWallBehindMaps, current_map());
        const bool rope_codes = !rope_fill_sky && !g_rope_under_fix.empty();
        if (out->scene_codes && (fx_codes || rope_codes)) {
            static thread_local std::vector<std::uint8_t> codes;
            codes.assign(g_scene_codes, g_scene_codes + static_cast<std::size_t>(w) * h);
            for (const std::vector<UnderFix>* fixes : {&g_fx_under_fix, &g_rope_under_fix}) {
                if (fixes == &g_fx_under_fix ? !fx_codes : !rope_codes) continue;
                for (const UnderFix& f : *fixes)
                    if (f.k < codes.size()) codes[f.k] = static_cast<std::uint8_t>((codes[f.k] & 0x0Fu) | (1u << 4));
            }
            out->scene_codes = codes.data();
        }
        out->blocks = g_blocks.data();
        out->block_count = static_cast<int>(g_blocks.size());
        out->atlas_rgb = g_atlas_rgb.data();
        out->atlas_opaque = g_atlas_opaque.data();
        out->atlas_blur = g_atlas_blur.data();
        out->atlas_w = g_atlas_w;
        out->atlas_h = g_atlas_h;
        out->atlas_per_row = g_atlas_per_row;
    } else {
        out->lift = nullptr;
        out->rope_air = nullptr;
        out->run_split = nullptr;
        out->over_sprite = nullptr;
        out->over_block = nullptr;
        out->lift_copy = nullptr;
        out->lift_ao = nullptr;
        out->under_rgb = nullptr;
        out->scene_codes = nullptr;
        out->block_count = 0;
    }
    out->cursor_xz = g_info.cursor_xz; out->cursor_y = g_info.cursor_y;
    out->fall_card = g_info.fall_card;
    out->fall_ready = g_info.fall_ready;
    out->fall_v[0] = g_info.fall_v[0]; out->fall_v[1] = g_info.fall_v[1];
    out->fall_on = g_info.fall_on;
    for (int i = 0; i < 3; ++i) {
        out->fall_fx[i] = g_info.fall_fx[i]; out->fall_fy[i] = g_info.fall_fy[i];
        out->fall_xz[i] = g_info.fall_xz[i];
    }
    out->fall_y = g_info.fall_y;
    out->spell_card = g_info.spell_card;
    out->spell_fx = g_info.spell_fx; out->spell_fy = g_info.spell_fy;
    out->spell_xz = g_info.spell_xz; out->spell_y = g_info.spell_y;
    out->tile_target = g_tile_target;
    std::copy_n(g_tile_rect, 4, out->tile_rect);
    out->cell_owner = g_tile_target >= 0 && g_cell_owner.size() == static_cast<std::size_t>(w) * h
                          ? g_cell_owner.data() : nullptr;
    out->tile_text = g_tile_target >= 0 ? g_tile_text.c_str() : nullptr;
    return true;
}

void focus_set_enabled(bool on) {
    gbarecomp::g_present_focus_provider = on ? &focus_provider : nullptr;
}
}  // namespace kol
