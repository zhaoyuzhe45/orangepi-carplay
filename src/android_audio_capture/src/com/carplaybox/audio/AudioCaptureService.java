package com.carplaybox.audio;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import java.io.IOException;
import java.io.OutputStream;
public final class AudioCaptureService extends Service {
    public static final String ACTION_STATUS = "com.carplaybox.audio.STATUS";
    public static final String EXTRA_STATUS = "status";
    private static final String TAG = "CarPlayAudioCapture";
    private static final String SOCKET = "carplay_audio_capture_v1";
    private static final String SPLIT_SOCKET = "carplay_audio_capture_v2";
    private static final String PRIORITY_PROPERTY = "debug.carplay.audio_priority";
    private static final String BUFFER_PROPERTY = "debug.carplay.audio_buffer_frames";
    private static final int FRAME_BYTES = 3840;
    private static volatile String status = "停止";
    private volatile boolean running;
    private Thread worker;
    private volatile LocalSocket socket;
    private volatile boolean splitTransport;
    private volatile SplitAudioCapture split;
    private volatile MediaCaptureStats mediaStats;
    private SharedBoardMicrophone microphoneBridge;
    public static String currentStatus() { return status; }
    public static void publishStatus(Context context, String value) {
        if(value.equals(status))return;
        status = value;
        Log.i(TAG, value);
        Intent update = new Intent(ACTION_STATUS);
        update.setPackage(context.getPackageName());
        update.putExtra(EXTRA_STATUS, value);
        context.sendBroadcast(update);
    }
    @Override public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel("audio_capture",
                    "CarPlay 音频采集", NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
            startForeground(41, new Notification.Builder(this, "audio_capture")
                    .setSmallIcon(android.R.drawable.ic_media_play)
                    .setContentTitle("CarPlay 音频桥")
                    .setContentText("正在为车机媒体通道采集声音")
                    .setOngoing(true).build());
        }
        // Board capture is independent of the vehicle connection and media focus.
        // The broker checks the installed HAL before registering any recorder route.
        microphoneBridge = new SharedBoardMicrophone(this);
        microphoneBridge.start();
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (!running) {
            running = true;
            worker = new Thread(this::captureLoop, "carplay-audio-capture");
            worker.start();
        }
        return START_STICKY;
    }
    private void captureLoop() {
        String lastFailure="";long lastFailureAt=0;
        while (running) {
            boolean connected=false;
            try {
                connect();
                connected=true;lastFailure="";
                publishStatus(this, "已连接，采集 48 kHz 立体声并等待车机音频流");
                readLoop();
            } catch (Throwable error) {
                if(running){
                    boolean waiting=!connected && error instanceof IOException &&
                        "Connection refused".equals(error.getMessage());
                    String detail=error.getClass().getSimpleName()+":"+error.getMessage();
                    publishStatus(this,waiting?"等待 CarPlay 音频通道":"等待 CarPlay："+error.getMessage());
                    long now=android.os.SystemClock.uptimeMillis();
                    if(!waiting && (!detail.equals(lastFailure) || now-lastFailureAt>=30000)){
                        Log.w(TAG,"capture session failed",error);lastFailureAt=now;
                    }
                    lastFailure=detail;
                }
            } finally {
                closeSocket();
            }
            if (!running) break;
            try { Thread.sleep(1000); } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt(); break;
            }
        }
        publishStatus(this, "停止");
        stopSelf();
    }
    private void connect() throws IOException {
        LocalSocket next = null;
        if (NavigationCaptureUids.useAltTransport(AudioCaptureService::audioProperty)) {
            try { next = connectSocket(SPLIT_SOCKET); }
            catch (IOException unavailable) { /* Old daemon or no compatible 101; use v1. */ }
        }
        splitTransport = next != null;
        socket = splitTransport ? next : connectSocket(SOCKET);
        CarPlayUiShortcut.requestWhenReady(this);
    }
    private static LocalSocket connectSocket(String name) throws IOException {
        LocalSocket next = new LocalSocket();
        try {
            next.connect(new LocalSocketAddress(name,LocalSocketAddress.Namespace.ABSTRACT));
        } catch(IOException | RuntimeException failure) {
            try { next.close(); } catch(IOException ignored) {}
            throw failure;
        }
        return next;
    }
    private SplitAudioCapture openCapture() throws IOException {
        PackageManager packages=getPackageManager();
        int[] uids=NavigationCaptureUids.resolve(audioProperty(NavigationCaptureUids.SPLIT_PROPERTY), name -> {
            try{return packages.getApplicationInfo(name,0).uid;}
            catch(PackageManager.NameNotFoundException absent){return -1;}
        });
        try {
            return new SplitAudioCapture(this,uids);
        }catch(Exception unavailable){
            if(uids.length!=0){
                Log.w(TAG,"Navigation split unavailable; trying isolated media capture",unavailable);
                try{return new SplitAudioCapture(this,-1);}
                catch(Exception fallback){unavailable.addSuppressed(fallback);}
            }
            // Generic REMOTE_SUBMIX address 0 can be silent or include virtual
            // microphone injection. Reconnect with backoff instead.
            throw new IOException("Audio policy capture unavailable; retrying",unavailable);
        }
    }
    private void readLoop() throws IOException {
        try(SplitAudioCapture capture=openCapture()){
            split=capture;
            try{
                if(splitTransport && !capture.hasNavigation()){
                    closeSocket();
                    splitTransport=false;
                    if(!running) return;
                    socket=connectSocket(SOCKET);
                }
                publishStatus(this,capture.hasNavigation()?"已连接，导航独立混音和音乐自动压低已启用":"已连接，系统媒体采集已启用");
                readSplit(capture);
            }finally{split=null;}
        }
    }
    private void readSplit(SplitAudioCapture capture)throws IOException{
        NavigationMixer mixer=new NavigationMixer();
        short[] media=new short[SplitAudioCapture.SAMPLES],nav=new short[SplitAudioCapture.SAMPLES],mixed=new short[SplitAudioCapture.SAMPLES];
        boolean separate = splitTransport;
        byte[] frame=new byte[separate?3*FRAME_BYTES:FRAME_BYTES];OutputStream output=socket.getOutputStream();
        int reserveFrames=MediaContinuityBuffer.reserveFrames(audioProperty(BUFFER_PROPERTY));
        MediaContinuityBuffer buffer=reserveFrames==0 ? null
                : new MediaContinuityBuffer(SplitAudioCapture.SAMPLES,reserveFrames,240);
        short[] incoming=buffer==null ? null : new short[SplitAudioCapture.BURST_SAMPLES];
        String setting=audioProperty(PRIORITY_PROPERTY);
        int original=CaptureThreadPriority.begin(setting);
        MediaCaptureStats stats=new MediaCaptureStats(Process.myTid(),original,
                CaptureThreadPriority.current(),!"0".equals(setting),reserveFrames);
        mediaStats=stats;
        try {
            boolean active=false;
            while(running){
                int missing;
                if(buffer==null){
                    missing=capture.read(media,nav);
                }else{
                    int received=capture.readMediaBurst(incoming,nav);
                    buffer.append(incoming,received);
                    buffer.render(media);
                    missing=Math.max(0,SplitAudioCapture.SAMPLES-received);
                }
                long capturedAt=SystemClock.elapsedRealtimeNanos();
                stats.recordRead(capturedAt,missing,buffer);
                mixer.mix(media,nav,mixed,capturedAt/1_000_000L);
                if(active!=mixer.navigationActive()){
                    active=mixer.navigationActive();
                    Log.i(TAG,active?"Navigation speech active; ducking media":"Navigation speech ended; restoring media");
                }
                encodePcm(mixed,frame,0);
                if(separate){
                    encodePcm(media,frame,FRAME_BYTES);
                    encodePcm(nav,frame,2*FRAME_BYTES);
                }
                long writingAt=SystemClock.elapsedRealtimeNanos();
                output.write(frame);output.flush();
                stats.recordWrite(writingAt,SystemClock.elapsedRealtimeNanos());
            }
        } finally {
            CaptureThreadPriority.end(original);
            stats.finish();
        }
    }
    private static void encodePcm(short[] pcm,byte[] bytes,int offset){
        for(short sample:pcm){
            bytes[offset++]=(byte)sample;
            bytes[offset++]=(byte)(sample>>8);
        }
    }
    private static String audioProperty(String key) {
        try {
            return (String) Class.forName("android.os.SystemProperties").getMethod("get",String.class)
                    .invoke(null,key);
        } catch (ReflectiveOperationException | SecurityException unavailable) {
            return "";
        }
    }
    private void closeSocket() {
        LocalSocket current = socket;
        socket = null;
        if (current != null) try { current.close(); } catch (IOException ignored) {}
    }
    @Override public void onDestroy() {
        running = false; closeSocket();
        SplitAudioCapture currentSplit=split;if(currentSplit!=null)currentSplit.stop();
        if (microphoneBridge != null) {
            microphoneBridge.close();
            microphoneBridge = null;
        }
        CarPlayUiShortcut.sessionEnded(this);
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override protected void dump(java.io.FileDescriptor fd, java.io.PrintWriter writer, String[] args) {
        MediaCaptureStats stats=mediaStats;
        SplitAudioCapture current=split;
        writer.println("audioRoute " + (current == null ? "inactive" : current.routeDescription()
                + " transport=" + (splitTransport?"v2":"v1")));
        writer.println("mediaCapture " + (stats == null ? "not started" : stats));
        writer.println("sharedMicrophone " + (microphoneBridge == null ? "stopped" : microphoneBridge.statistics()));
    }
}
