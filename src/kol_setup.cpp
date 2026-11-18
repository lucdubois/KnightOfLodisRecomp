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
#endif
namespace {

namespace fs = std::filesystem;

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
    int focus = 0;        // 0-2 the boxes, 3 Remove mod, 4 Play
    Box box[3];
    SDL_Rect remove_at{}, play_at{};
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
        const std::string rom = dir + "/totkol.gba", bios = dir + "/gba_bios.bin";
        rom_there = fs::exists(rom, ec);
        rom_ok = rom_there && setup_file_is(rom, kRomSha1);
        bios_there = fs::exists(bios, ec);
        bios_ok = bios_there && setup_file_is(bios, kBiosSha1);
        const std::string m = setup_mod_path(dir);
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

    void start_pick(int what) {
#if defined(__APPLE__)
        // A Mac: the system's Open panel, not the screen's browser.
        static const char* const kAsk[3] = {"Select the game's ROM (Tactics Ogre: The Knight of Lodis, USA)",
                                            "Select the GBA BIOS", "Select a mod (.ips, .ups or .bps)"};
        std::string file;
        const bool chosen = setup_mac_open_panel(kAsk[what], &file);
        if (window) SDL_RaiseWindow(window);
        if (!chosen) return;
        picking = what;
        take(file);
        if (picking == what) picking = -1;   // (refused: the message on the main page)
        return;
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
            if (!same_hex(sha1_of(data), kRomSha1)) { status = "Wrong file: not this game's ROM (the USA version is needed)."; return; }
            if (!write_file(dir + "/totkol.gba", data)) { status = "Copy failed."; return; }
        } else if (picking == 1) {
            if (!same_hex(sha1_of(data), kBiosSha1)) { status = "Not the GBA BIOS: the HLE BIOS will be used."; return; }
            if (!write_file(dir + "/gba_bios.bin", data)) { status = "Copy failed."; return; }
        } else {
            // A patch that does not apply, or changes the game's code, is
            // refused here when the ROM is in place to try it on.
            std::vector<std::uint8_t> rom, out;
            std::string summary, err;
            if (read_file(dir + "/totkol.gba", &rom) && same_hex(sha1_of(rom), kRomSha1) &&
                !gbarecomp::rom_patch_load_and_check(file, rom, &out, &summary, &err)) {
                status = "That mod can't be played: " + err + ".";
                return;
            }
            fs::create_directories(dir + "/mod", ec);
            for (const fs::directory_entry& e : fs::directory_iterator(dir + "/mod", ec)) fs::remove(e.path(), ec);
            if (!write_file(dir + "/mod/" + fs::path(file).filename().string(), data)) { status = "Copy failed."; return; }
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
        const int kind = same_hex(sha, kRomSha1) ? 0 : same_hex(sha, kBiosSha1) ? 1
                       : (ext == ".ips" || ext == ".ups" || ext == ".bps") ? 2 : -1;
        if (kind < 0) {
            status = fs::path(file).filename().string() +
                     " is not this game's ROM (the USA version), the GBA BIOS or a mod (.ips, .ups, .bps).";
            return;
        }
        const int was = picking;
        picking = kind;
        take(file);                          // (success: back to the main page)
        if (picking == kind) picking = was;  // (refused: where it was, with the message)
        focus = kind;
    }

    void remove_mod() {
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(dir + "/mod", ec)) fs::remove(e.path(), ec);
        refresh();
    }

    // ── The main page ───────────────────────────────────────────────────
    void draw_main() {
        Ui& u = ui;
        u.fill(0, 0, u.w, u.h, kPaper);
        // (The sizes, a window 800 high: larger than the first try, asked 2026-11-18.)
        const int title = u.px(48), name = u.px(40), small = u.px(26), button = u.px(36);
        int y = u.h / 18;
        u.text_mid(u.regular, u.w / 2, y, "Tactics Ogre: The Knight of Lodis", title, kInk);
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
    }

    // True: Play, with the ROM in place.
    bool input_main(const Input& in) {
        switch (in.act) {
            case Act::Left: if (focus <= 2) focus = std::max(0, focus - 1); else focus = 3; break;
            case Act::Right: if (focus <= 2) focus = std::min(2, focus + 1); else focus = 4; break;
            case Act::Down: if (focus <= 2) focus = focus == 0 ? 3 : 4; break;
            case Act::Up: if (focus >= 3) focus = focus == 3 ? 0 : 2; break;
            case Act::Accept:
                if (focus == 4 && rom_ok) return true;
                press_main(focus);
                break;
            case Act::Click: {
                const SDL_Point p{in.x, in.y};
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
        for (const Input& in : inputs) {
            if (in.act == Act::Quit) { done = true; break; }
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
    return play && st.rom_ok;
}

}  // namespace kol

#else   // Android has SetupActivity.java.

namespace kol {
bool setup_file_is(const std::string&, const char*) { return false; }
std::string setup_mod_path(const std::string&) { return {}; }
bool setup_run(const std::string&, bool) { return true; }
}  // namespace kol

#endif
