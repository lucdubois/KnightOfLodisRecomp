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

## The desktop's setup screen and files

The Japanese executable asks for its own cartridge (kol_region.h:
`kCartSha1`, `kCartAsk`, `kCartWrong`, `kCartVersion`; kol_setup.h's
`kRomSha1` is the build's): its setup screen is titled with the Japanese
game's name, takes the Japanese ROM and refuses the USA one by name, and the
USA build the other way round (headless, `KOL_SETUP_KEYS=drop:...`,
`regionmap/shots/setup_wrong.png`, `setup_jp.png`). The USA screen's pictures
are identical to the executable's from before the region work.

It keeps its own files: the player's folder is `KnightOfLodisRecomp-Japan`
(`kDataDirName`), and `rom_file_tag()` starts with `-jp` there
(`kRegionTag`), so that numbered states and the Fast Loading memo (which keys
on RAM only, not on the ROM it reads) are never shared with the USA build
even in a folder given by hand. In such a shared folder the ROM and the save
would still be the same two names (`totkol.gba`, `totkol.sav`): do not point
both builds at one folder. Both builds start in player mode from the folder
their setup screen filled (900 frames into the opening, HLE BIOS, the same
three cold-boot misses each).

## Two builds

A region is chosen when the program is compiled, for two reasons: each ROM
has its own recompiled code (`generated/` and `generated_atoj/`, about 440 MB
of C++ each, a 95 MB executable each, with the same function and table
names), and our own code takes its addresses at compile time (`usa()`), which
is what makes a missing address a compile error. One program for both ROMs
would need the recompiler's output namespaced per ROM, a runtime that picks
a dispatch table at start (gbarecomp), and `usa()` turned into a lookup at
run time at its 560 uses. One PACKAGE for both is much cheaper: both
programs (or, on Android, both native libraries) shipped together and the
one that matches the player's ROM started.

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
  kanji the game uses. "Enhancement Options" reaches its row's arrow.
- **Untested there**: the unit list's taps, opening a unit's profile from
  the world map,
  Fast Actions and the other hooks' behaviour, the stick cursor, story
  scenes' actors, sound placement, HD music, achievements, mods, versus mode
  (kol_vs_bot's heap addresses depend on the screen).
- **Two functions run interpreted** in the Japanese build (0x08012EDC,
  0x08012F4A); proposals in `regionmap/atoj/carried_proposals.toml`, not
  merged. gbarecomp's rule: a person reviews what goes into a game's
  config, so the translated game.toml should become a generated overlay
  under a small hand-written base.
- Android: `SetupActivity` still accepts only the USA ROM and the app is not
  built for Japan. One package for both ROMs or two is not decided (the
  user asked 2026-11-25 whether the two builds can be combined: see "Two
  builds" below).
- This branch starts at 9159d23: the uncommitted menu fix on `main`
  (kol_menu.cpp, the translation mod) is not in it, and will meet this
  branch's kol_menu.cpp changes when merged.
