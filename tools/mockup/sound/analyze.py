import numpy as np, sys, os, wave
from scipy.signal import butter, sosfiltfilt, spectrogram, welch
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
HERE=os.path.dirname(os.path.abspath(__file__)); R=48000
for tag in sys.argv[1:]:
    z=np.load(f"{HERE}/out/{tag}.npz"); now=z["canon"]; hqm=z["hq_m"]; hqs=z["hq_s"]
    # cheap fix: current output through a 7.5 kHz reconstruction low-pass
    sos=butter(8, 7500, fs=R, output="sos"); filt=sosfiltfilt(sos, now)
    w=wave.open(f"{HERE}/out/{tag}_1b_now_lowpassed.wav","wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(R)
    w.writeframes(np.round(np.clip(filt,-32000,32000)).astype("<i2").tobytes()); w.close()
    def band(x, lo, hi):
        f,P=welch(x, R, nperseg=4096); return 10*np.log10(P[(f>=lo)&(f<hi)].sum()/P.sum()+1e-20)
    print(f"== {tag}")
    for name,x in [("now",now),("now_lowpassed",filt),("hq_mono",hqm)]:
        print(f"  {name:14s} energy share 8-16k {band(x,8000,16000):6.1f} dB   16-24k {band(x,16000,24000):6.1f} dB")
    # quantisation floor: residual of now vs its own 7.5k lowpass in quiet frames is images, so instead
    # measure noise between notes: 10 ms frames, quietest 10% (both), level relative to the loud 10%
    def quiet(x):
        fr=len(x)//480; e=np.sqrt((x[:fr*480].reshape(fr,480)**2).mean(1)+1e-9); s=np.sort(e)
        return 20*np.log10(s[int(fr*.1)]/s[int(fr*.9)])
    print(f"  quiet/loud frame ratio: now {quiet(now):.1f} dB, hq {quiet(hqm):.1f} dB")
    lr=hqs[:,0]-hqs[:,1]; print(f"  stereo width: side/mid energy {10*np.log10(np.mean(lr**2)/np.mean((hqs[:,0]+hqs[:,1])**2)):.1f} dB")
    # spectrograms, 4 s
    a=int(4*R); b=int(8*R)
    fig,ax=plt.subplots(3,1,figsize=(12,10),sharex=True)
    for i,(name,x) in enumerate([("NOW (what the Thor plays)",now),("NOW + 7.5 kHz low-pass (cheap fix)",filt),("RE-RENDER (float, sinc, 48 kHz)",hqm)]):
        f,t,S=spectrogram(x[a:b],R,nperseg=1024,noverlap=768)
        ax[i].pcolormesh(t,f/1000,10*np.log10(S+1e-3),shading="auto",vmin=-10,vmax=60,cmap="magma")
        ax[i].set_title(name); ax[i].set_ylabel("kHz"); ax[i].axhline(7.884,color="c",lw=.6,ls="--")
    ax[2].set_xlabel("s"); plt.tight_layout(); plt.savefig(f"{HERE}/out/{tag}_spectrogram.png",dpi=80)
