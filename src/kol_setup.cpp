// kol_setup.cpp — see the header.

#include "kol_setup.h"

#if !defined(__ANDROID__)

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#if defined(__linux__)
#include <atomic>
#include <memory>
#include <thread>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "rom_patch.h"
#include "sha1.h"

#include "kol_dev.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"   // third_party/stb, public domain

namespace kol {
#if defined(__APPLE__)
// kol_setup_mac.mm: the system's Open panel. False when cancelled.
bool setup_mac_open_panel(const char* message, std::string* path);
#elif defined(_WIN32)
// kol_setup_win.cpp: the system's Open dialog. False when cancelled.
bool setup_win_open_dialog(SDL_Window* window, int what, const char* title, std::string* path);
#endif
namespace {

namespace fs = std::filesystem;

#if defined(__linux__)
// ── Linux: the desktop's own file dialog, but in a game console's shell ─────
// (asked 2026-11-18). In the Steam Deck's Gaming Mode (gamescope) or Steam's
// Big Picture, a desktop dialog cannot be counted on and would be driven by
// a mouse: the screen's own browser, made for the pad. Anywhere else, KDE's
// kdialog or GNOME's zenity, whichever is there; neither: the browser.
// KOL_FILE_PICKER=builtin or =system forces one.
bool console_shell() {
    auto set = [](const char* n) { const char* v = std::getenv(n); return v && *v; };
    if (set("GAMESCOPE_WAYLAND_DISPLAY")) return true;
    if (const char* d = std::getenv("XDG_CURRENT_DESKTOP"); d && std::strstr(d, "gamescope")) return true;
    if (const char* g = std::getenv("SteamGamepadUI"); g && std::strcmp(g, "1") == 0) return true;
    return false;
}

std::string on_path(const char* name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/bin:/bin";
    std::size_t at = 0;
    while (at <= dirs.size()) {
        const std::size_t end = std::min(dirs.find(':', at), dirs.size());
        const std::string cand = dirs.substr(at, end - at) + "/" + name;
        if (end > at && ::access(cand.c_str(), X_OK) == 0) return cand;
        at = end + 1;
    }
    return {};
}

// The system dialog's command for `title`, or "" when there is none to use.
std::string system_dialog_command(const std::string& title) {
    if (const char* f = std::getenv("KOL_FILE_PICKER")) {
        if (std::strcmp(f, "builtin") == 0) return {};
    } else if (console_shell()) {
        return {};
    }
    const char* home = std::getenv("HOME");
    const std::string start = home && *home ? home : "/";
    auto quote = [](const std::string& t) {
        std::string q = "'";
        for (char c : t) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    };
    // The child does without the AppImage's libraries (LD_LIBRARY_PATH points
    // at ours, older or newer than the desktop's own).
    const std::string env = "env -u LD_LIBRARY_PATH ";
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const bool kde = desktop && std::strstr(desktop, "KDE");
    const std::string kdialog = on_path("kdialog"), zenity = on_path("zenity");
    if (!kdialog.empty() && (kde || zenity.empty()))
        return env + quote(kdialog) + " --title " + quote(title) + " --getopenfilename " + quote(start) + " 2>/dev/null";
    if (!zenity.empty())
        return env + quote(zenity) + " --file-selection --title=" + quote(title) + " --filename=" + quote(start + "/") +
               " 2>/dev/null";
    return {};
}

// The dialog runs on a thread of its own, so the window keeps drawing (and
// answering the desktop) while it is up.
struct SystemPick {
    std::thread thread;
    std::atomic<bool> done{false};
    bool chosen = false;
    std::string file;
    ~SystemPick() { if (thread.joinable()) thread.join(); }
};
#endif

// ── Files ───────────────────────────────────────────────────────────────

constexpr std::size_t kMaxFile = 64u * 1024u * 1024u;

bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0 || static_cast<std::size_t>(n) > kMaxFile) return false;
    f.seekg(0);
    out->resize(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out->data()), n);
    return static_cast<bool>(f);
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
    const std::string tmp = path + ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string sha1_of(const std::vector<std::uint8_t>& data) { return gba::sha1(data.data(), data.size()).hex(); }

bool same_hex(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

bool setup_file_is(const std::string& path, const char* sha1_hex) {
    std::vector<std::uint8_t> data;
    return read_file(path, &data) && same_hex(sha1_of(data), sha1_hex);
}

// ── One package, more than one game ─────────────────────────────────────

std::string setup_game_dir(const std::string& dir, int cart) {
    if (cart < 0 || cart >= kCartCount) return dir;
    const char* folder = kCarts[cart].folder;
    return *folder ? dir + "/" + folder : dir;
}

std::string setup_program(int cart) {
    if (cart < 0 || cart >= kCartCount) return {};
    static const std::string self = []() -> std::string {
#if defined(_WIN32)
        wchar_t buf[32768];
        const DWORD n = ::GetModuleFileNameW(nullptr, buf, 32768);
        if (n == 0 || n >= 32768) return {};
        return fs::path(std::wstring(buf, n)).string();
#elif defined(__APPLE__)
        std::uint32_t n = 0;
        _NSGetExecutablePath(nullptr, &n);
        std::string p(n, '\0');
        if (_NSGetExecutablePath(p.data(), &n) != 0) return {};
        p.resize(std::strlen(p.c_str()));
        return p;
#else
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
#endif
    }();
    if (cart == kThisCart) return self.empty() ? std::string(kCarts[cart].program) : self;
    // Desktop checks (dev builds): KOL_OTHER_PROGRAM names the other game's
    // program where the two are not side by side (two build folders).
    if constexpr (kDevTools)
        if (const char* e = std::getenv("KOL_OTHER_PROGRAM"); e && *e) return e;
    if (self.empty()) return {};
    std::string name = kCarts[cart].program;
#if defined(_WIN32)
    name += ".exe";
#endif
    std::error_code ec;
    const fs::path other = fs::path(self).parent_path() / name;
    return fs::is_regular_file(other, ec) ? other.string() : std::string();
}

bool setup_rom_in_place(const std::string& dir, int cart) {
    if (cart < 0 || cart >= kCartCount) return false;
    return setup_file_is(setup_game_dir(dir, cart) + "/totkol.gba", kCarts[cart].sha1);
}

int setup_game(const std::string& dir) {
    const auto can = [&](int c) { return !setup_program(c).empty() && setup_rom_in_place(dir, c); };
    std::string key;
    {
        std::ifstream f(dir + "/game.cfg");
        std::getline(f, key);
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n' || key.back() == ' ')) key.pop_back();
    }
    for (int c = 0; c < kCartCount; ++c)
        if (key == kCarts[c].key && can(c)) return c;
    if (can(kThisCart)) return kThisCart;
    for (int c = 0; c < kCartCount; ++c)
        if (can(c)) return c;
    return -1;
}

void setup_game_set(const std::string& dir, int cart) {
    if (cart < 0 || cart >= kCartCount) return;
    std::ofstream f(dir + "/game.cfg", std::ios::trunc);
    f << kCarts[cart].key << "\n";
}

std::string setup_mod_path(const std::string& dir) {
    std::error_code ec;
    std::vector<std::string> names;
    for (const fs::directory_entry& e : fs::directory_iterator(dir + "/mod", ec)) {
        const std::string n = e.path().filename().string();
        if (!n.empty() && n[0] != '.' && e.is_regular_file(ec)) names.push_back(n);
    }
    if (names.empty()) return {};
    std::sort(names.begin(), names.end());
    return dir + "/mod/" + names.front();
}

bool setup_mod_take(const std::string& dir, const std::string& file, std::string* why, bool checked) {
    std::vector<std::uint8_t> data;
    if (!read_file(file, &data)) { *why = "Cannot read that file."; return false; }
    // A patch that does not apply, or changes the game's code, is refused
    // here when the ROM is in place to try it on.
    std::vector<std::uint8_t> rom, out;
    std::string summary, err;
    if (checked && read_file(dir + "/totkol.gba", &rom) && same_hex(sha1_of(rom), kRomSha1) &&
        !gbarecomp::rom_patch_load_and_check(file, rom, &out, &summary, &err)) {
        *why = "That mod can't be played: " + err + ".";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir + "/mod", ec);
    for (const fs::directory_entry& e : fs::directory_iterator(dir + "/mod", ec)) fs::remove(e.path(), ec);
    if (!write_file(dir + "/mod/" + fs::path(file).filename().string(), data)) { *why = "Copy failed."; return false; }
    return true;
}

void setup_mod_remove(const std::string& dir) {
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir + "/mod", ec)) fs::remove(e.path(), ec);
}

namespace {
// What a ROM is asked with: this program's game by its name when the package
// holds only it, either game's when it holds both.
const char* ask_rom() {
    int n = 0;
    for (int c = 0; c < kCartCount; ++c) n += !setup_program(c).empty();
    return n > 1 ? "Select the game's ROM (Tactics Ogre: The Knight of Lodis, USA or Japan)" : kCartAsk;
}
const char* ask_file(int what) {
    static const char* const kAsk[3] = {nullptr, "Select the GBA BIOS", "Select a mod (.ips, .ups or .bps)"};
    return what == 0 ? ask_rom() : kAsk[what];
}
#if defined(__linux__)
std::unique_ptr<SystemPick> g_game_pick;   // setup_file_dialog's, from the running game
#endif
}  // namespace

int setup_file_dialog(int what, std::string* file) {
    if (what < 0 || what > 2) return 0;
#if defined(__APPLE__)
    SDL_Window* const window = SDL_GetKeyboardFocus();
    const bool chosen = setup_mac_open_panel(ask_file(what), file);
    if (window) SDL_RaiseWindow(window);
    if (!chosen) file->clear();
    return 1;
#elif defined(_WIN32)
    SDL_Window* const window = SDL_GetKeyboardFocus();
    const bool chosen = setup_win_open_dialog(window, what, ask_file(what), file);
    if (window) SDL_RaiseWindow(window);
    if (!chosen) file->clear();
    return 1;
#elif defined(__linux__)
    if (g_game_pick) return 2;   // (one at a time: the one up answers)
    const std::string cmd = system_dialog_command(ask_file(what));
    if (cmd.empty()) return 0;
    g_game_pick = std::make_unique<SystemPick>();
    SystemPick* p = g_game_pick.get();
    p->thread = std::thread([p, cmd] {
        if (FILE* f = ::popen(cmd.c_str(), "r")) {
            char buf[4096];
            std::string out;
            while (std::fgets(buf, sizeof buf, f)) out += buf;
            const int rc = ::pclose(f);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            p->chosen = rc == 0 && !out.empty();
            p->file = out;
        }
        p->done.store(true, std::memory_order_release);
    });
    return 2;
#else
    (void)file;
    return 0;
#endif
}

bool setup_file_dialog_poll(std::string* file) {
#if defined(__linux__)
    if (!g_game_pick || !g_game_pick->done.load(std::memory_order_acquire)) return false;
    *file = g_game_pick->chosen ? g_game_pick->file : std::string();
    g_game_pick.reset();
    return true;
#else
    (void)file;
    return false;
#endif
}

namespace {

// ── Drawing ─────────────────────────────────────────────────────────────

struct Rgb { Uint8 r, g, b; };
constexpr Rgb kWhite{255, 255, 255}, kPaper{245, 245, 245}, kInk{33, 33, 33}, kSoft{97, 97, 97};
constexpr Rgb kRed{0xD3, 0x2F, 0x2F}, kGreen{0x2E, 0x7D, 0x32}, kGrey{0x9E, 0x9E, 0x9E};
constexpr Rgb kLine{0xBD, 0xBD, 0xBD}, kAccent{0x19, 0x76, 0xD2}, kOff{0xE0, 0xE0, 0xE0};

// Roboto (third_party/roboto, Apache 2.0), as Android's own screens use: the
// two weights built into the game (CMakeLists.txt writes the bytes), drawn
// with stb_truetype. A size's glyphs (ASCII) are baked once into a texture
// of coverage, tinted when drawn. (A bitmap 8 x 8 font came first: the user
// found it horrible, 2026-11-18.)
const unsigned char kRobotoRegular[] = {
#include "kol_font_roboto_Regular.inc"
};
const unsigned char kRobotoBold[] = {
#include "kol_font_roboto_Bold.inc"
};

struct Face {
    const unsigned char* data = nullptr;
    stbtt_fontinfo info{};
    bool ok = false;
    struct Baked {
        int px = 0;
        SDL_Texture* tex = nullptr;
        stbtt_bakedchar ch[96];
        int ascent = 0;
    };
    std::vector<Baked> sizes;

    void load(const unsigned char* ttf) {
        data = ttf;
        ok = stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)) != 0;
    }
    const Baked* at(SDL_Renderer* r, int px) {
        if (!ok || px < 4) return nullptr;
        for (const Baked& b : sizes)
            if (b.px == px) return &b;
        Baked b;
        b.px = px;
        int side = 256;
        std::vector<unsigned char> cover;
        for (;; side *= 2) {
            cover.assign(static_cast<std::size_t>(side) * side, 0);
            if (stbtt_BakeFontBitmap(data, 0, static_cast<float>(px), cover.data(), side, side, 32, 96, b.ch) > 0 || side >= 4096) break;
        }
        std::vector<std::uint32_t> argb(cover.size());
        for (std::size_t k = 0; k < cover.size(); ++k) argb[k] = (static_cast<std::uint32_t>(cover[k]) << 24) | 0x00FFFFFFu;
        b.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, side, side);
        if (!b.tex) return nullptr;
        SDL_UpdateTexture(b.tex, nullptr, argb.data(), side * 4);
        SDL_SetTextureBlendMode(b.tex, SDL_BLENDMODE_BLEND);
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        b.ascent = static_cast<int>(asc * stbtt_ScaleForPixelHeight(&info, static_cast<float>(px)) + 0.5f);
        sizes.push_back(b);
        return &sizes.back();
    }
    void drop() {
        for (Baked& b : sizes)
            if (b.tex) SDL_DestroyTexture(b.tex);
        sizes.clear();
    }
};

struct Ui {
    SDL_Renderer* r = nullptr;
    int w = 0, h = 0;
    float k = 1.0f;   // the layout's unit: 1 on a window 800 high
    Face regular, bold;

    int px(float v) const { return std::max(1, static_cast<int>(v * k + 0.5f)); }
    void color(Rgb c) const { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); }
    void fill(int x, int y, int ww, int hh, Rgb c) const {
        color(c);
        const SDL_Rect q{x, y, ww, hh};
        SDL_RenderFillRect(r, &q);
    }
    void disc(int cx, int cy, int rad, Rgb c) const {
        color(c);
        for (int dy = -rad; dy <= rad; ++dy) {
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) ++dx;
            const SDL_Rect q{cx - dx, cy + dy, 2 * dx + 1, 1};
            SDL_RenderFillRect(r, &q);
        }
    }
    // A rounded rectangle, filled.
    void round(int x, int y, int ww, int hh, int rad, Rgb c) const {
        rad = std::max(0, std::min({rad, ww / 2, hh / 2}));
        color(c);
        for (int row = 0; row < hh; ++row) {
            int inset = 0;
            const int dy = row < rad ? rad - row : row >= hh - rad ? row - (hh - rad) + 1 : 0;
            if (dy > 0) {
                int dx = 0;
                while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) ++dx;
                inset = rad - dx;
            }
            const SDL_Rect q{x + inset, y + row, ww - 2 * inset, 1};
            SDL_RenderFillRect(r, &q);
        }
    }
    // A card: a rounded box with a border `t` wide.
    void card(int x, int y, int ww, int hh, Rgb back, Rgb border, int t) const {
        const int rad = px(12);
        round(x, y, ww, hh, rad, border);
        round(x + t, y + t, ww - 2 * t, hh - 2 * t, std::max(0, rad - t), back);
    }
    int text_w(Face& f, const std::string& t, int size) {
        const Face::Baked* b = f.at(r, size);
        if (!b) return 0;
        float x = 0.0f;
        for (unsigned char ch : t)
            if (ch >= 32 && ch < 128) x += b->ch[ch - 32].xadvance;
        return static_cast<int>(x + 0.5f);
    }
    // `t` with its top at y.
    void text(Face& f, int x, int y, const std::string& t, int size, Rgb c) {
        const Face::Baked* b = f.at(r, size);
        if (!b) return;
        SDL_SetTextureColorMod(b->tex, c.r, c.g, c.b);
        float pen = static_cast<float>(x);
        for (unsigned char ch : t) {
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar& g = b->ch[ch - 32];
            const SDL_Rect src{g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0};
            const SDL_Rect dst{static_cast<int>(pen + g.xoff + 0.5f), y + b->ascent + static_cast<int>(g.yoff), src.w, src.h};
            SDL_RenderCopy(r, b->tex, &src, &dst);
            pen += g.xadvance;
        }
    }
    void text_mid(Face& f, int cx, int y, const std::string& t, int size, Rgb c) { text(f, cx - text_w(f, t, size) / 2, y, t, size, c); }
    // A check mark in a box `d` across: two thick strokes.
    void check(int cx, int cy, int d, Rgb c) const {
        color(c);
        const int t = std::max(2, d / 6);
        auto stroke = [&](float x0, float y0, float x1, float y1) {
            const int n = std::max(1, static_cast<int>(std::max(std::abs(x1 - x0), std::abs(y1 - y0))));
            for (int i = 0; i <= n; ++i) {
                const float f = static_cast<float>(i) / n;
                const SDL_Rect q{static_cast<int>(x0 + (x1 - x0) * f) - t / 2, static_cast<int>(y0 + (y1 - y0) * f) - t / 2, t, t};
                SDL_RenderFillRect(r, &q);
            }
        };
        const float l = cx - 0.42f * d, m = cx - 0.10f * d, rgt = cx + 0.42f * d;
        stroke(l, cy + 0.02f * d, m, cy + 0.32f * d);
        stroke(m, cy + 0.32f * d, rgt, cy - 0.30f * d);
    }
    // `t` in lines no wider than `max_w`, broken at spaces.
    std::vector<std::string> wrap(Face& f, const std::string& t, int size, int max_w) {
        std::vector<std::string> lines;
        std::string line, word;
        auto flush_word = [&] {
            if (word.empty()) return;
            const std::string tried = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_w(f, tried, size) > max_w) { lines.push_back(line); line = word; }
            else line = tried;
            word.clear();
        };
        for (char ch : t) {
            if (ch == ' ') flush_word();
            else word += ch;
        }
        flush_word();
        if (!line.empty()) lines.push_back(line);
        return lines;
    }
    // A name cut to `max_w`, its end kept (the extension).
    std::string fit(Face& f, const std::string& t, int size, int max_w) {
        if (text_w(f, t, size) <= max_w || t.size() < 9) return t;
        const std::string tail = t.substr(t.size() - 4);
        for (std::size_t keep = t.size() - 5; keep > 1; --keep) {
            const std::string tried = t.substr(0, keep) + "..." + tail;
            if (text_w(f, tried, size) <= max_w) return tried;
        }
        return "..." + tail;
    }
};

// ── The screen ──────────────────────────────────────────────────────────

enum class Act { None, Left, Right, Up, Down, Accept, Back, PageUp, PageDown, Click, Drop, Quit };
struct Input { Act act = Act::None; int x = 0, y = 0; std::string file; };

struct Box { SDL_Rect at{}; char icon = '?'; Rgb dot = kGrey; std::string line; };

struct Setup {
    std::string dir;
    Ui ui;
    // The state of the files there.
    bool rom_ok = false, rom_there = false, bios_ok = false, bios_there = false;
    std::string mod;      // its file name, or ""
    std::string status;   // a copy's error, a wrong file
    int focus = 0;        // 0-2 the boxes, 3 Remove mod, 4 Play, 5 the game's title (two games in place)
    Box box[3];
    SDL_Rect remove_at{}, play_at{}, title_at{};
    // The package's games (kol_region.h kCarts): the one the boxes are for,
    // which have their ROM in place, and which the package has a program for.
    int game = kThisCart;
    bool have[kCartCount] = {}, engine[kCartCount] = {};
    int engines = 1;
    std::string gdir() const { return setup_game_dir(dir, game); }
    // Two games ready: the title is a switch between them.
    bool can_switch() const {
        int n = 0;
        for (int c = 0; c < kCartCount; ++c) n += have[c] && engine[c];
        return n > 1;
    }
    void switch_game() {
        for (int k = 1; k < kCartCount; ++k) {
            const int c = (game + k) % kCartCount;
            if (have[c] && engine[c]) { game = c; break; }
        }
        status.clear();
        refresh();
    }
    // The browser.
    int picking = -1;     // 0 ROM, 1 BIOS, 2 Mod; -1 the main page
    std::string path;     // the folder shown
    struct Entry { std::string name; bool folder; };
    std::vector<Entry> entries;
    int sel = 0, top = 0, rows = 1;
    SDL_Rect list_at{};
    int row_h = 1;

    void refresh() {
        std::error_code ec;
        engines = 0;
        for (int c = 0; c < kCartCount; ++c) {
            engine[c] = !setup_program(c).empty();
            engines += engine[c];
            have[c] = setup_rom_in_place(dir, c);
        }
        const std::string rom = gdir() + "/totkol.gba", bios = dir + "/gba_bios.bin";
        rom_there = fs::exists(rom, ec);
        rom_ok = rom_there && have[game];
        bios_there = fs::exists(bios, ec);
        bios_ok = bios_there && setup_file_is(bios, kBiosSha1);
        const std::string m = setup_mod_path(gdir());
        mod = m.empty() ? std::string() : fs::path(m).filename().string();
        if (!rom_there) box[0] = {box[0].at, '!', kRed, "Required: select the game's ROM"};
        else if (rom_ok) box[0] = {box[0].at, 'v', kGreen, "Loaded"};
        else box[0] = {box[0].at, '!', kRed, "Wrong file: not this game's ROM"};
        if (!bios_there) box[1] = {box[1].at, '?', kGrey, "Optional: HLE BIOS used"};
        else if (bios_ok) box[1] = {box[1].at, 'v', kGreen, "Loaded"};
        else box[1] = {box[1].at, '!', kRed, "Not the GBA BIOS: HLE BIOS used"};
        if (mod.empty()) box[2] = {box[2].at, '?', kGrey, "Optional: an .ips, .ups or .bps patch"};
        else box[2] = {box[2].at, 'v', kGreen, mod};
        if (rom_ok && status == "Select the game's ROM first.") status.clear();
    }

    // The browser's folder read: folders first, then files, by name; hidden
    // ones left out; ".." on top but at the root.
    void open_folder(const std::string& p) {
        std::error_code ec;
        std::vector<Entry> found;
        for (const fs::directory_entry& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            const std::string n = e.path().filename().string();
            if (n.empty() || n[0] == '.') continue;
            std::error_code ec2;
            const bool folder = e.is_directory(ec2);
            if (!folder && !e.is_regular_file(ec2)) continue;
            found.push_back({n, folder});
        }
        if (ec && found.empty() && p != "/") {   // unreadable: stay where we were
            status = "Cannot open " + p;
            return;
        }
        std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
            if (a.folder != b.folder) return a.folder;
            return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                                [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
        });
        path = p;
        entries.clear();
        if (fs::path(p).has_parent_path() && fs::path(p) != fs::path(p).root_path()) entries.push_back({"..", true});
        entries.insert(entries.end(), found.begin(), found.end());
        sel = top = 0;
        status.clear();
    }

    SDL_Window* window = nullptr;
#if defined(__linux__)
    std::unique_ptr<SystemPick> sys_pick;   // the desktop's dialog, while it is up
    int sys_what = 0;
    // Called every picture: the dialog's answer, once it has one.
    void poll_system_pick() {
        if (!sys_pick || !sys_pick->done.load(std::memory_order_acquire)) return;
        sys_pick->thread.join();
        const bool chosen = sys_pick->chosen;
        const std::string file = sys_pick->file;
        sys_pick.reset();
        status.clear();
        if (window) SDL_RaiseWindow(window);
        if (!chosen) return;
        picking = sys_what;
        take(file);
        if (picking == sys_what) picking = -1;   // (refused: the message on the main page)
    }
    bool system_pick_up() const { return static_cast<bool>(sys_pick); }
#else
    void poll_system_pick() {}
    bool system_pick_up() const { return false; }
#endif

    void start_pick(int what) {
#if defined(__APPLE__)
        // A Mac: the system's Open panel, not the screen's browser.
        std::string file;
        const bool chosen = setup_mac_open_panel(ask_file(what), &file);
        if (window) SDL_RaiseWindow(window);
        if (!chosen) return;
        picking = what;
        take(file);
        if (picking == what) picking = -1;   // (refused: the message on the main page)
        return;
#elif defined(_WIN32)
        // Windows: the system's Open dialog, as the Mac's panel.
        std::string file;
        const bool chosen = setup_win_open_dialog(window, what, ask_file(what), &file);
        if (window) SDL_RaiseWindow(window);
        if (!chosen) return;
        picking = what;
        take(file);
        if (picking == what) picking = -1;   // (refused: the message on the main page)
        return;
#elif defined(__linux__)
        const std::string cmd = system_dialog_command(ask_file(what));
        if (!cmd.empty()) {
            if (sys_pick) return;   // (one at a time)
            sys_pick = std::make_unique<SystemPick>();
            sys_what = what;
            SystemPick* p = sys_pick.get();
            p->thread = std::thread([p, cmd] {
                if (FILE* f = ::popen(cmd.c_str(), "r")) {
                    char buf[4096];
                    std::string out;
                    while (std::fgets(buf, sizeof buf, f)) out += buf;
                    const int rc = ::pclose(f);
                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
                    p->chosen = rc == 0 && !out.empty();
                    p->file = out;
                }
                p->done.store(true, std::memory_order_release);
            });
            status = "Choose the file in the window that opened.";
            return;
        }
#endif
        picking = what;
        if (path.empty()) {
            const char* home = std::getenv("HOME");
            path = home && *home ? home : "/";
        }
        open_folder(path);
    }

    // The file chosen in the browser: checked, then copied into place.
    void take(const std::string& file) {
        std::vector<std::uint8_t> data;
        if (!read_file(file, &data)) { status = "Cannot read that file."; return; }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (picking == 0) {
            // Whichever game's ROM it is (kCarts): into that game's folder,
            // and that game is the one the boxes are for.
            const std::string sha = sha1_of(data);
            int cart = -1;
            for (int c = 0; c < kCartCount; ++c)
                if (same_hex(sha, kCarts[c].sha1)) cart = c;
            if (cart < 0) {
                status = engines > 1 ? "Wrong file: not the game's ROM (the USA or the Japanese version is needed)."
                                     : kCartWrong;
                return;
            }
            if (!engine[cart]) {
                status = engines > 1 || cart == kThisCart
                             ? std::string(kCartWrong)
                             : std::string("That is ") + kCarts[cart].name + ": this download plays only " +
                                   kCartVersion + ".";
                return;
            }
            const std::string to = setup_game_dir(dir, cart);
            fs::create_directories(to, ec);
            if (!write_file(to + "/totkol.gba", data)) { status = "Copy failed."; return; }
            game = cart;
        } else if (picking == 1) {
            if (!same_hex(sha1_of(data), kBiosSha1)) { status = "Not the GBA BIOS: the HLE BIOS will be used."; return; }
            if (!write_file(dir + "/gba_bios.bin", data)) { status = "Copy failed."; return; }
        } else {
            std::string why;
            if (!setup_mod_take(gdir(), file, &why, game == kThisCart)) { status = why; return; }
        }
        status.clear();
        picking = -1;
        refresh();
    }

    // A file dropped on the window (2026-11-18, asked): what it is, by its
    // contents (the ROM, the GBA BIOS) or its extension (a mod), then taken
    // as if picked in that box's browser. From the browser too.
    void dropped(const std::string& file) {
        std::vector<std::uint8_t> data;
        if (!read_file(file, &data)) { status = "Cannot read " + fs::path(file).filename().string() + "."; return; }
        const std::string sha = sha1_of(data);
        std::string ext = fs::path(file).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool a_rom = false;   // a ROM of the package's games (take says so if its program is not here)
        for (int c = 0; c < kCartCount; ++c) a_rom = a_rom || (engine[c] && same_hex(sha, kCarts[c].sha1));
        const int kind = a_rom ? 0 : same_hex(sha, kBiosSha1) ? 1
                       : (ext == ".ips" || ext == ".ups" || ext == ".bps") ? 2 : -1;
        if (kind < 0) {
            status = fs::path(file).filename().string() +
                     (engines > 1 ? " is not the game's ROM (the USA or the Japanese version), the GBA BIOS or a mod (.ips, .ups, .bps)."
                                  : " is not this game's ROM (" + std::string(kCartVersion) + "), the GBA BIOS or a mod (.ips, .ups, .bps).");
            return;
        }
        const int was = picking;
        picking = kind;
        take(file);                          // (success: back to the main page)
        if (picking == kind) picking = was;  // (refused: where it was, with the message)
        focus = kind;
    }

    void remove_mod() {
        setup_mod_remove(gdir());
        refresh();
    }

    // ── The main page ───────────────────────────────────────────────────
    void draw_main() {
        Ui& u = ui;
        u.fill(0, 0, u.w, u.h, kPaper);
        // (The sizes, a window 800 high: larger than the first try, asked 2026-11-18.)
        const int title = u.px(48), name = u.px(40), small = u.px(26), button = u.px(36);
        int y = u.h / 18;
        title_at = {};
        if (can_switch()) {
            // Two games in place: the title is a switch, in a card of its own.
            const int tw = u.text_w(u.regular, kCarts[game].name, title), pad = u.px(70);
            title_at = {u.w / 2 - tw / 2 - pad, y - u.px(8), tw + 2 * pad, title + u.px(22)};
            u.card(title_at.x, title_at.y, title_at.w, title_at.h, kWhite, focus == 5 ? kAccent : kLine,
                   focus == 5 ? u.px(4) : u.px(2));
            u.text_mid(u.bold, title_at.x + u.px(34), y, "<", title, kSoft);
            u.text_mid(u.bold, title_at.x + title_at.w - u.px(34), y, ">", title, kSoft);
        }
        u.text_mid(u.regular, u.w / 2, y, kCarts[game].name, title, kInk);
        y += title + u.h / 22;
        const int margin = u.w / 20, gap = u.w / 50;
        const int bw = (u.w - 2 * margin - 2 * gap) / 3, bh = u.h * 34 / 100;
        static const char* const kNames[3] = {"ROM", "BIOS", "Mod"};
        for (int i = 0; i < 3; ++i) {
            Box& b = box[i];
            b.at = {margin + i * (bw + gap), y, bw, bh};
            u.card(b.at.x, b.at.y, bw, bh, kWhite, focus == i ? kAccent : kLine, focus == i ? u.px(4) : u.px(2));
            const int cx = b.at.x + bw / 2, rad = u.px(30);
            int cy = b.at.y + bh / 10 + rad;
            u.disc(cx, cy, rad, b.dot);
            if (b.icon == 'v') u.check(cx, cy, rad, kWhite);
            else u.text_mid(u.bold, cx, cy - u.px(38) / 2 - u.px(1), std::string(1, b.icon), u.px(38), kWhite);
            cy += rad + u.px(10);
            u.text_mid(u.bold, cx, cy, kNames[i], name, kInk);
            cy += name + u.px(8);
            const int inner = bw - u.px(28);
            std::vector<std::string> lines = i == 2 && b.icon == 'v' ? std::vector<std::string>{u.fit(u.regular, b.line, small, inner)}
                                                                      : u.wrap(u.regular, b.line, small, inner);
            for (std::size_t l = 0; l < lines.size() && l < 3; ++l)
                u.text_mid(u.regular, cx, cy + static_cast<int>(l) * (small + u.px(4)), lines[l], small, kSoft);
        }
        y += bh + u.h / 40;
        u.text_mid(u.bold, u.w / 2, y, "Or drag a file onto this window: the ROM, the BIOS or a mod.", small, kInk);
        y += small + u.px(14);
        static const std::string kNote =
            "A GBA BIOS file is best: the game runs on the real BIOS, as on the console. Without one, an HLE BIOS "
            "(built-in replacements for the BIOS's functions) is used instead. It is less tested: some scenes may "
            "behave differently.";
        for (const std::string& l : u.wrap(u.regular, kNote, small, u.w - 2 * margin - u.px(40))) {
            u.text_mid(u.regular, u.w / 2, y, l, small, kSoft);
            y += small + u.px(5);
        }
        y += u.px(10);
        if (!status.empty())
            for (const std::string& l : u.wrap(u.regular, status, small, u.w - 2 * margin)) {
                u.text_mid(u.regular, u.w / 2, y, l, small, kRed);
                y += small + u.px(5);
            }
        const int bh2 = u.px(76), by = u.h - u.h / 20 - bh2, bw2 = (u.w - 2 * margin - gap) / 2;
        remove_at = {margin, by, bw2, bh2};
        play_at = {margin + bw2 + gap, by, bw2, bh2};
        const bool can_remove = !mod.empty();
        u.card(remove_at.x, by, bw2, bh2, can_remove ? kWhite : kOff, focus == 3 ? kAccent : kLine, focus == 3 ? u.px(4) : u.px(2));
        u.text_mid(u.regular, remove_at.x + bw2 / 2, by + (bh2 - button) / 2 - u.px(2), "Remove mod", button, can_remove ? kInk : kGrey);
        u.card(play_at.x, by, bw2, bh2, rom_ok ? kAccent : kOff, focus == 4 ? kInk : (rom_ok ? kAccent : kLine), focus == 4 ? u.px(4) : u.px(2));
        u.text_mid(u.bold, play_at.x + bw2 / 2, by + (bh2 - button) / 2 - u.px(2), "Play", button, rom_ok ? kWhite : kGrey);
    }

    void press_main(int what) {
        if (what >= 0 && what <= 2) start_pick(what);
        else if (what == 3) { if (!mod.empty()) remove_mod(); }
        else if (what == 4 && !rom_ok) status = "Select the game's ROM first.";
        else if (what == 5 && can_switch()) switch_game();
    }

    // True: Play, with the ROM in place.
    bool input_main(const Input& in) {
        switch (in.act) {
            case Act::Left:
                if (focus == 5) press_main(5);
                else if (focus <= 2) focus = std::max(0, focus - 1);
                else focus = 3;
                break;
            case Act::Right:
                if (focus == 5) press_main(5);
                else if (focus <= 2) focus = std::min(2, focus + 1);
                else focus = 4;
                break;
            case Act::Down:
                if (focus == 5) focus = 0;
                else if (focus <= 2) focus = focus == 0 ? 3 : 4;
                break;
            case Act::Up:
                if (focus == 3 || focus == 4) focus = focus == 3 ? 0 : 2;
                else if (focus <= 2 && can_switch()) focus = 5;
                break;
            case Act::Accept:
                if (focus == 4 && rom_ok) return true;
                press_main(focus);
                break;
            case Act::Click: {
                const SDL_Point p{in.x, in.y};
                if (can_switch() && SDL_PointInRect(&p, &title_at)) { focus = 5; press_main(5); return false; }
                for (int i = 0; i < 3; ++i)
                    if (SDL_PointInRect(&p, &box[i].at)) { focus = i; press_main(i); return false; }
                if (SDL_PointInRect(&p, &remove_at)) { focus = 3; press_main(3); }
                else if (SDL_PointInRect(&p, &play_at)) {
                    focus = 4;
                    if (rom_ok) return true;
                    press_main(4);
                }
                break;
            }
            default: break;
        }
        return false;
    }

    // ── The browser ─────────────────────────────────────────────────────
    void draw_browser() {
        Ui& u = ui;
        const int margin = u.w / 24, title = u.px(42), small = u.px(25), rowt = u.px(30);
        u.fill(0, 0, u.w, u.h, kPaper);
        static const char* const kWhat[3] = {"Select the game's ROM", "Select the GBA BIOS", "Select a mod (.ips, .ups or .bps)"};
        int y = u.h / 20;
        u.text(u.regular, margin, y, kWhat[picking], title, kInk);
        y += title + u.px(8);
        std::string shown = path;
        while (shown.size() > 8 && u.text_w(u.regular, "..." + shown, small) > u.w - 2 * margin) shown.erase(0, 1);
        u.text(u.regular, margin, y, shown.size() == path.size() ? path : "..." + shown, small, kSoft);
        y += small + u.px(12);
        row_h = u.px(52);
        const int foot = small + u.px(10) + (status.empty() ? 0 : small + u.px(6));
        list_at = {margin, y, u.w - 2 * margin, u.h - y - foot - u.h / 30};
        rows = std::max(1, (list_at.h - u.px(8)) / row_h);
        u.card(list_at.x, list_at.y, list_at.w, list_at.h, kWhite, kLine, u.px(2));
        sel = std::clamp(sel, 0, std::max(0, static_cast<int>(entries.size()) - 1));
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        top = std::clamp(top, 0, std::max(0, static_cast<int>(entries.size()) - rows));
        const int name_w = list_at.w - u.px(96);
        for (int i = 0; i < rows && top + i < static_cast<int>(entries.size()); ++i) {
            const Entry& e = entries[static_cast<std::size_t>(top + i)];
            const int ry = list_at.y + u.px(4) + i * row_h;
            if (top + i == sel) u.round(list_at.x + u.px(4), ry, list_at.w - u.px(8), row_h, u.px(8), kAccent);
            const Rgb c = top + i == sel ? kWhite : e.folder ? kInk : kSoft;
            const int ty = ry + (row_h - rowt) / 2 - u.px(2);
            if (e.folder) {   // a small folder: a tab and a body
                const int fx = list_at.x + u.px(18), fy = ry + row_h / 2 - u.px(11);
                u.fill(fx, fy, u.px(12), u.px(5), c);
                u.fill(fx, fy + u.px(4), u.px(30), u.px(19), c);
            }
            Face& face = e.folder ? u.bold : u.regular;
            u.text(face, list_at.x + u.px(64), ty, u.fit(face, e.name, rowt, name_w), rowt, c);
        }
        if (entries.empty()) u.text_mid(u.regular, u.w / 2, list_at.y + list_at.h / 2 - small / 2, "This folder is empty", small, kGrey);
        // The list's place in the folder: a bar at the right.
        if (static_cast<int>(entries.size()) > rows) {
            const int track = list_at.h - u.px(16);
            const int bar = std::max(u.px(24), track * rows / static_cast<int>(entries.size()));
            const int at = (track - bar) * top / std::max(1, static_cast<int>(entries.size()) - rows);
            u.round(list_at.x + list_at.w - u.px(10), list_at.y + u.px(8) + at, u.px(5), bar, u.px(2), kGrey);
        }
        int fy = list_at.y + list_at.h + u.px(8);
        if (!status.empty()) {
            u.text(u.regular, margin, fy, u.fit(u.regular, status, small, u.w - 2 * margin), small, kRed);
            fy += small + u.px(6);
        }
        u.text(u.regular, margin, fy, "A: open      B: back      L / R: a page up or down", small, kSoft);
    }

    void open_selected() {
        if (entries.empty()) return;
        const Entry& e = entries[static_cast<std::size_t>(sel)];
        if (e.folder) {
            const fs::path p(path);
            if (e.name == "..") {
                const std::string from = p.filename().string();
                open_folder(p.parent_path().string());
                for (std::size_t i = 0; i < entries.size(); ++i)   // the hand on the folder we left
                    if (entries[i].name == from) sel = static_cast<int>(i);
            } else {
                open_folder((p / e.name).string());
            }
        } else {
            take((fs::path(path) / e.name).string());
        }
    }

    void input_browser(const Input& in) {
        const int n = static_cast<int>(entries.size());
        switch (in.act) {
            case Act::Up: sel = std::max(0, sel - 1); break;
            case Act::Down: sel = std::min(std::max(0, n - 1), sel + 1); break;
            case Act::Left: case Act::PageUp: sel = std::max(0, sel - rows); break;
            case Act::Right: case Act::PageDown: sel = std::min(std::max(0, n - 1), sel + rows); break;
            case Act::Accept: open_selected(); break;
            case Act::Back: picking = -1; status.clear(); break;
            case Act::Click: {
                const SDL_Point p{in.x, in.y};
                if (!SDL_PointInRect(&p, &list_at)) break;
                const int i = top + (in.y - list_at.y - ui.px(4)) / row_h;
                if (i < 0 || i >= n) break;
                sel = i;
                open_selected();
                break;
            }
            default: break;
        }
    }
};

Input read_event(const SDL_Event& e, bool pad_attached) {
    Input in;
    switch (e.type) {
        case SDL_QUIT: in.act = Act::Quit; break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
                // (The D-pad and the left stick are read as held, in setup_run: a
                // direction held repeats, faster the longer it is held.)
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: in.act = Act::Accept; break;
                case SDL_CONTROLLER_BUTTON_B: in.act = Act::Back; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: in.act = Act::PageUp; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: in.act = Act::PageDown; break;
                default: break;
            }
            break;
        case SDL_KEYDOWN:
            // With a controller attached the keyboard is not a second one:
            // the Steam Deck's Desktop Mode types keys from its own buttons
            // (kol_desktop.cpp key_hook), and every press would count twice.
            if (pad_attached) break;
            switch (e.key.keysym.sym) {
                case SDLK_LEFT: in.act = Act::Left; break;
                case SDLK_RIGHT: in.act = Act::Right; break;
                case SDLK_UP: in.act = Act::Up; break;
                case SDLK_DOWN: in.act = Act::Down; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: in.act = Act::Accept; break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: in.act = Act::Back; break;
                case SDLK_PAGEUP: in.act = Act::PageUp; break;
                case SDLK_PAGEDOWN: in.act = Act::PageDown; break;
                default: break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) { in.act = Act::Click; in.x = e.button.x; in.y = e.button.y; }
            break;
        case SDL_DROPFILE:
            if (e.drop.file) {
                in.act = Act::Drop;
                in.file = e.drop.file;
                SDL_free(e.drop.file);
            }
            break;
        case SDL_MOUSEWHEEL:
            in.act = e.wheel.y > 0 ? Act::Up : e.wheel.y < 0 ? Act::Down : Act::None;
            break;
        default: break;
    }
    return in;
}

}  // namespace

bool setup_run(const std::string& dir, bool fullscreen) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "setup: SDL: %s\n", SDL_GetError());
        return false;
    }
    // Always a window with its frame (asked 2026-11-18: one that can be
    // closed, and that a file can be dropped on), whatever the game will be;
    // its size from the screen's, 16:10, at most 1120 x 700. (A desktop
    // compositor such as the Deck's Gaming Mode shows it full screen anyway.)
    (void)fullscreen;
    int ww = 1120, wh = 700;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        ww = std::min(ww, usable.w * 85 / 100);
        wh = std::min(ww * 10 / 16, usable.h * 85 / 100);
        ww = std::min(ww, wh * 16 / 10);
    }
    SDL_Window* win = SDL_CreateWindow("Knight of Lodis Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, 0);
    if (!win || !ren) {
        std::fprintf(stderr, "setup: no window: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        return false;
    }
    std::vector<SDL_GameController*> pads;
    auto open_pads = [&] {
        for (SDL_GameController* p : pads) SDL_GameControllerClose(p);
        pads.clear();
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i))
                if (SDL_GameController* p = SDL_GameControllerOpen(i)) pads.push_back(p);
    };
    open_pads();

    Setup st;
    st.dir = dir;
    st.ui.r = ren;
    st.window = win;
    st.ui.regular.load(kRobotoRegular);
    st.ui.bold.load(kRobotoBold);
    // The game the boxes are for: the one that would start, else this
    // program's own.
    if (const int g = setup_game(dir); g >= 0) st.game = g;
    st.refresh();
    st.focus = st.rom_ok ? 4 : 0;

    // Desktop checks (dev builds): KOL_SETUP_KEYS="down,a,b,left,right,up,
    // pgup,pgdn,click:x:y,..." feeds those one a frame; KOL_SETUP_SHOT=<path
    // with %d> saves the window after each (and once at the start);
    // KOL_SETUP_START=<folder> is where the browser opens.
    std::vector<Input> script;
    const char* shot = nullptr;
    if constexpr (kDevTools) {
        if (const char* k = std::getenv("KOL_SETUP_KEYS")) {
            std::string t;
            for (const char* p = k;; ++p) {
                if (*p && *p != ',') { t += *p; continue; }
                Input in;
                int x = 0, y = 0;
                if (t == "left") in.act = Act::Left;
                else if (t == "right") in.act = Act::Right;
                else if (t == "up") in.act = Act::Up;
                else if (t == "down") in.act = Act::Down;
                else if (t == "a") in.act = Act::Accept;
                else if (t == "b") in.act = Act::Back;
                else if (t == "pgup") in.act = Act::PageUp;
                else if (t == "pgdn") in.act = Act::PageDown;
                else if (std::sscanf(t.c_str(), "click:%d:%d", &x, &y) == 2) { in.act = Act::Click; in.x = x; in.y = y; }
                else if (t == "quit") in.act = Act::Quit;
                else if (t.rfind("drop:", 0) == 0) { in.act = Act::Drop; in.file = t.substr(5); }
                if (in.act != Act::None) script.push_back(in);
                t.clear();
                if (!*p) break;
            }
        }
        shot = std::getenv("KOL_SETUP_SHOT");
        if (const char* start = std::getenv("KOL_SETUP_START")) st.path = start;
    }
    std::size_t script_at = 0;
    int shots = 0;
    const bool scripted = !script.empty() || shot;

    bool play = false, done = false;
    while (!done) {
        std::vector<Input> inputs;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) { open_pads(); continue; }
            Input in = read_event(e, !pads.empty());
            if (in.act == Act::Click) {
                // (The mouse is in the window's points; the layout in the
                // renderer's pixels, more of them on a high-density display.)
                int ww = 0, wh = 0, ow = 0, oh = 0;
                SDL_GetWindowSize(win, &ww, &wh);
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                if (ww > 0 && wh > 0) { in.x = in.x * ow / ww; in.y = in.y * oh / wh; }
            }
            if (in.act != Act::None) inputs.push_back(in);
        }
        // The pads' direction, held: at once, again after 350 ms, then every
        // 90 ms, every 40 after 1.2 s and every picture after 2.5 s (asked
        // 2026-11-18, for a long folder). The D-pad, or the left stick past
        // half way.
        {
            Act dir = Act::None;
            for (SDL_GameController* p : pads) {
                const int ax = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
                const int ay = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
                if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) dir = Act::Up;
                else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) dir = Act::Down;
                else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) dir = Act::Left;
                else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) dir = Act::Right;
                if (dir != Act::None) break;
            }
            static Act held = Act::None;
            static Uint32 held_at = 0, next_at = 0;
            const Uint32 now = SDL_GetTicks();
            if (dir != held) {
                held = dir;
                held_at = now;
                next_at = now + 350;
                if (dir != Act::None) inputs.push_back({dir, 0, 0});
            } else if (dir != Act::None && static_cast<Sint32>(now - next_at) >= 0) {
                const Uint32 t = now - held_at;
                next_at = now + (t > 2500 ? 0u : t > 1200 ? 40u : 90u);
                inputs.push_back({dir, 0, 0});
            }
        }
        if (script_at < script.size()) inputs.push_back(script[script_at++]);
        else if (scripted && shots > static_cast<int>(script.size())) inputs.push_back({Act::Quit, 0, 0});
        st.poll_system_pick();
        for (const Input& in : inputs) {
            if (in.act == Act::Quit) { done = true; break; }
            if (st.system_pick_up()) continue;   // (the desktop's dialog has the player)
            if (in.act == Act::Drop) { st.dropped(in.file); continue; }
            if (st.picking >= 0) st.input_browser(in);
            else if (st.input_main(in)) { play = true; done = true; break; }
        }
        SDL_GetRendererOutputSize(ren, &st.ui.w, &st.ui.h);
        st.ui.k = std::min(st.ui.w / 1280.0f, st.ui.h / 800.0f);
        if (st.picking >= 0) st.draw_browser();
        else st.draw_main();
        if (shot && (shots == 0 || !inputs.empty())) {
            char name[1024];
            std::snprintf(name, sizeof name, shot, shots);
            if (SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, st.ui.w, st.ui.h, 24, SDL_PIXELFORMAT_BGR24)) {
                if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_BGR24, sf->pixels, sf->pitch) == 0) SDL_SaveBMP(sf, name);
                SDL_FreeSurface(sf);
            }
            ++shots;
        } else if (scripted && inputs.empty()) {
            ++shots;
        }
        SDL_RenderPresent(ren);
        if (!scripted) SDL_Delay(8);
    }
    for (SDL_GameController* p : pads) SDL_GameControllerClose(p);
    st.ui.regular.drop();
    st.ui.bold.drop();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (play && st.rom_ok) setup_game_set(dir, st.game);   // the game to start (kol_desktop)
    return play && st.rom_ok;
}

// ── A page of text, drawn on the CPU (setup_text_page) ──────────────────
namespace {

// The setup screen's Roboto, its sizes baked into coverage bitmaps kept here
// (no renderer: the page is a picture the game's window shows as an overlay).
struct CpuFace {
    stbtt_fontinfo info{};
    const unsigned char* data = nullptr;
    bool ok = false;
    struct Baked {
        int px = 0, side = 0, ascent = 0;
        std::vector<unsigned char> cover;
        stbtt_bakedchar ch[96];
    };
    std::vector<Baked> sizes;
    explicit CpuFace(const unsigned char* ttf) : data(ttf) {
        ok = stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)) != 0;
    }
    const Baked* at(int px) {
        if (!ok || px < 4) return nullptr;
        for (const Baked& b : sizes)
            if (b.px == px) return &b;
        Baked b;
        b.px = px;
        for (b.side = 256;; b.side *= 2) {
            b.cover.assign(static_cast<std::size_t>(b.side) * b.side, 0);
            if (stbtt_BakeFontBitmap(data, 0, static_cast<float>(px), b.cover.data(), b.side, b.side, 32, 96, b.ch) > 0 ||
                b.side >= 4096)
                break;
        }
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        b.ascent = static_cast<int>(asc * stbtt_ScaleForPixelHeight(&info, static_cast<float>(px)) + 0.5f);
        if (sizes.size() > 8) sizes.erase(sizes.begin());   // (window sizes come and go)
        sizes.push_back(std::move(b));
        return &sizes.back();
    }
    int width(const std::string& t, int px) {
        const Baked* b = at(px);
        if (!b) return 0;
        float x = 0.0f;
        for (unsigned char c : t)
            if (c >= 32 && c < 128) x += b->ch[c - 32].xadvance;
        return static_cast<int>(x + 0.5f);
    }
    // `t` with its top at y, blended over w x h ARGB.
    void draw(std::uint32_t* px_out, int w, int h, int x, int y, const std::string& t, int px, Rgb c) {
        const Baked* b = at(px);
        if (!b) return;
        float pen = static_cast<float>(x);
        for (unsigned char ch : t) {
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar& g = b->ch[ch - 32];
            const int dx = static_cast<int>(pen + g.xoff + 0.5f), dy = y + b->ascent + static_cast<int>(g.yoff);
            for (int yy = 0; yy < g.y1 - g.y0; ++yy) {
                const int Y = dy + yy;
                if (Y < 0 || Y >= h) continue;
                for (int xx = 0; xx < g.x1 - g.x0; ++xx) {
                    const int X = dx + xx;
                    if (X < 0 || X >= w) continue;
                    const unsigned a = b->cover[static_cast<std::size_t>(g.y0 + yy) * b->side + g.x0 + xx];
                    if (!a) continue;
                    std::uint32_t& d = px_out[static_cast<std::size_t>(Y) * w + X];
                    auto mix = [a](unsigned from, unsigned to) { return (from * (255 - a) + to * a + 127) / 255; };
                    d = 0xFF000000u | (mix((d >> 16) & 0xFF, c.r) << 16) | (mix((d >> 8) & 0xFF, c.g) << 8) |
                        mix(d & 0xFF, c.b);
                }
            }
            pen += g.xadvance;
        }
    }
};

void fill_rect(std::uint32_t* px, int w, int h, int x0, int y0, int ww, int hh, Rgb c) {
    const std::uint32_t v = 0xFF000000u | (static_cast<std::uint32_t>(c.r) << 16) |
                            (static_cast<std::uint32_t>(c.g) << 8) | c.b;
    for (int y = std::max(0, y0); y < std::min(h, y0 + hh); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x0 + ww); ++x) px[static_cast<std::size_t>(y) * w + x] = v;
}

}  // namespace

void setup_text_page(const std::string& title, const std::string& text, const std::string& hint, int w, int h,
                     int first, std::uint32_t* argb, int* lines_out, int* page_out) {
    static CpuFace regular(kRobotoRegular), bold(kRobotoBold);
    if (lines_out) *lines_out = 0;
    if (page_out) *page_out = 1;
    if (!argb || w <= 0 || h <= 0) return;
    const float k = std::max(0.5f, h / 800.0f);
    auto px = [k](float v) { return std::max(1, static_cast<int>(v * k + 0.5f)); };
    const int margin = px(32), size = px(18), line_h = px(25), title_size = px(26), bar = px(64), foot = px(48);
    fill_rect(argb, w, h, 0, 0, w, h, kPaper);
    // The text's lines: each of the file's kept, a longer one wrapped at spaces
    // (its indent kept); tabs as four spaces.
    std::vector<std::string> lines;
    const int max_w = w - 2 * margin;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t nl = text.find('\n', at);
        std::string src = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        for (std::size_t t; (t = src.find('\t')) != std::string::npos;) src.replace(t, 1, "    ");
        if (!src.empty() && src.back() == '\r') src.pop_back();
        if (regular.width(src, size) <= max_w) { lines.push_back(src); continue; }
        std::size_t indent = src.find_first_not_of(' ');
        if (indent == std::string::npos || indent > 16) indent = 0;
        const std::string lead(indent, ' ');
        std::string line = lead, word;
        auto flush = [&] {
            if (word.empty()) return;
            const bool fresh = line.size() == lead.size();
            const std::string tried = fresh ? line + word : line + " " + word;
            if (!fresh && regular.width(tried, size) > max_w) {
                lines.push_back(line);
                line = lead + word;
            } else {
                line = tried;
            }
            word.clear();
        };
        for (std::size_t i = indent; i < src.size(); ++i) {
            if (src[i] == ' ') flush();
            else word += src[i];
        }
        flush();
        if (line.size() > lead.size()) lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const int top = bar + px(16), bottom = h - foot - px(8);
    const int page = std::max(1, (bottom - top) / line_h);
    first = std::clamp(first, 0, std::max(0, static_cast<int>(lines.size()) - page));
    for (int i = 0; i < page && first + i < static_cast<int>(lines.size()); ++i)
        regular.draw(argb, w, h, margin, top + i * line_h, lines[static_cast<std::size_t>(first + i)], size, kInk);
    // The title bar and the foot, over the text's edges.
    fill_rect(argb, w, h, 0, 0, w, bar, kAccent);
    bold.draw(argb, w, h, margin, (bar - title_size) / 2 - px(2), title, title_size, kWhite);
    if (static_cast<int>(lines.size()) > page) {
        char where[48];
        std::snprintf(where, sizeof where, "%d-%d of %d", first + 1,
                      std::min(first + page, static_cast<int>(lines.size())), static_cast<int>(lines.size()));
        const int ws = regular.width(where, size);
        regular.draw(argb, w, h, w - margin - ws, (bar - size) / 2 - px(2), where, size, kWhite);
    }
    fill_rect(argb, w, h, 0, h - foot, w, foot, kOff);
    fill_rect(argb, w, h, 0, h - foot, w, std::max(1, px(1)), kLine);
    regular.draw(argb, w, h, margin, h - foot + (foot - size) / 2 - px(2), hint, size, kSoft);
    if (lines_out) *lines_out = static_cast<int>(lines.size());
    if (page_out) *page_out = page;
}

}  // namespace kol

#else   // Android has SetupActivity.java.

namespace kol {
bool setup_file_is(const std::string&, const char*) { return false; }
std::string setup_mod_path(const std::string&) { return {}; }
bool setup_mod_take(const std::string&, const std::string&, std::string* why, bool) { *why = "Not here."; return false; }
void setup_mod_remove(const std::string&) {}
std::string setup_game_dir(const std::string& dir, int) { return dir; }
std::string setup_program(int) { return {}; }
bool setup_rom_in_place(const std::string&, int) { return false; }
int setup_game(const std::string&) { return kThisCart; }
void setup_game_set(const std::string&, int) {}
int setup_file_dialog(int, std::string*) { return 0; }
bool setup_file_dialog_poll(std::string*) { return false; }
bool setup_run(const std::string&, bool) { return true; }
void setup_text_page(const std::string&, const std::string&, const std::string&, int, int, int, std::uint32_t*,
                     int*, int*) {}
}  // namespace kol

#endif
