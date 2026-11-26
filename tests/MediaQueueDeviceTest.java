package com.carplaybox.audio;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.widget.ListView;
public final class MediaQueueDeviceTest extends Instrumentation {
 private Intent fixture(){return new Intent().setClassName("com.carplaybox.appfixture","com.carplaybox.appfixture.AppMediaFixture");}
 @Override public void onCreate(Bundle b){super.onCreate(b);start();}
 static void check(boolean b,String why){if(!b)throw new AssertionError(why);}
 private AppIntegrationService await(int count,String title){
  for(int i=0;i<40;i++){AppIntegrationService s=AppIntegrationService.current();AppSnapshot p=s==null?null:s.media();if(p!=null && "com.carplaybox.appfixture".equals(p.packageName) && s.queue().count()==count && (title==null || title.equals(p.title)))return s;SystemClock.sleep(150);}
  throw new AssertionError("player state not reached: "+count+" "+title);
 }
 @Override public void onStart(){Bundle out=new Bundle();MediaQueueActivity activity=null;
  try{
   getTargetContext().startForegroundService(new Intent(getTargetContext(),AppIntegrationService.class));
   getTargetContext().startForegroundService(fixture().putExtra("count",3).putExtra("reverse",false).putExtra("reset",true));
   AppIntegrationService s=await(3,"Fixture歌曲1");long v=s.queue().version();
   check(s.queueSelect(v,1),"selection not queued");await(3,"Fixture歌曲2");
   check(s.command(1,127),"pause not queued");
   for(int i=0;i<30 && s.media().status!=2;i++)SystemClock.sleep(100);
   check(s.media().status==2,"pause state missing");
   check(s.queueSelect(v,2),"select while paused rejected");await(3,"Fixture歌曲3");
   check(s.media().status==1,"select did not resume playback");
   getTargetContext().startForegroundService(fixture().putExtra("reverse",true));
   for(int i=0;i<30 && s.queue().version()==v;i++)SystemClock.sleep(100);
   check(s.queue().version()!=v && !s.queueSelect(v,0),"old generation accepted");
   check(!s.queueSelect(s.queue().version(),3),"out of range accepted");
   check(s.queueSelect(s.queue().version(),2),"new generation rejected");await(3,"Fixture歌曲1");
   getTargetContext().startForegroundService(fixture().putExtra("count",240).putExtra("reverse",false));await(240,null);
   activity=(MediaQueueActivity)startActivitySync(new Intent(getTargetContext(),MediaQueueActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));SystemClock.sleep(1300);
   java.lang.reflect.Field f=MediaQueueActivity.class.getDeclaredField("list");f.setAccessible(true);ListView list=(ListView)f.get(activity);
   check(list.getAdapter().getCount()==240,"full queue not shown");
   runOnMainSync(()->{list.requestFocus();list.setSelection(238);});waitForIdleSync();
   sendKeyDownUpSync(KeyEvent.KEYCODE_DPAD_RIGHT);waitForIdleSync();SystemClock.sleep(100);
   check(list.getSelectedItemPosition()==239,"rotary right failed selected="+list.getSelectedItemPosition()+" focus="+activity.getCurrentFocus());
   sendKeyDownUpSync(KeyEvent.KEYCODE_DPAD_CENTER);await(240,"Fixture歌曲240");
   check((activity.getWindow().getDecorView().getSystemUiVisibility()&0x1006)==0x1006,"not immersive");
   out.putString("result","PASS real queue IDs, stale version rejection, 240-item list, rotary selection of final item, immersive window");finish(0,out);
  }catch(Throwable e){out.putString("error",android.util.Log.getStackTraceString(e));finish(-1,out);}
 }
}
