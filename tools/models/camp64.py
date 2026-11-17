#!/usr/bin/env python3
"""Map 64, the snowy night camp in the woods (Thor state 161, Cybil at the
fire). The same camp as map 63 (camp63.py), drawn again under snow: the
three tents, their ropes, rings and poles are map 63's drawing moved whole,
pixel for pixel (their fire-lit BG1 outlines coincide exactly), so their
model is map 63's, moved by MOVE. The props differ: four barrels (map 63's
barrel drawing, each matched alone) and two stacks of two crates (measured).

Fitted on the flat frame at 480 x 270 (perspective off, unlit): sx = (X - Z)
/ 2 + 248, sy = (X + Z) / 4 - Y / 2 + 127 (checked on the grid's corners:
X = Z = 0 at row 119, the front corner at 247). Map 63's frame: sx = (X - Z)
/ 2 + 232, sy = (X + Z) / 4 - Y / 2 + 79. A drawing moved by (ix, iy)
pixels from map 63's frame to this one, on the floor (Y 16 in both), is a
move of X - Z = 2 (ix - 16), X + Z = 4 (iy - 48).

    tools/models/camp64.py                      writes src/kol_model_map64.inc
    tools/models/camp64.py fit RGB INFO OUT [x0 y0 x1 y1]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import camp63  # noqa: E402
from camp63 import FLOOR, LEFT, barrel  # noqa: E402
from model import Model, overlay  # noqa: E402


def frame_move(ix, iy):
    """Map 63 world -> map 64 world for a drawing moved (ix, iy) pixels."""
    d, s = 2.0 * (ix - 16), 4.0 * (iy - 48)
    return (s + d) / 2.0, (s - d) / 2.0


# The tents: map 63's left and back tents found in this frame by correlating
# their pixels (map 63's model mask) with this frame's: both peak at (1, 26)
# (left 0.89 on the colours, the back 0.74; the right tent, darker here,
# peaks there on the edges), and the BG1 outlines (tents' lit faces, ropes,
# rings, poles) then coincide pixel for pixel. Not the fire's move (0, 24):
# map 64's fire, (80, 16, 80), sits 2 px higher on the camp than map 63's.
MOVE = frame_move(1, 26)            # (-59, -29)

# The poles end at their flags' tops, as map 63's left one does: the scene's
# flag sprites (left over the left tent, right over the right one) start at
# rows 75 and 62 here, 2 and 3 px off map 63's place on the camp; over the
# tents' middles that is Y 143.5 for both. The back tent has no flag: its
# pole keeps map 63's top.
# The back one ends at 113.5 (the reviewer, 2026-11-09: map 63's 148 stood
# over the woods here, columns 238 rows 31-48, where the art has no pole).
POLE_TOPS = {"left": 143.5, "right": 143.5, "back": 113.5}


def copy_moved(dst, src, dx, dz, skip=()):
    """src's triangles (but its `skip` parts) into dst, moved along the floor."""
    remap = {}
    for (a, b, c, part, flags) in src.tris:
        name = src.parts[part]
        if name in skip:
            continue
        idx = []
        for i in (a, b, c):
            if i not in remap:
                v = src.verts[i]
                remap[i] = dst.vert((v[0] + dx, v[1], v[2] + dz), v[3:])
            idx.append(remap[i])
        dst.tris.append(tuple(idx) + (dst.part(name), flags))


def centres():
    """The tents' middles as camp63 places them (left_tent, other_tents),
    moved onto this map."""
    dx, dz = MOVE
    rx, rz = camp63.centre_from_apex(263.8, 28.0)
    c = {
        "left": (LEFT["cx"], LEFT["cz"]),
        "right": (rx - 2.0 * camp63.RIGHT_UP, rz - 2.0 * camp63.RIGHT_UP),
        "back": (-2.3 + 2.0 * camp63.BACK_DOWN, -12.7 + 2.0 * camp63.BACK_DOWN),
    }
    return {k: (x + dx, z + dz) for k, (x, z) in c.items()}


def tents(m):
    """Map 63's three tents and their ropes, moved; the poles again, to this
    map's flags."""
    src = Model("camp63")
    camp63.left_tent(src)
    camp63.other_tents(src)
    dx, dz = MOVE
    copy_moved(m, src, dx, dz, skip=("pole",))
    for k, (cx, cz) in centres().items():
        m.cyl("pole", (cx, LEFT["apex_y"] - 4.0, cz), (cx, POLE_TOPS[k], cz), 0.8, sides=6)
    snow_lip(m)


# The right tent's snow lip (the reviewer, 2026-11-09): on map 63 the right
# tent's eave on its face toward the fire (+Z) meets the art to the pixel; here
# the snow on that roof hangs lower, the wall's dark top band starting at rows
# 100, 102, 104, 104, 104, 106, 106, 108, 108, 112, 113, 113, 113, 115, 115 in
# columns 280-308 (every second), a mean 3.2 px under the moved eave; with the
# camera low that lip, left on the wall's plane, stretched into a dashed band.
# So that face's roof goes on past its eave: LIP_OUT along +Z, dropping
# LIP_DROP, the whole eave's length, part of the roof; at the left corner it
# starts from nothing over LIP_TAPER along X, so it stays inside the tent's
# outline (the wall flares out under the corner only slowly). Map 64 only.
LIP_OUT, LIP_DROP, LIP_TAPER = 5.5, 1.5, 7.0


def snow_lip(m, n=8):
    cx, cz = centres()["right"]
    a, ey = LEFT["eave"], LEFT["eave_y"]
    z0 = cz + a
    rows = [[], []]
    for j in range(n + 1):                       # from the front corner to the left one (-X)
        x = cx + a - 2.0 * a * j / n
        t = min(1.0, (x - (cx - a)) / LIP_TAPER)
        rows[0].append((x, ey, z0))
        rows[1].append((x, ey - LIP_DROP * t, z0 + LIP_OUT * t))
    # The taper's end as a column of its own, so the lip is full past it.
    xt = cx - a + LIP_TAPER
    k = next(j for j, p in enumerate(rows[0]) if p[0] < xt)
    rows[0].insert(k, (xt, ey, z0))
    rows[1].insert(k, (xt, ey - LIP_DROP, z0 + LIP_OUT))
    m.strip("tent_right_roof", rows)


# The barrels: each is map 63's near barrel drawing (camp63.barrel, on the
# floor at (51.2, -4.4) there), found alone by correlating its pixels, moved
# by (ix, iy) px:
#   left of the left tent, beside the crates (-106, 79): colours 0.69;
#   behind, before the back tent (-8, 35): colours 0.43 (the art is soft
#     there), its BG1 hoops 0.63 of their pixels in common;
#   right of the right tent (71, 63): colours 0.52, its foot's outline at
#     row 149 as map 63's;
#   behind that one (85, 56): colours 0.49, cut by the art's right edge.
BARRELS = [(-106, 79), (-8, 35), (71, 63), (85, 56)]


def barrels(m):
    for (ix, iy) in BARRELS:
        dx, dz = frame_move(ix, iy)
        barrel(m, 51.2 + dx, -4.4 + dz)


# The crates left of the left tent: two green crates stacked, one box (map 63
# has no crate: measured here). Their outline: the left edge between columns
# 127 and 128 (the upper crate's outline at 127, the lower's at 128), the
# right one between 147 and 148, the top's back corner at (137, 129.75), its
# left corner at row 134.25, the foot's left corner at row 155 and the front
# one at (138.5, 160.5): 22 along X (the face toward the bottom left, 11 px),
# 18 along Z (9 px), 41.5 high, the front foot on the floor at (-26.5,
# 192.5). The art is soft here (2 x 2 blocks): +-1 px.
#
# The two crates between the back and right tents, where map 63 has its two
# barrels: the colours there are soft (the art's upper rows are blocky), so
# measured on their BG1 outlines, which are sharp (their faces toward the
# fire are lit): the lower crate's top edge from (257, 96) to its front
# corner (271, 103), its front edge at column 271 down to row 113; the upper
# one set back on it, its left edge at column 261 from row 85, its front
# edge at 270.5 from row 90 to 98, its right corner at column 283.5 (the
# green's edge), its top's back corner at (274, 78.5). Lower 28 along X, 20
# high; upper 19 along X, 26 along Z, 16.5 high; the lower's Z, hidden by
# the right tent, taken as the upper's. +-2 px.
CRATES = [
    ((-48.5, FLOOR, 174.5), (-26.5, FLOOR + 41.5, 192.5)),    # left, two crates as one box
    # (Reviewed: the lower crate's BG1 base runs to row 116, not 113, and both
    # stood inside the back tent's billowed wall, which took 79 crate pixels.
    # The lower one now (-12, 16, -56)-(16, 41, -30), its measured top edges
    # where they were; the upper slid +5 along the view ray, the same on the
    # flat frame, onto it. 79 -> 3 pixels lost to the tent.)
    ((-12.0, FLOOR, -56.0), (16.0, 41.0, -30.0)),             # back, lower
    ((-13.5, 41.0, -65.5), (5.5, 57.5, -39.5)),               # back, upper
]
# The grey bundle by the left tent's door (BG1, columns 170-184, rows
# 148-162), only on this map: unmodelled, the tent wall's foot took its top 8
# rows and it tore with the camera low (the reviewer's box).
BUNDLE = ((2.0, FLOOR, 143.0), (16.0, 32.0, 157.0))


def crates(m):
    for lo, hi in CRATES:
        m.box("crate", lo, hi)
    m.box("bundle", *BUNDLE)


def build():
    m = Model("camp")
    tents(m)
    barrels(m)
    crates(m)
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=4)
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map64.inc")
    m.write_inc(out, 64, "tools/models/camp64.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
