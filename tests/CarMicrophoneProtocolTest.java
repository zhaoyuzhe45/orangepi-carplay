import java.lang.reflect.*;
import java.util.Arrays;

public final class CarMicrophoneProtocolTest {
    static void check(boolean value) { if(!value) throw new AssertionError(); }
    static void number(byte[] b,int offset,int n) { for(int i=0;i<4;i++) b[offset+i]=(byte)(n>>>(8*i)); }
    public static void main(String[] args) throws Exception {
        Class<?> c;
        try { c=Class.forName("com.carplaybox.audio.CarMicrophoneBridge"); }
        catch(ClassNotFoundException e) { throw new AssertionError("Android microphone bridge is not implemented",e); }
        Method validate=c.getDeclaredMethod("validateHeader",byte[].class);validate.setAccessible(true);
        Method command=c.getDeclaredMethod("encodeCommand",int.class);command.setAccessible(true);
        Method source=c.getDeclaredMethod("isMicrophoneSource",int.class);source.setAccessible(true);
        byte[] header=new byte[20];header[0]='C';header[1]='P';header[2]='M';header[3]='1';number(header,4,1);number(header,12,16000);number(header,16,1);
        check((int)validate.invoke(null,(Object)header)==1);
        number(header,4,2);number(header,8,704);check((int)validate.invoke(null,(Object)header)==2);
        for(int position:new int[]{0,4,8,12,16}) {
            byte[] bad=header.clone();number(bad,position,-1);boolean rejected=false;
            try {validate.invoke(null,(Object)bad);}catch(InvocationTargetException e){rejected=e.getCause() instanceof java.io.IOException;}
            check(rejected);
        }
        byte[] emptyPcm=header.clone();number(emptyPcm,8,0);boolean emptyRejected=false;
        try {validate.invoke(null,(Object)emptyPcm);}catch(InvocationTargetException e){emptyRejected=e.getCause() instanceof java.io.IOException;}
        check(emptyRejected);
        byte[] on=(byte[])command.invoke(null,3);check(on.length==20&&on[4]==3&&Arrays.equals(Arrays.copyOfRange(on,8,20),new byte[12]));
        check((boolean)source.invoke(null,1)&&(boolean)source.invoke(null,6)&&(boolean)source.invoke(null,7));
        check((boolean)source.invoke(null,5) && (boolean)source.invoke(null,9));
        check(!(boolean)source.invoke(null,8) && !(boolean)source.invoke(null,4));
        Class<?> demandClass=Class.forName("com.carplaybox.audio.CarMicrophoneBridge$RecordingDemand");
        Constructor<?> constructor=demandClass.getDeclaredConstructor();constructor.setAccessible(true);
        Object demand=constructor.newInstance();
        Method update=demandClass.getDeclaredMethod("update",boolean.class,long.class);update.setAccessible(true);
        Method needed=demandClass.getDeclaredMethod("needed",long.class);needed.setAccessible(true);
        Method clear=demandClass.getDeclaredMethod("clear");clear.setAccessible(true);
        check(!(boolean)needed.invoke(demand,1000L));
        update.invoke(demand,true,1000L);check((boolean)needed.invoke(demand,10000L));
        update.invoke(demand,false,10000L);check((boolean)needed.invoke(demand,10249L));
        update.invoke(demand,false,10200L);check(!(boolean)needed.invoke(demand,10250L));
        update.invoke(demand,true,11000L);check((boolean)needed.invoke(demand,20000L));
        clear.invoke(demand);check(!(boolean)needed.invoke(demand,20000L));
        System.out.println("Android microphone framing and real recording source selection passed");
    }
}
