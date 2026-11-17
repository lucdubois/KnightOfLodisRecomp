# Integer MP2K tick model vs the PCM buffer segment the driver wrote.
import pickle, struct, sys, os
import numpy as np
ROM=open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "totkol.gba"),"rb").read()
u32=lambda b,o: struct.unpack_from("<I",b,o)[0]
def wav_hdr(w):
    o=w-0x08000000; flags=struct.unpack_from("<H",ROM,o+2)[0]
    return dict(loop=bool(flags&0xC000), ls=u32(ROM,o+8), size=u32(ROM,o+12), data=np.frombuffer(ROM[o+16:o+16+u32(ROM,o+12)+1],dtype=np.int8).astype(np.int64))
H={}
def wav(w):
    if w not in H: H[w]=wav_hdr(w)
    return H[w]
def chans(b):
    out=[]
    for ch in range(12):
        c=b[0x50+ch*0x40:0x90+ch*0x40]
        st,ty=c[0],c[1]
        if st&0xC7==0 or ty&7: out.append(None); continue
        cnt,fw,fq,w,cur=struct.unpack_from("<IIIII",c,0x18)
        out.append(dict(st=st,ty=ty,vr=c[2],vl=c[3],env=c[9],er=c[10],el=c[11],cnt=cnt,fw=fw,fq=fq,wav=w,cur=cur))
    return out
def segment(b,k_counter):
    spv=u32(b,0x10); per=b[0x0B]
    return None
if __name__=="__main__":
    tag=sys.argv[1]; D=pickle.load(open(tag+".pkl","rb")); S=D["snaps"]
    b0=S[0]["blk"]; spv=u32(b0,0x10); per=b0[0x0B]
    buf=lambda b,j: np.frombuffer(b[0x350+j*spv:0x350+(j+1)*spv],dtype=np.int8).astype(np.int64)
    # which segment changed between consecutive snapshots?
    hist={}
    for k in range(1,60):
        ch=[j for j in range(per) if not np.array_equal(buf(S[k]["blk"],j),buf(S[k-1]["blk"],j))]
        key=(S[k]["blk"][4], tuple(ch)); hist[key]=hist.get(key,0)+1
    print("(dmaCounter after tick, segments changed):", sorted(hist.items()))

def sim_voice(c, prev, divfreq, spv, order):
    """Integer driver simulation of one voice over one tick. Returns (samples int array, end idx, end fw, ended_at)."""
    w=wav(c["wav"] if c else prev["wav"]); d=w["data"]; size=w["size"]
    src = c if c else prev
    fixed = bool(src["ty"]&8)
    step = 1<<23 if fixed else (src["fq"]*divfreq) & 0xFFFFFFFF
    new = c is not None and (prev is None or prev["wav"]!=c["wav"] or is_restart(c,prev,step,spv,size))
    if new: idx, fw = 0, 0
    else: idx, fw = size-prev["cnt"], (0 if fixed else prev["fw"])
    out=np.zeros(spv,dtype=np.int64); ended=None
    for n in range(spv):
        if idx>=size:
            if w["loop"]: idx = w["ls"] + (idx-size)
            else: ended=n; break
        if fixed: s=d[idx]
        else:
            if order==0: f=fw
            s0=d[idx]; s1=d[idx+1] if idx+1<len(d) else d[idx]
            s = s0 + (((s1-s0)*fw)>>23)
        out[n]=s
        fw += step; idx += fw>>23; fw &= 0x7FFFFF
    return out, idx, fw, ended, new

def is_restart(c,prev,step,spv,size):
    adv=(step*spv)>>23
    pe=size-c["cnt"]; pp=size-prev["cnt"]
    return pe <= pp and abs(pe-adv) <= 2

def tick_model(Sk, Sp, spv, divfreq, wrap=True, order=0):
    ck, cp = chans(Sk), chans(Sp)
    acc=np.zeros(spv,dtype=np.int64); ok_pos=[]; info=[]
    for ch in range(12):
        c, p = ck[ch], cp[ch]
        if c is None and p is None: continue
        if c is None:
            # voice gone: did a one-shot sample run out during this tick?
            w=wav(p["wav"]); step=1<<23 if p["ty"]&8 else p["fq"]*divfreq
            if (p["ty"]&8) and not w["loop"] and p["cnt"] < ((step*spv)>>23)+2:
                s,_,_,ended,_=sim_voice(None,p,divfreq,spv,order)
                acc += (s*p["er"])>>8; info.append((ch,"tail",ended))
            continue
        s,idx,fw,ended,new=sim_voice(c,p,divfreq,spv,order)
        size=wav(c["wav"])["size"]
        ok_pos.append(bool(idx==size-c["cnt"] and (bool(c["ty"]&8) or fw==c["fw"])))
        acc += (s*c["er"])>>8
        info.append((ch,"new" if new else "cont",ok_pos[-1]))
    if wrap: acc=((acc+128)%256)-128
    else: acc=np.clip(acc,-128,127)
    return acc, ok_pos, info

if __name__=="__main__" and len(sys.argv)>2:
    divfreq=u32(b0,0x18)
    for wrap in (True,False):
        ex=[]; pos=[]; bad=[]
        for k in range(1,len(S)):
            seg=(per+1-S[k]["blk"][4])%per
            m,okp,info=tick_model(S[k]["blk"],S[k-1]["blk"],spv,divfreq,wrap)
            e=np.mean(m==buf(S[k]["blk"],seg)); ex.append(e); pos+=okp
            if e<0.99: bad.append((k,round(e,2),info))
        ex=np.array(ex)
        print(f"wrap={wrap}: samples exact {ex.mean()*100:.2f}%  ticks 100% {np.mean(ex==1)*100:.1f}%  ticks>=99% {np.mean(ex>=.99)*100:.1f}%  positions consistent {np.mean(pos)*100:.2f}%")
    for b in bad[:8]: print("  ",b)
