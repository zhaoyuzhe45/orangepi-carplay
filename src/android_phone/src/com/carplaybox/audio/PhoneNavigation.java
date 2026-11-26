package com.carplaybox.audio;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

/** This car sends LEFT/RIGHT for rotation; UP/DOWN retain spatial navigation. */
final class PhoneNavigation {
    private final List<View> order=new ArrayList<>();
    private View pressed;
    private int pressedKey=-1;
    private float wheel;
    void add(View v) { order.add(v); }
    void cancel() { if(pressed!=null)pressed.setPressed(false);pressed=null;pressedKey=-1; }
    private boolean usable(View v) { return v.isShown() && v.isEnabled() && v.isFocusable(); }
    private View focused() { for(View v:order)if(v.hasFocus())return v;return null; }
    void move(int direction) {
        cancel();int i=order.indexOf(focused());
        if(i<0)i=direction>0?-1:0;
        for(int n=0;n<order.size();n++) {
            i=(i+direction+order.size())%order.size();View v=order.get(i);
            if(usable(v)) { v.requestFocus();v.requestRectangleOnScreen(new android.graphics.Rect(0,0,v.getWidth(),v.getHeight()));return; }
        }
    }
    boolean key(KeyEvent e) {
        int k=e.getKeyCode();
        if(k==KeyEvent.KEYCODE_DPAD_UP || k==KeyEvent.KEYCODE_DPAD_DOWN) {
            if(e.getAction()==KeyEvent.ACTION_DOWN) {
                cancel();View current=focused();
                View next=current==null?null:current.focusSearch(k==KeyEvent.KEYCODE_DPAD_UP?View.FOCUS_UP:View.FOCUS_DOWN);
                if(next!=null && order.contains(next) && usable(next))next.requestFocus();
                else move(k==KeyEvent.KEYCODE_DPAD_UP?-1:1);
            }
            return true;
        }
        int direction=k==KeyEvent.KEYCODE_DPAD_RIGHT?1:
            k==KeyEvent.KEYCODE_DPAD_LEFT?-1:
            k==KeyEvent.KEYCODE_TAB?(e.isShiftPressed()?-1:1):0;
        if(direction!=0) { if(e.getAction()==KeyEvent.ACTION_DOWN)move(direction);return true; }
        if(k!=KeyEvent.KEYCODE_DPAD_CENTER && k!=KeyEvent.KEYCODE_ENTER && k!=KeyEvent.KEYCODE_NUMPAD_ENTER)return false;
        if(e.getAction()==KeyEvent.ACTION_DOWN && e.getRepeatCount()==0) {
            cancel();pressed=focused();pressedKey=k;
            if(pressed!=null && usable(pressed))pressed.setPressed(true);
        } else if(e.getAction()==KeyEvent.ACTION_UP) {
            View v=pressed;boolean click=k==pressedKey && !e.isCanceled() && v!=null && v==focused() && usable(v);
            cancel();if(click)v.performClick();
        }
        return true;
    }
    boolean motion(MotionEvent e) {
        if(e.getAction()!=MotionEvent.ACTION_SCROLL)return false;
        float value=e.isFromSource(InputDevice.SOURCE_ROTARY_ENCODER)?e.getAxisValue(MotionEvent.AXIS_SCROLL):e.getAxisValue(MotionEvent.AXIS_VSCROLL);
        if(Float.isNaN(value) || Float.isInfinite(value))return true;
        wheel+=Math.max(-12,Math.min(12,value));
        while(Math.abs(wheel)>=1) { int sign=wheel>0?1:-1;move(-sign);wheel-=sign; }
        return true;
    }
}
