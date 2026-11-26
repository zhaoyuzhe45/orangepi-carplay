package com.carplaybox.audio;

import android.app.Instrumentation;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import java.util.*;

/** Fake backend is local to this test panel. No fake call is ever sent to HFP. */
public final class PhoneUiDeviceTest extends Instrumentation {
    private PhoneActivity activity;
    private PhonePanel panel;
    private final Fake backend=new Fake();
    private int checks;
    private static final String UUID="01234567-89ab-cdef-0123-456789abcdef";
    static final class Fake implements PhoneBackend {
        PhoneState snapshot;
        final List<String> commands=new ArrayList<>();
        public PhoneState state() { return snapshot; }
        public String command(String a,String d,String v) { commands.add(a+":"+d+":"+v);return "fixture accepted"; }
        public String connect(String d) { commands.add("connect");return "fixture connect"; }
        public String tone(String d,String id,String v) { commands.add("tone:"+id+":"+v);return "fixture tone"; }
    }
    @Override public void onCreate(Bundle args) { super.onCreate(args);start(); }
    private void check(boolean value,String why) { checks++;if(!value)throw new AssertionError(why); }
    private void state(boolean known,int status) {
        List<BluetoothCalls.Call> calls=status<0?Collections.emptyList():Arrays.asList(new BluetoothCalls.Call(UUID,status,false,"123 4567 8900"));
        backend.snapshot=new PhoneState("fixture", "fixture-device",Collections.emptyList(),calls,known,known,0);
    }
    private void key(int code) { activity.dispatchKeyEvent(new KeyEvent(0,code));activity.dispatchKeyEvent(new KeyEvent(1,code)); }
    private View tag(String tag) { return panel.root.findViewWithTag(tag); }
    private String number() { return ((TextView)tag("dial-number")).getText().toString(); }
    private void screenshot(String name,View view,int width,int height) throws Exception {
        view.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(height,View.MeasureSpec.EXACTLY));
        view.layout(0,0,width,height);Bitmap b=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);view.draw(new Canvas(b));
        try(java.io.FileOutputStream f=getTargetContext().openFileOutput(name,0)) { b.compress(Bitmap.CompressFormat.PNG,100,f); }b.recycle();
    }
    @Override public void onStart() {
        Bundle result=new Bundle();
        try {
            activity=(PhoneActivity)startActivitySync(new Intent(getTargetContext(),PhoneActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            runOnMainSync(()-> {
                try {
                    int full=View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
                    check((activity.getWindow().getDecorView().getSystemUiVisibility()&full)==full,"launch hides both system bars");
                    activity.getWindow().getDecorView().setSystemUiVisibility(0);
                    activity.onWindowFocusChanged(true);
                    check((activity.getWindow().getDecorView().getSystemUiVisibility()&full)==full,"returning from system window restores fullscreen");
                    check(activity.findViewById(android.R.id.content).findViewWithTag("tab-dialer")!=null,"default contacts tab");
                    check(activity.findViewById(android.R.id.content).findViewWithTag("pair-phone")!=null,"contacts offers pairing without switching tabs");
                    java.lang.reflect.Field cp=PhoneActivity.class.getDeclaredField("contacts");cp.setAccessible(true);
                    ContactsPanel contacts=(ContactsPanel)cp.get(activity);contacts.stop();
                    java.lang.reflect.Field items=ContactsPanel.class.getDeclaredField("contacts");items.setAccessible(true);
                    List<PhoneContacts.Entry> samples=new ArrayList<>();
                    for(int i=1;i<=8;i++)samples.add(new PhoneContacts.Entry("测试联系人 0"+i,"1234567000"+i));items.set(contacts,samples);
                    java.lang.reflect.Method render=ContactsPanel.class.getDeclaredMethod("render");render.setAccessible(true);render.invoke(contacts);
                    check(((Button)contacts.root.findViewWithTag("contact-0")).getText().toString().contains("01"),"contact rows render names");
                    findText(contacts.root,"下一页").performClick();
                    check(((Button)contacts.root.findViewWithTag("contact-0")).getText().toString().contains("06"),"contact rotary page");
                    ((android.widget.EditText)contacts.root.findViewWithTag("contact-search")).setText("03");
                    check(((Button)contacts.root.findViewWithTag("contact-0")).getText().toString().contains("03"),"contact UI search");
                    ((android.widget.EditText)contacts.root.findViewWithTag("contact-search")).setText("");
                    screenshot("phone-contacts-fixture.png",contacts.root,Ui.dp(activity,800),Ui.dp(activity,480));
                    activity.showDialer(null);
                    java.lang.reflect.Field field=PhoneActivity.class.getDeclaredField("panel");field.setAccessible(true);
                    ((PhonePanel)field.get(activity)).stop();state(true,-1);
                    panel=new PhonePanel(activity,backend);field.set(activity,panel);activity.setContentView(panel.root);panel.refresh();
                    check(panel.root.findViewWithTag("pair-phone")!=null,"dialer offers pairing");
                }catch(Exception e) { throw new RuntimeException(e); }
            });waitForIdleSync();
            runOnMainSync(()-> {
                tag("digit-1").requestFocus();
                for(int i=1;i<=9;i++) {
                    check(activity.getCurrentFocus()==tag("digit-"+i),"rotation reaches digit "+i);
                    key(KeyEvent.KEYCODE_DPAD_CENTER);key(KeyEvent.KEYCODE_DPAD_RIGHT);
                }
                check(activity.getCurrentFocus()==tag("digit-*"),"rotation reaches plus");key(KeyEvent.KEYCODE_DPAD_RIGHT);
                check(activity.getCurrentFocus()==tag("digit-0"),"rotation reaches zero");key(KeyEvent.KEYCODE_DPAD_CENTER);
                check(number().equals("1234567890"),"rotary-only full number");
                key(KeyEvent.KEYCODE_DPAD_LEFT);key(KeyEvent.KEYCODE_DPAD_LEFT);check(activity.getCurrentFocus()==tag("digit-9"),"reverse rotation");
                tag("digit-2").requestFocus();key(KeyEvent.KEYCODE_DPAD_DOWN);check(activity.getCurrentFocus()==tag("digit-5"),"down same column");
                key(KeyEvent.KEYCODE_DPAD_UP);check(activity.getCurrentFocus()==tag("digit-2"),"up same column");
                tag("digit-1").requestFocus();long now=SystemClock.uptimeMillis();
                activity.dispatchKeyEvent(new KeyEvent(now,now,0,23,0));
                activity.dispatchKeyEvent(new KeyEvent(now,now+50,0,23,1));
                activity.dispatchKeyEvent(new KeyEvent(now,now+100,0,23,2));
                activity.dispatchKeyEvent(new KeyEvent(now,now+150,1,23,0));
                check(number().equals("12345678901"),"held confirm only clicks once");
                check(backend.commands.isEmpty(),"number entry never dials");
                tag("dial").performClick();
            });waitForIdleSync();
            runOnMainSync(()-> {
                try {
                    java.lang.reflect.Field f=PhonePanel.class.getDeclaredField("dialog");f.setAccessible(true);PhoneDialog d=(PhoneDialog)f.get(panel);
                    check(d.isShowing(),"dial confirmation visible");
                    check(((Button)d.getCurrentFocus()).getText().equals("取消"),"cancel initially focused");
                    d.dispatchKeyEvent(new KeyEvent(0,23));d.dispatchKeyEvent(new KeyEvent(1,23));
                    check(backend.commands.isEmpty(),"cancel never reaches backend");
                    tag("dial").performClick();d=(PhoneDialog)f.get(panel);
                    d.dispatchKeyEvent(new KeyEvent(0,22));d.dispatchKeyEvent(new KeyEvent(1,22));
                    d.dispatchKeyEvent(new KeyEvent(0,23));d.dispatchKeyEvent(new KeyEvent(1,23));
                    check(backend.commands.size()==1 && backend.commands.get(0).equals("dial:fixture-device:12345678901"),"confirmed dial exact target");
                    state(true,4);panel.refresh();
                    check(tag("call-answer")!=null && tag("call-reject")!=null,"incoming actions");
                    tag("call-answer").requestFocus();panel.refresh();check(activity.getCurrentFocus()==tag("call-answer"),"poll preserves focus");
                    key(KeyEvent.KEYCODE_DPAD_CENTER);check(backend.commands.get(1).equals("answer:fixture-device:"+UUID),"incoming target UUID");
                    state(false,4);panel.refresh();check(tag("call-answer")==null,"lost connection removes stale action");
                    check(!tag("dial").isEnabled(),"lost connection disables dial");
                    state(true,0);panel.refresh();check(tag("call-hangup")!=null,"active hangup");
                    View tone=findText(panel.root,"通话键盘");check(tone!=null && tone.isEnabled(),"active keypad available");tone.performClick();
                    tag("digit-#").performClick();check(backend.commands.get(2).equals("tone:"+UUID+":#"),"DTMF exact target");
                    state(false,-1);panel.refresh();check(((Button)tag("digit-#")).getText().equals("删除"),"disconnect exits tone mode");
                    state(true,-1);panel.refresh();
                    int w=activity.getResources().getDisplayMetrics().widthPixels,h=activity.getResources().getDisplayMetrics().heightPixels;
                    screenshot("phone-ui-native.png",panel.root,w,h);
                    // Render an 800x480 dp viewport without changing display size/density.
                    PhonePanel small=new PhonePanel(activity,backend);small.refresh();
                    screenshot("phone-ui-800x480.png",small.root,Ui.dp(activity,800),Ui.dp(activity,480));
                    state(true,4);PhoneCallCard card=new PhoneCallCard(activity,backend,s->{},()->{});card.render(backend.state());
                    screenshot("phone-incoming.png",card.root,Ui.dp(activity,560),Ui.dp(activity,210));
                    check(backend.commands.size()==3,"only three explicit fake commands");
                    android.view.ViewParent parent=tag("dial").getParent();
                    while(parent!=null) { check(!(parent instanceof android.widget.ScrollView),"dialpad must never scroll");parent=parent.getParent(); }
                    for(String target:new String[]{"dial","digit-1","digit-0","digit-#"}) {
                        View v=tag(target);android.graphics.Rect rect=new android.graphics.Rect(0,0,v.getWidth(),v.getHeight());
                        panel.root.offsetDescendantRectToMyCoords(v,rect);
                        check(rect.top>=0 && rect.bottom<=panel.root.getHeight() && rect.left>=0 && rect.right<=panel.root.getWidth(),"fully visible "+target);
                    }
                    activity.showContacts();activity.showDialer(null);panel.stop();
                    check(number().equals("12345678901"),"tab switching retains number");
                }catch(Exception e) { throw new RuntimeException(e); }
            });
            result.putString("result","PASS "+checks+" real Android UI checks; only isolated fake backend commands");finish(0,result);
        } catch(Throwable t) { result.putString("error",t.toString());finish(-1,result); }
        finally { if(activity!=null)runOnMainSync(()->activity.finish()); }
    }
    private View findText(View view,String label) {
        if(view instanceof Button && ((Button)view).getText().toString().equals(label))return view;
        if(view instanceof ViewGroup) { ViewGroup g=(ViewGroup)view;for(int i=0;i<g.getChildCount();i++) { View r=findText(g.getChildAt(i),label);if(r!=null)return r; } }
        return null;
    }
}
