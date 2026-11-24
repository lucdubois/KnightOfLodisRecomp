// kol_desktop.cpp — see the header.

#include "kol_desktop.h"
#include "kol_crash.h"

#if !defined(__ANDROID__)

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <vector>

#include "rom_patch.h"
#include "runtime.h"

#include "kol_cheevos.h"
#include "kol_dev.h"
#include "kol_menu.h"
#include "kol_settings.h"
#include "kol_setup.h"
#include "kol_sheet.h"
#include "kol_states.h"

namespace kol {
#if defined(__APPLE__)
int mac_main_screen_top_inset();   // kol_setup_mac.mm
#endif
}  // namespace kol

namespace kol {
namespace {

bool g_player = false;

// ── The options menu over the game ──────────────────────────────────────

std::uint32_t g_menu_px[kSheetW * kSheetH];
// Every overlay picture's version, from one count (the host keeps a texture a
// place in the list, and the menu and a popup can take turns at a place).
// The presenting thread's alone.
unsigned g_overlay_version = 0;

// ── Achievement popups (kol_cheevos.h CheevosPopup) ─────────────────────
// The desktop had none (2026-11-20, the Deck: no "Welcome" after a login, and
// an unlock went only to the log). As Android shows them (Cheevos.showPopup):
// one at a time at the top right, sliding in from the right edge in 250 ms,
// held 4 s, out in 250 ms, at the window's height x 3 / 1080 (2 on the Deck's
// 800 lines), 4 x that from the corner. Queued on the guest thread, shown by
// the presenting thread.
struct Popup {
    std::vector<std::uint32_t> px;
    int w = 0, h = 0;
    unsigned version = 0;
};
std::mutex g_pop_mu;
std::deque<Popup> g_pop_queue;   // under g_pop_mu
Popup g_pop;                     // the one up (px empty: none); presenting thread
std::chrono::steady_clock::time_point g_pop_since;

void desktop_popup(const unsigned* argb, int w, int h) {
    if (!argb || w <= 0 || h <= 0) return;
    Popup p;
    p.px.assign(argb, argb + static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    p.w = w;
    p.h = h;
    std::lock_guard<std::mutex> lock(g_pop_mu);
    if (g_pop_queue.size() < 64) g_pop_queue.push_back(std::move(p));
}

// ── Options > Licenses ──────────────────────────────────────────────────
// The third-party notices (third_party/NOTICES-desktop.txt, tools/make_licenses.py; built in
// by CMakeLists.txt) as a page over the whole window, in the setup screen's
// type (setup_text_page). The desktop had none: its Licenses row pointed at
// a file in the source tree (2026-11-20). The pad through the menu
// (MenuPlatform::page_*), Escape, Page Up / Down, Home / End and the wheel
// here. First line and page size are atomics: the pad runs on the guest
// thread, the keys and the wheel on the event loop's, the drawing at present.
const char kLicensesText[] =
#include "kol_licenses_baked.inc"
    ;
std::atomic<bool> g_lic_open{false};
std::atomic<int> g_lic_first{0}, g_lic_lines{0}, g_lic_page{30};
std::vector<std::uint32_t> g_lic_px;
int g_lic_w = 0, g_lic_h = 0, g_lic_drawn_first = -1;
unsigned g_lic_version = 0;

void licenses_show() {
    g_lic_first = 0;
    g_lic_open = true;
}
bool licenses_up() { return g_lic_open.load(); }
void licenses_close() { g_lic_open = false; }
void licenses_scroll(int dir, bool page) {
    const int step = page ? std::max(1, g_lic_page.load() - 2) : 2;
    const int last = std::max(0, g_lic_lines.load() - g_lic_page.load());
    g_lic_first = std::clamp(g_lic_first.load() + dir * step, 0, last);
}
// The wheel over the page (an SDL event watch: the host has no hook for it).
int licenses_wheel(void*, SDL_Event* e) {
    if (e->type == SDL_MOUSEWHEEL && g_lic_open.load() && e->wheel.y != 0) {
        const int last = std::max(0, g_lic_lines.load() - g_lic_page.load());
        g_lic_first = std::clamp(g_lic_first.load() - 3 * e->wheel.y, 0, last);
    }
    return 1;
}

// The page at this present, the whole window; false when it is not up.
bool licenses_overlay(gbarecomp::PresentOverlay* o, int out_w, int out_h) {
    if (!g_lic_open.load()) return false;
    const int first = g_lic_first.load();
    if (out_w != g_lic_w || out_h != g_lic_h || first != g_lic_drawn_first) {
        g_lic_w = out_w;
        g_lic_h = out_h;
        g_lic_px.assign(static_cast<std::size_t>(out_w) * out_h, 0xFF000000u);
        int lines = 0, page = 1;
        setup_text_page("Licenses", kLicensesText,
                        "Up / Down: scroll     L / R: a page     B or Esc: back", out_w, out_h, first,
                        g_lic_px.data(), &lines, &page);
        g_lic_lines = lines;
        g_lic_page = page;
        g_lic_drawn_first = first;
        g_lic_version = ++g_overlay_version;
    }
    o->argb = g_lic_px.data();
    o->w = o->dw = g_lic_w;
    o->h = o->dh = g_lic_h;
    o->x = o->y = 0;
    o->version = g_lic_version;
    return true;
}

// The popup's place at this present; false when none is up.
bool popup_overlay(gbarecomp::PresentOverlay* o, int out_w, int out_h) {
    constexpr long kSlideMs = 250, kHoldMs = 4000;
    const auto now = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_pop_since).count());
    };
    long t = g_pop.px.empty() ? 0 : elapsed();
    if (g_pop.px.empty() || t >= 2 * kSlideMs + kHoldMs) {
        g_pop = Popup{};
        {
            std::lock_guard<std::mutex> lock(g_pop_mu);
            if (g_pop_queue.empty()) return false;
            g_pop = std::move(g_pop_queue.front());
            g_pop_queue.pop_front();
        }
        g_pop.version = ++g_overlay_version;
        g_pop_since = now;
        t = 0;
    }
    const int scale = std::max(1, static_cast<int>(std::lround(out_h * 3.0 / 1080.0)));
    const int dw = g_pop.w * scale, dh = g_pop.h * scale, margin = 4 * scale;
    // In, held, out; eased at both ends (Android's default interpolator).
    float f = t < kSlideMs ? static_cast<float>(t) / kSlideMs
            : t < kSlideMs + kHoldMs ? 1.0f
                                     : 1.0f - static_cast<float>(t - kSlideMs - kHoldMs) / kSlideMs;
    f = std::clamp(f, 0.0f, 1.0f);
    f = f * f * (3.0f - 2.0f * f);
    const int shown_x = out_w - margin - dw, hidden_x = out_w;
    o->argb = g_pop.px.data();
    o->w = g_pop.w;
    o->h = g_pop.h;
    o->dw = dw;
    o->dh = dh;
    o->x = hidden_x + static_cast<int>(std::lround((shown_x - hidden_x) * f));
    o->y = margin;
    o->version = g_pop.version;
    return true;
}

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
void display_poll();
void mod_poll();
int overlay_provider(gbarecomp::PresentOverlay* out, int max, int out_w, int out_h) {
    display_poll();
    mod_poll();
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
        // KOL_DESKTOP_TOUCH_AT=<n>,<m>...: a setting "changed" to the value it
        // has, at those presents (the menu's held state is loaded and the
        // game runs its frames: compare the pictures before and after).
        static const std::vector<long> touch_at = [] {
            std::vector<long> v;
            if (const char* e = std::getenv("KOL_DESKTOP_TOUCH_AT"))
                for (const char* p = e; *p;) {
                    char* end = nullptr;
                    const long n = std::strtol(p, &end, 10);
                    if (end == p) break;
                    v.push_back(n);
                    p = *end == ',' ? end + 1 : end;
                }
            return v;
        }();
        for (long t : touch_at)
            if (presents == t) volume_choice_set(false, volume_choice_current(false));
        // KOL_DESKTOP_POPUP_AT=<n>: a sample notice at the nth present (the
        // achievement popup's place and slide, in present dumps after it).
        static const long pop_at = [] {
            const char* e = std::getenv("KOL_DESKTOP_POPUP_AT");
            return e && *e ? std::atol(e) : -1L;
        }();
        if (presents == pop_at) cheevos_notice("Welcome, Player", "0 of 87 achievements unlocked.");
        // KOL_DESKTOP_LICENSES_AT=<n>[,first]: Options > Licenses at the nth
        // present (the menu opened), from the text's line `first`.
        static const char* const lic = std::getenv("KOL_DESKTOP_LICENSES_AT");
        static long lic_at = -1, lic_first = 0;
        static const bool lic_read = lic && std::sscanf(lic, "%ld,%ld", &lic_at, &lic_first) >= 1;
        if (lic_read && presents == lic_at) {
            if (!menu_is_open()) menu_toggle();
            licenses_show();
            g_lic_first = static_cast<int>(lic_first);
        }
    }
    int n = 0;
    // The top screen's frame while the menu has the focus, under the menu.
    if (max >= 1 && menu_focus_frame(&out[n])) ++n;
    if (n < max && menu_is_open()) {
        menu_take_dirty();   // the second screen's cue; here every present redraws
        if (menu_render(g_menu_px)) {
            clear_outside(g_menu_px);
            // The largest whole scale that leaves a margin: 4 on the Deck's 1280x800.
            const int scale = std::max(1, std::min(out_w * 95 / 100 / kSheetW, out_h * 95 / 100 / kSheetH));
            gbarecomp::PresentOverlay& m = out[n];
            m.argb = g_menu_px;
            m.w = kSheetW;
            m.h = kSheetH;
            m.dw = kSheetW * scale;
            m.dh = kSheetH * scale;
            m.x = (out_w - m.dw) / 2;
            m.y = (out_h - m.dh) / 2;
            m.version = ++g_overlay_version;
            ++n;
        }
    }
    // The licenses' page over the menu, while it is open (it goes with it).
    if (!menu_is_open()) g_lic_open = false;
    else if (n < max && licenses_overlay(&out[n], out_w, out_h)) ++n;
    static bool wheel_watched = false;
    if (!wheel_watched) {
        wheel_watched = true;
        SDL_AddEventWatch(&licenses_wheel, nullptr);
    }
    // An achievement's popup over everything, the menu too.
    if (n < max && popup_overlay(&out[n], out_w, out_h)) ++n;
    return n;
}

// F10 or Escape opens and closes the menu (Escape quit the game at once
// before, 2026-11-18: the user asked for it to be the menu's key, as the
// right stick's click; the menu's Quit Game quits). A and S are the pad's
// west and north face buttons (Shortcut B / Previous unit, Shortcut A / Next
// unit: kol_actions' pad hook), which the GBA has no key for.
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
        if (down) menu_pad_toggle();
        return 1;
    }
    // (With a controller, the keyboard is not a second one in player mode:
    // the Deck's Desktop Mode types Escape from its Start button.)
    const bool keyboard_live = !g_player || SDL_NumJoysticks() <= 0;
    // Alt+Enter is the host's full-screen toggle, controller or not (with one
    // connected every other key was taken here, so it did nothing: players on
    // Windows, 2026-11-20).
    if (scancode == SDL_SCANCODE_RETURN && (SDL_GetModState() & KMOD_ALT) && !menu_login_up()) return 0;
    // The menu's login form (kol_menu login_open): every key is the form's
    // while it is up; the text itself comes as text (text_hook). With a
    // controller only Backspace, and Enter on a field, act (menu_login_key).
    if (menu_login_up()) {
        if (down) {
            switch (scancode) {
                case SDL_SCANCODE_BACKSPACE: menu_login_key(LoginKey::Backspace, !keyboard_live); break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER: menu_login_key(LoginKey::Enter, !keyboard_live); break;
                case SDL_SCANCODE_ESCAPE: menu_login_key(LoginKey::Escape, !keyboard_live); break;
                case SDL_SCANCODE_TAB:
                    menu_login_key((SDL_GetModState() & KMOD_SHIFT) ? LoginKey::BackTab : LoginKey::Tab, !keyboard_live);
                    break;
                case SDL_SCANCODE_UP: menu_login_key(LoginKey::Up, !keyboard_live); break;
                case SDL_SCANCODE_DOWN: menu_login_key(LoginKey::Down, !keyboard_live); break;
                case SDL_SCANCODE_LEFT: menu_login_key(LoginKey::Left, !keyboard_live); break;
                case SDL_SCANCODE_RIGHT: menu_login_key(LoginKey::Right, !keyboard_live); break;
                default: break;
            }
        }
        return 1;
    }
    // The licenses' page (Options > Licenses): Escape goes back to the menu;
    // Page Up / Down, Home and End move it (the pad's keys go through the menu).
    if (g_lic_open.load()) {
        switch (scancode) {
            case SDL_SCANCODE_ESCAPE: if (down) licenses_close(); return 1;
            case SDL_SCANCODE_PAGEUP: if (down) licenses_scroll(-1, true); return 1;
            case SDL_SCANCODE_PAGEDOWN: if (down) licenses_scroll(+1, true); return 1;
            case SDL_SCANCODE_HOME: if (down) g_lic_first = 0; return 1;
            case SDL_SCANCODE_END: if (down) licenses_scroll(+1 << 20, false); return 1;
            default: break;
        }
    }
    // F5 saves a new numbered state and F6 loads the latest (asked
    // 2026-11-21), as Shift+F1 and F1 do (the host's slot 1, which
    // kol_states numbers); with Shift too, controller or not.
    // KOL_AI_ARMY=1 (dev): F7 hands this side's army to the AI, and back.
    if (scancode == SDL_SCANCODE_F7 && ai_army_enabled()) {
        if (down) ai_army_toggle();
        return 1;
    }
    if (scancode == SDL_SCANCODE_F5 || scancode == SDL_SCANCODE_F6) {
        if (down) {
            if (scancode == SDL_SCANCODE_F5) states_save_new();
            else states_load_latest();
        }
        return 1;
    }
    if (keyboard_live) {
        if (scancode == SDL_SCANCODE_ESCAPE) {
            if (down) menu_pad_toggle();
            return 1;
        }
        if (scancode == SDL_SCANCODE_A || scancode == SDL_SCANCODE_S) {
            if (gbarecomp::g_host_pad_button_hook)
                gbarecomp::g_host_pad_button_hook(scancode == SDL_SCANCODE_S ? SDL_CONTROLLER_BUTTON_X : SDL_CONTROLLER_BUTTON_Y,
                                                  down);
            return 1;
        }
        return 0;
    }
    return scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12 ? 0 : 1;
}

// The login form's typed text (gbarecomp g_host_text_hook) and the host's
// text mode (MenuPlatform::text_input).
void text_hook(const char* utf8) { menu_login_text(utf8); }
void text_input(int on) { gbarecomp::g_host_text_input = on; }

// SteamOS (the Steam Deck): no keyboard in Gaming Mode, Steam's on screen
// with Steam + X in either mode. (Its steam://open/keyboard link was not
// verified to open it, and in Desktop Mode it may bring Steam's window up
// instead: not called.)
bool steamos() {
#if defined(__linux__)
    static const bool v = [] {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line))
            if (line == "ID=steamos") return true;
        return false;
    }();
    return v;
#else
    return false;
#endif
}

// Options > Quit Game: the window's close, which the host takes as its quit.
void quit_game() {
    SDL_Event e{};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

// A restart (Options > Technical > BIOS, Mod): the game quits as by Quit
// Game (its save written), and main starts the program again
// (desktop_restart_requested).
std::atomic<bool> g_restart{false};
void restart_game() {
    g_restart.store(true);
    quit_game();
}
// The same, onto the setup screen (main adds --setup).
std::atomic<bool> g_restart_setup{false};
void restart_on_setup() {
    g_restart_setup.store(true);
    restart_game();
}

// ── Options > Mod ───────────────────────────────────────────────────────
// As Android's (MenuPlatform::pick_mod / remove_mod, asked 2026-11-23): Choose
// opens the system's Open dialog, the file is checked on the ROM and copied
// in as <dir>/mod's one file (kol_setup setup_mod_take: the setup screen's
// Mod box does the same), then the menu asks "Restart to play ...?"
// (menu_mod_picked); the desktop reads <dir>/mod at start (desktop_prepare).
// Remove empties <dir>/mod. Where no system dialog can be used (the Deck's
// Gaming Mode, Big Picture, a Linux with neither kdialog nor zenity), the
// setup screen's own browser, made for the pad: a restart there.
void mod_take(const std::string& file) {
    if (file.empty()) return;   // (cancelled)
    std::string why;
    if (!setup_mod_take(settings_dir(), file, &why)) {
        std::fprintf(stderr, "desktop: mod %s not taken: %s\n", file.c_str(), why.c_str());
        menu_mod_picked(nullptr, why.c_str());
        return;
    }
    const std::string name = std::filesystem::path(file).filename().string();
    std::fprintf(stderr, "desktop: mod %s taken, played from the next start\n", name.c_str());
    menu_mod_picked(name.c_str(), nullptr);
}

void mod_pick() {
    // Desktop check: KOL_MOD_PICK_FILE=<path> stands in for the dialog's
    // answer. Not in a public build.
    if constexpr (kDevTools)
        if (const char* e = std::getenv("KOL_MOD_PICK_FILE"); e && *e) { mod_take(e); return; }
    std::string file;
    switch (setup_file_dialog(2, &file)) {
        case 1: mod_take(file); return;
        case 2:   // (answered at a present: mod_poll)
            menu_dialog("Mod", "Choose the mod in the window that opened.", {"OK"}, 0, nullptr);
            return;
        default: break;
    }
    menu_dialog("Mod", "Choose the mod on the setup screen? The game restarts there; your save is kept.",
                {"Restart", "Cancel"}, 1, [](int b) {
                    if (b == 0) restart_on_setup();
                });
}

// At every present: a dialog up on a thread (Linux) has answered.
void mod_poll() {
    std::string file;
    if (setup_file_dialog_poll(&file)) mod_take(file);
}

void mod_remove() {
    setup_mod_remove(settings_dir());
    std::fprintf(stderr, "desktop: mod removed, the original game from the next start\n");
}

// ── Options > Display ───────────────────────────────────────────────────
// The window's mode and size go to gbarecomp as requests (runtime.h
// g_host_window_*), taken on the window's thread at its next frame; what the
// window is now comes back the same way. Saved in display.cfg in player mode.

std::string g_display_cfg;          // display.cfg's path, empty outside player mode
int g_saved_full = -1;              // display.cfg's mode=, -1 none
int g_saved_w = 0, g_saved_h = 0;   // display.cfg's window=, 0 none

// Common window sizes, smallest first. Options > Display > Ratio picks the
// shape (kol_settings ratio_choice_*; 2026-11-21: the window's sizes follow
// it, so the window takes the picture's shape); they were the display's own
// shape's, 16:9 or 16:10, before. Within one shape a size is known by its
// height, so the window sizes ask nothing of the list's order across shapes.
constexpr int kWindowSizes[][2] = {
    // 1:1
    {720, 720},   {800, 800},   {960, 960},   {1080, 1080}, {1200, 1200}, {1440, 1440}, {1600, 1600},
    {2160, 2160},
    // 4:3
    {800, 600},   {960, 720},   {1024, 768},  {1152, 864},  {1280, 960},  {1400, 1050}, {1600, 1200},
    {1920, 1440}, {2048, 1536}, {2880, 2160},
    // 3:2 (the game's own: 720x480 is 3x, 960x640 4x, ...)
    {720, 480},   {960, 640},   {1080, 720},  {1200, 800},  {1440, 960},  {1680, 1120}, {1920, 1280},
    {2160, 1440}, {2400, 1600}, {3240, 2160},
    // 16:10
    {960, 600},   {1024, 640},  {1152, 720},  {1280, 800},  {1440, 900},  {1680, 1050}, {1920, 1200},
    {2560, 1600},
    // 16:9
    {960, 540},   {1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
    {3840, 2160}};

// Whether a size is the chosen ratio's shape (1366x768 passes as 16:9).
bool size_is_ratio(int w, int h, unsigned num, unsigned den) {
    if (w <= 0 || h <= 0 || num == 0u || den == 0u) return false;
    return std::fabs(static_cast<double>(w) * den / (static_cast<double>(h) * num) - 1.0) < 0.01;
}

void display_save() {
    if (g_display_cfg.empty()) return;
    std::FILE* f = std::fopen(g_display_cfg.c_str(), "w");
    if (!f) return;
    if (g_saved_full >= 0) std::fprintf(f, "mode=%d\n", g_saved_full);
    if (g_saved_w > 0 && g_saved_h > 0) std::fprintf(f, "window=%dx%d\n", g_saved_w, g_saved_h);
    std::fclose(f);
}

void display_load() {
    if (g_display_cfg.empty()) return;
    std::FILE* f = std::fopen(g_display_cfg.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        int a = 0, b = 0;
        if (std::sscanf(line, "mode=%d", &a) == 1 && (a == 0 || a == 1)) g_saved_full = a;
        else if (std::sscanf(line, "window=%dx%d", &a, &b) == 2 && a >= 320 && b >= 200 && a <= 0xFFFF && b <= 0xFFFF) {
            g_saved_w = a;
            g_saved_h = b;
        }
    }
    std::fclose(f);
}

// Each present: a mode changed by Alt+Enter (the host's toggle) is saved too.
void display_poll() {
    static int last = -1;
    const int now = gbarecomp::g_host_window_mode_now;
    if (last >= 0 && now != last && now != g_saved_full) {
        g_saved_full = now;
        display_save();
    }
    last = now;
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

bool desktop_restart_requested() { return g_restart.load(); }
bool desktop_restart_on_setup() { return g_restart_setup.load(); }

bool desktop_player_mode() { return g_player; }

std::vector<std::string> desktop_prepare(int argc, char** argv, bool* ok) {
    *ok = true;
    gbarecomp::g_present_overlay_provider = &overlay_provider;
    gbarecomp::g_host_key_hook = &key_hook;
    // The keyboard's layout (asked 2026-11-18), by KEYINPUT bit: A = X,
    // B = Z, Select = Right Shift, Start = Return, the arrows, R = E, L = Q;
    // Rewind on the key left of 1 (` on a US keyboard), Fast-forward on 2
    // (and Tab held, the host's hotkey). A and S: key_hook. keybinds.ini in
    // the player's folder overrides them.
    static constexpr int kKeys[10] = {
        SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_E, SDL_SCANCODE_Q};
    for (int i = 0; i < 10; ++i) gbarecomp::g_host_key_binds[i] = kKeys[i];
    gbarecomp::g_host_assist_keys[0] = SDL_SCANCODE_GRAVE;
    gbarecomp::g_host_assist_keys[1] = SDL_SCANCODE_2;
    MenuPlatform platform;
    platform.quit = &quit_game;
    platform.restart = &restart_game;
    // Options > Mod: Choose / Remove, as on Android (player mode only: a
    // developer's run takes --rom-patch, and kol_menu says so).
    platform.pick_mod = &mod_pick;
    platform.remove_mod = &mod_remove;
    // Options > Licenses: the notices on a page over the window.
    platform.show_licenses = &licenses_show;
    platform.page_up = &licenses_up;
    platform.page_scroll = &licenses_scroll;
    platform.page_close = &licenses_close;
    // RetroAchievements' login: a form in the menu, typed into (the desktop
    // had none: its Log In printed how to use KOL_RA_USER, 2026-11-19).
    platform.login_in_menu = true;
    platform.text_input = &text_input;
    platform.keyboard_hint = steamos() ? "Steam + X: on-screen keyboard." : nullptr;
    gbarecomp::g_host_text_hook = &text_hook;
    menu_set_platform(platform);
    // Achievement popups over the game (the welcome, unlocks), as Android's.
    cheevos_set_popup(&desktop_popup);

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
        bios_in_use_set(std::find(rest.begin(), rest.end(), "--no-bios") == rest.end());
        return args;
    }

    g_player = true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Absolute: a public build moves into <dir>/logs before the runtime opens
    // the ROM, so a relative --data-dir lost it (2026-11-20, under Wine: the
    // runtime's own Open dialog came up, unseen, and held the start).
    if (const auto abs = std::filesystem::absolute(dir, ec); !ec) dir = abs.string();
    // keybinds.ini and config.ini in the player's folder: the program's own
    // folder is read-only in an AppImage or a signed Mac app.
    static std::string input_dir;
    input_dir = dir;
    gbarecomp::g_host_input_config_dir = input_dir.c_str();
    const std::string rom = dir + "/totkol.gba";
    const std::string bios = dir + "/gba_bios.bin";
    std::fprintf(stderr, "desktop: player mode, files in %s\n", dir.c_str());

    // --setup (taken out of the arguments): the setup screen even with the
    // ROM in place (to change the BIOS or the mod).
    const auto setup_flag = std::find(rest.begin(), rest.end(), "--setup");
    const bool force_setup = setup_flag != rest.end();
    if (force_setup) rest.erase(setup_flag);
    const bool rom_given = std::find(rest.begin(), rest.end(), "--rom") != rest.end();
    // Options > Display's choice, when one was made, wins over the launcher's
    // --fullscreen (the AppImage passes it unless --windowed); the window
    // opens at the chosen size.
    g_display_cfg = dir + "/display.cfg";
    display_load();
    if (g_saved_full >= 0) {
        rest.erase(std::remove_if(rest.begin(), rest.end(),
                                  [](const std::string& a) { return a.rfind("--fullscreen", 0) == 0; }),
                   rest.end());
        if (g_saved_full == 1) rest.emplace_back("--fullscreen");
    }
    if (g_saved_w > 0 && g_saved_h > 0) gbarecomp::g_host_window_size_request = (g_saved_w << 16) | g_saved_h;
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

    // A public build keeps what the runtime writes beside itself (the
    // coverage summary, the miss list, a hang's dumps: names relative to the
    // current directory) in <dir>/logs, not wherever the game was started
    // from (a packaged game's is the player's home folder). A dev build
    // stays where it is: its test runs name files relative to the tree.
    if constexpr (!kDevTools) {
        const std::string logs = dir + "/logs";
        std::filesystem::create_directories(logs, ec);
        std::filesystem::current_path(logs, ec);
    }
    // The session's log there (kol_crash.h): the game's output in a public
    // build (nothing shows it: a Finder or Steam launch, Windows' windowed
    // program), a crash's report in any. KOL_LOG_TO_FILE=1: the output too
    // in a dev build (the Mac dev app's bundle sets it).
    {
        const char* to_file = std::getenv("KOL_LOG_TO_FILE");
        crash_log_begin(dir + "/logs", !kDevTools || (to_file && *to_file == '1'));
    }
    // No C toolchain on a player's machine: the static build and the
    // interpreter bridge only, as on Android.
    // (On Windows SDL_setenv leaves the C runtime's copy, which the runtime's
    // std::getenv reads, as it was: the Windows build healed with its TCC.)
#if defined(_WIN32)
    if (!std::getenv("GBARECOMP_SELFHEAL_RECOMPILE")) _putenv_s("GBARECOMP_SELFHEAL_RECOMPILE", "0");
#else
    SDL_setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 0);
#endif
    // A late picture does not hold the game back (2026-11-20, the Deck: the
    // music wobbled where the present worker ran late): the last picture is
    // shown again and the guest, and its sound, go on at their own speed, as
    // on Android. gbarecomp leaves it off on the desktop for its picture tests,
    // which need every frame's own picture: so not with the dummy video driver
    // (every headless test) nor with a present dump.
    {
        const char* vd = std::getenv("SDL_VIDEODRIVER");
        const bool test = (vd && std::strcmp(vd, "dummy") == 0) || std::getenv("GBARECOMP_PRESENT_DUMP");
        if (!test && !std::getenv("GBARECOMP_PRESENT_SKIP_LATE")) {
#if defined(_WIN32)
            _putenv_s("GBARECOMP_PRESENT_SKIP_LATE", "1");
#else
            SDL_setenv("GBARECOMP_PRESENT_SKIP_LATE", "1", 0);
#endif
        }
    }
    // With a controller, the keyboard is not a second one (key_hook's note).
    gbarecomp::g_host_keyboard_yields_to_pad = 1;

    if (!rom_given) { args.emplace_back("--rom"); args.push_back(rom); }
    // The real BIOS when the file is one (by its SHA-1), else the built-in
    // replacements (as Android's files/no_bios): a file that is not the GBA
    // BIOS would stop the runtime at start.
    // <dir>/no_bios (Options > Technical > BIOS) asks for the built-in ones
    // whatever the file.
    std::error_code bios_ec;
    const bool want_builtin = std::filesystem::exists(dir + "/no_bios", bios_ec);
    if (!want_builtin && setup_file_is(bios, kBiosSha1)) { args.emplace_back("--bios"); args.push_back(bios); }
    else args.emplace_back("--no-bios");
    bios_in_use_set(args.back() != "--no-bios");
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
                                         ".\n\nThe original game starts instead. Choose another mod in Options > Mod.";
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
#if defined(__APPLE__)
    // A notched MacBook: full screen is below the notch, so that is the
    // screen's shape (16:10, not the 3:2 the whole panel is nearest).
    if (panel_w != 0u && panel_h != 0u) {
        const unsigned inset = static_cast<unsigned>(std::max(mac_main_screen_top_inset(), 0));
        if (inset > 0u && inset < panel_h / 4u) panel_h -= inset;
    }
#endif
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

// Options > Display (kol_desktop.h).
bool display_full_screen() {
    // (A request not yet taken is what the window is about to be: the menu
    // draws its row at once.)
    const int req = gbarecomp::g_host_window_mode_request;
    return req >= 0 ? req != 0 : gbarecomp::g_host_window_mode_now != 0;
}

void display_full_screen_set(bool full) {
    gbarecomp::g_host_window_mode_request = full ? 1 : 0;
    // Back to a window: at the chosen size, not the one before full screen.
    if (!full && g_saved_w > 0 && g_saved_h > 0) gbarecomp::g_host_window_size_request = (g_saved_w << 16) | g_saved_h;
    g_saved_full = full ? 1 : 0;
    display_save();
}

namespace {
// The game's view size (Graphics > View Size; 240x160 when it is off).
void view_size(int* w, int* h) {
    *w = static_cast<int>(view_choice_width());
    *h = static_cast<int>(view_choice_height());
    if (*w <= 0 || *h <= 0) { *w = 240; *h = 160; }
}

// The window sizes offered, by height: the chosen shape's common sizes and
// the view's exact multiples (2x, 3x...: Integer scaling fills them), those
// that fit the display's usable area (all, until it is known).
std::vector<std::pair<int, int>> window_sizes_fitting() {
    const int usable = gbarecomp::g_host_display_usable;
    const int uw = (usable >> 16) & 0xFFFF, uh = usable & 0xFFFF;
    const auto fits = [&](int w, int h) { return usable == 0 || (w <= uw && h <= uh); };
    unsigned num = 16, den = 9;
    ratio_choice_shape(ratio_choice_current(), &num, &den);
    std::vector<std::pair<int, int>> fit;
    for (const auto& s : kWindowSizes)
        if (fits(s[0], s[1]) && size_is_ratio(s[0], s[1], num, den)) fit.emplace_back(s[0], s[1]);
    int vw = 0, vh = 0;
    view_size(&vw, &vh);
    for (int k = 2; vw * k <= 3840 && vh * k <= 2160; ++k)
        if (fits(vw * k, vh * k)) fit.emplace_back(vw * k, vh * k);
    if (fit.empty())   // none fits: the shape's smallest
        for (const auto& s : kWindowSizes)
            if (fit.empty() && size_is_ratio(s[0], s[1], num, den)) fit.emplace_back(s[0], s[1]);
    if (fit.empty()) fit.emplace_back(kWindowSizes[0][0], kWindowSizes[0][1]);
    std::sort(fit.begin(), fit.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    });
    fit.erase(std::unique(fit.begin(), fit.end()), fit.end());
    return fit;
}
}  // namespace

unsigned window_size_count() { return static_cast<unsigned>(window_sizes_fitting().size()); }

void window_size_at(unsigned i, int* w, int* h) {
    const auto fit = window_sizes_fitting();
    const auto& s = fit[std::min<std::size_t>(i, fit.size() - 1)];
    *w = s.first;
    *h = s.second;
}

int window_size_current() {
    const auto fit = window_sizes_fitting();
    for (std::size_t i = 0; i < fit.size(); ++i)
        if (fit[i].first == g_saved_w && fit[i].second == g_saved_h) return static_cast<int>(i);
    return -1;
}

int window_size_multiple(int w, int h) {
    int vw = 0, vh = 0;
    view_size(&vw, &vh);
    return w % vw == 0 && h % vh == 0 && w / vw == h / vh ? w / vw : 0;
}


void window_size_now(int* w, int* h) {
    const int s = gbarecomp::g_host_window_size_now;
    *w = (s >> 16) & 0xFFFF;
    *h = s & 0xFFFF;
}

void window_size_step(int dir) {
    const int n = static_cast<int>(window_size_count());
    int cur = window_size_current();
    int next;
    if (cur >= 0) {
        next = ((cur + (dir < 0 ? -1 : 1)) % n + n) % n;
    } else {   // none chosen: the next size past the window's own
        int ww = 0, wh = 0;
        window_size_now(&ww, &wh);
        const long area = static_cast<long>(ww) * wh;
        next = dir < 0 ? n - 1 : 0;
        for (int i = 0; i < n; ++i) {
            int w = 0, h = 0;
            window_size_at(static_cast<unsigned>(i), &w, &h);
            const long a = static_cast<long>(w) * h;
            if (dir > 0 && a > area) { next = i; break; }
            if (dir < 0 && a < area) next = i;
        }
    }
    window_size_at(static_cast<unsigned>(next), &g_saved_w, &g_saved_h);
    gbarecomp::g_host_window_size_request = (g_saved_w << 16) | g_saved_h;
    display_save();
}

// A new ratio: the window takes the new shape at the size nearest its height
// (the smaller on a tie), at once in Window mode, on going back to a window in
// full screen. A window already of that shape stays as it is.
void display_ratio_changed() {
    int ww = 0, wh = 0;
    const bool full = display_full_screen();
    if (full) { ww = g_saved_w; wh = g_saved_h; }
    else window_size_now(&ww, &wh);
    if (ww <= 0 || wh <= 0) return;   // nothing chosen, nothing known: the window as it is
    unsigned num = 16, den = 9;
    ratio_choice_shape(ratio_choice_current(), &num, &den);
    if (size_is_ratio(ww, wh, num, den)) return;
    const unsigned n = window_size_count();
    int best_w = 0, best_h = 0, best_d = 1 << 30;
    for (unsigned i = 0; i < n; ++i) {
        int w = 0, h = 0;
        window_size_at(i, &w, &h);
        const int d = std::abs(h - wh);
        if (d < best_d) { best_d = d; best_w = w; best_h = h; }
    }
    if (best_w <= 0) return;
    g_saved_w = best_w;
    g_saved_h = best_h;
    if (!full) gbarecomp::g_host_window_size_request = (g_saved_w << 16) | g_saved_h;
    display_save();
}

}  // namespace kol

#else   // Android has its own front end (android/app/jni/main_android.cpp).

namespace kol {
bool display_full_screen() { return true; }
void display_full_screen_set(bool) {}
unsigned window_size_count() { return 1; }
void window_size_at(unsigned, int* w, int* h) { *w = *h = 0; }
int window_size_current() { return -1; }
void window_size_now(int* w, int* h) { *w = *h = 0; }
void window_size_step(int) {}
void display_ratio_changed() {}
bool desktop_player_mode() { return false; }
bool desktop_restart_requested() { return false; }
bool desktop_restart_on_setup() { return false; }
int window_size_multiple(int, int) { return 0; }
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
