package com.carplaybox.audio;
import android.app.Instrumentation;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.os.Bundle;

/** Explicit maintenance action; never touches telephone call commands. */
public final class PhonePairingDeviceTest extends Instrumentation {
    private String action;
    @Override public void onCreate(Bundle args) { super.onCreate(args);action=args.getString("action","inspect");start(); }
    @Override public void onStart() {
        Bundle out=new Bundle();
        try {
            BluetoothAdapter adapter=BluetoothAdapter.getDefaultAdapter();
            out.putInt("scanModeBefore",adapter.getScanMode());out.putString("name",adapter.getName());
            if(action.equals("restart")) {
                out.putBoolean("disableQueued",adapter.disable());
                for(int i=0;i<80 && adapter.getState()!=BluetoothAdapter.STATE_OFF;i++)android.os.SystemClock.sleep(100);
                if(adapter.getState()!=BluetoothAdapter.STATE_OFF)throw new IllegalStateException("Bluetooth did not turn off");
                out.putBoolean("enableQueued",adapter.enable());
                for(int i=0;i<100 && !adapter.isEnabled();i++)android.os.SystemClock.sleep(100);
                if(!adapter.isEnabled())throw new IllegalStateException("Bluetooth did not turn on");
            }
            if(action.equals("repair-y70")) {
                BluetoothDevice device=adapter.getRemoteDevice("7C:A0:5E:B6:F3:0D");
                out.putInt("bondBefore",device.getBondState());
                if(device.getBondState()==BluetoothDevice.BOND_BONDED)
                    out.putString("forgetOldBond",String.valueOf(device.getClass().getMethod("removeBond").invoke(device)));
                out.putString("discoverable",String.valueOf(adapter.getClass().getMethod("setScanMode",int.class,int.class).invoke(adapter,23,180)));
            }
            if(action.equals("pair-y70")) {
                adapter.cancelDiscovery();
                BluetoothDevice device=adapter.getRemoteDevice("7C:A0:5E:B6:F3:0D");
                out.putInt("bondBefore",device.getBondState());
                out.putString("pairQueued",String.valueOf(device.getClass().getMethod("createBond",int.class).invoke(device,1)));
            }
            out.putInt("scanModeAfter",adapter.getScanMode());finish(0,out);
        }catch(Throwable e) { out.putString("error",e.toString());finish(-1,out); }
    }
}
