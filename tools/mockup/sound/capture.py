# Capture MP2K SoundInfo+channels every frame, plus the canon mixed stream.
import socket, json, sys, struct, time, subprocess, os, pickle
KOL=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT=os.path.dirname(os.path.abspath(__file__))
state, tag, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
keys = sys.argv[4] if len(sys.argv)>4 else ""   # e.g. "60:3FE,62:3FF"
port=int(os.environ.get("CAP_PORT","19871"))
env=dict(os.environ, GBARECOMP_SELFHEAL_RECOMPILE="0")
p=subprocess.Popen([KOL+"/build/KnightOfLodisRecomp","--rom",KOL+"/totkol.gba","--bios",KOL+"/../gbarecomp/bios/gba_bios.bin","--no-window","--quiet","--tcp",str(port)],cwd=KOL,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
for i in range(100):
    try: s=socket.create_connection(("127.0.0.1",port)); break
    except OSError: time.sleep(0.2)
f=s.makefile("rwb")
def cmd(o):
    f.write((json.dumps(o)+"\n").encode()); f.flush()
    return json.loads(f.readline())
if state!="-": cmd({"cmd":"savestate_load","path":KOL+"/states/"+state})
keymap={int(a):int(b,16) for a,b in (k.split(":") for k in keys.split(",") if k)}
snaps=[]; taps={}; canon=bytearray(); nxt=None
for fr in range(frames):
    if fr in keymap: cmd({"cmd":"set_keyinput","value":keymap[fr]})
    cmd({"cmd":"step"})
    si=int(cmd({"cmd":"read_iwram","addr":0x03007FF0,"len":4})["data"],16)
    si=struct.unpack("<I",bytes.fromhex(format(si,'08x')))[0]
    blk=b""
    if si>>24==2:
        blk=bytes.fromhex(cmd({"cmd":"read_ewram","addr":si,"len":0x350+0x630*2})["data"])
    cgb=b""
    if len(blk)>=0x20:
        cp=struct.unpack_from("<I",blk,0x1C)[0]
        if cp>>24==3: cgb=bytes.fromhex(cmd({"cmd":"read_iwram","addr":cp,"len":4*0x40})["data"])
        elif cp>>24==2: cgb=bytes.fromhex(cmd({"cmd":"read_ewram","addr":cp,"len":4*0x40})["data"])
    io=bytes.fromhex(cmd({"cmd":"read_io","addr":0x04000060,"len":0x30})["data"])
    if fr%50==49 or fr==frames-1:
        c=cmd({"cmd":"audio_cap","count":262144} if nxt is None else {"cmd":"audio_cap","start":nxt,"count":262144})
        canon+=bytes.fromhex(c["mixed"]); [taps.setdefault(k,bytearray()).extend(bytes.fromhex(c[k])) for k in ("ch1","ch2","ch3","ch4","direct_a","direct_b","left","right") if k in c]; 
        if nxt is None: first=c["first"]
        nxt=c["first"]+c["count"]; rate=c["rate"]
    head=None
    snaps.append(dict(si=si,blk=blk,cgb=cgb,io=io))
# canon covers [first, nxt); snapshot k taken after step k
pickle.dump(dict(snaps=snaps,canon=bytes(canon),taps={k:bytes(v) for k,v in taps.items()},canon_first=first,canon_end=nxt,rate=rate),open(OUT+f"/{tag}.pkl","wb"))
print(tag,"frames",frames,"canon samples",len(canon)//2,"rate",rate)
p.kill()
