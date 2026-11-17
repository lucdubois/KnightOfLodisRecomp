#pragma once

// HD instruments (Options > Sound: HD). In that mode gbarecomp's MP2K render
// plays soundfont instruments in place of the music's 8-bit samples and, in
// three songs, of the PSG channels (gbarecomp hd_instruments.h). The game's
// part is the bank file and the mapping: which instrument stands for which
// sample (src/kol_hd_map.inc, generated from tools/hd_audio/kol_hd_map.json).
//
// The bank (tools/hd_audio/build_bank.py, ~30-60 MB) is looked for at, in
// order: KOL_HD_BANK=<path>; on the desktop hd_instruments.bank next to the
// executable, then build/hd_instruments.bank from the working directory; on
// Android files/hd_instruments.bank, then the APK asset hd_instruments.bank
// (SDL_RWFromFile tries both for a relative path). It is read only when HD is
// picked, so other players pay nothing.

namespace kol {

// Load the bank and the mapping into gbarecomp, once. True when HD can play;
// false (logged once) when the bank is missing or bad: the caller then asks
// for HQ stereo instead. Later calls return the first answer.
bool hd_audio_activate();

}  // namespace kol
