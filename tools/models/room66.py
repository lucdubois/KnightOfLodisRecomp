#!/usr/bin/env python3
"""Map 66: map 65's room (tools/models/room65.py) with its door shut, a long table
in place of the wreck, and all three sconces burning. Same cells, the same walls,
windows and sconces on the same places; the table on the same three cells, its
top's corners solving to X 128-224, Z 64-96 at Y 35.5 (the art: BG1 over the
carpet, so it lifts).

The door: the doorway's opening is 65's, the door a plane 14 in (X 50), by the
7-column dark band of the far jamb beside it on the art.

The flames, BG1 and animated (their shape changes from frame to frame): cards on
the sconces' middle plane, so they stay on their cups with the camera moved. The
two on the back wall are alone on BG1, so a box takes every frame's whole flame and
nothing else; the left one's BG1 is the whole wall's, so it is cut to their union
over five frames.

    tools/models/room66.py      writes src/kol_model_map66.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import ASDRAWN, BG1, FXART, Model, NOGAP, NOSHADOW, OVER, SHEET  # noqa: E402
import room65  # noqa: E402

# The flat frame the flames were read on (viewer, 480x270): sx = (X - Z) / 2 + KX, ...
KX, KY = 208.0, 95.0
DOOR_X = 50.0
OUT = 7.6       # the flames' plane: the sconces' middle, this far off the wall
EDGE = 0.1      # (the raster closes a triangle's edges: a quad holds only its own pixels)
# OVER: the wall's BG0 pixels in a flame's tiles stay the wall's (without it they fell to
# the floor's place: a dark square behind each flame).
# FXART: with Special Effects on the art's flame goes: dimmed by the dark, it stood out
# round the effect's and slid against it with the camera, a flame's shadow on the wall.
FLAME = BG1 | SHEET | NOSHADOW | ASDRAWN | NOGAP | OVER | FXART
# Back wall: (first column, last column, first row, last row).
BACK_FLAMES = ((276, 291, 64, 87), (323, 339, 88, 111))
# Left wall: row -> (first, last column), the union of five frames.
LEFT_FLAME = {76: (173, 180), 77: (173, 180), 78: (173, 180), 79: (173, 180), 80: (172, 181), 81: (172, 181),
              82: (170, 181), 83: (170, 181), 84: (169, 184), 85: (169, 184), 86: (169, 184), 87: (169, 184),
              88: (169, 184), 89: (169, 184), 90: (169, 184), 91: (169, 184), 92: (169, 184), 93: (169, 184),
              94: (169, 184), 95: (169, 184), 96: (169, 184), 97: (169, 183), 98: (171, 182), 99: (172, 180)}


def on_z(c, r, Z):      # the point of the plane Z seen at (c, r)
    X = Z + 2.0 * (c - KX)
    return (X, 2.0 * ((X + Z) / 4.0 + KY - r), Z)


def on_x(c, r, X):      # the point of the plane X seen at (c, r)
    Z = X - 2.0 * (c - KX)
    return (X, 2.0 * ((X + Z) / 4.0 + KY - r), Z)


def quad(m, plane, a, b, r0, r1, at):
    m.poly("flame", [plane(a - EDGE, r0 - EDGE, at), plane(b + 1 - EDGE, r0 - EDGE, at),
                     plane(b + 1 - EDGE, r1 + 1 - EDGE, at), plane(a - EDGE, r1 + 1 - EDGE, at)], FLAME)


def build_parts(m):
    m.box("table", *room65.TABLE, BG1)
    room65.sconce(m, 0.0, 0.0)
    room65.sconce(m, 96.0, 0.0)
    room65.sconce(m, -22.5, room65.LX, back_wall=False, drop=5.0)
    room65.back_wall(m)
    LX, T = room65.LX, room65.T
    F, TOP = room65.FLOOR, room65.TOP
    Z0, Z1, DT = room65.DOOR_Z0, room65.DOOR_Z1, room65.DOOR_TOP
    m.box("left", (LX - T, F, 0.0), (LX, TOP, Z0))
    m.box("left", (LX - T, DT, Z0), (LX, TOP, Z1))
    m.box("left", (LX - T, F, Z1), (LX, TOP, room65.Z1))
    # The door, shut: its face in the opening, the far jamb and the lintel's underside
    # up to it, the threshold's floor before it.
    m.poly("door", [(DOOR_X, F, Z1), (DOOR_X, F, Z0), (DOOR_X, DT, Z0), (DOOR_X, DT, Z1)], SHEET)
    m.poly("door", [(DOOR_X, F, Z0), (LX, F, Z0), (LX, DT, Z0), (DOOR_X, DT, Z0)], SHEET)
    m.poly("door", [(DOOR_X, F, Z0), (DOOR_X, F, Z1), (LX, F, Z1), (LX, F, Z0)], SHEET)
    for (a, b, r0, r1) in BACK_FLAMES:
        quad(m, on_z, a, b, r0, r1, OUT)
    for r, (a, b) in LEFT_FLAME.items():
        quad(m, on_x, a, b, r, r, LX + OUT)


if __name__ == "__main__":
    m = Model("map66")
    build_parts(m)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map66.inc")
    m.write_inc(out, 66, "tools/models/room66.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
