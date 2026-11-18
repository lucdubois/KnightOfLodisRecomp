## HANDOFF — 2026-11-18: 1.0.1 released; the Steam Deck port started (read this first)

**Released:** v1.0.1 (main `255f514`, pushed, GitHub release with the APK): the save and the save states in Android's Files app (SavesProvider.java), `docs/RELEASE_1.0.1.md`. Installed over 1.0 on the Thor (`com.totkol.game`).

**Branch `steam-deck`, UNCOMMITTED in BOTH repos** (KoL: `src/kol_desktop.{h,cpp}`, main.cpp, kol_actions.cpp, both CMakeLists, docs/DEVLOG.md; gbarecomp: host_window.cpp, runtime.h, ENHANCEMENTS.md). docs/DEVLOG.md "Steam Deck" has what is built and the list of what is not.

- Built, checked headless on the Mac only: the options menu as an overlay over the game (F10 or a click of the right stick), player mode (`--data-dir`: files and settings in one directory as on Android), and a desktop fix for a view size chosen at run time (it drew enlarged).
- The Android debug build still compiles with it. The render invariants were not run (nothing in the PPU frame changed).
- **It builds and runs on the user's Steam Deck** (11-18): `ssh steamdeck`, `tools/deck_build.sh` (rsync + build in the freedesktop SDK at `~/kol`), `~/kol/run.sh` / `play.sh`, a desktop icon. 1280x800, 320x200 at 4x, GL effects, menu overlay: seen in a present dump from the Deck's screen. Dev build, Desktop Mode only; no Flatpak, no Gaming Mode run, nothing measured. DEVLOG "On the Deck" has the recipe and the X11-from-SSH trap.
- The user (11-18): has a Deck and SSH is set up (192.168.0.59); right-stick click is fine for now; yes to 16:10 on the Deck build (built: 320x200 / 384x240 / 432x270 on a 16:10 panel, Enhanced starts at 320x200; only 320x200 on BN checked).
- **Deck performance (11-18):** its cores are ~half a Mac core; map 55 with the camera panning was 47 fps. Window light + lamp light split over the row pool (byte-identical, in-frame checks `KOL_WIN_PAR_CHECK` / `KOL_LAMP_PAR_CHECK`), the window cache no longer stores one-off builds, 6 helpers on desktop, then the wall lifting split too (`KOL_LIFT_PAR_CHECK`): pans 58.6 fps, the worker 20.2 -> 14.6 ms. Still over budget at times (~179 of 1201 long frames; map 100 57 fps). The terrain pass (a painter's pass) was left. DEVLOG has every number, the bench scripts (`~/kol/bench.sh`, `out/pan.replay`) and what is left by section.
- Next, not started: the setup screen (ROM/BIOS/mod), the Save States grid over the game, a Quit row, then the Flatpak and a Gaming Mode run.

## HANDOFF — 2026-11-06: map 75, the mermaid's cave (read this first)

**Committed (this commit; nothing pushed; gbarecomp untouched).** APK 14:47 has all of it, for Ctrl+Option+I. Not seen on the Thor. Only map_ext.cfg is the user's (never commit). README "Map 75" and the sections after it; memory kol-map75-cave.

- **Model** (tools/models/room75.py, room75_extract.py -> room75_art.py, 9k triangles): the black roof at Y 320 past the height-20 cells, 114 stalactites and the vines lifted off BG0's wall, nine floor rocks anchored in a V (the user asked for the V), three stair corners cut on a diagonal (the stripes that changed with the camera; the user never confirmed these were the ones meant).
- **Light** (Thor state 147, the mermaid at 144,16,176): dark 0.12, two parallel shafts through openings in the air (`kMapWindows`, new `pool_soft` / `pool_peak`: a feathered, thinner landing), a soft fill lamp past the near right edge.

**Open:** Thor cost unmeasured (Mac worker 9.2 -> ~12.4 ms before the light, nearly all lifting ~20k pixels); strengths are my picks from sheets, the user to judge; a pale strip beside rock 4,11 at full right lean is the floor uncovered next to the rock's AO, offered to soften; depth step at column 370 is real and kept. The Thor's drawn_heights.cfg has `75 5 1 6`, `75 3 3 3`, `75 3 6 2` (the user's, same values as kDrawnCells) and its water tags: not baked. Keep tool calls short: the user steers by interrupting.

## HANDOFF — 2026-11-05, night: map 59 and 60's light, the maxed-army tool (read this first)

**Committed (this commit and the one before; nothing pushed; gbarecomp untouched).** APK 23:53 has all of it, for Ctrl+Option+I. Not seen on the Thor: the state screen's scrolling, Alphonse in bed, both rooms' light. Only map_ext.cfg is the user's (never commit).

- **Map 59** (README "Map 59 modelled", "Map 59's light", "Alphonse in the bed"): the window's sun (pick A), a weak second light on the bed's head (`own_deg`, pick 1, warm: kOwnLightTints), units' shadows from both (`window_unit_shadows`), `fixed_sun` (a map's own sun sank with the last battle's turns), near-white cap 1.08 (kWindowWhiteCaps; KOL_WIN_WHITE_CAP). Alphonse in bed = scene object by layout (only_y 0, tiles 0x200 on but the two 16x16 pieces 20 over his feet). Two suns through one window: the user to judge in motion.
- **Map 60** (README "Map 60"): same cells and art as 59, tools/models/room60.py = room59.build_parts(night=True): wall whole, no yarn, lantern on the table. kDarkMaps 0.15; lamp at the flame's screen place, 10 along the view line in front of the lantern's body (inside it: a black barrel and black bars), ring 48; faint bounce 0.12; bed's long edge rounded + `round_light 60`; `kLampModelOnlyMaps` {60} (grid cells past the bookcase's front split its light). The user chose by correcting renders, no pick from a sheet. **No state is on map 60:** viewer only (EV, KOL_DEBUG_MAP=60; scratch look.sh recipe in memory kol-map59-bedroom), the scene's characters unseen. Thor cost unmeasured.
- **Save States page** scrolls in StatsView, rows from native, thumbnails on a thread (README "Numbered save states"; KOL_STATES_ROWS_CHECK=1).
- **tools/max_army.py IN OUT [Name=Class ...]:** level 50, all five stats 999 (the game's clamp), all emblems, every item and spell x20, funds 99,999,999, counters 0, humans Neutral. The user is playing from Thor states 140/142 made with it (Evan is a Lich, class 26). New states go in files/states as the next free number; L3+L finds them at once, the page after a save or restart. Limits and class gates: memory kol-unit-record-layout. Lich in battle never exercised: the user says Liches are playable, the game's route to it was not found.

**Open:** map 60's look on the Thor; a dark wedge on the floor beside the girl on map 59, camera far left (predates the light); map 70's viewer slit. The user steers by interrupting: keep tool calls short and do not batch long runs.

## HANDOFF — 2026-10-26, morning: our own rain (read this first)

**Committed before:** KoL e727d0a / gbarecomp 7658a08 (Fast Attacks).
**UNCOMMITTED, both repos, APK 10:42 (not seen on the Thor):** README "Our
own rain". Options > Graphics > New Rain (`own_rain`, Enhanced on, Original
off): `src/kol_rain.{h,cpp}` (drops in the world, placed through the warp,
splashes where seen; game intensity 0/1/2 = 130/300/600 drops over 384x216,
2 leans 2.2x: the user's picks from tools/mockup/rain.py), called from both
providers; kol_obj_cull hides the game's drops only while ours drew
(`rain_drawn`, set at a picture's end: `RainFrame`); kol_focus
`rain_level_of` / `ScenePixels::rain_level`. gbarecomp
`PresentWarpMesh::over_xy` (coloured triangles over the warp, under the
HUD; ENHANCEMENTS). `KOL_RAIN_MOCKUP=1` + `KOL_RAIN_CAM=1` for the mockup
tool. Then from the Thor (JM = slot 2, map 58 night): slower, each fall
hashed (new landing, slant, length), wind gusts; and lit by the frame's
lamps on dark maps (ScenePixels::lamps; floor 0.35, reach x1.75, the user's
pick of three stills). Invariants 66e13ab2 / 9054c456. Thor cost not measured. Only
map_ext.cfg is the user's (never commit). Next state letter: JM.

## HANDOFF — 2026-10-25, after midnight: the ship fitted; a night lamp mocked up (read this first)

**Committed:** gbarecomp `2d78b87` (JH title band), KoL `7120b08` (map 102
props, rain sway, sunset, archers, perf) and `0b952ea` (the ship fitted to
the user's drawings on both maps: BG1ART hull on 58, bow on the water, far
rail round the nose, near rail top flat, bowsprit length; shared fits in
`tools/models/ship_fit.py`, map 97's stem-head plates; README notes after
"The same ship on map 58"). APK 23:48 has all of it and was installed.
JL = the user's slot 2 (map 58 night scene, the ship at the rock).

**UNCOMMITTED (kol_focus.cpp only, mockup work, keep or drop):**
- `KOL_MAP_LAMP=<map,ax,az,dx,dy,dz,reach,power,halo,hue>[;...]` adds
  kMapLamps (map_lamps()), desktop mockups.
- `KOL_MODEL_LIGHT=<map>` lights a map's model as 101's (model_occluder);
  tried for the plank shadow, NOT the cause; probably drop.
- unit_card_lamp_light now takes map lamps too (`KOL_UNIT_LAMP_NO_MAP_LAMPS=1`
  old): on a dark map the carrier of a map lamp went dark (light read at
  its feet, the water 70 below the gangplank). Affects only dark maps with
  map lamps (none yet).

**Then BUILT and COMMITTED (10-26, README "Map 58 at night, by torchlight", switch `kNightShip`; the user undecided night vs before).** Was parked:** map 58 at night with the sun off and
a lantern carried by Rictor. The user picked **B**: `KOL_INDOOR=1
KOL_INDOOR_DARK=0.2 KOL_MAP_LAMP="58,144,208,-2,20,7,150,1.6,12,0"`
(states/JL, Rictor at 144,80,208 on the gangplank). To make it real: map 58
in kIndoorMaps + kDarkMaps {58, 0.2, ...} (check map 58 is only ever this
night scene), and a lamp carried BY ACTOR, not by anchor (kMapLamps follow
the unit nearest a fixed point within 12): add the unit record's +0x4E
actor id to `Unit` (filled at kol_focus.cpp ~5465) and an actor field on
MapLamp; Rictor's id not read yet (KOL_OBJ_LOG=<n> prints rec/+0x4E/pos).

**The Thor's drawn_heights.cfg (pulled 10-25 00:03, NOT baked):** map 58
rows z 5-6, x 0-6 at height 2 (the channel and the gangplank cells, which
were 80-high blocks that shaded the wall under the plank from a lamp) and
`water 58 4 5 / 3 6 / 4 6`. Render with a fresh pull via KOL_TILE_EDIT_FILE.

**Then (10-25 evening), COMMITTED (kol_perspective.cpp, README "Strips
behind end at their own edge"; the user judged it correct enough):**
the user, on the Thor (JL, camera low), reported stretched strips drawing
on top of what stood in front of them (saw-teeth between the
deck and the near rail's top; other maps too with the camera low). Cause:
a corner row two surfaces share is placed by the one in front; the strip
behind is stretched to it, and with the camera low it folds back over the
rows before it, drawn after them. Change: `corner_at` factored out of
place_corners (byte-identical alone); a pass records the surface behind's
own corner (g_own_xy/px/dir) and the stitching uses it (an extra vertex,
g_own_vtx) where it reaches under the front surface; gaps still stretch.
`KOL_PERSP_STRETCH_OVER=1` = old. Result: the wedges are gone, but the band
is speckled (along a diagonal edge each deck row meets the rail for 1-2
columns, so strips switch own/shared every few px). Changed pixels vs old:
4738 at look 0,1; 213 at rest; 306 at 0,-0.8 (JL). APK 23:20 installed by
the user. Thor slot 3 = GE (map 55 castle, fountain statue; their old slot 3
in the session scratchpad thor_slots/). If the speckle bothers later: decide per run (or per edge span),
not per corner, or draw the strip behind with its own edge everywhere it
borders a different surface.

Only map_ext.cfg is the user's (never commit). Next state letter: JM.

## HANDOFF — 2026-10-23: map 102's props stand (read this first)

**UNCOMMITTED (KoL kol_focus.cpp, README "Map 102: every BG1 shape is a
prop"):** on kBg1PropMaps ({102}) every BG1 shape is a prop card at its
foot on the drawn ground (the Thor's heights, past the band too); ledge
triangles stay ground; rocks don't sway. KOL_NO_BG1_PROPS=1 old. Also
KOL_FOCUS_DUMP writes `.rgb`. Then (Thor, 18,-3: floating trees) shapes split by their feet = cell middles (KOL_BG1_PROPS_LOG=1). Then JI (slot 2, pan shadows): shapes continue past the scene / under the letterbox through the composed map (bg1_drawn + captured BG1 tiles), shadows whole (KOL_BG1_PROPS_CUT=1 old). Then plants sway more in heavy rain (rain_sway, ScenePixels::rain; KOL_RAIN_SWAY=1,1 old). Then JJ (slot 2, map 102 narration): the sun sets from the girl's exit (object +0x4E 0x5C gone, 39095) over 955 frames into the heavy rain (kSceneSunsets, wait_for; shadows kept in rain, greys eased, the game's palette change lagged ~6 s by palette_lag; KOL_NO_SCENE_SUNSET=1 off). Then JK (slot 2, map 101 -> 102 cut): palette lag only from the sunset's start; shore foam darkened by scene_bright. Then perf (reported very slow): Mac worker 16.9/22.0 -> 13.3/14.6 ms (hashes, opacity once, tile rows, upwind seeds, sun at 10 Hz); Thor 22:25 APK: 50-59 fps, worker-bound (85%); then reflect_ray linear drift, prop shadows moved with pans, APK 22:38 measured: 56-60 fps after the first 3 s (52, 8 long frames), worker 79%. Next letter JL. Rendered with the Thor's drawn_heights.cfg
pulled 10-23 (NOT baked). Invariants 66e13ab2 / 9054c456. APK 14:56, on top
of the uncommitted JH / archers work below. Next letter JI.

## HANDOFF — 2026-10-22: New Game's band across the top (read this first)

**UNCOMMITTED (gbarecomp gba_ppu.{h,cpp}, runtime.cpp, ENHANCEMENTS "A view
that changes size partway down a frame"; KoL README "A third: a band across
the top"):** JH = the user's slot 1 (title; Up, A). The view widened at line
29 of the map's first frame; the lines above are now drawn again at the new
size, and a scene not composed on every line is not presented. Invariants
66e13ab2 / 9054c456. APK 21:47 built, not installed. Next letter JI.
Then (HP/HK, map 98): the archers, 0x61 and the soldier stay after the
shot instead of being deleted during the pan (kol_gulls `archers_stay`,
README "They stay after the shot"), uncommitted, in the APK after it.

## HANDOFF — 2026-10-21: props on their V, drawn trees, ice (read this first)

**Committed (10-21 16:29, kol_perspective.cpp reflect_ray + README "Units in
the ice: no streaks"):** JF = slot 3 (map 40, angel flying over ice): unit
cards met at their depth, empty texels as the ground under, interface passed
as ground, unit hits take the sky behind. Then the angel's blob: halves paired, reach
128, wrapped Y (KOL_FLY_BLOB_SINGLE=1 old); ice under lifted pixels
(KOL_ICE_NOT_UNDER_LIFTED=1 old). JG (slot 3, map 116 dialog): ice/water
masks by scene_top and interface over the map as ground (portrait;
KOL_ICE_FRAME_TOP / KOL_WATER_FRAME_TOP old). APK 16:49.
Next letter JH.

**Committed (10-21 16:20, kol_focus.cpp g_lift_ground + README "The ground
under a lifted prop takes the props' shade"):** JE = slot 3 (map 40, rock
8,13, camera up): the floor shown under lifted prop pixels now takes the
foot spot and prop shadows. KOL_LIFT_GROUND_UNSHADED=1 old. APK 16:16. Next letter JF.

**Committed (10-21, kol_focus.cpp scene_props + README "Props cast
shadows"):** JD = slot 3 (map 40): prop shadow groups split by cell
(two pines on one diagonal/height were one caster between them).
KOL_PROP_GROUP_BY_PLACE=1 old. APK 15:42. Next letter JE.

**Committed 5eca19f (10-21):**
Enhanced preset AO = Half Tile; the options menu's enhancements are now
Preset, then pages Graphics (4, kGraphics: View Size + effects), Audio (2),
Gameplay (5, kGameplay: Shoulder Units, Analog Cursor, Rumble, the four
speed rows), Cheats (6, kCheats: Shop Anywhere, No Random Battles from
Debug), Debug. README "Four categories". Original preset also sets the
sound to HQ, Enhanced to HD (Preset::sound). APK 15:26.

**Committed:** KoL 013e192 (everything below through the ice). Only map_ext.cfg
is left modified (never committed).

**Committed before:** KoL 0b0a2b6 (map 34 trunks, spire V, drawn trunk,
bent spire cards). **UNCOMMITTED (KoL kol_focus.cpp/.h, kol_perspective.cpp,
README "Map 34: ..." > "Bushes in a V too", "And the pines"):** IN = the
user's slot 3. Records 30 (bushes) and 41 (pines) (kVPlantRecords) cards bend with the tip at the front corner (CardBend
tip 32; spires 0) and take their cell's top's front half (BG1 lifted, BG0
copied via g_spire_v); a bent card's foot shade follows its front edge.
KOL_BUSH_FLAT_CARD=1 old. The user chose this over two mock-ups (tip
forward alone floated; tip in the middle drooped). Overlapping pines
(IO, map 35) share their pixels by the front pine's triangle (g_pine_tip;
KOL_PINE_LATER_TAKES=1 old). Pines split between BG1 and BG0 (IP, map 35
cliff edge) copy their BG0 half onto the card (KOL_PINE_NO_BG0=1 old).
Pines seed only their triangle and do not spread (IQ: the wall behind
swayed with the tree; KOL_PINE_SPREAD=1 old). Pull the Thor's
drawn_heights.cfg again before each repro: the user edits it (IQ's 2,12).
Stalagmites (54, map 51 cave, IR) bend as the pines, V from the middle row
taking their own top (kVMiddleRecords; KOL_STALAGMITE_FLAT_CARD=1 old).
Committed 2053665. Then a steady cool ceiling lamp over map 51's 15,14
(IS; kCeilingLamps + CeilingLamp::steady; 220 / 0.8 hue 3, the user's
pick), committed a849e30. Bump mapping skips the range's tiles
(sp.range; IT; KOL_BUMP_ON_RANGE=1 old), uncommitted. Record 50 stones
whose foot puts them in the middle bend as the stalagmites (kVMiddleRecords
{54, 50}; IU map 49, also HY map 2), uncommitted. A spire's strip leaves a
front neighbour's BG1 top (a quarter of it BG1 outside the strip; IV, IW
map 12; KOL_SPIRE_TAKES_BG1_FLOOR=1 old), uncommitted. Map 12's leafless
trees (record 40, 6,0 and 9,8) cut from the user's painting (kDrawnCutouts
in the trees' rule; shots/tree_mask/), uncommitted. Cut-out bushes (42)
join kVMiddleRecords (IZ map 13; map 6 too), uncommitted. The tree drawing
also on map 17 (JA), uncommitted. Rocks (51) join kVMiddleRecords, and a
middle cut-out bends only when its art ends within 2 rows of its middle row
(JB map 17; DR's sliver), uncommitted. Record 32 bushes (map 17, 107) join
kMiddleRecords + kVMiddleRecords (their shadow fell in front), uncommitted.
Ice effect C (sheen + reflection + glints, record 22, Options > Water;
ice_effect; mirror 0.35 over 60 px: 0.6 tried, the user wanted 0.35 back),
committed 013e192. Then the mirror as a reflected ray (no chevrons beside
plants), its slope and sideways drift following the warp's camera (IceCam;
KOL_ICE_FIXED_VIEW=1 old), uncommitted. Snowflakes (tiles 0x166-0x168) kept
out of the cursor's hand and corner pieces (g_weather_piece + g_weather_px from
OAM in view mode; KOL_WEATHER_IN_HAND=1 old); props under the hand keep the
compositor's colour, no row fill (KOL_HAND_ROW_FILL=1 old), committed 442ed79.
Then the ice's base is a dark reflection of the sky's fade (KOL_ICE_SKY,
then lighter and blue: 1,1.1,0.35, tint 0.88/1.0/1.18; the sky by layer
code 5, not kSceneNone: brown bands gone; snow reflects as spots, the ray
passes through the weather; a vivid sky desaturated past 0.4, up to 0.75, map 12), committed c0e8a39.
Then the water reflects by the same ray (reflect_ray shared, camera passed,
calm_sky shared; KOL_WATER_COLUMN_MIRROR=1 old), uncommitted.
APK 14:59. (Stone shading model tried and dropped.) Invariants
66e13ab2 / 9054c456. Next state letter: JC (JB reused).

## HANDOFF — 2026-10-20, afternoon: map 34's rocks and trunks

**Committed before:** KoL 6b5192f (unit list). **UNCOMMITTED (KoL
kol_focus.cpp, README "Map 34: ice rocks at a cliff's edge, dead trunks"):**
IJ/IK = the user's slot 3. Record 46 (dead trunks) in kCutoutRecords; a
spire's BG1 below its strip joins its card in a V (KOL_SPIRE_FLAT_FRONT=1
old). An edge-ground rule was tried and DROPPED (it floated 12,10 and CY's
spire). The user's Thor drawn heights decide map 34's spires (1,18 = 12;
12,10 reset by the user). Then (IK, 12,10's lower left hidden): a spire's
card sorts 32 past its place when its front neighbours are at or under its
ground (SpriteBlock::sort_extra; KOL_SPIRE_CORNER_DEPTH=1 old). The V
also copies its own cell's wall BG0 onto the card, kept in the scene
(g_lift_copy / ScenePixels::lift_copy). Render reports with the Thor's
drawn_heights.cfg via KOL_TILE_EDIT_FILE as its own env argument.
The trunk's V hangs from the card's lowest claimed row (IM). Then the
trunk is cut from the user's own painting: kDrawnCutouts (map 34, record
46; shots/trunk_mask/, read at >= 30% magenta), BG1 lifted and BG0 copied,
in place of strip + V (KOL_NO_DRAWN_CUTOUTS=1 old). Its card stands at
its lowest taken row (depth 44 for the trunks; KOL_DRAWN_CORNER=1 old).
Front-corner spires' cards bend along the V (SpriteBlock::bend_sx, per
column depth in kol_perspective, a quad per pixel; KOL_SPIRE_FLAT_CARD=1
old). APK 15:20. The user
will paint the next mask square by square. Maps 26/108 trunks unchecked.
The focus dump needs KOL_EDGE_BLUR=1.
Invariants 66e13ab2 / 9054c456. Next state letter: IN.

## HANDOFF — 2026-10-20, afternoon: the unit list's done units (read this first)

**Committed before:** KoL 1ee2568 (blob top row). **UNCOMMITTED (KoL
kol_unit_list.cpp, README "The unit list on the second screen" > END,
corrected):** II = the user's slot 2 (Left x3: list order changes; more:
Alphonse loses END). Object +0x78 bit 1 is the sprite pool's reload bit,
not done; done = battle entry +0x0C. The list paints the END plate itself
(tiles 24-25 / 28-29 flipped). Invariants 66e13ab2 / 9054c456. Next state
letter: IJ. map_ext.cfg: never commit.

## HANDOFF — 2026-10-20, afternoon: the blob's top row under the fairy (read this first)

**Committed before:** KoL 88763d0 (tile-block ownership). **UNCOMMITTED
(KoL kol_focus.cpp, README "Units cast shadows" > "The blob's top row"):**
IH = the user's slot 3 (AO off): two dark lines under the fairy = the
game's blob oval's top row (feet-8, x feet-4..+3) left on the card, and its
lifted column. That row is now removed too. KOL_BLOB_NO_TOP=1 old.
Invariants 66e13ab2 / 9054c456. Next state letter: II.

## HANDOFF — 2026-10-20, afternoon: whose sprite is whose, by tile block (read this first)

**Committed before:** KoL 99090e3 (cards by the number's place).
**UNCOMMITTED (KoL kol_focus.cpp, README "Whose sprite is whose: the game's
tile blocks"):** IG = the user's slot 2 (A: the command menu stuck to the
unit above during its idle animation). Root cause of the whole CL/CH/FH/HY
family: the claim was geometric. Now a last pass: an OAM entry in a battle
unit's 16-tile block (its actor slot, object +0x08, pool [0x02026F08]+0x2020
stride 0x3C8; VRAM callback 0x08059000: slot i -> tiles i*0x10) is that
unit's, and a unit drawn from its block owns nothing else outside an
attack/spell camera. Scene actors/bags unchanged. KOL_NO_TILE_OWNERS=1 old.
26-state check + A/B in README. Invariants 66e13ab2 / 9054c456. Next state
letter: IH. map_ext.cfg: never commit.

## HANDOFF — 2026-10-20, afternoon: an area spell's cards in turn (read this first)

**UNCOMMITTED (KoL kol_focus.{h,cpp} damage_number_unit + result stride 16,
kol_unit_data.cpp, README "An area spell: each unit's card in turn"), on
top of the uncommitted arrow change below:** IF = the user's slot 3 (Crag
Crush on 4). The number's block +0x3C/+0x40 = the hit unit's world X/Z:
each number drains its own unit and becomes the lower card. Invariants
66e13ab2 / 9054c456. Next state letter: IG.

## HANDOFF — 2026-10-20, afternoon: an arrow's hit unit on the card (read this first)

**Committed before:** KoL 68679d2 (spell hit unit). **UNCOMMITTED (KoL
kol_unit_data.{h,cpp}, README "An arrow that hits the unit in the way"):**
IE = the user's slot 3 (Thompson's arrow at Alphonse hits Ivanna). The unit
an attack's number drains (not the attacker) becomes the lower card. Next
state letter: IF. map_ext.cfg: never commit.

## HANDOFF — 2026-10-20, afternoon: a spell's hit unit on the card (read this first)

**Committed before:** KoL 631b884 (spell bodies). **UNCOMMITTED (KoL:
kol_focus.{h,cpp} talent_results, kol_unit_data.{h,cpp}, README "A spell
that hits the unit in the way"):** ID = the user's slot 3 (Ice Javelin at
Arsnoba hits Elana; A to cast). The result list 0x02029EC0 names the units
actually taken; its 0x80 is NOT heal: heal/damage = talent record +0x1E low
nibble (1 damage, 2 heal). At the number the lower card switches to the
first result unit and it drains. KOL_TALENT_LIST=1. Thor slot 3 = ID (the
user's own). Invariants 66e13ab2 / 9054c456. Next state letter: IE.

## HANDOFF — 2026-10-20, early afternoon: units in a spell keep their place (read this first)

**Committed before:** KoL 88b4dc5 (heals, story events). **UNCOMMITTED
(KoL kol_focus.cpp only, README "Units in a spell keep their place"):** the
spell fade was built and DROPPED by the user; kept: during the spell camera
a 16x16/32x32 priority-0 entry fitting a unit's frame is that unit's
(`spell_body`; the game raises bodies to prio 0 for a spell's effect and
they were drawn flat, misplaced under the perspective). KOL_NO_SPELL_BODIES=1
old. Thor slot 3 = GD (Ice Javelin); the user's IC is backed up in the
session scratchpad thor_slots/ (and states/IC). map_ext.cfg: never commit.
Next state letter: ID.

## HANDOFF — 2026-10-20, midday: no cards in story events (read this first)

**UNCOMMITTED, on top of the heals below:** IC = the user's slot 3 (map
59 house, Eleanor, no battle): the sheet showed cards there. EWRAM
0x020013D0 == 1 = a story event (45/243 states, every scene + pre-battle
talk + NAVI-script preps; 0 in every battle turn): `kol::story_event()`
gates the sheet (except preparation and status windows) and the unit list.
README "No cards outside a battle". Next state letter: ID.

## HANDOFF — 2026-10-20, midday: heals on the sheet too

**Committed before:** KoL 8531ca1 (HP waits for the blow). **UNCOMMITTED
(KoL only: kol_focus.{h,cpp} talent_targets, kol_unit_data.{h,cpp},
kol_sheet.cpp, README "Heals too"):** IB = the user's slot 2 (Rictor heals
Lara). Talent block 0x02029D80 (+4 user obj, +0x20 count, +0x28 targets)
names the target at pick time; result 0x02029EC0 {count, obj, flags 0x80
heal, amount}; the game writes the healed HP AFTER the number, so the card
goes to the result's figure at the number. Green gained part. Invariants
66e13ab2 / 9054c456. map_ext.cfg: never commit. Next state letter: IC.

## HANDOFF — 2026-10-20, late morning: the HP waits for the blow (read this first)

**Committed before:** KoL c9b6e8b (fast walk, HZ, IA). **UNCOMMITTED (KoL
only: kol_unit_data.{h,cpp}, kol_sheet.cpp, kol_focus.{h,cpp}, README "The
HP waits for the blow, then drains"):** asked to see the target before it
loses its HP + an HP bar drop animation. The game writes the result
~2 s before the damage number; battle units' HP is now held on the cards
during an action until a number comes up (target, else actor, else all
waiting), then drains (white/red lost part, 15 + 30 frames). Death flash
after the drain. KOL_SHEET_PROBE prints thp/pairs/strike/dmg. Invariants
66e13ab2 / 9054c456. map_ext.cfg: never commit. Next state letter: IB.

## HANDOFF — 2026-10-20, morning: the sheet let go of Rictor (read this first)

**UNCOMMITTED (KoL only, on top of the uncommitted fast walk / HZ work
below), APK 09:36 built, not installed:** IA = the user's slot 2 (same file
as HZ, map 2 rain): the second screen held Rictor through the enemy's and
his own allied army's phases. `unit_in_action()`'s byte 0x0202B26F is the
top of a pointer (0x0202B26C, battle menus' context) the game never clears;
new `kol::ai_phase()` = 0x0202A903 & 0x80 (bit 31 of flags 0x0202A900,
found by phase memory dumps; 0x80 in all 8 enemy-turn states, 0 in 76
player's). In an AI phase the sheet shows the acting unit (any team) +
target; no acting unit = not in action. README "The sheet stayed on Rictor
through the AI's phases". Invariants 66e13ab2 / 9054c456. Open: HX reads
battle_preparation() true in its enemy phase. map_ext.cfg: never commit.
Next state letter: IB.

## HANDOFF — 2026-10-19, late night: fast walking, quick enemy turns (read this first)

**Committed before:** KoL 27ceba8 (map 2 fixes). **UNCOMMITTED (KoL only;
game.toml changed: regenerate `generated/` on a fresh tree):** Options >
Advanced > Fast Walking / Quick Enemy Turns (Enhanced on). README "Fast
walking and quick enemy turns": hooks on the phase switch's resume entries
0x0806B7A2 / 0x0806B7B2 repeat the enemy AI (x8) and the action executor
(walk x2 any team; enemy's range/cursor waits x4) in the same frame, clock
held; music unaffected. BL enemy phase 4039 -> 2552 frames. The RNG steps
once a frame, so battles diverge from the cartridge (as with Fast Actions).
Then (HZ, the damage number and raindrops were tied to the ground behind): the
combat camera's overlays are atlas cards now, not mesh pixels (kol_focus
overlay_card, SpriteBlock::overlay; KOL_PERSP_OVERLAY_MESH=1 old); README
under "A damage number over a lifted stone came apart". Both UNCOMMITTED.
map_ext.cfg: never commit. Next state letter: IA.

## HANDOFF — 2026-10-19, late night: the command menu stuck to Rictor (read this first)

**Committed before it:** Fast Actions (KoL b9b6410, gbarecomp 78d7858).
**UNCOMMITTED, KoL only (kol_focus.cpp run rule, README "The command menu
stuck to Rictor in the rain"):** HY = the user's slot 2, map 2 rain,
command menu over Rictor at 384x216: on 40 of 200 presents the menu's
icons OAM 5-6 went to Rictor (part join + run start; the run's cut test saw
only the gap after its first member). Now a leading GROUP cut off by
interface, whose unit comes again later, is released
(`KOL_RUN_GROUP_CUT=0` old). HY 0/200; ownership A/B over 28 sprite states;
invariants 66e13ab2 / 9054c456. Then (same state, asked for the stones on
this map to be anchored to the field, like the statue): stones,
record 50, stand where their art's lowest BG1 row meets the floor
(`kFootRecords`, README "The stones stand where their art meets the
floor"; `KOL_STONE_CORNER=1` old; stills shots/stone_foot/). Both
UNCOMMITTED, APK 22:27. Then HZ (slot 2, the damage number drawn broken up): a
damage number's 3 over a lifted stone under the combat camera; fixed in
kol_perspective (`moved_overlay`, README "A damage number over a lifted
stone came apart"; `KOL_PERSP_OVERLAY_LIFTED=1` old). All three
UNCOMMITTED. map_ext.cfg: never commit. Next state letter: IA.

## HANDOFF — 2026-10-19, night: Fast Actions, the battle's pauses gone (read this first)

**UNCOMMITTED, both repos, built, APK built (not installed, not seen on the
Thor):** README "The pauses on every battle action", part "Fast Actions".
HX = the user's slot 2 (every action slowed the game down): the game's
own 4-8 frame VBlank-handler holds (six per-cell routines; the enemy-turn one is the leaf 0x0809B5AC, not its every-frame caller). Options >
Advanced > Fast Actions (Enhanced on): gbarecomp `g_runtime_clock_frozen`
(runtime_tick counts cycles only; ENHANCEMENTS.md), KoL
`kol_fast_actions.cpp` holds the clock from a listed routine's entry to the
first function entry at or above its stack pointer; cap 60 frames. Verified:
HX/BL/GC/BV/S/GD no hold left, no I/O but IME, invariants 66e13ab2 /
9054c456. Desktop: `KOL_SETTINGS_ON=fast_actions`, `KOL_FAST_ACTIONS_LOG=1`,
`KOL_FAST_ACTIONS_CHECK=1`. Rumble committed earlier (KoL 6a72e8b,
gbarecomp 524875e). Open: scene-load holds of 14-17 frames (HT) are Fast
Loading's, not done. map_ext.cfg: never commit. Next state letter: HY.

## HANDOFF — 2026-10-19, night: the pauses on every battle action (read this first)

**Committed:** rumble (KoL 6a72e8b, gbarecomp 524875e). Nothing else
changed in code. HX = the user's slot 2 (map 2, rain, enemy phase).

**Finding (README "The pauses on every battle action"):** the
"slowdown" on actions is the game's own 4-8 frame holds (five routines
walking the 1024-cell table at 0x02010440), identical on the Mac and the
Thor, paced at 16.7 ms a frame; between them the game is at 60 fps with
headroom. Not today's pacing commit; unpacing them is the rejected
pre-09-29 behaviour; the memo would rarely hit. Only native
transcriptions (Fast Loading style, CHECK mode) would remove them: not
started, the user's call. map_ext.cfg: never commit. Next state letter: HY.

## HANDOFF — 2026-10-19, evening: archers whole, map 102's opening fixed (read this first)

**Committed (KoL + gbarecomp's present-dump log line), APK 16:41 (not seen on the Thor):**
- Archers' bodies vanished walking in (HP): the game's occlusion routine
  0x03005DE0 takes ground -255 off the grid -> lower half priority 3; a
  `[[mod_function_hook]]` re-calls it with X/Z on the grid's edge (map 98,
  walk-in on). README "Their bodies vanished on the last stretch off the
  grid". Memory kol-off-grid-actors.
- HW = slot 3, map 102's opening (README "Map 102's opening"): a lying
  body's frame (kFeetSlack 8); the thunder's BG3 white field (`g_ui_field`:
  continued over the whole view, not HUD, blended over the warp,
  `g_ui_field_drawn` because the hook's anchors are the next frame's at
  capture); letterbox plain rows on either UI layer; the light laid under
  the white fade and under a palette set to white (`under_white`,
  `scene_white`; KOL_NO_UNDER_WHITE=1 old).
Both invariants hold. map_ext.cfg: never commit. Next state letter: HX.

## HANDOFF — 2026-10-19, afternoon: all 32 songs HD (read this first)

**Committed (both repos), APK 13:25 (not installed):** every song's PSG
mapped by the user's ear (tools/hd_audio/kol_hd_map.json; README "HD
instruments" lists each pick). Bell sample = Tubular Bells (song 24 keeps the game's own); song
19's lead = English Horn, songs 8, 24 (the intro), 27, 28 (title), 29 have Timpani for the bass's drum-like low notes (song 28's opening roll fades in: intro_s) (per-song sample
rows: `HdSampleMap::song`, prog -1 = the game's own sample). gbarecomp:
clicks between notes fixed (`HdPsg::owned`), levels matched over 1 s of a
held note (`kLevelTo`), an HD-only limiter before the DAC clamp. All 32
songs from AN: 0 clamped samples, HD -1.3..+3.6 dB vs HQ (median +1.3), HQ
bit-identical. Desktop tools: `KOL_PLAY_SONG`, `KOL_MP2K_DUMP`,
`KOL_HD_PSG_MAP`, `KOL_HD_SAMPLE_MAP` (README "Any song, from any state").
Not heard on the Thor yet. map_ext.cfg: never commit. Next state letter: HU.

## HANDOFF — 2026-10-19, morning: the opening in HD, drums at 80% (read this first)

**Committed** (KoL only; gbarecomp unchanged; `map_ext.cfg` = the user's,
never commit). APK 08:42 built, not yet installed (Ctrl+Option+I).

- Thor: the user heard HD and liked it; its cost measured on GN,
  +0.5 M guest cycles a frame (~2%), fps unchanged (README "HD
  instruments", the "On the Thor" note). Open (the user): GN holds ~56.5
  fps, aim for ~60 later; neither thread is saturated, look at GPU and the
  worker hand-off first.
- Song 24 (the opening: boat, archers' town) HD: Flute / Oboe / String
  drone (pick C of six). The Orchestra Kit at 80% (-1.94 dB) in every song.
  Seven songs remain with states to record from: 1, 2, 3, 5, 7, 13, 22.
- Mock-ups now come from the game: `KOL_HD_PSG_MAP` + a trial bank +
  `GBARECOMP_AUDIO_RAW` (README "Mock-ups from the game itself"); the
  offline renderer drifts where the driver runs twice in a frame.
  Next state letter: HT.

## HANDOFF — 2026-10-19, 00:10: HD instruments, five songs (read this first)

**All committed** (gbarecomp + KoL, nothing pushed; `map_ext.cfg` = the
user's, never commit). APK 00:04 built, NOT installed (Ctrl+Option+I).

- Options > Sound = Original / HQ / HD (`music_quality=`). README "HD
  instruments (2026-10-18)", gbarecomp ENHANCEMENTS "HD instruments" and
  "Wave RAM after a state load". Mapping: tools/hd_audio/kol_hd_map.json ->
  `build_bank.py --atten-factor 1.0` (build/hd_instruments.bank, 29.5 MB,
  gitignored: rebuild before an APK on a fresh tree; get_soundfont.sh first)
  + `gen_map.py` (src/kol_hd_map.inc).
- Songs with every voice HD (user-picked by ear): 4 battle, 10 world map, 6,
  28 title (Horns / Strings), 9 battle prep (English Horn / Strings /
  Pizzicato). The other 27 songs: PCM samples HD, PSG still chip.
- Workflow the user likes: capture.py from a state -> hd.py mock-ups ->
  per-song folders in the scratchpad's listen/ (gone next session: the
  scripts are in memory kol-hd-instruments-mockup, rebuild them from
  tools/mockup/sound + the README) -> A/B file first -> pick -> map -> in-game
  check with GBARECOMP_AUDIO_RAW (bands vs the mock-up, clicks on an
  isolated channel). Record the HQ reference FROM THE GAME (states load with
  blank wave RAM; the restore fixed the game, not old captures).
- Fixed tonight, every mode: wave voice silent after any state load. HD:
  PSG note release tails (clicks the user heard on the English Horn).
- Also committed tonight: opt-in no-BIOS mode (gbarecomp f7e2f24, KoL
  c034179). Thor: neither HD nor no-BIOS tried on it yet; HD cost
  unmeasured. Next state letter: HT.

## HANDOFF — 2026-10-18, late night: HD instruments (Options > Sound = Original / HQ / HD)

**Committed (gbarecomp 696a539, KoL d2753a9 + the title's horns), APK 23:46 (not seen on the Thor):** README
"HD instruments (2026-10-18)"; gbarecomp ENHANCEMENTS "HD instruments" and
"Wave RAM after a state load". gbarecomp: Mp2kShadow::Mode::Hd (mp2k_hd.cpp),
hd_instruments.{h,cpp}, mixer PSG replacement, wave RAM restore after a load
(all modes: the battle's bass was silent after every slot load),
GBARECOMP_AUDIO_RAW. KoL: tools/hd_audio/ (kol_hd_map.json -> bank +
src/kol_hd_map.inc), src/kol_hd_audio.cpp, `music_quality=`, Gradle packs
build/hd_instruments.bank (27 MB, gitignored: rebuild with build_bank.py
--atten-factor 1.0). PSG mapped for songs 4, 10, 6, 28 (title: Horns +
Strings), 9 (battle prep: English Horn / Strings / Pizzicato). User-approved: Orchestra Kit + snare roll, Oboe on the world map. Thor cost unmeasured.
Desktop: KOL_MUSIC_QUALITY=2. The no-BIOS mode below is committed too
(gbarecomp f7e2f24, KoL c034179). map_ext.cfg: never commit. Next state letter: HT.

## HANDOFF — 2026-10-18, night: playing without a BIOS (opt-in)

**UNCOMMITTED, in BOTH repos, APK 21:56 (not seen on the Thor):** README
"Playing without a BIOS (opt-in, experimental, 2026-10-18)"; gbarecomp
ENHANCEMENTS.md "Opt-in: no BIOS at all". The user kept the BIOS-is-sacred rule:
the real BIOS stays default and oracle; `--no-bios` (BiosHleMode::NoBios,
g_bios_region_hook, IRQ stand-ins at 0x18/0x138, open-bus constants) is
for players without a dump. Android: SetupActivity checkbox + warning,
`files/no_bios` marker -> main_android passes --no-bios. kol_fast_load
chains to an installed HLE hook. no-bios == bios-hle byte-identical;
HLE vs real BIOS: same pictures, timing differs (FN loads ~17 frames
sooner). Both invariants hold. Open: the binary still links
bios_recompiled.cpp; no desktop UI; the Android box cannot be unticked
once the game auto-launches. map_ext.cfg: never commit. Next state
letter: HT.

## HANDOFF — 2026-10-18, 00:48: godrays fade with the scene (read this first)

**Committed aaa7c98:** map 101 castle room (dark 0.2, godrays through the
+X windows, chandelier as a ring of flames r 40, arcade model from the
user's 480 painting, model_occluder light). README "The intro's castle
room" and its sub-entries.

**UNCOMMITTED, built, APK 00:48 (not seen on the Thor):** the godrays did
not fade out after the scene -> README "The godrays fade with the scene".
The haze is ADDED to the art (window_light), so it stayed lit while the
room faded; the sunlit patch and the lamps multiply and fade by
themselves. ScenePixels::scene_bright (kol_focus, next to fx_dark):
BLDY darkening of BG0+BG1 (the room's fade out: BLDCNT 00f3, BLDY 1 -> 16
over frames 12020-12070 absolute) times the palette fade against
0x0202D90C where that one is the live palette darkened (nine entries in
ten; it is NOT on map 101 or battle maps). window_light's haze x
scene_bright. KOL_FADE_LOG=1 (kol_widescreen): palette sums + BLDCNT/BLDY
every 10 frames, headless and fast: the way to see how a scene fades.
Both invariants hold.

**Also UNCOMMITTED, in BOTH repos, APK 21:16:** no Game Boy logo at start on
Android: gbarecomp `--skip-bios-intro` (runtime.cpp, ENHANCEMENTS.md), passed
by main_android.cpp. README "No Game Boy logo at start". Thor slot 1 = HB
cave state (the user's old slot 1 backed up in the session scratchpad).

**Dropped by the user:** the table's drawn shadow -> dynamic shadow (kept as
it is); nothing of it is in the tree. The user also stopped a
3000-frame present dump: use KOL_FADE_LOG-style logs, not picture dumps,
to find a moment in a scene. Next (asked earlier): the statues (lion,
eagle) and pillar depth. Thor sun lines for maps 13/58/97/98 still to
bake ("later"). Next state letter: HT.

## HANDOFF — 2026-10-17, late night: the castle room dark, godrays, arcade modelled (read this first)

**UNCOMMITTED, built, APK built (not seen on the Thor):** README "The intro's
castle room, dark, lit through its windows (map 101)". HS = the user's slot
2. kol_focus: MapWindow::on_x (+X-facing windows), map 101 indoor + dark
0.2 + sun (40, -0.9, -0.35) + two windows + a chandelier (kCeilingLamps,
user's pick A: warm, 220, 1.0). Then the arcade MODELLED from the user's
painting (shots/castle_room/HS_drawn.png): tools/models/room101*.py ->
kol_model_map101.inc; model_occluder (4-unit cubes) stops lamp + window
rays, cells under its floor low for light, light worked out after the model
pass on map 101 only (lamp_and_window_light), kTerrainNearer 1e9 on 101,
windows moved to X 32. README "The arcade, modelled". Then the chandelier as a ring
of flames (soft shadows, lamp_seen, ring 40 = user's pick C), APK 00:07. Then the top band at 480 repainted by the user
(shots/castle_room/HS_drawn_480.png, extract offset 48 36), faces clamped
to the walls' top; APK 00:27. Next (asked): the
statues (lion, eagle) and further pillar depth. Fire/sunset/night committed cdd6b02. Next state
letter: HT.

## HANDOFF — 2026-10-17, late night: fire light; the burning town at night

**UNCOMMITTED, built, APK 22:25 (not seen on the Thor), with the sunset
below:** README "Fire light, and the burning town at night". kol_focus
fire_lights (flames 1.1/72, map 98 arrows 0x69-0x6D 0.9/48 + 10 px halo),
kIndoorMaps + kDarkMaps {99, 0.2}; kol_perspective fire_light_pools feeding
the darkness, the backdrop darkened, flame cards not. The user stops cost
runs during visual work (memory). Then the fire map was reported slowing the
game: 14.8 -> 9.5 ms Mac worker (effects per corner + 30 Hz cache, pools
boxed/cached, card light per card, no lifted walls/other props on map 99 only).
APK built after. Next state letter: HS.

## HANDOFF — 2026-10-17, night: the archers' town at sunset

**UNCOMMITTED, built, APK 22:06 (not seen on the Thor):** README "The
archers' town at sunset (map 98)". kol_focus kMapSunsets {98, 1.3, 1.0} +
kEdgeFadeMaps {98, 0, dusk}; kol_perspective sunset_grade (scene by ao,
cards by shade). User picked mockup 3. The Thor's drawn_heights.cfg has new
`sun` lines (13, 58, 97, 98) NOT baked into the build (desktop renders used
KOL_TILE_EDIT_FILE with a pulled copy). Special Effects below committed
(KoL b6d6a96, gbarecomp 936d122). Next state letter: HS.

## HANDOFF — 2026-10-17, night: Options > Special Effects

**Committed (KoL b6d6a96, gbarecomp 936d122):**
README "Options > Special Effects: fire and smoke drawn anew". Setting
SpecialEffects (off), Advanced row; kol_focus effect squares
(SpriteBlock::effect/fx_cx/fx_top/fx_seed); kol_perspective effect_texel
(fire tongues + ragged base, smoke plume), per-corner alpha; gbarecomp SDL
emulation per-corner MOD. Then: fades by the scene's full palette
(0x0202D90C), one prop per flame (prop cell -2 - index), effects anchored to
their tiles, smoke drift = upward turbulence only. APK 21:58. Worker cost
not measured (the user stopped that run). Hole depth below committed 9cd7f5c. Next state letter: HS.

## HANDOFF — 2026-10-17, night: deeper holes in the burnt houses

**Committed 9cd7f5c:** README "Deeper
holes in the burnt houses (map 100)". kol_focus hole_depth (kHoleMaps {100},
two dark colours + blob test, rim shadow and deeper middle at 0.75 on a
copy of the occlusion, cached by the mask). User picked D, stronger, from
the lit mockups. KOL_NO_HOLE_DEPTH=1 off. Smoke below committed (KoL
65e229f, gbarecomp d5b8c77). Next state letter: HS.

## HANDOFF — 2026-10-17, night: the smoke stands up

**Committed (KoL 65e229f, gbarecomp d5b8c77):**
README "The smoke stands too (map 100)" (in the flames' section; HR = a
state saved inside map 100). KoL kol_focus flame_cards column mode
(kSmokeMaps), smoke blocks see-through (SpriteBlock::see_through =
1 - EVB/16), kol_perspective premultiplied colour + mesh->alpha. gbarecomp
PresentWarpMesh::alpha (premultiplied; GL alpha program, SDL MOD+ADD runs;
ENHANCEMENTS.md §6 note). First APK: scene all black on the Thor (alpha
attribute on GLES2's position slot 6); now slot 5, APK 21:19, not yet seen.
Scouts below committed 6bf81cb. Next state letter: HS.

## HANDOFF — 2026-10-17, night: the smoke scene's scouts walk in

**Committed 6bf81cb:** README "The
town's archers walk in from beyond the edge", part "The smoke scene's two
scouts (map 100)". kol_gulls kWalkIns now per map: map 100's 0x75/0x76
(show ops 0x538/0x551) step out of a doorway (5,16): one cell back under
the roof, glide 1 px/frame, faded in by OUR card fade (Unit::fade,
SpriteBlock::fade, gulls_unit_fade from the capture), not the game's
blend (BLDALPHA is the smoke's). The first build (from 2,16) walked them
over the roof: rejected by the user. Flat view: shown at the doorway.
Scene 32 frames longer. Camera + sky
fixes below committed 6ae8472. Next state letter: HR.

## HANDOFF — 2026-10-17, night: the fire scene's sky flicker

**Committed 6ae8472 with the smoke-scene camera fix below:** README "And the camera a tile on" (in the
map_source grace section; HQ map 99 pan). kol_map_source: a frame whose
scroll stepped a tile (same ring) serves at >= 50% agreement, 2 frames max
(it was 83-85% for one frame, so the margins fell to the sky).
KOL_MAP_SOURCE_NO_STEP_GRACE=1 old. AS/BR/GH/HN byte-identical. Next state
letter: HR.

## HANDOFF — 2026-10-17, night: the smoke scene's light pops in late

**UNCOMMITTED, built, APK 19:18 (not seen on the Thor):** README "The
town's shadows broke after the boat", note "Then the smoke scene after
the burning town" (HQ, ~frame 240 on). kol_focus: with no unit/cursor
vote, the camera is unwrapped near the game's own camera once it has
rested 3 frames, not near the last (the load's placeholder 640,624 put it
512/768 off). KOL_FOCUS_LAST_NEAR=1 old. GG/GH/HN byte-identical. Flame
fix below committed 31c4c09. Next state letter: HR.

## HANDOFF — 2026-10-17, evening: the faint square in the burning town

**Committed 31c4c09, APK built:** README "The town's
flames stand up", last note "The faint square in the burning town" (HQ =
the user's slot 2, map 98 -> 99). kol_focus flame_cards over the whole
scene, not only the native 240x160 (KOL_FLAME_NATIVE_ONLY=1 old). Found
by averaging each frame's high-pass over the user's Thor mp4 (Swift
AVAssetReader frames). Archers' walk-in below committed 1439011. Next
state letter: HR.

## HANDOFF — 2026-10-17, evening: the town's archers walk in

**Committed 1439011, APK built (not seen on the Thor):** README "The
town's archers walk in from beyond the edge" (HP = the user's slot 2, end
of the boat fading into the town, map 98). kol_gulls step hook: at the
four show ops (0x2CA/0x2DA/0x2EA/0x2F4) the thread runs our op-10 glide
from EWRAM 0x0203F000 (10 cells west at 384, 13 at 480), then back to the
game's walk; no game blob off the grid. Trap: the glide's X is unsigned
(never glide across 0). The user will extend map 98 west in the map
editor later. KOL_NO_ARCHER_WALK_IN=1 / KOL_ARCHER_WALK_CELLS=n. Both
invariants. Next state letter: HQ. map_ext.cfg: never commit.

## HANDOFF — 2026-10-17, later: the town's shadows after the boat

**Committed (on top of 6404d5b), APK built (not seen on the Thor):** README "The
town's shadows broke after the boat" (HN = the user's slot 2, boat ->
town map 98; HO = saved inside the town, frame 9400). kol_focus camera
votes: stale sprite slots (a map-97 flag's, three of them) voted the
boat's camera in the town; a slot now votes only for a live object near
its place (KOL_FOCUS_STALE_SLOTS=1 old); the game-camera fallback votes
only at rest (KOL_FOCUS_GAME_VOTE_ANY=1 old). A/B vs 6404d5b: identical on
22 states, HJ 34 px (2 frames after load, kx 135 vs 136). Both invariants.
Next state letter: HP.

## HANDOFF — 2026-10-17, afternoon: the boat's flag and door

**Committed 68ad4c7 (map 58 ship: bb73f6d), APK built (not seen on the Thor), on top of the map
58 ship below:** README "The boat's flag and door on map 58" (HM = the
user's slot 2). kol_focus: part join by OAM stretch + kTallCard 48 + until
nothing joins (door twitched when a raindrop joined it); units up to 48
tall are two stacked cards (`upper`); hanging units (kHangRise 96): no
contact shade, lifted background at the ground's place, cards anchored at
the depth of the model pixels over their top (KOL_NO_HANG_ANCHOR=1).
kol_perspective: yref held within 0.5 of its target (KOL_YREF_HOLD=0). A/B
vs the build before: identical on 21 sprite states, only GY/HJ/HM differ.
Next state letter: HN.

## HANDOFF — 2026-10-17, midday: map 58's ship

**UNCOMMITTED, built, APK built (not seen on the Thor):** map 97's ship
model on map 58 (README "The same ship on map 58"): tools/models/ship58.py
(ship97.build(quay=False) moved X -416, Y +32, Z -256, its own plank),
src/kol_model_map58.inc (new, generated), kol_focus kModels + the NOGAP
flag (kModelNoGap: map 58's plank only). Map 97 byte-identical (table and
lit render); both invariants hold. Map 58 exported for the map editor
(tools/map_editor/maps/map58.json, gitignored). No state is on map 58: use
the viewer (EV + KOL_DEBUG_MAP=58, cell 0,2 shows the ship). Next state
letter: HM. map_ext.cfg: never commit.

## HANDOFF — 2026-10-17, 00:45: the boat scene's actors (read this first)

**Committed today (KoL, nothing pushed):** 65a9f8c gulls + map 98/99 flame
cards; ce9bcfc the quay's two people and the flags made before the gulls
(scene-script step hook 0x0811A248; flag 0x112 trap); 7491044 the street
soldier walks to 5,0 and fades out (blob callback hook 0x080598DC; semi
units' shadows scale with EVA). README sections under "The boat in the
intro".

**Then committed:** Alphonse waits hidden at 5,11 and walks down the
stairs once the camera's pan reaches Y 805 (README "Alphonse walks down
from further back"); enters at present ~1270. The user chose to keep this
timing over a fade-in or a 7,11/8,11 start (the user liked it). game.toml
unchanged since 7491044.

**States:** HL = the user's Thor slot 1 just before the boat scene (scene
starts ~470 frames after load; a 600-frame run ends in the narration, not
a hang). HJ = mid-scene (all hooks must leave it the game's way). Next
state letter: HM. map_ext.cfg: never commit (the user's editor file).

**Tools:** scratchpad dis.sh (THUMB disassembly with literals), the scene
script decoding (README "The two people on the quay"); rend.sh/rend480.sh
in the session scratchpad (gone next session: recreate from memory
kol-scene-actor-mover).

## HANDOFF — 2026-10-15 night: the worker cut; the castle in the dark

Committed: 1a586cf (no range mask without a range; the zip rescans only the
edge that moved), 81bb512 (window light skips unlit pixels; lifting keeps
its buffers), both byte-identical, README "The worker on the Thor: what
each option costs, and two cuts" (the per-option cost table, the yref
finding: the mesh cannot be reused on a still camera while `yref` eases;
settling it is the user's call, offered). Then committed with this
handoff: map 55 dark (README "The castle in the dark"): kDarkMaps 0.35,
the statue's flame a pot 32 up, godrays indoors with a radiated glow (C).
APK 00:30 installed; EM loaded on the Thor; Thor cost and the once-a-turn
beam rebuild (~28 ms Mac) not measured. Next state letter: GU.

## HANDOFF — 2026-10-14 night: the map atlas, indoor tags on the Thor

Committed 05223fe (bumps softened). Then committed: `KOL_VIEWER_CELL`
(x,z or center) for the New Game viewer, and the map atlas in
`shots/map_atlas/` (README "Every map at a glance: the map atlas"). On the
Thor's drawn_heights.cfg: map 13's heights/roofs/boxes copied to map 14,
and 23 `indoor` lines from the atlas tags (not baked). Next state letter: GU.

## HANDOFF — 2026-10-14: bumps softened; map 89's sun bump tried and dropped

Committed 7099bd7 (Bump Mapping switch). **UNCOMMITTED, built, APK built:**
the sun's bump code (tried on map 89, judged to add nothing: on no map now,
KOL_BUMP_SUN=<s> for mockups; README "Map 89 in the sun", "Off on map 89"). Bumps softened 1 px (KOL_BUMP_BLUR,
KOL_BUMP_DEPTH 4+4/px; glints sharp). Next state letter: GU.

## HANDOFF — 2026-10-14: map 51's wet floor (bump map + slime glints)

Committed 0decb50 (sprite order, unit card light). **UNCOMMITTED, built, APK
built:** the wet floor in kol_perspective apply_lighting, on dark maps:
stone bump 0.5 + slime glints, cached per view (README "Map 51's wet
floor"). KOL_SLIME=0 / KOL_SLIME_BUMP=0 off. Also: no game blob under units
on dark maps (units_cast_from_lamps; GT = slot 2, map 51). Next state letter: GU.

## HANDOFF — 2026-10-14: Allos bright in the dark (sprite order)

Committed 5888f96 (torchlight perf). **UNCOMMITTED, built, APK built:**
`draw_depth` in kol_focus.cpp: KoL draws unit sprites by X + Z, the higher
first when level (not X + Z + Y); a unit up a step behind another went to
interface and skipped all lighting (README "Allos bright in the dark").
`KOL_DEPTH_XYZ=1` = old key. GS = slot 3 (map 51, Allos). Next state letter: GT.
Also uncommitted: units in part shadow from the pots (`unit_card_lamp_light`,
ScenePixels::unit_lamp; README "Units in part shadow from the pots");
`KOL_UNIT_LAMP_FEET=1` = old.

## HANDOFF — 2026-10-14: range tiles spared by the shading

Committed 4ad5442 (pot shadows, flicker, lean). **UNCOMMITTED, built, APK
built:** range tiles' own pixels (8x8 ink masks) keep 0.4 of the shading (README "The range's tiles
spared by the shading"; kol_highlight mask -> kol_focus range_mask ->
kol_perspective range_keep). Next state letter: GS.

## HANDOFF — 2026-10-14: units cast shadows from the pots (dark maps)

Committed 5b23f8a (lifted walls lamp light). **UNCOMMITTED, built, APK
built:** card_shadow refactor (sun byte-identical), cast_lamp_shadows,
ScenePixels::lamp_shade (README "Map 51 lit by its blue fire pots", last
notes); pot flicker 0.08, lean 0.06 (lamp_lean), pool shift 1 px sub-pixel, lean speed 0.35 (lamp_sample). Props cast none from
lamps yet. Next state letter: GS.

## HANDOFF — 2026-10-14: lifted walls in the dark took one light a square

Committed 980ea16 (map 51 blue pots). **UNCOMMITTED, built, APK built:**
lifted terrain takes its own pixel's lamp light and tint on dark maps
(README "Map 51 lit by its blue fire pots", the GR note; GR = slot 3).
Next: unit shadows from lamps (not started). Next state letter: GS.

## HANDOFF — 2026-10-14: map 51 lit by blue fire pots

Committed c888ba2 (pots/stalagmites grounded, map_ext). **UNCOMMITTED,
built, APK built:** fire pots are blue lamps; kDarkMaps {51, 0.3, 3, 1.6}
darkness for indoor maps, 51 in kIndoorMaps (README "Map 51 lit by its blue
fire pots"). Next: units casting shadows from lamps (the user asked; not
started). Next state letter: GR.

## HANDOFF — 2026-10-14: fire pot carried the urn (map 51)

Committed f8b6bd9 (indoor maps). **UNCOMMITTED, built, APK built:** a fire
pot's strip starts 3 steps over its ground, and pots and stalagmites (54)
stand at the cell's middle (kMiddleRecords) (README "A fire pot carried the
urn beside it"; GQ = slot 3, map 51). Map 51 exported for the editor (the
user fixes the 3,4 pot's BG0 bowl with Override). map_ext.cfg still
uncommitted. Next state letter: GR.

## HANDOFF — 2026-10-14: indoor maps (SUN tab INDOOR)

Committed 0b73757 (house/bush fixes). **UNCOMMITTED, built, APK built:**
per-map indoor flag: SUN tab third button, `indoor map` lines,
kol_focus kIndoorMaps (empty); indoors no sun shadows / evening / weather
look (README "Indoors, a map with no sun"). Not tried on the Thor; no map
set yet. map_ext.cfg still uncommitted. Next state letter: GQ.

## HANDOFF — 2026-10-14: the house's gable swayed with the bushes

Committed 0c4868e (grey-sky weather, low sun). **UNCOMMITTED, built, APK
built:** a prop's plant flag from the cell that claimed it, props grouped
by place and cell, record 78 (map 6's house) never a prop, building colours skip every
cut-out strip (README
"The house's gable swayed with the bushes"; GP = slot 3). map_ext.cfg (the user's overrides, incl. their 66,2 tweak) still
uncommitted. Next state letter: GQ.

## HANDOFF — 2026-10-14: weather look only under a grey sky

Committed 1274c50 (soft card shade). **UNCOMMITTED, built, APK built:**
weather shadows/evening only while the game's palette buffer backdrop
(0x02002400) is grey (README "Only under a grey sky"; GN = slot 2 overcast,
GO = slot 3 blue-sky rain); and the low sun's shadows lifted to 0.28 at 5
degrees (README "Lifted at the low end"). map_ext.cfg still uncommitted. Next state
letter: GP.

## HANDOFF — 2026-10-14: softer shadow line on units and props

Committed 31d612d (overlapping cut-outs). **UNCOMMITTED, built, APK built:**
card shade eased over 4 rows (README "A softer shadow line on units and
props"; KOL_CARD_SHADE_SOFT). map_ext.cfg (the user's map 6 overrides)
still uncommitted. Next state letter: GN.

## HANDOFF — 2026-10-14: two overlapping bushes split by nearest centre

Override mode committed (gbarecomp 6ddcae2, KoL 785902c). **UNCOMMITTED,
built, APK built:** overlapping cut-out strips give a pixel to the nearer
art centre (README "Two bushes, one behind the other"; state GM = slot 3,
map 6). map_ext.cfg holds the user's 10 overrides for map 6 (not
committed). Next state letter: GN.

## HANDOFF — 2026-10-14: map editor Override mode

Committed 4aeba73 (bush/house colour split). **UNCOMMITTED, both repos,
built, APK built:** the editor paints over the game's art (`ov` lines in
map_ext.cfg; gbarecomp `g_ws_tilemap_override_provider`); README "Override:
repainting the game's own art". tools/map_editor/maps/map6.json exported
from GL. The user paints map 6's bush. Next state letter: GM.

## HANDOFF — 2026-10-14: a house's corner on a bush's card

Plant sway committed 360b3e0. **UNCOMMITTED, built, APK built:** a cut-out
beside a taller BG1 building splits the strip by colour (README "A house's
corner on a bush's card"; state GL = slot 3, map 6). Not seen on the Thor.
Next state letter: GM.

## HANDOFF — 2026-10-14: plants sway with the wind

Prop foot shade committed earlier today. **UNCOMMITTED, built, APK built:**
Options > Advanced > Plant Sway (`plant_sway`, on, Enhanced), 2 px, lifted
trees and bushes only (README "Plants sway with the wind"). Previews in
shots/prop_sway/. Not seen on the Thor. Next state letter: GL.

## HANDOFF — 2026-10-14: props shade their own foot

Committed cc968af (prop terrain shade, rocks at the middle). **UNCOMMITTED,
built, APK built:** props darken toward their foot, full up to 4 rows over
it (asked a bit higher), then fading over 10 (`prop_base_light`, with AO;
README "Props cast shadows", the foot note), and on the ground around the
foot as a round soft spot; prop cast shadows blur by at least 6 texels
(hard edges reported from the Thor). Stills in shots/prop_base/. Next state letter: GL.

## HANDOFF — 2026-10-13 late: props take the terrain's shadow; rocks stand

Committed 44c865f (props cast shadows). **UNCOMMITTED, built, APK built:**
props take the terrain's shadow like units (`card_shade_row`; unlifted via
g_ao, lifted via block shade_row; KOL_NO_PROP_SHADE=1; README "Props cast
shadows"); rocks (record 51) stand at the cell's middle (README statues
section; state GK = slot 3). Next state letter: GL.

## HANDOFF — 2026-10-13 late: props cast shadows, their contact AO gone

Committed d417621 (bushes at the cell middle). **UNCOMMITTED, built, APK
built:** prop_contact_ao removed; props cast sun shadows like units
(`cast_prop_shadows`, README "Props cast shadows"; KOL_NO_PROP_SHADOWS=1).
Next state letter: GK.

## HANDOFF — 2026-10-13 late: bushes stand on the ground

Committed 315b24c (church model removed). **UNCOMMITTED, built, APK built:**
record 42 (bushes) joins kMiddleRecords; the statues' outline body is its own
list kStatueRecords (README "Map 54's statues stand", later note). State GJ =
slot 3 (map 3). Next state letter: GK.

## HANDOFF — 2026-10-13 late: the church's model removed

Committed bf5b14f (view-mode stick, votes at rest). **UNCOMMITTED, built, APK
built:** the church's model is removed from kol_focus with
`KOL_PERSP_NO_BUILDINGS` (cell owners/faces always built); the user draws it
with the roof/box tools (map 3, state GI = slot 3). Bake their
drawn_heights.cfg when they say they are done. Next state letter: GJ.

## HANDOFF — 2026-10-13 later: the stick in view mode, votes at rest

Committed ef524f8 (prep camera). **UNCOMMITTED, built, APK built (not seen on
the Thor):** the stick works in preparation's view mode (+0x51 is view mode,
not a gate; README "The left stick moves the hand"), and kol_focus takes
camera votes only while the game's camera rests (README "Votes only while the
camera rests"; a long stick pan streaked the map's edge). State GH = slot 3.
Next state letter: GI.

## HANDOFF — 2026-10-13 late: prep with no voter gets its camera

Committed: world-map stick bfc8721, map_ext.cfg b4b8cde, list touch fix
c513d7b. **UNCOMMITTED, built, APK 22:57 (not seen on the Thor):** kol_focus
falls back to the game's own camera (`game_camera`, README "No voter at all:
the game's own camera") when no unit and no cursor vote: battle preparation
before anyone is placed had no scene, so no perspective/shadows/AO (state GG =
the user's slot 3). Next state letter: GH.

## HANDOFF — 2026-10-13 night: the left stick on the world map

**Committed** (the user asked to commit): gbarecomp `e336967` (left stick),
KoL `bd1588a` (map 97's ship), `649929f` (stick cursor, battle map).
**Then UNCOMMITTED, built, APK built (not seen on the Thor):** the stick moves
the world map's sword too; let go, the game's own pull takes it onto a town
within 24 px (README "On the world map too"). `src/kol_stick_cursor.cpp`
only (a fourth hook on the roaming task, no game.toml change). Trap: the
TCP fingerprint ring (GBARECOMP_INSN_TRACE + fp_save) returned 0 records with
this build; not investigated. Next state letter: GG.

## HANDOFF — 2026-10-13 evening: the left stick moves the hand

**UNCOMMITTED, built, APK 21:18 (not seen on the Thor), BOTH repos:**
README "The left stick moves the hand". gbarecomp: `g_host_pad_left_stick`
(runtime.h, host_window.cpp). KoL: `src/kol_stick_cursor.{h,cpp}`, three
`[[mod_function_hook]]`s (cursor task 0x08056E90; the hand's 0x08002394 and
the corner pieces' 0x0807696C, re-entered with the moved point), Options >
Analog Cursor (`analog_cursor`, on), kol_focus cursor readers exact between
cells. `game.toml` changed: regenerate `generated/`. Both invariants hold;
idle stick byte-identical; FULLY_STATIC. Not tried on the Thor's stick.
The ship work below is still uncommitted too. Next state letter: GG.

## HANDOFF — 2026-10-13 midday: the ship's masking corrected

**UNCOMMITTED, built, APK built (not seen on the Thor):** the user showed
the mask of the ship model (not perfect yet). Fixed by measurement
(README "The masking, corrected"): the yard as the user drew it (one
level spar along Z at 223, row 138 - x/2; my two "measured" yards were
ropes) with four swags, a flat transom whose top is the art's row 52 with
a row-by-row stern profile, a rounded bow on the user's curve (measured
on the UNLIT frame: the lit one's shadow on the water read as hull;
closing from X 630 at 0.65, nose radius 40, stem 726, forward rake below
the rail), the waterline at -5; in kol_focus a pixel is now the nearest part's on
either layer (a BG1 pixel nothing takes: the rigging), and a part's BG1
flag only means its BG1 pixels are lifted (the hull's are not: lifting
them tore the post off under the stick). Then from the Thor: a dark band
along the far rail and a dark bow front (README "The far bulwark's dark
band"): the kit's strip normals were inverted (fixed); a model point
facing away from the sun is now left as drawn (the art drew its lee), one
facing the sun takes the model's shadows; the terrain's shadow map is not
consulted for model points (the grid's quay cells touch the deck's where
the art has water, so the quay shaded the ship's far side).
`KOL_FOCUS_DUMP` now also writes `.ao` and a `sun` line. `shots/ship97/GF_mask.png` (the mask) and
`GF_model_v2.png` (old model vs new, five views). Both invariants hold.
Method and traps in memory `kol-map-models`. Next state letter: GG.

## HANDOFF — 2026-10-13: map 97's ship as a 3D model

**UNCOMMITTED, built, APK built (not seen on the Thor):** README "The ship on
map 97, a 3D model" (state GF = the user's slot 3). `tools/models/`
(model.py, ship97.py -> src/kol_model_map97.inc), kol_focus `kModels` /
`ModelView` / `ModelShadow`, map 97's 31-cell wall in `kDrawnCells`. The
user's map-97 height edits were removed from the Thor's drawn_heights.cfg
(backup in the session scratchpad) and from kol_drawn_baked.inc. GE
committed 72755bf. Next state letter: GG.

## HANDOFF — 2026-10-13: the angels flashed with the menu's hand

**Committed, APK 00:02 (not seen on the Thor):** claim_unit_sprites span
fixes (README "The angels flashed with the menu's hand", state GE). Spells
committed 33985d0. Next state letter: GF.

## HANDOFF — 2026-10-12 night: spells keep the wide view and perspective

**UNCOMMITTED, built, APK 23:26 (not seen on the Thor):** `is_battle_map` accepts
mode 1 and an off BG2 left 512 wide, sticky from the battle map (README
"A spell's effect keeps them too", state GD). Enemy-turn work committed
773814e.

## HANDOFF — 2026-10-12 midday: the enemy's turn shows its target

**UNCOMMITTED, built, APK 23:12 (not seen on the Thor):** the list no longer flashes on the enemy's turn (README "The list no longer flashes up"); targets by cell too (GC, the user's slot 2), the target kept for the enemy's whole action; enemy phase = acting
enemy on top, the unit its attack/spell names below (`acting_target`,
kol_focus.cpp; README "The enemy's turn: the enemy, and the unit it
attacks"). Area techniques show the enemy alone (no unit target found).

## HANDOFF — 2026-10-12 late morning: the sheet lets go of the last unit

**Committed 9ef5519, APK 10:38 (not seen on the Thor):** no unit on the
second screen off the battle map without a status window, the acting enemy
on the enemy's turn, nothing between turns; Java takes a null sheet (README
"The sheet lets go of the last unit"). Banner fix committed e455dc2.

## HANDOFF — 2026-10-12 morning: the BATTLE START banner stays wide and in perspective

**UNCOMMITTED, built and verified, APK 10:06 (not seen on the Thor):** the
banner after "Start battle? Yes" dropped the view to 240x160 and flat for
~165 frames (DISPCNT `1B40`, then `9F40` with an OBJ window gating only BG2).
`is_battle_map` now takes WINOUT and the previous verdict (README "Scene
policy", "The BATTLE START banner keeps the wide view"). Repros: GA (slot 3,
a map with dialogue first, no banner in 700 frames), GB (slot 2, A at 2360).
Invariants hold (66e13ab2 / 9054c456, centre 0/160). Next state letter: GC.

## HANDOFF — 2026-10-12: the sun sinks a degree a turn (read this first)

**Committed 09de4ca** (evening light, the sun 60 / -2.5 a turn, shadows by
height and weather). **Then UNCOMMITTED, built, APK built, not seen on the
Thor:** the water's waves and foam phased on world X/Z, not on screen pixels
(they swam when the camera moved), the foam a soft 3-px band (README "The
waves ride on the water"). Trap: the scene buffer is screen space (kx follows
the scroll); anything animated on it must be phased on the world.

**Later, UNCOMMITTED, built and verified, APK built (not seen on the
Thor):** evening light at strength 1.0 (the user's pick); the sun 60 on turn
1, -2.5 a turn; shadow strength by the sun's height (0.5 at 36); x0.35 in
rain or snow (weather particles, latched 240 frames). README "Higher at
first, sinking faster; shadows by the sun's height and the weather; evening
light". Stills in shots/evening, shots/sun_height, shots/weather_shadows.
The evening tint is x0.3 in rain or snow (same latch), and starts after turn 6 on an ease-in curve to full at 21.

## HANDOFF — 2026-10-10: water effect, WATER tags, darker shadows

**State.** KoL `widescreen-16-9`: water committed as `8c7e527` (the user asked
to commit), nothing pushed; gbarecomp untouched. `map_ext.cfg` is modified by
the parallel map-editor session: never commit it from here.

**Done and committed** (README "Water: ripples, shore foam, reflections"):
Options > Advanced > Water (on, Enhanced): ripple, reflection 0.4 (user's
pick, mockup C in `shots/water/`), shore foam; water = terrain class 2
(records 10-12); second-screen editor WATER tab tags cells water/dry
(`water map x z water|dry` lines; map 89 has no terrain records). APK 23:58
had all of it; not seen on the Thor. Map 89 has no tags yet (the user paints
them). `KOL_SETTINGS_OFF=key` added. Note: `KOL_SETTINGS_ON` has no
`shadows`/`ambient_occlusion` keys; use `KOL_SHADOWS=n KOL_PERSP_AO=0.5
KOL_PERSP_AO_R=32`.

**Built and verified 2026-10-11 (23:35), UNCOMMITTED, waiting on
"commit":** shadows darker. The user picked **0.5** (was 0.35) from
`shots/shadow_strength/sheet_*.png`. `kShadowStrength = 0.5f` in
kol_focus.cpp and the README "Shadows" note. Checked: BN at present 60
(`KOL_SHADOWS=2 KOL_PERSP_AO=0.5 KOL_PERSP_AO_R=32`, 480x270) with the
default is byte-identical to `KOL_PERSP_SHADOW=0.5` and differs from 0.35;
both invariants hold (66e13ab2 / 9054c456). APK built 23:35 (libmain.so
relinked) for Ctrl+Option+I; not seen on the Thor. Unit shadows follow the
same constant.

**Also UNCOMMITTED, built and verified 2026-10-11 (APK 23:51):** asked for the
current settings to become the Enhanced default, the shadow angle to be
the default for every map without an override, and the sun-angle option
to leave the menu. (1) Shadows is ON/OFF in
Advanced Settings; the default sun is the Thor's on map 9 (BN), read from
its `sun:` log line: 36.0 degrees toward (0.585, -0.811)
(`kShadowSunDegrees`, `shadow_dir_pick`). I first took the saved
`shadows=2` (Low Sun, 30 from (-1,-3)) for it: WRONG, the user had moved the
sun with L2 and the cfg does not record that. Maps in `kMapSuns` / `sun`
lines keep theirs; `shadow_from_side` is gone; a saved `shadows=1..4` reads
as on. (2) Enhanced preset's shadows is now 1, and with no
`enhancements.cfg` Android `settings_init` starts on Enhanced
(`preset_fields`). The Thor's cfg matches Enhanced. Checked: both
invariants; BN 384x216 default byte-identical to `KOL_PERSP_SHADOW_ELEV=36.0
KOL_PERSP_SHADOW_DIR=0.585,-0.811` (baf561ab). Fresh-install path not run
(Android only, hands off the Thor). Installing resets the L2 arc, so the
Thor shows the new default after the install. Debug > L2 Moves Sun kept (it sets maps' own suns). README:
"Shadows" note and "A fresh install starts on Enhanced".

**Open from the water work:** the quay's shadow on placed water is dark at
map 89's low sun (offered to lighten shadows on water; not asked); the L2 pad
editor has no water field; Thor cost of the water step not measured (0.25 ms
on the Mac worker for BN).

## HANDOFF — 2026-10-08: statues, map 89's dusk sky, map 13's houses, the SUN tab (read this first)

**State.** Committed (the user asked to commit), nothing pushed; gbarecomp
unchanged. The APK built 21:49 has all of it (Ctrl+Option+I). README
sections named:
- *Map 54's statues stand* (FS-FX, from 10-06/07): rounded edge boxes,
  record 87 statues (`kCutoutRecords`, `kMiddleRecords`), the hand hole.
- *Map 89's sky at dusk*: `kEdgeFadeMaps` {map, width, sky, desat}, map 89
  dusk 0.35 (the user's pick); `recolour_backdrop`; the fade still goes to
  the (recoloured) backdrop. I misread the fade-to-black request twice first.
- *Map 13's houses stood as cut-outs* (FY = the user's slot 1): record 71
  under byte 0x0A; `kTerrainRecords` {71}. Its front house's chimney still
  stretches (no box yet).
- *The SUN tab*: L2 + stick y turns the sun; editor SUN tab, SET SUN /
  CLEAR, `sun map degrees x z` lines (baked too). Stick turn not tried on
  the Thor.
Next state letter: FZ.

## HANDOFF — 2026-10-05: compositor cut, awnings, a map's own sun (read this first)

**State.** Committed in both repos (the user asked to commit, 13:55), nothing
pushed: gbarecomp = the compositor commit; KoL = the commit carrying this
handoff. The APK built 13:52 has all of it; the user installs with
Ctrl+Option+I. `git status` in both first.

**Done today** (README sections named):
- *The wide compositor's work cut*: gbarecomp `render_scanline_wide`, ~31%
  less on the Mac (guest phase 7.87 -> 6.72 ms on EM), byte-identical
  (`GBARECOMP_COMPOSE_HASH`, 20 runs). **Thor, 10-06** (EM, the user
  playing): guest thread 32.0 M cycles a frame at 59.8 fps (10-01: 30.6 M);
  the compositor 28.3% of it, ~9.1 M (10-01: 35%, ~10.6 M), ~15% less;
  the rest of the thread grew ~3 M with what was added since.
  The 2.6% `unw_getcontext` in that profile was the PLT (README "`unw_getcontext`
  at 2.6% of the thread was the PLT"): `-Wl,-Bsymbolic` on the Android
  `main` (committed 10-06 with the soft shadows, APK built 10:42); not yet
  measured on the Thor.
- *The range grown*: the right stick 1.5x sideways, 2x up/down.
- *Awnings: open roofs, and a pyramid*: OPEN + PYRAMID in the roof editor;
  the cloth by colour, a floating shadow, an overhang, BG0 cloth lifted.
  Map 89's kiosk is `roof 89 6 15 7 16 X 1 20 28 11 1`; the Thor's file has
  ridge 26 (the user can set RIDGE 14). The user approved it.
- *A map's own sun*: `kMapSuns`, map 89 = 18.5 degrees from (2, -1).

**Then committed (the user asked to commit, 14:40):** the Thor's
drawn_heights.cfg baked into the build (README "The Thor's edits, in the
build"): `tools/bake_drawn_heights.py`, `src/kol_drawn_baked.inc`,
`kol_tile_edit.cpp`. The user keeps editing: bake again when they ask
(pull the file, run the tool, build).

**Then committed (15:20):** a map's cut edge faded to the background
(README "A map's cut edge faded"): `kEdgeFadeMaps` {89, 32},
`map_source_extent`, kol_focus `edge_fade_build` (a distance transform in the
array's pixels, once per map), kol_perspective `edge_sky_rows`. The user
picked 32 from the mockup; fixed twice from the Thor (rows shorter than the
outermost, then the step's junction).

**Then committed (16:00):** dialog boxes' halo (README "Dialog boxes had a
halo": the light now falls on the map under HUD layers) and the
fortune teller's candle (README "A candle that lights": `kMapLamps`, map 89,
medium; `unit_lamp_glow`; `torch_tint`; state FC). Next state letter: FD.
And her table's sprite part now joins her unit (README "The fortune
teller's table took no light": a sandwiched, touching part; `KOL_NO_PART_JOIN`).

**Then committed (17:20):** two girls' flicker in map 89's opening (README "Two
girls flickered": kRunSlack 8; g_blob_of, the shadow blob as a ground decal;
state FE). Next state letter: FF.

**Waiting on the user:** the Thor look of the 17:16 build.

**Then committed (18:50, KoL only): the field past the game's grid** (README
"Past the grid, 32 cells on every side", in the viewer section). The
editor's heights, roofs and boxes reach X/Z -32..63 (`kTileMargin` 32,
`tile_cell`/`tile_x`/`tile_z`); kol_focus arrays are 96 across (`gi`).
Byte-identical with no margin edits (39 renders, both invariants, cache
check 0); worker cost back to noise after two fixes (unit shadow reach,
per-frame alloc). APK built for Ctrl+Option+I. Not seen on the Thor.

**Then committed (22:05, both repos): two flashes after New Game** (README
"Two flashes after New Game"): gbarecomp `present_job` draws a job made in
place when the worker's is the wrong size (was: the clear colour for a
frame); KoL `edge_fade_build` takes the scroll lead modulo 256. APK built.

**Then committed (22:40, KoL): a flying unit's shadow and GUARD** (README "A
flying unit's shadow, and GUARD bent over the griffin"; state FF = the
user's slot 1). Then the stone beside the command menu and the ground
behind its icons (README "The stone beside the command menu"; FG = slot 1).
Next state letter: FH. APK built.

**Then committed (23:08, KoL): menu icons moved with the siren** (README "Menu
icons moved with the siren"; FH = slot 1). Then the ground around a tree
(README "The ground around a tree was the tree's"). The snow on a tree's
own cell behind its trunk still stands with it; offered a trunk-column rule
for tall trees (mockup first), the user chose to leave it as is for now.
Next state letter: FI. APK built (23:03).

**Then committed (23:17, KoL): the hand cursor's cut-out in a shadow** (README
"The hand cursor's cut-out in a shadow"; FI = slot 1), and the second hand
over the sky (FJ = slot 1). Committed a9efdc7.

**Then committed (23:23, KoL): a cut-out's foot cut off under the stick**
(README "A cut-out's foot cut off under the stick"; FK = slot 1). Committed
8e128cf.

**Then committed (10-06, KoL): unit shadows soften with distance** (README "Unit
shadows soften with distance"; rate 0.06, the user's pick to try), and
terrain shadows the same (README "Terrain shadows soften with distance
too"; 0.11). Next state letter: FL. APK built.

**10-06: two more IWRAM copies compiled** (README "Two more IWRAM copies,
found by a sweep"; committed 10-06; regenerate
`generated/` on a fresh tree). `0x030012D0` (ROM `0x08025FE4`, 0x280) and
`0x03001550` (ROM `0x08022438`, 0x200): EX's ~30,000-instruction bursts
while a scene loads are gone. Byte-identical old/new (16 runs + cold boot),
both invariants. APK built 11:08 (has -Bsymbolic too). Open: the 21 ROM
places (159 PCs) from the Mac self-heal cache, not reviewed; the Thor does
not record its misses (exit-only, relative path).

**10-06, then (committed, both repos): every saved state fully static,
and the Thor records its misses** (README "Then the rest: every saved state
fully static" and "The Thor records its misses"). KoL `game.toml`: 55
`[[extra_func]]` (pointer-only entries after pools) + one `[[jump_table]]`
(0x080735A0, 129); `main_android.cpp` writes `files/misses/coverage_*.json`
every 600 frames. gbarecomp: per-PC `insns`, `GBARECOMP_MISS_FLUSH_FRAMES`.
**The user: before going public, remove the miss recording, the whole
Debug page (incl. L2 Moves Sun), the height editor and env.cfg** (README
"Before going public", the release checklist).
169/169 states FULLY_STATIC; byte-identical old/new; both invariants. APK
built 11:28. **When the user has played:** pull `files/misses`, review,
add, verify the same way. Left: cold-boot IWRAM 0x03007DC0 (3 PCs).

**10-06, committed (KoL): light through the castle's windows (godrays)**
(README "Light through the castle's windows"; FL = the user's slot 2, map
55). The user picked the map's own sun (row 1 of
`shots/godrays/sheet_suns_off_on.png`): default on with Shadows on map 55,
1.5 / haze 0.05. Cached: 0 on still frames; ~1.5 ms per moving-camera
frame and 2.6-9 ms per moving-sun frame on the Mac. Then from the Thor: map 55's own sun saved (kMapSuns
{55, 49.5, -0.316, -0.949}); the left window's cut haze, units in the
light, and the hand's cutout fixed (README "Then, from the Thor"). APK
rebuilt; Thor cost not measured. Then (committed): tamer and warmer (C:
1.0, colour 1/0.78/0.48), the sun turned 2/3 toward the side (D: 0.589,
-0.808), and lifted walls/props no longer cut holes in the light (FM =
slot 1; README "Holes in the light where walls are lifted"). Next state
letter: FN. To add another map's windows: KOL_FOCUS_DUMP,
read .wx/.y on the glass (README). Next state letter: FM.

**10-06, committed (KoL): Graphics presets** (README "Graphics
presets", under the options menu section): the first enhancement row,
Enhanced (the Thor's settings) / Original (all off, 240x160) / Custom.
`kol_settings.cpp` kPresets, `kol_menu.cpp` Kind::Preset; then the effect
rows moved to an Advanced Settings page (kAdvanced, page 4). APK, not
seen on the Thor (the desktop runner has no width ladder, so untested
there).

**10-06, committed (KoL kol_focus): lifting after View List** (README
"After View List, lifting died and a prop blinked"; FN = slot 1). The
stutter is the game's own 54-frame load; not fixed. Next state letter: FO.

**10-06, committed (both repos; gbarecomp ffc6c30): Fast Loading** (README "Making the
return from View List quick"): the 54-frame hold coming back from View List
(FN = slot 1) is the game's VBlank-handler steps; now native BIOS Huffman/
LZ77 (gbarecomp `bios_hle_service_decompress`), native A7, memset >= 4 KB,
and three pure steps memoised on their inputs (disk memo
`files/fast_load_memo.bin`). Hold 54 -> 6 frames on a hit (26 recording).
Setting `fast_loading` (Advanced; Enhanced on). Verified in check mode, all
states, memory diff, invariants. gbarecomp: PC histogram / PC watch /
read-write recorder (DEBUG.md), HLE Huffman fix, `g_runtime_presented_frame`.
**Thor, measured 15:19** (README "Measured on the Thor", `hold:` lines in
logcat): back from View List 883/866/883 ms off, 78/78/100 ms on (memo
hits); the 67 ms hold going in is gone too. APK 15:17 installed. Next
state letter: FO.

**10-06 evening, committed (both repos):** the cursor's hand over the
anchored HP/MP box (FO; README "Then the cursor's hand did cross one");
the dragon cut on the deployment tiles (FP; README "And only the panel's
interface goes with it": priority-0 sprites alone move with the roster
panel); sprites coming apart under the stick (README "Then sprites still
came apart under the stick": gbarecomp `GbaPpu::latched_obj_drawn`, KoL
places unowned OAM entries where they were drawn); **Terrain types** in
the editor (FQ, map 54; README "Terrain types: a cell drawn with a rounded
corner": Height/Type fields, `type map x z left|front|right|back` lines,
one-cell rounded boxes, radius 32). APK 18:14, not seen on the Thor. The
user still has to pick the FQ cell (likely 12,11 Round Front). Next state
letter: FR.

**Then committed (20:40, both repos): Debug > Instant Win** (README "Instant
Win" and "Not every battle can be won so", in "Debug: win the battle"):
replaces R3; a hook on the battle task 0x08060E54 sends it to the game's
outcome check at a state 0x00. Wins BL-type battles (routine 2); story
battles with a flagged leader (FR, map 54, routine 0) declare nothing, and
the task is put back to 0x00 so no turn is lost. Winning those would need
the game's own death path; offered, not asked. **gbarecomp: the hook
registry held 32; Fast Loading made 37, so auto-win, the menu sound, the
second screen's game options (SoundMain) and scene_strip were never
installed from the 14:59 APK until this commit** (now 128, refusals logged).
APK 20:33. Next state letter: FS.

**Rules learned today** (memories `kol-awnings`, `kol-compositor-cut-1005`,
`kol-map-suns`): windowed test runs one at a time; fit a shape with a
SOLID mask over the flat frame at the Thor's view size (384x216); when a
report does not reproduce, measure the Thor screenshot's pixels against
the art before theorising; read the Thor's `sun:` log line before an
install (a restart resets the debug sun); the Thor's screencap is black
while its screen sleeps. Headless map viewer: EV + `KOL_DEBUG_MAP=n` +
`GBARECOMP_INPUT_REPLAY` (A at 1324, then D-pad lines), dump at present
800; desktop light: `KOL_SHADOWS=<0..4>` or `KOL_PERSP_SHADOW=0.35
KOL_PERSP_SHADOW_ELEV=.. KOL_PERSP_SHADOW_DIR=..`, `KOL_PERSP_AO=0.7
KOL_PERSP_AO_R=32`.

## HANDOFF — 2026-10-03, morning: every map viewable, Tile Info, live height editor (read this first)

**State.** Committed, nothing pushed: KoL `widescreen-16-9` tip = the commit
carrying this handoff (before it `90f6105`, `41aa0be`, `200194f`,
`5a60948`); gbarecomp tip `f1e726a`, clean. The APK built 08:42 has all of
it; the user installs with Ctrl+Option+I (not confirmed on the Thor yet).
`git status` in both first. `game.toml` changed this session: a fresh tree
must regenerate `generated/`.

**What the user is doing now:** correcting the drawn heights of every map.
The tools, all under Options > Debug (README "Debug: any battle map, through
Training" and the sections after it):
- **Battle Map** (the 67 maps a Training battle can show: 1, 3-56, 104-115)
  and **Scene Map** (the other 52). One setting `debug_map=`.
  - Training / random battles take a Battle Map one: hook on `0x08118B4C`.
  - A story scene takes either: hook on the scene map loader `0x0811C77C`
    (an `[[extra_func]]` now). The scene-layout maps (header `1416 1213`)
    cannot load in a battle: their layers sit in the battle interface's VRAM.
  - Viewer: set the map, load the title (the user's slot 1; `EV`), New Game.
    Once the scene loads the map: A/B/Select/Start are kept from the game
    (scene held at its first line), BG2/BG3/sprites hidden, the D-pad moves
    the camera (writes the ease target `0x02026C80`; moving the camera
    `0x02026C78` itself is undone each frame).
- **Tile Info**: the target cell tinted, and map / cell / stored and drawn
  height / byte / record / kind at the top left (the warp's `moved`
  overlay, a 3x5 font). Target: the game's cursor cell, or in a scene the
  cell a third down the middle of the view.
- **Live editor** (`kol_tile_edit.cpp`): Tile Info on + hold L2, D-pad
  up/down = drawn height +-1, A = back to the game's. Saved at once to
  `files/drawn_heights.cfg` ("map x z height"), applied over `kDrawnCells`.
  **When the user says they are done:** pull it with
  `adb exec-out "run-as com.koldecomp.game cat files/drawn_heights.cfg"`,
  turn each line into a `kDrawnCells` entry (record -1), verify (both
  invariants, the edited maps' stills), commit, and the file can go.

**Also found this session:** the 122 story-scene script ops (table
`0x08165020`) were all non-static: every story scene ran interpreted on the
Thor. They are `[[extra_func]]` now (New Game's opening: 232 dispatch misses
-> 5). Op 23 `0x0811978C` hangs on purpose (`b .`) on foreign maps; under
Battle/Scene Map it answers 0x110. Earlier today: lifted props/walls with
AO under them, the castle's wall end squared (ET), lifted walls blur per
texel (all committed).

**Fixed after the first handoff commit (09:00 APK):** the scene viewer
broke on most maps (EX = the user's slot 2, map 100: camera 101, ease point
536). The D-pad now moves the camera and its ease point together (never the
copy at +4), the scene's own easing is cancelled each frame in the viewer
(`0x02026C88`/`0x02026C8C` zeroed), and `kol_map_source` follows the known
camera when too little is drawn to verify (README "When I gain control on
most maps, it's broken"). Loading a state ends the viewer (New Game again).
Stale tiles of the game's map buffer outside a map's area can show.

**Then (10:02-12:43 APKs):** the viewer's D-pad steps a tile cursor (the
game's directions: up Z-1, down Z+1, right X+1, left X-1), the camera eases
to centre it (goal from kx + camera, learned once with the camera still: a
per-frame sum shook it); off-field cells (byte FF, height 0) are drawn when
given a height (editor or table), and Tile Info says "OFF FIELD".

**Then (night of 10-03):** the light without the perspective (committed
`d1e5ff1`), and the roof tool (README "Roofs, set in the editor"): Tile Info +
L2: B twice = roof over a rectangle, left/right = field (height, eave,
ridge, gables, axis), up/down = change, A = delete; saved as `roof ...` lines
in drawn_heights.cfg; modelled after the building model in kol_focus.
State EY (map 95 houses). Next letter EZ.

**Not verified on the Thor:** the L2 editor and the A block (the headless
input replay writes keys after the host's key filter, so it cannot press
them; `KOL_TILE_EDIT_FILE` / `KOL_TILE_EDIT_HOLD` test the rest). The 3x5
readout font has not been seen on the device.

**Open:**
- The scene camera starts where the opening scene puts it: on 2, 57, 119
  the map is off-view until panned.
- A rounded cell (`kDrawnRounds`) keeps its top at the stored height; an
  edit moves its foot only.
- Map 2 in Training shows the wrong animated tiles/palette (the game's own
  battle switches to a rain variant later); it views fine through a scene.
- The scene script interpreter itself was not found.
- Still open from before: the compositor (35% of the guest thread), the
  idle loop (the user's call), Thor cost of lifting.

**Tools:** `KOL_DEBUG_MAP=n`, `KOL_SAVE_STATE_AT=<frame>[,slot]` (needs
`--window` + dummy video; the TCP rig cannot run KoL's hooks),
`KOL_TILE_EDIT_FILE`, `KOL_TILE_EDIT_HOLD=1`. States: EU (map 89 scene), EV
(title, New Game: load frame 1314, A at 1324), EW (New Game dialogue), EX
(slot 2, map 100 viewer broken; loads with the viewer off). Next state
letter: EY.

## HANDOFF — 2026-10-01, night: the present pipeline (read this first)

**State.** Uncommitted in BOTH repos (the user has not asked to commit):
gbarecomp `runtime.h`, `host_window.cpp`; KoL `kol_focus.{h,cpp}`,
`kol_perspective.{h,cpp}`, `kol_widescreen.cpp`, README, this file. The APK
built 22:28 has it; the Thor still runs the 21:57 APK (lighting, all
options on). `git status` in both first.

**Asked:** full speed on the Thor with all options on at the highest
resolution; on state 1 the audio showed the game was not at full speed.
Then the user changed the scene
(expected to be worse): states `EL_slot1_slow_audio`, `EM_slot1_slower`
(the castle map, ~15 units). Readings while the user played: 56.0 then
50.6 fps, 43.3 M then 47.6 M guest cycles a frame, guest thread 95-97% of a
core at ~2.5 GHz, GPU 24%.

**Done** (README "The present pipeline: the warp and the focus a frame
behind"): the present-time providers (warp, focus) run on a gbarecomp worker
one frame behind, over a capture of everything they read; the guest thread
keeps emulation, the compositor, ~2 MB of copies and the GL draw. The
profile said the present work was 47% of the guest thread, so the thread
should land near 25-28 M cycles a frame (budget ~41 M at 2.5 GHz).
Verified on the Mac: both invariants; direct present N == pipelined present
N+1 byte for byte on EM, B_rictor, CI_thor_snow_perf, stick and combat
camera. **Measured on the Thor** (the user installed at 22:29 and played
the same map): 59.8 fps with no long frames (was 50.6), guest thread 30.4 M
cycles a frame (was 47.6 M), 72% of a core (was 95%); the worker ("present",
with the row-pool workers, which now inherit that name) is 51% of the
process's samples. SoC 69-86 degrees (67-83 before): more total work.
**The user has not said how it feels or whether to commit.**

**Committed:** gbarecomp `7481f7a`, KoL `236f8b1` (the user asked
to commit). Then the user asked about heat and chose the worker route.

**Worker pass (uncommitted at the time of writing; README "The worker's
work cut"):** the terrain pass cached across still frames (PassCache,
keyed on everything it reads, self-check `KOL_PASS_CACHE_CHECK=1`), the
mesh index build through a pointer, `ao_reach()` hoisted. Worker job on
the Mac 5.28 -> 4.17 ms (-21%); byte-identical to the previous binary on
EM, B_rictor, CI, BK, stick, combat camera; both invariants. Also added,
env-gated: `KOL_PERSP_HASH=1` (per-stage hashes), `GBARECOMP_WARP_HASH=1/2`
(draw buffers / readback per pass), `[present-pipeline]` worker timing when
GBARECOMP_FRAME_PHASE is set, `KOL_PERSP_CHUNK`. **Thor, measured** (installed 23:22, the user
playing): guest thread 30.6 M cycles a frame (unchanged), worker + pool
22.2 M (was ~33 M), worker share 51% -> 41%, scene build 30% -> 12% of
the worker, GPU 27% -> 13%, SoC 58-82 degrees (was 69-86; scene may
differ), 59.8 fps. The user has not asked to commit this pass.

**Open, found on the way:** the Mac's SDL software renderer is
nondeterministic on `BL_enemy_turn` (45 outline pixels, two variants,
~1 run in 6 under load, pre-existing: the committed baseline shows it too,
pipeline on or off, pool on or off). Every CPU buffer hashed identical in
the outlier runs, so it is in SDL's software rasterisation or readback,
which the Thor never uses. Recipe `det14.sh` in the scratchpad (gone next
session; it is 12 loaded runs with dumps at present 61). Until understood,
byte-for-byte picture checks on the Mac use still states only.

**10-02: drawn heights COMMITTED** (KoL `31366ee`, gbarecomp `6bb67c9`, the
user asked to commit): README "Perspective: drawn heights"; the pit's water at
7 (map 55, record 77 cells); the Mac hairline was SDL's software renderer
(fixed in gbarecomp, ENHANCEMENTS.md §6).

**Then committed `6bfb767`: the gargoyle as a cut-out** (README "The
gargoyle stands as a cut-out").

**Then committed `d422879`: round towers** (EO).

**Then, uncommitted: rounded boxes** (EP = the user's slot 1: the two
visible towers and the big door frame). README "Round towers", "More of
them, and rounded boxes". `kDrawnRounds` is now {map, cells, radius,
corners}: the four towers (two corners of radius 32 each) and the door frame
(front corner, radius 32), all measured on EP's flat frame; drawn per cell
(painter order). The x 6-7 tower is round: my "square" from EO was wrong.
Verified: invariants, five lit states identical, EO/EP switch-off identical,
cache check 0, no gaps. Stills `shots/round_towers/EP_*`, `EO_*`. APK built
for Ctrl+Option+I. Then (EQ, the user's slot 1) the door frame's right
end, the wall's end beside it and the left wall's second tower. Then (ER,
the user picked red and blue on `ER_which_corners.png`) the walkway by the
pool, x 6 rows 6-10, right and front corners at radius 32 (the user's
pick over 16). Then (ES, the user's slot 1, seen on the Thor: extra pixels
beside the tower by the hand) the outline margin cut to one column, with a
second along the towers' cornice (`cornice` 40). **Waiting on the user:**
the Thor look, then a commit. Committed `15ff665`.

**Then, uncommitted in BOTH repos: lifted props** (asked: show what BG1 hid
when the camera moves; the user agreed to try it). README "Perspective: lifted props,
what they hid"; gbarecomp ENHANCEMENTS.md §6 (`g_ppu_scene_under_bg1`, the
software fallback's quarter-texel uv). Options > Lifted Props, off by
default. Verified: invariants, KoL stages identical with it off, cache check
0 with it on. Then BG1 terrain too (user: the castle's wall pillars over the BG0 wall):
surface squares, corners from their own pixels, per-pixel quads; Mac worker
+1.35 ms on ER. Split at the user's ask: Options > Lifted Walls (BG1
terrain) beside Lifted Props. `KOL_PERSP_MESH_DUMP` added. Thor cost not measured. APK
built.
Then (asked whether AO could go under these, then for both), also
uncommitted: contact occlusion where lifted props stand (`prop_contact_ao`,
the foot = the cell cut at the prop's lowest row), and the wall behind
lifted BG1 terrain with its own occlusion (`g_back_ao`, `lift_ao`); README
"Occlusion under them". Byte-identical with both switches off; cache
check 0. APK built.
Committed `5a60948` (gbarecomp `f1e726a`). Then (ET = the user's slot 1,
uncommitted): the wall end at x 1 z 11 is square, its rounding removed from
`kDrawnRounds` (README "And three more", the correction). Then the depth
of field on lifted walls (every wall square was at full blur; now per
texel; README "Depth of field on lifted walls"). APK built.
Committed `200194f`. Then (10-03, uncommitted): Debug > Battle Map (README
"Debug: any battle map, through Training"): a hook on `0x08118B4C` gives
Training and random battles map 1-119; 67 load whole, 2/57-103/116-119 not
(graphics from elsewhere, not found). `game.toml` changed: regenerate.
Committed `41aa0be`. Then (uncommitted): the scene route (README "The other
maps, through a story scene"): hook on the scene map loader 0x0811C77C, the
122 scene script ops as extra_func, op 23 answered 0x110 under Battle Map,
D-pad moves the camera target 0x02026C80. New Game shows any map. States
EU (map 89 scene), EV (title, New Game). Next state letter: EW.
Then Debug > Scene Map (the 52 non-Training maps) beside Battle Map, and
Debug > Tile Info (target cell tinted, readout of map/cell/heights/record
top left). Committed `90f6105`. Then (uncommitted) the live editor:
Tile Info on + L2 held, D-pad up/down = drawn height +-1, A = reset;
saved to files/drawn_heights.cfg (`kol_tile_edit.cpp`). To bake the user's
edits: `adb exec-out "run-as com.koldecomp.game cat files/drawn_heights.cfg"`,
then kDrawnCells entries. Then the viewer: once the scene loads the map, A/B/
Select/Start are kept from the game (scene held at its first line) and BG2,
BG3 and sprites hidden. States EW (New Game dialogue). Next letter EX. APK
built.
Next state letter: EU.

**TODO (the user, 10-01 night, asked to keep the compositor on the TODO list):**
the wide compositor, 35% of the guest thread (`render_scanline_wide`, its
per-pixel resolve `$_9` 21% of the thread). Must stay byte-identical (both
invariants). Also still open: the idle loop `0x08000768` (~1.5 ms, changes
guest timing: the user's call, not asked); on the worker, `keep_row`
(exact divisions), the focus loop, the AO/glow apply loops.

**Tools this session:** scratchpad `bench.sh` (frame phases, all effects
on), `abcheck.sh` (direct vs pipeline dumps), the simpleperf recipe from
memory `kol-thor-cycles-per-frame` (binary_cache_builder + llvm-symbolizer
per line). The Mac rule held: every run `--window` with `SDL_VIDEODRIVER=dummy
SDL_RENDER_DRIVER=software`, `--frames`, and an alarm.

## HANDOFF — 2026-10-01, morning: world-map profile polished, menu button feedback built (read this first)

**State.** Nothing pushed. gbarecomp clean (`d73016d`). KoL committed today, on
the user's go-ahead, one commit per feature: `c99d1da` camera votes, `4d88013`
grey bottom bar, `4022114` world-map army list + profile (with the two changes
below). **Uncommitted: the menu button feedback** (sound, pressed look, hand;
`kol_menu.*`, `game.toml`'s `0x080020B8` hook, JNI, `SecondScreen.java`,
`StatsView.java`, README, this file). The APK built 10-01 08:34 has all of it,
waiting for the user's Ctrl+Option+I. The Thor runs the 10-01 00:05 APK.
`git status` in both first.

**Done this session** (README sections named):
- The user on the 00:05 APK: the army list looked good to them. Asked: no Menu on
  the overworld, and the list to stay while the profile is open. Both in
  `4022114` (README "No command menu from roaming; the list stays; another
  tap switches"):
  - from roaming the command menu's draw `0x08127F7C` returns at once during
    the flow; on the way back the menu gets B in `0x03000822` at state 1 and
    closes itself (its cancel sound dropped via a hook on `0x0814506C`);
  - `kol::action_profile_busy()` keeps the army list composed until the
    menu is back at state 1;
  - a tap on another card while the window is up switches the window to it.
  - Checked against the game's own R, A, A, B, B, B: same end frame (the
    location name over the marker is the game's). From DW (menu open) B
    still lands on the menu.
- **Menu button feedback** (README "The menu button: pushed in, the game's
  sound, the hand"): the sound hang is FIXED by replacing `0x080020B8` (one
  byte flip) instead of returning into its hooked entry. Pressed look and
  the bobbing hand are wired (JNI `menuZone(h, pressed)`, `menuHand()`,
  `StatsView.drawHand`). The bob (0,1,2,1 px, 133 ms steps) is my choice:
  the game's OPTION hand does not move.
- Both invariants after each step (66e13ab2 / 9054c456).

**Later (09:22 APK, uncommitted):** Organize opened the normal way keeps the
army list on the second screen while a unit's window is up, with that unit
lit and the rest dimmed; in the grid the second screen shows the sheet of the
unit under the grid's cursor (asked on DX = the user's slot 1, the grid).
README "Organize keeps the army list, with its unit lit". A tap in the window
switches it; a tap in the grid does nothing yet. Next state letter: DY.
Committed `0de8de9`. **Then (09:36 APK, uncommitted):** the bottom bar and its
button back in the frame's yellow (grey reverted), pressed = 4/16 darker; the
hand beside the button is still; the hand inside the options menu bobs; the
top screen dims 50% (was 30%) while the menu is open; the menu reopens on
the page (submenus too), row and scroll it was closed on. Committed `a415b39`. Then (uncommitted) the bottom bar
takes the frame of the game's own status window (archive `0x086B75B4` tiles
9-16, DY = the user's slot 1); the button keeps its SUPPLY frame. Next state
letter: DZ. Committed `f41d5a8`. Uncommitted: no Alphonse lit during a tap's
fade (the tapped unit until the window is built); the button wears the
Option icon (icon 2 of `0x086DA2E4`; DZ = the user's slot 1, world map system
menu, Option highlighted). The second screen's canvas is 248 wide (31
columns) so the Thor's 1240 px have no side gaps (README "The second screen,
edge to edge"). Committed `153b01a`. Uncommitted: Save and Load buttons on the
bar on the world map, driving the game's own screens (README "Save and Load
on the bar"; EA = the user's slot 1, Save highlighted); greyed (no touch)
while the game's save or load screen is up. Committed `290a531`.
Uncommitted: Organize, Training, Shop at the bar's left (EB = the user's slot
1, the main menu at Solea), greyed as the game's menu leaves them out; a
Shop Anywhere option (README "Organize, Training and Shop on the bar").
Committed `1972338`. Uncommitted: Item, Spell and Class (Organize's R menu,
EC = the user's slot 1) replace Organize on the bar, opened through a hook
on the window's key reader 0x0803AD4C (R to the grid, A to its R menu).
Committed `6ed7dcd`. Uncommitted: a long press on a bar button shows its
game name and HELP text as a card (README "A long press shows the button's
help"); in Organize's item and spellbook stocks the second screen shows the
HELP of the one under the cursor (ED, EE = the user's slots 1, 2), and in its
class list the class's. Committed `dcc8ab3`. Uncommitted: nothing lit behind
the grid's R menu (screen 3, EF); Item/Spell/Class live on the bar inside
Organize (EG). Committed `7b38396`. Uncommitted: in a unit's profile (and its
R menu) the bar's Item/Spell/Class open that unit's screens (EH = the user's
slot 1); the grid's R menu is not drawn while the bar's steps pass through it
(for a clean look; the direct route was tried and reverted, README). Next
state letter: EI. Committed `aff3e59`. Then, committed together (the
commit carrying this line): ambient occlusion on the perspective
battlefield (Options > Ambient Occl.: OFF / Half Tile / Full Tile, strength
0.5; README "Ambient occlusion"); Options > Shadows (OFF / High Sun 45 / Low
Sun 30 / Side Sun = low, from (2, -1), right of the battlefield; strength
0.35; walls take shadows; same README section; EI = slot 1, the gate wall);
Options > Torchlight (README "Torchlight", EJ = slot 1, strength 1.0,
cached per view: ~0 ms with the camera still on the Mac). The user liked
it on the 16:02 APK. Thor cost of AO/shadows/torchlight not measured.
Then (committed after): props keep their own colour under AO/shadows/glow
(EK = slot 1, the snowfield's trees), and Shadows > Low Side Sun (20
degrees); APK 16:15. Then units take the terrain's shadow over them (a
card at the unit's place, rows below the shade height darken; README
"Units take the shadow over them"), APK 16:29, the user approved it.
Committed `1db5586`. Then (uncommitted) units CAST shadows and lose the
game's blob, with Shadows on (README "Units cast shadows"), cached per
unit: 0.05 ms for 9 units on the Mac; APK 21:43. Monsters' blob not
checked. Then contact AO under units (with Ambient Occl.); both behind
Options > Unit Shadows (on by default). Committed together (the commit
carrying this line), APK 21:57. Next state letter: EL. The menu
feedback is committed (`e65e814`).

**Not seen on the Thor yet:** all of today's changes. Ask the user how the
menu sound, the pressed button and the hand feel, and whether to commit.

**Headless tools used:** `KOL_LIST_TAP="f:x:y,..."` (frames from the list's
first frame) taps the list; `KOL_PROFILE_TRACE=1`; `KOL_MENU_PPM` opens the
menu (so it plays the sound); `KOL_ZONE_PPM=path,h` now writes 5 stacked
zones (plain, pressed, hand at 3 bob places). Present sequences:
`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software GBARECOMP_PRESENT_DUMP=dir/p%ld.bmp
GBARECOMP_PRESENT_DUMP_AT=1 GBARECOMP_PRESENT_DUMP_COUNT=N ... --window --scale 1`.
Load frames: DO 105031, DW 11137, B_rictor 28213. Battle OPTION menu from
B_rictor: R, Right x4, A, Right, A. Capstone + Pillow venv: session scratchpad
(gone next session).

**Possible follow-ups (offered, not asked for):** the army list in shops and
towns (other layer layouts); highlighting the card of the unit the window
shows.

**States:** DX = Organize grid (slot 1, 10-01). The next letter is DY.

## HANDOFF — 2026-09-30, morning: the unit list on the second screen (read this first)

**State.** Committed, nothing pushed: KoL `widescreen-16-9` tip = the commit
carrying this handoff; gbarecomp tip = its "Host: g_present_dim" commit. The
Thor runs exactly this code. Run `git status` in both.

**Working rules** (unchanged): commit only on "commit"; install only on
"install", `adb install -r` only, never launch/stop/input on the Thor; no
windowed Mac runs. "Load state N" = pull `files/totkol.stateN`, next letter.
This session used DE-DJ; the next is DK.

**Done** (README "The unit list on the second screen", and the menu dim in
"The options menu on the second screen"):
- While the player browses the map, the second screen lists the battle's
  units (`src/kol_unit_list.cpp`). Player's side left, others right, by NO.
  (record `+0x51`), done units last with the game's END over their heads.
  The unit under the cursor is lit and the rest dimmed, as the game's list
  does it.
- A tap glides the map cursor to the unit (the Next-unit hook writes cursor
  `+0x3C/+0x40/+0x35`). Its card slides up; a downward swipe puts it away.
- Every page sits at the top in the sheet's 184-line window. Below it is a
  zone in the SUPPLY window's frame, with the menu button inside.
- The main screen dims 30% while the options menu is open (gbarecomp
  `g_present_dim`).
- Traps are in memory `kol-unit-list`: the done bit blinks for one frame,
  END is painted into the sprite tiles, and the TCP rig doesn't run the
  frame hook.

**Open:** the list on the enemy's turn (asked, no answer); the space under a
done unit's box (the user will think of something); the swipe thresholds
are still guesses.

## HANDOFF — 2026-09-30, after midnight: options menu and EMBLEM page (read this first)

**State.** Committed, nothing pushed:
- KoL `widescreen-16-9`: the tip is the commit carrying this handoff.
- gbarecomp: tip `30612b5` (`g_host_keyinput_filter`).
- The Thor runs the APK built from exactly this code (`5c79947`, installed).

Run `git status` in both repos first.

**Working rules** (the user set these; they are also in memory):
- Commit only when the user says so.
- Install only when the user asks, with `adb install -r` only. Never launch, stop
  or inject input on the Thor while the user may be playing. Reading logcat
  and pulling `files/totkol.stateN` is fine.
- No windowed runs on the Mac (`--no-window` headless only). Frame dumps
  need a window, so use the TCP rig's `screenshot`.
- "Load state N" means pull `files/totkol.stateN` and name it with the next
  letter. This session used DC and DD; the next is DE.

**Done this session.** README has everything, in "The options menu on the
second screen" and "The EMBLEM page, and tapping for HELP".
- **Options menu** on the second screen, drawn from cartridge art:
  - the corner System-icon button;
  - the game's 12 options, editable through a SoundMain hook;
  - the enhancements, with Sound and Debug pages;
  - finger scrolling, and pad navigation through the new gbarecomp filter;
  - B closing the menu no longer reaches the game;
  - "Original" for 240x160;
  - Depth of Field is one list: OFF / Subtle / Medium / Strong / Very
    strong.
- **EMBLEM page:** a swipe on the lower card follows the finger and snaps.
  The emblem set, pictures and HELP all come from ROM (record `+0x08`
  masked by the class, archive `0x086D6B7C`).
- **Tap for HELP** on EQUIP., TALENT and emblem slots; the slot stays lit.
- **The HELP card reflows**: paragraphs only where a page ends with a
  period, word wrap.
- **One card while looking around**: move task state `0x05`, so no
  two-card sheet (`DD`).

**Verified:**
- Both render invariants hold after every change (240 `66e13ab2`,
  anchor-off 284 `9054c456`).
- Headless sheet and menu renders.
- The user confirmed on the Thor (09-30 morning): the swipe works
  well, with no flicker. The BACK menu is fine as it is. SELECT HELP
  on the game's EMBLEM page works.

**09-30 morning:** a tapped slot stayed dimmed when the HELP card reached it
(Philosopher's Stone, top right). The card now ends above the lit slot and
closes up to fit (README "Taps"). Checked on all 40 slots, committed and
installed.

**Open:**
- The swipe thresholds (a third of the width, a flick at 300 canvas px/s,
  a 70-230 ms snap) are untuned guesses.
- The Android BACK menu keeps separate DoF and blur-strength entries (the
  user: fine for now).
- Targeting phases with no saved state (items, persuasion) keep the two
  cards by design. Only `0x05` drops them.
- Still open from before: the torch on a pedestal (CA), the stick ranges,
  the idle-loop elision (the user's call), reverb and instruments for the
  sound.

## HANDOFF — 2026-09-29, late night: music timing and audio latency fixed (read this first)

**State.** Committed, nothing pushed: KoL `widescreen-16-9` tip = the commit
carrying this handoff; gbarecomp tip = its "Audio: steady pacing, audio as
the guest makes it, latency trim" commit. The Thor runs the APK built from
exactly this code. Run `git status` in both.

**Reported and fixed tonight** (README "Sound", part 4): music rushing and
crackling in menus / while units move, then lagging and arriving late. Three
causes, each measured with the audio probe on the Thor:
1. The pacer waited once per present; menus run several frames per present,
   so emulation ran 1.03-1.34x → pace every emulated frame.
2. The pacer never paid back lateness → absolute timeline, catch up < 100 ms.
3. The audio bridge: audio arrived only at presents (lumps), and concealed
   stalls became permanent latency → drain hook every 512 frames, and a
   surplus-only, pitch-preserving latency trim.
Final Thor log (95 s): 0 underrun / concealment / drops / resyncs, fill 58 ms.
The user judged it nearly perfect.

**Afterwards (README "Sound", part 5):** clicks the user heard are the
game's own (hard note starts, the square channels); the HQ render fades note
onsets in over ~1 ms, fallbacks crossfade, and "Soft tone channels" is the
answer for the squares — the user was pointed to it. The Thor runs the build
before the last change (the dump filter, which only matters with
`GBARECOMP_AUDIO_HQ_DUMP` set); install it when the user agrees.

**Device notes:**
- The sound diagnostics are off: `files/env.cfg` and `files/hqdump` were
  removed from the Thor at the user's request (the user is happy with soft
  tones and accepts the rare HQ fallbacks). To probe again, write env.cfg
  with `GBARECOMP_AUDIO_PROBE=1` (README "Sound", part 4).
- The user set view 240x160 and Sound = Original while testing.
- Memory `kol-thor-hands-off-while-user-plays`: ask before install / launch /
  input on the Thor; no windowed runs on the Mac.

**Open:** one ~2000-frame unpaced gap ~5 s after launch (a jump, not lag;
unidentified). Earlier sound items: reverb, replacement instruments.

## HANDOFF — 2026-09-29, evening: the sound (read this first)

**State.** Committed, nothing pushed: KoL `widescreen-16-9` tip = the commit
carrying this handoff; gbarecomp `widescreen-bg-y-provider-and-vram-view` tip
`76b0f93` (`audio_shadow.{h,cpp}` deleted). The Thor runs the APK built from
exactly this code; its slots, totkol.sav and enhancements.cfg were backed up
to the session scratchpad (`thor_slots/`) first. Run `git status` in both.

**Asked:** to explore a sound-engine rewrite for a better-sounding game,
then to do A and B first. README "Sound" and gbarecomp ENHANCEMENTS.md §5 hold
everything; the short form:

- **A, the mixer (gbarecomp, always on):** stereo end to end, each side mixed
  as mGBA does (PSG was ~3.9 dB hot), PSG edges band-limited (`PsgBlip`).
  Raw capture taps byte-identical to before; save states compatible both ways.
- **B, MP2K high-quality shadow (`mp2k_shadow`, rewritten):** syncs on
  SoundInfo.ident, re-mixes each tick with the driver's integer math and needs
  a byte-exact match with the driver's PCM buffer segment before its float
  render may play. 100% of ticks verified on 8 states + a 9000-frame boot.
- **KoL setting:** Options > Sound… Original / HQ mono / HQ stereo (default),
  `sound=` in enhancements.cfg. Desktop: off unless `GBARECOMP_AUDIO_SHADOW`.
- Thor: +0.4% guest cycles/frame, fps unchanged, stereo track confirmed.

**Later the same evening (items 1, 2 and 4):**
- **Music/effects volumes** (Options lists) and **effects placed where they
  happen** (toggle, on): voices classed by their MP2K player (player 0 =
  music); `kol_sound.cpp` places effects at the cursor / the unit hit / the
  acting unit. Subtle in fights: the camera centres the action.
- **Soft tone channels** (toggle, off): 5.5 kHz 2-pole low-pass on the PSG.
- **More voices: measured, not needed** (no effect ever steals a voice).
- Tail rule corrected: a one-shot's tail is mixed unless its track was stopped
  that tick. Every tick verified again in attacks, spells, enemy turn.
- On the Thor the user reports the music changing tempo and crackling a
  little when things happen on screen, whatever the graphics settings.

**Verified:** 240 `66e13ab2`, anchor-off 284 `9054c456` (also with HQ on);
gbarecomp audio ctests pass; BIOS `diff_audio.py` unchanged (rms 3.85).

**Open:**
- The user has not listened yet: `shots/sound_ab/engine/*.wav`, and the Thor.
- Not modelled, so those ticks play canon: driver reverb (only the BIOS chime
  so far), DPCM, reverse samples.
- Offered, not done: a reverb, replacement instruments. More polyphony was
  measured unnecessary (README "Sound", "More voices").
- Pre-existing, noticed: ch3/ch4 PSG state is not serialized (snapshot layout),
  so an excursion rewind silences the wave/noise channels until retriggered.

## HANDOFF — 2026-09-29, afternoon (read this first)

**State.** KoL `widescreen-16-9` tip = the commit carrying this handoff;
gbarecomp `widescreen-bg-y-provider-and-vram-view` tip `3f837b4`. Both
committed, nothing pushed. The Thor runs the APK built from exactly this
code. Run `git status` in both first.

**Working rules** (unchanged, also in memory):
- Every turn ends with an APK build, install and launch on the Thor.
- "Load state N" means pull `files/totkol.stateN` and save it under a new
  letter. This session used CU..DB.
- Commit only when the user says so.

**This session, all asked for or reported on the Thor.** README sections
are named in quotes.

Second screen:
- **B cancels the move card**: the move task's state byte `0x0202A82E ==
  0x0B` (`db7b302`).
- **Made items read on the game's side** (`ac84ea3`). The sheet, HELP and
  ATK/DEF use record `+0x5D` bit 5; the gauge uses the owner's bit 7.
  README "Made items", "Corrected 2026-09-29".
- **Battle preparation shows one card** (`ac84ea3`).
  `kol::battle_preparation()` is `0x0202A81A == 0`: 0xFF in battle, 88 in
  the army roster.

Debug:
- **R3 wins the battle** (BACK menu "Debug: R3 wins the battle", off by
  default; `4972e4c`). A hook on `0x0801FBAC` ("enemy side out", used by
  all five outcome routines) answers yes once. The win comes at the next
  defeated unit: the game checks only then (task states `0x14` -> `0x2F`
  -> `0x31`). The request is per battle, and the second screen shows
  "auto-win". README "Debug: win the battle".

Perspective:
- **Unflagged cells with a height are terrain** (`f14372b`): the ruins'
  border pillars (CV). Flag `0x80` means walkable, not drawn.
- **No sky streaks while scrolling** (`8638cfd` + gbarecomp `3f837b4`).
  The frame hook's IO holds the NEXT frame's scroll; the picture's camera
  now uses `GbaPpu::latched_bg0_scroll`.
- **Spires, pots and the church** (this commit):
  - Type `0x09` (rock spires, bushes) and `0x0A` (snow rocks, fire pots)
    are cut-outs. Their ground is the two front neighbours. Their pixels
    are their BG1 pixels, with no spread. A cell not above all four
    neighbours must rise 2 over its front.
  - The church is modelled from its art: gable planes, a stone shaft and a
    pyramid cap, rays solved exactly. The heights were measured by
    overlaying outlines on the flat frame.
  - README "rock spires stand like trees", "The fire pots too", "the
    church, modelled from its art".

**Verified each time:** both render invariants (240 `66e13ab2`, anchor-off
284 `9054c456`). Every perspective change was also A/B'd with its switch
on the maps that carry the types; the unaffected maps are byte-identical.

**Open:**
- The user has not judged the final church on the Thor. It was
  installed at the end of the session.
- The church's steeple rules (stone top = ridge + 3, point = grid + 1) are
  measured on this church only. If another free-standing building appears,
  measure it the same way (memory `kol-measure-drawn-shape`).
- The debug win waits for the next kill. An immediate win would mean
  driving the battle task into states `0x2F`/`0x31`. Offered, not done.
- The camera lag on `BL_enemy_turn` is probably the scroll-latch bug fixed
  here. Not re-checked.
- Still open from before: the torch on a pedestal (CA), the stick ranges,
  the idle-loop elision (the user's call), spells and ranged attacks with
  the two-card sheet.

**Switches added:** `KOL_PERSP_ONMAP_ONLY`, `KOL_PERSP_LIVE_SCROLL`,
`KOL_PERSP_NO_SPIRE_PROPS`, `KOL_PERSP_NO_POT_PROPS`,
`KOL_PERSP_NO_BUILDINGS`, `KOL_NO_AUTO_WIN`, plus `KOL_AUTO_WIN_AT` /
`KOL_AUTO_WIN_ANY` for headless runs.

**Tools:**
- gbarecomp `GBARECOMP_PRESENT_DUMP_COUNT=N`, with `%ld` in the path,
  dumps N presents.
- `KOL_FOCUS_DUMP` with `KOL_EDGE_BLUR=1` writes the layer map. Low nibble
  5 is the backdrop.
- The measuring method is in memory `kol-measure-drawn-shape`.

## HANDOFF — 2026-09-29 (read this first)

**State.** KoL `widescreen-16-9` tip = the commit carrying this handoff.
gbarecomp is unchanged at `8b00ad7`. Both committed, nothing pushed. The
Thor runs the APK built from exactly this code. Run `git status` in both
first.

**Working rules the user set today** (also in memory):
- Every turn ends with an APK build, install and launch on the Thor.
- Slot backups go to the session scratchpad, overwriting one set. Never to
  `states/backups/`: the user had that folder deleted.
- "Load state 1" means pull `files/totkol.state1`, save it under a new
  letter (today CN..CT), and repro headless at 480x270.

**Today, all committed and all asked for or reported on the Thor.** README
sections are named in quotes.

Second screen:
- **Acting unit and unit under the cursor** ("The acting unit and the unit
  under the cursor"). While a unit takes its action (`0x0202B26F != 0`),
  the top card is the actor (`[0x0202A5F0]`). While the map cursor is out
  (`0x03000FD5 == 1`), any other unit under it gets the lower card. In
  Attack/Cancel (`0x03000FF1`) the target goes below. Browsing shows the
  hovered unit.
- **Death flash.** A target that dies flashes red when the damage number
  pops (`0x0202AFCC`), then fades into the attacker's sheet. A cleared
  record (max HP 0, "AAAA") is no unit.
- **HELP in the battle's ARMS/TALENT lists** ("The battle's weapon and
  spell lists"): work block `[0x0202B26C]`, `+0x2047 == 3`, list
  `+0x1E94`, window `+0x206C`.
- **Support/fear gauge from live cells** (map objects, not the battle
  entry).
- **ATK from the game's weapon slots**, any position (`0x08139D1C`).
- **M.DEF for made-item bug ids** (base INT 200-255). Both were checked
  against the game's own window.
- **Move prompt and move messages skipped** ("The move prompt goes to the
  second screen"). A `[[mod_function_hook]]` on the text-box poll
  `0x0801670C` answers `0xFF` for two callers only: `0x080662F4` (NAVI
  "Choose where you want to move this character.") and `0x08066B2C` (the
  move choice's messages, e.g. "Can't move to that spot.").
  - A decline-only hook on `0x08015D50` records which text was fetched.
  - The second screen shows it for 4 s. The prompt goes when the cursor
    leaves the unit, once per move. A message goes when the cursor leaves
    the cell.
  - `KOL_NO_SKIP_MOVE_NAVI=1` turns all of this off.
  - "A move is being chosen" is the move task's state byte `0x0202A82E ==
    0x0B` (task `0x0202A5B0` + `0x27E`). B cancels it; the other flags and
    the range bits survive B (committed after the handoff commit).

Perspective:
- **Menu icons stay interface.** The unit run is trimmed at ends cut off by
  entries that fit no unit (`CP_spell_hand_merged`).
- **Bags on the map are objects.** Sprite slot routine `0x0805AD9D` makes a
  prop. A prop claims the sprites no unit takes, and competes by draw order
  when it drew something (`CQ_bag_stuck_ivanna`).

Also: a "Debug: perspective mesh" BACK-menu toggle (= `KOL_PERSP_KIND=1`).

**Verified each time:** both render invariants (240 `66e13ab2`, anchor-off
284 `9054c456`). The perspective changes were also checked byte-identical
on seven earlier repro states (CL, CH, CJ, CF, CK, BK, BV).

**Open:**
- The fresh-unit path is untested. NAVI entry `0x08` is the same "Choose
  where…" sentence, and its caller was not found. If pressing A on a unit
  at the start of its turn still shows the prompt, ask for a slot-1 save
  just before that A.
- Spells and ranged attacks with the two-card sheet are unchecked.
- DONE after this handoff: `item_at()` takes the game's side (README "Made
  items", "Corrected 2026-09-29"): bit 5 for the sheet, the owner's bit 7 for
  the gauge.
- Still open from before: the torch on a pedestal (CA), the stick ranges,
  the camera lag on BL, and the idle-loop elision (the user's call).

**Tools this session:**
- `KOL_SHEET_DUMP=dir/s_%04d.ppm` writes one sheet per changed frame.
- `KOL_NEXT_UNIT_AT` walks the game's status window, to check stats
  against the game.
- The entry hook `g_runtime_fn_entry_hook` on `0x08015D50` (help and text
  fetch) finds any text box's bank, index and caller.
- `GBARECOMP_INPUT_REPLAY` with an EMPTY file produces nothing.

## HANDOFF — 2026-09-28, late evening

**State.** KoL `widescreen-16-9` tip = the commit carrying this handoff;
gbarecomp `widescreen-bg-y-provider-and-vram-view` tip = its perf commit
right after `54ce493`. Both committed, nothing pushed. The Thor runs the APK
built from exactly this code. Run `git status` in both first.

**This session** (README, end of "Perspective on the battle map", three new
sections):
- **Performance, for heat and fan** ("Performance pass on the Thor"). Every
  change is byte-identical. Guest cycles per frame on the Thor went 48.8M ->
  39.2M, and the guest thread 97.7% -> 82.6%. The biggest single item was
  gbarecomp's Android frame pacer spinning `yield()` for 1.2 ms a frame (it
  sleeps now). The rest is in the compositor, the present uploads and KoL's
  OBJ providers.
- **Sprite ownership by the game's draw order** ("Sprite pieces go by the
  order the game draws them"). Three fixes, all user-reported:
  - the cockatrice and white dragon split (CJ);
  - the cursor chevrons riding a unit (CK);
  - the command menu embedded in the units (CL).
  The warp now fills under interface from `latched_scene_under`.
- **Mesh shear on diagonal surface edges** ("A shared corner sheared the
  ice", CK cursor down 5). Fixed in `keep_row`: a forced corner is
  fit-tested, and a row also breaks at the cuts of the rows beyond its
  neighbours.
- The user confirmed the three bug fixes (CJ, CK, CL) on the Thor only by
  asking for this commit; a new report followed.

- **The cursor's hand distorting the terrain** (`CM_hand_distorts`, stick
  bottom-left, only at the bottom of the hand's bob). Fixed in the same
  session's last commit (README "The cursor's hand pulled the ground out at
  the bottom of its bob"). Installed on the Thor, **not yet confirmed by the
  user**.

**Open:**
- The mainline idle loop elision (1.5 ms a frame on the Thor, changes guest
  timing): the user's call, not started.
- Still open from before: the torch on a pedestal (CA), the stick ranges'
  verdict, the camera lag on BL.

**How to measure on the Thor** (memory `kol-thor-cycles-per-frame`):
- simpleperf via `run-as`, symbolised with the unstripped lib from
  `android/app/build/intermediates/merged_native_libs/`;
- `/data/local/tmp/thor_stat.sh` for cycles per frame (fps x 10 s);
- A/B: stash, build, install, relaunch with `monkey -p com.koldecomp.game -c
  android.intent.category.LAUNCHER 1`, load a test slot with `input
  keyevent --longpress KEYCODE_F3` (F1 = slot 1);
- save the live game first with `input keycombination KEYCODE_SHIFT_LEFT
  KEYCODE_F3`, after backing the slots up to the session's SCRATCHPAD
  (overwrite one set). Never to `states/backups/`: the user does not want
  backups piling up in the project.

**Diagnostics that found this session's bugs:**
- `KOL_OAM_DEBUG=1` (kept): OAM entries with owners.
- `KOL_PERSP_KIND=1` (kept; the user asked to keep it): the scene painted by
  kind (green flat, red upright) with light/dark depth bands every 8 units,
  parallel on level ground. Render it with `KOL_PERSP_LOOK` and measure the
  band rows per column. Legend in README next to `KOL_PERSP_HOLES`. On the
  Thor: the last BACK-menu toggle, "Debug: perspective mesh".
- Turning corner merging off.
- Probing which triangle covers a pixel.
The last three were temporary patches; the method is in memory
`kol-mesh-shared-corner-shear`.

## HANDOFF — 2026-09-28, night (read this first)

**State.** KoL `widescreen-16-9` tip = the commit carrying this handoff,
gbarecomp `widescreen-bg-y-provider-and-vram-view` tip `54ce493`, both
committed and clean, nothing pushed. The Thor runs the APK built from
exactly this code (installed ~16:40). Run `git status` in both first.

**Committed today** (README sections named; all under "Perspective on the
battle map" unless noted):
- `75f5081` (+ gbarecomp `4f89ed5`, `212ea64`): monsters are 32 wide, unit
  pixels from the latched OAM ("Monsters torn in two"); the cursor's hand
  drawn flat ("The cursor's hand broke").
- `f6e2b48`, `d107207`, `3106503`: second-screen work (unit box hidden with
  the second screen up, SP boxes, SP on the sheet, the move row).
- `b326f0f` + gbarecomp `f183bed`: **units drawn from their own sprites**
  (atlas from latched OAM), user-approved on the Thor.
- This commit + gbarecomp `54ce493`, **not yet judged by the user on the
  Thor** (installed; the user was reporting bugs from it):
  - "The ground under a unit, from the compositor": gbarecomp records the
    no-sprite pixel under every sprite (`latched_scene_under`) and the
    scene's own layer codes (`latched_scene_own_layers`). Lifted unit
    pixels take the real ground (mirror fill kept behind
    `KOL_PERSP_MIRROR_FILL=1`). Every sprite test in kol_focus.cpp reads
    `scene_top()`, so units under the HUD (CB's "5 STEP" bubble, BK's unit
    bar) are whole. Mac +0.12 ms/frame.
  - Same section, "A second Alphonse in the sky" (`CG_alphonse_stretch`):
    the backdrop read the scene before lifted pixels got their ground; the
    kReach outline rule now reads a lifted pixel's under layer.
  - "The menu's hand: interface, and it follows its box" (`CH_menu_hand`,
    also fixes `BV_attack_confirm`): priority-0 OAM is interface, never a
    unit's (claim + iface); `kol::ui_sprite_shift` moves a priority-0 sprite
    lying mostly on an anchored box with it (X and Y providers). The user
    chose to have the hand follow the box. `KOL_WS_NO_UI_SPRITE_SHIFT=1`.
- The earlier "fill widens a shadow (CB)" did not reproduce as a shadow.

**Verified:** both render invariants unchanged (240 `66e13ab2`, anchor-off
284 `9054c456`), also with perspective + DoF forced on. Anchoring A/B: only
the hand changes (CH, BV); BQ, BD byte-identical.

**Open:**
- The user's verdict on today's three fixes on the Thor; Thor cost of the
  under-sprite recording not measured (`thor_cpu.sh`, `thor_fps.sh`).
- A map cursor passing mostly over an anchored panel would move with it
  (ui_sprite_shift); not seen.
- Camera lags a scroll step for ~4 pan frames on `BL_enemy_turn`.
- Torch on a pedestal (`CA_torch`), stick ranges verdict, Thor heat/fan.

**Tools / traps this session:**
- Headless repro: `shots/perspective_edges/render.sh` sets
  `KOL_WS_NO_HUD_ANCHOR=1 KOL_WS_NO_PANEL_PIN=1`. The Thor anchors: drop
  those two for HUD bugs (CH looked fine until they were dropped).
- Magenta diagnostic: paint a fill magenta with a temp env, render at
  several `KOL_PERSP_LOOK` values, read the normal render at those pixels.
- `KOL_SPRITE_DEBUG=1` lists the atlas squares; a temp OAM listing
  (owner/prio/mode per entry) found the hand's priority rule.
- Zoom the Thor screenshot 3-4x around the reported spot before judging:
  a smeared Alphonse read as HUD bars at full size.
- Present dumps are 32-bit BMPs; zsh doesn't word-split `$B` (use
  `bash -c`).

## HANDOFF — 2026-09-28 (read this first)

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed and clean (KoL tip
is this handoff; code at `2aee711`, gbarecomp `ce96ba9`), nothing pushed.
The Thor runs the APK built from exactly that code. Run `git status` in both
before believing any of this. The 2026-09-27 evening block below still holds
for the performance work.

**Shipped since that block** (README, end of "Perspective on the battle
map": "The combat camera", "The right stick moves the camera"):
- **Combat camera** (`f8e9020`, `897a958`): lower + closer while an attack
  plays (guest fn `0x08088912`); sprites near units move with them, rain
  stays flat. User-approved.
- **Right stick camera** (`2aee711` + gbarecomp `ce96ba9`
  `g_host_pad_right_stick`): down lower, up higher, left/right lean; springs
  back; centred = byte-identical. The user has not judged the ranges yet
  (lean 1.2 x half view, K x exp(-0.4 y), pitch 1 - 0.25 y).
- **Two BG1 prop fixes** the stick exposed: no 16-row seed margin above
  props (`BY_tile_sprites`); a cell is a prop only if it rises above its
  ground (`BZ_tile_piece2`).

**Open, in the user's order:**
- **Torch on a pedestal** (`states/CA_torch.state`, cursor on the left
  torch): its BG1 art takes the pedestal's geometry and the back wall is
  drawn as part of the torch's sprite under the stick. Three geometric
  rules tried and removed (README). The user asked to keep the pre-torch
  state. Proposed real fix, not started: learn from the game's data which
  BG1 tiles are props (the game draws them from its own tile graphics).
- The user offered to report more broken tiles: ask for 2-3 DIFFERENT-looking
  cases as save states (slot 1), not a list; each names a rule.
- Parked by the user: rain/snow depth of field (for much later; memory
  kol-weather-dof-parked).
- Still open from before: the fan / GPU (depth of field 18% busy), blur-map
  CPU ~0.9 ms, KoL mesh ~2.1 ms, compositor ~7 ms.

**How the user works now:** reports come as slot-N states; pull with
`adb exec-out "run-as com.koldecomp.game cat files/totkol.stateN"`, name
them `states/C?_*.state`, reproduce with `KOL_PERSP_LOOK=0,1` for the
stick. A Thor screenshot: `adb exec-out screencap -p -d
4630946441858561667`. Commit only when the user asks to.

## HANDOFF — 2026-09-27, evening (read this first)

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed (see `git log -6`),
nothing pushed. The Thor runs the APK built from gbarecomp `17bc430` + KoL
`897a958` (later commits are docs only). Run
`git status` in both before believing any of this.

**This session was performance, on the Thor** (README, end of "Perspective on
the battle map", has every number):
- Scene overscan cost: A/B/A with the user playing, 87% -> 98% of a core.
  Now composed only in the rows that can need it (gbarecomp `255a384`, KoL
  `b44417f`): 94%. `KOL_PERSP_TRIM_CHECK=1` proves nothing left out is seen.
- The warp provider's row passes on a worker pool (KoL `6d5012c`), ~0.9 ms a
  frame on the Thor; byte-identical, `KOL_PERSP_THREADS=0` for serial.
- The warp mesh through raw GL instead of SDL geometry (gbarecomp `29b8f4f`),
  ~1 ms a frame; byte-identical; `GBARECOMP_NO_GL_MESH=1`.
- The disc depth of field's gather at half size + depth-aware composite
  (gbarecomp `17bc430`): Thor GPU 29-30% -> 18% (off: 6-7%). The user
  approved the look. `GBARECOMP_DISC_FULL_SIZE=1`.
- Bug fix (KoL `e6d0609`, user-confirmed): everything blurry and broken on
  moving after free look. The move preview's pale unit copy sits in the unit's slot
  with the unit's real world position, and its camera vote put the map 216
  rows off. The cursor now votes (weight 8). Repro
  `states/BU_freelook_blurry.state` (states/ is gitignored).
- **The combat camera** (KoL `f8e9020`, `897a958`; README "The combat
  camera"): while an attack plays (guest fn `0x08088912`, found by a
  per-frame function-entry trace), K x0.4, pitch 0.75 (lower camera), zoom
  >= what keeps the overscan within rest's (1.63 at 480x270), 14-frame ease.
  Sprites near units move with them as `kSceneOverlay`; raindrops stay flat.
  Repros `BV_attack_confirm`, `BW_spell_confirm` (spells are their own
  pillarboxed scene, left alone), `BX_snow_flakes`. User-approved on the Thor.
- **Parked by the user (for much later):** rain/snow at random depths with the
  depth of field -- built, then reverted. Notes in memory
  (kol-weather-dof-parked), not in the code.
- Where the Thor's guest frame goes now (~16.8 ms CPU before the GPU work):
  compositor ~6.9, game ~4.5, KoL mesh ~2.1, blur map ~0.9, rest ~2.4.

**Methods that worked (use them again):**
- Android cannot take env switches (`std::getenv`; only SDL_ENV manifest
  metadata): A/B = throwaway build, or one build that cycles variants on a
  timer and logs switches (`SDL_Log`), with a 1 Hz sampler in step.
- Per-section wall + thread-CPU timers logged via `__android_log_print` show
  sub-ms costs; the fps/CPU scripts' session noise (+-4 points) hides them.
- Scripts on the device (`/data/local/tmp`): `thor_fps.sh`, `thor_cpu.sh`
  (Thread-4; worker threads inherit that name but come later),
  `thor_proc.sh` (whole process), `thor_gpu.sh` (GPU busy + clock, 10 s),
  `thor_gpulog.sh N` (1 Hz with timestamps). All sleep; never busy-wait.
- Desktop present dumps at `--scale 2` with `KOL_DOF_RADIUS=5` give the
  Thor's disc sizes at a stable window size (scale 4 is clamped by macOS and
  its size varies run to run).

**Open:**
- The user hears the fan with the effects on. Next candidates: the blur map
  on the CPU (~0.9 ms, the focus provider per scene pixel), KoL's mesh
  (~2.1 ms), the compositor (~7 ms, mostly the game's own frame; big).
- The user mentioned artifacts that predate this session; not identified
  yet. The blurred-silhouettes-against-the-sky item is obsolete (user).
- Ghost unit toggle, menu text over a unit: unchanged from before.

## HANDOFF — 2026-09-27 (read this first)

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed this session (see
`git log -3` in each), nothing pushed. The Thor has the APK built from these
commits. Run `git status` in both before believing any of this.

**Shipped this session** (README sections named in brackets):
- **Scene overscan for the perspective** ["Scene overscan: the far side
  reaches the edge", under "Perspective on the battle map"]. With the
  perspective on, gbarecomp composes the HUD-free scene past the view
  (`g_ppu_scene_overscan`; 45 px each side and 32 rows on top at 480x270, from
  K and the view size). The frame and its layer map are cut back to the view
  at latch, so hashes are untouched. `HostWindow::present` now takes a
  `PresentScene`, and `g_present_frame_view` tells providers where the view
  is. The 512-px limit on KoL's batched HUD row path (a +4.7 ms cliff) is 576
  now. `KOL_PERSP_NO_OVERSCAN=1` is the A/B.
- **Camera flip on a blinking unit** ["The camera flipped on a blinking
  unit"]. Only on-screen sprite slots vote for the camera; the last camera is
  kept when none does. Slots are handed out in CALL ORDER, not per unit.
  Repro `states/BT_persp_crazy.state` (the user's slot 1).
- **The blinking deployment unit is drawn at 50% instead** ["The blinking
  unit, half transparent"]: two declining `[[mod_function_hook]]`s
  (`0x080676EC` the blink flip, `0x0805A77C` the draw list) in
  `src/kol_ghost.cpp`, using the game's own semi-transparent flag and its
  BLDCNT/BLDALPHA copies at `0x03000E1A/E1C`. `KOL_NO_GHOST=1` keeps the blink.
  The user chose 50% from a mockup (`shots/transparent_unit/`).
- The second screen's Previous/Next unit buttons are gone (the shoulders do
  it); the sheet is centred vertically.

**Verified:** both render invariants unchanged (240 `66e13ab2`, anchor-off
284 `9054c456`, centre-240 0/160 at 284 and 480x270), also with perspective,
overscan and depth of field forced on. After the ghost unit is placed (A), the
frame is byte-identical to the game without the hooks.

**Device state.** `enhancements.cfg`: 480x270, depth_of_field=1,
perspective=1, dof_radius=2.5. Slot 1 = `BT_persp_crazy` (battle preparation).

**Open:**
- **Thor cost of the overscan is NOT measured.** Desktop headless is +1.1 ms
  a frame (7.8 -> 8.9). Before this the Thor was at 87% of a core, 58-59.5 fps.
  Measure with `/data/local/tmp/thor_fps.sh` and `thor_cpu.sh` (already
  pushed) while the user is on a battle map. If it drops frames, compose the
  side overscan only in the upper rows (the only place it is needed).
- The ghost unit is always on; the user may want a BACK-menu toggle.
- Without GL (Metal on the Mac) the overscanned warp draws without the
  layered blur. Desktop tests use `SDL_RENDER_DRIVER=opengl`.
- Still open from before: blurred map silhouettes stay crisp against the sky;
  menu text over a unit takes the unit's place ("Attack" on `BK`); the
  battle-prep unit-list rows are deprioritised (do not start them).

**Tools made this session** (scratch copies in `shots/perspective_edges/`):
`render.sh`/`compose.py` (present dumps -> side-by-side/zoom PNGs, pure
Python, no PIL). Headless input: `GBARECOMP_INPUT_REPLAY` with lines
`frame,0xKEYS` on the PPU's ABSOLUTE frame count (a state load keeps it, e.g.
210723). `--window` is needed for `GBARECOMP_PRESENT_DUMP`.

## HANDOFF — 2026-09-26, afternoon (read this first)

**State.** KoL `widescreen-16-9` at `f7b3040` + this handoff commit; gbarecomp
`widescreen-bg-y-provider-and-vram-view` at `9106a21`. Both committed and clean,
nothing pushed. **The APK on the Thor is ONE STEP BEHIND**: it was installed
just before the commits, and the Thor then disconnected. It differs only by a
removed mockup debug switch (`KOL_PERSP_BLURVIS`) and a comment. The APK
built from the commits is at `android/app/build/outputs/apk/debug/`; install
it when the Thor is back (`adb install -r ...`). Run `git status` in both
before believing any of this.

**Shipped this session** (README "Perspective on the battle map" and "Depth
of field as lens discs" have the design, costs and failed approaches):
- **Perspective** (BACK-menu toggle "Perspective (farther is smaller)"):
  - The scale follows WORLD depth, `s = K/(K+Dref-D)`, K 3600, so verticals
    stay vertical. This is not the rejected keystone warp.
  - gbarecomp composes a HUD-free frame (`g_ppu_scene_exclude_layers`) and
    draws it through a warp mesh (`g_present_warp_provider`). The HUD is drawn
    flat on top.
  - The mesh is watertight: each corner row is cut once, and both strips
    that share it use the same cuts.
- **Depth of field as lens discs**: a raw-GL shader in `host_window.cpp`
  (`disc::`) that averages a disc of each pixel's own radius. The user said
  the old layered blur looked like grease on the screen.
  - Strength is in BACK > Options > "Depth of field strength" (1.0 / 1.5
    default / 2.0 / 2.5 frame px, `dof_radius=`).
  - It works with and without the perspective. Without GL (Metal on the Mac)
    the old layered blur draws. For desktop tests use
    `SDL_RENDER_DRIVER=opengl`.
- Both effects on, at 480x270 on the Thor: guest thread 87% of a core,
  58-59.5 fps, GPU 27%.
- Both render invariants unchanged: 240 `66e13ab2`, anchor-off 284
  `9054c456`, centre-240 0/160. They also hold with both effects forced on.

**Device state.** `enhancements.cfg`: 480x270, depth_of_field=1,
perspective=1, dof_radius=2.5. The strength was changed from the menu at
12:19, either by the user or by the pad drift; the user was not asked which.
Slot 1 is `states/BS_slot1_glycinia_status.state` (a status window; press B
for the map).

**Open (none started):**
- **The perspective's frame edges.** The shrunken far side has no picture
  beyond the frame, so a map that runs off the top or side ends in a straight
  cut. A real fix renders a source larger than the screen. The renderer caps
  at 480x280 (margins 120/60), and HUD anchoring keys on the frame's edges.
- **The blur's silhouettes.** A blurred part of the map stays crisp where it
  meets the sky.
- Menu text over a unit (the "Attack" on `BK`) takes the unit's place.
- The battle-prep unit list rows were **deprioritised by the user**; do not
  start them unprompted.
- The self-heal misses listed below are still unreviewed.

**Traps paid for this session** (details in the README sections):
- Desktop GL draws nothing unless generic attribute 0 carries the position.
- SDL returns the same GL id for different streaming textures; go through
  render targets.
- GLES2 RGB888 textures are stored R/B-swapped; use BGR888 for anything a raw
  shader reads.
- Never busy-wait on the Thor between measurements (the game fell to 42 fps).
  Use `tools/thor_fps.sh` and `tools/thor_cpu.sh` from `/data/local/tmp`.
- UI automation on the Thor: `uiautomator dump` + `input tap`. Button texts
  are uppercase ("OPTIONS").

## HANDOFF — 2026-09-26 (read this first)

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed and clean, nothing
pushed. The APK on the Thor is built from these tips. Run `git status` in
both before believing this.

**Shipped this session** (each has a README section):
- gbarecomp `c7eedcd` + KoL `3b081b4`, `4957160`: **Depth of field on the
  battle map**, a BACK-menu toggle ("Depth of field (follows the cursor)",
  off by default).
  - Focus: the cursor; else the unit that just moved; else the tile at the
    screen centre.
  - Blur is rows apart plus height apart, sharp within 1.5 rows, ramping to
    the most distant terrain on screen.
  - Four blur levels, HUD kept sharp from a new per-pixel compositor layer
    map (`gba::g_ppu_record_layers`).
  - README "Depth of field on the battle map" has the design and seven
    failed approaches. The Thor has it switched OFF right now.
- **The long-open per-tile ELEVATION is solved.** EWRAM `0x02016440`,
  32x32 cells x 4 bytes (+0 terrain id, +1 height, +2 flags, `0x80` = on
  the map). The README section also has the object world positions, the
  exact isometric projection, the cursor's sprite slot (routine
  `0x08057AD5`) and the per-map camera constant.
- KoL `fa1c3ed`: **ARMS window** no longer rises into the top margin. The
  vertical anchoring rule now has the same width cap as the horizontal
  one; full-width bars are exempt.
- KoL `db68070`: **flash on select and cancel** (terrain in the bottom
  margin for one frame) fixed. The still-camera grace in
  `kol_map_source.cpp` now accepts agreement down to 25%, was 80%.
- gbarecomp `adaae74`: `GBARECOMP_FRAMEDUMP_DIR` now also dumps in
  present-in-place mode. Before, it silently wrote nothing there.

**Decided, do not re-propose** (memories exist):
- A perspective/keystone warp of the battle map was built, tried on the
  Thor, called ugly, and reverted.
- 3D maps: a two-map viewer exists
  (https://claude.ai/artifact/Y9L7ujBaV1UAFRAMbuuGTU). ROM-wide extraction
  and playing in 3D are PARKED as too much for now.

**Open, waiting on the user: more rows in the battle-preparation unit
list** at 384x216 and 480x270. Investigation only, nothing changed:
- List state struct at EWRAM `0x02037B40`:
  - `+0x00..0x0F` roster order;
  - `+0x12` count;
  - `+0x2C` cursor slot in page (row*2);
  - `+0x2E` scroll row;
  - `+0x43` is the scroll ANIMATION length (4), NOT the row count.
- The page size is hard-coded. The input handler at `0x0807EB58` uses
  `cmp r3, #5` at `0x0807EC14`, `0x0807EC3A` and `0x0807EC4C` (4 rows x
  2), overridable with the existing `[[thumb_alu_immediate_override]]`.
- Still to find: the draw loop's row count, and the portrait ring in OBJ
  VRAM (10 slots of 16 tiles from tile 48).
- Rows are 40 px apart and portraits 32 px tall. Only 10 fit (384) and 12
  (480), and only if the list moves UP into the top margin. That touches
  HUD anchoring, so the user was asked before going ahead; no answer yet.
- The HUD tilemap has 32 rows, so at 270 extra frame rows may wrap into
  the top margin.

**Repro states** (`states/`, local only):

| State | What it shows |
|---|---|
| `BK_tree_glass_cage` | DoF, tree props |
| `BL_enemy_turn` | DoF, enemy turn, camera pans |
| `BM_tree_broken` | DoF, tree art on BG0 |
| `BN_water_focus` | DoF, ruins |
| `BO_grimal_alphonse` | DoF, rows plus height |
| `BP_side_bar` | battle-prep list, 384x216 |
| `BQ_weapon_window_high` | ARMS window |
| `BR_flash_on_select` | the one-frame flash; A on Alphonse, then B, B |

**Device state.**
- Slot 2 is `AZ_alphonse_mismatch` (roster, Lv 50). The earlier slot 2
  backup was deleted on 2026-09-29 at the user's request.
- Slot 1 is whatever the user saved last; pull it before using it.
- `enhancements.cfg`: view 480x270, depth_of_field 0.

**Tools** (the scratchpad is gone; recreate as needed):
- TCP rig client: load a state, set keys, step, read memory. Watches:
  `GBARECOMP_READ_WATCH="lo,hi,max"` gives pc/lr per read.
- THUMB disassembly: `clang -target armv4t-none-eabi` over an `.incbin`,
  then `/Library/Developer/CommandLineTools/usr/bin/llvm-objdump
  --triple=thumbv4t-none-eabi`. There is no Capstone or PIL on this Mac, so
  write PNG and BMP helpers by hand.
- Frame sequences: `--window` + `GBARECOMP_INPUT_REPLAY` (absolute frames;
  `BR` loads at 146601) + `GBARECOMP_FRAMEDUMP_DIR/_START/_COUNT`.
- One window frame, present-time effects included:
  `GBARECOMP_PRESENT_DUMP=<bmp>` + `_AT`. Per-pixel DoF maps:
  `KOL_FOCUS_DUMP=<prefix>`.
- On the Thor, `adb shell input keyevent --longpress KEYCODE_X` is A and
  `KEYCODE_Z` is B; both reach the guest now. Also check `map_source`
  lines in logcat.

**Traps paid for this session:**
- SDL GLES2 on the Thor: RGBA render targets read back with alpha 0. Use
  RGB888 targets. Desktop does not reproduce this.
- `GBARECOMP_FRAMEDUMP_*` needs `--window`. Its frame number is the
  absolute PPU frame.
- zsh does not word-split `$var` in `env $extra`, so pass variables
  separately.
- The battle BG is 512x256: the vertical wrap is 256, not 512.
- Another self-heal miss seen in battle: `0x08076524` near
  `gf_tfunc_0807650A`, not reviewed. The older three are still open.

## HANDOFF — 2026-09-25, night (read this first)

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed and clean, nothing
pushed. The APK on the Thor is built from these tips. Run `git status` in
both before believing this. The evening block below still holds for the
earlier work of the day.

**Fixed tonight**, each reported by the user from their slot 1 and each with
a README section:
- `d3944ae` Units on the deployment tiles were carried behind the anchored
  roster panel. Portraits flashed over the map, and the margins flashed,
  when view mode (L) slid the panel. The map source now has a 2-frame grace
  when the camera is still.
- `568c306` While scrolling in view mode, units flashed in the sky. OAM sits
  halfway between two slot-list samples, so a 16x16 sprite within 16 px of
  a recorded unit now picks the wrap.
- `f06e08f` Inside the panel footprint only EXACT matches count as world
  units. Portraits are 16x16 too, and they floated (Deneb).
- `a048e1c` The support gauge gate was a typo: it is `0x020011CC` bit 0,
  not `0x020211CC`. Also, battle Next/Previous now reloads the window
  palette (`0x0802A7A0(0)`).
- `983e95f` An OBJ window with no sprite in it no longer pillarboxes the
  battle map. Prep stayed at 240x160 after Organize.
- The commit after `983e95f`: a HELP card gets its 4 px page gap only after
  a page that ends with a period.

**Repro states** (`states/`, gitignored, this Mac only):

| State | What it shows |
|---|---|
| `BD_units_invisible` | units carried behind the panel |
| `BE_scroll_left` | hold Left in view mode |
| `BF_panel_brackets` | selection brackets on the panel |
| `BG_gauge_palette` | support gauge; palette after Next |
| `BH_deneb_floating` | Deneb's portrait over the map |
| `BI_organize_return` | A on Organize, B, B |
| `BJ_help_spacing` | HELP card spacing |

**Tools rebuilt this session** (the scratchpad is gone; recreate as needed):
- Capstone in a venv, for Thumb disassembly with literal pools resolved.
  It decodes `bl` targets correctly, which the generated-code comments do
  not.
- PIL (Pillow) for frame strips.
- `KOL_VIEW_SIZE=480x270 GBARECOMP_VIEW_EXTRA_Y=55 --view-width 480` drives
  the per-frame size policy headlessly.
- `GBARECOMP_INPUT_REPLAY` needs absolute frames (a load's frame is printed
  as `savestate_loaded … frame=N`). SELECT is `0x3FB`, L `0x1FF`, R
  shoulder `0x2FF`.

**Traps paid for:**
- Near-matching sprites by position will grab UI sprites of the same size.
  Scope it (footprint, size) and re-check the panel states.
- Temporary traces: grep for `TMP` before every commit (one slipped into
  `d3944ae`).

**Open:**
- The earlier evening items (unverified help rows, an enemy-unit Next,
  where the grid cursor goes after Next).
- While scrolling, a unit standing in the panel's native footprint follows
  the panel for those frames (exact match only there).
- Battle self-heal misses `0x08007698`, `0x0800771C`, `0x080079D0` and
  `0x080124BC` (not reviewed).

## HANDOFF — 2026-09-25, evening

**State.** KoL `widescreen-16-9` and gbarecomp
`widescreen-bg-y-provider-and-vram-view`, both committed and clean, nothing
pushed. The APK on the Thor is built from these tips. Run `git status` in
both before believing this.

**Shipped today** (each has a README section):
- `715f256` **HELP text on Screen-2.** A card floats at the top of the
  sheet, sized to the text, over the rest dimmed to 9/16. It covers:
  - equipment and talent slots;
  - the equipment list (sub-state `0x1D`) and the spellbook list (`0x1F`);
  - every row of SELECT's help cursor (`ctl+0x61A6 != 0`), in the roster
    and in battle. `0x0802ABE0` is transcribed; see README "Every HELP text
    on the second screen".
- `9118252` **Class-change grid** (sub-state `0x21`). The card follows the
  cursor; the highlighted class is `ctl+0x58C3`.
- `815d683` **The Next-unit fix.** In the roster, the window's unit is
  derived from a list position (`ctl+0x5ECB` into `ctl+0x58FC`). The hook
  now moves the position too; before, class change went back to Alphonse.
- gbarecomp `ff1662e` + KoL `7dc2a78` **Controller.** GBA R is on the top
  face button and L on the left one (the Thor reports Nintendo labels, so
  these are SDL X and Y). The R/L shoulders are Next/Previous unit. The new
  seam is `g_host_pad_binds` + `g_host_pad_button_hook`.
- The commit after `9118252`: the remap is a BACK-menu toggle
  (`pad_remap`, on by default).

**Repro states** (gitignored; on this Mac only):
- `BB_spell_change` (spellbook list);
- `BC_class_change` (class grid, Shiven);
- `BA_equip_cursor` (equipment list; its list position is stale, from
  before the fix);
- `AY_kamui_talents` (battle);
- `AZ_alphonse_mismatch` (roster, view-only).

In the organize window, the R shoulder opens the icon menu.

**Open:**
- **Not verified against the game:** the leader/guest No. texts (503/504),
  the special class ids 364-370, the virtue/bane element rows, and the
  snowfield and battle-entry adjustments to the MOVE icons.
- **Not tried on the device:** the pad toggle switched off and on.
- **Next unit:**
  - a window opened on an ENEMY unit (no repro);
  - after closing, the grid cursor stays on the unit the window was opened
    from. Moving it is a further change: ask.
- **Battle self-heal misses:** `0x08007698`, `0x080079D0`, `0x080124BC`
  (not reviewed).

## HANDOFF — 2026-09-25 (earlier the same day)

**State.** KoL `widescreen-16-9`, committed (the unit switch in `bb5e638`,
the HELP text in the commit after it); gbarecomp unchanged. Nothing pushed.
The APK on the Thor is built from this tree. `git status` in both first.

**DONE: Next/Previous unit INSIDE the status window**, roster and battle
(README "Next unit, inside the status window"). The previous handoff's
blockers:
- **Self-heal misses reviewed.** 0x08034498, 0x08040D9C and 0x0803A8F8 are
  real entries. The other six (0x08035F9C, 0x0802B73C, 0x0802B75E, 0x08040D38,
  0x08041146, 0x08041270) are return points after a `bl`, not entries. The
  first two sit in a menu task table at 0x0860D77C, whose 23 routines were ALL
  non-static. All 23 plus 0x0803A8F8 are now `[[extra_func]]`. The roster
  state now runs with 0 interpreted instructions (was 725k per 120 frames);
  the battle state runs 1,160 (was 735k). Its three remaining misses are
  0x08007698, 0x080079D0 and 0x080124BC, not reviewed.
- **The battle window is drawn by the same roster task 0x08034498**; there is
  no separate battle builder. The hook repoints `+0x5854`/`+0x5860` and resets
  the state byte to 0. In battle it then tail-calls 0x0803A964 once to refresh
  the sprite picture.
- The disabled `KOL_STATUS_SWITCH` experiment on 0x0803A0D0 is removed. That
  hook now only records the window's unit for the second screen.
- JNI `nativePreviousUnit`, and a "Previous unit" button on the left of the
  StatsView band.
- Switch: `KOL_NO_STATUS_SWITCH=1`. Trace: `KOL_STATUS_TRACE=1`.

**Measurement trap:** on this Mac, runs that warm-load `recomp_cache/` began
taking over 30 s in `dlopen`, which looks like a hang. Use
`GBARECOMP_SELFHEAL_RECOMPILE=0` for headless checks. Input for headless
runs: `GBARECOMP_INPUT_REPLAY=file` with `frame,0xKEYS` lines. Frames are
absolute (AZ loads at 6085, AY at 271763). Battle B reopens only after the
close transition, about 200 frames after closing.

**ALSO DONE: HELP text on Screen-2.** It floats as a card at the top, sized
to the text, over the sheet dimmed to 9/16.
With the status window's cursor on equipment (an equipped slot, or the
equipment list), the top card shows the item's HELP text, the one SELECT
shows. Its repro is `states/BA_equip_cursor.state` (the user's slot 1). The
change is read-only, in `kol_unit_data.cpp` (`cursor_item`, `help_text`) and
`kol_sheet.cpp` (`help_card`). README "Item descriptions on the second
screen". Talents are done the same way (talent slots from the menu's
spellbook icon; repro in the README). The spellbook list (sub-state
`0x1F`) is verified from `states/BB_spell_change.state`. **Every
other HELP text too, roster and battle:** while SELECT's help cursor is up
(`ctl+0x61A6 != 0`), the card shows the text of the row under it (stats,
class, birthday with its fortune, element, MOVE icons). README "Every HELP
text on the second screen" has the table and what is unverified.

**Open:** a window opened on an enemy unit (no repro state). After closing,
the roster grid cursor stays on the unit the window was opened from; moving
it to the last unit viewed would be a further change, so ask first.

## HANDOFF — 2026-09-24, evening (read this first)

**State.** KoL `widescreen-16-9`, this handoff commit as the tip. gbarecomp is
clean and unchanged today. Nothing is pushed. The APK on the Thor is built
from this commit.

**Shipped today** (each has a README section):
- the "Next unit" button, through the game's own selection (`0x08074338`),
  with a kept index, a D-pad-lock fix and a 2 s reset;
- talent icons from ROM;
- POW and MP side by side, with the TALENT card 2 columns wider;
- made items (`0x02000D30` RAM table);
- per-picture item palettes;
- the ATK/DEF 999 cap;
- the talents moved 4 px left.

The user's downloaded save (`.sps`) was converted and installed as
`files/totkol.sav`. Backups are in `states/saves/`. `*.sps` is in
`.git/info/exclude`.

New repro states: `states/AY_kamui_talents.state` (battle, Kamui's status
open) and `states/AZ_alphonse_mismatch.state` (army roster, Alphonse's status
open, Lv50 endgame save).

**UNFINISHED: Next/Previous unit INSIDE the status window.** The user asked for
this as a QoL feature the game lacks, and wants "Previous unit" too.

The code is in `src/kol_actions.cpp` (`on_status_task`). It is a reviewed hook
on the status task `0x0803A0D0`, whose state byte is in r1: 3 = window up,
0 = init. It is **OFF unless `KOL_STATUS_SWITCH=1`**, because restarting that
task from 0 blacks the screen in both contexts.

What is known:
- **Control block:** `ctl = [0x02002B1C]` (`0x02004854`).
  - `+0x5854` is the unit record.
  - `+0x5860` is the battle entry (`0x02001AA0 + idx*0x1C`), or 0 in the roster.
  - `+0x61A4` is a sub-state: 1 = up, 0x1B = equipment-change build.
- **Roster (PROVEN, test-only poke):** set `+0x5854` to the next army record,
  then set the roster status task's own state byte (`0x08034498`'s r1, was
  `0x020096BC`) to 0. The game rebuilds the window cleanly: "NO. 2", portrait,
  items, talents.
  - Task state 4 with sub `0x1B` gives the equipment-change screen instead.
  - Task state 1 misses the portrait.
- **Blocker:** `0x08034498` and the window-content code are NOT in the static
  build. The miss log proposes `[[extra_func]]` entries for `0x08034498`,
  `0x08035F9C`, `0x0803A8F8` (bridged ~1/frame while a window is up),
  `0x0802B73C`, `0x0802B75E`, `0x08040D38`, `0x08040D9C`, `0x08041146` and
  `0x08041270`.
  - On Android these run INTERPRETED today, a perf gap too.
  - Review them, add them to `game.toml`, regenerate, re-check both
    invariants. Then hook `0x08034498` for the roster.
- **Battle:** the window is opened by `0x08039D38(unit, entry)` from the B
  handler at `0x08048414`, which then registers task slot `0x03002990`. Its
  content builder is not found yet. Candidates are the callers of the window
  builder `0x0803F298` (`0x08032CCC`, `0x0803339C`, `0x08033DC8`); none of them
  is static either.
  - Fallback the user was offered: automate the game's own close + reopen
    through hooks. Visible, about 1 s.
- **Unit order:**
  - Army: 32 slots from `0x02000030`, stride `0x68`; empty = class byte
    `+0x4F` == 0 (the grid shows 0-15, then 20).
  - Battle: 8 per side from `0x02001420`; a unit is on the map when `+0x4E`
    and HP are nonzero.
  - `neighbour()` implements both.
- **Map "Previous unit"** works (`0x08074338(1)`, `action_previous_unit()`),
  but there is NO Java button or JNI for it yet.
- **Second screen:** `unit_for_highlighted()` already prefers
  `status_window_unit()` while a window is up, so it will follow a switch.

**Tool notes from today:**
- `g_runtime_fn_entry_hook` does NOT fire in interpreted or self-healed code.
  Run with self-heal ON for probes, or add the function statically first.
- `GBARECOMP_READ_WATCH` found the picture-palette byte and the talent
  fields.
- `WRITE_WATCH` does not see DMA (the status tilemap is DMA'd from EWRAM
  `0x02006D58`) or IWRAM (the OAM shadow at `0x03000038`).
- TCP dump helper: `--tcp 19842`, then `savestate_load` and
  `read_vram/io/pal/ewram/iwram/oam`. The server needs ~20 s to start.

## HANDOFF — 2026-09-24 (read this first)

**State.** KoL on `widescreen-16-9` and gbarecomp on
`widescreen-bg-y-provider-and-vram-view`, each tipped by this session's
world-shadow commit (parents KoL `b7a2695`, gbarecomp `9f28609`). Nothing
pushed. The APK on the Thor is built from these. The user reported on the
previous APK: the save slots work and 480 performance much better.
Earlier work is in the 2026-09-23 block below.

**DONE 2026-09-24: the highlight stays see-through while scrolling.** The
shadow is keyed on WORLD cells now, filled from the composed EWRAM map, and
placed through a new gbarecomp seam, `g_ws_bg_underlay_sample_provider`. It
resets on every state load (`g_runtime_state_epoch`). README "Route 2, keyed
on the world" has the numbers: 0 ring/world disagreements, both invariants
unchanged, +0.04-0.06 ms/frame. The user confirmed it on the Thor. Switch:
`KOL_WS_NO_HIGHLIGHT_WORLD=1`. The repro is `states/AV_pre_range.state`
(the user's slot 2). To drive it headlessly, press A, then a SECOND A to
close the NAVI prompt; the D-pad does nothing until then. Still true: a
state loaded INTO an open range stays opaque until the range is cancelled
once. The user asked about sourcing originals from the ROM. It would not be
cheaper (same table, same lookup), and it is the tile-generator hunt, not
funded. Injected adb key events did not reach the guest on the Thor this
session.

**BACK menu now has "Save states"** (gbarecomp `runtime_request_state_save/
load`, JNI `nativeStateSave/Load`): save to or load slots 1-3, each shown with
its file time. Verified on the Thor: the dialog renders, and "Save to slot 3"
rewrote `files/totkol.state3`. Slot 3 now holds the 2026-09-24 00:16 battle;
its old contents are `states/AC_viewmode_cut.state`. Slot 2 is the user's
pre-range battle, pulled as `states/AV_pre_range.state`. The pad overwrote slot 1 again at 00:08; the user's 23:07
battle is `states/AU_thor_battle_perf.state`.

**Tools from this session** (scratchpad is gone; recreate if needed): ms/frame
= difference of `--frames 200` and `800` runs with
`GBARECOMP_SELFHEAL_RECOMPILE=0`, and for a REAL 480x270 add
`KOL_VIEW_SIZE=480x270 GBARECOMP_VIEW_EXTRA_Y=55 --view-width 480`. Single
runs drift +-0.4 ms: repeat three times, alternating builds. Thor: CPU of
`Thread-4` from /proc stat, fps from `dumpsys SurfaceFlinger --latency
"'<(BLAST) layer>'"`; under fast-forward real speed = fps x 4 / 60. ROM
disassembly: wrap bytes with `.incbin` in a `.s`, `clang -target
armv4t-none-eabi -c`, then `objdump -d --triple=thumbv4t-none-eabi`.

**Also still open:** naming guest functions in `game.toml` (candidates:
`0x08076810` obj_emit, `0x080005A0` vblank_frame_handler, `0x0800056E`
main_task_loop calling `0x08000768(0x03002910, 4)` — which the finder folded
into an entry at `0x0800075E`); the next compositor step (bulk handling of
skipped-pixel runs, ~10-15%); KoL's mainline scheduler loop (~25% of the frame,
a guest-behaviour change, the user's call); MOVE; panel icons and portrait;
the bank-5 tint toggle. DEF/M.DEF are DONE (README "Defence — solved"):
decoded from the game's routine at 0x08139FCC, 32/32 against the sheet, on
the panel. The user's slot 1 of that session is
`states/AW_alberich_mdef.state`. Element and alignment are DONE too (record +0x54/+0x55;
the game's own icon and letter on the panel, the art read from VRAM under a
checksum, README "Element and alignment"). Unit sprites now take their true X
from the guest slot list (README "Corrected 2026-09-24"; repro
`states/AX_units_left_margin.state`). The second screen is now the game's own status
sheet rendered from ROM (README "A7 archives" and "The status sheet, from
ROM"): the "bespoke codec" is cracked and the font is decoded. Open next:
the unit's walking sprite in the portrait slot, Move, and the unit number. The rain now covers the whole widened screen (README
"Weather across the whole screen"; new gbarecomp seam
g_ws_obj_margin_repeat_provider). **The 284 invariant is now 9054c456
(rain in that state's margins); KOL_WS_NO_WEATHER_REPEAT=1 gives 84a95b05.** Ask before changing HUD anchoring.

## HANDOFF — 2026-09-23 (read this first)

**This session was 480 performance, driven by fast-forward.** The user saw 480
run much slower than the lower resolution, mostly under fast-forward. Measured
on the Thor (battle, `states/AU_thor_battle_perf.state`, pulled from slot 1):
1x 59.5 fps at 85.6% of a core; held 4x fast-forward ran at **1.28x** with the
guest thread at 99%.

**Three changes, committed, verified byte-identical, NOT pushed:**
1. **Fast-forward composes only the frames it presents** (gbarecomp
   `GbaPpu::set_render_skip`, latch-keyed presentation in runtime.cpp).
   A frame is composed whole or skipped whole (decided at line 0); a skipped
   frame does not latch. Thor: **4x FF 1.28x -> 2.67x**, 1x unchanged.
   `GBARECOMP_FF_RENDER_SKIP=0` reverts; `GBARECOMP_RENDER_SKIP=N` forces it
   headlessly for A/B. Composed frames verified byte-identical to baseline after
   600 frames of skipping (S_move_highlight, AU).
2. **HUD anchoring answered a row at a time** (KoL `anchored_ui_row`,
   `anchored_x_row`, `world_x_row`). The row provider was still a per-pixel
   loop over up to nine candidates: **39.5% of the frame** at a real 480x270.
   Same decisions by construction, NO heuristic changed. `KOL_BGX_ROW_CHECK=1`
   checks every row against the per-pixel function and aborts on a mismatch:
   300k-400k rows clean on AU, G_panel_left, AD_user_charsel, AI_prep_vertical
   at 480x270 and 284x160.
3. **Wide compositor candidates carry no RGB copy** (converted once, for the
   winner). Small.

Desktop, AU at a REAL 480x270: **11.03 -> 6.99 ms/frame** 1x; **3.98** per guest
frame under 4x FF. The last APK (all three) is installed; the user has NOT yet
tested it — ask what they saw.

**Two recorded beliefs were wrong — README "Cost" has both:**
- **Height is not free.** Headless runs applied the 270-row view only at exit,
  so every desktop 480x270 number before today was 480x160. Use
  `GBARECOMP_VIEW_EXTRA_Y=55` with `KOL_VIEW_SIZE=480x270`.
- **The 284 reference `84a95b05` is the anchor-OFF dump.** Anchor-on is
  `22896dfb` on old and new builds.

**Where the frame goes now** (AU, 480x270, 1x): regular-BG pixel loop ~27% +
compositor body ~12%; KoL's mainline task-scheduler loop ~25%
(`runtime_should_yield` + `runtime_tick`, same at every width — it stores to its
own stack every iteration, so framework idle elision never fires; skipping it
changes guest execution, so it is a reviewed mod_function_hook and the USER's
call); margin `map_source_entry` ~10% (per pixel; per tile needs a new PPU
contract). The user asked to WAIT for instructions before the next target.

**Later the same session — the per-tile compositor (committed).** In the wide
regular-BG loop the map lookup, the margin provider and the VRAM tile fetch
are resolved once per 8-pixel tile; everything genuinely per pixel (window
gating, X/Y remaps, underlay, submit) is unchanged. The margin provider is
reused within a tile only under the new optional seam
`g_ws_tilemap_provider_x_phase`, when the game's declared grid phase equals the
line's HOFS & 7; KoL declares `g_fine_x` (its composed-map column is
floor((hw_x + fine_x)/8)) and the line's own phase for every branch that does
not look at hw_x. `GBARECOMP_WS_TILE_CACHE_CHECK=1` re-resolves every pixel the
old way and aborts on a difference: clean on AU and AS_scroll_flash at 480x270
and AS at 284. Per-row constants are also hoisted out of the loop. AU at a real
480x270, three alternating runs each: **7.08 -> 6.36 ms/frame** (11.03 at
session start). Single msf.sh runs drift +-0.4 ms — repeat and alternate.
What is left in that loop has no hotspot; the next step there is handling
runs of skipped pixels in bulk.

## HANDOFF — 2026-09-22 (read this first)

**This session was performance. Two changes, both committed, both verified.
The frame got ~36% cheaper at 240 and ~24% cheaper at 480 before the second
change, which is not yet measured.**

**FIRST THING NEXT SESSION: measure the APK on the Thor.** It was built and
installed from these commits but the user tested it manually and that result is
not recorded here. Get the two numbers before doing anything else — the method
is in README "Cost" and needs no rebuild:
```sh
# fps: layer name from `adb shell dumpsys SurfaceFlinger --list`, the (BLAST) one
adb shell dumpsys SurfaceFlinger --latency "'<layer>'"     # diff the middle column
# CPU: the guest thread is Thread-4
#   read $14+$15 of /proc/<pid>/task/<tid>/stat against /proc/uptime
```
Before this session the Thor sat at **99.0% of one core** while presenting
**59.1 fps** at 480x270. Zero headroom. That single number is the whole reason
480 felt slow and fast-forward barely helped — fast-forward runs N
guest frames per presented frame and there was no spare CPU to run them. The
question now is what those two numbers became.

**The README "Cost" table was a measurement artifact and is now corrected.** It
timed whole headless runs, and ~21 s of startup `dlopen` for 1554 warm overlays
swamped the signal — it reported +7% for widening to 426 when the real cost of
240 -> 480 is **+52%**. Difference two frame counts (200 and 800) so startup
drops out, and set `GBARECOMP_SELFHEAL_RECOMPILE=0` (that tier is off on Android
anyway and changes steady-state cost by nothing). See [[kol-perf-measurement]].

**Change 1 — the IWRAM code is compiled now, not interpreted (KoL `e0eea61`).**
Every IWRAM PC the interpreter bridged is a byte-for-byte copy of ROM, so all of
them compile ahead of time. Four `[[code_copy]]` blocks, eight `[[extra_func]]`
entries and one `[[resume_range]]` in `game.toml`. A 600-frame battle run went
from **15,383,255 interpreted instructions to 51,750**, and `AF_world_map` now
reports `FULLY_STATIC dispatch_misses=0 interpreted_insns=0`.

This CORRECTS a call recorded in `game.toml` on 2026-09-17 that RAM code
"belongs to the self-heal RAM-dispatch tier ... there is no fixed ROM byte range
to decode". There is one, and `[[code_copy]]` is the schema's mechanism for
exactly it. That tier also cannot run on Android at all (the platform blocks
`dlopen` of an app-written `.so`), so the cost was ~3.5 ms of every frame, at
every view size, permanently, on the target device. README "The interpreter was
half the frame" has the block table and four traps. See
[[kol-iwram-code-is-rom-resident]].

**Change 2 — the BG X remap is asked a row at a time (gbarecomp `0e92dbc`, KoL
`f50fce1`).** The expanded renderer called `g_ws_bg_x_provider` once per BG layer
per output column per scanline: **307,222 indirect calls a frame** at 480 wide
(measured 76,806 per layer per frame, four layers), and the callee alone was
**26.8% of total frame time**. New optional seam `g_ws_bg_x_row_provider` asks
for a whole output row in one call — 640 a frame. KoL implements it as a plain
loop over the existing per-pixel function, so the answer is identical by
construction rather than by review, and the HUD anchoring rules are untouched.

**Change 2 is verified but NOT yet measured.** All three invariants pass on the
final commit (240 `66e13ab2…`, 284 `84a95b05…`, centre-240 identity 0/160), and
900-frame dumps at 480x270 are byte-identical to the previous commit's binary on
`S_move_highlight`, `AT_status_sheet`, `AS_scroll_flash` and `AD_user_charsel`.
The desktop ms/frame number was never taken — the run was interrupted. Take it.

**What the profile looks like now, and where the next win is.** After change 1
and before change 2, at 480x270: `render_scanline_wide` **58.3%** of frame, of
which `kol_bg_x_provider` **26.8%**, `map_source_entry` 2.9%, `kol_bg_y_provider`
1.2%, interpreter **0.2%**. Change 2 attacks the 26.8%. What remains after it is
the compositor's own per-pixel work — the wide path costs **~55 ns/px against
the narrow path's ~21 ns/px**, a flat 2.6x that is NOT margin-specific (fitted
across five widths; switching to the wide path costs 1.34 ms/frame even at zero
margin). That flat 2.6x is the next target and it would help every width.

**Three measurement traps this session paid for, all in README "Cost":**
1. **Startup dominates short headless runs** — see above.
2. **Never sample two configs in parallel.** They contaminate each other's
   shares. Run `sample` sequentially.
3. **Width is expensive, height is free, and the reason is not what it looks
   like.** +240 columns costs +3.4 ms; +110 rows costs 0.03 ms. Not because
   margin rows are cheap — because they are **rendered once and cached, not per
   frame**. Counted directly: 480x270 makes 96,277 wide scanline calls over 600
   frames against 480x160's 96,007, a difference of 270, not 66,000. Do not
   model this cost per-pixel. `dumpsys gfxinfo` is also NOT the emulator's frame
   rate — it measures the HWUI second-screen panel.

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, `gbarecomp` on
`widescreen-bg-y-provider-and-vram-view`. Nothing pushed. `git status` in BOTH
before believing it. **`generated/` is gitignored and `game.toml` changed**, so
a fresh clone MUST re-run
`../gbarecomp/build/gba_recompile --rom totkol.gba --config game.toml --out generated`.

**Build gotcha, newly paid for.** `android/gradlew` resolves its distribution
through `/private/tmp/gradle-8.7`, which macOS cleans. When it dies with
`NoSuchFileException: /private/tmp/gradle-8.7/lib/plugins/...`, relink it:
```sh
ln -s ~/.gradle/wrapper/dists/gradle-8.7-bin/*/gradle-8.7 /private/tmp/gradle-8.7
```

**Still open, unchanged by this session:** the displayed melee/magic DEFENCE
pair and MOVE; the panel's item icons and portrait; the optional live tint
toggle (bank 5 at `0x0861E008`). And **ask before changing the HUD anchoring
heuristics** — that is still where this project's regressions come from.

## HANDOFF — 2026-09-21, night (read this first)

**SCREEN-2 CAN BE CAPTURED. Every earlier handoff saying otherwise is WRONG.**
`adb exec-out screencap -p -d 4` returns an empty file, which is where the
belief came from — but `-d` wants the PHYSICAL display id, not the logical
one. Get it from `adb shell dumpsys display` (`uniqueId="local:<id>"`), then:

```sh
adb exec-out screencap -p -d 4630946482288158084 > s2.png   # Screen-2, 1240x1080
adb exec-out screencap -p -d 4630946441858561667 > s1.png   # main screen
```

This project spent several sessions asking the user to be its eyes for a
panel nobody could see, and shipped a layout with three visible defects
because of it. **Look at the panel before changing it, and after.**

**The second screen now draws the game's own character sheet.** Parchment
field, blue-grey double border, corner studs, slate header plates, EQUIP. and
TALENT as two cards — and the frame is the cartridge's OWN artwork, twelve
tiles read straight from the ROM by address with no capture and no
decompression. README "Done: the frame IS read from the ROM" has the twelve
addresses and palette bank 4 at `0x0861DFE8`.

**Three traps that panel cost, all in README:**
1. **Per-tile bitmaps, never sub-rects of an atlas.** Scaling a src rect lets
   the sampler reach the neighbour's edge column and rules a faint vertical
   seam at every tile boundary.
2. **Derive the content inset from the frame.** A fixed 34px inset against a
   border a whole tile thick drew every label on top of the frame.
3. **Anchor the 9-slice's edges flush to the far side.** A grid from the near
   side overshoots unless the card is an exact multiple of the tile; the clip
   then eats the right and bottom borders.

**Equipment names have text now.** 193 of the 489 pool strings are pure
`0x81` bitmap strips with NO text form in the ROM, which is why one slot read
"Short Sword" and the rest were small images. `kol_unit_data.cpp` carries a
transcription table; `tools/dump_name_strips.py` regenerates the sheets it
was read from, so a wrong name is a transcription error to fix in the array.

**Margin units are FIXED and it was three bugs** — the guest's own OBJ cull,
OAM's 9-bit X wrap, and OAM's 8-bit Y wrap. See [[kol-obj-margin-cull]] and
the README section; do not reintroduce the top-slack cap, and sample the Y
table once per frame.

**The terrain box is FIXED** — it is 9 tile rows against an 8-row cap, and
its element dots are a separate BG component at col 24 that the edge test and
containment both miss, anchored now by the box's known rect.

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, this handoff as
the tip. `gbarecomp` on `widescreen-bg-y-provider-and-vram-view` at
`f529fd2` (one new seam, `g_ws_obj_attr_y_provider`). Nothing pushed.
`git status` in BOTH before believing it.

**Device state.** slot 1 = `AT_status_sheet` (the character sheet, the
reference for the panel's look), slot 2 = `S_move_highlight`, slot 3 =
`AC_viewmode_cut`. `states/` is gitignored; `AA`..`AT` exist only on this
machine. `enhancements.cfg` is `width=480 height=270`.

**Open, in the user's priority order:** the displayed melee/magic DEFENCE
pair and MOVE (still not in the record, still not captured); the panel's
remaining artwork — item icons and the portrait — which needs either a VRAM
capture or cracking the tile codec (README has how far that got); and an
optional live tint toggle, the game's own bank-5 green being the authentic
alternative to bank 4.

## HANDOFF — 2026-09-21, late evening (read this first)

**Units in the extended view's margins are FIXED, and it was THREE bugs.** The
previous handoff below has the hunt; this is the answer. README "Units did not
appear in the margins — SOLVED" has the full derivation, the dead ends and the
verification table. Do not re-derive any of it.

1. **The guest culls its own sprites to 240x160.** Its generic OBJ emitter,
   ROM `0x08076810`, rejects anything outside `X in [-32, 272]`,
   `Y in [-16, 192]` BEFORE writing the sprite, so nothing reaches OAM. That
   `-32` is exactly the boundary the previous session measured. The window is
   four THUMB immediates; `game.toml` routes those four — and only those four
   — through the framework's `[[thumb_alu_immediate_override]]` hook, and
   `src/kol_obj_cull.{h,cpp}` widens them to the margins actually rendered
   this frame. **`generated/` must be regenerated** for this
   (`../gbarecomp/build/gba_recompile --rom totkol.gba --config game.toml
   --out generated`); it is gitignored, so a fresh clone needs that step.
2. **OAM's X field is 9 bits, decoded SIGNED.** Widening alone fixed the left
   margin and did nothing for the right: a unit emitted at x = 296 is read
   back as 296-512 = -216 and lands off the LEFT edge. The same file installs
   `g_ws_obj_x_provider` to reinterpret a wrapped value as its unsigned
   reading. It is safe by construction, not by heuristic — it only touches
   sprites whose default decoding puts them entirely outside the visible
   region, so it can add an invisible sprite and can never move a visible one.
3. **OAM's Y field is 8 bits, and it wraps too.** A unit's halves sit at
   `[anchor-32, anchor)`, so an anchor of -68 writes OAM Y 156, which the
   hardware rule ("160 or more means negative") decodes POSITIVE and draws
   the head in the BOTTOM margin (`states/AQ_bottom_margin.state`). And a
   unit genuinely low on screen never appeared at all
   (`states/AR_rictor_bottom.state`, Rictor at anchor 196).
   **The X trick does not transfer**, and no rule over OAM can work: one byte
   addresses 256 rows while sprites span `192 + extra_top + extra_bottom`,
   which is 302 at 480x270, so both readings of a byte can be on screen.
   **The way out is that the guest never truncated anything** — it records
   each sprite in its own slot list (IWRAM `0x03004150`, stride `0x20`) with
   a full 16-bit `[+0x1C]` X and `[+0x1E]` Y, and that list survives to the
   frame boundary. `g_ws_obj_attr_y_provider` (added to gbarecomp for this)
   answers from it: each live slot predicts `(scrX-8, scrY-32)` and
   `(scrX-8, scrY-16)`, and the lookup matches the PAIR, which is what makes
   it exact. Anything the guest did not record falls through to hardware
   behaviour, so it fails closed. Bucketed by the Y byte because the renderer
   calls it ~35k times a frame.

**`gbarecomp` has ONE new seam**, `g_ws_obj_attr_y_provider` (`f529fd2`).
The other two seams already existed. Note `e63855f` removed an OBJ Y hook as
speculative — this is not that one reinstated; it has a measured failure it
fixes, and the README records it.

**Verified** (A/B against `KOL_NO_OBJ_CULL_WIDEN=1`, four repro states at
480x270): every changed pixel lands in a margin and **0** inside the centre
240x160, with centre-240 identity 0 px on each. `AQ_bottom_margin` has 0 px
in the bottom margin (the floating heads are resolved to their true rows, not
capped away) and `AR_rictor_bottom` has 401 px, all of it Rictor. Cost of the
whole widening at 480x270 over 600 frames: 32.2 s against 29.7 s, ~8%. Both render invariants byte-identical to the references (240
`66e13ab2…`, 284 `84a95b05…`, centre-240 identity 0/160 rows). On the device
at 480x270 the left-margin state shows six units with their END banners,
composed with the terrain and correctly depth-ordered.

**New test hook, and the reason for it:** `KOL_VIEW_SIZE=WxH` in
`kol_widescreen.cpp`. The desktop runner has `--view-width` and no
`--view-height`, and the real size comes from the Android menu, so the
VERTICAL margins could not be A/B'd anywhere but on the device. It is ignored
whenever a real menu choice exists. `KOL_OBJ_CULL_SLACK=<px>` widens the
window further on every side — a diagnostic that answers "is this a bounds
size question at all" in one run.

**Traps this cost, all in README:** the `bl.hi` address in a generated
disassembly comment is an ARTIFACT, not the call target (decode the BL pair
from the ROM bytes); `GBARECOMP_READ_WATCH` is blind to IWRAM, so the camera
at `0x03000DDA` cannot be watched; an OAM entry being "active" does not mean
visible (OBJ Y is 8-bit and only wraps when `y + height > 256`); and
`[rec+0x17] & 0x40` looks exactly like a visibility flag and is not one.

**The Y table is sampled ONCE per frame and that is load-bearing.** Rebuilding
it per scanline sweep tears it — the guest rebuilds its slot list during the
visible period while the renderer is still drawing from the OAM written in
VBlank, so a mid-frame read has the NEXT frame's positions, every lookup
misses and the entry falls through to hardware decoding. That showed as
top-of-field sprites flashing at the bottom edge WHILE SCROLLING
(`states/AS_scroll_flash.state`). There was a SECOND flash with the same
symptom and a different cause: the Y index used fixed-width buckets of four
and declined on overflow, and units stand on isometric rows, so four sharing
a row is ordinary (three frames of a 16-step scroll hit it). It is a counting
sort with no cap now. `KOL_OBJY_STATS=1` reports per-frame fall-throughs and
the worst row occupancy; both fixes together take fall-throughs to zero,
rebuilds from 34,290 a frame to 1, and the cost back to baseline (29.7 s over
600 frames). **Two traps in that counter, both paid for:** parked sprites at
`(y=168, x=248)` must be excluded (168 is inside the wrapped band, they can
never match, and left in they bury the signal under 26k a frame); and an
early return for a saturated bucket meant the counter reported a clean zero
across a scroll that was visibly still flashing. Count where the lookup gives
up, not per reason.

**The terrain box is FIXED, and it was two things.** It was not top-aligned
because `kVCornerTiles` was 8 and the box is 9 tile rows (raised to 9; still
under the roster's 10-row portrait column, which is what that cap protects).
And three element dots stayed in mid-screen because the panel is not one
component — it has no frame, so segmentation splits it, and the dots are a
1x3 island at col 24 while the anchored part is cols 26..29. They are BG,
not OBJ; there is no OAM entry near them, so this was NOT the ARMS-box
split, and an OBJ-follows-BG fix was written and then thrown away. Fixed by
position instead: every component entirely inside the box's known rect (cols
23..29 x rows 0..8, gated on the game's own `terrain_box_on_screen()`) takes
the shift of whichever part anchored. A proximity rule was the alternative
and was rejected as a new heuristic over every screen.

**Equipment names now have text.** 193 of the 489 pool strings are pure
`0x81` bitmap strips with NO text form in the ROM, which is why one slot read
as "Short Sword" and the rest as small images. `kol_unit_data.cpp` carries a
transcription table keyed by escape id; `read_name` fills `text` and reports
`bitmap = -1`, and the Java side needed no change because `-1` already means
text. Two traps, both in README "Equipment names that had no text":
`kEscTable`'s offset is in TILES not cells (reading it as cells lands on a
different name and looks plausible), and the strips are NOT all two-line —
105 of 188 are two 8px lines, 83 are one 16px line, told apart by whether
rows 7-8 are blank. Nothing decodes pixels at runtime — it is a static array and one index. A
wrong name is a TRANSCRIPTION error: regenerate the sheets with
`tools/dump_name_strips.py totkol.gba <dir>`, check the strip, fix the array;
do not assume the lookup is broken.

**The second screen draws the game's own sheet frame, read from the ROM.**
Twelve tiles (9-slice: 4 corners, 4 edges, 4 hatched field) at the addresses
in README "Done: the frame IS read from the ROM", plus palette bank 4 at
`0x0861DFE8`. They are NOT a tileset -- deltas 0x20/0x21, a bespoke codec,
not LZ77 or RLE -- but each occurs EXACTLY ONCE in the image and is fully
non-blank, so reading them by address is sound. No capture needed. Do not
re-chase the codec for the frame; it is only needed for the item icons and
the portrait.

**No vertical trade remains.** An earlier pass capped the top slack at 64 and
refused to widen the bottom, which removed the floating heads but gave up the
bottom margin — that is what hid Rictor. The slot-list lookup replaced it, so
both bounds follow the margins again. Do not reintroduce the cap.

**Device state.** slot 1 = `AS_scroll_flash` (the latest repro the user
saved; also kept as `states/AS_scroll_flash.state`), slot 2 = `S_move_highlight`,
slot 3 = `AC_viewmode_cut`. `states/` is gitignored, so `AA`..`AP` exist only
on this machine — `AO_obj_sides` (left), `AP_obj_right` (right),
`AQ_bottom_margin` (top), `AR_rictor_bottom` (bottom) and
`AS_scroll_flash` (the scrolling flash) are the five repros.

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, this commit as the
tip. `gbarecomp` on `widescreen-bg-y-provider-and-vram-view` at `f529fd2`,
clean. Nothing pushed. `git status` in BOTH before believing it.

# CLAUDE.md — Knight of Lodis Recomp

All rules live in the parent framework, not here:
- `../gbarecomp/PRINCIPLES.md`
- `../gbarecomp/DEBUG.md`
- `../gbarecomp/TCP.md`
- `../gbarecomp/CLAUDE.md`

Read those before touching this project. In particular: the BIOS is
recompiled and dispatched, never stubbed; a dispatch miss self-heals and
must be loudly logged, never silently interpreted forever; `game.toml` is
hand-maintained and merged from reviewed proposals, never auto-written;
and a build is never called "done" or "fully static" while any PC was
interpreted or healed-from-cache this session, without saying so.

## HANDOFF — 2026-09-21, evening (read this first)

**OPEN: units do not appear in the extended view's side margins.** Terrain
draws out there correctly; the units standing on it do not, and pop in as the
camera reaches them. `states/AO_obj_sides.state` is the repro at 480x270 —
tap left 5 times and they arrive.

**This is NOT the margin-tilemap problem again, and that matters.** There, the
guest had composed the whole map in EWRAM and the PPU was not asking. Here
the sprites genuinely do not exist: the guest emits no OAM entry for a unit
more than ~32 px outside its own 240. Measured — 21 active / 107 parked
before, 30 active / 98 parked after five taps left, with the new entries at
x = -32..16 and the active X range never passing -32 either way. No rendering
policy can reveal what is not in OAM. README "Units do not appear in the
margins" has the pipeline table, the two dead ends, and where to resume; do
not re-derive it.

**The agreed direction is option 2**: a reviewed `mod_function_hook` that
widens the guest's OWN cull, so the game emits the sprites and animation,
facing, palette and isometric depth ordering all come for free. Rejected
alternatives, with reasons, are in that README section and in the exchange
above it: synthesising sprites ourselves would be the first thing in this
whole effort that DRAWS what the guest never drew, and the depth sort alone
makes it bad value.

**Two dead ends already paid for.** The park value `(y=0xA8, x=0xF8)` is a
CLEAR, not a per-unit reject — every slot is parked each frame and only the
visible ones filled, so there is nothing to hook there. And `0x08144308`,
which the per-unit routine calls and skips the unit when it returns -1, picks
the sprite VARIANT from flag bits and returns -1 for "no sprite"; it never
looks at a screen position.

**Resume at** the code upstream of the per-unit setup at `0x08059EC0`: either
whatever builds the unit list, or a map-window test made when the scene is
composed. The lead is `0x08058EB2` (reached from `0x0805B353` and
`0x0806D8B3`), found by read-watching a live unit record — in that state the
record is at `0x02001898`, pointer at `0x02028960`.

**Tool trap, newly paid for:** `set_break_pc` + `call_stack_dump` over TCP
kills the session. The break unwinds into the runner, the step reports
failure and the server exits. Use `GBARECOMP_READ_WATCH` /
`GBARECOMP_WRITE_WATCH` instead; both cracked every step of this.

**Everything else from today is DONE and on the device.** The title-return
freeze, saving and the save/load round-trip, the vertical view, the HUD
anchoring on all four edges, the one view-size control with 240x160 as its
off switch, and pillarboxed scenes dropping to the authentic surface. Each
has its own README section.

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, this handoff
commit as the tip. `gbarecomp` on `widescreen-bg-y-provider-and-vram-view` at
`e63855f`. Nothing pushed. The APK on the Thor is built from these.
`git status` in BOTH before believing it.

**Device state.** slot 1 = `AO_obj_sides` (the margin-units repro), slot 2 =
`S_move_highlight`, slot 3 = `AC_viewmode_cut`. `enhancements.cfg` is
`width=480 height=270`, `widescreen.cfg` is `480`, and `files/totkol.sav`
holds a real battery save. States `AA`..`AO` exist only on this machine —
`states/` is gitignored.

## HANDOFF — 2026-09-21, later (read this first)

**Saving is FIXED — it was two bugs, both in the framework.** Reported as
saving the game failing; `states/AH_save_screen.state` is the repro. README
"Saving failed, and the save never reached disk" has the full derivation.
Short version:

1. **The flash model reported a 1 Mbit device ID (`0x09`) on a 512 Kbit
   cart.** The driver rejected it, AgbMain cleared the descriptor pointer at
   EWRAM `0x02029C18`, and every save then failed without a single command
   reaching the chip. The ID now follows the part's size (`0x1C` at 64 KB),
   and a savestate load re-derives it instead of trusting the blob.
   gbarecomp `a0a446d`.
2. **The battery save never reached disk on Android.** The auto-flush lived
   only at the bottom of the runner's outer loop, which under present-in-place
   may never come round again. It is called from the present hook now.
   gbarecomp `e82a323`.

**Bug 2 is the same shape as the title-return freeze** — work parked at the
runner's dispatch boundary does not happen when the guest never returns there.
If something else is broken and it lives only in that loop, suspect this
first.

**Two traps this cost:**
- **The flash ID read is invisible to `GBARECOMP_READ_WATCH`.** The game
  copies the read routine onto its own STACK and runs it there (the
  interpreted `0x03007E5C` in the boot banner), so it never goes through the
  bridge's `bus_read_*`. An hour went into "the game never reads the chip".
  Watch the model instead, or watch the WRITES, which are from ROM and do
  show up.
- **A savestate taken before the fix cannot validate it.** It carries the
  stale chip identity in the blob AND the guest's own cleared `0x02029C18`,
  so pressing A still fails on a fixed build. Only a fresh boot re-probes.
  `states/AH_save_screen_patched.state` is that state with the one pointer
  hand-poked — a test fixture, not a fix.

**This closes README "Next steps" 1**, the save/load round-trip that had never
been reachable. Confirmed on the Thor: save writes a 64 KB `totkol.sav` with
8164 bytes programmed, and after a force-stop and cold boot CONTINUE ->
"Continue from save" lands back on the world map at Solea with 3500 Goth.

**Device state.** `files/totkol.sav` now exists and holds that save — it came
from this testing, at the position of the state the user supplied. slot 1 =
the user's save-screen state (also kept as `states/AH_save_screen.state`),
slot 2 = `S_move_highlight`, slot 3 = `AC_viewmode_cut` (backed up and
restored after being borrowed for the test).

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, this handoff
commit as the tip. `gbarecomp` on `widescreen-bg-y-provider-and-vram-view` at
`e82a323`. Nothing pushed. The APK on the Thor is built from these.
`git status` in BOTH before believing it.

## HANDOFF — 2026-09-21 (read this first)

**The "Return to Title Screen" freeze is FIXED, and it was not a freeze.** The
guest was running the whole time — the RUNNER stopped presenting. Full
derivation in README "\"Return to Title Screen\" froze the picture — SOLVED
2026-09-21"; the one-line version is that the game soft-resets by branching to
its cartridge entry (`0x080000C0`) from inside its VBlank handler, so
`runtime_irq()`'s drive-to-completion loop never saw the iret it waits for,
`g_irq_nest_depth` stuck at 1, and `runtime_should_yield()` refused every
VBlank yield from then on. Fixed in gbarecomp `ae56c3c`.

**The FIRST-STEP question from the last handoff is answered: it is
PRE-EXISTING.** Built both repos at the pre-session pair (KoL `72cd7f5`,
gbarecomp `9eca428`) and the freeze reproduces there with the identical spin
signature. It was never the menu work or the present-unwind gate.

**Two traps this cost, and they are the reason it took a session before:**
- **The hang watchdog is a FALSE POSITIVE on this game.** It watches for
  "guest has not HALTed", and KoL never HALTs — its mainline at `0x0800056E`
  is an empty task loop and all the work is in the VBlank handler at
  `0x080005A0`. A cold boot that reaches the title screen and exits 0 trips it
  too. `hang_trace.csv` then holds 4096 records of that idle loop
  (`0x08000584..0x080008C8`) and reads exactly like a freeze. The previous
  handoff's "the stall is in the low-ROM idle loop, busy-waiting and never
  HALTing" was that artifact, not the bug.
- **`--frames N` cannot end a run whose frame boundary never arrives**, so a
  wedged runner and a wedged guest look identical from outside. The test that
  separates them, in one process: `--tcp`, `continue` (non-blocking — the
  server keeps answering while the core is wedged), then compare the `frame`
  command (the runner's completed-frame counter) against `run_status` (which
  reports `ppu.frame_count()` and `vblank_starts`). Frozen `frame` beside a
  climbing `run_status` = the guest is alive and presentation is what stopped.

**Verified.** Desktop: pressing A from `states/AG_title_return.state` reboots
the game, plays the opening and reaches the title screen (4000 frames, exit 0).
Device: same, confirmed by `adb exec-out screencap -p` — intro then "PUSH
START", nothing fatal in logcat. Both render invariants byte-identical to the
recorded references (240 `66e13ab2…`, 284 `84a95b05…`, centre-240 identity
0/160 rows), and `AG_title_return` with NO keypress dumps byte-identical BMPs
at 300 and 600 frames before and after the change.

**Tree state.** `KnightOfLodisRecomp` on `widescreen-16-9`, this handoff commit
as the tip. `gbarecomp` on `widescreen-bg-y-provider-and-vram-view` at
`ae56c3c`. Nothing pushed. The APK installed on the Thor is built from these.
`git status` in BOTH before believing it — this project has shipped that claim
falsely twice.

**Device state.** slot 1 = `AG_title_return` (pulled and byte-compared after
the session, still intact), slot 2 = `S_move_highlight`, slot 3 =
`AC_viewmode_cut`.

**The `auto` width question is SETTLED, and not by changing `auto`.** The
three widths were captured on the Thor at the same camera and the user's call
was to leave `auto` at 320 and make width a live menu item instead. Done: the
enhancement dialog has a Width button opening a single-choice list built
natively from the panel (284 / 320 / 384 / 480 on the Thor), each row naming
its trade-off, applied live through `runtime_request_view_width()` and
persisted as `width=` in `enhancements.cfg`. README "The enhancement menu" has
the details, including why the 16:9 entry is 284 and not the framework
helper's 285.

**Still open, in the user's priority order:** the second-screen panel LAYOUT,
which nobody has seen (Screen-2 is FLAG_SECURE — ask for a photo before
touching `drawUnitPanel`/`drawSlotList`/`slotName`, and never touch the decode
to fix a layout bug); and the displayed melee/magic DEFENCE pair and MOVE.

## HANDOFF — 2026-09-20, night (read this first)

**OPEN AND UNFINISHED: "Return to Title Screen?" freezes the guest.** This is
the only broken thing left. `states/AG_title_return.state` is the repro (world
map, the CHECK prompt up, hand on "Yes"); press A and the machine stops.

What is established, so it is not re-derived:
- **It is a FREEZE, not a crash.** No native abort, nothing in `logcat -b
  crash`; the process stays alive and the device sits on the half-faded world
  map indefinitely. `dumpsys activity exit-info` shows only force-stops.
- **Desktop reproduces it**, so the device is not needed:
  `GBARECOMP_INPUT_REPLAY` pressing A (`<load_frame+6>,0x3FE` then `0x3FF`)
  from that state. The runtime's own hang watchdog fires and dumps
  `hang_trace.csv` — that file is the diagnostic, read it rather than guessing.
- **The decompression phase COMPLETES.** One dump caught a byte-store loop at
  ROM `0x0800CB2C` writing EWRAM `0x0202BA48` -> `0x0202C084`, 224 distinct
  targets stepping by 4: advancing, not stuck.
- **The stall is after that**, in the low-ROM idle loop: the final 4096 trace
  records span only 88k cycles across PCs `0x08000584..0x080008C8`. The guest
  is busy-waiting and never HALTs — waiting for something that never arrives.

**The next step is one run, and it is the whole question**: does this freeze on
a build from BEFORE this session's `gbarecomp` changes? `git stash` them, run
the same repro, compare. My read is that it is NOT the menu work — the unwind
gate only fires for save/load/width requests and none is pending during a scene
transition — but that is a guess and it is not checked. If it freezes there
too, this is a pre-existing gap in a path nothing has ever exercised (README
lists the world map, shops and cutscenes as untested).

**What shipped and is verified this session** (all on the device):
1. `4db92ea` margins: the composed-map ring search ran in one direction only, so
   a camera parked LEFT of map column 0 (ring -1) scored 0% and silently fell
   back to the wrapped tilemap — the mangled right border. Rings now run both
   ways, and every disarm reports with a reason (three paths used to return
   before the reporter ran, which is why it looked like it worked).
2. `11db6db` (gbarecomp) + `7aa78cf` (gbarecomp): a save taken mid-IRQ wrote an
   unloadable state (pc at a THUMB address, cpsr in ARM+IRQ) that aborted at
   `pc=0x08BA1C02`. Root cause was deeper: **the present unwind had no safety
   gate at all**, so unwinding out of an exception handler left R15 on the
   interrupted mainline while CPSR said IRQ. It now waits for no-IRQ-nesting
   AND User/System mode, the same two conditions the excursion unwind beside it
   always used. That is the real fix; the save-side refusal is belt-and-braces.
3. `30ef364` the terrain box is back on the main screen, hiding machinery
   DELETED (flag, setter, tile matcher, env switch, the Android call). Do not
   reintroduce it behind a default-off flag.
4. `640a6fe` battle PREPARATION widens (it is the battle map plus WIN1; only
   OBJWIN disqualifies now) and `g_ws_authored_margin_layers` is set — but only
   once the composed map has verified THIS frame, never from the scene profile.
   Also: panel pinning blanked its source rect BEFORE anchoring and erased the
   "L VIEWMODE"/"R COMMAND" hints anchored into it.
5. `d774dbd` anchoring now requires a component to TOUCH an edge (a 6x6 CHECK
   menu in mid-screen was being dragged 40px off its own hand cursor), and a
   container's shift propagates to its contents, bounded by containment.
6. `ed2ecfe` the character selection panel left-aligns, sprites and all, via
   the framework's existing `g_ws_obj_attr_x_provider`. Its portraits and
   selection brackets are OBJ (14 sprites); moving the frame alone slid it out
   from under them exactly as it did the ARMS box.
7. `509cdbc` every enhancement now has its own switch; README has the table.
8. `92f1237` **enhancement menu on the BACK button** (Android): Widescreen,
   See-through move range, Second screen panel. Applied live, persisted in
   `files/enhancements.cfg`, built from the native `kol::Setting` list so a new
   toggle needs no Java change.

**Traps this session paid for. Do not re-learn these.**
- **`timeout` does not exist on macOS.** A repro command using it silently does
  nothing and looks like "could not reproduce". Cost a wrong conclusion.
- **Do not loop the emulator over every file in `states/`.** The user stopped
  it twice. Use the two render invariants plus the one or two states that bear
  on the change. When a question really does need every state, answer it by
  reading memory over the TCP rig in ONE process: the origin sweep that way
  took 0.4 s where a brute force would have run for hours.
- **With present-in-place, one `step_once()` spans many frames**, so anything
  serviced at the runner's dispatch boundary needs a present unwind to be
  serviced at all — and **must clear the unwind flag when it services it**, or
  every later `step_once()` returns immediately, the guest stops advancing, and
  the screen goes black and frozen while the process stays alive.
- **recomp-ui is NOT vendored here.** `RunOptions::ui_extra_items` exists in the
  API with no consumer, there is no paint-over-frame seam, and the game cannot
  see host keys. That is why the menu is Java-side and Android-only.
- **The pad writes save states on its own.** It reports L3 held, so any R1
  press saves; slot 1 was overwritten three times today, once while nothing was
  touching the device. PULL slot 1 before doing anything with it, and prefer
  slots 2/3 for test states (Shift+F2/F3 save, F2/F3 load, via
  `adb shell input keycombination KEYCODE_SHIFT_LEFT KEYCODE_F2`).

**Tree state.** BOTH repos committed and clean, nothing pushed.
`KnightOfLodisRecomp` on `widescreen-16-9` tip `92f1237`; `gbarecomp` on
`widescreen-bg-y-provider-and-vram-view` tip `7aa78cf`. The installed APK is
built from those two. `git status` in BOTH before believing this — this project
has shipped that claim falsely twice.

**Device state.** slot 1 = `AG_title_return` (the repro), slot 2 =
`S_move_highlight`, slot 3 = `AC_viewmode_cut`. `enhancements.cfg` is all-on.
`states/` is gitignored, so `AA`..`AG` exist only on this machine: `AA` crashing
unit-setup (will not load, kept as the repro), `AB`/`AC` battle prep,
`AD` character selection, `AE` the CHECK menu, `AF` world map, `AG` the freeze.

**Still open, unchanged from the previous handoff**: the `auto` width policy
(384/480 became defensible once margins drew real map — the user's call, ASK);
the second-screen panel LAYOUT, which still nobody has seen (Screen-2 is
FLAG_SECURE, ask for a photo); and the displayed melee/magic DEFENCE pair and
MOVE (README "Still not solved").

## HANDOFF — 2026-09-20, later (read this first)

**The widened margins now read the guest's COMPOSED MAP, not its tilemap.**
Reported as tiles popping in and out at the left and right border on the Thor
in widescreen. Root cause: the guest composes the whole map as tilemap entries
in EWRAM and blits a moving window of it into VRAM, keeping only TWO tile
columns past its own window on the side the camera is moving toward — so at 320
the outer three margin columns are the ones the guest has not written yet, and
the third flips with every 8 px of scroll. The margins now read the composed
array directly: exact, immediate, complete for every column.

```
array  = u32 at 0x02026CA8          BG0; BG1 = same array + 0x6000
stride = 0x100 bytes = 128 u16 columns, 96 rows
entry  = array + row * 0x100 + col * 2
```

New files `src/kol_map_source.{h,cpp}`; served through the framework's existing
`g_ws_tilemap_provider` seam — **no framework change, `gbarecomp` is untouched
this session.** README "The composed map is in EWRAM, and the margins read it"
has the derivation, the numbers and the two dead ends.

**Three things in there that cost time and should not be re-learned:**
1. **Do not position the lookup with the origin bytes** at `0x02026C94/95`.
   They hold the window origin at REST and match all twelve reference states,
   which is exactly why they look right — but they do not track the camera
   during a scroll (measured 23 rows out), and using them tore the margin by
   whole tiles on 60 frames of a 1,199-frame scroll. Position from BGxHOFS/VOFS
   and resolve the ring wrap by matching the guest's own drawn cells.
2. **A margin sample can only be DROPPED, never invented.** Every masking
   policy was simulated against recorded traces before the array was found:
   a fixed clip removes the flicker and half the battlefield; a direction-aware
   mask makes it WORSE (118 events vs 83), a motion-gated one worse still
   (171). Do not re-propose those.
3. **Report a fallback as a TRANSITION, not a capped repeat.** The first
   version logged at most four times, so a load transient on the device looked
   identical to a permanent failure and read as "it does not work on device".
   It logs state changes with durations now.

**Verified.** Both render invariants byte-identical to the recorded references
(240 `66e13ab2…`, 284 `84a95b05…`, centre-240 identity 0/160 rows). 100% of
drawn cells agree on twelve reference states, both layers, and on every frame
of a zigzag scroll. Margins carry 5 of 5 tile columns every frame against
3.19/4.90 before, with zero appear/disappear events against 57. No measurable
frame-rate cost. Confirmed on the Thor at 320, including at the map's real
edge, with no fallback while playing.

**Both repos are committed and clean, nothing is pushed.**
`KnightOfLodisRecomp` on `widescreen-16-9`: `a86ccd3` (the composed-map
margins), then this handoff commit as the tip. `gbarecomp` is untouched this
session and clean at `9eca428`. The installed APK is built from `a86ccd3`.

`git status` in BOTH before believing that — this project has twice shipped a
handoff claiming clean trees while `gbarecomp` held uncommitted hunks, and the
APK builds from the working tree, not from HEAD.

**Instruments left in, all quiet by default:** `KOL_WS_NO_MAP_SOURCE=1` (A/B),
`KOL_MAP_SOURCE_STATS=1` (agreement + resolved wrap), `KOL_MARGIN_TRACE=1`
(per-frame margin coverage, tilemap vs composed map), `KOL_COVERAGE_AT=<n>`
(hold `KOL_COVERAGE` until n frames after the battle scene comes up — the
default samples the first battle frame, which for several states is mid-redraw
and reports 55% of the AUTHENTIC 240 bare).

**Screen 1 CAN be captured** — `adb exec-out screencap -p` returns the game
at 1920x1080. Only `-d 4` (Screen-2) is blocked by `FLAG_SECURE`. The previous
handoff's claim that the device cannot be looked at applies to the second
screen only, and that is worth knowing before asking the user for photos.

**The panel layout task below was NOT done** — the user redirected this session
to the widescreen border. It is still open, and still needs someone to look at
Screen-2.

## HANDOFF — 2026-09-20 (read this first)

**FIRST TASK: look at the panel and fix its LAYOUT.** The data behind it is
verified line by line; the drawing is not, and cannot be from a terminal —
Screen-2 is `FLAG_SECURE`, so `adb exec-out screencap -d 4` returns an empty
file. Ask the user what the panel looks like, or ask them for a photo, before
changing anything. Everything drawn lives in `drawUnitPanel()` /
`drawSlotList()` / `slotName()` in
`android/app/src/main/java/com/koldecomp/game/StatsView.java`; the numbers and
strings arriving there are correct, so any problem is geometry, colour or
scale, not decode. Known guesses in that code, made blind and never seen:
the equipment/talent columns are placed by arithmetic that assumes four rows
each, `slotName()` picks a bitmap scale with `size/16` and baselines the strip
at `y - dh*0.8`, and the tint colours were chosen to match the existing panel
without being able to compare. Do not touch the decode to fix a layout bug.

**Both repos are committed and clean, nothing is pushed.**
`KnightOfLodisRecomp` on `widescreen-16-9`: `133d89a` (the panel rewrite and
the deletion of the capture path), then this handoff commit as the tip.
`gbarecomp` on `widescreen-bg-y-provider-and-vram-view`: `9eca428` (the read
watch). The installed APK is built from `133d89a`.

`git status` in BOTH before believing that, this handoff included — a previous
one asserted the same thing while `gbarecomp` in fact held three uncommitted
hunks, two of them the leak fixes it described as done, and the APK builds
from the working tree rather than from HEAD.

**The second-screen panel no longer captures anything.** Class, equipment,
talents, the equipment-adjusted STR/INT/AGI and the attack figures are read
from the unit record and a few ROM tables, exact and immediate for every unit
on the map. The speculative-capture path is **deleted**
(`src/kol_status_mirror.cpp`, `files/autostatus.cfg`, `files/excdump.cfg`),
replaced by `src/kol_unit_data.{h,cpp}`. The framework seam it used
(`gbarecomp/ENHANCEMENTS.md` §4) is deliberately UNTOUCHED — it is a general
capability, not dead KoL code, and nothing in this game uses it now.

Read README.md "Second screen: the unit panel, read from the record and ROM"
for the record layout, the three ROM tables, the `0x81` bitmap escape and the
formulas. Do not re-derive any of it.

**The single most expensive wrong fact this project carried**: the unit record
stride was documented as `0x4E0`. It is `0x68`. Alphonse is index 0 and Rictor
index 12, and those two happen to sit `0x4E0` apart, so a stride derived from
that pair looked right and showed exactly two units where there are eight. Two
sessions of "the class field cannot be found" trace back to it.

**What still needs the window, and so is NOT on the panel:** the displayed
melee/magic defence pair, Move, and the portrait (an OBJ sprite, in no tilemap,
so only a framebuffer capture could ever produce it). Removing the fork was a
deliberate trade, taken knowingly — see README "Still not solved".

**The tool that cracked it, and it is still there:**
`GBARECOMP_READ_WATCH="lo,hi[,max]"` in
`gbarecomp/src/runtime/runtime_bus_bridge.cpp` — a bounded read watchpoint, the
symmetric twin of the existing `GBARECOMP_WRITE_WATCH`. Arm it on a unit record
and the draw routine reads the fields at consecutive PCs; arm it on ROM and the
table lookups fall out with their indices. **This is uncommitted in gbarecomp.**

**Also worth knowing, because DEBUG.md and TCP.md are wrong about it:** the
`rdb_*` reverse debugger they document **does not exist** in this tree —
`src/debug/trace_ring.cpp` is a six-line stub. What does work is the TCP server
(`--tcp 19842`): `savestate_load`, `set_keyinput`, `step`, `read_ewram/vram/
pal/io`, ~180 frames/s, which is a complete experiment rig with no game-code
changes. Two traps: `--tcp` does NOT compose with `--load-state` (the TCP
branch returns first, leaving frame 0 and zero EWRAM — load over the socket
instead), and the server serves ONE client at a time, so a second connection
deadlocks silently. Note also that the widescreen frame hook does not run under
`--tcp`, so `KOL_UNIT_DUMP=1` needs the ordinary `--load-state --frames` path.

**Verified.** Both render invariants re-run after the change and byte-identical
to the recorded references (240 `66e13ab2…`, 284 `84a95b05…`, centre-240
identity 0/160 rows differing). Eight units decode correctly on the device,
each instantly as the cursor reaches it, no layout flip and no fork. Mullin
reads INT 55 against a base of 38 (staff +10, amulet +7) and ATK 39 at 65%
weapon power; Rictor reads INT 44 against 39 and ATK 76 at 90%.

**Not verified, and it cannot be from here:** the panel's visual LAYOUT. The
data reaching Java is confirmed line by line, and no exception is thrown, but
Screen-2 is `FLAG_SECURE` so `adb exec-out screencap -d 4` returns an empty
file. Someone has to look at the device.

**Unchanged traps** — the previous handoff's still apply: raise the log ring
with `adb logcat -G 16M` before instrumenting; `adb shell input keyevent` needs
`--longpress`; the pad reports L3+L1 held, so slot 1 auto-loads and the cursor
drifts on its own.

## HANDOFF — 2026-09-19, ~15:40 (previous session)

Short session on top of the previous one (whose notes are below, still accurate
apart from item 1). **EXP/Lv decoding is FIXED and verified on the device.**

**Also fixed: the panel changing LAYOUT while a capture runs.** Reported as the
second screen alternating between the old interface and the new one before
settling. It picked its layout off whether the hovered unit had a decoded
window, and a never-seen unit has none for the 500-600 ms the fork takes, so
every fresh unit flipped the whole panel over twice. It now holds the decoded
layout while a capture is in flight, showing what the record knows (name, Lv.,
HP, MP) and `--` for the rest. `status_capture_pending()` answers that LIVE on
the caller's thread — published once per emulation tick it lagged the cursor by
55-75 ms, which is still visible. See README "The panel must not change LAYOUT
while a capture is in flight".

**Tree state.** Both trees are clean and nothing is pushed.
`KnightOfLodisRecomp` on `widescreen-16-9`: `7da4d48` (the EXP/Lv decode fix),
`36af7ef` (handoff), `bd38fbb` (the panel layout fix, tip).
`gbarecomp` on `widescreen-bg-y-provider-and-vram-view`: `1e2c0a1` (tip).
The installed APK is built from `bd38fbb` and matches.

Worth knowing how that went, because the previous handoff said the same thing
and it was not true: `gbarecomp` had THREE uncommitted hunks — the `work_fb_`
seed in `restore_latched_framebuffer`, the `g_ws_pillarbox*` save/restore around
an excursion, and a `GBARECOMP_PRESENT_TRACE` fallback trace. Two of them are
the leak fixes that handoff describes as done. The APK builds from the working
tree, so it carried them while HEAD did not. They are committed now
(`1e2c0a1`). `git status` in BOTH repos before believing a handoff, this one
included.

**What was wrong with EXP.** Not the BG layer. `Lv.` and `EXP` were being read
from tiles `0x86,0x87` / `0x88,0x89`, which are not labels at all — they are the
2x2 **element icon** immediately left of the real labels, and it is per-unit.
The real labels are `0xB3,0xB4` (Lv.) and `0xB5,0xB6` (EXP), constant across
every window measured. The old mapping was derived from one unit's window, where
the icon happens to sit where the label was assumed to be, so it decoded that
unit and only that unit. README "Lv. and EXP were keyed to an ICON" has the
table and the evidence.

**The suggested fix would have broken the capture gate.** Matching each label
across all four layers independently lets a BATTLE MAP frame decode as valid —
the map's HP/MP bar is on BG2, BG0 matches `INT` by accident — and that frame
then gets cached as a status window. The decoder now picks the ONE layer
carrying HP+STR+AGI together and reads every field off it: no hardcoded BG1, gate
still strict. Skips layers that are disabled or non-text in the current mode.

**The Thor auto-loads slot 1, and that is the CONTROLLER, not a feature.**
Every launch this session logged `state_combo: L3+L1 -> load slot 1` about five
seconds in, with nothing touching the device — the pad reports L3+L1 held (and
the cursor drifts on its own between units). Handy, since slot 1 is the
reference state, but remember what it implies: L3 is a modifier, so while it is
"held" the shoulder buttons are withheld from the guest, and unexplained cursor
movement during a measurement is the pad, not the game.

**Verified.** Eight units on the device, every field decoded, zero misses; 148
excursions, invariant held on all of them; both render invariants re-run and
byte-identical to a pre-change build (240 `66e13ab2…`, 284 `84a95b05…`, centre
240 of the 284 dump identical to the 240 dump, 0/160 rows differing).

**Open, roughly in priority order.**
1. **Class name and portrait are gone** from the header (replaced by text). Class
   is neither in the record (never identified) nor decodable (dynamic tile run),
   so it can only return as a cropped image strip. The OLD crop was wrong —
   `src.set(16,8,216,24)` hardcoded, slicing the portrait box and clipping glyph
   tops. Derive the rect from the tile runs (name rows 1-2 cols 2-9, class rows
   1-2 cols 19-26) instead of hardcoding.
2. Equipment and talent lines are not carried at all. Same dynamic-tile-run
   problem, same solution as class.
3. The second screen's footer still credits `EWRAM 0x02028960` for data that now
   comes from the tilemap.
4. Frame-rate cost of the forks on the Thor is UNMEASURED.
5. The logged `kol: captured status window` line prints only on the FIRST cache
   insert for a unit. A later, brighter frame silently updates the stored values
   without logging, so that line is not proof of what the panel shows. It was
   nearly a wrong diagnosis this session.
6. **Offered and not taken: prime the cache at battle start.** The first hover
   on any unit still waits 500-600 ms for its fork — invisible now that the
   layout holds still, but the decoded figures do arrive late. Forking every
   unit on the map up front would make the first hover instant, at the cost of
   a burst of forks for units the player may never look at. UNMEASURED; the
   user was offered this and left before deciding, so ASK before building it.
   Note the README's finding that paging inside one open window yields no
   further units, so it is one fork per unit, not one fork for all.

**Instruments left in place, both deliberate and both quiet.**
- `screen2: values valid=N pending=N battle=N cached=N` prints only when the
  panel's layout inputs CHANGE — two lines per hover. This is what measured both
  the 500 ms fallback and the 55-75 ms lag; keep it unless it gets noisy.
- `status_decode: MISS …` dumps the tilemap the decode actually saw, bounded to
  twice per process. A silent missing field is what hid the EXP bug for a whole
  session.

**Traps this session paid for. Do not re-learn these.**
- **Raise the log ring before instrumenting: `adb logcat -G 16M`.** The device
  ring is 256 KiB, about 30 seconds, and this app out-logs it. The symptom is
  `adb logcat -s KoL` printing NOTHING while the game is visibly running — which
  reads as "logging is broken", not "you read too late". Most of this session.
- **`adb shell input keyevent` mostly does not reach the guest.** Down and up
  land in the same pump, so a level-sampled key is never seen. `--longpress`
  works. Arrow keys are the D-pad, `Z` is B, `X` is A, `F1` loads slot 1.
- **A missing field that prints nothing is a bug you cannot see.** The decode
  now dumps the tilemap it actually read when a field goes missing, bounded to
  twice per process. That dump found this in one run after a session of guesses.

## HANDOFF — 2026-09-19, ~00:40 (previous session)

Long session on the second-screen status panel. Everything is COMMITTED and both
trees are clean: `KnightOfLodisRecomp` on `widescreen-16-9` (`224e57d`),
`gbarecomp` on `widescreen-bg-y-provider-and-vram-view` (`394d0fc`). Nothing is
pushed. The installed APK matches HEAD.

**Device state.** `files/autostatus.cfg` = `auto` (speculative capture ON; write
`live` instead for the stepped shadow, delete the file to turn it off).
`files/totkol.state1` is `states/Y_cursor_on_char.state` (md5 b408a4fc, cursor on
Boise, loads at frame 317848). Debug markers and frame dumps were cleaned up.

**What works.** Hovering a character on the battle map forks the machine, presses
B in a discarded timeline, lets the guest draw that unit's status window, decodes
it, rewinds, and draws a native panel on the second screen: name + Lv + EXP from
memory, HP/MP bars, STR/INT/AGI, and ATK/DEF/MOVE decoded from the rendered
tilemap. Verified field-by-field against Boise; Rictor decodes INT 38, the
equipment-adjusted value this project had written off as unreachable.

**Open, roughly in priority order.**
1. ~~**EXP decodes as -1 on in-battle windows.**~~ FIXED 2026-09-19 — and not
   the way this item guessed. The labels were on BG1 all along; `0x88,0x89` is a
   per-unit element ICON, not the EXP label. Matching across all four layers, as
   suggested here, would have broken the capture gate. See the handoff above.
2. **Class name and portrait are gone** from the header (replaced by text). Class
   is neither in the record (never identified) nor decodable (dynamic tile run),
   so it can only return as a cropped image strip. The OLD crop was wrong —
   `src.set(16,8,216,24)` hardcoded, slicing the portrait box and clipping glyph
   tops. Derive the rect from the tile runs (name rows 1-2 cols 2-9, class rows
   1-2 cols 19-26) instead of hardcoding.
3. Equipment and talent lines are not carried at all. Same dynamic-tile-run
   problem, same solution as class.
4. The second screen's footer still credits `EWRAM 0x02028960` for data that now
   comes from the tilemap.
5. Frame-rate cost of the forks on the Thor is UNMEASURED.

**Traps this session paid for. Do not re-learn these.**
- **Desktop does not reproduce the device's rendering bugs.** Headless forks land
  on frame boundaries; the device's land mid-frame, and the device runs the
  extended view at 320. Three separate theories died to that gap. When a visual
  bug is reported on the device, instrument ON the device.
- **Do not compare frames across camera positions.** I posted a bare-margin table
  comparing a device frame to a desktop frame at a different scroll offset and
  called it damning; it was meaningless. The README's own numbers show the bare
  area ranges 0.8%-45.9% with camera position alone.
- **The invariant covers GUEST state only.** All five leaks found this session
  were present-time HOST state: the frame-present hook, the scene classifier, the
  UI anchors, the capture gate, and `g_ws_pillarbox`. Each presented as an
  unrelated visual glitch. Assume anything a speculative frame touches leaks
  until shown otherwise.
- **Two correct fixes can combine badly.** The policy early-return (correct, for
  the anchor leak) removed the margin re-authorization that made
  `GbaPpu::deserialize`'s deliberate fail-closed harmless — producing the edge
  flicker. Neither change was wrong alone.
- A logged VALUE found the last bug in one shot after four hypotheses failed.
  Print the state; do not reason about it.

## Game-specific facts (verified, do not re-derive from scratch)

- Title: Tactics Ogre: The Knight of Lodis, USA release.
- Game code: `ATOE`, maker `EB`.
- ROM: 8 MiB, SHA-1 `85a46320a31f16f5c6dda27fc094edd078dbe43d`, MD5
  `5d4f385e4e1cfa4a057d0ac828993906`.
- Entry point: `0x080000C0` (plain header, `B` to entry, nothing exotic).
- Header checksum byte at `0xBD`: `0x69`.
- Save chip: Flash 64KB / 512 Kbit, `FLASH_V` signature (auto-detected by
  the runtime from the ROM; not independently verified against real
  hardware).
- Sound engine: MP2K/Sappy, confirmed via a live M4A mixer-state dump
  (6 active channels) during a headless run.

## Editable

- `game.toml`, `symbols/*`
- `src/main.cpp`, `src/kol_widescreen.{h,cpp}`, `CMakeLists.txt`
- `android/**` (entry point, Gradle/CMake, Java glue)

## Never

- `generated/**` — 100% derived from the ROM via `gba_recompile`, never
  hand-edited. Fixes belong in the recompiler (`../gbarecomp/src/**`,
  `../gbarecomp/tools/gba_recompile`) or in this project's `game.toml`.

## Current status

See `README.md` for the full, itemized status — read it before claiming
anything about this project's state. Short version: `src/main.cpp` +
`CMakeLists.txt` exist and link against the real `gbarecomp::run_game()`
runtime path. A headless build has booted through the recompiled BIOS and
recompiled cartridge code via real `runtime_dispatch` (not the
interpreter, not an oracle-only smoke test) into the "Licensed by
Nintendo" splash and legible story-intro text through frame 5000 (~83s).
Cart-ROM coverage is fully static through 9000 frames (~150s) — every
single ROM-code address, with zero exceptions. Two things that looked
like open questions were both closed by disassembling the actual bytes
with gbarecomp's own decoder instead of guessing from the miss log: the
`0x0815B8xx`–`0x0815Dxxx` cluster turned out to be MP2K-style trampoline
functions plus their interior IRQ-resume points, not a jump table
despite the runtime's own misleading `jump_table_candidate` flag; and
`0x08000000` turned out to be a genuine, valid `b 0x080000C0`
instruction (the real GBA hardware boot entry point), not header data as
an earlier revision of this file wrongly claimed. See `game.toml` for
both writeups. What's left open is only a few IWRAM addresses correctly
left to the RAM self-heal tier — by design, not a gap.

Real hardware verification is now set up and running: `oracle_diff/
diff_frame_totkol.py` drives this project's executable against a real
mGBA-backed oracle (`gbarecomp/oracle/gbarecomp_oracle`, needs
`third_party/mgba` built first — see README.md) over the same TCP
protocol gbarecomp's own diff tooling uses, diffing framebuffer/OAM/
PAL/VRAM/EWRAM/IWRAM/IO byte-for-byte per frame. First run found a real
bug in gbarecomp itself (DISPCNT power-on default was 0 instead of the
correct 0x0080), now fixed upstream and verified. Every other
divergence found through frame 120 has since been chased to a root
cause and is understood: a DISPSTAT live-vs-latched status-bit
difference (inconsequential), write-only-register reads returning
open-bus garbage on the oracle vs a fixed value on native
(inconsequential, confirmed via a native memory-write watchpoint that
the "written" register is never actually written), and a one-frame
counter-convention offset between the two debug servers (compensated
in the tool via `--oracle-lead`, default on). With that compensation,
OAM/PAL/VRAM/EWRAM are confirmed byte-identical to the oracle across
the whole tested range. See README.md for the full writeup — do not
re-investigate these three without reading it first.

**Call-return stack crash — ROOT-CAUSED AND FIXED (2026-09-17).** Two
independent causes, both addressed upstream in gbarecomp:
1. `runtime_should_yield`'s forced unwind (fires at call depth 512) tore
   down the generated host call chain with bare `return`s that retire no
   call-return frames, stranding the whole chain — and self-reinforcing,
   since the depth then stays pinned above the limit so every later frame
   re-triggers it. Matches the measured "pinned at 3, then climbs to ~567
   and sticks" signature exactly. Fixed by publishing
   `g_runtime_dispatch_base_depth` at top-level dispatch and truncating
   back to it on both unwind exits.
2. `kCallReturnStackSize` of 1024 was genuinely too small: with the leak
   fixed, the game's measured peak legitimate depth is **6,153** (a
   transient spike that fully unwinds to 0). The earlier raise to 65536
   was necessary, not a band-aid.

Verified: light-input run zero growth (was 574); crash reproducer clean
to 9,950 (was SIGABRT at 2618); 56,360-frame aggressive mash clean;
42,710-frame instrumented run shows depth 0 at every sample; ctest 26/26;
idle baseline and mGBA oracle diff both unchanged.

CAVEAT: `GBARECOMP_CALL_STACK_ABORT_AT` below ~6,200 now false-trips on
legitimate deep recursion. Leave unset for normal use.

Two other real defects were fixed along the way (cancel_return checked
only the top slot instead of searching; the self-heal bridge cleanup
could resurrect retired frames). Eliminated hypotheses, recorded so they
are not re-chased: stranded IRQ floor; the `0x0815938a` coverage gap; and
"push-minus-match" as a leak metric (it is not — read the depth column).
See README.md for the full writeup and evidence table.

**Sprite fade bug — FIXED (2026-09-17).** Sprites did not fade with the
rest of the scene during fades. Framework bug, not game-specific: both
compositing paths in `gbarecomp/src/gba/gba_ppu.cpp` gated OBJ
first-target participation on `obj_mode == 1` (semi-transparent only),
ignoring BLDCNT bit 4. Fixed upstream (`fc961e5`), confirmed by
play-testing. Note that the mGBA oracle framebuffer diff did NOT detect
this fix (52174 -> 52059 summed pixels over frames 100-300 in a full
A/B) — whole-frame difference sums are a poor detector for a localized
rendering defect; diff a bounded region instead.

FIXED 2026-09-18 (semi-transparent OBJ vs brightness effect): with
BLDCNT=0x3FD0 + BLDY=8, `gba_ppu.cpp` gated the alpha branch on
`top[x].layer == 4` instead of "OBJ mode 1", and let a semi-transparent OBJ
with no valid 2nd target fall through to the brightness branch. Net effect:
normal-mode sprites never darkened (the command menu stopped greying out
disabled options) and semi-transparent sprites darkened whenever they
overlapped another sprite (KoL's "END" banners). `PixelCandidate` now carries a
`semi` flag; both copies of the pipeline fixed. Covered by
`test_semitransparent_obj_ignores_brightness_effect`.

STILL OPEN and NOT covered by that fix: frames 220-268 (`BLDCNT=0x3fbf`,
brightness increase, OBJ targeted) show a steady ~3622-pixel difference
vs mGBA, identical with and without the sprite fix. Undiagnosed; the
pixels have not been located on screen yet.
That open item is now a plausible candidate for the SAME defect: 0x3fbf names
every layer including OBJ as 1st target with effect 2 (brightness increase), so
defect 1 would have suppressed the brightening on normal-mode sprites. Untested
— the oracle is not built here. Do not assume; re-run the comparison. See README.md.

**Android port — WORKING (2026-09-17/18).** Runs natively on an AYN Thor
(arm64-v8a, Android 13). Framebuffer byte-identical to the macOS build at
frame 600; 5.00x realtime headless. NDK r27.3.13750724, SDL2 2.32.10 vendored
at `android/app/jni/SDL` (gitignored — fetch command in README.md). Build with
`cd android && ./gradlew assembleDebug`. See README.md "Android port" before
touching any of it. Three things that WILL bite otherwise:
1. `app/build.gradle` must keep `targets 'main'` — AGP otherwise builds
   gbarecomp's host test generators for arm64 and fails.
2. Self-heal recompilation MUST stay off on Android (no runtime toolchain,
   dlopen restricted, no arm64 tcc). The 4 IWRAM PCs stay interpreted.
3. SDLThread's stack is ~1 MB, not 8 MB. The guest runs on a 16 MB pthread
   created in `main_android.cpp`; without it the save-state serializer
   overflows the stack and SIGSEGVs. Do not "simplify" that away.

`adb logcat -s KoL` shows the runtime's stdout/stderr (routed in
main_android.cpp). Use it — without it Android failures are invisible.

Controls: L3+R1 saves state slot 1, L3+L1 loads. No on-screen touch controls;
the picture fills the screen at 3:2.

**Second screen (Thor) — live character panel.** `android.app.Presentation` on
`Display 4 "Screen-2"` (1240x1080). Reads guest EWRAM through the framework's
new READ-ONLY `runtime_ewram_view()`; it is a present-time shadow and must never
write. Unit record: `0x02028960` is a u32 pointer to the SELECTED unit;
`+0x30` name, `+0x40` u16 HP/HPmax/MP/MPmax/STR/INT/AGI, `+0x52` level.
Array stride 0x4E0.

KNOWN WRONG: STR/INT/AGI are BASE values; the game displays
equipment-adjusted ones and stores them nowhere (Rictor 33 vs 38 in-game).
Labelled as such in the UI. Move and weapon figures are the same class of
derived value — do not hunt addresses for them.

OPEN: the class field is NOT identified (5 candidate offsets). Three approaches
already failed — direct name-id search, strided table scan, and poke-and-observe
(the status text is cached in VRAM and never re-read). Read README.md before
re-attempting; the remaining route is a breakpoint on the 12 literal-pool refs
to the name table at ROM 0x623678.

**16:9 extended view — WORKING for the battle map (2026-09-18).** True 16:9
is **284x160** (160*16/9), not 426 — an earlier pass in-session used 426,
which is 8:3. No framework code was changed: the shared PPU already had the
whole wide-surface capability (`gbarecomp/ENHANCEMENTS.md` §3). What the game
owed it was scene policy, now in `src/kol_widescreen.cpp`, shared by the
desktop and Android entry points.

Two invariants, both verified and both cheap to re-check — do so after any
change here: (1) at 240 the faithful render is byte-identical to a pre-change
build; (2) at 284 *and* 480 the centre 240x160 of a widened frame is
byte-identical to the faithful render of the same state.

The **bottom unit bar is continued** into the margins seamlessly (band
y 139..159, sample `hw_x = hx mod 8` — the field is an 8x8 tiled hatch and
columns 0..7 are plain). It degrades safely: no bar drawn means transparent
source columns, so no test for visibility is needed. Other HUD panels are
corner-anchored and stay clipped; making those flush needs shifting, not
continuing. Otherwise the battle map exposes **BG0|BG1 only** in the margins. Suppressing BG2|BG3
there is the whole trick: a GBA tilemap wraps, so with all four layers exposed
the 256px-wide battle HUD drew a duplicate of itself into the left margin.
Every unclassified scene fails closed to black margins. Android is opt-in via
`files/widescreen.cfg` (absent = faithful 240).

**Character info panel pinning** forces the unit info panel to the bottom-right
of the wide view. The guest relocates it in BOTH axes to dodge the selected
sprite, so an X-only remap cannot re-place it; this needed a second framework
seam, `g_ws_bg_y_provider` (vertical companion to `g_ws_bg_x_provider`, layer
mask defaults to 0). The panel is identified as the only SOLID 10x7 block on
BG3 — verified at three positions across F/G/B, absent in D. Pure translation,
source rect blanked. `KOL_WS_NO_PANEL_PIN=1` reverts. NOTE this overrides
collision avoidance the guest does deliberately: a unit in the bottom-right of
the wide view can still end up behind the panel.

**Battle PREPARATION widens too** (2026-09-20). It is the battle map plus WIN1
around the NAVI dialogue (`dispcnt 5F40` vs `1F40`, identical BGxCNT), and the
classifier's old "no windows" clause pillarboxed the entire deployment phase.
Only OBJWIN disqualifies now. The PPU additionally blacks out margin rows whose
240 native pixels do not all pick the same window control, so KoL sets
`g_ws_authored_margin_layers` — gated on the composed-map check having passed
THIS frame, never on the scene profile alone.

**Pinning must not erase what anchoring moved in.** The panel's source-rect
blank ran before anchoring and ate any UI anchored into the vacated space: the
prep screen's "L VIEWMODE"/"R COMMAND" rendered as "L VI"/"R CO". The anchored
lookup now skips cells inside the panel source, and the blank runs after
anchoring. `KOL_WS_UI_DUMP=1` prints the per-cell anchoring grid.

**HUD edge anchoring** moves corner panels out to the screen edges (character
info panel to bottom-right, HP/MP to the left, info panel to the right). It is
the ONLY thing here that moves pixels inside the authentic 240, so invariant 2
is verified with `KOL_WS_NO_HUD_ANCHOR=1 KOL_WS_NO_PANEL_PIN=1`. Decisions are per connected
component of the UI tilemap, never per scanline (a per-scanline rule tears
elements whose run touches the edge on some rows only). Anything sharing a tile
row with full-width furniture is bar content and does NOT anchor — shifting the
bottom bar's BG2 contents against its full-width BG3 frame drew a stray box
around the "A" of "Alphonse". Panel bodies translate pixel-exactly.

Android `auto` snaps to an INTEGER scale, not the exact aspect: largest integer
scale whose 160 lines fit the panel, then the width that fills it. On 1920x1080
that is 6x -> **320x160** (1920x960, 60px bars), which is the default there.
284 is exact 16:9 but needs a fractional 6.75x, so it scales unevenly; 320 also
shows a third more battlefield.

The terrain box RELOCATES: with a NAVI bar up it is drawn at cols 15..21
instead of its usual 23..29. That is a fact about the game, and it matters to
anything that finds the box by position.

**Second screen mirrors the guest's own terrain box** (elevation, icon, all
modifiers) by rasterising BG3+BG2 over tile rect cols 23..29 x rows 0..8 out of
VRAM — NOT rebuilt from values, because elevation and the icon are per-tile and
in no ROM record. Gated on `kol::is_battle_scene()`; off the battle map that
rect holds intro graphics. Read from the tilemap, so HUD anchoring/pinning do
not affect it. The box also stays ON the main screen, top-right, where the game
draws it — the suppression that used to remove it there was deleted on request
2026-09-20, switch and all. Do not bring it back behind a flag. This does not
contradict "cannot mirror the status screen" — that box is not drawn during
combat, this one is.

**Panel text is ONE TILE PER CHARACTER** out of BG2's char block: digit d =
tile `0x56+d` (bank 4), `HP:`=`0xA8,0xA9` (bank 12), `MP:`=`0xAA,0xAB` (bank
13), `Lv.`=`0xB3,0xB4`, `EXP`=`0xB5,0xB6`. Verified against known values in
three states. Per-label palette banks are load-bearing — they are why HP reads
blue and MP pink. The second screen builds an 80x16 atlas from these and
renders stats in the game's own font. NAMES are different: rendered into a
dynamically allocated tile run (2 tiles per char), so no code->tile mapping
exists and they are captured as an image.

**STR:/INT:/AGI: labels are on the STATUS SCREEN only** (BG1, char block 2,
bank 12: STR `0x168,0x169`, INT `0x16B,0x16C`, AGI `0x16D,0x16E`, shared colon
`0x16A`). NOT resident during battle — that char region holds other data, so
they must be captured while the status screen is up and cached.
**The unit panel is recoloured per team**: same frame tiles `0x001..0x020`,
palette bank 4 (player, cream) vs 5 (other team, green). Capture the frame on
change, not once. The glyph atlas is team-independent (verified identical).

**Terrain properties table found (ROM):** base `0x0861CA5C`, stride `0x20`,
89 records, six s8 elemental modifiers at `+0x16` in row-major screen order
(orange, red, green, blue, white, grey). Verified — the sampled pattern is
unique in the whole ROM. SOLVED by breakpoint: 68 small accessor functions take the
terrain id as a **u16 in r0** (`(r0 & 0xFFFF) * 0x20 + base`), so hooking them
with `g_runtime_fn_entry_hook` yields the live id. Verified end to end against
the rendered panel. Enough to drive a second-screen modifier panel. STILL OPEN:
elevation and the terrain icon, both per-tile, not in the record.
See README.md "Terrain properties table" for the probe, the four searches that
failed first, and three traps: a parked save state never runs the lookup;
`GBARECOMP_INPUT_REPLAY` frames are absolute and the load frame varies between
runs; and `g_runtime_bus_read_override` cannot fake KEYINPUT because the
runtime writes it into the IO page. Also `0x02029B80` looks like a perfect
terrain pointer and is MP2K song data.

The unit panel and bottom bar are found by GRID SCAN, never by connected
components: they touch each other (panel bottom row on bar top row) and merge,
so neither matches its shape. Tile identity cannot separate them either — panel,
bar and dialogues share one 9-slice tileset and palette bank. Bar = run of rows
ending at the bottom, >=26/30 occupied, <=4 rows (a 27x5 bottom dialogue
otherwise qualifies). Panel = solid 10x7 on BG3 containing the HP:/MP: label
tiles on BG2. Pin target is bottom-right but ABOVE the bar when one is up.

HUD policy must not touch CUTSCENE DIALOGUES or the ARMS box: only components
at most 10 tiles wide anchor (the width of the widest real corner furniture,
not a margin above it). The ARMS box's weapon icons are OBJ SPRITES, which no
BG provider moves, so anchoring slid the box out from under them, and bar continuation additionally requires real
full-width furniture that frame — defined as the FULL 30 columns AND <= 4 rows
tall, because a 27x8 dialogue passed a looser `>= 27` width test. UI contents
also INHERIT their container's non-shift: text on one layer inside a frame on
the other are separate components, and a 14-wide caption anchored right out of
its own 22-wide box until they were tied together. Without both, a dialogue box slid away from
its portrait and its own tiles were smeared across the bottom band.

**Margins always show OFF-CAMERA tilemap content** — BG0's map is wider than
the camera, so this is inherent, not a bug. Mostly that is the real map; but
the tilemap also keeps regions the game never displays, which render as
scenery floating in the sky (found by playtesting, state E). Guarded by a
contiguity rule: a margin tile draws only if its tile row has content at the
window edge. Needs `runtime_vram_view()` (read-only, mirrors `runtime_ewram_view()`) — one
of the two framework additions in this work, the other being
`g_ws_bg_y_provider`. Verified: kills the artifact
in E, byte-for-byte no-op on B and D. It is a heuristic that fails closed.

Do NOT overstate this: it is verified on a handful of camera positions.
An earlier claim that a 480-wide stress test was "strong evidence" the margins
were safe was WRONG — state E disproved it. Keep testing new maps.

`--window` sets `quiet`, and Android always passes `--window`, so every
handheld session suppresses `!args.quiet` banners. Do not gate a new
player-facing message on it.
Cutscenes, world map, shops and combat animations simply pillarbox today.
`KOL_WS_PROBE=1` logs scene classifications; `KOL_WS_FORCE_EXPOSE=1` reveals
what a new scene would put in its margins. Read README.md "16:9 extended view"
before extending it.

The hang watchdog message
some headless runs print
("guest has not HALTed... MC-HP-002 class") is a confirmed false
positive, not a game bug — reproduces at different PCs on different
runs, both cold and warm self-heal cache, and the process always
completes normally afterward. It's the runtime's 4-second wall-clock
watchdog outrun by headless self-heal recompilation, not a guest freeze;
see README.md for the reproduction. No oracle diff has been run —
everything verified so far is "looks right," not "matches real
hardware." Do not describe this project as accurate, complete, or
oracle-verified; "boots into the game" is the extent of the verified
claim.

**Unit status window on the second screen (2026-09-18).** The status screen is a
full SCENE SWITCH, not an overlay, so it cannot be rendered next to the battle
map — "you cannot mirror the status screen" still holds. It is CAPTURED from
`active_ppu()->latched_framebuffer()` and replayed per unit. Cache keyed by
record address AND name bytes (addresses are only stable within a battle). Keep
the BRIGHTEST frame of a visit, never the last — the classifier still matches
during the fade-out, so blind overwriting cached a black rectangle (38400/38400
non-black captured, 0 stored).

`files/autostatus.cfg` (or `KOL_AUTO_STATUS=1`) captures it WITHOUT the player
opening it, via a speculative fork — see `gbarecomp/ENHANCEMENTS.md` §4 for the
three preconditions, all of which bit here first, and one of which presents as a
SIGSEGV in `runtime_call_cancel_return` with no obvious link to its cause. Write
`live` into that marker for a stepped shadow timeline that animates. Speculative
frames must NEVER touch the real scene classifier: they are status frames, so
they made `is_battle_scene()` false half the time and the second screen cleared
itself at the step rate (229 clears vs 171 draws — the flashing). Use
`is_battle_scene_now()`/`is_status_scene_now()` inside an excursion, the plain
ones everywhere else.

**The window reads back as DATA.** "Derived figures are stored nowhere" is true
of the battle map's memory and irrelevant once a shadow has DRAWN them: digits
are `tile - 0x56`, `0x60` is `/`, big Move digits use the two-tile `0x42..0x55`
set (same as terrain elevation), fields located by LABEL tile identity (`HP:`
0xA8,0xA9; `STR:` 0x168,0x169; ...). Verified field by field against Boise:
HP 52/136, STR 27, the derived 57/36 and 17/17, Move 49. NAME and CLASS are
dynamic tile runs with no code->tile mapping — capture those as images.
`KOL_STATUS_TILES=<prefix>` dumps the tilemaps. See README.md.

Movement-range highlight (2026-09-18): 0x02008440 in EWRAM mirrors the CURRENT
BG0 tilemap (stride 0x100, u16 entries, ring wrap -32), verified 100% on two
battles. It does NOT hold the pre-highlight tile, and neither does anything else
in EWRAM/IWRAM/VRAM — searched exhaustively. The game regenerates originals on
cancel. An earlier README claim that this was the base map was wrong; it came
from comparing snapshots across three processes. Sample inside ONE process with
KOL_MAP_SNAP and check known-changed cells, never an aggregate match rate.
