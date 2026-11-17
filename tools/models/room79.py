#!/usr/bin/env python3
"""Map 79: a mossy cave with a colonnade (four pillars, cells 4,3 6,3 4,9 6,9)
leading to a stone gate at its head, crystal clumps down the right side and
a cliff at the front left. Measured on the viewer's flat frame (480x270,
sprites and HUD off, unlit, perspective off), projection
sx = (X - Z)/2 + 240, sy = (X + Z)/4 - Y/2 + 87 (checked on the pillars'
cells: cell 4,9's top at Y 144 projects to the diamond x 144-176, rows
119-135, round the art's pillar top, x 152-167 from row 124). A pixel shows
what covers its corner point. Along a screen line the two families of edges
give, for a point (X, Y, Z):
    s = x + 2y = X - Y + 414        (an edge along Z, slope -1/2)
    d = x - 2y = Y - Z + 66         (an edge along X, slope +1/2)
Every "s" or "d" below is such a line measured on the art as the separating
line between the last pixel in and the first out.

The grid is the cave alone: the walls (row z 0, column x 0: height 20), the
walkway at height 5 (Y 80) over x 1-6, z 2-10, the pillars at 9 (lowered
to the walkway by the user: modelled below), the right side stepping down. The gate is NOT in it: its cells (x 2-3, z 5-8) are
plain walkway, so the terrain pass laid its pixels on the floor at the left
wall's foot behind it (X 32-60, Y 80), and with the camera moved its roof
slid onto the back wall as a slab and its posts sheared. The gate's art is
on BG0 with the wall's (one picture, drawn in 2x2 blocks above row 124, so
its edges are good to a pixel or two).

The gate faces +X down the colonnade, its long side along Z, centred on the
colonnade's axis (Z 208, between the pillar rows z 3 and z 9): a stepped
base on the floor, two square posts, and a slab roof overhanging them. The
opening between the posts shows the cave wall behind: no part there, it
stays the terrain's. Depth comes from contact: the base stands on the
floor (Y 80).

    tools/models/room79.py      writes src/kol_model_map79.inc
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import BG1, OVER, SHEET, Model, BG0ART  # noqa: E402

FLOOR = 80.0                 # the grid's height 5, the walkway

# ---------------------------------------------------------------- the roof
# A slab, its top lit, its +X face a darker band under it, its +Z end in shade.
#   top's back edge (along Z)    s = 337: the first stone pixels (158,90)
#                                (170,84) (176,80) (186,74) (196,70) (204,66)
#   the left end's top corner    (156.5, 91.5): its leftmost column, x 156,
#                                rows 92-93 (rows 94-97 the dark end face)
#   front face's bottom edge     s = 373: green from row 92 at x 190, row 86
#                                at x 200 (the opening's top)
#   top / front face split       s ~ 358-361 (x 190: light to row 85)
#   back end's top edge (along X) d = 78: (215,68) (217,70)
#   right extreme                x 218 (its dark outline at 218-219, rows 74-79)
# Solved together: the top's back edge and the left corner give
# X0 - Y1 = -76 and Z1 - Y1 = 92.5, the front's bottom X1 - Y0 = -41,
# depth 20, thickness 15, the back end Z0 = Y1 - 12 (all relative to the
# top's height Y1, which the base below fixes).
ROOF_TOP = 168.0
# The reviewer's reading (2026-11-09): the front face's bright band s 358-363
# is a capstone's face, about 6 thick, over the body's face s 364-373; the
# body's left end starts at x 160 (rows 94-99: the cap reaches x 155.5 at rows
# 90-93), and the cap's right end has the same lip (stone at x 218-219, rows
# 68-73). The top's back edge reaches s 336-337 in places: X0 91. The body's
# right end Z0 156.5 (on x 218 exactly it took column 218, wall green at rows
# 76-77); the cap's 155 (154 put a few wall pixels on it at x 212-216, rows
# 65-67).
ROOF_CAP = ((91.0, 162.0, 155.0), (112.0, ROOF_TOP, 260.5))
ROOF = ((91.0, 153.0, 156.5), (112.0, 162.0, 253.0))

# ---------------------------------------------------------------- the base
# Steps along its +X front, read as stripes of constant s (light along each
# line averaged over x 170-186, 186-200, 200-216, the same in all three):
# a bright nosing at s 436-438, dark 442-444, bright 448-450, dark 452, then
# the floor's moss from s 454 (green in every column). So the upper step's
# top front edge s = 437 and the lower step's foot s = 454 on the floor:
#   upper (the platform the posts stand on): top Y 94, front X 117
#   lower step: top Y 87, front X 120 (its foot X - 80 + 414 = 454)
# The nosing runs from x 172 to x 219; the base's right end is the column
# x 219 (stone at rows 104-115 to x 219, its outline at 220-221, the floor's
# moss from row 116: the corner's foot, (117 + 157)/4 - 40 + 87 = 115.5):
# Z 157 at X 117. Its left end Z 252 (the nosing's last bright pixel x 172;
# behind pillar 4,9 below that). Its back is seen only through the opening:
# the opening's bottom, green to row 105 at x 186-190 (s ~ 400), puts it at
# X 84 (s = 84 - 94 + 414 = 404). The lower step 3 in from each end (the
# art's step ends inside the platform's: its right end x 220).
BASE_TOP = 94.0
# Its right end to x 222 with its outline (220-221, rows 106-115, left to the
# wall slid off the gate with the camera): Z0 153, the step's 156 (reviewer).
BASE = ((84.0, FLOOR, 153.0), (117.0, BASE_TOP, 252.0))
STEP = ((117.0, FLOOR, 156.0), (120.0, 87.0, 250.0))

# ---------------------------------------------------------------- the posts
# Square posts on the base up to the roof's underside, 3 in from the roof's
# front. Each has a navy vertical line down its face (a fluting: front post
# x 172-173, rows 104-120; back post x 210-211, rows 86-100), the same
# height on screen and 38 columns apart: posts at one X, 76 apart in Z. The
# front post's left edge x 166 (its lit +Z face 166-171), its right x 178
# (the opening green from x 180); the back post's right edge is the gate's
# right outline, x 216 (the dark column 216-217 at rows 80-98), flush with
# the roof's back end. 12 x 12, X 97-109:
#   front post Z 232-244: left edge (97 - 244)/2 + 240 = 166.5, right
#   (109 - 232)/2 + 240 = 178.5; back post Z 156-168: right (109 - 156)/2 +
#   240 = 216.5.
# The back post's +Z face is dark in the art (x 204-209: the inside of the
# gate in shade, the dark running on to x 190 in the opening, the wall's).
# Corrected by the reviewer (2026-11-09): the two posts are one drawing, best
# matched shifted (+40, -20): 80 apart in Z, not 76 (the back post's fluting is
# at x 212-213, rows 86-97). And thin: in this art +Z faces are dark and +X
# faces light, so the bright band x 168-177 (light, mid, the navy groove,
# light, the dark edge) is the post's +X face, 20 wide, the groove in its
# middle, and its +Z face only the dark 2 px at 166-167: 4 deep. Posts X
# 105-109; front Z 233.5-253 (screen 166 / 177.75, corner 168), back Z
# 153.5-173 (206 / 217.75, corner 208, with its outline column 217).
POST_X = (105.0, 109.0)
POSTS_Z = ((153.5, 173.0), (233.5, 253.0))

# The height: the base's right end (X 117 at x 219: Z 157, the back post's
# and the roof's back end Z0 = Y1 - 12, one in) gives Y1 = 168 (X0 92, X1
# 112); the base top's front edge s = 437 then lies 8 in front of the posts,
# the posts 59 tall (Y 94-153), the gate 88 over the floor, its middle Z
# 208, the colonnade's (the pillar rows z 3 and 9). With the gate further
# back the base's right end came out right of the art's (x 229 for the
# posts on the nosing's line s = 414 + 25: the art is wall there).

# ---------------------------------------------------------------- the pillars
# The user lowered the four pillar cells to the walkway on the Thor (4,3 6,3
# 6,9 to 5, 4,9 to 4): no longer grid blocks, the pillars are modelled, as
# map 73's columns (room73.py column(): stacked pieces, BG1 | OVER). Their art
# is BG1 (fd.layers), and all four are ONE drawing: the same rows and widths,
# each at its own place. Per pillar, its BG1 rows (first row, the widths):
#              centre   top  w18 (abacus)  w16 (block)  w14 (shaft)  w16 (torus)  w20.. (plinth)
#   4,3        255.5     76     80-82        83-87        88-103      104-106      107-110
#   6,3        287.5     92     96-98        99-103      104-119      120-122      123-126
#   4,9        159.5    124    128-130      131-135      136-151      152-154      155-158
#   6,9        191.5    140    144-146      147-151      152-167      168-170      171-174
# Centres (pixels 254-257, 286-289, 158-161, 190-193 on the top row, every
# width symmetric about them): X - Z = 2 (c - 240) = 31, 95, -161, -97, a
# unit off each cell's middle (32, 96, -160, -96): X - 0.5, Z + 0.5. The top
# rows minus the cell middle's sy at Y 0 ((X + Z)/4 + 87 = 151, 167, 199,
# 215) are -75 for all four: all four stand at one height, on Y 80. Pillar
# 4,9 on a floor at Y 64 (the user's height 4) would be drawn 8 rows lower
# than it is: the art puts its foot at Y 80 like the others.
# Shapes, from the widths (pixel corners: a width of n pixels about c.5 is a
# half width n/2, the edges half a pixel outside the outer pixels):
#   - the capital's top widens 4 a row (4, 8, 12, 16, then 18): a diamond,
#     so a square abacus, half 9 (an ellipse would widen 8, 13, 17, 18);
#   - the block under it, half 8 (square: the capital's lower part, lit,
#     with a dark lower edge);
#   - the shaft, half 7, fluted in the art (vertical stripes): round,
#     r = 7 / 0.7071 = 9.9; the torus at its foot half 8, r 11.3;
#   - the plinth widens 4 a row to 32: a square slab, half 16 (the cell's
#     width), its top's back half on BG1, the front half on BG0 down to
#     row 134 (6,3): its top is the whole diamond, a slab on the floor.
# Heights (6,3's rows; centre's sy = 167 - Y/2, a square's back corner
# (half h) at 167 - h/2 - Y/2): the abacus's back corner at row 91 (row 92 4
# wide, row 95 16: 2 (r - v) in [7.5, 8.5)) -> top Y 143; its corners' foot
# between rows 98 and 99 -> 137 (at 136.5 the abacus's lower edges took a
# pixel either side on row 99, the block's 16 wide); the block's foot
# between 103 and 104 -> 127 (likewise on row 104); the shaft's sides rows
# 104-119 -> Y 126 down to 95; the torus's rows 120-122: its top rim's
# ellipse (half 8 by 4) must stay under 7.5 wide on row 119 and reach it on
# row 120 -> top Y 93; its round foot hidden behind the plinth; the plinth's back corner at row 118 (row 123 20 wide, row 126 32)
# -> top Y 82, 2 over the floor.
PILLARS = ((143.5, 112.5), (207.5, 112.5), (143.5, 304.5), (207.5, 304.5))   # 4,3 6,3 4,9 6,9
PILLAR_NAMES = ("pillar_4_3", "pillar_6_3", "pillar_4_9", "pillar_6_9")
PLINTH = (16.0, 82.0)          # half side, top
TORUS = (11.3, 93.0)           # radius, top
SHAFT = (9.9, 127.0)           # radius, top (under the capital's block)
BLOCK = (8.0, 137.0)           # half side, top
ABACUS = (9.0, 143.0)          # half side, top
RUIN = BG1 | OVER              # its art is BG1; lifted, it opens onto the gate behind (4,9)

# ---------------------------------------------------------------- left alone
# - The crystal clumps (cells 8,2 10,1 11,9 9,11, byte 0x09; 8,9 and 11,3,
#   byte 0x08): BG1, jagged; a box would take the wall between their shards.
# - The front-left cliff (row z 11, heights 16 16 10 ...) and the small BG1
#   lips of the terraces on the right: the grid's own steps, drawn on the
#   cells.
# - The cave walls' rock texture stretching with the camera.


# ---------------------------------------------------------------- the back wall
# The user (2026-11-09) asked for the arch as a doorway set into the back
# wall, and the back wall as an uneven slope rather than vertical, from X 0
# Y 20 to between X2 and X3, Y 5. The left
# wall (grid column x 0, height 20) as a slope from X 0 at its top, Y 320 (the
# art's top edge against the backdrop is the line s = x + 2y = 92, X = -2 at
# Y 320: no top drawn), down to the walkway's Y 80 at a foot that wanders
# between X 66 and 94 (the green rock comes down between the floor lines X 64
# and 96 in the art). Uneven: soft bumps over it, none at its top (the
# silhouette stays the art's). Around the gate the wall comes forward to the
# gate's back, X 84, so the gate is a doorway set in it: between its posts a
# recess about 20 deep, not the floor behind.
WALL_TOP = 320.0
WALL_Z = (32.0, 384.0)        # cells z 1-11 (the back wall's face at Z 32; through the cliff's row, else a gap opened at its end)
WALL_STEP = 8.0
GATE_FRAME = (150.0, 262.0, 150.0, 84.0)   # Z0, Z1, its top Y, its face X
# Up to Y 150 in full, gone by Y 166: under the roof's top (168), behind its
# body. Up to 172 and eased over 40 upward it stood as a flat plate over the
# arch, the roof shading it (lamp, occlusion) and the shade cut off where the
# plate turned back into the slope: a hard-edged dark oval over the archway (the
# user, 2026-11-09, asked for a softer shadow over the archway, with smooth
# transitions). Above the roof the wall is the slope.
# Faded over 128, not 16: from X 84 back to the slope (~X 50) over 16 the fade
# was a near-level shelf behind the roof, its pixels taken for a floor (normal
# y over 0.7): a floor's occlusion and a top's light, cut off hard where the
# wall began (a dark band hugging the roof; over 64 still a patch, the slope's
# own lean, 1 in 3, and the bumps adding to the fade's). Over 170, the bumps
# faded out where the frame is: wall all the way, receding into the slope.
GATE_FRAME_FADE = 170.0   # (to the wall's top; the bumps fade out in the frame: they added up to 0.3)
# Eased over 40: over 12 the step between the slope and the frame was a short
# wall on the gate's left facing +Z, away from the lamp at Z 220: black, a
# harsh shadow left of the arch (the user, 2026-11-09).
GATE_BLEND = 40.0


def wall_foot(z):
    return 80.0 + 10.0 * math.sin(z / 37.0) + 5.0 * math.sin(z / 13.0 + 2.0)


def wall_x(y, z):
    t = (WALL_TOP - y) / (WALL_TOP - FLOOR)          # 1 at the foot, 0 at the top
    x = wall_foot(z) * t
    z0, z1, ytop, face = GATE_FRAME
    dz = max(z0 - z, z - z1, 0.0)
    dy = max(y - ytop, 0.0)
    w = 0.0
    if dz < GATE_BLEND and dy < GATE_FRAME_FADE:
        u = dz / GATE_BLEND
        v = dy / GATE_FRAME_FADE
        w = (1.0 - u * u * (3.0 - 2.0 * u)) * (1.0 - v * v * (3.0 - 2.0 * v))
    bump = (5.0 * math.sin(z / 23.0 + 1.3) * math.sin(y / 31.0) + 3.0 * math.sin(z / 11.0 + y / 17.0))
    x += bump * math.sin(math.pi * min(1.0, t)) ** 0.5 * (1.0 - w)
    x += (max(x, face) - x) * w
    return x


# In the back corner the grid's ledge (row z 1: cells x 1-4 at height 10,
# Y 160, X 32-160) is the floor the wall stands on: the slope starts on its top
# there, easing to the walkway's Y 80 by Z 80. Run down to Y 80 inside the
# ledge, the slope's pixels there lay inside a grid block, the lamp's sight from
# them blocked: a black spike behind the farthest pillar (the user saw an
# odd shadow behind the farthest pillar).
LEDGE = (64.0, 80.0, 160.0)   # full to Z 64, eased out by Z 80, its top Y


# And in row z 11 (Z 352-384) the cliff's cells x 1-2 at height 16 (Y 256):
# the slope starts on their top there, so the row's tiles stand at their own
# heights (the user, 2026-11-09, asked for normal tiles in the Z11 row, some
# of which had joined the sloped back wall); above them it runs on to
# Z 384, which closes the corner (stopped at Z 352 it left a gap).
CLIFF = (352.0, 256.0)   # from Z, its top Y


def wall_floor(z):
    z0, z1, top = LEDGE
    if z >= CLIFF[0]:
        return CLIFF[1]
    if z <= z0:
        return top
    if z >= z1:
        return FLOOR
    u = (z - z0) / (z1 - z0)
    return top + (FLOOR - top) * u * u * (3.0 - 2.0 * u)


# The wall's columns: every WALL_STEP, and one just before the cliff's row, so
# the step up onto the cliff is vertical: from Z 344 (floor Y 80) to 352 (Y 256)
# the strip made a slanted face less than 48 behind cell x 3, z 11's top, which
# it took (the user, 2026-11-09, still saw a fault on x3 z11; the Thor drew the
# cursor's tile on it upright).
WALL_ZS = sorted(set([WALL_Z[0] + WALL_STEP * i for i in range(int((WALL_Z[1] - WALL_Z[0]) / WALL_STEP) + 1)] +
                     [CLIFF[0] - 0.05]))


def add_back_wall(m):
    rows = []
    y = FLOOR
    while y <= WALL_TOP + 1e-6:
        row = []
        for z in WALL_ZS:
            ye = max(y, wall_floor(z))
            row.append((wall_x(ye, z), ye, z))
        rows.append(row)
        y += WALL_STEP
    # BG0ART: its art is the BG0 rock; the cliff's and ledges' BG1 tiles in
    # front of it stay the terrain's (it took cell x 3, z 11's top).
    m.strip("back_wall", rows, SHEET | BG0ART)


def pillar(m, name, x, z):
    """A pillar on the floor: plinth slab, torus, round shaft, the capital's
    block and abacus (square). 16 sides: a vertex on the screen's sides, so
    the silhouette's width is the radius's (0.7071 r) exactly."""
    sq = lambda h, y0, y1: m.box(name, (x - h, y0, z - h), (x + h, y1, z + h), RUIN)
    sq(PLINTH[0], FLOOR, PLINTH[1])
    m.cyl(name, (x, PLINTH[1], z), (x, TORUS[1], z), TORUS[0], sides=16, caps=True, flags=RUIN)
    m.cyl(name, (x, TORUS[1], z), (x, SHAFT[1], z), SHAFT[0], sides=16, caps=False, flags=RUIN)
    sq(BLOCK[0], SHAFT[1], BLOCK[1])
    sq(ABACUS[0], BLOCK[1], ABACUS[1])


def build_parts(m):
    """The gate: base and its step, two posts, the roof. Boxes; their pixels
    are BG0 (the gate is drawn in one picture with the wall). And the back
    wall's slope it is set in. And the four pillars (BG1 | OVER), on the
    floor (Y 80) in the middle of their cells."""
    add_back_wall(m)
    # The cliff's two cells in row z 11 (x 1-2, height 16) as the model's own
    # block: the slope runs on above them a little behind their tops, and the
    # map's kTerrainNearer 48 gave it their tops' pixels (the user saw some
    # tiles joining the sloped back wall). As a part, the nearer wins.
    # (From x 3's top, Y 160: below it its +X face lay inside that cell, which
    # it took.)
    m.box("cliff_z11", (32.0, 160.0, CLIFF[0]), (96.0, CLIFF[1], 384.0))
    m.box("gate_base", *BASE)
    m.box("gate_base", *STEP)
    for z0, z1 in POSTS_Z:
        m.box("gate_posts", (POST_X[0], BASE_TOP, z0), (POST_X[1], ROOF[0][1], z1))
    m.box("gate_roof", *ROOF_CAP)
    m.box("gate_roof", *ROOF)
    for name, (x, z) in zip(PILLAR_NAMES, PILLARS):
        pillar(m, name, x, z)
    return m


def build():
    return build_parts(Model("room79"))


if __name__ == "__main__":
    m = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "kol_model_map79.inc")
    m.write_inc(out, 79, "tools/models/room79.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))
