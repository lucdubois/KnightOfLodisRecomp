#!/usr/bin/env python3
"""Map 61: map 59's bedroom again, later in the story (states/thor/0217). The same cells,
the same camera and the same art by day, window open; someone else lies in the bed. The
model is room59's but the bowl of yarn in front of the bookcase, which is not here (its
part for the one in the bed too: kSceneObjects names who)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import room59  # noqa: E402
from model import Model  # noqa: E402

if __name__ == "__main__":
    m = room59.build_parts(Model("room61"), yarn=False)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map61.inc")
    m.write_inc(out, 61, "tools/models/room61.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
