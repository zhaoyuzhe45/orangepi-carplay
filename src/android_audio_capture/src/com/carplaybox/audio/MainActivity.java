package com.carplaybox.audio;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.os.Bundle;
import android.widget.FrameLayout;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final int PAGE_STATUS = 0;
    private static final int PAGE_SETTINGS = 1;
    private static final int PAGE_LOGS = 2;
    private static final int PAGE_PHONE = 3;
    private final Button[] menu = new Button[4];
    private final View[] pages = new View[4];
    private int currentPage = -1;
    private StatusPage statusPage;
    private SettingsPage settingsPage;
    private LogsPage logsPage;
    private BluetoothPhonePage phonePage;
    private ExecutorService worker;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        CarPlayUiShortcut.requestWhenReady(this);
        CarPlayServices.start(this);
        Window window = getWindow();
        window.setStatusBarColor(Ui.BACKGROUND);
        window.setNavigationBarColor(Ui.BACKGROUND);
        worker = Executors.newSingleThreadExecutor();
        int port = getSharedPreferences("carplay_console", MODE_PRIVATE)
                .getInt("api_port", CarPlayApi.DEFAULT_PORT);
        CarPlayApi api = new CarPlayApi(port);

        LinearLayout root = Ui.vertical(this);
        root.setBackgroundColor(Ui.BACKGROUND);
        root.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 6));

        LinearLayout header = Ui.horizontal(this);
        header.addView(Ui.title(this, "CarPlay 车载控制台"), Ui.weight());
        TextView hint = Ui.text(this, "触摸 / Tab / 方向键 / Enter    F1状态  F2配置  F3日志",
                12, Ui.MUTED);
        header.addView(hint);
        root.addView(header, Ui.matchWrap());

        LinearLayout navigation = Ui.horizontal(this);
        String[] labels = {"运行状态", "配置", "运行日志", "蓝牙电话"};
        for (int i = 0; i < menu.length; i++) {
            final int page = i;
            menu[i] = Ui.button(this, labels[i], view -> showPage(page));
            menu[i].setTextSize(15);
            LinearLayout.LayoutParams params = Ui.weight();
            params.height = Ui.dp(this, 44);
            navigation.addView(menu[i], params);
        }
        root.addView(navigation, Ui.matchWrap());

        FrameLayout content = new FrameLayout(this);
        statusPage = new StatusPage(this, api, worker);
        settingsPage = new SettingsPage(this, api, worker);
        logsPage = new LogsPage(this, api, worker);
        phonePage = new BluetoothPhonePage(this);
        pages[PAGE_STATUS] = statusPage.view();
        pages[PAGE_SETTINGS] = settingsPage.view();
        pages[PAGE_LOGS] = logsPage.view();
        pages[PAGE_PHONE] = phonePage.view();
        for (View page : pages) {
            page.setVisibility(View.GONE);
            content.addView(page, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        }
        root.addView(content, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        TextView footer = Ui.text(this,
                "Esc/返回键回到状态页。配置保存后服务自动重启；插拔 USB 时保持运行并自动重连。",
                11, Ui.MUTED);
        root.addView(footer, Ui.matchWrap());
        setContentView(root);
        showPage(PAGE_STATUS);
    }

    private void showPage(int page) {
        if (page < 0 || page >= pages.length) return;
        if (currentPage == PAGE_STATUS) statusPage.deactivate();
        if (currentPage == PAGE_SETTINGS) settingsPage.deactivate();
        if (currentPage == PAGE_LOGS) logsPage.deactivate();
        if (currentPage == PAGE_PHONE) phonePage.deactivate();
        currentPage = page;
        for (int i = 0; i < pages.length; i++) {
            boolean selected = i == page;
            pages[i].setVisibility(selected ? View.VISIBLE : View.GONE);
            menu[i].setBackgroundTintList(ColorStateList.valueOf(
                    selected ? Ui.ACCENT : Ui.PANEL));
            menu[i].setTextColor(selected ? Ui.BACKGROUND : Ui.TEXT);
        }
        if (page == PAGE_STATUS) statusPage.activate();
        else if (page == PAGE_SETTINGS) settingsPage.activate();
        else if (page == PAGE_LOGS) logsPage.activate();
        else phonePage.activate();
        menu[page].requestFocus();
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_F1) { showPage(PAGE_STATUS); return true; }
        if (keyCode == KeyEvent.KEYCODE_F2) { showPage(PAGE_SETTINGS); return true; }
        if (keyCode == KeyEvent.KEYCODE_F3) { showPage(PAGE_LOGS); return true; }
        if (keyCode == KeyEvent.KEYCODE_F4) { showPage(PAGE_PHONE); return true; }
        if (keyCode == KeyEvent.KEYCODE_ESCAPE) {
            if (currentPage != PAGE_STATUS) showPage(PAGE_STATUS);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override public void onBackPressed() {
        if (currentPage != PAGE_STATUS) showPage(PAGE_STATUS);
        else super.onBackPressed();
    }

    @Override protected void onDestroy() {
        if (statusPage != null) statusPage.deactivate();
        if (logsPage != null) logsPage.deactivate();
        if (phonePage != null) phonePage.deactivate();
        if (worker != null) worker.shutdownNow();
        super.onDestroy();
    }
}
