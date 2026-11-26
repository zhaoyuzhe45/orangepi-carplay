package com.carplaybox.audio;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import java.util.*;

final class PhoneBridgeContract {
    static final String PERMISSION="com.carplaybox.audio.permission.PHONE_BRIDGE";
    static final Uri URI=Uri.parse("content://com.carplaybox.audio.phone");
    static Intent openPhone() { return new Intent().setClassName("com.carplaybox.phone","com.carplaybox.audio.PhoneActivity"); }
    static Intent incoming() { return new Intent().setClassName("com.carplaybox.phone","com.carplaybox.audio.IncomingCallActivity"); }
    static Bundle encode(PhoneState s,String contacts) {
        Bundle b=new Bundle();b.putInt("version",1);if(s==null)return b;
        b.putString("status",s.status);b.putString("selected",s.selected);b.putBoolean("connected",s.connected);b.putBoolean("known",s.knownCalls);
        b.putLong("sampled",s.sampled);b.putInt("audio",s.audio);b.putString("contactsStatus",contacts);
        ArrayList<Bundle> phones=new ArrayList<>(),calls=new ArrayList<>();
        for(PhoneState.Device d:s.phones) { Bundle v=new Bundle();v.putString("address",d.address);v.putString("name",d.name);phones.add(v); }
        for(BluetoothCalls.Call c:s.calls) { Bundle v=new Bundle();v.putString("uuid",c.uuid);v.putInt("state",c.state);v.putString("number",c.number);v.putBoolean("outgoing",c.outgoing);calls.add(v); }
        b.putParcelableArrayList("phones",phones);b.putParcelableArrayList("calls",calls);return b;
    }
    static PhoneState decode(Bundle b) {
        if(b==null || b.getInt("version")!=1 || !b.containsKey("sampled"))return null;
        ArrayList<PhoneState.Device> phones=new ArrayList<>();ArrayList<BluetoothCalls.Call> calls=new ArrayList<>();
        ArrayList<Bundle> raw=b.getParcelableArrayList("phones");
        if(raw!=null)for(Bundle v:raw)phones.add(new PhoneState.Device(v.getString("address",""),v.getString("name","")));
        raw=b.getParcelableArrayList("calls");
        if(raw!=null)for(Bundle v:raw)calls.add(new BluetoothCalls.Call(v.getString("uuid",""),v.getInt("state"),v.getBoolean("outgoing"),v.getString("number","")));
        return new PhoneState(b.getString("status",""),b.getString("selected",""),phones,calls,b.getBoolean("connected"),b.getBoolean("known"),b.getInt("audio"),b.getLong("sampled"),b.getString("contactsStatus",""));
    }
}
