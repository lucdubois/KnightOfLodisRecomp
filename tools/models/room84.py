#!/usr/bin/env python3
"""Map 84: an ice cliff at night. The grid already has the cliff: its wall, the
ledges and steps on its face, the stairs hidden in it (x 0-3, z 5-8), each measured
against BG0's art. What it misplaces is what the art draws over them:

  - two big clusters of ice crystals and ten small ones, all on BG1 (lifted here,
    so they open onto the cliff and floor BG0 draws behind them);
  - the wall across the stairs' mouth while the scene has not opened it (BG0 draws
    cliff there, the grid a corridor): part `plug`, its triangles last, left out
    of the model once the art shows the stairs (kol_focus_models.cpp, kModel84Open).

    tools/models/room84.py      writes src/kol_model_map84.inc

room84_art.py (room84_extract.py) holds the crystals per column of the layers' own
pixels: bx = (X - Z) / 2 + BX0, by = (X + Z) / 4 - Y / 2 + BY0. A pixel is sampled
at its corner and the game's raster closes a triangle's every edge, so a column's
quad runs c - EDGE to c + 1 - EDGE. A column's surface stands in a vertical plane:
D = X + Z fixes it, and at (c, r) then X - Z = 2 (c - BX0), Y = 2 (D / 4 + BY0 - r):
at rest no pixel moves.

A big cluster's depth: one curtain, upright, continuous from column to column. Where a
column's lowest pixels meet the floor it stands on the floor (the ray under the pixel,
on the grid); it is never behind the cells it covers (MARGIN in front of them), never
steps back going up, and a column is at most NEIGHBOUR off the next (2 a column is a
cell's edge on the floor), so a spike leaning out over the floor stays with the cluster.
Two things tried and dropped, both seen on the Thor:
  - leaning back 0.6 a row toward the cliff: the upper spikes passed behind the near
    corners of the ledges beside them, and the ledges showed through (holes);
  - depth in flat steps of 24 (so a lifted card would sort at one depth): with the camera
    leaned sideways every step opened into a gap, the clusters in vertical slabs.
What the holes were in the end was the scene's rows, not the curtain: a row ran from a
spike's pixel left in the scene on through the ledge under the lifted spikes beside it as
one run, keyed at the spike's depth and drawn over their cards (kModelRunSplitMaps and
kModelRunFineMaps, kol_focus_tables.h). The tops before the cliff's BG1 slab lift with
the wall painted in behind them (kModelWallBehindMaps).
A small crystal is anchored in a V, as the game's rocks are: it stands on its cell's
two front edges, and the cell's top between its art's bottom and those edges stands
up with it (part `small_v`, the floor's art on the floor's layer).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, ASDRAWN, BG1, BG1ART, BG0ART, NOGAP, OVER, SHEET  # noqa: E402
import room84_art as art  # noqa: E402

BX0, BY0 = float(art.BX0), float(art.BY0)
EDGE = 0.1
ROW_STEP = 8        # rows between a column's vertices
NEIGHBOUR = 2.0     # along D, the most a column stands behind (or before) the next
MARGIN = 6.0        # in front of the grid's cells behind
# Lifted, the art's own light kept (the sun put a dark side on every column), only
# BG1's pixels (the cliff between the spikes is BG0's or the slab's), and a gap they
# bound is not theirs. OVER: lifted before the wall across the stairs (plug), a spike
# opens onto that wall, not onto the corridor the grid has behind it.
HANGS = BG1 | SHEET | ASDRAWN | NOGAP | BG1ART | OVER
WALL_X, WALL_TOP = 128.0, 496.0     # the cliff's face and top (the grid's x 0-3, 31 steps)
PLUG_Z = (160.0, 288.0)             # the stairs' mouth, z 5-8
PLUG_FOOT = 96.0                    # the floor before it (x 4-5, 6 steps)

SMALL_FLOOR = {cell: fl for cell, fl, _ in art.SMALL}


def point(c, r, D):
    d = 2.0 * (c - BX0)
    return ((D + d) / 2.0, 2.0 * (D / 4.0 + BY0 - r), (D - d) / 2.0)


def height(x, z):
    """A cell's drawn top in world units, None off the field. The small crystals'
    cells at their floors (kDrawnCells)."""
    g = art.GRID.get((x, z))
    if g is None:
        return None
    if x <= 3 and 5 <= z <= 8:      # the stairs: the cliff's face runs across them (a spike
        return WALL_TOP             # leaning over their mouth stays before it, open or shut)
    return 16.0 * SMALL_FLOOR.get((x, z), g[1])


def terrain_D(c, r):
    """Where the ray of pixel corner (c, r) meets the grid: the largest D whose
    point is at or under its cell's top. None: past the field."""
    d = 2.0 * (c - BX0)
    k = 2.0 * (BY0 - r)
    D = 1400.0
    while D > -200.0:
        X, Z = (D + d) / 2.0, (D - d) / 2.0
        h = height(int(X // 32), int(Z // 32)) if X >= 0 and Z >= 0 else None
        if h is not None and D / 2.0 + k <= h + 1e-6:
            return D
        D -= 1.0
    return None


def cluster_depth(runs):
    """D(c, r) over a cluster's columns, each from its top row to its bottom."""
    cols = sorted(runs)
    top = {c: runs[c][0][0] for c in cols}
    bot = {c: runs[c][-1][1] for c in cols}
    r_lo, r_hi = min(top.values()), max(bot.values())
    n = r_hi - r_lo + 2
    NEG = -1e9
    # The grid behind every pixel of a column's span (between its spikes too), and
    # under its foot.
    T = {}
    for c in cols:
        col = [NEG] * n
        for r in range(top[c], bot[c] + 2):
            t = terrain_D(c, r)
            if t is not None:
                col[r - r_lo] = t + (MARGIN if r <= bot[c] else 0.0)
        T[c] = col
    F = T
    for _ in range(3):
        for c in cols:      # up each column: upright, never stepping back
            col = F[c]
            for i in range(n - 2, -1, -1):
                col[i] = max(col[i], col[i + 1])
        for seq in (cols, cols[::-1]):      # and from column to column
            for a, b in zip(seq, seq[1:]):
                if abs(a - b) != 1:
                    continue
                fa, fb = F[a], F[b]
                for i in range(n):
                    if fa[i] - NEIGHBOUR > fb[i]:
                        fb[i] = fa[i] - NEIGHBOUR
    # (Rows between a column's spikes hold a depth too, so the pass above runs through.)
    return lambda c, r: F[c][min(max(int(r) - r_lo, 0), n - 1)]


def column_strip(m, part, c, r0, r1, depth, n, flags):
    """A column's run of pixels, r0 to r1, as quads; depth(r) at each vertex."""
    idx = []
    r = r0
    while True:
        D = depth(min(r, r1))
        idx.append((m.vert(point(c - EDGE, r - EDGE, D), n), m.vert(point(c + 1 - EDGE, r - EDGE, D), n)))
        if r >= r1 + 1:
            break
        r = min(r + ROW_STEP, r1 + 1)
    for i in range(len(idx) - 1):
        a, b = idx[i]
        d, e = idx[i + 1]
        m.tris.append((a, b, e, part, flags))
        m.tris.append((a, e, d, part, flags))


def build_cluster(m, name, runs):
    part = m.part(name)
    depth = cluster_depth(runs)
    k = 1.0 / math.sqrt(2.0 + 0.04)
    n = (k, 0.2 * k, k)
    for c in sorted(runs):
        for (r0, r1) in runs[c]:
            column_strip(m, part, c, r0, r1, lambda r, c=c: depth(c, r), n, HANGS)


def build_small(m, cell, floor, runs):
    part, part_v = m.part("small"), m.part("small_v")
    Yf = 16.0 * floor
    D_cell = (cell[0] + cell[1] + 1) * 32.0
    cx = BX0 + (cell[0] - cell[1]) * 16.0
    cols = set(runs) | {int(cx) + u for u in range(-16, 16)}
    for c in sorted(cols):
        u = abs(c + 0.5 - cx)
        D = D_cell + 32.0 - 2.0 * min(u, 16.0)      # the cell's front edges; its side corners past them
        n = (1.0, 0.0, 0.0) if c + 0.5 >= cx else (0.0, 0.0, 1.0)
        for (r0, r1) in runs.get(c, ()):
            column_strip(m, part, c, r0, r1, lambda r, D=D: D, n, HANGS)
        if u >= 16.0:
            continue
        # The cell's top from the art's bottom (the cell's middle line where the
        # art has no pixel in this column) down to the edge.
        v_row = D / 4.0 - Yf / 2.0 + BY0
        back = (runs[c][-1][1] + 1) if c in runs else (D_cell / 4.0 - Yf / 2.0 + BY0)
        a, b = int(math.ceil(back)), int(math.ceil(v_row))
        if b > a:
            column_strip(m, part_v, c, a, b - 1, lambda r, D=D: D, n, SHEET | ASDRAWN | NOGAP | BG0ART)


def build_plug(m):
    """The cliff across the stairs' mouth, as BG0 draws it until the scene opens it:
    the face and the top behind it. Only BG0's pixels: the left cluster's spikes
    before it are theirs."""
    z0, z1 = PLUG_Z
    m.poly("plug", [(WALL_X, PLUG_FOOT, z0), (WALL_X, WALL_TOP, z0), (WALL_X, WALL_TOP, z1), (WALL_X, PLUG_FOOT, z1)],
           BG0ART | NOGAP)
    m.poly("plug", [(0.0, WALL_TOP, z0), (0.0, WALL_TOP, z1), (WALL_X, WALL_TOP, z1), (WALL_X, WALL_TOP, z0)],
           BG0ART | NOGAP)


def build_parts(m, plug=True):
    build_cluster(m, "cluster_left", art.LEFT)
    build_cluster(m, "cluster_right", art.RIGHT)
    for cell, floor, runs in art.SMALL:
        build_small(m, cell, floor, runs)
    n = len(m.tris)
    if plug:
        build_plug(m)
    return len(m.tris) - n


if __name__ == "__main__":
    m = Model("map84")
    plug_tris = build_parts(m)
    assert len(m.verts) < 65536, len(m.verts)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map84.inc")
    m.write_inc(out, 84, "tools/models/room84.py")
    with open(out, "a") as o:
        o.write("// The last %d triangles are the wall across the stairs' mouth (part plug).\n" % plug_tris)
        o.write("constexpr int kModel84PlugTris = %d;\n" % plug_tris)
    print("%d vertices, %d triangles (%d the plug's) -> %s" % (len(m.verts), len(m.tris), plug_tris, os.path.normpath(out)))
