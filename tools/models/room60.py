#!/usr/bin/env python3
"""Map 60: map 59's bedroom at night. The same cells and the same art but what is listed
in room59.build_parts(night=True): the window shut (its shutters are paint on the wall's
plane, measured flush), no yarn, and a lantern on the table, the room's only light.

The lantern, on tools shots/map60/flat.png: it stands at the table's middle (X 77.5, Z 142.5:
x 95.5, the top's middle row 188), its grey body x 90-101, its top row 171 (Y 70), the flame
x 94-97, rows 180-185 (Y 46; kCeilingLamps' lamp there). It stops no light: the lamp is in it.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import room59  # noqa: E402
from model import Model, NOSHADOW  # noqa: E402

LANTERN = (77.5, 36.0, 142.5, 7.5, 70.0)    # X, floor (the table's top), Z, radius, top


def build():
    m = room59.build_parts(Model("room60"), night=True)
    x, y0, z, r, y1 = LANTERN
    m.cyl("lantern", (x, y0, z), (x, y1, z), r, sides=12, flags=NOSHADOW)
    return m


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map60.inc")
    m.write_inc(out, 60, "tools/models/room60.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
