package com.carplaybox.audio;
public final class MediaExtrasTest {
    public static void main(String[] args)throws Exception{
        MediaExtras e=new MediaExtras();
        e.publish("track-a",new byte[]{1,2,3});
        int generation=e.generation();byte[] chunk=e.chunk(0);
        if(chunk.length!=19 || chunk[15]!=0 || chunk[16]!=1)throw new AssertionError("chunk layout");
        e.publish("track-a",new byte[]{1,2,3});
        if(e.generation()!=generation)throw new AssertionError("stable identity");
        e.publish("track-b",new byte[]{4,5});
        if(e.generation()==generation || e.chunk(9).length!=1)throw new AssertionError("new track and invalid offset");
        e.publish("track-c",new byte[40000]);
        if(e.size()!=0)throw new AssertionError("oversize art");
        System.out.println("Media artwork identity/bounds tests: PASS");
    }
}
