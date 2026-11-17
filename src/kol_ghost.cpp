// kol_ghost.cpp — a blinking unit drawn half transparent instead.
//
// In battle preparation the unit on a deployment square blinks: every frame
// 0x080676EC(ctx) flips bit 0x40 of byte +0x17 of the unit's record
// ([[ctx+0x40]+4]), and the map's draw list (0x0805A77C) takes an object only
// while that bit is set. So the unit is drawn on one frame in two.
//
// The game can draw a unit semi-transparent itself: object flag 0x100 (u16 at
// +0x78) makes its sprite builder give every part OBJ mode 1 (0x0805A702,
// descriptor +8, copied into attr0 at 0x030059B4). Mode 1 blends only with
// the layers BLDCNT names as second targets, by BLDALPHA's weights, and
// battle preparation sets neither (BLDCNT 0000, BLDALPHA 0010: all sprite).
//
// So, through two reviewed hooks (game.toml), both declining so the game's
// own code runs:
// - at the flip: clear the bit first, so the flip always sets it (drawn every
//   frame); set the object's 0x100; and while the game blends nothing (no
//   effect, no first targets in the BLDCNT copy at 0x03000E1A, which the game
//   copies to the register) make BG0-3 and the backdrop second targets at 8/8:
//   half the unit, half what is behind it.
// - at the draw list: if the flip has not run since the last call, the blink
//   is over; put back what was changed, where it still holds our value.
//
// KOL_NO_GHOST=1 leaves the blink alone. See README "The blinking unit, half
// transparent".

#include "kol_ghost.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"

namespace kol {
namespace {

constexpr std::uint32_t kFlip = 0x080676ECu;       // flips the unit's draw bit
constexpr std::uint32_t kDrawList = 0x0805A77Cu;   // the map's unit draw list
constexpr std::uint32_t kCtxObject = 0x40u;        // ctx: u32 -> map object
constexpr std::uint32_t kObjRecord = 0x04u;        // object: u32 -> unit record
constexpr std::uint32_t kObjFlags = 0x78u;         // object: u16 flags
constexpr std::uint16_t kSemi = 0x100u;            // ... drawn semi-transparent
constexpr std::uint32_t kRecDraw = 0x17u;          // record: u8, bit 0x40 = drawn
constexpr std::uint8_t kDrawBit = 0x40u;
constexpr std::uint32_t kBldcnt = 0x03000E1Au;     // the game's copy of BLDCNT
constexpr std::uint32_t kBldalpha = 0x03000E1Cu;   // ... and of BLDALPHA
constexpr std::uint16_t kSecondTargets = 0x2F00u;  // BG0-3 and the backdrop
constexpr std::uint16_t kHalf = 0x0808u;           // EVA 8, EVB 8
constexpr char kFlipId[] = "kol.ghost-flip";
constexpr char kDrawId[] = "kol.ghost-draw";

bool in_ewram(std::uint32_t a) { return a >= 0x02000000u && a < 0x0203FF00u; }

// The unit made transparent, and what was changed for it.
std::uint32_t g_object = 0;
bool g_set_semi = false;        // its 0x100 was ours
bool g_set_blend = false;       // BLDCNT/BLDALPHA were ours
std::uint16_t g_old_bldcnt = 0, g_old_bldalpha = 0;
bool g_flipped = false;         // the flip ran since the last draw list
unsigned long long g_epoch = 0;   // a state load puts memory back by itself

void forget_on_state_load() {
    if (g_epoch == g_runtime_state_epoch) return;
    g_epoch = g_runtime_state_epoch;
    g_object = 0;
    g_set_semi = g_set_blend = false;
    g_flipped = false;
}

void restore() {
    if (g_object && g_set_semi && in_ewram(g_object)) {
        const std::uint16_t f = bus_read_u16(g_object + kObjFlags);
        bus_write_u16(g_object + kObjFlags, static_cast<std::uint16_t>(f & ~kSemi));
    }
    if (g_set_blend) {
        const std::uint16_t cnt = bus_read_u16(kBldcnt);
        const std::uint16_t alpha = bus_read_u16(kBldalpha);
        // Only where the game has not written its own since.
        if (cnt == (g_old_bldcnt | kSecondTargets)) bus_write_u16(kBldcnt, g_old_bldcnt);
        if (alpha == kHalf) bus_write_u16(kBldalpha, g_old_bldalpha);
    }
    std::fprintf(stderr, "ghost: the blink stopped; object %08X put back\n", g_object);
    g_object = 0;
    g_set_semi = g_set_blend = false;
}

int on_flip(std::uint32_t, int, ArmCpuState* cpu) {
    forget_on_state_load();
    const std::uint32_t ctx = cpu->R[0];
    if (!in_ewram(ctx) && !(ctx >= 0x03000000u && ctx < 0x03008000u)) return 0;
    const std::uint32_t object = bus_read_u32(ctx + kCtxObject);
    if (!in_ewram(object)) return 0;
    const std::uint32_t record = bus_read_u32(object + kObjRecord);
    if (!in_ewram(record)) return 0;
    if (g_object && g_object != object) restore();   // another unit now
    g_flipped = true;
    // The flip below then always sets the bit.
    const std::uint8_t draw = bus_read_u8(record + kRecDraw);
    bus_write_u8(record + kRecDraw, static_cast<std::uint8_t>(draw & ~kDrawBit));
    const std::uint16_t flags = bus_read_u16(object + kObjFlags);
    if (!(flags & kSemi)) {
        bus_write_u16(object + kObjFlags, static_cast<std::uint16_t>(flags | kSemi));
        if (!g_object) g_set_semi = true;
    }
    if (!g_object)
        std::fprintf(stderr, "ghost: object %08X drawn half transparent, not blinking\n", object);
    g_object = object;
    const std::uint16_t cnt = bus_read_u16(kBldcnt);
    if (!g_set_blend && (cnt & 0x00FFu) == 0) {   // no effect, no first targets
        g_old_bldcnt = cnt;
        g_old_bldalpha = bus_read_u16(kBldalpha);
        bus_write_u16(kBldcnt, static_cast<std::uint16_t>(cnt | kSecondTargets));
        bus_write_u16(kBldalpha, kHalf);
        g_set_blend = true;
    }
    return 0;   // the game's own function runs, and flips
}

int on_draw_list(std::uint32_t, int, ArmCpuState*) {
    forget_on_state_load();
    if (g_object && !g_flipped) restore();
    g_flipped = false;
    return 0;
}

}  // namespace

void ghost_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_GHOST");
    if (off && off[0] && off[0] != '0') return;
    if (!gba_mod_register_function_entry_plugin(kFlipId, kFlip, 1, &on_flip) ||
        !gba_mod_set_function_hook_enabled(kFlipId, 1) ||
        !gba_mod_register_function_entry_plugin(kDrawId, kDrawList, 1, &on_draw_list) ||
        !gba_mod_set_function_hook_enabled(kDrawId, 1))
        std::fprintf(stderr, "ghost: blinking-unit hooks NOT installed\n");
}

}  // namespace kol
