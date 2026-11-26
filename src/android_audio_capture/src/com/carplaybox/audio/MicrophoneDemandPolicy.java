package com.carplaybox.audio;

import android.media.MediaRecorder.AudioSource;

/** The recorder mix and the demand check must cover the same physical inputs. */
final class MicrophoneDemandPolicy {
    private static final int[] SOURCES = { AudioSource.DEFAULT, AudioSource.MIC, AudioSource.CAMCORDER,
            AudioSource.VOICE_RECOGNITION, AudioSource.VOICE_COMMUNICATION, AudioSource.UNPROCESSED };
    static int[] routedSources() { return SOURCES.clone(); }
    static boolean isMicrophoneSource(int source) {
        for (int candidate : SOURCES) if (source == candidate) return true;
        return false;
    }
    static boolean requests(int source, int uid, int ownUid) {
        return uid >= 0 && uid != ownUid && isMicrophoneSource(source);
    }
}
