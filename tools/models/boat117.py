#!/usr/bin/env python3
"""Map 117: the boat of map 90 with its sail SET, beside the end of a plank
pier, sky behind it (the sky is the backdrop; the only water drawn is a ring of
ripples at the hull's waterline and small rings at the pier's feet).

    tools/models/boat117.py   writes src/kol_model_map117.inc
    build_parts(m)            the whole scene into a kit Model

Everything is measured on a flat mosaic of the map (260x350), whose projection is
    sx = (X - Z)/2 + 158,   sy = (X + Z)/4 - Y/2 + 85.
The art's own pixels are quoted as (x, y) of that image. A pixel shows what
covers its CORNER, so an outline is fitted half a pixel outside the last art
pixel: a left edge at x - 0.5, a right edge at x + 0.5, a top at y - 0.5, a
bottom at y + 0.5.

Depth comes from contact:
  * the boat: boat90.build_boat(m, at=(0, 32, 320)): map 90's boat, 32 up and
    320 along Z: waterline Y 32, deck Y 80, the yard at Y 378, Z 409, X 215..500;
  * the pier: map 90's pier, pixel-identical at the offset map 117 (x, y) = map 90
    (x + 90, y - 30): moved by (32, 32, 416): deck top Y 96, water Y 32;
  * the sail: see the comments at SAIL, below.
"""
import math
import os
import sys


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boat90 as boat  # noqa: E402
Model = boat.Model
SHEET, CLOTH, THIN = boat.SHEET, boat.CLOTH, boat.THIN
# As the boat's own hanging parts: the kit's BG1 (with OVER in the installed
# boat90: lifted, a part opens onto the hull behind it; a rope painted on BG0
# over a solid stays on the solid, and only over the sky at its own depth).
HANG = boat.BG1

KX, KY = 158.0, 85.0
BOAT_AT = (0.0, 32.0, 320.0)
WATER = 32.0
DECK_Y = 80.0        # the boat's deck, 48 + 32
RAIL_Y = 96.3        # the boat's rail amidships, 64.3 + 32


def pt(sx, sy, y):
    """The point at height y on the line of sight of pixel (sx, sy)."""
    d = 2.0 * (sx - KX)              # X - Z
    s = 4.0 * (sy + y / 2.0 - KY)    # X + Z
    return ((s + d) / 2.0, y, (s - d) / 2.0)


def pz(sx, sy, z):
    """The point at depth z on the line of sight of pixel (sx, sy)."""
    x = z + 2.0 * (sx - KX)
    return (x, 2.0 * ((x + z) / 4.0 + KY - sy), z)


def scr(p):
    return ((p[0] - p[2]) / 2.0 + KX, (p[0] + p[2]) / 4.0 - p[1] / 2.0 + KY)


def _lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def _poly_param(pts, measure):
    """Cumulative, normalised parameter along a polyline under measure(a, b)."""
    c = [0.0]
    for a, b in zip(pts, pts[1:]):
        c.append(c[-1] + measure(a, b))
    return [v / c[-1] for v in c]


def _poly_at(pts, c, t):
    t = min(max(t, 0.0), 1.0)
    for i in range(len(c) - 1):
        if t <= c[i + 1] or i == len(c) - 2:
            f = (t - c[i]) / (c[i + 1] - c[i]) if c[i + 1] > c[i] else 0.0
            return _lerp(pts[i], pts[i + 1], f)
    return pts[-1]


_SCREEN = lambda a, b: math.hypot(b[0] - a[0], b[1] - a[1])      # noqa: E731


# ---------------------------------------------------------------------------
# SAIL. A square sail seen from aft: the camera looks into its CONCAVE side.
#  * It is behind the yard and the mast (they are drawn over it), so at the mast's
#    column it has Z < 409: it bellies toward -Z, the bow, as wind from astern would.
#  * The shading reads the same way: dark ('+' (82,132,107)) under the yard, where
#    the concave side faces down and aft, cream at the foot where it faces up and
#    aft; the far half lit and the near half dark at one row, since the concave side
#    faces +X on the far half and -X on the near half and the art is lit from +X.
#  * The cream strip down the right side is the CONVEX side: the near leech is the
#    (66,66,90) line (LEECH_NEAR); right of it the cloth runs back forward and
#    round to a fold, the silhouette (FOLD), and the belly behind it is hidden by
#    it. So the art's right edge is not the sail's edge: it is a fold.
# Edge points are boundaries of art pixels, on the rules at the top of this file.

# Far leech, the silhouette; its top takes the rope's dim right outline, x 64 rows 56-60. (Its first
# pixels per row: 65 at rows 58-60, 66 at 61, 70 at
# 66, 80 at 85, 85 at 98, 90 at 114, 93 at 126, 94 at 134, a rope over 135-137, 96 at 138,
# 97 at 142-150, 98 at 151-161, 99 at 162-183, 98 at 184-191).
LEECH_FAR = [(63.7, 55.5), (63.7, 61.0), (65.5, 61.0), (72.5, 71.5), (79.5, 85.5), (84.5, 98.0), (89.5, 114.5), (92.5, 126.5), (93.5, 134.0),
             (95.5, 139.5), (96.5, 146.0), (97.5, 156.0), (98.5, 172.5), (98.5, 182.0), (97.5, 187.0), (97.6, 191.5)]
# Near leech: the (66,66,90) line: x 201 rows 130-133, 200 at 134-140, 198 at 146-150, 196 at
# 154-158, 193 at 166-169, 190 at 176, 187 at 184, 184 at 190, 180 at 199, 176 at 206, 174 at
# 210-211. Its top, rows 125-129, is under the rope from the yard's near tip.
LEECH_NEAR = [(204.0, 125.0), (202.6, 127.5), (200.5, 131.5), (197.5, 148.0), (194.5, 160.5), (190.5, 174.0), (186.5, 184.5),
              (181.5, 195.5), (173.5, 210.5), (174.5, 213.0)]
# The fold: the last pixel per row before the sky or the hull: 205 at 125-128, 206 at 129-131,
# 207 at 132-137, 208 at 138-151, 207 at 152-157, 206 at 158-162, 205 at 163-166, 204 at 167-169,
# 203 at 170, 200 at 176, 197 at 182, 195 at 186, 192 at 190, 188 at 196, 185 at 200, 181 at 205,
# 177 at 211.
FOLD = [(204.5, 124.0), (205.5, 127.0), (206.5, 130.5), (207.5, 134.5), (208.5, 144.5), (207.5, 154.5), (204.5, 168.0),
        (195.5, 186.5), (190.5, 194.0), (177.5, 211.0), (174.5, 213.0)]
# The foot, far clew to near clew: its last row per column (x 101-102: 191, 103-105: 190,
# 106-108: 189, 109-112: 188, 113-117: 187, 118-125: 186, the mast, 134-136: 185, 137-141: 186,
# 142-144: 187, 145-147: 188, 148-150: 189, 151-153: 190, 154-155: 191, 156-157: 192,
# 158-159: 193, 160-161: 194, 162: 195, 163: 196, 164: 197, 165: 198, 166: 199, 167: 200,
# 168: 201) and, where steep, its first column per row (169 at 202-203, 170 at 204-205,
# 171 at 206-208, 172 at 209-211, 173 at 212).
FOOT = [(97.6, 191.5), (101.5, 191.5), (104.0, 190.5), (107.0, 189.5), (110.5, 188.5), (115.0, 187.5), (121.5, 186.5),
        (128.0, 186.0), (135.0, 185.5), (139.0, 186.5), (143.0, 187.5), (146.0, 188.5), (149.0, 189.5), (152.0, 190.5),
        (154.5, 191.5), (156.5, 192.5), (158.5, 193.5), (160.5, 194.5), (162.0, 195.5), (163.5, 196.5), (165.0, 198.5),
        (168.0, 201.5), (168.5, 202.5), (169.5, 204.5), (170.5, 207.0), (171.5, 210.0), (172.5, 212.2), (174.5, 213.0)]
# The head, the art's top row per column over the yard: four scallops, the cloth
# gathered up behind the yard. (x, boundary): 69-81 row 53, 82-84: 54, 85-86: 55, 87-88: 56,
# 89-90: 57, 91: 58, 92: 59, 93: 60, 94: 61, 95: 62, 96: 64; the yard over it at x 97-101;
# 102-110: 69, 111-114: 70, 115-120: 71, 121-123: 72, 124-125: 73, 126: 74; behind the mast it
# comes out at x 133, rows 82-86; the yard at x 134-136; 137-140: 88, 141-149: 87, 150-153: 88,
# 154-156: 89, 157-158: 90, 159: 91, a rope over x 160-163, 164-165: 94, 166: 95, 167: 96, 168: 97,
# 169: 99, 170: 103, 171-173: 104, 174-180: 105, 181-187: 106, 188-189: 107, a rope over x 190-192,
# 193-194: 109, 195-196: 110, 197: 111, 198: 112, 199: 113, 200: 114, 201: 115, 202: 117, 203: 120.
HEAD = [(63.7, 55.5), (66.5, 54.0), (68.5, 52.5), (75.0, 52.5), (81.5, 52.5), (83.0, 53.5), (85.5, 54.5), (87.5, 55.5), (89.5, 56.5),
        (91.0, 57.5), (92.0, 58.5), (93.0, 59.5), (94.0, 60.5), (95.2, 62.0), (96.5, 64.5), (97.2, 67.0), (99.0, 69.0), (101.5, 68.6),
        (106.0, 68.5), (110.5, 68.5), (112.5, 69.5), (117.5, 70.5), (122.0, 71.5), (124.5, 72.5), (126.5, 73.5), (130.0, 77.0),
        (134.5, 81.5), (135.0, 86.0), (136.5, 87.5), (140.5, 87.5), (141.5, 86.5), (149.5, 86.5), (151.5, 87.5), (155.0, 88.5),
        (157.5, 89.5), (159.0, 90.5), (162.0, 92.0), (164.5, 93.5), (166.0, 94.5), (167.0, 95.5), (168.0, 96.5), (169.5, 98.5),
        (170.0, 102.5), (172.0, 103.5), (177.0, 104.5), (184.0, 105.5), (188.5, 106.5), (191.5, 107.5), (193.5, 108.5),
        (195.5, 109.5), (197.0, 110.5), (198.0, 111.5), (199.0, 112.5), (200.0, 113.5), (201.3, 114.5), (202.3, 116.5),
        (203.3, 119.5), (204.5, 121.5), (204.0, 125.0)]

# The head's sharp corners get a column each: the notches between the scallops.
HEAD_CORNERS = [(68.5, 52.5), (81.5, 52.5), (96.5, 64.5), (97.2, 67.0), (101.5, 68.6), (134.5, 81.5), (135.0, 86.0), (140.5, 87.5), (141.5, 86.5),
                (169.5, 98.5), (170.0, 102.5), (172.0, 103.5), (203.3, 119.5), (204.5, 121.5)]

# The head lies this far behind the yard's axis (Z 409, radius 3), so the yard and the
# lifts in its plane are in front of it, as drawn; Y and X from each pixel on that plane.
HEAD_Z = 404.0
# THE CLEWS' HEIGHT. A clew's pixel leaves one line of sight: the far clew (97.6, 191.5) has
# X = 152.6 + Y, Z = 273.4 + Y; the near clew (174.5, 213) X = 272.5 + Y, Z = 239.5 + Y. Solved from
# what the boat gives: at Y 141.5 they stand either side of the keel (X 294 and 414, mean 354; keel
# 352, the yard's middle 357.5), right over the rails' inner edges (X 290, 414), 61 over the deck;
# each sheet then runs down and aft in its own X plane at 40-47 degrees: far (294,141,414) to
# (287,80,471), near (414,141,381) to (414,96,436); and the two leeches come out the same length,
# 246. Much lower, the near sheet goes flat while the far one stays steep and the leeches part.
# THE FOOT, a free edge: its whole length level at that height. Read so, the drawn curve is a
# smooth plan arc: Z 415 at the far clew, 382 at x 120, 359 at x 150-165, 381 at the near clew: 38 forward
# of its chord. From x 134 on it has the far bulwark and the deck behind it, and a moved camera
# opens what a jump hides, so its depth was weighed: along the ray it stands 40-62 in front of them
# (bulwark at x 145: 40, x 165: 57; deck at x 168: 62) and 43 behind the mast; a lower foot only
# trades the first for the second.
FOOT_Y = 141.5


def _ss(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def foot_y(x):
    """The foot's height at screen column x. Level at FOOT_Y it read as a smooth
    arc, but nothing fixes it there, and each unit over the deck is stretch with
    the camera moved (the curtains under it): it falls 30 toward the near clew
    and rises 15 by the mast, keeping its pixels at rest."""
    return FOOT_Y - 30.0 * _ss((x - 134.0) / 28.0) + 15.0 * _ss((x - 98.0) / 30.0) * (1.0 if x <= 128.0 else _ss((134.0 - x) / 6.0))
# The leeches bow this far forward at mid height.
LEECH_BOW = 8.0
# THE BELLY: no contact gives its depth, the fold does: the cloth must pass the leech's X - Z by
# E = 2 (fold column - leech column), 24 at rows 150-165, where the chord closes 2 of X - Z per 1% of s:
# so at 10% in from the leech it must already be E + 20 = 44 forward. A section of about 50
# from a quarter of the way across to the fold is the least that reaches the drawn fold; deeper only
# adds to the jumps at the mast (now 43-53 along the ray) and at the near leech (now 25-51).
# BELLY is the part of it on top of the foot's arc carried up through the surface; it peaks
# at mid height and is gone at the head, the surface never steeper than 45 degrees forward of the
# yard: a point below the head by d rows may lie at most 2 d forward, or it projects above the head line.
BELLY = 10.0       # (30 at first: the fit does not need it; a fresh reviewer: every unit is stretch with the camera moved)
BELLY_POW = 3.0
# The ridge that makes the fold: from the leech it rises as a parabola to s_p = 1 - RIDGE_W E / (the
# row's span in X - Z), then dies away by s = RIDGE_S0, behind the strip, where no art pixel lands. Its
# height is solved per row so that the row's outline touches FOLD: 12 under the head, 22-32 below.
RIDGE_W = 0.7
RIDGE_S0 = 0.9
RIDGE_WMIN, RIDGE_WMAX = 0.012, 0.13

# Column and row parameters: columns dense toward the near leech (the fold), rows dense
# under the head.
SAIL_COLS = [i / 34.0 * 0.80 for i in range(34)] + [0.80 + 0.20 * (1.0 - (1.0 - i / 26.0) ** 1.6) for i in range(27)]
SAIL_ROWS = [i / 6.0 * 0.12 for i in range(6)] + [0.12 + i / 17.0 * 0.82 for i in range(18)] + [0.955, 0.97, 0.98, 0.99, 1.0]


def _fold_x(y):
    """The fold's boundary column at row y, or None off its ends."""
    for (x0, y0), (x1, y1) in zip(FOLD, FOLD[1:]):
        if y0 <= y <= y1:
            return x0 + (x1 - x0) * (y - y0) / (y1 - y0)
    return None


class _Sail:
    def __init__(self):
        self.head_pts = [pz(x, y, HEAD_Z) for x, y in HEAD]
        self.head_c = _poly_param(HEAD, lambda a, b: abs(b[0] - a[0]) + 0.15 * abs(b[1] - a[1]))
        # A straight head along the yard, under the scallops: the body does not carry them.
        self.h0, self.h1 = self.head_pts[0], self.head_pts[-1]
        self.foot_pts = [pt(x, y, foot_y(x)) for x, y in FOOT]
        self.foot_c = _poly_param(self.foot_pts, lambda a, b: math.hypot(b[0] - a[0], b[2] - a[2]))
        self.k0, self.k1 = self.foot_pts[0], self.foot_pts[-1]
        # Columns: the grid and the head's sharp corners (the notches between the scallops).
        self.cols = sorted(set(SAIL_COLS) | {self.head_c[HEAD.index(p)] for p in HEAD_CORNERS})
        self.lf_c = _poly_param(LEECH_FAR, _SCREEN)
        self.ln_c = _poly_param(LEECH_NEAR, _SCREEN)
        # The ridge per row, solved; rows wholly above the fold's top take the first
        # solved row's, scaled down to nothing at the head.
        solved = {}
        for v in SAIL_ROWS:
            solved[v] = self._solve_ridge(v)
        known = [v for v in SAIL_ROWS if solved[v] is not None]
        self.ridge = {}
        for v in SAIL_ROWS:
            if solved[v] is not None:
                self.ridge[v] = solved[v]
            elif v <= known[0]:
                self.ridge[v] = solved[known[0]] * (v / known[0]) if known[0] > 0.0 else 0.0
            else:
                self.ridge[v] = 0.0

    def leech(self, pts, c, z0, z1, v):
        x, y = _poly_at(pts, c, v)
        return pz(x, y, z0 + (z1 - z0) * v - LEECH_BOW * math.sin(math.pi * v))

    def far(self, v):
        return self.leech(LEECH_FAR, self.lf_c, self.h0[2], self.k0[2], v)

    def near(self, v):
        return self.leech(LEECH_NEAR, self.ln_c, self.h1[2], self.k1[2], v)

    def head(self, s):
        return _poly_at(self.head_pts, self.head_c, s)

    def head_line(self, s):
        p = self.head(s)
        t = (p[0] - self.h0[0]) / (self.h1[0] - self.h0[0])
        return (p[0], self.h0[1] + (self.h1[1] - self.h0[1]) * t, p[2])

    def foot(self, s):
        return _poly_at(self.foot_pts, self.foot_c, s)

    def base(self, s, v, ridge):
        """The Coons patch of head line, foot and leeches, the belly, the ridge, and
        the scallops fading out under the head."""
        h, k, l, r = self.head_line(s), self.foot(s), self.far(v), self.near(v)
        h0, h1, k0, k1 = self.far(0.0), self.near(0.0), self.far(1.0), self.near(1.0)
        p = [0.0, 0.0, 0.0]
        for i in range(3):
            p[i] = ((1 - v) * h[i] + v * k[i] + (1 - s) * l[i] + s * r[i]
                    - (1 - s) * (1 - v) * h0[i] - s * (1 - v) * h1[i] - (1 - s) * v * k0[i] - s * v * k1[i])
        w = abs(2.0 * s - 1.0)
        b = BELLY * math.sin(math.pi * v) * (1.0 - w ** BELLY_POW)
        sp = self.ridge_peak(v)
        if s >= sp:
            t = (s - sp) / (1.0 - sp)
            hh = 1.0 - t * t
        elif s > RIDGE_S0:
            t = (s - RIDGE_S0) / (sp - RIDGE_S0)
            hh = t * t * (3.0 - 2.0 * t)
        else:
            hh = 0.0
        p[2] -= b + ridge * hh
        vf = max(0.0, 1.0 - v / 0.14)
        if vf > 0.0:
            top = self.head(s)
            line = self.head_line(s)
            f = vf * vf
            p[1] += (top[1] - line[1]) * f
        return (p[0], p[1], p[2])

    def ridge_peak(self, v):
        """s_p: where the ridge tops, from the width of the strip at this row."""
        xn, yn = scr(self.near(v))
        f = _fold_x(yn)
        if f is None:
            return 1.0 - RIDGE_WMIN
        xf, _ = scr(self.far(v))
        span = 2.0 * (xn - xf)
        w = RIDGE_W * 2.0 * max(f - xn, 0.0) / span
        return 1.0 - min(max(w, RIDGE_WMIN), RIDGE_WMAX)

    def _excess(self, v, ridge):
        """How far the row's outline passes the fold (px), over the near part."""
        e = -1e9
        for i in range(81):
            s = 1.0 - 0.45 * (i / 80.0) ** 2
            x, y = scr(self.base(s, v, ridge))
            f = _fold_x(y)
            if f is not None:
                e = max(e, x - f)
        return e

    def _solve_ridge(self, v):
        if v <= 0.0 or v >= 1.0:
            return 0.0
        if self._excess(v, 0.0) < -1e8:
            return None        # the row lies above the fold's top
        lo, hi = -5.0, 90.0
        if self._excess(v, lo) > 0.0:
            return lo
        if self._excess(v, hi) < 0.0:
            return hi
        for _ in range(30):
            mid = (lo + hi) / 2.0
            if self._excess(v, mid) > 0.0:
                hi = mid
            else:
                lo = mid
        return (lo + hi) / 2.0

    def rows(self):
        out = []
        for v in SAIL_ROWS:
            out.append([self.base(s, v, self.ridge[v]) for s in self.cols])
        return out


# ---------------------------------------------------------------------------
# The sheets, this map's own ropes: each a straight line from its clew to its foot. Pixel
# lines (their pale core pixels, where they differ from map 90):
#   far: x = 98 - 0.756 (y - 192), rows 192 to 233, ending at (66, 234.5), the far bulwark's foot at
#   the deck's seam (map 90's "seam ... lies at Z 150 on Y 48 and starts at X 286"): Y 80, X 287, Z 471;
#   near: x = 165 - 0.727 (y - 224), rows 213 to 248, ending at (147, 249.5), on the near rail's inner
#   edge: Y 96.3 gives X 414.3, Z 436.3 (the inner edge is X 414). It tapers from the clew's 4-5 px
#   (rows 213-220) to 3 px, outlines included.
# (Its foot 4 over the deck, on the far bulwark: at the deck it ran along the wall for 12 rows.)
SHEET_FAR = ((98.2, 191.5), (66.0, 234.5, DECK_Y + 4.0), 1.5, 1.4)
SHEET_NEAR = ((174.5, 212.6), (147.0, 249.5, RAIL_Y), 2.3, 1.4)

# ---------------------------------------------------------------------------
# The pier: map 90's, pixel-identical: all but 2 of its 6628 pixels match map 90's at one of the
# pier's repeats, the offsets (26, 2), (90, -30), (-38, 34): a period is (-64, +32), 128 in Z.
# (90, -30) puts map 90's back edge (258,243)-(298,261) on this map's end (168,273)-(208,291), so
# it is THE offset: map 117 (x, y) = map 90 (x + 90, y - 30), the world moved by PIER_AT. Deck top Y 96
# on map 90's 64, the water Y 32 on its 0. So the far end is map 90's too. The stubs and bents go on with
# map 90's period; the two stubs nearest the end (Z 66, 92 in map 90's world) map 90 did not draw: its
# gangplank is there.
PIER_AT = (32.0, 32.0, 416.0)
# The far edge, read here: the art's first row per column is 356.5 - x/2 + {0, 1} where no stub stands
# (x 48-72, 100-136); map 90's X 448 + 32 took one more row, ripple pixels at x 120-170.
PIER_FAR_X = 481.5
# ... but a plank's end stands out over it, 2 rows, every 64 columns: x 105-113 rows 301-304,
# x 41-49 rows 333-336 (the next is the deck's own far corner, x 168-172 row 273).
PIER_PLANK_ENDS = ((570.0, 586.0), (698.0, 714.0))
PIER_PLANK_X = 479.5
PIER_END_Z = 960.0              # past the art's cut, row 340 (x 15 on the far edge is Z 766)
BENT_Z = (90.0, 218.0)          # map 90's: the bents above the cut: Z 506 (whole), 634 (only its uprights)
STUB_Z = (66.0, 92.0, 194.0, 220.0, 322.0, 348.0)
MOORING = ("post_1", "post_2")  # map 90's posts, in view at (156, 333) and (175, 333), cut by row 340

# ---------------------------------------------------------------------------
# The ripples, on the water plane Y 32. The hull's ring is a band whose outer edge is the last
# ripple pixel per column (x 52-204), stern to bow; its inner edge, RIPPLE_INSET rows up, lies under
# the hull, which is nearer, or over the sky at the stern, which is never the model's.
RIPPLE_EDGE = [(51.7, 294.6), (52.3, 294.6), (52.7, 295.6), (62.3, 295.6), (62.7, 294.6), (66.3, 294.6), (66.7, 293.6), (69.3, 293.6), (69.7, 292.6),
               (70.3, 292.6), (70.7, 295.6), (71.3, 295.6), (71.7, 296.6), (73.3, 296.6), (73.7, 297.6), (84.3, 297.6), (84.7, 296.6), (88.3, 296.6),
               (88.7, 295.6), (95.3, 295.6), (95.7, 296.6), (97.3, 296.6), (98.7, 298.6), (110.3, 298.6), (110.7, 297.6), (116.3, 297.6), (116.7, 296.6),
               (120.3, 296.6), (120.7, 295.6), (121.3, 295.6), (122.7, 293.6), (124.3, 293.6), (124.7, 292.6), (128.3, 292.6), (129.7, 290.6), (131.3, 290.6),
               (131.7, 289.6), (133.3, 289.6), (133.7, 288.6), (136.3, 288.6), (136.7, 286.6), (137.3, 286.6), (150.7, 279.6), (151.3, 279.6), (151.7, 280.6),
               (153.3, 280.6), (153.7, 278.6), (154.3, 278.6), (154.7, 277.6), (163.3, 274.6), (163.7, 273.6), (167.3, 273.6), (167.7, 272.6), (174.3, 272.6),
               (174.7, 271.6), (175.3, 271.6), (176.7, 269.6), (178.3, 269.6), (179.7, 267.6), (181.3, 267.6), (182.7, 265.6), (184.3, 265.6), (185.7, 263.6),
               (189.3, 263.6), (189.7, 262.6), (191.3, 262.6), (192.7, 260.6), (194.3, 260.6), (194.7, 259.6), (195.3, 259.6), (195.7, 258.6), (196.3, 258.6),
               (197.7, 256.6), (199.3, 256.6), (200.7, 254.6), (202.3, 254.6), (202.7, 252.6), (203.3, 252.6), (203.7, 251.6), (204.6, 251.6)]
RIPPLE_INSET = 14.0
# The rings at the pier's two last piles (BG1: x 182-199 rows 336-340, x 198-212 rows 330-337): the
# piles are nearer, so they stand in them.
RIPPLE_RINGS = [[(181.5, 335.5), (186.0, 335.0), (200.8, 335.0), (200.8, 339.5), (196.5, 341.0), (186.5, 341.0), (181.5, 337.5)],
                [(197.5, 329.5), (213.2, 329.5), (213.2, 333.5), (210.5, 336.0), (207.5, 337.5), (197.5, 338.5)]]


# ---------------------------------------------------------------------------
# The boat's own art pixels the shared build_boat leaves (rope outlines of (66,66,90), too light for
# the outline rule, and a few core pixels), each at its nearest part's depth (x, y, Y): over this map's
# sky they fall away as stray dots when the camera moves. A patch per pixel, so that they stay.
BOAT_LEFTOVERS = [(125, 30, 489.4), (127, 40, 476.5), (126, 43, 471.8), (126, 44, 468.9), (69, 51, 391.4), (67, 52, 387.2), (242, 150, 136.1), (231, 158, 130.8), (218, 168, 122.3), (215, 202, 114.6), (27, 224, 142.0), (39, 264, 91.8), (40, 265, 89.5)]


def _pier(m):
    ax, ay, az = PIER_AT

    def P(x, y, z):
        return (x + ax, y + ay, z + az)
    (x0, y0, z0), (x1, y1, _) = boat.PIER_BOX
    m.box("pier_deck", (PIER_FAR_X, y0 + ay, z0 + az), (x1 + ax, y1 + ay, PIER_END_Z), HANG)
    for za, zb in PIER_PLANK_ENDS:
        m.box("pier_deck", (PIER_PLANK_X, y0 + ay, za), (PIER_FAR_X + 1.0, y1 + ay, zb), HANG)
    for z in BENT_Z:
        for x, dz, ux, udz in ((539.0, -2.5, 537.0, -1.0), (540.0, -27.5, 538.0, -26.0)):
            zz = z + dz
            m.cyl("pier_pile", P(x, 0.0, zz), P(x, boat.PILE_TOP, zz), boat.PILE_R, sides=10, flags=HANG)
            m.cyl("pier_lashing", P(x, boat.LASH_Y[0], zz), P(x, boat.LASH_Y[1], zz), boat.LASH_R, sides=10, flags=HANG)
            m.cyl("pier_upright", P(ux, boat.PILE_TOP - 6.0, z + udz), P(ux, boat.UPRIGHT_TOP, z + udz), boat.UPRIGHT_R, sides=10, flags=HANG)
        m.cyl("pier_lashing", P(538.0, 32.0, z - 2.0), P(538.0, 44.0, z - 2.0), 6.6, sides=10, flags=HANG)
        m.cyl("pier_brace", P(540.0, 38.0, z + 7.0), P(540.0, 38.0, z - 24.0), 4.0, sides=6, flags=HANG)
        m.box("pier_beam", P(527.0, 42.0, z - 25.0), P(555.0, 53.0, z - 13.0), HANG)
        m.box("pier_bracket", P(527.0, 47.5, z - 11.0), P(540.0, 52.0, z - 0.5), HANG)
    for z in STUB_Z:
        m.cyl("pier_stub", P(boat.STUB_X, 0.0, z), P(boat.STUB_X, boat.STUB_TOP, z), boat.STUB_R, sides=10, flags=HANG)
    for z in STUB_Z[1::2]:
        m.box("pier_stub", P(448.0, 60.0, z), P(452.0, 68.0, z + 10.0), HANG)
    for name, fc, fr, tc, tr, r0, r1 in boat.MOORING:
        if name not in MOORING:
            continue
        row0 = fr + 0.5 - 0.354 * r0
        rowt = tr + 0.5 + 0.354 * r1
        foot = boat.pt(fc, row0, 0.0)
        s_ = foot[0] + foot[2]
        top_d = 2.0 * (tc - boat.KX)
        h = 2.0 * (row0 - rowt)
        m.cyl(name, P(*foot), P((s_ + top_d) / 2.0, h, (s_ - top_d) / 2.0), r0, r1=r1, sides=10, flags=HANG)


def _up_poly(m, name, pts):
    """A flat polygon on the water, its normal up."""
    q = [pt(x, y, WATER) for x, y in pts]
    a, b, c = q[0], q[1], q[2]
    ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2])
    if ny < 0.0:
        q = q[::-1]
    m.poly(name, q, 0)


def build_parts(m):
    """Map 117's scene: the boat, then what is this map's own."""
    boat.build_boat(m, at=BOAT_AT, body=0)

    sail = _Sail()
    # The sail's strip. Its corner normals point forward, the convex side's; the game turns a
    # sheet's normal to the side seen: the concave side on the body, the convex on the strip past the fold.
    rows = sail.rows()
    first = len(m.verts)
    m.strip("sail", rows, CLOTH | SHEET | boat._kit.ASDRAWN)
    for i in range(first, len(m.verts)):
        v = m.verts[i]
        if v[3] * v[3] + v[4] * v[4] + v[5] * v[5] < 0.5:
            m.verts[i] = (v[0], v[1], v[2], 0.0, 0.0, 1.0)

    # The sheets.
    for name, (clew, foot, r0, r1) in (("rope_sheet_far", SHEET_FAR), ("rope_sheet_near", SHEET_NEAR)):
        m.cyl(name, pt(clew[0], clew[1], foot_y(clew[0])), pt(*foot), r0, r1=r1, sides=6, flags=HANG | THIN | boat._kit.ASDRAWN)

    _pier(m)

    # The ripples.
    outer = [pt(x, y, WATER) for x, y in RIPPLE_EDGE]
    inner = [pt(x, y - RIPPLE_INSET, WATER) for x, y in RIPPLE_EDGE]
    first = len(m.verts)
    m.strip("ripples", [outer, inner], 0)
    for i in range(first, len(m.verts)):
        v = m.verts[i]
        m.verts[i] = (v[0], v[1], v[2], 0.0, 1.0, 0.0)
    for ring in RIPPLE_RINGS:
        _up_poly(m, "ripples", ring)

    # The boat's leftover pixels: a small upright square each, facing the camera's side.
    for x, y, h in BOAT_LEFTOVERS:
        s = 4.0 * (y - KY) + 2.0 * h
        q = []
        for dx, dy in ((-0.45, -0.45), (0.45, -0.45), (0.45, 0.45), (-0.45, 0.45)):
            d = 2.0 * (x + dx - KX)
            q.append(((s + d) / 2.0, 2.0 * (s / 4.0 + KY - (y + dy)), (s - d) / 2.0))
        m.poly("boat_leftovers", q, HANG | THIN | SHEET | boat._kit.ASDRAWN)


if __name__ == "__main__":
    m = Model("map117")
    build_parts(m)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map117.inc")
    m.write_inc(out, 117, "tools/models/boat117.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
