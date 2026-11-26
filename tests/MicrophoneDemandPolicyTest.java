package com.carplaybox.audio;

public final class MicrophoneDemandPolicyTest {
    static void check(boolean actual, boolean expected, String message) {
        if(actual!=expected)throw new AssertionError(message);
    }
    public static void main(String[] args) {
        check(MicrophoneDemandPolicy.requests(1,10074,10068),true,"one background wake-word MIC must request car input");
        check(MicrophoneDemandPolicy.requests(6,10077,10068),true,"another app may record speech without being foreground");
        check(MicrophoneDemandPolicy.requests(7,10077,10068),true,"communication recorder does not require an active call");
        for(int source:new int[]{0,5,9}) check(MicrophoneDemandPolicy.requests(source,10077,10068),true,"physical recording source " + source);
        for(int source:new int[]{2,3,4,8,1998}) check(MicrophoneDemandPolicy.requests(source,10077,10068),false,"non-mic recording source " + source);
        check(MicrophoneDemandPolicy.requests(1,10068,10068),false,"the capture bridge must not keep its own input open");
        check(MicrophoneDemandPolicy.requests(1,-1,10068),false,"unknown UID cannot request car input");
        System.out.println("Microphone demand: external active microphone sources PASS");
    }
}

