#!/usr/bin/env python3
"""Reads map 75's art into room75_art.py: the roof's edge per column, the
stalactites' runs and axes, the vines' runs (room75.py makes the model of them).

    tools/models/room75_extract.py DIR [OUT]

DIR holds flat viewer dumps at 480x270 (EV + KOL_DEBUG_MAP=75, A at 1324,
KOL_FOCUS_DUMP + present dump): <tag>_t.png at KOL_VIEWER_CELL=0,0 and <tag>_c.png
at center, for tag bg0 (KOL_SETTINGS_ON=debug_hide_bg1) and bg1 (debug_hide_bg0),
and the plain views' layer maps: top.layers (at 0,0) and centre.layers (at center).
The two views are pasted 240 rows apart, so columns and rows here are a frame with
the world's origin at (240, 287).
"""
import os
import sys
from PIL import Image

W, H = 480, 510

# What the colour rule gets wrong, read off the art at 11x (first column, last
# column, row). CAP: a stalactite ends by this row; below it is the repaint of the
# rock behind.
CAP = [(128, 129, 265), (130, 133, 271), (134, 143, 292), (267, 280, 247), (345, 347, 283),
       (348, 357, 287), (358, 363, 288), (364, 366, 291), (367, 374, 303), (375, 375, 300),
       (376, 379, 313), (380, 383, 309)]
# A vine's tip the strong green misses (x0, y0, x1, y1).
VINE_BOXES = [(323, 284, 328, 288)]


def load(d, tag):
    m = Image.new('RGB', (W, H))
    m.paste(Image.open(os.path.join(d, tag + '_c.png')).convert('RGB'), (0, 240))
    m.paste(Image.open(os.path.join(d, tag + '_t.png')).convert('RGB'), (0, 0))
    return m


def layers(d):
    lay = bytearray(W * H)
    c = open(os.path.join(d, 'centre.layers'), 'rb').read()
    t = open(os.path.join(d, 'top.layers'), 'rb').read()
    for y in range(270):
        lay[(y + 240) * W:(y + 240) * W + W] = c[y * W:y * W + W]
    for y in range(270):
        lay[y * W:y * W + W] = t[y * W:y * W + W]
    return lay


def classify(px):
    """A BG1 pixel by its colour (the palette shifts by row, so by relation):
    V strong green, v faint green, R the roof's near black, D dark blue, M mid
    blue, H a highlight, o the repaint of rock and floor."""
    r, g, b = px
    if g > r + 22 and g > b + 18:
        return 'V'
    if g > r + 10 and g > b + 8:
        return 'v'
    if b - g <= 12 and r <= 45 and g <= 66:
        return 'R'
    if r <= 62 and b >= r + 24 and b >= g + 6:
        return 'D'
    if r <= 100 and b >= r + 45 and b >= g + 15:
        return 'M'
    if r >= 120 and b >= 100 and abs(r - g) < 40:
        return 'H'
    return 'o'


def components(pixels, reach=1):
    seen, out = set(), []
    for k in sorted(pixels):
        if k in seen:
            continue
        st, comp = [k], []
        seen.add(k)
        while st:
            a = st.pop()
            comp.append(a)
            for dx in range(-reach, reach + 1):
                for dy in range(-reach, reach + 1):
                    q = (a[0] + dx, a[1] + dy)
                    if q in pixels and q not in seen:
                        seen.add(q)
                        st.append(q)
        out.append(comp)
    return out


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'room75_art.py')
    b1 = load(d, 'bg1')
    p = b1.load()
    lay = layers(d)
    C = {}
    for y in range(150, 350):
        for x in range(70, 420):
            if lay[y * W + x] & 15 == 1:
                C[(x, y)] = classify(p[x, y])

    # The roof's edge: the first of three blue-ish pixels down a column.
    E = {}
    for x in range(70, 420):
        for y in range(150, 330):
            if C.get((x, y)) in ('D', 'M') and C.get((x, y + 1)) in ('D', 'M', 'V', 'v') and \
                    C.get((x, y + 2)) in ('D', 'M', 'V', 'v', 'o'):
                E[x] = y
                break

    # A dark crack between stalactites is no notch in the roof's edge.
    for x in sorted(E):
        others = sorted(E[x + i] for i in (-2, -1, 1, 2) if x + i in E)
        if len(others) == 4 and E[x] - others[1] > 3 and E[x] - others[2] > 3:
            E[x] = others[1]

    # The vines: big lumps of strong green, grown along their dark parts.
    V = set()
    for comp in components({k for k, v in C.items() if v == 'V'}, 2):
        if len(comp) >= 25:
            V.update(comp)

    def green(q, lo):
        if q not in C:
            return False
        r, g, b = p[q]
        return g >= r + lo and g >= b + lo - 2
    V |= {(x + dx, y + dy) for (x, y) in V for dx in (-1, 0, 1) for dy in (-1, 0, 1)
          if C.get((x + dx, y + dy)) == 'v'}
    for it in range(5):
        add = set()
        for (x, y) in V:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in V or q not in C:
                        continue
                    if green(q, 9) or (it < 2 and green(q, 5) and p[q][1] < 90):
                        add.add(q)
        V |= add
    V = {k for comp in components(V) if len(comp) >= 8 for k in comp}
    for (x0, y0, x1, y1) in VINE_BOXES:
        V |= {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1) if green((x, y), 5)}

    # The stalactites' bottoms: where the 7 x 7 round a pixel stops being blue.
    # (The vines' first mask: the bottoms were read before they were grown.)
    Vb = set()
    for comp in components({k for k, v in C.items() if v == 'V'}, 2):
        if len(comp) >= 25:
            Vb.update(comp)
    Vb |= {(x + dx, y + dy) for (x, y) in Vb for dx in (-1, 0, 1) for dy in (-1, 0, 1)
           if C.get((x + dx, y + dy)) == 'v'}

    def frac(x, y):
        n = t = 0
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                q = (x + dx, y + dy)
                c = C.get(q)
                if c is None or c == 'R' or q in Vb:
                    continue
                t += 1
                n += c in ('D', 'M')
                n += 0.5 * (c == 'H')
        return n / t if t else 0
    Bl = {}
    for x in sorted(E):
        y, last, miss = E[x], E[x], 0
        while y < 345 and (x, y) in C:
            if frac(x, y) >= 0.40:
                last, miss = y, 0
            else:
                miss += 1
                if miss > 4:
                    break
            y += 1
        Bl[x] = last
    Bc = {}
    for x, y in Bl.items():
        while y + 1 < 345 and C.get((x, y + 1)) in ('D', 'M', 'H'):
            y += 1
        while y > E[x] and C.get((x, y)) in ('o', None, 'v'):
            y -= 1
        Bc[x] = y
    B = {x: max(Bc.get(x - 1, Bc[x]), Bc[x], Bc.get(x + 1, Bc[x]))
         if min(Bc.get(x - 1, Bc[x]), Bc.get(x + 1, Bc[x])) > Bc[x] else Bc[x] for x in Bc}
    tips = []
    for x in sorted(B):
        v = B[x]
        if v == max(B.get(x + i, -1) for i in range(-3, 4)) and \
                v - min(B.get(x + i, v) for i in (-5, -4, 4, 5)) >= 3:
            if not (tips and x - tips[-1][0] < 3):
                tips.append((x, v))

    for (c0, c1, r) in CAP:
        for c in range(c0, c1 + 1):
            if c in B:
                B[c] = min(B[c], r)

    # The roof: near-black in 2 x 2 blocks joined to the band over the edge.
    R = {k for k, v in C.items() if v == 'R'}
    solid = set()
    for (x, y) in R:
        if (x + 1, y) in R and (x, y + 1) in R and (x + 1, y + 1) in R:
            solid |= {(x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)}
    roof = {k for k in solid if k[1] <= E.get(k[0], 0)}
    st = list(roof)
    while st:
        x, y = st.pop()
        for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if q in solid and q not in roof:
                roof.add(q)
                st.append(q)

    # A column: runs of stalactite, split by the roof's tabs.
    stal, tabs = {}, {}
    for c in sorted(E):
        segs, tabrun, y, b, cur, t = [], [], E[c], B[c], E[c], None
        while y <= b:
            if (c, y) in roof:
                if t is None:
                    t = y
                    if y > cur:
                        segs.append((cur, y - 1))
            elif t is not None:
                tabrun.append((t, y - 1))
                t, cur = None, y
            y += 1
        if t is None:
            if b >= cur:
                segs.append((cur, b))
        else:
            tabrun.append((t, b))
        stal[c], tabs[c] = segs, tabrun

    # A stalactite to each highlight and each free tip.
    axes = []
    Hi = {k for k, v in C.items() if v == 'H' and k[0] in E and E[k[0]] <= k[1] <= B[k[0]]}
    for comp in components(Hi):
        if len(comp) >= 3:
            axes.append((round(sum(a for a, _ in comp) / len(comp), 1), max(b for _, b in comp) + 3))
    for (x, y) in tips:
        if not any(abs(a - x) <= 3 and abs(b - y) <= 8 for a, b in axes):
            axes.append((float(x), y))
    axes = [(a, min(b, max(B.get(int(a) + i, 0) for i in (-1, 0, 1)))) for a, b in axes if int(a) in B]

    vruns = []
    for comp in components(V, 2):
        cols = {}
        for (x, y) in comp:
            cols.setdefault(x, []).append(y)
        runs = []
        for x in sorted(cols):
            ys = sorted(cols[x])
            a = q = ys[0]
            for y in ys[1:] + [None]:
                if y is None or y > q + 2:
                    runs.append((x, a, q))
                    if y is not None:
                        a = y
                if y is not None:
                    q = y
        vruns.append(runs)

    with open(out, 'w') as o:
        o.write("# Generated by tools/models/room75_extract.py from map 75's flat dumps: frame KX 240, KY 287.\n")
        o.write('STAL = {\n')
        for c in sorted(stal):
            o.write(' %d: %s,\n' % (c, stal[c]))
        o.write('}\nTABS = {\n')
        for c in sorted(tabs):
            if tabs[c]:
                o.write(' %d: %s,\n' % (c, tabs[c]))
        o.write('}\nTOP = {\n')
        for c in sorted(E):
            o.write(' %d: %d,\n' % (c, E[c]))
        o.write('}\nAXES = [\n')
        for a in sorted(axes):
            o.write(' %s,\n' % (a,))
        o.write(']\nVINES = [\n')
        for r in vruns:
            o.write(' %s,\n' % r)
        o.write(']\n')
    print('%d columns, %d stalactites, %d vine runs -> %s' % (len(stal), len(axes), sum(len(r) for r in vruns), out))


if __name__ == '__main__':
    main()
