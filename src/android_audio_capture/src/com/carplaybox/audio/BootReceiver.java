package com.carplaybox.audio;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        CarPlayUiShortcut.requestWhenReady(context);
        CarPlayServices.start(context);
    }
}
