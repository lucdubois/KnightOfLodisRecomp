#pragma once

// Per-map settings (map_settings.cfg at the project's root): which maps get
// which of the enhancements' special cases. One line a setting:
//
//   <key> <map> [<map> ...]      # a comment
//
// The file is built into the game (CMake embeds it as
// kol_map_settings_baked.inc). A file of the same name beside
// enhancements.cfg (the device's files/), or KOL_MAP_SETTINGS=<path> on the
// desktop, is read instead, so a change can be tried with a restart and no
// build. An unknown key is reported on stderr and left out.

namespace kol {

enum class MapFlag {
    Indoor,          // indoor: no sun (with kDarkMaps, the map's darkness)
    NoLook,          // no_look: the right stick does not move the camera
    NoFoam,          // no_foam: the water effect without its shore foam
    NoPuddles,       // no_puddles: the wet look without puddles
    ModelAo,         // model_ao: ambient occlusion from the map's 3D model
    WetFloor,        // wet_floor: the dark map's wet stone floor
    Bg1Props,        // bg1_props: every BG1 shape is a prop
    ConeWindowTip,   // cone_window_tip: a cone window's shafts start in its shape
    ShipLight,       // ship_light: lamps lit and shadowed by the ship's model
    Flames,          // flames: BG1 flame blocks stood up as self-lit cards
    Smoke,           // smoke: the smoke columns as see-through cards
    RoundLight,      // round_light: lamps light the model's walls by their own normals
    PolishedFloor,   // polished_floor: kPolishedCells mirror as polished stone (the ice's mirror alone)
    FixedSun,        // fixed_sun: its own sun, not sunk by the turns of the battle before it
    WindowUnitShadows,   // window_unit_shadows: straight beams cast units' shadows, as a cone's do
    RainRises,       // rain_rises: our rain goes up, slower, and never lands; no wet look, no drips
    GlowFlame,       // glow_flame: a BG1 prop's blue flame lit by itself, and lighting round it
    MagicCircle,     // magic_circle: a circle the game draws on BG2 laid on the floor, glowing
    NoPropFoot,      // no_prop_foot: props have no foot shade (their lowest rows, the spot round the foot)
    OneFoot,         // one_foot: a BG1 shape (bg1_props) stands on one foot, the one under its middle
    SmallLook,       // small_look: the right stick moves the camera a little only (kSmallLook of its range)
    Count
};

// Whether `map` has the setting. Read once, on first use.
bool map_flag(MapFlag flag, int map);

}  // namespace kol
