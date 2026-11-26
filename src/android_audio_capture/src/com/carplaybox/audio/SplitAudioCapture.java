package com.carplaybox.audio;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.io.IOException;
import java.util.Arrays;

/** One policy with mutually exclusive UID rules. All record/policy ownership stays on the worker. */
final class SplitAudioCapture implements AutoCloseable {
    static final int SAMPLES=1920;
    static final int BURST_SAMPLES=3*SAMPLES;
    private static final String TAG="CarPlayAudioSplit";
    private final AudioManager manager;
    private final int[] navigationUids;
    private Object policy;
    private AudioRecord media,navigation;
    private volatile boolean stopped;
    private long nextFrame;
    private long frames,shortMediaFrames,shortNavigationFrames;
    SplitAudioCapture(Context context,int navigationUid) throws Exception {
        this(context,navigationUid<0?new int[0]:new int[]{navigationUid});
    }
    SplitAudioCapture(Context context,int[] navigationUids) throws Exception {
        this.navigationUids=navigationUids.clone();
        manager=context.getSystemService(AudioManager.class);
        if(context.checkSelfPermission("android.permission.MODIFY_AUDIO_ROUTING")!=PackageManager.PERMISSION_GRANTED)
            throw new IOException("Audio policy capture requires MODIFY_AUDIO_ROUTING");
        synchronized(CarMicrophoneBridge.ROUTING_LOCK){
        try {
            Class<?> rule=Class.forName("android.media.audiopolicy.AudioMixingRule");
            Class<?> rb=Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
            Class<?> mix=Class.forName("android.media.audiopolicy.AudioMix");
            Class<?> mb=Class.forName("android.media.audiopolicy.AudioMix$Builder");
            Class<?> pc=Class.forName("android.media.audiopolicy.AudioPolicy");
            Class<?> pb=Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
            Object builder=pb.getConstructor(Context.class).newInstance(context);
            pb.getMethod("setLooper",Looper.class).invoke(builder,Looper.getMainLooper());
            Object[] mixes=new Object[hasNavigation()?2:1];
            for(int i=0;i<mixes.length;i++){
                Object rules=rb.getConstructor().newInstance();
                int match=rule.getField("RULE_MATCH_UID").getInt(null);
                // createAudioTrackSource uses USAGE_VIRTUAL_SOURCE (15). Never
                // capture this injected car microphone in either output mix.
                AudioAttributes virtual=new AudioAttributes.Builder().setUsage(15).build();
                rb.getMethod("excludeRule",AudioAttributes.class,int.class).invoke(rules,virtual,
                    rule.getField("RULE_MATCH_ATTRIBUTE_USAGE").getInt(null));
                // Android 8.1's builder dereferences mAttr on existing criteria
                // when adding usage rules, so attributes must precede UID rules.
                java.lang.reflect.Method uidRule=rb.getMethod(i==0?"excludeMixRule":"addMixRule",int.class,Object.class);
                for(int uid:this.navigationUids)uidRule.invoke(rules,match,Integer.valueOf(uid));
                Object b=mb.getConstructor(rule).newInstance(rb.getMethod("build").invoke(rules));
                AudioFormat format=new AudioFormat.Builder().setSampleRate(48000)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
                mb.getMethod("setFormat",AudioFormat.class).invoke(b,format);
                mb.getMethod("setRouteFlags",int.class).invoke(b,2);
                mixes[i]=mb.getMethod("build").invoke(b);
                pb.getMethod("addMix",mix).invoke(builder,mixes[i]);
            }
            policy=pb.getMethod("build").invoke(builder);
            int result=(Integer)AudioManager.class.getMethod("registerAudioPolicy",pc).invoke(manager,policy);
            if(result!=0)throw new IOException("UID split policy registration failed: "+result);
            media=startSink(pc,mix,mixes[0]);
            if(mixes.length>1)navigation=startSink(pc,mix,mixes[1]);
            Log.i(TAG,"Capture policy active "+routeDescription()+" 48000Hz stereo");
        }catch(Exception failure){close();throw failure;}
        }
    }
    boolean hasNavigation(){return navigationUids.length!=0;}
    String routeDescription(){
        return (hasNavigation()?"split":"isolated-media")+" navigationUids="+Arrays.toString(navigationUids);
    }
    private AudioRecord startSink(Class<?> pc,Class<?> mix,Object audioMix)throws Exception{
        // A concurrent system route change may invalidate a newly allocated
        // input before start(). Recreate the handle, never keep a silent sink.
        for(int attempt=0;attempt<3;attempt++){
            AudioRecord record=(AudioRecord)pc.getMethod("createAudioRecordSink",mix).invoke(policy,audioMix);
            boolean started=false;
            try{
                if(record!=null && record.getState()==AudioRecord.STATE_INITIALIZED){
                    record.startRecording();
                    started=record.getRecordingState()==AudioRecord.RECORDSTATE_RECORDING;
                }
                if(started)return record;
            }finally{if(record!=null && !started)record.release();}
        }
        throw new IOException("Audio policy sink failed to start after 3 attempts");
    }
    void stop(){stopped=true;}
    int read(short[] music,short[] speech) throws IOException {
        if(music.length!=SAMPLES)throw new IllegalArgumentException("20ms stereo frame required");
        int received=readMedia(music,speech);
        Arrays.fill(music,received,music.length,(short)0);
        return music.length-received;
    }
    int readMediaBurst(short[] music,short[] speech) throws IOException {
        if(music.length!=BURST_SAMPLES)throw new IllegalArgumentException("bounded media burst required");
        return readMedia(music,speech);
    }
    private int readMedia(short[] music,short[] speech) throws IOException {
        if(speech.length!=SAMPLES)throw new IllegalArgumentException("20ms stereo speech frame required");
        Arrays.fill(speech,(short)0);
        long now=SystemClock.elapsedRealtime();
        if(nextFrame<now-40 || nextFrame==0)nextFrame=now;
        nextFrame+=20;
        int m=0,n=0;
        while(!stopped && !Thread.currentThread().isInterrupted()){
            if(m<music.length)m+=readAvailable(media,music,m);
            if(navigation!=null && n<speech.length)n+=readAvailable(navigation,speech,n);
            long remaining=nextFrame-SystemClock.elapsedRealtime();
            if(remaining<=0)break;
            try{Thread.sleep(Math.min(remaining,2));}catch(InterruptedException end){Thread.currentThread().interrupt();throw new IOException("UID split interrupted",end);}
        }
        if(stopped || Thread.currentThread().isInterrupted())throw new IOException("UID split stopped");
        frames++;if(m<SAMPLES)shortMediaFrames++;if(navigation!=null && n<speech.length)shortNavigationFrames++;
        return m;
    }
    private static int readAvailable(AudioRecord record,short[] buffer,int used)throws IOException{
        int n=record.read(buffer,used,buffer.length-used,AudioRecord.READ_NON_BLOCKING);
        if(n<0)throw new IOException("UID split read failed: "+n);
        if((n&1)!=0)throw new IOException("UID split read returned an incomplete stereo pair");
        return n;
    }
    @Override public void close(){
        synchronized(CarMicrophoneBridge.ROUTING_LOCK){closeLocked();}
    }
    private void closeLocked(){
        stopped=true;
        for(AudioRecord record:new AudioRecord[]{media,navigation})if(record!=null){
            try{record.stop();}catch(RuntimeException ignored){}
            record.release();
        }
        media=navigation=null;
        if(policy!=null){
            try{AudioManager.class.getMethod("unregisterAudioPolicyAsync",policy.getClass()).invoke(manager,policy);}
            catch(Exception failure){Log.e(TAG,"UID split policy cleanup failed",failure);}
            policy=null;
            Log.i(TAG,"UID split released frames="+frames+" shortMedia="+shortMediaFrames+" shortNavigation="+shortNavigationFrames);
        }
    }
}
