#!/usr/bin/env python3
"""Reads map 84's crystals into room84_art.py (room84.py makes the model of them).

    tools/models/room84_extract.py DUMP [OUT]

DUMP is a KOL_MEM_DUMP of a frame on map 84 (EWRAM, IWRAM, palette, VRAM, OAM, raw).
Nothing is read off a picture: the game keeps each layer's whole composed tilemap in
EWRAM (BG0 at 0x02004440, BG1 at 0x0200A440, 128 x 96 tiles), so the art comes out
whole, in the layers' own pixels (bx, by), whatever the camera:

    bx = (X - Z) / 2 + 272,  by = (X + Z) / 4 - Y / 2 + 424

BG1 is the crystals (two big clusters, ten small ones) and, over an empty BG0, a slab
of the cliff wall left of the hidden stairs (in front of whoever walks up them). The
slab's tiles are the wall's own (BG0 uses them too); where a spike of the left cluster
runs up into the slab the tile is neither: there the wall behind the spike is the wall
tile that matches most of the tile's pixels, and the pixels that differ are the spike.
"""
import os
import struct
import sys

PAL, VRAM = 0x48000, 0x48400
BG0_MAP, BG1_MAP, GRID = 0x4440, 0xA440, 0x16440
COLS, ROWS = 128, 96
W, H = COLS * 8, ROWS * 8
BX0, BY0 = 272, 424
OUTLINE = 49        # the crystals' dark outline (palette 3, colour 1)
SLAB = (16, 23, 51)  # the wall slab's tile columns, and its last row
WALL_MATCH = 20     # of 64 pixels: a mixed tile's wall behind (chance gives 9-18)
# A crystal's cell stores its top: the floor is 2 under a record-23 cell (byte 4),
# 1 under a record-24 cell (byte 3). The art's bottom middle lands on that cell's
# middle within 1.5 pixels on seven of the ten; three hang 7 rows over a step.
DROP = {4: 2, 3: 1}


def main(dump, out):
    m = open(dump, 'rb').read()

    def tile(e):
        o = VRAM + (e & 0x3ff) * 32
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(8):
                b = m[o + y * 4 + x // 2]
                v = (b >> 4) if x & 1 else (b & 15)
                px[7 - y if e & 0x800 else y][7 - x if e & 0x400 else x] = ((e >> 12) * 16 + v) if v else 0
        return px

    def layer(base):
        ents = [[struct.unpack_from('<H', m, base + r * 256 + c * 2)[0] for c in range(COLS)] for r in range(ROWS)]
        idx = [[0] * W for _ in range(H)]
        cache = {}
        for r in range(ROWS):
            for c in range(COLS):
                e = ents[r][c]
                if e:
                    t = cache.setdefault(e, tile(e))
                    for y in range(8):
                        idx[r * 8 + y][c * 8:c * 8 + 8] = t[y]
        return idx, ents

    _, e0 = layer(BG0_MAP)
    i1, e1 = layer(BG1_MAP)
    wall_tiles = {e & 0x3ff for row in e0 for e in row if e}
    T = {t: tile(0x3000 | t) for t in wall_tiles}
    kind = [[0] * W for _ in range(H)]     # 1 crystal, 2 the wall on BG1
    cand, mixed = set(), set()
    for r in range(ROWS):
        for c in range(COLS):
            e = e1[r][c]
            if not e:
                continue
            k = 2 if (e & 0x3ff) in wall_tiles else 1
            if k == 1 and e0[r][c] == 0 and SLAB[0] <= c <= SLAB[1] and r <= SLAB[2]:
                n, b = max((sum(1 for y in range(8) for x in range(8) if i1[r * 8 + y][c * 8 + x] == T[q][y][x]), q)
                           for q in T)
                if n >= WALL_MATCH:
                    mixed.add((r, c))
                    cand.update((c * 8 + x, r * 8 + y) for y in range(8) for x in range(8)
                                if i1[r * 8 + y][c * 8 + x] == T[b][y][x])
            for y in range(8):
                for x in range(8):
                    if i1[r * 8 + y][c * 8 + x]:
                        kind[r * 8 + y][c * 8 + x] = k
    # The wall, from the slab's own tiles on through the matching pixels.
    stack = [p for p in cand if kind[p[1] - 1][p[0]] == 2 or kind[p[1]][p[0] + 1] == 2 or kind[p[1]][p[0] - 1] == 2]
    while stack:
        x, y = stack.pop()
        if (x, y) not in cand or kind[y][x] != 1:
            continue
        kind[y][x] = 2
        stack.extend((x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1))
    # Speckle (a spike's pixel that happens to match, a wall's that does not): the
    # eight neighbours' majority, twice; never the outline.
    for _ in range(2):
        change = []
        for (r, c) in mixed:
            for y in range(r * 8, r * 8 + 8):
                for x in range(c * 8, c * 8 + 8):
                    if i1[y][x] == OUTLINE or not kind[y][x]:
                        continue
                    nb = [kind[y + dy][x + dx] for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                          if (dx or dy) and kind[y + dy][x + dx]]
                    w = sum(1 for v in nb if v == 2)
                    if kind[y][x] == 1 and w >= 6:
                        change.append((x, y, 2))
                    if kind[y][x] == 2 and len(nb) - w >= 6:
                        change.append((x, y, 1))
        for x, y, v in change:
            kind[y][x] = v

    # The grid (type, height) and the crystals' cells.
    grid = {}
    for z in range(32):
        for x in range(32):
            t, h = m[GRID + (z * 32 + x) * 4], m[GRID + (z * 32 + x) * 4 + 1]
            if t != 0xff:
                grid[(x, z)] = (t, h)
    small_cells = {k: v for k, v in grid.items() if v[0] in DROP}

    # Shapes, 8-connected.
    lab = [[0] * W for _ in range(H)]
    comps = []
    for y in range(H):
        for x in range(W):
            if kind[y][x] == 1 and not lab[y][x]:
                lab[y][x] = 1
                st, px = [(x, y)], []
                while st:
                    a, b = st.pop()
                    px.append((a, b))
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            u, v = a + dx, b + dy
                            if 0 <= u < W and 0 <= v < H and kind[v][u] == 1 and not lab[v][u]:
                                lab[v][u] = 1
                                st.append((u, v))
                comps.append(px)

    def runs(px):
        cols = {}
        for (x, y) in px:
            cols.setdefault(x, []).append(y)
        out_ = {}
        for c, ys in sorted(cols.items()):
            ys.sort()
            rr = [[ys[0], ys[0]]]
            for y in ys[1:]:
                if y == rr[-1][1] + 1:
                    rr[-1][1] = y
                else:
                    rr.append([y, y])
            out_[c] = [tuple(r) for r in rr]
        return out_

    big, small, left = [], [], []
    for px in comps:
        xs = [p[0] for p in px]
        ys = [p[1] for p in px]
        if len(px) > 5000:
            big.append(px)
            continue
        cx, bot = (min(xs) + max(xs)) / 2.0, max(ys)
        hit = None
        for (x, z), (t, h) in small_cells.items():
            fl = h - DROP[t]
            sx = (x - z) * 16 + BX0
            sy = (x + z + 1) * 8 - fl * 8 + BY0
            if abs(sx - cx) <= 2 and -2 <= bot - sy <= 8:
                hit = ((x, z), fl)
        if hit and len(px) > 100:
            small.append((hit[0], hit[1], px))
        else:
            left.append((len(px), min(xs), min(ys), max(xs), max(ys)))
    big.sort(key=lambda px: min(p[0] for p in px))      # the left one (z 9-16) first
    small.sort()
    assert len(big) == 2 and len(small) == 10, (len(big), len(small))
    with open(out, 'w') as o:
        o.write("# Generated by tools/models/room84_extract.py from a memory dump of map 84: the layers' own\n"
                "# pixels, bx = (X - Z) / 2 + %d, by = (X + Z) / 4 - Y / 2 + %d. column: [(first row, last row)].\n"
                % (BX0, BY0))
        o.write("BX0, BY0 = %d, %d\n" % (BX0, BY0))
        o.write("# (x, z): (byte, stored height)\nGRID = {\n")
        for k in sorted(grid):
            o.write(" %r: %r,\n" % (k, grid[k]))
        o.write("}\n")
        for name, px in zip(("LEFT", "RIGHT"), big):
            o.write("# %d pixels\n%s = {\n" % (len(px), name))
            for c, rr in runs(px).items():
                o.write(" %d: %r,\n" % (c, rr))
            o.write("}\n")
        o.write("# (cell, floor in steps, runs)\nSMALL = [\n")
        for cell, fl, px in small:
            o.write(" (%r, %d, {\n" % (cell, fl))
            for c, rr in runs(px).items():
                o.write("  %d: %r,\n" % (c, rr))
            o.write(" }),\n")
        o.write("]\n")
        o.write("# BG1 shapes that are neither (pixels, bx0, by0, bx1, by1): left to the grid.\nOTHER = %r\n" % (left,))
    print("left %d px, right %d px, %d small, %d other -> %s" % (len(big[0]), len(big[1]), len(small), len(left), out))


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "room84_art.py"))
