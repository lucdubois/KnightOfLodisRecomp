#pragma once

#include <cstdint>

// The gulls crossing the harbour at the start of map 97's boat scene
// (states/HJ) start off the view: see kol_gulls.cpp.
namespace kol {

void gulls_install();
// The view's right margin this frame, in screen pixels past the native 240.
void gulls_set_view_margin(int extra_right);
// Every guest frame: the street soldier's fade at the end of his walk.
void gulls_frame();
// A scene actor stepping out of a doorway fades in (map 100's scouts):
// 0..1 for actor `actor` at world X `x`, from `ew`, a copy of EWRAM (the
// present's capture); 1 when not fading.
float gulls_unit_fade(const std::uint8_t* ew, std::uint8_t actor, int x);

}  // namespace kol
