#!/usr/bin/env python3
"""Map 93's art, per object and column, from a flat focus dump (KOL_FOCUS_DUMP
with KOL_EDGE_BLUR=1, the perspective off, unlit): writes room93_art.py.

    tools/models/room93_extract.py <dump prefix>    (prefix.rgb / .layers / .info)
    tools/models/room93_extract.py <dump prefix> --labels out.png   (a check picture)

The dump used: states/ending_map93.state, 700 frames in (the ending's last
picture, camera at rest), 384 x 216, KX 328, KY 44; the art is the game's
240 x 160 at x 72-311, y 28-187, backdrop round it.

How the objects are found. Every object the art stands up (the fences, the
signpost, the trees, the berry bushes) is drawn with one outline colour,
OUTLINE (41, 57, 74), which the ground never uses: the outlines close round
each object but at its feet, where the art lets it run into the grass. So a
flood fill of the ground (4-connected) from a few seeds, walled by the
outline AND by every gap of at most GAP_H pixels between two outline pixels
in a row (a post's or a trunk's inside) or GAP_V in a column (a rail's), stops
at each object: what it does not reach is an object (the outline included).
The grass between two rails and two posts is enclosed too: a fence keeps only
its wood there (FENCE_KEEP), a bush or a tree all of it.
Then each object pixel goes to the first object in OBJECTS whose rectangles
hold it (the art's posts, rails and plants read at 7-12x).
The rock has no such outline: its grey palette (ROCK_COLOURS) in its box,
on BG0 and BG1 (its top is BG1). The bush at the bottom is BG1's only object:
BG1 pixels in its box. Sprites (the two characters) are unknown: the fill
passes through them; the signpost's post runs on under them in room93.py.
The far hills in the top left (BG0, pale: g > r, b >= 100, bright) are not
ground: GROUND_TOP per column is the first row under them (the ridge's BG1
outline included), GROUND_BOTTOM the art's last row.
"""
import os
import sys

OUTLINE = (41, 57, 74)
GAP_H = 6
GAP_V = 5
# the last two: grass pockets the walls close off (between R1, its rail and the
# frame's edge; the meadow between posts H and G under rail F-H)
SEEDS = [(222, 158), (132, 168), (92, 118), (292, 128), (192, 88), (308, 115), (81, 133)]
ROCK_BOX = (119, 50, 147, 81)
ROCK_COLOURS = {(107, 107, 99), (140, 140, 123), (82, 82, 82), (231, 222, 181), (66, 66, 90), (165, 156, 140),
                (198, 181, 156), OUTLINE}
BUSH_BG1_BOX = (160, 140, 215, 188)

# (name, kind, rectangles (x0, y0, x1, y1) inclusive): first match wins.
# kind: 'fence' keeps only the wood (and outline) of the pixels enclosed between
# its rails; 'plant' keeps all.
OBJECTS = [
    ("sign", "fence", [(170, 80, 192, 110)]),
    # The left fences: two lines along Z, the front one by the path (posts C D E G,
    # feet on X -18) and the back one (posts A B F H, X -44). Front first.
    ("fence_front", "fence", [
        (137, 100, 145, 122),     # post C
        (143, 101, 158, 112),     # rail from C toward the back right (ends under A's foot)
        (143, 96, 149, 100),
        (127, 105, 137, 120),     # rail C-D
        (120, 106, 128, 131),     # post D
        (111, 111, 120, 125),     # rail D-E
        (104, 114, 112, 138),     # post E
        (95, 118, 104, 134),      # rail E-G
        (88, 123, 97, 146),       # post G
    ]),
    ("fence_back", "fence", [
        (148, 76, 157, 101),      # post A
        (157, 76, 166, 94),       # A's rail on to the right
        (140, 84, 149, 100),      # rail A-B
        (132, 84, 141, 110),      # post B
        (84, 106, 93, 124),       # post F
        (93, 106, 99, 119),       # F's rail on to the right, behind E
        (77, 112, 86, 130),       # rail F-H
        (70, 114, 79, 142),       # post H
    ]),
    ("bush_left", "plant", [(70, 120, 100, 158)]),
    # The top: the big tree (its trunk's foot at 207-213, 88), a second tree whose
    # trunk rises at x 222-228 into leaves past the frame's top, a berry bush.
    ("tree2", "plant", [(221, 28, 229, 57), (231, 28, 262, 47)]),
    ("bush_top", "plant", [(233, 46, 257, 67)]),
    ("tree", "plant", [(183, 28, 240, 95), (178, 45, 182, 95)]),   # not the ledge's outline at row 44
    # The top right: berry bush, a broken post leaning with its rail, two posts.
    ("bush_tr", "plant", [(258, 28, 285, 41), (276, 41, 286, 47)]),
    ("fence_tr_broken", "fence", [(255, 40, 284, 62)]),
    ("fence_tr", "fence", [(284, 28, 312, 52)]),
    # The right: a post of the near fence (X ~144, as the bottom one), berry bushes,
    # a post of a short fence along X (its rail stubs slope +0.5).
    ("fence_r1", "fence", [(294, 98, 312, 127)]),
    ("fence_r2", "fence", [(300, 137, 307, 163), (293, 142, 299, 147), (307, 147, 311, 153)]),   # post, rail stubs
    ("bush_r1", "plant", [(283, 100, 312, 137)]),     # the bush at R1's foot
    ("bush_r", "plant", [(283, 138, 312, 162)]),      # the bush round R2
    ("bush_r2", "plant", [(274, 140, 290, 164)]),
    # The bottom: the near fence (along Z, X ~144) with leaves, a berry bush.
    ("fence_bottom", "fence", [(140, 164, 159, 188), (159, 164, 168, 188)]),
    ("leaves_bottom", "plant", [(140, 164, 182, 188)]),
    ("bush_bottom", "plant", [(190, 168, 216, 188)]),
]
# the grass is g > r; the berries are red with b > g
FENCE_WOOD = lambda c: (c[0] >= c[1] and not (c[2] > c[1] and c[0] > 1.5 * c[1])) or c == OUTLINE   # noqa: E731


def main(prefix, labels=None):
    info = open(prefix + ".info").read().split("\n")[0].split()
    p = {info[i]: int(info[i + 1]) for i in range(0, len(info) - 1, 2)}
    W, H = p["w"], p["h"]
    KX, KY = p["kx"] + p["ox"], p["ky"] + p["oy"]
    AX0, AY0, AX1, AY1 = p["ox"], p["oy"], p["ox"] + 239, p["oy"] + 159
    rgb = open(prefix + ".rgb", "rb").read()
    lay = open(prefix + ".layers", "rb").read()

    def col(x, y):
        i = (y * W + x) * 3
        return rgb[i], rgb[i + 1], rgb[i + 2]

    def layer(x, y):
        return lay[y * W + x] & 15

    def inside(x, y):
        return AX0 <= x <= AX1 and AY0 <= y <= AY1

    ol = {(x, y) for y in range(AY0, AY1 + 1) for x in range(AX0, AX1 + 1)
          if layer(x, y) == 0 and col(x, y) == OUTLINE}
    wall = set(ol)
    for y in range(AY0, AY1 + 1):
        xs = sorted(x for x in range(AX0, AX1 + 1) if (x, y) in ol)
        for a, b in zip(xs, xs[1:]):
            if 1 < b - a <= GAP_H + 1:
                wall.update((x, y) for x in range(a + 1, b))
    for x in range(AX0, AX1 + 1):
        ys = sorted(y for y in range(AY0, AY1 + 1) if (x, y) in ol)
        for a, b in zip(ys, ys[1:]):
            if 1 < b - a <= GAP_V + 1:
                wall.update((x, y) for y in range(a + 1, b))
    seen = set(SEEDS)
    st = list(SEEDS)
    while st:
        x, y = st.pop()
        for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if inside(*q) and q not in seen and q not in wall:
                seen.add(q)
                st.append(q)
    objpx = {(x, y) for y in range(AY0, AY1 + 1) for x in range(AX0, AX1 + 1)
             if (x, y) not in seen and layer(x, y) == 0}

    owner = {}
    for (x, y) in sorted(objpx):
        for name, kind, rects in OBJECTS:
            if any(a <= x <= c and b <= y <= d for a, b, c, d in rects):
                if kind == "fence" and (x, y) not in ol and not FENCE_WOOD(col(x, y)):
                    continue  # grass or berries seen between its rails (also in a wall gap): a plant's, or the ground's
                owner[(x, y)] = name
                break
    lost = [q for q in objpx if q not in owner]

    # The rock: its palette in its box, the largest 8-connected piece, holes filled.
    x0, y0, x1, y1 = ROCK_BOX
    rock = {(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)
            if layer(x, y) in (0, 1) and col(x, y) in ROCK_COLOURS}
    best = set()
    left = set(rock)
    while left:
        s = left.pop()
        cc = {s}
        st = [s]
        while st:
            x, y = st.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in left:
                        left.discard(q)
                        cc.add(q)
                        st.append(q)
        if len(cc) > len(best):
            best = cc
    for x in range(x0, x1 + 1):
        ys = [y for y in range(y0, y1 + 1) if (x, y) in best]
        if ys:
            for y in range(min(ys), max(ys) + 1):
                owner[(x, y)] = "rock"
    # The BG1 bush.
    x0, y0, x1, y1 = BUSH_BG1_BOX
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if layer(x, y) == 1:
                owner[(x, y)] = "bush_bg1"

    # The ground: per column, under the far hills.
    def far_hill(x, y):
        if layer(x, y) == 5:
            return True
        r, g, b = col(x, y)
        return layer(x, y) == 0 and g > r and b >= 100 and r + g + b > 400
    gtop = {}
    for x in range(AX0, AX1 + 1):
        y = AY0
        while y <= AY1 and far_hill(x, y):
            y += 1
        gtop[x] = y

    names = [o[0] for o in OBJECTS] + ["rock", "bush_bg1"]
    runs = {n: {} for n in names}
    for (x, y), n in owner.items():
        runs[n].setdefault(x, []).append(y)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "room93_art.py"), "w") as o:
        o.write("# Generated by tools/models/room93_extract.py from %s: do not edit.\n" % os.path.basename(prefix))
        o.write("# Map 93's art per object and column of the flat frame (W %d, H %d, KX %d, KY %d):\n" % (W, H, KX, KY))
        o.write("# {column: [(first row, last row), ...]}.\n")
        o.write("W, H, KX, KY = %d, %d, %d, %d\n" % (W, H, KX, KY))
        o.write("ART = (%d, %d, %d, %d)\n" % (AX0, AY0, AX1, AY1))
        o.write("OBJECTS = {\n")
        for n in names:
            o.write("    %r: {\n" % n)
            for x in sorted(runs[n]):
                ys = sorted(runs[n][x])
                rr = []
                for y in ys:
                    if rr and rr[-1][1] == y - 1:
                        rr[-1][1] = y
                    else:
                        rr.append([y, y])
                o.write("        %d: %r,\n" % (x, [tuple(a) for a in rr]))
            o.write("    },\n")
        o.write("}\n")
        o.write("GROUND_TOP = {\n")
        for x in sorted(gtop):
            o.write("    %d: %d,\n" % (x, gtop[x]))
        o.write("}\n")
        o.write("GROUND_BOTTOM = %d\n" % AY1)
    print("objects:", ", ".join("%s %d" % (n, sum(1 for v in owner.values() if v == n)) for n in names))
    print("object pixels in no object:", len(lost), sorted(lost)[:40])

    if labels:
        from PIL import Image
        pal = [(255, 60, 60), (60, 255, 60), (60, 120, 255), (255, 255, 60), (255, 60, 255), (60, 255, 255),
               (255, 160, 40), (160, 80, 255), (255, 255, 255), (120, 255, 160), (255, 120, 160), (160, 160, 60)]
        im = Image.new("RGB", (W, H))
        for y in range(H):
            for x in range(W):
                c = col(x, y)
                n = owner.get((x, y))
                if n is None:
                    g = sum(c) // 9
                    if (x, y) in lost:
                        im.putpixel((x, y), (255, 0, 0))
                    elif inside(x, y) and y < gtop.get(x, 0):
                        im.putpixel((x, y), (0, 0, g))
                    else:
                        im.putpixel((x, y), (g, g, g))
                else:
                    k = pal[names.index(n) % len(pal)]
                    m = (sum(c) / 765.0) * 0.6 + 0.4
                    im.putpixel((x, y), tuple(int(v * m) for v in k))
        im.save(labels)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--labels" else None)
