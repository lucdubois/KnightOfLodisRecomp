# Mockup: architectural perspective of a KoL battle frame. Distance-keyed
# scale, verticals kept vertical. Reads the KOL_MOCK_DUMP files.
import struct, sys, math
from png import write_png

def load(prefix):
    d = {}
    hdr = open(prefix + '.txt').read().split('\n')
    f = hdr[0].split()
    d['w'], d['h'], d['kx'], d['ky'], d['ox'], d['oy'], d['cursor'] = map(int, f[:7])
    d['fxz'], d['fy'] = float(f[7]), float(f[8])
    n = d['w'] * d['h']
    d['rgb'] = open(prefix + '.rgb', 'rb').read()
    d['layers'] = open(prefix + '.layers', 'rb').read()
    d['cov'] = open(prefix + '.covered', 'rb').read()
    d['xz'] = struct.unpack('<%di' % n, open(prefix + '.xz', 'rb').read())
    d['y'] = struct.unpack('<%di' % n, open(prefix + '.y', 'rb').read())
    return d

IMIN = -2**31

def render(scene, hud, s_far, out_path, scale=4, mark_holes=False, dref_mode='near', max_crack=2):
    w, h = scene['w'], scene['h']
    ox, oy, kx, ky = scene['ox'], scene['oy'], scene['kx'], scene['ky']
    xz, yy, cov, lay, rgb = scene['xz'], scene['y'], scene['cov'], scene['layers'], scene['rgb']
    W, H = w * scale, h * scale
    out = bytearray(W * H * 3)
    written = bytearray(W * H)
    zbuf = [0.0] * (W * H)

    def ground_row(D, Y):   # anchor row of world point
        return oy + ky + D / 4.0 - Y / 2.0

    # Which pixels are scene (warped) and which are sky (background, unwarped).
    scene_px = []
    Dmin, Dmax = 1 << 30, -(1 << 30)
    for py in range(h):
        for px in range(w):
            k = py * w + px
            D, Y = xz[k], yy[k]
            if D == IMIN:           # interface sprite: drawn flat with the HUD
                continue
            default = (D == 4 * (py - oy - ky) and Y == 0 and not cov[k])
            if default:
                continue
            scene_px.append(k)
            if cov[k]:
                Dmin = min(Dmin, D); Dmax = max(Dmax, D)
    # Sky gradient: per source row, the first sky pixel's colour.
    sky = [None] * h
    sky_set = set(scene_px)
    for py in range(h):
        for px in range(w):
            k = py * w + px
            if k not in sky_set:
                sky[py] = rgb[k*3:k*3+3]; break
        if sky[py] is None:
            sky[py] = sky[py - 1] if py else b'\0\0\0'
    for Y in range(H):
        row = sky[min(h - 1, Y // scale)]
        out[Y*W*3:(Y+1)*W*3] = row * W

    # Perspective: s = K / (K + Dref - D); s = 1 at Dref, s_far at Dmin.
    Dref = Dmax if dref_mode == 'near' else (Dmin + Dmax) / 2
    span = Dref - Dmin
    K = s_far * span / (1 - s_far) if s_far < 1 else 1e12
    print('  s at nearest terrain: %.2f' % (K / (K + Dref - Dmax)))
    cx = w / 2.0
    def S(D): return K / (K + Dref - D)
    def map_row(D, Y):
        s = S(D)
        Dp = Dref + (D - Dref) * s
        return ground_row(Dp, Y * s)

    # Map every scene pixel's top-left corner first; a pixel then stretches to
    # its right/lower neighbour when that neighbour is the same surface, so a
    # receding wall stays continuous instead of spreading apart.
    pos = {}
    for k in scene_px:
        py, px = divmod(k, w)
        D, Y = xz[k], yy[k]
        if D == IMIN:
            D, Y = int(scene['fxz']), int(scene['fy'])
        s = S(D)
        off = py - ground_row(D, Y)
        pos[k] = (cx + (px - cx) * s, map_row(D, Y) + off * s, s, D, Y)
    def same_surface(k, n):
        return n in pos and abs(pos[n][3] - pos[k][3]) <= 8 and abs(pos[n][4] - pos[k][4]) <= 4
    order = sorted(scene_px, key=lambda k: (pos[k][3], pos[k][4]))
    for k in order:
        py, px = divmod(k, w)
        x0, y0, s, D, Y = pos[k]
        x1 = x0 + s
        if px + 1 < w and same_surface(k, k + 1):
            x1 = max(x1, pos[k + 1][0])
        y1 = y0 + s
        if py + 1 < h and same_surface(k, k + w):
            y1 = pos[k + w][1]
        elif cov[k] and py + 1 < h and xz[k + w] == D + 4:
            y1 = map_row(D + 4, Y)
        X0, X1 = math.floor(x0 * scale), math.ceil(x1 * scale)
        Y0, Y1 = math.floor(y0 * scale), math.ceil(y1 * scale)
        if X1 <= X0: X1 = X0 + 1
        if Y1 <= Y0: Y1 = Y0 + 1
        c = rgb[k*3:k*3+3]
        X0 = max(0, X0); X1 = min(W, X1)
        if X1 <= X0: continue
        for Yo in range(max(0, Y0), min(H, Y1)):
            base = Yo * W
            out[(base + X0)*3:(base + X1)*3] = c * (X1 - X0)
            written[base + X0:base + X1] = b'\1' * (X1 - X0)
            zbuf[base + X0:base + X1] = [D] * (X1 - X0)

    # Cracks: a short unwritten run between two written pixels in a row takes
    # the colour of the farther side (what a gap between surfaces reveals).
    holes = 0
    for Yo in range(H):
        base = Yo * W
        x = 0
        while x < W:
            if written[base + x]:
                x += 1; continue
            e = x
            while e < W and not written[base + e]: e += 1
            if x > 0 and e < W and e - x <= max_crack * scale:
                l, r = base + x - 1, base + e
                src = l if zbuf[l] <= zbuf[r] else r
                c = out[src*3:src*3+3]
                out[(base + x)*3:(base + e)*3] = (b'\xff\x00\xff' if mark_holes else c) * (e - x)
                holes += e - x
            x = e
    print('  crack pixels filled: %d' % holes)

    # HUD from the HUD frame, unwarped: BG2/BG3 on top, and sprites over them.
    if hud:
        hl, hr = hud['layers'], hud['rgb']
        for py in range(h):
            for px in range(w):
                k = py * w + px
                top, under = hl[k] & 15, hl[k] >> 4
                if top in (2, 3) or (top == 4 and under in (2, 3)) or hud['xz'][k] == IMIN:
                    c = hr[k*3:k*3+3]
                    for Yo in range(py*scale, (py+1)*scale):
                        out[(Yo*W + px*scale)*3:(Yo*W + (px+1)*scale)*3] = c * scale
    write_png(out_path, W, H, out)
    print(out_path, 'K=%.0f Dref=%d Dmin=%d s_far=%.2f' % (K, Dref, Dmin, s_far))

if __name__ == '__main__':
    base = sys.argv[1]
    scene, hud = load(base + '/nohud'), load(base + '/hud')
    render(scene, hud, 0.8, base + '/mid_80_holes.png', dref_mode='mid', mark_holes=True)
    for s_far in [0.9, 0.8]:
        render(scene, hud, s_far, '%s/mid_%02d.png' % (base, int(s_far * 100)), dref_mode='mid')
