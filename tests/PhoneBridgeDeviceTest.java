package com.carplaybox.audio;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import java.util.*;
/** Exercises cross-APK reads and lifecycle. Never invokes any real call command. */
public final class PhoneBridgeDeviceTest extends Instrumentation {
    @Override public void onCreate(Bundle args){super.onCreate(args);start();}
    @Override public void onStart(){
        Bundle out=new Bundle();
        try{
            runOnMainSync(()->PhoneBackend.LIVE.start(getTargetContext()));
            for(int i=0;i<40 && PhoneBackend.LIVE.state()==null;i++)SystemClock.sleep(250);
            PhoneState actual=PhoneBackend.LIVE.state();
            if(actual==null)throw new AssertionError("Cross-APK state unavailable");
            out.putBoolean("phoneConnected",actual.connected);
            if(!getTargetContext().getPackageName().equals("com.carplaybox.phone"))throw new AssertionError("Wrong APK");
            try{Class.forName("com.carplaybox.audio.BluetoothCallBridge",false,getTargetContext().getClassLoader());throw new AssertionError("UI contains Bluetooth service implementation");}
            catch(ClassNotFoundException expected){}
            if(!PhoneBackend.LIVE.contacts("00:00:00:00:00:01").isEmpty())throw new AssertionError("Other phone contacts exposed");
            Bundle state=PhoneBackend.LIVE.call("state",new Bundle());
            if(PhoneBridgeContract.decode(state)==null)throw new AssertionError("State decode failed");
            try{PhoneBackend.LIVE.call("unknown-operation",new Bundle());throw new AssertionError("Unknown operation accepted");}
            catch(IllegalArgumentException expected){}
            List<BluetoothCalls.Call> fixture=Arrays.asList(new BluetoothCalls.Call("fixture-id",4,false,"123456789"));
            PhoneState encoded=new PhoneState("test","fixture-device",Collections.emptyList(),fixture,true,true,0,1234,"test contacts");
            PhoneState decoded=PhoneBridgeContract.decode(PhoneBridgeContract.encode(encoded,encoded.contactsStatus));
            if(decoded.sampled!=1234 || !decoded.calls.get(0).uuid.equals("fixture-id") || !decoded.contactsStatus.equals("test contacts"))throw new AssertionError("IPC loses target/freshness");
            if(PhoneBridgeContract.decode(new Bundle())!=null)throw new AssertionError("Old protocol accepted");
            IncomingCallActivity card=(IncomingCallActivity)startActivitySync(new Intent(getTargetContext(),IncomingCallActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra("device","fixture-device").putExtra("uuid","fixture-id"));
            SystemClock.sleep(700);waitForIdleSync();
            runOnMainSync(()->{if(!card.isFinishing() && !card.isDestroyed())throw new AssertionError("Wrong-device incoming activity remains visible");});
            out.putString("result","PASS separate UID/model IPC, no HFP owner in phone APK, scoped contacts, unknown-operation rejection, stale incoming lifecycle; no real calls");finish(0,out);
        }catch(Throwable t){out.putString("error",t.toString());finish(-1,out);}
        finally{runOnMainSync(()->PhoneBackend.LIVE.stop());}
    }
}
