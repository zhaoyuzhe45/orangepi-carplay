package com.carplaybox.audio;
import android.content.Context;
import android.os.Bundle;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/** UI snapshots are cached; Binder work and commands never block the rotary/UI thread. */
interface PhoneBackend {
    PhoneState state();
    String command(String action,String device,String argument);
    String tone(String device,String uuid,String digit);
    String connect(String device);
    default String consumeMessage(){return null;}
    Remote LIVE=new Remote();
    final class Remote implements PhoneBackend {
        private final ScheduledExecutorService worker=Executors.newSingleThreadScheduledExecutor();
        private final AtomicReference<String> message=new AtomicReference<>();
        private volatile Context context;
        private volatile PhoneState snapshot;
        private ScheduledFuture<?> poll;
        private int clients;
        synchronized void start(Context c) {
            context=c.getApplicationContext();if(clients++>0)return;snapshot=null;
            worker.execute(()->{try{call("start",new Bundle());}catch(RuntimeException e){message.set("请先安装或更新 CarPlay 控制台");}});
            poll=worker.scheduleWithFixedDelay(()->{try{snapshot=PhoneBridgeContract.decode(call("state",new Bundle()));}catch(RuntimeException e){snapshot=null;}},0,500,TimeUnit.MILLISECONDS);
        }
        synchronized void stop(){if(clients>0 && --clients==0){if(poll!=null)poll.cancel(false);snapshot=null;}}
        Bundle call(String method,Bundle args){Context c=context;if(c==null)throw new IllegalStateException("Phone backend not started");return c.getContentResolver().call(PhoneBridgeContract.URI,method,null,args);}
        public PhoneState state(){return snapshot;}
        public String consumeMessage(){return message.getAndSet(null);}
        private String submit(String method,String device,String value,String uuid){
            Bundle args=new Bundle();args.putString("device",device);args.putString("value",value);args.putString("uuid",uuid);
            worker.execute(()->{try{Bundle out=call(method,args);message.set(out==null?"服务未返回结果，不会自动重试":out.getString("result","请求已提交"));}
                catch(RuntimeException e){message.set("请求结果未确认，请查看手机；不会自动重试");}});
            return "请求已提交，等待手机状态更新";
        }
        public String command(String a,String d,String v){return submit(a,d,v,"");}
        public String connect(String d){return submit("connect",d,"","");}
        public String tone(String d,String id,String digit){return submit("tone",d,digit,id);}
        String sync(String device){return submit("sync",device,"","");}
        List<PhoneContacts.Entry> contacts(String device){
            if(device.isEmpty())return Collections.emptyList();ArrayList<PhoneContacts.Entry> rows=new ArrayList<>();int offset=0;
            while(offset<100000){Bundle args=new Bundle();args.putString("device",device);args.putInt("offset",offset);Bundle data=call("contacts",args);
                if(data==null)throw new IllegalStateException("Contact service did not respond");
                ArrayList<String> names=data.getStringArrayList("names"),numbers=data.getStringArrayList("numbers");
                if(names==null || numbers==null || names.size()!=numbers.size())throw new IllegalStateException("Invalid contact page");
                for(int i=0;i<names.size();i++)rows.add(new PhoneContacts.Entry(names.get(i),numbers.get(i)));
                offset+=names.size();if(names.isEmpty() || offset>=data.getInt("total"))break;
            }
            return rows;
        }
    }
}
