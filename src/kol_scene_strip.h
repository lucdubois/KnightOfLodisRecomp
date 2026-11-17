// kol_scene_strip.h — the opening scenes carried on past the game's picture.
//
// On the opening's panning mountain and on the title screen (with its NEW
// GAME / CONTINUE menu), what lies below the game's 160 lines carries the
// scene on as one picture: in the top screen's own extra rows, and on the
// second screen, which shows the next rows down and moves with the scene.
// The opening's fire keeps wobbling there; the title's dark veil over its
// clouds keeps covering them; the title's logo does not repeat. See README
// "The opening scenes on the second screen".
#pragma once

#include <cstdint>

namespace kol {

constexpr int kStripW = 248;   // the second screen at 5x (1240 px)
constexpr int kStripH = 280;   // enough for any whole-number scale down to 4

// Registers the record-only hook on the game's scroll-table fill (0x0800CAEC,
// see game.toml). KOL_NO_SCENE_STRIP=1 leaves it off.
void scene_strip_install();

// Once a frame, from the frame hook: which scene is up, and the title's veil
// row.
void scene_strip_frame();

// 0: neither scene, 1: the opening's mountain, 2: the title screen.
int opening_scene();

// For the widescreen providers, rows outside the game's 0-159:
// the title's BG1 samples one pixel of its field there (true: *hw_x, *src_y);
bool title_veil(int bg, int screen_y, int* hw_x, int* src_y);
// the opening's wobbling layers shift by *dx from their line-159 HOFS.
bool opening_wobble(int bg, int screen_y, int* dx);

// The strip below the top screen for the second screen: kStripW x kStripH
// ARGB, centred on the game's 240 columns (black outside them, as the top
// screen pillarboxes these scenes), starting at the first row under the
// presented picture (160 plus the view's extra rows). False when neither
// scene is up; *scene receives 1 or 2.
bool compose_scene_strip(std::uint32_t* argb, int* scene);

// Desktop check: KOL_STRIP_DUMP=<file.ppm> ("%d" takes the frame number)
// writes the strip whenever it changes. No-op unless set.
void scene_strip_dump_tick(unsigned frame);

}  // namespace kol
