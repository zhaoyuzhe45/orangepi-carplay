package com.carplaybox.audio;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.location.Criteria;
import android.location.Location;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.provider.Settings;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.List;
import java.util.UUID;

/** Publishes car coordinates through privileged hardware injection, with a mock fallback. */
final class VehicleAndroidProvider implements VehicleLocationBridge.Provider {
    private static final String MOCK_OP = "android:mock_location";
    private static final String HARDWARE_PERMISSION = "android.permission.LOCATION_HARDWARE";
    private static final String INSTALL_LOCATION_PERMISSION =
            "android.permission.INSTALL_LOCATION_PROVIDER";
    // Android 8.1's hidden OP_MOCK_LOCATION; the board's platform contract.
    private static final int MOCK_OP_CODE = 58;
    private static final String OWNER_PACKAGE = "carplayOwnerPackage";
    private static final String OWNER_TOKEN = "carplayOwnerToken";
    private static final String UPDATE = "carplayUpdate";
    private static final Object OWNER_LOCK = new Object();
    private static VehicleAndroidProvider processOwner;
    private final Context context;
    private final LocationManager manager;
    private final AppOpsManager appOps;
    private final VehicleGnssChannel gnss;
    private final Object hardwareTarget;
    private final Method hardwareMethod;
    private final String packageName;
    private String ownerToken = UUID.randomUUID().toString();
    private final int uid = Process.myUid();
    // A claim means an add may have occurred, not that Android accepted it.
    // Keep it until readback confirms cleanup or a different producer is seen.
    private boolean claimed, cleanupPending, removalReturned, hardwareInjected, gnssInjected;
    private long update, acknowledgedUpdate;

    VehicleAndroidProvider(Context context) {
        this.context = context.getApplicationContext();
        packageName = this.context.getPackageName();
        manager = (LocationManager)context.getSystemService(Context.LOCATION_SERVICE);
        appOps = (AppOpsManager)context.getSystemService(Context.APP_OPS_SERVICE);
        gnss = new VehicleGnssChannel(manager);
        Object target = null;
        Method method = null;
        try {
            method = LocationManager.class.getMethod("injectLocation", Location.class);
            target = manager;
        } catch (NoSuchMethodException publicMethodMissing) {
            // This 8.1 ROM keeps the Binder interface private. Prefer
            // LocationManager.injectLocation when a newer ROM exposes it,
            // then support the 8.1 reportLocation(Location, passive) entry.
            try {
                Field field = LocationManager.class.getDeclaredField("mService");
                field.setAccessible(true);
                target = field.get(manager);
            } catch (ReflectiveOperationException | RuntimeException unavailable) {
                target = null;
            }
            if (target != null) {
                try {
                    method = target.getClass().getMethod("injectLocation", Location.class);
                    method.setAccessible(true);
                } catch (NoSuchMethodException injectMissing) {
                    try {
                        method = target.getClass().getMethod("reportLocation", Location.class, boolean.class);
                        method.setAccessible(true);
                    } catch (NoSuchMethodException reportMissing) {
                        method = null;
                    }
                }
            }
        }
        hardwareTarget = target;
        hardwareMethod = method;
    }

    @Override public void apply(VehicleNmeaParser.Fix fix, long elapsedMillis, long wallMillis) throws Exception {
        synchronized (OWNER_LOCK) {
            if (cleanupPending) clear();
            if (gnss.available()) {
                requireReadPermission(); requireGpsSetting();
                if (processOwner != null && processOwner != this)
                    throw new IllegalStateException("另一个车机定位实例仍持有 GPS 提供器");
                Location existing = readLocation();
                if (existing != null && existing.isFromMockProvider()) {
                    resumeOwnOrphan(existing); clear();
                }
                gnssInjected = true;
                gnss.apply(makeLocation(fix, elapsedMillis, wallMillis, ++update));
                requireReadPermission(); requireGpsSetting();
                return;
            }
            if (hardwareInjectionAvailable()) {
                applyHardware(fix, elapsedMillis, wallMillis);
                return;
            }
            requireAccess();
            requireGpsSetting();
            if (!claimed) {
                if (processOwner != null && processOwner != this)
                    throw new IllegalStateException("另一个车机定位实例仍持有 GPS 提供器");
                Location existing = readLocation();
                if (existing != null && existing.isFromMockProvider()) {
                    resumeOwnOrphan(existing);
                } else {
                // Stock 8.1 addTestProvider("gps") replaces by name. A sole
                // allowed mock app and this process lock narrow that hazard;
                // the platform offers no atomic compare-and-add/remove API.
                requireAccess();
                requireGpsSetting();
                processOwner = this; claimed = true;
                manager.addTestProvider(LocationManager.GPS_PROVIDER, false, true, false,
                        false, true, true, true, Criteria.POWER_LOW, Criteria.ACCURACY_FINE);
                }
            } else {
                requireOwnedLocation();
            }
            requireAccess();
            requireGpsSetting();
            manager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, true);
            requireAccess();
            requireGpsSetting();
            if (acknowledgedUpdate != 0) requireOwnedLocation();
            Location location = makeLocation(fix, elapsedMillis, wallMillis, ++update);
            manager.setTestProviderLocation(LocationManager.GPS_PROVIDER, location);
            awaitReadback(location);
            acknowledgedUpdate = update;
        }
    }

    private boolean hardwareInjectionAvailable() {
        if (hardwareMethod == null || hardwareTarget == null) return false;
        String permission = "reportLocation".equals(hardwareMethod.getName())
                ? INSTALL_LOCATION_PERMISSION : HARDWARE_PERMISSION;
        return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private void applyHardware(VehicleNmeaParser.Fix fix, long elapsedMillis, long wallMillis) throws Exception {
        if (processOwner != null && processOwner != this)
            throw new IllegalStateException("另一个车机定位实例仍持有 GPS 提供器");
        requireReadPermission();
        requireGpsSetting();
        Location existing = readLocation();
        if (existing != null && existing.isFromMockProvider()) {
            // An app update leaves Android's test provider behind. Remove only
            // a stale provider proven to belong to this package before using
            // the hardware path; never leave mock and hardware events mixed.
            resumeOwnOrphan(existing);
            clear();
            ownerToken = UUID.randomUUID().toString();
            update = acknowledgedUpdate = 0;
        }
        if (acknowledgedUpdate != 0) verifyHardwareReadback();
        Location location = makeLocation(fix, elapsedMillis, wallMillis, ++update);
        try {
            Object accepted = hardwareMethod.getParameterTypes().length == 2
                    ? hardwareMethod.invoke(hardwareTarget, location, false)
                    : hardwareMethod.invoke(hardwareTarget, location);
            if (hardwareMethod.getReturnType() == Boolean.TYPE && !Boolean.TRUE.equals(accepted))
                throw new IllegalStateException("Android 硬件定位注入被拒绝");
        } catch (InvocationTargetException failure) {
            Throwable cause = failure.getTargetException();
            throw new IllegalStateException("Android 硬件定位注入失败：" + cause, cause);
        }
        awaitHardwareReadback(location);
        acknowledgedUpdate = update;
        hardwareInjected = true;
    }

    private void resumeOwnOrphan(Location existing) throws Exception {
        Bundle extras=existing.getExtras();
        long age=SystemClock.elapsedRealtime()-existing.getElapsedRealtimeNanos()/1000000L;
        if(extras==null || !packageName.equals(extras.getString(OWNER_PACKAGE)) ||
                !"car-iap2".equals(extras.getString("source")) ||
                extras.getString(OWNER_TOKEN)==null || extras.getString(OWNER_TOKEN).isEmpty() ||
                extras.getLong(UPDATE)<=0 || age<5000)
            throw new IllegalStateException("GPS 已有其他或仍活跃的测试提供器，拒绝替换");
        // Process death does not remove Android's test provider. Only resume a
        // stale fix marked by this package, with exclusive mock authorization
        // and no live owner in this process. Do not replace/remove the provider.
        requireAccess();requireGpsSetting();
        Location confirmed=readLocation();
        if(confirmed==null || !confirmed.isFromMockProvider() || confirmed.getExtras()==null ||
                !packageName.equals(confirmed.getExtras().getString(OWNER_PACKAGE)) ||
                !extras.getString(OWNER_TOKEN).equals(confirmed.getExtras().getString(OWNER_TOKEN)) ||
                extras.getLong(UPDATE)!=confirmed.getExtras().getLong(UPDATE) ||
                existing.getElapsedRealtimeNanos()!=confirmed.getElapsedRealtimeNanos())
            throw new IllegalStateException("遗留 GPS 归属正在变化，等待重新确认");
        ownerToken=extras.getString(OWNER_TOKEN);
        update=acknowledgedUpdate=extras.getLong(UPDATE);
        processOwner=this;claimed=true;
    }

    @Override public void check() throws Exception {
        synchronized (OWNER_LOCK) {
            if (gnssInjected) {
                requireReadPermission(); requireGpsSetting(); return;
            }
            if (hardwareInjected) {
                requireReadPermission();
                requireGpsSetting();
                verifyHardwareReadback();
                return;
            }
            requireAccess();
            requireGpsSetting();
            if (!claimed || cleanupPending)
                throw new IllegalStateException("Android GPS 提供器未验证");
            Location actual = requireOwnedLocation();
            if (actual.getExtras().getLong(UPDATE) != acknowledgedUpdate)
                throw new IllegalStateException("Android GPS 更新回读不匹配");
        }
    }

    @Override public void clear() throws Exception {
        synchronized (OWNER_LOCK) {
            if (gnssInjected) {
                gnssInjected = false; gnss.clear(); return;
            }
            if (hardwareInjected) {
                hardwareInjected = false;
                return;
            }
            if (!claimed) {
                // A restarted service may finish a previous instance's
                // pending cleanup, but may not disturb an active instance.
                if (processOwner != null && processOwner != this && processOwner.cleanupPending)
                    processOwner.clear();
                return;
            }
            cleanupPending = true;
            // Do not require the user's GPS setting here: turning GPS off is
            // one reason to remove our override. App-op/ownership are required.
            requireAccess();
            // Removal may already have returned before a post-call permission
            // check failed. Confirm that operation before demanding its now
            // deleted token, and never remove a subsequent foreign provider.
            if (removalReturned && removalConfirmed()) {
                releaseClaim();
                return;
            }
            requireOwnedLocation(true);
            requireAccess();
            requireOwnedLocation(true); // recheck token or confirmed GPS-off ownership
            manager.removeTestProvider(LocationManager.GPS_PROVIDER);
            removalReturned = true;
            requireAccess();
            if (!removalConfirmed())
                throw new IllegalStateException("Android GPS 删除未获回读确认，等待重试");
            releaseClaim();
        }
    }

    private boolean removalConfirmed() {
        Location remaining = readLocation();
        if (isOwned(remaining)) return false;
        // A foreign token proves our old location is gone; it is never ours
        // to remove. For an empty cache with GPS off, also check the override.
        if (remaining != null) return true;
        return gpsSettingEnabled() || !manager.isProviderEnabled(LocationManager.GPS_PROVIDER);
    }

    private void requireMockPermission() {
        if (manager == null || appOps == null)
            throw new IllegalStateException("LocationManager/AppOpsManager 不可用");
        int mode = appOps.checkOpNoThrow(MOCK_OP, uid, packageName);
        if (mode != AppOpsManager.MODE_ALLOWED)
            throw new IllegalStateException(MOCK_OP + " 未获 MODE_ALLOWED（mode=" + mode + "），等待授权");
    }

    private void requireReadPermission() {
        if (context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED ||
                appOps.checkOpNoThrow(AppOpsManager.OPSTR_FINE_LOCATION, uid, packageName) != AppOpsManager.MODE_ALLOWED)
            throw new IllegalStateException("ACCESS_FINE_LOCATION/精确定位 app-op 不可用，无法验证回读");
    }

    private void requireAccess() throws Exception {
        requireMockPermission();
        requireReadPermission();
        requireExclusiveMockApp();
    }

    private boolean isThisApp(String name, int appUid) {
        return uid == appUid && packageName.equals(name);
    }

    private void rejectOtherAllowed(String name, int appUid, int mode) {
        if (mode == AppOpsManager.MODE_ALLOWED && !isThisApp(name, appUid))
            throw new IllegalStateException("其他应用获准模拟定位：" + name + "（uid=" + appUid + "），拒绝操作 GPS");
    }

    private void requireExclusiveMockApp() throws Exception {
        try {
            Object raw = AppOpsManager.class.getMethod("getPackagesForOps", int[].class)
                    .invoke(appOps, (Object)new int[] { MOCK_OP_CODE });
            if (!(raw instanceof List))
                throw new IllegalStateException("GET_APP_OPS_STATS 未返回模拟定位应用列表");
            boolean ownEntry = false;
            for (Object item : (List<?>)raw) {
                String name = (String)invoke(item, "getPackageName");
                int appUid = (Integer)invoke(item, "getUid");
                List<?> entries = (List<?>)invoke(item, "getOps");
                for (Object entry : entries) {
                    if ((Integer)invoke(entry, "getOp") != MOCK_OP_CODE) continue;
                    rejectOtherAllowed(name, appUid, (Integer)invoke(entry, "getMode"));
                    int effective = appOps.checkOpNoThrow(MOCK_OP, appUid, name);
                    rejectOtherAllowed(name, appUid, effective);
                    if (isThisApp(name, appUid) && effective == AppOpsManager.MODE_ALLOWED) ownEntry = true;
                }
            }
            if (!ownEntry)
                throw new IllegalStateException("GET_APP_OPS_STATS 无法确认本应用为已授权模拟定位应用");
            // A UID-level grant may have no per-package op history. Android
            // 8.1 exposes installed apps without package visibility filtering.
            for (ApplicationInfo app : context.getPackageManager().getInstalledApplications(0)) {
                if (!isThisApp(app.packageName, app.uid))
                    rejectOtherAllowed(app.packageName, app.uid,
                            appOps.checkOpNoThrow(MOCK_OP, app.uid, app.packageName));
            }
        } catch (ReflectiveOperationException | ClassCastException failure) {
            Throwable cause = failure instanceof InvocationTargetException ?
                    ((InvocationTargetException)failure).getTargetException() : failure;
            throw new IllegalStateException("无法查询模拟定位归属（GET_APP_OPS_STATS）：" + cause, failure);
        }
    }

    private static Object invoke(Object target, String method) throws ReflectiveOperationException {
        return target.getClass().getMethod(method).invoke(target);
    }

    private void requireGpsSetting() {
        if (!gpsSettingEnabled())
            throw new IllegalStateException("Android GPS 设置已关闭；停止车机定位");
    }

    @SuppressWarnings("deprecation")
    private boolean gpsSettingEnabled() {
        int mode = Settings.Secure.getInt(context.getContentResolver(),
                Settings.Secure.LOCATION_MODE, Settings.Secure.LOCATION_MODE_OFF);
        String enabled = Settings.Secure.getString(context.getContentResolver(), Settings.Secure.LOCATION_PROVIDERS_ALLOWED);
        boolean gps = false;
        if (enabled != null) for (String name : enabled.split(",")) if ("gps".equals(name.trim())) gps = true;
        // isProviderEnabled("gps") reads our test override on Android 8.1 and
        // cannot tell whether the user has since disabled actual GPS settings.
        return gps && (mode == Settings.Secure.LOCATION_MODE_SENSORS_ONLY || mode == Settings.Secure.LOCATION_MODE_HIGH_ACCURACY);
    }

    private Location readLocation() {
        requireReadPermission();
        return manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
    }

    private Location readHardwareLocation(long revision) {
        requireReadPermission();
        Location gps = manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
        // A passive cache entry alone does not prove that GPS listeners can
        // receive anything: this board originally had no registered GPS HAL.
        return isHardwareUpdate(gps, revision) ? gps : null;
    }

    private boolean isHardwareUpdate(Location location, long revision) {
        if (location == null || location.isFromMockProvider() || location.getExtras() == null) return false;
        Bundle extras = location.getExtras();
        return "car-iap2".equals(extras.getString("source")) &&
                packageName.equals(extras.getString(OWNER_PACKAGE)) &&
                extras.getLong(UPDATE) == revision;
    }

    private boolean isOwned(Location location) {
        if (location == null || !location.isFromMockProvider() || location.getExtras() == null) return false;
        Bundle extras = location.getExtras();
        return ownerToken.equals(extras.getString(OWNER_TOKEN)) && packageName.equals(extras.getString(OWNER_PACKAGE));
    }

    private Location requireOwnedLocation() {
        return requireOwnedLocation(false);
    }

    private Location requireOwnedLocation(boolean cleanup) {
        Location actual = readLocation();
        if (actual == null) {
            // Stock 8.1 clears all public last locations when (for example)
            // network is disabled, but retains MockProvider's internal fix.
            // Only cleanup may use our earlier acknowledged token, with this
            // process claim, current exclusive authorization, GPS set off and
            // its still-enabled test override. Updates still require readback.
            if (cleanup && claimed && processOwner == this && acknowledgedUpdate != 0 &&
                    !gpsSettingEnabled() && manager.isProviderEnabled(LocationManager.GPS_PROVIDER))
                return null;
            throw new IllegalStateException("GPS 归属回读为空，保留待清理状态");
        }
        if (!isOwned(actual)) {
            releaseClaim();
            throw new IllegalStateException("GPS 提供器已被替换，拒绝修改或移除其他提供器");
        }
        return actual;
    }

    private void releaseClaim() {
        claimed = cleanupPending = removalReturned = false;
        acknowledgedUpdate = 0;
        if (processOwner == this) processOwner = null;
    }

    private void verifyHardwareReadback() {
        if (readHardwareLocation(acknowledgedUpdate) == null)
            throw new IllegalStateException("Android 硬件定位回读不匹配");
    }

    private void awaitHardwareReadback(Location expected) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 500;
        while (true) {
            requireReadPermission();
            Location actual = readHardwareLocation(update);
            if (actual != null &&
                    actual.getElapsedRealtimeNanos() == expected.getElapsedRealtimeNanos() &&
                    actual.getTime() == expected.getTime() &&
                    Double.compare(actual.getLatitude(), expected.getLatitude()) == 0 &&
                    Double.compare(actual.getLongitude(), expected.getLongitude()) == 0) {
                requireReadPermission();
                requireGpsSetting();
                return;
            }
            if (SystemClock.elapsedRealtime() >= deadline)
                throw new IllegalStateException("Android 硬件定位更新未获回读确认");
            if (Thread.currentThread().isInterrupted()) throw new InterruptedException("GPS 回读已中断");
            SystemClock.sleep(25);
        }
    }

    private void awaitReadback(Location expected) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 500;
        while (true) {
            requireMockPermission();
            Location actual = readLocation();
            if (isOwned(actual) && actual.getExtras().getLong(UPDATE) == update &&
                    actual.getElapsedRealtimeNanos() == expected.getElapsedRealtimeNanos() &&
                    actual.getTime() == expected.getTime() &&
                    Double.compare(actual.getLatitude(), expected.getLatitude()) == 0 &&
                    Double.compare(actual.getLongitude(), expected.getLongitude()) == 0) {
                // The ownership token was observed even if settings/access
                // change during the remaining guards. Preserve that evidence
                // for cleanup without reporting successful application.
                acknowledgedUpdate = update;
                requireAccess();
                requireGpsSetting();
                return;
            }
            if (actual != null && actual.isFromMockProvider() && !isOwned(actual)) {
                releaseClaim();
                throw new IllegalStateException("Android GPS 回读来自其他提供器，拒绝继续");
            }
            if (SystemClock.elapsedRealtime() >= deadline)
                throw new IllegalStateException("Android GPS 更新未获回读确认");
            if (Thread.currentThread().isInterrupted()) throw new InterruptedException("GPS 回读已中断");
            SystemClock.sleep(25);
        }
    }

    private Location makeLocation(VehicleNmeaParser.Fix fix, long elapsedMillis, long wallMillis, long revision) {
        Location location = new Location(LocationManager.GPS_PROVIDER);
        location.setLatitude(fix.latitude); location.setLongitude(fix.longitude);
        location.setTime(wallMillis); location.setElapsedRealtimeNanos(elapsedMillis * 1000000L);
        // NMEA HDOP is dimensionless, not measured accuracy in meters. Android
        // requires accuracy: publish a conservative, explicitly tagged estimate.
        location.setAccuracy(fix.accuracyEstimateM());
        if (fix.speedMps != null) location.setSpeed(fix.speedMps.floatValue());
        if (fix.bearingDeg != null) location.setBearing(fix.bearingDeg.floatValue());
        if (fix.altitudeM != null) location.setAltitude(fix.altitudeM);
        Bundle extras = new Bundle();
        extras.putString("source", "car-iap2");
        extras.putBoolean("accuracyEstimated", true);
        extras.putString("accuracyPolicy", "max(25m, HDOP*5m)");
        extras.putBoolean("timeFromCar", fix.utcMillis != 0);
        extras.putString(OWNER_PACKAGE, packageName);
        extras.putString(OWNER_TOKEN, ownerToken);
        extras.putLong(UPDATE, revision);
        location.setExtras(extras);
        return location;
    }
}
