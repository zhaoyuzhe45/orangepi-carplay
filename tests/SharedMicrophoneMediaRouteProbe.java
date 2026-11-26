package com.carplaybox.audio;

import android.content.Context;
import android.os.Looper;
import android.os.SystemClock;

/** Bounded media-route lifecycle exercise. Audio is discarded and never saved. */
public final class SharedMicrophoneMediaRouteProbe {
    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        Class<?> threadClass=Class.forName("android.app.ActivityThread");
        Object thread=threadClass.getMethod("systemMain").invoke(null);
        Context context=(Context)threadClass.getMethod("getSystemContext").invoke(thread);
        try (SplitAudioCapture capture=new SplitAudioCapture(context,-1)) {
            System.out.println("MEDIA_ROUTE_OPEN");
            short[] media=new short[SplitAudioCapture.SAMPLES], nav=new short[SplitAudioCapture.SAMPLES];
            long end=SystemClock.elapsedRealtime()+2500;
            int frames=0;
            while(SystemClock.elapsedRealtime()<end) { capture.read(media,nav); frames++; }
            System.out.println("MEDIA_ROUTE_FRAMES="+frames);
        }
        System.out.println("MEDIA_ROUTE_CLOSED");
        System.exit(0);
    }
}
