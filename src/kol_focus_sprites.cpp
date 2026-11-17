// kol_focus_sprites.cpp — whose sprite is whose: the units (Unit), the
// order the game draws them in, and the claim of every sprite pixel of the
// latched OAM by a unit, a prop, the cursor or the interface
// (claim_unit_sprites).

#include "kol_focus.h"
#include "kol_focus_internal.h"

namespace {
inline int depth_slack(int xz) { return depth_xyz() ? xz : xz * kDepthPerXZ; }
}  // namespace

namespace kol_focus_detail {
bool weather_tile(std::uint16_t a2) {
    const unsigned t = a2 & 0x3FFu;
    return t >= 0x164u && t <= 0x168u;
}
void build_weather_px(const std::uint8_t* layers, int w, int h, int ox, int oy) {
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    g_weather_px.assign(pixels, 0);
    const Capture& cap = g_cap;
    if (!cap.valid || !cap.obj_ok) return;
    const std::uint8_t* oam = cap.oam.data();
    const std::uint8_t* tiles = cap.obj_tiles.data();
    for (int i = 0; i < 128; ++i) {
        const std::uint8_t* a = oam + i * 8;
        const std::uint16_t a0 = static_cast<std::uint16_t>(a[0] | (a[1] << 8));
        const std::uint16_t a1 = static_cast<std::uint16_t>(a[2] | (a[3] << 8));
        const std::uint16_t a2 = static_cast<std::uint16_t>(a[4] | (a[5] << 8));
        if (a0 & 0x0300u) continue;                       // affine or disabled
        if (((a0 >> 10) & 3u) == 2u) continue;            // OBJ window
        if (((a0 >> 14) & 3u) != 0u || ((a1 >> 14) & 3u) != 0u) continue;   // 8x8 only
        if (!weather_tile(a2)) continue;
        const bool bpp8 = (a0 & 0x2000u) != 0u;
        const bool hflip = (a1 & 0x1000u) != 0u, vflip = (a1 & 0x2000u) != 0u;
        const unsigned tile = a2 & 0x3FFu;
        const int rx = a1 & 0x1FF, ry0 = a0 & 0xFF;
        const int ry = ry0 >= 160 ? ry0 - 256 : ry0;
        // X is 9 bits: the wide view shows an entry past 256 on either side.
        // And the renderer's copies in the margins (kol_obj_cull: one native
        // screen away each way, the sides 80 rows up / down), outside the
        // native 240 x 160 only; not in OAM, they made streaks too.
        static constexpr int kCopy[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const auto& cp : kCopy)
        for (int xs0 : {rx, rx - 512}) {
            const int xs = xs0 + cp[0] * 240;
            const int ys = ry + cp[1] * 160 - cp[0] * 80;
            for (int ty = 0; ty < 8; ++ty) {
                const int py = ys + ty + oy;
                if (py < 0 || py >= h) continue;
                const int sy = vflip ? 7 - ty : ty;
                for (int tx = 0; tx < 8; ++tx) {
                    const int px = xs + tx + ox;
                    if (px < 0 || px >= w) continue;
                    if ((cp[0] || cp[1]) && px >= ox && px < ox + 240 && py >= oy && py < oy + 160) continue;
                    const std::size_t p = static_cast<std::size_t>(py) * w + px;
                    if (scene_top(layers, p) != kol::kLayerObj) continue;
                    const int sx = hflip ? 7 - tx : tx;
                    const std::size_t base = static_cast<std::size_t>(tile) * 32u;
                    unsigned texel;
                    if (bpp8) {
                        texel = tiles[(base + sy * 8 + sx) & 0x7FFFu];
                    } else {
                        const std::uint8_t b = tiles[(base + sy * 4 + sx / 2) & 0x7FFFu];
                        texel = (sx & 1) ? (b >> 4) : (b & 0x0Fu);
                    }
                    if (texel) g_weather_px[p] = 1;
                }
            }
        }
    }
}
bool blob_tile(const ObjEntry& e) {
    const unsigned t = e.a2 & 0x3FFu;
    return (e.w == 8 && e.h == 8 && (t == 0x14Bu || t == 0x14Cu)) ||
           (e.w == 16 && e.h == 8 && (t == 0x153u || t == 0x155u));
}
}  // namespace kol_focus_detail

namespace {
// claim_unit_sprites' frame: its arguments, the OAM entries it reads and
// the units' frames each fits, and each of its passes a member function,
// run in order (run).
struct OamClaim {
    const std::uint8_t* layers;
    int w, h, ox, oy, kx, ky;
    const Unit* units;
    int count, props;
    bool cursor;
    int cursor_hx, cursor_hy;
    std::int32_t* xz_out;
    std::int32_t* y_out;
    std::uint8_t* kind_out;
    std::uint8_t* iface_out;
    const Capture& cap = g_cap;
    std::chrono::steady_clock::time_point t0;
    const std::uint8_t* oam = nullptr;
    const std::uint8_t* tiles = nullptr;
    bool map_1d = false;
    static constexpr int kSlack = 2;

    using Entry = ObjEntry;
    Entry* const entries = g_entries;
    int n = 0;   // the entries read
    // The units an entry fits, with the reading of its position for each.
    struct Cand { int n; int u[8], x[8], y[8]; };
    static Cand cands[128];
    // Entries that are interface though not at priority 0 (below).
    static bool released[128];
    // Entries at priority 0 that are a unit's body all the same (a spell).
    static bool spell_body[128];
    // A scene object's (a door, a flag) wide frame (see fits).
    bool wide_frame[64] = {};
    // The blobs' rule (blob_to_feet; KOL_NO_BLOB_FEET=1 off), and by unit,
    // whether its blob was drawn.
    const bool blob_feet = [] {
        static const bool v = std::getenv("KOL_NO_BLOB_FEET") == nullptr;
        return v;
    }();
    static std::vector<std::uint8_t> closed;   // by unit: its blob was drawn
    bool blob_join = false;   // blob_join_on()

    void run();
    bool fits(int u, int rx, int ry, int ew, int eh, int* X, int* Y);
    // Touching or overlapping, on any alignment (a rider's pieces lie offset
    // from his horse's): the big-figure join only (join_parts).
    static bool touch_any(const Entry& a, const Entry& b) {
        return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h &&
               !((a.x + a.w == b.x || b.x + b.w == a.x) && (a.y + a.h == b.y || b.y + b.h == a.y));   // (not a corner)
    }
    static bool adjoin(const Entry& a, const Entry& b) {
        return (a.x == b.x && (a.y + a.h == b.y || b.y + b.h == a.y)) ||
               (a.y == b.y && (a.x + a.w == b.x || b.x + b.w == a.x));
    }
    void decode();
    void bags_compete();
    void adjoin_owners();
    void blob_to_feet();
    void draw_order();
    void parts_before_blob();
    void blob_join_pass();
    void flying_blobs();
    void join_parts();
    void front_to_back_run();
    void sky_units();
    void owners_by_tile_block();
    void scene_object_claim();
    void seated_claim();
    void menu_icons_free();
    void debug_dump();
    void pixel_owners();
};

OamClaim::Cand OamClaim::cands[128];
bool OamClaim::released[128];
bool OamClaim::spell_body[128];
std::vector<std::uint8_t> OamClaim::closed;

// Whether an entry at hardware (rx, ry), ew x eh, lies in unit (or prop)
// u's 32x32 frame on its feet, and the reading of its place that does:
// the one whose centre is nearest the unit's column and whose bottom is
// nearest its feet.
// Below the feet the frame reaches 8 rows. 6 first: the sailors sitting on the
// ship's deck in the opening (24 x 24 sprites) end 5 under theirs, so
// their lower parts fit no frame of their own and went to whoever walked
// past; a walker given one was too tall for a slot and drawn flat, its
// blob a black oval (states/GY_fade_shadows.state, present 344).
// And 8: a body lying on the ground in map 102's opening (unit 1 of
// states/HW_slot3_cutscene_thunder, two 16x16s) ends 8 under its feet;
// fitting no frame it stayed interface, drawn flat where the frame has
// it, and did not move with the ground under the perspective. A/B 6 vs 8
// on BK, BO, CB, CJ, GY: the same owners; HP: the game's blob under an
// archer on a step is his now (his own shadow drawn instead); HJ: single
// pixels. KOL_FRAME_FEET_SLACK=6 (2) is the frame before (the first).
bool OamClaim::fits(int u, int rx, int ry, int ew, int eh, int* X, int* Y) {
    static const int kFeetSlack = kol::env_int("KOL_FRAME_FEET_SLACK", 8);
    const int hx = feet_sx(units[u]) + kx;
    const int hy = feet_sy(units[u]) + ky;
    *X = rx + 512 * static_cast<int>(std::lround((hx - rx - ew / 2) / 512.0));
    *Y = ry + 256 * static_cast<int>(std::lround((hy - ry - eh) / 256.0));
    const int half = u < 64 && wide_frame[u] ? 32 : 16, tall = u < 64 && wide_frame[u] ? 48 : 32;
    return !(*X < hx - half - kSlack || *X + ew > hx + half + kSlack ||
             *Y < hy - tall - kSlack || *Y + eh > hy + kFeetSlack);
}

// The plain OAM entries, in OAM order, and the units' frames each fits
// (cands): settled when one does.
void OamClaim::decode() {
    for (int i = 0; i < 128; ++i) {
        const std::uint8_t* a = oam + i * 8;
        const std::uint16_t a0 = static_cast<std::uint16_t>(a[0] | (a[1] << 8));
        const std::uint16_t a1 = static_cast<std::uint16_t>(a[2] | (a[3] << 8));
        const std::uint16_t a2 = static_cast<std::uint16_t>(a[4] | (a[5] << 8));
        // Units are plain sprites: affine ones (none seen on a map) and the
        // OBJ window are left to the box.
        if (a0 & 0x0100u) continue;
        if (a0 & 0x0200u) continue;                     // disabled
        if (((a0 >> 10) & 3u) == 2u) continue;          // OBJ window
        const unsigned shape = (a0 >> 14) & 3u, size = (a1 >> 14) & 3u;
        if (shape == 3u) continue;
        const int ew = kol::kObjW[shape][size], eh = kol::kObjH[shape][size];
        const int rx = a1 & 0x1FF, ry = a0 & 0xFF;
        // X is 9 bits and Y 8 (the renderer unwraps them for the wide view):
        // the reading that puts the entry in a unit's frame is the one drawn.
        Entry e{rx, ry, ew, eh, -1, a0, a1, a2, i};
        Cand& c = cands[n];
        c.n = 0;
        released[n] = false;
        g_cursor_piece[n] = false;
        g_weather_piece[n] = ew == 8 && eh == 8 && weather_tile(a2);
        // Priority 0 is over every background: the game's interface layer
        // (cursors, the menu's hand, damage numbers). Unit parts are 1 or 2
        // on every map state checked. The Attack/Cancel hand lay inside a
        // unit's frame and went with the unit (CH_menu_hand).
        bool interface = ((a2 >> 10) & 3u) == 0u;
        // Except while a spell plays: the game puts some units' bodies at
        // priority 0 for its effect (IB, Rictor's heal, present 600: Rictor,
        // Alphonse, Stan and three more at 0, Lara and two others at 1).
        // Left interface they stayed flat over the scene, Stan's beside the
        // healed Lara. A body-sized entry (16x16, 32x32: not a number's
        // 8x8s) is then a unit's if it fits a frame. KOL_NO_SPELL_BODIES=1:
        // interface, as before.
        // And outside a spell, a body-sized one drawn from a unit's own tile
        // block (its actor slot, below) that fits that unit's frame: map
        // 67's scene raises Nichart and the soldier behind him to 0 as they
        // come through the door into the cell, and they floated over the
        // room like the HUD (states/thor/0053 ~240; the user saw him float
        // like the UI). Its block is enough: stepping down a
        // stair the head piece fit no frame for one frame in 600 (the
        // tile-block pass below places it). KOL_NO_RAISED_BODIES=1:
        // interface.
        spell_body[n] = false;
        static const bool no_spell_bodies = std::getenv("KOL_NO_SPELL_BODIES") != nullptr;
        static const bool no_raised_bodies = std::getenv("KOL_NO_RAISED_BODIES") != nullptr;
        if (interface && ew == eh && (ew == 16 || ew == 32)) {
            const unsigned t = a2 & 0x3FFu;
            for (int u = 0; u < count && !spell_body[n]; ++u) {
                const bool own_block = !no_raised_bodies && t < 0x100u && units[u].block == static_cast<int>(t >> 4);
                if (!(cap.spell_camera && !no_spell_bodies) && !own_block) continue;
                int X, Y;
                spell_body[n] = fits(u, rx, ry, ew, eh, &X, &Y) || own_block;
            }
            if (spell_body[n]) interface = false;
        }
        // The grid cursor's corner pieces: 8x8s around its slot's own screen
        // place, 9 rows above it to 8 below. They are sorted in among the
        // units by the cursor's depth, so they sit in the unit that stands
        // on the cell's frame and in the frame of the one in front: taken by
        // those, they stood up with the units instead of lying on the cell,
        // and moved over the purple dragon in front under the stick
        // (states/CK_chevrons_ice.state). Left to no unit, the cursor code in
        // build_scene_now lays them on the cell's face.
        // (Not a snowflake: plain 8x8s too, beside the cursor they lay on
        // its cell.)
        if (cursor && ew == 8 && eh == 8 && !g_weather_piece[n]) {
            const int cx = rx >= 256 ? rx - 512 : rx, cy = ry >= 160 ? ry - 256 : ry;
            if (cx >= cursor_hx - 16 && cx + ew <= cursor_hx + 16 &&
                cy >= cursor_hy - 12 && cy + eh <= cursor_hy + 12) {
                interface = true;
                released[n] = true;
                g_cursor_piece[n] = true;
            }
        }
        // Units first; a prop (a bag) only for an entry no unit's frame
        // takes, so a stale bag slot never takes a unit's part. A bag that
        // drew something competes for the rest below.
        for (int u = 0; u < count + props && c.n < 8 && !interface; ++u) {
            if (u == count && c.n > 0) break;
            int X, Y;
            if (!fits(u, rx, ry, ew, eh, &X, &Y)) continue;
            c.u[c.n] = u; c.x[c.n] = X; c.y[c.n] = Y; ++c.n;
        }
        if (c.n == 0) {
            // Nobody's: where it was drawn, else the hardware's own reading.
            // The roster panel's portraits and brackets are drawn 72 px left
            // of their OAM place at 384 wide, with the panel; placed by OAM
            // they took the top of the pixels of a unit standing there,
            // which were then never lifted and stayed behind in the scene
            // under the stick (states/FP_slot1_dragon_cut_stick).
            if (cap.obj_drawn[i]) {
                e.x = cap.obj_x[i];
                e.y = cap.obj_y[i];
            } else {
                if (e.x >= 256) e.x -= 512;
                if (e.y >= 160) e.y -= 256;
            }
        } else if (c.n == 1) {
            e.owner = c.u[0]; e.x = c.x[0]; e.y = c.y[0];
        }
        entries[n++] = e;   // kept in OAM order: index order is draw order
    }
}

// A bag that drew something (the only fit of some entry) also competes
// for the entries a unit fits, and the rules below settle them by draw
// order. With Ivanna a cell in front of the bag, its lower half lay in
// her frame too, went to her, and stuck to her under the stick
// (states/CQ_bag_stuck_ivanna.state, OAM 19 under the bag's OAM 18).
void OamClaim::bags_compete() {
    if (props > 0) {
        bool live[kMaxProps] = {};
        for (int k = 0; k < n; ++k) {
            const Cand& c = cands[k];
            bool only_props = c.n > 0;
            for (int i = 0; i < c.n; ++i) only_props = only_props && c.u[i] >= count;
            for (int i = 0; only_props && i < c.n; ++i) live[c.u[i] - count] = true;
        }
        for (int k = 0; k < n; ++k) {
            Cand& c = cands[k];
            Entry& e = entries[k];
            if (c.n == 0 || c.u[0] >= count) continue;   // interface, or props already
            const int rx = e.a1 & 0x1FF, ry = e.a0 & 0xFF;
            const unsigned shape = (e.a0 >> 14) & 3u, size = (e.a1 >> 14) & 3u;
            const int ew = kol::kObjW[shape][size], eh = kol::kObjH[shape][size];
            const int before_n = c.n;
            for (int p = 0; p < props && c.n < 8; ++p) {
                int X, Y;
                if (!live[p] || !fits(count + p, rx, ry, ew, eh, &X, &Y)) continue;
                c.u[c.n] = count + p; c.x[c.n] = X; c.y[c.n] = Y; ++c.n;
            }
            if (c.n > before_n && before_n == 1) e.owner = -1;   // settled below
        }
    }
}

// An entry that fits more than one unit's frame: a unit's parts are
// emitted one after another, so it takes the owner of the entry before
// or after it that it adjoins (an edge shared), when that unit is one of
// its own. The front ghost's upper half and the back ghost's lower half
// lie on exactly the same 16x16 (states/CF_ghosts_overlap.state, OAM 19
// and 32), and the back one's went to the front one by column and depth.
// What is left follows the order the game emits sprites in: front to
// back (draw_depth: by x + z), each unit's parts together. So an entry goes on
// with the unit of the entry before it when that unit is one of its own,
// else to the frontmost of its units not in front of that one. A
// monster's lower half also lies in the frame of the unit a cell in
// front, and nearest-by-column-then-front gave it that unit: the
// cockatrice and the white dragon came apart under the stick
// (states/CJ_cockatrice_split.state). Nothing before it to go by: the
// nearest by column, then the front one.
void OamClaim::adjoin_owners() {
    for (bool changed = true; changed;) {
        changed = false;
        for (int k = 0; k < n; ++k) {
            Entry& e = entries[k];
            if (e.owner >= 0 || cands[k].n < 2) continue;
            for (int nb : {k - 1, k + 1}) {
                if (nb < 0 || nb >= n || entries[nb].owner < 0) continue;
                const Cand& c = cands[k];
                for (int i = 0; i < c.n; ++i) {
                    if (c.u[i] != entries[nb].owner) continue;
                    Entry t = e; t.x = c.x[i]; t.y = c.y[i];
                    if (!adjoin(t, entries[nb])) continue;
                    e = t; e.owner = c.u[i]; changed = true;
                    break;
                }
                if (e.owner >= 0) break;
            }
        }
    }
}

// A walker's blob pair (tiles 0x14B/0x14C) goes to the unit it lies
// under, whatever frame it fits: the game centres the pair one column
// right of the feet, its bottom row 1 above to 3 below them (more on a
// slope: the ground under the feet). And it is the last of that unit's
// parts in OAM: the parts drawn just before it are the unit's, and none
// after it. Walking down the gangplank in the opening's harbour scene
// (states/GY_fade_shadows.state, presents 322-348), by frames alone a
// blob drawn on the deck below its walker went to a sailor sitting there
// or to no unit (a black oval), and a walker 5 px from a sailor took the
// sailor's parts or gave it its own (too tall for a slot: drawn flat).
// KOL_NO_BLOB_FEET=1: by frames and OAM order only (A/B runs).
void OamClaim::blob_to_feet() {
    closed.assign(static_cast<std::size_t>(count), 0);
    for (int k = 0; k < n && blob_feet; ++k) {
        Entry& e = entries[k];
        if (e.w != 8 || e.h != 8 || !blob_tile(e) || ((e.a2 >> 10) & 3u) == 0u) continue;
        const bool left_half = ((e.a2 & 0x3FFu) == 0x14Bu) != ((e.a1 & 0x1000u) != 0u);
        const int cx = left_half ? e.x + 8 : e.x;
        int best = -1, best_dx = 3, best_dy = 0;
        for (int u = 0; u < count; ++u) {
            const int hx = feet_sx(units[u]) + kx;
            const int hy = feet_sy(units[u]) + ky;
            const int dx = std::abs(cx - (hx + 1)), dy = e.y + e.h - hy;
            if (dy < -2 || dy > 12 || dx > 2) continue;
            if (dx < best_dx || (dx == best_dx && std::abs(dy) < std::abs(best_dy))) {
                best = u; best_dx = dx; best_dy = dy;
            }
        }
        if (best >= 0) e.owner = best;
    }
}

// What is left by the order the game draws in (see adjoin_owners).
void OamClaim::draw_order() {
    int before = -1;   // the unit of the last entry that has one
    for (int k = 0; k < n; ++k) {
        Entry& e = entries[k];
        if (e.owner < 0 && cands[k].n >= 2) {
            // Not a unit whose blob was drawn already: its parts are done.
            Cand c = cands[k];
            if (blob_feet) {
                int m = 0;
                for (int i = 0; i < c.n; ++i)
                    if (c.u[i] >= count || !closed[c.u[i]]) {
                        c.u[m] = c.u[i]; c.x[m] = c.x[i]; c.y[m] = c.y[i]; ++m;
                    }
                if (m > 0) c.n = m; else c = cands[k];
            }
            int best = -1;
            if (before >= 0) {
                for (int i = 0; i < c.n && best < 0; ++i)
                    if (c.u[i] == before) best = i;
                if (best < 0)
                    for (int i = 0; i < c.n; ++i) {
                        const int d = units[c.u[i]].depth;
                        if (d <= units[before].depth &&
                            (best < 0 || d > units[c.u[best]].depth))
                            best = i;
                    }
            }
            if (best < 0) {
                int best_cost = 0, best_depth = 0;
                for (int i = 0; i < c.n; ++i) {
                    const int u = c.u[i];
                    const int hx = feet_sx(units[u]) + kx;
                    const int cost = std::abs(c.x[i] + e.w / 2 - hx);
                    if (best < 0 || cost < best_cost ||
                        (cost == best_cost && units[u].depth > best_depth)) {
                        best = i; best_cost = cost; best_depth = units[u].depth;
                    }
                }
            }
            e.owner = c.u[best]; e.x = c.x[best]; e.y = c.y[best];
        }
        if (blob_feet && blob_tile(e)) {
            if (e.owner >= 0 && e.owner < count) closed[e.owner] = 1;
            before = -1;
        } else if (e.owner >= 0) {
            before = e.owner;
        }
    }
}

// The parts just before a blob pair: its unit's, while they fit its
// frame and the parts together still fit a slot (32 x 32).
void OamClaim::parts_before_blob() {
    for (int k = 1; k < n && blob_feet; ++k) {
        const Entry& b = entries[k];
        const int u = b.owner;
        if (u < 0 || u >= count || !blob_tile(b) || blob_tile(entries[k - 1])) continue;
        int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
        for (int j = k - 1; j >= 0 && !blob_tile(entries[j]); --j) {
            const Cand& c = cands[j];
            int i = 0;
            while (i < c.n && c.u[i] != u) ++i;
            if (i == c.n) break;
            const int nx0 = std::min(x0, c.x[i]), ny0 = std::min(y0, c.y[i]);
            const int nx1 = std::max(x1, c.x[i] + entries[j].w), ny1 = std::max(y1, c.y[i] + entries[j].h);
            if (nx1 - nx0 > 32 || ny1 - ny0 > 32) break;
            x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
            entries[j].owner = u; entries[j].x = c.x[i]; entries[j].y = c.y[i];
        }
    }
}

// The blob parts out of every frame (see g_blob_of): to the unit whose
// part is drawn just before them, when they lie under its column and
// within 12 rows below its feet.
void OamClaim::blob_join_pass() {
    for (int k = 0; k < 128; ++k) g_blob_of[k] = -1;
    blob_join = blob_join_on();   // for A/B runs
    for (int k = 0; k < n && blob_join; ++k) {
        const Entry& e = entries[k];
        if (e.owner >= 0 || cands[k].n != 0 || !blob_tile(e) || ((e.a2 >> 10) & 3u) == 0u) continue;
        int prev = -1;
        for (int j = k - 1; j >= 0 && prev < 0; --j)
            if (entries[j].owner >= 0 && !blob_tile(entries[j])) prev = j;
            else if (g_blob_of[j] >= 0) prev = -2;   // the pair's other half: look past it
        if (prev == -2) {
            for (int j = k - 1; j >= 0; --j) if (g_blob_of[j] >= 0) { g_blob_of[k] = g_blob_of[j]; break; }
            continue;
        }
        if (prev < 0) continue;
        const int u = entries[prev].owner;
        const int hx = feet_sx(units[u]) + kx;
        const int hy = feet_sy(units[u]) + ky;
        if (e.x < hx - 16 - kSlack || e.x + e.w > hx + 16 + kSlack) continue;
        if (e.y + e.h <= hy || e.y + e.h > hy + 12) continue;
        g_blob_of[k] = u;
    }
}

// A flying unit's blob (see blob_tile): no frame held it, or a unit
// standing lower did (its feet under the blob: never its own). It goes
// to the unit whose column it lies in, its feet 12 to 128 rows above
// it, the nearest; its bottom row is the ground's row under that unit.
// A pair's half goes with the other half: the Angel Knight flying in
// over map 40's ice (JF, the user's slot 3: a round blob shadow
// that should not be there) draws the walkers' 8 x 8 pair, and the left half,
// its column a pixel short of her feet, stayed on the ice as half an
// oval; flying higher, 76 to 85 rows over it, neither half went (64
// before). KOL_FLY_BLOB_SINGLE=1: each half alone, within 64, as before.
void OamClaim::flying_blobs() {
    static const bool fly_single = std::getenv("KOL_FLY_BLOB_SINGLE") != nullptr;
    const int fly_reach = fly_single ? 65 : 129;
    g_unit_ground.assign(static_cast<std::size_t>(count), kNoGround);
    auto feet_of = [&](int u, int* hx, int* hy) {
        *hx = feet_sx(units[u]) + kx;
        *hy = feet_sy(units[u]) + ky;
    };
    for (int k = 0; k < n && blob_join; ++k) {
        Entry& e = entries[k];
        if (!blob_tile(e) || ((e.a2 >> 10) & 3u) == 0u || g_blob_of[k] >= 0) continue;
        const int by = e.y + e.h;
        int hx = 0, hy = 0;
        if (e.owner >= 0 && e.owner < count) {
            feet_of(e.owner, &hx, &hy);
            if (by >= hy - 8 && by <= hy + 12) continue;   // its own unit's
        }
        int best = -1, best_d = fly_reach;
        for (int u = 0; u < count; ++u) {
            feet_of(u, &hx, &hy);
            if (hx < e.x - 2 || hx > e.x + e.w + 2) continue;
            int d = by - hy;
            // (Under the GBA's 160 rows its Y byte reads as above the top:
            // the angel's first frames, her blob at 163 read as -93.)
            if (d < 0 && !fly_single) d += 256;
            if (d > 12 && d < best_d) { best = u; best_d = d; }
        }
        if (best < 0) continue;
        g_blob_of[k] = best;
        e.owner = -1;
        g_unit_ground[best] = 8 + 2 * ((units[best].x + units[best].z) / 4 + ky - by);
    }
    // The pair's other half: beside it on its row, next to it in OAM.
    for (int k = 0; k < n && blob_join && !fly_single; ++k) {
        Entry& e = entries[k];
        if (!blob_tile(e) || ((e.a2 >> 10) & 3u) == 0u || g_blob_of[k] >= 0) continue;
        for (const int j : {k - 1, k + 1}) {
            if (j < 0 || j >= n || g_blob_of[j] < 0 || !blob_tile(entries[j])) continue;
            const Entry& o = entries[j];
            if (o.y != e.y || o.h != e.h || o.w != e.w || std::abs(o.x - e.x) != e.w ||
                (o.a2 & 0x3FFu) == (e.a2 & 0x3FFu))
                continue;
            g_blob_of[k] = g_blob_of[j];
            e.owner = -1;
            break;
        }
    }
}

// A part that fits no unit's frame but is drawn next to a part of a unit
// (the nearest claimed entry before or after it, in OAM order), touching
// one of that unit's parts, is that unit's too, when the unit's parts
// with it still fit a sprite slot (32 x 32). The fortune teller's table
// hangs 4 rows below her feet (map 89's opening, OAM 14-15 after her
// 12-13): left to no unit, it stayed in the scene and took no light, no
// shadow and none of the candle (states/FC_slot1_candle_fortune_teller).
// Required at first to lie between two of her parts, it was lost on the
// frames the game drew Alphonse's shadow between hers and the table
// (states/FD_slot1_table_part_flicker.state, OAM 24-25 then 26-27).
// The unit's parts counted are those drawn with it: the stretch of OAM
// round `side` that holds no other unit's part. A raindrop in the door's
// frame (map 58's boat, states/HM_slot2_map58_flag, OAM 9-17) took the
// door for its owner until the run below gave it back to the interface;
// counted, it widened the door to 34 and the door's part 0x2B7 went flat
// on those frames: the door twitched in the rain. KOL_PART_JOIN_ANY=1:
// every part of the unit, as before.
// Up to kTallCard rows (two stacked slots, see the blocks): the boat's
// flag and its castle door are 32 x 40, their top part fitting no frame.
// Again until nothing joins: the door's parts join one through another.
void OamClaim::join_parts() {
    static const bool part_join = std::getenv("KOL_NO_PART_JOIN") == nullptr;   // for A/B runs
    static const bool join_any = std::getenv("KOL_PART_JOIN_ANY") != nullptr;
    for (bool changed = part_join; changed;) {
    changed = false;
    for (int k = 0; k < n; ++k) {
        Entry& e = entries[k];
        if (e.owner >= 0 || cands[k].n != 0 || ((e.a2 >> 10) & 3u) == 0u) continue;
        int prev = -1, next = -1;
        for (int j = k - 1; j >= 0 && prev < 0; --j) if (entries[j].owner >= 0) prev = j;
        for (int j = k + 1; j < n && next < 0; ++j) if (entries[j].owner >= 0) next = j;
        for (const int side : {prev, next}) {
        if (side < 0 || e.owner >= 0) continue;
        const int u = entries[side].owner;
        int lo = 0, hi = n - 1;
        if (!join_any) {
            lo = hi = side;
            while (lo > 0 && (entries[lo - 1].owner < 0 || entries[lo - 1].owner == u)) --lo;
            while (hi < n - 1 && (entries[hi + 1].owner < 0 || entries[hi + 1].owner == u)) ++hi;
        }
        // The piece's other readings too (X wraps at 512, Y at 256), for the
        // big join only: a rider's head over the top of the view read y 148
        // (-108 drawn), no part of the horse's at -92, stayed interface and was
        // drawn at 148 beside the house while the camera panned up to him
        // (map 95, states/thor/0244; the user saw the rider's sprite appear
        // during the camera pan). The reading that joins is kept.
        // (The reading as read first: what joined before joins the same.)
        static const bool no_big = std::getenv("KOL_NO_BIG_JOIN") != nullptr;
        for (const auto& rd : {std::pair<int, int>{0, 0}, {0, -256}, {0, 256}, {-512, 0}, {512, 0}}) {
        if (e.owner >= 0 || ((rd.first || rd.second) && no_big)) break;
        Entry ee = e;
        ee.x += rd.first;
        ee.y += rd.second;
        bool touches = false, touches_any = false, near_tiles = false;
        int x0 = ee.x, y0 = ee.y, x1 = ee.x + ee.w, y1 = ee.y + ee.h;
        for (int j = lo; j <= hi; ++j) {
            if (entries[j].owner != u) continue;
            const int dt = static_cast<int>(e.a2 & 0x3FFu) - static_cast<int>(entries[j].a2 & 0x3FFu);
            near_tiles = near_tiles || (dt >= -0x40 && dt <= 0x40);
            // Not its blob: split off when it does not fit the slot (the
            // walker's head left out at GY present 344, its blob 3 under).
            if (blob_feet && blob_tile(entries[j])) continue;
            touches = touches || adjoin(ee, entries[j]);
            touches_any = touches_any || touch_any(ee, entries[j]);
            x0 = std::min(x0, entries[j].x); y0 = std::min(y0, entries[j].y);
            x1 = std::max(x1, entries[j].x + entries[j].w); y1 = std::max(y1, entries[j].y + entries[j].h);
        }
        // A bigger figure (a rider on his horse, map 95, states/thor/0245: the
        // rider is a sprite of its own, 40 x 56 with the horse; his five
        // pieces, no unit's, were interface, drawn flat while the horse's
        // card moved: the rider came apart from it) joins up to kBigCard
        // when its tiles lie near the unit's (within 0x40: the menu's icons
        // are elsewhere). The atlas splits it into slots (build_sprite_atlas).
        // KOL_NO_BIG_JOIN=1: as before.
        const bool fits_slot = x1 - x0 <= kol::ScenePixels::kSpriteSlot && y1 - y0 <= kTallCard;
        // (Only past a slot: what fits one joins as before.)
        const bool fits_big = !no_big && !fits_slot && near_tiles && x1 - x0 <= kBigCard && y1 - y0 <= kBigCard;
        const bool reread = rd.first || rd.second;
        if ((!reread && touches && fits_slot) || (touches_any && fits_big)) {
            e.owner = u;
            e.x = ee.x;
            e.y = ee.y;
            changed = true;
        }
        }
        }
    }
    }
}

// The units come in one run, front to back; what the game draws before
// or after that run is interface, whatever unit's frame or box it lies in. The
// command menu's boxes and icons are priority 1 and come first: its icons
// went to the units they lay over and the menu was drawn into their
// sprites (states/CL_menu_embedded.state). The run is the longest
// front-to-back sequence of claimed entries; an entry inside it that
// breaks the order keeps its unit. "Front to back" by the units' depth
// within kRunSlack: the units' places are read a frame after the sprites
// were drawn, and the game orders two units its own way when they are
// level. A girl walking past the fortune teller (map 89's opening) came
// one unit in front of her by depth (833 against 832) on the frame her
// parts were drawn after the teller's: all four went to interface for a
// frame, and her shadow's pieces the next (states/FE_slot1_girl_flicker).
void OamClaim::front_to_back_run() {
    static const int kRunSlack = [] {
        const char* v = std::getenv("KOL_RUN_SLACK");   // for A/B runs
        return v ? std::atoi(v) : 8;
    }();
    {
        int len[128], from[128], best_end = -1;
        for (int k = 0; k < n; ++k) {
            len[k] = 0;
            from[k] = -1;
            if (entries[k].owner < 0) continue;
            len[k] = 1;
            const int d = units[entries[k].owner].depth;
            for (int j = 0; j < k; ++j)
                if (entries[j].owner >= 0 && units[entries[j].owner].depth >= d - depth_slack(kRunSlack) &&
                    len[j] + 1 > len[k]) {
                    len[k] = len[j] + 1;
                    from[k] = j;
                }
            if (best_end < 0 || len[k] > len[best_end]) best_end = k;
        }
        if (best_end >= 0) {
            // The run's members, first to last.
            int run[128], rn = 0;
            for (int k = best_end; k >= 0; k = from[k]) run[rn++] = k;
            std::reverse(run, run + rn);
            // A unit's parts are emitted together, so an end of the run cut
            // off from the next member by entries that fit no unit is not a
            // unit's part. The spell menu's hand (OAM 0, drawn first) lay in
            // Ivanna's frame and started the run; the menu's text and icons
            // after it (OAM 1-8, in no frame) then fell inside the run and
            // were never interface, and her texels under the highlighted
            // icon were drawn over it (states/CP_spell_hand_merged.state).
            auto cut_off = [&](int a, int b) {
                for (int k = a + 1; k < b; ++k)
                    if (entries[k].owner < 0 && cands[k].n == 0) return true;
                return false;
            };
            int lo = 0, hi = rn - 1;
            // Where a cut is, the span starts just past the last entry of the
            // gap that fits no unit (ends just before the first), not at the
            // run's next member: unit parts between, out of the run's depth
            // order, stay in the span. With the command menu's hand at its
            // lowest it fit unit 6's frame and started the run (0 6 11 ...);
            // cut at OAM 1-4 and 7-8, the span began at 11 and the angel's
            // own parts (OAM 9-10, unit 3) went to interface, flat, on the
            // hand's beat (states/GE_slot3_angels_flash.state).
            int span_lo = -1, span_hi = -1;
            // The same for a GROUP at the run's start (members of one unit,
            // consecutive in OAM), when that unit's parts come again after
            // the gap and interface is drawn just before the group: a unit's
            // parts are emitted together, never split by interface. The
            // command menu's third icon (OAM 5) touches its fourth (OAM 6),
            // which lies in Rictor's frame, so the two went to him together;
            // the test above saw only the empty gap between them, and the
            // menu moved with him on every frame no raindrop between the
            // icons and his parts (OAM 16-17) fell in another unit's frame
            // (states/HY_slot2_menu_sticks_rictor.state: 40 frames of 200).
            // KOL_RUN_GROUP_CUT=0: the pair test only (A/B runs).
            static const bool group_cut = kol::env_not_zero("KOL_RUN_GROUP_CUT");
            while (hi - lo >= 1) {
                if (cut_off(run[lo], run[lo + 1])) {
                    for (int k = run[lo] + 1; k < run[lo + 1]; ++k)
                        if (entries[k].owner < 0 && cands[k].n == 0) span_lo = k + 1;
                    ++lo;
                    continue;
                }
                if (!group_cut) break;
                const int u = entries[run[lo]].owner;
                int g = lo;
                while (g + 1 <= hi && entries[run[g + 1]].owner == u && run[g + 1] == run[g] + 1) ++g;
                if (g == lo || g >= hi || !cut_off(run[g], run[g + 1])) break;
                bool again = false;
                for (int k = run[g + 1]; k < n && !again; ++k) again = entries[k].owner == u;
                const int before = run[lo] - 1;
                const bool iface_before = before >= 0 && entries[before].owner < 0 && g_blob_of[before] < 0;
                if (!again || !iface_before) break;
                for (int k = run[g] + 1; k < run[g + 1]; ++k)
                    if (entries[k].owner < 0 && cands[k].n == 0) span_lo = k + 1;
                lo = g + 1;
            }
            while (hi - lo >= 1 && cut_off(run[hi - 1], run[hi])) {
                for (int k = run[hi] - 1; k > run[hi - 1]; --k)
                    if (entries[k].owner < 0 && cands[k].n == 0) span_hi = k - 1;
                --hi;
            }
            // For the same reason, the run's first entries are not a unit's
            // when that unit's parts come again later in the run, after
            // another unit's, and interface is drawn just before them. The
            // command menu's third and fourth icons (OAM 6-7, after its
            // other icons and text) lay in the siren's frame, her depth level
            // with the unit drawn next: they started the run, went to her and
            // moved with her under the stick (states/FH_slot1_siren_icons;
            // her own parts are OAM 10-11, after unit 8's 8-9).
            if (hi - lo >= 2) {
                const int u = entries[run[lo]].owner;
                int g = lo;   // the group: run members of u, consecutive in OAM
                while (g + 1 <= hi && entries[run[g + 1]].owner == u && run[g + 1] == run[g] + 1) ++g;
                bool other = false, again = false;   // (over every entry: the run skips some)
                for (int k = run[g] + 1; k < n && !again; ++k) {
                    const int o = entries[k].owner;
                    if (o >= 0 && o != u) other = true;
                    else if (o == u && other) again = true;
                }
                const int before = run[lo] - 1;
                const bool iface_before = before >= 0 && entries[before].owner < 0 && g_blob_of[before] < 0;
                if (again && iface_before) {   // (only the group: what follows keeps its units)
                    for (int i = lo; i <= g; ++i) {
                        entries[run[i]].owner = -1;
                        released[run[i]] = true;
                    }
                    // And the interface drawn between it and the run's next
                    // member, which fits no unit: still inside the run's
                    // span, it was neither a unit's nor interface, so it was
                    // laid on the map under the units. The command menu's
                    // fourth and fifth icons (OAM 7-8, after OAM 5-6 in unit
                    // 6's frame and before unit 3's parts) went under the
                    // angel standing over them, and came and went with the
                    // hand's bob (states/GE_slot3_angels_flash.state).
                    if (g + 1 <= hi)
                        for (int k = run[g] + 1; k < run[g + 1]; ++k)
                            if (entries[k].owner < 0 && cands[k].n == 0 && g_blob_of[k] < 0)
                                released[k] = true;
                }
            }
            const int first = span_lo >= 0 && span_lo <= run[lo] ? span_lo : run[lo];
            best_end = span_hi >= run[hi] ? span_hi : run[hi];

            for (int k = 0; k < n; ++k) {
                if (k >= first && k <= best_end) continue;
                if (g_blob_of[k] >= 0) continue;   // a unit's blob (above)
                entries[k].owner = -1;
                released[k] = true;
            }
        }
    }
}

// The sky units' parts, flat. Which unit a part is goes by its tile block
// (16 tiles: the game keeps a unit's frames there) as much as by frames:
// a part fitting a sky unit's frame and a ground unit's is the sky's
// when its block last fit only a sky unit (and the ground unit's when
// it last fit only a ground one), and a wing out of any frame is the
// sky's when its block was, within the last second.
void OamClaim::sky_units() {
    static const bool sky_on = std::getenv("KOL_NO_SKY_UNITS") == nullptr;
    static std::int8_t blk_sky[64];
    static std::uint32_t blk_at[64];
    static bool blk_init = false;
    if (!blk_init) { std::fill(blk_sky, blk_sky + 64, -1); blk_init = true; }
    const std::uint32_t now = cap.serial;
    // (With none in the sky, none: the next scene's units take the gulls'
    // tile blocks within the second.)
    bool any_sky = false;
    for (int u = 0; u < count && u < 64; ++u) any_sky = any_sky || g_unit_sky[u];
    if (!any_sky) std::fill(blk_sky, blk_sky + 64, -1);
    for (int k = 0; k < n && sky_on && any_sky; ++k) {
        Entry& e = entries[k];
        const int blk = static_cast<int>((e.a2 & 0x3FFu) >> 4);
        const Cand& c = cands[k];
        int sky_c = 0, ground_c = 0;
        for (int i = 0; i < c.n; ++i)
            if (c.u[i] < count) (g_unit_sky[c.u[i]] ? sky_c : ground_c)++;
        const bool recent = blk_sky[blk] >= 0 && now - blk_at[blk] < 60u;
        bool sky = false;
        if (sky_c && !ground_c) sky = true;
        else if (sky_c && ground_c) sky = recent ? blk_sky[blk] == 1 : (e.owner >= 0 && g_unit_sky[e.owner]);
        else if (c.n == 0) sky = recent && blk_sky[blk] == 1;
        // (Sky sticks for that second: a wing over a sailor fits his frame
        // alone, and that is no reason to give the gull to him.)
        if (sky_c && !ground_c) { blk_sky[blk] = 1; blk_at[blk] = now; }
        else if (c.n > 0 && !sky_c && !(recent && blk_sky[blk] == 1)) { blk_sky[blk] = 0; blk_at[blk] = now; }
        if (!sky && recent && blk_sky[blk] == 1 && ground_c && !sky_c) sky = true;
        if (sky) {
            e.owner = -1;
            released[k] = true;
        }
    }
}

// Last, the game's own word on whose sprite is whose: its tile block.
// A unit's frames are drawn from its actor's slot in the game's sprite
// pool (map object +0x08; the pool at [0x02026F08] + 0x2020, 0x3C8 a
// slot): the pool's VRAM callback 0x08059000, in its fixed-slot mode,
// puts slot i (0-15) at 0x06010000 + i * 0x200, the 16 OBJ tiles from
// i * 0x10, and the OAM builder 0x03005758 adds the frame's piece offsets
// (+0, +4 for the two 16x16 halves; a monster's 32x32 is the whole 16).
// The battle makes each object with its actor, both the first free, so
// the slot is the object's index on every one of 1143 objects in
// states/. The same blocks the sky units go by above. Checked over 26
// sprite states (BL,
// CJ, CF, CQ, CP, CL, CH, CK, CB, EM, B_rictor, DE, CN, BW, DM, AB, BO,
// HY, IB, FF, FH, HM, GY, HP, GD, IG), 450 frames each: no entry below
// tile 0x100 was ever a unit's other than its block's. Everything the
// frames above get wrong came from geometry: a command menu icon (tiles
// 0x1D7-0x203) lying in the frame of the unit above it as that unit's
// idle frame dips (IG, the user's slot 2, 103 of 450 frames; FH's siren
// case, 449; HY's before its fix), a raindrop (0x165) in a unit's frame
// (HY, B_rictor). So:
// - an entry in a battle unit's block (not at priority 0, which is the
//   interface's, nor a blob) is that unit's;
// - a battle unit whose frames are in its block (this frame) owns nothing
//   outside it, except while an attack or a spell plays, when the
//   effects and the GUARD letters over it go with it on purpose (FF,
//   GD, IB, BL, BW);
// - a scene actor drawn from tiles of its own (the boat's flag and
//   door, the sailors, the town's archers: HM, GY, HP), and the bags,
//   keep the frames' answer, the only one there is.
// KOL_NO_TILE_OWNERS=1: the frames' answer alone, as before.
void OamClaim::owners_by_tile_block() {
    static const bool tile_owners = std::getenv("KOL_NO_TILE_OWNERS") == nullptr;
    if (tile_owners) {
        int unit_of_block[16];
        std::fill(unit_of_block, unit_of_block + 16, -1);
        for (int u = 0; u < count; ++u)
            if (units[u].block >= 0) unit_of_block[units[u].block] = u;
        bool in_block[kObjects] = {};
        for (int k = 0; k < n; ++k) {
            Entry& e = entries[k];
            const unsigned t = e.a2 & 0x3FFu;
            if (t >= 0x100u || blob_tile(e)) continue;
            if (((e.a2 >> 10) & 3u) == 0u && !spell_body[k]) continue;
            const int u = unit_of_block[t >> 4];
            if (u < 0) continue;
            in_block[u] = true;
            if (e.owner == u) continue;
            int X, Y;
            // Where it was drawn when no reading fits the frame: map 67's
            // chain (unit 2, six 8x16s from its shackles up past the view's
            // top) has its top piece at Y 221, drawn at -35 in the wide view;
            // the reading nearest its feet kept 221, and the piece's pixels,
            // left to no one, floated off the chain (states/thor/0053, the
            // user saw a chain sprite floating at the top edge).
            if (!fits(u, e.a1 & 0x1FF, e.a0 & 0xFF, e.w, e.h, &X, &Y) && e.oam >= 0 && cap.obj_drawn[e.oam]) {
                X = cap.obj_x[e.oam];
                Y = cap.obj_y[e.oam];
            }
            e.owner = u;
            e.x = X;
            e.y = Y;
            released[k] = false;
        }
        const bool overlays = cap.combat_camera || cap.spell_camera;
        for (int k = 0; k < n && !overlays; ++k) {
            Entry& e = entries[k];
            if (e.owner < 0 || e.owner >= count || !in_block[e.owner]) continue;
            if ((e.a2 & 0x3FFu) < 0x100u || blob_tile(e) || g_blob_of[k] >= 0) continue;
            e.owner = -1;
            released[k] = true;
            // Where it was drawn, as for an entry nobody's frame takes.
            if (e.oam >= 0 && cap.obj_drawn[e.oam]) {
                e.x = cap.obj_x[e.oam];
                e.y = cap.obj_y[e.oam];
            } else {
                e.x = e.a1 & 0x1FF;
                e.y = e.a0 & 0xFF;
                if (e.x >= 256) e.x -= 512;
                if (e.y >= 160) e.y -= 256;
            }
        }
    }
}

// A scene object's own sprite (kSceneObjects: map 58's door opening, 32 x
// 40 left of its feet): no frame fits it and its tiles are no actor's
// block, so its pieces went to no one, drawn flat as interface, the art's
// colours bright in the dark (the user, 2026-10-26, states/JM ~880; the
// walker into it stood on its place). A piece nobody took in a box round
// the object's feet, not the weather's nor the interface's priority, is
// the object's. KOL_NO_SCENE_OBJECT_CLAIM=1: as before.
void OamClaim::scene_object_claim() {
    static const bool object_claim = std::getenv("KOL_NO_SCENE_OBJECT_CLAIM") == nullptr;
    for (int u = 0; u < count && object_claim; ++u) {
        if (!scene_object(units[u])) continue;
        const int fx = feet_sx(units[u]) + kx, fy = feet_sy(units[u]) + ky;
        for (int k = 0; k < n; ++k) {
            Entry& e = entries[k];
            if (e.owner >= 0 || weather_tile(e.a2) || ((e.a2 >> 10) & 3u) == 0u) continue;
            if (e.x < fx - 48 || e.x + e.w > fx + 48 || e.y < fy - 64 || e.y + e.h > fy + 8) continue;
            e.owner = u;
            released[k] = false;
        }
    }
}

// People seated in chairs (kSeatedMaps): the chair is a unit drawn from its own tile
// block, and the person in it another sprite the game draws there, from tiles of its
// own, whose object stands elsewhere (map 101, the intro's castle room: the two at
// the table, objects 0 and 1 at X 155 under the table, their sprites up-left at the
// chairs, units 2 and 3 at X 119). The block rule gives a block unit nothing outside
// its block, so the people went to no one and were drawn flat with the interface:
// they stayed where they were as the camera moved (states/thor/0296, the user
// asked for both anchored at the table as the camera moves). A piece
// nobody took, not the interface's priority nor the weather's, that fits a block
// unit's frame is that unit's: the person goes with the chair.
// KOL_NO_SEATED_CLAIM=1: as before.
void OamClaim::seated_claim() {
    static const bool off = std::getenv("KOL_NO_SEATED_CLAIM") != nullptr;
    if (off || !by_map(kSeatedMaps, current_map())) return;
    for (int k = 0; k < n; ++k) {
        Entry& e = entries[k];
        if (e.owner >= 0 || weather_tile(e.a2) || ((e.a2 >> 10) & 3u) == 0u || g_blob_of[k] >= 0) continue;
        for (int u = 0; u < count; ++u) {
            int X, Y;
            if (units[u].block < 0 || !fits(u, e.a1 & 0x1FF, e.a0 & 0xFF, e.w, e.h, &X, &Y)) continue;
            e.owner = u;
            e.x = X;
            e.y = Y;
            released[k] = false;
            break;
        }
    }
}

// The command menu's icons (tiles 0x1D7-0x203: Attack, Item, Spell,
// Standby) are no unit's: one that no unit owns by its tile block is
// interface. The run above releases the entries outside the units' span,
// but an icon among them kept the box's answer: states/thor/0087, Standby
// (#7, at 136,104) under the griffin's lower left part (#22) took the
// griffin's place and stood with it, its corner under the griffin's claw
// as it bobbed (the user saw part of the icon carried by the dragon).
// KOL_MENU_ICONS_BOXED=1: as before.
void OamClaim::menu_icons_free() {
    static const bool boxed = std::getenv("KOL_MENU_ICONS_BOXED") != nullptr;
    for (int k = 0; k < n && !boxed; ++k) {
        Entry& e = entries[k];
        const unsigned t = e.a2 & 0x3FFu;
        if (e.owner >= 0 || g_blob_of[k] >= 0 || t < 0x1D7u || t > 0x203u) continue;
        released[k] = true;
    }
}

// KOL_OAM_DEBUG=1: the units' feet, and every plain OAM entry with the
// unit it went to (-1: none) and the units whose frame it fits, a frame.
void OamClaim::debug_dump() {
    static const bool oam_debug = std::getenv("KOL_OAM_DEBUG") != nullptr;
    if (oam_debug) {
        for (int u = 0; u < count; ++u)
            std::fprintf(stderr, "[oam] unit %d xyz=(%d,%d,%d) feet=(%d,%d)\n", u, units[u].x,
                         units[u].y, units[u].z, feet_sx(units[u]) + kx,
                         feet_sy(units[u]) + ky);
        for (int k = 0; k < n; ++k) {
            const Entry& e = entries[k];
            if (e.x == 248 && e.y == -88) continue;   // parked
            std::fprintf(stderr, "[oam] #%d (%d,%d) %dx%d tile=0x%03X pal=%u prio=%u mode=%u owner=%d blob=%d%s fits:", k, e.x,
                         e.y, e.w, e.h, e.a2 & 0x3FFu, (e.a2 >> 12) & 15u, (e.a2 >> 10) & 3u, (e.a0 >> 10) & 3u, e.owner, g_blob_of[k],
                         released[k] ? " iface" : "");
            for (int i = 0; i < cands[k].n; ++i) std::fprintf(stderr, " %d", cands[k].u[i]);
            std::fprintf(stderr, "\n");
        }
    }
}

// Each sprite pixel's top entry (g_obj_top, as the hardware finds it), and
// the place, kind and interface of the pixels the units' entries drew;
// KOL_FOCUS_OBJ_STATS=1 prints how (below).
void OamClaim::pixel_owners() {
    static const bool stats = std::getenv("KOL_FOCUS_OBJ_STATS") != nullptr;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    g_obj_top.assign(pixels, -1);
    g_obj_prio.assign(pixels, 0xFF);
    g_obj_top_valid = true;
    for (int k = 0; k < n; ++k) {
        const Entry& e = entries[k];
        const unsigned prio = (e.a2 >> 10) & 3u;
        const bool bpp8 = (e.a0 & 0x2000u) != 0u;
        const bool hflip = (e.a1 & 0x1000u) != 0u, vflip = (e.a1 & 0x2000u) != 0u;
        const unsigned tile = e.a2 & 0x3FFu;
        const unsigned row_tiles = map_1d ? static_cast<unsigned>(e.w / 8) * (bpp8 ? 2u : 1u) : 32u;
        for (int ty = 0; ty < e.h; ++ty) {
            const int py = e.y + ty + oy;
            if (py < 0 || py >= h) continue;
            const int sy = vflip ? e.h - 1 - ty : ty;
            for (int tx = 0; tx < e.w; ++tx) {
                const int px = e.x + tx + ox;
                if (px < 0 || px >= w) continue;
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                if (scene_top(layers, p) != kol::kLayerObj) continue;
                if (g_obj_prio[p] <= prio) continue;   // an earlier entry wins ties
                const int sx = hflip ? e.w - 1 - tx : tx;
                if (!kol::obj_tile_texel(tiles, tile, row_tiles, sx, sy, bpp8)) continue;
                g_obj_top[p] = static_cast<std::int16_t>(k);
                g_obj_prio[p] = static_cast<std::uint8_t>(prio);
            }
        }
    }
    // A sprite pixel with no opaque texel under it (none expected from the
    // latched sprites; see KOL_FOCUS_OBJ_STATS) goes to the unit part on top
    // whose rectangle covers it.
    long by_rect = 0;
    for (int k = 0; k < n; ++k) {
        const Entry& e = entries[k];
        if (e.owner < 0) continue;
        for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py) {
            for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                if (scene_top(layers, p) != kol::kLayerObj || g_obj_top[p] >= 0) continue;
                g_obj_top[p] = static_cast<std::int16_t>(k);   // first (top) part wins
                ++by_rect;
            }
        }
    }
    // Each pixel whose top entry is a unit's goes to that unit. Only the
    // units' own rectangles can hold one.
    // KOL_FOCUS_OBJ_STATS=1 prints, a frame: the sprite pixels, those a unit
    // drew, those found by rectangle only, those no entry accounts for (zero
    // means the OAM read here is the one the frame was drawn from; the rain's
    // margin copies are not in OAM), and those the box had given elsewhere.
    long changed = 0;
    for (int k = 0; k < n; ++k) {
        const Entry& e = entries[k];
        if (e.owner < 0) continue;
        for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py) {
            for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                const int top = g_obj_top[p];
                if (top < 0 || entries[top].owner < 0) continue;
                const Unit& u = units[entries[top].owner];
                if (stats && (xz_out[p] != u.x + u.z || y_out[p] != u.y || iface_out[p]))
                    ++changed;
                xz_out[p] = u.x + u.z;
                y_out[p] = u.y;
                kind_out[p] = kol::kSceneUpright;
                iface_out[p] = 0;
            }
        }
    }
    // What a priority-0 entry drew is interface, whatever the box said: it
    // stays flat and over the units, as the game layers it. (The box gave
    // the Attack/Cancel hand to the unit it points at, and that unit's whole
    // sprite was drawn over it: CH_menu_hand.)
    bool any_iface = false;
    for (int k = 0; k < n; ++k) {
        const Entry& e = entries[k];
        if (((e.a2 >> 10) & 3u) != 0u && !released[k]) continue;
        if (spell_body[k] && e.owner >= 0) continue;
        any_iface = true;
        for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py)
            for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                if (g_obj_top[p] == k) iface_out[p] = 1;
            }
    }
    // A unit's blob out of its frame lies on the ground where it is drawn:
    // the ground's place (g_under_*, kept for sprite pixels), not the unit's
    // nor interface. (This pass sat inside the loop above by a misplaced
    // brace until 2026-11-02, so with no priority-0 entry on screen it never
    // ran and the blob stood with its unit. KOL_BLOB_NEEDS_IFACE=1: so.)
    static const bool blob_needs_iface = std::getenv("KOL_BLOB_NEEDS_IFACE") != nullptr;
    const bool have_under = g_under_xz.size() == pixels;
    // On a sea map (kSeaPlanes, the sea on BG2 in the scene) an entry no unit
    // owns, drawn at priority 2 or under (with the boat's BG0, not over it as
    // the interface is), lies where it is drawn as the blob does: map 117's
    // wake and bow foam (tiles 0x2F0-0x338, priority 2), on the sea's plane
    // or the hull's foot under it, so it stays with the boat as the camera
    // moves (the user, 2026-11-11, reported the water effect at the bow not
    // staying with the boat). KOL_SEA_SPRITES_FLAT=1: interface, as before.
    static const bool sea_sprites_flat = std::getenv("KOL_SEA_SPRITES_FLAT") != nullptr;
    const bool sea_decals = !sea_sprites_flat && !g_cap.sea_px.empty() && sea_plane_y(g_cap.map) >= 0;
    g_sea_decal_any = false;
    if (sea_decals) g_sea_decal_px.assign(pixels, 0);
    for (int k = 0; k < n && (any_iface || !blob_needs_iface || sea_decals); ++k) {
        const Entry& e = entries[k];
        const bool sea_decal = sea_decals && e.owner < 0 && ((e.a2 >> 10) & 3u) >= 2u;
        if (g_blob_of[k] < 0 && !sea_decal) continue;
        if (g_blob_of[k] >= 0 && !(any_iface || !blob_needs_iface)) continue;
        for (int py = std::max(0, e.y + oy); py < std::min(h, e.y + oy + e.h); ++py)
            for (int px = std::max(0, e.x + ox); px < std::min(w, e.x + ox + e.w); ++px) {
                const std::size_t p = static_cast<std::size_t>(py) * w + px;
                if (g_obj_top[p] != k) continue;
                iface_out[p] = 0;
                if (sea_decal && g_blob_of[k] < 0) {
                    g_sea_decal_px[p] = 1;
                    g_sea_decal_any = true;
                }
                if (have_under) {
                    xz_out[p] = g_under_xz[p];
                    y_out[p] = g_under_y[p];
                    kind_out[p] = g_under_kind[p];
                }
            }
    }
    if (stats) {
        const double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - t0).count();
        long obj = 0, unexplained = 0, owned = 0;
        for (std::size_t p = 0; p < pixels; ++p) {
            if (scene_top(layers, p) != kol::kLayerObj) continue;
            ++obj;
            if (g_obj_top[p] < 0) ++unexplained;
            else if (entries[g_obj_top[p]].owner >= 0) ++owned;
        }
        std::fprintf(stderr,
                     "[focus-obj] sprite=%ld units=%ld by_rect=%ld unexplained=%ld changed=%ld us=%.0f\n",
                     obj, owned, by_rect, unexplained, changed, us);
    }
}

void OamClaim::run() {
    t0 = std::chrono::steady_clock::now();
    // With no unit too: the cursor box reads which entry drew each pixel
    // (g_obj_top). It returned here with none, and the box's rows decided
    // (states/thor/0099; build_scene_now).
    if (!cap.valid) return;
    oam = cap.obj_ok ? cap.oam.data() : nullptr;
    tiles = cap.obj_ok ? cap.obj_tiles.data() : nullptr;
    if (!oam || !tiles) return;
    map_1d = (cap.dispcnt & 0x40u) != 0u;
    g_map_1d = map_1d;
    g_entry_n = 0;
    // A scene object (a door, a flag: kSceneObjects) has a frame twice as
    // wide and 48 tall: map 3's church door is a 32 x 32 from its feet's
    // column to 16 right of it, 4 over a unit's frame, so three of its four
    // pieces fit no frame, were interface, and were drawn over Eleanor
    // stepping out in front of it (states/thor/0013; the user saw the door
    // drawn over her). KOL_SCENERY_UNIT_FRAME=1: a unit's frame.
    static const bool scenery_unit_frame = std::getenv("KOL_SCENERY_UNIT_FRAME") != nullptr;
    for (int u = 0; u < count && u < 64; ++u) wide_frame[u] = !scenery_unit_frame && scene_object(units[u]);
    decode();
    bags_compete();
    adjoin_owners();
    blob_to_feet();
    draw_order();
    parts_before_blob();
    blob_join_pass();
    flying_blobs();
    join_parts();
    front_to_back_run();
    sky_units();
    owners_by_tile_block();
    scene_object_claim();
    seated_claim();
    menu_icons_free();
    g_entry_n = n;
    debug_dump();
    pixel_owners();
}
}  // namespace

namespace kol_focus_detail {
// Every unit's sprite pixels, from what the hardware drew rather than a box.
// A human is two 16x16 parts, 16 px wide; a monster (dragon, griffin) is four,
// 32 px wide, from its X - 16 to its X + 16 (states/CB_sprites_broken.state:
// units 1-4 and 7). A fixed box 12 px either side of the feet left a
// monster's outer columns flat, so the perspective tore the dragon down the
// middle and the griffin's tail off.
//
// An OAM entry belongs to the unit whose sprite frame, 32 x 32 standing on its
// feet (2 px of slack), holds it whole; the nearest unit by column when two
// frames do, then the front one. At each sprite pixel the entry on top is
// found as the hardware finds it (lower priority number, then lower index),
// over every entry, from its own texels, so a unit takes only what it drew
// there: a unit's leg that the box of the unit in front used to take is its
// own now (BO, BK). A pixel whose top entry belongs to no unit keeps the box's
// answer (menus, the cursor, the rain's margin copies).
//
// The OAM and tiles are the ones the frame was drawn with, latched by
// gbarecomp (g_ppu_record_obj). Live memory at present already holds the
// next frame's: the game rewrites both in VBlank. Read live, 2% of the sprite
// pixels had no texel under them at rest (a pose ahead), and 1,300-1,800 a
// frame while the camera panned on the enemy turn (a scroll step ahead;
// states/BL_enemy_turn.state).
void claim_unit_sprites(const std::uint8_t* layers, int w, int h, int ox, int oy,
                        int kx, int ky, const Unit* units, int count, int props,
                        bool cursor, int cursor_hx, int cursor_hy,
                        std::int32_t* xz_out, std::int32_t* y_out,
                        std::uint8_t* kind_out, std::uint8_t* iface_out) {
    OamClaim{layers, w, h, ox, oy, kx, ky, units, count, props, cursor, cursor_hx, cursor_hy,
             xz_out, y_out, kind_out, iface_out}.run();
}
}  // namespace kol_focus_detail
