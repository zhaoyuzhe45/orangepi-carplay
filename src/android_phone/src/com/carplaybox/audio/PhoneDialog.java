package com.carplaybox.audio;

import android.app.Activity;
import android.app.Dialog;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;

/** All modal choices, including cancel, are reachable with the car's rotary keys. */
final class PhoneDialog extends Dialog {
    final PhoneNavigation navigation=new PhoneNavigation();
    final LinearLayout body;
    private final Activity activity;
    PhoneDialog(Activity a,String title,String message) {
        super(a);activity=a;requestWindowFeature(Window.FEATURE_NO_TITLE);
        body=PhoneStyle.card(a);body.addView(PhoneStyle.text(a,title,24,PhoneStyle.TEXT));
        if(!message.isEmpty())body.addView(PhoneStyle.text(a,message,17,PhoneStyle.MUTED));
        ScrollView scroll=new ScrollView(a);scroll.setFillViewport(true);scroll.addView(body);setContentView(scroll);
        getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        setCanceledOnTouchOutside(false);
    }
    Button option(String text,int color,Runnable action) {
        Button b=PhoneStyle.button(activity,text,color,v->{dismiss();action.run();});
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,PhoneStyle.dp(activity,52));
        p.topMargin=PhoneStyle.dp(activity,8);body.addView(b,p);navigation.add(b);return b;
    }
    @Override public void show() {
        PhoneStyle.immersive(getWindow());super.show();PhoneStyle.immersive(getWindow());getWindow().setLayout(Math.min(activity.getResources().getDisplayMetrics().widthPixels-PhoneStyle.dp(activity,32),PhoneStyle.dp(activity,560)),-2);
        navigation.move(1);
    }
    @Override public void onWindowFocusChanged(boolean focused) { super.onWindowFocusChanged(focused);if(focused)PhoneStyle.immersive(getWindow()); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if(e.getKeyCode()==KeyEvent.KEYCODE_ESCAPE) { if(e.getAction()==KeyEvent.ACTION_UP)dismiss();return true; }
        return navigation.key(e)||super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return navigation.motion(e)||super.dispatchGenericMotionEvent(e); }
    @Override public void dismiss() { navigation.cancel();super.dismiss(); }
}
