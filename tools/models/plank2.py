#!/usr/bin/env python3
"""Map 2's plank, a slab as the boat scene's gangplanks (README "Map 2's
plank, a slab").

    tools/models/plank2.py                  writes src/kol_model_map2.inc
    tools/models/plank2.py fit FLAT INFO OUT [x0 y0 x1 y1]

The rainy dock's board (states/thor/0007, cell 4,11): the grid stores the
cell at the ledge's height 5 (Y 80), the step round it at 2, so as terrain
the board was a block, and drawn at 2 (the Thor's drawn heights) it lay on
the step and the cliff face and bent with them under the perspective. Its
far edge runs along Z on the flat frame, so it is level: a board at the
ledge's height, its shore end on the ledge (row 9-10), jutting out over the
step. Fitted on the flat frame (state 7 at 384x216, sprites hidden: KX, KY
= kx + ox, ky + oy) by its corners.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ship97  # noqa: E402
from model import Model, NOGAP, overlay  # noqa: E402

Y = 80.0            # the ledge's height (5 steps)
X_FAR, X_NEAR = 131.0, 155.0
Z_SHORE, Z_END = 316.0, 386.0
THICK = 4.0


def build():
    m = Model("plank")
    q0, q1 = (X_FAR, Y, Z_SHORE), (X_NEAR, Y, Z_SHORE)
    s1, s0 = (X_NEAR, Y, Z_END), (X_FAR, Y, Z_END)
    ship97.plank(m, q0, q1, s1, s0, thick=THICK, flags=NOGAP)
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=8,
                guides_only=os.environ.get("GUIDES") == "1")
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map2.inc")
    m.write_inc(out, 2, "tools/models/plank2.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
