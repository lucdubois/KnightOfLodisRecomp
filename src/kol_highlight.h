// kol_highlight.h — see-through movement-range highlight.
//
// The game paints its blue movement range INTO the BG0 tilemap, replacing the
// terrain tiles rather than layering over them, so there is nothing behind the
// highlight to show through. README.md "Map mirror at 0x02008440" records the
// hunt that established the original tiles are not retained anywhere in guest
// memory, so they cannot be looked up; they have to be remembered.
//
// This keeps a shadow of the last un-highlighted entry seen for each cell of
// the battlefield and hands it to the renderer's underlay seam. A cell we have never seen
// clean keeps the game's opaque look — see kol_highlight.cpp for exactly when
// that happens.

#pragma once

#include <cstdint>

namespace kol {

// Per-frame shadow maintenance. Cheap and safe to call every frame; does
// nothing unless the battle map is on screen and the feature is enabled.
void highlight_tick();

// Per-frame world-shadow maintenance: records every composed-map cell not
// wearing the highlight. Call after map_source_tick(), which it reads.
void highlight_world_tick();

// Installs the renderer seam. Idempotent.
void install_highlight();

// Diagnostics: how many visible cells currently carry a usable shadow, and how
// many highlighted cells had to fall back to opaque.
void highlight_stats(int* out_shadowed, int* out_fallback);

// Turn the see-through blend on or off while running. The underlay provider
// stays installed either way; only the layer mask it applies to changes, so
// this costs nothing when off.
void highlight_set_enabled(bool on);

// The composed-map cells wearing the highlight now (the range's tiles): per
// cell, which of its 8x8 pixels are the highlight's (bit y * 8 + x, as
// drawn), 128 cells a row, as map_source_world_cell names them; null when
// the world shadow holds nothing for `bg` or no cell wears the highlight.
const std::uint64_t* highlight_world_lit(int bg, int* out_rows);

}  // namespace kol
