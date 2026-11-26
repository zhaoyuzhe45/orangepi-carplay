package com.carplaybox.audio;
import java.util.List;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;

/** Platform-independent rules for a deliberately single-call controller. */
final class BluetoothCalls {
    static final class Reconnect {
        private long next,delay=5000;
        boolean due(boolean enabled,boolean connected,boolean connecting,long now) {
            if(!enabled || connected) { next=0;delay=5000;return false; }
            if(connecting || now<next)return false;
            next=now+delay;delay=Math.min(60000,delay*2);return true;
        }
    }
    static final class Call {
        final String uuid, number;
        final int state;
        final boolean outgoing;
        Call(String uuid,int state,boolean outgoing,String number) {
            this.uuid=uuid;this.state=state;this.outgoing=outgoing;
            this.number=number==null?"":number;
        }
        String label() {
            String[] states={"通话中","保持","正在拨号","对方振铃","来电","呼叫等待","保持待接","已结束"};
            return state>=0 && state<states.length?states[state]:"未知状态";
        }
    }
    static String number(String raw) {
        if(raw==null || raw.length()>80)return null;
        StringBuilder out=new StringBuilder();int digits=0;
        for(int i=0;i<raw.length();i++) {
            char c=raw.charAt(i);
            if(c>='0' && c<='9') { out.append(c);digits++; }
            else if(c=='+' && out.length()==0)out.append(c);
            else if(c!=' ' && c!='-' && c!='(' && c!=')')return null;
        }
        return digits>=3 && digits<=30?out.toString():null;
    }
    static int iapState(int state) {
        switch(state) {
            case 0:return 4;case 1:case 6:return 5;case 2:return 1;
            case 3:return 3;case 4:case 5:return 2;case 7:return 0;
            default:return -1;
        }
    }
    static byte[] encode(List<Call> calls,long sampled) throws java.io.IOException {
        if(calls==null || calls.size()>8 || sampled<0)throw new java.io.IOException("invalid calls");
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.writeInt(0x43504231);out.writeLong(sampled);out.writeByte(calls.size());
        java.util.HashSet<String> ids=new java.util.HashSet<>();
        for(Call call:calls) {
            if(call.uuid==null || !call.uuid.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}") ||
               !ids.add(call.uuid) || iapState(call.state)<0)throw new java.io.IOException("invalid call");
            out.write(call.uuid.getBytes(java.nio.charset.StandardCharsets.US_ASCII));
            out.writeByte(iapState(call.state));out.writeByte(call.outgoing?2:1);
            StringBuilder number=new StringBuilder();
            for(int i=0;i<call.number.length() && number.length()<64;i++) {
                char c=call.number.charAt(i);if(c>=32 && c<=126)number.append(c);
            }
            byte[] raw=number.toString().getBytes(java.nio.charset.StandardCharsets.US_ASCII);
            out.writeByte(raw.length);out.write(raw);
        }
        out.flush();return bytes.toByteArray();
    }
    static boolean allowed(String action,String uuid,List<Call> calls) {
        if(uuid==null || calls==null || calls.size()!=1)return false;
        Call call=calls.get(0);
        if(!uuid.equals(call.uuid))return false;
        if("answer".equals(action) || "reject".equals(action))return call.state==4;
        return "hangup".equals(action) && call.state>=0 && call.state<=3;
    }
}
