// kol_desktop.cpp — see the header.

#include "kol_desktop.h"

#if !defined(__ANDROID__)

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "rom_patch.h"
#include "runtime.h"

#include "kol_dev.h"
#include "kol_menu.h"
#include "kol_settings.h"
#include "kol_setup.h"
#include "kol_sheet.h"

namespace kol {
namespace {

bool g_player = false;

// ── The options menu over the game ──────────────────────────────────────

std::uint32_t g_menu_px[kSheetW * kSheetH];
unsigned g_menu_version = 0;

// The canvas comes on the second screen's black. Over the game, the black
// around the menu's windows is see-through: every opaque-black pixel reached
// from the canvas's edge through opaque black.
void clear_outside(std::uint32_t* px) {
    constexpr std::uint32_t kBlack = 0xFF000000u;
    static std::vector<int> stack;
    stack.clear();
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= kSheetW || y >= kSheetH) return;
        std::uint32_t& p = px[y * kSheetW + x];
        if (p != kBlack) return;
        p = 0;
        stack.push_back(y * kSheetW + x);
    };
    for (int x = 0; x < kSheetW; ++x) { push(x, 0); push(x, kSheetH - 1); }
    for (int y = 0; y < kSheetH; ++y) { push(0, y); push(kSheetW - 1, y); }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % kSheetW, y = i / kSheetW;
        push(x - 1, y); push(x + 1, y); push(x, y - 1); push(x, y + 1);
    }
}

// Called at every present, on the presenting thread. The menu is drawn anew
// each time while it is open (its hand bobs, a touch or the pad changes it);
// closed, nothing is drawn and nothing is rendered.
int overlay_provider(gbarecomp::PresentOverlay* out, int max, int out_w, int out_h) {
    // Desktop check: KOL_DESKTOP_MENU_AT=<n> opens the menu at the nth present
    // (a present dump after it shows the overlay). Not in a public build.
    if constexpr (kDevTools) {
        static const long at = [] {
            const char* e = std::getenv("KOL_DESKTOP_MENU_AT");
            return e && *e ? std::atol(e) : -1L;
        }();
        static long presents = 0;
        if (++presents == at && !menu_is_open()) menu_toggle();
        // KOL_DESKTOP_MENU_CLOSE_AT=<n>: closes it at the nth present.
        static const long close_at = [] {
            const char* e = std::getenv("KOL_DESKTOP_MENU_CLOSE_AT");
            return e && *e ? std::atol(e) : -1L;
        }();
        if (presents == close_at && menu_is_open()) menu_toggle();
        // KOL_DESKTOP_VIEW_CYCLE=<first>,<every>,<count>: from the first-th
        // present, the next view size every `every` presents, `count` times
        // (the menu reported gone after the view size was changed twice,
        // 2026-11-18: the same without a hand on the pad).
        static const char* const cyc = std::getenv("KOL_DESKTOP_VIEW_CYCLE");
        static long c_first = 0, c_every = 0, c_count = 0;
        static const bool c_read = cyc && std::sscanf(cyc, "%ld,%ld,%ld", &c_first, &c_every, &c_count) == 3;
        if (c_read && c_every > 0 && presents >= c_first && (presents - c_first) % c_every == 0 &&
            (presents - c_first) / c_every < c_count) {
            const unsigned n = width_choice_count();
            if (n > 0) width_choice_set((width_choice_current() + 1u) % n);
        }
    }
    if (max < 1 || !menu_is_open()) return 0;
    menu_take_dirty();   // the second screen's cue; here every present redraws
    if (!menu_render(g_menu_px)) return 0;
    clear_outside(g_menu_px);
    // The largest whole scale that leaves a margin: 4 on the Deck's 1280x800.
    const int scale = std::max(1, std::min(out_w * 95 / 100 / kSheetW, out_h * 95 / 100 / kSheetH));
    out[0].argb = g_menu_px;
    out[0].w = kSheetW;
    out[0].h = kSheetH;
    out[0].dw = kSheetW * scale;
    out[0].dh = kSheetH * scale;
    out[0].x = (out_w - out[0].dw) / 2;
    out[0].y = (out_h - out[0].dh) / 2;
    out[0].version = ++g_menu_version;
    return 1;
}

// F10 opens and closes the menu.
//
// In player mode with a controller connected, the rest of the keyboard is
// not the host's, the function keys aside: on the Steam Deck in Desktop Mode,
// Steam's desktop layout types keys from the Deck's own buttons while SDL
// also reads them as a pad (Start is Escape, the host's quit: the game closed
// on Start, 2026-11-18; Select is Tab, A is Enter, the D-pad the arrows).
// Quitting is the menu's Quit Game row. With no controller the keyboard is
// the controller, and everything is the host's as before.
int key_hook(int scancode, int down) {
    if (scancode == SDL_SCANCODE_F10) {
        if (down) menu_toggle();
        return 1;
    }
    if (!g_player || SDL_NumJoysticks() <= 0) return 0;
    return scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12 ? 0 : 1;
}

// Options > Quit Game: the window's close, which the host takes as its quit.
void quit_game() {
    SDL_Event e{};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

// ── Player mode ─────────────────────────────────────────────────────────

std::string default_data_dir() {
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/Library/Application Support/KnightOfLodisRecomp";
#elif defined(_WIN32)
    if (const char* app = std::getenv("APPDATA"); app && *app)
        return std::string(app) + "\\KnightOfLodisRecomp";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::string(xdg) + "/KnightOfLodisRecomp";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/share/KnightOfLodisRecomp";
#endif
    return {};
}

bool exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

}  // namespace

bool desktop_player_mode() { return g_player; }

std::vector<std::string> desktop_prepare(int argc, char** argv, bool* ok) {
    *ok = true;
    gbarecomp::g_present_overlay_provider = &overlay_provider;
    gbarecomp::g_host_key_hook = &key_hook;
    MenuPlatform platform;
    platform.quit = &quit_game;
    menu_set_platform(platform);

    // --data-dir <dir> (taken out of the arguments), else KOL_DATA_DIR, else
    // in a public build the platform's place.
    std::vector<std::string> rest;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) dir = argv[++i];
        else rest.emplace_back(argv[i]);
    }
    if (dir.empty())
        if (const char* e = std::getenv("KOL_DATA_DIR"); e && *e) dir = e;
    if (dir.empty() && !kDevTools) dir = default_data_dir();

    std::vector<std::string> args = {argc > 0 ? argv[0] : "KnightOfLodisRecomp"};
    if (dir.empty()) {   // the developer's run, as before
        args.insert(args.end(), rest.begin(), rest.end());
        return args;
    }

    g_player = true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string rom = dir + "/totkol.gba";
    const std::string bios = dir + "/gba_bios.bin";
    std::fprintf(stderr, "desktop: player mode, files in %s\n", dir.c_str());

    // --setup (taken out of the arguments): the setup screen even with the
    // ROM in place (to change the BIOS or the mod).
    const auto setup_flag = std::find(rest.begin(), rest.end(), "--setup");
    const bool force_setup = setup_flag != rest.end();
    if (force_setup) rest.erase(setup_flag);
    const bool rom_given = std::find(rest.begin(), rest.end(), "--rom") != rest.end();
    const bool fullscreen = std::find(rest.begin(), rest.end(), "--fullscreen") != rest.end();
    // The setup screen (kol_setup): with no ROM in the directory, or one that
    // is not this game's. Play there means the right ROM is in place.
    if (!rom_given && (force_setup || !setup_file_is(rom, kRomSha1))) {
        if (!setup_run(dir, fullscreen)) {
            std::fprintf(stderr, "desktop: the setup screen was closed without Play\n");
            *ok = false;
            return args;
        }
    }

    // No C toolchain on a player's machine: the static build and the
    // interpreter bridge only, as on Android.
    SDL_setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 0);
    // With a controller, the keyboard is not a second one (key_hook's note).
    gbarecomp::g_host_keyboard_yields_to_pad = 1;

    if (!rom_given) { args.emplace_back("--rom"); args.push_back(rom); }
    // The real BIOS when the file is one (by its SHA-1), else the built-in
    // replacements (as Android's files/no_bios): a file that is not the GBA
    // BIOS would stop the runtime at start.
    if (setup_file_is(bios, kBiosSha1)) { args.emplace_back("--bios"); args.push_back(bios); }
    else args.emplace_back("--no-bios");
    // The mod: the one file in <dir>/mod, tried on the ROM first, as the
    // runtime will (main_android.cpp pick_mod). One that does not apply, or
    // changes the game's code, is set aside in <dir>/mod_refused and the
    // original game starts, with a message.
    if (!rom_given) {
        const std::string mod = setup_mod_path(dir);
        if (!mod.empty()) {
            std::ifstream f(rom, std::ios::binary);
            const std::vector<std::uint8_t> rom_bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> out;
            std::string summary, err;
            if (!rom_bytes.empty() && gbarecomp::rom_patch_load_and_check(mod, rom_bytes, &out, &summary, &err)) {
                std::fprintf(stderr, "desktop: mod %s: %s\n", mod.c_str(), summary.c_str());
                args.emplace_back("--rom-patch");
                args.push_back(mod);
            } else {
                const std::string name = std::filesystem::path(mod).filename().string();
                std::filesystem::create_directories(dir + "/mod_refused", ec);
                std::filesystem::rename(mod, dir + "/mod_refused/" + name, ec);
                const std::string text = "The mod " + name + " can't be played: " + err +
                                         ".\n\nThe original game starts instead.";
                std::fprintf(stderr, "desktop: mod %s refused: %s\n", name.c_str(), err.c_str());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Mod", text.c_str(), nullptr);
            }
        }
    }
    args.emplace_back("--save-path");
    args.push_back(dir + "/totkol.sav");
    args.emplace_back("--window");
    args.emplace_back("--skip-bios-intro");
    args.insert(args.end(), rest.begin(), rest.end());

    // The display's size, for the settings' view-size ladder (as Android).
    unsigned panel_w = 0, panel_h = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0) {
            panel_w = static_cast<unsigned>(bounds.w);
            panel_h = static_cast<unsigned>(bounds.h);
        }
    }
    // Desktop check: KOL_PANEL=1280x800 stands in for the display's size (the
    // Deck's, on another machine). Not in a public build.
    if constexpr (kDevTools)
        if (const char* e = std::getenv("KOL_PANEL")) {
            unsigned w = 0, h = 0;
            if (std::sscanf(e, "%ux%u", &w, &h) == 2 && w && h) { panel_w = w; panel_h = h; }
        }
    settings_init(dir.c_str(), 240u, panel_w, panel_h);
    return args;
}

}  // namespace kol

#else   // Android has its own front end (android/app/jni/main_android.cpp).

namespace kol {
bool desktop_player_mode() { return false; }
std::vector<std::string> desktop_prepare(int argc, char** argv, bool* ok) {
    *ok = true;
    return std::vector<std::string>(argv, argv + argc);
}
}  // namespace kol

#endif

// The right stick's click, on every platform (the Thor too, 2026-11-18: it
// was the desktop's alone and the user expected it there). Down and up
// within kClickMs with the left stick never held meanwhile (L3 is the state
// combos' modifier) opens or closes the options menu.
#include <atomic>
#include <chrono>

#include "kol_menu.h"

namespace kol {
namespace {
constexpr long long kClickMs = 400;
std::atomic<long long> g_r3_down_at{0};
std::atomic<bool> g_r3_click{false};
long long click_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void desktop_r3(int down, bool l3_held) {
    if (down) {
        g_r3_click.store(!l3_held, std::memory_order_relaxed);
        g_r3_down_at.store(click_now_ms(), std::memory_order_relaxed);
        return;
    }
    if (l3_held) g_r3_click.store(false, std::memory_order_relaxed);
    if (!g_r3_click.exchange(false, std::memory_order_relaxed)) return;
    if (click_now_ms() - g_r3_down_at.load(std::memory_order_relaxed) <= kClickMs) menu_pad_toggle();
}
}  // namespace kol
