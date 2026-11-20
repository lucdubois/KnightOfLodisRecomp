#!/bin/sh
# Builds the public macOS download: build/release/KnightOfLodisRecomp-<version>-macos-arm64.zip,
# holding "Knight of Lodis Recomp.app".
#
#   tools/package_macos.sh            build, then pack
#   tools/package_macos.sh --no-build pack the last build again
#
# The public build (Release, KOL_DEV_TOOLS=OFF) in build-macos/, on this Mac.
# The app: the game in Contents/MacOS, the libraries it takes from Homebrew in
# Contents/Frameworks (SDL2 is sdl2-compat there, which loads SDL3 at run
# time: both go in), the HD bank and the icon in Contents/Resources, and an
# ad-hoc signature (no Apple Developer ID: a downloaded copy is opened the
# first time through System Settings > Privacy & Security > Open Anyway).
#
# Apple Silicon only, and macOS 15 or newer: Homebrew's SDL libraries are
# built for this Mac's system. See docs/DEVLOG.md "macOS and Windows".
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/.*versionName "\(.*\)".*/\1/p' "$ROOT/android/app/build.gradle" | head -1)"
[ -n "$VERSION" ] || { echo "no versionName in android/app/build.gradle"; exit 1; }
BUILD="$ROOT/build-macos"
OUT="$ROOT/build/release"
APPNAME="Knight of Lodis Recomp"
ZIP="KnightOfLodisRecomp-$VERSION-macos-arm64.zip"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ "${1:-}" != "--no-build" ]; then
    echo "==> public build (Release, KOL_DEV_TOOLS=OFF)"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKOL_DEV_TOOLS=OFF > "$BUILD.configure.log" 2>&1 \
        || { tail -20 "$BUILD.configure.log"; exit 1; }
    cmake --build "$BUILD" --target KnightOfLodisRecomp > "$BUILD.log" 2>&1 || { grep -E "error" -A3 "$BUILD.log" | head -30; exit 1; }
fi
BIN="$BUILD/KnightOfLodisRecomp"
[ -x "$BIN" ] || { echo "no build at $BIN"; exit 1; }
for s in "Tools for testing." "Tile Info" "L2 Moves Sun" "Instant Win" "drawn_heights.cfg" "KOL_DESKTOP_MENU_AT" "KOL_SETUP_KEYS"; do
    if grep -a -q -F -- "$s" "$BIN"; then echo "FOUND a dev tool's string in the release binary: $s"; exit 1; fi
done

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
  <key>CFBundleIdentifier</key><string>com.totkol.game</string>
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
rm -f "$OUT/$ZIP"
( cd "$WORK" && ditto -c -k --sequesterRsrc --keepParent "$APPNAME.app" "$OUT/$ZIP" )
ls -la "$OUT/$ZIP"
