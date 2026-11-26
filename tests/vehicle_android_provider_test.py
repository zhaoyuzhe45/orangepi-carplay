"""Run the production Android adapter against a small, source-backed 8.1 model.

The model preserves the relevant LocationManagerService behavior: silent
mock-app-op rejection, enabled-provider override, replacing GPS on add, async
location readback, clearing all cached locations when a provider is disabled
while the mock provider retains its internal fix, and Android 8.1's
UPDATE_APP_OPS_STATS requirement for checking another UID. It is not device
acceptance; tests intentionally use fixture coordinates only in this harness.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "src/android_audio_capture/src/com/carplaybox/audio"
STUBS = {
    "android/net/LocalSocket.java": """
package android.net;
import java.io.*;
public final class LocalSocket {
 public static final ByteArrayOutputStream wire=new ByteArrayOutputStream();
 private final InputStream acknowledgements=new ByteArrayInputStream(new byte[]{1,1,1});
 public void connect(LocalSocketAddress address) throws IOException {}
 public void setSoTimeout(int timeout) throws IOException {}
 public OutputStream getOutputStream() throws IOException { return wire; }
 public InputStream getInputStream() throws IOException { return acknowledgements; }
 public void close() throws IOException {}
}
""",
    "android/net/LocalSocketAddress.java": """
package android.net;
public final class LocalSocketAddress {
 public enum Namespace { ABSTRACT }
 public LocalSocketAddress(String name, Namespace namespace) {}
}
""",
    "android/os/Looper.java": """
package android.os;
public final class Looper { public static Looper getMainLooper() { return new Looper(); } }
""",
    "android/os/SystemProperties.java": """
package android.os;
public final class SystemProperties {
 public static boolean gnss;
 public static String get(String key, String fallback) { return gnss ? "1" : fallback; }
}
""",
    "android/location/LocationListener.java": """
package android.location;
public interface LocationListener {}
""",
    "android/Manifest.java": """
package android;
public final class Manifest { public static final class permission {
 public static final String ACCESS_FINE_LOCATION="android.permission.ACCESS_FINE_LOCATION";
} }
""",
    "android/content/ContentResolver.java": """
package android.content;
public final class ContentResolver { public int mode=3; public String providers="gps,network"; }
""",
    "android/content/Context.java": """
package android.content;
import android.app.AppOpsManager;
import android.location.LocationManager;
import android.content.pm.PackageManager;
public class Context {
 public static final String LOCATION_SERVICE="location", APP_OPS_SERVICE="appops";
 public final ContentResolver resolver=new ContentResolver();
 public final AppOpsManager appOps=new AppOpsManager();
 public final PackageManager packages=new PackageManager();
 public final LocationManager locations;
 public boolean finePermission=true, hardwarePermission=false, reportOnly;
 public Context() { this(false); }
 public Context(boolean reportOnly) {
  this.reportOnly=reportOnly; locations=new LocationManager(this);
 }
 public Object getSystemService(String name) { return LOCATION_SERVICE.equals(name)?locations:appOps; }
 public Context getApplicationContext() { return this; }
 public String getPackageName() { return "com.carplaybox.audio"; }
 public ContentResolver getContentResolver() { return resolver; }
 public PackageManager getPackageManager() { return packages; }
 public int checkSelfPermission(String permission) {
  if (permission.equals("android.permission.LOCATION_HARDWARE") ||
      permission.equals("android.permission.INSTALL_LOCATION_PROVIDER")) return hardwarePermission?0:-1;
  return finePermission?0:-1;
 }
}
""",
    "android/content/pm/ApplicationInfo.java": """
package android.content.pm;
public class ApplicationInfo {
 public final String packageName; public final int uid;
 public ApplicationInfo(String name,int id) { packageName=name; uid=id; }
}
""",
    "android/content/pm/PackageManager.java": """
package android.content.pm;
import java.util.*;
public class PackageManager {
 public static final int PERMISSION_GRANTED=0;
 public final List<ApplicationInfo> apps=new ArrayList<ApplicationInfo>();
 public PackageManager() {
  apps.add(new ApplicationInfo("com.carplaybox.audio",10123));
  apps.add(new ApplicationInfo("ordinary.other.app",10124));
 }
 public List<ApplicationInfo> getInstalledApplications(int flags) { return apps; }
}
""",
    "android/app/AppOpsManager.java": """
package android.app;
import java.util.*;
public final class AppOpsManager {
 public static final int MODE_ALLOWED=0, MODE_IGNORED=1, MODE_ERRORED=2, MODE_DEFAULT=3;
 public static final String OPSTR_FINE_LOCATION="android:fine_location";
 public int mockMode=MODE_ALLOWED, fineMode=MODE_ALLOWED;
 public boolean censusAvailable=true, updateStatsPermission=true;
 public int crossUidChecks;
 public final Map<String,Integer> otherModes=new HashMap<String,Integer>();
 public final List<PackageOps> otherRecords=new ArrayList<PackageOps>();
 public int checkOpNoThrow(String op,int uid,String name) {
  // AppOpsService.checkOperation -> verifyIncomingUid on stock Android 8.1.
  // GET_APP_OPS_STATS permits the package census, not this cross-UID check.
  if (uid!=10123) {
   crossUidChecks++;
   if (!updateStatsPermission)
    throw new SecurityException("uid 10123 does not have android.permission.UPDATE_APP_OPS_STATS");
  }
  if (OPSTR_FINE_LOCATION.equals(op)) return fineMode;
  if ("com.carplaybox.audio".equals(name) && uid==10123) return mockMode;
  Integer mode=otherModes.get(name); return mode==null?MODE_ERRORED:mode;
 }
 public List<PackageOps> getPackagesForOps(int[] ops) {
  if (!censusAvailable) throw new SecurityException("GET_APP_OPS_STATS denied");
  List<PackageOps> result=new ArrayList<PackageOps>();
  result.add(new PackageOps("com.carplaybox.audio",10123,mockMode));
  result.addAll(otherRecords); return result;
 }
 public static final class PackageOps {
  private final String name; private final int uid,mode;
  public PackageOps(String name,int uid,int mode) { this.name=name; this.uid=uid; this.mode=mode; }
  public String getPackageName() { return name; }
  public int getUid() { return uid; }
  public List<OpEntry> getOps() { return Arrays.asList(new OpEntry(mode)); }
 }
 public static final class OpEntry {
  private final int mode; public OpEntry(int mode) { this.mode=mode; }
  public int getOp() { return 58; } public int getMode() { return mode; }
 }
}
""",
    "android/os/Bundle.java": """
package android.os;
import java.util.*;
public class Bundle {
 private final Map<String,Object> data=new HashMap<String,Object>();
 public Bundle() {} public Bundle(Bundle other) { data.putAll(other.data); }
 public void putString(String key,String value) { data.put(key,value); }
 public String getString(String key) { Object v=data.get(key); return v instanceof String?(String)v:null; }
 public void putBoolean(String key,boolean value) { data.put(key,value); }
 public boolean getBoolean(String key) { return Boolean.TRUE.equals(data.get(key)); }
 public void putLong(String key,long value) { data.put(key,value); }
 public long getLong(String key) { Object v=data.get(key); return v instanceof Long?(Long)v:0; }
}
""",
    "android/os/Process.java": """
package android.os;
public final class Process { public static int myUid() { return 10123; } }
""",
    "android/os/SystemClock.java": """
package android.os;
public final class SystemClock {
 public static long now=10000;
 public static long elapsedRealtime() { return now; }
 public static void sleep(long delay) { now+=delay; }
}
""",
    "android/provider/Settings.java": """
package android.provider;
import android.content.ContentResolver;
public final class Settings { public static final class Secure {
 public static final String LOCATION_MODE="location_mode", LOCATION_PROVIDERS_ALLOWED="location_providers_allowed";
 public static final int LOCATION_MODE_OFF=0, LOCATION_MODE_SENSORS_ONLY=1, LOCATION_MODE_BATTERY_SAVING=2, LOCATION_MODE_HIGH_ACCURACY=3;
 public static int getInt(ContentResolver resolver,String name,int fallback) { return resolver.mode; }
 public static String getString(ContentResolver resolver,String name) { return resolver.providers; }
} }
""",
    "android/location/Criteria.java": """
package android.location;
public final class Criteria { public static final int POWER_LOW=1, ACCURACY_FINE=1; }
""",
    "android/location/Location.java": """
package android.location;
import android.os.Bundle;
public class Location {
 private final String provider; private double latitude,longitude,altitude;
 private long time,elapsed; private float accuracy,speed,bearing; private Bundle extras;
 public boolean mock;
 public Location(String provider) { this.provider=provider; }
 public Location(Location other) {
  provider=other.provider; latitude=other.latitude; longitude=other.longitude;
  time=other.time; elapsed=other.elapsed; accuracy=other.accuracy; speed=other.speed;
  bearing=other.bearing; altitude=other.altitude; mock=other.mock;
  extras=other.extras==null?null:new Bundle(other.extras);
 }
 public String getProvider() { return provider; }
 public void setLatitude(double value) { latitude=value; } public double getLatitude() { return latitude; }
 public void setLongitude(double value) { longitude=value; } public double getLongitude() { return longitude; }
 public void setTime(long value) { time=value; } public long getTime() { return time; }
 public void setElapsedRealtimeNanos(long value) { elapsed=value; } public long getElapsedRealtimeNanos() { return elapsed; }
 public void setAccuracy(float value) { accuracy=value; } public float getAccuracy() { return accuracy; }
 public void setSpeed(float value) { speed=value; } public void setBearing(float value) { bearing=value; }
 public void setAltitude(double value) { altitude=value; }
 public double getAltitude() { return altitude; } public boolean hasAltitude() { return altitude!=0; }
 public float getSpeed() { return speed; } public boolean hasSpeed() { return speed!=0; }
 public float getBearing() { return bearing; } public boolean hasBearing() { return bearing!=0; }
 public boolean hasAccuracy() { return accuracy!=0; }
 public void setExtras(Bundle value) { extras=value; } public Bundle getExtras() { return extras; }
 public boolean isFromMockProvider() { return mock; }
}
""",
    "android/location/LocationManager.java": """
package android.location;
import android.content.Context;
import android.os.SystemClock;
public class LocationManager {
 public static final String GPS_PROVIDER="gps", PASSIVE_PROVIDER="passive";
 private final Context context;
 public boolean installed,enabledOverride,ignorePublish,ignoreRemove,revokeOnAdd,revokeOnRemove,revokeAfterRemove,gpsOffAfterReadback;
 public int adds,enables,publishes,removes,injections,reports;
 public long readbackDelay; private long visibleAt; private Location pending;
 public Location last,mockLocation,passive;
 public boolean reportGps=true;
 private String observedProviders;
 public LocationManager(Context context) { this.context=context; mService=context.reportOnly?new ReportService():new Service(); observedProviders=context.resolver.providers; }
 private boolean allowed() { return context.appOps.mockMode==0; }
 private boolean listed(String providers,String name) { return (","+providers+",").contains(","+name+","); }
 private void settingsChanged() {
  String current=context.resolver.providers;
  // Stock 8.1 updateProvidersLocked clears ALL cached fixes when any provider
  // is disabled. The mock GPS override/internal fix survive network disable.
  if ((listed(observedProviders,"network") && !listed(current,"network")) ||
      (listed(observedProviders,"gps") && !listed(current,"gps") && !enabledOverride)) last=null;
  observedProviders=current;
 }
 public boolean isProviderEnabled(String name) {
  settingsChanged(); return enabledOverride || listed(context.resolver.providers,name);
 }
 public void requestLocationUpdates(String name,long interval,float distance,LocationListener listener,android.os.Looper looper) {}
 public void removeUpdates(LocationListener listener) {}
 public Location getLastKnownLocation(String name) {
  settingsChanged();
  if (PASSIVE_PROVIDER.equals(name)) return passive==null?null:new Location(passive);
  if (context.appOps.fineMode!=0) return null;
  if (pending!=null && SystemClock.elapsedRealtime()>=visibleAt) { last=pending; pending=null; }
  if (last!=null && gpsOffAfterReadback) {
   gpsOffAfterReadback=false; context.resolver.mode=0; context.resolver.providers="";
  }
  return last==null?null:new Location(last);
 }
 private final Object mService;
 public final class Service {
  public boolean injectLocation(Location location) {
   injections++; if (!context.hardwarePermission || context.appOps.fineMode!=0) return false;
   last=new Location(location); last.mock=false; return true;
  }
 }
 public final class ReportService {
  public void reportLocation(Location location,boolean passive) {
   reports++; injections++; if (!context.hardwarePermission || context.appOps.fineMode!=0 || passive)
    throw new SecurityException("reportLocation rejected");
   LocationManager.this.passive=new Location(location); LocationManager.this.passive.mock=false;
   if (reportGps) { last=new Location(location); last.mock=false; }
  }
 }
 public void addTestProvider(String name,boolean a,boolean b,boolean c,boolean d,boolean e,boolean f,boolean g,int power,int accuracy) {
  adds++; if (!allowed()) return;
  installed=true; last=null; pending=null; mockLocation=null;
  if (revokeOnAdd) context.appOps.mockMode=1;
 }
 public void setTestProviderEnabled(String name,boolean enabled) {
  enables++; if (!allowed()) return;
  if (!installed) throw new IllegalArgumentException("Provider unknown");
  enabledOverride=enabled;
 }
 public void setTestProviderLocation(String name,Location location) {
  publishes++; if (!allowed() || ignorePublish) return;
  if (!installed) throw new IllegalArgumentException("Provider unknown");
  mockLocation=new Location(location); mockLocation.mock=true;
  pending=new Location(mockLocation); visibleAt=SystemClock.elapsedRealtime()+readbackDelay;
 }
 public void removeTestProvider(String name) {
  removes++; if (revokeOnRemove) context.appOps.mockMode=1;
  if (!allowed() || ignoreRemove) return;
  if (!installed) throw new IllegalArgumentException("Provider unknown");
  installed=false; enabledOverride=false; last=null; pending=null; mockLocation=null;
  if (revokeAfterRemove) context.appOps.mockMode=1;
 }
}
""",
}

CASES = [
    "gnss_replaces_without_readback", "hardware_injection", "hardware_report_location", "hardware_report_passive_only", "hardware_transition_removes_orphan", "accepted_readback", "mode_ignored", "mode_default", "census_denied",
    "other_allowed", "uid_allowed", "foreign_existing", "foreign_replacement",
    "gps_toggle_updates", "gps_toggle_watchdog", "gps_token_match",
    "permission_watchdog", "cleanup_retry", "silent_publish",
    "silent_update", "silent_remove", "null_ownership", "same_process_owner",
    "revoke_during_add", "revoke_during_remove", "revoke_after_remove", "foreign_after_remove", "read_permission",
    "gps_off_silent_remove", "gps_off_permission_retry", "gps_off_foreign_allowed",
    "gps_off_unverified", "gps_off_foreign_readback", "gps_off_no_override",
    "gps_off_read_permission", "disabled_cache_stays_empty", "gps_off_after_readback",
]

PERMISSION_CASES = ["ordinary_other_uid", "cross_uid_permission_denied", "cross_uid_other_allowed"]
CASES += ["orphan_own", "orphan_fresh", "orphan_foreign", "orphan_other_authorized"]
PERMISSION_TEST = """
package com.carplaybox.audio;

/** The cross-UID guard is enforced even when the queried app has no mock grant. */
public final class VehicleAppOpsPermissionTest {
 public static void main(String[] args) {
  VehicleAndroidProviderTest.Environment e = new VehicleAndroidProviderTest.Environment();
  String name = args[0];
  if (name.equals("cross_uid_permission_denied")) e.context.appOps.updateStatsPermission=false;
  if (name.equals("cross_uid_other_allowed")) e.context.appOps.otherModes.put("ordinary.other.app",0);
  e.update();
  VehicleAndroidProviderTest.check(e.context.appOps.crossUidChecks>0,
    "fixture must exercise cross-UID effective-mode query");
  if (name.equals("ordinary_other_uid")) {
   VehicleAndroidProviderTest.check(e.bridge.applied,
     "authorized ordinary foreign UID must not prevent accepted injection");
   e.bridge.stop("test complete");
   VehicleAndroidProviderTest.check(!e.context.locations.installed,"authorized cleanup must remove own provider");
  } else if (name.equals("cross_uid_permission_denied")) {
   VehicleAndroidProviderTest.check(!e.bridge.applied && e.bridge.hasFix &&
     e.context.locations.adds==0 && e.bridge.status.contains("UPDATE_APP_OPS_STATS"),
     "GET-only permission must reject before mutation and name missing UPDATE_APP_OPS_STATS");
  } else if (name.equals("cross_uid_other_allowed")) {
   VehicleAndroidProviderTest.check(!e.bridge.applied && e.context.locations.adds==0 &&
     e.bridge.status.contains("ordinary.other.app"),
     "cross-UID query permission must not authorize replacing another allowed mock producer");
  } else throw new AssertionError("unknown case "+name);
  System.out.println("PASS "+name);
 }
}
"""

def main():
    with tempfile.TemporaryDirectory(prefix="vehicle-android-") as temporary:
        directory = Path(temporary)
        stubs = []
        for relative, source in STUBS.items():
            path = directory / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            stubs.append(str(path))
        permission_test = directory / "VehicleAppOpsPermissionTest.java"
        permission_test.write_text(PERMISSION_TEST, encoding="utf-8")
        classes = directory / "classes"
        classes.mkdir()
        subprocess.run(["javac", "-encoding", "UTF-8", "-d", str(classes), *stubs,
                        str(JAVA / "VehicleNmeaParser.java"),
                        str(JAVA / "VehicleLocationBridge.java"),
                        str(JAVA / "VehicleGnssChannel.java"),
                        str(JAVA / "VehicleAndroidProvider.java"),
                        str(ROOT / "tests/VehicleAndroidProviderTest.java"), str(permission_test)], check=True)
        failed = []
        for case in CASES + PERMISSION_CASES:
            test_class = "VehicleAppOpsPermissionTest" if case in PERMISSION_CASES else "VehicleAndroidProviderTest"
            result = subprocess.run(["java", "-cp", str(classes),
                                     "com.carplaybox.audio." + test_class, case],
                                    capture_output=True, text=True, encoding="utf-8")
            print(result.stdout.strip() or result.stderr.strip())
            if result.returncode:
                failed.append(case)
        if failed:
            raise SystemExit("FAIL Android provider cases: " + ", ".join(failed))
        print("PASS all %d Android 8.1 provider lifecycle cases" % (len(CASES) + len(PERMISSION_CASES)))

if __name__ == "__main__":
    main()
