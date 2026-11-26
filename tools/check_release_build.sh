#!/bin/sh
# Checks that the public (release) build carries none of the developer tools
# (src/kol_dev.h, CMake option KOL_DEV_TOOLS=OFF, the Android release build type).
#
#   tools/check_release_build.sh            build the release APK, then check it
#   tools/check_release_build.sh --no-build check the last release build
#
# It looks for strings only the dev tools hold in the release libmain.so. Exit
# status 0: clean; 1: something found (listed); 2: no library to check.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "${1:-}" != "--no-build" ]; then
    (cd "$ROOT/android" && ./gradlew assembleRelease -q) || exit 2
fi
# Every game's library (libmain.so, and libmain_jp.so when the app holds the
# Japanese game too: docs/REGIONS.md), from the release build's merged libs.
DIR=$(find "$ROOT/android/app/build/intermediates" -path '*release*' -path '*arm64-v8a*' -name libmain.so 2>/dev/null | head -1)
[ -n "$DIR" ] || { echo "no release libmain.so under android/app/build/intermediates"; exit 2; }
DIR=$(dirname "$DIR")
found=0
for LIB in "$DIR"/libmain*.so; do
echo "checking $LIB"
# One string per tool: the Debug page, the editor and its file, the device
# switches file, the miss recording, the load-hold and memo logs.
for s in "Tools for testing." "Unlink ROM" "Restart on the setup screen" "Tile Info" "L2 Moves Sun" "Instant Win" "Return to Debug." "No Second Screen" \
         "drawn_heights.cfg" "Drawn heights set with" "env.cfg" "/misses" "/coverage_" \
         "hold: %llu frames" "memo %s hit" "memo %s recorded"; do
    if strings -a "$LIB" | grep -qF -- "$s"; then
        echo "  FOUND: $s"
        found=1
    fi
done
done
[ $found -eq 0 ] && echo "clean: none of the dev tools' strings are in the release libraries"
exit $found
