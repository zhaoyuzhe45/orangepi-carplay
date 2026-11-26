package com.carplaybox.audio;

import android.app.Activity;
import android.os.SystemClock;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Rebuild actions only on a target/state change, binding every button to that snapshot. */
final class PhoneCallCard {
    final LinearLayout root;
    private final Activity activity;
    private final PhoneBackend backend;
    private final TextView heading,number,detail;
    private final LinearLayout actions;
    private final java.util.function.Consumer<String> result;
    private final Runnable changed;
    private String key="";
    PhoneCallCard(Activity a,PhoneBackend b,java.util.function.Consumer<String> output,Runnable changed) {
        activity=a;backend=b;result=output;this.changed=changed;root=PhoneStyle.card(a);
        heading=PhoneStyle.text(a,"准备就绪",17,PhoneStyle.GREEN);root.addView(heading);
        number=PhoneStyle.text(a,"手机蓝牙电话",30,PhoneStyle.TEXT);number.setMaxLines(2);
        number.setAutoSizeTextTypeUniformWithConfiguration(18,30,1,android.util.TypedValue.COMPLEX_UNIT_SP);root.addView(number,Ui.matchWrap());
        detail=PhoneStyle.text(a,"连接手机后，即可在这里查看通话",14,PhoneStyle.MUTED);root.addView(detail);
        actions=Ui.horizontal(a);root.addView(actions,Ui.matchWrap());
    }
    void render(PhoneState s) {
        boolean known=s!=null && PhoneUiModel.fresh(s.connected,s.knownCalls,s.sampled,SystemClock.uptimeMillis());
        BluetoothCalls.Call c=known && s.calls.size()==1?s.calls.get(0):null;
        String next=(s==null?"":s.selected)+"/"+known+"/"+(c==null?(known?s.calls.size():"unknown"):c.uuid+"/"+c.state+"/"+c.number);
        if(next.equals(key))return;key=next;
        heading.setText(!known?"等待手机连接":c==null?(s.calls.isEmpty()?"没有正在进行的通话":"多路通话"):c.label());
        number.setText(c==null?"手机蓝牙电话":c.number.isEmpty()?"未知号码":c.number);
        detail.setText(!known?"通话状态不可用，操作已暂停":c==null?(s.calls.isEmpty()?"使用右侧键盘输入号码":"请在手机上管理多路通话"):
            "通话声音请使用手机听筒或扬声器");
        actions.removeAllViews();
        if(c!=null) {
            final String device=s.selected,uuid=c.uuid;
            for(String action:new String[]{"answer","reject","hangup"})if(BluetoothCalls.allowed(action,uuid,s.calls)) {
                String label=action.equals("answer")?"接听":action.equals("reject")?"拒接":"挂断";
                Button b=PhoneStyle.button(activity,label,action.equals("answer")?PhoneStyle.GREEN:PhoneStyle.RED,
                    v->result.accept(backend.command(action,device,uuid)));
                b.setTag("call-"+action);actions.addView(b,PhoneStyle.row(activity));
            }
        }
        changed.run();
    }
    void addNavigation(PhoneNavigation navigation) {
        for(int i=0;i<actions.getChildCount();i++)navigation.add(actions.getChildAt(i));
    }
}
