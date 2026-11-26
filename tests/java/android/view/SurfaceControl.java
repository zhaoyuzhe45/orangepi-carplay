package android.view;
import android.os.IBinder;
import android.graphics.Rect;
public final class SurfaceControl {
 public static boolean created,destroyed,closed,failProjection; public static int rotation,layer; public static Rect physical,output;
 public static int creates,destroys; public static Thread attachThread;
 public static java.util.concurrent.CountDownLatch refreshed;
 public static void reset(){created=destroyed=closed=failProjection=false;rotation=layer=-1;physical=output=null;creates=destroys=0;attachThread=null;refreshed=new java.util.concurrent.CountDownLatch(1);}
 public static IBinder createDisplay(String name,boolean secure){created=true;creates++;return new IBinder(){};}
 public static void openTransaction(){} public static void closeTransaction(){closed=true;}
 public static void setDisplaySurface(IBinder token,Surface surface){}
 public static void setDisplayProjection(IBinder token,int r,Rect p,Rect o){attachThread=Thread.currentThread();if(creates>1)refreshed.countDown();if(failProjection)throw new IllegalStateException("projection failure");rotation=r;physical=p;output=o;}
 public static void setDisplayLayerStack(IBinder token,int l){layer=l;}
 public static void destroyDisplay(IBinder token){destroyed=true;destroys++;}
}
