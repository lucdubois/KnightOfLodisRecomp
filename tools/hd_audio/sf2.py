"""Minimal SoundFont 2 reader (pure Python + numpy), used by build_bank.py.

    bank = SF2(path)
    bank.presets()                       -> [(bank, program, name), ...]
    bank.zones(bank_no, program, key, velocity=100) -> [zone dict, ...]

Each zone dict:
    data          float32 array in [-1, 1], the sample's frames (start..end
                  after the address-offset generators)
    rate          sample rate (Hz)
    root          overridingRootKey (gen 58) if set, else shdr originalPitch
    tune_cents    coarseTune*100 + fineTune + shdr pitchCorrection
    scale_tuning  gen 56 (cents per key, default 100)
    loop_start, loop_end   frame indices within `data` (loop_end exclusive,
                  as in SF2: the loop is data[loop_start:loop_end])
    loop_mode     gen 54: 0 none, 1 continuous, 3 loop until release
    pan           -1..1 (gen 17 / 500)
    atten_db      initialAttenuation (gen 48, cB) / 10
    sample_start, sample_end   the frames' absolute span in the smpl chunk
                  (raw int16 at SF2.raw(start, end))
    plus informational keys: preset_name, inst_name, sample_name,
    sample_type, key_range, vel_range, exclusive_class, gens (the merged
    generator dict: instrument value + preset offset, by generator number).

Modulators and envelopes are not applied (gens holds the envelope
generators raw, in timecents / cB, if wanted).

Helper: playback_ratio(zone, key, out_rate) -> source frames per output frame.
"""

import mmap
import struct

import numpy as np

# generator numbers
G_START, G_END, G_SLOOP, G_ELOOP, G_START_C = 0, 1, 2, 3, 4
G_END_C = 12
G_PAN = 17
G_INSTRUMENT = 41
G_KEYRANGE, G_VELRANGE = 43, 44
G_SLOOP_C = 45
G_KEYNUM, G_VELOCITY = 46, 47
G_ATTEN = 48
G_ELOOP_C = 50
G_COARSE, G_FINE = 51, 52
G_SAMPLEID = 53
G_MODES = 54
G_SCALE = 56
G_EXCL = 57
G_ROOT = 58

_RANGE_GENS = (G_KEYRANGE, G_VELRANGE)
_UNSIGNED_GENS = (G_INSTRUMENT, G_SAMPLEID, G_MODES)
# generators that are not valid at preset level (spec 8.1.3)
_INST_ONLY = {G_START, G_END, G_SLOOP, G_ELOOP, G_START_C, G_END_C,
              G_SLOOP_C, G_KEYNUM, G_VELOCITY, G_ELOOP_C, G_SAMPLEID,
              G_MODES, G_EXCL, G_ROOT}

# spec defaults for the generators we read
_DEFAULTS = {G_PAN: 0, G_ATTEN: 0, G_COARSE: 0, G_FINE: 0, G_MODES: 0,
             G_SCALE: 100, G_EXCL: 0, G_ROOT: -1, G_KEYNUM: -1,
             G_VELOCITY: -1,
             G_START: 0, G_END: 0, G_SLOOP: 0, G_ELOOP: 0, G_START_C: 0,
             G_END_C: 0, G_SLOOP_C: 0, G_ELOOP_C: 0,
             # envelopes (timecents / cB), informational
             33: -12000, 34: -12000, 35: -12000, 36: -12000, 37: 0,
             38: -12000}


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1").strip()


class SF2:
    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        self._mm = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        mm = self._mm
        if mm[0:4] != b"RIFF" or mm[8:12] != b"sfbk":
            raise ValueError("not an SF2 file")
        self._chunks = {}
        self._walk(12, len(mm))
        smpl_off, smpl_len = self._chunks[b"smpl"]
        self._smpl = np.frombuffer(mm, dtype="<i2", count=smpl_len // 2,
                                   offset=smpl_off)
        self._parse_pdta()

    # ---- RIFF -----------------------------------------------------------
    def _walk(self, pos, end):
        mm = self._mm
        while pos + 8 <= end:
            cid = mm[pos:pos + 4]
            size = struct.unpack_from("<I", mm, pos + 4)[0]
            if cid == b"LIST":
                self._walk(pos + 12, pos + 8 + size)
            else:
                self._chunks[cid] = (pos + 8, size)
            pos += 8 + size + (size & 1)

    def _records(self, cid, fmt):
        off, size = self._chunks[cid]
        n = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, self._mm, off + i * n)
                for i in range(size // n)]

    def _gens(self, cid):
        off, size = self._chunks[cid]
        out = []
        for i in range(size // 4):
            op = struct.unpack_from("<H", self._mm, off + i * 4)[0]
            if op in _RANGE_GENS:
                lo, hi = struct.unpack_from("<BB", self._mm, off + i * 4 + 2)
                val = (lo, hi)
            elif op in _UNSIGNED_GENS:
                val = struct.unpack_from("<H", self._mm, off + i * 4 + 2)[0]
            else:
                val = struct.unpack_from("<h", self._mm, off + i * 4 + 2)[0]
            out.append((op, val))
        return out

    def _zones_of(self, bag_lo, bag_hi, bags, gens, terminal):
        """Split a preset/instrument's bags into (global, [zones])."""
        glob, zones = {}, []
        for b in range(bag_lo, bag_hi):
            g0, g1 = bags[b][0], bags[b + 1][0]
            z = {}
            for op, val in gens[g0:g1]:
                z[op] = val
            if terminal in z:
                zones.append(z)
            elif b == bag_lo:
                glob = z
            # a non-first zone without a terminal generator is ignored
        return glob, zones

    def _parse_pdta(self):
        phdr = self._records(b"phdr", "<20sHHHIII")
        pbag = self._records(b"pbag", "<HH")
        pgen = self._gens(b"pgen")
        inst = self._records(b"inst", "<20sH")
        ibag = self._records(b"ibag", "<HH")
        igen = self._gens(b"igen")
        self._shdr = [dict(name=_cstr(r[0]), start=r[1], end=r[2],
                           sloop=r[3], eloop=r[4], rate=r[5], pitch=r[6],
                           corr=r[7], link=r[8], type=r[9])
                      for r in self._records(b"shdr", "<20sIIIIIBbHH")]
        self._insts = []
        for i in range(len(inst) - 1):            # last is the EOI terminal
            g, zs = self._zones_of(inst[i][1], inst[i + 1][1], ibag, igen,
                                   G_SAMPLEID)
            self._insts.append((_cstr(inst[i][0]), g, zs))
        self._presets = {}
        for i in range(len(phdr) - 1):            # last is the EOP terminal
            name, prog, bnk, bag = phdr[i][0], phdr[i][1], phdr[i][2], phdr[i][3]
            g, zs = self._zones_of(bag, phdr[i + 1][3], pbag, pgen,
                                   G_INSTRUMENT)
            self._presets[(bnk, prog)] = (_cstr(name), g, zs)

    # ---- API ------------------------------------------------------------
    def raw(self, start, end):
        """The smpl chunk's int16 frames [start, end)."""
        return self._smpl[start:end]

    def presets(self):
        return sorted((b, p, v[0]) for (b, p), v in self._presets.items())

    def zones(self, bank_no, program, key, velocity=100):
        try:
            pname, pglob, pzones = self._presets[(bank_no, program)]
        except KeyError:
            raise KeyError("no preset bank %d program %d" % (bank_no, program))
        out = []
        for pz in pzones:
            pg = dict(pglob)
            pg.update(pz)
            if not _in_range(pg, key, velocity):
                continue
            iname, iglob, izones = self._insts[pz[G_INSTRUMENT]]
            for iz in izones:
                ig = dict(_DEFAULTS)
                ig.update(iglob)
                ig.update(iz)
                if not _in_range(ig, key, velocity):
                    continue
                out.append(self._make_zone(pname, iname, pg, ig))
        return out

    def _make_zone(self, pname, iname, pg, ig):
        merged = dict(ig)
        for op, val in pg.items():               # preset gens are offsets
            if op in _INST_ONLY or op in _RANGE_GENS or op == G_INSTRUMENT:
                continue
            merged[op] = merged.get(op, _DEFAULTS.get(op, 0)) + val
        sh = self._shdr[ig[G_SAMPLEID]]
        start = sh["start"] + ig[G_START] + 32768 * ig[G_START_C]
        end = sh["end"] + ig[G_END] + 32768 * ig[G_END_C]
        sloop = sh["sloop"] + ig[G_SLOOP] + 32768 * ig[G_SLOOP_C]
        eloop = sh["eloop"] + ig[G_ELOOP] + 32768 * ig[G_ELOOP_C]
        data = self._smpl[start:end].astype(np.float32) / 32768.0
        root = ig[G_ROOT] if ig[G_ROOT] >= 0 else sh["pitch"]
        if root > 127:                            # 255 = unpitched -> 60
            root = 60
        kr = _intersect(pg.get(G_KEYRANGE, (0, 127)), ig.get(G_KEYRANGE, (0, 127)))
        vr = _intersect(pg.get(G_VELRANGE, (0, 127)), ig.get(G_VELRANGE, (0, 127)))
        return dict(
            data=data,
            sample_start=int(start), sample_end=int(end),
            rate=sh["rate"],
            root=int(root),
            tune_cents=int(merged[G_COARSE] * 100 + merged[G_FINE] + sh["corr"]),
            scale_tuning=int(merged[G_SCALE]),
            loop_start=int(sloop - start),
            loop_end=int(eloop - start),
            loop_mode=int(ig[G_MODES] & 3),
            pan=max(-1.0, min(1.0, merged[G_PAN] / 500.0)),
            atten_db=merged[G_ATTEN] / 10.0,
            preset_name=pname, inst_name=iname, sample_name=sh["name"],
            sample_type=sh["type"], key_range=kr, vel_range=vr,
            exclusive_class=int(ig[G_EXCL]),
            gens=merged,
        )


def _in_range(g, key, vel):
    klo, khi = g.get(G_KEYRANGE, (0, 127))
    vlo, vhi = g.get(G_VELRANGE, (0, 127))
    return klo <= key <= khi and vlo <= vel <= vhi


def _intersect(a, b):
    return (max(a[0], b[0]), min(a[1], b[1]))


def zone_cents(zone, key):
    """Pitch offset (cents) of `key` relative to the sample's natural pitch."""
    return (key - zone["root"]) * zone["scale_tuning"] + zone["tune_cents"]


def playback_ratio(zone, key, out_rate):
    """Source frames to advance per output frame when playing `key`."""
    return zone["rate"] / float(out_rate) * 2.0 ** (zone_cents(zone, key) / 1200.0)


def render(zone, key, seconds, out_rate=44100):
    """Plain resampled (linear interp) render of a zone, looping per
    loop_mode as if the key were held for `seconds`. No envelope, no pan,
    attenuation applied. Mono float32."""
    data = zone["data"]
    step = playback_ratio(zone, key, out_rate)
    n = int(seconds * out_rate)
    pos = np.arange(n, dtype=np.float64) * step
    ls, le = zone["loop_start"], zone["loop_end"]
    if zone["loop_mode"] in (1, 3) and 0 <= ls < le <= len(data) and le - ls > 1:
        over = pos >= le
        pos[over] = ls + np.mod(pos[over] - ls, le - ls)
    else:
        pos = pos[pos < len(data) - 1]
    i = pos.astype(np.int64)
    f = (pos - i).astype(np.float32)
    nxt = np.minimum(i + 1, len(data) - 1)
    if zone["loop_mode"] in (1, 3) and le - ls > 1:
        nxt = np.where(nxt >= le, ls, nxt)
    y = data[i] * (1 - f) + data[nxt] * f
    out = np.zeros(n, np.float32)
    out[:len(y)] = y
    return out * np.float32(10 ** (-zone["atten_db"] / 20.0))
