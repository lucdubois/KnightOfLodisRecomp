#!/usr/bin/env python3
"""Map 82: map 81's room again (states/thor/0253, Alphonse at a smaller table with
someone under a white sheet, crossed swords on the left wall). Its height grid is
map 81's cell for cell (alcove walls at 17, the table x 4-5 z 5-7 at 2) and its
camera and scroll the same; the art differs only in its props (the table, the
swords, the suits of armour), none of them in the model. The user, 2026-11-13,
asked for its model and lighting to follow map 81: room81's model as it is.

    tools/models/room82.py      writes src/kol_model_map82.inc
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import room101  # noqa: E402
import room81  # noqa: E402


if __name__ == '__main__':
    m = room81.mirror(room101.build())
    out = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'kol_model_map82.inc')
    m.write_inc(out, 82, 'tools/models/room82.py')
    print('%d vertices, %d triangles, parts %s -> %s' % (len(m.verts), len(m.tris), m.parts, out))
