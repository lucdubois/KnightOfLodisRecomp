#!/usr/bin/env python3
"""unit_list.py — offline still of the second screen's battle unit list.

Built from a paused game's memory (EWRAM, OBJ VRAM and OBJ palette dumps) and
the ROM, the way the device version will be:

  - the units: the 26 map objects (u32 at 0x0202A238, stride 0xD0), record at
    +0x00, sorted by record +0x51 (the game's NO.), players left, others right;
  - DONE: object +0x78 bit 1, for the side whose turn it is;
  - the sprite: the unit's current frame as the game's own list draws it, from
    its actor (object +0x08): frame record at part +0xBC (u16 count, then
    attr0/attr1/attr2 per piece), tiles at part +0x28, flip in part +0xC4,
    palette bank at object +0x65;
  - DONE units show the END plate the game paints into their sprite tiles;
  - the cell art from the ROM: the box (sheet tiles 33-36, bank 4 player /
    7 enemy), the HP bar (archive 0x086BCA6C tiles 25-30, bank 12), LEADER
    (sheet tiles 242-245), the parchment field (tiles 5-8).

    tools/mockup/unit_list.py totkol.gba ew.bin obj.bin objpal.bin out.png [scale]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import kol_a7  # noqa: E402
from png import write_png  # noqa: E402

ROM = 0x08000000
BG_PAL = 0x0861DF68
SHEET, BARS = 0x086B75B4, 0x086BCA6C

u16 = lambda b, o: b[o] | b[o + 1] << 8
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]


def main():
    rom = open(sys.argv[1], 'rb').read()
    ew = open(sys.argv[2], 'rb').read()
    obj = open(sys.argv[3], 'rb').read()
    opal = open(sys.argv[4], 'rb').read()
    out = sys.argv[5]
    scale = int(sys.argv[6]) if len(sys.argv) > 6 else 3
    sheet = b''.join(kol_a7.entries(rom, SHEET))
    bars = b''.join(kol_a7.entries(rom, BARS))
    E = lambda a: a - 0x02000000

    def bgcol(bank, i):
        v = u16(rom, BG_PAL - ROM + (bank * 16 + i) * 2)
        return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)

    def objcol(bank, i):
        v = u16(opal, (bank * 16 + i) * 2)
        return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)

    # ── the units ──
    lst = u32(ew, E(0x0202A238))
    acting = u32(ew, E(0x0202A5F0))
    units = []
    for i in range(26):
        o = E(lst) + i * 0xD0
        rec = u32(ew, o)
        if not (0x02000000 <= rec < 0x02040000):
            continue
        r = E(rec)
        hp, hpm = u16(ew, r + 0x40), u16(ew, r + 0x42)
        if hpm == 0 or hp == 0:
            continue
        units.append(dict(i=i, o=o, rec=r, no=ew[r + 0x51], other=bool(ew[r + 0x5D] & 0x80),
                          hp=hp, hpm=hpm, flags=u16(ew, o + 0x78), pal=ew[o + 0x65],
                          name=ew[r + 0x30:r + 0x40]))
    # whose turn: the acting unit's side; none acting = the player browsing
    turn_other = False
    if acting:
        ar = u32(ew, E(acting))
        turn_other = bool(ew[E(ar) + 0x5D] & 0x80)
    for u in units:
        u['done'] = bool(u['flags'] & 2) and u['other'] == turn_other
    teams = [sorted([u for u in units if not u['other']], key=lambda u: (u['done'], u['no'])),
             sorted([u for u in units if u['other']], key=lambda u: (u['done'], u['no']))]

    # ── layout ──
    W = 240
    CELL_W, CELL_H, TOP = 48, 48, 8
    rows = max((len(t) + 1) // 2 for t in teams)
    H = TOP + rows * CELL_H
    px = [bgcol(4, 5)] * (W * H)

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            px[y * W + x] = c

    def tile(data, t, x0, y0, colf, hf=False, vf=False):
        for y in range(8):
            for x in range(8):
                sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                p = (data[t * 32 + sy * 4 + sx // 2] >> (4 * (sx & 1))) & 15
                if p:
                    put(x0 + x, y0 + y, colf(p))

    # parchment: the field tiles 5-8, shifting by two every row, as the sheet
    for ty in range(H // 8 + 1):
        for tx in range(W // 8):
            tile(sheet, 5 + (tx + 2 * ty) % 4, tx * 8, ty * 8, lambda p: bgcol(4, p))

    def sprite(u, ox, oy):
        actor = u32(ew, u['o'] + 0x08)
        part = E(actor) + 0x14
        frame = u32(ew, part + 0xBC)
        tbase = (u32(ew, part + 0x28) - 0x06010000) // 32
        flip = bool(u16(ew, part + 0xC4) & 0x1000)
        f = frame - ROM
        n = u16(rom, f)
        dims = {(0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (1, 0): (16, 8),
                (1, 1): (32, 8), (1, 2): (32, 16), (2, 0): (8, 16), (2, 1): (8, 32),
                (2, 2): (16, 32)}
        for k in range(n):
            a0, a1, a2 = (u16(rom, f + 2 + k * 6 + j * 2) for j in range(3))
            w, h = dims[(a0 >> 14, a1 >> 14)]
            dx = a1 & 0x1FF
            dx = dx - 512 if dx >= 256 else dx
            dy = a0 & 0xFF
            dy = dy - 256 if dy >= 128 else dy
            hf = bool(a1 & 0x1000) ^ flip
            if flip:
                dx = -dx - w
            t0 = tbase + (a2 & 0x3FF)
            for y in range(h):
                for x in range(w):
                    sx = w - 1 - x if hf else x
                    t = t0 + (y // 8) * (w // 8) + sx // 8
                    b = obj[t * 32 + (y % 8) * 4 + (sx % 8) // 2]
                    p = (b >> (4 * (sx & 1))) & 15
                    if p:
                        put(ox + dx + x, oy + dy + y, objcol(u['pal'], p))

    for side, team in enumerate(teams):
        for k, u in enumerate(team):
            cx = side * 120 + 12 + (k % 2) * CELL_W
            cy = TOP + (k // 2) * CELL_H
            bx, by = cx + 8, cy          # the box; the bar is the column left of it
            bank = 7 if u['other'] else 4
            for ry in range(4):
                for rx in range(4):
                    t = 33 + (1 if rx in (1, 2) else 0) + (2 if ry in (1, 2) else 0)
                    tile(sheet, t, bx + rx * 8, by + ry * 8, lambda p, b=bank: bgcol(b, p),
                         hf=rx == 3, vf=ry == 3)
            # HP bar: four segments filled from the bottom, 0/3/5/7/8 px each
            fill = (u['hp'] * 32 + u['hpm'] - 1) // u['hpm']
            for s in range(4):
                f = max(0, min(8, fill - (3 - s) * 8))
                t = {0: 30, 1: 26, 2: 26, 3: 26, 4: 27, 5: 27, 6: 28, 7: 28, 8: 29}[f]
                tile(bars, t, cx, by + s * 8, lambda p: bgcol(12, p))
            tile(bars, 25, cx, by, lambda p: bgcol(12, p))
            tile(bars, 25, cx, by + 24, lambda p: bgcol(12, p), vf=True)
            sprite(u, bx + 16, by + 32)
            if not u['done'] and not u['other'] and u['no'] == 1:
                for j in range(4):
                    tile(sheet, 242 + j, bx + j * 8, by + 33, lambda p: bgcol(12, p))

    rgb = bytearray()
    for y in range(H * scale):
        row = px[(y // scale) * W:(y // scale) * W + W]
        for c in row:
            rgb += bytes(c) * scale
    write_png(out, W * scale, H * scale, rgb)
    for side, team in enumerate(teams):
        print(['player', 'other'][side], [(u['no'], 'DONE' if u['done'] else '') for u in team])


if __name__ == '__main__':
    main()
