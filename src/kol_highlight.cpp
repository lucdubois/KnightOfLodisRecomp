// kol_highlight.cpp — see-through movement-range highlight. See the header.
//
// HOW A HIGHLIGHT CELL IS RECOGNISED
//
// Colour index 1 is reserved as the highlight colour across the terrain palette
// banks: banks 0-3 all hold the same bright blue there, and no ordinary terrain
// tile uses it. Measured on both reference states by diffing the tilemap with
// and without the range displayed:
//
//   S_move_highlight : 28 highlight tiles, 28 tiles containing index 1,
//                      sets identical — no false positives, no misses.
//   T_move_highlight2: 64 highlight tiles, 54 containing index 1 — every one a
//                      genuine highlight, 10 missed.
//
// So the test never mistakes terrain for a highlight, which is the direction
// that matters: a false positive would blend away real scenery. The 10 misses
// in T are cells whose replacement tile carries no flat blue at all (14 cells
// of 164), so they are not the opaque slabs this is here to fix and they render
// unchanged. In neither state does an ORIGINAL tile contain index 1, so a
// recovered tile is never re-detected as a highlight.
//
// TWO SHADOWS: ONE PER WORLD CELL, ONE PER RING SLOT
//
// The world shadow is the one that is used. The guest keeps the whole
// battlefield composed in EWRAM (kol_map_source.cpp), and the highlight is
// painted into that array as well as into VRAM, so every cell of the map —
// on screen or not — can be seen clean the moment the battle is up, before
// any range is opened. It is kept per WORLD cell (array row and column), and
// the renderer's sample seam (g_ws_bg_underlay_sample_provider) places each
// screen pixel on its world cell with the same mapping the margins use. That
// is what keeps the range see-through while the map scrolls: a cell scrolled
// in under an open range was captured long before, off screen.
//
// The ring shadow is the older design, kept as the fallback for frames the
// composed map is not armed on. It is keyed on VRAM tilemap slots, so it has
// to drop a slot the moment it leaves the window (the slot gets recycled for
// different terrain), which is exactly why the range used to go opaque while
// scrolling. It also cannot name a world cell at 270 lines, where 34 tile
// rows are on screen against a 32-row ring.
//
// WHEN IT FALLS BACK
//
// A cell only has a usable shadow if we have seen it un-highlighted since the
// battle came up or the last state load. So one case keeps the game's opaque
// look, by design rather than by accident: a save state loaded straight into
// an active highlight. Nothing in that battle has been observed clean yet, and
// the originals exist in no guest memory to recover them from (README "Map
// mirror at 0x02008440"). Cancelling the range once repairs it.
//
// Both shadows are dropped on every state load (g_runtime_state_epoch) and on
// leaving the battle map. A stale shadow would blend the WRONG scenery into
// the map, and being confidently wrong is a worse failure than staying opaque.

#include "kol_highlight.h"
#include "kol_map_source.h"   // the composed map the world shadow is read from
#include "kol_widescreen.h"   // is_battle_scene

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime_arm.h"          // g_runtime_state_epoch
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"

namespace kol {
namespace {

constexpr int kMapW = 64;   // BG0 is size code 1: 64x32 tiles
constexpr int kMapH = 32;
constexpr std::uint16_t kNoShadow = 0xFFFFu;

// The highlight is painted on both scrolling terrain layers, so each keeps its
// own shadow. BG0 and BG1 share geometry (64x32, char base 0) but not screen
// base, and a cell highlighted on one may be ordinary terrain on the other.
constexpr int kLayers = 2;
std::uint16_t g_shadow[kLayers][kMapH][kMapW];
bool g_installed = false;
bool g_enabled = true;
// Whether colour index 1 is the highlight's this frame: the four terrain
// banks hold one colour there. In a battle (and its preparation) the game
// writes its animated highlight colour to index 1 of banks 0-3 together
// (live PAL RAM, eight battle states: 0x7ED6, 0x7DEF, 0x7C42, 0x7FBD, 0x7DAD,
// 0x7CA5, 0x14DF, and 0x03E0 with no range up), and no terrain tile uses the
// index. A story scene's map need not keep it: map 84's ice cliff has 0x03E0,
// 0, 0, 0x24E6 there and draws its art with bank 3's, a dark navy (474 of
// BG1's cells, every crystal's outline, and 240 of BG0's). Those cells read
// as a range's, and the lighting spared their index-1 pixels as the range's
// own (kol_focus range_keep): pale dashes in the shape of the crystals'
// outlines over the shaded stairs behind them (states/thor/0183, the user
// saw the crystals' outline on the stairs). While the banks
// disagree there is no highlight colour: nothing is recorded, blended or
// handed out as lit. KOL_HIGHLIGHT_ANY_PALETTE=1: as before.
bool g_reserved = true;
int g_stat_shadowed = 0;
int g_stat_fallback = 0;

// Per-frame memo for "does this tile's art use the highlight colour index".
// Reset every frame because the game streams tile art in by DMA.
std::int8_t g_has_ink[1024];

const std::uint8_t* g_vram = nullptr;
std::uint32_t g_char_base = 0;

// The world shadow: one entry per composed-map cell, and a copy of the array
// as last read, so a frame only classifies the cells that changed. 256 rows is
// kol_map_source's own bound on the array.
constexpr int kWorldCols = 128;
constexpr int kWorldMaxRows = 256;
std::uint16_t g_world[kLayers][kWorldMaxRows][kWorldCols];
std::uint16_t g_world_seen[kLayers][kWorldMaxRows][kWorldCols];
// Each cell wearing the highlight now: which of its 8x8 pixels are in the
// highlight's colour (bit y * 8 + x, as drawn: the entry's flips applied),
// 0 for none. The range's own pixels, for the lighting to spare (kol_focus);
// a whole tile spared lit the floor beside the range's edge too.
std::uint64_t g_world_lit[kLayers][kWorldMaxRows][kWorldCols];
// How many cells of g_world_lit are non-zero: with none, there is no range
// to hand out (and nothing for kol_focus to copy and scan every frame).
int g_world_lit_cells[kLayers] = {0, 0};
// The cells last read wearing the highlight's colour, looked at again every
// frame: a cell is read when its array entry changes, but its tile's ART
// can change under the same entry. Back from the unit sheets (B on the map,
// B again) the array is read the first frame the battle is up, while some
// terrain tiles still hold the window's art, which uses the highlight's
// index: 950 terrain cells were taken for highlight and never recorded,
// and a range opened over them stayed opaque (states/thor/0104).
// KOL_HIGHLIGHT_NO_RECHECK=1: read only when the entry changes, as before.
std::vector<std::uint32_t> g_world_inked[kLayers];
std::uint8_t g_world_in_list[kLayers][kWorldMaxRows][kWorldCols];

std::uint64_t ink_mask(std::uint16_t entry) {
    const std::uint32_t base = g_char_base + (entry & 0x3FFu) * 32u;
    if (!g_vram || base + 32u > 96u * 1024u) return 0;
    const unsigned want = static_cast<unsigned>(gba::g_ws_bg_underlay_index) & 0x0Fu;
    std::uint64_t m = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t b = g_vram[base + y * 4 + (x >> 1)];
            if (((x & 1) ? (b >> 4) : (b & 0x0Fu)) != want) continue;
            const int sx = (entry & 0x400u) ? 7 - x : x, sy = (entry & 0x800u) ? 7 - y : y;
            m |= std::uint64_t{1} << (sy * 8 + sx);
        }
    return m;
}
const std::uint8_t* g_world_array[kLayers] = {nullptr, nullptr};
int g_world_rows[kLayers] = {0, 0};
bool g_world_on = true;
unsigned long long g_epoch = 0;
int g_stat_world_fallback = 0;

// Live IO page, for the ring fallback's tile coordinates.
const std::uint8_t* g_io = nullptr;

int g_clear_layer = 0;

void clear_col(int tx) {
    for (int ty = 0; ty < kMapH; ++ty) g_shadow[g_clear_layer][ty][tx] = kNoShadow;
}
void clear_row(int ty) {
    for (int tx = 0; tx < kMapW; ++tx) g_shadow[g_clear_layer][ty][tx] = kNoShadow;
}
// Window [from, from+span-1] becomes [to, to+span-1]. Scrolling forward by d
// drops [from, from+d-1]; scrolling back by d drops [from+d, ...] reduced
// modulo span. A jump of a whole span or more drops everything.
void recycle(int from, int to, int span, void (*clear)(int)) {
    int d = to - from;
    if (d >= span || d <= -span) { for (int i = 0; i < span; ++i) clear(i); return; }
    const int n = (d > 0) ? d : -d;
    for (int k = 0; k < n; ++k) {
        const int idx = (d > 0) ? (from + k) : (from + d + k);
        clear(((idx % span) + span) % span);
    }
}

bool tile_has_ink(std::uint32_t tile) {
    if (tile >= 1024u || !g_vram) return false;
    if (g_has_ink[tile]) return g_has_ink[tile] > 0;
    const std::uint32_t base = g_char_base + tile * 32u;
    bool found = false;
    if (base + 32u <= 96u * 1024u) {
        const unsigned want =
            static_cast<unsigned>(gba::g_ws_bg_underlay_index) & 0x0Fu;
        for (std::uint32_t i = 0; i < 32u && !found; ++i) {
            const std::uint8_t b = g_vram[base + i];
            found = ((b & 0x0Fu) == want) || ((b >> 4) == want);
        }
    }
    g_has_ink[tile] = found ? 1 : -1;
    return found;
}

int g_stat_calls = 0;
int g_stat_inked = 0;

extern "C" int kol_bg_underlay(int bg, int tile_x, int tile_y,
                               std::uint16_t entry, std::uint16_t* out_entry) {
    (void)entry;
    ++g_stat_calls;
    if (bg < 0 || bg >= kLayers || !g_enabled) return 0;
    if (tile_x < 0 || tile_x >= kMapW || tile_y < 0 || tile_y >= kMapH)
        return 0;
    const std::uint16_t s = g_shadow[bg][tile_y][tile_x];
    if (s == kNoShadow) { ++g_stat_fallback; return 0; }
    *out_entry = s;
    return 1;
}

void reset_world(int b) {
    for (int y = 0; y < kWorldMaxRows; ++y)
        for (int x = 0; x < kWorldCols; ++x) g_world[b][y][x] = kNoShadow;
    std::memset(g_world_lit[b], 0, sizeof g_world_lit[b]);
    g_world_inked[b].clear();
    std::memset(g_world_in_list[b], 0, sizeof g_world_in_list[b]);
    g_world_lit_cells[b] = 0;
    g_world_array[b] = nullptr;
    g_world_rows[b] = 0;
}

void reset_all() {
    for (int b = 0; b < kLayers; ++b) {
        for (int y = 0; y < kMapH; ++y)
            for (int x = 0; x < kMapW; ++x) g_shadow[b][y][x] = kNoShadow;
        reset_world(b);
    }
}

// The renderer's sample seam. Places the screen sample on its world cell by
// the margins' own mapping and answers from the world shadow; when the
// composed map is not armed this frame, answers from the ring shadow at the
// wrapped tile the renderer would have passed the tile-keyed seam.
extern "C" int kol_bg_underlay_sample(int bg, int sample_hx, int sample_y,
                                      std::uint16_t entry,
                                      std::uint16_t* out_entry) {
    if (bg < 0 || bg >= kLayers || !g_enabled) return 0;
    int row = 0, col = 0;
    if (map_source_world_cell(bg, sample_hx, sample_y, &row, &col)) {
        ++g_stat_calls;
        const std::uint16_t s = g_world[bg][row][col];
        if (s == kNoShadow) { ++g_stat_world_fallback; return 0; }
        *out_entry = s;
        return 1;
    }
    if (!g_io) return 0;
    // Both terrain layers are 64x32 (checked in highlight_tick): 512x256 px.
    const std::size_t scr = 0x10u + 4u * static_cast<std::size_t>(bg);
    const int hofs = (g_io[scr] | (g_io[scr + 1] << 8)) & 0x1FF;
    const int vofs = (g_io[scr + 2] | (g_io[scr + 3] << 8)) & 0x1FF;
    const int tx = static_cast<int>(
        (static_cast<std::uint32_t>(sample_hx + hofs) & 511u) >> 3);
    const int ty = static_cast<int>(
        (static_cast<std::uint32_t>(sample_y + vofs) & 255u) >> 3);
    return kol_bg_underlay(bg, tx, ty, entry, out_entry);
}

// KOL_HIGHLIGHT_WORLD_CHECK=1: wherever the ring shadow and the world shadow
// both hold a cell of the authentic 240x160, they must hold the same entry.
// Sampled once per visible tile, at its top-left pixel.
void world_check(const std::uint8_t* io) {
    static long frames = 0, agree = 0, differ = 0, world_only = 0,
                ring_only = 0;
    static int reported = 0;
    for (int b = 0; b < kLayers; ++b) {
        const std::size_t scr = 0x10u + 4u * static_cast<std::size_t>(b);
        const int hofs = (io[scr] | (io[scr + 1] << 8)) & 0x1FF;
        const int vofs = (io[scr + 2] | (io[scr + 3] << 8)) & 0x1FF;
        for (int y = -(vofs & 7); y < 160; y += 8) {
            for (int x = -(hofs & 7); x < 240; x += 8) {
                const int sy = y < 0 ? 0 : y, sx = x < 0 ? 0 : x;
                int row = 0, col = 0;
                if (!map_source_world_cell(b, sx, sy, &row, &col)) continue;
                const int tx = ((sx + hofs) & 511) >> 3;
                const int ty = ((sy + vofs) & 255) >> 3;
                const std::uint16_t r = g_shadow[b][ty][tx];
                const std::uint16_t w = g_world[b][row][col];
                if (r == kNoShadow && w == kNoShadow) continue;
                if (r == kNoShadow) { ++world_only; continue; }
                if (w == kNoShadow) { ++ring_only; continue; }
                if (r == w) { ++agree; continue; }
                ++differ;
                if (reported < 8) {
                    ++reported;
                    std::fprintf(stderr,
                        "[hl-check] MISMATCH bg%d screen(%d,%d) ring(%d,%d)=%04X "
                        "world(%d,%d)=%04X\n", b, sx, sy, tx, ty, r, row, col, w);
                }
            }
        }
    }
    if ((++frames % 60) == 0) {
        std::fprintf(stderr,
            "[hl-check] %ld frames: agree=%ld differ=%ld world_only=%ld "
            "ring_only=%ld\n", frames, agree, differ, world_only, ring_only);
        std::fflush(stderr);
    }
}

}  // namespace

void highlight_set_enabled(bool on) {
    g_enabled = on;
    // install_highlight() may not have run yet; it reads g_enabled itself.
    if (g_installed) gba::g_ws_bg_underlay_layers = on && g_reserved ? 0x3u : 0x0u;
}

void highlight_stats(int* out_shadowed, int* out_fallback) {
    if (out_shadowed) *out_shadowed = g_stat_shadowed;
    if (out_fallback) *out_fallback = g_stat_fallback;
}

void install_highlight() {
    if (g_installed) return;
    g_installed = true;
    reset_all();
    g_epoch = g_runtime_state_epoch;

    if (const char* off = std::getenv("KOL_WS_NO_HIGHLIGHT_BLEND"))
        g_enabled = !(off[0] && off[0] != '0');
    if (const char* a = std::getenv("KOL_HIGHLIGHT_ALPHA")) {
        const int v = std::atoi(a);
        if (v >= 0 && v <= 16) gba::g_ws_bg_underlay_alpha =
            static_cast<unsigned>(v);
    }
    gba::g_ws_bg_underlay_provider = kol_bg_underlay;
    // KOL_WS_NO_HIGHLIGHT_WORLD=1 goes back to the ring shadow alone, exactly
    // as the renderer used it before the sample seam existed.
    if (const char* w = std::getenv("KOL_WS_NO_HIGHLIGHT_WORLD"))
        g_world_on = !(w[0] && w[0] != '0');
    gba::g_ws_bg_underlay_sample_provider =
        g_world_on ? kol_bg_underlay_sample : nullptr;
    gba::g_ws_bg_underlay_layers = g_enabled ? 0x3u : 0x0u;   // BG0 + BG1
}

void highlight_tick() {
    install_highlight();
    if (!g_enabled) return;
    g_stat_shadowed = 0;
    g_stat_fallback = 0;
    g_stat_inked = 0;
    g_stat_world_fallback = 0;
    // A state load can land in a different battle, or in the same one with the
    // range already open; either way nothing remembered still applies.
    if (g_runtime_state_epoch != g_epoch) {
        g_epoch = g_runtime_state_epoch;
        reset_all();
    }
    if (!is_battle_scene()) {
        // Leaving the map invalidates everything: the next battle reuses the
        // same tilemap slots and array for entirely different terrain.
        reset_all();
        return;
    }
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !io) return;

    std::memset(g_has_ink, 0, sizeof g_has_ink);
    g_vram = vram;
    g_io = io;
    {
        static const bool any_palette = std::getenv("KOL_HIGHLIGHT_ANY_PALETTE") != nullptr;
        const std::uint8_t* pal = bus->pal_ptr();
        const unsigned idx = static_cast<unsigned>(gba::g_ws_bg_underlay_index) & 0x0Fu;
        auto colour = [&](unsigned bank) {
            const std::size_t at = (bank * 16u + idx) * 2u;
            return static_cast<unsigned>(pal[at] | (pal[at + 1] << 8));
        };
        g_reserved = any_palette || !pal ||
                     (colour(0) == colour(1) && colour(0) == colour(2) && colour(0) == colour(3));
        gba::g_ws_bg_underlay_layers = g_reserved ? 0x3u : 0x0u;
        if (!g_reserved) return;   // (the shadows stay as they were)
    }

    for (int b = 0; b < kLayers; ++b) {
        const std::size_t cnt = 0x08u + 2u * static_cast<std::size_t>(b);
        const std::size_t scr = 0x10u + 4u * static_cast<std::size_t>(b);
        const std::uint16_t bgcnt =
            static_cast<std::uint16_t>(io[cnt] | (io[cnt + 1] << 8));
        const std::uint32_t char_base = ((bgcnt >> 2) & 3u) * 0x4000u;
        const std::uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        // Both terrain layers must be the 64x32 shape this shadow assumes.
        if (((bgcnt >> 14) & 3u) != 1u) continue;
        g_char_base = char_base;

        const std::uint32_t hofs =
            static_cast<std::uint32_t>(io[scr] | (io[scr + 1] << 8)) & 0x1FFu;
        const std::uint32_t vofs =
            static_cast<std::uint32_t>(io[scr + 2] | (io[scr + 3] << 8)) & 0x1FFu;
        const int c0 = static_cast<int>(hofs >> 3);
        const int r0 = static_cast<int>(vofs >> 3);

        // Recycling, handled exactly rather than by a visible-window guess. The
        // tilemap is a ring: column tx carries world column W with W congruent
        // to tx modulo kMapW, drawn from the window [c0, c0+kMapW-1]. When c0
        // moves, the columns dropping out of that window are reused for other
        // world columns, so their shadows must go. Which ones those are is
        // exact, and does not depend on how wide a view this build presents.
        static bool have_origin[kLayers] = {false, false};
        static int last_c0[kLayers] = {0, 0}, last_r0[kLayers] = {0, 0};
        if (!have_origin[b]) {
            have_origin[b] = true; last_c0[b] = c0; last_r0[b] = r0;
        }
        g_clear_layer = b;
        recycle(last_c0[b], c0, kMapW, clear_col);
        recycle(last_r0[b], r0, kMapH, clear_row);
        last_c0[b] = c0;
        last_r0[b] = r0;

        // Record a clean sighting for every cell not currently wearing the
        // highlight. Scanning the whole ring rather than just the viewport
        // costs 2048 cheap reads and shadows terrain before it scrolls in.
        for (int ty = 0; ty < kMapH; ++ty) {
            for (int tx = 0; tx < kMapW; ++tx) {
                const std::uint32_t block = static_cast<std::uint32_t>(tx >> 5);
                const std::size_t off = screen_base + block * 0x800u +
                    (static_cast<std::size_t>(ty) * 32u +
                     static_cast<std::size_t>(tx & 31)) * 2u;
                if (off + 1 >= 96u * 1024u) continue;
                const std::uint16_t entry = static_cast<std::uint16_t>(
                    vram[off] | (vram[off + 1] << 8));
                if (tile_has_ink(entry & 0x3FFu)) {
                    ++g_stat_inked;
                    if (g_shadow[b][ty][tx] != kNoShadow) ++g_stat_shadowed;
                } else {
                    g_shadow[b][ty][tx] = entry;
                }
            }
        }
    }

    if (std::getenv("KOL_HIGHLIGHT_DEBUG")) {
        static int f = 0;
        if ((f++ % 30) == 0) {
            int inked = 0;
            for (int b = 0; b < kLayers; ++b)
                for (int ty = 0; ty < kMapH; ++ty)
                    for (int tx = 0; tx < kMapW; ++tx)
                        if (g_shadow[b][ty][tx] != kNoShadow) ++inked;
            std::fprintf(stderr,
                "[hl] on=%d layers=%u alpha=%u prov=%p shadowed=%d/%d "
                "calls=%d fallback=%d inked_cells=%d scroll=%u,%u\n",
                (int)g_enabled, gba::g_ws_bg_underlay_layers,
                gba::g_ws_bg_underlay_alpha,
                (void*)gba::g_ws_bg_underlay_provider, inked, kLayers * kMapW * kMapH,
                g_stat_calls, g_stat_fallback, g_stat_inked, 0u, 0u);
            std::fflush(stderr);
        }
        g_stat_calls = 0;
    }
}

void highlight_world_tick() {
    if (!g_installed || !g_enabled || !g_world_on || !g_vram || !g_io) return;
    if (!is_battle_scene()) return;   // highlight_tick has already reset
    if (!g_reserved) return;          // no highlight colour this frame
    // Only a map verified THIS frame against what the guest drew is read. A
    // frame it does not verify leaves the shadow as it was.
    if (!map_source_armed()) return;
    // Nor one served with BG1's array not whole yet (map_source_rearmed: map
    // 84's scene loads its map again): its empty cells were recorded as the
    // terrain, and the crystals' tiles with ink-coloured pixels that came
    // back over them were then drawn through, their outlines gone for the
    // rest of the scene.
    if (map_source_rearmed()) return;

    int changed = 0, recorded = 0, polluted = 0, shadowed = 0;
    for (int b = 0; b < kLayers; ++b) {
        int rows = 0;
        const std::uint8_t* arr = map_source_array(b, &rows);
        if (!arr || rows > kWorldMaxRows) continue;
        // A different array or shape is a different map: nothing carries over.
        const bool fresh = arr != g_world_array[b] || rows != g_world_rows[b];
        if (fresh) {
            reset_world(b);
            g_world_array[b] = arr;
            g_world_rows[b] = rows;
        }
        const std::size_t cnt = 0x08u + 2u * static_cast<std::size_t>(b);
        const std::uint16_t bgcnt =
            static_cast<std::uint16_t>(g_io[cnt] | (g_io[cnt + 1] << 8));
        g_char_base = ((bgcnt >> 2) & 3u) * 0x4000u;

        for (int r = 0; r < rows; ++r) {
            const std::uint8_t* src = arr + static_cast<std::size_t>(r) * 0x100u;
            // The terrain does not change during a battle; only the highlight
            // comes and goes. So almost every row is identical to last frame's
            // and costs one compare.
            if (!fresh && std::memcmp(src, g_world_seen[b][r], 0x100u) == 0)
                continue;
            for (int c = 0; c < kWorldCols; ++c) {
                const std::uint16_t v = static_cast<std::uint16_t>(
                    src[c * 2] | (src[c * 2 + 1] << 8));
                if (!fresh && v == g_world_seen[b][r][c]) continue;
                g_world_seen[b][r][c] = v;
                ++changed;
                const bool ink = tile_has_ink(v & 0x3FFu);
                const std::uint64_t lit = ink ? ink_mask(v) : 0;
                g_world_lit_cells[b] += (lit != 0) - (g_world_lit[b][r][c] != 0);
                g_world_lit[b][r][c] = lit;
                if (!ink) {
                    g_world[b][r][c] = v;
                    ++recorded;
                } else if (!g_world_in_list[b][r][c]) {
                    g_world_in_list[b][r][c] = 1;
                    g_world_inked[b].push_back(static_cast<std::uint32_t>(r * kWorldCols + c));
                }
            }
        }
        // The inked cells again, by their tile's art as it is this frame.
        static const bool no_recheck = std::getenv("KOL_HIGHLIGHT_NO_RECHECK") != nullptr;
        std::vector<std::uint32_t>& inked = g_world_inked[b];
        for (std::size_t i = 0; i < inked.size() && !no_recheck;) {
            const int r = static_cast<int>(inked[i] / kWorldCols), c = static_cast<int>(inked[i] % kWorldCols);
            const std::uint16_t v = g_world_seen[b][r][c];
            const bool ink = tile_has_ink(v & 0x3FFu);
            const std::uint64_t lit = ink ? ink_mask(v) : 0;
            g_world_lit_cells[b] += (lit != 0) - (g_world_lit[b][r][c] != 0);
            g_world_lit[b][r][c] = lit;
            if (ink) { ++i; continue; }
            g_world[b][r][c] = v;
            ++recorded;
            g_world_in_list[b][r][c] = 0;
            inked[i] = inked.back();
            inked.pop_back();
        }
    }

    static const bool debug = std::getenv("KOL_HIGHLIGHT_DEBUG") != nullptr;
    static const bool check =
        std::getenv("KOL_HIGHLIGHT_WORLD_CHECK") != nullptr;
    if (debug) {
        static int f = 0;
        if ((f++ % 30) == 0 || changed) {
            // A shadow whose own art now carries the highlight colour would
            // blend the highlight with itself. Counted, never expected.
            for (int b = 0; b < kLayers; ++b)
                for (int r = 0; r < g_world_rows[b]; ++r)
                    for (int c = 0; c < kWorldCols; ++c) {
                        const std::uint16_t w = g_world[b][r][c];
                        if (w == kNoShadow) continue;
                        ++shadowed;
                        if (tile_has_ink(w & 0x3FFu)) ++polluted;
                    }
            std::fprintf(stderr,
                "[hl-world] rows=%d,%d changed=%d recorded=%d shadowed=%d "
                "polluted=%d world_fallback=%d\n",
                g_world_rows[0], g_world_rows[1], changed, recorded, shadowed,
                polluted, g_stat_world_fallback);
            std::fflush(stderr);
        }
    }
    if (check) world_check(g_io);
}

const std::uint64_t* highlight_world_lit(int bg, int* out_rows) {
    if (bg < 0 || bg >= kLayers || !g_reserved || !g_world_array[bg] || g_world_lit_cells[bg] == 0) return nullptr;
    if (out_rows) *out_rows = g_world_rows[bg];
    return &g_world_lit[bg][0][0];
}

}  // namespace kol
