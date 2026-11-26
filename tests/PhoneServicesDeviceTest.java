package com.carplaybox.audio;

import android.app.Instrumentation;
import android.content.Intent;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.SystemClock;

/** Read-only contacts query and optional PBAP connect. Never invokes a telephone command. */
public final class PhoneServicesDeviceTest extends Instrumentation {
    private boolean sync;
    private volatile String status;
    @Override public void onCreate(Bundle args) { super.onCreate(args);sync=args.getString("sync","false").equals("true");start(); }
    @Override public void onStart() {
        Bundle out=new Bundle();
        try {
            Resources bt=getTargetContext().getPackageManager().getResourcesForApplication("com.android.bluetooth");
            for(String flag:new String[]{"profile_supported_hfpclient","profile_supported_pbapclient"})
                if(!bt.getBoolean(bt.getIdentifier(flag,"bool","com.android.bluetooth")))throw new AssertionError(flag+" disabled");
            getTargetContext().startForegroundService(new Intent(getTargetContext(),AppIntegrationService.class));
            for(int i=0;i<40 && BluetoothContacts.current()==null;i++)SystemClock.sleep(250);
            if(BluetoothContacts.current()==null)throw new AssertionError("PBAP bridge unavailable");
            String device=getTargetContext().getSharedPreferences("carplay_bluetooth",0).getString("phone","");
            int before=ContactsRepository.read(getTargetContext(),device).size();out.putInt("contactsBefore",before);
            if(sync && !device.isEmpty()) {
                runOnMainSync(()->status=BluetoothContacts.current().sync(device));
                out.putString("syncRequest",status);SystemClock.sleep(20000);
                runOnMainSync(()->status=BluetoothContacts.current().status());out.putString("syncState",status);
            }
            out.putInt("contactsAfter",ContactsRepository.read(getTargetContext(),device).size());
            out.putString("result","PASS overlay flags, PBAP bridge lifecycle, selected-account ContactsProvider query; no real calls");finish(0,out);
        }catch(Throwable t) { out.putString("error",t.toString());finish(-1,out); }
    }
}
