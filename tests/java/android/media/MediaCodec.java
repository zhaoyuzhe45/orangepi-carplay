package android.media;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import android.os.Bundle;
import android.view.Surface;
public final class MediaCodec {
 public static final int CONFIGURE_FLAG_ENCODE=1,BUFFER_FLAG_KEY_FRAME=1,BUFFER_FLAG_CODEC_CONFIG=2,BUFFER_FLAG_END_OF_STREAM=4,BUFFER_FLAG_PARTIAL_FRAME=8,INFO_TRY_AGAIN_LATER=-1,INFO_OUTPUT_FORMAT_CHANGED=-2,INFO_OUTPUT_BUFFERS_CHANGED=-3;
 public static final String PARAMETER_KEY_REQUEST_SYNC_FRAME="request-sync";
 public static MediaCodec last; public static String requestedName; public static CountDownLatch parameters;
 public static boolean failConfigure,partial,missingConfig; public static volatile boolean idle;
 public MediaFormat format; public final Surface surface=new Surface(); public boolean stopped,released,started; public int releasedBuffers,effectiveProfile; public Thread parameterThread; public Bundle request; private int index;
 public static void reset(){last=null;requestedName=null;parameters=new CountDownLatch(1);failConfigure=partial=missingConfig=idle=false;}
 public static MediaCodec createByCodecName(String name){requestedName=name;last=new MediaCodec();return last;}
 public String getName(){return requestedName;}
 public void configure(MediaFormat format,Surface s,Object crypto,int flags){if(failConfigure)throw new IllegalStateException("configure failure");if(format.containsKey("profile")&&!format.containsKey("level"))throw new IllegalStateException("Android 8.1 INVALID_OPERATION: profile requires level");effectiveProfile=format.containsKey("profile")?format.getInteger("profile"):MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline;this.format=format;}
 public Surface createInputSurface(){return surface;} public void start(){started=true;} public void stop(){stopped=true;} public void release(){released=true;}
 public void setParameters(Bundle b){request=b;parameterThread=Thread.currentThread();parameters.countDown();}
 public int dequeueOutputBuffer(BufferInfo info,long timeout){
  if(idle){try{Thread.sleep(1);}catch(InterruptedException ignored){} return INFO_TRY_AGAIN_LATER;}
  if(index++==0)return INFO_OUTPUT_FORMAT_CHANGED;
  info.offset=1;info.presentationTimeUs=123456789;
  if(partial && index==2){info.size=5;info.flags=BUFFER_FLAG_KEY_FRAME|BUFFER_FLAG_PARTIAL_FRAME;return 0;}
  if(partial && index==3){info.size=1;info.flags=0;return 1;}
  if(!partial && index==2){info.size=6;info.flags=BUFFER_FLAG_KEY_FRAME;return 0;}
  info.offset=0;info.size=0;info.flags=BUFFER_FLAG_END_OF_STREAM;return 2;
 }
 public ByteBuffer getOutputBuffer(int index){if(partial)return ByteBuffer.wrap(index==0?new byte[]{99,0,0,0,1,0x65,88}:new byte[]{99,9,88});return ByteBuffer.wrap(new byte[]{99,0,0,0,1,0x65,9,88});}
 public MediaFormat getOutputFormat(){MediaFormat f=new MediaFormat();if(!missingConfig){f.setByteBuffer("csd-0",ByteBuffer.wrap(new byte[]{0,0,0,1,0x67,9}));f.setByteBuffer("csd-1",ByteBuffer.wrap(new byte[]{0,0,0,1,0x68,8}));}return f;}
 public void releaseOutputBuffer(int index,boolean render){releasedBuffers++;}
 public static final class BufferInfo { public int offset,size,flags;public long presentationTimeUs; }
}
