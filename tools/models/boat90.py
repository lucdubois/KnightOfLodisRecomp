#!/usr/bin/env python3
"""Map 90: a small one-masted boat with its sail furled on the yard, moored at
a long plank pier on posts, over open water; map 117 shows THE SAME boat with
its sail set (build_boat(m, at=(0, 32, 320)) there).

    tools/models/boat90.py   writes src/kol_model_map90.inc
    build_boat(m, at)        the shared boat into a kit Model (map 117: at=(0, 32, 320))
    build_parts(m)           map 90's scene: the boat, the furled sail, gangplank, pier

Everything is measured on a flat mosaic of the map (360x470), whose projection is
    sx = (X - Z)/2 + 56,   sy = (X + Z)/4 - Y/2 + 151;
a map 117 pixel (x, y) is map 90's (x + 58, y + 2): "117" below marks such
coordinates. A pixel only gives a line of sight; depth comes from
  * contact: the waterline at Y 0, the deck at Y 48, the pier at Y 64;
  * the boat's symmetry about its keel plane X 352 (the middle of the deck's
    walkable cells 288..416, and the mast's foot): a far-rail point and the
    near-rail point of one station lie on one screen line of slope +1/2, half
    their distance across is the half-beam and their middle gives Z and Y.

The boat lies along Z, bow to -Z (the screen's upper right), stern to +Z.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model as _kit  # noqa: E402
Model, SHEET, CLOTH, LEE, THIN, TOP = _kit.Model, _kit.SHEET, _kit.CLOTH, _kit.LEE, _kit.THIN, _kit.TOP
# Every hanging part hangs in front of the hull: lifted, it opens onto the hull
# or deck behind it, and a rope painted on the deck's BG0 stays on the deck.
BG1 = _kit.BG1 | _kit.OVER
# Map 90 draws them on BG1 over the water, so a BG0 pixel one covers with no
# solid behind it is water, seen past it: left to it, 24 water pixels stood
# in the air (rope_stay's 19). Map 117 draws the boat on BG0 against the sky
# and passes BG1 alone.
BG1_OVER_WATER = BG1 | _kit.BG1ART

KX, KY = 56.0, 151.0
KEEL = 352.0        # the keel plane X (deck cells 288..416; mast foot, below)
DECK = 48.0         # grid height 3
RAIL_MID = 64.3     # the rail amidships, from the far/near pairs (flat from Z 47 to 126)
PIER = 64.0         # grid height 4


def pt(sx, sy, y):
    """The point at height y on the line of sight of pixel (sx, sy)."""
    d = 2.0 * (sx - KX)              # X - Z
    s = 4.0 * (sy + y / 2.0 - KY)    # X + Z
    return ((s + d) / 2.0, y, (s - d) / 2.0)


def pz(sx, sy, z):
    """The point at depth z on the line of sight of pixel (sx, sy)."""
    x = z + 2.0 * (sx - KX)
    return (x, 2.0 * ((x + z) / 4.0 + KY - sy), z)


def px(sx, sy, x):
    """The point at X x on the line of sight of pixel (sx, sy)."""
    z = x - 2.0 * (sx - KX)
    return (x, 2.0 * ((x + z) / 4.0 + KY - sy), z)


def w117(x, last_row):
    """The waterline under map 117's column x, whose last hull row is
    last_row: the boundary half a row below it (a pixel samples its corner)."""
    return pt(x + 58.0, last_row + 2.5, 0.0)


# ---------------------------------------------------------------------------
# The hull. Tier 0 is the rail from the stem head to the step at Z 146.
# (Z, half-beam at the rail's outer edge, at the cap's inner edge, height, water)
#
# Outer edge: pairs of the far rail's top outline (row = 206 - (x - 151)/2 from
# x 128 to 183, then (197,184) (209,180) (226,176) (246,172.7) (275,172)) with the near
# rail's outer line 117: (145.5,260) (175.5,245) (185,240) (192,236) (196,232)
# (200,228) (204,224) (208,220) (210,217) (213.5,208) (215.5,200) (217,190).
# Amidships both give half-beam 81.7, Y 64.3; forward the rail rises to 90 at
# the stem head and closes at 45 degrees in plan (so the far rail's last run is
# level on screen, row 172, and the near rail's nearly upright, x 216).
# Inner edge: the dark line 117 row: x 253:140 244:157.5 235:174.5 225:190
# 215:200 207:203 199:205 190:206 181:206 177:206: the near rail's inner edge
# over the deck; it gives half-beam 62 amidships. The far cap's inner edge
# agrees (row 214-217 at x 150: the light's end), so the cap is about 19 wide.
# Under station 6.9 the near side's silhouette bulges 1.4 px out of the straight line
# from rail to water (117 (205,230) (203,236)): _BULGE.
# Water: the hull's lowest row in map 117 where it meets the ripples: X 411
# from Z 137 to 46, then round the bow; forward of Z -5 it is hidden behind the
# flared side and only has to stay inside the silhouette.
_RAIL0 = [
    (146.0, 81.7, 63.7, RAIL_MID, w117(129.5, 288)),
    (130.0, 81.7, 62.3, RAIL_MID, w117(138, 284)),
    (126.0, 81.7, 62.3, RAIL_MID, w117(140, 283)),
    (95.0, 81.7, 61.8, RAIL_MID, w117(156, 275.5)),
    (78.0, 81.7, 61.0, RAIL_MID, w117(164.5, 271)),
    (60.0, 81.5, 60.3, 64.5, w117(173.5, 266.5)),
    (47.0, 81.1, 60.2, 64.9, w117(180, 263)),
    (33.3, 78.7, 60.1, 67.3, w117(186, 261)),
    (20.0, 76.0, 59.5, 69.9, w117(191.5, 255.5)),
    (13.0, 74.6, 58.0, 71.1, w117(194.5, 251)),
    (6.9, 73.1, 56.5, 72.3, w117(197, 247.5)),
    (-4.9, 68.9, 51.5, 75.1, (386.0, 0.0, -9.0)),
    (-15.0, 63.0, 44.5, 78.3, (385.0, 0.0, -17.0)),
    (-24.4, 56.4, 37.5, 80.9, (380.0, 0.0, -23.0)),
    (-33.6, 49.6, 30.0, 83.5, (376.0, 0.0, -29.0)),
    (-42.2, 42.2, 22.0, 85.8, (372.0, 0.0, -35.0)),
    (-51.0, 35.0, 13.0, 87.6, (368.0, 0.0, -40.0)),
    (-59.9, 27.9, 4.0, 88.8, (364.0, 0.0, -45.0)),
    (-68.0, 20.0, 0.0, 89.1, (360.0, 0.0, -50.0)),
    (-76.0, 12.0, 0.0, 89.5, (357.0, 0.0, -54.0)),
    (-84.0, 4.0, 0.0, 89.9, (354.0, 0.0, -58.0)),
    (-88.0, 0.0, 0.0, 90.0, (352.0, 0.0, -60.0)),
]
_BULGE = {6.9: (203.3, 236.5, 30.0), 13.0: (200.0, 240.5, 30.0)}
# The near rail runs in to X 430 at Z 146, hump 2's corner, where the raised rail's
# outer face meets it; forward of Z -40 a half-pixel out (its last columns are 117 x 218).
_NEAR_OUT = {146.0: -3.7, 130.0: -1.2, -42.2: 1.0, -51.0: 1.0, -59.9: 1.0, -68.0: 1.0}
# The far rail's top outline where it curves is a stair of runs: 0.4 higher, so each
# run's first column keeps its row (row 178 from x 213, 177 from 217, 176 from 221).
_FAR_UP = 0.4
APEX = (KEEL, 90.0, -62.0)      # the cap's inner edge meets at the bow: 117 (206, 177)

# Tier 1, Z 146..171, Y 86.3: the raised aft rail begins with an end line along
# X, far 117 (60,210)-(69,215) [X 270.5..288], near 117 (133,247)-(140,250.5) [X
# 415.7..430]: both give Z 146, Y 86.3 (hump 2 of the near side).
T1_Y, T1_Z0, T1_Z1 = 86.3, 146.0, 171.0
T1_FAR_O, T1_FAR_I, T1_NEAR_O, T1_NEAR_I = 270.3, 288.0, 430.0, 415.5
T0_END_I = 288.3

# Tier 2 and the stern, near side round to the far side: the rail's outer and
# inner edges by pixel and height (a loop whose heights come from the far/near
# pairs of the same stations). Each row: outer (x, y, Y), inner (x, y, Y), water.
# It begins at Z 171: far 117 (48,208)-(55,211.5) and near 117 (120,244.5)-(127,248)
# (hump 1's end) give Z 171, Y 103. The stern is a nearly flat transom along X at
# Z ~266, half-width ~32, Y ~125 (outer 117 (25,230) to (55,250)).
_STERN = [
    ((127, 248, 103), (120, 244.5, 103), w117(115, 293)),      # hump 1's corner, Z 171
    ((119, 254.5, 103), (110, 250, 103), w117(107, 292)),      # the row-250 line and the band's lower edge
    ((109, 255, 105), (100, 250, 105), w117(100, 291)),
    ((99, 255, 108), (90, 250, 108), w117(96, 290)),
    ((89, 255.5, 110), (80, 250.5, 109), w117(86, 290)),
    ((76, 255.5, 111), (72, 249, 112), w117(78, 288)),
    ((65, 252.5, 117), (60, 243.5, 118), w117(73.5, 286)),
    ((55, 250, 123), (55, 236, 120), w117(69, 284)),
    ((47, 245.5, 125), (48.5, 232.5, 122), w117(68, 283.5)),   # the plate's peak
    ((42.5, 242, 125.5), (44.5, 235.5, 124), w117(66.5, 284)),
    ((38, 239, 126), (42, 233, 124), w117(65, 284.5)),
    ((33, 236, 128), (38, 231, 125), w117(63, 286)),
    ((28, 232, 128), (33, 228.5, 125), w117(62, 286)),
]
_STERN_FAR = [
    ((24.7, 230, 125), (35, 225, 120), None),    # the far corner of the transom: the silhouette's post, below
    ((25, 227.5, 115), (36.5, 223.5, 116), (330.0, 0.0, 208.0)),
    ((26.5, 224.7, 110), (38, 222, 112), (322.0, 0.0, 205.0)),
    ((30, 220.5, 108), (41.5, 220.5, 109), (315.0, 0.0, 203.0)),
    ((34, 217, 105), (45, 219, 106), (300.0, 0.0, 195.0)),
    ((38, 214, 102), (48.5, 217, 104), (290.0, 0.0, 185.0)),
    ((42, 211, 102), (52, 215, 103), (288.0, 0.0, 178.0)),
    ((48, 208, 103), (55, 211.5, 103), (288.0, 0.0, 171.0)),
]
# The stern's far edge from the rail to the water: the silhouette's last row per
# column 117: x 27:249 30:250 31:254 37:259 38:264 48:273 56:282 61:286, upright under
# the corner, two jogs: a line of points by pixel and a height falling along it, so it
# closes on the water.
_STERN_POST = [(26, 240, 105), (26.5, 249.5, 90), (30, 250.7, 84), (31, 254.5, 80), (37.5, 259.8, 66), (38, 264.5, 58), (48, 273.5, 36),
               (56, 282.5, 14), (61, 286.5, 0)]


def p117(x, y, h):
    return pt(x + 58.0, y + 2.0, h)


def _strip(m, name, rows, flags, want, zero):
    """m.strip with the rows' order chosen so its normals pass want(n); a corner
    with no normal (the columns meeting at a point) takes zero."""
    nx = ny = nz = 0.0
    for r0, r1 in zip(rows, rows[1:]):
        for j in range(len(r0) - 1):
            ux, uy, uz = r0[j + 1][0] - r0[j][0], r0[j + 1][1] - r0[j][1], r0[j + 1][2] - r0[j][2]
            vx, vy, vz = r1[j][0] - r0[j][0], r1[j][1] - r0[j][1], r1[j][2] - r0[j][2]
            nx += uy * vz - uz * vy
            ny += uz * vx - ux * vz
            nz += ux * vy - uy * vx
    if not want((nx, ny, nz)):
        rows = rows[::-1]
    first = len(m.verts)
    m.strip(name, rows, flags)
    for i in range(first, len(m.verts)):
        v = m.verts[i]
        if v[3] * v[3] + v[4] * v[4] + v[5] * v[5] < 0.5:
            m.verts[i] = (v[0], v[1], v[2]) + zero


_UP = lambda n: n[1] > 0.0                  # noqa: E731
_TO_CAMERA = lambda n: sum(n) > 0.0         # noqa: E731
_TO_FAR = lambda n: n[0] < 0.0              # noqa: E731


def _under(p):
    """The far skin's top, hidden behind the rail: just under the cap's outer edge, so
    the hidden face never ties with the cap along the silhouette."""
    return (p[0], p[1] - 0.6, p[2])


def _sample(poly, f):
    """The polyline (top to bottom, Y falling) at height fraction f of its fall."""
    y = poly[0][1] + (poly[-1][1] - poly[0][1]) * f
    for a, b in zip(poly, poly[1:]):
        if y >= b[1]:
            t = (a[1] - y) / (a[1] - b[1]) if a[1] != b[1] else 0.0
            return (a[0] + (b[0] - a[0]) * t, y, a[2] + (b[2] - a[2]) * t)
    return poly[-1]


# The deck's far edge (Y 48), read as the art's change from bulwark to deck
# planks: X 290 from Z 158 down to 42 (rows: (150,225) (158,221) (170,215) (179,210.5)),
# (194,210) (200,208) (204,206) [X 304-306], then the row 204.5 from x 211 to
# 245: X + Z 310, the far bow's diagonal, to the corner under the bow 117 (198,204).
# The seam across the deck 117 (66,234)-(98,250) lies at Z 150 on Y 48 and starts at X 286.
_DECK_FAR = [(357.0, -43.0), (310.0, 0.0), (306.0, 10.0), (306.0, 18.0), (304.0, 28.0), (290.0, 42.0), (290.0, 158.0)]
# Its near edge, hidden under the near rail (117 (203,207) forward of that the
# visible end): runs from the same corner to X 414, under the cap.
_DECK_NEAR = [(357.0, -43.0), (369.0, -41.0), (379.0, -24.0), (392.0, -5.0), (400.0, 7.0), (409.0, 20.0), (412.0, 33.0),
              (414.0, 146.0), (414.0, 171.0), (410.0, 183.0), (394.0, 195.0)]

# The mast on the keel plane, its axis column 187.5 so X - Z = 263: Z 89. Its foot on
# the deck at row 237 (the coil's ring 236-241). Column by column (flat.png):
#   rows 98-236: 184-191 (core 186-189 with % shades, or # outlines at 183, 192 low down);
#   rows 30-83: core 186-189, outlines 185, 190;
#   rows 26-29: the band, 184-191;
#   rows 16-25: the head, core 186-189; its top row 16.
MAST_Z = 89.0
MAST_R, MAST_UP_R, MAST_BAND_R, MAST_HEAD_R = 5.3, 3.0, 5.1, 3.0
MAST_STEP = 330.0           # under the parrel (row 96)
# The foot, rows 202-237: 10 wide on the deck's BG0 (outlines at x 183 and 192):
# narrower, those outline columns were stretched as the slate fan beside the
# mast with the camera moved.
MAST_FOOT_R, MAST_FOOT_TOP = 6.7, 118.5
MAST_BAND = (460.0, 474.0)  # rows 31-24
MAST_TOP = 490.0            # row 16
# The yard, level along X in the mast's plane: its centre row = 88.1 + (x - 187.75)/2,
# so Y 346; tips x 118-121 and 260-263 (X 215 and 500); 5 rows thick with its outlines.
YARD_Y = 346.0
YARD_R = 3.0
YARD_X = (215.0, 500.0)
# The parrel, the rope collar where yard and mast meet: columns 182-193, rows 83-97.
PARREL_R = 8.0
PARREL_Y = (350.0, 335.0)
# The bow post, on the keel plane, its core by columns (flat pixels, top-bottom rows):
# x 278 169-181, 282 166-176, 284 164-173, 286 163-171, 290 160-167, 298 154-160,
# 301 152-157: the axis (x, row, radius; a cylinder's depth at this slope is 0.88 r).
# The rope wound round it, its pale pixels x 292-300, rows 154-160.
POST_AXIS = [(272.0, 180.8, 8.8), (284.0, 168.3, 5.9), (294.0, 159.8, 4.3), (302.5, 153.0, 3.2)]
POST_LASHING = ((293.0, 160.0), (299.5, 155.6), 6.2)

# The fixed rigging, centre lines (flat pixels; x, y) fitted to the ropes' pale
# core pixels; the same in both maps at the offset. Depth: the lifts in the yard's
# plane Z 89 (each ends on the yard's row there); the forestay on the keel plane; the
# three running aft from their top to their foot on the stern's near rail (the rope's
# end at the rail's inner edge, at the rail's height there: X 361-380, Z 240-247).
ROPE_R = 1.7
ROPES_IN_PLANE = {
    "rope_lift_far_tip": [(185.5, 30.0), (121.0, 54.2)],     # y = 32 + 0.375 (180 - x): mast head to the far yard tip
    "rope_lift_far": [(186.0, 31.0), (155.5, 71.0)],         # to the yard at x 155
    "rope_lift_near": [(191.0, 32.0), (226.0, 107.0)],       # x = 193 + 0.47 (y - 36)
    "rope_lift_near_tip": [(191.0, 31.5), (258.5, 122.0)],   # x = 195 + 0.738 (y - 36)
    "rope_parrel_tie": [(184.6, 76.0), (184.6, 85.0)],       # beside the mast over the parrel
}
# The forestay, mast head to the bow post's tip: on the keel plane, a little sagging:
# it passes from left of the near tip's lift (a dim line beside it, rows 34-60) under it
# (rows 61-79, its own pixels not drawn) to its right (rows 84-152, x = 232 + 0.9 (y - 84)).
ROPE_STAY = [(190.0, 33.0), (199.0, 44.0), (205.5, 53.0), (211.0, 60.0), (218.0, 68.0), (228.0, 80.0), (235.0, 88.0),
             (242.0, 96.0), (268.0, 124.0), (295.0, 152.0), (298.0, 155.0)]
# Top pixel (x, y) in the plane Z 89, foot pixel and height: mast head to the stern
# x = 183 - 0.33 (y - 44); far yard tip to the stern x = 121 - 0.043 (y - 57); near yard
# tip to the stern y = 133 + 0.897 (255 - x).
ROPE_AFT = {
    "rope_mast_stern": ((187.3, 31.0), (117.5, 245.0, 118.0)),
    "rope_far_yard_stern": ((121.0, 57.0), (113.0, 243.0, 120.0)),
    "rope_near_yard_stern": ((261.5, 127.2), (126.0, 249.0, 112.0)),
}

# ---------------------------------------------------------------------------
# Map 90's own: the furled sail, four swags under the yard: per column (x, the
# first and last row of its art under the yard's lower outline, a water gap of a
# row between them in places).
_SWAGS = [
    ("sail_swag_1", [(124, 58, 65.5), (126, 59, 68.5), (128, 60, 70.5), (130, 61, 72.5), (132, 64, 73.5), (134, 65, 74.5), (136, 66, 75.5),
                     (138, 67, 76.5), (140, 67.5, 76.5), (142, 67, 77.5), (146, 69, 77.5), (150, 71, 76.5), (152, 72, 75.5), (154.5, 73, 75.5)]),
    ("sail_swag_2", [(155, 73.5, 78.5), (156, 74, 81.5), (157, 74, 83.5), (159, 75, 85.5), (161, 76, 87.5), (163, 77, 88.5), (165, 81, 89.5), (167, 82, 90.5),
                     (169, 83, 91.5), (171, 82, 92.5), (172, 83, 92.5), (175, 83, 92.5), (179, 85, 92.5), (181, 86, 91.5), (183, 87, 91.5)]),
    ("sail_swag_3", [(195, 93, 98.5), (196, 94, 101.5), (197, 94, 103.5), (198, 95, 104.5), (200, 96, 106.5), (203, 97.5, 108.5), (204, 100, 109.5), (206, 101, 110.5),
                     (208, 102, 111.5), (210, 103, 111.5), (212, 103, 112.5), (214, 103, 112.5), (218, 105, 112.5), (220, 106, 111.5), (222, 107, 110.5), (224, 108, 109.5)]),
    ("sail_swag_4", [(227, 109, 114.5), (228, 110, 117.5), (229, 110, 119.5), (230, 111, 120.5), (232, 112, 122.5), (235, 113.5, 124.5), (236, 116, 125.5), (238, 117, 126.5),
                     (240, 118, 127.5), (242, 118.5, 127.5), (244, 118.5, 128.5), (250, 121, 128.5), (252, 122, 127.5), (254, 123, 126.5), (255.5, 123.5, 125.5)]),
]
SWAG_BULGE = 8.0

# The gangplank: near edge (214,248)-(247,264.5), far edge (228,239.5)-(265,257),
# at Y 66: X 416..486, Z 70..102 (the near rail's inner edge X 416, on the grid's
# step from height 3 to 4 at X 416, and one cell wide on the pier's z 2).
PLANK = ((416.0, 62.0, 70.0), (486.0, 66.0, 102.0))

# The pier: far edge at X 448 (top row 371 - x/2, grid x 14), near edge X 527
# (c = y + x/2 = 410 at the plank ends), back edge (258,243)-(298,261) at Z 42; its
# near side's lowest row 416 - x/2, so 12 thick at X 527. Runs off the art at the
# left, past the grid's z 18.
PIER_BOX = ((448.0, 52.0, 42.0), (527.0, PIER, 640.0))
# Trestle bents every 128 in Z: the art repeats exactly at (-64, +32). All numbers
# for the bent with z0 = 346 (columns 148-173, rows 325-373):
#   near pile: core columns 150-157 (rows 365-371), last row 373: X 539, Z 343.5;
#   far pile: core 163-170 (rows 358-366), last row 367: X 540, Z 318.5;
#   lashings on them, core 149-158, rows 359-364 / 162-171, rows 351-356: Y 15..27;
#   uprights over them, core 150-154, top row 331 and 163-167, top row 325: Y 77;
#   the brace between the piles: a light band (150,355)-(163,348.5): Y 38 along Z;
#   the beam across on the far pile: silhouette (162,338) (173,343.5) (173,347.5)
#   (167,350.5) (156,345), its lower edge's shade to row 348: X 527 (the deck's edge)
#   ..555, Z 321..333, Y 42..53;
#   a bracket from the deck's lower edge to the near upright, seen left of it at
#   (147-149,343-345).
BENT_Z = (90.0, 218.0, 346.0, 474.0, 602.0)
PILE_R, PILE_TOP = 5.5, 44.0
LASH_R, LASH_Y = 7.0, (15.0, 27.0)
UPRIGHT_R, UPRIGHT_TOP = 3.9, 77.0
# The stubs on the far edge: the same stations; top outlines x 183-187 row 271, 119-123
# row 303, 106-110 row 309, 55-59 row 335, 42-46 row 341: X 452, Y 81. They run on down
# behind the deck. At the left foot of the second of each pair a little block
# rises 2 rows over the far edge (x 38-41 rows 349-350).
STUB_X, STUB_R, STUB_TOP = 452.0, 4.3, 81.0
STUB_Z = (194.0, 220.0, 322.0, 348.0, 450.0, 476.0)
# The separate mooring posts in the water: foot column, its last row, top column,
# top outline's row, radius at the foot and the top.
MOORING = [
    ("post_1", 247.0, 316.0, 246.6, 303.0, 4.5, 5.7),    # a flat top: rim rows 303-307
    ("post_3", 200.0, 348.0, 200.5, 335.0, 4.5, 5.7),
    ("post_2", 263.6, 323.0, 265.3, 303.0, 4.8, 3.0),    # leaning, its head narrow
    ("post_4", 63.2, 411.0, 61.8, 391.0, 5.0, 2.6),
]


def build_boat(m, at=(0.0, 0.0, 0.0), hang=BG1, body=None):
    """The shared boat: hull, deck, mast, yard, fixed rigging, moved by at."""
    # hang: the ropes' flags. body: the post, mast, parrel and yard (hang unless
    # given; map 117, all on BG0, passes 0: solids).
    body = hang if body is None else body
    ax, ay, az = at

    def P(p):
        return (p[0] + ax, p[1] + ay, p[2] + az)

    # The rail's stations, far side from the stem head to the stern, then round
    # to the near side and back. Each column: outer, inner, the skin's polyline
    # from the rail's outer edge to the water.
    far0 = []
    near0 = []
    for z, bo, bi, y, w in _RAIL0:
        no = (KEEL + bo + _NEAR_OUT.get(z, 0.0), y, z)
        ni = (KEEL + bi, y, z) if bi > 0.0 else APEX
        fo = (KEEL - bo, y + (_FAR_UP if -45.0 < z < 35.0 else 0.0), z)
        fi = (KEEL - bi, y, z) if bi > 0.0 else APEX
        far_w = (2.0 * KEEL - w[0], 0.0, w[2])
        mid = [p117(*_BULGE[z])] if z in _BULGE else []
        near0.append((no, ni, [no] + mid + [w]))
        far0.append((fo, fi, [_under(fo), far_w]))
    far0.reverse()    # stem head first
    # Tier 1.
    f_o = (T1_FAR_O, T1_Y, T1_Z0 + 0.2)
    f_i = (T0_END_I, T1_Y, T1_Z0)     # tier 0's end: at 288, 146.2 a slit opened, 1 px, x 127
    f_w0 = far0[-1][2][-1]
    far1 = [(f_o, f_i, [_under(f_o), f_w0]),
            ((T1_FAR_O, T1_Y, T1_Z1), (T1_FAR_I - 3.0, T1_Y, T1_Z1), [_under((T1_FAR_O, T1_Y, T1_Z1)), (288.0, 0.0, 171.0)])]
    n_o = (T1_NEAR_O, T1_Y, T1_Z0 + 0.2)
    n_i = (T1_NEAR_I, T1_Y, T1_Z0 + 0.2)
    n_w0 = near0[0][2][-1]
    n_o1 = (T1_NEAR_O, T1_Y, T1_Z1)
    near1 = [(n_o, n_i, [n_o, n_w0]),
             (n_o1, (T1_NEAR_I, T1_Y, T1_Z1), [n_o1, w117(115, 293)])]
    # Tier 2 and the stern, near to far.
    stern = []
    for o, i, w in _STERN:
        O, I = p117(*o), p117(*i)
        stern.append((O, I, [O, w]))
    for k, (o, i, w) in enumerate(_STERN_FAR):
        O, I = p117(*o), p117(*i)
        if w is None:
            stern.append((O, I, [O] + [p117(*q) for q in _STERN_POST]))
        else:
            stern.append((O, I, [_under(O), w]))

    # The skin: rows from the water up, columns far side, stern, near side.
    cols = far0 + far1 + stern[::-1] + near1[::-1] + near0
    NR = 8
    rows = []
    for r in range(NR + 1):
        f = 1.0 - r / NR
        rows.append([P(_sample(c[2], f)) for c in cols])
    m.strip("hull", rows)

    # The rail's top: a cap per tier.
    def cap(name, section):
        _strip(m, name, [[P(c[0]) for c in section], [P(c[1]) for c in section]], TOP, _UP, (0.0, 1.0, 0.0))
    cap("rail_cap", far0)
    cap("rail_cap", near0)
    cap("rail_cap", far1)
    cap("rail_cap", near1)
    cap("rail_cap", stern)

    # The inner faces seen over the deck: the far bulwark, stern to bow, and the
    # near one's forward end (the art's strip beside the near rail at the bow).
    def foot_far(p):
        """Under p on the screen, on the deck's far edge: the same X - Z. Amidships
        the bulwark is upright (the deck's edge on the cap's inner edge's X)."""
        if p[2] >= 47.0:
            return (p[0], DECK, p[2])
        d = p[0] - p[2]
        for (x0, z0), (x1, z1) in zip(_DECK_FAR, _DECK_FAR[1:]):
            d0, d1 = x0 - z0, x1 - z1
            if d1 <= d <= d0:
                t = (d - d0) / (d1 - d0) if d0 != d1 else 0.0
                return (x0 + (x1 - x0) * t, DECK, z0 + (z1 - z0) * t)
        return (_DECK_FAR[0][0], DECK, _DECK_FAR[0][1]) if d > _DECK_FAR[0][0] - _DECK_FAR[0][1] else (p[0], DECK, p[2])
    # (apex, stations) on tier 0, far side
    far_inner = [c[1] for c in far0 if c[1] != APEX]
    far_inner = [APEX] + far_inner
    _strip(m, "bulwark", [[P(p) for p in far_inner], [P(foot_far(p)) for p in far_inner]], LEE, _TO_CAMERA, (0.7, 0.0, 0.7))
    t1 = [c[1] for c in far1]
    _strip(m, "bulwark", [[P(p) for p in t1], [P((p[0], DECK, p[2])) for p in t1]], LEE, _TO_CAMERA, (1.0, 0.0, 0.0))
    aft = [c[1] for c in stern[13:]]   # the far side of the stern loop
    # (Its first foot stops at Y 100: on the deck it poked out at 93, 261-263.)
    _strip(m, "bulwark", [[P(p) for p in aft], [P((p[0], 100.0 if i == 0 else DECK, p[2])) for i, p in enumerate(aft)]], LEE, _TO_CAMERA, (1.0, 0.0, 0.0))
    near_bow = [c for c in near0 if c[0][2] <= 33.4 and c[1] != APEX]
    near_bow_foot = {33.3: (412.0, 33.3), 20.0: (409.0, 20.0), 13.0: (405.0, 13.0), 6.9: (400.0, 6.9), -4.9: (392.0, -4.9), -15.0: (385.0, -15.0),
                     -24.4: (379.0, -24.4), -33.6: (373.7, -33.6), -42.2: (369.0, -41.0), -51.0: (363.0, -42.0), -59.9: (358.5, -42.7)}
    row_t = [c[1] for c in near_bow] + [APEX]
    row_b = [(near_bow_foot[c[0][2]][0], DECK, near_bow_foot[c[0][2]][1]) for c in near_bow] + [(357.0, DECK, -43.0)]
    _strip(m, "bulwark", [[P(p) for p in row_t], [P(p) for p in row_b]], LEE, _TO_FAR, (-0.7, 0.0, 0.7))

    # The deck: two rows, the far edge and the near edge, stem to stern.
    far_edge = [(foot_far(p)[0], foot_far(p)[2]) for p in far_inner]
    far_edge += [(p[0], p[2]) for p in t1] + [(p[0], p[2]) for p in aft[::-1] if 171.5 < p[2] < 203.0]
    n = len(far_edge)
    near_edge = []
    L = _DECK_NEAR
    for i in range(n):
        t = i / (n - 1) * (len(L) - 1)
        k = min(int(t), len(L) - 2)
        f = t - k
        near_edge.append((L[k][0] + (L[k + 1][0] - L[k][0]) * f, L[k][1] + (L[k + 1][1] - L[k][1]) * f))
    _strip(m, "deck", [[P((x, DECK, z)) for x, z in near_edge], [P((x, DECK, z)) for x, z in far_edge]], 0, _UP, (0.0, 1.0, 0.0))

    # The bow post.
    for (x0, y0, r0), (x1, y1, r1) in zip(POST_AXIS, POST_AXIS[1:]):
        m.cyl("bow_post", P(px(x0, y0, KEEL)), P(px(x1, y1, KEEL)), r0, r1=r1, sides=10, flags=body)
    (x0, y0), (x1, y1), r0 = POST_LASHING
    m.cyl("bow_post", P(px(x0, y0, KEEL)), P(px(x1, y1, KEEL)), r0, sides=10, flags=body)

    # The mast and what is on it.
    c = (KEEL, MAST_Z)
    m.cyl("mast", P((c[0], DECK, c[1])), P((c[0], MAST_FOOT_TOP, c[1])), MAST_FOOT_R, sides=12, flags=body)
    m.cyl("mast", P((c[0], MAST_FOOT_TOP, c[1])), P((c[0], MAST_STEP, c[1])), MAST_R, sides=12, flags=body)
    m.cyl("mast", P((c[0], MAST_STEP, c[1])), P((c[0], MAST_BAND[0], c[1])), MAST_UP_R, sides=10, flags=body)
    m.cyl("mast", P((c[0], MAST_BAND[0], c[1])), P((c[0], MAST_BAND[1], c[1])), MAST_BAND_R, sides=10, flags=body)
    m.cyl("mast", P((c[0], MAST_BAND[1], c[1])), P((c[0], MAST_TOP, c[1])), MAST_HEAD_R, r1=2.6, sides=10, flags=body)
    m.cyl("parrel", P((c[0], PARREL_Y[1], c[1])), P((c[0], PARREL_Y[0], c[1])), PARREL_R, sides=10, flags=body)
    m.cyl("yard", P((YARD_X[0], YARD_Y, MAST_Z)), P((YARD_X[1], YARD_Y, MAST_Z)), YARD_R, sides=8, flags=body)

    # The fixed rigging.
    # (ASDRAWN: a line a pixel or two wide in a cast shadow only reads as a dark
    # scratch or a broken rope.)
    F = hang | THIN | _kit.ASDRAWN
    for name, pts in ROPES_IN_PLANE.items():
        q = [pz(x, y, MAST_Z) for x, y in pts]
        for u, v in zip(q, q[1:]):
            m.cyl(name, P(u), P(v), ROPE_R, sides=6, flags=F)
    q = [px(x, y, KEEL) for x, y in ROPE_STAY]
    for u, v in zip(q, q[1:]):
        m.cyl("rope_stay", P(u), P(v), ROPE_R, sides=6, flags=F)
    # Each straight from its top to its foot: in the air to its ends (kol_focus
    # lifts a rope's pixels off whatever it passes in front of).
    for name, (top, foot) in ROPE_AFT.items():
        m.cyl(name, P(pz(top[0], top[1], MAST_Z)), P(pt(*foot)), ROPE_R, sides=6, flags=F)


def build_parts(m):
    """Map 90's scene."""
    build_boat(m, hang=BG1_OVER_WATER)

    # The furled sail: four swags hanging from the yard's axis, bellying a
    # little toward the camera, their art's lower outline on the columns' rows.
    for name, edge in _SWAGS:
        x0, x1 = edge[0][0], edge[-1][0]
        rows = []
        for v in (0.0, 0.34, 0.67, 1.0):
            row = []
            for x, top, last in edge:
                u = (x - x0) / (x1 - x0)
                row.append(pz(x, top + (last - top) * v, MAST_Z + SWAG_BULGE * v * math.sin(math.pi * u)))
            rows.append(row)
        _strip(m, name, rows, BG1_OVER_WATER | CLOTH | SHEET, _TO_CAMERA, (0.0, 0.0, 1.0))

    # The gangplank.
    m.box("gangplank", PLANK[0], PLANK[1], BG1)

    # The pier.
    m.box("pier_deck", PIER_BOX[0], PIER_BOX[1], BG1)
    for z0 in BENT_Z:
        for x, dz, ux, udz in ((539.0, -2.5, 537.0, -1.0), (540.0, -27.5, 538.0, -26.0)):
            z = z0 + dz
            m.cyl("pier_pile", (x, 0.0, z), (x, PILE_TOP, z), PILE_R, sides=10, flags=BG1)
            m.cyl("pier_lashing", (x, LASH_Y[0], z), (x, LASH_Y[1], z), LASH_R, sides=10, flags=BG1)
            m.cyl("pier_upright", (ux, PILE_TOP - 6.0, z0 + udz), (ux, UPRIGHT_TOP, z0 + udz), UPRIGHT_R, sides=10, flags=BG1)
        m.cyl("pier_lashing", (538.0, 32.0, z0 - 2.0), (538.0, 44.0, z0 - 2.0), 6.6, sides=10, flags=BG1)
        m.cyl("pier_brace", (540.0, 38.0, z0 + 7.0), (540.0, 38.0, z0 - 24.0), 4.0, sides=6, flags=BG1)
        m.box("pier_beam", (527.0, 42.0, z0 - 25.0), (555.0, 53.0, z0 - 13.0), BG1)
        m.box("pier_bracket", (527.0, 47.5, z0 - 11.0), (540.0, 52.0, z0 - 0.5), BG1)
    for z in STUB_Z:
        m.cyl("pier_stub", (STUB_X, 0.0, z), (STUB_X, STUB_TOP, z), STUB_R, sides=10, flags=BG1)
    for z in STUB_Z[1::2]:
        m.box("pier_stub", (448.0, 60.0, z), (452.0, 68.0, z + 10.0), BG1)
    for name, fc, fr, tc, tr, r0, r1 in MOORING:
        # A round foot's lowest row is .354 r under its centre, a top's first row as much over.
        row0 = fr + 0.5 - 0.354 * r0
        rowt = tr + 0.5 + 0.354 * r1
        foot = pt(fc, row0, 0.0)
        s_ = foot[0] + foot[2]
        top_d = 2.0 * (tc - KX)
        h = 2.0 * (row0 - rowt)
        m.cyl(name, foot, ((s_ + top_d) / 2.0, h, (s_ - top_d) / 2.0), r0, r1=r1, sides=10, flags=BG1)


if __name__ == "__main__":
    m = Model("map90")
    build_parts(m)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map90.inc")
    m.write_inc(out, 90, "tools/models/boat90.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
