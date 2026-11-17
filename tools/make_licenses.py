#!/usr/bin/env python3
"""Assemble the third-party notices the Android app ships with
(android/app/src/main/assets/licenses.txt, shown by Options > Licenses).

Run it again whenever a dependency or its licence changes; the text is taken
from the licence files themselves so the two cannot drift apart.
"""
import os

KOL = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GBA = os.path.abspath(os.path.join(KOL, "..", "gbarecomp"))
OUT = os.path.join(KOL, "android", "app", "src", "main", "assets", "licenses.txt")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def stb_licence():
    # The licence block at the end of stb_image.h (MIT or public domain).
    text = read(os.path.join(KOL, "third_party", "stb", "stb_image.h"))
    start = text.rfind("This software is available under 2 licenses")
    block = text[start:] if start >= 0 else ""
    return block.replace("*/", "").strip()


SECTIONS = [
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


def main():
    parts = ["Licenses and notices\n" + "=" * 20]
    for title, body in SECTIONS:
        parts.append("\n\n" + title + "\n" + "-" * len(title) + "\n\n" + body)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("".join(parts) + "\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
