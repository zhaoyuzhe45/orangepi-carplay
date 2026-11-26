import java.lang.reflect.*;
public final class AudioPolicyApiProbe {
 public static void main(String[] args)throws Exception {
  for(String name:new String[]{"android.media.audiopolicy.AudioMixingRule","android.media.audiopolicy.AudioMixingRule$Builder","android.media.audiopolicy.AudioPolicy","android.media.AudioRecord"}){
   Class<?> c=Class.forName(name);System.out.println(name);
   for(Field f:c.getDeclaredFields())if(f.getName().contains("RULE")||f.getName().contains("ROUTE")){f.setAccessible(true);System.out.println(f.getName()+"="+f.get(null));}
   for(Method m:c.getDeclaredMethods())if(m.getName().contains("MixRule")||m.getName().contains("RecordSink")||m.getName().equals("read"))System.out.println(m);
  }
 }
}
