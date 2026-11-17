// The second screen's editor page (see kol_edit_panel.h).
#include "kol_edit_panel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "kol_actions.h"
#include "kol_focus.h"
#include "kol_menu.h"
#include "kol_settings.h"
#include "kol_sheet.h"
#include "kol_tile_edit.h"

namespace kol {
namespace {

constexpr int kW = kSheetW, kH = kSheetH;   // 248 x 184
constexpr int kSide = kTileSide;   // the field's cells along X and Z
// The map: rows 0..kMapH-1, the target's cell in the middle at (kMapCx,
// kMapCy), each cell a diamond kStep * 2 wide and kStep tall (one step of X
// moves right kStep and down kStep / 2, as the game's 16 and 8).
constexpr int kMapH = 112;
constexpr float kMapCx = 124.0f, kMapCy = 56.0f, kStep = 5.0f;

// The buttons, in tile cells (8 px): three rows of windows under the map.
enum Button {
    kTabHeight, kTabRoof, kTabBox, kTabSun, kTabWater,
    kPrev, kField, kNext, kMinus, kPlus,
    kAction1, kAction2,
    kAction3,   // the SUN tab's INDOOR / OUTDOOR, where the other tabs' hint is
    kButtons
};
struct Rect { int c0, r0, c1, r1; };
constexpr Rect kRects[kButtons] = {
    {0, 14, 6, 16}, {7, 14, 10, 16}, {11, 14, 13, 16}, {14, 14, 16, 16}, {17, 14, 21, 16},
    {0, 17, 2, 19}, {3, 17, 19, 19}, {20, 17, 22, 19}, {23, 17, 26, 19}, {27, 17, 30, 19},
    {0, 20, 9, 22}, {10, 20, 19, 22},
    {20, 20, 30, 22},
};

std::mutex g_mu;
// The map's middle cell: it follows the target only when the target nears
// the map's edge, so a tap does not slide the map under the finger.
int g_cx = 0, g_cz = 0;
bool g_map_set = false;   // g_cx, g_cz hold a middle (cells may be negative)
int g_map = -1;
// A finger: on the map (the cell it went down on, a drag's far cell) or on a
// button (pressed, while it stays on it).
int g_down_button = -1;
bool g_down_inside = false;
int g_down_cell = -1, g_drag_cell = -1;
bool g_dragging = false;

// The cell under canvas pixel (x, y) of the map, -1 off the grid.
int cell_at(float x, float y, int cx, int cz, float* fx = nullptr, float* fz = nullptr) {
    const float u = (x - kMapCx) / kStep, v = (y - kMapCy) / (kStep * 0.5f);
    const float dx = (u + v) * 0.5f + 0.5f, dz = (v - u) * 0.5f + 0.5f;
    const float flx = std::floor(dx), flz = std::floor(dz);
    if (fx) *fx = dx - flx;
    if (fz) *fz = dz - flz;
    const int x_ = cx + static_cast<int>(flx), z_ = cz + static_cast<int>(flz);
    if (!tile_in_field(x_, z_)) return -1;
    return tile_cell(x_, z_);
}

// Where cell (x, z)'s middle lands on the map.
void cell_xy(int x, int z, int cx, int cz, float* px, float* py) {
    *px = kMapCx + ((x - cx) - (z - cz)) * kStep;
    *py = kMapCy + ((x - cx) + (z - cz)) * kStep * 0.5f;
}

std::uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
           static_cast<std::uint32_t>(b);
}
std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) {
    auto ch = [&](int s) {
        const float x = ((a >> s) & 0xFF) * (1.0f - t) + ((b >> s) & 0xFF) * t;
        return static_cast<std::uint32_t>(std::lround(x)) << s;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}
void darken(std::uint32_t* px, const Rect& r, float k) {
    for (int y = r.r0 * 8; y < (r.r1 + 1) * 8 && y < kH; ++y)
        for (int x = r.c0 * 8; x < (r.c1 + 1) * 8 && x < kW; ++x)
            px[y * kW + x] = mix(rgb(0, 0, 0), px[y * kW + x], k);
}

// A button: the game's window with its text centred.
void button(std::uint32_t* px, Button b, const char* text, bool live, bool pushed, bool dim = false) {
    const Rect& r = kRects[b];
    menu_draw_window(px, kW, kH, r.c0, r.r0, r.c1, r.r1);
    const int tw = menu_text_width(text);
    const int x = (r.c0 * 8 + (r.c1 + 1) * 8 - tw) / 2;
    menu_draw_text(px, kW, kH, text, x + (pushed ? 1 : 0), r.r0 * 8 + 4 + (pushed ? 1 : 0));
    if (pushed) darken(px, r, 0.75f);
    else if (!live) darken(px, r, 0.4f);
    else if (dim) darken(px, r, 0.6f);
}

}  // namespace

bool compose_edit_panel(std::uint32_t* px) {
    if (!setting_on(Setting::DebugTileInfo)) return false;
    static TileGrid grid;
    if (!tile_edit_grid(&grid)) return false;
    if (!menu_draw_window(px, kW, kH, 0, 0, 0, 0)) return false;   // ROM art not ready
    std::fill_n(px, kW * kH, rgb(0, 0, 0));
    std::lock_guard<std::mutex> lock(g_mu);

    // The map's middle: the target, once it nears the edge (or on another map).
    const int target = grid.target;
    if (grid.map != g_map) { g_map = grid.map; g_map_set = false; }
    if (target >= 0) {
        float tx = 0, ty = 0;
        if (g_map_set) cell_xy(tile_x(target), tile_z(target), g_cx, g_cz, &tx, &ty);
        if (!g_map_set || std::abs(tx - kMapCx) > 100.0f || std::abs(ty - kMapCy) > 44.0f) {
            g_cx = tile_x(target);
            g_cz = tile_z(target);
            g_map_set = true;
        }
    } else if (!g_map_set) {
        g_cx = g_cz = kTileGame / 2;
        g_map_set = true;
    }

    const std::vector<TileRoof> roofs = tile_edit_roofs(grid.map);
    const std::vector<TileBox> boxes = tile_edit_boxes(grid.map);
    static std::vector<int> edited_store(static_cast<std::size_t>(kSide) * kSide);
    int* const edited = edited_store.data();
    const bool any_edit = tile_edit_heights(grid.map, edited);
    std::vector<unsigned char> in_roof(kSide * kSide, 0), has_box(kSide * kSide, 0), typed(kSide * kSide, 0);
    for (const TileTyped& t : tile_edit_types(grid.map))
        if (tile_in_field(t.x, t.z)) typed[tile_cell(t.x, t.z)] = 1;
    for (const TileRoof& r : roofs)
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                if (tile_in_field(x, z)) in_roof[tile_cell(x, z)] = 1;
    for (const TileBox& b : boxes) has_box[tile_cell(b.x, b.z)] = 1;
    const int mode = tile_edit_mode();
    static std::vector<signed char> water_store(static_cast<std::size_t>(kSide) * kSide);
    signed char* const water = water_store.data();
    const bool any_water_tag = tile_edit_water(grid.map, water);
    // A roof being dragged out, or a corner waiting (L2 + B once).
    int rx0 = 0, rz0 = 0, rx1 = 0, rz1 = 0;
    bool has_rect = false;
    int corner = -1;
    if (g_dragging && g_down_cell >= 0 && g_drag_cell >= 0) {
        rx0 = std::min(tile_x(g_down_cell), tile_x(g_drag_cell));
        rx1 = std::max(tile_x(g_down_cell), tile_x(g_drag_cell));
        rz0 = std::min(tile_z(g_down_cell), tile_z(g_drag_cell));
        rz1 = std::max(tile_z(g_down_cell), tile_z(g_drag_cell));
        has_rect = true;
    } else if (mode == 1 && tile_edit_corner(grid.map, &corner)) {
        rx0 = rx1 = tile_x(corner);
        rz0 = rz1 = tile_z(corner);
        has_rect = true;
    }

    // The cells: low dark blue to high sand, off the field near black; a
    // roof's tinted red; the target's edge yellow, a roof being made cyan.
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kW; ++x) {
            float fx, fz;
            const int c = cell_at(x + 0.5f, y + 0.5f, g_cx, g_cz, &fx, &fz);
            if (c < 0) continue;
            std::uint32_t col;
            if (grid.flags[c] & kTileOffField && grid.drawn[c] <= 0) {
                col = rgb(22, 22, 28);
            } else {
                const float t = std::min(1.0f, std::max(0.0f, grid.drawn[c] / 20.0f));
                col = mix(rgb(34, 52, 86), rgb(236, 222, 168), t);
                if (!(grid.flags[c] & kTileWalkable)) col = mix(col, rgb(0, 0, 0), 0.25f);
            }
            if (mode == 4) {
                // WATER: the cells that are water blue (the game's paler,
                // a tag's deep), a dry tag brown, an ice tag icy white-cyan.
                const int tag = any_water_tag ? water[c] : kWaterAuto;
                if (tag == kWaterYes) col = mix(col, rgb(40, 110, 255), 0.7f);
                else if (tag == kWaterDry) col = mix(col, rgb(150, 96, 40), 0.6f);
                else if (tag == kWaterIce) col = mix(col, rgb(200, 245, 255), 0.75f);
                else if (grid.flags[c] & kTileWater) col = mix(col, rgb(90, 170, 255), 0.45f);
            } else if (in_roof[c]) {
                col = mix(col, rgb(200, 64, 52), 0.55f);
            }
            const int cx = tile_x(c), cz = tile_z(c);
            if (has_rect && cx >= rx0 && cx <= rx1 && cz >= rz0 && cz <= rz1) col = mix(col, rgb(64, 224, 255), 0.5f);
            const bool edge = fx < 0.1f || fz < 0.1f;
            if (edge) col = mix(col, rgb(0, 0, 0), 0.3f);
            if (c == target && (fx < 0.14f || fz < 0.14f || fx > 0.86f || fz > 0.86f)) col = rgb(255, 232, 64);
            px[y * kW + x] = col;
        }
    // Marks at the cells' middles: an edited height or type a white dot, a
    // box a dark block.
    for (int c = 0; c < kSide * kSide; ++c) {
        const bool ed = (any_edit && edited[c] >= 0) || typed[c];
        if (!ed && !has_box[c]) continue;
        float mx, my;
        cell_xy(tile_x(c), tile_z(c), g_cx, g_cz, &mx, &my);
        const int ix = static_cast<int>(std::floor(mx)), iy = static_cast<int>(std::floor(my));
        auto dot = [&](int x, int y, std::uint32_t v) {
            if (x >= 0 && y >= 0 && x < kW && y < kMapH) px[y * kW + x] = v;
        };
        if (has_box[c])
            for (int y = -1; y <= 1; ++y)
                for (int x = -2; x <= 1; ++x) dot(ix + x, iy + y, rgb(40, 30, 30));
        else {
            dot(ix, iy, rgb(255, 255, 255));
            dot(ix - 1, iy, rgb(255, 255, 255));
        }
    }

    // The controls.
    const bool on_cell = target >= 0;
    bool roof_here = false, box_here = false;
    if (on_cell) {
        roof_here = in_roof[target] != 0;
        box_here = has_box[target] != 0;
    }
    auto pushed = [&](Button b) { return g_down_button == b && g_down_inside; };
    button(px, kTabHeight, "TERRAIN", true, pushed(kTabHeight), mode != 0);
    button(px, kTabRoof, "ROOF", true, pushed(kTabRoof), mode != 1);
    button(px, kTabBox, "BOX", true, pushed(kTabBox), mode != 2);
    button(px, kTabSun, "SUN", true, pushed(kTabSun), mode != 3);
    button(px, kTabWater, "WATER", true, pushed(kTabWater), mode != 4);
    // The target, and the mode's hint, in windows of their own (the game's
    // ink is dark: on black it does not show).
    menu_draw_window(px, kW, kH, 22, 14, 30, 16);
    if (on_cell) {
        char info[48];
        std::snprintf(info, sizeof info, "%d,%d H%d", tile_x(target), tile_z(target), grid.drawn[target]);
        menu_draw_text(px, kW, kH, info, 22 * 8 + 6, 14 * 8 + 4);
    }
    auto hint = [&](const char* text) {
        menu_draw_window(px, kW, kH, 20, 20, 30, 22);
        menu_draw_text(px, kW, kH, text, 20 * 8 + 8, 20 * 8 + 4);
        darken(px, Rect{20, 20, 30, 22}, 0.7f);
    };
    const TileField f = tile_edit_field_now();
    const bool steps = mode != 3;   // the terrain has two fields too (height, type), water its brushes; the sun none
    button(px, kPrev, "<", steps, pushed(kPrev));
    char field[64];
    std::snprintf(field, sizeof field, "%s  %s", f.name, f.value);
    button(px, kField, field, f.live, false);
    button(px, kNext, ">", steps, pushed(kNext));
    button(px, kMinus, "-", f.live, pushed(kMinus));
    button(px, kPlus, "+", f.live, pushed(kPlus));
    if (mode == 0) {
        const bool edited_here = on_cell && ((any_edit && edited[target] >= 0) || typed[target]);
        button(px, kAction1, "RESET", edited_here, pushed(kAction1));
        hint("TAP A CELL");
    } else if (mode == 1) {
        button(px, kAction1, "DELETE", roof_here, pushed(kAction1));
        hint("DRAG: NEW");
    } else if (mode == 4) {
        // The brush on the target cell; a drag paints a rectangle.
        button(px, kAction1, "PAINT", on_cell, pushed(kAction1));
        hint("DRAG: PAINT");
    } else if (mode == 2) {
        button(px, kAction1, "ADD BOX", on_cell && !box_here, pushed(kAction1));
        button(px, kAction2, "DELETE", box_here, pushed(kAction2));
    } else {
        // The sun: L2 + the right stick moves it (Debug > L2 Moves Sun); SET
        // SUN keeps it for this map (drawn_heights.cfg), CLEAR drops that.
        float d, sx, sz;
        TileSun saved{};
        const bool have = sun_now(&d, &sx, &sz);
        const bool is_saved = tile_edit_sun(grid.map, &saved);
        button(px, kAction1, "SET SUN", have, pushed(kAction1));
        button(px, kAction2, "CLEAR", is_saved, pushed(kAction2));
        // The map indoors (no sun, kol_focus kIndoorMaps): a tap toggles it.
        button(px, kAction3, tile_edit_indoor(grid.map) ? "INDOOR" : "OUTDOOR", true, pushed(kAction3));
    }
    return true;
}

void edit_panel_touch(int action, float x, float y) {
    TileGrid grid;
    if (!tile_edit_grid(&grid)) return;
    std::unique_lock<std::mutex> lock(g_mu);
    auto button_at = [](float x, float y) {
        for (int b = 0; b < kButtons; ++b) {
            const Rect& r = kRects[b];
            if (x >= r.c0 * 8 && x < (r.c1 + 1) * 8 && y >= r.r0 * 8 && y < (r.r1 + 1) * 8) return b;
        }
        return -1;
    };
    if (action == 0) {
        g_dragging = false;
        g_down_button = -1;
        g_down_cell = -1;
        if (y < kMapH && g_map_set) {
            g_down_cell = cell_at(x, y, g_cx, g_cz);
            g_drag_cell = g_down_cell;
        } else {
            g_down_button = button_at(x, y);
            g_down_inside = g_down_button >= 0;
        }
        return;
    }
    if (action == 1) {
        if (g_down_button >= 0) g_down_inside = button_at(x, y) == g_down_button;
        if (g_down_cell >= 0 && y < kMapH) {
            const int c = cell_at(x, y, g_cx, g_cz);
            // In ROOF, across cells: a roof over the rectangle; in WATER,
            // the brush over it.
            const int m = tile_edit_mode();
            if (c >= 0 && c != g_down_cell && (m == 1 || m == 4)) g_dragging = true;
            if (c >= 0) g_drag_cell = c;
        }
        return;
    }
    const int down_button = g_down_button, down_cell = g_down_cell, drag_cell = g_drag_cell;
    const bool dragging = g_dragging, inside = g_down_inside;
    g_down_button = -1;
    g_down_cell = -1;
    g_dragging = false;
    lock.unlock();
    if (action != 2) return;   // cancelled
    if (down_cell >= 0) {
        if (dragging && drag_cell >= 0) {
            if (tile_edit_mode() == 4) tile_edit_paint_water(grid.map, down_cell, drag_cell, tile_edit_water_brush());
            else tile_edit_make_roof(grid.map, down_cell, drag_cell, 2 * grid.drawn[down_cell]);
            return;
        }
        // The target: in a story scene's viewer its tile cursor (the camera
        // follows), on a battle map until the game's cursor moves.
        if (debug_scene_viewer_active()) debug_viewer_cell_set(tile_x(down_cell), tile_z(down_cell));
        tile_edit_pick(down_cell);
        return;
    }
    if (down_button < 0 || !inside || button_at(x, y) != down_button) return;
    const int mode = tile_edit_mode();
    switch (down_button) {
        case kTabHeight: tile_edit_set_mode(0); break;
        case kTabRoof: tile_edit_set_mode(1); break;
        case kTabBox: tile_edit_set_mode(2); break;
        case kTabSun: tile_edit_set_mode(3); break;
        case kTabWater: tile_edit_set_mode(4); break;
        case kPrev: tile_edit_step_field(-1); break;
        case kNext: case kField: tile_edit_step_field(+1); break;
        case kMinus: tile_edit_change_now(-1); break;
        case kPlus: tile_edit_change_now(+1); break;
        case kAction1:
            if (mode == 4) {
                if (grid.target >= 0) tile_edit_paint_water(grid.map, grid.target, grid.target, tile_edit_water_brush());
            } else if (mode == 3) sun_save_request();
            else if (mode == 2) tile_edit_add_box_now();
            else tile_edit_delete_now();
            break;
        case kAction2:
            if (mode == 3) sun_clear_request();
            else if (mode == 2) tile_edit_delete_now();
            break;
        case kAction3:
            if (mode == 3) tile_edit_set_indoor(grid.map, !tile_edit_indoor(grid.map));
            break;
        default: break;
    }
}

void edit_panel_test_tick(unsigned long frame) {
    static const char* touches = std::getenv("KOL_EDIT_PANEL_TOUCH");
    if (touches) {
        for (const char* p = touches; *p;) {
            unsigned long f = 0;
            int a = 0;
            float x = 0, y = 0;
            if (std::sscanf(p, "%lu:%d:%f:%f", &f, &a, &x, &y) == 4 && f == frame) edit_panel_touch(a, x, y);
            const char* comma = std::strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
    }
    static const char* ppm = std::getenv("KOL_EDIT_PANEL_PPM");
    if (!ppm) return;
    std::string path = ppm;
    unsigned long at = 0;
    if (const char* comma = std::strchr(ppm, ',')) {
        path.assign(ppm, comma);
        at = std::strtoul(comma + 1, nullptr, 10);
    }
    if (at && frame != at) return;
    static std::vector<std::uint32_t> img(kW * kH);
    if (!compose_edit_panel(img.data())) return;
    std::string out = path;
    if (const std::size_t pct = out.find("%lu"); pct != std::string::npos)
        out.replace(pct, 3, std::to_string(frame));
    if (std::FILE* f = std::fopen(out.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
        for (std::uint32_t v : img) {
            const unsigned char c[3] = {static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8),
                                        static_cast<unsigned char>(v)};
            std::fwrite(c, 1, 3, f);
        }
        std::fclose(f);
    }
}

}  // namespace kol
