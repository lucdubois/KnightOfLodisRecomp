// kol_scene_strip.cpp — see the header.
//
// The two scenes, as the game sets them up (a cold boot, measured):
//   the opening's panning mountain (frames ~680-2045): mode 0, BG0CNT 0x990B
//     (the mountain and chasm: 256x512, char 0x8000, screen 0xC800, priority
//     3, panned down by BG0VOFS) and BG1CNT 0x9B08 (the fire: 256x512,
//     screen 0xD800, priority 0). BLDCNT 0x3D42 blends BG1 over BG0 at
//     BLDALPHA 8/8. The narration is sprites, never below the screen.
//   the title screen (frame ~2100 on, through PUSH START and its menu):
//     mode 0, BG0CNT 0x1D03 (the clouds: 256x256, a palette bank per map row
//     for the bands), BG1CNT 0x1E02 (the emblem over an opaque near-black
//     field, 240x160 of it) and BG2CNT 0x1F01 (logo and text). BLDCNT 0x2142
//     blends BG1 over the clouds at BLDALPHA 15/5: the veil that darkens them.
// The scenes' task pointers are shared slots that change under them, so the
// layer layout is what identifies them; no other scene sampled uses either.
//
// Past the game's 160 lines each scene carries on as one picture:
//   - the title's veil: BG1 outside lines 0-159 samples one pixel of its
//     field (its layer is transparent there, so the clouds showed unveiled,
//     and the emblem would otherwise repeat every 256 rows);
//   - the opening's fire wobbles by per-line HOFS: an HBlank DMA from a
//     160-line table that 0x0800CAEC fills each frame (HOFS = base +
//     (sin(phase) >> shift) on lines [start, end), the phase stepping each
//     line). A record-only hook keeps each table's parameters, and lines 160
//     on continue the same wave;
//   - the title's logo layer (BG2) is left out.
// The top screen's own rows past 160 get the same through KoL's widescreen
// providers (kol_widescreen.cpp), and the second screen's strip is composed
// here, so the two meet.
//
// The strip's layers are composed as the PPU does it for these registers:
// by priority, the top two opaque pixels, BLDCNT's alpha blend (effect 1) or
// brightness (2/3), in gbarecomp's arithmetic. Windows and mosaic are off in
// both scenes. Palette fades need nothing: the palette is read as it is.

#include "kol_scene_strip.h"

#include "mod_function_hooks.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "gba_bus.h"
#include "gba_ppu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace kol {
namespace {

struct Scene {
    std::uint16_t cnt[4];   // BG0-3 control as the scene sets it (0: any)
    unsigned layers;        // which layers continue past line 160
};
constexpr Scene kOpening = {{0x990B, 0x9B08, 0, 0}, 0x3u};
constexpr Scene kTitle = {{0x1D03, 0x1E02, 0x1F01, 0}, 0x3u};
constexpr int kTitleVeilLayer = 1;

std::uint16_t rd16(const std::uint8_t* m, std::uint32_t o) {
    return static_cast<std::uint16_t>(m[o] | (m[o + 1] << 8));
}

// ── The wave's parameters, as the game fills each table ────────────────
constexpr std::uint32_t kFillTable = 0x0800CAECu;   // (params, table)
constexpr std::uint32_t kSineLiteral = 0x08007F00u; // 0x08007EF0's table pointer
constexpr char kHookId[] = "kol.raster-params-seen";
struct Wave {
    std::uint32_t table = 0;
    std::uint64_t filled = 0;   // frame_count() when the game filled the table
    std::int16_t base = 0, start = 0, end = 0;
    std::uint8_t phase = 0, step = 0, shift = 0;
    std::uint16_t scale = 0;
};
// The last fills, several per table (the game double-buffers them, and the
// second screen draws a frame the game has already moved past).
constexpr int kFills = 8;
std::mutex g_waves_mutex;
Wave g_waves[kFills];
int g_wave_next = 0;

int on_fill_table(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t p = cpu->R[0];
    Wave w;
    w.table = cpu->R[1];
    w.base = static_cast<std::int16_t>(bus_read_u16(p));
    w.start = static_cast<std::int16_t>(bus_read_u16(p + 4));
    w.end = static_cast<std::int16_t>(bus_read_u16(p + 6));
    w.phase = bus_read_u8(p + 8);
    w.step = bus_read_u8(p + 9);
    w.shift = bus_read_u8(p + 0xA);
    w.scale = bus_read_u16(p + 0xC);
    if (gba::GbaPpu* ppu = gbarecomp::active_ppu()) w.filled = ppu->frame_count();
    static const bool log_on = [] { const char* e = std::getenv("KOL_STRIP_LOG"); return e && e[0] && e[0] != '0'; }();
    if (log_on)
        std::printf("strip: fill tbl=%08X params=%08X lr=%08X at frame=%llu base=%d [%d,%d) ph=%u st=%u sh=%u sc=%u\n",
                    w.table, p, cpu->R[14], static_cast<unsigned long long>(w.filled), w.base, w.start, w.end,
                    w.phase, w.step, w.shift, w.scale);
    std::lock_guard<std::mutex> lock(g_waves_mutex);
    g_waves[g_wave_next] = w;
    g_wave_next = (g_wave_next + 1) % kFills;
    return 0;   // the game fills its table as always
}

const std::int16_t* sine_table() {
    static const std::int16_t* t = [] () -> const std::int16_t* {
        gba::GbaBus* bus = gbarecomp::active_bus();
        if (!bus || !bus->rom_ptr()) return nullptr;
        const std::uint8_t* rom = bus->rom_ptr();
        const std::uint32_t lit = kSineLiteral - 0x08000000u;
        const std::uint32_t at = static_cast<std::uint32_t>(rom[lit] | (rom[lit + 1] << 8) |
                                                            (rom[lit + 2] << 16) | (rom[lit + 3] << 24));
        if (at < 0x08000000u || at - 0x08000000u + 512u > bus->rom_size()) return nullptr;
        return reinterpret_cast<const std::int16_t*>(rom + (at - 0x08000000u));
    }();
    return t;
}

// The wave's HOFS on line y, as 0x0800CAEC computes it for its lines, and the
// base beyond its range; false when there is no wave for that table. A range
// that ends where the screen does (160) is the screen's end, not the wave's:
// past it the wave goes on.
bool wave_hofs(const Wave& w, int y, int* hofs) {
    const std::int16_t* sine = sine_table();
    if (!sine || w.shift > 15) return false;
    const bool open_end = w.end >= 160 && y >= 160;
    if (y < w.start || (y >= w.end && !open_end)) { *hofs = w.base; return true; }
    const unsigned a = (w.phase + static_cast<unsigned>(y) * w.step) & 0xFFu;
    int s = sine[a] >> w.shift;
    if (w.scale) s = ((256 - static_cast<int>(w.scale)) * s) >> 8;   // 0x0800CB54's branch
    *hofs = w.base + s;
    return true;
}

// The wave feeding layer bg's HOFS in a frame whose registers are `io`: the
// HBlank DMA channel writing that layer's scroll, the table its source points
// into, and of that table's fills the last one made before frame `frame`
// (~0: the last one). The game fills a table during a VBlank, after that
// frame was drawn, so a frame is drawn from fills stamped before it.
bool wave_for(const std::uint8_t* io, int bg, Wave* out, std::uint64_t frame = ~0ull) {
    const std::uint32_t dest = 0x04000010u + 4u * static_cast<unsigned>(bg);
    for (int ch = 0; ch < 4; ++ch) {
        const std::uint32_t b = 0xB0u + 12u * static_cast<unsigned>(ch);
        const unsigned ctl = io[b + 10] | (io[b + 11] << 8);
        if (!(ctl & 0x8000u) || ((ctl >> 12) & 3u) != 2u) continue;   // enabled, HBlank
        const std::uint32_t dad = io[b + 4] | (io[b + 5] << 8) | (io[b + 6] << 16) |
                                  (static_cast<std::uint32_t>(io[b + 7]) << 24);
        if (dad != dest) continue;
        const std::uint32_t sad = io[b] | (io[b + 1] << 8) | (io[b + 2] << 16) |
                                  (static_cast<std::uint32_t>(io[b + 3]) << 24);
        std::lock_guard<std::mutex> lock(g_waves_mutex);
        const Wave* best = nullptr;
        for (const Wave& w : g_waves)
            if (w.table && sad >= w.table && sad < w.table + 161u * 4u && w.filled < frame &&
                (!best || w.filled > best->filled))
                best = &w;
        if (best) { *out = *best; return true; }
    }
    return false;
}

// ── The fire before the game starts its wave ────────────────────────────
// The game sets the opening's raster effects up 31 frames into the scene (its
// parameter block at 0x0202BDF4 is zero until then): on a GBA the fire is far
// below the screen, so nobody saw it stand still. Below the screen it shows
// from the first frame, so until the game's first fill the fire (BG1) runs
// the same wave, as the game's own would have been: its first drawn frame has
// phase 2 and each frame adds 2 (step 10 a line, sine >> 5), so the frame the
// game takes over on carries straight on. KOL_NO_FIRE_PREROLL=1: off.
constexpr int kFireLayer = 1;
constexpr int kFireLead = 30;       // frames from the scene's first frame to phase 0
constexpr int kFirePhaseStep = 2, kFireLineStep = 10, kFireShift = 5;
std::atomic<long long> g_opening_start{-1};   // first frame the opening's layout was up

bool fire_preroll(int bg, std::uint64_t frame, int base_hofs, Wave* out) {
    static const bool off = [] { const char* e = std::getenv("KOL_NO_FIRE_PREROLL"); return e && e[0] && e[0] != '0'; }();
    const long long start = g_opening_start.load(std::memory_order_relaxed);
    if (off || bg != kFireLayer || start < 0) return false;
    const long long rel = static_cast<long long>(frame) - start;
    if (rel < 0 || rel > kFireLead + 2) return false;     // only until the game's own wave is due
    Wave w;
    w.base = static_cast<std::int16_t>(base_hofs >= 256 ? base_hofs - 512 : base_hofs);
    w.start = 0;
    w.end = 160;
    w.phase = static_cast<std::uint8_t>((kFirePhaseStep * (rel - kFireLead)) & 0xFF);
    w.step = kFireLineStep;
    w.shift = kFireShift;
    *out = w;
    return true;
}

// ── Which scene, and the title's veil row ───────────────────────────────
std::atomic<int> g_scene{0};        // 0 none, 1 the opening, 2 the title
std::atomic<int> g_veil_x{-1}, g_veil_y{-1};   // a pixel of the title's BG1 field

int scene_of(const std::uint8_t* io, gba::GbaBus* bus) {
    auto reg = [&](std::uint32_t r) -> unsigned {
        return io ? static_cast<unsigned>(io[r] | (io[r + 1] << 8)) : bus->io().read16(r);
    };
    const unsigned disp = reg(0x000);
    if ((disp & 7u) != 0u) return 0;
    int i = 0;
    for (const Scene* s : {&kOpening, &kTitle}) {
        ++i;
        bool match = true;
        for (int b = 0; b < 4 && match; ++b)
            if (s->cnt[b] && (reg(0x008 + 2 * b) != s->cnt[b] || !(disp & (0x100u << b)))) match = false;
        if (match) return i;
    }
    return 0;
}

struct Layer {
    std::uint32_t chars, screen;
    unsigned w, h, hofs, vofs, prio;
};
Layer layer_of(unsigned cnt, unsigned hofs, unsigned vofs) {
    const unsigned size = cnt >> 14;
    return {((cnt >> 2) & 3u) * 0x4000u, ((cnt >> 8) & 0x1Fu) * 0x800u,
            (size & 1u) ? 512u : 256u, (size & 2u) ? 512u : 256u, hofs & 0x1FFu, vofs & 0x1FFu,
            cnt & 3u};
}
// A text-mode 16-colour layer's pixel at map (mx, my): colour index, 0 clear.
unsigned sample_map(const std::uint8_t* vram, const Layer& l, unsigned mx, unsigned my,
                    unsigned* bank) {
    mx &= l.w - 1u;
    my &= l.h - 1u;
    const unsigned block = (my >> 8) * (l.w >> 8) + (mx >> 8);
    const std::uint32_t at = l.screen + block * 0x800u + (((my & 255u) >> 3) * 32u + ((mx & 255u) >> 3)) * 2u;
    if (at + 2u > 0x10000u) return 0;
    const std::uint16_t e = rd16(vram, at);
    const unsigned sx = (e & 0x400u) ? 7u - (mx & 7u) : (mx & 7u);
    const unsigned sy = (e & 0x800u) ? 7u - (my & 7u) : (my & 7u);
    const std::uint32_t p = l.chars + (e & 0x3FFu) * 32u + sy * 4u + sx / 2u;
    if (p >= 0x10000u) return 0;
    *bank = e >> 12;
    return (vram[p] >> (4u * (sx & 1u))) & 15u;
}

// The title's BG1 is an opaque near-black field (one solid tile, the map's
// most common entry) with the emblem and text cut into it as transparent
// holes, so no row of it is field alone and some columns never are. One
// pixel of the field tile (x, y on screen) is the field: -1 when there is
// none.
void find_veil(gba::GbaBus* bus, int* vx, int* vy) {
    *vx = *vy = -1;
    const std::uint8_t* vram = bus->vram_ptr();
    if (!vram) return;
    const unsigned cnt = bus->io().read16(0x00A);
    const Layer l = layer_of(cnt, bus->io().read16(0x014), bus->io().read16(0x016));
    auto entry = [&](int c, int r) {
        const unsigned mx = (static_cast<unsigned>(c * 8) + l.hofs) & (l.w - 1u);
        const unsigned my = (static_cast<unsigned>(r * 8) + l.vofs) & (l.h - 1u);
        const unsigned block = (my >> 8) * (l.w >> 8) + (mx >> 8);
        return rd16(vram, l.screen + block * 0x800u + (((my & 255u) >> 3) * 32u + ((mx & 255u) >> 3)) * 2u);
    };
    // The field: the most common entry, provided it is a solid tile.
    std::uint16_t all[20 * 30];
    for (int r = 0; r < 20; ++r)
        for (int c = 0; c < 30; ++c) all[r * 30 + c] = entry(c, r);
    std::sort(all, all + 20 * 30);
    std::uint16_t field = all[0];
    int best = 0;
    for (int k = 0, run = 0; k < 20 * 30; ++k) {
        run = (k > 0 && all[k] == all[k - 1]) ? run + 1 : 1;
        if (run > best) { best = run; field = all[k]; }
    }
    const std::uint8_t* tile = vram + l.chars + (field & 0x3FFu) * 32u;
    for (int k = 1; k < 32; ++k)
        if (tile[k] != tile[0] || (tile[0] & 0xF) == 0) return;   // not solid: no veil
    for (int r = 19; r >= 0; --r)
        for (int c = 0; c < 30; ++c)
            if (entry(c, r) == field) { *vx = c * 8; *vy = r * 8; return; }
}

// ── The top screen's rows past 160: the providers' per-frame answers ────
constexpr int kMaxRows = 64;   // gbarecomp's kMaxExtraY is 60
std::uint64_t g_wobble_frame = ~0ull;
unsigned g_wobble_layers = 0;
int g_wobble_dx[4][kMaxRows] = {};

}  // namespace

void scene_strip_install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("KOL_NO_SCENE_STRIP");
    if (off && off[0] && off[0] != '0') return;
    if (!gba_mod_register_function_entry_plugin(kHookId, kFillTable, 1, &on_fill_table) ||
        !gba_mod_set_function_hook_enabled(kHookId, 1))
        std::fprintf(stderr, "scene_strip: raster-params hook NOT installed\n");
}

void scene_strip_frame() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const int scene = scene_of(nullptr, bus);
    if (scene == 1 && g_scene.load(std::memory_order_relaxed) != 1) {
        gba::GbaPpu* ppu = gbarecomp::active_ppu();
        g_opening_start.store(ppu ? static_cast<long long>(ppu->frame_count()) : -1, std::memory_order_relaxed);
        if (const char* e = std::getenv("KOL_STRIP_LOG"); e && e[0] && e[0] != '0')
            std::printf("strip: opening starts at frame=%lld\n", g_opening_start.load());
    }
    g_scene.store(scene, std::memory_order_relaxed);
    int vx = -1, vy = -1;
    if (scene == 2) find_veil(bus, &vx, &vy);
    g_veil_x.store(vx, std::memory_order_relaxed);
    g_veil_y.store(vy, std::memory_order_relaxed);
}

int opening_scene() { return g_scene.load(std::memory_order_relaxed); }

bool title_veil(int bg, int screen_y, int* hw_x, int* src_y) {
    if (bg != kTitleVeilLayer || (screen_y >= 0 && screen_y < 160)) return false;
    if (g_scene.load(std::memory_order_relaxed) != 2) return false;
    const int x = g_veil_x.load(std::memory_order_relaxed), y = g_veil_y.load(std::memory_order_relaxed);
    if (x < 0 || y < 0) return false;
    *hw_x = x;
    *src_y = y;
    return true;
}

bool opening_wobble(int bg, int screen_y, int* dx) {
    if (screen_y < 160 || screen_y >= 160 + kMaxRows || bg < 0 || bg > 3) return false;
    if (g_scene.load(std::memory_order_relaxed) != 1) return false;
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!ppu || !bus) return false;
    // Once a frame (the rows are drawn after line 159, on the guest thread):
    // each wobbling layer's shift from its line-159 HOFS, which the margin
    // rows are drawn with.
    const std::uint64_t frame = ppu->frame_count();
    if (frame != g_wobble_frame) {
        g_wobble_frame = frame;
        g_wobble_layers = 0;
        const std::uint8_t* io = bus->io().raw();
        for (int b = 0; b < 4; ++b) {
            Wave w;
            int h159 = 0;
            if (!wave_for(io, b, &w, frame) &&
                !fire_preroll(b, frame, static_cast<int>(bus->io().read16(0x010 + 4 * b) & 0x1FFu), &w))
                continue;
            if (!wave_hofs(w, 159, &h159)) continue;
            g_wobble_layers |= 1u << b;
            for (int r = 0; r < kMaxRows; ++r) {
                int h = h159;
                wave_hofs(w, 160 + r, &h);
                g_wobble_dx[b][r] = h - h159;
            }
        }
    }
    if (!(g_wobble_layers & (1u << bg))) return false;
    *dx = g_wobble_dx[bg][screen_y - 160];
    return true;
}

bool compose_scene_strip(std::uint32_t* out, int* scene_out) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !out) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    if (!vram || !pal) return false;
    // The registers as the presented frame's last scanline had them, which is
    // what the top screen's own rows below the game's 160 are drawn with
    // (gbarecomp's margin rows), so the two screens meet exactly; the live
    // ones before any frame has latched.
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint8_t* latched = ppu ? ppu->latched_io() : nullptr;
    auto io = [&](std::uint32_t reg) -> unsigned {
        return latched ? static_cast<unsigned>(latched[reg] | (latched[reg + 1] << 8))
                       : bus->io().read16(reg);
    };
    const int scene_id = scene_of(latched, bus);
    if (!scene_id) return false;
    if (scene_out) *scene_out = scene_id;
    const Scene& scene = scene_id == 1 ? kOpening : kTitle;

    Layer layers[4];
    Wave waves[4];
    bool wavy[4] = {};
    int order[4], n = 0;
    for (int b = 0; b < 4; ++b) {
        if (!(scene.layers & (1u << b))) continue;
        layers[b] = layer_of(io(0x008 + 2 * b), io(0x010 + 4 * b), io(0x012 + 4 * b));
        wavy[b] = latched && (wave_for(latched, b, &waves[b], ppu->latched_frame()) ||
                              fire_preroll(b, ppu->latched_frame(), static_cast<int>(layers[b].hofs), &waves[b]));
        order[n++] = b;
    }
    std::stable_sort(order, order + n, [&](int a, int b) { return layers[a].prio < layers[b].prio; });
    // KOL_STRIP_LOG=1: each composed frame's waves (which layers wobble, and
    // from which fill), to find frames drawn without one.
    static const bool log_on = [] { const char* e = std::getenv("KOL_STRIP_LOG"); return e && e[0] && e[0] != '0'; }();
    if (log_on && ppu) {
        std::printf("strip: latched=%llu now=%llu scene=%d", static_cast<unsigned long long>(ppu->latched_frame()),
                    static_cast<unsigned long long>(ppu->frame_count()), scene_id);
        for (int b = 0; b < 4; ++b) {
            if (!(scene.layers & (1u << b))) continue;
            if (wavy[b])
                std::printf("  bg%d wave tbl=%08X filled=%llu base=%d [%d,%d) ph=%u st=%u sh=%u sc=%u", b,
                            waves[b].table, static_cast<unsigned long long>(waves[b].filled), waves[b].base,
                            waves[b].start, waves[b].end, waves[b].phase, waves[b].step, waves[b].shift,
                            waves[b].scale);
            else
                std::printf("  bg%d NO-WAVE hofs=%u", b, layers[b].hofs);
        }
        if (latched) {
            for (int ch = 0; ch < 4; ++ch) {
                const std::uint32_t d = 0xB0u + 12u * static_cast<unsigned>(ch);
                const unsigned ctl = latched[d + 10] | (latched[d + 11] << 8);
                if (ctl & 0x8000u)
                    std::printf("  dma%d ctl=%04X sad=%08X dad=%08X", ch, ctl,
                                static_cast<unsigned>(latched[d] | (latched[d + 1] << 8) | (latched[d + 2] << 16) |
                                                      (static_cast<std::uint32_t>(latched[d + 3]) << 24)),
                                static_cast<unsigned>(latched[d + 4] | (latched[d + 5] << 8) | (latched[d + 6] << 16) |
                                                      (static_cast<std::uint32_t>(latched[d + 7]) << 24)));
            }
        }
        std::printf("\n");
    }
    const int veil_x = scene_id == 2 ? g_veil_x.load(std::memory_order_relaxed) : -1;
    const int veil_y = scene_id == 2 ? g_veil_y.load(std::memory_order_relaxed) : -1;

    const unsigned bld = io(0x050), effect = (bld >> 6) & 3u;
    const unsigned alpha = io(0x052);
    const unsigned eva = std::min(16u, alpha & 0x1Fu), evb = std::min(16u, (alpha >> 8) & 0x1Fu);
    const unsigned evy = std::min(16u, static_cast<unsigned>(io(0x054) & 0x1Fu));
    const unsigned backdrop_c = rd16(pal, 0);

    int y0 = 160;
    if (ppu) y0 += static_cast<int>(ppu->view_extra_bottom());
    const int x0 = (240 - kStripW) / 2;

    // Colour effects in the GBA's own domain, as gbarecomp's PPU applies them:
    // green carries a sixth bit (palette bit 15) until the end, blends round
    // to nearest, darkening rounds its term with +7.
    struct Rgb { unsigned r, g6, b; };
    auto rgb = [](unsigned c) { return Rgb{c & 31u, ((c >> 4) & 62u) | (c >> 15), (c >> 10) & 31u}; };
    auto argb = [](Rgb c) {
        const unsigned r = c.r, g = c.g6 >> 1, b = c.b;
        return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) |
               ((b << 3) | (b >> 2));
    };
    const Rgb backdrop = rgb(backdrop_c);

    for (int y = 0; y < kStripH; ++y) {
        const int sy = y0 + y;
        // Each layer's row: the wave's HOFS for a wobbling layer, the veil's
        // own row for the title's BG1.
        unsigned hofs[4], my[4];
        for (int k = 0; k < n; ++k) {
            const int b = order[k];
            int h = static_cast<int>(layers[b].hofs);
            if (wavy[b]) wave_hofs(waves[b], sy, &h);
            hofs[b] = static_cast<unsigned>(h);
            my[b] = static_cast<unsigned>(sy) + layers[b].vofs;
        }
        for (int x = 0; x < kStripW; ++x) {
            const int sx = x0 + x;
            if (sx < 0 || sx >= 240) { out[y * kStripW + x] = 0xFF000000u; continue; }
            // The top two opaque pixels, by priority (then layer number).
            Rgb top = backdrop, under = backdrop;
            unsigned top_bit = 0x20u, under_bit = 0x20u;   // 0x20: the backdrop
            int found = 0;
            for (int k = 0; k < n && found < 2; ++k) {
                const int b = order[k];
                unsigned col = static_cast<unsigned>(sx) + hofs[b], row = my[b];
                if (b == kTitleVeilLayer && scene_id == 2) {   // the veil, as the providers
                    if (veil_x < 0) continue;
                    col = static_cast<unsigned>(veil_x) + hofs[b];
                    row = static_cast<unsigned>(veil_y) + layers[b].vofs;
                }
                unsigned bank = 0;
                const unsigned i = sample_map(vram, layers[b], col, row, &bank);
                if (!i) continue;
                const Rgb c = rgb(rd16(pal, (bank * 16u + i) * 2u));
                if (found == 0) { top = c; top_bit = 1u << b; }
                else { under = c; under_bit = 1u << b; }
                ++found;
            }
            Rgb c = top;
            if (effect == 1u && (bld & top_bit) && ((bld >> 8) & under_bit)) {
                c.r = std::min(31u, (top.r * eva + under.r * evb + 8u) >> 4);
                c.g6 = std::min(63u, (top.g6 * eva + under.g6 * evb + 8u) >> 4);
                c.b = std::min(31u, (top.b * eva + under.b * evb + 8u) >> 4);
            } else if (effect == 2u && (bld & top_bit) && evy) {
                c.r += ((31u - c.r) * evy + 8u) >> 4;
                c.g6 += ((63u - c.g6) * evy + 8u) >> 4;
                c.b += ((31u - c.b) * evy + 8u) >> 4;
            } else if (effect == 3u && (bld & top_bit) && evy) {
                c.r -= (c.r * evy + 7u) >> 4;
                c.g6 -= (c.g6 * evy + 7u) >> 4;
                c.b -= (c.b * evy + 7u) >> 4;
            }
            out[y * kStripW + x] = argb(c);
        }
    }
    return true;
}

void scene_strip_dump_tick(unsigned frame) {
    static const char* path = std::getenv("KOL_STRIP_DUMP");
    if (!path) return;
    static std::vector<std::uint32_t> px(kStripW * kStripH), last;
    int scene = 0;
    const bool on = compose_scene_strip(px.data(), &scene);
    static int was = -1;
    const int now = on ? scene : 0;
    if (now != was) {
        std::printf("scene_strip: frame=%u %s\n", frame,
                    now == 1 ? "opening" : now == 2 ? "title" : "off");
        was = now;
    }
    if (!on || px == last) return;
    last = px;
    char name[512];
    std::snprintf(name, sizeof name, path, frame);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", kStripW, kStripH);
        for (const std::uint32_t c : px) {
            const unsigned char rgb3[3] = {static_cast<unsigned char>(c >> 16),
                                           static_cast<unsigned char>(c >> 8),
                                           static_cast<unsigned char>(c)};
            std::fwrite(rgb3, 1, 3, f);
        }
        std::fclose(f);
    }
}

}  // namespace kol
