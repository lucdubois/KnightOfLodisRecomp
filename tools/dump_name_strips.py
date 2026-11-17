#!/usr/bin/env python3
"""Rasterise the ROM's pre-rendered name strips so they can be read.

193 of the 489 pool strings begin with the `0x81 NN` escape, which makes the
whole entry a pre-rendered strip with NO text form anywhere in the ROM. The
second-screen panel needs text, so `kStripText[]` in src/kol_unit_data.cpp
carries those names as a TRANSCRIPTION -- read off the strips by eye.

This is the tool that produced the sheets they were read from. It exists so a
name that looks wrong can be checked against the pixels it came from, which a
hand transcription has to allow for.

    python3 tools/dump_name_strips.py totkol.gba out_dir

Two things it has to get right, both of which produced confident nonsense
first time round:

  * kEscTable's offset is in TILES, not cells -- consecutive entries differ by
    2 * cells. Read as cells it lands on a different name and still looks like
    a plausible item ("Ring of Flotation" for what is actually "Hard Leather").
  * The strips are not all one shape. 105 of the 188 referenced are two 8px
    lines stacked in one 16px cell run; the other 83 are a single 16px line.
    The test is whether rows 7-8 are blank. Split a 16px line in half and it
    reads as garbage beside a name that looks fine.
"""
import struct
import sys
import zlib

ROM_BASE = 0x08000000
ESC_TABLE = 0x0860D088     # (u16 tile offset, u16 cell count) per escape id
ESC_PIXELS = 0x086A2AB0    # 4bpp, 32 bytes per 8x8 tile
NAME_PTRS = 0x08623678
NAME_COUNT = 489
PER_SHEET = 24


def main(rom_path, out_dir):
    rom = open(rom_path, "rb").read()

    def u16(a):
        return int.from_bytes(rom[a - ROM_BASE:a - ROM_BASE + 2], "little")

    def u32(a):
        return int.from_bytes(rom[a - ROM_BASE:a - ROM_BASE + 4], "little")

    def tile(idx):
        base = ESC_PIXELS + idx * 32 - ROM_BASE
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(0, 8, 2):
                b = rom[base + y * 4 + x // 2]
                px[y][x] = b & 0xF
                px[y][x + 1] = b >> 4
        return px

    def strip(bid):
        off, cells = u16(ESC_TABLE + bid * 4), u16(ESC_TABLE + bid * 4 + 2)
        if cells == 0 or cells > 24:
            return None
        w = cells * 8
        img = [[0] * w for _ in range(16)]
        for c in range(cells):
            for half, ti in ((0, off + 2 * c), (1, off + 2 * c + 1)):
                g = tile(ti)
                for y in range(8):
                    for x in range(8):
                        img[half * 8 + y][c * 8 + x] = g[y][x]
        return w, img

    used = sorted({rom[u32(NAME_PTRS + n * 4) - ROM_BASE + 1]
                   for n in range(NAME_COUNT)
                   if rom[u32(NAME_PTRS + n * 4) - ROM_BASE] == 0x81})

    def png(path, w, h, pix, scale=5, rule=0):
        rows = []
        for y in range(h):
            for _ in range(scale):
                row = bytearray([0])
                for x in range(w):
                    v = 255 if pix[y][x] else (55 if rule and y % rule == 0 else 0)
                    row += bytes([v, v, v]) * scale
                rows.append(bytes(row))

        def chunk(tag, data):
            c = struct.pack(">I", len(data)) + tag + data
            return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        head = struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0)
        open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) +
                               chunk(b"IDAT", zlib.compress(b"".join(rows))) +
                               chunk(b"IEND", b""))

    two = one = 0
    for start in range(0, len(used), PER_SHEET):
        ids = used[start:start + PER_SHEET]
        blocks = []
        for bid in ids:
            sw, img = strip(bid)
            # Rows 7-8 blank means two stacked 8px lines; otherwise one 16px
            # line. Lay the two lines side by side so the name reads across.
            stacked = not any(img[7][x] or img[8][x] for x in range(sw))
            if stacked:
                two += 1
                w = sw * 2 + 3
                b = [[0] * w for _ in range(8)]
                for half in range(2):
                    for y in range(8):
                        for x in range(sw):
                            if img[half * 8 + y][x]:
                                b[y][half * (sw + 3) + x] = 1
            else:
                one += 1
                w, b = sw, [[1 if img[y][x] else 0 for x in range(sw)]
                            for y in range(16)]
            blocks.append((w, b))
        rowh = 19
        width = max(w for w, _ in blocks)
        height = len(blocks) * rowh
        pix = [[0] * width for _ in range(height)]
        for i, (w, b) in enumerate(blocks):
            for y in range(len(b)):
                for x in range(w):
                    pix[i * rowh + 2 + y][x] = b[y][x]
        n = start // PER_SHEET
        png(f"{out_dir}/nm_{n}.png", width, height, pix, rule=rowh)
        print(f"nm_{n}.png  escape ids: {ids}")
    print(f"\n{len(used)} strips: {two} two-line, {one} one-line")
    print("Rows are in the id order printed above, top to bottom.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
