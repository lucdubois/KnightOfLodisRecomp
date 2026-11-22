// kol_states.cpp — see the header.

#include "kol_states.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "kol_menu.h"
#include "kol_settings.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"

namespace kol {
namespace {

// A load picked on the Save States screen goes through this slot number;
// the path hook answers it with the picked file.
constexpr int kPickSlot = 90;
// The pad's combos (gbarecomp: L3 + R1 saves slot 1, L3 + L1 loads it).
constexpr int kComboSlot = 1;

constexpr std::uint32_t kRoster = 0x02001420u;   // unit record 0: Alphonse
constexpr std::uint32_t kLevel = 0x52u, kName = 0x30u;
constexpr std::uint32_t kMapNumber = 0x02029D40u;

std::mutex g_mu;
std::vector<StateEntry> g_list;
bool g_list_ok = false;
std::string g_picked;

std::string dir() {
    static const std::string d = [] {
        if (const char* e = std::getenv("KOL_STATES_DIR"); e && *e) return std::string(e);
        const std::string s = settings_dir();
        // A mod (a patched ROM) keeps its own numbered states.
        return s.empty() ? std::string() : s + "/states" + kol::rom_file_tag();
    }();
    return d;
}

std::string path_of(int n, const char* ext) {
    char name[32];
    std::snprintf(name, sizeof name, "/%04d.%s", n, ext);
    return dir() + name;
}

// The numbers of the .state files there.
std::vector<int> numbers() {
    std::vector<int> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir(), ec)) {
        const std::string f = e.path().filename().string();
        if (f.size() == 10 && f.compare(4, 6, ".state") == 0) {
            const int n = std::atoi(f.substr(0, 4).c_str());
            if (n > 0) out.push_back(n);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The next number to save: one past the highest ever given (kept in
// `next`), so a deleted state's number is never given again (a synced copy
// of the old one would be taken for the new).
int next_number(const std::vector<int>& have) {
    int n = have.empty() ? 1 : have.back() + 1;
    if (std::FILE* f = std::fopen((dir() + "/next").c_str(), "r")) {
        int kept = 0;
        if (std::fscanf(f, "%d", &kept) == 1) n = std::max(n, kept);
        std::fclose(f);
    }
    return n;
}

const char* on_path(int slot, int save) {
    static std::string out;
    if (dir().empty()) return nullptr;
    if (slot == kPickSlot && !save) {
        std::lock_guard<std::mutex> lock(g_mu);
        out = g_picked;
        return out.c_str();
    }
    if (slot != kComboSlot || !setting_on(Setting::DebugNumberedStates)) return nullptr;
    std::error_code ec;
    std::filesystem::create_directories(dir(), ec);
    const std::vector<int> have = numbers();
    if (save) {
        const int n = next_number(have);
        out = path_of(n, "state");
        if (std::FILE* f = std::fopen((dir() + "/next").c_str(), "w")) {
            std::fprintf(f, "%d\n", n + 1);
            std::fclose(f);
        }
    } else {
        // None yet: a file that is not there, so the load fails (logged)
        // rather than taking slot 1 behind the player's back.
        out = have.empty() ? dir() + "/none.state" : path_of(have.back(), "state");
    }
    std::fprintf(stderr, "states: %s %s\n", save ? "save" : "load", out.c_str());
    return out.c_str();
}

// ── PNG (deflate's stored blocks: no compression, read back the same) ─────
std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0) {
    static std::uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        made = true;
    }
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}

void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
    be32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    be32(png, crc32(td.data(), td.size()));
}

bool write_png(const std::string& path, const std::uint8_t* rgb, int w, int h) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), rgb + static_cast<std::size_t>(y) * w * 3,
                   rgb + static_cast<std::size_t>(y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) { a = (a + v) % 65521u; b = (b + a) % 65521u; }
    for (std::size_t o = 0; o < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - o);
        z.push_back(o + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(o),
                 raw.begin() + static_cast<std::ptrdiff_t>(o + n));
        o += n;
    }
    be32(z, (b << 16) | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, static_cast<std::uint32_t>(w));
    be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// Reads back a PNG written above (stored blocks, filter none); false for
// anything else.
bool read_png(const std::string& path, std::vector<std::uint8_t>* rgb, int* w, int* h) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<std::uint8_t> d;
    std::uint8_t buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    auto rd32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    };
    if (d.size() < 33 || std::memcmp(d.data() + 12, "IHDR", 4) != 0) return false;
    *w = static_cast<int>(rd32(16));
    *h = static_cast<int>(rd32(20));
    if (*w <= 0 || *h <= 0 || *w > 2048 || *h > 2048 || d[24] != 8 || d[25] != 2) return false;
    std::vector<std::uint8_t> z;
    for (std::size_t o = 8; o + 12 <= d.size();) {
        const std::uint32_t len = rd32(o);
        if (o + 12 + len > d.size()) return false;
        if (std::memcmp(d.data() + o + 4, "IDAT", 4) == 0)
            z.insert(z.end(), d.begin() + static_cast<std::ptrdiff_t>(o + 8),
                     d.begin() + static_cast<std::ptrdiff_t>(o + 8 + len));
        o += 12 + len;
    }
    std::vector<std::uint8_t> raw;
    for (std::size_t o = 2; o + 5 <= z.size();) {
        if ((z[o] & 0x06) != 0) return false;   // not a stored block
        const std::size_t n = z[o + 1] | (z[o + 2] << 8);
        if (o + 5 + n > z.size()) return false;
        raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(o + 5),
                   z.begin() + static_cast<std::ptrdiff_t>(o + 5 + n));
        const bool last = z[o] & 1;
        o += 5 + n;
        if (last) break;
    }
    const std::size_t row = static_cast<std::size_t>(*w) * 3 + 1;
    if (raw.size() < row * static_cast<std::size_t>(*h)) return false;
    rgb->resize(static_cast<std::size_t>(*w) * *h * 3);
    for (int y = 0; y < *h; ++y) {
        if (raw[row * y] != 0) return false;
        std::memcpy(rgb->data() + static_cast<std::size_t>(y) * *w * 3, raw.data() + row * y + 1, row - 1);
    }
    return true;
}

std::string ascii_name(const std::uint8_t* ew) {
    std::string s;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const std::uint8_t c = ew[kRoster - 0x02000000u + kName + i];
        if (c == 0xFF) break;
        if (c < 26) s += static_cast<char>('A' + c);
        else if (c < 52) s += static_cast<char>('a' + c - 26);
        else if (c == 0x89) s += ' ';
    }
    return s;
}

void on_saved(const char* path) {
    const std::string p = path ? path : "";
    if (dir().empty() || p.rfind(dir() + "/", 0) != 0) return;   // a slot's, not ours
    const std::string stem = p.substr(0, p.size() - 6);   // less ".state"
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (ppu && ppu->has_latched_framebuffer())
        write_png(stem + ".png", ppu->latched_framebuffer(), static_cast<int>(ppu->render_width()),
                  static_cast<int>(ppu->render_height()));
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (std::FILE* f = std::fopen((stem + ".txt").c_str(), "w")) {
        const std::time_t t = std::time(nullptr);
        char when[32] = "";
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        if (ew) {
            const auto u16 = [ew](std::uint32_t a) { return ew[a - 0x02000000u] | (ew[a - 0x02000000u + 1] << 8); };
            std::fprintf(f, "level=%d\nname=%s\nmap=%d\n", ew[kRoster - 0x02000000u + kLevel],
                         ascii_name(ew).c_str(), u16(kMapNumber));
        }
        std::fprintf(f, "time=%s\nframe=%llu\n", when,
                     ppu ? static_cast<unsigned long long>(ppu->latched_frame()) : 0ull);
        std::fclose(f);
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_list_ok = false;
    }
    menu_confirm_sound();
    std::fprintf(stderr, "states: saved %s (+ .png, .txt)\n", p.c_str());
}

StateEntry read_entry(int n) {
    StateEntry e;
    e.number = n;
    if (std::FILE* f = std::fopen(path_of(n, "txt").c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = '\0';
            if (std::strncmp(line, "level=", 6) == 0) e.level = std::atoi(line + 6);
            else if (std::strncmp(line, "map=", 4) == 0) e.map = std::atoi(line + 4);
            else if (std::strncmp(line, "time=", 5) == 0) e.when.assign(line + 5, std::min<std::size_t>(16, std::strlen(line + 5)));
        }
        std::fclose(f);
    }
    return e;
}

}  // namespace

void states_save_new() {
    gbarecomp::runtime_request_state_save(kComboSlot);
}

void states_load_latest() {
    gbarecomp::runtime_request_state_load(kComboSlot);
}

void states_install() {
    static bool done = false;
    if (done) return;
    done = true;
    gbarecomp::g_runtime_state_path_hook = &on_path;
    gbarecomp::g_runtime_state_saved_hook = &on_saved;
}

std::vector<StateEntry> states_list() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_list_ok && !dir().empty()) {
        g_list.clear();
        const std::vector<int> have = numbers();
        for (auto it = have.rbegin(); it != have.rend(); ++it) g_list.push_back(read_entry(*it));
        g_list_ok = true;
    }
    return g_list;
}

bool states_thumb(int number, int tw, int th, std::vector<std::uint32_t>* argb, int* w, int* h) {
    std::vector<std::uint8_t> rgb;
    int sw = 0, sh = 0;
    if (dir().empty() || !read_png(path_of(number, "png"), &rgb, &sw, &sh)) return false;
    const float k = std::min(static_cast<float>(tw) / sw, static_cast<float>(th) / sh);
    *w = std::max(1, static_cast<int>(sw * k));
    *h = std::max(1, static_cast<int>(sh * k));
    argb->assign(static_cast<std::size_t>(*w) * *h, 0xFF000000u);
    for (int y = 0; y < *h; ++y)
        for (int x = 0; x < *w; ++x) {
            // The box of source pixels this one covers, averaged.
            const int x0 = x * sw / *w, x1 = std::max(x0 + 1, (x + 1) * sw / *w);
            const int y0 = y * sh / *h, y1 = std::max(y0 + 1, (y + 1) * sh / *h);
            unsigned s[3] = {}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++n)
                    for (int c = 0; c < 3; ++c) s[c] += rgb[(static_cast<std::size_t>(yy) * sw + xx) * 3 + c];
            (*argb)[static_cast<std::size_t>(y) * *w + x] =
                0xFF000000u | (s[0] / n) << 16 | (s[1] / n) << 8 | (s[2] / n);
        }
    return true;
}

bool states_delete(int number) {
    if (dir().empty() || number <= 0) return false;
    std::error_code ec;
    const bool ok = std::filesystem::remove(path_of(number, "state"), ec);
    std::filesystem::remove(path_of(number, "png"), ec);
    std::filesystem::remove(path_of(number, "txt"), ec);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_list_ok = false;
    }
    std::fprintf(stderr, "states: deleted %04d%s\n", number, ok ? "" : " (no .state)");
    return ok;
}

void states_load(int number) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_picked = path_of(number, "state");
    }
    std::fprintf(stderr, "states: load %04d picked\n", number);
    gbarecomp::runtime_request_state_load(kPickSlot);
}

void states_test_tick() {
    static const std::string spec = [] {
        const char* e = std::getenv("KOL_STATES_TEST");
        return std::string(e ? e : "");
    }();
    if (spec.empty()) return;
    static int frame = 0;
    ++frame;
    for (std::size_t at = 0; at < spec.size();) {
        const std::size_t comma = spec.find(',', at);
        const std::string t = spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        int n = 0, f = 0;
        if (t.size() > 1 && t[0] == 's' && std::atoi(t.c_str() + 1) == frame) gbarecomp::runtime_request_state_save(kComboSlot);
        if (t.size() > 1 && t[0] == 'l' && std::atoi(t.c_str() + 1) == frame) gbarecomp::runtime_request_state_load(kComboSlot);
        if (std::sscanf(t.c_str(), "p%d@%d", &n, &f) == 2 && f == frame) states_load(n);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
}

}  // namespace kol
