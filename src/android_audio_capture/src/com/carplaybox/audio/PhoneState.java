package com.carplaybox.audio;

import android.os.SystemClock;
import java.util.*;

/** Shared immutable IPC model; no Bluetooth connection or Android service ownership. */
final class PhoneState {
    static final class Device {
        final String address,name;
        Device(String address,String name) { this.address=address;this.name=name==null?address:name; }
        @Override public String toString() { return name+" ("+address+")"; }
    }
    final String status,selected,contactsStatus;
    final List<Device> phones;
    final List<BluetoothCalls.Call> calls;
    final boolean connected,knownCalls;
    final int audio;
    final long sampled;
    PhoneState(String status,String selected,List<Device> phones,List<BluetoothCalls.Call> calls,boolean connected,boolean known,int audio) {
        this(status,selected,phones,calls,connected,known,audio,SystemClock.uptimeMillis(),"");
    }
    PhoneState(String status,String selected,List<Device> phones,List<BluetoothCalls.Call> calls,boolean connected,boolean known,int audio,long sampled,String contactsStatus) {
        this.status=status;this.selected=selected;this.phones=Collections.unmodifiableList(new ArrayList<>(phones));
        this.calls=Collections.unmodifiableList(new ArrayList<>(calls));this.connected=connected;this.knownCalls=known;
        this.audio=audio;this.sampled=sampled;this.contactsStatus=contactsStatus;
    }
}
