package com.carplaybox.audio;

import java.util.*;
import java.nio.charset.StandardCharsets;

/** Immutable queue snapshots; version includes session identity, order and displayed content. */
final class MediaQueue {
    static final class Entry {
        final long id;final String title,album,artist;
        Entry(long id,String title,String album,String artist){this.id=id;this.title=clean(title);this.album=clean(album);this.artist=clean(artist);}
        @Override public boolean equals(Object other){if(!(other instanceof Entry))return false;Entry e=(Entry)other;return id==e.id && title.equals(e.title) && album.equals(e.album) && artist.equals(e.artist);}
        @Override public int hashCode(){return Long.valueOf(id).hashCode();}
        private static String clean(String text){
            byte[] b=(text==null?"":text.replace("\u0000","")).getBytes(StandardCharsets.UTF_8);
            int n=Math.min(255,b.length);if(n<b.length)while(n>0 && (b[n]&0xc0)==0x80)n--;
            return new String(b,0,n,StandardCharsets.UTF_8);
        }
    }
    private Object session;
    private List<Entry> entries=Collections.emptyList();
    private long version=new java.security.SecureRandom().nextLong(),sampled;
    synchronized void publish(Object token,List<Entry> items,boolean canSelect,long now){
        List<Entry> next=Collections.emptyList();
        if(token!=null && canSelect && items!=null && items.size()<=10000){
            HashSet<Long> ids=new HashSet<>();boolean valid=true;
            for(Entry e:items)if(e==null || e.id==-1 || !ids.add(e.id)){valid=false;break;}
            if(valid)next=Collections.unmodifiableList(new ArrayList<>(items));
        }
        if(!Objects.equals(token,session) || !next.equals(entries)){
            version++;if(version==0)version++;session=token;entries=next;
        }
        sampled=now;
    }
    synchronized long version(){return version;}
    synchronized int count(){return entries.size();}
    synchronized long sampled(){return sampled;}
    private boolean fresh(long expected,long now){return expected==version && now>=sampled && now-sampled<=5000;}
    synchronized long select(long expected,int index,long now){return fresh(expected,now) && index>=0 && index<entries.size()?entries.get(index).id:-1;}
    synchronized List<Entry> page(long expected,int start,int count,long now){
        if(!fresh(expected,now) || start<0 || start>=entries.size() || count<=0)return Collections.emptyList();
        return new ArrayList<>(entries.subList(start,start+Math.min(count,entries.size()-start)));
    }
}
