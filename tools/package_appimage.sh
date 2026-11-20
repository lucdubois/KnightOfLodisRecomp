#!/bin/sh
# Builds the public Linux / Steam Deck download as an AppImage: one file,
# build/release/KnightOfLodisRecomp-<version>-x86_64.AppImage, that the player
# marks executable and runs (no install, no password; it adds to Steam as a
# non-Steam game). See docs/DEVLOG.md "Steam Deck".
#
#   tools/package_appimage.sh            build on the Deck, then pack
#   tools/package_appimage.sh --no-build pack the last build again
#
# The build is tools/package_flatpak.sh's: on the Steam Deck (ssh "steamdeck",
# ~/kol), in the freedesktop SDK 24.08, Release, KOL_DEV_TOOLS=OFF, no -march.
# The libraries it needs from the SDK go in the AppImage (SDL2, curl and
# theirs, libstdc++); the system's are left to the system: the C library, the
# graphics and sound stacks, X11 and Wayland. So the AppImage needs a glibc at
# least as new as the SDK's (2.40): SteamOS 3.7 has it; an older distribution
# (Ubuntu 22.04) does not.
#
# The packing tool is appimagetool, put on the Deck by hand at
# ~/kol/appimage/tools/appimagetool-x86_64.AppImage (this script does not
# download it).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/.*versionName "\(.*\)".*/\1/p' "$ROOT/android/app/build.gradle" | head -1)"
[ -n "$VERSION" ] || { echo "no versionName in android/app/build.gradle"; exit 1; }
OUT="$ROOT/build/release"
NAME="KnightOfLodisRecomp-$VERSION-x86_64.AppImage"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# The AppImage's own files.
cat > "$STAGE/AppRun" <<'EOF'
#!/bin/sh
# Knight of Lodis Recomp: full screen, as on a handheld (--windowed for a
# window). Its libraries are in usr/lib beside it; its files in
# ~/.local/share/KnightOfLodisRecomp.
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fullscreen=--fullscreen
for a in "$@"; do [ "$a" = "--windowed" ] && fullscreen=; done
args=""
for a in "$@"; do [ "$a" = "--windowed" ] || args="$args \"$(printf '%s' "$a" | sed 's/"/\\"/g')\""; done
eval exec \"\$HERE/usr/bin/KnightOfLodisRecomp\" $fullscreen $args
EOF
cat > "$STAGE/knightoflodisrecomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Knight of Lodis Recomp
Comment=Tactics Ogre: The Knight of Lodis, recompiled (your own ROM is needed)
Exec=KnightOfLodisRecomp
Icon=knightoflodisrecomp
Categories=Game;StrategyGame;
Terminal=false
X-AppImage-Version=$VERSION
EOF
cp "$ROOT/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" "$STAGE/knightoflodisrecomp.png"
[ -f "$ROOT/build/hd_instruments.bank" ] && cp "$ROOT/build/hd_instruments.bank" "$STAGE/"
cp "$ROOT/third_party/NOTICES-desktop.txt" "$STAGE/licenses.txt"

if [ "${1:-}" != "--no-build" ]; then
    echo "==> sources to the Deck"
    cd "$ROOT/.."
    rsync -a --delete --exclude '/build*/' --exclude '/.git/' --exclude 'hang_*' --exclude '.DS_Store' gbarecomp/ steamdeck:kol/gbarecomp/
    rsync -a --delete --exclude '/build/' --exclude '/build-deck/' --exclude '/build-release/' --exclude '/build-android/' --exclude '/android/' --exclude '/states/' --exclude '/shots/' --exclude '/recomp_cache/' --exclude '/.git/' --exclude '/diagnostics_*/' --exclude '*.bmp' --exclude '*.state*' --exclude 'hang_*' --exclude '*.gba' --exclude '*.sav' --exclude '.DS_Store' --exclude '__pycache__/' KnightOfLodisRecomp/ steamdeck:kol/KnightOfLodisRecomp/
    echo "==> public build on the Deck (Release, KOL_DEV_TOOLS=OFF)"
    ssh steamdeck 'cd /home/deck/kol && ./sdk.sh "cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKOL_DEV_TOOLS=OFF > build-release.configure.log 2>&1 && cmake --build build-release --target KnightOfLodisRecomp > build-release.log 2>&1; echo BUILD_EXIT \$?" | tail -1'
fi
ssh steamdeck 'mkdir -p /home/deck/kol/appimage/stage'
rsync -a --delete "$STAGE"/ steamdeck:kol/appimage/stage/

echo "==> AppDir"
ssh steamdeck "NAME=$NAME sh -s" <<'REMOTE'
set -e
K=/home/deck/kol
cd $K/appimage
BIN=$K/KnightOfLodisRecomp/build-release/KnightOfLodisRecomp
[ -x "$BIN" ] || { echo "no release binary"; exit 1; }
for s in "Tools for testing." "Tile Info" "L2 Moves Sun" "Instant Win" "drawn_heights.cfg" "KOL_DESKTOP_MENU_AT" "KOL_SETUP_KEYS"; do
    if grep -a -q -F -- "$s" "$BIN"; then echo "FOUND a dev tool's string in the release binary: $s"; exit 1; fi
done
rm -rf AppDir
mkdir -p AppDir/usr/bin AppDir/usr/lib
cp "$BIN" AppDir/usr/bin/KnightOfLodisRecomp
[ -f stage/hd_instruments.bank ] && cp stage/hd_instruments.bank AppDir/usr/bin/
cp stage/AppRun AppDir/AppRun && chmod 755 AppDir/AppRun
cp stage/knightoflodisrecomp.desktop stage/knightoflodisrecomp.png AppDir/
ln -sf knightoflodisrecomp.png AppDir/.DirIcon
mkdir -p AppDir/usr/share/doc/knightoflodisrecomp && cp stage/licenses.txt AppDir/usr/share/doc/knightoflodisrecomp/
# Inside the SDK: strip the game, and copy every library it needs (and those
# libraries need) but the system's own.
$K/sdk.sh "cd $K/appimage && strip AppDir/usr/bin/KnightOfLodisRecomp && \
  ldd AppDir/usr/bin/KnightOfLodisRecomp | awk '/=> \// {print \$3}' | while read -r lib; do \
    n=\$(basename \$lib); \
    case \$n in \
      ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*) continue ;; \
      libGL*|libEGL*|libGLX*|libGLdispatch*|libOpenGL*|libdrm*|libgbm*|libvulkan*) continue ;; \
      libX*|libxcb*|libwayland-*|libxkbcommon*|libasound*|libpulse*|libdbus*|libudev*|libsystemd*) continue ;; \
    esac; \
    cp -L \$lib AppDir/usr/lib/\$n; \
  done"
desktop-file-validate AppDir/knightoflodisrecomp.desktop
ls AppDir/usr/lib | tr '\n' ' '; echo
du -sh AppDir | cut -f1
REMOTE

TOOL=/home/deck/kol/appimage/tools/appimagetool-x86_64.AppImage
if ! ssh steamdeck "test -x $TOOL"; then
    echo
    echo "The AppDir is ready on the Deck (~/kol/appimage/AppDir), but there is no"
    echo "appimagetool at $TOOL to pack it. Put it there, then:"
    echo "    tools/package_appimage.sh --no-build"
    exit 2
fi
echo "==> AppImage"
# (Written beside, then moved over the last: an AppImage still running from
# here made appimagetool fail with "Text file busy", 2026-11-18.)
ssh steamdeck "cd /home/deck/kol/appimage && rm -f $NAME.new && ARCH=x86_64 VERSION=$VERSION $TOOL --appimage-extract-and-run --no-appstream AppDir $NAME.new > appimagetool.log 2>&1 || { tail -15 appimagetool.log; exit 1; }; mv -f $NAME.new $NAME; ls -la $NAME"
mkdir -p "$OUT"
scp -q "steamdeck:kol/appimage/$NAME" "$OUT/$NAME"
chmod +x "$OUT/$NAME"
ls -la "$OUT/$NAME"
