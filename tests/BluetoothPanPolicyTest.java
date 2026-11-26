package com.carplaybox.audio;

public final class BluetoothPanPolicyTest {
    private static void check(boolean value,String reason) {
        if(!value)throw new AssertionError(reason);
    }
    public static void main(String[] args) {
        String a="AA:BB:CC:DD:EE:01",b="AA:BB:CC:DD:EE:02";
        check(a.equals(BluetoothPanPolicy.target(a,new String[]{a,b},new String[]{b})),"explicit paired phone wins");
        check("".equals(BluetoothPanPolicy.target(a,new String[]{b},new String[]{b})),"unpaired selection must not switch phones");
        check(a.equals(BluetoothPanPolicy.target("",new String[]{a,b},new String[]{a})),"one NAP phone can be selected");
        check("".equals(BluetoothPanPolicy.target("",new String[]{a,b},new String[]{a,b})),"ambiguous phones require selection");
        check("".equals(BluetoothPanPolicy.target("",new String[]{a},new String[]{})),"do not guess a device without NAP");
        check("".equals(BluetoothPanPolicy.target("",new String[]{b},new String[]{a})),"only paired peers may be selected");
        BluetoothPanPolicy p=new BluetoothPanPolicy();
        check(p.next(true,0,0)==BluetoothPanPolicy.CONNECT,"boot starts without waiting");
        check(p.next(true,0,4999)==BluetoothPanPolicy.NONE,"first backoff");
        check(p.next(true,0,5000)==BluetoothPanPolicy.CONNECT,"retry after 5 seconds");
        check(p.next(true,0,14999)==BluetoothPanPolicy.NONE,"second backoff");
        check(p.next(true,0,15000)==BluetoothPanPolicy.CONNECT,"retry after 10 seconds");
        check(p.next(true,2,15001)==BluetoothPanPolicy.NONE,"connected must not reconnect");
        check(p.next(true,0,15002)==BluetoothPanPolicy.CONNECT,"disconnect resets retry");
        check(p.next(true,1,15003)==BluetoothPanPolicy.NONE,"one connection at a time");
        check(p.next(true,1,35002)==BluetoothPanPolicy.NONE,"connection gets full timeout");
        check(p.next(true,1,35003)==BluetoothPanPolicy.DISCONNECT,"cancel stuck connection after 20 seconds");
        check(p.next(true,1,35004)==BluetoothPanPolicy.NONE,"do not spam timeout cancellation");
        check(p.next(false,2,35005)==BluetoothPanPolicy.DISCONNECT,"manual disable disconnects");
        check(p.next(false,2,35006)==BluetoothPanPolicy.NONE,"manual disable is bounded");
        check(p.next(false,0,35007)==BluetoothPanPolicy.NONE,"manual disable prevents reconnect");
        check(p.next(true,0,35008)==BluetoothPanPolicy.CONNECT,"manual reenable is immediate");
        p.reset();
        long now=0;
        for(int i=0;i<15;i++) {
            check(p.next(true,0,now)==BluetoothPanPolicy.CONNECT,"retry remains possible");
            now+=60000;
        }
        check(p.next(true,0,now-1)==BluetoothPanPolicy.NONE,"backoff capped at 60 seconds");
        p.reset();check(p.next(true,0,0)==BluetoothPanPolicy.CONNECT,"new target resets old retry state");
        p.reset();
        check(p.next(true,true,0,0)==BluetoothPanPolicy.NONE,"usable Wi-Fi leaves PAN on standby at boot");
        check(p.next(true,true,2,1)==BluetoothPanPolicy.DISCONNECT,"Wi-Fi recovery releases connected PAN");
        check(p.next(true,true,2,2)==BluetoothPanPolicy.NONE,"Wi-Fi release is bounded");
        check(p.next(true,true,1,5001)==BluetoothPanPolicy.DISCONNECT,"Wi-Fi cancels connecting PAN too");
        check(p.next(true,true,0,7000)==BluetoothPanPolicy.NONE,"Wi-Fi prevents PAN reconnect");
        check(p.next(true,false,0,7001)==BluetoothPanPolicy.CONNECT,"Wi-Fi loss starts fallback immediately");
        check(p.next(true,false,0,7002)==BluetoothPanPolicy.NONE,"fallback failure backs off");
        check(p.next(true,true,0,7003)==BluetoothPanPolicy.NONE,"Wi-Fi recovery interrupts fallback backoff");
        check(p.next(true,false,0,7004)==BluetoothPanPolicy.CONNECT,"second Wi-Fi loss resets stale backoff");
        check(p.next(false,false,2,7005)==BluetoothPanPolicy.DISCONNECT,"disabled fallback disconnects without Wi-Fi");
        check(p.next(false,true,0,7006)==BluetoothPanPolicy.NONE,"disabled fallback with Wi-Fi stays off");
        check(p.next(false,false,0,7007)==BluetoothPanPolicy.NONE,"Wi-Fi loss never overrides manual disable");
        check(p.next(true,false,0,7008)==BluetoothPanPolicy.CONNECT,"reenabled fallback starts immediately without Wi-Fi");
        System.out.println("Bluetooth PAN target/backoff/disable/timeout/Wi-Fi preference tests: PASS");
    }
}
