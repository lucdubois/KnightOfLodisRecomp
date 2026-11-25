// kol_vs_bot.cpp — the computer's side of VS Mode before the battle.
//
// With the AI army switch on (dev: KOL_AI_ARMY=1 and F7 or KOL_AI_ARMY_AT,
// kol_states.cpp), in VS Mode this game's preparation plays itself: up to five
// random units from the roster, each on a random free deployment cell, then
// R > Start Game > Yes, then a random one of them appointed leader > Yes.
// It presses keys, as a player would (the host's key filter), and reads the
// game's own state to choose them, so the game's menus and checks do the work
// and nothing is written to guest memory. Found 2026-11-22 on GG (a story
// preparation, the same screens) and a linked pair (README / DEVLOG "The
// computer deploys its army"):
//
//   battle task 0x0202A5B0: phase 0x0202A81C, sub-state 0x0202A82E
//     phase 2 (deployment): 05 the unit list, 09 placing a unit on the map,
//       08 the NAVI box "Choose the direction...", 0C choosing the facing
//     phase 3: 01 the R command menu (VS: Start Game first), 10 "Start
//       battle?", 1B the leader: "Please select a leader." (map cursor off),
//       the cursor (on), then "Appoint X as leader?"; 1C.. the exchange
//   the unit list 0x02037B40: from +0x00 the roster index of each entry
//     (then zeros), +0x2C the cursor's place on the page (two a row, 0-7),
//     +0x2E rows scrolled: list entry = 2 scroll + place (a deployed unit's
//     battle entry +0x00 = 0x80 | roster index). On the last entry, alone
//     in its row, Right goes back one.
//   the map cursor IWRAM 0x03000FA0: +0x35 mode (1 out), +0x3C x, +0x40 z
//     (Right x+1, Left x-1, Down z+1, Up z-1)
//   cells [0x02029CCC] (32 x 32, 4 bytes): +2 flags, 0x40 a deployment cell
//   the Yes/No box 0x020376A0: +0x1F 3 while it takes input, +0x1E the hand
//     (0 Yes, 1 No; it opens on No)
//
// Before the preparation (from "Connecting... Please wait.", linked runs only:
// gbarecomp's SioLink has a peer), found the same way on the pair:
//   "Decide which player attacks 1st.": IWRAM 0x03000EF0 is 2 while it takes
//     input. A takes the hand's choice, which opens on Attack 1st in the
//     parent's game and Attack 2nd in the child's: the two defaults agree.
//   a roulette on the world map picks the battlefield by itself
//   "<town> is the battlefield. 1st attacker sets Winning Condition.": the
//     world map's state pointer 0x02029BD0 is 0x08246D50 and 0x02029BD4 is 1;
//     the hand opens on Defeat Leader, which A takes (when this game attacks
//     first; the other game only shows it)
//   "FIGHT IT OUT!", then the preparation (VS Mode's byte 0x020012E0 is 0
//   until then: it is 4 from the preparation on).
//
// A press that changes nothing is pressed again after the state settles; the
// same press three times on the same target gives the target up (a unit the
// game will not dispatch, a cell it will not take), and three units given up
// end the deployment (the list is full).
//
// Test, dev: KOL_KEY_SCRIPT=<file> (lines "frame,0xKEYS" as gbarecomp's input
// replay) presses those keys up to its last line, so a headless linked pair
// can get to the preparation without GBARECOMP_INPUT_REPLAY (which would
// override this filter). KOL_AI_SEED=n fixes the choices; KOL_AI_BOT_LOG=1
// logs each press.

#include "kol_vs_bot.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

#include "gba_bus.h"
#include "kol_dev.h"
#include "kol_states.h"
#include "kol_vs_cpu.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "sio_link.h"
#include "kol_region.h"

namespace kol {
namespace {

// GBA keys, active high here (KEYINPUT is active low).
constexpr std::uint16_t kA = 1u << 0, kR = 1u << 8;
constexpr std::uint16_t kRight = 1u << 4, kLeft = 1u << 5, kUp = 1u << 6, kDown = 1u << 7;

constexpr int kHold = 4;      // frames a key is held
constexpr int kGap = 4;       // frames released after it
constexpr int kSettle = 6;    // frames the state must stay the same before a press
constexpr int kTries = 3;     // the same press on the same target, then give it up
constexpr int kTeam = 5;      // VS Mode's MEMBER limit

// EWRAM / IWRAM offsets.
constexpr std::uint32_t kMode = 0x12E0u;          // 4: Quest (VS) battles
constexpr std::uint32_t kPrep = 0x2A81Au;         // 0 in the preparation
constexpr std::uint32_t kPhase = 0x2A81Cu;
constexpr std::uint32_t kSub = 0x2A82Eu;
constexpr std::uint32_t kList = 0x37B40u;
constexpr std::uint32_t kEntries = 0x1AA0u;
constexpr std::uint32_t kCellsPtr = 0x29CCCu;
constexpr std::uint32_t kYesNo = 0x376A0u;
constexpr std::uint32_t kCursor = 0x0FA0u;        // IWRAM
constexpr std::uint32_t kAttackMenu = 0x0EF0u;    // IWRAM: 2 while "attacks 1st" takes input
constexpr std::uint32_t kWorldState = 0x29BD0u;   // the world map's state pointer
constexpr std::uint32_t kWinCondition = usa(0x08246D50u);

std::atomic<std::uint16_t> g_out{0};              // keys pressed now (active high)
std::uint16_t (*g_prev_filter)(std::uint16_t) = nullptr;
unsigned long g_frame = 0;

// The key script (tests).
std::vector<std::pair<unsigned long, std::uint16_t>> g_script;
bool g_log = false;

// The bot.
int g_hold_left = 0, g_wait_left = 0, g_settle = 0;
std::uint64_t g_last_state = ~0ull;
std::uint64_t g_last_press_state = ~0ull;
int g_repeats = 0;
std::mt19937 g_rng;
int g_pick = -1;                 // the list entry chosen to dispatch next
std::vector<int> g_refused;      // list entries the game would not dispatch
int g_cell = -1;                 // the deployment cell chosen (x + 32 z)
std::vector<int> g_bad_cells;
int g_leader = -1;               // the leader's cell (x + 32 z)
bool g_leader_picked = false;    // A pressed on the leader
unsigned long g_confirm_frame = 0;   // the last confirming A
unsigned long g_before_frame = 0;    // the last press before the preparation
bool g_active = false;

bool in(const std::vector<int>& v, int x) {
    for (int y : v)
        if (y == x) return true;
    return false;
}

std::uint16_t filter(std::uint16_t keys) {
    if (g_prev_filter) keys = g_prev_filter(keys);
    if (!g_script.empty() && g_frame <= g_script.back().first) {
        std::uint16_t k = 0x03FFu;
        for (const auto& [f, v] : g_script)
            if (f <= g_frame) k = v;
        return k;
    }
    if (g_active) return static_cast<std::uint16_t>(0x03FFu & ~g_out.load(std::memory_order_relaxed));
    return keys;
}

void reset_battle() {
    g_pick = -1;
    g_refused.clear();
    g_cell = -1;
    g_bad_cells.clear();
    g_leader = -1;
    g_leader_picked = false;
    g_repeats = 0;
    g_last_press_state = ~0ull;
}

// g_repeats: presses made from this same state (any key): a press that
// changed nothing.
void press(std::uint16_t k, std::uint64_t state, const char* why) {
    if (state == g_last_press_state) ++g_repeats;
    else g_repeats = 1;
    g_last_press_state = state;
    g_out.store(k, std::memory_order_relaxed);
    g_hold_left = kHold;
    if (g_log) std::fprintf(stderr, "vs-bot: frame %lu press %03X (%s)\n", g_frame, k, why);
}

// One step toward a cell with the D-pad, or 0 when there.
std::uint16_t toward(int cx, int cz, int tx, int tz, bool z_first) {
    const std::uint16_t hx = cx < tx ? kRight : cx > tx ? kLeft : 0;
    const std::uint16_t hz = cz < tz ? kDown : cz > tz ? kUp : 0;
    if (z_first) return hz ? hz : hx;
    return hx ? hx : hz;
}

void decide(const std::uint8_t* ew, const std::uint8_t* iw, std::uint64_t state) {
    auto rd16i = [&](std::uint32_t o) { return static_cast<int>(iw[o] | iw[o + 1] << 8); };
    const std::uint8_t phase = ew[kPhase], sub = ew[kSub];
    const bool cursor_out = iw[kCursor + 0x35] == 1;
    const int cx = rd16i(kCursor + 0x3C), cz = rd16i(kCursor + 0x40);
    const bool yesno = ew[kYesNo + 0x1F] == 3;
    const bool on_no = ew[kYesNo + 0x1E] != 0;

    // This game's own units in the battle entries (+0x00 = 0x80 | roster
    // index, not the other game's, +0x17 bit 5), and their cells.
    std::vector<int> mine, cells;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t* e = ew + kEntries + i * 0x1C;
        if ((e[0] & 0x80u) && e[0] != 0xFFu && !(e[0x17] & 0x20u)) {
            mine.push_back(e[0] & 0x7F);
            cells.push_back(e[0x14] + 32 * e[0x15]);
        }
    }

    if (phase == 2 && sub == 0x05) {   // the unit list
        // The list's order: its first bytes are the roster index of each
        // entry, then zeros (VS on this save: roster 0-15, then 20); the
        // first value seen twice ends it.
        std::vector<int> order;
        for (int i = 0; i < 32; ++i) {
            const int r = ew[kList + i];
            if (in(order, r)) break;
            order.push_back(r);
        }
        const int at = 2 * ew[kList + 0x2E] + ew[kList + 0x2C];
        auto taken = [&](int i) { return in(mine, order[i]) || in(g_refused, i); };
        if (g_pick >= static_cast<int>(order.size()) || (g_pick >= 0 && taken(g_pick))) g_pick = -1;
        if (static_cast<int>(mine.size()) < kTeam && g_refused.size() < 3 && g_pick < 0) {
            std::vector<int> free;
            for (int i = 0; i < static_cast<int>(order.size()); ++i)
                if (!taken(i)) free.push_back(i);
            if (!free.empty()) g_pick = free[std::uniform_int_distribution<int>(0, static_cast<int>(free.size()) - 1)(g_rng)];
        }
        if (g_pick < 0) { press(kR, state, "deployed: the command menu"); return; }
        if (at == g_pick) {
            if (g_repeats >= kTries && g_last_press_state == state) {
                g_refused.push_back(g_pick);
                g_pick = -1;
                g_repeats = 0;
                return;
            }
            press(kA, state, "dispatch this unit");
            return;
        }
        const int row = at / 2, trow = g_pick / 2;
        const std::uint16_t k = trow < row ? kUp : trow > row ? kDown : (g_pick & 1) ? kRight : kLeft;
        press(k, state, "list");
        return;
    }
    if (phase == 2 && sub == 0x09) {   // placing the unit
        if (g_cell >= 0 && (in(cells, g_cell) || in(g_bad_cells, g_cell))) g_cell = -1;
        if (g_cell < 0) {
            std::vector<int> free;
            const std::uint32_t p = ew[kCellsPtr] | ew[kCellsPtr + 1] << 8 | ew[kCellsPtr + 2] << 16 |
                                    static_cast<std::uint32_t>(ew[kCellsPtr + 3]) << 24;
            if (p >= 0x02000000u && p + 4096u <= 0x02040000u)
                for (int c = 0; c < 1024; ++c)
                    if ((ew[p - 0x02000000u + c * 4 + 2] & 0x40u) && !in(cells, c) && !in(g_bad_cells, c)) free.push_back(c);
            if (free.empty()) return;
            g_cell = free[std::uniform_int_distribution<int>(0, static_cast<int>(free.size()) - 1)(g_rng)];
        }
        const int tx = g_cell % 32, tz = g_cell / 32;
        if (g_repeats >= kTries && g_last_press_state == state) {   // stuck: another cell
            g_bad_cells.push_back(g_cell);
            g_cell = -1;
            g_repeats = 0;
            return;
        }
        const std::uint16_t k = toward(cx, cz, tx, tz, g_repeats >= 2);
        press(k ? k : kA, state, k ? "to the cell" : "place");
        return;
    }
    if (phase == 2 && (sub == 0x08 || sub == 0x0C)) {   // the facing box, the facing
        press(kA, state, sub == 0x08 ? "close the box" : "default facing");
        return;
    }
    if (phase == 3 && sub == 0x01) { press(kA, state, "Start Game"); return; }
    if (phase == 3 && sub == 0x10) {   // "Start battle?"
        if (!yesno) return;
        press(on_no ? kUp : kA, state, on_no ? "to Yes" : "start the battle");
        return;
    }
    if (phase == 3 && sub == 0x1B) {   // the leader
        if (yesno && g_leader_picked) {
            press(on_no ? kUp : kA, state, on_no ? "to Yes" : "appoint");
            return;
        }
        if (cursor_out) {
            g_leader_picked = false;
            if (g_leader < 0 || !in(cells, g_leader)) {
                if (cells.empty()) return;
                g_leader = cells[std::uniform_int_distribution<int>(0, static_cast<int>(cells.size()) - 1)(g_rng)];
            }
            const std::uint16_t k = toward(cx, cz, g_leader % 32, g_leader / 32, g_repeats >= 2);
            if (!k) {
                g_leader_picked = true;
                g_confirm_frame = g_frame + 30 - 90;   // the first confirm 30 frames on
            }
            press(k ? k : kA, state, k ? "to the leader" : "pick the leader");
            return;
        }
        // Before the pick: the box "Please select a leader.". After it the
        // unit's cell blinks until A again opens "Appoint X as leader?". The
        // box block keeps the last box's state (1 after "Start battle?"), so
        // the confirming A is pressed once, and again only if 90 frames pass
        // with no box taking input.
        if (!g_leader_picked) {
            press(kA, state, "close the box");
        } else if (g_frame - g_confirm_frame >= 90) {
            g_confirm_frame = g_frame;
            press(kA, state, "confirm the leader");
        }
        return;
    }
}

// The screens before the preparation: only the two menus wait for this game.
void decide_before(const std::uint8_t* ew, const std::uint8_t* iw, std::uint64_t state) {
    const std::uint32_t world = ew[kWorldState] | ew[kWorldState + 1] << 8 | ew[kWorldState + 2] << 16 |
                                static_cast<std::uint32_t>(ew[kWorldState + 3]) << 24;
    const bool attack = iw[kAttackMenu] == 2;
    const bool win = world == kWinCondition && ew[kWorldState + 4] == 1;
    if (!attack && !win) return;
    // A menu that stays (the other game's to answer): once a second.
    if (g_repeats >= 2 && g_last_press_state == state && g_frame - g_before_frame < 60) return;
    g_before_frame = g_frame;
    press(kA, state, attack ? "attack order (the default)" : "Winning Condition (Defeat Leader)");
}

bool linked() {
    gba::SioLink* l = gba::SioLink::instance();
    return l && l->connected();
}

}  // namespace

void vs_bot_install() {
    if (!kol::kDevTools && !vs_cpu_child()) return;   // VS Computer's copy: every build
    static bool done = false;
    if (done) return;
    done = true;
    if (!ai_army_enabled()) return;
    g_log = [] { const char* e = std::getenv("KOL_AI_BOT_LOG"); return e && *e == '1'; }();
    if (const char* e = std::getenv("KOL_AI_SEED"); e && *e) g_rng.seed(static_cast<unsigned>(std::strtoul(e, nullptr, 0)));
    else g_rng.seed(std::random_device{}());
    if (const char* path = std::getenv("KOL_KEY_SCRIPT"); path && *path) {
        if (FILE* f = std::fopen(path, "r")) {
            char line[64];
            while (std::fgets(line, sizeof line, f)) {
                unsigned long fr = 0;
                unsigned k = 0;
                if (line[0] != '#' && std::sscanf(line, "%lu,0x%x", &fr, &k) == 2)
                    g_script.emplace_back(fr, static_cast<std::uint16_t>(k & 0x03FFu));
            }
            std::fclose(f);
        }
        std::fprintf(stderr, "vs-bot: key script %s, %zu lines\n", path, g_script.size());
    }
    g_prev_filter = gbarecomp::g_host_keyinput_filter;
    gbarecomp::g_host_keyinput_filter = &filter;
}

namespace {
void bot_tick(unsigned long frame);
}

void vs_bot_tick(unsigned long frame) {
    bot_tick(frame);
    // The computer's copy runs headless (kol_vs_cpu): no window polls the
    // pad, so the key filter above is never asked and its presses reached
    // nothing. Set here each frame instead.
    if (vs_cpu_child() && ai_army_enabled())
        if (gba::GbaBus* bus = gbarecomp::active_bus()) bus->io().set_keyinput(filter(0x03FFu));
}

namespace {
void bot_tick(unsigned long frame) {
    if (!kol::kDevTools && !vs_cpu_child()) return;   // VS Computer's copy: every build
    if (!ai_army_enabled()) return;
    g_frame = frame;
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    const std::uint8_t* iw = bus ? bus->iwram_ptr() : nullptr;
    if (!ew || !iw) return;
    const bool prep = ew[kMode] == 4 && ew[kPrep] == 0 && (ew[kPhase] == 2 || ew[kPhase] == 3);
    const bool before = !prep && ew[kMode] != 4 && linked();
    const bool on = ai_army_active() && (prep || before) && !(!g_script.empty() && frame <= g_script.back().first);
    if (!on) {
        if (g_active) {
            g_active = false;
            g_out.store(0, std::memory_order_relaxed);
            if (g_log) std::fprintf(stderr, "vs-bot: frame %lu off\n", frame);
        }
        if (!prep) reset_battle();
        return;
    }
    if (!g_active) {
        g_active = true;
        g_hold_left = g_wait_left = g_settle = 0;
        g_last_state = ~0ull;
        if (g_log) std::fprintf(stderr, "vs-bot: frame %lu on\n", frame);
    }
    if (g_hold_left > 0) {
        if (--g_hold_left == 0) {
            g_out.store(0, std::memory_order_relaxed);
            g_wait_left = kGap;
        }
        return;
    }
    if (g_wait_left > 0) { --g_wait_left; return; }
    if (before) {
        const std::uint64_t st = 0xB0000000ull | iw[kAttackMenu] | static_cast<std::uint64_t>(ew[kWorldState]) << 8 |
                                 static_cast<std::uint64_t>(ew[kWorldState + 4]) << 16;
        if (st != g_last_state) {
            g_last_state = st;
            g_settle = 0;
            return;
        }
        if (++g_settle < kSettle) return;
        g_settle = 0;
        decide_before(ew, iw, st);
        return;
    }
    // The state the presses act on: phase, sub-state, the map cursor (mode
    // and cell), the list's cursor and the Yes/No box.
    const std::uint64_t state = static_cast<std::uint64_t>(ew[kPhase]) | static_cast<std::uint64_t>(ew[kSub]) << 8 |
                                static_cast<std::uint64_t>(iw[kCursor + 0x35]) << 16 |
                                static_cast<std::uint64_t>(iw[kCursor + 0x3C]) << 24 |
                                static_cast<std::uint64_t>(iw[kCursor + 0x40]) << 32 |
                                static_cast<std::uint64_t>(ew[kList + 0x2C]) << 40 |
                                static_cast<std::uint64_t>(ew[kList + 0x2E]) << 48 |
                                static_cast<std::uint64_t>(ew[kYesNo + 0x1F] | ew[kYesNo + 0x1E] << 4) << 56;
    if (state != g_last_state) {
        g_last_state = state;
        g_settle = 0;
        return;
    }
    if (++g_settle < kSettle) return;
    g_settle = 0;
    decide(ew, iw, state);
}

}  // namespace

}  // namespace kol
