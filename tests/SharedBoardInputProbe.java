package com.carplaybox.audio;

import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.os.Looper;
import android.os.SystemClock;

/** Bounded metadata-only probe for a collector source outside virtual-recorder routing. */
public final class SharedBoardInputProbe {
    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        Class<?> at=Class.forName("android.app.ActivityThread");
        Object thread=at.getMethod("systemMain").invoke(null);
        Context context=(Context)at.getMethod("getSystemContext").invoke(thread);
        AudioManager manager=context.getSystemService(AudioManager.class);
        AudioRecord record=null; int status=1;
        try {
            int source=args.length>0?Integer.parseInt(args[0]):1999;
            int rate=args.length>1?Integer.parseInt(args[1]):16000;
            int min=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_MONO,2);
            record=new AudioRecord(source,rate,AudioFormat.CHANNEL_IN_MONO,2,Math.max(min,6400));
            if(record.getState()!=AudioRecord.STATE_INITIALIZED) throw new AssertionError("uninitialized");
            AudioDeviceInfo selected=null;
            for(AudioDeviceInfo device:manager.getDevices(AudioManager.GET_DEVICES_INPUTS))
                if(device.getType()==AudioDeviceInfo.TYPE_BUILTIN_MIC) {selected=device;break;}
            if(selected==null || !record.setPreferredDevice(selected)) throw new AssertionError("cannot prefer builtin mic");
            record.startRecording();
            if(record.getRecordingState()!=AudioRecord.RECORDSTATE_RECORDING) throw new AssertionError("not recording");
            short[] buffer=new short[320];long samples=0,squared=0,start=SystemClock.elapsedRealtime();
            while(SystemClock.elapsedRealtime()-start<3500) {
                int n=record.read(buffer,0,buffer.length,AudioRecord.READ_NON_BLOCKING);
                if(n<0) throw new AssertionError("read="+n);
                for(int i=0;i<n;i++){squared+=(long)buffer[i]*buffer[i];samples++;}
                if(n==0) SystemClock.sleep(2);
            }
            AudioDeviceInfo actual=record.getRoutedDevice();
            System.out.println("source="+source+" rate="+rate+" samples="+samples+" rms="+Math.sqrt((double)squared/Math.max(samples,1))+" device="+(actual==null?-1:actual.getType()));
            if(actual==null||actual.getType()!=AudioDeviceInfo.TYPE_BUILTIN_MIC||samples<rate*2.5||squared==0)
                throw new AssertionError("not verified physical board input");
            System.out.println("PASS physical board input");status=0;
        } catch(Throwable failure) {
            failure.printStackTrace(System.out);
        } finally {if(record!=null){try{record.stop();}catch(Exception ignored){}record.release();}}
        System.exit(status);
    }
}
