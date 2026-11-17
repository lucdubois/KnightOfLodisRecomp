#!/usr/bin/env python3
"""diff_frame_totkol.py — find the first OAM/VRAM/PAL/EWRAM/IWRAM/IO/
framebuffer divergence between this project's statically-recompiled
KnightOfLodisRecomp executable and the mGBA-backed oracle, for
Tactics Ogre: The Knight of Lodis (totkol.gba).

This is a game-specific adaptation of gbarecomp/oracle/diff_frame.py
(which only knows how to drive gbarecomp's own bios_smoke tool, i.e. a
BIOS-only boot). Same protocol, same sync discipline (PPU frame count
as a stop signal per CLAUDE.md SYNC RULES, not the primary clock) —
just pointed at our own executable + ROM instead.

Usage (run from this directory, or anywhere — paths below are absolute):
    python3 diff_frame_totkol.py --frame 60
    python3 diff_frame_totkol.py --scan 1 300 10

Both processes are auto-spawned unless --no-spawn is given:
    KnightOfLodisRecomp --rom totkol.gba --bios <gba_bios.bin> --tcp 19842
    gbarecomp_oracle    --rom totkol.gba --bios <gba_bios.bin> --port 19843
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import sys
import time
from typing import Iterable, Optional

PROJECT_ROOT   = pathlib.Path(__file__).resolve().parent.parent
GBARECOMP_ROOT = PROJECT_ROOT.parent / "gbarecomp"

NATIVE_EXE = PROJECT_ROOT / "build" / "KnightOfLodisRecomp"
ORACLE_EXE = GBARECOMP_ROOT / "build" / "oracle" / "gbarecomp_oracle"
ROM_PATH   = PROJECT_ROOT / "totkol.gba"
BIOS_PATH  = GBARECOMP_ROOT / "bios" / "gba_bios.bin"

# Region descriptors: (key, display name, byte size, native-cmd, oracle-cmd).
REGIONS = [
    ("oam",   "OAM",      1024,         "read_oam",   "read_emu_oam"),
    ("pal",   "PAL",      1024,         "read_pal",   "read_emu_pal"),
    ("vram",  "VRAM",     96 * 1024,    "read_vram",  "read_emu_vram"),
    ("ewram", "EWRAM",    256 * 1024,   "read_ewram", "read_emu_ewram"),
    ("iwram", "IWRAM",    32 * 1024,    "read_iwram", "read_emu_iwram"),
    ("io",    "IO",       0x400,        "read_io",    "read_emu_io"),
]


class JsonClient:
    """Line-delimited JSON request/response over TCP."""

    def __init__(self, host: str, port: int):
        deadline = time.time() + 10.0
        last_err: Optional[Exception] = None
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:
                last_err = e
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}: {last_err}")
        self.sock.settimeout(None)
        self.buf = b""

    def call(self, **kwargs) -> dict:
        line = json.dumps(kwargs).encode("utf-8") + b"\n"
        self.sock.sendall(line)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("peer closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.call(cmd="quit")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def read_region_chunked(self, cmd: str, size: int) -> bytes:
        chunk = 4096
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            resp = self.call(cmd=cmd, addr=off, len=n)
            if not resp.get("ok"):
                raise RuntimeError(f"{cmd} @{off}: {resp}")
            out += bytes.fromhex(resp["data"])
        return bytes(out)


def spawn(cmd: list, cwd: pathlib.Path, label: str) -> subprocess.Popen:
    print(f"==> spawning {label}: {' '.join(str(c) for c in cmd)}")
    return subprocess.Popen(
        [str(c) for c in cmd], cwd=str(cwd),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def find_first_diff(a: bytes, b: bytes) -> Optional[int]:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def diff_one_frame(frame: int, native: JsonClient, oracle: JsonClient,
                    oracle_lead: int = 1) -> bool:
    """Returns True iff everything checked was identical at this frame.

    oracle_lead compensates a confirmed, consistent one-frame difference
    in when each debug server's frame counter increments relative to its
    own internal per-frame update (native's `frame` command reports
    vblank_count, incremented at a different point in the main loop than
    mGBA's own frame counter). Verified 2026-09-17 across 20+ consecutive
    frames: native's snapshot at frame N is byte-identical to the
    oracle's snapshot at frame N+1, for every region that actually
    changes per frame (a counter at IWRAM+0x0c, OAM, PAL) — not a real
    execution divergence, a query-alignment offset. Default 1 compensates
    it; pass 0 to see the raw (misleading, one-frame-shifted) picture.
    """
    target_oracle_frame = frame + oracle_lead
    nf = native.call(cmd="frame")["frame"]
    of = oracle.call(cmd="frame")["frame"]
    if nf > frame or of > target_oracle_frame:
        print(f"==> frame {frame}: native@{nf} oracle@{of} — out of sync, skipping")
        return False
    print(f"==> Frame {frame} (oracle lead {oracle_lead}): stepping "
          f"({frame - nf} native, {target_oracle_frame - of} oracle)...",
          flush=True)
    while nf < frame:
        nf = native.call(cmd="step")["frame"]
    while of < target_oracle_frame:
        of = oracle.call(cmd="emu_step")["frame"]

    all_ok = True

    native_fb = native.call(cmd="screenshot")
    oracle_fb = oracle.call(cmd="emu_screenshot")
    if not native_fb.get("ok"):
        raise RuntimeError(f"native screenshot failed: {native_fb}")
    if not oracle_fb.get("ok"):
        raise RuntimeError(f"oracle screenshot failed: {oracle_fb}")
    nfb = bytes.fromhex(native_fb["data"])
    ofb = bytes.fromhex(oracle_fb["data"])
    first_fb = find_first_diff(nfb, ofb)
    if first_fb is None:
        print("    FRAMEBUFFER: identical")
    else:
        all_ok = False
        pixel = first_fb // 3
        chan = first_fb % 3
        x = pixel % 240
        y = pixel // 240
        print(f"    FRAMEBUFFER: first diff @({x},{y}) channel={chan} "
              f"native=0x{nfb[first_fb]:02x} oracle=0x{ofb[first_fb]:02x}")

    for key, name, size, ncmd, ocmd in REGIONS:
        native_blob = native.read_region_chunked(ncmd, size)
        oracle_blob = oracle.read_region_chunked(ocmd, size)
        first = find_first_diff(native_blob, oracle_blob)
        if first is None:
            print(f"    {name}: identical")
        else:
            all_ok = False
            nb = native_blob[first]
            ob = oracle_blob[first]
            print(f"    {name}: first diff @0x{first:05x} "
                  f"native=0x{nb:02x} oracle=0x{ob:02x}")

    return all_ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=60)
    ap.add_argument("--scan", nargs=3, type=int, metavar=("START", "END", "STEP"))
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--oracle-lead", type=int, default=1,
                    help="Compensate the confirmed 1-frame query-alignment "
                         "offset (see diff_one_frame docstring). 0 = raw.")
    ap.add_argument("--stop-at-first-diff", action="store_true",
                    help="Stop scanning as soon as any region diverges")
    ap.add_argument("--no-spawn", action="store_true",
                    help="Assume both processes are already running")
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_spawn:
            if not NATIVE_EXE.exists():
                print(f"missing {NATIVE_EXE} "
                      f"(cmake --build build --target KnightOfLodisRecomp first)",
                      file=sys.stderr)
                return 1
            if not ORACLE_EXE.exists():
                print(f"missing {ORACLE_EXE} "
                      f"(build gbarecomp with -DGBARECOMP_BUILD_ORACLE=ON)",
                      file=sys.stderr)
                return 1
            if not ROM_PATH.exists():
                print(f"missing {ROM_PATH}", file=sys.stderr)
                return 1
            procs.append(spawn(
                [NATIVE_EXE, "--rom", ROM_PATH, "--bios", BIOS_PATH,
                 "--tcp", str(args.native_port)],
                PROJECT_ROOT, "native (KnightOfLodisRecomp)"))
            procs.append(spawn(
                [ORACLE_EXE, "--bios", BIOS_PATH, "--rom", ROM_PATH,
                 "--port", str(args.oracle_port)],
                GBARECOMP_ROOT, "oracle (mGBA)"))

        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)
        try:
            if args.scan:
                start, end, step = args.scan
                frames: Iterable[int] = range(start, end + 1, step)
            else:
                frames = [args.frame]
            for f in frames:
                ok = diff_one_frame(f, native, oracle, args.oracle_lead)
                if not ok and args.stop_at_first_diff:
                    print(f"==> stopping at first divergence, frame {f}")
                    break
        finally:
            native.close()
            oracle.close()
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
