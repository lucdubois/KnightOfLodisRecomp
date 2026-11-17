#!/usr/bin/env python3
"""Our own rain, mocked up over a present dump (the game's drops hidden).

    tools/mockup/rain.py PRESENT.bmp FOCUS_PREFIX CAMLOG OUT_PREFIX [variant] [frames]

PRESENT.bmp: GBARECOMP_PRESENT_DUMP with KOL_RAIN_MOCKUP=1 (no drops at all);
FOCUS_PREFIX: KOL_FOCUS_DUMP of the same frame (.wx .wz .y .face .info);
CAMLOG: a stderr log with KOL_RAIN_CAM=1 (the perspective's camera).
Drops live in the world: X/Z over the map, falling straight down (a little
wind), projected with the perspective's own formulas (kol_perspective
corner_at), so near ones are bigger and faster; each lands on the ground the
world buffers give, with a splash where that ground is seen.
"""
import array
import math
import random
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from png import write_png  # noqa: E402

VARIANTS = {
    # drops (in the world box), fall per frame, streak length, alpha, colour,
    # lean (times WIND's slant)
    # i0..i2: the game's rain intensity 0..2 (the user's picks)
    "i0":     dict(n=130,  v=22.0, L=28.0, a=0.38, rgb=(214, 222, 234)),
    "i1":     dict(n=300,  v=22.0, L=28.0, a=0.38, rgb=(214, 222, 234)),
    "i2":     dict(n=600,  v=22.0, L=28.0, a=0.38, rgb=(214, 222, 234), lean=2.2),
    "light":  dict(n=900,  v=22.0, L=30.0, a=0.42, rgb=(214, 222, 234)),
    "medium": dict(n=1800, v=24.0, L=34.0, a=0.48, rgb=(214, 222, 234)),
    "heavy":  dict(n=3400, v=26.0, L=40.0, a=0.52, rgb=(214, 222, 234)),
}
HMAX = 420.0          # world units a drop falls from (above its ground)
WIND = (3.0, -3.0)    # tail offset per 34 units of streak (world X, Z): leans right
SPLASH_FRAMES = 6


def read_bmp(p):
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    flip = h > 0
    h = abs(h)
    rs = ((w * bpp + 3) // 4) * 4
    rgb = bytearray(w * h * 3)
    for y in range(h):
        sy = h - 1 - y if flip else y
        for x in range(w):
            i = off + sy * rs + x * bpp
            o = (y * w + x) * 3
            rgb[o], rgb[o + 1], rgb[o + 2] = d[i + 2], d[i + 1], d[i]
    return w, h, rgb


def read_cam(p):
    cam = None
    for line in open(p):
        if line.startswith("raincam"):
            t = line.split()
            kv = {}
            i = 1
            while i < len(t):
                k = t[i]
                if k == "view":
                    kv[k] = [int(v) for v in t[i + 1:i + 5]]; i += 5
                elif k == "wh":
                    kv[k] = [int(v) for v in t[i + 1:i + 3]]; i += 3
                else:
                    kv[k] = float(t[i + 1]); i += 2
            cam = kv
    return cam


class Proj:
    def __init__(self, c):
        self.c = c

    def __call__(self, X, Y, Z):
        c = self.c
        d = X + Z
        den = c["K"] + c["dref"] - d
        s = c["K"] / den if den > 0.25 * c["K"] else 4.0
        col = c["ox"] + c["kx"] + (X - Z) / 2.0
        tx = c["cxp"] + (col - c["cxp"]) * s
        ty = c["base"] + (c["dref"] + (d - c["dref"]) * s * c["pitch"]) / 4.0 - (c["yref"] + (Y - c["yref"]) * s) / 2.0
        z = c["zoom"]
        if z != 1.0:
            tx = c["cx"] + (tx - c["cx"]) * z
            ty = c["cy"] + (ty - c["cy"]) * z
        return tx - c["view"][0], ty - c["view"][1], s


def blend(img, w, h, x, y, rgb, a):
    xi, yi = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - xi, y - yi
    for dx, dy, wt in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        px, py = xi + dx, yi + dy
        if 0 <= px < w and 0 <= py < h:
            k = wt * a
            o = (py * w + px) * 3
            for c in range(3):
                img[o + c] = min(255.0, img[o + c] * (1 - k) + rgb[c] * k)


def line(img, w, h, x0, y0, x1, y1, rgb, a0, a1, width=1.0):
    n = max(2, int(math.hypot(x1 - x0, y1 - y0) * 3))
    for i in range(n + 1):
        t = i / n
        a = (a0 + (a1 - a0) * t) / 3.0 * width
        blend(img, w, h, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, rgb, min(a, 1.0))


def main():
    pres, focus, camlog, out = sys.argv[1:5]
    variant = sys.argv[5] if len(sys.argv) > 5 else "i1"
    frames = int(sys.argv[6]) if len(sys.argv) > 6 else 1
    V = VARIANTS[variant]
    W, H, base = read_bmp(pres)
    proj = Proj(read_cam(camlog))
    info = open(focus + ".info").read().split()
    sw, sh = int(info[1]), int(info[3])
    wy = array.array("i"); wy.frombytes(open(focus + ".y", "rb").read())
    face = open(focus + ".face", "rb").read()
    c = proj.c
    # The ground: highest top face per 8x8 world square (seen pixels, put
    # back in the world by their height: X + Z and X - Z from the flat frame).
    ground = {}
    kxo = c["ox"] + c["kx"]
    for k in range(sw * sh):
        if face[k] != 0:
            continue
        Y = wy[k]
        d = 4.0 * (k // sw - c["base"] + Y / 2.0)
        e = 2.0 * (k % sw - kxo)
        key = (int((d + e) / 2) >> 3, int((d - e) / 2) >> 3)
        if Y > ground.get(key, -9999):
            ground[key] = Y
    ys = sorted(ground.values())
    y_mid = ys[len(ys) // 2] if ys else 0

    def ground_at(X, Z):
        g = ground.get((int(X) >> 3, int(Z) >> 3))
        return (g, True) if g is not None else (y_mid, False)

    def seen(X, Y, Z):
        # The landing is seen where the flat frame's pixel there is that place.
        col = int(round(c["ox"] + c["kx"] + (X - Z) / 2.0))
        row = int(round(c["base"] + (X + Z) / 4.0 - Y / 2.0))
        if not (0 <= col < sw and 0 <= row < sh):
            return False
        k = row * sw + col
        return face[k] == 0 and abs(wy[k] - Y) <= 6

    rnd = random.Random(7)
    drops = []
    while len(drops) < V["n"]:
        X = rnd.uniform(-260.0, 760.0)
        Z = rnd.uniform(-160.0, 860.0)
        g, known = ground_at(X, Z)
        sx, sy, s = proj(X, g, Z)
        top = proj(X, g + HMAX, Z)[1]
        if sx < -20 or sx > W + 20 or sy < -10 or top > H + 10:
            continue
        drops.append((X, Z, g, rnd.uniform(0.0, HMAX), rnd.uniform(0.8, 1.15), known and seen(X, g, Z)))
    lean = V.get("lean", 1.0)
    wx, wz = WIND[0] / 34.0 * lean, WIND[1] / 34.0 * lean   # slant per unit of height
    for f in range(frames):
        img = [float(v) for v in base]
        for (X, Z, g, h0, sp, splash) in drops:
            v = V["v"] * sp
            alt = (h0 - v * f) % HMAX
            # It falls along its slant, so it lands at (X, Z).
            sx, sy, s = proj(X + wx * alt, g + alt, Z + wz * alt)
            tx, ty, _ = proj(X + wx * (alt + V["L"]), g + alt + V["L"], Z + wz * (alt + V["L"]))
            if -4 < sx < W + 4 and -40 < sy < H + 4:
                line(img, W, H, sx, sy, tx, ty, V["rgb"], V["a"] * min(1.0, s), 0.0, width=min(1.6, s))
            # Its last landing: alt wrapped (HMAX - alt) / v frames ago.
            age = (HMAX - alt) / v
            if splash and age < SPLASH_FRAMES:
                gx, gy, gs = proj(X, g, Z)
                r = (1.2 + age * 0.9) * gs
                a = 0.55 * (1.0 - age / SPLASH_FRAMES)
                for i in range(14):
                    t = 2 * math.pi * i / 14
                    blend(img, W, H, gx + r * math.cos(t), gy + 0.5 * r * math.sin(t), V["rgb"], a * 0.6)
                if age < 3:   # two droplets thrown up
                    up = (age * 2.2 - age * age * 0.5) * gs
                    blend(img, W, H, gx - 1.6 * gs - age * 0.6, gy - up, V["rgb"], a)
                    blend(img, W, H, gx + 1.6 * gs + age * 0.6, gy - up * 0.8, V["rgb"], a)
        name = "%s_%s.png" % (out, variant) if frames == 1 else "%s_%s_%03d.png" % (out, variant, f)
        write_png(name, W, H, bytes(int(v + 0.5) for v in img))
    print("%d drops, %d with a splash" % (len(drops), sum(1 for d in drops if d[5])))


if __name__ == "__main__":
    main()
