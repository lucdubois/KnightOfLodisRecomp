#!/usr/bin/env python3
"""Map 59, a small half-timbered bedroom: two walls with a lit stone-and-timber
pattern (all paint), a real window through the left wall, a bed, a tall
bookcase, a chest beside it, a square table with a chair, a basket, a plant in
the corner, a bowl of yarn at the bookcase's foot and a ball of green yarn on the floor, a grey
rug (paint on the floor). Measured on shots/map59/view.png (480x270, sprites and
HUD hidden), projection sx = (X - Z)/2 + 128, sy = (X + Z)/4 - Y/2 + 151.

The game samples pixel (px, py) at its corner: edges lie between the last pixel
in and the first out. Lines are constants of s = x + 2y (a line along Z:
s = X - Y + 430) or d = x - 2y (a line along X: d = Y - Z - 174). The art comes
in 2x2 chunks here, so every measured profile steps by pairs of s or d (a pair
(444, 445), then (446, 447)): edges are put at odd + 0.5, the pair boundary.
Mean colours along those lines over a stretch free of furniture give the edges
below (raster59.py checks them, prof.py measured them).

The grid is NOT flat: the row z 0 and column x 0 are height 10 (Y 160, the
walls), cell 4,1 height 3 (Y 48, the chest), 5,1 and 6,1 height 7 (Y 112, the
bookcase), 2,4 height 2 (Y 32, the table); the other cells of x 1-7, z 1-5 are
the floor, Y 16, which is not modelled except for floor_patch (below). The art's
walls are a little off the grid's, so nothing is taken from it: the grid's walls
fall inside or behind the model's.

    room59.py OUT.inc          writes the table (the repo's would be
                               src/kol_model_map59.inc)
"""
import math
import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
if not os.path.exists(os.path.join(_here, "model.py")):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, SHEET, LIGHTPASS, NOSHADOW, ACTOR, ASDRAWN  # noqa: E402

FLOOR = 16.0        # the grid's height 1

# ---------------------------------------------------------------- walls
# The silhouette, per column on flat.layers: outer top edges s = 251.5 (first in
# at s 252/253) and d = 3.5 (last in at d 2/3), peak (127.5, 62); columns 40-247.
# The foot lines, from mean colours along s and d over bare wall: left, dark
# junction pair s 444/445 then wood from s 448 (x 56-66); right, the same pair
# d -190/-189 (x 224-231). So LX = 445.5 - 430 + 16 and RZ = 174 + 16 + 190.5 - 348,
# which puts both inner faces on the grid's cell edge, 32 give or take 0.5.
LX = 31.5           # left wall's inner face, facing +X
RZ = 32.5           # right wall's inner face, facing +Z
# The cut ends are 16 columns wide (left x 40-55, right 232-247 with outline
# columns): the walls are 32 thick. With s = 251.5 and d = 3.5 that fixes the top:
# LX - 32 - TOP + 430 = 251.5 and TOP - (RZ - 32) - 174 = 3.5 both give 178.
# The slate band reads 34 wide (last slate pixels s 284/285, d -30/-29): its inner
# 2 is the dark outline pair, which stays on the wall's face here.
T = 32.0
TOP = 178.0
X0, Z0 = LX - T, RZ - T     # -0.5, 0.5
# Cut ends: left inner edge x 55.5 -> LX - ZE = -145; right inner edge x 231.5 ->
# XE - RZ = 207. They run down to the silhouette's lower edge: right s = 669.5 on
# XE -> Y 0 (the same on the left: d = -350.5 on ZE).
ZE = 176.5
XE = 239.5
END_Y0 = 0.0

# ---------------------------------------------------------------- floor slab
# The floor's rim is a stone face under the planks, as on map 78: by d over x
# 60-150 wood down to d -340 and outline at -350/-349, by s over x 165-228 dark
# from s 660 and outline at 668/669. Five pairs, 10 tall: top edges d = -340.5
# (Z 182.5) and s = 659.5 (X 245.5) on Y 16, bottoms d = -350.5 and s = 669.5, Y 6.
# The slab reaches 6 past the walls' cut ends; its faces stop where the ends begin
# on screen (x 55.5 and 231.5), so the end faces keep those columns. The grid's
# floor goes on to X 256, Z 192 on Y 16, whose edge is this silhouette's lower edge
# too: it adds nothing the slab's faces do not take.
XF, ZF = 245.5, 182.5
SLAB = 6.0
SLAB_X0 = 2.0 * (55.5 - 128.0) + ZF     # 37.5
SLAB_Z0 = XF - 2.0 * (231.5 - 128.0)    # 38.5

# ---------------------------------------------------------------- window
# A real opening through the left wall, seen from the room. By x over rows 136-149:
# x 79 wood, x 80-81 the frame's blue edge, sky 82-95, white frame 96-97, then the
# lit jamb 98-105 (yellow, gold, tan), stone from x 106. So the inner face's far edge
# is x 105.5 (WIN_Z0) and the jamb's back edge x 95.5 (GLASS_X, 20 behind the inner
# face); the near edge x 80.5 (WIN_Z1; the near reveal faces away): a pixel into the blue
# edge, so what a camera moved right stretches across the gap is sky, not that edge.
# By s over x 82-95: dark outline 346-349, sky from 350 (lintel s = 349.5 on the
# inner face: WIN_Y1 112), blue ends about s 393 (sill's back on the glass), white
# 398-411 then yellow to 413 (sill's front s = 413.5: WIN_Y0 48). 20 of sill, as
# the jamb says. The lit jamb's top is not the lintel: dark above it along
# d = -153.5 (x 97-106, rows 118-136), Y 97 on WIN_Z0, meeting the lintel line at
# (98, 125.75): the art's corner is a 45 degree brace on the wall's plane, so the
# opening is cut with one (BRACE_Y). The yellow band running on under the window to
# the corner (s 414-427, Y 34-48 on the wall) is the wall's lower rail lit by the
# painted light, not a shelf: it has no end cut and no foot.
WIN_Z0, WIN_Z1 = 76.5, 126.5
WIN_Y0, WIN_Y1 = 48.0, 112.0
BRACE_Y = 97.0
GLASS_X = LX - 20.0     # 11.5

# ---------------------------------------------------------------- bed
# A box under the white cover, its head short of the wall. The cover's upper-left
# edge s = 459.5 ((116,172) .. (140,160)), its upper-right edge d = -177.5, chosen by count (the art rounds it, (145,160)
# .. (150,165)); the side falls to the floor on s = 517.5 (white to 515, by s over
# x 136-150, shadow 516-519): X 103.5, and with it to Y 38 (no crease in the white
# to read: the board below fixes it).
# Two boards of one width, X 59 .. 103.5, the near one's right end on the cover's side.
# The headboard at the wall: a block from the wall to Z 38, its front's bright top rail
# (138,148) .. (160,159) is Y 54.5, its left end x 138.5, its foot the dark line
# (156,176) .. (165,178), and the navy column x 162-163, rows 161-175, is its +X side.
# The board at the foot: top d = -232.5 (white to -231, by d over x 112-127), foot
# d = -254.5, front x 109.25 .. 131.5 with its dark +X side x 131.5-133.5, rows 180-192
# (the white beside it, from x 134, is the cover falling, not the board).
BED_X0, BED_X1, BED_TOP = 67.5, 103.5, 38.0
BED_Z0, BED_Z1 = 41.5, 92.5
BED_HEAD_X, BED_HEAD_Z = 89.5, 55.5
BOARD = ((59.0, FLOOR, 92.5), (103.5, 38.0, 96.5))
BED_ROUND = 7.0   # map 60's rounded edge
HEADBOARD = ((59.0, FLOOR, RZ), (103.5, 54.5, 38.0))

# ---------------------------------------------------------------- bookcase
# Stands on the grid's two tall cells, against the right wall. +X foot s = 637.5
# (navy to 637, wood from 638, x 212-224): X 223.5. +Z foot d = -216.5 (dark foot
# line -216..-213, wood from -217, x 177-190): Z 58.5. Left edge x 179.5 (outline
# column 180): X 161.5.
# One flat top, no step: the rhombus (179.5,147), (189.5,142), (223.5,159), (213.5,164),
# olive at the left going to navy. It sits on a cornice a little wider than the case:
# Y 118, front on the case's front (the bright band on d = -114.5); +X side overhanging to
# X 229.5 (its band runs s = 541.5, its corner at x 213 past the case's at 210.5); back
# edge Z 38.5 (the stone on the wall ends at d -94). The upper-left edge widens toward
# the back: X 161.5 at the front to 157.25 at the back (s = 469.25, (182,143) .. (187,141)).
CASE = ((161.5, FLOOR, RZ), (223.5, 112.0, 58.5))
CORNICE_Y0, CORNICE_Y1 = 112.0, 118.0
CORNICE = ((161.5, 58.5), (229.5, 58.5), (229.5, 38.5), (157.25, 38.5))

# ---------------------------------------------------------------- chest
# On the grid's cell 4,1, between the bed and the bookcase, height the grid's Y 48.
# Foot d = -209.5 (dark pairs (168-171, 188) and (172-175, 190)): Z 51.5. It starts at
# the cell's edge, X 128, Z 38, its near top corner cut to BG1's sloping edge (x 166 row
# 178 to x 174 row 169): to the left of that are the wall's post and a stone panel,
# which run on down behind it. It runs on under the bookcase's side. The red and white
# things on its front and top are left as paint.
CHEST = ((128.0, FLOOR, 38.0), (161.5, 48.0, 51.5))
CHEST_CUT = ((128.0, 34.0, 51.5), (142.0, 48.0, 51.5), (128.0, 48.0, 38.0))

# ---------------------------------------------------------------- bowl of yarn
# A round wooden bowl on the floor, touching the bookcase's front; the red yarn heaped in
# it, x 192-203, rows 198-207, crosses the bookcase's foot line because it stands up in
# front of the case. Rim: the tan band rows 208-211, x 190-209 (r 13.4, centre x 199.5,
# rim's middle row 206.5: Y 31.4). Foot: its lowest row 217 (r 8, middle row 214.2, on
# the floor): X 213.9, Z 70.9. The heap sits left of the bowl's middle (centre x 197.5)
# and reaches past the red to the art's dark outline, x 190-205, row 196.5: what a
# moved camera stretches off its edge is then the outline, not a streak of red.
BOWL = (213.9, 70.9)
YARN = (211.9, 72.9)
BOWL_RINGS = ((8.0, FLOOR), (11.0, 20.0), (13.4, 26.0), (13.4, 31.4), (10.6, 31.4))
YARN_RINGS = ((10.6, 31.4), (10.3, 38.0), (8.2, 44.5), (5.0, 49.0), (0.5, 51.4))


def lathe(m, name, centre, rings, sides=16):
    """A round body from (radius, y) rings, from the bottom up."""
    cx, cz = centre
    rows = []
    for r, y in rings:
        rows.append([(cx + r * math.cos(-2 * math.pi * j / sides), y, cz + r * math.sin(-2 * math.pi * j / sides))
                     for j in range(sides + 1)])
    m.strip(name, rows)


# ---------------------------------------------------------------- Alphonse in bed
# Not the map's art: what the head and body of his sprite stand on while he lies in the
# bed (ACTOR, kol_focus kSceneObjects; states/thor/0014, then A: he sits up). The sprite's
# lower pieces repaint the cover and stay on it; the fold they end at, under his body,
# runs d = -198.5 ((230,134.5) .. (241,139.5) on that state's frame, 96 right and 32 up of
# view.png's): Z 62.5 on the cover, X 94.5 under his middle (x 144 on view.png). From
# there the surface faces the view across the bed, as a unit's card does (along the bed it
# went edge-on from one side and wide from the other), and leans back like someone propped
# on a pillow, up past the sitting frame's head (row 120 there). Wider than he is: only his
# own pixels go to it. It stops no light (an empty bed has nothing there) and takes none of
# the room's shade: his colours stay the sprite's.
BODY_FOOT = (94.5, 38.0, 62.5)
BODY_TOP_Y = 100.0
BODY_LEAN = 0.35    # X and Z back for each 1 of Y
BODY_HALF = 40.0

# ---------------------------------------------------------------- ball of yarn
# Green, on the floor in front of the bowl. The ball is the bright blob and its dark
# edge: centre (204, 215.5), x 199.5-208.5, rows 211.5-219.5, R 6.4, touching the floor.
# The darker green under it, x 196-204, rows 218-223, is the loose strand: floor paint.
BALL = (227.4, 22.4, 75.4, 6.4)

# ---------------------------------------------------------------- table
# Square, a round pale cloth on it. The table is on BG1 and BG1's top outline is the
# top's back corners exactly, a diamond's upper half: back (95.5, 181), left x 80 at
# row 188, right x 111 at row 188; edges s = 455.5 and d = -264.5. 32 x 32 as its grid
# cell, the cell's own place shifted 2.5: X 61.5-93.5, Z 126.5-158.5, Y 36 (front
# corner row 196, its leg's foot row 206 at x 95.5). The art's rim to row 197 is 1.5
# of thickness. Legs inset from feet (83, 200), (96, 206), (105, 199).
TABLE = ((61.5, 33.0, 126.5), (93.5, 36.0, 158.5))
TABLE_LEGS = ((67.25, 132.25), (67.25, 156.25), (91.25, 132.25), (91.25, 156.25))
TABLE_LEG = 1.5

# ---------------------------------------------------------------- chair
# Beside the left wall, its back to it, seat toward the table. The back's left post is
# the dark column x 66-69, rows 170-191 (foot (66, 192): X 35, Z 160; top s = 404.5,
# row 169.5); its right edge x 82.75, seat under the table's rim. The back runs to the
# floor (under the seat the art is dark, gaps between legs: a closed back keeps the
# hollow from streaking).
BACK = ((35.0, FLOOR, 129.5), (39.0, 60.5, 160.0))
SEAT = ((39.0, 31.0, 129.5), (62.0, 34.25, 160.0))
CHAIR_LEG = 3.0

# ---------------------------------------------------------------- basket
# A pale round rim on the wall's foot, x 104-120, rows 158-170, lit yellow inside. Its
# width gives r 12, and a free-standing drum that wide would have to stand inside the
# wall to end at row 172, so it is cut at the wall: centre X 34, Z 66, Y to 27.6.
BASKET = (34.0, 66.0, 12.0, 27.6)

# ---------------------------------------------------------------- plant
# Red and green at x 123-135, rows 140-155, in the back corner over the bed's head.
# Nothing under it holds it in the art (the wall's rail goes on behind), so it hangs
# in the corner: a quarter of a drum centred on the walls' meeting, touching both at
# its tips (a whole drum stood off the walls: green wedges beside it in moved views).
PLANT = (LX, RZ, 16.0, 35.0, 54.0)

# ---------------------------------------------------------------- floor patches
# The grid's furniture cells stand wider than the art's furniture: where the art
# shows floor the terrain's block would keep the pixel at the block's face. A sheet at
# the floor's height takes them back (the rays through a block start up to its height
# back of it on the floor). Under the table's and chest's blocks and in front of the
# bookcase's; under the bed and chair too, which cover them.
PATCHES = ((46.0, 96.0, 110.0, 160.0), (96.0, 161.5, RZ, 64.0), (161.5, 224.0, 58.5, 64.0))


def face(m, name, pts, normal, flags=0):
    """A flat polygon turned so its normal is `normal`'s side."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * normal[i] for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts, flags)


def left_wall(m, shut=False):
    """Top, inner and outer faces round the opening (no box: a box would leave
    the edges of hidden faces on the faces seen), its cut end down to Y 0.
    `shut`: no opening (map 60: its shutters are paint on the wall's plane)."""
    y0, y1 = FLOOR, TOP
    if shut:
        face(m, "left_wall", [(X0, y1, Z0), (X0, y1, ZE), (LX, y1, ZE), (LX, y1, Z0)], (0, 1, 0))
        for x, n, za in ((LX, 1, RZ), (X0, -1, Z0)):
            face(m, "left_wall", [(x, y0, za), (x, y1, za), (x, y1, ZE), (x, y0, ZE)], (n, 0, 0))
        face(m, "left_wall", [(X0, END_Y0, ZE), (LX, END_Y0, ZE), (LX, y1, ZE), (X0, y1, ZE)], (0, 0, 1))
        return
    wz0, wz1, wy0, wy1 = WIN_Z0, WIN_Z1, WIN_Y0, WIN_Y1
    bz = wz0 + (wy1 - BRACE_Y)      # where the brace meets the lintel
    face(m, "left_wall", [(X0, y1, Z0), (X0, y1, ZE), (LX, y1, ZE), (LX, y1, Z0)], (0, 1, 0))
    for x, n, za in ((LX, 1, RZ), (X0, -1, Z0)):
        face(m, "left_wall", [(x, y0, za), (x, y1, za), (x, y1, wz0), (x, y0, wz0)], (n, 0, 0))
        face(m, "left_wall", [(x, y0, wz0), (x, wy0, wz0), (x, wy0, wz1), (x, y0, wz1)], (n, 0, 0))
        face(m, "left_wall", [(x, BRACE_Y, wz0), (x, y1, wz0), (x, y1, bz), (x, wy1, bz)], (n, 0, 0))
        face(m, "left_wall", [(x, wy1, bz), (x, y1, bz), (x, y1, wz1), (x, wy1, wz1)], (n, 0, 0))
        face(m, "left_wall", [(x, y0, wz1), (x, y1, wz1), (x, y1, ZE), (x, y0, ZE)], (n, 0, 0))
    face(m, "left_wall", [(X0, END_Y0, ZE), (LX, END_Y0, ZE), (LX, y1, ZE), (X0, y1, ZE)], (0, 0, 1))


def right_wall(m):
    y0, y1 = FLOOR, TOP
    face(m, "right_wall", [(LX, y1, Z0), (LX, y1, RZ), (XE, y1, RZ), (XE, y1, Z0)], (0, 1, 0))
    face(m, "right_wall", [(LX, y0, RZ), (XE, y0, RZ), (XE, y1, RZ), (LX, y1, RZ)], (0, 0, 1))
    face(m, "right_wall", [(XE, END_Y0, Z0), (XE, END_Y0, RZ), (XE, y1, RZ), (XE, y1, Z0)], (1, 0, 0))
    face(m, "right_wall", [(X0, y0, Z0), (XE, y0, Z0), (XE, y1, Z0), (X0, y1, Z0)], (0, 0, -1))


def window(m):
    """The opening's far jamb and sill, the near jamb (faces away, for the
    light), and the glass, through which the light passes. No lintel underside:
    it faces down, as on map 77."""
    wz0, wz1, wy0, wy1 = WIN_Z0, WIN_Z1, WIN_Y0, WIN_Y1
    face(m, "window_reveal", [(X0, wy0, wz0), (LX, wy0, wz0), (LX, BRACE_Y, wz0), (X0, BRACE_Y, wz0)], (0, 0, 1))
    face(m, "window_reveal", [(X0, wy0, wz1), (LX, wy0, wz1), (LX, wy1, wz1), (X0, wy1, wz1)], (0, 0, -1))
    face(m, "window_reveal", [(X0, wy0, wz0), (LX, wy0, wz0), (LX, wy0, wz1), (X0, wy0, wz1)], (0, 1, 0))
    face(m, "window", [(GLASS_X, wy0, wz0), (GLASS_X, wy1, wz0), (GLASS_X, wy1, wz1), (GLASS_X, wy0, wz1)],
         (1, 0, 0), SHEET | LIGHTPASS)


def slab(m):
    """The rim's two stone faces, past the planks' edge. (They stop no occlusion: nothing
    stands at the open front, but their top edge read as a wall to the floor beside it.)"""
    face(m, "slab", [(SLAB_X0, SLAB, ZF), (XF, SLAB, ZF), (XF, FLOOR, ZF), (SLAB_X0, FLOOR, ZF)], (0, 0, 1), NOSHADOW)
    face(m, "slab", [(XF, SLAB, SLAB_Z0), (XF, SLAB, ZF), (XF, FLOOR, ZF), (XF, FLOOR, SLAB_Z0)], (1, 0, 0), NOSHADOW)


def wall_drum(m, name, cx, cz, r, y0, y1, wall_x, n=24):
    """An upright round drum cut at the wall X = wall_x: its side and its top."""
    q = (wall_x - cx) / r
    tm = math.acos(max(-1.0, min(1.0, q)))
    arc = [tm * (1 - 2 * j / n) for j in range(n + 1)]
    pts = [(cx + r * math.cos(t), cz + r * math.sin(t)) for t in arc]
    m.strip(name, [[(x, y0, z) for x, z in pts], [(x, y1, z) for x, z in pts]])
    m.poly(name, [(x, y1, z) for x, z in pts])


def solid(m, name, faces):
    """A closed convex solid from its faces, each turned to face outward."""
    pts = [p for f in faces for p in f]
    c = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
    for f in faces:
        a, b, d = f[0], f[1], f[2]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (d[0] - a[0], d[1] - a[1], d[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        fc = tuple(sum(p[i] for p in f) / len(f) for i in range(3))
        out = sum(n[i] * (fc[i] - c[i]) for i in range(3))
        m.poly(name, list(f) if out > 0 else list(reversed(f)))


def prism(m, name, plan, y0, y1):
    """An upright prism over a convex plan of (x, z) corners in order."""
    faces = [[(x, y0, z) for x, z in plan], [(x, y1, z) for x, z in plan]]
    for i, (x, z) in enumerate(plan):
        x2, z2 = plan[(i + 1) % len(plan)]
        faces.append([(x, y0, z), (x2, y0, z2), (x2, y1, z2), (x, y1, z)])
    solid(m, name, faces)


def build_parts(m, night=False, yarn=True):
    """The room's parts, one name per object. `night`: map 60, the same cells and
    art by lantern: its window shut, no yarn, no one in the bed, a lantern on the table.
    `yarn` False: map 61, no bowl of yarn in front of the bookcase."""
    left_wall(m, shut=night)
    right_wall(m)
    if not night:
        window(m)
    slab(m)
    # The bed: the box under the cover, the boards at its head and foot.
    # (The cover's head corner is cut: its true edge is the upright line x 152, rows
    # 165-183; beyond that is a stone panel of the wall and wood.)
    if night:
        # By lantern the cover's long side faces away from the light: the same bed with
        # that edge rounded, so the light rolls off it (a square corner lit one side only
        # was a hard line along the bed; the user found the bed's edges harsh).
        r = BED_ROUND
        xt = BED_X1 - r
        zc = xt - (BED_HEAD_X - BED_Z0)     # where the head's cut meets the top's new edge
        prism(m, "bed", [(BED_X0, BED_Z0), (BED_HEAD_X, BED_Z0), (xt, zc), (xt, BED_Z1), (BED_X0, BED_Z1)], FLOOR, BED_TOP)
        rows = [[(BED_X1, FLOOR, z) for z in (BED_Z1, BED_HEAD_Z)]]
        for i in range(6):
            t = math.pi / 2 * i / 5
            x = xt + r * math.cos(t)
            rows.append([(x, BED_TOP - r + r * math.sin(t), z) for z in (BED_Z1, x - (BED_HEAD_X - BED_Z0))])
        m.strip("bed", rows)
    else:
        prism(m, "bed", [(BED_X0, BED_Z0), (BED_HEAD_X, BED_Z0), (BED_X1, BED_HEAD_Z), (BED_X1, BED_Z1), (BED_X0, BED_Z1)],
              FLOOR, BED_TOP)
    m.box("bed", *BOARD)
    m.box("bed", *HEADBOARD)
    m.box("bookcase", *CASE)
    prism(m, "bookcase", CORNICE, CORNICE_Y0, CORNICE_Y1)
    (cx0, cy0, cz0), (cx1, cy1, cz1) = CHEST
    ca, cb, cc = CHEST_CUT
    solid(m, "chest", [
        [(cx0, cy0, cz0), (cx1, cy0, cz0), (cx1, cy0, cz1), (cx0, cy0, cz1)],   # under
        [(cx0, cy0, cz0), (cx1, cy0, cz0), (cx1, cy1, cz0), cc],                # back
        [(cx1, cy0, cz0), (cx1, cy0, cz1), (cx1, cy1, cz1), (cx1, cy1, cz0)],   # right
        [(cx0, cy0, cz1), (cx1, cy0, cz1), (cx1, cy1, cz1), cb, ca],            # front
        [(cx0, cy0, cz0), (cx0, cy0, cz1), ca, cc],                             # left
        [cc, cb, (cx1, cy1, cz1), (cx1, cy1, cz0)],                             # top
        [ca, cb, cc],                                                           # the cut
    ])
    if not night:
        fx, fy, fz = BODY_FOOT
        back = BODY_LEAN * (BODY_TOP_Y - fy)
        h = BODY_HALF
        face(m, "alphonse_in_bed", [(fx - h, fy, fz + h), (fx + h, fy, fz - h),
                                    (fx + h - back, BODY_TOP_Y, fz - h - back), (fx - h - back, BODY_TOP_Y, fz + h - back)],
             (1, 1, 1), ACTOR | NOSHADOW | ASDRAWN)
    if not night and yarn:
        lathe(m, "bowl", BOWL, BOWL_RINGS)
        lathe(m, "yarn", YARN, YARN_RINGS)
        bx, by, bz, br = BALL
        lathe(m, "yarn_ball", (bx, bz), [(max(0.2, br * math.cos(t)), by + br * math.sin(t))
                                         for t in [math.pi * (i / 6 - 0.5) for i in range(7)]], sides=12)
    m.box("table", *TABLE)
    w = TABLE_LEG
    for lx, lz in TABLE_LEGS:
        m.box("table", (lx - w, FLOOR, lz - w), (lx + w, TABLE[0][1], lz + w))
    m.box("chair", *BACK)
    m.box("chair", *SEAT)
    (sx0, sy0, sz0), (sx1, _, sz1) = SEAT
    for lz in (sz0, sz1 - CHAIR_LEG):
        m.box("chair", (sx1 - CHAIR_LEG, FLOOR, lz), (sx1, sy0, lz + CHAIR_LEG))
    kx, kz, kr, ky = BASKET
    wall_drum(m, "basket", kx, kz, kr, FLOOR, ky, LX)
    px, pz, pr, py0, py1 = PLANT
    arc = [(px + pr * math.cos(i * math.pi / 16), pz + pr * math.sin(i * math.pi / 16)) for i in range(9)]
    prism(m, "plant", [(px, pz)] + arc, py0, py1)
    for (x0, x1, z0, z1) in PATCHES:
        face(m, "floor_patch", [(x0, FLOOR, z0), (x0, FLOOR, z1), (x1, FLOOR, z1), (x1, FLOOR, z0)], (0, 1, 0), SHEET)
    return m


def build():
    return build_parts(Model("room59"))


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map59.inc")
    m.write_inc(out, 59, "tools/models/room59.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
