package com.carplaybox.audio;
import android.app.Activity;
import android.os.Handler;
import android.widget.LinearLayout;
import android.widget.TextView;
/** Console summary only. All telephone actions live in the separate phone app. */
final class BluetoothPhonePage {
    private final LinearLayout root;
    private final TextView status;
    private final TextView networkStatus;
    private final android.widget.Button networkToggle;
    private final Handler handler=new Handler();
    private boolean active;
    BluetoothPhonePage(Activity a) {
        root=Ui.vertical(a);root.addView(Ui.section(a,"手机电话"),Ui.matchWrap());
        status=Ui.text(a,"正在读取连接状态",16,Ui.TEXT);root.addView(status,Ui.matchWrap());
        root.addView(Ui.text(a,"通讯录、拨号和来电操作已统一到独立的电话应用。",15,Ui.MUTED),Ui.matchWrap());
        root.addView(Ui.button(a,"打开电话",v->{try{a.startActivity(PhoneBridgeContract.openPhone());}catch(RuntimeException e){status.setText("请先安装车载电话应用");}}),Ui.matchWrap());
        root.addView(Ui.section(a,"手机蓝牙备用网络"),Ui.matchWrap());
        root.addView(Ui.text(a,"Wi-Fi 优先；Wi-Fi 无法联网时自动使用手机蓝牙共享。",15,Ui.MUTED),Ui.matchWrap());
        networkStatus=Ui.text(a,"正在读取网络状态",15,Ui.TEXT);root.addView(networkStatus,Ui.matchWrap());
        networkToggle=Ui.button(a,"关闭蓝牙备用上网",v->{
            BluetoothNetworkBridge b=BluetoothNetworkBridge.current();
            if(b!=null)b.setEnabled(!b.enabled());
        });root.addView(networkToggle,Ui.matchWrap());
    }
    android.view.View view(){return root;}
    void activate(){active=true;handler.removeCallbacks(poll);poll.run();}
    void deactivate(){active=false;handler.removeCallbacksAndMessages(null);}
    private final Runnable poll=new Runnable(){public void run(){
        if(!active)return;
        BluetoothCallBridge b=BluetoothCallBridge.current();status.setText(b==null?"电话服务未就绪":b.state().status);
        BluetoothNetworkBridge network=BluetoothNetworkBridge.current();
        networkStatus.setText(network==null?"网络服务未就绪":network.state().status);
        networkToggle.setEnabled(network!=null);
        networkToggle.setText(network!=null && network.enabled()?"关闭蓝牙备用上网":"开启蓝牙备用上网");
        handler.postDelayed(this,1000);
    }};
}
