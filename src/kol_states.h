#pragma once

// Numbered save states, for testing (Options > Debug > Numbered States).
//
// With it on, the pad's state combos keep a numbered list instead of slot 1:
// L3 + R saves a new state, L3 + L loads the latest. Each state is three
// files in <files>/states/ (the app's files directory; KOL_STATES_DIR on the
// desktop): NNNN.state, NNNN.png (the frame as the game drew it, before our
// effects) and NNNN.txt (Alphonse's level, the map, the time). The Debug
// page's Save States screen lists them, newest first, with the picture and
// the level; a tap loads one, a hold then a tap deletes one.
// tools/thor_states.sh mirrors them into the project's states/thor/. See
// README "Numbered save states".

#include <cstdint>
#include <string>
#include <vector>

namespace kol {

// Points gbarecomp's state hooks here (g_runtime_state_path_hook,
// g_runtime_state_saved_hook). Once.
void states_install();

struct StateEntry {
    int number = 0;
    int level = -1;   // Alphonse's (roster record 0), -1 unknown
    int map = -1;
    std::string when;   // "2026-10-28 13:40"
};
// Every numbered state, newest first (cached; read again after a save).
std::vector<StateEntry> states_list();

// State `number`'s picture scaled to fit tw x th (its aspect kept), as ARGB.
// False when it has none.
bool states_thumb(int number, int tw, int th, std::vector<std::uint32_t>* argb, int* w, int* h);

// Loads state `number` at the next safe point (any thread).
void states_load(int number);

// Deletes state `number`'s three files. Its number is not given again.
bool states_delete(int number);

// Desktop check, every frame: KOL_STATES_TEST="s20,s40,l60" asks for the
// combos' save (s) and load (l) at those frames after start, as L3+R and
// L3+L do (slot 1, numbered with the setting on); "p3@70" loads state 3.
void states_test_tick();

// KOL_AI_ARMY=1 (dev): F7 hands the player's own army to the game's AI and
// takes it back (see the .cpp).
bool ai_army_enabled();
void ai_army_toggle();
// The switch is on (the AI plays this army).
bool ai_army_active();
void ai_army_tick(unsigned long frame);
// The turn-fork hook (VS Mode: this game's own turns go to the AI while F7 is on).
void ai_army_install();

// A new numbered state, as L3+R makes one (the Save States page's save
// button): asks the runtime for slot 1, which the path hook numbers.
void states_save_new();

// The latest numbered state, as L3+L loads it (slot 1; desktop F1 and F6).
void states_load_latest();

}  // namespace kol
