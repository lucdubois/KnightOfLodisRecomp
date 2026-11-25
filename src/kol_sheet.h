// kol_sheet.h — the unit's status sheet, drawn with the game's own art.
//
// Everything here comes from the cartridge: the sheet's tileset (decoded from
// its A7 archives, see kol_a7.h), the BG palette, the text font, and the
// pre-rendered name strips. The tile numbers and positions are the ones the
// game's own status window uses, read from its tilemaps, on a canvas three
// rows taller than the window (30x23 tiles): one row between the portrait and
// the stats, and a 4 px gap above each equipment/talent slot. The second
// screen keeps the space below it free.
// See README "The status sheet, from ROM".

#pragma once

#include <cstdint>

#include "kol_unit_data.h"

namespace kol {

// 31 columns: the Thor's second screen is 1240 px wide, so at its 5x scale
// a 248-px canvas fills it edge to edge where 240 left 20 px each side. The
// layouts were the window's 30 columns; each page puts the 31st where it
// reads best (kExtraCols in kol_sheet.cpp).
constexpr int kSheetCols = 31;
constexpr int kSheetRows = 23;
constexpr int kSheetW = kSheetCols * 8;   // 248
constexpr int kSheetH = kSheetRows * 8;   // 184
constexpr int kSheetLowerRow = 11;         // the lower card starts here

// Render `u` into a kSheetW x kSheetH ARGB canvas. False when the ROM art is
// unavailable (no ROM, or an archive fails to decode), so a caller can keep
// its older panel rather than draw half a sheet. With a valid `target` (the
// unit under the cursor, kol::cursor_unit_record), the lower half is that unit's
// card instead of `u`'s equipment and talents.
// `page` draws that lower page instead of the current one (see below), with
// no emblem lit.
bool render_unit_sheet(const UnitInfo& u, std::uint32_t* argb,
                       const UnitInfo* target = nullptr, int page = -1);

// While persuading: the chance in the battle confirm box's yellow digits,
// centred between the two cards of a sheet render_unit_sheet drew.
void draw_sheet_chance(std::uint32_t* argb, int percent);
// The battle confirmation between the two cards, while its arrow is up:
// the arrow's head pointing down (bobbing by `now_s`), each box's damage
// and hit lines, the action's word (kol_sheet.cpp); `beside_chance` moves
// the middle left of the persuade chance.
void draw_sheet_forecast(std::uint32_t* argb, double now_s, bool beside_chance);

// The lower card's page, as the game's status window pages it: 0 is EQUIP.
// and TALENT, 1 the EMBLEM page (the unit's 32 emblem slots, earned ones with
// their picture). The second screen's swipe flips it, and it stays on that
// page for every unit until the next swipe. It never applies to the
// two-card sheet (a target below the acting unit).
int sheet_lower_page();
void sheet_flip_lower_page();

// A tap at canvas pixel (x, y) on a slot of the lower card selects it: its
// HELP text then covers the top of the sheet as SELECT shows it in the game,
// with the slot lit under it. Slots 0-3 are EQUIP.'s rows and 4-7 TALENT's on
// page 0, 0-31 the emblems on page 1 ("not earned" for an empty one).
// Tapping it again, or anywhere else, clears it. Returns whether that changed
// anything.
bool sheet_tap(float x, float y);
// The selected slot on the current page, -1 for none.
int sheet_selected_slot();
// Drops the selection: a slot with nothing in it has no text to show.
void sheet_clear_selection();

// A HELP card as the sheet floats them: `title` (text codes) as a line of
// its own, then `text`'s help_codes reflowed, at the top of a kSheetW x
// kSheetH canvas, transparent below. Returns the card's height, 0 for none.
// With `fixed_h` the card is at least that tall, its text from the top.
int render_help_card(const font::Code* title, int title_len, const UnitInfo& text,
                     std::uint32_t* argb, int fixed_h = 0);

// The army on the world map (kol_unit_list): a card a unit, two to a row,
// kArmyCellH tall: portrait, name, Lv., HP: and MP: (their maxima). Into
// `argb`, kSheetW wide, at most `max_h` tall; returns the height drawn (at
// least kSheetH), 0 without the ROM art or units. Card k is at x (k % 2) *
// kArmyCellW, y (k / 2) * kArmyCellH.
constexpr int kArmyCellW = kSheetW / 2, kArmyCellH = 64;
// The magic shop's box (88 tall) with a unit tapped: stage 1 its slots as buttons,
// 2 the question, all as the bottom bar's buttons (the user asked for each slot to
// act exactly like the option button). `pressed` a slot, kSpellYes or kSpellNo, else -1.
constexpr int kSpellBoxH = 88, kSpellSlotW = 104, kSpellSlotH = 24;
constexpr int kSpellSlotX[2] = {16, 128}, kSpellSlotY[2] = {26, 52};
constexpr int kSpellButtonW = 72, kSpellButtonH = 24, kSpellButtonY = 50, kSpellYesX = 40, kSpellNoX = 136;
constexpr int kSpellYes = 10, kSpellNo = 11;
void render_spell_box(const UnitInfo& u, std::uint32_t record, int spell, int stage, int slot, bool afford,
                      bool free_copy, int pressed, std::uint32_t* argb);
// The same box in an item shop: the unit's four items as buttons; then the question
// with the changes the item would make, `after` the unit wearing it.
constexpr int kItemBoxH = 120, kItemButtonY = 86;
constexpr int kItemSlotY[2] = {36, 68};
void render_item_box(const UnitInfo& u, std::uint32_t record, int item, int stage, int slot, bool afford,
                     bool free_copy, int pressed, const UnitInfo* after, std::uint32_t* argb);
int render_army_list(const UnitInfo* units, int n, std::uint32_t* argb, int max_h);

}  // namespace kol
