#!/usr/bin/env python3
"""Map 76: map 101's castle room without its table (the user, 2026-11-10,
saw map 76 as map 101 without the table and asked for the same model). The two maps' flat frames (the scene
viewer, the same camera) agree on every pixel's layer but the table's (1599
pixels, all in it); their palettes differ. Map 101's table is no part of its
model (the grid's cells, raised by its drawn heights), so the model is 101's
as it is.

    tools/models/room76.py      writes src/kol_model_map76.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import room101  # noqa: E402

if __name__ == '__main__':
    m = room101.build()
    out = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'kol_model_map76.inc')
    m.write_inc(out, 76, 'tools/models/room76.py')
    print('%d vertices, %d triangles, parts %s -> %s' % (len(m.verts), len(m.tris), m.parts, out))
