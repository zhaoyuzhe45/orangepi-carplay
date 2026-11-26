package com.carplaybox.audio;

import java.util.Arrays;

/** Session-local PCM queue. Indices always advance in complete stereo pairs. */
final class MediaContinuityBuffer {
    private final short[] pcm;
    private final int frameSamples, primeSamples, fadePairs;
    private int head, size;
    private short lastLeft, lastRight;
    private boolean ready, smoothNext;
    private long underruns, droppedSamples, waitFrames;

    static int reserveFrames(String setting) {
        if ("0".equals(setting)) return 0;
        return "1".equals(setting) ? 1 : 2;
    }

    MediaContinuityBuffer(int frameSamples, int reserveFrames, int fadePairs) {
        if (frameSamples < 2 || (frameSamples & 1) != 0 || frameSamples > Integer.MAX_VALUE / 4
                || reserveFrames < 1 || reserveFrames > 2 || fadePairs < 1 || fadePairs > frameSamples / 2)
            throw new IllegalArgumentException("invalid media queue size");
        this.frameSamples = frameSamples;
        this.primeSamples = frameSamples * (1 + reserveFrames);
        this.fadePairs = fadePairs;
        pcm = new short[frameSamples * (reserveFrames + 2)];
    }

    void append(short[] input, int samples) {
        if (samples < 0 || samples > input.length || samples > pcm.length || (samples & 1) != 0)
            throw new IllegalArgumentException("invalid stereo sample count");
        int excess = samples - (pcm.length - size);
        if (excess > 0) {
            advance(excess);
            droppedSamples += excess;
            smoothNext = true;
        }
        int tail = (int)((head + (long)size) % pcm.length);
        int first = Math.min(samples, pcm.length - tail);
        System.arraycopy(input, 0, pcm, tail, first);
        System.arraycopy(input, first, pcm, 0, samples - first);
        size += samples;
    }

    void render(short[] output) {
        if (output.length != frameSamples) throw new IllegalArgumentException("invalid output frame");
        if (!ready && size >= primeSamples) {
            ready = true;
            smoothNext = true;
        }
        if (!ready || size < frameSamples) {
            if (ready) underruns++;
            ready = false;
            waitFrames++;
            Arrays.fill(output, (short)0);
            if (lastLeft != 0 || lastRight != 0) smooth(output);
        } else {
            int first = Math.min(frameSamples, pcm.length - head);
            System.arraycopy(pcm, head, output, 0, first);
            System.arraycopy(pcm, 0, output, first, frameSamples - first);
            advance(frameSamples);
            if (smoothNext) smooth(output);
            smoothNext = false;
        }
        lastLeft = output[frameSamples - 2];
        lastRight = output[frameSamples - 1];
    }

    private void advance(int samples) {
        head = (int)((head + (long)samples) % pcm.length);
        size -= samples;
    }

    private void smooth(short[] output) {
        for (int i = 0; i < fadePairs; i++) {
            int step = i + 1, index = 2 * i;
            output[index] = blend(lastLeft, output[index], step);
            output[index + 1] = blend(lastRight, output[index + 1], step);
        }
    }

    private short blend(short previous, short next, int step) {
        return (short)(previous + ((next - (long)previous) * step) / fadePairs);
    }

    int queuedSamples() { return size; }
    long underruns() { return underruns; }
    long droppedSamples() { return droppedSamples; }
    long waitFrames() { return waitFrames; }
}
