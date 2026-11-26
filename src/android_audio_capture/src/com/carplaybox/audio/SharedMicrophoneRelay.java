package com.carplaybox.audio;

import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

/** Single owner of the shared mic endpoints. Its worker is the only thread to touch them. */
final class SharedMicrophoneRelay implements AutoCloseable {
    static final String VERSION_KEY = "carplay_shared_mic_version";
    private static final int START_ATTEMPTS = 3;
    private static final int INITIAL_RETRY_MS = 10_000;
    private static final int MAX_RETRY_MS = 60_000;
    private static final int POLL_MS = 5;
    private static final long PENDING_LIMIT_NS = TimeUnit.MILLISECONDS.toNanos(100);
    private static final long FIRST_FRAME_NS = TimeUnit.MILLISECONDS.toNanos(500);
    private static final long INPUT_STALL_NS = TimeUnit.SECONDS.toNanos(1);
    private static final long OUTPUT_STALL_NS = TimeUnit.SECONDS.toNanos(1);
    private static final long DROP_REPORT_NS = TimeUnit.MINUTES.toNanos(1);

    interface Input extends AutoCloseable {
        int read(short[] pcm) throws IOException;
        boolean isBuiltIn();
        @Override void close() throws CleanupFailure;
    }

    interface Output extends AutoCloseable {
        int write(short[] pcm, int start, int size) throws IOException;
        @Override void close() throws CleanupFailure;
    }

    /** The previous input or policy could not be confirmed closed. Retrying may overlap it. */
    static final class CleanupFailure extends IOException {
        CleanupFailure(String message, Throwable cause) { super(message, cause); }
    }

    interface Backend {
        // Return a permanent disable reason; throw if the audio stack must be checked again later.

        String unavailableReason() throws Exception;
        Output openOutput() throws Exception;
        Input openInput() throws Exception;
        void report(String event, Exception error);
    }

    static boolean supportedVersion(String parameters) {
        if (parameters == null) return false;
        boolean found = false;
        for (String parameter : parameters.split(";")) {
            int separator = parameter.indexOf('=');
            if (separator < 0 || !VERSION_KEY.equals(parameter.substring(0, separator))) continue;
            if (found || !"1".equals(parameter.substring(separator + 1))) return false;
            found = true;
        }
        return found;
    }

    private final Backend backend;
    private final int maxChunkFrames;
    private final int maxPendingFrames;
    private Thread worker;
    private volatile boolean closed;
    private volatile boolean active;
    private volatile long droppedFrames;
    private volatile long inputFrames;
    private volatile long outputFrames;

    SharedMicrophoneRelay(Backend backend, int sampleRate) {
        this.backend = backend;
        maxChunkFrames = sampleRate / 50; // At most 20 ms per capture read.
        maxPendingFrames = sampleRate / 10; // At most 100 ms of queued samples.
    }

    synchronized void start() {
        if (worker != null || closed) return;
        worker = new Thread(this::run, "shared-board-microphone");
        worker.start();
    }

    boolean isActive() { return active; }
    long droppedFrames() { return droppedFrames; }
    String statistics() { return "active=" + active + " inputFrames=" + inputFrames
            + " outputFrames=" + outputFrames + " droppedFrames=" + droppedFrames; }

    @Override public void close() {
        Thread owned;
        synchronized (this) {
            if (closed) return;
            closed = true;
            owned = worker;
        }
        if (owned == null || owned == Thread.currentThread()) return;
        owned.interrupt();
        try {
            owned.join(1500);
            if (owned.isAlive()) backend.report("collector shutdown still waiting for audio stack", null);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private void run() {
        int delayMs = INITIAL_RETRY_MS;
        try {
            while (!closed) {
                try {
                    String reason = backend.unavailableReason();
                    if (reason != null) {
                        if (!closed) backend.report("disabled: " + reason, null);
                        return;
                    }
                    if (closed) return;
                    try (Output output = backend.openOutput()) {
                        runLease(output);
                    }
                } catch (Exception error) {
                    if (cleanupFailed(error)) {
                        backend.report("shared microphone cleanup unconfirmed; stopped", error);
                        return;
                    }
                    if (error instanceof InterruptedException) throw (InterruptedException) error;
                    if (!closed) backend.report("shared microphone unavailable; retry in " + delayMs + " ms", error);
                }
                if (closed) return;
                Thread.sleep(delayMs);
                delayMs = Math.min(MAX_RETRY_MS, delayMs * 2);
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private static boolean cleanupFailed(Throwable error) {
        if (error instanceof CleanupFailure) return true;
        // A try-with-resources cleanup failure can be suppressed by the original IO/interrupt error.
        for (Throwable suppressed : error.getSuppressed()) {
            if (cleanupFailed(suppressed)) return true;
        }
        return false;
    }

    private void runLease(Output output) throws Exception {
        for (int attempt = 0; attempt < START_ATTEMPTS && !closed; attempt++) {
            short[] first = new short[maxChunkFrames];
            boolean verified = false;
            try (Input input = backend.openInput()) {
                int size = 0;
                long deadline = System.nanoTime() + FIRST_FRAME_NS;
                while (!closed && size == 0 && System.nanoTime() - deadline < 0) {
                    size = checkRead(input.read(first));
                    if (size == 0) Thread.sleep(POLL_MS);
                }
                if (closed) return;
                if (size == 0) throw new IOException("board microphone delivered no initial frames");
                requireBuiltIn(input);
                verified = true;
                active = true;
                backend.report("board microphone active", null);
                try {
                    relay(input, output, first, size);
                } finally {
                    active = false;
                }
                return;
            } catch (InterruptedException interrupted) {
                throw interrupted;
            } catch (Exception error) {
                // Avoid reinstalling the policy while an app is still reopening its input.
                // After the collector has run, route/read/write failure releases everything.
                if (verified || cleanupFailed(error) || attempt + 1 == START_ATTEMPTS) throw error;
                if (!closed) Thread.sleep(250L * (attempt + 1));
            }
        }
    }

    private void relay(Input input, Output output, short[] scratch, int firstFrames)
            throws IOException, InterruptedException {
        PendingPcm pending = new PendingPcm(maxPendingFrames);
        long lastReadAt = System.nanoTime();
        pending.append(scratch, firstFrames, lastReadAt);
        inputFrames += firstFrames;
        long lastReportAt = lastReadAt;
        long reportedDrops = droppedFrames;
        long blockedAt = 0;
        boolean outputBlocked = false;
        while (!closed) {
            // Check the route even with no readable PCM so route failures remove the empty mix.
            requireBuiltIn(input);
            long now = System.nanoTime();
            droppedFrames += pending.expire(now);
            if (!pending.isEmpty()) {
                int written = pending.write(output);
                outputFrames += written;
                if (written > 0) {
                    outputBlocked = false;
                } else {
                    long writtenAt = System.nanoTime();
                    if (!outputBlocked) {
                        blockedAt = writtenAt;
                        outputBlocked = true;
                    } else if (writtenAt - blockedAt >= OUTPUT_STALL_NS) {
                        throw new IOException("virtual mic output stalled");
                    }
                }
            }

            // Always drain capture. Retain new PCM during short writes, evicting only old data.
            if (closed) return;
            int size = checkRead(input.read(scratch));
            if (size > 0) {
                inputFrames += size;
                lastReadAt = System.nanoTime();
                droppedFrames += pending.append(scratch, size, lastReadAt);
            } else if (System.nanoTime() - lastReadAt >= INPUT_STALL_NS) {
                throw new IOException("board microphone stopped delivering frames");
            }
            if (now - lastReportAt >= DROP_REPORT_NS && droppedFrames != reportedDrops) {
                backend.report("dropped mic frames total=" + droppedFrames, null);
                lastReportAt = now;
                reportedDrops = droppedFrames;
            }
            Thread.sleep(POLL_MS);
        }
    }

    /** Fixed-size FIFO with timestamps to expire small reads even before the buffer fills. */
    private static final class PendingPcm {
        private final short[] samples;
        private final long[] capturedAt;
        private int head;
        private int size;

        PendingPcm(int frames) {
            samples = new short[frames];
            capturedAt = new long[frames];
        }

        boolean isEmpty() { return size == 0; }

        private void discard(int count) {
            head = (head + count) % samples.length;
            size -= count;
        }

        int expire(long now) {
            int dropped = 0;
            while (size > 0 && now - capturedAt[head] >= PENDING_LIMIT_NS) {
                discard(1);
                dropped++;
            }
            return dropped;
        }

        int append(short[] source, int count, long now) {
            int dropped = Math.max(0, size + count - samples.length);
            discard(dropped);
            int tail = (head + size) % samples.length;
            int contiguous = Math.min(count, samples.length - tail);
            System.arraycopy(source, 0, samples, tail, contiguous);
            Arrays.fill(capturedAt, tail, tail + contiguous, now);
            if (count > contiguous) {
                System.arraycopy(source, contiguous, samples, 0, count - contiguous);
                Arrays.fill(capturedAt, 0, count - contiguous, now);
            }
            size += count;
            return dropped;
        }

        int write(Output output) throws IOException {
            int offered = Math.min(size, samples.length - head);
            int written = output.write(samples, head, offered);
            if (written < 0 || written > offered) throw new IOException("virtual mic write: " + written);
            discard(written);
            return written;
        }
    }

    private int checkRead(int size) throws IOException {
        if (size < 0 || size > maxChunkFrames) throw new IOException("board microphone read: " + size);
        return size;
    }

    private static void requireBuiltIn(Input input) throws IOException {
        if (!input.isBuiltIn()) throw new IOException("collector is not routed to the built-in microphone");
    }
}
