import socket, json, sys, struct, wave, time, subprocess, os
KOL=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT=os.path.dirname(os.path.abspath(__file__))
state=sys.argv[1]; tag=sys.argv[2]; frames=int(sys.argv[3]) if len(sys.argv)>3 else 300
port=19870
env=dict(os.environ, GBARECOMP_SELFHEAL_RECOMPILE="0")
env.update(dict(a.split("=",1) for a in sys.argv[4:]))
p=subprocess.Popen([KOL+"/build/KnightOfLodisRecomp","--rom",KOL+"/totkol.gba","--bios",KOL+"/../gbarecomp/bios/gba_bios.bin","--no-window","--quiet","--tcp",str(port)],cwd=KOL,env=env,stdout=subprocess.DEVNULL,stderr=open(OUT+f"/{tag}.stderr","w"))
for i in range(100):
    try: s=socket.create_connection(("127.0.0.1",port)); break
    except OSError: time.sleep(0.2)
f=s.makefile("rwb")
def cmd(o):
    f.write((json.dumps(o)+"\n").encode()); f.flush()
    return json.loads(f.readline())
print(cmd({"cmd":"savestate_load","path":KOL+"/states/"+state}))
for i in range(frames): cmd({"cmd":"step"})
m=cmd({"cmd":"m4a_dump"})
print({k:v for k,v in m.items() if k!="channels"})
for c in m.get("channels",[]):
    if c["active"]: print(" ch",c["ch"],"type",c["type"],"freq",c["freq"],"vol",c["vol_l"],c["vol_r"],"wav",hex(c["wav"]),"size",c["wav_size"],"loop",c["loop"])
io=cmd({"cmd":"read_io","addr":0x04000080,"len":12})
print("io 080..08B", io.get("data") or io)
cap=cmd({"cmd":"audio_cap","count":262144})
print("rate",cap["rate"],"n",cap["count"])
def dec(h): b=bytes.fromhex(h); return struct.unpack("<%dh"%(len(b)//2),b)
for k in ["mixed","direct_a","direct_b","ch1","ch2","ch3","ch4"]:
    d=dec(cap[k]); print(k,"peak",max(abs(x) for x in d) if d else 0)
    if k=="mixed":
        w=wave.open(OUT+f"/{tag}_mixed.wav","wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(cap["rate"]); w.writeframes(bytes.fromhex(cap[k])); w.close()
    if k in("direct_a","direct_b"):
        w=wave.open(OUT+f"/{tag}_{k}.wav","wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(cap["rate"]); w.writeframes(struct.pack("<%dh"%len(d),*[x*256 for x in d])); w.close()
p.kill()
