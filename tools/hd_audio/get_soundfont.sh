#!/bin/sh
# Fetch FluidR3_GM.sf2 (MIT, see LICENSE-FluidR3.txt) into tools/hd_audio/,
# the soundfont build_bank.py reads. From Debian's fluid-soundfont-gm
# package: the .deb is downloaded to a temporary directory, the one .sf2 is
# extracted and checked against the copy the HD instruments were chosen on,
# and the rest is deleted. Does nothing if the file is already there.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/FluidR3_GM.sf2"
DEB="fluid-soundfont-gm_3.1-6_all.deb"
URL="https://deb.debian.org/debian/pool/main/f/fluid-soundfont/$DEB"
SHA256="74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0"

if [ -f "$OUT" ]; then
    echo "$OUT already there"
    exit 0
fi
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "Downloading $URL (~120 MB)"
curl -fL --retry 3 -o "$TMP/$DEB" "$URL"
(cd "$TMP" && ar x "$DEB")
DATA="$(ls "$TMP"/data.tar.* | head -n 1)"
tar -xf "$DATA" -C "$TMP" ./usr/share/sounds/sf2/FluidR3_GM.sf2
GOT="$(sha256 "$TMP/usr/share/sounds/sf2/FluidR3_GM.sf2")"
if [ "$GOT" != "$SHA256" ]; then
    echo "FluidR3_GM.sf2 sha256 $GOT, expected $SHA256: not installed" >&2
    exit 1
fi
mv "$TMP/usr/share/sounds/sf2/FluidR3_GM.sf2" "$OUT"
echo "Wrote $OUT"
