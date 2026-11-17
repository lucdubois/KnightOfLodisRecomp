#pragma once

// Developer tools in or out of a build (CMake option KOL_DEV_TOOLS, ON by
// default; the Android release build type turns it OFF). Off, a build is the
// public one: no Debug page in the options menu and every debug_* setting reads
// as off, no height/roof/box editor and no drawn_heights.cfg (the baked table is
// what draws), no miss recording and no files/env.cfg on Android, no `hold:` or
// memo log lines. The dev code still compiles either way (`if constexpr`), so it
// cannot rot unseen; the compiler leaves it out of a public build.
// tools/check_release_build.sh checks a release library for what must not ship.
#ifndef KOL_DEV_TOOLS
#define KOL_DEV_TOOLS 1
#endif

namespace kol {
inline constexpr bool kDevTools = KOL_DEV_TOOLS != 0;
}  // namespace kol
