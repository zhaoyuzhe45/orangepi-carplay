package com.carplaybox.audio;

import android.content.Context;
import android.os.Looper;
import android.os.SystemClock;
import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Run from app_process on an idle bench box; no PCM is saved. */
public final class NavigationUidDeviceProbe {
    private static final String PREFIX="com.carplaybox.navuidfixture";
    private static final int[] FREQUENCIES={1000,1600,2400};
    private static final String[] SUFFIXES={"music","a","b"};

    public static void main(String[] args) {
        try { verify(); }
        catch(Throwable failure){failure.printStackTrace(System.err);System.exit(1);}
        System.exit(0);
    }

    private static void verify() throws Exception {
        Looper.prepareMainLooper();
        Class<?> activityThread=Class.forName("android.app.ActivityThread");
        Object thread=activityThread.getMethod("systemMain").invoke(null);
        Context context=(Context)activityThread.getMethod("getSystemContext").invoke(thread);
        int[] uids=new int[3];
        for(int i=0;i<uids.length;i++){
            String pkg=PREFIX+SUFFIXES[i];
            uids[i]=context.getPackageManager().getApplicationInfo(pkg,0).uid;
        }
        check(uids[0]!=uids[1] && uids[1]!=uids[2] && uids[0]!=uids[2],"need three distinct fixture UIDs");
        System.out.println("fixtureUids="+Arrays.toString(uids));
        int[] navigation={uids[1],uids[2]};
        run(context,navigation);
        run(context,new int[0]);
        run(context,navigation);
        System.out.println("PASS multi-UID capture, music duck/recovery, isolated media fallback, reopen");
    }

    private static SplitAudioCapture open(Context context,int[] uids) throws Exception {
        Constructor<SplitAudioCapture> constructor;
        try { constructor=SplitAudioCapture.class.getDeclaredConstructor(Context.class,int[].class); }
        catch(NoSuchMethodException legacy) {
            return new SplitAudioCapture(context,uids.length==0?-1:uids[0]);
        }
        return constructor.newInstance(context,uids);
    }

    private static void tone(int i,boolean start) throws Exception {
        List<String> command=new ArrayList<>(Arrays.asList("/system/bin/am",start?"start-foreground-service":"stopservice",
                "--user","0","-n",PREFIX+SUFFIXES[i]+"/com.carplaybox.fixture.AudioToneFixture"));
        if(start)command.addAll(Arrays.asList("--ei","frequency",String.valueOf(FREQUENCIES[i])));
        java.lang.Process process=new ProcessBuilder(command).inheritIO().start();
        if(!process.waitFor(3,TimeUnit.SECONDS)){process.destroyForcibly();throw new AssertionError("tone command timed out");}
        if(start)check(process.exitValue()==0,"tone fixture failed to start");
    }

    private static void run(Context context,int[] uids) throws Exception {
        boolean split=uids.length>0;
        try(SplitAudioCapture capture=open(context,uids)){
            for(int i=0;i<SUFFIXES.length;i++)tone(i,true);
            short[] media=new short[1920], nav=new short[1920], out=new short[1920];
            NavigationMixer mixer=new NavigationMixer();
            double[] m=new double[3], n=new double[3], o=new double[3];
            for(int i=0;i<110;i++){
                capture.read(media,nav);
                mixer.mix(media,nav,out,SystemClock.elapsedRealtime());
                if(i>=70){ measure(media,m,40); measure(nav,n,40); measure(out,o,40); }
            }
            System.out.printf(Locale.ROOT,"%s media=%s nav=%s output=%s%n",
                    split?"split":"single",Arrays.toString(m),Arrays.toString(n),Arrays.toString(o));
            check(m[0]>3000,"missing media tone");
            if(split){
                for(int i=1;i<3;i++){
                    check(n[i]>3000,"navigation UID "+uids[i-1]+" missing from navigation track");
                    check(m[i]<n[i]*.02,"navigation tone leaked to media");
                    check(o[i]>n[i]*.95 && o[i]<n[i]*1.05,"navigation changed or doubled in output");
                }
                check(n[0]<m[0]*.02,"music leaked to navigation");
                check(o[0]>m[0]*.22 && o[0]<m[0]*.28,"music did not duck");
                tone(1,false);
                tone(2,false);
                for(int i=0;i<90;i++){
                    capture.read(media,nav);
                    mixer.mix(media,nav,out,SystemClock.elapsedRealtime());
                }
                double music=amplitude(media,1000);
                check(!mixer.navigationActive() && music>3000 && amplitude(out,1000)>music*.98,
                        "music did not recover after prompt");
            }else{
                for(int i=0;i<3;i++){
                    check(m[i]>3000 && n[i]<m[i]*.02,"single mix did not capture all fixture UIDs");
                    check(o[i]>m[i]*.98 && o[i]<m[i]*1.02,"unexpected duck in single mix");
                }
                check(!mixer.navigationActive(),"navigation active with no nav mix");
            }
        } finally {
            for(int i=0;i<SUFFIXES.length;i++)tone(i,false);
        }
        SystemClock.sleep(250);
    }

    private static void measure(short[] samples,double[] average,int frames){
        for(int i=0;i<average.length;i++)average[i]+=amplitude(samples,FREQUENCIES[i])/frames;
    }

    private static double amplitude(short[] samples,int frequency){
        double re=0,im=0;
        int frames=samples.length/2;
        for(int i=0;i<frames;i++){
            double angle=2*Math.PI*frequency*i/48000;
            re+=samples[2*i]*Math.cos(angle);
            im+=samples[2*i]*Math.sin(angle);
        }
        return 2*Math.hypot(re,im)/frames;
    }

    private static void check(boolean condition,String problem){
        if(!condition)throw new AssertionError(problem);
    }
}
