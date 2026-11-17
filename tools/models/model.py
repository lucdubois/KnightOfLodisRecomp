"""Small geometry kit for KoL map models (see ship97.py).

World units as kol_focus uses them: X and Z across the map (32 a cell), Y up
(16 a height step). The game's projection: sx = (X - Z) / 2 + kx,
sy = (X + Z) / 4 - Y / 2 + ky. A pixel's ray is (cX + t, t, cZ + t): the
larger Y along it, the nearer the camera.

A model is triangles with a normal at each corner (smooth cylinders) and a
part number. Flags per triangle: SHEET (both sides, cloth
or a thin board), CLOTH (sails), BG1 (hangs over the ground: lifted where
its art is on the layer over the ground's; a pixel is the nearest part's on
either layer),
ROPES (the rigging's place: only its BG1 pixels take it, after any other),
LEE (an inner face: turned away from the sun it is in shade, as the
terrain's walls are; without it such a point is left as the art drew it),
THIN (a rope modelled as a part: a gap it closes is a rope's, never more
than a few pixels),
NOGAP (the art drew the ground round it on the same layer, in whole tiles:
map 58's gangplank among the rock's BG1; a gap it bounds is the ground's,
not the part's),
BG1ART (its art is on BG1 alone: a BG0 pixel it covers is the ground's,
seen past it; map 58's hull, whose bow bulged over the water),
TOP (a top surface however it slopes: the ships' rail cap, which the art
draws as the rail's light top where it climbs to the castle and rounds the
nose, steeper there than kol_focus's 45 degrees for a top),
OVER (it hangs in front of the model's solid parts, the parts without this
flag: its pixel lifted on BG1 opens onto the solid behind it, not onto the
terrain; a rope, THIN, is lifted off whatever part it passes in front of, on
either layer, so it is in the air to its ends; map 90's boat).
ACTOR (the art is a scene actor's sprite, not the map's: it takes only that
sprite's pixels, and those pixels lift off what the sprite covers; it still
casts and stops light; map 73's fourth column, kol_focus kSceneObjects).
"""
import math

SHEET = 1
CLOTH = 2
BG1 = 4
ROPES = 8   # a sheet the rigging's pixels (BG1) take where no other part is
LEE = 16    # in shade when turned away from the sun (the ship's inner faces)
THIN = 32   # a rope as a part (kol_focus: the gaps it bounds are small)
NOGAP = 64  # gaps it bounds stay the ground's (kol_focus kModelNoGap)
BG1ART = 128  # takes only the art's BG1 pixels (kol_focus kModelBg1Art)
TOP = 256     # a top surface, however it slopes (kol_focus kModelTop)
LIGHTPASS = 512  # light passes it: not in the light's blockers (kol_focus kModelLightPass; a railing)
NOSHADOW = 1024  # casts no light's shadow at all: not in the blockers (kol_focus kModelNoShadow; a flame)
ASDRAWN = 4096  # left as the art drew it: takes no shadow, its own or another part's; still casts (kol_focus kModelAsDrawn; map 117's sail, which the art lights itself)
ACTOR = 8192  # its art is a scene actor's sprite: takes only that sprite's pixels (kol_focus kModelActor; map 73's fourth column)
FXART = 16384  # the art's own flame: with Special Effects on it lifts and is not drawn, the wall on BG0 behind it showing, the effect's flame (kMapFlames) in its place (kol_focus kModelFxArt; map 66's)
BG0ART = 32768  # takes only the art's BG0 pixels: a BG1 pixel it covers is the terrain's (kol_focus kModelBg0Art; map 79's slope)
OVER = 2048   # hangs in front of the model's solid parts (kol_focus kModelOver): lifted on BG1, it opens onto the solid behind it; a rope (THIN) is lifted off any part it passes in front of


def norm(v):
    l = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) or 1.0
    return (v[0] / l, v[1] / l, v[2] / l)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


class Model:
    def __init__(self, name):
        self.name = name
        self.verts = []     # (x, y, z, nx, ny, nz)
        self.tris = []      # (a, b, c, part, flags)
        self.parts = []     # names
        self.guides = []    # (p0, p1, colour): fitting lines only, not exported

    def part(self, name):
        if name not in self.parts:
            self.parts.append(name)
        return self.parts.index(name)

    def vert(self, p, n):
        self.verts.append((p[0], p[1], p[2], n[0], n[1], n[2]))
        return len(self.verts) - 1

    def poly(self, name, pts, flags=0):
        """A flat polygon (convex, corners in order), one normal."""
        part = self.part(name)
        n = norm(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])))
        # Face the camera's side (+X +Y +Z) for a sheet, so a sheet's normal
        # is the side seen; a solid's polygons keep their winding (outward).
        if flags & SHEET and dot(n, (1, 1, 1)) < 0:
            n = mul(n, -1)
        idx = [self.vert(p, n) for p in pts]
        for i in range(1, len(pts) - 1):
            self.tris.append((idx[0], idx[i], idx[i + 1], part, flags))

    def strip(self, name, rows, flags=0, smooth=True):
        """A surface from rows of points (each row the same length): quads
        between neighbouring rows; normals averaged at the corners. The
        normal is (along the columns) x (along the rows): rows given bottom
        to top around a plan that runs far side, stern, near side face
        outward; given top to bottom (the bulwarks' inner faces) inward."""
        part = self.part(name)
        R, C = len(rows), len(rows[0])
        def corner_n(i, j):
            a = rows[min(i + 1, R - 1)][j]
            b = rows[max(i - 1, 0)][j]
            c = rows[i][min(j + 1, C - 1)]
            d = rows[i][max(j - 1, 0)]
            return norm(cross(sub(c, d), sub(a, b)))
        idx = [[self.vert(rows[i][j], corner_n(i, j)) for j in range(C)] for i in range(R)]
        def flat(a, b, c):   # no area (rows clamped together): nothing to draw
            p, q, r = (self.verts[k][:3] for k in (a, b, c))
            n = cross(sub(q, p), sub(r, p))
            return dot(n, n) < 1e-6
        for i in range(R - 1):
            for j in range(C - 1):
                a, b, c, d = idx[i][j], idx[i][j + 1], idx[i + 1][j + 1], idx[i + 1][j]
                for t in ((a, b, c), (a, c, d)):
                    if not flat(*t):
                        self.tris.append(t + (part, flags))

    def cyl(self, name, p0, p1, r, sides=12, caps=True, flags=0, r1=None):
        """A cylinder (a cone with r1) from p0 to p1."""
        part = self.part(name)
        r1 = r if r1 is None else r1
        axis = norm(sub(p1, p0))
        ref = (0, 1, 0) if abs(axis[1]) < 0.9 else (1, 0, 0)
        u = norm(cross(axis, ref))
        v = cross(axis, u)
        ring0, ring1 = [], []
        for i in range(sides):
            a = 2 * math.pi * i / sides
            d = add(mul(u, math.cos(a)), mul(v, math.sin(a)))
            ring0.append(self.vert(add(p0, mul(d, r)), d))
            ring1.append(self.vert(add(p1, mul(d, r1)), d))
        for i in range(sides):
            j = (i + 1) % sides
            self.tris.append((ring0[i], ring0[j], ring1[j], part, flags))
            self.tris.append((ring0[i], ring1[j], ring1[i], part, flags))
        if caps:
            for (p, rr, s) in ((p0, r, -1), (p1, r1, 1)):
                n = mul(axis, s)
                c = self.vert(p, n)
                ring = []
                for i in range(sides):
                    a = 2 * math.pi * i / sides
                    d = add(mul(u, math.cos(a)), mul(v, math.sin(a)))
                    ring.append(self.vert(add(p, mul(d, rr)), n))
                for i in range(sides):
                    self.tris.append((c, ring[i], ring[(i + 1) % sides], part, flags))

    def box(self, name, lo, hi, flags=0):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        P = lambda x, y, z: (x, y, z)
        self.poly(name, [P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1), P(x1, y1, z0)], flags)  # top
        self.poly(name, [P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)], flags)  # +X
        self.poly(name, [P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)], flags)  # +Z
        self.poly(name, [P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1), P(x0, y1, z0)], flags)  # -X
        self.poly(name, [P(x0, y0, z0), P(x0, y1, z0), P(x1, y1, z0), P(x1, y0, z0)], flags)  # -Z

    def guide(self, pts, colour=(255, 0, 255), closed=False):
        for i in range(len(pts) - (0 if closed else 1)):
            self.guides.append((pts[i], pts[(i + 1) % len(pts)], colour))

    # The C++ table (kol_focus's kModels).
    def write_inc(self, path, map_no, source):
        with open(path, "w") as o:
            o.write("// Generated by %s: do not edit, change the script and run it again.\n" % source)
            o.write("// Map %d, %s: %d vertices, %d triangles; parts: %s.\n" % (
                map_no, self.name, len(self.verts), len(self.tris), ", ".join(self.parts)))
            o.write("static const ModelVertex kModel%dVerts[] = {\n" % map_no)
            for v in self.verts:
                o.write("    {%.2ff, %.2ff, %.2ff, %.4ff, %.4ff, %.4ff},\n" % v)
            o.write("};\nstatic const ModelTri kModel%dTris[] = {\n" % map_no)
            for t in self.tris:
                o.write("    {%d, %d, %d, %d, %d},\n" % t)
            o.write("};\n")
            o.write("static const MapModel kModel%d = {%d, kModel%dVerts, %d, kModel%dTris, %d};\n" % (
                map_no, map_no, map_no, len(self.verts), map_no, len(self.tris)))


def overlay(model, png, info, out, crop=None, scale=4, alpha=0.45, wire=True, guides_only=False):
    """The model over a flat frame: each part a colour, nearest drawn last."""
    from PIL import Image, ImageDraw
    p = {}
    for l in open(info):
        a = l.split()
        if a[0] == 'w':
            for i in range(0, len(a), 2):
                p[a[i]] = int(a[i + 1])
    kx, ky = p['kx'] + p['ox'], p['ky'] + p['oy']
    x0, y0, x1, y1 = crop or (0, 0, p['w'], p['h'])

    def proj(v):
        return (((v[0] - v[2]) / 2 + kx - x0) * scale, ((v[0] + v[2]) / 4 - v[1] / 2 + ky - y0) * scale)
    base = Image.open(png).convert('RGB').crop((x0, y0, x1, y1))
    base = base.resize((base.width * scale, base.height * scale), Image.NEAREST)
    lay = Image.new('RGBA', base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    pal = [(255, 60, 60), (60, 255, 60), (60, 120, 255), (255, 255, 60), (255, 60, 255), (60, 255, 255),
           (255, 160, 40), (160, 80, 255), (255, 255, 255), (120, 255, 160), (255, 120, 160), (160, 160, 60)]
    tris = sorted(model.tris, key=lambda t: sum(model.verts[i][0] + model.verts[i][1] + model.verts[i][2]
                                                  for i in t[:3]))
    A = int(255 * alpha)
    if guides_only:
        tris = []
        for (p0, p1, c) in model.guides:
            d.line([proj(p0), proj(p1)], fill=c + (255,), width=1)
    for t in tris:
        pts = [proj(model.verts[i]) for i in t[:3]]
        c = pal[t[3] % len(pal)]
        d.polygon(pts, fill=c + (A,), outline=(c + (255,)) if wire else None)
    out_im = Image.alpha_composite(base.convert('RGBA'), lay).convert('RGB')
    dd = ImageDraw.Draw(out_im)
    for i, n in enumerate(model.parts):
        dd.text((4, 4 + 11 * i), n, fill=pal[i % len(pal)])
    out_im.save(out)
