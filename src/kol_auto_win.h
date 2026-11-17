#pragma once

namespace kol {

// Debug > Instant Win: with it on, every battle is won at its first unit's
// turn, by the game's own outcome check and victory (kol_auto_win.cpp: a
// reviewed hook on the battle task sends it to the check, another answers the
// check's "is the enemy side out?" with yes). KOL_NO_AUTO_WIN=1 leaves the
// hooks out; KOL_AUTO_WIN_ANY=1 wins with the setting off (headless runs).
void auto_win_install();

// The check was started and waits for its answer (the second screen tags
// itself "auto-win" meanwhile).
bool auto_win_pending();

// Once a frame: a new battle (its preparation) or a state load lets the next
// turn start a win again; an answer still waiting is dropped on a state load.
void auto_win_tick();

}  // namespace kol
