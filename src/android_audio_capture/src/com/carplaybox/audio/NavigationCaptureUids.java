package com.carplaybox.audio;

import java.util.Arrays;
import java.util.function.Function;
import java.util.function.ToIntFunction;

final class NavigationCaptureUids {
    static final String SPLIT_PROPERTY="debug.carplay.audio_nav_split";
    private static final String[] PACKAGES={
            "cn.audi.knobmap.amap1", "com.autonavi.amapauto", "com.baidu.naviauto"
    };

    private NavigationCaptureUids() {}

    static boolean useAltTransport(Function<String,String> property){
        if("0".equals(property.apply(SPLIT_PROPERTY)))return false;
        String override=property.apply("debug.carplay.nav101");
        return "1".equals(override.isEmpty()?property.apply("persist.carplay.nav101"):override);
    }

    static int[] resolve(String setting,ToIntFunction<String> uidForPackage){
        if("0".equals(setting))return new int[0];
        int[] uids=new int[PACKAGES.length];
        int count=0;
        for(String name:PACKAGES){
            int uid=uidForPackage.applyAsInt(name);
            if(uid<0)continue;
            int i=0;
            while(i<count && uids[i]!=uid)i++;
            if(i==count)uids[count++]=uid;
        }
        return Arrays.copyOf(uids,count);
    }
}
