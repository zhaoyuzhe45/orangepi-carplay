package com.carplaybox.audio;
import java.io.*;
public final class AppSnapshotTest {
    public static void main(String[] args) throws Exception {
        AppSnapshot s=new AppSnapshot();s.sampled=1000;s.status=1;s.speed=100;
        s.duration=Long.MAX_VALUE;s.position=-1;s.title="歌曲\u0000";
        StringBuilder longText=new StringBuilder();for(int i=0;i<100;i++)longText.append("😀");
        s.artist=longText.toString();
        s.mediaGeneration=123;s.artSize=456;s.queueIndex=2;s.queueCount=8;
        s.queueVersion=987654321L;s.queueAvailable=true;
        byte[] bytes=s.encode();DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes));
        if(in.readInt()!=0x43504e33 || in.readLong()!=1000)throw new AssertionError("header");
        if(in.readUnsignedByte()!=1 || in.readUnsignedByte()!=0 || in.readUnsignedShort()!=100)throw new AssertionError("state");
        if(in.readInt()!=-1 || in.readInt()!=0)throw new AssertionError("clamp");
        byte[] title=new byte[in.readUnsignedShort()];in.readFully(title);
        if(!new String(title,"UTF-8").equals("歌曲"))throw new AssertionError("NUL");
        if(in.readUnsignedShort()!=0)throw new AssertionError("empty album");
        byte[] artist=new byte[in.readUnsignedShort()];in.readFully(artist);
        if(artist.length!=252 || new String(artist,"UTF-8").contains("�"))throw new AssertionError("UTF8 truncation");
        for(int i=0;i<3;i++)in.skipBytes(in.readUnsignedShort());
        if(in.readInt()!=123 || in.readInt()!=456 || in.readInt()!=2 || in.readInt()!=8 || in.readLong()!=987654321L || in.readUnsignedByte()!=1 || in.available()!=0)
            throw new AssertionError("CPN2 artwork/queue tail");
        if(!AppSnapshot.mediaKey(87) || AppSnapshot.mediaKey(26))throw new AssertionError("key allowlist");
        System.out.println("Android snapshot codec: PASS");
    }
}
