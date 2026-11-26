package com.carplaybox.audio;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.os.SystemClock;
import android.os.Handler;
import android.util.Log;

/** System PBAP Client imports contacts; the app never writes the phone's address book. */
final class BluetoothContacts implements AutoCloseable {
    private static volatile BluetoothContacts instance;
    private final Context context;
    private final BluetoothAdapter adapter=BluetoothAdapter.getDefaultAdapter();
    private BluetoothProfile profile;
    private boolean running,requested,profileReady,pendingManual;
    private final PhoneContacts.AutoSync automatic=new PhoneContacts.AutoSync();
    private String pending="",target="",status="手机连接后自动同步通讯录";
    private String lastAutoLog="";
    private long pendingAt,attemptAt=-1;
    private boolean wasConnected;
    private final Handler handler=new Handler();
    private String refreshing="";
    BluetoothContacts(Context c) { context=c.getApplicationContext(); }
    static BluetoothContacts current() { return instance; }
    void start() { running=true;instance=this; }
    private final BluetoothProfile.ServiceListener listener=new BluetoothProfile.ServiceListener() {
        public void onServiceConnected(int type,BluetoothProfile p) {
            if(!running) { adapter.closeProfileProxy(17,p);return; }
            profile=p;profileReady=true;requested=false;
            String d=pending;boolean manual=pendingManual;pending="";
            if(!d.isEmpty() && SystemClock.elapsedRealtime()-pendingAt<30000)request(d,manual);
        }
        public void onServiceDisconnected(int type) { profileReady=false;status="通讯录蓝牙服务已断开，等待恢复"; }
    };
    /** Called by the boot-started service, even when no phone Activity is open. */
    void update(PhoneState phone) {
        if(!running)return;
        String selected=phone==null?"":phone.selected;
        long now=SystemClock.elapsedRealtime();
        boolean connected=phone!=null && PhoneContacts.autoTarget(selected,phone.selected,phone.connected,SystemClock.uptimeMillis()-phone.sampled);
        if(!connected) {
            automatic.due(selected,false,0,now);
            if(!pendingManual)pending="";
            return;
        }
        if(!target.equals(selected)) {
            target=selected;attemptAt=-1;wasConnected=false;
            refreshing="";handler.removeCallbacks(refresh);
        }
        if(requested || !refreshing.isEmpty() || (profile!=null && !profileReady))return;
        try {
            int state=profile==null?BluetoothProfile.STATE_DISCONNECTED:profile.getConnectionState(adapter.getRemoteDevice(selected));
            if(automatic.due(selected,true,state,now)) {
                String outcome=request(selected,false);
                // Log transitions with monotonic time, excluding contacts, numbers and addresses.
                if(!outcome.equals(lastAutoLog)) { Log.i("CarPlayContacts","auto_sync elapsed_ms="+now+" "+outcome);lastAutoLog=outcome; }
            }
        }catch(RuntimeException e) { status="自动同步等待蓝牙服务恢复"; }
    }
    String sync(String device) {
        automatic.manual(device,SystemClock.elapsedRealtime());
        return request(device,true);
    }
    private String request(String device,boolean manual) {
        PhoneState phone=BluetoothCallBridge.current()==null?null:BluetoothCallBridge.current().state();
        if(!running || adapter==null || !adapter.isEnabled())return status="请先开启蓝牙";
        if(phone==null || device.isEmpty() || !phone.selected.equals(device))return status="请先在拨号页面选择手机";
        if(!manual && !PhoneContacts.autoTarget(device,phone.selected,phone.connected,SystemClock.uptimeMillis()-phone.sampled))
            return status="等待手机连接后自动同步通讯录";
        try {
            BluetoothDevice d=adapter.getRemoteDevice(device);
            if(d.getBondState()!=BluetoothDevice.BOND_BONDED)return status="请先配对手机";
            if(profile==null) {
                pending=device;pendingManual=manual;pendingAt=SystemClock.elapsedRealtime();
                if(!requested)requested=adapter.getProfileProxy(context,listener,17);
                return status=requested?"正在准备通讯录同步":"通讯录服务未启用，请检查盒子系统补丁";
            }
            if(!profileReady)return status="通讯录蓝牙服务正在恢复";
            for(BluetoothDevice other:profile.getConnectedDevices())if(!other.equals(d))
                return status="另一台手机正在同步，请先在其蓝牙设置断开盒子";
            if(!target.equals(device)) { attemptAt=-1;wasConnected=false; }
            target=device;
            int state=profile.getConnectionState(d);
            if(!refreshing.isEmpty())return status="正在重新同步通讯录";
            if(state==BluetoothProfile.STATE_CONNECTED) {
                if(!manual) { wasConnected=true;return status="手机通讯录通道已连接"; }
                boolean queued=(Boolean)profile.getClass().getMethod("disconnect",BluetoothDevice.class).invoke(profile,d);
                if(!queued)return status="系统未接受重新同步，请稍后再试";
                refreshing=device;pendingAt=SystemClock.elapsedRealtime();handler.postDelayed(refresh,500);
                return status="正在重新同步通讯录";
            }
            if(state==BluetoothProfile.STATE_CONNECTING)return status="正在同步，请在手机允许共享联系人";
            if(state==BluetoothProfile.STATE_DISCONNECTING)return status="通讯录通道正在断开，等待下次同步";
            // Existing bonds predate enabling PBAP: their priority is UNDEFINED (-1).
            // Enable PBAP for the user's selected bonded phone; phone-side consent is still required.
            boolean priority=(Boolean)profile.getClass().getMethod("setPriority",BluetoothDevice.class,int.class).invoke(profile,d,100);
            if(!priority)return status="系统未允许此手机的通讯录同步";
            boolean ok=(Boolean)profile.getClass().getMethod("connect",BluetoothDevice.class).invoke(profile,d);
            attemptAt=ok?SystemClock.elapsedRealtime():-1;wasConnected=false;
            return status=ok?"已请求同步，请在手机允许共享联系人":"系统未接受同步，请检查蓝牙连接";
        }catch(Exception e) { return status="通讯录同步不可用："+e.getClass().getSimpleName(); }
    }
    private final Runnable refresh=new Runnable() { public void run() {
        if(!running || refreshing.isEmpty())return;
        String device=refreshing;
        if(SystemClock.elapsedRealtime()-pendingAt>15000 || profile==null) {
            refreshing="";status="重新同步等待超时，请稍后再试";return;
        }
        try {
            if(profile.getConnectionState(adapter.getRemoteDevice(device))==BluetoothProfile.STATE_DISCONNECTED) {
                refreshing="";sync(device);return;
            }
            handler.postDelayed(this,500);
        }catch(RuntimeException e) { refreshing="";status="通讯录连接已变化，请重试"; }
    } };
    String status() {
        if(!refreshing.isEmpty())return "正在重新同步通讯录";
        if(profile==null && requested && SystemClock.elapsedRealtime()-pendingAt>15000)return "通讯录服务未就绪，请检查盒子系统补丁";
        if(profile!=null && profileReady && !target.isEmpty())try {
            int state=profile.getConnectionState(adapter.getRemoteDevice(target));
            if(state==BluetoothProfile.STATE_CONNECTED)wasConnected=true;
            if(attemptAt>=0 || wasConnected || state!=BluetoothProfile.STATE_DISCONNECTED) {
                String live=PhoneContacts.syncStatus(state,attemptAt<0?0:SystemClock.elapsedRealtime()-attemptAt,wasConnected);
                if(live!=null)return live;
            }
        }catch(RuntimeException ignored) {}
        return status;
    }
    @Override public void close() {
        running=false;pending="";refreshing="";handler.removeCallbacksAndMessages(null);if(instance==this)instance=null;
        if(adapter!=null && profile!=null)adapter.closeProfileProxy(17,profile);profile=null;
    }
}
