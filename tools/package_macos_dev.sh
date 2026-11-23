#!/bin/sh
# Builds a DEV Mac app on this Mac: build/dev-app/"Knight of Lodis Recomp Dev.app".
#
#   tools/package_macos_dev.sh
#
# The dev build (build/, KOL_DEV_TOOLS=ON: the Debug page and the rest), packed
# as package_macos.sh packs the public one, with its own name and bundle id and
# its own data folder, ~/Library/Application Support/KnightOfLodisRecomp-Dev
# (KOL_DATA_DIR through the app's LSEnvironment), so the public app's save,
# settings and states stay apart. Put totkol.gba (and gba_bios.bin) there, and
# numbered states in its states/ (2026-11-21: a copy of states/thor).
# Not for release: nothing is zipped, and the dev tools are in it.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/.*versionName "\(.*\)".*/\1/p' "$ROOT/android/app/build.gradle" | head -1)"
[ -n "$VERSION" ] || { echo "no versionName in android/app/build.gradle"; exit 1; }
BUILD="$ROOT/build"
OUT="$ROOT/build/dev-app"
APPNAME="Knight of Lodis Recomp Dev"
DATADIR="$HOME/Library/Application Support/KnightOfLodisRecomp-Dev"
ZIP="KnightOfLodisRecomp-$VERSION-macos-arm64.zip"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cmake --build "$BUILD" --target KnightOfLodisRecomp > "$WORK/build.log" 2>&1 || { grep -E "error" -A3 "$WORK/build.log" | head -30; exit 1; }
BIN="$BUILD/KnightOfLodisRecomp"
[ -x "$BIN" ] || { echo "no build at $BIN"; exit 1; }
APP="$WORK/$APPNAME.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/KnightOfLodisRecomp"
strip -x "$APP/Contents/MacOS/KnightOfLodisRecomp"

echo "==> libraries"
# Every library a binary names outside the system's, copied in and renamed to
# its place in the bundle, and theirs in turn.
fix() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in /usr/lib/*|/System/*|@*) continue ;; esac
        base="$(basename "$dep")"
        if [ ! -f "$APP/Contents/Frameworks/$base" ]; then
            cp -L "$dep" "$APP/Contents/Frameworks/$base"
            chmod u+w "$APP/Contents/Frameworks/$base"
            install_name_tool -id "@rpath/$base" "$APP/Contents/Frameworks/$base"
            fix "$APP/Contents/Frameworks/$base"
        fi
        install_name_tool -change "$dep" "@rpath/$base" "$1"
    done
}
fix "$APP/Contents/MacOS/KnightOfLodisRecomp"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/KnightOfLodisRecomp" 2>/dev/null || true
# sdl2-compat loads SDL3 by name at run time (no link to see), first as
# @loader_path/libSDL3.dylib: under that name beside it. (As libSDL3.0.dylib it
# was not found, and sdl2-compat's error dialog held the app before main.)
SDL3="$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib 2>/dev/null || true)"
if [ -n "$SDL3" ] && [ ! -f "$APP/Contents/Frameworks/libSDL3.dylib" ]; then
    cp -L "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
    chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
    install_name_tool -id "@rpath/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
    fix "$APP/Contents/Frameworks/libSDL3.dylib"
fi
ls "$APP/Contents/Frameworks"

echo "==> resources"
[ -f "$ROOT/build/hd_instruments.bank" ] && cp "$ROOT/build/hd_instruments.bank" "$APP/Contents/Resources/"
cp "$ROOT/third_party/NOTICES-desktop.txt" "$APP/Contents/Resources/licenses.txt"
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
SRC="$ROOT/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"
for z in 16 32 64 128 256 512; do
    sips -z $z $z "$SRC" --out "$ICONSET/icon_${z}x${z}.png" > /dev/null
    z2=$((z * 2))
    sips -z $z2 $z2 "$SRC" --out "$ICONSET/icon_${z}x${z}@2x.png" > /dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APPNAME</string>
  <key>CFBundleDisplayName</key><string>$APPNAME</string>
  <key>CFBundleIdentifier</key><string>com.totkol.game.dev</string>
  <key>LSEnvironment</key><dict><key>KOL_DATA_DIR</key><string>$DATADIR</string><key>KOL_LOG_TO_FILE</key><string>1</string></dict>
  <key>CFBundleExecutable</key><string>KnightOfLodisRecomp</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>Knight of Lodis Recomp contributors. PolyForm Noncommercial 1.0.0.</string>
</dict>
</plist>
PLIST

echo "==> signature (ad hoc)"
codesign --force --sign - --timestamp=none "$APP/Contents/Frameworks/"*.dylib
codesign --force --sign - --timestamp=none "$APP"
codesign --verify --deep --strict "$APP"

mkdir -p "$OUT"
rm -rf "$OUT/$APPNAME.app"
ditto "$APP" "$OUT/$APPNAME.app"
ls -d "$OUT/$APPNAME.app"
