// kol_unit_data.h — a unit's full status, read from the record and the ROM.
//
// This replaces the speculative-capture path entirely. Everything the game's
// status window shows about a unit, except the portrait and Move, exists in
// the unit record plus a handful of ROM tables — so it can be read directly, for any unit on the map, with no fork, no window and no
// waiting. See README.md "The unit record and the ROM tables it indexes".
//
// The record is 0x68 bytes and the roster is contiguous from 0x02001420. The
// old "stride 0x4E0" in this file's history was WRONG and hid every unit but
// two: Alphonse is index 0 and Rictor index 12, and those two happen to sit
// 0x4E0 apart, so with only that pair the gap looked like the stride.

#pragma once

#include <cstdint>

namespace kol {

// One name as the game stores it. Most are literal text in the ROM string
// pool; the rest are a `0x81 NN` escape, which is NOT a dictionary reference
// but a PRE-RENDERED BITMAP — which is why compound names like "Tower Shield"
// can never be found as text, and why the window draws them into dynamically
// allocated tile runs with no code->tile mapping.
struct UnitName {
    char text[40] = {};   // decoded literal; empty when bitmap >= 0
    int  bitmap   = -1;   // escape index, or -1 when this is text
    // As the ROM stores it, for drawing in the game's own lettering: the
    // character codes of a text name, or the strip id of a pre-rendered one
    // (-1 when the name is text). Set even when `text` holds a transcription.
    std::uint8_t codes[40] = {};
    int ncodes = 0;
    int strip = -1;
};

struct UnitInfo {
    bool valid = false;
    std::uint32_t record = 0;

    char name[16] = {};
    std::uint8_t name_codes[16] = {};   // record +0x30 as stored
    int name_ncodes = 0;
    int level = -1, exp = -1;
    int hp = -1, hp_max = -1, mp = -1, mp_max = -1;
    // The HP as the sheet animates it (sheet_hp): the bar's solid part at
    // hp_bar and, while a blow drains it, the lost part up to hp_ghost (red;
    // a heal's part gained, green), white while hp_flash. -1: the bar is
    // drawn at `hp`, as stored.
    int hp_bar = -1, hp_ghost = -1;
    bool hp_flash = false, hp_heal = false;
    // The second gauge is SP, not MP: bit 7 of class field 0x0F (u16 +0x18,
    // with the accessor's fallback), as the game's map box (0x0801ED9A) and
    // status window (0x0806EF28) decide. The figures are the same record
    // fields either way.
    bool sp = false;
    // The move row under the MP/SP bar, as the status window draws it: the
    // land and water movement icons (0x0803BDC0: land 0-9, water 0-4) and the
    // speed x10 (0x08139920, printed as "7.0"). -1: unknown.
    int move_land = -1, move_water = -1, speed = -1;

    // Base figures as stored, and the equipment-adjusted ones the window
    // shows. The adjustment is a plain sum over the four equipment slots.
    int str_base = -1, int_base = -1, agi_base = -1;
    int str_ = -1, intel = -1, agi = -1;

    // melee ATK = STR + max(weapon power, class innate) * class weapon% / 100
    // magic ATK = the equipment-adjusted INT.
    int atk_melee = -1, atk_magic = -1;

    // The window's defence pair, as the game's own routine at 0x08139FCC
    // computes it for the sheet (see README "Defence"):
    //   DEF   = 100 * (STR + armour + beast innate)
    //           / (class dmg% + sum(item +0x16) / 2)
    //   M.DEF = 100 * (INT + a guest-bug term)
    //           / ((six class elemental % + sum(items' six) / 2) / 6)
    // -1 when this unit takes a branch the decode does not model.
    int def = -1, mdef = -1;

    // Record +0x54 and +0x55, as the game's own panel and sheet draw them:
    // element 1 Earth, 2 Fire, 3 Wind, 4 Water, 5 Holy, 6 Dark; alignment
    // 1 Lawful, 2 Neutral, 3 Chaotic. -1 when the byte is out of range.
    int element = -1, alignment = -1;
    bool other_team = false;   // record +0x5D bit 7: not the player's side

    // The support/fear gauge under the sheet's portrait, -2..+2, as the
    // game's 0x0813B7C8 computes it from the units within three squares.
    // `support_known` is false when it depends on something the decode does
    // not read (a made item's type).
    int support = 0;
    bool support_known = false;

    // The unit's portrait, as the game's loader (0x0801C1BC) picks it: up to
    // two 48x48 layers, each an index into the portrait archive (1-based, 0 =
    // none) and a palette index. A generic unit gets its CLASS portrait (one
    // layer) -- the game's own status window shows its walking sprite there
    // instead, but the ROM holds a painted portrait per class; a named
    // character gets its own, as the window shows it.
    int portrait_tiles[2] = {0, 0};
    int portrait_pal[2] = {0, 0};

    UnitName cls;
    UnitName equip[4];
    int equip_id[4] = {0, 0, 0, 0};   // item id; 0 is an empty slot
    // The picture each slot shows: item field 30, the item's own id or, for
    // a made item (id above 0xC7), the id of the item it was made from.
    int equip_pic[4] = {0, 0, 0, 0};
    UnitName talent[4];
    int talent_id[4] = {-1, -1, -1, -1};   // talent table index; -1 never filled
    // From the talent's ROM record: its kind (+0x02) and, for kind 8, a
    // subtype (+0x08), which pick the icon (0x0803EF44); POW (+0x22) and MP
    // cost (+0x0F), which the window's last column shows one or the other
    // of, by a flag bit (0x0803D978).
    int talent_kind[4] = {0, 0, 0, 0};
    int talent_sub[4] = {0, 0, 0, 0};
    int talent_pow[4] = {0, 0, 0, 0};
    int talent_mp[4] = {0, 0, 0, 0};

    // The emblems the status window's EMBLEM page shows: bit i is slot i
    // (row i / 8, column i % 8), the record's u32 +0x08 masked by the unit's
    // class, as the window's 0x0803C01C sets it up (control block +0x6198).
    // Two slots have a picture and text that depend on record +0x5E (0 a
    // dragon, 1 a man, 2 a woman), which emblem_picture and emblem_help take.
    std::uint32_t emblems = 0;
    int emblem_kind = 0;

    // The description the game's HELP box gives the thing under the status
    // window's cursor: an equipment or talent slot or list as it moves, and
    // any row of SELECT's help cursor (stats, class, birthday, element, the
    // MOVE icons...), roster and battle alike. The text's codes as the ROM
    // stores them, with `0x87 NN` names and `0x80 NN` figures written out:
    // 0x88 ends a line, 0x8E 0x8A a page (two lines, as the box shows them),
    // 0x8D 0x8A the text. help_item is the item's id, 0x1000 + a talent's,
    // or 0x2000 + any other help id; help_len is 0 when there is none.
    int help_item = 0;
    std::uint8_t help_codes[512] = {};
    int help_len = 0;
};

// The EMBLEM page's slot `slot` (0-31): its picture in the emblem archive
// (0x086D6B7C, two entries each, as 0x0803C0C6 loads them), and its HELP text
// id, as the help builder's emblem rows (kind 10, 0x0802ADB6) pick it:
// "not earned" for an empty slot.
int emblem_picture(int slot, int kind);
unsigned emblem_help(int slot, bool earned, int kind);

// Hand over the cartridge image. Nothing below works until this is called.
void unit_data_set_rom(const std::uint8_t* rom, std::uint32_t size);

// The unit under the cursor, decoded. False when the cursor names no unit,
// the ROM is not set, or the record is outside EWRAM — it fails closed rather
// than reading whatever happens to be there.
bool unit_for_highlighted(UnitInfo* out);

// The same for a given unit record (EWRAM address); false outside EWRAM.
bool unit_for_record(std::uint32_t record, UnitInfo* out);

// The shop's Buy list: the item under its cursor, -1 for none (no list
// open). An item's slot kind (0 not worn, 1 one-handed weapon, 2 two-handed,
// 3 shield, 4 armour, 5 accessory, 6 shoes). Whether the unit can wear it
// (the game's own rule, slots aside), and the unit as it would be with it
// on (its stats into `out`).
void shop_list_frame();   // every game frame: is the Buy list running?
int shop_cursor_item();
int shop_cursor_spell();   // in the magic shop: the talent under the cursor, else -1
bool unit_can_use_spell(std::uint32_t record, int id);
bool spell_help(int id, UnitInfo* out);
bool shop_spell_help(UnitInfo* out);
void spell_name(int id, UnitName* out);
int spell_price(int id);
int spell_icon_kind(int id);
int spell_icon_sub(int id);
std::uint32_t war_funds();
// The magic shop's spell under the cursor: a copy owned and not equipped (the
// stock entry {id, owned, equipped}: block +0x0C + 4 * block[+0x2AC + cursor]).
bool shop_spell_free();
// It can be had: a free copy, or the funds for one.
bool shop_spell_available(int spell);
// The item shops' the same. Item `id`'s name, picture, price and HELP.
bool shop_item_available(int id);
void shop_item_name(int id, UnitName* out);
int shop_item_price(int id);
bool shop_item_help(int id, UnitInfo* out);
// Whether item `id` can go in `slot`, what is there coming off, as 0x0814165C
// decides it. A two-handed weapon takes off any other weapon as well.
bool item_slot_takes(std::uint32_t record, int slot, int id);
// A unit's spell slot: true if a spell bought for it can go there. 255 is a slot
// it does not have (the crossed box), an innate talent stays, and a slot holding
// the spell itself has nothing to gain.
bool spell_slot_takes(std::uint32_t record, int slot, int spell);
bool unit_has_spell_slot(std::uint32_t record);
int item_category(int id);
bool unit_can_equip(std::uint32_t record, int id);
// ... with it in a given slot (0-3), and the kind of what a slot holds
// (item_category; 0 empty, -1 unknown).
bool unit_with_item_at(std::uint32_t record, int id, int slot, UnitInfo* out);
int unit_slot_category(std::uint32_t record, int slot);

// While a unit chooses whom to persuade: the game's chance (0-100) of
// persuading `target`, the confirm box's "% HIT"; -1 otherwise.
int persuade_chance(std::uint32_t target);

// In Organize's item or spellbook stock (its R menu's Item and Spell), the
// HELP text of the item or spellbook under the list's cursor, into
// out->help_codes (help_item names it). False elsewhere.
bool stock_help(UnitInfo* out);

// HELP text `id` (bank 0x0874B6DC up to 0x1FF, 0x0875976C past it) into
// out->help_codes / help_len, as the game's help box has it.
bool help_text_for(unsigned id, UnitInfo* out);

// The units the second screen's sheet shows. While one of the player's units
// takes its action (kol::unit_in_action: its menus, move and target choice;
// not while its cursor is only out looking around, kol::cursor_browsing),
// `top` is that unit (kol::acting_record), and true means `bottom` holds the
// unit under the map cursor (kol::cursor_unit_record) when that is someone
// else, or, in the Attack/Cancel confirmation (kol::target_confirming), the
// target. Otherwise `top` is `hovered`. The game's highlighted unit follows the
// cursor and stays on the last unit it passed, so it is never the actor.
bool sheet_units(const UnitInfo& hovered, UnitInfo* top, UnitInfo* bottom);

// A battle unit's HP as its card shows it: held at what it was while an
// action's result waits for its damage number, then drained (hp, hp_bar,
// hp_ghost, hp_flash). sheet_hp_busy: the card's HP is held or still moving.
void sheet_hp(UnitInfo* u);
bool sheet_hp_busy(std::uint32_t record);
// The unit the acting unit's talent or attack hit, once its number came up
// (it need not be the target: a unit in the way); 0 otherwise.
std::uint32_t sheet_hit_target();

// The second screen's sheet at `now_s` (seconds, any steady clock): the units
// sheet_units picks, and when the target shown in the lower card dies, that
// card flashing red and fading into the attacker's full sheet when the game
// pops the killing damage number over its sprite. False keeps
// the previous picture (no unit to draw, or no ROM art).
bool compose_unit_sheet(const UnitInfo& hovered, std::uint32_t* argb, double now_s);

// The same sheet with its OTHER lower page (kol::sheet_lower_page), for the
// second screen's swipe to show beside the current one while a finger drags
// it. False when the sheet has no pages to swipe: the two-card sheet.
bool compose_other_page(const UnitInfo& hovered, std::uint32_t* argb);

// A `0x81 NN` name as pixels: ARGB, 8x16 per character cell, alpha 0 where the
// glyph is not drawn so the panel can tint it in its own colour rather than
// pasting the game's parchment palette into a dark panel.
bool name_bitmap(int id, std::uint32_t* argb, int* w, int* h);

// The status sheet's own frame art, read straight from the cartridge: the
// four corners, the four edges and the four hatched field tiles, as a 12-tile
// atlas (96x8 ARGB) in that order. Static ROM data, so it needs no capture
// and is available before the sheet has ever been opened.
bool sheet_frame_atlas(std::uint32_t* argb, int* w, int* h);
int sheet_frame_atlas_pixels();


// The ROM address of a pre-rendered name strip's pixels (4bpp, one 8x16 cell
// per 64 bytes, 16 rows of 4 bytes) and its cell count; 0 when unknown.
int name_strip_raw(int id, std::uint32_t* rom_addr);

// The cartridge image handed to unit_data_set_rom(), for other readers of
// the same tables.
const std::uint8_t* unit_data_rom(std::uint32_t* size);

// Upper bound for the buffer name_bitmap() writes into.
int name_bitmap_max_pixels();

// KOL_UNIT_DUMP=1 prints the decoded unit whenever the cursor moves to a
// different one. This is the desktop check for a panel that otherwise only
// exists on a second screen: drive the guest over --tcp, sweep the cursor, and
// read the decode back without an Android build. No-op unless set.
void unit_dump_tick();

}  // namespace kol
