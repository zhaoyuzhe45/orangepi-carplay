package com.carplaybox.audio;

import java.util.*;
import java.text.Collator;

final class PhoneContacts {
    /** Bounded PBAP requests per HFP session; never reset an active transfer. */
    static final class AutoSync {
        private String device="";
        private int attempts;
        private long next;
        boolean due(String selected,boolean connected,int state,long now) {
            if(!connected || selected==null || selected.isEmpty()) { device="";attempts=0;next=0;return false; }
            if(!device.equals(selected)) { device=selected;attempts=0;next=0; }
            if(state!=0 || attempts>=3 || now<next)return false;
            attempts++;next=now+(attempts==1?5000:15000);return true;
        }
        void manual(String selected,long now) { device=selected;attempts=0;next=now+30000; }
    }
    static boolean autoTarget(String requested,String selected,boolean connected,long age) {
        return connected && requested!=null && !requested.isEmpty() && requested.equals(selected) && age>=0 && age<=5000;
    }
    static final class Entry {
        final String name,number;
        Entry(String name,String number) { this.name=name==null || name.trim().isEmpty()?"未命名联系人":name;this.number=number==null?"":number; }
    }
    static List<Entry> normalize(List<Entry> raw) {
        ArrayList<Entry> result=new ArrayList<>();Set<String> keys=new HashSet<>();
        for(Entry e:raw) {
            String n=BluetoothCalls.number(e.number);if(n==null)continue;
            if(keys.add(e.name+"\u0000"+n))result.add(new Entry(e.name,n));
        }
        Collator collator=Collator.getInstance(Locale.CHINA);
        Collections.sort(result,(a,b)->{int c=collator.compare(a.name,b.name);return c!=0?c:a.number.compareTo(b.number);});
        return result;
    }
    static List<Entry> filter(List<Entry> all,String query) {
        String q=query.trim().toLowerCase(Locale.ROOT);ArrayList<Entry> result=new ArrayList<>();
        for(Entry e:all)if(e.name.toLowerCase(Locale.ROOT).contains(q) || e.number.contains(q))result.add(e);return result;
    }
    static int pageCount(int count,int size) { return Math.max(1,(count+size-1)/size); }
    static int pageIndex(int page,int count) { return Math.max(0,Math.min(page,count-1)); }
    /** Android BluetoothProfile states; null leaves the short asynchronous request message. */
    static String syncStatus(int state,long elapsed,boolean wasConnected) {
        if(state==2)return "手机通讯录通道已连接";
        if(state==1)return elapsed>=30000?"通讯录连接超时，请检查手机授权后重试":"正在同步，请在手机允许共享联系人";
        if(state==3)return "通讯录通道正在断开";
        if(wasConnected)return "手机通讯录通道已断开，请重新同步";
        return elapsed>=3000?"通讯录连接未建立，请检查手机配对和联系人授权后重试":null;
    }
}
