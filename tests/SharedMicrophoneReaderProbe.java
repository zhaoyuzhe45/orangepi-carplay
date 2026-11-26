package com.carplaybox.audio;

import android.app.Instrumentation;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.os.Bundle;
import android.os.SystemClock;

/** Bounded, distinct-UID recording probe. Reports measurements; never stores input audio. */
public final class SharedMicrophoneReaderProbe extends Instrumentation {
    private Bundle args;
    @Override public void onCreate(Bundle value) { super.onCreate(value); args=value; start(); }
    @Override public void onStart() {
        Bundle result=new Bundle(); AudioRecord recorder=null;
        try {
            int source=Integer.parseInt(args.getString("source","1"));
            int rate=Integer.parseInt(args.getString("rate","16000"));
            int channels=Integer.parseInt(args.getString("channels","1"));
            if(channels!=1&&channels!=2) throw new IllegalArgumentException("channels");
            boolean synthetic=Boolean.parseBoolean(args.getString("synthetic","true"));
            int duration=Integer.parseInt(args.getString("duration","6000"));
            if(duration<500 || duration>15000) throw new IllegalArgumentException("duration");
            int mask=channels==1?AudioFormat.CHANNEL_IN_MONO:AudioFormat.CHANNEL_IN_STEREO;
            int min=AudioRecord.getMinBufferSize(rate,mask,2);
            recorder=new AudioRecord(source,rate,mask,2,Math.max(min,rate*channels/2));
            if(recorder.getState()!=AudioRecord.STATE_INITIALIZED) throw new AssertionError("uninitialized");
            recorder.startRecording();
            if(recorder.getRecordingState()!=AudioRecord.RECORDSTATE_RECORDING) throw new AssertionError("not recording");
            short[] data=new short[320*channels]; long start=SystemClock.elapsedRealtime(), samples=0,squared=0;
            int previous=0,gaps=0; boolean havePrevious=false;
            while(SystemClock.elapsedRealtime()-start<duration) {
                int n=recorder.read(data,0,data.length,AudioRecord.READ_NON_BLOCKING);
                if(n<0) throw new AssertionError("read="+n);
                if(n==0) { SystemClock.sleep(2); continue; }
                if(SystemClock.elapsedRealtime()-start<750) continue;
                for(int i=0;i<n;i++) {
                    int value=data[i]; samples++; squared+=(long)value*value;
                    if(i%channels==0) {
                        if(synthetic && havePrevious && value!=((previous+8192+2)%16384)-8192) gaps++;
                        previous=value; havePrevious=true;
                    }
                }
            }
            AudioDeviceInfo device=recorder.getRoutedDevice();
            double rms=Math.sqrt((double)squared/Math.max(1,samples));
            result.putInt("uid",android.os.Process.myUid());
            result.putInt("deviceType",device==null?-1:device.getType());
            result.putLong("samples",samples); result.putInt("sequenceGaps",gaps); result.putDouble("rms",rms);
            result.putInt("rate",rate);
            result.putInt("channels",channels);
            if(samples<rate*channels*(duration-1000)/1000L*.80) throw new AssertionError("insufficient samples "+result);
            if(rms<10) throw new AssertionError("silent input "+result);
            if(synthetic && gaps>12) throw new AssertionError("noncontinuous test sequence "+result);
            result.putString("result","PASS"); finish(0,result);
        } catch(Throwable failure) {
            result.putString("error",android.util.Log.getStackTraceString(failure)); finish(-1,result);
        } finally { if(recorder!=null){try{recorder.stop();}catch(Exception ignored){}recorder.release();} }
    }
}
