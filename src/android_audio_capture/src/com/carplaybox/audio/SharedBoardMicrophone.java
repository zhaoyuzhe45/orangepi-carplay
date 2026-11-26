package com.carplaybox.audio;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicReference;

/** Relays the fixed board input to ordinary recording sources on the verified Oreo HAL. */
final class SharedBoardMicrophone implements AutoCloseable {
    private static final String TAG = "SharedBoardMicrophone";
    private static final int SAMPLE_RATE = 48000;
    private static final int HOTWORD = 1999; // Android 8.1's hidden AUDIO_SOURCE_HOTWORD.
    private static final int MATCH_CAPTURE_PRESET = 2;
    private static final int LOOP_BACK = 2;
    private static final int PCM16 = AudioFormat.ENCODING_PCM_16BIT;
    private static final int UNREGISTER_TIMEOUT_MS = 1000;
    // Also protect services recreated before the old worker has confirmed its route removal.
    private enum PolicyOwnership { FREE, OWNED, CLEANUP_UNCONFIRMED }
    private static final AtomicReference<PolicyOwnership> policyOwner =
            new AtomicReference<>(PolicyOwnership.FREE);
    private final SharedMicrophoneRelay relay;

    SharedBoardMicrophone(Context context) {
        relay = new SharedMicrophoneRelay(new AndroidEndpoints(context.getApplicationContext()), SAMPLE_RATE);
    }

    void start() { relay.start(); }
    boolean isActive() { return relay.isActive(); }
    long droppedFrames() { return relay.droppedFrames(); }
    String statistics() { return relay.statistics(); }
    @Override public void close() { relay.close(); }

    private static final class AndroidEndpoints implements SharedMicrophoneRelay.Backend {
        private final Context context;
        private AudioManager audio;

        AndroidEndpoints(Context context) { this.context = context; }

        @Override public String unavailableReason() throws Exception {
            if (policyOwner.get() == PolicyOwnership.CLEANUP_UNCONFIRMED) {
                return "previous recorder policy cleanup unconfirmed";
            }
            for (String permission : new String[] { "android.permission.RECORD_AUDIO",
                    "android.permission.MODIFY_AUDIO_ROUTING", "android.permission.CAPTURE_AUDIO_HOTWORD" }) {
                if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                    return "missing " + permission;
                }
            }
            audio = context.getSystemService(AudioManager.class);
            if (audio == null) throw new IOException("AudioManager unavailable");
            if (builtIn() == null) throw new IOException("built-in microphone unavailable");
            Method getParameters;
            try {
                getParameters = AudioManager.class.getMethod("getParameters", String.class);
            } catch (NoSuchMethodException unsupported) {
                return "audio parameters API unavailable";
            }
            String parameters = (String) getParameters.invoke(audio, SharedMicrophoneRelay.VERSION_KEY);
            if (!SharedMicrophoneRelay.supportedVersion(parameters)) throw new IOException("shared microphone HAL unavailable");
            return null;
        }

        @Override public SharedMicrophoneRelay.Output openOutput() throws Exception {
            if (!policyOwner.compareAndSet(PolicyOwnership.FREE, PolicyOwnership.OWNED)) {
                throw new IOException("previous shared mic policy is still owned");
            }
            PolicyOutput output = new PolicyOutput(audio);
            boolean started = false;
            try {
                output.start(context);
                started = true;
                return output;
            } finally {
                if (!started) output.close();
            }
        }

        private AudioDeviceInfo builtIn() {
            for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (device.isSource() && device.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC) return device;
            }
            return null;
        }

        @Override public SharedMicrophoneRelay.Input openInput() throws IOException {
            AudioDeviceInfo preferred = builtIn();
            if (preferred == null) throw new IOException("built-in microphone disappeared");
            int min = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, PCM16);
            if (min <= 0) throw new IOException("48 kHz mono capture unsupported: " + min);
            AudioRecord record = new AudioRecord(HOTWORD, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                    PCM16, Math.max(min, SAMPLE_RATE * 2 / 10));
            BoardInput input = new BoardInput(record, preferred.getId());
            boolean started = false;
            try {
                if (record.getState() != AudioRecord.STATE_INITIALIZED) throw new IOException("board mic init failed");
                if (!record.setPreferredDevice(preferred)) throw new IOException("select built-in microphone failed");
                record.startRecording();
                if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                    throw new IOException("board mic start failed");
                }
                started = true;
                return input;
            } finally {
                if (!started) input.close();
            }
        }

        @Override public void report(String event, Exception error) {
            if (error == null) Log.i(TAG, event);
            else Log.w(TAG, event, error);
        }
    }

    private static final class BoardInput implements SharedMicrophoneRelay.Input {
        private final AudioRecord record;
        private final int deviceId;

        BoardInput(AudioRecord record, int deviceId) {
            this.record = record;
            this.deviceId = deviceId;
        }

        @Override public int read(short[] pcm) {
            return record.read(pcm, 0, pcm.length, AudioRecord.READ_NON_BLOCKING);
        }

        @Override public boolean isBuiltIn() {
            AudioDeviceInfo routed = record.getRoutedDevice();
            return routed != null && routed.getId() == deviceId && routed.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC;
        }

        @Override public void close() throws SharedMicrophoneRelay.CleanupFailure {
            try {
                record.release(); // Also stops the recorder.
            } catch (RuntimeException error) {
                throw new SharedMicrophoneRelay.CleanupFailure("release board microphone", error);
            }
        }
    }

    private static final class PolicyOutput implements SharedMicrophoneRelay.Output {
        private final AudioManager audio;
        private Object policy;
        private Method unregister;
        private Method listPorts;
        private Class<?> devicePortClass;
        private Method portAddress;
        private boolean registered;
        private String mixAddress;
        private boolean routeWasPresent;
        private AudioTrack track;

        PolicyOutput(AudioManager audio) { this.audio = audio; }

        void start(Context context) throws Exception {
            Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
            Class<?> ruleBuilderClass = Class.forName(ruleClass.getName() + "$Builder");
            Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();
            Method capturePreset = AudioAttributes.Builder.class.getMethod("setCapturePreset", int.class);
            Method addRule = ruleBuilderClass.getMethod("addRule", AudioAttributes.class, int.class);
            for (int source : MicrophoneDemandPolicy.routedSources()) {
                // Oreo's recorder mix cannot exclude a UID. Never mix the collector source.
                if (source == HOTWORD) throw new IOException("collector source in recorder mix");
                AudioAttributes.Builder attrs = new AudioAttributes.Builder();
                capturePreset.invoke(attrs, source);
                addRule.invoke(ruleBuilder, attrs.build(), MATCH_CAPTURE_PRESET);
            }
            Object rule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);
            Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
            Class<?> mixBuilderClass = Class.forName(mixClass.getName() + "$Builder");
            Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(rule);
            AudioFormat format = new AudioFormat.Builder().setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(PCM16).build();
            mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
            mixBuilderClass.getMethod("setRouteFlags", int.class).invoke(mixBuilder, LOOP_BACK);
            Object mix = mixBuilderClass.getMethod("build").invoke(mixBuilder);
            Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
            Class<?> policyBuilderClass = Class.forName(policyClass.getName() + "$Builder");
            Object policyBuilder = policyBuilderClass.getConstructor(Context.class).newInstance(context);
            policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
            policy = policyBuilderClass.getMethod("build").invoke(policyBuilder);
            Method register = AudioManager.class.getMethod("registerAudioPolicy", policyClass);
            unregister = AudioManager.class.getMethod("unregisterAudioPolicyAsync", policyClass);
            listPorts = Class.forName("android.media.AudioSystem")
                    .getMethod("listAudioPorts", ArrayList.class, int[].class);
            devicePortClass = Class.forName("android.media.AudioDevicePort");
            portAddress = devicePortClass.getMethod("address");
            Method getRegistration = mixClass.getMethod("getRegistration");
            // If Binder throws during registration, assume the policy may have been installed.
            registered = true;
            int result = (Integer) register.invoke(audio, policy);
            if (result != 0) {
                registered = false;
                throw new IOException("recorder policy registration: " + result);
            }
            mixAddress = (String) getRegistration.invoke(mix);
            routeWasPresent = mixAddress != null && !mixAddress.isEmpty() && routePresent();
            if (!routeWasPresent) throw new IOException("cannot verify registered recorder route");
            track = (AudioTrack) policyClass.getMethod("createAudioTrackSource", mixClass).invoke(policy, mix);
            if (track == null || track.getState() != AudioTrack.STATE_INITIALIZED) {
                throw new IOException("virtual mic source init failed");
            }
            track.play();
            if (track.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) throw new IOException("virtual mic start failed");
        }

        @Override public int write(short[] pcm, int start, int size) {
            return track.write(pcm, start, size, AudioTrack.WRITE_NON_BLOCKING);
        }

        private boolean routePresent() throws Exception {
            // Query AudioSystem directly: AudioManager's cached port list may be stale.
            ArrayList<Object> ports = new ArrayList<>();
            int status = (Integer) listPorts.invoke(null, ports, new int[1]);
            if (status != 0) throw new IOException("list audio ports: " + status);
            for (Object port : ports) {
                if (devicePortClass.isInstance(port) && mixAddress.equals(portAddress.invoke(port))) return true;
            }
            return false;
        }

        private void awaitRouteRemoval() throws Exception {
            if (!routeWasPresent) throw new IOException("cannot verify removal of unobserved recorder route");
            // Oreo clears the Java registration locally before its oneway Binder call completes.
            // APM removes the recorder mix before disconnecting both ports with this unique address.
            long deadline = SystemClock.elapsedRealtime() + UNREGISTER_TIMEOUT_MS;
            do {
                if (!routePresent()) return;
                SystemClock.sleep(25); // Continue cleanup if close() interrupted the worker.
            } while (SystemClock.elapsedRealtime() < deadline);
            throw new IOException("recorder route still present after unregister");
        }

        @Override public void close() throws SharedMicrophoneRelay.CleanupFailure {
            Exception failure = null;
            try {
                if (track != null) track.release();
            } catch (RuntimeException error) {
                failure = error;
            }
            try {
                if (registered) {
                    unregister.invoke(audio, policy);
                    awaitRouteRemoval();
                }
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
            if (failure != null) {
                policyOwner.set(PolicyOwnership.CLEANUP_UNCONFIRMED);
                throw new SharedMicrophoneRelay.CleanupFailure("release shared mic policy", failure);
            }
            policyOwner.set(PolicyOwnership.FREE);
        }
    }
}
