// kol_a7.h — the cartridge's compressed-asset format, decoded from ROM.
//
// Knight of Lodis stores its UI art (the status sheet's tiles, labels, icons,
// digits) in "A7" archives, not as raw tiles. An earlier note called this a
// bespoke codec that could not be read; it is the game's own decoder at ROM
// 0x08004990, transcribed here. See README "A7 archives".
//
// Archive layout, all big-endian, offsets relative to the archive start:
//   +0  bytes 0x41 0x37 ("A7")
//   +2  u16 entry count
//   +4  u32 offset of the shared dictionary header
//   +8  u32 offset of each entry's command stream, one per entry
//
// Dictionary header: u8 mode, u8 k, then for modes 0-2 a u16 output size;
// the dictionary bytes follow. Modes 3-5 carry the size at the start of each
// command stream instead. Back-references copy from the DICTIONARY, never
// from earlier output.

#pragma once

#include <cstdint>
#include <vector>

namespace kol {

// Decode one command stream against a dictionary header. Both are ROM
// addresses. Fails closed (false, `out` untouched) on anything malformed or
// on a mode not transcribed yet (2 and 5).
bool a7_decode(const std::uint8_t* rom, std::uint32_t rom_size,
               std::uint32_t stream, std::uint32_t dict,
               std::vector<std::uint8_t>* out);

// Entry count of the archive at `archive`, or -1 if it is not one.
int a7_count(const std::uint8_t* rom, std::uint32_t rom_size,
             std::uint32_t archive);

// Decode entry `index` of the archive at `archive`.
bool a7_entry(const std::uint8_t* rom, std::uint32_t rom_size,
              std::uint32_t archive, int index,
              std::vector<std::uint8_t>* out);

}  // namespace kol
