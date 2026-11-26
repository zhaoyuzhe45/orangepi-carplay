package com.carplaybox.fixture;
import android.app.Service;
import android.content.Intent;
import android.media.*;
import android.os.IBinder;
public final class AudioToneFixture extends Service {
 private volatile boolean running;private Thread thread;
 @Override public void onCreate(){super.onCreate();getSystemService(android.app.NotificationManager.class).createNotificationChannel(new android.app.NotificationChannel("test","PCM test",2));startForeground(71,new android.app.Notification.Builder(this,"test").setSmallIcon(android.R.drawable.ic_media_play).setContentTitle("Navigation PCM test").build());}
 @Override public int onStartCommand(Intent intent,int flags,int id){
  if(!running){
   final int requested=intent==null?2400:intent.getIntExtra("frequency",2400);
   final int frequency=requested==1000 || requested==1600?requested:2400;
   final int usage=frequency==2400?AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:AudioAttributes.USAGE_MEDIA;
   running=true;thread=new Thread(()->{
   AudioTrack t=new AudioTrack(new AudioAttributes.Builder().setUsage(usage).build(),
    new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build(),15360,AudioTrack.MODE_STREAM,0);
   try{t.play();short[] p=new short[1920];long frame=0;
    while(running){for(int i=0;i<p.length;i+=2){short s=(short)(6000*Math.sin(2*Math.PI*frequency*frame++/48000));p[i]=p[i+1]=s;}t.write(p,0,p.length);}
   }finally{t.stop();t.release();}
  },"navigation-tone");thread.start();}return START_NOT_STICKY;
 }
 @Override public void onDestroy(){running=false;if(thread!=null)try{thread.join(500);}catch(InterruptedException e){Thread.currentThread().interrupt();}super.onDestroy();}
 @Override public IBinder onBind(Intent i){return null;}
}
