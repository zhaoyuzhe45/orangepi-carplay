package com.carplaybox.audio;

import android.content.Context;
import android.content.ContextWrapper;
import android.os.SystemClock;
import java.lang.reflect.Field;

/** Runs in a separate Android process with no AudioManager bound to the microphone transport thread. */
public final class AudioDemandReadProbe {
    public static void main(String[] args) throws Exception {
        Context isolated = new ContextWrapper(null) {
            @Override public Context getApplicationContext() { return this; }
            @Override public Object getSystemService(String name) { return null; }
        };
        CarMicrophoneBridge bridge = new CarMicrophoneBridge(isolated);
        if (bridge.microphoneDemand()) throw new AssertionError("Idle input must not request car microphone");
        Field field = CarMicrophoneBridge.class.getDeclaredField("demand");
        field.setAccessible(true);
        CarMicrophoneBridge.RecordingDemand state = (CarMicrophoneBridge.RecordingDemand) field.get(bridge);
        state.update(true, SystemClock.elapsedRealtime());
        if (!bridge.microphoneDemand()) throw new AssertionError("Active recording must request car microphone");
        state.update(false, SystemClock.elapsedRealtime() - 300);
        if (bridge.microphoneDemand()) throw new AssertionError("Ended recording must release car microphone");
        // In board-mic mode there is no native car input socket or recording monitor.
        bridge.start();
        try { SystemClock.sleep(80); } finally { bridge.close(); }
        System.out.println("PASS microphone transport reads idle, active and released demand without system services");
    }
}
