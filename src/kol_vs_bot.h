#pragma once

// The computer's side of VS Mode before the battle (dev, with the AI army
// switch, kol_states): it dispatches random units on random deployment cells,
// starts the battle and appoints a random leader, by pressing keys as a player
// would, read from the game's own state. See kol_vs_bot.cpp.

namespace kol {

// Wraps the host's key filter (after the menu's and the tile editor's).
void vs_bot_install();
// Once a guest frame, from the frame hook: decides the next press.
void vs_bot_tick(unsigned long frame);

}  // namespace kol
