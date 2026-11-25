// Debug: win the battle at once (Debug > Instant Win).
//
// The game decides a battle's outcome in one of five routines, picked by story
// variable 0xB4 (0x08020E30: 0 -> 0x08020230, 1 -> 0x08020AB4, 2 ->
// 0x0801FEA8, 3 -> 0x0801FF74, 4 -> 0x08020004). Every one of them asks
// 0x0801FBAC whether the enemy side is out: all eight enemy records (battle
// slots 8-15) empty, fallen or gone. Nothing else calls it. So a win is asked
// for by answering that question "yes" (a declining hook otherwise), and the
// routine the battle uses goes on to its own victory: its result code, the
// spoils it rolls, the scene after. See README "Debug: win the battle".
//
// The battle task (0x08060E54, task 0x0202A5B0, state byte +0x27E) runs that
// check only after a unit falls: state 0x14 sets 0x2F (the check, result at
// +0x259), which pops 0x30, which pops 0x31 (acts on it). Before 0x14 the
// game pushes those two on the task's two return stacks, a (sub-state, state)
// pair each: +0x26E[+0x26D] the sub-state (+0x26C), +0x280[+0x27F] the
// state. With the setting on, at the first unit's turn of a battle (state
// 0x00) the task hook pushes the same pairs and sets 0x2F itself, and the
// game's own check, win and victory follow.
//
// Not every battle can be won so. Routine 0 (story battles) declares nothing
// while an enemy carries the flag at +0x1E bit 0x80 (0x0801F95C(1), likely
// the leader): such a battle ends by its own script when that unit falls
// (FR, map 54). And a declared "all enemies out" (code 5) still goes through
// the map's victory script (event 3, 0x0802105C(3, 1) in 0x08020CE4), whose
// result is the battle's. With no result the game ends the unit's action
// (0x31 -> 0x3B): the task is put back to the turn start instead, so no
// unit loses its turn (on FR it had). Forcing the result was tried: the
// story battle faded to black and stopped presenting.
#include "kol_auto_win.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "kol_focus.h"
#include "kol_settings.h"
#include "kol_widescreen.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "kol_region.h"

namespace kol {
namespace {

constexpr std::uint32_t kEnemiesOut = usa(0x0801FBACu);   // 1 when the enemy side is out
constexpr std::uint32_t kBattleTaskFn = usa(0x08060E54u);
constexpr std::uint32_t kBattleTask = usa(0x0202A5B0u);
constexpr std::uint32_t kSub = 0x26Cu, kSubSp = 0x26Du, kSubStack = 0x26Eu;
constexpr std::uint32_t kState = 0x27Eu, kStateSp = 0x27Fu, kStateStack = 0x280u;
constexpr std::uint8_t kTurnStart = 0x00u, kCheck = 0x2Fu, kEndAction = 0x3Bu;
constexpr char kHookId[] = "kol.auto-win";
constexpr char kTaskHookId[] = "kol.auto-win-task";

// The check was started and waits for its answer (the second screen's tag).
std::atomic<bool> g_armed{false};
std::atomic<unsigned long long> g_armed_epoch{0};
// This battle's win was started: once a battle, until the next preparation
// or a state load, so a battle whose outcome routine did not win is not sent
// to the check at every turn.
bool g_done = false;
unsigned long long g_done_epoch = 0;
// The check this hook started is running (until the task leaves 0x31).
bool g_checking = false;

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

bool wanted() { return setting_on(Setting::DebugAutoWin) || env_on("KOL_AUTO_WIN_ANY"); }

int on_enemies_out(std::uint32_t, int, ArmCpuState* cpu) {
    if (!g_armed.load(std::memory_order_acquire)) return 0;
    // A state loaded since the check started puts the battle back as it was.
    if (g_armed_epoch.load(std::memory_order_relaxed) != g_runtime_state_epoch) {
        g_armed.store(false, std::memory_order_relaxed);
        return 0;
    }
    // One answer: the outcome routine that asked declares the win, and the
    // game's victory runs from there (CO: "Congratulations!", the bonus and
    // the war trophy, then the world map).
    g_armed.store(false, std::memory_order_relaxed);
    std::fprintf(stderr, "auto_win: the enemy side reported out (caller %08X)\n", cpu->R[14] & ~1u);
    cpu->R[0] = 1u;
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the outcome routine, as its bl expects
    return 1;
}

int on_battle_task(std::uint32_t, int, ArmCpuState* cpu) {
    if (cpu->R[0] != kBattleTask) return 0;
    if (g_checking) {
        const std::uint8_t state = bus_read_u8(kBattleTask + kState);
        if (state == kCheck || state == 0x30u || state == 0x31u) return 0;
        g_checking = false;
        // No result: the game ends the unit's action. Back to its turn start
        // instead, and the answer is no longer wanted.
        if (state == kEndAction && g_armed.load(std::memory_order_acquire)) {
            g_armed.store(false, std::memory_order_relaxed);
            bus_write_u8(kBattleTask + kState, kTurnStart);
            std::fprintf(stderr, "auto_win: this battle's outcome routine (%u) declared nothing; the turn goes on\n",
                         static_cast<unsigned>(bus_read_u8(usa(0x020012E0u))));
        }
        return 0;
    }
    if (!wanted()) return 0;
    if (g_done && g_done_epoch == g_runtime_state_epoch) return 0;
    if (bus_read_u8(kBattleTask + kState) != kTurnStart) return 0;
    const std::uint8_t sub_sp = bus_read_u8(kBattleTask + kSubSp);
    const std::uint8_t state_sp = bus_read_u8(kBattleTask + kStateSp);
    // Room for two more: the sub-state stack is 16 bytes (+0x26E..+0x27D),
    // the state stack taken the same.
    if (sub_sp + 2u > 0x10u || state_sp + 2u > 0x10u) return 0;
    const std::uint8_t sub = bus_read_u8(kBattleTask + kSub);
    // As 0x0806250C-0x0806256A pushes them: (sub, 0x31), then (sub, 0x30).
    bus_write_u8(kBattleTask + kSubStack + sub_sp, sub);
    bus_write_u8(kBattleTask + kStateStack + state_sp, 0x31u);
    bus_write_u8(kBattleTask + kSubStack + sub_sp + 1u, sub);
    bus_write_u8(kBattleTask + kStateStack + state_sp + 1u, 0x30u);
    bus_write_u8(kBattleTask + kSubSp, static_cast<std::uint8_t>(sub_sp + 2u));
    bus_write_u8(kBattleTask + kStateSp, static_cast<std::uint8_t>(state_sp + 2u));
    bus_write_u8(kBattleTask + kState, kCheck);
    g_done = true;
    g_done_epoch = g_runtime_state_epoch;
    g_checking = true;
    g_armed_epoch.store(g_runtime_state_epoch, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
    std::fprintf(stderr, "auto_win: the battle task sent to its outcome check\n");
    return 0;   // the task runs on, from state 0x2F
}

}  // namespace

bool auto_win_pending() { return g_armed.load(std::memory_order_acquire); }

void auto_win_tick() {
    // A new battle (its preparation) or a state load: the next turn start
    // may start a win again.
    if (g_done && (battle_preparation() || g_done_epoch != g_runtime_state_epoch)) {
        g_done = false;
        g_checking = false;
    }
    if (!g_armed.load(std::memory_order_acquire)) return;
    // Left the battle without a check (a state load): drop the answer.
    if (g_armed_epoch.load(std::memory_order_relaxed) != g_runtime_state_epoch) {
        g_armed.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "auto_win: dropped\n");
    }
}

void auto_win_install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (env_on("KOL_NO_AUTO_WIN")) return;
    if (!gba_mod_register_function_entry_plugin(kHookId, kEnemiesOut, 1, &on_enemies_out) ||
        !gba_mod_set_function_hook_enabled(kHookId, 1))
        std::fprintf(stderr, "auto_win: hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kTaskHookId, kBattleTaskFn, 1, &on_battle_task) ||
        !gba_mod_set_function_hook_enabled(kTaskHookId, 1))
        std::fprintf(stderr, "auto_win: task hook NOT installed\n");
}

}  // namespace kol
