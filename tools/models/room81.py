#!/usr/bin/env python3
"""Map 81: map 76's room (101's without its table) drawn mirrored (the user,
2026-11-10, states/thor/0171, saw state 171's map as state 164's
flipped). Its height grid is map 76's with X and Z swapped (1018 of 1024
cells; the 6 others its own, larger table, x 4-5 z 5-7, height 2), and its
flat frame matches 76's mirrored about the X = Z line on 88% of the map's
pixels (the rest its table and chairs, a suit of armour for the eagle, the
floor's middle; none of them in the model). So the model is room101's with X
and Z swapped: the normals too, and each triangle wound the other way round
to keep its faces' facing.

    tools/models/room81.py      writes src/kol_model_map81.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import room101  # noqa: E402


def mirror(m):
    m.verts = [(z, y, x, nz, ny, nx) for (x, y, z, nx, ny, nz) in m.verts]
    m.tris = [(a, c, b, part, flags) for (a, b, c, part, flags) in m.tris]
    return m


if __name__ == '__main__':
    m = mirror(room101.build())
    out = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'kol_model_map81.inc')
    m.write_inc(out, 81, 'tools/models/room81.py')
    print('%d vertices, %d triangles, parts %s -> %s' % (len(m.verts), len(m.tris), m.parts, out))
