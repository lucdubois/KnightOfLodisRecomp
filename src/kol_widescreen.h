// kol_widescreen.h — Knight of Lodis extended-view (16:9) scene policy.
//
// gbarecomp supplies the wide render surface as a mechanical capability
// (ENHANCEMENTS.md §3); deciding what may appear in the added margins is
// game policy, which is this file. Both entry points — src/main.cpp for
// desktop and android/app/jni/main_android.cpp — install it through
// configure_extended_view() so the two builds cannot drift apart.
//
// The faithful 240x160 path is untouched: nothing here runs until the user
// selects a wider view with --view-width / [video].view_width /
// GBARECOMP_VIEW_WIDTH, and the authentic 240 columns are never modified.

#pragma once

#include "runtime.h"

namespace kol {

// Sets max_view_width and the extended_view_frame hook on `opts`.
void configure_extended_view(gbarecomp::RunOptions& opts);

// True while the frame hook last saw the tactical battle map. Uses the same
// DISPCNT + BGxCNT classifier the extended-view scene policy runs on, so a
// consumer cannot drift from it.
bool is_battle_scene();

// True while the frame hook last saw the unit status screen.
bool is_status_scene();

// The same two, but answering "what is on screen RIGHT NOW" — which inside a
// speculative excursion means the shadow timeline's screen, not the player's.
// Consumers on the presentation thread want is_battle_scene(); code running
// inside an excursion wants these.
bool is_status_scene_now();
bool is_battle_scene_now();

// Diagnostic: the policy state the margin compositor depends on, as a string.
const char* margin_state_debug();

// The guest's unit info panel: the solid 10x7 BG3 block, in native screen
// pixels. False when no panel is on screen this frame.
bool unit_panel_rect(int* x, int* y, int* w, int* h);

// How far HUD anchoring moved the UI element an interface sprite lies on:
// a priority-0 sprite (priority 0 is the game's interface; unit parts are 1
// or 2) with more than half of its rectangle on one anchored element's tiles.
// The Attack/Cancel box went down with the bottom bar and left its hand by
// the unit (states/CH_menu_hand). False when it lies on none that moved,
// and for the grid cursor's own sprites (kol::grid_cursor_sprite): its hand
// points at a cell, not at the box it happens to be over.
bool ui_sprite_shift(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
                     int* dx, int* dy);

// A UI layer that is one tile over the whole screen in the last picture (the
// thunder's white flash on map 102's opening, BG3): its number and its tile's
// colour (RGB888), or -1. Not HUD: the perspective blends it over the warped
// scene. For the present's capture: the anchors are already the next frame's.
int ui_field(std::uint8_t rgb[3]);
// A spell's closing circle, fitted this present's frame (kol_circle_window_span): its
// centre and radius in native screen pixels. False: none since the last call (call it
// once a present, from the capture).
bool spell_ring(float out[3]);

}  // namespace kol
