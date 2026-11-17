#pragma once

#include <cstdint>

// The second screen's editor page: while Debug > Tile Info is on, the
// sheet's place shows the battlefield's cells from above (the game's
// diagonal, as a map) around Tile Info's target, coloured by drawn height,
// with the roofs, boxes and edited cells marked, and under it the editor's
// controls (kol_tile_edit) as the game's windows:
//   HEIGHT / ROOF / BOX   what the fields edit;
//   <  FIELD VALUE  >  -  +   the field, and a step down or up;
//   the mode's actions    RESET (the height), DELETE (the roof, the box),
//                         ADD BOX.
// A tap on a cell makes it the target (in a story scene's viewer the tile
// cursor goes there, and the camera with it); in ROOF a drag across cells
// makes a roof over them. See README "The editor on the second screen".

namespace kol {

// Renders the page into argb (kSheetW x kSheetH); false when it is not up
// (Tile Info off, no grid published, or the ROM art not ready).
bool compose_edit_panel(std::uint32_t* argb);

// A touch at canvas pixel (x, y): action 0 down, 1 move, 2 up, 3 cancel.
void edit_panel_touch(int action, float x, float y);

// Desktop checks, every frame: KOL_EDIT_PANEL_PPM=<path>[,<frame>] writes
// the page; KOL_EDIT_PANEL_TOUCH=<frame>:<action>:<x>:<y>,... touches it.
void edit_panel_test_tick(unsigned long frame);

}  // namespace kol
