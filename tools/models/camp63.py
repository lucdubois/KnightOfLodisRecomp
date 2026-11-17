#!/usr/bin/env python3
"""Map 63, the night camp in the woods (states/thor/0039; README "Map 63: the
camp, modelled"). The game's grid is a flat clearing at height 1 (Y 16; the
user's drawn heights carry it past the grid); the tents, ropes and poles are
art on BG0 that lay flat under the perspective. Each part fitted to the
user's painting on the flat frame (shots/camp63/*_drawn.png, the 384 x 216
view at 4x; the template's pixel (x, y) is the scene's (x + 32, y + 23)).

    tools/models/camp63.py                      writes src/kol_model_map63.inc
    tools/models/camp63.py fit RGB INFO OUT [x0 y0 x1 y1]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, overlay  # noqa: E402

FLOOR = 16.0


def tent(m, name, cx, cz, base, eave, eave_y, apex_y, billow=8.0, sag=8.0, n=8):
    """A square tent: walls from a `base`-half-wide square on the floor up to
    an `eave`-half-wide one at `eave_y` (the canvas flares), each billowing
    outward at its foot's middle; a pyramid roof to `apex_y` over the
    middle, each face sagging between the eaves and the apex (the user:
    "there is a slight curve to every face of the tent")."""
    corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]   # back, right, front, left (anticlockwise from above)
    for k in range(4):
        (ax, az), (bx, bz) = corners[k], corners[(k + 1) % 4]
        out = ((ax + bx) / 2.0, (az + bz) / 2.0)        # the face's outward direction
        rows = []
        for i in range(n + 1):                          # bottom to top
            v = i / n
            h = base + (eave - base) * v
            y = FLOOR + (eave_y - FLOOR) * v
            row = []
            for j in range(n + 1):
                u = j / n
                px, pz = cx + h * (ax + (bx - ax) * u), cz + h * (az + (bz - az) * u)
                b = billow * math.sin(math.pi * u) * (1.0 - v) ** 2
                row.append((px + out[0] * b, y, pz + out[1] * b))
            rows.append(row)
        m.strip(name + "_wall", rows)
        rows = []
        for i in range(n + 1):                          # eave to apex
            v = i / n
            row = []
            for j in range(n + 1):
                u = j / n
                ex, ez = cx + eave * (ax + (bx - ax) * u), cz + eave * (az + (bz - az) * u)
                row.append((ex + (cx - ex) * v, eave_y + (apex_y - eave_y) * v - sag * 4 * v * (1 - v), ez + (cz - ez) * v))
            rows.append(row)
        m.strip(name + "_roof", rows)


def rope(m, a, b, r=0.6):
    m.cyl("rope", a, b, r, sides=4, caps=False)


# The left tent (shots/camp63/tent_left_drawn.png: blue and red its walls,
# orange and green its roof, yellow the ropes, magenta the flag). Measured
# in template pixels: the walls' foot at the front corner (119.5, 104.75),
# left (80, 85.75), right (158.75, 85.75); the eaves (119.75, 72.75),
# (86, 57), (153, 57); the apex (119.75, 37). With the map's projection
# (template x = (X - Z) / 2 + 184, y = (X + Z) / 4 - Y / 2 + 52): a square
# 78 at the floor, 67 at the eaves (Y 73.5), the apex at 113.5, over (19.25,
# 147.75). The hip edges sag ~4 px (8 units) at their middle, the walls'
# feet ~2 px at theirs.
LEFT = dict(cx=19.25, cz=147.75, base=39.0, eave=33.5, eave_y=73.5, apex_y=113.5)


def left_tent(m):
    t = LEFT
    tent(m, "tent_left", **t)
    cx, cz, b, a = t["cx"], t["cz"], t["base"], t["eave"]

    def edge(sx, sz, frac):      # a point down a corner's edge, `frac` from the eave
        h = a + (b - a) * frac
        return (cx + sx * h, t["eave_y"] + (FLOOR - t["eave_y"]) * frac, cz + sz * h)
    # Ropes: the front one straight down the screen from the front eave
    # corner to its stake at (120.5, 122.5) -> (93.5, 220.5); the right one
    # from (154, 63) on the right edge to (183.25, 93.5) -> (98.25, 99.75);
    # the left one from (84.75, 65.5) on the left edge, past the view's edge
    # along its drawn slope (its stake 113 out along Z, 9 back in X).
    rope(m, edge(1, 1, 0.0), (93.5, FLOOR, 220.5))
    rope(m, edge(1, -1, 0.21), (98.25, FLOOR, 99.75))
    lt = edge(-1, 1, 0.3)
    rope(m, lt, (lt[0] - 9.0, FLOOR, lt[2] + 113.0))
    # The flag's pole over the apex, to the flag's top (120, 19.75) -> Y 148.
    m.cyl("pole", (cx, t["apex_y"] - 4.0, cz), (cx, 148.0, cz), 0.8, sides=6)


def floor_at(x, y):
    """The floor point (Y 16) under template pixel (x, y)."""
    d, s = (x - 184.0) * 2.0, (y - 52.0 + FLOOR / 2.0) * 4.0
    return ((s + d) / 2.0, FLOOR, (s - d) / 2.0)


def centre_from_apex(x, y):
    """A tent with LEFT's size whose apex is at template pixel (x, y)."""
    d, s = (x - 184.0) * 2.0, (y - 52.0 + LEFT["apex_y"] / 2.0) * 4.0
    return (s + d) / 2.0, (s - d) / 2.0


# The other two tents are the left one's shape (the user suggested the model
# mirrored onto the two other tents; square, so mirrored only in
# their ropes). The right tent's apex at (263.8, 28) -> (145.3, -14.3); its
# eave corners read within 2 px of that. Its front rope to the ring at
# (263.8, 106.7), the one toward the fire from its left edge to (203.3,
# 76.7). The back tent's apex is above the view: placed by its pole (x 189.2)
# and eave corners (y ~11.5) -> (-2.3, -12.7); its front rope to the ring at
# (189.2, 76.7).
# Then the user, on the first fit, asked for the right one a bit
# higher and the further one a bit lower: 4 px each, slid along
# the floor's screen-vertical (X and Z together, 2 units a pixel).
RIGHT_UP = float(os.environ.get("CAMP_RIGHT_UP", "4"))     # px up the screen
BACK_DOWN = float(os.environ.get("CAMP_BACK_DOWN", "4"))   # px down


def other_tents(m):
    a, b = LEFT["eave"], LEFT["base"]
    dims = {k: LEFT[k] for k in ("base", "eave", "eave_y", "apex_y")}

    def edge(cx, cz, sx, sz, frac):
        h = a + (b - a) * frac
        return (cx + sx * h, LEFT["eave_y"] + (FLOOR - LEFT["eave_y"]) * frac, cz + sz * h)
    cx, cz = centre_from_apex(263.8, 28.0)
    cx, cz = cx - 2.0 * RIGHT_UP, cz - 2.0 * RIGHT_UP
    tent(m, "tent_right", cx=cx, cz=cz, **dims)
    rope(m, edge(cx, cz, 1, 1, 0.0), floor_at(263.8, 106.7))
    rope(m, edge(cx, cz, -1, 1, 0.21), floor_at(203.3, 76.7))
    m.cyl("pole", (cx, LEFT["apex_y"] - 4.0, cz), (cx, 148.0, cz), 0.8, sides=6)
    cx, cz = -2.3 + 2.0 * BACK_DOWN, -12.7 + 2.0 * BACK_DOWN
    tent(m, "tent_back", cx=cx, cz=cz, **dims)
    rope(m, edge(cx, cz, 1, 1, 0.0), floor_at(189.2, 76.7))
    m.cyl("pole", (cx, LEFT["apex_y"] - 4.0, cz), (cx, 148.0, cz), 0.8, sides=6)


# The pack left of the left tent and the bedroll on it (shots/camp63/
# pack_left_drawn.png: red the box's face toward the bottom left, orange
# toward the bottom right, cyan its top; green the roll, yellow its end).
# The box's front edge at template (99.4, 97.3-109.3): 24 high, on the floor
# at (46, 215.2); the red face 13 px left (26 along X), the orange 10.85 right
# (21.7 along Z). The roll as drawn: from its left end (84, 83.5) to the
# yellow end's middle (110.4, 93), slope 0.36 (not along X's 0.5): turned
# toward -Z, 46 long, radius 5.5, resting on the box (Y 40) and over both
# ends.
def pack_left(m):
    m.box("pack", (20.0, FLOOR, 193.5), (46.0, 40.0, 215.2))
    m.cyl("bedroll", (8.5, 45.5, 208.5), (53.9, 45.5, 201.1), 5.5, sides=12)


# The two barrels before the back tent (shots/camp63/barrels_drawn.png: red
# the near one's side, cyan the far one's, yellow their tops). Each fitted
# by its side's width (the widest 16 px at mid-height: radius 11.3), its
# side's lowest row (the base's front on the floor, base radius 10) and its
# top's middle (radius 9): the near one on the floor at (51.2, -4.4), the
# far at (51.7, -28.9), both 27.5 high, touching.
def barrel(m, x, z, h=27.5, r0=10.0, rm=11.3, r1=9.0):
    mid = FLOOR + h / 2.0
    m.cyl("barrel", (x, FLOOR, z), (x, mid, z), r0, sides=14, caps=False, r1=rm)
    m.cyl("barrel", (x, mid, z), (x, FLOOR + h, z), rm, sides=14, r1=r1)


def barrels(m):
    barrel(m, 51.2, -4.4)
    barrel(m, 51.7, -28.9)


# The pack right of the right tent and the boots on it (shots/camp63/
# pack_right_drawn.png: red, cyan and yellow the box's faces toward the
# bottom left, bottom right and its top; green, magenta and orange the
# boots'). The box's front edge at template x 281, from the floor at y 98.4
# to 88 (21 high), its faces 9.75 px left (19.5 along X) and 12.75 right
# (25.5 along Z); the boots' front edge at x 280.5 from 86.9 (on the box's
# top) to 80.25 (13 high), 6.5 px left (13) and 7.25 right (14.5).
def pack_right(m):
    m.box("pack", (186.3, FLOOR, -13.7), (205.8, 37.0, 11.8))
    m.box("boots", (190.3, 37.0, -4.2), (203.3, 50.0, 10.3))


def build():
    m = Model("camp")
    left_tent(m)
    other_tents(m)
    pack_left(m)
    barrels(m)
    pack_right(m)
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=4)
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map63.inc")
    m.write_inc(out, 63, "tools/models/camp63.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
