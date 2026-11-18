#include "kol_gulls.h"
#include "kol_perspective.h"
#include "kol_settings.h"

// The gulls crossing the harbour at the start of map 97's boat scene
// (states/HJ) are scene actors moved along a path by the game's mover,
// 0x08124B94(?, state*, mover): on its first call (state 0) the mover takes
// the actor's place as its base (+0x0A X, +0x0C Y, +0x0E Z); after that the
// actor's place is the base plus each axis's curve. The game starts the
// first gull mid-screen under the fade-in and the second just past the
// native 240, so in a wider view they appeared out of nothing (the user,
// 2026-10-16, asked them to start off screen). The base of a gull's mover on
// map 97 (its curve preset a gull's flight, 0x12 or 0x13, or what it moves
// over kSkyY) is moved back along the gull's path (Z: it flies along +Z)
// the first time the mover runs past its start (state 0x100),
// so that it starts kOffView past the view's right edge, whatever the view.
// It reaches each point later by as much (and is still crossing when the
// shot cuts). KOL_NO_GULL_SHIFT=1 leaves them.

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"

namespace kol {
namespace {

constexpr std::uint32_t kMover = 0x08124B94u;
constexpr char kMoverId[] = "kol.gull-mover";
constexpr std::uint32_t kMapNumber = 0x02029D40u;    // u16: the map shown
constexpr std::uint16_t kHarbourMap = 97;
constexpr std::uint32_t kBaseX = 0x0Au, kBaseY = 0x0Cu, kBaseZ = 0x0Eu;   // mover: s16
constexpr int kSkyY = 256;           // gulls fly 280-346 high; people and banners under 170
constexpr std::uint32_t kObject = 0x04u;   // mover: u32 -> the object it moves
constexpr std::uint32_t kPreset = 0x08u;   // mover: u8, the curve preset (ROM table 0x086132F4)
constexpr std::uint32_t kDuration = 0x09u; // mover: u8, its frames (the curves' segments share them)
constexpr float kSpeedUp = 1.4f;           // the gulls fly this much quicker than the game's
constexpr std::uint32_t kObjY = 0x12u;     // object: s16, its height
constexpr std::uint8_t kGullFlight1 = 0x12u, kGullFlight2 = 0x13u;   // the two gulls' presets
constexpr int kOffView = 44;         // its place this far past the edge: the sprite reaches 33 left of it
// The game's sprite slots (kol_focus): world X/Y/Z at +0x08/+0x0A/+0x0C and
// the screen X at +0x1C of an object's sprite, 8 left of its feet.
constexpr std::uint32_t kSlotArray = 0x03004150u, kSlotStride = 0x20u;
constexpr int kSlots = 26;

int g_extra_right = 0;
std::uint32_t g_done[16] = {};   // movers already moved back
int g_done_n = 0;
std::uint32_t g_longer[16] = {};   // movers whose last stretch is drawn out
int g_longer_n = 0;

// A curve axis's interpolation (the Z axis at mover +0x108), s16 words:
// segment (1, 2, ...), -, value, frame in it, target, frames, -, -, the
// segment's rise, frames, -, -, frames left. The gulls' Z curves are two
// segments (0 -> 160 -> 320 over 110 frames, 0 -> 128 -> 240 over 100).
constexpr std::uint32_t kZCurve = 0x108u;
constexpr std::uint32_t kSeg = 0x00u, kTarget = 0x08u, kFrames = 0x0Au, kRise = 0x10u, kFrames2 = 0x12u,
                        kLeft = 0x18u;

// The gull's last stretch, drawn out at its own speed until it is past the
// view's left edge (the game ended it mid-screen once it started off the
// right: the user, 2026-10-16, asked for the gulls to last longer). The mover is
// done when all its axes are, so the flight lasts as long as Z does; Y holds
// its last height.
void draw_out_last_stretch(std::uint32_t mover, int kx) {
    const std::uint32_t c = mover + kZCurve;
    if (bus_read_u16(c + kSeg) != 2u) return;
    for (int i = 0; i < g_longer_n; ++i)
        if (g_longer[i] == mover) return;
    if (g_longer_n < 16) g_longer[g_longer_n++] = mover;
    const int target = static_cast<std::int16_t>(bus_read_u16(c + kTarget));
    const int frames = static_cast<std::int16_t>(bus_read_u16(c + kFrames));
    const int rise = static_cast<std::int16_t>(bus_read_u16(c + kRise));
    const int left = static_cast<std::int16_t>(bus_read_u16(c + kLeft));
    if (frames <= 0 || rise <= 0) return;
    const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
    const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
    const int end = (bx - (bz + target)) / 2 + kx;      // its screen X when the game ends it
    const int gone = -g_extra_right - kOffView;          // past the view's left edge (the sprite
    if (end <= gone) return;                             // reaches ~33 left of its place, 7 right)
    const int more_z = 2 * (end - gone) + 16;            // Z still to fly
    const int more_f = (more_z * frames + rise - 1) / rise;   // at the stretch's own speed
    bus_write_u16(c + kTarget, static_cast<std::uint16_t>(target + more_z));
    bus_write_u16(c + kFrames, static_cast<std::uint16_t>(frames + more_f));
    bus_write_u16(c + kRise, static_cast<std::uint16_t>(rise + more_z));
    bus_write_u16(c + kFrames2, static_cast<std::uint16_t>(frames + more_f));
    bus_write_u16(c + kLeft, static_cast<std::uint16_t>(left + more_f));
    std::fprintf(stderr, "gulls: mover %08X would end at screen x %d: %d frames more, to %d\n", mover, end,
                 more_f, gone);
}
// Map 3's gulls after the rainy battle (states/thor/0013, the church): they
// fly east (+X, -Z) up from the west, and the game ends each flight (op 19
// 68 00 14 8C / 19 69 00 15 96: presets 0x14, 0x15, 140 and 150 frames) at
// the cartridge's right edge, inside the wide view: a wing clipped there,
// then nothing (the user, 2026-10-28). The last segment of their X and Z
// curves (X at mover +0x10, Z at +0x108, the same layout) is drawn out at
// its own speed until the gull is past the view's right edge.
constexpr std::uint16_t kChurchMap = 3;
constexpr std::uint32_t kXCurve = 0x10u;
constexpr std::uint8_t kChurchGull1 = 0x14u, kChurchGull2 = 0x15u;
void draw_out_east(std::uint32_t mover, int kx) {
    const std::uint32_t cx = mover + kXCurve, cz = mover + kZCurve;
    if (bus_read_u16(cx + kSeg) != 2u || bus_read_u16(cz + kSeg) != 2u) return;
    for (int i = 0; i < g_longer_n; ++i)
        if (g_longer[i] == mover) return;
    if (g_longer_n < 16) g_longer[g_longer_n++] = mover;
    const int tx = static_cast<std::int16_t>(bus_read_u16(cx + kTarget));
    const int tz = static_cast<std::int16_t>(bus_read_u16(cz + kTarget));
    const int vx = static_cast<std::int16_t>(bus_read_u16(cx + 0x04u));   // the curves' values now
    const int vz = static_cast<std::int16_t>(bus_read_u16(cz + 0x04u));
    const int fx = static_cast<std::int16_t>(bus_read_u16(cx + kFrames));
    const int rx = static_cast<std::int16_t>(bus_read_u16(cx + kRise));
    const int fz = static_cast<std::int16_t>(bus_read_u16(cz + kFrames));
    const int rz = static_cast<std::int16_t>(bus_read_u16(cz + kRise));
    if (fx <= 0 || fz <= 0 || fx != fz) return;
    const int dx = (tx > vx) - (tx < vx), dz = (tz > vz) - (tz < vz);
    const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
    const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
    const int end = ((bx + tx) - (bz + tz)) / 2 + kx;   // its screen X when the game ends it
    const int gone = 240 + g_extra_right + kOffView;    // past the view's right edge
    const float speed = (dx * rx - dz * rz) / 2.0f / static_cast<float>(fx);   // screen px a frame
    if (end >= gone || speed <= 0.25f) return;
    const int more_f = static_cast<int>((gone - end) / speed) + 1;
    const int more_x = rx * more_f / fx, more_z = rz * more_f / fz;
    bus_write_u16(cx + kTarget, static_cast<std::uint16_t>(tx + dx * more_x));
    bus_write_u16(cx + kRise, static_cast<std::uint16_t>(rx + more_x));
    bus_write_u16(cz + kTarget, static_cast<std::uint16_t>(tz + dz * more_z));
    bus_write_u16(cz + kRise, static_cast<std::uint16_t>(rz + more_z));
    for (std::uint32_t c : {cx, cz}) {
        bus_write_u16(c + kFrames, static_cast<std::uint16_t>(bus_read_u16(c + kFrames) + more_f));
        bus_write_u16(c + kFrames2, static_cast<std::uint16_t>(bus_read_u16(c + kFrames2) + more_f));
        bus_write_u16(c + kLeft, static_cast<std::uint16_t>(bus_read_u16(c + kLeft) + more_f));
    }
    std::fprintf(stderr, "gulls: map 3 mover %08X would end at screen x %d: %d frames more, to %d\n", mover, end,
                 more_f, gone);
}
// And their start: the game sets each one off a little way into the
// cartridge's screen (screen X 24 and 8), well inside the wide view, so
// they popped into the sky (the user, 2026-10-28, asked for the seagulls to start
// a bit further out, off screen). On the first frame of the
// flight (its curves set up, nothing flown) the base is moved back along
// the first segment by whole frames of it, so the gull starts at the view's
// left edge (flying east its sprite ends 12 left of its place, flat; more
// under the perspective) and reaches each point as many frames later. The
// scene deletes both gulls at a fixed time (158 frames after they set off),
// so they fly quicker, as much as the view's width asks, to be past its
// right edge kChurchFlight frames after they set off (they vanished over the
// church at the game's speed): x1.4 at 384, none at 240.
constexpr int kChurchFlight = 148;
constexpr float kChurchGullSpeed = 2.06f;   // screen px a frame at the game's speed (2.06, 2.13)
float church_speed_up() {
    const float path = static_cast<float>(240 + 2 * g_extra_right + kOffView);   // left edge to past the right
    return std::max(1.0f, path / (kChurchFlight * kChurchGullSpeed));
}
void start_west(std::uint32_t mover, int kx) {
    const std::uint32_t cx = mover + kXCurve, cz = mover + kZCurve;
    if (bus_read_u16(cx + kSeg) != 1u || bus_read_u16(cz + kSeg) != 1u) return;
    if (bus_read_u16(cx + 0x04u) != 0u || bus_read_u16(cz + 0x04u) != 0u) return;   // flown already
    for (int i = 0; i < g_done_n; ++i)
        if (g_done[i] == mover) return;
    if (g_done_n < 16) g_done[g_done_n++] = mover;
    const int tx = static_cast<std::int16_t>(bus_read_u16(cx + kTarget));
    const int tz = static_cast<std::int16_t>(bus_read_u16(cz + kTarget));
    const int fx = static_cast<std::int16_t>(bus_read_u16(cx + kFrames));
    const int fz = static_cast<std::int16_t>(bus_read_u16(cz + kFrames));
    if (fx <= 0 || fx != fz) return;
    const float speed = (tx - tz) / 2.0f / static_cast<float>(fx);   // screen px a frame
    if (speed <= 0.25f) return;
    const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
    const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
    const int start = (bx - bz) / 2 + kx;               // its screen X at the start
    const int gone = -g_extra_right;                    // at the view's left edge
    if (start <= gone) return;
    const int back = static_cast<int>((start - gone) / speed) + 1;   // frames
    const int nx = bx - tx * back / fx, nz = bz - tz * back / fz;
    bus_write_u16(mover + kBaseX, static_cast<std::uint16_t>(nx));
    bus_write_u16(mover + kBaseZ, static_cast<std::uint16_t>(nz));
    std::fprintf(stderr, "gulls: map 3 mover %08X starts at screen x %d: %d frames back, (%d,%d) -> (%d,%d)\n", mover,
                 start, back, bx, bz, nx, nz);
}
// Their height is a curve of its own (mover +0x8C: frames, frame now, then
// the curve's coefficients), done after the flight's frames: the game then
// puts the gull back at its base's height, on the ground, while the X and Z
// curves drawn out above still fly it (it dropped behind the church
// mid-sky). Held at its last frame while they fly: the gull keeps the
// height it ends at.
constexpr std::uint32_t kYFrames = 0x8Cu, kYNow = 0x8Eu;
void hold_height(std::uint32_t mover) {
    const unsigned frames = bus_read_u16(mover + kYFrames);
    const unsigned now = bus_read_u16(mover + kYNow);
    if (frames < 2u || now + 1u < frames) return;
    if (bus_read_u16(mover + kXCurve + kLeft) <= 1u && bus_read_u16(mover + kZCurve + kLeft) <= 1u) return;
    bus_write_u16(mover + kYNow, static_cast<std::uint16_t>(frames - 1u));   // the mover steps it to its last
}
// Map 117, the boat in the ending (scene 2's script 0x08770440, states/
// thor/0223): two gulls (actors 0x68, 0x69) made at the cartridge's left
// edge fly in (preset 0x1A, Z only: 256 in 100 frames, then 64 in 100),
// circle the sail (0x1B), and fly off (0x1C, Z only: 112 then 176 over 90
// frames each) to be deleted at screen X 256, inside the wide view. They
// appeared out of nothing at screen X -32 and vanished in the sky (the
// user, 2026-11-12, asked that they neither pop in nor vanish
// suddenly). The mover works each curve out from the ROM's preset by the
// frames gone (its words at +0x108 are a copy: a segment's target or rise
// written there changed nothing), and the place is the base plus the
// curves, every frame. So the base slides, the curves stay the game's: in
// 0x1A's first segment it starts kOffView past the view's left edge back
// along the line (+Z) and eases to the game's base by the segment's end (the
// gull reaches the same point at the same time, quicker on the way); in
// 0x1C's last segment it slides on along the line (-Z) so the segment ends
// kOffView past the view's right edge, where the scene deletes it. The
// scene's timing is the game's. KOL_NO_END_GULLS=1: as before.
constexpr std::uint16_t kBoatEndMap = 117;
constexpr std::uint8_t kEndGullIn = 0x1Au, kEndGullOut = 0x1Cu;
struct EndSlide { std::uint32_t mover; std::uint8_t preset; int bz0, dz; };
EndSlide g_end_slide[8];
int g_end_slide_n = 0;
constexpr std::uint32_t kCurveFrames = 0x0Au, kCurveLeft = 0x18u;   // (kFrames, kLeft)
// The share of the current segment flown, 0..1.
float seg_done(std::uint32_t c) {
    const int frames = static_cast<std::int16_t>(bus_read_u16(c + kCurveFrames));
    const int left = static_cast<std::int16_t>(bus_read_u16(c + kCurveLeft));
    if (frames <= 0) return 1.0f;
    return std::clamp(1.0f - static_cast<float>(left) / static_cast<float>(frames), 0.0f, 1.0f);
}
EndSlide* end_slide(std::uint32_t mover, std::uint8_t preset) {
    for (int i = 0; i < g_end_slide_n; ++i)
        if (g_end_slide[i].mover == mover && g_end_slide[i].preset == preset) return &g_end_slide[i];
    return nullptr;
}
void end_gull_in(std::uint32_t mover, int kx) {
    const std::uint32_t cz = mover + kZCurve;
    const int seg = bus_read_u16(cz + kSeg);
    EndSlide* e = end_slide(mover, kEndGullIn);
    if (!e) {
        if (seg != 1 || bus_read_u16(cz + 0x04u) != 0u || g_end_slide_n >= 8) return;   // (flown already)
        const int target = static_cast<std::int16_t>(bus_read_u16(cz + kTarget));
        if (target >= 0) return;   // (toward -Z: right on the screen)
        const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
        const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
        const int start = (bx - bz) / 2 + kx;             // its screen X at the start
        const int gone = -g_extra_right - kOffView;       // past the view's left edge
        e = &g_end_slide[g_end_slide_n++];
        *e = {mover, kEndGullIn, bz, start > gone ? 2 * (start - gone) : 0};
        std::fprintf(stderr, "gulls: map 117 mover %08X starts at screen x %d: slides in from Z %d\n", mover, start,
                     bz + e->dz);
    }
    if (!e->dz) return;
    const float t = seg == 1 ? seg_done(cz) : 1.0f;
    bus_write_u16(mover + kBaseZ, static_cast<std::uint16_t>(e->bz0 + static_cast<int>(std::lround(e->dz * (1.0f - t)))));
}
void end_gull_out(std::uint32_t mover, int kx) {
    const std::uint32_t cz = mover + kZCurve;
    const int seg = bus_read_u16(cz + kSeg);
    if (seg != 2) return;
    EndSlide* e = end_slide(mover, kEndGullOut);
    if (!e) {
        if (g_end_slide_n >= 8) return;
        const int target = static_cast<std::int16_t>(bus_read_u16(cz + kTarget));
        const int value = static_cast<std::int16_t>(bus_read_u16(cz + 0x04u));
        if (target >= value) return;   // (toward -Z)
        const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
        const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
        const int end = (bx - (bz + target)) / 2 + kx;    // its screen X when the game ends it
        const int gone = 240 + g_extra_right + kOffView;  // past the view's right edge
        e = &g_end_slide[g_end_slide_n++];
        *e = {mover, kEndGullOut, bz, end < gone ? 2 * (gone - end) : 0};
        std::fprintf(stderr, "gulls: map 117 mover %08X would end at screen x %d: slides on to %d\n", mover, end, gone);
    }
    if (!e->dz) return;
    bus_write_u16(mover + kBaseZ, static_cast<std::uint16_t>(e->bz0 - static_cast<int>(std::lround(e->dz * seg_done(cz)))));
}
unsigned long long g_epoch = 0;

bool in_ewram(std::uint32_t a) { return a >= 0x02000000u && a < 0x0203FF00u; }
bool in_iwram(std::uint32_t a) { return a >= 0x03000000u && a < 0x03008000u; }

// The camera's screen X offset, from any sprite slot with an object: screen
// X = (X - Z) / 2 + kx, native pixels.
bool camera_kx(int* kx) {
    for (int s = 0; s < kSlots; ++s) {
        const std::uint32_t o = kSlotArray + static_cast<std::uint32_t>(s) * kSlotStride;
        if (!bus_read_u32(o)) continue;
        const int x = static_cast<std::int16_t>(bus_read_u16(o + 0x08));
        const int z = static_cast<std::int16_t>(bus_read_u16(o + 0x0C));
        const int sx = static_cast<std::int16_t>(bus_read_u16(o + 0x1C));
        if (!x && !z) continue;
        *kx = sx + 8 - (x - z) / 2;
        return true;
    }
    return false;
}

// Everything here changes when and where a scene's people (and gulls)
// appear, walk and go, for the wide view. The Original preset keeps the
// game's own (asked 2026-10-28 for these spawn time and route changes
// to apply only outside the Original preset). Decided once
// per map shown, so a preset changed mid-scene can't leave a change half
// done (a thread in our ops still goes back to the game's: the *_back steps
// always run).
bool scenes_on() {
    static std::uint16_t map = 0xFFFFu;
    static unsigned long long epoch = ~0ull;
    static bool on = true;
    const std::uint16_t now = bus_read_u16(kMapNumber);
    if (now != map || epoch != g_runtime_state_epoch) {
        map = now;
        epoch = g_runtime_state_epoch;
        const int p = preset_current();
        on = !(p >= 0 && std::string_view(preset_label(static_cast<unsigned>(p))) == "Original");
    }
    return on;
}

int on_mover(std::uint32_t, int, ArmCpuState* cpu) {
    // Movers are reused: a new map (map 97's gulls, later map 3's) starts
    // the lists afresh, as a state load does.
    static std::uint16_t lists_map = 0xFFFFu;
    if (g_epoch != g_runtime_state_epoch || lists_map != bus_read_u16(kMapNumber)) {
        g_epoch = g_runtime_state_epoch;
        lists_map = bus_read_u16(kMapNumber);
        g_done_n = 0;
        g_longer_n = 0;
        g_end_slide_n = 0;
    }
    const std::uint32_t state = cpu->R[1], mover = cpu->R[2];
    static const bool debug = std::getenv("KOL_GULL_DEBUG") != nullptr;
    if (debug)
        std::fprintf(stderr, "gulls: mover call r1=%08X r2=%08X state=%04X map=%u obj=%08X y=%d curves=%02X base=(%d,%d,%d)\n",
                     state, mover,
                     (in_ewram(state) || in_iwram(state)) ? bus_read_u16(state) : 0xFFFFu, bus_read_u16(kMapNumber),
                     bus_read_u32(mover + 4), in_ewram(bus_read_u32(mover + 4)) ? static_cast<std::int16_t>(bus_read_u16(bus_read_u32(mover + 4) + 0x12)) : -1,
                     bus_read_u8(mover + 8),
                     static_cast<std::int16_t>(bus_read_u16(mover + kBaseX)),
                     static_cast<std::int16_t>(bus_read_u16(mover + kBaseY)),
                     static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ)));
    if (!(in_ewram(state) || in_iwram(state)) || !(in_ewram(mover) || in_iwram(mover))) return 0;
    if (bus_read_u16(state) < 0x100u) return 0;   // state 0: the base is taken in this call
    if (bus_read_u16(kMapNumber) == kBoatEndMap && scenes_on()) {
        static const bool off = std::getenv("KOL_NO_END_GULLS") != nullptr;
        const std::uint8_t preset = bus_read_u8(mover + kPreset);
        int kx = 0;
        if (!off && (preset == kEndGullIn || preset == kEndGullOut) && camera_kx(&kx)) {
            if (preset == kEndGullIn) end_gull_in(mover, kx);
            else end_gull_out(mover, kx);
        }
        return 0;
    }
    if (bus_read_u16(kMapNumber) == kChurchMap && scenes_on()) {
        const std::uint8_t preset = bus_read_u8(mover + kPreset);
        int kx = 0;
        if ((preset == kChurchGull1 || preset == kChurchGull2) && bus_read_u16(state) == 0x100u) {
            const unsigned dur = bus_read_u8(mover + kDuration);   // the curves are set from it in this call
            const unsigned quick = static_cast<unsigned>(dur / church_speed_up() + 0.5f);
            if (quick >= 8u && quick < dur) bus_write_u8(mover + kDuration, static_cast<std::uint8_t>(quick));
        }
        if ((preset == kChurchGull1 || preset == kChurchGull2) && camera_kx(&kx)) {
            hold_height(mover);
            start_west(mover, kx);
            draw_out_east(mover, kx);
        }
        return 0;
    }
    if (bus_read_u16(kMapNumber) != kHarbourMap || !scenes_on()) return 0;
    // A gull: its mover's curve preset is a gull's flight (the base is on
    // the ground, the Y curve lifts it; on its first calls it is still at
    // 0), or what it moves is already in the sky.
    const std::uint8_t preset = bus_read_u8(mover + kPreset);
    const std::uint32_t object = bus_read_u32(mover + kObject);
    const int oy = in_ewram(object) ? static_cast<std::int16_t>(bus_read_u16(object + kObjY)) : 0;
    if (preset != kGullFlight1 && preset != kGullFlight2 && oy <= kSkyY) return 0;
    int kx = 0;
    if (!camera_kx(&kx)) return 0;
    for (int i = 0; i < g_done_n; ++i)
        if (g_done[i] == mover) {
            draw_out_last_stretch(mover, kx);
            return 0;
        }
    if (g_done_n < 16) g_done[g_done_n++] = mover;
    // Quicker (kSpeedUp): crossing the wide view at the game's speed, the
    // second gull held the boat back 5 s (the scene waits for its flight);
    // the user, 2026-10-16, asked for about 2 seconds less.
    // Its curves are set up in this call (state 0x100) from the duration.
    if (bus_read_u16(state) == 0x100u) {
        const unsigned dur = bus_read_u8(mover + kDuration);
        const unsigned quick = static_cast<unsigned>(dur / kSpeedUp + 0.5f);
        if (quick >= 8u && quick < dur) bus_write_u8(mover + kDuration, static_cast<std::uint8_t>(quick));
    }
    const int bx = static_cast<std::int16_t>(bus_read_u16(mover + kBaseX));
    const int bz = static_cast<std::int16_t>(bus_read_u16(mover + kBaseZ));
    const int start = (bx - bz) / 2 + kx;               // its screen X at the start
    const int edge = 240 + g_extra_right + kOffView;    // past the view's right edge
    if (start >= edge) return 0;
    const int nz = bz - 2 * (edge - start);             // back along +Z: right on screen
    bus_write_u16(mover + kBaseZ, static_cast<std::uint16_t>(nz));
    std::fprintf(stderr, "gulls: mover %08X starts at screen x %d, moved back to %d (Z %d -> %d)\n", mover,
                 start, edge, bz, nz);
    return 0;   // the game's mover runs on the new base
}

// The two people on the quay (map 97, actors 0x05 and 0x75 at cells 15,10
// and 14,10) are created by the scene's script only after the gulls have
// flown. Its main thread (script 0x0876F78C; contexts at 0x02028ED0, stride
// 0x3C; ctx +0 s16 offset, +2 the wait timer, +0x10 the script) runs: 50 60
// (sleep 96 under the narration) | 00 12 00, 00 14 00, 00 17 00, 00 1b 00
// (spawn the fade and gull 1 threads) | 01 01 21 00 (spawn thread 1: gull 2)
// | 04 01 (join thread 1) | 6d 26 00 (jump) | 06 05 0f 0a 0a, 06 75 0e 0a
// 02 (the two people) | 50 01 | 06 76 .., 06 77 .., 11 77 .. (the two blue
// pennants on the deck) | 50 01 | three blocks of 6e 12 11, 06 71/72/73,
// 0b, 11, 13 .. (the banners hanging from the yard, each ended by 50 01) |
// the crew ... A step runs ops until one waits (the step loop, 0x0811A248,
// decoded 2026-10-16). At 240 the two people stand off the game's screen;
// in the wide view they stand in the right margin and appeared from
// nothing once the gulls had passed (the user, 2026-10-16, asked for them
// to be there from the start), and with the gulls' longer flight the pennants
// and banners came late too (the user asked for the same for the boat's
// flags). The main thread now makes all of them first: its step starting
// at 0x7E (the sleep over, nothing spawned yet) starts at 0xB9 instead and
// runs the blocks (a step each, their 50 01s between), the step after the
// last (at 0x13F) goes back to 0x7E, and the one after the join (at 0x90)
// skips to 0x13D. Op 06 makes the actor's record in the 8 "other side" unit
// slots (0x02001760), or the party's (0x02001420) when game flag 0x112 is
// set (6e 12 11; one-shot, cleared by the 06 that reads it). Early, the
// other side has 3 free slots (the fade's and the gulls' actors go only
// later), not the 4 the people and pennants need: with none, 06 goes on
// with no record and traps in 0x0801355A. So the flag is set before the
// pennants' step: 76 goes to the party's side, 77 takes the last free
// slot, as the banners then take the party's with the script's own flag.
// (A first try ran the creations in the gull's child thread: the game hung
// in 0x0801355A, a trap at the end of a sprite table search with a garbage
// key; a child's context lacks what the main thread's has.) A state saved
// mid-scene (states/HJ: the main thread already in its join) keeps the
// game's order.
constexpr std::uint32_t kStep = 0x0811A248u;
constexpr char kStepId[] = "kol.quay-early";
constexpr std::uint32_t kHarbourScript = 0x0876F78Cu;
constexpr int kAfterSleep = 0x7E;      // 00 12 00: the first spawn
constexpr int kAfterJoin = 0x90;       // 6d 26 00: the jump to the creations
constexpr int kPeople = 0xB9;          // 06 05 0f 0a 0a, 06 75 0e 0a 02 ...
constexpr int kSleepOne = 0x13D;       // the 50 01 after the last banner
constexpr int kAfterPeople = 0x13F;    // 6e 12 11, 06 6f ...: the crew
constexpr int kPennants = 0xC5;        // 06 76 12 12 0e, 06 77 0f 10 0e, 11 77 ...
constexpr std::uint32_t kFlag112Byte = 0x020011CCu + (0x112u >> 3);   // game flags: bit (flag & 7)
constexpr std::uint8_t kFlag112Bit = 1u << (0x112u & 7u);
// The soldier on the upper street (actor 0x31) walks route 9 of the scene's
// route table ([[0x02029AC4] + 0x18], 36-byte records: 16 cells as x, z
// bytes, then a u32 count), cells 5,8 to 5,3, up and to the right, and the
// script deletes him when he arrives (16 31 2a: walk actor 0x31 along route
// (0x2a & 0x1f) - 1; 09 31). At 240 he is off the game's screen by then; in
// the wide view he vanished mid-street (the user, 2026-10-16, asked for
// the one walking up-right to keep going until off screen).
// Before that op runs, the route goes on to the map's edge, 5,0 (the street
// is height 7 all along); the deletion waits for the walk, and nothing waits
// for that thread. KOL_NO_LONG_WALK=1 leaves it. Row 0 is the map's last
// row, and at 384x216 his feet still showed at the top edge when he went;
// at 480x270 all of him. So over his last cell and a half he fades out
// (the user asked for a fade-out), by the game's own blending, as the ghost unit
// (kol_ghost.cpp): object flag 0x100 (+0x78) gives his sprite OBJ mode 1,
// and the game's BLDCNT/BLDALPHA copies (0x03000E1A/E1C, written to the
// registers each frame) get BG0-3 and the backdrop as second targets and
// weights from 16/0 to 0/16 as his Z goes from kFadeFrom to the last cell's
// middle. Only while the game blends nothing itself (no effect, no first
// targets); put back when he is gone. gulls_frame() runs it every frame.
// While he fades, the game draws no shadow blob under him: its callback
// 0x080598DC(node, object) returns at once for him (a hook). His boots over
// the solid blob did not fade (our PPU blends a sprite with the layer under
// it only when that is not another sprite), and a solid shadow under a
// fading man is wrong anyway. With Unit Shadows our cast shadow fades with
// him instead (kol_focus: a semi-transparent unit's shadow at EVA / 16).
constexpr std::uint32_t kSceneRoutesPtr = 0x02029AC4u;
constexpr int kWalkOp = 0x98;          // 16 31 2a
constexpr int kWalkRoute = 9;
constexpr std::uint8_t kWalkFrom[6][2] = {{5, 8}, {5, 7}, {5, 6}, {5, 5}, {5, 4}, {5, 3}};
constexpr std::uint8_t kWalkMore[3][2] = {{5, 2}, {5, 1}, {5, 0}};
constexpr std::uint8_t kWalkerActor = 0x31;
constexpr std::uint32_t kBlob = 0x080598DCu;   // (node, object): a unit's shadow blob
constexpr char kBlobId[] = "kol.walker-blob";
constexpr std::uint32_t kOcclusion = 0x03005DE0u;   // (sx, sy, X, Y, [sp] Z, ...): a unit's sprite priorities
constexpr char kOcclusionId[] = "kol.walk-in-occlusion";
constexpr int kFadeFrom = 64, kFadeTo = 16;   // his Z: fully drawn, gone (5,0's middle)
constexpr std::uint32_t kObjectsPtr = 0x0202A238u;   // the map's objects: 26 of 0xD0
constexpr std::uint32_t kObjFlags = 0x78u;           // object: u16 flags; 0x100 semi-transparent
constexpr std::uint32_t kObjZ = 0x14u;               // object: s16 world Z
constexpr std::uint16_t kSemi = 0x100u;
constexpr std::uint32_t kBldcnt = 0x03000E1Au, kBldalpha = 0x03000E1Cu;   // the game's copies
constexpr std::uint16_t kSecondTargets = 0x2F00u;   // BG0-3 and the backdrop
bool g_walk_longer = false;   // this scene's route 9 was lengthened (lengthen_walk)
std::uint32_t g_fade_obj = 0;
bool g_fade_blend = false;
std::uint16_t g_fade_old_cnt = 0, g_fade_old_alpha = 0, g_fade_alpha = 0;
unsigned long long g_fade_epoch = 0;

void lengthen_walk() {
    static const bool off = [] {
        const char* v = std::getenv("KOL_NO_LONG_WALK");
        return v && v[0] && v[0] != '0';
    }();
    if (off) return;
    const std::uint32_t scene = bus_read_u32(kSceneRoutesPtr);
    if (!in_ewram(scene)) return;
    const std::uint32_t routes = bus_read_u32(scene + 0x18);
    if (!in_ewram(routes)) return;
    const std::uint32_t r = routes + kWalkRoute * 36u;
    if (bus_read_u32(r + 0x20) != 6u) return;   // the game's route, untouched
    for (int i = 0; i < 6; ++i)
        if (bus_read_u8(r + 2u * i) != kWalkFrom[i][0] || bus_read_u8(r + 2u * i + 1u) != kWalkFrom[i][1]) return;
    for (int i = 0; i < 3; ++i) {
        bus_write_u8(r + 12u + 2u * i, kWalkMore[i][0]);
        bus_write_u8(r + 12u + 2u * i + 1u, kWalkMore[i][1]);
    }
    bus_write_u32(r + 0x20, 9u);
    g_walk_longer = true;
    std::fprintf(stderr, "gulls: the street soldier walks on to 5,0\n");
}

// Alphonse (actor 02) is created at cell 9,11, at the foot of the stairs
// from the upper street, mostly behind the mast; the main thread spawns his
// walk to the quay (0x1C5: 2c 02 00 01, 16 02 26 = route 5, 9,11 to 15,11;
// 02) after the camera's pan down to the boat (its Y, u16 at 0x02026C7A,
// 704 to 824), and he shows from behind the mast. The user, 2026-10-17,
// asked for him to come from further back, then to appear (not walk in)
// later, as the camera's pan nearly ends. So he is put at
// 5,11 up on the upper street (height 7; his placement +0x14/+0x15, his
// object's place +0x10.. and the walker's leg end +0x18..) and not drawn
// (the draw list 0x0805A77C takes an object only while bit 0x40 of its
// record's +0x17 is set; kol_ghost). The fade thread (0x93: 27 13 00 3c,
// then 02 at 0x97, the opening's fade-in), free once the fade is over,
// waits (the script's 50 08 at 0x196, again and again) until the camera's Y
// reaches kCameraGo; then route 5 gets 5,11 to 8,11 in front (the stairs:
// heights 7, 7, 6, 5, then 4 at 9,11), he is drawn again (5,11 is above the
// view by then) and that thread runs his walk (0x1C5): he comes down the
// stairs into view as the pan ends. The walk thread the main spawns later
// waits the same way until his walk is over, then ends (its 02 at 0x1CC),
// so the main thread's wait for it (03) still covers his walk.
// KOL_NO_EARLY_ALPHONSE=1 leaves him to the game.
constexpr std::uint8_t kAlphonse = 0x02;
constexpr int kAfterPrologue = 0x4C;  // the main thread's step after the creations
constexpr int kWait8 = 0x196;         // 50 08
constexpr int kAfterWait8 = 0x198;
constexpr int kAlphonseWalk = 0x1C5;  // 2c 02 00 01, 16 02 26: the game's walk thread
constexpr std::uint32_t kCameraY = 0x02026C7Au;   // u16: the scene camera's Y
constexpr int kCameraGo = 805;        // the pan is 704 to 824; by 805 his 5,11 is above the view
constexpr int kFadeEnd = 0x97;        // 02: the fade thread's end
constexpr int kAlphonseEnd = 0x1CC;   // 02: the walk thread's end
constexpr int kAlphonseRoute = 5;
constexpr std::uint8_t kRouteFrom[7][2] = {{9, 11}, {10, 11}, {11, 11}, {12, 11}, {13, 11}, {14, 11}, {15, 11}};
constexpr std::uint8_t kRouteBack[4][2] = {{5, 11}, {6, 11}, {7, 11}, {8, 11}};
constexpr int kStartHeight = 7;       // the upper street at 5,11
constexpr int kFootHeight = 4;        // 9,11, where the game puts him
std::uint32_t g_alph_ctx = 0;
int g_alph_state = 0;   // 0 not yet, 1 placed back and hidden, 2 waiting for the camera, 3 walking, 4 walked, 5 off
std::uint32_t g_alph_game_ctx = 0;   // the walk thread the main spawned, held

std::uint32_t object_of(std::uint8_t actor) {
    const std::uint32_t list = bus_read_u32(0x0202A238u);
    if (!in_ewram(list)) return 0;
    for (int i = 0; i < 26; ++i) {
        const std::uint32_t o = list + static_cast<std::uint32_t>(i) * 0xD0u;
        const std::uint32_t rec = bus_read_u32(o);
        if (in_ewram(rec) && bus_read_u8(rec + 0x4E) == actor) return o;
    }
    return 0;
}

// Alphonse placed further back (at the main thread's first step after the
// prologue made him); false when he is not where the game put him.
bool place_alphonse(bool back) {
    const std::uint32_t obj = object_of(kAlphonse);
    if (!obj) return false;
    const std::uint32_t place = bus_read_u32(obj + 4);
    if (!in_ewram(place)) return false;
    const auto& from = back ? kRouteFrom[0] : kRouteBack[0];
    const auto& to = back ? kRouteBack[0] : kRouteFrom[0];
    if (bus_read_u8(place + 0x14) != from[0] || bus_read_u8(place + 0x15) != from[1]) return false;
    bus_write_u8(place + 0x14, to[0]);
    bus_write_u8(place + 0x15, to[1]);
    // His place (+0x10/+0x12/+0x14 X, Y, Z), and the walker's current leg's
    // end (+0x18/+0x1A/+0x1C): left at 9,11, his first leg walked him back
    // there from 5,11 before the route's next cell.
    const int x = 32 * to[0] + 16, y = 16 * (back ? kStartHeight : kFootHeight), z = 32 * to[1] + 16;
    for (const std::uint32_t at : {0x10u, 0x18u}) {
        bus_write_u16(obj + at, static_cast<std::uint16_t>(x));
        bus_write_u16(obj + at + 2u, static_cast<std::uint16_t>(y));
        bus_write_u16(obj + at + 4u, static_cast<std::uint16_t>(z));
    }
    return true;
}

// Drawn or not: bit 0x40 of his record's +0x17 (the draw list's test).
void show_alphonse(bool on) {
    const std::uint32_t obj = object_of(kAlphonse);
    if (!obj) return;
    const std::uint32_t rec = bus_read_u32(obj);
    if (!in_ewram(rec)) return;
    const std::uint8_t b = bus_read_u8(rec + 0x17);
    const std::uint8_t want = on ? static_cast<std::uint8_t>(b | 0x40u) : static_cast<std::uint8_t>(b & ~0x40u);
    if (want != b) bus_write_u8(rec + 0x17, want);
}

// His route 5 with the four cells back in front (when his walk starts: the
// scene's routes are not loaded yet at its first steps); false when it is
// not the game's.
bool lengthen_alphonse_route() {
    const std::uint32_t scene = bus_read_u32(kSceneRoutesPtr);
    if (!in_ewram(scene)) return false;
    const std::uint32_t routes = bus_read_u32(scene + 0x18);
    if (!in_ewram(routes)) return false;
    const std::uint32_t r = routes + kAlphonseRoute * 36u;
    if (bus_read_u32(r + 0x20) != 7u) return false;
    for (int i = 0; i < 7; ++i)
        if (bus_read_u8(r + 2u * i) != kRouteFrom[i][0] || bus_read_u8(r + 2u * i + 1u) != kRouteFrom[i][1])
            return false;
    for (int i = 6; i >= 0; --i) {
        bus_write_u8(r + 2u * (i + 4), kRouteFrom[i][0]);
        bus_write_u8(r + 2u * (i + 4) + 1u, kRouteFrom[i][1]);
    }
    for (int i = 0; i < 4; ++i) {
        bus_write_u8(r + 2u * i, kRouteBack[i][0]);
        bus_write_u8(r + 2u * i + 1u, kRouteBack[i][1]);
    }
    bus_write_u32(r + 0x20, 11u);
    return true;
}

// Alphonse's late walk (see above); true when it moved ctx.
bool early_alphonse(std::uint32_t ctx, int pc) {
    static const bool off = [] {
        const char* v = std::getenv("KOL_NO_EARLY_ALPHONSE");
        return v && v[0] && v[0] != '0';
    }();
    if (off) return false;
    auto go = [&](int to) { bus_write_u16(ctx, static_cast<std::uint16_t>(to)); };
    if (g_alph_state == 0 && pc == kAfterPrologue) {
        g_alph_state = place_alphonse(true) ? 1 : 5;
        if (g_alph_state == 1) {
            show_alphonse(false);
            std::fprintf(stderr, "gulls: Alphonse waits up the street at 5,11, not drawn\n");
        }
        return false;   // the main thread's step runs as the game's
    }
    // The fade thread: waits for the camera, then walks him.
    if (g_alph_state == 1 && pc == kFadeEnd) {
        g_alph_ctx = ctx;
        g_alph_state = 2;
        go(kWait8);
        return true;
    }
    if (g_alph_state == 2 && ctx == g_alph_ctx && pc == kAfterWait8) {
        if (static_cast<int>(bus_read_u16(kCameraY)) < kCameraGo) {
            go(kWait8);
            return true;
        }
        if (!lengthen_alphonse_route()) {   // not the game's route: his own place, drawn, the game's walk
            place_alphonse(false);
            show_alphonse(true);
            g_alph_state = 5;
            go(kAlphonseEnd);
            std::fprintf(stderr, "gulls: Alphonse's route is not the game's; put back at 9,11\n");
            return true;
        }
        show_alphonse(true);
        g_alph_state = 3;
        go(kAlphonseWalk);
        std::fprintf(stderr, "gulls: Alphonse comes down the stairs (camera at %u)\n", bus_read_u16(kCameraY));
        return true;
    }
    if (g_alph_state == 3 && ctx == g_alph_ctx && pc == kAlphonseEnd) {
        g_alph_state = 4;   // walked; the thread ends at its 02
        return false;
    }
    // The walk thread the main spawned: held until his walk is over, then ended.
    if (g_alph_state >= 1 && g_alph_state <= 4 && ctx != g_alph_ctx &&
        (pc == kAlphonseWalk || (ctx == g_alph_game_ctx && pc == kAfterWait8))) {
        g_alph_game_ctx = ctx;
        if (g_alph_state == 4) {
            g_alph_state = 5;
            go(kAlphonseEnd);
        } else {
            go(kWait8);
        }
        return true;
    }
    return false;
}

// The town under attack (map 98, the same script from 0x234): the three
// archers (actors 0x75, 0x76, 0x77) and the soldier 0x78 are made hidden on
// the map's west edge (cells 0,14, 0,15, 0,15, 0,16; nothing on the grid is
// west of column 0) and each thread shows its actor as it starts its walk
// in (0x2CA: 2c 75 00 01, then 17 75 01 02 10 02, the walk to 6,12; 0x2DA
// 76; 0x2EA 77; 0x2F4 78, who glides in 96 px: 11 78 00 f6 00, 0b 78 02
// 10, 10 78 60 00 02). In the wide view the edge column is in plain sight,
// so they appeared there out of nothing (the user, 2026-10-17, asked for them
// to come from further away before shooting). Routes cannot go
// past column 0 (op 15 packs cells in 5 bits; the walker reads unsigned
// bytes), but op 10 moves an actor by signed pixels, at once (speed 0) or
// gliding (0x08124EC0, 0x78's own entrance). So at each show op the thread
// runs a few ops of ours instead, from kWalkInBuf (the end of EWRAM: zero
// in every saved state, written only by a load's clear): op 10 moves the
// actor kWalkInCells cells west at once, 2c shows it, 0b 02 10 gives it the
// walk (0x78's), op 10 glides it back to its edge cell at 2 px a frame (a
// walk's pace); then the thread goes on at the game's next op, the walk
// from the edge cell. Everything after comes later by the walk in (16
// frames a cell), the arrows with it. Moving at once does not change the
// actor's cell (the handler adds to its X and Z only), so the route still
// starts there. kWalkInCells: past the view's left edge at its size (the
// camera holds still here); KOL_ARCHER_WALK_CELLS=<n> sets it,
// KOL_NO_ARCHER_WALK_IN=1 leaves them to the game. Off the grid the ground
// under them is the map editor's (map_ext.cfg) or nothing.
//
// The smoke scene after the burning town (map 100, the same script from
// 0x4E1): two scouts, 0x75 and 0x76, made hidden on cell 5,16, an inner
// cell of the street (the user, 2026-10-17, saw them pop in out of
// nowhere too); thread A shows 0x75 at 0x538 and walks it on (17 75 01 02 10 01,
// 5,16 -> 11,16 -> 11,13), thread B 78 frames later 0x76 at 0x551 (16 76
// 23, 5,16 -> 10,16). The same street goes on behind it at height 8, along
// row 16 to 2,16 -- but that is the floor of a building: its roof is
// drawn at height 11 over 2..5,16..20, and 5,16 is its doorway (only 3,16
// and 5,16 show through). Walked in from 2,16 they were drawn over the roof
// (the game draws every sprite over the map's art; the user noted he
// belongs behind it). So each steps out of the doorway: moved a
// cell back (4,16, under the roof) at once, shown, a glide of 32 px at 1 px
// a frame (their walk: 32 frames a cell), faded in over it (fade_to_x;
// walk_in_fade). The fade is ours, on the unit's card (kol_focus,
// kol_perspective): the game's one blend weight is the smoke's here (BLDCNT
// 1142, BLDALPHA 0B08), so its transparency would have pulsed the smoke.
// With no card (the perspective off) the scout is shown at the doorway, as
// the game does: our ops show it after the glide.
constexpr std::uint16_t kTownMap = 98;
constexpr std::uint16_t kSmokeMap = 100;
constexpr std::uint32_t kWalkInBuf = 0x0203F000u, kWalkInStride = 0x80u;
struct WalkIn {
    std::uint16_t map;
    int show, back;           // the thread's show op; where it goes on after ours
    std::uint8_t actor;
    bool lifted;              // 0x78: his Z -10 first (his own 11 78 00 f6 00)
    int cells;                // how far back; 0: past the view's left edge (walk_in_cells)
    int speed;                // the glide's pixels a frame
    int fade_to_x;            // faded in over the walk, full at this X (0: no fade)
};
constexpr WalkIn kWalkIns[] = {
    {kTownMap, 0x2CA, 0x2CE, 0x75, false, 0, 2, 0},
    {kTownMap, 0x2DA, 0x2DE, 0x76, false, 0, 2, 0},
    {kTownMap, 0x2EA, 0x2EE, 0x77, false, 0, 2, 0},
    {kTownMap, 0x2F4, 0x301, 0x78, true, 0, 2, 0},   // back at his own glide, 10 78 60 00 02
    {kSmokeMap, 0x538, 0x53C, 0x75, false, 1, 1, 176},
    {kSmokeMap, 0x551, 0x555, 0x76, false, 1, 1, 176},
};
constexpr int kWalkIns_n = static_cast<int>(sizeof kWalkIns / sizeof kWalkIns[0]);
constexpr int kEdgeZ = 14 * 32 + 16;   // 0x75's row, the furthest from the left edge
constexpr int kWalkInCushion = 48;     // past the edge: half a sprite, and the perspective's wider top

int walk_in_cells() {
    static const int set = [] {
        const char* v = std::getenv("KOL_ARCHER_WALK_CELLS");
        return v && *v ? std::atoi(v) : -1;
    }();
    if (set >= 0) return std::min(set, 24);
    // Feet on screen at (X - Z) / 2 + kx; off the view when that is left of
    // -(margin + cushion). The edge cell's X is 16.
    int kx = 256;   // the town's camera, 392,640
    camera_kx(&kx);
    const int x_need = kEdgeZ - 2 * (g_extra_right + kWalkInCushion + kx);
    return std::clamp((16 - x_need + 31) / 32, 1, 24);
}

// Ours for the thread at its show op; false when the script is not the
// game's there.
bool walk_in(std::uint32_t ctx, int i) {
    const WalkIn& w = kWalkIns[i];
    const std::uint8_t show[4] = {0x2C, w.actor, 0x00, 0x01};
    for (int k = 0; k < 4; ++k)
        if (bus_read_u8(kHarbourScript + w.show + k) != show[k]) return false;
    const std::uint8_t lift[5] = {0x11, w.actor, 0x00, 0xF6, 0x00};
    if (w.lifted)
        for (int k = 0; k < 5; ++k)
            if (bus_read_u8(kHarbourScript + w.show + 4 + k) != lift[k]) return false;
    const int px = 32 * (w.cells ? w.cells : walk_in_cells());
    const std::uint32_t buf = kWalkInBuf + static_cast<std::uint32_t>(i) * kWalkInStride;
    std::uint32_t at = buf;
    auto op = [&](std::initializer_list<std::uint8_t> b) {
        for (const std::uint8_t v : b) bus_write_u8(at++, v);
    };
    auto pieces = [&](int total, int speed, int sign) {
        for (int left = total; left > 0;) {
            const int p = std::min(left, 96);
            left -= p;
            op({0x10, w.actor, static_cast<std::uint8_t>(sign * p), 0x00, static_cast<std::uint8_t>(speed)});
        }
    };
    pieces(px, 0, -1);
    // A fade with no card to fade it on (the perspective off): shown only
    // at the end, where the game shows it.
    const bool show_late = w.fade_to_x && !kol::perspective_enabled();
    if (!show_late) op({0x2C, w.actor, 0x00, 0x01});
    if (w.lifted) op({0x11, w.actor, 0x00, 0xF6, 0x00});
    op({0x0B, w.actor, 0x02, 0x10});
    if (w.cells) {
        pieces(px, w.speed, 1);   // on the grid all the way
        if (show_late) op({0x2C, w.actor, 0x00, 0x01});
    } else {
        // The glide (0x0812418C) takes X as unsigned: one from below 0 to
        // above it runs 65,000 px the other way. So it glides to X -2, steps
        // the 2 px to 0 at once (a frame's walk), then glides the last 16.
        pieces(px - 18, w.speed, 1);
        op({0x10, w.actor, 0x02, 0x00, 0x00});
        op({0x10, w.actor, 0x10, 0x00, static_cast<std::uint8_t>(w.speed)});
    }
    op({0x02});   // never run: the step after the last glide goes back to the game's
    bus_write_u32(ctx + 0x10, buf);
    bus_write_u16(ctx, 0);
    std::fprintf(stderr, "gulls: actor %02X walks in from %d px back (map %u)\n", w.actor, px, w.map);
    return true;
}

// A thread in ours: back to the game's script after the last glide. Our
// ops end with 02 (a byte past the last glide's), so the step that starts
// on it is the one after the glide.
bool walk_in_back(std::uint32_t ctx) {
    const std::uint32_t base = bus_read_u32(ctx + 0x10);
    if (base < kWalkInBuf || base >= kWalkInBuf + kWalkIns_n * kWalkInStride) return false;
    const int i = static_cast<int>((base - kWalkInBuf) / kWalkInStride);
    if (base != kWalkInBuf + static_cast<std::uint32_t>(i) * kWalkInStride) return false;
    const int pc = static_cast<std::int16_t>(bus_read_u16(ctx));
    if (bus_read_u8(base + static_cast<std::uint32_t>(pc)) != 0x02) return true;   // still ours
    bus_write_u32(ctx + 0x10, kHarbourScript);
    bus_write_u16(ctx, static_cast<std::uint16_t>(kWalkIns[i].back));
    for (std::uint32_t a = base; a < base + kWalkInStride; ++a) bus_write_u8(a, 0);
    return true;
}

const bool g_walk_in_on = [] {
    const char* v = std::getenv("KOL_NO_ARCHER_WALK_IN");
    return !(v && v[0] && v[0] != '0');
}();

// How far a walking-in actor has faded in (1: not fading), from a reader
// of EWRAM: the live bus here, the present's capture in kol_focus.
template <typename Rd8, typename Rd16>
float walk_in_fade(Rd8 rd8, Rd16 rd16, std::uint8_t actor, int x) {
    if (!g_walk_in_on) return 1.0f;
    const std::uint16_t map = rd16(kMapNumber);
    for (int i = 0; i < kWalkIns_n; ++i) {
        const WalkIn& w = kWalkIns[i];
        if (!w.fade_to_x || w.map != map || w.actor != actor) continue;
        if (rd8(kWalkInBuf + static_cast<std::uint32_t>(i) * kWalkInStride) == 0) continue;   // not in ours
        const int px = 32 * w.cells;
        return std::clamp(static_cast<float>(x - (w.fade_to_x - px)) / static_cast<float>(px), 0.0f, 1.0f);
    }
    return 1.0f;
}

float live_fade(std::uint32_t obj) {
    const std::uint32_t rec = bus_read_u32(obj);
    if (!in_ewram(rec)) return 1.0f;
    return walk_in_fade([](std::uint32_t a) { return bus_read_u8(a); },
                        [](std::uint32_t a) { return bus_read_u16(a); }, bus_read_u8(rec + 0x4E),
                        static_cast<std::int16_t>(bus_read_u16(obj + 0x10)));
}

// The archers stay after their shot. Each one's thread deletes it 54 frames
// after it shoots (09 75, 09 76, 09 77), and another thread 0x61 and the
// soldier 0x78 (09 61 09 78, one step), while the camera pans east after
// the arrows: at 240 they have just left the game's screen, in the wide view
// they vanished in plain sight. A step starting on those ops goes past
// them; the map change to 99 clears them with the rest.
// KOL_ARCHERS_DELETED=1: the game's way.
struct KeptActor { int pc; std::uint8_t actor, actor2; };   // actor2: a second 09 right after (0: none)
constexpr KeptActor kKeptArchers[] = {{0x333, 0x75, 0}, {0x33C, 0x76, 0}, {0x347, 0x77, 0}, {0x351, 0x61, 0x78}};

bool archers_stay(std::uint32_t ctx, int pc) {
    static const bool off = [] {
        const char* v = std::getenv("KOL_ARCHERS_DELETED");
        return v && v[0] && v[0] != '0';
    }();
    if (off || !g_walk_in_on || bus_read_u16(kMapNumber) != kTownMap) return false;
    for (const KeptActor& k : kKeptArchers) {
        if (pc != k.pc) continue;
        const int n = k.actor2 ? 2 : 1;
        const std::uint8_t actors[2] = {k.actor, k.actor2};
        for (int j = 0; j < n; ++j)
            if (bus_read_u8(kHarbourScript + pc + 2 * j) != 0x09 ||
                bus_read_u8(kHarbourScript + pc + 2 * j + 1) != actors[j])
                return false;
        bus_write_u16(ctx, static_cast<std::uint16_t>(pc + 2 * n));
        for (int j = 0; j < n; ++j)
            std::fprintf(stderr, "gulls: actor %02X stays after the shot (map %u)\n", actors[j], kTownMap);
        return true;
    }
    return false;
}

bool town_walk_in(std::uint32_t ctx, int pc) {
    if (!g_walk_in_on) return false;
    const std::uint16_t map = bus_read_u16(kMapNumber);
    for (int i = 0; i < kWalkIns_n; ++i)
        if (kWalkIns[i].map == map && pc == kWalkIns[i].show) return walk_in(ctx, i);
    return false;
}

// The ship at night (map 58, script 0x08772304): after Rictor's talk, the
// soldier who brings word of the attack (actor 0x49) is made at cell 8,3
// (`06 49 08 03 06` at 0x1D3, the main thread jumping there from 0x1BD) at
// height 48, below the rock's top, and walks routes 1-2 (`17 49 01 02 0f 02`
// at 0x20B, another thread: 8,3 -> 4,3 -> 4,4, set by `15 68 10 23` and
// `15 64 10 44`). At 240 that cell is off the game's screen; in the wide
// view it is the rock's sea side, and he climbed out of the water (the
// user, 2026-10-27, asked for him to appear exactly where Rictor did
// earlier). Rictor came back on just before (actor 0x05, `06 05 02 00 06`
// at 0x16D) at cell 2,0 and walked 2,0 -> 2,3 -> 4,3 -> 4,6. So the main
// thread makes the soldier from ops of ours at 2,0 (`06 49 02 00 06`, the
// game's facing), sleeps a frame and goes on at the game's routes (0x1D8);
// and at his walk his two route records become Rictor's way, 2,0 -> 2,3 ->
// 3,3 -> 4,3, then his own last step 4,3 -> 4,4. Route records:
// [[0x02029AC4]+0x18], 36 bytes (16 cells x, z; u32 count).
// KOL_SHIP_SOLDIER_WATER=1: the game's way.
constexpr std::uint16_t kShipMap = 58;
constexpr std::uint32_t kShipScript = 0x08772304u;
constexpr int kSoldierJump = 0x1BD, kSoldierMade = 0x1D3, kSoldierBack = 0x1D8, kSoldierWalk = 0x20B;
constexpr std::uint32_t kSoldierBuf = kWalkInBuf + 0x400u;   // zero in all 288 saved states
constexpr std::uint8_t kSoldierRoute0[] = {2, 0, 2, 1, 2, 2, 2, 3, 3, 3, 4, 3};
constexpr std::uint8_t kSoldierRoute1[] = {4, 3, 4, 4};

bool ship_soldier_on() {
    static const bool off = [] {
        const char* v = std::getenv("KOL_SHIP_SOLDIER_WATER");
        return v && v[0] && v[0] != '0';
    }();
    return !off && bus_read_u16(kMapNumber) == kShipMap;
}

bool script_has(std::uint32_t at, std::initializer_list<std::uint8_t> bytes) {
    for (const std::uint8_t v : bytes)
        if (bus_read_u8(at++) != v) return false;
    return true;
}

// The main thread at its jump to the soldier's creation: ours instead.
bool ship_soldier_made(std::uint32_t ctx, int pc) {
    if (pc != kSoldierJump || !ship_soldier_on()) return false;
    if (!script_has(kShipScript + kSoldierJump, {0x6D, 0x13, 0x00}) ||
        !script_has(kShipScript + kSoldierMade, {0x06, 0x49, 0x08, 0x03, 0x06, 0x15, 0x68, 0x10, 0x23}))
        return false;
    std::uint32_t at = kSoldierBuf;
    for (const std::uint8_t v : {0x06, 0x49, 0x02, 0x00, 0x06, 0x50, 0x01, 0x02}) bus_write_u8(at++, v);
    bus_write_u32(ctx + 0x10, kSoldierBuf);
    bus_write_u16(ctx, 0);
    std::fprintf(stderr, "gulls: the soldier (actor 49) made at 2,0, where Rictor came on (map %u)\n", kShipMap);
    return true;
}

// The thread in ours: back to the game's script at the step after the sleep.
bool ship_soldier_back(std::uint32_t ctx) {
    if (bus_read_u32(ctx + 0x10) != kSoldierBuf) return false;
    const int pc = static_cast<std::int16_t>(bus_read_u16(ctx));
    if (bus_read_u8(kSoldierBuf + static_cast<std::uint32_t>(pc)) != 0x02) return true;   // still ours
    bus_write_u32(ctx + 0x10, kShipScript);
    bus_write_u16(ctx, static_cast<std::uint16_t>(kSoldierBack));
    for (std::uint32_t a = kSoldierBuf; a < kSoldierBuf + 0x10u; ++a) bus_write_u8(a, 0);
    return true;
}

// His walk: Rictor's way from 2,0, as his two route records.
void ship_soldier_walk(int pc) {
    if (pc != kSoldierWalk || !ship_soldier_on()) return;
    if (!script_has(kShipScript + kSoldierWalk, {0x17, 0x49, 0x01, 0x02})) return;
    const std::uint32_t p = bus_read_u32(0x02029AC4u);
    const std::uint32_t routes = in_ewram(p) ? bus_read_u32(p + 0x18) : 0;
    // Made at 2,0 by us, and the game's routes for him as its `15` ops left them.
    if (!in_ewram(routes) || bus_read_u8(routes) != 8 || bus_read_u8(routes + 1) != 3) return;
    auto put = [&](int r, const std::uint8_t* cells, int n) {
        const std::uint32_t rec = routes + static_cast<std::uint32_t>(r) * 36u;
        for (int k = 0; k < 32; ++k) bus_write_u8(rec + k, k < 2 * n ? cells[k] : 0);
        bus_write_u32(rec + 32, static_cast<std::uint32_t>(n));
    };
    put(0, kSoldierRoute0, static_cast<int>(sizeof kSoldierRoute0 / 2));
    put(1, kSoldierRoute1, static_cast<int>(sizeof kSoldierRoute1 / 2));
}

std::uint32_t g_early_ctx = 0;
int g_early_state = 0;   // 0 not yet, 1 making them, 2 made (the game's order resumed), 3 done
unsigned long long g_early_epoch = 0;

// Map 89, New Game's town (the market where Rictor and the dark-haired
// knight talk before his name is asked): its townspeople are made and
// deleted at the game's screen edge, or inside it, as the camera pans; in
// the wide view they popped in and out in plain sight. The user (2026-10-28,
// slot 1 = KA) asked for them to appear and move earlier and leave later so
// it looks right at the enhanced resolution, sized for 270 with the fade-in
// unseen at 216, and accepted NPC timing that differs from the original
// game.
//   Arrivals (kArrivals89): the step that makes them runs from our ops: the
// game's own ops up to and with the make, then each one put back out of
// sight and walked (glided) to where the game made it.
//   Departures: a step that starts on deletes (09 id ...) runs from our ops:
// each one still in sight fades out at once (a walker walking on as it
// does), then the deletes.
//   Both fade (Unit::fade, gulls_unit_fade) by how far their sprite lies
// past the 384 x 216 view, gone at the 480 x 270 one: at 216 nothing shows.
// Those that stand can't walk (no walk to walk with), so they fade in or
// out over kSceneFadeFrames where the game makes or deletes them; that
// shows a little at 216, where the game made them pop. The fade is a card
// fade: with the perspective off they pop as before.
// KOL_SCENE89_POPS=1: the game's way.
constexpr std::uint16_t kTownSceneMap = 89;
constexpr std::uint16_t kDockSceneMap = 2;          // and map 2's after the rainy battle (scene_dock)
constexpr std::uint32_t kDockSceneScript = 0x08772662u;
constexpr std::uint32_t kTownSceneScript = 0x0876FFA0u;
constexpr std::uint32_t kSceneBuf = 0x0203F600u, kSceneBufStride = 0x100u;   // 4 of ours; u16 back at +0xFE, u32 script at +0xF8
constexpr int kSceneBufs = 4;
constexpr std::uint32_t kSceneFade = 0x0203FE00u;   // per actor id: 0 none, else 1 + 254 x opacity
struct Arrival { int pc, end; std::uint8_t actors[2]; int dx, dz; std::uint8_t after[4]; };
constexpr Arrival kArrivals89[] = {
    {0x13F, 0x144, {0x51, 0x00}, -128, -128, {}},   // at the top: from further up
    {0x14E, 0x153, {0x5C, 0x00}, -128, -128, {}},
    {0x180, 0x185, {0x55, 0x00}, -128, -128, {}},
    {0x374, 0x379, {0x50, 0x00}, 128, 128, {}},   // at the bottom: from further down
    // Two who stand, made at the top edge as the camera pans up (0x4F inside
    // the game's own screen): no walk to walk in with (asked to, the game
    // spun in 0x0801355A), and made any earlier they found no sprite slot
    // (they take 0x31's and 0x32's, deleted just before): they fade in.
    {0x0DB, 0x0FC, {0x6E, 0x4F}, 0, 0, {}},
};
const bool g_scene89_on = [] {
    const char* v = std::getenv("KOL_SCENE89_POPS");
    return !(v && v[0] && v[0] != '0');
}();
constexpr std::uint16_t kMarshMap = 16;              // Shiven's arrival (scene_shiven)
constexpr std::uint32_t kMarshScript = 0x08774B75u;
const bool g_shiven_on = [] {
    const char* v = std::getenv("KOL_MAP16_SHIVEN_POPS");
    return !(v && v[0] && v[0] != '0');
}();
// The views, in the game's screen pixels (the 240 x 160 screen at 0,0).
constexpr int kInX0 = -72, kInX1 = 312, kInY0 = -28, kInY1 = 188;      // 384 x 216
constexpr int kOutX0 = -120, kOutX1 = 360, kOutY0 = -55, kOutY1 = 215;  // 480 x 270
struct SceneMark { std::uint8_t mode = 0; int age = 0; std::uint32_t hid = 0; };   // mode 1 fading in, 3 fading out; hid: kept from the draw list
constexpr int kSceneFadeFrames = 20;
SceneMark g_mark[256];
struct Hist { int x[16], z[16]; int n = 0; };
Hist g_hist[256];
unsigned long long g_scene_epoch = ~0ull;

// An object's sprite on the game's screen (its top-left; 16 x 32), from the
// sprite slot nearest its place (a slot is a frame behind a walker).
bool sprite_box(std::uint32_t obj, int* sx, int* sy) {
    const int x = static_cast<std::int16_t>(bus_read_u16(obj + 0x10));
    const int z = static_cast<std::int16_t>(bus_read_u16(obj + 0x14));
    int best = -1, bd = 7;
    for (int s = 0; s < kSlots; ++s) {
        const std::uint32_t o = kSlotArray + static_cast<std::uint32_t>(s) * kSlotStride;
        if (!bus_read_u32(o)) continue;
        const int d = std::abs(static_cast<std::int16_t>(bus_read_u16(o + 0x08)) - x) +
                      std::abs(static_cast<std::int16_t>(bus_read_u16(o + 0x0C)) - z);
        if (d < bd) { bd = d; best = s; }
    }
    if (best < 0) return false;
    const std::uint32_t o = kSlotArray + static_cast<std::uint32_t>(best) * kSlotStride;
    *sx = static_cast<std::int16_t>(bus_read_u16(o + 0x1C));
    *sy = static_cast<std::int16_t>(bus_read_u16(o + 0x1E));
    return true;
}
// Its opacity there: whole while any of it is in the 216 view, none at the
// 270 one, by the gap (in each axis, against that axis's band).
float band_opacity(int sx, int sy) {
    const int gx = std::max({0, kInX0 - (sx + 16), sx - kInX1});
    const int gy = std::max({0, kInY0 - (sy + 32), sy - kInY1});
    const float fx = static_cast<float>(gx) / static_cast<float>(kInX0 - kOutX0);
    const float fy = static_cast<float>(gy) / static_cast<float>(kInY0 - kOutY0);
    return std::clamp(1.0f - std::max(fx, fy), 0.0f, 1.0f);
}
void scene_buf_op(std::uint32_t& at, std::initializer_list<std::uint8_t> b) {
    for (const std::uint8_t v : b) bus_write_u8(at++, v);
}
// Glides of one actor by (dx, dz) px in pieces a signed byte holds.
void scene_glide(std::uint32_t& at, std::uint8_t actor, int dx, int dz, int speed) {
    while (dx || dz) {
        const int px = std::clamp(dx, -96, 96), pz = std::clamp(dz, -96, 96);
        scene_buf_op(at, {0x10, actor, static_cast<std::uint8_t>(px), static_cast<std::uint8_t>(pz),
                          static_cast<std::uint8_t>(speed)});
        dx -= px;
        dz -= pz;
    }
}
int scene_free_buf() {
    for (int i = 0; i < kSceneBufs; ++i)
        if (!bus_read_u8(kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride)) return i;
    return -1;
}
void scene_run(std::uint32_t ctx, int i, std::uint32_t at, int back) {
    const std::uint32_t buf = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
    // A sleep first: a step must START on the closing 02 for us to send the
    // thread back (run inside a step, 02 ends the thread: the deletes don't
    // wait, and the scene's joins then broke, 0x6E, 0x4F and 0x5C never made).
    scene_buf_op(at, {0x50, 0x01});
    scene_buf_op(at, {0x02});   // never run: the step that starts on it goes back to the game's
    bus_write_u16(buf + 0xFE, static_cast<std::uint16_t>(back));
    bus_write_u32(buf + 0xF8, bus_read_u32(ctx + 0x10));   // the script it came from (map 89's, map 2's)
    bus_write_u32(ctx + 0x10, buf);
    bus_write_u16(ctx, 0);
}
// A thread in ours: back to the game's script at our closing 02.
bool scene_back(std::uint32_t ctx) {
    const std::uint32_t base = bus_read_u32(ctx + 0x10);
    if (base < kSceneBuf || base >= kSceneBuf + kSceneBufs * kSceneBufStride) return false;
    const int pc = static_cast<std::int16_t>(bus_read_u16(ctx));
    if (bus_read_u8(base + static_cast<std::uint32_t>(pc)) != 0x02) return true;   // still ours
    const int back = bus_read_u16(base + 0xFE);
    bus_write_u32(ctx + 0x10, bus_read_u32(base + 0xF8));
    bus_write_u16(ctx, static_cast<std::uint16_t>(back));
    for (std::uint32_t a = base; a < base + kSceneBufStride; ++a) bus_write_u8(a, 0);
    return true;
}
bool scene_arrival(std::uint32_t ctx, int pc) {
    for (const Arrival& a : kArrivals89) {
        if (a.pc != pc) continue;
        const int i = scene_free_buf();
        if (i < 0) return false;
        std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
        for (int o = a.pc; o < a.end; ++o) bus_write_u8(at++, bus_read_u8(kTownSceneScript + static_cast<std::uint32_t>(o)));
        const bool walks = a.dx || a.dz;
        for (const std::uint8_t id : a.actors)
            if (id && walks) scene_glide(at, id, a.dx, a.dz, 0);   // put back, at once
        for (const std::uint8_t id : a.actors) {
            if (!id) continue;
            if (walks) {
                scene_buf_op(at, {0x0B, id, 0x02, 0x10});   // walking
                scene_glide(at, id, -a.dx, -a.dz, 2);
            }
            g_mark[id] = {1, 0};
            bus_write_u8(kSceneFade + id, 1);   // unseen until its first frame's opacity
            bus_write_u8(kSceneFade + 0xFF, 0xA5);
        }
        if (a.after[0]) scene_buf_op(at, {a.after[0], a.after[1], a.after[2], a.after[3]});
        scene_run(ctx, i, at, a.end);
        std::fprintf(stderr, "gulls: map 89 step %03X: %02X%s %s\n", pc, a.actors[0],
                     a.actors[1] ? " and the next" : "", walks ? "walks in from out of sight" : "fades in");
        return true;
    }
    return false;
}
// A step that starts on deletes: each one still in sight fades out at once
// (kSceneFadeFrames), then the deletes. A walker keeps walking the way it
// was going while it fades; one standing fades where it stands (no walk to
// walk with: the game spun in 0x0801355A when asked). Kept, it held a
// sprite slot the next ones made needed (the game spun there too). The user
// (2026-10-28) preferred this to walking them on out of sight first.
bool scene_departure(std::uint32_t ctx, int pc) {
    if (bus_read_u8(kTownSceneScript + static_cast<std::uint32_t>(pc)) != 0x09) return false;
    std::uint8_t ids[8];
    int n = 0;
    for (int o = pc; n < 8 && bus_read_u8(kTownSceneScript + static_cast<std::uint32_t>(o)) == 0x09; o += 2)
        ids[n++] = bus_read_u8(kTownSceneScript + static_cast<std::uint32_t>(o) + 1);
    const int i = scene_free_buf();
    if (i < 0) return false;
    std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
    bool any = false, walks = false;
    for (int k = 0; k < n; ++k) {
        const std::uint8_t id = ids[k];
        const std::uint32_t obj = object_of(id);
        int sx = 0, sy = 0;
        if (!obj || !sprite_box(obj, &sx, &sy) || band_opacity(sx, sy) <= 0.0f) continue;
        g_mark[id] = {3, 0};
        any = true;
        // Its way: its last 16 frames' walk.
        const Hist& h = g_hist[id];
        int wx = 0, wz = 0;
        if (h.n >= 16) {
            wx = h.x[(h.n - 1) & 15] - h.x[h.n & 15];
            wz = h.z[(h.n - 1) & 15] - h.z[h.n & 15];
        }
        const int ux = (wx > 2) - (wx < -2), uz = (wz > 2) - (wz < -2);
        if (!ux && !uz) {
            std::fprintf(stderr, "gulls: map 89 step %03X: %02X fades out (standing)\n", pc, id);
            continue;
        }
        // Speed n is n px a frame; the glide's X and Z are unsigned: it never
        // crosses 0 (Rictor at 0x3EC went the other way for good).
        const int speed = std::clamp((std::abs(wx) + std::abs(wz)) / 16, 1, 2);
        const int ox = static_cast<std::int16_t>(bus_read_u16(obj + 0x10));
        const int oz = static_cast<std::int16_t>(bus_read_u16(obj + 0x14));
        int t = kSceneFadeFrames * speed;
        if (ux < 0) t = std::min(t, ox - 8);
        if (uz < 0) t = std::min(t, oz - 8);
        t = std::max(0, t);
        scene_buf_op(at, {0x0B, id, 0x02, 0x10});   // walking
        scene_glide(at, id, ux * t, uz * t, speed);
        walks = true;
        std::fprintf(stderr, "gulls: map 89 step %03X: %02X fades out walking %d px on (%+d,%+d)\n", pc, id, t, ux, uz);
    }
    if (!any) return false;
    // The rest of the fade (a walker's glide took its share of it).
    scene_buf_op(at, {0x50, static_cast<std::uint8_t>(walks ? 6 : kSceneFadeFrames + 6)});
    for (int k = 0; k < n; ++k) scene_buf_op(at, {0x09, ids[k]});
    scene_run(ctx, i, at, pc + 2 * n);
    return true;
}
// Every frame on map 89 (and 2, 16): each actor's walk, and the faded
// ones' opacity.
void scene89_frame() {
    const std::uint16_t map_now = bus_read_u16(kMapNumber);
    const bool here = (g_scene89_on && (map_now == kTownSceneMap || map_now == kDockSceneMap)) ||
                      (g_shiven_on && map_now == kMarshMap);
    if (g_scene_epoch != g_runtime_state_epoch || !here) {
        if (g_scene_epoch != g_runtime_state_epoch || bus_read_u8(kSceneFade + 0xFF) == 0xA5) {
            for (int a = 0; a < 256; ++a) { g_mark[a] = {}; g_hist[a].n = 0; }
            for (std::uint32_t a = kSceneFade; a < kSceneFade + 0x100; ++a) bus_write_u8(a, 0);
        }
        g_scene_epoch = g_runtime_state_epoch;
        if (!here) return;
    }
    bool seen[256] = {};
    const std::uint32_t list = bus_read_u32(kObjectsPtr);
    if (!in_ewram(list)) return;
    for (int i = 0; i < 26; ++i) {
        const std::uint32_t o = list + static_cast<std::uint32_t>(i) * 0xD0u;
        const std::uint32_t rec = bus_read_u32(o);
        if (!in_ewram(rec)) continue;
        const std::uint8_t id = bus_read_u8(rec + 0x4E);
        seen[id] = true;
        Hist& h = g_hist[id];
        h.x[h.n & 15] = static_cast<std::int16_t>(bus_read_u16(o + 0x10));
        h.z[h.n & 15] = static_cast<std::int16_t>(bus_read_u16(o + 0x14));
        ++h.n;
        SceneMark& m = g_mark[id];
        if (!m.mode) continue;
        // Faded out, then made again elsewhere (an 06 on a living actor
        // moves it: map 2's 0x32): back in the draw list, fading in there.
        if (m.mode == 3 && h.n >= 2) {
            const int jx = h.x[(h.n - 1) & 15] - h.x[(h.n - 2) & 15], jz = h.z[(h.n - 1) & 15] - h.z[(h.n - 2) & 15];
            if (std::abs(jx) + std::abs(jz) > 48) {
                if (m.hid) bus_write_u8(m.hid + 0x17, static_cast<std::uint8_t>(bus_read_u8(m.hid + 0x17) | 0x40u));
                m = {1, 0};
                bus_write_u8(kSceneFade + id, 1);
                continue;
            }
        }
        ++m.age;
        int sx = 0, sy = 0;
        if (!sprite_box(o, &sx, &sy)) continue;
        float f = band_opacity(sx, sy);
        const float t = std::min(1.0f, static_cast<float>(m.age) / static_cast<float>(kSceneFadeFrames));
        if (m.mode == 1) {
            f *= t;
            if (f >= 1.0f && m.age > kSceneFadeFrames) { m = {}; bus_write_u8(kSceneFade + id, 0); continue; }
        } else if (m.mode == 3) {
            f *= 1.0f - t;
            // Faded out: kept from the game's draw list (bit 0x40 of +0x17
            // of the object's [+4], kol_ghost's draw bit) until the deletes.
            // The delete comes two frames before its sprite leaves the
            // picture, and that sprite, its unit gone, came back whole (the
            // user saw it). A make sets the bit again (0x6E/0x4F, made in
            // 0x31/0x32's places), so nothing is put back.
            const std::uint32_t drawn = bus_read_u32(o + 4);
            if (t >= 1.0f && !m.hid && in_ewram(drawn)) {
                const std::uint8_t b = bus_read_u8(drawn + 0x17);
                if (b & 0x40u) {
                    bus_write_u8(drawn + 0x17, static_cast<std::uint8_t>(b & ~0x40u));
                    m.hid = drawn;
                }
            }
        }
        bus_write_u8(kSceneFade + id, static_cast<std::uint8_t>(1 + std::lround(254.0f * f)));
        bus_write_u8(kSceneFade + 0xFF, 0xA5);   // (marks to clear)
    }
    for (int a = 0; a < 255; ++a)
        if (g_mark[a].mode && !seen[a] && g_mark[a].age > 0) {   // deleted
            g_mark[a] = {};
            bus_write_u8(kSceneFade + static_cast<std::uint32_t>(a), 0);
        }
}

// Map 2 after the rainy battle ("Find him!", states/thor/0013): at its main
// thread's step 0x247 the scene makes 0x31 at the map's west edge (0,10) and
// makes 0x32, the soldier standing on the left, again at 1,1 (an 06 on a
// living actor moves it): she vanished from the left and popped in at the
// top; 0x32's thread then waits 50 frames (50 32 at 0x265) before her walk.
// The user (2026-10-28): she walks a tile toward the bottom left (+Z) and
// fades out; the two who come in fade in and walk at once. The main step
// makes 0x31 (fading in) and goes on at 0x251 (his thread walks at once);
// her thread, in place of its wait, walks her off fading, makes her at 1,1
// (fading in) and walks her route; his route is cut into one-cell legs.
constexpr int kDockStep = 0x247, kDockBack = 0x251, kDockMake32 = 0x24C, kDockWait = 0x265, kDockWaitEnd = 0x267,
              kDockRoute31 = 0x25E;
constexpr std::uint32_t kDockRoute31Index = 1;   // op 16's route 0x42: the scene's route table entry 1
bool scene_dock(std::uint32_t ctx, int pc) {
    if (pc == kDockWait) {
        // 0x32's thread, in place of its wait (50 32): she walks a tile
        // toward the bottom left fading out, is made at 1,1 (the main
        // thread's 06 32 01 01 02, moved here) and walks her route at once.
        const int i = scene_free_buf();
        if (i < 0) return false;
        std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
        // Facing the way she walks (the user asked her to face the way
        // she walks): op 1A id dir, dir as the walker's +0x48 (0 +X, 1 +Z,
        // 2 -X, 3 -Z); she stood facing +X.
        scene_buf_op(at, {0x1A, 0x32, 0x01});
        scene_buf_op(at, {0x0B, 0x32, 0x02, 0x10});   // walking
        scene_glide(at, 0x32, 0, 32, 2);              // a tile toward the bottom left (+Z)
        scene_buf_op(at, {0x50, static_cast<std::uint8_t>(kSceneFadeFrames - 16 + 6)});   // the rest of her fade
        for (int o = kDockMake32; o < kDockMake32 + 5; ++o)
            bus_write_u8(at++, bus_read_u8(kDockSceneScript + static_cast<std::uint32_t>(o)));
        g_mark[0x32] = {3, 0};
        bus_write_u8(kSceneFade + 0xFF, 0xA5);
        scene_run(ctx, i, at, kDockWaitEnd);
        return true;
    }
    if (pc == kDockRoute31) {
        // 0x31's thread, before his route (16 31 42: the scene's route 1,
        // 0,7 to 6,7 a cell a step). Made at 0,10, he walked to 1,7 as one
        // leg, a tile east and three north, which no height profile of the
        // game's covers (0x08096AD8 leaves such a leg's table as the heap
        // had it): his Y read 0xFFFF, 0x7370, 0, -1 and a stale jump's
        // 102..83, and his sprite jumped and sank under his shadow (off the
        // cartridge's screen; the user saw him appear in mid-air). The
        // route now goes from where he stands a cell at a time: 0,10, 1,10,
        // 1,9, 1,8, 1,7 (all at 5; 0,9 is a rock), then 2,7 on as the game's.
        const std::uint32_t scene = bus_read_u32(kSceneRoutesPtr);
        const std::uint32_t routes = in_ewram(scene) ? bus_read_u32(scene + 0x18) : 0;
        if (!in_ewram(routes)) return false;
        const std::uint32_t r = routes + kDockRoute31Index * 36u;
        static constexpr std::uint8_t kFrom[7][2] = {{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}};
        static constexpr std::uint8_t kTo[10][2] = {{0, 10}, {1, 10}, {1, 9}, {1, 8}, {1, 7},
                                                   {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}};
        if (bus_read_u32(r + 0x20) != 7u) return false;   // the game's route, untouched
        for (int k = 0; k < 7; ++k)
            if (bus_read_u8(r + 2u * k) != kFrom[k][0] || bus_read_u8(r + 2u * k + 1u) != kFrom[k][1]) return false;
        for (int k = 0; k < 10; ++k) {
            bus_write_u8(r + 2u * k, kTo[k][0]);
            bus_write_u8(r + 2u * k + 1u, kTo[k][1]);
        }
        bus_write_u32(r + 0x20, 10u);
        std::fprintf(stderr, "gulls: map 2: 0x31's route a cell at a time from 0,10\n");
        return false;   // the step runs his route
    }
    if (pc != kDockStep) return false;
    const int i = scene_free_buf();
    if (i < 0) return false;
    std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
    auto rom_op = [&](int from, int n) {
        for (int o = from; o < from + n; ++o) bus_write_u8(at++, bus_read_u8(kDockSceneScript + static_cast<std::uint32_t>(o)));
    };
    rom_op(0x247, 5);                            // 06 31 00 0A 02: 0x31 at the west edge
    // (0x32's make at 1,1, 06 32 01 01 02 at 0x24C, is her thread's now.)
    g_mark[0x31] = {1, 0};
    bus_write_u8(kSceneFade + 0x31, 1);
    bus_write_u8(kSceneFade + 0xFF, 0xA5);
    scene_run(ctx, i, at, kDockBack);
    std::fprintf(stderr, "gulls: map 2 step %03X: 0x31 fades in; 0x32 walks off fading, then fades in at 1,1\n", pc);
    return true;
}

// Map 16 before its battle ("Familiar Voice": "I finally caught up with
// you.", states/thor/0093, script 0x08774B75): after A the main thread's
// step at 0x49 jumps to 0x67, makes Shiven (actor 0x07) at 12,2 with
// `06 07 0C 02 0A`, turns Alphonse to him (1A 02 00) and waits on his line
// (22 02 02 00 at 0x6F); his walk to 7,2 and 7,4 comes after it. 12,2 is
// just past the cartridge's right edge (game screen x 250) and well inside
// the wide view: he popped in there. The user (2026-11-02) asked for him to
// appear further right, off screen, then for Shiven to start walking only
// after Alphonse's line.
// Now he is made there as before and put six cells further along his own
// line at once (18,2: the row is flat at height 2 out to 18), unseen;
// Alphonse turns to the voice, as in the game, and says "You're...". Then
// the game's own walk brings him in: route 4, which the main thread builds
// after the line (15 4C 1C 82 at 0x73: 12,2 -> 7,2), starts at 18,2
// instead (15 52 1C 82), and he fades in through the 216 -> 270 band from
// the walk's start (its thread's step at 0x85). The walk is 96 frames
// longer (192 px at his 2 a frame), so the thread at 0x9D that turns
// Alphonse to where he stops (50 20, then 1A 02 03) waits that much longer.
// A first try walked him in before the line and stood him there: he went
// on walking on the spot (the user saw it on the Thor).
// KOL_MAP16_SHIVEN_POPS=1: the game's.
constexpr int kShivenStep = 0x49, kShivenMake = 0x67, kShivenLine = 0x6F;
constexpr int kShivenRoute = 0x73, kShivenRouteNext = 0x77, kShivenWalk = 0x85;
constexpr int kAlphonseTurnWait = 0x9D, kAlphonseTurn = 0x9F;
constexpr std::uint8_t kShiven = 0x07;
constexpr int kShivenCells = 6;
constexpr int kShivenWalkFrames = 32 * kShivenCells / 2;
unsigned long long g_shiven_out = ~0ull;   // the state epoch he was put further in
bool scene_shiven(std::uint32_t ctx, int pc) {
    const bool out = g_shiven_out == g_runtime_state_epoch;
    if (pc == kShivenRoute && out && script_has(kMarshScript + kShivenRoute, {0x15, 0x4C, 0x1C, 0x82})) {
        // u16 from x | from z << 5 | to x << 10, then to z | route << 5.
        constexpr int route = (12 + kShivenCells) | 2 << 5 | 7 << 10;
        const int i = scene_free_buf();
        if (i < 0) return false;
        std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
        scene_buf_op(at, {0x15, static_cast<std::uint8_t>(route & 0xFF), static_cast<std::uint8_t>(route >> 8), 0x82});
        scene_run(ctx, i, at, kShivenRouteNext);
        return true;
    }
    if (pc == kShivenWalk && out && script_has(kMarshScript + kShivenWalk, {0x17, kShiven, 0x04, 0x05})) {
        g_mark[kShiven] = {1, 0};   // fading in from his walk's first step
        bus_write_u8(kSceneFade + 0xFF, 0xA5);
        std::fprintf(stderr, "gulls: map 16 step %03X: Shiven walks in from %d cells further\n", pc, kShivenCells);
        return false;   // the game's walk
    }
    if (pc == kAlphonseTurnWait && out && script_has(kMarshScript + kAlphonseTurnWait, {0x50, 0x20, 0x1A, 0x02, 0x03})) {
        const int i = scene_free_buf();
        if (i < 0) return false;
        std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
        scene_buf_op(at, {0x50, static_cast<std::uint8_t>(0x20 + kShivenWalkFrames - 1)});   // (scene_run adds 1)
        scene_run(ctx, i, at, kAlphonseTurn);
        return true;
    }
    if (pc != kShivenStep || !script_has(kMarshScript + kShivenStep, {0x6D, 0x1B, 0x00}) ||
        !script_has(kMarshScript + kShivenMake, {0x06, kShiven, 0x0C, 0x02, 0x0A, 0x1A, 0x02, 0x00, 0x22}))
        return false;
    const int i = scene_free_buf();
    if (i < 0) return false;
    std::uint32_t at = kSceneBuf + static_cast<std::uint32_t>(i) * kSceneBufStride;
    scene_buf_op(at, {0x06, kShiven, 0x0C, 0x02, 0x0A});   // made at 12,2
    scene_glide(at, kShiven, 32 * kShivenCells, 0, 0);     // put further, at once
    scene_buf_op(at, {0x1A, 0x02, 0x00});                  // Alphonse turns to the voice
    bus_write_u8(kSceneFade + kShiven, 1);   // unseen (no mark: kept so) until his walk
    bus_write_u8(kSceneFade + 0xFF, 0xA5);
    g_shiven_out = g_runtime_state_epoch;
    scene_run(ctx, i, at, kShivenLine);
    std::fprintf(stderr, "gulls: map 16 step %03X: Shiven made %d cells further, unseen\n", pc, kShivenCells);
    return true;
}

int on_step(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_early_epoch != g_runtime_state_epoch) {
        g_early_epoch = g_runtime_state_epoch;
        g_early_state = 0;
        g_early_ctx = 0;
        g_alph_state = 0;
        g_alph_ctx = 0;
        g_alph_game_ctx = 0;
    }
    const std::uint32_t ctx = cpu->R[0];
    if (!in_ewram(ctx) || walk_in_back(ctx) || ship_soldier_back(ctx) || scene_back(ctx)) return 0;
    if (!scenes_on()) return 0;
    if (g_shiven_on && bus_read_u32(ctx + 0x10) == kMarshScript && bus_read_u16(kMapNumber) == kMarshMap) {
        scene_shiven(ctx, static_cast<std::int16_t>(bus_read_u16(ctx)));
        return 0;
    }
    if (g_scene89_on && bus_read_u32(ctx + 0x10) == kDockSceneScript && bus_read_u16(kMapNumber) == kDockSceneMap) {
        scene_dock(ctx, static_cast<std::int16_t>(bus_read_u16(ctx)));
        return 0;
    }
    if (g_scene89_on && bus_read_u32(ctx + 0x10) == kTownSceneScript && bus_read_u16(kMapNumber) == kTownSceneMap) {
        const int at = static_cast<std::int16_t>(bus_read_u16(ctx));
        if (!scene_arrival(ctx, at)) scene_departure(ctx, at);
        return 0;
    }
    if (bus_read_u32(ctx + 0x10) == kShipScript) {
        const int at = static_cast<std::int16_t>(bus_read_u16(ctx));
        if (!ship_soldier_made(ctx, at)) ship_soldier_walk(at);
        return 0;
    }
    if (bus_read_u32(ctx + 0x10) != kHarbourScript) return 0;
    const int pc = static_cast<std::int16_t>(bus_read_u16(ctx));
    if (town_walk_in(ctx, pc) || archers_stay(ctx, pc)) return 0;
    if (pc == kWalkOp && bus_read_u16(kMapNumber) == kHarbourMap) lengthen_walk();
    if (bus_read_u16(kMapNumber) == kHarbourMap && early_alphonse(ctx, pc)) return 0;
    if (g_early_state >= 3) return 0;
    static const bool debug = std::getenv("KOL_GULL_DEBUG") != nullptr;
    if (debug) std::fprintf(stderr, "quay: step ctx %08X at %02X (state %d, map %u)\n", ctx, pc, g_early_state, bus_read_u16(kMapNumber));
    if (bus_read_u16(kMapNumber) != kHarbourMap) return 0;
    auto go = [&](int to) { bus_write_u16(ctx, static_cast<std::uint16_t>(to)); };
    if (g_early_state == 0 && pc == kAfterSleep) {
        g_early_ctx = ctx;
        g_early_state = 1;
        go(kPeople);
        std::fprintf(stderr, "gulls: the quay's two people made before the gulls fly (thread %08X)\n", ctx);
    } else if (g_early_state == 1 && ctx == g_early_ctx && pc == kPennants) {
        bus_write_u8(kFlag112Byte, static_cast<std::uint8_t>(bus_read_u8(kFlag112Byte) | kFlag112Bit));
    } else if (g_early_state == 1 && ctx == g_early_ctx && pc == kAfterPeople) {
        g_early_state = 2;
        go(kAfterSleep);
    } else if (g_early_state == 2 && ctx == g_early_ctx && pc == kAfterJoin) {
        g_early_state = 3;
        go(kSleepOne);
    }
    return 0;   // the step runs the op at the offset now
}

// The game's shadow blob: none for the street soldier while he fades, and
// none under a town actor walking in off the grid (west of column 0 the game
// has no ground, so it put the blob at height 0, 176 px under his feet).
int on_blob(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t obj = cpu->R[1];
    const bool fading = g_fade_obj && g_fade_epoch == g_runtime_state_epoch && obj == g_fade_obj;
    const bool off_grid = g_walk_in_on && in_ewram(obj) && bus_read_u16(kMapNumber) == kTownMap &&
                          (static_cast<std::int16_t>(bus_read_u16(obj + 0x10)) < 0 ||
                           static_cast<std::int16_t>(bus_read_u16(obj + kObjZ)) < 0);
    const bool walking_in = in_ewram(obj) && live_fade(obj) < 1.0f;
    if (!fading && !off_grid && !walking_in) return 0;
    cpu->R[15] = cpu->R[14] & ~1u;   // back to the flush, no blob
    return 1;
}

// The terrain in front of a town actor walking in off the grid. The game
// works out each of a unit's four sprite quarters' priority against it
// (0x03005DE0, from the place its move started, object +0x16): west of
// column 0 it takes the ground there as height -255, and for the last 22 px
// before the grid the edge column's terrain then covered his lower half
// (priority 3): the archers' bodies vanished as they walked in, their heads
// still drawn (states/HP_slot2_town_archers, the wide view). The call is made
// again with the place put on the grid's edge, where his walk goes on, so
// the ground under him is the edge cell's.
bool g_pass_occlusion = false;

int on_occlusion(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_pass_occlusion) {
        g_pass_occlusion = false;
        return 0;
    }
    if (!g_walk_in_on || bus_read_u16(kMapNumber) != kTownMap) return 0;
    const bool west = static_cast<std::int16_t>(cpu->R[2]) < 0;
    const bool north = static_cast<std::int16_t>(bus_read_u32(cpu->R[13])) < 0;
    if (!west && !north) return 0;
    if (west) cpu->R[2] = 0;
    if (north) bus_write_u32(cpu->R[13], 0);
    cpu->R[15] = kOcclusion;   // the same call again, on the grid
    g_pass_occlusion = true;
    return 1;
}

// The fade's state goes back where it still holds our values.
void fade_restore() {
    if (g_fade_obj && in_ewram(g_fade_obj)) {
        const std::uint16_t f = bus_read_u16(g_fade_obj + kObjFlags);
        bus_write_u16(g_fade_obj + kObjFlags, static_cast<std::uint16_t>(f & ~kSemi));
    }
    if (g_fade_blend) {
        if (bus_read_u16(kBldcnt) == (g_fade_old_cnt | kSecondTargets)) bus_write_u16(kBldcnt, g_fade_old_cnt);
        if (bus_read_u16(kBldalpha) == g_fade_alpha) bus_write_u16(kBldalpha, g_fade_old_alpha);
    }
    g_fade_obj = 0;
    g_fade_blend = false;
}

}  // namespace

void gulls_frame() {
    scene89_frame();
    if ((g_alph_state == 1 || g_alph_state == 2) && g_early_epoch == g_runtime_state_epoch &&
        bus_read_u16(kMapNumber) == kHarbourMap)
        show_alphonse(false);   // not drawn until he walks
    if (g_fade_epoch != g_runtime_state_epoch) {   // a state load put memory back
        g_fade_epoch = g_runtime_state_epoch;
        g_fade_obj = 0;
        g_fade_blend = false;
        g_walk_longer = false;
    }
    if (!g_walk_longer) return;
    // The soldier: the object whose unit record is actor 0x31's.
    std::uint32_t walker = 0;
    const std::uint32_t list = bus_read_u32(kObjectsPtr);
    if (bus_read_u16(kMapNumber) == kHarbourMap && in_ewram(list))
        for (int i = 0; i < 26 && !walker; ++i) {
            const std::uint32_t o = list + static_cast<std::uint32_t>(i) * 0xD0u;
            const std::uint32_t rec = bus_read_u32(o);
            if (in_ewram(rec) && bus_read_u8(rec + 0x4E) == kWalkerActor) walker = o;
        }
    if (!walker) {   // deleted (or another scene): the fade is over
        if (g_fade_obj || g_fade_blend) {
            fade_restore();
            std::fprintf(stderr, "gulls: the street soldier gone; blending put back\n");
        }
        g_walk_longer = false;
        return;
    }
    const int z = static_cast<std::int16_t>(bus_read_u16(walker + kObjZ));
    if (z > kFadeFrom) return;
    if (!g_fade_obj) {
        const std::uint16_t cnt = bus_read_u16(kBldcnt);
        if ((cnt & 0x00FFu) != 0) return;   // the game blends: leave it to the game
        g_fade_old_cnt = cnt;
        g_fade_old_alpha = bus_read_u16(kBldalpha);
        g_fade_blend = true;
        g_fade_obj = walker;
        std::fprintf(stderr, "gulls: the street soldier fades out (object %08X)\n", walker);
    }
    const std::uint16_t f = bus_read_u16(walker + kObjFlags);
    if (!(f & kSemi)) bus_write_u16(walker + kObjFlags, static_cast<std::uint16_t>(f | kSemi));
    const int eva = std::clamp((16 * (z - kFadeTo) + (kFadeFrom - kFadeTo) / 2) / (kFadeFrom - kFadeTo), 0, 16);
    g_fade_alpha = static_cast<std::uint16_t>(eva | ((16 - eva) << 8));
    bus_write_u16(kBldcnt, static_cast<std::uint16_t>(g_fade_old_cnt | kSecondTargets));
    bus_write_u16(kBldalpha, g_fade_alpha);
}

float gulls_unit_fade(const std::uint8_t* ew, std::uint8_t actor, int x) {
    if (!ew) return 1.0f;
    auto at = [ew](std::uint32_t a) { return a - 0x02000000u; };
    if (const std::uint8_t v = ew[at(kSceneFade + actor)]; v && actor != 0xFF)   // map 89's townspeople
        return static_cast<float>(v - 1) / 254.0f;
    return walk_in_fade([&](std::uint32_t a) { return ew[at(a)]; },
                        [&](std::uint32_t a) {
                            return static_cast<std::uint16_t>(ew[at(a)] | (ew[at(a) + 1] << 8));
                        },
                        actor, x);
}

void gulls_set_view_margin(int extra_right) { g_extra_right = extra_right > 0 ? extra_right : 0; }

void gulls_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* no_walk = std::getenv("KOL_NO_LONG_WALK");
    if ((!(no_walk && no_walk[0] && no_walk[0] != '0') || g_walk_in_on) &&
        (!gba_mod_register_function_entry_plugin(kBlobId, kBlob, 1, &on_blob) ||
         !gba_mod_set_function_hook_enabled(kBlobId, 1)))
        std::fprintf(stderr, "gulls: could not hook the blob %08X\n", kBlob);
    if (g_walk_in_on &&
        (!gba_mod_register_function_entry_plugin(kOcclusionId, kOcclusion, 1, &on_occlusion) ||
         !gba_mod_set_function_hook_enabled(kOcclusionId, 1)))
        std::fprintf(stderr, "gulls: could not hook the occlusion %08X\n", kOcclusion);
    const char* no_early = std::getenv("KOL_NO_QUAY_EARLY");
    if (!(no_early && no_early[0] && no_early[0] != '0') &&
        (!gba_mod_register_function_entry_plugin(kStepId, kStep, 1, &on_step) ||
         !gba_mod_set_function_hook_enabled(kStepId, 1)))
        std::fprintf(stderr, "gulls: could not hook the scene step %08X\n", kStep);
    const char* off = std::getenv("KOL_NO_GULL_SHIFT");
    if (off && off[0] && off[0] != '0') return;
    if (!gba_mod_register_function_entry_plugin(kMoverId, kMover, 1, &on_mover) ||
        !gba_mod_set_function_hook_enabled(kMoverId, 1))
        std::fprintf(stderr, "gulls: could not hook the mover %08X\n", kMover);
}

}  // namespace kol
