package com.carplaybox.audio;
import java.util.*;
public final class PhoneContactsTest {
    static void check(boolean ok,String why) { if(!ok)throw new AssertionError(why); }
    public static void main(String[] args) {
        List<PhoneContacts.Entry> items=Arrays.asList(new PhoneContacts.Entry("张三","123 4567"),new PhoneContacts.Entry("Alice","7654321"),new PhoneContacts.Entry("张三","1234567"),new PhoneContacts.Entry("Service","*#06#"));
        List<PhoneContacts.Entry> unique=PhoneContacts.normalize(items);
        check(unique.size()==2,"deduplicate formatted numbers and reject service code");
        check(PhoneContacts.filter(unique,"张").size()==1,"Chinese name search");
        check(PhoneContacts.filter(unique,"ALICE").size()==1,"case independent name search");
        check(PhoneContacts.filter(unique,"1234").size()==1,"number search");
        check(PhoneContacts.filter(unique,"").size()==2,"empty search");
        check(PhoneContacts.pageCount(0,6)==1 && PhoneContacts.pageCount(7,6)==2,"bounded pages");
        check(PhoneContacts.pageIndex(9,2)==1 && PhoneContacts.pageIndex(-1,2)==0,"clamp page after refresh");
        check(new PhoneContacts.Entry(null,"1234567").name.equals("未命名联系人"),"null name");
        check(PhoneContacts.syncStatus(0,1000,false)==null,"async connect gets a brief startup window");
        check(PhoneContacts.syncStatus(0,4000,false).contains("未建立"),"rejected connection cannot stay requested forever");
        check(PhoneContacts.syncStatus(1,31000,false).contains("超时"),"connecting has a deadline");
        check(PhoneContacts.syncStatus(2,40000,false).contains("已连接"),"late successful connection takes precedence");
        check(PhoneContacts.syncStatus(0,2000,true).contains("已断开"),"established connection loss is shown");
        PhoneContacts.AutoSync auto=new PhoneContacts.AutoSync();
        check(!auto.due("A",false,0,0),"offline phone must not trigger PBAP");
        check(auto.due("A",true,0,1000),"first HFP connection triggers automatic PBAP");
        check(!auto.due("A",true,0,5999),"no reconnect storm before retry delay");
        check(auto.due("A",true,0,6000),"failed connection retries after backoff");
        check(!auto.due("A",true,1,21000),"connecting PBAP must not be restarted");
        check(!auto.due("A",true,2,21000),"connected PBAP must not be refreshed automatically");
        check(!auto.due("A",true,3,21000),"disconnecting PBAP must finish first");
        check(auto.due("A",true,0,21000),"third bounded connection request");
        check(!auto.due("A",true,0,60000),"stop after three failed requests");
        check(!auto.due("A",false,0,61000),"disconnection resets without requesting");
        check(auto.due("A",true,0,62000),"next connection starts a new sync session");
        check(auto.due("B",true,0,63000),"new selected phone gets independent session");
        auto.manual("B",64000);
        check(!auto.due("B",true,0,93999),"manual refresh suppresses automatic competition");
        check(auto.due("B",true,0,94000),"bounded recovery after manual refresh grace period");
        check(!auto.due("",true,0,100000),"no target must never trigger a request");
        check(PhoneContacts.autoTarget("A","A",true,0),"fresh connected selected target allowed");
        check(!PhoneContacts.autoTarget("A","B",true,0),"late PBAP callback cannot connect former phone");
        check(!PhoneContacts.autoTarget("A","A",false,0),"late callback after HFP disconnect is cancelled");
        check(!PhoneContacts.autoTarget("A","A",true,5001),"stale HFP snapshot cannot trigger sync");
        check(!PhoneContacts.autoTarget("A","A",true,-1),"future snapshot is invalid");
        System.out.println("Phone contacts: PASS");
    }
}
