#pragma once

#include <vector>

// Debug > Tile Info's live editor: drawn heights, terrain types and roofs,
// kept per map in drawn_heights.cfg beside enhancements.cfg, read by
// kol_focus's terrain pass ("map x z height" lines for heights, "type map x z
// left|front|right|back" for a cell drawn with that corner rounded, "roof map
// x0 z0 x1 z1 axis gables eave ridge shape open" for roofs, eave and ridge in
// half steps; shape and open may be left out, a gable with walls; "water
// map x z water|dry" for the WATER tab's tags).
//
// While Tile Info is on and L2 is held (the game sees neither the D-pad nor
// A or B then):
//   D-pad up / down   change the field being edited: the target cell's
//                     terrain (its drawn height, or its type: square, or one
//                     corner rounded), or, on a roof, its eave, ridge,
//                     gables, axis or shape (gable, or one slope either way)
//                     (repeating when held);
//   D-pad left/right  pick the field;
//   B                 a roof's corner: the first press marks the target cell,
//                     the second makes a roof over the rectangle between;
//   A                 on a roof, delete it; elsewhere, the cell's height and
//                     type back to the game's;
//   Select            a box on the target cell (a chimney, a dormer); on a
//                     cell with one, the fields switch between the box and
//                     the rest (A then deletes the box).

namespace kol {

// The field the editor works over: the game's 32 x 32 height grid and
// kTileMargin cells more on every side (cells -kTileMargin ..
// kTileGame + kTileMargin - 1 along X and Z), so a height, a roof or a box
// can be set where a map's art goes on past the game's grid. A cell is
// tile_cell(x, z) in every call below; the file keeps x and z themselves.
constexpr int kTileGame = 32;
constexpr int kTileMargin = 32;
constexpr int kTileSide = kTileGame + 2 * kTileMargin;
inline int tile_cell(int x, int z) { return (z + kTileMargin) * kTileSide + x + kTileMargin; }
inline int tile_x(int cell) { return cell % kTileSide - kTileMargin; }
inline int tile_z(int cell) { return cell / kTileSide - kTileMargin; }
inline bool tile_in_field(int x, int z) {
    return x >= -kTileMargin && z >= -kTileMargin && x < kTileGame + kTileMargin && z < kTileGame + kTileMargin;
}

// Wraps the host's key filter (after the options menu's). Once.
void tile_edit_install();
// KOL_TILE_EDIT_SCRIPT's pad states, fed to the editor (headless tests).
void tile_edit_test_tick(unsigned long frame);

// The target Tile Info shows this frame (kol_focus): the map, the cell
// (tile_cell, -1 none) and the height it is drawn at now.
void tile_edit_set_target(int map, int cell, int drawn);

// This map's edited heights into out[kTileSide * kTileSide] by tile_cell
// (-1 where none); false when
// the map has none.
bool tile_edit_heights(int map, int* out);

// The cell's edited height, -1 none.
int tile_edit_get(int map, int cell);

// A cell's terrain type: drawn square (the game's box) or with one vertical
// corner rounded, radius 32 (a whole cell), as the castle's towers are
// (kol_focus kDrawnRounds): the left corner (-X+Z), the front (+X+Z), the
// right (+X-Z) or the back (-X-Z), as they stand on screen.
enum TileType { kTypeSquare = 0, kTypeRoundLeft, kTypeRoundFront, kTypeRoundRight, kTypeRoundBack, kTileTypes };
struct TileTyped { int x, z, type; };
std::vector<TileTyped> tile_edit_types(int map);   // the map's cells that are not square
int tile_edit_type(int map, int cell);              // kTypeSquare when none set
const char* tile_type_name(int type);               // "SQUARE", "ROUND LEFT", ...

// A roof over cells x0..x1, z0..z1: `gables` prisms side by side, their
// ridges running along X (along_x) or Z, falling from the ridge height to
// the eave at the prisms' sides; heights in half steps (8 world units).
// shape 0 is that gable; 1 and 2 a single slope instead (a lean-to), from
// the ridge height at one side of each prism to the eave at the other:
// falling toward + (1) or - (2) across the ridges' axis; 3-6 a peak at one
// corner of the rectangle (a hip, where two ridges meet): the ridge height
// over that corner, falling to the eave along the two far sides, two
// slopes meeting on the diagonal (3 the +X+Z corner, the front one on
// screen; 4 +X-Z, the right; 5 -X-Z, the back; 6 -X+Z, the left); 7-10
// the inverse, a valley (the inside corner where two ridges meet): the eave
// height at that corner (7 front, 8 right, 9 back, 10 left), rising to the
// ridge along the two far sides, the slopes meeting in a concave crease on
// the diagonal; 11 a pyramid: the ridge height over the middle, falling to
// the eave on all four sides. Gables and axis do not apply to those.
// `open`: an awning. No walls and nothing under it: it floats at its
// height (on its posts), only the art's own layer (BG1) is placed on it, the
// ground under it stays the ground, and the sun passes under its edges.
// `dx8`, `dz8`: the whole roof shifted that many eighths of a cell (-8..8)
// along X and Z, for a house the art draws off the grid (map 89's top-left
// house stands half a cell out on both); it is still picked by its cells.
struct TileRoof {
    int x0, z0, x1, z1;
    bool along_x;
    int gables;
    int eave2, ridge2;
    int shape = 0;
    bool open = false;
    int dx8 = 0, dz8 = 0;
};
std::vector<TileRoof> tile_edit_roofs(int map);

// A box standing on a cell, over the roof or the ground: a chimney (flat
// top) or a dormer (a small gable over its walls). Its footprint is
// size_x8 x size_z8 eighths of a cell (4 units each), its middle shifted
// dx8, dz8 eighths from the cell's; its walls rise to top2, its gable (if
// peak2 > top2) to peak2, in half steps, the ridge along X (along_x) or Z.
struct TileBox {
    int x, z;
    int top2, peak2;
    int size_x8, size_z8;
    int dx8, dz8;
    bool along_x;
};
std::vector<TileBox> tile_edit_boxes(int map);

// A map's sun set in the editor (the SUN tab's SET SUN): its height over the
// ground in degrees and the direction toward it along the ground (X, Z),
// as kol_focus kMapSuns holds them; it takes the place of the map's row
// there. Saved as `sun map degrees x z` lines.
struct TileSun { float degrees, x, z; };
bool tile_edit_sun(int map, TileSun* out);
void tile_edit_set_sun(int map, const TileSun& sun);
bool tile_edit_clear_sun(int map);   // false when the map had none
// A map set indoors in the editor (the SUN tab's INDOOR): no sun there
// (kol_focus). Saved as `indoor map` lines.
bool tile_edit_indoor(int map);
void tile_edit_set_indoor(int map, bool indoor);

// Water tags (the second screen's WATER tab): a cell counted as water, or
// as not water, whatever the game's terrain table says (kol_focus
// water_mask: water is the terrain's class 2, records 10-12, and a story
// scene's map has none). A water cell the terrain pass would not place (off
// the field, or past the game's grid, as map 89's harbour is) is placed
// flat at its drawn height, 0 when it has none. And ice (asked 2026-10-27
// for an ice brush on the bottom screen, as there is one for water): a cell counted as ice (kol_focus ice_cells: the ice effect,
// with Options > Water), not water; dry is neither. Saved as `water map x z
// water|dry|ice` lines.
enum WaterTag { kWaterAuto = 0, kWaterYes = 1, kWaterDry = 2, kWaterIce = 3, kWaterTags };
// This map's tags into out[kTileSide * kTileSide] by tile_cell (kWaterAuto
// where none); false when the map has none.
bool tile_edit_water(int map, signed char* out);
// The brush over the rectangle between two cells (kWaterAuto clears).
void tile_edit_paint_water(int map, int cell0, int cell1, int tag);
// The WATER tab's brush, and its name ("WATER", "DRY", "ICE", "AUTO": the game's).
int tile_edit_water_brush();
const char* tile_water_name(int tag);

// What the readout shows of the editor: the field the D-pad edits (0 the
// cell's terrain, 1 eave, 2 ridge, 3 gables, 4 axis, 5 shape, 6 open,
// 7 shift x, 8 shift z), the
// terrain's own field (0 height, 1 type) and a roof corner waiting for its
// second (pending, cell).
int tile_edit_field();
int tile_edit_terrain_field();
bool tile_edit_corner(int map, int* cell);
// The fields edit the target cell's box (Select), and which: 0 top, 1 peak,
// 2 size X, 3 size Z, 4 shift X, 5 shift Z, 6 axis.
bool tile_edit_box_focus();
int tile_edit_box_field();

// ── For the second screen's editor page (kol_edit_panel) ──
// The grid Tile Info shows, published by kol_focus each frame it is on: the
// map, each cell's drawn height, its flags (kTileWalkable, kTileOffField),
// and the target cell.
constexpr int kTileWalkable = 1, kTileOffField = 2, kTileWater = 4;   // water: the game's or a tag
struct TileGrid {
    int map = -1;
    int target = -1;
    int drawn[kTileSide * kTileSide];
    unsigned char flags[kTileSide * kTileSide];
};
void tile_edit_publish(int map, const int* drawn, const unsigned char* flags, int target);
// The last published grid; false when none was published in the last second.
bool tile_edit_grid(TileGrid* out);
// Tile Info's target: the game's cell, or a cell the page picked, until the
// game's cursor leaves the cell it was on at the pick (kol_focus).
void tile_edit_pick(int cell);
int tile_edit_resolve_target(int game_cell);
// The page's modes: 0 the cell's terrain (height, type), 1 its roof, 2 its
// box, 3 the map's sun, 4 its water.
int tile_edit_mode();
void tile_edit_set_mode(int mode);
// The field the page shows: its name and value, and whether -/+ apply.
struct TileField { char name[16]; char value[24]; bool live; };
TileField tile_edit_field_now();
void tile_edit_step_field(int dir);   // the next (+1) or previous (-1) field
void tile_edit_change_now(int delta);   // as L2 + up / down
void tile_edit_delete_now();            // the box, the roof, or the height and type back
void tile_edit_add_box_now();           // as L2 + Select on a cell without one
// A roof over the rectangle between two cells, its eave at eave2 (half
// steps; -1: the target's drawn height), replacing any it overlaps.
void tile_edit_make_roof(int map, int cell0, int cell1, int eave2 = -1);

}  // namespace kol
