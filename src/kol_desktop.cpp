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
#include <system_error>
#include <vector>

#include "runtime.h"

#include "kol_dev.h"
#include "kol_menu.h"
#include "kol_settings.h"
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

// A click of the right stick: down and up within this, the left stick never
// held meanwhile (L3 + R3 is the Save States page).
constexpr std::uint32_t kClickMs = 400;
std::atomic<std::uint32_t> g_r3_down_at{0};
std::atomic<bool> g_r3_click{false};

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

void desktop_r3(int down, bool l3_held) {
    if (down) {
        g_r3_click.store(!l3_held, std::memory_order_relaxed);
        g_r3_down_at.store(SDL_GetTicks(), std::memory_order_relaxed);
        return;
    }
    if (l3_held) g_r3_click.store(false, std::memory_order_relaxed);
    if (!g_r3_click.exchange(false, std::memory_order_relaxed)) return;
    if (SDL_GetTicks() - g_r3_down_at.load(std::memory_order_relaxed) <= kClickMs) menu_toggle();
}

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

    const bool rom_given = std::find(rest.begin(), rest.end(), "--rom") != rest.end();
    if (!rom_given && !exists(rom)) {
        // The setup screen's place (Android: SetupActivity). Until there is
        // one here, say where the ROM goes.
        const std::string text =
            "Your Tactics Ogre: The Knight of Lodis (USA) ROM was not found.\n\n"
            "Copy it to:\n" + rom + "\n\n"
            "Optional: the GBA BIOS, beside it, as gba_bios.bin.";
        std::fprintf(stderr, "desktop: no ROM at %s\n", rom.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Knight of Lodis Recomp", text.c_str(), nullptr);
        *ok = false;
        return args;
    }

    // No C toolchain on a player's machine: the static build and the
    // interpreter bridge only, as on Android.
    SDL_setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 0);
    // With a controller, the keyboard is not a second one (key_hook's note).
    gbarecomp::g_host_keyboard_yields_to_pad = 1;

    if (!rom_given) { args.emplace_back("--rom"); args.push_back(rom); }
    if (exists(bios)) { args.emplace_back("--bios"); args.push_back(bios); }
    else args.emplace_back("--no-bios");
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
void desktop_r3(int, bool) {}
std::vector<std::string> desktop_prepare(int argc, char** argv, bool* ok) {
    *ok = true;
    return std::vector<std::string>(argv, argv + argc);
}
}  // namespace kol

#endif
