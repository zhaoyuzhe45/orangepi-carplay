package com.carplaybox.audio;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Amap 10001 data; types 0/1 navigate and type 2 cruises (installed APK). */
final class RouteSnapshot {
    private long sampled=-1;
    private int state=255,icon=255,index=65535,remaining,seconds,nextDistance;
    private String current="",next="",destination="";
    static int maneuver(int icon) {
        // Amap's documented IconType values; unsupported icons never become straight ahead.
        switch(icon) {case 2:return 1;case 3:return 2;case 8:case 19:return 4;
            case 9:return 3;case 11:case 17:return 6;case 12:case 18:return 7;
            case 15:return 12;case 65:return 13;case 66:return 14;default:return 255;}
    }
    synchronized void update(int type,int rawIcon,int segment,int remaining,int seconds,int nextDistance,
                            String current,String next,String destination,long now) {
        if(type==2){state=0;sampled=now;this.current=this.next=this.destination="";return;}
        if((type!=0 && type!=1) || remaining<0 || remaining>100000000 || seconds<0 || seconds>604800 ||
            nextDistance<0 || nextDistance>100000000 || now<0)return;
        this.state=1;this.sampled=now;this.icon=maneuver(rawIcon);
        this.index=segment>=0 && segment<65535?segment:65535;
        this.remaining=remaining;this.seconds=seconds;this.nextDistance=nextDistance;
        this.current=clean(current);this.next=clean(next);this.destination=clean(destination);
    }
    private static String clean(String s){return s==null?"":s.substring(0,Math.min(512,s.length())).replaceAll("[\\x00-\\x1f\\x7f]","");}
    private static void string(DataOutputStream out,String s)throws IOException{
        byte[] b=s.getBytes(StandardCharsets.UTF_8);int n=Math.min(255,b.length);
        if(n<b.length)while(n>0 && (b[n]&192)==128)n--;out.writeShort(n);out.write(b,0,n);
    }
    synchronized byte[] encode(long now)throws IOException {
        boolean fresh=sampled>=0 && now>=sampled && now-sampled<=10000;
        ByteArrayOutputStream b=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(b);
        out.writeInt(0x43505231);out.writeLong(now);out.writeByte(fresh?state:255);
        out.writeByte(fresh && state==1?icon:255);out.writeShort(fresh && state==1?index:65535);
        out.writeInt(remaining);out.writeInt(seconds);out.writeInt(nextDistance);
        string(out,fresh?current:"");string(out,fresh?next:"");string(out,fresh?destination:"");string(out,"高德地图");
        out.flush();return b.toByteArray();
    }
}
