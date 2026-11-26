package com.carplaybox.audio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import org.json.JSONObject;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Vehicle GPS has its own lifetime; it never depends on audio capture. */
public final class VehicleLocationService extends Service {
    private static volatile String status = "车机定位服务未启动";
    private volatile boolean running;
    private Thread worker;
    private VehicleLocationBridge bridge;
    private ScheduledExecutorService expiry;
    private long diagnosticRmcSequence = -1, diagnosticGgaSequence = -1;
    public static String currentStatus() { return status; }
    private static void publishStatus(String next) {
        if (!next.equals(status)) {
            status = next;
            if (next.contains("失败") || next.contains("未应用") || next.contains("不可用"))
                Log.w("CarPlayVehicleLocation", next);
            else Log.i("CarPlayVehicleLocation", next);
        }
    }
    @Override public void onCreate() {
        super.onCreate();
        bridge = new VehicleLocationBridge(new VehicleAndroidProvider(this));
        expiry = Executors.newSingleThreadScheduledExecutor();
        expiry.scheduleWithFixedDelay(() -> {
            synchronized (this) {
                bridge.expire(SystemClock.elapsedRealtime());
                publishStatus(bridge.status);
            }
        }, 250, 250, TimeUnit.MILLISECONDS);
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager manager = getSystemService(NotificationManager.class);
            manager.createNotificationChannel(new NotificationChannel("vehicle_location", "CarPlay 车机定位", NotificationManager.IMPORTANCE_LOW));
            startForeground(43, new Notification.Builder(this, "vehicle_location")
                    .setSmallIcon(android.R.drawable.ic_menu_mylocation)
                    .setContentTitle("CarPlay 车机定位")
                    .setContentText("等待车机提供定位数据").setOngoing(true).build());
        }
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (!running) {
            running = true;
            worker = new Thread(this::poll, "carplay-vehicle-location"); worker.start();
        }
        return START_STICKY;
    }
    private void poll() {
        while (running) {
            long started = SystemClock.elapsedRealtime();
            try {
                int port = getSharedPreferences("carplay_console", MODE_PRIVATE).getInt("api_port", CarPlayApi.DEFAULT_PORT);
                CarPlayApi.Result response = new CarPlayApi(port).get("/api/status");
                if (response.code != 200) throw new IllegalStateException("HTTP " + response.code);
                JSONObject vehicle = new JSONObject(response.body).getJSONObject("vehicle");
                JSONObject location = vehicle.getJSONObject("location");
                VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
                input.enabled = vehicle.getBoolean("gps"); input.active = location.getBoolean("active");
                input.instance = location.getLong("instance"); input.generation = location.getLong("generation");
                input.rmc = location.optString("rmc", ""); input.gga = location.optString("gga", "");
                input.rmcSequence = location.optLong("rmcSequence"); input.ggaSequence = location.optLong("ggaSequence");
                long now = SystemClock.elapsedRealtime(), delay = Math.max(0, now - started);
                input.rmcAgeMs = location.optLong("rmcAgeMs", 5000) + delay;
                input.ggaAgeMs = location.optLong("ggaAgeMs", 5000) + delay;
                synchronized (this) {
                    if (running) bridge.update(input, now, System.currentTimeMillis());
                    publishStatus(bridge.status);
                    if (input.rmcSequence != diagnosticRmcSequence || input.ggaSequence != diagnosticGgaSequence) {
                        diagnosticRmcSequence = input.rmcSequence;
                        diagnosticGgaSequence = input.ggaSequence;
                        Log.i("CarPlayVehicleLocation", "sample rmcSeq=" + input.rmcSequence +
                                " ggaSeq=" + input.ggaSequence +
                                " rmcArrivalMs=" + input.rmcAgeMs +
                                " ggaArrivalMs=" + input.ggaAgeMs +
                                " contentAgeMs=" + bridge.lastContentAgeMs +
                                " httpMs=" + delay +
                                " applyMs=" + bridge.lastApplyDurationMs +
                                " status=" + bridge.status);
                    }
                }
            } catch (Exception failure) {
                synchronized (this) { bridge.pollFailed(failure.getMessage()); publishStatus(bridge.status); }
            }
            if (!running) break;
            long delay = VehicleLocationPollSchedule.delayAfter(started, SystemClock.elapsedRealtime());
            if (delay > 0) {
                try { Thread.sleep(delay); } catch (InterruptedException interrupted) { break; }
            }
        }
        synchronized (this) { bridge.stop("车机定位已停止"); publishStatus(bridge.status); }
    }
    @Override public void onDestroy() {
        running = false;
        if (expiry != null) expiry.shutdownNow();
        if (worker != null) worker.interrupt();
        synchronized (this) { bridge.stop("车机定位已停止"); publishStatus(bridge.status); }
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
