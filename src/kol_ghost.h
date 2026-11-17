#pragma once

namespace kol {

// A unit the game blinks (the one on a deployment square in battle
// preparation) is drawn every frame, half transparent, instead: two reviewed
// function hooks, see kol_ghost.cpp. KOL_NO_GHOST=1 keeps the blink.
void ghost_install();

}  // namespace kol
