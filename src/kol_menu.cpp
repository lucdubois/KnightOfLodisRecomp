// kol_menu.cpp — see the header.

#include "kol_dev.h"
#include "kol_menu.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gba_bus.h"
#include "kol_a7.h"
#include "kol_cheevos.h"
#include "kol_fast_actions.h"
#include "kol_rumble.h"
#include "kol_settings.h"
#include "kol_sheet.h"
#include "kol_states.h"
#include "kol_unit_data.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace kol {
namespace {

constexpr std::uint32_t kRomBase = 0x08000000u;

// ── The cartridge art (each piece matched byte for byte against VRAM with
// the game's OPTION menu open, states/B_rictor_highlighted_map + R, System,
// Option; README "The options menu on the second screen") ────────────────
constexpr std::uint32_t kBaseArchive = 0x086B75B4u;  // BG tiles from tile 0:
                                                     // frame 0x11-0x18, weave
                                                     // 0x05-0x08, HELP 0xD6-0xD9
constexpr std::uint32_t kOptionTab = 0x086BBB2Cu;    // "OPTION", 4 tiles
constexpr std::uint32_t kHandTiles = 0x086D7F61u;    // hand cursor, 16x8 sprite
constexpr std::uint32_t kArrowTile = 0x086D7FC2u;    // scroll arrow (points up)
constexpr std::uint32_t kIconArchive = 0x086DA2E4u;  // the R bar's icons, 16x16
constexpr int kOptionIcon = 2;   // its Option icon (the world map's system menu has it too: DZ)
constexpr int kSaveIcon = 17, kLoadIcon = 18;   // Save and Load, as that menu has them (EA)
// The main menu's Training and Shop (EB); its table of icons is 0x08613544
// (an entry / 2 is the icon: Organize 14, Training 20, Shop 15, System 16,
// Save 17...).
constexpr int kTrainingIcon = 20, kShopIcon = 15;
// The Organize grid's R menu (EC): Item, Spell, Class, ... in their own
// archive, 16x16 each from tile 11 (Item 11, Spell 15, Class 19; found by
// their OAM tiles), OBJ bank 0.
constexpr std::uint32_t kRosterIconArchive = 0x086DCE04u;
constexpr int kRosterIconTile = 11;
// In front of the archive: a header word, then each icon tile's OBJ bank, a
// nibble a tile, low first (the menu's draw, 0x08127F7C, reads them from
// 0x086DA294 + 4).
constexpr std::uint32_t kIconBanks = 0x086DA298u;
constexpr std::uint32_t kBgPalette = 0x0861DF68u;
constexpr std::uint32_t kObjPalette = 0x0861E288u;
constexpr int kFrameBank = 4;     // frame, weave, HELP tab and the text
constexpr int kTabBank = 12;      // the OPTION tab
constexpr std::uint32_t kGlyphs = 0x0815DEC4u;       // font: 64 bytes a glyph
constexpr std::uint32_t kGlyphWidths = 0x0815DE6Cu;
constexpr std::uint8_t kSpace = 0x89u, kNewline = 0x88u;

// ── The game's options ───────────────────────────────────────────────────
// Names and values from 0x0875F238 (each "11 00 text 8D 8A FF"), HELP texts
// from 0x0875F4B6 ("21 00 text 8D 8A FF", lines split by 0x88), both in the
// order of the definition table at 0x0860D3D4 (8 bytes an option, byte 1 the
// value count): the option, then each value.
constexpr std::uint32_t kNames = 0x0875F238u;
constexpr std::uint32_t kHelps = 0x0875F4B6u;
constexpr std::uint32_t kOptionDefs = 0x0860D3D4u;
constexpr int kGameOptions = 12;
constexpr int kDefaultOption = 11;   // "Default": an action, restores these
// Where the game keeps them: EWRAM 0x0200132C, the eight bytes its OPTION
// menu copies to its work area at open (0x0801C9FA) and reads through the
// getter 0x0801CE80. Options 0-2 are bytes 0-2; 3-7 are bits 0-4 of byte 3
// (set = ON, the first value); 8-10 are bytes 4-6.
constexpr std::uint32_t kSettings = 0x0200132Cu;
constexpr std::uint32_t kMenuWork = 0x02028950u;   // -> the open menu's work area
constexpr std::uint32_t kMenuCopy = 0x1194u;       // its copy of the eight bytes
// The defaults, as every save state here holds them before any change.
constexpr std::uint8_t kDefaults[7] = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x01, 0x00};

constexpr std::uint32_t kSoundMain = 0x0815B838u;  // MP2K SoundMain, every frame
constexpr char kHookId[] = "kol.options";

struct Text {
    std::vector<std::uint8_t> codes;
    int strip = -1;   // a pre-rendered name (0x81 NN), drawn from its bitmap
};

struct GameOption {
    Text name, help;
    std::vector<Text> values, value_help;
};

struct Art {
    bool ok = false;
    std::vector<std::uint8_t> bg;       // base archive tiles
    std::vector<std::uint8_t> tab;      // OPTION tab tiles
    std::vector<std::uint8_t> icons;    // R bar icons
    std::vector<std::uint8_t> roster_icons;   // the Organize grid's R menu icons
    std::uint32_t bg_pal[16][16] = {};
    std::uint32_t obj_pal[16] = {};
    std::uint32_t obj_pals[16][16] = {};   // every OBJ bank, for the R bar's icons
    std::uint8_t icon_banks[0x4C] = {};    // their banks, a nibble a tile (low first)
    const std::uint8_t* rom = nullptr;
    std::uint32_t rom_size = 0;
    GameOption opts[kGameOptions];
};

std::uint32_t bgr555(std::uint16_t c) {
    const std::uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3,
                        b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) | (b | (b >> 5));
}

bool archive(const std::uint8_t* rom, std::uint32_t size, std::uint32_t a,
             std::vector<std::uint8_t>* out) {
    const int n = a7_count(rom, size, a);
    if (n <= 0) return false;
    out->clear();
    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> e;
        if (!a7_entry(rom, size, a, i, &e)) return false;
        out->insert(out->end(), e.begin(), e.end());
    }
    return true;
}

// The next string at or after `*pos` that starts with prefix (p0, 0x00):
// its codes up to 0x8D. A 0x81 NN escape (a pre-rendered name) is kept as
// `strip`. Advances `*pos` past the string's 0xFF.
bool next_string(const std::uint8_t* rom, std::uint32_t size, std::uint32_t* pos,
                 std::uint8_t p0, Text* out) {
    std::uint32_t o = *pos - kRomBase;
    const std::uint32_t end = std::min<std::uint32_t>(size, o + 0x800u);
    while (o + 1 < end && !(rom[o] == p0 && rom[o + 1] == 0x00)) ++o;
    if (o + 1 >= end) return false;
    o += 2;
    out->codes.clear();
    out->strip = -1;
    while (o < end && rom[o] != 0x8Du) {
        if (rom[o] == 0x81u && o + 1 < end) {
            out->strip = rom[o + 1];
            o += 2;
            continue;
        }
        out->codes.push_back(rom[o++]);
    }
    while (o < end && rom[o] != 0xFFu) ++o;
    *pos = kRomBase + o + 1;
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
    a.rom = rom;
    a.rom_size = size;
    if (!archive(rom, size, kBaseArchive, &a.bg) || a.bg.size() < 0xDAu * 32u) return a;
    if (!archive(rom, size, kOptionTab, &a.tab) || a.tab.size() < 4u * 32u) return a;
    if (!archive(rom, size, kRosterIconArchive, &a.roster_icons) ||
        a.roster_icons.size() < (kRosterIconTile + 12u) * 32u) return a;
    if (!archive(rom, size, kIconArchive, &a.icons) ||
        a.icons.size() < (kTrainingIcon + 1u) * 128u) return a;
    for (std::uint32_t addr : {kHandTiles + 64u, kArrowTile + 32u, kGlyphs + 0x8Au * 64u})
        if (addr - kRomBase > size) return a;
    for (int b = 0; b < 16; ++b)
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t o = kBgPalette - kRomBase + (b * 16 + i) * 2u;
            a.bg_pal[b][i] = bgr555(static_cast<std::uint16_t>(rom[o] | (rom[o + 1] << 8)));
        }
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t o = kObjPalette - kRomBase + i * 2u;
        a.obj_pal[i] = bgr555(static_cast<std::uint16_t>(rom[o] | (rom[o + 1] << 8)));
    }
    for (int b = 0; b < 16; ++b)
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t o = kObjPalette - kRomBase + (b * 16 + i) * 2u;
            a.obj_pals[b][i] = bgr555(static_cast<std::uint16_t>(rom[o] | (rom[o + 1] << 8)));
        }
    if (kIconBanks - kRomBase + sizeof a.icon_banks <= size) {
        std::memcpy(a.icon_banks, rom + (kIconBanks - kRomBase), sizeof a.icon_banks);
    }
    std::uint32_t np = kNames, hp = kHelps;
    for (int k = 0; k < kGameOptions; ++k) {
        GameOption& g = a.opts[k];
        const int count = rom[kOptionDefs - kRomBase + k * 8u + 1u];
        if (count < 1 || count > 8) return a;
        if (!next_string(rom, size, &np, 0x11u, &g.name)) return a;
        if (!next_string(rom, size, &hp, 0x21u, &g.help)) return a;
        g.values.resize(count);
        g.value_help.resize(count);
        for (int v = 0; v < count; ++v) {
            if (!next_string(rom, size, &np, 0x11u, &g.values[v])) return a;
            if (!next_string(rom, size, &hp, 0x21u, &g.value_help[v])) return a;
        }
    }
    a.ok = true;
    if (std::getenv("KOL_MENU_DUMP_TEXT")) {
        auto ascii = [](const Text& t) {
            std::string s;
            for (std::uint8_t c : t.codes) {
                if (c < 26) s += static_cast<char>('A' + c);
                else if (c < 52) s += static_cast<char>('a' + c - 26);
                else if (c >= 0x34 && c <= 0x3D) s += static_cast<char>('0' + c - 0x34);
                else if (c == kSpace) s += ' ';
                else if (c == kNewline) s += '|';
                else { char b[8]; std::snprintf(b, sizeof b, "<%02X>", c); s += b; }
            }
            if (t.strip >= 0) { char b[16]; std::snprintf(b, sizeof b, "<strip %02X>", t.strip); s += b; }
            return s;
        };
        for (const GameOption& g : a.opts) {
            std::fprintf(stderr, "menu: %s = ", ascii(g.name).c_str());
            for (const Text& v : g.values) std::fprintf(stderr, "[%s] ", ascii(v).c_str());
            std::fprintf(stderr, "\n      help: %s\n", ascii(g.help).c_str());
            for (const Text& v : g.value_help) std::fprintf(stderr, "      value help: %s\n", ascii(v).c_str());
        }
    }
    return a;
}

// Plain ASCII in the game's character codes (the font's glyph order).
Text encode(const char* s) {
    Text t;
    for (; *s; ++s) {
        const char ch = *s;
        std::uint8_t c = kSpace;
        if (ch >= 'A' && ch <= 'Z') c = static_cast<std::uint8_t>(ch - 'A');
        else if (ch >= 'a' && ch <= 'z') c = static_cast<std::uint8_t>(0x1A + ch - 'a');
        else if (ch >= '0' && ch <= '9') c = static_cast<std::uint8_t>(0x34 + ch - '0');
        else if (ch == '\n') c = kNewline;
        else {
            static const char kPunct[] = "!?.,:&_%'()<>+-*/=";
            const char* p = std::strchr(kPunct, ch);
            if (p && ch) c = static_cast<std::uint8_t>(0x3E + (p - kPunct));
            else if (ch == '"') c = 0x52;
        }
        t.codes.push_back(c);
    }
    return t;
}

// ── Drawing ──────────────────────────────────────────────────────────────
class Canvas {
public:
    Canvas(const Art& a, std::uint32_t* px, int w, int h) : a_(a), px_(px), w_(w), h_(h) {}
    void set_clip(int y0, int y1) { cy0_ = y0; cy1_ = y1; }
    void clear_clip() { cy0_ = 0; cy1_ = h_; }

    void tile4(int x0, int y0, const std::uint8_t* src, const std::uint32_t* pal,
               bool hflip = false, bool vflip = false) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int sx = hflip ? 7 - x : x, sy = vflip ? 7 - y : y;
                const std::uint8_t b = src[sy * 4 + sx / 2];
                const int i = (sx & 1) ? (b >> 4) : (b & 0xF);
                put(x0 + x, y0 + y, i, pal);
            }
    }
    void bg(int col, int row, int t, int bank) {
        tile4(col * 8, row * 8, &a_.bg[static_cast<std::size_t>(t) * 32u], a_.bg_pal[bank]);
    }
    // A window as the game draws it: frame 0x11-0x18 around the weave.
    void window(int c0, int r0, int c1, int r1) {
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                int t;
                if (r == r0) t = c == c0 ? 0x11 : c == c1 ? 0x12 : 0x15;
                else if (r == r1) t = c == c0 ? 0x13 : c == c1 ? 0x14 : 0x16;
                else if (c == c0) t = 0x17;
                else if (c == c1) t = 0x18;
                else t = 0x05 + ((c - c0 - 1) + 2 * ((r - r0 - 1) & 1)) % 4;
                bg(c, r, t, kFrameBank);
            }
    }
    void option_tab(int col, int row) {
        for (int k = 0; k < 4; ++k)
            tile4((col + k) * 8, row * 8, &a_.tab[static_cast<std::size_t>(k) * 32u],
                  a_.bg_pal[kTabBank]);
    }
    void help_tab(int col, int row) {
        for (int k = 0; k < 4; ++k) bg(col + k, row, 0xD6 + k, kFrameBank);
    }
    void hand(int x, int y) {
        const std::uint8_t* h = a_.rom + (kHandTiles - kRomBase);
        tile4(x, y, h, a_.obj_pal);
        tile4(x + 8, y, h + 32, a_.obj_pal);
    }
    void arrow(int x, int y, bool down) {
        tile4(x, y, a_.rom + (kArrowTile - kRomBase), a_.obj_pal, false, down);
    }
    // Icon k of the R bar as the menu draws it (0x08127F7C): a bank a tile,
    // a nibble each in the table before the archive (Save is bank 1, its
    // arrow red; Load bank 0). Training alone is two 8x16 sprites (template
    // 0x0861351C): its tiles in column order, the left column bank 0 and the
    // right bank 1, which is its blue and red. `dim`: greyed, each colour its
    // brightness x 0.6 (the ROM's OBJ banks 12-15 are not the menu's dimmed
    // copies: those are halved at run time; read from ROM they drew Save red
    // and Load yellow).
    // Icon k (Item, Spell, Class) of the Organize grid's R menu, in OBJ
    // bank 0 (greyed as icon() greys).
    void roster_icon(int k, int x, int y, bool dim = false) {
        const std::uint8_t* t = &a_.roster_icons[static_cast<std::size_t>(kRosterIconTile + 4 * k) * 32u];
        std::uint32_t grey[16];
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t v = a_.obj_pals[0][i];
            const unsigned l = static_cast<unsigned>(std::lround(
                0.6f * (0.299f * ((v >> 16) & 0xFF) + 0.587f * ((v >> 8) & 0xFF) + 0.114f * (v & 0xFF))));
            grey[i] = (v & 0xFF000000u) | (l << 16) | (l << 8) | l;
        }
        for (int q = 0; q < 4; ++q)
            tile4(x + (q & 1) * 8, y + (q >> 1) * 8, t + q * 32, dim ? grey : a_.obj_pals[0]);
    }
    void icon(int k, int x, int y, bool dim = false) {
        const std::uint8_t* t = &a_.icons[static_cast<std::size_t>(k) * 128u];
        const bool columns = k == kTrainingIcon;
        for (int q = 0; q < 4; ++q) {
            const int tile = k * 4 + q;
            const std::uint8_t b = tile / 2 < static_cast<int>(sizeof a_.icon_banks) ? a_.icon_banks[tile / 2] : 0;
            const int bank = (tile & 1) ? (b >> 4) : (b & 0xF);
            std::uint32_t grey[16];
            if (dim)
                for (int i = 0; i < 16; ++i) {
                    const std::uint32_t v = a_.obj_pals[bank][i];
                    const unsigned l = static_cast<unsigned>(std::lround(
                        0.6f * (0.299f * ((v >> 16) & 0xFF) + 0.587f * ((v >> 8) & 0xFF) +
                                0.114f * (v & 0xFF))));
                    grey[i] = (v & 0xFF000000u) | (l << 16) | (l << 8) | l;
                }
            const int col = columns ? q >> 1 : q & 1, row = columns ? q & 1 : q >> 1;
            tile4(x + col * 8, y + row * 8, t + q * 32, dim ? grey : a_.obj_pals[bank]);
        }
    }
    int advance(std::uint8_t c) const {
        if (c == kSpace) return 4;
        const std::uint32_t o = kGlyphWidths - kRomBase + c;
        return o < a_.rom_size ? a_.rom[o] : 6;
    }
    int width(const Text& t) const {
        if (t.strip >= 0) {
            std::uint32_t addr = 0;
            return name_strip_raw(t.strip, &addr) * 8;
        }
        int w = 0;
        for (std::uint8_t c : t.codes) {
            if (c == kNewline) break;
            w += advance(c);
        }
        return w;
    }
    void glyph(std::uint32_t addr, int x0, int y0) {
        if (addr - kRomBase + 64u > a_.rom_size) return;
        const std::uint8_t* g = a_.rom + (addr - kRomBase);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = g[y * 4 + x / 2];
                put(x0 + x, y0 + y, (x & 1) ? (b >> 4) : (b & 0xF), a_.bg_pal[kFrameBank]);
            }
    }
    // One line of text (up to a newline); returns the width.
    int text(const Text& t, int x, int y) {
        if (t.strip >= 0) {
            std::uint32_t addr = 0;
            const int cells = name_strip_raw(t.strip, &addr);
            for (int c = 0; c < cells; ++c) glyph(addr + c * 64u, x + c * 8, y);
            return cells * 8;
        }
        const int x0 = x;
        for (std::uint8_t c : t.codes) {
            if (c == kNewline) break;
            if (c != kSpace) glyph(kGlyphs + c * 64u, x, y);
            x += advance(c);
        }
        return x - x0;
    }
    std::uint32_t* px() { return px_; }
    // ARGB pixels as they are (a picture), clipped as the rest.
    void blit(int x0, int y0, const std::uint32_t* src, int w, int h) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) dot(x0 + x, y0 + y, src[y * w + x]);
    }
    // A rectangle's outline, `t` pixels thick.
    void frame(int x0, int y0, int w, int h, int t, std::uint32_t argb) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (x < t || y < t || x >= w - t || y >= h - t) dot(x0 + x, y0 + y, argb);
    }

private:
    void dot(int x, int y, std::uint32_t argb) {
        if (x < 0 || x >= w_ || y < cy0_ || y >= cy1_ || y >= h_) return;
        px_[y * w_ + x] = argb;
    }
    void put(int x, int y, int i, const std::uint32_t* pal) {
        if (!i || x < 0 || x >= w_ || y < cy0_ || y >= cy1_ || y >= h_) return;
        px_[y * w_ + x] = pal[i];
    }
    const Art& a_;
    std::uint32_t* px_;
    int w_, h_;
    int cy0_ = 0, cy1_ = 1 << 30;
};

// ── The rows ─────────────────────────────────────────────────────────────
enum class Kind : std::uint8_t { Game, Heading, Setting, View, Blur, Occlusion, Shadows, Sound, Music,
                                 Effects, Map, Page, Back, Preset, Rumble,
                                 Cheevo,     // the Achievements page's rows (page 8)
                                 Mod,        // a mod (a ROM patch): the platform's dialog
                                 UnlinkRom,     // Debug: back to the setup screen
                                 Licenses,      // the third-party notices (the platform shows them)
                                 Quit };        // a desktop: ends the game, after a question
struct Row {
    Kind kind;
    int id;               // game option index, or the Setting
    const char* name;     // ours, in ASCII
    const char* help;
};

const Row kMain[] = {
    {Kind::Game, 0, nullptr, nullptr}, {Kind::Game, 1, nullptr, nullptr},
    {Kind::Game, 2, nullptr, nullptr}, {Kind::Game, 3, nullptr, nullptr},
    {Kind::Game, 4, nullptr, nullptr}, {Kind::Game, 5, nullptr, nullptr},
    {Kind::Game, 6, nullptr, nullptr}, {Kind::Game, 7, nullptr, nullptr},
    {Kind::Game, 8, nullptr, nullptr}, {Kind::Game, 9, nullptr, nullptr},
    {Kind::Game, 10, nullptr, nullptr}, {Kind::Game, 11, nullptr, nullptr},
    {Kind::Heading, 0, "- Enhancements -", nullptr},
    {Kind::Preset, 0, "Preset", "Sets the other options at once:\nEnhanced, or the game as it was."},
    {Kind::Page, 4, "Graphics", "View size and each graphics\neffect, on or off."},
    {Kind::Page, 2, "Audio", "Sound quality, volumes\nand effects."},
    {Kind::Page, 5, "Gameplay", "Controls, rumble and\nquicker battles."},
    {Kind::Page, 6, "Cheats", "Shortcuts past the\ngame's rules."},
    {Kind::Page, 7, "Save States", "Every numbered state: tap one\nto load it. L3+R3 opens it."},
    {Kind::Page, 8, "Achievements", "RetroAchievements: log in and\nsee what you have unlocked."},
    {Kind::Mod, 0, "Mod", "Load a mod file made\nfor this game."},
    {Kind::Licenses, 0, "Licenses", "The licenses of the software\nthis app is built with."},
#if !defined(__ANDROID__)
    // A computer with a controller has no gesture that closes the game (the
    // Steam Deck in Desktop Mode); Android has its own.
    {Kind::Quit, 0, "Quit Game", "Close the game. Save first:\nunsaved progress is lost."},
#endif
#if KOL_DEV_TOOLS
    {Kind::Page, 1, "Debug", "Tools for testing."},   // (a public build has no Debug page: kol_dev.h)
#endif
};
const Row kGraphics[] = {
    {Kind::Back, 0, "Back", "Return to the options."},
    {Kind::View, 0, "View Size", "Size of the battlefield view.\nWider shows more of the map."},
    {Kind::Setting, static_cast<int>(Setting::Perspective), "Perspective",
     "The battlefield shrinks with\ndistance, like a real camera."},
    {Kind::Blur, 0, "Depth of Field", "Blurs terrain far from the\ncursor, this strongly."},
    {Kind::Occlusion, 0, "Ambient Occl.", "Shades ground near higher\nground. Not at Original size."},
    {Kind::Shadows, 0, "Shadows", "Higher ground casts shadows,\nsun at the top right."},
    {Kind::Setting, static_cast<int>(Setting::Torchlight), "Torchlight",
     "Torches cast a warm, flickering\nlight. Not at Original size."},
    {Kind::Setting, static_cast<int>(Setting::BumpMap), "Bump Mapping",
     "Wet floors glint in the lamps'\nlight. Needs Torchlight."},
    {Kind::Setting, static_cast<int>(Setting::Water), "Water",
     "Water ripples and reflects the\nshore. Not at Original size."},
    {Kind::Setting, static_cast<int>(Setting::SpecialEffects), "Special Effects",
     "Fire and smoke drawn anew.\nNeeds Lifted Props. Experimental."},
    {Kind::Setting, static_cast<int>(Setting::UnitShadows), "Unit Shadows",
     "Units cast shadows and sit in\nshade, with Shadows or AO on."},
    {Kind::Setting, static_cast<int>(Setting::LiftProps), "Lifted Props",
     "Trees and statues stand off\nthe ground. Heavy to run."},
    {Kind::Setting, static_cast<int>(Setting::LiftWalls), "Lifted Walls",
     "Pillar and wall edges show the\nwall behind. Heavy to run."},
    {Kind::Setting, static_cast<int>(Setting::PlantSway), "Plant Sway",
     "Trees and bushes move with the\nwind. Needs Lifted Props."},
    {Kind::Setting, static_cast<int>(Setting::Highlight), "Clear Move Range",
     "Movement range tiles are\nsee-through."},
    {Kind::Setting, static_cast<int>(Setting::WeatherRepeat), "Wide Weather",
     "Rain and snow fall across\nthe whole widened view."},
    {Kind::Setting, static_cast<int>(Setting::OwnRain), "New Rain",
     "Rain falls in the world and\nsplashes where it lands."},
    {Kind::Setting, static_cast<int>(Setting::WetRain), "Wet Rain",
     "Puddles grow as it rains and\ndry after; walls drip."},
    {Kind::Setting, static_cast<int>(Setting::OwnSnow), "New Snow",
     "Snow falls in the world and\nmelts where it lands."},
    {Kind::Setting, static_cast<int>(Setting::SmoothBoxes), "Smooth Boxes",
     "Dialog boxes open and close a\nlittle every frame."},
    {Kind::Setting, static_cast<int>(Setting::SmoothSky), "Smooth Sky",
     "The sky's colours blend\nwithout bands."},
    {Kind::Setting, static_cast<int>(Setting::SmoothPacing), "Smooth Pacing",
     "Frames at the screen's 60 Hz;\nthe game runs 0.5% faster."},
};
const Row kGameplay[] = {
    {Kind::Back, 0, "Back", "Return to the options."},
    {Kind::Setting, static_cast<int>(Setting::SwapFace), "Swap Face Buttons",
     "A and B trade places,\nand X and Y."},
    {Kind::Setting, static_cast<int>(Setting::AnalogCursor), "Analog Cursor",
     "The left stick moves the hand\nfreely; it settles on a tile."},
    {Kind::Rumble, 0, "Rumble", "The handheld shakes with the\ngame's thunder, this strongly."},
    {Kind::Setting, static_cast<int>(Setting::FastLoading), "Fast Loading",
     "Screens load quicker than\nthe cartridge did."},
    {Kind::Setting, static_cast<int>(Setting::FastActions), "Fast Actions",
     "No pause before each\nbattle action."},
    {Kind::Setting, static_cast<int>(Setting::FastWalk), "Fast Walking",
     "Units walk twice as fast,\nevery side."},
    {Kind::Setting, static_cast<int>(Setting::QuickEnemy), "Quick Enemy Turns",
     "The enemy decides its move\nat once."},
    {Kind::Setting, static_cast<int>(Setting::FastAttack), "Fast Attacks",
     "A strike plays twice as\nfast, every side."},
    {Kind::Setting, static_cast<int>(Setting::FastSpells), "Fast Spells",
     "A spell or an item's use\nplays twice as fast."},
};
const Row kCheats[] = {
    {Kind::Back, 0, "Back", "Return to the options."},
    {Kind::Setting, static_cast<int>(Setting::ShopAnywhere), "Shop Anywhere",
     "The Shop button works on the\nworld map away from towns."},
    {Kind::Setting, static_cast<int>(Setting::DebugNoEncounters), "No Random Battles",
     "No random battles when\ntravelling on the world map."},
    {Kind::Setting, static_cast<int>(Setting::Rewind), "Rewind",
     "L2 goes back a second at a\ntime, as held. Uses memory."},
    // (No Numbered States row: always on, the only way a player makes states; the
    // user, 2026-11-16, wanted it not to be a setting.)
};
const Row kSoundPage[] = {
    {Kind::Back, 0, "Back", "Return to the options."},
    {Kind::Sound, 0, "Sound", "HQ replays the music cleanly.\nHD: high-definition instruments."},
    {Kind::Music, 0, "Music Volume", "Volume of the music.\nNeeds HQ sound."},
    {Kind::Effects, 0, "Effects Volume", "Volume of sound effects.\nNeeds HQ sound."},
    {Kind::Setting, static_cast<int>(Setting::EffectsPlaced), "Placed Effects",
     "Sound effects come from where\nthey happen. Needs HQ sound."},
    {Kind::Setting, static_cast<int>(Setting::SoftTones), "Soft Tones",
     "Softens the square wave and\nnoise channels."},
};
const Row kDebug[] = {
    {Kind::Back, 0, "Back", "Return to the options."},
    {Kind::Setting, static_cast<int>(Setting::DebugMesh), "Perspective Mesh",
     "Paints the perspective's\nsurfaces: green flat, red upright."},
    {Kind::Setting, static_cast<int>(Setting::DebugAutoWin), "Instant Win",
     "Every battle is won at\nits first turn."},
    {Kind::Setting, static_cast<int>(Setting::DebugSunStick), "L2 Moves Sun",
     "Hold L2: the right stick\narcs the sun. Needs Shadows."},
    {Kind::Map, 0, "Battle Map",
     "Training and random battles\nare fought on this map. L/R: 10."},
    {Kind::Map, 1, "Scene Map",
     "Story scenes play on this map\n(New Game). D-pad: camera."},
    {Kind::Setting, static_cast<int>(Setting::DebugTileInfo), "Tile Info",
     "The target tile, its heights\nand its record, on screen."},
    {Kind::Page, 3, "Layers", "Hide the picture's layers\none by one."},
    {Kind::Setting, static_cast<int>(Setting::DebugNoMenuDim), "No Menu Dimming",
     "The top screen stays bright\nwhile this menu is open."},
    {Kind::Page, 7, "Save States", "Every numbered state: tap one\nto load it. L3+R3 opens it."},
    {Kind::Setting, static_cast<int>(Setting::DebugNoSecondScreen), "No Second Screen",
     "As a device with one screen:\nBACK pauses, this menu on top."},
    {Kind::UnlinkRom, 0, "Unlink ROM",
     "Restarts the game on the\nsetup screen. Saves kept."},
};
// Page 7, Save States, draws its own grid (states_render); its one row is
// for the pad code that reads rows().
const Row kStatesPage[] = {
    {Kind::Back, 0, "Back", kol::kDevTools ? "Return to Debug." : "Return to the options."},
};
const Row kLayers[] = {
    {Kind::Back, 0, "Back", "Return to Debug."},
    {Kind::Setting, static_cast<int>(Setting::DebugHideBG0), "Hide BG0",
     "Battle: the map's ground.\nMenus: varies."},
    {Kind::Setting, static_cast<int>(Setting::DebugHideBG1), "Hide BG1",
     "Battle: the map's props and\nupper pieces."},
    {Kind::Setting, static_cast<int>(Setting::DebugHideBG2), "Hide BG2",
     "Battle: interface text and\npanels."},
    {Kind::Setting, static_cast<int>(Setting::DebugHideBG3), "Hide BG3",
     "Battle: interface frames."},
    {Kind::Setting, static_cast<int>(Setting::DebugHideOBJ), "Hide Sprites",
     "Units, cursors, icons and\nweather."},
};

// Page 8, Achievements (kol_cheevos.h): rows made from the account's state
// and the game's list, rebuilt when that changes. Ids: 0 the account (A logs
// in), 1 Log Out, 2 the progress, 100 + i the list's achievements.
constexpr int kCheevoAccount = 0, kCheevoLogout = 1, kCheevoProgress = 2, kCheevoEntry = 100;
std::vector<Row> g_ra_rows;
std::vector<std::unique_ptr<std::string>> g_ra_text;   // the rows' names and helps
std::vector<CheevosEntry> g_ra_list;
int g_ra_built = -1;

const char* ra_keep(std::string s) {
    g_ra_text.push_back(std::make_unique<std::string>(std::move(s)));
    return g_ra_text.back()->c_str();
}

void ra_rebuild() {
    const int v = cheevos_version();
    if (v == g_ra_built && !g_ra_rows.empty()) return;
    g_ra_built = v;
    g_ra_rows.clear();
    g_ra_text.clear();
    const CheevosStatus st = cheevos_status();
    g_ra_list = cheevos_list();
    g_ra_rows.push_back({Kind::Back, 0, "Back", "Return to the options."});
    std::string help;
    switch (st.state) {
        case CheevosState::LoggedIn:
            help = "Logged in. Softcore only:\nhardcore is not offered here.";
            break;
        case CheevosState::LoggingIn: help = "Logging in..."; break;
        case CheevosState::Failed: help = st.message + " Press A to try again."; break;
        default: help = "Press A to log in with your\nRetroAchievements account."; break;
    }
    g_ra_rows.push_back({Kind::Cheevo, kCheevoAccount, "Account", ra_keep(help)});
    if (st.state == CheevosState::LoggedIn) {
        g_ra_rows.push_back({Kind::Cheevo, kCheevoLogout, "Log Out", "Forget this account on this\ndevice (the token is deleted)."});
        std::string ph;
        if (st.game_loaded) {
            ph = std::to_string(st.points_unlocked) + " of " + std::to_string(st.points_total) +
                 " points.\nYour softcore score: " + std::to_string(st.score) + ".";
        } else {
            ph = st.message.empty() ? std::string("Loading the game's\nachievements...") : "No achievements: " + st.message;
        }
        g_ra_rows.push_back({Kind::Cheevo, kCheevoProgress, "Unlocked", ra_keep(ph)});
        if (!g_ra_list.empty()) g_ra_rows.push_back({Kind::Heading, 0, "- Achievements -", nullptr});
        for (std::size_t i = 0; i < g_ra_list.size(); ++i)
            g_ra_rows.push_back({Kind::Cheevo, kCheevoEntry + static_cast<int>(i),
                                 ra_keep(g_ra_list[i].title), ra_keep(g_ra_list[i].description)});
    }
}

// ── State ────────────────────────────────────────────────────────────────
MenuPlatform g_platform;
bool g_mod_pending = false;   // a mod picked this session, played from the next start
std::mutex g_mu;
bool g_open = false;
std::atomic<bool> g_dirty{false};   // the picture needs sending again
struct Dialog {
    bool up = false;
    std::string title, text;
    std::vector<std::string> buttons;
    int sel = 0;        // the pad's button
    int pressed = -1;   // the button a finger is on
    int cancel = -1;    // the button B presses (-1: B closes with -1)
    std::function<void(int)> done;
};
Dialog g_dlg;   // under g_mu
int g_page = 0;            // 0 the options, 1 Debug, 2 Audio, 3 Layers (in Debug), 4 Graphics,
                           // 5 Gameplay, 6 Cheats, 7 Save States (in Debug)
int g_return_sel = 0;      // the main page's row that opened a page
int g_states_parent = 0;   // the page Save States was opened from (0 main, 1 Debug)
int g_sel = 0;             // the selected row
bool g_value_help = false; // show the value's HELP rather than the option's
float g_scroll = 0.0f;     // pixels
float g_down_x = 0, g_down_y = 0, g_down_scroll = 0;
bool g_dragging = false, g_pressed = false;
// Save States (page 7): the selected state's place in the list, the grid's
// scroll in pixels, and the pictures made so far (by state number).
int g_st_sel = 0;
float g_st_scroll = 0.0f;
int g_st_armed = -1;   // the state a hold (or Select) marked for deleting, by number
std::chrono::steady_clock::time_point g_st_down;   // when the finger went down
struct Thumb { int w = 0, h = 0; std::vector<std::uint32_t> argb; };
std::map<int, Thumb> g_thumbs;
// The grid outside the picture: the second screen scrolls it (menu_states_*).
// Where this side last asked the grid to be (pad, page opened) and a count of
// those asks; what the rows show, counted; the thumbnail thread at work.
#if defined(__ANDROID__)
bool g_st_rows_outside = true;
#else
bool g_st_rows_outside = false;
#endif
float g_st_target = 0.0f;
int g_st_target_seq = 0;
int g_st_version = 0;
int g_st_shown[4] = {-1, -1, -1, -1};   // selected, marked, states, thumbnails
bool g_st_worker = false;

// Queued writes to the game's eight bytes (mask bit i: byte i).
std::uint8_t g_pending[8] = {};
std::uint8_t g_pending_mask = 0;

const Row* rows(int* n) {
    // A public build (kol_dev.h): no Debug row, and none of its pages (their
    // tables are then referenced nowhere and left out of the binary).
    // Save States (page 7, L3+R3) is in both (the user, 2026-11-16, asked for L3 +
    // R3 outside the dev build too).
    if constexpr (kol::kDevTools) {
        if (g_page == 1) { *n = static_cast<int>(sizeof kDebug / sizeof kDebug[0]); return kDebug; }
        if (g_page == 3) { *n = static_cast<int>(sizeof kLayers / sizeof kLayers[0]); return kLayers; }
    } else if (g_page == 1 || g_page == 3) {
        g_page = 0;
    }
    if (g_page == 7) { *n = 1; return kStatesPage; }
    // (Rebuilt only where nothing holds a row: menu_render's start, ra_rebuild.)
    if (g_page == 8) {
        if (g_ra_rows.empty()) ra_rebuild();
        *n = static_cast<int>(g_ra_rows.size());
        return g_ra_rows.data();
    }
    if (g_page == 2) { *n = static_cast<int>(sizeof kSoundPage / sizeof kSoundPage[0]); return kSoundPage; }
    if (g_page == 4) { *n = static_cast<int>(sizeof kGraphics / sizeof kGraphics[0]); return kGraphics; }
    if (g_page == 5) { *n = static_cast<int>(sizeof kGameplay / sizeof kGameplay[0]); return kGameplay; }
    if (g_page == 6) { *n = static_cast<int>(sizeof kCheats / sizeof kCheats[0]); return kCheats; }
    *n = static_cast<int>(sizeof kMain / sizeof kMain[0]);
    return kMain;
}

// Layout, in canvas pixels. The OPTION window takes tile rows 0-15 (its tab
// over the top edge, as the game draws it) and shows seven whole rows, as the
// game's shows four; the HELP window takes rows 16-22, two lines (every help
// is two: the game's 45, KOL_MENU_DUMP_TEXT, and ours).
constexpr int kWinTop = 0, kWinBottomRow = 15;
constexpr int kListTop = 8, kListBottom = 120;   // the rows' visible band
constexpr int kRowH = 16;
constexpr int kLabelX = 24, kValueX = 144 + 8 * (kSheetCols - 30);   // the 31st column between them
constexpr int kHelpTopRow = 16;

std::uint8_t settings_byte(int i) {
    if (g_pending_mask & (1u << i)) return g_pending[i];
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    return ew ? ew[kSettings - 0x02000000u + i] : kDefaults[i < 7 ? i : 0];
}

void queue_byte(int i, std::uint8_t v) {
    g_pending[i] = v;
    g_pending_mask = static_cast<std::uint8_t>(g_pending_mask | (1u << i));
}

int game_value(int k) {
    if (k <= 2) return settings_byte(k);
    if (k <= 7) return (settings_byte(3) >> (k - 3)) & 1 ? 0 : 1;
    if (k <= 10) return settings_byte(k - 4);
    return 1;   // Default: "NO" until chosen
}

void set_game_value(int k, int v) {
    if (k <= 2) queue_byte(k, static_cast<std::uint8_t>(v));
    else if (k <= 7) {
        std::uint8_t b = settings_byte(3);
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (k - 3));
        b = v == 0 ? static_cast<std::uint8_t>(b | bit) : static_cast<std::uint8_t>(b & ~bit);
        queue_byte(3, b);
    } else if (k <= 10) queue_byte(k - 4, static_cast<std::uint8_t>(v));
    else if (v == 0)    // "YES": restore the defaults
        for (int i = 0; i < 7; ++i) queue_byte(i, kDefaults[i]);
}

bool selectable(const Row& r) { return r.kind != Kind::Heading; }

float max_scroll();
void states_scroll_to(float scroll);
void follow_selection();

// Back to the main page, the hand on the row that opened the page.
void leave_page() {
    // Layers goes back to Debug; Save States to where it was opened from
    // (Debug's row, else the main page's: its own row, or L3+R3).
    if (kol::kDevTools && (g_page == 3 || (g_page == 7 && g_states_parent == 1))) {
        const int from = g_page;
        g_page = 1;
        g_sel = 0;
        if constexpr (kol::kDevTools)
            for (int i = 0; i < static_cast<int>(sizeof kDebug / sizeof kDebug[0]); ++i)
                if (kDebug[i].kind == Kind::Page && kDebug[i].id == from) g_sel = i;
        g_scroll = 0;
        follow_selection();
        return;
    }
    g_page = 0;
    g_sel = g_return_sel;
    g_scroll = 0;
    follow_selection();
}

// The value shown for a row, and a step forward.
Text value_text(const Art& a, const Row& r) {
    char buf[40];
    switch (r.kind) {
        case Kind::Game: {
            const GameOption& g = a.opts[r.id];
            const int v = std::min<int>(game_value(r.id), static_cast<int>(g.values.size()) - 1);
            return g.values[std::max(v, 0)];
        }
        case Kind::Setting:
            return a.opts[3].values[setting_on(static_cast<Setting>(r.id)) ? 0 : 1];  // ON / OFF
        case Kind::View: {
            const unsigned i = width_choice_current();
            if (width_choice_width(i) == 240u && width_choice_height(i) == 160u)
                return encode("Original");
            std::snprintf(buf, sizeof buf, "%ux%u", width_choice_width(i), width_choice_height(i));
            return encode(buf);
        }
        case Kind::Blur: {   // OFF, or on at a strength
            if (!setting_on(Setting::DepthOfField)) return a.opts[3].values[1];
            std::string s = blur_choice_label(blur_choice_current());
            const auto p = s.find(" (");
            if (p != std::string::npos) s.resize(p);
            return encode(s.c_str());
        }
        case Kind::Occlusion: {   // OFF, or its reach
            if (ao_choice_current() == 0) return a.opts[3].values[1];
            return encode(ao_choice_current() == 1 ? "Half Tile" : "Full Tile");
        }
        case Kind::Rumble:   // OFF, or its strength
            if (rumble_choice_current() == 0) return a.opts[3].values[1];
            return encode(rumble_choice_label(rumble_choice_current()));
        case Kind::Shadows:   // ON / OFF (the sun's angle is fixed, or the map's own)
            return a.opts[3].values[shadow_choice_current() != 0 ? 0 : 1];
        case Kind::Sound: {
            static const char* const kShort[] = {"Original", "HQ", "HD"};
            return encode(kShort[std::min(sound_choice_current(), 2u)]);
        }
        case Kind::Music:
        case Kind::Effects:
            std::snprintf(buf, sizeof buf, "%u%%", volume_choice_current(r.kind == Kind::Effects) * 10u);
            return encode(buf);
        case Kind::Map: {   // the game's, or the map's number, from this row's list
            const unsigned m = debug_map_current();
            if (m == 0 || debug_map_in_training(m) != (r.id == 0)) return encode("Game's");
            std::snprintf(buf, sizeof buf, "No. %u", m);
            return encode(buf);
        }
        case Kind::Preset: {   // the preset the settings match, else Custom
            const int p = preset_current();
            return encode(p < 0 ? "Custom" : preset_label(static_cast<unsigned>(p)));
        }
        case Kind::Page: return encode(">");
        case Kind::Licenses: return encode(">");
        case Kind::Quit: return encode(">");
        case Kind::Mod: {   // the patch's name without its extension, cut to fit
            const char* m = rom_patch_name();
            if (!m) return encode("None");
            std::string s = m;
            if (const auto dot = s.rfind('.'); dot != std::string::npos && dot > 0) s.resize(dot);
            if (s.size() > 12) s = s.substr(0, 11) + "...";
            return encode(s.c_str());
        }
        case Kind::Cheevo: {
            const CheevosStatus st = cheevos_status();
            if (r.id == kCheevoAccount) {
                if (st.state == CheevosState::LoggedIn) return encode(st.user.substr(0, 12).c_str());
                if (st.state == CheevosState::LoggingIn) return encode("...");
                return encode("Log In");
            }
            if (r.id == kCheevoProgress) {
                if (!st.game_loaded) return encode("-");
                std::snprintf(buf, sizeof buf, "%u/%u", st.unlocked, st.total);
                return encode(buf);
            }
            const std::size_t i = static_cast<std::size_t>(r.id - kCheevoEntry);
            if (r.id >= kCheevoEntry && i < g_ra_list.size()) {
                const CheevosEntry& e = g_ra_list[i];
                if (e.unlocked) return encode("Done");
                if (!e.progress.empty()) return encode(e.progress.c_str());
                std::snprintf(buf, sizeof buf, "%u pts", e.points);
                return encode(buf);
            }
            return Text{};
        }
        default: return Text{};
    }
}

// One step of a row's value: dir +1 forward, -1 back (the pad's left).
unsigned wrap(unsigned cur, int dir, unsigned n) {
    if (n == 0) return 0;
    return (cur + n + static_cast<unsigned>(dir < 0 ? n - 1 : 1)) % n;
}

// Under g_mu: the dialog up, the menu open with it.
void open_dialog(const char* title, const char* text, const std::vector<std::string>& buttons,
                 int cancel_button, std::function<void(int)> done) {
    g_dlg = Dialog{};
    g_dlg.up = true;
    g_dlg.title = title ? title : "";
    g_dlg.text = text ? text : "";
    g_dlg.buttons = buttons;
    g_dlg.cancel = cancel_button;
    g_dlg.done = std::move(done);
    g_open = true;
    g_dirty.store(true, std::memory_order_release);
}

// "Restart now?" after a mod change (not under g_mu).
void ask_restart(const std::string& text) {
    menu_dialog("Mod", text.c_str(), {"Restart", "Later"}, 1, [](int b) {
        MenuPlatform p;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            p = g_platform;
        }
        if (b == 0 && p.restart) p.restart();
    });
}

// Options > Mod (under g_mu): what plays, and Choose / Remove / Cancel.
void mod_dialog() {
    if (!g_platform.pick_mod) {
        open_dialog("Mod", "Start the game with --rom-patch FILE.", {"OK"}, 0, nullptr);
        return;
    }
    std::string playing = "Playing: ";
    if (const char* m = rom_patch_name()) {
        std::string n = m;
        if (const auto dot = n.rfind('.'); dot != std::string::npos && dot > 0) n.resize(dot);
        playing += n;
    } else {
        playing += "the original game";
    }
    playing += ".";
    const bool removable = rom_patch_name() || g_mod_pending;
    std::vector<std::string> buttons = {"Choose"};
    if (removable) buttons.push_back("Remove");
    buttons.push_back("Cancel");
    const int cancel = static_cast<int>(buttons.size()) - 1;
    open_dialog("Mod", playing.c_str(), buttons, cancel, [removable](int b) {
        MenuPlatform p;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            p = g_platform;
        }
        if (b == 0 && p.pick_mod) p.pick_mod();
        else if (removable && b == 1 && p.remove_mod) {
            p.remove_mod();
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_mod_pending = false;
            }
            ask_restart("Mod removed. Restart now?");
        }
    });
}

void step_value(const Art& a, const Row& r, int dir = 1) {
    switch (r.kind) {
        case Kind::Game: {
            const int n = static_cast<int>(a.opts[r.id].values.size());
            if (r.id == kDefaultOption) { if (dir > 0) set_game_value(r.id, 0); break; }
            set_game_value(r.id, static_cast<int>(wrap(static_cast<unsigned>(game_value(r.id)), dir,
                                                       static_cast<unsigned>(n))));
            break;
        }
        case Kind::Setting:
            setting_set(static_cast<Setting>(r.id), !setting_on(static_cast<Setting>(r.id)));
            break;
        case Kind::View:
            width_choice_set(wrap(width_choice_current(), dir, width_choice_count()));
            break;
        case Kind::Blur: {
            // One list: OFF (0), then each strength (1..n).
            const unsigned cur = setting_on(Setting::DepthOfField) ? blur_choice_current() + 1u : 0u;
            const unsigned next = wrap(cur, dir, blur_choice_count() + 1u);
            if (next > 0) blur_choice_set(next - 1u);
            setting_set(Setting::DepthOfField, next > 0);
            break;
        }
        case Kind::Occlusion:
            ao_choice_set(wrap(ao_choice_current(), dir, ao_choice_count()));
            break;
        case Kind::Shadows:
            shadow_choice_set(shadow_choice_current() != 0 ? 0u : 1u);
            break;
        case Kind::Rumble:
            rumble_choice_set(wrap(rumble_choice_current(), dir, rumble_choice_count()));
            kol::rumble_test();   // feel the new strength at once
            break;
        case Kind::Sound:
            sound_choice_set(wrap(sound_choice_current(), dir, sound_choice_count()));
            break;
        case Kind::Music:
        case Kind::Effects: {
            const bool fx = r.kind == Kind::Effects;
            volume_choice_set(fx, wrap(volume_choice_current(fx), dir, volume_choice_count()));
            break;
        }
        case Kind::Map: {   // the game's (0), then this row's maps; dir may be +-10
            std::vector<unsigned> list{0u};
            for (unsigned n = 1; n <= debug_map_count(); ++n)
                if (debug_map_in_training(n) == (r.id == 0)) list.push_back(n);
            const unsigned cur = debug_map_current();
            int at = 0;
            for (int i = 0; i < static_cast<int>(list.size()); ++i)
                if (list[i] == cur) at = i;
            const int n = static_cast<int>(list.size());
            debug_map_set(list[static_cast<std::size_t>(((at + dir) % n + n) % n)]);
            break;
        }
        case Kind::Preset: {   // Custom steps to the first (right) or the last (left)
            const int cur = preset_current();
            const unsigned n = preset_count();
            preset_apply(cur < 0 ? (dir > 0 ? 0u : n - 1u) : wrap(static_cast<unsigned>(cur), dir, n));
            break;
        }
        case Kind::Page:
            if (g_page == 0) g_return_sel = g_sel;   // a page's page keeps the way home
            if (r.id == 7) g_states_parent = g_page;
            g_page = r.id; g_sel = 1; g_scroll = 0;
            if (r.id == 7) { g_st_sel = 0; states_scroll_to(0); g_st_armed = -1; }
            break;
        case Kind::Back: leave_page(); break;
        case Kind::Licenses:
            if (g_platform.show_licenses) g_platform.show_licenses();
            else open_dialog("Licenses", "See android/app/src/main/assets/licenses.txt.", {"OK"}, 0, nullptr);
            break;
        case Kind::Quit:
            open_dialog("Quit Game", "Close the game? Unsaved progress is lost.",
                        {"Quit", "Cancel"}, 1, [](int b) {
                            if (b == 0 && g_platform.quit) g_platform.quit();
                        });
            break;
        case Kind::UnlinkRom:
            if constexpr (!kol::kDevTools) break;   // (a dev tool: its row is on the Debug page)
            if (!g_platform.unlink_rom) {
                open_dialog("Unlink ROM", "Only on Android: the desktop takes --rom.", {"OK"}, 0, nullptr);
                break;
            }
            open_dialog("Unlink ROM", "Restart on the setup screen? Saves are kept.",
                        {"Unlink", "Cancel"}, 1, [](int b) {
                            if (b == 0 && g_platform.unlink_rom) g_platform.unlink_rom();
                        });
            break;
        case Kind::Mod: mod_dialog(); break;
        case Kind::Cheevo:
            if (r.id == kCheevoAccount && cheevos_status().state != CheevosState::LoggedIn &&
                cheevos_status().state != CheevosState::LoggingIn)
                cheevos_request_login_form();
            else if (r.id == kCheevoLogout)
                cheevos_logout();
            break;
        default: break;
    }
}

float max_scroll() {
    int n = 0;
    rows(&n);
    return std::max(0.0f, static_cast<float>(n * kRowH - (kListBottom - kListTop)));
}  // a whole number of rows: the band is seven rows

// ── Save States (page 7) ─────────────────────────────────────────────────
// The whole canvas is one window: "Save States" across the top (a tap there
// goes back), then a grid of three across, newest first: each state's
// picture in a 72 x 40 box and, under it, its number and Alphonse's level.
// Deleting takes two steps: a hold (or Select) marks one, red with
// "Delete?", and a tap on it (or A) deletes it; anything else unmarks it.
constexpr int kStTitleY = 8, kStTop = 28, kStBottom = kSheetH - 8;
constexpr int kStCols = 3, kStCellW = 80, kStCellH = 60, kStLeft = 4;
constexpr int kStThumbW = 72, kStThumbH = 40;
// The save button at the title's right end: the bottom bar's button (its
// frame, the world map's save icon), pushed in while touched; R on the pad.
constexpr int kStSaveX = kSheetW - 30, kStSaveY = 2, kStSaveSize = 24;
bool g_st_save_down = false;   // a finger on it
void button_palette(const std::uint32_t* bar, int style, std::uint32_t* out);   // (below)
bool st_on_save(float x, float y) {
    return x >= kStSaveX && x < kStSaveX + kStSaveSize && y >= kStSaveY && y < kStSaveY + kStSaveSize;
}

float states_max_scroll(int count) {
    const int rows_n = (count + kStCols - 1) / kStCols;
    return std::max(0.0f, static_cast<float>(rows_n * kStCellH - (kStBottom - kStTop)));
}
void states_follow(int count) {
    const float top = static_cast<float>((g_st_sel / kStCols) * kStCellH);
    const float band = static_cast<float>(kStBottom - kStTop);
    if (top < g_st_scroll) g_st_scroll = top;
    if (top + kStCellH > g_st_scroll + band) g_st_scroll = top + kStCellH - band;
    g_st_scroll = std::min(std::max(g_st_scroll, 0.0f), states_max_scroll(count));
    g_st_target = g_st_scroll;
    ++g_st_target_seq;
}
// The grid goes to `scroll` (the page opened).
void states_scroll_to(float scroll) {
    g_st_scroll = g_st_target = scroll;
    ++g_st_target_seq;
}
// Loads entry i of the list and closes the menu, to show the game.
void states_pick(const std::vector<StateEntry>& list, int i) {
    if (i < 0 || i >= static_cast<int>(list.size())) return;
    states_load(list[static_cast<std::size_t>(i)].number);
    menu_confirm_sound();
    g_open = false;
    g_st_armed = -1;
}
void states_remove(int number) {
    states_delete(number);
    g_thumbs.erase(number);
    g_st_armed = -1;
}
// One state's cell, its top left at x, y. With `decode` a thumbnail not made yet
// is made here; without, it stays a black box until the thread has made it.
void states_cell(Canvas& c, const StateEntry& e, bool selected, int x, int y, bool decode) {
    auto it = g_thumbs.find(e.number);
    if (it == g_thumbs.end() && decode) {
        Thumb t;
        if (!states_thumb(e.number, kStThumbW, kStThumbH, &t.argb, &t.w, &t.h)) t = Thumb{};
        it = g_thumbs.emplace(e.number, std::move(t)).first;
    }
    const int bx = x + (kStCellW - kStThumbW) / 2, by = y + 2;
    const std::vector<std::uint32_t> black(static_cast<std::size_t>(kStThumbW * kStThumbH), 0xFF101010u);
    c.blit(bx, by, black.data(), kStThumbW, kStThumbH);
    const bool armed = e.number == g_st_armed;
    if (it != g_thumbs.end() && it->second.w > 0) {
        const Thumb& t = it->second;
        c.blit(bx + (kStThumbW - t.w) / 2, by + (kStThumbH - t.h) / 2, t.argb.data(), t.w, t.h);
        if (armed) {   // darkened, under a red frame
            std::vector<std::uint32_t> dim(t.argb);
            for (std::uint32_t& p : dim) p = 0xFF000000u | ((p >> 2) & 0x3F3F3Fu) | 0x400000u;
            c.blit(bx + (kStThumbW - t.w) / 2, by + (kStThumbH - t.h) / 2, dim.data(), t.w, t.h);
        }
    }
    if (armed) c.frame(bx - 2, by - 2, kStThumbW + 4, kStThumbH + 4, 3, 0xFFE03030u);
    else if (selected) c.frame(bx - 2, by - 2, kStThumbW + 4, kStThumbH + 4, 2, 0xFFF8D040u);
    char label[24];
    if (armed) std::snprintf(label, sizeof label, "Delete %d?", e.number);
    else if (e.level > 0) std::snprintf(label, sizeof label, "%d  Lv %d", e.number, e.level);
    else std::snprintf(label, sizeof label, "%d", e.number);
    const Text l = encode(label);
    c.text(l, x + (kStCellW - c.width(l)) / 2, y + kStThumbH + 3);
}
void states_render(Canvas& c) {
    const std::vector<StateEntry> list = states_list();
    c.window(0, 0, kSheetCols - 1, kSheetRows - 1);
    const Text title = encode(list.empty() ? "Save States"
                              : g_st_armed >= 0 ? "Tap it again to delete it."
                                                : "Tap: load. Hold: delete.");
    c.text(title, (kSheetW - c.width(title)) / 2, kStTitleY);
    c.arrow(10, kStTitleY + 4, false);   // the way back, as the pages' Back row
    {   // the save button
        const Art& a = art();
        std::uint32_t pal[16];
        button_palette(a.bg_pal[kFrameBank], g_st_save_down ? 1 : 0, pal);
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) {
                int t;
                if (r == 0) t = col == 0 ? 0x11 : col == 2 ? 0x12 : 0x15;
                else if (r == 2) t = col == 0 ? 0x13 : col == 2 ? 0x14 : 0x16;
                else t = col == 0 ? 0x17 : col == 2 ? 0x18 : 0x05;
                c.tile4(kStSaveX + col * 8, kStSaveY + r * 8, &a.bg[static_cast<std::size_t>(t) * 32u], pal);
            }
        const int d = g_st_save_down ? 1 : 0;
        c.icon(kSaveIcon, kStSaveX + 4 + d, kStSaveY + 4 + d);
    }
    if (list.empty()) {
        const Text a = encode("No numbered states yet."), b = encode("The save button or L3+R saves one,");
        const Text d = encode("L3+L loads the latest.");
        c.text(a, (kSheetW - c.width(a)) / 2, 72);
        c.text(b, (kSheetW - c.width(b)) / 2, 96);
        c.text(d, (kSheetW - c.width(d)) / 2, 112);
        return;
    }
    if (g_st_sel >= static_cast<int>(list.size())) g_st_sel = static_cast<int>(list.size()) - 1;
    if (g_st_rows_outside) return;   // the rows and the arrows are the second screen's
    g_st_scroll = std::min(g_st_scroll, states_max_scroll(static_cast<int>(list.size())));
    c.set_clip(kStTop, kStBottom);
    const int first_row = static_cast<int>(g_st_scroll) / kStCellH;
    for (int i = first_row * kStCols; i < static_cast<int>(list.size()); ++i) {
        const int y = kStTop + (i / kStCols) * kStCellH - static_cast<int>(g_st_scroll);
        if (y >= kStBottom) break;
        states_cell(c, list[static_cast<std::size_t>(i)], i == g_st_sel, kStLeft + (i % kStCols) * kStCellW, y, true);
    }
    c.clear_clip();
    const float max = states_max_scroll(static_cast<int>(list.size()));
    if (g_st_scroll > 0) c.arrow(kSheetW - 16, kStTop, false);
    if (g_st_scroll < max) c.arrow(kSheetW - 16, kStBottom - 8, true);
}
// A touch on page 7 (the same down / move / up as the pages' rows).
void states_touch(int action, float x, float y) {
    const std::vector<StateEntry> list = states_list();
    const float max = states_max_scroll(static_cast<int>(list.size()));
    if (action == 0) {
        g_pressed = true;
        g_dragging = false;
        g_down_x = x; g_down_y = y; g_down_scroll = g_st_scroll;
        g_st_down = std::chrono::steady_clock::now();
        g_st_save_down = st_on_save(x, y);
        return;
    }
    if (!g_pressed) return;
    if (g_st_save_down) {   // the save button: pushed in until the finger leaves or lifts
        if (action == 1) { g_st_save_down = st_on_save(x, y); return; }
        g_pressed = false;
        g_st_save_down = false;
        if (action == 2 && st_on_save(x, y)) {
            menu_confirm_sound();
            states_save_new();
        }
        return;
    }
    if (action == 1) {
        if (!g_dragging && std::abs(y - g_down_y) > 4.0f) g_dragging = true;
        if (g_dragging) g_st_scroll = std::min(std::max(g_down_scroll - (y - g_down_y), 0.0f), max);
        return;
    }
    g_pressed = false;
    if (action != 2 || g_dragging) { g_dragging = false; return; }
    const bool hold = std::chrono::steady_clock::now() - g_st_down >= std::chrono::milliseconds(450);
    int i = -1;
    if (y >= kStTop && y < kStBottom && x >= kStLeft && x < kStLeft + kStCols * kStCellW) {
        const int col = static_cast<int>((x - kStLeft) / kStCellW);
        const int row = static_cast<int>((y - kStTop + g_st_scroll) / kStCellH);
        i = row * kStCols + col;
        if (i >= static_cast<int>(list.size())) i = -1;
    }
    if (g_st_armed >= 0) {   // marked: a tap on it deletes it, any other unmarks it
        if (i >= 0 && list[static_cast<std::size_t>(i)].number == g_st_armed && !hold)
            states_remove(g_st_armed);
        else
            g_st_armed = -1;
        return;
    }
    if (y < kStTop) { leave_page(); return; }
    if (i < 0) return;
    g_st_sel = i;
    if (hold) g_st_armed = list[static_cast<std::size_t>(i)].number;
    else states_pick(list, i);
}

// A sound for the second screen's button, played by the game's own sound
// effect routine 0x0814506C(id) (354 calls in the ROM with a constant id:
// 0xAD confirms, as the preparation screen's dispatch does; 0xB0 is the next
// most used, taken as the cancel). It is called where the game calls it,
// in the VBlank handler right after the game's tasks have run: 0x080020B8
// (called at 0x080005F4) only flips the byte 0x030032B8. With a sound
// pending, the hook flips it itself and runs the sound routine in its place,
// which returns to the handler as 0x080020B8 would have.
constexpr std::uint32_t kPlaySound = 0x0814506Cu;
constexpr std::uint32_t kAfterTasks = 0x080020B8u;
constexpr std::uint32_t kAfterTasksFlag = 0x030032B8u;   // what it flips
constexpr char kSoundHookId[] = "kol.menu-sound";
constexpr int kSoundOpen = 0xAD, kSoundClose = 0xB0;
std::atomic<int> g_sound_request{-1};

#if !defined(__ANDROID__)
// Desktop tools for the HD music mock-ups (README "Any song, from any
// state"), both off unless their variable is set:
// KOL_PLAY_SONG=n starts song n (1..32: the music) on the third call, through
// MP2K's m4aSongNumStart 0x0815C4E8, the routine the game's sound routine
// calls once its sound bit is set; player 0 then plays it as if the game had
// asked. KOL_MP2K_DUMP=path appends, every call, what tools/mockup/sound's
// capture.py reads over TCP: SoundInfo (from [0x03007FF0]) with its channels,
// the CGB channels and IO 0x04000060..8F, one record a frame.
constexpr std::uint32_t kSongStart = 0x0815C4E8u;
int song_test_take() {
    static int left = [] {
        const char* e = std::getenv("KOL_PLAY_SONG");
        return e ? std::atoi(e) : 0;
    }();
    static int calls = 0;
    if (left <= 0 || ++calls < 3) return -1;
    const int n = left;
    left = 0;
    std::fprintf(stderr, "song test: m4aSongNumStart(%d)\n", n);
    return n;
}
void mp2k_dump() {
    static FILE* f = [] {
        const char* e = std::getenv("KOL_MP2K_DUMP");
        return e ? std::fopen(e, "wb") : nullptr;
    }();
    if (!f) return;
    std::uint8_t rec[4 + 0xFB0 + 0x100 + 0x30] = {};
    const std::uint32_t si = bus_read_u32(0x03007FF0u);
    std::memcpy(rec, &si, 4);
    if ((si >> 24) == 2) {
        for (std::uint32_t i = 0; i < 0xFB0; ++i) rec[4 + i] = bus_read_u8(si + i);
        const std::uint32_t cgb = bus_read_u32(si + 0x1C);
        if ((cgb >> 24) == 2 || (cgb >> 24) == 3)
            for (std::uint32_t i = 0; i < 0x100; ++i) rec[4 + 0xFB0 + i] = bus_read_u8(cgb + i);
    }
    for (std::uint32_t i = 0; i < 0x30; ++i) rec[4 + 0xFB0 + 0x100 + i] = bus_read_u8(0x04000060u + i);
    std::fwrite(rec, sizeof rec, 1, f);
    std::fflush(f);
}
#endif

int on_after_tasks(std::uint32_t, int, ArmCpuState* cpu) {
    if (gbarecomp::runtime_excursion_active()) return 0;
#if !defined(__ANDROID__)
    mp2k_dump();
    const int song = song_test_take();
    if (song > 0) {
        bus_write_u8(kAfterTasksFlag, bus_read_u8(kAfterTasksFlag) ^ 1u);
        cpu->R[0] = static_cast<std::uint32_t>(song);
        cpu->R[15] = kSongStart;   // returns to the handler, as below
        return 1;
    }
#endif
    const int id = g_sound_request.exchange(-1);
    if (id < 0) return 0;
    bus_write_u8(kAfterTasksFlag, bus_read_u8(kAfterTasksFlag) ^ 1u);
    cpu->R[0] = static_cast<std::uint32_t>(id);
    cpu->R[15] = kPlaySound;   // r14 is still the handler's return
    return 1;
}

void write_pending_options();

int on_frame_hook(std::uint32_t, int, ArmCpuState* cpu) {
    // Inside the game, between its own frames: apply the queued writes to
    // the game's settings (and to the game's OPTION menu, if it is open, so
    // it does not write its older copy back on close). Declines the call,
    // unless Fast Attacks skips this tick of the sound driver.
    if (gbarecomp::runtime_excursion_active()) return 0;
    write_pending_options();
    return kol::fast_attack_on_sound_main(cpu) ? 1 : 0;
}

void write_pending_options() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_pending_mask) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew) return;
    const std::uint32_t work = ew[kMenuWork - 0x02000000u] |
                               (ew[kMenuWork - 0x02000000u + 1] << 8) |
                               (ew[kMenuWork - 0x02000000u + 2] << 16) |
                               (static_cast<std::uint32_t>(ew[kMenuWork - 0x02000000u + 3]) << 24);
    const bool menu_open = work >= 0x02000000u && work + kMenuCopy + 8u < 0x02040000u;
    for (int i = 0; i < 8; ++i) {
        if (!(g_pending_mask & (1u << i))) continue;
        ew[kSettings - 0x02000000u + i] = g_pending[i];
        if (menu_open) ew[work - 0x02000000u + kMenuCopy + i] = g_pending[i];
    }
    std::fprintf(stderr, "menu: game options written (mask %02X)\n", g_pending_mask);
    g_pending_mask = 0;
}

// A button's colours: 0 as they are, 1 pressed in (a brightness decrease of 4/16,
// as the PPU computes one on 5-bit colours), 2 greyed (by their brightness).
void button_palette(const std::uint32_t* bar, int style, std::uint32_t* out) {
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t v = bar[i];
        if (style == 1) {
            std::uint32_t o = v & 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                std::uint32_t q = ((v >> sh) & 0xFFu) >> 3;
                q -= (q * 4u) >> 4;
                o |= ((q << 3) | (q >> 2)) << sh;
            }
            out[i] = o;
        } else if (style == 2) {
            const unsigned l = static_cast<unsigned>(std::lround(0.299f * ((v >> 16) & 0xFF) +
                                                                 0.587f * ((v >> 8) & 0xFF) +
                                                                 0.114f * (v & 0xFF)));
            out[i] = (v & 0xFF000000u) | (l << 16) | (l << 8) | l;
        } else {
            out[i] = v;
        }
    }
}


// ── A dialog over the menu (menu_dialog) ────────────────────────────────
// The game's window centred over the dimmed page: a title, the text wrapped,
// and buttons in the bottom bar's buttons' frame, pushed in while a finger is
// on one, as the bar's are; the hand points at the pad's choice.

struct DialogLayout {
    int c0, r0, c1, r1;                  // the window, in tiles
    std::vector<std::string> lines;      // the text, wrapped
    int text_y;
    std::vector<int> bx, bw;             // each button's x and width (pixels)
    int by;                              // the buttons' top
};
constexpr int kDlgButtonH = 24;

DialogLayout dialog_layout(Canvas& c) {
    DialogLayout L;
    L.c0 = 1; L.c1 = kSheetCols - 2;
    const int inner = (L.c1 - L.c0 + 1) * 8 - 24;
    // Words to lines (a '\n' in the text breaks one).
    std::string line, word;
    auto flush = [&](bool hard) {
        const std::string tryl = line.empty() ? word : word.empty() ? line : line + " " + word;
        if (!line.empty() && c.width(encode(tryl.c_str())) > inner) { L.lines.push_back(line); line = word; }
        else line = tryl;
        word.clear();
        if (hard) { L.lines.push_back(line); line.clear(); }
    };
    for (char ch : g_dlg.text) {
        if (ch == ' ') flush(false);
        else if (ch == '\n') flush(true);
        else word += ch;
    }
    flush(false);
    if (!line.empty()) L.lines.push_back(line);
    // Buttons: as wide as their label (+ 12 a side), in tiles, centred in a row.
    int total = 0;
    for (const std::string& b : g_dlg.buttons) {
        const int w = ((c.width(encode(b.c_str())) + 24 + 7) / 8) * 8;
        L.bw.push_back(w);
        total += w;
    }
    const int gap = 8;
    total += gap * std::max<int>(0, static_cast<int>(g_dlg.buttons.size()) - 1);
    const int body = 22 + 16 * static_cast<int>(L.lines.size()) + 6 + kDlgButtonH + 8;
    const int rows = std::min((body + 7) / 8 + 1, kSheetRows - 1);
    L.r0 = std::max(0, (kSheetRows - rows) / 2);
    L.r1 = L.r0 + rows - 1;
    L.text_y = L.r0 * 8 + 24;
    L.by = (L.r1 + 1) * 8 - 8 - kDlgButtonH;
    int x = (kSheetW - total) / 2;
    for (int w : L.bw) { L.bx.push_back(x); x += w + gap; }
    return L;
}

int dialog_button_at(Canvas& c, float x, float y) {
    const DialogLayout L = dialog_layout(c);
    for (std::size_t i = 0; i < L.bx.size(); ++i)
        if (x >= L.bx[i] && x < L.bx[i] + L.bw[i] && y >= L.by && y < L.by + kDlgButtonH)
            return static_cast<int>(i);
    return -1;
}

void dialog_render(Canvas& c, std::uint32_t* argb, const Art& a, int bob) {
    // The page under it, at half brightness.
    for (int i = 0; i < kSheetW * kSheetH; ++i)
        argb[i] = 0xFF000000u | ((argb[i] >> 1) & 0x7F7F7Fu);
    const DialogLayout L = dialog_layout(c);
    c.window(L.c0, L.r0, L.c1, L.r1);
    c.text(encode(g_dlg.title.c_str()), L.c0 * 8 + 12, L.r0 * 8 + 6);
    for (std::size_t i = 0; i < L.lines.size(); ++i)
        c.text(encode(L.lines[i].c_str()), L.c0 * 8 + 12, L.text_y + 16 * static_cast<int>(i));
    std::uint32_t up[16], down[16];
    button_palette(a.bg_pal[kFrameBank], 0, up);
    button_palette(a.bg_pal[kFrameBank], 1, down);
    for (std::size_t i = 0; i < L.bx.size(); ++i) {
        const bool pushed = g_dlg.pressed == static_cast<int>(i);
        const int cols = L.bw[i] / 8;
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < cols; ++col) {
                int t;
                if (r == 0) t = col == 0 ? 0x11 : col == cols - 1 ? 0x12 : 0x15;
                else if (r == 2) t = col == 0 ? 0x13 : col == cols - 1 ? 0x14 : 0x16;
                else t = col == 0 ? 0x17 : col == cols - 1 ? 0x18 : 0x05;
                c.tile4(L.bx[i] + col * 8, L.by + r * 8, &a.bg[static_cast<std::size_t>(t) * 32u],
                        pushed ? down : up);
            }
        const Text t = encode(g_dlg.buttons[i].c_str());
        const int off = pushed ? 1 : 0;   // the label goes in with the button, as the bar's icon does
        c.text(t, L.bx[i] + (L.bw[i] - c.width(t)) / 2 + off, L.by + 5 + off);
    }
    // The hand points at the pad's choice, bobbing as the menu's does.
    if (g_dlg.sel >= 0 && g_dlg.sel < static_cast<int>(L.bx.size()))
        c.hand(L.bx[g_dlg.sel] - kHandW - 1 + bob, L.by + 8);
}

}  // namespace

bool menu_is_open() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_open;
}

void menu_confirm_sound() { g_sound_request.store(kSoundOpen); }
void menu_cancel_sound() { g_sound_request.store(kSoundClose); }

bool menu_toggle() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_open = !g_open;
    g_sound_request.store(g_open ? kSoundOpen : kSoundClose);   // the game's confirm / cancel
    // It opens where it was closed: the same page (a submenu too), row and
    // scroll. Resetting only the page put the hand on the main page's row
    // with the submenu's row number.
    if (g_open) g_value_help = false;
    g_pressed = g_dragging = false;
    return g_open;
}

void menu_touch(int action, float x, float y) {
    const Art& a = art();
    if (!a.ok) return;
    {   // A dialog takes every touch: a button pushes in under the finger and
        // acts when the finger lifts on it, as the bottom bar's do.
        std::function<void(int)> fire;
        int idx = -1;
        bool handled = false, cancel = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_open && g_dlg.up) {
                handled = true;
                Canvas m(a, nullptr, 0, 0);
                const int at = dialog_button_at(m, x, y);
                if (action == 0) {
                    g_dlg.pressed = at;
                    if (at >= 0) g_dlg.sel = at;
                } else if (action == 1) {
                    if (g_dlg.pressed >= 0 && at != g_dlg.pressed) g_dlg.pressed = -1;
                } else {
                    const int was = g_dlg.pressed;
                    g_dlg.pressed = -1;
                    if (action == 2 && was >= 0 && at == was) {
                        idx = was;
                        cancel = was == g_dlg.cancel;
                        fire = std::move(g_dlg.done);
                        g_dlg.up = false;
                    }
                }
                g_dirty.store(true, std::memory_order_release);
            }
        }
        if (idx >= 0) {
            if (cancel) menu_cancel_sound();
            else menu_confirm_sound();
            if (fire) fire(idx);
        }
        if (handled) return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_open) return;
    if (g_page == 7) { states_touch(action, x, y); return; }
    if (action == 0) {
        g_pressed = true;
        g_dragging = false;
        g_down_x = x; g_down_y = y; g_down_scroll = g_scroll;
        return;
    }
    if (!g_pressed) return;
    if (action == 1) {
        if (!g_dragging && std::abs(y - g_down_y) > 4.0f) g_dragging = true;
        if (g_dragging)
            g_scroll = std::min(std::max(g_down_scroll - (y - g_down_y), 0.0f), max_scroll());
        return;
    }
    g_pressed = false;
    if (action != 2 || g_dragging) {
        // Settle on whole rows, as the game's window only ever shows.
        g_scroll = std::min(std::max(static_cast<float>(static_cast<int>(g_scroll / kRowH + 0.5f) * kRowH),
                                     0.0f), max_scroll());
        g_dragging = false;
        return;
    }
    // A tap. The arrows scroll a row; a row is selected, and its value steps
    // when the tap lands on the value.
    int n = 0;
    const Row* r = rows(&n);
    if (y >= kListTop && y < kListBottom) {
        if (x >= 216 && y >= kListBottom - 16 && g_scroll < max_scroll()) {
            g_scroll = std::min(g_scroll + kRowH, max_scroll());
            return;
        }
        if (x >= 216 && y < kListTop + 16 && g_scroll > 0) {
            g_scroll = std::max(g_scroll - kRowH, 0.0f);
            return;
        }
        const int i = static_cast<int>((y - kListTop + g_scroll) / kRowH);
        if (i < 0 || i >= n || !selectable(r[i])) return;
        const bool on_value = x >= kValueX - 8 || r[i].kind == Kind::Page ||
                              r[i].kind == Kind::Back;
        const bool was = g_sel == i;
        g_sel = i;
        g_value_help = false;
        if (on_value || was) {
            step_value(a, r[i]);
            g_value_help = r[i].kind == Kind::Game;
        }
    }
}

namespace {

// The hand bobs toward its row: 0, 1, 2, 1 pixels, a step every 133 ms (a
// 32-frame cycle). The picture is sent again whenever the step changes.
int hand_bob() {
    static constexpr int kBob[4] = {0, 1, 2, 1};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    return kBob[(ms / 133) % 4];
}
std::atomic<int> g_bob_drawn{-1};

// Keep the selected row inside the visible band.
void follow_selection() {
    const float top = static_cast<float>(g_sel * kRowH);
    const float band = static_cast<float>(kListBottom - kListTop);
    if (top < g_scroll) g_scroll = top;
    if (top + kRowH > g_scroll + band) g_scroll = top + kRowH - band;
    g_scroll = std::min(std::max(g_scroll, 0.0f), max_scroll());
}

void move_selection(int dir) {
    int n = 0;
    const Row* r = rows(&n);
    int i = g_sel;
    for (int k = 0; k < n; ++k) {
        i = (i + dir + n) % n;
        if (selectable(r[i])) break;
    }
    g_sel = i;
    g_value_help = false;
    follow_selection();
}
}  // namespace

std::uint16_t menu_filter_keys(std::uint16_t keyinput) {
    // While the menu is open the pad drives it and the game sees nothing
    // pressed. A press acts at once; up, down, left and right repeat when held
    // (after 300 ms, every 80 ms), timed by the clock because the input is
    // polled more than once a frame.
    using clock = std::chrono::steady_clock;
    // A button held while the menu was open stays hidden from the game until
    // it is released: the B that closes the menu must not also reach the game.
    static std::uint16_t prev = 0x03FFu;
    static std::uint16_t swallowed = 0;
    static clock::time_point next_repeat[10];
    const Art& a = art();
    std::unique_lock<std::mutex> lock(g_mu);
    const std::uint16_t held = static_cast<std::uint16_t>(~keyinput & 0x03FFu);
    if (!g_open || !a.ok) {
        swallowed &= held;
        prev = keyinput;
        return static_cast<std::uint16_t>(keyinput | swallowed);
    }
    swallowed |= held;
    const auto now = clock::now();
    const std::uint16_t was = static_cast<std::uint16_t>(~prev & 0x03FFu);
    prev = keyinput;
    auto fired = [&](int bit, bool repeats) {
        const bool down = held & (1u << bit);
        if (!down) return false;
        if (!(was & (1u << bit))) {
            next_repeat[bit] = now + std::chrono::milliseconds(300);
            return true;
        }
        if (repeats && now >= next_repeat[bit]) {
            next_repeat[bit] = now + std::chrono::milliseconds(80);
            return true;
        }
        return false;
    };
    if (g_dlg.up) {   // the dialog: left/right move the hand, A presses, B cancels
        const int n = static_cast<int>(g_dlg.buttons.size());
        bool changed = false, close = false;
        int idx = -1;
        if (fired(5, true) && g_dlg.sel > 0) { --g_dlg.sel; changed = true; }
        if (fired(4, true) && g_dlg.sel < n - 1) { ++g_dlg.sel; changed = true; }
        if (fired(0, false)) { idx = g_dlg.sel; close = true; }
        else if (fired(1, false)) { idx = g_dlg.cancel; close = true; }
        if (!close) {
            if (changed) g_dirty.store(true, std::memory_order_release);
            return 0x03FFu;
        }
        const bool cancel = idx == g_dlg.cancel;
        std::function<void(int)> fire = std::move(g_dlg.done);
        g_dlg.up = false;
        g_dirty.store(true, std::memory_order_release);
        lock.unlock();
        if (cancel) menu_cancel_sound();
        else menu_confirm_sound();
        if (fire) fire(idx);
        return 0x03FFu;
    }
    if (g_page == 7) {   // Save States: the D-pad moves in the grid, A loads, B goes back
        const std::vector<StateEntry> list = states_list();
        const int count = static_cast<int>(list.size());
        int sel = g_st_sel;
        if (fired(6, true)) sel -= kStCols;
        if (fired(7, true)) sel += kStCols;
        if (fired(5, true)) sel -= 1;
        if (fired(4, true)) sel += 1;
        if (count > 0) sel = std::min(std::max(sel, 0), count - 1);
        const bool moved = sel != g_st_sel;
        g_st_sel = sel;
        if (moved) { states_follow(count); g_st_armed = -1; }
        bool changed = moved;
        const int at = count > 0 ? list[static_cast<std::size_t>(g_st_sel)].number : -1;
        if (fired(2, false) && count > 0) { g_st_armed = g_st_armed == at ? -1 : at; changed = true; }   // Select
        if (fired(8, false)) { menu_confirm_sound(); states_save_new(); changed = true; }   // R: save
        if (fired(0, false) && count > 0) {                                                             // A
            if (g_st_armed == at) states_remove(at);
            else states_pick(list, g_st_sel);
            changed = true;
        }
        if (fired(1, false)) {                                                                          // B
            if (g_st_armed >= 0) g_st_armed = -1;
            else leave_page();
            changed = true;
        }
        if (changed) g_dirty.store(true, std::memory_order_release);
        return 0x03FFu;
    }
    int n = 0;
    const Row* r = rows(&n);
    if (g_sel >= n) g_sel = 0;
    bool changed = false;
    if (fired(6, true)) { move_selection(-1); changed = true; }         // up
    if (fired(7, true)) { move_selection(+1); changed = true; }         // down
    const Row& sel = r[g_sel];
    const bool page_row = sel.kind == Kind::Page || sel.kind == Kind::Back;
    if (fired(4, true) && !page_row) { step_value(a, sel, +1); g_value_help = sel.kind == Kind::Game; changed = true; }
    if (fired(5, true) && !page_row) { step_value(a, sel, -1); g_value_help = sel.kind == Kind::Game; changed = true; }
    // R and L: ten maps at a time on the battle map's row.
    if (sel.kind == Kind::Map && fired(8, true)) { step_value(a, sel, +10); changed = true; }
    if (sel.kind == Kind::Map && fired(9, true)) { step_value(a, sel, -10); changed = true; }
    if (fired(0, false)) {                                              // A
        step_value(a, sel, +1);
        g_value_help = sel.kind == Kind::Game;
        if (page_row) follow_selection();
        changed = true;
    }
    if (fired(1, false)) {                                              // B
        if (g_page != 0) leave_page();
        else g_open = false;
        changed = true;
    }
    if (changed) g_dirty.store(true, std::memory_order_release);
    return 0x03FFu;
}

void menu_open_states() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_open && g_page == 7) {
            g_open = false;
            g_sound_request.store(kSoundClose);
        } else {
            // Back from it goes to the main page's Save States row.
            for (int i = 0; i < static_cast<int>(sizeof kMain / sizeof kMain[0]); ++i)
                if (kMain[i].kind == Kind::Page && kMain[i].id == 7) g_return_sel = i;
            g_states_parent = 0;
            g_open = true;
            g_page = 7;
            g_sel = 0;
            g_st_sel = 0;
            states_scroll_to(0);
            g_st_armed = -1;
            g_value_help = false;
            g_pressed = g_dragging = false;
            g_sound_request.store(kSoundOpen);
        }
    }
    g_dirty.store(true, std::memory_order_release);
}

// The thumbnails, one at a time and off every thread that draws: the one nearest
// the grid's place first. It ends when none is left or the page has gone.
static void states_thumb_worker() {
    for (;;) {
        int number = -1;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_open && g_page == 7) {
                const std::vector<StateEntry> list = states_list();
                const int at = static_cast<int>(g_st_scroll) / kStCellH * kStCols;
                int best = -1;
                for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                    if (g_thumbs.count(list[static_cast<std::size_t>(i)].number)) continue;
                    if (best < 0 || std::abs(i - at) < std::abs(best - at)) best = i;
                }
                if (best >= 0) number = list[static_cast<std::size_t>(best)].number;
            }
            if (number < 0) { g_st_worker = false; return; }
        }
        Thumb t;
        if (!states_thumb(number, kStThumbW, kStThumbH, &t.argb, &t.w, &t.h)) t = Thumb{};
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_thumbs.emplace(number, std::move(t));
        }
        g_dirty.store(true, std::memory_order_release);
    }
}

bool menu_states_info(int* info) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_open || g_page != 7 || !g_st_rows_outside) return false;
    const std::vector<StateEntry> list = states_list();
    const int count = static_cast<int>(list.size());
    const int shown[4] = {g_st_sel, g_st_armed, count, static_cast<int>(g_thumbs.size())};
    if (!std::equal(shown, shown + 4, g_st_shown)) {
        std::copy(shown, shown + 4, g_st_shown);
        ++g_st_version;
    }
    const bool missing = std::any_of(list.begin(), list.end(), [](const StateEntry& e) {
        return g_thumbs.count(e.number) == 0;
    });
    if (!g_st_worker && missing) {
        g_st_worker = true;
        std::thread(states_thumb_worker).detach();
    }
    info[0] = (count + kStCols - 1) / kStCols;
    info[1] = g_st_version;
    info[2] = g_st_target_seq;
    info[3] = static_cast<int>(g_st_target);
    info[4] = kStTop;
    info[5] = kStBottom;
    info[6] = kStCellH;
    return true;
}

bool menu_states_row(int row, std::uint32_t* argb) {
    const Art& a = art();
    if (!a.ok || !argb || row < 0) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    const std::vector<StateEntry> list = states_list();
    const int first = row * kStCols;
    if (first >= static_cast<int>(list.size())) return false;
    std::fill(argb, argb + kSheetW * kStCellH, 0u);
    Canvas c(a, argb, kSheetW, kStCellH);
    c.clear_clip();
    for (int i = first; i < first + kStCols && i < static_cast<int>(list.size()); ++i)
        states_cell(c, list[static_cast<std::size_t>(i)], i == g_st_sel, kStLeft + (i - first) * kStCellW, 0, false);
    return true;
}

void menu_states_scroll(float scroll) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_st_scroll = scroll;
}

bool menu_take_dirty() {
    const bool bob = menu_is_open() && hand_bob() != g_bob_drawn.load(std::memory_order_relaxed);
    // The Achievements page redraws as the account or the list changes.
    static std::atomic<int> ra_seen{-1};
    const int ra = cheevos_version();
    const bool ra_changed = menu_is_open() && ra_seen.exchange(ra) != ra;
    return g_dirty.exchange(false, std::memory_order_acq_rel) || bob || ra_changed;
}

bool menu_render(std::uint32_t* argb) {
    const Art& a = art();
    if (!a.ok || !argb) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    std::fill(argb, argb + kSheetW * kSheetH, 0xFF000000u);
    Canvas c(a, argb, kSheetW, kSheetH);
    if (g_page == 7) {
        states_render(c);
        return true;
    }
    if (g_page == 8) ra_rebuild();   // before any row is held
    // The OPTION window, its tab over the top edge as the game has it.
    c.window(1, kWinTop, kSheetCols - 1, kWinBottomRow);
    c.option_tab(2, kWinTop);
    int n = 0;
    const Row* r = rows(&n);
    if (g_sel >= n || !selectable(r[g_sel])) g_sel = 0;
    // The rows, clipped to the window's inner band.
    c.set_clip(kListTop, kListBottom);
    const int first = static_cast<int>(g_scroll) / kRowH;
    for (int i = first; i < n && i <= first + 8; ++i) {
        const int y = kListTop + i * kRowH - static_cast<int>(g_scroll);
        const Row& row = r[i];
        if (row.kind == Kind::Heading) {
            const Text t = encode(row.name);
            c.text(t, (kSheetW - c.width(t)) / 2, y);
            continue;
        }
        Text name = row.kind == Kind::Game ? a.opts[row.id].name : encode(row.name);
        if (row.kind == Kind::Cheevo) {
            // An achievement's title is cut to the label's column, with "..."
            const Text value = value_text(a, row);
            const int room = kSheetW - 16 - c.width(value) - 6 - kLabelX - (row.id >= kCheevoEntry ? 20 : 0);
            if (c.width(name) > room) {
                const Text dots = encode("...");
                while (!name.codes.empty() && c.width(name) + c.width(dots) > room) name.codes.pop_back();
                name.codes.insert(name.codes.end(), dots.codes.begin(), dots.codes.end());
            }
            // An achievement: its badge, 16 x 16, before the title.
            int tx = kLabelX;
            const std::size_t ei = static_cast<std::size_t>(row.id - kCheevoEntry);
            if (row.id >= kCheevoEntry && ei < g_ra_list.size()) {
                const auto badge = cheevos_badge(g_ra_list[ei].badge, 16, !g_ra_list[ei].unlocked);
                if (!badge.empty()) c.blit(kLabelX, y, badge.data(), 16, 16);
                tx = kLabelX + 20;
            }
            c.text(name, tx, y);
            c.text(value, kSheetW - 16 - c.width(value), y);
            continue;
        }
        c.text(name, kLabelX, y);
        c.text(value_text(a, row), kValueX, y);
    }
    c.clear_clip();
    // The hand points at the selected row, over the window's left edge.
    const int hy = kListTop + g_sel * kRowH - static_cast<int>(g_scroll) + 4;
    const int bob = hand_bob();
    g_bob_drawn.store(bob, std::memory_order_relaxed);
    if (hy >= kListTop - 4 && hy + 8 <= kListBottom + 4) c.hand(bob, hy);
    if (g_scroll > 0) c.arrow(kSheetW - 24, kListTop, false);
    if (g_scroll < max_scroll()) c.arrow(kSheetW - 24, kListBottom - 8, true);

    // The HELP window: the selected option's text (or, just after a change,
    // the value's), lines as the game breaks them.
    c.window(0, kHelpTopRow, kSheetCols - 1, kSheetRows - 1);
    c.help_tab(2, kHelpTopRow);
    Text help;
    std::vector<unsigned> help_badge;
    const Row& sel = r[g_sel];
    if (sel.kind == Kind::Game) {
        const GameOption& g = a.opts[sel.id];
        const int v = std::min<int>(game_value(sel.id), static_cast<int>(g.values.size()) - 1);
        help = g_value_help && sel.id != kDefaultOption ? g.value_help[std::max(v, 0)] : g.help;
    } else if (sel.kind == Kind::Cheevo && sel.help && !std::strchr(sel.help, '\n')) {
        // An achievement's description (or a server's message), word-wrapped into
        // the box's two lines; an achievement's badge, 32 x 32, at the box's left.
        const std::size_t ei = static_cast<std::size_t>(sel.id - kCheevoEntry);
        if (sel.id >= kCheevoEntry && ei < g_ra_list.size())
            help_badge = cheevos_badge(g_ra_list[ei].badge, 32, !g_ra_list[ei].unlocked);
        const int room = kSheetW - 20 - (help_badge.empty() ? 0 : 40);
        std::string line, out;
        int lines_used = 0;
        std::string word;
        auto flush_word = [&](bool end) {
            if (word.empty() && !end) return;
            const std::string tryl = line.empty() ? word : line + " " + word;
            if (!line.empty() && c.width(encode(tryl.c_str())) > room && lines_used == 0) {
                out += line + "\n";
                ++lines_used;
                line = word;
            } else {
                line = tryl;
            }
            word.clear();
        };
        for (const char* p = sel.help; *p; ++p) {
            if (*p == ' ' || *p == '\n') flush_word(false);
            else word += *p;
        }
        flush_word(true);
        out += line;
        help = encode(out.c_str());
    } else if (sel.help) {
        help = encode(sel.help);
    }
    // Split on 0x88 and centre each line, as the game's HELP box does.
    std::vector<Text> lines(1);
    for (std::uint8_t ch : help.codes) {
        if (ch == kNewline) { lines.emplace_back(); continue; }
        lines.back().codes.push_back(ch);
    }
    const int top = kHelpTopRow * 8 + 14;
    int left = 0;
    if (!help_badge.empty()) {
        c.blit(12, top, help_badge.data(), 32, 32);
        left = 40;
    }
    for (std::size_t k = 0; k < lines.size() && k < 2; ++k)   // every help is two lines
        c.text(lines[k], left + (kSheetW - left - c.width(lines[k])) / 2, top + static_cast<int>(k) * 16);
    if (g_dlg.up) dialog_render(c, argb, a, bob);
    return true;
}


bool menu_zone_render(std::uint32_t* argb, int h, int pressed, int world) {
    const Art& a = art();
    if (!a.ok || !argb || h < kMenuButtonSize) return false;
    std::fill(argb, argb + kSheetW * h, 0u);
    Canvas c(a, argb, kSheetW, h);
    // The bar in the frame of the game's status window as Organize shows it
    // (tiles 0x09-0x10 of this archive, bank 4: a bevel with rounded
    // corners; DY), over the parchment weave. The button keeps the SUPPLY
    // menu's frame (0x11-0x18); pressed, its window darkens by a brightness
    // decrease of 4/16, as the PPU computes one on 5-bit colours.
    const std::uint32_t* bar = a.bg_pal[kFrameBank];
    std::uint32_t button[16];   // a pressed button's colours
    button_palette(bar, 1, button);
    auto piece = [&](int x, int y, int t) {
        c.tile4(x, y, &a.bg[static_cast<std::size_t>(t) * 32u], bar);
    };
    std::uint32_t grey[16];   // a greyed button's frame: the colours by their brightness
    button_palette(bar, 2, grey);
    auto button_piece = [&](int x, int y, int t, const std::uint32_t* pal) {
        c.tile4(x, y, &a.bg[static_cast<std::size_t>(t) * 32u], pal);
    };
    // Rows of 8 from the top; the last one, the bottom edge, pinned to the
    // bottom whatever the height, over the row it overlaps.
    constexpr int cols = kSheetW / 8;
    for (int r = 0;; ++r) {
        const int y = std::min(r * 8, h - 8);
        const bool top = r == 0, bottom = y == h - 8;
        for (int col = 0; col < cols; ++col) {
            int t;
            if (top) t = col == 0 ? 0x09 : col == cols - 1 ? 0x0A : 0x0D;
            else if (bottom) t = col == 0 ? 0x0B : col == cols - 1 ? 0x0C : 0x0E;
            else if (col == 0) t = 0x0F;
            else if (col == cols - 1) t = 0x10;
            else t = 0x05 + ((col - 1) + 2 * ((r - 1) & 1)) % 4;
            piece(col * 8, y, t);
        }
        if (bottom) break;
    }
    // The buttons, as the corner one: a 3x3 window with an icon of the R
    // bar's (Option; on the world map also Save and Load, as its system
    // menu has them).
    const int by = menu_zone_button_y(h);
    for (int b = 0; b < ((world & 3) != kZoneWorldNone ? kZoneButtons : 1); ++b) {
        const int bx = menu_zone_button_x(b);
        const bool off = !menu_zone_button_live(b, world);
        const bool down = pressed == b && !off;
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) {
                int t;
                if (r == 0) t = col == 0 ? 0x11 : col == 2 ? 0x12 : 0x15;
                else if (r == 2) t = col == 0 ? 0x13 : col == 2 ? 0x14 : 0x16;
                else t = col == 0 ? 0x17 : col == 2 ? 0x18 : 0x05;
                button_piece(bx + col * 8, by + r * 8, t, off ? grey : down ? button : bar);
            }
        static constexpr int kIcons[kZoneButtons] = {kOptionIcon, kSaveIcon, kLoadIcon, 0, 0, 0,
                                                     kTrainingIcon, kShopIcon};
        const int ix = bx + 4 + (down ? 1 : 0), iy = by + 4 + (down ? 1 : 0);
        if (b >= kZoneItem && b <= kZoneClass) c.roster_icon(b - kZoneItem, ix, iy, off);
        else c.icon(kIcons[b], ix, iy, off);
    }
    return true;
}

int menu_zone_help_render(int button, std::uint32_t* argb) {
    // The game's own name for each and its HELP text (the R menus' and the
    // world map menus' help boxes, read from the HELP banks).
    struct Help { const char* name; unsigned id; };
    static constexpr Help kHelp[kZoneButtons] = {
        {"Option", 0x266},     // "Customize the game's settings."
        {"Save", 0x264},       // "Up to three data files can be saved."
        {"Load", 0x265},       // "Data can be loaded from one of your three saved files."
        {"Item", 0x1D6},       // "Items can be equipped and unequipped."
        {"Spell", 0x1D7},      // "Spellbooks can be equipped and unequipped."
        {"Class", 0x1D8},      // "By meeting certain criteria, a character can change class."
        {"Training", 0x262},   // "Strengthen characters and obtain emblems in Training Mode..."
        {"Shop", 0x261},       // "Buy items and spellbooks, or employ new characters."
    };
    if (button < 0 || button >= kZoneButtons || !argb) return 0;
    UnitInfo text;
    if (!help_text_for(kHelp[button].id, &text)) return 0;
    std::uint8_t title[16];
    int n = 0;
    for (const char* p = kHelp[button].name; *p && n < 16; ++p)
        title[n++] = static_cast<std::uint8_t>(*p >= 'a' ? 0x1A + (*p - 'a') : *p - 'A');
    return render_help_card(title, n, text, argb);
}

bool menu_draw_window(std::uint32_t* argb, int w, int h, int c0, int r0, int c1, int r1) {
    const Art& a = art();
    if (!a.ok) return false;
    Canvas c(a, argb, w, h);
    c.window(c0, r0, c1, r1);
    return true;
}

int menu_draw_text(std::uint32_t* argb, int w, int h, const char* s, int x, int y) {
    const Art& a = art();
    if (!a.ok) return 0;
    Canvas c(a, argb, w, h);
    return c.text(encode(s), x, y);
}

int menu_text_width(const char* s) {
    const Art& a = art();
    if (!a.ok) return 0;
    Canvas c(a, nullptr, 0, 0);
    return c.width(encode(s));
}

bool menu_hand_render(std::uint32_t* argb) {
    const Art& a = art();
    if (!a.ok || !argb) return false;
    std::fill(argb, argb + kHandW * kHandH, 0u);
    Canvas c(a, argb, kHandW, kHandH);
    c.hand(0, 0);
    return true;
}

bool menu_window_render(std::uint32_t* argb, int cols, int rows, int style) {
    const Art& a = art();
    if (!a.ok || !argb || cols < 2 || rows < 2) return false;
    std::fill(argb, argb + cols * 8 * rows * 8, 0u);
    Canvas c(a, argb, cols * 8, rows * 8);
    std::uint32_t pal[16];
    button_palette(a.bg_pal[kFrameBank], style, pal);
    for (int r = 0; r < rows; ++r)
        for (int col = 0; col < cols; ++col) {
            int t;
            if (r == 0) t = col == 0 ? 0x11 : col == cols - 1 ? 0x12 : 0x15;
            else if (r == rows - 1) t = col == 0 ? 0x13 : col == cols - 1 ? 0x14 : 0x16;
            else if (col == 0) t = 0x17;
            else if (col == cols - 1) t = 0x18;
            else t = 0x05 + ((col - 1) + 2 * ((r - 1) & 1)) % 4;
            c.tile4(col * 8, r * 8, &a.bg[static_cast<std::size_t>(t) * 32u], pal);
        }
    return true;
}

std::vector<std::uint32_t> menu_cheevo_popup(const char* title, unsigned points,
                                             const std::uint32_t* badge32, int* w, int* h) {
    char pts[24];
    std::snprintf(pts, sizeof pts, "%u pts", points);
    return menu_notice_popup("Achievement Unlocked!", pts, title, badge32, w, h);
}

std::vector<std::uint32_t> menu_notice_popup(const char* heading, const char* corner, const char* line,
                                             const std::uint32_t* badge32, int* w, int* h) {
    const Art& a = art();
    if (!a.ok) return {};
    // Measured on a scratch canvas (widths need the font only).
    std::vector<std::uint32_t> scratch(8 * 8);
    Canvas m(a, scratch.data(), 8, 8);
    const int x = badge32 ? 48 : 10;          // text starts after the badge
    constexpr int kMargin = 10, kGap = 12, kLineH = 16;
    const Text hd = encode(heading ? heading : ""), cr = encode(corner ? corner : "");
    const int top_w = m.width(hd) + (cr.codes.empty() ? 0 : kGap + m.width(cr));
    // The line's words, wrapped later to the width chosen.
    const std::string body = line ? line : "";
    const int line_w = m.width(encode(body.c_str()));
    // Wider first, up to kCheevoPopupMaxCols; then the line wraps and the
    // window grows down (two tiles a line).
    const int need = x + std::max(top_w, line_w) + kMargin;
    const int cols = std::clamp((need + 7) / 8, kCheevoPopupCols, kCheevoPopupMaxCols);
    const int W = cols * 8, right = W - kMargin, room = right - x;
    std::vector<std::string> lines;
    {
        std::string cur;
        std::size_t i = 0;
        while (i <= body.size()) {
            const std::size_t sp = body.find(' ', i);
            const std::string word = body.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            const std::string tryl = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && m.width(encode(tryl.c_str())) > room) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = tryl;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(cur);
        if (lines.size() > 4) {   // at most four lines; the rest is cut
            lines.resize(4);
            lines.back() += "...";
        }
    }
    const int rows = kCheevoPopupRows + 2 * (static_cast<int>(lines.size()) - 1);
    const int H = rows * 8;
    std::vector<std::uint32_t> argb(static_cast<std::size_t>(W) * H, 0u);
    Canvas c(a, argb.data(), W, H);
    c.window(0, 0, cols - 1, rows - 1);
    if (badge32) c.blit(8, 8, badge32, 32, 32);
    auto fit = [&](Text t, int width) {   // cut to `width` with "..."
        if (c.width(t) <= width) return t;
        const Text dots = encode("...");
        while (!t.codes.empty() && c.width(t) + c.width(dots) > width) t.codes.pop_back();
        t.codes.insert(t.codes.end(), dots.codes.begin(), dots.codes.end());
        return t;
    };
    c.text(fit(hd, room - (cr.codes.empty() ? 0 : kGap + c.width(cr))), x, 7);
    if (!cr.codes.empty()) c.text(cr, right - c.width(cr), 7);
    for (std::size_t i = 0; i < lines.size(); ++i)
        c.text(fit(encode(lines[i].c_str()), room), x, 24 + kLineH * static_cast<int>(i));
    if (w) *w = W;
    if (h) *h = H;
    return argb;
}

int menu_encode(const char* text, std::uint8_t* out, int max) {
    const Text e = encode(text);
    int n = 0;
    for (const auto b : e.codes)
        if (n < max) out[n++] = static_cast<std::uint8_t>(b);
    return n;
}

bool menu_arrow_render(std::uint32_t* argb, bool down) {
    const Art& a = art();
    if (!a.ok || !argb) return false;
    std::fill(argb, argb + 8 * 8, 0u);
    Canvas c(a, argb, 8, 8);
    c.arrow(0, 0, down);
    return true;
}

bool menu_button_render(std::uint32_t* argb) {
    const Art& a = art();
    if (!a.ok || !argb) return false;
    std::fill(argb, argb + kMenuButtonSize * kMenuButtonSize, 0u);
    Canvas c(a, argb, kMenuButtonSize, kMenuButtonSize);
    c.window(0, 0, 2, 2);
    c.icon(kOptionIcon, 4, 4);
    return true;
}

void menu_debug_frame() {
    // KOL_ZONE_PPM=path,h: the zone under the page, once (desktop check):
    // as it is, pushed in, and with the hand beside the button as StatsView
    // draws it while the menu is open, one under the other.
    static bool zone_done = false;
    if (!zone_done && art().ok) {
        zone_done = true;
        if (const char* z = std::getenv("KOL_ZONE_PPM")) {
            std::string spec = z;
            const auto comma = spec.find(',');
            const int h = comma == std::string::npos ? 32 : std::atoi(spec.c_str() + comma + 1);
            const std::size_t one = static_cast<std::size_t>(kSheetW) * std::max(h, 0);
            std::vector<std::uint32_t> px(one * 3), hand(kHandW * kHandH);
            if (menu_zone_render(px.data(), h, -1, kZoneWorldOn | kZoneNoShop) && menu_zone_render(px.data() + one, h, -1, kZoneWorldGreyed | kZoneOrganizeOn) &&
                menu_hand_render(hand.data())) {
                {
                    std::uint32_t* out = px.data() + one * 2;
                    menu_zone_render(out, h, kZoneSave, kZoneWorldOn);
                    const int hx = menu_zone_button_x() - (kHandW + 2);
                    const int hy = menu_zone_button_y(h) + (kMenuButtonSize - kHandH) / 2;
                    for (int y = 0; y < kHandH; ++y)
                        for (int x = 0; x < kHandW; ++x)
                            if (hand[y * kHandW + x] >> 24) out[(hy + y) * kSheetW + hx + x] = hand[y * kHandW + x];
                }
            }
            if (!px.empty())
                if (FILE* f = std::fopen(spec.substr(0, comma).c_str(), "wb")) {
                    std::fprintf(f, "P6 %d %d 255\n", kSheetW, h * 3);
                    for (const std::uint32_t c : px) {
                        const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16),
                                                      static_cast<unsigned char>(c >> 8),
                                                      static_cast<unsigned char>(c)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
        }
    }
    // KOL_STOCK_HELP_PPM=path (with a %d): the stock screens' help card each
    // time the item or spellbook under the cursor changes (desktop check).
    if (const char* sp = std::getenv("KOL_STOCK_HELP_PPM")) {
        static int last = -1, count = 0;
        UnitInfo u;
        const int item = stock_help(&u) || shop_spell_help(&u) ? u.help_item : 0;
        if (item != last) {
            last = item;
            std::vector<std::uint32_t> px(kSheetW * kSheetH);
            const int h = item ? render_help_card(nullptr, 0, u, px.data()) : 0;
            std::fprintf(stderr, "stock_help: item %04X card %d\n", item, h);
            char name[512];
            std::snprintf(name, sizeof name, sp, count++);
            if (h > 0)
                if (FILE* f = std::fopen(name, "wb")) {
                    std::fprintf(f, "P6 %d %d 255\n", kSheetW, h);
                    for (int i = 0; i < kSheetW * h; ++i) {
                        const unsigned char rgb[3] = {static_cast<unsigned char>(px[i] >> 16),
                                                      static_cast<unsigned char>(px[i] >> 8),
                                                      static_cast<unsigned char>(px[i])};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
        }
    }
    // KOL_ZONE_HELP_PPM=path: the bar's help cards, all of them one under the
    // other (desktop check).
    static bool help_done = false;
    if (!help_done && art().ok)
        if (const char* hp = std::getenv("KOL_ZONE_HELP_PPM")) {
            help_done = true;
            std::vector<std::uint32_t> all, one(kSheetW * kSheetH);
            int rows = 0;
            for (int b = 0; b < kZoneButtons; ++b) {
                const int h = menu_zone_help_render(b, one.data());
                all.insert(all.end(), one.begin(), one.begin() + kSheetW * (h + 4));
                rows += h + 4;
            }
            if (FILE* f = std::fopen(hp, "wb")) {
                std::fprintf(f, "P6 %d %d 255\n", kSheetW, rows);
                for (const std::uint32_t c : all) {
                    const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16),
                                                  static_cast<unsigned char>(c >> 8),
                                                  static_cast<unsigned char>(c)};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
    // KOL_MENU_PPM=path[,taps]: once the ROM art is up, open the menu, replay
    // "x:y" taps (';'-separated, canvas pixels), and write the image as a PPM
    // (desktop checks, no window needed). Also KOL_MENU_DUMP_TEXT=1.
    static int state = 0;
    if (state < 0) return;
    const char* e = std::getenv("KOL_MENU_PPM");
    if (!e || !art().ok) return;
    static const int wait = [] {   // KOL_MENU_PPM_WAIT=n: n frames first (8)
        const char* w = std::getenv("KOL_MENU_PPM_WAIT");
        return w ? std::atoi(w) : 8;
    }();
    if (++state < wait) return;   // after the save state has been applied
    state = -1;
    std::string spec = e, path = spec, taps;
    const auto comma = spec.find(',');
    if (comma != std::string::npos) { path = spec.substr(0, comma); taps = spec.substr(comma + 1); }
    if (!menu_is_open()) menu_toggle();
    // KOL_MENU_DIALOG="title|text|b1,b2,..|pressed": a dialog over it, one
    // button pushed in (or -1), to see the look.
    if (const char* d = std::getenv("KOL_MENU_DIALOG")) {
        std::vector<std::string> f;
        std::string cur;
        for (const char* q = d;; ++q) {
            if (*q == '|' || !*q) { f.push_back(cur); cur.clear(); if (!*q) break; }
            else cur += *q;
        }
        f.resize(4);
        std::vector<std::string> buttons;
        std::string b;
        for (char ch : f[2] + ",") {
            if (ch == ',') { if (!b.empty()) buttons.push_back(b); b.clear(); }
            else b += ch;
        }
        menu_dialog(f[0].c_str(), f[1].c_str(), buttons, static_cast<int>(buttons.size()) - 1, nullptr);
        std::lock_guard<std::mutex> lock(g_mu);
        g_dlg.pressed = f[3].empty() ? -1 : std::atoi(f[3].c_str());
    }
    std::size_t at = 0;
    while (at < taps.size()) {
        const auto semi = taps.find(';', at);
        const std::string t = taps.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        float x = 0, y = 0;
        if (std::sscanf(t.c_str(), "%f:%f", &x, &y) == 2) {
            menu_touch(0, x, y);
            menu_touch(2, x, y);
        } else if (t.rfind("k", 0) == 0) {    // k<hex>: one pad press (KEYINPUT bits)
            unsigned bits = 0;
            std::sscanf(t.c_str() + 1, "%x", &bits);
            menu_filter_keys(static_cast<std::uint16_t>(~bits & 0x03FFu));
            menu_filter_keys(0x03FFu);
        } else if (t == "states") {   // L3 + R3
            menu_open_states();
        } else if (std::sscanf(t.c_str(), "drag%f", &y) == 1) {
            menu_touch(0, 120, 60);
            menu_touch(1, 120, 60 - y);
            menu_touch(2, 120, 60 - y);
        }
        if (semi == std::string::npos) break;
        at = semi + 1;
    }
    if (std::getenv("KOL_STATES_ROWS_CHECK") && g_page == 7) {
        // The page as the second screen puts it together (frame, then the rows
        // at a scroll, then the arrows) against the page drawn whole.
        g_st_rows_outside = true;
        int info[7] = {};
        const auto t0 = std::chrono::steady_clock::now();
        for (int tries = 0; tries < 2000; ++tries) {
            menu_states_info(info);
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (!g_st_worker) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
        std::fprintf(stderr, "states rows: %d rows, version %d, thumbnails made in %d ms (%zu)\n",
                     info[0], info[1], ms, g_thumbs.size());
        const int count_rows = info[0];
        const int max = std::max(0, count_rows * info[6] - (info[5] - info[4]));
        const int scrolls[] = {0, 13, 60, 137, max / 2, max};
        for (int want : scrolls) {
            const int scroll = std::min(want, max);
            std::vector<std::uint32_t> whole(kSheetW * kSheetH), built(kSheetW * kSheetH), row(kSheetW * kStCellH);
            g_st_rows_outside = false;
            g_st_scroll = static_cast<float>(scroll);
            menu_render(whole.data());
            g_st_rows_outside = true;
            menu_render(built.data());
            for (int r = scroll / info[6]; r < count_rows; ++r) {
                const int y0 = info[4] + r * info[6] - scroll;
                if (y0 >= info[5] || !menu_states_row(r, row.data())) break;
                for (int y = std::max(0, -y0 + info[4]); y < info[6] && y0 + y < info[5]; ++y)
                    for (int x = 0; x < kSheetW; ++x) {
                        const std::uint32_t p = row[static_cast<std::size_t>(y) * kSheetW + x];
                        if (p >> 24) built[static_cast<std::size_t>(y0 + y) * kSheetW + x] = p;
                    }
            }
            std::vector<std::uint32_t> arrow(8 * 8);
            for (int d = 0; d < 2; ++d) {
                if (d == 0 ? scroll <= 0 : scroll >= max) continue;
                menu_arrow_render(arrow.data(), d == 1);
                const int ay = d == 0 ? info[4] : info[5] - 8;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        if (arrow[static_cast<std::size_t>(y) * 8 + x] >> 24)
                            built[static_cast<std::size_t>(ay + y) * kSheetW + kSheetW - 16 + x] =
                                arrow[static_cast<std::size_t>(y) * 8 + x];
            }
            int diff = 0;
            for (std::size_t i = 0; i < whole.size(); ++i) diff += whole[i] != built[i];
            std::fprintf(stderr, "states rows: scroll %d: %d pixels differ\n", scroll, diff);
        }
    }
    std::vector<std::uint32_t> px(kSheetW * kSheetH);
    std::vector<std::uint32_t> btn(kMenuButtonSize * kMenuButtonSize);
    if (!menu_render(px.data()) || !menu_button_render(btn.data())) return;
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", kSheetW + kMenuButtonSize + 8, kSheetH);
        for (int y = 0; y < kSheetH; ++y)
            for (int x = 0; x < kSheetW + kMenuButtonSize + 8; ++x) {
                std::uint32_t p = 0xFF202020u;
                if (x < kSheetW) p = px[y * kSheetW + x];
                else if (x >= kSheetW + 8 && y >= kSheetH - kMenuButtonSize) {
                    const std::uint32_t b = btn[(y - (kSheetH - kMenuButtonSize)) * kMenuButtonSize +
                                                (x - kSheetW - 8)];
                    if (b >> 24) p = b;
                }
                const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16),
                                              static_cast<unsigned char>(p >> 8),
                                              static_cast<unsigned char>(p)};
                std::fwrite(rgb, 1, 3, f);
            }
        std::fclose(f);
        std::fprintf(stderr, "menu: wrote %s\n", path.c_str());
    }
}

void menu_set_platform(const MenuPlatform& p) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_platform = p;
}

void menu_dialog(const char* title, const char* text, const std::vector<std::string>& buttons,
                 int cancel_button, std::function<void(int)> done) {
    std::lock_guard<std::mutex> lock(g_mu);
    open_dialog(title, text, buttons, cancel_button, std::move(done));
}

void menu_mod_picked(const char* name, const char* error) {
    if (!name) {
        menu_dialog("Mod", error ? error : "The file could not be used.", {"OK"}, 0, nullptr);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_mod_pending = true;
    }
    std::string n = name;
    if (const auto dot = n.rfind('.'); dot != std::string::npos && dot > 0) n.resize(dot);
    ask_restart("Restart to play " + n + "?");
}

void menu_install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (std::getenv("KOL_NO_MENU_OPTIONS")) return;
    gbarecomp::g_host_keyinput_filter = &menu_filter_keys;
    if (!gba_mod_register_function_entry_plugin(kSoundHookId, kAfterTasks, 1, &on_after_tasks) ||
        !gba_mod_set_function_hook_enabled(kSoundHookId, 1))
        std::fprintf(stderr, "menu: sound hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kHookId, kSoundMain, 1, &on_frame_hook) ||
        !gba_mod_set_function_hook_enabled(kHookId, 1))
        std::fprintf(stderr, "menu: SoundMain hook NOT installed\n");
}

}  // namespace kol
