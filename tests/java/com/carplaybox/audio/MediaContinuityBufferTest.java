package com.carplaybox.audio;

import java.util.Arrays;

public final class MediaContinuityBufferTest {
    private static final int FRAME = 8;

    public static void main(String[] args) {
        check(MediaContinuityBuffer.reserveFrames("") == 2, "default 40 ms");
        check(MediaContinuityBuffer.reserveFrames("0") == 0, "bypass");
        check(MediaContinuityBuffer.reserveFrames("1") == 1, "20 ms");
        check(MediaContinuityBuffer.reserveFrames("2") == 2, "40 ms");
        check(MediaContinuityBuffer.reserveFrames("999") == 2, "configuration remains bounded");
        short[] output = new short[FRAME];
        MediaContinuityBuffer fifo = new MediaContinuityBuffer(FRAME, 2, 2);
        fifo.append(frame(1000), FRAME);
        fifo.render(output);
        equal(output, frame(0), "first frame primes");
        fifo.append(frame(2000), FRAME);
        fifo.render(output);
        equal(output, frame(0), "second frame primes");
        short[] navigation = frame(3200), mix = new short[FRAME];
        new NavigationMixer().mix(output, navigation, mix, 0);
        equal(mix, navigation, "independent navigation still plays during media startup");
        fifo.append(frame(3000), FRAME);
        fifo.render(output);
        equal(output, new short[] {500,-500,1000,-1000,1000,-1000,1000,-1000}, "start fade");
        check(fifo.queuedSamples() == 2 * FRAME, "two-frame reserve");

        fifo.append(frame(4000), FRAME / 2);
        fifo.render(output);
        equal(output, frame(2000), "partial arrival has no audible gap");
        short[] catchUp = new short[] {4000,-4000,4000,-4000,5000,-5000,5000,-5000,
                                       5000,-5000,5000,-5000};
        fifo.append(catchUp, catchUp.length);
        fifo.render(output);
        equal(output, frame(3000), "catch-up does not skip or duplicate");
        fifo.render(output);
        equal(output, frame(4000), "partial stereo frames reassembled in order");
        fifo.render(output);
        equal(output, frame(5000), "second late tick still uses available samples");
        check(fifo.underruns() == 0, "no underruns within reserve");
        fifo.render(output);
        equal(output, new short[]{2500,-2500,0,0,0,0,0,0}, "sustained loss fades out");
        fifo.render(output);
        equal(output, frame(0), "silence while rebuffering");
        check(fifo.underruns() == 1, "one count per underrun episode");
        fifo.append(frame(6000), FRAME);
        fifo.append(frame(7000), FRAME);
        fifo.render(output);
        equal(output, frame(0), "rebuffer needs the full reserve");
        fifo.append(frame(8000), FRAME);
        fifo.render(output);
        equal(output, new short[]{3000,-3000,6000,-6000,6000,-6000,6000,-6000}, "resume fades in");

        fifo.append(frame(9000), FRAME);
        fifo.append(frame(10000), FRAME);
        fifo.append(frame(11000), FRAME);
        check(fifo.queuedSamples() == 4 * FRAME && fifo.droppedSamples() == FRAME,
              "hard limit drops only the oldest stereo pairs");
        fifo.render(output);
        equal(output, new short[]{7000,-7000,8000,-8000,8000,-8000,8000,-8000}, "overflow transition");
        fifo.render(output);
        equal(output, frame(9000), "no growing backlog after overflow");
        try {
            fifo.append(frame(99), 3);
            throw new AssertionError("odd input must be rejected");
        } catch (IllegalArgumentException expected) { }
        System.out.println("media continuity: reserve, jitter, stereo order, fallback and navigation passed");
    }

    private static short[] frame(int value) {
        return new short[]{(short)value,(short)-value,(short)value,(short)-value,
                           (short)value,(short)-value,(short)value,(short)-value};
    }

    private static void equal(short[] actual, short[] expected, String message) {
        check(Arrays.equals(actual, expected), message + ": " + Arrays.toString(actual));
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
}
