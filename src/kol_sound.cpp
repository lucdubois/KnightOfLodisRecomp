#include "kol_sound.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gba_audio.h"
#include "kol_focus.h"
#include "kol_settings.h"
#include "kol_widescreen.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()
#include "gba_bus.h"
#include "kol_region.h"

namespace kol {
namespace {

// The game's highlighted unit (a unit record), the target in an attack.
constexpr std::uint32_t kHighlightedUnitPtr = usa(0x02028960u);

// Hardware pixels from the middle at which a sound is fully to one side,
// and how far: the action sits near the middle (see kol_sound.h), so a
// narrower span than the view keeps it audible, and 0.9 never leaves one
// ear silent.
constexpr float kPanSpan = 96.0f;
constexpr float kPanMax = 0.9f;
// Up the screen is farther away on the battle map: up to 20% quieter at
// the top of the original frame.
constexpr float kFarLoss = 0.2f;

std::uint32_t highlighted_record() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew) return 0u;
    const std::uint8_t* p = ew + (kHighlightedUnitPtr - 0x02000000u);
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

void sound_frame(const gbarecomp::ExtendedViewFrameInfo*) {
    // KOL_MUSIC_QUALITY=0|1|2 (desktop runs, which load no enhancements.cfg):
    // Original / HQ / HD, as the menu sets it, from the first frame.
    static const bool quality_done = [] {
        if (const char* e = std::getenv("KOL_MUSIC_QUALITY"))
            if (*e) sound_choice_set(static_cast<unsigned>(std::atoi(e)));
        return true;
    }();
    (void)quality_done;
    if (!setting_on(Setting::EffectsPlaced) || !is_battle_scene_now()) {
        gba::gba_audio_set_effects_place(false, 0.0f, 1.0f);
        return;
    }
    int x = 0, y = 0;
    bool found = false;
    if (cursor_on_map()) {
        found = cursor_screen_pos(&x, &y);
    } else if (damage_number_shown()) {
        found = object_screen_pos(object_of_record(highlighted_record()), &x, &y);
    }
    if (!found) found = object_screen_pos(acting_object(), &x, &y);
    if (!found) {
        gba::gba_audio_set_effects_place(false, 0.0f, 1.0f);
        return;
    }
    const float pan = std::min(std::max((static_cast<float>(x) - 120.0f) / kPanSpan, -1.0f), 1.0f) * kPanMax;
    const float far = std::min(std::max((80.0f - static_cast<float>(y)) / 80.0f, 0.0f), 1.0f);
    gba::gba_audio_set_effects_place(true, pan, 1.0f - kFarLoss * far);
    // KOL_SOUND_LOG=1: the source whenever it moves (desktop A/B).
    static const bool log = std::getenv("KOL_SOUND_LOG") != nullptr;
    static int last_x = -9999, last_y = -9999;
    if (log && (x != last_x || y != last_y)) {
        std::fprintf(stderr, "sound: source (%d, %d) pan %+.2f gain %.2f\n", x, y, pan,
                     1.0f - kFarLoss * far);
        last_x = x;
        last_y = y;
    }
}

}  // namespace kol
