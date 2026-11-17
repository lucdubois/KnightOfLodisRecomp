#!/usr/bin/env python3
"""Map 91, the throne hall, as a 3-D model for the perspective warp.

All numbers are measured on flat.png (640x350, the three views' mosaic) with
its projection sx = (X - Z)/2 + 368, sy = (X + Z)/4 - Y/2 + 111. "First /
last pixel" means the pixels the art draws for a thing (its dark outline
included); a model edge belongs half a pixel outside them. The game samples
a pixel at its corner, so a pixel at (px, py) is the point (px, py).

The hall. The game's grid is the floor only (7 x 13 cells at height 2,
Y 32; the user added x 7-8, z 13 and a few cells at 2). The art draws:
- along the left (X < 0) a colonnade: five round columns on flared bases at
  X -48, carrying a flat entablature and a wall with blind arches (all
  relief on one face, X -47) up to a cornice; its cut top is drawn as the
  dark purple band, 62 deep. Through the openings under the lintel a dark
  aisle whose floor runs back ~100 units (its tiles show to X ~ -150);
- across the back (Z -65.5) a wall, turning at the right into the hall's
  right side (X 302, the dark fluted face on the floor slab's edge), both
  cut at Y 320 (the purple top at the top right);
- in front of the back wall a canopy (baldachin) over a dais: two front
  columns, a box top, red curtains between the columns (parted in a
  triangle), down the right side, and a dark inner curtain behind; the
  carpet climbs two steps of 16 up to the dais (Y 64);
- right of the canopy an arched alcove with the banner, two small steps
  in it;
- two gargoyles on square pedestals: one on cell (0,2), one on (7,1);
- the floor: a long pool (water, at floor level: no rim or inner wall is
  drawn) with the carpet over its back end; the floor slab's cut faces at
  Z 448 (front) and X 302 (right), 16 px = 32 tall.

    tools/models/room91.py      writes src/kol_model_map91.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, NOSHADOW, SHEET  # noqa: E402

# The gargoyles' two planes stop no lamp's light (NOSHADOW): the half on the X plane lies
# behind the half on the Z plane as the chandeliers see it, and stood in its shadow from the
# fold on, a straight vertical line down the right one's body (the user saw the
# shadow split vertically on the right statue). Their pedestals still cast.
STATUE_FLAGS = NOSHADOW
import os as _os
FOLD_ROUND = float(_os.environ.get("FOLD_ROUND", 40.0))   # units along each plane over which the fold turns
FOLD_TURN = float(_os.environ.get("FOLD_TURN", 12.0))     # degrees from +Z the far half ends at

KX, KY = 368.0, 111.0   # flat.png's projection offsets
FLOOR = 32.0            # the grid's height 2

# --- The colonnade -------------------------------------------------------
# Shafts: outline pixels 177/190 (col 1, rows 125-155), 217/230, 257/270,
# 297/310, 337(hidden)/350: centres sx 183.5 + 40k, 14 px wide (r 9.9).
# Base: a flared foot, 172..197 wide at row 163 (r 17), its lowest outline
# pixel row 169 at x 184 (col 3: 129, col 4: 110): centre (X+Z)/4 = sy - 6
# -> X -48. So columns at X -48, Z 321 - 80k (80 apart: 40 px).
COL_X = -48.0
COL_ZS = (321.0, 241.0, 161.0, 81.0, 1.0)
COL_PROFILE = [(FLOOR, 17.0), (35.0, 17.0), (45.0, 11.0), (46.0, 9.9), (136.0, 9.9)]
# The capital flares to r 12 at Y 147.25 (its rim's top edge, sy = 156.5 -
# Y/2 at x 230, covers the capital's pixels to the lintel line: at 145.5 it
# left 2 a column, e.g. (230,83) (231,84), to the aisle's back wall; at 147
# exactly it would run through those pixels' corners), then closes in to r 0.5 at
# the lintel (147.5), its last ring behind the wall's face: its top is no flat
# cap in front of the face (a disc at 147.5, r 12, stood 11 out of it and
# took ~50 pixels a column, cols 1-5 rows 103-109 .. 23-29, as a top where
# the art draws the capital's upright front; the lathe's normals are its
# rings', so the closing cone reads upright, lit like the shaft).
COL_CAPITAL = [(147.25, 12.0), (147.5, 0.5)]
# The wall over them. Its face meets the back wall at the vertical dark
# line x 377 (the lintel ends there) and the back wall is at Z -65.5 (the
# pier's front corner, below): X = -65.5 + 2 * (377.25 - 368) = -47.
FACE_X = -47.0
# The lintel's bottom (first dark pixel under it): sx + 2 sy = 396 on every
# bay but the last (x 162..334), so X - Y = -194.5.
LINTEL_Y = FACE_X + 194.5
# The cut top: the purple band between sx + 2 sy = 198 (its first pixel,
# under the backdrop) and 260 (the light cornice's first pixel): a flat top
# from the face (X - Y = -330.5) back 62 (X - Y = -392.5).
TOP_Y = FACE_X + 330.5
BLOCK_BACK_X = TOP_Y - 392.5
# The aisle behind: its floor tiles show through the openings to X ~ -150
# (x 248, row 80); a dark wall at X -144 closes it (hidden over Y 254 by
# the block, so 240 tall); it runs on past the back wall (lit tiles at
# Z -90 through the last bay) to an end wall at Z -176.
CORR_X, CORR_Z, CORR_TOP = -144.0, -176.0, 240.0
FRONT_Z = 448.0         # the floor slab's front face (row 215 at x 160 .. 283 at 296)

# --- The back wall, the right side ----------------------------------------
# The pier at the right: its +X face's foot sx + 2 sy = 860 (x 570, row 145;
# x 560, row 150) on the floor -> X 302, the same plane as the floor slab's
# right face (its bottom x 500 row 195, x 560 row 165); its front corner
# between x 551 and 552 -> Z = 302 - 2 * (551.75 - 368) = -65.5; its cut top
# (purple) sx + 2 sy = 572.5 on the +X face -> Y 320.
BACK_Z = -65.5
RIGHT_X = 302.0
WALL_TOP = 320.0
WALL_T = 64.0           # thickness (only the pier's top shows it)

# --- The alcove (banner) --------------------------------------------------
# Its opening from the jamb x 494 (X 188) to the pier's left edge x 523/524
# (X 246); the jamb's lit face x 494..501 -> 16 deep; the arch's crown at
# row 14 (x 509) -> Y 270 on the wall, a half circle on the 58 opening.
ALC_X0, ALC_X1, ALC_BACK = 188.0, 246.0, BACK_Z - 16.0
ARCH_SPRING, ARCH_R = 241.0, 29.0
# Two steps in it, down to the right: corners' vertical edges x 511.5 rows
# 112-121 and x 521.5 rows 127-136, their feet on the floor and on the
# lower step -> front face Z -72.5, ends X 213.5 and 235.5, tops 64 and 48.
ALC_STEPS = ((213.5, 64.0), (235.5, 48.0))
ALC_STEP_Z = -72.5

# --- The dais and the carpet's steps ---------------------------------------
# The carpet's left edge on the floor: sx + 2 sy = 636 (x 336 row 150 ..
# x 388 row 124) -> X 77.5; on tread 1: 620 -> Y 48; on the top: 604 -> 64.
# Riser 1's left end x 392 -> Z 30; riser 2's x 408 -> Z -2.
TREAD1_Z, RISER2_Z = 30.0, -2.0
# --- The canopy -------------------------------------------------------------
# Top: left corner x 380.5 (rows 21-44), front corner x 448.5 row 53, the
# right end against the back wall x 490 -> X 42.5 .. 178.5, Z to 17.5, top
# Y 214 (the top's left edge outline x 400 row 10, x 410 row 5). Its front
# face's bottom (the curtains' top) x 404 row 47.5, x 420 row 55, x 440
# row 64 -> Y 182; the side face's x 453 row 70, x 482 row 58 -> Y 173 (the
# art is not one box: the side hangs 9 lower; a valance strip does it, from
# Z 8.5, where that edge starts at x 453: to Z 17.5 it took column B's
# capital, x 447-452 rows 68-73, as its own dark +X face).
CANOPY_X0, CANOPY_X1, CANOPY_Z1 = 42.5, 178.5, 17.5
CANOPY_TOP, CANOPY_BOTTOM, CANOPY_SIDE_BOTTOM = 214.0, 182.0, 173.0
VALANCE_Z1 = 8.5
# Its front columns: outlines x 386/397 (A, rows 50-95) and 440/451 (B) ->
# X - Z = 47, 155; A's base bottom row 107.5 (r 11) on tread 1 -> X + Z 66:
# A at (56.5, 9.5), B at (165, 10) (13.5 in from the canopy's sides, 7.5
# from its front). Up to the canopy's bottom (they stopped at 167, 15
# short: the back wall and the curtain showed between capital and canopy,
# and their caps lay flat at the tops, A rows 41-47, B rows 68-74). The
# capital: a bell from the shaft at 157 out to r 10.8 at 159 and 11.5 at
# 166 (A's outline x 384 rows 45-48; B's x 438-439 rows 72-75), an abacus
# r 11.5 to 180.5 (A's outline x 383 rows 36-44, B's x 437 rows 68-71; at
# r 10 back wall and curtain showed at x 384 / 438). The last 1.5 close in
# to r 7, so the cap stays behind the canopy's front (Z 17.5): at r 10 it
# stood 2 out of it, 7 flat pixels. The foot starts at 46, under the
# step's top (48): with its ring on it, it tied with the step top (column
# A's (399,103) went to the column, dark).
CANOPY_COLS = ((56.5, 9.5), (165.0, 10.0))
CANOPY_COL_PROFILE = [(46.0, 11.0), (54.0, 8.5), (157.0, 8.5), (159.0, 10.8), (166.0, 11.5), (180.5, 11.5),
                      (182.0, 7.0)]
# The curtains: the front pair hang just behind the columns (Z 4), cut to
# the art's red (the parting: its outline x 414 row 60 .. x 398 row 94 on
# the left, x 419 row 60 .. x 439 row 120 on the right); the right side's
# runs from column B to the back wall in the columns' plane X 165 (its red
# ends at x 484: Z -65.5). The dark curtain seen through the parting is the
# canopy's LEFT side from inside, on X 70: its hem in the parting runs up
# to the right along sx + 2 sy ~ 596 ((403,96) (410,93) (416,91)), a line of
# one X on the dais top. (Read first as a back curtain on Z -40: its hem
# 8-19 rows too high, the parting's dark red painted on the dais, a sliver
# of back wall showing past its end.)
CURTAIN_Z, CURTAIN_X, INNER_X = 4.0, 165.0, 70.0

# --- The pedestals and the statues -----------------------------------------
# Left: edges x 322 / 350 (28 wide), front corner x 336, foot row 126/127
# -> X 4..32, Z 68..96 (cell 0,2); top row 95 at the corner -> Y 96.
# Right: x 448 / 479, front corner x 463.5, foot row 177/178 -> X 229.5..
# 261.5, Z 38.5..70.5 (cell 7,1; the grid's own obstacle is (6,0), height
# 6: the same place on screen one cell further back); top Y 104.
PEDESTALS = (((4.0, FLOOR, 68.0), (32.0, 96.0, 96.0)), ((229.5, FLOOR, 38.5), (261.5, 104.0, 70.5)))
# The gargoyles: cut-outs of their art folded in a V on their pedestal's
# middle planes, the fold at the pedestal's front corner, on the card's old
# plane (X + Z = 300 / 100): the right one x <= 463 on Z 54.5, x >= 464 on
# X 245.5 (fold x 463.5); the left one x <= 336 on Z 81.5, x >= 337 on
# X 18.5 (fold x 336.5; on Z 82.5 the fold fell on column 336 and its
# apex took a pixel of the pedestal top). Half units: their lines on the
# pedestal tops fall between pixel rows. As one
# card square to the view, lamp 1 was behind the right one's plane (black)
# and the left one lit flat; and the card met the pedestal top on one row
# (134 right, 88 left), the pixels there alternating statue / pedestal.
GARGOYLE_FOLDS = ((336, 81.5, 18.5), (463, 54.5, 245.5))   # last column on the Z plane, Z, X
LEFT_GARGOYLE = [(313, 45.5), (318, 45.5), (324, 47.5), (329, 50.5), (333, 54.5), (336, 56.5), (340, 55.5),
                 (344, 58.5), (347, 63.5), (349.5, 70), (350.5, 78), (350.5, 100), (321.5, 100), (321.5, 80),
                 (320.5, 68), (317.5, 60), (313.5, 52)]
# The right one's outline leaves out the side curtain's draped tail behind
# its wing (x 471-489, rows 97-123: 144 red pixels it took, up to 98 units
# in front of their place).
RIGHT_GARGOYLE = [(436, 88.5), (441, 89.5), (448, 96.5), (455, 99.5), (462, 102.5), (468, 103.5), (474, 97.5),
                  (480, 91.5), (486, 86.5), (493.5, 84.5), (492.5, 90), (486.5, 95.5), (480.5, 96.5),
                  (476.5, 100.5), (476.5, 103.5), (480.5, 107.5), (472.5, 108.5), (472.5, 114.5),
                  (474.5, 118.5), (475.5, 120.5), (478.5, 123.5), (479.5, 134), (479.5, 150),
                  (447.5, 150), (448.5, 125), (449.5, 119.5), (454.5, 117.5), (454.5, 110.5), (451.5, 108.5),
                  (446, 104), (440, 96)]
# The right gargoyle's own pixels: the art's BG1 there, which is the gargoyle and its pedestal
# and nothing else (the user, states/thor/0241, found the two statues clipped and suggested
# taking the statue from BG1 rather than guessing). Read from that state's focus dump (layer
# code 1, scene columns 232-305, rows 110-186; reference = scene + (192, -36), by the
# pedestal's edges 448/479 and foot row). The traced outline above cut the wing's lower left
# (columns 468-476 at row 95), which stayed the curtain's, dark beside the lit body. The left
# gargoyle is BG0 art (no BG1 there at all): its traced outline stays.
RIGHT_GARGOYLE_BG1 = {
    85: [(486, 491)],
    86: [(483, 491)],
    87: [(480, 490)],
    88: [(477, 486)],
    89: [(476, 484)],
    90: [(437, 439), (474, 483)],
    91: [(437, 441), (473, 482)],
    92: [(438, 442), (471, 481)],
    93: [(439, 444), (470, 481)],
    94: [(440, 445), (469, 481)],
    95: [(441, 446), (468, 484)],
    96: [(442, 447), (450, 453), (467, 484)],
    97: [(443, 454), (457, 459), (466, 483)],
    98: [(443, 461), (466, 480)],
    99: [(444, 463), (466, 478)],
    100: [(444, 465), (468, 477)],
    101: [(445, 466), (468, 476)],
    102: [(445, 476)],
    103: [(445, 477)],
    104: [(446, 463), (465, 477)],
    105: [(448, 464), (467, 478)],
    106: [(450, 479)],
    107: [(451, 480)],
    108: [(452, 475), (478, 480)],
    109: [(453, 471)],
    110: [(453, 472)],
    111: [(454, 472)],
    112: [(454, 472)],
    113: [(455, 472)],
    114: [(454, 472)],
    115: [(454, 473)],
    116: [(453, 473)],
    117: [(453, 474)],
    118: [(452, 474)],
    119: [(452, 475)],
    120: [(451, 475)],
    121: [(450, 476)],
    122: [(450, 477)],
    123: [(450, 478)],
    124: [(450, 479)],
    125: [(451, 479)],
    126: [(451, 479)],
    127: [(452, 479)],
    128: [(453, 479)],
    129: [(454, 478)],
    130: [(455, 478)],
    131: [(454, 477)],
    132: [(452, 475)],
    133: [(450, 477)],
    134: [(448, 479)],
    135: [(448, 479)],
    136: [(448, 479)],
    137: [(448, 479)],
    138: [(448, 479)],
    139: [(448, 479)],
    140: [(449, 478)],
    141: [(449, 478)],
    142: [(449, 478)],
    143: [(448, 478)],
    144: [(448, 479)],
    145: [(448, 479)],
    146: [(448, 479)],
    147: [(449, 479)],
    148: [(449, 479)],
    149: [(449, 479)],
    150: [(449, 479)],
}

# The stone wing on the stair's left (the curved scroll left of column A),
# a cut-out on X 40, just left of the dais: its outline (386,70) -> (375,88)
# then down x 374 to ~row 110, its bottom on the foot line sx + 2 sy = 598
# on that plane. (A wider first trace took 162 floor pixels, lifted 66
# units on average, and the pier's base.)
STAIR_WING = [(385.5, 69.5), (386.5, 69.5), (386.5, 106.0), (373.5, 112.5), (373.5, 89.5), (374.5, 87.5),
              (375.5, 85.5), (376.5, 83.5), (377.5, 81.5), (378.5, 79.5), (380.5, 76.5), (381.5, 74.5),
              (383.5, 72.5)]
# The gold finial on the dais (x 419-429, rows 88-103, standing at about
# (104, 64, -12)): a cut-out on X + Z = 92 (it lay painted on the dais).
FINIAL = [(420.5, 87.5), (426.5, 87.5), (427.5, 90.5), (429.5, 102.5), (423.5, 103.5), (420.5, 93.5)]
FRONT_CURTAIN_L = [(392, 38), (420, 52), (416.5, 56), (414.5, 60), (412.5, 65), (410.5, 70), (407.5, 75),
                   (405.5, 80), (403.5, 85), (400.5, 90), (398.5, 94), (397.5, 97), (392, 99)]
# (The right one's parting a pixel further left than first traced, so it
# keeps its own dark outline as the left one does: the curtain behind had
# it, and the parting filled with navy streaks as the camera moved.)
FRONT_CURTAIN_R = [(417.5, 54), (446, 62), (446, 125), (438.5, 124.5), (436.5, 115), (434.5, 110), (431.5, 105),
                   (429.5, 100), (427.5, 95), (425.5, 90), (423.5, 85), (422.5, 80), (420.5, 75), (419.5, 70),
                   (418.5, 65), (417.5, 60)]


# --- Helpers -----------------------------------------------------------------
def lathe(m, name, x, z, profile, sides=12):
    """A turned shape (column, base, capital) from (Y, radius) pairs: its
    rings and a cap on top only (caps between the rings had their edges on
    the surface seen, and could take its pixels on those rings; the foot
    stands on the floor or a step)."""
    for (y0, r0), (y1, r1) in zip(profile, profile[1:]):
        if y1 > y0:
            m.cyl(name, (x, y0, z), (x, y1, z), r0, sides=sides, caps=False, r1=r1)
    y, r = profile[-1]
    ring = [(x + r * math.cos(2 * math.pi * i / sides), y, z + r * math.sin(2 * math.pi * i / sides))
            for i in range(sides)]
    m.poly(name, ring[::-1])


def on_plane_z(zc):
    """Screen point -> the point of the vertical plane Z = zc seen there."""
    def f(sx, sy):
        X = zc + 2.0 * (sx - KX)
        return (X, 2.0 * ((X + zc) / 4.0 + KY - sy), zc)
    return f


def on_plane_x(xc):
    def f(sx, sy):
        Z = xc - 2.0 * (sx - KX)
        return (xc, 2.0 * ((xc + Z) / 4.0 + KY - sy), Z)
    return f


def on_plane_c(c):
    """The vertical plane X + Z = c, square to the camera's view."""
    def f(sx, sy):
        return (c / 2.0 + (sx - KX), 2.0 * (c / 4.0 + KY - sy), c / 2.0 - (sx - KX))
    return f


def row_runs(poly):
    """The pixels (sampled at their corner) inside a screen polygon, as runs."""
    ys = [p[1] for p in poly]
    out = []
    for y in range(int(math.floor(min(ys))), int(math.ceil(max(ys))) + 1):
        xs = []
        n = len(poly)
        for i in range(n):
            (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for a, b in zip(xs[::2], xs[1::2]):
            xa, xb = int(math.ceil(a)), int(math.floor(b))
            if xb >= xa:
                out.append((y, xa, xb))
    return out


def cutout(m, name, poly, lift, flags=SHEET, cols=None, keep=None):
    """A flat cut-out of the art: the pixels inside the screen polygon (and
    within the columns `cols`, first and last, if given), one rectangle a
    run (rows with the same run merged), lifted onto a plane. Each rectangle
    runs half a pixel round its pixels' corners, so it covers exactly those
    sample points."""
    byrow = {}
    rows = [(y, xa, xb) for y, rr in poly.items() for xa, xb in rr] if isinstance(poly, dict) else row_runs(poly)
    if keep:   # only the pixels keep(sx, sy) takes: the runs cut where it says no
        cut = []
        for y, xa, xb in rows:
            x = xa
            while x <= xb:
                if not keep(x, y):
                    x += 1
                    continue
                x0 = x
                while x <= xb and keep(x, y):
                    x += 1
                cut.append((y, x0, x - 1))
        rows = cut
    for y, xa, xb in rows:
        if cols:
            xa, xb = max(xa, cols[0]), min(xb, cols[1])
            if xb < xa:
                continue
        byrow.setdefault(y, []).append((xa, xb))
    done = set()
    for y in sorted(byrow):
        for run in byrow[y]:
            if (y, run) in done:
                continue
            y1 = y
            while run in byrow.get(y1 + 1, []):
                y1 += 1
                done.add((y1, run))
            xa, xb = run
            q = [(xa - 0.5, y1 + 0.5), (xb + 0.5, y1 + 0.5), (xb + 0.5, y - 0.5), (xa - 0.5, y - 0.5)]
            m.poly(name, [lift(sx, sy) for sx, sy in q], flags)


def arch_y(x):
    """The alcove arch's edge over X: a half circle on the opening."""
    c = 0.5 * (ALC_X0 + ALC_X1)
    d = min(1.0, abs(x - c) / ARCH_R)
    return ARCH_SPRING + ARCH_R * math.sqrt(1.0 - d * d)


# --- The parts -----------------------------------------------------------------
def build_parts(m):
    # The colonnade: columns up to the lintel, the wall over them (face,
    # cut top, the end at the back wall), the aisle's floor, back and end walls.
    for z in COL_ZS:
        lathe(m, 'columns', COL_X, z, COL_PROFILE + COL_CAPITAL, sides=16)
    m.box('arcade', (BLOCK_BACK_X, LINTEL_Y, BACK_Z), (FACE_X, TOP_Y, FRONT_Z))
    m.poly('corridor', [(CORR_X, FLOOR, CORR_Z), (CORR_X, FLOOR, FRONT_Z), (0.0, FLOOR, FRONT_Z), (0.0, FLOOR, CORR_Z)])
    m.poly('corridor', [(CORR_X, FLOOR, CORR_Z), (CORR_X, CORR_TOP, CORR_Z), (CORR_X, CORR_TOP, FRONT_Z),
                        (CORR_X, FLOOR, FRONT_Z)])
    m.poly('corridor', [(CORR_X, FLOOR, CORR_Z), (FACE_X, FLOOR, CORR_Z), (FACE_X, CORR_TOP, CORR_Z),
                        (CORR_X, CORR_TOP, CORR_Z)])

    # The back wall and the right side (the pier): either side of the
    # alcove, and the span over its arch.
    m.box('back_wall', (FACE_X, FLOOR, BACK_Z - WALL_T), (ALC_X0, WALL_TOP, BACK_Z))
    m.box('back_wall', (ALC_X1, FLOOR, BACK_Z - WALL_T), (RIGHT_X, WALL_TOP, BACK_Z))
    n = 8
    for i in range(n):
        xa = ALC_X0 + (ALC_X1 - ALC_X0) * i / n
        xb = ALC_X0 + (ALC_X1 - ALC_X0) * (i + 1) / n
        m.poly('back_wall', [(xa, arch_y(xa), BACK_Z), (xb, arch_y(xb), BACK_Z), (xb, WALL_TOP, BACK_Z),
                             (xa, WALL_TOP, BACK_Z)])
    m.poly('back_wall', [(ALC_X0, WALL_TOP, BACK_Z - WALL_T), (ALC_X0, WALL_TOP, BACK_Z), (ALC_X1, WALL_TOP, BACK_Z),
                         (ALC_X1, WALL_TOP, BACK_Z - WALL_T)])
    # The alcove: its back (the banner's wall), its floor, its two steps
    # (its left jamb is the wall box's +X face at X 188).
    m.poly('alcove', [(ALC_X0, FLOOR, ALC_BACK), (ALC_X1, FLOOR, ALC_BACK), (ALC_X1, WALL_TOP, ALC_BACK),
                      (ALC_X0, WALL_TOP, ALC_BACK)])
    m.poly('alcove', [(ALC_X0, FLOOR, ALC_BACK), (ALC_X0, FLOOR, BACK_Z), (ALC_X1, FLOOR, BACK_Z), (ALC_X1, FLOOR, ALC_BACK)])
    x0 = ALC_X0
    for x1, top in ALC_STEPS:
        m.box('alcove', (x0, FLOOR, ALC_BACK), (x1, top, ALC_STEP_Z))
        x0 = x1

    # The floor where the grid has no cells: in front of the back wall
    # (Z < 0), the strip X 288-302 the user's cells stop short of, and the
    # slab's cut faces there (the right one runs on under the pier).
    m.poly('floor', [(0.0, FLOOR, BACK_Z), (0.0, FLOOR, 0.0), (RIGHT_X, FLOOR, 0.0), (RIGHT_X, FLOOR, BACK_Z)])
    m.poly('floor', [(288.0, FLOOR, 0.0), (288.0, FLOOR, FRONT_Z), (RIGHT_X, FLOOR, FRONT_Z), (RIGHT_X, FLOOR, 0.0)])
    m.poly('floor', [(RIGHT_X, 0.0, BACK_Z - WALL_T), (RIGHT_X, FLOOR, BACK_Z - WALL_T), (RIGHT_X, FLOOR, FRONT_Z),
                     (RIGHT_X, 0.0, FRONT_Z)])
    m.poly('floor', [(288.0, 0.0, FRONT_Z), (RIGHT_X, 0.0, FRONT_Z), (RIGHT_X, FLOOR, FRONT_Z), (288.0, FLOOR, FRONT_Z)])

    # The dais (Y 64) and tread 1 (Y 48) under the canopy's span; the wing.
    m.box('dais', (CANOPY_X0, FLOOR, BACK_Z), (CANOPY_X1, 64.0, RISER2_Z))
    m.box('dais', (CANOPY_X0, FLOOR, RISER2_Z), (CANOPY_X1, 48.0, TREAD1_Z))
    cutout(m, 'stair_wing', STAIR_WING, on_plane_x(CANOPY_X0 - 2.5))

    # The canopy: the box, the side's lower valance, the two front columns.
    m.box('canopy', (CANOPY_X0, CANOPY_BOTTOM, BACK_Z), (CANOPY_X1, CANOPY_TOP, CANOPY_Z1))
    m.box('canopy', (CANOPY_X1 - 4.0, CANOPY_SIDE_BOTTOM, BACK_Z), (CANOPY_X1, CANOPY_BOTTOM, VALANCE_Z1))
    for x, z in CANOPY_COLS:
        lathe(m, 'canopy_cols', x, z, CANOPY_COL_PROFILE, sides=10)
    # The curtains.
    cutout(m, 'curtains', FRONT_CURTAIN_L, on_plane_z(CURTAIN_Z))
    cutout(m, 'curtains', FRONT_CURTAIN_R, on_plane_z(CURTAIN_Z))
    zb = CANOPY_COLS[1][1]
    m.poly('curtains', [(CURTAIN_X, 64.0, zb), (CURTAIN_X, 64.0, BACK_Z), (CURTAIN_X, CANOPY_BOTTOM, BACK_Z),
                        (CURTAIN_X, CANOPY_BOTTOM, zb)], SHEET)
    m.poly('curtains', [(INNER_X, 64.0, CURTAIN_Z), (INNER_X, 64.0, BACK_Z), (INNER_X, CANOPY_BOTTOM, BACK_Z),
                        (INNER_X, CANOPY_BOTTOM, CURTAIN_Z)], SHEET)
    cutout(m, 'curtains', FINIAL, on_plane_c(92.0))

    # The pedestals and the gargoyles.
    for lo, hi in PEDESTALS:
        m.box('pedestals', lo, hi)
    # Both halves of a gargoyle take the lamps' light by one normal: by its plane's, the half turned from
    # the chandeliers was dark from the fold on, a straight vertical cut down the statue
    # (the user, states/thor/0241, found the two statues clipped). Map 91 is a
    # round_light map: a model pixel turned between the axes is lit by its own normal.
    # The fold is rounded for the light (the user suggested keeping the fold but
    # rounding it for a softer transition): each corner's normal turns from the Z
    # plane's (+Z) to the X plane's (+X) over FOLD_ROUND units either side of the fold, by
    # how far along its plane the corner is from it, as a column's does round its shaft.
    # Map 91 is a round_light map: a model pixel turned between the axes is lit by its own
    # normal. (One normal for both halves, tried before, lit the statue flat.)
    # The right one stands on its pedestal's FRONT planes, from the pedestal's top up (the
    # user asked for the right statue's base, the dragon's feet, to be part of
    # it too): on the middle planes the feet, drawn over the front half of the pedestal's top,
    # were the top's (rays meet the top before a plane behind it), lit as a floor and left
    # behind by the statue. Every BG1 pixel over the top's front edges is the statue's now;
    # the pedestal keeps its two faces. Same fold column (the front corner's).
    for n, (poly, (xf, zp, xp)) in enumerate(zip((LEFT_GARGOYLE, RIGHT_GARGOYLE_BG1), GARGOYLE_FOLDS)):
        first = len(m.verts)
        keep_z = keep_x = None
        if n == 1:
            top = PEDESTALS[1][1][1]
            zp, xp = PEDESTALS[1][1][2], PEDESTALS[1][1][0]
            keep_z = lambda sx, sy, f=on_plane_z(zp): f(sx, sy)[1] >= top
            keep_x = lambda sx, sy, f=on_plane_x(xp): f(sx, sy)[1] >= top
        cutout(m, 'statues', poly, on_plane_z(zp), SHEET | STATUE_FLAGS, cols=(-10000, xf), keep=keep_z)
        cutout(m, 'statues', poly, on_plane_x(xp), SHEET | STATUE_FLAGS, cols=(xf + 1, 10000), keep=keep_x)
        for i in range(first, len(m.verts)):
            x, y, z = m.verts[i][:3]
            along = (x - xp) if abs(z - zp) < 1e-3 else (zp - z)   # < 0 on the Z plane, > 0 on the X plane
            a = math.radians(FOLD_TURN * (0.5 + 0.5 * max(-1.0, min(1.0, along / FOLD_ROUND))))
            m.verts[i] = (x, y, z, math.sin(a), 0.0, math.cos(a))


def build():
    m = Model('hall91')
    build_parts(m)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map91.inc")
    m.write_inc(out, 91, "tools/models/room91.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
