package com.carplaybox.audio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Handler;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.List;

/** Private bridge to actual Android app state, independent of USB/audio capture. */
public final class AppIntegrationService extends Service {
    private static final String TAG="CarPlayApps", SOCKET="carplay.app.integration.v1";
    private static volatile String status="媒体与语音服务未启动";
    private static volatile String voiceStatus="语音按钮等待车机请求";
    private static volatile String communicationsStatus="通信状态等待系统数据";
    private static volatile AppIntegrationService current;
    private volatile AppSnapshot latest;
    static AppIntegrationService current(){return current;}
    MediaQueue queue(){return mediaQueue;}
    AppSnapshot media(){return latest;}
    private final Handler handler=new Handler();
    private volatile boolean running;
    private volatile byte[] snapshot;
    private volatile byte[] communications;
    private volatile byte[] calls;
    private volatile MediaController controller;
    private volatile LocalServerSocket server;
    private volatile LocalSocket activeSocket;
    private Thread worker;
    private BluetoothCallBridge bluetoothCalls;
    private BluetoothNetworkBridge bluetoothNetwork;
    private PhoneNotifications phoneNotifications;
    private BluetoothContacts bluetoothContacts;
    private RouteProvider routeProvider;
    private final MediaExtras mediaExtras=new MediaExtras();
    private final MediaArtwork mediaArtwork=new MediaArtwork();
    private final MediaQueue mediaQueue=new MediaQueue();
    private boolean voiceDown;
    private long voiceDownTime;
    public static String currentStatus() { return status+"\n  "+voiceStatus+"\n  "+communicationsStatus; }
    private static void reportVoice(String next) {
        if(!voiceStatus.equals(next)) { voiceStatus=next;Log.i(TAG,next); }
    }
    private static void report(String next) {
        if(!status.equals(next)) { status=next;Log.i(TAG,next); }
    }
    private final Runnable poll=new Runnable() {
        @Override public void run() {
            if(!running)return;
            sample();handler.postDelayed(this,1000);
        }
    };
    @Override public void onCreate() {
        super.onCreate();
        current=this;
        bluetoothCalls=new BluetoothCallBridge(this);bluetoothCalls.start();
        phoneNotifications=new PhoneNotifications(this);
        bluetoothContacts=new BluetoothContacts(this);bluetoothContacts.start();
        routeProvider=new RouteProvider(this);
        NotificationManager manager=getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("carplay_apps","CarPlay 媒体与语音",NotificationManager.IMPORTANCE_LOW));
        startForeground(44,new Notification.Builder(this,"carplay_apps")
                .setSmallIcon(android.R.drawable.ic_media_play).setContentTitle("CarPlay 媒体与语音")
                .setContentText("同步 Android 播放状态").setOngoing(true).build());
        bluetoothNetwork=new BluetoothNetworkBridge(this);bluetoothNetwork.start();
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId) {
        if(!running) { running=true;handler.post(poll);worker=new Thread(this::serve,"carplay-app-bridge");worker.start(); }
        return START_STICKY;
    }
    private static int rank(MediaController candidate) {
        PlaybackState p=candidate.getPlaybackState();
        if(p==null)return candidate.getMetadata()!=null?1:0;
        switch(p.getState()) {
            case PlaybackState.STATE_PLAYING:case PlaybackState.STATE_FAST_FORWARDING:
            case PlaybackState.STATE_REWINDING:return 100;
            case PlaybackState.STATE_BUFFERING:case PlaybackState.STATE_CONNECTING:return 80;
            case PlaybackState.STATE_PAUSED:return 50;
            default:return candidate.getMetadata()!=null?10:0;
        }
    }
    private void sample() {
        PhoneState phone=bluetoothCalls.state();
        bluetoothContacts.update(phone);
        phoneNotifications.update(phone);
        try { calls=phone.connected && phone.knownCalls?BluetoothCalls.encode(phone.calls,phone.sampled):null; }
        catch(java.io.IOException invalid) { calls=null; }
        sampleCommunications();
        try {
            MediaSessionManager sessions=getSystemService(MediaSessionManager.class);
            List<MediaController> active=sessions.getActiveSessions(null);
            MediaController chosen=null;int score=0;
            for(MediaController candidate:active) {
                int value=rank(candidate);
                if(value>score || (value>0 && value==score && controller!=null &&
                    candidate.getSessionToken().equals(controller.getSessionToken()))) { chosen=candidate;score=value; }
            }
            controller=chosen;
            AppSnapshot s=new AppSnapshot();s.sampled=SystemClock.uptimeMillis();
            java.util.ArrayList<MediaQueue.Entry> queueEntries=new java.util.ArrayList<>();
            boolean canSelectQueue=false;
            String track="";MediaMetadata artworkMetadata=null;
            if(chosen!=null) {
                s.status=255; // Metadata can exist without a PlaybackState.
                s.packageName=chosen.getPackageName();
                try { s.appName=getPackageManager().getApplicationLabel(getPackageManager().getApplicationInfo(s.packageName,0)).toString(); }
                catch(Exception missing) { s.appName=s.packageName; }
                MediaMetadata m=chosen.getMetadata();
                artworkMetadata=m;
                if(m!=null) {
                    s.title=m.getString(MediaMetadata.METADATA_KEY_TITLE);
                    if(s.title==null)s.title=m.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE);
                    s.album=m.getString(MediaMetadata.METADATA_KEY_ALBUM);s.artist=m.getString(MediaMetadata.METADATA_KEY_ARTIST);
                    s.composer=m.getString(MediaMetadata.METADATA_KEY_COMPOSER);s.duration=m.getLong(MediaMetadata.METADATA_KEY_DURATION);
                }
                PlaybackState p=chosen.getPlaybackState();
                if(p!=null) {
                    int state=p.getState();
                    s.status=state==PlaybackState.STATE_PLAYING?1:state==PlaybackState.STATE_PAUSED?2:
                        state==PlaybackState.STATE_FAST_FORWARDING?3:state==PlaybackState.STATE_REWINDING?4:
                        state==PlaybackState.STATE_STOPPED || state==PlaybackState.STATE_NONE || state==PlaybackState.STATE_ERROR?0:255;
                    s.position=Math.max(0,p.getPosition());
                    float speed=p.getPlaybackSpeed();
                    s.speed=Float.isNaN(speed)||Float.isInfinite(speed)?0:Math.max(0,Math.min(65535,Math.round(speed*100)));
                    if((s.status==1 || s.status==3 || s.status==4) && p.getLastPositionUpdateTime()>0 && !Float.isNaN(speed) && !Float.isInfinite(speed))
                        s.position=Math.max(0,s.position+(long)(Math.max(0,SystemClock.elapsedRealtime()-p.getLastPositionUpdateTime())*speed));
                    if(s.duration>0)s.position=Math.min(s.duration,s.position);
                    s.canSeek=(p.getActions()&PlaybackState.ACTION_SEEK_TO)!=0;
                    List<android.media.session.MediaSession.QueueItem> queue=chosen.getQueue();
                    canSelectQueue=(p.getActions()&PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM)!=0;
                    if(queue!=null && !queue.isEmpty() && queue.size()<=10000){
                        s.queueCount=queue.size();long currentId=p.getActiveQueueItemId();
                        for(android.media.session.MediaSession.QueueItem item:queue){
                            if(item==null){queueEntries.add(null);continue;}
                            android.media.MediaDescription d=item.getDescription();
                            queueEntries.add(new MediaQueue.Entry(item.getQueueId(),text(d.getTitle()),text(d.getDescription()),text(d.getSubtitle())));
                        }
                        if(currentId!=android.media.session.MediaSession.QueueItem.UNKNOWN_ID)
                            for(int i=0;i<queue.size();i++)if(queue.get(i)!=null && queue.get(i).getQueueId()==currentId){s.queueIndex=i;break;}
                    }
                }
                track=chosen.getSessionToken().toString()+"|"+s.packageName+"|"+
                    (m==null?"":m.getString(MediaMetadata.METADATA_KEY_MEDIA_ID))+"|"+s.title+"|"+s.artist+"|"+s.album;
            }
            mediaQueue.publish(chosen==null?null:chosen.getSessionToken(),queueEntries,canSelectQueue,s.sampled);
            s.queueVersion=mediaQueue.version();s.queueAvailable=mediaQueue.count()>0;
            mediaExtras.publish(track,mediaArtwork.read(track,artworkMetadata));
            s.mediaGeneration=mediaExtras.generation();s.artSize=mediaExtras.size();
            latest=s;
            snapshot=s.encode();report(chosen==null?"媒体同步就绪：没有活动播放器":"正在同步播放器："+s.packageName);
        } catch(Exception failure) {
            mediaQueue.publish(null,null,false,SystemClock.uptimeMillis());
            controller=null;snapshot=null;report("媒体状态不可用："+failure.getClass().getSimpleName());
            latest=null;
        }
    }
    private static String text(CharSequence value){return value==null?"":value.toString();}
    private byte[] queuePage(int start)throws IOException{
        synchronized(mediaQueue){
            long version=mediaQueue.version();
            List<MediaQueue.Entry> entries=mediaQueue.page(version,start,2,SystemClock.uptimeMillis());
            java.io.ByteArrayOutputStream bytes=new java.io.ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
            out.writeInt(0x43514c31);out.writeLong(mediaQueue.sampled());out.writeLong(version);
            out.writeInt(mediaQueue.count());out.writeInt(start);out.writeShort(entries.size());
            for(MediaQueue.Entry e:entries){out.writeLong(e.id);for(String t:new String[]{e.title,e.album,e.artist}){
                byte[] b=t.getBytes(java.nio.charset.StandardCharsets.UTF_8);out.writeShort(b.length);out.write(b);
            }}out.flush();return bytes.toByteArray();
        }
    }
    boolean queueSelect(long version,int index){
        final MediaController target=controller;final long received=SystemClock.uptimeMillis();
        if(target==null || mediaQueue.select(version,index,received)==-1)return false;
        return handler.post(()->{
            if(!running || SystemClock.uptimeMillis()-received>1500)return;
            // Resample on the owning thread just before dispatch. Never translate an old index in a new session.
            sample();long id=mediaQueue.select(version,index,SystemClock.uptimeMillis());
            if(id==-1 || controller==null || !target.getSessionToken().equals(controller.getSessionToken())){
                Log.w(TAG,"Discarded stale queue selection index="+index);return;
            }
            try{PlaybackState p=target.getPlaybackState();
                if(p==null || (p.getActions()&PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM)==0)return;
                target.getTransportControls().skipToQueueItem(id);
                if((p.getActions()&PlaybackState.ACTION_PLAY)!=0)target.getTransportControls().play();
                Log.i(TAG,"Queue selection index="+index+" id="+id);
            }catch(RuntimeException failure){Log.w(TAG,"Queue selection failed",failure);}
        });
    }
    private void sampleCommunications() {
        try {
            boolean cellular=getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_TELEPHONY);
            android.telecom.TelecomManager telecom=getSystemService(android.telecom.TelecomManager.class);
            boolean enabled=telecom!=null && !telecom.getCallCapablePhoneAccounts().isEmpty();
            boolean canCall=enabled && telecom.getDefaultDialerPackage()!=null;
            boolean airplane=android.provider.Settings.Global.getInt(getContentResolver(),android.provider.Settings.Global.AIRPLANE_MODE_ON,0)!=0;
            boolean muted=getSystemService(android.media.AudioManager.class).isMicrophoneMute();
            java.io.ByteArrayOutputStream bytes=new java.io.ByteArrayOutputStream();
            DataOutputStream out=new DataOutputStream(bytes);out.writeInt(0x43504331);out.writeLong(SystemClock.uptimeMillis());
            for(boolean value:new boolean[]{airplane,cellular,enabled,canCall,muted})out.writeByte(value?1:0);
            out.flush();communications=bytes.toByteArray();
            communicationsStatus="蜂窝能力："+(cellular?"有":"无")+"；电话账户："+(enabled?"可用":"无")+"；可发起通话："+(canCall?"是":"否");
        } catch(Exception unavailable) {
            communications=null;communicationsStatus="通信状态不可用："+unavailable.getClass().getSimpleName();
        }
    }
    private Intent voiceIntent() {
        for(String action:new String[]{Intent.ACTION_VOICE_COMMAND,Intent.ACTION_ASSIST}) {
            Intent intent=new Intent(action).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ResolveInfo resolved=getPackageManager().resolveActivity(intent,0);
            if(resolved!=null && resolved.activityInfo!=null &&
                !resolved.activityInfo.name.endsWith("ResolverActivity")) {
                intent.setClassName(resolved.activityInfo.packageName,resolved.activityInfo.name);return intent;
            }
        }
        return null;
    }
    private void voice(int action) {
        long now=SystemClock.uptimeMillis();
        if(action==1) { if(voiceIntent()==null)reportVoice("语音不可用：未安装或未设置语音助手");return; }
        if(action==3) { voiceDown=false;return; }
        if(voiceDown && now-voiceDownTime<10000)return;
        voiceDown=true;voiceDownTime=now;
        Intent intent=voiceIntent();
        if(intent==null) { reportVoice("语音不可用：未安装或未设置语音助手");return; }
        try {
            String targetPackage=intent.getComponent().getPackageName();
            startActivity(intent);
            reportVoice("已请求语音助手："+targetPackage);
        }
        catch(RuntimeException failure) { reportVoice("语音助手启动失败："+failure.getClass().getSimpleName()); }
    }
    boolean command(int op,long arg) {
        final MediaController target=controller;
        if(op==6 && (arg==1 || arg==2)) {
            final PhoneState received=bluetoothCalls.state();
            if(received==null || !PhoneVehicleKeys.fresh(received.sampled,SystemClock.uptimeMillis()))return false;
            final String action=PhoneVehicleKeys.action((int)arg,received.connected,received.knownCalls,received.calls);
            if(action==null)return false;
            return handler.post(()-> {
                if(!PhoneVehicleKeys.fresh(received.sampled,SystemClock.uptimeMillis()))return;
                bluetoothCalls.vehicleKey(action,received);
            });
        }
        if(op==2 && arg>=1 && arg<=3)return handler.post(()->voice((int)arg));
        if(target==null)return false;
        if(op==1 && AppSnapshot.mediaKey((int)arg)) {
            return handler.post(()-> {
                try {
                    long now=SystemClock.uptimeMillis();
                    boolean accepted=target.dispatchMediaButtonEvent(new KeyEvent(now,now,KeyEvent.ACTION_DOWN,(int)arg,0));
                    target.dispatchMediaButtonEvent(new KeyEvent(now,now,KeyEvent.ACTION_UP,(int)arg,0));
                    if(!accepted)Log.w(TAG,"Player rejected media key "+arg);
                } catch(RuntimeException failure) { Log.w(TAG,"Media command failed",failure); }
                sample();
            });
        }
        if(op==3) {
            PlaybackState p=target.getPlaybackState();
            if(p==null || (p.getActions()&PlaybackState.ACTION_SEEK_TO)==0)return false;
            return handler.post(()-> {
                try { target.getTransportControls().seekTo(arg); }
                catch(RuntimeException failure) { Log.w(TAG,"Seek failed",failure); }
                sample();
            });
        }
        return false;
    }
    private void serve() {
        while(running) {
            try(LocalServerSocket listener=new LocalServerSocket(SOCKET)) {
                server=listener;
                while(running) {
                    try(LocalSocket client=listener.accept()) {
                        activeSocket=client;
                        if(client.getPeerCredentials().getUid()!=0) { Log.w(TAG,"Rejected non-root app bridge client");continue; }
                        client.setSoTimeout(100);
                        DataInputStream input=new DataInputStream(client.getInputStream());
                        if(input.readInt()!=0x43505131)continue;
                        int op=input.readInt();long arg=((long)input.readInt())&0xffffffffL;
                        byte[] response;
                        if(op==10)response=new byte[]{(byte)(queueSelect(input.readLong(),(int)arg)?0:1)};
                        else if(op==9)response=queuePage((int)arg);
                        else if(op==8)response=mediaExtras.chunk((int)arg);
                        else if(op==7)response=routeProvider.read();
                        else if(op==0 || op==4 || op==5) { response=op==0?snapshot:op==4?communications:calls;if(response==null)response=new byte[]{1}; }
                        else response=new byte[]{(byte)(command(op,arg)?0:1)};
                        DataOutputStream output=new DataOutputStream(client.getOutputStream());
                        output.writeInt(response.length);output.write(response);output.flush();
                    } catch(IOException | RuntimeException clientFailure) {
                        if(running)Log.w(TAG,"App bridge request failed: "+clientFailure.getClass().getSimpleName());
                    } finally { activeSocket=null; }
                }
            } catch(IOException failure) {
                if(running) { report("媒体桥接启动失败："+failure.getMessage());try { Thread.sleep(2000); } catch(InterruptedException stop) { break; } }
            } finally { server=null; }
        }
    }
    @Override public void onDestroy() {
        if(current==this)current=null;latest=null;
        if(routeProvider!=null)routeProvider.close();
        if(phoneNotifications!=null)phoneNotifications.close();
        if(bluetoothContacts!=null)bluetoothContacts.close();
        if(bluetoothCalls!=null)bluetoothCalls.close();
        if(bluetoothNetwork!=null)bluetoothNetwork.close();
        running=false;snapshot=null;communications=null;calls=null;controller=null;handler.removeCallbacksAndMessages(null);
        try { if(server!=null)server.close(); } catch(IOException ignored) {}
        try { if(activeSocket!=null)activeSocket.close(); } catch(IOException ignored) {}
        if(worker!=null)worker.interrupt();report("媒体与语音服务已停止");super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
