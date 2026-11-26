package com.carplaybox.audio;
import java.util.List;

/** Single-call vehicle button policy. No dialing or automatic redial action. */
final class PhoneVehicleKeys {
    static boolean fresh(long sampled,long now) { return sampled>=0 && now>=sampled && now-sampled<=1500; }
    static String action(int key,boolean connected,boolean known,List<BluetoothCalls.Call> calls) {
        if((key!=1 && key!=2) || !connected || !known || calls==null)return null;
        if(calls.isEmpty())return key==1?"open":null;
        if(calls.size()!=1)return null;
        BluetoothCalls.Call call=calls.get(0);
        if(call.state==4)return key==1?"answer":"reject";
        if(call.state>=0 && call.state<=3)return "hangup";
        return null;
    }
}
