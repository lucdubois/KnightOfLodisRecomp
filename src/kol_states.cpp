// kol_states.cpp — see the header.

#include "kol_states.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "kol_dev.h"
#include "kol_menu.h"
#include "kol_settings.h"
#include "kol_vs_cpu.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"
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
    if (const char* vs = vs_cpu_state_path(slot, save)) return vs;   // VS Computer's snapshot
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
    if (vs_cpu_state_saved(path)) return;   // VS Computer's snapshot: the copy starts from it
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

// KOL_AI_ARMY=1 (dev builds; 2026-11-21, the VS-against-the-AI experiment):
// the game's own AI plays this player's army. A battle entry's flag byte
// (+0x17) has bit 1 set for the armies the game plays itself (map 2's
// allied army: 0x42; the player's 0x40, its leader 0x41; enemies 0xC0/0xC1).
// Set on the player's own units (+0x00 = 0x80 | roster index), the enemy
// turn runs on into theirs, the AI phase flag stays up and they act by
// themselves (BL_enemy_turn: Alphonse done +5600 frames, Allos +6400; the
// control run waits for input). A host poke, for the experiment only: the
// real thing goes through a reviewed hook.
//
// In VS Mode the battle starts with each side sending its units' records and
// battle entries to the other, checksummed: set during that exchange, the bit
// broke it (the receiver refused the block, both resent it for ever on "Now
// loading"). So it is a key now: F7 (desktop, with KOL_AI_ARMY=1) hands the
// army to the AI once the battle runs, and takes it back.
namespace {
std::atomic<bool> g_ai_army_active{false};
// The battle slots the switch took (this game's own units): the roster byte
// to give back, 0 = not taken. Written by ai_army_tick, read by the hook.
std::uint8_t g_taken[16] = {};
unsigned long g_frame = 0;   // the frame hook's last frame, for the log
bool g_turn_logged = false;   // sub-state 2's answer logged since the last fork

// How VS Mode decides who plays (2026-11-22, two linked games):
//
// The turn fork 0x0806D924 (game.toml) answers a map object for a unit the
// game's AI plays, 0 for the player's choice. Its first test, 0x080A16F8, is
// "this game's own phase": story flag 7 (the second attacker's phase) XOR
// flag 20 (set in the first attacker's game). On its own phase the game asks
// the AI picker 0x08097098; on the other's, the link (0x080A1634, then
// 0x080A1130 for the unit the other game sent). The picker, and then the
// unit-turn machine's sub-state 2, ask story variables 0 / 1 whether a side is
// the computer's (on_story_var below): answered yes on this game's own phase,
// the picker chooses the unit and sub-state 2 runs the AI controller, which
// plans the move and the action; the moves cross the link as a player's do.
// Answering the fork itself with a unit (tried first) skipped the plan: the
// unit walked to cell 0,0, its destination never set. The fork now only logs
// (each unit's HP and cell). This game's own units: the slots linked to its
// roster (+0x00 = 0x80 | index) and not the other game's (+0x17 bit 5), or
// the ones ai_army_tick took in a story battle.
constexpr char kForkHookId[] = "kol.ai-army-fork";
constexpr std::uint32_t kTurnFork = 0x0806D924u;

int on_turn_fork(std::uint32_t, int, ArmCpuState* cpu) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew) return 0;
    auto rd32 = [&](std::uint32_t a) {
        const std::uint8_t* p = ew + (a - 0x02000000u);
        return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    };
    const bool vs = ew[0x12E0] == 4u;                   // VS Mode (0x08143070: mode 4)
    const bool f7 = (ew[0x11CC] & 0x80u) != 0u, f20 = (ew[0x11CE] & 0x10u) != 0u;
    const bool on = g_ai_army_active.load(std::memory_order_relaxed);
    unsigned own = 0, done = 0;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t* e = ew + 0x1AA0u + i * 0x1Cu;
        if (g_taken[i] || ((e[0] & 0x80u) && e[0] != 0xFFu && !(e[0x17] & 0x20u))) own |= 1u << i;
        if (e[0x0C]) done |= 1u << i;
    }
    // The units on the map: slot:HP@cell, for the log.
    char units[160] = {};
    {
        const std::uint32_t list = rd32(0x0202A238u);
        std::size_t at = 0;
        if (list >= 0x02000000u && list + 26u * 0xD0u <= 0x02040000u)
            for (int n = 0; n < 26 && at + 24 < sizeof units; ++n) {
                const std::uint32_t obj = list + n * 0xD0u;
                const std::uint32_t rec = rd32(obj);
                if (rec < 0x02001420u || (rec - 0x02001420u) % 0x68u != 0u || (rec - 0x02001420u) / 0x68u >= 16u) continue;
                const std::uint8_t* r = ew + (rec - 0x02000000u);
                if (r[0x4E] == 0u) continue;
                const std::uint8_t* o = ew + (obj - 0x02000000u);
                at += std::snprintf(units + at, sizeof units - at, " %u:%u@%u,%u", (rec - 0x02001420u) / 0x68u,
                                    r[0x40] | r[0x41] << 8, (o[0x10] | o[0x11] << 8) >> 5, (o[0x14] | o[0x15] << 8) >> 5);
            }
    }
    std::fprintf(stderr, "ai-army: frame %lu fork caller %08X vs=%d f7=%d f20=%d on=%d own=%04X done=%04X |%s\n",
                 g_frame, cpu->R[14] & ~1u, vs, f7, f20, on, own, done, units);
    g_turn_logged = false;
    return 0;
}

// The picker's question "is side n the computer's?" (story variable n, read by
// 0x08142FAC): yes for this game's own side, in VS Mode, with the switch on,
// asked from the picker's three calls only. The picker then runs its own
// selection and planning for this side, as for the enemy in a story battle.
// Own side: the side whose phase it is when the phase is this game's own
// (flag 7 XOR flag 20, see the fork), which is the side the picker asks about.
constexpr char kSideHookId[] = "kol.ai-army-side";
constexpr std::uint32_t kStoryVar = 0x08142FACu;

int on_story_var(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t ret = cpu->R[14] & ~1u;
    // The unit-turn machine's sub-state 2 (0x0806AE5C) runs the AI controller
    // (task +0x1A4, 0x0809F511, installed at every battle's start) in VS Mode
    // only when variable 1 is set, after variable 0 or flag 7: VS against the
    // computer on side 1 seems built in. Both its questions (0x0806AE68 for
    // variable 0, 0x0806AE80 for variable 1) are answered yes on this game's
    // own phase, whichever side it is. Without this the picker chose the unit
    // but no plan was made: the move went to cell 0,0.
    const bool sub2 = ret == 0x0806AE6Cu || ret == 0x0806AE84u;
    if (!sub2 && ret != 0x08097122u && ret != 0x08097234u && ret != 0x08097246u) return 0;
    if (!g_ai_army_active.load(std::memory_order_relaxed)) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew || ew[0x12E0] != 4u) return 0;
    const bool f7 = (ew[0x11CC] & 0x80u) != 0u, f20 = (ew[0x11CE] & 0x10u) != 0u;
    if (f7 == f20) return 0;                             // not this game's own phase
    const std::uint32_t side = f7 ? 1u : 0u;             // the phase's side
    if (!sub2 && (cpu->R[0] & 0xFFFFu) != side) return 0;
    if (!sub2 || !g_turn_logged)   // sub-state 2 asks every frame of the AI's turn
        std::fprintf(stderr, "ai-army: frame %lu %s asks variable %u (from %08X): computer\n", g_frame,
                     sub2 ? "turn" : "picker", static_cast<unsigned>(cpu->R[0] & 0xFFFFu), ret - 4u);
    if (sub2 && (cpu->R[0] & 0xFFFFu) == 1u) g_turn_logged = true;
    cpu->R[0] = 1u;
    cpu->R[15] = ret;
    return 1;
}

// KOL_AI_TRACE=<pc>[,<pc>...] (dev, with KOL_AI_ARMY=1): every entry of those
// guest functions, with the frame, the caller and r0..r2. Chained onto
// gbarecomp's function-entry hook (as kol_fast_actions does).
std::vector<std::uint32_t> g_trace_pcs;
void (*g_trace_prev)(std::uint32_t) = nullptr;
void on_trace_entry(std::uint32_t pc) {
    if (g_trace_prev) g_trace_prev(pc);
    for (const std::uint32_t t : g_trace_pcs)
        if (t == (pc & ~1u)) {
            std::fprintf(stderr, "ai-trace: frame %lu %08X from %08X r0=%08X r1=%08X r2=%08X\n", g_frame, t,
                         (g_cpu.R[14] & ~1u) - 4u, g_cpu.R[0], g_cpu.R[1], g_cpu.R[2]);
            break;
        }
}
}  // namespace

void ai_army_install() {
    if (!ai_army_enabled()) return;
    if (vs_cpu_child()) g_ai_army_active.store(true);   // the computer's game: its army from the start
    if (const char* t = std::getenv("KOL_AI_TRACE"); t && *t) {
        for (const char* p = t; *p;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) break;
            g_trace_pcs.push_back(static_cast<std::uint32_t>(v) & ~1u);
            p = *end == ',' ? end + 1 : end;
        }
        g_trace_prev = g_runtime_fn_entry_hook;
        g_runtime_fn_entry_hook = &on_trace_entry;
    }
    if (!gba_mod_register_function_entry_plugin(kForkHookId, kTurnFork, 1, &on_turn_fork) ||
        !gba_mod_set_function_hook_enabled(kForkHookId, 1))
        std::fprintf(stderr, "ai-army: turn fork hook NOT installed\n");
    if (!gba_mod_register_function_entry_plugin(kSideHookId, kStoryVar, 1, &on_story_var) ||
        !gba_mod_set_function_hook_enabled(kSideHookId, 1))
        std::fprintf(stderr, "ai-army: picker side hook NOT installed\n");
}

bool ai_army_enabled() {
    if (vs_cpu_child()) return true;   // VS Computer's unseen copy: in every build
    if constexpr (!kol::kDevTools) return false;
    static const bool on = [] {
        const char* e = std::getenv("KOL_AI_ARMY");
        return e && *e == '1';
    }();
    return on;
}

bool ai_army_active() { return ai_army_enabled() && g_ai_army_active.load(std::memory_order_relaxed); }

void ai_army_toggle() {
    if (!ai_army_enabled()) return;
    const bool now = !g_ai_army_active.load();
    g_ai_army_active.store(now);
    std::fprintf(stderr, "ai-army: the AI %s this army\n", now ? "plays" : "no longer plays");
}

void ai_army_tick(unsigned long frame) {
    if (!ai_army_enabled()) return;
    g_frame = frame;
    // KOL_AI_STATE_LOG=1: the battle task's state bytes and the map cursor's
    // mode, logged when they change (the preparation's screens).
    static const bool state_log = [] {
        const char* e = std::getenv("KOL_AI_STATE_LOG");
        return e && *e == '1';
    }();
    // KOL_AI_DUMP=<frame>[,<frame>...]: EWRAM and IWRAM at those frames, to
    // ew_<frame>.bin / iw_<frame>.bin in the working directory (linked pairs
    // cannot save states).
    static const std::vector<unsigned long> dumps = [] {
        std::vector<unsigned long> v;
        if (const char* e = std::getenv("KOL_AI_DUMP"))
            for (const char* p = e; *p;) {
                char* end = nullptr;
                const unsigned long f = std::strtoul(p, &end, 10);
                if (end == p) break;
                v.push_back(f);
                p = *end == ',' ? end + 1 : end;
            }
        return v;
    }();
    for (const unsigned long f : dumps)
        if (f == frame)
            if (gba::GbaBus* b = gbarecomp::active_bus()) {
                char name[64];
                std::snprintf(name, sizeof name, "ew_%lu.bin", frame);
                if (FILE* o = std::fopen(name, "wb")) { std::fwrite(b->ewram_ptr(), 1, 0x40000, o); std::fclose(o); }
                std::snprintf(name, sizeof name, "iw_%lu.bin", frame);
                if (FILE* o = std::fopen(name, "wb")) { std::fwrite(b->iwram_ptr(), 1, 0x8000, o); std::fclose(o); }
            }
    if (state_log) {
        gba::GbaBus* b = gbarecomp::active_bus();
        const std::uint8_t* e = b ? b->ewram_ptr() : nullptr;
        const std::uint8_t* i = b ? b->iwram_ptr() : nullptr;
        if (e && i) {
            char now[96];
            std::snprintf(now, sizeof now, "prep=%02X phase=%02X stack=%02X.%02X.%02X.%02X sub=%02X.%02X cur=%02X list=%02X/%02X",
                          e[0x2A81A], e[0x2A81C], e[0x2A81D], e[0x2A81E], e[0x2A81F], e[0x2A820], e[0x2A82E],
                          e[0x2A82F], i[0xFD5], e[0x37B6C], e[0x37B6E]);
            static std::string last;
            if (last != now) {
                std::fprintf(stderr, "ai-state: frame %lu %s\n", frame, now);
                last = now;
            }
        }
    }
    // KOL_AI_ARMY_AT=<frame> (headless pairs): F7 at that absolute frame.
    static const unsigned long on_at = [] {
        const char* e = std::getenv("KOL_AI_ARMY_AT");
        return e ? std::strtoul(e, nullptr, 10) : 0ul;
    }();
    static bool fired = false;
    if (on_at && !fired && frame >= on_at) {
        fired = true;
        if (!g_ai_army_active.load()) ai_army_toggle();
    }
    gba::GbaBus* bus = gbarecomp::active_bus();
    std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew) return;
    // The slots taken (this game's own units, linked to its roster: +0x00 =
    // 0x80 | index, on either side) and their roster byte, to give back.
    // In VS Mode bit 1 alone did not do it (the child's own units, in the
    // enemy slots at 0xC1, stayed the player's), so they are made what the
    // game's own AI armies are: no roster link (0xFF) and bit 1 (map 2's
    // allied army: 0xFF / 0x42). The other game's units there are 0xFF with
    // +0x20 (theirs, over the link): never touched.
    std::uint8_t* taken = g_taken;
    // VS Mode needs no poke: the turn fork's hook decides (and the slots
    // are part of the battle start's checksummed exchange).
    if (ew[0x12E0] == 4u || vs_cpu_child()) return;
    const bool on = g_ai_army_active.load();
    for (int i = 0; i < 16; ++i) {
        std::uint8_t* e = ew + 0x1AA0 + i * 0x1C;
        if (taken[i] && (e[0x17] & 0x20)) {   // the slot now holds the other game's unit: let go
            e[0x17] &= static_cast<std::uint8_t>(~0x02u);
            taken[i] = 0;
            continue;
        }
        if (on) {
            if (!taken[i] && (e[0] & 0x80) && e[0] != 0xFF && !(e[0x17] & 0x20)) taken[i] = e[0];
            if (taken[i]) { e[0] = 0xFF; e[0x17] |= 0x02; }
        } else if (taken[i]) {
            e[0] = taken[i];
            e[0x17] &= static_cast<std::uint8_t>(~0x02u);
            taken[i] = 0;
        }
    }
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
