#!/bin/sh
# Mirror the Thor's numbered save states (Options > Debug > Numbered States)
# into the project: states/thor/NNNN.state, .png (the picture) and .txt
# (Alphonse's level, the map, the time). Only reads the device. Run by the
# user (VS Code task "Sync Thor states"); then "Thor state 12" names
# states/thor/0012.state.
set -e
ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/states/thor"

"$ADB" get-state >/dev/null 2>&1 || { echo "Thor not connected (adb get-state failed)"; exit 1; }
# (exec-out does not pass the device's exit status back: look at what ls says.)
if [ -z "$("$ADB" exec-out "run-as com.koldecomp.game sh -c 'ls files/states 2>/dev/null'")" ]; then
    echo "No numbered states on the Thor yet (Debug > Numbered States, then L3+R)"
    exit 0
fi
# Pulled whole into a fresh folder, then mirrored: a state deleted on the
# Thor goes from states/thor/ too (nothing else is touched).
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$ADB" exec-out "run-as com.koldecomp.game sh -c 'cd files/states && tar -cf - .'" | tar -xf - -C "$TMP"
[ -f "$TMP/next" ] || ls "$TMP"/*.state >/dev/null 2>&1 || { echo "Pull failed"; exit 1; }
mkdir -p "$OUT"
rsync -a --delete "$TMP"/ "$OUT"/

echo "Thor states in $OUT:"
for t in $(ls "$OUT"/*.txt 2>/dev/null | sort); do
    n=$(basename "$t" .txt)
    level=$(sed -n 's/^level=//p' "$t")
    map=$(sed -n 's/^map=//p' "$t")
    when=$(sed -n 's/^time=//p' "$t")
    echo "  $n  Lv ${level:-?}  map ${map:-?}  $when"
done
