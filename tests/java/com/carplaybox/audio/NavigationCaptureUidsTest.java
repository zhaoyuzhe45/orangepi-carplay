package com.carplaybox.audio;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.function.ToIntFunction;

public final class NavigationCaptureUidsTest {
    private static void check(int[] actual,int... expected){
        if(!Arrays.equals(expected,actual)){
            throw new AssertionError("expected "+Arrays.toString(expected)+", got "+Arrays.toString(actual));
        }
    }

    public static void main(String[] args){
        Map<String,Integer> installed=new HashMap<>();
        ToIntFunction<String> lookup=name->installed.getOrDefault(name,-1);
        check(NavigationCaptureUids.resolve("",lookup));
        installed.put("com.baidu.naviauto",10042);
        check(NavigationCaptureUids.resolve("",lookup),10042);
        installed.put("com.autonavi.amapauto",10051);
        check(NavigationCaptureUids.resolve("1",lookup),10051,10042);
        installed.put("cn.audi.knobmap.amap1",10051);
        check(NavigationCaptureUids.resolve("1",lookup),10051,10042);
        installed.put("com.baidu.naviauto",10255);
        check(NavigationCaptureUids.resolve("1",lookup),10051,10255);
        check(NavigationCaptureUids.resolve("0",name->{throw new AssertionError("disabled lookup");}));
        checkTransport();
        System.out.println("Navigation UID resolution: current installs, shared UID and split switch passed");
    }

    private static void checkTransport(){
        Map<String,String> props=new HashMap<>();
        java.util.function.Function<String,String> read=key->props.getOrDefault(key,"");
        if(NavigationCaptureUids.useAltTransport(read))throw new AssertionError("default must stay off");
        props.put("persist.carplay.nav101","1");
        if(!NavigationCaptureUids.useAltTransport(read))throw new AssertionError("persistent enable ignored");
        props.put("debug.carplay.nav101","0");
        if(NavigationCaptureUids.useAltTransport(read))throw new AssertionError("debug disable ignored");
        props.clear();
        props.put("debug.carplay.nav101","1");
        if(!NavigationCaptureUids.useAltTransport(read))throw new AssertionError("debug enable ignored");
        props.put("debug.carplay.audio_nav_split","0");
        if(NavigationCaptureUids.useAltTransport(read))throw new AssertionError("capture split is disabled");
    }
}
