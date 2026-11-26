package com.carplaybox;

import android.app.Service;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/** Temporary external app used to verify the production virtual microphone. */
public final class ProbeService extends Service {
    private final java.util.concurrent.atomic.AtomicInteger active = new java.util.concurrent.atomic.AtomicInteger();
    @Override public void onCreate() {
        super.onCreate();
        NotificationManager manager=getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("probe","CarPlay bench",2));
        startForeground(72,new Notification.Builder(this,"probe")
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("CarPlay bench").build());
    }
    @Override public int onStartCommand(Intent intent,int flags,int id) {
        final String action=intent.getStringExtra("action");
        final int seconds=intent.getIntExtra("seconds",10);
        final int source=intent.getIntExtra("source",1);
        active.incrementAndGet();
        new Thread(() -> {
            try {
                CarServicesProbe.main(new String[]{action,Integer.toString(seconds),
                    getFilesDir().getAbsolutePath()+"/microphone.pcm",Integer.toString(source)});
                Log.i("CarServicesProbe",action+" success");
            } catch(Throwable t) { Log.e("CarServicesProbe",action+" failed",t); }
            finally { if(active.decrementAndGet()==0)stopSelf(); }
        },"services-probe-"+action).start();
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
