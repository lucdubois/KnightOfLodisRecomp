#pragma once

// The desktop's setup screen: the Android app's SetupActivity for a computer
// (written for the Steam Deck). The game's title, three boxes side by side,
// ROM / BIOS / Mod, each with its state (a red "!", a grey "?", a green
// check) and a line under its name, the note on the BIOS, then Remove mod
// and Play. A box opens a file browser of our own (no system dialog: none
// can be counted on in the Deck's Gaming Mode or inside a Flatpak), driven
// by the pad, a touch, the mouse or the keyboard.
//
// The files are checked by their contents (SHA-1), under any name, and
// copied into the player's directory under the names the game reads
// (kol_desktop.h): totkol.gba, gba_bios.bin, mod/<the patch's name>. A wrong
// file is not copied, and the box says so.
//
// Shown by kol::desktop_prepare in player mode when the directory holds no
// ROM (or not this game's), or with --setup. See docs/DEVLOG.md "Steam Deck".

#include <cstdint>
#include <string>

namespace kol {

// The ROM's and the GBA BIOS's SHA-1 (as android SetupActivity's).
inline constexpr char kRomSha1[] = "85a46320a31f16f5c6dda27fc094edd078dbe43d";
inline constexpr char kBiosSha1[] = "300c20df6731a33952ded8c436f7f186d25d3492";

// Whether `path` is a file with that SHA-1.
bool setup_file_is(const std::string& path, const char* sha1_hex);

// The mod in `dir`/mod (the one file there), or "".
std::string setup_mod_path(const std::string& dir);

// A mod file taken in as `dir`/mod's one file (the one there before goes),
// once it is checked on the ROM in `dir` (when the ROM is there): a patch
// that does not apply, or changes the game's code, is refused. False: *why,
// a sentence for the player. The setup screen's Mod box and Options > Mod.
bool setup_mod_take(const std::string& dir, const std::string& file, std::string* why);
// `dir`/mod emptied: the original game from the next start.
void setup_mod_remove(const std::string& dir);

// The system's Open dialog from the running game (Options > Mod; `what` 0
// the ROM, 1 the BIOS, 2 a mod, as the setup screen's boxes). 0: none to use
// here (Linux in the Deck's Gaming Mode or Big Picture, or with neither
// kdialog nor zenity). 1: answered (a Mac's panel and Windows' dialog are
// modal): *file, or "" if cancelled. 2: up on a thread (Linux):
// setup_file_dialog_poll answers, true once, with *file or "".
int setup_file_dialog(int what, std::string* file);
bool setup_file_dialog_poll(std::string* file);

// Runs the screen over `dir`, in a window of its own (closable; a file
// dropped on it is taken as the ROM, the BIOS or a mod by what it is). True:
// Play was chosen with the right ROM in place. False: the window was closed
// (the game does not start). `fullscreen`: unused, the screen is a window.
bool setup_run(const std::string& dir, bool fullscreen);

// A page of plain text in this screen's typeface, for the game's window (the
// desktop's Options > Licenses): a title bar (with "a-b of n" when the text
// runs on), the text from its line `first` (the file's lines kept, a longer
// one wrapped at spaces), and `hint` at the foot, on w x h ARGB, sized to the
// window (as the screen: its unit is h / 800). *lines: the text's lines once
// wrapped; *page: how many show at once.
void setup_text_page(const std::string& title, const std::string& text, const std::string& hint, int w, int h,
                     int first, std::uint32_t* argb, int* lines, int* page);

}  // namespace kol
