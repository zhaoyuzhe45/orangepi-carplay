package com.carplaybox.audio;
import android.app.Instrumentation;
import android.content.Intent;
import android.media.*;
import android.os.Bundle;
import android.os.SystemClock;
public final class AudioSplitDeviceTest extends Instrumentation {
 private volatile boolean tone;private Thread thread;
 @Override public void onCreate(Bundle b){super.onCreate(b);start();}
 static double amplitude(short[] p,int frequency){double a=0,b=0;int frames=p.length/2;for(int i=0;i<frames;i++){double angle=2*Math.PI*frequency*i/48000;a+=p[i*2]*Math.cos(angle);b+=p[i*2]*Math.sin(angle);}return 2*Math.hypot(a,b)/frames;}
 static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
 @Override public void onStart(){Bundle out=new Bundle();Intent nav=new Intent().setClassName("com.carplaybox.navfixture","com.carplaybox.fixture.AudioToneFixture");
  try{
   int uid=getTargetContext().getPackageManager().getApplicationInfo("com.carplaybox.navfixture",0).uid;
   for(int repeat=0;repeat<2;repeat++)try(SplitAudioCapture split=new SplitAudioCapture(getTargetContext(),uid)){
    getTargetContext().startForegroundService(nav);tone=true;thread=new Thread(()->{
     AudioTrack t=new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build(),new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build(),15360,AudioTrack.MODE_STREAM,0);
     try{t.play();short[] p=new short[1920];long frame=0;while(tone){for(int i=0;i<p.length;i+=2){short s=(short)(10000*Math.sin(2*Math.PI*1000*frame++/48000));p[i]=p[i+1]=s;}t.write(p,0,p.length);}}finally{t.stop();t.release();}
    });thread.start();
    short[] m=new short[1920],n=new short[1920],mixed=new short[1920];NavigationMixer mixer=new NavigationMixer();
    double mm=0,mn=0,nm=0,nn=0,om=0,on=0;
    for(int i=0;i<125;i++){split.read(m,n);mixer.mix(m,n,mixed,SystemClock.elapsedRealtime());if(i>=75){mm+=amplitude(m,1000);mn+=amplitude(m,2400);nm+=amplitude(n,1000);nn+=amplitude(n,2400);om+=amplitude(mixed,1000);on+=amplitude(mixed,2400);}}
    out.putString("split"+repeat,"media="+mm/50+",leak="+mn/50+" nav="+nn/50+",leak="+nm/50+" output="+om/50+","+on/50);
    check(mm>50000 && nn>30000,"real tones missing: "+out);
    check(mn<mm*.02 && nm<nn*.02,"UID streams not isolated: "+out);
    check(om>mm*.22 && om<mm*.28 && on>nn*.95,"duck/mix ratio: "+out);
    getTargetContext().stopService(nav);
    for(int i=0;i<100;i++){split.read(m,n);mixer.mix(m,n,mixed,SystemClock.elapsedRealtime());}
    check(!mixer.navigationActive(),"speech stayed active after stop");
    check(amplitude(mixed,1000)>amplitude(m,1000)*.98,"music did not recover");
    tone=false;thread.join(1000);check(!thread.isAlive(),"tone worker leak");
   }
   out.putString("result","PASS two distinct UIDs, isolation, navigation mix/duck, recovery, repeated policy release");finish(0,out);
  }catch(Throwable e){out.putString("error",android.util.Log.getStackTraceString(e));finish(-1,out);}
  finally{tone=false;getTargetContext().stopService(nav);}
 }
}
