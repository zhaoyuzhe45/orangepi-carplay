package com.carplaybox.audio;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.util.concurrent.ExecutorService;

final class StatusPage {
    private static final int REQUEST_MICROPHONE = 73;
    private final Activity activity;
    private final CarPlayApi api;
    private final ExecutorService worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final TextView summary;
    private final TextView captureStatus;
    private final TextView updated;
    private final TextView error;
    private final ProgressBar progress;
    private final ScrollView view;
    private boolean active;
    private boolean loading;
    private final Runnable refreshTask = new Runnable() {
        @Override public void run() {
            if (active) refresh();
        }
    };

    StatusPage(Activity activity, CarPlayApi api, ExecutorService worker) {
        this.activity = activity;
        this.api = api;
        this.worker = worker;
        LinearLayout content = Ui.vertical(activity);
        content.setPadding(Ui.dp(activity, 18), Ui.dp(activity, 10),
                Ui.dp(activity, 18), Ui.dp(activity, 18));

        LinearLayout heading = Ui.horizontal(activity);
        heading.addView(Ui.title(activity, "运行状态"), Ui.weight());
        progress = Ui.progress(activity);
        heading.addView(progress, new LinearLayout.LayoutParams(Ui.dp(activity, 28), Ui.dp(activity, 28)));
        Button refresh = Ui.button(activity, "刷新", view -> refresh());
        heading.addView(refresh);
        content.addView(heading, Ui.matchWrap());

        updated = Ui.text(activity, "尚未读取", 12, Ui.MUTED);
        content.addView(updated, Ui.matchWrap());
        summary = Ui.text(activity, "正在连接本地 CarPlay 服务…", 15, Ui.TEXT);
        summary.setTextIsSelectable(true);
        summary.setLineSpacing(0, 1.16f);
        content.addView(summary, Ui.matchWrap());
        error = Ui.text(activity, "", 13, Ui.ERROR);
        error.setVisibility(View.GONE);
        content.addView(error, Ui.matchWrap());

        content.addView(Ui.section(activity, "音频通道测试"), Ui.matchWrap());
        captureStatus = Ui.text(activity, AudioCaptureService.currentStatus(), 13, Ui.MUTED);
        content.addView(captureStatus, Ui.matchWrap());
        LinearLayout audioButtons = Ui.horizontal(activity);
        Button start = Ui.button(activity, "启动麦克风采集", view -> startCapture());
        Button stop = Ui.button(activity, "停止采集", view ->
                activity.stopService(new Intent(activity, AudioCaptureService.class)));
        Button test = Ui.button(activity, "播放媒体测试音", view -> playTestTone());
        audioButtons.addView(start, Ui.weight());
        audioButtons.addView(stop, Ui.weight());
        audioButtons.addView(test, Ui.weight());
        content.addView(audioButtons, Ui.matchWrap());
        TextView note = Ui.text(activity,
                "测试音明确使用 USAGE_MEDIA；麦克风输入来自车机能力协商，缺少对应流时由媒体通道兜底。",
                12, Ui.MUTED);
        content.addView(note, Ui.matchWrap());

        view = new ScrollView(activity);
        view.setBackgroundColor(Ui.BACKGROUND);
        view.setFillViewport(true);
        view.addView(content, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
    }

    View view() { return view; }

    void activate() {
        active = true;
        refresh();
    }

    void deactivate() {
        active = false;
        main.removeCallbacks(refreshTask);
    }

    private void refresh() {
        if (loading) return;
        loading = true;
        progress.setVisibility(View.VISIBLE);
        worker.execute(() -> {
            String text;
            boolean ok = false;
            try {
                CarPlayApi.Result result = api.get("/api/status");
                if (result.code != 200) throw new Exception("HTTP " + result.code + " " + result.body);
                JSONObject root = new JSONObject(result.body);
                JSONObject display = root.optJSONObject("display");
                JSONObject input = root.optJSONObject("input");
                JSONObject audio = root.optJSONObject("audio");
                JSONObject vehicle = root.optJSONObject("vehicle");
                StringBuilder out = new StringBuilder();
                out.append("服务状态  ").append(root.optString("state", "unknown"))
                        .append("\nUSB 连接  ").append(yesNo(root.optBoolean("usbBound")))
                        .append("    会话代次  ").append(root.optLong("generation"))
                        .append("\n\n显示输出\n")
                        .append("  车机请求  ").append(display.optInt("requestedWidth"))
                        .append(" × ").append(display.optInt("requestedHeight")).append("\n")
                        .append("  物理输出  ").append(display.optInt("physicalWidth"))
                        .append(" × ").append(display.optInt("physicalHeight"))
                        .append("    逻辑尺寸  ").append(display.optInt("logicalWidth"))
                        .append(" × ").append(display.optInt("logicalHeight"))
                        .append("    DPI  ").append(display.optInt("density")).append("\n")
                        .append("  帧率限制  ").append(display.optInt("requestedFps")).append(" fps")
                        .append("    实际配置  ").append(display.optInt("actualFps")).append(" fps\n")
                        .append("  原生尺寸  ").append(yesNo(display.optBoolean("nativeSize")))
                        .append("    显示结果  ").append(display.optString("error", "-"))
                        .append("\n\n输入\n  触摸  ").append(yesNo(input.optBoolean("touch")))
                        .append("    键盘/旋钮  ").append(yesNo(input.optBoolean("keyboard")))
                        .append("\n\n音频\n  媒体输出  ").append(yesNo(audio.optBoolean("media")))
                        .append("    麦克风  ").append(yesNo(audio.optBoolean("microphone")))
                        .append("    CarPlay 流 ").append(audio.optInt("streams"))
                        .append("\n  麦克风可用  ").append(yesNo(audio.optBoolean("microphoneAvailable")))
                        .append("    Android 就绪  ").append(yesNo(audio.optBoolean("microphoneReady")))
                        .append("    应用录音  ").append(yesNo(audio.optBoolean("microphoneDemand")))
                        .append("    接收包数  ").append(audio.optLong("microphonePacketsReceived"))
                        .append("\n  ").append(CarMicrophoneBridge.currentStatus())
                        .append("\n\n车辆数据\n  GPS 配置启用  ").append(yesNo(vehicle.optBoolean("gps")))
                        .append("\n  定位数据接收  ").append(yesNo(vehicle.optJSONObject("location") != null && vehicle.optJSONObject("location").optBoolean("received")))
                        .append("\n  ").append(VehicleLocationService.currentStatus())
                        .append("\n\n媒体与语音\n  ").append(AppIntegrationService.currentStatus())
                        .append("\n  外观已接收  ").append(yesNo(vehicle.optBoolean("appearanceKnown")))
                        .append("    Android 已应用  ").append(yesNo(vehicle.optBoolean("appearanceApplied")))
                        .append("    夜间模式  ").append(yesNo(vehicle.optBoolean("night")))
                        .append("\n  外观错误  ").append(vehicle.optString("appearanceError", ""))
                        .append("\n  车机图标  ").append(vehicle.optBoolean("oemIcon") ? "车机已返回" : "使用 OPi4 兜底")
                        .append("    requestUI 能力  ").append(yesNo(vehicle.optBoolean("carUiSupported")))
                        .append("    事件通道  ").append(yesNo(vehicle.optBoolean("carUiReady")))
                        .append("\n\n最近错误  ").append(root.optString("lastError", "无"));
                text = out.toString();
                ok = true;
            } catch (Throwable failure) {
                text = "无法读取状态：" + shortMessage(failure);
            }
            final String message = text;
            final boolean success = ok;
            main.post(() -> {
                loading = false;
                progress.setVisibility(View.GONE);
                updated.setText(new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss",
                        java.util.Locale.getDefault()).format(new java.util.Date()));
                captureStatus.setText(AudioCaptureService.currentStatus());
                if (success) {
                    summary.setText(message);
                    summary.setTextColor(Ui.TEXT);
                    error.setVisibility(View.GONE);
                } else {
                    error.setText(message);
                    error.setVisibility(View.VISIBLE);
                }
                if (active) main.postDelayed(refreshTask, 2500);
            });
        });
    }


    private void startCapture() {
        if (Build.VERSION.SDK_INT >= 23 &&
                activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            activity.requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, REQUEST_MICROPHONE);
            return;
        }
        Intent intent = new Intent(activity, AudioCaptureService.class);
        if (Build.VERSION.SDK_INT >= 26) activity.startForegroundService(intent);
        else activity.startService(intent);
        Toast.makeText(activity, "已启动车机麦克风采集", Toast.LENGTH_SHORT).show();
    }

    private void playTestTone() {
        new Thread(() -> {
            int rate = 48000;
            int frames = rate * 2;
            short[] samples = new short[frames * 2];
            for (int i = 0; i < frames; i++) {
                double fade = Math.min(1.0, Math.min(i, frames - i) / 480.0);
                short value = (short) (Math.sin(2.0 * Math.PI * 880.0 * i / rate) * 9000.0 * fade);
                samples[i * 2] = value;
                samples[i * 2 + 1] = value;
            }
            AudioTrack player = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(rate)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                    .setTransferMode(AudioTrack.MODE_STATIC).build();
            player.write(samples, 0, samples.length);
            player.play();
            try { Thread.sleep(2200); } catch (InterruptedException ignored) {}
            player.stop();
            player.release();
        }, "carplay-media-test-tone").start();
    }

    private static String yesNo(boolean value) { return value ? "是" : "否"; }

    private static String shortMessage(Throwable failure) {
        String value = failure.getMessage();
        return value == null ? failure.getClass().getSimpleName() : value;
    }
}
