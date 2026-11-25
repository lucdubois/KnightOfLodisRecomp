// kol_region.h — one source, more than one build of the game.
//
// Every address of the game in this source is written as the USA ROM's (ATOE)
// and wrapped in usa(): usa(0x02029D40u). The USA build takes it as written.
// Another region's build (KOL_REGION_ATOJ: the Japanese ROM, Tactics Ogre
// Gaiden) looks it up in its table, src/kol_region_atoj.inc, which
// koldecomp/regionmap generates from the two ROMs and checks against both
// games' memory. An address that is not in the table does not compile:
// nothing is translated by guessing, and nothing is missed silently.
//
// Not wrapped, on purpose:
//   - the hardware's own bounds (0x02000000, 0x02040000, 0x03000000,
//     0x03008000, 0x08000000, 0x08800000) and the sanity bounds near them;
//   - 0x03007FF0, the sound library's pointer, the same in every game;
//   - this project's own scratch RAM at the top of EWRAM (0x0203F000 up),
//     which both games leave empty;
//   - numbers that only look like addresses (a zip signature).
//
// What usa() does NOT cover: an offset inside a structure. The one structure
// known to differ, the window control block, has usa_ctl() below; anything
// else the two builds lay out differently, the code says with kRegion.
// And what only the USA game has (its font, its text banks): usa_only().
#pragma once

#include <cstddef>
#include <cstdint>

namespace kol {

enum class Region { Usa, Japan };

#if defined(KOL_REGION_ATOJ)

inline constexpr Region kRegion = Region::Japan;

namespace region_detail {
struct Pair {
    std::uint32_t usa, here;
};
// Sorted by the USA address.
inline constexpr Pair kTable[] = {
#include "kol_region_atoj.inc"
};
}  // namespace region_detail

// consteval: every use is worked out by the compiler, so a missing address is
// a compile error at the line that uses it (the throw below is what the
// compiler points at).
consteval std::uint32_t usa(std::uint32_t address) {
    std::size_t lo = 0, hi = sizeof(region_detail::kTable) / sizeof(region_detail::kTable[0]);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (region_detail::kTable[mid].usa < address)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < sizeof(region_detail::kTable) / sizeof(region_detail::kTable[0]) &&
        region_detail::kTable[lo].usa == address)
        return region_detail::kTable[lo].here;
    throw "kol::usa: this address is not in this region's table (src/kol_region_atoj.inc)";
}

// An address only the USA game has: its font and text banks (the Japanese game
// has its own text, in its own encoding), or code the other build does not
// have. 0 in another region, so the code that uses it has to take that
// region's path (kRegion) and must not read through it.
consteval std::uint32_t usa_only(std::uint32_t) { return 0u; }
// And one only the Japanese game has (its font, its text banks), written as
// the Japanese ROM's: 0 in the USA build.
consteval std::uint32_t japan_only(std::uint32_t address) { return address; }

// An offset inside the game's window control block (the status window's and
// the roster's work area; the battle's work block is laid out alike), written
// as the USA ROM's. The Japanese build lays the block out differently: 2 to 4
// bytes less from 0x1B92, 0x54 less from 0x1C24 on. Its table,
// src/kol_region_atoj_ctl.inc, comes from the constants the two builds'
// matching code loads, and for the few those do not settle, from both games'
// memory (regionmap/make_ctl_table.py, the overrides file's `ctl` lines); an
// offset not in it does not compile.
namespace region_detail {
inline constexpr Pair kCtlTable[] = {
#include "kol_region_atoj_ctl.inc"
};
}  // namespace region_detail
consteval std::uint32_t usa_ctl(std::uint32_t offset) {
    for (const region_detail::Pair& p : region_detail::kCtlTable)
        if (p.usa == offset) return p.here;
    throw "kol::usa_ctl: this offset is not in this region's table (src/kol_region_atoj_ctl.inc)";
}

// The cartridge this build runs.
inline constexpr const char* kCartName = "Tactics Ogre Gaiden: The Knight of Lodis (Japan)";
inline constexpr const char* kCartSha1 = "ec257d645c601e439f1ab34d9b598c0443aec3da";
// What the setup screen says of it (kol_setup.cpp).
inline constexpr const char* kCartAsk = "Select the game's ROM (Tactics Ogre Gaiden: The Knight of Lodis, Japan)";
inline constexpr const char* kCartWrong = "Wrong file: not this game's ROM (the Japanese version is needed).";
inline constexpr const char* kCartVersion = "the Japanese version";
// The player's folder (kol_desktop.cpp) and the tag of the files that hold
// what one ROM's game made (kol::rom_file_tag: numbered states, the Fast
// Loading memo): this build's own, so that it never reads or writes the USA
// build's save, states or memo, whichever folder it is pointed at.
inline constexpr const char* kDataDirName = "KnightOfLodisRecomp-Japan";
inline constexpr const char* kRegionTag = "-jp";

#else

inline constexpr Region kRegion = Region::Usa;

consteval std::uint32_t usa(std::uint32_t address) { return address; }
consteval std::uint32_t usa_only(std::uint32_t address) { return address; }
consteval std::uint32_t japan_only(std::uint32_t) { return 0u; }
consteval std::uint32_t usa_ctl(std::uint32_t offset) { return offset; }

inline constexpr const char* kCartName = "Tactics Ogre: The Knight of Lodis";
inline constexpr const char* kCartSha1 = "85a46320a31f16f5c6dda27fc094edd078dbe43d";
inline constexpr const char* kCartAsk = "Select the game's ROM (Tactics Ogre: The Knight of Lodis, USA)";
inline constexpr const char* kCartWrong = "Wrong file: not this game's ROM (the USA version is needed).";
inline constexpr const char* kCartVersion = "the USA version";
inline constexpr const char* kDataDirName = "KnightOfLodisRecomp";
inline constexpr const char* kRegionTag = "";

#endif

}  // namespace kol
