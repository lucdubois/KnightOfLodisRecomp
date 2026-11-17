#!/usr/bin/env python3
"""Map 65, a room in a house (states/thor/0043; README "Map 65: a room,
modelled"). The game's grid is the floor (height 1, Y 16) and the front of
the left wall (cells 1,3 and 1,4 at height 9, Y 144); the rest of the
walls, the doorway and the furniture are art on BG0, so under the
perspective they lay flat with the floor or hung in the sky. Two walls a
tile (32) thick, as the art draws their cut ends, measured on the flat
scene (KOL_FOCUS_DUMP .rgb, its projection):
- the back wall (the windows), inner face Z 0, outer Z -32, from the
  corner to its cut end at X 288 (screen x 319);
- the left wall (the doorway), inner face X 64, outer X 32, from the back
  to its cut end at Z 160 (screen x 113-129), the grid's height-9 cells
  its front part;
- the doorway through it at Z 64-101 (the grid's 1,2 its threshold, 0,2
  outside; its near side measured: the art's opening starts at screen
  column 158 on every row, the wall ends at 157, so Z 101 on X 64; the
  grid's 96 cut it short: "a bit short on the left"), its lintel's
  underside at Y 96: the inner face's edge over the
  opening runs from screen (158, 99) to (175, 91), Y 98 and 97 on X 64
  (the dark under it is the far jamb, Z 64, x 160-176; 104 first, by eye:
  "cut too high").
Both walls' tops at Y 144 (the back corner's inner edge 64 rows over its
foot at screen (208, 115)).

The table, the three chairs and the three sconces from the user's paintings
(TABLE, CHAIR_*, CHAIR2_*, CHAIR3_*, SCONCE).

    tools/models/room65.py                      writes src/kol_model_map65.inc
    tools/models/room65.py fit PNG INFO OUT [x0 y0 x1 y1]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import LIGHTPASS, Model, overlay  # noqa: E402

FLOOR = 16.0
TOP = 144.0
T = 32.0                # walls a tile thick
LX = 64.0               # the left wall's inner face
X1, Z1 = 288.0, 160.0   # the back wall's and the left wall's cut ends
DOOR_Z0, DOOR_Z1, DOOR_TOP = 64.0, 101.0, 96.0


# The table, from the user's painting (shots/room65/table_drawn.png on
# room_template_4x.png: yellow its top, red its face toward the bottom left
# (+Z), cyan its end toward the bottom right (+X)), solved as a box: the
# edge where red meets cyan is its front corner (its foot at screen 239.5,
# 179: X 223.5, Z 96.5), 19.5 tall; red runs 94 along X, cyan 32 along Z.
# That is the grid's three cells 4-6 on row 2 (stored at 2, drawn at 1 now:
# kDrawnCells), so it is taken whole on them: X 128-224, Z 64-96.
TABLE = ((128.0, FLOOR, 64.0), (224.0, 35.5, 96.0))


# The chair fallen at the table's front left, from the user's painting
# (shots/room65/chair_fallen_drawn.png: red a triangle, its near end;
# cyan and yellow its two sloping faces): a wedge lying on the floor. Each
# end's two lower corners solved on the floor (Y 16), its apex on the
# plane through them: near (126.5, 125.5), (165.3, 121.8), apex (160.2,
# 32.4, 122.2); far (122.5, 105.5), (164.3, 101.8), apex (156.9, 33.1,
# 102.4). The two apexes agree within a unit: about 40 wide, 20 deep, 17
# high, turned ~5 degrees.
CHAIR_NEAR = ((126.5, FLOOR, 125.5), (160.2, 32.4, 122.2), (165.3, FLOOR, 121.8))
CHAIR_FAR = ((122.5, FLOOR, 105.5), (156.9, 33.1, 102.4), (164.3, FLOOR, 101.8))

# The chair standing at the table's front, from the user's painting
# (shots/room65/chair_upright_drawn.png: red a triangle, its near end, its
# right edge upright; cyan its right face): a wedge on its end, the slope
# toward -X. Near end: floor from X 166.5 to the front corner (188.9,
# 119.1), the apex over that corner at Y 64.75; far end: the bottom right
# corner (188.2, 99.4) and its top at Y 61.8 (the painted top edge falls
# toward the back), the hidden bottom left as the near one moved the same.
CHAIR2_NEAR = ((166.5, FLOOR, 118.5), (188.9, 64.75, 119.1), (188.9, FLOOR, 119.1))
CHAIR2_FAR = ((165.8, FLOOR, 98.8), (188.2, 61.8, 99.4), (188.2, FLOOR, 99.4))

# The chair lying at the table's front right, from the user's painting on
# the second template (shots/room65/chair_front_drawn.png, scene (96, 24)
# at 4x: yellow its top, red and cyan its two sides): a triangular prism
# standing upright, its three edges' feet on the floor and their tops as
# painted (the top slopes, highest at the back right).
CHAIR3_FEET = ((210.1, 113.9), (238.4, 129.1), (246.9, 92.6))
CHAIR3_TOPS = (31.5, 34.75, 40.75)


def prism(m, name, feet, tops):
    bottom = [(x, FLOOR, z) for x, z in feet]
    top = [(x, y, z) for (x, z), y in zip(feet, tops)]
    pts = bottom + top
    centre = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
    solid_poly(m, name, top, centre)
    solid_poly(m, name, bottom, centre)
    for i in range(3):
        j = (i + 1) % 3
        solid_poly(m, name, [bottom[i], bottom[j], top[j], top[i]], centre)

# The three wall sconces, from the user's painting (shots/room65/
# torches_drawn.png: the first template at (138, 51) on a wider canvas;
# yellow the top, cyan and red the two faces under it): all one shape, an
# upside-down pyramid. Solved on the back wall's middle one (its back on
# the wall, Z 0): the top at Y 100.5, its back edge X 153.25-159.25 on the
# wall, its front edge X 156.1-165.9 15.25 out; its point on the wall at
# (156.5, 70). The right one is the same 96 along X. The left wall's is its
# mirror (X and Z swapped): on that wall's face (X 64) it comes 5 lower (a
# point slid along the line of sight, +5 in X, Y and Z, draws the same).
SCONCE = {"top": 100.5, "back": (153.25, 159.25), "front": (156.1, 165.9), "out": 15.25, "point": (156.5, 70.0)}


def sconce(m, along, wall, back_wall=True, drop=0.0):
    """One sconce: `along` moves it along its wall from the painted middle
    one, `wall` is that wall's inner face (Z on the back wall, X on the left)."""
    s = SCONCE
    y, py = s["top"] - drop, s["point"][1] - drop
    # In the back wall's frame: u along the wall (X), d out of it (Z).
    pts = [(s["back"][0], y, 0.0), (s["back"][1], y, 0.0), (s["front"][1], y, s["out"]), (s["front"][0], y, s["out"]),
           (s["point"][0], py, 0.0)]
    if back_wall:
        P = [(u + along, yy, wall + d) for u, yy, d in pts]
    else:   # mirrored onto the left wall: X out of it, Z along it
        P = [(wall + d, yy, u + along) for u, yy, d in pts]
    t0, t1, f1, f0, a = P
    centre = tuple(sum(p[i] for p in P) / 5.0 for i in range(3))
    for face in ([t0, t1, f1, f0], [t0, f0, a], [f0, f1, a], [f1, t1, a], [t1, t0, a]):
        solid_poly(m, "sconce", face, centre)


def solid_poly(m, name, pts, centre):
    """A polygon of a solid, its corners turned so it faces away from `centre`."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * (a[i] - centre[i]) for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts)


def wedge(m, name, near, far):
    pts = near + far
    centre = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
    n1, n2, n3 = near
    f1, f2, f3 = far
    for face in ([n1, n2, n3], [f1, f2, f3], [n1, n3, f3, f1], [n1, n2, f2, f1], [n2, n3, f3, f2]):
        solid_poly(m, name, face, centre)


# The back wall's two windows, cut through it so the light reaches the room
# (a solid wall stopped every ray to the sun): the glass measured on the art
# (its blue, on the wall's inner face Z 0), X 112-124 and 208-220, Y 56-100,
# a little wider for the frames. A pane of glass on the inner face lets the
# light through (LIGHTPASS) and keeps the window's pixels on the wall.
WINDOWS = ((108.0, 128.0), (204.0, 224.0))
WIN_Y0, WIN_Y1 = 56.0, 100.0


def back_wall(m):
    x = LX - T
    for wx0, wx1 in WINDOWS:
        m.box("back", (x, FLOOR, -T), (wx0, TOP, 0.0))
        m.box("back", (wx0, FLOOR, -T), (wx1, WIN_Y0, 0.0))
        m.box("back", (wx0, WIN_Y1, -T), (wx1, TOP, 0.0))
        m.poly("glass", [(wx0, WIN_Y0, 0.0), (wx1, WIN_Y0, 0.0), (wx1, WIN_Y1, 0.0), (wx0, WIN_Y1, 0.0)], LIGHTPASS)
        x = wx1
    m.box("back", (x, FLOOR, -T), (X1, TOP, 0.0))


def build():
    m = Model("room65")
    m.box("table", *TABLE)
    wedge(m, "chair", CHAIR_NEAR, CHAIR_FAR)
    wedge(m, "chair", CHAIR2_NEAR, CHAIR2_FAR)
    prism(m, "chair", CHAIR3_FEET, CHAIR3_TOPS)
    sconce(m, 0.0, 0.0)
    sconce(m, 96.0, 0.0)
    sconce(m, -22.5, LX, back_wall=False, drop=5.0)
    back_wall(m)
    m.box("left", (LX - T, FLOOR, 0.0), (LX, TOP, DOOR_Z0))
    m.box("left", (LX - T, DOOR_TOP, DOOR_Z0), (LX, TOP, DOOR_Z1))
    m.box("left", (LX - T, FLOOR, DOOR_Z1), (LX, TOP, Z1))
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=4, alpha=0.2)
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map65.inc")
    m.write_inc(out, 65, "tools/models/room65.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
