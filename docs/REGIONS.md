# One source, two cartridges: the Japanese ROM (ATOJ)

Started 2026-11-25 on the branch `region-jp` (a git worktree,
`koldecomp/KnightOfLodisRecomp-jp`). Not merged, not released. The tools and
the evidence are in `koldecomp/regionmap/` (its README first).

## How it works

- **The source speaks USA.** Every address of the game is written as the USA
  ROM's and wrapped: `usa(0x02029D40u)` (`src/kol_region.h`). The USA build
  takes it as written; the wrapped USA executable is byte-identical to the
  one before the change.
- **A region's build translates.** `-DKOL_REGION=ATOJ` defines
  `KOL_REGION_ATOJ`: `usa()` looks the address up in
  `src/kol_region_atoj.inc` at compile time (`consteval`). An address that is
  not in the table is a compile error at the line that uses it.
- **The table is generated**, never typed:
  `regionmap/make_region_table.py` places each wrapped address through the
  ROM-to-ROM map and `regionmap/out/atoj_overrides.map`, and notes what the
  memory check said. `src/kol_region_atoj.report.txt` lists what is not
  placed and what rests on weak evidence.
- **`usa_only()`** marks what only the USA game has (its font and text
  banks, one function the Japanese build lacks). It is 0 in the Japanese
  build: the code around it has to take that region's path. The readers it
  feeds are bounds-checked (an address under the ROM reads as nothing).
- **The recompiled code** is per region: `generated/` and `generated_atoj/`
  (from `regionmap/atoj/game.toml`, the USA `game.toml` translated).

## Building it

    # once: the Japanese ROM recompiled (regionmap/README.md), then
    ln -s ../regionmap/atoj/generated generated_atoj
    cmake -S . -B build-atoj -G Ninja -DCMAKE_BUILD_TYPE=Release -DKOL_REGION=ATOJ
    ninja -C build-atoj KnightOfLodisRecomp

After changing an address in the source or a correction in the overrides:

    python3 ../regionmap/make_ctl_table.py ../regionmap/out/atoe_to_atoj.changes src \
        --overrides ../regionmap/out/atoj_overrides.map --out src/kol_region_atoj_ctl.inc
    python3 ../regionmap/wrap_source.py src            # wraps any new address (usa_only/japan_only are left alone)
    python3 ../regionmap/make_region_table.py ../regionmap/atoe/atoe.gba \
        ../regionmap/atoj/atoj.gba src --funcs ../regionmap/atoe_functions.txt \
        --overrides ../regionmap/out/atoj_overrides.map \
        --checked ../regionmap/out/kol_ram_checked.map --out src/kol_region_atoj.inc

Not wrapped, on purpose (the list is in `wrap_source.py` and at the top of
`kol_region.h`): the hardware's bounds, sanity bounds near them, 0x03007FF0,
our own scratch RAM at the top of EWRAM, and a zip signature in
`kol_shaders.cpp` that only looks like an address.
`tools/hd_audio/gen_map.py` writes its ROM addresses wrapped.

## What is checked (2026-11-25)

- USA: the executable is byte-identical before and after (557 addresses
  wrapped in 29 files, the build option, `main.cpp`).
- Japan, enhancements off, 240x160: the full build equals the plain runtime
  on the same ROM, picture and EWRAM/IWRAM/VRAM/OAM/palette, at boot (600
  frames) and 60 frames into a battle.
- Japan against USA, the same moment in both games (the USA save loaded in
  both), perspective + shadows + ambient occlusion + lifted props and walls:
  a Training battle at 384x216 and 480x270, its setup screen, the world map.
  The pictures differ only where the two games' texts do (the hero's name,
  place names).

## The Japanese game's text (worked out 2026-11-25)

Found by watching the game print a line (DMA sources into VRAM, then the
script bytes), not by matching the USA code: the text engine is different
code there.

- **Font**: 8x16 glyphs of 64 bytes (two 4bpp tiles), the USA font's cell
  format, at `0x08715A5C + code * 64`. Every glyph is one 8-pixel cell.
  Punctuation 0x00-0x0B (0 a blank, 2 `、`, 3 `。`, 4 `ー`), kana from 0x0E,
  digits 0xAF, capitals 0xB9, small letters 0xD3, kanji from 0xED, more marks
  0x37F-0x399 (`．，・：；？！＿％―～※…` and `＋−±×÷＝（）`).
- **Codes** are 16-bit, low byte first. From 0xF000 they are controls:
  0xF000 ends a text, 0xF100 a line, 0xF701/0xF702 close and open a speaker's
  name. In the HELP, NAVI and message banks there are three more, and no
  others (counted over all three banks): 0xF2nn sets the colour of what
  follows (n added to the text's palette bank: 0 dark, 1 grey, 2 blue, 3 red;
  blue and red seen in the game, grey read off the palette), 0xF4nn is
  figure n filled in at run time (a birthday's month, day and sex; a team's
  size), 0xFEnn the name of character n (records of 8 codes at the Japanese
  address of the USA character-name table, 0x08622318).
- **Names**: 489 records of 8 codes, zero padded, at `0x0861E4C8 + id * 16`,
  the USA name table's ids (9 ソルジャー, 86 バトルファン). No pre-rendered
  strips: the USA ROM's `0x81 NN` bitmaps were its own answer to long English
  names.
- **Everything else is in "m7" banks** (82 of them): `m7`, a u16 count, 12
  bytes an entry (u32 offset from the bank; a byte of lines and a byte of
  pages, two lines to a page; the box's width in characters; a u32). An
  entry opens with the size of its own header, which lists where each of its
  lines starts; then codes. Options' names `0x08754A4C` and HELP `0x087535E4`
  (45 each: the option, then its values), HELP `0x08737B78` (696 entries, one
  bank where the USA ROM has two, under the USA help ids), NAVI `0x08751A74`
  (36) and the battle's messages `0x08750144` (72), both where `usa()` puts
  the USA banks and under the same entry numbers. Lines are set by hand for a
  box 26 characters wide and padded with blanks to it.
- Desktop readers: `regionmap/jtext.py`, and `regionmap/jhelp.py` (the two
  ROMs' HELP, NAVI and message entries side by side; `check` counts the
  entries whose numbers, figures and names agree: 629 of 696, 35 of 36, 70 of
  72, the rest wording a number differently).

In the source: `src/kol_font.h` (codes, the font, `ascii()`, advances, by
region) and `src/kol_jtext.h` (the bank reader). `japan_only()` marks a
Japanese ROM address, 0 in the USA build.

**The options menu works in the Japanese build** (`regionmap/shots/menu_jp.png`):
the game's 12 options and their HELP in Japanese from the ROM; our own labels
in the font's Latin letters, drawn by their ink so an English line is as wide
as it was (the cells would make it a third wider); stand-in glyphs of ours for
`> < & ' * "`, which that font lacks. The USA menu is pixel-identical to the
one before on 14 pages, its bobbing hand aside.

**The second screen's sheet works in the Japanese build**
(`regionmap/shots/sheet_jp.png`, `sheet_jp_hired.png`): the unit's name, its
class, equipment and talents in Japanese. The codes are 16 bits now
(`UnitName::codes`, `UnitInfo::name_codes`, `help_codes`, `menu_encode`),
the sheet draws through kol_font.h, names come from the Japanese name
records and a unit's own name from its record (8 codes at +0x30, zero
padded: checked with a unit hired and named あいう in the Japanese shop). The
USA sheet is byte-identical to the one before on 21 pictures
(`regionmap/sheet_shots.sh`, baseline `regionmap/out/sheet_base/`), and so is
its unit dump text.

**HELP, NAVI and messages work in the Japanese build**
(`regionmap/shots/help_jp.png`, `help_jp_rows.png`, `kol_move_cards.png`,
`kol_prepnavi_cards.png`, `kol_battle_lists.png`, `zone_help.png`): the card
of an equipment or talent slot, of any row of SELECT's help cursor (the
birthday row with its month, day, sex and the fortune's coloured stars, which
the USA game words instead), of the battle's ARMS and TALENT lists, the
battle's "can't move there" message, battle preparation's NAVI prompt with
its team size, and the bar's eight help cards. How:

- `help_text_at()` has a Japanese path (`help_text_japan`): the entry's lines
  as the game set them, padding dropped, figures and names filled in, colour
  controls kept for the card.
- The card does not reflow Japanese text (`help_card`, kol_sheet.cpp): the
  game's own lines, two to a page, a paragraph gap where a page ends a
  sentence, each glyph in its 8-pixel cell (`Canvas::cell`), colours from the
  palette banks the game's box uses. The card holds 28 cells; the lines are
  26 at most.
- Checked against the game's own help box on three texts
  (`regionmap/cmp_help_box.py`): every text pixel of the card's first page is
  in the game's box, in the same colour; the only pixels that differ are the
  box's frame and HELP tab.
- The USA cards are byte-identical to the ones before (the 21 sheet pictures;
  the bar's help cards against the executable from before the region work).

## Offsets inside the window control block

`usa_ctl(0x618Eu)` (kol_region.h): an offset in the status window's and the
roster's work area, written as the USA ROM's. The Japanese build lays that
block out differently: 2 to 4 bytes less from 0x1B92, 0x54 less from 0x1C24
on. The table
`src/kol_region_atoj_ctl.inc` is generated (`regionmap/make_ctl_table.py`)
from the literal-pool constants the two builds' matching code loads: 19 of
the 30 offsets by their own votes, 7 because 131 voted offsets from 0x1EA4 to
0x61DC all moved by 0x54, and 4 by hand (`ctl` lines of
`regionmap/out/atoj_overrides.map`): the battle's weapon and spell lists
(0x1E94, 0x203C, 0x2047, 0x206C), whose votes mixed in other structures'
constants. Each was found in both games' memory with the ARMS list and the
TALENT list open (`regionmap/battle_list_evidence.py`) and is 0x54 lower too.
With the table the sheet finds the unit under Organize's cursor in the
Japanese build, and battle preparation's NAVI hook (kol_actions.cpp, the
preparation block's 0x2052, 0x2053, 0x2064) takes the game's no-box branch
there as in the USA build. Wrapped: kol_unit_data.cpp's `kCtl*`,
kol_actions.cpp's `kWin*` and `kPrep*`, kol_widescreen.cpp's box descriptor.
Not wrapped: other structures' offsets (the battle control's `kCtlState`
0x73E.., the sprite pool's 0x2020, a unit record's fields): nothing has
shown them to differ.

## The actions (kol_actions.cpp) in the Japanese build

Run in both full builds from the same states, the test switches doing what
the pad and the bar do (`KOL_NEXT_UNIT_AT`, and `KOL_WORLD_BUTTON_AT="k:n"`,
new, dev builds only: world button k on the roam hook's n-th call):

- **Next / previous unit**: in Organize's status window (Alphonse, Allos,
  Flauros and back: the same records' twins, the sheets at the same frames),
  on the battle map and in the battle's status window of a Training battle
  with two units a side (states `battle2`, `b2_status`): the window's
  picture and colours follow in both. The traces are the same line for line
  on the map; in the window the third request is served one hook call apart.
- **The bar's world buttons**: Item, Spell, Class (Organize's stock screens
  and class list), Training, Shop, Save and Load each land on the same
  screen in both (`regionmap/shots/kol_world_buttons.png`).
- **The messages it takes from the game**: "can't move there" and battle
  preparation's NAVI prompt (above).

Coverage of those Japanese runs: the same two interpreted functions as
before (0x08012EDC, 0x08012F4A), nothing new.

## One package, two games (2026-11-25, the user's choice)

A region is still chosen when a program is compiled: each ROM has its own
recompiled code (`generated/` and `generated_atoj/`, about 440 MB of C++
each, the same function and table names), and our own code takes its
addresses at compile time (`usa()`), which is what makes a missing address a
compile error. So a package holds a PROGRAM for each game, and the front end
knows both (`kCarts` in kol_region.h; `Game.java` is the same table):

- **Desktop**: `KnightOfLodisRecomp` (USA) and `KnightOfLodisRecomp-jp`
  (CMake names the ATOJ build so) side by side. Either one can be started:
  the setup screen takes either ROM (told by its SHA-1), and the program
  that was started hands over to the other (`desktop_handover`, main.cpp's
  `start_again`) when the game to play is not its own. With one program
  alone, everything is as it was (its setup pictures are identical to the
  executable's from before the region work, on three scripted sequences).
- **Android**: one APK with `libmain.so` (USA) and `libmain_jp.so`
  (`kol_game_library` in jni/CMakeLists.txt; built when `generated_atoj` is
  there). `MainActivity.getLibraries()` loads the one to play; a process
  holds one, so changing game is a restart (`RestartActivity`).
- **Which game**: the one last chosen (`game.cfg` in the player's folder,
  `usa` or `japan`) when its ROM is in place, else the one whose ROM is
  (`setup_game`, `Game.current`). With both ROMs in place the setup screen's
  title is a switch (desktop) or shows a "Switch to ..." button (Android).
  From the game: Options > Technical > Game restarts on the setup screen.

### RetroAchievements: the USA game only

The achievement set is the USA cartridge's (kol_cheevos.cpp's fixed hash):
its conditions read that game's memory at fixed addresses, which no `usa()`
of ours translates. The first run of the Japanese game on the Thor
(2026-11-25, the user logged in) unlocked 25 achievements in its first
seconds, and the server kept them (5 of 87 before, 30 after, softcore). A
program for another cartridge now never loads the set
(`begin_game_load`): the page says "Only for the USA version of the game."
The lesson is wider than achievements: anything that reads the guest's
memory by address and is NOT in our source (a server's conditions, a cheat
file, a tool's script) is one game's only.

### Where each game's files are

| | USA | Japan |
|---|---|---|
| ROM, save, slots, mod, numbered states, Fast Loading memo | the player's folder itself, under the names they always had | its `japan` folder |
| settings, keys, display, BIOS file, shaders, logs, achievements login | the player's folder (shared) | the same |

`game_dir()` (kol_settings) is the game's own folder in the running game;
`setup_game_dir(dir, cart)` the same for the front end; `Game.dir` in Java.
A save state is one game's memory and the two lay theirs out differently
(the runtime refuses a state made on another ROM), a name typed in one game
is other characters in the other, and the Fast Loading memo keys on RAM
only: nothing a game made is shared.

### Nothing an earlier version made moves

The USA game's files are exactly where every earlier version put them, so an
update needs no migration. Checked on the desktop: a player's folder made by
the executable from before this work (ROM, save, a numbered state) started
with the package's program: no setup screen, the state loaded to the same
picture as in the old executable, the title's Continue offered as before,
and the save, ROM and state files byte for byte unchanged. On the Thor: see
the handoff note for what was checked after the install.

### Checked (desktop, headless, a folder with both programs linked in)

The Japanese ROM given to the USA program (it lands in `japan/`, the
Japanese program takes over and runs); the USA ROM added from the Japanese
program; a start with both in place from either program (the remembered game
plays); the switch on the setup screen (`regionmap/shots/setup_two_games.png`,
`pkg_boots.png`).

### Checked (Android)

On the emulator (arm64, headless): the two-game APK installed over an old
1.1.1 install went straight to the USA game, its ROM, save and slot state
byte for byte unchanged; the Japanese ROM put in `files/japan` and chosen
loads `libmain_jp.so` and shows the Japanese opening; with both ROMs the
setup screen's "Switch to ..." button changes game and Play loads the right
library; a ROM given through the system file picker is told by its SHA-1
and lands in its game's folder, a wrong file is refused with a message and
the ROMs in place are untouched. On the Thor: installed over the debug app
(1.2.1), its save, ROM, settings, memo and 883 state files unchanged; the
user then played both games there.

### The Japanese game's saves in the Files app and Sync Folder (2026-11-26)

Asked: the Japanese saves and states available in Android's Files app, and
synced. Both used to look only at the top of files/, so files/japan was
neither listed nor copied.

- **Files app** (`SavesProvider`): the `japan` folder is shown at the top,
  and inside it the same kinds of files and folders as at the top (saves,
  slot states, `states` and a mod's `states-<8 hex>`, `.bak`s); its ROM is
  never shown. The logs are every game's, at the top only. Copying a file in
  under a name that exists keeps the old one as `<name>.bak`, as at the top.
- **Sync Folder** (`SyncFolder`): the Japanese game's files are copied to a
  `japan` folder in the picked one, at the same paths as under files/
  (`japan/totkol.sav`, `japan/states/0001.state`). The question, the backups
  and the import treat both games' saves as one set: the question's counts
  include both, a backup keeps the `japan/` path, and choosing the folder's
  set replaces both games' saves at the next start. An earlier version
  syncing the same folder from another device ignores `japan/`.
- Game.java's `folder(g)` / `isFolder(name)` name the games' folders, so a
  third game's would follow.

Checked on the emulator (debug app, test files in files/japan, headless,
uiautomator): the Files app shows `japan` (holding `states`, the save and a
slot state, not its ROM) and its states; a save copied into `japan` from
Downloads replaced the one there and left it as `totkol.sav.bak`. Sync
Folder picked on an empty folder: all 8 files copied, the 5 Japanese ones
byte for byte under `japan/`; a Japanese save changed in the game was copied
again; a numbered state deleted in the game was deleted in the folder; the
folder's Japanese save changed from outside and then the game's went to
`backup-.../japan/totkol.sav` (byte for byte) before the game's was written.
The same folder picked again with a different Japanese save and a new state:
the question counted both games' files; Folder's, then Restart: the game's
`japan/` held the folder's save and state, its old save was in
`backup-.../japan/totkol.sav`, the identical slot state was left alone, and
the USA save and state stayed byte for byte unchanged throughout. Not tried:
the Thor, and a real save written by the Japanese game while synced (the
same files, written the same way as the USA game's).

### The APK's size (measured 2026-11-25)

113.0 MB packed fresh (debug, both games) against 77.1 MB (debug, one game)
and 72.5 MB (the 1.2.2 release). (A debug APK repacked in place by Gradle
after rebuilds was 148.9 MB: 36 MB of it a dead copy of a replaced library.
Delete the APK and assemble again before measuring.) What is in it: the HD
instrument bank 39.3 MB, stored as it is; each game's library 36 MB packed
(31.7 in a release), 115 MB unpacked, of which the ROM's recompiled code is
88 MB and our own sources 7 MB.

Tried or estimated, for a smaller one:
- one library for both games would share only our sources and the runtime
  (some 7 to 25 MB of 115 unpacked): a few MB packed, for the loss of the
  compile-time region;
- the Japanese library as a patch on the USA one (zstd --patch-from): 20.3 MB
  against 22.1 MB compressed alone, so the two builds' machine code shares
  almost nothing byte for byte;
- zstd -19 packs a library to 22 MB where the APK's deflate gives 36, but
  the app would have to unpack and load it itself;
- the bank through a lossless audio coder (FLAC, the file taken whole as
  16-bit mono): about 20 MB of the 39;
- the Japanese library downloaded when its ROM is first given: the APK as
  small as a one-game one. The packaging scripts pack both programs when `generated_atoj` is there:
`tools/package_macos.sh` (build-macos-atoj, the second executable in
Contents/MacOS, signed before the bundle), `tools/package_windows.sh`
(build-windows-atoj, KnightOfLodisRecomp-jp.exe beside the USA .exe, the
README names both games), `tools/package_appimage.sh` (this checkout's own
folder on the Deck, build-release-atoj there, both in usr/bin; KOL_SSH for
a Deck at another address, KOL_PACKAGE_TAG for a test build's file name, so
a release's AppImage on the Deck is never overwritten). Nothing is
compressed beyond what each package format already did (the user's choice,
2026-11-25). Not done: `tools/package_flatpak.sh`. The Files provider and
Sync Folder on Android carry the Japanese game's files since 2026-11-26
(below, "The Japanese game's saves in the Files app and Sync Folder").

### Checked (desktop packages, 2026-11-25)

Built with both programs (public builds, KOL_DEV_TOOLS=OFF), against
1.2.2's one-game packages:

| | both games | 1.2.2 |
|---|---|---|
| macOS zip | 98.3 MB | 68.3 MB |
| Windows zip | 112.7 MB | 74.1 MB |
| AppImage | 113.0 MB | 77.2 MB |

Each run on a copy of a player's folder made by the executable from before
this work (ROM, save, a numbered state; the same key presses from the
title): no setup screen, the save, ROM and state byte for byte unchanged,
and the picture at the end, the load menu with Continue from save,
identical to the old executable's. And on a folder with both ROMs and Japan
chosen: the USA program hands over to the Japanese one, which plays. The
Mac app headless (dummy video); the Windows zip under Wine with the null
display driver; the AppImage on the Deck headless, in a test folder
(removed after; the player's own folder untouched).

## Offsets written bare (2026-11-26)

Reported on the Thor (Japanese state on map 98, the archers' town, Enhanced
settings): when the screen fades to the burning town, our flames stayed lit
over the darkening town; in the USA game they fade with it. Cause: the
flames' fade compares PAL RAM with the scene's full palette in EWRAM, and
`kol_focus_present.cpp` named it by a bare EWRAM offset (`0x2D90C`), which
`wrap_source.py` never sees (it wraps whole 8-digit addresses only): the
Japanese build read the USA game's offset. Found the same way: four reads of
the map number (`ew[0x29D40]`) in kol_widescreen.cpp (the magic circle, the
sea and world-map BG2 maps, the fade log), which read 65535 in Japan.

- The scenes' full palette is at **0x0202E2EC** in Japan (USA 0x0202D90C):
  in the Thor state just before the fade, two EWRAM blocks equal the bright
  BG palette, 0x02000010 (the palette buffer, already in the table) and
  this one; 30 frames into the fade this one is unchanged while PAL and the
  buffer have darkened. Added as a `checked` line of
  `regionmap/out/atoj_overrides.map`; the table gained that one entry.
- Written through `kol::usa()` now: that palette, the map number, the
  palette buffer and the weather block (kol_widescreen.cpp), the game mode,
  the phase flags and the battle's unit entries (kol_states.cpp), the game
  mode (kol_unit_data.cpp). The USA code is unchanged: the four files
  compiled from the old and the new source give the same code.
- Checked: the Thor state through the fade, Japanese build, Enhanced
  settings, before and after (`KOL_FADE_LOG` now reads map 98 and the full
  palette, 2721/2721 before the fade, 1499/2721 into it): the flames now
  fade with the town.
- Then (same day, Thor state `0004`, map 102): the "An Anecdote of Ogre
  Battle Saga" title got our light, shadows and evening grade over the
  whited-out map. The check that the map is whited out walks its composed
  tilemap through the pointer at 0x02026CA8, read as `rd32(ew, 0x26CA8u)`
  (a read call's offset, which the first sweep missed) in three places:
  the title (`scene_white`), the sky's dimming (`map_palette_dim`) and map
  84's model fade. The table already had it (0x02026C28); now
  `kEwBg0Array` (kol_focus_internal.h). Checked: the Japanese title at three
  moments, before (effects over the map) and after (a white field and the
  text), and the USA title at the same point of its scene with the new USA
  build: the same picture but for the words (both have the same 9 black
  pixels at the field's edges, an older and separate thing). The USA code
  of the 15 files involved is identical, old source against new.
- `regionmap/bare_offsets.py` lists every offset still written bare (index,
  pointer arithmetic, read calls, and named constants used to index EWRAM),
  and whether the table has its address.
- Then the rest (same day): every offset the scan listed, EWRAM and IWRAM
  (kol_vs_bot's and kol_vs_cpu's constants, the link state, the unit list's
  trace), is written through `usa()` now; the scan finds none. The table
  placed all 13 new addresses; each was then checked in memory at the same
  10 paired states of both games (`regionmap/{atoe,atoj}/states`, made
  from the same save and keys), and four got a `checked` override line:
  war funds 0x02001864 (99,999,999 in both), the scene number 0x02001A9E,
  the time of day 0x0200289C (equal, and changing: 0 on the world map, 2
  in battle), and the window control block's pointer 0x020297B0 (the USA
  pointer + 0xA34, the heap's shift in Japan; the neighbours' guess
  0x0200044C held 0). Confirmed as placed: the Quest menu's screen pointer
  and the world state pointer (ROM pointers, + the ROM's 0x2D74 shift),
  the battle phase, the menu cursor (1 on VS in the Japanese Quest menu).
- Checked in use: **Smooth Boxes** in the Japanese build (state `dlg0`, A
  every 40 frames, `KOL_BOX_ANIM_LOG=1`): the old program never left idle,
  the new one opens the boxes in phases with the box's own descriptor (0 22
  3 6). **Fast Actions, Fast Walking and Quick Enemy Turns**: a move from
  `b_move` in both games skips the same holds to the frame and the cycle
  (2.09 and 7.41 frames) and shows the enemy's move x4 alike. **The shop's
  Buy list** (the bar's Shop button, `KOL_WORLD_BUTTON_AT=4:20`, Shop
  Anywhere): its block reads the same tab, count, cursor row and item ids
  in both games (the block's offsets are the same), so the second screen's
  shop list names the right item.
- Then (same day, the user on the Thor: no rumble when attacked or attacking,
  Japanese state `states/thor_japan/0010`, map 6): kol_rumble's battle
  records were `kBattleRecs = 0x1420u`, declared beside another constant, a
  form the scan's named-constant pattern missed; now `usa(0x02001420u)`
  (0x02001B00 in Japan, in the table). Before: the enemy's blow brought its
  number and found no HP drop; after: "hit" at frame 140432. The scan takes
  `kX = 0x....u,` too now. The sound effects' numbers are the USA game's in
  the Japanese one (its song table at 0x0816A92C: the same header bytes for
  43 entries checked, the same player word for all 449), so the thunder's
  (205) and Shaher's death's (418) rumble alike.

### VS in the Japanese game (2026-11-26)

VS against the computer did not start in the Japanese game: it went to
"Connecting...", found no one and went back. Four things, found by driving
the Japanese game from its title (regionmap's `atoj/states/vs_title_menu`
.. `vs_qmenu`: Down to QUEST MODE, A, A on the save slot, Down to VS; the
USA game's `atoe/states/vs_qmenu` the same way) and running whole matches
headless (`KOL_AI_ARMY=1`, `KOL_KEY_SCRIPT` for the player's A on VS, on
the attack order and on the Winning Condition, `KOL_VS_KEEP=1`):

- **The start.** The USA game is told by 0x02002830 = 0x24 and +6 0x1A on
  Connecting's first frame: a byte of its text engine, which the Japanese
  game does not share (the routines that load it moved by 0x14CC to 0x2FC4;
  no Japanese byte pair behaves alike: `guess` line in the overrides, NOT
  FOUND, now unused in Japan). The Japanese game starts VS where Exchange
  always started: the link routines' set-up (0x0800601C), when VS's
  Connecting calls it. VS's call comes from its own place, USA 0x0814D8F8 /
  Japan 0x08150734 (the only call there, frame 2082 after A on VS; Exchange's
  is 0x0814FB2C); `kVsAtSetUp` in kol_vs_cpu.cpp, false in the USA build
  (its code unchanged). VS Network takes the same start in Japan.
- **The attack-order byte** 0x03000EF0 (Japan 0x03000A80, as placed) was
  right: it reads 0, 2, 0, 2 through Connecting, the attack order and after
  the A in both games. (On that screen and on the Winning Condition the
  player answers, in both games; the bot only answers for the computer's
  side there, where the 2 comes.)
- **The Yes/No box** ("Start the battle?", "Appoint X as leader?"): at
  0x020380D4 as placed, but its input byte and hand are at +0x1C / +0x1B in
  the Japanese game, +0x1F / +0x1E in the USA one (the hand byte at
  0x020380EF went 1 -> 0 on Up). `kYesNoInput` / `kYesNoHand` in kol_vs_bot.
- **The deployment's unit list** is at 0x02038520 in the preparation (the
  overrides had 0x02038574, its place in battle): the same bytes as the USA
  list (roster order 00-12, cursor +0x2C, scroll +0x2E). Before, the bot sent
  out one unit and started; now five, as in the USA game.

Checked: VS against the computer in the Japanese game from the Quest menu:
the snapshot on Connecting, the computer's game started and linked in
121-132 ms, attack order, roulette (a battlefield chosen), Winning
Condition, both sides send out five units, Start Game, Yes, leaders
appointed, the battle's turns (frame 9065: "Enemy phase", units with END).
The USA code of the 18 files involved is identical, old source against
new. VS Network (two Japanese copies on this Mac, `KOL_VS_MODE=1`, both
armies by the AI, the copies too through `KOL_VS_CHILD_ENV="KOL_AI_ARMY=1
KOL_AI_ARMY_AT=3000"`): found each other, linked, five units each, 142
hashes matched to read 9156, well into the battle. (Without the AI switch
passed to the copies, both the Japanese and the USA pair went out of step
at the battle's first AI turn, read ~3900: the test's fault, not the game's.)

## Not done

- **Seen only in part.** Grey text (colour 1) has not been on screen; a unit
  with no sex shows two dashes (the USA ROM's "--" string has no Japanese
  twin where it should be); a made item's name is read as a unit's. The shop
  lists' help cards and the army stock's (R menus) go through the same
  reader and were not opened. The save state's note writes the hero's name
  as UTF-8 kana (`jtext::utf8`, tried on the ROM's name records), but no
  numbered state was saved from a Japanese game: that needs a window.
- A talent's name of 8 characters (アシドヴェイパー) ends 5 px short of its
  POW figure on the sheet: inside the 64 px the layout keeps for a name.
- Our own labels stay English in the Japanese build, the user's choice for
  now (2026-11-25): the options menu's added rows and pages, the titles of
  the bar's help cards over the game's Japanese text, the Save States page.
  Should they be translated later: the game's font has only the 650 or so
  kanji the game uses. The page "Enhancement Options" reached its row's
  arrow in the game's font; it is "Enhancements" since 2026-11-26 (asked).
- **Untested there**: the unit list's taps (they move the cursor as next /
  previous unit does, which is checked), opening a unit's profile from the
  world map, the stick cursor, story scenes' actors (the user played map
  98's archers in Japanese and reported only the fade), sound placement,
  mods. Checked since: the evening room (map 77, Thor state
  `states/thor_japan/0016.state`, after the flashback, from the USA save
  copied to `japan/`; 0015 is the same room by day): time of day 1, and the
  room keeps its day colours (the palette is the set at the table
  0x085EC5BC + 0x7D40, the header's +0x20; the game's evening set, +0x24,
  differs in 51 of 96 colours), the user saw it right on the Thor
  (2026-11-26); VS against the computer (above), Fast Actions and the other speed hooks,
  the bar's Shop button and the Buy list, Smooth Boxes (above); HD music
  runs (its log: the tick model matches, HD PSG notes played).
- **Two functions run interpreted** in the Japanese build (0x08012EDC,
  0x08012F4A); proposals in `regionmap/atoj/carried_proposals.toml`, not
  merged. gbarecomp's rule: a person reviews what goes into a game's
  config, so the translated game.toml should become a generated overlay
  under a small hand-written base.
- This branch starts at 9159d23: the uncommitted menu fix on `main`
  (kol_menu.cpp, the translation mod) is not in it, and will meet this
  branch's kol_menu.cpp changes when merged.
