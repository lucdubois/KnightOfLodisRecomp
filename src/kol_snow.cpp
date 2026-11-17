#include "kol_snow.h"

// Our own snow (asked 2026-11-02 to do as with the
// rain: hide the game's snow and replace it with our own effect).
// The game draws ten 8x8 flakes (2x2 of mint white, 214,247,239) in screen
// space at its weather block's level (+6, while +7 is 3): 0 five falling
// slowly, 1 ten, some faster, 2 a blizzard of ten falling 1.6-3 px a frame
// and blown sideways 2-3 (their particles' velocities, 33 saved states).
// These flakes live in the world, as our rain's drops do (kol_rain): each
// has a landing place (X - Z, X + Z) on the map, falls from kHeight above
// its ground there, swaying on a slow cycle of its own and carried by the
// level's wind, and is projected the way the warp places the world, so near
// flakes are larger and faster; one that lands where its place is seen
// rests there and melts. Mocked up first (tools/mockup/snow.py over state
// 0097, the user's picks from shots/snow/): the small flakes of "a", and
// per level 90 flakes falling 0.7 with no wind, 180 falling 1.5 with a
// light wind (the user asked 1.5 and a bit more drift), 260 falling 3.5
// blown 2.2 px a frame, counted over the 384x216 view as the mockup placed
// them (a density in the world: a larger view shows more). Drawn as soft
// discs over the warp, under the HUD.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "kol_focus.h"
#include "kol_rain.h"
#include "kol_settings.h"
#include "runtime.h"

namespace kol {
namespace {

struct Level {
    float flakes;   // over the mockup's view (mockup_area), calibrated (below)
    float fall;     // world units a frame (times a flake's own 0.75..1.25)
    float sway;     // world units, about its line down
    float wind;     // screen pixels a frame to the right (along X, against Z)
};
// The mockup's 90, 180 and 260 flakes showed 49, 82 and 103 in the 384x216
// view on average (state 0097); counted over this box they showed 43, 59
// and 70, so they are 1.14, 1.39 and 1.48 times as many here.
constexpr Level kLevels[3] = {{102.0f, 0.7f, 8.0f, 0.0f}, {250.0f, 1.5f, 8.0f, 0.35f}, {384.0f, 3.5f, 5.0f, 2.2f}};
constexpr float kHeight = 300.0f;     // a flake falls from this far above its ground
constexpr float kAlpha = 0.85f;
constexpr float kRadiusLo = 0.6f, kRadiusHi = 1.3f;   // pixels at scale 1
constexpr float kMelt = 30.0f;        // frames a landed flake takes to melt
constexpr std::uint8_t kRgb[3] = {226, 248, 244};     // near the game's own
// On a dark map a flake shows at the map's darkness, never under this.
constexpr float kDarkFloor = 0.35f;
constexpr int kMaxFlakes = 4000;
constexpr int kGrid = 8;              // the ground's squares, world units
// The mockup placed a flake where its landing fell within 20 columns of the
// 384-wide view (and its upwind start's travel more on the right) and its
// fall reached the view (rows -10 .. 216 + 10 + 150): 8 world (e, d) units
// a screen pixel at scale 1.
float mockup_area(const Level& lv) {
    const float travel = lv.wind * kHeight / lv.fall * 1.3f;
    return 8.0f * (424.0f + travel) * 386.0f;
}

std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
float unit(std::uint32_t& h) {
    h = mix(h + 0x6A09E667u);
    return static_cast<float>(h >> 8) / 16777216.0f;
}
// Flake i's own: its height at the start, speed, size and sway.
struct Flake {
    float h0, speed, radius, phase, period;
};
Flake flake_of(int i) {
    std::uint32_t h = mix(static_cast<std::uint32_t>(i) * 0x9E3779B1u ^ 0x5F356495u);
    Flake f;
    f.h0 = unit(h) * kHeight;
    f.speed = 0.75f + 0.5f * unit(h);
    f.radius = kRadiusLo + (kRadiusHi - kRadiusLo) * unit(h);
    f.phase = unit(h) * 6.2831853f;
    f.period = 110.0f + 120.0f * unit(h);
    return f;
}
// Its fall c's landing in the tiled box (0..1 each way): a new place every
// fall, as the rain's drops.
void landing_of(int i, int c, float* ue, float* ud) {
    std::uint32_t h = mix(static_cast<std::uint32_t>(i) * 0x27D4EB2Fu ^ static_cast<std::uint32_t>(c) * 0x165667B1u);
    *ue = unit(h);
    *ud = unit(h);
}

std::vector<Flake> g_flakes;
std::vector<float> g_xy;
std::vector<std::uint8_t> g_rgba;
std::vector<int> g_idx;
std::vector<std::int16_t> g_ground;
std::atomic<bool> g_drawn{false};
bool g_built = false;   // snow_build ran for this picture (SnowFrame)

float wrap(float v, float span) {
    const float m = std::fmod(v, span);
    return m < 0.0f ? m + span : m;
}

}  // namespace

bool snow_mockup() {
    static const bool on = [] {
        const char* e = std::getenv("KOL_SNOW_MOCKUP");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

bool snow_drawn() { return g_drawn.load(std::memory_order_relaxed); }

SnowFrame::SnowFrame() { g_built = false; }
SnowFrame::~SnowFrame() {
    if (!g_built) g_drawn.store(false, std::memory_order_relaxed);
}

void snow_build(const ScenePixels& sp, int w, int h, const RainCam& cam,
                gbarecomp::PresentWarpMesh* mesh) {
    // The snow eases in and out over kEase seconds, as the rain does, and a
    // level the game changes in a battle (map 29 has both 0 and 1) eases its
    // fall, sway, wind and count over kLevelEase: the flakes' places come
    // from a fall and a time added up a picture at a time, so nothing jumps.
    // A new map, or a jump in the frame count (a state loaded), starts it as
    // it is. KOL_SNOW_LOG=1: the level and amount a picture, and the flakes
    // in the view; KOL_SNOW_LEVEL=<0..2>: that level wherever it snows
    // (tests).
    constexpr float kEase = 1.0f, kLevelEase = 2.0f;
    static float amount = 0.0f, fallen = 0.0f, time = 0.0f;
    static Level now = kLevels[1];
    static int last_level = -1, last_map = -2;
    static unsigned long last_serial = 0;
    static const int forced = [] {
        const char* e = std::getenv("KOL_SNOW_LEVEL");
        return e && *e ? std::clamp(std::atoi(e), 0, 2) : -1;
    }();
    const int level = sp.snow_level >= 0 && forced >= 0 ? forced : sp.snow_level;
    const bool snowing = !snow_mockup() && setting_on(Setting::OwnSnow) && level >= 0 && level <= 2;
    float gap = static_cast<float>(sp.serial) - static_cast<float>(last_serial);
    const bool restart = sp.map != last_map || sp.serial < last_serial || gap > 600.0f;
    if (restart) {
        amount = snowing ? 1.0f : 0.0f;
        gap = 1.0f;
        if (snowing) now = kLevels[level];
    } else {
        amount = std::clamp(amount + (snowing ? 1.0f : -1.0f) * gap / (60.0f * kEase), 0.0f, 1.0f);
    }
    gap = std::clamp(gap, 0.0f, 4.0f);
    last_serial = sp.serial;
    last_map = sp.map;
    if (snowing) last_level = level;
    if (last_level >= 0) {
        const Level& to = kLevels[last_level];
        const float k = std::min(1.0f, gap / (60.0f * kLevelEase));
        now.flakes += (to.flakes - now.flakes) * k;
        now.fall += (to.fall - now.fall) * k;
        now.sway += (to.sway - now.sway) * k;
        now.wind += (to.wind - now.wind) * k;
    }
    fallen += now.fall * gap;
    time += gap;
    static const bool snow_log = std::getenv("KOL_SNOW_LOG") != nullptr;
    if (snow_log)
        std::fprintf(stderr, "snow: serial %lu level %d amount %.2f fall %.2f wind %.2f flakes %.0f\n", sp.serial,
                     level, amount, now.fall, now.wind, now.flakes);
    const bool on = amount > 0.0f && last_level >= 0 && sp.kind && sp.y && w > 0 && h > 0;
    g_built = true;
    g_drawn.store(on, std::memory_order_relaxed);
    if (!on) return;
    if (g_flakes.empty()) {
        g_flakes.resize(kMaxFlakes);
        for (int i = 0; i < kMaxFlakes; ++i) g_flakes[static_cast<std::size_t>(i)] = flake_of(i);
    }
    const float ease = amount * amount * (3.0f - 2.0f * amount);
    const std::uint8_t* kind = sp.kind;
    const std::int32_t* yy = sp.y;
    const std::uint8_t* lift = sp.lift;

    // The ground, as the rain finds it (kol_rain): the highest top face seen
    // in each square of the world, from the scene.
    const float ge0 = -2.0f * cam.col0 - 16.0f;
    const float gd0 = -4.0f * cam.base - 600.0f;
    const int ne = (2 * w + 32) / kGrid + 1;
    const int nd = (4 * h + 1600) / kGrid + 1;
    g_ground.assign(static_cast<std::size_t>(ne) * nd, INT16_MIN);
    double sum_y = 0.0;
    long n_y = 0;
    for (int row = 0; row < h; row += 2)
        for (int col = 0; col < w; col += 2) {
            const std::size_t k = static_cast<std::size_t>(row) * w + col;
            if (kind[k] != kSceneFlat || (lift && lift[k])) continue;
            const int Y = yy[k];
            const float e = 2.0f * (col - cam.col0);
            const float d = 4.0f * (row - cam.base) + 2.0f * Y;
            const int ie = static_cast<int>((e - ge0) / kGrid), id = static_cast<int>((d - gd0) / kGrid);
            if (ie < 0 || id < 0 || ie >= ne || id >= nd) continue;
            std::int16_t& g = g_ground[static_cast<std::size_t>(id) * ne + ie];
            g = std::max(g, static_cast<std::int16_t>(std::clamp(Y, -32000, 32000)));
            sum_y += Y;
            ++n_y;
        }
    const float y_mid = n_y ? static_cast<float>(sum_y / n_y) : cam.yref;
    auto ground_at = [&](float e, float d, bool* known) {
        const int ie = static_cast<int>(std::floor((e - ge0) / kGrid));
        const int id = static_cast<int>(std::floor((d - gd0) / kGrid));
        if (ie >= 0 && id >= 0 && ie < ne && id < nd) {
            const std::int16_t g = g_ground[static_cast<std::size_t>(id) * ne + ie];
            if (g != INT16_MIN) { *known = true; return static_cast<float>(g); }
        }
        *known = false;
        return y_mid;
    };
    auto seen = [&](float e, float d, float g) {
        const int col = static_cast<int>(std::lround(cam.col0 + e * 0.5f));
        const int row = static_cast<int>(std::lround(cam.base + d * 0.25f - g * 0.5f));
        if (col < 0 || row < 0 || col >= w || row >= h) return false;
        const std::size_t k = static_cast<std::size_t>(row) * w + col;
        return kind[k] == kSceneFlat && !(lift && lift[k]) && std::abs(static_cast<float>(yy[k]) - g) <= 6.0f;
    };
    // The warp's placing (kol_perspective corner_at, row_drawn), in the
    // view's pixels, as kol_rain's.
    auto project = [&](float e, float d, float Y, float* x, float* y) {
        const float col = cam.col0 + e * 0.5f;
        float s = 1.0f;
        if (cam.flat) {
            *x = col;
            *y = cam.base + d * 0.25f - Y * 0.5f;
        } else {
            const float den = cam.K + cam.dref - d;
            s = den > 0.25f * cam.K ? cam.K / den : 4.0f;
            *x = cam.cxp + (col - cam.cxp) * s;
            *y = cam.base + (cam.dref + (d - cam.dref) * s * cam.pitch) * 0.25f -
                 (cam.yref + (Y - cam.yref) * s) * 0.5f;
            if (cam.zoom != 1.0f) {
                *x = cam.cx + (*x - cam.cx) * cam.zoom;
                *y = cam.cy + (*y - cam.cy) * cam.zoom;
                s *= cam.zoom;
            }
        }
        *x -= static_cast<float>(cam.vx);
        *y -= static_cast<float>(cam.vy);
        return s;
    };

    // The box the landings are tiled over, as the rain's (wide enough for
    // the far side's shrink, from above the view down to landings whose
    // falls still reach it), and on the wind's side the way the slowest
    // flake is carried while it falls.
    const float travel = now.wind * kHeight / now.fall * 1.3f;   // screen pixels
    const float span_e0 = 2.0f * (cam.vw / 0.6f + 160.0f);
    const float span_e = span_e0 + 2.0f * travel;
    const float e_lo = 2.0f * (cam.cxp - cam.col0) - span_e0 * 0.5f;
    const float r_top = cam.vy - 0.4f * cam.vh - 60.0f;
    const float r_bot = cam.vy + cam.vh + 0.6f * kHeight + 60.0f;
    const float span_d = 4.0f * (r_bot - r_top);
    const float d_lo = 4.0f * (r_top - cam.base) + 2.0f * cam.yref;
    const float count_f = std::min(static_cast<float>(kMaxFlakes),
                                   ease * now.flakes * span_e * span_d / mockup_area(now));
    const int count = std::min(kMaxFlakes, static_cast<int>(std::ceil(count_f)));
    const float bright = std::clamp(sp.scene_bright, 0.0f, 1.0f) * (sp.dark < 1.0f ? std::max(sp.dark, kDarkFloor) : 1.0f);
    const float vw = static_cast<float>(cam.vw), vh = static_cast<float>(cam.vh);

    g_xy.clear();
    g_rgba.clear();
    g_idx.clear();
    auto vert = [&](float x, float y, float a) {
        g_xy.push_back(x);
        g_xy.push_back(y);
        g_rgba.insert(g_rgba.end(), {kRgb[0], kRgb[1], kRgb[2],
                                     static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(a, 0.0f, 1.0f)))});
        return static_cast<int>(g_xy.size() / 2 - 1);
    };
    auto tri = [&](int a, int b, int c) { g_idx.insert(g_idx.end(), {a, b, c}); };
    // A soft round flake: full out to r - 0.5, nothing past r + 0.5; one
    // under a pixel across is fainter rather than a hard dot.
    auto disc = [&](float x, float y, float r, float a) {
        if (r < 1.0f) a *= r;
        constexpr int kSegs = 8;
        const float ri = std::max(0.0f, r - 0.5f), ro = r + 0.5f;
        const int c = vert(x, y, a);
        int first = -1;
        for (int j = 0; j < kSegs; ++j) {
            const float an = 6.2831853f * static_cast<float>(j) / kSegs;
            const float cs = std::cos(an), sn = std::sin(an);
            const int in = vert(x + ri * cs, y + ri * sn, a);
            vert(x + ro * cs, y + ro * sn, 0.0f);
            if (j == 0) { first = in; continue; }
            const int pin = in - 2;
            tri(c, pin, in);
            tri(pin, pin + 1, in);
            tri(pin + 1, in + 1, in);
        }
        const int last = first + 2 * (kSegs - 1);
        tri(c, last, first);
        tri(last, last + 1, first);
        tri(last + 1, first + 1, first);
    };
    // Inside the map's faded edge only (kEdgeFadeMaps; map 64's round fade,
    // the user, 2026-11-10, asked for snow only inside the circle): a flake
    // as faint as the fade where it is drawn, none past it. KOL_SNOW_OVER_EDGE=1:
    // everywhere, as before.
    static const bool over_edge = std::getenv("KOL_SNOW_OVER_EDGE") != nullptr;
    auto in_fade = [&](float x, float y) {
        if (over_edge || !sp.edge_fade) return 1.0f;
        const int col = static_cast<int>(std::lround(x)) + cam.vx, row = static_cast<int>(std::lround(y)) + cam.vy;
        if (col < 0 || row < 0 || col >= w || row >= h) return 0.0f;
        const std::size_t k = static_cast<std::size_t>(row) * w + col;
        if (sp.kind[k] == kSceneNone) return 0.0f;   // (past the art: the fade's own grid stops there, the sky's black)
        return static_cast<float>(sp.edge_fade[k]) / 255.0f;
    };
    constexpr float kTwoPi = 6.2831853f;
    int drawn = 0;
    for (int i = 0; i < count; ++i) {
        const Flake& fl = g_flakes[static_cast<std::size_t>(i)];
        // The last flakes of the count come and go by degrees.
        const float share = std::clamp(count_f - static_cast<float>(i), 0.0f, 1.0f);
        const float v = now.fall * fl.speed;
        const float p = fl.h0 - fl.speed * fallen;
        const int c = static_cast<int>(std::floor(p / kHeight));   // which fall
        const float alt = p - static_cast<float>(c) * kHeight;
        // Where it sways to at time `at` (X and Z), as e and d.
        auto sway = [&](float at, float* de, float* dd) {
            const float sx = now.sway * std::sin(fl.phase + kTwoPi * at / fl.period);
            const float sz = 0.6f * now.sway * std::sin(fl.phase * 1.7f + kTwoPi * at / (fl.period * 1.3f));
            *de = sx - sz;
            *dd = sx + sz;
        };
        float ue, ud;
        landing_of(i, c, &ue, &ud);
        const float e = e_lo + wrap(ue * span_e - e_lo, span_e);
        const float d = d_lo + wrap(ud * span_d - d_lo, span_d);
        bool known = false;
        const float g = ground_at(e, d, &known);
        // Still upwind by what the wind carries it before it lands.
        const float up = now.wind * alt / v;
        float se, sd;
        sway(time, &se, &sd);
        float x, y;
        const float s = project(e + se - 2.0f * up, d + sd, g + alt, &x, &y);
        const float r = fl.radius * s;
        if (x > -r - 2.0f && x < vw + r + 2.0f && y > -r - 2.0f && y < vh + r + 2.0f) {
            disc(x, y, r, kAlpha * std::min(1.0f, s) * bright * share * in_fade(x, y));
            ++drawn;
        }
        // The fall before's flake, landed (kHeight - alt) / v frames ago,
        // resting where it landed and melting.
        const float age = (kHeight - alt) / v;
        if (age >= kMelt) continue;
        landing_of(i, c + 1, &ue, &ud);
        const float le = e_lo + wrap(ue * span_e - e_lo, span_e);
        const float ld = d_lo + wrap(ud * span_d - d_lo, span_d);
        const float lg = ground_at(le, ld, &known);
        if (!known || !seen(le, ld, lg)) continue;
        sway(time - age, &se, &sd);
        float gx, gy;
        const float gs = project(le + se, ld + sd, lg, &gx, &gy);
        if (gx < -4.0f || gx > vw + 4.0f || gy < -4.0f || gy > vh + 4.0f) continue;
        disc(gx, gy, 0.9f * fl.radius * gs, kAlpha * (1.0f - age / kMelt) * std::min(1.0f, gs) * bright * share * in_fade(gx, gy));
    }
    if (snow_log) std::fprintf(stderr, "snow: %d of %d flakes in the view\n", drawn, count);
    if (g_idx.empty()) return;
    mesh->over_xy = g_xy.data();
    mesh->over_rgba = g_rgba.data();
    mesh->over_vertices = static_cast<int>(g_xy.size() / 2);
    mesh->over_indices = g_idx.data();
    mesh->over_index_count = static_cast<int>(g_idx.size());
}

}  // namespace kol
