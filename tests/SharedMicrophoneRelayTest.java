package com.carplaybox.audio;

import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Exercises the real relay with deterministic input and a backpressured reader. */
public final class SharedMicrophoneRelayTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void await(CountDownLatch event, String message) throws InterruptedException {
        check(event.await(3, TimeUnit.SECONDS), message);
    }

    private static final class Endpoints implements SharedMicrophoneRelay.Backend {
        final CountDownLatch checked = new CountDownLatch(1);
        final CountDownLatch blocked = new CountDownLatch(1);
        final CountDownLatch delivered = new CountDownLatch(1);
        final CountDownLatch removed = new CountDownLatch(1);
        final CountDownLatch haveSequence = new CountDownLatch(1);
        final CountDownLatch retry = new CountDownLatch(1);
        final CountDownLatch cleanupUnconfirmed = new CountDownLatch(1);
        int acceptedSamples;
        String unavailable;
        int failedOpens;
        int notReadyChecks;
        int checks;
        int writes;
        boolean transientBackpressure;
        boolean failPolicyCleanup;
        volatile short firstGap;
        volatile String retryReason;
        volatile long firstBlockedAt, removedAt;
        int policies, inputs, inputCloses, outputCloses;
        volatile boolean isBuiltIn = true;
        volatile boolean writable = true;
        volatile boolean stalled;
        volatile short lastInput, blockedFrame, firstDelivered, lastDelivered;

        @Override public String unavailableReason() throws IOException {
            try {
                if (checks++ < notReadyChecks) throw new IOException("HAL initializing");
                return unavailable;
            } finally {
                checked.countDown();
            }
        }

        @Override public SharedMicrophoneRelay.Output openOutput() {
            if (policies++ != outputCloses) throw new AssertionError("overlapping policies");
            return new SharedMicrophoneRelay.Output() {
                @Override public int write(short[] pcm, int start, int size) {
                    int attempt = ++writes;
                    if (!writable || (transientBackpressure && attempt == 2)) {
                        if (blockedFrame == 0) {
                            blockedFrame = pcm[start];
                            firstBlockedAt = System.nanoTime();
                        }
                        blocked.countDown();
                        return 0;
                    }
                    int offered = transientBackpressure && attempt != 1 ? size : Math.min(3, size);
                    // Partial writes must deliver each sample exactly once, in source order.
                    for (int i = start; i < start + offered; i++) {
                        if (pcm[i] <= lastDelivered) throw new AssertionError("out-of-order sample " + pcm[i]);
                        if (firstGap == 0 && pcm[i] != lastDelivered + 1) firstGap = pcm[i];
                        lastDelivered = pcm[i];
                        acceptedSamples++;
                    }
                    if (firstDelivered == 0) firstDelivered = pcm[start];
                    delivered.countDown();
                    if (acceptedSamples >= 24) haveSequence.countDown();
                    return offered;
                }
                @Override public void close() throws SharedMicrophoneRelay.CleanupFailure {
                    if (inputs > inputCloses + failedOpens) throw new AssertionError("policy closed before input");
                    outputCloses++;
                    removedAt = System.nanoTime();
                    removed.countDown();
                    if (failPolicyCleanup) {
                        throw new SharedMicrophoneRelay.CleanupFailure("still registered", null);
                    }
                }
            };
        }

        @Override public SharedMicrophoneRelay.Input openInput() throws IOException {
            if (policies != outputCloses + 1) throw new AssertionError("capture started without policy");
            if (inputs++ < failedOpens) throw new IOException("input in use");
            return new SharedMicrophoneRelay.Input() {
                @Override public int read(short[] pcm) {
                    if (stalled) return 0;
                    for (int i = 0; i < 4; i++) pcm[i] = ++lastInput;
                    return 4;
                }
                @Override public boolean isBuiltIn() { return isBuiltIn; }
                @Override public void close() throws SharedMicrophoneRelay.CleanupFailure { inputCloses++; }
            };
        }

        @Override public void report(String event, Exception error) {
            if (error != null && event.contains("cleanup unconfirmed")) cleanupUnconfirmed.countDown();
            if (error != null && event.contains("retry")) {
                retryReason = error.getMessage();
                retry.countDown();
            }
        }
    }

    private static void checkVersion() {
        check(SharedMicrophoneRelay.supportedVersion("carplay_shared_mic_version=1"), "exact version");
        check(SharedMicrophoneRelay.supportedVersion("ignored=3;carplay_shared_mic_version=1;"), "parsed token");
        for (String invalid : new String[] { null, "", "carplay_shared_mic_version=10",
                "prefix_carplay_shared_mic_version=1", "carplay_shared_mic_version=1-rc1",
                "carplay_shared_mic_version=1;carplay_shared_mic_version=0" }) {
            check(!SharedMicrophoneRelay.supportedVersion(invalid), "must reject " + invalid);
        }
    }

    private static void unsupportedNeverInstallsPolicy() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.unavailable = "missing shared microphone HAL";
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.checked, "capability not checked");
        }
        check(io.policies == 0 && io.inputs == 0, "unsupported backend touched routing");
    }

    private static void singlePolicyAcrossTransientInputFailure() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.failedOpens = 1;
        SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000);
        try (SharedMicrophoneRelay ignored = relay) {
            relay.start();
            relay.start();
            await(io.delivered, "capture did not recover after busy input");
            check(relay.isActive(), "input is not marked active after delivery");
        }
        relay.close();
        check(io.policies == 1 && io.inputs == 2, "restart replaced the policy or made duplicate workers");
        check(io.inputCloses == 1 && io.outputCloses == 1 && !relay.isActive(), "shutdown leaked input or policy");
    }

    private static void policyRemovedWhenAllInputsFail() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.failedOpens = Integer.MAX_VALUE;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.removed, "unusable input left recorder policy installed");
        }
        check(io.policies == 1 && io.inputs == 3 && io.outputCloses == 1,
                "startup must retry three times, then release before backoff");
    }

    private static void routeLossRestoresNativeRecording() throws InterruptedException {
        Endpoints io = new Endpoints();
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.delivered, "collector did not deliver initial PCM");
            // Loss is exercised while blocked/no PCM to catch a pump that checks only on reads.
            io.stalled = true;
            io.isBuiltIn = false;
            await(io.removed, "invalid route left recorder policy installed");
            check(!relay.isActive(), "collector still active after route loss");
        }
        check(io.inputCloses == 1 && io.outputCloses == 1, "route loss leaked resources");
    }

    private static void initialInvalidRouteNeverForwards() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.isBuiltIn = false;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.removed, "wrong initial route left recorder policy installed");
        }
        check(io.acceptedSamples == 0 && io.inputCloses == io.inputs,
                "unverified input was forwarded or leaked");
    }

    private static void backedUpOutputDropsStalePcm() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.writable = false;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.blocked, "did not exercise output backpressure");
            Thread.sleep(225);
            io.writable = true;
            await(io.delivered, "output did not recover");
            check(io.firstDelivered > io.blockedFrame, "oldest packet replayed after long stall");
            check(relay.droppedFrames() > 0, "loss during backpressure was not counted");
        }
    }

    private static void transientPartialAndZeroWritesPreserveSequence() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.transientBackpressure = true;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.haveSequence, "sink did not recover after transient writes");
            check(io.firstGap == 0, "short write lost the sequence at sample " + io.firstGap);
            check(relay.droppedFrames() == 0, "transient pressure discarded PCM");
        }
    }

    private static void stalledOutputRestoresNativeRecording() throws InterruptedException {
        Endpoints io = new Endpoints();
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.delivered, "virtual mic did not accept initial PCM");
            io.writable = false;
            await(io.blocked, "virtual mic stall did not start");
            await(io.removed, "virtual mic stall left recorder policy installed");
            await(io.retry, "virtual mic stall did not schedule recovery");
            check(io.removedAt - io.firstBlockedAt >= TimeUnit.SECONDS.toNanos(1),
                    "temporary output backpressure caused premature teardown");
            check(io.retryReason.contains("output") && !relay.isActive(), "incorrect stall recovery state");
        }
        check(io.inputCloses == 1 && io.policies == 1 && io.outputCloses == 1,
                "stall failed to restore native routing or reinstalled policy before backoff");
    }

    private static void temporarilyUnavailableBackendRecovers() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.notReadyChecks = 1;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.checked, "did not verify HAL readiness");
            check(io.delivered.await(14, TimeUnit.SECONDS), "boot-time HAL readiness was never rechecked");
            check(relay.isActive() && io.checks == 2 && io.policies == 1,
                    "transient capability failure touched policy or failed to recover");
        }
    }

    private static void unconfirmedPolicyCleanupDoesNotRetry() throws InterruptedException {
        Endpoints io = new Endpoints();
        io.failPolicyCleanup = true;
        try (SharedMicrophoneRelay relay = new SharedMicrophoneRelay(io, 48000)) {
            relay.start();
            await(io.delivered, "collector did not deliver initial PCM");
            // A route error leaves cleanup failure suppressed under the original exception.
            io.isBuiltIn = false;
            await(io.cleanupUnconfirmed, "cleanup failure did not stop the relay");
            check(io.retry.getCount() == 1 && !relay.isActive(), "retry was scheduled over an uncertain policy");
        }
        check(io.inputCloses == 1 && io.policies == 1 && io.outputCloses == 1,
                "relay did not attempt to release both endpoints");
    }

    public static void main(String[] args) throws InterruptedException {
        if (args.length > 0) {
            switch (args[0]) {
                case "burst": transientPartialAndZeroWritesPreserveSequence(); return;
                case "stall": stalledOutputRestoresNativeRecording(); return;
                case "startup": temporarilyUnavailableBackendRecovers(); return;
                default: throw new IllegalArgumentException(args[0]);
            }
        }
        checkVersion();
        unsupportedNeverInstallsPolicy();
        singlePolicyAcrossTransientInputFailure();
        policyRemovedWhenAllInputsFail();
        routeLossRestoresNativeRecording();
        initialInvalidRouteNeverForwards();
        backedUpOutputDropsStalePcm();
        transientPartialAndZeroWritesPreserveSequence();
        stalledOutputRestoresNativeRecording();
        temporarilyUnavailableBackendRecovers();
        unconfirmedPolicyCleanupDoesNotRetry();
        System.out.println("Shared microphone relay: 11 tests passed");
    }
}
