#!/usr/bin/env python3
"""Map 21: the land bridge at the back of the lagoon. Its front row, cells x 5-8, z 2
(records 0x10-0x13, height 8), is an arch over water at height 2; the grid has no
opening (one height a cell), so the water painted under the arch stood up the bridge's
front wall. Measured on the dump's scene (states/thor/0218 at rest, a.rgb),
sx = (X - Z)/2 + 205, sy = (X + Z)/4 - Y/2 + 127:

  - the face (Z 96) is all shade from the deck's top edge, (237,127)-(301,159), down;
  - the water under it is bounded on the right by a curve on the face: over the face's
    foot line it stands Y 77-79 at X 234-250, 69 at 258, 57 at 270, 47 at 274 and meets
    the water (33) at X 278: half an ellipse, centre X 220, half width 60, rise 48
    (58 at 270, 69 at 258, 79 at 234);
  - on the left the water ends along a line of slope -1/2 from (237,175): the left
    pier's inner foot (X 160) going back, the pier's face seen through the opening;
  - and along a line of slope +1/2 through (254,168) and (264,173): X + Z = 4*sy - 444,
    Z 65: the back of the hollow, the next row of cells (z 1, height 8), one cell in.

So: the front row's cells are drawn at the water's height (kDrawnCells), and this model
puts the bridge back over them: its top, its front face round the arch, and the vault
from the face to the cells behind. The piers and the back wall are the grid's.

    tools/models/arch21.py      writes src/kol_model_map21.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import LEE, Model  # noqa: E402
from room59 import face  # noqa: E402

X0, X1 = 160.0, 288.0      # cells x 5-8
Z0, Z1 = 64.0, 96.0        # cell z 2
WATER, TOP = 32.0, 128.0
CX, HALF, RISE = 220.0, 62.0, 50.0   # 2 over the fit: the water's last pixels along the curve stay water (bright slivers on the rim)
STEPS = 16
# The user, 2026-11-11, asked for it one tile deeper to the right, through x6z1 and
# x7z1: the hollow goes on under cells 6,1 and 7,1 (the same curve between their walls).
BX0, BX1, BZ0 = 192.0, 256.0, 32.0


def build(name="arch21", water=WATER):
    m = Model(name)
    # The curve's points, the back hollow's two walls (X 192 and 256) among them.
    xs = sorted(set([CX - HALF * math.cos(math.pi * i / STEPS) for i in range(STEPS + 1)] + [BX0, BX1]))
    arc = [(x, max(water, WATER + RISE * math.sqrt(max(0.0, 1.0 - ((x - CX) / HALF) ** 2)))) for x in xs]
    face(m, "deck", [(X0, TOP, Z0), (X0, TOP, Z1), (X1, TOP, Z1), (X1, TOP, Z0)], (0, 1, 0))
    face(m, "deck", [(BX0, TOP, BZ0), (BX0, TOP, Z0), (BX1, TOP, Z0), (BX1, TOP, BZ0)], (0, 1, 0))
    for (xa, ya), (xb, yb) in zip(arc, arc[1:]):
        face(m, "front", [(xa, ya, Z1), (xb, yb, Z1), (xb, TOP, Z1), (xa, TOP, Z1)], (0, 0, 1), LEE)
        if ya <= water and yb <= water:   # under the higher water: no vault there
            continue
        back = BZ0 if xa >= BX0 - 0.01 and xb <= BX1 + 0.01 else Z0
        face(m, "vault", [(xa, ya, Z1), (xb, yb, Z1), (xb, yb, back), (xa, ya, back)],
             (CX - (xa + xb) / 2, WATER - (ya + yb) / 2 - 0.01, 0), LEE)
    face(m, "front", [(CX + HALF, water, Z1), (X1, water, Z1), (X1, TOP, Z1), (CX + HALF, TOP, Z1)], (0, 0, 1), LEE)
    return m


# The same lagoon in other weathers and tides (the user, 2026-11-11, named maps 22, 23, 24, 25
# and 114, some with a higher water line): the same cells and bridge;
# on 22, 25 and 114 the water stands at height 3 (Y 48; the bridge's records 0x14-0x17),
# the same curve cut by the higher water.
MAPS = {21: 32.0, 23: 32.0, 24: 32.0, 22: 48.0, 25: 48.0, 114: 48.0}

if __name__ == "__main__":
    for no, water in sorted(MAPS.items()):
        m = build("arch%d" % no, water)
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map%d.inc" % no)
        m.write_inc(out, no, "tools/models/arch21.py")
        print("map %d: %d vertices, %d triangles -> %s" % (no, len(m.verts), len(m.tris), os.path.normpath(out)))
