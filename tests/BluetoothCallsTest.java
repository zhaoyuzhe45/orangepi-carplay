package com.carplaybox.audio;
import java.util.*;
public final class BluetoothCallsTest {
    private static void check(boolean yes,String why) { if(!yes)throw new AssertionError(why); }
    public static void main(String[] args) throws Exception {
        check("+8613800138000".equals(BluetoothCalls.number("+86 138-0013-8000")),"ordinary number");
        for(String bad:new String[]{""," ","*#06#","123;456","123,456","ATD123","12+345","１２３","12\n345","+","1"})
            check(BluetoothCalls.number(bad)==null,"unsafe dial: "+bad);
        int[] expected={4,5,1,3,2,2,5,0};
        for(int i=0;i<8;i++)check(BluetoothCalls.iapState(i)==expected[i],"state "+i);
        check(BluetoothCalls.iapState(8)==-1,"unknown not fabricated");
        BluetoothCalls.Call ringing=new BluetoothCalls.Call("uuid-a",4,false,"13800138000");
        BluetoothCalls.Call active=new BluetoothCalls.Call("uuid-b",0,true,"");
        List<BluetoothCalls.Call> calls=Arrays.asList(ringing);
        check(BluetoothCalls.allowed("answer","uuid-a",calls),"incoming answer");
        check(BluetoothCalls.allowed("reject","uuid-a",calls),"incoming reject");
        check(!BluetoothCalls.allowed("hangup","uuid-a",calls),"ringing is reject");
        check(!BluetoothCalls.allowed("answer","uuid-stale",calls),"stale target");
        check(!BluetoothCalls.allowed("answer","uuid-a",Arrays.asList(ringing,active)),"do not disrupt second call");
        check(BluetoothCalls.allowed("hangup","uuid-b",Arrays.asList(active)),"active hangup");
        check(!BluetoothCalls.allowed("reject","uuid-b",Arrays.asList(active)),"active not reject");
        check(!BluetoothCalls.allowed("hangup","uuid-b",Collections.emptyList()),"disconnect clears target");
        check(!BluetoothCalls.allowed("hangup","uuid-b",Arrays.asList(active,active)),"duplicate ID rejected");
        check(!BluetoothCalls.allowed("answer","uuid-a",Arrays.asList(new BluetoothCalls.Call("uuid-a",5,false,""))),"waiting unsupported");
        BluetoothCalls.Call real=new BluetoothCalls.Call("01234567-89ab-cdef-0123-456789abcdef",0,false,"123");
        byte[] encoded=BluetoothCalls.encode(Arrays.asList(real),1000);
        check(encoded.length==55 && encoded[49]==4 && encoded[50]==1 && encoded[51]==3,"snapshot layout/state");
        check(BluetoothCalls.encode(Collections.emptyList(),1000).length==13,"valid empty calls");
        boolean rejected=false;
        try { BluetoothCalls.encode(Arrays.asList(real,real),1000); }catch(java.io.IOException expectedFailure) { rejected=true; }
        check(rejected,"duplicate UUID rejected");rejected=false;
        try { BluetoothCalls.encode(Arrays.asList(active),1000); }catch(java.io.IOException expectedFailure) { rejected=true; }
        check(rejected,"malformed UUID rejected");
        BluetoothCalls.Reconnect retry=new BluetoothCalls.Reconnect();
        check(!retry.due(false,false,false,0),"disabled reconnect");
        check(retry.due(true,false,false,0),"first reconnect");
        check(!retry.due(true,false,false,4999),"retry backoff");
        check(!retry.due(true,false,true,5000),"in-flight connect");
        check(retry.due(true,false,false,5000),"second retry");
        check(!retry.due(true,false,false,14999),"exponential retry");
        check(!retry.due(true,true,false,15000),"connected does not reconnect");
        check(retry.due(true,false,false,15001),"disconnect after connection resets delay");
        System.out.println("Bluetooth call validation/state/target tests: PASS");
    }
}
