package com.carplaybox.audio;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.util.*;
import java.util.concurrent.*;

/** Signature-protected IPC; this process remains the only owner of HFP and PBAP. */
public final class PhoneBridgeProvider extends ContentProvider {
    private final Handler main=new Handler(Looper.getMainLooper());
    @Override public boolean onCreate() { return true; }
    private static Bundle result(String text) { Bundle b=new Bundle();b.putString("result",text);return b; }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        // call() does not automatically enforce read/write permissions on every Android version.
        getContext().enforceCallingOrSelfPermission(PhoneBridgeContract.PERMISSION,"Private phone bridge");
        final String device=extras==null?"":extras.getString("device","");
        final String value=extras==null?"":extras.getString("value","");
        if(device.length()>32 || value.length()>128)throw new IllegalArgumentException("Oversized phone request");
        if("contacts".equals(method)) {
            BluetoothCallBridge bridge=BluetoothCallBridge.current();PhoneState s=bridge==null?null:bridge.state();
            Bundle out=new Bundle();ArrayList<String> names=new ArrayList<>(),numbers=new ArrayList<>();
            int offset=extras==null?0:Math.max(0,extras.getInt("offset",0));
            if(s!=null && !device.isEmpty() && device.equals(s.selected)) {
                List<PhoneContacts.Entry> entries;
                try{entries=ContactsRepository.read(getContext(),device);}
                catch(SecurityException e){throw e;}
                catch(Exception e){throw new IllegalStateException("Cannot read phone contacts",e);}
                int end=(int)Math.min(entries.size(),(long)offset+100);
                for(int i=offset;i<end;i++){names.add(entries.get(i).name);numbers.add(entries.get(i).number);}
                out.putInt("total",entries.size());
            }
            out.putStringArrayList("names",names);out.putStringArrayList("numbers",numbers);return out;
        }
        if(!Arrays.asList("start","state","sync","connect","disconnect","dial","answer","reject","hangup","tone").contains(method))
            throw new IllegalArgumentException("Unknown phone request");
        FutureTask<Bundle> work=new FutureTask<>(()-> {
            if("start".equals(method)){CarPlayServices.start(getContext());return result("电话服务正在启动");}
            BluetoothCallBridge b=BluetoothCallBridge.current();BluetoothContacts c=BluetoothContacts.current();
            if("state".equals(method))return PhoneBridgeContract.encode(b==null?null:b.state(),c==null?"通讯录服务正在启动":c.status());
            if(b==null)return result("电话服务未就绪");
            if("sync".equals(method))return result(c==null?"通讯录服务未就绪":c.sync(device));
            if("connect".equals(method)) { String r=b.select(device);return result(b.state().selected.equals(device)?b.command("connect",device,""):r); }
            if("tone".equals(method))return result(b.tone(device,extras.getString("uuid",""),value));
            return result(b.command(method,device,value));
        });
        if(Looper.myLooper()==Looper.getMainLooper())work.run();else main.post(work);
        try{return work.get(2500,TimeUnit.MILLISECONDS);}
        catch(Exception e){work.cancel(false);return result("请求结果未确认，请查看手机；不会自动重试");}
    }
    @Override public Cursor query(Uri u,String[] p,String s,String[] a,String o){throw new UnsupportedOperationException();}
    @Override public String getType(Uri u){return null;}
    @Override public Uri insert(Uri u,ContentValues v){throw new UnsupportedOperationException();}
    @Override public int delete(Uri u,String s,String[] a){throw new UnsupportedOperationException();}
    @Override public int update(Uri u,ContentValues v,String s,String[] a){throw new UnsupportedOperationException();}
}
