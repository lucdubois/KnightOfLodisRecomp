#pragma once

#include <string>

// Player-facing enhancement toggles.
//
// These are the settings a PLAYER changes from the in-game menu, as opposed to
// the KOL_* environment variables, which stay what they have always been:
// developer switches for A/B work, unreachable on a handheld. Each one here is
// live — set it and the next frame renders with it — and persisted beside the
// save, so a choice survives a launch.
//
// The extended view is ONE control covering everything in it: at 240x160 the
// widened path is not consulted at all, so the margins, HUD anchoring, panel
// pinning, bar continuation and side-panel alignment all go inert by
// construction. It is a size CHOICE rather than a checkbox — see below. The
// other two settings are independent of the view and so are their own
// switches.
namespace kol {

// Widescreen is NOT a toggle. "Off" is just the 240x160 entry at the top of
// the width list, so there is one control for the view instead of a checkbox
// and a list that can disagree with each other.
enum class Setting {
    Highlight = 0,    // see-through blue movement-range tiles
    SecondScreen,     // the unit panel on the Thor's second display
    WeatherRepeat,    // the rain repeated into the widened view's margins
    PadRemap,         // retired 2026-11-17 (Shoulder Units): kept so the others keep their places; always off
    DepthOfField,     // battle map blurred by depth around the cursor
    Perspective,      // battle map smaller with distance, verticals kept
    EffectsPlaced,    // sound effects panned to where they happen (kol_sound)
    SoftTones,        // the square/wave/noise channels gently low-passed
    DebugMesh,        // the perspective's geometry painted (KOL_PERSP_KIND)
    DebugAutoWin,     // every battle won at its first turn (kol_auto_win.cpp)
    DebugNoEncounters,  // no random battles on the world map (kol_actions.cpp)
    DebugHideBG0,     // layers left out of the picture (gbarecomp
    DebugHideBG1,     // g_ppu_debug_hide_layers); not saved, so a launch
    DebugHideBG2,     // always shows every layer
    DebugHideBG3,
    DebugHideOBJ,
    ShopAnywhere,     // the second screen's Shop button away from towns too
    Torchlight,       // the ruins' torches light the battlefield (kol_focus)
    UnitShadows,      // units cast shadows and sit in occlusion (kol_focus)
    DebugSunStick,    // L2 + right stick arcs the sun over the field (kol_perspective)
    LiftProps,        // BG1 props stand off the ground, what they hid shown (kol_focus)
    LiftWalls,        // BG1 terrain over BG0 the same, as its own surface (kol_focus)
    DebugTileInfo,    // the target cell tinted, its heights and record shown (kol_focus)
    DebugNoMenuDim,   // unused since 2026-11-23 (the picture is never dimmed under the menu); kept for the order
    FastLoading,      // the game's decompression and clears run natively (kol_fast_load)
    Water,            // the water ripples, foams at its shore, reflects (kol_perspective)
    AnalogCursor,     // the left stick moves the hand cursor freely (kol_stick_cursor)
    PlantSway,        // lifted trees and bushes sway with the wind (kol_perspective)
    BumpMap,          // a dark map's wet floor: bumps and the slime's glints in the lamps' light (kol_perspective)
    SpecialEffects,   // the game's fire and smoke drawn as effects of our own (kol_focus, kol_perspective)
    FastActions,      // the battle's long per-cell routines take no guest time (kol_fast_actions)
    FastWalk,         // units walk twice as fast, every team (kol_fast_actions)
    QuickEnemy,       // the enemy AI decides in a few frames, not ~100 (kol_fast_actions)
    FastAttack,       // an attack's strike plays at twice the speed, its sound as it was (kol_fast_attack)
    OwnRain,          // the game's raindrops replaced by our own, in the world, with splashes (kol_rain)
    WetRain,          // wet in the rain: puddles that grow, a sheen, walls that drip (kol_perspective)
    SmoothBoxes,      // dialog boxes open and close a size every frame, not in the game's two steps (kol_widescreen)
    DebugNumberedStates,  // L3+R saves a new numbered state, L3+L loads the latest (kol_states)
    FastSpells,       // a spell's animation (an item's use too) plays at twice the speed, its sound as it was (kol_fast_actions)
    SmoothSky,        // a sky gradient blurred to 8 bits a channel and spread over the view's rows (kol_perspective)
    OwnSnow,          // the game's snowflakes replaced by our own, in the world, melting where they land (kol_snow)
    SmoothPacing,     // frames paced at 60 Hz, two refreshes each on a 120 Hz screen; the game 0.46% faster (gbarecomp pacer)
    DebugNoSecondScreen,  // as a device with one screen: BACK pauses and shows the options menu over the game (SecondScreen.java)
    SwapFace,         // A <-> B and X <-> Y on the controller (kol_actions pad_swap_set)
    Rewind,           // Cheats: L2 and the on-screen button go back in time (gbarecomp rewind); off by default
    Count
};

// `dir` is where enhancements.cfg lives (the app's files directory on Android,
// null to run without persistence). `widescreen_width` is the width to return
// to when widescreen is switched back on — whatever the host resolved, 320 on
// a 1080p handheld. `panel_w`/`panel_h` are the display's pixel size in
// landscape, used to build the width ladder below; pass 0 for neither.
void settings_init(const char* dir, unsigned widescreen_width,
                   unsigned panel_w, unsigned panel_h);

bool setting_on(Setting s);

// Where enhancements.cfg lives (empty: no persistence).
std::string settings_dir();
// The BIOS (Options > Technical > BIOS). In use now: the GBA's own (true) or
// gbarecomp's built-in replacements (false, --no-bios), told by the platform
// at start. For the next start: the marker no_bios in settings_dir() (as
// Android's setup screen writes it) asks for the built-in one; the GBA BIOS
// needs gba_bios.bin there.
void bios_in_use_set(bool real);
bool bios_in_use();
bool bios_next_real();
void bios_next_set(bool real);
bool bios_file_present();

// A mod (a ROM patch, gbarecomp --rom-patch): its file name, else null, and
// "-<first 8 of the patched ROM's SHA-1>" for files kept apart from the stock
// game's (fast-load memo, numbered states); "" for the stock game.
const char* rom_patch_name();
std::string rom_file_tag();

// Applies immediately and persists. Safe to call from another thread: the one
// action that cannot be done in place — changing the view width — goes through
// the runtime's own deferred request rather than resizing anything here.
void setting_set(Setting s, bool on);

// ── The extended view's SIZE ──────────────────────────────────────────────
// The whole extended view is this one list: 240x160 (the authentic view) plus
// the sizes of the chosen shape (the ratio below). Each entry carries its
// HEIGHT as well as its width, because a view widened at scale N wants the
// matching height at scale N — on a 1080p handheld 320x180, 384x216 and
// 480x270 each fill the panel exactly with no bars. Asking for them
// separately renders a frame at the wrong aspect in between, which is why
// this is one choice. An entry is known by both: 320 wide is 320x180 at
// 16:9, 320x200 at 16:10, 320x240 at 4:3, and 240 wide is also 1:1's 240x240.
//
// Each entry is labelled by how it scales into the panel. The choice
// persists beside the toggles (width= and height=).
unsigned width_choice_count();
const char* width_choice_label(unsigned i);
unsigned width_choice_width(unsigned i);
unsigned width_choice_height(unsigned i);
unsigned width_choice_current();
// The size the PLAYER chose, or 0 when settings were never initialised (the
// desktop runner, which takes its size from the command line and must not be
// resized behind its back). The scene policy reads these to decide what to
// ask for when a scene has margin content and what to fall back to when it
// does not.
unsigned view_choice_width();
unsigned view_choice_height();
void width_choice_set(unsigned i);

// ── The picture's SHAPE (Options > Display > Ratio) ──────────────────────
// 1:1, 4:3, 3:2, 16:10 or 16:9: the list above holds the authentic 240x160
// and that shape's sizes. Unless the player picks one (saved as ratio=), the
// shape nearest the screen's own (the top screen on a two-screen device).
// Setting one rebuilds the list and keeps the view's size as near as it has
// (16:9's 384x216 becomes 4:3's 288x216; Original stays Original).
unsigned ratio_choice_count();
const char* ratio_choice_label(unsigned i);                     // "16:9"
void ratio_choice_shape(unsigned i, unsigned* num, unsigned* den);   // 16, 9
unsigned ratio_choice_current();
void ratio_choice_set(unsigned i);

// Options > Display > Scaling: how the picture fills the window (desktop) or
// screen (Android). 0 Integer (the largest whole multiple: even pixels, a
// border), 1 Smooth (filled: even pixels, soft edges), 2 Stretch (filled,
// nearest pixels: uneven at a fractional scale). scaling= in
// enhancements.cfg; gbarecomp g_host_scaling_mode. The help is the value's.
unsigned scaling_choice_count();
const char* scaling_choice_label(unsigned i);
const char* scaling_choice_help(unsigned i);
unsigned scaling_choice_current();
void scaling_choice_set(unsigned i);
// The whole multiple the screen is of both the chosen view and the native
// 240x160 (the title and the menus), 0 when not: Scaling changes nothing then.
int scaling_exact();

// Options > Display > Bottom Screen (Android, a second screen): the same
// three for StatsView's 248x184 canvas. Its size comes from StatsView; the
// row is off with none (exact -1) or when the screen is a whole multiple
// (exact > 0). bottom_scaling_mode(): what StatsView draws (0 Integer where it
// changes nothing). bottom_scaling= in enhancements.cfg.
// Options > Display > Shader's choice, kept for the next start (kol_shaders.h
// reads and applies it): "" none, or "user:<path>".
const std::string& shader_choice();
void shader_choice_set(const std::string& key);
// Its parameters as the player set them: "name=value;..." ("" the preset's own).
const std::string& shader_params_choice();
void shader_params_set(const std::string& values);

unsigned bottom_scaling_choice_current();
void bottom_scaling_choice_set(unsigned i);
void bottom_screen_size_set(int w, int h);
int bottom_scaling_exact();
int bottom_scaling_mode();

// ── The depth of field's STRENGTH ─────────────────────────────────────────
// How far the farthest terrain is blurred: the radius of its blur disc, in
// frame pixels. A short list like the view size; persisted as dof_radius=.
unsigned blur_choice_count();
const char* blur_choice_label(unsigned i);
unsigned blur_choice_current();
void blur_choice_set(unsigned i);
float blur_radius();

// ── The sound ─────────────────────────────────────────────────────────────
// 0 the hardware mix as the game made it; 1 (HQ, the default) the music
// voices re-rendered in high quality with the songs' own left/right pan;
// 2 (HD) the same with high-definition instruments (kol_hd_audio.h), HQ
// when their bank is missing. The re-render is gbarecomp's MP2K shadow:
// every tick is checked byte for byte against the game's sound driver and
// plays the hardware mix whenever it does not match. Persisted as
// music_quality= (an older sound= is read once, its HQ mono as HQ).
// Music and effects volumes, 0..100 in steps of 10 (choice i = i * 10%).
// Like the placement they need a high-quality mode. Persisted as
// music_volume= and effects_volume=.
unsigned volume_choice_count();
const char* volume_choice_label(unsigned i);
unsigned volume_choice_current(bool effects);
void volume_choice_set(bool effects, unsigned i);
// Ambient occlusion on the perspective battlefield (kol_focus.cpp): Off,
// Half tile, Full tile; saved as ambient_occlusion=. Its reach in world
// units, 0 for off.
unsigned ao_choice_count();
const char* ao_choice_label(unsigned i);
unsigned ao_choice_current();
void ao_choice_set(unsigned i);
float ao_reach_units();
// Rumble (kol_rumble.h): Off, Light, Medium (the default), Strong; saved as
// rumble=. The strength every effect is scaled by, 0 for off.
unsigned rumble_choice_count();
const char* rumble_choice_label(unsigned i);
unsigned rumble_choice_current();
void rumble_choice_set(unsigned i);
float rumble_strength();
// Shadows cast by the terrain on the perspective battlefield (kol_focus.cpp):
// Off, On; saved as shadows= (0 off, else on). The sun's height above the
// ground in degrees, 36 (the default sun; its direction and a map's own sun
// are kol_focus's business), 0 for off.
unsigned shadow_choice_count();
const char* shadow_choice_label(unsigned i);
unsigned shadow_choice_current();
void shadow_choice_set(unsigned i);
float shadow_sun_degrees();

// Debug: the map a battle started from the world map is fought on (Training
// and random battles; kol_actions.cpp), 0 the game's own, else 1..
// debug_map_count(), the game's map numbers. Saved as debug_map=;
// KOL_DEBUG_MAP=<n> overrides it (headless runs).
unsigned debug_map_count();
unsigned debug_map_current();
void debug_map_set(unsigned n);
// Which maps a Training battle can show: the battle layout ones (1, 3-56,
// 104-115). The others lay their layers out where the battle interface
// lives, and show in a story scene only (New Game). The menu lists them
// apart: Battle Map steps through the first, Scene Map the rest.
bool debug_map_in_training(unsigned n);

// VS Mode's other GBA (desktop, kol_vs_cpu.h): 0 "Computer" (an unseen copy
// of this game, played by the game's AI), 1 "Network" (a player on the local
// network, kol_vs_net.h). Saved as vs_mode=; Computer unless chosen.
// A VS match over the network is on (kol_vs_net): what would change one
// machine's copy of a game and not the other's is held off for it (the
// analog cursor: it moves the game's cursor outside the buttons).
void settings_net_match(bool on);
bool settings_net_match_on();
// Bumped at every save of enhancements.cfg, i.e. at every change of a setting.
unsigned settings_revision();
// The music silent (true) or at its volume again: while the held game runs
// a few frames for a change (kol_menu's menu_frame_tick). Asks the mixer
// only when it changes.
void settings_music_quiet(bool quiet);
// enhancements.cfg's path ("" before settings_init or without a folder).
std::string settings_file_path();
// The display's size given to settings_init (landscape; 0 unknown).
void settings_panel(unsigned* w, unsigned* h);
unsigned vs_mode_choice_count();
const char* vs_mode_choice_label(unsigned i);
unsigned vs_mode_choice_current();
void vs_mode_choice_set(unsigned i);

unsigned sound_choice_count();
const char* sound_choice_label(unsigned i);
unsigned sound_choice_current();
void sound_choice_set(unsigned i);
// Does the player want the second-screen panel? The Android push thread asks
// this rather than reading the setting array directly.
bool second_screen_wanted();
// Whether the Thor's second display is up (the Android thread says so) and
// the player has the panel on: then it already shows the unit, and the game's
// own unit box on the map is left out (kol_widescreen.cpp, panel pinning).
void set_second_screen_present(bool present);
bool second_screen_shown();

// ── Graphics presets ──────────────────────────────────────────────────────
// One step for every picture setting at once: 0 "Enhanced" (the user's Thor
// settings of 2026-10-06: 384x216, perspective, depth of field Medium,
// ambient occlusion Full tile, shadows, torchlight, unit shadows,
// lifted props and walls, see-through move range, rain across the view; and
// the default on a device with no enhancements.cfg since 2026-10-11),
// 1 "Original" (all of them off, 240x160, and the sound HQ); Fast Loading goes with them. The
// second screen, the controls and Debug are not part of it. Enhanced sets the sound to HD. preset_current() is the preset the
// settings match now, -1 none ("Custom").
unsigned preset_count();
const char* preset_label(unsigned i);
int preset_current();
void preset_apply(unsigned i);

// Stable identifiers for the menu and the config file.
const char* setting_key(Setting s);
const char* setting_label(Setting s);

}  // namespace kol
