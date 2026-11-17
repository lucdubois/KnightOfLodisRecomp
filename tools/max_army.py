#!/usr/bin/env python3
"""A save state with the army at its best, to go through the game quickly.

  tools/max_army.py IN.state OUT.state

Best on the world map, where the roster is what counts. Every unit in the roster
(0x02000030, stride 0x68): level 50, EXP 0, HP, MP, STR, INT and AGI 999 (the level-up
routine's own limit for all five, 0x08140DFC on), every emblem (+0x08; each class shows
the ones it has), so class changes open by stats and emblems; and for the other gates of
the class routine (0x0813A378): the three counters +0x2C..+0x2E at 0 (limits on Priest,
Dragon Tamer, Beast Tamer), and every unit who can change class (class +0x18 bit 2) Neutral
(+0x55 = 2), which passes all the alignment rules. Sex and monsters stay as they are.
War funds (0x02001184) at 99,999,999, the game's own limit (0x08133C44 and four more).
Stock tables (entries {id, owned,
equipped, 0}; what is there is kept and appended to, as the game's shop does): every
piece of equipment, ids 1-164 but the six with no name (17-22; 0x02000DA0, 164 entries),
every spell a class can learn (talents of kinds 1-6, 11 with a class mask; 0x02001040,
54 entries), COPIES of each, so all can wear one.
"""
import struct
import sys

COPIES = 20
ROM = "totkol.gba"


def ewram(b):
    i = 0x34
    while i + 8 <= len(b):
        tag, size = bytes(b[i:i + 4]), struct.unpack("<I", b[i + 4:i + 8])[0]
        if tag == b"BUS0":
            return i + 8
        i += 8 + size
    raise SystemExit("no BUS0 section: not a GBAS v2 state")


def stock(b, table, capacity, ids):
    ent = {}
    n = 0
    while n < capacity and b[table + n * 4]:
        ent[b[table + n * 4]] = table + n * 4
        n += 1
    for i in ids:
        if i not in ent:
            if n >= capacity:
                raise SystemExit("stock table full")
            ent[i] = table + n * 4
            b[ent[i]:ent[i] + 4] = bytes((i, 0, 0, 0))
            n += 1
    for at in ent.values():
        b[at + 1] = max(b[at + 1], COPIES)
    return n


def main():
    # tools/max_army.py IN OUT [NAME=CLASS ...]: units to put in a class the game would
    # not offer (26 Lich): +0x4F and +0x50, as a class change does, spell slots opened.
    swaps = dict(a.split("=") for a in sys.argv[3:])
    b = bytearray(open(sys.argv[1], "rb").read())
    if b[:4] != b"GBAS":
        raise SystemExit("not a GBAS state")
    rom = open(ROM, "rb").read()
    e = ewram(b)
    units = 0
    for r in range(32):
        o = e + 0x30 + r * 0x68
        if b[o + 0x52] == 0:
            continue
        units += 1
        b[o + 0x52], b[o + 0x53] = 50, 0
        b[o + 0x40:o + 0x4E] = struct.pack("<7H", 999, 999, 999, 999, 999, 999, 999)
        b[o + 0x08:o + 0x0C] = b"\xff\xff\xff\xff"
        b[o + 0x2C:o + 0x2F] = bytes(3)
        who = bytes(b[o + 0x30:o + 0x40]).split(b"\xff")[0]
        for nm, to in swaps.items():
            if who == bytes((ord(ch) - 65 if ch < "a" else ord(ch) - 97 + 26) for ch in nm):
                b[o + 0x4F] = b[o + 0x50] = int(to)
                b[o + 0x64:o + 0x68] = bytes((0, 0, 0, 255))
                print("%s -> class %s" % (nm, to))
        c = b[o + 0x50]
        cls = rom[0x615228 + c * 88:0x615228 + (c + 1) * 88]
        if cls[9] and cls[0x54] == 0:   # a female entry: the data is the male class's
            cls = rom[0x615228 + cls[9] * 88:0x615228 + (cls[9] + 1) * 88]
        if cls[0x18] & 4:
            b[o + 0x55] = 2
    items = [i for i in range(1, 165) if struct.unpack_from("<H", rom, 0x616F60 + i * 36)[0]]
    spells = []
    for i in range(1, 94):
        t = 0x61A8F0 + i * 64
        if struct.unpack_from("<H", rom, t + 2)[0] in (1, 2, 3, 4, 5, 6, 11) and struct.unpack_from("<H", rom, t + 0x20)[0]:
            spells.append(i)
    b[e + 0x1184:e + 0x1188] = struct.pack("<I", 99999999)
    ni = stock(b, e + 0xDA0, 164, items)
    ns = stock(b, e + 0x1040, 54, spells)
    open(sys.argv[2], "wb").write(b)
    print("%d units; %d kinds of equipment, %d spells, %d copies of each" % (units, ni, ns, COPIES))


if __name__ == "__main__":
    main()
