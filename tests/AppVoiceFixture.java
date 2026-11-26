package com.carplaybox.appfixture;
import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
public final class AppVoiceFixture extends Activity {
    @Override public void onCreate(Bundle state){
        super.onCreate(state);Log.i("CarPlayMediaFixture","VOICE_LAUNCHED");
        android.content.SharedPreferences p=getSharedPreferences("evidence",0);
        p.edit().putInt("voice_launches",p.getInt("voice_launches",0)+1).commit();finish();
    }
}
