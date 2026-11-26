package com.carplaybox.audio;

import android.os.Process;

public final class MediaCaptureTuningTest {
    public static void main(String[] args) {
        Process.priority = 0;
        int restore = CaptureThreadPriority.begin("0");
        check(CaptureThreadPriority.current() == 0, "disabled leaves thread alone");
        CaptureThreadPriority.end(restore);

        restore = CaptureThreadPriority.begin("");
        check(CaptureThreadPriority.current() == -16, "default requests Android audio priority");
        CaptureThreadPriority.end(restore);
        check(CaptureThreadPriority.current() == 0, "restores original priority");

        Process.priority = -19;
        restore = CaptureThreadPriority.begin("1");
        check(CaptureThreadPriority.current() == -19, "preserves higher existing priority");
        CaptureThreadPriority.end(restore);

        Process.priority = 4;
        Process.refuse = true;
        restore = CaptureThreadPriority.begin("1");
        check(CaptureThreadPriority.current() == 4, "priority rejection is nonfatal");
        CaptureThreadPriority.end(restore);

        MediaCaptureStats stats = new MediaCaptureStats(123, 0, -16, true, 2);
        MediaContinuityBuffer fifo = new MediaContinuityBuffer(8, 2, 2);
        stats.recordRead(20_000_000, 0, fifo);
        stats.recordWrite(21_000_000, 22_000_000);
        stats.recordRead(40_000_000, 6, fifo);
        stats.recordWrite(41_000_000, 46_000_000);
        fifo.append(new short[]{1, -1}, 2);
        fifo.render(new short[8]);
        stats.recordRead(78_000_000, 0, fifo);
        String snapshot = stats.toString();
        for (String field : new String[]{"tid=123", "baseNice=0", "nice=-16", "enabled=true",
                "frames=3", "shortFrames=1", "missingSamples=6", "maxReadGapUs=38000",
                "gapsOver30Ms=1", "socketWrites=2", "maxSocketWriteUs=5000",
                "reserveFrames=2", "queuedSamples=2", "bufferUnderruns=0", "bufferWaitFrames=1"}) {
            check(snapshot.contains(field), "missing " + field + ": " + snapshot);
        }
        System.out.println("media capture: scheduling, fallback and timing counters passed");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
