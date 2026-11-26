package com.carplaybox.audio;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;

final class LogsPage {
    private final Activity activity;
    private final CarPlayApi api;
    private final ExecutorService worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final LinearLayout view;
    private final ScrollView logScroll;
    private final TextView logs;
    private final TextView metrics;
    private final ProgressBar progress;
    private boolean active;
    private boolean loading;
    private final Runnable refreshTask = new Runnable() {
        @Override public void run() {
            if (active) refresh(true);
        }
    };

    LogsPage(Activity activity, CarPlayApi api, ExecutorService worker) {
        this.activity = activity;
        this.api = api;
        this.worker = worker;
        LinearLayout content = Ui.vertical(activity);
        content.setPadding(Ui.dp(activity, 18), Ui.dp(activity, 10),
                Ui.dp(activity, 18), Ui.dp(activity, 10));
        LinearLayout heading = Ui.horizontal(activity);
        heading.addView(Ui.title(activity, "运行日志"), Ui.weight());
        progress = Ui.progress(activity);
        heading.addView(progress, new LinearLayout.LayoutParams(Ui.dp(activity, 28), Ui.dp(activity, 28)));
        Button refresh = Ui.button(activity, "刷新", view -> refresh(false));
        heading.addView(refresh);
        content.addView(heading, Ui.matchWrap());
        metrics = Ui.text(activity,
                "每 5 秒刷新；日志按单文件大小、总容量和保留天数自动轮转清理。", 12, Ui.MUTED);
        content.addView(metrics, Ui.matchWrap());
        logs = Ui.text(activity, "正在读取日志…", 12, Ui.TEXT);
        logs.setTypeface(android.graphics.Typeface.MONOSPACE);
        logs.setTextIsSelectable(true);
        logs.setLineSpacing(0, 1.05f);
        logs.setPadding(Ui.dp(activity, 8), Ui.dp(activity, 6),
                Ui.dp(activity, 8), Ui.dp(activity, 12));
        logScroll = new ScrollView(activity);
        logScroll.setFillViewport(true);
        logScroll.setBackgroundColor(Ui.PANEL);
        logScroll.addView(logs, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        content.addView(logScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        view = content;
        view.setBackgroundColor(Ui.BACKGROUND);
    }

    View view() { return view; }

    void activate() {
        active = true;
        refresh(false);
    }

    void deactivate() {
        active = false;
        main.removeCallbacks(refreshTask);
    }

    private void refresh(boolean automatic) {
        if (loading) return;
        loading = true;
        progress.setVisibility(View.VISIBLE);
        worker.execute(() -> {
            try {
                CarPlayApi.Result response = api.get("/api/logs");
                if (response.code != 200) throw new Exception("HTTP " + response.code + " " + response.body);
                JSONObject root = new JSONObject(response.body);
                String text = root.optString("text", "");
                long activeBytes = root.optLong("activeBytes");
                long totalBytes = root.optLong("totalBytes");
                long rotated = root.optLong("rotatedFiles");
                String header = "刷新 " + new SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(new Date())
                        + "    当前文件 " + human(activeBytes)
                        + "    总占用 " + human(totalBytes)
                        + "    轮转文件 " + rotated;
                main.post(() -> {
                    metrics.setText(header);
                    logs.setText(text.isEmpty() ? "日志为空" : text);
                    logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
                });
            } catch (Throwable failure) {
                String message = failure.getMessage() == null
                        ? failure.getClass().getSimpleName() : failure.getMessage();
                main.post(() -> metrics.setText("日志读取失败：" + message + (automatic ? "（自动刷新）" : "")));
            } finally {
                main.post(() -> {
                    loading = false;
                    progress.setVisibility(View.GONE);
                    if (active) {
                        main.removeCallbacks(refreshTask);
                        main.postDelayed(refreshTask, 5000);
                    }
                });
            }
        });
    }

    private static String human(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(Locale.US, "%.1f KiB", bytes / 1024.0);
        return String.format(Locale.US, "%.1f MiB", bytes / 1048576.0);
    }
}