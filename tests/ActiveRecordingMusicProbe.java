package com.carplaybox.audio;

import android.content.Context;
import android.content.ContextWrapper;
import android.media.AudioManager;
import android.media.AudioRecordingConfiguration;
import android.os.Looper;
import android.os.SystemClock;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** Read-only on-device check with a running external recorder; never opens a new recorder or route. */
public final class ActiveRecordingMusicProbe {
    public static void main(String[] args) throws Exception {
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Class<?> at = Class.forName("android.app.ActivityThread");
        Object thread = at.getMethod("systemMain").invoke(null);
        Context context = (Context)at.getMethod("getSystemContext").invoke(thread);
        AudioManager manager = (AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
        List<AudioRecordingConfiguration> live = manager.getActiveRecordingConfigurations();
        CarMicrophoneBridge bridge = new CarMicrophoneBridge(new ContextWrapper(context) {
            @Override public Context getApplicationContext() { return this; }
        });
        if (bridge.microphoneDemand()) throw new AssertionError("idle demand");
        bridge.updateRecordings(live);
        if (!bridge.microphoneDemand()) throw new AssertionError("need at least one visible external microphone recorder for this probe");
        short[] music = new short[1920], nav = new short[1920], out = new short[1920];
        NavigationMixer mixer = new NavigationMixer();
        for (int amplitude : new int[]{12000, 3000, 12000}) {
            Arrays.fill(music,(short)amplitude); mixer.mix(music,nav,out,SystemClock.elapsedRealtime());
            if (!Arrays.equals(music,out) || !bridge.microphoneDemand()) throw new AssertionError("gain or demand changed");
        }
        bridge.updateRecordings(Collections.<AudioRecordingConfiguration>emptyList());
        if (!bridge.microphoneDemand()) throw new AssertionError("missing release grace");
        SystemClock.sleep(300);
        if (bridge.microphoneDemand()) throw new AssertionError("not released after last recorder");
        System.out.println("PASS live external recorder detected; full, ducked and restored PCM preserved; idle released");
    }
}
