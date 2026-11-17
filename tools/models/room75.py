#!/usr/bin/env python3
"""Map 75: a cave whose black roof runs one to three cells past the grid's wall
of height-20 cells, over walkable cells of height 2-9, with a curtain of blue
stalactites hanging from the roof's edge and green vines hanging from them.

    tools/models/room75.py      writes src/kol_model_map75.inc

The art is split for us: BG0 is the whole cave (roof band, walls, floor), BG1
the curtain and vines in front of it, so lifted they open onto the real wall.
room75_art.py holds what the art shows, per column of the frame KX 240, KY 287
(sx = (X - Z) / 2 + KX, sy = (X + Z) / 4 - Y / 2 + KY); a pixel is sampled at
its corner, and the game's raster closes a triangle's every edge, so a quad holds its
pixel from c - EDGE to c + 1 - EDGE, or it takes the next one too. One column's surface stands in a vertical plane: the sum D = X + Z
fixes it, and at (c, r) then X - Z = 2 (c - KX), Y = 2 (D / 4 + KY - r).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, ASDRAWN, BG1, NOGAP, OVER, SHEET, TOP  # noqa: E402

# Both on BG1 over the repaint of the rock on the same layer: a gap they bound is
# that repaint (the fill gave a vine a block of purple rock beside it).
# OVER: lifted, they open onto the wall's cut corner behind them, not the cell's stair.
HANGS = BG1 | SHEET | ASDRAWN | NOGAP | OVER
import room75_art as art  # noqa: E402

KX, KY = 240.0, 287.0
ROOF_Y = 320.0
ROW_STEP = 8        # rows between a column's vertices
WMAX = 9.0          # a stalactite's half width at its widest, in columns
TAPER = 0.17        # columns of half width per row up from the tip
SHEET_BACK = 34.0   # what no stalactite covers: this far behind the edge, along D
VINE_OUT = 6.0      # the vines this far in front of what they pass
MARGIN = (3.0, 14.0)  # each stalactite's axis this far behind the edge, by a hash
NEIGHBOUR = 12.0    # along D, the most a column's surface falls off its neighbours' per column
VINE_SLOPE = 3.0    # along D, the most a vine's depth changes per column
EDGE = 0.1


def point(c, r, D):
    d = 2.0 * (c - KX)
    return ((D + d) / 2.0, 2.0 * (D / 4.0 + KY - r), (D - d) / 2.0)


def roof_point(c, r):
    D = 4.0 * (r - KY + ROOF_Y / 2.0)
    d = 2.0 * (c - KX)
    return ((D + d) / 2.0, ROOF_Y, (D - d) / 2.0)


def edge_D(r):      # the depth of the roof's edge seen at row r
    return 4.0 * (r - KY + ROOF_Y / 2.0)


def top_row(c):     # the map's far edge at the roof's height
    return abs(c - KX) / 2.0 + KY - ROOF_Y / 2.0


def near_row(c):    # its near edge, X 416 or Z 416
    return (832.0 - 2.0 * abs(c - KX)) / 4.0 + KY - ROOF_Y / 2.0


def hash01(a):
    return (math.sin(a * 12.9898) * 43758.5453) % 1.0


# The rocks on the floor: two pieces of art, each repainted at several cells (found
# by matching BG1 against itself: whole tiles apart). The stored height of a cell is
# the rock's top, not the floor: floor = stored - 3 for both, by the stalagmites'
# feet and by BG0's flat floor under the domes (kDrawnCells lowers the cells to it).
# (rows of the frame) -> ((first, last column) of each lobe), as drawn at the first
# cell; a lobe is round about its own middle at each row, so the outline is the art's.
def _rows(table):
    out = {}
    for r0, r1, *lobes in table:
        for r in range(r0, r1 + 1):
            out[r] = lobes
    return out


# Stalagmite, at 9,8: BG1 alone there (tall one, short one in front at its left).
STALAGMITE = _rows([
    (384, 385, (258, 260)), (386, 386, (257, 261)), (387, 388, (256, 262)), (389, 390, (255, 263)),
    (391, 393, (254, 264)), (394, 395, (254, 265)),
    (396, 396, (254, 265), (249, 251)), (397, 398, (254, 265), (248, 252)),
    (399, 403, (254, 265), (247, 253)), (404, 405, (254, 266), (247, 253)),
    (406, 408, (254, 267), (246, 253)), (409, 411, (254, 267), (245, 253)),
    (412, 414, (254, 267), (245, 253)),
])
# Its foot, past the lobes: on the V with the rest of the bottom.
STALAGMITE_FOOT = {413: (242, 269), 414: (240, 271)}
# Twin domes, at 1,5: read off the dark arcs at 12x, the same at 2,8 and 6,2.
DOMES = _rows([
    (284, 284, (168, 172)), (285, 285, (167, 173)), (286, 286, (166, 174)), (287, 288, (165, 175)),
    (289, 290, (164, 176)), (291, 291, (164, 177)),
    (292, 292, (164, 177), (180, 185)), (293, 293, (164, 178), (179, 186)),
    (294, 294, (164, 178), (179, 187)), (295, 295, (164, 178), (179, 188)),
    (296, 297, (163, 178), (179, 188)), (298, 298, (163, 178), (179, 189)),
    (299, 300, (163, 178), (179, 190)), (301, 301, (162, 178), (179, 191)),
    (302, 303, (162, 178), (179, 191)), (304, 305, (162, 178), (179, 192)),
    (306, 307, (161, 178), (179, 192)),
])
# (art, its foot's rows, cell, column and row offset of the art there, floor in steps)
ROCKS = [
    (STALAGMITE, STALAGMITE_FOOT, (9, 8), 0, 0, 2),
    (STALAGMITE, STALAGMITE_FOOT, (3, 3), -16, -96, 3),
    (STALAGMITE, STALAGMITE_FOOT, (3, 6), -64, -64, 2),
    (STALAGMITE, STALAGMITE_FOOT, (4, 11), -128, -24, 3),
    (STALAGMITE, STALAGMITE_FOOT, (5, 1), 48, -120, 6),
    (STALAGMITE, STALAGMITE_FOOT, (10, 3), 96, -48, 4),
    (DOMES, {}, (1, 5), 0, 0, 4),
    (DOMES, {}, (2, 8), -32, 48, 2),
    (DOMES, {}, (6, 2), 128, 32, 2),
]
ROCK_ROW_STEP = 6
V_BLEND = 12.0      # rows over which a rock eases from its V into its round body


def rock_pixels():
    px = set()
    for art_rows, foot, _, dx, dy, _ in ROCKS:
        for r, lobes in art_rows.items():
            for (l, h) in lobes:
                px.update((c + dx, r + dy) for c in range(l, h + 1))
        for r, (l, h) in foot.items():
            px.update((c + dx, r + dy) for c in range(l, h + 1))
    return px


def build_rock(m, art_rows, foot, cell, dx, dy, floor, skip, part, part_v):
    """A rock anchored in a V, as the game's other rocks are: its bottom follows its
    cell's two front edges on the floor, and the cell's top between the art's bottom
    and those edges stands up with it (part_v, not lifted: it is the floor's art on
    the floor's layer). Higher up a lobe is round about its own middle at each row,
    so the outline is the art's, easing into the V over V_BLEND rows."""
    Yf = 16.0 * floor
    last = max(art_rows)
    D_cell = (cell[0] + cell[1] + 1) * 32.0
    cx = KX + (cell[0] - cell[1]) * 16.0
    n_lobes = max(len(v) for v in art_rows.values())
    axis = []
    for i in range(n_lobes):
        l, h = art_rows[last][i]
        # Its front at the cell's middle (the art's foot on the stalagmites): set by
        # the floor instead, the domes stood 32 back and sheared into their V.
        axis.append(D_cell - (h - l + 1.0))

    def lobe_at(c, r):      # (c, r) in the art's own frame
        for i, (l, h) in enumerate(art_rows.get(r, ())):
            if l <= c <= h:
                return i
        return None

    def round_at(i, c, r):
        lobes = art_rows[min(max(r, min(art_rows)), last)]
        if i >= len(lobes):
            lobes = art_rows[last]
        l, h = lobes[i]
        mid = (l + h + 1) / 2.0
        w = (h - l + 1) / 2.0
        s = max(-0.98, min(0.98, (c + 0.5 - mid) / w))
        e = math.sqrt(1.0 - s * s)
        kk = 1.0 / math.sqrt(2.0)
        return axis[i] + 2.0 * w * e, (kk * (e + s), 0.25, kk * (e - s))

    def plane_row(D):       # the row where depth D meets the floor
        return D / 4.0 - Yf / 2.0 + KY

    cols = set()
    for lobes in art_rows.values():
        for (l, h) in lobes:
            cols.update(range(l, h + 1))
    for (l, h) in foot.values():
        cols.update(range(l, h + 1))
    # The V's whole width too: the cell's front edges past the art.
    cols.update(int(cx - dx) + u for u in range(-16, 16))
    for c in sorted(cols):
        cc = c + dx
        u = abs(cc + 0.5 - cx)
        rows = [r for r in sorted(set(art_rows) | set(foot))
                if lobe_at(c, r) is not None or (r in foot and foot[r][0] <= c <= foot[r][1])]
        bottom = rows[-1] + dy if rows else None
        # The V under this column; a rock wider than its cell stands on the floor
        # at its own bottom there.
        v_row = plane_row(D_cell + 32.0 - 2.0 * min(u, 16.0)) if u < 16.0 else None
        if v_row is None and bottom is None:
            continue
        anchor = max(v_row if v_row is not None else -1e9, (bottom + 1) if bottom is not None else -1e9)
        D_v = 4.0 * (anchor - KY) + 2.0 * Yf
        n_v = (1.0, 0.0, 0.0) if cc + 0.5 >= cx else (0.0, 0.0, 1.0)    # the V's two arms
        # The rock's own pixels.
        runs = []
        for r in rows:
            kind = lobe_at(c, r)
            kind = 'foot' if kind is None else kind
            if (cc, r + dy) in skip:
                continue
            if runs and runs[-1][0] == kind and runs[-1][2] == r - 1:
                runs[-1][2] = r
            else:
                runs.append([kind, r, r])
        for kind, r0, r1 in runs:
            idx = []
            r = r0
            while True:
                row = r + dy - EDGE
                if kind == 'foot':
                    D, n = D_v, n_v
                else:
                    D, n = round_at(kind, c, min(r, r1))
                    t = min(1.0, max(0.0, (bottom + 1 - (r + dy)) / V_BLEND))
                    D = D_v + (D - D_v) * t
                idx.append((m.vert(point(cc - EDGE, row, D), n), m.vert(point(cc + 1 - EDGE, row, D), n)))
                if r >= r1 + 1:
                    break
                r = min(r + ROCK_ROW_STEP, r1 + 1)
            for i in range(len(idx) - 1):
                a, b = idx[i]
                d, e = idx[i + 1]
                m.tris.append((a, b, e, part, HANGS))
                m.tris.append((a, e, d, part, HANGS))
        # The cell's top from there down to the V.
        if v_row is None:
            continue
        back = plane_row(D_cell) if bottom is None else bottom + 1
        if v_row - back < 0.5:
            continue
        r0 = math.ceil(back)
        r1 = math.ceil(v_row)
        if r1 <= r0:
            continue
        a = m.vert(point(cc - EDGE, r0 - EDGE, D_v), n_v)
        b = m.vert(point(cc + 1 - EDGE, r0 - EDGE, D_v), n_v)
        d = m.vert(point(cc - EDGE, r1 - EDGE, D_v), n_v)
        e = m.vert(point(cc + 1 - EDGE, r1 - EDGE, D_v), n_v)
        m.tris.append((a, b, e, part_v, SHEET | ASDRAWN | NOGAP))
        m.tris.append((a, e, d, part_v, SHEET | ASDRAWN | NOGAP))


# Where the walls of height-20 cells step toward the camera, a stair's corner hides wall
# the art never drew (a cell's -X or -Z face): with the camera moved, the one row of
# pixels beside the corner stretched across the gap, stripes changing with the camera.
# (Test: drawing 1,8 at height 20 took the patch away.) So the corner is cut on a
# diagonal, as a rock wall would be: over CUT_COLS columns the wall runs from the
# corner to the wall behind, its foot on the floor all the way, no jump left to stretch.
# (corner X, corner Z, 'x' its wall behind is X - 32 to the right on screen, or 'z' Z -
# 32 to the left; floor's Y.)
WALL_CUTS = [(64.0, 288.0, 'x', 64.0), (96.0, 384.0, 'x', 48.0), (352.0, 64.0, 'z', 96.0)]
CUT_COLS = 16


def build_wall_cuts(m, skip):
    part = m.part("wall_cut")
    k = 1.0 / math.sqrt(2.0)
    for (X1, Z1, axis, Yf) in WALL_CUTS:
        cc = int(KX + (X1 - Z1) / 2.0)
        if axis == 'x':
            cols = range(cc, cc + CUT_COLS)
            far = 2.0 * (X1 - 32.0) - 2.0 * (cc + CUT_COLS - KX)
        else:
            cols = range(cc - CUT_COLS, cc)
            far = 2.0 * (Z1 - 32.0) + 2.0 * (cc - CUT_COLS - KX)
        near = X1 + Z1
        n = (k, 0.0, k)
        for c in cols:
            t = abs(c + 0.5 - cc) / CUT_COLS
            D = near + (far - near) * t
            foot = D / 4.0 - Yf / 2.0 + KY
            top = D / 4.0 - ROOF_Y / 2.0 + KY
            if c in art.TOP:
                top = max(top, art.TOP[c])
            r = int(math.ceil(top))
            last = int(math.ceil(foot)) - 1
            while r <= last:
                if (c, r) in skip:
                    r += 1
                    continue
                r1 = r
                while r1 < last and (c, r1 + 1) not in skip:
                    r1 += 1
                a = m.vert(point(c - EDGE, r - EDGE, D), n)
                b = m.vert(point(c + 1 - EDGE, r - EDGE, D), n)
                d = m.vert(point(c - EDGE, r1 + 1 - EDGE, D), n)
                e = m.vert(point(c + 1 - EDGE, r1 + 1 - EDGE, D), n)
                m.tris.append((a, b, e, part, SHEET | ASDRAWN | NOGAP))
                m.tris.append((a, e, d, part, SHEET | ASDRAWN | NOGAP))
                r = r1 + 1


class Field:
    """The curtain's depth at each pixel: the nearest stalactite, else the sheet."""

    def __init__(self):
        self.cones = []
        for (a, t) in art.AXES:
            c = int(a)
            top = art.TOP.get(c)
            if top is None:
                continue
            length = t - top
            m = MARGIN[0] + (MARGIN[1] - MARGIN[0]) * hash01(a * 7.0 + t) * (1.0 - min(1.0, length / 90.0) * 0.8)
            self.cones.append((a, t, edge_D(top) - 2.0 * WMAX - m))
        self.near = {}
        for i, (a, t, _) in enumerate(self.cones):
            for c in range(int(a - WMAX) - 1, int(a + WMAX) + 3):
                self.near.setdefault(c, []).append(i)

    def at(self, c, r, seg_top):
        """(D, normal) in column c at row r: its own stalactite, or its neighbours'
        less NEIGHBOUR a column (a column of the sheet between two stalactites
        fell 30-50 at once: wide bands with the camera moved). Never in front of
        this column's own edge: its pixels would stand over the roof."""
        edge = edge_D(seg_top)
        D, n = self.raw(c, r, seg_top)
        for i in range(1, 5):
            for o in (c - i, c + i):
                for seg in art.STAL.get(o, ()):
                    if seg[0] <= r <= seg[1] + 1:
                        D = max(D, self.raw(o, r, seg[0])[0] - NEIGHBOUR * i)
        return min(D, edge - 2.0), n

    def raw(self, c, r, seg_top):
        edge = edge_D(seg_top)
        best = edge - SHEET_BACK
        n = (0.70, -0.12, 0.70)
        for i in self.near.get(c, ()):
            a, t, Da = self.cones[i]
            if r > t + 1.0:
                continue
            w = min(WMAX, 1.0 + TAPER * max(0.0, t - r))
            u = c + 0.5 - a
            if abs(u) >= w:
                continue
            s = u / w
            e = math.sqrt(1.0 - s * s)
            D = Da + 2.0 * w * e
            D = min(D, edge - 2.0)
            if D > best:
                best = D
                k = 1.0 / math.sqrt(2.0)
                n = (k * (e + s), -0.12, k * (e - s))
        return best, n


def build_parts(m):
    # The roof: one plane, a quad a column to the edge the art draws.
    for c in range(32, 448):
        l, r = c - EDGE, c + 1 - EDGE
        bottom = art.TOP.get(c)
        b0 = bottom - EDGE if bottom is not None else near_row(l)
        b1 = bottom - EDGE if bottom is not None else near_row(r)
        m.poly("roof", [roof_point(l, top_row(l)), roof_point(r, top_row(r)),
                        roof_point(r, b1), roof_point(l, b0)], TOP | SHEET)
    for c, runs in art.TABS.items():
        for (r0, r1) in runs:
            l, h = c - EDGE, c + 1 - EDGE
            m.poly("roof", [roof_point(l, r0 - EDGE), roof_point(h, r0 - EDGE),
                            roof_point(h, r1 + 1 - EDGE), roof_point(l, r1 + 1 - EDGE)], TOP | SHEET)

    f = Field()
    # The stalactites: a strip a column, lifted off the wall behind; a vine's
    # pixels are left out, so they can only be the vine's.
    vine_rows = {}
    for runs in art.VINES:
        for (c, r0, r1) in runs:
            vine_rows.setdefault(c, set()).update(range(r0, r1 + 1))
    rocks = rock_pixels()
    part = m.part("stalactites")
    for c, segs in art.STAL.items():
        for (seg0, seg1) in segs:
            r = seg0
            while r <= seg1:
                if r in vine_rows.get(c, ()) or (c, r) in rocks:
                    r += 1
                    continue
                r1 = r
                while r1 < seg1 and r1 + 1 not in vine_rows.get(c, ()) and (c, r1 + 1) not in rocks:
                    r1 += 1
                nodes = []
                y = float(r)
                while True:
                    D, n = f.at(c, y, seg0)
                    nodes.append((y, D, n))
                    if y >= r1 + 1:
                        break
                    y = min(y + ROW_STEP, r1 + 1.0)
                idx = []
                for j, (y, D, n) in enumerate(nodes):
                    row = y - EDGE
                    idx.append((m.vert(point(c - EDGE, row, D), n), m.vert(point(c + 1 - EDGE, row, D), n)))
                for i in range(len(idx) - 1):
                    a, b = idx[i]
                    d, e = idx[i + 1]
                    m.tris.append((a, b, e, part, HANGS))
                    m.tris.append((a, e, d, part, HANGS))
                r = r1 + 1

    # The vines: every one rests on the stalactites at the column it passes, and
    # the depth is one curve along the whole curtain, so a swag and the stubs
    # beside it stay joined; where they swing clear of the stalactites, the line
    # between the columns where they touch.
    want = {}
    runs_all = [run for runs in art.VINES for run in runs]
    for (c, r0, r1) in runs_all:
        for r in range(r0, r1 + 1):
            for seg in art.STAL.get(c, ()):
                if seg[0] <= r <= seg[1]:
                    D, _ = f.at(c, float(r), seg[0])
                    want[c] = max(want.get(c, D), D)
    known = sorted(want)
    cols = sorted({run[0] for run in runs_all})
    curve = {}
    for c in cols:
        if c in want:
            curve[c] = want[c]
            continue
        lo = [k for k in known if k < c]
        hi = [k for k in known if k > c]
        if lo and hi:
            t = (c - lo[-1]) / float(hi[0] - lo[-1])
            curve[c] = want[lo[-1]] + (want[hi[0]] - want[lo[-1]]) * t
        else:
            curve[c] = want[lo[-1]] if lo else want[hi[0]]
    # Held in front of every column it passes, by a slope (a plain average jumped
    # 10-30 from column to column: a stretched blob with the camera moved).
    smooth = {}
    for c in cols:
        smooth[c] = max(curve[k] - VINE_SLOPE * abs(k - c) for k in cols) + VINE_OUT
    k = 1.0 / math.sqrt(2.0)
    part = m.part("vines")
    for (c, r0, r1) in runs_all:
        D = smooth[c]
        n = (k, 0.0, k)
        a = m.vert(point(c - EDGE, r0 - EDGE, D), n)
        b = m.vert(point(c + 1 - EDGE, r0 - EDGE, D), n)
        d = m.vert(point(c - EDGE, r1 + 1 - EDGE, D), n)
        e = m.vert(point(c + 1 - EDGE, r1 + 1 - EDGE, D), n)
        m.tris.append((a, b, e, part, HANGS))
        m.tris.append((a, e, d, part, HANGS))

    # The rocks on the floor, under whatever vine passes in front.
    vine_px = {(c, r) for (c, r0, r1) in runs_all for r in range(r0, r1 + 1)}
    part = m.part("rocks")
    part_v = m.part("rock_v")
    for art_rows, foot, cell, dx, dy, floor in ROCKS:
        build_rock(m, art_rows, foot, cell, dx, dy, floor, vine_px, part, part_v)

    # The stair corners of the walls, under all of that.
    build_wall_cuts(m, rocks | vine_px)


if __name__ == "__main__":
    m = Model("map75")
    build_parts(m)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map75.inc")
    m.write_inc(out, 75, "tools/models/room75.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
