package com.carplaybox.audio;

/** PAN connection scheduling uses monotonic time and never sleeps. */
final class BluetoothPanPolicy {
    static final int NONE=0,CONNECT=1,DISCONNECT=2;
    private long nextAttempt,delay=5000,busySince=-1,lastCancel=-60000;
    private int busyState=-1;
    private boolean wasEnabled=true;

    static String target(String preferred,String[] paired,String[] napPeers) {
        if(preferred!=null && !preferred.isEmpty()) {
            for(String address:paired)if(preferred.equals(address))return preferred;
            return "";
        }
        String selected="";
        for(String address:paired)for(String nap:napPeers)if(address.equals(nap)) {
            if(!selected.isEmpty() && !selected.equals(address))return "";
            selected=address;
        }
        return selected;
    }
    void reset() {
        nextAttempt=0;delay=5000;busySince=-1;busyState=-1;lastCancel=-60000;wasEnabled=true;
    }
    int next(boolean enabled,int state,long now) {
        if(enabled!=wasEnabled) {
            nextAttempt=0;delay=5000;busySince=-1;lastCancel=-60000;wasEnabled=enabled;
        }
        if(!enabled) {
            nextAttempt=0;delay=5000;busySince=-1;
            if(state!=0 && now-lastCancel>=5000) { lastCancel=now;return DISCONNECT; }
            return NONE;
        }
        if(state==2) { nextAttempt=0;delay=5000;busySince=-1;busyState=-1;return NONE; }
        if(state==1 || state==3) {
            if(busySince<0 || busyState!=state) { busySince=now;busyState=state; }
            if(now-busySince>=20000 && now-lastCancel>=20000) {
                lastCancel=now;nextAttempt=now+delay;busySince=now;return DISCONNECT;
            }
            return NONE;
        }
        busySince=-1;busyState=-1;
        if(now<nextAttempt)return NONE;
        nextAttempt=now+delay;delay=Math.min(60000,delay*2);return CONNECT;
    }
    int next(boolean enabled,boolean wifiUsable,int state,long now) {
        return next(enabled && !wifiUsable,state,now);
    }
}
