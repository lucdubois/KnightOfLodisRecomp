#include "kol_rumble.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "kol_env.h"
#include "kol_focus.h"
#include "kol_settings.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "kol_region.h"

namespace kol {
namespace {

// ── The effects ──────────────────────────────────────────────────────────
// The thunder: a crack, then a roll that swells once and dies away (1.2 s,
// about the length of the sound's loud part).
constexpr RumbleStep kThunder[] = {
    {30, 1.00f}, {50, 0.50f}, {80, 0.85f}, {120, 0.60f},
    {150, 0.70f}, {200, 0.45f}, {250, 0.30f}, {300, 0.15f},
};
// The menu's preview of a strength.
constexpr RumbleStep kTest[] = {{120, 1.0f}};

// The game's sound effects that rumble, by MP2K song number (the table at
// 0x08167BB8; m4aSongNumStart's r0). Found by watching the table's reads
// (GBARECOMP_READ_WATCH over it) while the effect played:
// 205 the thunder (player 3; states/HW_slot3_cutscene_thunder, frames 17895
// and 18447, each with its flash).
struct SoundRumble {
    std::uint16_t song;
    const RumbleStep* steps;
    int n;
    const char* name;
};
constexpr SoundRumble kSoundRumbles[] = {
    {205, kThunder, static_cast<int>(sizeof kThunder / sizeof kThunder[0]), "thunder"},
};

// ── Shaher's death (map 57, story scene 179) ─────────────────────────────
// After the last blow of the final battle (states/final_battle_won_route_b.state,
// frames counted from it, A every 60): three white flashes, each with sound
// 418 (232, 314, 337); his rays, BG2/BG3 blended over the picture (BLDCNT
// 0x334C, EVA 0 -> 16 -> 0, 464-664); the demon (BG1 art) cross-fading away
// (BLDCNT 0x3142, EVA 16 -> 0, 959-1066); the screen brightened to white
// (BLDCNT 0x00BF, BLDY 0 -> 16, 1079-1136), held, then back from white on
// Shaher as a man. Asked 2026-11-12 for rumble through the death sequence
// wherever it fits. The flashes jolt; the rays, the fading and the
// brightening rumble with the game's own weights; full white ends in one
// burst; the way back from white is still. KOL_NO_DEATH_RUMBLE=1 off.
constexpr int kDeathMap = 57;
constexpr std::uint16_t kDeathScene = 179;
constexpr std::uint16_t kDeathFlashSong = 418;
constexpr RumbleStep kDeathFlash[] = {{40, 1.00f}, {60, 0.55f}, {100, 0.30f}, {80, 0.12f}};
constexpr RumbleStep kDeathWhite[] = {{250, 1.00f}, {200, 0.60f}, {250, 0.30f}, {300, 0.12f}};

bool death_scene() {
    static const bool off = std::getenv("KOL_NO_DEATH_RUMBLE") != nullptr;
    std::uint32_t size = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&size);
    // The map shown, the scene number (u16) and the story-event flag, as
    // EWRAM offsets through kol_region.h.
    constexpr std::uint32_t kEwMap = usa(0x02029D40u) - 0x02000000u;
    constexpr std::uint32_t kEwScene = usa(0x020013BEu) - 0x02000000u;
    constexpr std::uint32_t kEwStory = usa(0x020013D0u) - 0x02000000u;
    if (off || !ew || size < kEwMap + 2u) return false;
    const unsigned map = ew[kEwMap] | (ew[kEwMap + 1] << 8);
    const unsigned scene = ew[kEwScene] | (ew[kEwScene + 1] << 8);
    return map == kDeathMap && scene == kDeathScene && ew[kEwStory] == 1u;
}

constexpr std::uint32_t kSongStart = usa(0x0815C4E8u);   // m4aSongNumStart(song)
constexpr char kSongStartId[] = "kol.rumble-song-start";

RumbleBackend g_backend = nullptr;

bool log_on() {
    static const bool on = [] {
        const char* e = std::getenv("KOL_RUMBLE_LOG");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

// The controller's effect, stepped by rumble_frame (no backend).
std::mutex g_mu;
std::vector<RumbleStep> g_steps;
std::chrono::steady_clock::time_point g_start;
bool g_playing = false;
float g_sent = -1.0f;

// m4aSongNumStart: the effect a sound starts. Declines every call.
// Each song's starts (song_starts). KOL_SONG_LOG=1 prints every start.
unsigned g_song_starts[1024];

int on_song_start(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint16_t song = static_cast<std::uint16_t>(cpu->R[0]);
    if (song < 1024) ++g_song_starts[song];
    static const bool song_log = std::getenv("KOL_SONG_LOG") != nullptr;
    if (song_log)   // (with the guest frame of the last present, to place it in a scene)
        std::fprintf(stderr, "song: %u frame %llu\n", song,
                     static_cast<unsigned long long>(gbarecomp::g_runtime_presented_frame));
    for (const SoundRumble& r : kSoundRumbles)
        if (r.song == song) rumble_play(r.steps, r.n, r.name);
    if (song == kDeathFlashSong && death_scene())
        rumble_play(kDeathFlash, static_cast<int>(sizeof kDeathFlash / sizeof kDeathFlash[0]), "shaher flash");
    return 0;
}

// The death's steady parts, a guest frame at a time from its blend registers
// (`io` the frame's I/O). A level is sent as a short step, again when it moves
// or every ~100 ms: the device's vibrator plays the newest effect only.
void death_frame(const std::uint8_t* io) {
    static bool white_done = false;
    static int last_bldy = 0;
    static int dissolve = 0;
    static float sent = 0.0f;
    static auto sent_at = std::chrono::steady_clock::now();
    if (!io || !death_scene()) {
        white_done = false;
        last_bldy = 0;
        dissolve = 0;
        sent = 0.0f;
        return;
    }
    const unsigned bldcnt = io[0x50] | (io[0x51] << 8);
    const int eva = std::min(16, io[0x52] & 31), bldy = std::min(16, io[0x54] & 31);
    const unsigned mode = (bldcnt >> 6) & 3u;
    float level = 0.0f;
    if (mode == 1u && (bldcnt & 0x3Fu) == 0x0Cu && eva > 0) {
        level = 0.15f + 0.35f * eva / 16.0f;               // the rays
    } else if (mode == 1u && (bldcnt & 0x3Fu) == 0x02u) {
        // The demon fading: its weights are written partway through each
        // frame (BLDALPHA 0x1000 at the frame's start, its shadow at
        // 0x03000E1C too; 0x10 -> 0x1000 by the frame's end), so by time
        // instead: the fade is ~107 frames.
        level = 0.30f + 0.40f * std::min(1.0f, static_cast<float>(++dissolve) / 107.0f);
    } else if (mode == 2u && (bldcnt & 0x3Fu) == 0x3Fu && !white_done) {
        if (bldy >= 16 && last_bldy < 16) {                // full white: the burst
            white_done = true;
            rumble_play(kDeathWhite, static_cast<int>(sizeof kDeathWhite / sizeof kDeathWhite[0]), "shaher white");
            sent = 0.0f;
        } else if (bldy > 0 && bldy >= last_bldy) {
            level = 0.70f + 0.30f * bldy / 16.0f;          // brightening (on from the fading's 0.70)
        }
    }
    last_bldy = bldy;
    const auto now = std::chrono::steady_clock::now();
    const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_at).count() >= 100;
    if (level > 0.0f && (std::abs(level - sent) >= 0.05f || due)) {
        const RumbleStep step[] = {{140, level}};
        rumble_play(step, 1, "shaher death");
        sent = level;
        sent_at = now;
    } else if (level <= 0.0f) {
        sent = 0.0f;
    }
}

// ── An attack landing ────────────────────────────────────────────────────
// Asked 2026-11-13 for rumble when an attack lands. Every blow
// that lands brings up a damage number over the unit it hit (0x0202AFCC, the
// number's sprite block; kol_focus damage_number_object), whatever the weapon
// or the spell, while the sounds differ (BV's sword 410 + 376, IE's arrow
// 194). A number is also a heal's, so a new number rumbles only for damage:
//   - an attack's: the game writes the hit unit's HP when the action starts,
//     about 2 s before its number (BV: 40240 the blow's sounds, the number a
//     few frames after; README "The HP waits for the blow"); each battle
//     record's HP dropping (0x02001420 on, stride 0x68, +0x40) is a blow
//     waiting for its number: the number over that unit (its map object's
//     record, +0x00), not the next number whatever its unit. Thor state 0260:
//     Rictor's attack on a 999-HP unit took nothing and the counter took his
//     HP; matched by order, the attack's 0 took the counter's drop and the
//     counter's number found none (the user, 2026-11-13, found the counter attack
//     did not rumble). A number over a unit that lost nothing (a 0, a block)
//     does not rumble;
//   - a talent's: its HP is written after its numbers, so by the talent's
//     result list (kol_focus talent_results: the unit's entry, not a heal).
// Drops within kAfterNumber frames after a number has gone are the talents'
// late writes, already rumbled (IB 870 after its number's 749-833, ID 691 after
// 563-652). Not while a number is up: a counterattack's HP is written then, the
// attack's number on screen (Thor state 0260, saved at the attack's number with
// the counter's loss already in: the counter's number never rumbled; the user,
// 2026-11-13, reported it). A drop no number takes in
// kDropLife frames is forgotten. A blow that takes the unit to 0 HP is the
// heavier kKill. KOL_NO_HIT_RUMBLE=1 off.
// Then (2026-11-13), with every rumble starting 40 ms late (rumble_play), asked for about 20 ms
// more on hits and on everything else in battle: the
// first part still felt after the 40 ms is 20 ms longer (the hit's 0.45, the kill's 1.0), so
// each ends 20 ms later.
constexpr RumbleStep kHit[] = {{40, 0.90f}, {80, 0.45f}, {70, 0.15f}};
constexpr RumbleStep kKill[] = {{80, 1.00f}, {90, 0.60f}, {120, 0.30f}, {120, 0.10f}};
constexpr std::uint32_t kBattleRecs = 0x1420u, kRecStride = 0x68u;
constexpr int kBattleUnits = 16, kAfterNumber = 90, kDropLife = 600;

void hit_frame() {
    static const bool off = std::getenv("KOL_NO_HIT_RUMBLE") != nullptr;
    if (off) return;
    std::uint32_t size = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&size);
    if (!ew || size < kBattleRecs + kBattleUnits * kRecStride) return;
    static int hp_was[kBattleUnits];
    static bool have = false;
    static int since_number = kAfterNumber;
    static int pend_age[kBattleUnits];   // frames a unit's drop has waited for its number, -1 none
    static bool pend_kill[kBattleUnits];
    if (!have) std::fill(std::begin(pend_age), std::end(pend_age), -1);
    static std::uint32_t number_was = 0, unit_was = 0;
    const std::uint32_t number = damage_number_object();
    const std::uint32_t unit = number ? damage_number_unit() : 0u;
    if (number) since_number = 0;
    else if (since_number < kAfterNumber) ++since_number;
    static const bool late_while_up = std::getenv("KOL_HIT_LATE_WHILE_UP") != nullptr;   // (as before)
    const bool late = (late_while_up && number != 0u) || (number == 0u && since_number < kAfterNumber);
    for (int i = 0; i < kBattleUnits; ++i) {
        const std::uint8_t* r = ew + kBattleRecs + static_cast<std::uint32_t>(i) * kRecStride;
        const int hp = r[0x40] | (r[0x41] << 8), max = r[0x42] | (r[0x43] << 8);
        if (max <= 0 || hp > max) { hp_was[i] = -1; continue; }
        if (have && hp_was[i] >= 0 && hp < hp_was[i] && !late) {
            pend_age[i] = 0;
            pend_kill[i] = hp == 0;
        } else if (pend_age[i] >= 0 && ++pend_age[i] > kDropLife) {
            pend_age[i] = -1;
        }
        hp_was[i] = hp;
    }
    have = true;
    const bool new_number = number != 0u && (number != number_was || unit != unit_was);
    number_was = number;
    unit_was = unit;
    if (!new_number) return;
    bool damage = false, kill = false;
    // The number's unit's record (its map object's +0x00), as a slot.
    int slot = -1;
    if (unit >= 0x02000000u && unit + 4u <= 0x02000000u + size) {
        const std::uint8_t* o = ew + (unit - 0x02000000u);
        const std::uint32_t rec = o[0] | (o[1] << 8) | (o[2] << 16) | (static_cast<std::uint32_t>(o[3]) << 24);
        const std::uint32_t off = rec - 0x02000000u - kBattleRecs;
        if (rec >= 0x02000000u + kBattleRecs && off % kRecStride == 0u && off / kRecStride < static_cast<std::uint32_t>(kBattleUnits))
            slot = static_cast<int>(off / kRecStride);
    }
    if (slot < 0)   // (no unit known there: the drop that has waited longest)
        for (int i = 0; i < kBattleUnits; ++i)
            if (pend_age[i] >= 0 && (slot < 0 || pend_age[i] > pend_age[slot])) slot = i;
    if (log_on())
        std::fprintf(stderr, "rumble: number over slot %d (its drop %s)\n", slot,
                     slot >= 0 && pend_age[slot] >= 0 ? "waiting" : "none");
    if (slot >= 0 && pend_age[slot] >= 0) {
        damage = true;
        kill = pend_kill[slot];
        pend_age[slot] = -1;
    } else {
        TalentResult res[8];
        const int n = talent_results(res, 8);
        for (int k = 0; k < n && !damage; ++k)
            damage = res[k].known && res[k].object == unit && !res[k].heal && res[k].amount > 0;
    }
    if (!damage) return;
    if (kill) rumble_play(kKill, static_cast<int>(sizeof kKill / sizeof kKill[0]), "kill");
    else rumble_play(kHit, static_cast<int>(sizeof kHit / sizeof kHit[0]), "hit");
}

}  // namespace

void rumble_set_backend(RumbleBackend backend) { g_backend = backend; }

void rumble_scene_frame(const std::uint8_t* io) {
    death_frame(io);
    hit_frame();
}

unsigned song_starts(int song) { return song >= 0 && song < 1024 ? g_song_starts[song] : 0u; }

// Every rumble starts kDelay ms late and ends when it would have (the user,
// 2026-11-13, asked for every rumble to start 80 ms late and end on time,
// then for 40 ms instead):
// the waveform's first kDelay ms are stillness.
// Only from stillness: an effect sent while another still plays (Shaher's
// death refreshes a 140 ms step every ~100 ms) carries on at once, or the
// steady rumble would come apart into pulses. An effect no longer than the
// delay is not felt. KOL_RUMBLE_DELAY=<ms> (0: none).
std::vector<RumbleStep> delayed(const RumbleStep* steps, int n) {
    static const int kDelay = static_cast<int>(kol::env_num_min("KOL_RUMBLE_DELAY", 0.0f, 40.0f));
    static auto busy_until = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    std::vector<RumbleStep> out(steps, steps + n);
    long total = 0;
    for (const RumbleStep& s : out) total += s.ms;
    if (kDelay > 0 && now >= busy_until) {
        std::vector<RumbleStep> cut;
        cut.push_back({static_cast<std::uint16_t>(kDelay), 0.0f});
        int skip = kDelay;
        for (const RumbleStep& s : out) {
            if (skip >= s.ms) { skip -= s.ms; continue; }
            cut.push_back({static_cast<std::uint16_t>(s.ms - skip), s.level});
            skip = 0;
        }
        out.swap(cut);
    }
    busy_until = now + std::chrono::milliseconds(total);
    return out;
}

void rumble_play(const RumbleStep* steps_in, int n_in, const char* why) {
    if (!steps_in || n_in <= 0) return;
    const std::vector<RumbleStep> late = delayed(steps_in, n_in);
    const RumbleStep* steps = late.data();
    const int n = static_cast<int>(late.size());
    const float strength = rumble_strength();
    if (log_on()) {
        std::fprintf(stderr, "rumble: %s x%.2f (%s) level %.2f frame %llu\n", why ? why : "?", strength,
                     strength <= 0.0f ? "off" : g_backend ? "device" : "controller",
                     steps_in[0].level,
                     static_cast<unsigned long long>(gbarecomp::g_runtime_presented_frame));
        std::fflush(stderr);
    }
    if (strength <= 0.0f || !steps || n <= 0) return;
    if (g_backend) {
        std::vector<int> ms(static_cast<std::size_t>(n));
        std::vector<float> level(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            ms[static_cast<std::size_t>(i)] = steps[i].ms;
            level[static_cast<std::size_t>(i)] = std::clamp(steps[i].level * strength, 0.0f, 1.0f);
        }
        g_backend(ms.data(), level.data(), n);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_steps.assign(steps, steps + n);
    for (RumbleStep& s : g_steps) s.level = std::clamp(s.level * strength, 0.0f, 1.0f);
    g_start = std::chrono::steady_clock::now();
    g_playing = true;
    g_sent = -1.0f;
}

void rumble_test() { rumble_play(kTest, 1, "test"); }

void rumble_frame() {
    if (g_backend) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_playing) return;
    // Where in the waveform the wall clock is (not guest frames: fast-forward
    // and holds would stretch it).
    const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - g_start).count();
    long at = 0;
    float level = -1.0f;
    long left = 0;
    for (const RumbleStep& s : g_steps) {
        if (t < at + s.ms) { level = s.level; left = at + s.ms - t; break; }
        at += s.ms;
    }
    if (level < 0.0f) {   // over
        gbarecomp::gbarecomp_host_rumble(0.0f, 0.0f, 0);
        g_playing = false;
        return;
    }
    // The low motor carries it, the high one a little less (the crack).
    if (level != g_sent) {
        gbarecomp::gbarecomp_host_rumble(level, level * 0.6f, static_cast<unsigned>(left + 50));
        g_sent = level;
    }
}

void rumble_install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!gba_mod_register_function_entry_plugin(kSongStartId, kSongStart, 1, &on_song_start) ||
        !gba_mod_set_function_hook_enabled(kSongStartId, 1))
        std::fprintf(stderr, "rumble: could not hook m4aSongNumStart %08X\n", kSongStart);
}

}  // namespace kol
