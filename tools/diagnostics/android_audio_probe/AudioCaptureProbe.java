package com.carplaybox;

import android.media.AudioFormat;
import android.media.AudioRecord;

import java.io.OutputStream;

public final class AudioCaptureProbe {
    public static void main(String[] args) throws Exception {
        int sampleRate = args.length > 0 ? Integer.parseInt(args[0]) : 48000;
        int channels = args.length > 1 ? Integer.parseInt(args[1]) : 2;
        int source = args.length > 2 ? Integer.parseInt(args[2]) : 8;
        int channelMask = channels == 1 ? AudioFormat.CHANNEL_IN_MONO
                : AudioFormat.CHANNEL_IN_STEREO;
        int min = AudioRecord.getMinBufferSize(sampleRate, channelMask,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufferBytes = Math.max(min, 4096);
        System.err.println("probe source=" + source + " rate=" + sampleRate +
                " channels=" + channels + " min=" + min);
        AudioRecord record = new AudioRecord(source, sampleRate, channelMask,
                AudioFormat.ENCODING_PCM_16BIT, bufferBytes);
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            System.err.println("probe init failed state=" + record.getState());
            record.release();
            System.exit(2);
        }
        byte[] buffer = new byte[3840];
        OutputStream out = System.out;
        record.startRecording();
        if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
            System.err.println("probe start failed state=" +
                    record.getRecordingState());
            record.release();
            System.exit(3);
        }
        System.err.println("probe recording");
        try {
            for (;;) {
                int count = record.read(buffer, 0, buffer.length);
                if (count < 0) {
                    System.err.println("probe read failed=" + count);
                    break;
                }
                if (count > 0)
                    out.write(buffer, 0, count);
            }
        } finally {
            record.stop();
            record.release();
        }
    }
}
