#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "kol_workers.h"   // WorkerTimer, worker_time_job_done

namespace kol {

// What a pixel of the battle map is, for present-time effects.
enum : std::uint8_t {
    kSceneNone = 0,      // nothing the map places: sky
    kSceneFlat = 1,      // a surface facing up (a cell's top)
    kSceneUpright = 2,   // upright: a wall, a prop, a unit, the cursor
    kSceneOverlay = 3,   // interface drawn with a unit while the combat camera
                         // is on: placed as upright, in front of everything
};

// A pixel's layer codes (gbarecomp's layer map, ScenePixels::scene_codes):
// the low nibble the layer on top, 0-3 BG0-BG3 or these two, the high
// nibble the one under it.
constexpr unsigned kLayerObj = 4u;        // a sprite
constexpr unsigned kLayerBackdrop = 5u;   // the backdrop, the sky

// Every pixel's place on the battle map, rebuilt from guest memory (see
// kol_focus.cpp). World units: xz = X + Z (32 a row of cells), y = height
// (16 a step). Screen pixel (px, py) of the frame is hardware
// (px - ox, py - oy), and a place projects to
//   hardware x = (X - Z) / 2 + kx,   hardware y = xz / 4 - y / 2 + ky.
// An upright pixel's place is where it stands (a unit's feet), so it sits
// above that point on screen.
// KOL_POT_LEAN's default, a fire pot's lean (the user's pick; 0.25, then 0.1,
// leaned too much): kol_focus's lamps and kol_perspective's pool shift must
// agree. Each file keeps its own clamp.
constexpr float kPotLeanDefault = 0.06f;
// A map's sky (ScenePixels::sky).
enum SkyRamp : int { kSkyGame = 0, kSkyGrey = 1, kSkyDusk = 2, kSkyBlack = 3, kSkyMeadow = 4 };
// A lamp's hue (SceneLamp::hue, ScenePixels::glow_hue): the row of each
// table that colours its light (kol_perspective torch_tint's, kol_rain's
// kLampRgb), each kHueCount rows. The embers' red is map 63's campfire; the
// green, map 83's torches (their flames are green in the art); the lantern,
// halfway from the torches' orange to the warm daylight (map 52's lanterns).
enum LampHue : std::uint8_t { kHueOrange, kHueBlue, kHueDaylight, kHueSkylight, kHueEmbers, kHueGreen, kHueLantern, kHueCount };
// A light of this frame in the world (torches, flames, pots, a map's
// lamps carried by its units), for what is lit in the air (kol_rain).
// `hue` as torch_tint's: 0 orange, 1 blue, 2 warm daylight, 3 cool skylight
// (LampHue).
struct SceneLamp {
    float x, y, z, reach, power, flick;
    int hue;
};

// Map 73's waterfalls (tools/models/room73.py FALLS, the same order, its parts 26 on):
// a sheet on the plane X = x1 (on_x) or Z = z1, top and foot its heights.
struct MapFall { float x0, x1, z0, z1, top, foot; bool on_x; };
constexpr int kFallsMap = 73, kFallsFirstPart = 27, kFallCount = 12;   // (first: g_modeled, part + 1)
constexpr float kFallPool = 33.2f;
constexpr MapFall kMapFalls[kFallCount] = {
    {128, 192, 32, 64, 258, 192, false}, {128, 192, 64, 96, 194, kFallPool, false}, {224, 256, 32, 64, 194, kFallPool, false},
    {288, 352, 32, 64, 242, kFallPool, false}, {384, 416, 32, 64, 210, 128, false}, {384, 416, 64, 96, 130, kFallPool, false},
    {32, 64, 128, 160, 242, 176, true}, {64, 96, 128, 160, 178, kFallPool, true}, {32, 64, 192, 256, 226, kFallPool, true},
    {32, 64, 288, 320, 178, kFallPool, true}, {32, 64, 384, 448, 210, 96, true}, {64, 96, 384, 448, 98, kFallPool, true}};
// A fall's cloud of vapor this picture, at half the scene's resolution, on the
// world's screen ((X - Z) / 2, (X + Z) / 4 - Y / 2): coverage and tone (premultiplied)
// per cell from (wx0, wy0).
struct FallVapor {
    int wx0 = 0, wy0 = 0, w = 0, h = 0;
    std::vector<float> cells;   // (coverage, tone) x w x h
};

struct ScenePixels {
    int w = 0, h = 0, ox = 0, oy = 0, kx = 0, ky = 0;
    // The view in the frame: all of it, unless the frame is a scene composed
    // past the view (gbarecomp's scene overscan).
    int vx = 0, vy = 0, vw = 0, vh = 0;
    const std::int32_t* xz = nullptr;
    const std::int32_t* y = nullptr;
    const std::uint8_t* kind = nullptr;
    const std::uint8_t* iface = nullptr;   // nonzero: an interface sprite
    // Nonzero: interface to draw flat where its place has gone, the cursor's
    // floating hand, which stands at (cursor_xz, cursor_y). Null when none.
    const std::uint8_t* floating = nullptr;
    // Nonzero: interface over a lifted prop that an overlay card draws (the
    // prop's card keeps the prop's own colour there). Null when none.
    const std::uint8_t* ovl_card = nullptr;
    int cursor_xz = 0, cursor_y = 0;
    // A spell's effect (the combat camera for a spell, kol_perspective.cpp):
    // the picture the game draws flat for it -- Summon Darkness's floating
    // circle and its column -- stands as one card on the unit it is centred
    // over, whose feet are at scene pixel (spell_fx, spell_fy) and world
    // place (spell_xz, spell_y). False when no spell or no such unit.
    bool spell_card = false;
    // A piece of the map the game animates on its interface layers (kFallCards): the
    // HUD layers' pixels inside the native screen and the interface sprites are one
    // card in a plane of the world: three of its points at scene pixels (fall_fx[i],
    // fall_fy[i]), world places (fall_xz[i], fall_y).
    bool fall_card = false;
    // fall_ready: the map and the story event are the card's, the anchors below are
    // set, whether or not the piece is on the interface layers yet (before it is, the
    // warp records the light the scene puts on it). fall_v: BG2's and BG3's vertical
    // offsets (a layer's pixels have fallen by its first offset as a card minus this).
    bool fall_ready = false;
    int fall_v[2] = {0, 0};
    unsigned fall_on = 0;   // bit 0 BG2 on, bit 1 BG3 on
    int fall_fx[3] = {0, 0, 0}, fall_fy[3] = {0, 0, 0}, fall_xz[3] = {0, 0, 0}, fall_y = 0;
    int spell_fx = 0, spell_fy = 0, spell_xz = 0, spell_y = 0;
    // Options > Water: 1 on the water's surface, the map's own pixels on the
    // flat top of a cell whose terrain is water (class 2 of the terrain
    // table: records 10-12; or tagged water in the editor); 2 on the rest the terrain pass placed (the
    // shore, and water falling down a step); 0 elsewhere (the sky, sprites,
    // what lies past the cells). Null when off or none in view.
    // `serial` counts the guest's frames, for the water's motion.
    const std::uint8_t* water = nullptr;
    // The ice's flat top (1; kol_focus ice_mask, Options > Water), null none.
    const std::uint8_t* ice = nullptr;
    // A lifted pixel whose own surface is the ice's flat top (1; a lifted
    // wall's square of ice, drawn from the atlas over `ice`), null none.
    const std::uint8_t* ice_lift = nullptr;
    // With the ice: the weather's particles' pixels (1; kol_focus
    // g_weather_px, from the latched OAM), which a reflected ray passes
    // through, and the live flakes / drops for their own reflections: each
    // sprite's top-left in the scene and the row of the ground it falls to
    // (a guess kept for its whole fall, see kol_focus weather_reflections).
    const std::uint8_t* weather = nullptr;
    struct Flake { int x, y, ground; };
    const Flake* flakes = nullptr;
    int flake_count = 0;
    unsigned long serial = 0;
    // A map's cut edge faded to the background (kol_focus, kEdgeFadeMaps):
    // per pixel how much of the map's colour it keeps, 255 all, 0 none (the
    // backdrop's colour on its row). Null when none this frame.
    const std::uint8_t* edge_fade = nullptr;
    // The map's backdrop recoloured (kEdgeFadeMaps): a ramp by brightness
    // (kSkyGame: the game's colours), then `sky_desat` toward grey.
    int sky = 0;
    float sky_desat = 0.0f;
    float sky_mix = 1.0f;   // how much of the recoloured sky (a scene's sunset: as it goes down)
    // The recoloured sky dimmed as the map's own palette is (map_palette_dim):
    // a scene that darkens the map's colours, not the backdrop's, darkens it too.
    float sky_dim = 1.0f;
    // The map's own palette against its brightest since it came up
    // (map_palette_dim), on every map: what our own effects' colours follow
    // (map 73's falls).
    float map_dim = 1.0f;
    // How warm the terrain's palette is now: (red - blue) / (red + green + blue)
    // summed over map 84's two ice rows of the BG palette (0 and 3, entry 0
    // left out; kol_focus), 0 on other maps. Map 84's scene holds its grey sky
    // by it (kol_perspective smooth_sky).
    float pal_warm = 0.0f;
    // Units drawn whole from their own sprites (with the perspective only;
    // kol_focus_atlas.cpp, "Units from their own sprites"). A lifted pixel (lift =
    // block index + 1) is a unit's in the frame: the mesh places the scene
    // without units there (under_*) and draws the unit from its block, a
    // kSpriteSlot square of the atlas: slot s at column (s % per_row) *
    // kSpriteSlot, row (s / per_row) * kSpriteSlot, atlas_w = w wide.
    static constexpr int kSpriteSlot = 32;
    // A prop's light at `rows_up` scene rows over its foot: `dark` darker up
    // to `raise` rows over the foot (and below it), then easing out (squared)
    // to none `rows` rows higher, as if it shaded itself where it meets the
    // ground.
    // The terrain's shadow over a card (a unit, a prop) at scene row `row`:
    // the colour's factor, full light above shade_row, `k` below, eased
    // (smoothstep) over `soft` rows centred on the line (0: a hard cut).
    static float card_shade(float row, float shade_row, float k, float soft) {
        if (shade_row >= 1.0e8f || k >= 1.0f) return 1.0f;
        if (soft <= 0.0f) return row > shade_row ? k : 1.0f;
        float t = (row - shade_row) / soft + 0.5f;
        if (t <= 0.0f) return 1.0f;
        if (t >= 1.0f) return k;
        t = t * t * (3.0f - 2.0f * t);
        return 1.0f - (1.0f - k) * t;
    }
    static float prop_base_light(float rows_up, float rows, float dark, float raise) {
        if (rows <= 0.0f || dark <= 0.0f || rows_up >= raise + rows) return 1.0f;
        const float t = rows_up <= raise ? 1.0f : 1.0f - (rows_up - raise) / rows;
        return 1.0f - dark * t * t;
    }
    struct SpriteBlock {
        int sx, sy, w, h;   // the sprite's rectangle in the scene
        int slot;           // its square in the atlas
        int xz, y;          // the unit's place
        // The terrain's shadow over it: scene rows below shade_row are in
        // shade (lit * shade_k); none when shade_row is past the square.
        float shade_row;
        float shade_k;
        // A BG1 prop lifted off the ground (KOL_PERSP_LIFT_PROPS): drawn in
        // depth order with the map, not after it as units are.
        bool prop = false;
        // Added to a prop's sort depth (a rock spire past its front
        // neighbours' floor), its place unchanged.
        int sort_extra = 0;
        // A card bent along its cell's front edges (a V): scene column
        // bend_sx is at xz + bend_tip, each column out 2 behind, to 32 behind
        // at 16 out (the side corners): a spire's tip at its place, a bush's
        // 32 past (its card in the cell's middle). -1: flat.
        int bend_sx = -1;
        int bend_tip = 0;
        // Height units added to the card's ground left and right of the tip
        // (a V whose arms stand on floors a step apart; kol_focus
        // kVStepCutouts).
        int bend_dyl = 0, bend_dyr = 0;
        // BG1 terrain over BG0, lifted the same way (KOL_PERSP_LIFT_PROPS):
        // not a card but the surface it is, each corner placed from its own
        // pixels' places (xz, y, kind) and each pixel a quad of its own.
        bool surface = false;
        // A prop's shade over its own foot (prop_base_light): scene row
        // foot_row is its foot, darkest there, open base_rows rows up.
        float foot_row = 0.0f, base_rows = 0.0f, base_dark = 0.0f, base_raise = 0.0f;
        // A plant (a tree, a bush), which sways with the wind, at world
        // (wind_x, wind_z) for the gust's phase.
        bool plant = false;
        float wind_x = 0.0f, wind_z = 0.0f;
        // A prop carrying a lamp's flame on a dark map (the castle's statue,
        // record 86): lit by its own flame at self_lit 0..1 and tinted by
        // lamp self_lamp, full over its upper part and fading to dark toward
        // its lowest lifted texel; self_top and self_low are its highest and
        // lowest texels' world heights. 0: not one.
        float self_lit = 0.0f, self_top = 0.0f, self_low = 0.0f;
        int self_lamp = 0;
        // A unit fading in (map 100's scouts stepping out of a doorway,
        // kol_gulls): its texels this much its own, the rest the ground
        // under the sprite (under_rgb). 1: opaque.
        float fade = 1.0f;
        // A scene object the game draws as a unit (map 58's door and flag,
        // kSceneObjects): lit by the scene's light at each texel's own
        // pixel, as lifted terrain is, not as a person's card.
        bool scenery = false;
        // The OAM index of its first sprite entry: at the same place and
        // height, the card with the earlier entry is drawn over the other,
        // as the hardware draws the lower index on top (map 39's throne and
        // the one sitting on it, both at one unit's place: the throne was
        // drawn over its sitter; states/thor/0046).
        int oam_first = 1 << 30;
        // See-through, drawn premultiplied at this alpha over what the warp
        // puts behind it (PresentWarpMesh::alpha; map 100's smoke, 1 - EVB /
        // 16): its texels the frame's less the ground the game blended in
        // (EVB / 16 of it). 1: opaque.
        float see_through = 1.0f;
        // Options > Special Effects: drawn by an effect of our own instead of
        // the game's texels (kol_perspective effect_texel): 1 fire, 2 smoke;
        // fx_cx its middle column and fx_top its top row (scene pixels; its
        // foot is foot_row), fx_seed to tell one from another. 0: none.
        int effect = 0;
        bool fire = false;   // a flame's or the smoke's (flame_cards): lit by itself, no fire light
        // A prop carrying a blue flame (map_settings.cfg `glow_flame`, map 57's
        // Shaher): its flame's texels (blue, kol_perspective glow_flame_px)
        // keep their own colour, lit by themselves.
        bool glow_flame = false;
        // An ice crystal's (records 56 and 57): frosted, a little of what
        // lies behind it seen through, with a sheen (kol_perspective).
        // Its cell (gi), -1 none: a crystal's art spans several squares.
        int crystal = -1;
        // Interface the combat camera moved with a unit (a damage number, a
        // raindrop beside it, GUARD): a rigid card at that unit's place,
        // its colours as the game drew them (no light, no blur), drawn after
        // everything else. Its pixels in the scene show the ground under them.
        bool overlay = false;
        float fx_cx = 0.0f, fx_top = 0.0f;
        int fx_seed = 0;
        // A sprite's fire (kFireActors): no BG1 fade (the scene's BG1
        // brightening is the camp's glow, not a fade).
        bool fx_over = false;
        float fx_heat = 1.0f;   // a fire's heat: under 1 less of it white-hot, more orange
        float fx_width = 1.0f;  // a fire's width (1: 32 px across at its foot)
        int fx_palette = 0;     // a fire's colours: 0 the burning town's, 1 deeper (a campfire on lit ground), 2 green (map 83's torches)
        // (effect 3, a fall's vapor: fx_seed its fall, fx_cx and fx_top the scene
        // pixel of the world's screen origin, so a texel reads FallVapor by world.)
    };
    const std::uint8_t* lift = nullptr;
    // w x h, nonzero: a background pixel the game drew over a sprite's texel
    // by priority (a door's frame over a unit in the doorway). The sprite's
    // card keeps that texel; the warp draws these pixels again after the
    // cards, each at its own place, so the cut follows the camera.
    const std::uint8_t* over_sprite = nullptr;
    // ... and whose card each covers (its block + 1, as `lift`): drawn
    // right after that card, so nearer cards still go over it.
    const std::uint8_t* over_block = nullptr;

    // Pixels copied onto a card (its block + 1) but kept in the scene too
    // (a rock spire's V of BG0); null: none.
    const std::uint8_t* lift_copy = nullptr;
    // A model's rope pixels lifted so it is in the air to its ends (kol_focus
    // g_rope_air; null on other maps): what they open onto is the surface
    // behind, not a prop's ground, and over the sky there is nothing to place.
    const std::uint8_t* rope_air = nullptr;
    // The model's pixels where its long runs are split (kModelRunSplitMaps,
    // kol_perspective warp_runs); null on other maps.
    const std::uint8_t* run_split = nullptr;
    // ... past this far along depth from the run's first pixel (0: the default, 48).
    // Map 84: 8 (kModelRunFineMaps).
    int run_split_drift = 0;
    // Colours swapped on some model parts before the light (kol_focus
    // kModelRecolours): a pixel with recolour_mask set and its colour the
    // pair's `from` (RGB888, 0xRRGGBB) takes its `to`. Null: none.
    const std::uint8_t* recolour_mask = nullptr;
    const std::uint32_t* recolour_pairs = nullptr;   // from, to, from, to...
    int recolour_count = 0;
    // Options > Special Effects, map 73's waterfalls (kMapFalls): the model's part
    // + 1 per pixel, and each fall's vapor. Null elsewhere.
    const std::uint8_t* fall_parts = nullptr;
    const FallVapor* fall_vapor = nullptr;
    // Far scenery (kol_focus kFarParts, map 93's far hills): nonzero on its
    // pixels, which the warp moves only far_share of what the camera moves
    // them from where they are drawn at rest. Null: none.
    const std::uint8_t* far_px = nullptr;
    float far_share = 1.0f;
    // A scene object's pixels painted on the wall it stands in (map 58's
    // door, kol_focus g_wall_px): kept in the scene at the wall's place and
    // lit as the map's own (null: none).
    const std::uint8_t* wall_px = nullptr;
    // Special Effects on a sea map (kSeaPlanes): the foam drawn anew round the
    // boat (kol_perspective sea_foam). Each entry a sea pixel near the hull's
    // waterline, ahead of its bow or in its wake: k, its world X and Z on the
    // sea, and weights 0-255 for the ring (with the bow wave) and the wake.
    // `sea_foam_hide`: the game's own foam sprites' pixels (show what is under
    // them). `sea_foam_flow`: world units a frame the water runs along +Z.
    struct SeaFoamPx { std::uint32_t k; std::int16_t x, z; std::uint8_t ring, wake; };
    const SeaFoamPx* sea_foam = nullptr;
    int sea_foam_count = 0;
    const std::uint8_t* sea_foam_hide = nullptr;
    float sea_foam_flow = 0.0f;
    // Ambient occlusion of the terrain's pixels, 255 open (null when off).
    // At lifted BG1 terrain, the surface's behind it, which the scene shows.
    const std::uint8_t* ao = nullptr;
    // ... and the lifted terrain's own, for its texels (null: none lifted).
    const std::uint8_t* lift_ao = nullptr;
    // Torchlight on the terrain's pixels, 0 none .. 255 full (null when off).
    const std::uint8_t* glow = nullptr;
    // ... the torch most of it comes from, and each torch's flicker this
    // frame (a factor about 1), to multiply it by.
    const std::uint8_t* glow_lamp = nullptr;
    // The lamps' second colour there and its share (kol_focus g_glow_hue2 / g_glow_mix): null none.
    const std::uint8_t* glow_hue2 = nullptr;
    const std::uint8_t* glow_mix = nullptr;
    const float* glow_flick = nullptr;
    // ... each torch's flame on screen and its lean (see kol_focus g_glow_fx).
    // The pools' key (kol_focus g_glow_view_key): the lamps' light and the
    // map under it are the same while it is.
    std::uint32_t glow_key = 0;
    const float* glow_fx = nullptr;
    const float* glow_fy = nullptr;
    const float* glow_lx = nullptr;
    const float* glow_ly = nullptr;
    // ... and its colour: 0 orange, 1 blue.
    const std::uint8_t* glow_hue = nullptr;
    // ... and the torchlight's strength this picture (kol::glow_strength(),
    // read once here: kol_perspective's torch_tint asked it a lit pixel at
    // a time, a call across files).
    float glow_s = 0.0f;
    // Sunlight through the map's windows (godrays, experimental): per pixel
    // the sun on it through a window and the haze of the beams in front of
    // it, 0..255 each, at `win_strength`. Null when none.
    const std::uint8_t* win_patch = nullptr;
    const std::uint8_t* win_haze = nullptr;
    float win_strength = 0.0f;
    // The light's colour through them: the sun's warm (kol_focus kMapWindowTints).
    float win_tint[3] = {1.0f, 0.78f, 0.48f};
    // The windows with a light of their own (kol_focus MapWindow::own_deg,
    // map 67's moonlight): the same, on the scene's ground and in the air,
    // at `moon_strength`, in `moon_tint`. Null when none.
    const std::uint8_t* moon_patch = nullptr;
    const std::uint8_t* moon_haze = nullptr;
    float moon_strength = 0.0f;
    float moon_tint[3] = {0.55f, 0.7f, 1.0f};
    float win_white_cap = 0.0f;   // kWindowWhiteCaps: near-white art under the windows' light, at most its colour x this; 0 none
    // ... and on the units' own pixels (as unit_win_* and unit_win_*_a).
    const std::uint8_t* moon_unit_patch = nullptr;
    const std::uint8_t* moon_unit_haze = nullptr;
    const std::uint8_t* moon_unit_patch_a = nullptr;
    const std::uint8_t* moon_unit_haze_a = nullptr;
    std::size_t moon_unit_a_n = 0;
    // On a dark map with windows (kol_focus dark_windows): the light the
    // beams radiate on each pixel, 0..255, which lifts the darkness as a
    // lamp's does (with the patches). Null otherwise.
    const std::uint8_t* win_glow = nullptr;
    // The late day's warmth, 0..1 (kol_focus evening_amount): the light
    // more orange as a battle's turns go by, with Shadows on.
    float evening = 0.0f;
    // A map at sunset (kMapSunsets): the light graded warm where the sun
    // falls and cool in the shade, at this strength (0 none).
    float sunset_grade = 0.0f;
    // An indoor map's darkness (kol_focus indoor_dark): the battlefield's
    // brightness where no lamp reaches, 1 none; a lamp's light (glow) brings
    // it back up.
    float dark = 1.0f;
    // A campfire's logs (kFireActors: its sprite's texels left in the
    // scene), 1; darkened by fire_logs_shade after the light, so the flame
    // has a dark base. Null: none.
    const std::uint8_t* fire_logs = nullptr;
    float fire_logs_shade = 1.0f;
    // The ground round a campfire darkened (kol_focus, KOL_FIRE_GROUND): its
    // place on screen (scene px), the ellipse's half width (half as tall),
    // how much darker at the middle, how much redder.
    bool fire_ground = false;
    float fire_x = 0.0f, fire_y = 0.0f, fire_ground_r = 0.0f, fire_ground_depth = 0.0f, fire_ground_red = 0.0f;
    // A dark map's pool of faint light (kol_focus kDarkPools): within an
    // ellipse on the floor (world X, Z at height `pool_y`) the darkness is
    // at least `pool_level`, full over the inner `pool_inner` of the radius
    // and easing to none at its edge. pool_level 0: none.
    float pool_level = 0.0f, pool_x = 0.0f, pool_z = 0.0f, pool_rx = 1.0f, pool_rz = 1.0f, pool_inner = 0.5f,
          pool_y = 16.0f;
    // A dark map's roofed space (kol_focus kDarkCovers): inside the box
    // (world X, Z, below Y cover_y1) the darkness is cover_level times the
    // map's, eased in over cover_soft from its open side at cover_z1.
    // cover_level 1: none.
    float cover_level = 1.0f, cover_x0 = 0.0f, cover_x1 = 0.0f, cover_z0 = 0.0f, cover_z1 = 0.0f, cover_y1 = 0.0f,
          cover_soft = 1.0f;
    // On a dark map with a daylight palette (kol_focus kLampPalette): a
    // night colour (its 5-bit BGR555 key) to its daylight one, 8 bits a
    // channel, -1 none; the lamps' light turns a pixel toward it past
    // lamp_pal_from of the way from the darkness to full. Null: none.
    const std::int32_t* lamp_pal = nullptr;
    float lamp_pal_from = 0.30f;
    // The bump map's strength in the sun (kol_focus sun_bump_strength: mockups only),
    // 0 none.
    float sun_bump = 0.0f;
    // The wet floor (Options > Bump Mapping: stone bumps and slime glints in
    // the lamps' light) on this map (kol_focus kWetFloorMaps).
    bool wet_floor = false;
    // The bumps alone, from the art's brightness (kol_focus kPlainBumpMaps):
    // no slime glints, no stone shapes, no relief shadows (map 79's rock).
    bool bump_plain = false;
    // ... and only inside a box of the world (kol_focus kBumpPanels: a carved panel on a
    // wall): X, Y, Z from bump_box[0..2] to bump_box[3..5], at bump_box_strength. Off: false.
    bool bump_boxed = false;
    float bump_box[6] = {0, 0, 0, 0, 0, 0};
    float bump_box_strength = 0.0f;
    float dark_gain = 1.5f;   // how strongly a lamp's light lifts it
    // The movement / action range's tiles on screen (1), which the shading
    // spares in part (kol_focus range_mask), null when none.
    const std::uint8_t* range = nullptr;
    // On a dark map: the share of the lamps' light each pixel loses to a
    // unit's shadow (0 none .. 255 all; kol_focus cast_lamp_shadows), null none.
    const std::uint8_t* lamp_shade = nullptr;
    // ... and on the units' own pixels (null when none).
    const std::uint8_t* unit_win_patch = nullptr;
    const std::uint8_t* unit_win_haze = nullptr;
    // ... and on the lifted terrain's and props' texels (null when none).
    const std::uint8_t* lift_win_patch = nullptr;
    const std::uint8_t* lift_win_haze = nullptr;
    // A map's own lamps (kol_focus, kMapLamps) on the units: per pixel of a
    // unit drawn from its sprite, the light on it (0 none .. 255) and the
    // lamp it is from. Null when none.
    const std::uint8_t* unit_glow = nullptr;
    const std::uint8_t* unit_glow_lamp = nullptr;
    // On a dark map, the fixed lamps' light on each unit's own pixels
    // (0 none .. 255, the flicker in; kol_focus unit_card_lamp_light). Null:
    // a unit takes the light on the ground at its feet.
    const std::uint8_t* unit_lamp = nullptr;   // per atlas texel (unit cards), unit_lamp_n of them
    std::size_t unit_lamp_n = 0;
    // The window light on each unit card's texels, per atlas texel (hidden
    // parts included), unit_win_a_n of them; null when none.
    const std::uint8_t* unit_win_patch_a = nullptr;
    const std::uint8_t* unit_win_haze_a = nullptr;
    std::size_t unit_win_a_n = 0;
    const std::int32_t* under_xz = nullptr;
    const std::int32_t* under_y = nullptr;
    const std::uint8_t* under_kind = nullptr;
    // What the game drew under each sprite pixel (RGB888, w x h, read only
    // at lifted pixels): the scene without sprites, from the compositor
    // (gbarecomp g_ppu_scene_under_obj). Null when not recorded.
    const std::uint8_t* under_rgb = nullptr;
    // Recorded with it: the scene's own layer codes (top | second << 4), so
    // a lifted pixel's second is the layer its ground comes from.
    const std::uint8_t* scene_codes = nullptr;
    // Each scene row's backdrop as the game set it for that line (RGB888, h
    // rows; gbarecomp latched_row_backdrop): a sky gradient made by HBlank DMA
    // is there on rows the map covers whole. Null when not recorded.
    const std::uint8_t* row_backdrop = nullptr;
    // Debug > Tile Info: the target cell (cy * 32 + cx, -1 none), each
    // pixel's cell as the terrain pass drew it (-1 none), and the readout,
    // lines split by '\n'.
    int tile_target = -1;
    // ... and a rectangle of cells tinted lighter (x0, z0, x1, z1; x0 -1
    // none): the editor's roof under the target, or the one being marked.
    int tile_rect[4] = {-1, 0, 0, 0};
    const std::int16_t* cell_owner = nullptr;
    const char* tile_text = nullptr;
    const SpriteBlock* blocks = nullptr;
    int block_count = 0;
    std::uint8_t* atlas_rgb = nullptr;       // atlas_w x atlas_h, RGB888
    const std::uint8_t* atlas_opaque = nullptr;
    std::uint8_t* atlas_blur = nullptr;      // filled by the focus provider
    int atlas_w = 0, atlas_h = 0, atlas_per_row = 0;
    // The game's brightness effect on BG1 this frame (BLDCNT effect 3 / 2,
    // BLDY): the scene's fades. Options > Special Effects' fire and smoke,
    // drawn in colours of their own, take it as the game's would (darkened
    // by fx_dark, whitened by fx_white, 0..1).
    float fx_dark = 0.0f, fx_white = 0.0f;
    // The same for the sprite layer (BLDCNT's OBJ bit): a sprite's fire
    // (SpriteBlock::fx_over, map 63's campfire) fades with it.
    float fx_obj_dark = 0.0f, fx_obj_white = 0.0f;
    // The scene's brightness through a palette fade (the scenes fade PAL RAM
    // from their full palette at 0x0202D90C), times BLDY's darkening of the
    // whole picture: 1 none, 0 black. For what is added on top of the art
    // (the godrays' haze), which would stay lit as the scene fades out.
    float scene_bright = 1.0f;
    // The game's brightness increase over the map (BLDCNT effect 2 on BG0
    // and BG1, BLDY / 16): 0 none, 1 white. The frame is already whitened,
    // so the light is laid under it (kol_perspective under_white): at 1 the
    // shadows and occlusion are gone with the rest of the picture.
    float scene_white = 0.0f;
    // ... on the backdrop alone (BLDCNT effect 2, target BD only): the
    // lightning's flash behind map 58's ship in the opening (states/thor/
    // 0006, BLDCNT 00a0, BLDY to 16: a white sky, the ship's mast a black
    // silhouette). The darkness leaves the backdrop's pixels by as much.
    float backdrop_white = 0.0f;
    // The rain's strength, 0 none (or snow) to 1 the game's heaviest: its
    // weather block (0x0202A1C0: +0 drops 3/6/9, +4 1 while on, +6
    // intensity 0..2, +7 3 for snow) while drops fall. Plants sway more in
    // it (kol_perspective prop_sway).
    float rain = 0.0f;
    // ... and its intensity as the game keeps it, 0..2, -1 none (or snow):
    // our own rain's strength (kol_rain).
    int rain_level = -1;
    // A spell's closing circle (kol_circle_window_span) with Special Effects on: centre
    // and radius in the scene's pixels.
    bool ring_on = false;
    float ring[3] = {0.0f, 0.0f, 0.0f};
    // ... and its snow, 0..2, -1 none (or rain): our own snow's level (kol_snow).
    int snow_level = -1;
    // A story event (kol::story_event(), at capture), and the map's number:
    // the wet look (kol_perspective wet_effect) builds up through a battle's
    // rain and starts wet in a scene's; a new map starts it again, and so
    // does a map loaded again (map_loads: kol::map_loads(), the game's map
    // loads so far: the same map number reopened is dry).
    bool story = false;
    int map = -1;
    unsigned map_loads = 0;
    // BG0's scroll the picture was composed with (gbarecomp
    // latched_bg0_scroll), bg0_ok false when it has none: smooth pans
    // (kol_perspective pan_smooth).
    bool bg0_ok = false;
    unsigned bg0_h = 0, bg0_v = 0;
    // The frame's lights (with Torchlight on), lamp_count of them.
    const SceneLamp* lamps = nullptr;
    int lamp_count = 0;
    // A scene's sunset (kol_focus kSceneSunsets) while the game turns its
    // map's palette to the storm's: the game's BG palette (PAL RAM as
    // latched, 256 BGR555), whose change the warp shows more slowly
    // (kol_perspective palette_lag). Null: as drawn.
    const std::uint8_t* lag_pal = nullptr;
    // The sun has set (the scene's sunset at 1): once the shown palette has
    // caught up with the game's after this, it stops lagging (the game's
    // later changes, the lightning's flashes, as it draws them).
    bool lag_sun_set = false;
    // A UI layer that is one tile over the whole screen (kol::ui_field: the
    // thunder's white flash, BG3 on map 102's opening), 2 or 3, -1 none: not
    // HUD here. The warp keeps its pixels out of the flat ones and blends its
    // colour over the warped picture as the game does (BLDCNT, BLDALPHA):
    // field_rgb at field_eva / 16 over a pixel whose layer (0-3 BG, 4 OBJ,
    // 5 backdrop) is a bit of field_second at field_evb / 16, else it alone.
    int field_layer = -1;
    // HUD layers the game blends over the picture as light (an alpha blend
    // with BG2/BG3 first: the beams from Shaher on map 57, states/thor/0208),
    // a bit per layer (bit 2, bit 3), 0 none: not HUD here either. Their
    // pixels' own light, the picture under them taken out (kol_focus capture),
    // frame coordinates: the warp draws it see-through over the moved picture
    // instead of the frame's pixels, which hold the picture as it was flat.
    struct HudGlowPx { std::int16_t x, y; std::uint8_t r, g, b; };
    unsigned hud_glow_layers = 0;
    // The blend's second targets (BLDCNT bits 8-13 as bits 0-5: BG0-3, OBJ,
    // the backdrop): a glow layer's pixel over none of them is not blended by
    // the game, drawn whole (kol_perspective hud_pixel).
    unsigned hud_glow_second = 0x3Fu;
    const HudGlowPx* hud_glow = nullptr;
    int hud_glow_n = 0;
    // The point the light fans out from (frame pixels), when it does
    // (hud_glow_apex_ok): the game draws it over its 240 x 160 only, and the
    // warp carries it on to the view's edges, scaled about this point.
    bool hud_glow_apex_ok = false;
    // In a battle action (Capture::hud_glow_add): the light added as the game
    // blends it, its colour times EVA (hud_glow_eva) / 16, not normalised and
    // drawn at 0.75 of its brightest channel (kol_perspective hud_glow_over).
    bool hud_glow_add = false;
    int hud_glow_eva = 16;
    float hud_glow_apex[2] = {0.0f, 0.0f};
    // The field (field_layer) shown through the game's windows, when WIN0 or
    // WIN1 is on and does not show it everywhere (map 57's scene, states/
    // thor/0209: a white BG2 between two circles the game writes into WIN0H
    // and WIN1H line by line, a ring; then everywhere but inside WIN0): the
    // warp draws it over the moved picture where the windows show it
    // (kol_perspective window_light_over) in place of the blend over every
    // pixel. field_layer is -1 then. Native coordinates (the game's 240 x 160).
    struct WindowLight {
        bool on = false;
        // Each window: off, or a disc (its lines' spans fit a circle: carried
        // on past the native lines and columns as that circle), or its spans
        // as the lines had them ([x0, x1) a line, nothing past the native area).
        struct Win {
            bool on = false, disc = false;
            float cx = 0.0f, cy = 0.0f, r = 0.0f;
            std::uint8_t x0[160] = {}, x1[160] = {};
        } win[2];
        // The field inside WIN0, inside WIN1 alone, outside both: 0 not shown,
        // 1 blended (rgba), 2 as it is (opaque).
        std::uint8_t show[3] = {0, 0, 0};
        // The blend as one colour: `add` (EVB 16, the game adds the light)
        // rgba is added, colour * alpha (gbarecomp PresentWarpMesh::
        // over_add_from: the game's sum, clamp and all); else laid over at
        // its alpha. `eva`: the blend's first weight (hud_glow's with it).
        std::uint8_t rgba[4] = {0, 0, 0, 0};
        bool add = false;
        int eva = 16;
        std::uint8_t rgb[3] = {0, 0, 0};       // the field's own colour
        // A disc's centre: the light is about that point of the picture, and
        // moves and grows with it under the warp (hud_glow with it).
        bool anchor = false;
        float ax = 0.0f, ay = 0.0f;
    };
    WindowLight window_light;
    std::uint8_t field_rgb[3] = {0, 0, 0};
    int field_eva = 16, field_evb = 0;
    unsigned field_second = 0;
    // Fire light (with Torchlight): the burning town's flames and the archers'
    // fire arrows (maps 98, 99), each a soft orange pool by world distance,
    // no shadows; an arrow also a halo of `halo` scene pixels about itself.
    struct FireLight { float x, y, z, reach, power, halo; };
    const FireLight* fire_lights = nullptr;
    int fire_light_count = 0;
    // A spell's light (with Torchlight; kol_focus spell_lights): each cluster
    // of its effect's bright, saturated pixels (its sprites, and the affine
    // layer of an effect such as Ice Javelin's spike) a pool of their colour
    // (r, g, b: the colour over its brightest channel), from over the unit
    // under it at the effect's height.
    struct SpellLight { float x, y, z, reach, power, r, g, b; };
    const SpellLight* spell_lights = nullptr;
    int spell_light_count = 0;
    // A spell's circle the game draws on BG2, laid on the floor
    // (map_settings.cfg `magic_circle`; kol_focus circle_capture,
    // kol_perspective magic_circle): BG2's 256 x 256 tilemap, 1 where it
    // draws; scene pixel (x, y) is texel (x + circle_su, y + circle_sv)
    // inside circle_box (x0, y0, x1, y1; no wrap); the colour it adds
    // (already x EVA / 16), and the blend's EVA and EVB (the floor under
    // it at EVB / 16). Null: none.
    const std::uint8_t* circle = nullptr;
    int circle_su = 0, circle_sv = 0;
    int circle_box[4] = {0, 0, 0, 0};
    std::uint8_t circle_rgb[3] = {0, 0, 0};
    int circle_eva = 0, circle_evb = 16;
};
// Atlas square `slot`'s top-left texel, x then y, with `per_row` squares a
// row (ScenePixels::kSpriteSlot).
struct AtlasXY { int x, y; };
inline AtlasXY slot_origin(int slot, int per_row) {
    return {(slot % per_row) * ScenePixels::kSpriteSlot, (slot / per_row) * ScenePixels::kSpriteSlot};
}
// A light's halo on screen: each pixel of a w x h picture within `halo`
// pixels of (fx, fy), f(px, py, r), r its distance over `halo` (below 1),
// a pixel taken at its corner plus `centre_off` (0.5: its middle, kol_focus's
// lamps; 0: its corner, kol_perspective's fire arrows).
template <class F>
void for_halo(float fx, float fy, float halo, int w, int h, float centre_off, F&& f) {
    const int hr = static_cast<int>(std::ceil(halo));
    for (int py = std::max(0, static_cast<int>(fy) - hr); py <= std::min(h - 1, static_cast<int>(fy) + hr); ++py)
        for (int px = std::max(0, static_cast<int>(fx) - hr); px <= std::min(w - 1, static_cast<int>(fx) + hr); ++px) {
            const float r = std::hypot(px + centre_off - fx, py + centre_off - fy) / halo;
            if (r >= 1.0f) continue;
            f(px, py, r);
        }
}
// Rebuilds the places for the frame being presented; false when this is not
// a battle map. The arrays stay valid until the next call.
// The torchlight's strength (Options > Torchlight, or KOL_PERSP_GLOW), 0
// off; kol_perspective applies the light at it.
float glow_strength();
// Whether any of the light (Ambient Occl., Shadows, Torchlight) is on.
bool lighting_on();

// `rgb`, the frame's colours (w x h x 3), lets a pedestal tell its stone
// from the floor around it; null takes its whole strip.
// Lifted Props / Lifted Walls (experimental): BG1 props as cards off the
// ground, BG1 terrain as its own surface, the scene behind them from the
// compositor (g_ppu_scene_under_bg1). True when either is on.
bool perspective_lift_props();

// The rows the terrain's shadow eases over on units and props
// (ScenePixels::card_shade; KOL_CARD_SHADE_SOFT=<rows>, 0 a hard cut).
float card_shade_soft();
bool scene_pixels(const std::uint8_t* layers, int w, int h, ScenePixels* out,
                  const std::uint8_t* rgb = nullptr);
// A battle action's light layer over BG1 (Capture::hud_bake): the frame's own
// colours written into the scene (w x h, the latched scene's size), so a
// prop's card carries the effect as the game blended it. Before scene_pixels.
void hud_bake_into(std::uint8_t* rgb, int w, int h);

// Depth of field on the battle map, following the cursor (or, with no cursor,
// the unit that is moving). Installs (or removes) gbarecomp's present-time
// focus provider: every pixel of the presented frame gets a blur amount from
// its distance to the focus along the camera's line of sight. Present-time
// only; frames and hashes are unaffected.
void focus_set_enabled(bool on);

// While a unit takes its action and the cursor is out choosing its move or
// target, the record of the unit on the cursor's cell; 0 otherwise.
std::uint32_t cursor_unit_record();

// The record of the unit whose turn it is, or 0.
std::uint32_t acting_record();

// The record of the unit the acting unit's action is aimed at, when the game
// has named one (an attack or a spell on a unit; not one aimed at a cell), or 0.
std::uint32_t acting_target();

// True while the map cursor is out (browsing, or choosing a move or target).
bool cursor_on_map();

// True while one of the player's units takes its action, menus included.
// Never in battle preparation, nor in an AI army's phase.
bool unit_in_action();

// An army the game plays takes its phase: the enemy's, or an allied army's
// such as Rictor's on map 2.
bool ai_phase();
// A story event (a scene, a talk on the map) is playing: no battle is being
// fought, whatever the map looks like. Preparation's NAVI script counts too.
bool story_event();

// Whether an OAM entry (its attr2) is a scene object's painted light or
// shadow that our light replaces (map 67's door: its shadow's pieces while
// the light through the door is drawn), so the compositor leaves it out.
// Any thread; set by the focus pass (one picture behind).
bool object_decal_hidden(std::uint16_t attr2);

// Battle preparation: deployment, the roster list and its view mode.
bool battle_preparation();

// True while an attack or spell waits on its Attack/Cancel confirmation.
bool target_confirming();

// The map cursor's cell, from its sprite slot; false when it has none.
bool map_cursor_cell(int* x, int* z);

// True while a unit's move is being chosen, and while the cursor is also
// still on that unit.
bool move_choice_active();
bool move_choice_on_actor();

// True while the acting unit's cursor is out only to look around: its turn,
// with neither a move nor a target being chosen.
bool cursor_browsing();

// True while the game shows a damage number over a sprite.
bool damage_number_shown();
// The map object the damage number is over (0x0202AFCC), 0 when none is up.
std::uint32_t damage_number_object();
// The map object standing where the damage number is (the unit it is for),
// 0 when none is up or no unit stands there.
std::uint32_t damage_number_unit();

// Screen positions (hardware pixels, the original 240x160 frame's space;
// the widened view extends it on both sides) from the game's sprite slots,
// for the sound placement (kol_sound.cpp). False when there is no slot.
// The map object of the unit whose turn it is (EWRAM 0x0202A5F0), or 0.
std::uint32_t acting_object();
// The map object whose record is `record`, or 0.
std::uint32_t object_of_record(std::uint32_t record);
// The talent the acting unit is using: its targets' map objects (up to
// `max`, the count returned; 0 when no talent of the acting unit's is up).
// What each takes: talent_results.
struct TalentResult {
    std::uint32_t object = 0;
    bool known = false, heal = false;
    int amount = 0;
};
int talent_targets(std::uint32_t* objs, int max);
// The talent's result list (0x02029EC0): the units it actually takes, which
// need not be its targets (an Ice Javelin hits a unit in the way: ID), with
// what each takes; 0 when no talent of the acting unit's is up.
int talent_results(TalentResult* results, int max);
bool object_screen_pos(std::uint32_t object, int* x, int* y);
bool cursor_screen_pos(int* x, int* y);

// BG0's scroll for the frame about to be drawn; the frame hook calls this
// every real (non-speculative) frame. It places the terrain exactly.
void focus_note_scroll(unsigned bg0_hofs, unsigned bg0_vofs);

// The present's capture: gbarecomp's g_present_capture_hook (runtime.h). On
// the guest's thread, at present, copies everything the present path
// (scene_pixels, the focus provider, the warp) reads from the guest, so it
// may run on gbarecomp's present worker a frame behind.
void present_capture();
// Marks the calling thread as on the present path while it lives: the
// guest-state queries above answer from the capture, not live memory.
struct PresentScope {
    PresentScope();
    ~PresentScope();
    bool prev;
};
// The scene's excluded (HUD) layers as captured, for the warp.
unsigned present_exclude_layers();
// The layer map a present provider works from. The one given, except while
// BG2 is a piece of the world in the scene (map 84's cliff over its stairs,
// kol_widescreen world_bg2_frame): then a copy with BG2 read as BG0, the
// ground it covers (one copy a picture: the same pointer to every provider).
// Call inside a PresentScope.
const std::uint8_t* present_layers(const std::uint8_t* layers, int w, int h);


// Debug > L2 Moves Sun: the sun moved along an arc over the battlefield, in
// the vertical plane of the Options > Shadows pick's direction, from that
// side's horizon over the top to the other (5..175 degrees along it), by
// degrees toward the screen's right (> 0) or left. On the guest's thread;
// reset when the option is off. The report logs where it is, as
// KOL_PERSP_SHADOW_ELEV and KOL_PERSP_SHADOW_DIR values.
void sun_arc_move(float degrees_right);
void sun_arc_reset();   // the turn too
void sun_arc_report();
// The stick's other axis (L2 + up / down): the sun's direction along the
// ground turned about the field, by degrees clockwise on screen (> 0).
void sun_turn_move(float degrees_clockwise);
// The editor's SUN tab (kol_tile_edit): save the sun as it is now as the
// map's own (drawn_heights.cfg `sun` line), or drop the map's saved one.
// Asked from any thread, done on the guest's at its next sun_tick().
void sun_save_request();
void sun_clear_request();
// Each guest frame (kol_perspective): the requests above, and what
// sun_now() answers.
void sun_tick();
// The sun as the last guest frame had it: degrees over the ground and the
// direction toward it (X, Z); false with Options > Shadows off.
bool sun_now(float* degrees, float* x, float* z);
// Whether this map keeps the right stick from moving the camera (kNoLookMaps).
bool map_no_look();
// Whether this map lets the right stick move the camera a little only
// (map_settings.cfg `small_look`).
bool map_small_look();

}  // namespace kol
