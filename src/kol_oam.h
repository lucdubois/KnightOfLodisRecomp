#pragma once

// The GBA's sprite attributes (OAM): an entry's size from its shape and
// size bits, and a texel of its tiles (kol_focus.cpp, kol_widescreen.cpp).

#include <cstddef>
#include <cstdint>

namespace kol {

// Width and height by shape (0 square, 1 wide, 2 tall; 3 is no shape) and
// size.
constexpr int kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr int kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// The palette index (0 transparent) of texel (sx, sy) of an entry, its
// flips already applied: `tile` its first tile (attribute 2's low 10 bits),
// `row_tiles` the tiles from one row of 8 to the next (1D or 2D mapping),
// 8bpp or 4bpp. The tile number and the 32 KB of tiles wrap as the
// hardware's do.
inline unsigned obj_tile_texel(const std::uint8_t* tiles, unsigned tile, unsigned row_tiles, int sx, int sy,
                               bool bpp8) {
    const unsigned t = (tile + static_cast<unsigned>(sy / 8) * row_tiles +
                        static_cast<unsigned>(sx / 8) * (bpp8 ? 2u : 1u)) & 0x3FFu;
    const std::size_t base = t * 32u;
    if (bpp8) return tiles[(base + (sy & 7) * 8 + (sx & 7)) & 0x7FFFu];
    const std::uint8_t b = tiles[(base + (sy & 7) * 4 + (sx & 7) / 2) & 0x7FFFu];
    return (sx & 1) ? (b >> 4) : (b & 0x0Fu);
}

}  // namespace kol
