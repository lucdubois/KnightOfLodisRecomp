# Offline MP2K re-render from per-tick channel snapshots (capture.py).
#   model  : the driver's own 15768 Hz / 8-bit mono mix, to validate the decode
#   hq     : float voices, band-limited interpolation, smooth gains, 48 kHz
import pickle, struct, sys, os, wave
import numpy as np
from scipy.signal import resample_poly, correlate

HERE = os.path.dirname(os.path.abspath(__file__))
ROM = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "totkol.gba"), "rb").read()
tag = sys.argv[1]
D = pickle.load(open(f"{HERE}/{tag}.pkl", "rb"))
snaps = D["snaps"]
NT = len(snaps)
FPS = 16777216 / 280896.0

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def rom_slice(addr, n):
    o = addr - 0x08000000
    return ROM[o:o + n]

_wav = {}
def wave_info(wav):
    if wav not in _wav:
        h = rom_slice(wav, 16)
        flags = struct.unpack_from("<H", h, 2)[0]
        loop_start, size = u32(h, 8), u32(h, 12)
        data = np.frombuffer(rom_slice(wav + 16, size), dtype=np.int8).astype(np.float64)
        _wav[wav] = (data, loop_start, size, bool(flags & 0xC000))
    return _wav[wav]

# ── parse ticks ───────────────────────────────────────────────────────
blk0 = snaps[0]["blk"]
PCMF = u32(blk0, 0x14); SPV = u32(blk0, 0x10)
ticks = []
types_seen = set(); reverbs = set(); maxch = set()
for s in snaps:
    b = s["blk"]
    reverbs.add(b[5] & 0x7F); maxch.add(b[6])
    chans = []
    for ch in range(12):
        c = b[0x50 + ch * 0x40: 0x90 + ch * 0x40]
        st, ty = c[0], c[1]
        if st & 0xC7 == 0 or ty & 7:
            chans.append(None); continue
        types_seen.add(ty)
        cnt, fw, fq, wav, cur = struct.unpack_from("<IIIII", c, 0x18)
        chans.append(dict(st=st, ty=ty, vr=c[2], vl=c[3], env=c[9], er=c[10], el=c[11],
                          cnt=cnt, fw=fw, fq=fq, wav=wav, cur=cur))
    ticks.append(chans)
print(f"{tag}: ticks {NT} pcm_freq {PCMF} spv {SPV} types {sorted(types_seen)} reverb {reverbs} maxch {maxch}")

# ── canon (65536 Hz) aligned: last sample = end of the last step ──────
canon = np.frombuffer(D["canon"], dtype=np.int16).astype(np.float64)
da = np.frombuffer(D["taps"]["direct_a"], dtype=np.int16).astype(np.float64)
n65 = int(round(NT * 65536 / FPS))
canon, da = canon[-n65:], da[-n65:]
psg = canon - da * 192.0          # A on both sides at full volume: byte*4*48

# ── render one variant ────────────────────────────────────────────────
def render(rate, hq, stereo):
    spt = rate / FPS
    n = int(NT * spt) + 64
    out = np.zeros((n, 2))
    for ch in range(12):
        pos = None; prev = None; gprev = None; wavp = None
        for k in range(NT):
            c = ticks[k][ch]
            t0 = int(round(k * spt)); t1 = int(round((k + 1) * spt))
            if c is None:
                if hq and prev is not None and pos is not None:   # 2 ms release, no click
                    data, ls, size, looped = wave_info(prev["wav"])
                    m = int(rate * 0.002)
                    seg = interp(data, ls, size, looped, pos, step, m, hq)
                    fade = np.linspace(1, 0, m)
                    out[t0:t0 + m, 0] += seg * gprev[0] * fade
                    out[t0:t0 + m, 1] += seg * gprev[1] * fade
                pos = None; prev = None; gprev = None
                continue
            data, ls, size, looped = wave_info(c["wav"])
            fixed = bool(c["ty"] & 0x08)
            hz = PCMF if fixed else c["fq"]
            step = hz / rate
            if not hq:                          # the driver's fixed point step
                step = 1.0 if fixed else (c["fq"] * u32(blk0, 0x18)) / 2 ** 23
            pe = size - c["cnt"] + (0 if fixed else c["fw"] / 2 ** 23)
            adv = SPV * hz / PCMF if hq else SPV * step
            new = (prev is None or c["wav"] != prev["wav"] or
                   (pe < prev_pe and abs(pe - adv) < 3))
            if not hq and not new and pos is not None:
                pos = prev_pe                   # exactly where the last tick ended
            elif new or pos is None:
                pos = max(pe - adv, 0.0)
            else:
                # keep our continuous phase, but re-sync if the driver disagrees
                exp = pos
                if looped and exp >= size: exp = ls + (exp - ls) % (size - ls)
                if abs(exp - (pe - adv)) > 2 and not (looped and abs(exp - (pe - adv)) > (size - ls) - 3):
                    pos = pe - adv
            if hq:
                if stereo:
                    g = (c["env"] * c["vl"] / 65536.0, c["env"] * c["vr"] / 65536.0)
                else:
                    gm = c["env"] * (c["vl"] + c["vr"]) / 131072.0
                    g = (gm, gm)
            else:
                g = (c["er"] / 256.0, c["er"] / 256.0)   # the driver's integer gain
            m = t1 - t0
            seg = interp(data, ls, size, looped, pos, step, m, hq)
            if hq and gprev is not None and not new:
                ramp0 = np.linspace(gprev[0], g[0], m, endpoint=False)
                ramp1 = np.linspace(gprev[1], g[1], m, endpoint=False)
            else:
                ramp0 = np.full(m, g[0]); ramp1 = np.full(m, g[1])
            if hq:
                out[t0:t1, 0] += seg * ramp0
                out[t0:t1, 1] += seg * ramp1
            else:                                   # (s * envR) >> 8 per voice
                out[t0:t1, 0] += np.floor(seg * c["er"] / 256.0)
            pos = pos + step * m
            if looped and pos >= size: pos = ls + (pos - ls) % (size - ls)
            prev = c; prev_pe = pe; gprev = g
    return out[:int(NT * spt)]

def interp(data, ls, size, looped, pos, step, m, hq):
    p = pos + step * np.arange(m)
    if looped and size > ls:
        p = np.where(p >= size, ls + (p - ls) % (size - ls), p)
    i = np.floor(p).astype(np.int64); f = p - i
    def at(j):
        if looped and size > ls:
            j = np.where(j >= size, ls + (j - ls) % (size - ls), j)
        j = np.clip(j, 0, size - 1)
        v = data[j]
        return v if looped else np.where((j >= 0) & (j < size), v, 0.0)
    if not hq:          # the driver: s0 + (((s1-s0)*fw) >> 23), integer
        return at(i) + np.floor((at(i + 1) - at(i)) * f)
    # Lanczos-4 (8 taps): band-limited upsampling, sources are <= 31.5 kHz
    acc = np.zeros(m); wsum = np.zeros(m)
    for t in range(-3, 5):
        x = f - t
        w = np.sinc(x) * np.sinc(x / 4)
        acc += at(i + t) * w; wsum += w
    return acc / wsum

# ── 1. validate: the driver model vs the canon FIFO bytes ─────────────
model = render(PCMF, hq=False, stereo=False)[:, 0]
mbytes = np.clip(np.floor(model), -128, 127)
# canon FIFO bytes back on the FIFO timer grid: pick the phase that holds
best = None
for ph in np.arange(0, 4.2, 0.25):
    idx = (np.arange(int(len(da) * PCMF / 65536) - 2) * 65536 / PCMF + ph).astype(int)
    c15 = da[idx]
    chg = np.mean(c15[1:] != c15[:-1])
    if best is None or chg > best[0]: best = (chg, ph, c15)
ca15 = best[2]
L = min(len(ca15), len(mbytes))
xc = correlate(ca15[:L] - ca15[:L].mean(), mbytes[:L] - mbytes[:L].mean(), mode="full", method="fft")
lag = int(np.argmax(xc)) - (L - 1)
a = ca15[max(lag, 0):][:L - abs(lag)]; bb = mbytes[max(-lag, 0):][:L - abs(lag)]
r = np.corrcoef(a, bb)[0, 1]
for dl in range(-3, 4):
    a2 = ca15[max(lag + dl, 0):][:L - abs(lag) - 4]; b2 = mbytes[max(-(lag + dl), 0):][:len(a2)]
    print(f"   lag {lag + dl}: exact {np.mean(a2 == b2) * 100:.1f}%  within1 {np.mean(np.abs(a2 - b2) <= 1) * 100:.1f}%  wrap-exact {np.mean(((a2 - b2) % 256) == 0) * 100:.1f}%")
print(f"model vs canon FIFO: lag {lag} samples @{PCMF} ({lag / PCMF * 1000:.1f} ms), r={r:.4f}, "
      f"exact {np.mean(a == bb) * 100:.1f}%, within1 {np.mean(np.abs(a - bb) <= 1) * 100:.1f}%")
lag_s = lag / PCMF

# ── 2. variants at 48 kHz ─────────────────────────────────────────────
R = 48000
def to48(x65): return resample_poly(x65, 375, 512)   # 48000/65536
psg48 = to48(psg)
canon48 = to48(canon)
def place(x, n):          # shift a render by the measured driver latency
    sh = int(round(lag_s * R))
    y = np.zeros((n,) + x.shape[1:])
    if sh >= 0: y[sh:] = x[:n - sh]
    else: y[:n + sh] = x[-sh:n]
    return y

n48 = len(canon48)
hq_m = place(render(R, hq=True, stereo=False), n48) * 192.0
hq_s = place(render(R, hq=True, stereo=True), n48) * 192.0
mix_m = hq_m[:, 0] + psg48
mix_s = hq_s + psg48[:, None]

# loudness check of the HQ direct part vs canon's direct part
d48 = to48(da * 192.0)
print(f"rms canon direct {np.sqrt(np.mean(d48 ** 2)):.0f}  hq mono {np.sqrt(np.mean(hq_m[:, 0] ** 2)):.0f}  "
      f"corr {np.corrcoef(d48, hq_m[:, 0])[0, 1]:.3f}")
lp = resample_poly(resample_poly(canon, 1, 1), 375, 512)

def save(name, x):
    x = np.asarray(x)
    if x.ndim == 1: x = x[:, None]
    pk = np.max(np.abs(x))
    if pk > 32000: print(f"  {name}: peak {pk:.0f} -> scaled"); x = x * (32000 / pk)
    w = wave.open(f"{HERE}/out/{tag}_{name}.wav", "wb")
    w.setnchannels(x.shape[1]); w.setsampwidth(2); w.setframerate(R)
    w.writeframes(np.round(x).astype("<i2").tobytes()); w.close()

os.makedirs(f"{HERE}/out", exist_ok=True)
save("1_now", canon48)
save("2_hq_mono", mix_m)
save("3_hq_stereo", mix_s)
np.savez(f"{HERE}/out/{tag}.npz", canon=canon48, hq_m=mix_m, hq_s=mix_s, psg=psg48)
print("wrote", f"{HERE}/out/{tag}_*.wav")

# ── diagnostics: where do model and canon disagree? ───────────────────
if os.environ.get("DIAG"):
    a2 = ca15[lag:][:L - lag - 4]; b2 = mbytes[:len(a2)]
    bad = a2 != b2
    ph = np.arange(len(a2)) % SPV
    print("mismatch by position in tick (8 bins):", [f"{bad[(ph >= i*33) & (ph < (i+1)*33)].mean()*100:.0f}" for i in range(8)])
    per_tick = bad[:len(bad)//SPV*SPV].reshape(-1, SPV).mean(1)
    print("ticks fully exact:", np.mean(per_tick == 0)*100, "% ; worst ticks", np.argsort(per_tick)[-8:])
    nact = np.array([sum(c is not None for c in t) for t in ticks])
    print("active voices in worst ticks", nact[np.argsort(per_tick)[-8:]], "; mean exact by nvoices:",
          {int(v): f"{(1-per_tick[nact[:len(per_tick)]==v].mean())*100:.0f}%" for v in np.unique(nact)})
    d = (a2 - b2)[bad]
    print("diff histogram", dict(zip(*np.unique(np.clip(d, -5, 5), return_counts=True))))
if os.environ.get("DIAG"):
    pt = per_tick
    for k in list(np.argsort(pt)[-4:]):
        print(f"tick {k} mismatch {pt[k]*100:.0f}%")
        for kk in (k-1, k):
            print("   ", kk, [(ch, hex(c['st']), c['ty'], c['env'], c['er'], c['cnt'], hex(c['wav'])) for ch, c in enumerate(ticks[kk]) if c])
    # fraction of ticks >50% bad
    print("ticks >50% bad:", np.mean(pt > .5)*100, "%   ticks <5% bad:", np.mean(pt < .05)*100, "%")
if os.environ.get("DIAG"):
    ex = []; lags = []
    for k in range(2, NT - 3):
        m0 = mbytes[k*SPV:(k+1)*SPV]
        best = (-1, 0)
        for dl in range(-8, 9):
            s0 = k*SPV + lag + dl
            cc = ca15[s0:s0+SPV]
            if len(cc) < SPV: continue
            e = np.mean(cc == m0)
            if e > best[0]: best = (e, dl)
        ex.append(best[0]); lags.append(best[1])
    ex = np.array(ex)
    print(f"per-tick best lag: mean exact {ex.mean()*100:.1f}%, ticks >=99% exact {np.mean(ex>=.99)*100:.1f}%, lag offsets {dict(zip(*np.unique(lags, return_counts=True)))}")
    print("worst per-tick-aligned ticks:", [(k+2, f'{ex[k]*100:.0f}') for k in np.argsort(ex)[:6]])
if os.environ.get("DIAG"):
    for k in (700, 701, 400):
        m0 = model[k*SPV:(k+1)*SPV]
        print(k, "model range", m0.min(), m0.max(), [(ch, hex(c['st']), c['ty'], c['env'], c['er'], hex(c['wav'])) for ch, c in enumerate(ticks[k]) if c])
