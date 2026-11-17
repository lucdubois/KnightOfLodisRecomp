#!/usr/bin/env python3
"""Map 58's ship, the same drawing as map 97's (README "The ship on map 58").

    tools/models/ship58.py                  writes src/kol_model_map58.inc
    tools/models/ship58.py fit FLAT INFO OUT [x0 y0 x1 y1]

Map 58's ship art is map 97's tile for tile (night palette), 32 px left and
16 down on the screen; its deck cells are stored at 3 where map 97's are at
1. So the model is ship97.py's, built in map 97's world and moved by OFFSET:
X - 416, Y + 32, Z - 256 (13 cells, 2 steps, 8 cells). What map 97 has
round the ship and map 58 does not (the quay, its tie and barrels, the
gangplank to it) is left out; map 58's own gangplank and rope are fitted
here on its flat frame (the viewer at cell 0,2, 384x216: KX, KY).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ship97  # noqa: E402
import ship_fit  # noqa: E402
from model import Model, NOGAP, BG1ART, overlay  # noqa: E402

OFFSET = (-416.0, 32.0, -256.0)
KX, KY = 224.0, 124.0   # map 58's viewer frame at cell 0,2 (kx + ox, ky + oy)


def moved(m, d):
    out = Model(m.name)
    out.parts = list(m.parts)
    out.verts = [(x + d[0], y + d[1], z + d[2], nx, ny, nz) for (x, y, z, nx, ny, nz) in m.verts]
    out.tris = list(m.tris)
    mv = lambda p: (p[0] + d[0], p[1] + d[1], p[2] + d[2])
    out.guides = [(mv(a), mv(b), c) for a, b, c in m.guides]
    return out


# The gangplank from the far rail to the rock (map 58's own: narrower and
# level with the rock; the grid stores it as cells 4,5 and 4,6 at 80, the
# rock's height). Its top's far edge on the flat frame runs from (184, 168.5)
# at the rail to (227, 147) on the rock, 11 rows deep over a 3-row lip: 22
# wide. The rock end on the rock's 80; the ship end on the far rail (Z 227,
# 219 since the far side stands ship97.FAR_OUT further out, 2026-10-26),
# which puts it at 90, the rail's top there (as map 97's lies on its rail).
# That is where its far edge crosses the rail; the art carries the plank on
# over the rail, its end and the end's thickness 3 columns further (the
# user, 2026-10-26, found it a bit short): PLANK_IN more along its own slope.
# The rock's lip is drawn over the plank's end (columns 228-231 over rows
# 146-153): the end is cut where the plank stops showing, (227, 147) to
# (232, 155.5), askew.
PLANK_W = 22.0
PLANK_IN = 8.0


def plank_corners():
    def at_y(col, row, y):
        d, s = 2.0 * (col - KX), 4.0 * (row - KY) + 2.0 * y
        return ((s + d) / 2.0, y, (s - d) / 2.0)
    def at_z(col, row, z):
        x = z + 2.0 * (col - KX)
        return (x, 2.0 * ((x + z) / 4.0 + KY - row), z)
    q0 = at_y(227.0, 147.0, 80.0)
    q1 = at_y(232.0, 155.5, 80.0)
    s0 = at_z(184.0, 168.5, 227.0 - ship97.FAR_OUT)
    s1 = (s0[0] + PLANK_W, s0[1], s0[2])
    on = lambda q, s: tuple(b + (b - a) * PLANK_IN / (s[2] - q[2]) for a, b in zip(q, s))
    return q0, q1, on(q1, s1), on(q0, s0)


def build():
    m = moved(ship_fit.build(quay=False), OFFSET)
    # Map 58 draws the whole hull on BG1 (map 97 on BG0), and the water round
    # it on BG0: the hull takes only BG1 pixels. Its bow, fitted on map 97
    # where the water's cells stand nearer, bulged over the water here as a
    # wedge under the bowsprit (the user, 2026-10-24, states/JL).
    # The rail too (2026-10-26): its art is BG1 alone, and the cliff's dark
    # BG0 face beside the far rail's corner joined it as outline.
    bg1 = (m.part("hull"), m.part("rail"))
    m.tris = [t[:4] + (t[4] | BG1ART,) if t[3] in bg1 else t for t in m.tris]
    # The rock round it is BG1 too, in whole tiles: gaps it bounds are the
    # rock's (left to the gap fill, the plank took 203 of its pixels).
    ship97.plank(m, *plank_corners(), flags=NOGAP)
    return m


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop,
                guides_only=os.environ.get("GUIDES") == "1")
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map58.inc")
    m.write_inc(out, 58, "tools/models/ship58.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
