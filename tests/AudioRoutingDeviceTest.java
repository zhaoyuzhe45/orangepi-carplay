package com.carplaybox.audio;
import android.app.Instrumentation;
import android.media.*;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;

/** Real Android policy test: virtual input and two capture sinks coexist. */
public final class AudioRoutingDeviceTest extends Instrumentation {
    static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
    @Override public void onCreate(Bundle b){super.onCreate(b);start();}
    static Object call(Object target,String name,Class<?>[] types,Object... args)throws Exception{
        Method m=target.getClass().getDeclaredMethod(name,types);m.setAccessible(true);return m.invoke(target,args);
    }
    static void set(Object target,String name,Object value)throws Exception{
        Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);f.set(target,value);
    }
    static Object get(Object target,String name)throws Exception{
        Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(target);
    }
    static double amplitude(short[] p,int hz){double a=0,b=0;for(int i=0;i<p.length/2;i++){double angle=2*Math.PI*hz*i/48000;a+=p[i*2]*Math.cos(angle);b+=p[i*2]*Math.sin(angle);}return 4*Math.hypot(a,b)/p.length;}
    @Override public void onStart(){Bundle result=new Bundle();CarMicrophoneBridge bridge=null;HandlerThread callbacks=null;
        AudioManager audio=getTargetContext().getSystemService(AudioManager.class);
        AudioManager.OnAudioFocusChangeListener focus=change->{};
        try {
            bridge=new CarMicrophoneBridge(getTargetContext());
            List<AudioRecordingConfiguration> configs=audio.getActiveRecordingConfigurations();
            call(bridge,"updateRecordings",new Class<?>[]{List.class},configs);
            // This is a field-reproduction gate: no user recording or call is active on this test device.
            check(!(Boolean)call(bridge,"microphoneDemand",new Class<?>[]{}),"background recorder incorrectly requests telephone channel");
            result.putInt("backgroundRecorders",configs.size());
            boolean otherForeground=false;
            for(android.app.ActivityManager.RunningAppProcessInfo p:getTargetContext().getSystemService(android.app.ActivityManager.class).getRunningAppProcesses())
                if(p.uid!=android.os.Process.myUid() && p.importance==100)otherForeground=true;
            check(otherForeground,"foreground UID access unavailable; active user recording would be lost");
            check(audio.requestAudioFocus(focus,AudioManager.STREAM_MUSIC,AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)==AudioManager.AUDIOFOCUS_REQUEST_GRANTED,"test requires exclusive transient focus to avoid music contaminating tone measurements");
            callbacks=new HandlerThread("routing-test");callbacks.start();set(bridge,"callbacks",callbacks);
            for(int repeat=0;repeat<4;repeat++){
                call(bridge,"installPolicy",new Class<?>[]{int.class,int.class},16000,1);
                try(SplitAudioCapture split=new SplitAudioCapture(getTargetContext(),repeat==3?-1:10073)){
                    AudioTrack injected=(AudioTrack)get(bridge,"track");
                    AudioTrack music=new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build(),
                        new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(2).build(),15360,AudioTrack.MODE_STREAM,0);
                    try{
                        music.play();short[] pcm=new short[1920],mic=new short[320],m=new short[1920],n=new short[1920];double signal=0,echo=0;long frame=0,micFrame=0;
                        for(int j=0;j<125;j++){
                            for(int i=0;i<pcm.length;i+=2){short v=(short)(6000*Math.sin(2*Math.PI*1000*frame++/48000));pcm[i]=pcm[i+1]=v;}
                            for(int i=0;i<mic.length;i++)mic[i]=(short)(6000*Math.sin(2*Math.PI*3300*micFrame++/16000));
                            music.write(pcm,0,pcm.length,AudioTrack.WRITE_NON_BLOCKING);
                            injected.write(mic,0,mic.length,AudioTrack.WRITE_NON_BLOCKING);
                            split.read(m,n);
                            if(j>=75){signal+=amplitude(m,1000);echo+=amplitude(m,3300);}
                        }
                        result.putString("round"+repeat,"media="+signal/50+",echo="+echo/50);
                        check(signal>50000,"media missing with microphone policy: "+result);
                        check(echo<signal*.02,"virtual microphone returned to media: "+result);
                    }finally{music.stop();music.release();}
                }finally{call(bridge,"releasePolicy",new Class<?>[]{});}
            }
            result.putString("result","PASS background demand, foreground identity, 3 split cycles and navigation-absent media capture with microphone");finish(0,result);
        }catch(Throwable e){result.putString("error",android.util.Log.getStackTraceString(e));finish(-1,result);}
        finally{audio.abandonAudioFocus(focus);if(bridge!=null)try{call(bridge,"releasePolicy",new Class<?>[]{});}catch(Exception ignored){}if(callbacks!=null)callbacks.quitSafely();}
    }
}
