package com.carplaybox.audio;
import java.util.*;
public final class PhoneUiModelTest {
    static int checks;
    static void check(boolean ok,String why) { checks++;if(!ok)throw new AssertionError(why); }
    static BluetoothCalls.Call call(String id,int state) { return new BluetoothCalls.Call(id,state,false,"1234567"); }
    public static void main(String[] args) {
        PhoneUiModel.Incoming gate=new PhoneUiModel.Incoming();
        List<BluetoothCalls.Call> ring=Arrays.asList(call("a",4));
        check(gate.update("phone",true,ring).equals("a"),"first incoming opens");
        check(gate.update("phone",true,ring).isEmpty(),"poll cannot reopen dismissed card");
        check(gate.update("phone",false,Collections.emptyList()).isEmpty(),"unknown cannot fabricate end");
        check(gate.update("phone",true,ring).isEmpty(),"transient disconnect cannot reopen");
        check(gate.update("phone",true,Arrays.asList(call("a",0))).isEmpty(),"active no popup");
        check(gate.update("phone",true,Collections.emptyList()).isEmpty(),"confirmed ended clears");
        check(gate.update("phone",true,ring).equals("a"),"new confirmed lifecycle opens");
        check(gate.update("other",true,ring).equals("a"),"device is part of target");
        check(gate.update("other",true,Arrays.asList(call("b",4),call("a",0))).isEmpty(),"multi call cannot steal focus");
        check(gate.update("other",true,Arrays.asList(call("b",4))).equals("b"),"sole remaining incoming");
        check(!PhoneUiModel.fresh(true,true,100,5101),"stale snapshot");
        check(!PhoneUiModel.fresh(true,true,200,100),"future clock rejected");
        check(!PhoneUiModel.fresh(false,true,100,100),"disconnected");
        check(PhoneUiModel.fresh(true,true,100,5000),"known fresh");
        String n="";for(char c:"+1234567".toCharArray())n=PhoneUiModel.append(n,c);
        check(n.equals("+1234567"),"digits and leading plus");
        check(PhoneUiModel.append(n,'+').equals(n),"embedded plus rejected");
        check(PhoneUiModel.append(n,';').equals(n),"no AT extension");
        for(int i=0;i<40;i++)n=PhoneUiModel.append(n,'1');
        check(n.length()==31,"bound + thirty digits");
        check(PhoneUiModel.dtmf("#"),'#'+" DTMF");
        for(String s:new String[]{"","11","+","A","1;","\n"})check(!PhoneUiModel.dtmf(s),"bad tone");
        check(PhoneUiModel.canTone("a",Arrays.asList(call("a",0))),"active tone");
        check(!PhoneUiModel.canTone("old",Arrays.asList(call("a",0))),"stale tone target");
        check(!PhoneUiModel.canTone("a",ring),"no tone while ringing");
        check(!PhoneUiModel.canTone("a",Arrays.asList(call("a",0),call("b",1))),"ambiguous tone");
        System.out.println("Phone UI model: PASS "+checks+" checks");
    }
}
