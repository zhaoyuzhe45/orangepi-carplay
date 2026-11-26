package com.carplaybox.audio;

import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

/** Start independent audio and vehicle consumers on boot, update and app open. */
final class CarPlayServices {
    private CarPlayServices() {}
    static void start(Context context) {
        for (Class<?> type : new Class<?>[]{AudioCaptureService.class, VehicleLocationService.class, AppIntegrationService.class}) {
            try {
                Intent intent=new Intent(context,type);
                if(Build.VERSION.SDK_INT>=26)context.startForegroundService(intent);
                else context.startService(intent);
            } catch(RuntimeException failure) {
                Log.e("CarPlayServices","Unable to start "+type.getSimpleName(),failure);
            }
        }
    }
}
