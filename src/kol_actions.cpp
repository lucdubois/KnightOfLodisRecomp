// kol_actions.cpp — see the header.
//
// Next / previous unit, on the battle map. The game's hold-L cycle is a
// per-frame state machine at 0x080745A4 (state byte ctl+0x73E; 0 = ordinary
// cursor control). Holding L enters its own mode, and each A (B) there calls
// 0x08074338(0) (with 1), which picks the next (previous) unit after the
// index byte ctl+0x73F (0xFF = none yet) whose map object is live and can
// act, stores the new index, and sends the cursor to it.
// See README "Next unit, from a button".
//
// The button enters that selection directly: hooked at the state machine's
// entry, when a request is pending and the machine is in its ordinary state,
// the hook writes OUR index into ctl+0x73F and tail-calls 0x08074338 in
// place of that frame's handler (R14 still holds the handler's return). The
// index the game stores back is read into g_index on the next call, so the
// cycle survives the game's own L mode resetting that byte. Two seconds
// without a tap starts the cycle over from the first available unit.
//
// The unit buttons (the north and west face buttons) are the game's
// Shortcut A and B there: the options the
// OPTION window sets (EWRAM 0x02001330 and 0x02001331: Next, Reverse,
// Element, Wounded, Level Up), which the game's hold-L mode runs on A and B.
// Holding A in that mode (state 5) calls 0x08074338(Shortcut A) every frame
// and, on release, restores each object's +0x65 from ctl+0x740 and calls
// 0x0806D970(2). Next and Reverse step the cursor once per press, as above.
// The other three recolour the matching units (+0x65 = 0x0C-0x0F), blinking
// through the routine's own counter ctl+0x75A while the button is held. The
// hook does the same: the selection each frame while held, then the game's
// release steps.
//
// A unit picked on the second screen's list: the same hook, in the same
// ordinary state, sends the cursor to that unit's cell the way the game's
// selections do (0x08074338 at 0x080744BA): the cell (world >> 5, the game's
// 0x08058D5C) into cursor +0x3C/+0x40 and 4, the glide, into +0x35. Any unit,
// the enemy's and the done ones too, as the game's own list (SEARCH) allows.
//
// Next / previous unit, in the status window. The game has no such thing:
// the window shows one unit until it is closed. The army roster and the
// battle map open the same window, and in both it is drawn by the roster
// task 0x08034498 (menu task table 0x0860D77C), whose state byte (its r1) is
// 3 while the window is up; the control block ([0x02002B1C]) names the unit
// at +0x5854 and, in battle, its battle entry at +0x5860, with the sub-state
// +0x61A4 at 1 while the window is idle. Hooked at the task's entry, a
// pending request there moves those to the neighbouring unit of the same
// army or side and sets the state to 0, and the task's own init draws the new
// unit. In battle the picture is a sprite object the init does not touch, so
// on the next idle call the hook tail-calls the game's 0x0803A964, which
// re-points it at the new unit. KOL_NO_STATUS_SWITCH=1 turns this off.
// See README "Next unit, inside the status window".

#include "kol_desktop.h"
#include "kol_shop_buy.h"
#include "kol_actions.h"
#include "kol_menu.h"
#include "kol_auto_win.h"
#include "kol_settings.h"
#include "kol_tile_edit.h"
#include "kol_unit_data.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"

namespace gba {
extern "C" unsigned g_ppu_debug_hide_layers;   // gba_ppu.h
}

namespace kol {
namespace {

// ── The battle map's cycle ──────────────────────────────────────────────
constexpr std::uint32_t kHandler = 0x080745A4u;   // hold-L state machine
constexpr std::uint32_t kSelect = 0x08074338u;    // select(cmd): 0 next, 1 previous
constexpr std::uint32_t kCtlPtr = 0x0202AB8Cu;    // u32 -> battle control
constexpr std::uint32_t kFlags = 0x0202A900u;     // u32 battle flags
constexpr std::uint32_t kObjPtr = 0x0202A238u;    // u32 -> map objects
constexpr std::uint32_t kObjStride = 0xD0u;
constexpr int kObjCount = 26;
constexpr std::uint32_t kCtlState = 0x73Eu;
constexpr std::uint32_t kCtlIndex = 0x73Fu;
constexpr std::uint32_t kCtlSaved = 0x740u;       // 26 saved obj+0x65 bytes
constexpr std::uint32_t kCtlBlink = 0x75Au;       // u16
constexpr std::uint32_t kCtlFresh = 0x75Cu;
constexpr std::uint32_t kCursorMode = 0x03000FD5u;
constexpr std::uint32_t kCursorLock = 0x03000FD6u;  // 0xFF while selecting
constexpr std::uint32_t kCursorCellX = 0x03000FDCu;  // cursor (0x03000FA0) +0x3C
constexpr std::uint32_t kCursorCellY = 0x03000FE0u;  // +0x40
constexpr std::uint8_t kCursorFree = 1, kCursorGlide = 4;   // +0x35
constexpr char kHookId[] = "kol.next-unit";
constexpr std::uint32_t kShortcutA = 0x02001330u;   // option 8: 0 Next .. 4 Level Up
constexpr std::uint32_t kShortcutB = 0x02001331u;   // option 9
constexpr int kShortcutCommands = 5;
constexpr std::uint32_t kShortcutEnd = 0x0806D970u;  // (2) after the restore, as the game

// ── The status window's ─────────────────────────────────────────────────
constexpr std::uint32_t kStatusTask = 0x0803A0D0u;
constexpr std::uint32_t kWinPtr = 0x02002B1Cu;    // u32 -> window control
constexpr std::uint32_t kWinUnit = 0x5854u;       // u32 unit record
constexpr std::uint32_t kWinEntry = 0x5860u;      // u32 battle entry, or 0
constexpr std::uint8_t kTaskBuild = 0;
constexpr std::uint32_t kRecSize = 0x68u;
// The army: 32 record slots from 0x02000030, an empty one has class 0 — the
// roster grid lists them in this order and skips those.
constexpr std::uint32_t kArmy = 0x02000030u;
constexpr std::uint32_t kArmySlots = 32u;
// A battle: two sides of 8 records from 0x02001420, each with a 0x1C-byte
// entry from 0x02001AA0; a unit is on the map while its state byte +0x4E and
// its HP are nonzero (as the support gauge reads it).
constexpr std::uint32_t kBattle = 0x02001420u;
constexpr std::uint32_t kBattleEntries = 0x02001AA0u;
constexpr std::uint32_t kEntrySize = 0x1Cu;
constexpr std::uint32_t kSideSlots = 8u;
constexpr char kStatusHookId[] = "kol.status-unit";
constexpr std::uint32_t kWinSub = 0x61A4u;        // u8 sub-state: 1 = up
constexpr std::uint32_t kWinPage = 0x61D6u;       // u8 lower page: 0 EQUIP., 1 EMBLEM
constexpr std::uint32_t kWinPageSlide = 0x5838u;  // u16, 0 when the window opens
// The army roster's task, which opens the status window from the grid and
// draws its contents; its state byte is its r1 too.
constexpr std::uint32_t kRosterTask = 0x08034498u;
constexpr std::uint8_t kRosterWindowUp = 3;
// The battle window's picture is a sprite object, not drawn by the task's
// init: 0x0803A964(i) re-points window sprite i (index at ctl+0x5ED2) at the
// unit the control block names, as the game's own menus do.
constexpr std::uint32_t kRefreshSprite = 0x0803A964u;
constexpr std::uint32_t kWinSpriteIndex = 0x5ED2u;
// Its colours are separate: 0x0802A7A0(0) loads the window unit's palette
// (0x08143378, into the OBJ palette shadow) when the window opens, for every
// unit of the list in the roster (ctl+0x6194 bit 4) but only for the current
// one in battle. So after a switch in battle the new unit wore the first
// unit's colours (Alphonse's leader palette on everyone). The hook calls it
// for the new unit first, on the idle call before the sprite refresh.
constexpr std::uint32_t kLoadPalettes = 0x0802A7A0u;
constexpr char kRosterHookId[] = "kol.roster-status-unit";
// In the army roster the window's unit is not the pointer alone: 0x0803AA8C
// derives it from a position in the grid's list of record indices, and the
// menus (class change among them) call that again. So the position and the
// record index move with the pointer. The battle window has no list there
// (0x0803AA8C returns early for it).
constexpr std::uint32_t kWinList = 0x58FCu;       // u8 record indices, 0xFF ends
constexpr std::uint32_t kWinListLen = 0x20u;
constexpr std::uint32_t kWinPos = 0x5ECBu;        // u8 position in that list
constexpr std::uint32_t kWinRecIndex = 0x5ED2u;   // u8 its record index

// One request, a direction (+1 next, -1 previous), served by whichever hook
// the game runs first: the status task while its window is up, the map's
// state machine otherwise.
std::atomic<int> g_dir{0};
// A tap is served only if a hook can take it soon after: one made while a
// menu or move range is up is dropped, not replayed later.
constexpr std::int64_t kRequestLifeMs = 400;
std::atomic<std::int64_t> g_request_ms{0};
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// The map cycle restarts from the first available unit when the last tap is
// older than this.
constexpr std::int64_t kCycleResetMs = 2000;
std::int64_t g_last_served_ms = 0;
void request(int dir) {
    g_request_ms.store(now_ms(), std::memory_order_relaxed);
    g_dir.store(dir, std::memory_order_release);
}
// The pending direction if it is still fresh, else 0 (and it is dropped).
int pending() {
    const int dir = g_dir.load(std::memory_order_acquire);
    if (!dir) return 0;
    if (now_ms() - g_request_ms.load(std::memory_order_relaxed) > kRequestLifeMs) {
        g_dir.store(0, std::memory_order_release);
        return 0;
    }
    return dir;
}
// The unit button held now: 1 the first (Shortcut A), 2 the second, 0 none.
std::atomic<int> g_button_held{0};
// Every controller button press, for messages that go away on one.
std::atomic<unsigned> g_key_presses{0};
// A recolouring shortcut in progress on the map: its command (2-4), or -1,
// and the button that started it.
int g_highlight = -1;
int g_highlight_button = 0;
// A unit picked on the list: its map object's index, or -1.
std::atomic<int> g_jump{-1};
std::atomic<std::int64_t> g_jump_ms{0};
bool g_readback = false;
std::uint8_t g_lock_before = 0;
std::uint8_t g_index = 0xFF;
std::uint32_t g_epoch = 0;
std::uint32_t g_ctl = 0;
// The status window's unit as its task last saw it, for the second screen.
std::atomic<std::uint32_t> g_window_unit{0};
std::atomic<std::int64_t> g_window_seen_ms{0};

// TEST: KOL_NEXT_UNIT_AT="n1,-n2,..." requests next (previous, negative) on
// those hook calls, counted over both hooks, and traces what follows.
int g_calls = 0;
int g_trace = 0;
long next_test_call() {
    static const char* s = std::getenv("KOL_NEXT_UNIT_AT");
    while (s && *s) {
        char* end = nullptr;
        const long v = std::strtol(s, &end, 10);
        if (end == s) break;
        s = *end ? end + 1 : end;
        if ((v < 0 ? -v : v) > g_calls) return v;
    }
    return 0;
}
long g_next_test = -1;
// TEST: KOL_SHORTCUT_AT="b:n1-n2,..." holds unit button b (1 first, 2
// second) from hook call n1 to n2, as the pad would.
void shortcut_test() {
    static const char* spec = std::getenv("KOL_SHORTCUT_AT");
    if (!spec) return;
    int held = 0;
    for (const char* t = spec; t && *t;) {
        int b = 0, from = 0, to = 0;
        if (std::sscanf(t, "%d:%d-%d", &b, &from, &to) == 3 && g_calls >= from && g_calls <= to)
            held = b;
        if (std::sscanf(t, "%d:%d-%d", &b, &from, &to) == 3 && g_calls == from)
            request(b == 1 ? 1 : -1);
        t = std::strchr(t, ',');
        if (t) ++t;
    }
    g_button_held.store(held, std::memory_order_relaxed);
}

void tick_test() {
    ++g_calls;
    shortcut_test();
    if (g_next_test == -1) g_next_test = next_test_call();
    if (g_next_test && (g_next_test < 0 ? -g_next_test : g_next_test) == g_calls) {
        request(g_next_test < 0 ? -1 : 1);
        g_next_test = next_test_call();
    }
}

int on_handler(std::uint32_t, int, ArmCpuState* cpu) {
    tick_test();
    const std::uint32_t ctl = bus_read_u32(kCtlPtr);
    if (g_trace > 0 && ctl) {
        --g_trace;
        std::printf("next_unit: call=%d S=%u idx=%u mode=%u m36=%u cur=(%u,%u) blink=%u\n",
                    g_calls, bus_read_u8(ctl + kCtlState),
                    bus_read_u8(ctl + kCtlIndex), bus_read_u8(kCursorMode), bus_read_u8(kCursorLock),
                    bus_read_u16(0x03000FDCu), bus_read_u16(0x03000FE0u),
                    bus_read_u16(ctl + kCtlBlink));
    }
    if (g_epoch != g_runtime_state_epoch || ctl != g_ctl) {
        g_epoch = g_runtime_state_epoch;
        g_ctl = ctl;
        g_index = 0xFF;
        g_readback = false;
        g_highlight = -1;
    }
    if (!ctl) return 0;
    if (g_readback) {
        // The selection leaves cursor+0x36 at 0xFF, which locks the D-pad;
        // the game's own flow puts it back when A is released. Do the same.
        g_readback = false;
        g_index = bus_read_u8(ctl + kCtlIndex);
        bus_write_u8(kCursorLock, g_lock_before);
    }
    // Only from ordinary cursor control, and not while the flags the
    // machine's own L entry refuses on are up.
    const std::uint32_t flags = bus_read_u32(kFlags);
    const bool ordinary = bus_read_u8(ctl + kCtlState) == 0 && !(flags & 0x1000u) &&
                          !(flags & 0x80000000u);
    const int jump = g_jump.load(std::memory_order_acquire);
    if (jump >= 0) {
        if (now_ms() - g_jump_ms.load(std::memory_order_relaxed) > kRequestLifeMs) {
            g_jump.store(-1, std::memory_order_release);
        } else if (ordinary && bus_read_u8(kCursorMode) == kCursorFree) {
            g_jump.store(-1, std::memory_order_release);
            const std::uint32_t obj = bus_read_u32(kObjPtr) + jump * kObjStride;
            const std::uint32_t x = (bus_read_u16(obj + 0x10) >> 5) & 0xFFu;
            const std::uint32_t y = (bus_read_u16(obj + 0x14) >> 5) & 0xFFu;
            if (std::getenv("KOL_LIST_TAP"))
                std::printf("unit_list: call=%d cursor -> object %d cell (%u,%u)\n", g_calls,
                            jump, x, y);
            bus_write_u16(kCursorCellX, static_cast<std::uint16_t>(x));
            bus_write_u16(kCursorCellY, static_cast<std::uint16_t>(y));
            bus_write_u8(kCursorMode, kCursorGlide);
            return 0;   // the cursor glides from this frame's handler on
        }
    }
    if (g_highlight >= 0) {
        // A recolouring shortcut: the selection again each frame while its
        // button is held, as state 5 does; then the release, as the game's.
        if (g_button_held.load(std::memory_order_relaxed) == g_highlight_button && ordinary) {
            cpu->R[0] = static_cast<std::uint32_t>(g_highlight);
            cpu->R[15] = kSelect;
            return 1;
        }
        const std::uint32_t objs = bus_read_u32(kObjPtr);
        for (int i = 0; i < kObjCount; ++i)
            bus_write_u8(objs + i * kObjStride + 0x65, bus_read_u8(ctl + kCtlSaved + i));
        bus_write_u16(ctl + kCtlBlink, 0);
        bus_write_u8(ctl + kCtlFresh, 1);
        if (std::getenv("KOL_SHORTCUT_AT"))
            std::printf("shortcut: call=%d release command %d\n", g_calls, g_highlight);
        g_highlight = -1;
        cpu->R[0] = 2;
        cpu->R[15] = kShortcutEnd;
        return 1;
    }
    const int dir = pending();
    if (!dir) return 0;
    if (!ordinary) return 0;
    g_dir.store(0, std::memory_order_release);
    // The button's shortcut, as the OPTION window sets it.
    int command = bus_read_u8(dir > 0 ? kShortcutA : kShortcutB);
    if (const char* f = std::getenv("KOL_SHORTCUT_FORCE")) command = std::atoi(f);   // TEST
    if (command < 0 || command >= kShortcutCommands) command = dir > 0 ? 0 : 1;
    const std::int64_t tap_ms = g_request_ms.load(std::memory_order_relaxed);
    if (tap_ms - g_last_served_ms > kCycleResetMs) g_index = 0xFF;
    g_last_served_ms = tap_ms;

    // What the machine's L entry does before its first selection: snapshot
    // each object's +0x65 (the selection restores the previous unit's from
    // it) and start a fresh selection rather than a blink.
    const std::uint32_t objs = bus_read_u32(kObjPtr);
    for (int i = 0; i < kObjCount; ++i)
        bus_write_u8(ctl + kCtlSaved + i,
                     bus_read_u8(objs + i * kObjStride + 0x65));
    bus_write_u8(ctl + kCtlFresh, 1);
    if (command >= 2) {
        // Element, Wounded, Level Up: recolour while the button is held.
        bus_write_u16(ctl + kCtlBlink, 0);
        g_highlight = command;
        g_highlight_button = dir > 0 ? 1 : 2;
        if (std::getenv("KOL_SHORTCUT_AT"))
            std::printf("shortcut: call=%d start command %d\n", g_calls, command);
        cpu->R[0] = static_cast<std::uint32_t>(command);
        cpu->R[15] = kSelect;
        return 1;
    }
    bus_write_u8(ctl + kCtlIndex, g_index);
    g_lock_before = bus_read_u8(kCursorLock);
    g_readback = true;
    if (std::getenv("KOL_NEXT_UNIT_AT")) {
        std::printf("next_unit: call=%d t=%lld ms command %d (%s button) from idx=%u\n",
                    g_calls, static_cast<long long>(tap_ms), command, dir > 0 ? "first" : "second",
                    g_index);
        g_trace = 40;
    }

    cpu->R[0] = static_cast<std::uint32_t>(command);   // 0 next, 1 reverse
    cpu->R[15] = kSelect;   // R14 is the handler's return: a tail call
    return 1;
}

// In the roster: the unit `dir` steps from `unit` in the window's own list
// (the grid's order), wrapping, and its position there; 0 when `unit` is not
// in it.
std::uint32_t list_neighbour(std::uint32_t win, std::uint32_t unit, int dir,
                             std::uint8_t* pos) {
    if (unit < kArmy || (unit - kArmy) % kRecSize != 0u) return 0;
    const std::uint32_t rec = (unit - kArmy) / kRecSize;
    std::uint32_t n = 0, at = kWinListLen;
    for (; n < kWinListLen; ++n) {
        const std::uint8_t r = bus_read_u8(win + kWinList + n);
        if (r == 0xFFu) break;
        if (r == rec) at = n;
    }
    if (at == kWinListLen || n < 2u) return 0;
    *pos = static_cast<std::uint8_t>((at + n + (dir > 0 ? 1u : n - 1u)) % n);
    return kArmy + bus_read_u8(win + kWinList + *pos) * kRecSize;
}

// The unit `dir` steps from `unit` in its own list — the army's slots or its
// side of the battle — wrapping, skipping empty slots; 0 when there is none.
// `entry` receives the battle entry to go with it (0 for the army).
std::uint32_t neighbour(std::uint32_t unit, int dir, std::uint32_t* entry) {
    std::uint32_t first = 0, slots = 0, entries = 0;
    bool battle = false;
    if (unit >= kArmy && unit < kArmy + kArmySlots * kRecSize) {
        first = kArmy;
        slots = kArmySlots;
    } else if (unit >= kBattle && unit < kBattle + 2u * kSideSlots * kRecSize) {
        const std::uint32_t side = (unit - kBattle) / (kSideSlots * kRecSize);
        first = kBattle + side * kSideSlots * kRecSize;
        entries = kBattleEntries + side * kSideSlots * kEntrySize;
        slots = kSideSlots;
        battle = true;
    } else {
        return 0;
    }
    if ((unit - first) % kRecSize != 0u) return 0;
    const std::uint32_t at = (unit - first) / kRecSize;
    for (std::uint32_t k = 1; k < slots; ++k) {
        const std::uint32_t i = (at + slots + (dir > 0 ? k : slots - k)) % slots;
        const std::uint32_t r = first + i * kRecSize;
        const bool live = battle
            ? bus_read_u8(r + 0x4Eu) != 0u && bus_read_u16(r + 0x40u) != 0u
            : bus_read_u8(r + 0x4Fu) != 0u;
        if (!live) continue;
        *entry = battle ? entries + i * kEntrySize : 0u;
        return r;
    }
    return 0;
}

bool status_trace() {
    static const bool on = [] {
        const char* v = std::getenv("KOL_STATUS_TRACE");
        return v && v[0] && v[0] != '0';
    }();
    return on;
}

bool status_switch_off() {
    static const bool off = [] {
        const char* v = std::getenv("KOL_NO_STATUS_SWITCH");
        return v && v[0] && v[0] != '0';
    }();
    return off;
}

// The status window's own task: records the unit it shows, for the second
// screen. It changes nothing — restarting THIS task from 0 is a scene
// transition that leaves the screen black; the contents are drawn by the
// task that opened the window.
// ── A unit's profile from the world map ─────────────────────────────────
// A tap on the second screen's army list opens the status window Organize
// opens, on that unit, with no grid before it. Every step is the game's own:
// 1. Roaming (task 0x08128D9C, state 4): its R branch (0x081292E8) starts
//    the command menu's task in slot 1 and suspends itself (slot 0). The
//    hook does the same, as two tail calls on two calls.
// 2. The command menu (task 0x08128220, state 1): A on Organize queues
//    state 11 at 0x02029C54 and enters the fade-out, state 15. The hook does
//    the same. State 11 sets the roster up (0x08039EC0, screen 0 = grid) and
//    starts the status task 0x0803A0D0; state 14 waits for it, and 17 and 18
//    fade back in to state 1, the command menu, Organize highlighted.
// 3. The status task's first call: the screen becomes 1, the window (the
//    grid's A writes 1 there, 0x080402EA), and the previous screen 0, the
//    grid's. The task's init builds the list (0x08039854) and the palettes.
// 4. The roster task's first call: the window's list position (+0x5ECB),
//    from which its init derives the unit (0x0803AA8C), is the tapped one.
// 5. B in the window goes back to the grid unless the mode byte +0x6194 has
//    bit 5, the battle window's, which closes the whole menu (0x08036224;
//    the grid's B does the same, exit code 5 at +0x61A7). The hook sets the
//    bit for the one call that takes B and clears it on the next: bit 5 also
//    turns the R menu off (0x080362C0), and a call takes one key.
// From roaming, the command menu is never seen (asked for on 10-01): its
// icons and label are OAM the task draws every frame (0x08127F7C, called at
// the end of every state but the wait, 14), and the draw returns at once
// from the tap until the menu is gone again. Back from the window, the menu
// fades in (17, 18) to state 1, where the hook hands it a B (new keys
// 0x03000822, read there and only there while the menu is up): the game's
// own close, 0x0812841C, resumes roaming (slot 0), ends the menu (slot 1)
// and puts the map's marker back. Its cancel sound 0xB0 is dropped: the
// window's B made one a moment before.
// While the window is up, a tap on another unit of the list shows that one,
// as Next does (point the window at it, rebuild from the task's init).
constexpr std::uint32_t kWorldMap = 0x02029C5Cu;      // u32 -> world map control
constexpr std::uint32_t kWorldTasks = 0xB4u;          // its task slots
constexpr std::uint32_t kRoamTask = 0x08128D9Cu;
constexpr std::uint8_t kRoamPad = 4;
constexpr std::uint32_t kCommandTask = 0x08128220u;
constexpr std::uint8_t kCommandPad = 1, kCommandOrganize = 11, kCommandFadeOut = 15;
constexpr std::uint8_t kCommandFadeIn = 17;
constexpr std::uint32_t kCommandNext = 0x02029C54u;   // u16 the state after the fade
constexpr std::uint32_t kTaskStart = 0x08002D38u;     // (slots, i, fn|1)
constexpr std::uint32_t kTaskSuspend = 0x08002E7Cu;   // (slots, i)
constexpr std::uint32_t kWinPrevScreen = 0x61A5u;     // kWinSub is the screen
constexpr std::uint32_t kWinExit = 0x61A7u;           // u8 exit code
constexpr std::uint8_t kStatusUp = 3;                 // the status task, the grid (or a screen) up
// Item, Spell or Class from the bar open as the player opens them: R in the
// grid, then A in its R menu on the item. The window's key reader 0x0803AD4C
// (the first new key of A, B, Select, Start, Right, Left, Up, Down, R, L;
// it keeps the key at ctl+0x61BC and "new" at +0x61C0) answers R to the
// grid's read (its call at 0x08032A56) and, with the R menu's cursor
// (+0x61CC) on the item, A to the R menu's (0x0802BEA8). The R menu's A
// then does the rest: 0x080402E0(screen 4 / 9 / 15, exit 4), the fade.
// Setting the screen id by hand (at the status task's first call, or with
// the grid up) left the screen black.
constexpr std::uint32_t kKeyRead = 0x0803AD4Cu;
constexpr std::uint32_t kGridKeyReadReturn = 0x08032A5Au, kRMenuKeyReadReturn = 0x0802BEACu;
constexpr std::uint32_t kWinKey = 0x61BCu, kWinKeyNew = 0x61C0u, kWinRMenuCursor = 0x61CCu;
// The same in a unit's window (its screen 1): R there opens the unit's own
// R menu, screen 0x1B (0x080362DE), Item, Spell, Class and Dismiss for that
// unit, its cursor at +0x61CE (0x080343EE). The window reads its keys from
// its call returning to 0x08035F9C, that R menu from 0x08034068.
struct KeyContext { std::uint32_t open_ret, menu_ret, cursor; };
constexpr KeyContext kGridContext{kGridKeyReadReturn, kRMenuKeyReadReturn, kWinRMenuCursor};
constexpr KeyContext kUnitContext{0x08035F9Cu, 0x08034068u, 0x61CEu};
constexpr std::uint8_t kScreenGrid = 0, kScreenUnit = 1, kScreenGridMenu = 3, kScreenUnitMenu = 0x1B;
KeyContext g_key_ctx = kGridContext;
int g_flow_item = 0;   // Item 0, Spell 1, Class 2: the R menus' order
constexpr std::uint32_t kRMenuDraw = 0x080414C0u, kGridSprites = 0x0804096Cu;
constexpr char kRMenuDrawHookId[] = "kol.rmenu-draw";
bool g_hide_rmenu = false;   // the grid's R menu is ours to pass through: not drawn
constexpr std::uint16_t kReadKeyR = 0x100;   // the reader's mask for R
constexpr char kKeyReadHookId[] = "kol.profile-key-read";
// Back from the screen (the player's B returns to the R menu), from roaming
// the reader answers B to the R menu and B to the grid (its B closes
// Organize, 0x08032C5E): one B back to the map, as the other bar buttons.
int g_screen_stage = 0;   // 1: R to the grid; 2: A to the R menu; 3, 4: B to the R menu, the grid
int g_screen_chain = 0;   // B's chained after the screen: 2 from roaming, 1 from the grid, 0 from the R menu
// Item, Spell or Class tapped with Organize's grid (screen 0) or its R menu
// (3), or a unit's window (1) or its R menu (0x1B), already up: the item + 1,
// straight into the key reader's steps.
std::atomic<int> g_org_request{0};
std::atomic<std::int64_t> g_org_request_ms{0};
std::atomic<bool> g_org_ready{false};   // one of those is up, nothing under way
constexpr std::uint32_t kWinMode = 0x6194u;
constexpr std::uint8_t kModeBattle = 0x20u;
constexpr std::uint32_t kKeysNew = 0x03000822u;       // the window's 0x0803AD4C reads A, then B
constexpr std::uint16_t kKeyA = 1, kKeyB = 2;
constexpr char kRoamHookId[] = "kol.profile-roam";
constexpr char kCommandHookId[] = "kol.profile-command";
constexpr std::uint32_t kCommandDraw = 0x08127F7Cu;
constexpr std::uint32_t kPlaySound = 0x0814506Cu;     // (id)
constexpr std::uint32_t kSoundCancel = 0xB0u;
constexpr char kCommandDrawHookId[] = "kol.profile-command-draw";
constexpr char kSoundHookId[] = "kol.profile-sound";

// The second screen's Save and Load buttons (world map only) take the same
// road: A on Save / Load in the menu's system submenu (state 3, 0x081285D0)
// queues state 7 / 6 behind the fade-out, as A on Organize queues 11; the
// game's save or load screen follows, the menu waits (14), fades back in
// (17, 18) and returns to the submenu (3), where the hook hands it B: back to
// the main menu (1) when there is one (story flag 0x6E, 0x08142F50), then B
// again, roaming. The screens wait on the player, so the flow does not time
// out there; it ends when the menu's task is gone (a save loaded).
constexpr std::uint8_t kCommandSubPad = 3;
constexpr std::uint32_t kMenuSubOpen = 0x27u, kMenuSubCursor = 0x21u;   // world map control
constexpr std::uint16_t kCommandSave = 7, kCommandLoad = 6;
constexpr std::uint16_t kCommandTraining = 12, kCommandShop = 13;   // Organize is 11
// What the menu lists (its builder 0x08127E00): Organize and System always;
// Training with two units or more in the army (record +0x4E); Shop where
// the place is a town (0x08144214(wm+0x14) & 3 == 2: its EWRAM byte at
// 0x0200119E + place nonzero and byte 2 of its ROM record, 0x08621228 +
// place * 0x2C, nonzero); neither while story flag 0x92 (EWRAM 0x0200122C +
// 0x92) names the place the army is at (0x02001197).
constexpr std::uint32_t kPlaceKnown = 0x0200119Eu, kPlaceRecords = 0x08621228u;
constexpr std::uint32_t kPlaceHere = 0x02001197u, kStoryFlags = 0x0200122Cu;
constexpr std::uint32_t kWmPlace = 0x14u;
std::atomic<int> g_items_ok{0};   // bit 0 Training, bit 1 Shop
enum class Profile { Idle, Starting, Suspending, Launch, Window, Up, Returning };
Profile g_profile = Profile::Idle;
bool g_profile_from_roam = false;   // the menu stays unseen, and B goes back to roaming
std::uint16_t g_flow_next = 0;      // the state queued behind the fade: 11, 7 or 6
int g_mute_cancel = 0;              // the menu's B, ours: those cancel sounds dropped
std::int64_t g_mute_until = 0;
std::int64_t g_command_seen_ms = 0;   // the menu's task last ran
std::atomic<std::int64_t> g_command_seen_at{0};   // the same, for the second screen's thread
std::uint32_t g_profile_target = 0;   // the tapped unit's record
std::uint32_t g_profile_epoch = 0;
std::int64_t g_profile_ms = 0;   // when the flow last moved
std::uint32_t g_mode_restore = 0;   // the control block whose bit 5 is ours
std::atomic<std::uint32_t> g_profile_record{0};   // a tap, for the guest thread to take
std::atomic<std::int64_t> g_profile_request_ms{0};
std::atomic<int> g_menu_request{0};   // a world button's state: Save 7, Load 6, 11, 12, 13
std::atomic<int> g_menu_screen{0};    // with 11: Organize's screen to open (Item 4, Spell 9, Class 15)
std::uint8_t g_flow_screen = 0;       // the same, taken with the request
std::atomic<std::int64_t> g_menu_request_ms{0};
std::atomic<bool> g_profile_busy{false};   // for the second screen's thread
// One of the menu's screens (save, load, Organize, training, shop), however
// it was opened: from the menu queueing it (its fade-out included) until it
// is back at a pad (1 or 3). The second screen greys its world buttons.
bool g_file_phase = false;
std::atomic<bool> g_file_open{false};
std::atomic<std::int64_t> g_file_seen_ms{0};
// Organize, however it was opened (the menu's A or a tap): from its state 11
// until the menu is back at state 1. The army list stays on the second
// screen meanwhile, with the unit the grid's cursor or the window is on lit:
// the record index ctl+0x5ED2, which the grid's cursor moves (0x0803AA8C,
// from the list position +0x5ECB) and the window keeps.
constexpr std::uint8_t kCommandWait = 14;
bool g_roster_phase = false;
std::atomic<bool> g_roster_open{false};
std::atomic<std::int64_t> g_roster_seen_ms{0};
std::atomic<std::uint32_t> g_roster_unit{0};
// Organize's screen: the grid (ctl+0x61A4 == 0) or a unit's window. In the
// grid the second screen shows the sheet of the unit under the grid's
// cursor; with a window up (the sheet is on the top screen) the army list.
std::atomic<bool> g_roster_window{false};

bool profile_off() {
    static const bool off = [] {
        const char* e = std::getenv("KOL_NO_WORLD_PROFILE");
        return e && e[0] && e[0] != '0';
    }();
    return off;
}

bool profile_trace() {
    static const bool on = std::getenv("KOL_PROFILE_TRACE") != nullptr;
    return on;
}

void profile_to(Profile p) {
    g_profile = p;
    g_profile_ms = now_ms();
    g_profile_busy.store(p != Profile::Idle, std::memory_order_relaxed);
    if (profile_trace())
        std::printf("profile: call=%d -> %d record=%08X roam=%d\n", g_calls, static_cast<int>(p),
                    g_profile_target, g_profile_from_roam ? 1 : 0);
}

// The flow, after a state load or a step that never came (the menu was left
// another way before the window): back to idle. Up lasts as long as the menu.
void profile_check() {
    if (g_profile_epoch != g_runtime_state_epoch) {
        g_profile_epoch = g_runtime_state_epoch;
        g_profile = Profile::Idle;
        g_profile_busy.store(false, std::memory_order_relaxed);
        g_mode_restore = 0;
        g_mute_cancel = 0;
        g_roster_phase = false;
        g_roster_open.store(false, std::memory_order_relaxed);
        g_file_phase = false;
        g_file_open.store(false, std::memory_order_relaxed);
        g_screen_stage = 0;
        g_hide_rmenu = false;
        g_org_ready.store(false, std::memory_order_relaxed);
    }
    // A step that never came: before the menu took the tap, or the window
    // that did not open. Past that the menu's own screens wait on the
    // player; they end with the menu's task.
    const bool waits = g_profile == Profile::Launch || g_profile == Profile::Up;
    if (g_profile != Profile::Idle && !waits && now_ms() - g_profile_ms > 3000)
        profile_to(Profile::Idle);
    if (g_profile != Profile::Idle && g_profile != Profile::Starting &&
        g_profile != Profile::Suspending && now_ms() - g_command_seen_ms > 1000)
        profile_to(Profile::Idle);
}

// A fresh tap on a world button: the state it queues (Save 7, Load 6,
// Organize 11, Training 12, Shop 13), taken (0 for none). The main menu's
// items only where the main menu is (`main_ok`), the submenu's anywhere.
std::uint16_t menu_take(bool main_ok) {
    int next = g_menu_request.load(std::memory_order_acquire);
    if (!next || now_ms() - g_menu_request_ms.load(std::memory_order_relaxed) > kRequestLifeMs)
        return 0;
    if (!main_ok && next != kCommandSave && next != kCommandLoad) return 0;
    if (!g_menu_request.compare_exchange_strong(next, 0)) return 0;
    g_flow_screen = static_cast<std::uint8_t>(g_menu_screen.load(std::memory_order_relaxed));
    return static_cast<std::uint16_t>(next);
}

// Training and Shop where the menu would list them (see kPlaceKnown).
int items_ok(std::uint32_t wm) {
    const std::uint8_t here = bus_read_u8(kPlaceHere);
    const std::uint8_t lock = bus_read_u8(kStoryFlags + 0x92u);
    if (lock != 0 && lock == here) return 0;
    int ok = 0;
    int units = 0;
    for (std::uint32_t i = 0; i < kArmySlots; ++i)
        if (bus_read_u8(kArmy + i * kRecSize + 0x4Eu) != 0) ++units;
    if (units > 1) ok |= 1;
    const std::uint16_t place = bus_read_u16(wm + kWmPlace);
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    const std::uint32_t rec = kPlaceRecords - 0x08000000u + place * 0x2Cu + 2u;
    const bool town = bus_read_u8(kPlaceKnown + place) != 0 && rom && rec < rom_size && rom[rec] != 0;
    // Shop Anywhere: the shop's screen and stock do not depend on where the
    // army is (the same, byte for byte, with the place set to one without a
    // shop: README), so its button works away from towns too.
    if (town || setting_on(Setting::ShopAnywhere)) ok |= 2;
    return ok;
}

std::uint32_t profile_take();

// A tap of either kind for an idle flow: the state to queue (0 for none).
std::uint16_t flow_take(bool organize_ok) {
    g_flow_screen = 0;
    if (organize_ok && (g_profile_target = profile_take()) != 0) return kCommandOrganize;
    g_profile_target = 0;   // Organize on a screen of its R menu (g_flow_screen)
    return menu_take(organize_ok);
}

// A fresh tap's record, taken (0 for none): each tap is served once.
std::uint32_t profile_take() {
    std::uint32_t rec = g_profile_record.load(std::memory_order_acquire);
    if (!rec || now_ms() - g_profile_request_ms.load(std::memory_order_relaxed) > kRequestLifeMs)
        return 0;
    return g_profile_record.compare_exchange_strong(rec, 0u) ? rec : 0;
}

int on_roam_task(std::uint32_t, int, ArmCpuState* cpu) {
    profile_check();
    if (const std::uint32_t at = bus_read_u32(kWorldMap)) {
        const int ok = items_ok(at);
        if (ok != g_items_ok.exchange(ok, std::memory_order_relaxed) && profile_trace())
            std::printf("profile: place %u (here %u): training %d shop %d\n",
                        bus_read_u16(at + kWmPlace), bus_read_u8(kPlaceHere), ok & 1, (ok >> 1) & 1);
    }
    if (profile_off() || bus_read_u8(cpu->R[1]) != kRoamPad) return 0;
    const std::uint32_t wm = bus_read_u32(kWorldMap);
    if (!wm) return 0;
    if (g_profile == Profile::Idle && (g_flow_next = flow_take(true)) != 0) {
        g_profile_from_roam = true;
        cpu->R[0] = wm + kWorldTasks;
        cpu->R[1] = 1;
        cpu->R[2] = kCommandTask | 1u;
        cpu->R[15] = kTaskStart;
        profile_to(Profile::Starting);
        return 1;
    }
    if (g_profile == Profile::Starting) {
        cpu->R[0] = wm + kWorldTasks;
        cpu->R[1] = 0;
        cpu->R[15] = kTaskSuspend;
        profile_to(Profile::Suspending);
        return 1;
    }
    return 0;
}

// Organize's phase and the unit to light, from the command menu's state.
void roster_track(std::uint8_t s) {
    if (s == kCommandOrganize) g_roster_phase = true;
    else if (s == kCommandPad || s == 0 || s == 12 || s == 13) g_roster_phase = false;
    // After a state load in Organize: the menu waits (14) on the status task,
    // which Organize starts and the other items do not.
    else if (s == kCommandWait &&
             now_ms() - g_window_seen_ms.load(std::memory_order_relaxed) < 200)
        g_roster_phase = true;
    const std::uint32_t ctl = bus_read_u32(kWinPtr);
    g_roster_window.store(g_roster_phase && ctl && bus_read_u8(ctl + kWinSub) != 0,
                          std::memory_order_relaxed);
    {
        const std::uint8_t scr = ctl ? bus_read_u8(ctl + kWinSub) : 0xFFu;
        // Our pass through the R menu is over once its screen has gone with
        // nothing left to answer (the last B closes it a few frames on).
        if (g_hide_rmenu && g_screen_stage == 0 && scr != kScreenGridMenu) g_hide_rmenu = false;
        const bool screen_ok = scr == kScreenGrid || scr == kScreenGridMenu ||
                               scr == kScreenUnit || scr == kScreenUnitMenu;
        g_org_ready.store(g_roster_phase && s == kCommandWait && screen_ok &&
                              (g_profile == Profile::Idle || g_profile == Profile::Up) &&
                              g_screen_stage == 0,
                          std::memory_order_relaxed);
    }
    std::uint32_t unit = g_roster_unit.load(std::memory_order_relaxed);
    const bool tap_on_its_way = g_profile == Profile::Starting || g_profile == Profile::Suspending ||
                                g_profile == Profile::Launch || g_profile == Profile::Window;
    if (tap_on_its_way) {
        // The tapped unit, until the window is built on it: before that the
        // record index is still the grid's first (Alphonse lit in the fade).
        unit = g_profile_target;
    } else if (!g_roster_phase) {
        unit = 0;
    } else if (s == kCommandWait) {   // the grid or the window is up
        // The R menu (screen 3) and its Item, Spell and Class screens are the
        // army's, no one unit's: nothing lit there (the index still names
        // the grid's unit).
        const std::uint8_t screen = ctl ? bus_read_u8(ctl + kWinSub) : 0u;
        const bool army_wide = screen == 3u || screen == 4u || screen == 9u || screen == 15u;
        const std::uint8_t index = ctl && !army_wide ? bus_read_u8(ctl + kWinRecIndex) : 0xFFu;
        unit = index < kArmySlots ? kArmy + index * kRecSize : 0;
    } else if (s != kCommandFadeIn && s != kCommandFadeIn + 1) {
        unit = 0;
    }
    g_roster_unit.store(unit, std::memory_order_relaxed);
    g_roster_seen_ms.store(now_ms(), std::memory_order_relaxed);
    g_roster_open.store(g_roster_phase, std::memory_order_relaxed);
}

int on_command_task(std::uint32_t, int, ArmCpuState* cpu) {
    profile_check();
    const std::uint32_t state = cpu->R[1];
    const std::uint8_t s = bus_read_u8(state);
    roster_track(s);
    g_command_seen_ms = now_ms();
    g_command_seen_at.store(g_command_seen_ms, std::memory_order_relaxed);
    if (const std::uint32_t at = bus_read_u32(kWorldMap))
        g_items_ok.store(items_ok(at), std::memory_order_relaxed);
    {
        const std::uint16_t next = bus_read_u16(kCommandNext);
        auto screen = [](unsigned v) {
            return v == kCommandSave || v == kCommandLoad || v == kCommandOrganize ||
                   v == kCommandTraining || v == kCommandShop;
        };
        const bool queued = (s == kCommandFadeOut || s == kCommandFadeOut + 1) && screen(next);
        const bool was = g_file_phase;
        if (queued || screen(s)) g_file_phase = true;
        else if (s == kCommandPad || s == kCommandSubPad || s == 0) g_file_phase = false;
        if (g_file_phase != was && profile_trace())
            std::printf("profile: menu screen %s (menu state %u)\n", g_file_phase ? "up" : "down", s);
        g_file_open.store(g_file_phase, std::memory_order_relaxed);
        g_file_seen_ms.store(now_ms(), std::memory_order_relaxed);
    }
    if (profile_off()) return 0;
    if ((g_profile == Profile::Up || g_profile == Profile::Launch || g_profile == Profile::Window) &&
        s == kCommandFadeIn) {
        g_mode_restore = 0;
        profile_to(Profile::Returning);   // the window or screen is closed; the menu fades in
        return 0;
    }
    if (s != kCommandPad && s != kCommandSubPad) return 0;
    if (g_profile == Profile::Returning) {
        // From roaming, on to the menu's B (from the submenu too, which goes
        // to the main menu first); else the menu, where the game itself
        // comes back.
        if (g_profile_from_roam) {
            bus_write_u16(kKeysNew, kKeyB);
            ++g_mute_cancel;
            g_mute_until = now_ms() + 1000;
            if (s == kCommandSubPad) {
                profile_to(Profile::Returning);
                return 0;
            }
        }
        profile_to(Profile::Idle);
        return 0;   // the task takes B: roaming again
    }
    if (g_profile == Profile::Idle) {
        g_flow_next = flow_take(s == kCommandPad);   // Organize from the main menu only
        if (!g_flow_next) return 0;
        g_profile_from_roam = false;   // the player's menu
    } else if (g_profile != Profile::Suspending) {
        return 0;
    }
    if (g_flow_next == kCommandSave || g_flow_next == kCommandLoad) {
        // As the system submenu's A leaves it: the submenu open (+0x27) with
        // its cursor (+0x21) on Save or Load, where the game comes back after
        // the screen (state 3). Queued from the main menu without them, it
        // came back to the submenu drawing the main menu's icons (DW).
        if (const std::uint32_t wm = bus_read_u32(kWorldMap)) {
            bus_write_u8(wm + kMenuSubOpen, 1);
            bus_write_u8(wm + kMenuSubCursor, g_flow_next == kCommandSave ? 0 : 1);
        }
    }
    bus_write_u16(kCommandNext, g_flow_next);
    bus_write_u8(state, kCommandFadeOut);
    profile_to(Profile::Launch);
    return 0;   // the task runs on, from its fade-out
}

int on_key_read(std::uint32_t, int, ArmCpuState* cpu) {
    if (profile_off()) return 0;
    const std::uint32_t ret = cpu->R[14] & ~1u;
    if (g_screen_stage == 0 && (g_profile == Profile::Idle || g_profile == Profile::Up)) {
        const KeyContext* ctx = ret == kGridContext.open_ret || ret == kGridContext.menu_ret ? &kGridContext
                              : ret == kUnitContext.open_ret || ret == kUnitContext.menu_ret ? &kUnitContext
                              : nullptr;
        int item = g_org_request.load(std::memory_order_acquire);
        if (ctx && item && now_ms() - g_org_request_ms.load(std::memory_order_relaxed) <= kRequestLifeMs &&
            g_org_request.compare_exchange_strong(item, 0)) {
            // A tap in Organize: from the grid or a unit's window R then A,
            // back with one B; from an R menu just A, back to it as the game
            // goes.
            g_key_ctx = *ctx;
            g_flow_item = item - 1;
            const bool open = ret == ctx->open_ret;
            g_screen_stage = open ? 1 : 2;
            g_screen_chain = open ? 1 : 0;
        }
    }
    if (g_screen_stage == 0) return 0;
    const std::uint32_t ctl = bus_read_u32(kWinPtr);
    if (!ctl) return 0;
    std::uint16_t key = 0;
    if (g_screen_stage == 1 && ret == g_key_ctx.open_ret) {
        key = kReadKeyR;
        g_screen_stage = 2;
        if (g_key_ctx.open_ret == kGridKeyReadReturn) g_hide_rmenu = true;
    } else if (g_screen_stage == 2 && ret == g_key_ctx.menu_ret) {
        // Item 0, Spell 1, Class 2: both R menus' order.
        bus_write_u8(ctl + g_key_ctx.cursor, static_cast<std::uint8_t>(g_flow_item));
        key = kKeyA;
        g_screen_stage = g_screen_chain > 0 ? 3 : 0;
        if (g_profile == Profile::Launch) profile_to(Profile::Up);
    } else if ((g_screen_stage == 3 && ret == g_key_ctx.menu_ret) ||
               (g_screen_stage == 4 && ret == g_key_ctx.open_ret)) {
        key = kKeyB;
        g_screen_stage = g_screen_stage == 3 && g_screen_chain >= 2 ? 4 : 0;
        ++g_mute_cancel;   // the player's B made the one cancel sound
        g_mute_until = now_ms() + 1000;
    } else {
        return 0;
    }
    if (profile_trace()) std::printf("profile: key read -> %03X\n", key);
    bus_write_u16(ctl + kWinKey, key);
    bus_write_u16(ctl + kWinKeyNew, 1);
    cpu->R[0] = key;
    cpu->R[15] = ret;   // back to the caller, as the reader returns
    return 1;
}

// The grid's R menu, unseen while the steps above pass through it (the
// user wanted a clean look): its screen's draw 0x080414C0 (second routine of
// screen 3 at 0x0860D77C) puts up the menu's icons, label and hand and
// ends with the grid's own sprites, 0x0804096C (no arguments); the hook runs
// only that. From the R it answers until the menu's screen is gone again.
int on_rmenu_draw(std::uint32_t, int, ArmCpuState* cpu) {
    if (!g_hide_rmenu || profile_off()) return 0;
    cpu->R[15] = kGridSprites;   // the units only; returns to the draw's caller
    return 1;
}

// The command menu's draw: not while a tap from roaming is on its way to the
// window or back.
int on_command_draw(std::uint32_t, int, ArmCpuState* cpu) {
    profile_check();
    if (profile_off() || g_profile == Profile::Idle || !g_profile_from_roam) return 0;
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the task, nothing drawn
    return 1;
}

// The sound effect routine: only the cancel sounds of the menu's B, ours.
int on_play_sound(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_mute_cancel <= 0) return 0;
    if (now_ms() > g_mute_until) { g_mute_cancel = 0; return 0; }
    if (profile_trace()) std::printf("profile: call=%d sound %02X\n", g_calls, cpu->R[0]);
    if (cpu->R[0] != kSoundCancel) return 0;
    --g_mute_cancel;
    cpu->R[15] = cpu->R[14] & ~1u;
    return 1;
}

// Points the open window at the army unit `rec`, by its place in the
// window's list; false if it is not listed.
bool profile_point_window(std::uint32_t win, std::uint32_t rec) {
    for (std::uint32_t pos = 0; pos < kWinListLen; ++pos) {
        const std::uint8_t index = bus_read_u8(win + kWinList + pos);
        if (index == 0xFFu) break;
        if (kArmy + index * kRecSize != rec) continue;
        bus_write_u8(win + kWinPos, static_cast<std::uint8_t>(pos));
        bus_write_u8(win + kWinRecIndex, index);
        bus_write_u32(win + kWinUnit, rec);
        return true;
    }
    return false;
}

// The status task's call (step 3).
void profile_status_task(std::uint32_t win, std::uint32_t state) {
    if (g_profile == Profile::Launch && g_flow_next == kCommandOrganize && g_profile_target == 0 &&
        g_flow_screen != 0 && g_screen_stage == 0 && bus_read_u8(state) == kStatusUp &&
        bus_read_u8(win + kWinSub) == 0) {
        g_screen_stage = 1;   // the grid is up: its next key read gets R
        g_screen_chain = g_profile_from_roam ? 2 : 0;
        g_key_ctx = kGridContext;
        g_flow_item = g_flow_screen == 4 ? 0 : g_flow_screen == 9 ? 1 : 2;
    }
    if (g_profile != Profile::Launch || g_flow_next != kCommandOrganize || bus_read_u8(state) != 0)
        return;
    if (g_profile_target != 0) {
        bus_write_u8(win + kWinSub, 1);
        bus_write_u8(win + kWinPrevScreen, 0);
        profile_to(Profile::Window);
    }
}

// The roster task's call (steps 4 and 5).
void profile_roster_task(std::uint32_t win, std::uint32_t state) {

    if (g_mode_restore == win)
        bus_write_u8(win + kWinMode, bus_read_u8(win + kWinMode) & ~kModeBattle);
    g_mode_restore = 0;
    if (g_profile == Profile::Window && bus_read_u8(state) == kTaskBuild) {
        profile_point_window(win, g_profile_target);
        profile_to(Profile::Up);
        return;
    }
    if ((g_profile != Profile::Up && !g_roster_phase) || bus_read_u8(state) != kRosterWindowUp ||
        bus_read_u8(win + kWinSub) != 1)
        return;
    if (const std::uint32_t rec = profile_take()) {
        // Another unit tapped: the window rebuilt on it, as Next does (the
        // EQUIP page; the roster loads every listed unit's colours).
        if (rec != bus_read_u32(win + kWinUnit) && profile_point_window(win, rec)) {
            g_profile_target = rec;
            bus_write_u8(win + kWinPage, 0);
            bus_write_u16(win + kWinPageSlide, 0);
            bus_write_u8(state, kTaskBuild);
            g_window_unit.store(rec, std::memory_order_relaxed);
            if (profile_trace()) std::printf("profile: call=%d switch -> %08X\n", g_calls, rec);
            return;
        }
    }
    if (g_profile != Profile::Up) return;   // Organize opened by the menu: its own B
    const std::uint16_t keys = bus_read_u16(kKeysNew);
    if ((keys & kKeyB) && !(keys & kKeyA)) {
        bus_write_u8(win + kWinMode, bus_read_u8(win + kWinMode) | kModeBattle);
        g_mode_restore = win;
    }
}

// ── A room's day colours (map 77) ───────────────────────────────────────
// 0x080286EC(set) loads a room's palette set (rows 0-3, then 12 on) into the
// palette buffer 0x02002400. Its callers pick the set from the map header
// ([0x02029CC0]) by the time of day, the byte 0x020021BC: 0 the set at
// +0x20, 1-2 at +0x24, 3-4 at +0x28, each an offset into the table
// 0x085E710C (0xC0 a set). Map 77 after the flashback (states/thor/0157)
// loads its +0x24 set: the floor purple. The user asked for the normal colours
// (outside Original), with the lighting changed instead: on map 77, outside
// the Original preset, a later set is swapped for the +0x20 one and the call
// made again; the time of day stays as the game set it (kol_focus lights the
// room for the evening by it). KOL_NO_DAY_PALETTE=1: off.
constexpr std::uint32_t kRoomPaletteLoad = 0x080286ECu;
constexpr std::uint32_t kRoomPaletteTable = 0x085E710Cu;
constexpr std::uint32_t kMapHeaderPtr = 0x02029CC0u;
constexpr std::uint32_t kMapNumberAt = 0x02029D40u;
constexpr char kDayPaletteHookId[] = "kol.day-palette";
bool g_pass_day_palette = false;   // the call made again: let it run

std::uint32_t header_u32(std::uint32_t at) {   // (the game reads them a byte at a time)
    return bus_read_u8(at) | (bus_read_u8(at + 1) << 8) | (bus_read_u8(at + 2) << 16) |
           (static_cast<std::uint32_t>(bus_read_u8(at + 3)) << 24);
}

int on_room_palette(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_pass_day_palette) {
        g_pass_day_palette = false;
        return 0;
    }
    if (bus_read_u16(kMapNumberAt) != 77 || preset_current() == 1) return 0;   // (1: Original)
    const std::uint32_t hdr = bus_read_u32(kMapHeaderPtr);
    if (hdr < 0x02000000u) return 0;
    const std::uint32_t day = kRoomPaletteTable + header_u32(hdr + 0x20);
    const std::uint32_t set = cpu->R[0];
    if (set == day || (set != kRoomPaletteTable + header_u32(hdr + 0x24) &&
                       set != kRoomPaletteTable + header_u32(hdr + 0x28)))
        return 0;
    cpu->R[0] = day;
    cpu->R[15] = kRoomPaletteLoad;
    g_pass_day_palette = true;
    return 1;
}

int on_status_task(std::uint32_t, int, ArmCpuState* cpu) {
    tick_test();
    const std::uint32_t win = bus_read_u32(kWinPtr);
    if (!win) return 0;
    profile_check();
    if (!profile_off()) profile_status_task(win, cpu->R[1]);
    std::uint32_t unit = bus_read_u32(win + kWinUnit);
    if (g_roster_phase && bus_read_u8(win + kWinSub) == 0) {
        // Organize's grid: the unit under its cursor (the pointer is the
        // grid's scratch while it draws).
        const std::uint8_t index = bus_read_u8(win + kWinRecIndex);
        if (index < kArmySlots) unit = kArmy + index * kRecSize;
    }
    g_window_unit.store(unit, std::memory_order_relaxed);
    g_window_seen_ms.store(now_ms(), std::memory_order_relaxed);
    if (status_trace()) {
        static std::uint32_t last = ~0u;
        const std::uint32_t state = cpu->R[1];
        if (bus_read_u8(state) != last) {
            last = bus_read_u8(state);
            std::printf("status_task: call=%d st@%08X=%u unit=%08X\n", g_calls, state,
                        last, unit);
        }
    }
    return 0;
}

// The roster task, which draws the status window in the army roster and on
// the battle map. Its state byte (r1) is 3 while the window is up, with the
// control block's sub-state 1 while it is idle; the roster grid is state 4.
// With a tap pending there, it points the window at the neighbouring unit
// and sets its own state to 0, and its init draws the new unit exactly as it
// drew the first ("NO. n", portrait or sprite frame, items, talents). The
// battle window's sprite is refreshed on the next idle call (see above).
bool g_refresh_palette = false;
bool g_refresh_sprite = false;
std::uint32_t g_refresh_epoch = 0;

int on_roster_task(std::uint32_t, int, ArmCpuState* cpu) {
    tick_test();
    const std::uint32_t win = bus_read_u32(kWinPtr);
    if (!win) return 0;
    const std::uint32_t state = cpu->R[1];
    if (status_trace()) {
        static std::uint32_t last = ~0u;
        const std::uint32_t key = bus_read_u8(state) | bus_read_u8(win + kWinSub) << 8;
        if (key != last) {
            last = key;
            std::printf("roster_task: call=%d st@%08X=%u sub=%u unit=%08X\n", g_calls,
                        state, bus_read_u8(state), bus_read_u8(win + kWinSub),
                        bus_read_u32(win + kWinUnit));
        }
    }
    profile_check();
    if (!profile_off()) profile_roster_task(win, state);
    if (status_switch_off()) return 0;
    if (g_refresh_epoch != g_runtime_state_epoch) {
        g_refresh_epoch = g_runtime_state_epoch;
        g_refresh_palette = false;
        g_refresh_sprite = false;
    }
    if (bus_read_u8(state) != kRosterWindowUp || bus_read_u8(win + kWinSub) != 1)
        return 0;
    if (g_refresh_palette) {
        // The window is rebuilt for the new unit; now its colours, in place
        // of this one call of the task (R14 still holds the task's return).
        g_refresh_palette = false;
        g_refresh_sprite = true;
        cpu->R[0] = 0;
        cpu->R[15] = kLoadPalettes;
        return 1;
    }
    if (g_refresh_sprite) {
        // Then its picture, the same way.
        g_refresh_sprite = false;
        cpu->R[0] = bus_read_u8(win + kWinSpriteIndex);
        cpu->R[15] = kRefreshSprite;
        return 1;
    }
    const int dir = pending();
    if (!dir) return 0;
    const std::uint32_t unit = bus_read_u32(win + kWinUnit);
    std::uint32_t entry = 0;
    std::uint8_t pos = 0;
    const std::uint32_t listed = list_neighbour(win, unit, dir, &pos);
    const std::uint32_t next = listed ? listed : neighbour(unit, dir, &entry);
    if (!next) return 0;
    g_dir.store(0, std::memory_order_release);
    if (std::getenv("KOL_NEXT_UNIT_AT")) {
        std::printf("roster_unit: call=%d %08X -> %08X (entry %08X, was %08X)\n", g_calls,
                    unit, next, entry, bus_read_u32(win + kWinEntry));
        std::fflush(stdout);
    }
    bus_write_u32(win + kWinUnit, next);
    if (entry) bus_write_u32(win + kWinEntry, entry);
    if (listed) {
        bus_write_u8(win + kWinPos, pos);
        bus_write_u8(win + kWinRecIndex, static_cast<std::uint8_t>((next - kArmy) / kRecSize));
    }
    // As the window's open routine (0x0803A5E0) starts it: on the EQUIP page
    // (+0x61D6 = 0, +0x5838 = 0). Rebuilt on the EMBLEM page, its page and
    // slide fields disagreed, and each Right after that drew the lower cards
    // further off (states/DK_status_equip_broken).
    bus_write_u8(win + kWinPage, 0);
    bus_write_u16(win + kWinPageSlide, 0);
    bus_write_u8(state, kTaskBuild);
    g_refresh_palette = entry != 0;
    g_window_unit.store(next, std::memory_order_relaxed);
    return 0;   // the task runs as usual, from its init
}

// ── The controller ──────────────────────────────────────────────────────
// The GBA's R and L stay on the shoulders, as the game's own button prompts
// say. The unit buttons are the upper face buttons, which the GBA has no use
// for: the top one Shortcut A on the map and Next in the status window, the
// left one Shortcut B and Previous. SDL names them by Xbox position, but the
// Thor's pad reports its Nintendo labels (Vendor_2020_Product_0111.kl:
// BTN_NORTH -> BUTTON_X, BTN_WEST -> BUTTON_Y), so SDL's X is its top button
// and SDL's Y its left one. (Options > Shoulder Units put R and L on those
// and the units on the shoulders; removed 2026-11-17: the game's prompts
// named the wrong buttons.)
constexpr int kPadX = 2, kPadY = 3;                  // SDL_CONTROLLER_BUTTON_X/Y
constexpr int kPadA = 0, kPadB = 1;                  // SDL_CONTROLLER_BUTTON_A/B
constexpr int kBitA = 0, kBitB = 1;                  // KEYINPUT bits
std::atomic<bool> g_swap_face{false};   // Swap Face Buttons
// The sticks' clicks. L3 is the host's state combos' (L3 + R saves, L3 + L
// loads); the right stick's click alone opens the options menu
// (kol_desktop). The Save States page is a row of the menu and has no
// shortcut of its own (the two sticks pressed together opened it until
// 2026-11-18: taken out of every build, the user's call).
constexpr int kPadL3 = 7, kPadR3 = 8;   // SDL_CONTROLLER_BUTTON_LEFTSTICK/RIGHTSTICK
std::atomic<bool> g_l3{false};

void on_pad_button(int button, int down) {
    if (down) g_key_presses.fetch_add(1, std::memory_order_relaxed);
    if (button == kPadL3 || button == kPadR3) {
        if (button == kPadL3) g_l3.store(down != 0, std::memory_order_relaxed);
        // A click of the right stick alone opens the options menu, on every
        // platform (kol_desktop).
        if (button == kPadR3) desktop_r3(down, g_l3.load(std::memory_order_relaxed));
        else if (down) desktop_r3(0, true);   // L3 during the click: not a click
        return;
    }
    const bool swap = g_swap_face.load(std::memory_order_relaxed);
    const int first = swap ? kPadY : kPadX, second = swap ? kPadX : kPadY;
    const int which = button == first ? 1 : button == second ? 2 : 0;
    if (!down) {
        if (which && g_button_held.load(std::memory_order_relaxed) == which)
            g_button_held.store(0, std::memory_order_relaxed);
        return;
    }
    if (menu_is_open()) return;   // the second screen's menu has the pad
    if (!which) return;
    g_button_held.store(which, std::memory_order_relaxed);
    request(which == 1 ? 1 : -1);
}

}  // namespace

void pad_install() {
    gbarecomp::g_host_pad_button_hook = &on_pad_button;
}

void pad_swap_set(bool on) {
    gbarecomp::g_host_pad_binds[kBitA] = on ? kPadB : kPadA;
    gbarecomp::g_host_pad_binds[kBitB] = on ? kPadA : kPadB;
    g_swap_face.store(on, std::memory_order_relaxed);
}

namespace {

}  // namespace

// The move choice's NAVI prompt, answered as if A had closed it. Its state-9
// handler calls the text-box poll 0x0801670C at 0x080662F0 and moves on when
// that returns 0xFF (see game.toml). Only that caller is answered.
constexpr std::uint32_t kTextPoll = 0x0801670Cu;
constexpr std::uint32_t kShopBuyPollReturn = 0x08133A10u;   // the magic shop (kol_shop_buy.cpp)
constexpr std::uint32_t kMoveNaviReturn = 0x080662F4u;
constexpr char kMoveNaviHookId[] = "kol.skip-move-navi";
// The move choice's message state (0x08066B28): it polls the same routine,
// then closes the box and returns to the state it came from. A for a cell
// out of range puts up "Can't move to that spot." (bank 0x0875C8DE entry
// 0x1D, fetched from 0x0806657E). That is answered too; the second screen
// shows the text instead (move_message).
constexpr std::uint32_t kMoveMessageReturn = 0x08066B2Cu;
// The text fetch 0x08015D50(box, ?, bank, index): only recorded, to know
// which message the box would have shown.
constexpr std::uint32_t kTextFetch = 0x08015D50u;
constexpr char kTextFetchHookId[] = "kol.text-fetch-seen";
std::atomic<std::uint32_t> g_fetch_bank{0}, g_msg_bank{0};
std::atomic<unsigned> g_fetch_index{0}, g_msg_index{0}, g_msg_serial{0};
// Battle preparation's NAVI box, "To form an Attack Team, dispatch up to 5
// characters." (NAVI bank 0x0875D99A entry 0x10; the "5" is its figure 0).
// The preparation task (0x0807EC78, state byte ctl+0x2052, ctl = [0x0202B2A8])
// slides its unit list in during state 0 (0x0807ED24, frames left in u16
// ctl+0x2064). On the frame the count runs out it goes straight to state 2 if
// ctl+0x2053, the game's own "shown already" flag, is set; otherwise it shows
// the one-time tutorial (entry 0x1B) while flag 0x4A is clear and the battle
// mode (0x020012E0) is not 3 or 4, and else opens this box when the
// Navigation option (bit 0 of 0x0200132F) is on. On that last frame, when the
// box would open, the hook sets ctl+0x2053, so the game takes its no-box
// branch, and the second screen shows the text instead. The tutorial is left
// to the game. The figure is 5 in modes 3 and 4 and 8 otherwise, the game's
// own choice (0x08143054 / 0x08143070).
constexpr std::uint32_t kPrepTask = 0x0807EC78u;
constexpr std::uint32_t kPrepCtlPtr = 0x0202B2A8u;
constexpr std::uint32_t kPrepState = 0x2052u, kPrepNaviShown = 0x2053u, kPrepSlideLeft = 0x2064u;
constexpr std::uint32_t kOptionFlags = 0x0200132Fu;   // option byte 3: bit 0 Navigation ON
constexpr std::uint32_t kTutorialFlags = 0x020011D5u; // event flag 0x4A: bit 2 (0x020011CC + 9)
constexpr std::uint32_t kBattleMode = 0x020012E0u;
constexpr char kPrepNaviHookId[] = "kol.prep-navi";
constexpr std::uint32_t kNaviBank = 0x0875D99Au;
constexpr unsigned kPrepNaviEntry = 0x10u;
// Battle preparation's warnings. A on a unit in the preparation screen
// (state 2) may pick a warning instead of dispatching it: bank 0x0875C8DE
// entry 0x2D "No more characters can be dispatched." with the team full, or
// 0x2C. The task then opens a box with it (fetch 0x08015D50 from 0x0807F1FC,
// window set-up 0x08019088(1,1) from 0x0807F204, a first poll from 0x0807F208
// whose result it ignores) and goes to state 1, which only waits on the box
// (poll from 0x0807EF98) and then goes back to state 2. For that caller the
// hooks skip the fetch and the set-up, so no box is ever opened and the text
// system is untouched, answer the first poll 0, and answer state 1's first
// poll "closed": the next frame the task is back in state 2, having done
// nothing, and the second screen shows the text instead.
constexpr std::uint32_t kPrepWarnFetchReturn = 0x0807F200u;
constexpr std::uint32_t kPrepWarnOpenReturn = 0x0807F208u;
constexpr std::uint32_t kPrepWarnPollReturn = 0x0807F20Cu;
constexpr std::uint32_t kPrepWaitPollReturn = 0x0807EF9Cu;
constexpr std::uint32_t kTextWindow = 0x08019088u;
constexpr char kTextWindowHookId[] = "kol.prep-warning-window";
std::uint64_t g_warn_epoch = 0;
bool g_warn_pending = false;   // a skipped warning; state 1 still to be let go
std::atomic<unsigned> g_navi_serial{0};
std::atomic<std::uint32_t> g_navi_bank{kNaviBank};
std::atomic<unsigned> g_navi_index{0};
std::atomic<int> g_navi_number{-1};

bool warn_pending() {
    return g_warn_pending && g_warn_epoch == g_runtime_state_epoch;
}

// Random battles on the world map. On arriving somewhere the game picks an
// event with little scripts (bank 0x08765D54, interpreter 0x080213B4, ops
// 0x6D-0x94 through the table at 0x0815F75C): group 0 is the place's own,
// then groups 1 and 2 are global. Group 2 is the random battles: per story
// stage a 20% roll (op 0x8F, routine 0x08021A40(ctx, params): when
// rand() % 101 > params[1] the script jumps by params[2], past the battle),
// then the range checks, scene 0x44, the battle's choice (op 0x73) and op
// 0x93 (0x0801FA78), which starts it. With "No random battles" on, the hook
// takes the roll's jump for group 2 (ctx byte 0 == 2) exactly as the game
// does when it fails; other scripts' rolls run as always.
constexpr std::uint32_t kRandomChance = 0x08021A40u;
constexpr char kNoEncounterHookId[] = "kol.no-random-battles";

int on_random_chance(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t ctx = cpu->R[0], params = cpu->R[1];
    if (!setting_on(Setting::DebugNoEncounters) || bus_read_u8(ctx) != 2u) return 0;
    const auto jump = static_cast<std::int8_t>(bus_read_u8(params + 2));
    bus_write_u16(ctx + 2, static_cast<std::uint16_t>(bus_read_u16(ctx + 2) + jump));
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the interpreter, as its bl expects
    return 1;
}

// The battle a world map script starts (op 0x93, 0x0801FA78) takes its map
// from 0x08118B4C(place, n): a table of 12 map numbers a place at
// 0x08768BCC (255 none; n + 6 under a story flag), its only caller. Training
// starts its battle the same way. With Debug > Battle Map set, the hook
// answers that map instead, and the battle is fought on it: the units'
// starting places stay the game's, wherever they land.
// Set when a scene loaded the debug map (see debug_scene_camera_tick).
std::atomic<bool> g_scene_map_loaded{false};
std::uint32_t g_scene_map_epoch = 0;
constexpr std::uint32_t kBattleMapPick = 0x08118B4Cu;
constexpr char kBattleMapHookId[] = "kol.debug-battle-map";

int on_battle_map_pick(std::uint32_t, int, ArmCpuState* cpu) {
    const unsigned map = debug_map_current();
    if (map == 0 || !debug_map_in_training(map)) return 0;   // a scene map: the game's pick
    std::fprintf(stderr, "actions: battle map %u (the game's pick declined)\n", map);
    g_scene_map_loaded = false;   // a battle: the D-pad is the game's
    cpu->R[0] = map;
    cpu->R[15] = cpu->R[14] & ~1u;
    return 1;
}

// A story scene loads its map through 0x0811C77C, which hands the number
// at 0x02029D40 to the header parser. Scene maps lay their layers out where
// the battle interface lives (screen bases 0x16/0x14/0x13/0x12), so a
// battle cannot show them; a scene can. With Debug > Battle Map set, the
// number is written first and the game's loader runs as always.
constexpr std::uint32_t kSceneMapLoad = 0x0811C77Cu;
constexpr std::uint32_t kMapNumberAddr = 0x02029D40u;
constexpr char kSceneMapHookId[] = "kol.debug-scene-map";

int on_scene_map_load(std::uint32_t, int, ArmCpuState*) {
    const unsigned map = debug_map_current();
    if (map == 0) return 0;
    std::fprintf(stderr, "actions: scene map %u (was %u)\n", map, bus_read_u16(kMapNumberAddr));
    bus_write_u16(kMapNumberAddr, static_cast<std::uint16_t>(map));
    g_scene_map_epoch = g_runtime_state_epoch;
    g_scene_map_loaded = true;
    return 0;   // the loader runs, on the chosen map
}

// Every map the game loads goes through its header parser 0x08021B68 (it
// reads the map number at 0x02029D40). Its callers: the battle's map load
// 0x08047A4C (four calls; a Training battle, states/HG, came through
// 0x08047CB4), the story scene loader 0x0811C77C (0x0811C780), and two
// reloads of a map already open: 0x080482AC after View List (states/FN,
// 0x0804846C) and 0x08048774 (0x080488F0, not seen yet). A load from the
// first two is a map opened (the user, 2026-10-27: the wetness level
// resets whenever a map closes): counted for effects that
// start over with a map. The reloads are the same map going on: not
// counted (a look at the unit list would have dried the battle). The hook
// declines. KOL_MAP_LOAD_LOG=1: each call, its map number and caller.
constexpr std::uint32_t kMapHeader = 0x08021B68u;
constexpr char kMapHeaderHookId[] = "kol.map-loads";
constexpr std::uint32_t kMapOpens[] = {0x08047ABAu, 0x08047BE8u, 0x08047CB4u, 0x08047D3Cu, 0x0811C780u};
std::atomic<unsigned> g_map_loads{0};

int on_map_header(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t caller = (cpu->R[14] & ~1u) - 4u;   // the bl
    bool opens = false;
    for (const std::uint32_t c : kMapOpens) opens = opens || c == caller;
    const unsigned n = opens ? g_map_loads.fetch_add(1, std::memory_order_relaxed) + 1
                             : g_map_loads.load(std::memory_order_relaxed);
    static const bool log = [] {
        const char* e = std::getenv("KOL_MAP_LOAD_LOG");
        return e && e[0] && e[0] != '0';
    }();
    if (log)
        std::fprintf(stderr, "actions: map %s %u, map %u, from %08X\n", opens ? "load" : "reload (not counted)", n,
                     bus_read_u16(kMapNumberAddr), caller);
    return 0;
}
unsigned map_loads() { return g_map_loads.load(std::memory_order_relaxed); }

// Scene op 23 (0x0811978C) joins an actor's routes and walks it; on a map
// the scene was not written for the routes overflow its 16 steps and it
// hangs on purpose. With Debug > Battle Map set it answers its own "no such
// actor" result (0x110): the actor stays, the script goes on.
constexpr std::uint32_t kSceneWalk = 0x0811978Cu;
constexpr char kSceneWalkHookId[] = "kol.debug-scene-walk";

int on_scene_walk(std::uint32_t, int, ArmCpuState* cpu) {
    if (debug_map_current() == 0) return 0;
    cpu->R[0] = 0x110u;
    cpu->R[15] = cpu->R[14] & ~1u;
    return 1;
}

int on_text_fetch(std::uint32_t, int, ArmCpuState* cpu) {
    g_fetch_bank.store(cpu->R[2], std::memory_order_relaxed);
    g_fetch_index.store(cpu->R[3], std::memory_order_relaxed);
    const std::uint32_t ret = cpu->R[14] & ~1u;
    if (ret != kPrepWarnFetchReturn)
        return 0;   // the game fetches as always
    // A preparation warning: not fetched, never shown; the card has it.
    g_warn_pending = true;
    g_warn_epoch = g_runtime_state_epoch;
    g_navi_number.store(-1, std::memory_order_relaxed);
    g_navi_bank.store(cpu->R[2], std::memory_order_relaxed);
    g_navi_index.store(cpu->R[3], std::memory_order_relaxed);
    g_navi_serial.fetch_add(1, std::memory_order_release);
    cpu->R[15] = ret;
    return 1;
}

int on_text_window(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t ret = cpu->R[14] & ~1u;
    if (ret != kPrepWarnOpenReturn || !warn_pending())
        return 0;
    cpu->R[15] = ret;   // no window for the skipped warning
    return 1;
}

int on_prep_task(std::uint32_t, int, ArmCpuState*) {
    const std::uint32_t ctl = bus_read_u32(kPrepCtlPtr);
    // State 0, on the frame its slide-in ends (the count goes 1 -> 0 below).
    if (!ctl || bus_read_u8(ctl + kPrepState) != 0 || bus_read_u16(ctl + kPrepSlideLeft) != 1 ||
        bus_read_u8(ctl + kPrepNaviShown) != 0)
        return 0;
    const unsigned mode = bus_read_u8(kBattleMode);
    const bool small_team = mode == 3 || mode == 4;
    if (!small_team && !(bus_read_u8(kTutorialFlags) & 0x04u))
        return 0;   // the one-time tutorial comes next: the game's own
    if (!(bus_read_u8(kOptionFlags) & 1u))
        return 0;   // Navigation off: no box and no message, as the game does
    bus_write_u8(ctl + kPrepNaviShown, 1);   // the game's "shown already": no box
    g_navi_number.store(small_team ? 5 : 8, std::memory_order_relaxed);
    g_navi_bank.store(kNaviBank, std::memory_order_relaxed);
    g_navi_index.store(kPrepNaviEntry, std::memory_order_relaxed);
    g_navi_serial.fetch_add(1, std::memory_order_release);
    return 0;   // the task runs as always, and takes its no-box branch
}

int on_text_poll(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t ret = cpu->R[14] & ~1u;
    if (ret == kShopBuyPollReturn && shop_buy_answering()) {
        cpu->R[0] = 0xFFu;
        cpu->R[15] = ret;
        return 1;
    }
    if ((ret == kPrepWarnPollReturn || ret == kPrepWaitPollReturn) && warn_pending()) {
        // The skipped warning: its first poll is ignored by the task; state
        // 1's first poll finds it closed and goes back to state 2.
        if (ret == kPrepWaitPollReturn) g_warn_pending = false;
        cpu->R[0] = ret == kPrepWaitPollReturn ? 0xFFu : 0u;
        cpu->R[15] = ret;
        return 1;
    }
    if (ret == kMoveMessageReturn) {
        g_msg_bank.store(g_fetch_bank.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_msg_index.store(g_fetch_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_msg_serial.fetch_add(1, std::memory_order_release);
    } else if (ret != kMoveNaviReturn) {
        return 0;
    }
    cpu->R[0] = 0xFFu;
    cpu->R[15] = ret;   // back to the caller, as its bl expects
    return 1;
}

// Debug > Battle Map in a story scene: the D-pad moves the camera, so the
// whole map can be looked over (scene maps are only viewable in scenes).
// The map engine's camera is EWRAM 0x02026C78/7A (x, y; the scroll is it
// less a constant), a copy at +4, and at +8 the point it eases to. Moving
// the camera alone is undone the next frame (it is pulled back to that
// point), and moving it with its copy shows nothing (the scroll follows the
// step between them); moving the camera and the point a few pixels a frame
// scrolls at once and lets the game copy the map in as it goes (one big
// jump left part of the view unfilled). Only after the scene
// map hook ran (not in Training's battle, where the D-pad is the game's),
// in this run of the state (g_runtime_state_epoch).
constexpr std::uint32_t kCameraX = 0x02026C78u;
constexpr std::uint32_t kKeyInput = 0x04000130u;

// The viewer's tile cursor: a cell the D-pad steps (as the game's cursor
// does: up Z-1, down Z+1, right X+1, left X-1), -1 until kol_focus sets it
// from the view's middle; and the camera that centres it (kol_focus, from the
// projection, at the cell's drawn height), which the camera eases to.
// (kNoViewCell: none yet. Cells may be negative: the editor's field goes
// past the game's grid, kol_tile_edit.)
constexpr int kNoViewCell = -100000;
std::atomic<int> g_view_cx{kNoViewCell}, g_view_cz{kNoViewCell};
std::atomic<int> g_cam_goal_x{0}, g_cam_goal_y{0};
std::atomic<bool> g_cam_goal_valid{false};
// kx + camera (and ky + camera y): constant while the scene lasts, learned
// once with the camera still (kol_focus).
std::atomic<int> g_anchor_x{0}, g_anchor_y{0};
std::atomic<bool> g_anchor_valid{false};

bool debug_viewer_cell(int* cx, int* cz) {
    *cx = g_view_cx.load();
    *cz = g_view_cz.load();
    return *cx != kNoViewCell && *cz != kNoViewCell;
}

void debug_viewer_cell_init(int cx, int cz) {
    // KOL_VIEWER_CELL=x,z starts the cursor there (the map survey's shots);
    // =center at the middle of the grid's field cells (map byte not 0xFF).
    if (const char* s = std::getenv("KOL_VIEWER_CELL")) {
        if (std::strcmp(s, "center") == 0) {
            int x0 = 32, z0 = 32, x1 = -1, z1 = -1;
            for (int z = 0; z < 32; ++z)
                for (int x = 0; x < 32; ++x)
                    if (bus_read_u8(0x02016440u + static_cast<std::uint32_t>((z * 32 + x) * 4)) != 0xFF) {
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        z0 = std::min(z0, z); z1 = std::max(z1, z);
                    }
            if (x1 >= 0) { cx = (x0 + x1) / 2; cz = (z0 + z1) / 2; }
        } else {
            std::sscanf(s, "%d,%d", &cx, &cz);
        }
    }
    int unset = kNoViewCell;
    if (g_view_cx.compare_exchange_strong(unset, cx)) g_view_cz = cz;
}

void debug_viewer_cell_set(int cx, int cz) {
    g_view_cz = cz;
    g_view_cx = cx;
}

bool debug_viewer_anchor(int* ax, int* ay) {
    *ax = g_anchor_x.load();
    *ay = g_anchor_y.load();
    return g_anchor_valid.load();
}

void debug_viewer_anchor_set(int ax, int ay) {
    g_anchor_x = ax;
    g_anchor_y = ay;
    g_anchor_valid = true;
}

void debug_viewer_camera_goal(int x, int y) {
    g_cam_goal_x = x;
    g_cam_goal_y = y;
    g_cam_goal_valid = true;
}

bool debug_scene_viewer_active() {
    return debug_map_current() != 0 && g_scene_map_loaded.load(std::memory_order_relaxed) &&
           g_scene_map_epoch == g_runtime_state_epoch;
}

void debug_scene_camera_tick() {
    // The viewer shows the map alone: the dialogue's box (BG3), its text
    // (BG2) and the sprites (its portrait, and the scene's characters, who
    // stand where the scene put them on a map it was not written for) are
    // hidden while it is on, over the Debug > Hide switches; and the scene
    // never gets past its first line (kol_tile_edit keeps A, B, Select and
    // Start from the game). Switched only when the viewer turns on or off.
    static bool shown = false;
    const bool viewer = debug_scene_viewer_active();
    if (viewer != shown) {
        shown = viewer;
        unsigned mask = 0;
        for (int l = 0; l < 5; ++l)
            if (setting_on(static_cast<Setting>(static_cast<int>(Setting::DebugHideBG0) + l))) mask |= 1u << l;
        gba::g_ppu_debug_hide_layers = mask | (viewer ? 0x1Cu : 0u);
    }
    if (!viewer) {
        g_view_cx = kNoViewCell;
        g_view_cz = kNoViewCell;
        g_cam_goal_valid = false;
        g_anchor_valid = false;
    }
    if (!viewer || gbarecomp::runtime_excursion_active()) return;
    // The camera is the viewer's: the scene's own camera moves (its script
    // sets the point and the game eases toward it over a count of frames at
    // 0x02026C88, a rate at 0x02026C8C) are cancelled every frame, or they
    // fought the D-pad and the map's copy tore (EX: from frame ~420 of the
    // opening the script pans to 568,672).
    bus_write_u16(kCameraX + 0x10, 0);
    bus_write_u16(kCameraX + 0x14, 0);
    bus_write_u16(kCameraX + 8, bus_read_u16(kCameraX));
    bus_write_u16(kCameraX + 10, bus_read_u16(kCameraX + 2));
    // The D-pad steps the tile cursor: a tap one cell, held one more after
    // 14 frames and then every 6.
    static unsigned was = 0;
    static int held_frames = 0;
    const unsigned keys = ~bus_read_u16(kKeyInput) & 0xF0u;   // the D-pad
    held_frames = keys && keys == was ? held_frames + 1 : 0;
    const bool fire = keys && (keys != was || (held_frames >= 14 && (held_frames - 14) % 6 == 0));
    was = keys;
    int cx = g_view_cx.load(), cz = g_view_cz.load();
    if (fire && cx != kNoViewCell) {
        if (keys & 0x40u) --cz;   // up
        if (keys & 0x80u) ++cz;   // down
        if (keys & 0x10u) ++cx;   // right
        if (keys & 0x20u) --cx;   // left
        g_view_cx = std::clamp(cx, -kTileMargin, kTileGame + kTileMargin - 1);
        g_view_cz = std::clamp(cz, -kTileMargin, kTileGame + kTileMargin - 1);
    }
    // The camera eases to the goal: a quarter of the way a frame, 1..12 px,
    // the camera and its ease point together (see above).
    if (!g_cam_goal_valid.load()) return;
    const int cam[2] = {static_cast<std::int16_t>(bus_read_u16(kCameraX)),
                        static_cast<std::int16_t>(bus_read_u16(kCameraX + 2))};
    const int goal[2] = {g_cam_goal_x.load(), g_cam_goal_y.load()};
    for (int axis = 0; axis < 2; ++axis) {
        const int d = goal[axis] - cam[axis];
        if (!d) continue;
        const int mag = std::min(12, std::max(1, std::abs(d) / 4));
        const int move = d > 0 ? std::min(d, mag) : std::max(d, -mag);
        for (std::uint32_t o : {0u, 8u}) {
            const std::uint32_t at = kCameraX + o + 2u * axis;
            bus_write_u16(at, static_cast<std::uint16_t>(bus_read_u16(at) + move));
        }
    }
}

void actions_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_ACTIONS");
    if (off && off[0] && off[0] != '0') return;
    pad_install();
    if (!gba_mod_register_function_entry_plugin(kHookId, kHandler, 1,
                                                &on_handler) ||
        !gba_mod_set_function_hook_enabled(kHookId, 1))
        std::fprintf(stderr, "actions: next-unit hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kStatusHookId, kStatusTask, 1,
                                                &on_status_task) ||
        !gba_mod_set_function_hook_enabled(kStatusHookId, 1))
        std::fprintf(stderr, "actions: status-unit hook NOT installed\n");
    if (const char* np = std::getenv("KOL_NO_DAY_PALETTE"); !(np && np[0] && np[0] != '0'))
        if (!gba_mod_register_function_entry_plugin(kDayPaletteHookId, kRoomPaletteLoad, 1, &on_room_palette) ||
            !gba_mod_set_function_hook_enabled(kDayPaletteHookId, 1))
            std::fprintf(stderr, "actions: day-palette hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kRosterHookId, kRosterTask, 1,
                                                &on_roster_task) ||
        !gba_mod_set_function_hook_enabled(kRosterHookId, 1))
        std::fprintf(stderr, "actions: roster-status-unit hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kRoamHookId, kRoamTask, 1, &on_roam_task) ||
        !gba_mod_set_function_hook_enabled(kRoamHookId, 1) ||
        !gba_mod_register_function_entry_plugin(kCommandHookId, kCommandTask, 1,
                                                &on_command_task) ||
        !gba_mod_set_function_hook_enabled(kCommandHookId, 1) ||
        !gba_mod_register_function_entry_plugin(kCommandDrawHookId, kCommandDraw, 1,
                                                &on_command_draw) ||
        !gba_mod_set_function_hook_enabled(kCommandDrawHookId, 1) ||
        !gba_mod_register_function_entry_plugin(kSoundHookId, kPlaySound, 1, &on_play_sound) ||
        !gba_mod_set_function_hook_enabled(kSoundHookId, 1) ||
        !gba_mod_register_function_entry_plugin(kKeyReadHookId, kKeyRead, 1, &on_key_read) ||
        !gba_mod_set_function_hook_enabled(kKeyReadHookId, 1))
        std::fprintf(stderr, "actions: world-map profile hooks NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kRMenuDrawHookId, kRMenuDraw, 1, &on_rmenu_draw) ||
        !gba_mod_set_function_hook_enabled(kRMenuDrawHookId, 1))
        std::fprintf(stderr, "actions: R menu draw hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kNoEncounterHookId, kRandomChance, 1,
                                                &on_random_chance) ||
        !gba_mod_set_function_hook_enabled(kNoEncounterHookId, 1))
        std::fprintf(stderr, "actions: no-random-battles hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kBattleMapHookId, kBattleMapPick, 1,
                                                &on_battle_map_pick) ||
        !gba_mod_set_function_hook_enabled(kBattleMapHookId, 1))
        std::fprintf(stderr, "actions: battle-map hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kSceneMapHookId, kSceneMapLoad, 1,
                                                &on_scene_map_load) ||
        !gba_mod_set_function_hook_enabled(kSceneMapHookId, 1))
        std::fprintf(stderr, "actions: scene-map hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kMapHeaderHookId, kMapHeader, 1, &on_map_header) ||
        !gba_mod_set_function_hook_enabled(kMapHeaderHookId, 1))
        std::fprintf(stderr, "actions: map-load hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kSceneWalkHookId, kSceneWalk, 1, &on_scene_walk) ||
        !gba_mod_set_function_hook_enabled(kSceneWalkHookId, 1))
        std::fprintf(stderr, "actions: scene-walk hook NOT installed\n");
    const char* navi_off = std::getenv("KOL_NO_SKIP_MOVE_NAVI");
    if (!(navi_off && navi_off[0] && navi_off[0] != '0') &&
        (!gba_mod_register_function_entry_plugin(kPrepNaviHookId, kPrepTask, 1, &on_prep_task) ||
         !gba_mod_set_function_hook_enabled(kPrepNaviHookId, 1)))
        std::fprintf(stderr, "actions: prep-NAVI hook NOT installed\n");
    if (!(navi_off && navi_off[0] && navi_off[0] != '0') &&
        (!gba_mod_register_function_entry_plugin(kMoveNaviHookId, kTextPoll, 1,
                                                 &on_text_poll) ||
         !gba_mod_set_function_hook_enabled(kMoveNaviHookId, 1) ||
         !gba_mod_register_function_entry_plugin(kTextFetchHookId, kTextFetch, 1,
                                                 &on_text_fetch) ||
         !gba_mod_set_function_hook_enabled(kTextFetchHookId, 1)))
        std::fprintf(stderr, "actions: move-NAVI hooks NOT installed\n");
    if (!(navi_off && navi_off[0] && navi_off[0] != '0') &&
        (!gba_mod_register_function_entry_plugin(kTextWindowHookId, kTextWindow, 1,
                                                 &on_text_window) ||
         !gba_mod_set_function_hook_enabled(kTextWindowHookId, 1)))
        std::fprintf(stderr, "actions: prep-warning hook NOT installed\n");
}

void action_next_unit() { request(1); }
void action_cursor_to_object(int index) {
    if (index < 0 || index >= kObjCount) return;
    g_jump_ms.store(now_ms(), std::memory_order_relaxed);
    g_jump.store(index, std::memory_order_release);
}
void action_previous_unit() { request(-1); }
void action_open_profile(std::uint32_t record) {
    g_profile_request_ms.store(now_ms(), std::memory_order_relaxed);
    g_profile_record.store(record, std::memory_order_release);
}
void action_world_item(int item) {
    // Item, Spell, Class (Organize's R menu screens 4, 9, 15), Training, Shop.
    static constexpr int kNext[5] = {kCommandOrganize, kCommandOrganize, kCommandOrganize,
                                     kCommandTraining, kCommandShop};
    static constexpr int kScreen[5] = {4, 9, 15, 0, 0};
    if (item < 0 || item > 4) return;
    if (item <= 2 && action_organize_ready()) {   // already in Organize
        g_org_request_ms.store(now_ms(), std::memory_order_relaxed);
        g_org_request.store(item + 1, std::memory_order_release);
        return;
    }
    g_menu_screen.store(kScreen[item], std::memory_order_relaxed);
    g_menu_request_ms.store(now_ms(), std::memory_order_relaxed);
    g_menu_request.store(kNext[item], std::memory_order_release);
}
int action_world_items_ok() { return g_items_ok.load(std::memory_order_relaxed); }
void action_world_save() {
    g_menu_request_ms.store(now_ms(), std::memory_order_relaxed);
    g_menu_request.store(kCommandSave, std::memory_order_release);
}
void action_world_load() {
    g_menu_request_ms.store(now_ms(), std::memory_order_relaxed);
    g_menu_request.store(kCommandLoad, std::memory_order_release);
}
bool action_file_screen_open() {
    return g_file_open.load(std::memory_order_relaxed) &&
           now_ms() - g_file_seen_ms.load(std::memory_order_relaxed) < 500;
}
bool roster_open_now() {
    return g_roster_open.load(std::memory_order_relaxed) &&
           now_ms() - g_roster_seen_ms.load(std::memory_order_relaxed) < 500;
}
bool action_organize_ready() {
    return g_org_ready.load(std::memory_order_relaxed) && roster_open_now();
}
bool action_profile_busy() {
    // The flow is checked on the guest's hooks, which a scene the menu
    // hands over to (Training's battle) may not run: it is busy only while
    // the menu's task runs too (before it starts the world map is still up,
    // and the list with it).
    const bool flow = g_profile_busy.load(std::memory_order_relaxed) &&
                      now_ms() - g_command_seen_at.load(std::memory_order_relaxed) < 1000;
    return flow ||
           (roster_open_now() && g_roster_window.load(std::memory_order_relaxed));
}
std::uint32_t action_roster_unit() {
    const bool on = roster_open_now() || g_profile_busy.load(std::memory_order_relaxed);
    return on ? g_roster_unit.load(std::memory_order_relaxed) : 0;
}

unsigned navi_message(std::uint32_t* bank, unsigned* index, int* number) {
    const unsigned serial = g_navi_serial.load(std::memory_order_acquire);
    *bank = g_navi_bank.load(std::memory_order_relaxed);
    *index = g_navi_index.load(std::memory_order_relaxed);
    if (number) *number = g_navi_number.load(std::memory_order_relaxed);
    return serial;
}

unsigned key_presses() { return g_key_presses.load(std::memory_order_relaxed); }
void note_key_press() { g_key_presses.fetch_add(1, std::memory_order_relaxed); }

unsigned move_message(std::uint32_t* bank, unsigned* index) {
    const unsigned serial = g_msg_serial.load(std::memory_order_acquire);
    *bank = g_msg_bank.load(std::memory_order_relaxed);
    *index = g_msg_index.load(std::memory_order_relaxed);
    return serial;
}

std::uint32_t status_window_unit() {
    if (now_ms() - g_window_seen_ms.load(std::memory_order_relaxed) > 100) return 0;
    return g_window_unit.load(std::memory_order_relaxed);
}

}  // namespace kol
