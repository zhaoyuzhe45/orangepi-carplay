package com.carplaybox.audio;
import java.util.*;
public final class PhoneVehicleKeysTest {
    static void check(boolean ok,String why) { if(!ok)throw new AssertionError(why); }
    public static void main(String[] args) {
        List<BluetoothCalls.Call> empty=Collections.emptyList();
        check("open".equals(PhoneVehicleKeys.action(1,true,true,empty)),"idle opens phone, never redials");
        check(PhoneVehicleKeys.action(2,true,true,empty)==null,"idle drop ignored");
        BluetoothCalls.Call incoming=new BluetoothCalls.Call("one",4,false,"");
        BluetoothCalls.Call active=new BluetoothCalls.Call("two",0,false,"");
        check("answer".equals(PhoneVehicleKeys.action(1,true,true,Arrays.asList(incoming))),"hook answers incoming");
        check("reject".equals(PhoneVehicleKeys.action(2,true,true,Arrays.asList(incoming))),"drop rejects incoming");
        check("hangup".equals(PhoneVehicleKeys.action(1,true,true,Arrays.asList(active))),"hook ends active");
        check("hangup".equals(PhoneVehicleKeys.action(2,true,true,Arrays.asList(active))),"drop ends active");
        check(PhoneVehicleKeys.action(1,true,true,Arrays.asList(incoming,active))==null,"ambiguous multi-call ignored");
        check(PhoneVehicleKeys.action(3,true,true,Arrays.asList(incoming))==null,"simultaneous keys ignored");
        check(PhoneVehicleKeys.action(1,true,false,Arrays.asList(incoming))==null,"unknown state ignored");
        check(PhoneVehicleKeys.action(1,false,true,Arrays.asList(incoming))==null,"disconnected stale call ignored");
        check(PhoneVehicleKeys.fresh(1000,2000),"fresh");
        check(!PhoneVehicleKeys.fresh(1000,2600) && !PhoneVehicleKeys.fresh(1000,999),"stale and future rejected");
        System.out.println("Vehicle telephone key policy tests: PASS");
    }
}
