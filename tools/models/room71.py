#!/usr/bin/env python3
"""Map 71, a boat-builder's shed (states/thor/0050). The game's grid is the
floor alone (height 1, Y 16, cells x 0-7, z 0-6); the shed's two walls are
art on the floor, so under the perspective the room lay down like a rug.
Two walls a plank-wall thick, measured on the flat scene (KOL_FOCUS_DUMP
.rgb, its projection): the right wall along X (inner face Z = RZ, facing
+Z) to its cut end at the grid's edge X 256 (screen x 298-305, its end 7
px wide: 16 thick); the left wall along Z (inner face X = LX, facing +X)
to its cut end at Z 224. Their tops at Y TOP.

The things in the room, solved on the flat scene as map 65's furniture was
(room65.py): a thing standing on the floor has its foot's lowest row at
sy = (X + Z)/4 + 72, its left/right extent gives X - Z (1 px = 2), the rows
of its top above its foot its height (1 row = 2 in Y). Each solid fits its
art or sits just inside it; every one stands on the floor (Y 16), none
crosses the walls' inner faces or another solid.

Skipped (flat or soft, the wall or floor carries them):
  rack   the two pitchforks: art on the left wall's face X 32 (flat x
         100-135, rows 80-100; handles to row ~120): Z 130-200, Y 70-120.
  shelf  the ladder/shelf: plank strips on the right wall's face (flat x
         232-300, rows 100-115); no foot on the floor.
  window the pale rectangle on the right wall's face Z 32, kept for the
         lighting: glass X 190-196, Y 65-101 (its blues at flat x 263-265,
         rows 81-99); frame X 180-204, Y 58-108.
  the cloth pooled on the floor (flat x 207-227 rows 113-128) stays floor
  art. The cloth over the boat's stern is modelled (part "cover", below).

The boat is an open skiff, one skin seen from both sides (boat()); its fit
was checked by rasterising the model as the game does (a pixel sampled at
its corner, the nearest surface along its ray) against the art's outlines.

    tools/models/room71.py                      writes src/kol_model_map71.inc
    tools/models/room71.py fit PNG INFO OUT [x0 y0 x1 y1]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, overlay, SHEET  # noqa: E402

FLOOR = 16.0
T = 16.0                # the right wall (its cut end 7 px wide)
TL = float(os.environ.get("R71_TL", 16))   # the left wall
# The inner faces at 32: the user's pick of the overlays at 24, 32 and 40
# (the middle one; shots/shed71/faces_24_32_40.png).
LX = float(os.environ.get("R71_LX", 32))
RZ = float(os.environ.get("R71_RZ", 32))
TOP = float(os.environ.get("R71_TOP", 138))
X1, Z1 = 256.0, float(os.environ.get("R71_Z1", 220))   # the walls' cut ends

# The post: flat columns 189-196 (outlines 189 and 195-196, lit 191-194),
# its foot ends at row 131 (floor from 132), front corner x 191.5 ->
# X - Z 15, X + Z 256; its top the blue-grey row 70 at x 190-194 (wall
# planks above): Y = 2((127.5 + 112.5)/4 + 76 - 70) = 132.
POST = ((127.5, FLOOR, 112.5), (135.5, 132.0, 120.5))
# The beam from the post to the left wall, on the post's Z: its bottom-near
# dark pairs (155.5,70) (163.5,74) (171.5,78) (183.5,84) give Y 104.5, the
# top-near edge Y 111.5; the lit band's left end x 151-153 (end outline
# 150-151) -> X 53. It meets the post's left face at rows 78-85.5.
BEAM_Y0, BEAM_Y1 = 104.5, 111.5
BEAM = ((53.0, BEAM_Y0, 112.5), (127.5, BEAM_Y1, 120.5))
# Its other half, from the post back to the right wall, on the post's X: the
# cross-section right of the post at row 78 spans x 198-212, at row 80 x
# 196-208; the wall end lit patch at x 230-235 rows 60-64. Its bottom 1.5
# lower than the front half's: at 104.5 its own bottom outline, (210,79)
# (208,80) (206,81) by the tarp and (204,82) (202,83) (200,84) by the wall,
# went to the tarp and the wall (the tarp's reviewer, 2026-11-02).
BEAM_BACK = ((127.5, 103.0, RZ), (135.5, BEAM_Y1, 112.5))
# The bucket, a cylinder: rim outline row 120 x 175-181, pale ring rows
# 120-123 x 172-183 (r 8), body outlines x 172-173 and 183 to row 131 (r 8
# at both ends: tapered, its left outline x 172 went to the runner behind);
# rim centre row 121.5 -> top Y 32; bottom centre row 129.5 -> X + Z 246,
# column 177.5 -> X - Z -13.
BUCKET = ((116.5, FLOOR, 129.5), (116.5, 32.0, 129.5), 8.0, 8.0)
# The small crate by the post's foot: its lattice top rows 128-140 x
# 204-226, dark front faces to row 147; projected corners top (204,134)
# (214.5,128.75) (226,134.5) (215.5,139.75), bottom front (215.5,146.5).
CRATE_SMALL = ((165.5, FLOOR, 104.5), (188.5, 29.5, 125.5))
# The crate lid: front top edge the bright row 124 (x 224-235), left edge x
# 224, front-bottom corner (235,130.5), right edge x 240-241.
CRATE_LID = ((156.0, FLOOR, 60.0), (176.0, 29.0, 74.0))
# The stack against the right wall: the upper crate's top diamond (272.5,
# 103) (282.5,98) (292.5,103) (282.5,108) -> X 230-250, Z 32-52, Y 87; both
# crates' right edge the dark-blue column x 293 rows 92-138 (X 250); the
# lower crate's foot along (287,141)-(281,145) (front-right corner Z 56),
# its top-left corner (266,109) -> X 220, Y 72 (the upper's bottom-front
# edge c -27 -> Y 74, 72 taken so the two touch).
CRATES_LOWER = ((220.0, FLOOR, RZ), (250.0, 72.0, 56.0))
CRATES_UPPER = ((230.0, 72.0, RZ), (250.0, 87.0, 52.0))
# The barrels, radius 10, 26 tall. barrel_2: rim top row 130 (x 265-270),
# rim rows 130-136 x 262-275, dark base row 149 -> X + Z 312, X - Z 169.
# barrel_1: rim rows 123-129 x 242-255 (14 px -> r 10), centre column 248.5
# -> X - Z 129; its foot is behind the balls, height taken from barrel_2.
BARREL_R, BARREL_TOP = 10.0, 42.0
BARRELS = (("barrel_1", 206.5, 77.5), ("barrel_2", 240.5, 71.5))
# The balls at the front right: pale round things x 248-257 rows 139-147
# and x 246-255 rows 148-156: short cylinders r 7, 10 tall. The rope coil
# (x 240-250 rows 147-162) stays floor art.
BALL_R, BALL_TOP = 7.0, 26.0
BALLS = ((223.5, 86.5), (239.5, 106.5))
# The boat (refitted 2026-11-02, a reviewer reading the art beside me): an
# open, flat-bottomed skiff along Z, upright, its bow to +Z (the screen's
# left, x 126) and a flat transom at -Z by the post (x 186). Measured on
# the flat scene's outlines (66,66,90):
#   far gunwale round the bow: (141.5,95) (137,97) (133,99) (127,104), then
#     down x 126-127 to row 111: the widest beam where it runs at slope -1/2;
#   near gunwale: rows 115-116, x 133-159, rising aft through (164,113)
#     (169,110) along a thin bar to (185,96);
#   stem: upright at x 124-129, rows 113-119.5, curving aft into the
#     forefoot through (129,121) (132,122.5) (134,125) (137,126);
#   near chine (the bottom's edge): (140,128) (147,127.5) (156,125.5)
#     (164,124) (169,121) (173,119), on the runner's top;
#   transom: the cloth's straight top-right edge along X, (175,87)-(186,93).
# A pair of gunwale points at one station (near and far, x 137 and 166)
# gives the half-beam (29) and the centreline; the chine on the runner
# (Y 28) fixes the place along the line of sight.
BOAT_X = 78.0                                     # the centreline
BOAT_ZT, BOAT_ZM, BOAT_ZS = 85.0, 142.0, 193.0    # transom, widest, stem
BOAT_BT, BOAT_B = 11.0, 30.0                      # half-beam: transom top, widest
BOAT_FLOOR = 28.0                                 # the bottom, on the runner


def boat_half_beam(z):
    """Half the beam at the gunwale."""
    if z <= BOAT_ZT:
        return BOAT_BT
    if z <= BOAT_ZM:
        u = (z - BOAT_ZT) / (BOAT_ZM - BOAT_ZT)
        return BOAT_BT + (BOAT_B - BOAT_BT) * math.sin(math.pi / 2 * u) ** 0.8
    if z >= BOAT_ZS:
        return 0.0
    u = (z - BOAT_ZM) / (BOAT_ZS - BOAT_ZM)
    return BOAT_B * (1 - u ** 2) ** 0.9


def boat_sheer(z):
    """The gunwale's height: level aft, rising to the stem head (61.5)."""
    if z <= BOAT_ZM:
        return 54 - 0.5 * ((BOAT_ZM - z) / (BOAT_ZM - BOAT_ZT)) ** 2
    return 54 + 7.5 * ((z - BOAT_ZM) / (BOAT_ZS - BOAT_ZM)) ** 2


def boat_keel(z):
    """The bottom's height: flat, then the forefoot curving up to the stem."""
    if z <= 172:
        return BOAT_FLOOR
    u = (z - 172) / (BOAT_ZS - 172)
    return BOAT_FLOOR + 21.5 * u ** 2.2


def boat_chine(z):
    """Half the bottom's width: 4 in from the gunwale, narrowing inboard at
    45 degrees toward the bow (the art's bottom edge: row 126 at x 135-155,
    125 at 156-159, 124 at 160-163, then a row per 2 px up to (173,119));
    1.5 in aft of Z 120 (its outline (174-175,118) (176-177,117)), easing
    to 4 by Z 135."""
    aft = 2.5 * min(1.0, max(0.0, (135 - z) / 15))
    return max(0.0, min(boat_half_beam(z) - 4 + aft, 22 - 0.8 * (z - 156)))


def boat_section(z):
    """A station across the hull: far gunwale, far chine, keel, near chine,
    near gunwale (X, Y, Z)."""
    b, c, k, g = boat_half_beam(z), boat_chine(z), boat_keel(z), boat_sheer(z)
    return [(BOAT_X - b, g, z), (BOAT_X - c, k, z), (BOAT_X, k, z), (BOAT_X + c, k, z), (BOAT_X + b, g, z)]


def boat_stations(n=48):
    """Stations from the transom to the stem, closer toward the bow's curve."""
    return [BOAT_ZT + (BOAT_ZS - BOAT_ZT) * math.sin(math.pi / 2 * i / n) ** 0.8 for i in range(n + 1)]


# The stem: an upright post at the bow, its dark front face x 124-126 and
# lit side 128, rows 112-120 (without it the bow ended blunt).
STEM_POST = ((76.0, 50.0, 190.0), (80.0, 63.0, 196.0))
# What it stands on: a runner along Z under the near chine (its end face's
# outline x 132 rows 137-140, its floor edge (136,142)-(170,126): its last
# row is 211.5 - x/2, X 103; its top ends at x 167, floor shows at x 168-171
# rows 124-126 in front of where it would run on: it ends at Z 137), a chock beside the
# bow (its end face's edge the dark column x 112 rows 124-130, its bottom
# (114-117, 131-132), the top's front highlight (118,127)-(126,122)), a
# plank flat on the floor along X in front of both (its front highlight
# (103,128)-(130,141) at Y 19 is Z 204, its back edge (123.5,134)
# (129.5,137) Z 195.5, its end edge (143,143)-(139,146) X 113.5).
RUNNER = ((93.0, FLOOR, 137.0), (103.0, BOAT_FLOOR, 197.0))
CHOCK = ((54.0, FLOOR, 172.0), (63.0, 30.0, 198.0))
PLANK = ((42.0, FLOOR, 195.5), (111.5, 19.0, 203.5))


# The cloth over the stern (part "cover"): laid on the gunwales from Z 88
# (the transom's near top corner, x 181-186 rows 93-99, is wood) forward.
# Heaped 9 over the far gunwale from Z 91 to 116 (its silhouette (170,82.5)
# (163,83.5) (156,85.5) (152,86.5): Y ~63), falling to 0.5 at the transom's
# corner; the heap rides the far side only.
def cloth_heap(z):
    if z <= 87:
        return 0.5
    if z <= 91:
        return 0.5 + 8.5 * (z - 87) / 4
    if z <= 116:
        return 9.0
    return max(1.0, 9 - 8 * (z - 116) / 12)


# Its forward edge, from the far gunwale round to the near one, on the
# boundaries of the art's cloth pixels (the game samples a pixel at its
# corner): the upper cloth's left edge down to its lower fold, along the
# fold, then round the shaded band that hangs into the hull (its rounded
# end x 158-164, rows 102-108) and up to the near gunwale where the flap
# begins (Z 137). Each point has its X across the boat (screen x, row, X);
# its depth is solved from its pixel: the far half sags ~10 into the hull
# (Y 44-47 at Z 119-127; a flat edge at Y 55 left a curtain of streaks under
# it with the camera moved, and took the green interior at x 153-164 rows
# 101-106), the band's end hangs near the centreline (Y 37-46; with X
# spread evenly along the edge it lay as a shelf at the gunwale's height,
# folded), the near end lies on the gunwale.
COVER_EDGE = ((147.5, 92.0, 49.5), (147.5, 96.0, 52.0), (149.5, 98.0, 55.0), (151.5, 100.0, 59.0),
              (155.0, 101.5, 66.0), (159.5, 101.5, 72.0), (163.5, 101.5, 78.0), (158.5, 104.0, 76.0),
              (158.5, 106.0, 76.0), (160.5, 107.5, 78.0), (163.5, 108.5, 82.0), (166.5, 107.5, 92.0),
              (169.5, 107.0, 102.0), (169.8, 109.6, 108.9))


def cover_edge(u):
    """The forward edge's point at u (far 0 .. near 1)."""
    L = [0.0]
    for a, b in zip(COVER_EDGE, COVER_EDGE[1:]):
        L.append(L[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    t = min(max(u, 0.0), 1.0) * L[-1]
    i = 0
    while i < len(L) - 2 and L[i + 1] < t:
        i += 1
    f = (t - L[i]) / (L[i + 1] - L[i])
    (x0, y0, X0), (x1, y1, X1) = COVER_EDGE[i], COVER_EDGE[i + 1]
    sx, sy, X = x0 + f * (x1 - x0), y0 + f * (y1 - y0), X0 + f * (X1 - X0)
    Z = X - 2 * (sx - 184)
    return (X, 2 * ((X + Z) / 4 + 76 - sy), Z)


def cloth_rest(u, z):
    """The cloth lying on the gunwales at z (the heap on the far side)."""
    zb = max(z, BOAT_ZT)
    b = boat_half_beam(zb)
    y = (boat_sheer(zb) + 0.5 + cloth_heap(z) * (1 - u) ** 3 + u
         - 5 * math.sin(math.pi * u) * max(0.0, 1 - (z - 84) / 8))
    return (BOAT_X - b - 1 + u * (2 * b + 2), y, z)


def cloth_pt(u, v):
    """u: far 0 .. near 1; v: aft 0 .. the forward edge 1: lying on the
    gunwales, bent toward the edge as it nears it."""
    e = cover_edge(u)
    r = cloth_rest(u, 88 + v * (e[2] - 88))
    r1 = cloth_rest(u, e[2])
    w = v * v
    return tuple(r[i] + w * (e[i] - r1[i]) for i in range(3))


def drape_row(z):
    """Down the near side: to the floor at X 118 over the runner's outer
    top edge (106, 29) aft of Z 119 (its forward edge the art's upright at
    x 178 rows 112-119; its foot the cloth pooled to rows 125-127 at x
    183-187); then a
    flap hanging a little, to Z 137 (its hem the dark row 111 at x 168-175:
    (108, 46, 126), (109.5, 53, 137); lower, 43, where it meets the drape:
    the cloth at (176-177, 110-111))."""
    t = (BOAT_X + boat_half_beam(z) + 1, boat_sheer(z) + 1.5, z)
    if z <= 119:
        return [t, (106.0, 29.0, z), (118.0, FLOOR, z)]
    hy = 43.0 if z <= 124 else (46.0 if z <= 126 else 46 + 7 * (z - 126) / 11)
    h = (BOAT_X + boat_half_beam(z) + 1.5, hy, z)
    return [t, ((t[0] + h[0]) / 2, (t[1] + h[1]) / 2, z), h]


def boat(m):
    """The hull as one skin, both sides seen (SHEET): the camera sees the
    near side's outside and, over the near gunwale, the bottom and the far
    side's inside; the transom closes the stern."""
    rows = [boat_section(z) for z in boat_stations()]
    m.strip("boat", rows, SHEET)
    # The transom, its top dipped 5 in the middle (the cloth's top-right
    # edge sags to (180.5,91.5)-(182,92.5)); a fan from the dip.
    s = boat_section(BOAT_ZT)
    m.poly("boat", [(BOAT_X + 2, 48.5, BOAT_ZT), s[4], s[3], s[2], s[1], s[0]], SHEET)
    m.box("boat", *STEM_POST)
    m.box("stand", *RUNNER)
    m.box("stand", *CHOCK)
    m.box("stand", *PLANK)
    m.strip("cover", [[cloth_pt(i / 28, j / 16) for i in range(29)] for j in range(17)], SHEET)
    m.strip("cover", [drape_row(85 + 2 * i) for i in range(18)], SHEET)         # drape, Z 85-119
    m.strip("cover", [drape_row(119.01 + 2 * i) for i in range(10)], SHEET)     # flap, Z 119-137


def solid_poly(m, name, pts, centre):
    """A polygon of a solid, its corners turned so it faces away from `centre`."""
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if sum(n[i] * (a[i] - centre[i]) for i in range(3)) < 0:
        pts = list(reversed(pts))
    m.poly(name, pts)


# The tarp left of the window (2026-11-02, read from the art by a subagent
# first, as the boat): a sheet thrown over beam B (BEAM_BACK) and hanging
# down both sides. A split of the cloth colours into connected parts gives
# two layers with an outline between them. The NEAR side (+X, the plane
# X 137) is short: x 211.5-233.5 from row 61 to a U-shaped hem near row 105.
# The FAR side (X 127.5, the beam's far face) is long: the panel
# at x 204.5-212 below the beam and all the cloth under the near hem, down
# to the floor. The far panel's top starts at the beam's near bottom edge
# with the beam's own faces above it (it is behind the beam), and its foot
# (x 204-211, rows 121.5-118.5) solves on the floor to X 127-128. Over
# the beam a puffy roll; its back shoulder is the top silhouette. (A flat
# sheet on X 136 had both layers on one plane and stood as a stiff board.)
TARP_XN, TARP_XF = 137.0, 127.5
# Outline points by the game's sampling: a pixel shows what covers its
# corner, so a column's top or bottom lies at the column's own x.
# The back shoulder's height by Z (on X 127.5): the silhouette over the
# beam's far top edge, its right end lying flat on the beam's top (Y 112.5
# at Z 39.5), a step up at Z 52-54 to the crest (120 at Z 54.5-65.5).
TARP_SHOULDER = ((39.5, 112.5), (40.5, 113.0), (41.5, 115.5), (42.5, 116.0), (51.5, 116.0), (53.5, 119.0),
                 (54.5, 120.0), (65.5, 120.0), (67.5, 118.5), (69.5, 117.5), (71.5, 117.0), (81.5, 117.0),
                 (83.5, 116.5), (85.5, 115.5), (87.0, 115.0))
# The near side's hem (Z, Y) from its right edge (Z 38, upright to Y 64) to
# its left edge, which leans up to the top at Z 82: the U at row 105
# (lowest Y 37.5 at Z 55).
TARP_HEM = ((38, 64.0), (39, 61.0), (41, 52.0), (43, 47.0), (45, 42.5), (47, 41.0), (49, 40.0), (51, 39.0),
            (53, 38.0), (55, 37.5), (57, 38.0), (59, 39.0), (61, 40.0), (63, 41.5), (65, 43.5), (67, 45.5),
            (69, 48.0), (71, 50.5), (73, 52.5), (75, 55.0), (77, 58.0), (79, 71.0), (81, 90.0), (82, 114.5))
# The far side's rows (Y) below the shoulder: its left edge steps (x 205 out
# at rows 92-106: Z 85 between Y 45 and 75, 86.5 elsewhere).
TARP_FAR_YS = (100.0, 90.0, 76.0, 74.0, 60.0, 46.0, 44.0, 36.0, 26.0, 16.0)
# Relief, moving a point along (1, 1, 1) (its pixel stays; the art's shading
# gives the folds, lit from the right): near side, by Z: the edges +1, the
# left roll (x 213-216) +3, the central hollow (x 217-225) 0, the right
# panel (x 228-232) +2; the hem's rim stands +4 and its turned-up underside
# +3 (the left roll's rise spread over Z 65-73: over Z 71-73 it ran 14.5
# degrees off the view's ray and opened a bare band down x 216-217 with
# the camera moved). Far side: its left edge -1, the ridge (x 209-212) +2,
# creases -1.
TARP_NEAR_D = ((38, 1.0), (41, 2.0), (49, 2.0), (55, 0.0), (65, 0.0), (73, 3.0), (79, 3.0), (82, 1.0))
TARP_FAR_D = ((48, 0.0), (56, 0.0), (59, -1.0), (62, 0.0), (65, -1.0), (68, 0.0), (71, 2.0),
              (77, 2.0), (80, 0.0), (84, -1.0), (86, -1.0))


def _by(pts, x):
    """Piecewise-linear through (x, y) points sorted by x."""
    pts = sorted(pts)
    if x <= pts[0][0]:
        return pts[0][1]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


def _smooth(e0, e1, x):
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def tarp_column(z, kn=10):
    """One column across the beam at z: the near side from its hem up to its
    top, over the roll, and the far side down to the floor (a side not at
    this z collapses onto the roll, its triangles skipped)."""
    ytop = 110.5 + (min(z, 82.0) - 38) / 44 * 4
    rise = _smooth(39.5, 41.5, z)    # the right end flat on the beam's top
    tight = _smooth(80.0, 84.0, z)   # the flap's foot lies on the beam's top (x 206-211, rows 73-74)
    near = []
    ybot = _by(TARP_HEM, z)
    for i in range(kn):
        v = 1 - i / (kn - 1)                   # the hem first, the top last
        d = max(_by(TARP_NEAR_D, z), 4 * _smooth(0.75, 0.92, v) - _smooth(0.92, 1.0, v))
        y = ytop + v * (ybot - ytop)
        near.append((TARP_XN + d, y + d, z + d))
    ysh = _by(TARP_SHOULDER, z)
    zr = max(z, 39.5)
    # The roll's top no higher than the back shoulder drawn at its x (at the
    # crest's step it filled the notch at x 222-223).
    y1 = min(ysh + 2.5 * rise, _by(TARP_SHOULDER, z - 3.5) + 2.5)
    y0 = max(112.5, min(ytop + 2.5 * rise, _by(TARP_SHOULDER, z - 6.5) + 6.3))
    x0 = max(130.0, 134.0 - 1.33 * max(0.0, z - 81.0))
    y0 += (112.5 - y0) * tight
    roll = [(x0, y0, zr), (131.0, y1, zr), (TARP_XF, ysh, zr)]
    if z > 82:
        near = [roll[0]] * kn
    far = []
    for Y in TARP_FAR_YS:
        if z < 49:                             # its right edge at Z 49 (x 223.5)
            far.append(roll[-1])
            continue
        w = (ysh - Y) / (ysh - FLOOR)
        d = _by(TARP_FAR_D, z) * min(1.0, 4 * w) * min(1.0, 4 * (1 - w))
        zz = (85.0 if 45 <= Y <= 75 else 86.5) if z >= 86 else z
        far.append((TARP_XF + d, Y + d, zz + d))   # upright: its foot's floor line is X 127-128
    return near + roll + far


def tarp(m):
    zs = sorted(set([38.0 + i for i in range(49)] + [39.5]))
    m.strip("tarp", [tarp_column(z) for z in zs], SHEET)
    # The far side's foot flares back on the left (x 200-205, rows 112-121).
    m.poly("tarp", [(TARP_XF, 36.0, 86.5), (125.0, 31.5, 86.0), (122.0, 26.5, 85.0), (119.5, 23.0, 84.5),
                    (117.5, 21.0, 84.5), (117.5, FLOOR, 84.5), (121.0, FLOOR, 85.0), (124.0, FLOOR, 86.0),
                    (127.0, FLOOR, 87.0), (TARP_XF, FLOOR, 86.5)], SHEET)
    # A tongue of it on the floor under the near hem (x 221-228, rows
    # 111-117: its floor edge (222,115.5)-(226,117.5) runs along +X at Z 57,
    # to X 141.5): the far side's foot spilt forward under the beam. (A sheet
    # from the hem down to there took floor at x 224-227 rows 105-112.)
    m.poly("tarp", [(TARP_XF, 24.0, 52.5), (133.5, 23.0, 52.5), (135.0, 21.5, 52.5), (141.5, 21.5, 52.5),
                    (141.5, 21.5, 57.2), (TARP_XF, 21.5, 57.2)], SHEET)
    m.poly("tarp", [(141.5, 21.5, 52.5), (141.5, FLOOR, 52.5), (141.5, FLOOR, 57.2), (141.5, 21.5, 57.2)], SHEET)
    m.poly("tarp", [(TARP_XF, 21.5, 57.2), (141.5, 21.5, 57.2), (141.5, FLOOR, 57.2), (TARP_XF, FLOOR, 57.2)],
           SHEET)


# The anchor (a grapnel) standing at the tarp's foot, not cloth: its ring
# and rope (rows 113-121, a hole at (215-217, 115)), shank x 213-214, two
# flukes with feet on the floor at row 127.5 (row 128 is its shadow), its
# rope from the ring down to the floor at x 221-223 (rows 123-127). An
# upright card on the plane X + Z = 238, in front of the whole tarp (whose
# X + Z is at most 214), one quad per run of its pixels.
ANCHOR_S = 238.0
ANCHOR_RUNS = {113: [(213, 217)], 114: [(212, 218)], 115: [(212, 219)], 116: [(211, 220)],
               117: [(211, 216), (218, 220)], 118: [(211, 215), (218, 221)], 119: [(212, 215), (218, 222)],
               120: [(212, 215), (217, 217), (219, 221), (223, 223)], 121: [(211, 216), (218, 218), (220, 223)],
               122: [(210, 218), (220, 223)], 123: [(206, 208), (210, 217), (219, 223)], 124: [(206, 223)],
               125: [(206, 223)], 126: [(206, 223)], 127: [(207, 210), (217, 220), (221, 223)]}


def _on_card(sx, sy, s=ANCHOR_S):
    """The point of the plane X + Z = s drawn at (sx, sy)."""
    d = 2 * (sx - 184)
    return ((s + d) / 2, 2 * (s / 4 + 76 - sy), (s - d) / 2)


def anchor(m):
    for y, runs in ANCHOR_RUNS.items():
        for a, b in runs:
            m.poly("anchor", [_on_card(a - 0.5, y - 0.5), _on_card(b + 0.5, y - 0.5),
                              _on_card(b + 0.5, y + 0.5), _on_card(a - 0.5, y + 0.5)], SHEET)


def strip_card(m):
    """The pale strip right of the post (x 196-200, rows 95-121): an upright
    card on X 120 behind the post, its other half the cloth at x 184-187
    (rounded tops at rows 96-99); not the tarp, nor the cover's drape, which
    reaches x 196-199 only at rows 112-121."""
    m.poly("strip", [(120.0, FLOOR, 87.0), (120.0, 61.0, 87.0), (120.0, 64.0, 89.0), (120.0, 71.0, 96.0),
                     (120.0, 71.0, 100.0), (120.0, FLOOR, 100.0)], SHEET)

def build():
    m = Model("room71")
    m.box("right", (LX - T, FLOOR, RZ - T), (X1, TOP, RZ))
    m.box("left", (LX - TL, FLOOR, RZ), (LX, TOP, Z1))
    m.box("post", *POST)
    m.box("beam", *BEAM)
    m.box("beam", *BEAM_BACK)
    p0, p1, r0, r1 = BUCKET
    m.cyl("bucket", p0, p1, r0, r1=r1)
    m.box("crate_small", *CRATE_SMALL)
    m.box("crate_lid", *CRATE_LID)
    m.box("crates", *CRATES_LOWER)
    m.box("crates", *CRATES_UPPER)
    for name, x, z in BARRELS:
        m.cyl(name, (x, FLOOR, z), (x, BARREL_TOP, z), BARREL_R)
    for x, z in BALLS:
        m.cyl("balls", (x, FLOOR, z), (x, BALL_TOP, z), BALL_R)
    boat(m)
    tarp(m)
    anchor(m)
    strip_card(m)
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        scale = int(os.environ.get("R71_SCALE", 3))
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=scale, alpha=0.3)
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map71.inc")
    m.write_inc(out, 71, "tools/models/room71.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
