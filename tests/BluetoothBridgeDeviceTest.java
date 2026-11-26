package com.carplaybox.audio;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.content.res.Resources;

/** Test-only instrumentation. Never dials, answers, rejects or hangs up a call. */
public final class BluetoothBridgeDeviceTest extends Instrumentation {
    private String address;
    private volatile PhoneState state;
    private volatile String response;
    @Override public void onCreate(Bundle args) { super.onCreate(args);address=args.getString("device","");start(); }
    private void sample() { runOnMainSync(()-> {
        BluetoothCallBridge bridge=BluetoothCallBridge.current();
        if(bridge!=null) { bridge.sample();state=bridge.state(); }
    }); }
    @Override public void onStart() {
        Bundle result=new Bundle();
        try {
            Resources bt=getTargetContext().getPackageManager().getResourcesForApplication("com.android.bluetooth");
            if(!bt.getBoolean(bt.getIdentifier("profile_supported_hfpclient","bool","com.android.bluetooth")))throw new AssertionError("HFP disabled");
            if(bt.getBoolean(bt.getIdentifier("headset_client_initial_audio_route_allowed","bool","com.android.bluetooth")))throw new AssertionError("SCO auto route enabled");
            getTargetContext().startForegroundService(new Intent(getTargetContext(),AppIntegrationService.class));
            for(int i=0;i<30;i++) { sample();if(state!=null && !state.phones.isEmpty())break;SystemClock.sleep(500); }
            if(state==null)throw new AssertionError("bridge not started");
            if(!address.isEmpty()) {
                runOnMainSync(()-> { BluetoothCallBridge bridge=BluetoothCallBridge.current();response=bridge.select(address); });
                sample();if(!state.selected.equals(address))throw new AssertionError("select: "+response);
                runOnMainSync(()->response=BluetoothCallBridge.current().command("connect",address,""));
                for(int i=0;i<30 && !state.connected;i++) {
                    SystemClock.sleep(1000);sample();
                }
                if(!state.connected)throw new AssertionError("connect failed: "+response+" / "+state.status);
                if(!state.knownCalls)throw new AssertionError("unknown calls");
                SystemClock.sleep(1600);
                result.putInt("actualCalls",state.calls.size());
                runOnMainSync(()->response=BluetoothCallBridge.current().command("dial","stale-device","1234567"));
                if(!response.contains("已变更"))throw new AssertionError("stale device allowed");
                runOnMainSync(()->response=BluetoothCallBridge.current().command("dial",address,""));
                if(!response.contains("普通电话号码"))throw new AssertionError("empty redial allowed: "+response);
                runOnMainSync(()->response=BluetoothCallBridge.current().command("hangup",address,"stale-call-uuid"));
                if(!response.contains("通话已变化"))throw new AssertionError("stale call allowed: "+response);
                if(!state.calls.isEmpty())throw new AssertionError("skip connection lifecycle while real call exists");
                runOnMainSync(()->response=BluetoothCallBridge.current().command("disconnect",address,""));
                SystemClock.sleep(7000);sample();
                if(state.connected)throw new AssertionError("manual disconnect reconnected");
                if(getTargetContext().getSharedPreferences("carplay_bluetooth",0).getBoolean("auto_connect",true))throw new AssertionError("manual disconnect retained reconnect");
                runOnMainSync(()->response=BluetoothCallBridge.current().command("connect",address,""));
                for(int i=0;i<30;i++) { SystemClock.sleep(1000);sample();if(state.connected)break; }
                if(!state.connected)throw new AssertionError("reconnect failed: "+response);
                if(!getTargetContext().getSharedPreferences("carplay_bluetooth",0).getBoolean("auto_connect",false))throw new AssertionError("connect lost reconnect preference");
                result.putInt("scoState",state.audio);
            }
            result.putString("status",state.status);result.putString("result","PASS real HFP connection/state and non-destructive validation");finish(0,result);
        } catch(Throwable failure) { result.putString("error",failure.toString());finish(-1,result); }
    }
}
