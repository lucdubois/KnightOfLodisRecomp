#!/usr/bin/env python3
"""Map 83, down a circular stairwell (flat.png, 480x270, sprites and HUD
hidden). Projection: sx = (X - Z)/2 + 144, sy = (X + Z)/4 - Y/2 + 152; the
grid is the floor alone (cells x 0-6, z 0-7, height 1, Y 16; cells 6,4-7
left out). All numbers below are measured on flat.png; "row"/"column" are
its pixels, sampled at their corner (pixel (x, y) shows the point (x, y)).

The scene: a square room with a back wall (the door between two wall
torches) and a short left wall, its floor a platform standing over a dark
void (its front edge the yellow curb along X 224). In the room's front-left
corner a round stairwell: a tall newel column, a curved wall (the "drum")
wrapped round the newel's front-right, a spiral of wedge treads rising
counterclockwise (seen from above) from the front up the left side to the
back, the stairwell's far wall behind them (cut on the plane through the
axis facing the camera: the textured strip at the picture's left edge), and
its near wall, cut down to a rim that follows the stair, dropping as a
curved brick face into the void at the bottom-left.

Depth (free along (1,1,1)) is fixed by contact: the walls stand on the floor
(their feet on Z 0 / X -32 at Y 16); the stair's foot is a platform 23
over the floor with a lower step in front of its right end, both standing
on the floor (the platform's front-right corner meets it at (151.5, 235),
the step's end at row 224.25 under column 173); the drum's cut end stands on
the platform's back edge; the newel shares the drum's axis; the stair and
the near wall share that axis. (Until 11-03 the drum stood on the floor and
the landing was floor: the whole stairwell sat 23 too deep.)

    tools/models/room83.py      writes src/kol_model_map83.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, SHEET, NOSHADOW  # noqa: E402

FLOOR = 16.0          # the grid: every cell height 1
WALL_TOP = 144.0      # back wall: its cut end's top corners (271.5,128) (255.5,136.5) -> Y 144
VOID = -200.0         # faces over the void go down past the picture's bottom row
SKY = 420.0           # newel and far wall rise past the picture's top row (72)

# ---- room ------------------------------------------------------------------
# Back wall: inner face Z 0 (its foot meets the floor on the line from the
# corner (128,136) to (255.5,199.75); the top's inner edge row 83 at column
# 150, 93 at 170, the outer edge row 77 at 170: 16 rows = 32 thick); cut end
# X 224 (columns 256-271, the grid's edge), top Y 144.
BACK_Z0, BACK_Z1, BACK_X1 = -32.0, 0.0, 224.0
# Left wall: inner face X -32 (its top inner edge row 79 at column 112, 75 at
# 120, the corner at column 128 row 72); runs from the back wall to behind
# the newel (it is hidden past Z 38) where the stairwell's far wall meets it
# (its outer face crosses X -32 at Z 57.5 since the stairwell was raised).
LEFT_X0, LEFT_X1, LEFT_Z1 = -64.0, -32.0, 59.0
# Door: a recessed opening in the back wall, the stone frame flush with the
# wall. Left jamb (facing +X) columns 172-177: the opening's edge X 56 on the
# face, the leaf 12 back (6 columns); right edge column 209 -> X 130; the
# lintel's underside rows 113 (col 172) to 131.5 (col 209) -> Y 106.
DOOR_X0, DOOR_X1, DOOR_TOP, DOOR_DEPTH = 56.0, 130.0, 106.0, 12.0
# Torch brackets: inverted cones on the back wall, their tips on the face
# (left column 145.5 row 130 -> X 3, Y 45.5; right column 234 row 168 ->
# X 180, Y 57), their cups 9 columns wide (r 6.5) standing off the wall by
# their radius (left cup centre column 144.5 row 115.5, right 232.5 row 155.5).
TORCH_R = 6.5
TORCHES = (((3.0, 45.5, 0.0), (8.0, 80.5, 7.0)), ((180.0, 57.0, 0.0), (184.0, 88.5, 7.0)))
# Their flames on cards at the cups' depth (Z 7), cut to the flame's own
# pixels (its colours (156,222,0) (247,239,16) (247,255,222), a run a row):
# on the wall plane they left their cups as the camera moved.
FLAME_Z = 7.0
FLAMES = ({96: (139, 139), 97: (138, 139), 98: (138, 142), 99: (142, 144), 100: (143, 145), 101: (143, 147),
           102: (142, 147), 103: (139, 148), 104: (138, 148), 105: (137, 148), 106: (137, 147), 107: (138, 147),
           108: (138, 148), 109: (139, 149), 110: (141, 149), 111: (139, 149), 112: (139, 148), 113: (140, 147),
           114: (142, 145)},
          {136: (226, 226), 137: (225, 226), 138: (225, 229), 139: (229, 231), 140: (230, 232), 141: (230, 234),
           142: (229, 234), 143: (226, 235), 144: (225, 235), 145: (224, 235), 146: (224, 234), 147: (225, 234),
           148: (225, 235), 149: (226, 236), 150: (228, 236), 151: (226, 236), 152: (226, 235), 153: (227, 234),
           154: (229, 232)})
# A breach in the back wall left of the door (a dark hole with rubble, x
# 158-167, its dark from rows 126-127): a recess as the door's, X 28-48,
# Y 16-72 (to X 52 it took the door frame's light pilaster, x 168-169, a
# blank slab beside the hole with the camera moved; to Y 80, 3-4 rows of lit
# wall over the hole); 40 deep, through the wall (12 first, a recess as the
# door's; the user, under the torchlight, wanted the hole to feel dark
# and hollow and picked 40 of 12 and 40).
BREACH_X0, BREACH_X1, BREACH_TOP, BREACH_DEPTH = 28.0, 48.0, 72.0, 40.0
# Platform: the floor's front edge (the curb) along X 224 from the cut end
# (256,200) to the front-left corner (168,244) = Z 176; the corner's other
# face Z 176 back to X 192 (column 152, where the stairwell's wall ends at
# column 151). The grid leaves out cells 6,4-7: the art's floor goes on to
# the curb there (X 192-224, Z 128-176), so that corner's top is in the model.
PLAT_X1, PLAT_Z1, NOTCH_X0, NOTCH_Z0 = 224.0, 176.0, 192.0, 128.0
# Floor between the left wall and the grid's edge (X -32..0), near the back
# corner (columns 112-144, rows 136-160); off the grid, so in the model.
MARGIN_Z1 = 100.0

# ---- the stair's foot (the user's painting of what stands over the floor, 11-03)
# A platform (the landing) 23 over the floor, and in front of its right end
# a lower step, the "foot step", both standing on the floor. Measured (a
# dark outline's centre line is the edge, as for line (d) and B):
# - The foot step's +X end (the user's blue strip) is a face of constant X:
#   its top edge runs along Z from the end of line (d), (173, 219.75), down
#   to the left; its foot is the dark outline row = -0.5 col + 310.75
#   (2-pixel runs (171-172, 225) ... (153-154, 234)), from (173, 224.25)
#   (column 173 is a vertical, rows 220-223, from one to the other: 4.5 rows
#   = 9 tall) to (151.5, 235). So its back-bottom corner is X 189.5, Z 131.5
#   on the floor, its top Y 25.
# - Line (d) (row = 0.5 col + 133.25) is that step's back top edge, not a
#   seam in the floor: past its end the floor's own seam (Z 127.5, row =
#   0.5 col + 135.75) carries on 2.5 rows lower. Along (d), Z - Y = 106.5.
# - The platform's front-right corner: one vertical at column 151 from the
#   rim (row 223.5) down to the floor at row 235 (the end of that outline,
#   the dark spot at columns 150-155, rows 235-237: the user's purple, the
#   corner where the foot meets the floor): X 189.5, Z 174.5 on the floor,
#   top Y 39. The rim stays level at row 223.5 from column 112 to the corner
#   (the cap's first row is 224 all along), so the front there is the plane
#   X + Z = 364, facing the camera; the user's yellow is its part over the
#   floor, under it it carries on into the void as the near wall does.
# - B, the platform's edge over the foot step: the straight dark line from
#   (120.5, 193.5) on line (d) to the corner (151.5, 223.5) (through (126,
#   199), (141, 214)), at Y 39 from (98.5, 145.5) to (189.5, 174.5). Its
#   riser faces away from the camera, so the step's top shows right under it
#   (the user's red, between (d) and B) and the platform's top over it (the
#   user's green, between A and B).
# - Line A ((113, 191) to (129.5, 224), row = 2 col - 35) and the column-106
#   line are joints in the platform's paving, not steps: the rim runs level
#   across both, and a step down at A would show its side (it faces the
#   camera) as a band under A; there is none. The step's top carries joints
#   too (row = 0.5 col + 143.5 and its cross joint from (153, 210) to (143.5,
#   215.5)).
# - Colours: the stair's treads and the near wall's cap are the art's
#   blue-grey family, the foot's tops (red, green, and the slabs from column
#   96 to A, rows 186-223) its green family: one paving.


def xz_at(sx, sy, y):
    """(X, Z) of the point at height y drawn at (sx, sy)."""
    d, s = 2.0 * (sx - 144.0), 4.0 * (sy - 152.0) + 2.0 * y
    return ((s + d) / 2, (s - d) / 2)


D_LINE = (0.5, 133.25)    # line (d): row = 0.5 * column + 133.25 (2-pixel runs at rows 216-219)
D_ZY = 2.0 * (D_LINE[1] - 80.0)   # 106.5: Z - Y along it
FOOT_TOP, FOOT_BASE = (173.0, 219.75), (173.0, 224.25)  # the foot step's +X end, back corner
FOOT_Y = FLOOR + 2.0 * (FOOT_BASE[1] - FOOT_TOP[1])     # 25
FOOT_X1, FOOT_Z0 = xz_at(FOOT_BASE[0], FOOT_BASE[1], FLOOR)   # 189.5, 131.5 (= FOOT_Y + D_ZY)
CORNER_BASE, FRONT_ROW = (151.5, 235.0), 223.5          # the platform's corner on the floor; the rim
PLAT_Y = FLOOR + 2.0 * (CORNER_BASE[1] - FRONT_ROW)     # 39: the platform's top, the landing
CORNER = xz_at(CORNER_BASE[0], CORNER_BASE[1], FLOOR)   # (189.5, 174.5)
FRONT_XZ = CORNER[0] + CORNER[1]                        # 364: the platform's front, X + Z
FRONT_X0 = 111.5          # the front plane's left end: the paving starts at column 112 (columns
                          # 108-111, rows 216-223, are the cap's colours, as left of 107)
JOIN_DTH = 4.0            # over its last 4 degrees the near wall's outer face swings out to the plane
B0 = xz_at(120.5, 193.5, PLAT_Y)                        # (98.5, 145.5): B on line (d)
PLAT_Z0 = PLAT_Y + D_ZY   # 145.5: the platform's back edge on line (d) (columns 105-120.5)

# ---- stairwell ---------------------------------------------------------------
# Axis: the newel spans columns 80-111 (edges 79.85 and 111.65: centre
# 95.75 -> X - Z = -96.5, radius 15.9 columns = 22.5). The drum's top shares
# that centre (its arc meets row 133 at column 92 and row 130.5 at column
# 110); its cut end's top edge runs along +X from (113.75,124.5) to
# (119.5,127.4) (outline pixels 112-119, rows 123-127), its vertical edge at
# column 119 down to row 191. First placed with that foot on the floor (top
# Y 144, the room's wall height: axis X + Z = 142), which put the landing at
# the floor; the landing is the platform, 23 higher, so the whole
# stairwell stands T_LIFT along (1, 1, 1) higher (the picture is the same):
# the bottom flight (two steps of 7) steps down 6.9 onto the platform, the
# drum (128 tall, as the room's walls) stands on it, its cut face 3.25 behind
# the platform's back edge (the art draws its foot 1.6 rows over line (d)).
T_LIFT = PLAT_Y - FLOOR   # 23
AX, AZ = 22.75 + T_LIFT, 119.25 + T_LIFT
DRUM_TOP = WALL_TOP + T_LIFT
R_NEWEL = 22.5
NEWEL_FOOT = FLOOR        # down to the floor: raised (to 63) it left a slot over the landing
                          # and the lowest step, a lit tread inside the pillar (11-03, torches)
# Drum: a curved wall from the cut end (theta 0: +X from the axis) round the
# front to theta 73.3 (its outer face meets the newel's left edge, column
# 79.85); outer radius 47.5 (the cut end at column 119.5), inner 36 (the
# pale top's back edge rows 126.5 at column 108, 128.5 at 100).
DRUM_RI, DRUM_RO, DRUM_T1 = 36.0, 47.5, 73.3
DRUM_CLOSE = 12.0         # from here round the drum stands on the landing and the steps
# Line (d) (D_LINE): the straight dark line from (100,183) through
# (119,192.5) to (173,219.75): the drum's bricks end on it. Right of the drum
# it is the foot step's back edge, at the drum's cut end the platform's back
# edge (the cut end stands on it). Further left the art draws it on across
# the drum's foot (the platform and the bottom steps drawn up to it), not on
# its foot's arc on the platform (12.7 rows lower at x 100). No geometry
# consistent with the stair puts the drum's foot on (d) there (a piece in
# front of it with its back edge on (d) would stand as high as the gap, where
# the art draws the paving). Up to DRUM_CLOSE (column ~114, the gap under 5
# rows) the drum's bottom follows (d); from there round it stands on the
# landing and the bottom flight's steps, its foot stepping at their risers,
# and the paving the art draws between (d) and that foot (425 pixels,
# columns 80-114, rows 174-196) lies on the drum's face. (Until 11-03 it
# hovered on (d), up to 33 over the steps: the torches lit the landing under
# it and its shadow ran as one straight edge down the steps, a ramp.)
# Treads: their front edges (a lit top over a dark riser) traced on the left
# (six edges, 5-9 points each, columns 33-79, rows 98-177) and fitted as
# radial edges about the axis with a constant rise of one height step:
# edge at theta 107.02 + k * 16.89 deg, top Y 52.92 + 16 k (rms 1 px) with
# the drum on the floor; T_LIFT higher now.
T_TH0, T_DTH, T_Y0, T_H = 107.02, 16.89, 52.92 + T_LIFT, 16.0
T_RO = 92.0           # inner face of the far wall at the strip's edge (column 30.7, theta 135)
T_RO_MAX = 104.0
STRIP_X = 30.75       # every tread ends on this column (the strip's right edge)
FAR_DR = 6.0          # the far wall's inner face widens toward the back: the art's
                      # upper treads end 2-3 px further out than radius 92 allows
LOW_H = 7.0           # the bottom flight: two steps of 7 under the helix's tread at theta 90
# Their nosings at ~91, 70.3 and 50 degrees: 20.5 apart, not the helix's
# 16.89 (at 16.89 the lowest step's edge sat 2-8 px off the art and took
# ~105 px of the landing's paving; the reviewer's fit).
LOW_DTH = 20.5
O_FRONT_END = 98.4    # the near wall's left end: outer face at column 30.5 (the strip's edge)
O_FULL0, O_END = 135.0, 234.0   # far wall: from the cut through the axis (the strip,
                                # columns 14.4-30.7) round past the room's left wall (X -32,
                                # crossed at 227.5 deg, Z 57.5), behind the newel
O_RO = 115.0          # outer radius: the strip's left (outer) edge just past the picture's
                      # edge, the near wall's cap 8 rows deep at the front
LAND_Y = PLAT_Y       # the landing in front of the drum: the platform (the bottom flight's
                      # second step 6.9 above it)
# Near wall's rim, traced: the top of its brick face (the top of the pale cap
# at the front). Left of column 88 the art draws no top, so the coping is
# seen edge-on (both edges on the rim); right of column 100 the landing's
# paving runs on level over the wall's top to the rim, so there the wall's
# outer face swings out from radius 115 to meet the rim at the landing's
# level. From column 107 to the corner (151.5) the rim is 2.5 rows lower
# (223.5) and level: the platform's front plane (FRONT_XZ), from column
# 111.5 (where the paving starts; the cap is there to row 223 on columns
# 108-111). No corner joins the two below the rim: the brick courses run on
# across columns 100-115 (the joints at rows 226, 233, 240, 246, 252, 259
# cross from column 88 to 111 and on), and the dark line at column 106-107
# stops at row 225. So over its last JOIN_DTH (columns ~105-111.5) the near
# wall's outer face swings out to the plane, one face with it, and its top
# keeps the rim on row 221 there: it rises to a coping 5 over the paving
# (Y 44 at the joint), the rim's 2.5 rows. (A radial face from the landing
# down into the void stood there before, 11-03: edge-on at rest, it opened
# into a stripe down the wall with the camera to the left.)
RIM_PTS = [(33.0, 168.0), (44, 187), (54, 198), (66, 207), (80, 215), (90, 219), (100, 221), (106.75, 221)]
RIM_LEVEL_X0, RIM_LEVEL_X1 = 88.0, 100.0


def pol(r, th, y):
    t = math.radians(th)
    return (AX + r * math.cos(t), y, AZ + r * math.sin(t))


def sx_of(r, th):
    p = pol(r, th, 0.0)
    return (p[0] - p[2]) / 2 + 144


def y_at(r, th, sy):
    """Height of the point at radius r, angle th drawn on row sy."""
    p = pol(r, th, 0.0)
    return 2 * ((p[0] + p[2]) / 4 + 152 - sy)


def sector(m, name, r0, r1, t0, t1, ybot, ytop, step=4.0, flags=0, top_name=None):
    """A ring-sector solid about the axis: radii r0..r1 (numbers or functions
    of the angle), angles t0..t1 in degrees (counterclockwise seen from
    above: +X toward +Z, 45 = toward the camera, 135 = screen left), from
    ybot up to ytop (numbers or functions of the angle; ytop may return
    (top at r0, top at r1)). Faces wound outward; the top faces may take
    another part's name (top_name)."""
    n = max(1, int(math.ceil(abs(t1 - t0) / step)))
    ths = [t0 + (t1 - t0) * i / n for i in range(n + 1)]
    def tops(t):
        v = ytop(t) if callable(ytop) else ytop
        return v if isinstance(v, tuple) else (v, v)
    bot = ybot if callable(ybot) else (lambda t: ybot)
    R0 = r0 if callable(r0) else (lambda t: r0)
    R1 = r1 if callable(r1) else (lambda t: r1)
    for i in range(n):
        a, b = ths[i], ths[i + 1]
        (ia, oa), (ib, ob), ba, bb = tops(a), tops(b), bot(a), bot(b)
        ra0, rb0, ra1, rb1 = R0(a), R0(b), R1(a), R1(b)
        m.poly(top_name or name, [pol(ra0, a, ia), pol(rb0, b, ib), pol(rb1, b, ob)], flags)    # top
        m.poly(top_name or name, [pol(ra0, a, ia), pol(rb1, b, ob), pol(ra1, a, oa)], flags)
        m.poly(name, [pol(ra1, a, ba), pol(ra1, a, oa), pol(rb1, b, ob), pol(rb1, b, bb)], flags)  # outer
        if ra0 > 0:
            m.poly(name, [pol(ra0, a, ba), pol(rb0, b, bb), pol(rb0, b, ib), pol(ra0, a, ia)], flags)  # inner
        m.poly(name, [pol(ra0, a, ba), pol(ra1, a, ba), pol(rb1, b, bb), pol(rb0, b, bb)], flags)  # bottom
    (i0, o0), (i1, o1) = tops(t0), tops(t1)
    m.poly(name, [pol(R0(t0), t0, bot(t0)), pol(R0(t0), t0, i0), pol(R1(t0), t0, o0), pol(R1(t0), t0, bot(t0))],
           flags)
    m.poly(name, [pol(R0(t1), t1, bot(t1)), pol(R1(t1), t1, bot(t1)), pol(R1(t1), t1, o1), pol(R0(t1), t1, i1)],
           flags)


def far_ri(t):
    """The far wall's inner radius: 92 at the strip (theta 135), widening by
    FAR_DR over the next 25 degrees."""
    return T_RO + FAR_DR * min(1.0, max(0.0, (t - O_FULL0) / 25.0))


def tread_ro(t):
    """A tread's outer end: the near wall's inner face where that wall stands;
    past its end, out to the strip's edge (STRIP_X) but at most T_RO_MAX; on
    the far side at least to the far wall's inner face."""
    if t <= O_FRONT_END:
        return T_RO
    d = math.cos(math.radians(t)) - math.sin(math.radians(t))
    r = T_RO_MAX
    if d < 0:
        r = min(r, (STRIP_X - 144 - (AX - AZ) / 2) * 2 / d)
    r = max(T_RO, r)
    return max(r, far_ri(t)) if t > O_FULL0 else r


def drum_bottom(t):
    """The drum's foot by its cut end: the platform it stands on, or line (d)
    where that is higher."""
    x = sx_of(DRUM_RO, t)
    return max(PLAT_Y, y_at(DRUM_RO, t, D_LINE[0] * x + D_LINE[1]))


def drum_spans():
    """The drum in (from, to, bottom) spans: on line (d) up to DRUM_CLOSE,
    then on the landing and each of the bottom flight's steps (split at their
    risers, so its foot steps with them)."""
    t1 = T_TH0 - T_DTH
    edges = [0.0] + [c for c in (DRUM_CLOSE, t1 - 2 * LOW_DTH, t1 - LOW_DTH) if 0.0 < c < DRUM_T1] + [DRUM_T1]
    return [(a, b, drum_bottom if b <= DRUM_CLOSE + 1e-9 else stair_top(0.5 * (a + b)))
            for a, b in zip(edges, edges[1:])]


def rim_row(x):
    pts = RIM_PTS
    if x <= pts[0][0]:
        (x0, y0), (x1, y1) = pts[0], pts[1]
    elif x >= pts[-1][0]:
        (x0, y0), (x1, y1) = pts[-2], pts[-1]
    else:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if x <= x1:
                break
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0)


def on_z(sx, sy, z):
    """The point of the plane Z = z drawn at (sx, sy)."""
    x = 2.0 * (sx - 144.0) + z
    return (x, 2.0 * ((x + z) / 4.0 + 152.0 - sy), z)


def stair_top(t):
    """The top of the step (or the landing) at angle t, below the helix's
    first tread: the bottom flight, then the landing."""
    t1 = T_TH0 - T_DTH
    if t >= t1:
        return T_Y0 - T_H
    if t >= t1 - LOW_DTH:
        return T_Y0 - T_H - LOW_H
    if t >= t1 - 2 * LOW_DTH:
        return T_Y0 - T_H - 2 * LOW_H
    return LAND_Y


def front_ro(t):
    """The near wall's outer radius at angle t: O_RO, or out to where its top
    at the landing's level is drawn on the traced rim, when that is farther
    (right of ~column 96, where the rim at O_RO would be below the landing)."""
    if y_at(O_RO, t, rim_row(sx_of(O_RO, t))) >= LAND_Y:
        return O_RO
    c = math.cos(math.radians(t)) + math.sin(math.radians(t))
    r = O_RO
    for _ in range(8):
        r = (4.0 * (rim_row(sx_of(r, t)) - 152.0 + LAND_Y / 2) - (AX + AZ)) / c
    return max(O_RO, r)


def plane_ro(t):
    """The platform's front (the plane X + Z = FRONT_XZ) at angle t."""
    return (FRONT_XZ - AX - AZ) / (math.cos(math.radians(t)) + math.sin(math.radians(t)))


def angle_of(x, z):
    return math.degrees(math.atan2(z - AZ, x - AX))


def front_angles():
    """The platform front's two ends: the corner (column 151.5) and its left
    end at column FRONT_X0, where the near wall's rim takes over."""
    d = 2.0 * (FRONT_X0 - 144.0)
    return angle_of(*CORNER), angle_of((FRONT_XZ + d) / 2, (FRONT_XZ - d) / 2)


def near_ro(t):
    """The near wall's outer radius: front_ro, swung out to the platform's
    front plane over the last JOIN_DTH before the joint (smoothstep)."""
    th_j = front_angles()[1]
    s = min(1.0, max(0.0, (th_j + JOIN_DTH - t) / JOIN_DTH))
    s = s * s * (3.0 - 2.0 * s)
    return front_ro(t) * (1.0 - s) + max(front_ro(t), plane_ro(t)) * s


def coping_y(t):
    """The near wall's top where it swings out: level with its inner edge
    (rim_y's), the landing's paving running on over it. (Raised on the
    traced rim, up to 5 over the landing at the joint, it kept the cap's
    rows 221-223 at columns 108-111 upright, but its ambient occlusion lay
    as a dark smudge on the landing beside it, columns 98-128, which the art
    does not draw; the reviewer, 11-03.)"""
    inner = rim_y(t, near_ro(t))[0]
    return (inner, inner)


def rim_y(t, ro=None):
    """The near wall's top at angle t: (inner edge, outer edge). Left of
    RIM_LEVEL_X0 both edges on the traced rim (the coping seen edge-on);
    right of RIM_LEVEL_X1 the inner edge at the landing's level and the outer
    edge on the traced rim (the paving runs level over the wall: front_ro)."""
    ro = front_ro(t) if ro is None else ro
    xi, xo = sx_of(T_RO, t), sx_of(ro, t)
    edge_in, edge_out = y_at(T_RO, t, rim_row(xi)), y_at(ro, t, rim_row(xo))
    w = min(1.0, max(0.0, (xo - RIM_LEVEL_X0) / (RIM_LEVEL_X1 - RIM_LEVEL_X0)))
    # The inner edge never under the step or landing beside it: on the traced
    # rim (~7 under the landing at r 92) it dropped below them, and the rays
    # at x 72-102 rows 210-217 passed into the wall's empty inside (103 px of
    # the grid's floor showing through).
    inner = max(edge_in * (1 - w) + LAND_Y * w, stair_top(t))
    return (inner, edge_out * (1 - w) + min(LAND_Y, edge_out) * w)


def room(m):
    """The room. Its walls are built of their real faces only, no boxes: a
    box's hidden end face whose edge lies on a face the camera sees (the
    left part of the back wall's end at X 28 against the front, Z 0, over
    the breach) tied with that face on its column, and took a 1-pixel line
    the wall's height that the torches did not light (11-03). Faces the
    camera never sees (backs at Z -32, ends inside the wall) are left out;
    coplanar faces meet edge to edge."""
    x0, x1, top, z = DOOR_X0, DOOR_X1, DOOR_TOP, DOOR_DEPTH
    bx0, bx1, btop, bz = BREACH_X0, BREACH_X1, BREACH_TOP, BREACH_DEPTH
    zb, zf, y0, y1 = BACK_Z0, BACK_Z1, FLOOR, WALL_TOP
    for name in ('back_wall', 'breach', 'door'):   # the parts' numbers as before
        m.part(name)

    def front(name, xa, xb, ya, yb, zz=zf):      # a rectangle of a face toward +Z
        m.poly(name, [(xa, ya, zz), (xb, ya, zz), (xb, yb, zz), (xa, yb, zz)])

    def side(name, x, za, zb_, ya, yb, sign):    # a face toward +X (sign 1) or -X (-1)
        q = [(x, ya, za), (x, yb, za), (x, yb, zb_), (x, ya, zb_)]
        m.poly(name, q if sign > 0 else q[::-1])

    # (Where two faces the camera sees meet at a corner the first drawn takes
    # the corner's pixels: as the boxes did, the top, the cut end and the
    # left jambs before the face beside them, the right jambs after it.)
    # the back wall's top (over the left wall's end too), its cut end at X 224
    m.poly('back_wall', [(LEFT_X0, y1, zb), (LEFT_X0, y1, zf), (BACK_X1, y1, zf), (BACK_X1, y1, zb)])
    side('back_wall', BACK_X1, zb, zf, y0, y1, 1)
    # the breach's left jamb (lit), 12 deep
    side('back_wall', bx0, -bz, zf, y0, btop, 1)
    # the back wall's face (Z 0), round the breach and the door's opening
    # (2-unit reveals belong to the door); from the left wall's face (X -32)
    front('back_wall', LEFT_X1, bx0, y0, y1)
    front('back_wall', bx0, bx1, btop, y1)
    front('back_wall', bx1, x0 - 2.0, y0, y1)
    front('back_wall', x0 - 2.0, x1 + 2.0, top, y1)
    front('back_wall', x1 + 2.0, BACK_X1, y0, y1)
    side('door', x0, -z, zf, y0, top, 1)          # the door's left jamb (lit), under the wall over it
    front('door', x0 - 2.0, x0, y0, top)
    front('door', x1, x1 + 2.0, y0, top)
    # the right jambs, the wall's outer end at X -64
    side('back_wall', bx1, -bz, zf, y0, btop, -1)
    side('door', x1, -z, zf, y0, top, -1)
    side('back_wall', LEFT_X0, zb, zf, y0, y1, -1)
    # the recesses' backs 12 in (no lintel undersides: they face down, no
    # camera of the game sees them, and their front edges would tie on the
    # wall's face), the breach's floor, the door's threshold
    front('breach', bx0, bx1, y0, btop, -bz)
    m.poly('breach', [(bx0, FLOOR, -bz), (bx0, FLOOR, 0.0), (bx1, FLOOR, 0.0), (bx1, FLOOR, -bz)], SHEET)
    front('door', x0, x1, y0, top, -z)
    m.poly('door', [(x0, FLOOR, -z), (x0, FLOOR, 0.0), (x1, FLOOR, 0.0), (x1, FLOOR, -z)], SHEET)  # threshold
    for tip, cup in TORCHES:
        m.cyl('torch', tip, cup, 0.6, sides=10, r1=TORCH_R)
    for runs in FLAMES:
        for row, (a, b) in runs.items():
            m.poly('torch', [on_z(a - 0.5, row - 0.5, FLAME_Z), on_z(b + 0.5, row - 0.5, FLAME_Z),
                             on_z(b + 0.5, row + 0.5, FLAME_Z), on_z(a - 0.5, row + 0.5, FLAME_Z)],
                   SHEET | NOSHADOW)   # (the torches' light: no shadow of its own flame)
    # the left wall: its top, its face (X -32), its end behind the newel and
    # its outer side; none toward the back wall (Z 0), where it stands
    lx0, lx1, lz = LEFT_X0, LEFT_X1, LEFT_Z1
    m.poly('left_wall', [(lx0, WALL_TOP, 0.0), (lx0, WALL_TOP, lz), (lx1, WALL_TOP, lz), (lx1, WALL_TOP, 0.0)])
    side('left_wall', lx1, 0.0, lz, FLOOR, WALL_TOP, 1)
    front('left_wall', lx0, lx1, FLOOR, WALL_TOP, lz)
    side('left_wall', lx0, 0.0, lz, FLOOR, WALL_TOP, -1)
    # the platform's faces over the void: the curb (+X, X 224) and the
    # notch's front (+Z, Z 176), and the curb's top strip; not the notch's
    # -X and -Z sides (the grid's cells stand against them, and their top
    # edges tied on the floor: a row of their pixels along X 192 and Z 128)
    a, b, c, d, v = NOTCH_X0, PLAT_X1, NOTCH_Z0, PLAT_Z1, VOID
    m.poly('platform', [(b - 1.0, FLOOR, BACK_Z0), (b - 1.0, FLOOR, c), (b, FLOOR, c), (b, FLOOR, BACK_Z0)])
    side('platform', b, BACK_Z0, d, v, FLOOR, 1)
    front('platform', a, b, v, FLOOR, d)
    m.poly('floor_patch', [(a, FLOOR, c), (a, FLOOR, d), (b, FLOOR, d), (b, FLOOR, c)], SHEET)
    m.poly('floor_margin', [(LEFT_X1, FLOOR, 0.0), (LEFT_X1, FLOOR, MARGIN_Z1), (0.0, FLOOR, MARGIN_Z1),
                            (0.0, FLOOR, 0.0)], SHEET)


def stairwell(m):
    m.cyl('newel', (AX, NEWEL_FOOT, AZ), (AX, SKY, AZ), R_NEWEL, sides=32)
    for a, b, bottom in drum_spans():
        sector(m, 'drum', DRUM_RI, DRUM_RO, a, b, bottom, DRUM_TOP, step=3.0)
    # the helix, from the tread just above the bottom flight to the far wall's end
    k = -1
    while True:
        t0 = T_TH0 + k * T_DTH
        if t0 >= O_END:
            break
        t1 = min(t0 + T_DTH, O_END)
        sector(m, 'stair', R_NEWEL, tread_ro, t0, t1, FLOOR, T_Y0 + k * T_H, step=4.0)
        k += 1
    # the bottom flight: two shallower steps down to the landing
    t1 = T_TH0 - T_DTH
    for j in (1, 2):
        t0 = t1 - LOW_DTH
        sector(m, 'stair', R_NEWEL, tread_ro, t0, t1, FLOOR, T_Y0 - T_H - j * LOW_H, step=4.0)
        t1 = t0
    # the landing (the platform's top) from the bottom step round to the
    # corner's angle, inside the near wall; past that angle out to B
    th_c, th_j = front_angles()
    n = int(math.ceil((t1 - th_c) / 3.0))
    for i in range(n):
        a, b = th_c + (t1 - th_c) * i / n, th_c + (t1 - th_c) * (i + 1) / n
        m.poly('landing', [pol(R_NEWEL, a, LAND_Y), pol(R_NEWEL, b, LAND_Y), pol(T_RO, b, LAND_Y),
                           pol(T_RO, a, LAND_Y)], SHEET)
    foot(m, th_c)
    # far wall: from the cut (the strip, down past the picture's bottom)
    # round past the room's left wall, one solid down into the void (below the
    # treads only its cut face shows; as two pieces split at the floor, the
    # upper one's bottom tied on the cut face: a dark row across the strip)
    sector(m, 'tower_wall', far_ri, O_RO, O_FULL0, O_END, VOID, SKY, step=4.0)
    # near wall: the platform's front from the corner to column 111.5, then
    # the wall swinging in from it and on round the front to the strip's
    # edge, down into the void (its top: edge-on on the left, the landing's
    # paving at the front, a coping where it swings out); one outer face
    sector(m, 'front_wall', T_RO, plane_ro, th_c, th_j, VOID, LAND_Y, step=3.0, top_name='landing')
    sector(m, 'front_wall', T_RO, near_ro, th_j, th_j + JOIN_DTH, VOID, coping_y, step=1.0, top_name='landing')
    sector(m, 'front_wall', T_RO, front_ro, th_j + JOIN_DTH, O_FRONT_END, VOID, rim_y, step=3.0,
           top_name='landing')
    foot_step(m)


def foot(m, th_c):
    """The platform's right part at the landing's level, between B and the
    corner's angle: B from line (d) to the corner, the corner's radial in to
    the newel, the newel to the back edge (line (d), Z PLAT_Z0); B's riser
    down to the foot step (it faces away: never in the art), and the back
    edge's face where it is not under the drum."""
    y = LAND_Y
    nx = AX + math.sqrt(R_NEWEL ** 2 - (PLAT_Z0 - AZ) ** 2)
    q = pol(R_NEWEL, th_c, y)
    b0, c = (B0[0], y, B0[1]), (CORNER[0], y, CORNER[1])
    m.poly('landing', [b0, (nx, y, PLAT_Z0), q, c], SHEET)
    m.poly('landing', [(B0[0], FOOT_Y, B0[1]), b0, c, (CORNER[0], FOOT_Y, CORNER[1])])
    dx = AX + math.sqrt(DRUM_RO ** 2 - (PLAT_Z0 - AZ) ** 2)
    m.poly('landing', [(dx, FOOT_Y, PLAT_Z0), (dx, y, PLAT_Z0), b0, (B0[0], FOOT_Y, B0[1])])


def foot_step(m):
    """The foot step: on the floor in front of the platform's right end, its
    top FOOT_Y from line (d) (its back edge, Z FOOT_Z0) to B (where it runs
    under the platform), its +X end (the user's blue strip) at FOOT_X1 from
    the back edge to the corner."""
    y0, y1 = FLOOR, FOOT_Y
    z0 = FOOT_Z0
    x0 = B0[0] + (z0 - B0[1]) * (CORNER[0] - B0[0]) / (CORNER[1] - B0[1])   # B's line at the back edge
    x1, zc = FOOT_X1, CORNER[1]
    m.poly('foot_step', [(x0, y1, z0), (x1, y1, zc), (x1, y1, z0)])                      # top
    m.poly('foot_step', [(x1, y0, z0), (x1, y1, z0), (x1, y1, zc), (x1, y0, zc)])       # +X end
    m.poly('foot_step', [(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)])       # back
    m.poly('foot_step', [(x1, y0, zc), (x1, y1, zc), (x0, y1, z0), (x0, y0, z0)])       # under the platform


def build_parts(m):
    room(m)
    stairwell(m)


def build():
    m = Model('room83')
    build_parts(m)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map83.inc")
    m.write_inc(out, 83, "tools/models/room83.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
