#!/usr/bin/env python3
"""Map 94: a rocky outcrop jutting over a sea of clouds, two characters on its
summit (a story scene). The game's grid has only cells x 0-1, z 0-8 at height
1 (Y 16, the strip the two stand on); every other cell is off the field
(byte ff, height 0), so without a model the whole cliff lay flat as ground.

    tools/models/room94_extract.py <dump>   writes room94_art.py (the art per column)
    tools/models/room94.py                  writes src/kol_model_map94.inc

The art (Thor state 0228, a dialogue, camera at rest; flat frame 384 x 216,
sx = (X - Z)/2 + KX, sy = (X + Z)/4 - Y/2 + KY, KX 208, KY 124: the unit at
16,16,16 stands at 208,124): the cliff AND the clouds are on BG0, BG1 only
the cliff's top outline. Rock is r > b (a brown top, a yellow lit face), the
clouds b >= r. Two rocks, split by a crack of cloud a pixel wide: the main
cliff and a lower spire at the lower right. A pixel is sampled at its corner
(integer sx, sy): every edge below lies E = 0.1 before a pixel boundary, so a
quad from c - E to c + 1 - E holds column c and only it.

How each rock is built, column by column (the art's own silhouette, to the
pixel, as room75 does):
  - its TOP is level, at the height the units stand on (main: Y 16, the
    grid's cells; spire: SPIRE_Y), from the column's first rock row down to
    the face's top edge. At a fixed Y a pixel is one world point.
  - its FACE is the yellow palette: it starts on a line through each column's
    first yellow row (FIRST_FACE, a running median, half a row above it), and
    hangs straight down from that line's point on the top: every column of
    the face a vertical line in the world (X, Z fixed, Y falling 2 a row). So
    the face meets the top with no step in depth, and where the art's rock
    ends (the face's right silhouette leaning back under the summit: an
    overhang) the column ends.
  - runs touching the frame's bottom go on EXT_ROWS rows past it, and the
    plateau EXT_COLS columns past its left edge (its far edge continued at
    its slope), for the camera's overscan.
The main face's edge (where the brown top ends, SPLIT) is three pieces: from
the summit's tip (241,125.5) down to (195,159.5) at the art's slope (mostly
along Z: the face looks +X, toward the art's light from the right); there a
concave corner, the edge dropping to (194,177.5) between two columns; then
down-left steeply, -3.33 rows a column, to the frame's bottom (row 215.9 at
column 182.5), where it stops: past the frame's bottom and left of it all is
the plateau's top. The face does NOT hang from that steep piece (it runs
nearly along the view: hung from it, the face was a wall edge-on to the
camera, 51 units painted from one column at the corner, and with the stick
moved its stripes swept the frame: upright pixels 3242 at rest, 5898 at look
0.5,0.5, 14518 at 1,1). Every column of the face hangs from HANG, the
tip-to-corner line continued left at its own slope, so it all faces +X; under
the steep piece the brown spur at Y 16 stands in front of it, a step in depth
(its silhouette), not a wall.
The spire: its top at SPIRE_Y = -112, BEHIND the main face: in the art the
main face's dark outline runs on unbroken past (220,196) and the spire's top
outline stops at it (a T-junction). At -48 it stood ~120 in front and swung
clear of the face with the stick; below about -89 it stays behind over
columns 221-230. Its face from (275,174.5) to (248,206.5) and on down-left at
-1.5 (its edge and its hang the same line).

The clouds are left as they are (the user, 2026-11-12): no backdrop part; they
stay the off-field ground at Y 0, as the game's grid places them.

Left alone: the far edge's hidden side (a face hanging from it is never seen
and its edge on the top drew as a black line on other maps); the seams in
the columns the user painted past the game's 240.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, TOP, dot, cross, sub  # noqa: E402
import room94_art as art  # noqa: E402

KX, KY = float(art.KX), float(art.KY)
E = 0.1
PLATEAU_Y = 16.0          # the grid's height 1: where the units stand
SPIRE_Y = -112.0          # the lower spire's top: behind the main face
EXT_ROWS = 120            # past the frame's bottom
EXT_COLS = 120            # past the frame's left edge
BOTTOM = art.H - 1 + EXT_ROWS


def on_y(sx, sy, y):
    """The world point seen at (sx, sy) at height y."""
    d = 2.0 * (sx - KX)
    D = 4.0 * (sy - KY) + 2.0 * y
    return ((D + d) / 2.0, y, (D - d) / 2.0)


def median5(vals, c):
    w = sorted(vals[k] for k in range(c - 2, c + 3) if k in vals)
    return w[len(w) // 2]


class Polyline:
    """A rock face's top edge on the screen: row as a function of sx."""

    def __init__(self, pts):
        self.pts = sorted(pts)

    def __call__(self, x):
        p = self.pts
        if x < p[0][0] or x > p[-1][0]:
            return None
        for (x0, y0), (x1, y1) in zip(p, p[1:]):
            if x0 <= x <= x1:
                return y0 + (y1 - y0) * (x - x0) / (x1 - x0) if x1 > x0 else y1
        return p[-1][1]

    def xs(self):
        return [q[0] for q in self.pts]


def art_edge(c0, c1):
    """The art's face edge over columns c0..c1: the first yellow rows (median
    of 5), half a row above."""
    ff = {c: v for c, v in art.FIRST_FACE.items() if c0 <= c <= c1}
    return [(float(c), median5(ff, c) - 0.5) for c in range(c0, c1 + 1)]


def slope_of(pts):
    """Least-squares rows a column."""
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / sum((p[0] - mx) ** 2 for p in pts)


def extended(pts, slope_left, to_row, slope_right):
    """Continued left of its leftmost point at slope_left rows a column (down,
    going left) to to_row, and two columns right at slope_right."""
    pts = sorted(pts)
    xl, yl = pts[0]
    pts.insert(0, (xl - (to_row - yl) / slope_left, float(to_row)))
    pts.append((pts[-1][0] + 2.0, pts[-1][1] - 2.0 * slope_right))
    return Polyline(pts)


MAIN_ART = art_edge(195, 241)
# Where the main rock's top ends: the art's edge, the corner between columns
# 194 and 195, the steep piece (194,177.5)-(185,207.5) fitted (its first yellow
# rows are ragged: 178 184 182 187 195 194 199 198 201 208), stopped at the
# frame's bottom row.
MAIN_SPLIT = extended(MAIN_ART + [(194.6, 159.5), (194.5, 177.5), (185.0, 207.5)], 3.33, art.H - E, 0.9)
# What the main face hangs from: the art's edge, continued left at the slope of
# its left end (columns 195-212).
MAIN_HANG_SLOPE = -slope_of([p for p in MAIN_ART if p[0] <= 212])
MAIN_HANG = extended(MAIN_ART, MAIN_HANG_SLOPE, MAIN_ART[0][1] + MAIN_HANG_SLOPE * 60.0, 0.9)
# The spire's: columns 248-275 from the art, continued down-left at -1.5; it
# hangs from the same line.
SPIRE_EDGE = extended(art_edge(248, 275), 1.5, BOTTOM + 1, 1.0)


def far_edge_slope():
    """The plateau's far edge left of the frame: its slope over columns 0-80."""
    xs = [c for c in range(0, 81) if c in art.MAIN]
    ys = [art.MAIN[c][0][0] for c in xs]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    k = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return k, my - k * mx


def runs_of(rock, c):
    rs = list(rock.get(c, []))
    return [(r0, BOTTOM if r1 == art.H - 1 else r1) for r0, r1 in rs]


def facing(pts, out):
    """Corners ordered so the polygon's normal points along `out` (a top up,
    a face +X): not by the camera's side, which an edge-on face leaves to chance."""
    q = []
    for p in pts:
        if not q or max(abs(a - b) for a, b in zip(p, q[-1])) > 1e-6:
            q.append(p)
    if len(q) > 1 and max(abs(a - b) for a, b in zip(q[0], q[-1])) < 1e-6:
        q.pop()
    if len(q) < 3:
        return None
    n = (0.0, 0.0, 0.0)
    for i in range(1, len(q) - 1):
        c = cross(sub(q[i], q[0]), sub(q[i + 1], q[0]))
        n = (n[0] + c[0], n[1] + c[1], n[2] + c[2])
    if dot(n, n) < 1e-8:
        return None
    if dot(n, out) < 0:
        q.reverse()
    # the first three corners must not be in a line (the kit's normal)
    for s in range(len(q)):
        r = q[s:] + q[:s]
        c = cross(sub(r[1], r[0]), sub(r[2], r[0]))
        if dot(c, c) > 1e-6:
            return r
    return None


UP, PLUS_X = (0.0, 1.0, 0.0), (1.0, 0.0, 0.0)


def add_rock(m, name, rock, cols, top_y, split, hang):
    """Each column's runs: the top down to `split`, the face below it hanging
    from `hang` (the same line but where the main face's corner is)."""
    top_flags, face_flags = TOP, 0

    def top_pt(x, y):
        return on_y(x, y, top_y)

    def face_pt(x, y):
        b = hang(x)
        X, _, Z = on_y(x, b, top_y)
        return (X, top_y - 2.0 * (y - b), Z)

    breaks = sorted(set(split.xs() + hang.xs()))
    for c in cols:
        xa0, xb0 = c - E, c + 1 - E
        xs = [xa0] + [x for x in breaks if xa0 < x < xb0] + [xb0]
        for r0, r1 in runs_of(rock, c):
            yt, yb = r0 - E, r1 + 1 - E
            for xa, xb in zip(xs, xs[1:]):
                ba, bb = split(xa), split(xb)
                if ba is None or bb is None:   # no face here: all top
                    p = facing([top_pt(xa, yt), top_pt(xb, yt), top_pt(xb, yb), top_pt(xa, yb)], UP)
                    if p:
                        m.poly(name + "_top", p, top_flags)
                    continue
                assert hang(xa) is not None and hang(xb) is not None, (name, xa, xb)
                ca, cb = min(max(ba, yt), yb), min(max(bb, yt), yb)
                p = facing([top_pt(xa, yt), top_pt(xb, yt), top_pt(xb, cb), top_pt(xa, ca)], UP)
                if p:
                    m.poly(name + "_top", p, top_flags)
                p = facing([face_pt(xa, ca), face_pt(xb, cb), face_pt(xb, yb), face_pt(xa, yb)], PLUS_X)
                if p:
                    m.poly(name + "_face", p, face_flags)


def main_runs():
    """The main rock's runs, the plateau continued past the frame's left edge."""
    k, b0 = far_edge_slope()
    rock = dict(art.MAIN)
    for c in range(-EXT_COLS, 0):
        rock[c] = [(int(round(k * c + b0)), art.H - 1)]
    return rock


def build():
    m = Model("room94")
    rock = main_runs()
    add_rock(m, "cliff", rock, sorted(rock), PLATEAU_Y, MAIN_SPLIT, MAIN_HANG)
    add_rock(m, "spire", art.SPIRE, sorted(art.SPIRE), SPIRE_Y, SPIRE_EDGE, SPIRE_EDGE)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map94.inc")
    m.write_inc(out, 94, "tools/models/room94.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
