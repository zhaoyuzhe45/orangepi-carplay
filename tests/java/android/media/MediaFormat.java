package android.media;
import java.nio.ByteBuffer;
import java.util.HashMap;
public class MediaFormat {
 public static final String KEY_COLOR_FORMAT="color-format",KEY_BIT_RATE="bitrate",KEY_FRAME_RATE="frame-rate",KEY_I_FRAME_INTERVAL="i-frame-interval",KEY_PROFILE="profile",KEY_LEVEL="level",KEY_REPEAT_PREVIOUS_FRAME_AFTER="repeat-previous-frame-after";
 private final HashMap<String,Object> values=new HashMap<>();
 public static MediaFormat createVideoFormat(String mime,int width,int height) { MediaFormat f=new MediaFormat();f.values.put("mime",mime);f.setInteger("width",width);f.setInteger("height",height);return f; }
 public void setInteger(String k,int v){values.put(k,v);} public void setLong(String k,long v){values.put(k,v);} public void setFloat(String k,float v){values.put(k,v);}
 public int getInteger(String k){return ((Number)values.get(k)).intValue();} public long getLong(String k){return ((Number)values.get(k)).longValue();} public float getFloat(String k){return ((Number)values.get(k)).floatValue();}
 public ByteBuffer getByteBuffer(String k){return (ByteBuffer)values.get(k);} public void setByteBuffer(String k,ByteBuffer v){values.put(k,v);} public boolean containsKey(String k){return values.containsKey(k);}
}
