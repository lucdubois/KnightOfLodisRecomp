#pragma once

// kol_focus_tables.h — the kol_focus files' per-map and per-record data:
// which terrain records are props and how they stand, the cut-out masks, the
// drawn heights and rounded boxes, and each map's own light (its sun,
// darkness, lamps, edge fade, sunset, windows). Each entry keeps the
// comment of who asked for it and on which state. The code that reads them
// is in kol_focus.cpp and kol_focus_*.cpp (kol_focus_internal.h).

#include "kol_focus.h"   // kol::kSky*

#include <cstddef>
#include <cstdint>


namespace kol_focus_detail {
// Whether a record list (kCutoutRecords, kTreeRecords, ...) holds `v`.
template <std::size_t N>
constexpr bool in_list(const int (&list)[N], int v) {
    for (int r : list)
        if (r == v) return true;
    return false;
}
// A cut-out mask's row count (kTrunk34, kBush63, ...).
template <std::size_t N>
constexpr int rows_of(const char* const (&)[N]) { return static_cast<int>(N); }
constexpr int kCutoutRecords[] = {32, 33, 42, 46, 50, 51, 53, 54, 56, 57, 70, 76, 79, 83, 84, 86, 87};   // rocks, pots, the torch, the gargoyle,
                                                                           // map 54's statues (FU: 2,13 and 12,2)
// 32: the marsh's reeds (map 16's 4,6 and 8,8, byte 0x0E; states/thor/0080,
// the user asked for them to be like the bushes, V anchored and upright). 0x0E is no
// prop byte, so they lay flat on the water. The same record is the
// snowfield's dark bushes (map 17, byte 0x0A, a cut-out already by its
// byte); 0x0E carries it on maps 7, 16, 19-23, 27, 28, 33, 106, 107 and 114.
// In kMiddleRecords and kVMiddleRecords already: standing in the middle,
// bent in a V there; and a plant (kPlantRecords), swaying.
// 83, 84: map 39's suits of armour (the throne hall, states/thor/0058; their
// two facings, on map 39 only), on the user's drawn heights for their
// pedestals; at the cell's corner (the user's pick A of
// shots/throne39/statues_AB.png; B, in the middle, darkened the pedestal).
// 56: the ice crystals (map 85, bytes 0x0B-0x0D; JX, the user's slot 3: they
// lay flat on the ground; the user asked for them upright and anchored in a
// V like most stones and bushes: kMiddleRecords and kVMiddleRecords too).
// 57: map 88's crystals (bytes 0x08 and 0x16, the only map with them; JY,
// the user's slot 3: the user asked for them anchored on their V shape),
// as map 85's.
// 46: the dead trunks. Byte 0x09 on maps 26 and 108 (II, DL), where the
// spire rule stands them up; byte 0x0B on map 34, no prop byte, so they lay
// flat on the ground there (IJ, the user's slot 3: 0,10 and 12,5).
// Cut-outs drawn by the user, pixel for pixel: the card takes these pixels
// and nothing else of the cell (its BG1 lifted, its BG0 copied, as the V's).
// The trunk's BG1 tile holds the snow diamond it stands on too, and that went
// up with it (the user asked for a precise cut instead of the whole V tile;
// painted on shots/trunk_mask/trunk_paint.png, from IM: both map 34
// trunks are the same art). Rows from the cell's top corner (sx, y0), '#'
// taken. KOL_NO_DRAWN_CUTOUTS=1: the strip and its V, as before.
// `cx`, `cz`: one cell only (-1: every cell of the record on the map).
struct DrawnCutout { int map, record, dx, dy; const char* const* rows; int count; int cx = -1, cz = -1; };
constexpr const char* kTrunk34[] = {
    "......##.........",
    "......####.......",
    ".....#######.....",
    ".....#######.....",
    ".....#######.##..",
    ".....##########..",
    ".....##########..",
    ".....########....",
    ".....########....",
    ".....#######.....",
    "....########.....",
    "....##########...",
    "...############..",
    "###############..",
    "#################",
    "#######.#########",
    "..#.###..###..#..",
    "....#....#.#.....",
};
// Map 12's leafless tree at 6,0 (record 40, a tree by the trees' rule; IX,
// the user's slot 3: the user offered to draw on the leafless tree for it to
// be lifted; shots/tree_mask/, painted square by square). Every record-40 tree on
// map 12: 9,8's BG1 is the drawing square for square (IY: it did not work on
// the tree in the middle of the screen; its colours differ by the light).
constexpr const char* kTree12[] = {
    ".......##..................",
    "......###..................",
    "......###...............##.",
    "......###.......#.......###",
    "......###......###......###",
    "##....###......###......###",
    "###..####......###......###",
    "###..###..##...###......###",
    "###..###..###..###.....###.",
    "###.####..###..###.....###.",
    "###.###...####.###....###..",
    ".######....#########..###..",
    ".######....#########.###...",
    "..######..#########.######.",
    "...##########.####.########",
    "....########..#############",
    "......#####....######......",
    ".......####....#####.......",
    ".......###.....####........",
    ".......##......###.........",
    "....##.##......###.........",
    "....######.....###.........",
    ".....#####....####.........",
    "......#######.####.........",
    ".......###########..##.....",
    "........#########..###.....",
    ".......##############......",
    ".......#############.......",
    "..........#########........",
    "..........########.........",
    ".........########..........",
    "............#####..........",
    "...........#######.........",
};
// Map 85's ice crystals (record 56; JX, the user's slot 3): the crystal and
// the ice face behind it share their tiles, outline colour and a stripe
// colour, so the strip took a piece of the face with five of the six (one
// crystal under the blond-haired unit took the tile behind it).
// The user suggested applying the exact shape of the crystal on the far left
// of the blonde unit, which has the same shape. Two shapes:
// the tall one (bytes 0x0C, 0x0D: 4,13, 12,12, 14,16) cut from 4,13's
// lifted pixels, the short one (0x0B: 6,10, 10,15, 5,18) from 6,10's, both
// clean of the face (KOL_FOCUS_DUMP .lift, with the Thor's drawn heights).
constexpr const char* kCrystalTall85[] = {
    ".........###...................",
    ".........####..................",
    ".........#####.................",
    ".........######................",
    "........#########..............",
    "........##########.............",
    "........###########............",
    "........###########............",
    "........###########............",
    ".......#############...........",
    ".##....#############...........",
    ".###...#############...........",
    ".#####.#############.....####..",
    ".####################...#####..",
    ".####################..######..",
    ".####################.#######..",
    ".############################..",
    ".############################..",
    ".############################..",
    ".#############################.",
    "##############################.",
    "###############################",
};
constexpr const char* kCrystalShort85[] = {
    "....##........................",
    "....###.......................",
    "...#####......................",
    "...######.....##..............",
    "...#######...###..............",
    "...########.#####......###....",
    "..###############.....####....",
    "..###############....######...",
    "..################..#######...",
    "..################.#########..",
    "..##########################..",
    ".###########################..",
    ".###########################..",
    "#############################.",
    "##############################",
};
// Map 88's crystals (record 57; JY, the user's slot 3). The user asked to
// reuse the cutout of the same crystal elsewhere on the map. Two shapes, by
// map byte: the twin (0x16: 3,1, 9,6, 3,12) cut from 9,6's lifted pixels,
// the tall one (0x08: 8,1, 1,8, 10,13) from 1,8's (KOL_FOCUS_DUMP .lift,
// with the Thor's drawn heights); 3,12 and 10,13 are the same pixel for
// pixel. 10,13's stands on a pillar whose top and upper walls are BG1 with
// it: the strip took them, and its art seemed to end at the front corner.
constexpr const char* kCrystalTwin88[] = {
    "................#...........",
    "...............###..........",
    "...............###..........",
    "..............#####.........",
    "..............#####.........",
    ".............#######........",
    ".........#...#######........",
    "........###.#########.......",
    "........###.#########.......",
    ".......###############......",
    ".......###############......",
    "......################......",
    "......################......",
    ".....#################......",
    ".....#################......",
    ".....#################......",
    ".....#################......",
    ".....###################....",
    ".....###################....",
    "..##.###################....",
    "..######################.##.",
    "..#########################.",
    "..#########################.",
    "############################",
};
constexpr const char* kCrystalTall88[] = {
    "............##............",
    "............##............",
    "...........####...........",
    "...........####...........",
    "..........######..........",
    "..........######..........",
    ".........########.........",
    ".........########.........",
    "........##########........",
    "........##########........",
    ".......############.......",
    ".......############.......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "......##############......",
    "....################..##..",
    "....################.###..",
    "....####################..",
    "##..####################..",
    "########################..",
    "##########################",
    "##########################",
    "#########################.",
};
// Map 7's tree at 12,1 (record 40; states/thor/0028, the user asked for this
// tree to be done as on map 102, so it does not take the ground
// behind its base): painted by the user square by square
// (shots/tree_mask_7/, read at each square's middle 8 x 8). Its art is not
// map 12's (291 BG1 pixels outside that drawing at the same anchor).
constexpr const char* kTree7[] = {
    "...............#.............",
    "..............###............",
    "........###...###............",
    "........####.####............",
    "..##.....#########...........",
    "..####...#########...###.....",
    "...################.######...",
    "....#######################..",
    "###.#######################..",
    "############################.",
    ".#######################..##.",
    ".########################....",
    "..######################.....",
    "...#######################...",
    "....#######################..",
    "...#########################.",
    "..###########################",
    ".############################",
    ".#######################...##",
    ".....##################......",
    "......################.......",
    "......################.......",
    ".....##############.##.......",
    ".....##############..#.......",
    "......#############..........",
    ".......############..........",
    "........####.######..#.......",
    "..........########..###......",
    "........##############.......",
    ".........############........",
    "............########.........",
    ".............#####...........",
    ".............#####...........",
    "..............#.#............",
};
// Map 63's bush (the night camp's ferns, BG0 off the grid), painted by the
// user square by square (shots/bush_mask_63/, read at each square's middle
// 8 x 8): the user noted the bushes all seem to share the same shape.
constexpr const char* kBush63[] = {
    ".............##.................",
    "....##.......###....##..........",
    "....###......###..####..........",
    ".....###.....###.####...........",
    "......###....##.####..##....##..",
    "......####...#######.###...###..",
    ".......#####.#######.###.####...",
    ".....##################.#####...",
    "....########################....",
    "...########################.....",
    "..##############################",
    "..##..#########################.",
    "......#####################.##..",
    "..########################..#...",
    ".#############################..",
    "##############################..",
    ".....#######################....",
    "......##################..##....",
    ".........###############........",
    ".........###.#..#....#..........",
    ".........##.....................",
};
// Drawn cut-outs placed by where their art is, not by a cell (art past the
// grid, with no record): `rows` at (ax, ay) from the map's art origin (the
// scene's kx + ox, ky + oy; a template pixel (x, y) of shots/camp63 is (x -
// 184, y - 52)), standing where its lowest taken row meets the floor at
// `ground`. Map 63's bushes: the painted one at template (130, 122) and
// every place it matched the art (shots/bush_mask_63/candidates.png): 1-5;
// 6-8 skipped (the user chose to skip 6, 7 and 8 as they are far from
// the lighting). KOL_NO_PLACED_CUTOUTS=1: flat.
// plant: it sways with the wind (Options > Plant Sway), as the trees and
// bushes found by their records do (the user, map 63: asked for the plants
// to sway on this map too).
struct PlacedCutout { int map, ax, ay, ground; const char* const* rows; int count; bool plant = true; };
constexpr int kBush63Rows = rows_of(kBush63);
constexpr PlacedCutout kPlacedCutouts[] = {
    {63, 130 - 184, 122 - 52, 1, kBush63, kBush63Rows},
    {63, 76 - 184, 131 - 52, 1, kBush63, kBush63Rows},
    {63, 285 - 184, 160 - 52, 1, kBush63, kBush63Rows},
    {63, 276 - 184, 97 - 52, 1, kBush63, kBush63Rows},
    {63, 125 - 184, 20 - 52, 1, kBush63, kBush63Rows},
};
// A per-map table's entry for `map` (its `map` field), the first; null none.
template <class T, std::size_t N>
const T* by_map(const T (&table)[N], int map) {
    for (const T& t : table)
        if (t.map == map) return &t;
    return nullptr;
}
constexpr DrawnCutout kDrawnCutouts[] = {
    {34, 46, -8, 1, kTrunk34, rows_of(kTrunk34)},
    {12, 40, -13, 3, kTree12, rows_of(kTree12)},
    // Map 17's (JA, 18,0: the user asked for it to match the drawn one): its
    // BG1 is the drawing square for square at the same anchor.
    {17, 40, -13, 3, kTree12, rows_of(kTree12)},
    // Every record-40 tree on map 7 is the same art (12,1, 7,11, 0,5, 4,1,
    // 6,12: BG1 pixel for pixel at the same anchor, 0028-0030; 4,1's trunk
    // and 6,12 partly hidden in some states, which made them look different).
    {7, 40, -14, 1, kTree7, rows_of(kTree7)},
    {85, 56, -15, 2, kCrystalTall85, rows_of(kCrystalTall85), 4, 13},
    {85, 56, -15, 2, kCrystalTall85, rows_of(kCrystalTall85), 12, 12},
    {85, 56, -15, 2, kCrystalTall85, rows_of(kCrystalTall85), 14, 16},
    {85, 56, -15, 1, kCrystalShort85, rows_of(kCrystalShort85), 6, 10},
    {85, 56, -15, 1, kCrystalShort85, rows_of(kCrystalShort85), 10, 15},
    {85, 56, -15, 1, kCrystalShort85, rows_of(kCrystalShort85), 5, 18},
    {88, 57, -14, 0, kCrystalTwin88, rows_of(kCrystalTwin88), 3, 1},
    {88, 57, -14, 0, kCrystalTwin88, rows_of(kCrystalTwin88), 9, 6},
    {88, 57, -14, 0, kCrystalTwin88, rows_of(kCrystalTwin88), 3, 12},
    {88, 57, -13, 3, kCrystalTall88, rows_of(kCrystalTall88), 8, 1},
    {88, 57, -13, 3, kCrystalTall88, rows_of(kCrystalTall88), 1, 8},
    {88, 57, -13, 3, kCrystalTall88, rows_of(kCrystalTall88), 10, 13},
};
// Record 31 is record 30's bush in flower (pink; byte 0x0D beside 30's 0x0C
// on maps 7, 10, 11, 16, 19-23, 27, 28, 33, 106, 107, 114), and was in no
// list: drawn flat (states/thor/0025, map 7's 8,5, the user reported it was
// not lifted as a prop). It takes every rule 30 has.
constexpr int kTreeRecords[] = {30, 31, 40, 41, 45, 47, 55};         // trees, bushes
// Of those, the plants whose cards bend along their cell's front edges: the
// bushes (30: map 34's, and on maps 3, 8, 12, 13, 17, 24, 29 and 32) and the
// pines (41: maps 29, 34 and 40). Their art ends on the cell's middle row.
constexpr int kVPlantRecords[] = {30, 31, 41};
constexpr int kPineRecord = 41;
// Cut-outs standing in their cell's middle that bend the same way, their
// cell's top's front half up with them: the cave's stalagmites (54, map 51;
// kMiddleRecords) and the stones standing by their foot (50, kFootRecords;
// map 49's at 10,1, its foot on the middle row) and the cut-out bushes (42,
// kMiddleRecords; map 13's at 7,5 and 13,4, IZ: the user asked for the same
// treatment as the pine trees and other bush) and the rocks (51, kMiddleRecords; map 17's twin rocks at
// 1,0, JB, on the user's drawn floor 4), whose art ends on the middle row as
// a pine's does, and the ice crystals (56, map 85, JX; 57, map 88, JY). Only when the card
// is in the middle.
constexpr int kVMiddleRecords[] = {54, 50, 42, 51, 32, 56, 57};
// Props that are plants, which sway with the wind (Options > Plant Sway):
// the trees' records and map 3's bushes (42; the snowfield's dry bushes are
// 42 too), and the reeds and dark bushes (32).
constexpr int kPlantRecords[] = {30, 31, 40, 41, 45, 47, 55, 42, 32};
constexpr bool plants_hold_trees() {
    for (int t : kTreeRecords)
        if (!in_list(kPlantRecords, t)) return false;
    return true;
}
static_assert(plants_hold_trees(), "kPlantRecords lists every kTreeRecords entry");
// Never props, whatever the map's byte: buildings. Map 13's houses are
// record 71 (BT's church) under byte 0x0A, the pots' byte, and stood as
// cut-outs over the user's roofs (FY, 2026-10-08).
// KOL_PERSP_NO_TERRAIN_RECORDS=1 lets them be props again.
// Map 6's chimney house is record 78, its front corner (0,16) under byte 0x0A
// too: its gable and chimney stood up as that cell's card (state GP, mesh
// paint), and swayed with the bushes in front before the plant flag came
// from the claiming cell.
constexpr int kTerrainRecords[] = {71, 78};
// ... and on one map only: map 39's walls (record 60, the ruins' pillars'
// record) under bytes 0x09 and 0x0A, the prop bytes, at its windows (10,1,
// 11,1, 15,1, 16,1): taken as cut-outs, their height came down to the floor
// in front (7) and the window's left side stretched as the camera turned
// (the user, states/thor/0059: it was treated as if x 15 z 1 were empty,
// but it is a wall). Maps 52 and 79 have record 60 under
// those bytes too (not looked at: no state).
// Map 90's row z 0 (x 0-8, record 50, the stones' record, byte 0x06) is a walk
// route over open water: no stones there. As cut-outs they took 537 pixels of
// the boat's far rail and ropes (flat over those cells) and stood them up at
// the cells' depth, and kept the model's place under them, so the water beside
// them rose in a fan.
struct MapTerrainRecord { int map, record; };
// Map 75's stalagmites (54) and twin domes (51) are its model's (tools/models/room75.py):
// as cards their tops, on BG1 among the repaint of the rock behind, came away.
constexpr MapTerrainRecord kMapTerrainRecords[] = {{39, 60}, {90, 50}, {75, 54}, {75, 51}};
// Cut-outs whose art stands on a pedestal drawn on BG0 below their BG1
// body: the pedestal joins the cut-out. Its top is `steps` under the
// stored top at the cell's middle; it lies within `half` columns of the
// cell's middle. The gargoyle (86, states/EN_slot1_gargoyle_pit): body on
// BG1 down to the rim's 11, a BG0 vase under it in the pool, measured on
// the flat frame (README "Perspective: drawn heights").
// `copy`: the BG0 pedestal is copied onto the cut-out's card (and stays the
// terrain's, as a spire's V: g_spire_v), so it moves with the card exactly.
struct Pedestal { int record, steps, half; bool copy = false; };
// Cut-outs whose art stands in the middle of their cell, not down to its
// front corner: map 54's statues (87, states/FU). Stood at the corner like
// a rock, the base read as a step over the floor and the statue floated
// over it under the stick, its dark base left on the floor. The bushes of
// map 3 (record 42, states/GJ_slot3_bushes_floating) too: at the corner the
// one by the church floated over the grass and another hung out past its
// ledge as the stick lowered the camera. And map 3's rocks (51,
// states/GK_slot3_rocks_floating): at the corner the one at the map's edge
// floated, its foot cut off flat. (The snowfield's stone, 50, FK, keeps
// the corner: at the middle it sank.)
// And the ruins' fire pots (76, states/GQ): at the corner their bowls hung
// out past the ledge in front as the stick lowered the camera; and their
// stalagmites (54) the same.
// And the snowfield's dark bushes (32, a cut-out by its byte 0x0A; map 17,
// JB): at the corner their shadow fell in front of them and they looked to
// float (the user reported they seemed to float, the shadow badly
// placed); their art ends on the middle row.
// And map 85's ice crystals (56, JX) and map 88's (57, JY), as the stones and bushes.
// (79: the castle hall's torch stands, map 56; the user, 2026-11-16: asked for the torches
// to be props. As the fire pots, 76.)
constexpr int kMiddleRecords[] = {87, 42, 51, 76, 79, 54, 32, 56, 57};
// Of those, the statues, whose body is taken by its outline (below).
constexpr int kStatueRecords[] = {87};
// Cut-outs that stand where their own art meets the floor (below): the
// stones (50: map 2's in the rain, HY; the snowfield's, FK; DQ's).
constexpr int kFootRecords[] = {50};
// Special cases: a cut-out in a V whose two arms stand on floors a step
// apart: bent at its cell's front corner, each arm on its own floor (steps).
// Map 2's rock beside the cliff path at 3,2 (states/thor/0012, the user:
// a V-shaped prop, but the left side of the V a step higher than the
// bottom side, a special case): the V is the foot of
// its cell's front walls, each on its neighbour's floor: the left arm on
// 3,3's (6), the right on 4,2's (5), where the rock's art ends (measured
// over the flat frame; on its own top, 7 and 6, the lines cut through the
// rock: the left side did not follow the tile's front edge).
// Map 7's stone at 15,7 (stored 5; states/thor/0024, the user reported it
// detached from the ground on the left side): the same, left on 15,8's
// floor (3), right on 16,7's (2), measured over the flat frame.
// KOL_NO_V_STEPS=1: the rule they had (a flat card).
struct VStepCutout { int map, x, z, left, right; };
constexpr VStepCutout kVStepCutouts[] = {{2, 3, 2, 6, 5}, {7, 15, 7, 3, 2}};
// Special cases: a stone whose art fills its cell's front half, its lower
// right painted on BG0: stood in the middle and bent in a V there, taking
// its cell's top's front half, as kVMiddleRecords' cards do, though its
// foot (BG1 down to the front corner on its left) puts it at the corner.
// Map 7's stone at 8,9 (states/thor/0025, the user reported the same problem
// as the stone before, on the right side; 9,9's grass cut into its lower
// right, left flat on its cell's top under the stick).
// KOL_NO_MIDDLE_V_CELLS=1: by its foot, as before.
struct MiddleVCell { int map, x, z; };
constexpr MiddleVCell kMiddleVCells[] = {{7, 8, 9}};
constexpr Pedestal kPedestals[] = {{86, 4, 11}};
// Cut-outs standing on top of their cell's block: the card is the art over
// the middle of the cell's top (at its drawn height) and stands there; the
// art under it (a pedestal) stays the block's terrain. Map 39's suits of
// armour (83, 84; states/thor/0058): their pedestal is a box one step tall,
// its top edge at the middle of the cell's top at the user's drawn height
// (5, over the floor's 4); its left half is BG0, its right half BG1. Lifted
// with the statue, the right half went with the card and the left half lay
// on the block (the statues' bottom left came away from their
// pedestal); stood with the card or copied onto it, the block's own art
// still showed beside it with the camera up (still detached looking up).
// KOL_TOP_STAND_LIFT_ALL=1: the whole strip, at the corner (as pick A).
constexpr int kTopStandRecords[] = {83, 84};
// The ruins' fire pots (record 76) stand 3 steps over their ground, flame
// included: a cut-out's strip starts there, not at the cell's stored top.
// Map 51's pot at 6,4 is stored a step higher (15 over 11), and its strip's
// top 8 rows took the round urn beside it, which floated with the pot under
// the stick (state GQ). KOL_PERSP_POT_FROM_TOP=1: from the stored top.
constexpr int kPotRecord = 76, kPotSteps = 3;
// Torch light from places with no torch drawn (a cell and its flame's height in steps): a
// blue fire pot's light, flicker and lean. Map 56's hall has torch stands up its left stair
// only (cells 2, 9 / 13 / 17); the user, 2026-11-16, asked for blue torch light from an
// imaginary X 14 row at Z 9, 13 and 17: the same three mirrored past the map's
// right edge, at the left ones' heights (17, 14, 11).
// A piece of the map the game moves onto its interface layers to animate it (map 88's
// bridge collapsing: BG2 and BG3 scrolled down, with sprites for the rubble; Thor state
// 0294, the user noted it was treated as an interface element, unaffected by lighting
// and camera angle changes): while a story event shows BG2 or BG3 with the OBJ
// window's bit set (dispcnt 0x9740 / 0x9F40 / 0x9B40; the battle's own is 0x1F40), those
// layers and the interface sprites are one card standing at this world place (the
// bridge's middle), lit as the ground round it (kol_perspective, ScenePixels::fall_card).
// The card lies in a plane of the world, given by three of its points at height y: the
// line where the piece is joined to what stays, (x0, z0)-(x1, z1) (the bridge's end on
// the castle's terrace, the user asked to anchor the whole line joined to the castle),
// and a point along the piece, (x2, z2) (then asked for it to follow the angle of
// the rest of the map on a perspective shift). Each goes where the warp puts it, the
// card affine between.
struct FallCard { int map, x0, z0, x1, z1, x2, z2, y; };
constexpr FallCard kFallCards[] = {{88, 160, 704, 320, 704, 160, 928, 160}};

struct PhantomTorch { int map, x, z, top; };
constexpr PhantomTorch kPhantomTorches[] = {{56, 14, 9, 17}, {56, 14, 13, 14}, {56, 14, 17, 11}};
// Heights the art draws that differ from the stored ones. The grid holds
// walkable heights, and a cell no unit enters keeps whatever the map gave
// it. These feed cell_height, so the terrain pass, the shadows, the
// occlusion and the torchlight follow; the game, the cursor and the units
// keep the game's own heights. By map and cells (with the record the cells
// must hold, -1 any) for one spot. (By record, for a shape the same on every
// map, there was kDrawnRecords, steps added to the stored height; it never
// had an entry and is gone.) KOL_PERSP_NO_DRAWN_HEIGHTS=1 draws the stored
// heights.
// Under a cover the art did not darken: the world box of a hollow (a model's roof over
// cells drawn low), where every pixel keeps only `keep_in` of its light at the box's
// back (Z z0) rising to `keep_out` at its mouth (Z z1). The sun's shadow alone (0.5)
// left the water under map 21's bridge a pale blue (the user asked for the shadows
// there to be darker). The box starts in front of the mouth at full light
// and eases in (a box ending at the mouth at 0.6 drew a line across the water; the
// user wanted no line, just a gradual darkening). KOL_NO_COVER_SHADE=1: off.
struct CoverShade { int map; float x0, x1, z0, z1, y1, keep_in, keep_out; };
#define KOL_LAGOON_COVER(m) {m, 152.0f, 290.0f, 31.0f, 120.0f, 84.0f, 0.3f, 1.0f}
constexpr CoverShade kCoverShades[] = {KOL_LAGOON_COVER(21), KOL_LAGOON_COVER(22), KOL_LAGOON_COVER(23),
                                       KOL_LAGOON_COVER(24), KOL_LAGOON_COVER(25), KOL_LAGOON_COVER(114)};
#undef KOL_LAGOON_COVER
// A slit in a cell (map 37's parapets): split_z > 0 draws only the cell's Z offsets split_z0
// to split_z (world units, of its 32) at height less `lower` units (2 a pixel); the rest keeps
// the stored height on either side, and the part behind the span faces it with a wall.
// as_drawn: the span's pixels (its top and that wall) keep the art's own shading (no sun
// shadow, occlusion or darkness: kol_focus range_mask, value 2); the art shades them already.
// split_x: the span runs over the cell's X offsets instead (a slit in a wall along X: its back
// wall faces +X, lit in the art, where a Z slit's faces +Z, dark).
struct DrawnCells { int map, record, x0, z0, x1, z1, height; bool as_drawn = false; int lower = 0; int split_z = 0;
                    int split_z0 = 0; bool split_x = false; };
constexpr DrawnCells kDrawnCells[] = {
    // The castle's gargoyle pit (states/EN_slot1_gargoyle_pit): 14 cells
    // around the statue store their rim's 11; the water is drawn at 7,
    // measured on the flat frame off the back-right inner wall, whose top
    // lands on the rim's 11 (README "Drawn heights"). Record 77 is the
    // game's plain blocked cell on ten maps, so not keyed by record.
    {55, 77, 7, 6, 9, 10, 7},
    // Map 21's land bridge (states/thor/0218; the user asked for a hole modelled under
    // the bridge, then for it to arch): its front row stores the deck's 8; drawn at
    // the water's 2, the bridge over it is the model's (tools/models/arch21.py: top,
    // face round the arch, vault) and the water painted under the arch lies flat.
    {21, -1, 5, 2, 8, 2, 2},
    {21, -1, 6, 1, 7, 1, 2},   // one tile deeper, out to x6z1 and x7z1 (the user)
    // The same lagoon on maps 23 and 24 (water at 2) and 22, 25 and 114 (water at 3:
    // some of them have a higher water line, as the user noted).
    {23, -1, 5, 2, 8, 2, 2}, {23, -1, 6, 1, 7, 1, 2}, {24, -1, 5, 2, 8, 2, 2}, {24, -1, 6, 1, 7, 1, 2},
    {22, -1, 5, 2, 8, 2, 3}, {22, -1, 6, 1, 7, 1, 3}, {25, -1, 5, 2, 8, 2, 3}, {25, -1, 6, 1, 7, 1, 3},
    {114, -1, 5, 2, 8, 2, 3}, {114, -1, 6, 1, 7, 1, 3},
    // Map 97, the ship at the quay (states/GF): a line of cells stored at 31
    // across the quay's edge and the ship's deck, a wall to the sky in the
    // art's way. The quay's three at its 3, the deck's at its 1 (the ship
    // itself is a model, see kModels).
    {97, -1, 14, 12, 14, 14, 3},
    {97, -1, 14, 15, 14, 18, 1},
    {97, -1, 15, 17, 15, 18, 1},
    {97, -1, 16, 18, 16, 18, 1},
    // Map 65's room (states/thor/0043): the floor a slab 1 thick on the
    // room's cells (x 1-8, z 0-4) only, the grid's holes in it (the table's
    // and chairs' cells, off the field) filled, the table's three cells (4-6,
    // 2, stored at 2) brought down to it (the user asked for the floor at
    // height 1; the table is a model, tools/models/room65.py); the left wall's
    // front (1,3 and 1,4, at 9) kept. Past it the cells stay at 0, so its
    // front and right edges are a step and their art stands (with the whole
    // map's off-field cells at 1, kMapFloors, it lay flat; the user wanted it
    // 1 thick). And under the back wall, past the grid (z -1: it did not
    // reach under the right wall): the slab's right end runs
    // on under the wall's cut end.
    // Map 67, a dungeon cell (states/thor/0044): the door in the left wall
    // open (its cells (2, 3) and (2, 4) the threshold at 6; the wall over and
    // beside the opening is a model, tools/models/room67.py), and the
    // corridor seen through it, at 6, a cell past the grid (x -1: a view
    // line through the opening's top reaches the floor at X -4).
    {67, -1, 2, 3, 2, 4, 6},
    // Map 90, a boat at a pier (tools/models/boat90.py): the model stands
    // alone on the flat sea, Y 0. The grid's cells are walk routes, not what
    // is drawn: its row z 0 (x 0-8) lies over open water and stood up as a
    // band of sea, and the pier's cells (x 14-15, z 2-18) raised the water
    // beside it. All of the art's footprint, past the grid too: a pixel off
    // every cell has no place, so a rope or yard over it could not lift off
    // and the water stood up in walls behind it.
    {90, -1, -12, -20, 30, 22, 0},
    // Map 73, a flooded cavern (tools/models/room73.py): the ruin on the dais is
    // the model's, so the cells it stood on as blocks (row z 6, x 6-10, stored
    // 11, and 6,8, stored 9) are the dais, 5. Left tall, the water seen through
    // the arch's opening and round its columns landed on them: no opening.
    {73, -1, 6, 6, 10, 6, 5},
    {73, -1, 6, 8, 6, 8, 5},
    // Map 75: a rock's cell stores the rock's top; the floor round it is the art's
    // foot and BG0's flat floor, stored - 3. Left tall, with the rocks now the
    // model's, the floor stood up as a block round each.
    {75, -1, 3, 3, 3, 3, 3}, {75, -1, 3, 6, 3, 6, 2}, {75, -1, 4, 11, 4, 11, 3}, {75, -1, 5, 1, 5, 1, 6},
    {75, -1, 9, 8, 9, 8, 2}, {75, -1, 10, 3, 10, 3, 4}, {75, -1, 1, 5, 1, 5, 4}, {75, -1, 2, 8, 2, 8, 2},
    {75, -1, 6, 2, 6, 2, 2},
    // Map 84: a small crystal's cell stores the crystal's top; the floor BG0 draws
    // under it is 2 lower (record 23) or 1 (record 24): the art's bottom middle lands
    // on that cell's middle. The crystals are the model's (tools/models/room84.py).
    {84, -1, 7, 12, 7, 12, 4}, {84, -1, 8, 9, 8, 9, 4}, {84, -1, 9, 1, 9, 1, 5}, {84, -1, 11, 5, 11, 5, 4},
    {84, -1, 11, 9, 11, 9, 3}, {84, -1, 12, 13, 12, 13, 3}, {84, -1, 13, 3, 13, 3, 3}, {84, -1, 15, 0, 15, 0, 5}, {84, -1, 15, 11, 15, 11, 2}, {84, -1, 19, 8, 19, 8, 2},
    // Map 76: map 101's drawn heights (baked from the Thor) but its table's
    // cells (4-5, 4-6), which are floor here: the alcove walls at 17.
    {76, -1, 1, 3, 1, 3, 17}, {76, -1, 1, 5, 1, 7, 17}, {76, -1, 3, 3, 4, 3, 1},
    // Maps 37 and 38 (the castle by day and at sunset; Thor state 0261): the slits in the
    // parapets (byte 0x08 cells; the user's drawing, 2026-11-13, asking for a slit in
    // the parapet). The art's dark band is the slit's back wall seen through its opening: on
    // the top plane it spans Z +12 to +22 of the cell (rows 146-151 at column 176 on (5,10)),
    // across the parapet's thickness; 12 pixels deep by the drawing (the floor 24 under the
    // top: a step and 8 units), hidden behind the near block. Lit as the map is, floor and
    // back wall (the user's pick of as drawn / lit, the wall lit as well as the floor).
    // The cells: every one in four
    // views (Thor 0261, 0262, 0172, 0205) with that band in the art at its own height (dark
    // across at +12 to +18 on its top plane, its own pixels, clear before and after): not by
    // byte (0x08, 0x07, 0x0F hold them). (6,17) by the user's cursor (Thor 0266: one slit
    // had been missed; out of those views).
    {37, -1, 5, 6, 5, 6, 21, false, 8, 22, 12}, {37, -1, 5, 8, 5, 8, 21, false, 8, 22, 12}, {37, -1, 5, 10, 5, 10, 21, false, 8, 22, 12}, {37, -1, 5, 12, 5, 12, 21, false, 8, 22, 12},
    {37, -1, 5, 14, 5, 14, 21, false, 8, 22, 12}, {37, -1, 0, 7, 0, 7, 30, false, 8, 22, 12}, {37, -1, 0, 13, 0, 13, 30, false, 8, 22, 12}, {37, -1, 1, 10, 1, 10, 30, false, 8, 22, 12},
    {37, -1, 1, 16, 1, 16, 30, false, 8, 22, 12}, {37, -1, 2, 3, 2, 3, 30, false, 8, 22, 12}, {37, -1, 6, 17, 6, 17, 15, false, 8, 22, 12},
    {38, -1, 5, 6, 5, 6, 21, false, 8, 22, 12}, {38, -1, 5, 8, 5, 8, 21, false, 8, 22, 12}, {38, -1, 5, 10, 5, 10, 21, false, 8, 22, 12}, {38, -1, 5, 12, 5, 12, 21, false, 8, 22, 12},
    {38, -1, 5, 14, 5, 14, 21, false, 8, 22, 12}, {38, -1, 0, 7, 0, 7, 30, false, 8, 22, 12}, {38, -1, 0, 13, 0, 13, 30, false, 8, 22, 12}, {38, -1, 1, 10, 1, 10, 30, false, 8, 22, 12},
    {38, -1, 1, 16, 1, 16, 30, false, 8, 22, 12}, {38, -1, 2, 3, 2, 3, 30, false, 8, 22, 12}, {38, -1, 6, 17, 6, 17, 15, false, 8, 22, 12},
    // Map 9, the ruins (Thor state 0263, the cursor on (1,7); the user asked for the
    // slits on this map too): the same tile art, the same band (the detector's nine in
    // that view: the upper wall x 1, the lower x 5); the castle's slit, a brick shallower
    // (the user found them one brick too deep here: 8 px, a step, not the castle's 12).
    {9, -1, 1, 1, 1, 1, 18, false, 0, 22, 12}, {9, -1, 1, 3, 1, 3, 18, false, 0, 22, 12}, {9, -1, 1, 4, 1, 4, 18, false, 0, 22, 12}, {9, -1, 1, 6, 1, 6, 18, false, 0, 22, 12}, {9, -1, 1, 7, 1, 7, 18, false, 0, 22, 12},
    {9, -1, 1, 9, 1, 9, 18, false, 0, 22, 12}, {9, -1, 5, 3, 5, 3, 12, false, 0, 22, 12}, {9, -1, 5, 5, 5, 5, 12, false, 0, 22, 12}, {9, -1, 5, 7, 5, 7, 12, false, 0, 22, 12},
    // ... and its wall along X (Thor state 0264, the cursor on (3,8): a slit oriented
    // on the other axis): (3,8) and (4,8), the same opening over X +12 to +22 (the art's
    // bright band there, the back wall facing the light, its outline at +22).
    {9, -1, 3, 8, 3, 8, 12, false, 0, 22, 12, true}, {9, -1, 4, 8, 4, 8, 12, false, 0, 22, 12, true},
    // Map 36, the grey castle (Thor state 0267: several slits on the Z axis): its walls along X,
    // the X slit's signature in the art (the top plane's profile along X: bright at +12, the
    // near block's outline dark at +22) on six cells in that view, and (13,1), at the view's edge. The castle's depth (12 px),
    // by its family, not measured.
    // Map 56's torch stands (record 79, props): each on its own landing, not on the lower
    // step in front of it (a prop's ground is its front neighbours', which stood them in a
    // dark notch cut into the landing): the floor behind or beside them.
    {56, -1, 7, 1, 7, 1, 17}, {56, -1, 9, 1, 9, 1, 17}, {56, -1, 2, 9, 2, 9, 13}, {56, -1, 2, 13, 2, 13, 10},
    {56, -1, 2, 17, 2, 17, 7},
    {36, -1, 5, 0, 5, 0, 27, false, 8, 22, 12, true}, {36, -1, 7, 0, 7, 0, 27, false, 8, 22, 12, true}, {36, -1, 4, 4, 4, 4, 23, false, 8, 22, 12, true},
    {36, -1, 6, 4, 6, 4, 23, false, 8, 22, 12, true}, {36, -1, 11, 1, 11, 1, 27, false, 8, 22, 12, true}, {36, -1, 9, 5, 9, 5, 21, false, 8, 22, 12, true}, {36, -1, 13, 1, 13, 1, 27, false, 8, 22, 12, true},
    {36, -1, 1, 1, 1, 1, 28, false, 8, 22, 12, true}, {36, -1, 1, 5, 1, 5, 21, false, 8, 22, 12, true},   // (Thor 0268: two slits that had been missed)
    {36, -1, 3, 7, 3, 7, 11, false, 8, 22, 12}, {36, -1, 3, 8, 3, 8, 11, false, 8, 22, 12},   // (Thor 0269: two Z slits, the dark band, on the low wall x 3)
    // Map 81: map 76's room mirrored (X and Z swapped): its alcove walls at 17.
    {81, -1, 3, 1, 3, 1, 17}, {81, -1, 5, 1, 7, 1, 17},
    {82, -1, 3, 1, 3, 1, 17}, {82, -1, 5, 1, 7, 1, 17},   // (82: map 81's room again)
    // Map 79: the left back wall's column (x 0, z 1-11, stored 20) drawn at the
    // walkway's 5: the wall is the model's slope (room79.py back_wall), and the
    // grid's vertical face at X 32 under it made the map need kTerrainNearer 48,
    // which let the slope take terrain up to 48 in front of it: the cliff cell
    // x 3, z 11's top (the user, 2026-11-09: still wrong at x3 z11).
    {79, -1, 0, 1, 0, 11, 5},
    // ... and the wall left of the door, (2, 5), at the walls' 18 (stored at
    // 15, it broke with the camera moving: the wall left of the door
    // broke; the user's heights left it out after their re-edit).
    {67, -1, 2, 5, 2, 5, 18},
    {67, -1, -1, 3, -1, 4, 6},
    // Map 68, the same cell flooded (states/thor/0045; the user noted state 45
    // has the same shape and asked for the room shaped the same, with the
    // door opening, chain, etc.): map 67's drawn heights, the user's from
    // the Thor (the back wall, row 1, at 18; the step's row 2 at 6, 5, 5)
    // and the ones above; its water floor at 4 is its own. Not the box the
    // user put on 67 at 7,4 (asked to leave it off the new map).
    {68, -1, 2, 1, 7, 1, 18},
    {68, -1, 2, 2, 2, 2, 6},
    {68, -1, 3, 2, 4, 2, 5},
    {68, -1, 2, 3, 2, 4, 6},
    {68, -1, 2, 5, 2, 5, 18},
    {68, -1, -1, 3, -1, 4, 6},
    {65, -1, 1, -1, 8, -1, 1},
    {65, -1, 1, 0, 8, 2, 1},
    {65, -1, 2, 3, 8, 4, 1},
    // Map 66: the same room, the same floor.
    {66, -1, 1, -1, 8, -1, 1},
    {66, -1, 1, 0, 8, 2, 1},
    {66, -1, 2, 3, 8, 4, 1},
};
// Rounded boxes: cells the grid has as a box whose vertical corners the art
// draws rounded (radius in world units). The pass lays the cells as the
// ground at their foot and draws, with each cell, the part of the rounded
// box over it: its top at the cells' stored height and its front, each
// column at the depth of the shape's front there, so the wall behind shows
// where a rounded corner does not reach. Shadows, occlusion and torchlight
// keep the box. Corners: kRoundLeft (X0, Z1), kRoundFront (X1, Z1),
// kRoundRight (X1, Z0), the ones the camera sees, and kRoundBack (X0, Z0),
// seen only in the top's outline. The editor's terrain types
// (kol_tile_edit, "type" lines) add one cell each, radius 32, one corner. The art outlines the
// shape 1 px past an arc's silhouette (that column goes with it); `cornice`:
// the top's band, in world units, whose outline sits one column further out
// (the towers' cornice: 40, from the flat frame's outline per row on EO).
// The castle (map 55, measured on the flat frame, README "Round towers"):
// four half-round towers on its walls (two corners of radius 32 on a box
// one cell deep: a half-cylinder on the wall's line; the fronts of their
// tops within a pixel of a circle, the box 1.38), states/EO, EP and EQ;
// the big door frame's two ends (EP, EQ), radius 32 from the width of the
// front one's rounded band and the right one's silhouette, 9 px inside the
// box's corner. The wall's end beside the door frame (x 1, z 11) is square:
// its face shades flat across all 16 columns (ET; rounded in EQ by mistake).
constexpr int kRoundLeft = 1, kRoundFront = 2, kRoundRight = 4, kRoundBack = 8;
struct DrawnRound { int map, x0, z0, x1, z1, r, corners, cornice = 0; };
constexpr DrawnRound kDrawnRounds[] = {
    {55, 13, 2, 14, 2, 32, kRoundLeft | kRoundFront, 40},    // back wall
    {55, 18, 2, 19, 2, 32, kRoundLeft | kRoundFront, 40},    // back wall
    {55, 6, 1, 7, 1, 32, kRoundLeft | kRoundFront, 40},      // back wall, the corner's
    {55, 2, 2, 2, 3, 32, kRoundFront | kRoundRight, 40},     // left wall
    {55, 2, 13, 2, 14, 32, kRoundFront | kRoundRight, 40},   // left wall
    {55, 1, 6, 2, 10, 32, kRoundFront | kRoundRight},    // the door frame, both ends
    {55, 6, 6, 6, 10, 32, kRoundFront | kRoundRight},    // the walkway by the pool, both ends
};
// A map's own sun, where the Shadows pick (when it is on) would put another:
// its height over the ground in degrees and the direction toward it along
// the ground, (X, Z). Set on the Thor with Debug > L2 Moves Sun and read
// from its log line ("sun: ... KOL_PERSP_SHADOW_ELEV=... _DIR=..."). The
// debug arc still moves it from there.
//   89 (a story scene's town, the fishmonger's kiosk): the user's, 2026-10-05.
struct MapSun { int map; float degrees, x, z; };
constexpr MapSun kMapSuns[] = {
    //   75 (the cave): down steeply from the screen's upper left, through its two
    //   openings (kMapWindows); map_settings.cfg `fixed_sun`, it must not sink with the turns.
    {75, 75.6f, -0.707f, 0.707f},
    {89, 18.5f, 2.0f, -1.0f},
    //   101 (the castle room of the intro, HS): from outside its window wall
    //   (X = 64, facing +X), so the light comes in through its two windows
    //   (asked 2026-10-17 for it dark, with godrays coming into the room from
    //   the two windows).
    {101, 40.0f, -0.9f, -0.35f},
    // 76: 101's room without its table, at night (the user, 2026-11-10: pick A of
    // shots/map76_night.png, with the moonlight moved to where it would hit the girl
    // in purple): the moon through the near window (X 32, Z 95-125, Y 92-180, its
    // middle 32, 136, 110) onto her feet at (304, 16, 144): along (272, -120, 34),
    // 23.6 degrees up. (First 101's Thor sun, 27.1 from -0.961, -0.275.)
    {76, 23.6f, -0.992f, -0.124f},
    // 81: map 76's room mirrored (tools/models/room81.py), at night (the user, 2026-11-10:
    // a dim moonlight from the windows): 101's Thor sun mirrored, X and Z
    // swapped (27.1 from -0.961, -0.275), in through the windows in the Z 32 wall.
    {81, 27.1f, -0.275f, -0.961f},
    // 82: map 81's room again (tools/models/room82.py; the user, 2026-11-13: model
    // and lighting based on map 81): 81's moon.
    {82, 27.1f, -0.275f, -0.961f},
    //   55 (the castle, FL): from behind its long wall, so the light comes
    //   through the arched windows (godrays): the user's, 2026-10-06 (set
    //   with L2 from the Low Sun direction (-0.316, -0.949), then turned
    //   two-thirds of the way toward Side Sun: D in
    //   shots/godrays/sheet_sun_toward_side.png).
    {55, 49.5f, 0.589f, -0.808f},
    //   62 (the attic room, states/thor/0016): straight in through its back
    //   wall's windows, 24 degrees down (the user's pick, 2026-10-29: first
    //   from their drawing of the light, 27 degrees toward the rug, then
    //   turned to reach the gable and the shelf too).
    {62, 24.0f, 0.0f, -1.0f},
    //   65 (a room, states/thor/0043): the moon, high (40 degrees) behind
    //   its back wall, in through the windows (the user's pick F of
    //   shots/room65/moonlight_mockups.png).
    {65, 40.0f, 0.0f, -1.0f},
    //   66: the same room, the same moon.
    {66, 40.0f, 0.0f, -1.0f},
    //   77 (the cottage kitchen): from behind its left wall (X, facing +X),
    //   in through the boarded window and angled toward the back, onto the
    //   oven's face and the wall beside the window (the user's pick H of
    //   rooms/r77/light/m77_inward2.png, 2026-11-03).
    {77, 30.0f, -1.0f, 0.6f},
    {59, 30.0f, -1.0f, 0.2f},   // the bedroom: over the bed and across the rug (the user's pick A, shots/map59/light/)
    {61, 30.0f, -1.0f, 0.2f},   // 61: the same room later (states/thor/0217)
    //   78 (the wrecked bedroom): from behind its left wall, in through the
    //   boarded window's glass, 40 degrees, a little toward the back (the
    //   user's pick E of rooms/r78/light/m78_window.png, 2026-11-03).
    {78, 40.0f, -1.0f, 0.3f},
    //   67 (the dungeon cell, states/thor/0053): a torch out of view at the
    //   corridor's far end, its light straight along -X, 30 degrees down,
    //   into the cell through the barred door (the user's pick B of
    //   shots/room67/light_mockups.png; it sways: kWindowTorches).
    {67, 30.0f, -1.0f, 0.0f},
    //   68 (map 67's cell flooded, states/thor/0045): the moon, its only
    //   light (the user: the window by the chain is the only light source,
    //   with moonlight coming in there), from map 67's
    //   moonlight from (0.21, -1), 65 degrees high (46 first, map 67's; the
    //   user asked to move the beam as if the moon had moved, to 65 degrees).
    {68, 65.0f, 0.21f, -1.0f},
    //   39 (the throne hall, states/thor/0059; the user asked to base it on
    //   map 101, with godrays from the windows and 3 invisible
    //   chandeliers): from behind its window wall, 40 degrees, as 101's;
    //   then morning, lower (28, pick A of shots/throne39/morning_mockups.png)
    //   and straight along Z (the user asked for the light along the Z axis).
    {39, 28.0f, 0.0f, -1.0f},
    //   71 (the boat-builder's shed, states/thor/0050; the user asked for
    //   lighting from the window, dynamic lighting for
    //   now): grey daylight from behind its right wall, through its
    //   one window, 35 degrees, a little from the right.
    {71, 22.0f, 0.3f, -1.0f},   // (35 first; 22 the user's pick C)
    {70, 22.0f, 0.3f, -1.0f},   // the same shed by day, its window shut: the crack's light, as 71's
};
// An indoor map's own light: its darkness where no lamp reaches (the
// battlefield's brightness, ScenePixels::dark), how strongly a lamp's light
// brings it back (the glow's gain), and how far its fire pots' light
// reaches (x a torch's). 51: the user's pick B from
// shots/blue_pots/sheet2.png (2026-10-14, asked for it lit by the blue fire
// pots, then darker with more reach). KOL_INDOOR_DARK, KOL_INDOOR_LAMP,
// KOL_POT_REACH override them (mockups).
struct DarkMap { int map; float dark, lamp_gain, pot_reach; };
// A room the game shows in the evening (its time of day, the byte 0x020021BC,
// 1-4), its day colours forced (kol_actions' day-palette hook): darker, and its
// window's light from another way, its own sun (kMapSuns' form). Map 77 after
// the flashback (states/thor/0157; the user asked for the room darker, the
// light from the window turned a little): darkness 0.5 -> 0.3,
// the sun 30 -> 20 degrees, turned 20 degrees from (-1, 0.6) toward the back.
// KOL_EVENING_ROOM=<dark,degrees,x,z>: every one (mockups); KOL_NO_EVENING_ROOM=1: off.
struct EveningRoom { int map; float dark, degrees, x, z; };
constexpr EveningRoom kEveningRooms[] = {{77, 0.3f, 20.0f, -0.63f, 0.78f}};
// 55: asked 2026-10-15 for no light, just shade as on the cave map, the light
// radiating from the godrays and the statue's blue flame lighting as the
// cave's pots do (its flame is a pot on a dark map, see the lamps).
constexpr DarkMap kDarkMaps[] = {
    {51, 0.3f, 3.0f, 1.6f},
    {56, 0.25f, 3.0f, 1.6f},    // the castle hall (states/thor/0194, the user asked for it darker, lit by chandelier and torches)
    {55, 0.35f, 3.0f, 1.6f},   // a tiny bit brighter than the cave (asked 2026-10-15)
    {99, 0.2f, 3.0f, 1.0f},    // the burning town at night, lit by its flames (fire_lights; the user's pick, shots/night)
    {81, 0.12f, 3.0f, 1.6f},   // 76's room mirrored, at night as 76
    {82, 0.7f, 3.0f, 1.0f},    // 81's room again, by day (the user, 2026-11-13: daytime, brighter sunlight from the window, a hopeful mood; pick A)
    {76, 0.12f, 3.0f, 1.6f},   // 101's room without its table, at night (pick A, 2026-11-10; was 101's 0.2)
    {101, 0.2f, 3.0f, 1.6f},   // the intro's castle room, lit through its windows and its chandelier (kCeilingLamps)
    {52, 0.07f, 1.5f, 1.0f},   // the crypt (battle), lit only by the lanterns every unit carries (kMapLamps; the user's pick A at 0.15, then darker still at 0.07, 2026-11-10)
    {58, 0.2f, 1.5f, 1.0f},
    {118, 0.4f, 1.0f, 1.0f},   // map 89's town at night, by the fortune teller's candle (0.2 first, the boat's; the user asked for 0.4; lamp gain 1.5 -> 1.0 with reach 170, pick B)
    {62, 0.6f, 3.0f, 1.6f},    // the attic room, lit through its windows (the user's pick, 2026-10-29)
    {59, 0.6f, 3.0f, 1.0f},    // the bedroom, lit through its window
    {61, 0.6f, 3.0f, 1.0f},   // 61: the same room later (states/thor/0217)
    {60, 0.15f, 3.0f, 1.0f},   // the same bedroom at night, by its lantern (0.3 first; the user asked for more contrast)
    {63, 0.0f, 1.5f, 1.0f},
    {64, 0.0f, 1.5f, 1.0f},    // the same camp in the snow, its tents set out otherwise: lit as 63 (the user, 2026-11-09: the same 3D mesh and lighting where it fits)    // the night camp: no light but the fire and a faint moon (kCeilingLamps; 2026-10-29)
    {65, 0.15f, 3.0f, 1.0f},   // a room at night, the moon through its windows and its left torch (the user's pick F, plus the left torch lit)
    {66, 0.15f, 3.0f, 1.0f},   // the same room, its door shut, the moon through its windows and all three torches (the user asked for all torches lit)
    {67, 0.35f, 3.0f, 1.0f},   // the dungeon cell, lit through its barred door (the user's pick B)
    {68, 0.2f, 3.0f, 1.0f},    // the same cell flooded, by moonlight
    {39, 0.35f, 3.0f, 1.6f},   // the throne hall: its windows and three chandeliers (the user's pick D of shots/throne39/light_mockups.png)
    {71, 0.35f, 3.0f, 1.0f},   // the boat-builder's shed, lit through its window
    {75, 0.12f, 3.0f, 1.0f},   // the cave with the mermaid: two shafts from above and a soft light from the front right (0.3 first; the user asked for the unlit parts darker)
    {79, 0.1f, 3.0f, 1.0f},    // the grotto: dark, lit from the front right (the user, 2026-11-09: asked for dynamic shadows, darker, light from the front right, then a little less light: 0.2 -> 0.1, the lamp's power kept for the units' shadows)
    {70, 0.6f, 3.0f, 1.0f},    // the same shed by day, its window shut (the user's pick F)
    {83, 0.2f, 3.0f, 1.0f},    // the stairwell room, lit by its two green torches (kCeilingLamps; 2026-11-03)
    {77, 0.5f, 3.0f, 1.0f},    // the cottage kitchen: soft, not too dark, its window's light (the user's pick A, 2026-11-03)
    {91, 0.15f, 3.0f, 1.0f},   // the throne hall: lit by two chandeliers over the pool (the user's pick 2, 2026-11-03), then almost fully dark: 0.15 (pick 1 of rooms/r91/dark/m91_dark_sheet.png)
    {78, 0.4f, 3.0f, 1.0f},    // the wrecked bedroom: as 77's (the user asked for similar lighting), then a little more contrast: 0.4 (pick C1, 2026-11-03)
};
// A dark map's pool of faint light over where its people are (ScenePixels
// pool_*): within an ellipse on the floor the darkness is at least `level`,
// full over the inner `inner` of the radius, easing to none at the edge.
//   63, the night camp: the user asked for the scene lightened a little, but
//       only in an ellipse over the characters and tents, enough to see the
//       left side of the left tent (2026-10-29).
// KOL_DARK_POOL=<x,z,rx,rz,level,inner> (mockups; on the map now).
struct DarkPool { int map; float x, z, rx, rz, level, inner; int ground; };
constexpr DarkPool kDarkPools[] = {{63, 76.0f, 70.0f, 230.0f, 230.0f, 0.15f, 0.5f, 1},
                                   {64, 12.0f, 38.0f, 230.0f, 230.0f, 0.15f, 0.5f, 1}};   // (64: 63's, moved with the fire)
// A roofed space on a dark map (ScenePixels cover_*): under it the light
// from above is shut out, so inside a box (world X x0..x1, Z z0..z1, below
// Y y1) the darkness is `level` times the map's, eased in over `soft` units
// from its open side at z1.
//   91, the throne hall's canopy: the user asked for the inside of the
//       tent to be darker (2026-11-03): between its inner left curtain
//       (X 70), its right side's (165), the back wall (Z -65.5) and the
//       front pair (Z 4), under its roof (Y 182); the curtains' outer
//       faces lie on the box's sides and keep the hall's light. 0.5: the
//       user's pick C of 0.7 / 0.5 / 0.35 (rooms/r91/tent/m91_tent_sheet.png).
// Only with Options > Shadows on (dark_cover).
// KOL_DARK_COVER=<level,soft> (mockups: the map's own box).
struct DarkCover { int map; float x0, z0, x1, z1, y1, level, soft; };
constexpr DarkCover kDarkCovers[] = {{91, 69.0f, -67.0f, 164.0f, 3.0f, 182.0f, 0.5f, 24.0f}};
// The torchlight brings the colours (asked 2026-10-26 on map 58 whether the
// torchlight could change the palette gradually): a dark
// map's palette is its night, and where a lamp's light is past
// kLampPaletteFrom of a pixel's way from the darkness to full, the pixel
// turns from its night colour toward its daylight one (kol_perspective
// lamp_palette), all the way at full light. The user's pick: the threshold
// 0.30 (over multipliers 1, 0.85 and 0.75: the night colours reach as far
// into the light as at 0.75, the wood by the torch stays as warm as at 1).
// Map 58 has no daylight palette in the game: its colours are map 97's (the
// same ship by day), matched tile by tile on their pixels (JL against GF,
// 19 of the 55 colours map 58's map uses), then by the same night colour
// (18), then by the nearest matched night colour's change (18; two put by
// hand on the wood's ramp). Pairs in BGR555, the night one first.
// KOL_LAMP_PALETTE=0: none; KOL_LAMP_PALETTE_FROM=<0..0.95> (mockups).
struct ColourPair { int map; std::uint16_t night, day; };
constexpr ColourPair kLampPalette[] = {
    {58, 0x0e11, 0x2656}, {58, 0x12b1, 0x2af6}, {58, 0x1f14, 0x3759}, {58, 0x3d28, 0x218c},
    {58, 0x496a, 0x19d0}, {58, 0x4dea, 0x1614}, {58, 0x562c, 0x2698}, {58, 0x628f, 0x3b1a},
    {58, 0x6333, 0x4b59}, {58, 0x50a9, 0x218c}, {58, 0x5d27, 0x19d0}, {58, 0x5227, 0x1614},
    {58, 0x6a6c, 0x2698}, {58, 0x7eb4, 0x3ef8}, {58, 0x6b76, 0x539c}, {58, 0x40c3, 0x4981},
    {58, 0x5127, 0x5602}, {58, 0x6547, 0x66a7}, {58, 0x7dc8, 0x772c}, {58, 0x7687, 0x7fb4},
    {58, 0x7ef0, 0x77fe}, {58, 0x39a1, 0x4260}, {58, 0x5d47, 0x19f0}, {58, 0x6aab, 0x26d7},
    {58, 0x7f32, 0x77ff}, {58, 0x6374, 0x4b9a},
    // 118 (map 89's town at night): its colours by map 89's, palette entry by palette
    // entry (the two draw the same tiles in the same cells; tools/map_editor exports of
    // both), the most used where one night colour takes two.
    {118, 0x20c5, 0x24e5}, {118, 0x2149, 0x094c}, {118, 0x2549, 0x094c}, {118, 0x296a, 0x0dcf},
    {118, 0x2ce7, 0x3108}, {118, 0x2d08, 0x3108}, {118, 0x2d23, 0x0d89}, {118, 0x2d89, 0x0db2},
    {118, 0x3544, 0x09ae}, {118, 0x3546, 0x0dcf}, {118, 0x3946, 0x514a}, {118, 0x3984, 0x1611},
    {118, 0x39a9, 0x15f5}, {118, 0x3d87, 0x15f5}, {118, 0x3d8a, 0x2678}, {118, 0x4122, 0x458e},
    {118, 0x41e6, 0x16b3}, {118, 0x4564, 0x216e}, {118, 0x4985, 0x51ea}, {118, 0x4e2c, 0x2678},
    {118, 0x4e48, 0x16da}, {118, 0x5162, 0x5def}, {118, 0x520b, 0x3ab9}, {118, 0x5290, 0x3abe},
    {118, 0x59c3, 0x5251}, {118, 0x5aca, 0x377a}, {118, 0x5e0b, 0x3ab9}, {118, 0x61a2, 0x6e16},
    {118, 0x61e3, 0x3236}, {118, 0x628e, 0x4b3e}, {118, 0x6a24, 0x6adb}, {118, 0x6ad0, 0x4b1d},
    {118, 0x6f33, 0x4b5c}, {118, 0x72f5, 0x3f3f}, {118, 0x76ea, 0x4f7d}, {118, 0x7775, 0x5b5f},
};
constexpr float kLampPaletteFrom = 0.30f;
// A map's cut edge faded to the background: where the composed map's art
// stops (map_source_extent) inside a view that is wider or taller than the
// map, the art fades out over `width` hardware pixels toward the colour
// the backdrop shows on that row (kol_perspective applies it, after the
// light). For the story scenes' maps, which are narrower than the view.
// `sky` (kol::kSkyGame: the game's colours) recolours the backdrop itself
// through a ramp by each pixel's brightness, so the game's gradient (and
// its fades) stays a gradient (kol_perspective, recolour_backdrop), then
// pulls it `desat` (0..1) toward grey; the edge then fades to that sky.
//   89 (the town of the fishmonger's kiosk): asked 2026-10-05; the sky a
//       black-to-grey gradient instead of blue, then dusk-coloured but a
//       little desaturated, asked 2026-10-08 (the intro).
// KOL_EDGE_FADE=<px> fades any map's (0: none), KOL_SKY=<0 game, 1 grey,
// 2 dusk, 3 black, 4 meadow> and KOL_SKY_DESAT=<0..1> set the sky, for mockups;
// KOL_SKY_RGB=r,g,b makes a recoloured sky one flat colour (map 93 tried black
// and 40,40,42, 2026-11-13: the user kept the green).
// round: a round fade by the distance from the art's middle (kol_focus
// edge_fade_build): -1 none, 0 a circle of half the art's height, 1 an
// ellipse of its half width and height; round_len its fade's length in
// pixels, round_scale x its radius. The edge's fade still bounds it.
struct MapEdgeFade { int map, width, sky; float desat; int round = -1; float round_len = 0.0f, round_scale = 1.0f; };
// A map's own lamps (with Options > Torchlight on): a flame carried by the
// unit standing nearest `anchor` (within 12 units), at `offset` from it, so
// it shines only while that unit is there; or, with `actor` (its unit
// record's +0x4E), by that actor wherever it goes; or, with kEveryActor, one
// for every unit whose actor is under `actors_below` (the scene's people,
// not its objects: map 58's door 0x70 and flag 0x71 are units too). (An
// actor's id is not a character's: map 58's Rictor is 0x31 at the scene's
// start, 0x05 later, states/JM and JL.) It lights the ground as a torch
// does, within `reach` and at `power` of a torch, and also the units within
// reach (torches do not), and glows on screen over `halo` pixels.
//   89: the fortune teller's candle in the opening (the woman in purple at
//       the table; the light on her and on Alphonse beside it): asked
//       2026-10-05.
//   58: the ship at night (states/JL, JM), the map dark (kDarkMaps):
//       everyone carries a torch, the user's pick B of the mockups for
//       Rictor's (2026-10-25, scratchpad night/night_mockups.png), then
//       every character given a torch; then (2026-10-26) none for the
//       character going into the door: actor 0x32
//       (unit record 0x020017C8, from 208,272 along z 336 into the door
//       at 48,336, states/JM frames ~17180-17480), `no_actor`.
constexpr int kEveryActor = -2;
// The scene objects a map's script makes as units (its actors from
// `actors_from` on): map 58's door 0x70 and flag 0x71; map 3's church door
// 0x70 after the rainy battle (it cast a sun shadow on the ground before
// the church, states/thor/0013). Lit as the scene
// (SpriteBlock::scenery): as a card by the torches, the open door was a
// square of light in the dark wall (the user, 2026-10-26, states/JM ~900).
// `in_wall`: its pieces stay in the scene at its wall's place, hiding what
// they cover of a unit (map 58's door: the one going in went in behind its
// frame); else a card of its own, sorted a cell back (kol_perspective), so a
// unit in its doorway is drawn over it. Map 3's church door is a card: the
// game sorts its sprites and Eleanor's by depth and, tied in the doorway,
// their order flips from frame to frame; taken as in its wall, it hid half
// of her as she stood out in front of it (states/thor/0013).
// `actors_to`: the last actor (one alone: map 67's door, 0x70 after it is
// the chain over Alphonse). `plane_x` (>= 0): its pieces stand in that
// plane (X = plane_x, facing +X) instead of on what is behind them. With a
// plane, every piece in its tile block (64 tiles from its lowest) is its own,
// whoever the claim gave it to, and `decals_from` (> 0): the pieces from that
// tile of the block on (its shadow on the floor) stay in the scene on the
// ground under them, not in the plane.
//   67, a dungeon cell (states/thor/0053; the user asked for the door to be
//   part of the environment, so real light could later be cast through
//   its bars): the barred door (0x6F) in its doorway, its bottom edge
//   measured on the frame (screen (139, 104) and (159, 92.5) on the
//   threshold's Y 96: X 91 and 88), so X 90; its shadow's eight pieces
//   (tiles 0x218-0x22F) on the floor. Its two lower pieces (0x210, 0x214)
//   fit no unit's frame and went to the door or to no one from frame to
//   frame, so as decals they jumped between the plane and the floor (the
//   user reported the door's bottom left shaking before it opened).
// model_actor: the object is a piece of the map's model (triangles flagged
// kModelActor) that the game draws as a sprite: map 73's fourth column, actor
// 0x68 on cell 10,8, the same art as the BG1 column on 6,8 (the user,
// states/thor/0113: the right pillar was a sprite). Its pixels inside the
// column's outline take the model's place, light and lift (model_pass,
// g_model_actor_px); no card. The rest of its sprite repeats the dais's art and
// stays on the dais.
// only_y: it is this object only while it stands at this world Y; elsewhere it is
// a unit like any other. body_tiles_from (> 0): not all of its sprite is the model
// part's: its pieces from this tile on are, but for the 16x16 ones whose top lies
// cover_above_feet rows over its feet, which stay in the scene.
//   59, the bedroom (states/thor/0014; the user asked for him anchored on the bed
//   as if in the sheets): Alphonse, actor 2 at Y 0, under the floor. His sprite is
//   his head and body over two 16x16 pieces, always 20 over his feet, repainting
//   the cover over his body. Those stay on the real cover; the head and body, in
//   every frame of the scene (lying, sitting up, turning his head: tiles 0x200 on,
//   a new set each time), on room59.py's `alphonse_in_bed`. A list of tiles lost
//   his head at the first turn (turning his head crushed it).
// Maps whose chairs hold people the game draws as sprites of their own
// (OamClaim::seated_claim): 101, the intro's castle room.
struct SeatedMap { int map; };
constexpr SeatedMap kSeatedMaps[] = {{101}};
constexpr int kAnyY = -1000;
// units_in_front: in its wall, but what it covers of a unit stays the unit's
//   (map 3's church door: Eleanor ends in the doorway at the door's own place,
//   and the game's sprite order puts the door over half of her). Map 3's door
//   was a card of its own for that (2026-10-28); as a card it floated off the
//   church while the camera panned (Thor state 0305, the user asked for the
//   door to be part of the church, as the door on the boat is).
struct SceneObjects { int map, actors_from; bool in_wall; int actors_to = 0xFF; float plane_x = -1.0f; int decals_from = 0;
                      bool model_actor = false; int only_y = kAnyY; int body_tiles_from = 0; int cover_above_feet = 0;
                      bool units_in_front = false; };
constexpr SceneObjects kSceneObjects[] = {{58, 0x70, true}, {.map = 3, .actors_from = 0x70, .in_wall = true, .units_in_front = true}, {67, 0x6F, true, 0x6F, 90.0f, 0x18},
                                          {73, 0x68, true, 0x68, -1.0f, 0, true},
                                          {59, 0x02, true, 0x02, -1.0f, 0, true, 0, 0x200, 20},
                                          {61, 0x03, true, 0x03, -1.0f, 0, true, 0, 0x200, 20}};
struct MapLamp {
    int map; float anchor_x, anchor_z, dx, dy, dz, reach, power, halo; std::uint8_t hue;
    int actor = -1;
    int actors_below = 0;
    float flicker = 1.0f;   // of a candle's (1); map 58's torches steadier (the user, 2026-10-26)
    int no_actor = -1;      // with kEveryActor: this actor carries none
    float flicker_rate = 1.0f;   // its speed: 1 a candle's
    float feather = 0.0f;        // lamp_fall: 1 a smoothstep rim, 0 linear
};
constexpr MapLamp kMapLamps[] = {
    {89, 368.0f, 368.0f, -2.0f, 20.0f, 7.0f, 72.0f, 1.4f, 10.0f, 0},   // the user's medium pick
    // 118, map 89's town at night (states/thor/0239): the same fortune teller at
    // the same table, the same candle (the user, 2026-11-12, asked for the
    // same light as map 89); reach 100, not 72 (the lit area a little bigger).
    // Then a softer, slower flicker and a bigger range with
    // feathering: flicker half as deep at a quarter the
    // speed. The range: reach 170 with kDarkMaps' lamp gain 1.0 (pick B of a
    // sheet; a smoothstep rim, feather 1 at reach 140, was built and not kept).
    {118, 368.0f, 368.0f, -2.0f, 20.0f, 7.0f, 170.0f, 1.4f, 10.0f, 0, -1, 0, 0.5f, -1, 0.25f, 0.0f},
    {58, 0.0f, 0.0f, -2.0f, 36.0f, 7.0f, 150.0f, 1.6f, 12.0f, 0, kEveryActor, 0x70, 0.4f, 0x32},
    // 52, the crypt (states/thor/0176; the user, 2026-11-10: the map very dark, but every
    // unit carrying a lantern): map 58's torch on every unit, reach 96 (pick A of
    // shots/map52_lanterns.png, with reach 96); the lantern colour, halfway from the
    // torches' orange to daylight (a little more neutral, still a bit warm:
    // shots/map52_lantern_colour.png, 1).
    {52, 0.0f, 0.0f, -2.0f, 36.0f, 7.0f, 96.0f, 1.6f, 12.0f, kol::kHueLantern, kEveryActor, 0x10000, 0.4f},
    // 71, the shed: the man who comes in through the unseen door (actor
    // 0x45, states/thor/0062 after A) carries a torch (the user,
    // 2026-10-30: asked for him to carry a torch).
    {71, 0.0f, 0.0f, -2.0f, 30.0f, 7.0f, 120.0f, 1.1f, 10.0f, 0, 0x45},
};
// Daylight in through a door out of sight (asked 2026-10-15 on GX for
// light coming in from this door, the opening itself unseen, just the
// light entering from a point out of view): a light at (x, y, z)
// lighting a cone `spread` degrees either side of (dir_x, dir_z) on the
// ground, within `reach` at `power` of a torch; it lights the units and
// they cast shadows from it, as from the dark maps' lamps. Warm white.
// The light sits in the doorway, `width` wide across its direction (the
// opening's middle at x, z). KOL_DOOR_LIGHT=<map,x,y,z,dir_x,dir_z,reach,
// power,spread[,width[,shadow[,hue]]]>[;...] adds (shadow: the slope of the
// units' shadows from it, all parallel, 0.3 about 3x a unit's height, < 0
// none; hue: 2 warm sunlight, 3 cool skylight)
// some (mockups).
// A light hanging from a room's ceiling (a chandelier): a point lamp at
// (x, y, z), its light over `reach` at `power` of a torch's, the units'
// shadows radiating from it, a candle's slight flicker; hue as a lamp's
// (0 orange, 1 the blue flames', 2 warm white, 3 cool white). `steady`: no
// flicker. KOL_CHANDELIER=<x,y,z,reach,power,hue> replaces the map's
// (mockups).
struct CeilingLamp { int map; float x, y, z, reach, power; int hue; bool steady = false;
                     float ring = -1.0f;   // its soft shadows' ring (-1: chandelier_ring())
                     bool fire = false;     // a fire's stronger flicker
                     bool flame_shown = false; };  // lit only while a BG1 blue flame is in the frame (Capture::bg1_flame)
constexpr CeilingLamp kCeilingLamps[] = {
    // 75, the cave: the soft light from the front right (the user asked for a more
    // diffused light from the front right): out past the map's near right edge, a wide ring.
    {75, 500.0f, 220.0f, 300.0f, 520.0f, 0.5f, 3, true, 80.0f},
    // 79, the grotto: the light from the front right, out past the map's right
    // edge (X 520, Z 220), steady, a wide ring (soft shadows). First at Y 260,
    // reach 900, power 1.0: it came in 12 degrees over the floor, and the gate's
    // roof shaded its own top in streaks, the farthest pillar's shadow ran far
    // and the terrace in front of a crystal left its foot unlit. Now Y 420,
    // reach 1000, power 0.8 (0.65, for a little less light, faded the units'
    // shadows, which take the lamp's strength on the unit: the darkness went
    // from 0.2 to 0.1 instead).
    // Then reach 1000 -> 650, power 0.8 -> 1.2 (the user asked for the light to be
    // faint enough to hit the back wall less): it fades faster with the
    // distance, the slope behind the gate (about 540 from it) a third darker, the
    // walkway (about 470) a fifth (R2 of shots/map79/reach_options.png).
    {79, 520.0f, 420.0f, 220.0f, 650.0f, 1.2f, 3, true, 80.0f},
    // 101, the intro's castle room (asked 2026-10-17 for it darker, but as if
    // lit by a chandelier hanging in the middle of the roof):
    // over the floor's middle (cells x 2-11, z 2-8), 72 under the walls' top.
    {101, 224.0f, 200.0f, 176.0f, 220.0f, 1.0f, 0},
    // 56, the castle hall: a chandelier over the middle of its floor (cells 8, 10), high
    // under the ceiling (the walls stand to 480); the art draws none.
    // Power 0.55 (1.0 lit the whole hall warm, not darker as asked): pick B of
    // shots/hall56/dark_chandelier_ABCD.png, mine, the user to choose.
    {56, 256.0f, 420.0f, 320.0f, 360.0f, 0.55f, 0},
    // A second, the same, further down the hall (the user, 2026-11-16, asked for a second
    // one at Z 18): tile 18's edge as the first is on tile 10's. Then asked for it lower:
    // the floor there is 8 steps down (height 2 against 10), so it hangs as far over its own
    // floor as the first does (260): Y 292, not the first's 420.
    {56, 256.0f, 292.0f, 576.0f, 360.0f, 0.55f, 0},
    {81, 176.0f, 200.0f, 224.0f, 220.0f, 1.8f, 0},   // (81: 76's chandelier, X and Z swapped: the same chandelier light)
    // 82, by day: no chandelier, the windows' light thrown back, warm, steady, over the
    // room's middle (X 160, Z 208), as 59's bounce (pick A).
    {82, 160.0f, 120.0f, 208.0f, 280.0f, 0.3f, 2, true, 80.0f},
    {76, 224.0f, 200.0f, 176.0f, 220.0f, 1.8f, 0},   // (76: as 101, then at night the chandelier a little brighter: 1.3 barely showed, 1.8)
    // 51, the cave (asked 2026-10-20 on IR/IS, the user's slot 3, for something
    // similar aligned with the cursor, in colder tones, then for no
    // flicker): over cell 15,14 (floor 2), steady, cool white; the
    // user's pick from shots/cave_light/IS_mockups.png: cool, but a
    // little less strong (A was 1.0).
    {51, 496.0f, 160.0f, 464.0f, 220.0f, 0.8f, 3, true},
    // 63, the night camp (states/thor/0039; the user asked for no light but a
    // faint cold moonlight and the fire's light, then for full darkness
    // except the moon and the fire, then for the
    // moonlight faint, from the right; mockups
    // shots/camp63/night_mockups*.png): the fire at the campfire (template
    // (199, 108) on the floor -> 143, 113), 14 up, warm, flickering, a small
    // ring; the moon far off to the screen's right (+X -Z) and high, cool,
    // steady, 0.4, a wide ring (soft shadows).
    // The fire then raised (the user found the fire darker than
    // before): 1.2 / 180 -> 1.5 / 200. Its ring then 8 -> 24 (the user
    // asked to soften the shadows on top of the left tent: the
    // low fire's shadow of the tent's own flared wall across its roof, a
    // hard stepped edge; shots/camp63/fire_ring_8_24_40.png).
    // Then 1.5 -> 1.0 in embers' red (hue 4), with a small darker, redder
    // ring of ground round the pit (kFireGround) so the flame stands out
    // from the ground its light made pale yellow (the user's pick P of
    // shots/camp63/campfire_ring_small.png).
    {63, 143.0f, 30.0f, 113.0f, 200.0f, 1.0f, 4, false, 24.0f, true},
    {64, 79.0f, 30.0f, 81.0f, 200.0f, 1.0f, 4, false, 24.0f, true},   // 64: 63's fire and moon, the fire at its own (80, 80)
    {63, 1000.0f, 1100.0f, -1000.0f, 2600.0f, 0.4f, 3, true, 200.0f},
    {64, 936.0f, 1100.0f, -1032.0f, 2600.0f, 0.4f, 3, true, 200.0f},
    // 65, a room at night (states/thor/0043; the user asked for the left
    // torch lit): the left wall's sconce (tools/models/room65.py, its top at
    // Y 95.5, X 64-79, Z 131-143), its flame 8 over the top, warm,
    // flickering as the camp's fire, a small ring.
    {65, 72.0f, 104.0f, 137.0f, 160.0f, 1.2f, 0, false, 6.0f, true},
    // ... and a faint cool light over the whole room, high in its middle,
    // steady, a wide ring: the moonlight thrown back off the floor (the
    // user asked for a general cold faint light in the room to fake bounce
    // lighting; pick F of shots/room65/bounce_mockups.png, 0.25).
    {65, 176.0f, 70.0f, 80.0f, 280.0f, 0.25f, 3, true, 80.0f},
    // 66, the same room with its three sconces burning (the user asked for all torches
    // lit): each flame 8 over its sconce's top, as 65's left one; 0.9 against that
    // one's 1.2, three of them. And the same faint moonlight off the floor.
    {66, 72.0f, 104.0f, 137.0f, 160.0f, 0.9f, 0, false, 6.0f, true},
    {66, 161.0f, 108.5f, 8.0f, 160.0f, 0.9f, 0, false, 6.0f, true},
    {66, 257.0f, 108.5f, 8.0f, 160.0f, 0.9f, 0, false, 6.0f, true},
    {66, 176.0f, 70.0f, 80.0f, 280.0f, 0.25f, 3, true, 80.0f},
    // 68, the flooded cell: the moon's light bounced, as map 65's (the
    // user suggested a global light to fake light bounce on the
    // map): cool, steady, over the room's middle (X 96-256, Z 64-192); 0.25,
    // the user's pick (of 0, 0.06, 0.1, 0.15 in
    // shots/room67/map68_bounce_mockups.png, and 0.25 before them).
    {68, 176.0f, 120.0f, 128.0f, 280.0f, 0.25f, 3, true, 80.0f},
    // 39, the throne hall: three chandeliers, not seen (the user asked for 3
    // invisible chandeliers), over the carpet's line (row 8) at cells 4, 10
    // and 16, each 120 over the floor under it (9, 5, 3), warm as 101's.
    {39, 144.0f, 264.0f, 272.0f, 220.0f, 1.0f, 0},
    {39, 336.0f, 200.0f, 272.0f, 220.0f, 1.0f, 0},
    {39, 528.0f, 168.0f, 272.0f, 220.0f, 1.0f, 0},
    // 71, the boat-builder's shed: the window's light thrown back (the
    // user asked for fake light bounce), as 65's and 68's: cool, steady, over the
    // shed's middle (X 32-256, Z 32-220), a wide ring.
    {71, 144.0f, 110.0f, 128.0f, 280.0f, 0.25f, 3, true, 80.0f},
    // 70, the same shed by day, its window shut (the user asked for some ambient
    // bounce light): warm, the daylight's.
    {70, 144.0f, 110.0f, 128.0f, 280.0f, 0.25f, 2, true, 80.0f},
    // 83, the stairwell room (the user, 2026-11-03, asked for green light from the
    // torches and the rest dark): its two wall
    // torches (tools/models/room83.py TORCHES/FLAMES), each light at its
    // flame's middle (the flame card's plane Z 7: left column 143 row 105,
    // right 230, 145), 3 out from it, green, flickering as map 65's torch;
    // reach 240 (the user's pick of 160, 240, 320, 400, asking for a wider
    // torch light range; shots in the scratchpad's
    // rooms/r83/light/reach_83_v8.png).
    {83, 5.0f, 100.0f, 10.0f, 240.0f, 1.2f, 5, false, 6.0f, true},
    {83, 179.0f, 107.0f, 10.0f, 240.0f, 1.2f, 5, false, 6.0f, true},
    // ... and a normal, dim light from the top of the stairs (the
    // user, 2026-11-03; pick A of rooms/r83/light/sat_stair_83.png):
    // warm white, steady, out of sight above the flight on the far side of
    // the newel (its axis 45.75, 142.25; the treads there top out near Y
    // 164), so it falls down the treads, a wide ring for soft shadows.
    {83, -10.0f, 280.0f, 122.0f, 320.0f, 0.4f, 2, true, 40.0f},
    // ... and the torches' light thrown on the wall round the breach (the
    // hole the units came in by, left of the door; the user asked for the area
    // around it lit and the hole itself dark
    // and hollow): green, out in the room before it, so the wall takes it
    // face on and the hole, 40 deep, stays dark (the user's pick of 0.6 and
    // 0.8, rooms/r83/light/hole_fill.png).
    {83, 38.0f, 60.0f, 30.0f, 80.0f, 0.6f, 5, false, 4.0f},
    // 77, the cottage kitchen (the user, 2026-11-03, asked for soft light, not too dark,
    // mainly from the window, with a fake bounce): the window's
    // light thrown back, warm, steady, high over the room's middle (X 0-192,
    // Z 0-128), a wide ring (pick A of rooms/r77/light/m77_light_A.png).
    {77, 96.0f, 120.0f, 64.0f, 280.0f, 0.25f, 2, true, 80.0f},
    // 59, the bedroom: the window's light thrown back, as 77's, over the room's middle
    // (X 32-245, Z 32-182); the mock-up's bounce in pick A.
    {59, 138.0f, 120.0f, 107.0f, 280.0f, 0.25f, 2, true, 80.0f},
    {61, 138.0f, 120.0f, 107.0f, 280.0f, 0.25f, 2, true, 80.0f},   // 61: the same room later (states/thor/0217)
    // 60, the same bedroom at night: its lantern on the table, the only light. At the
    // flame's place on screen, moved 10 along the view line in front of the lantern's body:
    // inside it, the body's faces turned away from it (a black barrel) and its shadows
    // cast hard bars on the chair and headboard. Ring 48: the user asked for soft
    // shadow edges.
    {60, 87.5f, 60.0f, 152.5f, 170.0f, 1.0f, 0, false, 48.0f},
    // ... and its light thrown back, faint, from across the room, so what faces away from
    // the lantern (the bed's long side) is not black beside what faces it (the bed
    // showed harsh edges; 0.25 washed the cover's folds out).
    {60, 175.0f, 130.0f, 120.0f, 300.0f, 0.12f, 0, true, 80.0f},
    // 78, the wrecked bedroom: as 77's, over its room's middle (X -40.5 to
    // 157.5, Z -7.5 to 190.5).
    {78, 58.0f, 120.0f, 92.0f, 300.0f, 0.25f, 2, true, 80.0f},
    // 91, the throne hall (the user, 2026-11-03, asked for two imaginary chandeliers
    // in line with the blue carpet): over the pool's middle line (X 112;
    // the pool x 2-4, z 5-12), a third and two thirds along it, 168 over the
    // floor, warm as 39's (pick 2 of rooms/r91/light/m91_light.png); then,
    // the hall at 0.15, lit only by the chandeliers with a wider
    // light: reach 330 (pick 1 of rooms/r91/dark/m91_dark_sheet.png); the
    // throne's side one moved from Z 220 to 120, over the red carpet's foot
    // (moved toward the throne as asked: pick C of rooms/r91/move/m91_move_sheet.png).
    {91, 112.0f, 200.0f, 120.0f, 330.0f, 1.0f, 0, false, 24.0f},
    {91, 112.0f, 200.0f, 350.0f, 330.0f, 1.0f, 0, false, 24.0f},
    // 57, Shaher at the board's far corner (Thor state 0232; the user,
    // 2026-11-11, asked for Shaher's blue flame to be bright and to
    // emit light): the blue flame below his chest (map_settings.cfg
    // glow_flame), a fire's flicker, the blue flames' colour; only while his
    // flame is in the frame (the map's earlier scenes have BG1 on, no Shaher).
    {57, 16.0f, 40.0f, 16.0f, 160.0f, 1.2f, 1, false, -1.0f, true, true},
};
struct DoorLight { int map; float x, y, z, dir_x, dir_z, reach, power, spread, width = 0.0f, shadow = 1.0f, hue = 2.0f; };
constexpr DoorLight kDoorLights[] = {
    // 55, the castle: a door out of sight in the east wall (X 672, the edge
    // past the platform GX's group stands on), 3 tiles wide (Z 224-320), the
    // daylight going in (-X); the user's picks: D1's tight fan (40), 0.8 and
    // the shadows about 3x long with reach 360 (shots/door_light/).
    {55, 672.0f, 110.0f, 272.0f, -1.0f, 0.0f, 360.0f, 0.8f, 40.0f, 96.0f, 0.3f, 2.0f},
    // 55: two imaginary windows in the near wall (z 14, the edge past the
    // map), at x 16 and x 10, 80 over their floors, facing the real ones
    // across the hall: not the sun's side, a faint glow with no shadows
    // (asked 2026-10-15; the user's pick: warm, 0.5).
    {55, 528.0f, 112.0f, 480.0f, 0.0f, -1.0f, 220.0f, 0.5f, 60.0f, 32.0f, -1.0f, 2.0f},
    {55, 336.0f, 176.0f, 480.0f, 0.0f, -1.0f, 220.0f, 0.5f, 60.0f, 32.0f, -1.0f, 2.0f},
    // 39, the throne hall: the same, across from its two windows (the user
    // asked, as on map 55, for faint light on the side opposite
    // the windows): in the near edge (Z 448, past row 13) at the windows'
    // X (368, 516), 80 over the floor under them (5, 3), warm, 0.5.
    {39, 368.0f, 160.0f, 448.0f, 0.0f, -1.0f, 260.0f, 0.5f, 60.0f, 32.0f, -1.0f, 2.0f},
    {39, 516.0f, 128.0f, 448.0f, 0.0f, -1.0f, 260.0f, 0.5f, 60.0f, 32.0f, -1.0f, 2.0f},
    // 71, the shed: the door out of sight the man comes in by (past the
    // room's open front, X 272, his row Z 176; states/thor/0062 after A),
    // lit only while it is open (door_open_71: the game's sounds 203 open,
    // 200 shut); not godrays, just a little light and no
    // shadows (the user, 2026-10-30). Warm torchlight from the room
    // beyond (hue 0).
    {71, 272.0f, 40.0f, 176.0f, -1.0f, 0.0f, 200.0f, 0.45f, 55.0f, 32.0f, -1.0f, 0.0f},
};
// Maps whose ground past the field is a floor at `height` (see the drawn
// heights' pass: map 63).
struct MapFloor { int map, height; };
constexpr MapFloor kMapFloors[] = {{63, 1}, {64, 1}};
constexpr MapEdgeFade kEdgeFadeMaps[] = {
    {89, 32, kol::kSkyDusk, 0.35f},
    {98, 0, kol::kSkyDusk, 0.0f},   // the archers' town at sunset (kMapSunsets): the dusk sky, no fade
    // The night camp (states/thor/0039, the user asked for the fade round a
    // map where nothing is drawn, made black on
    // this one): black around it, the woods fading into it over 32.
    {63, 32, kol::kSkyBlack, 0.0f},
    // The snowy camp: as 63, and round (the user, 2026-11-10: the fade made
    // circular and smoother, at the cost of a longer fade):
    // an ellipse to the art, 120 px long, 1.05 x (pick C of
    // shots/map64/round_fade.png).
    {64, 32, kol::kSkyBlack, 0.0f, 1, 120.0f, 1.05f},
    // The ending's hillside path (states/ending_map93; the user, 2026-11-13,
    // asked for a border fade as in some other cutscenes, in a colour close to
    // the greenish background): the blue sky made the pale sage
    // of the art's far hills, the cut faded into it over 32.
    {93, 32, kol::kSkyMeadow, 0.0f},
};
// Maps at sunset (the user, 2026-10-17, asked for the archers' scene to feel
// like sunset, colours and all; mockup 3 of
// shots/sunset/HK_sunset_mockups.png): the dusk sky (kEdgeFadeMaps), the
// evening light at least `evening` whatever the turn, and the light graded
// golden where the sun falls and cool in the shade at `grade`
// (ScenePixels::sunset_grade). The sun itself is the map's (the user's
// `sun 98 13.0` in the editor). With Shadows on, as the evening.
struct MapSunset { int map; float evening, grade; };
constexpr MapSunset kMapSunsets[] = {{98, 1.3f, 1.0f}};
// Sunsets played over a scene (asked 2026-10-23 on map 102's narration, JJ =
// the user's slot 2, for the sun to set fairly quickly to show time passing
// a little before the storm, then for the storm to start near the end of
// the sunset, for a better transition). From the
// map's first frame the sun goes down from the map's own height to
// `to_degrees` in `frames` (first 1830, the scene's start to its heavy
// rain, intensity 2 at 30.5 s; light from 6 s, medium from 23.5 s, the
// thunder at 44 s), and once the heavy rain has come it stays set. The
// evening light, the sunset's grading and the dusk sky (map 98's, kMapSunsets
// and kEdgeFadeMaps) come in with the square of the way along (light reddens
// fastest near the horizon), greyed toward 0.3 of themselves as the rain
// thickens (ScenePixels::rain): the sunset ends in the storm.
// It waits while an actor of `wait_for` (its unit record's +0x4E) is on the
// map (then asked to wait for the little girl to leave before starting the
// sunset; map 102's girl, 0x5C, walks off and is removed at 39095, 14.6 s
// in, so the sun goes down over the 955 frames from there to the heavy rain).
// KOL_SUNSET_FRAMES=<n> overrides the length; KOL_NO_SCENE_SUNSET=1 off.
// And on such a map (then, the same scene: asked for shadow contrast
// unaffected by the weather and a less abrupt change from orange to
// overcast): `shadow_weather` false keeps the shadows'
// strength in rain (kWeatherShadow cut it to 0.35 the moment the game turned
// its sky grey, at 39630 with the medium rain), and the sunset's colours and
// the evening light go over to the weather's greys through one value easing
// over `grey_frames` toward it (scene_storm: the rain's strength, or 1 under
// the grey sky), not at the sky's turn and the rain's steps.
struct SceneSunset {
    int map; float to_degrees; int frames; float evening, grade, sky; int wait_for;
    bool shadow_weather; int grey_frames;
};
constexpr SceneSunset kSceneSunsets[] = {{102, 6.0f, 955, 1.3f, 1.0f, 1.0f, 0x5C, false, 360}};
// Light through a map's windows (godrays; experimental, KOL_GODRAYS=<strength>,
// asked 2026-10-06 on the castle, states/FL_slot2_godrays). A window is an
// opening in a wall facing +Z: the wall's plane Z, the opening's X span and
// its sill and top Y, the top rounded (a half circle as wide as it). Where a
// point's ray toward the sun passes through one, the sun reaches it (a patch
// of light in the wall's shade), and along each pixel's view line the length
// that lies in such a beam is a haze. Measured from the scene's world
// buffers (KOL_FOCUS_DUMP .wx/.wz/.y/.face) on the arched windows' glass.
// A window: in the plane Z = `plane` (a wall facing +Z), x0..x1 its X
// span; or, with `on_x`, in the plane X = `plane` (a wall facing +X), x0..x1
// its Z span. y0..y1 its sill and top.
// `spread` (0: none): the light through it spreads as a cone (asked
// 2026-10-29 on map 62 for a wide cone spreading from the left window),
// see window_aperture; `gain` its strength.
// `panes_x` x `panes_y` (0: none): the window divided into panes by bars
// (kPaneBar wide), its top square, so its light falls in the panes' grid.
// `own_deg` (> 0): the window has a light of its own, not the map's sun:
// from (own_x, own_z) along the ground, own_deg high, moonlight's colour,
// at `own_gain` (map 67's small window: a little moonlight, as asked, while
// the map's light is the corridor's torch). Built as a second set
// (g_win_set 1) into its own buffers (g_moon_patch, g_moon_haze); it lights
// the scene's ground and its haze, not units or lifted pixels.
struct MapWindow { int map; float plane_z, x0, x1, y0, y1; bool on_x = false; float spread = 0.0f, gain = 1.0f;
                   int panes_x = 0, panes_y = 0;
                   float own_deg = 0.0f, own_x = 0.0f, own_z = 0.0f, own_gain = 1.0f;
                   float pool_soft = 0.0f;      // > 0: the light lands as a round pool feathered over this many units (window_aperture)
                   float pool_peak = 0.0f; };   // > 0: and at this share of the light, spread thinner (window_sun_at; not the beam's haze)
constexpr float kPaneBar = 3.0f;
constexpr MapWindow kMapWindows[] = {
    // 75, the cave (states/thor/0147; the user asked for the map dark with two godrays
    // on the mermaid, then for one on her and one nearer the camera from
    // the same angle): openings in the air over the open middle, not in a wall, so the
    // beams are free of the curtain. The sun (kMapSuns) comes down steeply from the
    // screen's upper left: planes on X, Z 45 on from where each lands. Y 240-360 gives a
    // shaft some width, the ray meeting these planes at a shallow angle. The first lands
    // on her (144, 16, 176), the other, narrower, on the step in front of Alphonse.
    // (Asked for the godrays more diffuse where they land: a soft pool, 70 of feather, at half
    // the light; my pick of four, shots/map75/pool_soft_variants.png; the user to judge.)
    {75, 99.0f, 206.0f, 236.0f, 240.0f, 360.0f, true, 0.0f, 1.0f, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f, 70.0f, 0.5f},
    {75, 315.0f, 264.0f, 284.0f, 240.0f, 360.0f, true, 0.0f, 1.0f, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f, 70.0f, 0.5f},
    // 55, the castle: the two arched windows in the long wall (FL).
    {55, 32.0f, 327.0f, 349.0f, 272.0f, 386.0f},
    {55, 64.0f, 519.0f, 541.0f, 269.0f, 381.0f},
    // 101, the intro's castle room (HS): two windows in the back of the
    // window wall's alcoves, X 32 (the arcade's model, tools/models/
    // room101.py): the glass the grid put at X 64 (a focus dump: Z 227-241
    // and 129-155, Y 126-218) moved along the view line to X 32 (Z and Y
    // 32 less), a little wider for their frames.
    {101, 32.0f, 193.0f, 211.0f, 94.0f, 188.0f, true},
    {101, 32.0f, 95.0f, 125.0f, 92.0f, 180.0f, true},
    {76, 32.0f, 193.0f, 211.0f, 94.0f, 188.0f, true},   // (76: as 101)
    {76, 32.0f, 95.0f, 125.0f, 92.0f, 180.0f, true},
    {81, 32.0f, 193.0f, 211.0f, 94.0f, 188.0f, false},  // (81: 76's, mirrored: the Z 32 wall)
    {81, 32.0f, 95.0f, 125.0f, 92.0f, 180.0f, false},
    {82, 32.0f, 193.0f, 211.0f, 94.0f, 188.0f, false},  // (82: 81's)
    {82, 32.0f, 95.0f, 125.0f, 92.0f, 180.0f, false},
    // 62, the attic room (states/thor/0016): the two windows in the back
    // wall's inner face (Z 32, the room's model), read off the art: the
    // left one's shutters open, the right one's slats drawn across.
    // Both as cones (window_cone_spread): the left one's light the room's,
    // starting as wide as the whole window (the user), the right one's
    // faint (the user asked for only a faint light from the right one).
    {62, 32.0f, 51.0f, 90.0f, 33.0f, 70.0f, false, 0.6f, 1.0f},
    {62, 32.0f, 183.0f, 233.0f, 32.0f, 75.0f, false, 0.6f, 0.25f},
    // 65, a room (states/thor/0043): the back wall's two windows (its inner
    // face Z 0, the room's model, which has them cut through), their glass
    // measured on the art (X 112-124 and 208-220, Y 56-100), a little wider
    // for the frames. Straight beams the windows' shape (the user asked for the
    // godrays shaped like the windows), moonlight (kMapWindowTints).
    // Their light in the panes' grid, 2 across and 4 down (the user asked for
    // a 2x4 grid in the light on the floor).
    {65, 0.0f, 108.0f, 128.0f, 56.0f, 100.0f, false, 0.0f, 1.0f, 2, 4},
    // 77, the cottage kitchen: its boarded window in the left wall's inner
    // face (X 1.25; the model, tools/models/room77.py, has it cut through,
    // the glass 6 in), Z 80.25-102.25, Y 45.75-100.75 (the modeller's read
    // of the art); straight beams at 1.5, the boards across them
    // (kWindowBoards).
    {77, 1.25f, 80.25f, 102.25f, 45.75f, 100.75f, true, 0.0f, 1.5f},
    // 59, the bedroom: its window in the left wall's inner face, at the grid's
    // cell edge (X 32: the model's 31.5 lies in the wall's cell, which stopped the
    // light), Z 76.5-126.5, Y 48-112 (room59.py), straight beams at 1.5 (pick A).
    {59, 32.0f, 76.5f, 126.5f, 48.0f, 112.0f, true, 0.0f, 1.5f},
    {61, 32.0f, 76.5f, 126.5f, 48.0f, 112.0f, true, 0.0f, 1.5f},   // 61: the same room later (states/thor/0217)
    // ... and a light of its own through the same opening, toward the head of the bed:
    // pick A's sun misses Alphonse and the girl by a few units, so no shadow of theirs
    // could show (the user's pick 1, the weak one, of shots/map59/light/shaft_*.png).
    {59, 32.0f, 76.5f, 126.5f, 48.0f, 112.0f, true, 0.0f, 1.0f, 0, 0, 30.0f, -1.0f, 0.7f, 0.4f},
    {61, 32.0f, 76.5f, 126.5f, 48.0f, 112.0f, true, 0.0f, 1.0f, 0, 0, 30.0f, -1.0f, 0.7f, 0.4f},   // 61: the same room later (states/thor/0217)
    // 78, the wrecked bedroom: its left window in the left wall's inner face
    // (X -40.5; the model has it cut through, the glass 8 in), Z
    // 114.5-138.5, Y 38-84 (the modeller's read), boarded but for its glass
    // (kWindowBoards); the back wall's window is a closed shutter, no light.
    {78, -40.5f, 114.5f, 138.5f, 38.0f, 84.0f, true, 0.0f, 1.5f},
    {65, 0.0f, 204.0f, 224.0f, 56.0f, 100.0f, false, 0.0f, 1.0f, 2, 4},
    // 66: the same room, the same two windows.
    {66, 0.0f, 108.0f, 128.0f, 56.0f, 100.0f, false, 0.0f, 1.0f, 2, 4},
    {66, 0.0f, 204.0f, 224.0f, 56.0f, 100.0f, false, 0.0f, 1.0f, 2, 4},
    // 67, the dungeon cell (states/thor/0053): the light of a torch out of
    // view, from the corridor's far end (X 2, facing +X, Z 70-170, up to Y
    // 300; the user asked for the light source further back, so the
    // hallway floor is lit too). The walls and the
    // room's model let it into the cell through the doorway only, and the
    // door's bars (g_plane_mask) cut it in stripes.
    // Its top at Y 240: a ray from higher meets the lintel (Y 182 at X 64-96
    // after falling 54 at 30 degrees) or the wall; 300 first, a third more
    // rays for nothing.
    {67, 2.0f, 70.0f, 170.0f, 96.0f, 240.0f, true, 0.0f, 1.0f},
    // ... and the small barred window in its back wall, by the chain (its
    // inner face Z 64, the opening read off the flat scene's world buffers:
    // X 150-186, Y 180-224, four bars): a little moonlight of its own (the
    // user asked for the small window by the chain to cast a little
    // moonlight).
    {67, 64.0f, 150.0f, 186.0f, 180.0f, 224.0f, false, 0.0f, 1.0f, 4, 1, 46.0f, 0.21f, -1.0f, 0.5f},
    // 68, the same cell flooded (states/thor/0045): the same small window,
    // the map's only light (its sun is the moon, kMapSuns), so a plain
    // paned window: its light in the bars' grid, units' shadows from it.
    {68, 64.0f, 150.0f, 186.0f, 180.0f, 224.0f, false, 0.0f, 1.0f, 4, 1},
    // 39, the throne hall (states/thor/0059): its two tall arched windows in
    // the back wall's face (Z 64, cells 10-11 and 15-16), read off the flat
    // scene (its projection on Z 64): X 354-382 and 502-530, Y 154 to the
    // arch's top at 262.
    {39, 64.0f, 354.0f, 382.0f, 154.0f, 262.0f},
    {39, 64.0f, 502.0f, 530.0f, 154.0f, 262.0f},
    // 71, the boat-builder's shed: its one window in the right wall's inner
    // face (Z 32, tools/models/room71.py), its glass read off the flat
    // scene by the shed's analyst agent: X 190-196, Y 65-101, a little
    // wider for the frame; square (one pane: no arch), so units cast
    // shadows from it. A cone, not a straight beam (the user wanted it
    // much wider than a godray):
    // spread 0.9 first (the front lit evenly, a straight back edge: the user
    // asked why only the bottom half of the map was lit), then the
    // user's pick C of shots/shed71/cone_variants.png (spread 0.3, the sun
    // 22 degrees) with less intense light: gain 0.5 (C's 0.8).
    {71, 32.0f, 186.0f, 200.0f, 62.0f, 104.0f, false, 0.3f, 0.5f, 1, 1},
    // 70, the same shed by day, its shutter closed: the one crack the art draws in it
    // (screen col 273, rows 66-88 in the viewer: X 194, Y 57-101), a faint blade of daylight
    // (the user asked for a little light from the window; pick F of shots/map70/variants.png).
    {70, 32.0f, 193.0f, 195.0f, 57.0f, 101.0f, false, 0.12f, 0.22f, 1, 1},
};
// The light's colour through a map's windows (window_light): the sun's
// warm, or 65's moonlight, cool and pale (the user asked for them to look like
// moonlight; shots/room65/moonlight_mockups.png).
// `haze`: the beams' haze in the air (0.05 elsewhere); 65's fainter, under
// its cool fill (the user's pick F of shots/room65/bounce_mockups.png).
struct MapWindowTint { int map; float r, g, b, haze; };
// 39: morning sun, paler than the afternoon's (the user asked for it to feel
// more like morning sun).
constexpr MapWindowTint kMapWindowTints[] = {{65, 0.55f, 0.7f, 1.0f, 0.04f}, {66, 0.55f, 0.7f, 1.0f, 0.04f}, {68, 0.55f, 0.7f, 1.0f, 0.04f},
                                             {39, 1.0f, 0.9f, 0.72f, 0.05f},
                                             {71, 0.8f, 0.88f, 1.0f, 0.05f},    // 71: a storm's grey daylight
                                             {70, 1.0f, 0.93f, 0.76f, 0.05f},   // 70: plain warm daylight
                                             {59, 1.0f, 0.9f, 0.7f, 0.05f}, {61, 1.0f, 0.9f, 0.7f, 0.05f},     // 59: warm afternoon, the bedroom's pick A
                                             {75, 1.0f, 0.95f, 0.82f, 0.1f},    // 75: pale daylight down into the cave, its shafts plain to see
                                             {76, 0.35f, 0.434f, 0.665f, 0.05f},
                                             {81, 0.35f, 0.434f, 0.665f, 0.05f},
                                             {82, 1.0f, 0.9f, 0.72f, 0.12f}};   // 82: a warm morning sun, its shafts plain (pick A)  // 81: 76's moonlight  // 76: moonlight (pick A, 2026-11-10; then fainter: x0.7)
// The colour of a window's light of its own (MapWindow::own_deg), moonlight's
// without an entry.
struct OwnLightTint { int map; float r, g, b; };
constexpr OwnLightTint kOwnLightTints[] = {{59, 1.0f, 0.9f, 0.7f}, {61, 1.0f, 0.9f, 0.7f}};
// How far the windows' light may take near-white art past its own colour, so a
// white sheet keeps its folds and does not clip (0 or no entry: no limit).
struct WindowWhiteCap { int map; float cap; };
// 59: the two lights took 193 of the cover's 296 bright pixels to pure white, its folds
// gone (the user asked for the sheets' white less overblown); 1.08: 59.
constexpr WindowWhiteCap kWindowWhiteCaps[] = {{59, 1.08f}, {61, 1.08f}, {82, 1.08f}};
// A map's windows' light stronger or weaker than the godrays' 1.0 (with Shadows
// on; KOL_GODRAYS=<s> overrides every map's, mockups).
// 82: map 81's room by day (the user, 2026-11-13: daytime, brighter
// sunlight from the window, a happy scene that should feel
// hopeful): 1.6, pick A of the scratchpad's m82/day_ABC.png (warm morning).
struct GodrayStrength { int map; float strength; };
constexpr GodrayStrength kGodrayStrengths[] = {{82, 1.6f}};
// A map whose window light is a torch out of view (map 67: the corridor's;
// the user asked for the light or shadows to sway a little, like a torch
// out of view): its direction wanders by up to `yaw`
// degrees along the ground and `pitch` up and down, and its strength by
// `flicker`, in steps of `step` frames (each step rebuilds the window
// light). KOL_WINDOW_TORCH=<yaw,pitch,flicker,step> for the map in view
// (mockups); KOL_NO_WINDOW_TORCH=1 still.
struct WindowTorch { int map; float yaw, pitch, flicker; int step; };
// 67: 4 / 2 degrees and 0.12 first, too much sway for the user: under half,
// then 0.75 sideways too; then no sway at all, a torch's flicker only (the
// user found it did not look good here and asked for a slight
// flicker like the torches instead): 8%, every frame, the fire pots'
// rhythm. With no sway the light is never rebuilt for it.
constexpr WindowTorch kWindowTorches[] = {{67, 0.0f, 0.0f, 0.08f, 1}};
// The cells whose prop is an ice crystal (map 85's, 41's and 115's, record
// 56; map 86's, 88's, 53's and 54's, 57): see SpriteBlock::crystal.
constexpr int kCrystalRecords[] = {56, 57};

// A window's boards nailed across it (a barricade): bands across its opening
// that the light through it passes as through map 67's door bars (a
// PlaneMask, plane_mask_for): plane X (on_x windows), Z z0..z1, Y y0..y1.
// Drawn where the art shows them through the glass, as seen on the inner
// face, so their shadows follow the art.
//   77, the cottage kitchen (the user, 2026-11-03: the window is partly
//   barricaded outside, and the shadow should show that,
//   softly): two boards, their fronts on rows -0.5 col + 220.25..223.25
//   and 228.25..231.25 (the modeller's read): Y 82.75-88.75 and
//   66.75-72.75 on the inner face (X 1.25), half a unit inside it so the
//   beams' haze takes them too.
struct WindowBoard { int map; float plane, z0, z1, y0, y1; };
// A polished stone floor's cells (map_settings.cfg `polished_floor`): the
// ice's mirror alone over them (kol_perspective ice_effect), rectangles of
// cells x0..x1, z0..z1. 91, the throne hall: the marble, not the pool (x 2-4
// from z 3) or the red carpet (x 3), the raised cell 6,0 or the gargoyle's
// pedestals 0,2 and 7,1 (read on the art cell by cell, 2026-11-03); with the
// user's cells past the game's 7 x 13 (x 7-8, 9,12, row z 13: the user asked
// why X7 and X8 were not reflective).
struct PolishedCells { int map, x0, z0, x1, z1; };
constexpr PolishedCells kPolishedCells[] = {
    {91, 0, 0, 0, 1}, {91, 0, 3, 0, 13}, {91, 1, 0, 1, 13}, {91, 2, 0, 2, 2},
    {91, 4, 0, 4, 2}, {91, 5, 0, 5, 13}, {91, 6, 1, 6, 13}, {91, 7, 0, 7, 0},
    {91, 7, 2, 7, 13}, {91, 8, 0, 8, 13}, {91, 9, 12, 9, 13},
};
// The map model's floor on a polished map (the floor the grid has no cells
// for): its flat pixels at this height are polished too. 91: the strip and
// aisle along the colonnade (x < 0: -1 too, as asked), the floor before the
// back wall and in the alcove, the strip X 288-302, all at Y 32 (the grid's
// height 2); the dais under the canopy (Y 48, 64) and the pedestals' tops
// are not.
// Cells a map model stands on whole, whose drawn height is the art's and
// not the light's: for the lamps (lamp_seen's cell test) they are the floor
// under the model, whose own parts stop the light. 91: the gargoyles'
// pedestals, drawn 12 and 11 for the statues' art; as 32 x 32 pillars to
// Y 192 they shaded the left statue's V-card halves outside the cell
// (the reviewer: x 314-327 and 346-351 at 0-24, simulated 99).
struct ModelStandCell { int map, x, z, floor; };
constexpr ModelStandCell kModelStandCells[] = {{91, 0, 2, 2}, {91, 7, 1, 2}};
// Maps whose art leaves the sky showing over the grid's cells, and whose model
// is all that stands: the sky is no surface. Map 117's cells cover the whole
// map at height 1 with the user's blocks under the boat, all of it sky in the
// art: those sky pixels stood on them, and the warp pulled the rail's corners
// out to them (wedges at the far stern, a pale plane across the sky).
// Map 59's wall cells reach some 8 px past its art on both sides.
constexpr int kSkyOverCellsMaps[] = {117, 59, 60, 61};
// A sea drawn on BG2 (kol_widescreen sea_bg2_frame: map 117's water under the
// boat, a scrolled field): its pixels are a flat plane at world height y, the
// water line (map 90's sea is at 0, and 117's boat is 90's moved up 32), so the
// water moves with the camera as the boat does (the user, 2026-11-11).
// Units the game keeps on the grid while the art stands them on the model's deck:
// at the grid's height `from` they move to the deck's `to` along the ray through
// their feet (X and Z up by to - from: (X + Z) / 4 - Y / 2 stays, (X - Z) / 2 too),
// so their screen place is the game's and in the warp they stand on the deck.
// Map 117: the boat under sail, its grid flat at 16 (one step), the deck at 80
// (90's 48 moved up 32); the two at the mast stood 64 under the deck, cut in half
// by it, a hole of their shape beside them with the camera moved (the user,
// 2026-11-11, asked for the two characters to be anchored to the boat).
// KOL_NO_UNIT_DECK=1: off.
struct UnitDeck { int map, from, to; };
constexpr UnitDeck kUnitDecks[] = {{117, 16, 80}};
struct SeaPlane { int map, y; };
constexpr SeaPlane kSeaPlanes[] = {{117, 32}};
inline int sea_plane_y(int map) {
    for (const SeaPlane& p : kSeaPlanes)
        if (p.map == map) return p.y;
    return -1;
}
// Maps whose model's occlusion is read from their floor's height and the
// projection: no lamps, so g_wx / g_wz hold nothing (the floor looked out from the
// world's corner, 0.7 all over), and walls stand on raised cells (map 59's are 160
// high, so a band of shade crossed the wall's top stones).
// Maps whose model is set INTO the terrain (map 36's doorway in the keep's wall): a model
// pixel is occluded by how far behind the terrain's face it lies along its ray, full at
// `depth` (0.6 of the AO's strength, as a cell's rise is at most). The grid's AO knows
// nothing of a recess, and the door stood lighter than the wall round it (the user
// asked for the AO pushed into the door).
// (x, z): the terrain cell it is set into, opening on its +Z face. The floor before it
// takes that cell's occlusion from `depth` further back, and the model's flat pixels (the
// sill) take the floor's from the model's back: the floor's shade stopped at the wall's
// face, across the doorway (the floor's AO stopped at x5z5; the user asked for it
// pushed inside the doorway).
// on_x: it opens on the cell's +X face (maps 37 and 38's door, cell (2,3)).
// water_y >= 0: the model's flat pixels at that height are water (the pool running in
// under map 9's water gate): the Water effect takes them (water_mask).
struct ModelRecessAo { int map; float depth; int x, z; bool on_x = false; int water_y = -1; };
constexpr ModelRecessAo kModelRecessAo[] = {{36, 16.0f, 5, 4}, {36, 16.0f, 10, 13, true, 96}, {36, 16.0f, 10, 14, true, 96}, {37, 12.0f, 2, 3, true}, {38, 12.0f, 2, 3, true},
                                             {37, 12.0f, 5, 9, true}, {38, 12.0f, 5, 9, true},
                                             {37, 12.0f, 1, 17, true}, {38, 12.0f, 1, 17, true}, {9, 12.0f, 1, 5, true}, {9, 12.0f, 5, 6, true, 32}};
inline float recess_depth_at(int map, int x, int z, bool on_x) {
    for (const ModelRecessAo& r : kModelRecessAo)
        if (r.map == map && r.x == x && r.z == z && r.on_x == on_x) return r.depth;
    return 0.0f;
}
struct ModelAoFloor { int map; float y; };
constexpr ModelAoFloor kModelAoFloors[] = {{59, 16.0f}, {60, 16.0f}, {61, 16.0f}};
// Maps whose lamps are stopped by the model alone, not by the grid's cells too. Map 60's
// bookcase cells reach 5.5 past its front: the lantern's rays to the front's right half
// crossed the left cell's block, a wall that is not there, and the front split down its
// middle at the cells' edge (the user reported a split in the furniture).
constexpr int kLampModelOnlyMaps[] = {60};
// Colours swapped on a map's model parts before the light (ScenePixels
// recolour_*): `parts`, a bit per part index (g_modeled - 1), and the
// colour pairs, RGB888 0xRRGGBB, from then to.
//   64, the snowy camp (the user, 2026-11-09: the right tent's green should
//   be neither green nor so dark): its tents' fire-facing canvas is drawn
//   in darker greens than map 63's, the same drawing (camp64.py: moved
//   whole), so the fire's light left it dark green where 63's turns beige.
//   Paired pixel by pixel on the tents (map 63 moved (1, 26)): its four
//   greens to 63's. On the tents only (parts 0, 1, 3, 4, 5, 6: the three
//   tents' walls and roofs): the same greens are the firelit grass.
// KOL_NO_MODEL_RECOLOUR=1: none.
struct ModelRecolour { int map; std::uint32_t parts; std::uint32_t from, to; };
constexpr ModelRecolour kModelRecolours[] = {
    {64, 0x7Bu, 0xADDEBDu, 0xA5D684u},
    {64, 0x7Bu, 0x529C52u, 0x9CC66Bu},
    {64, 0x7Bu, 0x396342u, 0x63A56Bu},
    {64, 0x7Bu, 0x6BAD73u, 0x9CC66Bu},
    // The game's crackle: every second or so, for 6-9 frames, the firelit
    // canvas takes a dimmer set, then a dimmest one for 1-2 frames (three
    // states over 120 frames of states/thor/0161) (the user noted the game flickers
    // the palette to make the fire crackle, which the palette change did not take
    // into account: those frames missed the table and the tent went
    // green again). Each to 63's colour dimmed by the game's own ratio, per
    // channel, so the crackle stays.
    {64, 0x7Bu, 0xA5CEBDu, 0x9DC784u},   // (the crackle's dim state of 0xADDEBDu)
    {64, 0x7Bu, 0x428C52u, 0x7EB26Bu},   // (the crackle's dim state of 0x529C52u)
    {64, 0x7Bu, 0x315A39u, 0x55965Cu},   // (the crackle's dim state of 0x396342u)
    {64, 0x7Bu, 0x529484u, 0x78A97Bu},   // (the crackle's dim state of 0x6BAD73u)
    {64, 0x7Bu, 0x9CBDBDu, 0x95B684u},   // (its dimmest, the pulse's last 1-2 frames, of 0xADDEBDu)
    {64, 0x7Bu, 0x397B4Au, 0x6C9C61u},   // (its dimmest, the pulse's last 1-2 frames, of 0x529C52u)
    {64, 0x7Bu, 0x315239u, 0x55895Cu},   // (its dimmest, the pulse's last 1-2 frames, of 0x396342u)
    {64, 0x7Bu, 0x428484u, 0x60977Bu},   // (its dimmest, the pulse's last 1-2 frames, of 0x6BAD73u)
};
// Maps with Bump Mapping's bumps alone, from the art's brightness, in the
// lamps' light (ScenePixels::bump_plain): no wet floor's glints, no stone
// shapes, no relief shadows; at 0.6 (KOL_PLAIN_BUMP), and not on the grey
// stone (light, pale pixels). 79, the grotto (the user, 2026-11-09, asked to
// try normal mapping on this map, then of shots/map79/
// normal_map_options.png chose E without the grey, on the green walls
// only, then the green walls and floor): the stone shapes made the
// walkway cobbles, and the glints the moss sparkle.
constexpr int kPlainBumpMaps[] = {79};
// Bump mapping on one piece of a map's art alone (Options > Bump Mapping): a plain bump map
// (the art's brightness, no glints), on the pixels whose world place lies in the box.
// Map 56: the carved panel on the hall's back wall behind the dais (Thor state 0288, the
// user asked for normal mapping on this art alone): the wall's face
// Z 32, X 228-318, Y 300-392, read by drawing height lines over the state's flat frame.
struct BumpPanel { int map; float x0, y0, z0, x1, y1, z1, strength; };
// strength: x the plain bump map's 0.6. 1.2 at first (the middle of
// shots/hall56/panel_bump_off_0.6_1.2_2.0.png); then asked for it more exaggerated still:
// 4.0 (the third of panel_bump_1.2_2.4_4.0_7.0.png; 7.0 crushed the carving to black lines),
// its light's swing let out to 0.3-1.9 (kol_perspective slime_px). Then the user found
// the relief's details lost: 1.8 from unsoftened heights, the swing 0.7-1.9 (the third of
// panel_bump_sharp_off_1.0_1.8_2.7_soft4.png).
constexpr BumpPanel kBumpPanels[] = {{56, 228.0f, 300.0f, 28.0f, 318.0f, 392.0f, 36.0f, 3.0f}};
// Maps whose props (scene_props: the crystals, rocks...) cast the lamps'
// shadows too, as units do (cast_prop_lamp_shadows; the sun's they cast on
// any map). 79, the grotto (the user, 2026-11-09, asked for the props to cast
// shadows too). KOL_NO_PROP_LAMP_SHADOWS=1: off.
constexpr int kLampPropShadowMaps[] = {79};
// Maps whose model's faces of one kind join with no jump in depth (73: rock and
// fall at a concave corner), so that whole rows are one run of the warp's, drawn at
// its first pixel's depth, far nearer than most of it: rock and water painted over
// the arch's piers in the moved views (28,366 pixels with a key over 64 ahead of
// their own). There a model pixel splits the run past 48 in depth (warp_runs,
// KOL_MODEL_RUN_DRIFT; KOL_MODEL_RUN_WHOLE=1: not at all).
constexpr int kModelRunSplitMaps[] = {73, 75, 84};
// ... and past 8 along depth, not 48, on these. Map 84: its crystal clusters stand on
// the floor before a mound of ledges, some of whose faces are under 48 behind them. A
// row of the scene ran from a spike's pixel left in the scene (near) on through the
// ledge shown under the lifted spikes beside it (behind), one run keyed at the spike's
// depth: the ledge was drawn after the spikes' cards and over them, with the camera at
// rest too (the user, 2026-11-10: the holes came back with the camera still).
constexpr int kModelRunFineMaps[] = {84};
// Maps whose model hangs before a wall drawn on BG1 with nothing on BG0 behind it:
// those pixels lift too, the wall painted in behind them from its own column
// (lift_wall_surfaces). Map 84: the left crystal cluster's tops before the slab of
// cliff beside the hidden stairs.
// Maps whose raised blocks on BG1 lift even a step over the floor behind them
// (lift_wall_surfaces' rule against it is map 16's, whose BG0 there is the block
// again): map 36's BG0 holds that floor (Thor state 0273, the user expected to see
// the ground behind the tile, but it was stretched instead).
constexpr int kWallLiftStepMaps[] = {36};
constexpr int kModelWallBehindMaps[] = {84};
// A model's hanging part (kModelOver) whose foot the art painted into the solid's BG0
// tiles: those pixels lift as a rope's do (g_rope_air 2), the solid behind them filled
// from its own pixels beside them, along (dx, dy) on the screen: the deck's planks, so
// their texture runs on through the gap (one colour a row across the mast's 12 pixels
// read as bands). Map 90's planks run with the keel, along Z: (-2, +1). Map 90's mast (part 5; parts in order in
// kol_model_map90.inc's header): its foot on the deck's BG0 stood at the mast's depth
// with nothing behind it, and the deck stretched up to it, a smear beside the mast with
// the camera moved (the user, 2026-11-11, asked for the same treatment as the
// ropes' lift to the mast). KOL_MAST_ON_DECK=1: old.
struct LiftFillPart { int map, part, dx, dy; };
constexpr LiftFillPart kLiftFillParts[] = {{90, 5, -2, 1}};
struct PolishedModelFloor { int map, y; };
constexpr PolishedModelFloor kPolishedModelFloor[] = {{91, 32}};
constexpr WindowBoard kWindowBoards[] = {
    {77, 1.75f, 76.25f, 106.25f, 82.75f, 88.75f},
    {77, 1.75f, 76.25f, 106.25f, 66.75f, 72.75f},
    // 78, the wrecked bedroom: its left window (inner face X -40.5, Z
    // 114.5-138.5, Y 38-84) is boards but for its glass low in the middle
    // (the modeller's read: open from Y 47.5 to about 54 for Z up to 124.5,
    // to about 62 past it); the bottom rail under it.
    {78, -40.0f, 114.0f, 124.5f, 54.0f, 85.0f},
    {78, -40.0f, 124.5f, 139.0f, 62.0f, 85.0f},
    {78, -40.0f, 114.0f, 139.0f, 37.0f, 47.5f},
};

// A flame of our own on a map, with Special Effects on: no sprite of the
// game's, a torch the art draws unlit (map 65's left sconce; the user asked
// for the left torch lit, with a flame rendered when Special Effects are on).
// Its foot at (x, y, z) in the world, `rows` tall on screen, its width and
// heat as the campfire's (SpriteBlock::fx_width, fx_heat).
// A unit sitting on a modelled seat the grid puts higher or lower (map 78's bed:
// the grid's cell at 32, the model's mattress top at 25.4): a unit in cell
// (cx, cz) at the grid's height from_y is slid along the line of sight,
// (X, Y, Z) + t (1, 1, 1), to stand at seat_y, the same place on the flat
// picture, on the seat under the perspective. KOL_NO_UNIT_SEATS=1: off.
struct UnitSeat { int map, cx, cz, from_y, seat_y; };
constexpr UnitSeat kUnitSeats[] = {
    // 78: Euphaire in bed (the user, states/thor/0151: she must lie on the bed, not
    // float above it, then she still hovered a little and should be anchored to the
    // middle of the bed): at (24, 32, 96), the cell's 32, at the bed's front corner. Her card
    // at the bed's middle depth instead: slid 24 back, its foot at (0, 8, 72), so the
    // card's row at the mattress's height (tools/models/room78.py BED_DECK top 25.4,
    // 8.7 rows over her sprite's bottom) is the mattress's middle whatever the camera;
    // her lap under it hangs in front, drawn over the bed as before. (25, her foot on
    // the mattress at its front edge, still hovered with the camera moved.)
    {78, 0, 3, 32, 8},
};
// palette: SpriteBlock::fx_palette. art_half: with Special Effects on, the art's own flame
// in a lifted prop (a brazier's) is not drawn: its BG1 pixels within art_half columns of
// the flame's middle, from its foot up rows + 2 (lift_props); 0 keeps the art.
// light > 0: it throws fire light of that power over light_reach (ScenePixels::fire_lights,
// as the burning town's flames: the pools that lift the shade round it), with Torchlight,
// Special Effects on or off.
struct MapFlame { int map; float x, y, z; int rows; float width, heat; int palette = 1; int art_half = 0;
                  float light = 0.0f, light_reach = 0.0f; };
constexpr MapFlame kMapFlames[] = {
    // 65: on the left wall's sconce (tools/models/room65.py: its top Y 95.5,
    // X 64-79, Z 131-143), its lamp in kCeilingLamps.
    // 20 rows, 0.45 wide, 0.9 hot: the user's pick B (14 and 26 the others,
    // shots/room65/torch_flame_sizes.png).
    {65, 72.0f, 96.0f, 137.0f, 20, 0.45f, 0.9f},
    // 66: on all three sconces. The art's own flames on the back wall are gone
    // (kModelFxArt), BG0's wall behind them; the left wall is BG1 alone, the sky on BG0
    // under it, so the wall there is painted from the pilaster (g_fx_under_fix).
    {66, 72.0f, 96.0f, 137.0f, 26, 0.6f, 0.9f},
    {66, 161.0f, 100.5f, 8.0f, 26, 0.6f, 0.9f},
    {66, 257.0f, 100.5f, 8.0f, 26, 0.6f, 0.9f},
    // 83: the two wall torches (the user asked to try Special Effects flames on the
    // torches when the option is on), each on its art flame's foot on
    // the flame card's plane (Z 7: the left flame's bottom row 114 at column
    // 143, the right's 154 at 230), as tall as the art's (19 rows), green.
    {83, 5.0f, 82.0f, 7.0f, 20, 0.45f, 0.9f, 2},
    {83, 179.0f, 89.0f, 7.0f, 20, 0.45f, 0.9f, 2},
    // 9, the ruins' pool (the user asked for map 9's torches to take Special Effects
    // too): the two braziers by the pool on cells (6, 4) and (6, 8), their middles
    // (208, 144) and (208, 272), the ground 48, the bowls' tops 70 (the art's lowest flame
    // row); the third on the ruins' top, cell (2, 0) (the user pointed out a missed one,
    // states/thor/0156), its ground 208, its bowl's top 230. The art's flames 21 rows,
    // 13 wide, gone under ours (art_half 8: the bowl is under the foot).
    {9, 208.0f, 70.0f, 144.0f, 22, 0.5f, 0.9f, 1, 8},
    {9, 208.0f, 70.0f, 272.0f, 22, 0.5f, 0.9f, 1, 8},
    {9, 80.0f, 230.0f, 16.0f, 22, 0.5f, 0.9f, 1, 8},
    // 37, the castle the death scene comes to (Thor state 0252; the user, 2026-11-13, asked
    // for Special Effects fire on the fire pits): its four braziers, the cells with
    // byte 0x14 (7,4), (1,6), (6,13), (1,20), each 4 steps over its floor; the bowls' tops
    // 26 under the cell's top (read on (7,4) and (6,13) from a focus dump: the art's lowest
    // flame row at its column, Y 198 and 182). As map 9's.
    {37, 240.0f, 198.0f, 144.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    // (1,6), high in a corner the sun never reaches: it throws its light (the user found the
    // top brazier's being in shadow made no sense and asked it to emit light): 1.6 over
    // 110, my pick over the burning town's 1.1 / 72 (barely warmed the corner;
    // the scratchpad's m37/light.png).
    {37, 48.0f, 374.0f, 208.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {37, 208.0f, 182.0f, 432.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {37, 48.0f, 342.0f, 656.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    // 38, the same castle at sunset (its grid map 37's cell for cell, the braziers the same
    // four byte-0x14 cells; the user, 2026-11-13, noted map 38 has 37's torches and asked
    // for the same effect): 37's four, the high one's light too.
    // Then, on both maps, every torch lit like the one that had been in the
    // shadow: every brazier throws its light, 1.6 over 110.
    {38, 240.0f, 198.0f, 144.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {38, 48.0f, 374.0f, 208.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {38, 208.0f, 182.0f, 432.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {38, 48.0f, 342.0f, 656.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    // 36, the snowy castle (the user, 2026-11-13, asked for Special Effects fire on
    // the map's torches): its two tripod braziers, cells (14,11) (byte 0x15, three steps
    // over its floor 96) and (0,10) (byte 0x14, on a plinth: floor 176). The bowl's top 24
    // over the floor (read on (14,11) from Thor state 0273's focus dump: the art's lowest
    // flame row 143-144 at column 288, Y 120); (0,10)'s by the same 24. As 37's, light too.
    // 56, the castle hall's five torch stands (record 79; the user, 2026-11-16, asked for
    // the visible torches to have the Special Effects flame): blue
    // (palette 3). Each flame's foot 12 under its cell's stored top (read on Thor state
    // 0286's dump: the art's lowest flame row 5 rows under the top's, at 7,1 / 9,1 / 2,9 /
    // 2,13 alike); the art's flame 12 rows, 9 wide: 14 rows, 0.4 wide, art_half 6. No light
    // of their own here: the stands are lamps already (gather_lamps). The three imagined
    // ones at X 14 (kPhantomTorches) have no flame.
    {56, 240.0f, 324.0f, 48.0f, 14, 0.4f, 0.9f, 3, 6}, {56, 304.0f, 324.0f, 48.0f, 14, 0.4f, 0.9f, 3, 6},
    {56, 80.0f, 260.0f, 304.0f, 14, 0.4f, 0.9f, 3, 6}, {56, 80.0f, 212.0f, 432.0f, 14, 0.4f, 0.9f, 3, 6},
    {56, 80.0f, 164.0f, 560.0f, 14, 0.4f, 0.9f, 3, 6},
    // 51, the cave's four blue fire pots (record 76: cells 12,2 / 3,4 / 6,4 / 7,12; Thor
    // state 0291, the user asked for Special Effects torches): blue (palette 3), each
    // flame's foot 26 under its cell's stored top as the braziers' (the art's flame 21 rows
    // from 8 over the top's row, read on the state's dump at all four), their size too. No
    // light of their own: the pots are lamps already.
    {51, 400.0f, 182.0f, 80.0f, 22, 0.5f, 0.9f, 3, 8}, {51, 112.0f, 214.0f, 144.0f, 22, 0.5f, 0.9f, 3, 8},
    {51, 208.0f, 214.0f, 144.0f, 22, 0.5f, 0.9f, 3, 8}, {51, 240.0f, 102.0f, 400.0f, 22, 0.5f, 0.9f, 3, 8},
    {36, 464.0f, 120.0f, 368.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
    {36, 16.0f, 200.0f, 336.0f, 22, 0.5f, 0.9f, 1, 8, 1.6f, 110.0f},
};
}  // namespace kol_focus_detail
