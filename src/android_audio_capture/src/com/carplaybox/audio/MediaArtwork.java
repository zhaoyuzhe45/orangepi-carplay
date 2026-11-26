package com.carplaybox.audio;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import java.io.ByteArrayOutputStream;

/** Bounded local MediaSession bitmap cache. No external URL fetching. */
final class MediaArtwork {
    private String key="";private int bitmapId;private byte[] bytes=new byte[0];
    byte[] read(String track,MediaMetadata metadata){
        Bitmap image=null;
        if(metadata!=null)for(String field:new String[]{MediaMetadata.METADATA_KEY_ART,MediaMetadata.METADATA_KEY_ALBUM_ART,MediaMetadata.METADATA_KEY_DISPLAY_ICON}){
            image=metadata.getBitmap(field);if(image!=null)break;
        }
        int id=image==null?0:image.getGenerationId();
        if(track.equals(key) && id==bitmapId)return bytes;
        key=track;bitmapId=id;bytes=new byte[0];
        if(image==null || image.isRecycled() || image.getWidth()<=0 || image.getHeight()<=0)return bytes;
        Bitmap scaled=null;
        try{
            float scale=Math.min(1f,256f/Math.max(image.getWidth(),image.getHeight()));
            scaled=Bitmap.createScaledBitmap(image,Math.max(1,Math.round(image.getWidth()*scale)),Math.max(1,Math.round(image.getHeight()*scale)),true);
            for(int quality:new int[]{80,60,40}){
                ByteArrayOutputStream b=new ByteArrayOutputStream();
                if(scaled.compress(Bitmap.CompressFormat.JPEG,quality,b) && b.size()<=32768){bytes=b.toByteArray();break;}
            }
        }catch(RuntimeException invalid){bytes=new byte[0];}
        finally{if(scaled!=null && scaled!=image)scaled.recycle();}
        return bytes;
    }
}
