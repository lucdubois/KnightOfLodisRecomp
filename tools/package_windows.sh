#!/bin/sh
# Builds the public Windows download: build/release/KnightOfLodisRecomp-<version>-windows-x64.zip,
# holding a folder "Knight of Lodis Recomp" with KnightOfLodisRecomp.exe, the HD
# instruments' bank beside it, licenses.txt and README.txt.
#
#   tools/package_windows.sh            build, then pack
#   tools/package_windows.sh --no-build pack the last build again
#
# Cross-compiled on this Mac with Homebrew's MinGW-w64 (brew install mingw-w64),
# CMake toolchain tools/mingw-w64-x86_64.cmake. SDL2 is built here from its
# release source, the version and checksum gbarecomp's Flatpak pins, as a static
# library (build-windows-deps/). The public build (Release, KOL_DEV_TOOLS=OFF)
# in build-windows/ links SDL2 and the C++ runtime in (GBARECOMP_STATIC_RELEASE):
# one .exe, no DLLs. Achievements over WinHTTP (src/kol_cheevos_winhttp.cpp),
# the setup screen's files through the system's Open dialog
# (src/kol_setup_win.cpp), the icon and a UTF-8 manifest (src/kol_windows.rc).
#
# x86-64, Windows 10 or newer (the UTF-8 code page needs 1903). Not signed:
# SmartScreen warns about a downloaded copy once ("More info", "Run anyway").
#
# Both games when the Japanese ROM's recompiled code is there (generated_atoj,
# docs/REGIONS.md): KnightOfLodisRecomp-jp.exe (build-windows-atoj) beside the
# USA one, which hands over to it when the Japanese game is the one to play.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/.*versionName "\(.*\)".*/\1/p' "$ROOT/android/app/build.gradle" | head -1)"
[ -n "$VERSION" ] || { echo "no versionName in android/app/build.gradle"; exit 1; }
DEPS="$ROOT/build-windows-deps"
BUILD="$ROOT/build-windows"
BUILD_JP="$ROOT/build-windows-atoj"
OUT="$ROOT/build/release"
JAPAN=0
[ -f "$ROOT/generated_atoj/dispatch_table.cpp" ] && JAPAN=1
TOOLCHAIN="$ROOT/tools/mingw-w64-x86_64.cmake"
FOLDER="Knight of Lodis Recomp"
ZIP="KnightOfLodisRecomp-$VERSION-windows-x64.zip"
SDL_VERSION=2.30.9
SDL_SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
command -v x86_64-w64-mingw32-g++ > /dev/null || { echo "MinGW-w64 is missing: brew install mingw-w64"; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ "${1:-}" != "--no-build" ]; then
    if [ ! -f "$DEPS/prefix/lib/libSDL2.a" ]; then
        echo "==> SDL2 $SDL_VERSION for Windows (static)"
        TAR="$DEPS/download/SDL2-$SDL_VERSION.tar.gz"
        mkdir -p "$DEPS/download"
        [ -f "$TAR" ] || curl -sSLf -o "$TAR" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
        echo "$SDL_SHA256  $TAR" | shasum -a 256 -c - > /dev/null || { echo "SDL2: wrong checksum"; exit 1; }
        rm -rf "$DEPS/src" "$DEPS/sdl-build" "$DEPS/prefix"
        mkdir -p "$DEPS/src"
        tar -xzf "$TAR" -C "$DEPS/src"
        cmake -S "$DEPS/src/SDL2-$SDL_VERSION" -B "$DEPS/sdl-build" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DKOL_WIN_PREFIX="$DEPS/prefix" \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS/prefix" \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF > "$DEPS/sdl-configure.log" 2>&1 \
            || { tail -20 "$DEPS/sdl-configure.log"; exit 1; }
        cmake --build "$DEPS/sdl-build" > "$DEPS/sdl-build.log" 2>&1 || { tail -20 "$DEPS/sdl-build.log"; exit 1; }
        cmake --install "$DEPS/sdl-build" > /dev/null
    fi
    echo "==> public build (Release, KOL_DEV_TOOLS=OFF, static)"
    P="$DEPS/prefix"
    # One build directory a game: the USA one, then the Japanese one's.
    for b in "$BUILD" "$BUILD_JP"; do
        region=ATOE
        if [ "$b" = "$BUILD_JP" ]; then
            [ "$JAPAN" = 1 ] || continue
            region=ATOJ
            echo "==> the Japanese game's program (KOL_REGION=ATOJ)"
        fi
        cmake -S "$ROOT" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DKOL_WIN_PREFIX="$P" \
            -DCMAKE_BUILD_TYPE=Release -DKOL_DEV_TOOLS=OFF -DKOL_REGION=$region \
            -DGBARECOMP_STATIC_RELEASE=ON -DGBARECOMP_MINGW_PREFIX_UNIX="$P" -DSDL2_INCLUDE_DIR="$P/include/SDL2" \
            "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++" > "$b.configure.log" 2>&1 \
            || { tail -20 "$b.configure.log"; exit 1; }
        cmake --build "$b" --target KnightOfLodisRecomp > "$b.log" 2>&1 \
            || { grep -E "error" -A3 "$b.log" | head -30; exit 1; }
    done
fi
BIN="$BUILD/KnightOfLodisRecomp.exe"
BIN_JP="$BUILD_JP/KnightOfLodisRecomp-jp.exe"
[ -f "$BIN" ] || { echo "no build at $BIN"; exit 1; }
if [ "$JAPAN" = 1 ] && [ ! -f "$BIN_JP" ]; then echo "no build at $BIN_JP"; exit 1; fi
for b in "$BIN" "$BIN_JP"; do
    [ -f "$b" ] || continue
    for s in "Tools for testing." "Tile Info" "L2 Moves Sun" "Instant Win" "drawn_heights.cfg" "KOL_DESKTOP_MENU_AT" "KOL_SETUP_KEYS"; do
        if grep -a -q -F -- "$s" "$b"; then echo "FOUND a dev tool's string in the release binary $b: $s"; exit 1; fi
    done
    # No DLL but Windows' own.
    DLLS="$(x86_64-w64-mingw32-objdump -p "$b" | sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)"
    for d in $DLLS; do
        case "$d" in
            kernel32.dll|user32.dll|gdi32.dll|winmm.dll|imm32.dll|ole32.dll|oleaut32.dll|version.dll|advapi32.dll|\
            setupapi.dll|shell32.dll|winhttp.dll|comdlg32.dll|msvcrt.dll|ws2_32.dll|api-ms-win-*|ucrtbase.dll|\
            bcrypt.dll|dbghelp.dll|psapi.dll|dinput8.dll|hid.dll|cfgmgr32.dll|dwmapi.dll) ;;
            *) echo "$b needs a DLL Windows does not have: $d"; exit 1 ;;
        esac
    done
done

echo "==> pack"
DIR="$WORK/$FOLDER"
mkdir -p "$DIR"
cp "$BIN" "$DIR/KnightOfLodisRecomp.exe"
x86_64-w64-mingw32-strip "$DIR/KnightOfLodisRecomp.exe"
if [ "$JAPAN" = 1 ]; then
    cp "$BIN_JP" "$DIR/KnightOfLodisRecomp-jp.exe"
    x86_64-w64-mingw32-strip "$DIR/KnightOfLodisRecomp-jp.exe"
fi
GAMES="(Tactics Ogre: The Knight of Lodis, USA)"
[ "$JAPAN" = 1 ] && GAMES="(Tactics Ogre: The Knight of Lodis, USA,
or Tactics Ogre Gaiden: The Knight of Lodis, Japan)"
[ -f "$ROOT/build/hd_instruments.bank" ] && cp "$ROOT/build/hd_instruments.bank" "$DIR/"
cp "$ROOT/third_party/NOTICES-desktop.txt" "$DIR/licenses.txt"
# Windows line ends, for Notepad.
sed 's/$/\r/' > "$DIR/README.txt" <<README
Knight of Lodis Recomp $VERSION for Windows (64-bit, Windows 10 or newer)

Start KnightOfLodisRecomp.exe. The first time, it asks for your own copy of
the game $GAMES
and of the GBA BIOS; it keeps copies of them, your saves and your settings
in %APPDATA%\\KnightOfLodisRecomp.

Windows may warn that the program is from an unknown publisher (it is not
signed): choose "More info", then "Run anyway".

Keyboard: arrows = D-pad, X = A, Z = B, Return = Start, Right Shift = Select,
Q = L, E = R, Escape or F10 = the options menu, Alt+Return = full screen.
A game controller works too (the right stick's click opens the menu).

Keep hd_instruments.bank next to the .exe (the HD sound option uses it).
licenses.txt lists the licences of the parts it is made of.
README
mkdir -p "$OUT"
rm -f "$OUT/$ZIP"
( cd "$WORK" && zip -qr -X "$OUT/$ZIP" "$FOLDER" )
ls -la "$OUT/$ZIP"
