package android.os;

public final class Process {
    public static final int THREAD_PRIORITY_AUDIO = -16;
    public static int priority;
    public static boolean refuse;

    public static int myTid() { return 123; }
    public static int getThreadPriority(int tid) {
        if (tid != myTid()) throw new AssertionError("wrong thread");
        return priority;
    }
    public static void setThreadPriority(int value) {
        if (refuse) throw new SecurityException("priority rejected");
        priority = value;
    }
}
