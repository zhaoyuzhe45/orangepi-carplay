package com.carplaybox.audio;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.content.Intent;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/** Standalone launcher entry, sharing the one service-owned Bluetooth connection. */
public final class PhoneActivity extends Activity {
    private static final int REQUEST_PAIRING=71;
    private PhonePanel panel;
    private ContactsPanel contacts;
    private boolean showingContacts=true;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);PhoneBackend.LIVE.start(this);
        getWindow().setStatusBarColor(PhoneStyle.BG);getWindow().setNavigationBarColor(PhoneStyle.BG);
        PhoneStyle.immersive(getWindow());
        panel=new PhonePanel(this,PhoneBackend.LIVE);panel.restore(state);contacts=new ContactsPanel(this);
        showingContacts=state==null || state.getBoolean("phone-contacts",true);
        setContentView(showingContacts?contacts.root:panel.root);
    }
    void showContacts() { panel.stop();showingContacts=true;setContentView(contacts.root);contacts.start(); }
    void pairPhone() {
        // Settings owns the timeout alarm, including if our process exits while pairing.
        try {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE)
                .putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION,180),REQUEST_PAIRING);
        }catch(RuntimeException e) { pairingMessage("无法开启配对，请在系统蓝牙设置中开启可被发现"); }
    }
    private void pairingMessage(String text) { contacts.message(text);panel.message(text); }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(request==REQUEST_PAIRING) {
            BluetoothAdapter adapter=BluetoothAdapter.getDefaultAdapter();
            String name=adapter==null?"盒子":adapter.getName();
            pairingMessage(result>0?"请在手机搜索 "+name+" · 配对窗口 3 分钟":"已取消配对模式");
        }
    }
    void showDialer(String number) {
        contacts.stop();showingContacts=false;if(number!=null)panel.setNumber(number);setContentView(panel.root);
        ((android.view.inputmethod.InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(panel.root.getWindowToken(),0);panel.start();
    }
    @Override protected void onResume() { super.onResume();PhoneStyle.immersive(getWindow());if(showingContacts)contacts.start();else panel.start(); }
    @Override public void onWindowFocusChanged(boolean focused) { super.onWindowFocusChanged(focused);if(focused)PhoneStyle.immersive(getWindow()); }
    @Override protected void onPause() { panel.stop();contacts.stop();super.onPause(); }
    @Override protected void onDestroy() { contacts.close();PhoneBackend.LIVE.stop();super.onDestroy(); }
    @Override protected void onSaveInstanceState(Bundle state) { panel.save(state);state.putBoolean("phone-contacts",showingContacts);super.onSaveInstanceState(state); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if(e.getKeyCode()==KeyEvent.KEYCODE_ESCAPE) { if(e.getAction()==KeyEvent.ACTION_UP)finish();return true; }
        return (showingContacts?contacts!=null && contacts.key(e):panel!=null && panel.key(e))||super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return (showingContacts?contacts!=null && contacts.motion(e):panel!=null && panel.motion(e))||super.dispatchGenericMotionEvent(e); }
}
