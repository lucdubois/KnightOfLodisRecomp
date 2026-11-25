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
//    drives it (kol_menu's key filter), and the game is held while it is
//    open (kol_menu's menu_pause_query, as on Android).
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
// The menu asked for a restart (restart_game): main starts the program again
// with its first arguments once the game has quit.
bool desktop_restart_requested();
// That restart is onto the setup screen (Options > Mod where no system
// dialog can be used): main adds --setup.
bool desktop_restart_on_setup();
// After desktop_prepare said no (*ok false): the other game's program to
// start in this one's place, or "" (the setup screen was closed). A package
// holds a program for each game (kol_region.h kCarts), and the player's
// choice, or the ROM in place, may be the other one's.
std::string desktop_handover();

// The right stick's click, from kol_actions' pad hook (down 1, up 0), with
// whether the left stick is held. A short click alone opens or closes the
// options menu, on every platform (on Android the second screen, or the
// overlay on a device with one, shows it); with L3 held it is not a click
// (L3 is the state combos').
void desktop_r3(int down, bool l3_held);

// Options > Display (a desktop's; 2026-11-20, players on Windows: Alt+Enter
// did nothing with a controller connected). The window or full screen, and
// the window's size, which is the window's own and not the game's view size
// (Graphics > View Size): the picture is fitted into it. Saved in player
// mode to display.cfg beside enhancements.cfg (mode=, window=WxH), read at
// start before the window opens; Alt+Enter's toggle is saved too.
bool display_full_screen();                 // the window's mode now
void display_full_screen_set(bool full);
unsigned window_size_count();               // the sizes that fit this display
void window_size_at(unsigned i, int* w, int* h);
int window_size_current();                  // the chosen size's index, -1 none
void window_size_now(int* w, int* h);       // the window's size now
void window_size_step(int dir);             // the next size up (+1) or down (-1)
int window_size_multiple(int w, int h);     // the view's exact multiple w x h is (3: 3x), else 0
// (Options > Display > Scaling is kol_settings.h's scaling_choice_*.)
void scaling_choice_set(unsigned i);
// Options > Display > Ratio changed (kol_settings ratio_choice_set): the
// window sizes offered are the new shape's, and the window takes that shape
// (at once in Window mode; for going back to a window in full screen).
// Nothing on Android.
void display_ratio_changed();

}  // namespace kol
