#!/usr/bin/env python3
"""Map 67, a dungeon cell (states/thor/0044; README "Map 67: the door's
opening"). The user's drawn heights make the left wall (cells x 2) and the
back wall (row 1) stand at 18 (Y 288); the doorway in the left wall is cut
through them: its two cells, (2, 3) and (2, 4), are the threshold and the
corridor at 6 (Y 96, kDrawnCells), and this model is the wall over and
beside the opening, and the wall right of the door (cell (2, 2)) half a
tile thick. From the user's painting (shots/room67/door_drawn.png:
red the wall's face, yellow the corridor floor seen through the door, cyan
the reveal), solved on the flat scene's projection: the wall's inner face
X 96 (its floor line on the room's Y 80 at both ends), the opening Z 97-154
on it, its threshold Y 96, its top Y 182; the reveal 14 deep in the art.

    tools/models/room67.py                      writes src/kol_model_map67.inc and _map68.inc
    tools/models/room67.py fit PNG INFO OUT [x0 y0 x1 y1]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import Model, overlay  # noqa: E402

TOP = 288.0                 # the wall's top (the user's 18)
WALL_X0, WALL_X1 = 64.0, 96.0
DOOR_Z0, DOOR_Z1 = 96.0, 154.0
SILL, LINTEL = 96.0, 182.0
# To the wall's front end (cell (2, 5)'s far side, Z 192), not the door's
# cells' (160): a box ending at 160 had its +Z face inside the wall, and the
# model, taking pixels before the grid, put the wall's art left of the door
# on that face (the user saw the wall's art on the door's hidden side
# face).
CELL_Z1 = 192.0
# The wall right of the door (cell (2, 2), which the user's heights put at
# 6): half a tile thick against the room (the user asked for the wall right
# of the door raised, half a tile thick), on the step's floor
# (Y 80, cells (3, 2) at 5) to the wall's top; its end at the opening is
# the reveal (the art's is 14 deep).
HALF_X0 = 80.0
RIGHT_Z0, RIGHT_FLOOR = 64.0, 80.0


# Map 68's opening (the same cell flooded, its door open): a game pixel
# (2 units of Z on this wall) narrower on the right, where the art draws the
# open door's grill as part of the wall, and one wider on the left (the
# user asked for the entrance a game pixel narrower on the right, where the
# art paints the door grill, and a game pixel longer on the left; then for
# a little more each way: two pixels each).
DOOR_68 = (DOOR_Z0 + 4.0, DOOR_Z1 + 4.0)
# ... and a game pixel taller (2 units of Y; the user found the doorway
# a game pixel too short).
LINTEL_68 = LINTEL + 2.0


def build(door=(DOOR_Z0, DOOR_Z1), lintel=LINTEL):
    z0, z1 = door
    m = Model("room67")
    # Over the opening, both its cells' width (the grid's wall beside is a
    # cell thick too).
    m.box("lintel", (WALL_X0, lintel, min(z0, DOOR_Z0)), (WALL_X1, TOP, CELL_Z1))
    # The near jamb: beside the opening to the wall's front end.
    m.box("jamb", (WALL_X0, SILL, z1), (WALL_X1, lintel, CELL_Z1))
    m.box("right", (HALF_X0, RIGHT_FLOOR, RIGHT_Z0), (WALL_X1, TOP, z0 + 1.0))
    return m


def main():
    m = build(DOOR_68, LINTEL_68) if os.environ.get("ROOM67_MAP") == "68" else build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=4, alpha=0.25)
        return
    # Map 68 is the same cell flooded (states/thor/0045; the user noted state
    # 45 has the same shape and asked for the same room model): the
    # model fits its art as it is.
    for map_no, door, lintel in ((67, (DOOR_Z0, DOOR_Z1), LINTEL), (68, DOOR_68, LINTEL_68)):
        m = build(door, lintel)
        out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map%d.inc" % map_no)
        m.write_inc(out, map_no, "tools/models/room67.py")
        print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
