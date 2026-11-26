package com.carplaybox.audio;

/** 48 kHz stereo PCM mixer. Activity follows actual speech samples, not route state. */
final class NavigationMixer {
    private float mediaGain=1f;
    private long lastNow=-1,holdUntil=-1;
    private boolean navigationActive;
    boolean navigationActive(){return navigationActive;}
    void reset(){mediaGain=1f;lastNow=holdUntil=-1;navigationActive=false;}
    void mix(short[] media,short[] navigation,short[] output,long now){
        if(media==null || navigation==null || output==null || media.length!=navigation.length ||
            media.length!=output.length || media.length==0 || (media.length&1)!=0 || now<0)
            throw new IllegalArgumentException("bounded equal stereo frames required");
        if(lastNow>now)reset();lastNow=now;
        int peak=0;long energy=0;
        for(short sample:navigation){int v=sample;peak=Math.max(peak,Math.abs(v));energy+=(long)v*v;}
        if(peak>=160 && energy/navigation.length>=1600)holdUntil=now+250;
        navigationActive=holdUntil>=now;
        float target=navigationActive?.25f:1f;
        float step=navigationActive?.75f/1920f:.75f/24000f;
        for(int i=0;i<media.length;i+=2){
            mediaGain=mediaGain<target?Math.min(target,mediaGain+step):Math.max(target,mediaGain-step);
            for(int c=0;c<2;c++){
                int v=Math.round(media[i+c]*mediaGain)+navigation[i+c];
                output[i+c]=(short)Math.max(-32768,Math.min(32767,v));
            }
        }
    }
}
