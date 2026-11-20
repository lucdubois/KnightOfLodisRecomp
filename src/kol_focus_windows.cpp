// kol_focus_windows.cpp — light through a map's windows (kMapWindows): the
// beams and their haze (godrays), the cones and panes, the second set's
// moonlight, a torch out of view, the window light on units and lifted
// pieces, and map 71's shutter.

#include "kol_focus.h"
#include "kol_focus_internal.h"

#include <atomic>
#include <deque>
#include <functional>

#include "kol_workers.h"

namespace {
float map_window_haze(int map) {
    const MapWindowTint* t = by_map(kMapWindowTints, map);
    return t ? t->haze : 0.05f;
}
}  // namespace

namespace kol_focus_detail {
const float* map_window_tint(int map) {
    static constexpr float kSun[3] = {1.0f, 0.78f, 0.48f};
    const MapWindowTint* t = by_map(kMapWindowTints, map);
    return t ? &t->r : kSun;
}
}  // namespace kol_focus_detail

namespace {
bool in_win_set(const MapWindow& W) { return (W.own_deg > 0.0f) == (g_win_set == 1); }
}  // namespace

namespace kol_focus_detail {
// kMapWindows and KOL_MAP_WINDOWS=<map,plane,x0,x1,y0,y1,on_x,spread,gain[,panes_x,panes_y[,own_deg,own_x,own_z,own_gain]]>[;...]
// (mockups).
const std::vector<MapWindow>& map_windows_all() {
    static const std::vector<MapWindow> all = [] {
        std::vector<MapWindow> v(std::begin(kMapWindows), std::end(kMapWindows));
        kol::env_items("KOL_MAP_WINDOWS", [&](const char* p) {
            MapWindow W{};
            int on_x = 0;
            if (std::sscanf(p, "%d,%f,%f,%f,%f,%f,%d,%f,%f,%d,%d,%f,%f,%f,%f", &W.map, &W.plane_z, &W.x0, &W.x1, &W.y0, &W.y1, &on_x, &W.spread,
                            &W.gain, &W.panes_x, &W.panes_y, &W.own_deg, &W.own_x, &W.own_z, &W.own_gain) >= 9) {
                W.on_x = on_x != 0;
                v.push_back(W);
            }
        });
        return v;
    }();
    return all;
}
}  // namespace kol_focus_detail

namespace {
// A window's point (a along its span, on its plane) in the world: X and Z.
inline float win_X(const MapWindow& W, float a) { return W.on_x ? W.plane_z : a; }
inline float win_Z(const MapWindow& W, float a) { return W.on_x ? a : W.plane_z; }
}  // namespace

namespace kol_focus_detail {
// With Options > Shadows on, at the user's pick: 1.0 (C in
// shots/godrays/sheet_tame_warm.png, a little tamer; first 1.5 from
// sheet_suns_off_on.png), haze 0.05. KOL_GODRAYS=<strength> overrides it,
// 0 off.
float godray_strength() {
    static const float env = kol::env_num_min("KOL_GODRAYS", 0.0f, -1.0f);   // -1: not set
    if (env >= 0.0f) return env;
    if (kol::shadow_sun_degrees() <= 0.0f) return 0.0f;
    // A map's own (kGodrayStrengths).
    const int map = current_map();
    for (const GodrayStrength& g : kGodrayStrengths)
        if (g.map == map) return g.strength;
    return 1.0f;
}
const WindowTorch* window_torch() {
    static const bool off = std::getenv("KOL_NO_WINDOW_TORCH") != nullptr;
    if (off) return nullptr;
    const int map = current_map();
    static WindowTorch env{};
    static const bool env_on = [] {
        const char* e = std::getenv("KOL_WINDOW_TORCH");
        return e && std::sscanf(e, "%f,%f,%f,%d", &env.yaw, &env.pitch, &env.flicker, &env.step) == 4;
    }();
    if (env_on) {
        env.map = map;
        return &env;
    }
    return by_map(kWindowTorches, map);
}
}  // namespace kol_focus_detail

namespace {
// The torch's sway now, -1..1 each: along the ground, up and down, and its
// strength's flicker. Slow swells and a quicker shiver, stepped.
// Set while the beams of a torch-lit map are built (build_window_light):
// they take the torch at rest.
thread_local bool t_torch_at_rest = false;
}  // namespace

namespace kol_focus_detail {
void window_torch_sway(const WindowTorch& w, float& a, float& b, float& f) {
    if (t_torch_at_rest) {
        a = b = f = 0.0f;
        return;
    }
    const std::uint32_t serial = t_present ? g_cap_serial() : static_cast<std::uint32_t>(g_frame_serial);
    const std::uint32_t step = static_cast<std::uint32_t>(std::max(1, w.step));
    const float t = static_cast<float>(serial / step * step);
    a = 0.6f * std::sin(t * 0.047f) + 0.4f * std::sin(t * 0.131f + 1.3f);
    b = 0.6f * std::sin(t * 0.061f + 2.1f) + 0.4f * std::sin(t * 0.173f + 0.4f);
    // The flicker in the fire pots' rhythm (their 0.05 sin 0.9t + 0.03 sin
    // (2.3t + 0.6), scaled to -1..1).
    f = 0.625f * std::sin(t * 0.9f) + 0.375f * std::sin(t * 2.3f + 0.6f);
}
bool map_has_windows() {
    const int map = current_map();
    for (const MapWindow& W : map_windows_all())
        if (W.map == map) return true;
    return false;
}
// A room lit through its windows with Options > Shadows off: no window light
// (it is the sun's), so none of the rest either: the game's own lighting,
// not its darkness and lamps without the window that lights it (the user,
// 2026-11-03, asked for these rooms to keep the original game's lighting
// with shadows off). KOL_WINDOW_ROOMS_LIT=1: as before
// (dark, its lamps on).
bool window_room_plain() {
    static const bool lit = std::getenv("KOL_WINDOW_ROOMS_LIT") != nullptr;
    return !lit && kol::shadow_sun_degrees() <= 0.0f && map_has_windows();
}
}  // namespace kol_focus_detail

namespace {
// Whether the map's windows let their light in as a cone (window_cone_spread):
// then units cast shadows from it (cast_window_shadows) and lose the game's
// blob. Map 62 only; the castles' (55, 101) are as they were.
float window_cone_spread(const MapWindow& W);
}  // namespace

namespace kol_focus_detail {
// A paned window's too (map 65's moonlight; the user wanted the godrays to
// cast sprite shadows, e.g. Alphonse in front of the window).
bool map_cone_windows() {
    const int map = current_map();
    for (const MapWindow& W : map_windows_all())
        if (W.map == map && W.own_deg <= 0.0f && (window_cone_spread(W) > 0.0f || W.panes_x > 0 || W.panes_y > 0))
            return true;
    return false;
}
// And a torch's out of view (map 67's corridor; the user expected the
// units to drop their black blob and take a shadow cast by the
// light).
bool units_cast_from_windows() {
    static const bool off = std::getenv("KOL_NO_WINDOW_SHADOWS") != nullptr;
    return !off && indoor_now() && unit_shadows_on() && godray_strength() > 0.0f &&
           (map_cone_windows() || (window_torch() && map_has_windows()) ||
            (kol::map_flag(kol::MapFlag::WindowUnitShadows, current_map()) && map_has_windows()));
}
// A dark map with windows (55, the castle; asked 2026-10-15 for shade as on
// the cave map, the light coming from the godray): indoors, so
// no sun shadows, but the sun still comes in through its windows, its patches
// lift the darkness and its beams light the hall around them (the glow).
bool dark_windows() { return indoor_now() && dark_map() && map_has_windows(); }
}  // namespace kol_focus_detail

namespace {
// The light the beams radiate on a dark map: the beam cells blurred over
// about KOL_WIN_GLOW_R world units (box passes), times KOL_WIN_GLOW.
float window_glow_reach() {
    static const float v = kol::env_num_min("KOL_WIN_GLOW_R", 4.0f, 48.0f);
    return v;
}
float window_glow_gain() {
    static const float v = [] {
        const char* e = std::getenv("KOL_WIN_GLOW");
        return e ? std::max(0.0f, static_cast<float>(std::atof(e))) : 8.0f;   // the user's pick C (glow_variants.png)
    }();
    return v;
}
bool g_beams_bars = true;   // the beams cut by g_plane_mask (build_window_light)
constexpr float kBeamCell = 4.0f;
constexpr int kConeShaftCount = 9;
constexpr float kConeVolume = 0.25f;
// Cones whose shafts start as the window (map 71; the user asked for the
// cone's tip shaped like the window): their nine shafts start on a 3 x 3
// grid over the opening, each as thick as its share of it, so at the
// window they fill its rectangle; elsewhere they start as thin lines across
// its width, meeting near the window in a point. (The volume alone, whole
// and unbanded, was tried: a wash through the whole room, no cone.)
// (map_settings.cfg `cone_window_tip`.)
bool cone_window_tip(const MapWindow& W) { return kol::map_flag(kol::MapFlag::ConeWindowTip, W.map); }
// The window's opening: how far inside it the point (xc, yc) of its plane
// lies, in units (negative outside), the top rounded.
// A paned window (panes_x, panes_y): square, and outside where a bar is.
float window_inside(const MapWindow& W, float xc, float yc) {
    const float r = 0.5f * (W.x1 - W.x0), xm = 0.5f * (W.x0 + W.x1), yr = W.y1 - r;
    float in = std::min({xc - W.x0, W.x1 - xc, yc - W.y0});
    if (W.panes_x > 0 || W.panes_y > 0) {
        in = std::min(in, W.y1 - yc);
        auto bars = [&](float v, float v0, float v1, int panes) {
            const float pane = (v1 - v0) / static_cast<float>(panes);
            for (int i = 1; i < panes; ++i)
                in = std::min(in, std::fabs(v - (v0 + pane * static_cast<float>(i))) - 0.5f * kPaneBar);
        };
        if (W.panes_x > 1) bars(xc, W.x0, W.x1, W.panes_x);
        if (W.panes_y > 1) bars(yc, W.y0, W.y1, W.panes_y);
        return in;
    }
    if (yc > yr) in = std::min(in, r - std::hypot(xc - xm, yc - yr));
    return in;
}
// How much of the sun reaches a point whose ray toward it crosses the
// window's plane at (along, y), t along the ray (0..1). A plain window: the
// opening with a soft edge 1.5 + 0.01 t wide (as it always was). A window
// with a spread: the light is a cone, the opening grown by spread x t
// with rounded corners (the rectangle's distance, no arch) and its edge
// softer by kConeSoft x that, so near the window the patch is the
// window's shape and further in a widening pool. A cone's light is
// kConeGain strong, fading over kConeFade (window_sun_at). Map 62's, the
// user's picks from the mockups (2026-10-29: more light and shadow).
constexpr float kConeSoft = 0.6f, kConeGain = 2.5f, kConeFade = 250.0f;
constexpr float kConeSoftR = 5.0f;          // the spots of the opening a cone's light is tested to, this far apart
// The beams' grid around a cone: sampled this many times its spread past
// the opening (2.5: the reach the user approved; the grid's bounds also
// bound the light on surfaces, and a smaller one cut the pool short), every
// kConeStep units (a faint fill: 2 took ~0.4 s to build, at each new sun).
constexpr float kConeReach = 2.5f, kConeStep = 4.0f;
constexpr float kWindowWallDepth = 5.0f;    // a window's own wall (the model's 4-unit cubes), not tested
constexpr float kConeShafts = 3.0f, kConeShaftFloor = 0.25f;   // the haze's shafts across a cone, and the light between them
constexpr float kConeShaftHaze = 1.0f, kConeShaftLight = 0.3f;  // the shafts' haze in the air, and the light they lay
float window_cone_spread(const MapWindow& W) { return W.map == 71 ? W.spread * g_shutter_spread : W.spread; }
// What of a ray from (X, Y, Z) along (dx, dy, dz), up to tmax, passes the
// object standing in its plane on the window's map (g_plane_mask: map 67's
// door, its bars): 1 unless it crosses that plane, then 1 less the cover
// where it does.
// The mask in a window's way on `map`: the scene object's (map 67's door,
// g_plane_mask), else the map's boards (kWindowBoards), built once.
const PlaneMask& plane_mask_for(int map) {
    if (g_plane_mask.map == map && !g_plane_mask.m.empty()) return g_plane_mask;
    static std::deque<PlaneMask> boards;   // one a map that has them (a deque: references stay)
    for (const PlaneMask& pm : boards)
        if (pm.map == map) return pm;
    PlaneMask pm;
    float z0 = 1e9f, z1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const WindowBoard& b : kWindowBoards)
        if (b.map == map) {
            pm.plane = b.plane;
            z0 = std::min(z0, b.z0); z1 = std::max(z1, b.z1); y0 = std::min(y0, b.y0); y1 = std::max(y1, b.y1);
        }
    if (z1 < z0) {
        static const PlaneMask none;
        return none;
    }
    pm.map = map;
    pm.z0 = static_cast<int>(std::floor(z0)) - 2;
    pm.y0 = static_cast<int>(std::floor(y0)) - 2;
    pm.nz = static_cast<int>(std::ceil(z1)) + 3 - pm.z0;
    pm.ny = static_cast<int>(std::ceil(y1)) + 3 - pm.y0;
    pm.m.assign(static_cast<std::size_t>(pm.nz) * pm.ny, 0);
    for (const WindowBoard& b : kWindowBoards)
        if (b.map == map)
            for (int j = 0; j < pm.ny; ++j)
                for (int i = 0; i < pm.nz; ++i) {
                    const float z = pm.z0 + i + 0.5f, y = pm.y0 + j + 0.5f;
                    if (z >= b.z0 && z <= b.z1 && y >= b.y0 && y <= b.y1) pm.m[static_cast<std::size_t>(j) * pm.nz + i] = 1;
                }
    std::uint32_t hsh = kol::kFnvBasis;
    for (std::uint8_t v : pm.m) kol::fnv_mix(hsh, v);
    pm.hash = (hsh ^ static_cast<std::uint32_t>(map * 131)) * 16777619u;
    boards.push_back(std::move(pm));
    return boards.back();
}
float plane_mask_pass(const MapWindow& W, float X, float Y, float Z, float dx, float dy, float dz, float tmax) {
    const PlaneMask& pm = plane_mask_for(W.map);
    if (pm.m.empty() || pm.map != W.map || std::fabs(dx) < 1e-4f) return 1.0f;
    const float tp = (pm.plane - X) / dx;
    if (tp <= 0.0f || tp > tmax) return 1.0f;
    return 1.0f - pm.cover_soft(Z + tp * dz, Y + tp * dy, plane_soft(tp * std::sqrt(dx * dx + dy * dy + dz * dz)));
}
float window_aperture(const MapWindow& W, float along, float y, float t) {
    const float spread = window_cone_spread(W);
    // Feathered (MapWindow::pool_soft): the opening's two sizes made one (a ray meets a
    // plane in the air at a shallow angle, so its height is far longer than what it
    // lights), rounded, eased; the light lands as a soft pool, not a cut-out patch.
    // KOL_WIN_POOL_SOFT=<units>: every such map's (mockups).
    static const float pool_env = kol::env_num("KOL_WIN_POOL_SOFT", -1.0f);
    const float pool = pool_env >= 0.0f ? pool_env : W.pool_soft;
    if (spread <= 0.0f && pool > 0.0f) {
        const float hw = 0.5f * (W.x1 - W.x0), hh = 0.5f * (W.y1 - W.y0);
        const float qx = std::fabs(along - 0.5f * (W.x0 + W.x1)) - hw;
        const float qy = (std::fabs(y - 0.5f * (W.y0 + W.y1)) - hh) * hw / hh;
        const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f);
        const float v = std::max(0.0f, std::min(1.0f, 0.5f - d / pool));
        return v * v * (3.0f - 2.0f * v);
    }
    if (spread <= 0.0f) {
        // A paned window's edge sharper (its bars are 3 wide; at 1.5 +
        // 0.01 t, 3.5 by the floor, the grid washed out).
        const bool paned = W.panes_x > 0 || W.panes_y > 0;
        const float soft = paned ? 0.6f + 0.004f * t : 1.5f + 0.01f * t;
        return std::max(0.0f, std::min(1.0f, window_inside(W, along, y) / soft + 0.5f));
    }
    const float grow = spread * t;
    const float qx = std::fabs(along - 0.5f * (W.x0 + W.x1)) - 0.5f * (W.x1 - W.x0);
    const float qy = std::fabs(y - 0.5f * (W.y0 + W.y1)) - 0.5f * (W.y1 - W.y0);
    const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f);
    const float soft = 1.5f + 0.01f * t + kConeSoft * grow;
    return std::max(0.0f, std::min(1.0f, (grow - d) / soft + 0.5f));
}
float godray_fade() {
    static const float v = kol::env_num("KOL_GODRAYS_FADE", 160.0f);
    return v;
}

void build_window_beams(const std::vector<MapWindow>& wins, float sun_x, float sun_z, float rise,
                        const CellHeights& cell_height) {
    WindowBeams& B = g_beams;
    B.lit = 0;
    g_shafts.clear();
    {
        const float L = std::sqrt(sun_x * sun_x + rise * rise + sun_z * sun_z);
        for (const MapWindow& W : wins) {
            const float sp = window_cone_spread(W);
            if (sp <= 0.0f || W.on_x || L <= 0.0f) continue;
            const float lx = -sun_x / L, ly = -rise / L, lz = -sun_z / L;   // the light's way
            const float fan = std::atan(sp);
            for (int i = 0; i < kConeShaftCount; ++i) {
                const float f = (i + 0.5f) / kConeShaftCount;
                std::uint32_t hsh = static_cast<std::uint32_t>(i * 2654435761u + 977u);
                auto rnd = [&]() { hsh ^= hsh << 13; hsh ^= hsh >> 17; hsh ^= hsh << 5; return (hsh & 0xFFFF) / 65535.0f; };
                const float th = fan * (2.0f * f - 1.0f) * 0.9f;            // across the fan (about the vertical)
                const float ph = fan * 0.35f * (2.0f * rnd() - 1.0f);        // a little up or down
                const float c = std::cos(th), sn = std::sin(th);
                float dx = lx * c - lz * sn, dz = lx * sn + lz * c, dy = ly + ph;
                const float n = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= n; dy /= n; dz /= n;
                ConeShaft S;
                S.ax = W.x0 + f * (W.x1 - W.x0);
                S.ay = W.y0 + (0.15f + 0.7f * rnd()) * (W.y1 - W.y0);
                const bool tip = cone_window_tip(W);
                if (tip) {   // a 3 x 3 grid over the opening
                    S.ax = W.x0 + ((i % 3) + 0.5f) / 3.0f * (W.x1 - W.x0);
                    S.ay = W.y0 + ((i / 3 % 3) + 0.5f) / 3.0f * (W.y1 - W.y0);
                }
                S.az = W.plane_z + 1.0f;
                S.dx = dx; S.dy = dy; S.dz = dz;
                S.len = dy < -1e-3f ? std::min(400.0f, (S.ay - 16.0f) / -dy) : 400.0f;   // to the floor
                S.fade = std::min(250.0f, S.len);
                // ... or to the first wall or piece of furniture in its way
                // (the room's model), or where it leaves the room's floor
                // (through the open front: it lit the air outside).
                for (float u = 2.0f; u < S.len; u += 2.0f) {
                    const float x = S.ax + u * dx, y = S.ay + u * dy, z = S.az + u * dz;
                    const int cx = static_cast<int>(std::floor(x / 32.0f)), cz = static_cast<int>(std::floor(z / 32.0f));
                    if (cell_height(cx, cz) <= 0 || (g_occ && u > kOccluderSkip && g_occ->solid_hard_at(x, y, z))) {
                        S.len = u;
                        break;
                    }
                }
                S.w0 = 0.5f * (W.x1 - W.x0) / kConeShaftCount + 0.5f;   // together they fill the window at its tip
                if (tip) S.w0 = 0.5f * std::max((W.x1 - W.x0), (W.y1 - W.y0)) / 3.0f;   // its share of the opening
                S.grow = 0.03f + 0.03f * sp;   // narrow: each a beam of its own (wider ran together into a glow)
                S.gain = W.gain * (0.6f + 0.4f * rnd());
                g_shafts.push_back(S);
            }
        }
    }
    B.lx0 = B.ly0 = B.lz0 = 1e9f;
    B.lx1 = B.ly1 = B.lz1 = -1e9f;
    B.ymax = 0.0f;
    // A cone's light reaches past its opening by up to its spread over the
    // whole ray (and its soft edge): the area sampled and the bounds grow so.
    auto cone_ext = [&](const MapWindow& W) {
        const float sp = window_cone_spread(W);
        return sp > 0.0f ? sp * (W.y1 / std::max(0.05f, rise)) * kConeReach + 4.0f : 0.0f;
    };
    for (const MapWindow& W : wins) B.ymax = std::max(B.ymax, W.y1 + cone_ext(W));
    float bx0 = 1e9f, bx1 = -1e9f, bz0 = 1e9f, bz1 = -1e9f;
    for (const MapWindow& W : wins)
        for (float x : {W.x0 - cone_ext(W), W.x1 + cone_ext(W)})
            for (float y : {W.y0, W.y1 + cone_ext(W)}) {
                const float t = y / std::max(0.05f, rise);
                for (float u : {0.0f, t}) {
                    const float X = win_X(W, x) - u * sun_x, Z = win_Z(W, x) - u * sun_z;
                    bx0 = std::min(bx0, X); bx1 = std::max(bx1, X);
                    bz0 = std::min(bz0, Z); bz1 = std::max(bz1, Z);
                }
            }
    // (Room around the beams for their glow to spread into.)
    const bool glow = dark_windows();
    const int pad = glow ? static_cast<int>(std::ceil(window_glow_reach() / kBeamCell)) : 0;
    B.gx0 = static_cast<int>(std::floor(bx0 / kBeamCell)) - 2 - pad;
    B.gz0 = static_cast<int>(std::floor(bz0 / kBeamCell)) - 2 - pad;
    B.gnx = std::min(1024, static_cast<int>(std::ceil(bx1 / kBeamCell)) + 3 + pad - B.gx0);
    B.gnz = std::min(1024, static_cast<int>(std::ceil(bz1 / kBeamCell)) + 3 + pad - B.gz0);
    B.gny = static_cast<int>(std::ceil(B.ymax / kBeamCell)) + 3 + pad;
    B.cell.assign(static_cast<std::size_t>(B.gnx) * B.gny * B.gnz, 0.0f);
    const float fade = godray_fade();
    // A step along a ray: 2 units of the world, whatever the sun's height.
    const float len = std::sqrt(sun_x * sun_x + rise * rise + sun_z * sun_z);
    const float dt = 2.0f / std::max(1e-3f, len);
    for (const MapWindow& W : wins) {
        const float kEdge = 4.0f + cone_ext(W);   // past the opening, for the soft edge (a cone's spread)
        const bool cone = window_cone_spread(W) > 0.0f;
        const float step = cone ? kConeStep : 2.0f, tstep = cone ? dt * kConeStep * 0.5f : dt;
        for (float yc = W.y0 - kEdge; yc <= W.y1 + kEdge; yc += step)
            for (float xc = W.x0 - kEdge; xc <= W.x1 + kEdge; xc += step) {
                const float in = window_inside(W, xc, yc);
                if (!cone && in < -kEdge) continue;
                // Past the door's plane (g_plane_mask), what its bars let by
                // (not for a torch's beams: g_beams_bars, below).
                float t_bars = 1e30f, bars = 1.0f;
                {
                    const PlaneMask& pm = plane_mask_for(W.map);
                    if (g_beams_bars && !pm.m.empty() && pm.map == W.map && std::fabs(sun_x) > 1e-4f) {
                        const float tc = (win_X(W, xc) - pm.plane) / sun_x;
                        if (tc > 0.0f) {
                            t_bars = tc;
                            bars = 1.0f - pm.cover(win_Z(W, xc) - tc * sun_z, yc - tc * rise);
                        }
                    }
                }
                for (float t = 1.0f + tstep * 0.5f;; t += tstep) {
                    const float X = win_X(W, xc) - t * sun_x, Y = yc - t * rise, Z = win_Z(W, xc) - t * sun_z;
                    if (Y < 0.0f) break;
                    const int cx = static_cast<int>(std::floor(X / 32.0f)), cz = static_cast<int>(std::floor(Z / 32.0f));
                    if (16.0f * cell_height(cx, cz) > Y) break;   // the terrain stops it
                    if (g_occ && t * len > kOccluderSkip && g_occ->solid_hard_at(X, Y, Z)) break;   // or the model (a railing lets the beam on)
                    float v = window_aperture(W, xc, yc, t) * (cone ? std::exp(-t / kConeFade) * W.gain * kConeVolume
                                                                     : std::exp(-t / fade));
                    if (t > t_bars) v *= bars;
                    // A cone's haze in shafts fanning from the opening (as
                    // godrays do): stripes across it at a fixed angle from
                    // the window, so each runs straight out of it.
                    if (cone && v > 0.0f) {
                        const float half = 0.5f * (W.x1 - W.x0) + window_cone_spread(W) * t + 1.0f;
                        const float u = (xc - 0.5f * (W.x0 + W.x1)) / half;
                        const float c = 0.5f + 0.5f * std::cos(3.14159265f * kConeShafts * u);
                        v *= kConeShaftFloor + (1.0f - kConeShaftFloor) * c * c;
                    }
                    if (v <= 0.0f) continue;
                    const int ix = static_cast<int>(std::floor(X / kBeamCell)) - B.gx0;
                    const int iy = static_cast<int>(std::floor(Y / kBeamCell));
                    const int iz = static_cast<int>(std::floor(Z / kBeamCell)) - B.gz0;
                    if (ix < 0 || iy < 0 || iz < 0 || ix >= B.gnx || iy >= B.gny || iz >= B.gnz) break;
                    float& c = B.cell[(static_cast<std::size_t>(iz) * B.gny + iy) * B.gnx + ix];
                    if (c == 0.0f) {
                        ++B.lit;
                        const float x0 = (B.gx0 + ix) * kBeamCell, y0 = iy * kBeamCell, z0 = (B.gz0 + iz) * kBeamCell;
                        B.lx0 = std::min(B.lx0, x0); B.lx1 = std::max(B.lx1, x0 + kBeamCell);
                        B.ly0 = std::min(B.ly0, y0); B.ly1 = std::max(B.ly1, y0 + kBeamCell);
                        B.lz0 = std::min(B.lz0, z0); B.lz1 = std::max(B.lz1, z0 + kBeamCell);
                    }
                    c = std::max(c, v);
                }
            }
    }
    B.diag.assign(B.cell.size(), 0.0f);
    for (int iz = B.gnz - 1; iz >= 0; --iz)
        for (int iy = B.gny - 1; iy >= 0; --iy)
            for (int ix = B.gnx - 1; ix >= 0; --ix) {
                const std::size_t i = (static_cast<std::size_t>(iz) * B.gny + iy) * B.gnx + ix;
                float next = 0.0f;
                if (ix + 1 < B.gnx && iy + 1 < B.gny && iz + 1 < B.gnz)
                    next = B.diag[(static_cast<std::size_t>(iz + 1) * B.gny + iy + 1) * B.gnx + ix + 1];
                B.diag[i] = B.cell[i] + next;
            }
    B.glow.clear();
    if (glow && B.lit > 0) {
        // Three box passes along each axis (close to a gaussian), each a
        // third of the reach wide on either side.
        B.glow = B.cell;
        const int r = std::max(1, static_cast<int>(std::lround(window_glow_reach() / kBeamCell / 3.0f)));
        const int dims[3] = {B.gnx, B.gny, B.gnz};
        const std::size_t strides[3] = {1u, static_cast<std::size_t>(B.gnx),
                                        static_cast<std::size_t>(B.gnx) * B.gny};
        std::vector<float> line, sum;
        for (int axis = 0; axis < 3; ++axis) {
            const int len = dims[axis];
            const int o1 = axis == 0 ? 1 : 0, o2 = axis == 2 ? 1 : 2;   // the other two axes
            line.resize(static_cast<std::size_t>(len));
            sum.resize(static_cast<std::size_t>(len) + 1);
            for (int a = 0; a < dims[o1]; ++a)
                for (int b = 0; b < dims[o2]; ++b) {
                    const std::size_t base = a * strides[o1] + b * strides[o2];
                    const std::size_t st = strides[axis];
                    for (int i = 0; i < len; ++i) line[static_cast<std::size_t>(i)] = B.glow[base + i * st];
                    for (int pass = 0; pass < 3; ++pass) {
                        sum[0] = 0.0f;
                        for (int i = 0; i < len; ++i) sum[static_cast<std::size_t>(i) + 1] = sum[static_cast<std::size_t>(i)] + line[static_cast<std::size_t>(i)];
                        for (int i = 0; i < len; ++i) {
                            const int lo = std::max(0, i - r), hi = std::min(len, i + r + 1);
                            line[static_cast<std::size_t>(i)] = (sum[static_cast<std::size_t>(hi)] - sum[static_cast<std::size_t>(lo)]) / static_cast<float>(2 * r + 1);
                        }
                    }
                    for (int i = 0; i < len; ++i) B.glow[base + i * st] = line[static_cast<std::size_t>(i)];
                }
        }
    }
}
// The light the beams radiate at world point (X, Y, Z) on a dark map, 0..1
// (the blurred cells, trilinear at cell centres, times window_glow_gain).
float window_glow_at(float X, float Y, float Z) {
    const WindowBeams& B = g_beams;
    if (B.glow.empty()) return 0.0f;
    const float fx = X / kBeamCell - B.gx0 - 0.5f, fy = Y / kBeamCell - 0.5f, fz = Z / kBeamCell - B.gz0 - 0.5f;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy)),
              iz = static_cast<int>(std::floor(fz));
    if (ix < -1 || iy < -1 || iz < -1 || ix >= B.gnx || iy >= B.gny || iz >= B.gnz) return 0.0f;
    const float ax = fx - ix, ay = fy - iy, az = fz - iz;
    float acc = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
        const int a = ix + dx, b = std::max(0, iy + dy), cc = iz + dz;   // (below the floor: the floor's cell)
        if (a < 0 || cc < 0 || a >= B.gnx || b >= B.gny || cc >= B.gnz) continue;
        const float wgt = (dx ? ax : 1.0f - ax) * (dy ? ay : 1.0f - ay) * (dz ? az : 1.0f - az);
        acc += wgt * B.glow[(static_cast<std::size_t>(cc) * B.gny + b) * B.gnx + a];
    }
    return std::min(1.0f, acc * window_glow_gain());
}
// The sun through the windows on world point (X, Y, Z), 0..1: through the
// opening (soft edge) on its ray toward the sun, the terrain not in the way
// (marched every 4 units, short of the wall).
float cone_shafts_on(float X, float Y, float Z);
float window_sun_at(const std::vector<MapWindow>& wins, float X, float Y, float Z, float sun_x, float sun_z,
                    float rise, const CellHeights& cell_height) {
    float best = 0.0f;
    for (const MapWindow& W : wins) {
        const float to = W.on_x ? sun_x : sun_z;
        if (std::fabs(to) < 1e-4f) continue;
        const float t = (W.plane_z - (W.on_x ? X : Z)) / to;
        if (t <= 1.0f) continue;
        const float along = W.on_x ? Z + t * sun_z : X + t * sun_x;
        float v = window_aperture(W, along, Y + t * rise, t);
        // A feathered pool's light is spread thinner the wider it lands (not its haze:
        // the beam stays as it was). KOL_WIN_POOL_PEAK=<share>: mockups.
        static const float peak_env = kol::env_num("KOL_WIN_POOL_PEAK", -1.0f);
        if (peak_env >= 0.0f) v *= peak_env;
        else if (W.pool_peak > 0.0f) v *= W.pool_peak;
        if (window_cone_spread(W) > 0.0f) v = std::min(1.0f, v * kConeGain * W.gain * std::exp(-t / kConeFade));   // a cone fades into the room
        if (v > best) v *= plane_mask_pass(W, X, Y, Z, sun_x, rise, sun_z, t + 0.5f);
        if (v <= best) continue;
        bool clear = true;
        if (window_cone_spread(W) > 0.0f && !W.on_x) {
            // A cone's light comes from its opening: tested from the point to
            // the nearest spot of the opening (along the sun's way the wall
            // beside the window stood in the way of all but the window's own
            // projection, and the cone never spread), and to four spots
            // around it (kConeSoftR apart, within the opening): lit by the
            // share that is clear, so a shadow's edge is soft, softer the
            // further it falls from what casts it (the user asked for
            // slightly softer shadow edges). Its shadows fan out from the
            // window.
            const float cx0 = std::max(W.x0, std::min(W.x1, along)), cy0 = std::max(W.y0, std::min(W.y1, Y + t * rise));
            int seen = 0;
            for (int smp = 0; smp < 5; ++smp) {
                static constexpr float kOff[5][2] = {{0, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                const float tx = std::max(W.x0, std::min(W.x1, cx0 + kOff[smp][0] * kConeSoftR));
                const float ty = std::max(W.y0, std::min(W.y1, cy0 + kOff[smp][1] * kConeSoftR));
                const float dx = tx - X, dy = ty - Y, dz = W.plane_z - Z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                bool ok = true;
                for (float d = 2.0f; d < dist - kOccluderSkip; d += 3.0f) {
                    const float f = d / dist, x = X + f * dx, y = Y + f * dy, z = Z + f * dz;
                    const int cx = static_cast<int>(std::floor(x / 32.0f)), cz = static_cast<int>(std::floor(z / 32.0f));
                    // (The window's own wall, a few units deep and with no
                    // hole in the model, does not count: a ray reaching the
                    // opening at a slant ran through more of it than the
                    // skip's 6 units, and the cone's sides went dark.)
                    if (16.0f * cell_height(cx, cz) > y + 0.5f ||
                        (g_occ && d > kOccluderSkip && z > W.plane_z + kWindowWallDepth && g_occ->solid_at(x, y, z))) {
                        ok = false;
                        break;
                    }
                }
                seen += ok;
            }
            v *= seen / 5.0f;
            if (v > best) best = v;
            continue;
        }
        const float len = std::sqrt(sun_x * sun_x + rise * rise + sun_z * sun_z);
        for (float d = 2.0f; d < t - 3.0f; d += 4.0f) {
            const float x = X + d * sun_x, z = Z + d * sun_z, y = Y + d * rise;
            const int cx = static_cast<int>(std::floor(x / 32.0f)), cz = static_cast<int>(std::floor(z / 32.0f));
            if (16.0f * cell_height(cx, cz) > y + 0.5f) { clear = false; break; }
            if (g_occ && d * len > kOccluderSkip && (t - d) * len > kOccluderSkip && g_occ->solid_at(x, y, z)) {
                clear = false;
                break;
            }
        }
        if (clear) best = v;
    }
    if (!g_shafts.empty()) best = std::max(best, cone_shafts_on(X, Y, Z));   // a cone's shafts land
    return best;
}
// The beams' haze in front of world point (X, Y, Z) along the view line
// toward the camera, 0..1 (g_beams' diagonal sums, trilinear at cell
// centres, a cell counting its 4 units of the line).
// The cone shafts' light in front of world point (X, Y, Z) along its view
// line toward the camera (+1, +1, +1), 0..1: for each shaft, its nearest
// approach to the line in front of the point, a soft tube widening and
// fading along the shaft.
float cone_shafts_at(float X, float Y, float Z) {
    constexpr float k = kConeShaftHaze;
    constexpr float kInv3 = 0.57735027f;
    float acc = 0.0f;
    for (const ConeShaft& S : g_shafts) {
        const float wx = X - S.ax, wy = Y - S.ay, wz = Z - S.az;
        const float b = kInv3 * (S.dx + S.dy + S.dz);          // view . shaft
        const float d = kInv3 * (wx + wy + wz);                // view . (P - A)
        const float e = S.dx * wx + S.dy * wy + S.dz * wz;      // shaft . (P - A)
        const float den = 1.0f - b * b;
        if (den < 1e-4f) continue;
        float u = (e - b * d) / den, sv = (b * e - d) / den;
        if (sv < -2.0f) continue;                              // behind the surface
        if (sv < 0.0f) { sv = 0.0f; u = e; }
        u = std::max(0.0f, std::min(S.len, u));
        sv = std::max(0.0f, u * b - d);
        const float rx = wx + sv * kInv3 - u * S.dx, ry = wy + sv * kInv3 - u * S.dy, rz = wz + sv * kInv3 - u * S.dz;
        const float r2 = rx * rx + ry * ry + rz * rz;
        const float w = S.w0 + S.grow * u;
        const float in = std::min(1.0f, u / 6.0f) * std::max(0.0f, 1.0f - u / S.fade);   // from the opening, fading to the floor
        acc += S.gain * in * std::exp(-r2 / (w * w));
    }
    return std::min(1.0f, k * acc);
}
// ... and the light they lay on the surfaces they pass or land on: each
// shaft's tube at the point itself (its distance from the shaft's line),
// as bright as its haze reads in the air.
float cone_shafts_on(float X, float Y, float Z) {
    constexpr float k = kConeShaftLight;
    float acc = 0.0f;
    for (const ConeShaft& S : g_shafts) {
        const float wx = X - S.ax, wy = Y - S.ay, wz = Z - S.az;
        const float u = std::max(0.0f, std::min(S.len, S.dx * wx + S.dy * wy + S.dz * wz));
        const float rx = wx - u * S.dx, ry = wy - u * S.dy, rz = wz - u * S.dz;
        const float w = S.w0 + S.grow * u;
        const float in = std::min(1.0f, u / 6.0f) * std::max(0.0f, 1.0f - 0.5f * u / S.fade);
        acc += S.gain * in * std::exp(-(rx * rx + ry * ry + rz * rz) / (w * w));
    }
    return std::min(1.0f, k * acc);
}
float window_haze_at(float X, float Y, float Z) {
    const float X0 = X, Y0 = Y, Z0 = Z;   // (the shafts read the surface point itself)
    const WindowBeams& B = g_beams;
    static const float haze_env = kol::env_num("KOL_GODRAYS_HAZE", -1.0f);
    const float haze_k = haze_env >= 0.0f ? haze_env : g_win_haze_k;
    // No haze at all, the cones' shafts in the air too, on a map whose
    // haze is 0 (map 71: the shaft painted on the back wall stood up as a
    // column with the camera low; the user chose none).
    if (haze_k <= 0.0f) return 0.0f;
    // Start a little in front of the surface, so a wall's own cell does not
    // count behind it.
    X += 1.0f; Y += 1.0f; Z += 1.0f;
    // A point before the grid (low X or Z: its view line enters it further
    // on) moves on along the line by whole cells: the cells it passes are
    // empty, and the read stays on the same diagonals with the same weights.
    const float need = std::max({B.gx0 * kBeamCell - X, B.gz0 * kBeamCell - Z, -Y});
    if (need > 0.0f) {
        const float adv = kBeamCell * (std::ceil(need / kBeamCell) + 1.0f);
        X += adv; Y += adv; Z += adv;
    }
    const float fx = X / kBeamCell - B.gx0 - 0.5f, fy = Y / kBeamCell - 0.5f, fz = Z / kBeamCell - B.gz0 - 0.5f;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy)),
              iz = static_cast<int>(std::floor(fz));
    if (ix < -1 || iy < -1 || iz < -1 || ix >= B.gnx || iy >= B.gny || iz >= B.gnz)
        return g_shafts.empty() ? 0.0f : cone_shafts_at(X0, Y0, Z0);
    auto diag_at = [&](int a, int b, int c) -> float {
        if (a < 0 || b < 0 || c < 0 || a >= B.gnx || b >= B.gny || c >= B.gnz) return 0.0f;
        return B.diag[(static_cast<std::size_t>(c) * B.gny + b) * B.gnx + a];
    };
    const float ax = fx - ix, ay = fy - iy, az = fz - iz;
    float acc = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
        const float wgt = (dx ? ax : 1.0f - ax) * (dy ? ay : 1.0f - ay) * (dz ? az : 1.0f - az);
        if (wgt > 0.0f) acc += wgt * diag_at(ix + dx, iy + dy, iz + dz);
    }
    acc *= kBeamCell;
    const float vol = acc > 0.0f ? 1.0f - std::exp(-acc * haze_k) : 0.0f;
    if (g_shafts.empty()) return vol;
    return 1.0f - (1.0f - vol) * (1.0f - cone_shafts_at(X0, Y0, Z0));
}
// Whether the sun is behind one of the windows: a window facing +Z
// (sun_z < 0) or, `on_x`, facing +X (sun_x < 0; map 67's corridor, the
// light straight along -X: the check was sun_z's alone and it never lit).
bool sun_behind_a_window(const std::vector<MapWindow>& wins, float sun_x, float sun_z) {
    for (const MapWindow& W : wins)
        if ((W.on_x ? sun_x : sun_z) < -1e-4f) return true;
    return false;
}
// This map's windows in the set being built (in_win_set).
std::vector<MapWindow> windows_in_set(int map) {
    std::vector<MapWindow> wins;
    for (const MapWindow& W : map_windows_all())
        if (W.map == map && in_win_set(W)) wins.push_back(W);
    return wins;
}
// The screen bounds of a world box (its 8 corners), unclipped.
void screen_bounds(float X0, float X1, float Y0, float Y1, float Z0, float Z1, int ox, int oy, int kx, int ky,
                   float& a0, float& a1, float& b0, float& b1) {
    a0 = 1e9f; a1 = -1e9f; b0 = 1e9f; b1 = -1e9f;
    for (float X : {X0, X1})
        for (float Z : {Z0, Z1})
            for (float Y : {Y0, Y1}) {
                const float sx = (X - Z) / 2.0f + ox + kx, sy = (X + Z) / 4.0f - Y / 2.0f + oy + ky;
                a0 = std::min(a0, sx); a1 = std::max(a1, sx); b0 = std::min(b0, sy); b1 = std::max(b1, sy);
            }
}
// The screen rectangle of a world box (its 8 corners), with a margin for
// the points' rounding, within the w x h picture.
void screen_rect(float X0, float X1, float Y0, float Y1, float Z0, float Z1, int w, int h, int ox, int oy, int kx,
                 int ky, int& x0, int& x1, int& y0, int& y1) {
    float a0, a1, b0, b1;
    screen_bounds(X0, X1, Y0, Y1, Z0, Z1, ox, oy, kx, ky, a0, a1, b0, b1);
    x0 = std::max(0, static_cast<int>(std::floor(a0 - 3.0f)));
    x1 = std::min(w - 1, static_cast<int>(std::ceil(a1 + 3.0f)));
    y0 = std::max(0, static_cast<int>(std::floor(b0 - 3.0f)));
    y1 = std::min(h - 1, static_cast<int>(std::ceil(b1 + 3.0f)));
}
}  // namespace

namespace kol_focus_detail {
// Then, per view: the sun through a window on each point the terrain pass
// drew (exact, its own ray), and the beams' haze along each pixel's view
// line, only where the lit cells' bounds can be.
void build_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                        const CellHeights& cell_height) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t n = static_cast<std::size_t>(w) * h;
    g_win_patch.assign(n, 0);
    g_win_haze.assign(n, 0);
    g_win_glow.clear();
    g_win_any = false;
    const int map = current_map();
    g_win_haze_k = g_win_set ? 0.04f : map_window_haze(map);   // (a moon's haze: map 65's)
    const std::vector<MapWindow> wins = windows_in_set(map);
    const bool dark = dark_windows();
    // The sun must be behind one of them (sun_behind_a_window).
    if (!sun_behind_a_window(wins, sun_x, sun_z) || (!g_shadow_on && !dark)) return;
    const float rise = g_win_set ? g_win_set_rise : shadow_rise();
    // (The second set's light is fixed: its beams change with the heights
    // only, not with the map's sun or the torch's sway.)
    const std::uint32_t bkey = g_win_set ? (g_cast_heights_key ^ static_cast<std::uint32_t>(map + 1)) * 16777619u ^ 0x27d4eb2fu
                                         : ((dark ? (g_win_sun_key ^ static_cast<std::uint32_t>(map + 1)) * 16777619u ^ 0x5bd1e995u
                                                  : (g_shadow_key ^ static_cast<std::uint32_t>(map + 1)) * 16777619u) ^
                                            g_plane_mask.hash ^ static_cast<std::uint32_t>(g_shutter_spread * 1000.0f)) * 16777619u;
    // A torch out of view (window_torch, map 67): its beams (the haze, and
    // the light they radiate) are built once, the torch at rest and the
    // door's bars left out; the sway and the bars move the light on the
    // surfaces only (window_sun_at, below). Built with them, every sway step
    // and every frame of the door's swing rebuilt the grid: ~18 ms on the
    // Mac each, the walk-in into the cell slow on the Thor (2026-10-30).
    const bool torch = !g_win_set && window_torch();
    float beam_x = sun_x, beam_z = sun_z, beam_rise = rise;
    std::uint32_t beam_key = bkey;
    if (torch) {
        t_torch_at_rest = true;
        shadow_dir(beam_x, beam_z);
        beam_rise = shadow_rise();
        t_torch_at_rest = false;
        beam_key = (g_cast_heights_key ^ static_cast<std::uint32_t>(map + 1)) * 16777619u ^ 0x1b873593u;
    }
    bool rebuilt = false;
    if (!g_beams.valid || g_beams.key != beam_key) {
        g_beams_bars = !torch;
        build_window_beams(wins, beam_x, beam_z, beam_rise, cell_height);
        g_beams_bars = true;
        g_beams.key = beam_key;
        g_beams.valid = true;
        rebuilt = true;
    }
    const WindowBeams& B = g_beams;
    const auto t1 = std::chrono::steady_clock::now();
    // A pixel's point: its surface's, a wall's a little in front of it; the
    // sky's on the ground plane.
    auto point = [&](int px, int py, std::size_t k, float& X, float& Y, float& Z) {
        if (g_covered[k]) {
            X = g_wx[k];
            Z = g_wz[k];
            Y = static_cast<float>(g_y[k]);
            if (g_face[k] == 1) X += 0.5f;
            else if (g_face[k] == 2) Z += 0.5f;
        } else {
            const float a = 2.0f * static_cast<float>(px - ox - kx);
            const float b = 4.0f * static_cast<float>(py - oy - ky);
            X = 0.5f * (a + b);
            Z = 0.5f * (b - a);
            Y = 0.0f;
        }
    };
    // The screen rectangle of a world box (screen_rect).
    auto rect = [&](float X0, float X1, float Y0, float Y1, float Z0, float Z1, int& x0, int& x1, int& y0, int& y1) {
        screen_rect(X0, X1, Y0, Y1, Z0, Z1, w, h, ox, oy, kx, ky, x0, x1, y0, y1);
    };
    if (B.lit == 0) return;
    // The three per-pixel steps below go over the row pool (2026-11-18: with
    // the camera panning the view changes every picture and they are worked
    // out again each time, 2.8 ms a job on the Mac's map 55, the largest
    // piece of the Steam Deck's 19.5). A pixel's value is its own point's
    // alone, so the rows may go in any order; for the helpers, everything a
    // point's read fills as it goes is filled first:
    //  - each cell's height (CellHeights::raw fills its memo through
    //    SceneBuild::cell_height_of, which reads this thread's thread_locals:
    //    on a helper it would answer for an empty map);
    //  - the windows' plane masks (plane_mask_for builds a map's on first use).
    // A helper also takes this thread's t_present for the run (current_map()
    // and the like read the capture or the live game by it).
    // KOL_WIN_SERIAL=1: on this thread alone, as before. KOL_WIN_PAR_CHECK=1:
    // both ways, compared (a difference is logged).
    static const bool serial_only = kol::env_set("KOL_WIN_SERIAL");
    static const bool par_check = kol::env_set("KOL_WIN_PAR_CHECK");
    if (!serial_only) {
        for (int cz = -kMargin; cz < kGameSide + kMargin; ++cz)
            for (int cx = -kMargin; cx < kGameSide + kMargin; ++cx) (void)cell_height.raw(cx, cz);
        for (const MapWindow& W : wins) (void)plane_mask_for(W.map);
    }
    const bool present = t_present;
    // fn(px, py, k) over the rectangle's pixels, split by rows or not.
    auto over = [&](int rx0, int rx1, int ry0, int ry1, bool split, const std::function<void(int, int, std::size_t)>& fn) {
        if (ry1 < ry0 || rx1 < rx0) return;
        auto rows = [&](int r0, int r1) {
            for (int py = ry0 + r0; py < ry0 + r1; ++py)
                for (int px = rx0; px <= rx1; ++px) fn(px, py, static_cast<std::size_t>(py) * w + px);
        };
        if (!split) { rows(0, ry1 - ry0 + 1); return; }
        kol::parallel_rows(ry1 - ry0 + 1, 4, [&](int, int r0, int r1) {
            const bool was = t_present;
            t_present = present;
            rows(r0, r1);
            t_present = was;
        });
    };
    // The steps, each into the buffer it is given (the check runs them twice).
    std::atomic<bool> patch_any{false};
    // The sun on the points themselves: they lie in the beams' cells or
    // just under them (the cell a surface point sits in can be the
    // terrain's), so within the lit bounds grown by a cell.
    auto patch_step = [&](std::uint8_t* out, bool split) {
        int x0, x1, y0, y1;
        rect(B.lx0 - kBeamCell, B.lx1 + kBeamCell, B.ly0 - kBeamCell, B.ly1 + kBeamCell,
             B.lz0 - kBeamCell, B.lz1 + kBeamCell, x0, x1, y0, y1);
        over(x0, x1, y0, y1, split, [&](int px, int py, std::size_t k) {
            if (!g_covered[k]) return;
            float X, Y, Z;
            point(px, py, k, X, Y, Z);
            const float best = window_sun_at(wins, X, Y, Z, sun_x, sun_z, rise, cell_height);
            if (best > 0.0f) {
                out[k] = static_cast<std::uint8_t>(std::lround(255.0f * best));
                patch_any.store(true, std::memory_order_relaxed);   // (even where it rounds to 0, as before)
            }
        });
    };
    // The haze: every pixel whose view line can cross the lit cells (their
    // bounds on screen; points in front of the grid are carried on to it).
    auto haze_step = [&](std::uint8_t* out, bool split) {
        int x0, x1, y0, y1;
        rect(B.lx0, B.lx1, B.ly0, B.ly1, B.lz0, B.lz1, x0, x1, y0, y1);
        over(x0, x1, y0, y1, split, [&](int px, int py, std::size_t k) {
            float X, Y, Z;
            point(px, py, k, X, Y, Z);
            const float hz = window_haze_at(X, Y, Z);
            if (hz > 0.0f) out[k] = static_cast<std::uint8_t>(std::lround(255.0f * hz));
        });
    };
    // On a dark map, the light the beams radiate, around their bounds grown
    // by its reach.
    auto glow_step = [&](std::uint8_t* out, bool split) {
        int x0, x1, y0, y1;
        const float R = window_glow_reach() * 1.5f;
        rect(B.lx0 - R, B.lx1 + R, 0.0f, B.ly1 + R, B.lz0 - R, B.lz1 + R, x0, x1, y0, y1);
        over(x0, x1, y0, y1, split, [&](int px, int py, std::size_t k) {
            float X, Y, Z;
            point(px, py, k, X, Y, Z);
            const float gl = window_glow_at(X, Y + 2.0f, Z);
            if (gl > 0.0f) out[k] = static_cast<std::uint8_t>(std::lround(255.0f * gl));
        });
    };
    const bool split = !serial_only;
    patch_step(g_win_patch.data(), split);
    const auto t2 = std::chrono::steady_clock::now();
    haze_step(g_win_haze.data(), split);
    if (!B.glow.empty()) {
        g_win_glow.assign(n, 0);
        glow_step(g_win_glow.data(), split);
    }
    if (par_check && split) {
        static thread_local std::vector<std::uint8_t> ref;
        auto compare = [&](const char* what, const std::vector<std::uint8_t>& got,
                           const std::function<void(std::uint8_t*, bool)>& step) {
            ref.assign(n, 0);
            step(ref.data(), false);
            std::size_t bad = 0;
            for (std::size_t k = 0; k < n; ++k) bad += ref[k] != got[k];
            static long checks = 0, fails = 0;
            ++checks;
            if (bad) ++fails;
            if (bad || checks % 600 == 1)
                std::fprintf(stderr, "win-par-check: %s: %zu of %zu pixels differ (%ld checks, %ld with a difference)\n",
                             what, bad, n, checks, fails);
        };
        compare("patch", g_win_patch, patch_step);
        compare("haze", g_win_haze, haze_step);
        if (!B.glow.empty()) compare("glow", g_win_glow, glow_step);
    }
    // Any light at all (as the steps set it pixel by pixel before: the sun on
    // any point, a haze or a glow that is not 0).
    g_win_any = patch_any.load();
    for (std::size_t k = 0; k < n && !g_win_any; ++k)
        g_win_any = g_win_haze[k] || (!g_win_glow.empty() && g_win_glow[k]);
    static const bool godrays_time = kol::env_set("KOL_GODRAYS_TIME");
    if (godrays_time)
        std::fprintf(stderr, "godrays: map %d, %zu cells lit%s: grid %.2f ms, patch %.2f ms, haze %.2f ms\n", map,
                     B.lit, rebuilt ? " (rebuilt)" : "",
                     std::chrono::duration<double, std::milli>(t1 - t0).count(),
                     std::chrono::duration<double, std::milli>(t2 - t1).count(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count());
}
void unit_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                       const CellHeights& cell_height) {
    g_unit_win_any = false;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const WindowBeams& B = g_beams;
    if (godray_strength() <= 0.0f || !B.valid || B.lit == 0 || (!g_shadow_on && !dark_windows()) ||
        g_lift.size() != n)
        return;
    const std::vector<MapWindow> wins = windows_in_set(current_map());
    if (!sun_behind_a_window(wins, sun_x, sun_z)) return;
    const float rise = g_win_set ? g_win_set_rise : shadow_rise();
    g_unit_win_patch.assign(n, 0);
    g_unit_win_haze.assign(n, 0);
    constexpr int S = kol::ScenePixels::kSpriteSlot;
    const std::size_t an = static_cast<std::size_t>(g_atlas_w) * g_atlas_h;
    g_unit_win_patch_a.assign(an, 0);
    g_unit_win_haze_a.assign(an, 0);
    // The lit cells' bounds on screen, a card's width (16) around them.
    float a0, a1, b0, b1;
    screen_bounds(B.lx0, B.lx1, B.ly0, B.ly1, B.lz0, B.lz1, ox, oy, kx, ky, a0, a1, b0, b1);
    a0 -= 16.0f; a1 += 16.0f; b0 -= 24.0f; b1 += 24.0f;
    for (std::size_t bi = 0; bi < g_casters.size() && bi < g_blocks.size(); ++bi) {
        const BlockCaster& c = g_casters[bi];
        if (!c.unit) continue;
        const kol::ScenePixels::SpriteBlock& b = g_blocks[bi];
        if (b.sx + b.w < a0 || b.sx > a1 || b.sy + b.h < b0 || b.sy > b1) continue;
        const int ax0 = g_atlas_per_row > 0 ? (b.slot % g_atlas_per_row) * S : 0;
        const int ay0 = g_atlas_per_row > 0 ? (b.slot / g_atlas_per_row) * S : 0;
        for (int ty = 0; ty < b.h; ++ty)
            for (int tx = 0; tx < b.w; ++tx) {
                const int px = b.sx + tx, py = b.sy + ty;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                const std::size_t k = static_cast<std::size_t>(py) * w + px;
                const std::size_t a = static_cast<std::size_t>(ay0 + ty) * g_atlas_w + ax0 + tx;
                const bool own = g_lift[k] == bi + 1;
                // Every texel of the card the atlas draws, hidden or not.
                if (!own && (a >= an || a >= g_atlas_opaque.size() || !g_atlas_opaque[a])) continue;
                float X, Y, Z;
                card_point(c, px, py, ox, oy, &X, &Y, &Z);
                const float sun = window_sun_at(wins, X, Y, Z, sun_x, sun_z, rise, cell_height);
                const float hz = window_haze_at(X, Y, Z);
                const std::uint8_t ps = sun > 0.0f ? static_cast<std::uint8_t>(std::lround(255.0f * sun)) : 0;
                const std::uint8_t hs = hz > 0.0f ? static_cast<std::uint8_t>(std::lround(255.0f * hz)) : 0;
                if (own) { g_unit_win_patch[k] = ps; g_unit_win_haze[k] = hs; }
                if (a < an) { g_unit_win_patch_a[a] = ps; g_unit_win_haze_a[a] = hs; }
                if (ps || hs) g_unit_win_any = true;
            }
    }
    static const bool godrays_time = kol::env_set("KOL_GODRAYS_TIME");
    if (godrays_time && g_unit_win_any) {
        std::size_t p = 0, z = 0;
        for (std::size_t k = 0; k < n; ++k) { p += g_unit_win_patch[k] != 0; z += g_unit_win_haze[k] != 0; }
        std::fprintf(stderr, "godrays: units: %zu pixels in the sun, %zu hazed\n", p, z);
    }
}
void lift_window_light(int w, int h, int ox, int oy, int kx, int ky, float sun_x, float sun_z,
                       const CellHeights& cell_height, std::uint32_t memo_key) {
    g_lift_win_any = false;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (!g_win_any || g_win_patch.size() != n || g_lift.size() != n || g_under_xz.size() != n) return;
    // The main set's windows (run only as the main set, g_win_set 0).
    const std::vector<MapWindow> wins = windows_in_set(current_map());
    const float rise = shadow_rise();
    g_lift_win_patch.assign(n, 0);
    g_lift_win_haze.assign(n, 0);
    // Only within the beams' bounds on screen (grown by a cell, as the
    // patch's): outside them the light is none, front or behind.
    const WindowBeams& B = g_beams;
    int x0, x1, y0, y1;
    screen_rect(B.lx0 - kBeamCell, B.lx1 + kBeamCell, B.ly0 - kBeamCell, B.ly1 + kBeamCell, B.lz0 - kBeamCell,
                B.lz1 + kBeamCell, w, h, ox, oy, kx, ky, x0, x1, y0, y1);
    // The light on the point under each lifted pixel, kept from the last
    // picture (2026-11-18, the lifting's cost on the Steam Deck: this step
    // was 0.6 ms a picture on map 55, the same points worked out again each
    // time while the camera turned or the units moved). A point's light is
    // its own (X, Y, Z)'s alone under everything memo_key names, so where
    // the key is the same and a pixel's point is the last picture's at the
    // pixel the scroll brings it from (the same pixel if the view did not
    // move), the last picture's values are its own. The memo holds, for
    // every pixel of the last rectangle, the point (none: kNoPoint) and its
    // two values. KOL_LIFT_WIN_MEMO=0: every point worked out.
    // KOL_LIFT_WIN_MEMO_CHECK=1: every point worked out and compared with
    // what the memo gave (a difference is logged).
    struct Memo {
        std::uint32_t key = 0;
        int w = 0, h = 0, sx = 0, sy = 0, x0 = 0, x1 = -1, y0 = 0, y1 = -1;
        std::vector<std::int32_t> xz, y;
        std::vector<std::uint8_t> sun, hz;
    };
    static Memo memo, next;
    static const bool memo_on = kol::env_not_zero("KOL_LIFT_WIN_MEMO");
    static const bool memo_check = kol::env_set("KOL_LIFT_WIN_MEMO_CHECK");
    constexpr std::int32_t kNoPoint = INT32_MIN;
    const bool use = memo_on && memo_key != 0u && memo.key == memo_key && memo.w == w && memo.h == h;
    // A point's pixel moves with the scroll: from (px - dx, py - dy) then.
    const int dx = (ox + kx) - memo.sx, dy = (oy + ky) - memo.sy;
    if (next.xz.size() != n) {
        next.xz.assign(n, kNoPoint);
        next.y.assign(n, 0);
        next.sun.assign(n, 0);
        next.hz.assign(n, 0);
    }
    long reused = 0, worked = 0, differ = 0;
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            const std::size_t k = static_cast<std::size_t>(py) * w + px;
            next.xz[k] = kNoPoint;
            const int bi = static_cast<int>(g_lift[k]) - 1;
            if (bi < 0 || bi >= static_cast<int>(g_blocks.size())) continue;
            if (!g_blocks[bi].prop && !g_blocks[bi].surface) continue;
            g_lift_win_patch[k] = g_win_patch[k];
            g_lift_win_haze[k] = g_win_haze[k];
            if (g_lift_win_patch[k] || g_lift_win_haze[k]) g_lift_win_any = true;
            g_win_patch[k] = g_win_haze[k] = 0;
            if (g_under_kind[k] == kol::kSceneNone) continue;
            const std::int32_t pxz = g_under_xz[k], pyy = g_under_y[k];
            std::uint8_t ps = 0, hs = 0;
            bool have = false;
            if (use) {
                const int qx = px - dx, qy = py - dy;
                if (qx >= memo.x0 && qx <= memo.x1 && qy >= memo.y0 && qy <= memo.y1) {
                    const std::size_t q = static_cast<std::size_t>(qy) * w + qx;
                    if (memo.xz[q] == pxz && memo.y[q] == pyy) {
                        ps = memo.sun[q];
                        hs = memo.hz[q];
                        have = true;
                    }
                }
            }
            if (!have || memo_check) {
                const float u = 2.0f * static_cast<float>(px - ox - kx) + 1.0f;   // X - Z
                const float xz = static_cast<float>(pxz);
                const float X = 0.5f * (xz + u), Z = 0.5f * (xz - u), Y = static_cast<float>(pyy);
                const float sun = window_sun_at(wins, X, Y, Z, sun_x, sun_z, rise, cell_height);
                const float hz = window_haze_at(X, Y, Z);
                const std::uint8_t cs = sun > 0.0f ? static_cast<std::uint8_t>(std::lround(255.0f * sun)) : 0;
                const std::uint8_t ch = hz > 0.0f ? static_cast<std::uint8_t>(std::lround(255.0f * hz)) : 0;
                if (have && (cs != ps || ch != hs)) ++differ;
                ps = cs;
                hs = ch;
                ++worked;
            }
            if (have) ++reused;
            g_win_patch[k] = ps;
            g_win_haze[k] = hs;
            next.xz[k] = pxz;
            next.y[k] = pyy;
            next.sun[k] = ps;
            next.hz[k] = hs;
        }
    std::swap(memo, next);
    memo.key = memo_key;
    memo.w = w;
    memo.h = h;
    memo.sx = ox + kx;
    memo.sy = oy + ky;
    memo.x0 = x0; memo.x1 = x1; memo.y0 = y0; memo.y1 = y1;
    if (memo_check) {
        static long checks = 0, fails = 0;
        ++checks;
        if (differ) ++fails;
        if (differ || checks % 300 == 1)
            std::fprintf(stderr, "lift-win-memo-check: %ld reused, %ld differ, %ld worked out (%ld checks, %ld with a difference)\n",
                         reused, differ, worked, checks, fails);
    }
}
}  // namespace kol_focus_detail

namespace kol {
// Map 71's window shutter (the user asked for the light to follow the
// window panel's movement): a panel hinged at the window's left edge
// that the game swings among four positions by swapping BG tiles (no
// sprite). Each picture, the scene's pixels over the wall around the window
// (Z 32, X 140-200, Y 60-106, wherever the camera has it) are hashed; a
// known hash gives how far the panel lets the light in, and the window's
// light (its strength) follows it. An unknown hash (lightning, a fade, a
// unit in front) keeps the last. KOL_SHUTTER_LOG=1 prints the hashes.
struct ShutterState { std::uint32_t hash; float open; };
// The four positions (states/thor/0050; hashes logged at 384x216 on the
// Mac, each checked against its frame with the light off: presents 120,
// 150, 175, 200): the panel flat against the wall, open (e16ec4c6); angled,
// half open (9833cea3); nearly shut (fa696628); mostly closed (e74e04dc;
// the user pointed out this fourth position). ~25 frames each.
// The lightning's flash (68784066) is not listed: it keeps the last.
constexpr ShutterState kShutter71[] = {
    {0xe16ec4c6u, 1.0f}, {0x9833cea3u, 0.6f}, {0xfa696628u, 0.25f}, {0xe74e04dcu, 0.35f}};
float g_shutter_open = 1.0f;     // the position's (kShutter71)
// ... eased toward it a picture at a time (g_shutter_shown, beside
// window_cone_spread; the user asked for a short transition between
// states, then a faster ease): the light's
// strength and the bounce lamp follow it; the cone's spread snaps to the
// position (g_shutter_spread; its four window lights cached).
// The window's light for an eased openness (the user found the brightest
// rays too bright and asked for every step dimmer, the brightest most,
// then gave the values): through the positions' points
// 0.25 -> 0.2, 0.35 -> 0.28, 0.6 -> 0.38, 1.0 -> 0.5. The cone and the
// bounce keep the openness itself.
float shutter_light(float open) {
    constexpr float kIn[] = {0.25f, 0.35f, 0.6f, 1.0f}, kOut[] = {0.2f, 0.28f, 0.38f, 0.5f};
    if (open <= kIn[0]) return open * (kOut[0] / kIn[0]);
    for (int i = 1; i < 4; ++i)
        if (open <= kIn[i]) return kOut[i - 1] + (open - kIn[i - 1]) * (kOut[i] - kOut[i - 1]) / (kIn[i] - kIn[i - 1]);
    return kOut[3];
}

void read_shutter(const std::uint8_t* rgb, const std::uint8_t* layers, int w, int h, int ox, int oy, int kx, int ky) {
    static const bool log = std::getenv("KOL_SHUTTER_LOG") != nullptr;
    if (!rgb || current_map() != 71) {
        g_shutter_open = g_shutter_shown = g_shutter_spread = 1.0f;
        g_lightning = 0.0f;
        return;
    }
    int x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (float X : {140.0f, 200.0f})
        for (float Y : {60.0f, 106.0f}) {
            const int sx = static_cast<int>(std::lround((X - 32.0f) / 2.0f)) + kx + ox;
            const int sy = static_cast<int>(std::lround((X + 32.0f) / 4.0f - Y / 2.0f)) + ky + oy;
            x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        }
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    std::uint32_t hsh = kol::kFnvBasis, sum = 0;
    int bg1 = 0, bg1_bright = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const std::uint8_t* c = rgb + (static_cast<std::size_t>(y) * w + x) * 3;
            kol::fnv_mix(hsh, c[0]);
            kol::fnv_mix(hsh, c[1]);
            kol::fnv_mix(hsh, c[2]);
            if (log) sum += c[0] + c[1] + c[2];   // (only the log's mean)
            if (layers && scene_top(layers, static_cast<std::size_t>(y) * w + x) == 1u) {
                ++bg1;
                if (c[0] + c[1] + c[2] > 450) ++bg1_bright;
            }
        }
    // The game's lightning: BG1 tiles laid over the window's wall (the
    // user identified them as overlaid BG1 tiles); the rectangle has no BG1
    // otherwise. 612 BG1 pixels, 529 of them bright, in a flash (state 50:
    // double flashes ~400 pictures apart, 2-3 then 8-12 pictures).
    g_lightning = bg1_bright > 100 ? 1.0f : g_lightning * 0.5f;
    if (g_lightning < 0.01f) g_lightning = 0.0f;
    bool known = false;
    for (const ShutterState& st : kShutter71)
        if (st.hash == hsh) { g_shutter_open = st.open; known = true; }
    // Over about 4 pictures (0.6 of the way a picture: 97% after 4; 0.12,
    // then 0.3, were too slow; the user asked for a faster ease, about 4
    // frames).
    g_shutter_shown += (g_shutter_open - g_shutter_shown) * 0.6f;
    if (std::fabs(g_shutter_open - g_shutter_shown) < 0.005f) g_shutter_shown = g_shutter_open;
    // The cone's width snaps to the position (eased, each step was a
    // rebuild; four widths are cached).
    g_shutter_spread = std::round((0.4f + 0.6f * g_shutter_open) * 20.0f) / 20.0f;
    if (log) {
        const float mean = static_cast<float>(sum) / (3.0f * static_cast<float>((x1 - x0 + 1) * (y1 - y0 + 1)));
        std::fprintf(stderr, "shutter: serial %u hash %08x %s open %.2f shown %.2f spread %.2f mean %.1f bg1 %d bright %d lightning %.2f\n",
                     static_cast<unsigned>(g_cap.serial), hsh, known ? "known" : "unknown", g_shutter_open, g_shutter_shown, g_shutter_spread, mean, bg1,
                     bg1_bright, g_lightning);
    }
}
}  // namespace kol
