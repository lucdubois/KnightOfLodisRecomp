#!/usr/bin/env python3
"""The mask of a map model, painted and measured (README "The masking,
corrected").

    tools/models/mask.py FRAME PREFIX OUT.png [x0 y0 x1 y1 [scale]]

FRAME is the present dump (bmp/png) of the run that wrote PREFIX.model and
PREFIX.layers (KOL_FOCUS_DUMP=PREFIX, with KOL_EDGE_BLUR=1 and the
perspective off). Paints each pixel by the part that took it (the palette
of model.py's overlay: hull red, bulwark green, castle blue, deck yellow,
mast magenta, yard cyan, sail orange, bowsprit purple, rigging white) and
prints, for the crop, the leftmost and rightmost modeled column per row
and the part counts: judge the fit by these, not by the tint.
"""
import sys
from collections import Counter
from PIL import Image

PAL = [(255, 60, 60), (60, 255, 60), (60, 120, 255), (255, 255, 60), (255, 60, 255), (60, 255, 255),
       (255, 160, 40), (160, 80, 255), (255, 255, 255), (120, 255, 160), (255, 120, 160), (160, 160, 60)]


def main():
    frame, prefix, out = sys.argv[1:4]
    im = Image.open(frame).convert('RGB')
    W, H = im.size
    M = open(prefix + '.model', 'rb').read()
    crop = tuple(int(a) for a in sys.argv[4:8]) if len(sys.argv) >= 8 else (0, 0, W, H)
    scale = int(sys.argv[8]) if len(sys.argv) >= 9 else 4
    px = im.load()
    for y in range(H):
        for x in range(W):
            m = M[y * W + x]
            if m:
                c = PAL[(m - 1) % len(PAL)]
                r, g, b = px[x, y]
                px[x, y] = ((r + c[0]) // 2, (g + c[1]) // 2, (b + c[2]) // 2)
    x0, y0, x1, y1 = crop
    im.crop(crop).resize(((x1 - x0) * scale, (y1 - y0) * scale), Image.NEAREST).save(out)
    print("parts in the crop:", dict(Counter(M[y * W + x] - 1 for y in range(y0, y1) for x in range(x0, x1)
                                          if M[y * W + x])))
    print("row: leftmost/rightmost modeled column")
    for y in range(y0, y1, 2):
        cols = [x for x in range(x0, x1) if M[y * W + x]]
        print("%d:%s" % (y, "%d/%d" % (cols[0], cols[-1]) if cols else "-"), end=" ")
    print()


if __name__ == "__main__":
    main()
