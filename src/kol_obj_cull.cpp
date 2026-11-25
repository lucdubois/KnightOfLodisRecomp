// Widening the guest's OWN sprite visibility window.
//
// The game's generic sprite emitter at ROM 0x08076810 rejects any OBJ whose
// screen position falls outside the GBA's native 240x160, and it does so
// BEFORE writing the sprite's fields — so the entry never reaches OAM at all.
// That is why units standing in the extended view's margins were invisible
// while the terrain under them drew correctly: there was nothing to draw, and
// no rendering policy on our side could have revealed it. Measured in OAM: 21
// active entries before scrolling and 30 after, with the new ones arriving at
// x = -32..16 and the active range never once passing -32 either way.
//
// The window is four THUMB immediates in one basic block, and game.toml routes
// exactly those four through the framework's opt-in immediate hook (see the
// "[[thumb_alu_immediate_override]]" block there for the annotated
// disassembly):
//
//   08076874  adds r0,#0x20   left slack      -> 32  + extra_left
//   08076878  movs r1,#0x98   limit, <<17     -> ceil(span / 2)
//   08076884  movs r0,#0x10   top slack       -> 16  + extra_top
//   0807688C  cmp  r1,#0xC0   bottom limit    -> 192 + extra_bottom
//
// This is the whole point of doing it here rather than synthesising sprites
// ourselves: the guest emits them, so animation, facing, palette and the
// isometric depth sort all still come from the game.
//
// KOL_NO_OBJ_CULL_WIDEN=1 reverts to the stock window at runtime.
#include "kol_obj_cull.h"
#include "kol_focus.h"
#include "kol_rain.h"
#include "kol_snow.h"
#include "kol_settings.h"
#include "kol_widescreen.h"   // ui_sprite_shift

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gba_bus.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "runtime_arm.h"          // g_runtime_state_epoch
#include <cstdlib>
#include "kol_region.h"

// Declared here rather than included: only gbarecomp/src/runtime is on this
// target's include path, and this lives in gbarecomp/src/armv4t.
typedef int (*RuntimeThumbAluImmediateOverride)(std::uint32_t instruction_pc,
                                                std::uint32_t original_value,
                                                std::uint32_t* out_value);
extern RuntimeThumbAluImmediateOverride g_runtime_thumb_alu_imm_override;
extern "C" int (*g_ws_obj_x_provider)(int raw_x, int* out_x);
extern "C" unsigned (*g_ws_obj_margin_repeat_provider)(int, std::uint16_t,
                                                       std::uint16_t,
                                                       std::uint16_t);
extern "C" int g_ws_obj_repeat_skew_y;
extern "C" int (*g_ws_obj_attr_y_provider)(int oam_index, std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t attr2, int* out_y);

namespace kol {
namespace {

// The stock window, and the anchors every widened value is derived from.
constexpr std::uint32_t kStockLeftSlack    = 0x20u;  // 32 px
constexpr std::uint32_t kStockLimitHalf    = 0x98u;  // <<17 == 304 px
constexpr std::uint32_t kStockTopSlack     = 0x10u;  // negated to -16
constexpr std::uint32_t kStockBottomLimit  = 0xC0u;  // 192

std::uint32_t g_left_slack   = kStockLeftSlack;
std::uint32_t g_limit_half   = kStockLimitHalf;
std::uint32_t g_top_slack    = kStockTopSlack;
std::uint32_t g_bottom_limit = kStockBottomLimit;
// While false the callback declines every site, so the generated code keeps
// its compile-time operand and the faithful 240x160 render stays byte-exact.
bool g_widened = false;
bool g_disabled = false;
// KOL_OBJ_CULL_SLACK=<px> adds slack beyond the rendered margins on every
// side. A diagnostic: setting it very large answers "is this a bounds size
// question at all, or is the sprite filtered somewhere else entirely" in one
// run, which no watchpoint here can.
unsigned g_extra_slack = 0;
// The margins actually rendered, kept for the 9-bit unwrap below.
unsigned g_view_left = 0, g_view_right = 0;

// ── Recovering the true OBJ Y from the guest's own slot list ────────────
//
// OAM's Y is 8 bits, and the hardware reading ("160 or more means negative")
// is exact only for rows in [-96, 159]. A tall view needs more than that: at
// 480x270 sprites span 192 + 55 + 55 = 302 rows, so a single byte cannot
// address them and BOTH readings of one value can land on screen. No rule
// over OAM alone can separate those two cases.
//
// It does not have to. Before the guest truncates anything it records each
// sprite in its own slot list, and there the screen position is a full 16-bit
// pair — `[slot+0x1C]` X and `[slot+0x1E]` Y, verified against the projection
// on `AR_rictor_bottom` (164, 100, 100, 132, 108, 124, 84, 108). The list
// survives to the frame boundary, so the renderer can simply look the sprite
// up instead of guessing.
//
// A unit is two 16x16 halves drawn at `[anchor-32, anchor)` and the OAM entry
// sits 8 px left of the slot's X, so each slot predicts two entries:
// `(scrX - 8, scrY - 32)` and `(scrX - 8, scrY - 16)`. Matching on the PAIR is
// what makes this exact rather than a heuristic — two sprites 256 rows apart
// would share a Y byte, but not an X as well.
constexpr std::uint32_t kSlotArray = usa(0x03004150u);   // IWRAM, stride 0x20
constexpr unsigned kSlotStride = 0x20u;
constexpr unsigned kSlotCount  = 128u;

// Indexed by the byte the renderer actually sees, because this is called for
// every OBJ on every scanline -- about 35k times a frame. A linear scan of the
// expected list would be ~9M comparisons a frame.
//
// It is a counting sort rather than fixed-width buckets, and that matters:
// an earlier version capped each bucket at four and DECLINED when it
// overflowed. Units in this game stand on isometric rows, so four or more
// sharing a screen row is ordinary, and every one of them then fell through
// to hardware decoding -- which is the flash of a top-of-field sprite at the
// bottom edge. There is no cap here, so the case cannot arise.
constexpr unsigned kMaxExpected = kSlotCount * 4u;   // a slot's halves, and a 32-wide's right column
std::int16_t g_ex_x[kMaxExpected];
std::int16_t g_ex_y[kMaxExpected];
std::uint16_t g_start[257];
unsigned g_ex_n = 0;
bool g_have_table = false;
// The previous frame's table too. While the camera scrolls in view mode the
// guest updates its slot list a frame BEFORE the OAM being drawn catches up:
// measured on states/BE_scroll_left.state, every moving unit sat 4 px (one
// scroll step) from its slot, no exact pair matched, and units below the
// view fell through to hardware decoding and flashed in the sky at the top.
// The OAM comes from one of the last two samples, so a pair is looked up in
// the current one first, then in the previous.
std::int16_t g_px[kMaxExpected];
std::int16_t g_py[kMaxExpected];
std::uint16_t g_pstart[257];
bool g_have_prev = false;
// KOL_OBJY_STATS=1 reports, per frame, how many lookups fell through in the
// wrapped band. Parked sprites live at y=168 and legitimately never match,
// so they are excluded -- counting them buried the real signal under 26k.
long g_declined_high = 0;
long g_rebuilds = 0;
int  g_table_entries = 0;
int  g_worst_bucket = 0;
// Rebuilt ONCE per frame, not per scanline sweep. The guest rebuilds its slot
// list during the visible period, while the renderer is still composing the
// frame from the OAM it wrote back in VBlank, so a mid-frame read picks up the
// NEXT frame's positions and every lookup misses. Measured while scrolling:
// 26,730 declines in a single frame, which is the flash of a top-of-field
// sprite at the bottom edge. The frame hook sets this, so the first lookup of
// a frame samples the list that produced the OAM now being drawn.
bool g_table_dirty = true;

// The grid cursor's screen place, from its slot (routine 0x08057AD5, as in
// kol_focus.cpp), in this sample and the one before: it glides between cells
// and the OAM drawn comes from one of the two. Absent (false) when no cursor
// slot is live; with a menu up the slot sits at (-24, -300), off every
// sprite. See grid_cursor_sprite().
constexpr std::uint32_t kCursorSlotFn = usa(0x08057AD5u);
bool g_cursor_now = false, g_cursor_prev = false;
int g_cursor_x = 0, g_cursor_y = 0, g_cursor_px = 0, g_cursor_py = 0;

inline int rd16(const std::uint8_t* p) {
    return static_cast<int>(static_cast<std::int16_t>(
        static_cast<std::uint16_t>(p[0] | (p[1] << 8))));
}

void rebuild_expected() {
    ++g_rebuilds;
    g_have_prev = g_have_table;
    if (g_have_table) {
        std::memcpy(g_px, g_ex_x, sizeof g_px);
        std::memcpy(g_py, g_ex_y, sizeof g_py);
        std::memcpy(g_pstart, g_start, sizeof g_pstart);
    }
    g_ex_n = 0;
    g_table_entries = 0;
    g_worst_bucket = 0;
    g_have_table = false;
    g_cursor_prev = g_cursor_now;
    g_cursor_px = g_cursor_x;
    g_cursor_py = g_cursor_y;
    g_cursor_now = false;
    for (unsigned i = 0; i < 257u; ++i) g_start[i] = 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* iw = bus->iwram_ptr();
    if (!iw) return;

    std::int16_t tx[kMaxExpected], ty[kMaxExpected];
    unsigned n = 0;
    const std::uint32_t base = kSlotArray - 0x03000000u;
    // The camera's row, as most slots have it (sy = (X + Z) / 4 - Y / 2 + ky
    // from their own +8/+0xA/+0xC): a slot whose row is a whole wrap off it
    // is put back. Map 3's church door (actor 0x70)
    // keeps its place in a byte: off the view's top as the camera panned down
    // to the shore its slot said row 212 for -44, the table predicted it on
    // the beach, and there it stood for five frames (states/thor/0013; the
    // user saw the door flash briefly in the field). (Off the
    // cartridge's 160 rows either reading is unseen.) KOL_OBJY_NO_UNWRAP=1:
    // the slots as they are.
    static const bool no_unwrap = std::getenv("KOL_OBJY_NO_UNWRAP") != nullptr;
    int cams_y[kSlotCount];
    unsigned cams = 0;
    for (unsigned i = 0; i < kSlotCount && !no_unwrap; ++i) {
        const std::uint32_t o = base + i * kSlotStride;
        if (o + kSlotStride > 0x8000u) break;
        if (!(iw[o] | iw[o + 1] | iw[o + 2] | iw[o + 3])) continue;
        const int wx = rd16(iw + o + 0x08), wy = rd16(iw + o + 0x0A), wz = rd16(iw + o + 0x0C);
        if (!wx && !wz) continue;
        cams_y[cams] = rd16(iw + o + 0x1E) - (wx + wz) / 4 + wy / 2;
        ++cams;
    }
    int cam_y = 0;
    if (cams > 0) {
        std::nth_element(cams_y, cams_y + cams / 2, cams_y + cams);
        cam_y = cams_y[cams / 2];
    }
    for (unsigned i = 0; i < kSlotCount && n + 4u <= kMaxExpected; ++i) {
        const std::uint32_t o = base + i * kSlotStride;
        if (o + kSlotStride > 0x8000u) break;
        // A live slot carries its owner pointer at +0; a cleared one is zero
        // throughout and would otherwise predict a sprite at (-8, -32).
        const std::uint32_t owner = static_cast<std::uint32_t>(iw[o]) |
                                    (static_cast<std::uint32_t>(iw[o+1]) << 8) |
                                    (static_cast<std::uint32_t>(iw[o+2]) << 16) |
                                    (static_cast<std::uint32_t>(iw[o+3]) << 24);
        if (owner == 0u) continue;
        int sx = rd16(iw + o + 0x1C);
        int sy = rd16(iw + o + 0x1E);
        if (cams > 2) {
            const int wx = rd16(iw + o + 0x08), wy = rd16(iw + o + 0x0A), wz = rd16(iw + o + 0x0C);
            // Only a row in the byte's wrapped band (160-255), which the
            // hardware reads as negative, and only to that reading: a gull's
            // slot (map 3, flying high) does not follow the walkers' formula,
            // and moved a wrap it broke into pieces on alternate frames.
            if ((wx || wz) && sy >= 160 && sy < 256) {
                const int ey = (wx + wz) / 4 - wy / 2 + cam_y;
                if (std::abs(ey - (sy - 256)) < std::abs(ey - sy)) sy -= 256;
            }
        }
        if (owner == kCursorSlotFn && !g_cursor_now) {
            g_cursor_now = true;
            g_cursor_x = sx;
            g_cursor_y = sy;
        }
        // The grid cursor's slot is no unit's: its own sprites (the hand,
        // the chevrons) never match its predictions, and in the near search
        // it made a unit off the top ambiguous where the cursor stood on the
        // byte-wrapped row (a unit 256 rows above it): the lookup declined
        // and the unit's head flashed beside the cursor for a frame while
        // the camera scrolled (states/GW_slot3_scroll_right_flash, frame 72;
        // the mage's top at -132 against the cursor's row 124).
        // KOL_OBJY_CURSOR_SLOT=1: the old table.
        static const bool cursor_in_table = std::getenv("KOL_OBJY_CURSOR_SLOT") != nullptr;
        if (owner == kCursorSlotFn && !cursor_in_table) continue;
        tx[n] = static_cast<std::int16_t>(sx - 8);
        ty[n] = static_cast<std::int16_t>(sy - 32); ++n;
        tx[n] = static_cast<std::int16_t>(sx - 8);
        ty[n] = static_cast<std::int16_t>(sy - 16); ++n;
        // A 32-wide object's right column (16 x 16s at its feet's column and
        // 16 right of it: map 3's church door, actor 0x70). Only its left
        // column fell within the near search of the halves above; the right
        // one took the hardware's reading (row 155 for -101) and stood on the
        // beach as the camera panned down to the shore (states/thor/0013).
        // KOL_OBJY_NO_RIGHT=1: as before.
        static const bool no_right = std::getenv("KOL_OBJY_NO_RIGHT") != nullptr;
        if (!no_right) {
            tx[n] = static_cast<std::int16_t>(sx + 8);
            ty[n] = static_cast<std::int16_t>(sy - 32); ++n;
            tx[n] = static_cast<std::int16_t>(sx + 8);
            ty[n] = static_cast<std::int16_t>(sy - 16); ++n;
        }
    }
    if (n == 0) return;

    unsigned cnt[256] = {0};
    for (unsigned i = 0; i < n; ++i) ++cnt[static_cast<unsigned>(ty[i]) & 0xFFu];
    unsigned run = 0;
    for (unsigned bkt = 0; bkt < 256u; ++bkt) {
        g_start[bkt] = static_cast<std::uint16_t>(run);
        run += cnt[bkt];
        if (static_cast<int>(cnt[bkt]) > g_worst_bucket)
            g_worst_bucket = static_cast<int>(cnt[bkt]);
    }
    g_start[256] = static_cast<std::uint16_t>(run);
    unsigned pos[256];
    for (unsigned bkt = 0; bkt < 256u; ++bkt) pos[bkt] = g_start[bkt];
    for (unsigned i = 0; i < n; ++i) {
        const unsigned bkt = static_cast<unsigned>(ty[i]) & 0xFFu;
        const unsigned at = pos[bkt]++;
        g_ex_x[at] = tx[i];
        g_ex_y[at] = ty[i];
    }
    g_ex_n = n;
    g_table_entries = static_cast<int>(n);
    g_have_table = true;
}

// Which reading of a sprite's wrapped OAM position is the one the guest
// meant. OAM's Y is one byte and its X nine bits, so a raw value has readings
// 256 (512) apart; the guest's slot list says which. An exact (x, y) match in
// the current or the previous sample answers outright. While the camera
// scrolls in view mode no exact match exists at all: the slot list moves
// 8 px between samples and the OAM on screen sits halfway, 4 px from either
// (states/BE_scroll_left.state), and every unit below the view fell through
// to hardware decoding and flashed at the top of the sky. So a recorded
// sprite within kNear px, in either sample, picks the reading nearest to it.
// The readings are 256 apart and kNear is 16, so one unit cannot suggest two
// of them; candidates that disagree decline. Answers are cached per raw pair
// for the frame, as this runs for every sprite on every scanline. Only a
// 16x16 sprite, the size of both halves of every unit, is matched near: the
// roster panel's 8x8 selection brackets sit within 16 px of units standing
// on the deployment tiles behind it, were taken for them, and so stayed at
// their native position instead of moving with the panel
// (states/BF_panel_brackets.state).
constexpr int kNear = 16;

bool unit_sized(std::uint16_t attr0, std::uint16_t attr1) {
    return (attr0 >> 14) == 0u && (attr1 >> 14) == 1u;   // square, 16x16
}

// The guest parks every OAM entry it does not use this frame as an 8x8 at
// (x 248, y 168), off its 240x160 -- but inside the widened view's
// bottom-right margin, so only the hardware's reading of the byte (y = -88)
// keeps it hidden. A unit's half standing exactly there (its slot predicts
// (248, 168)) matched every parked entry, and the kept row then held them
// all there for as long as they stayed parked: tile 0, a green scrap, stuck
// in the bottom-right corner over whatever the camera showed, until a state
// load (the Thor, 2026-10-15; states/HA_slot3_rogue_green_piece with the
// left stick, 847 presents in). The slot list predicts 16x16 unit halves
// only, so an 8x8 there is never a recorded sprite: it keeps the hardware's
// reading. KOL_OBJY_PARKED_OLD=1: the old way.
bool parked(std::uint16_t attr0, std::uint16_t attr1) {
    static const bool old = std::getenv("KOL_OBJY_PARKED_OLD") != nullptr;
    return !old && (attr0 & 0xC0FFu) == 0x00A8u && (attr1 & 0xC1FFu) == 0x00F8u;
}

int wrap_near(int raw, int ref, int period) {
    int best = raw;
    for (int k = -1; k <= 1; ++k) {
        const int v = raw + k * period;
        if (std::abs(v - ref) < std::abs(best - ref)) best = v;
    }
    return best;
}

// 1 and the true (x, y), 0 when nothing recorded is there, -1 ambiguous.
int search(int raw_x, int raw_y, bool near, int* tx, int* ty) {
    int found = 0, ax = 0, ay = 0;
    const std::int16_t* xs[2] = {g_ex_x, g_px};
    const std::int16_t* ys[2] = {g_ex_y, g_py};
    const std::uint16_t* st[2] = {g_start, g_pstart};
    const int tables = g_have_prev ? 2 : 1;
    for (int t = 0; t < tables; ++t) {
        for (int dy = near ? -kNear : 0; dy <= (near ? kNear : 0); ++dy) {
            const int b = (raw_y + dy) & 0xFF;
            for (unsigned i = st[t][b]; i < st[t][b + 1]; ++i) {
                const int rx = xs[t][i], ry = ys[t][i];
                int dx = ((rx & 0x1FF) - raw_x) & 0x1FF;
                if (dx > 0xFF) dx -= 0x200;
                if (near ? std::abs(dx) > kNear : dx != 0) continue;
                const int cx = near ? wrap_near(raw_x, rx, 512) : rx;
                const int cy = near ? wrap_near(raw_y, ry, 256) : ry;
                if (found && (cx != ax || cy != ay)) return -1;
                ax = cx; ay = cy; found = 1;
            }
        }
        if (found && !near) break;   // an exact match in the current sample wins
    }
    if (found) { *tx = ax; *ty = ay; }
    return found;
}

struct Resolved { long gen = -1; int key = -1; int res = 0; int x = 0, y = 0; };
Resolved g_resolved[512];

int find_recorded(int raw_x, int raw_y, bool near_ok, int* tx, int* ty) {
    const int key = raw_x | (raw_y << 9) | (near_ok ? 1 << 17 : 0);
    Resolved* e = nullptr;
    for (int p = 0; p < 8; ++p) {
        Resolved& c = g_resolved[(key * 2654435761u + p) % 512u];
        if (c.gen == g_rebuilds && c.key == key) {
            *tx = c.x; *ty = c.y;
            return c.res;
        }
        if (!e && c.gen != g_rebuilds) e = &c;
    }
    int res = search(raw_x, raw_y, false, tx, ty);
    if (res == 0 && near_ok) res = search(raw_x, raw_y, true, tx, ty);
    if (e) { e->gen = g_rebuilds; e->key = key; e->res = res; e->x = *tx; e->y = *ty; }
    return res;
}

// raw_y is one byte, so the two candidate rows are raw_y and raw_y - 256.
// Answer only when the guest recorded a sprite at this exact (x, y) pair.
// Anything it did not record -- the HUD, the cursor, effects -- falls through
// to hardware behaviour, so this fails closed. Returns 2 for an interface
// sprite moved with its box (not a lookup: never kept, see StickyY).
int on_obj_y_now(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
                 int* out_y) {
    if (!out_y || !g_widened) return 0;
    // An interface sprite on an anchored element moves with it, as in
    // kol_widescreen.cpp's X provider (the menu's hand on its box).
    int ui_dx = 0, ui_dy = 0;
    if (kol::ui_sprite_shift(attr0, attr1, attr2, &ui_dx, &ui_dy)) {
        int y = static_cast<int>(attr0 & 0xFFu);
        if (y >= 160) y -= 256;
        *out_y = y + ui_dy;
        return 2;
    }
    if (g_table_dirty) {
        g_table_dirty = false;
        rebuild_expected();
    }
    if (!g_have_table) return 0;

    const int raw_y = static_cast<int>(attr0 & 0xFFu);
    const int raw_x = static_cast<int>(attr1 & 0x1FFu);
    if (parked(attr0, attr1)) return 0;
    // The command menu's icons (tiles 0x1D7-0x203, interface: kol_focus's
    // tile blocks) are never a unit's: on states/thor/0070 (map 13,
    // Alphonse's menu) the Attack and Item icons, 16x16 at row 104, lay
    // where units 4-6's pieces would be read a wrap up (-152: they stand
    // above the view) and were drawn there, out of sight (the user found the
    // attack and item icons missing).
    if (const unsigned tile = attr2 & 0x3FFu; tile >= 0x1D7u && tile <= 0x203u) return 0;
    int answer = 0, true_x = 0;
    const int found = find_recorded(raw_x, raw_y, unit_sized(attr0, attr1), &true_x, &answer);
    if (found < 0) return 0;   // ambiguous: decline
    if (!found) {
        // A fall-through in this band means a sprite the guest put ABOVE the
        // screen is about to be drawn at a positive row. Parked sprites sit at
        // y=168 and can never match, so they are not a failure.
        if (raw_y >= 160 && !(raw_y == 0xA8 && raw_x == 0xF8)) ++g_declined_high;
        return 0;
    }
    *out_y = answer;
    return 1;
}

// Kept per entry for the frame (see obj_attr_new_frame).
struct AttrMemo {
    std::uint64_t epoch = 0;
    std::uint16_t a0 = 0, a1 = 0, a2 = 0;
    int ret = 0, value = 0;
};
std::uint64_t g_attr_epoch = 1;
AttrMemo g_y_memo[128];

// The last row each OAM entry was resolved to, kept across frames: the
// guest's slots go by call order, not by unit, and a slot that recorded a
// sprite can hold another unit's place a frame later while OAM still
// carries the first sprite, unchanged. Its lookup then found nothing and the
// hardware reading drew it 256 rows off: a unit above the view (its head at
// -108, byte 148) flashed into the bottom-left margin every few frames
// (states/GU_slot3_sprite_flicker_bottom_left, perspective on, 2026-10-15).
// An entry whose three attributes are what they were when last resolved is
// that same sprite, so it keeps that row -- but only while it stays so: the
// row is forgotten the first time the entry shows anything else, and on
// every state load (a row kept from before them once left a floating piece
// of a unit at the bottom right until the app was restarted, GV). KOL_OBJY_NO_STICKY=1:
// the old way. Only the lookup's rows are kept: a dialogue portrait's row
// while its box slid into place (moved with it, ui_sprite_shift) was kept
// and the portrait hung 28 rows low under its frame once the box had
// stopped, as long as nothing else took its entries -- with our rain the
// game's hidden drops no longer did (JM played on to the soldier's line,
// 2026-10-26).
struct StickyY {
    bool valid = false;
    std::uint16_t a0 = 0, a1 = 0, a2 = 0;
    int y = 0;
};
StickyY g_sticky_y[128];
std::uint32_t g_sticky_epoch = 0;

// Each entry's row last frame, for continuity: a sprite cannot move 256
// rows in a frame, but the two readings of its one Y byte are 256 apart. A
// lookup can still pick the wrong one -- its own slot gone that frame while
// another unit's prediction lands on the same x and byte -- and the Thor
// showed it (a unit above the view, its head at the bottom edge for one frame
// while scrolling, slot 3 GW, 2026-10-15; not reproduced on the Mac). So
// when this frame's row is far (over 64) from the same sprite's last frame
// (same tile and palette, x within 16) and the other reading is near, the
// other reading is taken. Frame to frame only, and forgotten on a state load.
// KOL_OBJY_NO_CONTINUITY=1: the old way.
// It may override the lookup two frames running at most: every case it
// fixed lasted one frame, and a sprite that truly moves ~200 rows in a frame
// (the camera snapping) was otherwise held at the wrong reading for as long
// as it stayed still -- each frame compared with the last frame's wrong row
// -- a rogue piece of a unit standing in the margin until its next animation
// step (the Thor, 2026-10-15; gone from any saved state, which resets it).
struct LastY {
    std::uint64_t epoch = 0;
    std::uint16_t a2 = 0;
    int x = 0, y = 0;
    int overrides = 0;   // frames running this entry took the other reading
};
LastY g_last_y[128];

}  // namespace
bool confirm_arrow_piece(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2);
bool confirm_arrow_hidden_now();
namespace {
bool own_rain_hides(std::uint16_t attr0, std::uint16_t attr1);

// The confirmation's arrow from the actor's box to the target's (attack,
// spell, item; priority 0): dots of three sizes (tiles 0x148-0x14A), a ball
// running along it (0x13C over 0x13E), and its head, blue (0x144 over 0x146)
// and white (0x140 over 0x142) in turn, each two 16x8 halves. Tiles
// 0x13C-0x14A at priority 0 showed only in the confirmations (BV, BW, CH,
// GD, states/thor/0069; none in 15 other battle states). At first only
// 0x144-0x149: the ball and the white head stayed (BW; the user saw them
// stay for the whole animation). It is left out of the top screen
// while the second screen is up: its head is drawn there, between the two
// cards (kol_sheet draw_sheet_arrow; the user, 2026-11-01, asked for the
// blue arrow to be hidden on the top screen). KOL_KEEP_CONFIRM_ARROW=1:
// as before; KOL_CONFIRM_ARROW_TEST=1: as with the second screen (desktop).
// And only where it is drawn: the strip between the two units' boxes at the
// top (its pieces' corners at x 101-126, y 16-24 in every confirmation
// seen), so another sprite the game loads into those tiles elsewhere is not
// taken (the user asked whether other sprites could be affected).
bool confirm_arrow_hides(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    if (!confirm_arrow_piece(attr0, attr1, attr2)) return false;
    return confirm_arrow_hidden_now();
}

int on_obj_y(int oam_index, std::uint16_t attr0, std::uint16_t attr1,
             std::uint16_t attr2, int* out_y) {
    if (!out_y) return 0;
    if (own_rain_hides(attr0, attr1) || object_decal_hidden(attr2) || confirm_arrow_hides(attr0, attr1, attr2)) {
        *out_y = -1024;   // far above the view: not drawn (our own rain is)
        if (oam_index >= 0 && oam_index < 128) {   // the entry showed something else
            g_sticky_y[oam_index].valid = false;
            g_last_y[oam_index].epoch = 0;
        }
        return 1;
    }
    if (oam_index < 0 || oam_index >= 128) return on_obj_y_now(attr0, attr1, attr2, out_y) ? 1 : 0;
    AttrMemo& m = g_y_memo[oam_index];
    if (m.epoch != g_attr_epoch || m.a0 != attr0 || m.a1 != attr1 || m.a2 != attr2) {
        int v = 0;
        m.ret = on_obj_y_now(attr0, attr1, attr2, &v);
        // A piece of a figure whose next pieces (already placed: the picture
        // goes from OAM 127 down) were read a wrap up follows them when that
        // reading puts it against one of them, its tiles near theirs. A rider
        // over the top of the view (map 95, states/thor/0244, the camera
        // panning up to him): the slot list placed his horse and body at -92,
        // his head (OAM 10-11, raw 148) matched no record and stayed at 148,
        // hidden behind the house; the lifted walls showed it there, a head
        // at the house's door. KOL_OBJY_NO_FOLLOW=1: as before.
        if (!m.ret && g_widened) {
            static const bool no_follow = std::getenv("KOL_OBJY_NO_FOLLOW") != nullptr;
            const int raw = static_cast<int>(attr0 & 0xFFu);
            if (!no_follow && raw < 160 && !parked(attr0, attr1)) {
                const unsigned shape = (attr0 >> 14) & 3u, size = (attr1 >> 14) & 3u;
                static constexpr int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
                static constexpr int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
                if (shape < 3) {
                    const int w = kW[shape][size], h = kH[shape][size], x = static_cast<int>(attr1 & 0x1FFu);
                    const int up = raw - 256;
                    for (int j = oam_index + 1; j < 128 && j <= oam_index + 4; ++j) {
                        const AttrMemo& o = g_y_memo[j];
                        if (o.epoch != g_attr_epoch) continue;
                        const int oraw = static_cast<int>(o.a0 & 0xFFu);
                        // Where it was drawn: the answer, else the hardware's
                        // reading (160 and up a wrap up).
                        const int oy = o.ret ? o.value : (oraw >= 160 ? oraw - 256 : oraw);
                        if (oy != oraw - 256 || oy >= 0) continue;   // (only a figure read above the view)
                        const unsigned os = (o.a0 >> 14) & 3u, oz = (o.a1 >> 14) & 3u;
                        if (os >= 3) continue;
                        const int ow = kW[os][oz], oh = kH[os][oz], ox = static_cast<int>(o.a1 & 0x1FFu);
                        const int dt = static_cast<int>(attr2 & 0x3FFu) - static_cast<int>(o.a2 & 0x3FFu);
                        if (dt < -0x40 || dt > 0x40) continue;
                        if (x > ox + ow || ox > x + w || up > oy + oh || oy > up + h) continue;   // touching
                        m.ret = 1;
                        v = up;
                        break;
                    }
                }
            }
        }
        // KOL_OBJY_TRACE=1 (the Thor's files/env.cfg too): which rule placed
        // each sprite that lands in a margin (above the 160 rows or below),
        // logged when it changes. 'L' the lookup, 'S' the kept row, 'C'
        // continuity, 'H' the hardware reading.
        char how = m.ret ? 'L' : 'H';
        static const bool no_sticky = std::getenv("KOL_OBJY_NO_STICKY") != nullptr;
        if (g_sticky_epoch != g_runtime_state_epoch) {
            g_sticky_epoch = g_runtime_state_epoch;
            for (StickyY& e : g_sticky_y) e.valid = false;
            for (LastY& e : g_last_y) e.epoch = 0;
        }
        StickyY& st = g_sticky_y[oam_index];
        if (st.valid && (st.a0 != attr0 || st.a1 != attr1 || st.a2 != attr2)) st.valid = false;
        if (m.ret == 2) {
            m.ret = 1;
            st.valid = false;
        } else if (m.ret) {
            st.valid = true;
            st.a0 = attr0; st.a1 = attr1; st.a2 = attr2;
            st.y = v;
        } else if (!no_sticky && g_widened && st.valid && st.a0 == attr0 && st.a1 == attr1 && st.a2 == attr2) {
            m.ret = 1;
            v = st.y;
            how = 'S';
        }
        if (g_widened) {
            static const bool no_continuity = std::getenv("KOL_OBJY_NO_CONTINUITY") != nullptr;
            const int raw = static_cast<int>(attr0 & 0xFFu);
            const int x = static_cast<int>(attr1 & 0x1FFu);
            int y = m.ret ? v : (raw >= 160 ? raw - 256 : raw);
            LastY& last = g_last_y[oam_index];
            int dx = (x - last.x) & 0x1FF;
            if (dx > 0xFF) dx -= 0x200;
            if (!no_continuity && last.epoch != 0 && last.epoch + 1 == g_attr_epoch && last.a2 == attr2 &&
                std::abs(dx) <= 16 && (y & 0xFF) == raw && !parked(attr0, attr1)) {
                const int alt = y == raw ? raw - 256 : raw;
                if (std::abs(y - last.y) > 64 && std::abs(alt - last.y) <= 64 && last.overrides < 2) {
                    y = alt;
                    m.ret = 1;
                    v = alt;
                    ++last.overrides;
                    how = 'C';
                } else {
                    last.overrides = 0;
                }
            } else {
                last.overrides = 0;
            }
            static const bool trace = [] { const char* e = std::getenv("KOL_OBJY_TRACE"); return e && *e && *e != '0'; }();
            if (trace && (y < -8 || y >= 160) && !(raw == 0xA8 && x == 0xF8 && y == -88)) {   // (not the hidden parked slots)
                static std::uint64_t sig[128];
                const std::uint64_t sg = (static_cast<std::uint64_t>(attr0) << 48) ^ (static_cast<std::uint64_t>(attr1) << 32) ^
                                         (static_cast<std::uint64_t>(attr2) << 16) ^ static_cast<std::uint64_t>(how) ^
                                         (static_cast<std::uint64_t>(y & 0x3FF) << 8);
                if (sig[oam_index] != sg) {
                    sig[oam_index] = sg;
                    std::fprintf(stderr, "objy: f=%llu #%d a0=%04x a1=%04x a2=%04x raw=%d -> y=%d %c (last %d, frame %llu)\n",
                                 static_cast<unsigned long long>(g_attr_epoch), oam_index, attr0, attr1, attr2, raw, y, how,
                                 last.y, static_cast<unsigned long long>(last.epoch));
                }
            }
            last.epoch = g_attr_epoch;
            last.a2 = attr2;
            last.x = x;
            last.y = y;
        }
        m.value = v;
        m.epoch = g_attr_epoch;
        m.a0 = attr0; m.a1 = attr1; m.a2 = attr2;
    }
    if (m.ret) *out_y = m.value;
    return m.ret;
}

}  // namespace

// The true X of a sprite the guest recorded, by the same (X, Y) pair match as
// on_obj_y. OAM's X is 9 bits, so every value has two readings 512 apart,
// and at 480 wide both can be on screen: the widened cull emits units up to
// x = 392, whose signed reading 392 - 512 = -120 is exactly the left edge.
// on_obj_x below can only guess which reading is meant, and it guessed the
// visible one — units standing off the right edge drawn in the left margin
// (states/AX_units_left_margin.state). The slot list records the answer.
bool obj_true_x(std::uint16_t attr0, std::uint16_t attr1, int* out_x, bool near) {
    if (!out_x || !g_widened) return false;
    if (g_table_dirty) {
        g_table_dirty = false;
        rebuild_expected();
    }
    if (!g_have_table) return false;
    const int raw_y = static_cast<int>(attr0 & 0xFFu);
    const int raw_x = static_cast<int>(attr1 & 0x1FFu);
    int answer = 0, true_y = 0;
    if (find_recorded(raw_x, raw_y, near && unit_sized(attr0, attr1), &answer, &true_y) != 1)
        return false;
    *out_x = answer;
    return true;
}

// Whether a sprite (native screen rect, OAM's signed reading) is one the grid
// cursor draws: its corner pieces lie on its cell, 12 rows either side of the
// slot's place, and its pointing hand floats above, 27-43 rows up as it bobs
// (kol_focus.cpp, the cursor in build_scene_now). The area is that, 16
// columns either side, plus 8 for the glide between the two samples.
bool grid_cursor_sprite(int x, int y, int w, int h) {
    if (g_table_dirty) {
        g_table_dirty = false;
        rebuild_expected();
    }
    auto inside = [&](int cx, int cy) {
        return x >= cx - 24 && x + w <= cx + 24 && y >= cy - 56 && y + h <= cy + 20;
    };
    return (g_cursor_now && inside(g_cursor_x, g_cursor_y)) ||
           (g_cursor_prev && inside(g_cursor_px, g_cursor_py));
}

namespace {

// ── Weather, repeated into the margins ──────────────────────────────────
//
// The rain is a screen-space particle effect the guest only ever places
// inside its own 240x160, so the widened view had rain in the middle and dry
// margins. The particles are an EWRAM array of ten 0x14-byte entries at
// 0x0202A0F8 (the literal at 0x0804926C): +0x04 nonzero when live, +0x08 and
// +0x0A the position in 8.8 fixed point. The routine at 0x08049188 draws each
// live one as an 8x8 sprite at (x - 4, y - 4). Measured: six live particles,
// six rain sprites, every position exact.
//
// A sprite is repeated into all eight neighbouring native screens only when
// it matches a live particle's position, so nothing else is ever repeated.
// The renderer draws repeats only outside the native 240x160. Sampled once
// per frame with the unit table, for the same reason (see g_table_dirty).
constexpr std::uint32_t kWeatherParticles = usa(0x0202A0F8u);
constexpr unsigned kWeatherCount = 10u;
constexpr unsigned kWeatherStride = 0x14u;
int g_weather_x[kWeatherCount];
int g_weather_y[kWeatherCount];
unsigned g_weather_n = 0;
bool g_weather_dirty = true;
bool g_weather_off = false;       // the player's switch (BACK menu)
bool g_weather_env_off = false;   // KOL_WS_NO_WEATHER_REPEAT, for A/B work
bool g_rain_now = false;          // the weather block says rain, this frame
bool g_snow_now = false;          // ... snow (+7 3)
// Half the screen height: the right margin shows the rain 80 rows higher than
// the middle does and the left margin 80 rows lower, so neither side reads as
// a copy of the centre. With the view's 55 extra rows each way every row is
// still covered (the lattice needs skew <= 105).
constexpr int kWeatherSkew = 80;

void rebuild_weather() {
    g_weather_n = 0;
    g_rain_now = false;
    g_snow_now = false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* ew = bus->ewram_ptr();
    if (!ew) return;
    const std::uint8_t* block = ew + (usa(0x0202A1C0u) - 0x02000000u);
    g_rain_now = block[4] != 0 && block[7] != 3;
    g_snow_now = block[4] != 0 && block[7] == 3;
    for (unsigned i = 0; i < kWeatherCount; ++i) {
        const std::uint8_t* p = ew + (kWeatherParticles - 0x02000000u) + i * kWeatherStride;
        if (p[4] == 0) continue;
        const int x = (p[8] | (p[9] << 8)) >> 8;
        const int y = (p[10] | (p[11] << 8)) >> 8;
        g_weather_x[g_weather_n] = (x - 4) & 0x1FF;
        g_weather_y[g_weather_n] = (y - 4) & 0xFF;
        ++g_weather_n;
    }
}

// ── Our own rain in place of the game's ─────────────────────────────────
//
// While our rain draws (kol_rain: Options > New Rain, its last picture had
// rain), the game's drops are not drawn: the sprites at the live particles'
// places, in the widened view, while the weather block (0x0202A1C0) says
// rain (+4 on, +7 not 3: snow keeps its flakes). The particles still move
// and draw their random numbers as before: only the picture changes. Where
// ours cannot draw (no scene composed, the authentic view) the game's stay,
// except over a picture faded to black: the game's drops are semi-
// transparent, so its BLDY fade leaves them falling in the black, and
// before map 58's ship scene they fell there for some 80 frames (the map
// not drawn yet, so no scene of ours: JT, the user's slot 2: the old
// rain showed as the new scene came in). With New Rain on they are not drawn
// there; ours comes in with the scene. KOL_RAIN_BLACK_KEEPS_GAME=1: old.

bool own_rain() { return rain_drawn() || rain_mockup(); }

// The line being drawn is faded to black (BLDCNT brightness down at 16 on
// BG0, BG1 and the backdrop), and New Rain is on.
bool own_rain_black() {
    static const bool keep = std::getenv("KOL_RAIN_BLACK_KEEPS_GAME") != nullptr;
    if (keep || !kol::setting_on(kol::Setting::OwnRain)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const unsigned bldcnt = bus->read16(0x04000050u);
    return ((bldcnt >> 6) & 3u) == 3u && (bldcnt & 0x23u) == 0x23u && (bus->read16(0x04000054u) & 31u) >= 16u;
}

// Our own snow the same (kol_snow: Options > New Snow): the game's flakes
// are the same particles, while the weather block says snow (+7 3).
bool own_snow() { return snow_drawn() || snow_mockup(); }
bool own_snow_black() {
    static const bool keep = std::getenv("KOL_RAIN_BLACK_KEEPS_GAME") != nullptr;
    if (keep || !kol::setting_on(kol::Setting::OwnSnow)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const unsigned bldcnt = bus->read16(0x04000050u);
    return ((bldcnt >> 6) & 3u) == 3u && (bldcnt & 0x23u) == 0x23u && (bus->read16(0x04000054u) & 31u) >= 16u;
}

bool own_rain_hides(std::uint16_t attr0, std::uint16_t attr1) {
    if (!g_widened) return false;
    if ((attr0 >> 14) != 0 || (attr1 >> 14) != 0) return false;   // 8x8 only
    if (g_weather_dirty) {
        g_weather_dirty = false;
        rebuild_weather();
    }
    const bool rain = g_rain_now && (own_rain() || own_rain_black());
    const bool snow = g_snow_now && (own_snow() || own_snow_black());
    if (!rain && !snow) return false;
    const int x = attr1 & 0x1FF, y = attr0 & 0xFF;
    for (unsigned i = 0; i < g_weather_n; ++i)
        if (g_weather_x[i] == x && g_weather_y[i] == y) return true;
    return false;
}

unsigned on_obj_repeat(int /*oam_index*/, std::uint16_t attr0,
                       std::uint16_t attr1, std::uint16_t /*attr2*/) {
    if (!g_widened || g_weather_off || g_weather_env_off) return 0;
    if ((attr0 >> 14) != 0 || (attr1 >> 14) != 0) return 0;   // 8x8 only
    if (g_weather_dirty) {
        g_weather_dirty = false;
        rebuild_weather();
    }
    if (g_rain_now && (own_rain() || own_rain_black())) return 0;
    if (g_snow_now && (own_snow() || own_snow_black())) return 0;
    const int x = attr1 & 0x1FF, y = attr0 & 0xFF;
    for (unsigned i = 0; i < g_weather_n; ++i)
        if (g_weather_x[i] == x && g_weather_y[i] == y) return 0xFFu;
    return 0;
}

// ── Undoing the 9-bit OAM X wrap ────────────────────────────────────────
//
// The fallback for sprites the guest did NOT record in its slot list (the
// cursor, the HUD, effects). Units are answered exactly by obj_true_x().
//
// Widening the cull is only half of it. OAM's X field is 9 bits decoded as
// SIGNED, so the furthest right a sprite can be placed is +255; a unit the
// guest now emits at screen x = 296 is written as 296 and the renderer reads
// it back as 296 - 512 = -216. It lands off the LEFT of the extended view and
// is still invisible, which is why widening alone fixed the left margin and
// not the right.
//
// The framework anticipates this: g_ws_obj_x_provider is "optional per-game
// interpretation of the 9-bit OBJ X field in expanded-view rendering". This
// one reinterprets a wrapped value as its unsigned reading, and it is safe by
// construction rather than by heuristic — it only ever touches a sprite whose
// DEFAULT decoding puts it entirely outside the visible region, so it can add
// a sprite that would otherwise be invisible and can never move a visible one.
// The two readings are 512 apart and the widest view is 480, so at most one of
// them is on screen.
int on_obj_x(int raw_x, int* out_x) {
    if (!out_x || !g_widened) return 0;
    if (raw_x < 0x100) return 0;                 // no wrap; the default is right
    const int view_left  = -static_cast<int>(g_view_left);
    const int view_right = 240 + static_cast<int>(g_view_right);  // exclusive
    if (raw_x >= view_right) return 0;           // unsigned reading not visible
    if (raw_x - 0x200 >= view_left) return 0;    // signed reading IS visible
    *out_x = raw_x;
    return 1;
}

int on_immediate(std::uint32_t pc, std::uint32_t original,
                 std::uint32_t* out) {
    if (!g_widened) return 0;
    switch (pc) {
        case usa(0x08076874u): *out = g_left_slack;   return 1;
        case usa(0x08076878u): *out = g_limit_half;   return 1;
        case usa(0x08076884u): *out = g_top_slack;    return 1;
        case usa(0x0807688Cu): *out = g_bottom_limit; return 1;
        default: break;
    }
    (void)original;
    return 0;
}

}  // namespace

void obj_cull_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_OBJ_CULL_WIDEN");
    g_disabled = off && off[0] && off[0] != '0';
    if (g_disabled) return;
    if (const char* e = std::getenv("KOL_OBJ_CULL_SLACK"))
        g_extra_slack = static_cast<unsigned>(std::atoi(e));
    g_runtime_thumb_alu_imm_override = &on_immediate;
    g_ws_obj_x_provider = &on_obj_x;
    g_ws_obj_attr_y_provider = &on_obj_y;
    const char* w = std::getenv("KOL_WS_NO_WEATHER_REPEAT");
    g_weather_env_off = w && w[0] && w[0] != '0';
    g_ws_obj_margin_repeat_provider = &on_obj_repeat;
    g_ws_obj_repeat_skew_y = kWeatherSkew;
}

void obj_cull_set_weather_repeat(bool on) { g_weather_off = !on; }

void obj_attr_new_frame() { ++g_attr_epoch; }
std::uint64_t obj_attr_epoch() { return g_attr_epoch; }

void obj_cull_set_margins(unsigned extra_left, unsigned extra_right,
                          unsigned extra_top, unsigned extra_bottom) {
    if (g_disabled ||
        (extra_left | extra_right | extra_top | extra_bottom) == 0u) {
        g_widened = false;
        g_table_dirty = true;
        g_weather_dirty = true;
        g_view_left = g_view_right = 0;
        return;
    }

    // Horizontal. The emitter compares ((screen_x + slack) & 0xFFFF) against
    // limit_half << 1, so the window is [-slack, (limit_half * 2) - slack].
    // The left slack must cover a whole sprite (the stock 32) plus the margin,
    // or a unit one pixel inside the left edge is still dropped; the stock 32
    // of right slack is kept for the same reason on the other side. The
    // immediate reaches the generated code as a plain uint32_t, not an 8-bit
    // field, so neither value is capped at 255.
    g_table_dirty = true;
    g_weather_dirty = true;
    g_view_left = extra_left;
    g_view_right = extra_right;
    const unsigned s = g_extra_slack;
    const unsigned span = 304u + extra_left + extra_right + 2u * s;
    g_left_slack = kStockLeftSlack + extra_left + s;
    g_limit_half = (span + 1u) / 2u;   // round up: the limit is 2x this

    // Vertical. OAM's Y is only 8 bits and the hardware reading is exact
    // just for rows in [-96, 159], which a tall view exceeds -- at 480x270
    // sprites span 302 rows. That used to force a choice between the top and
    // bottom margins. It no longer does: on_obj_y above recovers the true row
    // from the guest's own slot list, so both bounds can follow the margins.
    g_top_slack    = kStockTopSlack + extra_top + s;
    g_bottom_limit = kStockBottomLimit + extra_bottom + s;
    g_widened = true;
    if (std::getenv("KOL_OBJY_STATS")) {
        static long frame = 0;  // reports the PREVIOUS frame's counters
        std::fprintf(stderr,
                     "[objy] frame %ld declined=%ld rebuilds=%ld entries=%d "
                     "worst_bucket=%d have=%d\n",
                     frame++, g_declined_high, g_rebuilds, g_table_entries,
                     g_worst_bucket, g_have_table ? 1 : 0);
        g_declined_high = 0; g_rebuilds = 0;
    }
    static unsigned last[4] = {0, 0, 0, 0};
    if (last[0] != g_left_slack || last[1] != g_limit_half ||
        last[2] != g_top_slack || last[3] != g_bottom_limit) {
        last[0] = g_left_slack; last[1] = g_limit_half;
        last[2] = g_top_slack;  last[3] = g_bottom_limit;
        std::fprintf(stderr,
                     "obj_cull: margins %u/%u/%u/%u -> accept X [%d,%d] "
                     "Y [%d,%d]\n", extra_left, extra_right, extra_top,
                     extra_bottom, -(int)g_left_slack,
                     (int)(2u * g_limit_half) - (int)g_left_slack,
                     -(int)g_top_slack, (int)g_bottom_limit);
        std::fflush(stderr);
    }
}

bool confirm_arrow_piece(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    const unsigned tile = attr2 & 0x3FFu;
    if (tile < 0x13Cu || tile > 0x14Au || ((attr2 >> 10) & 3u) != 0u) return false;
    const unsigned x = attr1 & 0x1FFu, y = attr0 & 0xFFu;
    return x >= 96u && x <= 136u && y >= 12u && y <= 28u;
}

// Some piece of it is up on every frame of a confirmation (BW, 0069: 150
// of 150 frames), though its head blinks.
bool confirm_arrow_up(int* bank) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const std::uint8_t* oam = bus->oam_ptr();
    for (int i = 0; i < 128; ++i) {
        const std::uint16_t a0 = static_cast<std::uint16_t>(oam[i * 8] | oam[i * 8 + 1] << 8);
        const std::uint16_t a1 = static_cast<std::uint16_t>(oam[i * 8 + 2] | oam[i * 8 + 3] << 8);
        const std::uint16_t a2 = static_cast<std::uint16_t>(oam[i * 8 + 4] | oam[i * 8 + 5] << 8);
        if ((a0 & 0x0300u) == 0x0200u) continue;   // not drawn
        if (!confirm_arrow_piece(a0, a1, a2)) continue;
        if (bank) *bank = a2 >> 12;
        return true;
    }
    return false;
}

// Read once a frame on the guest's thread (confirm_arrow_tick, from the
// frame hook), held 8 frames past the last piece seen: the second screen's
// thread read OAM itself and now and then caught it without the arrow, and
// the strip blinked off at no pattern on the Thor (the user saw it
// blink now and then, with no pattern).
std::atomic<int> g_confirm_bank{-1};
void confirm_arrow_tick(bool battle) {
    static int hold = 0, bank = -1;
    int b = -1;
    if (battle && confirm_arrow_up(&b)) { bank = b; hold = 8; }
    else if (hold > 0) --hold;
    else bank = -1;
    g_confirm_bank.store(bank, std::memory_order_relaxed);
}
int confirm_arrow_bank() { return g_confirm_bank.load(std::memory_order_relaxed); }

bool confirm_arrow_hidden_now() {
    static const bool keep = std::getenv("KOL_KEEP_CONFIRM_ARROW") != nullptr;
    static const bool test = std::getenv("KOL_CONFIRM_ARROW_TEST") != nullptr;
    return !keep && (second_screen_shown() || test) && is_battle_scene();
}

}  // namespace kol
