package com.carplaybox.audio;
import android.content.*;
import android.os.*;
import android.util.Log;

/** Receives the installed Amap Auto standard integration broadcasts. */
final class RouteProvider extends BroadcastReceiver implements AutoCloseable {
    private final Context context;
    private final RouteSnapshot snapshot=new RouteSnapshot();
    private int lastType=-99;
    RouteProvider(Context context) {
        this.context=context;
        context.registerReceiver(this,new IntentFilter("AUTONAVI_STANDARD_BROADCAST_SEND"),
            "android.permission.ACCESS_FINE_LOCATION",new Handler());
    }
    @Override public void onReceive(Context c,Intent intent) {
        if(intent==null || !"AUTONAVI_STANDARD_BROADCAST_SEND".equals(intent.getAction()))return;
        try {
            if(intent.getIntExtra("KEY_TYPE",-1)!=10001)return;
            // Android 8 does not expose broadcast sender UID. Require the sender's
            // location permission and the configured navigation app to be installed.
            context.getPackageManager().getApplicationInfo("cn.audi.knobmap.amap1",0);
            int type=intent.getIntExtra("TYPE",-1);
            snapshot.update(type,intent.getIntExtra("NEW_ICON",-1),intent.getIntExtra("CUR_SEG_NUM",-1),
                intent.getIntExtra("ROUTE_REMAIN_DIS",-1),intent.getIntExtra("ROUTE_REMAIN_TIME",-1),
                intent.getIntExtra("SEG_REMAIN_DIS",-1),intent.getStringExtra("CUR_ROAD_NAME"),
                intent.getStringExtra("NEXT_ROAD_NAME"),intent.getStringExtra("endPOIName"),SystemClock.uptimeMillis());
            if(type!=lastType){Log.i("CarPlayRoute","Amap navigation broadcast type="+type);lastType=type;}
        } catch(Exception invalid){Log.w("CarPlayRoute","Ignored invalid navigation broadcast: "+invalid.getClass().getSimpleName());}
    }
    byte[] read(){try{return snapshot.encode(SystemClock.uptimeMillis());}catch(java.io.IOException e){return new byte[]{1};}}
    @Override public void close(){context.unregisterReceiver(this);}
}
