package com.carplaybox.audio;

import android.app.Activity;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Native drawable states keep touch and rotary feedback identical. */
final class PhoneStyle {
    static final int BG=0xff0c1420, CARD=0xff152332, KEY=0xff203347,
        TEXT=0xfff2f6fa, MUTED=0xffafc1d2, GREEN=0xff79e1c2, RED=0xffff9b9e, FOCUS=0xffb2f4e1;
    static int dp(Activity a,int n) { return Ui.dp(a,n); }
    static void immersive(android.view.Window window) {
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_STABLE|
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    static GradientDrawable shape(Activity a,int color,int stroke) {
        GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(a,16));
        if(stroke!=0)d.setStroke(dp(a,3),stroke);return d;
    }
    static Button button(Activity a,String label,int fill,View.OnClickListener action) {
        Button b=new Button(a);b.setText(label);b.setTextSize(16);b.setAllCaps(false);
        b.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        b.setTextColor(fill==GREEN || fill==RED?BG:TEXT);
        b.setMinWidth(0);b.setMinimumWidth(0);b.setMinHeight(dp(a,48));b.setMinimumHeight(dp(a,48));
        b.setPadding(dp(a,10),dp(a,3),dp(a,10),dp(a,3));
        b.setFocusable(true);b.setFocusableInTouchMode(true);b.setStateListAnimator(null);
        StateListDrawable states=new StateListDrawable();
        states.addState(new int[]{-android.R.attr.state_enabled},shape(a,0xff28313c,0));
        states.addState(new int[]{android.R.attr.state_pressed},shape(a,0xff476779,FOCUS));
        states.addState(new int[]{android.R.attr.state_focused},shape(a,fill,FOCUS));
        states.addState(new int[]{},shape(a,fill,0));b.setBackground(states);
        b.setOnClickListener(action);return b;
    }
    static TextView text(Activity a,String text,int size,int color) {
        TextView v=Ui.text(a,text,size,color);v.setFontFeatureSettings("tnum");return v;
    }
    static LinearLayout card(Activity a) {
        LinearLayout v=Ui.vertical(a);v.setBackground(shape(a,CARD,0));
        v.setPadding(dp(a,16),dp(a,12),dp(a,16),dp(a,12));return v;
    }
    static LinearLayout.LayoutParams row(Activity a) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(a,50),1);
        p.setMargins(dp(a,4),dp(a,4),dp(a,4),dp(a,4));return p;
    }
    static void enabled(View v,boolean value) { v.setEnabled(value);v.setAlpha(value?1f:.45f); }
}
