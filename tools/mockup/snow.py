#!/usr/bin/env python3
"""Our own snow, mocked up over a present dump (the game's flakes hidden).

    tools/mockup/snow.py PRESENT.bmp FOCUS_PREFIX CAMLOG OUT_PREFIX [variant] [frames]

PRESENT.bmp: GBARECOMP_PRESENT_DUMP with KOL_SNOW_MOCKUP=1 (no flakes);
FOCUS_PREFIX: KOL_FOCUS_DUMP of the same frame (.y .face .info);
CAMLOG: a stderr log with KOL_RAIN_CAM=1 (the perspective's camera).
Flakes live in the world as the rain's drops do (tools/mockup/rain.py): a
place over the map, falling slowly from HMAX above its ground and swaying
on a slow cycle of its own, the whole snow drifting with the wind; soft
round flakes, larger and faster near (the perspective's own scale). One
that lands where its place is seen rests there and melts.
"""
import array
import math
import random
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from png import write_png  # noqa: E402
from rain import read_bmp, read_cam, Proj  # noqa: E402

VARIANTS = {
    # flakes (in the world box), fall per frame, radius range (px at scale
    # 1), alpha, sway (world units), wind drift (X units a frame)
    "a": dict(n=180, v=1.0, r=(0.6, 1.3), a=0.85, sway=8.0, drift=0.25),
    "b": dict(n=350, v=1.0, r=(0.6, 1.3), a=0.85, sway=8.0, drift=0.25),
    "c": dict(n=650, v=1.1, r=(0.6, 1.3), a=0.80, sway=8.0, drift=0.30),
    "big": dict(n=160, v=0.9, r=(1.1, 2.3), a=0.80, sway=10.0, drift=0.25),
    # The game's levels (weather block +6; the user picked "a" on level 2's
    # state, 0097): 0 five slow flakes, no wind; 1 ten, some faster; 2 a
    # blizzard, falling 1.6-3 px a frame and blown sideways 2-3 (`wind`:
    # screen pixels a frame to the right, along X and against Z).
    "l0": dict(n=90, v=0.7, r=(0.6, 1.3), a=0.85, sway=8.0, drift=0.0),
    # l1: the user asked for 1.5 and a bit more drift (on "a")
    "l1": dict(n=180, v=1.5, r=(0.6, 1.3), a=0.85, sway=8.0, drift=0.0, wind=0.35),
    "l2": dict(n=260, v=3.5, r=(0.6, 1.3), a=0.85, sway=5.0, drift=0.0, wind=2.2),
}
HMAX = 300.0     # world units a flake falls from (above its ground)
MELT = 30        # frames a landed flake takes to melt
RGB = (226, 248, 244)   # near the game's own flakes (214, 247, 239)


def disc(img, w, h, x, y, r, a):
    """A soft round flake: full inside r - 0.5, nothing past r + 0.5."""
    x0, x1 = int(math.floor(x - r - 1)), int(math.ceil(x + r + 1))
    y0, y1 = int(math.floor(y - r - 1)), int(math.ceil(y + r + 1))
    for py in range(max(0, y0), min(h, y1 + 1)):
        for px in range(max(0, x0), min(w, x1 + 1)):
            dist = math.hypot(px + 0.5 - x, py + 0.5 - y)
            k = min(1.0, max(0.0, r + 0.5 - dist))
            if r < 1.0:
                k *= r   # a small flake is fainter, not a hard pixel
            if k <= 0.0:
                continue
            k *= a
            o = (py * w + px) * 3
            for c in range(3):
                img[o + c] = img[o + c] * (1 - k) + RGB[c] * k


def main():
    pres, focus, camlog, out = sys.argv[1:5]
    variant = sys.argv[5] if len(sys.argv) > 5 else "b"
    frames = int(sys.argv[6]) if len(sys.argv) > 6 else 1
    V = VARIANTS[variant]
    W, H, base = read_bmp(pres)
    proj = Proj(read_cam(camlog))
    info = open(focus + ".info").read().split()
    sw, sh = int(info[1]), int(info[3])
    wy = array.array("i"); wy.frombytes(open(focus + ".y", "rb").read())
    face = open(focus + ".face", "rb").read()
    c = proj.c
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
        col = int(round(c["ox"] + c["kx"] + (X - Z) / 2.0))
        row = int(round(c["base"] + (X + Z) / 4.0 - Y / 2.0))
        if not (0 <= col < sw and 0 <= row < sh):
            return False
        k = row * sw + col
        return face[k] == 0 and abs(wy[k] - Y) <= 6

    wind = V.get("wind", 0.0)
    travel = wind * HMAX / V["v"] * 1.3   # how far upwind the slowest starts
    rnd = random.Random(11)
    flakes = []   # (h0, speed, radius, sway phase, sway period, seed)
    for i in range(V["n"]):
        flakes.append((rnd.uniform(0.0, HMAX), rnd.uniform(0.75, 1.25), rnd.uniform(*V["r"]),
                       rnd.uniform(0.0, 6.283), rnd.uniform(110.0, 230.0), i))

    def place(i, fall):
        """A flake's place for fall `fall`: anywhere it is seen from."""
        r = random.Random(i * 7919 + fall * 104729)
        for _ in range(40):
            X = r.uniform(-300.0, 800.0)
            Z = r.uniform(-200.0, 900.0)
            g, known = ground_at(X, Z)
            sx, sy, s = proj(X, g, Z)
            top = proj(X, g + HMAX, Z)[1]
            if -20 < sx < W + 20 + travel and sy > -10 and top < c["view"][3] + 10:
                return X, Z, g, known and seen(X, g, Z)
        return X, Z, g, False

    # The dump may hold the letterbox round the view: the view's rows only.
    top_off = max(0, (H - c["view"][3]) // 2)
    vy0, vy1 = top_off, top_off + c["view"][3]

    def flake(img, x, y, r, a):
        if vy0 - 4 < y + top_off < vy1 + 4:
            disc(img, W, H, x, y + top_off, r, a)

    cache = {}
    for f in range(frames):
        img = [float(v) for v in base]
        in_view = 0
        for (h0, sp, rad, ph, per, i) in flakes:
            v = V["v"] * sp
            p = h0 - v * f
            fall = int(math.floor(p / HMAX))
            alt = p - fall * HMAX
            key = (i, fall)
            if key not in cache:
                cache[key] = place(i, fall)
            X, Z, g, landing_seen = cache[key]
            # Sway about its line down, and the wind's drift since it set off.
            t = (HMAX - alt) / v
            swx = V["sway"] * math.sin(ph + 6.283 * f / per)
            swz = 0.6 * V["sway"] * math.sin(ph * 1.7 + 6.283 * f / (per * 1.3))
            dx = V["drift"] * t
            up = wind * (HMAX - (HMAX - alt)) / v   # still to blow before it lands
            sx, sy, s = proj(X + swx + dx - V["drift"] * HMAX / v - up, g + alt, Z + swz + up)
            if -4 < sx < W + 4:
                flake(img, sx, sy, rad * s, V["a"] * min(1.0, s))
                if 0 <= sx < W and 0 <= sy < c["view"][3]:
                    in_view += 1
            # The fall before's flake, landed (HMAX - alt) / v frames ago:
            # resting where it landed, melting.
            age = (HMAX - alt) / v
            if age < MELT:
                pk = (i, fall + 1)
                if pk not in cache:
                    cache[pk] = place(i, fall + 1)
                LX, LZ, lg, lseen = cache[pk]
                if lseen:
                    lswx = V["sway"] * math.sin(ph + 6.283 * (f - age) / per)
                    lswz = 0.6 * V["sway"] * math.sin(ph * 1.7 + 6.283 * (f - age) / (per * 1.3))
                    gx, gy, gs = proj(LX + lswx, lg, LZ + lswz)
                    flake(img, gx, gy, rad * gs * 0.9, V["a"] * (1.0 - age / MELT) * min(1.0, gs))
        for k in list(range(0, vy0 * W * 3)) + list(range(vy1 * W * 3, H * W * 3)):   # the letterbox stays black
            img[k] = base[k]
        if os.environ.get("SNOW_COUNT"):
            print("frame %d: %d flakes in the view" % (f, in_view))
        name = "%s_%s.png" % (out, variant) if frames == 1 else "%s_%s_%03d.png" % (out, variant, f)
        write_png(name, W, H, bytes(int(min(255.0, v) + 0.5) for v in img))
    print("%d flakes" % len(flakes))


if __name__ == "__main__":
    main()
