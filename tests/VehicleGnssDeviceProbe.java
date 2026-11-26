package com.carplaybox.audio;

import android.app.Instrumentation;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Looper;
import android.os.SystemClock;

/** Bench-only fixture. Never start this while connected to a vehicle. */
public final class VehicleGnssDeviceProbe extends Instrumentation {
    private volatile int gps, passive;
    private volatile Location delivered;
    private int samples = 12;
    private LocationListener listener(boolean isGps) {
        return new LocationListener() {
            public void onLocationChanged(Location location) {
                if (isGps) { gps++; delivered = new Location(location); } else passive++;
            }
            public void onStatusChanged(String name, int status, Bundle extras) {}
            public void onProviderEnabled(String name) {}
            public void onProviderDisabled(String name) {}
        };
    }
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        if (arguments != null && arguments.containsKey("samples"))
            samples = Math.max(12, Math.min(60, Integer.parseInt(arguments.getString("samples"))));
        start();
    }
    @Override public void onStart() {
        Bundle result = new Bundle(); int code = -1;
        LocationManager manager = (LocationManager)getTargetContext().getSystemService(android.content.Context.LOCATION_SERVICE);
        VehicleAndroidProvider provider = new VehicleAndroidProvider(getTargetContext());
        LocationListener direct = listener(true), indirect = listener(false);
        try {
            if (manager.getProvider("gps") == null) throw new AssertionError("GPS provider absent: launcher cannot subscribe");
            manager.requestLocationUpdates("gps", 0, 0, direct, Looper.getMainLooper());
            manager.requestLocationUpdates("passive", 0, 0, indirect, Looper.getMainLooper());
            VehicleNmeaParser.Fix fix = VehicleNmeaParser.parse(nmea(
                    "GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A"));
            for (int i = 0; i < samples; i++) {
                provider.apply(fix, SystemClock.elapsedRealtime(), System.currentTimeMillis());
                provider.check(); SystemClock.sleep(500);
            }
            if (gps < 5 || passive < 5 || delivered == null || delivered.isFromMockProvider() ||
                    !delivered.hasSpeed() || !delivered.hasAccuracy() || !delivered.hasBearing() ||
                    Math.abs(delivered.getSpeed() - 21.208 * 1852 / 3600) > 0.0001)
                throw new AssertionError("GPS/passive fields failed: gps=" + gps + " passive=" + passive);
            result.putInt("gpsCallbacks", gps); result.putInt("passiveCallbacks", passive);
            result.putInt("launcherSpeedKmh", (int)(delivered.getSpeed() * 3.6));
            result.putBoolean("mock", delivered.isFromMockProvider());
            provider.clear(); int stopped = gps; SystemClock.sleep(5500);
            if (gps != stopped) throw new AssertionError("cleared source still emits GPS fixes");
            if (manager.getProvider("gps") == null) throw new AssertionError("cleanup removed real GPS provider");
            result.putString("result", "PASS real gps/passive callbacks, launcher speed, non-mock and source cleanup");
            code = 0;
        } catch (Throwable failure) { result.putString("result", "FAIL");result.putString("error", failure.toString()); }
        finally {
            try { provider.clear(); } catch (Throwable failure) { code = -1; result.putString("cleanupError", failure.toString()); }
            manager.removeUpdates(direct); manager.removeUpdates(indirect);
        }
        finish(code, result);
    }
    private static String nmea(String body) {
        int checksum = 0; for (int i=0;i<body.length();i++) checksum ^= body.charAt(i);
        return "$" + body + "*" + String.format("%02X", checksum);
    }
}
