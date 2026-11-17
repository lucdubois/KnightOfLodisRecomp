// kol_map_settings.cpp — see the header.
#include "kol_map_settings.h"
#include "kol_settings.h"   // settings_dir

#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace kol {
namespace {

// map_settings.cfg as it was when the game was built (CMake: a raw string).
constexpr const char kBaked[] =
#include "kol_map_settings_baked.inc"
    ;

constexpr const char* kKeys[] = {"indoor",    "no_look",   "no_foam",         "no_puddles",
                                 "model_ao",  "wet_floor", "bg1_props",       "cone_window_tip",
                                 "ship_light", "flames",   "smoke",           "round_light",
                                 "polished_floor", "fixed_sun", "window_unit_shadows", "rain_rises",
                                 "glow_flame", "magic_circle", "no_prop_foot", "one_foot", "small_look"};
static_assert(sizeof kKeys / sizeof kKeys[0] == static_cast<int>(MapFlag::Count), "a key per flag");

constexpr int kMaxMap = 512;
using Flags = std::bitset<kMaxMap>[static_cast<int>(MapFlag::Count)];

void parse(const std::string& text, const char* from, Flags& out) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string key;
        if (!(words >> key)) continue;
        int f = -1;
        for (int i = 0; i < static_cast<int>(MapFlag::Count); ++i)
            if (key == kKeys[i]) f = i;
        if (f < 0) {
            std::fprintf(stderr, "map settings: %s line %d: unknown key '%s'\n", from, n, key.c_str());
            continue;
        }
        int map = 0;
        while (words >> map)
            if (map >= 0 && map < kMaxMap) out[f].set(static_cast<std::size_t>(map));
    }
}

const Flags& flags() {
    static Flags f;
    static const bool loaded = [] {
        std::string path;
        if (const char* e = std::getenv("KOL_MAP_SETTINGS")) path = e;
        else if (const std::string dir = settings_dir(); !dir.empty()) path = dir + "/map_settings.cfg";
        std::ifstream file(path.empty() ? std::string("/nonexistent") : path);
        if (file) {
            std::stringstream text;
            text << file.rdbuf();
            parse(text.str(), path.c_str(), f);
            std::fprintf(stderr, "map settings: read %s\n", path.c_str());
        } else {
            parse(kBaked, "built-in map_settings.cfg", f);
        }
        return true;
    }();
    (void)loaded;
    return f;
}

}  // namespace

bool map_flag(MapFlag flag, int map) {
    return map >= 0 && map < kMaxMap && flags()[static_cast<int>(flag)].test(static_cast<std::size_t>(map));
}

}  // namespace kol
