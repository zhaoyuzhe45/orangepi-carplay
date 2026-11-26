package com.carplaybox.encoder;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.SurfaceControl;
import java.io.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class ScreenEncoderTest {
 private static int checks;
 private static final String[] ARGS={"1280","720","8000000","30"};
 public static void main(String[] args) throws Exception {
  configuresHardwareAndWritesConfigBeforeCompleteFrames();
  limitsBitrateAndPreservesLowerValues();
  stretchesWholePresetIntoReceiverCanvas();
  padsEncoderHeightWithoutCroppingSource();
  assemblesPartialCodecBuffers();
  emitsHeartbeatWhilePictureIsStill();
  refreshesCaptureOnOwnerThreadWithoutReplacingCodec();
  requestsKeyframeOnEncodingThreadAndStopsOnEof();
  stopsOnQAndUnblocksAnOutputWrite();
  releasesResourcesWhenSetupOrOutputFails();
  rejectsFramesWithoutConfiguration();
  rejectsInvalidArguments();
  rejectsUnencodableDimensionsBeforeCreatingCodec();
  System.out.println("ScreenEncoderTest: "+checks+" checks passed");
 }
 private static void reset(){MediaCodec.reset();SurfaceControl.reset();Thread.interrupted();}
 private static void configuresHardwareAndWritesConfigBeforeCompleteFrames() throws Exception {
  reset();ByteArrayOutputStream out=new ByteArrayOutputStream();
  eq(ScreenEncoder.run(ARGS,new HeldInput(),out,()->{}),0,"success");
  check("OMX.rk.video_encoder.avc".equals(MediaCodec.requestedName),"explicit hardware name");
  MediaFormat f=MediaCodec.last.format;
  eq(f.getInteger("width"),1280,"width");eq(f.getInteger("height"),720,"height");eq(f.getInteger("bitrate"),2000000,"bitrate capped before MediaCodec");
  eq(f.getInteger("frame-rate"),30,"fps");eq(f.getInteger("i-frame-interval"),1,"I-frame interval");
  eq(f.getInteger("profile"),1,"Baseline profile");eq(f.getInteger("level"),0x200,"legal Level 4.0");
  eq(MediaCodec.last.effectiveProfile,1,"ACodec default remains Baseline");check(f.getLong("repeat-previous-frame-after")==1000000,"repeat one second");
  check(f.getFloat("max-fps-to-encoder")==30f,"fps cap");eq(SurfaceControl.rotation,0,"rotation");eq(SurfaceControl.layer,0,"layer");
  eq(SurfaceControl.physical.right,1280,"physical width");eq(SurfaceControl.physical.bottom,720,"physical height");
  DataInputStream packets=new DataInputStream(new ByteArrayInputStream(out.toByteArray()));
  eq(packet(packets,1,0).length,12,"SPS PPS before frame");
  byte[] frame=packet(packets,2,123456789);check(frame.length==6 && frame[4]==0x65 && frame[5]==9,"complete key access unit");
  eq(packet(packets,4,123456789).length,0,"EOS");eq(packets.read(),-1,"no extra stdout bytes");cleaned();
 }
 private static void limitsBitrateAndPreservesLowerValues() throws Exception {
  int[] requested={2000000,500000,2147483647};
  int[] expected={2000000,500000,2000000};
  for(int i=0;i<requested.length;i++){
   reset();String[] args={"1280","720",""+requested[i],"30"};
   eq(ScreenEncoder.run(args,new HeldInput(),new ByteArrayOutputStream(),()->{}),0,"bitrate case succeeds");
   eq(MediaCodec.last.format.getInteger("bitrate"),expected[i],"effective bitrate");cleaned();
  }
 }
 private static void assemblesPartialCodecBuffers() throws Exception {
  reset();MediaCodec.partial=true;ByteArrayOutputStream out=new ByteArrayOutputStream();
  eq(ScreenEncoder.run(ARGS,new HeldInput(),out,()->{}),0,"partial result");
  DataInputStream packets=new DataInputStream(new ByteArrayInputStream(out.toByteArray()));packet(packets,1,0);
  byte[] frame=packet(packets,2,123456789);check(frame.length==6 && frame[5]==9,"partial buffers joined into one AU");
  packet(packets,4,123456789);eq(packets.read(),-1,"no partial packet");cleaned();
 }
 private static void stretchesWholePresetIntoReceiverCanvas() throws Exception {
  reset();ByteArrayOutputStream out=new ByteArrayOutputStream();
  String[] scaled={"1366","768","8000000","30","1280","720"};
  eq(ScreenEncoder.run(scaled,new HeldInput(),out,()->{}),0,"scaled capture success");
  eq(MediaCodec.last.format.getInteger("width"),1376,"macroblock aligned encoded width");
  eq(MediaCodec.last.format.getInteger("height"),768,"receiver encoded height");
  eq(SurfaceControl.physical.right,1280,"whole physical preset source width");
  eq(SurfaceControl.physical.bottom,720,"whole physical preset source height");
  eq(SurfaceControl.output.right,1366,"stretched output width");
  eq(SurfaceControl.output.bottom,768,"stretched output height");cleaned();
 }
 private static void padsEncoderHeightWithoutCroppingSource() throws Exception {
  reset();ByteArrayOutputStream out=new ByteArrayOutputStream();
  String[] scaled={"1280","600","8000000","30","800","480"};
  eq(ScreenEncoder.run(scaled,new HeldInput(),out,()->{}),0,"height padded capture success");
  eq(MediaCodec.last.format.getInteger("width"),1280,"aligned width unchanged");
  eq(MediaCodec.last.format.getInteger("height"),608,"macroblock aligned encoded height");
  eq(SurfaceControl.physical.right,800,"whole source width retained");
  eq(SurfaceControl.physical.bottom,480,"whole source height retained");
  eq(SurfaceControl.output.right,1280,"visible receiver width");
  eq(SurfaceControl.output.bottom,600,"visible receiver height excludes padding");cleaned();
 }
 private static void requestsKeyframeOnEncodingThreadAndStopsOnEof() throws Exception {
  reset();MediaCodec.idle=true;Thread owner=Thread.currentThread();AtomicInteger unblocks=new AtomicInteger();
  InputStream commands=new InputStream(){int reads;public int read()throws IOException{if(reads++==0)return 'K';try{if(!MediaCodec.parameters.await(2,TimeUnit.SECONDS))throw new IOException("K was not serviced");}catch(InterruptedException e){throw new IOException(e);}return -1;}};
  eq(ScreenEncoder.run(ARGS,commands,new ByteArrayOutputStream(),unblocks::incrementAndGet),0,"EOF stops");
  check(MediaCodec.last.parameterThread==owner,"setParameters on owner thread");
  check("request-sync".equals(MediaCodec.last.request.key),"sync request");eq(unblocks.get(),1,"EOF unblocks peer output");cleaned();
 }
 private static void emitsHeartbeatWhilePictureIsStill() throws Exception {
  reset();MediaCodec.idle=true;CountDownLatch beats=new CountDownLatch(2);
  ByteArrayOutputStream out=new ByteArrayOutputStream(){public synchronized void write(byte[] b,int off,int len){super.write(b,off,len);if(len==24 && b[off+7]==8)beats.countDown();}};
  InputStream commands=new InputStream(){public int read()throws IOException{try{beats.await(3,TimeUnit.SECONDS);}catch(InterruptedException e){throw new IOException(e);}return 'Q';}};
  eq(ScreenEncoder.run(ARGS,commands,out,()->{}),0,"idle encoder stopped");
  check(beats.getCount()==0,"idle encoding thread emits heartbeat packets");
  DataInputStream packets=new DataInputStream(new ByteArrayInputStream(out.toByteArray()));
  eq(packet(packets,8,0).length,0,"heartbeat without fake video");
  eq(packet(packets,8,0).length,0,"second heartbeat");cleaned();
 }
 private static void refreshesCaptureOnOwnerThreadWithoutReplacingCodec() throws Exception {
  reset();MediaCodec.idle=true;Thread owner=Thread.currentThread();
  InputStream commands=new InputStream(){int reads;public int read()throws IOException{if(reads++==0)return 'R';try{SurfaceControl.refreshed.await(2,TimeUnit.SECONDS);}catch(InterruptedException e){throw new IOException(e);}return 'Q';}};
  eq(ScreenEncoder.run(ARGS,commands,new ByteArrayOutputStream(),()->{}),0,"refresh then quit");
  eq(SurfaceControl.creates,2,"capture display recreated");eq(SurfaceControl.destroys,2,"both displays released");
  check(SurfaceControl.attachThread==owner,"capture refresh on codec owner thread");
  check(MediaCodec.last.parameterThread==owner,"refresh also requests keyframe");cleaned();
 }
 private static void stopsOnQAndUnblocksAnOutputWrite() throws Exception {
  reset();CountDownLatch writing=new CountDownLatch(1),release=new CountDownLatch(1);
  OutputStream blocked=new OutputStream(){public void write(int value)throws IOException{writing.countDown();try{if(!release.await(2,TimeUnit.SECONDS))throw new IOException("not unblocked");}catch(InterruptedException ignored){}throw new IOException("peer closed");}};
  InputStream commands=new InputStream(){public int read()throws IOException{try{writing.await(2,TimeUnit.SECONDS);}catch(InterruptedException e){throw new IOException(e);}return 'Q';}};
  long start=System.nanoTime();eq(ScreenEncoder.run(ARGS,commands,blocked,release::countDown),0,"Q stops");
  check(System.nanoTime()-start<TimeUnit.SECONDS.toNanos(2),"bounded blocked output stop");cleaned();
 }
 private static void releasesResourcesWhenSetupOrOutputFails() throws Exception {
  reset();SurfaceControl.failProjection=true;eq(ScreenEncoder.run(ARGS,new HeldInput(),new ByteArrayOutputStream(),()->{}),1,"projection failure");cleaned();check(SurfaceControl.closed,"transaction closed");
  reset();MediaCodec.failConfigure=true;eq(ScreenEncoder.run(ARGS,new HeldInput(),new ByteArrayOutputStream(),()->{}),1,"configure failure");check(MediaCodec.last.released,"unstarted codec released");
  reset();eq(ScreenEncoder.run(ARGS,new HeldInput(),new OutputStream(){public void write(int b)throws IOException{throw new IOException("broken pipe");}},()->{}),1,"output failure");cleaned();
 }
 private static void rejectsFramesWithoutConfiguration() throws Exception {
  reset();MediaCodec.missingConfig=true;ByteArrayOutputStream out=new ByteArrayOutputStream();eq(ScreenEncoder.run(ARGS,new HeldInput(),out,()->{}),1,"config absent");eq(out.size(),0,"no undecodable frame emitted");cleaned();
 }
 private static void rejectsInvalidArguments(){
  String[][] invalid={{},{"0","720","1","30"},{"1281","720","1","30"},{"1280","720","-1","30"},{"1280","720","1","0"},{"1280","720","1","30","extra"}};
  for(String[] args:invalid){reset();eq(ScreenEncoder.run(args,new HeldInput(),new ByteArrayOutputStream(),()->{}),1,"invalid args");check(MediaCodec.last==null,"no resources for invalid args");}
 }
 private static void rejectsUnencodableDimensionsBeforeCreatingCodec(){
  int[][] invalid={{3168,1440},{1440,3168},{1922,1080},{1920,1090},{174,144},{176,142}};
  for(int[] size:invalid){
   reset();String[] args={""+size[0],""+size[1],"8000000","30","1024","480"};
   eq(ScreenEncoder.run(args,new HeldInput(),new ByteArrayOutputStream(),()->{}),1,"unencodable dimensions rejected");
   check(MediaCodec.last==null,"unsupported size cannot allocate codec resources");
  }
 }
 private static byte[] packet(DataInputStream in,int flags,long pts)throws IOException{eq(in.readInt(),0x43504531,"packet magic");eq(in.readInt(),flags,"packet flags");int n=in.readInt();eq(in.readInt(),0,"reserved");check(in.readLong()==pts,"packet pts");byte[] b=new byte[n];in.readFully(b);return b;}
 private static void cleaned(){check(MediaCodec.last.released,"codec released");check(MediaCodec.last.surface.released,"surface released");check(SurfaceControl.destroyed,"display destroyed");}
 private static void eq(int actual,int expected,String label){check(actual==expected,label+": "+actual+" != "+expected);}
 private static void check(boolean value,String label){if(!value)throw new AssertionError(label);checks++;}
 private static final class HeldInput extends InputStream {private boolean closed;public synchronized int read()throws IOException{while(!closed)try{wait();}catch(InterruptedException e){throw new IOException(e);}return -1;}public synchronized void close(){closed=true;notifyAll();}}
}
