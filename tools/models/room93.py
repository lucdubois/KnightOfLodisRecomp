#!/usr/bin/env python3
"""Map 93: the ending's last picture, a hillside path. A dirt path runs from the
top right to the bottom left between fences of posts and rails; a big tree and
a second one (its leaves past the frame's top), berry bushes, a signpost, a
grey rock on the meadow by the ridge, a small tree on BG1 at the bottom. The
game's grid has only cells x 0-3, z 0-20 at height 1 (Y 16: the path); every
other cell is off the field (the port's ground at Y 0), so without a model
everything lay flat on the ground and the meadow sat 16 under the path.

    tools/models/room93_extract.py <dump>   writes room93_art.py (the art per object)
    tools/models/room93.py                  writes src/kol_model_map93.inc

The frame (states/ending_map93.state, camera at rest; flat 384 x 216): sx =
(X - Z)/2 + KX, sy = (X + Z)/4 - Y/2 + KY, KX 328, KY 44. A pixel is sampled at
its corner (integer sx, sy): every quad below runs from c - E to c + 1 - E
(E 0.1), so it holds column c only. At a depth D = X + Z a column's pixel is one
world point: X - Z = 2 (sx - KX), Y = 2 (D/4 + KY - sy). Everything here is
built column by column on the art's own pixels (room93_art.py), each column a
vertical line or ribbon in the world, so at rest no pixel moves.

The ground (part `ground`, TOP), from each column's first row under the far
hills (pale, top left: left as they are, off-field ground at Y 0 behind the
ridge) to the art's last row:
  - level at Y 16, the path's own height, out to X HILL_X (-55): the fences'
    feet line up on level lines along Z (posts' feet fit X = const to a pixel
    at Y 16) and the units stand at Y 16, so the meadow meets the path with no
    step;
  - behind that the meadow rises as a plane, HILL_GRADE 0.5 up per unit back
    (-X), to the ridge the art outlines against the far hills (its BG1 crest,
    and the grass step and dirt ledge at x 162-185 where it climbs out of the
    frame's top): the crest about Y 43, the rock's foot Y 34.5. A plane, so a
    column is two quads (hill, level) meeting on the crease.

The objects (outlines and masks: room93_extract.py):
  - Fences are vertical planes along Z (X fixed: the art's rails run at -0.5
    rows a column), each through its posts' feet at Y 16: the front fence by
    the path's left edge X -17.5 (posts C D E G), the back one X -43 (A B F H),
    the top right's X -11 (the front one's line, on past the gap), the near
    fence along the path's right side X 145 (the bottom one and the post at
    the right edge, R1); the right edge's lower post R2 is a fence along X
    (its rail stubs slope +0.5), Z 276.5 through its foot (row 162). The broken
    piece at the top right (a post's top and a rail fallen to the ground at
    its right end) is a plane along X, Z 95. Wood below a fence's foot (a
    bush's branches) is the ground's.
  - The signpost is a plane along X (its board's edges slope +0.5), Z 294.5,
    its post's foot under the purple-haired unit's head at row 110 (the post
    runs on under the sprite).
  - Plants and the rock are anchored in a V (the user's rule for rocks and
    plants, map 75): the bottom follows the two front edges of a square
    footprint on the ground (D = D_front - 2|u| at u columns from the middle, out to
    W columns), a column whose art ends lower stands on its own bottom; the
    ground between the art's bottom and the V stands up with it (`_v` parts,
    as drawn). Higher up each row is round about its own middle (K times a
    circle's bulge, as room75's lobes), eased in from the foot over BLEND rows.
    The trees are the same rule with a narrow V at the trunk's foot: their
    leaves, wider than the V, hang at the V's back corners' depth plus the
    round bulge, not on the ground under them. A column's depth changes at
    most SLOPE (1.2) a row from its foot up (a ribbon leaning more is classed
    as a floor), and never falls behind the ground at its pixel.
  - The BG1 tree at the bottom (its foot below the frame, row 194 by its
    height) is BG1 | BG1ART: lifted, it opens onto the ground drawn under it.
  - Flags: NOGAP on every object (the ground drawn round it on its layer is
    what it hid), ASDRAWN on the plants (their self-shadow read as dirt; the
    art shades its leaves), the ground stood up under a V SHEET | ASDRAWN |
    NOGAP. The fences, the sign and the rock take the model's shadows.

Left, true parallax (no art was drawn there): with the stick moved, the ground
behind the big tree's canopy (which stands 100-130 in front of the slope
and the second tree) shows as stretched curtains at its sides and, camera low,
under its left lobe; camera high, the ground behind every fence and bush shows
as short streaks over them, and the art's top edge over the modelled ground
hangs a band past the frame (the far hills, not modelled, stay at the frame's
top). The right edge's post and bushes stretch past the art as the strip did
before.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Model, TOP, BG1, BG1ART, NOGAP, ASDRAWN, SHEET, NOSHADOW, LIGHTPASS  # noqa: E402
import room93_art as art  # noqa: E402

KX, KY = float(art.KX), float(art.KY)
E = 0.1
YG = 16.0            # the meadow and the path
ROW_STEP = 4         # rows between a plant column's vertices
SLOPE = 1.2          # the most a plant column's depth changes a row


def point(sx, sy, D):
    d = 2.0 * (sx - KX)
    return ((D + d) / 2.0, 2.0 * (D / 4.0 + KY - sy), (D - d) / 2.0)


HILL_X = -55.0       # the meadow rises behind the back fence (X -43) ...
HILL_GRADE = 0.5     # ... half a unit up per unit back (-X), to the ridge


def level_D(row):
    """The depth of the level ground (Y 16) seen at a row's sample point."""
    return 4.0 * (row - KY) + 2.0 * YG


def ground_D(row, sx=None):
    """The depth of the ground seen at (sx, row): level at Y 16, or on the hill
    (X < HILL_X: Y = 16 + HILL_GRADE (HILL_X - X)), a plane too."""
    D = level_D(row)
    if sx is None:
        return D
    d = 2.0 * (sx - KX)
    if (D + d) / 2.0 >= HILL_X:
        return D
    g = HILL_GRADE
    return (4.0 * (row - KY) + 2.0 * YG + 2.0 * g * HILL_X - g * d) / (1.0 + g)


def ground_row(D, sx):
    """The row where depth D meets the ground (level or hill) at column sx."""
    d = 2.0 * (sx - KX)
    if (D + d) / 2.0 >= HILL_X:
        return foot_row(D)
    g = HILL_GRADE
    return (D * (1.0 + g) - 2.0 * YG - 2.0 * g * HILL_X + g * d) / 4.0 + KY


def foot_row(D):
    return (D - 2.0 * YG) / 4.0 + KY


# Flags: a gap a part bounds is the ground's (NOGAP: the art drew the ground
# round every object on its own layer, so the ground is what was hidden); the
# plants keep the art's own shading (ASDRAWN: their self-shadow read as dirt);
# the ground stood up under a V is left as drawn too.
V_FLAGS = SHEET | ASDRAWN | NOGAP
PLANE_FLAGS = NOGAP
PLANT_FLAGS = NOGAP | ASDRAWN


# Planes: depth as a function of sx.
def plane_z(X0):     # along Z, X = X0
    return lambda sx: 2.0 * X0 - 2.0 * (sx - KX)


def plane_x(Z0):     # along X, Z = Z0
    return lambda sx: 2.0 * Z0 + 2.0 * (sx - KX)


# (part, plane, its normal, extra pixels {column: [(r0, r1)]})
PLANES = [
    ("sign", plane_x(294.5), (0.0, 0.0, 1.0), {c: [(106, 110)] for c in range(179, 184)}),
    ("fence_front", plane_z(-17.5), (1.0, 0.0, 0.0), {}),
    ("fence_back", plane_z(-43.0), (1.0, 0.0, 0.0), {}),
    ("fence_tr", plane_z(-11.0), (1.0, 0.0, 0.0), {}),
    ("fence_tr_broken", plane_x(95.0), (0.0, 0.0, 1.0), {}),
    ("fence_r1", plane_z(145.0), (1.0, 0.0, 0.0), {}),
    ("fence_bottom", plane_z(145.0), (1.0, 0.0, 0.0), {}),
    ("leaves_bottom", plane_z(145.0), (1.0, 0.0, 0.0), {}),
    ("fence_r2", plane_x(276.5), (0.0, 0.0, 1.0), {}),
]

# Anchored in a V: (part, middle column, front row (the V's tip on the ground),
# half width W in columns, roundness K, blend rows, flags)
PLANTS = [
    ("rock", 133.5, 78.0, 14.0, 0.6, 6.0, NOGAP),
    ("bush_left", 84.0, 155.0, 11.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_top", 245.0, 65.0, 9.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_tr", 275.0, 47.5, 12.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_r1", 299.0, 138.0, 12.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_r", 299.0, 157.0, 14.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_r2", 282.0, 158.0, 6.0, 0.6, 6.0, PLANT_FLAGS),
    ("bush_bottom", 203.0, 190.0, 10.0, 0.6, 6.0, PLANT_FLAGS),
    ("tree", 210.5, 90.0, 5.0, 0.4, 10.0, PLANT_FLAGS),
    ("tree2", 225.5, 56.0, 4.0, 0.4, 10.0, PLANT_FLAGS),
    ("bush_bg1", 184.5, 194.0, 8.0, 0.5, 10.0, BG1 | BG1ART | PLANT_FLAGS),
]
ART_PX = {(c, r) for obj in art.OBJECTS.values() for c, rr in obj.items() for a, b in rr for r in range(a, b + 1)}


def runs_of(rows):
    rows = sorted(set(rows))
    out = []
    for r in rows:
        if out and out[-1][1] == r - 1:
            out[-1][1] = r
        else:
            out.append([r, r])
    return out


def rows_of(obj):
    return {c: [r for a, b in rr for r in range(a, b + 1)] for c, rr in obj.items()}


def quad(m, part, flags, corners, normals):
    """corners: top-left, top-right, bottom-right, bottom-left."""
    idx = [m.vert(p, n) for p, n in zip(corners, normals)]
    a, b, e, d = idx
    m.tris.append((a, b, e, part, flags))
    m.tris.append((a, e, d, part, flags))


def build_ground(m, taken):
    part = m.part("ground")
    n = (0.0, 1.0, 0.0)
    x0, y0, x1, y1 = art.ART
    for c in range(x0, x1 + 1):
        t = art.GROUND_TOP[c]
        if t > y1:
            continue
        xa, xb = c - E, c + 1 - E
        # the crease where the hill meets the level meadow, at each edge
        ka = foot_row(2.0 * HILL_X - 2.0 * (xa - KX))
        kb = foot_row(2.0 * HILL_X - 2.0 * (xb - KX))
        ya, yb = t - E, y1 + 1 - E
        nh = ((HILL_GRADE / math.sqrt(1.0 + HILL_GRADE ** 2), 1.0 / math.sqrt(1.0 + HILL_GRADE ** 2), 0.0))
        if ka > ya:      # the hill, down to the crease
            qa, qb = min(ka, yb), min(kb, yb)
            quad(m, part, TOP, [point(xa, ya, ground_D(ya, xa)), point(xb, ya, ground_D(ya, xb)),
                                point(xb, qb, ground_D(qb, xb)), point(xa, qa, ground_D(qa, xa))], [nh] * 4)
            ya_a, ya_b = qa, qb
        else:
            ya_a = ya_b = ya
        if yb > ya_a:
            quad(m, part, TOP, [point(xa, ya_a, level_D(ya_a)), point(xb, ya_b, level_D(ya_b)),
                                point(xb, yb, level_D(yb)), point(xa, yb, level_D(yb))], [n] * 4)


def build_plane(m, name, Dfn, n, extra, owned):
    part = m.part(name)
    px = rows_of(art.OBJECTS[name])
    for c, rr in extra.items():
        for a, b in rr:
            px.setdefault(c, []).extend(r for r in range(a, b + 1) if (c, r) not in owned)
    for c in sorted(px):
        # wood below the plane's foot (two rows: Y under 12) is the ground's
        lim = foot_row(Dfn(c + 0.4)) + 2.0
        keep = [r for r in px[c] if r <= lim]
        for r0, r1 in runs_of(keep):
            xa, xb = c - E, c + 1 - E
            ya, yb = r0 - E, r1 + 1 - E
            quad(m, part, PLANE_FLAGS, [point(xa, ya, Dfn(xa)), point(xb, ya, Dfn(xb)),
                              point(xb, yb, Dfn(xb)), point(xa, yb, Dfn(xa))], [n] * 4)
            for r in range(r0, r1 + 1):
                owned.add((c, r))


def build_plant(m, name, cx, rf, W, K, blend, flags, owned):
    part = m.part(name)
    part_v = m.part(name + "_v")
    px = rows_of(art.OBJECTS[name])
    D_front = ground_D(rf, cx)
    D_axis = D_front - 2.0 * W
    # each row's extent, for the round body
    ext = {}
    for c, rs in px.items():
        for r in rs:
            l, h = ext.get(r, (c, c))
            ext[r] = (min(l, c), max(h, c))
    kk = 1.0 / math.sqrt(2.0)

    def round_at(c, r):
        rr = min(max(r, min(ext)), max(ext))
        while rr not in ext:
            rr += 1
        l, h = ext[rr]
        mid = (l + h + 1) / 2.0
        w = (h - l + 1) / 2.0
        s = max(-0.98, min(0.98, (c + 0.5 - mid) / w))
        e = math.sqrt(1.0 - s * s)
        return D_axis + K * 2.0 * w * e, (kk * (e + s), 0.3, kk * (e - s))

    cols = set(px) | set(range(int(math.floor(cx - W)), int(math.ceil(cx + W)) + 1))
    for c in sorted(cols):
        u = abs(c + 0.4 - cx)
        rows = sorted(px.get(c, []))
        bottom = rows[-1] if rows else None
        D_v = D_front - 2.0 * min(u, W)
        v_row = ground_row(D_v, c + 0.4) if u <= W else None
        D_anchor = max(D_v, ground_D(bottom + 1, c + 0.4) if bottom is not None else -1e9)
        n_v = (1.0, 0.0, 0.0) if c + 0.4 >= cx else (0.0, 0.0, 1.0)
        # the column's depth from its foot up, a row at a time: the blend from
        # the anchor into the round body, its change held to SLOPE a row (a
        # ribbon leaning more is classed as a floor: normal y over 0.7 at 1.65)
        prof, nrm = {}, {}
        if rows:
            prof[bottom + 1], nrm[bottom + 1] = D_anchor, n_v
            for r in range(bottom, rows[0] - 1, -1):
                Dr, nr = round_at(c, r)
                t = min(1.0, max(0.0, (bottom + 1 - r) / blend))
                D = D_anchor + (Dr - D_anchor) * t
                D = min(max(D, prof[r + 1] - SLOPE), prof[r + 1] + SLOPE)
                # never behind the ground seen at that pixel (the round body's
                # edges recede: on the hill the ground there is nearer)
                prof[r] = max(D, ground_D(r - E, c + 0.4) + 2.0)
                nrm[r] = tuple(n_v[k] + (nr[k] - n_v[k]) * t for k in range(3))
        for r0, r1 in runs_of(rows):
            verts = []
            r = r0
            while True:
                rr = min(r, r1 + 1)
                verts.append((rr - E, prof[rr], nrm[rr]))
                if rr >= r1 + 1:
                    break
                r = rr + ROW_STEP
            for (ya, Da, na), (yb, Db, nb) in zip(verts, verts[1:]):
                quad(m, part, flags, [point(c - E, ya, Da), point(c + 1 - E, ya, Da),
                                      point(c + 1 - E, yb, Db), point(c - E, yb, Db)], [na, na, nb, nb])
            for r in range(r0, r1 + 1):
                owned.add((c, r))
        # the ground between the art's bottom and the V stands up with it
        if v_row is None:
            continue
        start = (bottom + 1) if bottom is not None else int(math.floor(ground_row(D_axis, c + 0.4)))
        stop = int(math.ceil(v_row)) - 1
        vr = [r for r in range(start, stop + 1) if (c, r) not in owned and (c, r) not in ART_PX and r <= art.ART[3]]
        D = D_v
        for r0, r1 in runs_of(vr):
            quad(m, part_v, V_FLAGS, [point(c - E, r0 - E, D), point(c + 1 - E, r0 - E, D),
                                      point(c + 1 - E, r1 + 1 - E, D), point(c - E, r1 + 1 - E, D)], [n_v] * 4)
            for r in range(r0, r1 + 1):
                owned.add((c, r))


# The far hills (top left, pale, over the ridge): distant mountains (the user,
# 2026-11-13, asked for them to move with the camera as distant
# mountains would). One upright plane facing the camera (a constant depth D, so the
# perspective moves and scales it whole, never stretched as the ground is), its
# depth HILLS_BACK behind the ridge's farthest crest point, so the ridge is in front
# of it everywhere; left as drawn, casting nothing. Each column's hills: the art's
# top row down to GROUND_TOP. Placed farther back in this projection it would rise
# off the picture (its horizon is ~900 rows over the view: tried at 150-2000), so
# the distance is the warp's (kol_perspective, kFarParts): this part's pixels move
# only a share of what the camera moves them (kModel93FarPart, written after the
# model), as far mountains move less than the ridge in front of them.
HILLS_BACK = float(os.environ.get("ROOM93_HILLS_BACK", "48"))


def build_hills(m):
    part = m.part("far_hills")
    x0, y0, x1, y1 = art.ART
    cols = [c for c in range(x0, x1 + 1) if art.GROUND_TOP[c] > y0]
    if not cols:
        return
    crest = min(ground_D(art.GROUND_TOP[c] - E, c) for c in cols)
    D = crest - HILLS_BACK
    n = (1.0 / math.sqrt(2.0), 0.0, 1.0 / math.sqrt(2.0))
    for c in cols:
        xa, xb = c - E, c + 1 - E
        ya, yb = y0 - E, art.GROUND_TOP[c] - E
        quad(m, part, ASDRAWN | NOSHADOW | LIGHTPASS, [point(xa, ya, D), point(xb, ya, D),
                                                       point(xb, yb, D), point(xa, yb, D)], [n] * 4)


def build():
    m = Model("room93")
    owned = set()
    for name, cx, rf, W, K, blend, flags in PLANTS:
        build_plant(m, name, cx, rf, W, K, blend, flags, owned)
    for name, Dfn, n, extra in PLANES:
        build_plane(m, name, Dfn, n, extra, owned)
    build_ground(m, owned)
    build_hills(m)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map93.inc")
    m.write_inc(out, 93, "tools/models/room93.py")
    with open(out, "a") as o:
        o.write("// The far hills' part (kol_focus kFarParts: moved a share of the camera's move).\n")
        o.write("constexpr int kModel93FarPart = %d;\n" % m.part("far_hills"))
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
