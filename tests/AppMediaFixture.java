package com.carplaybox.appfixture;
import android.app.Service;
import android.content.Intent;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import java.io.DataOutputStream;

/** Test APK only: publishes actual MediaSession state and handles real controls. */
public final class AppMediaFixture extends Service {
    private MediaSession session;private int state=PlaybackState.STATE_PLAYING,track=1,count=3;private long position=1234;private boolean reversed;
    private android.graphics.Bitmap sizedArtwork;
    private static android.graphics.Bitmap artworkOfSize(int size) {
        android.graphics.Bitmap image=android.graphics.Bitmap.createBitmap(256,144,android.graphics.Bitmap.Config.ARGB_8888);
        java.io.ByteArrayOutputStream jpeg=new java.io.ByteArrayOutputStream();
        for(int seed=0;seed<16;seed++) {
            image.eraseColor(0xff225588);
            java.util.Random random=new java.util.Random(seed);
            for(int i=0;i<1500;i++) {
                image.setPixel(i%48,i/48,0xff000000 | random.nextInt(1<<24));
                jpeg.reset();
                if(image.compress(android.graphics.Bitmap.CompressFormat.JPEG,80,jpeg) && jpeg.size()==size) return image;
            }
        }
        image.recycle();
        throw new IllegalArgumentException("No fixture artwork for JPEG size "+size);
    }
    private void update() {
        android.graphics.Bitmap artwork=sizedArtwork;
        if(artwork==null){artwork=android.graphics.Bitmap.createBitmap(320,180,android.graphics.Bitmap.Config.ARGB_8888);
            artwork.eraseColor(track%2==0?0xff225588:0xff884422);}
        session.setMetadata(new MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE,"Fixture歌曲"+track)
            .putString(MediaMetadata.METADATA_KEY_ARTIST,"Fixture Artist").putString(MediaMetadata.METADATA_KEY_ALBUM,"Fixture Album")
            .putBitmap(MediaMetadata.METADATA_KEY_ART,artwork)
            .putLong(MediaMetadata.METADATA_KEY_DURATION,300000).build());
        java.util.List<MediaSession.QueueItem> queue=new java.util.ArrayList<>();
        for(int j=1;j<=count;j++){int i=reversed?count+1-j:j;queue.add(new MediaSession.QueueItem(new android.media.MediaDescription.Builder()
            .setMediaId("fixture-"+i).setTitle("Fixture歌曲"+i).setSubtitle("Fixture Artist").setDescription("Fixture Album").build(),i*101L));}
        session.setQueue(queue);
        session.setPlaybackState(new PlaybackState.Builder().setActions(PlaybackState.ACTION_PLAY|PlaybackState.ACTION_PAUSE|
            PlaybackState.ACTION_PLAY_PAUSE|PlaybackState.ACTION_SKIP_TO_NEXT|PlaybackState.ACTION_SKIP_TO_PREVIOUS|PlaybackState.ACTION_SEEK_TO|PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM)
            .setActiveQueueItemId(track*101L).setState(state,position,state==PlaybackState.STATE_PLAYING?1:0,SystemClock.elapsedRealtime()).build());
        Log.i("CarPlayMediaFixture","state="+state+" track="+track+" position="+position);
    }
    @Override public void onCreate() {
        super.onCreate();
        getSharedPreferences("evidence",0).edit().clear().commit();
        android.app.NotificationManager nm=getSystemService(android.app.NotificationManager.class);
        nm.createNotificationChannel(new android.app.NotificationChannel("fixture","CarPlay test",2));
        startForeground(77,new android.app.Notification.Builder(this,"fixture").setSmallIcon(android.R.drawable.ic_media_play).setContentTitle("CarPlay test player").build());
        session=new MediaSession(this,"CarPlay test player");
        session.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS|MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay(){state=PlaybackState.STATE_PLAYING;update();}
            @Override public void onPause(){state=PlaybackState.STATE_PAUSED;update();}
            @Override public void onSkipToNext(){track++;update();}
            @Override public void onSkipToPrevious(){track--;update();}
            @Override public void onSeekTo(long n){position=n;update();}
            @Override public void onSkipToQueueItem(long id){if(id%101!=0 || id/101<1 || id/101>count)return;track=(int)(id/101);position=0;update();getSharedPreferences("evidence",0).edit().putLong("selected_queue_id",id).commit();}
        });session.setActive(true);update();
        new Thread(()-> {
            boolean rejected=false;
            try(LocalSocket socket=new LocalSocket()) {
                socket.connect(new LocalSocketAddress("carplay.app.integration.v1"));socket.setSoTimeout(1000);
                DataOutputStream out=new DataOutputStream(socket.getOutputStream());
                out.writeInt(0x43505131);out.writeInt(0);out.writeInt(0);out.flush();
                rejected=socket.getInputStream().read()<0;
            } catch(Exception expected) { rejected=true; }
            Log.i("CarPlayMediaFixture","non-root rejected="+rejected);
            getSharedPreferences("evidence",0).edit().putBoolean("non_root_rejected",rejected).commit();
        }).start();
    }
    @Override public int onStartCommand(Intent intent,int flags,int id){if(intent!=null){if(intent.getBooleanExtra("reset",false)){track=1;position=1234;state=PlaybackState.STATE_PLAYING;}count=Math.max(1,Math.min(10000,intent.getIntExtra("count",count)));reversed=intent.getBooleanExtra("reverse",reversed);int size=intent.getIntExtra("jpegSize",0);if(size>0){sizedArtwork=artworkOfSize(size);track++;}update();}return START_NOT_STICKY;}
    @Override public void onDestroy(){session.release();super.onDestroy();}
    @Override public IBinder onBind(Intent intent){return null;}
}
