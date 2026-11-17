#!/bin/sh
# Install the last built APK on the Thor and start it: back up the save
# slots first (one set, overwritten each time), `adb install -r` (which
# stops the game), then launch it. Run by the user (Ctrl+Option+I in VS Code).
set -e
ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
BACKUP="$HOME/thor_slots_backup"

[ -f "$APK" ] || { echo "No APK at $APK (build it first)"; exit 1; }
"$ADB" get-state >/dev/null 2>&1 || { echo "Thor not connected (adb get-state failed)"; exit 1; }

echo "APK: $(date -r "$APK" '+%Y-%m-%d %H:%M')  $(du -h "$APK" | cut -f1)"
mkdir -p "$BACKUP"
for f in totkol.state1 totkol.state2 totkol.state3 totkol.sav enhancements.cfg; do
    "$ADB" exec-out "run-as com.koldecomp.game cat files/$f" > "$BACKUP/$f" 2>/dev/null || true
done
echo "Slots backed up to $BACKUP"
"$ADB" install -r "$APK"
# The install stops the game; start it again.
"$ADB" shell monkey -p com.koldecomp.game -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 &&
    echo "Launched" || echo "Launch failed"
