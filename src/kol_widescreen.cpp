// kol_widescreen.cpp — see kol_widescreen.h.

#include "kol_widescreen.h"
#include "kol_cheevos.h"
#include "kol_crash.h"
#include "kol_map_settings.h"
#include "kol_oam.h"
#include "kol_tile_edit.h"
#include "kol_edit_panel.h"
#include "kol_settings.h"
#include "kol_sound.h"
#include "kol_menu.h"
#include "runtime.h"
#include "kol_highlight.h"

#include "kol_terrain_probe.h"
#include "kol_unit_data.h"
#include "kol_unit_list.h"
#include "kol_scene_strip.h"
#include "kol_map_source.h"
#include "kol_actions.h"
#include "kol_fast_load.h"
#include "kol_fast_actions.h"
#include "kol_ghost.h"
#include "kol_shop_buy.h"
#include "kol_gulls.h"
#include "kol_states.h"
#include "kol_vs_bot.h"
#include "kol_vs_cpu.h"
#include "kol_rumble.h"
#include "kol_stick_cursor.h"
#include "kol_auto_win.h"
#include "kol_obj_cull.h"
#include "kol_focus.h"
#include "kol_perspective.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "gba_bus.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdlib>

// ── Extended-view policy hooks ──────────────────────────────────────────
// Declared here rather than included: only gbarecomp/src/runtime is on this
// target's include path. Each is extern "C" in gbarecomp/src/gba/gba_ppu.h,
// so the unmangled symbol matches despite the header declaring them inside
// namespace gba.
extern "C" int g_ws_pillarbox;
extern "C" int g_ws_letterbox_windows;   // (gbarecomp: a letterbox window reaches the view's edges)
extern "C" int (*g_ws_window_margin_span)(int win, int y, int* x1, int* x2);   // (gbarecomp: WIN0 past the native screen)
extern "C" int g_ws_authored_margin_layers;
extern "C" int (*g_ws_obj_attr_x_provider)(int oam_index, std::uint16_t attr0,
                                           std::uint16_t attr1,
                                           std::uint16_t attr2, int* out_x);
extern "C" int g_ws_pillarbox_left;
extern "C" int g_ws_pillarbox_right;

// The bars beside a scene that is not wide (the title, the opening, menus at
// 240x160 on a 16:9 panel) are black, not the host window's dark navy (the
// user, 2026-11-16, asked for black instead of the blue-grey). Set before the
// first present.
[[maybe_unused]] static const int kBlackBars = (gbarecomp::g_present_border_rgb = 0x000000u, 0);
// Supplies the tilemap ENTRY for a margin sample, which is what lets the
// margins show map the guest has composed but not yet blitted into VRAM.
// 1 = use *out_entry, 0 = nothing here (the PPU leaves the pixel transparent).
extern "C" int (*g_ws_tilemap_provider_x_phase)(int bg);
extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        std::uint16_t* out_entry);
extern "C" int (*g_ws_tilemap_fill_provider)(int bg, int hw_x, int screen_y,
                                             std::uint16_t* out_entry);
extern "C" int (*g_ws_tilemap_override_provider)(int bg, int hw_x, int screen_y,
                                                 std::uint16_t entry, std::uint16_t* out_entry);
extern "C" unsigned g_ws_tilemap_override_layers;
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x);
extern "C" unsigned g_ws_bg_x_provider_layers;
extern "C" int (*g_ws_bg_x_row_provider)(int bg, int screen_y, int x0, int count,
                                         signed char* out_action, int* out_hx);
extern "C" int (*g_ws_bg_y_provider)(int bg, int output_x, int screen_y,
                                     int* out_src_y);
extern "C" unsigned g_ws_bg_y_provider_layers;
extern "C" int g_ws_obj_native_clip;
// The compositor's per-pixel layer map, which lets the edge blur keep the HUD
// sharp.
extern "C" int g_ppu_record_layers;
// The sprites each frame was drawn with, for the scene rebuild.
extern "C" int g_ppu_record_obj;
extern "C" void runtime_pc_hist_arm(int on);
extern "C" void runtime_rw_record(int on, std::uint32_t depth);
extern "C" void runtime_rw_dump(const char* path);
extern "C" void runtime_pc_watch_set(std::uint32_t pc, void (*cb)(void*), void* ctx);
// Temporary: every generated function entry while the histogram is armed
// (pc, IRQ nesting depth, sp, cycles), chained on g_runtime_fn_entry_hook.
#include "runtime_arm.h"
extern "C" uint32_t g_irq_nest_depth;
namespace {
// EWRAM offsets read below, through kol_region.h (an offset written bare is
// the USA game's in the Japanese build too): the map shown (u16), the
// scenes' full palette, the palette buffer, the weather's drops and block.
constexpr std::uint32_t kEwMap = kol::usa(0x02029D40u) - 0x02000000u;
constexpr std::uint32_t kEwFullPal = kol::usa(0x0202D90Cu) - 0x02000000u;
constexpr std::uint32_t kEwPalBuffer = kol::usa(0x02002400u) - 0x02000000u;
constexpr std::uint32_t kEwDrops = kol::usa(0x0202A0F8u) - 0x02000000u;
constexpr std::uint32_t kEwWeather = kol::usa(0x0202A1C0u) - 0x02000000u;
int ew_map(const std::uint8_t* ew) { return ew[kEwMap] | (ew[kEwMap + 1] << 8); }
struct EntryRec { std::uint32_t pc, depth, sp; unsigned long long cycles; };
std::vector<EntryRec> g_entry_log;
bool g_entry_log_on = false;
void (*g_entry_prev)(std::uint32_t) = nullptr;
// KOL_RW_FN=<pc>,<path>: record the reads and writes of one call of the
// function at pc (its own IRQ depth), dumped at its return (needs
// GBARECOMP_INSN_TRACE=1 for the return watch).
std::uint32_t g_rw_fn = 0;
std::string g_rw_path;
bool g_rw_done = false;
void rw_return(void*) {
    runtime_rw_dump(g_rw_path.c_str());
    std::fprintf(stderr, "rw_fn: %08X returned, sets -> %s\n", g_rw_fn, g_rw_path.c_str());
}
void entry_log_hook(std::uint32_t pc) {
    if (g_entry_log_on && g_entry_log.size() < 4000000u)
        g_entry_log.push_back({pc, g_irq_nest_depth, g_cpu.R[13], g_runtime_cycles});
    if (g_rw_fn && pc == g_rw_fn && !g_rw_done) {
        g_rw_done = true;
        runtime_rw_record(1, g_irq_nest_depth);
        runtime_pc_watch_set(g_cpu.R[14] & ~1u, &rw_return, nullptr);
        std::fprintf(stderr, "rw_fn: %08X entered at depth %u, lr %08X\n", pc, g_irq_nest_depth, g_cpu.R[14]);
    }
    if (g_entry_prev) g_entry_prev(pc);
}
void entry_log_chain() {
    static bool chained = false;
    if (!chained) { chained = true; g_entry_prev = g_runtime_fn_entry_hook; g_runtime_fn_entry_hook = entry_log_hook; }
}
void entry_log_arm(bool on) {
    entry_log_chain();
    g_entry_log.clear();
    g_entry_log.reserve(4000000u);
    g_entry_log_on = on;
}
void entry_log_dump(const char* path) {
    g_entry_log_on = false;
    if (FILE* f = std::fopen(path, "w")) {
        for (const EntryRec& e : g_entry_log) std::fprintf(f, "%08X %u %08X %llu\n", e.pc, e.depth, e.sp, e.cycles);
        std::fclose(f);
    }
}
}  // namespace
extern "C" void runtime_pc_hist_dump(const char* path);
// The frame composed again without these layers, for the perspective.
extern "C" unsigned g_ppu_scene_exclude_layers;
// And what lies under each sprite there, for the units the perspective lifts.
extern "C" int g_ppu_scene_under_obj;
extern "C" int g_ppu_scene_under_bg1;
extern "C" unsigned g_ppu_scene_overscan[4];
extern "C" const unsigned char* g_ppu_scene_overscan_rows;

namespace {

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

std::uint16_t io16(const std::uint8_t* io, std::size_t off) {
    return static_cast<std::uint16_t>(io[off] | (io[off + 1] << 8));
}

// Bitmask of regular BG layers allowed to draw in the widened margins, and
// the output-column span of the authentic 240px image. Republished every
// frame by the scene policy below.
// BG2|BG3 carry the battle UI. Except while BG2 holds a piece of the world
// (g_world_bg2, world_bg2_frame below): then BG3 alone.
bool g_world_bg2 = false;
bool g_sea_bg2 = false;   // BG2 is the sea's field (sea_bg2_frame below)
inline unsigned ui_layers() { return g_world_bg2 ? 0x8u : 0xCu; }
// (KOL_WS_WAS_BATTLE=1: a state loaded in a scene that only stays the battle map coming
// from it, is_battle_map's was_battle, starts as if it did: map 88's collapse, 0294.)
bool g_battle_scene = std::getenv("KOL_WS_WAS_BATTLE") != nullptr;
// The view size the scene asks for, settled (the request below), whatever
// size is applied yet: VS over a network culls by it (kol_vs_net).
unsigned g_vs_target_w = 0, g_vs_target_h = 0;

// Set per frame: is there actually full-width furniture to continue? The
// dialogue box occupies the same scanlines as the bottom bar, and continuing
// IT produced repeated vertical slices across the rest of the band.
bool g_has_fullwidth_ui = false;
// The anchored full-height side panel, in NATIVE pixels, and the shift it got.
// The battle-preparation roster's portraits and its selection brackets are OBJ
// sprites, which no BG provider moves: anchoring the frame alone slides it out
// from under them, exactly as it did to the ARMS box. Anything drawn inside the
// panel's footprint moves with it — see kol_obj_attr_x().
int g_side_panel_x0 = 0, g_side_panel_x1 = -1, g_side_panel_shift = 0;
// ── Continuing full-height side furniture into a VERTICAL margin ────────
// Without this the roster panel keeps its authentic 160-line height and the
// map shows above and below it, which reads as the panel floating in a hole.
//
// How to continue it is measured per frame, not assumed. The roster's
// background is a hatch with a period in BOTH axes (on AI_prep_vertical its
// BG3 columns cycle tiles 005-008 and the rows repeat every 2 tile rows), so
// the margin repeats that period. Two things this already rules out, both
// tried: repeating ONE scanline turns the diagonal hatch into vertical
// streaks, and repeating the panel's top rows stamps slices of the portrait
// frames into the margin.
//
// Only the layer the panel's BACKGROUND sits on is continued. Its contents
// are on the other UI layer, and repeating those would tile the margin with
// portraits. KOL_WS_NO_PANEL_EXTEND=1 turns it off.
int g_extend_side_panel_v = 1;
int g_side_panel_bg = -1;        // UI layer holding the panel background
int g_side_panel_period_y = 0;   // vertical repeat in pixels; 0 = none found
int g_side_panel_top_y = 0;      // first scanline of the panel background
// KOL_WS_NO_SIDE_PANEL=1 leaves a full-height side panel where the guest drew
// it. Read once per frame beside the other policy gates, not per component.
int g_side_panel_anchor = 1;
// First screen row occupied by the bottom bar, or 160 when there is none.
int g_bar_top_y = 160;
unsigned g_margin_layers = 0;
unsigned g_native_left = 0;
unsigned g_native_right = 240;

// ── Bottom unit bar continuation ────────────────────────────────────────
// The bar the game draws when a unit is selected is a full-bleed horizontal
// band with NO decorative end caps: a dark rule, a white highlight, a shaded
// rule, then a cream field carrying an 8x8 tiled diagonal hatch. Measured on
// states/D_thor_unit_bar.state: the band is screen y 139..159, rows 139-141
// are uniform across all 240 columns, and screen columns 0..7 are plain field
// (no glyphs) for every row of the band.
//
// So the bar can be continued into the margins exactly, not approximately:
// sample the plain-field column with the SAME 8-pixel phase. The hatch is
// 8-periodic horizontally and vertically, so phase-matched sampling
// reproduces it with no seam, and the three uniform rules extend trivially.
//
// This degrades safely. When no unit is selected the bar is not drawn, so the
// source columns are transparent too and the margins keep showing terrain —
// the rule needs no "is the bar visible" test of its own.
// ── Margin contiguity ───────────────────────────────────────────────────
// BG0's tilemap is wider than the camera: with hofs=24 the camera shows tile
// columns 3..32 while the map itself occupies 0..45. So margin pixels always
// come from off-camera tilemap content. Usually that IS the real map, which is
// why widening looks right — but the tilemap also retains regions the game
// never displays from the current camera position, and those render as scenery
// floating in the sky. Measured on states/E_water_tile_margin.state: BG0 tile
// column 35, rows 21..23, holding real tile indices disconnected from the map.
//
// Rule: a margin tile is drawn only if the SAME tile row has content at the
// window edge — i.e. only scenery contiguous with what is actually on screen
// continues outward. Verified: rejects exactly the three bad rows in state E,
// and is a byte-for-byte no-op on states B and D, whose margins have zero
// disconnected rows.
//
// It is a heuristic, and it fails closed: a genuinely detached island just
// off-screen would be suppressed too. That is the right direction for this
// project — better a missing rock than an invented one.
const std::uint8_t* g_vram = nullptr;
const std::uint8_t* g_io = nullptr;

// BG2 holds a spell's circle (map_settings.cfg `magic_circle`): a wide,
// lying shape in a 256 x 256 map, by its tiles (as kol_focus circle_capture
// tests its pixels). Its margins then show nothing: the map wraps every 256,
// and widened past the game's 240 the circle's far side came round again
// beside it (map 57, Shaher's Descent, states/thor/0236: a red copy in the
// right border while the picture is flat). Set once a frame
// (bg2_circle_update); KOL_WS_BG2_CIRCLE_WRAP=1: wrapped, as before.
bool g_bg2_circle = false;
void bg2_circle_update(const std::uint8_t* ew) {
    g_bg2_circle = false;
    static const bool wrap = std::getenv("KOL_WS_BG2_CIRCLE_WRAP") != nullptr;
    if (wrap || !g_vram || !g_io || !ew) return;
    const int map = ew_map(ew);
    if (!kol::map_flag(kol::MapFlag::MagicCircle, map)) return;
    const unsigned dispcnt = g_io[0] | (g_io[1] << 8);
    const unsigned cnt = g_io[0x0C] | (g_io[0x0D] << 8);
    if ((dispcnt & 7u) != 0u || !(dispcnt & 0x0400u) || (cnt & 0xC080u) != 0u) return;
    const std::uint32_t screen = ((cnt >> 8) & 31u) * 0x800u, chars = ((cnt >> 2) & 3u) * 0x4000u;
    int n = 0, x0 = 32, y0 = 32, x1 = -1, y1 = -1;
    for (int ty = 0; ty < 32; ++ty)
        for (int tx = 0; tx < 32; ++tx) {
            const std::uint32_t at = screen + 2u * static_cast<std::uint32_t>(ty * 32 + tx);
            // A tile with pixels (the empty map points at a blank tile, not 0).
            const std::uint32_t tile = chars + ((g_vram[at] | (g_vram[at + 1] << 8)) & 0x3FFu) * 32u;
            if (tile + 32u > 0x10000u) continue;
            bool any = false;
            for (std::uint32_t i = 0; i < 32u && !any; ++i) any = g_vram[tile + i] != 0;
            if (!any) continue;
            ++n;
            x0 = std::min(x0, tx); y0 = std::min(y0, ty); x1 = std::max(x1, tx); y1 = std::max(y1, ty);
        }
    // (circle_capture: at least 1500 pixels, 120 wide, at most 2/3 as high.)
    g_bg2_circle = n >= 24 && x1 - x0 + 1 >= 15 && 3 * (y1 - y0 + 1) <= 2 * (x1 - x0 + 1);
}

inline std::uint16_t vram16(std::uint32_t off) {
    return static_cast<std::uint16_t>(g_vram[off] | (g_vram[off + 1] << 8));
}

// Tilemap entry for a regular BG, following the GBA screenblock layout.
std::uint16_t bg_tile_entry(std::uint16_t bgcnt, int tx, int ty) {
    const std::uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
    const unsigned size_code = (bgcnt >> 14) & 3u;
    const int w_tiles = (size_code & 1u) ? 64 : 32;
    const int h_tiles = (size_code & 2u) ? 64 : 32;
    tx = ((tx % w_tiles) + w_tiles) % w_tiles;
    ty = ((ty % h_tiles) + h_tiles) % h_tiles;
    const int block = (ty / 32) * (w_tiles / 32) + (tx / 32);
    const std::uint32_t off = screen_base + block * 0x800u +
                              ((ty % 32) * 32 + (tx % 32)) * 2u;
    if (off + 1 >= 96u * 1024u) return 0;
    return vram16(off);
}

// Non-zero tile index = something is drawn there.
bool margin_row_is_contiguous(int bg, int screen_y, bool right_side) {
    if (!g_vram || !g_io) return true;   // no view: keep prior behaviour
    const std::size_t cnt_off = 0x08 + 2u * static_cast<std::size_t>(bg);
    const std::size_t scr_off = 0x10 + 4u * static_cast<std::size_t>(bg);
    const std::uint16_t bgcnt =
        static_cast<std::uint16_t>(g_io[cnt_off] | (g_io[cnt_off + 1] << 8));
    const int hofs = (g_io[scr_off] | (g_io[scr_off + 1] << 8)) & 0x1FF;
    const int vofs = (g_io[scr_off + 2] | (g_io[scr_off + 3] << 8)) & 0x1FF;
    const int ty = (vofs + screen_y) / 8;
    const int edge_tx = right_side ? (hofs + 239) / 8 : hofs / 8;
    return (bg_tile_entry(bgcnt, edge_tx, ty) & 0x3FFu) != 0u;
}

// ── HUD edge anchoring ──────────────────────────────────────────────────
// The battle HUD is authored for a 240px screen, so at 284 its corner panels
// keep their native position and float 22px in from the edge. Anchoring moves
// each HUD element out to the screen edge it was designed to sit against.
//
// The decision is made per ELEMENT, not per scanline. A scanline-local rule
// tears things apart: in states/D_thor_unit_bar.state the top-left HP bar
// occupies tile cols 1..7 on one row and 0..9 on the next, so an "is it
// touching the edge" test would shift one row and not the other. Instead the
// UI tilemap is segmented into 4-connected components once per frame and every
// tile of a component gets the same shift.
//
// Classification per component bounding box [c0,c1] over the 30 visible tile
// columns: spanning almost the whole width => full-width furniture, no shift
// (the bottom bar background; its margins are handled by the continuation rule
// above). Otherwise the box centre picks an edge: left half anchors left,
// right half anchors right.
//
// UNLIKE everything else in this file, anchoring MOVES PIXELS INSIDE the
// authentic 240 — it is the one deliberate exception to the centre-240
// invariant, which is why it has its own switch. KOL_WS_NO_HUD_ANCHOR=1 turns
// it off and restores byte-identical centre-240 output.
constexpr int kUiTileCols = 30;   // 240px
constexpr int kUiTileRows = 20;   // 160px

// ── Character info panel pinning ────────────────────────────────────────
// The guest moves the unit info panel around the screen to avoid covering the
// selected sprite — it is not a left/right corner swap, it relocates in BOTH
// axes (states/F_char_info_panel.state has it at tile cols 20..29 rows 13..19,
// states/G_panel_left.state at cols 10..19 rows 6..12). Widescreen gives the
// panel a permanent home in the bottom-right corner, so it is pinned there and
// the guest's avoidance placement is overridden.
//
// The panel is identified structurally, not by position: it is the only SOLID
// 10x7 tile block on BG3. Verified across all four battle states — F, G and B
// each have exactly one such component (at three different positions), and
// state D, which shows the bottom bar instead, has none (its components are
// 10x4 ragged, 7x9 ragged, and the 30x3 full-width bar).
//
// Relocation is a pure translation: both the BG3 background and the BG2
// contents move by the same delta, sampled through the X and Y providers.
constexpr int kPanelTilesW = 10;
constexpr int kPanelTilesH = 7;
int g_panel_pin = 0;
// The panel is left out altogether: vacated like a pinned one, drawn nowhere.
// Set while the second screen shows the same unit (kol::second_screen_shown).
int g_panel_hide = 0;
bool g_panel_found = false;
int g_panel_src_x = 0, g_panel_src_y = 0;    // native screen px of the panel
int g_panel_dst_x = 0, g_panel_dst_y = 0;    // output px it is pinned to

int g_hud_anchor = 0;
int g_extra_left = 0;
int g_extra_right = 0;
// Per UI layer, per tile cell: the horizontal shift in pixels applied to that
// cell's element. Indexed [bg-2][ty][tx].
signed char g_ui_shift[2][kUiTileRows][kUiTileCols];
// ── Vertical anchoring ──────────────────────────────────────────────────
// The same idea one axis over, and it only does anything once the view has a
// TOP/BOTTOM margin. A box the game drew against the top or bottom of the
// authentic 160 lines otherwise keeps its native scanline and floats with a
// strip of map beyond it. Full-height furniture is excluded: the side panel
// is CONTINUED into the margin, not moved.
//
// The contents come along through the BG containment pass below, not through
// any OBJ hook: every HUD glyph these boxes carry is BG. A vertical OBJ hook
// was written on the assumption that they were sprites, and it claimed WORLD
// sprites that merely overlapped a moved box — units pinned to the top edge
// with the terrain they stand on scrolled away beneath them. Measured: with
// the hook removed, the HP/MP readout, the MEMBER counter and the character
// info box all still move intact.
signed char g_ui_shift_y[2][kUiTileRows][kUiTileCols];
int g_extra_top = 0;
int g_extra_bottom = 0;
// Vertical shift applied to the full-width bottom bar, so its
// horizontal continuation follows it to the new bottom edge.
int g_bar_shift_y = 0;
constexpr int kBarTopY = 139;      // first scanline of the bar band
constexpr int kBarPhase = 8;       // hatch tile period
int g_extend_bottom_bar = 0;
// ── Plain edge rows (cutscene letterbox) ────────────────────────────────
// The battle-map cutscenes of the opening frame the scene with black bars on
// BG3: a full row at the top and a run at the bottom that carries the
// narration (states/GX_intro_bars.state, rows 0 and 16..19). The bar band
// above only covers the unit bar's measured rows 139..159, so the letterbox
// stopped at the old 240 edges and the map showed past both ends.
//
// A row is continued whole when it runs edge to edge on BG3 and its two END
// tiles are the same tile as its second one, on both UI layers (BG2 may also
// have nothing at its first tile): then the 8 columns the continuation
// samples are the same field the row is made of, and the repeat has no seam. A framed box fails that test at once (its end tiles are
// corner/edge pieces), so a full-width dialogue is never smeared outward.
// Only runs touching the top or bottom edge count, like the bottom bar.
// A UI layer that is one tile over the whole screen: the thunder's flash on
// map 102's opening (states/HW_slot3_cutscene_thunder), BG3 filled with one
// white tile and blended over the picture (BLDCNT 3348) for a few frames. As
// HUD it was one 30x20 element: continued across the width row by row but
// not into the top and bottom margins, and the letterbox's bars on BG2 were
// held at their native rows with it ("stays put with what is under it"), so
// the flash stopped short of the edges and the bars jumped in. A field is
// left out of the elements and drawn over the whole view (any of its pixels
// is the same), and the other layer's elements anchor as they do without it.
bool g_ui_field[2] = {false, false};
std::uint8_t g_ui_field_rgb[2][3] = {};   // its tile's mean colour (kol::ui_field)
// The field the last picture was drawn with: the frame hook rebuilds the
// anchors for the next frame before the present captures the last one, so
// at the flash's end the capture saw the next frame's "none" and that last
// flash frame lost its perspective (HW, present 650).
int g_ui_field_drawn = -1;
std::uint8_t g_ui_field_drawn_rgb[3] = {};

bool g_plain_row[kUiTileRows];
int g_plain_row_dy[kUiTileRows];   // the row's vertical anchoring shift
bool g_any_plain_row = false;
// Separate from the shift: a cell holding no element and a cell holding a
// full-width element both have shift 0, and conflating them makes the
// zero-shift case match empty tiles and swallow the anchored one.
bool g_ui_occ[2][kUiTileRows][kUiTileCols];
bool g_ui_shift_valid = false;

void rebuild_ui_anchors() {
    g_ui_field_drawn = g_ui_field[1] ? 3 : g_ui_field[0] ? 2 : -1;
    if (g_ui_field_drawn >= 2) std::copy_n(g_ui_field_rgb[g_ui_field_drawn - 2], 3, g_ui_field_drawn_rgb);
    g_ui_shift_valid = false;
    g_panel_found = false;
    g_has_fullwidth_ui = false;
    g_side_panel_x1 = -1;
    g_side_panel_bg = -1;
    g_side_panel_period_y = 0;
    g_bar_shift_y = 0;
    g_bar_top_y = 160;
    g_any_plain_row = false;
    g_ui_field[0] = g_ui_field[1] = false;
    if (!g_vram || !g_io ||
        (!g_hud_anchor && !g_panel_pin)) return;

    static bool occ[2][kUiTileRows][kUiTileCols];
    static int  lab[2][kUiTileRows][kUiTileCols];
    static int  comp_shift[2][kUiTileRows * kUiTileCols + 1];
    static int  comp_shift_y[2][kUiTileRows * kUiTileCols + 1];
    static bool comp_full[2][kUiTileRows * kUiTileCols + 1];
    static int  comp_x0[2][kUiTileRows * kUiTileCols + 1];
    static int  comp_x1[2][kUiTileRows * kUiTileCols + 1];
    static int  comp_y0[2][kUiTileRows * kUiTileCols + 1];
    static int  comp_y1[2][kUiTileRows * kUiTileCols + 1];
    int comp_count[2] = {0, 0};
    bool fullwidth_row[kUiTileRows] = {};

    for (int bg = 2; bg <= 3; ++bg) {
        const int L = bg - 2;
        std::memset(lab[L], 0, sizeof(lab[L]));
        const std::size_t cnt_off = 0x08 + 2u * static_cast<std::size_t>(bg);
        const std::uint16_t bgcnt =
            static_cast<std::uint16_t>(g_io[cnt_off] | (g_io[cnt_off + 1] << 8));
        for (int ty = 0; ty < kUiTileRows; ++ty)
            for (int tx = 0; tx < kUiTileCols; ++tx)
                occ[L][ty][tx] = (bg_tile_entry(bgcnt, tx, ty) & 0x3FFu) != 0u;
        // One tile over the whole screen: a field (g_ui_field), no element.
        {
            const std::uint16_t e0 = bg_tile_entry(bgcnt, 0, 0);
            bool field = (e0 & 0x3FFu) != 0u;
            for (int ty = 0; ty < kUiTileRows && field; ++ty)
                for (int tx = 0; tx < kUiTileCols && field; ++tx)
                    field = bg_tile_entry(bgcnt, tx, ty) == e0;
            if (bg == 2 && g_world_bg2) {   // the world's, no element of the interface
                field = false;
                std::memset(occ[L], 0, sizeof(occ[L]));
            }
            g_ui_field[L] = field;
            if (field) std::memset(occ[L], 0, sizeof(occ[L]));
            // Its colour, for the perspective (kol::ui_field): the mean of
            // the tile's opaque pixels in its palette.
            gba::GbaBus* bus = field ? gbarecomp::active_bus() : nullptr;
            const std::uint8_t* pal = bus ? bus->pal_ptr() : nullptr;
            if (pal) {
                const bool bpp8 = (bgcnt & 0x80u) != 0u;
                const std::uint32_t chr = ((bgcnt >> 2) & 3u) * 0x4000u +
                                          (e0 & 0x3FFu) * (bpp8 ? 64u : 32u);
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int i = 0; i < 64 && chr + 64u <= 0x10000u; ++i) {
                    const unsigned v = bpp8 ? g_vram[chr + i]
                                            : (g_vram[chr + i / 2] >> ((i & 1) * 4)) & 15u;
                    if (!v) continue;
                    const unsigned idx = bpp8 ? v : ((e0 >> 12) & 15u) * 16u + v;
                    const unsigned c = pal[idx * 2] | (pal[idx * 2 + 1] << 8);
                    sum[0] += c & 31u; sum[1] += (c >> 5) & 31u; sum[2] += (c >> 10) & 31u;
                    ++n;
                }
                for (int ch = 0; ch < 3; ++ch) {
                    const unsigned c5 = n ? (sum[ch] + n / 2) / n : 0u;
                    g_ui_field_rgb[L][ch] = static_cast<std::uint8_t>((c5 << 3) | (c5 >> 2));
                }
                if (!n) g_ui_field[L] = false;   // all transparent: nothing drawn
            }
        }

        int sx_stack[kUiTileRows * kUiTileCols];
        int sy_stack[kUiTileRows * kUiTileCols];
        for (int sy = 0; sy < kUiTileRows; ++sy) {
            for (int sx = 0; sx < kUiTileCols; ++sx) {
                if (!occ[L][sy][sx] || lab[L][sy][sx]) continue;
                const int id = ++comp_count[L];
                int sp = 0;
                sx_stack[sp] = sx; sy_stack[sp] = sy; ++sp;
                lab[L][sy][sx] = id;
                int c0 = sx, c1 = sx;
                int rows_lo = sy, rows_hi = sy;
                while (sp > 0) {
                    --sp;
                    const int x = sx_stack[sp], y = sy_stack[sp];
                    if (x < c0) c0 = x;
                    if (x > c1) c1 = x;
                    if (y < rows_lo) rows_lo = y;
                    if (y > rows_hi) rows_hi = y;
                    static const int dx[4] = {1, -1, 0, 0};
                    static const int dy[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        const int nx = x + dx[k], ny = y + dy[k];
                        if (nx < 0 || ny < 0 || nx >= kUiTileCols ||
                            ny >= kUiTileRows) continue;
                        if (!occ[L][ny][nx] || lab[L][ny][nx]) continue;
                        lab[L][ny][nx] = id;
                        sx_stack[sp] = nx; sy_stack[sp] = ny; ++sp;
                    }
                }
                const int cw = c1 - c0 + 1;
                // Full-width furniture is no longer decided per component; see
                // the row scan after this loop.
                const bool full = false;
                // Anchor only what can plausibly BE corner furniture, and
                // bound it by measurement rather than by a safety margin.
                // Every real corner element is at most 10 tiles wide — the
                // HP/MP cluster is 10x4 and the terrain box 7x9 — while each
                // thing that broke was wider: a 22-wide cutscene frame, its
                // 14-wide caption, and a 13-wide ARMS box whose weapon icons
                // are SPRITES and therefore do not move with it, so anchoring
                // slid the box out from under them.
                const int ch = rows_hi - rows_lo + 1;
                // A full-height SIDE panel anchors to its edge even though it
                // is wider than the corner cap. The battle-preparation roster
                // is 11x20 — one tile over — and it is not what that cap
                // protects: the 22-wide cutscene frame, its 14-wide caption
                // and the 13-wide ARMS box are all SHORT boxes, none more than
                // about half the screen tall. Still bounded in width, so when
                // the NAVI dialogue overlaps the roster and the two merge into
                // one 29-wide component on BG3, that merged component is not
                // dragged to the edge with the dialogue inside it.
                const bool side_panel = g_side_panel_anchor &&
                                        ch >= kUiTileRows - 1 &&
                                        cw <= kPanelTilesW + 2 &&
                                        (c0 == 0 || c1 == kUiTileCols - 1);
                // Corner furniture is AT a corner. A component touching
                // neither side of the native screen is placed by the game in
                // world space, not against an edge: the CHECK menu's Yes/No
                // box sits at columns 11..16 in the middle of the screen and
                // was being dragged 40px left, away from its own hand cursor —
                // an OBJ sprite, which no BG provider moves. Width alone never
                // distinguished the two; every real corner element (HP/MP
                // 10x4 at column 0, the terrain box 7x9 and the info panel
                // both ending at column 29) touches an edge by construction.
                //
                // One tile column of slack, for the same reason the vertical
                // test has one tile row of it: the battle-preparation MEMBER
                // counter is drawn at columns 25..28 with column 29 unused,
                // so an exact test left it stranded mid-screen while
                // everything around it anchored. The slack cannot reach the
                // world-space case this test exists to exclude — the CHECK
                // menu's Yes/No box ends at column 16.
                const bool touches_edge =
                    c0 <= 1 || c1 >= kUiTileCols - 2;
                const bool corner =
                    (cw <= kPanelTilesW && touches_edge) || side_panel;
                comp_full[L][id] = full;
                comp_x0[L][id] = c0; comp_x1[L][id] = c1;
                comp_y0[L][id] = rows_lo; comp_y1[L][id] = rows_hi;
                comp_shift[L][id] = corner
                    ? ((c0 + c1) >= kUiTileCols ? g_extra_right : -g_extra_left)
                    : 0;
                // Vertical, by the same rule: a SHORT box that sits against
                // the top or bottom of the authentic 160 lines was drawn
                // against that edge and follows it out. A box touching
                // neither is in world space — the lesson the CHECK menu
                // taught horizontally.
                //
                // Two exclusions, both learned from the dump. The cap is 8
                // tile rows, not half the screen: the roster's right-hand
                // portrait column is 10 rows and starts at row 0, so a
                // half-screen cap swept it up into the margin. And anything
                // within the side panel's columns is excluded outright —
                // the panel is CONTINUED, and its contents stay with it.
                //
                // The edge test allows one tile row of slack. The character
                // info box ends at row 18 with row 19 unused, so an exact
                // test never fired for the one element this was asked for.
                // 9, not 8: the terrain box is rows 0..8 and an 8-row cap
                // left it alone, so it anchored right but not up and sat a
                // margin's depth below the corner it belongs in. Still under
                // the roster's right-hand portrait column, which is 10 rows
                // from row 0 and must NOT be swept into the margin.
                constexpr int kVCornerTiles = 9;
                const bool in_side_panel =
                    g_side_panel_x1 >= g_side_panel_x0 &&
                    c0 * 8 <= g_side_panel_x1 &&
                    (c1 + 1) * 8 - 1 >= g_side_panel_x0;
                // And the same width cap as the horizontal rule, for the same
                // box: the 13-wide ARMS window starts at row 1, so without it
                // the window rose into the top margin and left its hand
                // cursor and weapon icon -- sprites -- behind
                // (states/BQ_weapon_window_high.state, 384x216). Full-width
                // furniture (the bottom unit bar) still follows its edge.
                // A dialog box is no bar, however wide. One drawn at the screen's left is
                // exactly 30 tiles and went to the view's top (or foot) with the text and
                // portrait in it, while the same box further right stayed put (the user,
                // states/thor/0117: the dialog box sat against the top border
                // instead of its default position). It is known by its
                // frame's tiles on BG3, as the smooth boxes know it.
                // KOL_WS_WIDE_BOX_ANCHORED=1: as before.
                static const bool wide_box_anchored = env_on("KOL_WS_WIDE_BOX_ANCHORED");
                bool dialog_frame = false;
                if (bg == 3 && cw >= kUiTileCols && !wide_box_anchored)
                    for (int tx = c0; tx <= c1 && !dialog_frame; ++tx) {
                        const unsigned t = bg_tile_entry(bgcnt, tx, rows_lo) & 0x3FFu;
                        dialog_frame = t >= 0x19u && t <= 0x20u;
                    }
                const bool vertical_width_ok =
                    cw <= kPanelTilesW || (cw >= kUiTileCols && !dialog_frame);
                const bool short_box = ch <= kVCornerTiles && !in_side_panel &&
                                       vertical_width_ok;
                const bool at_top = rows_lo <= 1;
                const bool at_bottom = rows_hi >= kUiTileRows - 2;
                comp_shift_y[L][id] =
                    (short_box && at_bottom && !at_top) ? g_extra_bottom
                  : (short_box && at_top && !at_bottom) ? -g_extra_top
                                                        : 0;
                if (full && comp_shift_y[L][id] != 0)
                    g_bar_shift_y = comp_shift_y[L][id];
                if (side_panel) {
                    g_side_panel_x0 = c0 * 8;
                    g_side_panel_x1 = (c1 + 1) * 8 - 1;
                    g_side_panel_shift = comp_shift[L][id];
                    // Smallest tile-row period that reproduces the whole
                    // component. None found => nothing safe to repeat.
                    g_side_panel_bg = bg;
                    g_side_panel_top_y = rows_lo * 8;
                    g_side_panel_period_y = 0;
                    for (int prd = 1; prd <= 4; prd <<= 1) {
                        if (rows_hi - rows_lo < prd) break;
                        bool same = true;
                        for (int ty = rows_lo; ty + prd <= rows_hi && same; ++ty) {
                            for (int tx = c0; tx <= c1; ++tx) {
                                if ((bg_tile_entry(bgcnt, tx, ty) & 0x3FFu) !=
                                    (bg_tile_entry(bgcnt, tx, ty + prd) & 0x3FFu)) {
                                    same = false;
                                    break;
                                }
                            }
                        }
                        if (same) { g_side_panel_period_y = prd * 8; break; }
                    }
                }
                (void)full;
            }
        }
    }

    // ── Furniture found WITHOUT connected components ────────────────────
    // Components merge whatever touches. The unit panel's bottom row sits on
    // the bottom bar's top row, so the two became one component and neither
    // matched its shape: no pin, and no bar to continue. Tile identity cannot
    // help here either — panel, bar and dialogue boxes are drawn from the same
    // 9-slice tileset in the same palette bank, all checked. So both are found
    // by scanning the grid directly, where adjacency is irrelevant.

    // The bar: a short run of near-full rows ending at the bottom. The run
    // must be at most 4 rows or a bottom-anchored dialogue qualifies — one
    // measured 27 wide over rows 15..19 and would be "continued" into the
    // margins, which is the smearing bug again.
    {
        int run = 0;
        for (int ty = kUiTileRows - 1; ty >= 0; --ty) {
            int occupied = 0;
            for (int tx = 0; tx < kUiTileCols; ++tx)
                if (occ[1][ty][tx]) ++occupied;
            if (occupied < kUiTileCols - 4) break;
            ++run;
        }
        if (run >= 1 && run <= 4) {
            g_has_fullwidth_ui = true;
            for (int k = 0; k < run; ++k)
                fullwidth_row[kUiTileRows - 1 - k] = true;
            g_bar_top_y = (kUiTileRows - run) * 8;
        }
    }

    // The panel: a solid 10x7 block on BG3 whose interior carries the HP: and
    // MP: label tiles on BG2. The solid block alone is not enough — a dialogue
    // box contains many 10x7 solid sub-rectangles — and the labels are what
    // make it the unit panel rather than furniture. Some units' second line
    // reads SP: (0xAC/0xAD) instead of MP: (states/CC_box_elsewhere1.state,
    // CD_box_elsewhere3.state, "SP:150", "SP:111"); without it their panel was
    // neither pinned nor left out with the second screen up.
    for (int r = 0; r + kPanelTilesH <= kUiTileRows && !g_panel_found; ++r) {
        for (int c = 0; c + kPanelTilesW <= kUiTileCols && !g_panel_found; ++c) {
            bool solid = true;
            for (int dy = 0; dy < kPanelTilesH && solid; ++dy)
                for (int dx = 0; dx < kPanelTilesW; ++dx)
                    if (!occ[1][r + dy][c + dx]) { solid = false; break; }
            if (!solid) continue;
            bool has_hp = false, has_mp = false;
            const std::uint16_t bg2cnt =
                static_cast<std::uint16_t>(g_io[0x0C] | (g_io[0x0D] << 8));
            for (int dy = 0; dy < kPanelTilesH; ++dy)
                for (int dx = 0; dx < kPanelTilesW; ++dx) {
                    const std::uint16_t t =
                        bg_tile_entry(bg2cnt, c + dx, r + dy) & 0x3FFu;
                    if (t == 0xA8u || t == 0xA9u) has_hp = true;
                    if (t == 0xAAu || t == 0xABu ||           // MP:
                        t == 0xACu || t == 0xADu) has_mp = true;  // SP:
                }
            if (has_hp && has_mp) {
                g_panel_found = true;
                g_panel_src_x = c * 8;
                g_panel_src_y = r * 8;
            }
        }
    }

    // Full-width furniture (the bottom unit bar) spans the screen on one layer
    // while its labels and figures sit on the other. Those two must not be
    // shifted differently or the bar's own frame separates from its contents —
    // it drew a stray box around the "A" of "Alphonse" when they did. Anything
    // sharing a tile row with full-width furniture is bar content and stays
    // put; only free-standing corner panels anchor.
    for (int L = 0; L < 2; ++L)
        for (int ty = 0; ty < kUiTileRows; ++ty)
            for (int tx = 0; tx < kUiTileCols; ++tx)
                if (lab[L][ty][tx] && fullwidth_row[ty])
                    comp_shift[L][lab[L][ty][tx]] = 0;

    // Vertically the opposite: the bar was drawn against the bottom, so it and
    // everything sharing its rows follow that edge out TOGETHER. Splitting
    // them is the same failure as splitting them horizontally.
    if (g_has_fullwidth_ui && g_extra_bottom) {
        g_bar_shift_y = g_extra_bottom;
        for (int L = 0; L < 2; ++L)
            for (int ty = 0; ty < kUiTileRows; ++ty)
                for (int tx = 0; tx < kUiTileCols; ++tx)
                    if (lab[L][ty][tx] && fullwidth_row[ty])
                        comp_shift_y[L][lab[L][ty][tx]] = g_extra_bottom;
    }

    // A container that anchors takes its CONTENTS with it. Contents are
    // classified on their own and usually fail the edge test — the HP/MP
    // figures sit three columns inside the frame that holds them — so without
    // this they stay put, and the pass below then drags the frame back to
    // match them, un-anchoring the whole cluster. Bounded by CONTAINMENT, so a
    // shift only ever propagates to what is drawn inside the moving element.
    for (int L = 0; L < 2; ++L) {
        const int other = 1 - L;
        for (int ty = 0; ty < kUiTileRows; ++ty) {
            for (int tx = 0; tx < kUiTileCols; ++tx) {
                const int id = lab[L][ty][tx];
                const int oid = lab[other][ty][tx];
                if (!id || !oid) continue;
                if (comp_shift[L][id] != 0 || comp_shift[other][oid] == 0)
                    continue;
                if (comp_x0[L][id] >= comp_x0[other][oid] &&
                    comp_x1[L][id] <= comp_x1[other][oid] &&
                    comp_y0[L][id] >= comp_y0[other][oid] &&
                    comp_y1[L][id] <= comp_y1[other][oid])
                    comp_shift[L][id] = comp_shift[other][oid];
            }
        }
    }

    // Same again for the vertical shift, and it matters MORE here: the HP/MP
    // readout is several disconnected BG2 components at different tile rows
    // inside one BG3 frame, so classifying them on their own moved the top row
    // of digits and left the rest behind — "10 / 12" rendered as "0 / 2" with
    // two stray 1s at the old height.
    for (int L = 0; L < 2; ++L) {
        const int other = 1 - L;
        for (int ty = 0; ty < kUiTileRows; ++ty) {
            for (int tx = 0; tx < kUiTileCols; ++tx) {
                const int id = lab[L][ty][tx];
                const int oid = lab[other][ty][tx];
                if (!id || !oid) continue;
                if (comp_shift_y[L][id] != 0 ||
                    comp_shift_y[other][oid] == 0) continue;
                if (comp_x0[L][id] >= comp_x0[other][oid] &&
                    comp_x1[L][id] <= comp_x1[other][oid] &&
                    comp_y0[L][id] >= comp_y0[other][oid] &&
                    comp_y1[L][id] <= comp_y1[other][oid])
                    comp_shift_y[L][id] = comp_shift_y[other][oid];
            }
        }
    }

    // The same separation, without any full-width furniture involved: a
    // dialogue's text lives on one layer INSIDE its frame on the other, and
    // the two are independent components. One cutscene box measured 22 wide
    // (left alone) with 14-wide text (anchored right), so the text slid out of
    // its own frame. Anything drawn on top of a component that is staying put
    // stays put with it.
    for (int pass = 0; pass < 2; ++pass) {
        for (int L = 0; L < 2; ++L) {
            const int other = 1 - L;
            for (int ty = 0; ty < kUiTileRows; ++ty) {
                for (int tx = 0; tx < kUiTileCols; ++tx) {
                    const int id = lab[L][ty][tx];
                    const int oid = lab[other][ty][tx];
                    if (!id || !oid) continue;
                    if (comp_shift[L][id] != 0 && comp_shift[other][oid] == 0)
                        comp_shift[L][id] = 0;
                    if (comp_shift_y[L][id] != 0 &&
                        comp_shift_y[other][oid] == 0)
                        comp_shift_y[L][id] = 0;
                }
            }
        }
    }

    // Pin the panel flush to the bottom-right of the WIDE view. Its tiles must
    // not also be anchored, or the two would fight; the explicit rect handling
    // in the providers owns it entirely.
    if (g_panel_found && g_panel_pin) {
        g_panel_dst_x = 240 + g_extra_left + g_extra_right - kPanelTilesW * 8;
        // Bottom-right, but above the bottom bar when one is up. Pinning to
        // the screen bottom regardless dropped the panel on top of the bar and
        // buried its MOV readout; the game itself puts the panel above it.
        g_panel_dst_y = g_bar_top_y - kPanelTilesH * 8;
        if (g_panel_dst_y < 0) g_panel_dst_y = 0;
        for (int L = 0; L < 2; ++L)
            for (int ty = 0; ty < kUiTileRows; ++ty)
                for (int tx = 0; tx < kUiTileCols; ++tx) {
                    const int sx = tx * 8, sy = ty * 8;
                    if (sx >= g_panel_src_x &&
                        sx < g_panel_src_x + kPanelTilesW * 8 &&
                        sy >= g_panel_src_y &&
                        sy < g_panel_src_y + kPanelTilesH * 8 &&
                        lab[L][ty][tx])
                        comp_shift[L][lab[L][ty][tx]] = 0;
                }
    }

    // ── The terrain box is not one component ──────────────────────────
    // It has no frame: it is a loose cluster of glyphs and icons, and the
    // segmentation splits it. Its right-hand part reaches column 29, passes
    // the edge test and anchors; the element dots are a separate 1x3 island
    // at column 24 with two blank columns between, so they touch nothing and
    // stay behind in mid-screen. Containment does not reach them either —
    // they are beside the anchored component, not inside it.
    //
    // Fixed by position rather than by a proximity rule, because this game
    // already identifies that furniture by position: the second screen
    // rasterises it from exactly this rect (kol_terrain_probe.cpp, cols
    // 23..29 x rows 0..8), and terrain_box_on_screen() is the game's own test
    // for whether it is really drawn there rather than a NAVI dialogue. A
    // proximity rule would have been a new heuristic over every screen; this
    // cannot affect anything but this box.
    //
    // NOTE the box RELOCATES to cols 15..21 when a NAVI bar is up, and
    // terrain_box_on_screen() is false then, so this correctly does nothing.
    if (g_hud_anchor && kol::terrain_box_on_screen()) {
        constexpr int kTx0 = 23, kTx1 = kUiTileCols - 1, kTy0 = 0, kTy1 = 8;
        auto inside = [&](int L, int id) {
            return comp_x0[L][id] >= kTx0 && comp_x1[L][id] <= kTx1 &&
                   comp_y0[L][id] >= kTy0 && comp_y1[L][id] <= kTy1;
        };
        int sx = 0, sy = 0;
        for (int L = 0; L < 2; ++L)
            for (int id = 1; id <= comp_count[L]; ++id) {
                if (!inside(L, id)) continue;
                if (comp_shift[L][id] != 0)   sx = comp_shift[L][id];
                if (comp_shift_y[L][id] != 0) sy = comp_shift_y[L][id];
            }
        if (sx != 0 || sy != 0)
            for (int L = 0; L < 2; ++L)
                for (int id = 1; id <= comp_count[L]; ++id)
                    if (inside(L, id)) {
                        comp_shift[L][id]   = sx;
                        comp_shift_y[L][id] = sy;
                    }
    }

    for (int L = 0; L < 2; ++L)
        for (int ty = 0; ty < kUiTileRows; ++ty)
            for (int tx = 0; tx < kUiTileCols; ++tx) {
                g_ui_occ[L][ty][tx] = occ[L][ty][tx];
                g_ui_shift[L][ty][tx] = static_cast<signed char>(
                    lab[L][ty][tx] ? comp_shift[L][lab[L][ty][tx]] : 0);
                g_ui_shift_y[L][ty][tx] = static_cast<signed char>(
                    lab[L][ty][tx] ? comp_shift_y[L][lab[L][ty][tx]] : 0);
            }
    g_ui_shift_valid = true;

    // Plain edge rows: see g_plain_row.
    {
        const std::uint16_t cnt[2] = {
            static_cast<std::uint16_t>(g_io[0x0C] | (g_io[0x0D] << 8)),
            static_cast<std::uint16_t>(g_io[0x0E] | (g_io[0x0F] << 8))};
        // The layer that runs edge to edge (BG3 by preference): after the
        // thunder's flash the opening's letterbox is left on BG2 alone (the
        // flash filled BG3 and cleared it), and taken for BG3 only, its bars
        // were anchored as boxes and no longer continued: 240 wide at the
        // view's top and bottom edges (HW).
        auto full_layer = [&](int ty) {
            for (int L = 1; L >= 0; --L) {
                bool all = true;
                for (int tx = 0; tx < kUiTileCols && all; ++tx) all = occ[L][ty][tx];
                if (all) return L;
            }
            return -1;
        };
        auto plain = [&](int ty) {
            const int full = full_layer(ty);
            if (full < 0) return false;
            for (int L = 0; L < 2; ++L) {
                const std::uint16_t e = bg_tile_entry(cnt[L], 0, ty);
                // Nothing at the sampled tile: the continuation shows the
                // field alone (the narration's text starts 3 columns in).
                // A field layer (g_ui_field) is drawn whole on its own.
                if ((L != full && (e & 0x3FFu) == 0u) || g_ui_field[L]) continue;
                if (bg_tile_entry(cnt[L], 1, ty) != e ||
                    bg_tile_entry(cnt[L], kUiTileCols - 1, ty) != e)
                    return false;
            }
            return true;
        };
        for (int ty = 0; ty < kUiTileRows; ++ty) g_plain_row[ty] = false;
        for (int ty = 0; ty < kUiTileRows && plain(ty); ++ty)
            g_plain_row[ty] = true;
        for (int ty = kUiTileRows - 1; ty >= 0 && plain(ty); --ty)
            g_plain_row[ty] = true;
        for (int ty = 0; ty < kUiTileRows; ++ty) {
            const int full = full_layer(ty);
            g_plain_row_dy[ty] = g_ui_shift_y[full < 0 ? 1 : full][ty][0];
            if (g_plain_row[ty]) g_any_plain_row = true;
        }
    }

    // KOL_WS_UI_DUMP=1 prints the anchoring decision per UI tile cell: '.'
    // empty, '=' occupied and not shifted, '>' shifted right, '<' shifted
    // left. A cut or displaced HUD element is a disagreement between adjacent
    // cells of one element, and that is only visible as a grid.
    // KOL_WS_UI_DUMP=N prints the Nth rebuild's grid (1 = the first).
    if (env_on("KOL_WS_UI_DUMP")) {
        static long calls = 0;
        if (++calls == std::atol(std::getenv("KOL_WS_UI_DUMP"))) {
            for (int L = 0; L < 2; ++L) {
                std::fprintf(stderr,
                             "[ui] BG%d  (extra %d/%d, side panel hw %d..%d "
                             "shift %d)\n", L + 2, g_extra_left, g_extra_right,
                             g_side_panel_x0, g_side_panel_x1,
                             g_side_panel_shift);
                for (int ty = 0; ty < kUiTileRows; ++ty) {
                    std::fprintf(stderr, "  r%02d ", ty);
                    for (int tx = 0; tx < kUiTileCols; ++tx) {
                        const int sh = g_ui_shift[L][ty][tx];
                        const int sv = g_ui_shift_y[L][ty][tx];
                        char c = !g_ui_occ[L][ty][tx] ? '.'
                               : sv < 0 ? '^'
                               : sv > 0 ? 'v'
                               : sh == 0 ? '='
                               : sh > 0 ? '>' : '<';
                        std::fputc(c, stderr);
                    }
                    std::fputc('\n', stderr);
                }
            }
            std::fflush(stderr);
        }
    }
}

bool panel_in_target(int output_x, int screen_y) {
    return g_panel_pin && g_panel_found && !g_panel_hide &&
           output_x >= g_panel_dst_x &&
           output_x < g_panel_dst_x + kPanelTilesW * 8 &&
           screen_y >= g_panel_dst_y &&
           screen_y < g_panel_dst_y + kPanelTilesH * 8;
}

// Where the guest actually drew it — blanked, because it now lives elsewhere.
// In NATIVE hardware coordinates, so the anchored lookup can ask the same
// question about a cell it is considering serving.
bool hw_in_panel_source(int hw, int screen_y) {
    if (!g_panel_pin || !g_panel_found) return false;
    return hw >= g_panel_src_x && hw < g_panel_src_x + kPanelTilesW * 8 &&
           screen_y >= g_panel_src_y &&
           screen_y < g_panel_src_y + kPanelTilesH * 8;
}

bool panel_in_source(int output_x, int screen_y) {
    return hw_in_panel_source(output_x - g_extra_left, screen_y);
}

// The scanline a plain edge row (g_plain_row) continues through this output
// row from, after its vertical anchoring; -1 when none does.
int plain_row_source(int screen_y) {
    if (!g_any_plain_row || !g_extend_bottom_bar) return -1;
    const int cand_y[3] = {0, g_extra_bottom, -g_extra_top};
    for (int j = 0; j < 3; ++j) {
        const int hwy = screen_y - cand_y[j];
        if (hwy < 0 || hwy >= kUiTileRows * 8) continue;
        const int ty = hwy / 8;
        if (g_plain_row[ty] && g_plain_row_dy[ty] == cand_y[j]) return hwy;
    }
    return -1;
}

// Which scanline should this output column sample to continue the side panel
// into a vertical margin row? -1 when the column is not the panel's, the panel
// is absent, or no repeat period was found. See the notes beside
// g_side_panel_period_y.
int side_panel_continuation_row(int bg, int output_x, int screen_y);
bool anchored_ui_hw(int bg, int output_x, int screen_y,
                    int* out_hw_x, int* out_hw_y);

// ── Smooth dialog boxes ─────────────────────────────────────────────────
// The game opens a dialog box in two steps, a short bar (4 frames) then the
// box's whole width as a bar (5), and closes it the other way, the bar (4)
// then the short bar (4): BG3 tiles, the box's frame (0x19-0x20) two rows
// tall at its middle row. Asked 2026-10-27 (states/JU, Eleanor's house)
// for it to grow or shrink linearly instead, the user's pick A of
// three mockups (shots in the session scratchpad dialog/mock): the same two
// phases, a size every frame. The box's place and size are the text block's
// descriptor ([0x0200283C] + 0x1B93: left column, inner width, middle row,
// inner height), set before the first step and kept through the close. Each
// composed frame (at its first BG3 provider call, from live VRAM) the frame
// tiles inside that box give the game's step; a pixel inside our rectangle
// samples the game's bar 9-slice-wise (its 8-pixel ends, its middle repeated,
// its top and bottom tile rows, its paper lines in turn down the middle), at the
// shift anchoring gave the bar's cells; a pixel of the game's bar outside our
// rectangle is not drawn. The expanded view only (the native renderer never
// asks the providers). Options > Graphics > Smooth Boxes.
unsigned long g_box_frame = 0;        // guest frames (the frame hook)
unsigned long g_box_done = ~0ul;      // the frame box_anim_update last ran for
enum class BoxStep { None, Short, Bar, Full, Other };
enum class BoxPhase { Idle, Open1, Open2, Close1, Close2 };
struct BoxAnim {
    BoxPhase phase = BoxPhase::Idle;
    BoxStep last = BoxStep::Other;
    int k = 0;                        // frames into the phase
    int c0 = 0, c1 = 0, r0 = 0, r1 = 0, mid = 0;   // the whole box, tiles
    int sx0 = 0, sx1 = 0, sy0 = 0, sy1 = 0;        // the game's bar, hardware px
    int bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;        // ours, hardware px
    int shift_x = 0, shift_y = 0;                  // the bar's anchoring
} g_box;
constexpr int kBoxBg = 3;
constexpr int kOpen1 = 4, kOpen2 = 5, kClose1 = 4, kClose2 = 4;   // the game's frames a step

void box_anim_update() {
    if (g_box_done == g_box_frame) return;
    g_box_done = g_box_frame;
    BoxAnim& b = g_box;
    const BoxStep prev = b.last;
    b.last = BoxStep::Other;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !g_io || !kol::setting_on(kol::Setting::SmoothBoxes)) { b.phase = BoxPhase::Idle; return; }
    const std::uint8_t* ew = bus->ewram_ptr();
    constexpr std::uint32_t kEwBlockPtr = kol::usa(0x0200283Cu) - 0x02000000u;   // u32: the window control block
    const std::uint32_t blk = ew[kEwBlockPtr] | (ew[kEwBlockPtr + 1] << 8) | (ew[kEwBlockPtr + 2] << 16) |
                              (static_cast<std::uint32_t>(ew[kEwBlockPtr + 3]) << 24);
    if (blk < 0x02000000u || blk + kol::usa_ctl(0x1B93u) + 4u >= 0x02040000u) { b.phase = BoxPhase::Idle; return; }
    const std::uint8_t* d = ew + (blk - 0x02000000u) + kol::usa_ctl(0x1B93u);
    const int left = d[0], inner_w = d[1], mid = d[2], inner_h = d[3];
    b.c0 = left; b.c1 = left + inner_w + 1;
    if (inner_w < 2 || inner_h < 2 || mid - inner_h / 2 < 0 || b.c1 >= 32) { b.phase = BoxPhase::Idle; return; }
    // The frame tiles in the box's columns: the game's step. The middle row
    // is the box's own (3); a box at the screen's foot (the ship's talk:
    // rows 12-19) is that many rows lower, read off the tiles: two rows are
    // the bar at the middle row, the box's height the box.
    const std::uint8_t* map = bus->vram_ptr() + ((io16(g_io, 0x0E) >> 8) & 31u) * 0x800u;
    int tc0 = 99, tc1 = -1, tr0 = 99, tr1 = -1;
    for (int r = 0; r < 32; ++r)
        for (int c = b.c0; c <= b.c1; ++c) {
            const unsigned t = (map[(r * 32 + c) * 2] | (map[(r * 32 + c) * 2 + 1] << 8)) & 0x3FFu;
            if (t < 0x19u || t > 0x20u) continue;
            tc0 = std::min(tc0, c); tc1 = std::max(tc1, c); tr0 = std::min(tr0, r); tr1 = std::max(tr1, r);
        }
    static const bool verbose = env_on("KOL_BOX_ANIM_LOG");
    if (verbose) {   // the descriptor and every frame tile on BG3, as they change
        int ac0 = 99, ac1 = -1, ar0 = 99, ar1 = -1;
        for (int r = 0; r < 32; ++r)
            for (int c = 0; c < 32; ++c) {
                const unsigned t = (map[(r * 32 + c) * 2] | (map[(r * 32 + c) * 2 + 1] << 8)) & 0x3FFu;
                if (t < 0x19u || t > 0x20u) continue;
                ac0 = std::min(ac0, c); ac1 = std::max(ac1, c); ar0 = std::min(ar0, r); ar1 = std::max(ar1, r);
            }
        static int seen[8] = {-1};
        const int now[8] = {left, inner_w, mid, inner_h, ac0, ar0, ac1, ar1};
        if (!std::equal(now, now + 8, seen)) {
            std::copy(now, now + 8, seen);
            std::fprintf(stderr, "box_anim: frame %lu desc %d %d %d %d, BG3 frame tiles %d,%d..%d,%d\n", g_box_frame,
                         left, inner_w, mid, inner_h, ac0, ar0, ac1, ar1);
        }
    }
    int down = 0;   // the rows below the descriptor's place
    if (tc1 >= 0 && tr1 - tr0 == 1) down = tr0 - mid;
    else if (tc1 >= 0 && tr1 - tr0 == inner_h + 1) down = tr0 - (mid - inner_h / 2);
    else if (b.phase != BoxPhase::Idle) down = b.mid - mid;   // as it was (gone, or another window)
    b.mid = mid + down;
    b.r0 = b.mid - inner_h / 2; b.r1 = b.r0 + inner_h + 1;
    if (b.r0 < 0 || b.r1 >= 32) { b.phase = BoxPhase::Idle; return; }
    BoxStep step = BoxStep::Other;
    if (tc1 < 0) step = BoxStep::None;
    else if (tc0 == b.c0 && tc1 == b.c1 && tr0 == b.r0 && tr1 == b.r1) step = BoxStep::Full;
    else if (tr0 == b.mid && tr1 == b.mid + 1 && tc0 == b.c0 && tc1 == b.c1) step = BoxStep::Bar;
    else if (tr0 == b.mid && tr1 == b.mid + 1 && tc1 - tc0 == 3) step = BoxStep::Short;
    b.last = step;
    // The phase: by the step and the one before it.
    BoxPhase next = BoxPhase::Idle;
    if (step == BoxStep::Short)
        next = prev == BoxStep::None ? BoxPhase::Open1 : prev == BoxStep::Bar ? BoxPhase::Close2
             : (b.phase == BoxPhase::Open1 || b.phase == BoxPhase::Close2) ? b.phase : BoxPhase::Idle;
    else if (step == BoxStep::Bar)
        next = prev == BoxStep::Short ? BoxPhase::Open2 : prev == BoxStep::Full ? BoxPhase::Close1
             : (b.phase == BoxPhase::Open2 || b.phase == BoxPhase::Close1) ? b.phase : BoxPhase::Idle;
    b.k = next == b.phase ? b.k + 1 : 0;
    // KOL_BOX_ANIM_LOG=1: each phase as it starts, and the box.
    static const bool log = env_on("KOL_BOX_ANIM_LOG");
    if (log && next != b.phase)
        std::fprintf(stderr, "box_anim: frame %lu phase %d (box %d,%d..%d,%d mid %d)\n", g_box_frame,
                     static_cast<int>(next), b.c0, b.r0, b.c1, b.r1, mid);
    b.phase = next;
    if (next == BoxPhase::Idle) return;
    b.sx0 = tc0 * 8; b.sx1 = (tc1 + 1) * 8; b.sy0 = tr0 * 8; b.sy1 = (tr1 + 1) * 8;
    // Ours: the box's centre, a size along the phase (as the game's steps:
    // the width over the short bar's frames, the height over the bar's).
    const int W = (b.c1 - b.c0 + 1) * 8, H = (b.r1 - b.r0 + 1) * 8, Ws = 32, Hb = 16;
    auto at = [&](int n) { return static_cast<float>(std::min(b.k, n - 1) + 1) / static_cast<float>(n + 1); };
    int w = W, h = Hb;
    switch (b.phase) {
        case BoxPhase::Open1: w = Ws + static_cast<int>(std::lround((W - Ws) * at(kOpen1))); break;
        case BoxPhase::Open2: h = Hb + static_cast<int>(std::lround((H - Hb) * at(kOpen2))); break;
        case BoxPhase::Close1: h = H - static_cast<int>(std::lround((H - Hb) * at(kClose1))); break;
        case BoxPhase::Close2: w = W - static_cast<int>(std::lround(W * at(kClose2))); break;
        default: break;
    }
    const int cx = (b.c0 + b.c1 + 1) * 4, cy = (b.r0 + b.r1 + 1) * 4;
    b.bx0 = cx - w / 2; b.bx1 = b.bx0 + w;
    b.by0 = cy - h / 2; b.by1 = b.by0 + h;
    // The bar's anchoring (kol_bg_x_provider's), for all of ours.
    b.shift_x = b.shift_y = 0;
    if (g_hud_anchor && g_ui_shift_valid && tr0 < kUiTileRows && tc0 < kUiTileCols && g_ui_occ[kBoxBg - 2][tr0][tc0]) {
        b.shift_x = g_ui_shift[kBoxBg - 2][tr0][tc0];
        b.shift_y = g_ui_shift_y[kBoxBg - 2][tr0][tc0];
    }
}

// The 9-slice: our coordinate u in [lo, hi) to the game's bar [slo, shi),
// its 8-pixel ends kept and its middle repeated: across, the middle tile;
// down (`rows`), the bar's paper lines 6-9 in turn, on from the top tile's
// 6 and 7 (one line repeated striped the paper, whose strokes run
// diagonally).
int box_slice(int u, int lo, int hi, int slo, int shi, bool rows) {
    const int n = hi - lo, a = u - lo, end = std::min(8, n / 2);
    if (a < end) return slo + a;
    if (hi - u <= std::min(8, n - n / 2)) return shi - (hi - u);
    return rows ? slo + 6 + (a - 6) % 4 : slo + 8 + (a - 8) % 8;
}

// For BG3 at an output pixel: 1 with the hardware pixel to sample, -1 not
// drawn, 0 not the animation's.
int box_anim_sample(int output_x, int screen_y, int* hx, int* hy) {
    box_anim_update();
    const BoxAnim& b = g_box;
    if (b.phase == BoxPhase::Idle) return 0;
    const int x = output_x - g_extra_left - b.shift_x, y = screen_y - b.shift_y;
    if (x >= b.bx0 && x < b.bx1 && y >= b.by0 && y < b.by1) {
        *hx = box_slice(x, b.bx0, b.bx1, b.sx0, b.sx1, false);
        *hy = box_slice(y, b.by0, b.by1, b.sy0, b.sy1, true);
        return 1;
    }
    if (x >= b.sx0 && x < b.sx1 && y >= b.sy0 && y < b.sy1) return -1;
    return 0;
}

// A row the animation may touch (the batched X provider leaves it to the
// per-pixel one).
bool box_anim_row(int screen_y) {
    box_anim_update();
    const BoxAnim& b = g_box;
    if (b.phase == BoxPhase::Idle) return false;
    const int y = screen_y - b.shift_y;
    return (y >= b.by0 && y < b.by1) || (y >= b.sy0 && y < b.sy1);
}

int bg_y_provider_inner(int bg, int output_x, int screen_y, int* out_src_y) {
    // The title's dark veil (BG1's field) past the game's 160 lines.
    {
        int hx = 0;
        if (kol::title_veil(bg, screen_y, &hx, out_src_y)) return 1;
    }
    if (bg == kBoxBg) {   // a dialog box opening or closing (box_anim_sample)
        int hx = 0, hy = 0;
        const int r = box_anim_sample(output_x, screen_y, &hx, &hy);
        if (r > 0) *out_src_y = hy;
        if (r) return r;
    }
    // Layer guard: the X provider has always had one, but this callback did
    // not need it while its layer mask was UI-only. The zoom probe widened the
    // mask to the terrain layers and promptly remapped terrain Y into the
    // panel's source rows.
    if (g_hud_anchor && (ui_layers() & (1u << bg)) && bg >= 2 && g_ui_field[bg - 2]) {
        *out_src_y = ((screen_y % kBarPhase) + kBarPhase) % kBarPhase;   // a field
        return 1;
    }
    if ((ui_layers() & (1u << bg)) && panel_in_target(output_x, screen_y)) {
        *out_src_y = g_panel_src_y + (screen_y - g_panel_dst_y);
        return 1;
    }
    const int cont = side_panel_continuation_row(bg, output_x, screen_y);
    if (cont >= 0) { *out_src_y = cont; return 1; }
    // Vertical anchoring: a box that followed an edge outward samples the row
    // it was drawn on, not the row it now occupies.
    if (g_hud_anchor && (ui_layers() & (1u << bg)) &&
        (g_extra_top || g_extra_bottom)) {
        int hwx = 0, hwy = 0;
        if (anchored_ui_hw(bg, output_x, screen_y, &hwx, &hwy) &&
            hwy != screen_y) {
            *out_src_y = hwy;
            return 1;
        }
        const int plain_y = plain_row_source(screen_y);
        if (plain_y >= 0 && plain_y != screen_y) {
            *out_src_y = plain_y;
            return 1;
        }
        // The bottom bar's horizontal continuation samples a synthetic column;
        // give it the bar's own row once the bar has moved down with the edge.
        if (g_bar_shift_y != 0 && g_extend_bottom_bar && g_has_fullwidth_ui &&
            screen_y >= kBarTopY + g_bar_shift_y &&
            screen_y < kUiTileRows * 8 + g_bar_shift_y) {
            *out_src_y = screen_y - g_bar_shift_y;
            return 1;
        }
    }
    return 0;
}

// The battle confirmation's boxes at the top (the units' HP/MP, ATK/DEF,
// damage and hit, the action's word: the UI layers' rows 0-9, nothing else
// there while it is up) are left out while the second screen shows them
// (kol_sheet draw_sheet_forecast; the user, 2026-11-01, suggested
// hiding the whole top HP/MP and ATK/DEF part). Set each frame (g_forecast_off).
bool g_forecast_off = false;
// And while one of the player's units takes its action, the unit's HP/MP box
// at the top left (rows 0-3, columns 0-9) and the unit bar along the bottom
// (rows 17-19), whose unit the second screen's card shows (states/thor/0076,
// Allos moving; the user: with his card on the bottom screen, the top
// screen's top-left and bottom info is not needed). The terrain panel
// at the top right (columns 23-29) stays.
bool g_unit_box_off = false, g_unit_bar_off = false;
// The box's cells another window covers, kept (set with g_unit_box_off).
bool g_unit_box_window[4][10] = {};
int kol_bg_x_provider(int bg, int output_x, int screen_y, int* out_hw_x);
int kol_bg_y_provider(int bg, int output_x, int screen_y, int* out_src_y) {
    int src = screen_y;
    const int r = bg_y_provider_inner(bg, output_x, screen_y, &src);
    if (r < 0) return r;
    const int row = r > 0 ? src : screen_y;
    if (g_forecast_off && (ui_layers() & (1u << bg)) && row >= 0 && row < 80)
        return -1;
    if ((g_unit_box_off || g_unit_bar_off) && (ui_layers() & (1u << bg))) {
        if (g_unit_bar_off && row >= 17 * 8 && row < 160) return -1;
        if (g_unit_box_off && row >= 0 && row < 4 * 8) {
            int hx = output_x - static_cast<int>(g_native_left);
            if (kol_bg_x_provider(bg, output_x, screen_y, &hx) < 0) return -1;
            if (hx >= 0 && hx < 10 * 8 && !g_unit_box_window[row / 8][hx / 8]) return -1;
        }
    }
    if (r > 0) *out_src_y = src;
    return r;
}

// Inverse of the per-element shift: which hardware X does this output column
// take its UI pixel from? Returns false when no element lands here.
bool anchored_ui_hw(int bg, int output_x, int screen_y,
                    int* out_hw_x, int* out_hw_y) {
    if (!g_ui_shift_valid || bg < 2 || bg > 3) return false;
    const int cand_x[3] = {0, g_extra_right, -g_extra_left};
    const int cand_y[3] = {0, g_extra_bottom, -g_extra_top};
    for (int j = 0; j < 3; ++j) {
        // A cell shifted by cand_y[j] was drawn cand_y[j] higher up.
        const int hwy = screen_y - cand_y[j];
        if (hwy < 0 || hwy >= kUiTileRows * 8) continue;
        const int ty = hwy / 8;
        for (int i = 0; i < 3; ++i) {
            const int hw = output_x - g_extra_left - cand_x[i];
            if (hw < 0 || hw >= 240) continue;
            // The panel is pinned elsewhere, so its own cells are not
            // available here — otherwise the candidate that maps straight
            // back into the vacated rect wins, and whatever was anchored into
            // that space loses.
            if (hw_in_panel_source(hw, hwy)) continue;
            const int tx = hw / 8;
            if (g_ui_occ[bg - 2][ty][tx] &&
                g_ui_shift[bg - 2][ty][tx] == cand_x[i] &&
                g_ui_shift_y[bg - 2][ty][tx] == cand_y[j]) {
                *out_hw_x = hw;
                if (out_hw_y) *out_hw_y = hwy;
                return true;
            }
        }
        if (cand_y[j] == 0 && g_extra_top == 0 && g_extra_bottom == 0) break;
    }
    return false;
}

bool anchored_ui_hw_x(int bg, int output_x, int screen_y, int* out_hw_x) {
    return anchored_ui_hw(bg, output_x, screen_y, out_hw_x, nullptr);
}

// See the forward declaration above.
int side_panel_continuation_row(int bg, int output_x, int screen_y) {
    if (!g_extend_side_panel_v || !g_hud_anchor) return -1;
    if (bg != g_side_panel_bg || g_side_panel_period_y <= 0) return -1;
    if (g_side_panel_x1 < g_side_panel_x0) return -1;
    if (screen_y >= 0 && screen_y < kUiTileRows * 8) return -1;   // real row
    const int prd = g_side_panel_period_y;
    const int src_y = g_side_panel_top_y +
        (((screen_y - g_side_panel_top_y) % prd) + prd) % prd;
    int hw = 0;
    if (!anchored_ui_hw_x(bg, output_x, src_y, &hw)) return -1;
    if (hw < g_side_panel_x0 || hw > g_side_panel_x1) return -1;
    return src_y;
}


// Margin tilemap source. The guest composes the whole map in EWRAM and blits
// a moving window of it into VRAM, so past that window the tilemap is empty
// and the wrapped ring entry is meaningless. Hand the PPU the entry the guest
// itself would have written; see kol_map_source.cpp for how it is located and
// for the per-frame check that it agrees with everything already drawn.
int kol_tilemap_provider(int bg, int hw_x, int screen_y,
                         std::uint16_t* out_entry) {
    // Anything this does not serve must keep the PPU's established behaviour
    // (2 = use the wrapped hardware entry), NOT fall through to "nothing
    // here" — a UI layer that HUD anchoring has deliberately placed out in
    // the margin samples through this hook too, and answering 0 for it drops
    // those pixels. That regression showed up as state B rendering
    // differently at 284 with anchoring on.
    if (bg == 2 && g_bg2_circle && (hw_x < 0 || hw_x >= 240 || screen_y < 0 || screen_y >= 160)) return 0;   // (bg2_circle_update)
    if (!(g_margin_layers & (1u << bg))) return 2;   // kWsTilemapKeepWrapped
    if (bg == 2 && g_sea_bg2) return 2;              // the sea: its own field, wrapped
    std::uint16_t entry = 0;
    if (!kol::map_source_entry(bg, hw_x, screen_y, &entry))
        return kol::map_source_armed() ? 0 : 2;      // armed: off the map
    *out_entry = entry;
    return 1;   // kWsTilemapReplace
}

// Inside the native screen, an empty entry over a map extension's cell
// (kol_map_source.cpp, "Map extensions"): the painted scenery continues
// where the camera has brought the map's cut edge into the 240.
int kol_tilemap_fill_provider(int bg, int hw_x, int screen_y,
                              std::uint16_t* out_entry) {
    if (!(g_margin_layers & (1u << bg))) return 0;
    return kol::map_source_fill_entry(bg, hw_x, screen_y, out_entry) ? 1 : 0;
}

// Inside the native screen, the map's overrides (kol_map_source.cpp, "ov"
// lines): the editor's repainting of the game's own art. Asked only for the
// layers g_ws_tilemap_override_layers names, set each frame below.
int kol_tilemap_override_provider(int bg, int hw_x, int screen_y, std::uint16_t entry,
                                  std::uint16_t* out_entry) {
    if (!(g_margin_layers & (1u << bg))) return 0;
    return kol::map_source_override_entry(bg, hw_x, screen_y, entry, out_entry) ? 1 : 0;
}

// The tile grid kol_tilemap_provider's answer is constant over, for the PPU's
// once-per-tile lookup (g_ws_tilemap_provider_x_phase). Only the composed-map
// branch looks at hw_x, through floor((hw_x + fine_x) / 8); every other branch
// answers the same for the whole row, which holds on any grid, so report the
// line's own HOFS phase and the PPU's check passes by construction.
int kol_tilemap_x_phase(int bg) {
    const int fx = kol::map_source_x_phase(bg);
    if ((g_margin_layers & (1u << bg)) && fx >= 0) return fx;
    if (!g_io || bg < 0 || bg > 3) return -1;
    return io16(g_io, 0x10 + 4u * static_cast<unsigned>(bg)) & 7;
}

}  // namespace

bool kol::ui_sprite_shift(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
                          int* dx, int* dy) {
    // KOL_WS_NO_UI_SPRITE_SHIFT=1 leaves interface sprites where the game put
    // them, as before.
    static const bool off = env_on("KOL_WS_NO_UI_SPRITE_SHIFT");
    if (off || !g_hud_anchor || !g_ui_shift_valid) return false;
    if (((attr2 >> 10) & 3u) != 0u) return false;         // not interface
    if (attr0 & 0x0300u) return false;                    // affine, or disabled
    const unsigned shape = (attr0 >> 14) & 3u, size = (attr1 >> 14) & 3u;
    if (shape == 3u) return false;
    const int w = kol::kObjW[shape][size], h = kol::kObjH[shape][size];
    int x = static_cast<int>(attr1 & 0x1FFu);
    if (x & 0x100) x -= 0x200;
    int y = static_cast<int>(attr0 & 0xFFu);
    if (y >= 160) y -= 256;
    // The grid cursor's hand is interface too, but it points at a cell of
    // the map: over an anchored box (the HP/MP bars, top left) it went to
    // the corner with the box (states/FO_slot1_hand_with_hp_bar).
    if (kol::grid_cursor_sprite(x, y, w, h)) return false;
    // The area on each moved element, told apart by its shift.
    int best_area = 0, best_dx = 0, best_dy = 0;
    struct Part { int dx, dy, area; } parts[8];
    int np = 0;
    for (int ty = std::max(0, y / 8); ty <= std::min(kUiTileRows - 1, (y + h - 1) / 8); ++ty)
        for (int tx = std::max(0, x / 8); tx <= std::min(kUiTileCols - 1, (x + w - 1) / 8); ++tx) {
            int sx = 0, sy = 0;
            bool moved = false;
            for (int L = 0; L < 2 && !moved; ++L)
                if (g_ui_occ[L][ty][tx] && (g_ui_shift[L][ty][tx] || g_ui_shift_y[L][ty][tx])) {
                    sx = g_ui_shift[L][ty][tx];
                    sy = g_ui_shift_y[L][ty][tx];
                    moved = true;
                }
            if (!moved) continue;
            const int ax = std::max(x, tx * 8), bx = std::min(x + w, tx * 8 + 8);
            const int ay = std::max(y, ty * 8), by = std::min(y + h, ty * 8 + 8);
            const int area = (bx - ax) * (by - ay);
            int i = 0;
            while (i < np && (parts[i].dx != sx || parts[i].dy != sy)) ++i;
            if (i == np) {
                if (np == 8) continue;
                parts[np++] = {sx, sy, 0};
            }
            parts[i].area += area;
            if (parts[i].area > best_area) {
                best_area = parts[i].area;
                best_dx = sx;
                best_dy = sy;
            }
        }
    if (2 * best_area <= w * h) return false;
    *dx = best_dx;
    *dy = best_dy;
    return true;
}

namespace {

// The roster panel's portraits and its selection brackets are OBJ sprites, and
// no BG provider moves sprites — anchoring the frame alone slid it out from
// under its own contents, which is the same way the ARMS box broke. So when a
// side panel has been anchored, everything drawn inside its native footprint
// travels with it. The framework calls this only from the expanded renderer,
// so the faithful 240 path is untouched.
//
// Returning non-zero SKIPS the hardware's signed decode, so do it here.
//
// Otherwise, a sprite the guest recorded in its slot list takes the true X it
// recorded there (kol_obj_cull.cpp, obj_true_x): OAM's 9-bit X has two
// readings 512 apart, and at 480 wide both can be on screen.
int kol_obj_attr_x_now(std::uint16_t attr0, std::uint16_t attr1,
                       std::uint16_t attr2, int* out_x) {
    if (!out_x) return 0;
    // An interface sprite on an anchored element (the menu's hand on its box)
    // moves with it; kol_obj_cull.cpp's Y provider asks the same question.
    int ui_dx = 0, ui_dy = 0;
    if (kol::ui_sprite_shift(attr0, attr1, attr2, &ui_dx, &ui_dy)) {
        int x = static_cast<int>(attr1 & 0x1FFu);
        if (x & 0x100) x -= 0x200;
        *out_x = x + ui_dx;
        return 1;
    }
    if (g_hud_anchor && g_side_panel_shift != 0 &&
        g_side_panel_x1 >= g_side_panel_x0 &&
        ((attr0 >> 8) & 3u) != 2u) {                  // not disabled
        int x = static_cast<int>(attr1 & 0x1FFu);
        if (x & 0x100) x -= 0x200;
        const int y = static_cast<int>(attr0 & 0xFFu);
        // A panel anchored at the left edge owns what is left of it too: while
        // it slides in or out (view mode, L) the game draws the portraits it
        // has pushed off its own screen at negative X, which the widened view
        // would otherwise show over the map for a frame or two.
        const bool from_left = g_side_panel_x0 <= 0;
        if (y < 160 && (from_left || x >= g_side_panel_x0) && x <= g_side_panel_x1) {
            // A unit the guest recorded in its world slot list stands on the
            // map: it keeps its true X even inside the panel's footprint,
            // where the battle-preparation deployment tiles are
            // (states/BD_units_invisible: Alphonse and Lechies were carried
            // behind the anchored panel). Only an EXACT match counts here: the
            // portraits are 16x16 like units, and the near match that resolves
            // scrolling took Deneb's for a unit standing behind the panel and
            // left it floating over the map (states/BH_deneb_floating).
            if (kol::obj_true_x(attr0, attr1, out_x, false)) return 1;
            // Only the game's interface is the panel's: its portraits,
            // brackets and arrows are priority 0 (BD, BF, BH, BP, FP), unit
            // parts 1 or 2. A world sprite the slot list does not predict
            // stays on the map: it predicts 16-wide units, so a 32-wide
            // monster's left pieces went off with the panel, half the dragon
            // gone and its pieces loose to the left under the perspective
            // (states/FP_slot1_dragon_cut_stick).
            if (((attr2 >> 10) & 3u) != 0u)
                return kol::obj_true_x(attr0, attr1, out_x) ? 1 : 0;
            *out_x = x + g_side_panel_shift;
            return 1;
        }
    }
    return kol::obj_true_x(attr0, attr1, out_x) ? 1 : 0;
}

// Kept per entry for the frame, as the Y provider is (kol::obj_attr_new_frame).
int kol_obj_attr_x(int oam_index, std::uint16_t attr0, std::uint16_t attr1,
                   std::uint16_t attr2, int* out_x) {
    if (!out_x) return 0;
    if (oam_index < 0 || oam_index >= 128) return kol_obj_attr_x_now(attr0, attr1, attr2, out_x);
    struct Memo {
        std::uint64_t epoch = 0;
        std::uint16_t a0 = 0, a1 = 0, a2 = 0;
        int ret = 0, value = 0;
    };
    static Memo memo[128];
    Memo& m = memo[oam_index];
    const std::uint64_t epoch = kol::obj_attr_epoch();
    if (m.epoch != epoch || m.a0 != attr0 || m.a1 != attr1 || m.a2 != attr2) {
        int v = 0;
        m.ret = kol_obj_attr_x_now(attr0, attr1, attr2, &v);
        m.value = v;
        m.epoch = epoch;
        m.a0 = attr0; m.a1 = attr1; m.a2 = attr2;
    }
    if (m.ret) *out_x = m.value;
    return m.ret;
}

// A GBA tilemap wraps. Beyond the authentic 240 columns that wrap is not
// scenery, it is a second copy of whatever the layer happens to hold — which
// is how the battle HUD ended up duplicated into the left margin the first
// time this was tried. Suppress every layer the scene policy has not cleared
// as world content; inside the authentic 240 nothing is touched.
int kol_bg_x_provider(int bg, int output_x, int screen_y, int* out_hw_x) {
    // The title's veil past the game's 160 lines: BG1's field (the Y
    // provider gives the row).
    {
        int hx = 0, sy = 0;
        if (kol::title_veil(bg, screen_y, &hx, &sy)) { *out_hw_x = hx; return 1; }
    }
    // The opening's wobbling fire past line 160: the wave carried on.
    {
        int dx = 0;
        if (kol::opening_wobble(bg, screen_y, &dx)) {
            *out_hw_x = output_x - static_cast<int>(g_native_left) + dx;
            return 1;
        }
    }
    if (bg == kBoxBg) {   // a dialog box opening or closing (box_anim_sample)
        int hx = 0, hy = 0;
        const int r = box_anim_sample(output_x, screen_y, &hx, &hy);
        if (r > 0) *out_hw_x = hx;
        if (r) return r;
    }
    const bool in_margin = output_x < static_cast<int>(g_native_left) ||
                           output_x >= static_cast<int>(g_native_right);
    // BG2 holding a spell's circle (bg2_circle_update) is no interface: not
    // anchored, not continued. Taken for a bar, its rows ran on into the right
    // border as straight bands of one repeated motif (states/thor/0236, the
    // save's own picture, as Descent's circle faded).
    if (bg == 2 && g_bg2_circle) return in_margin ? -1 : 0;

    // UI layers under anchoring are placed across the WHOLE output width, so
    // this runs before the margin split: an element anchored to the right edge
    // deliberately lands in what used to be margin.
    // Panel pinning owns its two rectangles outright and runs before anchoring.
    if (g_panel_pin && g_panel_found && (ui_layers() & (1u << bg))) {
        if (panel_in_target(output_x, screen_y)) {
            *out_hw_x = g_panel_src_x + (output_x - g_panel_dst_x);
            return 1;
        }
        // The source rect is blanked BELOW, after anchoring, not here. Pinning
        // vacates that space and anchoring may then legitimately move another
        // element into it: the battle-preparation screen anchors its "L
        // VIEWMODE" / "R COMMAND" hints right, straight into the rect the
        // panel had just left, and blanking first erased all but their first
        // two glyphs. The panel's own cells cannot come back through anchoring
        // — anchored_ui_hw_x skips them — so ordering it this way loses
        // nothing and keeps whatever else lands there.
    }

    if (g_hud_anchor && (ui_layers() & (1u << bg))) {
        if (bg >= 2 && g_ui_field[bg - 2]) {   // a field: every pixel is the same
            const int hx = output_x - static_cast<int>(g_native_left);
            *out_hw_x = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
            return 1;
        }
        int hw = 0;
        if (anchored_ui_hw_x(bg, output_x, screen_y, &hw)) {
            *out_hw_x = hw;
            return 1;
        }
        // No element here. Continue a plain edge row, or the bottom bar's
        // field if we are in its band, otherwise draw nothing — the content
        // that used to be at this column has moved to its anchor.
        if (plain_row_source(screen_y) >= 0) {
            const int hx = output_x - static_cast<int>(g_native_left);
            *out_hw_x = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
            return 1;
        }
        if (g_extend_bottom_bar && g_has_fullwidth_ui &&
            screen_y >= kBarTopY + g_bar_shift_y &&
            screen_y < kUiTileRows * 8 + g_bar_shift_y) {
            const int hx = output_x - static_cast<int>(g_native_left);
            *out_hw_x = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
            return 1;
        }
        // A vertical margin row: the side panel continues, and a box anchored
        // to the new top/bottom edge lives out here now. Everything else
        // stays where the guest drew it and simply is not present.
        const int cont = side_panel_continuation_row(bg, output_x, screen_y);
        if (cont >= 0 && anchored_ui_hw_x(bg, output_x, cont, out_hw_x))
            return 1;
        return -1;
    }

    if (g_panel_pin && g_panel_found && (ui_layers() & (1u << bg)) &&
        panel_in_source(output_x, screen_y))
        return -1;

    if (!in_margin) {
        // Unanchored UI still samples its own column.
        return 0;
    }
    if (g_margin_layers & (1u << bg)) {
        if (bg == 2 && g_sea_bg2) return 0;   // the sea's field, wrapped
        // With the composed map available, every margin sample has a real
        // entry behind it and the contiguity heuristic has nothing left to
        // do: it exists to reject the stale ring content that showed through
        // where the guest's tilemap had run out. Keep it for the fallback.
        if (kol::map_source_armed()) return 0;
        const bool right_side = output_x >= static_cast<int>(g_native_right);
        return margin_row_is_contiguous(bg, screen_y, right_side) ? 0 : -1;
    }

    // Continue the bottom unit bar rather than clipping it at the old edge.
    if (g_extend_bottom_bar && g_has_fullwidth_ui &&
        (ui_layers() & (1u << bg)) && screen_y >= kBarTopY) {
        const int hx = output_x - static_cast<int>(g_native_left);
        *out_hw_x = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
        return 1;
    }
    return -1;
}

// Batched form of kol_bg_x_provider, for one whole output row.
//
// This exists ONLY to remove indirect-call traffic, and it is deliberately a
// plain loop over the per-pixel function above rather than a second, cleverer
// implementation of the same policy: the answer is then identical by
// construction, not by review. The HUD anchoring rules are untouched — this
// changes when they are evaluated, never what they decide.
//
// What it buys: the expanded renderer asked kol_bg_x_provider once per BG
// layer per output column per scanline, which at 480 wide is 307,222 indirect
// calls a frame (measured: 76,806 per layer per frame, four layers) and 26.8%
// of total frame time. Called this way it is 4 calls a scanline, 640 a frame,
// and the per-pixel body inlines into this loop because both live here.
//
// That was not enough. Per pixel, anchored_ui_hw tries up to nine candidate
// shifts, and with vertical margins on it cannot stop early, so every UI pixel
// with no HUD behind it — most of them — paid all nine. At a real 480x270 that
// was 39.5% of the whole frame (2,578 of 6,524 samples, battle state AU).
// anchored_ui_row below answers the same question for a whole row: see there.
// world_x_row does the same for the terrain layers, whose answer is constant
// per row and side. A UI layer with anchoring off still takes the plain loop.

namespace {

// The widest row the batched forms take: the widest the PPU composes (480
// plus a scene overscan of up to 48 a side).
constexpr int kMaxRow = 576;

// anchored_ui_hw for every column of one row, with the same answer per pixel.
//
// Per pixel, the answer is the FIRST candidate in (j, i) order whose tile is
// occupied with a matching shift and is not in the pinned panel's vacated
// rect. The occupancy/shift test depends on the hardware TILE only, so walking
// the candidates in that same order across the whole row, testing each tile
// once and filling only pixels that are still unresolved, yields the same
// first match for every pixel. The panel-source test stays per pixel. The
// early `break` is kept exactly as the per-pixel loop has it.
void anchored_ui_row(int bg, int screen_y, int x0, int count,
                     bool* found, int* out_hw) {
    for (int k = 0; k < count; ++k) found[k] = false;
    if (!g_ui_shift_valid || bg < 2 || bg > 3) return;
    const int cand_x[3] = {0, g_extra_right, -g_extra_left};
    const int cand_y[3] = {0, g_extra_bottom, -g_extra_top};
    const int L = bg - 2;
    for (int j = 0; j < 3; ++j) {
        const int hwy = screen_y - cand_y[j];
        if (hwy < 0 || hwy >= kUiTileRows * 8) continue;
        const int ty = hwy / 8;
        for (int i = 0; i < 3; ++i) {
            // Output column x samples hardware column hw = x - base.
            const int base = g_extra_left + cand_x[i];
            for (int tx = 0; tx < kUiTileCols; ++tx) {
                if (!(g_ui_occ[L][ty][tx] &&
                      g_ui_shift[L][ty][tx] == cand_x[i] &&
                      g_ui_shift_y[L][ty][tx] == cand_y[j]))
                    continue;
                const int k_lo = std::max(tx * 8 + base - x0, 0);
                const int k_hi = std::min(tx * 8 + 8 + base - x0, count);
                for (int k = k_lo; k < k_hi; ++k) {
                    if (found[k]) continue;
                    const int hw = x0 + k - base;
                    if (hw_in_panel_source(hw, hwy)) continue;
                    found[k] = true;
                    out_hw[k] = hw;
                }
            }
        }
        if (cand_y[j] == 0 && g_extra_top == 0 && g_extra_bottom == 0) break;
    }
}

// kol_bg_x_provider's UI-layer branch (g_hud_anchor on, bg in kUiLayers),
// for a whole row, in the same order: panel target, anchored element, bottom
// bar band, side-panel continuation, else nothing.
void anchored_x_row(int bg, int screen_y, int x0, int count,
                    signed char* out_action, int* out_hx) {
    if (bg >= 2 && g_ui_field[bg - 2]) {   // a field (as kol_bg_x_provider)
        for (int k = 0; k < count; ++k) {
            const int hx = x0 + k - static_cast<int>(g_native_left);
            out_action[k] = 1;
            out_hx[k] = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
        }
        return;
    }
    bool here[kMaxRow];
    int here_hw[kMaxRow];
    anchored_ui_row(bg, screen_y, x0, count, here, here_hw);

    const bool bar_band = plain_row_source(screen_y) >= 0 ||
                          (g_extend_bottom_bar && g_has_fullwidth_ui &&
                           screen_y >= kBarTopY + g_bar_shift_y &&
                           screen_y < kUiTileRows * 8 + g_bar_shift_y);
    // side_panel_continuation_row's per-row conditions; its column test is the
    // anchored lookup at src_y plus the [x0, x1] range, applied below.
    int cont_y = -1;
    if (g_extend_side_panel_v && g_hud_anchor && bg == g_side_panel_bg &&
        g_side_panel_period_y > 0 && g_side_panel_x1 >= g_side_panel_x0 &&
        !(screen_y >= 0 && screen_y < kUiTileRows * 8)) {
        const int prd = g_side_panel_period_y;
        cont_y = g_side_panel_top_y +
            (((screen_y - g_side_panel_top_y) % prd) + prd) % prd;
    }
    bool cont[kMaxRow];
    int cont_hw[kMaxRow];
    if (cont_y >= 0) anchored_ui_row(bg, cont_y, x0, count, cont, cont_hw);

    const bool pin = g_panel_pin && g_panel_found;
    for (int k = 0; k < count; ++k) {
        const int x = x0 + k;
        if (pin && panel_in_target(x, screen_y)) {
            out_action[k] = 1;
            out_hx[k] = g_panel_src_x + (x - g_panel_dst_x);
        } else if (here[k]) {
            out_action[k] = 1;
            out_hx[k] = here_hw[k];
        } else if (bar_band) {
            const int hx = x - static_cast<int>(g_native_left);
            out_action[k] = 1;
            out_hx[k] = ((hx % kBarPhase) + kBarPhase) % kBarPhase;
        } else if (cont_y >= 0 && cont[k] && cont_hw[k] >= g_side_panel_x0 &&
                   cont_hw[k] <= g_side_panel_x1) {
            out_action[k] = 1;
            out_hx[k] = cont_hw[k];
        } else {
            out_action[k] = -1;
            out_hx[k] = 0;
        }
    }
}

// kol_bg_x_provider for a layer outside kUiLayers, for a whole row. None of
// its UI-only branches can fire, so the answer is 0 inside the native 240 and,
// in a margin, one value per side that depends on the row alone.
void world_x_row(int bg, int screen_y, int x0, int count,
                 signed char* out_action, int* out_hx) {
    auto margin_action = [&](bool right_side) -> signed char {
        if (!(g_margin_layers & (1u << bg))) return -1;
        if (bg == 2 && g_sea_bg2) return 0;
        if (kol::map_source_armed()) return 0;
        return margin_row_is_contiguous(bg, screen_y, right_side) ? 0 : -1;
    };
    const signed char left = margin_action(false);
    const signed char right = margin_action(true);
    for (int k = 0; k < count; ++k) {
        const int x = x0 + k;
        out_action[k] = x < static_cast<int>(g_native_left) ? left
                      : x >= static_cast<int>(g_native_right) ? right : 0;
        out_hx[k] = 0;
    }
}


// A spell's closing circle (Thor state 0293, map 88, the user: the screen darkens as a circle
// closes on him, and at higher resolution the circle starts square). The game darkens
// everything outside WIN0 (BLDY, WINOUT's effect bit) and rewrites WIN0H every line by an
// HBlank DMA (DMA0, 0xA240, to 0x04000040) from a table of 160 entries (left << 8 | right,
// clamped to 0..250): a circle round the caster, closing in from beyond the screen. While
// it is wider than the screen its lit part was the native 240 x 160, a rectangle in the
// wide view. Here the circle is fitted to the table's unclamped edges (least squares:
// x^2 + y^2 = 2 cx x + 2 cy y + c) and continued over the view's margins
// (gbarecomp g_ws_window_margin_span); the native screen keeps the game's own lines. A
// table entry is the line two after it (read against the latched lines). With too few
// edges to fit (the circle still past every side) the margins are lit whole.
// KOL_NO_CIRCLE_WINDOW=1: as before.
// The fitted circle of the last frame that had one, for the picture's own effect on it
// (kol::spell_ring): native screen pixels, and a count of the frames fitted.
float g_spell_ring[3] = {0.0f, 0.0f, 0.0f};
unsigned g_spell_ring_serial = 0;

int kol_circle_window_span(int win, int y, int* x1, int* x2) {
    static const bool off = std::getenv("KOL_NO_CIRCLE_WINDOW") != nullptr;
    if (off || win != 0) return 0;
    static int last_y = 1 << 30;
    static int state = 0;   // 0 none, 1 a circle, 2 lit whole
    static double cx = 0.0, cy = 0.0, r2 = 0.0;
    if (y <= last_y) {   // a new frame's first row
        state = 0;
        gba::GbaBus* bus = gbarecomp::active_bus();
        const std::uint32_t src = bus ? bus->read32(0x040000B0u) : 0u;
        if (bus && bus->read32(0x040000B4u) == 0x04000040u && (bus->read16(0x040000BAu) & 0xB000u) == 0xA000u &&
            src >= 0x02000000u && src < 0x0203FE00u) {
            double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxz = 0, syz = 0, sz = 0;
            int n = 0, open = 0, shut = 0;
            for (int i = 0; i < 159; ++i) {
                const unsigned v = bus->read16(src + 2u * static_cast<unsigned>(i));
                const int a = static_cast<int>(v >> 8), b = static_cast<int>(v & 0xFFu), row = i - 2;
                // (A line the circle does not reach is parked past the screen, 245..250,
                // not emptied: taken for an edge at 245 it bent the fit, and its own
                // five columns showed in the right margin as a piece of a circle.)
                if (a >= b || a >= 240) { ++shut; continue; }
                if (a == 0 && b >= 240) ++open;
                for (const int px : {a > 0 ? a : -1, b < 250 ? b : -1}) {
                    if (px < 0) continue;
                    const double X = px, Y = row, Z = X * X + Y * Y;
                    sxx += X * X; sxy += X * Y; syy += Y * Y; sx += X; sy += Y;
                    sxz += X * Z; syz += Y * Z; sz += Z;
                    ++n;
                }
            }
            if (n >= 8) {
                // Normal equations of Z = A X + B Y + C, by Cramer's rule.
                const double m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, static_cast<double>(n)}};
                const double v3[3] = {sxz, syz, sz};
                auto det = [](const double q[3][3]) {
                    return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
                           q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
                };
                const double d = det(m);
                if (std::fabs(d) > 1e-6) {
                    double sol[3];
                    for (int c = 0; c < 3; ++c) {
                        double t[3][3];
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) t[i][j] = j == c ? v3[i] : m[i][j];
                        sol[c] = det(t) / d;
                    }
                    cx = 0.5 * sol[0];
                    cy = 0.5 * sol[1];
                    r2 = sol[2] + cx * cx + cy * cy;
                    if (r2 > 16.0 && r2 < 4.0e6) {
                        state = 1;
                        g_spell_ring[0] = static_cast<float>(cx);
                        g_spell_ring[1] = static_cast<float>(cy);
                        g_spell_ring[2] = static_cast<float>(std::sqrt(r2));
                        ++g_spell_ring_serial;
                    }
                }
            } else if (open > 80 && shut == 0) {
                state = 2;
            }
        }
    }
    last_y = y;
    if (state == 0) return 0;
    if (state == 2) { *x1 = -4096; *x2 = 4096; return 1; }
    // (2: the native screen's rows too. The game's first lines are a step off its own
    // circle, the top edge of its screen and now the middle of the view: "the small tear
    // on the top of the circle". KOL_CIRCLE_WINDOW_MARGINS=1: the margins alone.)
    static const int whole = std::getenv("KOL_CIRCLE_WINDOW_MARGINS") ? 1 : 2;
    const double dy = y - cy, h2 = r2 - dy * dy;
    if (h2 <= 0.0) { *x1 = *x2 = 0; return whole; }
    const double hw = std::sqrt(h2);
    *x1 = static_cast<int>(std::lround(cx - hw));
    *x2 = static_cast<int>(std::lround(cx + hw));
    return whole;
}

}  // namespace

extern "C" int kol_bg_x_row_provider(int bg, int screen_y, int x0, int count,
                                     signed char* out_action, int* out_hx) {
    {
        int hx = 0, sy = 0;
        if (kol::title_veil(bg, screen_y, &hx, &sy)) {   // as kol_bg_x_provider
            for (int i = 0; i < count; ++i) { out_action[i] = 1; out_hx[i] = hx; }
            return 1;
        }
    }
    {
        int dx = 0;
        if (kol::opening_wobble(bg, screen_y, &dx)) {   // as kol_bg_x_provider
            for (int i = 0; i < count; ++i) {
                out_action[i] = 1;
                out_hx[i] = x0 + i - static_cast<int>(g_native_left) + dx;
            }
            return 1;
        }
    }
    if (bg == 2 && g_bg2_circle) {   // as kol_bg_x_provider (bg2_circle_update)
        for (int i = 0; i < count; ++i) {
            const int x = x0 + i;
            const bool margin = x < static_cast<int>(g_native_left) || x >= static_cast<int>(g_native_right);
            out_action[i] = static_cast<signed char>(margin ? -1 : 0);
            out_hx[i] = 0;
        }
        return 1;
    }
    const bool ui = (ui_layers() & (1u << bg)) != 0;
    if ((!ui || g_hud_anchor) && count <= kMaxRow && !(bg == kBoxBg && box_anim_row(screen_y))) {
        if (ui) anchored_x_row(bg, screen_y, x0, count, out_action, out_hx);
        else world_x_row(bg, screen_y, x0, count, out_action, out_hx);
        // KOL_BGX_ROW_CHECK=1: compare every row against the per-pixel
        // function it replaces, and abort on the first disagreement.
        static const bool check = env_on("KOL_BGX_ROW_CHECK");
        if (check) {
            static unsigned long long rows = 0;
            for (int i = 0; i < count; ++i) {
                int hw = 0;
                const int a = kol_bg_x_provider(bg, x0 + i, screen_y, &hw);
                const int want = a < 0 ? -1 : (a > 0 ? 1 : 0);
                if (want != out_action[i] || (want > 0 && hw != out_hx[i])) {
                    std::fprintf(stderr, "bgx_row MISMATCH bg=%d y=%d x=%d: "
                                 "row=(%d,%d) pixel=(%d,%d) after %llu rows\n",
                                 bg, screen_y, x0 + i, out_action[i],
                                 out_hx[i], want, hw, rows);
                    std::abort();
                }
            }
            if (++rows % 100000 == 0)
                std::fprintf(stderr, "bgx_row: %llu rows agree\n", rows);
        }
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        int hw = 0;
        const int action = kol_bg_x_provider(bg, x0 + i, screen_y, &hw);
        out_action[i] = static_cast<signed char>(action < 0 ? -1
                                                 : (action > 0 ? 1 : 0));
        out_hx[i] = hw;
    }
    return 1;
}

// Is this the tactical battle map?
//
// Verified against states/B_rictor_highlighted_map.state
// (dispcnt=0x1F40 bg=4C02/4E01/0B08/0A08) and separated from every other
// scene profile observed in a 6000-frame cold boot (intro, logos, title,
// story text) plus both status-screen states. Structural rather than an
// exact register match, but every clause is load-bearing against a scene
// actually observed — see README.md "16:9 extended view".
// Does any live sprite draw into the OBJ window (OBJ mode 2)? A sprite with
// the affine bit clear and bit 9 set is disabled. KOL_WS_OBJWIN_STRICT=1
// answers yes regardless, the rule before 2026-09-25.
bool obj_window_in_use() {
    if (env_on("KOL_WS_OBJWIN_STRICT")) return true;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return true;
    const std::uint8_t* oam = bus->oam_ptr();
    for (int i = 0; i < 128; ++i) {
        const unsigned a0 = oam[i * 8] | (oam[i * 8 + 1] << 8);
        const bool affine = (a0 & 0x100u) != 0u;
        if (!affine && (a0 & 0x200u)) continue;       // disabled
        if (((a0 >> 10) & 3u) == 2u) return true;     // OBJ window
    }
    return false;
}

// BG2 as a piece of the world, over BG0 (map 84's ice cliff, states/thor/0183
// with A pressed through the talk, frames 349-684 after the load): the scene
// writes the stairs into BG0's map, lays the cliff as it was over them on BG2
// (BG2CNT 0x9209: 256x512, char block 2, screen block 18; scrolled so that
// its pixel 0,0 is the composed map's column 16, row 27; BG3 off, dispcnt
// 0x1740), whitens that piece's palette and cross-fades it away (BLDCNT
// 0x3344, BLDALPHA 16/0 to 0/16). The 512-high BG2 failed the HUD clause
// below, and the view went back to 240x160 and flat for 336 frames.
// The rule: coming from the battle map, on a listed map, mode 0 with exactly
// BG0|BG1|BG2 on, no window, BG2 256x512 out of char block 2. Then BG2 is
// not interface: it is composed into the perspective's scene with its blend,
// keeps its own rows in the top and bottom margins, and is left out of the
// side margins as before (its map is 256 wide: the piece again at +256).
// KOL_NO_WORLD_BG2=1 old.
constexpr int kWorldBg2Maps[] = {84};
// The sea on BG2 (map 117, the boat under sail in ending scene 2: BG0 the boat,
// BG2 the water scrolled ~0.67 px right and 0.33 up a frame, BG3 the text box;
// states/thor/0223). A 256x256 field of water every cell of which is drawn: it
// repeats into the margins (the PPU's own wrap) and stands in the scene as the
// sea (kol_focus, kSeaBg2Maps). Any empty cell (a text layer) and it is the
// interface again. The user, 2026-11-11, asked for the moving water to fill the
// screen and move with the camera like the boat. KOL_NO_SEA_BG2=1 old.
constexpr int kSeaBg2Maps[] = {117};
bool sea_bg2_frame(std::uint16_t dispcnt, const std::uint16_t bgcnt[4]) {
    static const bool off = env_on("KOL_NO_SEA_BG2");
    if (off || (dispcnt & 7u) != 0u || !(dispcnt & 0x0400u)) return false;
    if (((bgcnt[2] >> 14) & 3u) != 0u) return false;   // 256x256
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || esz < kEwMap + 2u) return false;
    const int map = ew_map(ew);
    bool listed = false;
    for (int m : kSeaBg2Maps) listed = listed || m == map;
    if (!listed) return false;
    for (int ty = 0; ty < 32; ++ty)
        for (int tx = 0; tx < 32; ++tx)
            if ((bg_tile_entry(bgcnt[2], tx, ty) & 0x3FFu) == 0u) return false;
    return true;
}
bool world_bg2_frame(std::uint16_t dispcnt, const std::uint16_t bgcnt[4], bool was_battle) {
    static const bool off = env_on("KOL_NO_WORLD_BG2");
    if (!off && was_battle && sea_bg2_frame(dispcnt, bgcnt)) return true;
    if (off || !was_battle) return false;
    if ((dispcnt & 0xEF07u) != 0x0700u) return false;
    if (((bgcnt[2] >> 14) & 3u) != 2u || ((bgcnt[2] >> 2) & 3u) != 2u) return false;
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || esz < kEwMap + 2u) return false;
    const int map = ew_map(ew);
    for (int m : kWorldBg2Maps)
        if (m == map) return true;
    return false;
}

// `was_battle`: the previous frame was the battle map. `winout`: WINOUT,
// which says what the OBJ window shows against the outside.
bool is_battle_map(std::uint16_t dispcnt, const std::uint16_t bgcnt[4],
                   std::uint16_t winout, bool was_battle) {
    // Tiled mode 0; or, coming from the battle map, mode 1: a spell's effect
    // on an affine BG2 over the same terrain (Ice Javelin in
    // states/GD_slot3_spell_cast.state, dispcnt 0x1341/0x1741, frames
    // 338-440 after Cast), which put the view back to 240x160 and flat.
    const unsigned mode = dispcnt & 7u;
    // Nothing on at all (no BG, no OBJ): the screen is the backdrop alone
    // (map 58's storm in the opening, states/thor/0006 ~frame 24855: dispcnt
    // 0x0040 with BLDY 16 for 36 frames, the picture black). Read as no
    // battle map, it pillarboxed to 240 wide with grey sides (the user
    // saw it leave the wide screen); the verdict before it stands.
    if ((dispcnt & 0x1F00u) == 0u) return was_battle;
    if (mode != 0u && !(was_battle && mode == 1u)) return false;
    // All four BGs on; or, coming from the battle map, the terrain (BG0|BG1)
    // on with a HUD layer off: the BATTLE START banner's first frame turns
    // BG2 off (dispcnt 0x1B40), and dropping out for it put the view back to
    // 240x160 and flat (states/GB_slot2_battle_start_camera.state).
    // Or BG1 alone off, the rest on (dispcnt 0x1D40): map 57's platform in the
    // void is all on BG0, and its scene after the battle turns the unused BG1
    // off (states/thor/0204, Shaher's last words: every BGxCNT as the battle
    // map's), which showed 240x160 and flat (the user, 2026-11-10, saw it at
    // the Original resolution and settings). Not only coming from the battle map: a state
    // loaded there starts on it; the registers below still have to be the
    // battle map's, BG1's too. KOL_WS_NO_BG1_OFF=1 old.
    static const bool no_bg1_off = env_on("KOL_WS_NO_BG1_OFF");
    const bool bg1_off = !no_bg1_off && (dispcnt & 0x0F00u) == 0x0D00u;
    // And from there, coming from the battle map, BG0 alone of the terrain with
    // a HUD layer off or both (states/thor/0208, ~40 frames in: dispcnt 0x1140,
    // then 0x1540 and 0x1940, an effect on BG2 then BG3 blended over the
    // picture, BLDCNT 0x334C, for ~110 frames, then 0x1D40 again), as the
    // banner's rule below for BG0|BG1.
    const bool bg0_only = !no_bg1_off && was_battle && (dispcnt & 0x0300u) == 0x0100u;
    if ((dispcnt & 0x0F00u) != 0x0F00u && !bg1_off && !bg0_only &&
        !(was_battle && (dispcnt & 0x0300u) == 0x0300u))
        return false;
    // WIN0/WIN1 do NOT disqualify it. Battle PREPARATION is this same screen
    // with WIN1 clipping the NAVI dialogue (dispcnt 0x5F40 against 0x1F40,
    // every BGxCNT identical), and rejecting it pillarboxed the whole
    // deployment phase. The rest of the signature below is what actually
    // identifies the scene, and the status screen stays separated by its char
    // base. OBJWIN still disqualifies: it is a per-pixel stencil built in
    // 240-space, with no defined continuation into the margins -- but only
    // when a sprite actually draws into it. Returning from ORGANIZE in battle
    // preparation leaves the enable bit set (dispcnt 0xDF40 against 0x1F40)
    // with no OBJ-window sprite at all, and the whole deployment phase then
    // stayed pillarboxed at 240x160 (states/BI_organize_return.state). An
    // empty OBJ window selects nothing: every pixel is outside every window,
    // which is how margin columns are evaluated anyway.
    // Nor when the OBJ window only gates the HUD layers: the BATTLE START
    // banner (dispcnt 0x9F40, WINOUT 0x3F3B) shows BG2, which holds the
    // words, only inside its sprites, as a wipe; BG0, BG1 and the sprites
    // draw the same inside it and out, so the terrain is the battle map's
    // and the margins (outside every window) are what they always are.
    // Before this the whole banner was 240x160 and flat (state GB).
    const bool objwin_hud_only =
        (((winout >> 8) ^ winout) & 0x13u) == 0u;  // BG0, BG1, OBJ alike
    if ((dispcnt & 0x8000u) != 0u && !objwin_hud_only && obj_window_in_use())
        return false;
    // KOL_WS_NO_WINDOWED_MAP=1 restores the original rule, where any window
    // disqualified the scene — which pillarboxes battle preparation again.
    if (env_on("KOL_WS_NO_WINDOWED_MAP") && (dispcnt & 0xE000u) != 0u)
        return false;

    auto size_code = [](std::uint16_t c) { return (c >> 14) & 3u; };
    auto char_base = [](std::uint16_t c) { return (c >> 2) & 3u; };

    // Terrain rides BG0/BG1 on 512x256 maps out of char block 0; the status
    // screen reuses the same screen blocks but draws from char block 2.
    if (size_code(bgcnt[0]) != 1u || size_code(bgcnt[1]) != 1u) return false;
    if (char_base(bgcnt[0]) != 0u || char_base(bgcnt[1]) != 0u) return false;
    // HUD/overlay rides BG2/BG3 on 256x256 maps — the layers that must not
    // wrap into the margins. Coming from the battle map, only a layer that
    // is drawn counts: after a spell's effect the game leaves BG2 set up as
    // it was (0x4A08, 512 wide) but off while the camera pans back (GD:
    // 440-683, dispcnt 0x1340); and in mode 1 BG2 is the affine effect, BG3
    // not drawn at all.
    const bool world_bg2 = world_bg2_frame(dispcnt, bgcnt, was_battle);
    for (int b = 2; b < 4; ++b) {
        if (b == 2 && world_bg2) continue;   // a piece of the world, 256x512
        const bool drawn = (dispcnt & (0x100u << b)) != 0u && mode == 0u;
        if ((drawn || !was_battle) && size_code(bgcnt[b]) != 0u) return false;
    }
    return true;
}

// The unit status screen. Same mode-0 four-layer shape as the battle map and
// the same screen blocks, but it draws BG0/BG1 from char block 2 where the
// battle map uses block 0 — the clause that already separates the two in
// is_battle_map(). Verified on states A, C and J.
bool is_status_screen(std::uint16_t dispcnt, const std::uint16_t bgcnt[4]) {
    if ((dispcnt & 7u) != 0u) return false;
    if ((dispcnt & 0x0F00u) != 0x0F00u) return false;
    if ((dispcnt & 0xE000u) != 0u) return false;
    auto size_code = [](std::uint16_t c) { return (c >> 14) & 3u; };
    auto char_base = [](std::uint16_t c) { return (c >> 2) & 3u; };
    if (size_code(bgcnt[0]) != 1u || size_code(bgcnt[1]) != 1u) return false;
    return char_base(bgcnt[0]) == 2u && char_base(bgcnt[1]) == 2u;
}

bool g_status_screen = false;
// The same two, as seen INSIDE a speculative excursion. Kept apart so a
// discarded timeline cannot be mistaken for the real one.
bool g_exc_battle_scene = false;
bool g_exc_status_screen = false;

void kol_extended_view_frame(const gbarecomp::ExtendedViewFrameInfo* f) {
    if (f) kol::gulls_set_view_margin(static_cast<int>(f->extra_right));
    if (!gbarecomp::runtime_excursion_active()) kol::gulls_frame();
    if (!gbarecomp::runtime_excursion_active()) kol::rumble_frame();
    // RetroAchievements: every guest frame, never inside a speculative
    // excursion (its frames are rewound; kol_cheevos.h).
    if (f && !gbarecomp::runtime_excursion_active()) {
#if !defined(__ANDROID__)
        kol::cheevos_desktop_init();
        kol::cheevos_popup_test();
        if (kol::cheevos_take_login_form_request())
            std::fprintf(stderr, "[RetroAchievements] log in with KOL_RA_USER=... KOL_RA_PASSWORD=... "
                                 "(once; the token is kept)\n");
#endif
        kol::cheevos_frame(f->frame_count);
        kol::cheevos_mod_notice_frame(f->frame_count);
        kol::crash_notice_frame(f->frame_count);
    }
    // L2 is Rewind (main.cpp, main_android.cpp) once Cheats > Rewind is on
    // (off by default), except while a debug tool holds it: the tile editor
    // (Tile Info + L2) or L2 Moves Sun. Blocked, gbarecomp keeps no history
    // and hides the on-screen button.
    gbarecomp::g_runtime_rewind_blocked =
        !kol::setting_on(kol::Setting::Rewind) || kol::setting_on(kol::Setting::DebugTileInfo) ||
        kol::setting_on(kol::Setting::DebugSunStick) ? 1 : 0;
    // Before anything below can change what the OBJ attribute providers
    // answer from (their per-frame memo; kol_obj_cull.h).
    kol::obj_attr_new_frame();
    if (!f || !f->io || f->io_size < 0x60) return;
    if (!gbarecomp::runtime_excursion_active()) kol::rumble_scene_frame(f->io);
    if (!gbarecomp::runtime_excursion_active())
        kol::fast_load_present();

    // run_game() clears every game-owned enhancement hook on entry so a second
    // run in the same process cannot inherit a stale one. Installing these
    // from main() therefore does nothing; install from the frame hook, which
    // the runtime calls only after that reset.
    g_ws_bg_x_provider = kol_bg_x_provider;
    g_ws_bg_x_row_provider = kol_bg_x_row_provider;
    g_ws_bg_x_provider_layers = 0xFu;
    g_ws_tilemap_provider = kol_tilemap_provider;
    g_ws_tilemap_provider_x_phase = kol_tilemap_x_phase;
    g_ws_tilemap_fill_provider = kol_tilemap_fill_provider;
    g_ws_tilemap_override_provider = kol_tilemap_override_provider;
    g_ws_bg_y_provider = kol_bg_y_provider;
    g_ws_obj_attr_x_provider = kol_obj_attr_x;
    g_ws_bg_y_provider_layers = ui_layers();
    // The present path reads the guest through a capture taken at present,
    // so gbarecomp may run it a frame behind on its worker (kol_focus.cpp).
    gbarecomp::g_present_capture_hook = kol::present_capture;
    gbarecomp::g_runtime_pause_query = kol::menu_pause_query;
    // The top screen's frame while the menu has the focus: a host with no
    // overlays of its own (Android) takes the frame's alone.
    if (!gbarecomp::g_present_overlay_provider)
        gbarecomp::g_present_overlay_provider = kol::menu_focus_frame_provider;
    // See-through movement-range highlight. Installs its own renderer seam and
    // maintains the shadow it needs; see kol_highlight.cpp.
    kol::highlight_tick();
    // Live pointers, not copies: the PPU composes scanlines as the guest runs,
    // so the policy reads the same registers/VRAM the renderer is sampling.
    if (!g_vram) g_vram = gbarecomp::runtime_vram_view(nullptr);
    g_io = f->io;
    {
        gba::GbaBus* b = gbarecomp::active_bus();
        bg2_circle_update(b ? b->ewram_ptr() : nullptr);
    }
    if (gba::GbaBus* bus = gbarecomp::active_bus()) {
        kol::install_terrain_probe(bus->rom_ptr(),
                              static_cast<std::uint32_t>(bus->rom_size()));
        // The second screen reads a unit straight out of the record and the
        // ROM tables it indexes, so it needs the cartridge image too.
        kol::unit_data_set_rom(bus->rom_ptr(),
                               static_cast<std::uint32_t>(bus->rom_size()));
        kol::obj_cull_install();
        kol::actions_install();
        kol::fast_load_install();
        kol::ghost_install();
        kol::shop_buy_install();
        kol::song_tie_install();
        kol::gulls_install();
        kol::stick_cursor_install();
        kol::rumble_install();
        kol::auto_win_install();
        kol::menu_install();
        kol::tile_edit_install();   // after the menu's key filter, which it wraps
        kol::perspective_strike_install();
        kol::fast_actions_install();
        kol::scene_strip_install();
        kol::states_install();
        kol::ai_army_install();
        kol::vs_bot_install();   // after every other key filter: it overrides them
        kol::vs_cpu_install();
    }
    kol::terrain_probe_tick(f->frame_count);
    kol::debug_scene_camera_tick();
    kol::tile_edit_test_tick(f->frame_count);
    kol::states_test_tick();
    kol::ai_army_tick(f->frame_count);
    kol::vs_bot_tick(f->frame_count);
    kol::vs_cpu_tick();
    kol::edit_panel_test_tick(f->frame_count);
    // KOL_SAVE_STATE_AT=<frame>[,<slot>] (headless runs): one save state at
    // that absolute frame, to the ROM's .state<slot> (default 9), the way
    // the host's slot keys save. The TCP rig cannot take these: KoL's hooks
    // are installed from this frame hook, which does not run under --tcp.
    {
        static const long at = [] {
            const char* v = std::getenv("KOL_SAVE_STATE_AT");
            return v && *v ? std::atol(v) : -1L;
        }();
        static bool done = false;
        if (at >= 0 && !done && static_cast<long>(f->frame_count) >= at) {
            const char* v = std::getenv("KOL_SAVE_STATE_AT");
            const char* comma = std::strchr(v, ',');
            gbarecomp::runtime_request_state_save(comma ? std::atoi(comma + 1) : 9);
            done = true;
        }
    }
    // KOL_LOAD_STATE_AT=<frame>[,<slot>]: the other way, one load from the
    // ROM's .state<slot> (default 9) at that absolute frame, so one process
    // plays one scene and then another (host caches carried across, as on
    // the device when the player goes from one battle to the next).
    {
        static const long at = [] {
            const char* v = std::getenv("KOL_LOAD_STATE_AT");
            return v && *v ? std::atol(v) : -1L;
        }();
        static bool done = false;
        if (at >= 0 && !done && static_cast<long>(f->frame_count) >= at) {
            const char* v = std::getenv("KOL_LOAD_STATE_AT");
            const char* comma = std::strchr(v, ',');
            gbarecomp::runtime_request_state_load(comma ? std::atoi(comma + 1) : 9);
            done = true;
        }
    }
    kol::unit_dump_tick();
    kol::unit_list_dump_tick(f->frame_count);
    kol::scene_strip_frame();
    // The title's veil continues past line 160 through the Y provider, which
    // otherwise only sees the UI layers: BG1 is added while the title is up.
    g_ws_bg_y_provider_layers = ui_layers() | (kol::opening_scene() == 2 ? 0x2u : 0u);
    kol::scene_strip_dump_tick(f->frame_count);
    kol::auto_win_tick();
    // World units that stand past the old 240 edge should be visible, so OBJ
    // stays unclipped. Only 38 pixels of one unit differed in the verified
    // battle state; no parked-sprite junk appeared. KOL_WS_OBJ_CLIP=1 forces
    // the conservative native clip if a later scene needs it.
    g_ws_obj_native_clip = env_on("KOL_WS_OBJ_CLIP") ? 1 : 0;

    g_native_left = f->extra_left;
    g_native_right = f->extra_left + 240u;
    g_extra_left = static_cast<int>(f->extra_left);
    g_extra_right = static_cast<int>(f->extra_right);
    g_extra_top = static_cast<int>(f->extra_top);
    g_extra_bottom = static_cast<int>(f->extra_bottom);

    ++g_box_frame;   // the dialog box animation's frame (box_anim_update)
    const std::uint16_t dispcnt = io16(f->io, 0x00);
    const std::uint16_t bgcnt[4] = {io16(f->io, 0x08), io16(f->io, 0x0A),
                                    io16(f->io, 0x0C), io16(f->io, 0x0E)};

    // KOL_WS_PROBE=1 logs one line per scene-profile change; this is how the
    // signatures above were collected and how new ones get classified.
    if (env_on("KOL_WS_PROBE")) {
        static std::uint64_t sig_prev = ~0ull;
        const std::uint64_t sig =
            (static_cast<std::uint64_t>(dispcnt) << 48) ^
            (static_cast<std::uint64_t>(bgcnt[0]) << 36) ^
            (static_cast<std::uint64_t>(bgcnt[1]) << 24) ^
            (static_cast<std::uint64_t>(bgcnt[2]) << 12) ^
            static_cast<std::uint64_t>(bgcnt[3]) ^
            (static_cast<std::uint64_t>(io16(f->io, 0x10)) << 20) ^
            (static_cast<std::uint64_t>(io16(f->io, 0x48)) << 4) ^
            (static_cast<std::uint64_t>(io16(f->io, 0x4A)) << 40);
        if (sig != sig_prev) {
            sig_prev = sig;
            std::fprintf(stderr,
                "[kol-ws] f=%llu mode=%u dispcnt=%04X bg=%04X/%04X/%04X/%04X "
                "scroll=%u,%u/%u,%u ui=%u,%u/%u,%u "
                "bldcnt=%04X bldalpha=%04X bldy=%04X winin=%04X winout=%04X battle=%d\n",
                static_cast<unsigned long long>(f->frame_count),
                static_cast<unsigned>(dispcnt & 7u), dispcnt,
                bgcnt[0], bgcnt[1], bgcnt[2], bgcnt[3],
                io16(f->io, 0x10) & 0x1FFu, io16(f->io, 0x12) & 0x1FFu,
                io16(f->io, 0x14) & 0x1FFu, io16(f->io, 0x16) & 0x1FFu,
                io16(f->io, 0x18) & 0x1FFu, io16(f->io, 0x1A) & 0x1FFu,
                io16(f->io, 0x1C) & 0x1FFu, io16(f->io, 0x1E) & 0x1FFu,
                io16(f->io, 0x50), io16(f->io, 0x52), io16(f->io, 0x54),
                io16(f->io, 0x48), io16(f->io, 0x4A),
                is_battle_map(dispcnt, bgcnt, io16(f->io, 0x4A), g_battle_scene) ? 1 : 0);
            std::fflush(stderr);
        }
    }

    // KOL_WS_FORCE_EXPOSE=1 ignores the policy and shows every layer in the
    // margins. Diagnostic only — this is what reveals whether a new scene's
    // margins hold real scenery or wrapped garbage.
    if (env_on("KOL_WS_FORCE_EXPOSE")) {
        g_hud_anchor = 0;
        g_panel_pin = 0;
        g_panel_found = false;
        g_ui_shift_valid = false;
        // KOL_WS_MARGIN_LAYERS=<hex> narrows the exposed set, which is how you
        // find out which layer a given UI element lives on: expose one layer
        // at a time and see which one wraps the element into the margin.
        const char* m = std::getenv("KOL_WS_MARGIN_LAYERS");
        g_margin_layers = m ? static_cast<unsigned>(std::strtoul(m, nullptr, 0))
                            : 0xFu;
        g_ws_pillarbox = 0;
        g_ws_pillarbox_left = 0;
        g_ws_pillarbox_right = 0;
        return;
    }

    // Speculative frames must NOT pollute the real classifier. A shadow
    // timeline parked on a status screen would otherwise make is_battle_scene()
    // false on every frame it is stepped, and the second screen — which reads
    // this from another thread — would conclude the player had left the battle
    // map and clear itself. Measured: 229 clears against 171 draws, i.e. the
    // panel visibly flashing at the step rate.
    {
        const bool exc = gbarecomp::runtime_excursion_active();
        const bool bm = is_battle_map(dispcnt, bgcnt, io16(f->io, 0x4A),
                                      exc ? g_exc_battle_scene : g_battle_scene);
        const bool ss = is_status_screen(dispcnt, bgcnt);
        if (exc) {
            g_exc_battle_scene = bm;
            g_exc_status_screen = ss;
        } else {
            g_world_bg2 = bm && world_bg2_frame(dispcnt, bgcnt, g_battle_scene);
            g_sea_bg2 = g_world_bg2 && sea_bg2_frame(dispcnt, bgcnt);
            g_battle_scene = bm;
            g_status_screen = ss;
            g_ws_bg_y_provider_layers = ui_layers() | (kol::opening_scene() == 2 ? 0x2u : 0u);
        }
    }
    // A speculative frame must not touch PRESENT-TIME policy state. The scene
    // classifier is already split, but everything below is host state the REAL
    // frame renders with — in particular rebuild_ui_anchors(), which would
    // compute HUD anchor positions from the shadow's status screen and then
    // apply them to the next battle-map frame. That is the flash on the main
    // screen when the cursor reaches a new character. Margins and anchors keep
    // the real timeline's values; the shadow renders with them, which is
    // harmless because only its centre 240 columns are ever read.
    if (gbarecomp::runtime_excursion_active()) return;

    // Where the sound effects come from (Options; kol_sound.h).
    kol::sound_frame(f);
    kol::menu_debug_frame();
    kol::fast_load_frame();
    {
        static bool read = false;
        if (!read) {
            read = true;
            if (const char* e = std::getenv("KOL_RW_FN")) {
                char buf[512]; unsigned pc = 0;
                if (std::sscanf(e, "%x,%511s", &pc, buf) == 2) { g_rw_fn = pc; g_rw_path = buf; entry_log_chain(); }
            }
        }
    }
    // KOL_TEST_WORLD_ITEM=<item>,<frame>: queue a world-map bar action (0-2
    // Organize's screens, 3 Training, 4 Shop) at the first present at or
    // past the frame, for a headless test of what follows.
    {
        static long at = -1; static int item = -1; static bool done = false, read = false;
        if (!read) {
            read = true;
            if (const char* e = std::getenv("KOL_TEST_WORLD_ITEM")) std::sscanf(e, "%d,%ld", &item, &at);
        }
        if (!done && item >= 0 && static_cast<long>(f->frame_count) >= at) { done = true; kol::action_world_item(item); }
    }
    // KOL_MEM_DUMP=<frame>,<path>: at the first present at or past the frame,
    // EWRAM, IWRAM, palette, VRAM and OAM, raw, one after the other (for
    // comparing two runs' state without comparing their pictures).
    {
        static long at = -1;
        static std::string path;
        static bool done = false, read = false;
        if (!read) {
            read = true;
            if (const char* e = std::getenv("KOL_MEM_DUMP")) {
                char buf[512];
                if (std::sscanf(e, "%ld,%511s", &at, buf) == 2) path = buf;
            }
        }
        if (at >= 0 && !done && static_cast<long>(f->frame_count) >= at) {
            done = true;
            gba::GbaBus* bus = gbarecomp::active_bus();
            if (bus)
                if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(bus->ewram_ptr(), 1, 0x40000, fp);
                    std::fwrite(bus->iwram_ptr(), 1, 0x8000, fp);
                    std::fwrite(bus->pal_ptr(), 1, 0x400, fp);
                    std::fwrite(bus->vram_ptr(), 1, 0x18000, fp);
                    std::fwrite(bus->oam_ptr(), 1, 0x400, fp);
                    std::fclose(fp);
                    std::fprintf(stderr, "mem_dump: frame %lu -> %s\n", static_cast<unsigned long>(f->frame_count), path.c_str());
                }
        }
    }
    // KOL_FADE_LOG=1: every 10 frames (KOL_FADE_LOG_EVERY=n: every n), the map, and the BG palette's
    // brightness (5-bit channel sums) in rows 0-3 and 4-8, live (PAL RAM)
    // and in the scenes' full palette at 0x0202D90C (scene_bright), and the
    // blend registers.
    {
        static const int fade_log = [] {
            const char* every = std::getenv("KOL_FADE_LOG_EVERY");
            return std::getenv("KOL_FADE_LOG") ? (every && std::atoi(every) > 0 ? std::atoi(every) : 10) : 0;
        }();
        if (fade_log && f->frame_count % fade_log == 0)
            if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                const std::uint8_t* pal = bus->pal_ptr();
                const std::uint8_t* ew = bus->ewram_ptr();
                auto sum = [](const std::uint8_t* p, int r0, int r1) {
                    unsigned s = 0;
                    for (int i = r0 * 16; i < r1 * 16; ++i) {
                        const unsigned c = p[i * 2] | (p[i * 2 + 1] << 8);
                        s += (c & 31u) + ((c >> 5) & 31u) + ((c >> 10) & 31u);
                    }
                    return s;
                };
                int drops = 0;   // the weather's live drops and block (kol_focus kWeatherBlock)
                for (int i = 0; i < 10; ++i) drops += ew[kEwDrops + i * 0x14 + 4] != 0;
                std::fprintf(stderr, "fade: frame %lu map %u rows0-3 %u/%u rows4-8 %u/%u bldcnt %04x bldy %u bldalpha %04x dispcnt %04x drops %d weather %02x %02x %02x field %d bg9-15 %u obj %u pillarbox %d win0 %04x/%04x winin %04x winout %04x\n",
                             static_cast<unsigned long>(f->frame_count), ew_map(ew),
                             sum(pal, 0, 4), sum(ew + kEwFullPal, 0, 4), sum(pal, 4, 9), sum(ew + kEwFullPal, 4, 9),
                             bus->read16(0x04000050u), bus->read16(0x04000054u) & 31u, bus->read16(0x04000052u),
                             bus->read16(0x04000000u), drops, ew[kEwWeather], ew[kEwWeather + 4], ew[kEwWeather + 6], g_ui_field_drawn,
                             sum(pal, 9, 16), sum(pal, 16, 32), g_ws_pillarbox,
                             bus->read16(0x04000040u), bus->read16(0x04000044u), bus->read16(0x04000048u),
                             bus->read16(0x0400004Au));
                std::fprintf(stderr, "fade: frame %lu backdrop %04x\n", static_cast<unsigned long>(f->frame_count),
                             ew[kEwPalBuffer] | (ew[kEwPalBuffer + 1] << 8));   // the sky (kol_focus g_overcast)
                // The frame's BG layout and scroll (as the frame hook has them).
                std::fprintf(stderr, "fade: frame %lu bgcnt %04x %04x %04x %04x ofs %u,%u %u,%u %u,%u %u,%u battle %d world_bg2 %d\n",
                             static_cast<unsigned long>(f->frame_count), io16(f->io, 0x08), io16(f->io, 0x0A),
                             io16(f->io, 0x0C), io16(f->io, 0x0E), io16(f->io, 0x10) & 511u, io16(f->io, 0x12) & 511u,
                             io16(f->io, 0x14) & 511u, io16(f->io, 0x16) & 511u, io16(f->io, 0x18) & 511u,
                             io16(f->io, 0x1A) & 511u, io16(f->io, 0x1C) & 511u, io16(f->io, 0x1E) & 511u,
                             g_battle_scene ? 1 : 0, g_world_bg2 ? 1 : 0);
            }
    }
    // KOL_CAM_LOG=1: every frame, the scene camera (0x02026C78/7A), its ease
    // target (0x02026C80/82), BG0's scroll and the presents so far (to see
    // how evenly a pan steps).
    {
        static const bool cam_log = std::getenv("KOL_CAM_LOG") != nullptr;
        if (cam_log)
            if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                const std::uint8_t* ew = bus->ewram_ptr();
                auto r16 = [&](std::uint32_t a) { return static_cast<std::int16_t>(ew[a] | (ew[a + 1] << 8)); };
                std::fprintf(stderr, "cam: frame %lu cam %d,%d target %d,%d bg0 %u,%u presented %llu\n",
                             static_cast<unsigned long>(f->frame_count), r16(0x26C78), r16(0x26C7A), r16(0x26C80),
                             r16(0x26C82), bus->read16(0x04000010u) & 511u, bus->read16(0x04000012u) & 511u,
                             static_cast<unsigned long long>(gbarecomp::g_runtime_presented_frame));
            }
    }
    // KOL_OBJ_LOG=<n>: every n frames, the map's objects (u32 at 0x0202A238,
    // 26 x 0xD0): slot, unit record, its +0x4E, world X, Y, Z (to find a
    // scene's actor coming and going).
    {
        static const int obj_log = [] {
            const char* e = std::getenv("KOL_OBJ_LOG");
            return e ? std::max(1, std::atoi(e)) : 0;
        }();
        if (obj_log && f->frame_count % obj_log == 0)
            if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                const std::uint8_t* ew = bus->ewram_ptr();
                auto r32 = [&](std::uint32_t a) { return ew[a] | (ew[a + 1] << 8) | (ew[a + 2] << 16) | (static_cast<std::uint32_t>(ew[a + 3]) << 24); };
                auto r16 = [&](std::uint32_t a) { return ew[a] | (ew[a + 1] << 8); };
                const std::uint32_t list = r32(0x2A238);
                std::fprintf(stderr, "objs: frame %lu", static_cast<unsigned long>(f->frame_count));
                if (list >= 0x02000000u && list + 26u * 0xD0u <= 0x02040000u)
                    for (int i = 0; i < 26; ++i) {
                        const std::uint32_t o = list - 0x02000000u + static_cast<std::uint32_t>(i) * 0xD0u;
                        const std::uint32_t rec = r32(o);
                        if (rec < 0x02000000u || rec >= 0x02040000u) continue;
                        std::fprintf(stderr, " %d:%x/%02x@%d,%d,%d", i, rec & 0xFFFFFu, ew[rec - 0x02000000u + 0x4E],
                                     static_cast<int>(static_cast<std::int16_t>(r16(o + 0x10))),
                                     static_cast<int>(static_cast<std::int16_t>(r16(o + 0x12))),
                                     static_cast<int>(static_cast<std::int16_t>(r16(o + 0x14))));
                    }
                std::fprintf(stderr, "\n");
            }
    }
    // KOL_PC_HIST=<frame_lo>,<frame_hi>,<path> (with GBARECOMP_INSN_TRACE=1):
    // a histogram of the guest's PCs, by cycles, from the first present at or
    // past frame_lo to the first at or past frame_hi (gbarecomp
    // runtime_pc_hist_*). For a stall between two presents.
    {
        static long lo = -1, hi = -1;
        static std::string path;
        static int state = 0;
        if (state == 0) {
            state = 1;
            if (const char* e = std::getenv("KOL_PC_HIST")) {
                char buf[512];
                if (std::sscanf(e, "%ld,%ld,%511s", &lo, &hi, buf) == 3) path = buf;
            }
        }
        if (lo >= 0 && state == 1 && static_cast<long>(f->frame_count) >= lo) {
            runtime_pc_hist_arm(1);
            entry_log_arm(true);
            state = 2;
        } else if (state == 2 && static_cast<long>(f->frame_count) >= hi) {
            runtime_pc_hist_dump(path.c_str());
            entry_log_dump((path + ".entries").c_str());
            std::fprintf(stderr, "pc_hist: frames %ld..%lu -> %s\n", lo, static_cast<unsigned long>(f->frame_count), path.c_str());
            state = 3;
        }
    }
    // The options menu holds the game (kol_menu's menu_pause_query); a
    // change of a setting lets a few frames run, counted here. The picture
    // is no longer dimmed under the menu (until 2026-11-23: 50%), so a
    // change shows as it is made.
    kol::menu_frame_tick();

    // Battle map only, and only when the player switched it on in the BACK
    // menu (KOL_EDGE_BLUR=1 forces it on for desktop runs, KOL_NO_EDGE_BLUR=1
    // off): blur by depth around the cursor. Drawn at present, so frames and
    // hashes are unaffected. The HUD is BG2|BG3 on the battle map
    // (the same split the margin policy below relies on), so those layers,
    // and sprites drawn over them, stay sharp; KOL_EDGE_BLUR_HUD=1 blurs them
    // too, for an A/B.
    const bool edge_blur = g_battle_scene && !env_on("KOL_NO_EDGE_BLUR") &&
        (kol::setting_on(kol::Setting::DepthOfField) || env_on("KOL_EDGE_BLUR"));
    // Perspective (BACK menu; KOL_PERSPECTIVE=1 forces it on for desktop
    // runs, KOL_NO_PERSPECTIVE=1 off): the map drawn smaller with distance at
    // present, from the frame composed without the HUD (BG2|BG3). With the
    // depth of field on too, the blurred scene is what it moves.
    const bool perspective = g_battle_scene && !env_on("KOL_NO_PERSPECTIVE") &&
        (kol::setting_on(kol::Setting::Perspective) || env_on("KOL_PERSPECTIVE"));
    // The LAYERED blur's strength (the fallback where the renderer has no GL;
    // the disc blur below uses a radius instead). 0.5: the farthest pixels
    // stop halfway up the blur ladder (between the 1/2 and 1/4 levels), a
    // subtle depth rather than a fog. With the
    // perspective the distance already reads from the size, so the blur is
    // lighter: 0.35, just past the 1/2 level (the user's call, 2026-09-26).
    // KOL_EDGE_BLUR_STRENGTH=<0..1> overrides both, for trying values.
    static const float blur_override = [] {
        const char* v = std::getenv("KOL_EDGE_BLUR_STRENGTH");
        return v && *v ? static_cast<float>(std::atof(v)) : -1.0f;
    }();
    gbarecomp::g_present_edge_blur = !edge_blur ? 0.0f
        : blur_override >= 0.0f ? blur_override
        : perspective ? 0.35f : 0.5f;
    // The blur as lens discs, of the radius the player picked (BACK menu,
    // Options; KOL_DOF_RADIUS=<frame px> overrides, 0 = the layered blur).
    // Where the renderer has no GL (Metal on a Mac) the layered blur above
    // still draws.
    static const float radius_override = [] {
        const char* v = std::getenv("KOL_DOF_RADIUS");
        return v && *v ? static_cast<float>(std::atof(v)) : -1.0f;
    }();
    gbarecomp::g_present_focus_radius = !edge_blur ? 0.0f
        : radius_override >= 0.0f ? radius_override : kol::blur_radius();
    gbarecomp::g_present_sharp_layers =
        edge_blur && !env_on("KOL_EDGE_BLUR_HUD") ? ui_layers() : 0u;
    // The light without the perspective (kol_perspective's flat_provider):
    // the same scene and light, drawn back onto itself pixel for pixel.
    // KOL_NO_FLAT_LIGHT=1 leaves it off.
    const bool flat_light = g_battle_scene && !perspective && kol::lighting_on() &&
                            !env_on("KOL_NO_FLAT_LIGHT");
    g_ppu_scene_exclude_layers = perspective || flat_light ? ui_layers() : 0u;
    // In a spell the warp draws the effect as a card (kol_perspective.cpp),
    // leaving out the pixels where it adds nothing to the scene: gbarecomp
    // keeps the scene as composed for that test.
    gbarecomp::g_present_keep_scene = perspective && kol::perspective_spell_camera() ? 1 : 0;
    // Where a unit drawn from its own sprite leaves its place, the ground the
    // game drew under it (kol_perspective.cpp). KOL_PERSP_MIRROR_FILL=1 fills
    // from beside it instead, as before.
    g_ppu_scene_under_obj = (perspective || flat_light) && !env_on("KOL_PERSP_NO_SPRITES") &&
                            !env_on("KOL_PERSP_MIRROR_FILL") ? 1 : 0;
    // And behind BG1, for props lifted off the ground (experimental).
    g_ppu_scene_under_bg1 = g_ppu_scene_under_obj && perspective && kol::perspective_lift_props() ? 1 : 0;
    // The perspective shrinks the far side inward, so the scene is composed
    // past the view there: the map runs on to the edge instead of stopping
    // in a straight cut. KOL_PERSP_NO_OVERSCAN=1 turns it off, for an A/B.
    kol::perspective_strike_tick(perspective);
    unsigned overscan[4] = {0u, 0u, 0u, 0u};
    if (perspective && !env_on("KOL_PERSP_NO_OVERSCAN"))
        kol::perspective_overscan(
            static_cast<int>(240u + f->extra_left + f->extra_right),
            static_cast<int>(160u + f->extra_top + f->extra_bottom), overscan);
    for (int i = 0; i < 4; ++i) g_ppu_scene_overscan[i] = overscan[i];
    // Only the far rows need the side columns: the rest are left out.
    // KOL_PERSP_NO_TRIM=1 composes all of them (and KOL_PERSP_TRIM_CHECK=1,
    // which checks that nothing left out would have been seen).
    static unsigned char overscan_rows[512];
    const int compose_h = static_cast<int>(160u + f->extra_top + f->extra_bottom +
                                           overscan[2] + overscan[3]);
    g_ppu_scene_overscan_rows =
        (overscan[0] | overscan[1]) != 0 && compose_h <= 512 &&
        !env_on("KOL_PERSP_NO_TRIM") && !env_on("KOL_PERSP_TRIM_CHECK") &&
        kol::perspective_overscan_rows(
            static_cast<int>(240u + f->extra_left + f->extra_right),
            static_cast<int>(160u + f->extra_top + f->extra_bottom), overscan,
            overscan_rows, compose_h)
            ? overscan_rows : nullptr;
    g_ppu_record_layers =
        gbarecomp::g_present_sharp_layers != 0 || perspective || flat_light ? 1 : 0;
    // The sprites the frame was drawn with, for the scene rebuild's unit
    // pixels (kol_focus.cpp, claim_unit_sprites).
    g_ppu_record_obj = g_ppu_record_layers;
    kol::perspective_set_enabled(perspective);
    if (!perspective) kol::perspective_set_flat(flat_light);
    // KOL_NO_FOCUS=1 goes back to the two bands.
    kol::focus_set_enabled(edge_blur && !env_on("KOL_NO_FOCUS"));
    kol::focus_note_scroll(io16(f->io, 0x10), io16(f->io, 0x12));

    if (g_battle_scene) {
        g_margin_layers = 0x3u;  // BG0|BG1 terrain only; BG2|BG3 HUD clipped
        if (g_sea_bg2) g_margin_layers |= 0x4u;   // the sea's field, repeated (sea_bg2_frame)
        // ...except the bottom unit bar, which is continued instead of
        // clipped. KOL_WS_NO_BAR=1 reverts to clipping for an A/B.
        g_extend_bottom_bar = env_on("KOL_WS_NO_BAR") ? 0 : 1;
        g_hud_anchor = env_on("KOL_WS_NO_HUD_ANCHOR") ? 0 : 1;
        g_panel_pin = env_on("KOL_WS_NO_PANEL_PIN") ? 0 : 1;
        // The unit box (name, Lv, HP, MP) repeats what the second screen
        // shows, so with the second screen up it is left out: vacated as
        // pinning vacates it, and not drawn at the pin. KOL_HIDE_UNIT_PANEL=1
        // forces that (desktop tests), KOL_NO_HIDE_UNIT_PANEL=1 keeps the box.
        g_panel_hide = g_panel_pin && !env_on("KOL_NO_HIDE_UNIT_PANEL") &&
            (kol::second_screen_shown() || env_on("KOL_HIDE_UNIT_PANEL")) ? 1 : 0;
        g_side_panel_anchor = env_on("KOL_WS_NO_SIDE_PANEL") ? 0 : 1;
        g_extend_side_panel_v = env_on("KOL_WS_NO_PANEL_EXTEND") ? 0 : 1;
        // The margins are sourced from the guest's composed map and checked
        // against its own drawn cells every frame, so they do not depend on
        // the native scanline's window state. Say so: otherwise the PPU's
        // fail-closed rule blacks out any margin row whose 240 native pixels
        // do not all select the SAME window control, which is every row of a
        // NAVI dialogue — a black band across both margins for the height of
        // the box. With this set, margin columns evaluate as outside the
        // guest's window rectangles and WINOUT governs them, which is what
        // "off the side of the native screen" means.
        g_ws_authored_margin_layers = 1;
        // A letterbox window's edges go out with the bars held at the view's
        // edges (map 58's storm in the opening, states/thor/0006: the bars
        // open with WIN0; README "Map 58's storm in the opening").
        g_ws_letterbox_windows = 1;
        g_ws_window_margin_span = kol_circle_window_span;
        kol::confirm_arrow_tick(true);
        g_forecast_off = kol::confirm_arrow_hidden_now() && kol::confirm_arrow_bank() >= 0;
        // Each only while it is there, by its own tiles: the EXP window and
        // "... emblem acquired." lie over the box's place once the action
        // plays, and were cut (states/thor/0077; the user saw the EXP
        // window cut). The box: its edge tile 0x0C2 at
        // column 0 on rows 1 and 3, nothing at column 10 (a message window
        // starts at column 3). The bar: its "Lv." (BG2 0x0B3 0x0B4, row 18).
        {
            const std::uint16_t c2 = static_cast<std::uint16_t>(g_io[0x0C] | (g_io[0x0D] << 8));
            const std::uint16_t c3 = static_cast<std::uint16_t>(g_io[0x0E] | (g_io[0x0F] << 8));
            const bool on = kol::confirm_arrow_hidden_now() && kol::unit_in_action();
            g_unit_box_off = on && (bg_tile_entry(c3, 0, 1) & 0x3FFu) == 0x0C2u &&
                             (bg_tile_entry(c3, 0, 3) & 0x3FFu) == 0x0C2u && (bg_tile_entry(c3, 10, 1) & 0x3FFu) == 0u;
            g_unit_bar_off = on && (bg_tile_entry(c2, 1, 18) & 0x3FFu) == 0x0B3u &&
                             (bg_tile_entry(c2, 2, 18) & 0x3FFu) == 0x0B4u;
            // A window over part of the box (the item's HELP from column 1,
            // row 2, its "HELP" tab on BG2; states/thor/0102: "the text box
            // is cut") leaves the test above true: its cells, BG3 window
            // paper 0x005-0x008 or frame 0x011-0x018, are its own and stay.
            // KOL_UNIT_BOX_OVER_WINDOWS=1 old (the whole place left out).
            static const bool over_windows = std::getenv("KOL_UNIT_BOX_OVER_WINDOWS") != nullptr;
            for (int ty = 0; ty < 4; ++ty)
                for (int tx = 0; tx < 10; ++tx) {
                    const unsigned t = bg_tile_entry(c3, tx, ty) & 0x3FFu;
                    g_unit_box_window[ty][tx] = g_unit_box_off && !over_windows &&
                                                ((t >= 0x005u && t <= 0x008u) || (t >= 0x011u && t <= 0x018u));
                }
        }
        rebuild_ui_anchors();
        g_ws_pillarbox = 0;
        // Re-point the composed-map lookup and re-verify it against what the
        // guest has drawn. Off a battle map it disarms itself.
        {
            std::uint32_t esz = 0;
            const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
            kol::map_source_tick(ew, g_vram, g_io, true);
            g_ws_authored_margin_layers = kol::map_source_armed() ? 1 : 0;
            g_ws_tilemap_override_layers = kol::map_source_override_layers() & g_margin_layers;
            // The world shadow reads the array that tick just verified.
            kol::highlight_world_tick();
        }
    } else {
        // Fail closed. An unclassified scene gets black margins rather than
        // wrapped tilemap, which is what every non-battle scene tested so far
        // would otherwise show.
        g_margin_layers = 0u;
        g_extend_bottom_bar = 0;
        g_hud_anchor = 0;
        g_panel_pin = 0;
        g_panel_found = false;
        g_ui_shift_valid = false;
        g_ws_authored_margin_layers = 0;
        g_ws_letterbox_windows = 0;
        g_ws_window_margin_span = nullptr;
        g_forecast_off = false;
        g_unit_box_off = g_unit_bar_off = false;
        kol::confirm_arrow_tick(false);
        g_ws_pillarbox = 1;
        kol::map_source_tick(nullptr, nullptr, nullptr, false);
        g_ws_tilemap_override_layers = 0;
    }
    g_ws_pillarbox_left = 0;
    g_ws_pillarbox_right = 0;

    // Tell the guest how much wider its own sprite cull may be. This has to
    // follow the pillarbox decision, not the requested view size: a scene
    // whose margins are blacked out must keep the stock window, or the game
    // spends OAM slots on sprites nobody can see. Zero margins restore the
    // stock immediates exactly, so the faithful render is untouched.
    // VS over a network (kol_vs_net): the guest's cull is the game's own
    // sprite list, and the other machine's copy of this game must build the
    // same one, so it follows only what both copies know alike: the size
    // this scene asks for (g_vs_target_w/h, below: the applied one lags by a
    // host-timed boundary) and the settings, with a fixed overscan for the
    // perspective's far side rather than the camera's (the right stick's
    // look and the combat camera's zoom move it). Matches went out of step
    // at the battle's preparation, where the view widens (2026-11-22).
    if (g_ws_pillarbox) {
        kol::obj_cull_set_margins(0, 0, 0, 0);
    } else if (kol::settings_net_match_on() && g_vs_target_w >= 240u && g_vs_target_h >= 160u &&
               !env_on("KOL_VS_NET_CULL_OLD")) {   // (=1: the applied view and live overscan, as before)
        const unsigned xw = g_vs_target_w - 240u, xh = g_vs_target_h - 160u;
        const unsigned el = xw / 2u, er = xw - el, et = xh / 2u, eb = xh - et;
        const unsigned os = perspective ? g_vs_target_w / 4u : 0u;   // past the perspective's widest at rest
        const unsigned ot = perspective ? g_vs_target_h / 4u : 0u;
        kol::obj_cull_set_margins(el + os, er + os, et + ot, eb);
    } else
        kol::obj_cull_set_margins(f->extra_left + g_ppu_scene_overscan[0],
                                  f->extra_right + g_ppu_scene_overscan[1],
                                  f->extra_top + g_ppu_scene_overscan[2],
                                  f->extra_bottom + g_ppu_scene_overscan[3]);

    // ── The surface follows the scene ───────────────────────────────────
    // A scene with no margin content is drawn at the AUTHENTIC size, because
    // a wider surface only makes it smaller: at 480x270 the world map still
    // occupies 240x160 of the surface, so a 1920x1080 panel shows it at 4x =
    // 960x640, against 6x = 1440x960 when the surface is 240x160 itself.
    // Picking a wider view used to shrink every pillarboxed scene to
    // two-thirds. Measured on the world map, whose four BG layers are all
    // exactly 20 tile rows with nothing beyond — see README.
    //
    // Only when a player-facing size exists: the desktop runner takes its
    // size from the command line and must not be resized behind its back.
    //
    // WHEN to resize matters as much as whether. The classifier is a pure
    // function of DISPCNT and the four BGxCNTs, so it flips the moment the
    // game writes the incoming scene's registers — which is before the scene
    // fades in. Acting then puts the resize inside the transition, where it
    // is invisible. A half-second debounce instead put it half a second AFTER
    // the new scene was already on screen, which is what it felt like.
    //
    // So: hold while the display is FORCED BLANK, because a blanked frame is
    // a scene being torn down and its registers mean nothing, and otherwise
    // act on two consecutive agreeing frames — enough to absorb a single-frame
    // anomaly while staying imperceptible.
    // The player-facing size normally comes from the Android settings menu.
    // KOL_VIEW_SIZE=WxH supplies one headlessly: the desktop runner has
    // --view-width and no --view-height, so without this the VERTICAL margins
    // cannot be exercised or A/B'd anywhere but on the device. Test hook only
    // — it is ignored whenever a real menu choice exists.
    unsigned choice_w = kol::view_choice_width();
    unsigned choice_h = kol::view_choice_height();
    if (choice_w == 0u) {
        if (const char* e = std::getenv("KOL_VIEW_SIZE")) {
            unsigned w = 0, h = 0;
            if (std::sscanf(e, "%ux%u", &w, &h) == 2 && w >= 240u && h >= 160u) {
                choice_w = w;
                choice_h = h;
            }
        }
    }
    if (choice_w != 0u) {
        constexpr int kSettleFrames = 2;
        const bool forced_blank = (dispcnt & 0x0080u) != 0u;
        const unsigned want_w = g_battle_scene ? choice_w : 240u;
        const unsigned want_h = g_battle_scene ? choice_h : 160u;
        static unsigned seen_w = 0, seen_h = 0, asked_w = 0, asked_h = 0;
        static int settled = 0;
        if (forced_blank) {
            settled = 0;                       // decide nothing mid-teardown
        } else {
            if (want_w != seen_w || want_h != seen_h) {
                seen_w = want_w; seen_h = want_h; settled = 0;
            } else if (settled < kSettleFrames) {
                ++settled;
            }
            if (settled >= kSettleFrames) {   // what the scene asks for, whatever is applied (VS cull, above)
                g_vs_target_w = want_w;
                g_vs_target_h = want_h;
            }
            if (settled >= kSettleFrames &&
                (f->view_width != want_w || f->view_height != want_h) &&
                (asked_w != want_w || asked_h != want_h)) {
                asked_w = want_w; asked_h = want_h;
                gbarecomp::runtime_request_view_size(want_w, want_h);
            }
        }
    }
}

}  // namespace

namespace kol {

bool is_battle_scene() { return g_battle_scene; }

bool spell_ring(float out[3]) {
    static unsigned seen = 0;
    if (g_spell_ring_serial == seen) return false;   // no circle fitted since the last present
    seen = g_spell_ring_serial;
    std::copy_n(g_spell_ring, 3, out);
    return true;
}

int ui_field(std::uint8_t rgb[3]) {
    if (g_ui_field_drawn >= 2 && rgb) std::copy_n(g_ui_field_drawn_rgb, 3, rgb);
    return g_ui_field_drawn;
}

const char* margin_state_debug() {
    static char buf[160];
    std::snprintf(buf, sizeof buf,
                  "layers=%X pillarbox=%d battle=%d panel_found=%d "
                  "ui_shift_valid=%d vram=%d io=%d extra=%d/%d",
                  g_margin_layers, g_ws_pillarbox, g_battle_scene ? 1 : 0,
                  g_panel_found ? 1 : 0, g_ui_shift_valid ? 1 : 0,
                  g_vram ? 1 : 0, g_io ? 1 : 0, g_extra_left, g_extra_right);
    return buf;
}

bool is_status_scene() { return g_status_screen; }

bool is_status_scene_now() {
    return gbarecomp::runtime_excursion_active() ? g_exc_status_screen
                                                 : g_status_screen;
}

bool is_battle_scene_now() {
    return gbarecomp::runtime_excursion_active() ? g_exc_battle_scene
                                                 : g_battle_scene;
}


bool unit_panel_rect(int* x, int* y, int* w, int* h) {
    if (!g_panel_found || !g_battle_scene) return false;
    if (x) *x = g_panel_src_x;
    if (y) *y = g_panel_src_y;
    if (w) *w = kPanelTilesW * 8;
    if (h) *h = kPanelTilesH * 8;
    return true;
}

void configure_extended_view(gbarecomp::RunOptions& opts) {
    // Extended-view opt-in. The faithful 240x160 path stays the default: this
    // only makes --view-width / [video].view_width reachable at all. 284x160
    // is true 16:9; 480 is the PPU's compile-time ceiling (kMaxExtraX 120 per
    // side).
    opts.max_view_width = 480;
    opts.extended_view_frame = kol_extended_view_frame;
    // Only the battle map is validated, so this stays out of the player-facing
    // launcher; an explicit --view-width still reaches it.
    opts.launcher_expose_widescreen = false;
}

}  // namespace kol
