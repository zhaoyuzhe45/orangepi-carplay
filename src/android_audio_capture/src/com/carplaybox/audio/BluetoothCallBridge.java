package com.carplaybox.audio;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Android 8.1 HFP HF role. Control-only until the SCO-to-car path is verified. */
final class BluetoothCallBridge implements AutoCloseable {
    private static final String TAG="CarPlayBluetooth", PREFS="carplay_bluetooth";
    private static final int HEADSET_CLIENT=16;
    private static volatile BluetoothCallBridge instance;
    private final Context context;
    private final Handler handler=new Handler();
    private final BluetoothAdapter adapter=BluetoothAdapter.getDefaultAdapter();
    private BluetoothProfile profile;
    private boolean running,requested;
    private long requestedAt,lastCommand=-10000,dialPendingUntil,lastAudioRelease=-10000;
    private long lastTone=-10000;
    private String selected;
    private boolean autoConnect;
    private final BluetoothCalls.Reconnect reconnect=new BluetoothCalls.Reconnect();
    private volatile PhoneState state;
    private String lastLog="";
    static BluetoothCallBridge current() { return instance; }
    PhoneState state() { return state; }
    private final BluetoothProfile.ServiceListener listener=new BluetoothProfile.ServiceListener() {
        @Override public void onServiceConnected(int type,BluetoothProfile value) {
            if(!running) { adapter.closeProfileProxy(HEADSET_CLIENT,value);return; }
            profile=value;requested=false;sample();
        }
        @Override public void onServiceDisconnected(int type) {
            // The framework proxy rebinds after Bluetooth restarts.
            publish("蓝牙电话服务已断开",Collections.emptyList(),Collections.emptyList(),false,false,0);
        }
    };
    BluetoothCallBridge(Context context) {
        this.context=context.getApplicationContext();
        selected=context.getSharedPreferences(PREFS,0).getString("phone","");
        autoConnect=context.getSharedPreferences(PREFS,0).getBoolean("auto_connect",false);
        publish("蓝牙电话正在启动",Collections.emptyList(),Collections.emptyList(),false,false,0);
    }
    void start() { if(running)return;running=true;instance=this;handler.post(poll); }
    private final Runnable poll=new Runnable() {
        @Override public void run() { if(!running)return;sample();handler.postDelayed(this,1000); }
    };
    private Object invoke(String method,Class<?>[] types,Object... values) throws Exception {
        if(profile==null)throw new IllegalStateException("HFP client unavailable");
        Method found=profile.getClass().getMethod(method,types);
        return found.invoke(profile,values);
    }
    private Object onDevice(String method,BluetoothDevice device) throws Exception {
        return invoke(method,new Class<?>[]{BluetoothDevice.class},device);
    }
    private void publish(String message,List<PhoneState.Device> phones,List<BluetoothCalls.Call> calls,
                         boolean connected,boolean known,int audio) {
        state=new PhoneState(message,selected,phones,calls,connected,known,audio);
        // No phone numbers or call UUIDs in routine persistent logs.
        String diagnostic=message+"; calls="+calls.size()+"; SCO="+audio;
        if(!diagnostic.equals(lastLog)) { Log.i(TAG,diagnostic);lastLog=diagnostic; }
    }
    private List<?> rawCalls(BluetoothDevice device) throws Exception {
        Object result=onDevice("getCurrentCalls",device);
        if(!(result instanceof List))throw new IllegalStateException("call list unavailable");
        return (List<?>)result;
    }
    private BluetoothCalls.Call decode(Object value) throws Exception {
        Class<?> type=value.getClass();
        return new BluetoothCalls.Call(type.getMethod("getUUID").invoke(value).toString(),
            (Integer)type.getMethod("getState").invoke(value),
            (Boolean)type.getMethod("isOutgoing").invoke(value),
            (String)type.getMethod("getNumber").invoke(value));
    }
    void sample() {
        sample(true);
    }
    private void sample(boolean allowReconnect) {
        ArrayList<PhoneState.Device> phones=new ArrayList<>();ArrayList<BluetoothCalls.Call> calls=new ArrayList<>();
        if(adapter==null) { publish("设备没有蓝牙适配器",phones,calls,false,false,0);return; }
        try {
            if(!adapter.isEnabled()) { publish("蓝牙已关闭",phones,calls,false,false,0);return; }
            for(BluetoothDevice device:adapter.getBondedDevices())phones.add(new PhoneState.Device(device.getAddress(),device.getName()));
            Collections.sort(phones,(a,b)->a.address.compareTo(b.address));
            if(profile==null) {
                long now=SystemClock.elapsedRealtime();
                if(!requested && now-requestedAt>=5000) {
                    requestedAt=now;requested=adapter.getProfileProxy(context,listener,HEADSET_CLIENT);
                }
                publish(now-requestedAt>10000?"HFP 客户端未就绪：检查系统配置补丁":"正在等待 HFP 客户端",phones,calls,false,false,0);return;
            }
            if(selected.isEmpty()) { publish("请选择已配对手机",phones,calls,false,false,0);return; }
            BluetoothDevice device=adapter.getRemoteDevice(selected);
            int connection=profile.getConnectionState(device);
            if(allowReconnect && reconnect.due(autoConnect,connection==BluetoothProfile.STATE_CONNECTED,
                    connection==BluetoothProfile.STATE_CONNECTING || connection==BluetoothProfile.STATE_DISCONNECTING,
                    SystemClock.elapsedRealtime()) && device.getBondState()==BluetoothDevice.BOND_BONDED) {
                boolean queued=(Boolean)onDevice("connect",device);
                Log.i(TAG,"Selected phone reconnect queued="+queued);
            }
            if(connection!=BluetoothProfile.STATE_CONNECTED) {
                publish(connection==BluetoothProfile.STATE_CONNECTING?"正在连接手机电话服务":"手机电话服务未连接",phones,calls,false,false,0);return;
            }
            // Android 8.1 setAudioRouteAllowed is a no-op in HeadsetClientService.
            // Do not call it or assume the identically named resource is enforced.
            for(Object raw:rawCalls(device)) {
                BluetoothCalls.Call call=decode(raw);
                if(call.state!=7)calls.add(call);
            }
            int audio=(Integer)onDevice("getAudioState",device);
            if(audio!=0 && SystemClock.elapsedRealtime()-lastAudioRelease>=2000) {
                lastAudioRelease=SystemClock.elapsedRealtime();
                boolean queued=(Boolean)onDevice("disconnectAudio",device);
                Log.i(TAG,"Control-only HFP: release SCO queued="+queued);
            }
            publish(audio==0?"手机电话控制已连接；请在手机确认听筒或扬声器":
                "电话控制已连接；正在释放未验证的蓝牙语音链路",phones,calls,true,true,audio);
        } catch(Exception error) {
            publish("蓝牙电话读取失败："+error.getClass().getSimpleName(),phones,Collections.emptyList(),false,false,0);
        }
    }
    String select(String address) {
        sample(false);
        if(state.connected && !selected.equals(address))return "请先断开当前手机";
        for(PhoneState.Device phone:state.phones)if(phone.address.equals(address)) {
            if(!selected.equals(address))setAutoConnect(false);
            selected=address;context.getSharedPreferences(PREFS,0).edit().putString("phone",address).apply();
            sample(false);return "已选择手机";
        }
        return "手机未配对，请先打开蓝牙设置";
    }
    void vehicleKey(String action,PhoneState received) {
        if(received==null || !PhoneVehicleKeys.fresh(received.sampled,SystemClock.uptimeMillis()))return;
        sample(false);
        if(!state.connected || !state.knownCalls || !selected.equals(received.selected))return;
        if("open".equals(action)) {
            if(!received.calls.isEmpty() || !state.calls.isEmpty())return;
            android.content.Intent launch=context.getPackageManager().getLaunchIntentForPackage("com.carplaybox.phone");
            if(launch!=null)try { context.startActivity(launch.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)); }
            catch(RuntimeException failure) { Log.w(TAG,"Vehicle phone UI unavailable"); }
        } else if(received.calls.size()==1) {
            // command re-reads the actual call list and validates this UUID.
            command(action,received.selected,received.calls.get(0).uuid);
        }
    }
    private void setAutoConnect(boolean enabled) {
        autoConnect=enabled;
        context.getSharedPreferences(PREFS,0).edit().putBoolean("auto_connect",enabled).apply();
        if(!enabled)reconnect.due(false,false,false,SystemClock.elapsedRealtime());
    }
    String command(String action,String expectedDevice,String argument) {
        // Disable before sampling, so a manual disconnect cannot enqueue a reconnect.
        if("disconnect".equals(action) && selected.equals(expectedDevice))setAutoConnect(false);
        sample(false);long now=SystemClock.elapsedRealtime();
        if(!running || profile==null || selected.isEmpty() || !selected.equals(expectedDevice))return "手机已变更或服务未就绪，请刷新";
        if(now-lastCommand<1500)return "请求处理中，请等待状态更新";
        try {
            BluetoothDevice device=adapter.getRemoteDevice(selected);
            if(device.getBondState()!=BluetoothDevice.BOND_BONDED)return "手机尚未配对";
            boolean accepted;
            if("connect".equals(action)) {
                setAutoConnect(true);
                if(state.connected)return "手机电话服务已连接";
                reconnect.due(true,false,false,now);
                accepted=(Boolean)onDevice("connect",device);
            } else if("disconnect".equals(action)) {
                if(!state.connected)return "手机电话服务未连接";
                accepted=(Boolean)onDevice("disconnect",device);
            } else {
                if(!state.connected || !state.knownCalls)return "电话连接或通话状态不可用";
                List<?> raw=rawCalls(device);ArrayList<BluetoothCalls.Call> current=new ArrayList<>();
                Object target=null;
                for(Object item:raw) {
                    BluetoothCalls.Call call=decode(item);
                    if(call.state!=7) { current.add(call);if(call.uuid.equals(argument))target=item; }
                }
                if("dial".equals(action)) {
                    String number=BluetoothCalls.number(argument);
                    if(number==null)return "请输入普通电话号码；不支持服务码、分机或重拨指令";
                    if(!current.isEmpty() || now<dialPendingUntil)return "已有通话或拨号正在处理";
                    Object result=invoke("dial",new Class<?>[]{BluetoothDevice.class,String.class},device,number);
                    accepted=result!=null;
                    if(accepted)dialPendingUntil=now+10000;
                } else {
                    if(!BluetoothCalls.allowed(action,argument,current))return "通话已变化或多通话操作不支持，请在手机处理";
                    if("answer".equals(action))accepted=(Boolean)invoke("acceptCall",new Class<?>[]{BluetoothDevice.class,int.class},device,0);
                    else if("reject".equals(action))accepted=(Boolean)onDevice("rejectCall",device);
                    else accepted=(Boolean)invoke("terminateCall",new Class<?>[]{BluetoothDevice.class,target.getClass()},device,target);
                }
            }
            lastCommand=now;
            Log.i(TAG,"HFP command "+action+": queued="+accepted);
            return accepted?"请求已提交，等待手机状态更新":"系统未接受请求，请检查手机连接";
        } catch(Exception error) {
            // Never retry: delivery may already have happened in a remote process.
            lastCommand=now;
            if("dial".equals(action))dialPendingUntil=now+10000;
            Log.w(TAG,"HFP command result uncertain: "+action+"/"+error.getClass().getSimpleName());
            return "请求结果未确认，请查看手机；不会自动重试";
        }
    }
    String tone(String expectedDevice,String uuid,String digit) {
        if(!PhoneUiModel.dtmf(digit))return "无效的通话按键";
        sample(false);long now=SystemClock.elapsedRealtime();
        if(!running || profile==null || !selected.equals(expectedDevice) || !state.connected || !state.knownCalls)
            return "手机连接已变化，请重新打开通话键盘";
        if(now-lastTone<160)return "按键发送中";
        try {
            BluetoothDevice device=adapter.getRemoteDevice(selected);
            ArrayList<BluetoothCalls.Call> current=new ArrayList<>();
            for(Object raw:rawCalls(device)) { BluetoothCalls.Call c=decode(raw);if(c.state!=7)current.add(c); }
            if(!PhoneUiModel.canTone(uuid,current))return "通话已变化，按键未发送";
            lastTone=now;
            boolean accepted=(Boolean)invoke("sendDTMF",new Class<?>[]{BluetoothDevice.class,byte.class},device,(byte)digit.charAt(0));
            Log.i(TAG,"HFP DTMF queued="+accepted); // Never persist the digit sequence.
            return accepted?"通话按键已提交":"手机未接受通话按键";
        } catch(Exception e) { lastTone=now;return "按键结果未确认，不会自动重试"; }
    }
    @Override public void close() {
        running=false;handler.removeCallbacksAndMessages(null);
        if(instance==this)instance=null;
        if(adapter!=null && profile!=null)adapter.closeProfileProxy(HEADSET_CLIENT,profile);
        profile=null;publish("蓝牙电话服务已停止",Collections.emptyList(),Collections.emptyList(),false,false,0);
    }
}
