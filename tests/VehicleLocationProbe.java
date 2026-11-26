package com.carplaybox.audio;

import android.app.Instrumentation;
import android.app.AppOpsManager;
import android.content.Context;
import android.location.Location;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.Settings;
import java.io.File;
import java.io.FileOutputStream;

/** TEST APK ONLY: real production parser/provider with an isolated fixture. */
public final class VehicleLocationProbe extends Instrumentation {
    private boolean staged;
    private boolean cleanupOnly;
    private boolean cleanupConfirmed;
    private boolean revoke;
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        staged = arguments != null && "true".equals(arguments.getString("staged"));
        cleanupOnly = arguments != null && "true".equals(arguments.getString("cleanup"));
        cleanupConfirmed = arguments != null && "true".equals(arguments.getString("cleanupConfirmed"));
        revoke = arguments != null && "revocation".equals(arguments.getString("scenario"));
        start();
    }
    @Override public void onStart() {
        Bundle result = new Bundle();
        VehicleAndroidProvider provider = new VehicleAndroidProvider(getTargetContext());
        File marker = new File(getTargetContext().getFilesDir(), "vehicle-probe-ready");
        int code = -1;
        try {
            if (cleanupOnly) {
                LocationManager manager = (LocationManager)getTargetContext().getSystemService(Context.LOCATION_SERVICE);
                Location previous = manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
                // TEST ONLY: after Settings clears the public location cache,
                // root can confirm this fixture in dumpsys before requesting cleanup.
                if (!(previous == null && cleanupConfirmed) && (previous == null || !previous.isFromMockProvider() || previous.getExtras() == null ||
                        !"car-iap2".equals(previous.getExtras().getString("source")) ||
                        Math.abs(previous.getLatitude() - 48.8567) > 1e-7 || Math.abs(previous.getLongitude() - 2.3517) > 1e-7))
                    throw new AssertionError("refusing cleanup of unknown/non-fixture GPS provider");
                manager.removeTestProvider(LocationManager.GPS_PROVIDER);
                Location remaining = manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
                if (remaining != null && remaining.isFromMockProvider()) throw new AssertionError("fixture cleanup not confirmed");
                result.putString("result", "PASS removed previous isolated fixture provider");
                finish(0, result);
                return;
            }
            VehicleNmeaParser.Fix fix = VehicleNmeaParser.parse(sentence(
                    "GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A"));
            if (fix == null || !fix.valid) throw new AssertionError("fixture parser rejected");
            provider.apply(fix, SystemClock.elapsedRealtime(), System.currentTimeMillis());
            LocationManager manager = (LocationManager)getTargetContext().getSystemService(Context.LOCATION_SERVICE);
            Location actual = null;
            for (int i = 0; i < 20; i++) {
                actual = manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
                if (actual != null && actual.getExtras() != null && "car-iap2".equals(actual.getExtras().getString("source"))) break;
                SystemClock.sleep(100);
            }
            if (actual == null || Math.abs(actual.getLatitude() - 48.8567) > 1e-7 ||
                    Math.abs(actual.getLongitude() - 2.3517) > 1e-7 || !actual.isFromMockProvider())
                throw new AssertionError("Android did not return the isolated car fixture");
            result.putString("result", "PASS real parser, provider acceptance and marked mock location");
            result.putDouble("latitude", actual.getLatitude());
            result.putDouble("longitude", actual.getLongitude());
            if (revoke) {
                testRevocation(provider, marker, result);
            } else if (staged) {
                VehicleLocationBridge bridge = new VehicleLocationBridge(provider);
                VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
                input.enabled = input.active = true;
                input.instance = input.generation = 1;
                input.rmc = sentence("GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A");
                input.ggaAgeMs = 5000;
                input.rmcSequence++;
                bridge.update(input, SystemClock.elapsedRealtime(), System.currentTimeMillis());
                if (!bridge.applied) throw new AssertionError("GPS-off setup not applied: " + bridge.status);
                try (FileOutputStream ready = new FileOutputStream(marker)) { ready.write(1); }
                long deadline = SystemClock.elapsedRealtime() + 20000;
                int disabledUpdates = 0;
                while (SystemClock.elapsedRealtime() < deadline && disabledUpdates < 4) {
                    input.rmcSequence++;
                    bridge.update(input, SystemClock.elapsedRealtime(), System.currentTimeMillis());
                    bridge.expire(SystemClock.elapsedRealtime());
                    int mode = Settings.Secure.getInt(getTargetContext().getContentResolver(),
                            Settings.Secure.LOCATION_MODE, -1);
                    if (mode == Settings.Secure.LOCATION_MODE_OFF) {
                        if (bridge.applied || bridge.status.contains("清理失败"))
                            throw new AssertionError("GPS-off did not clean provider: " + bridge.status);
                        disabledUpdates++;
                    }
                    SystemClock.sleep(250);
                }
                if (disabledUpdates < 4) throw new AssertionError("shell did not disable GPS after READY");
                result.putInt("freshUpdatesWithGpsDisabled", disabledUpdates);
                result.putString("disabledStatus", bridge.status);
                result.putString("result", "PASS real injection then GPS-off cleanup with continuing fresh fixes");
            }
            code = 0;
        } catch (Throwable failure) {
            result.putString("result", "FAIL");
            result.putString("error", failure.toString());
        }
        finally {
            marker.delete();
            try { provider.clear(); result.putBoolean("providerRemoved", true); }
            catch (Throwable failure) { code = -1; result.putString("cleanupError", failure.toString()); }
        }
        finish(code, result);
    }
    private void testRevocation(VehicleAndroidProvider provider, File marker, Bundle result) throws Exception {
        VehicleLocationBridge bridge = new VehicleLocationBridge(provider);
        VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
        input.enabled = input.active = true;
        input.instance = input.generation = 1;
        input.rmc = sentence("GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A");
        input.ggaAgeMs = 5000;
        AppOpsManager appOps = (AppOpsManager)getTargetContext().getSystemService(Context.APP_OPS_SERVICE);
        // Establish the bridge generation and its provider before inviting
        // revocation. Its first update legitimately clears the direct probe's
        // earlier provider; racing that transition is a different scenario.
        input.rmcSequence++;
        bridge.update(input, SystemClock.elapsedRealtime(), System.currentTimeMillis());
        if (!bridge.applied) throw new AssertionError("revocation setup not applied: " + bridge.status);
        try (FileOutputStream ready = new FileOutputStream(marker)) { ready.write(1); }
        int denied = 0;
        long deadline = SystemClock.elapsedRealtime() + 20000;
        while (SystemClock.elapsedRealtime() < deadline) {
            int mode = appOps.checkOpNoThrow("android:mock_location", android.os.Process.myUid(), getTargetContext().getPackageName());
            if (denied >= 4 && mode == AppOpsManager.MODE_ALLOWED) input.active = false;
            input.rmcSequence++;
            bridge.update(input, SystemClock.elapsedRealtime(), System.currentTimeMillis());
            bridge.expire(SystemClock.elapsedRealtime());
            if (mode != AppOpsManager.MODE_ALLOWED) {
                if (bridge.applied || !bridge.status.contains("清理失败"))
                    throw new AssertionError("revoked permission lost cleanup obligation: " + bridge.status);
                denied++;
            } else if (!input.active) {
                if (bridge.applied || bridge.status.contains("清理失败"))
                    throw new AssertionError("restored permission did not finish cleanup: " + bridge.status);
                result.putInt("revokedUpdates", denied);
                result.putString("result", "PASS revoked app-op retained cleanup then restored and removed provider");
                return;
            }
            SystemClock.sleep(250);
        }
        throw new AssertionError("shell did not revoke and restore app-op after READY");
    }
    private static String sentence(String body) {
        int checksum = 0;
        for (int i = 0; i < body.length(); i++) checksum ^= body.charAt(i);
        return "$" + body + "*" + String.format("%02X", checksum);
    }
}
