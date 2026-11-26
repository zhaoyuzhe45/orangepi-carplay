#!/usr/bin/env python3
"""Validate actual media RTP and independently recorded Android microphone PCM."""
import argparse
import array
import json
import math
from pathlib import Path
import struct
import sys

def tone_amplitude(samples, rate, frequency):
    # Scan windows because warm-up/switching silence is expected; every window
    # still comes from the recorded wire or actual Android AudioRecord file.
    count=min(rate//2, len(samples))
    if count<rate//10:return 0.0
    maximum=0.0
    for start in range(0,len(samples)-count+1,max(count,1)):
        a=b=0.0
        for i,v in enumerate(samples[start:start+count]):
            phase=2*math.pi*frequency*i/rate
            a+=v*math.cos(phase);b+=v*math.sin(phase)
        maximum=max(maximum,2*math.hypot(a,b)/count)
    return maximum

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("capture",type=Path)
    parser.add_argument("--mic",type=Path,required=True)
    args=parser.parse_args()
    state=json.loads((args.capture/"trace.json").read_text(encoding="utf8"))
    assert state["services"] and state["alive"] and not state["errors"],state["errors"]
    streams=state["audio"]
    modes=[s["setup"]["audioType"] for s in streams]
    assert modes==["media","telephony","media"],modes
    borrowed=next(e["time"] for e in state["events"] if e["label"]=="audio borrow")
    # Audio and the RTSP response use different sockets. Valid resumed UDP
    # can arrive before the event thread reads its response; end the pause
    # interval at release submission, not response observation.
    returned=next(e.get("sent_at",e["time"]) for e in state["events"] if e["label"]=="audio return")
    for event in state["events"]:
        if event["label"] in ("permanent take","permanent release","late permanent release"):continue
        if event["label"]=="unsupported permanent take":continue
        resources={r["resourceID"]:r for r in event["reply"]["params"]["resources"]}
        assert resources[2]["permanentEntity"]==1,event
        assert resources[2]["entity"]==(2 if event["label"]=="audio borrow" else 1),event
    results=[]
    for stream in streams:
        data=(args.capture/stream["file"]).read_bytes(); pos=0;samples=[];previous=None;packets=0
        channels=stream["channels"]
        while pos<len(data):
            size=struct.unpack_from("<I",data,pos)[0];pos+=4
            packet=data[pos:pos+size];pos+=size
            assert len(packet)==size and 12<size<=1500,"bad/unbounded RTP packet"
            version,pt,seq,timestamp,ssrc=struct.unpack_from(">BBHII",packet,0)
            assert version==0x80 and pt&127==100
            payload=packet[12:]
            assert len(payload)%(2*channels)==0
            frames=len(payload)//(2*channels)
            if previous:
                prev_seq,prev_ts,prev_frames=previous
                delta=(seq-prev_seq)&65535
                assert 0<delta<100,"RTP sequence reversal/large loss"
                if delta==1:assert (timestamp-prev_ts)&0xffffffff==prev_frames,"RTP timestamp wrong units"
            previous=(seq,timestamp,frames)
            pcm=struct.unpack(">"+"h"*(len(payload)//2),payload)
            samples.extend(pcm[::channels]);packets+=1
        assert packets>20,(stream["setup"],packets)
        assert stream["rtcp_probe_sent"]==1,"receiver RTCP probe was not sent"
        assert not [p for p in stream["packets"] if borrowed+.3<p["time"]<returned-.1],"audio during Borrow"
        if state.get("permanent_modes"):
            taken=next(e["time"] for e in state["events"] if e["label"]=="permanent take")
            released=next(e.get("sent_at",e["time"]) for e in state["events"] if e["label"]=="permanent release")
            assert not [p for p in stream["packets"] if taken+.3<p["time"]<released-.1],"audio during Take"
        media=tone_amplitude(samples,stream["rate"],431)
        echo=tone_amplitude(samples,stream["rate"],997)
        assert media>100,("missing actual Android media capture",media)
        assert echo<max(60,media*.05),("car mic echoed into car playback",echo,media)
        results.append({"mode":stream["setup"]["audioType"],"packets":packets,
                        "media_amplitude":round(media,1),"mic_echo_amplitude":round(echo,1),
                        "rtcp_received":len(stream["rtcp"]),"rtcp_receiver_probes_sent":stream["rtcp_probe_sent"],
                        "mic_sent":stream["mic_sent"]})
    raw=args.mic.read_bytes();pcm=array.array("h");pcm.frombytes(raw)
    if sys.byteorder!="little":pcm.byteswap()
    amplitude=tone_amplitude(pcm,48000,997)
    assert amplitude>500,("actual Android AudioRecord did not receive car microphone",amplitude)
    assert [e["night"] for e in state["night_events"]]==[True,False]
    result={"streams":results,"android_mic_bytes":len(raw),"android_mic_997Hz_amplitude":round(amplitude,1),
            "audio_borrow":True,"media_telephony_media":True,"no_mic_echo":True}
    (args.capture/"services-validation.json").write_text(json.dumps(result,indent=2),encoding="utf8")
    print(json.dumps(result,indent=2))

if __name__=="__main__":main()
