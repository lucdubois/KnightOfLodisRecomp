// kol_shaders.cpp — Options > Display > Shader (kol_shaders.h).

#include "kol_shaders.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "kol_cheevos.h"   // http_get
#include "kol_menu.h"      // menu_shaders_imported
#include "kol_settings.h"
#include "shader_preset.h"
#include "stb_image.h"   // (its implementation is kol_cheevos.cpp's: PNG only, from memory)

namespace kol {
namespace {

namespace fs = std::filesystem;

// A preset's lookup texture: PNG to RGBA8, top row first (read through SDL,
// as the presets are).
bool load_image(const char* path, std::vector<uint8_t>* rgba, int* w, int* h) {
    SDL_RWops* rw = SDL_RWFromFile(path, "rb");
    if (!rw) return false;
    const Sint64 n = SDL_RWsize(rw);
    std::vector<unsigned char> bytes(n > 0 ? static_cast<std::size_t>(n) : 0u);
    const std::size_t got = bytes.empty() ? 0 : SDL_RWread(rw, bytes.data(), 1, bytes.size());
    SDL_RWclose(rw);
    if (got == 0) return false;
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(got), w, h, &comp, 4);
    if (!px) return false;
    rgba->assign(px, px + static_cast<std::size_t>(*w) * *h * 4);
    stbi_image_free(px);
    return true;
}

std::string path_of(const std::string& key) {
    if (key.rfind("user:", 0) == 0) {
        const std::string dir = shaders_dir();
        return dir.empty() ? std::string() : dir + "/" + key.substr(5);
    }
    return {};
}

// "name=value;..." to pairs, and back.
std::vector<std::pair<std::string, float>> parse_params(const std::string& s) {
    std::vector<std::pair<std::string, float>> out;
    std::size_t a = 0;
    while (a < s.size()) {
        std::size_t b = s.find(';', a);
        if (b == std::string::npos) b = s.size();
        const std::string item = s.substr(a, b - a);
        const auto eq = item.find('=');
        if (eq != std::string::npos && eq > 0)
            out.emplace_back(item.substr(0, eq), std::strtof(item.c_str() + eq + 1, nullptr));
        a = b + 1;
    }
    return out;
}

void apply(int screen) {
    if (screen == 1) {
        gbarecomp::shader::set_bottom_param_overrides(parse_params(bottom_shader_params_choice()));
        const std::string path = path_of(bottom_shader_choice());
        runtime_set_bottom_shader_preset(path.empty() ? nullptr : path.c_str());
        return;
    }
    gbarecomp::shader::set_param_overrides(parse_params(shader_params_choice()));
    const std::string path = path_of(shader_choice());
    runtime_set_shader_preset(path.empty() ? nullptr : path.c_str());
}

std::vector<gbarecomp::shader::Param> params_of(int screen) {
    return screen == 1 ? gbarecomp::shader::bottom_params() : gbarecomp::shader::params();
}
void set_param_of(int screen, const std::string& name, float v) {
    if (screen == 1) gbarecomp::shader::set_bottom_param(name, v);
    else gbarecomp::shader::set_param(name, v);
}
void params_set_of(int screen, const std::string& values) {
    if (screen == 1) bottom_shader_params_set(values);
    else shader_params_set(values);
}

bool hidden(const std::string& name) { return name.empty() || name[0] == '.'; }   // ".", "._x" (macOS)

bool is_preset(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".glslp";
}

// A folder with a preset somewhere inside (the collection's `shaders`
// subfolders hold only the passes' .glsl).
bool has_preset(const fs::path& dir, int depth = 0) {
    if (depth > 6) return false;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (hidden(name)) continue;
        if (e.is_regular_file(ec) && is_preset(e.path())) return true;
        if (e.is_directory(ec) && has_preset(e.path(), depth + 1)) return true;
    }
    return false;
}

std::string label_of(const std::string& key) {
    if (key.empty() || key.rfind("user:", 0) != 0) return "None";
    return fs::path(key.substr(5)).stem().string();
}

// ── Download Shader Pack ─────────────────────────────────────────────────
// libretro's own GLSL pack, the one RetroArch's "Update GLSL Shaders" takes,
// fetched on the player's ask (nothing of it ships with the game).
constexpr char kPackUrl[] = "https://buildbot.libretro.com/assets/frontend/shaders_glsl.zip";
constexpr char kPackFolder[] = "libretro";
std::atomic<int> g_download{0};   // 0 idle, 1 downloading, 2 unpacking

uint32_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// A zip's files under `dest` (stored or deflated; no zip64, no encryption:
// the pack is neither). Names that would leave `dest` are refused.
bool unzip(const std::string& zip, const fs::path& dest, int* presets, std::string* err) {
    const auto* d = reinterpret_cast<const uint8_t*>(zip.data());
    const std::size_t n = zip.size();
    if (n < 22) { *err = "the download is not a zip"; return false; }
    std::size_t eocd = std::string::npos;
    for (std::size_t i = n - 22 + 1; i-- > 0 && n - i <= 65557;)
        if (u32(d + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd == std::string::npos) { *err = "the download is not a zip"; return false; }
    const uint32_t count = u16(d + eocd + 10);
    std::size_t p = u32(d + eocd + 16);
    for (uint32_t e = 0; e < count; ++e) {
        if (p + 46 > n || u32(d + p) != 0x02014b50u) { *err = "the zip's directory is damaged"; return false; }
        const uint32_t method = u16(d + p + 10), csize = u32(d + p + 20), usize = u32(d + p + 24);
        const uint32_t nlen = u16(d + p + 28), xlen = u16(d + p + 30), clen = u16(d + p + 32);
        const std::size_t local = u32(d + p + 42);
        if (p + 46 + nlen > n) { *err = "the zip's directory is damaged"; return false; }
        const std::string name(reinterpret_cast<const char*>(d + p + 46), nlen);
        p += 46 + nlen + xlen + clen;
        // Safe names only: relative, no "..", no hidden parts (macOS "._").
        bool safe = !name.empty() && name[0] != '/' && name.find('\\') == std::string::npos &&
                    name.find(':') == std::string::npos;
        for (std::size_t a = 0; safe && a < name.size();) {
            const std::size_t b = name.find('/', a);
            const std::string part = name.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (part == ".." || (!part.empty() && part[0] == '.')) safe = false;
            a = b == std::string::npos ? name.size() : b + 1;
        }
        if (!safe) continue;
        std::error_code ec;
        if (name.back() == '/') {
            fs::create_directories(dest / name, ec);
            continue;
        }
        if (local + 30 > n || u32(d + local) != 0x04034b50u) { *err = "the zip is damaged"; return false; }
        const std::size_t data = local + 30 + u16(d + local + 26) + u16(d + local + 28);
        if (data + csize > n) { *err = "the zip is cut short"; return false; }
        const fs::path out = dest / name;
        fs::create_directories(out.parent_path(), ec);
        std::FILE* f = std::fopen(out.string().c_str(), "wb");
        if (!f) { *err = "cannot write " + out.string(); return false; }
        bool ok = true;
        if (method == 0) {
            ok = std::fwrite(d + data, 1, csize, f) == csize;
        } else if (method == 8) {
            int len = 0;
            char* raw = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(d + data),
                                                          static_cast<int>(csize), &len);
            ok = raw && static_cast<uint32_t>(len) == usize &&
                 std::fwrite(raw, 1, static_cast<std::size_t>(len), f) == static_cast<std::size_t>(len);
            std::free(raw);
        }
        std::fclose(f);
        if (!ok) { *err = "cannot unpack " + name; return false; }
        if (is_preset(out)) ++*presets;
    }
    return true;
}

void download_pack() {
    std::string err;
    int presets = 0;
    const std::string root = shaders_dir();
    int status = 0;
    std::string body;
    if (root.empty()) {
        err = "There is no shaders folder on this build.";
    } else if (!http_get(kPackUrl, &status, &body)) {
        err = "The shader pack could not be downloaded: no connection to buildbot.libretro.com.";
    } else if (status != 200) {
        err = "The shader pack could not be downloaded (the server answered " + std::to_string(status) + ").";
    } else {
        g_download.store(2);
        std::error_code ec;
        const fs::path dest = fs::path(root) / kPackFolder;
        const fs::path fresh = fs::path(root) / (std::string(kPackFolder) + ".new");
        fs::remove_all(fresh, ec);
        fs::create_directories(fresh, ec);
        if (unzip(body, fresh, &presets, &err)) {
            // Into place only whole: a failed download keeps the last pack.
            fs::remove_all(dest, ec);
            fs::rename(fresh, dest, ec);
            if (ec) err = "The shader pack could not be put in place: " + ec.message();
        } else {
            fs::remove_all(fresh, ec);
            err = "The shader pack could not be unpacked: " + err + ".";
        }
    }
    std::fprintf(stderr, "shaders: download %s (%d presets)\n", err.empty() ? "done" : err.c_str(), presets);
    g_download.store(0);
    menu_shaders_imported(err.empty() ? presets : 0, kPackFolder, err.empty() ? nullptr : err.c_str());
}

}  // namespace

bool shaders_download_start() {
    int idle = 0;
    if (shaders_dir().empty() || !g_download.compare_exchange_strong(idle, 1)) return false;
    std::thread(download_pack).detach();
    return true;
}

int shaders_download_state() { return g_download.load(); }

bool shaders_pack_installed() {
    const std::string root = shaders_dir();
    std::error_code ec;
    return !root.empty() && fs::is_directory(fs::path(root) / kPackFolder, ec);
}

std::string shaders_dir() {
    const std::string d = settings_dir();
    return d.empty() ? std::string() : d + "/shaders";
}

void shaders_install() {
    gbarecomp::g_shader_image_loader = &load_image;
    apply(0);
    apply(1);
}

std::vector<ShaderEntry> shaders_list(const std::string& folder) {
    std::vector<ShaderEntry> out;
    if (folder.empty()) {
        ShaderEntry none;
        none.label = "Reset";
        none.none = true;
        none.help = "No shader: the picture\nas it is.";
        out.push_back(none);
    } else {
        ShaderEntry up;
        up.label = "..";
        up.is_folder = true;
        const auto slash = folder.find_last_of('/');
        up.folder = slash == std::string::npos ? std::string() : folder.substr(0, slash);
        up.help = "Up a folder.";
        out.push_back(up);
    }
    const std::string root = shaders_dir();
    if (root.empty()) return out;
    const fs::path dir = folder.empty() ? fs::path(root) : fs::path(root) / folder;
    std::vector<ShaderEntry> folders, presets;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (hidden(name)) continue;
        const std::string rel = folder.empty() ? name : folder + "/" + name;
        if (e.is_directory(ec)) {
            if (!has_preset(e.path())) continue;
            ShaderEntry f;
            f.label = name;
            f.is_folder = true;
            f.folder = rel;
            f.help = "A folder of presets.";
            folders.push_back(f);
        } else if (e.is_regular_file(ec) && is_preset(e.path())) {
            ShaderEntry p;
            p.label = e.path().stem().string();
            p.key = "user:" + rel;
            p.help = folder.empty() ? "Your shaders folder." : folder + "/";
            presets.push_back(p);
        }
    }
    auto lower = [](std::string t) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
        return t;
    };
    auto by_label = [&](const ShaderEntry& a, const ShaderEntry& b) { return lower(a.label) < lower(b.label); };
    std::sort(folders.begin(), folders.end(), by_label);
    std::sort(presets.begin(), presets.end(), by_label);
    out.insert(out.end(), folders.begin(), folders.end());
    out.insert(out.end(), presets.begin(), presets.end());
    return out;
}

const std::string& shaders_choice(int screen) {
    return screen == 1 ? bottom_shader_choice() : shader_choice();
}

void shaders_choose(const std::string& key, int screen) {
    if (key != shaders_choice(screen)) params_set_of(screen, "");   // another preset: its own values
    if (screen == 1) bottom_shader_choice_set(key);
    else shader_choice_set(key);
    apply(screen);
}

std::vector<ShaderParam> shaders_params(int screen) {
    std::vector<ShaderParam> out;
    for (const auto& p : params_of(screen))
        out.push_back({p.name, p.desc, p.value, p.def, p.min, p.max, p.step});
    return out;
}

namespace {
// The values that differ from the preset's defaults, kept with the choice.
void keep_params(int screen) {
    std::string s;
    char buf[64];
    for (const auto& p : params_of(screen)) {
        if (std::abs(p.value - p.def) < 1e-6f) continue;
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(p.value));
        if (!s.empty()) s += ';';
        s += p.name + "=" + buf;
    }
    params_set_of(screen, s);
}
}  // namespace

void shaders_param_step(const std::string& name, int dir, int screen) {
    for (const auto& p : params_of(screen)) {
        if (p.name != name) continue;
        float v = dir == 0 ? p.def : p.value + static_cast<float>(dir) * p.step;
        // Back onto the step grid from the minimum (float steps drift).
        if (dir != 0 && p.step > 0.0f) v = p.min + std::round((v - p.min) / p.step) * p.step;
        set_param_of(screen, name, v);
    }
    keep_params(screen);
}

void shaders_params_reset(int screen) {
    for (const auto& p : params_of(screen)) set_param_of(screen, p.name, p.def);
    params_set_of(screen, "");
}

std::string shaders_label(int screen) { return label_of(shaders_choice(screen)); }
int shaders_status(int screen) {
    return screen == 1 ? runtime_bottom_shader_status() : runtime_shader_status();
}
std::string shaders_error(int screen) {
    return screen == 1 ? runtime_bottom_shader_error() : runtime_shader_error();
}

}  // namespace kol
