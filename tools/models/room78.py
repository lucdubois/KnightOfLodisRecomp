#!/usr/bin/env python3
"""Map 78: a small wrecked bedroom (flat.png, 480x270, sprites and HUD off).

Two plank-and-plaster walls (the plaster fallen off in patches, wooden
posts and beams on them), a raised wooden floor slab whose front is cut in
an L, a bed with its sheet pulled half off, a dresser with a chest on it in
the back corner, a chair knocked over, two low tables (benches) and a long
board fallen across them, and a board lying on the floor.

Projection (exact): sx = (X - Z)/2 + 176, sy = (X + Z)/4 - Y/2 + 168; a
pixel shows what covers its corner point. Along a screen line the two line
families give, for a point (X, Y, Z):
    sx + 2 sy = X - Y + 512        (an edge along Z, slope -1/2)
    sx - 2 sy = -Z + Y - 160       (an edge along X, slope +1/2)
Every "c" below is such a line measured on the art as the separating line
between the last pixel in and the first out (half-integers: "x + 2r = c").

Depth comes from contact: the floor is the game's grid (height 1, Y 16)
and everything stands on it, on each other, or against a wall.

What the grid does NOT match (the grid is x 0-4, z 0-7; the art's room is
X -40.5..157.5, Z -7.5..190.5 on the same floor height):
  - the strip of floor along the left wall, X -40.5..0, is off the grid:
    part "floor_left" puts it at Y 16;
  - the grid's raised strip (x 0, z 0-3: X 0-32, Z 0-128 at height 2) is
    where the bed is, but the art's bed is X -27..21, Z 31..97 with its deck
    at Y 25, and the rest of the strip is plain floor (the dresser's front,
    the floor right of the bed under the sheet, the floor at the bed's
    foot): part "floor_strip" lays that footprint at Y 16 so the pixels no
    furniture covers do not stand 16 up;
  - the grid's cells x 4 at z 3-7 and z 6-7 lie past the slab's edges: the
    art there is the dark backdrop, which the game will lay on the floor
    (a model cannot remove terrain).

    tools/models/room78.py      writes src/kol_model_map78.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, SHEET, LIGHTPASS  # noqa: E402

FLOOR = 16.0                 # the grid's height 1

# ---------------------------------------------------------------- walls
# Both walls 16 thick: their tops are the slate band, exactly 8 rows tall on
# every column (outer line / inner line: left x + 2r = 327.5 / 343.5, right
# x - 2r = -8.5 / -24.5), and the cut ends 8 px wide (left x 56-63, right
# 256-263, the outline columns 56 and 263 included).
T = 16.0
# The feet: the dark junction line where plaster or board meets the floor
# planks, left x + 2r = 455.5 (pixels (72,191) (74,190) (76,189) (80,187)),
# right x - 2r = -136.5 ((242,189) (244,190) (246,191) (248,192)), on the
# floor's Y 16: LX - 16 + 512 = 455.5, -RZ + 16 - 160 = -136.5.
LX = -40.5                   # the left wall's inner face (X), facing +X
RZ = -7.5                    # the right wall's inner face (Z), facing +Z
# The inner top edges then put both tops at 128 (LX - TOP = 343.5 - 512,
# TOP - RZ = 160 - 24.5): the same height from either wall, and the two
# inner lines meet at the art's corner, x 159.5 (LX - RZ = -33).
TOP = 128.0
# The cut ends: the right end's inner corner is the column between the post
# (x 250-255) and the lit end face (256-263): X1 - RZ = 2 (255.5 - 176). Its
# top edge then runs a row above the slate's (x + 2r = 535.5, the art's
# 537.5). The left end: the end face (dark, x 56-63) against the end post's
# lit face (64-70): LX - Z1 = 2 (63.5 - 176); its top edge x - 2r = -216.5
# is the slate's lower edge with its outline (rows 137-140 at x 58-64).
X1 = 151.5
Z1 = 184.5
# Both end faces run down to the floor slab's bottom (the slab's bottom
# outline continues into theirs: x - 2r = -344.5 left, x + 2r = 663.5 right):
# from Y 0.
END_Y0 = 0.0
# The corner post, x 155-164 (its two faces 5 px each: 10 square), its top
# the slate diamond at rows 92-96.
POST = 10.0
# The posts on the walls stand 4 out of them: the right one, x 199-205 (its
# foot at x - 2r = -140.5: (202-205, 172)); the left one, x 112-118 (foot at
# x + 2r = 459.5, (112-114, 172.5)).
POST_RIGHT = ((41.5, FLOOR, RZ), (51.5, TOP, RZ + 4.0))
POST_LEFT = ((LX, FLOOR, 78.5), (LX + 4.0, TOP, 88.5))

# The window in the left wall: on the inner face its left edge is the dark
# outline at column 86 (edge 86.5: Z 138.5), its lintel the dark line
# x + 2r = 386.5 (2-px runs (86-87, 150) .. (96-97, 145): under it, Y 84),
# its right edge between the jamb (x 95-98, grey-green) and the wall's lit
# arris (x 99): 98.5, Z 114.5. The jamb is the opening's +Z-facing side, 4
# columns wide: the window stands 8 back (X -48.5; its frame's dark line at
# column 94 is the jamb's back edge, 94.5). The sill: the jamb's foot (98.5,
# 167.5) and the sill's front highlight (90-91, 172): Y 38 on the inner
# face, its top a 4-row gold band in front of the window (rows 168-171 at
# x 90). In the window: sky-blue glass only in a band over its bottom rail
# (Y 47.5 up to 53.5-54.5 on the right half, Z 114.5-124; 57.5-62.5 at Z
# 125.5-129.5), boards above it to the lintel (brown and gold, crossed by
# dark diagonals), the rail under it (Y 38-47.5). The glass pane takes the
# window's pixels at rest and lets the window light through.
# The window in the back wall (columns 211-225, rows 139-168: lintel x - 2r =
# -65.5, X 63.5-91.5 on the inner face, the frame 10 back) is shuttered: its
# planks fill it with no sky pixel and no gap, so it is left uncut.
WIN_Z0, WIN_Z1 = 114.5, 138.5
WIN_Y0, WIN_Y1 = 38.0, 84.0
GLASS_X = LX - 8.0

# ---------------------------------------------------------------- floor slab
# The floor is a slab cut in an L; its rim (a lit band, BB yyyy) is part of
# the top, the faces under it 5 rows (10) tall, as the front corner shows
# (column 143: top to row 238, face 239-243; column 144 the same).
#   front (+Z) top edge x - 2r = -334.5 -> ZF = 190.5
#   main +X top edge   x + 2r = 621.5  -> XM = 125.5
#   notch (+Z)         x - 2r = -238.5 -> ZN = 94.5   (the outline at (192,216))
#   protrusion +X      x + 2r = 653.5  -> XP = 157.5
#   bottoms 10 lower: -344.5, 631.5, -248.5, 663.5.
XM, XP, ZN, ZF = 125.5, 157.5, 94.5, 190.5
SLAB = 6.0
# The slab reaches 6 past both walls' ends (the art draws the end faces over
# its corners): its faces stop where the ends begin on screen, x 63.5 and
# 255.5, so the end faces keep those pixels.
SLAB_X0 = -34.5              # (SLAB_X0 - ZF)/2 + 176 = 63.5
SLAB_Z0 = -1.5               # (XP - SLAB_Z0)/2 + 176 = 255.5

# ---------------------------------------------------------------- dresser
# The chest on the dresser, against the right wall (back Z = RZ): its top's
# corners on the outline, left (159,137), back (167-170,133), right
# (180,138), its front-bottom corner where the dresser's top edges meet. The
# dresser: its +X face's top edge the 'hh' line x + 2r = 477-480 ((177,150)
# .. (169,154)), its back right edge x 182-183 (rows 151-160), its top-front
# edge (161-168, 150-153). Fitted to the art's outline (14 pixels off).
DRESSER = ((-19.1, FLOOR, RZ), (8.1, 41.4, 22.7))
CHEST = ((-22.0, 41.4, RZ), (0.8, 57.5, 12.5))   # (its top a row lower, its left outline x 159: the reviewer)

# ---------------------------------------------------------------- bed
# Posts at the four corners (head x 145-149 / 166-171 from rows 145 / 155,
# foot x 115-120 from row 167 and x 136-141, rows 177-189: its foot on the
# floor gives X 22, Z 96); a low slatted deck whose top is the line along
# the left side (x + 2r = 461: (145,158) .. (121,171)) and the right side's
# top edge ((150,180) .. (142,184), x + 2r = 510); a footboard on the foot
# (top x - 2r = -222, (122,172) .. (134,178); bottom -234); a headboard
# between the head posts (top x - 2r = -148, (150,149) .. (164,156)). The
# measured values fitted together on the bed's outline (63 pixels off of
# 1447, mostly 1 px).
BED_DECK = ((-25.1, 21.4, 30.7), (20.9, 25.4, 96.7))   # far-left edge on x + 2r = 461.5 (it took a row of floor at -27.2)
BED_FOOTBOARD = ((-23.0, 21.4, 92.7), (16.7, 32.8, 96.7))
BED_HEADBOARD = ((-23.0, 25.4, 32.2), (16.7, 46.7, 36.2))
BED_POST = 4.2
BED_POSTS = (((-27.2, 30.7), 49.6), ((16.7, 30.7), 49.6), ((-27.2, 92.5), 36.7), ((16.7, 92.5), 36.7))
# The sheet: lying on the deck (its back edge the 'hh' line (143,161) ..
# (161,167): Z 43 on the deck) and pulled over the right side down to the
# floor; its lowest point (160.5,187) and its tip's far corner (172,184)
# land on the floor at X 39-45, Z 51-71. One sheet from the deck's right
# edge to the floor (both sides seen).
CLOTH = ((20.9, 25.9, 39.6), (20.9, 25.9, 75.9), (41.8, FLOOR, 70.8), (44.8, FLOOR, 51.0))

# ---------------------------------------------------------------- chair
# Knocked over against the wall's foot: its seat box (outline (189-192,158)
# .. (202,163), x 201 down to row 173, bottom (190-192,178)) and its two leg
# rails (x 178-186, rows 160-166 and 168-174) taken as one block standing
# on the floor (the rails' gaps are a few pixels of floor). A box turned
# 1.4 degrees: centre (38.9, 10.3), half-sizes 12.7 x 12.5, top 35.1.
CHAIR = (38.9, 10.3, 12.7, 12.5, 0.025, 35.1)

# ---------------------------------------------------------------- tables
# Bench B (front): its top square to the axes, back corner (117,189.5),
# right (135.5,198.5), left (93.5,201.5); legs' feet at (97,212.5),
# (109,218.5), (132,207.5) on the floor. x0, x1, z0, z1, top, slab, leg inset.
BENCH_B = (22.3, 57.3, 135.5, 190.1, 38.0, 9.1, 2.5)
# Bench A (back): turned (its top's left edge runs (95,174) .. (80,189), its
# back edge (97,174) .. (116,177)); its front legs x 81-85 (feet row 198)
# and x 97-101 (row 199) at the top's corners put the top at 35. Its top is
# not a rectangle (its edges ~98 degrees apart; a turned rectangle's back
# and front edges ran 3.9 px a row against the art's 5.5-6.5, and it took
# 58 px of floor shadow and 11 of wall): a parallelogram at Y 35, corners
# back-left, back-right, front-right, front-left; its slab 4.5 thick (the
# art draws ~2 rows).
BENCH_A_TOP = ((-35.3, 126.1), (-6.5, 112.5), (10.5, 159.5), (-18.3, 173.1))
BENCH_A_Y, BENCH_A_SLAB = 35.0, 4.5

# ---------------------------------------------------------------- boards
# The long board: drawn as one straight band (its edges x = r - 86 and
# x = r - 77) from bench A's top (cap at (95-103, 178-181)) over bench B to
# the floor ((142-149, 226-229)). No straight board does that: lying on
# bench A (top 35) it would pass through bench B (top 38). Taken as broken
# in three on the same screen line: up from bench A onto bench B's near
# edge (X 22.3), flat across bench B, down to the floor. Centre line
# points, width 8.2, thickness 6.
PLANK_LONG = ((-15.7, 38.0, 139.3), (22.3, 41.0, 154.1), (57.3, 41.0, 165.8), (106.5, 19.0, 167.5), 8.2, 6.0)
# The board lying on the floor at the front: x 121-169, its top outline the
# row 212, bottom 218 (a horizontal band: it runs along (1, 0, -1)); its
# top face rows 213-215, its side 216-218. cx, cz, half-length, half-width,
# top.
PLANK_FLOOR = (81.6, 144.5, 33.9, 4.6, 23.4)


# ---------------------------------------------------------------- helpers
def solid_poly(m, name, pts, centre, flags=0):
    """A polygon of a solid, its corners turned so it faces away from `centre`."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * (a[i] - centre[i]) for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts, flags)


def rbox(m, name, cx, cz, a, b, ang, y0, y1, flags=0):
    """A box turned about Y: centre (cx, cz), half-length a along
    u = (cos, sin) (in X, Z), half-width b along v = (-sin, cos)."""
    c, s = math.cos(ang), math.sin(ang)

    def P(i, j, y):
        return (cx + i * a * c - j * b * s, y, cz + i * a * s + j * b * c)
    corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    top = [P(i, j, y1) for i, j in corners]
    bot = [P(i, j, y0) for i, j in corners]
    centre = (cx, (y0 + y1) / 2, cz)
    for f in [top, bot[::-1]] + [[bot[k], bot[(k + 1) % 4], top[(k + 1) % 4], top[k]] for k in range(4)]:
        solid_poly(m, name, f, centre, flags)


def beam(m, name, p0, p1, w, t, flags=0):
    """A board along p0 -> p1 (its centre line), w wide across it
    (horizontally) and t thick."""
    d = [p1[i] - p0[i] for i in range(3)]
    L = math.sqrt(sum(x * x for x in d))
    d = [x / L for x in d]
    sl = math.hypot(d[0], d[2]) or 1.0
    side = (-d[2] / sl, 0.0, d[0] / sl)
    up = (d[1] * side[2] - d[2] * side[1], d[2] * side[0] - d[0] * side[2], d[0] * side[1] - d[1] * side[0])
    if up[1] < 0:
        up = tuple(-x for x in up)

    def P(p, i, j):
        return tuple(p[k] + i * w / 2 * side[k] + j * t / 2 * up[k] for k in range(3))
    q = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    A = [P(p0, i, j) for i, j in q]
    B = [P(p1, i, j) for i, j in q]
    centre = tuple((p0[k] + p1[k]) / 2 for k in range(3))
    for k in range(4):
        solid_poly(m, name, [A[k], A[(k + 1) % 4], B[(k + 1) % 4], B[k]], centre, flags)
    solid_poly(m, name, A, centre, flags)
    solid_poly(m, name, B, centre, flags)


def face(m, name, pts, normal, flags=0):
    """A flat polygon turned so its normal is `normal`'s side."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * normal[i] for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts, flags)


# ---------------------------------------------------------------- parts
def wall_left(m):
    """The left wall, X LX - T .. LX, Z RZ - T .. Z1, from its faces (a box's
    would leave edges of hidden faces on the faces seen): its top, its inner
    face (from the corner, Z RZ) and outer face round the window's opening,
    its cut end and back end, the opening's sides and sill (no lintel
    underside: it faces down, no camera of the game sees it)."""
    x0, x1, z0, z1, y0, y1 = LX - T, LX, RZ - T, Z1, FLOOR, TOP
    wz0, wz1, wy0, wy1 = WIN_Z0, WIN_Z1, WIN_Y0, WIN_Y1
    n = "wall_left"
    face(m, n, [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], (0, 1, 0))       # top
    for x, sgn, za in ((x1, 1, RZ), (x0, -1, z0)):      # inner face, outer face
        for (a, b, c, d) in ((za, wz0, y0, y1), (wz0, wz1, y0, wy0), (wz0, wz1, wy1, y1), (wz1, z1, y0, y1)):
            face(m, n, [(x, c, a), (x, d, a), (x, d, b), (x, c, b)], (sgn, 0, 0))
    face(m, n, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1))       # cut end
    face(m, n, [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1))      # back end
    face(m, n, [(x0, wy0, wz0), (x1, wy0, wz0), (x1, wy1, wz0), (x0, wy1, wz0)], (0, 0, 1))    # the jamb seen
    face(m, n, [(x0, wy0, wz1), (x1, wy0, wz1), (x1, wy1, wz1), (x0, wy1, wz1)], (0, 0, -1))
    face(m, n, [(x0, wy0, wz0), (x1, wy0, wz0), (x1, wy0, wz1), (x0, wy0, wz1)], (0, 1, 0))    # sill


def add_window(m):
    """The left window's pane: glass, boards and rail as drawn, light passing."""
    face(m, "window", [(GLASS_X, WIN_Y0, WIN_Z0), (GLASS_X, WIN_Y1, WIN_Z0), (GLASS_X, WIN_Y1, WIN_Z1),
                       (GLASS_X, WIN_Y0, WIN_Z1)], (1, 0, 0), SHEET | LIGHTPASS)


def add_walls(m):
    # The corner block (X LX-T..LX, Z RZ-T..RZ) is the left wall's.
    m.box("wall_right", (LX, FLOOR, RZ - T), (X1, TOP, RZ))
    m.poly("wall_right", [(X1, END_Y0, RZ - T), (X1, FLOOR, RZ - T), (X1, FLOOR, RZ), (X1, END_Y0, RZ)])
    wall_left(m)
    m.poly("wall_left", [(LX - T, END_Y0, Z1), (LX, END_Y0, Z1), (LX, FLOOR, Z1), (LX - T, FLOOR, Z1)])
    m.box("corner_post", (LX, FLOOR, RZ), (LX + POST, TOP, RZ + POST))
    m.box("post_right", *POST_RIGHT)
    m.box("post_left", *POST_LEFT)


def add_floor(m):
    e = "floor_edge"
    m.poly(e, [(SLAB_X0, SLAB, ZF), (XM, SLAB, ZF), (XM, FLOOR, ZF), (SLAB_X0, FLOOR, ZF)])
    m.poly(e, [(XM, SLAB, ZF), (XM, SLAB, ZN), (XM, FLOOR, ZN), (XM, FLOOR, ZF)])
    m.poly(e, [(XM, SLAB, ZN), (XP, SLAB, ZN), (XP, FLOOR, ZN), (XM, FLOOR, ZN)])
    m.poly(e, [(XP, SLAB, ZN), (XP, SLAB, SLAB_Z0), (XP, FLOOR, SLAB_Z0), (XP, FLOOR, ZN)])
    # Off the grid along the left wall (its corner by the wall's end left
    # to the end face, as the art draws it).
    m.poly("floor_left", [(LX, FLOOR, RZ), (LX, FLOOR, Z1), (SLAB_X0, FLOOR, ZF), (0.0, FLOOR, ZF), (0.0, FLOOR, RZ)])
    # Over the grid's raised strip (x 0, z 0-3): the art's floor there.
    m.poly("floor_strip", [(0.0, FLOOR, 0.0), (0.0, FLOOR, 128.0), (32.0, FLOOR, 128.0), (32.0, FLOOR, 0.0)])
    # Off the grid along the right wall too (Z RZ..0): left to the off-field
    # height 0, its 154 pixels along the wall's foot stretched into a curtain
    # of streaks with the camera moved (the reviewer). To X1: XP took 4 px
    # of the wall's end face.
    m.poly("floor_left", [(0.0, FLOOR, RZ), (0.0, FLOOR, 0.0), (X1, FLOOR, 0.0), (X1, FLOOR, RZ)])



def add_dresser(m):
    m.box("dresser", *DRESSER)
    m.box("chest", *CHEST)


def add_bed(m):
    m.box("bed_frame", *BED_DECK)
    m.box("bed_frame", *BED_FOOTBOARD)
    m.box("headboard", *BED_HEADBOARD)
    for (x, z), y in BED_POSTS:
        m.box("bed_posts", (x, FLOOR, z), (x + BED_POST, y, z + BED_POST))
    m.poly("cloth", list(CLOTH), SHEET)


def add_chair(m):
    cx, cz, a, b, ang, yt = CHAIR
    rbox(m, "chair", cx, cz, a, b, ang, FLOOR, yt)


def add_bench_a(m):
    yt, yb = BENCH_A_Y, BENCH_A_Y - BENCH_A_SLAB
    c = BENCH_A_TOP
    cen = (sum(p[0] for p in c) / 4, (yt + yb) / 2, sum(p[1] for p in c) / 4)
    top = [(x, yt, z) for x, z in c]
    bot = [(x, yb, z) for x, z in c]
    solid_poly(m, "bench_a", top, cen)
    for k in range(4):
        solid_poly(m, "bench_a", [bot[k], bot[(k + 1) % 4], top[(k + 1) % 4], top[k]], cen)
    # Its legs 4 x 4, 4 in from each corner toward the middle.
    for x, z in c:
        dx, dz = cen[0] - x, cen[2] - z
        n = math.hypot(dx, dz)
        rbox(m, "bench_a", x + dx / n * 4.0, z + dz / n * 4.0, 2, 2, -0.42, FLOOR, yb)


def add_bench_b(m):
    x0, x1, z0, z1, yt, th, e = BENCH_B
    m.box("bench_b", (x0, yt - th, z0), (x1, yt, z1))
    for x, z in ((x0 + e, z0 + e), (x1 - e - 4, z0 + e), (x0 + e, z1 - e - 4), (x1 - e - 4, z1 - e - 4)):
        m.box("bench_b", (x, FLOOR, z), (x + 4, yt - th, z + 4))


def add_boards(m):
    pts, w, t = PLANK_LONG[:-2], PLANK_LONG[-2], PLANK_LONG[-1]
    for a, b in zip(pts, pts[1:]):
        beam(m, "plank_long", a, b, w, t)
    cx, cz, a, b, yt = PLANK_FLOOR
    rbox(m, "plank_floor", cx, cz, a, b, -math.pi / 4, FLOOR, yt)


def build_parts(m):
    add_walls(m)
    add_floor(m)
    add_dresser(m)
    add_bed(m)
    add_chair(m)
    add_bench_a(m)
    add_bench_b(m)
    add_boards(m)
    add_window(m)


def build():
    m = Model("room78")
    build_parts(m)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map78.inc")
    m.write_inc(out, 78, "tools/models/room78.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
