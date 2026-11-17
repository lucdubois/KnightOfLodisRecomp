#pragma once

#include <functional>

#include "kol_env.h"
#include "kol_workers.h"   // parallel_rows

namespace kol {

// KOL_PERSP_HASH=1 prints a hash of each pool pass's output per frame, to
// find the first pass that differs between two runs or two chunk sizes
// (and kol_focus the capture's, to trace a difference to its inputs).
inline bool persp_hash_trace() {
    static const bool on = env_true("KOL_PERSP_HASH");
    return on;
}

// Perspective on the battle map: the farther a thing is, the smaller it is
// drawn, while everything upright stays upright. Installs (or removes)
// gbarecomp's present-time warp provider; the PPU frame, frame hashes and
// both render invariants are unaffected. Needs the extended view.
void perspective_set_enabled(bool on);
// Whether the perspective is drawing this frame.
bool perspective_enabled();
// The light without the perspective: the occlusion, the shadows and the
// torchlight the perspective draws, on the map as the game drew it, pixel
// for pixel (the same present-time provider, with the scene drawn back onto
// itself). Off whenever the perspective is on.
void perspective_set_flat(bool on);
bool perspective_flat_enabled();

// How far past a view_w x view_h view the scene must be composed (left,
// right, top, bottom) for the shrunken far side to reach the view's edges.
// The near side grows, so the bottom needs none.
void perspective_overscan(int view_w, int view_h, unsigned out[4]);

// Per row of the scene composed with that overscan (n rows, 0 = its top),
// how many of the side columns are needed: only rows drawn smaller than they
// are reach in from beyond the view's sides. Returns false, filling nothing,
// while the reference height is not known yet (then every row needs all).
// The combat camera: while the game plays an attack on the map, a deeper
// perspective and a zoom, eased in and out. Install once (after anything
// else that owns the function-entry hook); tick once a frame, before the
// overscan is chosen.
void perspective_strike_install();
void perspective_strike_tick(bool perspective);
// True while the combat camera is on at all (eased in any amount).
bool perspective_combat_camera();
// The combat camera is on for a spell's effect (kSpellFn), not an attack.
bool perspective_spell_camera();
// A spell is running: its charge (the caster's orb) or its effect, whether
// the camera has eased in or not (kol_focus's spell lights).
bool perspective_spell_running();
// The camera (the combat camera's ease, the look) as the warp will draw it,
// copied for the present path (kol::present_capture).
void perspective_capture();
// ... what it took: the strike's ease, the look (0 x, 1 y).
float g_cam_cap_strike();
float g_cam_cap_look(int i);

bool perspective_overscan_rows(int view_w, int view_h, const unsigned overscan[4],
                               unsigned char* rows, int n);

}  // namespace kol
