#include "kol_dev.h"
#include "kol_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kol_actions.h"
#include "kol_hd_audio.h"
#include "kol_highlight.h"
#include "kol_obj_cull.h"
#include "kol_widescreen.h"
#include "rom_patch.h"

namespace gbarecomp {
extern "C" void runtime_request_view_size(unsigned width, unsigned height);
}
namespace gba {                            // gba_audio.h
void gba_audio_request_hq_mode(int mode);
void gba_audio_request_mix_volumes(int music_percent, int effects_percent);
void gba_audio_request_psg_soften(bool on);
extern "C" unsigned g_ppu_debug_hide_layers;   // gba_ppu.h
}

namespace kol {
namespace {

constexpr int kCount = static_cast<int>(Setting::Count);

// Depth of field and perspective start off: they are experimental and cost
// CPU every frame. Unit shadows start on: they only act with the shadows or
// the occlusion on, which start off. The water starts on: it only acts
// where the scene is composed (the perspective or the light). Plant sway
// starts on: it only acts on lifted props. The new rain starts on: it only
// acts where the scene is composed.
bool g_on[kCount] = {true, true, true, false, false, false, true, false, false, false,
                     false, false, false, false, false, false, false, false, true,
                     false, false, false, false, false, false, true, true, true, true,
                     false, false, false, false, false, true, true, true, false, false, true, true, true,
                     false, false, false, true};
// The layer switches are for the moment: never saved, never loaded.
bool persisted(int i) {
    return i < static_cast<int>(Setting::DebugHideBG0) || i > static_cast<int>(Setting::DebugHideOBJ);
}
std::string g_dir;
int g_file_version = -1;   // enhancements.cfg's settings_version= (0 a file without it), -1 no file
unsigned g_widescreen_width = 320;
unsigned g_widescreen_height = 160;
bool g_settings_ready = false;
// The depth-of-field strengths, radius in frame pixels. 1.5 is the default,
// the user's pick from the mockups on 2026-09-26.
struct BlurChoice { float radius; const char* label; };
const BlurChoice kBlurChoices[] = {
    {1.0f, "Subtle"}, {1.5f, "Medium"}, {2.0f, "Strong"}, {2.5f, "Very strong"}};
constexpr unsigned kBlurChoiceCount = sizeof(kBlurChoices) / sizeof(kBlurChoices[0]);
unsigned g_blur_choice = 1;

// The music's quality, saved as music_quality= (0..2). Each asks gbarecomp
// for an audio mode (gba_audio_request_hq_mode): HQ is the stereo render
// (the mono one left the menu on 2026-10-18), HD the stereo render with the
// HD instruments, which falls back to HQ when the bank is missing.
const char* const kSoundChoices[] = {"Original (hardware mix)",
                                     "HQ (high quality, stereo)",
                                     "HD (high-definition instruments)"};
constexpr int kSoundModes[] = {0, 2, 3};
constexpr unsigned kSoundChoiceCount = sizeof(kSoundChoices) / sizeof(kSoundChoices[0]);
constexpr unsigned kSoundHD = 2;
unsigned g_sound_choice = 1;
// Ambient occlusion on the perspective battlefield: off, or shade reaching
// half a tile or a whole one (16, 32 world units) out from higher ground.
const char* const kAoChoices[] = {"Off", "Half tile", "Full tile"};
constexpr unsigned kAoChoiceCount = sizeof(kAoChoices) / sizeof(kAoChoices[0]);
unsigned g_ao_choice = 0;
// Rumble (kol_rumble): off, or the device's vibrator / the controller's
// motors at a strength; saved as rumble=. Medium unless chosen.
const char* const kRumbleChoices[] = {"Off", "Light", "Medium", "Strong"};
constexpr float kRumbleStrength[] = {0.0f, 0.4f, 0.7f, 1.0f};
constexpr unsigned kRumbleChoiceCount = sizeof(kRumbleChoices) / sizeof(kRumbleChoices[0]);
unsigned g_rumble_choice = 2;
// Shadows cast by the terrain: off or on. The sun is 36 degrees above the
// ground (its direction is kol_focus shadow_dir_pick's) unless the map has
// its own sun (kol_focus kMapSuns, the editor's `sun` lines): the sun the
// user had on the Thor on map 9, read from its log on 2026-10-11 (`sun: ...
// KOL_PERSP_SHADOW_ELEV=36.0 KOL_PERSP_SHADOW_DIR=0.585,-0.811`), when the
// former five-way pick left the menu.
const char* const kShadowChoices[] = {"Off", "On"};
constexpr unsigned kShadowChoiceCount = sizeof(kShadowChoices) / sizeof(kShadowChoices[0]);
// Since 2026-10-12 it starts at 60 on the first turn and sinks 2.5 a turn
// (kol_focus sun_turn_drop); it was 36 all battle, then 36 less 1 a turn.
constexpr float kShadowSunDegrees = 60.0f;
// (KOL_SHADOWS=<n> turns it on (n != 0) for a desktop test run, which loads
// no enhancements.cfg; the old 1..4 picks all mean on.)
unsigned g_shadow_choice = [] {
    const char* e = std::getenv("KOL_SHADOWS");
    return e && std::atoi(e) != 0 ? 1u : 0u;
}();
unsigned g_music_volume = 10, g_effects_volume = 10;   // x 10%
// Read on the game thread (the map hook), set from the menu's.
std::atomic<unsigned> g_debug_map{0};
// The map headers' count, the u16 at 0x085C55C4 (headers follow it).
constexpr unsigned kMapCount = 119;
bool g_second_screen_wanted = true;
// Set by the Android second-screen thread: a presentation display is up.
std::atomic<bool> g_second_screen_present{false};

// ── The width ladder ──────────────────────────────────────────────────────
// The view sizes of one shape (the ratio below) plus the authentic 240x160,
// built from the panel's own geometry so each is labelled by how it scales
// into it. 480x280 is the PPU's compile-time ceiling (kMaxRenderWidth,
// kMaxRenderHeight).
struct WidthChoice {
    unsigned width = 0;
    unsigned height = 160;
    char label[56] = {0};
    bool whole = true;   // scales into the panel by a whole number (or the panel is unknown)
};
constexpr unsigned kMaxChoices = 8;
// Mirrors GbaPpu's caps. A size the PPU would clamp must not be offered, or
// the menu promises a size it cannot render.
constexpr unsigned kMaxViewWidth = 480, kMaxViewHeight = 280;
WidthChoice g_choices[kMaxChoices];
unsigned g_choice_count = 0;

void add_choice(unsigned width, unsigned height, const char* how) {
    if (width < 240u || width > kMaxViewWidth || height < 160u || height > kMaxViewHeight) return;
    for (unsigned i = 0; i < g_choice_count; ++i)
        if (g_choices[i].width == width && g_choices[i].height == height) return;
    if (g_choice_count >= kMaxChoices) return;
    WidthChoice& c = g_choices[g_choice_count++];
    c.width = width;
    c.height = height;
    c.whole = std::strcmp(how, "uneven pixels") != 0;
    std::snprintf(c.label, sizeof c.label, "%ux%u   %s", width, height, how);
}

// ── The picture's shape (Options > Display > Ratio) ───────────────────────
// Asked 2026-11-21: a ratio option, 1:1, 4:3, 3:2, 16:10 or 16:9, the one
// that suits the screen chosen by itself; the view sizes (and a desktop's
// window sizes) follow it. Until then the list was 16:9 everywhere (asked
// 2026-11-17), 16:10 on a 16:10 computer (the Deck, 2026-11-18).
//
// Each shape's sizes: the renderer's range (240..480 wide, 160..280 tall),
// ending at 270 or 280 lines; 4:3 and 1:1 are taller rather than wider (at
// 160 lines they would be narrower than the game). 240x240 is the only one
// at the game's own width: margins above and below only (checked: B_rictor
// renders whole at 240x240, 256x192 and 280x280). The authentic 240x160
// (itself 3:2) comes first in every list.
struct Size { unsigned w, h; };
struct Ratio {
    const char* label;
    unsigned num, den;
    Size sizes[6];   // ascending; {0, 0} ends the list
};
const Ratio kRatios[] = {
    {"1:1", 1, 1, {{240u, 240u}, {256u, 256u}, {270u, 270u}, {280u, 280u}}},
    {"4:3", 4, 3, {{256u, 192u}, {288u, 216u}, {320u, 240u}, {360u, 270u}}},
    {"3:2", 3, 2, {{270u, 180u}, {300u, 200u}, {324u, 216u}, {360u, 240u}, {420u, 280u}}},
    {"16:10", 16, 10, {{256u, 160u}, {320u, 200u}, {384u, 240u}, {432u, 270u}}},
    {"16:9", 16, 9, {{284u, 160u}, {320u, 180u}, {384u, 216u}, {480u, 270u}}},
};
constexpr unsigned kRatioCount = sizeof(kRatios) / sizeof(kRatios[0]);
constexpr unsigned kRatio169 = kRatioCount - 1u;   // with no panel known
unsigned g_ratio = kRatio169;
bool g_ratio_chosen = false;            // the player picked it (saved as ratio=)
unsigned g_panel_w = 0, g_panel_h = 0;  // the display, landscape; 0 unknown

// The shape nearest the panel's (by the ratio of the ratios, so 16:9 is as
// far from 2:1 as 2:1 is from 2.25:1). A 20:9 phone is nearest 16:9.
unsigned ratio_for_panel(unsigned w, unsigned h) {
    if (w == 0u || h == 0u) return kRatio169;
    const double r = std::log(static_cast<double>(w) / h);
    unsigned best = kRatio169;
    double best_d = 1e9;
    for (unsigned i = 0; i < kRatioCount; ++i) {
        const double d = std::fabs(r - std::log(static_cast<double>(kRatios[i].num) / kRatios[i].den));
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

void build_width_ladder() {
    g_choice_count = 0;
    // The authentic view is the first entry, not a checkbox beside the list.
    add_choice(240u, 160u, "original");
    // Then the shape's sizes, each labelled by how it scales into the panel's
    // largest box of that shape (the host keeps the picture's aspect: bars
    // where the panel's shape differs): a whole number fills it with even
    // pixels.
    const Ratio& q = kRatios[g_ratio];
    unsigned bh = 0;   // that box's height
    if (g_panel_w != 0u && g_panel_h != 0u)
        bh = g_panel_w * q.den >= g_panel_h * q.num ? g_panel_h : g_panel_w * q.den / q.num;
    for (const Size& z : q.sizes) {
        if (z.w == 0u) break;
        char how[40];
        if (bh == 0u) std::snprintf(how, sizeof how, "%s", q.label);
        else if (bh % z.h == 0u) std::snprintf(how, sizeof how, "%ux", bh / z.h);
        else std::snprintf(how, sizeof how, "uneven pixels");
        add_choice(z.w, z.h, how);
    }
}

// 480 wide (16:9's 480x270) is never picked for the player: the maps are not
// made for it, it is experimental (the user, 2026-11-21, after a change of
// ratio on the Mac went 16:10 384x240 -> 16:9 480x270 by the whole-pixel
// rule). It stays on the list, to be chosen by hand.
bool auto_pickable(const WidthChoice& c) { return c.width < kMaxViewWidth; }

// The ladder entry for a size: that size; else the authentic view for one no
// wider or taller than it; else the wide size nearest it in height (the
// smaller on a tie), never a 480-wide one. How a size survives a change of
// shape (16:9's 384x216 becomes 4:3's 288x216) and a size saved for another
// shape is read.
unsigned choice_like(unsigned w, unsigned h) {
    for (unsigned c = 0; c < g_choice_count; ++c)
        if (g_choices[c].width == w && g_choices[c].height == h) return c;
    if ((w <= 240u && h <= 160u) || g_choice_count < 2u) return 0u;
    unsigned best = 1u, best_d = ~0u;
    for (unsigned c = 1; c < g_choice_count; ++c) {
        if (!auto_pickable(g_choices[c])) continue;
        const unsigned d = g_choices[c].height > h ? g_choices[c].height - h : h - g_choices[c].height;
        if (d < best_d) { best_d = d; best = c; }
    }
    return best;
}

const char* const kKeys[kCount] = {"highlight", "second_screen",
                                   "weather_repeat", "pad_remap",
                                   "depth_of_field", "perspective",
                                   "effects_placed", "soft_tones", "debug_mesh",
                                   "debug_auto_win", "debug_no_encounters",
                                   "debug_hide_bg0", "debug_hide_bg1",
                                   "debug_hide_bg2", "debug_hide_bg3",
                                   "debug_hide_obj", "shop_anywhere", "torchlight",
                                   "unit_shadows", "debug_sun_stick", "lift_props",
                                   "lift_walls", "debug_tile_info", "debug_no_menu_dim",
                                   "fast_loading", "water", "analog_cursor", "plant_sway",
                                   "bump_map", "special_effects", "fast_actions",
                                   "fast_walk", "quick_enemy", "fast_attack", "own_rain", "wet_rain",
                                   "smooth_boxes", "debug_numbered_states", "fast_spells", "smooth_sky", "own_snow", "smooth_pacing",
                                   "debug_no_second_screen", "swap_face", "rewind", "vs_computer"};
const char* const kLabels[kCount] = {"See-through move range",
                                     "Second screen panel",
                                     "Rain across the whole screen",
                                     "",   // PadRemap, retired: not listed
                                     "Depth of field (follows the cursor)",
                                     "Perspective (farther is smaller)",
                                     "Sound effects come from where they happen (high quality sound)",
                                     "Soft tone channels (squares, wave, noise)",
                                     "Debug: perspective mesh (green flat, red upright)",
                                     "Debug: instant win",
                                     "Debug: no random battles",
                                     "Debug: hide BG0",
                                     "Debug: hide BG1",
                                     "Debug: hide BG2",
                                     "Debug: hide BG3",
                                     "Debug: hide sprites",
                                     "Shop anywhere on the world map (second screen)",
                                     "Torchlight (torches light the battlefield)",
                                     "Unit shadows (units cast shadows, no blob)",
                                     "Debug: L2 + right stick moves the sun across the sky",
                                     "Lifted props (trees and statues stand off the ground; heavy to run)",
                                     "Lifted walls (BG1 walls and pillars show what is behind; heavy to run)",
                                     "Debug: tile info (target tile, heights, record)",
                                     "Debug: the main screen stays bright while the menu is open",
                                     "Fast loading (decompression runs natively; loads are shorter)",
                                     "Water (ripples, shore foam, reflections)",
                                     "Analog cursor (the left stick moves the hand freely)",
                                     "Plant sway (lifted trees and bushes move with the wind)",
                                     "Bump mapping (wet floors glint in torchlight)",
                                     "Special effects (fire and smoke drawn as new effects)",
                                     "Fast actions (the battle's long steps take no time)",
                                     "Fast walking (units walk twice as fast)",
                                     "Quick enemy turns (the enemy decides at once)",
                                     "Fast attacks (a strike plays twice as fast)",
                                     "New rain (falls in the world, splashes where it lands)",
                                     "Wet in the rain (puddles grow, walls drip)",
                                     "Dialog boxes open and close smoothly",
                                     "Debug: numbered states (L3+R saves a new one, L3+L loads the latest)",
                                     "Fast spells (a spell plays twice as fast)",
                                     "Smooth sky (the sky's gradient without steps)",
                                     "New snow (falls in the world, melts where it lands)",
                                     "Smooth pacing (frames at the display's 60 Hz; the game 0.46% faster)",
                                     "Debug: no second screen (BACK pauses and shows the menu)",
                                     "Swap face buttons (A/B, X/Y)",
                                     "Rewind (L2)",
                                     "VS Mode against the computer"};

// enhancements.cfg's settings_version=: 2 since lifting went back into
// Enhanced (2026-11-20); a file without it is older, and is moved to the new
// Enhanced once if it was 1.1's Enhanced exactly.
constexpr int kSettingsVersion = 2;

std::string cfg_path() { return g_dir.empty() ? std::string() : g_dir + "/enhancements.cfg"; }

void apply(Setting s) {
    switch (s) {
        case Setting::Highlight:
            highlight_set_enabled(g_on[0]);
            break;
        case Setting::SecondScreen:
            g_second_screen_wanted = g_on[1];
            break;
        case Setting::WeatherRepeat:
            obj_cull_set_weather_repeat(g_on[2]);
            break;
        case Setting::PadRemap:   // retired: never on
            g_on[3] = false;
            break;
        case Setting::SwapFace:
            pad_swap_set(g_on[static_cast<int>(Setting::SwapFace)]);
            break;
        case Setting::SoftTones:
            gba::gba_audio_request_psg_soften(g_on[static_cast<int>(Setting::SoftTones)]);
            break;
        case Setting::SmoothPacing:
            // 60 Hz, two refreshes a frame on the Thor's 120 Hz screen, or
            // the GBA's 59.7275 (gbarecomp runtime_set_frame_rate; the audio
            // bridge follows it).
            gbarecomp::runtime_set_frame_rate(g_on[static_cast<int>(Setting::SmoothPacing)] ? 60.0 : 0.0);
            break;
        case Setting::DebugHideBG0:
        case Setting::DebugHideBG1:
        case Setting::DebugHideBG2:
        case Setting::DebugHideBG3:
        case Setting::DebugHideOBJ: {
            unsigned mask = 0;
            for (int l = 0; l < 5; ++l)
                if (g_on[static_cast<int>(Setting::DebugHideBG0) + l]) mask |= 1u << l;
            gba::g_ppu_debug_hide_layers = mask;
            break;
        }
        default:
            break;
    }
}

// The whole extended view, requested as one size. The runtime services it at a
// dispatch boundary. At 240x160 every widened-path provider is simply never
// consulted, so "off" needs no separate switch.
void apply_view_size() {
    // A scene with no margin content is drawn at the authentic size, because
    // a wider surface only shrinks it: at 480x270 the world map occupies
    // 240x160 of the surface, so the panel shows it at 4x = 960x640, against
    // 6x = 1440x960 when the surface IS 240x160. The scene policy holds the
    // same rule per frame; applying it here too stops a size picked while on
    // such a scene from being visibly snapped back a moment later.
    const bool has_margins = kol::is_battle_scene();
    gbarecomp::runtime_request_view_size(
        has_margins ? g_widescreen_width : 240u,
        has_margins ? g_widescreen_height : 160u);
}

// The choice's audio mode. HD loads its bank here, the first time it is
// picked (at start or from a menu), so no one else pays for it.
void apply_sound() {
    int mode = kSoundModes[g_sound_choice];
    if (g_sound_choice == kSoundHD && !hd_audio_activate()) mode = kSoundModes[1];
    gba::gba_audio_request_hq_mode(mode);
}

void save() {
    const std::string path = cfg_path();
    if (path.empty()) return;
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (int i = 0; i < kCount; ++i)
        if (persisted(i)) std::fprintf(f, "%s=%d\n", kKeys[i], g_on[i] ? 1 : 0);
    std::fprintf(f, "dof_radius=%.1f\n", kBlurChoices[g_blur_choice].radius);
    std::fprintf(f, "music_quality=%u\n", g_sound_choice);
    std::fprintf(f, "ambient_occlusion=%u\n", g_ao_choice);
    std::fprintf(f, "rumble=%u\n", g_rumble_choice);
    std::fprintf(f, "shadows=%u\n", g_shadow_choice);
    std::fprintf(f, "music_volume=%u\n", g_music_volume * 10u);
    std::fprintf(f, "effects_volume=%u\n", g_effects_volume * 10u);
    std::fprintf(f, "debug_map=%u\n", g_debug_map.load());
    std::fprintf(f, "width=%u\n", g_widescreen_width);
    std::fprintf(f, "height=%u\n", g_widescreen_height);
    // Only a picked shape: otherwise the screen's own decides at each start.
    if (g_ratio_chosen) std::fprintf(f, "ratio=%s\n", kRatios[g_ratio].label);
    std::fprintf(f, "settings_version=%d\n", kSettingsVersion);
    std::fclose(f);
}

}  // namespace

unsigned width_choice_count() { return g_choice_count; }

const char* width_choice_label(unsigned i) {
    return i < g_choice_count ? g_choices[i].label : "";
}

unsigned width_choice_width(unsigned i) {
    return i < g_choice_count ? g_choices[i].width : 0u;
}

unsigned width_choice_height(unsigned i) {
    return i < g_choice_count ? g_choices[i].height : 160u;
}

unsigned width_choice_current() {
    for (unsigned i = 0; i < g_choice_count; ++i)
        if (g_choices[i].width == g_widescreen_width && g_choices[i].height == g_widescreen_height) return i;
    return g_choice_count;  // one past the end: no entry matches
}

void width_choice_set(unsigned i) {
    if (i >= g_choice_count) return;
    g_widescreen_width = g_choices[i].width;
    g_widescreen_height = g_choices[i].height;
    apply_view_size();
    save();
    std::fprintf(stderr, "settings: view = %ux%u\n", g_widescreen_width,
                 g_widescreen_height);
    std::fflush(stderr);
}

unsigned ratio_choice_count() { return kRatioCount; }
const char* ratio_choice_label(unsigned i) { return i < kRatioCount ? kRatios[i].label : ""; }
void ratio_choice_shape(unsigned i, unsigned* num, unsigned* den) {
    const Ratio& q = kRatios[i < kRatioCount ? i : kRatio169];
    *num = q.num;
    *den = q.den;
}
unsigned ratio_choice_current() { return g_ratio; }

unsigned view_choice_width()  { return g_settings_ready ? g_widescreen_width : 0u; }
unsigned view_choice_height() { return g_settings_ready ? g_widescreen_height : 0u; }

unsigned blur_choice_count() { return kBlurChoiceCount; }

const char* blur_choice_label(unsigned i) {
    static char labels[kBlurChoiceCount][40];
    if (i >= kBlurChoiceCount) return "";
    std::snprintf(labels[i], sizeof labels[i], "%s (%.1f)", kBlurChoices[i].label,
                  kBlurChoices[i].radius);
    return labels[i];
}

unsigned blur_choice_current() { return g_blur_choice; }

void blur_choice_set(unsigned i) {
    if (i >= kBlurChoiceCount) return;
    g_blur_choice = i;
    save();
    std::fprintf(stderr, "settings: dof_radius = %.1f\n", kBlurChoices[i].radius);
    std::fflush(stderr);
}

float blur_radius() { return kBlurChoices[g_blur_choice].radius; }

unsigned volume_choice_count() { return 11u; }

const char* volume_choice_label(unsigned i) {
    static char labels[11][8];
    if (i > 10u) return "";
    std::snprintf(labels[i], sizeof labels[i], "%u%%", i * 10u);
    return labels[i];
}

unsigned volume_choice_current(bool effects) {
    return effects ? g_effects_volume : g_music_volume;
}

void volume_choice_set(bool effects, unsigned i) {
    if (i > 10u) return;
    (effects ? g_effects_volume : g_music_volume) = i;
    gba::gba_audio_request_mix_volumes(static_cast<int>(g_music_volume * 10u),
                                       static_cast<int>(g_effects_volume * 10u));
    save();
    std::fprintf(stderr, "settings: music %u%%, effects %u%%\n", g_music_volume * 10u,
                 g_effects_volume * 10u);
    std::fflush(stderr);
}

std::string settings_dir() { return g_dir; }

const char* rom_patch_name() { return g_runtime_rom_patch_name; }
std::string rom_file_tag() {
    if (!g_runtime_rom_patch_name || !g_runtime_rom_sha1) return {};
    return "-" + std::string(g_runtime_rom_sha1).substr(0, 8);
}

unsigned debug_map_count() { return kMapCount; }

bool debug_map_in_training(unsigned n) {
    return n == 1 || (n >= 3 && n <= 56) || (n >= 104 && n <= 115);
}

unsigned debug_map_current() {
    if constexpr (!kol::kDevTools) return 0;   // a public build: the game's own maps
    static const int env = [] {
        const char* v = std::getenv("KOL_DEBUG_MAP");
        return v && *v ? std::atoi(v) : -1;
    }();
    if (env >= 0 && static_cast<unsigned>(env) <= kMapCount) return static_cast<unsigned>(env);
    return g_debug_map.load(std::memory_order_relaxed);
}

void debug_map_set(unsigned n) {
    if (n > kMapCount) return;
    g_debug_map = n;
    save();
    std::fprintf(stderr, "settings: debug_map = %u\n", n);
    std::fflush(stderr);
}

unsigned sound_choice_count() { return kSoundChoiceCount; }

const char* sound_choice_label(unsigned i) {
    return i < kSoundChoiceCount ? kSoundChoices[i] : "";
}

unsigned sound_choice_current() { return g_sound_choice; }

void sound_choice_set(unsigned i) {
    if (i >= kSoundChoiceCount) return;
    g_sound_choice = i;
    apply_sound();
    save();
    std::fprintf(stderr, "settings: sound = %s\n", kSoundChoices[i]);
    std::fflush(stderr);
}

unsigned ao_choice_count() { return kAoChoiceCount; }
const char* ao_choice_label(unsigned i) { return i < kAoChoiceCount ? kAoChoices[i] : ""; }
unsigned ao_choice_current() { return g_ao_choice; }
void ao_choice_set(unsigned i) {
    if (i >= kAoChoiceCount) return;
    g_ao_choice = i;
    save();
    std::fprintf(stderr, "settings: ambient occlusion = %s\n", kAoChoices[i]);
    std::fflush(stderr);
}
unsigned rumble_choice_count() { return kRumbleChoiceCount; }
const char* rumble_choice_label(unsigned i) { return i < kRumbleChoiceCount ? kRumbleChoices[i] : ""; }
unsigned rumble_choice_current() { return g_rumble_choice; }
void rumble_choice_set(unsigned i) {
    if (i >= kRumbleChoiceCount) return;
    g_rumble_choice = i;
    save();
    std::fprintf(stderr, "settings: rumble = %s\n", kRumbleChoices[i]);
    std::fflush(stderr);
}
float rumble_strength() { return kRumbleStrength[g_rumble_choice < kRumbleChoiceCount ? g_rumble_choice : 0]; }

float ao_reach_units() { return g_ao_choice == 1 ? 16.0f : g_ao_choice == 2 ? 32.0f : 0.0f; }

unsigned shadow_choice_count() { return kShadowChoiceCount; }
const char* shadow_choice_label(unsigned i) { return i < kShadowChoiceCount ? kShadowChoices[i] : ""; }
unsigned shadow_choice_current() { return g_shadow_choice; }
void shadow_choice_set(unsigned i) {
    if (i >= kShadowChoiceCount) return;
    g_shadow_choice = i;
    save();
    std::fprintf(stderr, "settings: shadows = %s\n", kShadowChoices[i]);
    std::fflush(stderr);
}
float shadow_sun_degrees() { return g_shadow_choice != 0 ? kShadowSunDegrees : 0.0f; }

// ── Graphics presets ──────────────────────────────────────────────────────
namespace {
struct Preset {
    const char* label;
    unsigned view_h;         // the view's lines: the shape's size nearest (160: the authentic 240x160)
    bool perspective, dof;
    unsigned blur;           // depth of field strength (kBlurChoices)
    unsigned ao, shadows;    // kAoChoices, kShadowChoices
    bool torchlight, unit_shadows, lift_props, lift_walls, highlight, weather, fast_loading, water,
        plant_sway, bump_map, fast_actions, fast_walk, quick_enemy, fast_attack, fast_spells, own_rain, wet_rain,
        smooth_boxes, smooth_sky, own_snow, smooth_pacing, special_effects;
    int sound;               // kSoundChoices, or -1 to leave the sound alone
};
// Enhanced's ambient occlusion: Half tile (asked 2026-10-21 for half-tile
// ambient occlusion in the Enhanced preset; it was Full).
// Original's sound: HQ (asked 2026-10-21 for the Original
// preset to set HQ audio); Enhanced's: HD (asked the same day).
// Special Effects on in Enhanced, off in Original (asked 2026-11-16: Enhanced is
// the default, so special effects are on by default; it was in
// no preset, so a fresh install had it off).
// Lifted Props and Lifted Walls were taken out of Enhanced for 1.1 (asked
// 2026-11-18: the heaviest pair, and the Deck could not hold 60 with them),
// then put back (asked 2026-11-20, after the worker's cuts that night).
// Saved settings that were 1.1's Enhanced exactly are moved to this one once
// (kSettingsVersion, settings_init).
const Preset kPresets[] = {
    {"Enhanced", 216u, true, true, 1u, 1u, 1u, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, 2},
    {"Original", 160u, false, false, 1u, 0u, 0u, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, 1},
};
constexpr unsigned kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
// The ladder entry for a preset's size: the authentic view, or the size of
// the shape nearest its lines (Enhanced: 384x216 at 16:9, 288x216 at 4:3,
// 320x200 at 16:10, 240x240 at 1:1).
unsigned preset_width_choice(const Preset& p) {
    if (p.view_h <= 160u || g_choice_count < 2u) return 0u;
    const unsigned c = choice_like(0u, p.view_h);
#if !defined(__ANDROID__)
    // On a computer, a size that scales unevenly into the panel gives way to
    // the widest one with whole pixels (the Deck: 384x240 is 3.33x, so
    // Enhanced starts at 320x200, 4x). Android keeps the preset's own size,
    // as shipped.
    if (!g_choices[c].whole)
        for (unsigned w = g_choice_count; w-- > 1u;)
            if (g_choices[w].whole && auto_pickable(g_choices[w])) return w;
#endif
    return c;
}
// The preset's values into the settings, nothing applied or saved.
void preset_fields(const Preset& p) {
    auto put = [](Setting s, bool on) { g_on[static_cast<int>(s)] = on; };
    put(Setting::Perspective, p.perspective);
    put(Setting::DepthOfField, p.dof);
    if (p.dof) g_blur_choice = p.blur;
    g_ao_choice = p.ao;
    g_shadow_choice = p.shadows;
    put(Setting::Torchlight, p.torchlight);
    put(Setting::UnitShadows, p.unit_shadows);
    put(Setting::LiftProps, p.lift_props);
    put(Setting::LiftWalls, p.lift_walls);
    put(Setting::Highlight, p.highlight);
    put(Setting::WeatherRepeat, p.weather);
    put(Setting::FastLoading, p.fast_loading);
    put(Setting::Water, p.water);
    put(Setting::PlantSway, p.plant_sway);
    put(Setting::BumpMap, p.bump_map);
    put(Setting::FastActions, p.fast_actions);
    put(Setting::FastWalk, p.fast_walk);
    put(Setting::QuickEnemy, p.quick_enemy);
    put(Setting::FastAttack, p.fast_attack);
    put(Setting::FastSpells, p.fast_spells);
    put(Setting::OwnRain, p.own_rain);
    put(Setting::WetRain, p.wet_rain);
    put(Setting::SmoothBoxes, p.smooth_boxes);
    put(Setting::SmoothSky, p.smooth_sky);
    put(Setting::OwnSnow, p.own_snow);
    put(Setting::SmoothPacing, p.smooth_pacing);
    put(Setting::SpecialEffects, p.special_effects);
    if (p.sound >= 0) g_sound_choice = static_cast<unsigned>(p.sound);
    const unsigned c = preset_width_choice(p);
    if (c < g_choice_count) {
        g_widescreen_width = g_choices[c].width;
        g_widescreen_height = g_choices[c].height;
    }
}
bool preset_matches(const Preset& p) {
    auto on = [](Setting s) { return g_on[static_cast<int>(s)]; };
    const unsigned c = preset_width_choice(p);
    if (c >= g_choice_count || g_choices[c].width != g_widescreen_width ||
        g_choices[c].height != g_widescreen_height) return false;
    if (on(Setting::Perspective) != p.perspective || on(Setting::DepthOfField) != p.dof) return false;
    if (p.dof && g_blur_choice != p.blur) return false;
    if (p.sound >= 0 && g_sound_choice != static_cast<unsigned>(p.sound)) return false;
    return g_ao_choice == p.ao && g_shadow_choice == p.shadows && on(Setting::Torchlight) == p.torchlight &&
           on(Setting::UnitShadows) == p.unit_shadows && on(Setting::LiftProps) == p.lift_props &&
           on(Setting::LiftWalls) == p.lift_walls && on(Setting::Highlight) == p.highlight &&
           on(Setting::WeatherRepeat) == p.weather && on(Setting::FastLoading) == p.fast_loading &&
           on(Setting::Water) == p.water && on(Setting::PlantSway) == p.plant_sway &&
           on(Setting::BumpMap) == p.bump_map && on(Setting::FastActions) == p.fast_actions &&
           on(Setting::FastWalk) == p.fast_walk && on(Setting::QuickEnemy) == p.quick_enemy &&
           on(Setting::FastAttack) == p.fast_attack && on(Setting::FastSpells) == p.fast_spells &&
           on(Setting::OwnRain) == p.own_rain &&
           on(Setting::WetRain) == p.wet_rain && on(Setting::SmoothBoxes) == p.smooth_boxes &&
           on(Setting::SmoothSky) == p.smooth_sky && on(Setting::OwnSnow) == p.own_snow &&
           on(Setting::SmoothPacing) == p.smooth_pacing && on(Setting::SpecialEffects) == p.special_effects;
}
}  // namespace

unsigned preset_count() { return kPresetCount; }
const char* preset_label(unsigned i) { return i < kPresetCount ? kPresets[i].label : ""; }
int preset_current() {
    for (unsigned i = 0; i < kPresetCount; ++i)
        if (preset_matches(kPresets[i])) return static_cast<int>(i);
    return -1;
}
void preset_apply(unsigned i) {
    if (i >= kPresetCount) return;
    const Preset& p = kPresets[i];
    preset_fields(p);
    for (Setting s : {Setting::Perspective, Setting::DepthOfField, Setting::Torchlight, Setting::UnitShadows,
                      Setting::LiftProps, Setting::LiftWalls, Setting::Highlight, Setting::WeatherRepeat,
                      Setting::FastLoading, Setting::Water, Setting::PlantSway, Setting::BumpMap,
                      Setting::FastActions, Setting::FastWalk, Setting::QuickEnemy, Setting::FastAttack,
                      Setting::FastSpells, Setting::OwnRain, Setting::WetRain, Setting::SmoothBoxes,
                      Setting::SmoothSky, Setting::OwnSnow, Setting::SmoothPacing, Setting::SpecialEffects})
        apply(s);
    if (p.sound >= 0) apply_sound();
    apply_view_size();
    save();
    std::fprintf(stderr, "settings: preset %s (view %ux%u)\n", p.label, g_widescreen_width, g_widescreen_height);
    std::fflush(stderr);
}

// The view's size after its shape changed: a preset's size for the new shape
// while the settings are that preset's (so the Preset row stays on it: the
// Deck's Enhanced is 320x200 at 16:10 and 320x180 at 16:9, both whole), else
// the new shape's size nearest the old.
namespace {
void size_for_new_shape(int preset, unsigned old_w, unsigned old_h) {
    const unsigned c = preset >= 0 ? preset_width_choice(kPresets[preset]) : choice_like(old_w, old_h);
    g_widescreen_width = g_choices[c].width;
    g_widescreen_height = g_choices[c].height;
}
}  // namespace

void ratio_choice_set(unsigned i) {
    if (i >= kRatioCount) return;
    const int preset = preset_current();
    g_ratio = i;
    g_ratio_chosen = true;
    build_width_ladder();
    size_for_new_shape(preset, g_widescreen_width, g_widescreen_height);
    apply_view_size();
    save();
    std::fprintf(stderr, "settings: ratio = %s, view = %ux%u\n", kRatios[i].label, g_widescreen_width,
                 g_widescreen_height);
    std::fflush(stderr);
}

bool second_screen_wanted() { return g_second_screen_wanted; }

void set_second_screen_present(bool present) { g_second_screen_present = present; }

bool second_screen_shown() { return g_second_screen_wanted && g_second_screen_present; }

const char* setting_key(Setting s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < kCount) ? kKeys[i] : "";
}

const char* setting_label(Setting s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < kCount) ? kLabels[i] : "";
}

bool setting_on(Setting s) {
    // KOL_SETTINGS_ON=key,key turns settings on for a desktop test run (the
    // desktop runner loads no enhancements.cfg), by their cfg keys;
    // KOL_SETTINGS_OFF=key,key turns ones that start on off (water).
    static const bool env_done = [] {
        for (const bool on : {true, false})
            if (const char* e = std::getenv(on ? "KOL_SETTINGS_ON" : "KOL_SETTINGS_OFF"))
                for (int k = 0; k < kCount; ++k) {
                    const std::size_t n = std::strlen(kKeys[k]);
                    for (const char* p = std::strstr(e, kKeys[k]); p; p = std::strstr(p + 1, kKeys[k]))
                        if ((p == e || p[-1] == ',') && (p[n] == '\0' || p[n] == ',')) {
                            g_on[k] = on;
                            apply(static_cast<Setting>(k));
                        }
                }
        return true;
    }();
    (void)env_done;
    const int i = static_cast<int>(s);
    // Numbered states (L3+R saves one, L3+L loads the latest; the menu's Save States lists them):
    // always on, in every build, no menu row (the user, 2026-11-16, wanted it
    // fixed on: it is the players' only way to make states).
    if (s == Setting::DebugNumberedStates) return true;
    // A public build (kol_dev.h): every debug setting off, whatever an old
    // enhancements.cfg says. Not No Random Battles: a Cheats row since 10-21.
    if constexpr (!kol::kDevTools)
        switch (s) {
        case Setting::DebugMesh: case Setting::DebugAutoWin: case Setting::DebugHideBG0:
        case Setting::DebugHideBG1: case Setting::DebugHideBG2: case Setting::DebugHideBG3:
        case Setting::DebugHideOBJ: case Setting::DebugSunStick: case Setting::DebugTileInfo:
        case Setting::DebugNoMenuDim: case Setting::DebugNoSecondScreen:
            return false;
        // (Not Numbered States: always on, above.)
        default:
            break;
        }
    return (i >= 0 && i < kCount) ? g_on[i] : false;
}

void setting_set(Setting s, bool on) {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= kCount) return;
    g_on[i] = on;
    apply(s);
    save();
    std::fprintf(stderr, "settings: %s = %s\n", kKeys[i], on ? "on" : "off");
    std::fflush(stderr);
}

void settings_init(const char* dir, unsigned widescreen_width,
                   unsigned panel_w, unsigned panel_h) {
    g_dir = dir ? dir : "";
    g_panel_w = std::max(panel_w, panel_h);
    g_panel_h = std::min(panel_w, panel_h);
    // The screen's own shape, until the file names one.
    g_ratio = ratio_for_panel(g_panel_w, g_panel_h);
    build_width_ladder();
    // Start at whatever the host already resolved, so a device with no
    // widescreen.cfg does not silently gain the extended view here. Take the
    // ladder's height for that width, so the marker file selects a whole size.
    g_widescreen_width = 240u;
    g_widescreen_height = 160u;
    for (unsigned c = g_choice_count; c-- > 0;)
        if (g_choices[c].width == widescreen_width) {
            g_widescreen_width = g_choices[c].width;
            g_widescreen_height = g_choices[c].height;
        }
    unsigned saved_w = 0, saved_h = 0;   // the file's view size, resolved once its shape is known

    const std::string path = cfg_path();
    if (!path.empty()) {
        std::FILE* f = std::fopen(path.c_str(), "r");
        // No file (a fresh install): the Enhanced preset, every effect on
        // (2026-10-11, asked for the current settings to be the
        // Enhanced default). Only Android calls settings_init: a desktop
        // run keeps the switches' own defaults, so the test recipes hold.
        if (!f) preset_fields(kPresets[0]);
        if (f) {
            // A file from before music_quality= has sound= (the mode: 0
            // hardware, 1 HQ mono, 2 HQ stereo); both HQ modes read as HQ.
            // A new key, so an old sound=2 is never taken for HD.
            bool quality_saved = false;
            int old_sound = -1;
            int version = 0;
            char line[128];
            while (std::fgets(line, sizeof line, f)) {
                char* eq = std::strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                const int value = std::atoi(eq + 1);
                // The size is resolved after the loop, against the shape's
                // list (ratio= is written after it): only a size this panel
                // offers, so a config copied from another device cannot put
                // the view where the display cannot show it.
                if (std::strcmp(line, "width") == 0) { saved_w = value > 0 ? static_cast<unsigned>(value) : 0u; continue; }
                if (std::strcmp(line, "height") == 0) { saved_h = value > 0 ? static_cast<unsigned>(value) : 0u; continue; }
                if (std::strcmp(line, "ratio") == 0) {
                    char* v = eq + 1;
                    v[std::strcspn(v, " \r\n")] = '\0';
                    for (unsigned r = 0; r < kRatioCount; ++r)
                        if (std::strcmp(v, kRatios[r].label) == 0) {
                            g_ratio = r;
                            g_ratio_chosen = true;
                        }
                    continue;
                }
                if (std::strcmp(line, "debug_map") == 0) {
                    if (value >= 0 && static_cast<unsigned>(value) <= kMapCount)
                        g_debug_map = static_cast<unsigned>(value);
                    continue;
                }
                if (std::strcmp(line, "music_volume") == 0 ||
                    std::strcmp(line, "effects_volume") == 0) {
                    const unsigned v = static_cast<unsigned>(std::min(std::max(value, 0), 100) + 5) / 10u;
                    (line[0] == 'm' ? g_music_volume : g_effects_volume) = v;
                    continue;
                }
                if (std::strcmp(line, "rumble") == 0) {
                    if (value >= 0 && static_cast<unsigned>(value) < kRumbleChoiceCount)
                        g_rumble_choice = static_cast<unsigned>(value);
                    continue;
                }
                if (std::strcmp(line, "ambient_occlusion") == 0) {
                    if (value >= 0 && static_cast<unsigned>(value) < kAoChoiceCount)
                        g_ao_choice = static_cast<unsigned>(value);
                    continue;
                }
                if (std::strcmp(line, "shadows") == 0) {   // a saved 1..4 pick is on
                    g_shadow_choice = value != 0 ? 1u : 0u;
                    continue;
                }
                if (std::strcmp(line, "music_quality") == 0) {
                    if (value >= 0 && static_cast<unsigned>(value) < kSoundChoiceCount)
                        g_sound_choice = static_cast<unsigned>(value);
                    quality_saved = true;
                    continue;
                }
                if (std::strcmp(line, "settings_version") == 0) {
                    version = value;
                    continue;
                }
                if (std::strcmp(line, "sound") == 0) {   // before 2026-10-18: the mode
                    old_sound = value;
                    continue;
                }
                if (std::strcmp(line, "dof_radius") == 0) {
                    const float r = static_cast<float>(std::atof(eq + 1));
                    for (unsigned c = 0; c < kBlurChoiceCount; ++c)
                        if (std::fabs(kBlurChoices[c].radius - r) < 0.05f) g_blur_choice = c;
                    continue;
                }
                for (int i = 0; i < kCount; ++i)
                    if (std::strcmp(line, kKeys[i]) == 0 && persisted(i)) g_on[i] = value != 0;
            }
            std::fclose(f);
            if (!quality_saved && old_sound >= 0) g_sound_choice = old_sound == 0 ? 0u : 1u;
            g_file_version = version;
        }
    }
    // The file's shape and size. A size not on the shape's list (another
    // shape's, or a phone's old 20:9 size from before the list was 16:9 only)
    // becomes the list's size nearest it in height; a file from before
    // height= was saved takes 16:9's.
    if (g_ratio_chosen) build_width_ladder();
    if (saved_w != 0u) {
        if (saved_h == 0u) saved_h = saved_w <= 240u ? 160u : saved_w * 9u / 16u;
        const unsigned c = choice_like(saved_w, saved_h);
        g_widescreen_width = g_choices[c].width;
        g_widescreen_height = g_choices[c].height;
        // Not on the list (saved under another shape): a preset's size there
        // (as many lines) with the rest of the settings that preset's takes
        // the preset's size for this shape, as a change of ratio in the menu
        // does; a size picked by hand stays the nearest.
        if (g_choices[c].width != saved_w || g_choices[c].height != saved_h)
            for (unsigned p = 0; p < kPresetCount; ++p) {
                if (kPresets[p].view_h != saved_h) continue;
                const unsigned pc = preset_width_choice(kPresets[p]);
                const unsigned kw = g_widescreen_width, kh = g_widescreen_height;
                g_widescreen_width = g_choices[pc].width;
                g_widescreen_height = g_choices[pc].height;
                if (preset_matches(kPresets[p])) break;
                g_widescreen_width = kw;
                g_widescreen_height = kh;
            }
    } else if (g_ratio_chosen) {
        const unsigned c = choice_like(g_widescreen_width, g_widescreen_height);
        g_widescreen_width = g_choices[c].width;
        g_widescreen_height = g_choices[c].height;
    }
    // Settings saved before version 2 that are 1.1's Enhanced exactly (lifting
    // off, all else Enhanced: a player who never changed anything) become
    // today's Enhanced, once: saved with the version, so turning them off
    // again sticks.
    if (g_file_version >= 0 && g_file_version < 2) {
        Preset old = kPresets[0];
        old.lift_props = old.lift_walls = false;
        if (preset_matches(old)) {
            g_on[static_cast<int>(Setting::LiftProps)] = true;
            g_on[static_cast<int>(Setting::LiftWalls)] = true;
            std::fprintf(stderr, "settings: 1.1's Enhanced moved to today's (Lifted Props and Walls on)\n");
        }
        save();   // (the version, whichever way)
    }
    // KOL_SETTINGS_ON / KOL_SETTINGS_OFF over the file or the preset, for a
    // test run in player mode (setting_on's own pass ran before the file was
    // read, so the preset undid it: five Deck timing runs measured the same
    // thing, 2026-11-18). Dev builds only.
    if constexpr (kol::kDevTools)
        for (const bool on : {true, false})
            if (const char* e = std::getenv(on ? "KOL_SETTINGS_ON" : "KOL_SETTINGS_OFF"))
                for (int k = 0; k < kCount; ++k) {
                    const std::size_t n = std::strlen(kKeys[k]);
                    for (const char* q = std::strstr(e, kKeys[k]); q; q = std::strstr(q + 1, kKeys[k]))
                        if ((q == e || q[-1] == ',') && (q[n] == '\0' || q[n] == ',')) {
                            g_on[k] = on;
                            std::fprintf(stderr, "settings: %s forced %s\n", kKeys[k], on ? "on" : "off");
                        }
                }
    for (int i = 0; i < kCount; ++i) apply(static_cast<Setting>(i));
    apply_sound();
    gba::gba_audio_request_mix_volumes(static_cast<int>(g_music_volume * 10u),
                                       static_cast<int>(g_effects_volume * 10u));
    g_settings_ready = true;
    apply_view_size();
    std::fprintf(stderr, "settings: view=%ux%u music_quality=%u highlight=%d screen2=%d weather=%d pad=%d dof=%d persp=%d debug_mesh=%d auto_win=%d\n",
                 g_widescreen_width, g_widescreen_height, g_sound_choice,
                 g_on[0] ? 1 : 0, g_on[1] ? 1 : 0, g_on[2] ? 1 : 0, g_on[3] ? 1 : 0,
                 g_on[4] ? 1 : 0, g_on[5] ? 1 : 0, g_on[8] ? 1 : 0, g_on[9] ? 1 : 0);
    std::fprintf(stderr, "settings: ratio %s (%s; panel %ux%u), view ladder (%u):", kRatios[g_ratio].label,
                 g_ratio_chosen ? "chosen" : "the screen's", g_panel_w, g_panel_h, g_choice_count);
    for (unsigned c = 0; c < g_choice_count; ++c)
        std::fprintf(stderr, " %s%ux%u",
                     c == width_choice_current() ? "*" : "",
                     g_choices[c].width, g_choices[c].height);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

}  // namespace kol
