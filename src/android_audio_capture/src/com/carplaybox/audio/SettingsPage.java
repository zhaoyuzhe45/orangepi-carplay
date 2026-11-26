package com.carplaybox.audio;

import android.app.Activity;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.util.concurrent.ExecutorService;

final class SettingsPage {
    private static final int MAX_VIDEO_BITRATE = 2000000;
    private static final String PREFS = "carplay_console";
    private static final String PREF_PORT = "api_port";
    private final Activity activity;
    private final CarPlayApi api;
    private final ExecutorService worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final LinearLayout view;
    private final ProgressBar progress;
    private final TextView result;
    private boolean loading;
    private boolean busy;
    private boolean suppressDpiSelection;

    private final CheckBox enabled;
    private final CheckBox bootStart;
    private final EditText displayWidth;
    private final EditText displayHeight;
    private final EditText displayDensity;
    private final Spinner densitySpinner;
    private final Spinner physicalModeSpinner;
    private final EditText fallbackWidth;
    private final EditText fallbackHeight;
    private final EditText videoFps;
    private final EditText videoBitrate;
    private final CheckBox mediaAudio;
    private final CheckBox microphone;
    private final CheckBox carGps;
    private final CheckBox carAppearance;
    private final EditText reconnectInitial;
    private final EditText reconnectMax;
    private final EditText controlPort;
    private final EditText logLevel;
    private final EditText logFileBytes;
    private final EditText logRetentionDays;
    private final EditText logTotalBytes;

    private static final String[] DPI_LABELS = {
            "自动（0）", "120", "140", "160", "180", "200", "240", "280", "320", "400", "480", "自定义"
    };
    private static final int[] DPI_VALUES = {0, 120, 140, 160, 180, 200, 240, 280, 320, 400, 480, -1};
    private static final String[] PHYSICAL_MODE_LABELS = {
            "自动（选择最接近的预制分辨率）", "800×480 @ 60", "1024×480 @ 60",
            "1280×600 @ 60", "1280×720 @ 60", "1920×900 @ 60", "1920×1080 @ 60"
    };
    private static final String[] PHYSICAL_MODE_VALUES = {
            "auto", "800x480@60", "1024x480@60", "1280x600@60",
            "1280x720@60", "1920x900@60", "1920x1080@60"
    };

    SettingsPage(Activity activity, CarPlayApi api, ExecutorService worker) {
        this.activity = activity;
        this.api = api;
        this.worker = worker;
        LinearLayout outer = Ui.vertical(activity);
        outer.setPadding(Ui.dp(activity, 18), Ui.dp(activity, 10),
                Ui.dp(activity, 18), Ui.dp(activity, 10));
        LinearLayout content = Ui.vertical(activity);
        content.setPadding(0, Ui.dp(activity, 2), 0, Ui.dp(activity, 20));
        LinearLayout heading = Ui.horizontal(activity);
        heading.addView(Ui.title(activity, "配置"), Ui.weight());
        progress = Ui.progress(activity);
        heading.addView(progress, new LinearLayout.LayoutParams(Ui.dp(activity, 28), Ui.dp(activity, 28)));
        Button reload = Ui.button(activity, "重新读取", view -> load());
        Button save = Ui.button(activity, "保存并重启服务", view -> save());
        heading.addView(reload);
        heading.addView(save);
        outer.addView(heading, Ui.matchWrap());
        result = Ui.text(activity, "Tab/方向键切换项目，Enter 确认；所有修改保存后生效。", 12, Ui.MUTED);
        outer.addView(result, Ui.matchWrap());

        content.addView(Ui.section(activity, "启动与显示"), Ui.matchWrap());
        enabled = check("启用 CarPlay 服务");
        bootStart = check("开机自动启动");
        content.addView(enabled, Ui.matchWrap());
        content.addView(bootStart, Ui.matchWrap());
        displayWidth = number();
        displayHeight = number();
        addPair(content, "协商尺寸（0×0 跟随车机）", displayWidth, displayHeight);
        physicalModeSpinner = new Spinner(activity);
        ArrayAdapter<String> physicalAdapter = new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_item, PHYSICAL_MODE_LABELS);
        physicalAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        physicalModeSpinner.setAdapter(physicalAdapter);
        addRow(content, "物理输出模式", physicalModeSpinner);
        content.addView(Ui.text(activity,
                "自动匹配预制分辨率，整幅画面拉伸至车机尺寸。切换档位后自动重连；成功输出画面后记住档位，下次开机直接使用。首次默认 800×480。",
                12, Ui.MUTED), Ui.matchWrap());
        displayDensity = number();
        densitySpinner = new Spinner(activity);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_item, DPI_LABELS);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        densitySpinner.setAdapter(adapter);
        addRow(content, "DPI 常见值", densitySpinner);
        addRow(content, "DPI 数值（0 自动，120–480）", displayDensity);
        densitySpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View selectedView,
                                                  int position, long id) {
                if (!loading && !suppressDpiSelection && position < DPI_VALUES.length && DPI_VALUES[position] >= 0)
                    displayDensity.setText(String.valueOf(DPI_VALUES[position]));
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        fallbackWidth = number();
        fallbackHeight = number();
        addPair(content, "车机未报告时的回退尺寸", fallbackWidth, fallbackHeight);
        TextView dpiHint = Ui.text(activity,
                "1080p 自动映射 280，800×480 自动映射 160；手动 DPI 对所有协商分辨率生效。",
                12, Ui.MUTED);
        content.addView(dpiHint, Ui.matchWrap());

        content.addView(Ui.section(activity, "视频"), Ui.matchWrap());
        videoFps = number();
        videoBitrate = number();
        addPair(content, "帧率上限 / 码率 bps（最高 2000000）", videoFps, videoBitrate);
        content.addView(Ui.text(activity,
                "帧率取配置值与车机最大值的较小者；分辨率不做画面缩放。", 12, Ui.MUTED), Ui.matchWrap());

        content.addView(Ui.section(activity, "输入与车辆数据"), Ui.matchWrap());
        mediaAudio = check("启用车机媒体音频（默认媒体通道）");
        microphone = check("启用车机麦克风");
        carGps = check("使用车机 GPS 数据");
        carAppearance = check("使用车机白天/夜间模式");
        content.addView(mediaAudio, Ui.matchWrap());
        content.addView(microphone, Ui.matchWrap());
        content.addView(Ui.text(activity, "关闭时使用盒子本地麦克风，音乐保留媒体音质。", 12, Ui.MUTED), Ui.matchWrap());
        content.addView(carGps, Ui.matchWrap());
        content.addView(carAppearance, Ui.matchWrap());

        content.addView(Ui.section(activity, "连接与本地管理"), Ui.matchWrap());
        reconnectInitial = number();
        reconnectMax = number();
        addPair(content, "重连初始 / 最大毫秒", reconnectInitial, reconnectMax);
        controlPort = number();
        logLevel = number();
        addPair(content, "本地 API 端口 / 日志等级 0–3", controlPort, logLevel);

        content.addView(Ui.section(activity, "日志存储策略"), Ui.matchWrap());
        logFileBytes = number();
        logRetentionDays = number();
        addPair(content, "单文件字节上限 / 保留天数", logFileBytes, logRetentionDays);
        logTotalBytes = number();
        addRow(content, "日志总容量字节上限", logTotalBytes);

        ScrollView form = new ScrollView(activity);
        form.setFillViewport(true);
        form.addView(content, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        view = outer;
        view.setBackgroundColor(Ui.BACKGROUND);
        outer.addView(form, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
    }

    View view() { return view; }

    void activate() {
        if (!loading && !busy) load();
    }

    void deactivate() {}

    private void load() {
        if (busy) return;
        busy = true;
        progress.setVisibility(View.VISIBLE);
        result.setText("正在读取配置…");
        worker.execute(() -> {
            try {
                CarPlayApi.Result response = api.get("/api/config");
                if (response.code != 200) throw new Exception(apiError(response));
                JSONObject config = new JSONObject(response.body);
                main.post(() -> populate(config));
            } catch (Throwable failure) {
                main.post(() -> showResult("读取失败：" + message(failure), true));
            } finally {
                main.post(() -> {
                    busy = false;
                    progress.setVisibility(View.GONE);
                });
            }
        });
    }

    private void populate(JSONObject config) {
        loading = true;
        enabled.setChecked(config.optBoolean("enabled", true));
        bootStart.setChecked(config.optBoolean("bootStart", true));
        displayWidth.setText(String.valueOf(config.optInt("displayWidth", 0)));
        displayHeight.setText(String.valueOf(config.optInt("displayHeight", 0)));
        String physicalMode = config.optString("physicalMode", "auto");
        physicalModeSpinner.setSelection(physicalModeIndex(physicalMode), false);
        int density = config.optInt("displayDensity", 0);
        displayDensity.setText(String.valueOf(density));
        suppressDpiSelection = true;
        int match = -1;
        for (int i = 0; i < DPI_VALUES.length; i++) {
            if (DPI_VALUES[i] == density) { match = i; break; }
        }
        densitySpinner.setSelection(match >= 0 ? match : DPI_VALUES.length - 1);
        suppressDpiSelection = false;
        fallbackWidth.setText(String.valueOf(config.optInt("fallbackWidth", 800)));
        fallbackHeight.setText(String.valueOf(config.optInt("fallbackHeight", 480)));
        videoFps.setText(String.valueOf(config.optInt("videoFps", 30)));
        videoBitrate.setText(String.valueOf(Math.min(
                config.optInt("videoBitrate", MAX_VIDEO_BITRATE), MAX_VIDEO_BITRATE)));
        mediaAudio.setChecked(config.optBoolean("mediaAudio", true));
        microphone.setChecked(config.optBoolean("microphone", true));
        carGps.setChecked(config.optBoolean("carGps", true));
        carAppearance.setChecked(config.optBoolean("carAppearance", true));
        reconnectInitial.setText(String.valueOf(config.optInt("reconnectInitialMs", 1000)));
        reconnectMax.setText(String.valueOf(config.optInt("reconnectMaxMs", 30000)));
        controlPort.setText(String.valueOf(config.optInt("controlPort", CarPlayApi.DEFAULT_PORT)));
        logLevel.setText(String.valueOf(config.optInt("logLevel", 1)));
        logFileBytes.setText(String.valueOf(config.optLong("logFileBytes", 8388608L)));
        logRetentionDays.setText(String.valueOf(config.optInt("logRetentionDays", 7)));
        logTotalBytes.setText(String.valueOf(config.optLong("logTotalBytes", 268435456L)));
        int configuredPort = config.optInt("controlPort", CarPlayApi.DEFAULT_PORT);
        api.setPort(configuredPort);
        activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putInt(PREF_PORT, configuredPort).apply();
        loading = false;
        showResult("配置已读取。DPI 选择 0 时使用分辨率自动映射。", false);
    }

    private void save() {
        if (busy) return;
        final JSONObject config = new JSONObject();
        final int nextPort;
        try {
            config.put("enabled", enabled.isChecked());
            config.put("bootStart", bootStart.isChecked());
            config.put("displayWidth", integer(displayWidth, "协商宽度"));
            config.put("displayHeight", integer(displayHeight, "协商高度"));
            config.put("displayDensity", integer(displayDensity, "DPI"));
            int physicalIndex = physicalModeSpinner.getSelectedItemPosition();
            if (physicalIndex < 0 || physicalIndex >= PHYSICAL_MODE_VALUES.length)
                throw new IllegalArgumentException("请选择物理输出模式");
            config.put("physicalMode", PHYSICAL_MODE_VALUES[physicalIndex]);
            config.put("fallbackWidth", integer(fallbackWidth, "回退宽度"));
            config.put("fallbackHeight", integer(fallbackHeight, "回退高度"));
            config.put("videoFps", integer(videoFps, "帧率"));
            config.put("videoBitrate", Math.min(integer(videoBitrate, "码率"),
                    MAX_VIDEO_BITRATE));
            config.put("mediaAudio", mediaAudio.isChecked());
            config.put("microphone", microphone.isChecked());
            config.put("carGps", carGps.isChecked());
            config.put("carAppearance", carAppearance.isChecked());
            config.put("reconnectInitialMs", integer(reconnectInitial, "初始重连时间"));
            config.put("reconnectMaxMs", integer(reconnectMax, "最大重连时间"));
            nextPort = integer(controlPort, "API 端口");
            config.put("controlPort", nextPort);
            config.put("logLevel", integer(logLevel, "日志等级"));
            config.put("logFileBytes", longInteger(logFileBytes, "日志单文件大小"));
            config.put("logRetentionDays", integer(logRetentionDays, "日志保留天数"));
            config.put("logTotalBytes", longInteger(logTotalBytes, "日志总容量"));
        } catch (Throwable failure) {
            showResult("配置有误：" + message(failure), true);
            return;
        }
        busy = true;
        progress.setVisibility(View.VISIBLE);
        result.setText("正在保存…");
        worker.execute(() -> {
            try {
                CarPlayApi.Result response = api.putJson("/api/config", config.toString());
                if (response.code != 200) throw new Exception(apiError(response));
                activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                        .putInt(PREF_PORT, nextPort).apply();
                api.setPort(nextPort);
                main.post(() -> showResult("配置已保存，服务正在重启。等待数秒后状态会恢复。", false));
            } catch (Throwable failure) {
                main.post(() -> showResult("保存失败：" + message(failure), true));
            } finally {
                main.post(() -> {
                    busy = false;
                    progress.setVisibility(View.GONE);
                });
            }
        });
    }

    private CheckBox check(String label) {
        CheckBox box = new CheckBox(activity);
        box.setText(label);
        box.setTextColor(Ui.TEXT);
        box.setTextSize(14);
        return box;
    }

    private EditText number() {
        return Ui.number(activity, "0");
    }

    private void addRow(LinearLayout parent, String label, View field) {
        LinearLayout row = Ui.horizontal(activity);
        TextView text = Ui.text(activity, label, 13, Ui.MUTED);
        row.addView(text, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 0.48f));
        row.addView(field, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 0.52f));
        parent.addView(row, Ui.matchWrap());
    }

    private void addPair(LinearLayout parent, String label, EditText first, EditText second) {
        LinearLayout row = Ui.horizontal(activity);
        TextView text = Ui.text(activity, label, 13, Ui.MUTED);
        row.addView(text, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 0.48f));
        row.addView(first, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 0.26f));
        row.addView(second, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 0.26f));
        parent.addView(row, Ui.matchWrap());
    }

    private static int physicalModeIndex(String value) {
        for (int i = 0; i < PHYSICAL_MODE_VALUES.length; i++) {
            if (PHYSICAL_MODE_VALUES[i].equals(value)) return i;
        }
        return 0;
    }

    private static int integer(EditText field, String label) {
        try { return Integer.parseInt(field.getText().toString().trim()); }
        catch (Throwable failure) { throw new IllegalArgumentException(label + "必须是整数"); }
    }

    private static long longInteger(EditText field, String label) {
        try { return Long.parseLong(field.getText().toString().trim()); }
        catch (Throwable failure) { throw new IllegalArgumentException(label + "必须是整数"); }
    }

    private void showResult(String text, boolean isError) {
        result.setText(text);
        result.setTextColor(isError ? Ui.ERROR : Ui.MUTED);
        if (isError) Toast.makeText(activity, text, Toast.LENGTH_LONG).show();
    }

    private static String apiError(CarPlayApi.Result result) {
        try { return new JSONObject(result.body).optString("error", result.body); }
        catch (Throwable ignored) { return "HTTP " + result.code + " " + result.body; }
    }

    private static String message(Throwable failure) {
        return failure.getMessage() == null ? failure.getClass().getSimpleName() : failure.getMessage();
    }
}
