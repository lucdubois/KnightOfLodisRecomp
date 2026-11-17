# Three listening files per song, all 48 kHz 16-bit:
#   1_now        what the Thor plays today (mono canon mix, resampled)
#   2_fixes      same engine, accuracy fixes only: stereo PSG routing, PSG level per
#                mGBA (-3.9 dB), FIFO through a 7.5 kHz reconstruction low-pass,
#                PSG band-limited (12 kHz low-pass stands in for blip_buf synthesis)
#   3_rerender   MP2K voices re-rendered from the driver's state (float, Lanczos,
#                smooth envelopes, song pan) + the fixed stereo PSG of 2
import numpy as np, pickle, os, sys, wave
from scipy.signal import resample_poly, butter, sosfiltfilt
HERE=os.path.dirname(os.path.abspath(__file__)); R=48000; FPS=16777216/280896.0
def to48(x): return resample_poly(x, 375, 512)
def save(path, x):
    x=np.asarray(x); x=x[:,None] if x.ndim==1 else x
    pk=np.max(np.abs(x)); 
    if pk>32000: x=x*(32000/pk)
    w=wave.open(path,"wb"); w.setnchannels(x.shape[1]); w.setsampwidth(2); w.setframerate(R)
    w.writeframes(np.round(x).astype("<i2").tobytes()); w.close()
lp12=butter(8,12000,fs=R,output="sos"); lp75=butter(8,7500,fs=R,output="sos")
for tag in sys.argv[1:]:
    D=pickle.load(open(f"{HERE}/{tag}.pkl","rb")); Z=np.load(f"{HERE}/out/{tag}.npz")
    NT=len(D["snaps"]); n65=int(round(NT*65536/FPS))
    tap={k:np.frombuffer(v,dtype=np.int16).astype(float)[-n65:] for k,v in D["taps"].items()}
    # per-tick SOUNDCNT_L routing + master volumes, held over each frame's samples
    spf=65536/FPS; idx=np.minimum((np.arange(n65)/spf).astype(int),NT-1)
    cntl=np.array([s["io"][0x20]|(s["io"][0x21]<<8) for s in D["snaps"]])[idx]
    vr=((cntl&7)+1)/8.0; vl=(((cntl>>4)&7)+1)/8.0
    L=np.zeros(n65); Rr=np.zeros(n65)
    for i in range(4):
        L+=tap[f"ch{i+1}"]*((cntl>>(12+i))&1); Rr+=tap[f"ch{i+1}"]*((cntl>>(8+i))&1)
    k=384.0          # hardware: one PSG unit = 2 FIFO bytes; a FIFO byte is 192 here
    psgL=to48(L*vl*k); psgR=to48(Rr*vr*k)
    psgL=sosfiltfilt(lp12,psgL); psgR=sosfiltfilt(lp12,psgR)
    fifo=sosfiltfilt(lp75,to48(tap["direct_a"]*192.0))
    n=min(len(psgL),len(Z["canon"]))
    fixes=np.stack([fifo[:n]+psgL[:n], fifo[:n]+psgR[:n]],1)
    direct_s=Z["hq_s"][:n]-Z["psg"][:n,None]
    rer=direct_s+np.stack([psgL[:n],psgR[:n]],1)
    save(f"{HERE}/out/{tag}_1_now.wav",Z["canon"][:n]); save(f"{HERE}/out/{tag}_2_fixes.wav",fixes); save(f"{HERE}/out/{tag}_3_rerender.wav",rer)
    m=lambda x: 20*np.log10(np.sqrt(np.mean(np.asarray(x)**2)))
    print(f"{tag}: rms dBFS-ish now {m(Z['canon'][:n]):.1f}  fixes {m(fixes):.1f}  rerender {m(rer):.1f}; PSG share now {m(Z['psg'][:n])-m(Z['canon'][:n]):.1f} dB -> fixed {m(np.stack([psgL,psgR],1))-m(rer):.1f} dB")
    for f in ("2_hq_mono","3_hq_stereo","1b_now_lowpassed"):
        p=f"{HERE}/out/{tag}_{f}.wav"
        if os.path.exists(p): os.remove(p)
