import android.net.*;
import java.io.*;
public final class AudioSocketProbe {
 public static void main(String[] args)throws Exception{
  try(LocalServerSocket server=new LocalServerSocket("carplay_audio_capture_v1")){
   for(int pass=0;pass<2;pass++)try(LocalSocket socket=server.accept()){
    socket.setSoTimeout(5000);DataInputStream in=new DataInputStream(socket.getInputStream());byte[] frame=new byte[3840];
    long energy=0,samples=0,start=android.os.SystemClock.elapsedRealtime();
    for(int i=0;i<75;i++){in.readFully(frame);if(i>=25)for(int j=0;j<frame.length;j+=2){int s=(short)((frame[j]&255)|(frame[j+1]<<8));energy+=(long)s*s;samples++;}}
    double rms=Math.sqrt((double)energy/samples);if(rms<1000)throw new AssertionError("PCM not delivered: "+rms);
    System.out.println("session="+pass+" frames=75 bytes=288000 rms="+rms+" elapsedMs="+(android.os.SystemClock.elapsedRealtime()-start));
   }
  }
  System.out.println("PASS production socket frame stream and reconnection");
 }
}
