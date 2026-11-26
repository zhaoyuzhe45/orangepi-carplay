package com.carplaybox.audio;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;

/** Injects only a bounded deterministic test signal, never captures hardware audio. */
public final class SharedMicrophoneInjectionProbe {
    private static boolean routePresent(String address) throws Exception {
        java.util.ArrayList<Object> ports=new java.util.ArrayList<>();
        int status=(Integer)Class.forName("android.media.AudioSystem")
            .getMethod("listAudioPorts",java.util.ArrayList.class,int[].class)
            .invoke(null,ports,new int[1]);
        if(status!=0) throw new AssertionError("ports="+status);
        Class<?> devicePort=Class.forName("android.media.AudioDevicePort");
        for(Object port:ports) {
            if(devicePort.isInstance(port) && address.equals(devicePort.getMethod("address").invoke(port))) return true;
        }
        return false;
    }
    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        Class<?> activityThread=Class.forName("android.app.ActivityThread");
        Object thread=activityThread.getMethod("systemMain").invoke(null);
        Context context=(Context)activityThread.getMethod("getSystemContext").invoke(thread);
        AudioManager audio=context.getSystemService(AudioManager.class);
        int source=args.length>0?Integer.parseInt(args[0]):9;
        int duration=args.length>1?Integer.parseInt(args[1]):15000;
        if(duration<1000||duration>60000) throw new IllegalArgumentException("duration");
        HandlerThread callbacks=new HandlerThread("shared-mic-probe-policy"); callbacks.start();
        Object policy=null; AudioTrack track=null; int status=1; String address=null;
        try {
            Class<?> ruleClass=Class.forName("android.media.audiopolicy.AudioMixingRule");
            Class<?> ruleBuilderClass=Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
            Object ruleBuilder=ruleBuilderClass.getConstructor().newInstance();
            AudioAttributes.Builder attrs=new AudioAttributes.Builder();
            AudioAttributes.Builder.class.getMethod("setCapturePreset",int.class).invoke(attrs,source);
            ruleBuilderClass.getMethod("addRule",AudioAttributes.class,int.class).invoke(ruleBuilder,attrs.build(),2);
            Object rule=ruleBuilderClass.getMethod("build").invoke(ruleBuilder);
            Class<?> mixClass=Class.forName("android.media.audiopolicy.AudioMix");
            Class<?> mixBuilderClass=Class.forName("android.media.audiopolicy.AudioMix$Builder");
            Object mixBuilder=mixBuilderClass.getConstructor(ruleClass).newInstance(rule);
            AudioFormat format=new AudioFormat.Builder().setSampleRate(16000).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(2).build();
            mixBuilderClass.getMethod("setFormat",AudioFormat.class).invoke(mixBuilder,format);
            mixBuilderClass.getMethod("setRouteFlags",int.class).invoke(mixBuilder,2);
            Object mix=mixBuilderClass.getMethod("build").invoke(mixBuilder);
            Class<?> policyClass=Class.forName("android.media.audiopolicy.AudioPolicy");
            Class<?> policyBuilderClass=Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
            Object builder=policyBuilderClass.getConstructor(Context.class).newInstance(context);
            policyBuilderClass.getMethod("setLooper",Looper.class).invoke(builder,callbacks.getLooper());
            policyBuilderClass.getMethod("addMix",mixClass).invoke(builder,mix);
            policy=policyBuilderClass.getMethod("build").invoke(builder);
            int registered=(Integer)AudioManager.class.getMethod("registerAudioPolicy",policyClass).invoke(audio,policy);
            if(registered!=0) throw new AssertionError("register="+registered);
            address=(String)mixClass.getMethod("getRegistration").invoke(mix);
            if(address==null || address.isEmpty() || !routePresent(address)) throw new AssertionError("registered route missing");
            System.out.println("REGISTERED_PORT="+address);
            track=(AudioTrack)policyClass.getMethod("createAudioTrackSource",mixClass).invoke(policy,mix);
            if(track==null||track.getState()!=AudioTrack.STATE_INITIALIZED) throw new AssertionError("track unavailable");
            track.play(); System.out.println("READY source="+source+" rate=16000 pid="+android.os.Process.myPid());
            short[] ramp=new short[320]; int value=-8192,offset=320; long frames=0;
            long end=SystemClock.elapsedRealtime()+duration;
            while(SystemClock.elapsedRealtime()<end) {
                if(offset==ramp.length) {
                    for(int i=0;i<ramp.length;i++){ramp[i]=(short)value;value=((value+8192+2)%16384)-8192;}
                    offset=0;
                }
                int n=track.write(ramp,offset,ramp.length-offset,AudioTrack.WRITE_NON_BLOCKING);
                if(n<0) throw new AssertionError("write="+n);
                offset+=n;frames+=n;
                SystemClock.sleep(2);
            }
            System.out.println("DONE frames="+frames);status=0;
        } finally {
            if(track!=null){try{track.stop();}catch(Exception ignored){}track.release();}
            if(policy!=null) AudioManager.class.getMethod("unregisterAudioPolicyAsync",policy.getClass()).invoke(audio,policy);
            if(address!=null) {
                long removalStart=SystemClock.elapsedRealtime();
                while(routePresent(address) && SystemClock.elapsedRealtime()-removalStart<2000) SystemClock.sleep(25);
                if(routePresent(address)) throw new AssertionError("route remains after unregister");
                System.out.println("UNREGISTERED_PORT ms="+(SystemClock.elapsedRealtime()-removalStart));
            }
            callbacks.quitSafely();
            System.out.println("CLEANED");
        }
        System.exit(status);
    }
}
