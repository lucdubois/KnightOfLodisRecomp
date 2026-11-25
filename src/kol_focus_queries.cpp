// kol_focus_queries.cpp — the guest-state queries (kol_focus.h): the
// cursor, the acting unit and its target, the damage numbers, talents,
// where a map object is drawn. On the present path (t_present,
// kol::PresentScope) they answer from the capture, elsewhere (the sheet, the
// sound, the HUD) from live memory (guest_ew / guest_iw).

#include "kol_focus.h"
#include "kol_focus_internal.h"
#include "kol_region.h"

namespace kol {
// While a unit takes its action and the cursor is out choosing its move or
// target (unit_in_action, cursor_on_map), the unit on the cursor's cell: the
// map object standing on the cursor slot's world cell. In range or not, moving or attacking: the user wants the unit
// shown wherever the cursor rests (README "The acting unit and the unit
// under the cursor").
// A dead unit's map object stays in the list at the cell where it fell
// (states/thor/0066: Ivanna under Lechies, the user reported dead Ivanna
// still shown and suggested a flag must mark her as off the
// field): object +0x0B is 0 for it, 2 for a
// unit on the field (every unit of 26 battle states; the five dead ones in
// 0065-0069 all 0).
bool object_on_field(const std::uint8_t* ew, std::uint32_t obj) {
    return ew[obj - 0x02000000u + 0x0Bu] != 0u;
}

namespace {
// An EWRAM address as an offset into the EWRAM copy.
constexpr std::uint32_t ew_off(std::uint32_t a) { return a - 0x02000000u; }
// The acting unit's map object (see acting_record).
constexpr std::uint32_t kActingObject = usa(0x0202A5F0u);
// The move task's state byte (see move_choice_active, cursor_browsing).
constexpr std::uint32_t kMoveTaskState = usa(0x0202A5B0u) + 0x27Eu;
// Whether o is an entry of the map objects' list (at list).
bool is_map_object(std::uint32_t list, std::uint32_t o) {
    return in_ewram(list, kObjects * kObjStride) && o >= list && o < list + kObjects * kObjStride &&
           (o - list) % kObjStride == 0;
}
// The map object standing at world (x, z) (a cell's middle), not skip, its
// unit record valid; with on_field, only a unit on the field
// (object_on_field). 0 when none. The list (at list) must be valid.
std::uint32_t object_at(const std::uint8_t* ew, std::uint32_t list, unsigned x, unsigned z, std::uint32_t skip,
                        bool on_field) {
    for (int i = 0; i < kObjects; ++i) {
        const std::uint32_t o = list + i * kObjStride;
        if (o == skip || (on_field && !object_on_field(ew, o))) continue;
        if (static_cast<unsigned>(rd16(ew, ew_off(o + 0x10))) == x &&
            static_cast<unsigned>(rd16(ew, ew_off(o + 0x14))) == z && in_ewram(rd32(ew, ew_off(o)), 0x68))
            return o;
    }
    return 0u;
}
}  // namespace
std::uint32_t cursor_unit_record() {
    const std::uint8_t* ew = guest_ew();
    const std::uint8_t* iw = guest_iw();
    if (!ew || !iw || !unit_in_action() || !cursor_on_map()) return 0u;
    // The cell's middle, where its unit stands: the left stick draws the
    // cursor anywhere in the cell (kol_stick_cursor.cpp).
    int cx, cz;
    if (!map_cursor_cell(&cx, &cz)) return 0u;
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    if (!in_ewram(list, kObjects * kObjStride)) return 0u;
    const std::uint32_t o = object_at(ew, list, static_cast<unsigned>(cx * 32 + 16), static_cast<unsigned>(cz * 32 + 16), 0u, true);
    return o ? rd32(ew, ew_off(o)) : 0u;
}

// The map cursor is out: IWRAM 0x03000FD5 is 1 while the player moves it over
// the map, browsing (B_rictor_highlighted_map, BO, D_thor_unit_bar,
// H_terrain_info) or choosing the acting unit's move or target (CN, CO,
// S_move_highlight, BU_freelook_blurry, AV_pre_range, BV after backing out of
// Attack/Cancel). It is 0 in the command and weapon menus, the Attack/Cancel
// and spell confirmations (BV, BW), and on the enemy's turn (BL_enemy_turn,
// where it is also seen at 4).
bool cursor_on_map() {
    const std::uint8_t* iw = guest_iw();
    return iw && iw[usa(0x03000FD5u) - 0x03000000u] == 1u;
}

// An attack or spell waits on its confirmation (Attack/Cancel, the forecast
// with both units' HP): IWRAM 0x03000FF1 is 1. In CO it rises the frame A
// confirms the target and drops when B returns to targeting, and it stays 0
// in the weapon and command menus (CO, CN); it is 1 in BV and BW as saved.
bool target_confirming() {
    const std::uint8_t* iw = guest_iw();
    return iw && iw[usa(0x03000FF1u) - 0x03000000u] == 1u;
}

// A damage number is up over a sprite: EWRAM 0x0202AFCC points at its object
// from the frame before its digits appear until they leave. CO (Ivanna on
// Mulinne): set 264-353, digits 265-353 (OAM tiles 0x1E0/0x1DD). BV
// (Alphonse on Flauros): set at 112, digits at 113 (0x1D8/0x1D7). 0 before
// and after, including while the HP has already dropped.
bool damage_number_shown() { return damage_number_object() != 0u; }

std::uint32_t damage_number_object() {
    const std::uint8_t* ew = guest_ew();
    return ew ? rd32(ew, usa(0x0202AFCCu) - 0x02000000u) : 0u;
}

// The number's block (its sprite's, at [0x0202AFCC]) holds where it stands:
// +0x3C the world X (low half) and Y, +0x40 the world Z, the hit unit's
// place. IF (Crag Crush): (144,144), (144,112), (176,144), (112,144), its
// four units' places in the order their numbers came; ID: Elana's (400,
// 272); IE: Ivanna's. So the unit is the map object standing there.
std::uint32_t damage_number_unit() {
    const std::uint8_t* ew = guest_ew();
    const std::uint32_t blk = damage_number_object();
    if (!ew || !in_ewram(blk, 0x44)) return 0u;
    const unsigned x = rd16(ew, ew_off(blk + 0x3Cu)), z = rd16(ew, ew_off(blk + 0x40u));
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    if (!in_ewram(list, kObjects * kObjStride)) return 0u;
    return object_at(ew, list, x, z, 0u, true);   // (on the field: states/thor/0069, dead Ivanna's under Lechies)
}

// A unit of the player's is taking its action: EWRAM 0x0202B26F is 2 from its
// command menu on, through the move choice, the weapon menu, the target
// choice and the confirmations (CN, CO, BV, S_move_highlight, BU,
// AV_pre_range), and 0 while browsing the map (B_rictor_highlighted_map, BK,
// BO, D_thor_unit_bar, H_terrain_info) and on the enemy's turn
// (BL_enemy_turn). Found by diffing those two sets of snapshots.
// Battle preparation also leaves it at 2 (CU_prep_two_cards, AB, AI, BT),
// with 0x03000FF1 at 1 and 0x0202A5F0 on the last unit placed, which read as
// an attack confirmation: Allos on top and the roster list's Deneb below. No
// unit acts there, so it counts only once the battle is under way.
// The byte is the top of a pointer, 0x0202B26C, the battle menus' context
// (the weapon and talent lists, 0x08077000-0x08082000), and the game never
// clears it: once a battle's first list has opened it stays set, through
// browsing and through the AI's phases too (0x0202B740 in IA, all of its
// 6000 frames, with no write). On IA_slot2_sheet_keeps_rictor that held the
// second screen on Rictor through the enemy's and his own army's phases
// (the user, 10-20). So an AI army's phase is never the player's action,
// and neither is a moment with no acting unit at all (IA 5916-6268, between
// Rictor's phase and the player's, when the sheet held on him too).
bool unit_in_action() {
    const std::uint8_t* ew = guest_ew();
    return ew && ew[usa(0x0202B26Fu) - 0x02000000u] != 0u && !battle_preparation() && !ai_phase() &&
           acting_record() != 0u;
}

// An AI army's phase: bit 31 of the flags word at EWRAM 0x0202A900 (byte
// 0x0202A903 = 0x80). Found by diffing IA's enemy phase (frames 1000, 2000),
// Rictor's allied army's (3000, 4500, 5500) and the player's (6300, 6380):
// 0x80 in both AI phases, 0 in the player's; the game clears it (0x08075E18)
// at 5901, as the allied phase ends. 0x80 in every enemy-turn state (BL, GC,
// FF, HX, HZ, L, M, N), 0 in the 76 player's-phase battle states.
bool ai_phase() {
    const std::uint8_t* ew = guest_ew();
    return ew && (ew[usa(0x0202A903u) - 0x02000000u] & 0x80u) != 0u;
}

bool object_decal_hidden(std::uint16_t attr2) {
    const std::uint32_t v = g_hidden_decals.load(std::memory_order_relaxed);
    if (!v) return false;
    const unsigned tile = attr2 & 0x3FFu;
    if (tile < ((v >> 11) & 0x7FFu) || tile >= (v & 0x7FFu)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus && bus->read16(kMapNumber) == (v >> 22);
}

// A story event is playing: EWRAM 0x020013D0 is 1. Found by comparing the
// saved states (EWRAM at file offset 0xFC): 1 in all 45 story-scene states
// (the opening, the towns, the boat, map 59's house with Eleanor
// (states/IC_slot3_eleanor_house.state, the user's slot 3, where the map
// and a stale "acting" unit put two cards on the second screen), the
// pre-battle talk on map 2 (L, N: Alphonse and Stan), map 55's before its
// battle starts (GA)) and in the preparation screens whose NAVI script is
// running (BD-BJ, BN, BO, CB-CE, DL, DM); 0 in the other 198, every
// battle turn among them.
bool story_event() {
    const std::uint8_t* ew = guest_ew();
    return ew && ew[usa(0x020013D0u) - 0x02000000u] == 1u;
}

// Battle preparation (deployment, the roster list, its view mode): EWRAM
// 0x0202A81A, the move-choice task's +0x26A, is 0. It is 0xFF in every battle
// state here (the player's turn, the enemy's, targeting, the confirmations,
// status windows) and 0 in every preparation state (AB, AC, AI, BD-BI, BN,
// BO, BP, BT, BY, BZ, CA-CE, CU), 88 in the army roster between battles.
// Found by diffing seven preparation states against fourteen battle states,
// then checked on all 100.
bool battle_preparation() {
    const std::uint8_t* ew = guest_ew();
    return ew && ew[usa(0x0202A81Au) - 0x02000000u] == 0u;
}

// The move choice has just begun: a unit takes its action, the map cursor
// is out choosing a move (not a target: EWRAM 0x0202A753 is 0, 2 when
// targeting) and it still rests on the moving unit's own cell. While
// choosing a move the game draws a pale copy of the unit at the cursor, in
// the unit's own slot, but its map object stays where it stands.
bool map_cursor_cell(int* x, int* z) {
    const std::uint8_t* iw = guest_iw();
    if (!iw) return false;
    for (int s = 0; s < kSlots; ++s) {
        const std::uint32_t o = kSlotArray - 0x03000000u + s * kSlotStride;
        if (rd32(iw, o) != kCursorSlotFn) continue;
        // The middle of the cell, or, under the left stick, anywhere in it
        // (kol_stick_cursor.cpp).
        const int X = rd16(iw, o + 0x08), Z = rd16(iw, o + 0x0C);
        if (X < 0 || Z < 0) return false;
        *x = X >> 5;
        *z = Z >> 5;
        return true;
    }
    return false;
}

// The move choice: the acting unit's flags as for any cursor choice, and the
// move-choice task (0x0202A5B0, the r7 of the NAVI and message handlers at
// 0x080662F0 / 0x08066B28) in its choose-a-destination state 0x0B: set when
// the prompt closes and again after a message (CN, CT). B cancels through
// 17, 16, 18, 7 to 5, back on the map, while every other flag, the move range
// bits included, stays as it was; testing those kept the prompt's card up.
// S_move_highlight reads 0x0B; BU_freelook_blurry and AV_pre_range read 5.
bool move_choice_active() {
    const std::uint8_t* ew = guest_ew();
    // (unit_in_action holds an acting unit.)
    return ew && ew[ew_off(kMoveTaskState)] == 0x0Bu && unit_in_action() && cursor_on_map();
}

// The move task's state byte (0x0202A82E) by phase, read from the saved
// states: 0x02 the command menu (CL, CP), 0x05 the cursor out to look around
// (CK, CM, DD), 0x0B a move being chosen (CT), 0x07 and 0x09 a target (CN,
// CO; CS), and 0x07 / 0x17 again under the Attack/Cancel confirmation (BV,
// BW).
bool cursor_browsing() {
    const std::uint8_t* ew = guest_ew();
    return ew && ew[ew_off(kMoveTaskState)] == 0x05u && unit_in_action() && cursor_on_map();
}

bool move_choice_on_actor() {
    return move_choice_active() && cursor_unit_record() == acting_record();
}

// The unit whose turn it is. EWRAM 0x0202A5F0 points at its map object: at
// Alphonse's while he acts (CN, BV), and on BL_enemy_turn it steps through
// the enemies one after another as each takes its turn. The game's own
// "highlighted" pointer is no use here, since it follows the cursor onto the
// target.
std::uint32_t acting_record() {
    const std::uint32_t obj = acting_object();   // (its map object; its unit record at +0x00)
    if (!obj) return 0u;
    const std::uint32_t rec = rd32(guest_ew(), ew_off(obj));
    return in_ewram(rec, 0x68) ? rec : 0u;
}

std::uint32_t acting_object() {
    const std::uint8_t* ew = guest_ew();
    if (!ew) return 0u;
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    const std::uint32_t obj = rd32(ew, ew_off(kActingObject));
    return is_map_object(list, obj) ? obj : 0u;
}

// Two places name the acting unit's target as a pair of map objects
// {attacker, target}, found on BL_enemy_turn (Stella on Alphonse, whose HP
// drops at 2632):
// - 0x0203C734: the AI's choice, written as the enemy picks its target
//   (2110, the move task in state 07, before it moves). It is not cleared:
//   on Norton's turn it still read Stella / Alphonse.
// - 0x0202B710: the attack itself, written just before the blow (2612 /
//   2620, states 0C-08) and cleared after (2846). The block is shared
//   scratch: a spell aimed at a cell leaves a cell there instead of an
//   object (Norton: 0x00021308), and Ning's area technique another
//   layout entirely.
// The attack's pair can also name the target by its cell: on
// GC_slot2_enemy_hits_alphonse, Persace and Eiton write 0x00020909 at their
// blow (state 10), Alphonse standing on cell (9, 9), world (304, 304), and
// Bridd 0x00020F0A, unit 1 on cell (10, 15): byte 0 the X, byte 1 the Z.
// So a pair counts only while its attacker is the acting unit and its
// target is a map object other than it, or a cell a unit stands on. The
// attack's pair first.
std::uint32_t acting_target() {
    const std::uint8_t* ew = guest_ew();
    if (!ew) return 0u;
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    const std::uint32_t actor = rd32(ew, ew_off(kActingObject));
    auto is_obj = [&](std::uint32_t o) { return is_map_object(list, o); };
    if (!is_obj(actor)) return 0u;
    for (const std::uint32_t pair : {usa(0x0202B710u), usa(0x0203C734u)}) {
        if (rd32(ew, ew_off(pair)) != actor) continue;
        const std::uint32_t t = rd32(ew, ew_off(pair + 4u));
        if (t == actor) continue;
        if (is_obj(t)) {
            const std::uint32_t rec = rd32(ew, ew_off(t));
            if (in_ewram(rec, 0x68)) return rec;
            continue;
        }
        const unsigned cx = t & 0xFFu, cz = (t >> 8) & 0xFFu;
        if ((t >> 24) != 0u || cx >= 64u || cz >= 64u) continue;
        // (Any unit standing there, on the field or not.)
        if (const std::uint32_t o = object_at(ew, list, cx * 32u + 16u, cz * 32u + 16u, actor, false))
            return rd32(ew, ew_off(o));
    }
    // A talent's own block names its targets from the moment the unit picks
    // it (see talent_targets), before it moves: Rictor's heal on Lara. Read
    // after the attack's pairs, which an attack writes and a talent does
    // not, since the block is not cleared after its action.
    {
        std::uint32_t objs[1];
        if (talent_targets(objs, 1) == 1 && objs[0] != actor) {
            const std::uint32_t rec = rd32(ew, ew_off(objs[0]));
            if (in_ewram(rec, 0x68)) return rec;
        }
    }
    return 0u;
}

// A talent (a spell, a heal) in use: the block at EWRAM 0x02029D80 holds
// its ROM record (+0x00, 0x0861B1F0 for Rictor's heal), the unit using it
// (+0x04, its map object), and its targets (+0x20 the count, +0x28 on the
// objects); its result list at 0x02029EC0 holds a count, then per unit
// {object, flags, amount}: Lara, 0x80, 60 for a heal taking her 38 -> 98;
// on ID an Ice Javelin aimed at one unit names another, the one in the way,
// 0x80, 51 (331 -> 280). So 0x80 is no heal flag; the talent's record says
// (talent_results).
// Found on states/IB_slot2_rictor_heals_lara.state (Rictor picks it at
// 163, before he moves; the result is in from 313; the number pops at 749
// and goes at 833; the game writes her HP only at 870). Neither is cleared
// after (the result outlived the block into Orson's turn), so both count
// only while the block's user is the acting unit. Attacks use neither (IA:
// the block empty).
int talent_targets(std::uint32_t* objs, int max) {
    const std::uint8_t* ew = guest_ew();
    if (!ew) return 0;
    constexpr std::uint32_t kBlock = usa(0x02029D80u);
    const std::uint32_t actor = rd32(ew, ew_off(kActingObject));
    if (!actor || rd32(ew, ew_off(kBlock + 4u)) != actor) return 0;
    const std::uint32_t n = rd32(ew, ew_off(kBlock + 0x20u));
    if (n == 0u || n > 8u) return 0;
    const int k = std::min<int>(static_cast<int>(n), max);
    for (int i = 0; i < k; ++i) objs[i] = rd32(ew, ew_off(kBlock + 0x28u + 4u * i));
    return k;
}

int talent_results(TalentResult* results, int max) {
    const std::uint8_t* ew = guest_ew();
    if (!ew || max <= 0) return 0;
    std::uint32_t objs[8];
    if (talent_targets(objs, 8) == 0) return 0;
    constexpr std::uint32_t kBlock = usa(0x02029D80u), kResult = usa(0x02029EC0u);
    // What the talent does to HP, from its record (the block's +0x00, a row
    // of the talent table 0x0861A8F0, stride 64): +0x1E's low nibble is 1
    // for every damage spell (Ice Javelin, Fireball, the breaths' row too)
    // and 2 for the HP heals (Heal, Heal Plus, Full Heal and the healing
    // items: 0x82); status, MP and the rest have other values, and their
    // result's figure is not an HP change (KOL_TALENT_LIST=1 lists them).
    const std::uint32_t talent = rd32(ew, ew_off(kBlock));
    constexpr std::uint32_t kTable = usa(0x0861A8F0u), kStride = 64u;
    int effect = 0;
    if (talent >= kTable && talent < kTable + 127u * kStride && (talent - kTable) % kStride == 0) {
        gba::GbaBus* bus = gbarecomp::active_bus();
        const std::uint8_t* rom = bus ? bus->rom_ptr() : nullptr;
        if (rom && talent + 0x1Fu - 0x08000000u < bus->rom_size())
            effect = rom[talent + 0x1Eu - 0x08000000u] & 0x0F;
    }
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    const std::uint32_t rn = rd32(ew, ew_off(kResult));
    if (rn == 0u || rn > 8u) return 0;
    const int k = std::min<int>(static_cast<int>(rn), max);
    for (int i = 0; i < k; ++i) {
        // {object, flags, amount, 0}: 16 bytes each (IF, Crag Crush on
        // four units).
        const std::uint32_t at = kResult + 4u + 16u * i;
        TalentResult& r = results[i];
        r = TalentResult{};
        r.object = rd32(ew, ew_off(at));
        if (!is_map_object(list, r.object)) { r.object = 0; continue; }
        const std::uint32_t flags = rd32(ew, ew_off(at + 4u));
        r.amount = static_cast<int>(rd32(ew, ew_off(at + 8u)));
        r.heal = effect == 2;
        r.known = (effect == 1 || effect == 2) && (flags & 0x80u) && r.amount > 0 && r.amount < 10000;
    }
    return k;
}

std::uint32_t object_of_record(std::uint32_t record) {
    const std::uint8_t* ew = guest_ew();
    if (!ew || !record) return 0u;
    const std::uint32_t list = rd32(ew, ew_off(kObjectsPtr));
    if (!in_ewram(list, kObjects * kObjStride)) return 0u;
    for (int i = 0; i < kObjects; ++i)
        if (rd32(ew, ew_off(list + i * kObjStride)) == record) return list + i * kObjStride;
    return 0u;
}

namespace {
// A sprite slot's +0x1C / +0x1E are where the game draws it on screen (the
// cursor's are used the same way for the camera, above). Traced over an
// attack (BV): attacker at (136, 116), its target at (120, 100).
bool slot_screen_pos(bool cursor, std::uint32_t owner, int* x, int* y) {
    const std::uint8_t* iw = guest_iw();
    if (!iw || (!cursor && !owner)) return false;
    for (int s = 0; s < kSlots; ++s) {
        const std::uint32_t o = kSlotArray - 0x03000000u + s * kSlotStride;
        if (cursor ? rd32(iw, o) != kCursorSlotFn : rd32(iw, o + 4) != owner) continue;
        *x = static_cast<std::int16_t>(rd16(iw, o + 0x1C));
        *y = static_cast<std::int16_t>(rd16(iw, o + 0x1E));
        return true;
    }
    return false;
}
}  // namespace
bool object_screen_pos(std::uint32_t object, int* x, int* y) {
    return slot_screen_pos(false, object, x, y);
}

bool cursor_screen_pos(int* x, int* y) { return slot_screen_pos(true, 0u, x, y); }
}  // namespace kol
