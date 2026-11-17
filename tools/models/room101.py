#!/usr/bin/env python3
"""Map 101's arcade (the intro's castle room, states/HS; README "The castle
room's arcade"): pillars and arches along both walls in front of alcoves
whose floor runs back under the arches, where the height grid has two cells
of solid wall (height 17). Fitted to the user's painting of the walls
(shots/castle_room/HS_drawn_480.png, its runs in room101_runs.py): each painted
face a plane at the depth where its foot meets the floor (Y 16), holding
the pixels painted (a quad a column of the view, the run's rows), so the
arch openings are the painting's. Behind them the alcoves' walls (X 32, Z
32) whole, and the floors at Y 16 to them.

    tools/models/room101.py          writes src/kol_model_map101.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model  # noqa: E402
import room101_runs  # noqa: E402

# The painting's frame: the flat (perspective off) 384x216 present, its
# picture 28 rows down the 272-row window, cropped from row 36:
# sx = (X - Z) / 2 + KX, sy = (X + Z) / 4 - Y / 2 + KY in the window's rows.
KX, KY = 168.0, 88.0    # kx + ox (96 + 72), ky + oy + 28 (32 + 28 + 28)
TOP = 272.0             # the walls' top (height 17)
ALCOVE = 32.0           # the alcoves' walls: X 32 (the window wall's), Z 32 (the back wall's); the painting's red
FLOOR = 16.0
# (colour, first column) -> (part, plane, depth): plane 'x' a +X face at X =
# depth, 'z' a +Z face at Z = depth; the depths where each face's foot meets
# the painted floor. None: left out (the alcove walls are whole planes).
# The painting: shots/castle_room/HS_drawn_480.png (the first one, 384 wide,
# and the band over it the 480 view shows, painted 2026-10-18), whose
# orange window-wall arcade joins the corner pillar's +X face (orange too)
# over the top: columns from CORNER_X on are the corner's.
FACE = {
    ('O', 52): ('arcade', 'x', 56.0),     # the window wall's arcade and pillar fronts
    ('M', 168): ('arcade', 'z', 56.5),    # the back wall's
    ('M', 102): ('pillar', 'z', 162.5),   # the side of the pillar between the windows
    ('P', 219): ('pillar', 'x', 162.5),   # the back wall's pillars' sides
    ('Q', 268): ('pillar', 'x', 259.5),
    ('M', 148): ('corner', 'z', 64.5),    # the corner pillar, out further
    ('R', 77): None, ('R', 124): None,    # the window alcoves: the wall at X 32, whole
}
CORNER_X = 168
CORNER = ('corner', 'x', 65.5)


def world(plane, depth, sx, sy):
    """The point of the face's plane seen at frame pixel (sx, sy)."""
    if plane == 'x':
        X = depth
        Z = X - 2.0 * (sx - KX)
    else:
        Z = depth
        X = Z + 2.0 * (sx - KX)
    Y = 2.0 * ((X + Z) / 4.0 + KY - sy)
    return (X, Y, Z)


def top_row(plane, depth, sx):   # the row of the wall's top at this column
    X, _, Z = world(plane, depth, sx, 0.0)
    return (X + Z) / 4.0 - TOP / 2.0 + KY


def build():
    m = Model('arcade')
    for ch, x0, runs in room101_runs.FACES:
        spec = FACE[(ch, x0)]
        if spec is None:
            continue
        for (x, y0, y1) in runs:
            part, plane, depth = CORNER if (ch, x0) == ('O', 52) and x >= CORNER_X else spec
            want = (1, 0, 0) if plane == 'x' else (0, 0, 1)
            corners = []
            for sx in (x, x + 1.0):
                # Never past the walls' top (the painting runs over the wall
                # tops into the sky); within 2 rows of it, to it.
                top = max(36.0 + y0, top_row(plane, depth, sx))
                if top < top_row(plane, depth, sx) + 2.0:
                    top = top_row(plane, depth, sx)
                bot = 36.0 + y1 + 1.0
                pt, pb = world(plane, depth, sx, top), world(plane, depth, sx, bot)
                if abs(pb[1] - FLOOR) < 3.0:   # on the floor
                    pb = world(plane, depth, sx, (pb[0] + pb[2]) / 4.0 - FLOOR / 2.0 + KY)
                corners.append((pt, pb))
            (a_t, a_b), (b_t, b_b) = corners
            quad = [a_b, b_b, b_t, a_t]
            # Wound to face `want` (outward).
            u = [quad[1][i] - quad[0][i] for i in range(3)]
            v = [quad[3][i] - quad[0][i] for i in range(3)]
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if n[0] * want[0] + n[2] * want[2] < 0:
                quad.reverse()
            m.poly(part, quad)
    # The alcoves' walls, whole (behind the arcade), and the floors to them.
    B = ALCOVE
    m.poly('alcove', [(B, FLOOR, B), (B, FLOOR, 352.0), (B, TOP, 352.0), (B, TOP, B)][::-1])
    m.poly('alcove', [(B, FLOOR, B), (B, TOP, B), (416.0, TOP, B), (416.0, FLOOR, B)][::-1])
    m.poly('floor', [(B, FLOOR, B), (B, FLOOR, 352.0), (64.0, FLOOR, 352.0), (64.0, FLOOR, B)])
    m.poly('floor', [(64.0, FLOOR, B), (64.0, FLOOR, 64.0), (416.0, FLOOR, 64.0), (416.0, FLOOR, B)])
    return m


if __name__ == '__main__':
    m = build()
    for t in m.tris[:0]:
        pass
    out = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'kol_model_map101.inc')
    m.write_inc(out, 101, 'tools/models/room101.py')
    print('%d vertices, %d triangles, parts %s -> %s' % (len(m.verts), len(m.tris), m.parts, out))
