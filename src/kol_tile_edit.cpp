// Debug > Tile Info's live editor (see kol_tile_edit.h).
#include "kol_dev.h"
#include "kol_tile_edit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "kol_actions.h"
#include "kol_focus.h"
#include "kol_settings.h"
#include "runtime.h"

namespace kol {
namespace {

constexpr int kSide = kTileSide;   // the field's cells along X and Z (see the header)
constexpr int kMaxHeight = 63;
constexpr int kShapes = 12;   // see TileRoof
constexpr int kFields = 9;   // height, eave, ridge, gables, axis, shape, open, shift x, shift z
constexpr int kBoxFields = 7;   // top, peak, size x, size z, shift x, shift z, axis

std::mutex g_mu;
std::map<int, std::vector<int>> g_edits;   // map -> heights by tile_cell, -1 none
std::map<int, std::vector<signed char>> g_types;   // map -> TileType by tile_cell
std::map<int, std::vector<TileRoof>> g_roofs;
std::map<int, std::vector<TileBox>> g_boxes;
std::map<int, TileSun> g_suns;
std::set<int> g_indoor;   // maps set indoors (the SUN tab's INDOOR)
std::map<int, std::vector<signed char>> g_water;   // map -> WaterTag by tile_cell
bool g_loaded = false;

std::atomic<int> g_target_map{0}, g_target_cell{-1}, g_target_drawn{0};
std::atomic<int> g_field{0};
std::atomic<int> g_terrain_field{0};   // with g_field 0: 0 the height, 1 the type
std::atomic<int> g_corner_map{0}, g_corner_cell{-1};
std::atomic<bool> g_box_focus{false};
std::atomic<int> g_box_field{0};
std::atomic<bool> g_sun_focus{false};   // the page's SUN tab
std::atomic<bool> g_water_focus{false};   // the page's WATER tab
std::atomic<int> g_water_brush{kWaterYes};

// Beside enhancements.cfg; KOL_TILE_EDIT_FILE=<path> where there is none
// (the desktop runs).
std::string path() {
    if constexpr (!kol::kDevTools) return std::string();   // a public build: the baked table alone
    if (const char* f = std::getenv("KOL_TILE_EDIT_FILE")) return f;
    const std::string dir = settings_dir();
    return dir.empty() ? std::string() : dir + "/drawn_heights.cfg";
}

// The heights, roofs and boxes set on the Thor so far, baked into the build
// (tools/bake_drawn_heights.py): what a device with no drawn_heights.cfg of
// its own starts from, and what the desktop runs draw. A device's file,
// once there is one, is all of it (the first edit saves these with it), so
// deleting a roof in the editor stays deleted. KOL_TILE_EDIT_FILE=/dev/null
// runs with none.
// The type names in the file, by TileType (square is no line).
const char* const kTypeWords[kTileTypes] = {"square", "left", "front", "right", "back"};
// The water tags' words, by WaterTag (the game's own is no line).
const char* const kWaterWords[kWaterTags] = {"auto", "water", "dry", "ice"};

const char* const kBaked =
#include "kol_drawn_baked.inc"
    ;

// Under g_mu.
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    const std::string p = path();
    std::FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "r");
    const char* baked = f ? nullptr : kBaked;
    // The next line of the file, or of the baked text when there is none.
    auto next = [&](char* line, std::size_t size) -> bool {
        if (f) return std::fgets(line, static_cast<int>(size), f) != nullptr;
        if (!baked || !*baked) return false;
        const char* end = std::strchr(baked, '\n');
        const std::size_t len = end ? static_cast<std::size_t>(end - baked) : std::strlen(baked);
        const std::size_t take = std::min(len, size - 1);
        std::memcpy(line, baked, take);
        line[take] = '\0';
        baked += len + (end ? 1 : 0);
        return true;
    };
    char line[160];
    int n = 0, roofs = 0, boxes = 0, types = 0, suns = 0, waters = 0;
    while (next(line, sizeof line)) {
        if (line[0] == '#') continue;
        if (std::strncmp(line, "indoor ", 7) == 0) {
            int map;
            if (std::sscanf(line + 7, "%d", &map) == 1) g_indoor.insert(map);
            continue;
        }
        if (std::strncmp(line, "sun ", 4) == 0) {
            int map;
            TileSun s{};
            if (std::sscanf(line + 4, "%d %f %f %f", &map, &s.degrees, &s.x, &s.z) != 4) continue;
            if (s.degrees <= 0.0f || s.degrees >= 90.0f || (s.x == 0.0f && s.z == 0.0f)) continue;
            g_suns[map] = s;
            ++suns;
            continue;
        }
        if (std::strncmp(line, "water ", 6) == 0) {
            int map, x, z;
            char word[16] = "";
            if (std::sscanf(line + 6, "%d %d %d %15s", &map, &x, &z, word) != 4 || !tile_in_field(x, z)) continue;
            for (int t = 1; t < kWaterTags; ++t)
                if (std::strcmp(word, kWaterWords[t]) == 0) {
                    auto& v = g_water[map];
                    if (v.empty()) v.assign(kSide * kSide, kWaterAuto);
                    v[tile_cell(x, z)] = static_cast<signed char>(t);
                    ++waters;
                }
            continue;
        }
        if (std::strncmp(line, "type ", 5) == 0) {
            int map, x, z;
            char word[16] = "";
            if (std::sscanf(line + 5, "%d %d %d %15s", &map, &x, &z, word) != 4 || !tile_in_field(x, z)) continue;
            for (int t = 1; t < kTileTypes; ++t)
                if (std::strcmp(word, kTypeWords[t]) == 0) {
                    auto& v = g_types[map];
                    if (v.empty()) v.assign(kSide * kSide, kTypeSquare);
                    v[tile_cell(x, z)] = static_cast<signed char>(t);
                    ++types;
                }
            continue;
        }
        if (std::strncmp(line, "box ", 4) == 0) {
            int map;
            TileBox b{};
            char axis = 0;
            if (std::sscanf(line + 4, "%d %d %d %d %d %d %d %d %d %c", &map, &b.x, &b.z, &b.top2, &b.peak2,
                            &b.size_x8, &b.size_z8, &b.dx8, &b.dz8, &axis) != 10)
                continue;
            if (!tile_in_field(b.x, b.z) || b.size_x8 < 1 || b.size_z8 < 1) continue;
            b.along_x = axis == 'X';
            g_boxes[map].push_back(b);
            ++boxes;
            continue;
        }
        if (std::strncmp(line, "roof ", 5) == 0) {
            int map, x0, z0, x1, z1, gables, eave2, ridge2, shape = 0, open = 0, dx8 = 0, dz8 = 0;
            char axis = 0;
            const int got = std::sscanf(line + 5, "%d %d %d %d %d %c %d %d %d %d %d %d %d", &map, &x0, &z0, &x1, &z1,
                                        &axis, &gables, &eave2, &ridge2, &shape, &open, &dx8, &dz8);
            if (got < 9) continue;
            if (!tile_in_field(x0, z0) || !tile_in_field(x1, z1) || x0 > x1 || z0 > z1 || gables < 1) continue;
            g_roofs[map].push_back({x0, z0, x1, z1, axis == 'X', gables, eave2, ridge2,
                                    std::clamp(shape, 0, kShapes - 1), open != 0, std::clamp(dx8, -8, 8),
                                    std::clamp(dz8, -8, 8)});
            ++roofs;
            continue;
        }
        int map, x, z, h;
        if (std::sscanf(line, "%d %d %d %d", &map, &x, &z, &h) != 4) continue;
        if (!tile_in_field(x, z) || h < 0 || h > kMaxHeight) continue;
        auto& v = g_edits[map];
        if (v.empty()) v.assign(kSide * kSide, -1);
        v[tile_cell(x, z)] = h;
        ++n;
    }
    if (f) std::fclose(f);
    std::fprintf(stderr, "tile edit: %d drawn heights, %d types, %d roofs, %d boxes, %d suns, %d water tags from %s\n",
                 n, types, roofs, boxes, suns, waters, f ? p.c_str() : "the build");
}

// Under g_mu.
void save_locked() {
    if constexpr (!kol::kDevTools) return;   // a public build writes no drawn_heights.cfg
    const std::string p = path();
    if (p.empty()) return;
    std::FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# Drawn heights set with Debug > Tile Info (L2 + up/down): map x z height\n");
    std::fprintf(f, "# Terrain types: type map x z left|front|right|back, the corner drawn rounded\n");
    std::fprintf(f, "# Roofs (L2 + B twice): roof map x0 z0 x1 z1 axis gables eave ridge shape open shift_x shift_z,\n"
                    "# the shifts in eighths of a cell (the whole roof moved off the grid), heights in\n"
                    "# half steps, shape 0 gable, 1 / 2 one slope falling toward + / - across the ridges,\n"
                    "# 3-6 a peak at the +X+Z, +X-Z, -X-Z, -X+Z corner, 7-10 a valley low at that corner,\n"
                    "# 11 a pyramid (peak in the middle); open 1: no walls, an awning on its posts\n"
                    "# Boxes (L2 + Select): box map x z top peak size_x size_z shift_x shift_z axis, heights in\n"
                    "# half steps, sizes and shifts in eighths of a cell; a gable over the walls if peak > top\n"
                    "# Suns (SUN tab, SET SUN): sun map degrees x z, the height over the ground and the\n"
                    "# direction toward it along the ground (as kol_focus kMapSuns)\n"
                    "# Indoors (SUN tab, INDOOR): indoor map, no sun there (as map_settings.cfg `indoor`)\n"
                    "# Water (WATER tab): water map x z water|dry, a cell counted as water or not\n");
    for (const auto& [map, v] : g_edits)
        for (int c = 0; c < kSide * kSide; ++c)
            if (v[c] >= 0) std::fprintf(f, "%d %d %d %d\n", map, tile_x(c), tile_z(c), v[c]);
    for (const auto& [map, v] : g_types)
        for (int c = 0; c < kSide * kSide; ++c)
            if (v[c] > kTypeSquare && v[c] < kTileTypes)
                std::fprintf(f, "type %d %d %d %s\n", map, tile_x(c), tile_z(c), kTypeWords[v[c]]);
    for (const auto& [map, list] : g_roofs)
        for (const TileRoof& r : list)
            std::fprintf(f, "roof %d %d %d %d %d %c %d %d %d %d %d %d %d\n", map, r.x0, r.z0, r.x1, r.z1,
                         r.along_x ? 'X' : 'Z', r.gables, r.eave2, r.ridge2, r.shape, r.open ? 1 : 0, r.dx8, r.dz8);
    for (const auto& [map, list] : g_boxes)
        for (const TileBox& b : list)
            std::fprintf(f, "box %d %d %d %d %d %d %d %d %d %c\n", map, b.x, b.z, b.top2, b.peak2, b.size_x8,
                         b.size_z8, b.dx8, b.dz8, b.along_x ? 'X' : 'Z');
    for (const auto& [map, s] : g_suns) std::fprintf(f, "sun %d %.1f %.3f %.3f\n", map, s.degrees, s.x, s.z);
    for (int map : g_indoor) std::fprintf(f, "indoor %d\n", map);
    for (const auto& [map, v] : g_water)
        for (int c = 0; c < kSide * kSide; ++c)
            if (v[c] > kWaterAuto && v[c] < kWaterTags)
                std::fprintf(f, "water %d %d %d %s\n", map, tile_x(c), tile_z(c), kWaterWords[v[c]]);
    std::fclose(f);
}

// The roof over the cell on the map, -1 none. Under g_mu.
int roof_at_locked(int map, int cell) {
    const auto it = g_roofs.find(map);
    if (it == g_roofs.end() || cell < 0) return -1;
    const int cx = tile_x(cell), cz = tile_z(cell);
    for (int i = 0; i < static_cast<int>(it->second.size()); ++i) {
        const TileRoof& r = it->second[i];
        if (cx >= r.x0 && cx <= r.x1 && cz >= r.z0 && cz <= r.z1) return i;
    }
    return -1;
}

// The box on the cell, -1 none. Under g_mu.
int box_at_locked(int map, int cell) {
    const auto it = g_boxes.find(map);
    if (it == g_boxes.end() || cell < 0) return -1;
    for (int i = 0; i < static_cast<int>(it->second.size()); ++i)
        if (tile_cell(it->second[i].x, it->second[i].z) == cell) return i;
    return -1;
}

// Whether the fields edit a box: the target's, while Select chose it. Under g_mu.
int focused_box_locked() {
    if (!g_box_focus.load()) return -1;
    return box_at_locked(g_target_map.load(), g_target_cell.load());
}

// D-pad up (+1) or down (-1) on the field being edited.
void change(int delta) {
    const int map = g_target_map.load(), cell = g_target_cell.load();
    if (cell < 0 || cell >= kSide * kSide) return;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const int bi = focused_box_locked();
    if (bi >= 0) {
        TileBox& b = g_boxes[map][bi];
        switch (g_box_field.load()) {
            case 0: b.top2 = std::clamp(b.top2 + delta, 0, 2 * kMaxHeight); break;
            case 1: b.peak2 = std::clamp(b.peak2 + delta, 0, 2 * kMaxHeight); break;
            case 2: b.size_x8 = std::clamp(b.size_x8 + delta, 1, 16); break;
            case 3: b.size_z8 = std::clamp(b.size_z8 + delta, 1, 16); break;
            case 4: b.dx8 = std::clamp(b.dx8 + delta, -8, 8); break;
            case 5: b.dz8 = std::clamp(b.dz8 + delta, -8, 8); break;
            default: b.along_x = !b.along_x; break;
        }
        save_locked();
        std::fprintf(stderr, "tile edit: map %d box %d,%d top %d peak %d size %dx%d shift %d,%d %c\n", map, b.x,
                     b.z, b.top2, b.peak2, b.size_x8, b.size_z8, b.dx8, b.dz8, b.along_x ? 'X' : 'Z');
        return;
    }
    const int field = g_field.load();
    const int ri = roof_at_locked(map, cell);
    if (field != 0 && ri >= 0) {
        TileRoof& r = g_roofs[map][ri];
        if (field == 1) r.eave2 = std::clamp(r.eave2 + delta, 0, 2 * kMaxHeight);
        if (field == 2) r.ridge2 = std::clamp(r.ridge2 + delta, 0, 2 * kMaxHeight);
        if (field == 3) r.gables = std::clamp(r.gables + delta, 1, 8);
        if (field == 4) r.along_x = !r.along_x;
        if (field == 5) r.shape = (r.shape + (delta > 0 ? 1 : kShapes - 1)) % kShapes;
        if (field == 6) r.open = !r.open;
        if (field == 7) r.dx8 = std::clamp(r.dx8 + delta, -8, 8);
        if (field == 8) r.dz8 = std::clamp(r.dz8 + delta, -8, 8);
        save_locked();
        std::fprintf(stderr, "tile edit: map %d roof %d,%d-%d,%d %c x%d eave %d ridge %d (half steps) shift %d,%d/8\n",
                     map, r.x0, r.z0, r.x1, r.z1, r.along_x ? 'X' : 'Z', r.gables, r.eave2, r.ridge2, r.dx8, r.dz8);
        return;
    }
    if (g_terrain_field.load() == 1) {
        auto& t = g_types[map];
        if (t.empty()) t.assign(kSide * kSide, kTypeSquare);
        t[cell] = static_cast<signed char>((t[cell] + (delta > 0 ? 1 : kTileTypes - 1)) % kTileTypes);
        save_locked();
        std::fprintf(stderr, "tile edit: map %d cell %d,%d %s\n", map, tile_x(cell), tile_z(cell),
                     tile_type_name(t[cell]));
        return;
    }
    auto& v = g_edits[map];
    if (v.empty()) v.assign(kSide * kSide, -1);
    v[cell] = std::clamp(g_target_drawn.load() + delta, 0, kMaxHeight);
    g_target_drawn.store(v[cell]);   // a second press before the next frame
    save_locked();
    std::fprintf(stderr, "tile edit: map %d cell %d,%d %d\n", map, tile_x(cell), tile_z(cell), v[cell]);
}

// The cell's height and type back to the game's. Under g_mu.
void reset_terrain_locked(int map, int cell) {
    auto& v = g_edits[map];
    if (v.empty()) v.assign(kSide * kSide, -1);
    v[cell] = -1;
    if (auto it = g_types.find(map); it != g_types.end()) it->second[cell] = kTypeSquare;
    std::fprintf(stderr, "tile edit: map %d cell %d,%d reset\n", map, tile_x(cell), tile_z(cell));
}

// A: on a roof, delete it; else the cell's height back to the game's.
void reset() {
    const int map = g_target_map.load(), cell = g_target_cell.load();
    if (cell < 0 || cell >= kSide * kSide) return;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const int bi = focused_box_locked();
    if (bi >= 0) {
        g_boxes[map].erase(g_boxes[map].begin() + bi);
        g_box_focus = false;
        save_locked();
        std::fprintf(stderr, "tile edit: map %d box deleted\n", map);
        return;
    }
    const int ri = roof_at_locked(map, cell);
    if (ri >= 0) {
        g_roofs[map].erase(g_roofs[map].begin() + ri);
        std::fprintf(stderr, "tile edit: map %d roof deleted\n", map);
    } else {
        reset_terrain_locked(map, cell);
    }
    g_field = 0;
    save_locked();
}

// B: a roof's corner; the second press makes the roof. It starts as one
// gable, its ridge along the rectangle's longer side, the eave at the
// target's drawn height and the ridge three steps over it.
void corner() {
    const int map = g_target_map.load(), cell = g_target_cell.load();
    if (cell < 0 || cell >= kSide * kSide) return;
    const int first = g_corner_cell.load();
    if (first < 0 || g_corner_map.load() != map) {
        g_corner_map = map;
        g_corner_cell = cell;
        return;
    }
    g_corner_cell = -1;
    tile_edit_make_roof(map, first, cell);
}

}  // namespace

void tile_edit_make_roof(int map, int first, int cell, int eave2) {
    if (first < 0 || cell < 0 || first >= kSide * kSide || cell >= kSide * kSide) return;
    TileRoof r;
    r.x0 = std::min(tile_x(first), tile_x(cell));
    r.x1 = std::max(tile_x(first), tile_x(cell));
    r.z0 = std::min(tile_z(first), tile_z(cell));
    r.z1 = std::max(tile_z(first), tile_z(cell));
    r.along_x = r.x1 - r.x0 >= r.z1 - r.z0;
    r.gables = 1;
    r.eave2 = eave2 >= 0 ? eave2 : 2 * g_target_drawn.load();
    r.ridge2 = r.eave2 + 6;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    auto& list = g_roofs[map];
    // A new roof replaces any it overlaps.
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const TileRoof& o) {
                                  return o.x0 <= r.x1 && r.x0 <= o.x1 && o.z0 <= r.z1 && r.z0 <= o.z1;
                              }),
               list.end());
    list.push_back(r);
    g_field = 2;   // the ridge, the first thing to set
    g_box_focus = false;
    save_locked();
    std::fprintf(stderr, "tile edit: map %d roof %d,%d-%d,%d\n", map, r.x0, r.z0, r.x1, r.z1);
}

namespace {

// Select: a box on the target cell, a chimney to start with (3 eighths
// square, flat, a step over the roof there or the cell); on a cell with one,
// the fields go from the box to the rest and back.
void box() {
    const int map = g_target_map.load(), cell = g_target_cell.load();
    if (cell < 0 || cell >= kSide * kSide) return;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (box_at_locked(map, cell) >= 0) {
        g_box_focus = !g_box_focus.load();
        return;
    }
    TileBox b{};
    b.x = tile_x(cell);
    b.z = tile_z(cell);
    const int ri = roof_at_locked(map, cell);
    b.top2 = (ri >= 0 ? g_roofs[map][ri].ridge2 : 2 * g_target_drawn.load()) + 2;
    b.peak2 = b.top2;
    b.size_x8 = b.size_z8 = 3;
    b.along_x = true;
    g_boxes[map].push_back(b);
    g_box_focus = true;
    g_box_field = 0;
    save_locked();
    std::fprintf(stderr, "tile edit: map %d box %d,%d\n", map, b.x, b.z);
}

std::uint16_t (*g_prev_filter)(std::uint16_t) = nullptr;

std::uint16_t filter(std::uint16_t keys) {
    if (g_prev_filter) keys = g_prev_filter(keys);
    // The scene viewer: the scene stays on its first line (A, B, Select and
    // Start never reach the game); the D-pad still does, it steps the cursor.
    // Read first: hidden before the editor looked, L2 + A and L2 + B did
    // nothing in the viewer (the user, on the Thor).
    const std::uint16_t pad = keys;
    if (debug_scene_viewer_active()) keys = static_cast<std::uint16_t>(keys | 0x000Fu);
    using clock = std::chrono::steady_clock;
    static unsigned was = 0;
    static clock::time_point next_repeat[2];
    // KOL_TILE_EDIT_HOLD=1: L2 counts as held (headless runs, no pad).
    static const bool hold_env = [] {
        const char* v = std::getenv("KOL_TILE_EDIT_HOLD");
        return v && v[0] && v[0] != '0';
    }();
    const bool held = setting_on(Setting::DebugTileInfo) &&
                      (hold_env || gbarecomp::g_host_pad_left_trigger > 0.5f);
    if (!held) {
        was = 0;
        return keys;
    }
    const unsigned now_keys = ~pad & 0x3FFu;
    const auto now = clock::now();
    // Up (bit 6) and down (bit 7): a press, then every 120 ms after 350 ms.
    for (int i = 0; i < 2; ++i) {
        const unsigned bit = 1u << (6 + i);
        if (!(now_keys & bit)) continue;
        bool fire = false;
        if (!(was & bit)) {
            fire = true;
            next_repeat[i] = now + std::chrono::milliseconds(350);
        } else if (now >= next_repeat[i]) {
            fire = true;
            next_repeat[i] = now + std::chrono::milliseconds(120);
        }
        if (fire) change(i == 0 ? +1 : -1);
    }
    // Right (bit 4) / left (bit 5): the next / previous field, on a roof.
    auto pressed = [&](unsigned bit) { return (now_keys & bit) && !(was & bit); };
    if (pressed(0x10u) || pressed(0x20u)) {
        int on_roof = -1, on_box = -1;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            load_locked();
            on_roof = roof_at_locked(g_target_map.load(), g_target_cell.load());
            on_box = focused_box_locked();
        }
        if (on_box >= 0) {
            const int f = g_box_field.load() % kBoxFields;
            g_box_field = (f + (pressed(0x10u) ? 1 : kBoxFields - 1)) % kBoxFields;
        } else {
            // The terrain's two (height, type), then a roof's eight.
            const int n = on_roof >= 0 ? kFields + 1 : 2;
            const int f = (g_field.load() == 0 ? g_terrain_field.load() : g_field.load() + 1) % n;
            const int next = (f + (pressed(0x10u) ? 1 : n - 1)) % n;
            g_field = next < 2 ? 0 : next - 1;
            if (next < 2) g_terrain_field = next;
        }
    }
    if (pressed(0x01u)) reset();    // A
    if (pressed(0x02u)) corner();   // B
    if (pressed(0x04u)) box();      // Select
    was = now_keys;
    // The game sees neither A, B, Select nor the D-pad while L2 is held.
    return static_cast<std::uint16_t>(keys | 0x00F7u);
}

}  // namespace

void tile_edit_test_tick(unsigned long frame) {
    // KOL_TILE_EDIT_SCRIPT=<frame>:<keys hex, active low>,... feeds the pad
    // to the editor's filter at those absolute frames, as the host's poll
    // does (the input replay writes the game's keys after the filter, so it
    // cannot drive the editor). With KOL_TILE_EDIT_HOLD=1 for L2.
    static const char* script = std::getenv("KOL_TILE_EDIT_SCRIPT");
    if (!script) return;
    static std::vector<std::pair<unsigned long, unsigned>> steps;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        for (const char* p = script; *p;) {
            unsigned long f = 0;
            unsigned k = 0;
            if (std::sscanf(p, "%lu:%x", &f, &k) == 2) steps.push_back({f, k});
            const char* comma = std::strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
    }
    for (const auto& [f, k] : steps)
        if (f == frame) filter(static_cast<std::uint16_t>(k));
}

void tile_edit_install() {
    if constexpr (!kol::kDevTools) return;   // a public build: no editor
    static bool done = false;
    if (done) return;
    done = true;
    g_prev_filter = gbarecomp::g_host_keyinput_filter;
    gbarecomp::g_host_keyinput_filter = &filter;
}

void tile_edit_set_target(int map, int cell, int drawn) {
    g_target_map.store(map);
    g_target_cell.store(cell);
    g_target_drawn.store(drawn);
}

bool tile_edit_heights(int map, int* out) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_edits.find(map);
    if (it == g_edits.end()) return false;
    bool any = false;
    for (int c = 0; c < kSide * kSide; ++c) {
        out[c] = it->second[c];
        any = any || out[c] >= 0;
    }
    return any;
}

std::vector<TileTyped> tile_edit_types(int map) {
    std::vector<TileTyped> out;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_types.find(map);
    if (it == g_types.end()) return out;
    for (int c = 0; c < kSide * kSide; ++c)
        if (it->second[c] > kTypeSquare && it->second[c] < kTileTypes) out.push_back({tile_x(c), tile_z(c), it->second[c]});
    return out;
}

int tile_edit_type(int map, int cell) {
    if (cell < 0 || cell >= kSide * kSide) return kTypeSquare;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_types.find(map);
    return it == g_types.end() ? kTypeSquare : it->second[cell];
}

const char* tile_type_name(int type) {
    static const char* const kNames[kTileTypes] = {"SQUARE", "ROUND LEFT", "ROUND FRONT", "ROUND RIGHT", "ROUND BACK"};
    return type >= 0 && type < kTileTypes ? kNames[type] : "?";
}

int tile_edit_get(int map, int cell) {
    if (cell < 0 || cell >= kSide * kSide) return -1;
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_edits.find(map);
    return it == g_edits.end() ? -1 : it->second[cell];
}

std::vector<TileRoof> tile_edit_roofs(int map) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_roofs.find(map);
    return it == g_roofs.end() ? std::vector<TileRoof>{} : it->second;
}

std::vector<TileBox> tile_edit_boxes(int map) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_boxes.find(map);
    return it == g_boxes.end() ? std::vector<TileBox>{} : it->second;
}

int tile_edit_field() { return g_field.load(); }
int tile_edit_terrain_field() { return g_terrain_field.load(); }

bool tile_edit_box_focus() {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    return focused_box_locked() >= 0;
}

int tile_edit_box_field() { return g_box_field.load(); }

namespace {
std::mutex g_grid_mu;
TileGrid g_grid;
std::chrono::steady_clock::time_point g_grid_at;
std::atomic<int> g_pick{-1}, g_pick_base{-2};
}  // namespace

void tile_edit_publish(int map, const int* drawn, const unsigned char* flags, int target) {
    std::lock_guard<std::mutex> lock(g_grid_mu);
    g_grid.map = map;
    g_grid.target = target;
    std::copy_n(drawn, kSide * kSide, g_grid.drawn);
    std::copy_n(flags, kSide * kSide, g_grid.flags);
    g_grid_at = std::chrono::steady_clock::now();
}

bool tile_edit_grid(TileGrid* out) {
    std::lock_guard<std::mutex> lock(g_grid_mu);
    if (g_grid.map < 0 || std::chrono::steady_clock::now() - g_grid_at > std::chrono::seconds(1)) return false;
    *out = g_grid;
    return true;
}

void tile_edit_pick(int cell) {
    g_pick_base = -2;   // the game's cell, learned at the next frame
    g_pick = cell;
}

int tile_edit_resolve_target(int game_cell) {
    const int pick = g_pick.load();
    if (pick < 0) return game_cell;
    int base = g_pick_base.load();
    if (base == -2) {
        g_pick_base.compare_exchange_strong(base, game_cell);
        return pick;
    }
    if (game_cell != base) {   // the game's cursor moved: it leads again
        g_pick = -1;
        return game_cell;
    }
    return pick;
}

int tile_edit_mode() {
    if (g_water_focus.load()) return 4;
    if (g_sun_focus.load()) return 3;
    if (g_box_focus.load()) return 2;
    return g_field.load() != 0 ? 1 : 0;
}

void tile_edit_set_mode(int mode) {
    g_water_focus = mode == 4;
    g_sun_focus = mode == 3;
    g_box_focus = mode == 2;
    if (mode == 0) g_field = 0;
    if (mode == 1 && g_field.load() == 0) g_field = 2;   // the ridge
}

TileField tile_edit_field_now() {
    TileField f{};
    const int map = g_target_map.load(), cell = g_target_cell.load();
    auto halves = [](int v, char* out, std::size_t n) { std::snprintf(out, n, "%d%s", v / 2, v % 2 ? ".5" : ""); };
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const int mode = tile_edit_mode();
    if (mode == 4) {
        std::snprintf(f.name, sizeof f.name, "BRUSH");
        std::snprintf(f.value, sizeof f.value, "%s", tile_water_name(g_water_brush.load()));
        f.live = true;
        return f;
    }
    if (mode == 3) {
        // The sun now (with L2 Moves Sun, where the stick put it).
        float deg = 0.0f, x = 0.0f, z = 0.0f;
        std::snprintf(f.name, sizeof f.name, "SUN");
        if (sun_now(&deg, &x, &z)) std::snprintf(f.value, sizeof f.value, "%.1f  %.2f,%.2f", deg, x, z);
        else std::snprintf(f.value, sizeof f.value, "SHADOWS OFF");
        return f;   // not live: the stick moves it (L2)
    }
    if (mode == 2) {
        static const char* const kNames[kBoxFields] = {"TOP", "PEAK", "SIZE X", "SIZE Z", "SHIFT X", "SHIFT Z", "AXIS"};
        const int k = g_box_field.load() % kBoxFields;
        std::snprintf(f.name, sizeof f.name, "%s", kNames[k]);
        const int bi = focused_box_locked();
        if (bi < 0) { std::snprintf(f.value, sizeof f.value, "NO BOX"); return f; }
        const TileBox& b = g_boxes[map][bi];
        switch (k) {
            case 0: halves(b.top2, f.value, sizeof f.value); break;
            case 1: halves(std::max(b.top2, b.peak2), f.value, sizeof f.value); break;
            case 2: std::snprintf(f.value, sizeof f.value, "%d/8", b.size_x8); break;
            case 3: std::snprintf(f.value, sizeof f.value, "%d/8", b.size_z8); break;
            case 4: std::snprintf(f.value, sizeof f.value, "%d/8", b.dx8); break;
            case 5: std::snprintf(f.value, sizeof f.value, "%d/8", b.dz8); break;
            default: std::snprintf(f.value, sizeof f.value, "%c", b.along_x ? 'X' : 'Z'); break;
        }
        f.live = true;
        return f;
    }
    if (mode == 1) {
        static const char* const kNames[kFields] = {"HEIGHT", "EAVE", "RIDGE", "GABLES", "AXIS",
                                                    "SHAPE", "OPEN", "SHIFT X", "SHIFT Z"};
        const int k = std::max(1, g_field.load() % kFields);
        std::snprintf(f.name, sizeof f.name, "%s", kNames[k]);
        const int ri = roof_at_locked(map, cell);
        if (ri < 0) { std::snprintf(f.value, sizeof f.value, "NO ROOF"); return f; }
        const TileRoof& r = g_roofs[map][ri];
        static const char* const kShapeNames[kShapes] = {"GABLE", "SLOPE +", "SLOPE -", "PEAK FRONT", "PEAK RIGHT",
                                                         "PEAK BACK", "PEAK LEFT", "LOW FRONT", "LOW RIGHT",
                                                         "LOW BACK", "LOW LEFT", "PYRAMID"};
        switch (k) {
            case 1: halves(r.eave2, f.value, sizeof f.value); break;
            case 2: halves(r.ridge2, f.value, sizeof f.value); break;
            case 3: std::snprintf(f.value, sizeof f.value, "%d", r.gables); break;
            case 4: std::snprintf(f.value, sizeof f.value, "%c", r.along_x ? 'X' : 'Z'); break;
            case 5: std::snprintf(f.value, sizeof f.value, "%s", kShapeNames[std::clamp(r.shape, 0, kShapes - 1)]); break;
            case 6: std::snprintf(f.value, sizeof f.value, "%s", r.open ? "YES (AWNING)" : "NO"); break;
            case 7: std::snprintf(f.value, sizeof f.value, "%d/8", r.dx8); break;
            default: std::snprintf(f.value, sizeof f.value, "%d/8", r.dz8); break;
        }
        f.live = true;
        return f;
    }
    if (g_terrain_field.load() == 1) {
        std::snprintf(f.name, sizeof f.name, "TYPE");
        int t = kTypeSquare;
        if (const auto it = g_types.find(map); it != g_types.end() && cell >= 0 && cell < kSide * kSide)
            t = it->second[cell];
        std::snprintf(f.value, sizeof f.value, "%s", tile_type_name(t));
    } else {
        std::snprintf(f.name, sizeof f.name, "HEIGHT");
        std::snprintf(f.value, sizeof f.value, "%d", g_target_drawn.load());
    }
    f.live = cell >= 0;
    return f;
}

void tile_edit_step_field(int dir) {
    const int mode = tile_edit_mode();
    if (mode == 4) {   // the brush: water, dry, ice, the game's
        g_water_brush = (g_water_brush.load() + (dir > 0 ? 1 : kWaterTags - 1)) % kWaterTags;
    } else if (mode == 2) {
        g_box_field = (g_box_field.load() % kBoxFields + (dir > 0 ? 1 : kBoxFields - 1)) % kBoxFields;
    } else if (mode == 0) {   // the terrain's height and type
        g_terrain_field = 1 - g_terrain_field.load();
    } else if (mode == 1) {   // the roof's eight, the height being its own tab
        const int k = std::max(1, g_field.load() % kFields) - 1;
        g_field = 1 + (k + (dir > 0 ? 1 : kFields - 2)) % (kFields - 1);
    }
}

void tile_edit_change_now(int delta) {
    if (tile_edit_mode() == 4) { tile_edit_step_field(delta); return; }
    if (!tile_edit_field_now().live) return;
    change(delta);
}

void tile_edit_delete_now() {
    const int mode = tile_edit_mode();
    const int map = g_target_map.load(), cell = g_target_cell.load();
    if (mode == 1) {
        // The roof only (reset() would put the height back without one).
        std::lock_guard<std::mutex> lock(g_mu);
        load_locked();
        const int ri = roof_at_locked(map, cell);
        if (ri < 0) return;
        g_roofs[map].erase(g_roofs[map].begin() + ri);
        save_locked();
        std::fprintf(stderr, "tile edit: map %d roof deleted\n", map);
        return;
    }
    if (mode == 0) {
        // The terrain only (reset() would take a roof over the cell first).
        if (cell < 0 || cell >= kSide * kSide) return;
        std::lock_guard<std::mutex> lock(g_mu);
        load_locked();
        reset_terrain_locked(map, cell);
        save_locked();
        return;
    }
    if (!tile_edit_box_focus()) return;
    reset();                 // the focused box
    g_box_focus = true;      // stays on the box tab
}

void tile_edit_add_box_now() {
    const int map = g_target_map.load(), cell = g_target_cell.load();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        load_locked();
        if (box_at_locked(map, cell) >= 0) { g_box_focus = true; return; }
    }
    box();
}

bool tile_edit_sun(int map, TileSun* out) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_suns.find(map);
    if (it == g_suns.end()) return false;
    *out = it->second;
    return true;
}

void tile_edit_set_sun(int map, const TileSun& sun) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    g_suns[map] = sun;
    save_locked();
    // In kMapSuns' form, to copy into the code.
    std::fprintf(stderr, "tile edit: map %d sun %.1f degrees toward %.3f,%.3f: kMapSuns {%d, %.1ff, %.3ff, %.3ff}\n",
                 map, sun.degrees, sun.x, sun.z, map, sun.degrees, sun.x, sun.z);
}

bool tile_edit_indoor(int map) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    return g_indoor.count(map) != 0;
}

void tile_edit_set_indoor(int map, bool indoor) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (indoor) g_indoor.insert(map); else g_indoor.erase(map);
    save_locked();
    std::fprintf(stderr, "tile edit: map %d %s: map_settings.cfg indoor %d\n", map, indoor ? "indoors" : "outdoors", map);
}

bool tile_edit_clear_sun(int map) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (!g_suns.erase(map)) return false;
    save_locked();
    std::fprintf(stderr, "tile edit: map %d sun cleared\n", map);
    return true;
}

// Water the build knows of, where the edits say nothing (auto): cells drawn at the
// water's height under a model, whose records are not water's. Map 21's hollow under
// the land bridge (records 0x10-0x13 and 0x00): the water effect stopped at the arch's
// mouth, the water under it brighter than the shaded water in front (the user still saw
// a brightness step between the arch's mouth and the rest).
struct BuiltWater { int map, x0, z0, x1, z1; };
constexpr BuiltWater kBuiltWater[] = {{21, 5, 2, 8, 2}, {21, 6, 1, 7, 1}, {22, 5, 2, 8, 2}, {22, 6, 1, 7, 1}, {23, 5, 2, 8, 2}, {23, 6, 1, 7, 1}, {24, 5, 2, 8, 2}, {24, 6, 1, 7, 1}, {25, 5, 2, 8, 2}, {25, 6, 1, 7, 1}, {114, 5, 2, 8, 2}, {114, 6, 1, 7, 1}};

bool tile_edit_water(int map, signed char* out) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    const auto it = g_water.find(map);
    bool any = false;
    for (int c = 0; c < kSide * kSide; ++c) {
        out[c] = it != g_water.end() ? it->second[c] : static_cast<signed char>(kWaterAuto);
        any = any || out[c] != kWaterAuto;
    }
    for (const BuiltWater& b : kBuiltWater)
        if (b.map == map)
            for (int z = b.z0; z <= b.z1; ++z)
                for (int x = b.x0; x <= b.x1; ++x)
                    if (out[tile_cell(x, z)] == kWaterAuto) {
                        out[tile_cell(x, z)] = kWaterYes;
                        any = true;
                    }
    return any;
}

void tile_edit_paint_water(int map, int cell0, int cell1, int tag) {
    if (cell0 < 0 || cell1 < 0 || cell0 >= kSide * kSide || cell1 >= kSide * kSide) return;
    if (tag < 0 || tag >= kWaterTags) return;
    const int x0 = std::min(tile_x(cell0), tile_x(cell1)), x1 = std::max(tile_x(cell0), tile_x(cell1));
    const int z0 = std::min(tile_z(cell0), tile_z(cell1)), z1 = std::max(tile_z(cell0), tile_z(cell1));
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    auto& v = g_water[map];
    if (v.empty()) v.assign(kSide * kSide, kWaterAuto);
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) v[tile_cell(x, z)] = static_cast<signed char>(tag);
    save_locked();
    std::fprintf(stderr, "tile edit: map %d water %d,%d-%d,%d %s\n", map, x0, z0, x1, z1, kWaterWords[tag]);
}

int tile_edit_water_brush() { return g_water_brush.load(); }

const char* tile_water_name(int tag) {
    static const char* const kNames[kWaterTags] = {"AUTO", "WATER", "DRY", "ICE"};
    return tag >= 0 && tag < kWaterTags ? kNames[tag] : "?";
}

bool tile_edit_corner(int map, int* cell) {
    if (g_corner_cell.load() < 0 || g_corner_map.load() != map) return false;
    *cell = g_corner_cell.load();
    return true;
}

}  // namespace kol
