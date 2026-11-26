package com.carplaybox.encoder;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
final class PacketWriter {
    static final int CONFIG=1, KEYFRAME=2, EOS=4, HEARTBEAT=8, MAX_PAYLOAD=4*1024*1024;
    static void write(OutputStream out,int flags,long ptsUs,byte[] payload) throws IOException {
        boolean heartbeat=flags==HEARTBEAT;
        if ((flags & ~(CONFIG | KEYFRAME | EOS | HEARTBEAT)) != 0 || payload == null
                || payload.length > MAX_PAYLOAD
                || ((flags & HEARTBEAT) != 0 && (!heartbeat || payload.length != 0))
                || (payload.length == 0 && (flags & EOS) == 0 && !heartbeat)) {
            throw new IllegalArgumentException("invalid encoder packet");
        }
        byte[] header = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN)
                .putInt(0x43504531).putInt(flags).putInt(payload.length).putInt(0).putLong(ptsUs).array();
        out.write(header);
        out.write(payload);
        out.flush();
    }
}
