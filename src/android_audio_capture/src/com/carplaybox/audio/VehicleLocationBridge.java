package com.carplaybox.audio;

/** Location selection/lifetime logic, independent of Android and HTTP. */
final class VehicleLocationBridge {
    interface Provider {
        void apply(VehicleNmeaParser.Fix fix, long elapsedMillis, long wallMillis) throws Exception;
        default void check() throws Exception {}
        void clear() throws Exception;
    }
    static final class Input {
        boolean enabled, active;
        long instance, generation, rmcSequence, ggaSequence, rmcAgeMs, ggaAgeMs;
        String rmc = "", gga = "";
    }
    private final Provider provider;
    private long instance = -1, generation = -1;
    boolean hasFix, applied;
    long lastContentAgeMs = -1, lastApplyDurationMs = -1;
    String status = "等待车机 GPS";
    private String baseStatus = status;
    VehicleLocationBridge(Provider provider) { this.provider = provider; }
    void update(Input input, long elapsedMillis, long wallMillis) {
        if (input == null || !input.enabled || !input.active) {
            stop(input != null && !input.enabled ? "GPS 已禁用" : "等待车机 GPS"); return;
        }
        if (instance != input.instance || generation != input.generation) {
            stop("车机 GPS 会话切换");
            instance = input.instance; generation = input.generation;
        }
        // The car is the live location owner. Every successful status read is
        // a new snapshot to publish; NMEA age and the previous sequence are
        // diagnostics, never gates for Android delivery.
        VehicleNmeaParser.Fix rmc = VehicleNmeaParser.parse(input.rmc);
        VehicleNmeaParser.Fix gga = VehicleNmeaParser.parse(input.gga);
        boolean useRmc = rmc != null && (gga == null || input.rmcSequence >= input.ggaSequence);
        VehicleNmeaParser.Fix fix = useRmc ? rmc : gga;
        long sequence = useRmc ? input.rmcSequence : input.ggaSequence;
        if (fix == null || !fix.valid || sequence <= 0) {
            stop(fix == null ? "等待车机提供可解析定位" : "车机当前无定位"); return;
        }
        hasFix = true;
        if (canCombine(rmc, gga)) {
            // The latest sentence supplies coordinates. The other valid
            // sentence may contribute fields it alone carries, such as speed or
            // altitude, when both describe the same position.
            fix = combine(rmc, gga, useRmc);
        }
        lastContentAgeMs = contentAgeMs(fix, wallMillis);
        long applyStarted = System.nanoTime();
        try {
            // Location.time describes when this sample reached Android. NMEA may
            // carry a stale or malformed car clock; using it here can move the
            // timestamp days into the future and make map SDKs reject later fixes.
            provider.apply(fix, elapsedMillis, wallMillis);
            lastApplyDurationMs = (System.nanoTime() - applyStarted) / 1000000L;
            applied = true;
            status = baseStatus = "车机定位有效；已提交 Android（精度为估计值）";
        } catch (Exception failure) {
            lastApplyDurationMs = (System.nanoTime() - applyStarted) / 1000000L;
            applied = false;
            baseStatus = "车机定位有效；Android 未应用：" + failure.getMessage();
            clearProvider();
        }
    }
    private static boolean canCombine(VehicleNmeaParser.Fix rmc,
            VehicleNmeaParser.Fix gga) {
        if (rmc == null || gga == null || !rmc.valid || !gga.valid) return false;
        double north = (rmc.latitude - gga.latitude) * 111320;
        double east = (rmc.longitude - gga.longitude) * 111320 *
                Math.cos(Math.toRadians(rmc.latitude));
        return north * north + east * east <= 25 * 25;
    }
    private static VehicleNmeaParser.Fix combine(VehicleNmeaParser.Fix rmc,
            VehicleNmeaParser.Fix gga, boolean useRmc) {
        VehicleNmeaParser.Fix position = useRmc ? rmc : gga;
        VehicleNmeaParser.Fix result = new VehicleNmeaParser.Fix();
        result.valid = true;
        result.latitude = position.latitude; result.longitude = position.longitude;
        result.utcTimeOfDayMs = position.utcTimeOfDayMs;
        result.utcMillis = rmc.utcMillis;
        result.speedMps = rmc.speedMps; result.bearingDeg = rmc.bearingDeg;
        result.altitudeM = gga.altitudeM; result.hdop = gga.hdop;
        return result;
    }
    static long contentAgeMs(VehicleNmeaParser.Fix fix, long wallMillis) {
        if (fix == null || fix.utcMillis <= 0) return -1;
        long age = wallMillis - fix.utcMillis;
        // RMC carries a full date. A timestamp more than two seconds in the
        // future means the board clock is not yet aligned, so report unknown.
        return age >= -2000 ? Math.max(0, age) : -1;
    }
    void expire(long elapsedMillis) {
        // Time does not invalidate a car snapshot. This watchdog still checks
        // live Android settings and app-ops so revoked delivery is cleaned up.
        // without a new car sample, and denied cleanup must be retried later.
        if (applied) {
            try { provider.check(); }
            catch (Exception failure) {
                applied = false;
                baseStatus = "车机定位有效；Android 未应用：" + failure.getMessage();
            }
        }
        if (!applied) clearProvider();
    }
    void pollFailed(String reason) {
        // The local API was unavailable. Only a decoded car/session update can
        // revoke the last fix. Do not publish an old position with a new time.
        status = applied ? "车机定位读取失败，保留上次定位：" + reason : "车机定位读取失败：" + reason;
        if (!applied) baseStatus = status;
    }
    void stop(String reason) {
        hasFix = false; applied = false; baseStatus = reason;
        clearProvider();
    }
    private void clearProvider() {
        status = baseStatus;
        try { provider.clear(); } catch (Exception failure) { status += "；提供器清理失败：" + failure.getMessage(); }
    }
}
