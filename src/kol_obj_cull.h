#pragma once

#include <cstdint>

namespace kol {

// Installs the immediate-override callback that widens the guest's own sprite
// visibility window. Must run AFTER gbarecomp::run_game() has cleared the
// game-owned hooks, i.e. from the frame hook, never from main().
void obj_cull_install();

// The margins actually being rendered this frame. All zero (the authentic
// 240x160, or a pillarboxed scene whose margins are black) restores the stock
// window, so the faithful render is untouched.
void obj_cull_set_margins(unsigned extra_left, unsigned extra_right,
                          unsigned extra_top, unsigned extra_bottom);

// The true screen X of a sprite the guest recorded in its slot list, matched
// on its (OAM X, OAM Y) pair. False for anything it did not record, which then
// keeps the renderer's default reading. Expanded rendering only.
// `near` also accepts a 16x16 sprite within a few pixels of a recorded one
// (scrolling; see kol_obj_cull.cpp); pass false to require an exact match.
bool obj_true_x(std::uint16_t attr0, std::uint16_t attr1, int* out_x,
                bool near = true);

// The OBJ attribute providers (the Y provider here, kol_widescreen.cpp's X
// provider) answer from an entry's attributes and from state the per-frame
// policy sets, so each answer is kept per OAM entry until the next frame: the
// renderer asks for all 128 entries on every scanline, some 38k times a frame.
// obj_attr_new_frame() starts a frame; call it first thing in the policy.
void obj_attr_new_frame();

// Whether a sprite at this native screen rect is one the grid cursor draws
// (its hand floating over its cell, its corner pieces on it), from the
// cursor's slot in the guest's list; false when no cursor is out. HUD
// anchoring leaves those where the game put them (kol::ui_sprite_shift).
bool grid_cursor_sprite(int x, int y, int w, int h);
std::uint64_t obj_attr_epoch();

// The battle confirmation's arrow between the units' boxes at the top: a
// piece of it (OBJ tiles 0x13C-0x14A, priority 0, in its strip), whether
// one is up now (its palette bank), and whether the top screen leaves it
// and the boxes out (the second screen shows them: kol_sheet
// draw_sheet_forecast).
bool confirm_arrow_piece(std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2);
bool confirm_arrow_up(int* bank);
bool confirm_arrow_hidden_now();
// Once a frame on the guest's thread (the frame hook); then the bank of the
// arrow up now or within 8 frames, -1 none, for any thread.
void confirm_arrow_tick(bool battle);
int confirm_arrow_bank();

// The player's switch for repeating the rain into the margins.
void obj_cull_set_weather_repeat(bool on);

}  // namespace kol
