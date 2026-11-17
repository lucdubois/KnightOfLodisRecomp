// kol_fast_actions.h — the battle's actions without their pauses.
//
// Every battle action (a unit picked, a range shown or cleared, an attack,
// the enemy deciding) runs one of a few routines over the map's 1024-cell
// table inside the VBlank handler, 3 to 6 frames each (README "The pauses on
// every battle action"): no frame is presented meanwhile, so the picture
// freezes 70-125 ms, on the cartridge as here. With Options > Advanced > Fast
// Actions on, the guest clock is held while such a routine runs: the game's
// own recompiled code does the work, so the bytes it leaves are its own, and
// only the time it took is gone. The routine touches no I/O (measured with
// the runtime's read/write-set recorder), so nothing it does needs the clock.
// Off (the default, and the Original preset), nothing here runs.
//
// How: the runtime's function-entry hook sees the routine's entry (the
// clock is held, its stack pointer kept) and the first entry at or above
// that stack pointer after it (the routine has returned: the clock runs
// again). A cap of kCapFrames lets time run again if a run goes on past it.
//
// KOL_FAST_ACTIONS_LOG=1 logs each run (routine, frames skipped, host ms);
// KOL_FAST_ACTIONS_CHECK=1 also records the run's reads and writes and
// reports any that touched I/O.

#pragma once

#include "mod_function_hooks.h"   // ArmCpuState

namespace kol {

// Chain onto the runtime's function-entry hook. Once, after the other
// users of that hook (the terrain probe, the strike camera) are installed.
void fast_actions_install();

// Fast Attacks and Fast Spells, at the game's sound mix (MP2K SoundMain's
// entry, once a frame, kol_menu's hook): on an extra frame of a strike or a
// spell at 2x (decided at SoundVSync, see kol_fast_actions.cpp), the mix is
// skipped too (true: R15 set to its caller).
bool fast_attack_on_sound_main(ArmCpuState* cpu);

}  // namespace kol
