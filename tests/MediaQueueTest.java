package com.carplaybox.audio;
import java.util.*;
public final class MediaQueueTest {
 static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
 static MediaQueue.Entry e(long id,String title){return new MediaQueue.Entry(id,title,"album","artist");}
 public static void main(String[] args)throws Exception{
  MediaQueue q=new MediaQueue();Object session=new Object();List<MediaQueue.Entry> a=Arrays.asList(e(42,"A"),e(700,"B"));
  q.publish(session,a,true,100);long v=q.version();check(v!=0 && q.count()==2,"available queue");
  check(q.select(v,1,101)==700,"queue id distinct from index");q.publish(session,a,true,102);check(v==q.version(),"stable refresh");
  q.publish(session,Arrays.asList(a.get(1),a.get(0)),true,103);check(q.select(v,1,104)==-1,"reject old order");
  v=q.version();check(q.select(v,1,104)==42,"new order");
  q.publish(new Object(),Arrays.asList(a.get(1),a.get(0)),true,105);check(q.select(v,1,106)==-1,"reject old session");
  v=q.version();check(q.select(v,-1,106)==-1 && q.select(v,2,106)==-1,"index bounds");
  check(q.select(v,0,6000)==-1 && q.select(v,0,100)==-1,"stale/future snapshot");
  q.publish(session,Arrays.asList(e(5,"x"),e(5,"x")),true,110);check(q.count()==0,"duplicate ids rejected");
  q.publish(session,a,false,111);check(q.count()==0,"unsupported selection unavailable");
  q.publish(session,a,true,112);v=q.version();check(q.page(v,1,1,113).get(0).id==700,"last page");
  check(q.page(v,2,1,113).isEmpty() && q.page(v,-1,1,113).isEmpty(),"page bounds");
  a=new ArrayList<>();for(int i=0;i<10000;i++)a.add(e(i,"track"+i));q.publish(session,a,true,114);
  check(q.count()==10000,"full large queue");check(q.page(q.version(),9990,100,115).size()==10,"bounded tail");
  a.add(e(10001,"excess"));q.publish(session,a,true,116);check(q.count()==0,"no silent truncation");
  q.publish(null,null,false,117);check(q.count()==0,"session removal");
  System.out.println("Media queue session/version/order/bounds/freshness: PASS");
 }
}
