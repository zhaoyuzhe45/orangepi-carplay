package com.carplaybox.audio;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Pure rules shared by the dialer, incoming card and HFP command boundary. */
final class PhoneUiModel {
    static boolean fresh(boolean connected,boolean known,long sampled,long now) {
        return connected && known && sampled>=0 && now>=sampled && now-sampled<=5000;
    }
    static String append(String number,char c) {
        int limit=number.startsWith("+")?31:30;
        if(c=='+' && number.isEmpty())return "+";
        if(c>='0' && c<='9' && number.length()<limit)return number+c;
        return number;
    }
    static boolean dtmf(String value) {
        return value!=null && value.length()==1 && "0123456789*#".indexOf(value.charAt(0))>=0;
    }
    static boolean canTone(String uuid,List<BluetoothCalls.Call> calls) {
        return uuid!=null && calls!=null && calls.size()==1 &&
            uuid.equals(calls.get(0).uuid) && calls.get(0).state==0;
    }
    static final class Incoming {
        private String device="";
        private final Set<String> announced=new HashSet<>();
        String update(String selected,boolean known,List<BluetoothCalls.Call> calls) {
            if(!device.equals(selected)) { device=selected;announced.clear(); }
            if(!known)return ""; // An unavailable provider is not a confirmed end.
            Set<String> live=new HashSet<>();
            for(BluetoothCalls.Call call:calls)live.add(call.uuid);
            announced.retainAll(live);
            if(calls.size()!=1 || calls.get(0).state!=4)return "";
            String id=calls.get(0).uuid;
            return announced.add(id)?id:"";
        }
    }
}
