#!/usr/bin/env python3
"""Rearrange a map's background tiles (the game's own) so the background runs
on without seams, and write the result as map editor lines in map_ext.cfg.

    tools/cloud_tiles.py --map 94 --preview out.png   # look first
    tools/cloud_tiles.py --map 94 --apply             # back up, then write
    tools/cloud_tiles.py --map 94 --revert            # put the backup back

Made for map 94 (the cliff over the clouds, 2026-11-12; the user asked
for the best-matching tiles for a continuous background, the central art
included, not just the sides and top). No new pixels: every cell
gets one of the tiles the game itself uses for the background, from about
the same row of its art (the sky, the cloud tops and the clouds below keep
their order).

Input: tools/map_editor/maps/map<N>.json (export_map.py: tiles, palette, the
game's composed layers) and the map's lines in map_ext.cfg (extension cells
and overrides). A cell with any rock in it (red over blue by 20) is fixed;
every other cell of the layer is free: the extension's, the game's own
(written as `ov` lines, the game's entry as `orig`), and the empty cells
in the columns the camera can show (--cols), down to the art's last row.
Each free cell takes the tile with the least colour jump to its four
neighbours, plus a cost for leaving the game's own tile (--change-cost), for
a tile used next to itself, and per row away from where the game uses it;
sweeps until nothing changes (empty cells start from a greedy pass in
reading order).

--apply saves the map's lines as they were to
shots/<map>_clouds/map_ext_before.cfg first (overwritten only by a later
--apply), then replaces the map's lines on that layer; the other layer's
lines and the rock cells are left as they are. --revert puts the saved
lines back. Reload the map editor's page afterwards: it writes the map's
lines back as it holds them when it saves. The Thor reads its own copy
(files/map_ext.cfg): the editor's "Send to Thor" copies it.
"""
import argparse
import base64
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKY = (239, 255, 255)   # the backdrop under an empty cell (map 94's sky)
NB = {'R': (0, 1), 'L': (0, -1), 'D': (1, 0), 'U': (-1, 0)}


def load(mp, layer, cfg):
    d = json.load(open(os.path.join(REPO, 'tools/map_editor/maps', f'map{mp}.json')))
    tiles = base64.b64decode(d['tiles'])
    pal = [tuple(c) for c in d['palette']]
    C = d['cols']
    game = {(i // C, i % C): v for i, v in enumerate(d[f'bg{layer}']) if v}
    ext, ov, ov_orig = {}, {}, {}
    for line in open(cfg):
        p = line.split()
        if not p or p[0].startswith('#'):
            continue
        if p[0] == 'ov' and p[1] == str(mp) and p[2] == str(layer):
            ov[(int(p[3]), int(p[4]))] = int(p[5], 16)
            ov_orig[(int(p[3]), int(p[4]))] = int(p[6], 16)
        elif p[0] == str(mp) and p[1] == str(layer):
            ext[(int(p[2]), int(p[3]))] = int(p[4], 16)
    return tiles, pal, game, ext, ov, ov_orig


def cell_rgb(tiles, pal, e):
    if e == 0:
        return [[SKY] * 8 for _ in range(8)]
    t = (e & 0x3FF) * 32
    bank = (e >> 12) & 15
    out = []
    for y in range(8):
        sy = 7 - y if e & 0x800 else y
        row = []
        for x in range(8):
            sx = 7 - x if e & 0x400 else x
            i = t + sy * 4 + sx // 2
            b = tiles[i] if i < len(tiles) else 0
            v = (b >> 4) if sx & 1 else (b & 15)
            row.append(SKY if v == 0 else pal[bank * 16 + v])
        out.append(row)
    return out


def solve(a, tiles, pal, game, ext, ov):
    cache = {}

    def px(e):
        if e not in cache:
            cache[e] = cell_rgb(tiles, pal, e)
        return cache[e]

    def is_rock(e):
        return e != 0 and any(p[0] > p[2] + 20 for row in px(e) for p in row)

    def is_plain(e):   # one colour (the sky): repeating it is no repeat
        P = px(e)
        return all(p == P[0][0] for row in P for p in row)

    comp = dict(game)
    comp.update(ext)
    comp.update(ov)
    # Rock stays, and so does the sky over the clouds: a cell of pure sky is
    # left as it is (with the repeat cost the solver had put cloud specks there).
    fixed = {k for k, e in comp.items() if is_rock(e) or (e != 0 and is_plain(e))}
    by_row, row_of = {}, {}
    for (r, c), e in game.items():
        if not is_rock(e):
            by_row.setdefault(r, set()).add(e)
            row_of.setdefault(e, set()).add(r)
    R0, R1 = min(by_row), max(by_row)

    def cands(r):
        rr = min(R1, max(R0, r))
        s = set()
        for d in range(-a.row_span, a.row_span + 1):
            s |= by_row.get(rr + d, set())
        return sorted(s)

    def row_cost(r, e):
        if e not in row_of:
            return 0.0
        rr = min(R1, max(R0, r))
        return a.row_cost * min(abs(rr - x) for x in row_of[e])

    def edge(x, y, side):
        A, B = px(x), px(y)
        if side == 'R':
            pa, pb = [A[i][7] for i in range(8)], [B[i][0] for i in range(8)]
        elif side == 'L':
            pa, pb = [A[i][0] for i in range(8)], [B[i][7] for i in range(8)]
        elif side == 'D':
            pa, pb = A[7], B[0]
        else:
            pa, pb = A[0], B[7]
        return sum((p[i] - q[i]) ** 2 for p, q in zip(pa, pb) for i in range(3))

    empties = set()
    rows = [r for r, c in comp]
    for r in range(min(rows), R1 + 1):
        for c in range(a.cols[0], a.cols[1]):
            if (r, c) not in comp:
                comp[(r, c)] = 0
                empties.add((r, c))
    cur = dict(comp)

    def cost(k, e, filled_only=False):
        r, c = k
        s = 0
        for side, (dr, dc) in NB.items():
            n = (r + dr, c + dc)
            if n in cur and not (filled_only and cur[n] == 0):
                s += edge(e, cur[n], side)
                if cur[n] == e and not is_plain(e):
                    s += a.repeat_cost
        if k in game and e != game[k]:
            s += a.change_cost
        return s + row_cost(r, e)

    for k in sorted(empties):
        if k[0] < R0 - 2:
            continue   # sky over the clouds: left empty
        cs = cands(k[0])
        if cs:
            cur[k] = min(cs, key=lambda e: cost(k, e, True))
    free = [k for k in comp if k not in fixed and not (k in empties and cur[k] == 0)]
    for sweep in range(20):
        changed = 0
        for k in (sorted(free) if sweep % 2 == 0 else sorted(free, reverse=True)):
            best, bc = cur[k], cost(k, cur[k])
            for e in cands(k[0]):
                c = cost(k, e)
                if c < bc:
                    bc, best = c, e
            if best != cur[k]:
                cur[k] = best
                changed += 1
        print(f'sweep {sweep}: {changed} changed', file=sys.stderr)
        if not changed:
            break
    return comp, cur, px


def lines_for(mp, layer, game, ext, ov, ov_orig, cur):
    """The map's lines on this layer: extension cells and overrides."""
    out = []
    for (r, c), e in sorted(cur.items()):
        if (r, c) in game:
            if e != game[(r, c)]:
                orig = ov_orig.get((r, c), game[(r, c)])
                out.append(f'ov {mp} {layer} {r} {c} {e:04x} {orig:04x}')
        elif e != 0:
            out.append(f'{mp} {layer} {r} {c} {e:04x}')
    return out


def render(px, m, c0, c1, r0, r1):
    from PIL import Image
    im = Image.new('RGB', ((c1 - c0) * 8, (r1 - r0) * 8), SKY)
    p = im.load()
    for (r, c), e in m.items():
        if e and r0 <= r < r1 and c0 <= c < c1:
            P = px(e)
            for y in range(8):
                for x in range(8):
                    p[(c - c0) * 8 + x, (r - r0) * 8 + y] = P[y][x]
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', type=int, required=True)
    ap.add_argument('--layer', type=int, default=0)
    ap.add_argument('--cfg', default=os.path.join(REPO, 'map_ext.cfg'))
    ap.add_argument('--cols', type=int, nargs=2, default=(-16, 48), help='columns the camera can show')
    ap.add_argument('--change-cost', type=float, default=3000.0)
    ap.add_argument('--repeat-cost', type=float, default=4000.0)
    ap.add_argument('--row-cost', type=float, default=500.0)
    ap.add_argument('--row-span', type=int, default=3)
    ap.add_argument('--preview')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--revert', action='store_true')
    a = ap.parse_args()
    backup_dir = os.path.join(REPO, 'shots', f'map{a.map}_clouds')
    backup = os.path.join(backup_dir, 'map_ext_before.cfg')

    def is_map_line(line):
        p = line.split()
        return bool(p) and ((p[0] == 'ov' and p[1:2] == [str(a.map)]) or p[0] == str(a.map))

    if a.revert:
        if not os.path.exists(backup):
            sys.exit(f'no backup at {backup}')
        keep = [l for l in open(a.cfg) if not is_map_line(l)]
        with open(a.cfg, 'w') as f:
            f.writelines(keep + open(backup).readlines())
        print(f'map {a.map}: lines put back from {backup}')
        return
    tiles, pal, game, ext, ov, ov_orig = load(a.map, a.layer, a.cfg)
    comp, cur, px = solve(a, tiles, pal, game, ext, ov)
    ng = sum(1 for k in game if cur.get(k, game[k]) != game[k])
    print(f'map {a.map} layer {a.layer}: game cells changed {ng}, extension cells {sum(1 for k in cur if k not in game and cur[k])}',
          file=sys.stderr)
    if a.preview:
        rs = [r for r, c in cur if cur[(r, c)]]
        before = render(px, comp, a.cols[0], a.cols[1], min(rs), max(rs) + 1)
        after = render(px, cur, a.cols[0], a.cols[1], min(rs), max(rs) + 1)
        from PIL import Image
        z = Image.new('RGB', (before.width, before.height * 2 + 6), (255, 255, 255))
        z.paste(before, (0, 0))
        z.paste(after, (0, before.height + 6))
        z.save(a.preview)
    if a.apply:
        old = open(a.cfg).readlines()
        os.makedirs(backup_dir, exist_ok=True)
        if not os.path.exists(backup):   # the lines before the first --apply
            with open(backup, 'w') as f:
                f.writelines(l for l in old if is_map_line(l))
        def this_layer(l):
            p = l.split()
            return is_map_line(l) and ((p[0] == 'ov' and p[2] == str(a.layer)) or (p[0] != 'ov' and p[1] == str(a.layer)))
        keep = [l for l in old if not this_layer(l)]
        new = lines_for(a.map, a.layer, game, ext, ov, ov_orig, cur)
        with open(a.cfg, 'w') as f:
            f.writelines(keep)
            f.writelines(l + '\n' for l in new)
        print(f'map {a.map}: {len(new)} lines written; backup {backup}')


if __name__ == '__main__':
    main()
