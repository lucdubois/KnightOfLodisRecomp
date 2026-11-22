#pragma once

// VS Mode against the computer (desktop; Options > Gameplay > VS Computer).
//
// At VS Mode's "Connecting..." screen this game takes a snapshot of itself,
// starts a second copy of the program from it, unseen (no window, no sound),
// as the other GBA on the link cable, and holds still until the two are
// linked. That copy plays its army with the game's own AI and answers the
// screens before the battle (kol_states ai_army, kol_vs_bot). When the game
// closes its link, or this one ends, the copy ends too. See kol_vs_cpu.cpp.

#include <string>
#include <vector>

namespace kol {

// This process is that unseen copy (KOL_VS_OPPONENT=1, set by the parent).
bool vs_cpu_child();
// The run's own arguments (desktop_prepare's), for the copy's command line.
void vs_cpu_args(const std::vector<std::string>& args);
// Once, from the frame hook's installs: the parent's key filter (it watches
// the link while the game is held), the copy's watchdog.
void vs_cpu_install();
// Once a guest frame, from the frame hook.
void vs_cpu_tick();
// kol_states' state hooks ask these first: the snapshot's path for its own
// slot (null for any other), and whether a save just written was the snapshot
// (then the copy is started).
const char* vs_cpu_state_path(int slot, int save);
bool vs_cpu_state_saved(const char* path);

}  // namespace kol
