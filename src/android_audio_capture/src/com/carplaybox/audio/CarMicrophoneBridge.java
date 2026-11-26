package com.carplaybox.audio;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecordingConfiguration;
import android.media.AudioTrack;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.List;

/** Car RTP terminates in a recorder-only policy mix, never in media playback.
 * Owned independently of the media capture socket and vehicle/GPS service. */
public final class CarMicrophoneBridge implements AutoCloseable {
    private static final String TAG = "CarMicrophoneBridge";
    private static final String SOCKET = "carplay_audio_microphone_v1";
    private final Context context;
    private final AudioManager audio;
    private volatile boolean running;
    private final RecordingDemand demand = new RecordingDemand();
    private volatile LocalSocket connection;
    private Thread worker;
    private volatile HandlerThread callbacks;
    private boolean watching;
    private static final long DEMAND_REFRESH_MS = 500;
    static final Object ROUTING_LOCK = new Object();
    private Object policy;
    private AudioTrack track;
    private int rate;
    private int channels;
    private static volatile String status = "停止";
    static final class RecordingDemand {
        private boolean active;
        private long stoppedAt = -1;
        synchronized void update(boolean next, long now) {
            if (active && !next) stoppedAt = now;
            if (next) stoppedAt = -1;
            active = next;
        }
        synchronized boolean needed(long now) {
            return active || (stoppedAt >= 0 && now - stoppedAt < 250);
        }
        synchronized void clear() { active = false; stoppedAt = -1; }
    }
    private final AudioManager.AudioRecordingCallback recordingCallback =
            new AudioManager.AudioRecordingCallback() {
        @Override public void onRecordingConfigChanged(List<AudioRecordingConfiguration> configs) {
            updateRecordings(configs);
        }
    };
    private boolean identityWarning;

    public CarMicrophoneBridge(Context context) {
        this.context = context.getApplicationContext();
        this.audio = (AudioManager) this.context.getSystemService(Context.AUDIO_SERVICE);
    }
    public static String currentStatus() { return status; }
    private static void publish(String message) { if (!message.equals(status)) { status = message; Log.i(TAG, message); } }
    public synchronized void start() {
        if (running) return;
        running = true;
        worker = new Thread(this::run, "carplay-microphone");
        worker.start();
    }
    // No vehicle microphone socket exists when the user selects local input.
    // Keep Android recording policies and monitoring untouched in that mode.
    private void startWatchingRecorders() {
        final HandlerThread owner = new HandlerThread("carplay-microphone-state");
        callbacks = owner;
        owner.start();
        Handler callbackHandler = new Handler(owner.getLooper());
        audio.registerAudioRecordingCallback(recordingCallback, callbackHandler);
        watching = true;
        // Recover missed recorder callbacks on this handler, never on the PCM worker.
        callbackHandler.post(new Runnable() {
            private boolean warned;
            @Override public void run() {
                if (!running || callbacks != owner) return;
                try {
                    updateRecordings(audio.getActiveRecordingConfigurations());
                    warned = false;
                } catch (RuntimeException unavailable) {
                    if (!warned) Log.w(TAG, "Cannot refresh microphone state", unavailable);
                    warned = true;
                } finally {
                    if (running && callbacks == owner) callbackHandler.postDelayed(this, DEMAND_REFRESH_MS);
                }
            }
        });
    }
    private void stopWatchingRecorders() {
        HandlerThread owner = callbacks;
        if (owner == null) return;
        callbacks = null;
        try {
            if (watching) audio.unregisterAudioRecordingCallback(recordingCallback);
        } catch (RuntimeException error) {
            Log.w(TAG, "Cannot unregister recording callback", error);
        } finally {
            watching = false;
            owner.quitSafely();
            demand.clear();
        }
    }
    static boolean isMicrophoneSource(int source) {
        return MicrophoneDemandPolicy.isMicrophoneSource(source);
    }
    void updateRecordings(List<AudioRecordingConfiguration> configs) {
        if (configs == null) return;
        boolean active = false;
        int ownUid = android.os.Process.myUid();
        for (AudioRecordingConfiguration config : configs) {
            if (config == null || !isMicrophoneSource(config.getClientAudioSource())) continue;
            try {
                int uid = (Integer) AudioRecordingConfiguration.class.getMethod("getClientUid").invoke(config);
                if (MicrophoneDemandPolicy.requests(config.getClientAudioSource(), uid, ownUid)) {
                    active = true;
                    break;
                }
            } catch (ReflectiveOperationException unavailable) {
                if (!identityWarning) {
                    Log.w(TAG, "Recording identity unavailable; ignoring unidentified recorder", unavailable);
                    identityWarning = true;
                }
            }
        }
        demand.update(active, SystemClock.elapsedRealtime());
    }
    boolean microphoneDemand() {
        return demand.needed(SystemClock.elapsedRealtime());
    }
    static int number(byte[] bytes, int offset) {
        return (bytes[offset] & 255) | ((bytes[offset + 1] & 255) << 8)
                | ((bytes[offset + 2] & 255) << 16) | ((bytes[offset + 3] & 255) << 24);
    }
    private static void number(byte[] bytes, int offset, int value) {
        for (int i = 0; i < 4; i++) bytes[offset + i] = (byte) (value >>> (i * 8));
    }
    static byte[] encodeCommand(int type) throws IOException {
        if (type < 3 || type > 6) throw new IOException("非法麦克风命令");
        byte[] header = new byte[20];
        header[0] = 'C'; header[1] = 'P'; header[2] = 'M'; header[3] = '1';
        number(header, 4, type);
        return header;
    }
    static int validateHeader(byte[] header) throws IOException {
        if (header.length != 20 || header[0] != 'C' || header[1] != 'P'
                || header[2] != 'M' || header[3] != '1') throw new IOException("麦克风帧头无效");
        int type = number(header, 4), size = number(header, 8), rate = number(header, 12);
        if ((type != 1 && type != 2) || size < 0 || size > 4096 || (size & 1) != 0
                || (type == 1 && size != 0) || (type == 2 && size == 0) || number(header, 16) != 1
                || !(rate == 8000 || rate == 16000 || rate == 24000 || rate == 32000
                     || rate == 44100 || rate == 48000)) throw new IOException("麦克风格式无效");
        return type;
    }
    private void run() {
        while (running) {
            try {
                LocalSocket socket = new LocalSocket();
                connection = socket;
                socket.connect(new LocalSocketAddress(SOCKET, LocalSocketAddress.Namespace.ABSTRACT));
                if (!running) break;
                startWatchingRecorders();
                pump(socket);
            } catch (Exception error) {
                if (running) publish("Connection refused".equals(error.getMessage())
                        ? "使用 Android 本地录音输入" : "等待车载麦克风：" + error.getMessage());
            } finally {
                releasePolicy();
                stopWatchingRecorders();
                LocalSocket socket = connection; connection = null;
                if (socket != null) try { socket.close(); } catch (IOException ignored) { }
            }
            if (running) try { Thread.sleep(1000); } catch (InterruptedException ignored) { break; }
        }
        publish("停止");
    }
    private void pump(LocalSocket socket) throws Exception {
        InputStream input = socket.getInputStream();
        OutputStream output = socket.getOutputStream();
        byte[] header = new byte[20], payload = null, pending = null, silence = null;
        int headerUsed = 0, payloadUsed = 0, payloadSize = 0, pendingOffset = 0;
        long heartbeat = 0, lastPcm = 0, lastSilence = 0;
        boolean lastDemand = false, announced = false, received = false;
        ArrayDeque<byte[]> queue = new ArrayDeque<>();
        StructPollfd descriptor = new StructPollfd();
        descriptor.fd = socket.getFileDescriptor();
        descriptor.events = (short) OsConstants.POLLIN;
        StructPollfd[] descriptors = new StructPollfd[] { descriptor };
        while (running) {
            // Android 8.1 LocalSocket maps receive timeouts to plain IOException.
            // Poll instead: timeout is normal, EOF/error still tears down policy.
            int ready = Os.poll(descriptors, 10);
            if ((descriptor.revents & (OsConstants.POLLHUP | OsConstants.POLLERR | OsConstants.POLLNVAL)) != 0)
                throw new IOException("麦克风通道已断开");
            if (ready > 0 && (descriptor.revents & OsConstants.POLLIN) != 0) {
                if (headerUsed < 20) {
                    int n = input.read(header, headerUsed, 20 - headerUsed);
                    if (n < 0) throw new IOException("麦克风通道已断开");
                    headerUsed += n;
                    if (headerUsed == 20) {
                        int type = validateHeader(header);
                        payloadSize = number(header, 8); payloadUsed = 0;
                        if (type == 1) {
                            if (policy != null) throw new IOException("重复麦克风格式");
                            installPolicy(number(header, 12), number(header, 16));
                            silence = new byte[rate / 50 * channels * 2];
                            output.write(encodeCommand(5)); output.flush();
                            publish("虚拟麦克风就绪，等待应用录音");
                            headerUsed = 0;
                        } else {
                            if (track == null || number(header, 12) != rate || number(header, 16) != channels)
                                throw new IOException("麦克风会话格式不匹配");
                            payload = new byte[payloadSize];
                        }
                    }
                } else if (payload != null) {
                    int n = input.read(payload, payloadUsed, payloadSize - payloadUsed);
                    if (n < 0) throw new IOException("麦克风数据已断开");
                    payloadUsed += n;
                    if (payloadUsed == payloadSize) {
                        if (queue.size() >= 8) queue.removeFirst();
                        if (payloadSize > 0) queue.addLast(payload);
                        lastPcm = SystemClock.elapsedRealtime();
                        if (!received) { received = true; publish("已收到车载麦克风声音"); }
                        payload = null; headerUsed = 0;
                    }
                }
            }
            long now = SystemClock.elapsedRealtime();
            if (track != null) {
                boolean demand = microphoneDemand();
                if (!announced || demand != lastDemand || now - heartbeat >= 2000) {
                    output.write(encodeCommand(demand ? 3 : 4)); output.flush();
                    lastDemand = demand; announced = true; heartbeat = now;
                    if (!demand) { queue.clear(); pending = null; pendingOffset = 0; }
                }
                if (pending == null && !queue.isEmpty()) { pending = queue.removeFirst(); pendingOffset = 0; }
                if (pending == null && now - lastPcm >= 100 && now - lastSilence >= 20) {
                    pending = silence; pendingOffset = 0; lastSilence = now;
                }
                if (pending != null) {
                    int n = track.write(pending, pendingOffset, pending.length - pendingOffset, AudioTrack.WRITE_NON_BLOCKING);
                    if (n < 0) throw new IOException("虚拟麦克风写入失败 " + n);
                    pendingOffset += n;
                    if (pendingOffset == pending.length) { pending = null; pendingOffset = 0; }
                }
            }
        }
    }
    private void installPolicy(int sampleRate, int channelCount) throws Exception {
        if (context.checkSelfPermission("android.permission.MODIFY_AUDIO_ROUTING") != PackageManager.PERMISSION_GRANTED)
            throw new IOException("缺少 MODIFY_AUDIO_ROUTING 权限");
        Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
        Class<?> ruleBuilderClass = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();
        Method preset = AudioAttributes.Builder.class.getMethod("setCapturePreset", int.class);
        Method addRule = ruleBuilderClass.getMethod("addRule", AudioAttributes.class, int.class);
        for (int source : MicrophoneDemandPolicy.routedSources()) {
            AudioAttributes.Builder attributes = new AudioAttributes.Builder();
            preset.invoke(attributes, source);
            addRule.invoke(ruleBuilder, attributes.build(), 2);
        }
        Object rule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);
        Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
        Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(rule);
        AudioFormat format = new AudioFormat.Builder().setSampleRate(sampleRate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
        mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
        mixBuilderClass.getMethod("setRouteFlags", int.class).invoke(mixBuilder, 2);
        Object mix = mixBuilderClass.getMethod("build").invoke(mixBuilder);
        Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
        Class<?> policyBuilderClass = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object policyBuilder = policyBuilderClass.getConstructor(Context.class).newInstance(context);
        policyBuilderClass.getMethod("setLooper", Looper.class).invoke(policyBuilder, callbacks.getLooper());
        policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
        policy = policyBuilderClass.getMethod("build").invoke(policyBuilder);
        int result = (Integer) AudioManager.class.getMethod("registerAudioPolicy", policyClass).invoke(audio, policy);
        if (result != 0) throw new IOException("录音策略注册失败 " + result); // hidden AudioManager.SUCCESS
        track = (AudioTrack) policyClass.getMethod("createAudioTrackSource", mixClass).invoke(policy, mix);
        if (track == null || track.getState() != AudioTrack.STATE_INITIALIZED)
            throw new IOException("虚拟麦克风无法初始化");
        rate = sampleRate; channels = channelCount;
        track.play(); // Keep this input device available for the whole CarPlay session.
    }
    private void releasePolicy() {
        if (track != null) {
            try { track.stop(); } catch (IllegalStateException ignored) { }
            track.release(); track = null;
        }
        if (policy != null) {
            try { AudioManager.class.getMethod("unregisterAudioPolicyAsync", policy.getClass()).invoke(audio, policy); }
            catch (Exception error) { Log.w(TAG, "unregister microphone policy", error); }
            policy = null;
        }
        rate = channels = 0;
    }
    @Override public synchronized void close() {
        running = false;
        LocalSocket socket = connection;
        if (socket != null) try { socket.close(); } catch (IOException ignored) { }
        if (worker != null) {
            worker.interrupt();
            try { worker.join(1500); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            worker = null;
        }
        demand.clear();
    }
}
