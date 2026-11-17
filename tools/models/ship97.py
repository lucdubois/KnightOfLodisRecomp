#!/usr/bin/env python3
"""Map 97's ship at the quay, as a 3D model (README "The ship on map 97").

    tools/models/ship97.py                  writes src/kol_model_map97.inc
    tools/models/ship97.py fit FLAT INFO OUT [x0 y0 x1 y1]
                                            the model over a flat frame
                                            (KOL_FOCUS_DUMP's .info)

Measured on the flat frame of states/GF (the user's slot 3) at 384x216: the
projection puts each unit's ring at its feet (deck Y 16, quay Y 48). The ship
lies along X, bow at +X, the quay on its -Z side. Where the art cheats (the
stern is drawn head on: its balustrade is a flat line across the screen, its
post upright), the model follows the pixels, not physics: a screen row or
column measured on that frame is turned into a height or a depth with the
frame's projection (KX, KY).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, SHEET, CLOTH, BG1, ROPES, LEE, THIN, TOP, overlay, lerp, add  # noqa: E402

# The fit frame's projection: sx = (X - Z) / 2 + KX, sy = (X + Z) / 4 - Y / 2 + KY
# (kx + ox, ky + oy of its .info).
KX, KY = 208.0, -108.0


def y_at_row(x, z, row):
    """The height that puts the point (x, ?, z) on screen row `row`."""
    return 2.0 * ((x + z) / 4.0 + KY - row)


def z_at_col(x, col):
    """The depth that puts a point at X on screen column `col`."""
    return x - 2.0 * (col - KX)


ZC = 565.0          # the keel's line
DECK = 16.0         # the main deck (units stand on it)
QUAY = 48.0         # the quay's top
RAIL = 56.0         # the bulwarks' top along the waist
CASTLE = 80.0       # the stern castle's deck (its front wall's top)
STERN_RAIL = 98.0   # the post's foot (row 100); above it the profile is the post's
# The balustrade's top on the screen, column by column (the art's top
# outline: it steps, and row 52 over the corner is water, over 134-145 the
# wall behind: the user, 2026-10-16); past 151 it turns down the far side
# (RAIL_TOP).
TRANSOM_TOP = [(60.0, 53.0), (87.6, 53.0), (88.4, 52.0), (133.6, 52.0), (134.4, 53.0), (145.6, 53.0),
               (146.4, 54.0), (151.0, 54.0)]
X_CORNER = 378.0    # the transom's corners at the rail: the post is column 72.5
CORNER = 2.0        # the transom's corners' rounding: nearly flat, or its back pokes into the
                    # notch of water the art leaves under the counter (rows 118-122)
X_CASTLE = 446.0    # the castle's front wall (was 467: the striped band under the castle deck's
                    # edge is this wall, not deck; the user's painting, 2026-10-26)
X_CLOSE = 630.0     # the rail holds its beam to here, then closes at CLOSE_S (the user's line: the
                    # art's edge is columns 285-294 on rows 144-168, read on the UNLIT frame; lit,
                    # the bow's shadow on the water read as hull and put it 7-14 px out)
CLOSE_S = 0.65
BOW_RAKE = 0.8      # below the rail the bow leans forward: the prow bulges to column 303 on row 180
WATER = -5.0        # the hull's lowest drawn line (the stern's bottom, rows 174-182)
W_RAIL = 84.0       # half the beam (far rail Z 481, near 649)
FAR_OUT = 8.0       # the far side along the main deck stands this much further out: the art's
                    # deck reaches 4 rows further under the far bulwark (the user's green line,
                    # 2026-10-26); full from X_CASTLE + 14 (the long box's front) to X_CLOSE,
                    # none at the castle's front and at the nose
INNER = 6.0         # the bulwarks' thickness
CAP_DROP = 6.0      # the rail's inner edge under its outer one (the beam's rounding)
CAP_BOW = 8.0       # the beam's width round the bow (the art draws it wider there)
MAST = (507.0, ZC)  # the main mast's foot
MAST_R, TOP_R = 5.66, 20.6  # the mast under its top; the top (a platform, 29 wide with its rim)
# The mast as the art draws it, outline included (the user, 2026-10-26, found it 2
# pixels too narrow on the right): columns 144-151 down to row 75
# of map 58's frame (radius 5.66: 4 px either side of its axis), 143-152
# from row 76 to its foot (7.07 under MAST_FOOT_Y), both centred half a
# column right of MAST's (MAST_SHIFT along X - Z; the yard keeps MAST).
MAST_FOOT_R, MAST_FOOT_Y, MAST_SHIFT = 7.07, 89.0, 0.5
TOP_RV, TOP_Y = 9.2, 371.0  # the top's depth half-axis (the art draws it flat) and its face
MAST_TIP = 454.0            # the topmast's point (460 stood 3 rows over the art's)
# The brace from the yard's far end to its tie on the quay: its centre line
# (column, row) on the flat frame, from the yard's end down over a barrel.
BRACE = [(243.0, 16.5), (238.0, 36.0), (234.0, 49.0), (230.0, 56.0), (224.0, 64.0), (215.0, 75.0),
         (214.5, 76.5)]
BRACE_R = 2.0
# The near shrouds, five ropes from under the mast's top to the castle's
# near rail: their upper runs are BG1 (over the quay), the lower ones painted
# into the castle's art on BG0, and as two surfaces they came apart under
# the stick (the user, 2026-10-16). Each its top and its foot (column, row)
# on the flat frame: lines fitted to their BG1 pixels, followed down the
# castle's art to their end; the top at 360 high, the foot on the near rail.
SHROUDS = [((171.3, -12.0), (90.4, 81.0)), ((170.9, -12.0), (105.7, 77.0)), ((174.5, -12.0), (118.5, 76.0)),
           ((175.8, -12.0), (145.7, 72.0)), ((174.7, -12.0), (169.5, 52.0))]
SHROUD_R = 1.5
N_SIDE, N_ARC = 28, 28
# The bow is rounded: a circular nose of radius NOSE_R tangent to the sides'
# closing lines (the stem lands at X_CLOSE + 96 at the rail, 726).
NOSE_R, NOSE_N = 40.0, 6

# The stern's profile: the silhouette's left edge, read row by row on the
# flat frame as (height, the near corner's X). The post stands at 378 from
# the balustrade down to the near rail; the counter tucks in to 401 at 69;
# the wale is a ledge at 41-44 (the counter's underside to the wale's top,
# 60-69, is the notch); below it the stern rakes forward to the keel's line
# at -8 (the art's rows 128-180: columns 74 to 128).
# (2026-10-16, states/HJ: the wale's ledge at 44-60 jutted out to 381-391,
# where the art is open water to 399-401: the hull took the notch's water,
# rows 120-137. Now 399 / 401 there, the art's edge row by row.)
# (Then the same day, the user's spot 4: rows 133-149 refitted to the art's
# edge row by row, it took a column of water at 133-138.)
STERN_PROFILE = [(-5.0, 469.0), (-1.0, 445.0), (7.0, 429.0), (15.0, 413.0), (22.0, 409.4), (27.2, 407.4),
                 (35.5, 407.4), (41.0, 403.0), (44.0, 401.0), (60.0, 401.0), (69.0, 401.0), (79.0, 389.0), (93.0, 385.0),
                 (STERN_RAIL, 378.0),
                 # The post's top is rounded: in 2 columns over its top 4 rows,
                 # 1 over the 6 under them (the art's water there, the user's
                 # spot 1, 2026-10-16).
                 (171.4, 378.0), (172.7, 380.2), (184.5, 380.2), (185.7, 382.2), (200.0, 382.2)]
LEVELS = [y for y, _ in STERN_PROFILE]   # the hull skin's rows (the top row is the rail)


def corner_x(y):
    """The transom's corners' X at height Y (the profile above the rail is the post)."""
    if y <= STERN_PROFILE[0][0]:
        return STERN_PROFILE[0][1]
    for (y0, x0), (y1, x1) in zip(STERN_PROFILE, STERN_PROFILE[1:]):
        if y <= y1:
            return x0 + (x1 - x0) * (y - y0) / (y1 - y0)
    return STERN_PROFILE[-1][1]


def x_close(y):
    return X_CLOSE + max(0.0, RAIL - y) * BOW_RAKE


# A function (y, inset, pts) -> pts that reshapes the plan, and one
# (i, x, z) -> height or None that gives a rail point's top (ship58.py: map
# 58's bow from the user's waterline, stem and far rail); None here.
BOW_PLAN = None
RAIL_FIT = None
# A function (i, top, cap) -> point or None: the rail's inner edge over plan
# point i, given its outer top and the default (ship58.py: map 58's near
# rail flat to the art's outline). The bulwark's foot stays on the plan.
CAP_IN = None
# The bowsprit's ends on its axis (ship58.py moves them to map 58's drawing).
BOWSPRIT = ((715.0, 80.0, ZC), (850.0, 131.0, ZC))


def outline(y, inset=0.0):
    pts = plan(y, inset)
    return BOW_PLAN(y, inset, pts) if BOW_PLAN else pts


def nose_circle(y, inset=0.0):
    """The nose: a circle tangent to the closing line (its centre on the
    keel's line), from the stem (angle 0) round to the tangent, then the side
    straight back to the transom's corner. Its centre's X, the tangent's X,
    its radius and the tangent's angle."""
    W = W_RAIL - inset
    q = math.sqrt(1.0 + CLOSE_S ** 2)
    r = NOSE_R - inset
    xc = x_close(y) + (W - r * q) / CLOSE_S
    at = math.atan2(r / q, r * CLOSE_S / q)
    return xc, xc + r * math.cos(at), r, at


# The sides' points, as fractions of the way from the nose to the transom's
# corner: N_SIDE even steps, each step that reaches X DENSE_X at the rail cut
# in DENSE_K. There the castle's rail sweeps down to the main rail's end,
# which rises nearly straight (the user's two curves, 2026-10-26): 11 apart,
# the points drew one curve through the corner. The same fractions on every
# level, so the levels keep one count. Above the rail the transom's corner
# moves aft (the stern's profile) and the sides' points went with it, each
# by its fraction: a point forward of the castle's front taking the castle's
# top slid 9 aft, and the corner's points bunched up and left a gap the rail
# bridged over the cliff (the user saw it take part of the cliff). The
# sides are straight there, so a point's slide only moves where the side is
# sampled: points forward of X_CASTLE keep their place at the rail
# (STAY_BLEND aft of it they slide less, wider than any slide so they never
# cross).
DENSE_X, DENSE_K = (425.0, 486.0), 6
STAY_BLEND = 30.0


def side_t():
    xa, (_, xj, _, _) = corner_x(RAIL), nose_circle(RAIL)
    ts = []
    for i in range(N_SIDE):
        t0, t1 = i / N_SIDE, (i + 1) / N_SIDE
        x0, x1 = xj - (xj - xa) * t0, xj - (xj - xa) * t1
        ts.append(t0)
        if x1 < DENSE_X[1] and x0 > DENSE_X[0]:
            ts += [t0 + (t1 - t0) * k / DENSE_K for k in range(1, DENSE_K)]
    return ts


def plan(y, inset=0.0):
    """The hull's plan at height Y, as points from the stem along the far
    side, round the transom's corners, back along the near side: (X, Z).
    Index ranges: far side [0, NF), the transom [NF, NF + N_ARC], near side
    after (NF = NOSE_N + len(SIDE_T))."""
    W = W_RAIL - inset
    xa, xo = corner_x(y), x_close(y)
    def side_z(x):
        return min(W, W - CLOSE_S * (x - xo))
    xc, xj, r, at = nose_circle(y, inset)
    nose = [(xc + r * math.cos(a), r * math.sin(a)) for a in (at * i / NOSE_N for i in range(NOSE_N))]
    xa_r = corner_x(RAIL)
    def side_x(t):
        x = xj - (xj - xa) * t
        if y <= RAIL:
            return x
        x_r = xj - (xj - xa_r) * t
        stay = min(1.0, max(0.0, (x_r - X_CASTLE) / STAY_BLEND + 1.0))
        return x - (xa - xa_r) * t * stay
    far = [side_x(t) for t in SIDE_T]
    def far_out(x):
        aft = (x - X_CASTLE) / 14.0
        bow = (xj - x) / (xj - xo)
        return FAR_OUT * min(1.0, max(0.0, min(aft, bow)))
    pts = [(x, ZC - off) for x, off in nose] + [(x, ZC - side_z(x) - far_out(x)) for x in far]
    for i in range(N_ARC + 1):
        t = -math.pi / 2 + math.pi * i / N_ARC
        pts.append((xa - (CORNER - inset) * math.cos(t), ZC + W * math.sin(t)))
    pts += [(x, ZC + side_z(x)) for x in reversed(far)] + [(x, ZC + off) for x, off in reversed(nose)]
    return pts


SIDE_T = side_t()
NF = NOSE_N + len(SIDE_T)


def is_transom(i):
    return NF <= i <= NF + N_ARC


# The rail's top on the screen as the user drew it (2026-10-16, over the
# fitting overlay of states/HJ at 384x216, the same projection as GF): per
# screen column, the far side's (and the transom's) line and the near
# side's, smoothed over 5 columns. It rises toward the bow (the sheer), sweeps
# up smoothly onto the castle on the far side (the old rail stepped from 56
# to 94 at the castle's front wall), and on the near side runs from the
# post's foot (row ~100) down to the waist lower than the old straight ramp.
# Then (2026-10-16) the far side's put a row or two lower wherever its top
# row was the art's water (columns 170-172, 210-214, 280-288: the user's
# spots by the barrels and at the bow). Then (2026-10-26, the user saw two curves
# on the boat's sides where the model had one) columns 198-212.5 from the
# art's outline: the castle's rail sweeps down to 206, row 87.8, and the main
# rail's end drops from there, a column in 12 rows (206.45 at 92.5 to 208.2
# at 100.2: the art's edge pixels at 174 / 175 / 176 of map 58's frame),
# before its own curve
# (the smoothing had drawn one curve through the corner). And columns
# 178-196: the castle rail's top cap right of the mast (the user asked for the
# railing right of the mast to follow it too: the line sagged 4-6
# rows under the cap there, and the rope sheets through the mast took it):
# level at 66 behind the mast, half a row a column to (193, 71), straight
# down to 77.5, then into the curve. The near side the
# same (the user asked for the near side to match): columns 114-124.6 follow the
# castle's rail outline (3 rows under it, as the main rail's line does) down
# to its end post at 124, then drop to the main rail's line at 124.6.
RAIL_TOP = [(74, 58.0), (76, 56.2), (78, 53.4), (80, 53.4), (82, 53.4), (84, 53.4), (86, 53.3), (88, 53.2), (90, 53.2), (92, 53.4), (94, 53.6), (96, 53.6), (98, 53.6), (100, 53.8), (102, 54.0), (104, 54.0), (106, 53.7), (108, 53.4), (110, 53.1), (112, 53.1), (114, 53.0), (116, 53.0), (118, 53.1), (120, 53.3), (122, 53.6), (124, 53.9), (126, 54.0), (128, 54.2), (130, 54.4), (132, 54.4), (134, 54.5), (136, 54.8), (138, 54.9), (140, 54.7), (142, 54.2), (144, 54.1), (146, 54.2), (148, 54.3), (150, 54.4), (152, 54.6), (154, 55.0), (156, 55.5), (158, 56.1), (160, 56.8), (162, 57.5), (164, 58.1), (166, 58.7), (168, 59.4), (170, 60.7), (172, 62.7), (174, 63.2), (176, 65.1), (178, 66.0), (180, 66.0), (182, 66.0), (183.5, 66.0), (185.5, 67.0), (187.5, 68.0), (189.5, 69.0), (191.5, 70.0), (193, 71.0), (193.4, 77.5), (195, 81.2), (196, 83.0), (198, 84.3), (200, 85.2), (202, 86.0), (204, 86.8), (206, 87.8), (206.45, 92.5), (207.4, 98.5), (208.2, 100.2), (209, 101.0), (210, 102.0), (211, 103.0), (212.5, 104.0), (214, 104.7), (216, 105.5), (218, 106.9), (220, 108.3), (222, 109.5), (224, 110.7), (226, 112.0), (228, 113.4), (230, 114.7), (232, 115.7), (234, 116.3), (236, 116.7), (238, 117.2), (240, 118.1), (242, 119.1), (244, 120.2), (246, 121.2), (248, 122.5), (250, 123.7), (252, 125.0), (254, 126.5), (256, 127.9), (258, 128.8), (260, 129.2), (262, 129.3), (264, 129.9), (266, 130.8), (268, 131.8), (270, 132.8), (272, 133.7), (274, 134.6), (276, 135.7), (278, 136.9), (280, 139.7), (282, 141.7), (284, 142.7), (286, 145.6), (288, 150.7), (290, 152.2), (292, 158.5), (294, 164.2)]
RAIL_BOTTOM = [(74, 93.0), (76, 98.6), (78, 100.6), (80, 102.0), (82, 103.3), (84, 104.5), (86, 105.6), (88, 106.8), (90, 108.1), (92, 109.2), (94, 110.1), (96, 110.8), (98, 111.6), (100, 112.7), (102, 114.4), (104, 116.5), (106, 119.1), (108, 121.8), (110, 124.4), (112, 127.0), (114, 128.25), (116, 129.25), (118, 130.25), (120, 131.25), (122, 132.25), (123.8, 133.2), (124.6, 146.0), (126, 147.9), (128, 149.8), (130, 151.6), (132, 153.3), (134, 154.9), (136, 156.1), (138, 157.0), (140, 157.9), (142, 158.8), (144, 160.1), (146, 161.7), (148, 163.5), (150, 165.1), (152, 166.2), (154, 166.8), (156, 167.2), (158, 167.9), (160, 168.9), (162, 170.0), (164, 171.1), (166, 172.0), (168, 172.8), (170, 173.8), (172, 175.2), (174, 176.7), (176, 178.0), (178, 179.1), (180, 180.2), (182, 181.3), (184, 182.1), (186, 182.8), (188, 183.6), (190, 184.6), (192, 185.4), (194, 186.1), (196, 186.6), (198, 187.0), (200, 187.5), (202, 188.0), (204, 188.4), (206, 188.6), (208, 188.8), (210, 189.1), (212, 189.2), (214, 189.2), (216, 189.2), (218, 189.2), (220, 189.2), (222, 189.2), (224, 189.2), (226, 189.2), (228, 189.1), (230, 189.0), (232, 189.0), (234, 188.8), (236, 188.7), (238, 188.5), (240, 188.3), (242, 188.1), (244, 187.8), (246, 187.6), (248, 187.4), (250, 187.3), (252, 187.2), (254, 186.8), (256, 186.4), (258, 185.9), (260, 185.6), (262, 185.3), (264, 185.0), (266, 184.6), (268, 184.1), (270, 183.4), (272, 182.7), (274, 181.7), (276, 180.8), (278, 180.3), (280, 179.9), (282, 179.4), (284, 178.8), (286, 178.1), (288, 177.3), (290, 176.3), (292, 171.5), (294, 167.8)]


def rail_row(table, col):
    """The table's row at a screen column (linear between its points)."""
    if col <= table[0][0]:
        return table[0][1]
    for (c0, r0), (c1, r1) in zip(table, table[1:]):
        if col <= c1:
            return r0 + (r1 - r0) * (col - c0) / (c1 - c0)
    return table[-1][1]


def top_at(i, x, z):
    """The rail over plan point i: the balustrade's top on the transom (the
    screen's row TRANSOM_TOP gives, so rising toward the near corner);
    elsewhere the height that puts the point on the user's line (RAIL_TOP on
    the far side, RAIL_BOTTOM on the near side) at its own screen column."""
    if RAIL_FIT:
        y = RAIL_FIT(i, x, z)
        if y is not None:
            return y
    col = (x - z) / 2.0 + KX
    if is_transom(i) and col <= TRANSOM_TOP[-1][0]:
        return y_at_row(x, z, rail_row(TRANSOM_TOP, col))
    return y_at_row(x, z, rail_row(RAIL_TOP if z < ZC else RAIL_BOTTOM, col))


# The yard's centre line on the art (the user drew it): row 138 - x / 2 from
# column 116 to 243, slope -1/2, i.e. level along Z, 223 high; 5 px thick
# (outline, tan, olive, tan, outline). What looked like a shallower spar
# left of the mast was a rope above it.
YARD_LINE = [(116, 80.0), (243, 16.5)]
YARD_R = 4.0


def yard_pts():
    return [(MAST[0], y_at_row(MAST[0], z_at_col(MAST[0], c), r), z_at_col(MAST[0], c)) for c, r in YARD_LINE]


def yard_y(z):
    """The yard's axis height at depth Z (between its points)."""
    pts = yard_pts()
    for (x0, y0, z0), (x1, y1, z1) in zip(pts, pts[1:]):
        if min(z0, z1) <= z <= max(z0, z1):
            return y0 + (y1 - y0) * (z - z0) / (z1 - z0)
    return pts[0][1] if z > pts[0][2] else pts[-1][1]


SAIL_X = 503.0   # just behind the mast's axis: the mast is drawn over the sail
# The sail's outline as the user drew it (2026-10-16, over the viewer's flat
# frame at cell 14,12, as the model did not take all of the sail): per screen column,
# its top and bottom rows, all four swags (the leftmost one, columns 118-140,
# was cut short), the top kept under the castle's strip the user marked
# between the yard and the two left swags (not sail).
SAIL_EDGE = [(118, 82, 85), (120, 81, 87), (122, 82, 88), (124, 82, 89), (126, 81, 89), (128, 81, 89), (130, 81, 89), (132, 80, 89), (134, 79, 88), (136, 78, 88), (138, 77, 87), (140, 75, 86), (142, 74, 84), (144, 72, 81), (146, 67, 79), (148, 66, 74), (150, 65, 72), (152, 65, 73), (154, 66, 73), (156, 66, 74), (158, 65, 73), (160, 65, 73), (162, 65, 73), (164, 64, 73), (166, 62, 72), (168, 61, 71), (170, 57, 70), (172, 55, 69), (174, 53, 68), (176, 52, 65), (178, 52, 63), (180, 50, 58), (182, 49, 56), (184, 49, 57), (186, 49, 57), (188, 48, 57), (190, 48, 57), (192, 47, 57), (194, 46, 57), (196, 46, 57), (198, 44, 56), (200, 43, 56), (202, 41, 55), (204, 40, 54), (206, 38, 52), (208, 37, 50), (210, 36, 46), (212, 35, 41), (214, 33, 39), (216, 33, 41), (218, 33, 41), (220, 32, 42), (222, 32, 42), (224, 31, 42), (226, 30, 41), (228, 30, 41), (230, 29, 41), (232, 27, 40), (234, 26, 39), (236, 23, 37), (238, 21, 35), (240, 20, 34), (242, 20, 31)]


def plank(m, q0, q1, s1, s0, thick=6.0, flags=0):
    """A gangplank's slab by the four corners of its top: q0/q1 the shore
    end's far and near (+X) corners, s1/s0 the ship end's near and far. Its
    front (+X) and its ship end (+Z) show their thickness."""
    down = lambda p: (p[0], p[1] - thick, p[2])
    m.poly("plank", [q0, s0, s1, q1], flags)                # the top, facing up
    m.poly("plank", [q1, s1, down(s1), down(q1)], flags)    # the front (+X)
    m.poly("plank", [s0, down(s0), down(s1), s1], flags)    # the ship end (+Z)
    m.guide([q0, q1, s1, s0, q0], (255, 128, 255))
    m.guide([down(q1), down(s1), down(s0)], (255, 128, 255))


def build(quay=True):
    """quay=False: without what belongs to map 97's quay (ship58.py)."""
    m = Model("ship")
    # The hull's skin: the plan at each level, joined level to level; the
    # top row at the rail's height over each point (the plan taken at the
    # point's own height: the stern's profile moves it).
    n = len(outline(RAIL))
    def at(i, y, inset=0.0):
        x, z = outline(y, inset)[i]
        return (x, y, z)
    def tops_of(inset):
        # Solved where the point stands at its own height (the plan moves
        # with the height: the bow's rake, the stern's profile), so the top
        # lands on the line; taken at STERN_RAIL it missed by up to 3 rows
        # and the rail took the water and the wall above it (the user,
        # 2026-10-16).
        t = []
        for i in range(n):
            y = STERN_RAIL
            for _ in range(8):
                x, z = outline(y, inset)[i]
                y = top_at(i, x, z)
            t.append(y)
        return t
    tops = tops_of(0.0)
    rows = [[at(i, min(L, tops[i])) for i in range(n)] for L in LEVELS]
    rows.append([at(i, tops[i]) for i in range(n)])
    m.strip("hull", rows)
    rail_plan = rows[-1]
    # The bulwarks' inner faces, deck to rail (main deck) and castle deck to
    # rail (castle: the balusters, the transom's inner side). LEE: turned away
    # from the sun they are in shade (the user, 2026-10-16: the far bulwark's
    # inner face lit as if in the sun, much better shaded); so are the
    # castle's and the crates'. The hull's outer skin stays as drawn.
    itops = tops_of(INNER)
    plan_x = [x for x, _ in outline(RAIL, INNER)]
    main_i = [i for i in range(n) if plan_x[i] >= X_CASTLE]
    far_i = [i for i in main_i if i < NF + N_ARC // 2]
    near_i = [i for i in main_i if i > NF + N_ARC // 2]
    # The rail along their top (the art's light, rounded beam, widest round
    # the bow): from the outer line down to the inner face's top, CAP_DROP
    # lower, so it faces up and inward and takes the sun. Without it the
    # inner face began on the rail's line and its lee took the beam (the
    # user, 2026-10-16, saw shadow where none was expected).
    # The beam widens round the bow (CAP_BOW there, from X 590 to 650), and
    # the bulwark under it with it. TOP: a top however steep (it climbs to the
    # castle and rounds the nose past 45 degrees, and was taken for a wall
    # there, lit and placed as one: the user, 2026-10-27, states/JV).
    rail_x = [x for x, _ in outline(RAIL)]
    def ins(i):
        t = min(1.0, max(0.0, (rail_x[i] - 590.0) / 60.0))
        return INNER + (CAP_BOW - INNER) * t
    def cap_in(i):
        x, z = outline(tops[i], ins(i))[i]
        cap = (x, tops[i] - CAP_DROP, z)
        return (CAP_IN(i, at(i, tops[i]), cap) if CAP_IN else None) or cap
    for seg in (far_i, near_i):
        m.strip("rail", [[at(i, tops[i]) for i in seg], [cap_in(i) for i in seg]], flags=TOP)
        m.strip("bulwark", [[cap_in(i) for i in seg], [at(i, DECK, ins(i)) for i in seg]], flags=LEE)
        m.guide([cap_in(i) for i in seg], (255, 255, 0))
    aft_i = [i for i in range(n) if plan_x[i] <= X_CASTLE]
    m.strip("castle", [[at(i, itops[i], INNER) for i in aft_i], [at(i, CASTLE, INNER) for i in aft_i]], flags=LEE)
    far_main = [(p[0], p[2]) for p in (at(i, DECK, ins(i)) for i in far_i)]
    near_main = [(p[0], p[2]) for p in (at(i, DECK, ins(i)) for i in near_i)]
    aft = [(p[0], p[2]) for p in (at(i, CASTLE, INNER) for i in aft_i)]
    # The decks: fans from their middles over the inner plan.
    def fan(name, pts, y, flags=0):
        cx = sum(p[0] for p in pts) / len(pts)
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            m.poly(name, [(cx, y, ZC), (a[0], y, a[1]), (b[0], y, b[1])], flags)
    fan("deck", [(X_CASTLE, far_main[-1][1])] + near_main + far_main + [(X_CASTLE, near_main[0][1])], DECK)
    fan("castle", [(X_CASTLE, aft[0][1])] + aft + [(X_CASTLE, aft[-1][1])], CASTLE, LEE)
    # The castle's front wall, with the door.
    zf, zn = aft[0][1], aft[-1][1]
    m.poly("castle", [(X_CASTLE, DECK, zf), (X_CASTLE, CASTLE, zf), (X_CASTLE, CASTLE, zn), (X_CASTLE, DECK, zn)], LEE)
    # The mast, the yard, the furled sail hanging under it in three swags,
    # the bowsprit. The yard and the sail are drawn on either layer of the
    # art (BG1 over the quay, BG0 over the ship's own art): a pixel is
    # theirs on both, and BG1 only says it is lifted (kol_focus kModelBg1).
    mx, mz = MAST
    # The mast as drawn: 8 wide under its top, 10 at its foot (above), the topmast
    # above 5 (3.6 to 3.2); the top, a round platform 26 wide, 367 high (the
    # rigging meets there). BG1: over the quay they hang as the yard does,
    # and the ropes crowded beside them stay the rigging's (kol_focus
    # rope_px). (The user, 2026-10-16, saw the sail, mast and ropes
    # only partly taken by the model: the mast was 5 wide, the top missing.)
    sx, sz = mx + MAST_SHIFT, mz - MAST_SHIFT
    m.cyl("mast", (sx, DECK, sz), (sx, MAST_FOOT_Y, sz), MAST_FOOT_R, flags=BG1)
    m.cyl("mast", (sx, MAST_FOOT_Y, sz), (sx, 364.0, sz), MAST_R, flags=BG1)
    m.cyl("mast", (mx, 364.0, mz), (mx, MAST_TIP, mz), 3.6, r1=1.5, flags=BG1)
    # The top: drawn flatter than a disc (29 wide, its face 6.5 rows high,
    # the rim under it 2.5): an ellipse in plan, TOP_R across the screen,
    # TOP_RV along the line of sight's ground direction.
    ring_t, ring_b = [], []
    for i in range(25):
        a = 2.0 * math.pi * i / 24
        cu, cv = TOP_R * math.cos(a), TOP_RV * math.sin(a)
        dx, dz = (cu + cv) / math.sqrt(2.0), (cv - cu) / math.sqrt(2.0)   # u = (1, -1), v = (1, 1)
        ring_t.append((mx + dx, TOP_Y, mz + dz))
        ring_b.append((mx + dx, TOP_Y - 5.0, mz + dz))
    m.strip("mast", [ring_t, ring_b], flags=BG1)   # (top first: outward)
    for i in range(24):
        m.poly("mast", [(mx, TOP_Y, mz), ring_t[i + 1], ring_t[i]], BG1)
    yp = yard_pts()
    for a, b in zip(yp, yp[1:]):
        m.cyl("yard", a, b, YARD_R, sides=8, flags=BG1)
    top_row, bot_row = [], []
    for col, top, bottom in SAIL_EDGE:
        z = z_at_col(SAIL_X, col)
        yt = min(yard_y(z), y_at_row(SAIL_X, z, top))
        yb = y_at_row(SAIL_X, z, bottom + 1.0)
        top_row.append((SAIL_X, yt, z))
        bot_row.append((SAIL_X, yb, z))
    m.strip("sail", [top_row, bot_row], flags=BG1 | CLOTH | SHEET)
    m.cyl("bowsprit", BOWSPRIT[0], BOWSPRIT[1], 4.5, sides=8)
    # The rigging over the quay (BG1; its lower runs are painted into the
    # castle's art on BG0): the near shrouds, five lines that meet over the
    # mast at row ~-20 (a masthead at 360), fan down to the castle's near
    # rail; the rest (a halyard by the mast, the lines out to the yard's
    # ends) lie about the mast's own plane.
    head = (mx, 360.0, mz)
    m.poly("rigging", [head, (X_CORNER, STERN_RAIL, 649.0), (507.0, 85.0, 649.0)], SHEET | BG1 | ROPES)
    # The mast's plane between the masthead and the yard (the lines out to
    # its ends: a triangle; a rectangle past the yard's far end took the
    # rim of a barrel's lid on the quay, the user, 2026-10-16), and a strip
    # by the mast below it (the halyard): under the yard's far end the plane
    # took the brace below at its own depth, and the brace leans away from
    # it (the stick stood it up as a spike).
    R = SHEET | BG1 | ROPES
    (_, _, yz0), (_, _, yz1) = yard_pts()[-1], yard_pts()[0]
    m.poly("rigging", [(mx, 368.0, mz), (mx, 215.0, min(yz0, yz1) - 4.0), (mx, 215.0, max(yz0, yz1) + 4.0)], R)
    m.poly("rigging", [(mx, 120.0, 590.0), (mx, 215.0, 590.0), (mx, 215.0, 540.0), (mx, 120.0, 540.0)], R)
    # The brace from the yard's far end down to a tie on the quay by the
    # barrels, and the mooring line from that tie to the far rail by the
    # castle (the user, 2026-10-16: rogue pixels): each a sheet standing on
    # its line in plan, so a rope pixel takes its own depth wherever the
    # rope sags. Read on the viewer's flat frame: the tie at (168.5, 94.5),
    # the line's end on the rail at (147, 104), cell 14,12.
    tie = (413.5, QUAY, 396.5)
    rail_end = (455.0, 92.0, 481.0)
    for (ax, _, az), (bx, _, bz), lo, hi in ((tie, rail_end, 30.0, 130.0),):
        m.poly("rigging", [(ax, lo, az), (ax, hi, az), (bx, hi, bz), (bx, lo, bz)], R)
    # The brace itself, a rope (not a sheet: it crosses in front of a
    # barrel, a solid patch of BG1 the sheets leave to the quay; the user,
    # 2026-10-16): through its centre line read row by row on the flat
    # frame, in the vertical plane from the yard's end to its tie on the
    # quay behind the barrel; radius 2, the drawn line's 3 pixels.
    bx0, _, bz0 = yard_pts()[-1]
    tie_col, tie_row = BRACE[-1]
    tx_z = 2.0 * (tie_col - KX)
    tx_pz = 4.0 * (tie_row - KY) + 2.0 * QUAY
    btx, btz = (tx_pz + tx_z) / 2.0, (tx_pz - tx_z) / 2.0
    pts = []
    for col, row in BRACE:
        d = 2.0 * (col - KX)                    # X - Z on the screen column
        t = ((bx0 - bz0) - d) / ((bx0 - bz0) - (btx - btz))
        x, z = bx0 + t * (btx - bx0), bz0 + t * (btz - bz0)
        pts.append((x, y_at_row(x, z, row), z))
    for a, b in zip(pts, pts[1:]):
        m.cyl("brace", a, b, BRACE_R, sides=6, flags=BG1 | THIN)
    m.guide(pts, (255, 255, 0))
    for (tc, tr), (fc, fr) in SHROUDS:
        d = 2.0 * (tc - KX)
        s_ = 4.0 * (tr - KY) + 2.0 * 360.0
        top = ((s_ + d) / 2.0, 360.0, (s_ - d) / 2.0)
        fx = W_RAIL + ZC + 2.0 * (fc - KX)
        foot = (fx, y_at_row(fx, W_RAIL + ZC, fr), W_RAIL + ZC)
        m.cyl("shrouds", top, foot, SHROUD_R, sides=6, flags=BG1 | THIN)
        m.guide([top, foot], (255, 255, 0))
    # The boxes on the deck, as the user marked them (2026-10-16: tops
    # horizontal, faces walls), fitted to the corners of their outlines on
    # the flat frame: the two crates against the far bulwark by the gangplank
    # (14 high, one cell apart). Against the castle's front wall, from the
    # user's painting of faces on map 58's flat frame (2026-10-26): a long
    # low box from the door to the far bulwark (top 62); in front of it the
    # crates right of the mast (top 52) and a barrel either side of them,
    # both the same size (top 47; the far one in the corner).
    boxes = [((511.0, DECK, 489.0), (535.0, 30.0, 511.0)), ((543.0, DECK, 489.0), (567.0, 30.0, 511.0))]
    if not quay:   # the gangplank's two crates: map 58's deck has none
        boxes = []
    boxes += [((X_CASTLE, DECK, 484.0), (460.0, 62.0, 568.0)),
              ((460.0, DECK, 502.0), (480.0, 52.0, 536.0)),
              ((460.0, DECK, 484.0), (474.0, 47.0, 502.0)),
              ((460.0, DECK, 539.0), (474.0, 47.0, 557.0))]
    for lo, hi in boxes:
        m.box("crate", lo, hi, LEE)
        (x0, y0, z0), (x1, y1, z1) = lo, hi
        m.guide([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1), (x0, y1, z0)], (0, 255, 255))
        m.guide([(x0, y1, z1), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1)], (0, 255, 255))
        m.guide([(x1, y0, z1), (x1, y0, z0), (x1, y1, z0)], (0, 255, 255))
    # The gangplank from the far rail down to the quay (the cells under it
    # are the channel's water, 0: as terrain it lay in the channel and took
    # the quay's shadow). Fitted on the flat frame by the four corners of its
    # top (the user, 2026-10-16, found it too narrow): the quay end at the
    # quay's 48, X 513-569; the ship end on the far rail, 63 high, X 531-581
    # (it lies a little askew). Its front and its end show their thickness
    # (the light lip along the front, PT). With the far side out by FAR_OUT
    # (2026-10-26) the ship end moved onto the new rail along the line of
    # sight (X, Y and Z less FAR_OUT: the same place on the screen), 55 high.
    PZQ, PZS, PYQ, PYS = 387.0, 483.0 - FAR_OUT, 48.0, 63.0 - FAR_OUT
    if quay:
        plank(m, (513.0, PYQ, PZQ), (569.0, PYQ, PZQ), (581.0 - FAR_OUT, PYS, PZS), (531.0 - FAR_OUT, PYS, PZS))
    # Fitting guides: the rail, the hull at the water, the castle's front.
    m.guide(rail_plan, (255, 0, 255))
    m.guide([(x, WATER, z) for x, z in outline(WATER)], (255, 128, 0))
    m.guide([(x, CASTLE, z) for x, z in aft], (0, 255, 0))
    m.guide([(mx, DECK, mz), (mx, 400, mz)], (255, 255, 255))
    m.guide(yard_pts(), (255, 255, 255))
    return m


def main():
    import ship_fit   # (the fits to the user's drawings, shared with map 58)
    m = ship_fit.build()
    ship_fit.stem_head(m)
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop,
                guides_only=os.environ.get("GUIDES") == "1")
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map97.inc")
    m.write_inc(out, 97, "tools/models/ship97.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
