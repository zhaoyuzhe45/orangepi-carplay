package com.carplaybox.audio;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Internal card over navigation. Closing the card never rejects the phone call. */
public final class IncomingCallActivity extends Activity {
    private final Handler handler=new Handler();
    private PhoneNavigation navigation=new PhoneNavigation();
    private PhoneCallCard card;
    private Button hide;
    private String device="",uuid="";
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);PhoneBackend.LIVE.start(this);requestWindowFeature(Window.FEATURE_NO_TITLE);
        PhoneStyle.immersive(getWindow());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED|WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
        LinearLayout root=PhoneStyle.card(this);TextView result=PhoneStyle.text(this,"返回或收起可继续使用其他应用",14,PhoneStyle.MUTED);
        hide=PhoneStyle.button(this,"收起",PhoneStyle.KEY,v->finish());
        card=new PhoneCallCard(this,PhoneBackend.LIVE,result::setText,()-> {
            navigation.cancel();navigation=new PhoneNavigation();navigation.add(hide);card.addNavigation(navigation);
        });
        root.addView(card.root,Ui.matchWrap());root.addView(result,Ui.matchWrap());
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,PhoneStyle.dp(this,50));p.topMargin=PhoneStyle.dp(this,10);root.addView(hide,p);
        ScrollView scroll=new ScrollView(this);scroll.addView(root);setContentView(scroll);
        getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        getWindow().setLayout(Math.min(getResources().getDisplayMetrics().widthPixels-PhoneStyle.dp(this,40),PhoneStyle.dp(this,620)),-2);
        readTarget(getIntent());hide.requestFocus();
    }
    private void readTarget(Intent i) { device=i.getStringExtra("device");uuid=i.getStringExtra("uuid");if(device==null || uuid==null)finish(); }
    @Override protected void onNewIntent(Intent i) { super.onNewIntent(i);setIntent(i);readTarget(i); }
    private final Runnable poll=new Runnable() { public void run() {
        if(isFinishing())return;
        PhoneState s=PhoneBackend.LIVE.state();
        if(s!=null && !s.selected.equals(device)) { finish();return; }
        if(s!=null && PhoneUiModel.fresh(s.connected,s.knownCalls,s.sampled,SystemClock.uptimeMillis())) {
            boolean found=false;for(BluetoothCalls.Call c:s.calls)if(c.uuid.equals(uuid))found=true;
            if(!found) { finish();return; }
        }
        card.render(s);handler.postDelayed(this,300);
    } };
    @Override protected void onResume() { super.onResume();PhoneStyle.immersive(getWindow());handler.post(poll); }
    @Override public void onWindowFocusChanged(boolean focused) { super.onWindowFocusChanged(focused);if(focused)PhoneStyle.immersive(getWindow()); }
    @Override protected void onPause() { handler.removeCallbacksAndMessages(null);navigation.cancel();super.onPause(); }
    @Override protected void onDestroy(){PhoneBackend.LIVE.stop();super.onDestroy();}
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if(e.getKeyCode()==KeyEvent.KEYCODE_ESCAPE) { if(e.getAction()==KeyEvent.ACTION_UP)finish();return true; }
        return navigation.key(e)||super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return navigation.motion(e)||super.dispatchGenericMotionEvent(e); }
}
