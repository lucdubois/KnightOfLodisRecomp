// kol_jtext.h — the Japanese ROM's (ATOJ) text banks.
//
// A bank ("m7"): 'm', '7', a u16 count, then 12 bytes an entry: a u32 offset
// from the bank's start, two u16 of layout (the box's size in characters among
// them) and a u32. An entry opens with a u16: the size in bytes of its own
// header, that word included; then 16-bit codes (kol_font.h) up to 0xF000.
// The option names are the bank at 0x08754A4C, their HELP texts 0x087535E4
// (both 45 entries: each option, then its values, in the order of the game's
// option table); HELP is 0x08737B78 (696), NAVI 0x08751A74 (36).
//
// Names are not in a bank: 489 records of 8 codes, zero padded, at
// 0x0861E4C8 + id * 16, the ids the USA ROM's name table uses.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kol::jtext {

inline unsigned bank_count(const std::uint8_t* rom, std::uint32_t size, std::uint32_t bank) {
    const std::uint32_t o = bank - 0x08000000u;
    if (bank < 0x08000000u || o + 4u > size || rom[o] != 'm' || rom[o + 1] != '7') return 0;
    return static_cast<unsigned>(rom[o + 2] | (rom[o + 3] << 8));
}

// Entry `index` of the bank: its codes, the closing 0xF000 left out. Trailing
// blanks of each line (the game pads lines to the box's width) are dropped
// when `trim`. False when the bank or the entry is not there.
inline bool entry(const std::uint8_t* rom, std::uint32_t size, std::uint32_t bank, unsigned index,
                  std::vector<std::uint16_t>* out, bool trim = true) {
    out->clear();
    if (index >= bank_count(rom, size, bank)) return false;
    const std::uint32_t b = bank - 0x08000000u;
    const std::uint32_t r = b + 4u + index * 12u;
    if (r + 12u > size) return false;
    const std::uint32_t off = static_cast<std::uint32_t>(rom[r]) | (rom[r + 1] << 8) | (rom[r + 2] << 16) |
                              (static_cast<std::uint32_t>(rom[r + 3]) << 24);
    std::uint32_t p = b + off;
    if (p + 2u > size) return false;
    p += static_cast<std::uint32_t>(rom[p] | (rom[p + 1] << 8));
    for (int n = 0; n < 2048 && p + 2u <= size; ++n, p += 2u) {
        const std::uint16_t c = static_cast<std::uint16_t>(rom[p] | (rom[p + 1] << 8));
        const bool line_end = c == 0xF000u || c == 0xF100u;
        if (line_end && trim)
            while (!out->empty() && out->back() == 0x0000u) out->pop_back();
        if (c == 0xF000u) return true;
        out->push_back(c);
    }
    return false;
}

// Codes as text a log can hold: digits and Latin letters as themselves, every
// other code as <hex> (kana and kanji have no ASCII).
inline void show(const std::uint16_t* codes, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o + 8 < cap; ++i) {
        const std::uint16_t c = codes[i];
        if (c >= 0xAFu && c <= 0xB8u) out[o++] = static_cast<char>('0' + (c - 0xAFu));
        else if (c >= 0xB9u && c <= 0xD2u) out[o++] = static_cast<char>('A' + (c - 0xB9u));
        else if (c >= 0xD3u && c <= 0xECu) out[o++] = static_cast<char>('a' + (c - 0xD3u));
        else o += std::snprintf(out + o, static_cast<std::size_t>(cap - o), "<%X>", c);
    }
    if (cap > 0) out[o < cap ? o : cap - 1] = '\0';
}

// Codes as UTF-8, for a text file a person reads (a save state's notes): the
// kana, digits and Latin letters a name can be typed in, in the font's
// order from 0x0E (46 hiragana, 9 small, 25 voiced; the same of katakana,
// and vu; then 0-9, A-Z, a-z), the long-vowel mark and the blank. Anything
// else (kanji, marks) as <hex>.
inline std::string utf8(const std::uint16_t* codes, int n) {
    static const char* const kKana[2] = {
        "\u3042\u3044\u3046\u3048\u304A\u304B\u304D\u304F\u3051\u3053\u3055\u3057\u3059\u305B\u305D"
        "\u305F\u3061\u3064\u3066\u3068\u306A\u306B\u306C\u306D\u306E\u306F\u3072\u3075\u3078\u307B"
        "\u307E\u307F\u3080\u3081\u3082\u3084\u3086\u3088\u3089\u308A\u308B\u308C\u308D\u308F\u3092\u3093"
        "\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087"
        "\u304C\u304E\u3050\u3052\u3054\u3056\u3058\u305A\u305C\u305E\u3060\u3062\u3065\u3067\u3069"
        "\u3070\u3073\u3076\u3079\u307C\u3071\u3074\u3077\u307A\u307D",
        "\u30A2\u30A4\u30A6\u30A8\u30AA\u30AB\u30AD\u30AF\u30B1\u30B3\u30B5\u30B7\u30B9\u30BB\u30BD"
        "\u30BF\u30C1\u30C4\u30C6\u30C8\u30CA\u30CB\u30CC\u30CD\u30CE\u30CF\u30D2\u30D5\u30D8\u30DB"
        "\u30DE\u30DF\u30E0\u30E1\u30E2\u30E4\u30E6\u30E8\u30E9\u30EA\u30EB\u30EC\u30ED\u30EF\u30F2\u30F3"
        "\u30A1\u30A3\u30A5\u30A7\u30A9\u30C3\u30E3\u30E5\u30E7"
        "\u30AC\u30AE\u30B0\u30B2\u30B4\u30B6\u30B8\u30BA\u30BC\u30BE\u30C0\u30C2\u30C5\u30C7\u30C9"
        "\u30D0\u30D3\u30D6\u30D9\u30DC\u30D1\u30D4\u30D7\u30DA\u30DD\u30F4"};
    std::string out;
    for (int i = 0; i < n; ++i) {
        const std::uint16_t c = codes[i];
        if (c >= 0x0Eu && c < 0x0Eu + 80u) out.append(kKana[0] + (c - 0x0Eu) * 3u, 3);        // 3 bytes each
        else if (c >= 0x5Eu && c < 0x5Eu + 81u) out.append(kKana[1] + (c - 0x5Eu) * 3u, 3);
        else if (c >= 0xAFu && c <= 0xB8u) out += static_cast<char>('0' + (c - 0xAFu));
        else if (c >= 0xB9u && c <= 0xD2u) out += static_cast<char>('A' + (c - 0xB9u));
        else if (c >= 0xD3u && c <= 0xECu) out += static_cast<char>('a' + (c - 0xD3u));
        else if (c == 0x04u) out += "\u30FC";
        else if (c == 0x00u) out += ' ';
        else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "<%X>", c);
            out += hex;
        }
    }
    return out;
}

}  // namespace kol::jtext
