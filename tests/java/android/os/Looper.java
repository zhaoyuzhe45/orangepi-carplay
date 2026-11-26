package android.os;
public final class Looper { private static final ThreadLocal<Looper> CURRENT=new ThreadLocal<>(); public static Looper myLooper(){return CURRENT.get();} public static void prepare(){CURRENT.set(new Looper());} }
