#pragma once

#include <cstdint>

// The composed battle map, read from the guest's own EWRAM array.
//
// The guest keeps the WHOLE map composed as tilemap entries in EWRAM and
// blits a 34-column window of it into VRAM as the camera moves. The VRAM
// tilemap is therefore a lagging cache, not the source: past the window the
// guest keeps only two tile columns, so a widened view runs off the end of
// what has been copied. The array does not lag, so the margins can be drawn
// from it exactly.
namespace kol {

// Per battle-map frame: re-read the array pointer and the camera origin, and
// verify the array against every cell the guest has actually drawn. Arms the
// lookup below only when that verification passes.
void map_source_tick(const std::uint8_t* ewram, const std::uint8_t* vram,
                     const std::uint8_t* io, bool battle_map);

// True when the last tick verified. Margin policy that has a fallback should
// ask, rather than assume.
bool map_source_armed();
// While BG1's margins come from its array as last verified whole (the guest
// is still putting it back after loading the map again: map 84's scene),
// writes that array over the guest's in a copy of EWRAM, so a presenter
// reading the copy sees the BG1 the picture has. Otherwise nothing.
void map_source_bg1_kept_into(unsigned char* ewram_copy, unsigned long size);
// Serving, but re-armed on BG0 alone after a short fallback (map 84's scene
// loading its map again): BG1's array is not whole yet. A reader that
// records the arrays (the highlight's world shadow) waits.
bool map_source_rearmed();

// The tilemap entry the guest WOULD have written for this margin sample, or
// false when there is none (off the map, or not armed).
bool map_source_entry(int bg, int hw_x, int screen_y, std::uint16_t* out);
// The map extension's entry for a sample the guest left EMPTY (both layers of
// the composed map are 0 there), by map_source_entry's own mapping; false
// when there is none. For the native screen, where the guest's VRAM holds a
// 0 entry over the cut past a map's art (tools/map_editor, map_ext.cfg).
bool map_source_fill_entry(int bg, int hw_x, int screen_y, std::uint16_t* out);
// The map override's entry (map_ext.cfg "ov" lines) for a native-screen
// sample whose guest entry is `cur`, by map_source_entry's own mapping; false
// when there is none or the guest's entry is no longer the one painted over.
bool map_source_override_entry(int bg, int hw_x, int screen_y, std::uint16_t cur,
                               std::uint16_t* out);
// The layers the current map has overrides on (0: none, or not armed).
unsigned map_source_override_layers();
// The fine-X phase of map_source_entry's column grid for `bg`: within one
// scanline its answer depends on hw_x only through floor((hw_x + phase) / 8).
// -1 when it cannot answer for this layer at all (not armed, or not BG0/BG1).
int map_source_x_phase(int bg);

// The world cell (array row and column) a screen sample lands on, by exactly
// the mapping map_source_entry uses. False when not armed, not BG0/BG1, or
// off the array.
// map_source_world_cell's mapping itself: row = base_row + floor((screen_y +
// fine_y) / 8), col = base_col + floor((hw_x + fine_x) / 8). False when not armed.
bool map_source_cell_mapping(int bg, int* base_col, int* base_row, int* fine_x, int* fine_y);
bool map_source_world_cell(int bg, int hw_x, int screen_y, int* out_row,
                           int* out_col);

// The composed array itself, for a caller that wants every cell rather than
// one sample: host pointer to row 0 (stride 0x100 bytes, 128 u16 entries per
// row) and its row count. Null when not armed or not BG0/BG1.
const std::uint8_t* map_source_array(int bg, int* out_rows);

// Where the composed map's art is: which tiles of the array hold anything on
// BG0 or BG1 (filled[row * kCols + col]). Past them the guest draws nothing,
// and a view wider or taller than the map shows the backdrop there: a cut
// along tile edges, which can step from row to row. Hardware pixel (hx, hy)
// is array pixel (hx + origin_x, hy + origin_y) for the BG0 scroll (hofs,
// vofs) the tick saw; for another scroll of the same ring, shift by the
// difference (a picture presented a frame later than the tick). False when
// not armed.
// filled[] is 1 for a tile with art on either layer; bg1[] is the guest's
// own BG1 entry there (map 98's and 99's flames, kol_focus flame_cards).
struct MapExtent {
    static constexpr int kMaxRows = 256, kCols = 128;
    int rows = 0;
    std::uint8_t filled[kMaxRows * kCols];
    std::uint16_t bg1[kMaxRows * kCols];
    // BG1 as drawn: the guest's entry with the map editor's override or
    // extension over it, as map_source_entry serves it (kol_focus: a map's
    // BG1 props past the picture, bg1_shape_props).
    std::uint16_t bg1_drawn[kMaxRows * kCols];
    int origin_x = 0, origin_y = 0, hofs = 0, vofs = 0;
};
bool map_source_extent(MapExtent* out);

}  // namespace kol
