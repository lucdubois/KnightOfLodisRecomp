// kol_terrain_probe.cpp — breakpoint hunt for the tile -> terrain-record link.
//
// The battle map's terrain panel is driven by a ROM table at 0x0861CA5C
// (stride 0x20, 89 records, six s8 elemental modifiers at +0x16). What is NOT
// known is how the game gets from the cursor tile to a record; see README.md
// "Terrain properties table" for the four searches that failed to find it.
//
// This is the route that was left: the table base appears as a literal in 68
// places, inside the 66 functions listed below. g_runtime_fn_entry_hook fires
// on every recompiled function entry with g_cpu.R[0..3] holding the AAPCS
// arguments, so this watches those functions and reports any argument that
// resolves to a plausible terrain record.
//
// Diagnostic only, entirely env-gated, and never installed unless
// KOL_TERRAIN_PROBE=1 is set.

#include "kol_terrain_probe.h"
#include "kol_widescreen.h"   // is_battle_scene, is_status_scene, unit_panel_rect
#include "kol_map_source.h"   // map_source_entry (the composed map behind the margins)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>

#include "runtime_arm.h"          // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "runtime.h"              // runtime_ewram_view
#include "gba_bus.h"
#include "gba_io.h"

namespace kol {
namespace {

constexpr std::uint32_t kTableBase = 0x0861CA5Cu;
constexpr std::uint32_t kStride    = 0x20u;
constexpr std::uint32_t kModsOff   = 0x16u;
constexpr int           kRecords   = 89;

// Functions whose bodies reference the table base.
const std::uint32_t kWatch[] = {
    0x0804BFC8u,
    0x0804C0D6u,
    0x0804C432u,
    0x0804C4FAu,
    0x0804C59Au,
    0x0804CC6Cu,
    0x0804D58Cu,
    0x0804DD3Au,
    0x08058620u,
    0x08058C4Cu,
    0x08059AC2u,
    0x08059BA2u,
    0x08059D06u,
    0x0805AFACu,
    0x0805B436u,
    0x0805BFC2u,
    0x0805F8B2u,
    0x0805F9D6u,
    0x08064248u,
    0x0806598Eu,
    0x08065A74u,
    0x0806C9EAu,
    0x0806D3E6u,
    0x0806DB42u,
    0x0806F868u,
    0x0807226Au,
    0x0807595Au,
    0x0807BA0Eu,
    0x08084144u,
    0x0808431Cu,
    0x08084426u,
    0x080846EAu,
    0x08084D7Au,
    0x08084DE2u,
    0x080858E2u,
    0x08085C50u,
    0x08085E9Eu,
    0x08089164u,
    0x0808926Cu,
    0x0808A5D8u,
    0x0808A872u,
    0x0808A932u,
    0x0808CB12u,
    0x0808CB9Au,
    0x0808CBF0u,
    0x0808D55Eu,
    0x0808D756u,
    0x0808E6C8u,
    0x0808E712u,
    0x0808F2F4u,
    0x0809763Eu,
    0x08099CC6u,
    0x08099DF2u,
    0x0809D278u,
    0x0809F3FCu,
    0x080A5BE0u,
    0x080A71EAu,
    0x080A7634u,
    0x080AEA4Cu,
    0x080B09A6u,
    0x080B2AB8u,
    0x0811E228u,
    0x0813BA60u,
    0x0813CA70u,
    0x0813CBE0u,
    0x0813D4BCu,
    0x0813DBEEu,
    0x0813DC92u
};
constexpr std::size_t kWatchCount = sizeof(kWatch) / sizeof(kWatch[0]);

// This hook runs on EVERY recompiled function entry, so the reject path has to
// be near-free. A direct-mapped table keyed on (pc >> 1) & 0x7FF settles it in
// one load and one compare; the two PCs that collide fall through to a search
// that only those slots ever reach.
constexpr std::uint32_t kAmbiguous = 0xFFFFFFFFu;
std::uint32_t g_slot[2048];

bool watched_slow(std::uint32_t pc) {
    std::size_t lo = 0, hi = kWatchCount;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (kWatch[mid] == pc) return true;
        if (kWatch[mid] < pc) lo = mid + 1; else hi = mid;
    }
    return false;
}

inline bool watched(std::uint32_t pc) {
    const std::uint32_t v = g_slot[(pc >> 1) & 0x7FFu];
    if (v == pc) return true;
    if (v != kAmbiguous) return false;
    return watched_slow(pc);
}

void build_slots() {
    for (std::uint32_t& v : g_slot) v = 0;
    for (std::size_t i = 0; i < kWatchCount; ++i) {
        std::uint32_t& v = g_slot[(kWatch[i] >> 1) & 0x7FFu];
        v = (v == 0) ? kWatch[i] : kAmbiguous;
    }
}

// Latest terrain id the guest looked up. -1 until the game first rebuilds the
// terrain panel. Present-time shadow: written only from the observation hook,
// never read back into emulation.
volatile int g_last_id = -1;

// Expected modifiers for the tile the probe run is parked on, as
// "orange,red,green,blue,white,grey" via KOL_TERRAIN_MODS. Any argument that
// resolves to a record with these modifiers is the link we are looking for.
int g_want[6];
bool g_have_want = false;
int g_logged = 0;
bool g_verbose = false;

const std::uint8_t* g_rom = nullptr;
std::uint32_t g_rom_size = 0;

int record_index_of(std::uint32_t maybe) {
    // Accept an index, a record address, or a pointer to the modifier field.
    if (maybe < kRecords) return static_cast<int>(maybe);
    for (std::uint32_t off : {0u, kModsOff}) {
        if (maybe < kTableBase + off) continue;
        const std::uint32_t rel = maybe - kTableBase - off;
        if (rel % kStride) continue;
        const std::uint32_t i = rel / kStride;
        if (i < kRecords) return static_cast<int>(i);
    }
    return -1;
}

bool mods_match(int idx) {
    if (!g_have_want || !g_rom) return false;
    const std::uint32_t off = (kTableBase - 0x08000000u) + idx * kStride + kModsOff;
    if (off + 6 > g_rom_size) return false;
    for (int k = 0; k < 6; ++k) {
        const int v = static_cast<signed char>(g_rom[off + k]);
        if (v != g_want[k]) return false;
    }
    return true;
}

// The terrain panel is composited once and not re-read while the game idles,
// so a parked save state never exercises the lookup. KOL_TERRAIN_INPUT=1
// walks the cursor by overriding KEYINPUT reads: press a direction for a few
// frames, release, alternate. Nothing else is touched, and the override is
// only installed alongside the probe.
constexpr std::uint32_t kKeyInput = 0x04000130u;
std::uint16_t g_fake_keys = 0x03FFu;   // active low: 1 = released
bool g_drive_input = false;
bool g_drive_select = false;

extern "C" int kol_terrain_read_override(std::uint32_t /*pc*/,
                                         std::uint32_t address,
                                         std::uint32_t width,
                                         std::uint32_t /*original*/,
                                         std::uint32_t* out_value) {
    if ((!g_drive_input && !g_drive_select) || address != kKeyInput ||
        width < 2) return 0;
    *out_value = g_fake_keys;
    return 1;
}

extern "C" void kol_terrain_fn_entry(std::uint32_t pc) {
    if (!watched(pc)) return;
    // r0 on entry is the terrain id; see the idiom in this file's header.
    const std::uint32_t entry_id = g_cpu.R[0] & 0xFFFFu;
    if (entry_id < kRecords && is_battle_scene())
        g_last_id = static_cast<int>(entry_id);
    if (!g_verbose) return;
    if (g_logged > 600) return;
    const std::uint32_t r[4] = {g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3]};
    int hit = -1, hit_reg = -1;
    for (int i = 0; i < 4; ++i) {
        const int idx = record_index_of(r[i]);
        if (idx >= 0 && mods_match(idx)) { hit = idx; hit_reg = i; break; }
    }
    // Every site shares one idiom:
    //   movs r0,r0,lsl #16 / movs r0,r0,lsr #11   -> (r0 & 0xFFFF) * 0x20
    //   ldr r1,[pc,#..] (= 0x0861CA5C) / adds r0,r0,r1 / ldrb r0,[r0]
    // so r0 on entry is the TERRAIN ID and the record is base + id*0x20.
    const std::uint32_t id = r[0] & 0xFFFFu;
    if (id < kRecords && g_rom) {
        const std::uint32_t mo = (kTableBase - 0x08000000u) + id * kStride + kModsOff;
        if (mo + 6 <= g_rom_size) {
            std::fprintf(stderr,
                "[kol-terrain] id=%u pc=0x%08X mods=%d,%d,%d,%d,%d,%d "
                "r4=%08X r5=%08X r6=%08X r7=%08X lr=%08X\n", id, pc,
                (int)(signed char)g_rom[mo+0], (int)(signed char)g_rom[mo+1],
                (int)(signed char)g_rom[mo+2], (int)(signed char)g_rom[mo+3],
                (int)(signed char)g_rom[mo+4], (int)(signed char)g_rom[mo+5],
                g_cpu.R[4], g_cpu.R[5], g_cpu.R[6], g_cpu.R[7], g_cpu.R[14]);
            std::fflush(stderr);
        }
    }
    if (hit >= 0) {
        std::fprintf(stderr,
            "[kol-terrain] MATCH pc=0x%08X r%d=0x%08X -> record %d  "
            "(r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X)\n",
            pc, hit_reg, r[hit_reg], hit, r[0], r[1], r[2], r[3], g_cpu.R[14]);
        ++g_logged;
        std::fflush(stderr);
        return;
    }
    // Otherwise report the call. Deduped on (pc, args) so a per-frame poller
    // does not drown the interesting one-shot lookups.
    struct Seen { std::uint32_t pc, a0, a1, a2, a3; };
    static Seen seen[512];
    static int seen_n = 0;
    for (int i = 0; i < seen_n; ++i)
        if (seen[i].pc == pc && seen[i].a0 == r[0] && seen[i].a1 == r[1] &&
            seen[i].a2 == r[2] && seen[i].a3 == r[3]) return;
    if (seen_n < 512) seen[seen_n++] = {pc, r[0], r[1], r[2], r[3]};
    std::fprintf(stderr,
        "[kol-terrain] call pc=0x%08X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X\n",
        pc, r[0], r[1], r[2], r[3], g_cpu.R[14]);
    ++g_logged;
    std::fflush(stderr);
}

}  // namespace

// Diagnostic: KOL_TERRAIN_BOX_DUMP=<path.bmp> writes the rasterised box once,
// so it can be diffed against the same region of a rendered frame.
void dump_bmp(const char* path, const std::uint32_t* buf, int w, int h);

void maybe_dump_frame() {
    static bool done = false;
    if (done) return;
    const char* path = std::getenv("KOL_PANEL_FRAME_DUMP");
    if (!path || !path[0]) return;
    std::uint32_t buf[10 * 8 * 7 * 8];
    int w = 0, h = 0;
    if (!capture_unit_panel_frame(buf, &w, &h)) return;
    done = true;
    dump_bmp(path, buf, w, h);
    std::fprintf(stderr, "panel_frame: dumped %dx%d to %s\n", w, h, path);
    std::fflush(stderr);
}

namespace {
void render_tile(std::uint32_t* dst, int dstW, int dx, int dy,
                 std::uint32_t tile, std::uint32_t bank,
                 const std::uint8_t* vram, const std::uint8_t* pal,
                 std::uint32_t char_base);
}

// KOL_TILE_STRIP="base,count,bank,bg" renders a run of tiles to a BMP so a
// suspected glyph range can be eyeballed.
void maybe_dump_strip() {
    static bool done = false;
    if (done) return;
    const char* spec = std::getenv("KOL_TILE_STRIP");
    if (!spec || !spec[0]) return;
    int base = 0, count = 0, bank = 0, bg = 3;
    if (std::sscanf(spec, "%x,%d,%d,%d", &base, &count, &bank, &bg) < 3) return;
    if (!is_battle_scene()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return;
    done = true;
    const std::size_t cnt_off = 0x08 + 2u * static_cast<std::size_t>(bg);
    const std::uint16_t bgcnt =
        static_cast<std::uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
    const std::uint32_t cb = ((bgcnt >> 2) & 3u) * 0x4000u;
    static std::uint32_t buf[64 * 8 * 8];
    const int W = count * 8;
    for (int i = 0; i < W * 8; ++i) buf[i] = 0xFF101010u;
    for (int i = 0; i < count; ++i)
        render_tile(buf, W, i * 8, 0, base + i, bank, vram, pal, cb);
    dump_bmp("/tmp/tilestrip.bmp", buf, W, 8);
    std::fprintf(stderr, "tile_strip: %03X..%03X bank %d bg %d -> /tmp/tilestrip.bmp\n",
                 base, base + count - 1, bank, bg);
    std::fflush(stderr);
}

// KOL_OBJ_PROBE=<y0,y1> lists every enabled OBJ whose rows touch that band,
// with the raw (unblended) colours of its palette bank. Comparing those to the
// rendered frame says whether a colour effect was applied to the sprite.
void maybe_probe_obj() {
    static bool done = false;
    if (done) return;
    const char* spec = std::getenv("KOL_OBJ_PROBE");
    if (!spec || !spec[0]) return;
    int y0 = 0, y1 = 159;
    std::sscanf(spec, "%d,%d", &y0, &y1);
    if (!is_battle_scene()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* oam = bus->oam_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    if (!oam || !pal) return;
    done = true;
    static const int kSizeW[4][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32},{0,0,0,0}};
    static const int kSizeH[4][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64},{0,0,0,0}};
    for (int i = 0; i < 128; ++i) {
        const std::uint8_t* a = oam + i * 8;
        const std::uint16_t a0 = static_cast<std::uint16_t>(a[0] | (a[1] << 8));
        const std::uint16_t a1 = static_cast<std::uint16_t>(a[2] | (a[3] << 8));
        const std::uint16_t a2 = static_cast<std::uint16_t>(a[4] | (a[5] << 8));
        const std::uint32_t shape = (a0 >> 14) & 3u;
        const std::uint32_t sz    = (a1 >> 14) & 3u;
        const bool affine = (a0 & 0x0100u) != 0;
        if (!affine && (a0 & 0x0200u)) continue;  // disabled
        const int h = kSizeH[shape][sz], w = kSizeW[shape][sz];
        if (!h) continue;
        int oy = a0 & 0xFFu; if (oy >= 160) oy -= 256;
        int ox = a1 & 0x1FFu; if (ox >= 240) ox -= 512;
        if (oy + h <= y0 || oy > y1) continue;
        const std::uint32_t mode = (a0 >> 10) & 3u;
        const std::uint32_t bank = (a2 >> 12) & 0xFu;
        std::fprintf(stderr,
            "[obj] %3d x=%4d y=%4d %2dx%-2d tile=%03X pri=%u mode=%u bank=%2u pal=",
            i, ox, oy, w, h, a2 & 0x3FFu, (a2 >> 10) & 3u, mode, bank);
        for (int c = 1; c < 16; ++c) {
            const std::size_t o = 0x200u + bank * 32u + c * 2u;
            const std::uint16_t v =
                static_cast<std::uint16_t>(pal[o] | (pal[o + 1] << 8));
            std::fprintf(stderr, "%02X%02X%02X ",
                         ((v & 31) * 255 + 15) / 31,
                         (((v >> 5) & 31) * 255 + 15) / 31,
                         (((v >> 10) & 31) * 255 + 15) / 31);
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

void maybe_dump_labels() {
    static bool done = false;
    if (done) return;
    const char* path = std::getenv("KOL_STAT_LABELS_DUMP");
    if (!path || !path[0]) return;
    std::uint32_t buf[72 * 8];
    int w = 0, h = 0;
    if (!capture_status_labels(buf, &w, &h)) return;
    done = true;
    dump_bmp(path, buf, w, h);
    std::fprintf(stderr, "stat_labels: dumped %dx%d\n", w, h);
    std::fflush(stderr);
}

void maybe_dump_atlas() {
    static bool done = false;
    if (done) return;
    const char* path = std::getenv("KOL_FONT_ATLAS_DUMP");
    if (!path || !path[0]) return;
    std::uint32_t buf[80 * 16];
    int w = 0, h = 0;
    if (!capture_font_atlas(buf, &w, &h)) return;
    done = true;
    dump_bmp(path, buf, w, h);
    std::fprintf(stderr, "font_atlas: dumped %dx%d to %s\n", w, h, path);
    std::uint32_t nm[64 * 16]; int nw = 0, nh = 0;
    if (capture_unit_name(nm, &nw, &nh)) {
        std::string np(path); np.insert(np.size() - 4, "_name");
        dump_bmp(np.c_str(), nm, nw, nh);
        std::fprintf(stderr, "unit_name: dumped %dx%d\n", nw, nh);
    }
    std::fflush(stderr);
}

void maybe_dump_box() {
    static bool done = false;
    if (done) return;
    const char* path = std::getenv("KOL_TERRAIN_BOX_DUMP");
    if (!path || !path[0]) return;
    std::uint32_t buf[56 * 72];
    int w = 0, h = 0;
    if (!capture_terrain_box(buf, &w, &h)) return;
    done = true;
    dump_bmp(path, buf, w, h);
    std::fprintf(stderr, "terrain_box: dumped %dx%d to %s\n", w, h, path);
    std::fflush(stderr);
}

void dump_bmp(const char* path, const std::uint32_t* buf, int w, int h) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const int stride = (w * 3 + 3) / 4 * 4;
    const int data = stride * h;
    std::uint8_t hdr[54] = {};
    hdr[0]='B'; hdr[1]='M';
    const int fsz = 54 + data;
    std::memcpy(hdr+2,&fsz,4); const int off=54; std::memcpy(hdr+10,&off,4);
    const int hs=40; std::memcpy(hdr+14,&hs,4);
    std::memcpy(hdr+18,&w,4); std::memcpy(hdr+22,&h,4);
    const std::uint16_t planes=1, bpp=24;
    std::memcpy(hdr+26,&planes,2); std::memcpy(hdr+28,&bpp,2);
    std::memcpy(hdr+34,&data,4);
    std::fwrite(hdr,1,54,f);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const std::uint32_t p = buf[y * w + x];
            const std::uint8_t px[3] = {static_cast<std::uint8_t>(p & 0xFF),
                                        static_cast<std::uint8_t>((p >> 8) & 0xFF),
                                        static_cast<std::uint8_t>((p >> 16) & 0xFF)};
            std::fwrite(px, 1, 3, f);
        }
        for (int k = 0; k < stride - w * 3; ++k) std::fputc(0, f);
    }
    std::fclose(f);
}

namespace { void maybe_dump_bg(std::uint64_t frame); void maybe_report_coverage(std::uint64_t frame); }  // defined below, after blit_layer_rect

// KOL_MAP_WRITE_WATCH=1 reports every traced memory write whose destination
// lands in BG0's tilemap, with the PC and registers that produced it. The trace
// ring is always-on, so this queries a window rather than arming a capture.
// Point: the movement-range highlight replaces tiles in that map, and whoever
// writes them must have read the original from somewhere.
void maybe_watch_map_writes() {
    static bool on = std::getenv("KOL_MAP_WRITE_WATCH") != nullptr;
    if (!on) return;
    if (!is_battle_scene()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* io = bus->io().raw();
    if (!io) return;
    const std::uint16_t bg0 =
        static_cast<std::uint16_t>(io[0x08] | (io[0x09] << 8));
    const std::uint32_t sb = ((bg0 >> 8) & 0x1Fu) * 0x800u;
    std::uint32_t lo = 0x06000000u + sb;
    std::uint32_t hi = lo + 0x1000u;
    // KOL_MAP_WRITE_WATCH="lo,hi" (hex) overrides the window, so the same probe
    // can sweep all of VRAM or any RAM region.
    if (const char* w = std::getenv("KOL_MAP_WRITE_WATCH")) {
        unsigned a = 0, b = 0;
        if (std::sscanf(w, "%x,%x", &a, &b) == 2) { lo = a; hi = b; }
    }

    static RuntimeTraceEntry tr[4096];
    const std::uint32_t n = runtime_trace_copy_recent(tr, 4096);
    static std::uint32_t last_seq = 0;
    static int reported = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const RuntimeTraceEntry& e = tr[i];
        if (e.seq <= last_seq) continue;
        if (e.kind != RUNTIME_TRACE_MEM_WRITE) continue;
        if (e.addr < lo || e.addr >= hi) continue;
        if (reported++ >= 400) break;
        std::fprintf(stderr,
            "[mapw] pc=%08X addr=%08X val=%08X aux=%u "
            "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X lr=%08X\n",
            e.pc, e.addr, e.value, e.aux, e.r0, e.r1, e.r2, e.r3, e.r4, e.r5,
            e.r14);
    }
    if (n) last_seq = tr[n - 1].seq;
    std::fflush(stderr);
}

// KOL_MAP_SNAP="a,b" writes the BG0 tilemap to /tmp/snap_<n>.bin at n frames
// after this run started, for as many points as listed. Unlatched, so one
// process can sample the same map before and after an input.
void maybe_snap_map(std::uint64_t frame) {
    const char* spec = std::getenv("KOL_MAP_SNAP");
    if (!spec || !spec[0]) return;
    if (!is_battle_scene()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !io) return;
    static std::uint64_t first = frame;
    const std::uint64_t rel = frame - first;
    unsigned a = 0, b = 0;
    const int n = std::sscanf(spec, "%u,%u", &a, &b);
    if (!((n >= 1 && rel == a) || (n >= 2 && rel == b))) return;
    const std::uint16_t bg0 =
        static_cast<std::uint16_t>(io[0x08] | (io[0x09] << 8));
    const std::uint32_t sb = ((bg0 >> 8) & 0x1Fu) * 0x800u;
    char path[256];
    std::snprintf(path, sizeof path, "/tmp/snap_%llu.bin",
                  static_cast<unsigned long long>(rel));
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(vram + sb, 1, 0x1000, f);
        std::fclose(f);
    }
    std::fprintf(stderr, "map_snap: rel=%llu scrbase=%04X bg0cnt=%04X -> %s\n",
                 static_cast<unsigned long long>(rel), sb, bg0, path);
    std::fflush(stderr);
}

// KOL_MARGIN_TRACE=1 logs, once per battle-map frame, how far the guest's
// tilemap is actually populated to the left and right of its own 240-wide
// window. This is the temporal half of the widened-margin problem: a widened
// view needs 5 more tile columns per side (at 320) than the guest guarantees
// to have written, and the shortfall appears on whichever side the camera is
// moving TOWARD, then fills in a frame or two later — which is what "tiles
// pop in and out at the border" looks like.
//
// Reported in TILE COLUMNS beyond the native edge, counted contiguously: a
// column counts as populated when any cell in the visible row band is
// non-empty, which is the same test margin_row_is_contiguous() uses per row.
void maybe_trace_margins(std::uint64_t frame) {
    static const bool on = [] {
        const char* e = std::getenv("KOL_MARGIN_TRACE");
        return e && e[0] && e[0] != '0';
    }();
    if (!on) return;
    if (!is_battle_scene()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !io) return;

    auto entry = [&](std::uint16_t bgcnt, int tx, int ty) -> std::uint16_t {
        const std::uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        const unsigned size_code = (bgcnt >> 14) & 3u;
        const int w_tiles = (size_code & 1u) ? 64 : 32;
        const int h_tiles = (size_code & 2u) ? 64 : 32;
        tx = ((tx % w_tiles) + w_tiles) % w_tiles;
        ty = ((ty % h_tiles) + h_tiles) % h_tiles;
        const int block = (ty / 32) * (w_tiles / 32) + (tx / 32);
        const std::uint32_t off = screen_base + block * 0x800u +
                                  ((ty % 32) * 32 + (tx % 32)) * 2u;
        if (off + 1 >= 96u * 1024u) return 0;
        return static_cast<std::uint16_t>(vram[off] | (vram[off + 1] << 8));
    };

    char line[256];
    int n = std::snprintf(line, sizeof(line), "margin_trace f=%llu",
                          static_cast<unsigned long long>(frame));
    for (int bg = 0; bg < 2; ++bg) {
        const std::size_t cnt_off = 0x08 + 2u * static_cast<std::size_t>(bg);
        const std::size_t scr_off = 0x10 + 4u * static_cast<std::size_t>(bg);
        const std::uint16_t bgcnt =
            static_cast<std::uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        const int hofs = (io[scr_off] | (io[scr_off + 1] << 8)) & 0x1FF;
        const int vofs = (io[scr_off + 2] | (io[scr_off + 3] << 8)) & 0x1FF;
        auto col_populated = [&](int tx) {
            for (int y = 0; y < 160; y += 8)
                if (entry(bgcnt, tx, (vofs + y) / 8) & 0x3FFu) return true;
            return false;
        };
        const int left_edge = hofs / 8;
        const int right_edge = (hofs + 239) / 8;
        int lpop = 0, rpop = 0;
        while (lpop < 48 && col_populated(left_edge - 1 - lpop)) ++lpop;
        while (rpop < 48 && col_populated(right_edge + 1 + rpop)) ++rpop;
        // ...and the same count taken from the composed EWRAM map, which is
        // what the margins now draw from. The difference between the two is
        // exactly the streaming lag the widened view used to show.
        int lsrc = 0, rsrc = 0;
        auto src_col_populated = [&](int hw_x) {
            for (int y = 0; y < 160; y += 8) {
                std::uint16_t e = 0;
                if (kol::map_source_entry(bg, hw_x, y, &e) && (e & 0x3FFu))
                    return true;
            }
            return false;
        };
        for (int k = 0; k < 5; ++k) {
            if (src_col_populated(-8 * k - 1)) ++lsrc;
            if (src_col_populated(240 + 8 * k)) ++rsrc;
        }
        n += std::snprintf(line + n, sizeof(line) - n,
                           " bg%d:hofs=%d,L=%d,R=%d,srcL=%d,srcR=%d",
                           bg, hofs, lpop, rpop, lsrc, rsrc);
    }
    std::fprintf(stderr, "%s\n", line);
}

void terrain_probe_tick(std::uint64_t frame) {
    maybe_snap_map(frame);
    maybe_watch_map_writes();
    maybe_dump_box();
    maybe_dump_frame();
    maybe_dump_atlas();
    maybe_dump_labels();
    maybe_dump_strip();
    maybe_probe_obj();
    maybe_dump_bg(frame);
    maybe_report_coverage(frame);
    maybe_trace_margins(frame);
    // KOL_DRIVE_SELECT=1 cancels and re-selects on a 120-frame cycle (B, then A
    // 60 frames later). Clearing the movement range makes the game write the
    // ORIGINAL tiles back into the map, which is what the highlight replaced.
    if (g_drive_select) {
        // KOL_DRIVE_SELECT="30:B,90:A,150:A" presses each key for 4 frames at
        // the given frame, counted from the start of THIS run. Frames are
        // relative because `frame` is the absolute PPU count a save state
        // carries over, so absolute numbers fire immediately.
        // Latch the origin on the first BATTLE frame, not the first call. The
        // probe ticks once before the save state is applied, with frame == 0,
        // and the state then jumps `frame` to its own absolute PPU count — so
        // latching on call zero puts every scripted window tens of thousands of
        // frames in the past. maybe_snap_map and maybe_dump_bg avoid this only
        // because they return early until the battle map is up; do the same.
        if (!is_battle_scene()) { g_fake_keys = 0x03FFu; return; }
        static std::uint64_t first = frame;
        const std::uint64_t rel = frame - first;
        std::uint16_t k = 0x03FFu;
        const char* sp = std::getenv("KOL_DRIVE_SELECT");
        if (sp && std::strchr(sp, ':')) {
            for (const char* c = sp; *c;) {
                unsigned at = 0;
                int n = 0;
                if (std::sscanf(c, "%u:%n", &at, &n) == 1 && n > 0) {
                    const char key = c[n];
                    if (rel >= at && rel < at + 4u) {
                        switch (key) {
                            case 'A': k &= static_cast<std::uint16_t>(~0x0001u); break;
                            case 'B': k &= static_cast<std::uint16_t>(~0x0002u); break;
                            case 'U': k &= static_cast<std::uint16_t>(~0x0040u); break;
                            case 'D': k &= static_cast<std::uint16_t>(~0x0080u); break;
                            case 'L': k &= static_cast<std::uint16_t>(~0x0020u); break;
                            case 'R': k &= static_cast<std::uint16_t>(~0x0010u); break;
                            default: break;
                        }
                    }
                }
                const char* comma = std::strchr(c, ',');
                if (!comma) break;
                c = comma + 1;
            }
        } else {
            const std::uint64_t ph = rel % 120u;
            if (ph >= 30u && ph < 34u)      k &= static_cast<std::uint16_t>(~0x0002u);
            else if (ph >= 90u && ph < 94u) k &= static_cast<std::uint16_t>(~0x0001u);
        }
        g_fake_keys = k;
        return;
    }
    if (!g_drive_input) return;
    // 16-frame cycle: 4 held, 12 released, alternating right and left so the
    // cursor oscillates over two tiles and the panel is rebuilt repeatedly.
    const std::uint64_t phase = frame % 32u;
    std::uint16_t keys = 0x03FFu;
    if (phase < 4u)       keys = static_cast<std::uint16_t>(0x03FFu & ~0x0010u); // Right
    else if (phase >= 16u && phase < 20u)
                          keys = static_cast<std::uint16_t>(0x03FFu & ~0x0020u); // Left
    g_fake_keys = keys;
}

void install_terrain_probe(const std::uint8_t* rom, std::uint32_t rom_size) {
    if (g_rom) return;                      // already installed
    g_rom = rom; g_rom_size = rom_size;
    build_slots();
    const char* on = std::getenv("KOL_TERRAIN_PROBE");
    g_verbose = on && on[0] && on[0] != '0';
    if (const char* w = std::getenv("KOL_TERRAIN_MODS")) {
        int n = std::sscanf(w, "%d,%d,%d,%d,%d,%d",
                            &g_want[0], &g_want[1], &g_want[2],
                            &g_want[3], &g_want[4], &g_want[5]);
        g_have_want = (n == 6);
    }
    const char* drive = std::getenv("KOL_TERRAIN_INPUT");
    g_drive_input = drive && drive[0] && drive[0] != '0';
    g_drive_select = std::getenv("KOL_DRIVE_SELECT") != nullptr;
    if (g_drive_input || g_drive_select)
        g_runtime_bus_read_override = kol_terrain_read_override;
    g_runtime_fn_entry_hook = kol_terrain_fn_entry;
    std::fprintf(stderr,
        "terrain_shadow: watching %zu accessor functions%s\n",
        kWatchCount, g_verbose ? " (verbose probe on)" : "");
    std::fflush(stderr);
}

// ── Mirroring the guest's own terrain box ───────────────────────────────
// The box the game draws top-right (elevation, terrain icon, elemental
// modifiers) sits in a stable tile rect on the UI layers: BG3 carries the
// frame and BG2 the contents, cols 23..29 x rows 0..8 in both sampled states.
// Rasterising those tiles straight out of VRAM gives all three fields exactly,
// including the two — elevation and the icon — that are per-tile and so are
// not in the ROM record at all.
//
// This does NOT contradict README.md's "you cannot mirror the status screen":
// that holds because the status screen is not being drawn during combat. The
// terrain box IS on screen, so its tiles are live.
//
// Read from the TILEMAP, not the framebuffer, so HUD anchoring and panel
// pinning — which move where the box is presented — cannot affect it.
//
// Present-time shadow, like every other second-screen read: it can catch a
// mid-update frame, and that is acceptable for a display that never feeds back
// into emulation.
namespace {

constexpr int kBoxX = 184;   // tile col 23
constexpr int kBoxY = 0;
constexpr int kBoxW = 56;    // cols 23..29
constexpr int kBoxH = 72;    // rows 0..8

inline std::uint32_t bgr555_to_argb(std::uint16_t c) {
    const std::uint32_t r = (c & 0x1Fu) << 3;
    const std::uint32_t g = ((c >> 5) & 0x1Fu) << 3;
    const std::uint32_t b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b |
           (r >> 5) << 16 | (g >> 5) << 8 | (b >> 5);
}

// Composites one regular BG layer over `argb`, leaving palette index 0 alone.
void blit_layer_rect(std::uint32_t* argb, const std::uint8_t* vram,
                     const std::uint8_t* pal, const std::uint8_t* io, int bg,
                     int ox, int oy, int ow, int oh);

void blit_layer(std::uint32_t* argb, const std::uint8_t* vram,
                const std::uint8_t* pal, const std::uint8_t* io, int bg) {
    blit_layer_rect(argb, vram, pal, io, bg, kBoxX, kBoxY, kBoxW, kBoxH);
}

void blit_layer_rect(std::uint32_t* argb, const std::uint8_t* vram,
                     const std::uint8_t* pal, const std::uint8_t* io, int bg,
                     int ox, int oy, int ow, int oh) {
    const std::size_t cnt = 0x08u + 2u * static_cast<std::size_t>(bg);
    const std::size_t scr = 0x10u + 4u * static_cast<std::size_t>(bg);
    const std::uint16_t bgcnt =
        static_cast<std::uint16_t>(io[cnt] | (io[cnt + 1] << 8));
    const std::uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
    const std::uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
    const bool color256 = (bgcnt & 0x0080u) != 0;
    const unsigned size_code = (bgcnt >> 14) & 3u;
    const std::uint32_t w_px = ((size_code & 1u) ? 64u : 32u) * 8u;
    const std::uint32_t h_px = ((size_code & 2u) ? 64u : 32u) * 8u;
    const std::uint32_t block_cols = ((size_code & 1u) ? 64u : 32u) / 32u;
    const std::uint32_t hofs = (io[scr] | (io[scr + 1] << 8)) & 0x1FFu;
    const std::uint32_t vofs = (io[scr + 2] | (io[scr + 3] << 8)) & 0x1FFu;

    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const std::uint32_t tex_x = (ox + x + hofs) & (w_px - 1u);
            const std::uint32_t tex_y = (oy + y + vofs) & (h_px - 1u);
            const std::uint32_t tile_x = tex_x >> 3, tile_y = tex_y >> 3;
            const std::uint32_t block =
                (tile_x >> 5) + (tile_y >> 5) * block_cols;
            const std::uint32_t mo = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            if (mo + 1u >= 96u * 1024u) continue;
            const std::uint16_t entry =
                static_cast<std::uint16_t>(vram[mo] | (vram[mo + 1] << 8));
            const std::uint32_t tile = entry & 0x03FFu;
            std::uint32_t px = tex_x & 7u, py = tex_y & 7u;
            if (entry & 0x0400u) px = 7u - px;
            if (entry & 0x0800u) py = 7u - py;
            std::uint32_t idx;
            if (color256) {
                const std::uint32_t a = char_base + tile * 64u + py * 8u + px;
                if (a >= 96u * 1024u) continue;
                idx = vram[a];
            } else {
                const std::uint32_t a =
                    char_base + tile * 32u + py * 4u + (px >> 1);
                if (a >= 96u * 1024u) continue;
                const std::uint8_t packed = vram[a];
                idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                if (idx) idx |= ((entry >> 12) & 0x0Fu) << 4;
            }
            if ((idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;  // transparent
            const std::uint16_t c =
                static_cast<std::uint16_t>(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
            argb[y * ow + x] = bgr555_to_argb(c);
        }
    }
}

// KOL_COVERAGE=<w> reports, for a w-pixel-wide view centred on the authentic
// 240, how many pixels in each margin NO regular BG layer draws at all. Those
// are the pixels that fall through to the backdrop, which is what "the edge is
// mangled" looks like: the guest only populates its tilemap for the columns it
// intends to display, so a widened view can run past the data.
void maybe_report_coverage(std::uint64_t frame) {
    static bool done = false;
    if (done) return;
    const char* spec = std::getenv("KOL_COVERAGE");
    if (!spec || !spec[0]) return;
    if (!is_battle_scene()) return;
    // KOL_COVERAGE_AT=<n> holds the report until n frames after the battle
    // scene first comes up. The default (0) samples the FIRST battle frame
    // after a state load, and for several reference states that is mid-redraw:
    // state E reports 55.5% of the AUTHENTIC 240 bare, which is a transient,
    // not a margin fact. Counted RELATIVE to the first battle frame, per the
    // absolute-frame-count trap recorded in README.md.
    static std::uint64_t first_battle_frame = frame;
    if (const char* at = std::getenv("KOL_COVERAGE_AT")) {
        if (frame - first_battle_frame <
            static_cast<std::uint64_t>(std::strtoull(at, nullptr, 10)))
            return;
    }
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return;
    done = true;
    const int W = std::atoi(spec);
    if (W < 240 || W > 512) return;
    const int m = (W - 240) / 2;
    static std::uint32_t acc[512 * 160];
    for (int i = 0; i < W * 160; ++i) acc[i] = 0u;
    static std::uint32_t one[512 * 160];
    for (int bg = 0; bg < 4; ++bg) {
        for (int i = 0; i < W * 160; ++i) one[i] = 0xFFFF00FFu;
        blit_layer_rect(one, vram, pal, io, bg, -m, 0, W, 160);
        for (int i = 0; i < W * 160; ++i)
            if (one[i] != 0xFFFF00FFu) acc[i] |= (1u << bg);
    }
    auto bare = [&](int x0, int x1) {
        int n = 0;
        for (int y = 0; y < 160; ++y)
            for (int x = x0; x < x1; ++x) if (!acc[y * W + x]) ++n;
        return n;
    };
    const int lm = m ? bare(0, m) : 0;
    const int rm = m ? bare(W - m, W) : 0;
    const int mid = bare(m, m + 240);
    std::fprintf(stderr,
        "[cov] width=%d margin=%d  no-BG px: left=%d/%d (%.1f%%)  "
        "centre=%d/%d (%.1f%%)  right=%d/%d (%.1f%%)\n",
        W, m, lm, m * 160, m ? 100.0 * lm / (m * 160) : 0.0,
        mid, 240 * 160, 100.0 * mid / (240 * 160),
        rm, m * 160, m ? 100.0 * rm / (m * 160) : 0.0);
    if (const char* mp = std::getenv("KOL_COVERAGE_MASK")) {
        static std::uint32_t mask[512 * 160];
        for (int i = 0; i < W * 160; ++i)
            mask[i] = acc[i] ? 0xFF000000u : 0xFFFFFFFFu;   // white = no BG
        dump_bmp(mp, mask, W, 160);
    }
    std::fflush(stderr);
}

// KOL_BG_DUMP=<bg> rasterises one BG layer's visible 240x160 on its own, so a
// feature can be attributed to a layer instead of guessed at. Magenta marks
// pixels the layer leaves transparent.
void maybe_dump_bg(std::uint64_t frame) {
    static bool done = false;
    if (done) return;
    const char* spec = std::getenv("KOL_BG_DUMP");
    if (!spec || !spec[0]) return;
    const int bg = std::atoi(spec);
    if (bg < 0 || bg > 3) return;
    // KOL_BG_DUMP_ANY=1: any scene, not only the battle map (menus).
    if (!is_battle_scene() && !std::getenv("KOL_BG_DUMP_ANY")) return;
    // KOL_BG_DUMP_AT=<n> holds the dump until n frames after this run started,
    // so the same scene can be sampled before and after an input drives a
    // change. Counted RELATIVE to the first frame seen: `frame` is the absolute
    // PPU frame count, which a save state carries over, so an absolute gate
    // fires immediately (the same trap README.md records for INPUT_REPLAY).
    static std::uint64_t first_frame = frame;
    if (const char* at = std::getenv("KOL_BG_DUMP_AT")) {
        if (frame - first_frame <
            static_cast<std::uint64_t>(std::strtoull(at, nullptr, 10)))
            return;
    }
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return;
    done = true;
    static std::uint32_t buf[240 * 160];
    for (int i = 0; i < 240 * 160; ++i) buf[i] = 0xFFFF00FFu;
    blit_layer_rect(buf, vram, pal, io, bg, 0, 0, 240, 160);
    // The BG palette, bank by bank (BGR555 hex), to match a bank against ROM.
    for (int b = 0; b < 16; ++b) {
        std::fprintf(stderr, "[pal] bank%2d:", b);
        for (int i = 0; i < 16; ++i)
            std::fprintf(stderr, " %04X", pal[b * 32 + i * 2] | (pal[b * 32 + i * 2 + 1] << 8));
        std::fprintf(stderr, "\n");
    }
    if (const std::uint8_t* oam = bus->oam_ptr())   // the sprites: y, x, tile, bank, shape/size
        for (int i = 0; i < 128; ++i) {
            const int a0 = oam[i * 8] | (oam[i * 8 + 1] << 8), a1 = oam[i * 8 + 2] | (oam[i * 8 + 3] << 8);
            const int a2 = oam[i * 8 + 4] | (oam[i * 8 + 5] << 8);
            if ((a0 & 0x300) == 0x200) continue;   // hidden
            std::fprintf(stderr, "[oam] %3d y=%3d x=%3d tile=%03X bank=%d shape=%d size=%d first=", i,
                         a0 & 0xFF, a1 & 0x1FF, a2 & 0x3FF, (a2 >> 12) & 15, a0 >> 14, a1 >> 14);
            const std::uint32_t t0 = 0x10000u + (a2 & 0x3FFu) * 32u;   // its first tile (4bpp)
            for (int b = 0; b < 32; ++b) std::fprintf(stderr, "%02X", vram[t0 + b]);
            std::fprintf(stderr, "\n");
        }
    for (int b = 0; b < 16; ++b) {   // and the OBJ banks
        std::fprintf(stderr, "[objpal] bank%2d:", b);
        for (int i = 0; i < 16; ++i)
            std::fprintf(stderr, " %04X", pal[0x200 + b * 32 + i * 2] | (pal[0x200 + b * 32 + i * 2 + 1] << 8));
        std::fprintf(stderr, "\n");
    }
    // The layer's tiles 0x001-0x030 as stored (4bpp, 32 bytes each), to
    // find their art in ROM.
    {
        const std::size_t cnt = 0x08u + 2u * static_cast<std::size_t>(bg);
        const std::uint16_t bgcnt = static_cast<std::uint16_t>(io[cnt] | (io[cnt + 1] << 8));
        const std::uint32_t cb = ((bgcnt >> 2) & 3u) * 0x4000u;
        for (int t = 0x001; t <= 0x030; ++t) {
            std::fprintf(stderr, "[tile] bg%d %03X:", bg, t);
            for (int i = 0; i < 32; ++i) std::fprintf(stderr, "%02X", vram[cb + t * 32 + i]);
            std::fprintf(stderr, "\n");
        }
    }
    // Also print the tilemap entries covering the visible area, so a feature
    // can be traced to a tile index + palette bank rather than eyeballed.
    {
        const std::size_t cnt = 0x08u + 2u * static_cast<std::size_t>(bg);
        const std::size_t scr = 0x10u + 4u * static_cast<std::size_t>(bg);
        const std::uint16_t bgcnt =
            static_cast<std::uint16_t>(io[cnt] | (io[cnt + 1] << 8));
        const std::uint32_t sb = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        const unsigned size_code = (bgcnt >> 14) & 3u;
        const std::uint32_t tw = (size_code & 1u) ? 64u : 32u;
        const std::uint32_t th = (size_code & 2u) ? 64u : 32u;
        const std::uint32_t hofs = (io[scr] | (io[scr + 1] << 8)) & 0x1FFu;
        const std::uint32_t vofs = (io[scr + 2] | (io[scr + 3] << 8)) & 0x1FFu;
        for (int ty = 0; ty < 20; ++ty) {
            std::fprintf(stderr, "[map] bg%d row%2d:", bg, ty);
            for (int tx = 0; tx < 30; ++tx) {
                const std::uint32_t mx = ((tx * 8 + hofs) & (tw * 8u - 1u)) >> 3;
                const std::uint32_t my = ((ty * 8 + vofs) & (th * 8u - 1u)) >> 3;
                const std::uint32_t blk = (mx / 32u) + (my / 32u) * (tw / 32u);
                const std::uint32_t idx = (my % 32u) * 32u + (mx % 32u);
                const std::size_t o = sb + (blk * 1024u + idx) * 2u;
                const std::uint16_t e =
                    static_cast<std::uint16_t>(vram[o] | (vram[o + 1] << 8));
                std::fprintf(stderr, " %03X/%X", e & 0x3FFu, (e >> 12) & 0xFu);
            }
            std::fprintf(stderr, "\n");
        }
    }
    for (int bank = 0; bank < 8; ++bank) {
        std::fprintf(stderr, "[pal] bg bank %d:", bank);
        for (int c = 0; c < 16; ++c) {
            const std::size_t o = static_cast<std::size_t>(bank) * 32u + c * 2u;
            const std::uint16_t v =
                static_cast<std::uint16_t>(pal[o] | (pal[o + 1] << 8));
            std::fprintf(stderr, " %02X%02X%02X", ((v & 31) * 255 + 15) / 31,
                         (((v >> 5) & 31) * 255 + 15) / 31,
                         (((v >> 10) & 31) * 255 + 15) / 31);
        }
        std::fprintf(stderr, "\n");
    }
    {   // raw 4bpp char block, for offline tile matching
        const std::size_t cnt2 = 0x08u + 2u * static_cast<std::size_t>(bg);
        const std::uint16_t bc =
            static_cast<std::uint16_t>(io[cnt2] | (io[cnt2 + 1] << 8));
        const std::uint32_t cb = ((bc >> 2) & 3u) * 0x4000u;
        if (FILE* cf = std::fopen("/tmp/chr.bin", "wb")) {
            std::fwrite(vram + cb, 1, 0x4000, cf);
            std::fclose(cf);
        }
    }
    {   // raw BG map + EWRAM, for offline searching
        const std::size_t cnt3 = 0x08u + 2u * static_cast<std::size_t>(bg);
        const std::uint16_t bc3 =
            static_cast<std::uint16_t>(io[cnt3] | (io[cnt3 + 1] << 8));
        const std::uint32_t sb3 = ((bc3 >> 8) & 0x1Fu) * 0x800u;
        if (FILE* mf = std::fopen("/tmp/map.bin", "wb")) {
            std::fwrite(vram + sb3, 1, 0x1000, mf);
            std::fclose(mf);
        }
        if (FILE* wf = std::fopen("/tmp/iwram.bin", "wb")) {
            std::fwrite(bus->iwram_ptr(), 1, 32u * 1024u, wf);
            std::fclose(wf);
        }
        if (FILE* vf = std::fopen("/tmp/vram.bin", "wb")) {
            std::fwrite(vram, 1, 96u * 1024u, vf);
            std::fclose(vf);
        }
        std::uint32_t esz = 0;
        if (const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz)) {
            if (FILE* ef = std::fopen("/tmp/ewram.bin", "wb")) {
                std::fwrite(ew, 1, esz, ef);
                std::fclose(ef);
            }
        }
    }
    char path[256];
    std::snprintf(path, sizeof path, "/tmp/bg%d.bmp", bg);
    dump_bmp(path, buf, 240, 160);
    std::fprintf(stderr, "bg_dump: BG%d frame=%llu scrbase=%04X -> %s\n", bg, (unsigned long long)frame, (unsigned)(((static_cast<std::uint16_t>(io[0x08] | (io[0x09] << 8)) >> 8) & 0x1Fu) * 0x800u), path);
    std::fflush(stderr);
}

}  // namespace

// The six element orbs, which the terrain box always draws whatever the
// values are. Their presence in the rect is what distinguishes "the terrain
// box is on screen" from "something else is in those cells".
bool terrain_box_present(const std::uint8_t* vram, const std::uint8_t* io) {
    const std::uint16_t bg3 =
        static_cast<std::uint16_t>(io[0x0E] | (io[0x0F] << 8));
    const std::uint32_t sb = ((bg3 >> 8) & 0x1Fu) * 0x800u;
    bool seen[6] = {};
    for (int ty = 0; ty < 9; ++ty) {
        for (int tx = 23; tx < 30; ++tx) {
            const std::uint32_t o = sb + (ty * 32 + tx) * 2u;
            if (o + 1 >= 96u * 1024u) continue;
            const std::uint32_t t =
                (vram[o] | (vram[o + 1] << 8)) & 0x3FFu;
            if (t >= 0xA2u && t <= 0xA7u) seen[t - 0xA2u] = true;
        }
    }
    for (bool b : seen) if (!b) return false;
    return true;
}

bool capture_terrain_box(std::uint32_t* argb, int* w, int* h) {
    // Only meaningful on the battle map. Off it the UI layers hold something
    // else entirely — on the title screen this rect rasterised to intro
    // graphics, which is worse than showing nothing. Reuses the extended-view
    // scene classifier so the two cannot disagree.
    if (!is_battle_scene()) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !argb) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return false;
    // The rect is a fixed screen position, not a thing the game owns, so
    // whatever else the game puts there lands in the capture. A NAVI dialogue
    // ("Choose where you want to move this character") overwrites rows 2..7
    // with its own tiles, and the second screen showed the dialogue over the
    // terrain readout. Refuse the capture unless the box is actually there;
    // the caller then keeps the last good one.
    if (!terrain_box_present(vram, io)) return false;
    for (int i = 0; i < kBoxW * kBoxH; ++i) argb[i] = 0;
    // BG3 is the frame, BG2 the contents; equal priority means the lower BG
    // number wins, so BG2 goes on top.
    blit_layer(argb, vram, pal, io, 3);
    blit_layer(argb, vram, pal, io, 2);
    bool any = false;
    for (int i = 0; i < kBoxW * kBoxH && !any; ++i) any = argb[i] != 0;
    *w = kBoxW; *h = kBoxH;
    return any;
}

int terrain_box_pixels() { return kBoxW * kBoxH; }

bool terrain_box_on_screen() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !io) return false;
    return terrain_box_present(vram, io);
}

// The guest's unit panel FRAME: BG3 only, so the cream field and its border
// without any of BG2's text or icons. Used as a 9-slice background so the
// rebuilt panel can wear the game's own styling.
bool capture_unit_panel_frame(std::uint32_t* argb, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!unit_panel_rect(&px, &py, &pw, &ph)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !argb) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return false;
    for (int i = 0; i < pw * ph; ++i) argb[i] = 0;
    blit_layer_rect(argb, vram, pal, io, 3, px, py, pw, ph);
    *w = pw; *h = ph;
    return true;
}

int unit_panel_frame_pixels() { return 10 * 8 * 7 * 8; }

// ── The game's own glyphs ───────────────────────────────────────────────
// Text in the unit panel is one tile per character out of BG2's char block,
// and the mapping is fixed: digit d is tile 0x56 + d, and the labels are
// static pairs. Verified across three states by reading the tilemap under
// known values — Brigitte Lv1 HP130 gives tiles 57,59,56 and Rictor Lv3
// HP172 gives 57,5D,58.
//
// The palette bank differs per label, which is why MP comes out pink and HP
// blue exactly as in the game; taking one bank for everything would have
// flattened that.
namespace {

constexpr int kAtlasW = 80, kAtlasH = 16;

void render_tile(std::uint32_t* dst, int dstW, int dx, int dy,
                 std::uint32_t tile, std::uint32_t bank,
                 const std::uint8_t* vram, const std::uint8_t* pal,
                 std::uint32_t char_base) {
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::uint32_t a = char_base + tile * 32u + y * 4u + (x >> 1);
            if (a >= 96u * 1024u) continue;
            const std::uint8_t packed = vram[a];
            std::uint32_t idx = (x & 1) ? (packed >> 4) : (packed & 0x0Fu);
            if (!idx) continue;                     // transparent
            idx |= bank << 4;
            const std::uint16_t c = static_cast<std::uint16_t>(
                pal[idx * 2] | (pal[idx * 2 + 1] << 8));
            dst[(dy + y) * dstW + (dx + x)] = bgr555_to_argb(c);
        }
    }
}

}  // namespace

bool capture_font_atlas(std::uint32_t* argb, int* w, int* h) {
    if (!is_battle_scene()) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !argb) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return false;
    const std::uint16_t bg2 =
        static_cast<std::uint16_t>(io[0x0C] | (io[0x0D] << 8));
    const std::uint32_t cb = ((bg2 >> 2) & 3u) * 0x4000u;
    for (int i = 0; i < kAtlasW * kAtlasH; ++i) argb[i] = 0;
    for (int d = 0; d < 10; ++d)
        render_tile(argb, kAtlasW, d * 8, 0, 0x56u + d, 4u, vram, pal, cb);
    // row 1: HP, MP, Lv., EXP — each two tiles, each with its own bank.
    const std::uint32_t lab[4][3] = {
        {0xA8u, 0xA9u, 12u}, {0xAAu, 0xABu, 13u},
        {0xB3u, 0xB4u, 12u}, {0xB5u, 0xB6u, 12u},
    };
    for (int i = 0; i < 4; ++i) {
        render_tile(argb, kAtlasW, i * 16 + 0, 8, lab[i][0], lab[i][2],
                    vram, pal, cb);
        render_tile(argb, kAtlasW, i * 16 + 8, 8, lab[i][1], lab[i][2],
                    vram, pal, cb);
    }
    *w = kAtlasW; *h = kAtlasH;
    return true;
}

int font_atlas_pixels() { return kAtlasW * kAtlasH; }

// STR:/INT:/AGI: labels. These live ONLY on the status screen — the same char
// region holds different data during battle, checked against five battle
// states — so they are captured when that screen is up and cached by the
// caller for use on the battle map.
//
// BG1 there, char block 2, palette bank 12:
//   STR = 0x168,0x169   INT = 0x16B,0x16C   AGI = 0x16D,0x16E
// all sharing the colon tile 0x16A. Verified by reading the values beside
// them: 58,5A = 24, 57,58 = 12, 58,56 = 20, matching the unit's STR/INT/AGI.
bool capture_status_labels(std::uint32_t* argb, int* w, int* h) {
    if (!is_status_scene()) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !argb) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return false;
    const std::uint16_t bg1 =
        static_cast<std::uint16_t>(io[0x0A] | (io[0x0B] << 8));
    const std::uint32_t cb = ((bg1 >> 2) & 3u) * 0x4000u;
    const int W = 72, H = 8;
    for (int i = 0; i < W * H; ++i) argb[i] = 0;
    const std::uint32_t lab[3][2] = {
        {0x168u, 0x169u}, {0x16Bu, 0x16Cu}, {0x16Du, 0x16Eu},
    };
    for (int i = 0; i < 3; ++i) {
        render_tile(argb, W, i * 24 + 0, 0, lab[i][0], 12u, vram, pal, cb);
        render_tile(argb, W, i * 24 + 8, 0, lab[i][1], 12u, vram, pal, cb);
        render_tile(argb, W, i * 24 + 16, 0, 0x16Au, 12u, vram, pal, cb);
    }
    *w = W; *h = H;
    return true;
}

int status_label_pixels() { return 72 * 8; }

// The unit's name, which the game renders into a dynamically allocated run of
// tiles rather than indexing a static font — so it is taken as an image from
// the panel itself: 8 characters, 8x16 each, at panel-relative (+8, +16).
bool capture_unit_name(std::uint32_t* argb, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!unit_panel_rect(&px, &py, &pw, &ph)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !argb) return false;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return false;
    for (int i = 0; i < 64 * 16; ++i) argb[i] = 0;
    blit_layer_rect(argb, vram, pal, io, 2, px + 8, py + 16, 64, 16);
    *w = 64; *h = 16;
    return true;
}

int unit_name_pixels() { return 64 * 16; }

int current_terrain_id() { return g_last_id; }

bool terrain_modifiers(int id, int out[6]) {
    if (id < 0 || id >= kRecords || !g_rom) return false;
    const std::uint32_t off =
        (kTableBase - 0x08000000u) + static_cast<std::uint32_t>(id) * kStride +
        kModsOff;
    if (off + 6u > g_rom_size) return false;
    for (int k = 0; k < 6; ++k)
        out[k] = static_cast<signed char>(g_rom[off + k]);
    return true;
}

}  // namespace kol
