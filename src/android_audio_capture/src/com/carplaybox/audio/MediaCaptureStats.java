package com.carplaybox.audio;

/** One writer (the media worker); synchronized snapshots are read only on demand. */
final class MediaCaptureStats {
    private final int tid, baseNice, nice, reserveFrames;
    private final boolean enabled;
    private boolean active = true;
    private long frames, shortFrames, missingSamples, lastReadNanos, maxReadGapNanos, gapsOver30Ms;
    private long socketWrites, maxSocketWriteNanos;
    private int queuedSamples;
    private long bufferUnderruns, bufferDroppedSamples, bufferWaitFrames;

    MediaCaptureStats(int tid, int baseNice, int nice, boolean enabled, int reserveFrames) {
        this.tid = tid;
        this.baseNice = baseNice;
        this.nice = nice;
        this.enabled = enabled;
        this.reserveFrames = reserveFrames;
    }

    synchronized void recordRead(long now, int missing, MediaContinuityBuffer buffer) {
        if (frames != 0) {
            long gap = now - lastReadNanos;
            maxReadGapNanos = Math.max(maxReadGapNanos, gap);
            if (gap > 30_000_000L) gapsOver30Ms++;
        }
        lastReadNanos = now;
        frames++;
        if (missing > 0) {
            shortFrames++;
            missingSamples += missing;
        }
        if (buffer != null) {
            queuedSamples = buffer.queuedSamples();
            bufferUnderruns = buffer.underruns();
            bufferDroppedSamples = buffer.droppedSamples();
            bufferWaitFrames = buffer.waitFrames();
        }
    }

    synchronized void recordWrite(long start, long end) {
        socketWrites++;
        maxSocketWriteNanos = Math.max(maxSocketWriteNanos, end - start);
    }

    synchronized void finish() { active = false; }

    private static String niceString(int value) {
        return value == CaptureThreadPriority.UNKNOWN ? "unknown" : Integer.toString(value);
    }

    @Override public synchronized String toString() {
        return "active=" + active + " enabled=" + enabled + " tid=" + tid
                + " baseNice=" + niceString(baseNice) + " nice=" + niceString(nice)
                + " frames=" + frames + " shortFrames=" + shortFrames + " missingSamples=" + missingSamples
                + " maxReadGapUs=" + maxReadGapNanos / 1000 + " gapsOver30Ms=" + gapsOver30Ms
                + " socketWrites=" + socketWrites + " maxSocketWriteUs=" + maxSocketWriteNanos / 1000
                + " reserveFrames=" + reserveFrames + " queuedSamples=" + queuedSamples
                + " bufferUnderruns=" + bufferUnderruns + " bufferDroppedSamples=" + bufferDroppedSamples
                + " bufferWaitFrames=" + bufferWaitFrames;
    }
}
