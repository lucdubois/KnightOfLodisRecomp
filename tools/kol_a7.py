#!/usr/bin/env python3
"""kol_a7.py — list and decode Knight of Lodis "A7" asset archives.

The same decoder as src/kol_a7.cpp (transcribed from ROM 0x08004990), for
exploring assets offline:

    tools/kol_a7.py totkol.gba list                 # every A7 archive in the ROM
    tools/kol_a7.py totkol.gba sheet 0x086B75B4 out.png   # its tiles, grayscale

See README "A7 archives" for the format.
"""
import struct
import sys
import zlib

def decode(rom, stream, dic, base=0x08000000):
    s = stream - base; d = dic - base
    mode = rom[d]; k = rom[d + 1]; dp = d + 2
    size = None
    if mode <= 2:
        size = (rom[dp] << 8) | rom[dp + 1]; dp += 2
    if mode in (0, 3): mask = 0xFFFF >> (k + 1)
    elif mode in (1, 2, 4, 5): mask = 0xFFFF >> k
    else: raise ValueError("mode %d" % mode)
    if mode >= 3:
        size = (rom[s] << 8) | rom[s + 1]; s += 2
    out = bytearray()
    if mode in (0, 3):
        while len(out) < size:
            b = rom[s]
            if b & 0x80:
                n = (b & 0x7F) + 1; s += 1
                out += rom[s:s + n]; s += n
            else:
                ln = (((b << 1) >> (8 - k)) & 0xFFFF) + 3
                off = ((b << 8) | rom[s + 1]) & mask; s += 2
                for i in range(ln): out.append(rom[dp + off + i])
    elif mode in (1, 4):
        flags = 0
        while len(out) < size:
            flags = (flags << 1) & 0xFFFF
            if flags & 0xFF == 0:
                flags = (rom[s] << 8) | 0xFF; s += 1
            if flags & 0x8000:
                out.append(rom[s]); s += 1
            else:
                b = rom[s]
                ln = (b >> (8 - k)) + 3
                off = ((b << 8) | rom[s + 1]) & mask; s += 2
                for i in range(ln): out.append(rom[dp + off + i])
    else:
        raise NotImplementedError("modes 2/5 not needed yet")
    if len(out) != size: raise ValueError("overrun")
    return bytes(out)


def archives(rom):
    """Every plausible A7 archive: magic, a sane count, a dictionary header
    with a known mode."""
    out, i = [], -1
    while True:
        i = rom.find(b'A7', i + 1)
        if i < 0 or i + 8 > len(rom):
            return out
        n = struct.unpack_from('>H', rom, i + 2)[0]
        d = struct.unpack_from('>I', rom, i + 4)[0]
        if 0 < n < 2000 and 8 + 4 * n <= d < 0x100000 and i + d + 4 < len(rom) \
                and rom[i + d] <= 5 and rom[i + d + 1] <= 8:
            out.append((0x08000000 + i, n, rom[i + d]))


def entries(rom, archive):
    b = archive - 0x08000000
    n = struct.unpack_from('>H', rom, b + 2)[0]
    d = struct.unpack_from('>I', rom, b + 4)[0] + archive
    return [decode(rom, struct.unpack_from('>I', rom, b + 8 + 4 * i)[0] + archive, d)
            for i in range(n)]


def sheet(rom, archive, path, per_row=32, k=3):
    tiles = b''.join(entries(rom, archive))
    count = len(tiles) // 32
    rows = (count + per_row - 1) // per_row
    w, h = per_row * 8, rows * 8
    img = [[255] * w for _ in range(h)]
    for t in range(count):
        for y in range(8):
            for x in range(8):
                v = (tiles[t * 32 + y * 4 + x // 2] >> (4 * (x & 1))) & 15
                img[(t // per_row) * 8 + y][(t % per_row) * 8 + x] = 255 - v * 16
    raw = b''.join(bytes([0]) + b''.join(bytes((p, p, p)) * k for p in r)
                   for r in img for _ in range(k))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + \
        struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', w * k, h * k, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    rom = open(sys.argv[1], 'rb').read()
    if sys.argv[2] == 'list':
        for a, n, mode in archives(rom):
            print('%08X  entries=%4d  mode=%d' % (a, n, mode))
    elif sys.argv[2] == 'sheet':
        sheet(rom, int(sys.argv[3], 16), sys.argv[4])
