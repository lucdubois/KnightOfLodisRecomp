// kol_fast_actions.cpp — see the header.
#include "kol_fast_actions.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "kol_settings.h"
#include "mod_function_hooks.h"
#include "runtime.h"        // gbarecomp::runtime_excursion_active
#include "runtime_arm.h"    // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus

extern "C" std::uint32_t g_irq_nest_depth;
extern "C" unsigned g_runtime_clock_frozen;
extern "C" unsigned long long g_runtime_clock_frozen_cycles;
extern "C" unsigned long long g_runtime_clock_frozen_cap;
extern "C" unsigned g_runtime_clock_frozen_tripped;
extern "C" void runtime_rw_record(int on, std::uint32_t depth);
extern "C" void runtime_rw_clear(void);
extern "C" int runtime_rw_io_touched(void);

namespace kol {
namespace {

// The routines (README "The pauses on every battle action", measured on HX:
// frames each took, what they read and write). All begin with a push, so
// every entry inside them has a lower stack pointer than their own.
struct Routine {
    std::uint32_t pc;
    const char* name;
};
constexpr Routine kRoutines[] = {
    {0x0804C5D8u, "reach map"},     // 5.6 frames: a 16 KB table from the height grid and the units' places
    {0x08024CA0u, "tile redraw"},   // 2.6: BG0/BG1 mirror rows in a diamond, from the cell table
    {0x0802411Cu, "cell pass A"},   // 3.2: the cell table, little written
    {0x0808CDE8u, "cell pass B"},   // 5.4: the cell table and the grid, 5 KB written
    {0x080867BCu, "cell pass C"},   // 4.0: the cell table and the grid, 4 KB written
    {0x0809B5ACu, "enemy choice"},  // 2.8-3.0 (BL, the enemy choosing): called only for that work. Not its
                                    // callers 0x0809CE80 / 0x0809E71C: those run every frame for 0.06 of one
};
constexpr std::uint32_t kFrameCycles = 280896u;
constexpr unsigned kCapFrames = 60;   // past this the clock runs again (a wait would never end)

void (*g_prev_entry)(std::uint32_t) = nullptr;
bool g_installed = false, g_check = false, g_log = false;
const Routine* g_active = nullptr;
std::uint32_t g_active_sp = 0, g_active_depth = 0;
std::chrono::steady_clock::time_point g_active_t;
unsigned long g_runs = 0, g_io_runs = 0, g_trips = 0;
unsigned long long g_cycles = 0;

bool enabled() {
    return g_installed && setting_on(Setting::FastActions) && !gbarecomp::runtime_excursion_active();
}

void start(const Routine& r) {
    g_active = &r;
    g_active_sp = g_cpu.R[13];
    g_active_depth = g_irq_nest_depth;
    g_runtime_clock_frozen_cycles = 0;
    g_runtime_clock_frozen_cap = static_cast<unsigned long long>(kCapFrames) * kFrameCycles;
    g_runtime_clock_frozen = 1;
    if (g_log || g_check) g_active_t = std::chrono::steady_clock::now();
    if (g_check) {
        runtime_rw_clear();
        runtime_rw_record(1, g_irq_nest_depth);
    }
}

bool g_repeat_hold = false;   // the clock held over a handler's repeats (below)

void end() {
    const bool tripped = g_runtime_clock_frozen == 0;   // the cap let time run again
    g_runtime_clock_frozen = 0;
    const unsigned long long c = g_runtime_clock_frozen_cycles;
    ++g_runs;
    g_cycles += c;
    bool io = false;
    if (g_check) {
        runtime_rw_record(0, 0);
        io = runtime_rw_io_touched() != 0;
        runtime_rw_clear();
        if (io) ++g_io_runs;
    }
    if (tripped) ++g_trips;
    if (g_log || g_check || io || tripped) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_active_t).count();
        std::fprintf(stderr, "fast_actions: %s %08X: %.2f frames (%llu cycles) skipped in %.1f ms%s%s\n", g_active->name,
                     g_active->pc, static_cast<double>(c) / kFrameCycles, c, (g_log || g_check) ? ms : 0.0,
                     io ? " — TOUCHED I/O (not safe to hold the clock over)" : "",
                     tripped ? " — CAP TRIPPED, time ran again" : "");
        if ((g_runs % 50) == 0)
            std::fprintf(stderr, "fast_actions: %lu runs, %.1f frames skipped in all, %lu touched I/O, %lu tripped\n",
                         g_runs, static_cast<double>(g_cycles) / kFrameCycles, g_io_runs, g_trips);
    }
    g_active = nullptr;
}

// ── Fast Walking, Quick Enemy Turns ───────────────────────────────────────
// The battle's phase task (0x0202A5B0) is stepped once a frame by 0x0806B640,
// which switches on its mode byte to one of eight handlers, each called as
// `adds r0, r2, #0; bl handler; b exit` (game.toml has the table). Two of
// them make a turn slow to watch:
// - case 4, 0x0806AA44, the enemy AI: it decides over ~100 frames before each
//   enemy moves, about a tenth of a frame of work each (phase byte 0x02);
// - case 6, 0x08065B20, the action executor: a unit walking (phase 0x15)
//   moves 2 world units a frame, 16 frames a cell, plus a 10-frame stop
//   between straight runs.
// A hook on the resume entry after each bl declines, or, in that phase, sends
// R15 back to the case's own `adds` with r2 = the task: the game itself calls
// the handler again in the same frame. The guest clock is held over the
// repeats (g_runtime_clock_frozen), so the frame keeps its length; the camera,
// the other tasks, the sprites' animation and the sound driver run once a
// frame as before, so the music and the effects keep their speed.
// KOL_FAST_WALK_X / KOL_QUICK_ENEMY_X: calls a frame (2 and 8).
constexpr std::uint32_t kPhaseTask = 0x0202A5B0u;
constexpr std::uint32_t kPhaseByte = 0x0202A82Eu;
struct Repeat {
    std::uint32_t ret, again, handler;
    const char* id;
    Setting setting;
    std::uint8_t phase;
    int times;
    const char* env;
    const char* name;
    bool other_side = false;     // only while the acting unit is not the player's
    std::uint32_t last_r0 = 0;   // the handler's r0 at its last entry
    int done = 0;                // repeats this frame
    unsigned long frames = 0, extra = 0;
};
Repeat g_repeats[] = {
    {0x0806B7A2u, 0x0806B79Cu, 0x0806AA44u, "kol.quick-enemy", Setting::QuickEnemy, 0x02, 8, "KOL_QUICK_ENEMY_X",
     "enemy decides"},
    {0x0806B7B2u, 0x0806B7ACu, 0x08065B20u, "kol.fast-walk", Setting::FastWalk, 0x15, 2, "KOL_FAST_WALK_X", "walk"},
    // The executor again, phase 0x02 with an enemy acting: it shows the
    // enemy's range and moves the cursor to where it goes, waiting ~45
    // frames twice before the walk (BL). Shared with the player's turn, so
    // only for a unit not on the player's side (record +0x5D bit 7: enemies
    // and guests, both moved by the AI).
    {0x0806B7B2u, 0x0806B7ACu, 0x08065B20u, "kol.fast-walk", Setting::QuickEnemy, 0x02, 4, "KOL_QUICK_SHOW_X",
     "enemy shows its move", 0, 0, 0, 0, true},
};

// The acting unit (EWRAM 0x0202A5F0: its map object) is not on the
// player's side.
bool acting_other_side() {
    const std::uint32_t obj = bus_read_u32(0x0202A5F0u);
    if (obj < 0x02000000u || obj >= 0x0203FFF0u) return false;
    const std::uint32_t rec = bus_read_u32(obj);
    if (rec < 0x02000000u || rec >= 0x0203FF98u) return false;
    return (bus_read_u8(rec + 0x5Du) & 0x80u) != 0u;
}

void repeat_release() {
    if (!g_repeat_hold) return;
    g_repeat_hold = false;
    if (!g_active) g_runtime_clock_frozen = 0;
}

int on_repeat(std::uint32_t addr, int, ArmCpuState* cpu) {
    const std::uint8_t phase = bus_read_u8(kPhaseByte);
    Repeat* rp = nullptr;
    for (Repeat& r : g_repeats)
        if (r.ret == addr && (!rp || r.phase == phase)) rp = &r;
    if (!rp) return 0;
    Repeat& r = *rp;
    if (g_active) end();   // a Fast Actions run inside the handler has returned
    std::uint32_t r0 = 0;   // the handler's r0 (both entries of a return point share it)
    for (const Repeat& q : g_repeats)
        if (q.ret == addr && q.last_r0) r0 = q.last_r0;
    const bool want = g_installed && setting_on(r.setting) && !gbarecomp::runtime_excursion_active() &&
                      r0 == kPhaseTask && phase == r.phase && (!r.other_side || acting_other_side());
    for (Repeat& q : g_repeats)
        if (q.ret == addr && &q != &r) q.done = 0;
    if (!want || r.done >= r.times - 1) {
        if (want && g_log && r.done)
            std::fprintf(stderr, "fast_actions: %s x%d (%lu frames, %lu extra calls so far)\n", r.name, r.done + 1,
                         r.frames + 1, r.extra);
        if (r.done) ++r.frames;
        r.done = 0;
        repeat_release();
        return 0;
    }
    ++r.done;
    ++r.extra;
    if (!g_repeat_hold) {
        g_repeat_hold = true;
        g_runtime_clock_frozen_cycles = 0;
        g_runtime_clock_frozen_cap = static_cast<unsigned long long>(kCapFrames) * kFrameCycles;
        g_runtime_clock_frozen = 1;
    }
    cpu->R[2] = kPhaseTask;
    cpu->R[15] = r.again;
    return 1;
}

// ── Fast Attacks ──────────────────────────────────────────────────────────
// An attack's strike (the combat camera's ~300 frames, phase byte 0x10) is
// stepped once a frame by the battle task (0x08060870 -> 0x08088E58), but
// its length is the actors' animations, played elsewhere (the step waits on
// them, 0x0805E318 per actor): repeating the step as Fast Walking does the
// walk left it 300 frames (asked 2026-10-26 for a Fast Attacks
// switch too; the user's pick: the strike's frames at 2x, its sound
// kept). While the step runs, the game runs two frames to a presented one
// (gbarecomp g_runtime_guest_speed), and over every second frame the sound
// hardware stands still (GbaAudio::set_paused) and the MP2K driver's two
// routines are skipped: SoundVSync (0x0815BE18, from the VBlank interrupt:
// the DMA's frame count) and SoundMain (the mix, from the main loop). The
// sound then lives one frame per presented frame: the music and the effects
// keep their tempo and pitch, the HQ/HD render its ticks. (Leaving the
// extra frames' samples out instead, with every tick run, pulsed every
// third frame: which samples came from the driver's stale buffer did not
// line up with the frame; the user, on the Thor, heard the audio go wrong.)
constexpr std::uint32_t kStrikeStep = 0x08088E58u;
constexpr std::uint32_t kSoundVSync = 0x0815BE18u;
bool g_strike_seen = false;   // the strike's step ran since the last SoundVSync
unsigned g_fast_frames = 0;   // frames of a strike or a spell at 2x (their parity)
bool g_fast_spell = false;    // ... of a spell (the log's word)
bool g_sound_skipped = false;   // this frame is an extra one: no sound driver

// ── Fast Spells ───────────────────────────────────────────────────────────
// A spell's animation, the same way (asked 2026-11-02 for the same
// for spells, as a Fast Spells option). It is the phase task's
// mode 0x24 (the phase byte, set by the task the dispatcher 0x0806B640 steps
// once a frame): the camera to the caster, the charge (0x080AC5C8 once a
// frame), the effect (0x080AC608) and the way back. Ice Javelin 509 frames
// (GD), Crag Crush 916 (IF), a heal 591 (IB), the rain spell 506 and its
// storm rolling in 315 more (JQ); an item's use is the same mode (0069, 366).
// Its name box before (mode 0x1D, 128 frames) and the EXP box after (0x3C)
// keep their pace, as an attack's do. The byte is read only on a frame the
// dispatcher stepped the task: off the battle map the EWRAM there is not the
// task's.
constexpr std::uint32_t kPhaseStep = 0x0806B640u;
constexpr std::uint8_t kSpellPhase = 0x24u;
bool g_phase_stepped = false;   // the dispatcher ran since the last SoundVSync

// SoundVSync's entry, at each frame's VBlank: whether the frame starting is
// an extra one (sound paused, the driver skipped) or a played one.
int on_vsync(std::uint32_t, int, ArmCpuState* cpu) {
    const bool ok = g_installed && !gbarecomp::runtime_excursion_active();
    const bool strike = ok && g_strike_seen && setting_on(Setting::FastAttack);
    const bool spell = ok && g_phase_stepped && setting_on(Setting::FastSpells) && bus_read_u8(kPhaseByte) == kSpellPhase;
    g_strike_seen = g_phase_stepped = false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!strike && !spell) {
        if (g_fast_frames) {
            gbarecomp::g_runtime_guest_speed = 1;
            if (bus) bus->audio().set_paused(false);
            if (g_log)
                std::fprintf(stderr, "fast_actions: %s at 2x for %u frames\n", g_fast_spell ? "spell" : "strike",
                             g_fast_frames);
            g_fast_frames = 0;
        }
        g_sound_skipped = false;
        return 0;
    }
    if (!g_fast_frames) g_fast_spell = spell;
    gbarecomp::g_runtime_guest_speed = 2;
    g_sound_skipped = (++g_fast_frames & 1u) == 0u;   // every second frame
    if (bus) bus->audio().set_paused(g_sound_skipped);
    if (!g_sound_skipped) return 0;
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the VBlank handler, as its bl expects
    return 1;
}

// Every function entry (the runtime's hook): the end of a run is the first
// entry at or above the routine's own stack pointer (it has returned; the
// caller's next call, or a return further up); a run starts at a listed
// routine's entry. The clock held, no interrupt can arrive, so an entry at
// another nesting depth means the cap has let time run (ended too).
void on_entry(std::uint32_t pc) {
    if (pc == kStrikeStep) g_strike_seen = true;
    if (pc == kPhaseStep) g_phase_stepped = true;
    for (Repeat& r : g_repeats)
        if (pc == r.handler) r.last_r0 = g_cpu.R[0];
    if (g_active && (g_cpu.R[13] >= g_active_sp || g_irq_nest_depth != g_active_depth || !g_runtime_clock_frozen))
        end();
    if (!g_active && enabled() && !g_runtime_clock_frozen)   // (held already: by a repeat, below)
        for (const Routine& r : kRoutines)
            if (r.pc == pc) { start(r); break; }
    if (g_prev_entry) g_prev_entry(pc);
}

}  // namespace

bool fast_attack_on_sound_main(ArmCpuState* cpu) {
    if (!g_sound_skipped) return false;
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the main loop's wrapper, as its bl expects
    return true;
}

void fast_actions_install() {
    static bool done = false;
    if (done) return;
    done = true;
    auto env_on = [](const char* k) { const char* e = std::getenv(k); return e && e[0] && e[0] != '0'; };
    g_check = env_on("KOL_FAST_ACTIONS_CHECK");
    g_log = env_on("KOL_FAST_ACTIONS_LOG");
    g_prev_entry = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = &on_entry;
    for (Repeat& r : g_repeats) {
        if (const char* v = std::getenv(r.env)) r.times = std::max(1, std::atoi(v));
        bool first = true;   // one hook per return point
        for (const Repeat& q : g_repeats) {
            if (&q == &r) break;
            if (q.ret == r.ret) first = false;
        }
        if (!first) continue;
        if (!gba_mod_register_function_entry_plugin(r.id, r.ret, 1, &on_repeat) ||
            !gba_mod_set_function_hook_enabled(r.id, 1))
            std::fprintf(stderr, "fast_actions: could not hook %08X (%s)\n", r.ret, r.name);
    }
    if (!gba_mod_register_function_entry_plugin("kol.fast-attack", kSoundVSync, 1, &on_vsync) ||
        !gba_mod_set_function_hook_enabled("kol.fast-attack", 1))
        std::fprintf(stderr, "fast_actions: could not hook %08X (SoundVSync)\n", kSoundVSync);
    g_installed = true;
    if (g_check) std::fprintf(stderr, "fast_actions: CHECK mode (each run's reads and writes recorded; I/O reported)\n");
}

}  // namespace kol
