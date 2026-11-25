// kol_font.h — the cartridge's own text font and character codes, for
// whichever cartridge the build is for (kol_region.h).
//
// USA (ATOE): one byte a character, in the font's glyph order ('A' 0x00, 'a'
// 0x1A, '0' 0x34; space 0x89, a new line 0x88). A glyph is 8x16, 4bpp, 64
// bytes (two tiles, the top one first), and has its own advance.
//
// Japan (ATOJ, worked out 2026-11-25 by watching the game print a line):
// 16-bit codes, low byte first. A code under 0xF000 is a glyph, the same
// 64-byte cell, at the font + code * 64; every glyph advances 8. Kana from
// 0x0E, digits 0xAF, capitals 0xB9, small letters 0xD3, kanji from 0xED,
// punctuation at 0x00-0x0B and 0x37F-0x399. Codes from 0xF000 are controls:
// 0xF000 ends a text, 0xF100 a line. The texts are in "m7" banks
// (kol_jtext.h).
#pragma once

#include <cstdint>

#include "kol_region.h"

namespace kol::font {

using Code = std::uint16_t;

// "The font has no glyph for this character" (ascii()).
inline constexpr Code kNone = 0xFFFFu;

#if defined(KOL_REGION_ATOJ)

inline constexpr Code kSpace = 0x0000u, kNewline = 0xF100u, kEnd = 0xF000u;
// Our own pixel glyphs for what the font lacks (kol_menu's stand-ins): a
// range the game's texts never use.
inline constexpr Code kStandIn = 0xE000u;
inline constexpr std::uint32_t kGlyphs = japan_only(0x08715A5Cu);

// A code that draws a glyph of the font (not a control, not one of ours).
constexpr bool is_glyph(Code c) { return c != kSpace && c < kStandIn; }

// The controls the game's HELP, NAVI and message texts use besides the two
// above. 0xF2nn sets the colour of what follows: n is added to the text's
// palette bank (4 dark, the default; 5 grey, 6 blue, 7 red: the banks the
// box's cells name, read off the palette with a help box open and the three
// blue stars of a fortune on screen). 0xF4nn is figure n, filled in at run
// time (a birthday's month, day and sex; a team's size), and 0xFEnn the name
// of character n.
constexpr bool is_colour(Code c) { return (c & 0xFF00u) == 0xF200u; }
constexpr bool is_figure(Code c) { return (c & 0xFF00u) == 0xF400u; }
constexpr bool is_character(Code c) { return (c & 0xFF00u) == 0xFE00u; }
// The full stop a sentence ends with.
inline constexpr Code kFullStop = 0x0003u;

// The blank between the words of OUR texts (ascii(' ')): half a cell, as the
// USA font's space, where the game's own blank (kSpace) is a whole cell.
inline constexpr Code kAsciiSpace = 0xEFFFu;

// The Latin letters, digits and marks ascii() gives: in the game's texts
// each sits in an 8-pixel cell, which spreads an English line a third wider
// than the USA font did and off the ends of our boxes. OUR texts draw them
// by their ink instead: `left` blank columns skipped, the ink's width plus
// one advanced. Kana and kanji keep their cell.
constexpr bool is_latin(Code c) {
    return (c >= 0xAFu && c <= 0xECu) || (c >= 0x37Fu && c <= 0x399u) || (c >= 0x005u && c <= 0x007u);
}
struct Metric {
    std::uint8_t left = 0, advance = 8;
};
inline Metric metric(const std::uint8_t* rom, std::uint32_t size, Code c) {
    if (c == kAsciiSpace) return {0, 4};
    if (!is_latin(c)) return {0, 8};
    static Metric cache[0x400];
    static bool known[0x400];
    if (known[c]) return cache[c];
    Metric m{0, 4};
    const std::uint32_t o = kGlyphs - 0x08000000u + c * 64u;
    if (rom && o + 64u <= size) {
        int lo = 8, hi = -1;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t b = rom[o + y * 4 + x / 2];
                if ((x & 1) ? (b >> 4) : (b & 0xF)) {
                    if (x < lo) lo = x;
                    if (x > hi) hi = x;
                }
            }
        if (hi >= lo) m = {static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi - lo + 2)};
        known[c] = true;
        cache[c] = m;
    }
    return m;
}
inline int advance(const std::uint8_t* rom, std::uint32_t size, Code c) { return metric(rom, size, c).advance; }
// Blank columns at a glyph's left that drawing skips (0 but for our Latin).
inline int left(const std::uint8_t* rom, std::uint32_t size, Code c) { return metric(rom, size, c).left; }

constexpr Code ascii(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<Code>(0xAF + (ch - '0'));
    if (ch >= 'A' && ch <= 'Z') return static_cast<Code>(0xB9 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<Code>(0xD3 + (ch - 'a'));
    switch (ch) {
        case ' ': return kAsciiSpace;
        case '/': return 0x005u;
        case '[': return 0x006u;
        case ']': return 0x007u;
        case '.': return 0x37Fu;
        case ',': return 0x380u;
        case ':': return 0x382u;
        case ';': return 0x383u;
        case '?': return 0x384u;
        case '!': return 0x385u;
        case '_': return 0x386u;
        case '%': return 0x387u;
        case '~': return 0x389u;
        case '+': return 0x392u;
        case '-': return 0x393u;
        case '=': return 0x397u;
        case '(': return 0x398u;
        case ')': return 0x399u;
        default: return kNone;
    }
}

#else

inline constexpr Code kSpace = 0x89u, kNewline = 0x88u;
inline constexpr Code kStandIn = 0xE0u;   // past every code the game's texts use
inline constexpr std::uint32_t kGlyphs = usa_only(0x0815DEC4u);        // 64 bytes a glyph
inline constexpr std::uint32_t kGlyphWidths = usa_only(0x0815DE6Cu);   // a byte a code

constexpr bool is_glyph(Code c) { return c != kSpace && c < kStandIn; }

// (The Japanese texts' controls: none of them here. The USA texts' own
// escapes are bytes with an argument, read where the text is.)
constexpr bool is_colour(Code) { return false; }
constexpr bool is_figure(Code) { return false; }
constexpr bool is_character(Code) { return false; }
inline constexpr Code kFullStop = 0x40u;

inline constexpr Code kAsciiSpace = kSpace;

inline int advance(const std::uint8_t* rom, std::uint32_t size, Code c) {
    if (c == kSpace) return 4;
    const std::uint32_t o = kGlyphWidths - 0x08000000u + c;
    return o < size ? rom[o] : 6;
}
inline int left(const std::uint8_t*, std::uint32_t, Code) { return 0; }

constexpr Code ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<Code>(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return static_cast<Code>(0x1A + ch - 'a');
    if (ch >= '0' && ch <= '9') return static_cast<Code>(0x34 + ch - '0');
    if (ch == ' ') return kSpace;
    // The font's own quote and brackets (0x52 is an ellipsis at the foot of
    // the line, not a quote).
    if (ch == '"') return 0x51u;
    if (ch == '[') return 0x53u;
    if (ch == ']') return 0x54u;
    constexpr char kPunct[] = "!?.,:&_%'()<>+-*/=";
    for (int i = 0; kPunct[i]; ++i)
        if (kPunct[i] == ch) return static_cast<Code>(0x3E + i);
    return kNone;
}

#endif

}  // namespace kol::font
