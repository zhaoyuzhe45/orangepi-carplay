package com.carplaybox.encoder;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;

public final class EncoderProtocolTest {
    private static int checks;
    public static void main(String[] args) throws Exception {
        packetHeaderIsBigEndianAndPreservesAccessUnit();
        rejectsInvalidPacketsBeforeWriting();
        normalizesMultipleAvccNalsAndKeepsBufferPosition();
        acceptsThreeAndFourByteAnnexBStartCodes();
        readsAvcConfigurationAndItsLengthSize();
        disambiguatesAvccLengthsThatBeginWithStartCodes();
        rejectsMalformedNalInputs();
        System.out.println("EncoderProtocolTest: " + checks + " checks passed");
    }
    private static void packetHeaderIsBigEndianAndPreservesAccessUnit() throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] nal = bytes(0,0,0,1,0x65,3,4);
        PacketWriter.write(out, PacketWriter.KEYFRAME, 0x0102030405060708L, nal);
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(out.toByteArray()));
        eq(in.readInt(), 0x43504531, "magic"); eq(in.readInt(), 2, "key flag");
        eq(in.readInt(), nal.length, "length"); eq(in.readInt(), 0, "reserved");
        check(in.readLong() == 0x0102030405060708L, "pts64");
        byte[] payload = new byte[nal.length]; in.readFully(payload);
        check(Arrays.equals(payload, nal), "complete access unit"); eq(in.read(), -1, "one packet");
        out.reset(); PacketWriter.write(out, PacketWriter.EOS, 9, new byte[0]);
        eq(out.size(), 24, "empty EOS header");
    }
    private static void rejectsInvalidPacketsBeforeWriting() throws Exception {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        rejects(() -> PacketWriter.write(out, 8, 0, bytes(1)), "unknown flags");
        rejects(() -> PacketWriter.write(out, 0, 0, new byte[0]), "empty frame");
        rejects(() -> PacketWriter.write(out, 1, 0, new byte[PacketWriter.MAX_PAYLOAD + 1]), "oversize");
        eq(out.size(), 0, "no invalid header emitted");
    }
    private static void normalizesMultipleAvccNalsAndKeepsBufferPosition() throws Exception {
        ByteBuffer src = ByteBuffer.wrap(bytes(99,0,0,0,2,0x65,9,0,0,0,3,0x41,5,6,88));
        src.position(1); src.limit(src.capacity()-1);
        byte[] actual = AnnexB.normalize(src, 4);
        same(actual, bytes(0,0,0,1,0x65,9,0,0,0,1,0x41,5,6), "multi NAL");
        eq(src.position(), 1, "position retained");
        same(AnnexB.normalize(ByteBuffer.wrap(bytes(0,2,0x61,7)),2), bytes(0,0,0,1,0x61,7), "2-byte lengths");
        same(AnnexB.normalize(ByteBuffer.wrap(bytes(2,0x61,7)),1), bytes(0,0,0,1,0x61,7), "1-byte lengths");
    }
    private static void acceptsThreeAndFourByteAnnexBStartCodes() throws Exception {
        byte[] nals=bytes(0,0,1,0x67,9,0,0,0,1,0x68,8);
        same(AnnexB.normalize(ByteBuffer.wrap(nals),4), nals, "Annex B preserved");
    }
    private static void readsAvcConfigurationAndItsLengthSize() throws Exception {
        AnnexB.Configuration config = AnnexB.configuration(ByteBuffer.wrap(bytes(1,66,0,30,0xfd,0xe1,0,2,0x67,9,1,0,2,0x68,8)),4);
        eq(config.lengthSize,2,"avcC length size");
        same(config.data,bytes(0,0,0,1,0x67,9,0,0,0,1,0x68,8),"avcC SPS PPS");
        rejects(() -> AnnexB.configuration(ByteBuffer.wrap(bytes(1,66,0,30,0xff,0xe1,0,9,0x67)),4),"truncated avcC");
    }
    private static void rejectsMalformedNalInputs() throws Exception {
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(new byte[0]),4),"empty input");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,0)),4),"truncated length");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,0,0)),4),"zero length");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0xff,0xff,0xff,0xff,0x61)),4),"overflow length");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,0,8,0x61)),4),"overrun");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,0,1)),4),"empty Annex B NAL");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,1,0x61,0,0,1)),4),"empty trailing NAL");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,1,0xe5)),4),"forbidden NAL bit");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(bytes(0,0,1,0x61)),3),"reserved length size");
        rejects(() -> AnnexB.normalize(ByteBuffer.wrap(new byte[PacketWriter.MAX_PAYLOAD+1]),4),"oversize source");
    }
    private static void disambiguatesAvccLengthsThatBeginWithStartCodes() {
        for(int length:new int[]{256,357}) {
            byte[] avcc=new byte[length+4];avcc[2]=(byte)(length>>>8);avcc[3]=(byte)length;avcc[4]=0x61;
            Arrays.fill(avcc,5,avcc.length,(byte)0x22);
            byte[] annex=AnnexB.normalize(ByteBuffer.wrap(avcc),4);
            eq(annex.length,avcc.length,"ambiguous AVCC length "+length);
            check(annex[0]==0 && annex[1]==0 && annex[2]==0 && annex[3]==1 && annex[4]==0x61,"AVCC prefix normalized "+length);
        }
    }
    private interface Action { void run() throws Exception; }
    private static void rejects(Action action,String label) throws Exception {
        try { action.run(); } catch (IllegalArgumentException expected) { checks++; return; }
        throw new AssertionError("accepted " + label);
    }
    private static void eq(int got,int expected,String label) { check(got==expected,label+": "+got+" != "+expected); }
    private static void same(byte[] got,byte[] expected,String label) { check(Arrays.equals(got,expected),label); }
    private static void check(boolean ok,String label) { if(!ok) throw new AssertionError(label); checks++; }
    private static byte[] bytes(int... values) { byte[] b=new byte[values.length]; for(int i=0;i<b.length;i++)b[i]=(byte)values[i]; return b; }
}
