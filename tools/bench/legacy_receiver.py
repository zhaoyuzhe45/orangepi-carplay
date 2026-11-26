#!/usr/bin/env python3
"""Screen-only legacy receiver bench using OrangeCar /info as a fixture.

Stores actual video wire bytes for independent AES/H264 validation. Does not
assert telephony, audio playback or permanent screen transfers are implemented.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import plistlib
import socket
import struct
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("--reference", type=Path,
                default=Path(__file__).resolve().parent / "orangecar" / "carplay_receiver_sim.py",
                help="OrangeCar receiver module; defaults to the bundled copy")
ap.add_argument("--output", required=True, type=Path)
ap.add_argument("--bind", default="0.0.0.0")
ap.add_argument("--port", type=int, default=7011)
ap.add_argument("--seconds", type=int, default=40)
ap.add_argument("--pair-required", action="store_true")
ap.add_argument("--services", action="store_true", help="Exercise media, microphone and appearance")
ap.add_argument("--permanent-modes", action="store_true", help="Exercise valid Take/Untake after Borrow cases")
args = ap.parse_args()
spec = importlib.util.spec_from_file_location("orangecar_receiver", args.reference)
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)
args.output.mkdir(parents=True, exist_ok=True)
start = time.monotonic()
state = {"requests": [], "events": [], "frames": [], "configs": [], "keepalives": 0,
         "udp_pings": 0, "first_udp": None, "errors": [], "connections": 0, "alive": False,
         "services": args.services, "permanent_modes": args.permanent_modes, "audio": [], "night_events": []}
done = threading.Event()
lock = threading.Lock()
active_audio = None

def now(): return round(time.monotonic()-start, 4)
def log(message): print(f"[{now():7.3f}s] {message}", flush=True)
def listen(kind=socket.SOCK_STREAM):
    sock = socket.socket(socket.AF_INET, kind)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, 0))
    if kind==socket.SOCK_STREAM: sock.listen(3)
    sock.settimeout(1)
    return sock

class Channel:
    def __init__(self, sock): self.sock, self.buffer = sock, b""
    def read(self):
        while b"\r\n\r\n" not in self.buffer:
            data=self.sock.recv(65536)
            if not data: return None
            self.buffer+=data
        raw, rest=self.buffer.split(b"\r\n\r\n",1)
        lines=raw.decode("latin1").split("\r\n")
        fields={k.lower():v.strip() for k,v in (line.split(":",1) for line in lines[1:] if ":" in line)}
        size=int(fields.get("content-length",0))
        if size>131072: raise ValueError("oversized RTSP body")
        while len(rest)<size:
            data=self.sock.recv(65536)
            if not data: raise EOFError("partial RTSP body")
            rest+=data
        self.buffer=rest[size:]
        body=plistlib.loads(rest[:size]) if rest[:8]==b"bplist00" else rest[:size]
        return lines[0],fields,body
    def send(self, line, seq, obj=None):
        body=plistlib.dumps(obj,fmt=plistlib.FMT_BINARY) if obj is not None else b""
        self.sock.sendall((f"{line}\r\nCSeq: {seq}\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: {len(body)}\r\n\r\n").encode()+body)

def worker(function):
    def run():
        try: function()
        except (OSError,EOFError) as exc:
            if not done.is_set(): state["errors"].append(f"{function.__name__}: {exc}")
        except Exception as exc: state["errors"].append(f"{function.__name__}: {exc!r}")
    thread=threading.Thread(target=run,daemon=True); thread.start(); return thread

event_listener, data_listener, udp_listener = listen(),listen(),listen(socket.SOCK_DGRAM)
def receive_udp():
    while not done.is_set():
        try: data,_=udp_listener.recvfrom(256)
        except socket.timeout: continue
        if data!=bytes(4): state["errors"].append("invalid UDP keepalive")
        if state["first_udp"] is None: state["first_udp"] = now()
        state["udp_pings"]+=1

def events():
    while not done.is_set():
        try: conn,_=event_listener.accept();break
        except socket.timeout: continue
    else: return
    conn.settimeout(5); channel=Channel(conn)
    sequence=[(4,"audio borrow",2,3),(6,"audio return",2,4),
              (9,"screen borrow",1,3),(12,"screen return",1,4),
              (15,"unsupported permanent take",1,1)]
    if args.services:
        sequence=[(8,"audio borrow",2,3),(10,"audio return",2,4),
                  (12,"screen borrow",1,3),(15,"screen return",1,4),
                  (18,"unsupported permanent take",1,1)]
    for seq,(at,label,resource,transfer) in enumerate(sequence,1):
        while now()<at and not done.wait(.05): pass
        if done.is_set():return
        sent_at=now()
        channel.send("POST /command RTSP/1.0",seq,{"type":"changeModes","params":{
            "reasonStr":label,"resources":[{"resourceID":resource,"transferType":transfer}]}})
        response=channel.read()
        if response is None: raise EOFError("event closed")
        state["events"].append({"label":label,"sent_at":sent_at,"time":now(),"reply":response[2]})
        log(f"{label}: {response[2]}")
    if args.services:
        for at, night in ((20, True), (23, False)):
            while now()<at and not done.wait(.05): pass
            if done.is_set(): return
            channel.send("POST /command RTSP/1.0",90+int(night),
                         {"type":"setNightMode","params":{"nightMode":night}})
            response=channel.read()
            if response is None: raise EOFError("night event closed")
            state["night_events"].append({"time":now(),"night":night,
                                         "reply":response[2] if isinstance(response[2],dict) else response[2].hex()})
    if args.permanent_modes:
        for seq,at,label,transfer in [(101,36,"permanent take",1),
                                     (102,39,"permanent release",2),
                                     (103,41,"late permanent release",2)]:
            while now()<at and not done.wait(.05): pass
            if done.is_set(): return
            resources=[dict(resourceID=i,transferType=transfer) for i in (1,2)]
            if transfer==1:
                for resource in resources:
                    resource.update(transferPriority=500,takeConstraint=100,borrowConstraint=100)
            sent_at=now()
            channel.send("POST /command RTSP/1.0",seq,{
                "type":"changeModes","params":{"resources":resources}})
            response=channel.read()
            if response is None: raise EOFError("permanent event closed")
            state["events"].append({"label":label,"sent_at":sent_at,"time":now(),"reply":response[2]})
            log(f"{label}: {response[2]}")
    done.wait(args.seconds)
    conn.close()

# Only LPCM formats accepted by this bench. Capture actual packet bytes;
# verification is intentionally in an independent process.
PCM_FORMATS={1:(8000,1), 4:(16000,1), 6:(24000,1), 8:(32000,1),
             10:(44100,1), 11:(44100,2), 14:(48000,1), 15:(48000,2)}

def start_audio(stream, peer):
    global active_audio
    assert active_audio is None, "new MainAudio without acknowledged teardown"
    assert stream["type"]==100 and stream["streamConnectionID"]==0
    assert "uuid" not in stream
    assert stream["audioType"] in ("media","telephony")
    assert stream.get("controlPort",0)>0, "no local RTCP socket"
    assert stream.get("audioLatencyMs")==100
    assert stream.get("supportsRTPPacketRedundancy") is False
    assert stream.get("supportsHighAccuracyTimestamps") is True
    fmt=int(stream["audioFormat"])
    assert fmt>0 and fmt & (fmt-1)==0
    rate,channels=PCM_FORMATS[fmt.bit_length()-1]
    input_enabled=stream.get("input",False)
    assert input_enabled==(stream["audioType"]=="telephony")
    if input_enabled: assert stream.get("dataPort",0)>0
    data,control=listen(socket.SOCK_DGRAM),listen(socket.SOCK_DGRAM)
    entry={"setup":stream,"start":now(),"end":None,"rate":rate,"channels":channels,
           "packets":[],"rtcp":[],"rtcp_probe_sent":0,"mic_sent":0,"file":f"audio-{len(state['audio'])}.rtp"}
    state["audio"].append(entry)
    stop=threading.Event()
    active_audio=(stop,data,control,entry)
    # The wired reference retains/drains RTCP; it does not originate sender
    # reports. Send an empty receiver report to exercise the advertised port.
    control.sendto(struct.pack(">BBHI",0x80,201,1,0x42454e43),(peer[0],stream["controlPort"]))
    entry["rtcp_probe_sent"]+=1
    def receive_audio():
        with (args.output/entry["file"]).open("wb") as out:
            while not done.is_set() and not stop.is_set():
                try: packet,_=data.recvfrom(8192)
                except socket.timeout: continue
                except OSError:
                    if stop.is_set() or done.is_set():break
                    raise
                out.write(struct.pack("<I",len(packet))+packet)
                entry["packets"].append({"time":now(),"size":len(packet)})
    def receive_rtcp():
        while not done.is_set() and not stop.is_set():
            try: packet,_=control.recvfrom(8192)
            except socket.timeout:continue
            except OSError:
                if stop.is_set() or done.is_set():break
                raise
            entry["rtcp"].append({"time":now(),"hex":packet.hex()})
    threads.extend([worker(receive_audio),worker(receive_rtcp)])
    if input_enabled:
        def microphone():
            seq=0;timestamp=0;frames=352
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as mic:
                deadline=time.monotonic()
                while not done.is_set() and not stop.is_set():
                    samples=[int(8000*math.sin(2*math.pi*997*(timestamp+i)/rate))
                             for i in range(frames) for _ in range(channels)]
                    payload=struct.pack(">"+"h"*len(samples),*samples)
                    packet=struct.pack(">BBHII",0x80,100,seq&65535,timestamp&0xffffffff,0x4d494331)+payload
                    mic.sendto(packet,(peer[0],stream["dataPort"]))
                    entry["mic_sent"]+=1;seq+=1;timestamp+=frames
                    deadline+=frames/rate
                    stop.wait(max(0,deadline-time.monotonic()))
        threads.append(worker(microphone))
    return {"type":100,"streamConnectionID":0,"dataPort":data.getsockname()[1],
            "controlPort":control.getsockname()[1]}

def stop_audio():
    global active_audio
    if active_audio is not None:
        stop,data,control,entry=active_audio
        entry["end"]=now();stop.set();data.close();control.close();active_audio=None

def receive_screen():
    while not done.is_set():
        try: conn,_=data_listener.accept(); break
        except socket.timeout:continue
    else:return
    conn.settimeout(1); buffer=b""
    with (args.output/"screen.wire").open("wb") as output:
        while not done.is_set():
            try: data=conn.recv(65536)
            except socket.timeout:continue
            if not data: break
            buffer+=data
            while len(buffer)>=128:
                size=struct.unpack_from("<I",buffer)[0]
                if size>8*1024*1024:raise ValueError("oversized video frame")
                if len(buffer)<128+size:break
                packet,buffer=buffer[:128+size],buffer[128+size:]
                pos=output.tell(); output.write(packet)
                entry={"time":now(),"offset":pos,"size":len(packet)}
                if packet[4]==0:state["frames"].append(entry)
                elif packet[4]==1:state["configs"].append(entry)
                elif packet[4] in (2,5):state["keepalives"]+=1
                else:state["errors"].append(f"unknown video opcode {packet[4]}")
    conn.close()

def serve(conn):
    conn.settimeout(1); channel=Channel(conn)
    while not done.is_set():
        try: request=channel.read()
        except socket.timeout: continue
        if request is None: break
        line,headers,body=request; seq=headers.get("cseq"); method,path,_=line.split(" ",2)
        state["requests"].append({"time":now(),"line":line,"body":body if isinstance(body,dict) else None})
        log(line)
        if args.pair_required and method=="SETUP" and path=="/setup":
            channel.send("RTSP/1.0 470 Authorization Required",seq);continue
        if method=="GET" and path=="/info":
            channel.send("RTSP/1.0 200 OK",seq,ref.info_plist(1280,720));continue
        if path.startswith("/pair-"):
            channel.send("RTSP/1.0 470 Authorization Required",seq);continue
        if method=="SETUP" and path=="/setup":
            assert body["et"]==1 and body["features"]==[]
            channel.send("RTSP/1.0 200 OK",seq,{"eventPort":event_listener.getsockname()[1],
                "timingPort":0,"keepAlivePort":udp_listener.getsockname()[1]});continue
        if method=="SETUP" and isinstance(body,dict) and "streams" in body:
            assert len(body["streams"])==1,"one stream per legacy SETUP expected"
            stream=body["streams"][0]
            if stream["type"]==110:
                state["connection_id"]=stream["streamConnectionID"]
                reply={"type":110,"dataPort":data_listener.getsockname()[1]}
            else:
                assert args.services,"unexpected audio SETUP"
                reply=start_audio(stream,conn.getpeername())
            channel.send("RTSP/1.0 200 OK",seq,{"streams":[reply]});continue
        if method=="TEARDOWN" and isinstance(body,dict) and any(s.get("type")==100 for s in body.get("streams",[])):
            stop_audio()
        channel.send("RTSP/1.0 200 OK",seq,{} if path in ("/command","/feedback") else None)
    conn.close()

srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
srv.bind((args.bind,args.port));srv.listen(3);srv.settimeout(1)
log(f"LISTEN {args.bind}:{args.port}")
threads=[]
if not args.pair_required:threads=[worker(events),worker(receive_screen),worker(receive_udp)]
try:
    while now()<args.seconds:
        try:conn,peer=srv.accept()
        except socket.timeout:continue
        state["connections"]+=1;log(f"control #{state['connections']} from {peer}")
        if state["connections"] == 1: start = time.monotonic()
        t=worker(lambda:serve(conn));threads.append(t)
        if not args.pair_required:
            while now()<args.seconds and t.is_alive():time.sleep(.1)
            state["alive"]=t.is_alive();break
        if state["connections"]>=2:
            t.join(8);break
finally:
    done.set();stop_audio();srv.close()
    for sock in (event_listener,data_listener,udp_listener):sock.close()
    for thread in threads:thread.join(2)
    state["duration"]=now()
    (args.output/"trace.json").write_text(json.dumps(state,indent=2),encoding="utf8")
    log(f"END frames={len(state['frames'])} configs={len(state['configs'])} keepalive={state['keepalives']} udp={state['udp_pings']} alive={state['alive']} errors={state['errors']}")
raise SystemExit(bool(state["errors"]))
