package com.carplaybox.audio;
import java.io.*;
import java.util.Arrays;
final class MediaExtras {
    private String key="";
    private byte[] art=new byte[0];
    private int generation;
    synchronized void publish(String key,byte[] data){
        if(data==null || data.length>32768)data=new byte[0];
        if(!this.key.equals(key) || !Arrays.equals(art,data)){
            this.key=key;art=data.clone();generation++;if(generation==0)generation=1;
        }
    }
    synchronized int generation(){return generation;}
    synchronized int size(){return art.length;}
    synchronized byte[] chunk(int offset)throws IOException{
        if(offset<0 || offset>=art.length)return new byte[]{1};
        ByteArrayOutputStream b=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(b);
        out.writeInt(0x43504131);out.writeInt(generation);out.writeInt(art.length);out.writeInt(offset);
        out.write(art,offset,Math.min(1024,art.length-offset));out.flush();return b.toByteArray();
    }
}
