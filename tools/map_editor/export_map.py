#!/usr/bin/env python3
"""Export a map's tiles, palette and composed map for the map editor.

Runs the game headless from a save state (no window, a frame limit and an
alarm), dumps its memory (KOL_MEM_DUMP) and writes maps/map<N>.json beside
this script: the BG tiles of char blocks 0-1 (tile ids 0-1023, 4bpp), the
BG palette, and the guest's composed map for BG0 and BG1 (the array at
[0x02026CA8], 128 entries a row, src/kol_map_source.cpp).

    python3 tools/map_editor/export_map.py states/EW_newgame_dialogue.state
    python3 tools/map_editor/export_map.py --dump ew.bin   # an existing dump

The JSON holds the cartridge's art, so maps/ is not committed.
"""
import argparse, base64, json, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))   # KnightOfLodisRecomp

EWRAM, IWRAM, PAL, VRAM = 0x40000, 0x8000, 0x400, 0x18000
COLS, STRIDE = 128, 0x100
ARRAY_PTR, MAP_NUMBER = 0x26CA8, 0x29D40          # EWRAM offsets


def dump_state(state, frames):
    fd, path = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    env = dict(os.environ, KOL_MEM_DUMP=f"{frames - 10},{path}",
               GBARECOMP_SELFHEAL_RECOMPILE="0")
    cmd = ["perl", "-e", "alarm 120; exec @ARGV",
           os.path.join(ROOT, "build", "KnightOfLodisRecomp"), "--no-window", "--quiet",
           "--rom", "totkol.gba", "--load-state", state, "--frames", str(frames)]
    subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if os.path.getsize(path) < EWRAM + IWRAM + PAL + VRAM:
        sys.exit("export_map: the run wrote no memory dump")
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("state", nargs="?", help="save state to load (relative to the repo)")
    ap.add_argument("--dump", help="an existing KOL_MEM_DUMP file instead of a run")
    ap.add_argument("--frames", type=int, default=30)
    a = ap.parse_args()
    if not a.state and not a.dump:
        ap.error("give a state or --dump")
    path = a.dump or dump_state(a.state, a.frames)
    d = open(path, "rb").read()
    if not a.dump:
        os.remove(path)
    ew = d[:EWRAM]
    pal = d[EWRAM + IWRAM:EWRAM + IWRAM + PAL]
    vram = d[EWRAM + IWRAM + PAL:EWRAM + IWRAM + PAL + VRAM]

    map_no = struct.unpack_from("<H", ew, MAP_NUMBER)[0]
    bg0, bg1 = struct.unpack_from("<II", ew, ARRAY_PTR)
    rows = (bg1 - bg0) // STRIDE
    if bg0 < 0x02000000 or rows <= 0 or rows > 256 or (bg1 - bg0) % STRIDE:
        sys.exit(f"export_map: no composed map here (pointers {bg0:08x} {bg1:08x})")
    layers = []
    for base in (bg0, bg1):
        off = base - 0x02000000
        layers.append(list(struct.unpack_from(f"<{rows * COLS}H", ew, off)))
    colours = []
    for i in range(256):
        v = struct.unpack_from("<H", pal, i * 2)[0]
        colours.append([(v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
                        ((v >> 10) & 31) * 255 // 31])
    out = {
        "map": map_no, "cols": COLS, "rows": rows,
        "source": a.state or a.dump,
        "palette": colours,
        "tiles": base64.b64encode(vram[:0x8000]).decode(),
        "bg0": layers[0], "bg1": layers[1],
    }
    os.makedirs(os.path.join(HERE, "maps"), exist_ok=True)
    dst = os.path.join(HERE, "maps", f"map{map_no}.json")
    with open(dst, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    used = sum(1 for i in range(rows * COLS) if layers[0][i] or layers[1][i])
    print(f"map {map_no}: {rows}x{COLS}, {used} cells of art -> {os.path.relpath(dst, ROOT)}")


if __name__ == "__main__":
    main()
