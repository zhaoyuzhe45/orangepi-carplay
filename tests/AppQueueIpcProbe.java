import android.net.*;
import java.io.*;
public final class AppQueueIpcProbe {
 public static void main(String[] args)throws Exception{
  int op=args.length==0?0:Integer.parseInt(args[0]);if(op!=0 && op!=9 && op!=10)throw new IllegalArgumentException("queue-only probe");
  try(LocalSocket socket=new LocalSocket()){
   socket.connect(new LocalSocketAddress("carplay.app.integration.v1"));socket.setSoTimeout(2000);
   DataOutputStream out=new DataOutputStream(socket.getOutputStream());out.writeInt(0x43505131);out.writeInt(op);out.writeInt(args.length>1?Integer.parseInt(args[1]):0);
   if(op==10)out.writeLong(Long.parseLong(args[2]));out.flush();
   DataInputStream in=new DataInputStream(socket.getInputStream());int n=in.readInt();if(n<1 || n>2048)throw new IOException("bad response length");
   byte[] b=new byte[n];in.readFully(b);StringBuilder s=new StringBuilder();for(byte v:b)s.append(String.format("%02x",v&255));System.out.println(s);
  }
 }
}
