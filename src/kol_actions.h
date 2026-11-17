// kol_actions.h — game actions a host UI can trigger (on-screen buttons).
//
// Each action runs the game's OWN routine for it, entered through a reviewed
// [[mod_function_hook]] (game.toml), so the game does exactly what it would
// do for the pad gesture. Requests may come from any thread; the hook serves
// them on the guest thread at the next safe call.
#pragma once

#include <cstdint>

namespace kol {

// Register the hooks. Idempotent; KOL_NO_ACTIONS=1 leaves them off.
void actions_install();
// How many maps the game has loaded (its map header parser, 0x08021B68:
// every battle map and story scene map): effects that start over with a new
// map compare it (kol_perspective's wetness).
unsigned map_loads();
// Debug > Battle Map: the D-pad pans a story scene's camera (once a frame).
void debug_scene_camera_tick();
// The scene viewer is on: Battle Map set, and a story scene loaded it in
// this run of the state.
bool debug_scene_viewer_active();
// The viewer's tile cursor (kol_actions steps it with the D-pad): false until
// kol_focus sets it from the view's middle; and the camera that centres it,
// worked out by kol_focus from the projection.
bool debug_viewer_cell(int* cx, int* cz);
void debug_viewer_cell_init(int cx, int cz);
// Moves it (the second screen's editor page: a tapped cell).
void debug_viewer_cell_set(int cx, int cz);
void debug_viewer_camera_goal(int x, int y);
// kx + camera, ky + camera y: the projection's offset from the camera,
// constant for the scene (kol_focus learns it once, the camera still).
bool debug_viewer_anchor(int* ax, int* ay);
void debug_viewer_anchor_set(int ax, int ay);

// On the battle map, the game's hold-L + A (+ B for previous): moves the
// cursor to the next (previous) unit that can still act. The position in the
// cycle is kept here, so the first tap goes to the first available unit and
// each later tap moves on; after two seconds without a tap it starts over.
// With a status window up, shows the next (previous) unit of the same army
// or side in it instead — something the game itself does not offer.
void action_next_unit();
void action_previous_unit();

// On the battle map, while the cursor is free: sends it to the unit of map
// object `index` (0-25), any side, as the game's selections do (a glide).
void action_cursor_to_object(int index);

// On the world map, roaming or in its command menu: opens the status window
// Organize opens, on the army unit at `record`, and B there closes the menu.
void action_open_profile(std::uint32_t record);
// From that tap until the menu is closed again (the window up included),
// and while Organize, however it was opened, has a unit's window up (in its
// grid the second screen shows the cursor's unit's sheet instead).
bool action_profile_busy();
// On the world map, roaming or in its command menu: the game's own save /
// load screen, as the system submenu's Save / Load opens it; back from it,
// the menu closes again when the tap came from roaming.
void action_world_save();
void action_world_load();
// Organize's Item (0), Spell (1) and Class (2) screens, as its grid's R menu
// opens them, and the main menu's Training (3) and Shop (4), the same way; and which of Training (bit 0) and Shop (bit 1) the menu would
// list where the army is (a town for the shop).
void action_world_item(int item);
// Organize's grid or its R menu is up with nothing under way: there Item,
// Spell and Class open their screens straight (the bar keeps them live).
bool action_organize_ready();
int action_world_items_ok();
// One of the menu's screens (save, load, Organize, training, shop) is up or
// on its way, however it was opened.
bool action_file_screen_open();
// Meanwhile, the army record the Organize grid's cursor or the status
// window is on (0 for none yet).
std::uint32_t action_roster_unit();

// The controller: R and L on the shoulders (the game's own), the top and
// left face buttons the unit buttons (Shortcut A/B, Next/Previous).
void pad_install();
// Swap Face Buttons (Options > Gameplay): the game's A and B on each other's
// buttons, and the unit buttons X and Y too. Applies at once.
void pad_swap_set(bool on);

// The last message the move choice put up and the game was spared (its text
// bank and entry, same layout as the HELP banks), and a count that goes up
// each time one is. 0 before any.
unsigned move_message(std::uint32_t* bank, unsigned* index);

// The NAVI box the game was spared in battle preparation ("To form an Attack
// Team..."): its bank and entry, the number its figure 0 shows (the team
// size, -1 unknown) and a count that goes up each time. 0 before any.
unsigned navi_message(std::uint32_t* bank, unsigned* index, int* number);

// Every controller button press so far (a count); note_key_press adds one
// (tests).
unsigned key_presses();
void note_key_press();

// The unit the status window shows while it is up, else 0.
std::uint32_t status_window_unit();

}  // namespace kol
