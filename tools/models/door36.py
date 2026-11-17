#!/usr/bin/env python3
"""Map 36: the arched door in the keep's wall, cell (5,4) (byte 0x10), on its +Z face
(Z 160) over the walkway (Y 160). The art paints it on the wall's plane; it is a doorway
set into the wall. Measured on Thor state 0275's focus dump (d.rgb),
sx = (X - Z)/2 + 272, sy = (X + Z)/4 - Y/2 + 123:

  - the opening is the cell's whole face, X 160-192 (columns 272-288 on the wall's plane);
  - its head is a half round: the apex at (278, 80), X 172 / Y 252, the right spring at
    (288, 92.5), Y 237: centre X 176, Y 236, radius 16;
  - the pale post up its left side, 3.5 columns wide, is the left jamb's inner face (+X,
    lit, as the art lights +X faces): 7-8 deep along Z.

So the model is what the wall's face hides: the door's plane DEPTH behind the face, the
left jamb's inner face, and the sill. The right
jamb and the rest of the arch face away. No kDrawnCells: the wall's face stays the
grid's, and a model point up to 24 behind the terrain takes the pixel (kTerrainNearer).

    tools/models/door36.py [depth]     writes src/kol_model_map36.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import LEE, Model  # noqa: E402
from room59 import face  # noqa: E402

# (u0, u1): the opening along the face (X on a +Z face, Z on a +X face); at first 160-192,
# the cell's edge: reported 1 px too wide on the right, the wall's last column stretched as the door.
#
# Maps 37 and 38 (one castle, by day and at sunset; Thor state 0281, a door the user asked to
# treat the same): cell (2,3) (byte 0x0F), on its +X face (X 96) over the walkway (Y 336).
# On the state's dump, sx = (X - Z)/2 + 256: the opening's dark starts at column 241 (Z 126)
# and ends at 256 (Z 96); the apex at (248, 112), Z 112 / Y 416. The jamb seen is the one at
# the low end again (it faces +Z).
#
# A second on the same two maps (Thor state 0282, "another door"): cell (5,9) (byte 0x10), on
# its +X face (X 192) over the lower walkway (Y 160). sx = (X - Z)/2 + 288 there: the dark
# from column 225 (Z 318) to 240 (Z 288), the apex at (232, 72.5), Z 304 / Y 253: map 36's
# door, 92 tall.
# Both then fitted to the art's dark pixels (the user found the opening too big on the right,
# top-right most, on both doors; I had taken the cell's edge for the low
# end, and read the apex by eye): of openings Z0 / Z1 / apex / rise tried a unit apart, the
# most pixels agreeing with (sum of RGB < 300) is the cell's edge + 3 to + 30, a rise of 14,
# the apex 414 and 254: the two doors are one drawing, 27 wide.
# depth 12, not 16: the grating's nearer bar is 6 columns in from the low edge, and at 16 (and 14) the jamb took it (the user saw
# it take part of the grating); the jamb is the 6 columns before it.
UPPER = dict(axis="x", face=96.0, u0=99.0, u1=126.0, floor=336.0, apex=414.0, depth=12.0)
LOWER = dict(axis="x", face=192.0, u0=291.0, u1=318.0, floor=160.0, apex=254.0, depth=12.0)
# A third (Thor state 0283): cell (1,17) (byte 0x18), on its +X face (X 64) over the walkway
# at Y 320; fitted the same way: Z 547-574, the apex 398 (the upper door's drawing, 78 tall).
WEST = dict(axis="x", face=64.0, u0=547.0, u1=574.0, floor=320.0, apex=398.0, depth=12.0)
# Map 9, the ruins (Thor state 0284, the door with a square head): cell (1,5), on its +X face (X 64)
# over the walkway at Y 192; a flat head (`square`). Fitted (RGB sum < 160 there): Z 160-189 (162 used: at 160 the mask took a column of wall on the right),
# the head at Y 257. A grating again, its nearer bar 7 columns in: 12 deep.
RUINS = dict(axis="x", face=64.0, u0=162.0, u1=193.0, floor=192.0, apex=257.0, depth=12.0, square=True)
# And its water gate (Thor state 0285, asked the same way): cell (5,6), +X face
# (X 192), standing in the pool (the water, Y 32, its floor). Fitted: Z 192-221, the head at
# Y 113; 194 for the low end, as RUINS.
# 223 for the high end (the upper rows' dark starts a column further left than the fit's
# 221). Its sill is the pool's water running in under the gate: the art's water (with the
# bars' reflection) stands 12 rows over the wall's foot line in the doorway, 24 deep, so the
# sill goes 24 back and the gate's plane ends 12 over the water (`sill_depth`). kol_focus
# counts it water (kModelRecessAo::water_y; the user asked for the water effect on the water
# in front of the door).
WATERGATE = dict(axis="x", face=192.0, u0=194.0, u1=225.0, floor=32.0, apex=113.0, depth=12.0, square=True,
                 sill_depth=24.0)
# Both squared doors' high end then moved out to the cell's edge + 1 (193, 225): the fit's
# 189 / 221 left the grating's first bar, two columns further, on the wall's face, and it
# stood before the rest with the camera moved ("The grating has one part that floats in
# front"). The bars are 4 columns apart from the opening's very edge.
# Map 36's small bridge (cells 10, 10-15, height 8) has two culverts through it at the
# water (cells 10,13 and 10,14, byte 0x0F; Thor state 0289, the user asked for its two holes
# to be true holes, properly lit, with the water effect): on its +X face (X 352),
# over the channel's water (Y 96). On the state's dump (sx = (X - Z)/2 + 240): their dark is
# columns 197-204 rows 151-157 and 180-188 rows 159-165: each 16 wide at its cell's middle
# (Z 432, 464), a half round to Y 112. 16 deep, the water running in to their back
# (the sill at the water's height: kModelRecessAo::water_y makes it water).
CULVERT_A = dict(axis="x", face=352.0, u0=424.0, u1=440.0, floor=96.0, apex=112.0, depth=16.0)
CULVERT_B = dict(axis="x", face=352.0, u0=456.0, u1=472.0, floor=96.0, apex=112.0, depth=16.0)
DOORS = {
    36: [dict(axis="z", face=160.0, u0=160.0, u1=190.0, floor=160.0, apex=252.0), CULVERT_A, CULVERT_B],
    37: [UPPER, LOWER, WEST],
    38: [UPPER, LOWER, WEST],
    9: [RUINS, WATERGATE],
}
DEPTH = 16.0
STEPS = 8


def clip(poly, axis, lo):
    """A convex polygon (2D points) cut to the side where its `axis` coordinate >= lo."""
    out = []
    for a, b in zip(poly, poly[1:] + poly[:1]):
        ia, ib = a[axis] >= lo, b[axis] >= lo
        if ia:
            out.append(a)
        if ia != ib:
            t = (lo - a[axis]) / (b[axis] - a[axis])
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    return out


def build(map_no=36, depth=DEPTH, name=None):
    """Each part only where it is seen through the opening: a pixel's ray enters the
    wall's face at (u, Y) and is at (u - s, Y - s) when s behind it, and a model point up
    to 24 behind the terrain takes the pixel whether the wall has an opening there or
    not (the first build's door plane, the whole cell wide, took 4 columns of the wall
    to its right)."""
    m = Model(name or "door%d" % map_no)
    for d in DOORS[map_no]:
        one_door(m, d, d.get("depth", depth))
    return m


def one_door(m, d, depth):
    U0, U1, FLOOR = d["u0"], d["u1"], d["floor"]
    R = (U1 - U0) / 2
    CU, SPRING = (U0 + U1) / 2, d["apex"] - R

    def P(u, y, back):   # a point `back` behind the face
        return (u, y, d["face"] - back) if d["axis"] == "z" else (d["face"] - back, y, u)

    def N(nu, ny, nf):   # a direction: along the face, up, out of the face
        return (nu, ny, nf) if d["axis"] == "z" else (nf, ny, nu)

    arc = [(CU + R * math.cos(math.pi * i / (2 * STEPS)), SPRING + R * math.sin(math.pi * i / (2 * STEPS)))
           for i in range(2 * STEPS + 1)]            # high spring, over the apex, to the low spring
    if d.get("square"):                             # a flat head at `apex`
        arc = [(U1, d["apex"]), (U0, d["apex"])]
    opening = [(U0, FLOOR), (U1, FLOOR)] + arc
    # The door's plane: the opening moved (-depth, -depth), cut by the low jamb and the sill.
    sill_depth = d.get("sill_depth", depth)
    door = clip(clip([(u - depth, y - depth) for (u, y) in opening], 0, U0), 1, FLOOR + (sill_depth - depth))
    if len(door) >= 3:   # (a hole as deep as it is wide shows none of its back: the culverts)
        face(m, "door", [P(u, y, depth) for (u, y) in door], N(0, 0, 1), LEE)
    # The low jamb's inner face, up to where the arch's line cuts it: s behind the face it is
    # seen through the opening at (U0 + s, y + s), so up to the arch's height there less s.
    # (A rectangle to the spring and the arch's underside over it, at first: their back edge
    # showed 8 rows over the arch's outline, on the wall.)
    def jamb_top(sb):
        if d.get("square"):
            return d["apex"] - sb
        return SPRING + math.sqrt(max(0.0, R * R - (U0 + sb - CU) ** 2)) - sb
    cuts = [depth * i / STEPS for i in range(STEPS + 1)]
    for sa, sb in zip(cuts, cuts[1:]):
        ta, tb = max(FLOOR, jamb_top(sa)), max(FLOOR, jamb_top(sb))
        face(m, "jamb", [P(U0, FLOOR, sa), P(U0, FLOOR, sb), P(U0, tb, sb), P(U0, ta, sa)], N(1, 0, 0), LEE)
    if sill_depth > depth:
        # Under a gate that ends over its sill (the water gate): the jamb runs on to the
        # sill's back, as much of it as is seen under the gate's foot. Without it the rays
        # that pass under the gate and over the sill's far edge met nothing: two pixels of
        # the wall's face (a bar's bright reflection) were left in the doorway, before the
        # gate (the user saw it as a bright dot in the door).
        face(m, "jamb", [P(U0, FLOOR, depth), P(U0, FLOOR, sill_depth), P(U0, FLOOR + (sill_depth - depth), depth)],
             N(1, 0, 0), LEE)
    if d.get("sill", True):
        face(m, "sill", [P(U0, FLOOR, sill_depth), P(U0, FLOOR, 0), P(U1, FLOOR, 0), P(U1 - sill_depth, FLOOR, sill_depth)],
             N(0, 1, 0), LEE)


if __name__ == "__main__":
    depth = float(sys.argv[1]) if len(sys.argv) > 1 else DEPTH
    for no in sorted(DOORS):
        m = build(no, depth)
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map%d.inc" % no)
        m.write_inc(out, no, "tools/models/door36.py")
        print("map %d: %d vertices, %d triangles -> %s" % (no, len(m.verts), len(m.tris), os.path.normpath(out)))
