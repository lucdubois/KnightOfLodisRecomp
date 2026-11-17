#!/usr/bin/env python3
"""The ship's fits to the user's drawings (2026-10-24, states/JL), shared by
both ships: map 58's and map 97's are one drawing, tile for tile (README "The
ship on map 58" and the notes after it).

    ship_fit.build(quay)    ship97.build with the fits, in map 97's world

Measured on map 58's flat frame (FRAME) and turned into map 97's world by
map 58's move (MAP58), so they hold on map 97 as they are: the bow under
the rail (the waterline and the stem), the far rail round the nose, the
near rail's top flat, the bowsprit's length.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ship97  # noqa: E402

MAP58 = (-416.0, 32.0, -256.0)   # map 58's ship is map 97's moved by this (ship58.OFFSET)


# The bow under the rail as the user drew it on map 58's flat frame
# (2026-10-24, states/JL: the boat's front did not reach the water),
# in map 97's world: the waterline keeps the beam to X 622, then closes in a
# quarter ellipse to the stem at 678; the stem rises forward from there to
# the rail's (726 at 56). Map 97's plan leans forward going DOWN (BOW_RAKE):
# here the art's bottom edge met it 6-30 over the water, and the bow floated
# when the camera went down.
WATER_BOW = (622.0, 56.0)   # where the waterline leaves the beam, and its length on to the stem
STEM = [(-5.0, 678.0), (2.4, 688.0), (14.8, 698.0), (31.1, 708.0), (45.9, 718.0), (56.0, 726.0)]


def stem_x(y):
    if y <= STEM[0][0]:
        return STEM[0][1]
    for (y0, x0), (y1, x1) in zip(STEM, STEM[1:]):
        if y <= y1:
            return x0 + (x1 - x0) * (y - y0) / (y1 - y0)
    return STEM[-1][1]


# The far rail round the nose as the art draws it (the user's magenta line,
# the same day, asked for it joined up and rising toward the front): its
# outer edge runs nearly straight down, columns 259-262 from row 113 to the
# bowsprit's root at row 128 (map 58's flat frame of states/JL). The plan's
# round nose put it 5 px further out, and RAIL_TOP (a row per column, where
# the edge is nearly upright) gave the point beside the stem 112 against the
# stem's 75: the curve broke in two. Points 1 to N_FIT - 1 of the far nose
# now sit on that edge, evenly by row from point N_FIT's to the stem's, their
# tops from those rows; the stem's top is the edge's foot.
FRAME = (256.0, 36.0)   # that frame's kx + ox, ky + oy
FAR_EDGE = [(113.0, 259.0), (118.0, 260.0), (126.0, 261.0), (128.0, 262.0)]   # (row, the outline's column)
STEM_ROW = 128.0
N_FIT = 9


def frame_point(col, row, z):
    """(X, Y), in map 97's world, of the point at depth Z (map 97's) that
    map 58's frame shows at (col, row)."""
    z58 = z + MAP58[2]
    x58 = z58 + 2.0 * (col - FRAME[0])
    y58 = 2.0 * ((x58 + z58) / 4.0 + FRAME[1] - row)
    return x58 - MAP58[0], y58 - MAP58[1]


def frame_row(x, y, z):
    return (x + MAP58[0] + z + MAP58[2]) / 4.0 - (y + MAP58[1]) / 2.0 + FRAME[1]


def edge_col(row):
    """The far rail's last column inside its outline at a row."""
    pts = FAR_EDGE
    if row <= pts[0][0]:
        return pts[0][1] - 1.0
    for (r0, c0), (r1, c1) in zip(pts, pts[1:]):
        if row <= r1:
            return c0 + (c1 - c0) * (row - r0) / (r1 - r0) - 1.0
    return pts[-1][1] - 1.0


_far_nose = {}


def far_nose():
    """{plan index: (X, top)} for the stem (point 0 and its twin, the last)
    and the far nose's points fitted to the art's edge."""
    if not _far_nose:
        rail = ship97.plan(ship97.RAIL)
        x9, z9 = rail[N_FIT]
        fit, ship97.RAIL_FIT = ship97.RAIL_FIT, None   # (point N_FIT's own top)
        try:
            row9 = frame_row(x9, ship97.top_at(N_FIT, x9, z9), z9)
        finally:
            ship97.RAIL_FIT = fit
        for i in range(1, N_FIT):
            row = STEM_ROW + (row9 - STEM_ROW) * i / N_FIT
            _far_nose[i] = frame_point(edge_col(row) + 0.5, row, rail[i][1])
        xs, zs = rail[0]
        top = 2.0 * ((xs + MAP58[0] + zs + MAP58[2]) / 4.0 + FRAME[1] - STEM_ROW) - MAP58[1]
        _far_nose[0] = _far_nose[len(rail) - 1] = (xs, top)
    return _far_nose


def fitted_rail(inset):
    """The rail's plan with the far nose on the art's edge (an inset plan
    moved as the outer one is)."""
    rail = ship97.plan(ship97.RAIL, inset)
    outer = ship97.plan(ship97.RAIL)
    for i, (x, _) in far_nose().items():
        rail[i] = (x + rail[i][0] - outer[i][0], rail[i][1])
    return rail


def rail_fit(i, x, z):
    """ship97.RAIL_FIT: the fitted points' tops."""
    fit = far_nose().get(i)
    return fit[1] if fit else None


# The near rail's top, flat (the user, the same day, asked for a flat part on the
# boat's side, as the near walls looked thin). The art draws it as
# a light band 3-5 rows deep over the hull's planks, under a dark outline;
# the model's rail dropped inward (CAP_DROP, the far side's rounding), so on
# the near side it faced away and the deck took the band at 48. Its inner
# edge now lies level with its outer top, on that outline's row (measured
# per column on the flat frame): the outline is the top's edge, so with the
# camera up the deck uncovered beside it is stretched from the deck's own
# planks (from the outline, it streaked dark).
NEAR_CAP_EDGE = [(94, 105.0), (110, 116.0), (130, 126.0), (150, 136.0), (158, 140.0), (164, 143.0), (170, 145.0),
                 (174, 146.0), (178, 147.0), (186, 148.0), (202, 148.0), (212, 147.0), (218, 146.0), (224, 145.0),
                 (228, 144.0), (232, 142.0), (236, 141.0), (240, 139.0), (244, 137.0), (246, 136.0)]


def cap_edge_row(col):
    """The outline's row at a column (its end segments carried on)."""
    pts = NEAR_CAP_EDGE
    segs = list(zip(pts, pts[1:]))
    (c0, r0), (c1, r1) = next((s for s in segs if col <= s[1][0]), segs[-1])
    return r0 + (r1 - r0) * (col - c0) / (c1 - c0)


def near_cap(i, top, cap):
    """ship97.CAP_IN: on the near side, the rail's inner edge level with its
    top, moved in (toward the default's) until it sits on the art's row;
    where the top is on that row already, or past the measured columns (by
    the stem), level at the default's width."""
    x, y, z = top
    if z <= ship97.ZC:
        return None
    level = (cap[0], y, cap[2])
    dx, dz = cap[0] - x, cap[2] - z
    d = math.hypot(dx, dz)
    col = (x + MAP58[0] - z - MAP58[2]) / 2.0 + FRAME[0]
    if d < 1e-6 or not NEAR_CAP_EDGE[0][0] <= col <= NEAR_CAP_EDGE[-1][0]:
        return level
    dx, dz = dx / d, dz / d
    def miss(t):
        px, pz = x + dx * t, z + dz * t
        return frame_row(px, y, pz) - cap_edge_row((px + MAP58[0] - pz - MAP58[2]) / 2.0 + FRAME[0])
    lo, hi = d, 60.0
    if miss(lo) <= 0.0 or miss(hi) > 0.0:
        return level
    for _ in range(30):
        t = (lo + hi) / 2.0
        lo, hi = (t, hi) if miss(t) > 0.0 else (lo, t)
    t = (lo + hi) / 2.0
    return (x + dx * t, y, z + dz * t)


# The bowsprit as the user drew it (the same day: too long on the right,
# a little short on the left): from column 245 to 294.5 on the flat
# frame of states/JL, where map 97's reached 249 to 320 (its tip over the
# water). The same axis, its ends at X 707 and 799 (map 97's world; its
# rounded ends reach 2 px past them).
BOWSPRIT_X = (707.0, 799.0)


def bowsprit():
    (x0, y0, z0), (x1, y1, z1) = ship97.BOWSPRIT
    at = lambda x: (x, y0 + (y1 - y0) * (x - x0) / (x1 - x0), z0)
    return at(BOWSPRIT_X[0]), at(BOWSPRIT_X[1])


def bow_plan(y, inset, pts):
    """ship97.BOW_PLAN: from the rail up, the far nose on the art's edge;
    under it, each point of that plan moved aft along X, at its own distance
    off the keel, toward the waterline's ellipse as the stem's profile does;
    the side behind it spread from the transom's corner to where the
    ellipse begins. The transom keeps its own plan."""
    rail = fitted_rail(inset)
    if y >= ship97.RAIL:
        fit = far_nose()
        return [rail[i] if i in fit else p for i, p in enumerate(pts)]
    f = (stem_x(y) - STEM[0][1]) / (STEM[-1][1] - STEM[0][1])
    xe, a = WATER_BOW
    a, b = a - inset, ship97.W_RAIL - inset
    xa, xa_rail = ship97.corner_x(y), ship97.corner_x(ship97.RAIL)
    out = []
    for i, (p, (xr, zr)) in enumerate(zip(pts, rail)):
        if ship97.is_transom(i):
            out.append(p)
        elif xr >= xe:
            off = min(abs(zr - ship97.ZC), b)
            xw = xe + a * math.sqrt(1.0 - (off / b) ** 2)
            out.append((xw + (xr - xw) * f, zr))
        else:
            out.append((xa + (xr - xa_rail) * (xe - xa) / (xe - xa_rail), zr))
    return out


# The stem's head under the bowsprit (the curl), on map 97: its art is BG0
# there and the hull's nose, narrowed by the fits, no longer reaches it
# (70 of its pixels fell to the water's height). A plate on the keel's
# line from the stem to the curl's last column (column 281 on map 97's
# viewer frame at cell 17,17, row 131 the map's edge), up under the
# bowsprit, and the bowsprit's shaded underside beside it (to column 287).
# Map 58 draws them on BG1: the gap fill gives them to the parts round
# them, so they are left out there.
STEM_HEAD = [[(725.0, 54.0), (744.0, 63.5), (744.0, 87.0), (725.0, 87.0)],
             [(744.0, 80.0), (756.0, 81.5), (756.0, 92.0), (744.0, 92.0)]]   # (X, Y) on the keel's line


def stem_head(m):
    for plate in STEM_HEAD:
        m.poly("hull", [(x, y, ship97.ZC) for x, y in plate])


def build(quay=True):
    """ship97.build with the fits hooked in (and taken out again)."""
    sprit = ship97.BOWSPRIT
    ship97.BOW_PLAN, ship97.RAIL_FIT, ship97.CAP_IN = bow_plan, rail_fit, near_cap
    ship97.BOWSPRIT = bowsprit()
    try:
        return ship97.build(quay=quay)
    finally:
        ship97.BOW_PLAN = ship97.RAIL_FIT = ship97.CAP_IN = None
        ship97.BOWSPRIT = sprit
