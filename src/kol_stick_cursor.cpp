// kol_stick_cursor.cpp — the left stick moves the hand cursor freely.
//
// The battle map's cursor (IWRAM 0x03000FA0) stands on a cell: x at +0x3C,
// z at +0x40 (u16). Its task 0x08056E90 runs once a frame; with a direction
// held (the held keys at +0x30, the repeat pulse at +0x2E) it sets the mode
// +0x35 to 1 and calls the step 0x08056F90, which copies the cell (the u32s at
// +0x3C and +0x40) to +0x44/+0x48 and writes the new one, moving only onto a
// cell whose flags have bit 0x20 (cell (x, z) is the u32 at
// [0x02029CCC] + (x + 32 z) * 4: +1 height, +2 flags; the map is [0x02029CC0]
// +6 wide and +8 deep). Everything else follows the cell: the camera, the
// terrain box, the unit under the cursor. Then 0x08057768 (x, z) draws it,
// from the cell's world point (X = 32x + 16, Z = 32z + 16, Y = 16 height):
// the floating hand through 0x08002394 (screen x, y; returning to 0x08057874)
// and the corner pieces on the cell through 0x0807696C (routine 0x08057AD5,
// owner, X, Y, [sp] Z; returning to 0x08057900), which hands the point to the
// sprite emitter 0x08076810: it projects it (screen x = (X - Z) >> 1, y =
// ((X + Z) >> 2) - (Y >> 1), plus the camera) and stores it in the sprite slot.
//
// Three reviewed hooks (game.toml):
// - the task: the stick's push, turned from screen pixels into the world,
//   moves a point P; when P crosses into another cell the hook writes the cell
//   the way the step does. Only in the task's ordinary state with the cursor
//   out (mode 1); a direction on the pad, a glide or anything else that moves
//   the cell takes over, and P goes back to the cell's middle. A cell the step
//   would not enter blocks P (it slides along the other axis). While the
//   camera pans to the cursor (+0x95 is 1; the task skips the pad's step then)
//   P stays inside its cell. Let go, P eases to the middle of its cell.
//   Declines: the game's task runs as it is.
// - the hand's and the corner pieces' draw calls: while P is off its cell's
//   middle, the call is made again with P's point (the hand moved by the same
//   pixels as the corner pieces), so the game projects and depth-sorts it.
//   The height eases from cell to cell instead of jumping.
// With P on its cell's middle (the stick never touched) all three decline and
// the game runs untouched.
//
// The world map has a free cursor of its own, the sword: its roaming task
// 0x08128D9C, in state 4 (its first argument's byte), moves it 3 px a frame
// for each direction held (control [0x02029C5C], +0x1E x, +0x1F y, screen
// pixels, kept to 8..232 and 8..152), then 0x0812A268(&x, &y) pulls it a
// pixel a frame on each axis toward the middle of a town within 24 px (the
// town under it is chosen once it sits there), then draws it. A fourth hook,
// there (declining): while the stick is pushed and no direction held, the
// sword's place is the stick's, kept in fractions of a pixel and written
// before the task runs, so the pull nudges it a pixel at most and a slow
// push still leaves a town. Let go, the game's own pull takes it onto a town
// that close, as after the pad.
//
// Options > Analog Cursor; KOL_NO_STICK_CURSOR=1 leaves the hooks out.
// KOL_STICK_TEST="a-b:x,y;..." holds the stick at (x, y) from the a-th call of
// either task after a state load to its b-th (headless runs); KOL_STICK_TRACE=1 logs
// each frame. See README "The left stick moves the hand".

#include "kol_stick_cursor.h"

#include "kol_settings.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"

namespace kol {
namespace {

constexpr std::uint32_t kTask = 0x08056E90u;       // the cursor's task, once a frame
constexpr std::uint32_t kHandDraw = 0x08002394u;   // draws a sprite at (r1, r2)
constexpr std::uint32_t kPieceDraw = 0x0807696Cu;  // emits a sprite slot (r2 X, r3 Y, [sp] Z)
constexpr std::uint32_t kHandReturn = 0x08057875u;   // LR of cursor_draw's hand call
constexpr std::uint32_t kPieceReturn = 0x08057901u;  // ... and of its corner pieces'
constexpr std::uint32_t kPieceFn = 0x08057AD5u;      // the corner pieces' slot routine
constexpr std::uint32_t kDrawArgX = 0x88u;          // cursor_draw's x, z on its stack
constexpr std::uint32_t kDrawArgZ = 0x8Cu;

constexpr std::uint32_t kCursor = 0x03000FA0u;
constexpr std::uint32_t kHeld = 0x30u;              // u16 keys held
constexpr std::uint32_t kMode = 0x35u;              // 1 out under the pad, 4 gliding
constexpr std::uint32_t kLock = 0x36u;              // 0xFF: the step refuses
constexpr std::uint32_t kCellX = 0x3Cu, kCellZ = 0x40u;
constexpr std::uint32_t kPrevX = 0x44u, kPrevZ = 0x48u;
constexpr std::uint32_t kViewMode = 0x51u;          // 1 in preparation's view mode (L)
constexpr std::uint32_t kPanning = 0x95u;           // 1 while the camera pans to the cursor
constexpr std::uint8_t kModeFree = 1;
constexpr std::uint32_t kCellsPtr = 0x02029CCCu;    // u32 -> 32 x 32 cells of 4 bytes
constexpr std::uint32_t kMapPtr = 0x02029CC0u;      // u32 -> map header (+6 width, +8 depth)
constexpr std::uint8_t kCursorMay = 0x20u;          // cell flag the step requires

constexpr char kTaskId[] = "kol.stick-cursor-task";
constexpr char kHandId[] = "kol.stick-cursor-hand";
constexpr char kPieceId[] = "kol.stick-cursor-pieces";
constexpr char kWorldId[] = "kol.stick-cursor-world";

constexpr std::uint32_t kRoamTask = 0x08128D9Cu;    // the world map's roaming task
constexpr std::uint8_t kRoamPad = 4;                // its state that reads the pad
constexpr std::uint32_t kWorldMap = 0x02029C5Cu;    // u32 -> the world map's control
constexpr std::uint32_t kSwordX = 0x1Eu, kSwordY = 0x1Fu;   // u8 screen pixels
constexpr std::uint32_t kWorldKeys = 0x03000820u;   // u16 keys held, as the task reads them
constexpr float kSwordTop = 4.0f;                   // px a frame at full tilt (the pad: 3)

// The stick: no push inside kDead, full speed from kFull; the speed grows as
// the square of the push past the dead zone, so a small push is precise.
// kTopSpeed is in screen pixels a frame (the pad's held repeat goes a cell,
// 16 across and 8 down, every 3 frames: about 6).
constexpr float kDead = 0.2f, kFull = 0.95f;
constexpr float kTopSpeed = 5.0f;
constexpr float kSettle = 0.3f;     // of the way to the cell's middle, a frame
constexpr float kRise = 0.35f;      // of the way to the cell's height, a frame
constexpr float kNear = 0.75f;      // world units: there

bool in_ewram(std::uint32_t a) { return a >= 0x02000000u && a < 0x02040000u; }

// P and the height drawn, world units.
float g_px = 0.0f, g_pz = 0.0f, g_py = 0.0f;
int g_cx = -1, g_cz = -1;        // the cell P belongs to, as last seen or written
bool g_off = false;               // P (or the height drawn) is off the cell's point
bool g_pass_hand = false, g_pass_piece = false;   // the call made again: let it run
unsigned long long g_epoch = ~0ull;
long g_calls = 0;                 // task calls (either task) since the state load (tests)

struct TestHold { long a, b; float x, y; };
std::vector<TestHold> g_test;
bool g_trace = false;

float centre(int c) { return static_cast<float>(c * 32 + 16); }

// The cell's flags and height, or false where the step would not go.
bool cell_at(int x, int z, std::uint8_t* height) {
    const std::uint32_t map = bus_read_u32(kMapPtr);
    const std::uint32_t cells = bus_read_u32(kCellsPtr);
    // The header is in the cartridge, the cells in EWRAM.
    if (!(map >= 0x08000000u && map < 0x08800000u) && !in_ewram(map)) return false;
    if (!in_ewram(cells)) return false;
    const int w = bus_read_u16(map + 6), d = bus_read_u16(map + 8);
    if (x < 0 || z < 0 || x >= w || z >= d || x > 31 || z > 31) return false;
    const std::uint32_t e = cells + static_cast<std::uint32_t>(x + z * 32) * 4u;
    if (!(bus_read_u8(e + 2) & kCursorMay)) return false;
    if (height) *height = bus_read_u8(e + 1);
    return true;
}

float cell_y(int x, int z) {
    std::uint8_t h = 0;
    const std::uint32_t cells = bus_read_u32(kCellsPtr);
    if (in_ewram(cells) && x >= 0 && z >= 0 && x < 32 && z < 32)
        h = bus_read_u8(cells + static_cast<std::uint32_t>(x + z * 32) * 4u + 1);
    return static_cast<float>(h * 16);
}

int floor32(float v) { return static_cast<int>(std::floor(v / 32.0f)); }

void settle_now() {
    g_px = centre(g_cx);
    g_pz = centre(g_cz);
    g_py = cell_y(g_cx, g_cz);
    g_off = false;
}

void stick(float* x, float* y) {
    *x = gbarecomp::g_host_pad_left_stick[0];
    *y = gbarecomp::g_host_pad_left_stick[1];
    for (const TestHold& t : g_test)
        if (g_calls >= t.a && g_calls <= t.b) {
            *x = t.x;
            *y = t.y;
        }
}

// The world map's sword (see the top): its place while the stick drives it.
float g_wx = 0.0f, g_wy = 0.0f;
bool g_world_on = false;

void forget_on_state_load() {
    if (g_epoch == g_runtime_state_epoch) return;
    g_epoch = g_runtime_state_epoch;
    g_cx = g_cz = -1;
    g_off = false;
    g_calls = 0;
    g_world_on = false;
}

int on_world(std::uint32_t, int, ArmCpuState* cpu) {
    forget_on_state_load();
    ++g_calls;
    const std::uint32_t wm = bus_read_u32(kWorldMap);
    if (!setting_on(Setting::AnalogCursor) || bus_read_u8(cpu->R[1]) != kRoamPad ||
        !((wm >= 0x03000000u && wm < 0x03008000u) || in_ewram(wm)) ||
        (bus_read_u16(kWorldKeys) & 0x00F0u)) {
        g_world_on = false;
        return 0;
    }
    float sx = 0.0f, sy = 0.0f;
    stick(&sx, &sy);
    const float len = std::sqrt(sx * sx + sy * sy);
    const float push = len <= kDead ? 0.0f : std::fmin(1.0f, (len - kDead) / (kFull - kDead));
    if (push == 0.0f) {   // the game's own pull from here
        if (g_trace && g_world_on)
            std::printf("stick_cursor: world call=%ld let go at (%u,%u)\n", g_calls,
                        bus_read_u8(wm + kSwordX), bus_read_u8(wm + kSwordY));
        g_world_on = false;
        return 0;
    }
    if (!g_world_on) {
        g_world_on = true;
        g_wx = bus_read_u8(wm + kSwordX);
        g_wy = bus_read_u8(wm + kSwordY);
    }
    const float speed = kSwordTop * push * push / len;
    g_wx = std::fmin(232.0f, std::fmax(8.0f, g_wx + sx * speed));
    g_wy = std::fmin(152.0f, std::fmax(8.0f, g_wy + sy * speed));
    bus_write_u8(wm + kSwordX, static_cast<std::uint8_t>(std::lround(g_wx)));
    bus_write_u8(wm + kSwordY, static_cast<std::uint8_t>(std::lround(g_wy)));
    if (g_trace)
        std::printf("stick_cursor: world call=%ld stick=(%.2f,%.2f) sword=(%.1f,%.1f)\n",
                    g_calls, sx, sy, g_wx, g_wy);
    return 0;
}

int on_task(std::uint32_t, int, ArmCpuState*) {
    forget_on_state_load();
    ++g_calls;
    g_pass_hand = g_pass_piece = false;
    const int cx = bus_read_u16(kCursor + kCellX), cz = bus_read_u16(kCursor + kCellZ);
    if (cx != g_cx || cz != g_cz) {   // the game moved it (the pad, a glide, a jump)
        g_cx = cx;
        g_cz = cz;
        settle_now();
    }
    const std::uint16_t held = bus_read_u16(kCursor + kHeld);
    const bool ours = setting_on(Setting::AnalogCursor) &&
                      bus_read_u8(kCursor + kMode) == kModeFree &&
                      bus_read_u8(kCursor + kLock) != 0xFFu &&
                      // No direction, and none of A, Select, Start, R, L:
                      // the pad's step path refuses on those (0x030D).
                      !(held & 0x03FDu) &&
                      cell_at(cx, cz, nullptr);
    if (!ours) {
        if (g_trace)
            std::printf("stick_cursor: call=%ld not ours: on=%d mode=%u lock=%u view=%u "
                        "held=%04X cell(%d,%d) ok=%d\n",
                        g_calls, setting_on(Setting::AnalogCursor) ? 1 : 0,
                        bus_read_u8(kCursor + kMode), bus_read_u8(kCursor + kLock),
                        bus_read_u8(kCursor + kViewMode), held, cx, cz,
                        cell_at(cx, cz, nullptr) ? 1 : 0);
        if (g_off) settle_now();
        return 0;
    }
    float sx = 0.0f, sy = 0.0f;
    stick(&sx, &sy);
    const float len = std::sqrt(sx * sx + sy * sy);
    const float push = len <= kDead ? 0.0f : std::fmin(1.0f, (len - kDead) / (kFull - kDead));
    if (push > 0.0f) {
        // Screen pixels to the world: x = (X - Z) / 2, y = (X + Z) / 4.
        const float speed = kTopSpeed * push * push / len;
        const float vx = sx * speed, vy = sy * speed;
        const float dX = vx + 2.0f * vy, dZ = 2.0f * vy - vx;
        // While the camera pans the pad waits (the task skips the step), and
        // so does the cell: P moves on inside it.
        const bool panning = bus_read_u8(kCursor + kPanning) != 0;
        auto may = [&](float x, float z) {
            const int nx = floor32(x), nz = floor32(z);
            return (nx == g_cx && nz == g_cz) || (!panning && cell_at(nx, nz, nullptr));
        };
        if (may(g_px + dX, g_pz + dZ)) {
            g_px += dX;
            g_pz += dZ;
        } else if (may(g_px + dX, g_pz)) {
            g_px += dX;
        } else if (may(g_px, g_pz + dZ)) {
            g_pz += dZ;
        }
        g_off = true;
        const int nx = floor32(g_px), nz = floor32(g_pz);
        if (nx != g_cx || nz != g_cz) {   // into the next cell, as the step goes
            bus_write_u32(kCursor + kPrevX, bus_read_u32(kCursor + kCellX));
            bus_write_u32(kCursor + kPrevZ, bus_read_u32(kCursor + kCellZ));
            bus_write_u16(kCursor + kCellX, static_cast<std::uint16_t>(nx));
            bus_write_u16(kCursor + kCellZ, static_cast<std::uint16_t>(nz));
            g_cx = nx;
            g_cz = nz;
        }
    } else if (g_off) {
        g_px += (centre(g_cx) - g_px) * kSettle;
        g_pz += (centre(g_cz) - g_pz) * kSettle;
    }
    if (g_off) {
        const float y = cell_y(g_cx, g_cz);
        g_py += (y - g_py) * kRise;
        if (push == 0.0f && std::fabs(g_px - centre(g_cx)) < kNear &&
            std::fabs(g_pz - centre(g_cz)) < kNear && std::fabs(g_py - y) < kNear)
            settle_now();
    }
    if (g_trace)
        std::printf("stick_cursor: call=%ld stick=(%.2f,%.2f) P=(%.1f,%.1f,%.1f) cell=(%d,%d) off=%d\n",
                    g_calls, sx, sy, g_px, g_py, g_pz, g_cx, g_cz, g_off ? 1 : 0);
    return 0;
}

// The point drawn, rounded, and the screen offset from the cell's own.
struct Point { int X, Y, Z, dx, dy; };
bool drawn_point(const ArmCpuState* cpu, Point* p) {
    if (!g_off || !setting_on(Setting::AnalogCursor)) return false;
    const std::uint32_t sp = cpu->R[13];
    if (bus_read_u16(sp + kDrawArgX) != g_cx || bus_read_u16(sp + kDrawArgZ) != g_cz) return false;
    const int X0 = g_cx * 32 + 16, Z0 = g_cz * 32 + 16;
    const int Y0 = static_cast<int>(cell_y(g_cx, g_cz));
    p->X = static_cast<int>(std::lround(g_px));
    p->Z = static_cast<int>(std::lround(g_pz));
    p->Y = static_cast<int>(std::lround(g_py));
    p->dx = ((p->X - p->Z) >> 1) - ((X0 - Z0) >> 1);
    p->dy = (((p->X + p->Z) >> 2) - (p->Y >> 1)) - (((X0 + Z0) >> 2) - (Y0 >> 1));
    return true;
}

int on_hand(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_pass_hand) {
        g_pass_hand = false;
        return 0;
    }
    Point p;
    if (cpu->R[14] != kHandReturn || !drawn_point(cpu, &p)) return 0;
    cpu->R[1] += static_cast<std::uint32_t>(p.dx);
    cpu->R[2] += static_cast<std::uint32_t>(p.dy);
    cpu->R[15] = kHandDraw;   // the same call again, with the moved place
    g_pass_hand = true;
    return 1;
}

int on_pieces(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_pass_piece) {
        g_pass_piece = false;
        return 0;
    }
    Point p;
    if (cpu->R[14] != kPieceReturn || cpu->R[0] != kPieceFn || cpu->R[1] != kCursor ||
        cpu->R[2] != static_cast<std::uint32_t>(g_cx * 32 + 16) || !drawn_point(cpu, &p))
        return 0;
    cpu->R[2] = static_cast<std::uint32_t>(p.X);
    cpu->R[3] = static_cast<std::uint32_t>(p.Y);
    bus_write_u32(cpu->R[13], static_cast<std::uint32_t>(p.Z));
    cpu->R[15] = kPieceDraw;
    g_pass_piece = true;
    return 1;
}

void parse_test() {
    const char* e = std::getenv("KOL_STICK_TEST");
    while (e && *e) {
        TestHold t{};
        if (std::sscanf(e, "%ld-%ld:%f,%f", &t.a, &t.b, &t.x, &t.y) == 4) g_test.push_back(t);
        while (*e && *e != ';') ++e;
        if (*e == ';') ++e;
    }
    const char* tr = std::getenv("KOL_STICK_TRACE");
    g_trace = tr && tr[0] && tr[0] != '0';
}

}  // namespace

void stick_cursor_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_STICK_CURSOR");
    if (off && off[0] && off[0] != '0') return;
    parse_test();
    if (!gba_mod_register_function_entry_plugin(kTaskId, kTask, 1, &on_task) ||
        !gba_mod_set_function_hook_enabled(kTaskId, 1) ||
        !gba_mod_register_function_entry_plugin(kHandId, kHandDraw, 1, &on_hand) ||
        !gba_mod_set_function_hook_enabled(kHandId, 1) ||
        !gba_mod_register_function_entry_plugin(kPieceId, kPieceDraw, 1, &on_pieces) ||
        !gba_mod_set_function_hook_enabled(kPieceId, 1) ||
        !gba_mod_register_function_entry_plugin(kWorldId, kRoamTask, 1, &on_world) ||
        !gba_mod_set_function_hook_enabled(kWorldId, 1))
        std::fprintf(stderr, "stick_cursor: analog cursor hooks NOT installed\n");
}

}  // namespace kol
