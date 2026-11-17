// kol_unit_list.h — the second screen's list of the battle's units.
//
// While the player browses the battle map, the second screen shows every unit
// on it, as the game's own "View List" does: the player's side on the left,
// the others on the right, each unit in a framed box with its HP bar beside
// it. Within a side the units go by the game's number (record +0x51, the
// window's "NO."), and those whose turn is done go last. A done unit wears
// the END plate the game paints into its sprite over the map.
//
// Everything is the game's: the sprites are each unit's current frame, drawn
// from its own actor as the game draws it on the map; the boxes, HP bars,
// parchment and LEADER plate are cartridge tiles. See README "The unit list
// on the second screen".
#pragma once

#include <cstdint>

namespace kol {

constexpr int kListW = 248;   // the second screen's canvas, kol::kSheetW (kol_sheet.h)
constexpr int kListMaxH = 16 * 64;       // the army on the world map: 32 cards, two a row

// The list's picture (kListW wide, ARGB) and its height, at least the status
// sheet's 184 (the second screen shows that window and scrolls the rest); 0
// when there is no list: not a battle, battle preparation, or the map cursor
// is not out browsing (a unit's action, a menu, the enemy's turn).
// `on_unit` receives the record of the unit on the cursor's cell, 0 on empty
// ground; the second screen shows that unit's card over the list.
int compose_unit_list(std::uint32_t* argb, std::uint32_t* on_unit);

// On the world map (its layer layout: DISPCNT 9F40, BG0-3 2F03 2E02 2D08
// 2C09, the save and title prompts over it included), the list is the army
// instead: every unit on the roster (32 records from 0x02000030, stride 0x68,
// empty ones' class byte +0x4F 0), in roster order, a card each with its
// portrait (kol_sheet's render_army_list). `on_unit` stays 0: a tap opens
// the game's own status window on the unit, on the top screen
// (kol::action_open_profile), and the army list stays up while it is open.
bool world_map_scene();

// A tap on the list at canvas (x, y) of the last picture composed: the unit
// there, if any, is selected on the battlefield (the map cursor glides to
// it, as the game's own selections send it). Its record, or 0 for none.
// The rows at the top of the list's picture that stay when it scrolls.
int unit_list_pin();
// The list composed last was a shop's: no bottom bar on the second screen.
bool unit_list_in_shop();
// How many faces the magic shop's picture last had.
int unit_list_faces();
// The finger on the magic shop's buttons: phase 0 down, 1 moved, 2 up, 3 cancelled.
int unit_list_press(float x, float y, int phase);
std::uint32_t unit_list_tap(float x, float y);

// Desktop check: KOL_LIST_DUMP=<file.ppm> writes the list whenever its
// picture changes ("%d" takes the frame number), and KOL_LIST_TAP=
// "frame:x:y,..." taps it there on those frames, counted from the
// first frame the list shows. No-op unless set.
void unit_list_dump_tick(unsigned frame);

}  // namespace kol
