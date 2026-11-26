package com.carplaybox.encoder;
import java.nio.ByteBuffer;
import java.io.ByteArrayOutputStream;
final class AnnexB {
    static final class Configuration { final byte[] data; final int lengthSize;
        Configuration(byte[] data,int lengthSize) { this.data=data; this.lengthSize=lengthSize; }
    }
    static byte[] normalize(ByteBuffer input,int lengthSize) {
        validateLengthSize(lengthSize);
        byte[] data = copy(input);
        // 00 00 01 xx can be a 4-byte AVCC length (256..511), so test
        // a complete bounded AVCC sequence before treating it as a start code.
        if(validAvcc(data,lengthSize)) return fromAvcc(data,lengthSize);
        if (startCode(data,0) != 0) {
            int position=0;
            while(position < data.length) {
                int prefix=startCode(data,position);
                if(prefix==0) throw bad("missing Annex B prefix");
                int first=position+prefix;
                int end=first;
                while(end<data.length && startCode(data,end)==0) end++;
                validateNal(data,first,end-first);
                position=end;
            }
            return data;
        }
        return fromAvcc(data,lengthSize);
    }
    private static byte[] fromAvcc(byte[] data,int lengthSize) {
        ByteArrayOutputStream output = new ByteArrayOutputStream(data.length);
        int position=0;
        while(position<data.length) {
            if(data.length-position<lengthSize) throw bad("truncated AVCC length");
            long length=0;
            for(int i=0;i<lengthSize;i++) length=(length<<8)|(data[position++]&255);
            if(length<=0 || length>data.length-position) throw bad("invalid AVCC NAL length");
            append(output,data,position,(int)length);
            position+=(int)length;
        }
        return output.toByteArray();
    }
    private static boolean validAvcc(byte[] data,int lengthSize) {
        int position=0;
        long normalizedSize=0;
        while(position<data.length) {
            if(data.length-position<lengthSize) return false;
            long length=0;
            for(int i=0;i<lengthSize;i++) length=(length<<8)|(data[position++]&255);
            if(length<=0 || length>data.length-position) return false;
            int header=data[position]&255,type=header&31;
            if((header&128)!=0 || type==0 || type>23) return false;
            normalizedSize+=4+length;
            if(normalizedSize>PacketWriter.MAX_PAYLOAD) return false;
            position+=(int)length;
        }
        return true;
    }
    static Configuration configuration(ByteBuffer input,int lengthSize) {
        byte[] data=copy(input);
        if(data[0]!=1) return new Configuration(normalize(ByteBuffer.wrap(data),lengthSize),lengthSize);
        if(data.length<7 || (data[4]&252)!=252 || (data[5]&224)!=224) throw bad("invalid avcC header");
        int encodedLength=(data[4]&3)+1;
        validateLengthSize(encodedLength);
        int position=6, count=data[5]&31;
        if(count==0) throw bad("avcC has no SPS");
        ByteArrayOutputStream output=new ByteArrayOutputStream(data.length);
        for(int group=0;group<2;group++) {
            for(int i=0;i<count;i++) {
                if(data.length-position<2) throw bad("truncated avcC NAL length");
                int length=((data[position]&255)<<8)|(data[position+1]&255); position+=2;
                if(length==0 || length>data.length-position) throw bad("invalid avcC NAL length");
                if((data[position]&31)!=(group==0?7:8)) throw bad("unexpected avcC NAL type");
                append(output,data,position,length); position+=length;
            }
            if(group==0) {
                if(position==data.length) throw bad("avcC has no PPS count");
                count=data[position++]&255;
                if(count==0) throw bad("avcC has no PPS");
            }
        }
        // Baseline is required, so extended high-profile avcC records are not accepted.
        if(position!=data.length) throw bad("trailing avcC bytes");
        return new Configuration(output.toByteArray(),encodedLength);
    }
    private static byte[] copy(ByteBuffer input) {
        if(input==null || !input.hasRemaining() || input.remaining()>PacketWriter.MAX_PAYLOAD) throw bad("invalid NAL buffer size");
        ByteBuffer source=input.duplicate(); byte[] data=new byte[source.remaining()]; source.get(data); return data;
    }
    private static void validateLengthSize(int lengthSize) {
        if(lengthSize!=1 && lengthSize!=2 && lengthSize!=4) throw bad("invalid AVCC length size");
    }
    private static int startCode(byte[] data,int offset) {
        if(offset+3<=data.length && data[offset]==0 && data[offset+1]==0) {
            if(data[offset+2]==1) return 3;
            if(offset+4<=data.length && data[offset+2]==0 && data[offset+3]==1) return 4;
        }
        return 0;
    }
    private static void validateNal(byte[] data,int offset,int length) {
        if(length<=0 || (data[offset]&128)!=0 || (data[offset]&31)==0 || (data[offset]&31)>23) throw bad("invalid NAL header");
    }
    private static void append(ByteArrayOutputStream out,byte[] data,int offset,int length) {
        validateNal(data,offset,length);
        if((long)out.size()+4+length>PacketWriter.MAX_PAYLOAD) throw bad("Annex B payload too large");
        out.write(0);out.write(0);out.write(0);out.write(1);out.write(data,offset,length);
    }
    private static IllegalArgumentException bad(String message) { return new IllegalArgumentException(message); }
}
