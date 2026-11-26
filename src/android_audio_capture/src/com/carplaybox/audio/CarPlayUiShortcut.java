package com.carplaybox.audio;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** Removes the legacy launcher shortcut when updating an older installation. */
final class CarPlayUiShortcut {
    private static final String TAG = "CarPlayUiShortcut";
    private static final String COMPONENT_SUFFIX = ".CarUiShortcutActivity";
    private static final String REMOVE_ACTION = "com.android.launcher.action.UNINSTALL_SHORTCUT";
    private static final String SHORTCUT_NAME = "返回车机";

    private CarPlayUiShortcut() {}

    static void requestWhenReady(Context source) {
        Context context = source.getApplicationContext();
        removeHomeShortcut(context);
    }

    static void sessionEnded(Context source) {
        // No launcher entry is created for the car UI.
    }

    private static void removeHomeShortcut(Context context) {
        try {
            ComponentName target = new ComponentName(context.getPackageName(),
                    context.getPackageName() + COMPONENT_SUFFIX);
            Intent shortcutTarget = new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(target);
            Intent remove = new Intent(REMOVE_ACTION)
                    .putExtra(Intent.EXTRA_SHORTCUT_INTENT, shortcutTarget)
                    .putExtra(Intent.EXTRA_SHORTCUT_NAME, SHORTCUT_NAME)
                    .putExtra("duplicate", false);
            context.sendBroadcast(remove);
            Log.i(TAG, "requested removal of legacy return-to-car shortcut");
        } catch (Throwable failure) {
            Log.w(TAG, "could not remove the legacy home-screen shortcut", failure);
        }
    }
}
