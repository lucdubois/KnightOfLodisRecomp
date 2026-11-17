// kol_sheet.cpp — see the header.

#include "kol_sheet.h"
#include "kol_a7.h"

#include "gba_bus.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "kol_menu.h"              // menu_encode, the bar buttons' frame
#include "kol_obj_cull.h"           // confirm_arrow_up

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace kol {
namespace {

constexpr std::uint32_t kRomBase = 0x08000000u;

// The sheet's char block, as the game assembles it for the status window
// (measured: every static tile of the open window matches these byte for
// byte). Tile t of an archive's output lands at `first_tile + t`.
struct Load { std::uint32_t archive; int first_tile; };
constexpr Load kLoads[] = {
    {0x086B75B4u, 0},     // the sheet tileset: labels, digits, icons, bars
    {0x086FCD38u, 9},     // the frame's corners and edges
    {0x086BC3D8u, 250},   // talent icons
    {0x086BD8C0u, 314},   // STR:/INT:/AGI:, Move and more labels
};
constexpr int kCharTiles = 1024;

constexpr std::uint32_t kBgPalette = 0x0861DF68u;   // BG banks 0-15
constexpr std::uint32_t kGlyphs = 0x0815DEC4u;      // font: 64 bytes per glyph
constexpr std::uint32_t kGlyphWidths = 0x0815DE6Cu; // advance per code
constexpr std::uint8_t kSpaceCode = 0x89u;
constexpr int kSpaceWidth = 4;
constexpr std::uint8_t kDashCode = 0x4Cu;   // '-' in the font
constexpr std::uint32_t kIconBaseTable = 0x0860DD2Eu;   // element icon tiles
constexpr std::uint32_t kTileBanks = 0x086B7534u;       // palette nibble/tile
// Item pictures: raw 16x16 4bpp sprites, 128 bytes each (tiles TL TR BL BR),
// indexed by picture (item field 30) after a 0x1A0-byte header. From the DMA
// at 0x0802BC08: 0x08724208 + u16[0x08724208] + picture * 128. The sprite
// palette is the high nibble of header byte 4 + picture * 2 (0x08040D42).
constexpr std::uint32_t kItemIconTable = 0x08724208u;
constexpr std::uint32_t kObjPalette = 0x0861E288u;   // OBJ banks 0-15
constexpr int kMaxRomItem = 0xC7;
// The support/fear gauge: five six-tile bars (+2 +1 0 -1 -2) from tile 0x13A,
// each tile's bank a nibble in its own table (as 0x0803E6D8 reads it).
constexpr int kGaugeTile = 0x13A;
// Portraits: 48x48 layers of 36 tiles, 18 archive entries each, in the A7
// archive at 0x086C052C; palettes of 16 colours at 0x0861EA28 (0x0801C1BC).
constexpr std::uint32_t kPortraitArchive = 0x086C052Cu;
constexpr std::uint32_t kPortraitPalettes = 0x0861EA28u;
// The dialog's portraits (loader 0x0801B8xx): the same layer numbers and palettes, 15
// entries (30 tiles) a layer in this archive, 122 layers where the status window's has
// 98. A layer is 40x48 in the dialog's sprite pieces: 32x32 (16 tiles), the 8x32 column
// right of it (4), 32x16 under it (8), 8x16 (2).
constexpr std::uint32_t kDialogPortraitArchive = 0x086E3F3Cu;
constexpr std::uint32_t kGaugeBanks = 0x086BD888u + 4u;
// Talent icons: tiles from 0xFA (the talent archive above), each tile's bank
// a nibble in the table in front of that archive, as 0x0803EF44 reads it.
constexpr int kTalentTile = 0x0FA;
constexpr std::uint32_t kTalentBanks = 0x086BC3B0u + 4u;
// Move icons (0x0803BDC0): land (10 icons) and water (5), each two entries of
// its own A7 archive, each row's bank a nibble in the table in front of it.
constexpr std::uint32_t kLandIcons = 0x086BF004u;
constexpr std::uint32_t kLandBanks = 0x086BEFE8u + 4u;
constexpr std::uint32_t kWaterIcons = 0x086BED28u;
constexpr std::uint32_t kWaterBanks = 0x086BED18u + 4u;
// Emblem pictures: 35 of 16x16 in their own A7 archive, two entries each, each
// tile's bank a nibble in the table in front of it (0x0803C0C6, 0x0803BA8E).
constexpr std::uint32_t kEmblemArchive = 0x086D6B7Cu;
constexpr std::uint32_t kEmblemBanks = 0x086D6B30u + 4u;
// The speed's decimal point, a sprite the window places over its last two
// digits (OAM tile 0x2FC, OBJ bank 1, measured in states/AY_kamui_talents).
// Its source in the ROM was not found (in no A7 archive), so the 3x3 dot is
// written out here: 4bpp rows, colours 4 1 4 / 1 3 1 / 4 1 4.
constexpr std::uint8_t kSpeedDot[32] = {
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    0x40, 0x41, 0, 0,  0x10, 0x13, 0, 0,  0x40, 0x41, 0, 0,  0, 0, 0, 0};

// Palette banks the window uses for each kind of tile (from its tilemaps).
constexpr int kInk = 4;        // digits and text, on either team's sheet
constexpr int kFurniture = 4;  // frame, parchment and boxes, player's side
constexpr int kFurnitureOther = 5;   // the same, green, for any other side
constexpr int kPortraitOther = 7;    // the portrait frame on another side's
constexpr int kLabel = 12;     // Lv. EXP HP: STR: icons, plates
constexpr int kLabel2 = 13;    // MP: and the MP bar's fill

struct Art {
    bool ok = false;
    std::vector<std::uint8_t> chars;   // kCharTiles * 32
    std::uint32_t pal[16][16] = {};
    std::uint32_t obj_pal[16][16] = {};   // sprite palettes, for item pictures
};

std::uint32_t bgr555(std::uint16_t c) {
    const std::uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3,
                        b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) |
           (b | (b >> 5));
}

const Art& art() {
    static Art a;
    static bool tried = false;
    if (tried) return a;
    std::uint32_t size = 0;
    const std::uint8_t* rom = unit_data_rom(&size);
    if (!rom) return a;   // try again once the ROM has been handed over
    tried = true;
    a.chars.assign(kCharTiles * 32, 0);
    for (const Load& l : kLoads) {
        const int n = a7_count(rom, size, l.archive);
        if (n <= 0) return a;
        std::size_t at = static_cast<std::size_t>(l.first_tile) * 32u;
        for (int i = 0; i < n; ++i) {
            std::vector<std::uint8_t> e;
            if (!a7_entry(rom, size, l.archive, i, &e)) return a;
            if (at + e.size() > a.chars.size()) return a;
            std::memcpy(&a.chars[at], e.data(), e.size());
            at += e.size();
        }
    }
    const std::uint32_t pal = kBgPalette - kRomBase;
    if (pal + 512u > size) return a;
    for (int b = 0; b < 16; ++b)
        for (int i = 0; i < 16; ++i)
            a.pal[b][i] = bgr555(static_cast<std::uint16_t>(
                rom[pal + (b * 16 + i) * 2] | (rom[pal + (b * 16 + i) * 2 + 1] << 8)));
    const std::uint32_t op = kObjPalette - kRomBase;
    if (op + 512u > size) return a;
    for (int b = 0; b < 16; ++b)
        for (int i = 0; i < 16; ++i)
            a.obj_pal[b][i] = bgr555(static_cast<std::uint16_t>(
                rom[op + (b * 16 + i) * 2] | (rom[op + (b * 16 + i) * 2 + 1] << 8)));
    a.ok = true;
    return a;
}

class Canvas {
public:
    Canvas(const Art& a, const std::uint8_t* rom, std::uint32_t rom_size,
           std::uint32_t* px, int h = kSheetH)
        : a_(a), rom_(rom), rom_size_(rom_size), px_(px), h_(h) {}

    // Everything drawn after this lands `y` pixels lower: a second unit card
    // under the first (render_unit_sheet's target).
    void set_origin_y(int y) { oy_ = y; }
    // And `x` pixels to the right: a card's content at its own left edge
    // (the army list's cards, 124 px apart).
    void set_origin_x(int x) { ox_ = x; }
    // The pixel at (x, y) as drawn (origin applied), or null off the canvas.
    std::uint32_t* pixel(int x, int y) {
        const int X = x + ox_, Y = y + oy_;
        return X >= 0 && X < kSheetW && Y >= 0 && Y < h_ ? &px_[Y * kSheetW + X] : nullptr;
    }

    void tile(int col, int row, int t, int bank, bool hflip = false, bool vflip = false) {
        tile_px(col * 8, row * 8, t, bank, hflip, vflip);
    }
    // The same at a pixel position, for the slots spaced off the tile grid.
    void tile_px(int x0, int y0, int t, int bank, bool hflip = false, bool vflip = false) {
        if (t < 0 || t >= kCharTiles) return;
        const std::uint8_t* src = &a_.chars[static_cast<std::size_t>(t) * 32u];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int sx = hflip ? 7 - x : x;
                const int sy = vflip ? 7 - y : y;
                const std::uint8_t b = src[sy * 4 + sx / 2];
                const int i = (sx & 1) ? (b >> 4) : (b & 0xF);
                const int X = x0 + x + ox_, Y = y0 + y + oy_;
                if (i && X >= 0 && X < kSheetW && Y >= 0 && Y < h_)
                    px_[Y * kSheetW + X] = a_.pal[bank][i];
            }
    }
    // A run of consecutive tiles, as the window lays its labels.
    void run(int col, int row, int t0, int n, int bank) {
        for (int i = 0; i < n; ++i) tile(col + i, row, t0 + i, bank);
    }
    // The window's small digits (tile 0x56 + d), right-aligned so the last
    // digit sits in column `last`.
    void number(int last, int row, int value) {
        if (value < 0) return;
        int col = last;
        do {
            tile(col--, row, 0x56 + value % 10, kInk);
            value /= 10;
        } while (value > 0);
    }
    // A change: the digits as number() draws them, their ink green when up
    // and red when down, a sign drawn in the same weight before them; none
    // for no change.
    void signed_number(int last, int row, int value) {
        // Up in the ink of the game's bank 6, its blue for a better figure (the user
        // asked for the same blue for + changes in the item shop).
        const std::uint32_t tint = value > 0 ? bgr555(0x6065u) : value < 0 ? 0xFFD02818u : 0u;
        int v = value < 0 ? -value : value;
        int col = last;
        do {
            digit(col--, row, v % 10, tint);
            v /= 10;
        } while (v > 0);
        if (value == 0) return;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const bool bar = y >= 3 && y <= 4 && x >= 1 && x <= 6;
                const bool stem = value > 0 && x >= 3 && x <= 4 && y >= 1 && y <= 6;
                if (bar || stem)
                    if (std::uint32_t* p = pixel(col * 8 + x, row * 8 + y)) *p = tint;
            }
    }
    // One of the window's digits, its dark ink in `tint` (0: as drawn).
    void digit(int col, int row, int d, std::uint32_t tint) {
        const std::uint8_t* src = &a_.chars[static_cast<std::size_t>(0x56 + d) * 32u];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = src[y * 4 + x / 2];
                const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                if (!i) continue;
                std::uint32_t c = a_.pal[kInk][i];
                const unsigned luma = ((c >> 16) & 0xFFu) * 3 + ((c >> 8) & 0xFFu) * 6 + (c & 0xFFu);
                if (tint && luma < 1000u) c = tint;
                if (std::uint32_t* p = pixel(col * 8 + x, row * 8 + y)) *p = c;
            }
    }
    // The game's ink for what cannot be picked, as a mermaid's list of shoes
    // (measured on states/thor/0125).
    static constexpr std::uint32_t kGreyInk = 0xFF63737Bu, kGreyShade = 0xFF94AD94u;
    void set_grey(bool on) { grey_ = on; }
    // A change in tenths, as the speed is printed: sign, digits, a point before the
    // last (the digits fill their tiles: the whole part 3 px left to make it room),
    // ending at x_end, on tile row `row`. Up in the game's blue, down in red.
    void signed_tenths(int x_end, int row, int value) {
        const std::uint32_t tint = value > 0 ? bgr555(0x6065u) : 0xFFD02818u;
        int v = value < 0 ? -value : value;
        auto glyph = [&](int x0, int d) {
            const std::uint8_t* src = &a_.chars[static_cast<std::size_t>(0x56 + d) * 32u];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const std::uint8_t b = src[y * 4 + x / 2];
                    const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                    if (!i) continue;
                    std::uint32_t c = a_.pal[kInk][i];
                    const unsigned luma = ((c >> 16) & 0xFFu) * 3 + ((c >> 8) & 0xFFu) * 6 + (c & 0xFFu);
                    if (luma < 1000u) c = tint;
                    if (std::uint32_t* p = pixel(x0 + x, row * 8 + y)) *p = c;
                }
        };
        int x = x_end - 8;
        glyph(x, v % 10);
        for (int y = 4; y < 6; ++y)
            for (int q = -2; q < 0; ++q)
                if (std::uint32_t* p = pixel(x + q, row * 8 + y)) *p = tint;
        x -= 11;
        v /= 10;
        do {
            glyph(x, v % 10);
            v /= 10;
            x -= 8;
        } while (v > 0);
        for (int y = 0; y < 8; ++y)
            for (int q = 0; q < 8; ++q) {
                const bool bar = y >= 3 && y <= 4 && q >= 1 && q <= 6;
                const bool stem = value > 0 && q >= 3 && q <= 4 && y >= 1 && y <= 6;
                if (bar || stem)
                    if (std::uint32_t* p = pixel(x + q, row * 8 + y)) *p = tint;
            }
    }
    // A 16-row 4bpp glyph as the font and the name strips store it, drawn
    // at a pixel position in the palette's ink bank.
    void glyph16(std::uint32_t addr, int x0, int y0) {
        if (addr < kRomBase || addr - kRomBase + 64u > rom_size_) return;
        const std::uint8_t* g = rom_ + (addr - kRomBase);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = g[y * 4 + x / 2];
                const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                const int X = x0 + x + ox_, Y = y0 + y + oy_;
                if (i && X >= 0 && X < kSheetW && Y >= 0 && Y < h_) {
                    std::uint32_t ink = a_.pal[kInk][i];
                    if (grey_) {
                        const unsigned luma = ((ink >> 16) & 0xFFu) * 3 + ((ink >> 8) & 0xFFu) * 6 + (ink & 0xFFu);
                        ink = luma < 1000u ? kGreyInk : kGreyShade;
                    }
                    px_[Y * kSheetW + X] = ink;
                }
            }
    }
    // Text in the game's own proportional font, from ROM character codes.
    int text_width(const std::uint8_t* codes, int n) const {
        int w = 0;
        for (int i = 0; i < n; ++i) w += advance(codes[i]);
        return w;
    }
    void text(const std::uint8_t* codes, int n, int x, int y) {
        for (int i = 0; i < n; ++i) {
            if (codes[i] != kSpaceCode)
                glyph16(kGlyphs + static_cast<std::uint32_t>(codes[i]) * 64u, x, y);
            x += advance(codes[i]);
        }
    }
    // A name the way the game draws it: its strip if it has one, otherwise
    // its text in the font. Returns the width drawn.
    int name(const UnitName& n, int x, int y) {
        if (n.strip >= 0) {
            std::uint32_t addr = 0;
            const int cells = name_strip_raw(n.strip, &addr);
            for (int c = 0; c < cells; ++c)
                glyph16(addr + static_cast<std::uint32_t>(c) * 64u, x + c * 8, y);
            return cells * 8;
        }
        text(n.codes, n.ncodes, x, y);
        return text_width(n.codes, n.ncodes);
    }
    // An item's picture over its box, as the game's sprite draws it, in the
    // sprite palette the picture table names for it.
    void item_icon(int pic, int x0, int y0) {
        if (pic <= 0 || pic > kMaxRomItem) return;
        const std::uint32_t hdr = kItemIconTable - kRomBase;
        if (hdr + 2u > rom_size_) return;
        const std::uint32_t at = hdr + (rom_[hdr] | (rom_[hdr + 1] << 8)) +
                                 static_cast<std::uint32_t>(pic) * 128u;
        if (at + 128u > rom_size_) return;
        const int bank = rom_[hdr + 4u + static_cast<std::uint32_t>(pic) * 2u] >> 4;
        for (int q = 0; q < 4; ++q)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const std::uint8_t b = rom_[at + q * 32u + y * 4 + x / 2];
                    const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                    if (!i) continue;
                    const int X = x0 + (q & 1) * 8 + x + ox_, Y = y0 + (q >> 1) * 8 + y + oy_;
                    if (X >= 0 && X < kSheetW && Y >= 0 && Y < h_)
                        px_[Y * kSheetW + X] = a_.obj_pal[bank][i];
                }
    }
    // One 48x48 portrait layer over the 6x6 cells from (col, row): the tiles
    // decoded from the portrait archive, coloured by its own ROM palette.
    void portrait_layer(int layer_index, int pal_index, int col, int row) {
        if (layer_index <= 0) return;
        std::vector<std::uint8_t> tiles;
        int shift = 0;
        if (layer_index * 18 <= a7_count(rom_, rom_size_, kPortraitArchive)) {
            for (int i = 0; i < 18; ++i) {
                std::vector<std::uint8_t> e;
                if (!a7_entry(rom_, rom_size_, kPortraitArchive, (layer_index - 1) * 18 + i, &e))
                    return;
                tiles.insert(tiles.end(), e.begin(), e.end());
            }
        } else {
            // A layer the status window's archive does not hold (99 and up: Cirvante's
            // and Lethe's later portraits, 120 and 121, Thor states 0300 / 0301: "They
            // have no portrait on the bottom screen. Yet ... we see their dialog
            // portraits"): the dialog's picture of it, 40 wide, in the box's middle.
            std::vector<std::uint8_t> d;
            for (int i = 0; i < 15; ++i) {
                std::vector<std::uint8_t> e;
                if (!a7_entry(rom_, rom_size_, kDialogPortraitArchive, (layer_index - 1) * 15 + i, &e))
                    return;
                d.insert(d.end(), e.begin(), e.end());
            }
            if (d.size() < 30u * 32u) return;
            tiles.assign(36u * 32u, 0);
            auto put = [&](int src, int c, int r) {
                std::copy_n(d.begin() + src * 32, 32, tiles.begin() + (r * 6 + c) * 32);
            };
            for (int i = 0; i < 16; ++i) put(i, i % 4, i / 4);
            for (int i = 0; i < 4; ++i) put(16 + i, 4, i);
            for (int i = 0; i < 8; ++i) put(20 + i, i % 4, 4 + i / 4);
            for (int i = 0; i < 2; ++i) put(28 + i, 4, 4 + i);
            shift = 4;
        }
        if (tiles.size() < 36u * 32u) return;
        const std::uint32_t pal = kPortraitPalettes - kRomBase +
                                  static_cast<std::uint32_t>(pal_index) * 32u;
        if (pal + 32u > rom_size_) return;
        std::uint32_t colours[16];
        for (int i = 0; i < 16; ++i)
            colours[i] = bgr555(static_cast<std::uint16_t>(rom_[pal + i * 2] |
                                                           (rom_[pal + i * 2 + 1] << 8)));
        for (int k = 0; k < 36; ++k)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const std::uint8_t b = tiles[k * 32 + y * 4 + x / 2];
                    const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                    if (!i) continue;
                    const int X = (col + k % 6) * 8 + x + ox_ + shift, Y = (row + k / 6) * 8 + y + oy_;
                    if (X >= 0 && X < kSheetW && Y >= 0 && Y < h_) px_[Y * kSheetW + X] = colours[i];
                }
    }
    // A 2x2 icon decoded from an A7 archive two entries at a time (two tiles
    // each: the top row, then the bottom), each row in the bank the nibble
    // table gives it, as the window's 0x0803BDC0 copies and maps its move
    // icons: row k of icon i takes nibble 4i + 2k.
    void archive_icon(std::uint32_t archive, std::uint32_t banks, int i, int x0, int y0) {
        for (int row = 0; row < 2; ++row) {
            std::vector<std::uint8_t> e;
            if (!a7_entry(rom_, rom_size_, archive, 2 * i + row, &e) || e.size() < 64u) return;
            const int bank = nibble_bank(banks, 4 * i + 2 * row);
            for (int q = 0; q < 2; ++q)
                raw_tile(x0 + q * 8, y0 + row * 8, &e[static_cast<std::size_t>(q) * 32u], a_.pal[bank]);
        }
    }
    // An 8x8 4bpp tile from outside the sheet's char block.
    void raw_tile(int x0, int y0, const std::uint8_t* src, const std::uint32_t* colours) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = src[y * 4 + x / 2];
                const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                const int X = x0 + x + ox_, Y = y0 + y + oy_;
                if (i && X >= 0 && X < kSheetW && Y >= 0 && Y < h_)
                    px_[Y * kSheetW + X] = colours[i];
            }
    }
    const std::uint32_t* obj_palette(int bank) const { return a_.obj_pal[bank]; }
    int nibble_bank(std::uint32_t table, int k) const {
        const std::uint32_t a = table + static_cast<std::uint32_t>(k) / 2u;
        if (a - kRomBase >= rom_size_) return kInk;
        const std::uint8_t b = rom_[a - kRomBase];
        return (k & 1) ? (b & 0xF) : (b >> 4);
    }
    int palette_bank_of(int t) const {
        if (t < 0) return kInk;
        const std::uint32_t a = kTileBanks + static_cast<std::uint32_t>(t) / 2u;
        if (a - kRomBase >= rom_size_) return kInk;
        const std::uint8_t b = rom_[a - kRomBase];
        // As the game reads it (0x0803B4CE): odd tiles the low nibble, even
        // tiles the high one. The other way round draws Neutral in the
        // Chaotic colour and Chaotic in the Neutral one.
        return (t & 1) ? (b & 0xF) : (b >> 4);
    }
    std::uint16_t rom16(std::uint32_t a) const {
        if (a - kRomBase + 2u > rom_size_) return 0;
        return static_cast<std::uint16_t>(rom_[a - kRomBase] |
                                          (rom_[a - kRomBase + 1] << 8));
    }

private:
    bool grey_ = false;
    int advance(std::uint8_t c) const {
        if (c == kSpaceCode) return kSpaceWidth;
        const std::uint32_t a = kGlyphWidths + c;
        return (a - kRomBase < rom_size_) ? rom_[a - kRomBase] : 6;
    }
    const Art& a_;
    const std::uint8_t* rom_;
    std::uint32_t rom_size_;
    std::uint32_t* px_;
    int oy_ = 0;
    int ox_ = 0;
    int h_ = kSheetH;
};

// A card: the sheet's frame and its parchment weave, in tile coordinates.
void card(Canvas& c, int c0, int r0, int c1, int r1, int bank) {
    for (int r = r0; r <= r1; ++r)
        for (int k = c0; k <= c1; ++k) {
            int t;
            if (r == r0) t = (k == c0) ? 0x009 : (k == c1) ? 0x00A : 0x00D;
            else if (r == r1) t = (k == c0) ? 0x00B : (k == c1) ? 0x00C : 0x00E;
            else if (k == c0) t = 0x00F;
            else if (k == c1) t = 0x010;
            else t = 0x005 + ((k - c0 - 1) + 2 * ((r - r0 - 1) & 1)) % 4;
            c.tile(k, r, t, bank);
        }
}

// The same `w` pixels wide from the canvas's x origin (w >= 16, any multiple
// of 4): whole columns from the left, the right edge's column pinned to the
// card's right side over the one it overlaps.
void card_w(Canvas& c, int r0, int r1, int w, int bank) {
    const int last = (w - 8 + 7) / 8;   // columns 0 .. last
    for (int r = r0; r <= r1; ++r)
        for (int k = 0; k <= last; ++k) {
            const bool right = k == last;
            int t;
            if (r == r0) t = (k == 0) ? 0x009 : right ? 0x00A : 0x00D;
            else if (r == r1) t = (k == 0) ? 0x00B : right ? 0x00C : 0x00E;
            else if (k == 0) t = 0x00F;
            else if (right) t = 0x010;
            else t = 0x005 + ((k - 1) + 2 * ((r - r0 - 1) & 1)) % 4;
            c.tile_px(right ? w - 8 : k * 8, r * 8, t, bank);
        }
}

// The bevelled box the game puts under an element or item icon (2x2).
void box(Canvas& c, int x, int y, int bank) {
    c.tile_px(x, y, 0x001, bank);
    c.tile_px(x + 8, y, 0x002, bank);
    c.tile_px(x, y + 8, 0x003, bank);
    c.tile_px(x + 8, y + 8, 0x004, bank);
}
// The lower cards' split, and the columns in them (see render_unit_sheet).
// The columns past the window's 30 (kol_sheet.h): on the unit card they open
// between the portrait and the HP/MP group, below they widen EQUIP. (the
// TALENT card moves right with its POW and MP columns).
constexpr int kExtraCols = kSheetCols - 30;
constexpr int kTalentCard = 13 + kExtraCols;   // first column of the TALENT card
constexpr int kPowX = 192 + 8 * kExtraCols;    // POW's two digits, under its plate
constexpr int kMpX = 216 + 8 * kExtraCols;     // MP's, where the window has its column

// A talent's icon as 0x0803EF44 composes it: two offsets from tile 0xFA,
// chosen by the talent's kind (and, for kind 8, its subtype), give the
// top-left, top-right and bottom-left tiles (a, a+1, a+2) and the
// bottom-right one (b); each tile takes its own bank. A slot never filled
// (id 0xFF) is the crossed box, offsets 0x22/0x25.
void talent_icon(Canvas& c, int x, int y, int id, int kind, int sub) {
    int a = 0, b = 0;
    if (id < 0) {
        a = 0x22; b = 0x25;
    } else if (kind >= 1 && kind <= 6) {
        b = kind + 2;
    } else if (kind == 11) {
        a = 0x1E; b = 0x21;
    } else if (kind >= 13 && kind <= 17) {
        a = 0x1A; b = 0x1D;
    } else if (kind == 8) {
        if (sub == 0) { a = 0x16; b = 0x19; }
        else if (sub >= 1 && sub <= 6) { a = 9; b = sub + 11; }
        else if (sub == 10) {
            // This one case writes its four tiles in one row, banks straight
            // from bytes +0xD/+0xE of the table; drawn as the game draws it.
            const int bd = c.nibble_bank(kTalentBanks, 0x12), be = c.nibble_bank(kTalentBanks, 0x14);
            const int ld = c.nibble_bank(kTalentBanks, 0x13), le = c.nibble_bank(kTalentBanks, 0x15);
            c.tile_px(x, y, 0x10C, bd);
            c.tile_px(x + 8, y, 0x10E, be);
            c.tile_px(x + 16, y, 0x10D, ld);
            c.tile_px(x + 24, y, 0x10F, le);
            return;
        }
    }
    const int t[4] = {a, a + 1, a + 2, b};
    for (int q = 0; q < 4; ++q)
        c.tile_px(x + (q & 1) * 8, y + (q >> 1) * 8, kTalentTile + t[q],
                  c.nibble_bank(kTalentBanks, t[q]));
}

// The POW column's two big digits (0x0803AEEC): tile 0x29 + 2d over 0x2A +
// 2d, leading zero blank, only the last two digits, as the game draws them.
void big_number2(Canvas& c, int x, int y, int value) {
    const int tens = (value / 10) % 10, units = value % 10;
    if (tens) {
        c.tile_px(x, y, 0x029 + 2 * tens, kInk);
        c.tile_px(x, y + 8, 0x02A + 2 * tens, kInk);
    }
    c.tile_px(x + 8, y, 0x029 + 2 * units, kInk);
    c.tile_px(x + 8, y + 8, 0x02A + 2 * units, kInk);
}

// An HP/MP bar: caps at both ends, eight fill tiles moving in 2 px steps
// (0xC7 empty, 0xC3 2 px, 0xC4 4 px, 0xC5 6 px, 0xC6 full).
// An SP bar is the same shape from its own tiles, 0xC8 on: caps 0xC8 and
// fill 0xC9-0xCD, each tile in its own bank from the bank table, as the
// window's bar routine 0x0806EBD0 draws mode 2 (fill_bank < 0).
void bar(Canvas& c, int col, int row, int cur, int max, int fill_bank) {
    const bool sp = fill_bank < 0;
    const int base = sp ? 0x0C8 : 0x0C2;
    const int cap_bank = sp ? c.palette_bank_of(base) : kLabel;
    c.tile(col, row, base, cap_bank);
    c.tile(col + 9, row, base, cap_bank, true);
    int px = (max > 0 && cur > 0) ? cur * 64 / max : 0;
    if (cur > 0 && px < 2) px = 2;
    px &= ~1;
    for (int i = 0; i < 8; ++i) {
        const int f = px - i * 8;
        const int t = base + (f >= 8 ? 4 : f >= 6 ? 3 : f >= 4 ? 2 : f >= 2 ? 1 : 5);
        c.tile(col + 1 + i, row, t, sp ? c.palette_bank_of(t) : fill_bank);
    }
}

// The HP bar with the part a blow took still showing: the bar is drawn full
// to `ghost`, then at `cur`, and the pixels that differ (the lost part's
// fill) are tinted, white as the blow lands, then red as it drains (a heal:
// the part gained, green, the bar filling into it).
void bar_ghost(Canvas& c, int col, int row, int cur, int ghost, int max, int fill_bank, bool flash,
               bool heal) {
    if (ghost <= cur) {
        bar(c, col, row, cur, max, fill_bank);
        return;
    }
    std::uint32_t full[10 * 8 * 8];
    bar(c, col, row, ghost, max, fill_bank);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 80; ++x)
            if (const std::uint32_t* p = c.pixel(col * 8 + x, row * 8 + y)) full[y * 80 + x] = *p;
    bar(c, col, row, cur, max, fill_bank);
    const std::uint32_t tint = flash ? 0xFFFFFFFFu : heal ? 0xFF60E070u : 0xFFF03028u;
    const double k = flash ? 0.85 : heal ? 0.6 : 0.7;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 80; ++x) {
            std::uint32_t* p = c.pixel(col * 8 + x, row * 8 + y);
            const std::uint32_t f = full[y * 80 + x];
            if (!p || *p == f) continue;
            auto ch = [&](int sh) {
                const double v = ((f >> sh) & 0xFFu) * (1.0 - k) + ((tint >> sh) & 0xFFu) * k;
                return static_cast<std::uint32_t>(v + 0.5) << sh;
            };
            *p = 0xFF000000u | ch(16) | ch(8) | ch(0);
        }
}

// The top card's contents: the unit, as the window has it.
// `confirm` (a battle confirmation, draw_sheet_forecast): 1 the actor's card,
// 2 the target's; neither has its move row, and the target's class, HP and
// MP stand kConfirmDrop lower (its card's origin is `base_y`), the room
// above them and the actor's move row taken by the confirmation's lines
// (the user, 2026-11-01).
// `drop` is how far they stand lower now: they slide (g_drop, render_unit_
// sheet; the user asked for the class, HP and MP to move with a quick
// animation).
constexpr int kConfirmDrop = 24;
constexpr double kDropSeconds = 0.15;
int g_drop = 0;   // the target's drop drawn last (draw_sheet_forecast waits for the whole of it)
void unit_card(Canvas& c, const UnitInfo& u, int fb, int confirm = 0, int base_y = 0, int drop = 0) {
    // Name on the top line, class right-aligned on it.
    c.text(u.name_codes, u.name_ncodes, 16, 8);
    if (confirm == 2) c.set_origin_y(base_y + drop);
    {
        const int cw = u.cls.strip >= 0 ? 0 : c.text_width(u.cls.codes, u.cls.ncodes);
        std::uint32_t addr = 0;
        const int strip_w = u.cls.strip >= 0 ? name_strip_raw(u.cls.strip, &addr) * 8 : 0;
        c.name(u.cls, (27 + kExtraCols) * 8 - (cw + strip_w), 8);
    }
    if (confirm == 2) c.set_origin_y(base_y);

    // Element icon in its box, alignment letter at its lower right.
    if (u.element >= 1 && u.element <= 6) {
        box(c, 16, 24, fb);
        const int base = c.rom16(kIconBaseTable + static_cast<std::uint32_t>(u.element - 1) * 2u);
        for (int q = 0; q < 4; ++q)
            c.tile(2 + (q & 1), 3 + (q >> 1), base + q, c.palette_bank_of(base + q));
    }
    if (u.alignment >= 1 && u.alignment <= 3) {
        const int t = 0x0BF + u.alignment - 1;
        c.tile(4, 4, t, c.palette_bank_of(t));
    }
    // Lv. over EXP beside the icon, where the window has them.
    c.run(6, 3, 0x0B3, 2, kLabel);             // Lv.
    c.number(10, 3, u.level);
    c.run(6, 4, 0x0B5, 2, kLabel);             // EXP
    c.number(10, 4, u.exp);

    // The portrait's frame, and the support/fear gauge along its foot, as
    // the window draws them (the unit's own sprite inside is not drawn).
    // Three distinct rows, mirrored: the right column flipped across, the
    // lower three rows flipped down, exactly as the window's tilemap has it.
    for (int r = 1; r <= 6; ++r) {
        const int edge = (r == 1 || r == 6) ? 0x021 : (r == 2 || r == 5) ? 0x023 : 0x025;
        for (int k = 12; k <= 17; ++k)
            c.tile(k, r, (k == 12 || k == 17) ? edge : edge + 1,
                   u.other_team ? kPortraitOther : kFurniture, k == 17, r >= 4);
    }
    // The portrait fills the box, layer 0 under layer 1 as the window
    // stacks them (BG1 under BG2), and the gauge lies over its bottom row,
    // where the game puts it over the walking sprite.
    c.portrait_layer(u.portrait_tiles[0], u.portrait_pal[0], 12, 1);
    c.portrait_layer(u.portrait_tiles[1], u.portrait_pal[1], 12, 1);
    if (u.support_known) {
        const int level = 2 - u.support;
        for (int i = 0; i < 6; ++i)
            c.tile(12 + i, 6, kGaugeTile + level * 6 + i,
                   c.nibble_bank(kGaugeBanks, level * 6 + i));
    }

    // HP and MP, each over its bar, where the window has them (and the
    // columns right of the portrait one further over, kExtraCols).
    constexpr int R = kExtraCols;
    if (confirm == 2) c.set_origin_y(base_y + drop);
    c.run(19 + R, 3, 0x0A8, 2, kLabel);            // HP:
    c.number(23 + R, 3, u.hp);
    c.tile(24 + R, 3, 0x060, kInk);                // /
    c.number(27 + R, 3, u.hp_max);
    bar_ghost(c, 19 + R, 4, u.hp_bar >= 0 ? u.hp_bar : u.hp, u.hp_ghost, u.hp_max, kLabel,
              u.hp_flash, u.hp_heal);
    // MP:, or SP: for the classes the game gives SP (UnitInfo::sp), in the
    // label's own bank as the window reads it (0x086B7530 +0x59 / +0x5A).
    if (u.sp) c.run(19 + R, 5, 0x0AC, 2, c.palette_bank_of(0x0AC));   // SP:
    else      c.run(19 + R, 5, 0x0AA, 2, kLabel2);                    // MP:
    c.number(23 + R, 5, u.mp);
    c.tile(24 + R, 5, 0x060, kInk);
    c.number(27 + R, 5, u.mp_max);
    bar(c, 19 + R, 6, u.mp, u.mp_max, u.sp ? -1 : kLabel2);
    if (confirm == 2) c.set_origin_y(base_y);

    // STR / INT / AGI, with the attack/defence pairs on the INT and AGI rows.
    // One row lower than the window, so the stats clear the portrait's box.
    c.run(2, 7, 0x168, 2, kLabel);  c.tile(4, 7, 0x16A, kLabel);
    c.run(2, 8, 0x16B, 2, kLabel);  c.tile(4, 8, 0x16A, kLabel);
    c.run(2, 9, 0x16D, 2, kLabel);  c.tile(4, 9, 0x16A, kLabel);
    c.number(7, 7, u.str_);
    c.number(7, 8, u.intel);
    c.number(7, 9, u.agi);
    c.run(9, 8, 0x0E7, 2, kLabel);             // melee attack
    c.number(13, 8, u.atk_melee);
    c.run(14, 8, 0x0EB, 2, kLabel);            // melee defence
    c.number(18, 8, u.def);
    c.run(9, 9, 0x0E9, 2, kLabel);             // magic attack
    c.number(13, 9, u.atk_magic);
    c.run(14, 9, 0x0ED, 2, kLabel);            // magic defence
    c.number(18, 9, u.mdef);

    // The move row beside INT/AGI, as the window has it (one row lower, with
    // the stats): the land and water movement icons, MOVE, and the speed.
    if (confirm) return;
    if (u.move_land >= 0) c.archive_icon(kLandIcons, kLandBanks, u.move_land, (20 + R) * 8, 8 * 8);
    if (u.move_water >= 0) c.archive_icon(kWaterIcons, kWaterBanks, u.move_water, (22 + R) * 8, 8 * 8);
    if (u.speed >= 0) {
        // MOVE in the window's own furniture bank (0x0803DCD4 reads it from
        // the control block, +0x5EC4): grey on another side's green sheet.
        c.run(24 + R, 8, 0x15C, 3, fb);
        // Three big digits (0x0803AEEC, tile 0x42 + 2d over 0x43 + 2d, a
        // leading zero blank), the dot sprite over the last two.
        const int d[3] = {u.speed / 100 % 10, u.speed / 10 % 10, u.speed % 10};
        for (int k = 0; k < 3; ++k) {
            if (k == 0 && d[0] == 0) continue;
            c.tile(26 + R + k, 8, 0x042 + 2 * d[k], kInk);
            c.tile(26 + R + k, 9, 0x043 + 2 * d[k], kInk);
        }
        c.raw_tile((27 + R) * 8 + 5, 9 * 8, kSpeedDot, c.obj_palette(1));
    }
}

// A card floating over the sheet while the window's cursor is on something
// with a HELP text: the words the game's help box shows for it, in its font,
// all of them at once, the card as tall as the text needs. The box shows one
// page of two short lines at a time; here the text flows across the card's
// width instead, a new paragraph only where a page ends a sentence (its last
// character is a period), with 4 px more above it. A sentence often runs
// across a page, and a break there read as awkward. Lines are 16 px apart,
// as the game sets them; a text too long for the sheet drops the paragraph
// gaps, then closes the lines up until it fits. The card sits at the top, and the sheet it covers is dimmed so the
// card stands out. It ends at or above `limit`, the top of a slot left lit
// under it (a top-row emblem starts at 100: Philosopher's Stone's five lines
// and a paragraph needed 104).
int help_card(Canvas& c, const UnitInfo& u, int bank, std::uint32_t* argb, int limit,
              const std::uint8_t* title = nullptr, int title_len = 0, int fixed_h = 0) {
    // The text reflowed: the box's line ends (0x88) join their lines, and a
    // page end (0x8E) joins too unless the page ends a sentence (a period),
    // which starts a new paragraph. A break after a hyphen joins without a
    // space ("blood-" "curdling"). Then word wrap to the card's width. A
    // `title` is a paragraph of its own above the text (the bar's help).
    std::uint8_t text[sizeof u.help_codes + 32];
    int paras[34] = {0};   // paragraph k is text[paras[k] .. paras[k + 1])
    int tn = 0, np = 0;
    for (int i = 0; title && i < title_len && i < 32; ++i) text[tn++] = title[i];
    if (tn > 0) paras[++np] = tn;
    auto join = [&] {
        if (tn > paras[np] && text[tn - 1] != 0x89u && text[tn - 1] != kDashCode)
            text[tn++] = 0x89u;
    };
    for (int i = 0; i < u.help_len && tn < static_cast<int>(sizeof text) - 1; ++i) {
        const std::uint8_t b = u.help_codes[i];
        if (b == 0x8Du) break;
        if (b == 0x88u) { join(); continue; }
        if (b == 0x8Eu) {
            int k = tn - 1;
            while (k >= paras[np] && text[k] == 0x89u) --k;   // past any spaces
            if (k >= paras[np] && text[k] == 0x40u && np < 31) {
                tn = k + 1;                                    // a period: new paragraph
                paras[++np] = tn;
            } else {
                join();
            }
            if (i + 1 < u.help_len && u.help_codes[i + 1] == 0x8Au) ++i;
            continue;
        }
        if (b == 0x89u && (tn == paras[np] || text[tn - 1] == 0x89u)) continue;
        text[tn++] = b;
    }
    paras[++np] = tn;

    constexpr int kTextX = 12, kWidth = kSheetW - 2 * kTextX;
    struct Line { int start, len, gaps; };   // gaps: paragraph breaks above
    Line lines[32];
    int n = 0;
    for (int p = 0; p < np && n < 32; ++p) {
        int i = paras[p];
        const int e = paras[p + 1];
        while (i < e && n < 32) {
            // As many whole words as fit; a word wider than the card alone.
            int end = i, w = 0, fit = i;
            while (end < e) {
                int we = end;
                while (we < e && text[we] != 0x89u) ++we;
                const int ww = c.text_width(text + end, we - end);
                const int sp = end > i ? c.text_width(text + end - 1, 1) : 0;
                if (end > i && w + sp + ww > kWidth) break;
                w += sp + ww;
                fit = we;
                end = we < e ? we + 1 : we;
            }
            lines[n++] = {i, fit - i, p};
            i = fit;
            while (i < e && text[i] == 0x89u) ++i;
        }
    }
    if (n == 0) return 0;
    constexpr int kTop = 10, kBottom = 8, kGlyph = 16;
    const int room = limit / 8 * 8 - kTop - kBottom;   // the card is whole tile rows
    int pitch = 16, gap = 4;
    const int breaks = lines[n - 1].gaps;
    auto span = [&] { return (n - 1) * pitch + breaks * gap + kGlyph; };
    if (span() > room) gap = 0;
    if (span() > room) pitch = (room - kGlyph) / (n - 1);
    // `fixed_h` (the magic shop's): the card that tall at least, the text from
    // its top, so the first line stays put from one card to the next.
    const int rows = std::max((kTop + span() + kBottom + 7) / 8, fixed_h / 8);
    constexpr int r0 = 0;
    // Dim the sheet to 9/16, then clear what the card covers: the frame's
    // corners have see-through pixels.
    for (int i = 0; i < kSheetW * kSheetH; ++i) {
        const std::uint32_t p = argb[i];
        const std::uint32_t r = ((p >> 16) & 0xFFu) * 9u / 16u;
        const std::uint32_t g = ((p >> 8) & 0xFFu) * 9u / 16u;
        const std::uint32_t b = (p & 0xFFu) * 9u / 16u;
        argb[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
    for (int y = r0 * 8; y < (r0 + rows) * 8; ++y)
        for (int x = 0; x < kSheetW; ++x) argb[y * kSheetW + x] = 0xFF000000u;
    card(c, 0, r0, kSheetCols - 1, r0 + rows - 1, bank);
    const int y0 = r0 * 8 + kTop + (fixed_h > 0 ? 0 : (rows * 8 - kTop - span() - kBottom) / 2);
    for (int i = 0; i < n; ++i)
        c.text(text + lines[i].start, lines[i].len, kTextX,
               y0 + pitch * i + gap * lines[i].gaps);
    return (r0 + rows) * 8;
}

// The EMBLEM page: its slots where the window has them, four and four across
// (columns 3, 6, 9, 12 and 16, 19, 22, 25), in four rows spaced as the EQUIP.
// slots are (16 px apart, 4 px between).
int emblem_x(int slot) {
    const int col = slot % 8;
    return 8 * (col < 4 ? 3 + 3 * col : 16 + 3 * (col - 4)) + 4 * kExtraCols;   // centred
}
int emblem_y(int slot) { return 12 * 8 + 4 + (slot / 8) * 20; }

std::atomic<int> g_lower_page{0};
std::atomic<int> g_sel{-1};   // the tapped slot, on the page it was tapped on

// Where a selected slot is lit under the help card: an emblem's plate, or a
// whole EQUIP. or TALENT row, icon and name.
struct Spot { int x, y, w, h; };
Spot slot_spot(int page, int slot);

Spot slot_spot(int page, int slot) {
    if (page == 1) return {emblem_x(slot), emblem_y(slot), 16, 16};
    const int y = 12 * 8 + 4 + (slot % 4) * 20;   // as render_unit_sheet lays the rows
    if (slot < 4) return {8, y, (kTalentCard - 1) * 8 - 8, 16};
    const int x = (kTalentCard + 1) * 8 - 4;
    return {x, y, (kSheetCols - 1) * 8 - x, 16};
}

void emblem_card(Canvas& c, const UnitInfo& u, int fb) {
    card(c, 0, kSheetLowerRow, kSheetCols - 1, 22, fb);
    c.run(1, kSheetLowerRow, 0x15F, 5, kLabel);   // EMBLEM
    for (int s = 0; s < 32; ++s) {
        const int x = emblem_x(s), y = emblem_y(s);
        // The empty slot's plate, and over it the picture of an earned one.
        c.tile_px(x, y, 0x158, fb);
        c.tile_px(x + 8, y, 0x159, fb);
        c.tile_px(x, y + 8, 0x15A, fb);
        c.tile_px(x + 8, y + 8, 0x15B, fb);
        if ((u.emblems >> s) & 1u)
            c.archive_icon(kEmblemArchive, kEmblemBanks, emblem_picture(s, u.emblem_kind), x, y);
    }
}

}  // namespace

constexpr int kGothGap = 2;
// A price as the shop's GOTH box on the top screen has it (states/thor/0122:
// BG1 tiles 0x159-0x15B bank 9, the digits 0x56 + d bank 4), but in a line (the
// user asked for one after the other): the digits, then the plate, ending at x_right,
// centred on a text line's 16 rows from y. The plate is the last three tiles of
// archive 0x0870A3E7's entries 8 and 9. Its two colours are measured from that
// bank: no ROM palette holds them. Returns its left.
int draw_goth_price(Canvas& c, std::uint32_t* px, const std::uint8_t* rom, std::uint32_t rom_size, int x_right, int y,
                    int value) {
    static std::vector<std::uint8_t> plate = [&] {
        std::vector<std::uint8_t> a, b, out;
        if (a7_entry(rom, rom_size, 0x0870A3E7u, 8, &a) && a7_entry(rom, rom_size, 0x0870A3E7u, 9, &b) &&
            a.size() >= 64u && b.size() >= 64u) {
            out.assign(a.begin() + 32, a.begin() + 64);
            out.insert(out.end(), b.begin(), b.begin() + 64);
        }
        return out;
    }();
    char digits[12];
    const int n = std::snprintf(digits, sizeof digits, "%d", value);
    const int left = x_right - 24 - kGothGap - n * 8;
    if (plate.size() == 96u) {
        const std::uint32_t ink1 = bgr555(0x0087u), ink3 = bgr555(0x77FFu);
        for (int t = 0; t < 3; ++t)
            for (int py = 0; py < 8; ++py)
                for (int fx = 0; fx < 8; ++fx) {
                    const std::uint8_t v = plate[static_cast<std::size_t>(t) * 32u + static_cast<std::size_t>(py) * 4u + fx / 2u];
                    const int i = (fx & 1) ? (v >> 4) : (v & 0xF);
                    if (i) px[(y + 4 + py) * kSheetW + x_right - 24 + t * 8 + fx] = i == 3 ? ink3 : ink1;
                }
    }
    for (int k = 0; k < n; ++k)
        c.tile_px(left + k * 8, y + 4, 0x056 + (digits[k] - '0'), kInk);
    return left;
}

// The box in an item shop (README "Tap a character to buy and equip", items): as
// render_spell_box, with the unit's four items, and on the question the changes.
void render_item_box(const UnitInfo& u, std::uint32_t record, int item, int stage, int slot, bool afford,
                     bool free_copy, int pressed, const UnitInfo* after, std::uint32_t* argb) {
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!a.ok || !rom || !argb) return;
    std::vector<std::uint32_t> px(kSheetW * kSheetH, 0xFF000000u);
    Canvas c(a, rom, rom_size, px.data());
    card(c, 0, 0, kSheetCols - 1, kItemBoxH / 8 - 1, kFurniture);
    auto say = [&](const char* t, int x, int y) {
        std::uint8_t b[64];
        const int n = menu_encode(t, b, 64);
        c.text(b, n, x, y);
        return c.text_width(b, n);
    };
    auto button = [&](int x, int y, int w, int h, int style) {
        std::vector<std::uint32_t> frame(static_cast<std::size_t>(w) * h);
        menu_window_render(frame.data(), w / 8, h / 8, style);
        for (int fy = 0; fy < h; ++fy)
            for (int fx = 0; fx < w; ++fx)
                if (frame[fy * w + fx] >> 24) px[(y + fy) * kSheetW + x + fx] = frame[fy * w + fx];
    };
    UnitName name;
    shop_item_name(item, &name);
    const int price = shop_item_price(item);
    c.item_icon(item, 12, 10);
    c.name(name, 32, 10);
    if (stage == 2) {
        if (free_copy) {
            say("From your stock", 124, 10);
        } else {
            const int bx = 124 + say("Buy for", 124, 10) + 4;
            char d[12];
            const int n = std::snprintf(d, sizeof d, "%d", price);
            draw_goth_price(c, px.data(), rom, rom_size, bx + n * 8 + kGothGap + 24, 10, price);
        }
        // What comes off: this slot's, and with a two-handed weapon the others.
        int off[4], n_off = 0;
        for (int s2 = 0; s2 < 4; ++s2) {
            const int held = unit_slot_category(record, s2);
            if (held <= 0) continue;
            const bool weapon = held == 1 || held == 2;
            if (s2 == slot || (item_category(item) == 2 && weapon)) off[n_off++] = s2;
        }
        int x = 12;
        if (n_off < 2) {
            c.text(u.name_codes, u.name_ncodes, x, 30);
            x += c.text_width(u.name_codes, u.name_ncodes) + 4;
        }
        if (n_off == 0) {
            x += say("will equip", x, 30) + 4;
            c.item_icon(item, x, 30);
            x += 18 + c.name(name, x + 18, 30);
        } else {
            // (Two names do not fit after the unit's: its card is framed below.)
            x += say(n_off < 2 ? "will replace" : "Replaces", x, 30) + 4;
            for (int k = 0; k < n_off; ++k) {
                c.item_icon(u.equip_pic[off[k]], x, 30);
                x += 17 + c.name(u.equip[off[k]], x + 17, 30);
                if (k + 1 < n_off) x += say(",", x, 30) + 4;
            }
        }
        say(".", x, 30);
        // The changes, laid out as the character sheet has these figures (the user asked for
        // the character sheet's layout): STR, INT, AGI down the left, the
        // attack and defence pairs on the INT and AGI rows, then MOVE and the speed. Each
        // figure the change, signed; the columns are the sheet's, wider for the sign and
        // a gap. Rows 6-8 for its 7-9.
        if (after) {
            constexpr int kTop = 6;
            c.run(2, kTop, 0x168, 2, kLabel);      c.tile(4, kTop, 0x16A, kLabel);
            c.run(2, kTop + 1, 0x16B, 2, kLabel);  c.tile(4, kTop + 1, 0x16A, kLabel);
            c.run(2, kTop + 2, 0x16D, 2, kLabel);  c.tile(4, kTop + 2, 0x16A, kLabel);
            c.signed_number(8, kTop, after->str_ - u.str_);
            c.signed_number(8, kTop + 1, after->intel - u.intel);
            c.signed_number(8, kTop + 2, after->agi - u.agi);
            c.run(10, kTop + 1, 0x0E7, 2, kLabel);
            c.signed_number(15, kTop + 1, after->atk_melee - u.atk_melee);
            c.run(17, kTop + 1, 0x0EB, 2, kLabel);
            c.signed_number(22, kTop + 1, after->def - u.def);
            c.run(10, kTop + 2, 0x0E9, 2, kLabel);
            c.signed_number(15, kTop + 2, after->atk_magic - u.atk_magic);
            c.run(17, kTop + 2, 0x0ED, 2, kLabel);
            c.signed_number(22, kTop + 2, after->mdef - u.mdef);
            // The speed as the game's own equipment list previews it (states/thor/0126
            // and 0127): the new figure in the sheet's big digits, bank 7 red if slower,
            // 6 blue if faster, the ink's if the same.
            if (after->speed >= 0) {
                c.run(25, kTop + 1, 0x15C, 3, kFurniture);
                const int bank = after->speed < u.speed ? 7 : after->speed > u.speed ? 6 : kInk;
                const int d[3] = {after->speed / 100 % 10, after->speed / 10 % 10, after->speed % 10};
                for (int k = 0; k < 3; ++k) {
                    if (k == 0 && d[0] == 0) continue;
                    c.tile(27 + k, kTop + 1, 0x042 + 2 * d[k], bank);
                    c.tile(27 + k, kTop + 2, 0x043 + 2 * d[k], bank);
                }
                c.raw_tile(28 * 8 + 5, (kTop + 2) * 8, kSpeedDot, c.obj_palette(1));
                // And under the plate, the change (the user asked for the + or - in
                // blue below MOVE), up to the big digits, a blank column unless 10.0 up.
                if (after->speed != u.speed && u.speed >= 0)
                    c.signed_tenths((d[0] ? 27 : 28) * 8, kTop + 2, after->speed - u.speed);
            }
        }
        for (const int id : {kSpellYes, kSpellNo}) {
            const int bx = id == kSpellYes ? kSpellYesX : kSpellNoX, in = pressed == id ? 1 : 0;
            button(bx, kItemButtonY, kSpellButtonW, kSpellButtonH, in);
            std::uint8_t b[8];
            const int n = menu_encode(id == kSpellYes ? "Yes" : "No", b, 8);
            c.text(b, n, bx + (kSpellButtonW - c.text_width(b, n)) / 2 + in, kItemButtonY + 4 + in);
        }
    } else {
        if (free_copy) {
            std::uint8_t b[32];
            const int n = menu_encode("In stock", b, 32);
            c.text(b, n, 236 - c.text_width(b, n), 10);
        } else {
            const int left = draw_goth_price(c, px.data(), rom, rom_size, 236, 10, price);
            if (!afford) say("Need", left - 6 - 28, 10);
        }
        static const std::uint8_t kDashes[2] = {kDashCode, kDashCode};
        for (int s = 0; s < 4; ++s) {
            const int x = kSpellSlotX[s % 2], y = kItemSlotY[s / 2];
            const bool live = afford && item_slot_takes(record, s, item);
            const int in = live && pressed == s ? 1 : 0;
            if (live) button(x, y, kSpellSlotW, kSpellSlotH, in);
            const int ix = x + 8 + in, iy = y + 4 + in;
            c.set_grey(!live);
            if (u.equip_id[s] != 0) {
                c.item_icon(u.equip_pic[s], ix, iy);
                c.name(u.equip[s], ix + 20, iy);
            } else {
                c.text(kDashes, 2, ix + 20, iy);
            }
            c.set_grey(false);
        }
    }
    for (int y = 0; y < kItemBoxH; ++y) std::copy_n(px.data() + y * kSheetW, kSheetW, argb + y * kSheetW);
}

// The magic shop's box when a unit is tapped (README "Tap a character to buy and
// equip"): its slots and what is on sale, then the question across the box.
void render_spell_box(const UnitInfo& u, std::uint32_t record, int spell, int stage, int slot, bool afford,
                      bool free_copy, int pressed, std::uint32_t* argb) {
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!a.ok || !rom || !argb) return;
    std::vector<std::uint32_t> px(kSheetW * kSheetH, 0xFF000000u);
    Canvas c(a, rom, rom_size, px.data());
    card(c, 0, 0, kSheetCols - 1, kSpellBoxH / 8 - 1, kFurniture);
    auto encoded = [&](const char* t, std::uint8_t* b) { return menu_encode(t, b, 64); };
    auto say = [&](const char* t, int x, int y) {
        std::uint8_t b[64];
        const int n = encoded(t, b);
        c.text(b, n, x, y);
        return c.text_width(b, n);
    };
    UnitName sn;
    spell_name(spell, &sn);
    const int skind = spell_icon_kind(spell), ssub = spell_icon_sub(spell);
    auto goth_price = [&](int x_right, int y, int value) { return draw_goth_price(c, px.data(), rom, rom_size, x_right, y, value); };
    // A button as the bottom bar's: its frame by style, pushed in or greyed.
    auto button = [&](int x, int y, int w, int h, int style) {
        std::vector<std::uint32_t> frame(static_cast<std::size_t>(w) * h);
        menu_window_render(frame.data(), w / 8, h / 8, style);
        for (int fy = 0; fy < h; ++fy)
            for (int fx = 0; fx < w; ++fx)
                if (frame[fy * w + fx] >> 24) px[(y + fy) * kSheetW + x + fx] = frame[fy * w + fx];
    };
    if (stage == 2) {
        // Across the box: what is bought, what goes, the bottom bar's buttons.
        talent_icon(c, 12, 10, spell, skind, ssub);
        c.name(sn, 28, 10);
        if (free_copy) {
            say("From your stock", 112, 10);
        } else {
            const int bx = 112 + say("Buy for", 112, 10) + 6;
            char d[12];
            const int n = std::snprintf(d, sizeof d, "%d", spell_price(spell));
            goth_price(bx + n * 8 + kGothGap + 24, 10, spell_price(spell));
        }
        c.text(u.name_codes, u.name_ncodes, 12, 28);
        int x = 12 + c.text_width(u.name_codes, u.name_ncodes) + 4;
        const int held = u.talent_id[slot];
        if (held > 0) {
            x += say("will replace", x, 28) + 4;
            talent_icon(c, x, 28, held, u.talent_kind[slot], u.talent_sub[slot]);
            x += 16 + c.name(u.talent[slot], x + 16, 28);
            say(".", x, 28);
        } else {
            x += say("will learn", x, 28) + 4;
            talent_icon(c, x, 28, spell, skind, ssub);
            x += 16 + c.name(sn, x + 16, 28);
            say(".", x, 28);
        }
        for (const int id : {kSpellYes, kSpellNo}) {
            const int bx = id == kSpellYes ? kSpellYesX : kSpellNoX, in = pressed == id ? 1 : 0;
            button(bx, kSpellButtonY, kSpellButtonW, kSpellButtonH, in);
            std::uint8_t b[8];
            const int n = encoded(id == kSpellYes ? "Yes" : "No", b);
            c.text(b, n, bx + (kSpellButtonW - c.text_width(b, n)) / 2 + in, kSpellButtonY + 4 + in);
        }
    } else {
        talent_icon(c, 12, 8, spell, skind, ssub);
        c.name(sn, 28, 8);
        if (free_copy) {
            std::uint8_t b[64];
            const int n = encoded("In stock", b);
            c.text(b, n, 236 - c.text_width(b, n), 8);
        } else {
            const int left = goth_price(236, 8, spell_price(spell));   // on the name's line
            if (!afford) {
                std::uint8_t b[64];
                const int n = encoded("Need", b);
                c.text(b, n, left - 6 - c.text_width(b, n), 8);
            }
        }
        static const std::uint8_t kDashes[2] = {kDashCode, kDashCode};
        for (int s = 0; s < 4; ++s) {
            const int x = kSpellSlotX[s % 2], y = kSpellSlotY[s / 2];
            const bool live = afford && spell_slot_takes(record, s, spell);
            const int in = live && pressed == s ? 1 : 0;
            if (live) button(x, y, kSpellSlotW, kSpellSlotH, in);   // else no button: its text in grey
            const int tid = u.talent_id[s], ix = x + 8 + in, iy = y + 4 + in;
            c.set_grey(!live);
            if (tid > 0) {
                talent_icon(c, ix, iy, tid, u.talent_kind[s], u.talent_sub[s]);
                c.name(u.talent[s], ix + 16, iy);
            } else {
                if (tid < 0) talent_icon(c, ix, iy, tid, 0, 0);
                c.text(kDashes, 2, ix + 16, iy);
            }
            c.set_grey(false);
        }
    }
    for (int y = 0; y < kSpellBoxH; ++y) std::copy_n(px.data() + y * kSheetW, kSheetW, argb + y * kSheetW);
}

int render_help_card(const std::uint8_t* title, int title_len, const UnitInfo& text,
                     std::uint32_t* argb, int fixed_h) {
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!a.ok || !rom || !argb || text.help_len <= 0) return 0;
    std::fill(argb, argb + kSheetW * kSheetH, 0u);
    Canvas c(a, rom, rom_size, argb);
    return help_card(c, text, kFurniture, argb, kSheetH, title, title_len, fixed_h);
}

bool render_unit_sheet(const UnitInfo& u, std::uint32_t* argb, const UnitInfo* target, int page) {
    if (!argb || !u.valid) return false;
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!a.ok || !rom) return false;
    for (int i = 0; i < kSheetW * kSheetH; ++i) argb[i] = 0xFF000000u;
    Canvas c(a, rom, rom_size, argb);

    // Three cards, as the window has them: the unit above (rows 0-10), EQUIP.
    // and TALENT below (rows 11-22). One row more than the window above, two
    // below, for the second screen (see the header). TALENT is two columns
    // wider than the window's, EQUIP. two narrower, so it holds POW and MP
    // side by side where the window shows one or the other; every item and
    // talent name is at most 64 px, which is what both cards keep for one.
    // The sheet is cream for the player's units and green for anyone else's,
    // as the game draws it: only the furniture changes bank, never the text.
    const int fb = u.other_team ? kFurnitureOther : kFurniture;
    // Choosing a target: the attacker above as always, and below, in place
    // of its EQUIP. and TALENT, the same card for the unit under the cursor,
    // in that unit's own colours (cream for the player's, green otherwise).
    if (target && target->valid) {
        const int tfb = target->other_team ? kFurnitureOther : kFurniture;
        card(c, 0, 0, kSheetCols - 1, 10, fb);
        card(c, 0, kSheetLowerRow, kSheetCols - 1, 22, tfb);
        // The target's class, HP and MP slide down as a confirmation opens
        // and back up as it closes, eased over kDropSeconds; the move rows
        // stay away until they are home.
        const bool confirm = confirm_arrow_bank() >= 0;   // (draw_sheet_forecast fills the room)
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static bool was = false;
        static double t0 = -1.0, from = 0.0;
        if (confirm != was) { from = g_drop; t0 = now; was = confirm; }
        const double to = confirm ? kConfirmDrop : 0.0;
        double k = t0 < 0.0 ? 1.0 : std::min(1.0, (now - t0) / kDropSeconds);
        k = 1.0 - (1.0 - k) * (1.0 - k);   // ease out
        g_drop = static_cast<int>(std::lround(from + (to - from) * k));
        const int mode = confirm || g_drop > 0;
        unit_card(c, u, fb, mode ? 1 : 0, 0);
        c.set_origin_y(kSheetLowerRow * 8 + 4);   // centred in the lower card's 12 rows
        unit_card(c, *target, tfb, mode ? 2 : 0, kSheetLowerRow * 8 + 4, g_drop);
        c.set_origin_y(0);
        return true;
    }
    card(c, 0, 0, kSheetCols - 1, 10, fb);
    unit_card(c, u, fb);

    const int shown = page < 0 ? g_lower_page.load() : page;
    // The help card over the sheet, the selected slot left lit under it.
    auto help = [&] {
        if (u.help_len <= 0) return;
        const int sel = page < 0 ? g_sel.load() : -1;
        std::vector<std::uint32_t> lit;
        const Spot sp = sel >= 0 ? slot_spot(shown, sel) : Spot{0, 0, 0, 0};
        for (int y = 0; y < sp.h; ++y)
            for (int x = 0; x < sp.w; ++x) lit.push_back(argb[(sp.y + y) * kSheetW + sp.x + x]);
        const int bottom = help_card(c, u, fb, argb, sp.w > 0 ? sp.y : kSheetH);
        if (sp.w > 0 && sp.y >= bottom)
            for (int y = 0, i = 0; y < sp.h; ++y)
                for (int x = 0; x < sp.w; ++x) argb[(sp.y + y) * kSheetW + sp.x + x] = lit[i++];
    };
    if (shown == 1) {
        emblem_card(c, u, fb);
        help();
        return true;
    }

    card(c, 0, 11, kTalentCard - 1, 22, fb);
    card(c, kTalentCard, 11, kSheetCols - 1, 22, fb);

    // EQUIP. and TALENT: headers on the card borders, four slots each; POW
    // and MP plates over their columns. Icons sit against each card's inner
    // edge with the name straight after, as the window lays talents; the
    // talents 4 px further in, so POW and MP stand 8 px apart.
    c.run(1, 11, 0x0B7, 4, kLabel);
    c.run(kTalentCard + 1, 11, 0x0BB, 4, kLabel);
    for (int k = 0; k < 2; ++k) {
        c.tile_px(kPowX + k * 8, 11 * 8, 0x124 + k, kLabel);   // POW
        c.tile_px(kMpX + k * 8, 11 * 8, 0x138 + k, kLabel);    // MP
    }
    // Talent icons take the parchment half of the card's edge tile.
    const int ticon = (kTalentCard + 1) * 8 - 4;
    for (int s = 0; s < 4; ++s) {
        // The window's 16 px slots with a 4 px gap above each, filling the
        // card's ten inner rows exactly (first slot at y 100, last ends 176).
        const int y = 12 * 8 + 4 + s * 20;
        box(c, 8, y, fb);
        if (u.equip_id[s] != 0) {
            c.item_icon(u.equip_pic[s], 8, y);
            c.name(u.equip[s], 3 * 8 + 4, y);
        }
        // As the window does it: a learnt talent is its icon, its name and
        // its figures; id 0 is a blank row; a slot never filled is the
        // crossed box, "--" in the font, and big dashes in each column.
        const int tid = u.talent_id[s];
        if (tid > 0) {
            talent_icon(c, ticon, y, tid, u.talent_kind[s], u.talent_sub[s]);
            c.name(u.talent[s], ticon + 16, y);
            big_number2(c, kPowX, y, u.talent_pow[s]);
            big_number2(c, kMpX, y, u.talent_mp[s]);
        } else if (tid < 0) {
            talent_icon(c, ticon, y, tid, 0, 0);
            static const std::uint8_t kDashes[2] = {kDashCode, kDashCode};
            c.text(kDashes, 2, ticon + 16, y);
            for (const int x : {kPowX, kMpX})
                for (int k = 0; k < 2; ++k) {
                    c.tile_px(x + k * 8, y, 0x03F, kInk);
                    c.tile_px(x + k * 8, y + 8, 0x040, kInk);
                }
        }
    }
    help();
    return true;
}

int sheet_lower_page() { return g_lower_page.load(); }

void sheet_flip_lower_page() {
    g_lower_page.store(g_lower_page.load() ^ 1);
    g_sel.store(-1);
}

int sheet_selected_slot() { return g_sel.load(); }

void sheet_clear_selection() { g_sel.store(-1); }

bool sheet_tap(float x, float y) {
    const int page = g_lower_page.load(), was = g_sel.load();
    int hit = -1;
    for (int s = 0; s < (page == 1 ? 32 : 8) && hit < 0; ++s) {
        // Each slot answers a little beyond what it lights.
        const Spot sp = slot_spot(page, s);
        if (x >= sp.x - 4 && x < sp.x + sp.w + 4 && y >= sp.y - 2 && y < sp.y + sp.h + 2) hit = s;
    }
    const int now = (hit == was) ? -1 : hit;
    g_sel.store(now);
    return now != was;
}

// The army on the world map: a card a unit in the sheet's frame, two to a
// row: the unit's portrait in its frame, its name in the game's font, Lv.,
// HP: and MP: with the window's own labels and digits.
int render_army_list(const UnitInfo* units, int n, std::uint32_t* argb, int max_h) {
    const Art& a = art();
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!a.ok || !rom || !argb || n <= 0) return 0;
    const int rows = (n + 1) / 2;
    const int h = std::min(max_h - max_h % kArmyCellH,
                           std::max(kSheetH, rows * kArmyCellH));
    for (int i = 0; i < kSheetW * h; ++i) argb[i] = 0xFF000000u;
    Canvas c(a, rom, rom_size, argb, h);
    for (int k = 0; k < n; ++k) {
        const UnitInfo& u = units[k];
        // Each card kArmyCellW (124) wide, drawn from its own left edge; the
        // content keeps the window's columns from there, so the 4 px past 15
        // columns are parchment on the card's right.
        constexpr int c0 = 0;
        const int r0 = (k / 2) * (kArmyCellH / 8);
        if ((r0 + kArmyCellH / 8) * 8 > h) break;
        c.set_origin_x((k % 2) * kArmyCellW);
        card_w(c, r0, r0 + 7, kArmyCellW, kFurniture);
        for (int r = r0 + 1; r <= r0 + 6; ++r) {
            const int d = r - r0;
            const int edge = (d == 1 || d == 6) ? 0x021 : (d == 2 || d == 5) ? 0x023 : 0x025;
            for (int col = c0 + 1; col <= c0 + 6; ++col)
                c.tile(col, r, (col == c0 + 1 || col == c0 + 6) ? edge : edge + 1, kFurniture,
                       col == c0 + 6, d >= 4);
        }
        c.portrait_layer(u.portrait_tiles[0], u.portrait_pal[0], c0 + 1, r0 + 1);
        c.portrait_layer(u.portrait_tiles[1], u.portrait_pal[1], c0 + 1, r0 + 1);
        c.text(u.name_codes, u.name_ncodes, (c0 + 8) * 8, (r0 + 1) * 8);
        {
            c.run(c0 + 8, r0 + 3, 0x0B3, 2, kLabel);   // Lv.
            c.number(c0 + 13, r0 + 3, u.level);
            c.run(c0 + 8, r0 + 4, 0x0A8, 2, kLabel);   // HP:
            c.number(c0 + 13, r0 + 4, u.hp_max);
            if (u.sp) c.run(c0 + 8, r0 + 5, 0x0AC, 2, c.palette_bank_of(0x0AC));   // SP:
            else      c.run(c0 + 8, r0 + 5, 0x0AA, 2, kLabel2);                    // MP:
            c.number(c0 + 13, r0 + 5, u.mp_max);
        }
        // The element (asked 2026-10-29 for each card to show
        // the unit's element too): its icon in its box on the portrait's
        // bottom-right corner, the alignment letter beside it, as the unit
        // card has them (the user's pick of three mockups: the stats moved
        // up ran into the name's two rows; at the top right it covered long
        // names).
        if (u.element >= 1 && u.element <= 6) {
            box(c, (c0 + 5) * 8, (r0 + 5) * 8, kFurniture);
            const int base = c.rom16(kIconBaseTable + static_cast<std::uint32_t>(u.element - 1) * 2u);
            for (int q = 0; q < 4; ++q)
                c.tile(c0 + 5 + (q & 1), r0 + 5 + (q >> 1), base + q, c.palette_bank_of(base + q));
        }
        if (u.alignment >= 1 && u.alignment <= 3) {
            const int t = 0x0BF + u.alignment - 1;
            c.tile(c0 + 7, r0 + 6, t, c.palette_bank_of(t));
        }
    }
    c.set_origin_x(0);
    return h;
}

// The battle confirmation on the second screen (the user, 2026-11-01): the
// top screen's boxes and arrow are left out (kol_obj_cull
// confirm_arrow_hidden_now; kol_widescreen forecast rows), and the strip
// between the two cards shows what the player needs of them:
// - the arrow's head, pointing from the actor's card down to the target's,
//   always (the game's blinks: "it was always visible"), bobbing 0-2 px:
//   OBJ tiles 0x144-0x147 (its blue frame, 16x8 over 16x8), the bank of a
//   piece of it that is up;
// - each box's damage and hit lines ("50 pt", "100 % HIT"; the boxes' tile
//   rows 6 and 7, columns 0-9 and 20-29) and its word ("CURE", "WATER", the
//   counter's "WIND": row 9, a run from either edge) in its unit's card
//   (first on plates on the seam: "remove the blue boxes ... remove the whole
//   move section of each unit ... move the class, HP and MP down a bit").
// The rows are the game's own BG2/BG3 tiles, read live from VRAM.
namespace {
void bg_cells(std::uint32_t* argb, gba::GbaBus* bus, int tx0, int ty0, int tw, int th, int dx, int dy) {
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    int order[2] = {3, 2};   // drawn back to front by the layers' priority
    const std::uint16_t c2 = bus->read16(0x0400000Cu), c3 = bus->read16(0x0400000Eu);
    if ((c2 & 3u) > (c3 & 3u)) { order[0] = 2; order[1] = 3; }
    for (int bg : order) {
        const std::uint16_t cnt = bg == 2 ? c2 : c3;
        if (cnt & 0x80u) continue;   // (8bpp: not the HUD's)
        const std::uint32_t chr = ((cnt >> 2) & 3u) * 0x4000u, scr = ((cnt >> 8) & 0x1Fu) * 0x800u;
        const int hofs = bus->read16(0x04000010u + bg * 4u) & 0x1FF, vofs = bus->read16(0x04000012u + bg * 4u) & 0x1FF;
        for (int y = 0; y < th * 8; ++y)
            for (int x = 0; x < tw * 8; ++x) {
                const int X = (tx0 * 8 + x + hofs) & 255, Y = (ty0 * 8 + y + vofs) & 255;
                const std::uint32_t eo = scr + ((Y >> 3) * 32 + (X >> 3)) * 2u;
                const std::uint16_t e = static_cast<std::uint16_t>(vram[eo] | vram[eo + 1] << 8);
                const int px = (e & 0x400u) ? 7 - (X & 7) : (X & 7), py = (e & 0x800u) ? 7 - (Y & 7) : (Y & 7);
                const std::uint32_t to = chr + (e & 0x3FFu) * 32u + py * 4 + px / 2;
                if (to >= 0x10000u) continue;
                const int i = (px & 1) ? (vram[to] >> 4) : (vram[to] & 0xF);
                if (!i) continue;
                const std::uint32_t po = (e >> 12) * 32u + i * 2u;
                const int ox = dx + x, oy = dy + y;
                if (ox < 0 || oy < 0 || ox >= kSheetW || oy >= kSheetH) continue;
                argb[oy * kSheetW + ox] = bgr555(static_cast<std::uint16_t>(pal[po] | pal[po + 1] << 8));
            }
    }
}
// The columns row `ty` holds on the HUD layers (BG2, BG3), in either half:
// [l0, l1) in columns 0-14 and [r0, r1) in 15-29 (empty: equal). The
// action's "CURE" was columns 0-5 (0069), the counter's "WIND" 23-28
// (0075): a run need not touch the edge.
void row_runs(gba::GbaBus* bus, int ty, int* l0, int* l1, int* r0, int* r1) {
    const std::uint8_t* vram = bus->vram_ptr();
    bool on[30] = {};
    for (int bg = 2; bg <= 3; ++bg) {
        const std::uint16_t cnt = bus->read16(0x04000008u + bg * 2u);
        const std::uint32_t scr = ((cnt >> 8) & 0x1Fu) * 0x800u;
        for (int tx = 0; tx < 30; ++tx) {
            const std::uint32_t eo = scr + (ty * 32 + tx) * 2u;
            if ((vram[eo] | vram[eo + 1] << 8) & 0x3FFu) on[tx] = true;
        }
    }
    auto span = [&](int a, int b, int* s0, int* s1) {
        *s0 = *s1 = a;
        int lo = -1, hi = -1;
        for (int tx = a; tx < b; ++tx)
            if (on[tx]) { if (lo < 0) lo = tx; hi = tx; }
        if (lo >= 0) { *s0 = lo; *s1 = hi + 1; }
    };
    span(0, 15, l0, l1);
    span(15, 30, r0, r1);
}
// The column of BG tile `tile` (either HUD layer) on row `ty` in [a, b), -1 none.
int tile_column(gba::GbaBus* bus, int ty, int a, int b, unsigned tile) {
    const std::uint8_t* vram = bus->vram_ptr();
    for (int bg = 2; bg <= 3; ++bg) {
        const std::uint16_t cnt = bus->read16(0x04000008u + bg * 2u);
        const std::uint32_t scr = ((cnt >> 8) & 0x1Fu) * 0x800u;
        for (int tx = a; tx < b; ++tx) {
            const std::uint32_t eo = scr + (ty * 32 + tx) * 2u;
            if (((vram[eo] | vram[eo + 1] << 8) & 0x3FFu) == tile) return tx;
        }
    }
    return -1;
}
}  // namespace

void draw_sheet_forecast(std::uint32_t* argb, double now_s, bool beside_chance) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    const int bank = confirm_arrow_bank();   // (held, from the guest's thread)
    if (!argb || !bus || bank < 0) return;
    // Each box's two lines and its word (the action's on the left box, the
    // counter's on the right: "WIND" there on map 13) in its unit's card,
    // right of the portrait: the actor's where its move row was (render_unit_
    // sheet leaves it out), the target's above its class, HP and MP, which
    // stand lower (kConfirmDrop).
    constexpr int kInfoX = (19 + kExtraCols) * 8;
    const int lower = kSheetLowerRow * 8 + 4;
    int l0, l1, r0, r1;
    row_runs(bus, 9, &l0, &l1, &r0, &r1);

    // The two boxes' numbers in one column (the user asked for the
    // numbers aligned): each copy placed so its "%" (BG tile 0x13F, row 7) lands on
    // kPctX; a box without one keeps the left box's own place (column 3).
    constexpr unsigned kPctTile = 0x13F;
    constexpr int kPctX = kInfoX + 3 * 8;
    const int pl = tile_column(bus, 7, 0, 10, kPctTile), pr = tile_column(bus, 7, 20, 30, kPctTile);
    int word_x = kInfoX;   // (both words, moved with the numbers)
    int left_x = kPctX - (pl >= 0 ? pl : 3) * 8;
    int right_x = kPctX - (pr >= 0 ? pr - 20 : 8) * 8;
    // Then both, together, in the middle of the room right of the portrait
    // (x 144 to the card's edge at 240; the user asked for the numbers in
    // the middle of that blank space, so HIT does not crowd the
    // portrait): the span the two boxes' rows 6-7 cover, centred on 192.
    {
        int lo = kSheetW, hi = 0;
        for (int ty = 6; ty <= 7; ++ty) {
            int a0, a1, b0, b1;
            row_runs(bus, ty, &a0, &a1, &b0, &b1);
            if (a1 > a0) { lo = std::min(lo, left_x + a0 * 8); hi = std::max(hi, left_x + a1 * 8); }
            if (b1 > b0) { lo = std::min(lo, right_x + (b0 - 20) * 8); hi = std::max(hi, right_x + (b1 - 20) * 8); }
        }
        if (hi > lo) word_x += (144 + 240) / 2 - (lo + hi) / 2;
        if (hi > lo) {
            const int shift = (144 + 240) / 2 - (lo + hi) / 2;
            left_x += shift;
            right_x += shift;
        }
    }
    if (l1 > l0) bg_cells(argb, bus, l0, 9, std::min(l1 - l0, 10), 1, word_x, 7 * 8);
    bg_cells(argb, bus, 0, 6, 10, 2, left_x, 8 * 8);
    if (g_drop >= kConfirmDrop) {   // (once the class, HP and MP have slid down)
        if (r1 > r0) {
            const int n = std::min(r1 - r0, 10);
            bg_cells(argb, bus, r1 - n, 9, n, 1, word_x, lower + 8);   // (lined up with the actor's)
        }
        bg_cells(argb, bus, 20, 6, 10, 2, right_x, lower + 16);
    }
    // The head on the seam, bobbing, over the middle of the same room as the
    // numbers (x 192; the user asked for the arrow centred on that section
    // too), clear of the persuade chance at the sheet's middle.
    (void)beside_chance;
    const int mid = (144 + 240) / 2;
    const int seam = kSheetLowerRow * 8;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    static const int kBob[4] = {0, 1, 2, 1};
    const int bob = kBob[static_cast<int>(now_s / 0.133) & 3];
    auto px = [&](int sx, int sy) {   // the head as the game draws it, 16x16
        const int tile = 0x144 + (sy >= 8 ? 2 : 0) + (sx >= 8 ? 1 : 0);
        const std::uint8_t b = vram[0x10000 + tile * 32 + (sy & 7) * 4 + (sx & 7) / 2];
        return (sx & 1) ? (b >> 4) : (b & 0xF);
    };
    const int x0 = mid - 8, y0 = seam - 9 + bob;
    for (int sy = 0; sy < 16; ++sy)
        for (int sx = 0; sx < 16; ++sx) {
            const int i = px(sx, sy);
            if (!i) continue;
            const std::uint16_t c = static_cast<std::uint16_t>(pal[0x200 + bank * 32 + i * 2] | pal[0x200 + bank * 32 + i * 2 + 1] << 8);
            // Turned a quarter clockwise: pointing right there, down here.
            argb[(y0 + sx) * kSheetW + x0 + 15 - sy] = bgr555(c);
        }
}

// The battle confirm box's yellow digits and "%" ("25 % HIT", 0x080709AE):
// its BG tiles 0x11C-0x125 and 0x13F are tiles 34-43 and 69 of archive
// 0x086B904C, drawn in BG bank 12. Centred on the seam of the two cards.
void draw_sheet_chance(std::uint32_t* argb, int percent) {
    static std::vector<std::uint8_t> tiles;
    static bool tried = false;
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!tried && rom) {
        tried = true;
        constexpr std::uint32_t kConfirmArchive = 0x086B904Cu;
        const int n = a7_count(rom, rom_size, kConfirmArchive);
        for (int i = 0; i < n; ++i) {
            std::vector<std::uint8_t> e;
            if (!a7_entry(rom, rom_size, kConfirmArchive, i, &e)) { tiles.clear(); break; }
            tiles.insert(tiles.end(), e.begin(), e.end());
        }
    }
    const Art& a = art();
    if (!argb || !a.ok || tiles.size() < 70u * 32u || percent < 0) return;
    constexpr int kDigit0 = 34, kPercent = 69, kBank = 12;
    int glyphs[4], n = 0;
    for (int v = std::min(percent, 100), d = v >= 100 ? 100 : v >= 10 ? 10 : 1; d > 0; d /= 10)
        glyphs[n++] = kDigit0 + v / d % 10;
    glyphs[n++] = kPercent;
    const int x0 = (kSheetW - n * 8) / 2, y0 = kSheetLowerRow * 8 - 4;
    for (int g = 0; g < n; ++g)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = tiles[static_cast<std::size_t>(glyphs[g]) * 32u + y * 4 + x / 2];
                const int i = (x & 1) ? (b >> 4) : (b & 0xF);
                if (i) argb[(y0 + y) * kSheetW + x0 + g * 8 + x] = a.pal[kBank][i];
            }
}

}  // namespace kol
