package com.carplaybox.audio;

import android.os.Process;

final class CaptureThreadPriority {
    static final int UNKNOWN = Integer.MAX_VALUE;

    private CaptureThreadPriority() {}

    static int current() {
        try {
            return Process.getThreadPriority(Process.myTid());
        } catch (IllegalArgumentException | SecurityException unavailable) {
            return UNKNOWN;
        }
    }

    static int begin(String setting) {
        int original = current();
        if (!"0".equals(setting) && original != UNKNOWN && original > Process.THREAD_PRIORITY_AUDIO) {
            set(Process.THREAD_PRIORITY_AUDIO);
        }
        return original;
    }

    static void end(int original) {
        if (original != UNKNOWN && current() != original) set(original);
    }

    private static void set(int priority) {
        try {
            Process.setThreadPriority(priority);
        } catch (IllegalArgumentException | SecurityException unavailable) {
            // Some firmware policies disallow a negative nice for this app.
        }
    }
}
