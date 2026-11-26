#!/usr/bin/env python3
"""Independent AES/H264 + resource/liveness checks of a recorded legacy bench."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import subprocess
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ap=argparse.ArgumentParser()
ap.add_argument("capture",type=Path)
ap.add_argument("--min-frames",type=int,default=20)
ap.add_argument("--ffmpeg",default="ffmpeg")
args=ap.parse_args()
trace=json.loads((args.capture/"trace.json").read_text())
assert not trace["errors"],trace["errors"]
assert trace["alive"] and trace["connections"]==1, "session did not survive"
assert len(trace["frames"])>=args.min_frames, "too few captured frames"
assert trace["keepalives"]>=20 and trace["udp_pings"]>=20, "liveness signals missing"
feedback=[r for r in trace["requests"] if r["line"].startswith("POST /feedback ")]
assert len(feedback)>=20,"control feedback missing"
info_request=next(r for r in trace["requests"] if r["line"].startswith("GET /info "))
assert trace["first_udp"] is not None and trace["first_udp"]<=info_request["time"]+1,"idle keepalive started after info/display work"
events=trace["events"]
permanent_events=events[5:]
events=events[:5]
assert [e["label"] for e in events]==["audio borrow","audio return","screen borrow","screen return","unsupported permanent take"]
for e in events:
    assert e["reply"]["status"]==(1 if e is events[-1] else 0),e
    if e is events[-1]:continue
    resources={r["resourceID"]:r for r in e["reply"]["params"]["resources"]}
    if trace.get("services"):
        assert resources[2]["permanentEntity"]==1
        assert resources[2]["entity"]==(2 if e["label"]=="audio borrow" else 1)
    else:
        assert resources[2]["entity"]==2 and resources[2]["permanentEntity"]==2,"disabled audio falsely acquired"
    assert resources[1]["permanentEntity"]==1
    assert resources[1]["entity"]==(2 if e["label"]=="screen borrow" else 1)
borrow,returned=events[2]["time"],events[3].get("sent_at",events[3]["time"])
assert not [f for f in trace["frames"] if borrow+.3<f["time"]<returned-.1],"video sent while screen borrowed"
assert not [f for f in trace["configs"] if borrow+.3<f["time"]<returned-.1],"avcC sent while screen borrowed"
notifications=[r for r in trace["requests"] if isinstance(r["body"],dict) and r["body"].get("type")=="modesChanged"]
assert len(notifications)>=5 if trace.get("services") or trace.get("permanent_modes") else len(notifications)==5,notifications
resume_times=[returned]
if trace.get("permanent_modes"):
    assert [e["label"] for e in permanent_events]==["permanent take","permanent release","late permanent release"]
    audio_owner=1 if trace.get("services") else 0
    for event,owners in zip(permanent_events,[(2,2),(0,0),(1,audio_owner)]):
        assert event["reply"]["status"]==0,event
        resources={r["resourceID"]:r for r in event["reply"]["params"]["resources"]}
        for i,owner in enumerate(owners,1):
            assert resources[i]["entity"]==owner and resources[i]["permanentEntity"]==owner,event
    taken=permanent_events[0]["time"]
    released=permanent_events[1].get("sent_at",permanent_events[1]["time"])
    for kind in ("frames","configs"):
        assert not [f for f in trace[kind] if taken+.3<f["time"]<released-.1],kind
    tail=[r["body"]["params"]["resources"] for r in notifications[-4:]]
    assert [[(r["entity"],r["permanentEntity"]) for r in resources] for resources in tail]==[
        [(2,2),(2,2)],[(0,0),(0,0)],[(1,1),(audio_owner,audio_owner)],[(1,1),(audio_owner,audio_owner)]],tail
    resume_times.append(released)
id=str(trace["connection_id"]).encode()
key=hashlib.sha512(b"AirPlayStreamKey"+id+bytes(16)).digest()[:16]
iv=hashlib.sha512(b"AirPlayStreamIV"+id+bytes(16)).digest()[:16]
decrypt=Cipher(algorithms.AES(key),modes.CTR(iv)).decryptor()
wire=(args.capture/"screen.wire").read_bytes()
decoded=args.capture/"screen.h264"
position=0;frames=0;configs=0;types_by_offset={}
with decoded.open("wb") as out:
    while position<len(wire):
        header=wire[position:position+128];assert len(header)==128
        size=struct.unpack_from("<I",header)[0]
        body=wire[position+128:position+128+size];assert len(body)==size
        if header[4]==1:
            assert header[6]==4 and body[0]==1, "wrong legacy configuration"
            width,height=struct.unpack_from("<ff",header,16); assert width>0 and height>0
            p=6
            for _ in range(body[5]&31):
                n=int.from_bytes(body[p:p+2],"big");p+=2
                assert n and p+n<=len(body);out.write(b"\0\0\0\1"+body[p:p+n]);p+=n
            count=body[p];p+=1
            for _ in range(count):
                n=int.from_bytes(body[p:p+2],"big");p+=2
                assert n and p+n<=len(body);out.write(b"\0\0\0\1"+body[p:p+n]);p+=n
            assert p==len(body);configs+=1
        elif header[4]==0:
            plain=decrypt.update(body);p=0;naltypes=[]
            while p<len(plain):
                assert p+4<=len(plain)
                n=int.from_bytes(plain[p:p+4],"big");p+=4
                assert n and p+n<=len(plain),"AES stream broken or invalid H264 NAL length"
                assert not plain[p]&0x80 and 0<plain[p]&31<24
                naltypes.append(plain[p]&31);out.write(b"\0\0\0\1"+plain[p:p+n]);p+=n
            types_by_offset[position]=naltypes;frames+=1
        elif header[4]==5:
            assert plistlib.loads(body)=={} and struct.unpack_from("<f",header,124)[0]==42
        else:assert header[4]==2 and size==0
        position+=128+size
for returned in resume_times:
    resumed=next(f for f in trace["frames"] if f["time"]>=returned)
    assert 5 in types_by_offset[resumed["offset"]],"first resumed picture was not an IDR"
result=subprocess.run([args.ffmpeg,"-v","error","-xerror","-i",str(decoded),"-f","null","-"],capture_output=True,text=True,timeout=90)
(args.capture/"decode.log").write_text(result.stderr)
assert result.returncode==0,result.stderr
summary={"frames":frames,"configs":configs,"udp_pings":trace["udp_pings"],
         "screen_keepalives":trace["keepalives"],"feedback":len(feedback),
         "events":len(trace["events"]),"permanent_modes":bool(trace.get("permanent_modes")),"continuous_aes":True,"resumed_with_idr":True,
         "ffmpeg_decode":"PASS","sha256":hashlib.sha256(wire).hexdigest()}
(args.capture/"validation.json").write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
