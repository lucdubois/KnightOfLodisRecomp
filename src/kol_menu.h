#pragma once

// The options menu on the second screen, drawn like the game's own OPTION
// window (R on an empty tile > System > Option) and built from the cartridge:
// the window frame, the OPTION and HELP tabs, the hand cursor, the scroll
// arrows, the font, and the game's own option names, values and HELP texts.
//
// It holds the game's twelve options, which it reads from and writes to the
// game's settings (EWRAM 0x0200132C, eight bytes, the block the game's OPTION
// menu copies in and out). Writes are queued here and applied inside the game
// by a reviewed function hook on SoundMain (game.toml), never from outside
// guest execution and never during a speculative excursion. After them, the
// recomp's own enhancement settings, and a Debug page.
//
// Touch drives it: drag to scroll, tap a name to select it (its HELP text
// shows), tap a value to change it. The second screen asks for a fresh image
// after every touch. See README "The options menu on the second screen".

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kol_sheet.h"

namespace gbarecomp { struct PresentOverlay; }

namespace kol {

// The menu's canvas is the status sheet's: kSheetW x kSheetH (kol_sheet.h),
// so the second screen shows it in the same place at the same scale.

bool menu_is_open();
// gbarecomp's g_runtime_pause_query: the game held while the menu is open,
// let run a few frames after each change of a setting (kol_menu.cpp).
int menu_pause_query();
// Every frame, from the frame hook: counts those frames down.
void menu_frame_tick();
// The state saved as the menu holds the game, loaded at each change (from
// kol_states' path hook): its file for that slot, else null.
const char* menu_hold_state_path(int slot);
// After a save: true if it was that state (now ready to load).
bool menu_hold_saved(const char* path);
// Another state was loaded: that one is taken again.
void menu_hold_forget();
// While the menu is open: the OPTION window's border round the game's
// picture (a gbarecomp present overlay; false while it is closed).
bool menu_focus_frame(gbarecomp::PresentOverlay* out);
// The same as an overlay provider, where the host has none of its own
// (Android); the desktop's puts the frame under its menu.
int menu_focus_frame_provider(gbarecomp::PresentOverlay* out, int max, int out_w, int out_h);
// Opens or closes the menu (the corner button). Returns whether it is open.
bool menu_toggle();
// The same from the pad (the right stick's click): also tells the second
// screen, which did not ask for it, that the menu changed (menu_take_dirty).
bool menu_pad_toggle();
// Opens the menu on the Save States page, or closes it when that page is
// already up (the desktop check's `states`; the pad has no shortcut for it,
// the page is a row of the menu). The second screen gets the image on its
// next tick (menu_take_dirty).
void menu_open_states();

// A touch at canvas pixel (x, y). action: 0 down, 1 move, 2 up, 3 cancel.
void menu_touch(int action, float x, float y);

// The pad while the menu is open (gbarecomp's g_host_keyinput_filter): up and
// down move the hand, left and right step the value, A changes it or opens a
// page, B leaves a page (Sound, Debug) or closes the menu. The game gets nothing
// pressed meanwhile, nor any button still held from then until it is released.
// Returns the KEYINPUT the game sees.
std::uint16_t menu_filter_keys(std::uint16_t keyinput);
// True once after the pad changed the menu (the second screen then asks for a
// new image; after a touch it has one already).
bool menu_take_dirty();

// The Save States page's grid, scrolled by the second screen (README "Numbered
// save states"): menu_render draws the frame and title, these the rest.
// info: rows (0 none), version of what the rows show, count of the asks to move
// the grid, where the last ask wants it (canvas px), the grid's top and bottom
// lines, a row's height. False when the page is not up.
bool menu_states_info(int* info);
// Row `row`, kSheetW x the row's height, unset pixels clear. False past the end.
bool menu_states_row(int row, std::uint32_t* argb);
// Where the grid is now (a tap reads it; the pad moves from it).
void menu_states_scroll(float scroll);

// Renders the menu into `argb` (kSheetW x kSheetH). False when the ROM art is
// not available yet.
bool menu_render(std::uint32_t* argb);

// The corner button: the game's Option icon (R bar) on a parchment tile,
// kMenuButtonSize square.
constexpr int kMenuButtonSize = 24;
bool menu_button_render(std::uint32_t* argb);

// The zone under the second screen's page (sheet, list or menu): kSheetW
// wide and `h` canvas pixels tall (at least 24), in the frame of the game's
// windows (the SUPPLY menu's: tiles 0x11-0x18 around the weave), with the
// corner button inside it at the right, vertically centred: its top-left at
// (menu_zone_button_x(), menu_zone_button_y(h)). False before the ROM art.
// `pressed`: the button a finger is on (kZoneOption ...), pushed in (its
// window darker, the icon a pixel down and right); -1 none. `world`, the
// world map's buttons: Save and Load left of Option, and Item, Spell, Class
// (Organize's R menu), Training and Shop at the left, on (kZoneWorldOn),
// greyed while one of
// the menu's own screens is up (kZoneWorldGreyed), or none; on, with
// kZoneNoTraining / kZoneNoShop those two greyed (not in the menu here).
constexpr int kZoneOption = 0, kZoneSave = 1, kZoneLoad = 2;
constexpr int kZoneItem = 3, kZoneSpell = 4, kZoneClass = 5, kZoneTraining = 6, kZoneShop = 7;
constexpr int kZoneButtons = 8;
constexpr int kZoneWorldNone = 0, kZoneWorldOn = 1, kZoneWorldGreyed = 2;
constexpr int kZoneNoTraining = 4, kZoneNoShop = 8;   // flags with kZoneWorldOn
constexpr int kZoneOrganizeOn = 16;   // with kZoneWorldGreyed: Item, Spell, Class live (in Organize)
bool menu_zone_render(std::uint32_t* argb, int h, int pressed = -1, int world = kZoneWorldNone);

// The game's confirm sound (0xAD), for a second-screen button that starts
// something in the game (Save, Load).
void menu_confirm_sound();

// The bar's button's help, as a long press shows it: a HELP card with the
// game's name for it and the game's help text, at the top of a kSheetW x
// kSheetH canvas (transparent below). Returns its height, 0 for none.
int menu_zone_help_render(int button, std::uint32_t* argb);

// The game's pointing hand (the menus' cursor), kHandW x kHandH, for the
// second screen to draw by the button while the menu is open.
constexpr int kHandW = 16, kHandH = 8;
bool menu_hand_render(std::uint32_t* argb);
// A window in the bottom bar's buttons' frame, cols x rows of tiles: style 0 as
// they are, 1 pressed in, 2 greyed.
bool menu_window_render(std::uint32_t* argb, int cols, int rows, int style = 0);
// The game's cancel sound (0xB0).
void menu_cancel_sound();
// Text in the game's character codes. Returns the count.
int menu_encode(const char* text, std::uint8_t* out, int max);
// The menu's scroll arrow, 8 x 8.
bool menu_arrow_render(std::uint32_t* argb, bool down);
// Button b's left edge: Option at the right, Save and Load to its left;
// Item, Spell, Class, Training and Shop from the left edge; kZoneGap apart.
constexpr int kZoneGap = 4;
constexpr int menu_zone_button_x(int b = kZoneOption) {
    return b >= kZoneItem ? 8 + (b - kZoneItem) * (kMenuButtonSize + kZoneGap)
         : kSheetW - 8 - kMenuButtonSize - (b == kZoneOption ? 0 : (3 - b) * (kMenuButtonSize + kZoneGap));
}
// Whether button b takes a touch with the bar's `world` (kZoneWorld... and flags).
constexpr bool menu_zone_button_live(int b, int world) {
    return b == kZoneOption ||
           ((world & 3) == kZoneWorldOn && !(b == kZoneTraining && (world & kZoneNoTraining)) &&
            !(b == kZoneShop && (world & kZoneNoShop))) ||
           ((world & kZoneOrganizeOn) && b >= kZoneItem && b <= kZoneClass);
}
constexpr int menu_zone_button_y(int h) { return (h - kMenuButtonSize) / 2; }

// Registers the SoundMain hook that applies queued writes to the game's
// options. Called from the frame hook, like the other hooks.
void menu_install();

// Desktop check: KOL_MENU_PPM (see kol_menu.cpp). Called every frame.
void menu_debug_frame();

// A dialog in the game's look over the menu (opening it if closed): a
// title, the text, and buttons in the bottom bar's frame, pushed in while
// touched; the pad's left/right move the hand, A presses, B presses
// `cancel_button` (or closes with -1). `done` gets the button's index, on the
// thread that pressed it, after the dialog has closed.
void menu_dialog(const char* title, const char* text, const std::vector<std::string>& buttons,
                 int cancel_button, std::function<void(int)> done);

// What the platform does for Options > Mod and Debug > Unlink ROM (Android:
// Mods.java, SetupActivity, RestartActivity). Null members: not on this
// platform (the desktop takes --rom-patch).
struct MenuPlatform {
    void (*pick_mod)() = nullptr;     // the system's file picker; then menu_mod_picked
    void (*pick_bios)() = nullptr;    // the same for a BIOS file; then menu_bios_picked
    void (*pick_shader_folder)() = nullptr;   // Android: a folder of shaders copied in; then menu_shaders_imported
    void (*remove_mod)() = nullptr;   // files/mod/ emptied
    void (*restart)() = nullptr;      // the app ends and starts again
    void (*unlink_rom)() = nullptr;   // the ROM copy set aside, then restart
    void (*show_licenses)() = nullptr;   // Options > Licenses: the app's notices (assets/licenses.txt)
    void (*quit)() = nullptr;         // Options > Quit Game (a desktop: no system gesture closes the game)
    // The RetroAchievements login as a form in the menu itself (a desktop:
    // Android has its own dialog, kol_cheevos.h's login form). Typed text
    // comes in through menu_login_text and menu_login_key.
    bool login_in_menu = false;
    void (*text_input)(int on) = nullptr;   // the host takes typed text (gbarecomp g_host_text_input) or not
    void (*show_keyboard)() = nullptr;      // the system's on-screen keyboard (SteamOS: Steam's), if any
    const char* keyboard_hint = nullptr;    // the form's line when nothing else is said
    // A page the platform draws over the menu (the desktop's Licenses,
    // show_licenses): while page_up() the pad is the page's (menu_filter_keys:
    // up/down scroll a step, L/R or left/right a page, B or A closes), called
    // under the menu's lock (they must not call the menu back).
    bool (*page_up)() = nullptr;
    void (*page_scroll)(int dir, bool page) = nullptr;
    void (*page_close)() = nullptr;
};
void menu_set_platform(const MenuPlatform& p);

// The login form (MenuPlatform::login_in_menu). Up: whether it is open.
// Text: typed UTF-8 (printable ASCII kept) into the field being typed in.
// Key: a keyboard key while it is up; `pad` true when a controller is
// attached (only Backspace, and Enter on a field, are the keyboard's then:
// the Steam Deck's Desktop Mode types Enter, Escape and the arrows from its
// own buttons, which the pad already drives). True: the key was the form's.
enum class LoginKey { Backspace, Enter, Escape, Tab, BackTab, Up, Down, Left, Right };
bool menu_login_up();
void menu_login_text(const char* utf8);
bool menu_login_key(LoginKey k, bool pad);
// The picker's answer: the mod's name, or an error (name null).
void menu_mod_picked(const char* name, const char* error);
// Technical > BIOS's picker: null (the GBA BIOS kept, chosen from the next
// start), or why not.
void menu_bios_picked(const char* error);
// Display > Shader > Import Folder's answer: the presets copied in (and the
// folder's name), or an error.
void menu_shaders_imported(int presets, const char* name, const char* error);

// For the other second-screen pages (kol_edit_panel): the game's window
// frame over tile cells c0..c1 x r0..r1 of a w x h canvas, and one line of
// plain ASCII in the game's font (16 tall) with its top-left at (x, y),
// returning its width. False / 0 before the ROM art.
bool menu_draw_window(std::uint32_t* argb, int w, int h, int c0, int r0, int c1, int r1);
int menu_draw_text(std::uint32_t* argb, int w, int h, const char* s, int x, int y);

// The achievement popup (kol_cheevos.h): the game's window, at least
// kCheevoPopupCols x kCheevoPopupRows tiles, the badge (32 x 32 ARGB, may be
// null) at its left, "Achievement Unlocked!" and the points, then the title.
// The window widens to fit the text, up to kCheevoPopupMaxCols, then the line
// wraps and it grows down. Returns ARGB *w x *h (outside the window's tiles
// clear); empty before the ROM's art is up.
constexpr int kCheevoPopupCols = 28, kCheevoPopupRows = 6, kCheevoPopupMaxCols = 40;
std::vector<std::uint32_t> menu_cheevo_popup(const char* title, unsigned points,
                                             const std::uint32_t* badge32, int* w, int* h);
// The same window with any heading (corner text at its right, may be "") and
// the line under it: the mod notice, the RetroAchievements welcome.
std::vector<std::uint32_t> menu_notice_popup(const char* heading, const char* corner, const char* line,
                                             const std::uint32_t* badge32, int* w, int* h);
int menu_text_width(const char* s);

}  // namespace kol
