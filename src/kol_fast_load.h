// kol_fast_load.h — screens that load quicker than the cartridge did.
//
// Coming back to the battle map from View List, the game rebuilds the map
// screen inside its VBlank handler: ~54 frames (0.9 s) during which nothing
// new is shown (README "Making the return from View List quick"). Most of
// that is work with an exact native equivalent: the BIOS's Huffman and LZ77
// decompressors (bit by bit on the hardware), the game's own A7 asset decoder
// (src/kol_a7.cpp, verified byte for byte), and a byte-at-a-time memset. With
// Options > Advanced > Fast Loading on, each runs natively: the same bytes
// land in the same places, the registers return as the routine's would, and
// the guest clock does not move, so the game gets on with the next step in
// the same frame. Off (the default, and the Original preset), nothing here
// runs and the build is byte-identical to one without it.
//
// KOL_FAST_LOAD_CHECK=1: each hook works out its result, then DECLINES so the
// game does its own work, and the bytes are compared afterwards (at the next
// hook, or the frame hook); the count of matches and mismatches is logged.

#pragma once

namespace kol {

// Register the hooks (game.toml [[mod_function_hook]] 0x08004990 and
// 0x08002F92; the runtime's g_bios_hle_hook for SWIs 0x11-0x13). Once.
void fast_load_install();

// From the frame hook, first thing at every real (not speculative) VBlank
// start: a "hold:" line for every stretch of more than 3 game frames between
// two presents, with its wall time.
void fast_load_present();

// From the frame hook, later: the memo's deferred save, the check mode's log.
void fast_load_frame();

}  // namespace kol
