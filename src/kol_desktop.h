#pragma once

// The desktop's player front end: what the Android app does around the game
// (android/app/jni/main_android.cpp and its Java), for a computer with one
// screen and a controller. Written for the Steam Deck; the same code runs on
// any desktop build. See docs/DEVLOG.md "Steam Deck".
//
// Two parts:
//
//  - The options menu over the game. Android shows kol_menu's canvas on the
//    second screen (or over the game from Java); here it is a present-time
//    overlay (gbarecomp g_present_overlay_provider), centred, at a whole
//    scale. F10 or a click of the right stick opens and closes it; the pad
//    drives it (kol_menu's key filter), and the picture behind dims
//    (g_present_dim, as on Android).
//
//  - Player mode: the game's files in one directory, as on Android (the ROM
//    as totkol.gba, the optional BIOS as gba_bios.bin, totkol.sav,
//    enhancements.cfg, states/), with the settings read and saved there
//    (kol::settings_init: a fresh directory starts on the Enhanced preset).
//    Asked for with --data-dir <dir> or KOL_DATA_DIR; a public build
//    (KOL_DEV_TOOLS=OFF) takes the platform's own place when neither is given
//    ($XDG_DATA_HOME/KnightOfLodisRecomp, ~/Library/Application Support/...).
//    Without it the run is the developer's, exactly as before: no settings
//    file, the switches from the environment.

#include <cstdint>
#include <string>
#include <vector>

namespace kol {

// Call first in main. Installs the menu overlay and its keys, and in player
// mode turns the command line into the runtime's (--rom, --bios or --no-bios,
// --save-path, --window, --skip-bios-intro; arguments given after are kept
// and win). Returns the arguments to run with. `ok` false: nothing to run
// (no ROM in the directory; a message was shown), and main returns 1.
std::vector<std::string> desktop_prepare(int argc, char** argv, bool* ok);

// True in player mode.
bool desktop_player_mode();

// The right stick's click, from kol_actions' pad hook (down 1, up 0), with
// whether the left stick is held. A short click alone opens or closes the
// options menu, on every platform (on Android the second screen, or the
// overlay on a device with one, shows it); with L3 held it is not a click
// (L3 is the state combos').
void desktop_r3(int down, bool l3_held);

}  // namespace kol
