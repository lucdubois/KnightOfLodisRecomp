// kol_shop_buy.cpp — buy a spell or an item and equip it, from the second screen, by
// the game's own purchase (the same states for both, +0x49F choosing the tables).
// README "Tap a character to buy and equip".
//
// The Buy list's task 0x08132444 runs one frame of its state, block +0x49C
// (block = [0x02029CAC]): 3 the list, 8 the quantity (A), 0x0B the "1 Air Blade
// for 400 Goth?" box, 0x0C "Would you like to see anything else?" opening, 0x0D
// its answer, back to the state +0x49D holds. State 0x0B buys when the box's
// poll 0x0801670C answers (return 0x08133A10) and 0x080197C0 says it was not
// cancelled (return 0x08133A26): funds, the stock table's entry, the list built
// anew (0x08131EF4), with the quantity +0x4BC already 1 at the list.
//
// So, through reviewed hooks (game.toml), as the game skips a box itself:
// - at the task, in state 3: go to 0x0B, no box opened;
// - the box's two questions answered: bought;
// - at the task, in 0x0C: the unit's slot takes the spell, as the game's equip
//   writes it (0x08038AF6), and back to the list, no box opened;
// - at the task's last call 0x081300E0, taking its place for a frame each: the
//   game's recount of what is equipped 0x0813A6B0 (as its equip calls it), then
//   the list built again, so the rows show it.
//
// A copy owned and not equipped goes in with no purchase (the user asked to use the
// one already owned before buying another): the slot, then the same two calls.
//
// KOL_NO_SHOP_BUY=1 leaves the hooks out.

#include "kol_shop_buy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "kol_unit_data.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "kol_region.h"

namespace kol {
namespace {

constexpr std::uint32_t kShopTask = usa(0x08132444u);
constexpr std::uint32_t kShopTail = usa(0x081300E0u);
constexpr std::uint32_t kBoxCancelled = usa(0x080197C0u);
constexpr std::uint32_t kCancelledReturn = usa(0x08133A26u);
constexpr std::uint32_t kRecount = usa(0x0813A6B0u);
constexpr std::uint32_t kBuildList = usa(0x08131EF4u);
constexpr std::uint32_t kBlockPtr = usa(0x02029CACu);
constexpr std::uint32_t kState = 0x49Cu, kReturnState = 0x49Du, kQuantity = 0x4BCu;
constexpr std::uint8_t kStateList = 3, kStateBox = 0x0B, kStateAfter = 0x0C;
constexpr std::uint32_t kRecordSlots = 0x64u, kRecordItems = 0x60u;
constexpr int kGiveUpFrames = 180;
constexpr char kTaskId[] = "kol.shop-buy-task";
constexpr char kTailId[] = "kol.shop-buy-tail";
constexpr char kCancelledId[] = "kol.shop-buy-cancelled";

enum Phase : int { kIdle, kAsked, kBuying, kRecounting, kRebuilding };
std::atomic<int> g_phase{kIdle};
std::atomic<std::uint32_t> g_record{0};
std::atomic<int> g_slot{0}, g_spell{0};
std::atomic<bool> g_is_item{false};
int g_waited = 0;
unsigned long long g_epoch = 0;   // a state load ends it

// As the game's equip writes a slot (spells 0x08038AF6, items 0x080355D8). An item
// shop's two-handed weapon takes off any other weapon: 0x0814165C would refuse it
// with one on, and a slot at 0 is empty.
void equip() {
    const std::uint32_t record = g_record.load(std::memory_order_relaxed);
    const int slot = g_slot.load(std::memory_order_relaxed), id = g_spell.load(std::memory_order_relaxed);
    const bool item = g_is_item.load(std::memory_order_relaxed);
    const std::uint32_t slots = record + (item ? kRecordItems : kRecordSlots);
    if (item && item_category(id) == 2)
        for (int s = 0; s < 4; ++s) {
            const int held = unit_slot_category(record, s);
            if (s != slot && (held == 1 || held == 2)) bus_write_u8(slots + static_cast<std::uint32_t>(s), 0);
        }
    bus_write_u8(slots + static_cast<std::uint32_t>(slot), static_cast<std::uint8_t>(id));
}

int on_task(std::uint32_t, int, ArmCpuState*) {
    if (g_epoch != g_runtime_state_epoch) {
        g_epoch = g_runtime_state_epoch;
        g_phase.store(kIdle, std::memory_order_relaxed);
    }
    const int phase = g_phase.load(std::memory_order_relaxed);
    if (phase != kAsked && phase != kBuying) return 0;
    const std::uint32_t block = bus_read_u32(kBlockPtr);
    if (block < 0x02000000u || block >= 0x0203FB00u) {
        g_phase.store(kIdle, std::memory_order_relaxed);
        return 0;
    }
    const std::uint8_t state = bus_read_u8(block + kState);
    if (phase == kAsked) {
        if (state != kStateList) {   // the player is in a box of the game's: wait, a while
            if (++g_waited > kGiveUpFrames) g_phase.store(kIdle, std::memory_order_relaxed);
            return 0;
        }
        const bool item = g_is_item.load(std::memory_order_relaxed);
        if ((item ? shop_cursor_item() : shop_cursor_spell()) != g_spell.load(std::memory_order_relaxed)) {
            g_phase.store(kIdle, std::memory_order_relaxed);   // the cursor moved on
            return 0;
        }
        if (shop_spell_free()) {   // no purchase: a copy owned goes in the slot
            equip();
            g_phase.store(kRecounting, std::memory_order_relaxed);
            return 0;
        }
        bus_write_u8(block + kQuantity, 1);
        bus_write_u8(block + kState, kStateBox);
        g_phase.store(kBuying, std::memory_order_relaxed);
        return 0;
    }
    if (state == kStateAfter) {
        equip();
        bus_write_u8(block + kState, bus_read_u8(block + kReturnState));
        g_phase.store(kRecounting, std::memory_order_relaxed);
    } else if (state != kStateBox) {   // not the way it goes: leave it to the game
        g_phase.store(kIdle, std::memory_order_relaxed);
    }
    return 0;
}

int on_tail(std::uint32_t, int, ArmCpuState* cpu) {
    const int phase = g_phase.load(std::memory_order_relaxed);
    if (phase == kRecounting) {
        g_phase.store(kRebuilding, std::memory_order_relaxed);
        cpu->R[15] = kRecount;   // R14 stays: it returns to the task, whose next line sets its own R0
        return 1;
    }
    if (phase == kRebuilding) {
        g_phase.store(kIdle, std::memory_order_relaxed);
        cpu->R[1] = 0;           // R0 is the task's own argument, as at 0x08133DE4
        cpu->R[15] = kBuildList;
        return 1;
    }
    return 0;
}

int on_cancelled(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t ret = cpu->R[14] & ~1u;
    if (ret != kCancelledReturn || !shop_buy_answering()) return 0;
    cpu->R[0] = 0;
    cpu->R[15] = ret;
    return 1;
}

}  // namespace

bool shop_buy_answering() { return g_phase.load(std::memory_order_relaxed) == kBuying; }

void shop_buy_request(std::uint32_t record, int slot, int id, bool is_item) {
    if (g_phase.load(std::memory_order_relaxed) != kIdle) return;
    g_record.store(record, std::memory_order_relaxed);
    g_slot.store(slot, std::memory_order_relaxed);
    g_spell.store(id, std::memory_order_relaxed);
    g_is_item.store(is_item, std::memory_order_relaxed);
    g_waited = 0;
    g_phase.store(kAsked, std::memory_order_release);
}

// The world map's song (10, the shops play it too) has one stuck note, in the cartridge:
// its track 4 ends TIE 58, its waits, GOTO 0x0823B231, and the EOT 58 after that GOTO
// (0x0823B284) is never reached, so each time round leaves one more D held, up to all the
// voices (the only such track of the 373 in the ROM's songs that loop with a tie held,
// but for two one-note effects, songs 204 and 288). The driver's GOTO 0x0815BC94, r1 the
// track, its position +0x40 just past the opcode: at this one, every held key 58 on the
// track's channels (+0x20, next +0x34) ends as ply_endtie ends one, status | 0x40 where
// status & 0x83 and not yet 0x40, midiKey +0x11. All of them: a save's piled-up copies
// go too. 6 ticks before the lost EOT would have. KOL_KEEP_SONG_TIE=1 keeps the game's.
int on_song_goto(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t track = cpu->R[1];
    if (bus_read_u32(track + 0x40u) != usa(0x0823B27Fu)) return 0;
    std::uint32_t chan = bus_read_u32(track + 0x20u);
    for (int guard = 0; chan && guard < 32; ++guard) {
        const std::uint8_t status = bus_read_u8(chan);
        if ((status & 0x83u) && !(status & 0x40u) && bus_read_u8(chan + 0x11u) == 58u)
            bus_write_u8(chan, static_cast<std::uint8_t>(status | 0x40u));
        chan = bus_read_u32(chan + 0x34u);
    }
    return 0;
}

void song_tie_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_KEEP_SONG_TIE");
    if (off && off[0] && off[0] != '0') return;
    if (!gba_mod_register_function_entry_plugin("kol.song-tie", usa(0x0815BC94u), 1, &on_song_goto) ||
        !gba_mod_set_function_hook_enabled("kol.song-tie", 1))
        std::fprintf(stderr, "song tie: hook NOT installed\n");
}

void shop_buy_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_SHOP_BUY");
    if (off && off[0] && off[0] != '0') return;
    if (!gba_mod_register_function_entry_plugin(kTaskId, kShopTask, 1, &on_task) ||
        !gba_mod_set_function_hook_enabled(kTaskId, 1) ||
        !gba_mod_register_function_entry_plugin(kTailId, kShopTail, 1, &on_tail) ||
        !gba_mod_set_function_hook_enabled(kTailId, 1) ||
        !gba_mod_register_function_entry_plugin(kCancelledId, kBoxCancelled, 1, &on_cancelled) ||
        !gba_mod_set_function_hook_enabled(kCancelledId, 1))
        std::fprintf(stderr, "shop buy: hooks NOT installed\n");
}

}  // namespace kol
