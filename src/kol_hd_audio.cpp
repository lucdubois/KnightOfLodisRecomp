#include "kol_hd_audio.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <iterator>
#include <vector>

#include "hd_instruments.h"

#if defined(__ANDROID__)
#include <SDL.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace kol {
namespace {

// The rows of src/kol_hd_map.inc: an instrument named by (bank, program),
// resolved to a bank index once the bank is loaded.
struct HdSampleRow {
    std::uint32_t wav;
    int bank, prog;
    float offset, gain_db;
    int kit_key;
    float roll_hz, accent;
    std::uint32_t song;   // 0: every song
    float intro_s, intro_db;
    int intro_ramp;
};
struct HdPsgRow {
    std::uint32_t song;
    int channel, bank, prog;
    float transpose, gain_db;
};
#include "kol_hd_map.inc"

constexpr char kBankName[] = "hd_instruments.bank";

std::mutex g_mu;
int g_state = 0;   // 0 not tried, 1 loaded, -1 missing or bad

// The whole file, or false. On Android through SDL, which for a relative path
// tries the app's files/ and then the APK's assets.
bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
#if defined(__ANDROID__)
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw) return false;
    const Sint64 n = SDL_RWsize(rw);
    bool ok = n > 0;
    if (ok) {
        out->resize(static_cast<std::size_t>(n));
        ok = SDL_RWread(rw, out->data(), 1, out->size()) == out->size();
    }
    SDL_RWclose(rw);
    return ok;
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long n = ok ? std::ftell(f) : -1;
    ok = n > 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out->resize(static_cast<std::size_t>(n));
        ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    }
    std::fclose(f);
    return ok;
#endif
}

#if !defined(__ANDROID__)
// The running executable's directory, with its separator, or "".
std::string exe_dir() {
    std::string p;
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) p = buf;
#elif defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) p.assign(buf, n);
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof buf) p.assign(buf, static_cast<std::size_t>(n));
#endif
    const std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}
#endif

// Where the bank may be, in order (kol_hd_audio.h).
std::vector<std::string> bank_paths() {
    std::vector<std::string> paths;
    if (const char* e = std::getenv("KOL_HD_BANK"))
        if (*e) paths.emplace_back(e);
#if defined(__ANDROID__)
    paths.emplace_back(kBankName);
#else
    const std::string dir = exe_dir();
    if (!dir.empty()) paths.push_back(dir + kBankName);
    paths.push_back(std::string("build/") + kBankName);
#endif
    return paths;
}

bool load() {
    std::vector<std::uint8_t> data;
    std::string found, tried;
    for (const std::string& p : bank_paths()) {
        if (read_file(p, &data)) { found = p; break; }
        tried += (tried.empty() ? "" : ", ") + p;
    }
    if (found.empty()) {
        std::fprintf(stderr, "hd_audio: no instrument bank (%s): HD plays as HQ stereo\n", tried.c_str());
        return false;
    }
    std::string err;
    if (!gba::gba_audio_set_hd_bank(data.data(), data.size(), &err)) {
        std::fprintf(stderr, "hd_audio: %s is not a usable bank (%s): HD plays as HQ stereo\n",
                     found.c_str(), err.c_str());
        return false;
    }
    data = {};   // the bank keeps its own copy
    const gba::HdBank* bank = gba::gba_audio_hd_bank();
    if (!bank || !bank->loaded()) {
        std::fprintf(stderr, "hd_audio: %s loaded no instruments: HD plays as HQ stereo\n", found.c_str());
        return false;
    }
    // A row whose instrument the bank lacks is left to the game's sample (or chip).
    std::vector<gba::HdSampleMap> samples;
    // KOL_HD_SAMPLE_MAP=<file>: trial rows for mock-ups, as KOL_HD_PSG_MAP
    // below: "wav bank prog offset gain_db [song [kit_key [intro_s intro_db
    // intro_ramp]]]" per line (wav and song in hex, song 0 = every song, kit
    // key -1 = pitched), each replacing the built-in row for its sample and
    // song; a kit_key plays that drum, unpitched.
    std::vector<HdSampleRow> srows(std::begin(kHdSamples), std::end(kHdSamples));
    if (const char* e = std::getenv("KOL_HD_SAMPLE_MAP")) {
        if (FILE* f = std::fopen(e, "r")) {
            char line[256];
            int n = 0;
            while (std::fgets(line, sizeof line, f)) {
                HdSampleRow t{0, 0, 0, 0.0f, 0.0f, -1, 0.0f, 0.7f, 0, 0.0f, 0.0f, 0};
                if (std::sscanf(line, "%x %d %d %f %f %x %d %f %f %d", &t.wav, &t.bank, &t.prog, &t.offset,
                                &t.gain_db, &t.song, &t.kit_key, &t.intro_s, &t.intro_db, &t.intro_ramp) < 5)
                    continue;
                bool replaced = false;
                for (HdSampleRow& r : srows)
                    if (r.wav == t.wav && r.song == t.song) { r = t; replaced = true; }
                if (!replaced) srows.push_back(t);
                ++n;
            }
            std::fclose(f);
            std::fprintf(stderr, "hd_audio: %d sample rows from %s\n", n, e);
        } else {
            std::fprintf(stderr, "hd_audio: KOL_HD_SAMPLE_MAP %s not readable\n", e);
        }
    }
    for (const HdSampleRow& r : srows) {
        // prog -1: the game's own sample (a song row keeping it where the
        // sample's general row would replace it).
        const int inst = r.prog < 0 ? -1 : bank->find(r.bank, r.prog);
        if (inst < 0 && r.prog >= 0) {
            std::fprintf(stderr, "hd_audio: sample 0x%08X: no instrument %d:%d in the bank, skipped\n",
                         r.wav, r.bank, r.prog);
            continue;
        }
        gba::HdSampleMap sm{r.wav, inst, r.offset, r.gain_db, r.kit_key, r.roll_hz, r.accent};
        sm.intro_s = r.intro_s;
        sm.intro_db = r.intro_db;
        sm.intro_ramp = r.intro_ramp != 0;
        sm.song = r.song;
        samples.push_back(sm);
    }
    std::vector<gba::HdPsgMap> psg;
    // KOL_HD_PSG=0 leaves every PSG channel to the chip (A/B of the PCM part).
    const char* psg_env = std::getenv("KOL_HD_PSG");
    const bool psg_on = !(psg_env && psg_env[0] == '0');
    // KOL_HD_PSG_MAP=<file>: trial rows for mock-ups recorded from the game,
    // "song channel bank prog transpose gain_db" per line (song in hex); a row
    // replaces the built-in one for its song and channel. Needs a bank built
    // with those instruments (build_bank.py --map).
    std::vector<HdPsgRow> rows;
    if (const char* e = std::getenv("KOL_HD_PSG_MAP")) {
        if (FILE* f = std::fopen(e, "r")) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                HdPsgRow r{};
                if (std::sscanf(line, "%x %d %d %d %f %f", &r.song, &r.channel, &r.bank, &r.prog,
                                &r.transpose, &r.gain_db) == 6)
                    rows.push_back(r);
            }
            std::fclose(f);
            std::fprintf(stderr, "hd_audio: %zu PSG rows from %s\n", rows.size(), e);
        } else {
            std::fprintf(stderr, "hd_audio: KOL_HD_PSG_MAP %s not readable\n", e);
        }
    }
    const std::size_t trial = rows.size();
    for (const HdPsgRow& r : kHdPsg) {
        bool replaced = false;
        for (std::size_t i = 0; i < trial; ++i)
            replaced |= rows[i].song == r.song && rows[i].channel == r.channel;
        if (!replaced) rows.push_back(r);
    }
    for (const HdPsgRow& r : rows) {
        if (!psg_on) break;
        const int inst = bank->find(r.bank, r.prog);
        if (inst < 0) {
            std::fprintf(stderr, "hd_audio: song 0x%08X channel %d: no instrument %d:%d in the bank, skipped\n",
                         r.song, r.channel, r.bank, r.prog);
            continue;
        }
        psg.push_back({r.song, r.channel, inst, r.transpose, r.gain_db});
    }
    gba::gba_audio_set_hd_map(samples, psg);
    std::fprintf(stderr, "hd_audio: %s, %zu instruments; %zu of %zu samples, %zu of %zu PSG channels mapped\n",
                 found.c_str(), bank->size(), samples.size(), sizeof kHdSamples / sizeof kHdSamples[0],
                 psg.size(), rows.size());
    return true;
}

}  // namespace

bool hd_audio_activate() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == 0) {
        g_state = load() ? 1 : -1;
        std::fflush(stderr);
    }
    return g_state > 0;
}

}  // namespace kol
