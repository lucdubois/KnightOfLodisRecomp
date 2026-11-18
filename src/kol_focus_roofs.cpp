// kol_focus_roofs.cpp — build_scene_now's roofs and boxes passes
// (SceneBuild::roofs_pass, boxes_pass): the editor's roofs, awnings and
// boxes (chimneys, dormers) placed over the terrain, with their cache.

#include "kol_focus.h"
#include "kol_focus_internal.h"
#include "kol_workers.h"

namespace {
// The screen box a roof's or a box's footprint (X0..X1 x Z0..Z1, up to
// height T) can cover, unclipped: columns x0..x1 - 1, rows y0..y1 - 1.
struct ScreenBox { int x0, x1, y0, y1; };
ScreenBox footprint_box(float X0, float X1, float Z0, float Z1, float T, int ox, int oy, int kx, int ky) {
    return {static_cast<int>(std::floor((X0 - Z1) * 0.5f)) + kx + ox - 1,
            static_cast<int>(std::ceil((X1 - Z0) * 0.5f)) + kx + ox + 1,
            static_cast<int>(std::floor((X0 + Z0) * 0.25f - T * 0.5f)) + ky + oy - 1,
            static_cast<int>(std::ceil((X1 + Z1) * 0.25f)) + ky + oy + 1};
}
// A pixel's ray (screen column hx and row, scene to camera) through a
// footprint X0..X1 x Z0..Z1: its point at height 0 (cX, cZ; at height y it
// is cX + y, cZ + y) and the heights it is over the footprint between
// (ylo..yhi; ylo > yhi: it misses).
struct FootRay { float row, cX, cZ, ylo, yhi; };
FootRay foot_ray(int px, int py, float X0, float X1, float Z0, float Z1, int ox, int oy, int kx, int ky) {
    const float hx = static_cast<float>(px - ox - kx), row = static_cast<float>(py - oy - ky);
    const float cX = 2.0f * row + hx, cZ = 2.0f * row - hx;
    const float ylo = std::max(X0 - cX, Z0 - cZ), yhi = std::min(X1 - cX, Z1 - cZ);
    return {row, cX, cZ, ylo, yhi};
}
// Where a ray first goes under a surface f(y) (>= 0 under it), searched
// piece by piece down the heights cuts[0] > cuts[1] > ... (the surface
// straight between them): false when it never does.
template <class F>
bool first_root(F&& f, const float* cuts, int nc, float* hit) {
    bool found = false;
    for (int i = 0; i + 1 < nc && !found; ++i) {
        const float hi = cuts[i], lo = cuts[i + 1], fh = f(hi), fl = f(lo);
        if (fh >= 0.0f) { *hit = hi; found = true; }
        else if (fl >= 0.0f) { *hit = lo + fl / (fl - fh) * (hi - lo); found = true; }
    }
    return found;
}
// A pass's point on a pixel (a roof's, a box's, the ray's at height hit, at
// X, Z): its place, kind (face 0 a top, flat) and face, and its world X, Z.
void place_hit(std::size_t k, float row, float hit, float X, float Z, int face, bool faces, bool world) {
    g_y[k] = static_cast<std::int32_t>(std::lround(hit));
    g_xz[k] = static_cast<std::int32_t>(std::lround(4.0f * row + 2.0f * hit));
    g_covered[k] = 1;
    g_kind[k] = face == 0 ? kol::kSceneFlat : kol::kSceneUpright;
    if (faces) g_face[k] = static_cast<std::uint8_t>(face);
    if (world) {
        g_wx[k] = static_cast<std::int16_t>(std::lround(X));
        g_wz[k] = static_cast<std::int16_t>(std::lround(Z));
    }
}
struct RoofResult {
    PassBuffers pass;   // all its parts
    std::vector<std::uint8_t> roofed, awning;
    std::vector<UnderFix> under_fix;
    void take() {
        pass.take(PassBuffers::kAll);
        roofed = g_roofed; awning = g_awning; under_fix = g_under_fix;
    }
    void put() const {
        pass.put(PassBuffers::kAll);
        g_roofed = roofed; g_awning = awning; g_under_fix = under_fix;
    }
    bool same() const {
        bool uf = under_fix.size() == g_under_fix.size();
        for (std::size_t i = 0; uf && i < under_fix.size(); ++i)
            uf = under_fix[i].k == g_under_fix[i].k && std::memcmp(under_fix[i].rgb, g_under_fix[i].rgb, 3) == 0;
        return uf && pass.same(PassBuffers::kAll) && roofed == g_roofed && awning == g_awning;
    }
};
// A few results (the cursor's hand bobs in four steps, a unit's idle frames
// cycle: one result was thrown away every other picture), kept by when last
// used; a result is taken once its key was seen in the last kRecent pictures.
struct RoofCache : CacheStats {
    static constexpr int kSlots = 4, kRecent = 8;
    struct Slot { std::uint64_t key = 0; bool valid = false; long used = 0; RoofResult r; };
    Slot slots[kSlots];
    std::uint64_t recent[kRecent] = {};
    int recent_at = 0;
    Slot* find(std::uint64_t key) {
        for (Slot& s : slots)
            if (s.valid && s.key == key) return &s;
        return nullptr;
    }
    bool seen(std::uint64_t key) const {
        for (std::uint64_t k : recent)
            if (k == key) return true;
        return false;
    }
    void note(std::uint64_t key) {
        recent[recent_at] = key;
        recent_at = (recent_at + 1) % kRecent;
    }
    Slot& oldest() {
        Slot* o = &slots[0];
        for (Slot& s : slots)
            if (!s.valid || s.used < o->used) { o = &s; if (!s.valid) break; }
        return *o;
    }
};
RoofCache g_roof_cache;
}  // namespace

namespace kol_focus_detail {
// A roof's or a box's wall (face 1 the +X one, 2 the +Z): turned away
// from the sun in its own shadow, else what the terrain casts on it, a
// little in front of it (shaded_at). And a pixel's occlusion byte as
// lit or not.
bool SceneBuild::wall_lit(int face, float X, float Z, float hit, bool awnings) {
    const float facing = face == 1 ? sun_x : sun_z;
    return facing > 0.0f && !shaded_at(face == 1 ? X + 0.5f : X, face == 2 ? Z + 0.5f : Z, hit, awnings);
}

// The editor's roofs drawn over the terrain pass's pixels (RoofCache).
void SceneBuild::roofs_pass() {
    wt.lap("roofs.key");
    // The roofs' and boxes' cache (RoofCache).
    static const bool roof_cache_on = kol::env_not_zero("KOL_ROOF_CACHE");
    if (roof_cache_on && (!roofs.empty() || !boxes.empty())) {
        std::uint64_t hk = 1469598103934665603ull ^ pass_key;
        auto mix = [&hk](std::uint64_t v) { kol::fnv64x_mix(hk, v); };
        auto mixf = [&mix](float f) {
            std::uint32_t u;
            std::memcpy(&u, &f, sizeof u);
            mix(u);
        };
        mix(static_cast<std::uint64_t>(any_open));
        // What the roofs' and boxes' pass reads of the frame, over each one's
        // screen box: the layer it takes a pixel for (scene_top, under a
        // sprite the layer below), and of the colours only whether a pixel
        // can be a roof's outline (a unit's idle frames, the hidden rain and
        // the water's animation change the colours, not those). An awning
        // reads the colours whole, and 8 px round it for its cloth, 16 for
        // the ground behind it (under-fix): those, whole, over its box grown
        // by 18. The roofs' outline joins a pixel beside one: grown by 1.
        auto ground_class = [&](std::size_t k) -> std::uint8_t {
            std::uint8_t v = static_cast<std::uint8_t>(ground_layer(layers, k) & 0x0Fu);
            if (g_scene_rgb) {
                const std::uint8_t* c = g_scene_rgb + k * 3;
                if (c[0] + c[1] + c[2] < 200 && c[2] >= c[0]) v |= 0x10u;   // (roof_outline's test)
            }
            return v;
        };
        auto cover = [&](float X0, float X1, float Z0, float Z1, float T, int margin, bool rgb) {
            const ScreenBox B = footprint_box(X0, X1, Z0, Z1, T, ox, oy, kx, ky);
            const int sx0 = std::max(0, B.x0 - margin), sx1 = std::min(w, B.x1 + margin);
            const int sy0 = std::max(0, B.y0 - margin), sy1 = std::min(h, B.y1 + margin);
            mix(static_cast<std::uint64_t>(sx0) << 48 | static_cast<std::uint64_t>(sx1) << 32 |
                static_cast<std::uint64_t>(sy0) << 16 | static_cast<std::uint64_t>(sy1));
            for (int y = sy0; y < sy1; ++y) {
                std::uint64_t acc = 0;
                int n = 0;
                for (int x = sx0; x < sx1; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * w + x;
                    acc = acc << 8 | ground_class(k);
                    if (rgb && g_scene_rgb) {
                        const std::uint8_t* c = g_scene_rgb + k * 3;
                        mix(static_cast<std::uint64_t>(c[0]) << 16 | static_cast<std::uint64_t>(c[1]) << 8 | c[2]);
                    }
                    if (++n == 8) { mix(acc); acc = 0; n = 0; }
                }
                mix(acc);
            }
        };
        for (std::size_t ri = 0; ri < roofs.size(); ++ri) {
            const kol::TileRoof& r = roofs[ri];
            for (int v : {r.x0, r.z0, r.x1, r.z1, static_cast<int>(r.along_x), r.gables, r.eave2, r.ridge2, r.shape,
                          static_cast<int>(r.open), r.dx8, r.dz8})
                mix(static_cast<std::uint64_t>(v + 65536));
            const RoofExtent& ext = roof_ext[ri];
            cover(ext.X0, ext.X1, ext.Z0, ext.Z1, 8.0f * r.ridge2, r.open ? 18 : 1, r.open);
        }
        for (const Box& b : boxes) {
            for (float f : {b.X0, b.X1, b.Z0, b.Z1, b.E, b.T}) mixf(f);
            mix(static_cast<std::uint64_t>(b.along_x));
            cover(b.X0, b.X1, b.Z0, b.Z1, b.T, 1, false);
        }
        roof_key = hk;
        ++g_roof_cache.frames;
        if (RoofCache::Slot* slot = g_roof_cache.find(roof_key)) {
            ++g_roof_cache.hits;
            slot->used = g_roof_cache.frames;
            roof_hit = !roof_cache_check;   // check: built anyway, compared below
            if (roof_hit) slot->r.put();
        }
    }
    // Roofs set with the editor (see roof_y): a house is its footprint up to
    // the roof, `gables` prisms whose slopes fall from the ridge to the eave.
    // The grid holds a house as a flat block (map 95: every cell at 9), and
    // the block drawn under the art put square tops and walls where the art
    // has slopes and gable ends: the light fell wrong on them. Each map pixel
    // in a roof's screen box follows its ray (X = 2 row + hx + Y, Z = 2 row
    // - hx + Y, nearer as Y grows) to the first point inside the house: a
    // near wall where the ray enters the footprint at or under the roof
    // there, else the first slope it meets (a slope steeper than the ray,
    // facing away, is not met). The block's own pixels always take it; any
    // other pixel only where the house is nearer than what it shows.
    wt.lap("scene.roofs");
    // KOL_ROOF_PAR_CHECK=1: the roofs drawn again from the same start on one
    // thread (roof_par_serial_now), and everything the pass leaves compared.
    static const bool roof_par_check = kol::env_set("KOL_ROOF_PAR_CHECK");
    static thread_local RoofResult check_start;
    const bool checking_split = roof_par_check && !roof_par_serial_now && !roof_hit && !roofs.empty();
    if (checking_split) check_start.take();
    if (!roof_hit) {
        g_awning.clear();
        g_under_fix.clear();
    }
    if (!roof_hit && !roofs.empty()) {
        auto ground_layer_of = [&](std::size_t k) { return ground_layer(layers, k); };
        const std::size_t npx = static_cast<std::size_t>(w) * h;
        const bool owners = g_cell_owner.size() == npx;
        const bool faces = g_face.size() == npx;
        const bool shade = g_ao.size() == npx && (ao_s > 0.0f || sh_s > 0.0f);
        const bool world = g_wx.size() == npx && g_wz.size() == npx;
        g_roofed.assign(npx, 0);
        if (any_open) g_awning.assign(npx, 0);
        // KOL_AWNING_MASK=<path>: the pixels the awnings took this pass, as a
        // PGM (1 a slope, 2 and 3 the hanging edge), for fitting one to its
        // art over the flat frame.
        static const char* const mask_path = std::getenv("KOL_AWNING_MASK");
        std::vector<std::uint8_t> awning_mask_store;
        std::vector<std::uint8_t>* const awning_mask = mask_path && any_open ? &awning_mask_store : nullptr;
        if (awning_mask) awning_mask->assign(npx, 0);
        // Roof ri over the pixels of rows [row_lo, row_hi).
        auto draw_roof = [&](std::size_t ri, int row_lo, int row_hi) {
            const kol::TileRoof& r = roofs[ri];
            const RoofExtent& ext = roof_ext[ri];
            const float X0 = ext.X0, X1 = ext.X1, Z0 = ext.Z0, Z1 = ext.Z1;
            const float T = 8.0f * r.ridge2, E = 8.0f * r.eave2;
            const bool across_x = !r.along_x;   // the slopes fall along X
            const float a0 = across_x ? X0 : Z0, a1 = across_x ? X1 : Z1;
            const float wg = (a1 - a0) / static_cast<float>(r.gables);
            // Fall per unit: a gable's half falls over half the prism, a
            // single slope (shape 1 or 2) over all of it.
            const float run = r.shape == 0 ? 0.5f * wg : wg;
            const float s = run > 0.0f ? (T - E) / run : 0.0f;
            const float sun_a = across_x ? sun_x : sun_z;
            // A pyramid (shape 11): four planes from the middle.
            const bool pyramid = r.shape == 11;
            const float Xm = 0.5f * (X0 + X1), Zm = 0.5f * (Z0 + Z1);
            const float hxw = 0.5f * (X1 - X0), hzw = 0.5f * (Z1 - Z0);
            auto pyramid_at = [&](float X, float Z) {
                return T - (T - E) * std::max(std::abs(X - Xm) / hxw, std::abs(Z - Zm) / hzw);
            };
            // A corner peak (shape 3-6): two planes, A over the part
            // nearer the X sides (u >= v), B the Z sides.
            // A valley (7-10) is the same with the planes rising from
            // the corner at the eave: v_sign -1, base E.
            const bool corner = r.shape >= 3 && !pyramid;
            const bool valley = r.shape >= 7 && !pyramid;
            const int ci = (r.shape - 3) % 4;
            const float sxp = (ci == 0 || ci == 1) ? 1.0f : -1.0f;   // the corner's X side
            const float szp = (ci == 0 || ci == 3) ? 1.0f : -1.0f;
            const float v_sign = valley ? -1.0f : 1.0f;
            const float base = valley ? E : T;
            const float kA = (T - E) / (X1 - X0), kB = (T - E) / (Z1 - Z0);
            auto corner_at = [&](float X, float Z) {
                const float u = (sxp > 0.0f ? X1 - X : X - X0) / (X1 - X0);
                const float v = (szp > 0.0f ? Z1 - Z : Z - Z0) / (Z1 - Z0);
                return base - v_sign * (T - E) * std::max(u, v);
            };
            auto roof_at = [&](float a) {
                const int i = std::clamp(static_cast<int>((a - a0) / wg), 0, r.gables - 1);
                const float aL = a0 + i * wg;
                if (r.shape == 1) return T - s * (a - aL);
                if (r.shape == 2) return T - s * (aL + wg - a);
                return T - s * std::abs(a - (aL + 0.5f * wg));
            };
            auto in_roof_cell = [&](int cell) {
                if (cell < 0) return false;
                const int cx = cell_x(cell), cz = cell_y(cell);
                return cx >= r.x0 && cx <= r.x1 && cz >= r.z0 && cz <= r.z1;
            };
            const auto [sx0, sx1, sy0, sy1] = footprint_box(X0, X1, Z0, Z1, T, ox, oy, kx, ky);
            // An awning's art shares its layer with what stands at its
            // edge (map 89: the fishmonger's head is drawn over the cloth's
            // hem), and nothing in the shape tells them apart. Its colours
            // do: a first pass collects those of the pixels well up its
            // slopes, where only the cloth can be, and the second takes
            // only pixels wearing one of them. (Without the scene's
            // colours, the depth of field's call, every pixel is taken.)
            std::vector<std::uint32_t> cloth;
            // ... and a third pass, the cloth the game drew on BG0: in the
            // tiles where other art has BG1 (the fishmonger), so only a
            // BG0 pixel that has, along its row within kAwningNear, the
            // cloth on one side and the cloth or a BG1 pixel the second pass
            // refused on the other (BG0 between them passed over). The wall under the
            // hem wears the cloth's colours too, and has no such neighbours.
            // (Kept per thread, cleared over the box's rows only: it is
            // written in the box and read along its rows.)
            static thread_local std::vector<std::uint8_t> refused;
            if (r.open && g_scene_rgb) {
                refused.resize(npx);
                const int ry0 = std::max(0, sy0), ry1 = std::min(h, sy1);
                if (ry1 > ry0)
                    std::fill(refused.begin() + static_cast<std::ptrdiff_t>(ry0) * w,
                              refused.begin() + static_cast<std::ptrdiff_t>(ry1) * w, 0);
            }
            constexpr int kAwningNear = 8;
            for (int phase = r.open && g_scene_rgb ? 0 : 1; phase < (r.open && g_scene_rgb ? 3 : 2); ++phase)
            for (int py = std::max({0, sy0, row_lo}); py < std::min({h, sy1, row_hi}); ++py)
                for (int px = std::max(0, sx0); px < std::min(w, sx1); ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    const unsigned ground_layer = ground_layer_of(k);
                    if (ground_layer > 1u) continue;   // sky, interface
                    // An awning is its own art only: the layer drawn over the
                    // ground's (BG1), and the ground's own (BG0) only where
                    // it wears the cloth's colours (below): map 89 draws the
                    // cloth on BG0 in the tiles the fishmonger has on BG1,
                    // and left there it was a bite out of the hem under the
                    // stick. The ground seen past its edge stays the ground.
                    if (r.open && (ground_layer == 0u) != (phase == 2)) continue;
                    const FootRay ray = foot_ray(px, py, X0, X1, Z0, Z1, ox, oy, kx, ky);
                    const float row = ray.row, cX = ray.cX, cZ = ray.cZ, ylo = ray.ylo, yhi = ray.yhi;
                    if (ylo > yhi) continue;   // the ray misses the footprint
                    const float cA = across_x ? cX : cZ;
                    float hit = 0.0f;
                    int face = 0;   // 0 the roof, 1 the +X wall, 2 the +Z wall
                    if (r.open) {
                        // No walls: a ray that comes in under the edge goes
                        // on under the awning, to whatever stands there.
                        // Except just under it, the cloth's hanging edge
                        // (awning_skirt()), which the walls' case below takes.
                        const float Xin = cX + yhi, Zin = cZ + yhi;
                        const float edge = pyramid ? pyramid_at(Xin, Zin) : corner ? corner_at(Xin, Zin)
                                                                                   : roof_at(cA + yhi);
                        if (edge - yhi > awning_skirt()) continue;
                    }
                    if (pyramid) {
                        auto f = [&](float y) { return pyramid_at(cX + y, cZ + y) - y; };
                        if (f(yhi) >= 0.0f) {
                            hit = yhi;
                            face = X1 - cX <= Z1 - cZ ? 1 : 2;
                        } else {
                            // The height along the ray is straight between
                            // the middle lines and the two diagonals: the
                            // first piece it goes under the roof in.
                            float cuts[6];
                            int nc = 0;
                            cuts[nc++] = yhi;
                            auto cut = [&](float y) { if (y > ylo && y < yhi) cuts[nc++] = y; };
                            cut(Xm - cX);
                            cut(Zm - cZ);
                            const float dX = (cX - Xm) / hxw, dZ = (cZ - Zm) / hzw;
                            if (hxw != hzw) cut((dZ - dX) / (1.0f / hxw - 1.0f / hzw));
                            cut(-(dZ + dX) / (1.0f / hxw + 1.0f / hzw));
                            cuts[nc++] = ylo;
                            std::sort(cuts, cuts + nc, std::greater<float>());
                            if (!first_root(f, cuts, nc, &hit)) continue;
                        }
                    } else if (corner) {
                        if (yhi <= corner_at(cX + yhi, cZ + yhi)) {
                            hit = yhi;
                            face = X1 - cX <= Z1 - cZ ? 1 : 2;
                        } else {
                            // Each plane: Y = H on the ray, entered from above
                            // only where it is not steeper than the ray.
                            bool found = false;
                            const float ax = sxp > 0.0f ? X1 : X0, az = szp > 0.0f ? Z1 : Z0;
                            for (int plane = 0; plane < 2; ++plane) {
                                const float k = v_sign * (plane == 0 ? kA * sxp : kB * szp);
                                if (1.0f - k <= 0.0f) continue;
                                const float apex = plane == 0 ? ax : az, c = plane == 0 ? cX : cZ;
                                const float y = (base - k * (apex - c)) / (1.0f - k);
                                if (y < ylo || y > yhi || (found && y <= hit)) continue;
                                const float Xh = cX + y, Zh = cZ + y;
                                const float u = (sxp > 0.0f ? X1 - Xh : Xh - X0) / (X1 - X0);
                                const float v = (szp > 0.0f ? Z1 - Zh : Zh - Z0) / (Z1 - Z0);
                                if ((plane == 0) != (u >= v)) continue;   // the other plane's part
                                hit = y;
                                found = true;
                            }
                            if (!found) continue;
                        }
                    } else if (yhi <= roof_at(cA + yhi)) {
                        hit = yhi;
                        face = X1 - cX <= Z1 - cZ ? 1 : 2;
                    } else {
                        bool found = false;
                        const int top_i = std::clamp(static_cast<int>((cA + yhi - a0) / wg), 0, r.gables - 1);
                        for (int i = top_i; i >= 0 && !found; --i) {
                            const float aL = a0 + i * wg, aR = aL + wg;
                            // The ridge: the middle of a gable, the high side
                            // of a single slope (its other half is empty).
                            const float rr = r.shape == 1 ? aL : r.shape == 2 ? aR : aL + 0.5f * wg;
                            // The falling half (a past the ridge), then the rising one.
                            const float yf = (T - s * (cA - rr)) / (1.0f + s);
                            if (cA + yf >= rr - 0.01f && cA + yf <= aR + 0.01f && yf >= ylo && yf <= yhi) {
                                hit = yf;
                                found = true;
                                break;
                            }
                            if (s < 1.0f) {
                                const float yr = (T - s * rr + s * cA) / (1.0f - s);
                                if (cA + yr >= aL - 0.01f && cA + yr <= rr + 0.01f && yr >= ylo && yr <= yhi) {
                                    hit = yr;
                                    found = true;
                                }
                            }
                        }
                        if (!found) continue;
                    }
                    // (An awning owns no cell: the ground under it does.)
                    const bool own = !r.open && owners && in_roof_cell(g_cell_owner[k]);
                    if (!own && g_covered[k] && static_cast<float>(g_y[k]) > hit) continue;   // in front
                    if (r.open && ground_layer == 0u) {
                        // BG0: only in a colour learned from BG1, and only
                        // where the pixel itself shows (not under a sprite).
                        if (cloth.empty() || scene_top(layers, k) != 0u) continue;
                        // 1 the cloth, 2 refused, 0 neither, each way.
                        auto side = [&](int step) {
                            for (int d = 1; d <= kAwningNear; ++d) {
                                const int x = px + step * d;
                                if (x < 0 || x >= w) return 0;
                                const std::size_t q = static_cast<std::size_t>(py) * w + x;
                                if (g_awning[q]) return 1;
                                if (refused[q]) return 2;
                                if (scene_top(layers, q) != 0u) return 0;
                            }
                            return 0;
                        };
                        const int left = side(-1), right = side(1);
                        if (left == 0 || right == 0 || (left != 1 && right != 1)) continue;
                        const std::uint8_t* c = g_scene_rgb + k * 3;
                        const std::uint32_t rgb = static_cast<std::uint32_t>(c[0]) << 16 |
                                                  static_cast<std::uint32_t>(c[1]) << 8 | c[2];
                        if (std::find(cloth.begin(), cloth.end(), rgb) == cloth.end()) continue;
                    } else if (r.open && g_scene_rgb && scene_top(layers, k) == 1u) {
                        const std::uint8_t* c = g_scene_rgb + k * 3;
                        const std::uint32_t rgb = static_cast<std::uint32_t>(c[0]) << 16 |
                                                  static_cast<std::uint32_t>(c[1]) << 8 | c[2];
                        const bool known = std::find(cloth.begin(), cloth.end(), rgb) != cloth.end();
                        if (phase == 0) {
                            if (face == 0 && hit > E + awning_sure() * (T - E) && !known) cloth.push_back(rgb);
                            continue;
                        }
                        if (!cloth.empty() && !known) {   // something else's art
                            refused[k] = 1;
                            continue;
                        }
                    } else if (phase == 0) {
                        continue;
                    }
                    // The house's art is its own (BG1 on map 6): nothing
                    // behind it for Lifted Walls to lift it off. Lifted, the
                    // roof showed BG0 under its art as holes (FA). Except
                    // where the roof rises over ground beyond the house (its
                    // edge against the terrace behind): that ground is behind
                    // it, BG0 there, and a camera move opens onto it instead
                    // of stretching the edge.
                    // An awning: all of it is over something else, the ground
                    // and the wall the game keeps on BG0 under its art.
                    if (g_back_kind.size() == npx) {
                        const bool beyond = r.open ? g_covered[k] && g_kind[k] != kol::kSceneNone
                                          : !own && g_covered[k] && !g_roofed[k] &&
                                            g_kind[k] != kol::kSceneNone &&
                                            hit - static_cast<float>(g_y[k]) > 16.0f;
                        keep_behind(k, beyond, npx);
                    }
                    const float X = cX + hit, Z = cZ + hit;
                    place_hit(k, row, hit, X, Z, face, faces, world);
                    if (r.open) g_awning[k] = ground_layer == 0u ? 2 : 1;
                    else g_roofed[k] = 1;
                    if (r.open && awning_mask) (*awning_mask)[k] = static_cast<std::uint8_t>(face + 1);
                    const int hcx = std::clamp(static_cast<int>(std::floor(X / 32.0f)), r.x0, r.x1);
                    const int hcz = std::clamp(static_cast<int>(std::floor(Z / 32.0f)), r.z0, r.z1);
                    if (owners) g_cell_owner[k] = static_cast<std::int16_t>(gi(hcx, hcz));
                    // The block's own walls keep the terrain pass's light up to
                    // the eave; over it (a gable end) that pass drew the
                    // cell's top or the house behind, and lit the wall as
                    // that: shadowed wedges on a wall facing the sun (map 6).
                    if (shade && (face == 0 || !own || hit > E + 0.5f)) {
                        // A slope facing away from the sun is in its own
                        // shadow; else the shadow map (roofs in it) says.
                        bool lit = true;
                        if (sh_s > 0.0f && r.open) {
                            // An awning's cloth is not shaded by which way
                            // its slopes face: a whole slope went dark at
                            // once, along the model's ridge and not the fold
                            // the art draws, and stayed so until the sun
                            // passed one angle (the Thor, 2026-10-05: the
                            // shadow did not move). Only what the terrain
                            // casts on it.
                            lit = !shaded_at(X, Z, hit + 0.5f, false);
                        } else if (sh_s > 0.0f) {
                            if (face == 0 && pyramid) {
                                // The plane falls away from the middle.
                                const float du = (X - Xm) / hxw, dv = (Z - Zm) / hzw;
                                const float facing = std::abs(du) >= std::abs(dv)
                                    ? (du >= 0.0f ? 1.0f : -1.0f) * (T - E) / hxw * sun_x
                                    : (dv >= 0.0f ? 1.0f : -1.0f) * (T - E) / hzw * sun_z;
                                lit = facing + sun_rise > 0.0f && !shaded_at(X, Z, hit + 0.5f, !r.open);
                            } else if (face == 0 && corner) {
                                const float u = (sxp > 0.0f ? X1 - X : X - X0) / (X1 - X0);
                                const float v = (szp > 0.0f ? Z1 - Z : Z - Z0) / (Z1 - Z0);
                                // Rising toward the peak: the plane faces away
                                // from it, -sxp * kA along X (or Z).
                                const float facing = -v_sign * (u >= v ? sxp * kA * sun_x : szp * kB * sun_z);
                                lit = facing + sun_rise > 0.0f && !shaded_at(X, Z, hit + 0.5f, !r.open);
                            } else if (face == 0) {
                                const float a = across_x ? X : Z;
                                const int i = std::clamp(static_cast<int>((a - a0) / wg), 0, r.gables - 1);
                                const float side = r.shape == 1 ? 1.0f : r.shape == 2 ? -1.0f
                                                 : a >= a0 + (i + 0.5f) * wg ? 1.0f : -1.0f;
                                lit = side * s * sun_a + sun_rise > 0.0f && !shaded_at(X, Z, hit + 0.5f, !r.open);
                            } else {
                                lit = wall_lit(face, X, Z, hit, !r.open);
                            }
                        }
                        g_ao[k] = lit_ao(lit);
                    }
                }
        };
        // The roofs in order, a run of closed ones over the row pool in bands
        // of rows: each band's thread draws every roof of the run in order
        // over its own rows, so a pixel still takes the roofs in the order one
        // thread gives it (a later roof tests what an earlier one left there),
        // and a roof pixel reads and writes its own pixel only. An awning
        // (open) stays on this thread, whole: its three passes hand each other
        // the cloth's colours and the pixels refused, over all its rows.
        // (2026-11-18: map 89's 60 roofs, drawn again every picture because
        // the water under its awning never leaves their cache's key alone,
        // were 3.3 ms a job on the Steam Deck.) A helper takes this thread's
        // t_present for the run. KOL_ROOF_SERIAL=1: one thread, as before.
        // KOL_ROOF_PAR_CHECK=1: see roofs_pass's end.
        static const bool roof_serial = kol::env_set("KOL_ROOF_SERIAL");
        const bool roof_split = !roof_serial && !roof_par_serial_now;
        const bool present = t_present;
        constexpr int kRoofBand = 16;
        for (std::size_t ri = 0; ri < roofs.size();) {
            if (roofs[ri].open || !roof_split) {
                draw_roof(ri, 0, h);
                ++ri;
                continue;
            }
            std::size_t end = ri;
            while (end < roofs.size() && !roofs[end].open) ++end;
            kol::parallel_rows(h, kRoofBand, [&](int, int r0, int r1) {
                const bool was = t_present;
                t_present = present;
                for (std::size_t i = ri; i < end; ++i) draw_roof(i, r0, r1);
                t_present = was;
            });
            ri = end;
        }
        // A roof's art is outlined in dark blue one pixel outside the cells
        // it covers (its back eave, the gable's slopes): those pixels' rays
        // miss the roof and they stayed on the terrain behind, a line that
        // came away from the house under the stick (FZ, map 13: a thin
        // black line not following the house). An outline-coloured
        // pixel beside a roofed one, and not in front of it, takes its place.
        // Growing the footprint instead (2 or 4 units) pulled the ground
        // beside the house onto the roof. KOL_ROOF_NO_OUTLINE=1 leaves them.
        wt.lap("roofs.outline");
        static const bool roof_outline = !kol::env_true("KOL_ROOF_NO_OUTLINE");
        if (roof_outline && g_scene_rgb) {
            auto outline = [](const std::uint8_t* c) { return c[0] + c[1] + c[2] < 200 && c[2] >= c[0]; };
            std::vector<std::pair<std::size_t, std::size_t>> claims;   // pixel, the roof pixel it joins
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if (g_roofed[k] || ground_layer_of(k) > 1u || (any_open && g_awning[k])) continue;
                    if (!outline(g_scene_rgb + k * 3)) continue;
                    static constexpr int kN[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                                     {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                    std::size_t from = npx;
                    for (const auto& d : kN) {
                        const int x = px + d[0], y = py + d[1];
                        if (x < 0 || y < 0 || x >= w || y >= h) continue;
                        const std::size_t q = static_cast<std::size_t>(y) * w + x;
                        if (!g_roofed[q]) continue;
                        // Not in front of it: nothing placed, or no higher
                        // than the roof beside it (along a ray, farther is
                        // lower). Screen depth was the first test (16 units
                        // deeper), and refused the line where the terrace
                        // behind the house stands high (FZ).
                        if (g_kind[k] != kol::kSceneNone && g_y[k] > g_y[q] + 4) continue;
                        from = q;
                        break;
                    }
                    if (from != npx) claims.emplace_back(k, from);
                }
            for (const auto& [k, q] : claims) {
                // Nothing behind it for Lifted Walls to open onto (as the
                // roof's own pixels): a one-pixel edge, and a lifted one
                // added atlas squares for nothing.
                if (g_back_kind.size() == npx) g_back_kind[k] = kol::kSceneNone;
                g_xz[k] = g_xz[q];
                g_y[k] = g_y[q];
                g_kind[k] = g_kind[q];
                g_covered[k] = 1;
                g_roofed[k] = 1;
                if (faces) g_face[k] = g_face[q];
                if (owners) g_cell_owner[k] = g_cell_owner[q];
                if (world) {
                    g_wx[k] = g_wx[q];
                    g_wz[k] = g_wz[q];
                }
                if (g_ao.size() == npx) g_ao[k] = g_ao[q];
            }
        }
        // Behind the awnings' BG0 cloth (g_awning 2): the nearest BG0 pixel
        // that is not cloth, down its column first (what stands under the
        // hem), then along its row.
        wt.lap("roofs.under");
        if (any_open && g_scene_rgb)
            for (std::size_t k = 0; k < npx; ++k) {
                if (g_awning[k] != 2) continue;
                const int x0 = static_cast<int>(k % static_cast<std::size_t>(w));
                const int y0 = static_cast<int>(k / static_cast<std::size_t>(w));
                auto plain = [&](int x, int y) {
                    if (x < 0 || y < 0 || x >= w || y >= h) return false;
                    const std::size_t q = static_cast<std::size_t>(y) * w + x;
                    return !g_awning[q] && scene_top(layers, q) == 0u;
                };
                std::size_t from = npx;
                for (int d = 1; d <= 16 && from == npx; ++d) {
                    if (plain(x0, y0 + d)) from = static_cast<std::size_t>(y0 + d) * w + x0;
                    else if (plain(x0 - d, y0)) from = static_cast<std::size_t>(y0) * w + x0 - d;
                    else if (plain(x0 + d, y0)) from = static_cast<std::size_t>(y0) * w + x0 + d;
                }
                if (from == npx) continue;
                const std::uint8_t* c = g_scene_rgb + from * 3;
                g_under_fix.push_back({static_cast<std::uint32_t>(k), {c[0], c[1], c[2]}});
            }
        if (awning_mask)
            if (std::FILE* f = std::fopen(mask_path, "wb")) {
                std::fprintf(f, "P5 %d %d 255\n", w, h);
                std::fwrite(awning_mask->data(), 1, awning_mask->size(), f);
                std::fclose(f);
            }
    }
    if (checking_split) {
        static thread_local RoofResult split_out;
        split_out.take();
        check_start.put();
        roof_par_serial_now = true;
        roofs_pass();
        roof_par_serial_now = false;
        const bool same = split_out.same();
        static long checks = 0, fails = 0;
        ++checks;
        if (!same) ++fails;
        if (!same || checks % 300 == 1)
            std::fprintf(stderr, "roof-par-check: %s, %zu roofs (%ld checks, %ld with a difference)\n",
                         same ? "the same" : "DIFFERENT", roofs.size(), checks, fails);
    }
}

// The editor's boxes, then the roofs' and boxes' cache stored.
void SceneBuild::boxes_pass() {
    wt.lap("scene.boxes");
    // Boxes set with the editor (chimneys, dormers), over the roofs: the
    // art draws them on the roof's layer, and the roof's slope flattened
    // them into it. Each pixel in a box's screen box follows its ray as the
    // roofs' do, to the nearest point inside the box: a near wall, the flat
    // top, or one of its gable's slopes (the largest Y on the ray at or
    // under the box's height, found piece by piece either side of the
    // ridge); it takes that point where the box is nearer than what the
    // pixel shows. Its art stays on it (not lifted: see the roofs).
    if (!roof_hit && !boxes.empty()) {
        const std::size_t npx = static_cast<std::size_t>(w) * h;
        const bool owners = g_cell_owner.size() == npx;
        const bool faces = g_face.size() == npx;
        const bool shade = g_ao.size() == npx && (ao_s > 0.0f || sh_s > 0.0f);
        const bool world = g_wx.size() == npx && g_wz.size() == npx;
        const bool roofed = !roofs.empty() && g_roofed.size() == npx;
        for (const Box& b : boxes) {
            const auto [sx0, sx1, sy0, sy1] = footprint_box(b.X0, b.X1, b.Z0, b.Z1, b.T, ox, oy, kx, ky);
            const bool gable = b.T > b.E;
            const float am = b.along_x ? 0.5f * (b.Z0 + b.Z1) : 0.5f * (b.X0 + b.X1);   // the ridge
            for (int py = std::max(0, sy0); py < std::min(h, sy1); ++py)
                for (int px = std::max(0, sx0); px < std::min(w, sx1); ++px) {
                    const std::size_t k = static_cast<std::size_t>(py) * w + px;
                    if (ground_layer(layers, k) > 1u) continue;   // sky, interface
                    const FootRay ray = foot_ray(px, py, b.X0, b.X1, b.Z0, b.Z1, ox, oy, kx, ky);
                    const float row = ray.row, cX = ray.cX, cZ = ray.cZ, ylo = ray.ylo, yhi = ray.yhi;
                    if (ylo > yhi) continue;   // the ray misses the footprint
                    auto f = [&](float y) { return box_h(b, cX + y, cZ + y) - y; };
                    float hit = 0.0f;
                    int face = 0;   // 0 the top, 1 the +X wall, 2 the +Z wall
                    if (f(yhi) >= 0.0f) {
                        hit = yhi;
                        face = b.X1 - cX <= b.Z1 - cZ ? 1 : 2;
                    } else {
                        float cuts[3];
                        int nc = 0;
                        cuts[nc++] = yhi;
                        const float ym = am - (b.along_x ? cZ : cX);
                        if (gable && ym > ylo && ym < yhi) cuts[nc++] = ym;
                        cuts[nc++] = ylo;
                        if (!first_root(f, cuts, nc, &hit)) continue;
                    }
                    if (g_covered[k] && static_cast<float>(g_y[k]) > hit) continue;   // in front
                    // Behind the box, for Lifted Walls: what the pixel showed
                    // (the terrain the chimney's art hides, BG0 under it), so a
                    // camera move opens onto it instead of stretching. Over a
                    // roof's pixel, what the roof keeps behind it (nothing on
                    // the house's own art: holes, FA).
                    if (g_back_kind.size() == npx && !(roofed && g_roofed[k]))
                        keep_behind(k, g_covered[k] && g_kind[k] != kol::kSceneNone, npx);
                    const float X = cX + hit, Z = cZ + hit;
                    place_hit(k, row, hit, X, Z, face, faces, world);
                    if (owners) g_cell_owner[k] = static_cast<std::int16_t>(gi(b.x, b.z));
                    if (shade) {
                        bool lit = true;
                        if (sh_s > 0.0f) {
                            if (face == 0 && gable) {
                                const float a = b.along_x ? Z : X;
                                const float half = b.along_x ? 0.5f * (b.Z1 - b.Z0) : 0.5f * (b.X1 - b.X0);
                                const float sun_a = b.along_x ? sun_z : sun_x;
                                const float side = a >= am ? 1.0f : -1.0f;
                                lit = side * (b.T - b.E) / half * sun_a + sun_rise > 0.0f &&
                                      !shaded_at(X, Z, hit + 0.5f);
                            } else if (face == 0) {
                                lit = !shaded_at(X, Z, hit + 0.5f);
                            } else {
                                lit = wall_lit(face, X, Z, hit, true);
                            }
                        }
                        g_ao[k] = lit_ao(lit);
                    }
                }
        }
    }

    wt.lap("roofs.keep");
    if (roof_key != 0 && !roof_hit) {
        if (RoofCache::Slot* slot = roof_cache_check ? g_roof_cache.find(roof_key) : nullptr) {
            g_roof_cache.count_check(slot->r.same(), "roof");
        } else if (g_roof_cache.seen(roof_key)) {
            RoofCache::Slot& slot = g_roof_cache.oldest();   // the inputs came again: keep them
            slot.r.take();
            slot.key = roof_key;
            slot.valid = true;
            slot.used = g_roof_cache.frames;
        }
        g_roof_cache.note(roof_key);
    }
}
}  // namespace kol_focus_detail
