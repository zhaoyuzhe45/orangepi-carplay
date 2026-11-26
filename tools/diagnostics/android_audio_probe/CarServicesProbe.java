package com.carplaybox;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.SystemClock;
import java.io.FileOutputStream;

/** Standalone bench fixture; never packaged in the production application. */
public final class CarServicesProbe {
    public static void main(String[] args) throws Exception {
        String action = args[0];
        int seconds = Integer.parseInt(args[1]);
        int rate = 48000;
        if (action.equals("play")) {
            AudioTrack track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setBufferSizeInBytes(19200).setTransferMode(AudioTrack.MODE_STREAM).build();
            if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new Exception("track init");
            short[] samples = new short[1920];
            long frame = 0, until = SystemClock.elapsedRealtime() + seconds * 1000L;
            try {
                track.setVolume(1f); track.play();
                while (SystemClock.elapsedRealtime() < until) {
                    for (int i=0;i<samples.length/2;i++,frame++) {
                        short value=(short)(6000*Math.sin(2*Math.PI*431*frame/rate));
                        samples[i*2]=value; samples[i*2+1]=value;
                    }
                    int used=0;
                    while(used<samples.length) {
                        int count=track.write(samples,used,samples.length-used);
                        if(count<=0)throw new Exception("track write="+count);
                        used+=count;
                    }
                }
            } finally { track.stop(); track.release(); }
            System.out.println("PLAY_OK frames="+frame);
        } else if (action.equals("record")) {
            int source=args.length>3 ? Integer.parseInt(args[3]) : MediaRecorder.AudioSource.MIC;
            int min=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_MONO,
                                               AudioFormat.ENCODING_PCM_16BIT);
            AudioRecord record=new AudioRecord(source,rate,AudioFormat.CHANNEL_IN_MONO,
                                                AudioFormat.ENCODING_PCM_16BIT,Math.max(9600,min*2));
            if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new Exception("record init");
            long until=SystemClock.elapsedRealtime()+seconds*1000L,total=0;
            byte[] bytes=new byte[1920];
            try(FileOutputStream out=new FileOutputStream(args[2])) {
                record.startRecording();
                if(record.getRecordingState()!=AudioRecord.RECORDSTATE_RECORDING)
                    throw new Exception("record start");
                System.out.println("RECORD_ACTIVE source="+source);
                while(SystemClock.elapsedRealtime()<until) {
                    int count=record.read(bytes,0,bytes.length);
                    if(count<=0)throw new Exception("record read="+count);
                    out.write(bytes,0,count);total+=count;
                }
            } finally { record.stop();record.release(); }
            System.out.println("RECORD_OK bytes="+total);
        } else throw new IllegalArgumentException(action);
    }
}
