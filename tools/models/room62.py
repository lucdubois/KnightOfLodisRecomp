#!/usr/bin/env python3
"""Map 62, a house's upper floor under its gable roof (states/thor/0016;
README "Map 62: the attic room, modelled"). The game's grid is only the
floor (8 x 6 cells at height 1, Y 16); the walls, the sloped ceiling and
the furniture are art on BG0, so under the perspective they lay flat with
the floor. The room is a cut-away: the roof's ridge runs along X at Z 96
(the middle of the depth), the near slope and walls are cut off, so what
stands is the knee wall along the back (Z 0, to the eave), the far slope's
underside over it (the ceiling, its rafters painted on), and the gable end
at X 0 (a pentagon: eaves at Z 0 and 192, apex at the ridge). Fitted on
the scene state 16 composes without the HUD (KOL_FOCUS_DUMP .rgb, the
dump's own projection): the ridge on the roof's red edge, the apex on the
art's corner, the eave on the wall plate under the rafters (Y 70; 62 cut
through the plaster, 78 ran into the rafters).

    tools/models/room62.py                      writes src/kol_model_map62.inc
    tools/models/room62.py fit RGB INFO OUT [x0 y0 x1 y1]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from model import LIGHTPASS, Model, overlay  # noqa: E402

FLOOR = 16.0
X1, Z1 = 256.0, 192.0   # the grid's far edges (8 x 6 cells)
T = 32.0                # walls and roof a tile thick, as the art draws their cut ends
RIDGE_Z = 96.0
RIDGE_OUT = 232.0       # the roof's outer surface at the ridge (its red edge)
EAVE_OUT = 70.0         # ... at the back wall's outer face (Z 0)
FRONT_OUT = 110.0       # the gable's cut edge at the front (Z 192): the art's cut is not the far slope's mirror
SLOPE = (RIDGE_OUT - EAVE_OUT) / RIDGE_Z


def roof_out(z):        # the far slope's outer surface over Z
    return RIDGE_OUT - SLOPE * (RIDGE_Z - z)


def roof_in(z):         # its underside, a tile under it
    return roof_out(z) - T


def build():
    m = Model("attic")
    ki = roof_in(T)   # where the ceiling meets the back wall's inner face
    # The gable end, its inner face at X 32 (the wall on the grid's first
    # column, its outer face at X 0): under the ceiling at the back, up to
    # the roof's outer line at the front (the near roof is cut away there).
    m.poly("gable", [(T, FLOOR, T), (T, ki, T), (T, roof_in(RIDGE_Z), RIDGE_Z), (T, RIDGE_OUT, RIDGE_Z),
                     (T, FRONT_OUT, Z1), (T, FLOOR, Z1)])
    # Its cut edge along the front slope and its cut front end.
    m.poly("gable_cut", [(0.0, FRONT_OUT, Z1), (T, FRONT_OUT, Z1), (T, RIDGE_OUT, RIDGE_Z), (0.0, RIDGE_OUT, RIDGE_Z)])
    m.poly("gable_cut", [(0.0, FLOOR, Z1), (T, FLOOR, Z1), (T, FRONT_OUT, Z1), (0.0, FRONT_OUT, Z1)])
    # The back (knee) wall, its inner face at Z 32, to the ceiling.
    m.poly("knee", [(T, FLOOR, T), (X1, FLOOR, T), (X1, ki, T), (T, ki, T)])
    # The ceiling: the far slope's underside, from the back wall to the ridge.
    m.poly("ceiling", [(T, ki, T), (X1, ki, T), (X1, roof_in(RIDGE_Z), RIDGE_Z), (T, roof_in(RIDGE_Z), RIDGE_Z)])
    # The roof's cut edge along the ridge, facing +Z.
    m.poly("roof_cut", [(0.0, roof_in(RIDGE_Z), RIDGE_Z), (X1, roof_in(RIDGE_Z), RIDGE_Z), (X1, RIDGE_OUT, RIDGE_Z),
                        (0.0, RIDGE_OUT, RIDGE_Z)])
    # The cut ends at X 256, facing +X: the back wall's, and the roof's over it.
    m.poly("end_cut", [(X1, FLOOR, 0.0), (X1, EAVE_OUT, 0.0), (X1, ki, T), (X1, FLOOR, T)])
    m.poly("end_cut", [(X1, EAVE_OUT, 0.0), (X1, RIDGE_OUT, RIDGE_Z), (X1, roof_in(RIDGE_Z), RIDGE_Z), (X1, ki, T)])
    railing(m)
    furniture(m)
    return m


# The shelf, the box on its top and the crate beside it, from the user's
# painting (shots/attic62/furniture_drawn.png: yellow tops, red the faces
# toward the bottom left (+Z), cyan toward the bottom right (+X)), solved as
# boxes: the edge where red meets cyan is the front corner, its bottom the
# foot (on the floor, or the shelf's top for the box), the faces' far ends
# the depths. All three against the gable's inner face (X 32); the painted
# corners within ~2 px of the boxes'. The crate's top back corner lands on
# the painting's; its left end is behind the shelf.
def furniture(m):
    m.box("shelf", (T, FLOOR, 131.0), (55.0, 86.0, 179.0))
    m.box("shelf", (T, 86.0, 142.0), (50.0, 102.0, 162.0))
    m.box("crate", (T, FLOOR, 106.0), (56.0, 39.0, 126.0))
    table(m)


# The round table, two cylinders stacked one over the other (the user),
# from their painting (shots/attic62/table_drawn.png: red the upper one's
# top, magenta its side, cyan the lower one's top, yellow its side), each
# from its own colours: the upper sits 3 px higher and 1.5 px left of the
# lower on screen, the cyan ring between them (on purpose: the user, after
# I first put both on one axis). A circle of radius r is r x 1.414 wide on
# screen and r x 0.707 tall; both painted ~31 px wide: radius 22.
# Lower: yellow centred at screen x 258, its front on the floor at row
# 156.5; its top's front (cyan) at 150.5: X 227, Z 63.5, Y 16 to 28.
# Upper: red/magenta centred at x 256.4, standing on the lower's top
# (magenta's front at 147.25), its top's middle (red) at row 131.1: X 219,
# Z 58.5, Y 28 to 44.5.
TABLE_R = 22.0


def table(m):
    m.cyl("table", (227.0, FLOOR, 63.5), (227.0, 28.0, 63.5), TABLE_R, sides=16)
    m.cyl("table", (219.0, 28.0, 58.5), (219.0, 44.5, 58.5), TABLE_R, sides=16)
    stools(m)


# The three stools, from the user's painting (shots/attic62/stools_drawn.png,
# the template 47 rows down: yellow the top, red the side, wider at the
# floor). All three alike: the top 10 px wide (radius 7), the side 12.5 px
# at the floor (radius 9), 14 high: fitted to the silhouette, the yellow's
# top to the red's bottom (12.25 and 12.5 px; the painted tops are rounder
# than the projection's ellipse, so their middles gave 12, 2 px short);
# each placed by its side's front on the floor (the back one's is hidden by
# the left one's top: placed by its top's middle).
STOOL_R0, STOOL_R1, STOOL_TOP = 9.0, 7.0, 30.0


def stools(m):
    for x, z in ((180.0, 59.0), (196.0, 87.0), (239.0, 102.5)):
        m.cyl("stool", (x, FLOOR, z), (x, STOOL_TOP, z), STOOL_R0, sides=12, r1=STOOL_R1)
    bottles(m)


# The two bottles on the floor before the shelf, from the user's painting
# (shots/attic62/bottles_drawn.png, the template 23 px right and 32 down:
# blue the tall one, red the short one, yellow their tops), as tapered
# cylinders fitted as the stools: the widths at the bottom and under the
# top give the radii, the silhouette's top and bottom the height, the
# side's front on the floor the place.
def bottles(m):
    m.cyl("bottle", (66.7, FLOOR, 182.5), (66.7, 40.0, 182.5), 4.8, sides=10, r1=3.4)
    m.cyl("bottle", (78.8, FLOOR, 182.8), (78.8, 29.6, 182.8), 5.3, sides=10, r1=3.7)


# The railing round the stair opening, from the user's drawing
# (shots/attic62/railing_drawn.png, on railing_template_4x.png: red the
# rail's top, green its front face, magenta the side run's top, each post a
# yellow and cyan pair). Level: its two runs follow the floor's axes. The
# posts' feet (their pairs' bottoms on the floor) 16 apart: the front run
# along Z 92 (X 84 to 148), the side run along X 150 (Z 74, 58, 42) to the
# back wall; the rail's top at Y 45, its left end at X 72.
RAIL_Z, RAIL_X = 92.0, 150.0
RAIL_X0 = 72.0
RAIL_TOP, RAIL_H = 45.0, 5.0
POST = 3.0            # half a post's width
RAIL = 3.0            # half the rail's depth


def railing(m):
    # Light passes it (LIGHTPASS): as the light's solid blocks, the posts
    # and rail stopped every window beam before the rug.
    for x in (84.0, 100.0, 116.0, 132.0, 148.0):
        m.box("rail", (x - POST, FLOOR, RAIL_Z - POST), (x + POST, RAIL_TOP - RAIL_H, RAIL_Z + POST), LIGHTPASS)
    for z in (74.0, 58.0, 42.0):
        m.box("rail", (RAIL_X - POST, FLOOR, z - POST), (RAIL_X + POST, RAIL_TOP - RAIL_H, z + POST), LIGHTPASS)
    m.box("rail", (RAIL_X0, RAIL_TOP - RAIL_H, RAIL_Z - RAIL), (RAIL_X + RAIL, RAIL_TOP, RAIL_Z + RAIL), LIGHTPASS)
    m.box("rail", (RAIL_X - RAIL, RAIL_TOP - RAIL_H, T), (RAIL_X + RAIL, RAIL_TOP, RAIL_Z - RAIL), LIGHTPASS)


def main():
    m = build()
    if len(sys.argv) > 1 and sys.argv[1] == "fit":
        crop = tuple(int(a) for a in sys.argv[5:9]) if len(sys.argv) >= 9 else None
        overlay(m, sys.argv[2], sys.argv[3], sys.argv[4], crop=crop, scale=3)
        return
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kol_model_map62.inc")
    m.write_inc(out, 62, "tools/models/room62.py")
    print("%d vertices, %d triangles -> %s" % (len(m.verts), len(m.tris), os.path.normpath(out)))


if __name__ == "__main__":
    main()
