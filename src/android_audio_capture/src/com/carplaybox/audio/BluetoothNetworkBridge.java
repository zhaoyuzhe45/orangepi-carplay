package com.carplaybox.audio;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelUuid;
import android.os.SystemClock;
import android.util.Log;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.List;

/** Android owns PANU/DHCP/DNS. Bluetooth/network IPC stays off the main thread. */
final class BluetoothNetworkBridge implements AutoCloseable {
    private static final String TAG="CarPlayPan",PREFS="carplay_bluetooth_network";
    private static final int PAN=5;
    private static final ParcelUuid NAP=ParcelUuid.fromString("00001116-0000-1000-8000-00805f9b34fb");
    private static volatile BluetoothNetworkBridge current;
    private final Context context;
    private final SharedPreferences preferences,phonePreferences;
    private final BluetoothPanPolicy policy=new BluetoothPanPolicy();
    private final HandlerThread thread=new HandlerThread("carplay-bluetooth-network");
    private Handler handler;
    private BluetoothAdapter adapter;
    private ConnectivityManager connectivity;
    private BluetoothProfile profile;
    private boolean binding,receiverRegistered,networkRegistered,wifiUsable;
    private long bindAt=-5000;
    private String selected="";
    private volatile boolean running;
    private volatile Snapshot snapshot=new Snapshot("",0,"","",false,"蓝牙共享网络正在后台启动");

    static final class Snapshot {
        final String phone,ip,dns,status;
        final int connection;
        final boolean validated;
        Snapshot(String phone,int connection,String ip,String dns,boolean validated,String status) {
            this.phone=phone;this.connection=connection;this.ip=ip;this.dns=dns;this.validated=validated;this.status=status;
        }
    }
    BluetoothNetworkBridge(Context context) {
        this.context=context.getApplicationContext();
        preferences=this.context.getSharedPreferences(PREFS,0);
        phonePreferences=this.context.getSharedPreferences("carplay_bluetooth",0);
    }
    static BluetoothNetworkBridge current() { return current; }
    Snapshot state() { return snapshot; }
    boolean enabled() { return preferences.getBoolean("enabled",false); }
    void setEnabled(boolean enabled) { preferences.edit().putBoolean("enabled",enabled).apply();wake(); }
    void start() {
        if(running)return;
        running=true;current=this;thread.start();handler=new Handler(thread.getLooper());
        handler.post(()->{
            if(!running)return;
            adapter=BluetoothAdapter.getDefaultAdapter();connectivity=context.getSystemService(ConnectivityManager.class);
            IntentFilter filter=new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
            filter.addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
            filter.addAction("android.bluetooth.pan.profile.action.CONNECTION_STATE_CHANGED");
            try { context.registerReceiver(receiver,filter,null,handler);receiverRegistered=true; }
            catch(RuntimeException failure) { Log.w(TAG,"Bluetooth broadcasts unavailable; polling continues",failure); }
            if(connectivity!=null)try {
                connectivity.registerNetworkCallback(new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build(),networkChanges,handler);
                networkRegistered=true;
            } catch(RuntimeException failure) { Log.w(TAG,"Wi-Fi callbacks unavailable; polling continues",failure); }
            phonePreferences.registerOnSharedPreferenceChangeListener(phoneChanges);
            poll.run();
        });
    }
    private final SharedPreferences.OnSharedPreferenceChangeListener phoneChanges=(prefs,key)->{
        if("phone".equals(key))wake();
    };
    private final BroadcastReceiver receiver=new BroadcastReceiver() {
        @Override public void onReceive(Context c,Intent intent) { wake(); }
    };
    private final ConnectivityManager.NetworkCallback networkChanges=new ConnectivityManager.NetworkCallback() {
        @Override public void onAvailable(Network network) { wake(); }
        @Override public void onLost(Network network) { wake(); }
        @Override public void onCapabilitiesChanged(Network network,NetworkCapabilities capabilities) { wake(); }
    };
    private void wake() {
        Handler h=handler;
        if(running && h!=null) { h.removeCallbacks(poll);h.post(poll); }
    }
    private final Runnable poll=new Runnable() {
        @Override public void run() {
            if(!running)return;
            try { sample(); }
            catch(Exception failure) { publish(0,"","",false,"蓝牙共享网络暂不可用："+failure.getClass().getSimpleName());Log.w(TAG,"PAN sample failed",failure); }
            if(running) { handler.removeCallbacks(this);handler.postDelayed(this,3000); }
        }
    };
    private final BluetoothProfile.ServiceListener listener=new BluetoothProfile.ServiceListener() {
        @Override public void onServiceConnected(int type,BluetoothProfile value) {
            Handler h=handler;
            if(h==null || !h.post(()->{
                if(!running) { adapter.closeProfileProxy(PAN,value);return; }
                if(profile!=null && profile!=value)adapter.closeProfileProxy(PAN,profile);
                profile=value;binding=false;wake();
            }))adapter.closeProfileProxy(PAN,value);
        }
        @Override public void onServiceDisconnected(int type) {
            // The existing framework proxy automatically rebinds when Bluetooth restarts.
            wake();
        }
    };
    private void publish(int connection,String ip,String dns,boolean validated,String status) {
        Snapshot before=snapshot;
        snapshot=new Snapshot(selected,connection,ip,dns,validated,status);
        if(!before.status.equals(status))Log.i(TAG,status);
    }
    private boolean command(String operation,BluetoothDevice device)throws Exception {
        return (Boolean)profile.getClass().getMethod(operation,BluetoothDevice.class).invoke(profile,device);
    }
    private boolean hasUsableWifi() {
        if(connectivity==null)return false;
        // PAN can outrank Wi-Fi, so examine every network rather than the current default.
        for(Network network:connectivity.getAllNetworks()) {
            NetworkCapabilities caps=connectivity.getNetworkCapabilities(network);
            if(caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))return true;
        }
        return false;
    }
    private void sample()throws Exception {
        boolean enabled=enabled();
        wifiUsable=hasUsableWifi();
        if(adapter==null) { publish(0,"","",false,"设备没有蓝牙适配器");return; }
        if(!adapter.isEnabled()) { policy.reset();publish(0,"","",false,"蓝牙已关闭");return; }
        List<String> paired=new ArrayList<>(),napPeers=new ArrayList<>();
        for(BluetoothDevice device:adapter.getBondedDevices()) {
            paired.add(device.getAddress());
            ParcelUuid[] uuids=device.getUuids();
            if(uuids!=null)for(ParcelUuid uuid:uuids)if(NAP.equals(uuid)) { napPeers.add(device.getAddress());break; }
        }
        String preferred=phonePreferences.getString("phone","");
        if(preferred.isEmpty())preferred=preferences.getString("phone","");
        String next=BluetoothPanPolicy.target(preferred,paired.toArray(new String[0]),napPeers.toArray(new String[0]));
        if(!next.equals(selected)) {
            // Only release our previous target, leaving unrelated PAN connections alone.
            if(profile!=null && !selected.isEmpty())command("disconnect",adapter.getRemoteDevice(selected));
            selected=next;policy.reset();
        }
        if(selected.isEmpty()) { publish(0,"","",false,enabled?"请选择已配对的共享网络手机（电话应用内）":"蓝牙共享网络已关闭");return; }
        if(!selected.equals(preferences.getString("phone","")))preferences.edit().putString("phone",selected).apply();
        if(profile==null) {
            // Bind even in standby/disabled mode to release a PAN link inherited after a restart.
            long now=SystemClock.elapsedRealtime();
            if(binding && now-bindAt>15000)binding=false;
            if(!binding && now-bindAt>=5000) { bindAt=now;binding=adapter.getProfileProxy(context,listener,PAN); }
            publish(0,"","",false,"正在等待蓝牙网络服务");return;
        }
        BluetoothDevice device=adapter.getRemoteDevice(selected);
        int connection=profile.getConnectionState(device);
        int action=policy.next(enabled,wifiUsable,connection,SystemClock.elapsedRealtime());
        if(action!=BluetoothPanPolicy.NONE) {
            String operation=action==BluetoothPanPolicy.CONNECT?"connect":"disconnect";
            boolean accepted=command(operation,device);
            Log.i(TAG,"PAN "+operation+" accepted="+accepted);
        }
        if(!enabled) { publish(connection,"","",false,"蓝牙共享网络已关闭");return; }
        if(wifiUsable) { publish(connection,"","",false,"优先使用 Wi-Fi；蓝牙备用网络待机");return; }
        String name=device.getName();if(name==null)name="已选手机";
        String ip="",dns="";boolean validated=false;
        if(connection==BluetoothProfile.STATE_CONNECTED && connectivity!=null) {
            for(Network network:connectivity.getAllNetworks()) {
                NetworkCapabilities capabilities=connectivity.getNetworkCapabilities(network);
                LinkProperties link=connectivity.getLinkProperties(network);
                if(capabilities==null || !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_BLUETOOTH) || link==null)continue;
                StringBuilder addresses=new StringBuilder(),servers=new StringBuilder();
                for(LinkAddress address:link.getLinkAddresses()) {
                    if(address.getAddress().isLinkLocalAddress())continue;
                    if(addresses.length()>0)addresses.append(", ");addresses.append(address.toString());
                }
                for(InetAddress address:link.getDnsServers()) {
                    if(servers.length()>0)servers.append(", ");servers.append(address.getHostAddress());
                }
                ip=addresses.toString();dns=servers.toString();
                validated=capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);break;
            }
        }
        String status;
        if(!ip.isEmpty())status="蓝牙网络已连接："+name+"\nIP："+ip+"\nDNS："+dns+"\n"+(validated?"系统互联网检测通过":"尚未通过系统互联网检测");
        else if(connection==BluetoothProfile.STATE_CONNECTED)status="蓝牙已连接 "+name+"，正在获取网络地址";
        else if(connection==BluetoothProfile.STATE_CONNECTING)status="正在连接 "+name+" 的共享网络";
        else status="等待 "+name+" 的蓝牙共享网络；后台自动重试";
        publish(connection,ip,dns,validated,status);
    }
    @Override public void close() {
        running=false;if(current==this)current=null;
        Handler h=handler;if(h==null)return;
        h.post(()->{
            if(receiverRegistered)context.unregisterReceiver(receiver);
            if(networkRegistered && connectivity!=null)connectivity.unregisterNetworkCallback(networkChanges);
            phonePreferences.unregisterOnSharedPreferenceChangeListener(phoneChanges);
            if(adapter!=null && profile!=null)adapter.closeProfileProxy(PAN,profile);
            profile=null;handler.removeCallbacksAndMessages(null);thread.quitSafely();
        });
        // An established system-managed PAN link survives service/application restarts.
    }
}
