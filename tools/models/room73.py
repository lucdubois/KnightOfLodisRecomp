#!/usr/bin/env python3
"""Map 73: a flooded cavern. Two straight outer rock walls, free-standing and
wall-bound rock pillars with flat rounded caps, waterfalls between them, and on
the dais a ruined arch on two piers among three small round columns.

    python3 model.py     prints the vertex and triangle counts
    build_parts(m)       adds the scene to a kit Model
    (check.py beside this file measures it against flat.png.)

Projection of flat.png (570x465):  sx = (X - Z)/2 + 318,  sy = (X + Z)/4 - Y/2 + 177.
Every number comes from flat.png / layers.raw / grid.txt; "row" and "col" are its
pixels, sampled at their corner, so an edge between col a and a+1 is a + 0.5.

What the art says (measured):
* The outer walls are two plain planes. The last row of the dark band over them
  is 286 columns (left) and 222 columns (right) of one straight 2:1 line, every
  column within 0.25 px: nothing bulges in plan; the ribs are texture.
* The "curve" of the rocks is the pillars': every pillar sits exactly on its
  grid cells (cap at Y = 16 * height, foot corner on the cell's corner: e.g. the
  foot tip of cell 2,5 at (270,233), of 11,2 at (462,281)), but its free corner
  is rounded and the rock widens downward: the silhouettes below are read row by
  row where the rock meets water or a dark outline (41,49,57). A pillar's two
  camera-facing faces meet in a sharp foot tip, so the front corner stays tight.
* The waterfalls are exact cell boxes (cap at the cell's height, sheet on the
  cell's +X or +Z face).
* The ruin is a row along Z 208: column, pier, opening, pier, column on cells
  6..10,6, and a third column (shorter by 8) alone on cell 6,8. The arch and
  columns are BG1 over water, falls and dais on BG0: BG1 | OVER.
* A fourth column stands on cell 10,8, in no flat.png: the game draws it as a
  scene actor's sprite (actor 0x68, at X 336, Y 80, Z 272), in every scene on
  this map, so the viewer's hidden sprites hid it. It is the third column's art
  64 cols and 32 rows on (574 of 582 pixels equal in states/thor/0113), so the
  same column 128 along X. The sprite goes on down in the dais's own art (rows
  under its foot equal the floor under them): those stay the floor's.

How it goes together: a rock is a closed ring strip with a flat cap; a side
that touches what stands behind it is pushed EMBED inside that (never lying on
its plane), the crease between merged rocks is where one solid enters the other.
Walls and rocks are LEE (in shade like the terrain's walls); falls are left as
drawn. The spikes in the pool, the fire pots, floor, dais, steps stay terrain.
"""
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model as _kit  # noqa: E402
Model, BG1, OVER, LEE, TOP, SHEET, ACTOR = _kit.Model, _kit.BG1, _kit.OVER, _kit.LEE, _kit.TOP, _kit.SHEET, _kit.ACTOR

KX, KY = 318.0, 177.0
POOL = 32.0       # grid height 2, the water
WALK = 48.0       # grid height 3, the walkway
# A foot on the water: first water row under a rock's +Z face minus the grid's Y 32
# line is 0 +- 0.5 at every column of cells 2,5 11,2 14,3 8,1 13,2 (they are drawn a
# row short of the line), and pixels sample at their corner: half a row up.
FOOT = POOL + 1.2
FOOTW = WALK + 0.75  # the rocks' feet on the walkway: scanned, 23 wrong against 61 at + 0
EMBED = 4.0       # a flush side's hidden back goes this far into what it touches

WALL_X, WALL_Z = 32.0, 32.0     # inner faces of the ring (cells x 0, z 0)
# Last dark band row + 1: y = 33 + (318 - x)/2 - 1.25 on the left wall, - 0.75 on
# the right, 286 and 222 columns measured: the tops at Y 322.5 and 321.5.
WALL_TOP_LEFT, WALL_TOP_RIGHT = 322.5, 321.5
FIELD_X, FIELD_Z = 480.0, 608.0   # the art's cut ends (cells x 14, z 18)
WALL_END = 1.0    # the walls stop short of the ring's own cut ends by half a col


def S(p):
    return ((p[0] - p[2]) / 2.0 + KX, (p[0] + p[2]) / 4.0 - p[1] / 2.0 + KY)


def _interp(table, row):
    """Piecewise-linear in row; held past either end."""
    if row <= table[0][0]:
        return table[0][1]
    for (a, xa), (b, xb) in zip(table, table[1:]):
        if row <= b:
            return xa + (xb - xa) * (row - a) / float(b - a)
    return table[-1][1]


def _arc(cx, cz, r, a0, a1, n):
    return [(cx + r * math.cos(a0 + (a1 - a0) * i / float(n)), cz + r * math.sin(a0 + (a1 - a0) * i / float(n)))
            for i in range(n + 1)]


def _ring(x0, x1, z0, z1, flush, rc, rf, cut_x, cut_z):
    """The plan outline A (back) B (right) C (front) D (left), clockwise on the
    screen seen from above. flush: 'x' the -X side touches what stands behind it
    (the free corner is B), 'z' the -Z side does (the free corner is D), 'xz'
    both (no free corner). Hidden sides go EMBED past the flush plane."""
    ex = EMBED if 'x' in flush else 0.0
    ez = EMBED if 'z' in flush else 0.0
    pts = [(x0 - ex, z0 - ez)]
    # B
    if flush == 'x' and rc > 0.01 and not cut_x:
        pts += _arc(x1 - rc, z0 + rc, rc, -math.pi / 2, 0.0, 3)
    else:
        # A cut end stays embedded too, except at the wall, where the ring's own cut
        # end (terrain, cols 542-558) begins.
        if cut_x and z0 <= WALL_Z:
            pts += [(x1, z0 + 0.5)] * 4
        elif cut_x:      # a hair inside the plane, so the hidden back never ties on it
            pts += [(x1 - 1.0, z0 - ez)] * 4
        else:
            pts += [(x1, z0 - ez)] * 4
    # the +X side's middle, then C, then the +Z side's middle
    r = 0.0 if (cut_x or cut_z) else rf
    c = _arc(x1 - r, z1 - r, r, 0.0, math.pi / 2, 2)
    pts.append(((pts[-1][0] + c[0][0]) / 2.0, (pts[-1][1] + c[0][1]) / 2.0))
    pts += c
    # D
    if flush == 'z' and rc > 0.01 and not cut_z:
        d = _arc(x0 + rc, z1 - rc, rc, math.pi / 2, math.pi, 3)
    else:
        if cut_z and x0 <= WALL_X:
            d = [(x0 + 0.5, z1)] * 4
        elif cut_z:
            d = [(x0 - ex, z1 - 1.0)] * 4
        else:
            d = [(x0 - ex, z1)] * 4
    pts.append(((pts[-1][0] + d[0][0]) / 2.0, (pts[-1][1] + d[0][1]) / 2.0))
    pts += d
    return pts


def rock(m, name, cell, ycap, yfoot, flush, sil=None, other=None, rc=8.0, rf=3.0, base=None):
    """One pillar. cell = (x0, x1, z0, z1) in cells, its plan in world units x32.
    sil: the free corner's silhouette, [(row, edge col)...] from the cap down.
    other: the other side's, where it meets water. The radius of the free corner
    follows the silhouette (r = inset / 0.293) so the rock is round at the top
    and tight at the foot, as drawn; what is left is taken by shifting the ring
    sideways on the screen. base: the Y a cut face runs down to (the field's edge)."""
    x0, x1, z0, z1 = cell[0] * 32.0, cell[1] * 32.0, cell[2] * 32.0, cell[3] * 32.0
    cut_x, cut_z = x1 >= FIELD_X, z1 >= FIELD_Z
    fl = LEE
    # The nominal silhouette corners.
    u_b = (x1 - (z0 - (EMBED if 'z' in flush else 0.0) * 0)) / 2.0
    u_d = (x0 - z1) / 2.0
    u_f = (x1 - z1) / 2.0
    v_b = (x1 + z0) / 4.0
    v_d = (x0 + z1) / 4.0
    if flush == 'x':
        u_e, v_e, sgn = u_b, v_b, 1.0
    else:
        u_e, v_e, sgn = u_d, v_d, -1.0
    if sil:
        inset = sgn * (u_e - (sil[0][1] - KX))
        rc = max(0.0, inset / 0.2929)
    rows = {ycap: None, yfoot: None}
    for tab, ve in ((sil, v_e), (other, v_d if flush == 'x' else v_b)):
        for (row, _) in (tab or ()):
            y = 2.0 * (ve + KY - row)
            if yfoot < y < ycap:
                rows[y] = None
    ys = sorted(rows, reverse=True)
    # Thin the rows a little: closer than 4 units adds nothing.
    keep = [ys[0]]
    for y in ys[1:-1]:
        if keep[-1] - y >= 4.0 and y - ys[-1] >= 4.0:
            keep.append(y)
    keep.append(ys[-1])
    out = []
    for k, y in enumerate(keep):
        row_e = v_e - y / 2.0 + KY
        if sil:
            edge = _interp(sil, row_e) - KX
            r = max(0.0, sgn * (u_e - edge) / 0.2929)
            r = max(0.5, min(r, 0.8 * min(x1 - x0, z1 - z0)))
        else:
            # no silhouette seen: round at the top, tight at the foot
            r = max(0.5, rc * (1.0 - float(k) / max(1, len(keep) - 1)) ** 0.5)
        rf_y = rf if k < len(keep) - 1 else min(rf, 1.0)
        ring = _ring(x0, x1, z0, z1, flush, r, rf_y, cut_x, cut_z)
        # Shift sideways: the silhouette corner onto its column.
        us = [(p[0] - p[1]) / 2.0 for p in ring]
        vs = [(p[0] + p[1]) / 4.0 for p in ring]
        res = []
        if flush == 'x':
            umax = max(us[1:5])
            hi = _interp(sil, row_e) - KX if sil else umax
            lo_nom, lo = u_d, u_d
            if other:
                lo = _interp(other, v_d - y / 2.0 + KY) - KX
        else:
            umin = min(us[-4:])
            lo_nom = umin
            lo = _interp(sil, row_e) - KX if sil else umin
            umax = u_b
            hi = u_b
            if other:
                hi = _interp(other, v_b - y / 2.0 + KY) - KX
        for u, v in zip(us, vs):
            if u >= u_f:
                un = u_f + (u - u_f) * (hi - u_f) / max(1e-6, (umax - u_f))
            else:
                un = u_f - (u_f - u) * (u_f - lo) / max(1e-6, (u_f - lo_nom))
            res.append((un + 2.0 * v, y, 2.0 * v - un))
        out.append(res + res[:1])
    m.strip(name, out, fl)
    # The cap: flat, its normal straight up, fanned from its middle.
    part = m.part(name)
    cap = []
    for p in out[0][:-1]:
        if not cap or abs(p[0] - cap[-1][0]) + abs(p[2] - cap[-1][2]) > 1e-3:
            cap.append(p)
    mid = (sum(p[0] for p in cap) / len(cap), ycap, sum(p[2] for p in cap) / len(cap))
    up = (0.0, 1.0, 0.0)
    ic = m.vert(mid, up)
    ring_i = [m.vert(p, up) for p in cap]
    for i in range(len(cap)):
        m.tris.append((ic, ring_i[(i + 1) % len(cap)], ring_i[i], part, 0))
    if base is not None:
        if cut_z:
            xe = x0 + 0.5 if x0 <= WALL_X else x0
            m.poly(name, [(xe, base, z1), (x1, base, z1), (x1, yfoot + 2.0, z1), (xe, yfoot + 2.0, z1)], fl)
        if cut_x:
            ze = z0 + 0.5 if z0 <= WALL_Z else z0
            m.poly(name, [(x1, base, z1), (x1, base, ze), (x1, yfoot + 2.0, ze), (x1, yfoot + 2.0, z1)], fl)


# Spike 9,3 stands in front of rock 8,1's foot: the ring stops at Y 49 (row 232.5 at
# col 413.5) and the lower face is a polygon round the spike's rows 234-245.
ROCK_8_1_CUT = 49.0
ROCK_8_1_LOW = [(413.5, 232.5), (430.0, 240.5), (430.0, 248.4), (423.5, 245.1), (423.5, 244.0), (422.5, 241.0),
                (421.5, 239.0), (420.5, 237.0), (419.5, 235.0), (418.5, 233.5), (415.5, 233.5), (414.5, 236.5),
                (413.5, 238.5)]

# name, cells (x0, x1, z0, z1), cap Y, foot Y, flush side, silhouette of the free
# corner [(row, edge col)...], the other side where it meets water.
# 'x': against what stands at -X, its free corner is the right one, the edge read
# as last rock col + 0.5; 'z': against -Z, its free corner the left one, first rock
# col - 0.5. Rows from flat.png: rock | water, or rock | dark outline (41,49,57)
# with the outline counted as rock.
ROCKS = [
    # The corner mass: Y 288 an L, Y 256 an L in front of it, two lower blocks.
    ("rock_corner_a", (1, 4, 1, 2), 288.0, FOOT, 'xz', None, None),
    ("rock_corner_a", (1, 2, 2, 3), 288.0, FOOT, 'xz', None, None),
    ("rock_corner_b", (2, 3, 2, 3), 256.0, FOOT, 'xz', None, None),
    ("rock_corner_b", (1, 3, 3, 4), 256.0, FOOT, 'xz', None, None),
    ("rock_corner_c", (3, 4, 2, 3), 224.0, FOOT, 'xz', None, [(100, 349.5), (128, 349.5)]),
    ("rock_corner_d", (3, 4, 3, 4), 128.0, FOOT, 'xz', None, [(136, 333.5), (196, 333.5)]),
    # Along the right wall (Z 32) and in front of it.
    ("rock_6_1", (6, 7, 1, 2), 272.0, FOOT, 'z', [(105, 384.5), (110, 383.5), (114, 381.5)], None),
    ("rock_6_2", (6, 7, 2, 3), 208.0, FOOT, 'z', [(145, 368.5), (150, 367.5), (154, 365.5)], [(152, 397.5), (232, 397.5)]),
    ("rock_8_1", (8, 9, 1, 2), 256.0, ROCK_8_1_CUT, 'z', [(129, 416.5), (134, 415.5), (138, 413.5)], None),
    ("rock_11_1", (11, 12, 1, 2), 272.0, FOOT, 'z', [(145, 464.5), (150, 463.5), (154, 461.5)], None),
    ("rock_11_2", (11, 12, 2, 3), 176.0, FOOT, 'z', [(201, 450.5), (219, 450.5), (220, 449.5), (231, 449.5), (237, 447.5), (249, 447.5), (254, 445.5)], [(200, 477.5), (224, 477.5)]),
    ("rock_13_1", (13, 15, 1, 2), 240.0, FOOT, 'z', [(177, 496.5), (182, 495.5), (186, 493.5)], None),
    ("rock_13_2", (13, 15, 2, 3), 144.0, FOOT, 'z', [(233, 480.5), (238, 479.5), (242, 477.5)], None),
    ("rock_14_3", (14, 15, 3, 4), 64.0, FOOT, 'z', [(289, 480.5), (294, 479.5), (298, 477.5)], None),
    # Along the left wall (X 32) and in front of it.
    ("rock_1_5", (1, 2, 5, 6), 288.0, FOOT, 'x', [(89, 267.5), (94, 269.5)], None),
    ("rock_2_5", (2, 3, 5, 6), 192.0, FOOT, 'x', [(145, 283.5), (150, 285.5)], [(140, 253.5), (224, 253.5)]),
    ("rock_1_8", (1, 2, 8, 9), 240.0, FOOT, 'x', [(137, 219.5), (142, 221.5)], None),
    ("rock_1_10", (1, 2, 10, 11), 272.0, FOOT, 'x', [(137, 183.5), (155, 184.5), (159, 186.5), (176, 186.5), (186, 189.5)], None),
    ("rock_1_11", (1, 2, 11, 12), 256.0, FOOT, 'x', None, None),
    ("rock_2_11", (2, 3, 11, 12), 160.0, FOOT, 'x', [(209, 183.5), (227, 184.5), (231, 186.5), (248, 186.5), (258, 189.5)], [(206, 157.5), (240, 157.5)]),
    ("rock_1_14", (1, 2, 14, 16), 240.0, FOOTW, 'x', [(185, 123.5), (190, 125.5)], None),
    ("rock_1_16", (1, 2, 16, 17), 224.0, FOOTW, 'x', None, None),
    ("rock_1_17", (1, 2, 17, 19), 208.0, FOOTW, 'x', None, None),
    ("rock_2_14", (2, 3, 14, 15), 112.0, FOOTW, 'x', [(257, 139.5), (262, 141.5)], None),
    ("rock_2_17", (2, 3, 17, 18), 176.0, FOOTW, 'x', [(249, 87.5), (267, 88.5), (271, 90.5), (288, 90.5), (298, 93.5)], None),
    ("rock_2_18", (2, 3, 18, 19), 144.0, FOOTW, 'x', None, None),
]
# The cut ends run down to Y 0: the lower left corner (30,337) (46,345) (62,353),
# the lower right (494,329) (510,321) (526,313).
CUT_BASE = 0.0

# The falls: exact cell boxes. name, plan (x0, x1, z0, z1) in cells, top Y, foot Y,
# the axis of the sheet. Foot on the pool, or on the cap of the fall below it.
# The water is drawn a row over the cell's own edge (scanned over all twelve: rock or
# wall taking water + fall taking rock is 88 pixels at + 2, 159 at + 1, 293 at + 0, 170 at + 3).
FALL_LIFT = 2.0
FALLS = [
    ("fall_4_1", (4, 6, 1, 2), 256.0, 192.0, 'z'),
    ("fall_4_2", (4, 6, 2, 3), 192.0, FOOT, 'z'),
    ("fall_7_1", (7, 8, 1, 2), 192.0, FOOT, 'z'),
    ("fall_9_1", (9, 11, 1, 2), 240.0, FOOT, 'z'),
    ("fall_12_1", (12, 13, 1, 2), 208.0, 128.0, 'z'),
    ("fall_12_2", (12, 13, 2, 3), 128.0, FOOT, 'z'),
    ("fall_1_4", (1, 2, 4, 5), 240.0, 176.0, 'x'),
    ("fall_2_4", (2, 3, 4, 5), 176.0, FOOT, 'x'),
    ("fall_1_6", (1, 2, 6, 8), 224.0, FOOT, 'x'),
    ("fall_1_9", (1, 2, 9, 10), 176.0, FOOT, 'x'),
    ("fall_1_12", (1, 2, 12, 14), 208.0, 96.0, 'x'),
    ("fall_2_12", (2, 3, 12, 14), 96.0, FOOT, 'x'),
]


def on_x(sx, sy, x):
    """The point on the plane X = x on the line of sight of pixel (sx, sy)."""
    z = x - 2.0 * (sx - KX)
    return (x, 2.0 * ((x + z) / 4.0 + KY - sy), z)


def on_z(sx, sy, z):
    x = z + 2.0 * (sx - KX)
    return (x, 2.0 * ((x + z) / 4.0 + KY - sy), z)


def flat(m, name, pts, flags=0):
    """A flat polygon, concave allowed, ear-clipped on the screen; pts in order."""
    part = m.part(name)
    n = _kit.norm(_kit.cross(_kit.sub(pts[1], pts[0]), _kit.sub(pts[2], pts[0])))
    if _kit.dot(n, (1.0, 1.0, 1.0)) < 0:
        n = _kit.mul(n, -1.0)
    idx = [m.vert(p, n) for p in pts]
    P = [S(p) for p in pts]
    area = sum(P[i][0] * P[(i + 1) % len(P)][1] - P[(i + 1) % len(P)][0] * P[i][1] for i in range(len(P)))
    order = list(range(len(P)))
    if area < 0:
        order.reverse()

    def cross2(a, b, c):
        return (P[b][0] - P[a][0]) * (P[c][1] - P[a][1]) - (P[b][1] - P[a][1]) * (P[c][0] - P[a][0])

    def inside(q, a, b, c):
        return cross2(a, b, q) >= -1e-9 and cross2(b, c, q) >= -1e-9 and cross2(c, a, q) >= -1e-9
    while len(order) > 3:
        for i in range(len(order)):
            a, b, c = order[i - 1], order[i], order[(i + 1) % len(order)]
            if cross2(a, b, c) <= 1e-9:
                continue
            if any(inside(q, a, b, c) for q in order if q not in (a, b, c)):
                continue
            m.tris.append((idx[a], idx[b], idx[c], part, flags))
            order.pop(i)
            break
        else:
            raise ValueError("ear clip failed on " + name)
    m.tris.append((idx[order[0]], idx[order[1]], idx[order[2]], part, flags))


# Screen outlines that the sheet stops at, in its plane: what stays terrain in
# front of it. (rows of layers.raw; the spike is the same tile at +192 cols)
FALL_EDGE = {
    # Spike 3,9 stands before this sheet: its rows 234-241 cols 222-229.
    "fall_1_6": [(229.5, 237.0), (228.5, 236.5), (227.5, 234.5), (226.5, 233.5), (223.5, 233.5), (222.0, 236.5)],
    # The walkway's corner (142,289) comes in front of the sheet's foot: floor edge
    # to (150,293) on the foot line.
    "fall_2_12": [(150.0, 293.0), (142.0, 289.0)],
}


def fall(m, name, cell, ytop, yfoot, axis):
    x0, x1, z0, z1 = cell[0] * 32.0, cell[1] * 32.0, cell[2] * 32.0, cell[3] * 32.0
    yt = ytop + FALL_LIFT
    # The cap reaches into what it touches at the back and the ends, so no gap.
    e = 3.0
    if axis == 'x':
        ze = z0 + 0.5   # water through col a, rock from a + 1 at the sheet's right end
        m.poly(name, [(x0 - e, yt, z1), (x1, yt, z1), (x1, yt, ze), (x0 - e, yt, ze)])
        pts = [(x1, yfoot, z1), (x1, yfoot, ze), (x1, yt, ze), (x1, yt, z1)]
        if name in FALL_EDGE:
            pts = [(x1, yfoot, ze), (x1, yt, ze), (x1, yt, z1)] + [on_x(a, b, x1) for a, b in reversed(FALL_EDGE[name])]
        flat(m, name, pts)
    else:
        m.poly(name, [(x0, yt, z1), (x1, yt, z1), (x1, yt, z0 - e), (x0, yt, z0 - e)])
        m.poly(name, [(x0, yfoot, z1), (x1, yfoot, z1), (x1, yt, z1), (x0, yt, z1)])


# ---- the ruin -------------------------------------------------------------
RUIN = BG1 | OVER
DAIS = 78.0   # the art's dais edge lies a row under the grid's Y 80 at the ruin
              # (back edge (344,247) (357,253); pier feet below)
PIER_FOOT = 79.0


def column(m, name, x, z, top, neck, base_top, flags=RUIN):
    """Radii from half widths: half col width / 0.7071. Rows of cell 6,6's column:
    capital's top rows 202-208 half 9, under it half 8, shaft half 7, base half 8."""
    m.cyl(name, (x, DAIS, z), (x, base_top, z), 11.5, sides=12, caps=False, flags=flags)
    m.cyl(name, (x, base_top, z), (x, base_top + 2.0, z), 11.5, sides=12, caps=False, flags=flags, r1=9.9)
    m.cyl(name, (x, base_top + 2.0, z), (x, neck, z), 9.9, sides=12, caps=False, flags=flags)
    m.cyl(name, (x, neck, z), (x, neck + 1.0, z), 9.9, sides=12, caps=False, flags=flags, r1=11.3)
    m.cyl(name, (x, neck + 1.0, z), (x, top - 3.0, z), 11.3, sides=12, caps=False, flags=flags)
    m.cyl(name, (x, top - 3.0, z), (x, top, z), 11.3, sides=12, caps=True, flags=flags, r1=13.0)


def ruin(m):
    # Centre col 317.5 / 381.5 / 285.5 (half widths about them); base-top rows 234 /
    # 266 / 250, foot centres 6.5 rows under them (front of the base rows 244.5 / 276.5 /
    # 260); capital tops widest at rows 207 / 239 / 227, necks at rows 214 / 246 / 234.
    column(m, "column_6_6", 204.5, 205.5, 146.0, 131.0, 91.0)
    column(m, "column_10_6", 332.5, 205.5, 146.0, 131.0, 91.0)
    column(m, "column_6_8", 203.5, 268.5, 137.0, 122.0, 90.0)
    # The lintel: top corners apex (331.5,195.25) right (378.5,218.75), Y 176, depth
    # 22 from the left end (331.5->321.5 cols); underside row 224 at col 345.5.
    # Its front slopes back to the piers' front plane, the lower edge moved 8.5 along
    # the line of sight (same pixels): as a box it overhung them and came apart
    # from them with the camera low, a gap of 7 rows.
    LX0, LX1, LZ0, LZ1, LY0, LY1, LBACK = 225.5, 319.5, 198.5, 220.5, 154.0, 176.0, 8.5
    low = lambda x: (x - LBACK, LY0 - LBACK, LZ1 - LBACK)
    flat(m, "arch_lintel", [(LX0, LY1, LZ0), (LX1, LY1, LZ0), (LX1, LY1, LZ1), (LX0, LY1, LZ1)], RUIN)
    flat(m, "arch_lintel", [(LX0, LY1, LZ1), (LX1, LY1, LZ1), low(LX1), low(LX0)], RUIN)
    flat(m, "arch_lintel", [(LX1, LY1, LZ1), (LX1, LY1, LZ0), (LX1, LY0, LZ0)], RUIN)
    flat(m, "arch_lintel", [(LX1, LY1, LZ1), (LX1, LY0, LZ0), low(LX1)], RUIN)
    # Left pier. Its right edge: col 342.5 rows 224-233, 343.5 below. Front right corner
    # of the base's foot (336,255), of the upper body col 338.5.
    # (One box over the foot: stepped, their 2-unit ledges cast black slits.)
    m.box("arch_pier_left", (223.0, 92.0, 203.0), (253.0, 154.0, 213.0), RUIN)
    m.box("arch_pier_left", (222.0, PIER_FOOT, 201.0), (252.0, 92.0, 216.0), RUIN)
    # Right pier. Its left edge: col 357.5 rows 231-236, 356.5 rows 237-240 (the
    # capital), 357.5 rows 241-250, 356.5 rows 251-258 (the moulding), 357.5 to the
    # foot corner (358,265.5). Right edge col 376.5 over the column, rows 231-235.
    m.box("arch_pier_right", (290.0, 88.0, 203.0), (321.5, 154.0, 213.0), RUIN)
    m.box("arch_pier_right", (296.0, PIER_FOOT, 200.0), (323.0, 88.0, 214.0), RUIN)
    # The fourth column, the actor's sprite (see the top): column_6_8 moved 128 along X.
    column(m, "column_10_8", 203.5 + 128.0, 268.5, 137.0, 122.0, 90.0, RUIN | ACTOR)


def build_parts(m, pool_patch=False):
    """The scene's parts. pool_patch: a flat piece of the pool at Y 32 behind the
    ruin, for a game that keeps the ruin's cells (6..10,6 and 6,8) tall: what is seen
    through the arch and round the columns then lies on the water, not on those
    blocks. Off by default: lower the cells instead (the terrain's water keeps its
    water look). Its outline follows the dais's edges (270,257) (318,233) (398,273)."""
    if pool_patch:
        P = lambda sx, sy: ((4.0 * (sy + POOL / 2.0 - KY) + 2.0 * (sx - KX)) / 2.0, POOL,
                            (4.0 * (sy + POOL / 2.0 - KY) - 2.0 * (sx - KX)) / 2.0)
        flat(m, "pool_behind_ruin", [P(*q) for q in ((270, 257), (318, 233), (398, 273), (402, 273), (402, 150), (270, 150))])
    # walls first
    m.poly("wall_left", [(WALL_X, FOOTW, FIELD_Z), (WALL_X, FOOTW, WALL_Z), (WALL_X, WALL_TOP_LEFT, WALL_Z), (WALL_X, WALL_TOP_LEFT, FIELD_Z)], LEE)
    m.poly("wall_right", [(WALL_X, FOOT, WALL_Z), (FIELD_X, FOOT, WALL_Z), (FIELD_X, WALL_TOP_RIGHT, WALL_Z), (WALL_X, WALL_TOP_RIGHT, WALL_Z)], LEE)
    for name, cell, ycap, yfoot, flush, sil, other in ROCKS:
        # (Square at the foot where the rounded tip lies against a fall's plane: it
        # made a one-column full-shade seam.)
        rock(m, name, cell, ycap, yfoot, flush, sil, other, base=CUT_BASE,
             rf=0.0 if name in ("rock_1_8", "rock_2_11") else 3.0)
    flat(m, "rock_8_1", [on_z(a, b, 64.0) for a, b in ROCK_8_1_LOW], LEE)
    for name, cell, ytop, yfoot, axis in FALLS:
        fall(m, name, cell, ytop, yfoot, axis)
    ruin(m)


if __name__ == "__main__":
    m = Model("map73")
    build_parts(m)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map73.inc")
    m.write_inc(out, 73, "tools/models/room73.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
