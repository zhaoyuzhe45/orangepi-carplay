package com.carplaybox.encoder;
import android.graphics.Rect;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.system.Os;
import android.system.OsConstants;
import android.view.Surface;
import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Root app_process entry point. stdout is exclusively the private CPE1 stream. */
public final class ScreenEncoder {
    private static final String CODEC_NAME="OMX.rk.video_encoder.avc";
    private static final int MAX_BITRATE=2000000;
    private static final String PREFIX="carplay-encoder: ";
    private ScreenEncoder() { }

    public static void main(String[] args) {
        // The descriptors are two ends of the same private socket in the child.
        // shutdown() also wakes a write blocked behind peer backpressure on EOF/Q.
        int result=run(args,new FileInputStream(FileDescriptor.in),
                new FileOutputStream(FileDescriptor.out),() -> {
                    try { Os.shutdown(FileDescriptor.out,OsConstants.SHUT_RDWR); }
                    catch(Exception ignored) { }
                });
        System.exit(result);
    }

    static int run(String[] args,InputStream in,OutputStream out,Runnable unblockOutput) {
        MediaCodec codec=null;
        Surface surface=null;
        Display display=null;
        boolean started=false;
        Control control=new Control(Thread.currentThread(),in,unblockOutput);
        CountDownLatch finished=new CountDownLatch(1);
        Thread shutdownHook=new Thread(() -> {
            control.requestStop();
            try { finished.await(1200,TimeUnit.MILLISECONDS); }
            catch(InterruptedException ignored) { Thread.currentThread().interrupt(); }
        },"encoder-shutdown");
        boolean hooked=false;
        long frames=0,keys=0,bytes=0;
        int result=1;
        try {
            int[] settings=parseArgs(args);
            settings[2]=Math.min(settings[2],MAX_BITRATE);
            if(Looper.myLooper()==null) Looper.prepare();
            Runtime.getRuntime().addShutdownHook(shutdownHook); hooked=true;
            codec=MediaCodec.createByCodecName(CODEC_NAME);
            String actualName=codec.getName();
            if(!CODEC_NAME.equals(actualName)) throw new IOException("unexpected codec "+actualName);
            // Rockchip's surface encoder rejects even sizes such as1366x768
            // unless the allocation is macroblock aligned. Keep the receiver's
            // entire visible canvas at the top-left; C rewrites SPS cropping
            // to remove only the right/bottom allocation padding.
            int codedWidth=(settings[0]+15)&~15;
            int codedHeight=(settings[1]+15)&~15;
            log("codec="+actualName+" visible="+settings[0]+"x"+settings[1]+
                    " coded="+codedWidth+"x"+codedHeight+" bitrate="+settings[2]+" fps="+settings[3]);
            MediaFormat format=MediaFormat.createVideoFormat("video/avc",codedWidth,codedHeight);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            format.setInteger(MediaFormat.KEY_BIT_RATE,settings[2]);
            format.setInteger(MediaFormat.KEY_FRAME_RATE,settings[3]);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);
            // RK3399's default AVC pair is Baseline/Level 3.1 even when the
            // surface is 1800x1080. A strict CarPlay receiver may accept the
            // config while misrendering every macroblock row from this illegal
            // conformance level. Select a legal pair explicitly.
            format.setInteger(MediaFormat.KEY_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
            format.setInteger(MediaFormat.KEY_LEVEL,
                    MediaCodecInfo.CodecProfileLevel.AVCLevel4);
            format.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER,1000000L);
            // This hidden key is understood by the Android 8.1 encoder surface path.
            format.setFloat("max-fps-to-encoder",(float)settings[3]);
            codec.configure(format,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);
            surface=codec.createInputSurface();
            display=new Display();
            display.attach(surface,settings[0],settings[1],settings[4],settings[5]);
            codec.start(); started=true;
            control.start();
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            int lengthSize=4;
            boolean configSent=false;
            ByteArrayOutputStream partial=new ByteArrayOutputStream();
            int partialFlags=0;
            long partialPts=0,lastStats=System.nanoTime(),lastHeartbeat=lastStats;
            while(!control.stop.get()) {
                boolean refreshCapture=control.refresh.getAndSet(false);
                boolean requestKeyframe=control.keyframe.getAndSet(false);
                if(refreshCapture || requestKeyframe) {
                    Bundle parameters=new Bundle();
                    parameters.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME,0);
                    codec.setParameters(parameters);
                    log("request-keyframe");
                }
                if(refreshCapture) {
                    // A still SurfaceFlinger display need not submit new buffers.
                    // Reattach capture before treating silence as a codec fault.
                    display.close();display=null;
                    display=new Display();
                    display.attach(surface,settings[0],settings[1],settings[4],settings[5]);
                    log("capture-refresh");
                }
                int index=codec.dequeueOutputBuffer(info,100000);
                if(control.stop.get()) break;
                if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    if(partial.size()!=0) throw new IOException("format changed within access unit");
                    MediaFormat output=codec.getOutputFormat();
                    ByteArrayOutputStream configuration=new ByteArrayOutputStream();
                    for(int i=0;i<2;i++) {
                        String key="csd-"+i;
                        if(output.containsKey(key)) {
                            AnnexB.Configuration csd=AnnexB.configuration(output.getByteBuffer(key),lengthSize);
                            lengthSize=csd.lengthSize;
                            if((long)configuration.size()+csd.data.length>PacketWriter.MAX_PAYLOAD) throw new IOException("oversize codec config");
                            configuration.write(csd.data);
                        }
                    }
                    if(configuration.size()!=0) {
                        PacketWriter.write(out,PacketWriter.CONFIG,0,configuration.toByteArray());
                        configSent=true;
                        log("config bytes="+configuration.size());
                    }
                } else if(index>=0) {
                    try {
                        if(info.size<0 || info.offset<0 || info.size>PacketWriter.MAX_PAYLOAD) throw new IOException("invalid codec buffer bounds");
                        boolean end=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                        boolean fragment=(info.flags&MediaCodec.BUFFER_FLAG_PARTIAL_FRAME)!=0;
                        boolean config=(info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0;
                        if(fragment && (end || config)) throw new IOException("invalid fragmented config/EOS");
                        if(info.size>0) {
                            ByteBuffer buffer=codec.getOutputBuffer(index);
                            if(buffer==null || (long)info.offset+info.size>buffer.capacity()) throw new IOException("codec buffer exceeds capacity");
                            buffer=buffer.duplicate();buffer.clear();buffer.position(info.offset);buffer.limit(info.offset+info.size);
                            if(config) {
                                if(partial.size()!=0) throw new IOException("config within access unit");
                                AnnexB.Configuration csd=AnnexB.configuration(buffer,lengthSize);
                                lengthSize=csd.lengthSize;
                                PacketWriter.write(out,PacketWriter.CONFIG,info.presentationTimeUs,csd.data);
                                configSent=true;
                            } else {
                                if(!configSent) throw new IOException("frame before codec config");
                                if(partial.size()==0) partialPts=info.presentationTimeUs;
                                else if(info.presentationTimeUs!=partialPts) throw new IOException("fragment timestamps differ");
                                if((long)partial.size()+info.size>PacketWriter.MAX_PAYLOAD) throw new IOException("oversize access unit");
                                byte[] chunk=new byte[info.size];buffer.get(chunk);partial.write(chunk);
                                if((info.flags&MediaCodec.BUFFER_FLAG_KEY_FRAME)!=0) partialFlags|=PacketWriter.KEYFRAME;
                                if(!fragment) {
                                    byte[] accessUnit=AnnexB.normalize(ByteBuffer.wrap(partial.toByteArray()),lengthSize);
                                    PacketWriter.write(out,partialFlags,partialPts,accessUnit);
                                    frames++;bytes+=accessUnit.length;
                                    if((partialFlags&PacketWriter.KEYFRAME)!=0) {
                                        keys++;
                                        if(keys==1 || keys%30==0) log("keyframe ptsUs="+partialPts+" bytes="+accessUnit.length);
                                    }
                                    partial.reset();partialFlags=0;
                                }
                            }
                        }
                        if(end) {
                            if(partial.size()!=0) throw new IOException("EOS within access unit");
                            PacketWriter.write(out,PacketWriter.EOS,info.presentationTimeUs,new byte[0]);
                            break;
                        }
                    } finally { codec.releaseOutputBuffer(index,false); }
                }
                long now=System.nanoTime();
                if(now-lastHeartbeat>=TimeUnit.SECONDS.toNanos(1)) {
                    // Owner-thread liveness only: never substitute this for a video frame.
                    PacketWriter.write(out,PacketWriter.HEARTBEAT,frames,new byte[0]);
                    lastHeartbeat=now;
                }
                if(now-lastStats>=TimeUnit.SECONDS.toNanos(30)) {
                    log("frames="+frames+" keys="+keys+" bytes="+bytes);lastStats=now;
                }
            }
            result=0;
        } catch(Exception error) {
            if(control.stop.get()) result=0;
            else log("error "+error.getClass().getSimpleName()+": "+error.getMessage());
        } finally {
            // Mark stop before closing input, so a normal completion EOF does not
            // shutdown the socket while the final packet is still being handled.
            control.stop.set(true);
            if(display!=null) { try { display.close(); } catch(Exception e) { log("display cleanup: "+e); } }
            if(surface!=null) { try { surface.release(); } catch(Exception e) { log("surface cleanup: "+e); } }
            if(codec!=null) {
                if(started) { try { codec.stop(); } catch(Exception e) { log("codec stop: "+e); } }
                try { codec.release(); } catch(Exception e) { log("codec release: "+e); }
            }
            control.close();
            finished.countDown();
            if(hooked) { try { Runtime.getRuntime().removeShutdownHook(shutdownHook); } catch(IllegalStateException ignored) { } }
            Thread.interrupted();
            log("stop status="+result+" frames="+frames+" keys="+keys+" bytes="+bytes);
        }
        return result;
    }

    private static int[] parseArgs(String[] args) {
        if(args.length!=4 && args.length!=6) throw new IllegalArgumentException("usage: ScreenEncoder width height bitrate fps [sourceWidth sourceHeight]");
        int[] settings=new int[6];
        for(int i=0;i<args.length;i++) {
            settings[i]=Integer.parseInt(args[i]);
            if(settings[i]<=0) throw new IllegalArgumentException("positive arguments required");
        }
        if(args.length==4) { settings[4]=settings[0];settings[5]=settings[1]; }
        // Validate before creating OMX resources. Failed oversized starts can
        // exhaust this BSP's 32-bit media.codec address space across retries.
        if(settings[0]<176 || settings[1]<144 || settings[0]>1920 || settings[1]>1088)
            throw new IllegalArgumentException("RK3399 encoder size must fit 176x144..1920x1088");
        if((settings[0]&1)!=0 || (settings[1]&1)!=0) throw new IllegalArgumentException("even capture dimensions required");
        if((settings[4]&1)!=0 || (settings[5]&1)!=0) throw new IllegalArgumentException("even source dimensions required");
        return settings;
    }
    private static void log(String value) { System.err.println(PREFIX+value); }

    private static final class Control {
        final AtomicBoolean stop=new AtomicBoolean(),keyframe=new AtomicBoolean(),refresh=new AtomicBoolean();
        private final Thread owner;
        private final InputStream in;
        private final Runnable unblockOutput;
        private Thread reader;
        Control(Thread owner,InputStream in,Runnable unblockOutput) { this.owner=owner;this.in=in;this.unblockOutput=unblockOutput; }
        void start() {
            reader=new Thread(() -> {
                try {
                    while(!stop.get()) {
                        int command=in.read();
                        if(command<0 || command=='Q') { requestStop();break; }
                        if(command=='K') keyframe.set(true);
                        if(command=='R') refresh.set(true);
                    }
                } catch(IOException e) { requestStop(); }
            },"encoder-commands");
            reader.setDaemon(true);reader.start();
        }
        void requestStop() {
            if(stop.compareAndSet(false,true)) { unblockOutput.run();owner.interrupt(); }
        }
        void close() {
            try { in.close(); } catch(IOException ignored) { }
            if(reader!=null) {
                reader.interrupt();
                try { reader.join(200); } catch(InterruptedException ignored) { }
            }
        }
    }

    /** Android 8.1 hidden API, resolved on the installed framework. */
    private static final class Display {
        private final Class<?> surfaceControl;
        private final IBinder token;
        Display() throws Exception {
            surfaceControl=Class.forName("android.view.SurfaceControl");
            token=(IBinder)method("createDisplay",String.class,boolean.class).invoke(null,"CarPlayEncoder",true);
            if(token==null) throw new IOException("createDisplay returned null");
        }
        void attach(Surface surface,int width,int height,int sourceWidth,int sourceHeight) throws Exception {
            method("openTransaction").invoke(null);
            try {
                method("setDisplaySurface",IBinder.class,Surface.class).invoke(null,token,surface);
                Rect source=new Rect(0,0,sourceWidth,sourceHeight);
                Rect output=new Rect(0,0,width,height);
                method("setDisplayProjection",IBinder.class,int.class,Rect.class,Rect.class).invoke(null,token,0,source,output);
                method("setDisplayLayerStack",IBinder.class,int.class).invoke(null,token,0);
            } finally { method("closeTransaction").invoke(null); }
        }
        void close() throws Exception { method("destroyDisplay",IBinder.class).invoke(null,token); }
        private Method method(String name,Class<?>... types) throws Exception { return surfaceControl.getDeclaredMethod(name,types); }
    }
}
