#!/usr/bin/env python3
"""Build hd_instruments.bank: the soundfont instruments KoL's HD music plays.

    tools/hd_audio/get_soundfont.sh            # once: fetches FluidR3_GM.sf2
    python3 tools/hd_audio/build_bank.py [FluidR3_GM.sf2] [--out PATH]

Reads kol_hd_map.json (every (bank, program) its "samples" and "psg" use),
takes each preset's zones at velocity 100 across keys 0-127 from the
soundfont, and writes the bank file gbarecomp/src/gba/hd_instruments.h
specifies (default build/hd_instruments.bank). Needs numpy.

Per zone: the tuning (coarse, fine, the sample's pitch correction) is folded
into `root`; `gain` is the initial attenuation as FluidSynth applies it
(0.4 x the generator's dB, --atten-factor); the zone keeps its sample's own
rate and int16 frames. Only the velocity-100 layer is kept, and it is stored
for every velocity (vel 0-127): the driver's envelope carries the dynamics.
Per instrument, `rms` is its level at gain 1, as the renderer will play it:
its zones at the middle key of its range (a kit: the mean over the kit keys
the map uses), stereo pairs panned and summed, no envelope, over 10..350 ms.
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from sf2 import SF2  # noqa: E402

VERSION = 1
VELOCITY = 100           # the layer kept (FluidR3 layers velocity)
NEAREST = 24             # keys searched past an instrument's range (HdBank::zones)
OUT_RATE = 48000         # the rate the level is measured at
RMS_FROM, RMS_TO = 0.010, 0.350
INST_FMT = "<HHIIf32s"                # 48 bytes
ZONE_FMT = "<BBBBfIIIIIB3xff"         # 40 bytes
assert struct.calcsize(INST_FMT) == 48 and struct.calcsize(ZONE_FMT) == 40


def wanted(map_path):
    """{(bank, program): set of kit keys} from the map's samples and psg."""
    m = json.load(open(map_path))
    out = {}
    for e in m["samples"] + m["psg"]:
        if int(e["prog"]) < 0:          # the game's own sample: no instrument
            continue
        keys = out.setdefault((int(e.get("bank", 0)), int(e["prog"])), set())
        if e.get("kit_key") is not None:
            keys.add(int(e["kit_key"]))
    return out


def preset_zones(bank, bk, prog, atten_factor, notes):
    """The preset's distinct velocity-100 zones, as bank-file zone dicts."""
    seen = {}
    for key in range(128):
        for z in bank.zones(bk, prog, key, VELOCITY):
            klo, khi = z["key_range"]
            root = z["root"] - z["tune_cents"] / 100.0
            if z["scale_tuning"] != 100:
                # Not representable: the renderer pitches 100 cents a key.
                # Exact at the range's middle key (these are one-key zones).
                mid = (klo + khi) // 2
                cents = (mid - z["root"]) * z["scale_tuning"] + z["tune_cents"]
                root = mid - cents / 100.0
                notes.append("%d:%d key %d-%d %s: scale_tuning %d, root folded at key %d%s" % (
                    bk, prog, klo, khi, z["sample_name"], z["scale_tuning"], mid,
                    "" if klo == khi else " (inexact off it)"))
            length = z["sample_end"] - z["sample_start"]
            ls, le, mode = z["loop_start"], z["loop_end"], z["loop_mode"]
            if not (0 <= ls < le <= length):
                if mode:
                    notes.append("%d:%d %s: loop %d-%d outside %d frames, not looped" % (
                        bk, prog, z["sample_name"], ls, le, length))
                ls, le, mode = 0, 0, 0
            zone = dict(key_lo=klo, key_hi=khi, vel_lo=0, vel_hi=127, root=root,
                        rate=int(z["rate"]), start=z["sample_start"], end=z["sample_end"],
                        loop_start=ls, loop_end=le, loop_mode=mode, pan=float(z["pan"]),
                        gain=10 ** (-atten_factor * z["atten_db"] / 20.0),
                        name=z["sample_name"], preset=z["preset_name"])
            ident = tuple(zone[k] for k in ("key_lo", "key_hi", "root", "rate", "start", "end",
                                            "loop_start", "loop_end", "loop_mode", "pan", "gain"))
            seen.setdefault(ident, zone)
    return sorted(seen.values(), key=lambda z: (z["key_lo"], z["key_hi"], z["pan"], z["start"]))


def zones_at(zones, key):
    """HdBank::zones: the key's zones, else its nearest key's (below first)."""
    for d in range(NEAREST + 1):
        for k in ((key,) if d == 0 else (key - d, key + d)):
            if 0 <= k <= 127:
                hit = [z for z in zones if z["key_lo"] <= k <= z["key_hi"]]
                if hit:
                    return hit
    return []


def render(bank, z, key, n):
    """The zone at `key` for n frames at OUT_RATE: linear, looped, no envelope."""
    data = bank.raw(z["start"], z["end"]).astype(np.float64) / 32768.0
    step = z["rate"] / OUT_RATE * 2.0 ** ((key - z["root"]) / 12.0)
    pos = np.arange(n, dtype=np.float64) * step
    ls, le = z["loop_start"], z["loop_end"]
    looped = z["loop_mode"] in (1, 3)
    if looped:
        over = pos >= le
        pos[over] = ls + np.mod(pos[over] - ls, le - ls)
    else:
        pos = pos[pos < len(data) - 1]
    i = pos.astype(np.int64)
    f = pos - i
    nxt = i + 1
    if looped:
        nxt = np.where(nxt >= le, ls, nxt)
    nxt = np.minimum(nxt, len(data) - 1)
    out = np.zeros(n)
    out[:len(i)] = data[i] * (1 - f) + data[nxt] * f
    return out * z["gain"]


def level(bank, zones, key):
    """RMS over 10..350 ms of the key's zones, panned and summed (the mock-up's
    pan law: cos/sin of (pan+1)pi/4, times sqrt 2, so a centred zone is unity)."""
    n = int(RMS_TO * OUT_RATE)
    left, right = np.zeros(n), np.zeros(n)
    for z in zones_at(zones, key):
        y = render(bank, z, key, n)
        a = (z["pan"] + 1.0) * math.pi / 4.0
        left += y * math.cos(a) * math.sqrt(2.0)
        right += y * math.sin(a) * math.sqrt(2.0)
    a = int(RMS_FROM * OUT_RATE)
    return math.sqrt(float(np.mean((left[a:] ** 2 + right[a:] ** 2) / 2.0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("soundfont", nargs="?", default=os.path.join(HERE, "FluidR3_GM.sf2"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "hd_instruments.bank"))
    ap.add_argument("--map", default=os.path.join(HERE, "kol_hd_map.json"))
    ap.add_argument("--atten-factor", type=float, default=0.4,
                    help="share of initialAttenuation applied (FluidSynth: 0.4)")
    args = ap.parse_args()
    if not os.path.exists(args.soundfont):
        sys.exit("no soundfont at %s (run tools/hd_audio/get_soundfont.sh)" % args.soundfont)

    bank = SF2(args.soundfont)
    notes = []
    insts, zones, chunks = [], [], []
    spans = {}           # (start, end) in the soundfont -> offset in the bank
    n_samples = 0
    report = []
    for (bk, prog), kit_keys in sorted(wanted(args.map).items()):
        zs = preset_zones(bank, bk, prog, args.atten_factor, notes)
        if not zs:
            sys.exit("%d:%d has no zones at velocity %d" % (bk, prog, VELOCITY))
        new_frames = 0
        for z in zs:
            span = (z["start"], z["end"])
            if span not in spans:
                spans[span] = n_samples
                chunks.append(bank.raw(*span))
                n_samples += span[1] - span[0]
                new_frames += span[1] - span[0]
            z["offset"] = spans[span]
        if kit_keys:
            keys = sorted(kit_keys)
            rms = sum(level(bank, zs, k) for k in keys) / len(keys)
        else:
            keys = [(min(z["key_lo"] for z in zs) + max(z["key_hi"] for z in zs)) // 2]
            rms = level(bank, zs, keys[0])
        if not rms > 0:
            sys.exit("%d:%d is silent at key %s" % (bk, prog, keys))
        name = zs[0]["preset"]
        insts.append(struct.pack(INST_FMT, bk, prog, len(zones), len(zs), rms,
                                 name.encode("latin-1")[:31]))
        zones += zs
        report.append((bk, prog, name, len(zs), new_frames, keys, rms))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(b"GBHD" + struct.pack("<IIII", VERSION, len(insts), len(zones), n_samples))
        for b in insts:
            f.write(b)
        for z in zones:
            f.write(struct.pack(ZONE_FMT, z["key_lo"], z["key_hi"], z["vel_lo"], z["vel_hi"],
                                z["root"], z["rate"], z["offset"], z["end"] - z["start"],
                                z["loop_start"], z["loop_end"], z["loop_mode"], z["pan"],
                                z["gain"]))
        for c in chunks:
            f.write(np.ascontiguousarray(c, dtype="<i2").tobytes())

    print("%-9s %-24s %5s %9s  %s" % ("bank:prog", "preset", "zones", "new MB", "rms (at key)"))
    for bk, prog, name, nz, frames, keys, rms in report:
        print("%3d:%-5d %-24s %5d %9.2f  %.4f (%s)" % (
            bk, prog, name, nz, frames * 2 / 1e6, rms, ",".join(map(str, keys))))
    print("(new MB: frames not already in the bank; String Ensembles 1 and 2 share theirs)")
    for n in notes:
        print("note:", n)
    size = os.path.getsize(args.out)
    shown = os.path.relpath(args.out)
    print("%d instruments, %d zones, %d frames: %.2f MB -> %s" % (
        len(insts), len(zones), n_samples, size / 1e6,
        shown if not shown.startswith("..") else os.path.abspath(args.out)))


if __name__ == "__main__":
    main()
