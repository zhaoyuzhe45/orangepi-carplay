package com.carplaybox.audio;

/** Keeps status polling near four samples per second without stacking request latency. */
final class VehicleLocationPollSchedule {
    static final long INTERVAL_MS = 250;

    private VehicleLocationPollSchedule() {}

    static long delayAfter(long cycleStartedMs, long nowMs) {
        long elapsed = Math.max(0, nowMs - cycleStartedMs);
        return Math.max(0, INTERVAL_MS - elapsed);
    }
}
