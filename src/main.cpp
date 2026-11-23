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

#include <string>
#include <vector>

#include "kol_desktop.h"
#include "kol_vs_cpu.h"
#include "kol_vs_net.h"
#include "kol_widescreen.h"

int main(int argc, char** argv) {
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
    return gbarecomp::run_game(argc, argv, opts);
}
