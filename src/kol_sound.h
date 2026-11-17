#pragma once

// Sound effects placed where they happen on the battle map (Options: "Sound
// effects come from where they happen"). Every real frame the game's state
// names a place: the map cursor while it is out, the unit being hit while a
// damage number is up, otherwise the unit whose turn it is. Its screen
// position becomes a pan and a distance gain for gbarecomp's mixer, which
// applies them to the effects only, in the high-quality sound modes (only
// there is every voice known to be music or an effect).
//
// Measured: the game's camera keeps the action near the middle of the
// screen (the enemy turn follows the acting unit at x = 120; in an attack
// the two units stand 16 pixels apart), so in fights the placement is
// subtle. It is plain for the cursor and for anything off to one side.

namespace gbarecomp { struct ExtendedViewFrameInfo; }

namespace kol {

void sound_frame(const gbarecomp::ExtendedViewFrameInfo* f);

}  // namespace kol
