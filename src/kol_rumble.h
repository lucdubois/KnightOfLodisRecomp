#pragma once

#include <cstdint>

// Rumble (Options > Rumble, kol_settings rumble_choice_*).
//
// An effect is a waveform: steps of a duration and a strength 0..1, played
// once and scaled by the player's choice (Off, Light, Medium, Strong). What
// plays it is the platform's: on Android the device's own vibrator through
// Android's Vibrator (the Thor's controller has no motors of its own; its
// vibrator has amplitude control, so the steps are played as they are), on
// the desktop the open controller's motors through gbarecomp_host_rumble,
// stepped a guest frame at a time (rumble_frame).
//
// Effects are started by what the game does, through declining hooks: the
// first is the thunder (MP2K song 205, started by m4aSongNumStart; map 102's
// opening, the rain battles). KOL_RUMBLE_LOG=1 logs every effect played,
// with its strength, whatever plays it (headless runs too).
namespace kol {

struct RumbleStep {
    std::uint16_t ms;
    float level;   // 0..1
};

// A platform's player: the whole waveform at once, durations in ms and
// levels 0..1 already scaled. Android sets one (main_android.cpp); with none
// the controller is stepped through rumble_frame.
using RumbleBackend = void (*)(const int* ms, const float* level, int n);
void rumble_set_backend(RumbleBackend backend);

// Plays an effect at the player's strength (nothing when Off). `why` names
// it in the log.
void rumble_play(const RumbleStep* steps, int n, const char* why);

// The menu's preview: a short pulse at the strength just chosen.
void rumble_test();

// Installs the hooks that start the effects (the thunder). Once.
void rumble_install();

// How many times song `song` was started (m4aSongNumStart, the hook
// above), for effects that follow a sound (map 71's door). 0..1023.
unsigned song_starts(int song);

// Every guest frame: steps the controller's motors through the effect
// playing (no backend set).
void rumble_frame();

// Every guest frame, with its I/O registers: effects that follow a scene's
// picture rather than a sound (Shaher's death on map 57, story scene 179),
// and a blow landing (a new damage number that is damage, not a heal).
void rumble_scene_frame(const std::uint8_t* io);

}  // namespace kol
