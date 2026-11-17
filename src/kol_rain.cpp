#include "kol_rain.h"

// Our own rain (asked 2026-10-26 to hide the game's rain and draw our own).
// The game draws ten 8x8 drops in screen space; with the perspective they
// stayed flat over a moving picture. These drops live in the world: each
// has a landing place (X - Z, X + Z) fixed on the map, falls from 420 units
// above the ground there along a slight slant, and is projected the way the
// warp places the world, so near drops are larger and faster. Where its
// landing is seen (a top face at that height, nothing lifted over it) it
// leaves a ring and two droplets. Mocked up first (tools/mockup/rain.py, the
// user's picks): intensity 0 = 130 drops, 1 = 300, 2 = 600 with twice the
// slant, counted over the 384x216 view (a density in the world, so a larger
// view shows more). Drawn as soft lines over the warp, under the HUD.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "kol_env.h"
#include "kol_focus.h"
#include "kol_map_settings.h"
#include "kol_settings.h"
#include "runtime.h"

namespace kol {
namespace {

struct Level {
    float drops;   // over the mockup's 384x216 view (kMockupArea)
    float lean;    // times kSlant
};
constexpr Level kLevels[3] = {{130.0f, 1.0f}, {300.0f, 1.0f}, {600.0f, 2.2f}};
// Slower than the mockup's 22 (the user on the Thor found the rain too fast,
// too uniform in direction and not random enough), each drop its
// own speed, each fall its own place, slant, length and strength, and the
// wind gusting.
constexpr float kFall = 13.0f;        // world units a frame (times a drop's own 0.7..1.3)
constexpr float kStreak = 28.0f;      // the streak's length, world units up (times 0.7..1.3 a fall)
constexpr float kAlpha = 0.38f;       // (times 0.7..1.2 a fall)
// A fall's slant: the wind's times 1 +- kSlantSpread, then +- kSlantJitter
// per unit up (so even a calm rain is not all one way).
constexpr float kSlantSpread = 0.4f;
constexpr float kSlantJitter = 0.06f;
constexpr std::uint8_t kRgb[3] = {214, 222, 234};
// Lit by the lights around it (asked 2026-10-26 whether the rain's transparency
// could follow the light): on a dark map a drop away from every light shows at
// the map's darkness (ScenePixels::dark), but never under kLitFloor, one
// near a torch more, up to kLitMax times as strong, and takes the light's
// colour (torch_tint's hues); a light reaches kLitReach times further in the
// air than on the ground; a white flash lights every drop. Day maps
// unchanged. The user's pick of three stills (JM): floor 0.35 and reach 1.75
// over the map's own 0.2 and 1 (only ~14 of 222 streaks were lit, the rest
// all but gone). KOL_RAIN_LIGHT=0: as before.
constexpr float kLitMax = 1.3f;
constexpr float kLitFloor = 0.35f;
constexpr float kLitReach = 1.75f;
constexpr float kLitTint = 0.6f;      // the light's colour, at most this share
constexpr float kLampRgb[kol::kHueCount][3] = {{255.0f, 165.0f, 80.0f}, {110.0f, 170.0f, 255.0f}, {255.0f, 235.0f, 200.0f},
                                               {185.0f, 205.0f, 245.0f}, {255.0f, 120.0f, 50.0f},
                                               {156.0f, 219.0f, 120.0f},    // (the green at 60% saturation)
                                               {255.0f, 200.0f, 140.0f}};   // the lantern (halfway from the torches' to daylight's)
constexpr float kHeight = 420.0f;     // a drop falls from this far above its ground
// The slant: the streak's top 3 units further along X and 3 back along Z
// per 34 up, so it leans right on screen (X - Z only).
constexpr float kSlant = 6.0f / 34.0f;
constexpr float kSplashFrames = 6.0f;
// The mockup counted the drops whose landing fell within 20 columns of the
// 384-wide view and whose streak reached it (rows -10 .. 216 + 220): 424 x
// 446 screen pixels, 8 world (e, d) units each at scale 1.
constexpr float kMockupArea = 8.0f * 424.0f * 446.0f;
constexpr int kMaxDrops = 6000;
constexpr int kGrid = 8;              // the ground's squares, world units

struct Drop {
    float h0;       // its height at frame 0
    float speed;
};

// Drop i's fall c: its landing in the tiled box (0..1 each way), its slant
// about the wind's, its length and its strength. A new place every fall: a
// drop that always came down on the same spot made a steady pattern.
struct Fall {
    float ue, ud, spread, slant, len, alpha;
};
std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
Fall fall_of(int i, int c) {
    std::uint32_t h = mix(static_cast<std::uint32_t>(i) * 0x9E3779B1u ^ static_cast<std::uint32_t>(c) * 0x27D4EB2Fu);
    auto next = [&h]() {
        h = mix(h + 0x6A09E667u);
        return static_cast<float>(h >> 8) / 16777216.0f;
    };
    Fall f;
    f.ue = next();
    f.ud = next();
    f.spread = 1.0f + (next() * 2.0f - 1.0f) * kSlantSpread;
    f.slant = (next() * 2.0f - 1.0f) * kSlantJitter;
    f.len = 0.7f + 0.6f * next();
    f.alpha = 0.7f + 0.5f * next();
    return f;
}

std::vector<Drop> g_drops;
std::vector<float> g_xy;
std::vector<std::uint8_t> g_rgba;
std::vector<int> g_idx;
std::vector<std::int16_t> g_ground;
std::atomic<bool> g_drawn{false};
bool g_built = false;   // rain_build ran for this picture (RainFrame)
std::uint32_t g_tick = 0;

void make_drops() {
    if (!g_drops.empty()) return;
    std::uint32_t r = 0x9E3779B9u;
    auto next = [&r]() {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return static_cast<float>(r >> 8) / 16777216.0f;
    };
    g_drops.resize(kMaxDrops);
    for (Drop& d : g_drops) {
        d.h0 = next() * kHeight;
        d.speed = 0.7f + 0.6f * next();
    }
}

float wrap(float v, float span) {
    const float m = std::fmod(v, span);
    return m < 0.0f ? m + span : m;
}

}  // namespace

bool rain_mockup() {
    static const bool on = [] {
        const char* e = std::getenv("KOL_RAIN_MOCKUP");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

bool rain_drawn() { return g_drawn.load(std::memory_order_relaxed); }

RainFrame::RainFrame() { g_built = false; }
RainFrame::~RainFrame() {
    if (!g_built) g_drawn.store(false, std::memory_order_relaxed);
}

void rain_build(const ScenePixels& sp, int w, int h, const RainCam& cam,
                gbarecomp::PresentWarpMesh* mesh) {
    mesh->over_xy = nullptr;
    mesh->over_rgba = nullptr;
    mesh->over_vertices = 0;
    mesh->over_indices = nullptr;
    mesh->over_index_count = 0;
    static const bool log_cam = std::getenv("KOL_RAIN_CAM") != nullptr;
    if (log_cam)   // the camera, for tools/mockup/rain.py
        std::fprintf(stderr, "raincam serial %lu base %.4f dref %.4f yref %.4f K %.4f pitch %.4f zoom %.4f cx %.2f cy %.2f cxp %.2f view %d %d %d %d wh %d %d kx %d ky %d ox %d oy %d\n",
                     sp.serial, cam.base, cam.dref, cam.yref, cam.K, cam.pitch, cam.zoom, cam.cx, cam.cy, cam.cxp,
                     cam.vx, cam.vy, cam.vw, cam.vh, w, h, sp.kx, sp.ky, sp.ox, sp.oy);
    // The rain eases out when the game's stops and in when it comes back,
    // over kEase seconds (the user, 2026-10-26, a spell on slot 3: the rain's brief
    // stop was too sudden; asked for it to ease out and back in, even over
    // about a second): its drops thin out (and come back) in their own order, at the
    // last level the game had. A new map, or a jump in the frame count (a
    // state loaded), starts it as it is. KOL_RAIN_LOG=1: the amount a picture.
    constexpr float kEase = 1.0f;
    static float amount = 0.0f;
    static int last_level = -1, last_map = -2;
    static unsigned long last_serial = 0;
    const bool raining = !rain_mockup() && setting_on(Setting::OwnRain) && sp.rain_level >= 0 && sp.rain_level <= 2;
    {
        const float gap = static_cast<float>(sp.serial) - static_cast<float>(last_serial);
        if (sp.map != last_map || sp.serial < last_serial || gap > 600.0f) amount = raining ? 1.0f : 0.0f;
        else amount = std::clamp(amount + (raining ? 1.0f : -1.0f) * gap / (60.0f * kEase), 0.0f, 1.0f);
        last_serial = sp.serial;
        last_map = sp.map;
        if (raining) last_level = sp.rain_level;
    }
    static const bool rain_log = std::getenv("KOL_RAIN_LOG") != nullptr;
    if (rain_log) std::fprintf(stderr, "rain: serial %lu level %d amount %.2f\n", sp.serial, sp.rain_level, amount);
    const bool on = amount > 0.0f && last_level >= 0 && sp.kind && sp.y && w > 0 && h > 0;
    g_built = true;
    g_drawn.store(on, std::memory_order_relaxed);
    if (!on) return;
    make_drops();
    const Level& lv = kLevels[last_level];
    const float ease = amount * amount * (3.0f - 2.0f * amount);   // smooth at both ends
    const float t = static_cast<float>(g_tick++ % 0x100000u);
    const std::uint8_t* kind = sp.kind;
    const std::int32_t* yy = sp.y;
    const std::uint8_t* lift = sp.lift;

    // The ground: the highest top face seen in each square of the world, from
    // the scene (a top face's pixel is back in the world by its height).
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
    // A landing is seen where the flat frame's pixel there is that place.
    auto seen = [&](float e, float d, float g) {
        const int col = static_cast<int>(std::lround(cam.col0 + e * 0.5f));
        const int row = static_cast<int>(std::lround(cam.base + d * 0.25f - g * 0.5f));
        if (col < 0 || row < 0 || col >= w || row >= h) return false;
        const std::size_t k = static_cast<std::size_t>(row) * w + col;
        return kind[k] == kSceneFlat && !(lift && lift[k]) && std::abs(static_cast<float>(yy[k]) - g) <= 6.0f;
    };
    // The warp's placing (kol_perspective corner_at, row_drawn), in the
    // view's pixels.
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

    // The box the drops are tiled over: wide enough for the far side's
    // shrink, from above the view down to landings whose streaks still
    // reach it. Its size depends on the view only, so the count holds.
    const float span_e = 2.0f * (cam.vw / 0.6f + 160.0f);
    const float e_lo = 2.0f * (cam.cxp - cam.col0) - span_e * 0.5f;
    const float r_top = cam.vy - 0.4f * cam.vh - 60.0f;
    const float r_bot = cam.vy + cam.vh + 0.6f * kHeight + 60.0f;
    const float span_d = 4.0f * (r_bot - r_top);
    const float d_lo = 4.0f * (r_top - cam.base) + 2.0f * cam.yref;
    const int count = std::min(kMaxDrops, static_cast<int>(std::lround(ease * lv.drops * span_e * span_d / kMockupArea)));
    // The wind's slant, gusting over some seconds.
    const float gust = 1.0f + 0.45f * std::sin(t * 0.011f) + 0.25f * std::sin(t * 0.029f + 1.3f);
    const float wind = kSlant * lv.lean * gust;
    const float bright = std::clamp(sp.scene_bright, 0.0f, 1.0f);
    const float vw = static_cast<float>(cam.vw), vh = static_cast<float>(cam.vh);

    g_xy.clear();
    g_rgba.clear();
    g_idx.clear();
    // The light on what is drawn next (light_at): its colour, and how much
    // of its strength it shows.
    std::uint8_t cur[3] = {kRgb[0], kRgb[1], kRgb[2]};
    static const bool lit_on = [] {
        const char* e = std::getenv("KOL_RAIN_LIGHT");
        return !(e && e[0] == '0');
    }();
    const bool dark_map = lit_on && sp.dark < 1.0f;
    float flash = 0.0f;   // a white field over the scene (the thunder)
    if (lit_on && sp.field_layer >= 0) {
        const float lum = 0.3f * sp.field_rgb[0] + 0.55f * sp.field_rgb[1] + 0.15f * sp.field_rgb[2];
        if (lum > 200.0f) flash = std::clamp(sp.field_eva / 16.0f, 0.0f, 1.0f);
    }
    auto light_at = [&](float e, float d, float Y) {
        cur[0] = kRgb[0]; cur[1] = kRgb[1]; cur[2] = kRgb[2];
        if (!dark_map && flash <= 0.0f) return 1.0f;
        float vis = 1.0f, rgb[3] = {kRgb[0], kRgb[1], kRgb[2]};
        if (dark_map) {
            const float X = (d + e) * 0.5f, Z = (d - e) * 0.5f;
            float sum = 0.0f, lc[3] = {0.0f, 0.0f, 0.0f};
            for (int i = 0; i < sp.lamp_count; ++i) {
                const SceneLamp& L = sp.lamps[i];
                const float dx = X - L.x, dy = Y - L.y, dz = Z - L.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float reach = L.reach * kLitReach;
                if (dist >= reach) continue;
                const float l = L.power * (1.0f - dist / reach) * L.flick;
                // (The green as torch_tint's, KOL_GREEN_SAT.)
                static float green[3];
                static const bool green_ready = [] {
                    const float sat = kol::env_num_clamp("KOL_GREEN_SAT", 0.0f, 1.0f, 1.0f);
                    const float* g = kLampRgb[kol::kHueGreen];
                    const float mean = (g[0] + g[1] + g[2]) / 3.0f;
                    for (int ch = 0; ch < 3; ++ch) green[ch] = mean + sat * (g[ch] - mean);
                    return true;
                }();
                (void)green_ready;
                const int hue = std::clamp(L.hue, 0, kol::kHueCount - 1);
                const float* c = hue == kol::kHueGreen ? green : kLampRgb[hue];
                sum += l;
                for (int ch = 0; ch < 3; ++ch) lc[ch] += l * c[ch];
            }
            const float base = std::max(sp.dark, kLitFloor);
            const float lift = std::max(0.0f, std::min(kLitMax - base, sp.dark_gain * sum));
            vis = base + lift;
            if (sum > 0.0f) {
                const float share = std::min(1.0f, sp.dark_gain * sum) * kLitTint;
                for (int ch = 0; ch < 3; ++ch) rgb[ch] += (lc[ch] / sum - rgb[ch]) * share;
            }
        }
        if (flash > 0.0f) {
            vis = std::max(vis, flash * kLitMax);
            for (int ch = 0; ch < 3; ++ch) rgb[ch] += (255.0f - rgb[ch]) * flash;
        }
        for (int ch = 0; ch < 3; ++ch) cur[ch] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[ch], 0.0f, 255.0f)));
        return vis;
    };
    auto vert = [&](float x, float y, float a) {
        g_xy.push_back(x);
        g_xy.push_back(y);
        g_rgba.insert(g_rgba.end(), {cur[0], cur[1], cur[2],
                                     static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(a, 0.0f, 1.0f)))});
        return static_cast<int>(g_xy.size() / 2 - 1);
    };
    auto tri = [&](int a, int b, int c) { g_idx.insert(g_idx.end(), {a, b, c}); };
    // A soft dot: full at its middle, nothing a pixel away.
    auto dot = [&](float x, float y, float a) {
        const int c = vert(x, y, a);
        const int l = vert(x - 1.0f, y, 0.0f), r = vert(x + 1.0f, y, 0.0f);
        const int u = vert(x, y - 1.0f, 0.0f), b = vert(x, y + 1.0f, 0.0f);
        tri(c, l, u); tri(c, u, r); tri(c, r, b); tri(c, b, l);
    };

    constexpr float kRiseSpeed = 0.4f;
    const bool rises = map_flag(MapFlag::RainRises, sp.map);
    for (int i = 0; i < count; ++i) {
        const Drop& dr = g_drops[static_cast<std::size_t>(i)];
        // Rising (map_settings.cfg `rain_rises`: map 57, where the game's own rain
        // goes up; the user, 2026-11-10, asked for ours to match, all of it
        // slower and never falling): at kRiseSpeed of its fall's speed, its streak's
        // bright end on top, no landing.
        const float v = kFall * dr.speed * (rises ? kRiseSpeed : 1.0f);
        const float p = rises ? dr.h0 + v * t : dr.h0 - v * t;
        const int c = static_cast<int>(std::floor(p / kHeight));   // which fall
        const float alt = p - static_cast<float>(c) * kHeight;
        const Fall f = fall_of(i, c);
        const float e = e_lo + wrap(f.ue * span_e - e_lo, span_e);
        const float d = d_lo + wrap(f.ud * span_d - d_lo, span_d);
        bool known = false;
        const float g = ground_at(e, d, &known);
        const float slant = wind * f.spread + f.slant, len = kStreak * f.len;
        float hx, hy, tx, ty;
        const float s = project(e + slant * alt, d, g + alt, &hx, &hy);
        project(e + slant * (alt + len), d, g + alt + len, &tx, &ty);
        if (std::max(hx, tx) > -2.0f && std::min(hx, tx) < vw + 2.0f && hy > -2.0f && ty < vh + 2.0f) {
            // A line a pixel wide, soft across, fading up to its top.
            const float lit = light_at(e + slant * (alt + 0.5f * len), d, g + alt + 0.5f * len);
            const float a = kAlpha * f.alpha * std::min(1.0f, s) * std::min(1.6f, s) * bright * lit;
            const int h0 = vert(hx, hy, rises ? 0.0f : a), t0 = vert(tx, ty, rises ? a : 0.0f);
            const int hl = vert(hx - 1.0f, hy, 0.0f), hr = vert(hx + 1.0f, hy, 0.0f);
            const int tl = vert(tx - 1.0f, ty, 0.0f), tr = vert(tx + 1.0f, ty, 0.0f);
            tri(hl, h0, tl); tri(h0, t0, tl); tri(h0, hr, t0); tri(hr, tr, t0);
        }
        if (rises) continue;
        // Its last landing, the fall before's, (kHeight - alt) / v frames ago.
        const float age = (kHeight - alt) / v;
        if (age >= kSplashFrames) continue;
        const Fall lf = fall_of(i, c + 1);
        const float le = e_lo + wrap(lf.ue * span_e - e_lo, span_e);
        const float ld = d_lo + wrap(lf.ud * span_d - d_lo, span_d);
        const float lg = ground_at(le, ld, &known);
        if (!known) continue;
        float gx, gy;
        const float gs = project(le, ld, lg, &gx, &gy);
        if (gx < -8.0f || gx > vw + 8.0f || gy < -8.0f || gy > vh + 8.0f || !seen(le, ld, lg)) continue;
        const float fade = 1.0f - age / kSplashFrames;
        // A ring on the ground, spreading.
        const float r = (1.2f + age * 0.9f) * gs;
        const float lit = light_at(le, ld, lg + 2.0f);
        const float ar = std::min(0.33f, 0.98f / std::max(r, 0.1f)) * fade * bright * lit;
        constexpr int kSegs = 16;
        int first = -1;
        for (int j = 0; j <= kSegs; ++j) {
            if (j == kSegs) {
                const int pin = first, pmid = first + 1, pout = first + 2;
                const int in = static_cast<int>(g_xy.size() / 2) - 3;
                tri(in, in + 1, pin); tri(in + 1, pmid, pin);
                tri(in + 1, in + 2, pmid); tri(in + 2, pout, pmid);
                break;
            }
            const float an = 6.2831853f * j / kSegs, c = std::cos(an), sn = std::sin(an);
            const int in = vert(gx + std::max(0.0f, r - 1.0f) * c, gy + std::max(0.0f, 0.5f * r - 1.0f) * sn, 0.0f);
            vert(gx + r * c, gy + 0.5f * r * sn, ar);
            vert(gx + (r + 1.0f) * c, gy + (0.5f * r + 1.0f) * sn, 0.0f);
            if (j == 0) { first = in; continue; }
            const int pin = in - 3;
            tri(pin, pin + 1, in); tri(pin + 1, in + 1, in);
            tri(pin + 1, pin + 2, in + 1); tri(pin + 2, in + 2, in + 1);
        }
        if (age < 3.0f) {   // two droplets thrown up
            const float up = (age * 2.2f - age * age * 0.5f) * gs;
            const float a = 0.55f * fade * bright * lit;
            dot(gx - 1.6f * gs - age * 0.6f, gy - up, a);
            dot(gx + 1.6f * gs + age * 0.6f, gy - up * 0.8f, a);
        }
    }
    if (g_idx.empty()) return;
    mesh->over_xy = g_xy.data();
    mesh->over_rgba = g_rgba.data();
    mesh->over_vertices = static_cast<int>(g_xy.size() / 2);
    mesh->over_indices = g_idx.data();
    mesh->over_index_count = static_cast<int>(g_idx.size());
}

}  // namespace kol
