#!/usr/bin/env python3
"""Assemble the third-party notices the game ships with, from the licence files
themselves so the two cannot drift apart. Run it again whenever a dependency
or its licence changes.

  android/app/src/main/assets/licenses.txt  the Android app (Options > Licenses)
  third_party/NOTICES-desktop.txt           the desktop builds: built into the
      program for Options > Licenses (CMakeLists.txt) and put beside it as
      licenses.txt by tools/package_{appimage,flatpak,macos,windows}.sh.

The desktop text adds what only the desktop builds hold: the setup screen's
typeface and text renderer, and the libraries each package carries (the
AppImage's from the freedesktop SDK 24.08, the Windows .exe's MinGW-w64
runtime, the Mac app's SDL3). Their licence texts are in third_party/licenses/
(fetched from each project at the version the packages carry, 2026-11-20).
The desktop text is ASCII: the page that shows it draws ASCII only.
"""
import os
import unicodedata

KOL = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GBA = os.path.abspath(os.path.join(KOL, "..", "gbarecomp"))
LIC = os.path.join(KOL, "third_party", "licenses")
OUT_ANDROID = os.path.join(KOL, "android", "app", "src", "main", "assets", "licenses.txt")
OUT_DESKTOP = os.path.join(KOL, "third_party", "NOTICES-desktop.txt")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def lic(name):
    return read(os.path.join(LIC, name))


def stb_licence():
    # The licence block at the end of stb_image.h (MIT or public domain).
    text = read(os.path.join(KOL, "third_party", "stb", "stb_image.h"))
    start = text.rfind("This software is available under 2 licenses")
    block = text[start:] if start >= 0 else ""
    return block.replace("*/", "").strip()


COMMON = [
    ("Knight of Lodis Recomp",
     # (Paragraphs without hard breaks: the app's dialog wraps them.)
     "This app runs the player's own copy of Tactics Ogre: The Knight of Lodis. "
     "The game, its ROM and the GBA BIOS are copyrighted by their respective "
     "owners and are not distributed with this app.\n\n"
     "Knight of Lodis Recomp is licensed under the PolyForm Noncommercial License "
     "1.0.0 (https://polyformproject.org/licenses/noncommercial/1.0.0).\n"
     "Required Notice: Copyright (c) 2026 Knight of Lodis Recomp contributors"),
    ("gbarecomp (the GBA recompilation framework)",
     read(os.path.join(GBA, "LICENSE"))),
    ("arm-recomp-core (part of gbarecomp)",
     read(os.path.join(GBA, "external", "arm-recomp-core", "LICENSE"))),
    ("JRickey/gba-recomp (ported into gbarecomp)",
     "https://github.com/JRickey/gba-recomp, by Jrickey. License: MIT OR Apache-2.0. "
     "Ported: the MP2K sound driver detection, the screen colour profiles, the "
     "cartridge RTC and the idea of the high-quality MP2K re-render."),
    ("mGBA (portions of gbarecomp's BIOS replacements)",
     "https://github.com/mgba-emu/mgba, by Jeffrey Pfau and contributors. "
     "License: Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/). "
     "gbarecomp's src/runtime/bios_hle.cpp derives in part from mGBA's "
     "src/gba/bios.c; that source is available from the mGBA repository."),
    ("SDL (Simple DirectMedia Layer)",
     read(os.path.join(KOL, "android", "app", "jni", "SDL", "LICENSE.txt"))),
    ("rcheevos (RetroAchievements)",
     read(os.path.join(KOL, "third_party", "rcheevos", "LICENSE"))),
    ("stb_image (achievement badges)", stb_licence()),
    ("FluidR3 GM soundfont (the HD instruments)",
     read(os.path.join(KOL, "tools", "hd_audio", "LICENSE-FluidR3.txt"))),
]

LGPL_NOTE = (
    "Used under the GNU Lesser General Public License version 3 (its text and the "
    "GNU General Public License version 3 it builds on are at the end of this "
    "file). It is an unmodified shared library in the AppImage's usr/lib, which "
    "can be replaced by another build of it. Its source is the freedesktop SDK "
    "24.08's (https://gitlab.com/freedesktop-sdk/freedesktop-sdk) and the "
    "project's own: ")

DESKTOP = [
    ("Roboto (the typeface of the setup screen and the licenses page)",
     "Copyright 2011 Google Inc. Licensed under the Apache License, Version 2.0; "
     "its text is under \"Apache License 2.0\" below."),
    ("stb_truetype (text rendering in the setup screen and the licenses page)",
     "By Sean Barrett and contributors. Public domain (https://github.com/nothings/stb)."),
    # The Linux AppImage's libraries (freedesktop SDK 24.08).
    ("libcurl 8.11 (Linux AppImage: achievements' HTTPS)", lic("curl.txt")),
    ("OpenSSL 3.3 (Linux AppImage)",
     "Copyright 1995-2024 The OpenSSL Project Authors. All Rights Reserved. "
     "Licensed under the Apache License, Version 2.0; its text is under "
     "\"Apache License 2.0\" below. https://www.openssl.org"),
    ("nghttp2 1.65 (Linux AppImage)", lic("nghttp2.txt")),
    ("libpsl 0.21 (Linux AppImage)", lic("libpsl.txt")),
    ("libidn2 2.3 (Linux AppImage)",
     "Copyright Simon Josefsson, Tim Ruehsen and others. Dual-licensed under the "
     "GNU LGPLv3+ or the GNU GPLv2+. " + LGPL_NOTE + "https://gitlab.com/libidn/libidn2"),
    ("libunistring (Linux AppImage)",
     "Copyright Free Software Foundation, Inc. Dual-licensed under the GNU "
     "LGPLv3+ or the GNU GPLv2+. " + LGPL_NOTE + "https://www.gnu.org/software/libunistring/"),
    ("zlib 1.3 (Linux AppImage)", lic("zlib.txt")),
    ("libffi 3.4 (Linux AppImage)", lic("libffi.txt")),
    ("libdecor (Linux AppImage)", lic("libdecor.txt")),
    ("GCC runtime: libstdc++, libgcc (Linux AppImage, Windows build)",
     "Copyright Free Software Foundation, Inc. Licensed under the GNU General "
     "Public License version 3 with the GCC Runtime Library Exception, version 3.1:\n\n"
     + lic("gcc-runtime-library-exception-3.1.txt")),
    # The Windows build.
    ("MinGW-w64 runtime (Windows build)", lic("mingw-w64.txt")),
    ("winpthreads (Windows build)", lic("winpthreads.txt")),
    # The Mac app.
    ("SDL 3 and sdl2-compat (macOS app)",
     lic("SDL3.txt") + "\n\nsdl2-compat:\n\n" + lic("sdl2-compat.txt")),
    # The licences named above, in full.
    ("Apache License 2.0", read(os.path.join(KOL, "third_party", "roboto", "LICENSE"))),
    ("GNU Lesser General Public License version 3", lic("LGPL-3.0.txt")),
    ("GNU General Public License version 3", lic("GPL-3.0.txt")),
]


def assemble(sections):
    parts = ["Licenses and notices\n" + "=" * 20]
    for title, body in sections:
        parts.append("\n\n" + title + "\n" + "-" * len(title) + "\n\n" + body)
    return "".join(parts) + "\n"


def ascii_only(text):
    # Accents dropped, quotes and dashes plain.
    for a, b in (("‘", "'"), ("’", "'"), ("“", '"'), ("”", '"'),
                 ("–", "-"), ("—", "-"), ("©", "(c)"), (" ", " ")):
        text = text.replace(a, b)
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    return text.replace("\f", "\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote", path, os.path.getsize(path), "bytes")


def main():
    write(OUT_ANDROID, assemble(COMMON))
    write(OUT_DESKTOP, ascii_only(assemble(COMMON + DESKTOP)))


if __name__ == "__main__":
    main()
