#!/usr/bin/env python3
"""Map 77, a small wrecked cottage kitchen: two half-timbered stone walls
(the left one with a window and a bread oven with its flue, the right one
with a doorway and an open shelf), a rug, a table with a chair, a bench and
a stool, a crate, firewood; at the front right the floor steps down to a
wooden tread. Measured on flat.png (480x270, sprites and HUD hidden),
projection sx = (X - Z)/2 + 160, sy = (X + Z)/4 - Y/2 + 184. The grid is
the floor alone: cells x 0-4 at height 2 (Y 32), the front column x 5 at
height 1 (Y 16, the wooden step), z 0-3, plus a stray cell 0,4.

The game samples pixel (px, py) at its corner, the point (px, py): edges
are put between the last pixel in and the first pixel out (here at
quarter units, so no edge runs through a pixel corner). Lines are given as
constants of s = x + 2y (a line along Z: s = X - Y + 528) or d = x - 2y (a
line along X: d = Y - Z - 208).

A model surface takes its pixels from the terrain wherever it covers them,
so nothing is modelled that the grid's cells would hide: the walls stand on
the floor (Y 32), and below it only their cut ends and the faces past the
grid. The slab's front faces under the floor (Z 128 and X 192, Y 0-32; the
floor's front edge d = -302.5, the outline row on the face) are the grid's
own cell sides, except where the stray cell 0,4 stands in front of them.

    tools/models/room77.py      writes src/kol_model_map77.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, SHEET, LIGHTPASS  # noqa: E402,F401

FLOOR = 32.0
# The walls, 18 thick: the top band is 9 rows deep at every column, the
# left wall's cut end 9 columns wide (x 88-96).
TOP = 129.5         # outer top edges s = 381.75 (top rows at s 382/383) and d = -46.25 (d -48/-47): peak (167.5, 107)
T = 18.0
LX = 1.25           # left wall's inner face: its top's inner edge s = 399.75 (band ends at s 398/399), end face to 96.625
RZ = -14.25         # right wall's inner face: its top's inner edge d = -64.25 (band ends at d -64/-63)
Z1 = 128.0          # left wall's cut end, flush with the slab's front: end face columns 88-96
X1 = 191.5          # right wall's cut end: end face columns 263-271 (262.875..271.875), slab's +X side
BASE = -0.25        # the slab's bottom: silhouette lower edges d = -336.25 (bottom rows d -336/-335) and s = 719.75 (718/719)

# The window in the left wall, set 6 into it, barred outside by two boards
# (the user asked for a few boards across the outside). On the inner face (X LX)
# the opening is the dark outline at column 109 (left edge 109.5: Z 102.25),
# its lintel the dark line row = -0.5 col + 213.75 (2-px runs (109-110, 159)
# ... (119-120, 154): under it, Y 100.75), its right edge between the lit
# jamb (x 118-120) and the wall (x 121): 120.5, Z 80.25. The jamb is the
# opening's +Z-facing side, 3 columns wide: the glass stands 6 back (X -4.75;
# its frame's dark line at column 117 is the jamb's back edge, 117.5). The
# sill: its front edge the dark line row = -0.5 col + 241.75 (109.5, 187) to
# (121.5, 181) (Y 45.75 on the inner face), its top 3 rows in front of the
# glass (the olive band, rows 182-185 at x 113) and, seen through it, on to
# the outer face (the sunlit stone, to its back edge row = -0.5 col + 232.75,
# the dark line (116, 175) ... (110, 178)). Behind the glass two boards
# cross the opening along Z, each a navy top edge, a pale top (rows 162-163
# at x 113-114) and an olive/gold front 3 rows tall (164-166, and 172-174),
# on rows = -0.5 col + 220.25..223.25 and 228.25..231.25: nailed on the outer
# face (fronts at X -16.75, 4 thick), Y 64.75-70.75 and 48.75-54.75. The
# glass pane takes the window's pixels at rest (glass, boards and the outer
# sill as drawn) and lets the window light through; the boards stop it.
WIN_Z0, WIN_Z1 = 80.25, 102.25
WIN_Y0, WIN_Y1 = 45.75, 100.75
GLASS_X = LX - 6.0
BOARD_X0, BOARD_X1 = LX - T - 4.0, LX - T
BOARDS = ((48.75, 54.75), (64.75, 70.75))
BOARD_Z = (WIN_Z0 - 4.0, WIN_Z1 + 4.0)      # nailed 4 past the opening on either side

# The dark beyond the doorway stands 4 behind the wall's outer face (Z -36),
# the threshold's floor running on to it: at the outer face itself, the
# threshold floor at x 193-200 rows 168-171 lay painted on the dark.
DOOR_DARK_Z = -36.0
# The doorway in the right wall: the dark beyond is columns 193-200, so the
# outer face's opening starts at 192.6 (X 33 on Z -32.25) and the inner
# face's ends at 200.6 (X 67 on Z -14.25); its top is the inner face's
# lintel line d = -100.5 (the dark's top rows at d -101/-102): Y 93.5. The
# purple reveal (x 188-192) is the opening's side at X 33. The left jamb
# post (x 180-187, its top along d = -104 under the lintel beam) stands 1
# proud of the wall and hides the reveal's first columns.
DOOR_X0, DOOR_X1, DOOR_TOP = 33.0, 67.0, 93.5
DOOR_POST = ((25.0, FLOOR, RZ), (41.0, 91.5, RZ + 1.0))

# The raised corner by the step: cell 5,0 is drawn at the floor's height,
# not the grid's 1. Its +X face is the pale block x 240-262 (top s = 685.5:
# Y 34 on X 191.5; its left end x 240 is Z 32), its +Z face the dark patch
# from x 224 under it, beside the riser (s 653.5..671.5: X 160, Y 16.5-34.5,
# the grid's own step).
STEP_X0 = 159.5
STEP_Z0, STEP_Z1 = -0.25, 31.75
GRID_Z0 = -0.25     # the floor strip past the grid ends short of the grid's edge (Z 0)
SLAB_X1, SLAB_TOP = 32.25, 32.25   # the slab's front face kept from cell 0,4 (x 97-112)

# The bread oven on the left wall, round in plan and domed, its arched
# mouth facing the room: an upright elliptic cylinder (centre on the wall,
# Z 38; radii 19 in X, 25 in Z) to Y 49, capped by half an ellipsoid 26
# high leaning 4 toward +Z, all cut at the wall. Fitted with the flue to
# the art's own outline (the reviewer's fit): the dome's dark curve
# (141,160) (135,163) (131,169), then x 130 down to row 183 (x 121-129 is
# the window's lit jamb and the wall lit beside it, the wall's foot line
# running on under it), its
# top at rows 158-160 under the flue, the rug starting right under the
# mouth (s 514-516: its front at floor level X ~19). The first fit, twice
# this size, took the reveal and the rug and smeared them with the camera
# moved.
OVEN_C = (LX, FLOOR, 38.0)
OVEN_A, OVEN_B = 19.0, 25.0
OVEN_WALL, OVEN_CAP = 49.0, 26.0
OVEN_LEAN = 4.0
# Its flue, a stone shaft up the wall: its shaded side x 137-140 is 8 deep
# (4 px), its lit front x 141-146 12 wide (6 px), right edge 146.5 -> Z 36;
# its top's front edge s = 403 (the band ends at row 130 at x 143, 2 rows
# below the walls' own): it stands a little over the wall top. 7 deep and
# its top at 132.5 in the fit with the oven.
FLUE = ((LX, FLOOR, 36.0), (LX + 7.0, 132.5, 48.0))

# The firewood stacked in the back corner beside the oven: the logs span
# x 158-174, top row 160, foot row 173 (x 175-178 is shadow and wall, rows
# 174-177 its shadow on the floor).
WOOD = ((2.25, FLOOR, RZ), (15.0, 45.0, 10.0))

# The crate against the right wall: left edge x 211 and its foot along
# d = -181 (Z 5.5); its right foot edge s = 623 (X 127.5); its top's left
# edge s = 580/581 (Y 56 with X 107.5), back edge on the wall (d = -139).
CRATE = ((107.5, FLOOR, RZ), (127.5, 56.0, 5.5))

# The open shelf against the right wall: three boards between posts, its
# right side a panel. The bottom board's foot line d = -178 (Z 3); the left
# post x 227-228 (X 136.5), the front right post x 246-248 (X 180); the
# top's left back edge s = 574 (Y 91); the top board's underside d = -130
# (Y 81), the middle board's top edge d = -154 (Y 56.5), the bottom
# board's top d = -172 (Y 40.5). Between the top board and the middle one
# the wall behind shows through.
SH_X0, SH_X1, SH_Z1 = 136.5, 180.0, 3.0
SH_TOP, SH_TOP_UNDER = 91.0, 81.0
SH_MID = (48.0, 56.5)
SH_LOW = (FLOOR, 40.5)
SH_POST = 3.0
# The two vessels on its top: the bottle x 238-244 (r 4.5), its neck x
# 240-242 (r 2.1) from row 167, foot row 176 (X + Z 146), top row 162
# (Y 116.5); the round vase x 244-250 (r 5.7), neck x 246-248 from row 173,
# foot row 180 (X + Z 160), top row 170 (Y 106.5).
BOTTLE = (154.0, -8.0, 4.5, 2.1, 104.0, 116.5)
VASE = (167.0, -6.875, 5.7, 2.3, 97.5, 106.5)

# The table: its top's back-left edge s = 530 (outline (138,196)-(168,181)),
# its back-right edge d = -191 (172-181, rows 182-186), its front-left edge
# d = -260 (139-150, rows 200-205), its front-right edge s = 562 ((154,204)
# -(172,197)); its right corner is behind the chair. The left leg x 139-143,
# foot row 208, and the front leg x 150-154, foot row 213, fix its height
# (Y 52.5) and the legs (4 square, 3 in from the corners).
TABLE_TOP = ((54.0, 50.5, 33.5), (85.0, 52.5, 102.0))
TABLE_LEG, TABLE_INSET = 4.0, 3.0
# The chair at the table's +X side, its back-rest toward the camera: the
# back's frame x 169-183, its top rail from (170, 190) up to (181, 186)
# (Y 67 on X 94.5), the seat rows 199-203 from x 165 (Y 45, from X 78:
# under the table's edge), legs x 165-166 (foot row 207), 171-172 (208),
# 177-182 (204).
CHAIR = ((88.5, FLOOR, 48.5), (94.5, 67.0, 71.0))
CHAIR_SEAT = (78.0, 39.5, 45.0)     # its -X edge, underside, top
CHAIR_LEG = 3.0
# The bench between the oven and the table, its slatted side toward the
# camera: left edge x 121 (rows 186-196), top-left (122-127, 186-187), foot
# from (122,196) to (133,200) (d = -269.5: Z 92), its top's back right
# under the table's left corner (137, 192).
BENCH = ((13.0, FLOOR, 86.0), (40.0, 47.0, 92.0))
# The stool in front of the table: x 152-162 (r 8.5), its top ellipse rows
# 211-217 (Y 44), legs to row 222.
STOOL = (100.2, 106.2, 8.5, 44.0)


def half_oven(m, name, c, a, b, yw, hc, lean=0.0, wall=None, nu=24, nv=8):
    """An oven bulging from the wall X = `wall` toward +X: an upright
    elliptic cylinder (centre c, radii a in X, b in Z) from c's height to
    yw, capped by half an ellipsoid hc high whose top leans `lean` along Z.
    Each ring is cut where it meets the wall, so nothing lies inside it."""
    wall = c[0] if wall is None else wall
    rows = []
    for i in range(nv + 2):          # the foot, then the cap's rings to the top
        k = 0.0 if i == 0 else (i - 1) / nv
        r = 1.0 if i == 0 else math.cos(math.pi / 2 * k)
        y = c[1] if i == 0 else yw + hc * math.sin(math.pi / 2 * k)
        zc = c[2] + lean * math.sin(math.pi / 2 * k)
        q = (wall - c[0]) / (a * r) if a * r > 1e-9 else -2.0
        tm = math.acos(max(-1.0, min(1.0, q)))      # the arc in front of the wall
        row = []
        for j in range(nu + 1):      # from the wall at +Z round to the wall at -Z
            t = tm * (1 - 2 * j / nu)
            row.append((max(wall, c[0] + a * r * math.cos(t)), y, zc + b * r * math.sin(t)))
        rows.append(row)
    m.strip(name, rows)


def vessel(m, name, x, z, r, rn, yneck, ytop, ybase):
    m.cyl(name, (x, ybase, z), (x, yneck, z), r, sides=12)
    m.cyl(name, (x, yneck, z), (x, ytop, z), rn, sides=8)


def face(m, name, pts, normal, flags=0):
    """A flat polygon turned so its normal is `normal`'s side."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * normal[i] for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts, flags)


def left_wall(m):
    """The left wall, X LX - T .. LX, Z RZ - T .. Z1: its top, its inner face
    (from the back corner, Z RZ) and outer face round the window's opening,
    its cut end and back end, and the opening's sides and sill (no lintel
    underside: it faces down, no camera of the game sees it)."""
    x0, x1, z0, z1, y0, y1 = LX - T, LX, RZ - T, Z1, FLOOR, TOP
    wz0, wz1, wy0, wy1 = WIN_Z0, WIN_Z1, WIN_Y0, WIN_Y1
    face(m, "left_wall", [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], (0, 1, 0))     # top
    for x, n, za in ((x1, 1, RZ), (x0, -1, z0)):     # inner face, outer face
        for (a, b, c, d) in ((za, wz0, y0, y1), (wz0, wz1, y0, wy0), (wz0, wz1, wy1, y1), (wz1, z1, y0, y1)):
            face(m, "left_wall", [(x, c, a), (x, d, a), (x, d, b), (x, c, b)], (n, 0, 0))
    face(m, "left_wall", [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1))     # cut end
    face(m, "left_wall", [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1))    # back end
    face(m, "left_wall", [(x0, wy0, wz0), (x1, wy0, wz0), (x1, wy1, wz0), (x0, wy1, wz0)], (0, 0, 1))  # the lit jamb
    face(m, "left_wall", [(x0, wy0, wz1), (x1, wy0, wz1), (x1, wy1, wz1), (x0, wy1, wz1)], (0, 0, -1))
    face(m, "left_wall", [(x0, wy0, wz0), (x1, wy0, wz0), (x1, wy0, wz1), (x0, wy0, wz1)], (0, 1, 0))  # sill


def window(m):
    """The window's glass (light passes it) and the boards nailed over the
    opening outside (they stop the light: 4 thick, the light's blockers are
    4-unit cubes)."""
    face(m, "window", [(GLASS_X, WIN_Y0, WIN_Z0), (GLASS_X, WIN_Y1, WIN_Z0), (GLASS_X, WIN_Y1, WIN_Z1),
                       (GLASS_X, WIN_Y0, WIN_Z1)], (1, 0, 0), SHEET | LIGHTPASS)
    for (b0, b1) in BOARDS:
        m.box("window_boards", (BOARD_X0, b0, BOARD_Z[0]), (BOARD_X1, b1, BOARD_Z[1]))


def build_parts(m):
    """The room's parts. A model surface takes its pixels from the terrain
    wherever it covers them, so nothing is modelled the grid's cells would
    hide: the walls stand on the floor (Y 32), and below it only their cut
    ends and the floor pieces' outer faces, which are past the grid."""
    # Walls. The left wall from its faces (a box's would leave edges of
    # hidden faces on the faces seen), with the window's opening through it.
    left_wall(m)
    face(m, "left_wall", [(LX - T, BASE, Z1), (LX, BASE, Z1), (LX, FLOOR, Z1), (LX - T, FLOOR, Z1)], (0, 0, 1))
    m.box("right_wall", (LX, FLOOR, RZ - T), (DOOR_X0, TOP, RZ))
    m.box("right_wall", (DOOR_X1, FLOOR, RZ - T), (X1, TOP, RZ))
    m.box("right_wall", (DOOR_X0, DOOR_TOP, RZ - T), (DOOR_X1, TOP, RZ))
    face(m, "right_wall", [(X1, BASE, RZ - T), (X1, BASE, RZ), (X1, FLOOR, RZ), (X1, FLOOR, RZ - T)], (1, 0, 0))
    m.box("door_post", *DOOR_POST)
    # The dark beyond the doorway, at the wall's outer face.
    face(m, "door_dark", [(DOOR_X0, FLOOR, DOOR_DARK_Z), (DOOR_X1, FLOOR, DOOR_DARK_Z),
                          (DOOR_X1, DOOR_TOP, DOOR_DARK_Z), (DOOR_X0, DOOR_TOP, DOOR_DARK_Z)], (0, 0, 1))
    # The opening's sides and top run on back to it (a seam of 2 px at x
    # 193-194 rows 158-167 fell to the terrain between the wall's outer face
    # and the dark).
    o = RZ - T
    face(m, "right_wall", [(DOOR_X0, FLOOR, DOOR_DARK_Z), (DOOR_X0, FLOOR, o), (DOOR_X0, DOOR_TOP, o),
                           (DOOR_X0, DOOR_TOP, DOOR_DARK_Z)], (1, 0, 0))
    face(m, "right_wall", [(DOOR_X1, FLOOR, DOOR_DARK_Z), (DOOR_X1, FLOOR, o), (DOOR_X1, DOOR_TOP, o),
                           (DOOR_X1, DOOR_TOP, DOOR_DARK_Z)], (-1, 0, 0))
    face(m, "right_wall", [(DOOR_X0, DOOR_TOP, DOOR_DARK_Z), (DOOR_X1, DOOR_TOP, DOOR_DARK_Z),
                           (DOOR_X1, DOOR_TOP, o), (DOOR_X0, DOOR_TOP, o)], (0, -1, 0))
    # The slab's front under the left end of the floor: the grid's cell 0,4
    # (height 2, in front of it, X 0-32, Z 128-160) is not in the art, and
    # would take these pixels at its own top.
    face(m, "slab_front", [(LX, BASE, Z1), (SLAB_X1, BASE, Z1), (SLAB_X1, SLAB_TOP, Z1), (LX, SLAB_TOP, Z1)],
         (0, 0, 1))
    # Floor past the grid: the strip under the right wall and its end, the
    # threshold.
    face(m, "floor_back", [(LX, FLOOR, RZ), (X1, FLOOR, RZ), (X1, FLOOR, GRID_Z0), (LX, FLOOR, GRID_Z0)],
         (0, 1, 0))
    face(m, "floor_back", [(X1, BASE, RZ), (X1, BASE, GRID_Z0), (X1, FLOOR, GRID_Z0), (X1, FLOOR, RZ)], (1, 0, 0))
    face(m, "floor_back", [(DOOR_X0, FLOOR, DOOR_DARK_Z), (DOOR_X1, FLOOR, DOOR_DARK_Z), (DOOR_X1, FLOOR, RZ),
                           (DOOR_X0, FLOOR, RZ)], (0, 1, 0))
    # Cell 5,0 at the floor's height: its top, and its end (+X) and its side
    # over the step (+Z) down to the grid's own cell top, Y 16.
    face(m, "step_block", [(STEP_X0, FLOOR, STEP_Z0), (X1, FLOOR, STEP_Z0), (X1, FLOOR, STEP_Z1),
                           (STEP_X0, FLOOR, STEP_Z1)], (0, 1, 0))
    face(m, "step_block", [(X1, 16.0, STEP_Z0), (X1, 16.0, STEP_Z1), (X1, FLOOR, STEP_Z1), (X1, FLOOR, STEP_Z0)],
         (1, 0, 0))
    face(m, "step_block", [(STEP_X0, 16.0, STEP_Z1), (X1, 16.0, STEP_Z1), (X1, FLOOR, STEP_Z1),
                           (STEP_X0, FLOOR, STEP_Z1)],
         (0, 0, 1))
    # The oven and its flue.
    half_oven(m, "oven", OVEN_C, OVEN_A, OVEN_B, OVEN_WALL, OVEN_CAP, OVEN_LEAN, wall=LX)
    m.box("flue", *FLUE)
    m.box("firewood", *WOOD)
    m.box("crate", *CRATE)
    # The shelf: posts, three boards.
    p = SH_POST
    for (x0, z0) in ((SH_X0, SH_Z1 - p), (SH_X1 - p, SH_Z1 - p), (SH_X1 - p, RZ)):
        m.box("shelf", (x0, FLOOR, z0), (x0 + p, SH_TOP, z0 + p))
    m.box("shelf", (SH_X1 - p, FLOOR, RZ), (SH_X1, SH_TOP, SH_Z1))     # its right side, a panel
    m.box("shelf", (SH_X0, SH_TOP_UNDER, RZ), (SH_X1, SH_TOP, SH_Z1))
    m.box("shelf", (SH_X0, SH_MID[0], RZ), (SH_X1, SH_MID[1], SH_Z1))
    m.box("shelf", (SH_X0, SH_LOW[0], RZ), (SH_X1, SH_LOW[1], SH_Z1))
    vessel(m, "bottle", *BOTTLE, ybase=SH_TOP)
    vessel(m, "vase", *VASE, ybase=SH_TOP)
    # The table: top and four legs.
    (tx0, ty0, tz0), (tx1, ty1, tz1) = TABLE_TOP
    m.box("table", (tx0, ty0, tz0), (tx1, ty1, tz1))
    w, a = TABLE_LEG, TABLE_INSET
    for lx in (tx0 + a, tx1 - a - w):
        for lz in (tz0 + a, tz1 - a - w):
            m.box("table", (lx, FLOOR, lz), (lx + w, ty0, lz + w))
    (cx0, _, cz0), (cx1, cy1, cz1) = CHAIR
    sx0, sy0, sy1 = CHAIR_SEAT
    m.box("chair", (cx0, sy1, cz0), (cx1, cy1, cz1))           # the back-rest
    m.box("chair", (sx0, sy0, cz0), (cx1, sy1, cz1))           # the seat
    for lx in (sx0, cx1 - CHAIR_LEG):
        for lz in (cz0, cz1 - CHAIR_LEG):
            m.box("chair", (lx, FLOOR, lz), (lx + CHAIR_LEG, sy0, lz + CHAIR_LEG))
    m.box("bench", *BENCH)
    # A cream bundle on the bench's right end (x 132-140, y 185-189), about
    # 6 rows over its top (a rough fit).
    m.box("bench", (32.0, 47.0, 80.0), (40.0, 55.0, 88.0))
    sx, sz, sr, sy = STOOL
    m.cyl("stool", (sx, FLOOR, sz), (sx, sy, sz), sr, sides=14)
    window(m)
    return m


def build():
    return build_parts(Model("room77"))


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map77.inc")
    m.write_inc(out, 77, "tools/models/room77.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
