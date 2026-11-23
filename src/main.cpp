// main.cpp — Knight of Lodis Recomp entry point.
//
// This file is intentionally thin: the shared GBARecomp runtime
// (gbarecomp/src/runtime/runtime.cpp) owns ROM/BIOS loading, identity
// verification, CPU/bus/PPU/audio setup, the host window, and the main
// loop. A per-game binary only needs to (a) link the recompiled cart
// dispatch table generated into ../generated, and (b) tell run_game()
// this cartridge's built-in identity so a standalone release doesn't
// require a sidecar game.toml.
//
// See ../CLAUDE.md and ../README.md for this project's status. As of
// this file's authorship, coverage has NOT been validated against the
// mGBA oracle and dispatch misses are expected — check the coverage
// report + recomp_master_misses.toml.frag printed/written at exit.
//
// The extended-view (16:9) scene policy lives in kol_widescreen.cpp, shared
// with the Android entry point.

#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "kol_crash.h"
#include "kol_desktop.h"
#include "kol_vs_cpu.h"
#include "kol_vs_net.h"
#include "kol_widescreen.h"

namespace {

// The program again, with its first arguments (the menu's restart: Options >
// Technical > BIOS, a mod), once the game has quit and written its save.
void start_again(std::vector<std::string> first) {
    std::string self;
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) self = buf;
#elif defined(__APPLE__)
    char buf[4096] = {};
    std::uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) self = buf;
#else
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) self = std::string(buf, static_cast<std::size_t>(n));
#endif
    if (self.empty() && !first.empty()) self = first[0];
    std::vector<char*> av;
    for (auto& a : first) av.push_back(a.data());
    av.push_back(nullptr);
    std::fprintf(stderr, "desktop: starting again (%s)\n", self.c_str());
    std::fflush(stderr);
#if defined(_WIN32)
    _execv(self.c_str(), av.data());
#else
    execv(self.c_str(), av.data());
#endif
    std::fprintf(stderr, "desktop: could not start again\n");
}

}  // namespace

int main(int argc, char** argv) {
    // A crash's report in the session's log (kol_crash.h; stderr until the
    // log is opened, and for VS's copies, whose stderr is their own log).
    kol::crash_handlers_install();
    const std::vector<std::string> first(argv, argv + argc);   // for a restart (start_again)
    // The desktop's player front end (kol_desktop.h): the options menu over
    // the game, and in player mode (--data-dir, or a public build) the game's
    // files and settings in one directory, as the Android app keeps them.
    // VS Mode's unseen copies (kol_vs_cpu.h: the computer's game; kol_vs_net.h:
    // the other player's) run on the command line their parent gave them: no
    // player mode, no setup, no window of their own. The other player's game
    // takes their settings (from its session folder).
    bool ok = true;
    const bool copy = kol::vs_cpu_child() || kol::vs_mirror_child();
    if (kol::vs_mirror_child()) kol::vsnet::mirror_settings();
    std::vector<std::string> args = copy ? std::vector<std::string>(argv, argv + argc)
                                         : kol::desktop_prepare(argc, argv, &ok);
    if (!ok) return 1;
    if (!copy) kol::vs_cpu_args(args);
    std::vector<char*> args_v;
    for (auto& a : args) args_v.push_back(a.data());
    args_v.push_back(nullptr);
    argc = static_cast<int>(args.size());
    argv = args_v.data();

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Tactics Ogre: The Knight of Lodis";
    opts.builtin_rom_sha1 = "85a46320a31f16f5c6dda27fc094edd078dbe43d";
    opts.show_fps_by_default = !kol::desktop_player_mode();
    // Rewind on L2 (gbarecomp's default binding): 20 s kept, a state every
    // 15 frames; each press goes back a second, held it repeats. Off while
    // Debug > Tile Info or L2 Moves Sun has L2 (kol_widescreen).
    opts.rewind_history_seconds = 20;
    kol::configure_extended_view(opts);
    const int result = gbarecomp::run_game(argc, argv, opts);
    if (!copy && kol::desktop_restart_requested()) start_again(first);
    return result;
}
