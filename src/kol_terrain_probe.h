// kol_terrain_probe.h — see kol_terrain_probe.cpp. Diagnostic only.
#pragma once
#include <cstdint>
namespace kol {
// Arms the function-entry probe when KOL_TERRAIN_PROBE=1. No-op otherwise.
void install_terrain_probe(const std::uint8_t* rom, std::uint32_t rom_size);
// Advances the synthetic cursor input when KOL_TERRAIN_INPUT=1. No-op otherwise.
void terrain_probe_tick(std::uint64_t frame);
// Latest terrain id the guest looked up, or -1 before the first lookup.
int current_terrain_id();
// Six signed elemental modifiers for an id, in screen row-major order:
// orange, red, green, blue, white, grey. False if the id is out of range.
bool terrain_modifiers(int id, int out[6]);
// Rasterises the guest's own top-right terrain box straight out of the UI
// tilemaps into ARGB. False when nothing is drawn there. Buffer must hold at
// least terrain_box_pixels() entries.
bool capture_terrain_box(std::uint32_t* argb, int* w, int* h);
int terrain_box_pixels();
// True when the guest's terrain box is actually drawn in its rect. False when
// something else occupies it — a NAVI dialogue, or no tile highlighted.
bool terrain_box_on_screen();
// The unit panel's BG3-only frame: the game's cream box with no contents.
bool capture_unit_panel_frame(std::uint32_t* argb, int* w, int* h);
int unit_panel_frame_pixels();
// The game's glyphs: 80x16 atlas. Row 0 is digits 0-9 (8x8 each); row 1 is
// HP, MP, Lv., EXP (16x8 each), each with its own palette bank.
bool capture_font_atlas(std::uint32_t* argb, int* w, int* h);
int font_atlas_pixels();
// STR:/INT:/AGI: labels, 72x8. Only capturable while the status screen is up;
// the caller caches them.
bool capture_status_labels(std::uint32_t* argb, int* w, int* h);
int status_label_pixels();
// The unit's name as the game rendered it: 64x16.
bool capture_unit_name(std::uint32_t* argb, int* w, int* h);
int unit_name_pixels();
}
