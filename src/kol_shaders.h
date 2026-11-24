// kol_shaders.h — Options > Display > Shader: RetroArch GLSL presets
// (gbarecomp shader_preset.h) over the picture.
//
// None ship with the game: the player's go in a `shaders` folder beside the
// game's files (the save's folder; Android imports a folder into it; Download
// Shader Pack fetches libretro's). The choice is kept in enhancements.cfg
// (kol_settings shader_choice) as "user:<path>" and applied again at start.
#pragma once

#include <string>
#include <vector>

namespace kol {

// Once, after the settings are read: gbarecomp's image decoder for presets'
// lookup textures, and the kept choice applied.
void shaders_install();

// The player's shaders folder ("" without a settings folder: a developer's
// run, built-in shaders only).
std::string shaders_dir();

// One line of the Shader page: a folder to enter (or "..", up), or a preset
// with its key for shaders_choose.
struct ShaderEntry {
    std::string label;
    std::string key;      // a preset's "user:...", "" for Reset (no shader)
    std::string folder;   // a folder's path under shaders_dir() ("" the top)
    std::string help;
    bool is_folder = false;
    bool none = false;
};
// The page's lines in `folder` ("" the top: Reset, then the player's folders
// and presets). Folders without a preset anywhere
// inside are left out (the collection's own `shaders` subfolders).
std::vector<ShaderEntry> shaders_list(const std::string& folder);

// Display > Shader > Download Shader Pack: libretro's GLSL pack (27 MB, from
// buildbot.libretro.com, as RetroArch's "Update GLSL Shaders") into
// shaders/libretro, on a thread; menu_shaders_imported reports. False: one is
// running, or there is no shaders folder.
bool shaders_download_start();
int shaders_download_state();   // 0 idle, 1 downloading, 2 unpacking
bool shaders_pack_installed();

// The running preset's parameters (empty while none runs or loading), and a
// step of one (dir +1 / -1) or its default (dir 0); kept with the choice.
struct ShaderParam {
    std::string name, desc;
    float value = 0.0f, def = 0.0f, min = 0.0f, max = 0.0f, step = 0.0f;
};
// `screen` everywhere below: 0 the main picture's preset, 1 a second
// screen's own (Options > Display > Bottom Shader; Android's lower panel,
// drawn by its GL surface: gbarecomp shader::run_gl).
std::vector<ShaderParam> shaders_params(int screen = 0);
void shaders_param_step(const std::string& name, int dir, int screen = 0);
void shaders_params_reset(int screen = 0);

// Choose a preset (its key; "" none): kept and applied at once.
void shaders_choose(const std::string& key, int screen = 0);
// The chosen preset's key ("" none) and name for the menu ("None" when none).
const std::string& shaders_choice(int screen = 0);
std::string shaders_label(int screen = 0);
// 0 none, 1 running, 2 loading, -1 failed (shaders_error says why).
int shaders_status(int screen = 0);
std::string shaders_error(int screen = 0);

}  // namespace kol
