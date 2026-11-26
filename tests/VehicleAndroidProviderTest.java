package com.carplaybox.audio;

import android.app.AppOpsManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.location.Location;
import android.os.Bundle;
import android.os.SystemClock;

/** Runs production adapter/bridge code against the documented Android API model. */
public final class VehicleAndroidProviderTest {
    static final class Environment {
        final Context context;
        final VehicleLocationBridge bridge;
        final VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
        Environment() { this(false); }
        Environment(boolean reportOnly) {
            context = new Context(reportOnly);
            bridge = new VehicleLocationBridge(new VehicleAndroidProvider(context));
            input.enabled = input.active = true;
            input.instance = input.generation = 1;
            setRmc("GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A");
        }
        void setRmc(String body) {
            int sum = 0;
            for (int i = 0; i < body.length(); i++) sum ^= body.charAt(i);
            input.rmc = "$" + body + "*" + String.format("%02X", sum);
        }
        void update() {
            input.rmcSequence++;
            bridge.update(input, SystemClock.elapsedRealtime(), 1789992000000L);
        }
        void gpsOff() { context.resolver.mode = 0; context.resolver.providers = ""; }
        void foreignLocation() {
            Location foreign = new Location("gps"); foreign.mock = true;
            foreign.setLatitude(1); foreign.setLongitude(2);
            Bundle extras = new Bundle(); extras.putString("source", "foreign");
            foreign.setExtras(extras); context.locations.last = foreign;
            context.locations.installed = true;
        }
    }
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] arguments) {
        String name = arguments[0];
        Environment e = new Environment(name.startsWith("hardware_report_"));
        switch (name) {
        case "gnss_replaces_without_readback":
            android.os.SystemProperties.gnss = true;
            long before = SystemClock.elapsedRealtime();
            e.update();
            e.bridge.expire(before);
            e.setRmc("GPRMC,115900,A,4852.402,N,00221.102,E,0.0,218.18,210926,,,A");
            e.update();
            java.nio.ByteBuffer wire = java.nio.ByteBuffer.wrap(android.net.LocalSocket.wire.toByteArray());
            check(e.bridge.applied && wire.remaining() == 160 && SystemClock.elapsedRealtime() == before,
                    "two GNSS updates must submit without waiting for Android location readback");
            check(wire.getLong(8) == 1 && wire.getLong(88) == 2 &&
                    wire.getDouble(32) != wire.getDouble(112) && wire.getFloat(136) == 0,
                    "second frame replaces coordinates and speed even with an earlier GPS timestamp");
            e.bridge.stop("test complete");
            break;
        case "hardware_report_passive_only":
            e.context.hardwarePermission = true;
            e.context.locations.reportGps = false;
            e.update();
            check(e.bridge.hasFix && !e.bridge.applied,
                    "passive-only reception must not claim gps delivery to navigation/launcher");
            break;
        case "hardware_injection":
            e.context.hardwarePermission = true;
            e.update();
            check(e.bridge.applied, "privileged hardware injection must be preferred");
            check(e.context.locations.injections == 1 && e.context.locations.adds == 0,
                    "hardware injection must not create a mock test provider");
            Location injected = e.context.locations.getLastKnownLocation("gps");
            check(injected != null && !injected.isFromMockProvider() &&
                    injected.getExtras().getString("source").equals("car-iap2"),
                    "hardware-injected fix must remain non-mock and tagged");
            e.bridge.stop("test complete");
            check(e.context.locations.injections == 1 && !e.context.locations.installed,
                    "stopping hardware injection must not mutate providers");
            break;
        case "hardware_report_location":
            e.context.hardwarePermission = true;
            e.update();
            check(e.bridge.applied, "Android 8.1 reportLocation hardware path must be preferred");
            check(e.context.locations.reports == 1 && e.context.locations.adds == 0,
                    "reported hardware location must not create a mock test provider: applied=" +
                    e.bridge.applied + " reports=" + e.context.locations.reports +
                    " adds=" + e.context.locations.adds + " injections=" +
                    e.context.locations.injections);
            Location reported = e.context.locations.getLastKnownLocation("passive");
            check(reported != null && !reported.isFromMockProvider() &&
                    reported.getExtras().getString("source").equals("car-iap2"),
                    "reported fix must remain non-mock and tagged");
            break;
        case "hardware_transition_removes_orphan":
            e.context.hardwarePermission = true;
            Location transitionOrphan = new Location("gps"); transitionOrphan.mock = true;
            transitionOrphan.setElapsedRealtimeNanos((SystemClock.elapsedRealtime() - 6000) * 1000000L);
            Bundle transitionMetadata = new Bundle();
            transitionMetadata.putString("source", "car-iap2");
            transitionMetadata.putString("carplayOwnerPackage", "com.carplaybox.audio");
            transitionMetadata.putString("carplayOwnerToken", "old-process-token");
            transitionMetadata.putLong("carplayUpdate", 7);
            transitionOrphan.setExtras(transitionMetadata);
            e.context.locations.installed = true;
            e.context.locations.last = transitionOrphan;
            e.context.locations.mockLocation = new Location(transitionOrphan);
            e.update();
            check(e.bridge.applied, "hardware path must recover after an app update");
            check(e.context.locations.removes == 1 && !e.context.locations.installed,
                    "stale mock provider must be removed before hardware injection");
            check(e.context.locations.injections == 1 && e.context.locations.adds == 0,
                    "transition must inject hardware location without adding a test provider");
            Location migrated = e.context.locations.getLastKnownLocation("gps");
            check(migrated != null && !migrated.isFromMockProvider(),
                    "transitioned location must be non-mock");
            break;
        case "accepted_readback":
            e.context.locations.readbackDelay = 150;
            e.update();
            check(e.bridge.applied, "accepted async fixture must be applied");
            Location accepted = e.context.locations.getLastKnownLocation("gps");
            check(accepted != null && accepted.isFromMockProvider() &&
                    accepted.getExtras().getString("carplayOwnerToken") != null &&
                    accepted.getExtras().getLong("carplayUpdate") > 0,
                    "production readback must carry per-owner and per-update identities");
            e.bridge.stop("test complete");
            check(!e.context.locations.installed, "verified owned provider removed");
            break;
        case "mode_ignored":
        case "mode_default":
            e.context.appOps.mockMode = name.equals("mode_ignored") ? 1 : 3;
            e.update();
            check(e.bridge.hasFix && !e.bridge.applied && e.context.locations.adds == 0 &&
                    e.bridge.status.contains("mock_location"), "non-allowed app-op cannot report application");
            break;
        case "census_denied":
            e.context.appOps.censusAvailable = false; e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0 &&
                    e.bridge.status.contains("GET_APP_OPS_STATS"), "unavailable ownership census fails closed");
            break;
        case "other_allowed":
            e.context.appOps.otherRecords.add(new AppOpsManager.PackageOps("foreign.app", 10124, 0));
            e.context.appOps.otherModes.put("foreign.app", 0); e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0, "second allowed mock app prevents replacement");
            break;
        case "uid_allowed":
            e.context.packages.apps.add(new ApplicationInfo("uid.allowed.app", 10125));
            e.context.appOps.otherModes.put("uid.allowed.app", 0); e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0, "effective UID app-op cannot evade package census");
            break;
        case "foreign_existing":
            e.foreignLocation(); e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0 && e.context.locations.removes == 0,
                    "existing foreign mock token is never replaced or removed");
            break;
        case "orphan_own":
        case "orphan_fresh":
        case "orphan_foreign":
        case "orphan_other_authorized":
            SystemClock.sleep(6000);
            Location orphan=new Location("gps");orphan.mock=true;
            orphan.setElapsedRealtimeNanos((SystemClock.elapsedRealtime()-(name.equals("orphan_fresh")?100:6000))*1000000L);
            Bundle metadata=new Bundle();metadata.putString("source","car-iap2");
            metadata.putString("carplayOwnerPackage",name.equals("orphan_foreign")?"foreign.app":"com.carplaybox.audio");
            metadata.putString("carplayOwnerToken","old-process-token");metadata.putLong("carplayUpdate",7);
            orphan.setExtras(metadata);e.context.locations.last=orphan;e.context.locations.installed=true;
            if(name.equals("orphan_other_authorized"))e.context.appOps.otherModes.put("ordinary.other.app",0);
            e.update();
            if(name.equals("orphan_own")){
                check(e.bridge.applied && e.context.locations.adds==0 && e.context.locations.publishes==1,
                    "own stale orphan must resume after process death without replacing GPS");
                check(e.context.locations.last.getExtras().getLong("carplayUpdate")==8,"resume update sequence");
                e.bridge.stop("test complete");check(!e.context.locations.installed,"adopted own orphan supports verified cleanup");
            }else check(!e.bridge.applied && e.context.locations.publishes==0 && e.context.locations.removes==0,
                "fresh, foreign or nonexclusive provider cannot be adopted");
            break;
        case "foreign_replacement":
            e.update(); check(e.bridge.applied, "setup applied");
            e.foreignLocation(); e.bridge.stop("disconnect");
            check(e.context.locations.removes == 0 && e.context.locations.last.getLatitude() == 1,
                    "replacement producer is never removed by our cleanup");
            break;
        case "gps_toggle_updates":
        case "gps_toggle_watchdog":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            check(e.context.locations.isProviderEnabled("gps"), "model must retain test-enabled override");
            check(e.context.locations.getLastKnownLocation("gps") == null && e.context.locations.mockLocation != null,
                    "provider disable clears public cache but preserves mock GPS internal fix");
            if (name.equals("gps_toggle_updates")) e.update();
            else e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.bridge.applied && !e.context.locations.installed && e.context.locations.publishes == 1,
                    "real GPS setting stops injection and removes owner despite test override");
            e.context.resolver.mode = 3; e.context.resolver.providers = "gps,network"; e.update();
            check(e.bridge.applied && e.context.locations.adds == 2,
                    "verified cleanup releases ownership so reenabled GPS accepts a later fresh fix");
            break;
        case "gps_off_after_readback":
            e.context.locations.gpsOffAfterReadback = true; e.update();
            check(!e.bridge.applied && !e.context.locations.installed &&
                    !e.bridge.status.contains("清理失败"),
                    "verified token survives GPS-off during post-readback checks and permits cleanup");
            break;
        case "gps_off_silent_remove":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            e.context.locations.ignoreRemove = true;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 1 && e.context.locations.installed &&
                    e.bridge.status.contains("清理失败"), "empty-cache cleanup must detect a retained test override");
            e.context.locations.ignoreRemove = false;
            e.bridge.expire(SystemClock.elapsedRealtime() + 500);
            check(!e.context.locations.installed && !e.context.locations.isProviderEnabled("gps") &&
                    !e.bridge.status.contains("清理失败"), "GPS-off cleanup retries until the override is removed");
            break;
        case "gps_off_permission_retry":
        case "gps_off_read_permission":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            if (name.equals("gps_off_permission_retry")) e.context.appOps.mockMode = 1;
            else e.context.appOps.fineMode = 1;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 0 && e.context.locations.installed &&
                    e.bridge.status.contains("清理失败"), "GPS-off fallback does not bypass permission loss");
            e.context.appOps.mockMode = e.context.appOps.fineMode = 0;
            e.bridge.expire(SystemClock.elapsedRealtime() + 500);
            check(!e.context.locations.installed && !e.bridge.status.contains("清理失败"),
                    "restored permission permits previously verified GPS-off cleanup");
            break;
        case "gps_off_foreign_allowed":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            e.context.appOps.otherRecords.add(new AppOpsManager.PackageOps("foreign.app", 10124, 0));
            e.context.appOps.otherModes.put("foreign.app", 0);
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 0 && e.context.locations.installed &&
                    e.bridge.status.contains("foreign.app"), "cached ownership cannot override another allowed producer");
            break;
        case "gps_off_unverified":
            e.context.locations.ignorePublish = true; e.update();
            check(!e.bridge.applied, "unverified setup"); e.gpsOff();
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 0 && e.context.locations.installed &&
                    e.bridge.status.contains("清理失败"), "never-observed ownership cannot use GPS-off fallback");
            break;
        case "gps_off_foreign_readback":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            e.context.locations.getLastKnownLocation("gps"); // deliver the settings/cache-clear event first
            e.foreignLocation(); e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 0 && e.context.locations.last.getLatitude() == 1,
                    "observable foreign token wins over previous owned-token evidence");
            break;
        case "gps_off_no_override":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            e.context.locations.enabledOverride = false;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 0 && e.bridge.status.contains("清理失败"),
                    "empty readback without our enabled override cannot establish retained ownership");
            break;
        case "disabled_cache_stays_empty":
            e.update(); check(e.bridge.applied, "setup applied"); e.gpsOff();
            check(e.context.locations.getLastKnownLocation("gps") == null, "provider disable clears the cache");
            e.context.resolver.mode = 3; e.context.resolver.providers = "gps,network";
            check(e.context.locations.getLastKnownLocation("gps") == null &&
                    e.context.locations.mockLocation != null, "reenabling GPS does not restore the public cached token");
            e.bridge.stop("disconnect");
            check(e.context.locations.removes == 0 && e.bridge.status.contains("清理失败"),
                    "empty cache while settings are enabled still requires ownership evidence");
            break;
        case "gps_token_match":
            e.context.resolver.providers = "agps,network"; e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0, "GPS provider setting requires exact token");
            break;
        case "permission_watchdog":
            e.update(); check(e.bridge.applied, "setup applied");
            e.context.appOps.mockMode = 1;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.bridge.applied && e.bridge.hasFix && e.context.locations.installed &&
                    e.context.locations.removes == 0 && e.bridge.status.contains("清理失败"),
                    "watchdog notices app-op revocation and retains pending cleanup");
            break;
        case "cleanup_retry":
            e.update(); e.context.appOps.mockMode = 1;
            e.bridge.stop("disconnect");
            check(e.context.locations.installed && e.context.locations.removes == 0 &&
                    e.bridge.status.contains("清理失败"), "revoked permission cannot silently lose ownership");
            e.context.appOps.mockMode = 0;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.context.locations.installed && !e.bridge.status.contains("清理失败"),
                    "watchdog retries pending cleanup after permission returns");
            break;
        case "silent_publish":
            e.context.locations.ignorePublish = true; e.update();
            check(!e.bridge.applied && e.bridge.status.contains("回读") && e.context.locations.removes == 0,
                    "silent publish failure is unverified, never applied or blindly removed");
            break;
        case "silent_update":
            e.update(); check(e.bridge.applied, "setup applied");
            e.context.locations.ignorePublish = true; e.update();
            check(!e.bridge.applied && !e.context.locations.installed,
                    "old own-token location cannot acknowledge a new update");
            break;
        case "silent_remove":
            e.update(); e.context.locations.ignoreRemove = true; e.bridge.stop("disconnect");
            check(e.context.locations.installed && e.bridge.status.contains("清理失败"),
                    "normal no-op removal return does not confirm cleanup");
            e.context.locations.ignoreRemove = false;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.context.locations.installed && !e.bridge.status.contains("清理失败"),
                    "unacknowledged removal retains state for a later retry");
            break;
        case "null_ownership":
            e.update();
            Location own = e.context.locations.getLastKnownLocation("gps");
            e.context.locations.last = null; e.bridge.stop("disconnect");
            check(e.context.locations.removes == 0 && e.bridge.status.contains("清理失败"),
                    "null readback is not ownership proof");
            e.context.locations.last = own;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.context.locations.installed, "later matching ownership readback allows cleanup");
            break;
        case "same_process_owner":
            e.update(); check(e.bridge.applied, "setup applied");
            VehicleLocationBridge other = new VehicleLocationBridge(new VehicleAndroidProvider(e.context));
            other.update(e.input, SystemClock.elapsedRealtime(), 1789992000000L);
            check(!other.applied && e.context.locations.adds == 1 && e.context.locations.removes == 0,
                    "two process-local adapters cannot replace one another");
            e.bridge.stop("test complete");
            break;
        case "revoke_during_add":
            e.context.locations.revokeOnAdd = true; e.update();
            check(!e.bridge.applied && e.context.locations.enables == 0 && e.context.locations.publishes == 0 &&
                    e.context.locations.removes == 0 && e.bridge.status.contains("清理失败"),
                    "every mutation is gated and interrupted installation retains pending ownership");
            break;
        case "revoke_during_remove":
            e.update(); e.context.locations.revokeOnRemove = true; e.bridge.stop("disconnect");
            check(e.context.locations.installed && e.bridge.status.contains("清理失败"),
                    "revocation inside remove must not be acknowledged");
            e.context.locations.revokeOnRemove = false; e.context.appOps.mockMode = 0;
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(!e.context.locations.installed, "permission restoration retries unacknowledged remove");
            break;
        case "revoke_after_remove":
        case "foreign_after_remove":
            e.update(); e.context.locations.revokeAfterRemove = true; e.bridge.stop("disconnect");
            check(!e.context.locations.installed && e.bridge.status.contains("清理失败"),
                    "permission loss after successful remove retains confirmation obligation");
            e.context.appOps.mockMode = 0;
            if (name.equals("foreign_after_remove")) e.foreignLocation();
            e.bridge.expire(SystemClock.elapsedRealtime() + 250);
            check(e.context.locations.removes == 1 && !e.bridge.status.contains("清理失败"),
                    "restored access confirms prior removal without removing again");
            if (name.equals("foreign_after_remove"))
                check(e.context.locations.installed && e.context.locations.last.getLatitude() == 1,
                        "confirmation never removes a subsequently installed foreign provider");
            else {
                e.context.locations.revokeAfterRemove = false; e.update();
                check(e.bridge.applied && e.context.locations.adds == 2,
                        "confirmed removal releases claim and permits the next session");
            }
            break;
        case "read_permission":
            e.context.appOps.fineMode = 1; e.update();
            check(!e.bridge.applied && e.context.locations.adds == 0, "readback permission is a precondition");
            break;
        default: throw new AssertionError("unknown case " + name);
        }
        System.out.println("PASS " + name);
    }
}
