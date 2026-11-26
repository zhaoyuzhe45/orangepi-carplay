package com.carplaybox.audio;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

final class PhonePanel {
    final LinearLayout root;
    private final Activity activity;
    private final PhoneBackend backend;
    private final TextView deviceStatus,number,message,padTitle;
    private final PhoneCallCard calls;
    private final List<View> fixed=new ArrayList<>();
    private final List<Button> digits=new ArrayList<>();
    private final Handler handler=new Handler();
    private final Button dial,plus,erase,clear,tones,disconnect;
    private PhoneNavigation navigation=new PhoneNavigation();
    private PhoneDialog dialog;
    private String entered="",toneDevice="",toneId="";
    private boolean active,toneMode;
    PhonePanel(Activity a,PhoneBackend b) {
        activity=a;backend=b;root=Ui.vertical(a);root.setBackgroundColor(PhoneStyle.BG);
        root.setPadding(PhoneStyle.dp(a,20),PhoneStyle.dp(a,12),PhoneStyle.dp(a,20),PhoneStyle.dp(a,8));
        LinearLayout header=Ui.horizontal(a);header.addView(PhoneStyle.text(a,"电话",28,PhoneStyle.TEXT),Ui.weight());
        header.addView(button("通讯录",PhoneStyle.KEY,v->((PhoneActivity)a).showContacts()),new LinearLayout.LayoutParams(PhoneStyle.dp(a,104),PhoneStyle.dp(a,48)));
        Button close=button("关闭",PhoneStyle.KEY,v->a.finish());header.addView(close,new LinearLayout.LayoutParams(PhoneStyle.dp(a,84),PhoneStyle.dp(a,48)));
        root.addView(header,Ui.matchWrap());
        LinearLayout columns=Ui.horizontal(a);columns.setGravity(Gravity.TOP);
        LinearLayout left=Ui.vertical(a);deviceStatus=PhoneStyle.text(a,"正在读取手机状态",15,PhoneStyle.MUTED);left.addView(deviceStatus,Ui.matchWrap());
        message=PhoneStyle.text(a,"旋转选择 · 按下确认",14,PhoneStyle.GREEN);message.setMinHeight(PhoneStyle.dp(a,42));
        calls=new PhoneCallCard(a,b,this::message,this::rebuildNavigation);left.addView(calls.root,Ui.matchWrap());
        tones=button("通话键盘",PhoneStyle.KEY,v->toggleTones());left.addView(tones,block(a));
        LinearLayout connection=Ui.horizontal(a);
        connection.addView(button("连接手机",PhoneStyle.KEY,v->chooseDevice()),PhoneStyle.row(a));
        disconnect=button("断开",PhoneStyle.KEY,v->{PhoneState s=b.state();if(s!=null)message(b.command("disconnect",s.selected,""));});
        connection.addView(disconnect,PhoneStyle.row(a));left.addView(connection,Ui.matchWrap());
        LinearLayout pairing=Ui.horizontal(a);
        Button pair=button("配对手机",PhoneStyle.KEY,v->((PhoneActivity)a).pairPhone());pair.setTag("pair-phone");pairing.addView(pair,PhoneStyle.row(a));
        pairing.addView(button("蓝牙设置",PhoneStyle.KEY,v->{try { a.startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)); }catch(RuntimeException e) { message("无法打开系统蓝牙设置"); }}),PhoneStyle.row(a));
        left.addView(pairing,Ui.matchWrap());
        left.addView(message,Ui.matchWrap());
        ScrollView leftScroll=new ScrollView(a);leftScroll.setFillViewport(true);leftScroll.addView(left);
        columns.addView(leftScroll,new LinearLayout.LayoutParams(0,-1,1));
        LinearLayout right=PhoneStyle.card(a);padTitle=PhoneStyle.text(a,"拨号键盘",14,PhoneStyle.MUTED);right.addView(padTitle);
        number=PhoneStyle.text(a,"输入电话号码",30,PhoneStyle.TEXT);number.setGravity(Gravity.CENTER_VERTICAL);
        number.setSingleLine(true);number.setAutoSizeTextTypeUniformWithConfiguration(16,30,1,android.util.TypedValue.COMPLEX_UNIT_SP);
        number.setTag("dial-number");LinearLayout numberRow=Ui.horizontal(a);
        numberRow.addView(number,new LinearLayout.LayoutParams(0,PhoneStyle.dp(a,52),1));
        clear=button("清空",PhoneStyle.KEY,v->{entered="";showNumber();});
        numberRow.addView(clear,new LinearLayout.LayoutParams(PhoneStyle.dp(a,64),PhoneStyle.dp(a,48)));right.addView(numberRow,Ui.matchWrap());
        String[] labels={"1","2","3","4","5","6","7","8","9","*","0","#"};
        for(int row=0;row<4;row++) {
            LinearLayout line=Ui.horizontal(a);
            for(int col=0;col<3;col++) {
                final String label=labels[row*3+col];Button key=button(label,PhoneStyle.KEY,v->digit(label.charAt(0)));
                key.setId(View.generateViewId());key.setTag("digit-"+label);key.setTextSize(25);digits.add(key);
                LinearLayout.LayoutParams kp=PhoneStyle.row(a);kp.height=-1;line.addView(key,kp);
            }
            right.addView(line,new LinearLayout.LayoutParams(-1,0,1));
        }
        for(int i=0;i<12;i++) {
            if(i>=3)digits.get(i).setNextFocusUpId(digits.get(i-3).getId());
            if(i<9)digits.get(i).setNextFocusDownId(digits.get(i+3).getId());
        }
        plus=digits.get(9);erase=digits.get(11);
        dial=button("拨打电话",PhoneStyle.GREEN,v->confirmDial());dial.setTag("dial");right.addView(dial,block(a));
        LinearLayout.LayoutParams rp=new LinearLayout.LayoutParams(0,-1,1.16f);rp.leftMargin=PhoneStyle.dp(a,20);columns.addView(right,rp);
        root.addView(columns,new LinearLayout.LayoutParams(-1,0,1));
        root.addView(PhoneStyle.text(a,"旋转：前后选择    方向键：移动    按下：确认    返回：关闭",12,PhoneStyle.MUTED));
        rebuildNavigation();
    }
    private static LinearLayout.LayoutParams block(Activity a) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,PhoneStyle.dp(a,50));p.topMargin=PhoneStyle.dp(a,8);return p;
    }
    private Button button(String label,int color,View.OnClickListener action) {
        Button b=PhoneStyle.button(activity,label,color,action);fixed.add(b);return b;
    }
    private void rebuildNavigation() {
        navigation.cancel();navigation=new PhoneNavigation();
        // Header then left card/actions, then keypad in reading order. Stable across polls.
        for(int i=0;i<Math.min(2,fixed.size());i++)navigation.add(fixed.get(i));
        if(calls!=null)calls.addNavigation(navigation);
        for(int i=2;i<fixed.size();i++)navigation.add(fixed.get(i));
    }
    void start() { active=true;refresh();handler.removeCallbacks(poll);handler.postDelayed(poll,500); }
    void stop() { active=false;handler.removeCallbacksAndMessages(null);navigation.cancel();if(dialog!=null)dialog.dismiss(); }
    private final Runnable poll=new Runnable() { public void run() { if(active) { refresh();handler.postDelayed(this,500); } } };
    void save(Bundle state) { state.putString("phone-number",entered); }
    void setNumber(String value) { String n=BluetoothCalls.number(value);if(n!=null) { entered=n;toneMode=false;showNumber(); } }
    void restore(Bundle state) { if(state!=null) { entered=state.getString("phone-number","");showNumber(); } }
    void message(String value) { message.setText(value); }
    private void showNumber() { number.setText(toneMode?"按数字发送按键音":entered.isEmpty()?"输入电话号码":entered); }
    private void erase() { if(!entered.isEmpty())entered=entered.substring(0,entered.length()-1);showNumber(); }
    private void digit(char value) {
        if(toneMode) { message(backend.tone(toneDevice,toneId,String.valueOf(value)));return; }
        if(value=='*')value='+';
        if(value=='#') { erase();return; }
        entered=PhoneUiModel.append(entered,value);showNumber();
    }
    private void toggleTones() {
        PhoneState s=backend.state();
        if(toneMode) { toneMode=false;toneId="";showNumber();refresh();return; }
        if(!fresh(s) || s.calls.size()!=1 || s.calls.get(0).state!=0) { message("请在单路通话接通后使用");return; }
        toneMode=true;toneId=s.calls.get(0).uuid;toneDevice=s.selected;showNumber();refresh();
    }
    private static boolean fresh(PhoneState s) {
        return s!=null && PhoneUiModel.fresh(s.connected,s.knownCalls,s.sampled,SystemClock.uptimeMillis());
    }
    void refresh() {
        String response=backend.consumeMessage();if(response!=null)message(response);
        PhoneState s=backend.state();boolean known=fresh(s);
        String name=s!=null && !s.selected.isEmpty()?"已配对手机":"未选择手机";
        if(s!=null)for(PhoneState.Device p:s.phones)if(p.address.equals(s.selected))name=p.name;
        deviceStatus.setText(name+"  ·  "+(known?"已连接":s!=null && s.connected?"等待通话状态":"未连接"));
        calls.render(s);
        boolean canTone=known && s.calls.size()==1 && s.calls.get(0).state==0;
        if(toneMode && (!canTone || !s.selected.equals(toneDevice) || !s.calls.get(0).uuid.equals(toneId))) {
            toneMode=false;toneId="";message("通话状态已变化，已退出通话键盘");showNumber();
        }
        tones.setText(toneMode?"返回拨号键盘":"通话键盘");PhoneStyle.enabled(tones,canTone);
        PhoneStyle.enabled(disconnect,s!=null && !s.selected.isEmpty());
        PhoneStyle.enabled(dial,known && s.calls.isEmpty() && !toneMode);
        plus.setText(toneMode?"*":"+");erase.setText(toneMode?"#":"删除");erase.setTextSize(toneMode?25:17);
        PhoneStyle.enabled(clear,!toneMode);
        padTitle.setText(toneMode?"通话键盘 · 逐键发送":"拨号键盘");
    }
    private void confirmDial() {
        String normalized=BluetoothCalls.number(entered);PhoneState s=backend.state();
        if(normalized==null) { message("请输入至少三位的普通电话号码");return; }
        if(!fresh(s) || !s.calls.isEmpty()) { message("请先连接手机，并结束其他通话");return; }
        final String target=s.selected;
        dialog=new PhoneDialog(activity,"确认拨打",normalized+"\n通话声音请使用手机听筒或扬声器");
        dialog.option("取消",PhoneStyle.KEY,()->{});
        dialog.option("拨打",PhoneStyle.GREEN,()->message(backend.command("dial",target,normalized)));dialog.show();
    }
    private void chooseDevice() {
        PhoneState s=backend.state();dialog=new PhoneDialog(activity,"连接手机","选择已配对的手机，连接后将自动重连");
        dialog.option("取消",PhoneStyle.KEY,()->{});
        if(s!=null)for(PhoneState.Device p:s.phones)dialog.option(p.name,PhoneStyle.KEY,()->message(backend.connect(p.address)));
        if(s==null || s.phones.isEmpty())dialog.body.addView(PhoneStyle.text(activity,"暂无已配对手机，请先打开蓝牙设置",16,PhoneStyle.MUTED));
        dialog.show();
    }
    boolean key(KeyEvent e) {
        int k=e.getKeyCode();
        if(k==KeyEvent.KEYCODE_DEL) { if(e.getAction()==KeyEvent.ACTION_DOWN)erase();return true; }
        int c=e.getUnicodeChar();
        if((c>='0' && c<='9') || c=='+' || c=='*' || c=='#') {
            if(e.getAction()==KeyEvent.ACTION_DOWN && e.getRepeatCount()==0)digit((char)c);return true;
        }
        return navigation.key(e);
    }
    boolean motion(MotionEvent e) { return navigation.motion(e); }
}
