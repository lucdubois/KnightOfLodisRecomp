// kol_focus_cutouts.cpp — build_scene_now's props passes: which pixels are
// a prop's (prop_base), the cut-outs that stand the props up as cards
// (cutouts), the BG1 shapes on the maps whose BG1 is props (bg1_shapes,
// with their cache) and the spread of the props' places (spread).

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
// The BG1 shapes' work on a map whose BG1 is props (kBg1PropMaps), as the
// last picture with the same inputs left it (asked 2026-10-26 to start
// with props, map 102's opening ~1.2 ms a picture on the Mac, more on the
// Thor): the flood fill of each shape and its feet read the opaque map, the
// scene's layers, the cells' heights, the camera and the sun's way, and
// write each in-scene shape pixel's depth, height, cell and shape (and
// upright, claimed), the pixels past the scene, and the plant cells. Keyed
// on those, taken once a key repeats, replayed on a hit.
// KOL_BG1_CACHE=0 off; KOL_BG1_CACHE_CHECK=1 runs it anyway and compares.
struct Bg1Px {
    std::uint32_t k;
    std::int32_t xz, y;
    int cell, shape;
    bool operator==(const Bg1Px&) const = default;
};
// Eight of them, the least lately used replaced (2026-11-20, Thor state
// 0311: map 102's BG1 art is animated, its shapes change for two pictures
// every sixteen and come round again; with one result kept, and only once
// its key repeated, a fifth of the pictures were worked out). Every result
// is kept: the store is two swaps and a small copy.
struct Bg1Slot {
    std::uint64_t key = 0;
    bool valid = false;
    unsigned long used = 0;
    std::vector<Bg1Px> px;
    std::vector<PropOutPx> out;
    std::vector<int> plants;
};
struct Bg1Cache : CacheStats {
    Bg1Slot slot[8];
    unsigned long clock = 0;
};
Bg1Cache g_bg1_cache;
}  // namespace

namespace kol_focus_detail {
// Props: their own pixels over their cell take the depth of the ground
// they stand on, back to front like the terrain. A prop's pixels are BG1,
// or BG0 where no terrain was drawn (a tree is partly on the ground layer,
// and its tip against the sky would otherwise read as the sky: far). The
// box is only a seed: tree art is wider and taller than its cell, so each
// prop's depth then spreads through the prop pixels connected to it.
bool SceneBuild::prop_pixel(std::size_t k) {
    if (!g_awning.empty() && g_awning[k]) return false;   // an awning's cloth
    if (is_flame(k)) return false;                        // a flame's own card
    const unsigned top = layers[k] & 0x0Fu;
    return top == 1u || (top == 0u && !g_covered[k]);
}

// A drawn cut-out's pixels (kDrawnCutouts, kPlacedCutouts: '#' in its
// rows, its top-left at x0, y0): those in the picture that are BG0 or a
// prop's BG1, each to f(k, py), row by row. It reads the layers, the
// awnings, the flames and what is covered (prop_pixel), none of which
// claiming changes.
template <class F>
void SceneBuild::mask_pixels(const char* const* rows, int count, int x0, int y0, F&& f) {
    for (int r = 0; r < count; ++r)
        for (int c = 0; rows[r][c]; ++c) {
            const int py = y0 + r, px = x0 + c;
            if (rows[r][c] != '#' || py < 0 || py >= h || px < 0 || px >= w) continue;
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            const unsigned lay = layers[k] & 0x0Fu;
            if (lay == 1u ? !prop_pixel(k) : lay != 0u) continue;
            f(k, py);
        }
}

void SceneBuild::flat_cell(std::size_t k, int* cx, int* cy) {   // the cell under a flat point
    const int dxz = 2 * (static_cast<int>(k % w) - ox - kx);
    *cx = static_cast<int>(std::floor((g_xz[k] + dxz) / 64.0f));
    *cy = static_cast<int>(std::floor((g_xz[k] - dxz) / 64.0f));
}

// A prop stands on its base: the middle of its cell's top. Its art is
// above that row, never below, where BG1 is the ground around it: the
// snowfield's raised blocks are BG1, and a dead tree took the block's
// top in front of it and beside its foot into its prop, standing up
// with the tree (states/FH_slot1_siren_icons, the mesh paint). Neither
// the seeds nor the spread go more than kPropBelow rows under the base
// row. KOL_PROP_BELOW=<rows> for A/B (large: as before).
// And BG1 the terrain pass laid on the top of another cell that is no
// prop, level with the prop or in front of it (its diagonal, cx + cy,
// not behind the prop's), is that cell's ground: the raised block
// beside the tree, on its diagonal, stayed the tree's above its base
// row. A prop's art reaches over the cells behind it only.
bool SceneBuild::below_base(std::size_t k, int xz, int y) {
    static const int kPropBelow = kol::env_int("KOL_PROP_BELOW", 2);
    if (static_cast<int>(k / w) > xz / 4 - y / 2 + ky + oy + kPropBelow) return true;
    if (!g_covered[k] || g_kind[k] != kol::kSceneFlat) return false;
    int ocx = 0, ocy = 0;   // the cell under the flat point
    flat_cell(k, &ocx, &ocy);
    if (!in_game(ocx, ocy) || ocx + ocy < xz / 32 - 1) return false;   // off the grid, behind the prop
    bool p = false;
    raw_cell(ocx, ocy, &p);
    return !p;
}

// What the props' passes share: which pixels are a prop's, what is lifted.
void SceneBuild::prop_base() {
    wt.lap("scene.propbase");
    pixels = static_cast<std::size_t>(w) * h;
    // Lifting props (see lift_props_on): the places the props overwrite.
    lift_on = lift_props_on() && cap.perspective && cap.under_ok;
    walls_on = lift_walls_on() && cap.perspective && cap.under_ok;
    if (lift_on) {
        g_pre_xz.assign(g_xz.begin(), g_xz.end());
        g_pre_y.assign(g_y.begin(), g_y.end());
        g_pre_kind.assign(g_kind.begin(), g_kind.end());
    }
}

// The props' pixels: cut-outs (spires, pots, drawn and placed masks) and
// the trees' rule, each prop's seed and its place.
void SceneBuild::cutouts() {
    wt.lap("scene.cutouts");
    g_prop_dist.assign(pixels, kUnreached);
    g_prop_cell.assign(pixels, -1);
    flame_cards(layers, w, h, ox, oy, kx, ky);
    g_queue.clear();
    // Which cut-out holds each pixel it claimed (its cell, -1 none), and each
    // cut-out's art centre on the screen: where two strips overlap (map 6's
    // bushes at 0,17 and 1,17, one behind the other, state GM) a pixel goes to
    // the nearer centre. The later one took the whole overlap: the back bush's
    // right half stood on the front bush's card, cut down a straight line,
    // shaded and swaying with the other. KOL_PERSP_CUT_LATER_WINS=1: as before.
    static thread_local std::vector<int> cut_of;
    cut_of.assign(pixels, -1);
    g_spire_past.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
    g_card_bend.assign(static_cast<std::size_t>(kGridSide) * kGridSide, CardBend{});
    g_pine_tip.assign(static_cast<std::size_t>(kGridSide) * kGridSide, {INT_MIN, 0});
    g_spire_v.assign(pixels, 0);
    g_no_wall_lift.assign(pixels, 0);
    static thread_local std::vector<std::pair<int, int>> cut_centre;
    cut_centre.assign(static_cast<std::size_t>(kGridSide) * kGridSide, {0, 0});
    static const bool later_wins = std::getenv("KOL_PERSP_CUT_LATER_WINS") != nullptr;
    // Cells whose drawn pixels are mostly BG1 (a building on BG1), filled by
    // the first cut-out that asks.
    static thread_local std::vector<std::uint8_t> bg1_cell;
    // With each cell's box on screen (x0, x1, y0, y1, inclusive): a
    // building's colours are looked for only there.
    static thread_local std::vector<std::array<int, 4>> cell_box;
    bool bg1_cells_ready = false;
    // Per cell: its top's (face 0) pixels and those of them on BG1 (a snow
    // overlay), counted on the first spire that needs them (see the spire's
    // strip).
    static thread_local std::vector<int> top_px, top_bg1;
    bool bg1_tops_ready = false;
    // Every cut-out's strip, before any claims: the buildings' colours leave
    // them out (below), claimed yet or not. Map 6's two bushes in front of
    // the house both reach over its cell; the one not yet claimed counted as
    // the house's colours, and the other lost most of its leaves (GP).
    struct Strip { int x0, y0, x1, y1; };
    static thread_local std::vector<Strip> strips;
    strips.clear();
    for (int cy = 0; cy < kGameSide; ++cy)
        for (int cx = 0; cx < kGameSide; ++cx) {
            bool prop = false;
            const int top = raw_cell(cx, cy, &prop);
            if (top < 0 || !prop || !is_spire(cx, cy)) continue;
            const int ground = cell_height(cx, cy);
            if (top <= ground) continue;
            const int sx = 16 * (cx - cy) + kx + ox;
            strips.push_back({sx - 16, 8 * (cx + cy) - 8 * top + ky + oy, sx + 16,
                              8 * (cx + cy) - 8 * ground + ky + oy + 16});
        }
    auto in_a_strip = [&](int px, int py) {
        for (const Strip& t : strips)
            if (px >= t.x0 && px < t.x1 && py >= t.y0 && py < t.y1) return true;
        return false;
    };
    for (int diag = 0; diag <= 2 * (kGameSide - 1); ++diag) {
        for (int cx = std::max(0, diag - kGameSide + 1);
             cx <= std::min(diag, kGameSide - 1); ++cx) {
            const int cy = diag - cx;
            bool prop = false;
            const int top = raw_cell(cx, cy, &prop);
            if (top < 0 || !prop) continue;
            if (is_spire(cx, cy)) {
                // A rock spire is its BG1 pixels (and BG0 the terrain left
                // uncovered) over its cell, from its top to its front
                // corner: the rock's own outline. The wall and ground around
                // it are BG0 and keep the terrain's places, the spire's cell
                // taken at its front ground. No spread: the platform's edge
                // line behind the spire is BG1 too and touches the rock
                // (CY_spire_not_fixed). Taking the whole block the cell
                // would fill carried that background along with the rock.
                const int ground = cell_height(cx, cy);
                if (top <= ground) continue;
                // The card stands at the cell's front corner: the art reaches
                // down to it, and stood at the middle its last 8 rows were
                // under the ground, cut along a straight line by the cell's
                // front edge as the stick lowered the camera (the stone at the
                // snowfield's edge, states/FK_slot1_stone_cutout).
                // KOL_SPIRE_MIDDLE=1: at the middle, as before.
                static const bool all_middle = std::getenv("KOL_SPIRE_MIDDLE") != nullptr;
                const int cut_rec = in_game(cx, cy) ? record_of(ew[e(kGrid) + game_cell(cx, cy)]) : -1;
                bool middle = all_middle, statue_body = false;
                // KOL_PERSP_BUSH_CORNER=1: the bushes (42) at the corner, as
                // before; KOL_PERSP_ROCK_CORNER=1 the rocks (51),
                // KOL_PERSP_POT_CORNER=1 the fire pots (76),
                // KOL_PERSP_STALAGMITE_CORNER=1 the stalagmites (54).
                static const bool bush_corner = std::getenv("KOL_PERSP_BUSH_CORNER") != nullptr;
                static const bool rock_corner = std::getenv("KOL_PERSP_ROCK_CORNER") != nullptr;
                static const bool pot_corner = std::getenv("KOL_PERSP_POT_CORNER") != nullptr;
                static const bool mite_corner = std::getenv("KOL_PERSP_STALAGMITE_CORNER") != nullptr;
                middle = middle || (in_list(kMiddleRecords, cut_rec) && !(bush_corner && cut_rec == 42) &&
                                    !(rock_corner && cut_rec == 51) && !(pot_corner && cut_rec == kPotRecord) &&
                                    !(mite_corner && cut_rec == 54));
                statue_body = statue_body || in_list(kStatueRecords, cut_rec);
                static const bool top_stand_all = std::getenv("KOL_TOP_STAND_LIFT_ALL") != nullptr;
                bool top_stand = in_list(kTopStandRecords, cut_rec) && !top_stand_all;
                if (top_stand) middle = true;
                int spire_depth = middle ? 32 : 64;
                // Sorted past its front neighbours' floor. Placed at the front
                // corner (64) and sorted 32 past that, the card tied with
                // their floor at their front corner, which, drawn after it,
                // covered the rock's art where it reaches over them (IK, the
                // user's slot 3, map 34 12,10: its pieces at the lower left
                // gone, still broken at the prop's lower left).
                // Their floor is never over a spire's ground (it comes from
                // them), so it is behind the rock: the card sorts 32 further
                // (SpriteBlock::sort_extra), its place unchanged (placed
                // nearer it drew larger and uncovered the ground filled in
                // under it). Not when a drawn height puts one over the
                // ground. KOL_SPIRE_CORNER_DEPTH=1: sorted as before.
                static const bool corner_depth = std::getenv("KOL_SPIRE_CORNER_DEPTH") != nullptr;
                if (!middle && !corner_depth && g_spire_past.size() == static_cast<std::size_t>(kGridSide) * kGridSide) {
                    bool p1 = false, p2 = false;
                    const int f1 = ground_cell(cx + 1, cy, &p1), f2 = ground_cell(cx, cy + 1, &p2);
                    g_spire_past[gi(cx, cy)] = f1 <= ground && f2 <= ground;
                }
                const int sx = 16 * (cx - cy) + kx + ox;
                static const bool pot_from_top = std::getenv("KOL_PERSP_POT_FROM_TOP") != nullptr;
                const int art_top = cut_rec == kPotRecord && !pot_from_top ? std::min(top, ground + kPotSteps) : top;
                const int y0 = std::max(0, 8 * (cx + cy) - 8 * art_top + ky + oy);
                // (On top of its block, kTopStandRecords: down to the top's
                // middle row only; the pedestal under it, the block's top's
                // front half and its faces a step down, kept in the scene.)
                const int y1 = std::min(h, 8 * (cx + cy) - 8 * ground + ky + oy + (top_stand ? 8 : 16));
                if (top_stand)
                    for (int py = std::max(0, y1); py < std::min(h, y1 + 16); ++py)
                        for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px)
                            g_no_wall_lift[static_cast<std::size_t>(py) * w + px] = 1;
                // A stone (kFootRecords) stands where its own art meets the
                // floor, as a statue does: the lowest row of its BG1 pixels
                // in the middle of the strip gives the card's depth (the
                // floor's row at depth d is the cell's back row + d / 4). At
                // the front corner, map 2's stones in the rain floated over
                // a strip of ground as the stick lowered the camera
                // (states/HY_slot2_menu_sticks_rictor.state, 3,9... the one
                // by the cliff); at the middle the snowfield's (FK) sank
                // under the floor. Each now goes by its own foot.
                // KOL_STONE_CORNER=1: at the corner, as before.
                static const bool stone_corner = std::getenv("KOL_STONE_CORNER") != nullptr;
                bool foot_rec = in_list(kFootRecords, cut_rec);
                if (foot_rec && !stone_corner && !statue_body) {
                    const int back = 8 * (cx + cy) - 8 * ground + ky + oy;
                    int foot = -1;
                    for (int py = y0; py < y1; ++py)
                        for (int px = std::max(0, sx - 8); px < std::min(w, sx + 8); ++px)
                            if ((layers[static_cast<std::size_t>(py) * w + px] & 0x0Fu) == 1u) { foot = py; break; }
                    if (foot >= 0) spire_depth = std::clamp(4 * (foot + 1 - back), 16, 64);
                }
                const bool v_middle_cell = middle_v_cell(map, cx, cy);
                static const bool v_middle_cliff = std::getenv("KOL_MIDDLE_V_CLIFF") != nullptr;   // the cliff with it
                if (v_middle_cell) spire_depth = 32;
                static const bool no_v_steps = std::getenv("KOL_NO_V_STEPS") != nullptr;
                const VStepCutout* v_step = nullptr;
                if (!no_v_steps)
                    for (const VStepCutout& vs : kVStepCutouts)
                        if (vs.map == map && vs.x == cx && vs.z == cy) v_step = &vs;
                if (v_step) {
                    spire_depth = 32;   // its place the middle, the tip 32 past it: the front corner
                    // Sorted past its front neighbours' floor and its own
                    // walls, as a spire (its art down the +X wall went under
                    // that wall as the camera moved).
                    if (g_spire_past.size() == static_cast<std::size_t>(kGridSide) * kGridSide)
                        g_spire_past[gi(cx, cy)] = true;
                }
                // A statue (kStatueRecords). Its body is BG1: per row only
                // the pixels between its own (BG1) dark outline, a row
                // without both sides taking the last outlined row's (the
                // snow on BG1 around its base stood up with it; the niche's
                // dark BG0 wall beside it widened the rows). Its base's
                // front is BG0 below the body: every BG0 pixel of the last
                // span, widened to the base's own outline, down to the last
                // row with outline or blue in it (the base's right part is
                // light; a triangle was missing, FU, FV, FW). The card stands
                // where that row meets the floor: at the corner the base
                // floated, at the middle its front went under the floor.
                // What the statue hid above its cell's floor is the niche's
                // wall behind it: upright at the cell's back edge (laid flat
                // on the floor, it stretched under the stick, FW).
                const int back_row = 8 * (cx + cy) - 8 * ground + ky + oy;
                auto outline = [](const std::uint8_t* c) { return c[0] + c[1] + c[2] < 200 && c[2] >= c[0]; };   // (33, 49, 66)
                const int self_cut = gi(cx, cy);
                cut_centre[self_cut] = {sx, (y0 + y1) / 2};
                auto claim_px = [&](std::size_t k, int depth) {
                    // On its block's top (kTopStandRecords): nothing under
                    // the cut, whichever rule reaches it (the spread took the
                    // top's right half, BG1, with the pedestal's corner).
                    if (top_stand && static_cast<int>(k / static_cast<std::size_t>(w)) >= y1) return;
                    const int other = cut_of[k];
                    if (!later_wins && other >= 0 && other != self_cut) {
                        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
                        auto d2 = [&](std::pair<int, int> c) {
                            return (px - c.first) * (px - c.first) + (py - c.second) * (py - c.second);
                        };
                        if (d2(cut_centre[other]) < d2(cut_centre[self_cut])) return;
                    }
                    cut_of[k] = self_cut;
                    set_prop_px(k, 32 * (cx + cy) + depth, 16 * ground, self_cut);   // claimed, not spread from
                };
                const DrawnCutout* drawn = drawn_cutout(map, cut_rec, cx, cy);
                if (drawn) {
                    // It stands where its lowest drawn row meets the floor,
                    // as a stone by its foot (the user asked for them to be
                    // anchored at the bottom edge of their lift; at the
                    // front corner the trunk's roots end 5 rows above the
                    // corner's: depth 44, not 64). KOL_DRAWN_CORNER=1: at the corner.
                    static const bool drawn_corner = std::getenv("KOL_DRAWN_CORNER") != nullptr;
                    const int top_y = 8 * (cx + cy) - 8 * art_top + ky + oy;   // y0 before its clamp
                    // Its pixels, gathered once (see mask_pixels), with its
                    // lowest drawn row.
                    static thread_local std::vector<std::size_t> taken;
                    taken.clear();
                    int drawn_low = -1;
                    mask_pixels(drawn->rows, drawn->count, sx + drawn->dx, top_y + drawn->dy, [&](std::size_t k, int py) {
                        drawn_low = py;
                        taken.push_back(k);
                    });
                    int depth = spire_depth;
                    if (!drawn_corner && drawn_low >= 0) depth = std::clamp(4 * (drawn_low + 1 - back_row), 0, 64);
                    // In a V as the strip's stones (kVMiddleRecords): when
                    // its drawing ends on the cell's middle row (within 2),
                    // it stands at the middle, bent along its cell's front
                    // edges, the V under it (its cell's front half) joining
                    // its card (map 85's crystals, JX: asked for a V-shaped
                    // base; the drawn path had skipped the strip's V).
                    bool v_drawn = in_list(kVMiddleRecords, cut_rec);
                    const bool drawn_flat = stalagmite_flat_card() || spire_flat_front();
                    v_drawn = v_drawn && !drawn_flat && !statue_body && spire_depth == 32 && drawn_low >= 0 &&
                              std::abs(drawn_low + 1 - (y1 - 8)) <= 2 &&
                              g_card_bend.size() == static_cast<std::size_t>(kGridSide) * kGridSide;
                    if (v_drawn) depth = spire_depth;
                    for (const std::size_t k : taken) {
                        claim_px(k, depth);
                        if ((layers[k] & 0x0Fu) == 0u) g_spire_v[k] = 1;   // copied, not vacated
                    }
                    if (v_drawn) {
                        g_card_bend[gi(cx, cy)] = {sx, 32};
                        const int vy = y1 - 8;   // the side corners' row
                        for (int py = std::max(0, vy); py < std::min(h, vy + 8); ++py) {
                            const int half2 = 32 - 4 * (py - vy);   // twice the V's half width on this row
                            for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                                if (std::abs(2 * px + 1 - 2 * sx) >= half2) continue;
                                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                                const unsigned lay = layers[k] & 0x0Fu;
                                if (lay > 1u || (lay == 1u && !prop_pixel(k))) continue;
                                if (lay == 0u && !g_awning.empty() && g_awning[k]) continue;
                                // (Its own cell's BG0 only, its top too: as the strip's V.)
                                if (lay == 0u && (g_cell_owner.size() != pixels || g_cell_owner[k] != gi(cx, cy))) continue;
                                claim_px(k, spire_depth);
                                g_spire_v[k] = 1;
                            }
                        }
                    }
                } else if (statue_body && g_scene_rgb) {
                    struct Row { int lo, hi; bool base; };
                    static thread_local std::vector<Row> rows;
                    rows.assign(static_cast<std::size_t>(std::max(0, y1 - y0)), {-1, -1, false});
                    auto rgb_at = [&](int px, int py) { return g_scene_rgb + (static_cast<std::size_t>(py) * w + px) * 3; };
                    auto top_at = [&](int px, int py) { return layers[static_cast<std::size_t>(py) * w + px] & 0x0Fu; };
                    int span_lo = -1, span_hi = -1, bottom = -1;
                    for (int py = y0; py < y1; ++py) {
                        int first = -1, last = -1;
                        bool body = false;
                        for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                            if (top_at(px, py) != 1u) continue;
                            if (outline(rgb_at(px, py))) {
                                if (first < 0) first = px;
                                last = px;
                            }
                        }
                        if (first >= 0 && last > first) { span_lo = first; span_hi = last; }
                        if (span_lo < 0) continue;
                        for (int px = span_lo; px <= span_hi && !body; ++px) body = top_at(px, py) == 1u;
                        Row r{span_lo, span_hi, !body};
                        if (r.base) {
                            // The base's own outline, a few columns out.
                            for (int px = std::max(0, span_lo - 3); px < span_lo; ++px)
                                if (top_at(px, py) == 0u && outline(rgb_at(px, py))) { r.lo = px; break; }
                            for (int px = std::min(w - 1, span_hi + 3); px > span_hi; --px)
                                if (top_at(px, py) == 0u && outline(rgb_at(px, py))) { r.hi = px; break; }
                            bool statue = false;
                            for (int px = r.lo; px <= r.hi && !statue; ++px) {
                                const std::uint8_t* c = rgb_at(px, py);
                                statue = top_at(px, py) == 0u && (outline(c) || c[2] > c[0] + 30);
                            }
                            if (!statue) break;   // past the base
                        }
                        rows[static_cast<std::size_t>(py - y0)] = r;
                        bottom = py;
                    }
                    const int depth = bottom < 0 ? spire_depth : std::clamp(4 * (bottom - back_row), 0, 64);
                    const bool pre = g_pre_xz.size() == pixels && g_pre_kind.size() == pixels;
                    // Under interface (the cursor's floating hand over the
                    // statue) the layer below it in the scene: a hand-shaped
                    // hole went through the lifted statue (FX), as the lifting
                    // takes such pixels (lift_prop_pixels).
                    // (Interface by its OAM priority, 0: g_iface is set later;
                    // a unit in front keeps its own pixels.)
                    auto layer_of = [&](std::size_t k) -> unsigned {
                        const unsigned top = layers[k] & 0x0Fu;
                        if (top == 4u && g_obj_top_valid && g_obj_prio.size() == pixels && g_obj_prio[k] == 0u &&
                            g_scene_codes && (g_scene_codes[k] & 0x0Fu) == kol::kLayerObj)
                            return g_scene_codes[k] >> 4;
                        return top;
                    };
                    for (int py = y0; py <= bottom; ++py) {
                        const Row& r = rows[static_cast<std::size_t>(py - y0)];
                        if (r.lo < 0) continue;
                        for (int px = r.lo; px <= r.hi; ++px) {
                            const std::size_t k = static_cast<std::size_t>(py) * w + px;
                            if (r.base ? layer_of(k) != 0u : !(prop_pixel(k) || layer_of(k) == 1u)) continue;
                            if (pre && py < back_row && g_pre_kind[k] == kol::kSceneFlat) {
                                g_pre_xz[k] = 32 * (cx + cy);
                                g_pre_y[k] = 16 * ground;
                                g_pre_kind[k] = kol::kSceneUpright;
                            }
                            claim_px(k, depth);
                        }
                    }
                } else {
                    // A building beside it: a cell drawn taller than the
                    // cut-out's top, its pixels mostly BG1, with pixels in the
                    // strip (map 6's house beside the bush at 6,8: its corner
                    // stood up on the bush's card, state GL). Its art reaches
                    // over the cells around too, so then every BG1 pixel in
                    // the strip not drawn for the cut-out's own cell goes by
                    // colour, against the cut-out's own cell's BG1 colours and
                    // the buildings' BG1 colours elsewhere (below). Over a taller cell
                    // drawn on BG0 the BG1 is the cut-out's (the ruins' flames
                    // over their wall, CA). KOL_PERSP_SPIRE_TAKES_TALLER=1:
                    // all taken, as before.
                    static const bool takes_taller = std::getenv("KOL_PERSP_SPIRE_TAKES_TALLER") != nullptr;
                    const bool owners = !takes_taller && g_cell_owner.size() == pixels && g_scene_rgb;
                    if (owners && !bg1_cells_ready) {
                        bg1_cells_ready = true;
                        std::vector<int> all(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
                        std::vector<int> on1(all.size(), 0);
                        cell_box.assign(all.size(), {w, -1, h, -1});
                        for (int py = 0; py < h; ++py)
                            for (int px = 0; px < w; ++px) {
                                const std::size_t q = static_cast<std::size_t>(py) * w + px;
                                const int own = g_cell_owner[q];
                                if (own < 0) continue;
                                ++all[own];
                                on1[own] += (layers[q] & 0x0Fu) == 1u;
                                std::array<int, 4>& b = cell_box[own];
                                b[0] = std::min(b[0], px); b[1] = std::max(b[1], px);
                                b[2] = std::min(b[2], py); b[3] = std::max(b[3], py);
                            }
                        bg1_cell.assign(all.size(), 0);
                        for (std::size_t c = 0; c < all.size(); ++c) bg1_cell[c] = all[c] > 0 && 2 * on1[c] > all[c];
                    }
                    const int sx0 = std::max(0, sx - 16), sx1 = std::min(w, sx + 16);
                    const int self = gi(cx, cy);
                    auto rgb_of = [&](std::size_t k) {
                        const std::uint8_t* c = g_scene_rgb + k * 3;
                        return static_cast<std::uint32_t>(c[0] | (c[1] << 8) | (c[2] << 16));
                    };
                    static thread_local std::vector<std::uint32_t> tested, own_rgb, bld_rgb;
                    static thread_local std::vector<int> bld_cells;
                    tested.clear(); own_rgb.clear(); bld_rgb.clear(); bld_cells.clear();
                    for (int py = y0; py < y1 && owners; ++py)
                        for (int px = sx0; px < sx1; ++px) {
                            const std::size_t k = static_cast<std::size_t>(py) * w + px;
                            const int own = g_cell_owner[k];
                            if (own < 0 || own == self || !bg1_cell[own] || !prop_pixel(k) ||
                                cell_height(cell_x(own), cell_y(own)) <= top)
                                continue;
                            if (std::find(bld_cells.begin(), bld_cells.end(), own) == bld_cells.end())
                                bld_cells.push_back(own);
                        }
                    // Not the ground in front of it drawn on BG1: a cell in
                    // front, no higher than its ground, whose top is BG1 (a
                    // snow overlay) is that cell's top there, not the rock's
                    // art, and it stood up with the card at the strip's
                    // lower left, cut square by the strip's edge (IV, the
                    // user's slot 3, map 12's rock at 13,12 took some of the
                    // ground at its front left; IW, 6,17's at its front
                    // right, that cell's top only half BG1). Over a floor on
                    // BG0 (map 34's IK) the BG1 there is the rock's, reaching
                    // over it: a cell's top is BG1 when a quarter of it
                    // outside the strip is.
                    // KOL_SPIRE_TAKES_BG1_FLOOR=1: taken, as before.
                    static const bool takes_floor = std::getenv("KOL_SPIRE_TAKES_BG1_FLOOR") != nullptr;
                    if (!takes_floor && !bg1_tops_ready && g_cell_owner.size() == pixels && g_face.size() == pixels) {
                        bg1_tops_ready = true;
                        top_px.assign(static_cast<std::size_t>(kGridSide) * kGridSide, 0);
                        top_bg1.assign(top_px.size(), 0);
                        for (std::size_t q = 0; q < pixels; ++q) {
                            const int own = g_cell_owner[q];
                            if (own < 0 || g_face[q] != 0) continue;
                            ++top_px[own];
                            top_bg1[own] += (layers[q] & 0x0Fu) == 1u;
                        }
                    }
                    // The front cells' tops inside this strip, to leave out.
                    static thread_local std::vector<std::array<int, 3>> in_strip;   // cell, pixels, on BG1
                    in_strip.clear();
                    if (bg1_tops_ready)
                        for (int py = std::max(0, y0); py < std::min(h, y1); ++py)
                            for (int px = sx0; px < sx1; ++px) {
                                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                                const int own = g_cell_owner[k];
                                if (own < 0 || own == self || g_face[k] != 0) continue;
                                auto it = std::find_if(in_strip.begin(), in_strip.end(),
                                                       [own](const std::array<int, 3>& a) { return a[0] == own; });
                                if (it == in_strip.end()) { in_strip.push_back({own, 0, 0}); it = in_strip.end() - 1; }
                                ++(*it)[1];
                                (*it)[2] += (layers[k] & 0x0Fu) == 1u;
                            }
                    auto front_floor = [&](std::size_t k) {
                        if (!bg1_tops_ready || (layers[k] & 0x0Fu) != 1u || g_face[k] != 0) return false;
                        const int own = g_cell_owner[k];
                        if (own < 0 || own == self || cell_x(own) + cell_y(own) <= cx + cy ||
                            cell_height(cell_x(own), cell_y(own)) > ground)
                            return false;
                        int out = top_px[own], out1 = top_bg1[own];
                        for (const auto& a : in_strip)
                            if (a[0] == own) { out -= a[1]; out1 -= a[2]; }
                        return out > 0 && 4 * out1 >= out;
                    };
                    for (int py = y0; py < y1; ++py)
                        for (int px = sx0; px < sx1; ++px) {
                            const std::size_t k = static_cast<std::size_t>(py) * w + px;
                            if (!prop_pixel(k) || front_floor(k)) continue;
                            // A kMiddleVCells stone's bottom is its V: the
                            // cliff under its front edges (BG1 in the rock's
                            // tiles) stays the cliff's (map 7's 8,9: the rock
                            // took a bit of the cliff on its left and was
                            // not in a V shape).
                            if (v_middle_cell && !v_middle_cliff &&
                                py >= y1 - (std::abs(2 * px + 1 - 2 * sx) + 1) / 4)
                                continue;
                            if (!bld_cells.empty() && (layers[k] & 0x0Fu) == 1u) {
                                if (g_cell_owner[k] == self) {
                                    own_rgb.push_back(rgb_of(k));
                                } else {
                                    tested.push_back(static_cast<std::uint32_t>(k));
                                    continue;
                                }
                            }
                            claim_px(k, spire_depth);
                        }
                    // Its art below the strip, in a V: the rock's BG1 goes
                    // on under the strip's bottom (its ground's front corner
                    // row), over the cliff face below it (IJ, the user's
                    // slot 3, map 34, 1,18, with the Thor's drawn 12: the
                    // rock's lower rows stayed on the wall as its card stood
                    // up). The user asked for it to reach into the dotted zone
                    // under the red square, in a V shape: from the strip's two lower
                    // corners down to a point under its middle along the
                    // cell's diagonals (two across for one down, 8 rows),
                    // the BG1 there joins the card and is lifted with it.
                    // KOL_SPIRE_FLAT_FRONT=1: the straight bottom, as before.
                    // It hangs from the card's own bottom: the trunk at 0,10
                    // (IM, the user's slot 3, asked for the V shape on the
                    // trunk too) ends its BG1 nine rows over the
                    // strip's, its roots going on below on BG0, its cell's
                    // own top; a V from the strip's bottom found only snow.
                    const bool flat_front = spire_flat_front();
                    int vy = y1;   // the row under the card's lowest claimed row
                    {
                        int low = -1;
                        for (int py = std::max(0, y0); py < std::min(h, y1); ++py)
                            for (int px = sx0; px < sx1; ++px)
                                if (cut_of[static_cast<std::size_t>(py) * w + px] == gi(cx, cy)) { low = py; break; }
                        if (low >= 0) vy = std::min(y1, low + 1);
                    }
                    // Its card bent along the V: the V's arms are its cell's
                    // front edges, and the card stood flat at the tip, the
                    // arms in front of the floor they meet (the user asked
                    // for the stone anchored along the whole V, not only
                    // its tip). At the front corner only, where
                    // the tip is. KOL_SPIRE_FLAT_CARD=1: flat, as before.
                    static const bool flat_card = std::getenv("KOL_SPIRE_FLAT_CARD") != nullptr;
                    if (!flat_front && !flat_card && !middle && spire_depth == 64 &&
                        g_card_bend.size() == static_cast<std::size_t>(kGridSide) * kGridSide)
                        g_card_bend[gi(cx, cy)] = {sx, 0};
                    // A stalagmite or a stone (kVMiddleRecords) in its cell's
                    // middle, as a pine (the user's slot 3, map 51's cave, IR:
                    // asked for the spire to get the pines' and bushes'
                    // treatment; map 49's stone, IU): bent to the
                    // front corner, its V from the middle row taking its
                    // cell's own top too.
                    // KOL_STALAGMITE_FLAT_CARD=1: flat, the V its walls only.
                    const bool mite_flat = stalagmite_flat_card();
                    bool v_middle = in_list(kVMiddleRecords, cut_rec);
                    // Only one whose art ends on its middle row (within 2):
                    // bent with its art ending higher, its middle came 32
                    // nearer and over the wall that hid it (map 24's rock
                    // in the water, DR, by the view's bottom edge).
                    v_middle = v_middle && !statue_body && !mite_flat && !flat_front && spire_depth == 32 &&
                               vy < y1 && std::abs(vy - (y1 - 8)) <= 2;
                    if (v_middle_cell) v_middle = true;
                    if (v_middle && g_card_bend.size() == static_cast<std::size_t>(kGridSide) * kGridSide) {
                        g_card_bend[gi(cx, cy)] = {sx, 32};
                        vy = y1 - 8;   // the side corners' row
                    }
                    if (v_step && g_card_bend.size() == static_cast<std::size_t>(kGridSide) * kGridSide)
                        g_card_bend[gi(cx, cy)] = {sx, 32, 16 * (v_step->left - ground), 16 * (v_step->right - ground)};
                    if (v_step) {
                        // Its V: below the side corners' row, down to its
                        // cell's front edges at the arms' floors (the left
                        // at `left` steps, the right `left - right` steps
                        // lower): the card's bottom is the V, as the stones'
                        // (the user found it detached from the tile's
                        // edge). Its BG1 and its own cell's BG0 (its front
                        // walls, copied onto the card).
                        const int fc = 8 * (cx + cy) - 8 * v_step->left + ky + oy + 16;   // the front corner's row
                        const int drop = 8 * (v_step->left - v_step->right);
                        for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                            const int out2 = std::abs(2 * px + 1 - 2 * sx);   // twice the columns out
                            const int edge = fc - (out2 + 1) / 4 + (px >= sx ? drop : 0);
                            for (int py = std::max(0, fc - 8); py < std::min(h, edge); ++py) {
                                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                                const unsigned lay = layers[k] & 0x0Fu;
                                if (lay > 1u) continue;
                                if (lay == 1u && !prop_pixel(k)) continue;
                                if (lay == 0u && (g_cell_owner.size() != pixels || g_cell_owner[k] != gi(cx, cy)))
                                    continue;
                                claim_px(k, spire_depth);
                                g_spire_v[k] = 1;
                            }
                        }
                    }
                    // (Not under one standing on its block's top,
                    // kTopStandRecords: the V there is its pedestal, the
                    // block's; its right half, BG1, went with the card: the
                    // user wanted the right part attached as
                    // well.)
                    if (!flat_front && !v_step && !top_stand)
                        for (int py = std::max(0, vy); py < std::min(h, vy + 8); ++py) {
                            const int half2 = 32 - 4 * (py - vy);   // twice the V's half width on this row
                            for (int px = sx0; px < sx1; ++px) {
                                if (std::abs(2 * px + 1 - 2 * sx) >= half2) continue;
                                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                                // Its BG1, and the BG0 of the cell's front
                                // under it: under the stone at 12,10 (IK) the
                                // V is the cliff's face, BG0, and the card's
                                // bottom stayed straight there (asked for the
                                // triangular bottom, as on the
                                // other stone).
                                const unsigned lay = layers[k] & 0x0Fu;
                                if (lay > 1u) continue;
                                if (lay == 1u && (!prop_pixel(k) || !bld_cells.empty())) continue;
                                // Nor the ground in front of it drawn on BG1,
                                // as in the strip (front_floor): the V's first
                                // row is the strip's width, and under map 19's
                                // rocks it took the front cells' BG1 ground
                                // there, a line standing up under them (state
                                // 109). KOL_V_TAKES_BG1_FLOOR=1: taken, as before.
                                static const bool v_takes_floor = std::getenv("KOL_V_TAKES_BG1_FLOOR") != nullptr;
                                if (lay == 1u && !v_takes_floor && front_floor(k)) continue;
                                if (lay == 0u && !g_awning.empty() && g_awning[k]) continue;
                                // The BG0 only where it is its own cell's:
                                // under a pot or a rock whose front
                                // cells are floor, the V's BG0 is their
                                // floor, and copied upright it stood as a
                                // plank under it (CZ, GK).
                                // Its own top too where it stands at its
                                // front corner (the trunk's roots), not in
                                // the middle (a pot, a bush: their own floor
                                // around them).
                                if (lay == 0u && (g_cell_owner.size() != pixels || g_face.size() != pixels ||
                                                  g_cell_owner[k] != gi(cx, cy) ||
                                                  (g_face[k] == 0 && middle && !v_middle)))
                                    continue;
                                claim_px(k, spire_depth);
                                g_spire_v[k] = 1;
                            }
                        }
                    if (!tested.empty()) {
                        // The building's colours: its cells' BG1 outside the
                        // strip (their BG0 there was the bush's left half;
                        // every tall BG1 cell's took leaf greens from
                        // elsewhere, and the church's bush lost its leaves),
                        // not what another cut-out claimed there (map 6's back
                        // bush over the house's cell: its greens counted as
                        // the house's, and the front bush lost its lower
                        // half, GP).
                        // (Over the buildings' cells' boxes only: the
                        // colours are sorted and made unique below, so the
                        // order they are met in does not matter.)
                        int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
                        for (const int c : bld_cells) {
                            const std::array<int, 4>& b = cell_box[static_cast<std::size_t>(c)];
                            bx0 = std::min(bx0, b[0]); bx1 = std::max(bx1, b[1]);
                            by0 = std::min(by0, b[2]); by1 = std::max(by1, b[3]);
                        }
                        for (int py = by0; py <= by1; ++py)
                            for (int px = bx0; px <= bx1; ++px) {
                                const std::size_t q = static_cast<std::size_t>(py) * w + px;
                                const int own = g_cell_owner[q];
                                if (own < 0 || (layers[q] & 0x0Fu) != 1u || cut_of[q] >= 0 ||
                                    std::find(bld_cells.begin(), bld_cells.end(), own) == bld_cells.end())
                                    continue;
                                if ((px >= sx0 && px < sx1 && py >= y0 && py < y1) || in_a_strip(px, py)) continue;
                                bld_rgb.push_back(rgb_of(q));
                            }
                        for (auto* v : {&own_rgb, &bld_rgb}) {
                            std::sort(v->begin(), v->end());
                            v->erase(std::unique(v->begin(), v->end()), v->end());
                        }
                        auto has = [](const std::vector<std::uint32_t>& v, std::uint32_t c) {
                            return std::binary_search(v.begin(), v.end(), c);
                        };
                        auto claimed = [&](std::size_t m) {
                            return g_prop_dist[m] == 0 && g_xz[m] == 32 * (cx + cy) + spire_depth &&
                                   g_y[m] == 16 * ground;
                        };
                        auto claimed_around = [&](std::uint32_t k) {
                            const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
                            int n = 0;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const int nx = px + dx, ny = py + dy;
                                    n += (dx || dy) && nx >= 0 && ny >= 0 && nx < w && ny < h &&
                                         claimed(static_cast<std::size_t>(ny) * w + nx);
                                }
                            return n;
                        };
                        auto beside_claimed = [&](std::uint32_t k) { return claimed_around(k) > 0; };
                        // Drawn for another cell: the building's colours only
                        // beside what the cut-out took (its outline), the rest
                        // the cut-out's. Drawn for the building's own cell: the
                        // cut-out's colours reached from what it holds, and a
                        // colour neither has where at least half its neighbours
                        // are the cut-out's (the bush's dark inside: the leaves
                        // over the church, GK, came only so; the house's window
                        // beside the bush touches it along one edge, GL).
                        std::vector<std::uint32_t> shared, reach, inside;
                        for (std::uint32_t k : tested) {
                            const std::uint32_t c = rgb_of(k);
                            const bool mine = has(own_rgb, c), theirs = has(bld_rgb, c);
                            const bool bld = std::find(bld_cells.begin(), bld_cells.end(), g_cell_owner[k]) != bld_cells.end();
                            if (bld) {
                                if (mine && theirs) shared.push_back(k);
                                else if (mine) reach.push_back(k);
                                else if (!theirs) inside.push_back(k);
                            } else if (theirs) {
                                shared.push_back(k);
                            } else {
                                claim_px(k, spire_depth);
                            }
                        }
                        for (bool grew = true; grew;) {
                            grew = false;
                            for (std::uint32_t& k : reach)
                                if (k != UINT32_MAX && beside_claimed(k)) {
                                    claim_px(k, spire_depth);
                                    k = UINT32_MAX;
                                    grew = true;
                                }
                            for (std::uint32_t& k : inside)
                                if (k != UINT32_MAX && claimed_around(k) >= 4) {
                                    claim_px(k, spire_depth);
                                    k = UINT32_MAX;
                                    grew = true;
                                }
                        }
                        // Then the shared ones beside what it took, all at once
                        // (one after another, the house's outline came along
                        // pixel by pixel).
                        std::vector<std::uint32_t> take;
                        for (std::uint32_t k : shared)
                            if (beside_claimed(k)) take.push_back(k);
                        for (std::uint32_t k : take) claim_px(k, spire_depth);
                    }
                }
                // A pedestal under the body: its BG0 pixels in the strip,
                // but not those of a cell in front that stands above the
                // cut-out's ground (the pool's front rim, drawn over the
                // vase's foot). Without the cells' owners the
                // body goes alone.
                const int rec = record_of(ew[e(kGrid) + game_cell(cx, cy)]);
                for (const Pedestal& pd : kPedestals) {
                    if (pd.record != rec || g_cell_owner.size() != pixels) continue;
                    const int py0 = std::max(0, 8 * (cx + cy) + 8 - 8 * (top - pd.steps) + ky + oy);
                    // The floor beside the strip, at the cut-out's ground: its
                    // commonest colour is the water around the vase, which
                    // stays the pool's (taken, it kept the prop's own colour
                    // and showed as a light band beside the vase).
                    std::uint32_t floor_rgb = 0xFFFFFFFFu;
                    if (g_scene_rgb) {
                        std::vector<std::pair<std::uint32_t, int>> seen;
                        for (int py = py0; py < y1; ++py)
                            for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                                if (px >= sx - pd.half && px <= sx + pd.half) continue;
                                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                                if (g_kind[k] != kol::kSceneFlat || g_y[k] != 16 * ground) continue;
                                const std::uint8_t* c = g_scene_rgb + k * 3;
                                const std::uint32_t v = c[0] | (c[1] << 8) | (c[2] << 16);
                                auto it = std::find_if(seen.begin(), seen.end(), [v](const auto& p) { return p.first == v; });
                                if (it == seen.end()) seen.push_back({v, 1}); else ++it->second;
                            }
                        int best = 0;
                        for (const auto& p : seen) if (p.second > best) { best = p.second; floor_rgb = p.first; }
                    }
                    for (int py = py0; py < y1; ++py)
                        for (int px = std::max(0, sx - pd.half); px <= std::min(w - 1, sx + pd.half); ++px) {
                            const std::size_t k = static_cast<std::size_t>(py) * w + px;
                            if ((layers[k] & 0x0Fu) != 0u || g_prop_dist[k] == 0) continue;
                            if (g_scene_rgb) {
                                const std::uint8_t* c = g_scene_rgb + k * 3;
                                if ((c[0] | (c[1] << 8) | (c[2] << 16)) == floor_rgb) continue;
                            }
                            const int own = g_cell_owner[k];
                            if (own >= 0 && cell_x(own) + cell_y(own) > cx + cy &&
                                cell_height(cell_x(own), cell_y(own)) > ground)
                                continue;
                            if (pd.copy) {
                                if (g_spire_v.size() != pixels) continue;
                                g_spire_v[k] = 1;
                                g_prop_dist[k] = 0;
                                g_prop_cell[k] = gi(cx, cy);
                                continue;
                            }
                            set_prop_px(k, 32 * (cx + cy) + spire_depth, 16 * ground, gi(cx, cy));   // with the body
                        }
                }
                continue;
            }
            const int ground = cell_height(cx, cy);
            // A prop rises above the ground it stands on. The same terrain
            // types are trees on the snowfield and plain block tops on the
            // ruins (0x07, stored height = ground: states/BZ_tile_piece2),
            // whose BG1 pieces of stone stood up as a tree.
            if (top <= ground) continue;
            const int prop_xz = 32 * (cx + cy) + 32;
            const int prop_y = 16 * ground;
            const int sx = 16 * (cx - cy) + kx + ox;
            // A bush or a pine (kVPlantRecords) bent along its cell's front
            // edges, its middle at the front corner, as a spire's V (the
            // user's slot 3, map 34's bushes, IN, asked to anchor them in a V
            // shape, then the same for the pines): standing flat in the
            // middle, its art over the front half of the cell stood over the
            // floor there. KOL_BUSH_FLAT_CARD=1: flat.
            // Its art ends on the cell's middle row (the side corners'), so
            // the front half of its cell's top under it, down to the front
            // edges, goes up with it, as asked: following the tile's edge, the
            // flat bottom part of the lift (bent with
            // its art only, it floated over a gap there).
            static const bool bush_flat = std::getenv("KOL_BUSH_FLAT_CARD") != nullptr;
            const int rec = in_game(cx, cy) ? record_of(ew[e(kGrid) + game_cell(cx, cy)]) : -1;
            bool bush_v = false;
            if (!bush_flat && g_card_bend.size() == static_cast<std::size_t>(kGridSide) * kGridSide) {
                bush_v = bush_v || in_list(kVPlantRecords, rec);
                if (bush_v) g_card_bend[gi(cx, cy)] = {sx, 32};
            }
            // From the prop's stored top down to the cell's front corner. Art
            // taller than that is reached by the spread below, through the
            // prop's own connected pixels. (A 16-row margin above used to
            // seed any BG1 there too, and BG1 also carries pieces of the
            // ground -- a tile's corner drawn over a unit behind it: the one
            // on the tile behind a prop stood up with the prop, and the right
            // stick's look pulled it off its tile, states/BY_tile_sprites.)
            const int y0 = 8 * (cx + cy) - 8 * top + ky + oy;
            const int y1 = 8 * (cx + cy) - 8 * ground + ky + oy + 16;
            // Drawn by the user (kDrawnCutouts): exactly those pixels, BG1
            // lifted and BG0 copied, standing where its lowest drawn row
            // meets the floor (as the trunk's), not spread from.
            if (const DrawnCutout* drawn = drawn_cutout(map, rec, cx, cy)) {
                const int back = 8 * (cx + cy) - 8 * ground + ky + oy;
                const int mx0 = sx + drawn->dx, my0 = y0 + drawn->dy;
                int foot = -1;   // its lowest taken row
                mask_pixels(drawn->rows, drawn->count, mx0, my0, [&](std::size_t, int py) { foot = py; });
                const int depth = foot >= 0 ? std::clamp(4 * (foot + 1 - back), 0, 64) : 32;
                mask_pixels(drawn->rows, drawn->count, mx0, my0, [&](std::size_t k, int) {
                    set_prop_px(k, 32 * (cx + cy) + depth, prop_y, gi(cx, cy));
                    if ((layers[k] & 0x0Fu) == 0u && g_spire_v.size() == pixels) g_spire_v[k] = 1;
                });
                continue;
            }
            // Two pines whose strips overlap (one behind the other, a cell
            // apart): the later cell took every pixel of both there, half
            // the back pine on the front one's card, and swaying it split
            // down its middle (IO, the user's slot 3, map 35's 10,15 and
            // 10,16: the left part split from the right, visible as they
            // sway). A pine is a triangle from its tip,
            // half as wide as it is tall below it (+1, measured on IO): the
            // front pine, drawn over the other, takes its own triangle, the
            // back one the rest of its own. KOL_PINE_LATER_TAKES=1: as before.
            static const bool later_takes = std::getenv("KOL_PINE_LATER_TAKES") != nullptr;
            const bool pine = !later_takes && rec == kPineRecord &&
                              g_pine_tip.size() == static_cast<std::size_t>(kGridSide) * kGridSide;
            if (pine) g_pine_tip[gi(cx, cy)] = {sx, y0};
            auto in_pine = [](std::pair<int, int> t, int px, int py) {
                return py >= t.second && std::abs(2 * px + 1 - 2 * t.first) <= py - t.second + 3;
            };
            // A pine's art is its triangle, all inside its strip: only that
            // is seeded, and not spread from. The BG1 wall of a taller cell
            // behind it touched its art, and the spread took the whole wall
            // onto its card, swaying with it (IQ, the user's slot 3, map 35's
            // 2,12 by the stone wall: the wall swayed with the tree).
            // KOL_PINE_SPREAD=1: its whole strip, spread from, as before.
            static const bool pine_spread = std::getenv("KOL_PINE_SPREAD") != nullptr;
            const bool pine_own = rec == kPineRecord && !pine_spread;
            for (int py = std::max(0, y0); py < std::min(h, y1); ++py) {
                for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if (!prop_pixel(k) || below_base(k, prop_xz, prop_y)) continue;
                    if (pine_own && !in_pine({sx, y0}, px, py)) continue;
                    if (pine && g_prop_dist[k] == 0 && g_prop_cell[k] >= 0 && g_prop_cell[k] != gi(cx, cy) &&
                        g_pine_tip[g_prop_cell[k]].first != INT_MIN) {
                        const int o = g_prop_cell[k];
                        const bool front = cx + cy >= cell_x(o) + cell_y(o);
                        const bool in_me = in_pine({sx, y0}, px, py), in_other = in_pine(g_pine_tip[o], px, py);
                        if (front ? !in_me && in_other : !in_me || in_other) continue;
                    }
                    if (g_prop_dist[k] == kUnreached && !pine_own) g_queue.push_back(static_cast<std::uint32_t>(k));
                    set_prop_px(k, prop_xz, prop_y, gi(cx, cy));
                }
            }
            if (pine && g_cell_owner.size() == pixels && g_spire_v.size() == pixels) {
                // Its BG0 too: on map 35's cliff edge (IP, the user's slot 3,
                // 11,0 and 12,0, the same asked for the state 3 trees) the game draws
                // half of each pine on BG0, which stayed flat as the BG1 half
                // bent and swayed. Its triangle's BG0, copied (under BG0 is
                // the backdrop), not a nearer cell's (a wall in front of it),
                // shared with another pine as its BG1 is. KOL_PINE_NO_BG0=1:
                // its BG1 only, as before.
                // Only a split pine, its BG1 (nearly) all on one side of its
                // middle, and only on the other side, inside its edge: a pine
                // all on BG1 (IO's) took the water around it as a blue fringe.
                static const bool pine_no_bg0 = std::getenv("KOL_PINE_NO_BG0") != nullptr;
                auto in_edge = [&](int px, int py) {
                    return py >= y0 && std::abs(2 * px + 1 - 2 * sx) <= py - y0 + 2;
                };
                int left = 0, right = 0;
                for (int py = std::max(0, y0); py < std::min(h, y1 - 8) && !pine_no_bg0; ++py)
                    for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px)
                        if ((layers[static_cast<std::size_t>(py) * w + px] & 0x0Fu) == 1u && in_edge(px, py))
                            ++(px < sx ? left : right);
                const int side = 4 * right < left ? 1 : 4 * left < right ? -1 : 0;   // the BG0 side
                for (int py = std::max(0, y0); py < std::min(h, y1 - 8) && side != 0; ++py)
                    for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                        const std::size_t k = static_cast<std::size_t>(py) * w + px;
                        if ((side > 0) != (px >= sx)) continue;
                        if ((layers[k] & 0x0Fu) != 0u || !in_edge(px, py)) continue;
                        const int own = g_cell_owner[k];
                        if (own >= 0 && cell_x(own) + cell_y(own) > cx + cy) continue;
                        if (g_prop_dist[k] == 0 && g_prop_cell[k] >= 0 && g_prop_cell[k] != gi(cx, cy)) {
                            const int o = g_prop_cell[k];
                            if (g_pine_tip[o].first == INT_MIN) continue;   // another prop's
                            const bool front = cx + cy >= cell_x(o) + cell_y(o);
                            if (front ? in_pine(g_pine_tip[o], px, py) && !in_pine({sx, y0}, px, py)
                                      : in_pine(g_pine_tip[o], px, py))
                                continue;
                        }
                        set_prop_px(k, prop_xz, prop_y, gi(cx, cy));
                        g_spire_v[k] = 1;
                    }
            }
            if (bush_v && g_cell_owner.size() == pixels && g_face.size() == pixels) {
                // The top's front half: its BG1 lifted, its BG0 copied (kept in
                // the scene too, as a spire's V: lifted, BG0 leaves the
                // backdrop). Not spread from.
                const int mid = y1 - 8;   // the side corners' row
                for (int py = std::max(0, mid); py < std::min(h, y1); ++py) {
                    const int half2 = 32 - 4 * (py - mid);
                    for (int px = std::max(0, sx - 16); px < std::min(w, sx + 16); ++px) {
                        if (std::abs(2 * px + 1 - 2 * sx) >= half2) continue;
                        const std::size_t k = static_cast<std::size_t>(py) * w + px;
                        const unsigned lay = layers[k] & 0x0Fu;
                        if (g_cell_owner[k] != gi(cx, cy) || g_face[k] != 0) continue;
                        if (lay == 1u ? !prop_pixel(k) : lay != 0u) continue;
                        set_prop_px(k, prop_xz, prop_y, gi(cx, cy));
                        if (lay == 0u && g_spire_v.size() == pixels) g_spire_v[k] = 1;
                    }
                }
            }
        }
    }
    // Cut-outs placed by their art (kPlacedCutouts): the drawing's pixels,
    // BG1 lifted and BG0 copied (kept in the scene, as a trunk's roots), on
    // one card standing where its lowest taken row meets the floor.
    static const bool no_placed = std::getenv("KOL_NO_PLACED_CUTOUTS") != nullptr;
    if (!no_placed && g_spire_v.size() == pixels) {
        int index = 0;
        for (const PlacedCutout& pc : kPlacedCutouts) {
            if (pc.map != map) continue;
            const int x0 = pc.ax + kx + ox, y0 = pc.ay + ky + oy;
            int foot = -1;   // its lowest taken row
            mask_pixels(pc.rows, pc.count, x0, y0, [&](std::size_t, int py) { foot = py; });
            ++index;
            if (foot < 0) continue;
            const int xz = 4 * (foot + 1 - ky - oy + 8 * pc.ground);
            mask_pixels(pc.rows, pc.count, x0, y0, [&](std::size_t k, int) {
                set_prop_px(k, xz, 16 * pc.ground, -100 - index);   // no cell: its own group
                if ((layers[k] & 0x0Fu) == 0u) g_spire_v[k] = 1;
            });
        }
    }
    // A map whose BG1 is props (kBg1PropMaps): each BG1 shape (8-connected)
    // stands where its foot meets the ground, its pixels all on one card,
    // as a drawn cut-out stands by its lowest row. The foot is its lowest
    // row; the ground there is the first height, from the top, whose cell
    // under that point is at it (the drawn heights, past the grid too).
    // A ledge's top triangle (at most 12 rows, every row solid, widest at
    // its foot) stays ground. Rocks (solid, a narrow top) do not sway.
    // A shape goes on past the scene's edge (and into rows and columns the
    // PPU was told to leave out) through the guest's composed map, BG1 as
    // drawn (the user's repainted tiles too) with the captured tiles: whole,
    // its feet and its shadow do not change as the camera pans it in. Only
    // the part with the scene's own pixels, its card, is drawn. Cut at the
    // scene's top, the bushes and trees on map 102's upper ledges cast the
    // shadow of what showed, which grew and slid at every step of the slow
    // pan in the narration (JI, the user's slot 2: the top section's props
    // had moving or flickering shadows). KOL_BG1_PROPS_CUT=1:
    // the scene's pixels alone, as before.
    g_bg1_shape.clear();
    g_prop_out.clear();
}

// On kBg1PropMaps, every BG1 shape a prop at its foot (cached by place).
void SceneBuild::bg1_shapes() {
    wt.lap("scene.bg1_shapes");
    if (bg1_props) {
        g_bg1_shape.assign(pixels, -1);
        constexpr int M = 64;   // how far past the scene a shape is followed (map 102's trees: under 60 tall)
        const int W2 = w + 2 * M, H2 = h + 2 * M;
        static thread_local std::vector<std::uint8_t> seen;
        static thread_local std::vector<std::uint32_t> shape, stack;   // padded indices
        auto pad = [&](int px, int py) { return static_cast<std::uint32_t>((py + M) * W2 + px + M); };
        auto pad_x = [&](std::uint32_t q) { return static_cast<int>(q % W2) - M; };
        auto pad_y = [&](std::uint32_t q) { return static_cast<int>(q / W2) - M; };
        // The composed map's BG1 past the scene (see flame_cards for the
        // mapping and the frame hook's lead).
        static const bool cut = std::getenv("KOL_BG1_PROPS_CUT") != nullptr;
        const Capture& cap = g_cap;
        const kol::MapExtent& ext = cap.extent;
        const int lead_x = cap.extent_ok ? scroll_lead(static_cast<int>(cap.bg0_h), ext.hofs) : 0;
        const int lead_y = cap.extent_ok ? scroll_lead(static_cast<int>(cap.bg0_v), ext.vofs) : 0;
        const bool past = !cut && cap.extent_ok && cap.bg0_ok && cap.bg1_chars.size() >= 32u &&
                          std::abs(lead_x) <= 16 && std::abs(lead_y) <= 16;
        const int ax0 = ext.origin_x + lead_x - ox, ay0 = ext.origin_y + lead_y - oy;   // array = scene + a0
        auto map_bg1 = [&](int px, int py) {   // the composed map's BG1 is opaque there
            const int ax = px + ax0, ay = py + ay0;
            if (ax < 0 || ay < 0) return false;
            const int c = ax >> 3, r = ay >> 3;
            if (c >= kol::MapExtent::kCols || r >= ext.rows) return false;
            const std::uint16_t e = ext.bg1_drawn[r * kol::MapExtent::kCols + c];
            if (e == 0) return false;
            const int tx = (e & 0x400u) ? 7 - (ax & 7) : (ax & 7), ty = (e & 0x800u) ? 7 - (ay & 7) : (ay & 7);
            const std::size_t at = (e & 0x3FFu) * 32u + static_cast<std::size_t>(ty) * 4u + static_cast<std::size_t>(tx >> 1);
            if (at >= cap.bg1_chars.size()) return false;
            return ((tx & 1) ? cap.bg1_chars[at] >> 4 : cap.bg1_chars[at] & 15u) != 0u;
        };
        // A shape's pixel: the scene's own BG1 prop pixels, and past the
        // scene the composed map's; also where interface or a sprite covers
        // the map (the narration's letterbox bar, BG3 over rows 23-30: the
        // shapes under it were cut along the bar, fixed on the screen, and
        // their shadows still slid as the art moved through it).
        // A layer blended over the picture as light (Capture::hud_glow_layers:
        // Shaher's attack, a BG2 disc over his body, map 57, states/thor/0235)
        // is not interface: the BG1 under it is the shape's own, on its card.
        // Left out, his chest stayed on the ground (flat, blurred as the
        // floor) while the rest of him moved with the combat camera (the
        // user, 2026-11-12, reported Shaher's body split, part of it out of
        // focus). KOL_BG1_PROPS_UNDER_LIGHT=0: as before.
        static const bool under_light = kol::env_not_zero("KOL_BG1_PROPS_UNDER_LIGHT");
        const unsigned light = under_light ? (cap.hud_glow_layers & 0x0Cu) : 0u;
        auto bg1_under_light = [&](std::uint8_t c) {
            const unsigned top = c & 0x0Fu;
            return ((light >> top) & 1u) != 0u && (c >> 4) == 1u;
        };
        auto in_scene = [&](int px, int py) {
            if (px < 0 || py < 0 || px >= w || py >= h) return false;
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            if (layers[k] == gba::kLayerNotComposed) return false;
            const unsigned top = layers[k] & 0x0Fu;   // the frame's: the HUD over it too
            return top <= 1u || top == 5u || bg1_under_light(layers[k]);
        };
        wt.lap("bg1.setup");
        // The key of everything the opaque map below and in_scene read
        // (2026-11-20, Thor state 0311: the map was made every picture, 0.46
        // ms, only to be hashed into the cache's key; and that key took the
        // frame's layer codes whole, so every letter the narration typed
        // missed, a fifth of the pictures). Of the layer codes, only what is
        // asked of them: not composed, BG1 on top, BG1 under a light layer,
        // the map showing (in_scene). The map is made again only when this
        // key changes; the cache below is keyed on it too.
        // KOL_BG1_CACHE_CHECK=1 also makes the map every picture and compares.
        std::uint64_t ok = 1469598103934665603ull;
        {
            auto omix = [&ok](std::uint64_t v) { kol::fnv64x_mix(ok, v); };
            auto omix_bytes = [&](const void* data, std::size_t n) {
                const auto* p8 = static_cast<const std::uint8_t*>(data);
                std::size_t i = 0;
                for (; i + 8 <= n; i += 8) {
                    std::uint64_t v;
                    std::memcpy(&v, p8 + i, 8);
                    omix(v);
                }
                std::uint64_t v = 0;
                for (; i < n; ++i) v = v << 8 | p8[i];
                omix(v ^ static_cast<std::uint64_t>(n) << 40);
            };
            for (int v : {w, h, M, ax0, ay0, past ? 1 : 0, static_cast<int>(light), past ? ext.rows : 0})
                omix(static_cast<std::uint64_t>(v + 65536));
            std::uint8_t cls[256];
            for (int c = 0; c < 256; ++c) {
                const unsigned top = static_cast<unsigned>(c) & 0x0Fu;
                const bool under = bg1_under_light(static_cast<std::uint8_t>(c));
                cls[c] = c == gba::kLayerNotComposed
                             ? 8u
                             : static_cast<std::uint8_t>((top == 1u ? 1u : 0u) | (under ? 2u : 0u) |
                                                         (top <= 1u || top == 5u || under ? 4u : 0u));
            }
            const std::size_t n = static_cast<std::size_t>(w) * h;
            std::size_t i = 0;
            for (; i + 8 <= n; i += 8) {
                std::uint64_t v = 0;
                for (int j = 0; j < 8; ++j) v = v << 8 | cls[layers[i + j]];
                omix(v);
            }
            for (; i < n; ++i) omix(cls[layers[i]]);
            omix(g_awning.size());
            if (!g_awning.empty()) omix_bytes(g_awning.data(), g_awning.size());
            omix(g_flame.size());
            if (!g_flame.empty()) omix_bytes(g_flame.data(), g_flame.size());
            if (past) {
                // The composed map's entries by their tile and flips (not their
                // palette), the tiles by which texels are opaque (not their
                // colours: a tile that animates keeps its shape, and whole,
                // the key missed three pictures in four).
                const std::size_t entries = static_cast<std::size_t>(ext.rows) * kol::MapExtent::kCols;
                for (std::size_t e = 0; e + 4 <= entries; e += 4) {
                    std::uint64_t v;
                    std::memcpy(&v, ext.bg1_drawn + e, 8);
                    omix(v & 0x0FFF0FFF0FFF0FFFull);
                }
                const std::size_t cn = cap.bg1_chars.size();
                omix(cn);
                std::size_t c = 0;
                for (; c + 8 <= cn; c += 8) {
                    std::uint64_t v;
                    std::memcpy(&v, cap.bg1_chars.data() + c, 8);
                    v |= v >> 1;
                    v |= v >> 2;
                    omix(v & 0x1111111111111111ull);   // a bit a texel: not colour 0
                }
                for (; c < cn; ++c) omix((cap.bg1_chars[c] & 0x0Fu ? 1u : 0u) | (cap.bg1_chars[c] & 0xF0u ? 2u : 0u));
            }
        }
        // (Worked out once a pixel: asked by each neighbour in the fill,
        // it was a quarter of the worker's time.)
        static thread_local std::vector<std::uint8_t> opaque, opaque_was;
        static thread_local std::uint64_t opaque_key = 0;
        static thread_local bool opaque_valid = false;
        static const bool opaque_check = kol::env_true("KOL_BG1_CACHE_CHECK");
        // (Made when the fill below runs: a kept result needs none of it.)
        auto make_opaque = [&]() {
        const bool opaque_kept = opaque_valid && opaque_key == ok && opaque.size() == static_cast<std::size_t>(W2) * H2;
        if (opaque_kept && opaque_check) opaque_was = opaque;
        if (!opaque_kept || opaque_check) {
        opaque.assign(static_cast<std::size_t>(W2) * H2, 0);
        // A tile's row of the composed map's BG1, 8 pixels as bits (bit i:
        // the tile's column i on screen).
        auto tile_row = [&](std::uint16_t e, int ty) -> unsigned {
            if (e == 0) return 0u;
            const int row = (e & 0x800u) ? 7 - ty : ty;
            const std::size_t at = (e & 0x3FFu) * 32u + static_cast<std::size_t>(row) * 4u;
            if (at + 4 > cap.bg1_chars.size()) return 0u;
            unsigned bits = 0;
            for (int i = 0; i < 8; ++i) {
                const int tx = (e & 0x400u) ? 7 - i : i;
                const std::uint8_t b = cap.bg1_chars[at + static_cast<std::size_t>(tx >> 1)];
                if (((tx & 1) ? b >> 4 : b & 15u) != 0u) bits |= 1u << i;
            }
            return bits;
        };
        for (int py = -M; py < h + M; ++py) {
            const bool row_in = py >= 0 && py < h;
            std::uint8_t* orow = &opaque[pad(-M, py)];
            if (row_in) {   // the scene's own pixels: BG1 on top, a prop's
                const std::uint8_t* lrow = layers + static_cast<std::size_t>(py) * w;
                for (int px = 0; px < w; ++px) {
                    const std::uint8_t c = lrow[px];
                    if (c == gba::kLayerNotComposed) continue;
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if ((c & 0x0Fu) == 1u ? prop_pixel(k) : bg1_under_light(c) && !is_flame(k) &&
                                                             (g_awning.empty() || !g_awning[k]))
                        orow[px + M] = 1;
                }
            }
            if (!past) continue;
            // Past the scene, and where the scene does not show the map
            // (not composed, or interface or a sprite over it): the map's.
            const int ay = py + ay0;
            if (ay < 0 || (ay >> 3) >= ext.rows) continue;
            const std::uint16_t* erow = ext.bg1_drawn + (ay >> 3) * kol::MapExtent::kCols;
            for (int px = -M; px < w + M;) {
                const int ax = px + ax0;
                const int span = 8 - (ax & 7);   // to this tile's end
                if (ax < 0 || (ax >> 3) >= kol::MapExtent::kCols) { px += span; continue; }
                const unsigned bits = tile_row(erow[ax >> 3], ay & 7) >> (ax & 7);
                for (int i = 0; i < span && px + i < w + M; ++i) {
                    if (!((bits >> i) & 1u)) continue;
                    const int x = px + i;
                    if (row_in && x >= 0 && x < w && in_scene(x, py)) continue;   // (the scene's own, above)
                    orow[x + M] = 1;
                }
                px += span;
            }
        }
        if (opaque_kept && opaque_check && opaque != opaque_was)
            std::fprintf(stderr, "kol-focus: BG1 OPAQUE MAP KEPT BUT DIFFERENT\n");
        opaque_key = ok;
        opaque_valid = true;
        }
        };
        auto shape_px = [&](int px, int py) {
            return px >= -M && py >= -M && px < w + M && py < h + M && opaque[pad(px, py)] != 0;
        };
        static const bool shape_log = std::getenv("KOL_BG1_PROPS_LOG") != nullptr;
        if (shape_log && past) {
            // The mapping checked on the scene: its BG1 pixels against the
            // composed map's.
            long both = 0, scene_only = 0;
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if ((layers[k] & 0x0Fu) != 1u) continue;
                    (map_bg1(px, py) ? both : scene_only) += 1;
                }
            std::fprintf(stderr, "bg1_shape: map agrees on %ld of %ld scene BG1 pixels (lead %d,%d)\n", both,
                         both + scene_only, lead_x, lead_y);
        }
        wt.lap("bg1.fill");
        // Every cell's top middle on the scene, where it has a height.
        struct Foot { int sx, sy, cx, cz, h, xz, low = 0; };   // low: the shape's lowest row there
        static thread_local std::vector<Foot> middles;
        middles.clear();
        for (int cz = -kMargin; cz < kGameSide + kMargin; ++cz)
            for (int cx = -kMargin; cx < kGameSide + kMargin; ++cx) {
                const int msx = 16 * (cx - cz) + kx + ox;
                if (msx < -M - 8 || msx >= w + M + 8) continue;   // (before its height: most cells are off the scene)
                const int msy0 = 8 * (cx + cz) + 8 + ky + oy;     // its row at height 0; heights only raise it
                if (msy0 < -M - 8) continue;
                // (The frame's heights where worked out: cell_height on
                // every cell here cost the worker a millisecond.)
                const bool known = heights_ready && cx >= field_x0 && cx <= field_x1 && cz >= field_z0 && cz <= field_z1;
                const int ch = known ? heights[gi(cx, cz)] : cell_height(cx, cz);
                if (ch < 0) continue;
                const int msy = msy0 - 8 * ch;
                if (msy < -M - 8 || msy >= h + M + 8) continue;
                middles.push_back({msx, msy, cx, cz, ch, 32 * (cx + cz) + 32});
            }
        wt.lap("bg1.middles");
        // The middles whose sx lies in [lo, hi], in their own order (the
        // feet's de-dup and the fallback's first nearest go by it), through
        // an index sorted by sx, made at the first shape that asks: every
        // shape scanned all of them (a few thousand, map 102's pans).
        static thread_local std::vector<std::uint32_t> by_sx, near_idx;
        bool by_sx_ready = false;
        auto middles_in = [&](int lo, int hi) -> const std::vector<std::uint32_t>& {
            if (!by_sx_ready) {
                by_sx_ready = true;
                by_sx.resize(middles.size());
                for (std::size_t i = 0; i < middles.size(); ++i) by_sx[i] = static_cast<std::uint32_t>(i);
                std::sort(by_sx.begin(), by_sx.end(), [&](std::uint32_t a, std::uint32_t b) {
                    return middles[a].sx != middles[b].sx ? middles[a].sx < middles[b].sx : a < b;
                });
            }
            near_idx.clear();
            auto it = std::lower_bound(by_sx.begin(), by_sx.end(), lo,
                                       [&](std::uint32_t i, int v) { return middles[i].sx < v; });
            for (; it != by_sx.end() && middles[*it].sx <= hi; ++it) near_idx.push_back(*it);
            std::sort(near_idx.begin(), near_idx.end());
            return near_idx;
        };
        int shape_id = 0;
        // Seeds wholly past the scene within the margin, on the sides their
        // shadows fall from (toward_scene, below): 32 popped a shadow in as
        // a prop came within it on the pan.
        constexpr int kSeedBand = M;
        const float sun_lx = sun_x, sun_lz = sun_z;
        const float shadow_sx = 0.5f * (sun_lz - sun_lx), shadow_sy = 0.25f * (-sun_lx - sun_lz);
        auto toward_scene = [&](int px, int py) {
            constexpr float kSlack = 0.15f;
            if (px >= w && shadow_sx > -kSlack) return false;
            if (px < 0 && shadow_sx < kSlack) return false;
            // (Up and down only when clearly away: the ground's heights move
            // a shadow up and down too, and one at the top edge popped.)
            if (py >= h && shadow_sy > kSlack) return false;
            if (py < 0 && shadow_sy < -kSlack) return false;
            return true;
        };
        // (Rows and columns, not a division per padded pixel; the fill
        // steps by +-1 and +-W2.)
        // The cache (Bg1Cache): the key over everything the loop below reads.
        static const bool bg1_cache_on = kol::env_not_zero("KOL_BG1_CACHE");
        static const bool bg1_cache_check = kol::env_true("KOL_BG1_CACHE_CHECK");
        // By the terrain's places (place_key: not its light, so the sunset's
        // steps keep it), the heights, and the rest below.
        std::uint64_t bk = 1469598103934665603ull ^ (static_cast<std::uint64_t>(place_key) << 32 | heights_key);
        auto bmix = [&bk](std::uint64_t v) { kol::fnv64x_mix(bk, v); };
        for (int v : {w, h, kx, ky, ox, oy, M}) bmix(static_cast<std::uint64_t>(v + 65536));
        {
            std::uint32_t a, b;
            std::memcpy(&a, &sun_lx, 4);
            std::memcpy(&b, &sun_lz, 4);
            bmix(static_cast<std::uint64_t>(a) << 32 | b);
        }
        bmix(ok);   // the opaque map and in_scene, by what they read (above)
        for (const Foot& m : middles) bmix(static_cast<std::uint64_t>(m.sx + 65536) << 40 ^ static_cast<std::uint64_t>(m.sy + 65536) << 20 ^ static_cast<std::uint64_t>(m.h + 64));
        ++g_bg1_cache.frames;
        Bg1Slot* hit_slot = nullptr;
        if (bg1_cache_on)
            for (Bg1Slot& sl : g_bg1_cache.slot)
                if (sl.valid && sl.key == bk) hit_slot = &sl;
        const bool bg1_hit = hit_slot != nullptr;
        static thread_local std::vector<Bg1Px> rec_px;
        static thread_local std::vector<int> rec_plants;
        rec_px.clear();
        rec_plants.clear();
        auto replay = [&](const Bg1Slot& c) {
            for (const Bg1Px& r : c.px) {
                set_prop_px(r.k, r.xz, r.y, r.cell);
                g_bg1_shape[r.k] = r.shape;
            }
            g_prop_out = c.out;
            for (int cell : c.plants) g_plant_cell[cell] = true;
        };
        if (bg1_hit) {
            ++g_bg1_cache.hits;
            hit_slot->used = ++g_bg1_cache.clock;
        }
        if (bg1_hit && !bg1_cache_check) replay(*hit_slot);
        else {
        make_opaque();
        seen.assign(static_cast<std::size_t>(W2) * H2, 0);
        for (int sy0 = -kSeedBand; sy0 < h + kSeedBand; ++sy0)
        for (int sx0 = -kSeedBand; sx0 < w + kSeedBand; ++sx0) {
            const std::uint32_t s0 = pad(sx0, sy0);
            if (seen[s0] || !opaque[s0]) continue;
            // Wholly past the scene, only where its shadow falls toward the
            // scene: the shadow's way on the screen is (lz - lx) / 2 across,
            // (-lx - lz) / 4 down (map 102's falls to the left: a prop past
            // the left, top or bottom edge darkens nothing of it).
            if (!in_scene(sx0, sy0) && !toward_scene(sx0, sy0)) continue;
            shape.clear();
            stack.assign(1, s0);
            seen[s0] = 1;
            int x0 = INT_MAX, x1 = INT_MIN, y0 = INT_MAX, y1 = INT_MIN;
            while (!stack.empty()) {
                const std::uint32_t q = stack.back();
                stack.pop_back();
                shape.push_back(q);
                const int px = pad_x(q), py = pad_y(q);
                x0 = std::min(x0, px); x1 = std::max(x1, px);
                y0 = std::min(y0, py); y1 = std::max(y1, py);
                const int dx0 = px > -M ? -1 : 0, dx1 = px < w + M - 1 ? 1 : 0;
                const int dy0 = py > -M ? -1 : 0, dy1 = py < h + M - 1 ? 1 : 0;
                for (int dy = dy0; dy <= dy1; ++dy)
                    for (int dx = dx0; dx <= dx1; ++dx) {
                        const std::uint32_t n2 = static_cast<std::uint32_t>(static_cast<int>(q) + dy * W2 + dx);
                        if (!opaque[n2] || seen[n2]) continue;
                        seen[n2] = 1;
                        stack.push_back(n2);
                    }
            }
            const int rows = y1 - y0 + 1;
            // Each row's span and count.
            static thread_local std::vector<int> lo, hi, cnt;
            lo.assign(rows, w); hi.assign(rows, -1); cnt.assign(rows, 0);
            for (std::uint32_t q : shape) {
                const int r = pad_y(q) - y0, px = pad_x(q);
                lo[r] = std::min(lo[r], px); hi[r] = std::max(hi[r], px); ++cnt[r];
            }
            int solid = 0, widest = 0;
            for (int r = 0; r < rows; ++r) {
                solid += cnt[r] == hi[r] - lo[r] + 1;
                widest = std::max(widest, hi[r] - lo[r] + 1);
            }
            if (rows >= 3 && rows <= 12 && solid == rows && hi[rows - 1] - lo[rows - 1] + 1 == widest) continue;
            // Its feet: the cell middles its bottom edge rests on (a tree's
            // trunk or a bush's base stands on its cell's middle). A row of
            // trees whose branches touch is one shape with a foot per tree:
            // all on the front one's card, the trees behind floated over
            // their ground under the stick (the Thor, viewer at 18,-3). A
            // middle counts when the shape has a few pixels just over it and
            // its lowest pixel there is from 4 rows over it to 5 under it
            // (the trunks end 4-5 under).
            const int bw = x1 - x0 + 1;
            static thread_local std::vector<int> bottom;
            static thread_local std::vector<std::uint8_t> mask;
            bottom.assign(bw, -1);
            mask.assign(static_cast<std::size_t>(bw) * rows, 0);
            for (std::uint32_t q : shape) {
                const int c = pad_x(q) - x0, r = pad_y(q) - y0;
                mask[static_cast<std::size_t>(r) * bw + c] = 1;
                bottom[c] = std::max(bottom[c], r + y0);
            }
            static thread_local std::vector<Foot> feet;
            feet.clear();
            for (const std::uint32_t mi : middles_in(x0 - 2, x1 + 2)) {
                const Foot& m = middles[mi];
                if (m.sx < x0 - 2 || m.sx > x1 + 2 || m.sy < y0 || m.sy > y1 + 4) continue;
                int near = 0, low = -1;
                for (int c = std::max(x0, m.sx - 3); c <= std::min(x1, m.sx + 3); ++c) {
                    if (std::abs(c - m.sx) <= 2) low = std::max(low, bottom[c - x0]);
                    for (int r = std::max(y0, m.sy - 6); r <= std::min(y1, m.sy + 1); ++r)
                        near += mask[static_cast<std::size_t>(r - y0) * bw + (c - x0)];
                }
                if (near < 3 || low < m.sy - 4 || low > m.sy + 5) continue;
                Foot mf = m;
                mf.low = low;
                bool dup = false;
                for (Foot& f : feet)
                    if (std::abs(f.sx - m.sx) < 6) {
                        dup = true;
                        if (m.sy > f.sy) f = mf;
                    }
                if (!dup) feet.push_back(mf);
            }
            // Two feet less than 20 across whose lowest rows agree (within 2)
            // are one base, a bush or a rock wide over two middles: trunks on
            // two diagonals end 8 rows apart, as their middles are. It keeps
            // the middle its bottom ends about 4 rows under, as a trunk does.
            std::sort(feet.begin(), feet.end(), [](const Foot& a, const Foot& b) { return a.sx < b.sx; });
            for (std::size_t i = 1; i < feet.size();) {
                const Foot& a = feet[i - 1];
                const Foot& b = feet[i];
                if (b.sx - a.sx >= 20 || std::abs(a.low - b.low) > 2) { ++i; continue; }
                if (std::abs(b.low - b.sy - 4) < std::abs(a.low - a.sy - 4)) feet[i - 1] = b;
                feet.erase(feet.begin() + static_cast<std::ptrdiff_t>(i));
            }
            if (feet.empty()) {
                // One foot: the middle of the last two rows, on the first
                // height, from the top, whose cell under that point is at it;
                // else the nearest cell middle within 12 pixels.
                long sum_x = 0, n_x = 0;
                for (std::uint32_t q : shape)
                    if (pad_y(q) >= y1 - 1) { sum_x += 2 * static_cast<long>(pad_x(q)) + 1; n_x += 2; }
                const float fx = static_cast<float>(sum_x) / static_cast<float>(n_x);
                const float fy = static_cast<float>(y1) + 1.0f;
                for (int gh = 63; gh >= -16 && feet.empty(); --gh) {
                    const float d = 2.0f * (fx - static_cast<float>(kx + ox));             // X - Z
                    const float sxz = 4.0f * (fy - static_cast<float>(ky + oy) + 8.0f * gh);   // X + Z
                    const int ccx = static_cast<int>(std::floor((sxz + d) / 64.0f));
                    const int ccz = static_cast<int>(std::floor((sxz - d) / 64.0f));
                    if (!in_grid(ccx, ccz) || cell_height(ccx, ccz) != gh) continue;
                    feet.push_back({static_cast<int>(fx), static_cast<int>(fy), ccx, ccz, gh,
                                    static_cast<int>(std::lround(sxz))});
                }
                if (feet.empty()) {
                    int best = 12 * 12 + 1;
                    // (Only middles within 12 across can come under it.)
                    for (const std::uint32_t mi : middles_in(static_cast<int>(fx) - 12, static_cast<int>(fx) + 12)) {
                        const Foot& m = middles[mi];
                        const int dx = m.sx - static_cast<int>(fx), dy = m.sy - static_cast<int>(fy);
                        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; feet.assign(1, m); }
                    }
                }
            }
            // KOL_BG1_PROPS_LOG=1: each shape's box and feet.
            if (shape_log) {
                std::fprintf(stderr, "bg1_shape: n %zu box %d,%d-%d,%d feet", shape.size(), x0, y0, x1, y1);
                for (const Foot& f : feet) std::fprintf(stderr, " %d,%d@%d,%d h%d", f.sx, f.sy, f.cx, f.cz, f.h);
                std::fprintf(stderr, "\n");
            }
            if (feet.empty()) continue;   // over no ground
            // One creature, one foot (map_settings one_foot): the one nearest
            // the shape's middle column.
            if (feet.size() > 1 && kol::map_flag(kol::MapFlag::OneFoot, map)) {
                const int mid = (x0 + x1 + 1) / 2;
                std::size_t keep = 0;
                for (std::size_t i = 1; i < feet.size(); ++i)
                    if (std::abs(feet[i].sx - mid) < std::abs(feet[keep].sx - mid)) keep = i;
                feet.assign(1, feet[keep]);
            }
            // A rock: nearly every row solid, its top third narrow.
            int top_w = 0, top_n = 0;
            for (int r = 0; r < std::max(1, rows / 3); ++r) { top_w += hi[r] - lo[r] + 1; ++top_n; }
            // (Map 57: its BG1 is Shaher, the final boss, not a plant: he does not
            // sway; the user, 2026-11-11, asked that he not sway like a tree.)
            const bool rock = (4 * solid >= 3 * rows && 20 * top_w < 11 * widest * top_n) || map == 57;
            for (const Foot& f : feet) {
                g_plant_cell[gi(f.cx, f.cz)] = g_plant_cell[gi(f.cx, f.cz)] || !rock;
                if (!rock) rec_plants.push_back(gi(f.cx, f.cz));
            }
            // Each pixel goes with the foot nearest across among those under
            // it (else the nearest).
            const int id = shape_id++;
            for (std::uint32_t q : shape) {
                const int px = pad_x(q), py = pad_y(q);
                const Foot* best = nullptr;
                int bd = INT_MAX;
                for (const Foot& f : feet) {
                    if (f.sy < py - 2) continue;
                    const int d = std::abs(px - f.sx);
                    if (d < bd || (d == bd && best && f.sy > best->sy)) { bd = d; best = &f; }
                }
                if (!best)
                    for (const Foot& f : feet) {
                        const int d = std::abs(px - f.sx) + std::abs(py - f.sy);
                        if (d < bd) { bd = d; best = &f; }
                    }
                if (!in_scene(px, py)) {   // past the scene: its shadow's only
                    g_prop_out.push_back({id, gi(best->cx, best->cz), px, py, best->xz, 16 * best->h});
                    continue;
                }
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                set_prop_px(k, best->xz, 16 * best->h, gi(best->cx, best->cz));   // claimed, not spread from
                g_bg1_shape[k] = id;
                rec_px.push_back({static_cast<std::uint32_t>(k), best->xz, 16 * best->h, gi(best->cx, best->cz), id});
            }
        }
        }
        if (bg1_cache_on && !(bg1_hit && !bg1_cache_check)) {
            if (bg1_hit) {   // the check: what the loop wrote against the cache
                const bool same = rec_px == hit_slot->px && g_prop_out == hit_slot->out && rec_plants == hit_slot->plants;
                g_bg1_cache.count_check(same, "bg1");
            } else {   // kept, in the slot least lately used
                Bg1Slot* to = &g_bg1_cache.slot[0];
                for (Bg1Slot& sl : g_bg1_cache.slot) {
                    if (!sl.valid) { to = &sl; break; }
                    if (sl.used < to->used) to = &sl;
                }
                to->key = bk;
                to->px.swap(rec_px);   // (scratch: cleared before its next use)
                to->out = g_prop_out;
                to->plants.swap(rec_plants);
                to->valid = true;
                to->used = ++g_bg1_cache.clock;
            }
        }
    }
}

// Each prop's place spread through its connected prop pixels.
void SceneBuild::spread() {
    wt.lap("scene.spread");
    // Breadth first, so every unclaimed prop pixel takes the depth of the
    // nearest seeded prop -- but only within kPropReach pixels, so a prop does
    // not swallow the cliff-edge overlays that are also on BG1.
    for (std::size_t head = 0; head < g_queue.size(); ++head) {
        const std::uint32_t k = g_queue[head];
        const std::uint8_t d = g_prop_dist[k];
        if (d >= kPropReach) continue;
        const int px = static_cast<int>(k % w), py = static_cast<int>(k / w);
        const int nx[4] = {px - 1, px + 1, px, px};
        const int ny[4] = {py, py, py - 1, py + 1};
        for (int n = 0; n < 4; ++n) {
            if (nx[n] < 0 || ny[n] < 0 || nx[n] >= w || ny[n] >= h) continue;
            const std::size_t m = static_cast<std::size_t>(ny[n]) * w + nx[n];
            if (g_prop_dist[m] != kUnreached || !prop_pixel(m) || below_base(m, g_xz[k], g_y[k])) continue;
            g_prop_dist[m] = static_cast<std::uint8_t>(d + 1);
            g_prop_cell[m] = g_prop_cell[k];
            g_xz[m] = g_xz[k];
            g_y[m] = g_y[k];
            g_kind[m] = kol::kSceneUpright;
            g_queue.push_back(static_cast<std::uint32_t>(m));
        }
    }
}
}  // namespace kol_focus_detail
