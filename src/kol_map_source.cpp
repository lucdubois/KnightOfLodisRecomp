#include "kol_dev.h"
#include "kol_map_source.h"

#include "kol_actions.h"   // debug_scene_viewer_active

#include "kol_settings.h"   // settings_dir

#include <algorithm>
#include <cstring>
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "kol_region.h"

// ── The composed map lives in EWRAM, and the tilemap is its lagging cache ──
//
// Found by watching what the guest's own tilemap writes READ
// (GBARECOMP_WRITE_WATCH on the BG0 screen blocks gave the blit at
// 0x08025672; GBARECOMP_READ_WATCH on EWRAM gave what it copies FROM). The
// blit reads u16 entries out of an EWRAM array and stores them straight into
// BG0 and BG1 — so that array is what the guest draws, and VRAM only ever
// holds the part of it the camera has reached.
//
//   array   = u32 at 0x02026CA8         BG0; BG1's own pointer follows it
//   stride  = 0x100 bytes = 128 u16 columns; the row count is the gap
//             between the two pointers, 96 in both battles measured
//   entry   = array + row * 0x100 + col * 2, ordinary tilemap entries
//
// BGxHOFS/VOFS position the view, but they cannot say WHERE in the map that
// is: the tilemap is a 64x32 ring over a map up to 128x96, so vofs/8 is short
// by 32 rows wherever the camera has passed the wrap (five of twelve
// reference states sit past it, where a vofs-derived row matches 2.5%-15%).
//
// The wrap is therefore established from the guest's own drawn cells, every
// frame: try each candidate ring and keep the one whose entries match what is
// already on screen. That is self-checking by construction — a wrong array
// pointer, a map being rebuilt, or a scene this does not understand all fail
// the same way, and failing means the margins simply fall back.
//
// The candidate rings run in BOTH directions, because the camera can sit off
// the map: the first battle parks the window five tile columns left of map
// column 0 and fills the rest with sea. A search that started at ring 0 found
// nothing there, disarmed, and left the margins on the wrapped tilemap.
//
// A byte pair in the same struct ((int8)[0x02026C94]+2, [0x02026C95]+2) does
// hold the window origin at rest, and matched all twelve states — but it does
// NOT track the camera during a scroll (measured 23 rows out mid-move), so
// using it to position the lookup tore the margin by whole tiles on 60 of
// 1,199 frames. It is not used.
//
// Verified on twelve reference states, both layers: every cell the guest has
// drawn (6,625 of them on BG0, 1,065 on BG1) is byte-identical to the array.
// That check is cheap, so it is not a one-off — map_source_tick() re-runs it
// every frame and refuses to arm when it fails. Drawing scenery the guest did
// not compose is the one thing this must never do.

namespace kol {
namespace {

constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kEwramSize = 256u * 1024u;
// A table of pointers, not a single one. The two the margins need are the
// composed maps for BG0 and BG1; the entries after them are two more arrays
// and the VRAM screen blocks the blit copies into:
//
//   0x02026CA8  02004440   BG0 composed map
//   0x02026CAC  0200A440   BG1 composed map
//   0x02026CB0  02010440   a third array
//   0x02026CB4  02013440   a fourth
//   0x02026CB8  06006000   BG0's blit destination in VRAM
//   0x02026CBC  06007000   BG1's
constexpr std::uint32_t kArrayPtrAddr = usa(0x02026CA8u);   // u32, BG0 array
constexpr std::uint32_t kBg1PtrAddr = usa(0x02026CACu);     // u32, BG1 array
constexpr int kStrideBytes = 0x100;                    // 128 entries per row
constexpr int kArrayCols = 128;
// Rows are DERIVED from the gap between those two pointers rather than
// assumed: BG1's array follows BG0's, so the gap is how many rows BG0 has.
// It measures 96 in both battles observed, with the maps bottom-aligned
// inside it (rows 59..91 in one, 42..91 in the other), but a battle that
// allocated differently would now be read correctly instead of being cut at
// a constant. Bounded so a garbage pointer pair cannot produce a huge scan.
constexpr int kMaxArrayRows = 256;
constexpr int kMinArrayRows = 16;
constexpr int kRingCols = 64;   // the BG ring the camera scrolls around, in
constexpr int kRingRows = 32;   // tiles: 64x32 for both terrain layers
constexpr int kWinCols = 31;    // window score() samples, in tiles
constexpr int kWinRows = 21;

const std::uint8_t* g_ewram = nullptr;
std::uint32_t g_array[2] = {0u, 0u};   // BG0, BG1, as guest addresses
int g_base_col[2] = {0, 0};            // absolute map column of hardware x 0
int g_base_row[2] = {0, 0};            // absolute map row of scanline 0
int g_fine_x[2] = {0, 0};              // hofs & 7, per layer
int g_fine_y[2] = {0, 0};              // vofs & 7, per layer
bool g_armed = false;
int g_wrap_col = 0;   // which 64-column ring of the map the camera is on
int g_wrap_row = 0;   // which 32-row ring
int g_array_rows = 0; // rows per layer, from the pointer gap
// A short grace for a frame whose drawn cells disagree while nothing moved:
// the camera's registers and ring as they were when last served, and how many
// frames in a row have been served on grace.
int g_last_reg_col[2] = {0, 0}, g_last_reg_row[2] = {0, 0};
int g_grace_frames = 0;
int g_step_frames = 0;   // frames in a row served on the step grace (`stepped`)
// Debug > Scene Map's viewer moves the map engine's camera (EWRAM
// 0x02026C78/7A, kol_actions.cpp) itself, so where the view is on the map is
// known even when too little is drawn to verify it (a camera panned off the
// map's edge): learned on verified frames as the map pixel of the window's
// origin less the camera, per layer, and used when there is nothing to match.
constexpr std::uint32_t kCameraXAddr = usa(0x02026C78u);
bool g_cam_known = false;
int g_cam_dc[2] = {0, 0}, g_cam_dr[2] = {0, 0};
constexpr int kGraceFrames = 2;
constexpr int kRearmFrames = 30;   // a fallback this short may re-arm on BG0 alone
// Low on purpose. The grace only applies while the camera is exactly still
// (same registers, same ring), and a repaint cannot move the map; only the
// camera can. Selecting a unit repaints the move range into VRAM a frame
// before the composed array follows, and agreement fell to 51.7% (select)
// and 67.9% (cancel) with the camera still: at 0.80 each fell back for one
// frame and flashed the wrapped tilemap -- the top of the map -- into the
// bottom margin (states/BR_flash_on_select.state, 480x270). 0.25 still
// refuses a different scene, and kGraceFrames still bounds it.
constexpr double kGraceAgree = 0.25;
// The camera a tile on (see `stepped`): its frame agreed at 83-85%.
constexpr double kStepAgree = 0.5;


bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

inline std::uint16_t ew16(std::uint32_t addr) {
    const std::uint32_t off = addr - kEwramBase;
    if (off + 1u >= kEwramSize) return 0u;
    return static_cast<std::uint16_t>(g_ewram[off] | (g_ewram[off + 1] << 8));
}

inline std::uint32_t ew32(std::uint32_t addr) {
    return static_cast<std::uint32_t>(ew16(addr)) |
           (static_cast<std::uint32_t>(ew16(addr + 2)) << 16);
}

// Floor division, because a margin sample's x is negative on the left.
inline int floor_div8(int v) { return (v >= 0) ? (v >> 3) : -((-v + 7) >> 3); }

// BG1's array as last verified whole, and whether it is served in place of
// the guest's (the re-arm in map_source_tick: map 84's scene loads the map
// again and BG1's array stays without its crystals for 12 frames after
// BG0's is whole, while the picture keeps them: the right cluster's top, in
// the top margin, was cut off flat for those frames).
std::vector<std::uint16_t> g_bg1_keep;
int g_bg1_keep_rows = 0;
std::uint32_t g_bg1_keep_of = 0u;
bool g_bg1_from_keep = false;
bool g_rearmed = false;   // serving after a re-arm on BG0 alone, until whole again
bool bg1_keep_fits();

inline bool array_entry(int bg, int row, int col, std::uint16_t* out) {
    if (row < 0 || row >= g_array_rows || col < 0 || col >= kArrayCols)
        return false;
    if (bg == 1 && g_bg1_from_keep) {
        *out = g_bg1_keep[static_cast<std::size_t>(row) * kArrayCols + col];
        return true;
    }
    const std::uint32_t addr = g_array[bg] +
                               static_cast<std::uint32_t>(row * kStrideBytes) +
                               static_cast<std::uint32_t>(col * 2);
    if (addr < kEwramBase || addr + 1u >= kEwramBase + kEwramSize) return false;
    *out = ew16(addr);
    return true;
}

// ── Map extensions (tools/map_editor) ──
//
// Scenery painted past a map's art, out of the map's own tiles, so a wide or
// tall view shows more of the place instead of the backdrop. Read from
// map_ext.cfg (beside enhancements.cfg; on the desktop the working directory,
// or KOL_MAP_EXT_FILE=<path>), one cell a line: "map layer row col entry",
// the entry a tilemap entry in hex, in the composed array's own coordinates.
// A cell is served only where the guest's array is empty on BOTH layers, so
// the extension never covers what the game composed, and only through the
// margins: the game sees none of it. No file, no change. The file is re-read
// when it changes on disk (checked every 30 ticks), so the editor's saves
// show up in a running game.
//
// Overrides repaint the game's own art (the editor's Override mode): lines
// "ov map layer row col entry orig", served on that layer wherever the
// guest's entry there is still `orig`, the one painted over (entry 0 clears
// the tile). Where the game changes the tile itself (the move range's
// highlight) the game's shows. Through the margins and, inside the native
// screen, gbarecomp's g_ws_tilemap_override_provider. Map 6's bush at 6,8 is
// half BG0 under the house's corner: moved onto BG1, it lifts whole.
constexpr std::uint32_t kMapNumberAddr = usa(0x02029D40u);   // u16, the header parser's
// Painted cells also reach left of the array's column 0 (map 94's art starts
// at column 1 and the camera shows ten columns past it) and below its last
// row (up to kMaxArrayRows): the margin sampler asks for those cells too and
// the guest's array has none, so they are the extension's alone.
constexpr int kExtLeft = 64;
constexpr int kExtCols = kExtLeft + kArrayCols;
inline std::size_t ext_index(int row, int col) {
    return static_cast<std::size_t>(row) * kExtCols + static_cast<std::size_t>(col + kExtLeft);
}
struct MapExt {
    std::vector<std::uint16_t> cell[2];   // kExtCols * kMaxArrayRows (ext_index), 0 = none
    std::vector<std::uint32_t> ov[2];     // 0 = none, else 0x10000 | entry
    std::vector<std::uint16_t> orig[2];   // the guest's entry an override replaces
    unsigned ov_layers = 0;               // the layers with any override
};
std::map<int, MapExt> g_exts;
const MapExt* g_ext = nullptr;   // the current map's, or none

// A save that replaces one cell keeps the file's size, so the change is
// told by the mtime's nanoseconds.
#if defined(__APPLE__)
inline long kMtimeNs(const struct stat& st) { return st.st_mtimespec.tv_nsec; }
#elif defined(_WIN32)
inline long kMtimeNs(const struct stat&) { return 0; }   // (whole seconds there)
#else
inline long kMtimeNs(const struct stat& st) { return st.st_mtim.tv_nsec; }
#endif

std::string ext_path() {
    if (const char* f = std::getenv("KOL_MAP_EXT_FILE")) return f;
    const std::string dir = settings_dir();
    return dir.empty() ? std::string("map_ext.cfg") : dir + "/map_ext.cfg";
}

// map_ext.cfg as it was when the game was built (CMake: a raw string): what draws
// with no file beside enhancements.cfg, and in a public build always (kol_dev.h).
const char* const kBakedExt =
#include "kol_map_ext_baked.inc"
    ;

// From the file at `path`, or (empty path) the baked text.
void ext_load(const std::string& path) {
    g_exts.clear();
    FILE* fp = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
    if (!path.empty() && !fp) return;
    const char* baked = fp ? nullptr : kBakedExt;
    char line[256];
    auto next = [&]() -> bool {
        if (fp) return std::fgets(line, sizeof line, fp) != nullptr;
        if (!*baked) return false;
        const char* e = std::strchr(baked, '\n');
        const std::size_t n = e ? static_cast<std::size_t>(e - baked) : std::strlen(baked);
        const std::size_t k = std::min(n, sizeof line - 1);
        std::memcpy(line, baked, k);
        line[k] = '\0';
        baked += e ? n + 1 : n;
        return true;
    };
    int cells = 0, overrides = 0;
    while (next()) {
        int map = 0, layer = 0, row = 0, col = 0;
        unsigned entry = 0, orig = 0;
        if (line[0] == '#') continue;
        if (line[0] == 'o' && line[1] == 'v') {
            if (std::sscanf(line + 2, "%d %d %d %d %x %x", &map, &layer, &row, &col, &entry, &orig) != 6 ||
                layer < 0 || layer > 1 || row < 0 || row >= kMaxArrayRows || col < 0 ||
                col >= kArrayCols || entry > 0xFFFFu || orig > 0xFFFFu)
                continue;
            MapExt& m = g_exts[map];
            for (int b = 0; b < 2; ++b) {
                if (m.cell[b].empty()) m.cell[b].assign(static_cast<std::size_t>(kExtCols) * kMaxArrayRows, 0);
                if (m.ov[b].empty()) m.ov[b].assign(static_cast<std::size_t>(kArrayCols) * kMaxArrayRows, 0);
                if (m.orig[b].empty()) m.orig[b].assign(static_cast<std::size_t>(kArrayCols) * kMaxArrayRows, 0);
            }
            const std::size_t i = static_cast<std::size_t>(row) * kArrayCols + col;
            m.ov[layer][i] = 0x10000u | entry;
            m.orig[layer][i] = static_cast<std::uint16_t>(orig);
            m.ov_layers |= 1u << layer;
            ++overrides;
            continue;
        }
        if (std::sscanf(line, "%d %d %d %d %x", &map, &layer, &row, &col, &entry) != 5) continue;
        if (layer < 0 || layer > 1 || row < 0 || row >= kMaxArrayRows || col < -kExtLeft ||
            col >= kArrayCols || entry > 0xFFFFu)
            continue;
        MapExt& m = g_exts[map];
        for (auto& v : m.cell)
            if (v.empty()) v.assign(static_cast<std::size_t>(kExtCols) * kMaxArrayRows, 0);
        m.cell[layer][ext_index(row, col)] =
            static_cast<std::uint16_t>(entry);
        ++cells;
    }
    if (fp) std::fclose(fp);
    std::fprintf(stderr, "map_ext: %d cells, %d overrides on %zu map(s) from %s\n", cells, overrides,
                 g_exts.size(), fp ? path.c_str() : "the build");
}

// Per tick: reload on a change, then pick the current map's extension.
void ext_tick() {
    static const bool off = env_on("KOL_NO_MAP_EXT");
    static int countdown = 0;
    static std::string path;
    static long long stamp = -2;   // -1: no file
    g_ext = nullptr;
    if (off) return;
    if constexpr (!kol::kDevTools) {
        // A public build: the baked extensions, read once; no file looked at.
        if (stamp == -2) ext_load(std::string());
        stamp = -3;
    } else if (--countdown <= 0) {
        countdown = 30;
        if (path.empty()) path = ext_path();
        struct stat st {};
        const long long now = ::stat(path.c_str(), &st) == 0
                                  ? static_cast<long long>(st.st_mtime) * 1000000000LL +
                                        static_cast<long long>(kMtimeNs(st)) +
                                        static_cast<long long>(st.st_size) * 7
                                  : -1;
        // No file (a device that never had one): the baked extensions. The file,
        // where there is one, is all of it, re-read as the editor saves.
        if (now != stamp) {
            stamp = now;
            ext_load(now < 0 ? std::string() : path);
        }
    }
    if (g_exts.empty()) return;
    const auto it = g_exts.find(static_cast<int>(ew16(kMapNumberAddr)));
    if (it != g_exts.end()) g_ext = &it->second;
}

// The extension's entry for a cell the guest left empty on both layers.
bool ext_entry(int bg, int row, int col, std::uint16_t* out) {
    if (!g_ext || row < 0 || row >= kMaxArrayRows || col < -kExtLeft || col >= kArrayCols)
        return false;
    const std::uint16_t e = g_ext->cell[bg][ext_index(row, col)];
    const std::uint16_t o = g_ext->cell[bg ^ 1][ext_index(row, col)];
    if (e == 0 && o == 0) return false;
    std::uint16_t a = 0;
    for (int b = 0; b < 2; ++b)
        if (array_entry(b, row, col, &a) && a != 0) return false;
    *out = e;
    return true;
}

// The override for a cell whose guest entry is `cur`, if it is still the one
// the override painted over.
bool ov_entry(int bg, int row, int col, std::uint16_t cur, std::uint16_t* out) {
    if (!g_ext || !(g_ext->ov_layers & (1u << bg)) || row < 0 || row >= kMaxArrayRows || col < 0 ||
        col >= kArrayCols)
        return false;
    const std::size_t i = static_cast<std::size_t>(row) * kArrayCols + col;
    const std::uint32_t v = g_ext->ov[bg][i];
    if (!v || g_ext->orig[bg][i] != cur) return false;
    *out = static_cast<std::uint16_t>(v & 0xFFFFu);
    return true;
}

std::uint16_t vram_entry(const std::uint8_t* vram, std::uint16_t bgcnt,
                         int tx, int ty) {
    const std::uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
    const unsigned size_code = (bgcnt >> 14) & 3u;
    const int w_tiles = (size_code & 1u) ? 64 : 32;
    const int h_tiles = (size_code & 2u) ? 64 : 32;
    tx = ((tx % w_tiles) + w_tiles) % w_tiles;
    ty = ((ty % h_tiles) + h_tiles) % h_tiles;
    const int block = (ty / 32) * (w_tiles / 32) + (tx / 32);
    const std::uint32_t off = screen_base + block * 0x800u +
                              ((ty % 32) * 32 + (tx % 32)) * 2u;
    if (off + 1u >= 96u * 1024u) return 0u;
    return static_cast<std::uint16_t>(vram[off] | (vram[off + 1] << 8));
}

// Report only when the lookup changes between serving the margins and falling
// back, and say how many frames the previous state lasted — a transient at a
// state load and a scene it never understands look identical otherwise.
//
// Every disarm reports, and says WHY. The early returns below used to bail out
// before this was reached, so a battle map the lookup could not position left
// no trace at all while the margins quietly reverted to the wrapped tilemap —
// which is the defect this whole file exists to remove. A fallback that is not
// logged reads as "it works", and did.
void report_state(bool armed, const char* why, double agree, int drawn) {
    static int last = -1;
    static long run = 0;
    if (armed == (last == 1)) { ++run; return; }
    if (last >= 0)
        std::fprintf(stderr, "map_source: %s after %ld frames %s (%s, %.1f%% of %d)\n",
                     armed ? "serving margins" : "falling back", run,
                     last == 1 ? "serving" : "in fallback", why, 100.0 * agree, drawn);
    else
        std::fprintf(stderr,
                     "map_source: %s (%s, array=%08X, %.1f%% of %d drawn "
                     "cells agree)\n",
                     armed ? "serving margins" : "falling back", why,
                     g_array[0], 100.0 * agree, drawn);
    last = armed ? 1 : 0;
    run = 1;
}

}  // namespace

namespace {
bool bg1_keep_fits() {
    return g_bg1_keep_of == g_array[1] && g_bg1_keep_rows == g_array_rows &&
           g_bg1_keep.size() == static_cast<std::size_t>(g_array_rows) * kArrayCols;
}
}  // namespace

bool map_source_armed() { return g_armed; }
bool map_source_rearmed() { return g_armed && g_rearmed; }

void map_source_bg1_kept_into(unsigned char* ewram_copy, unsigned long size) {
    if (!g_armed || !g_bg1_from_keep || !ewram_copy || g_array[1] < kEwramBase) return;
    const std::uint32_t base = g_array[1] - kEwramBase;
    for (int r = 0; r < g_bg1_keep_rows; ++r)
        for (int c = 0; c < kArrayCols; ++c) {
            const std::size_t at = base + static_cast<std::size_t>(r) * kStrideBytes + static_cast<std::size_t>(c) * 2u;
            if (at + 2u > size) return;
            const std::uint16_t v = g_bg1_keep[static_cast<std::size_t>(r) * kArrayCols + c];
            ewram_copy[at] = static_cast<unsigned char>(v & 0xFFu);
            ewram_copy[at + 1] = static_cast<unsigned char>(v >> 8);
        }
}

void map_source_tick(const std::uint8_t* ewram, const std::uint8_t* vram,
                     const std::uint8_t* io, bool battle_map) {
    const bool was_armed = g_armed;
    g_armed = false;
    g_bg1_from_keep = false;   // (the scoring below reads the guest's)
    // Battle-map frames since the margins were last served (the re-arm below).
    static int since_armed = kRearmFrames + 1;
    since_armed = was_armed ? 0 : !battle_map ? kRearmFrames + 1 : std::min(since_armed + 1, kRearmFrames + 1);
    // Off the battle map the margins are pillarboxed by policy, so this is not
    // a fallback and is not worth a line.
    if (!battle_map) return;
    if (!ewram || !vram || !io) {
        report_state(false, "no memory view", 0.0, 0);
        return;
    }
    if (env_on("KOL_WS_NO_MAP_SOURCE")) {
        report_state(false, "KOL_WS_NO_MAP_SOURCE", 0.0, 0);
        return;
    }
    g_ewram = ewram;
    ext_tick();

    const std::uint32_t ptr = ew32(kArrayPtrAddr);
    const std::uint32_t ptr1 = ew32(kBg1PtrAddr);
    g_array[0] = ptr;
    g_array[1] = ptr1;
    if (ptr < kEwramBase || ptr1 <= ptr) {
        report_state(false, "array pointers not a layer pair", 0.0, 0);
        return;
    }
    const std::uint32_t span = ptr1 - ptr;
    g_array_rows = static_cast<int>(span / kStrideBytes);
    if (span % kStrideBytes != 0u || g_array_rows < kMinArrayRows ||
        g_array_rows > kMaxArrayRows) {
        report_state(false, "layer gap is not a row count", 0.0, 0);
        return;
    }
    if (ptr1 + span >= kEwramBase + kEwramSize) {
        report_state(false, "array pointer out of EWRAM", 0.0, 0);
        return;
    }

    std::uint16_t bgcnt[2] = {0, 0};
    int reg_col[2] = {0, 0}, reg_row[2] = {0, 0};
    for (int bg = 0; bg < 2; ++bg) {
        const std::size_t cnt_off = 0x08 + 2u * static_cast<std::size_t>(bg);
        const std::size_t scr_off = 0x10 + 4u * static_cast<std::size_t>(bg);
        bgcnt[bg] =
            static_cast<std::uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        const int hofs = (io[scr_off] | (io[scr_off + 1] << 8)) & 0x1FF;
        const int vofs = (io[scr_off + 2] | (io[scr_off + 3] << 8)) & 0x1FF;
        g_fine_x[bg] = hofs & 7;
        g_fine_y[bg] = vofs & 7;
        reg_col[bg] = hofs / 8;
        reg_row[bg] = vofs / 8;
    }
    // BG1 off (DISPCNT bit 9): its cells are no evidence and its scroll no
    // camera. Map 117's last shot (states/thor/0223) turns the pier's BG1 off
    // and goes on scrolling it as the pier draws away: its cells disagreed more
    // each frame, the source fell back every 12 frames and the band over the
    // game's 160 showed the tilemap's ring, the sail's own rows again (the
    // user, 2026-11-11, reported the top band broken).
    // KOL_MAP_SOURCE_BG1_OFF_COUNTS=1: as before.
    static const bool bg1_off_counts = env_on("KOL_MAP_SOURCE_BG1_OFF_COUNTS");
    const bool bg1_shown = bg1_off_counts || ((io[0x00] | (io[0x01] << 8)) & 0x0200u) != 0u;
    if (!bg1_shown && was_armed) {
        reg_col[1] = g_last_reg_col[1];
        reg_row[1] = g_last_reg_row[1];
    }

    // Score one candidate wrap over every cell the guest has actually drawn.
    // Empty cells are the ones it has not reached — the whole point — so they
    // are not evidence either way.
    auto score = [&](int kcol, int mrow, int* out_drawn) {
        int ok = 0, drawn = 0;
        for (int bg = 0; bg < (bg1_shown ? 2 : 1); ++bg) {
            const int base_c = reg_col[bg] + kcol * 64;
            const int base_r = reg_row[bg] + mrow * 32;
            for (int r = 0; r < kWinRows; ++r) {
                for (int c = 0; c < kWinCols; ++c) {
                    const std::uint16_t v = vram_entry(
                        vram, bgcnt[bg], reg_col[bg] + c, reg_row[bg] + r);
                    if ((v & 0x3FFu) == 0u) continue;
                    ++drawn;
                    std::uint16_t a = 0;
                    if (array_entry(bg, base_r + r, base_c + c, &a) && a == v)
                        ++ok;
                }
            }
        }
        if (out_drawn) *out_drawn = drawn;
        return ok;
    };

    // Last frame's wrap first: the camera crosses a ring boundary rarely, and
    // trying the previous answer first keeps the common frame to one pass.
    int best_k = g_wrap_col, best_m = g_wrap_row, best_ok = -1, drawn = 0;
    if (was_armed) best_ok = score(best_k, best_m, &drawn);
    if (drawn && best_ok * 100 < drawn * 95) best_ok = -1;
    if (best_ok < 0) {
        // The camera can sit OFF the composed map. This game parks the first
        // battle five tile columns LEFT of map column 0, with the sea filling
        // everything before it, so the window's true origin is negative — and
        // a search that starts at ring 0 cannot express that. It scored 0% on
        // every candidate there, disarmed, and handed the margins silently
        // back to the wrapped tilemap: the mangled right border.
        //
        // So enumerate rings in BOTH directions, keeping only those whose
        // sampled window still overlaps the array at all.
        auto overlaps = [](int base, int span, int limit) {
            return base + span > 0 && base < limit;
        };
        for (int k = -kArrayCols / kRingCols; k <= kArrayCols / kRingCols; ++k) {
            if (!overlaps(reg_col[0] + k * kRingCols, kWinCols, kArrayCols))
                continue;
            for (int m = -g_array_rows / kRingRows;
                 m <= g_array_rows / kRingRows; ++m) {
                if (!overlaps(reg_row[0] + m * kRingRows, kWinRows,
                              g_array_rows))
                    continue;
                int d = 0;
                const int ok = score(k, m, &d);
                drawn = d;
                if (ok > best_ok) { best_ok = ok; best_k = k; best_m = m; }
            }
        }
    }

    const double agree = drawn ? double(best_ok) / double(drawn) : 0.0;
    // The guest repaints part of its tilemap between frames (toggling view
    // mode in battle preparation adds or removes the deployment highlight),
    // and for that one frame VRAM and the array disagree on about 13% of the
    // drawn cells with the camera standing still. Falling back then showed
    // the wrapped tilemap in the margins for a frame. So while the camera's
    // registers and ring are exactly those last served, a still-strong
    // agreement keeps serving for a frame or two. A wrong pointer, a rebuilt
    // map or a moving camera fails as before.
    const bool still = was_armed && best_k == g_wrap_col && best_m == g_wrap_row &&
                       reg_col[0] == g_last_reg_col[0] && reg_row[0] == g_last_reg_row[0] &&
                       reg_col[1] == g_last_reg_col[1] && reg_row[1] == g_last_reg_row[1];
    const bool grace = still && drawn >= 64 && agree < 0.95 && agree >= kGraceAgree &&
                       g_grace_frames < kGraceFrames;
    if (grace) {
        ++g_grace_frames;
        g_armed = true;
        g_bg1_from_keep = g_rearmed && bg1_keep_fits();
        return;
    }
    // BG1 alone repainted, the camera still: BG0 is the map's ground, and it
    // still agrees. Map 64 (the snowy camp, states/thor/0161) rewrites BG1
    // over the whole window for frames on end (up to 275 of its cells, BG0's
    // all agreeing), the grace ran out and the margins fell to the black sky
    // for 2 frames every few seconds (the user saw the sides and top
    // flash now and then on this map). A wrong map or a moving camera breaks BG0's
    // agreement as before. KOL_MAP_SOURCE_BG1_STRICT=1: as before.
    // And after a short fallback, at the registers and ring last served: map
    // 84's story scene loads the map again with its stairs (states/thor/0183,
    // frames 336-357 after the load: the arrays rewritten over 10 frames, no
    // picture shown, then BG0's whole and BG1's not yet for 12 more, while
    // VRAM keeps the old picture: BG1 as it was, and on BG0 124 cells of the
    // slab of cliff over the stairs' mouth that the array no longer has, 79%
    // of BG0's drawn cells agreeing), and through those 12 the margins showed
    // the sky and the wrapped tilemap. Then three cells in four are enough.
    // KOL_MAP_SOURCE_NO_REARM_BG0=1: as before.
    static const bool bg1_strict = env_on("KOL_MAP_SOURCE_BG1_STRICT");
    static const bool no_rearm = env_on("KOL_MAP_SOURCE_NO_REARM_BG0");
    const bool rearm = !no_rearm && !was_armed && since_armed <= kRearmFrames &&
                       best_k == g_wrap_col && best_m == g_wrap_row &&
                       reg_col[0] == g_last_reg_col[0] && reg_row[0] == g_last_reg_row[0] &&
                       reg_col[1] == g_last_reg_col[1] && reg_row[1] == g_last_reg_row[1];
    if (!was_armed || agree >= 0.95) g_rearmed = false;
    if (!bg1_strict && (still || rearm) && agree < 0.95) {
        int ok0 = 0, drawn0 = 0;
        for (int r = 0; r < kWinRows; ++r)
            for (int c = 0; c < kWinCols; ++c) {
                const std::uint16_t v = vram_entry(vram, bgcnt[0], reg_col[0] + c, reg_row[0] + r);
                if ((v & 0x3FFu) == 0u) continue;
                ++drawn0;
                std::uint16_t a = 0;
                if (array_entry(0, reg_row[0] + best_m * 32 + r, reg_col[0] + best_k * 64 + c, &a) && a == v) ++ok0;
            }
        if (drawn0 >= 64 && ok0 * 100 >= drawn0 * (rearm || g_rearmed ? 75 : 95)) {
            g_rearmed = rearm || g_rearmed;
            g_bg1_from_keep = g_rearmed && bg1_keep_fits();
            g_grace_frames = 0;
            g_armed = true;
            return;
        }
    }
    g_grace_frames = 0;
    const bool viewer = kol::debug_scene_viewer_active();
    const int cam_x = static_cast<std::int16_t>(ew16(kCameraXAddr));
    const int cam_y = static_cast<std::int16_t>(ew16(kCameraXAddr + 2));
    // Outside the viewer too, but only to carry on from a served frame, never
    // to start, and only while every cell still drawn agrees: map 57's
    // platform in the void (states/thor/0201) pans up until under 64 of its
    // cells are on screen, the source fell back, and the rows over the game's
    // 160 showed the wrapped tilemap, the platform's edge again at the top
    // (the user, 2026-11-10, asked for a black top border while the camera
    // pans). KOL_MAP_SOURCE_NO_CAMERA_FOLLOW=1: the viewer alone, as before.
    static const bool no_follow = env_on("KOL_MAP_SOURCE_NO_CAMERA_FOLLOW");
    auto ring = [](double want, int reg, int span) {
        return static_cast<int>(std::lround((want - reg) / span));
    };
    bool follow = drawn < 64 && g_cam_known && (viewer || (was_armed && !no_follow));
    if (follow && !viewer) {
        int d = 0;
        const int ok = score(ring((cam_x + g_cam_dc[0]) / 8.0, reg_col[0], 64),
                             ring((cam_y + g_cam_dr[0]) / 8.0, reg_row[0], 32), &d);
        follow = ok == d;
    }
    if (follow) {
        // The ring from the camera: the window's map column is the camera's
        // learned offset away, and the ring the one that puts the hardware
        // column there.
        const int k = ring((cam_x + g_cam_dc[0]) / 8.0, reg_col[0], 64);
        const int m = ring((cam_y + g_cam_dr[0]) / 8.0, reg_row[0], 32);
        report_state(true, viewer ? "following the viewer's camera (nothing drawn to verify)"
                                  : "following the game's camera (too little drawn to verify)", agree, drawn);
        g_wrap_col = k;
        g_wrap_row = m;
        for (int bg = 0; bg < 2; ++bg) {
            g_last_reg_col[bg] = reg_col[bg];
            g_last_reg_row[bg] = reg_row[bg];
            g_base_col[bg] = reg_col[bg] + ring((cam_x + g_cam_dc[bg]) / 8.0, reg_col[bg], 64) * 64;
            g_base_row[bg] = reg_row[bg] + ring((cam_y + g_cam_dr[bg]) / 8.0, reg_row[bg], 32) * 32;
        }
        g_armed = true;
        return;
    }
    // And the camera one tile on, the same ring: the frame its scroll
    // crosses into the next tile, the guest's tilemap is behind it, and
    // for that frame 83-85% of the drawn cells agreed (the burning town's
    // pan, map 99, states/HQ: twice in 40 frames), so the margins fell back
    // to the backdrop for a frame and the sky flickered over them. The ring
    // is the one last served and the step a tile: the array, the map's own
    // composition, is right where VRAM has not caught up. Served at the new
    // registers. KOL_MAP_SOURCE_NO_STEP_GRACE=1: falls back, as before.
    static const bool no_step_grace = env_on("KOL_MAP_SOURCE_NO_STEP_GRACE");
    auto one_step = [](int now, int last) { return std::abs(now - last) <= 1; };
    const bool stepped = !no_step_grace && !still && g_step_frames < kGraceFrames && was_armed &&
                         best_k == g_wrap_col && best_m == g_wrap_row &&
                         one_step(reg_col[0], g_last_reg_col[0]) && one_step(reg_row[0], g_last_reg_row[0]) &&
                         one_step(reg_col[1], g_last_reg_col[1]) && one_step(reg_row[1], g_last_reg_row[1]) &&
                         drawn >= 64 && agree >= kStepAgree;
    g_step_frames = stepped && agree < 0.95 ? g_step_frames + 1 : 0;
    if (!stepped && (drawn < 64 || agree < 0.95)) {
        // Log the TRANSITION, with how long the previous state lasted. A cap
        // on repeats cannot tell a load transient from a permanent failure,
        // and that is exactly the question a fallback raises.
        report_state(false,
                     drawn < 64 ? "too little drawn to verify against"
                                : "no ring matches what the guest drew",
                     agree, drawn);
        if (!viewer) g_cam_known = false;
        return;
    }

    report_state(true, "verified against the guest's drawn cells", agree,
                 drawn);
    // BG1's array, whole (g_bg1_keep).
    {
        g_bg1_keep.resize(static_cast<std::size_t>(g_array_rows) * kArrayCols);
        for (int r = 0; r < g_array_rows; ++r)
            for (int c = 0; c < kArrayCols; ++c) {
                std::uint16_t a = 0;
                g_bg1_keep[static_cast<std::size_t>(r) * kArrayCols + c] = array_entry(1, r, c, &a) ? a : 0;
            }
        g_bg1_keep_rows = g_array_rows;
        g_bg1_keep_of = g_array[1];
    }
    g_wrap_col = best_k;
    g_wrap_row = best_m;
    for (int bg = 0; bg < 2; ++bg) {
        g_last_reg_col[bg] = reg_col[bg];
        g_last_reg_row[bg] = reg_row[bg];
    }
    for (int bg = 0; bg < 2; ++bg) {
        g_base_col[bg] = reg_col[bg] + best_k * 64;
        g_base_row[bg] = reg_row[bg] + best_m * 32;
    }
    for (int bg = 0; bg < 2; ++bg) {
        g_cam_dc[bg] = g_base_col[bg] * 8 - cam_x;
        g_cam_dr[bg] = g_base_row[bg] * 8 - cam_y;
    }
    g_cam_known = true;

    if (std::getenv("KOL_MAP_SOURCE_STATS")) {
        static double worst = 1.0;
        static long frames = 0, imperfect = 0;
        ++frames;
        if (agree < 1.0) ++imperfect;
        if (agree < worst) worst = agree;
        if ((frames % 300) == 0)
            std::fprintf(stderr,
                         "map_source: %ld frames armed, %ld below 100%%, "
                         "worst %.2f%%, wrap col%d row%d\n",
                         frames, imperfect, 100.0 * worst, g_wrap_col,
                         g_wrap_row);
    }
    g_armed = true;
}

int map_source_x_phase(int bg) {
    if (!g_armed || bg < 0 || bg > 1) return -1;
    return g_fine_x[bg];
}

bool map_source_cell_mapping(int bg, int* base_col, int* base_row, int* fine_x, int* fine_y) {
    if (!g_armed || bg < 0 || bg > 1) return false;
    *base_col = g_base_col[bg];
    *base_row = g_base_row[bg];
    *fine_x = g_fine_x[bg];
    *fine_y = g_fine_y[bg];
    return true;
}

bool map_source_world_cell(int bg, int hw_x, int screen_y, int* out_row,
                           int* out_col) {
    if (!g_armed || bg < 0 || bg > 1) return false;
    const int col = g_base_col[bg] + floor_div8(hw_x + g_fine_x[bg]);
    const int row = g_base_row[bg] + floor_div8(screen_y + g_fine_y[bg]);
    if (row < 0 || row >= g_array_rows || col < 0 || col >= kArrayCols)
        return false;
    *out_row = row;
    *out_col = col;
    return true;
}

const std::uint8_t* map_source_array(int bg, int* out_rows) {
    if (!g_armed || bg < 0 || bg > 1 || !g_ewram) return nullptr;
    const std::uint32_t off = g_array[bg] - kEwramBase;
    const std::uint32_t bytes =
        static_cast<std::uint32_t>(g_array_rows) * kStrideBytes;
    if (g_array[bg] < kEwramBase || off + bytes > kEwramSize) return nullptr;
    if (out_rows) *out_rows = g_array_rows;
    return g_ewram + off;
}

bool map_source_extent(MapExtent* out) {
    if (!g_armed || !out || g_array_rows > MapExtent::kMaxRows) return false;
    out->rows = g_array_rows;
    bool any = false;
    for (int r = 0; r < g_array_rows; ++r)
        for (int c = 0; c < kArrayCols; ++c) {
            std::uint16_t a0 = 0, a1 = 0;
            // The extension's cells count as art: the edge fade moves out
            // to where the painted scenery ends.
            const bool has = (array_entry(0, r, c, &a0) && a0 != 0) ||
                             (array_entry(1, r, c, &a1) && a1 != 0) ||
                             ext_entry(0, r, c, &a0) || ext_entry(1, r, c, &a1);
            out->filled[r * MapExtent::kCols + c] = has ? 1 : 0;
            std::uint16_t b1 = 0;
            out->bg1[r * MapExtent::kCols + c] = array_entry(1, r, c, &b1) ? b1 : 0;
            std::uint16_t d1 = out->bg1[r * MapExtent::kCols + c];
            ov_entry(1, r, c, d1, &d1);
            if (d1 == 0) ext_entry(1, r, c, &d1);
            out->bg1_drawn[r * MapExtent::kCols + c] = d1;
            any |= has;
        }
    if (!any) return false;
    out->origin_x = g_base_col[0] * 8 + g_fine_x[0];
    out->origin_y = g_base_row[0] * 8 + g_fine_y[0];
    out->hofs = g_last_reg_col[0] * 8 + g_fine_x[0];
    out->vofs = g_last_reg_row[0] * 8 + g_fine_y[0];
    return true;
}

bool map_source_entry(int bg, int hw_x, int screen_y, std::uint16_t* out) {
    if (!g_armed || bg < 0 || bg > 1 || !out) return false;
    const int col = g_base_col[bg] + floor_div8(hw_x + g_fine_x[bg]);
    const int row = g_base_row[bg] + floor_div8(screen_y + g_fine_y[bg]);
    std::uint16_t entry = 0;
    if (!array_entry(bg, row, col, &entry))   // past the array: painted cells only
        return ext_entry(bg, row, col, out);
    ov_entry(bg, row, col, entry, &entry);              // an override, if any
    if (entry == 0) ext_entry(bg, row, col, &entry);   // the map extension, if any
    *out = entry;
    return true;
}

unsigned map_source_override_layers() {
    return g_armed && g_ext ? g_ext->ov_layers : 0u;
}

bool map_source_override_entry(int bg, int hw_x, int screen_y, std::uint16_t cur,
                               std::uint16_t* out) {
    if (!g_armed || !g_ext || bg < 0 || bg > 1 || !out) return false;
    const int col = g_base_col[bg] + floor_div8(hw_x + g_fine_x[bg]);
    const int row = g_base_row[bg] + floor_div8(screen_y + g_fine_y[bg]);
    if (row < 0 || row >= g_array_rows) return false;
    return ov_entry(bg, row, col, cur, out);
}

bool map_source_fill_entry(int bg, int hw_x, int screen_y, std::uint16_t* out) {
    if (!g_armed || !g_ext || bg < 0 || bg > 1 || !out) return false;
    const int col = g_base_col[bg] + floor_div8(hw_x + g_fine_x[bg]);
    const int row = g_base_row[bg] + floor_div8(screen_y + g_fine_y[bg]);
    return ext_entry(bg, row, col, out);
}

}  // namespace kol
