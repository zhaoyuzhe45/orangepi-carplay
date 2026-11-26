package com.carplaybox.audio;

import android.app.Instrumentation;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Real phone required. HTTP uses the Bluetooth Network explicitly, never the Wi-Fi default. */
public final class BluetoothNetworkDeviceTest extends Instrumentation {
    private boolean lifecycle,wifiFirst,speed;
    private BluetoothProfile pan;
    private final CountDownLatch bound=new CountDownLatch(1);
    private static void check(boolean value,String reason) { if(!value)throw new AssertionError(reason); }
    @Override public void onCreate(Bundle args) {
        super.onCreate(args);lifecycle="true".equals(args.getString("lifecycle"));
        wifiFirst="true".equals(args.getString("wifiFirst"));speed="true".equals(args.getString("speed"));start();
    }
    private BluetoothNetworkBridge bridge() {
        BluetoothNetworkBridge bridge=BluetoothNetworkBridge.current();check(bridge!=null,"PAN bridge not started");return bridge;
    }
    private Network bluetoothNetwork() {
        ConnectivityManager cm=getTargetContext().getSystemService(ConnectivityManager.class);
        for(Network network:cm.getAllNetworks()) {
            NetworkCapabilities caps=cm.getNetworkCapabilities(network);
            LinkProperties link=cm.getLinkProperties(network);
            if(caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_BLUETOOTH) && link!=null && !link.getLinkAddresses().isEmpty())return network;
        }
        return null;
    }
    private Network awaitNetwork() {
        long deadline=SystemClock.elapsedRealtime()+45000;
        while(SystemClock.elapsedRealtime()<deadline) {
            Network network=bluetoothNetwork();
            if(network!=null && bridge().state().connection==2 && !bridge().state().ip.isEmpty())return network;
            SystemClock.sleep(250);
        }
        throw new AssertionError("no Bluetooth IP: "+bridge().state().status);
    }
    private void wifi(boolean enabled)throws Exception {
        Process process=Runtime.getRuntime().exec(new String[]{"su","-c","svc wifi "+(enabled?"enable":"disable")});
        check(process.waitFor(10,TimeUnit.SECONDS),"Wi-Fi command timed out");
        check(process.exitValue()==0,"Wi-Fi command failed");
    }
    private Network awaitWifiDefault() {
        ConnectivityManager cm=getTargetContext().getSystemService(ConnectivityManager.class);
        long deadline=SystemClock.elapsedRealtime()+45000;
        while(SystemClock.elapsedRealtime()<deadline) {
            Network active=cm.getActiveNetwork();NetworkCapabilities caps=cm.getNetworkCapabilities(active);
            if(caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                && bluetoothNetwork()==null && bridge().state().connection==0)return active;
            SystemClock.sleep(250);
        }
        throw new AssertionError("Wi-Fi did not become default with PAN on standby: "+bridge().state().status);
    }
    private void checkBluetoothDefault(Network network) {
        ConnectivityManager cm=getTargetContext().getSystemService(ConnectivityManager.class);
        check(network.equals(cm.getActiveNetwork()),"fallback Bluetooth network is not system default");
    }
    private void internet(Network network,Bundle result,String prefix)throws Exception {
        check(network.getAllByName("www.baidu.com").length>0,"Bluetooth DNS failed");
        HttpURLConnection connection=(HttpURLConnection)network.openConnection(new URL("https://www.baidu.com/robots.txt"));
        try {
            connection.setConnectTimeout(10000);connection.setReadTimeout(10000);connection.setInstanceFollowRedirects(false);
            int response=connection.getResponseCode();
            check(response==200,"Bluetooth HTTPS status="+response);
            check(connection.getInputStream().read()>=0,"Bluetooth HTTPS empty response");
            result.putInt(prefix+"Https",response);result.putString(prefix+"Network",network.toString());
            result.putString(prefix+"Status",bridge().state().status);
            Log.i("BluetoothNetworkDeviceTest",prefix+" DNS/HTTPS passed on "+network+" "+bridge().state().status);
        } finally { connection.disconnect(); }
    }
    private void throughput(Network network,Bundle result)throws Exception {
        HttpURLConnection connection=(HttpURLConnection)network.openConnection(new URL("https://speed.cloudflare.com/__down?bytes=2097152"));
        try {
            connection.setConnectTimeout(10000);connection.setReadTimeout(20000);connection.setInstanceFollowRedirects(false);
            check(connection.getResponseCode()==200,"Bluetooth speed test HTTP status="+connection.getResponseCode());
            byte[] buffer=new byte[32768];int count,total=0;long start=SystemClock.elapsedRealtime();
            while((count=connection.getInputStream().read(buffer))>=0)total+=count;
            long elapsed=Math.max(1,SystemClock.elapsedRealtime()-start);
            long kbps=(long)total*8/elapsed;
            result.putInt("downloadedBytes",total);result.putLong("downloadDurationMs",elapsed);result.putLong("downloadKbps",kbps);
            Log.i("BluetoothNetworkDeviceTest","Bluetooth-bound download bytes="+total+" durationMs="+elapsed+" kbps="+kbps);
        } finally { connection.disconnect(); }
    }
    @Override public void onStart() {
        Bundle result=new Bundle();BluetoothAdapter adapter=BluetoothAdapter.getDefaultAdapter();boolean originallyEnabled=true,readEnabled=false;
        boolean originalWifi=Settings.Global.getInt(getTargetContext().getContentResolver(),"wifi_on",0)!=0,wifiChanged=false;
        int resultCode=0;
        try {
            getTargetContext().startForegroundService(new Intent(getTargetContext(),AppIntegrationService.class));
            long start=SystemClock.elapsedRealtime();
            while(BluetoothNetworkBridge.current()==null && SystemClock.elapsedRealtime()-start<5000)SystemClock.sleep(100);
            originallyEnabled=bridge().enabled();readEnabled=true;bridge().setEnabled(true);
            if(wifiFirst) {
                check(originalWifi,"Wi-Fi priority test requires Wi-Fi initially enabled");
                result.putString("preferredWifiNetwork",awaitWifiDefault().toString());
                SystemClock.sleep(6500);check(bluetoothNetwork()==null,"PAN reconnected while Wi-Fi usable");
                Log.i("BluetoothNetworkDeviceTest","Wi-Fi default and PAN standby confirmed");
                wifiChanged=true;long switchAt=SystemClock.elapsedRealtime();wifi(false);
                Network fallback=awaitNetwork();checkBluetoothDefault(fallback);
                result.putLong("wifiLossToBluetoothMs",SystemClock.elapsedRealtime()-switchAt);
                internet(fallback,result,"fallback");
            }
            Network initial=awaitNetwork();internet(initial,result,"initial");if(speed)throughput(initial,result);
            if(lifecycle) {
                bridge().setEnabled(false);
                long deadline=SystemClock.elapsedRealtime()+25000;
                while((bridge().state().connection!=0 || bluetoothNetwork()!=null) && SystemClock.elapsedRealtime()<deadline)SystemClock.sleep(250);
                check(bridge().state().connection==0 && bluetoothNetwork()==null,"manual disable did not disconnect");
                SystemClock.sleep(6500);check(bluetoothNetwork()==null,"disabled PAN reconnected");
                Log.i("BluetoothNetworkDeviceTest","Manual disable stayed disconnected");
                bridge().setEnabled(true);Network reenabled=awaitNetwork();internet(reenabled,result,"reenabled");
                Log.i("BluetoothNetworkDeviceTest","Manual re-enable recovered on "+reenabled);
                adapter.getProfileProxy(getTargetContext(),new BluetoothProfile.ServiceListener() {
                    @Override public void onServiceConnected(int type,BluetoothProfile profile) { pan=profile;bound.countDown(); }
                    @Override public void onServiceDisconnected(int type) {}
                },5);
                check(bound.await(8,TimeUnit.SECONDS),"test PAN proxy unavailable");
                BluetoothDevice phone=adapter.getRemoteDevice(bridge().state().phone);
                check((Boolean)pan.getClass().getMethod("disconnect",BluetoothDevice.class).invoke(pan,phone),"external disconnect refused");
                deadline=SystemClock.elapsedRealtime()+45000;Network recovered=null;
                while(SystemClock.elapsedRealtime()<deadline) {
                    Network candidate=bluetoothNetwork();
                    if(candidate!=null && !candidate.equals(reenabled) && bridge().state().connection==2 && !bridge().state().ip.isEmpty()) { recovered=candidate;break; }
                    SystemClock.sleep(250);
                }
                check(recovered!=null,"unexpected PAN loss did not reconnect");internet(recovered,result,"recovered");
                Log.i("BluetoothNetworkDeviceTest","Unexpected PAN loss recovered on "+recovered);
            }
            if(wifiFirst) {
                long switchAt=SystemClock.elapsedRealtime();wifi(true);
                result.putString("restoredWifiNetwork",awaitWifiDefault().toString());
                result.putLong("wifiEnableToDefaultMs",SystemClock.elapsedRealtime()-switchAt);
                SystemClock.sleep(6500);check(bluetoothNetwork()==null,"PAN reconnected after Wi-Fi restoration");
                wifiChanged=false;
            }
            result.putString("result","PASS Bluetooth-bound DNS/HTTPS"+(lifecycle?", manual disable/reenable and external-loss recovery":"")+(wifiFirst?", Wi-Fi priority/fallback/switchback":""));
        } catch(Throwable failure) { resultCode=-1;result.putString("error",Log.getStackTraceString(failure));Log.e("BluetoothNetworkDeviceTest","FAILED",failure); }
        finally {
            if(wifiChanged)try { wifi(originalWifi); }catch(Exception failure) { resultCode=-1;result.putString("restoreWifiError",Log.getStackTraceString(failure)); }
            BluetoothNetworkBridge bridge=BluetoothNetworkBridge.current();if(bridge!=null && readEnabled)bridge.setEnabled(originallyEnabled);
            if(pan!=null)adapter.closeProfileProxy(5,pan);
            Log.i("BluetoothNetworkDeviceTest","Finished; network enabled="+originallyEnabled);
        }
        finish(resultCode,result);
    }
}
