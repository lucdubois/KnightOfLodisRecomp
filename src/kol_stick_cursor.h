#pragma once

namespace kol {

// The left stick moves the battle map's hand cursor freely, as fast as the
// stick is pushed, and the hand settles on the nearest cell when the stick is
// let go: three reviewed function hooks, see kol_stick_cursor.cpp. Options >
// Analog Cursor; KOL_NO_STICK_CURSOR=1 leaves them out.
void stick_cursor_install();

}  // namespace kol
