#!/usr/bin/env python3
"""repro_callstack_crash.py — deterministic reproduction of the call-return
stack crash(es) tracked in README.md / CLAUDE.md, for Tactics Ogre: The
Knight of Lodis.

Drives KnightOfLodisRecomp over its own TCP debug protocol (NOT the
file-based GBARECOMP_INPUT_REPLAY mechanism, which does not apply when
stepping over TCP via `step`/`run_frames` — a gotcha confirmed the hard
way; see README.md). Input is injected directly via run_frames's own
`keyinput` parameter, mashing Start to get past the title screen, then a
repeating A/B/direction sequence into menu/gameplay code.

Prints every new entry pushed onto the tracked call-return stack (via
the `call_stack_dump` TCP command) whenever depth grows, letting two
dumps at different times show exactly which return addresses never got
removed. This is how bug #1 (a slow leak; fixed upstream in gbarecomp)
was found. Bug #2 (a separate, faster, still-unfixed crash under this
exact input pattern) reproduces at the same deterministic frame every
run — see README.md for what's understood about it so far.

Usage:
    python3 repro_callstack_crash.py
    python3 repro_callstack_crash.py --abort-at 50   # force a fast, dense
                                                       # repro instead of
                                                       # waiting for a real
                                                       # climb; requires
                                                       # gbarecomp built
                                                       # with the
                                                       # GBARECOMP_CALL_STACK_ABORT_AT
                                                       # diagnostic (see
                                                       # gbarecomp's own
                                                       # runtime_arm.cpp)
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import sys
import time

PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
GBARECOMP_ROOT = PROJECT_ROOT.parent / "gbarecomp"
NATIVE_EXE = PROJECT_ROOT / "build" / "KnightOfLodisRecomp"
ROM_PATH = PROJECT_ROOT / "totkol.gba"
BIOS_PATH = GBARECOMP_ROOT / "bios" / "gba_bios.bin"

NONE = 0x3FF
BITMAP = {"none": [], "start": [3], "a": [0], "b": [1], "up": [6],
          "down": [7], "left": [5], "right": [4]}


def press(bits: list[int]) -> int:
    v = NONE
    for b in bits:
        v &= ~(1 << b)
    return v & 0x3FF


class JsonClient:
    def __init__(self, host: str, port: int):
        deadline = time.time() + 10.0
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}")
        self.sock.settimeout(30.0)
        self.buf = b""

    def call(self, **kwargs):
        self.sock.sendall((json.dumps(kwargs) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("peer closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19871)
    ap.add_argument("--reps", type=int, default=30,
                    help="Repetitions of the A/B/direction sequence")
    ap.add_argument("--abort-at", type=int, default=0,
                    help="Set GBARECOMP_CALL_STACK_ABORT_AT for a fast, "
                         "dense repro (0 = use the real 65536 limit)")
    args = ap.parse_args()

    if not NATIVE_EXE.exists():
        print(f"missing {NATIVE_EXE} (cmake --build build --target "
              f"KnightOfLodisRecomp first)", file=sys.stderr)
        return 1

    env = {"GBARECOMP_HANG_WATCHDOG": "0", "PATH": "/usr/bin:/bin"}
    if args.abort_at:
        env["GBARECOMP_CALL_STACK_ABORT_AT"] = str(args.abort_at)

    proc = subprocess.Popen(
        [str(NATIVE_EXE), "--rom", str(ROM_PATH), "--bios", str(BIOS_PATH),
         "--tcp", str(args.port)],
        cwd=str(PROJECT_ROOT), env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)
    c = JsonClient("127.0.0.1", args.port)
    print(c.call(cmd="ping"))
    c.call(cmd="run_frames", n=200)
    total = 200

    # Phase 1: mash Start to get past the title screen.
    for _ in range(60):
        c.call(cmd="run_frames", n=6, keyinput=press(BITMAP["start"]))
        c.call(cmd="run_frames", n=20, keyinput=NONE)
        total += 26

    # Phase 2: repeating A/B/direction sequence into menu/gameplay code.
    seq = ["a", "a", "none", "down", "none", "a", "up", "none", "a", "a",
           "none", "b", "none", "left", "none", "right", "none", "a",
           "none", "start", "none"]
    prev_entries: list[int] = []
    try:
        for rep in range(args.reps):
            for name in seq:
                c.call(cmd="run_frames", n=5, keyinput=press(BITMAP[name]))
                c.call(cmd="run_frames", n=8, keyinput=NONE)
                total += 13
                dump = c.call(cmd="call_stack_dump")
                entries = dump["entries"]
                if len(entries) > len(prev_entries):
                    new = entries[len(prev_entries):]
                    print(f"frame={total:6d} name={name:6s} "
                          f"depth={len(entries):3d} "
                          f"NEW_ENTRIES={[hex(x) for x in new]}", flush=True)
                elif len(entries) < len(prev_entries):
                    print(f"frame={total:6d} name={name:6s} "
                          f"depth={len(entries):3d} (shrank)", flush=True)
                prev_entries = entries
    except (RuntimeError, ConnectionResetError, BrokenPipeError, OSError) as e:
        print(f"connection lost at frame={total}: {e} "
              f"(this is the crash reproducing)")
    finally:
        try:
            c.sock.close()
        except Exception:
            pass
        ret = proc.poll()
        print(f"process poll: {ret} (final frame ~{total})")
        if ret is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
