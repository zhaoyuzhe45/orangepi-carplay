package com.carplaybox.audio;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Versioned, bounded private IPC format; deliberately independent of Android. */
final class AppSnapshot {
    long sampled, duration, position;
    int status, speed;
    boolean canSeek;
    int mediaGeneration,artSize,queueIndex=-1,queueCount=-1;
    long queueVersion;boolean queueAvailable;
    String title="", album="", artist="", composer="", appName="", packageName="";
    static boolean mediaKey(int key) { return key==126 || key==127 || key==85 || key==87 || key==88; }
    private static long u32(long n) { return Math.max(0,Math.min(0xffffffffL,n)); }
    private static void string(DataOutputStream out,String text) throws IOException {
        byte[] data=(text==null?"":text.replace("\u0000","")).getBytes(StandardCharsets.UTF_8);
        int count=Math.min(255,data.length);
        if(count<data.length)while(count>0 && (data[count]&0xc0)==0x80)count--;
        out.writeShort(count);out.write(data,0,count);
    }
    byte[] encode() throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        DataOutputStream out=new DataOutputStream(bytes);
        out.writeInt(0x43504e33);out.writeLong(sampled);
        out.writeByte(status);out.writeByte(canSeek?1:0);out.writeShort(speed);
        out.writeInt((int)u32(duration));out.writeInt((int)u32(position));
        for(String text:new String[]{title,album,artist,composer,appName,packageName})string(out,text);
        out.writeInt(mediaGeneration);out.writeInt(artSize);out.writeInt(queueIndex);out.writeInt(queueCount);
        out.writeLong(queueVersion);out.writeByte(queueAvailable?1:0);
        out.flush();return bytes.toByteArray();
    }
}
