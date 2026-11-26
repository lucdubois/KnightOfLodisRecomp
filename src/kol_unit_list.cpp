// kol_unit_list.cpp — see the header.
//
// The units are the battle's map objects (u32 at 0x0202A238, 26 of stride
// 0xD0), each naming its record at +0x00. What the list reads from them:
//   object +0x10/+0x14  world x/z; the cell is >> 5, as the game's 0x08058D5C
//   battle entry +0x0C  the unit has acted this phase (the entry at
//                       0x02001AA0 + record index * 0x1C); counted only for
//                       the side whose turn it is: the acting unit's
//                       (0x0202A5F0), the player's while the cursor browses.
//                       It also blinks on for single frames (see below).
//   object +0x65        its OBJ palette bank
//   object +0x08        its actor; part 0 at +0x14: the current frame at
//                       +0xBC (ROM: u16 count, then attr0/1/2 per piece),
//                       the frame's tiles at +0x28 (OBJ VRAM), and at +0xC4
//                       the attr1 bits the actor ORs in (0x1000: flipped)
//   record +0x51        the game's NO.; +0x5D bit 7 another side's unit
// The game's own list screen draws its units from the same tiles. The map's
// END plate is painted into them too, but only while the game keeps the
// unit's sprite: its tiles reloaded (scrolled out and back) come without it,
// so the list paints the plate itself for a done unit, as the game does.
//
// Cell art (measured from the game's list, states/DE_battle_unit_list):
//   box      sheet tiles 33/34 (top and bottom rows) and 35/36, mirrored to
//            the right and below, bank 4 for the player's side, 7 otherwise
//   HP bar   archive 0x086BCA6C: 25 the cap (above, and flipped below), then
//            four segments from the bottom, 30 empty, 26/27/28 three, five
//            and seven pixels, 29 full; bank 12
//   LEADER   sheet tiles 242-245, bank 12 (either side's leader: battle
//            entry +0x17 bit 0)
//   field    sheet tiles 5-8, shifting by two a row, as the status sheet's

#include "kol_unit_list.h"
#include "kol_menu.h"
#include "kol_shop_buy.h"
#include "kol_a7.h"
#include "kol_actions.h"
#include "kol_focus.h"
#include "kol_unit_data.h"
#include "kol_sheet.h"

#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "gba_bus.h"
#include "gba_ppu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "kol_region.h"

namespace kol {
namespace {

constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::uint32_t kSheetArchive = usa(0x086B75B4u);
// The END plate: tiles 24-25 of this archive, 28-29 the same pre-mirrored.
constexpr std::uint32_t kPlateArchive = usa(0x086D80C4u);
constexpr std::uint32_t kBarArchive = usa(0x086BCA6Cu);
constexpr std::uint32_t kBgPalette = usa(0x0861DF68u);
constexpr std::uint32_t kObjectsPtr = usa(0x0202A238u);
constexpr std::uint32_t kActingObject = usa(0x0202A5F0u);
constexpr std::uint32_t kCursorMode = usa(0x03000FD5u);
constexpr std::uint32_t kMoveTaskState = usa(0x0202A5B0u) + 0x27Eu;
constexpr int kObjects = 26;
constexpr std::uint32_t kObjStride = 0xD0u;
constexpr int kBankPlayer = 4, kBankOther = 7, kBankBar = 12;
// Rows 44 apart (the game's list has 48): four rows, a full side of eight,
// fill the status sheet's 184 lines exactly, so the list takes the same
// window on the second screen as the sheet and the menu.
// A side is half the canvas (124 of 248), its two columns of cells centred.
constexpr int kCellW = 48, kCellH = 44, kTop = 8, kSideW = kListW / 2;
constexpr int kInset = (kSideW - 2 * kCellW) / 2;
static_assert(kListW == kSheetW, "the list and the sheet share the second screen's canvas");
constexpr int kWindowH = 184;

std::uint32_t bgr555(std::uint16_t c) {
    const std::uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3,
                        b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) | (b | (b >> 5));
}

struct Art {
    bool ok = false;
    std::vector<std::uint8_t> sheet, bars;   // 32 bytes a tile
    std::vector<std::uint8_t> plates;        // the END plate (kPlateArchive)
    std::uint32_t pal[16][16] = {};
};

bool load_archive(const std::uint8_t* rom, std::uint32_t size, std::uint32_t archive,
                  std::vector<std::uint8_t>* out) {
    const int n = a7_count(rom, size, archive);
    if (n <= 0) return false;
    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> e;
        if (!a7_entry(rom, size, archive, i, &e)) return false;
        out->insert(out->end(), e.begin(), e.end());
    }
    return true;
}

const Art& art() {
    static Art a;
    static bool tried = false;
    if (tried) return a;
    std::uint32_t size = 0;
    const std::uint8_t* rom = unit_data_rom(&size);
    if (!rom) return a;
    tried = true;
    if (!load_archive(rom, size, kSheetArchive, &a.sheet) || a.sheet.size() < 250u * 32u ||
        !load_archive(rom, size, kBarArchive, &a.bars) || a.bars.size() < 31u * 32u ||
        !load_archive(rom, size, kPlateArchive, &a.plates) || a.plates.size() < 30u * 32u)
        return a;
    const std::uint32_t p = kBgPalette - kRomBase;
    if (p + 512u > size) return a;
    for (int b = 0; b < 16; ++b)
        for (int i = 0; i < 16; ++i)
            a.pal[b][i] = bgr555(static_cast<std::uint16_t>(rom[p + (b * 16 + i) * 2] |
                                                            (rom[p + (b * 16 + i) * 2 + 1] << 8)));
    a.ok = true;
    return a;
}

std::uint32_t rd32(const std::uint8_t* m, std::uint32_t o) {
    return static_cast<std::uint32_t>(m[o]) | (m[o + 1] << 8) | (m[o + 2] << 16) |
           (static_cast<std::uint32_t>(m[o + 3]) << 24);
}
std::uint16_t rd16(const std::uint8_t* m, std::uint32_t o) {
    return static_cast<std::uint16_t>(m[o] | (m[o + 1] << 8));
}
bool in_ewram(std::uint32_t a, std::uint32_t len) {
    return a >= 0x02000000u && a + len <= 0x02040000u;
}

struct Unit {
    int object = 0;          // index in the map objects
    std::uint32_t obj = 0;   // its EWRAM offset
    int no = 0, hp = 0, hp_max = 0;
    bool other = false, done = false;
    bool leader = false;     // battle entry +0x17 bit 0, either side
};

struct Cell { int x, y, object; std::uint32_t record; };
std::mutex g_cells_mutex;
std::vector<Cell> g_cells;

// The world map's army list (see the header).
constexpr std::uint32_t kArmy = usa(0x02000030u);
constexpr int kArmySlots = 32;
constexpr std::uint32_t kRecordSize = 0x68u;
bool g_roster_mode = false;   // the last list composed was the army's

// The game's list cursor: everything but the lit unit darkened, a brightness
// decrease of 8/16 (BLDCNT 0x0FD3, BLDY 8) on 5-bit colours, as the PPU does.
std::uint32_t list_dim(std::uint32_t c) {
    std::uint32_t out = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        std::uint32_t v = ((c >> sh) & 0xFFu) >> 3;
        v -= (v * 8u) >> 4;
        out |= ((v << 3) | (v >> 2)) << sh;
    }
    return out;
}

// The rows at the picture's top that stay when the list scrolls (the spell's
// description in the shop). Read by the second screen after each compose.
std::atomic<int> g_list_pin{0};
std::atomic<int> g_list_faces{0};   // the army's size, for the animation (kept when it ends)

// The magic shop's tapped unit: stage 1 its slots, 2 the question; `press` the
// button a finger went down on, `inside` while it is still on it (pushed in), as
// StatsView keeps the bottom bar's. Under g_cells_mutex, with the faces as drawn.
struct SpellPick {
    std::uint32_t record = 0;
    int stage = 0, slot = 0, spell = 0, press = -1;
    bool inside = false;
};
SpellPick g_pick;
struct Face { std::uint32_t record; bool can; };
std::vector<Face> g_faces;

// A shop's list open (README "Spells on the shop's list", the user's pick B of
// shots/shop_compare/spell_description_options.png; item shops the same, switched to
// the spell portraits): the HELP card, or with a unit tapped its box, over the army
// as faces, five to a row, cut from the finished cards so they are dimmed alike.
// Always two whole rows (the user asked for 10 characters always visible),
// edge to edge; the 8 px on their right are the arrows' (StatsView). The box is a
// fixed size (as the user asked): the magic shop's 88, the tallest
// of any spell on sale, a third row peeking under the 216 lines; the item shops' 120
// (the user asked for a little more room for it), filling them. It stays when the
// rest scrolls. Returns the picture's height.
int compose_shop_faces(std::uint32_t* argb, int h, int spell, int item, int n, const std::uint32_t* face_record) {
    constexpr int kFace = 48, kFaceInset = 8, kPerRow = 5;
    static_assert(kSpellBoxH + 2 * kFace == kSheetH, "two rows of faces under the magic shop's box");
    const int slot = spell > 0 ? kSpellBoxH : kItemBoxH;
    static std::vector<std::uint32_t> full, card(kSheetW * kSheetH);
    full.assign(argb, argb + static_cast<std::size_t>(kListW) * h);
    UnitInfo u;
    const bool have_help = spell > 0 ? spell_help(spell, &u) : shop_item_help(item, &u);
    const int card_h = std::min(slot, have_help ? render_help_card(nullptr, 0, u, card.data(), slot) : 0);
    const int rows = (n + kPerRow - 1) / kPerRow;
    const int out_h = std::min(kListMaxH, std::max(slot + 2 * kFace, slot + rows * kFace));
    std::fill(argb, argb + static_cast<std::size_t>(kListW) * out_h, 0xFF000000u);
    for (int y = 0; y < card_h; ++y)
        std::copy_n(card.data() + static_cast<std::size_t>(y) * kSheetW, kSheetW, argb + static_cast<std::size_t>(y) * kListW);
    for (int k = 0; k < n; ++k) {
        const int sx = k % 2 * kArmyCellW + kFaceInset, sy = k / 2 * kArmyCellH + kFaceInset;
        const int dx = k % kPerRow * kFace, dy = slot + k / kPerRow * kFace;
        if (sy + kFace > h) break;
        for (int y = 0; y < kFace && dy + y < out_h; ++y)
            std::copy_n(full.data() + static_cast<std::size_t>(sy + y) * kListW + sx, kFace,
                        argb + static_cast<std::size_t>(dy + y) * kListW + dx);
    }
    SpellPick pick;
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        pick = g_pick;
    }
    if (pick.stage > 0) {
        UnitInfo w;
        if (unit_for_record(pick.record, &w) && w.valid) {
            const int pressed = pick.inside ? pick.press : -1;
            if (spell > 0) {
                render_spell_box(w, pick.record, spell, pick.stage, pick.slot, shop_spell_available(spell),
                                 shop_spell_free(), pressed, argb);
            } else {
                UnitInfo after;
                const bool have = pick.stage == 2 && unit_with_item_at(pick.record, item, pick.slot, &after) && after.valid;
                render_item_box(w, pick.record, item, pick.stage, pick.slot, shop_item_available(item), shop_spell_free(),
                                pressed, have ? &after : nullptr, argb);
            }
        }
        // Its face framed.
        for (int k = 0; k < n; ++k) {
            if (face_record[k] != pick.record) continue;
            const int dx = k % kPerRow * kFace, dy = slot + k / kPerRow * kFace;
            constexpr std::uint32_t kMark = 0xFFFFF070u;
            for (int t = 0; t < 2; ++t)
                for (int q = 0; q < kFace; ++q) {
                    argb[static_cast<std::size_t>(dy + t) * kListW + dx + q] = kMark;
                    argb[static_cast<std::size_t>(dy + kFace - 1 - t) * kListW + dx + q] = kMark;
                    argb[static_cast<std::size_t>(dy + q) * kListW + dx + t] = kMark;
                    argb[static_cast<std::size_t>(dy + q) * kListW + dx + kFace - 1 - t] = kMark;
                }
        }
    }
    g_list_pin.store(slot, std::memory_order_relaxed);
    g_list_faces.store(n, std::memory_order_relaxed);
    return out_h;
}

int compose_army_list(std::uint32_t* argb, const std::uint8_t* ew, bool shop = false) {
    // The picture is only redrawn when the army's records change: each card
    // decodes its portrait from the ROM archive, and the second screen asks
    // every tick.
    static std::vector<std::uint32_t> cached;
    static std::vector<Cell> cached_cells;
    static std::uint32_t cached_hash = 0;
    static int cached_h = 0, cached_pin = 0;
    // While Organize is open, the unit its grid or window is on is lit and
    // the rest dimmed, as the battle list marks the cursor's unit.
    // (Never in the shop: the flag behind it can outlive Organize there, and it
    // dimmed every card but the grid's first, states/thor/0122 on the Thor.)
    const std::uint32_t lit = shop ? 0u : action_roster_unit();
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t o = kArmy - 0x02000000u; o < kArmy - 0x02000000u + kArmySlots * kRecordSize; ++o)
        hash = (hash ^ ew[o]) * 16777619u;
    for (int sh = 0; sh < 32; sh += 8) hash = (hash ^ ((lit >> sh) & 0xFFu)) * 16777619u;
    // In the shop, the item under the Buy list's cursor (README "The army
    // list in the shop: who can equip it"): each card shows what it would
    // change, and the units that cannot wear it are dimmed.
    int item = shop ? shop_cursor_item() : -1;
    // A tab change moves the tab a frame before the game writes the tab's ids,
    // so a new spell counts when two reads agree (else the old tab's first
    // spell, its text and its dimming flashed for a frame).
    static int shown_spell = -1, pending_spell = -1;
    int spell = shop ? shop_cursor_spell() : -1;
    if (spell > 0 && shown_spell > 0 && spell != shown_spell && spell != pending_spell) {
        pending_spell = spell;
        spell = shown_spell;
    } else {
        pending_spell = -1;
        shown_spell = spell;
    }
    // (The same frame's lag for the item shops' description.)
    static int shown_item = -1, pending_item = -1;
    if (item > 0 && shown_item > 0 && item != shown_item && item != pending_item) {
        pending_item = item;
        item = shown_item;
    } else {
        pending_item = -1;
        shown_item = item;
    }
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        const int key = spell > 0 ? spell : item > 0 ? 0x1000 + item : -1;
        if (g_pick.stage > 0 && g_pick.spell != key) g_pick = SpellPick{};
        hash = (hash ^ static_cast<std::uint32_t>(g_pick.stage * 16 + g_pick.slot)) * 16777619u;
        hash = (hash ^ static_cast<std::uint32_t>(g_pick.inside ? g_pick.press + 1 : 0)) * 16777619u;
        hash = (hash ^ g_pick.record) * 16777619u;
    }
    if (spell > 0 || item > 0) hash = (hash ^ war_funds()) * 16777619u;
    const int cat = item_category(item);
    const bool compare = (cat >= 1 && cat <= 6) || spell > 0;
    hash = (hash ^ static_cast<std::uint32_t>(spell > 0 ? 0x100 + spell : item > 0 ? item + 1 : 0)) * 16777619u;
    if (cached_h > 0 && hash == cached_hash) {
        std::copy(cached.begin(), cached.end(), argb);
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        g_cells = cached_cells;
        g_roster_mode = true;
        g_list_pin.store(cached_pin, std::memory_order_relaxed);
        return cached_h;
    }
    static std::vector<UnitInfo> units;
    units.clear();
    std::vector<std::uint32_t> recs;
    for (int i = 0; i < kArmySlots; ++i) {
        const std::uint32_t rec = kArmy + static_cast<std::uint32_t>(i) * kRecordSize;
        const std::uint32_t r = rec - 0x02000000u;
        if (ew[r + 0x4Fu] == 0u) continue;   // an empty slot
        UnitInfo u;
        if (!unit_for_record(rec, &u) || !u.valid) continue;
        units.push_back(u);
        recs.push_back(rec);
    }
    if (units.empty()) return 0;
    // Who can have it: the others are dimmed.
    std::vector<bool> can;
    if (compare) {
        can.resize(units.size());
        for (std::size_t k = 0; k < units.size(); ++k)
            can[k] = spell > 0 ? unit_can_use_spell(recs[k], spell) && unit_has_spell_slot(recs[k])
                               : unit_can_equip(recs[k], item);
    }
    int h = render_army_list(units.data(), static_cast<int>(units.size()), argb, kListMaxH);
    if (h <= 0) return 0;
    std::vector<Cell> cells;
    for (std::size_t k = 0; k < recs.size(); ++k) {
        const int y = static_cast<int>(k / 2) * kArmyCellH;
        if (y + kArmyCellH > h) break;
        cells.push_back({static_cast<int>(k % 2) * kArmyCellW, y, -1, recs[k]});
    }
    // The units that cannot wear the item on sale, dimmed as the game's
    // lists dim what is not under the cursor.
    for (std::size_t k = 0; k < can.size() && k < cells.size(); ++k) {
        if (can[k]) continue;
        const Cell& c = cells[k];
        for (int y = c.y; y < c.y + kArmyCellH && y < h; ++y)
            for (int x = c.x; x < c.x + kArmyCellW && x < kListW; ++x)
                argb[y * kListW + x] = list_dim(argb[y * kListW + x]);
    }
    if (lit) {
        int lx = -1, ly = -1;
        for (const Cell& c : cells)
            if (c.record == lit) { lx = c.x; ly = c.y; }
        if (lx >= 0)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < kListW; ++x)
                    if (x < lx || x >= lx + kArmyCellW || y < ly || y >= ly + kArmyCellH)
                        argb[y * kListW + x] = list_dim(argb[y * kListW + x]);
    }
    if (spell > 0 || item > 0) {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        g_faces.clear();
        for (std::size_t k = 0; k < recs.size(); ++k) g_faces.push_back({recs[k], k < can.size() && can[k]});
    }
    if (spell > 0 || item > 0) h = compose_shop_faces(argb, h, spell, item, static_cast<int>(units.size()), recs.data());
    cached_pin = g_list_pin.load(std::memory_order_relaxed);
    cached.assign(argb, argb + static_cast<std::size_t>(kListW) * h);
    cached_cells = cells;
    cached_hash = hash;
    cached_h = h;
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        g_cells = std::move(cells);
        g_roster_mode = true;
    }
    return h;
}

class Picture {
public:
    Picture(std::uint32_t* px, int h) : px_(px), h_(h) {}
    void put(int x, int y, std::uint32_t c) {
        if (x >= 0 && x < kListW && y >= 0 && y < h_) px_[y * kListW + x] = c;
    }
    // One 8x8 4bpp tile from `tiles`, colour 0 clear.
    void tile(const std::vector<std::uint8_t>& tiles, int t, int x0, int y0,
              const std::uint32_t* pal, bool hf = false, bool vf = false) {
        if (t < 0 || static_cast<std::size_t>(t + 1) * 32u > tiles.size()) return;
        const std::uint8_t* d = tiles.data() + t * 32;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int sx = hf ? 7 - x : x, sy = vf ? 7 - y : y;
                const std::uint8_t b = d[sy * 4 + sx / 2];
                const int i = (sx & 1) ? (b >> 4) : (b & 0xF);
                if (i) put(x0 + x, y0 + y, pal[i]);
            }
    }

private:
    std::uint32_t* px_;
    int h_;
};

// The unit's current frame, as its actor puts it on the map: each piece of
// the frame record at its offset from (ox, oy), the unit's feet.
// The END plate's two tiles for a unit, the mirrored pair when it faces
// the other way (as the game picks them for its frames).
const std::uint8_t* end_plate(const Art& a, const std::uint8_t* ew, const Unit& u) {
    const std::uint32_t actor = rd32(ew, u.obj + 0x08u);
    if (!in_ewram(actor, 0x14u + 0xECu)) return nullptr;
    const bool flip = (rd16(ew, actor - 0x02000000u + 0x14u + 0xC4u) & 0x1000u) != 0u;
    return a.plates.data() + (flip ? 28 : 24) * 32;
}

void draw_sprite(Picture& p, const std::uint8_t* ew, const std::uint8_t* rom,
                 std::uint32_t rom_size, const std::uint8_t* tiles,
                 const std::uint8_t* obj_pal, const Unit& u, int ox, int oy,
                 const std::uint8_t* plate = nullptr) {
    const std::uint32_t actor = rd32(ew, u.obj + 0x08u);
    if (!in_ewram(actor, 0x14u + 0xECu)) return;
    const std::uint32_t part = actor - 0x02000000u + 0x14u;
    const std::uint32_t frame = rd32(ew, part + 0xBCu);
    const std::uint32_t vram = rd32(ew, part + 0x28u);
    if (frame < kRomBase || frame - kRomBase + 2u > rom_size) return;
    if (vram < 0x06010000u || vram >= 0x06018000u) return;
    const bool flip = (rd16(ew, part + 0xC4u) & 0x1000u) != 0u;
    const int base = static_cast<int>((vram - 0x06010000u) / 32u);
    const int bank = ew[u.obj + 0x65u] & 0xF;
    std::uint32_t pal[16];
    for (int i = 0; i < 16; ++i) pal[i] = bgr555(rd16(obj_pal, (bank * 16 + i) * 2));
    const std::uint32_t f = frame - kRomBase;
    const int n = rd16(rom, f);
    if (n > 16 || f + 2u + n * 6u > rom_size) return;
    static const int kDims[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    for (int k = 0; k < n; ++k) {
        const std::uint16_t a0 = rd16(rom, f + 2u + k * 6u);
        const std::uint16_t a1 = rd16(rom, f + 4u + k * 6u);
        const std::uint16_t a2 = rd16(rom, f + 6u + k * 6u);
        const int shape = a0 >> 14;
        if (shape > 2) continue;
        const int w = kDims[shape][a1 >> 14][0], h = kDims[shape][a1 >> 14][1];
        int dx = a1 & 0x1FF;
        if (dx >= 256) dx -= 512;
        int dy = a0 & 0xFF;
        if (dy >= 128) dy -= 256;
        const bool hf = ((a1 & 0x1000u) != 0u) != flip;
        if (flip) dx = -dx - w;
        const int t0 = base + (a2 & 0x3FF);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = hf ? w - 1 - x : x;
                const int t = t0 + (y / 8) * (w / 8) + sx / 8;
                if (t < 0 || t >= 1024) continue;
                // A done unit's END plate, over rows 1-7 of its first two
                // tiles as the game paints it (II: Alphonse's tiles hold it
                // byte for byte, transparent pixels too).
                const bool in_plate = plate && (t == base || t == base + 1) && (y % 8) >= 1;
                const std::uint8_t* src = in_plate ? plate + (t - base) * 32 : tiles + t * 32;
                const std::uint8_t b = src[(y % 8) * 4 + (sx % 8) / 2];
                const int i = (sx & 1) ? (b >> 4) : (b & 0xF);
                if (i) p.put(ox + dx + x, oy + dy + y, pal[i]);
            }
    }
}

// The HP bar's four segments, from the bottom: how many of each one's eight
// pixels are filled, as the bar tiles have them (0, 3, 5, 7, 8).
int bar_tile(int filled) {
    static const int kTile[9] = {30, 26, 26, 26, 27, 27, 28, 28, 29};
    return kTile[std::max(0, std::min(8, filled))];
}

}  // namespace

bool world_map_scene() {
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint8_t* io = ppu ? ppu->latched_io() : nullptr;   // the presented frame's
    if (!io) return false;
    return rd16(io, 0x00) == 0x9F40u && rd16(io, 0x08) == 0x2F03u && rd16(io, 0x0A) == 0x2E02u &&
           rd16(io, 0x0C) == 0x2D08u && rd16(io, 0x0E) == 0x2C09u;
}

// The shop (Buy / Sell / Hire; states/thor/0018): mode 0 on its four
// layers, whose BGxCNTs every screen of it keeps (the menu, Item /
// Spellbook, the shopkeeper's dialogue, the lists). The army list shows
// there too (asked 2026-10-29 for the unit list on the bottom
// screen while shopping), to look at only: a tap there would drive the
// world map's menus into the shop's (unit_list_tap).
bool shop_scene() {
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint8_t* io = ppu ? ppu->latched_io() : nullptr;   // the presented frame's
    if (!io) return false;
    return (rd16(io, 0x00) & 7u) == 0u && rd16(io, 0x08) == 0x2E03u && rd16(io, 0x0A) == 0x2D0Au &&
           rd16(io, 0x0C) == 0x2B08u && rd16(io, 0x0E) == 0x2A09u;
}
std::atomic<bool> g_shop_list{false};   // the list composed last was the army's, in the shop

// The cursor has been under the pad (mode 1) since it was last put away
// (mode 0); kept by unit_list_dump_tick, every real frame.
std::atomic<bool> g_browsed{false};

int compose_unit_list(std::uint32_t* argb, std::uint32_t* on_unit) {
    if (on_unit) *on_unit = 0;
    g_list_pin.store(0, std::memory_order_relaxed);
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!a.ok || !rom || !bus || !argb) return 0;
    const std::uint8_t* ew = bus->ewram_ptr();
    if (!ew) return 0;
    // The army stays while a unit's status window opened from it is up.
    const bool shop = shop_scene();
    g_shop_list.store(shop, std::memory_order_relaxed);
    if (world_map_scene() || action_profile_busy() || shop) return compose_army_list(argb, ew, shop);
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        g_roster_mode = false;
    }
    // Only while the player browses the map: the cursor out (IWRAM 0x03000FD5,
    // cursor +0x35: 1 under the pad, 4 while it glides to a unit picked here)
    // and either no unit taking its action or the turn's cursor only looking
    // around (the move task at 0x05, as kol::cursor_browsing: DF); not while
    // preparing the battle.
    const std::uint8_t* iw = bus->iwram_ptr();
    if (!iw) return 0;
    const std::uint8_t mode = iw[kCursorMode - 0x03000000u];
    if (battle_preparation() || story_event() || (mode != 1u && mode != 4u)) return 0;
    if (unit_in_action() && ew[kMoveTaskState - 0x02000000u] != 0x05u) return 0;
    // A glide (4) only carries on a browse: the game glides on its own too,
    // with no 1 before it, as the camera goes to each enemy on its turn
    // (BL_enemy_turn: 672, 854, 948, 1182 ...) and to the player's first
    // unit as the turn comes back (GC: 2744, 2760-2776, 2780, 2919), and the
    // list flashed up over the sheet each time (the user, 10-12).
    if (mode == 4u && !g_browsed.load(std::memory_order_relaxed)) return 0;
    const std::uint32_t list = rd32(ew, kObjectsPtr - 0x02000000u);
    if (!in_ewram(list, kObjects * kObjStride)) return 0;

    // Sprites: the frame the screen shows if the presenter keeps it, else the
    // tiles as they stand (a list does not mind a frame's lead).
    const std::uint8_t* tiles = nullptr;
    const std::uint8_t* obj_pal = nullptr;
    if (gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        tiles = ppu->latched_obj_tiles();
        obj_pal = ppu->latched_pal() ? ppu->latched_pal() + 0x200 : nullptr;
    }
    if (!tiles || !obj_pal) {
        tiles = bus->vram_ptr() ? bus->vram_ptr() + 0x10000 : nullptr;
        obj_pal = bus->pal_ptr() ? bus->pal_ptr() + 0x200 : nullptr;
    }
    if (!tiles || !obj_pal) return 0;

    bool turn_other = false;   // whose turn: the acting unit's side
    const std::uint32_t acting = rd32(ew, kActingObject - 0x02000000u);
    if (in_ewram(acting, 4)) {
        const std::uint32_t rec = rd32(ew, acting - 0x02000000u);
        if (in_ewram(rec, 0x68)) turn_other = (ew[rec - 0x02000000u + 0x5Du] & 0x80u) != 0u;
    }
    int cx = -1, cz = -1;
    const bool cursor = mode == 1u && map_cursor_cell(&cx, &cz);   // not mid-glide
    std::vector<Unit> sides[2];
    for (int i = 0; i < kObjects; ++i) {
        const std::uint32_t o = list - 0x02000000u + i * kObjStride;
        const std::uint32_t rec = rd32(ew, o);
        if (!in_ewram(rec, 0x68)) continue;
        const std::uint32_t r = rec - 0x02000000u;
        Unit u;
        u.object = i;
        u.obj = o;
        u.hp = rd16(ew, r + 0x40u);
        u.hp_max = rd16(ew, r + 0x42u);
        if (ew[r + 0x4Eu] == 0u || u.hp == 0 || u.hp_max == 0) continue;   // not on the map
        u.no = ew[r + 0x51u];
        u.other = (ew[r + 0x5Du] & 0x80u) != 0u;
        // Done: the battle entry's +0x0C (II, the user's slot 2: 1 on
        // Alphonse only, as in DG on Euphaire, DH on both, CN on Flauros and
        // Deneb, HX on Rictor in his army's phase). Object +0x78 bit 1, read
        // before, is the sprite pool's: the VRAM callback 0x08059000 sets it
        // as it (re)loads a unit's frames, so every unit the camera scrolled
        // out and back read as done (II, Left a few times) and the order
        // changed; it also blinked on one unit at a time (DG).
        const std::uint32_t ri = (rec - usa(0x02001420u)) / 0x68u;
        const bool in_battle = rec >= usa(0x02001420u) && (rec - usa(0x02001420u)) % 0x68u == 0u && ri < 16u;
        u.done = in_battle && ew[usa(0x02001AA0u) - 0x02000000u + ri * 0x1Cu + 0x0Cu] != 0u && u.other == turn_other;
        // The leader: the battle entry's +0x17 bit 0 (the status window's
        // "leader" No. help, 503), set on the player's NO. 1 (0x41) and on
        // the enemy's leader (0xC1: map 7's and map 2's NO. 1); none on a
        // Training or random battle's enemies (BL, FK). Bit 1 is a guest.
        u.leader = in_battle ? (ew[usa(0x02001AA0u) - 0x02000000u + ri * 0x1Cu + 0x17u] & 1u) != 0u
                             : !u.other && u.no == 1;
        if (cursor && on_unit && (rd16(ew, o + 0x10u) >> 5) == cx && (rd16(ew, o + 0x14u) >> 5) == cz)
            *on_unit = rec;
        sides[u.other ? 1 : 0].push_back(u);
    }
    for (auto& s : sides)
        std::stable_sort(s.begin(), s.end(), [](const Unit& x, const Unit& y) {
            return x.done != y.done ? !x.done : x.no < y.no;
        });
    const int rows = static_cast<int>(std::max((sides[0].size() + 1) / 2, (sides[1].size() + 1) / 2));
    const int h = std::min(kListMaxH, std::max(kWindowH, kTop + rows * kCellH));

    Picture p(argb, h);
    for (int ty = 0; ty < (h + 7) / 8; ++ty)
        for (int tx = 0; tx < kListW / 8; ++tx)
            p.tile(a.sheet, 5 + (tx + 2 * ty) % 4, tx * 8, ty * 8, a.pal[kBankPlayer]);

    std::vector<Cell> cells;
    for (int side = 0; side < 2; ++side) {
        const std::uint32_t* box = a.pal[side ? kBankOther : kBankPlayer];
        for (std::size_t k = 0; k < sides[side].size(); ++k) {
            const Unit& u = sides[side][k];
            const int x = side * kSideW + kInset + static_cast<int>(k % 2) * kCellW;
            const int y = kTop + static_cast<int>(k / 2) * kCellH;
            if (y + 40 > h) break;
            const int bx = x + 8;   // the box; the bar is the column left of it
            for (int ry = 0; ry < 4; ++ry)
                for (int rx = 0; rx < 4; ++rx)
                    p.tile(a.sheet, 33 + (rx == 1 || rx == 2 ? 1 : 0) + (ry == 1 || ry == 2 ? 2 : 0),
                           bx + rx * 8, y + ry * 8, box, rx == 3, ry == 3);
            const int filled = (u.hp * 32 + u.hp_max - 1) / u.hp_max;
            for (int s = 0; s < 4; ++s)
                p.tile(a.bars, bar_tile(filled - (3 - s) * 8), x, y + s * 8, a.pal[kBankBar]);
            p.tile(a.bars, 25, x, y, a.pal[kBankBar]);
            p.tile(a.bars, 25, x, y + 24, a.pal[kBankBar], false, true);
            draw_sprite(p, ew, rom, rom_size, tiles, obj_pal, u, bx + 16, y + 32,
                        u.done ? end_plate(a, ew, u) : nullptr);
            if (u.leader)
                for (int j = 0; j < 4; ++j)
                    p.tile(a.sheet, 242 + j, bx + j * 8, y + 33, a.pal[kBankBar]);
            cells.push_back({x, y, u.object, rd32(ew, u.obj)});
        }
    }
    // The unit on the cursor's cell lit, the rest dimmed, as the game's list
    // marks its cursor: brightness decrease 8/16 (BLDCNT 0x0FD3, BLDY 8) on
    // everything but a window from 2 px left of the bar to 1 px right of the
    // box, 2 px above the box to 1 px under the label row, corners cut (WIN1
    // plus the OBJ-window strips 0x3F5/0x3F6: DE). The card lies over the
    // list while the cursor is on a unit, so this shows once it is put away.
    if (on_unit && *on_unit) {
        int lx = -1, ly = -1;
        for (const Cell& c : cells)
            if (c.record == *on_unit) { lx = c.x - 2; ly = c.y - 2; }
        if (lx >= -2) {
            const int rx = lx + 42, by = ly + 42;   // inclusive
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < kListW; ++x) {
                    const bool in = x >= lx && x <= rx && y >= ly && y <= by &&
                                    !((x == lx || x == rx) && (y == ly || y == by));
                    if (!in) argb[y * kListW + x] = list_dim(argb[y * kListW + x]);
                }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        g_cells = std::move(cells);
    }
    return h;
}

// The live button at a place in the magic shop's box, -1 for none (g_cells_mutex held).
int spell_button_at(const SpellPick& p, float x, float y, int spell) {
    const int item = spell > 0 ? -1 : shop_cursor_item();
    if (p.stage == 1) {
        if (spell > 0 ? !shop_spell_available(spell) : !shop_item_available(item)) return -1;
        for (int s = 0; s < 4; ++s) {
            const int bx = kSpellSlotX[s % 2], by = spell > 0 ? kSpellSlotY[s / 2] : kItemSlotY[s / 2];
            if (x >= bx && x < bx + kSpellSlotW && y >= by && y < by + kSpellSlotH)
                return (spell > 0 ? spell_slot_takes(p.record, s, spell) : item_slot_takes(p.record, s, item)) ? s : -1;
        }
    } else if (p.stage == 2 && y >= (spell > 0 ? kSpellButtonY : kItemButtonY) &&
               y < (spell > 0 ? kSpellButtonY : kItemButtonY) + kSpellButtonH) {
        if (x >= kSpellYesX && x < kSpellYesX + kSpellButtonW) return kSpellYes;
        if (x >= kSpellNoX && x < kSpellNoX + kSpellButtonW) return kSpellNo;
    }
    return -1;
}

// A button let go on: each with the game's sound, as the bottom bar's.
void spell_button_acts(SpellPick& p, int button, int spell) {
    if (button == kSpellYes) {
        menu_confirm_sound();
        if (spell > 0) shop_buy_request(p.record, p.slot, spell, false);
        else shop_buy_request(p.record, p.slot, shop_cursor_item(), true);
        p = SpellPick{};
    } else if (button == kSpellNo) {
        menu_cancel_sound();
        p.stage = 1;
    } else if (button >= 0) {
        menu_confirm_sound();
        p.stage = 2;
        p.slot = button;
    }
}

// A tap in the magic shop's picture, g_cells_mutex held, on what is no button:
// a face picks its unit (or lets go), beside the slots lets go.
void spell_tap(float x, float y, int spell) {
    constexpr int kFace = 48, kPerRow = 5;
    SpellPick& p = g_pick;
    const int box_h = spell > 0 ? kSpellBoxH : kItemBoxH;
    if (y >= box_h) {
        const int col = static_cast<int>(x) / kFace, row = (static_cast<int>(y) - box_h) / kFace;
        const int k = row * kPerRow + col;
        const bool on = col >= 0 && col < kPerRow && k >= 0 && k < static_cast<int>(g_faces.size());
        if (on && g_faces[static_cast<std::size_t>(k)].can && !(p.stage > 0 && p.record == g_faces[static_cast<std::size_t>(k)].record)) {
            p = SpellPick{};
            p.record = g_faces[static_cast<std::size_t>(k)].record;
            p.stage = 1;
            p.spell = spell > 0 ? spell : 0x1000 + shop_cursor_item();
        } else {
            p = SpellPick{};
        }
        return;
    }
    const int b = spell_button_at(p, x, y, spell);
    if (b >= 0) {
        spell_button_acts(p, b, spell);   // (a headless tap: down and up at once)
        return;
    }
    if (p.stage == 1) {
        bool on_slot = false;   // a greyed one takes no press, and lets nothing go
        for (int s = 0; s < 4; ++s)
            on_slot = on_slot || (x >= kSpellSlotX[s % 2] && x < kSpellSlotX[s % 2] + kSpellSlotW &&
                                  y >= (spell > 0 ? kSpellSlotY[s / 2] : kItemSlotY[s / 2]) &&
                                  y < (spell > 0 ? kSpellSlotY[s / 2] : kItemSlotY[s / 2]) + kSpellSlotH);
        if (!on_slot) p = SpellPick{};
    }
}

// The finger on the magic shop's buttons, phase 0 down, 1 moved, 2 up, 3 cancelled,
// as StatsView handles the bottom bar's. Returns whether a button holds the touch.
int unit_list_press(float x, float y, int phase) {
    if (!g_shop_list.load(std::memory_order_relaxed)) return 0;
    const int spell = shop_cursor_spell();
    std::lock_guard<std::mutex> lock(g_cells_mutex);
    SpellPick& p = g_pick;
    if (phase == 0) {
        const int b = spell > 0 || shop_cursor_item() > 0 ? spell_button_at(p, x, y, spell) : -1;
        p.press = b;
        p.inside = b >= 0;
        return b >= 0;
    }
    if (p.press < 0) return 0;
    const bool on = spell_button_at(p, x, y, spell) == p.press;
    if (phase == 1) {
        p.inside = on;
    } else {
        const int b = p.press;
        p.press = -1;
        p.inside = false;
        if (phase == 2 && on) spell_button_acts(p, b, spell);
    }
    return 1;
}

int unit_list_pin() { return g_list_pin.load(std::memory_order_relaxed); }
bool unit_list_in_shop() { return g_shop_list.load(std::memory_order_relaxed); }
int unit_list_faces() { return g_list_faces.load(std::memory_order_relaxed); }

std::uint32_t unit_list_tap(float x, float y) {
    int object = -1;
    std::uint32_t record = 0;
    {
        std::lock_guard<std::mutex> lock(g_cells_mutex);
        if (g_roster_mode && g_shop_list.load(std::memory_order_relaxed)) {
            // The shop's is to look at, but for the magic shop's faces.
            const int spell = shop_cursor_spell();
            if (spell > 0 || shop_cursor_item() > 0) spell_tap(x, y, spell);
            return 0;
        }
        if (g_roster_mode) {   // the army: the game's status window, on that unit
            for (const Cell& c : g_cells)
                if (x >= c.x && x < c.x + kArmyCellW && y >= c.y && y < c.y + kArmyCellH) record = c.record;
            if (record) action_open_profile(record);
            return record;
        }
        for (const Cell& c : g_cells)
            if (x >= c.x - 2 && x < c.x + 42 && y >= c.y - 2 && y < c.y + 42) {
                object = c.object;
                record = c.record;
            }
    }
    if (object < 0) return 0;
    action_cursor_to_object(object);
    return record;
}

void unit_list_dump_tick(unsigned frame) {
    if (!gbarecomp::runtime_excursion_active()) shop_list_frame();
    if (!gbarecomp::runtime_excursion_active())
        if (gba::GbaBus* bus = gbarecomp::active_bus())
            if (const std::uint8_t* iw = bus->iwram_ptr()) {
                const std::uint8_t mode = iw[kCursorMode - 0x03000000u];
                if (mode == 1u) g_browsed.store(true, std::memory_order_relaxed);
                else if (mode != 4u) g_browsed.store(false, std::memory_order_relaxed);
            }
    static const char* path = std::getenv("KOL_LIST_DUMP");
    static const char* taps = std::getenv("KOL_LIST_TAP");
    if (!path && !taps) return;
    static std::vector<std::uint32_t> px(kListW * kListMaxH), last;
    std::uint32_t on_unit = 0;
    const int h = compose_unit_list(px.data(), &on_unit);
    if (std::getenv("KOL_LIST_TRACE")) {
        gba::GbaBus* bus = gbarecomp::active_bus();
        const std::uint8_t* iw = bus ? bus->iwram_ptr() : nullptr;
        int sx = -1, sz = -1;
        map_cursor_cell(&sx, &sz);
        if (iw)
            std::printf("unit_list: trace frame=%u on=%08X mode=%u target=(%u,%u) slot=(%d,%d) hl=%08X\n",
                        frame, on_unit, iw[usa(0x03000FD5u) - 0x03000000u], rd16(iw, usa(0x03000FDCu) - 0x03000000u),
                        rd16(iw, usa(0x03000FE0u) - 0x03000000u), sx, sz,
                        rd32(bus->ewram_ptr(), usa(0x02028960u) - 0x02000000u));
    }
    static int last_h = -1;
    static std::uint32_t last_on = ~0u;
    if (h != last_h || on_unit != last_on) {
        std::printf("unit_list: frame=%u height=%d on_unit=%08X\n", frame, h, on_unit);
        last_h = h;
        last_on = on_unit;
    }
    {   // the order, whenever it changes
        std::string order;
        {
            std::lock_guard<std::mutex> lock(g_cells_mutex);
            for (const Cell& c : g_cells) order += std::to_string(c.object) + " ";
        }
        static std::string last_order;
        if (order != last_order) {
            std::printf("unit_list: frame=%u order %s\n", frame, order.c_str());
            last_order = order;
        }
    }
    static unsigned first = ~0u;   // taps count from the list's first frame
    static const bool from_start = std::getenv("KOL_TAP_FROM_START") != nullptr;   // or the run's
    static unsigned last_frame = 0;
    if (from_start && frame - last_frame > 1000u) first = frame;   // a state load: start again
    last_frame = frame;
    if ((h > 0 || from_start) && first == ~0u) first = frame;
    // Each tap fires once, at the first frame at or past its own: in menus a
    // present can carry several frames, and an exact match was skipped.
    static unsigned fired = 0;   // bit k: tap k done
    int k = -1;
    for (const char* t = first == ~0u ? nullptr : taps; t && *t;) {
        ++k;
        unsigned f = 0;
        float x = 0, y = 0;
        char what[12] = {};
        const bool due = k < 32 && !(fired & (1u << k));
        if (due && std::sscanf(t, "%u:%f:%f", &f, &x, &y) == 3 && f <= frame - first) {
            fired |= 1u << k;
            std::printf("unit_list: frame=%u tap (%.0f,%.0f) -> %08X\n", frame, x, y,
                        unit_list_tap(x, y));
        } else if (due && std::sscanf(t, "%u:%11[a-z]", &f, what) == 2 && f <= frame - first) {
            fired |= 1u << k;
            // "f:save", "f:load", "f:item", "f:spell", "f:class", "f:training",
            // "f:shop": the bar's world-map buttons.
            std::printf("unit_list: frame=%u %s\n", frame, what);
            if (std::strcmp(what, "save") == 0) action_world_save();
            if (std::strcmp(what, "load") == 0) action_world_load();
            if (std::strcmp(what, "item") == 0) action_world_item(0);
            if (std::strcmp(what, "spell") == 0) action_world_item(1);
            if (std::strcmp(what, "class") == 0) action_world_item(2);
            if (std::strcmp(what, "training") == 0) action_world_item(3);
            if (std::strcmp(what, "shop") == 0) action_world_item(4);
        }
        t = std::strchr(t, ',');
        if (t) ++t;
    }
    if (!path || h <= 0) return;
    const std::vector<std::uint32_t> now(px.begin(), px.begin() + kListW * h);
    if (now == last) return;
    last = now;
    char name[512];
    std::snprintf(name, sizeof name, path, frame);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", kListW, h);
        for (const std::uint32_t c : now) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16),
                                          static_cast<unsigned char>(c >> 8),
                                          static_cast<unsigned char>(c)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
}

}  // namespace kol
