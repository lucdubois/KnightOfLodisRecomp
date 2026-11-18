#!/bin/sh
# Builds the public Linux / Steam Deck package: one file,
# build/release/KnightOfLodisRecomp-<version>-x86_64.flatpak.
#
#   tools/package_flatpak.sh
#
# The build runs on the Steam Deck (ssh host "steamdeck", folder ~/kol; see
# tools/deck_build.sh and docs/DEVLOG.md "Steam Deck"), inside the freedesktop
# SDK 24.08 already installed there: a Release build with the developer tools
# out (KOL_DEV_TOOLS=OFF) and no processor tuning, so it runs on any x86-64.
# The package is then put together with the flatpak command's own build-init /
# build-finish / build-export / build-bundle: the Deck has no flatpak-builder,
# and none is needed for a binary that is already built against the runtime.
#
# What goes in: the game, the HD instruments' bank beside it, a launcher that
# starts it full screen, the desktop entry, the icon, the metainfo. No ROM, no
# BIOS: the player picks them on the setup screen. generated/ (the recompiled
# game, from the ROM) must be up to date in the tree.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_ID="com.totkol.game"            # the public app's id, as Android's
RUNTIME="24.08"
VERSION="$(sed -n 's/.*versionName "\(.*\)".*/\1/p' "$ROOT/android/app/build.gradle" | head -1)"
[ -n "$VERSION" ] || { echo "no versionName in android/app/build.gradle"; exit 1; }
OUT="$ROOT/build/release"
BUNDLE="KnightOfLodisRecomp-$VERSION-x86_64.flatpak"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

ls "$ROOT"/generated/recompiled_*.cpp >/dev/null 2>&1 || { echo "generated/ is empty: run gba_recompile first"; exit 1; }
[ -f "$ROOT/build/hd_instruments.bank" ] || echo "warning: no build/hd_instruments.bank: HD will play as HQ"

# The package's own files.
cat > "$STAGE/knightoflodis" <<'EOF'
#!/bin/sh
# Full screen, as on a handheld; --windowed on the command line for a window.
for a in "$@"; do
    if [ "$a" = "--windowed" ]; then
        exec /app/bin/KnightOfLodisRecomp $(for b in "$@"; do [ "$b" = "--windowed" ] || printf '%s ' "$b"; done)
    fi
done
exec /app/bin/KnightOfLodisRecomp --fullscreen "$@"
EOF
cat > "$STAGE/$APP_ID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Knight of Lodis Recomp
Comment=Tactics Ogre: The Knight of Lodis, recompiled (your own ROM is needed)
Exec=knightoflodis
Icon=$APP_ID
Categories=Game;StrategyGame;
Terminal=false
EOF
cat > "$STAGE/$APP_ID.metainfo.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>$APP_ID</id>
  <name>Knight of Lodis Recomp</name>
  <summary>Tactics Ogre: The Knight of Lodis, recompiled</summary>
  <developer id="io.github.knightoflodisrecomp">
    <name>Knight of Lodis Recomp contributors</name>
  </developer>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>PolyForm-Noncommercial-1.0.0</project_license>
  <description>
    <p>A native port of Tactics Ogre: The Knight of Lodis (Game Boy Advance), with optional enhancements. The game is not included: you select your own copy of the US ROM on first start.</p>
  </description>
  <launchable type="desktop-id">$APP_ID.desktop</launchable>
  <url type="homepage">https://github.com/lucdubois/KnightOfLodisRecomp</url>
  <content_rating type="oars-1.1"/>
  <categories><category>Game</category><category>StrategyGame</category></categories>
  <releases><release version="$VERSION" date="$(date +%Y-%m-%d)"/></releases>
</component>
EOF
# The Android launcher's icon (192 px), and the 128 and 64 px a store's
# catalogue looks for (appstreamcli: icon-not-found without them).
cp "$ROOT/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" "$STAGE/$APP_ID.png"
for z in 128 64; do sips -z $z $z "$STAGE/$APP_ID.png" --out "$STAGE/$APP_ID-$z.png" > /dev/null; done
[ -f "$ROOT/build/hd_instruments.bank" ] && cp "$ROOT/build/hd_instruments.bank" "$STAGE/"
cp "$ROOT/android/app/src/main/assets/licenses.txt" "$STAGE/licenses.txt"

echo "==> sources to the Deck"
cd "$ROOT/.."
rsync -a --delete --exclude '/build*/' --exclude '/.git/' --exclude 'hang_*' --exclude '.DS_Store' gbarecomp/ steamdeck:kol/gbarecomp/
rsync -a --delete --exclude '/build/' --exclude '/build-deck/' --exclude '/build-release/' --exclude '/build-android/' --exclude '/android/' --exclude '/states/' --exclude '/shots/' --exclude '/recomp_cache/' --exclude '/.git/' --exclude '/diagnostics_*/' --exclude '*.bmp' --exclude '*.state*' --exclude 'hang_*' --exclude '*.gba' --exclude '*.sav' --exclude '.DS_Store' --exclude '__pycache__/' KnightOfLodisRecomp/ steamdeck:kol/KnightOfLodisRecomp/
ssh steamdeck 'mkdir -p /home/deck/kol/flatpak/stage'
rsync -a --delete "$STAGE"/ steamdeck:kol/flatpak/stage/

echo "==> public build on the Deck (Release, KOL_DEV_TOOLS=OFF)"
ssh steamdeck 'cd /home/deck/kol && ./sdk.sh "cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKOL_DEV_TOOLS=OFF > build-release.configure.log 2>&1 && cmake --build build-release --target KnightOfLodisRecomp > build-release.log 2>&1; echo BUILD_EXIT \$?" | tail -1; tail -1 KnightOfLodisRecomp/build-release.log'

echo "==> package"
ssh steamdeck "APP_ID=$APP_ID RUNTIME=$RUNTIME BUNDLE=$BUNDLE sh -s" <<'REMOTE'
set -e
cd /home/deck/kol/flatpak
BIN=/home/deck/kol/KnightOfLodisRecomp/build-release/KnightOfLodisRecomp
[ -x "$BIN" ] || { echo "no release binary"; exit 1; }
# No developer tool may be in a public build (as tools/check_release_build.sh).
for s in "Tools for testing." "Tile Info" "L2 Moves Sun" "Instant Win" "drawn_heights.cfg" "KOL_DESKTOP_MENU_AT" "KOL_SETUP_KEYS"; do
    if grep -a -q -F -- "$s" "$BIN"; then echo "FOUND a dev tool's string in the release binary: $s"; exit 1; fi
done
rm -rf /home/deck/kol/flatpak/appdir /home/deck/kol/flatpak/repo
flatpak build-init appdir "$APP_ID" org.freedesktop.Sdk org.freedesktop.Platform "$RUNTIME" > /dev/null
install -Dm755 "$BIN" appdir/files/bin/KnightOfLodisRecomp
flatpak run --filesystem=/home/deck/kol --command=strip org.freedesktop.Sdk//"$RUNTIME" /home/deck/kol/flatpak/appdir/files/bin/KnightOfLodisRecomp
install -Dm755 stage/knightoflodis appdir/files/bin/knightoflodis
[ -f stage/hd_instruments.bank ] && install -Dm644 stage/hd_instruments.bank appdir/files/bin/hd_instruments.bank
install -Dm644 stage/licenses.txt appdir/files/share/licenses/"$APP_ID"/licenses.txt
install -Dm644 "stage/$APP_ID.desktop" "appdir/files/share/applications/$APP_ID.desktop"
install -Dm644 "stage/$APP_ID.png" "appdir/files/share/icons/hicolor/192x192/apps/$APP_ID.png"
for z in 128 64; do install -Dm644 "stage/$APP_ID-$z.png" "appdir/files/share/icons/hicolor/${z}x${z}/apps/$APP_ID.png"; done
install -Dm644 "stage/$APP_ID.metainfo.xml" "appdir/files/share/metainfo/$APP_ID.metainfo.xml"
# Graphics, sound, every controller; the network for RetroAchievements; the
# home folder and removable media, read, for the setup screen's file browser.
flatpak build-finish appdir --command=knightoflodis \
    --socket=x11 --socket=wayland --socket=pulseaudio --device=all --share=ipc --share=network \
    --filesystem=home:ro --filesystem=/run/media:ro --filesystem=/media:ro > /dev/null
# The catalogue entry a store shows before installing (Discover read the
# bundle without one: the app id for a name, "Unknown author", "Licenses:
# Unknown"): the metainfo compiled into share/app-info by appstreamcli, in
# the app's own build environment, then the repo's appstream branch that
# build-bundle carries along.
flatpak build appdir appstreamcli compose --origin="$APP_ID" --result-root=/app --prefix=/ \
    --data-dir=/app/share/app-info/xmls --icons-dir=/app/share/app-info/icons/flatpak /app > compose.log 2>&1 \
    || { echo "appstreamcli compose failed:"; tail -20 compose.log; exit 1; }
ls appdir/files/share/app-info/xmls/ > /dev/null
flatpak build-export repo appdir stable > /dev/null
flatpak build-update-repo repo > /dev/null
# (No --runtime-repo: naming Flathub made Discover add it as a source on
# install, a privileged change that asked for the deck password, 2026-11-18.
# Flathub is set up on every Steam Deck; elsewhere the runtime comes from
# whatever Flathub remote the system has.)
flatpak build-bundle repo "$BUNDLE" "$APP_ID" stable
ls -la "$BUNDLE"
REMOTE

mkdir -p "$OUT"
scp -q "steamdeck:kol/flatpak/$BUNDLE" "$OUT/$BUNDLE"
ls -la "$OUT/$BUNDLE"
echo "Install on a Deck or any Linux with Flatpak:  flatpak install --user $BUNDLE"
