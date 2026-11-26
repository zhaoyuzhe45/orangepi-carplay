package com.carplaybox.audio;

import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.os.Looper;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

/** Sends the current car sample to the board's standard GNSS HAL. */
final class VehicleGnssChannel {
    private final LocationManager manager;
    private LocalSocket socket;
    private DataOutputStream output;
    private DataInputStream input;
    private boolean listening;
    private long revision;
    private final LocationListener listener = new LocationListener() {
        public void onLocationChanged(Location location) {}
        public void onStatusChanged(String name, int status, Bundle extras) {}
        public void onProviderEnabled(String name) {}
        public void onProviderDisabled(String name) {}
    };
    VehicleGnssChannel(LocationManager manager) { this.manager = manager; }
    boolean available() {
        try {
            return "1".equals(Class.forName("android.os.SystemProperties")
                    .getMethod("get", String.class, String.class)
                    .invoke(null, "sys.carplay.gnss.bridge", "0"));
        } catch (ReflectiveOperationException | RuntimeException unavailable) { return false; }
    }
    void apply(Location location) throws Exception {
        if (!listening) {
            manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 0, 0, listener, Looper.getMainLooper());
            listening = true;
        }
        if (socket == null) {
            LocalSocket next = new LocalSocket();
            try {
                next.connect(new LocalSocketAddress("carplay.vehicle.gnss.v1", LocalSocketAddress.Namespace.ABSTRACT));
                next.setSoTimeout(750);
                output = new DataOutputStream(next.getOutputStream());
                input = new DataInputStream(next.getInputStream());
                socket = next;
            } catch (IOException failure) { next.close(); throw failure; }
        }
        // The HAL ack confirms acceptance; Android's location cache updates asynchronously.
        send(location);
    }
    private void send(Location location) throws IOException {
        int flags = location == null ? 0 : 17;
        if (location != null) {
            if (location.hasAltitude()) flags |= 2;
            if (location.hasSpeed()) flags |= 4;
            if (location.hasBearing()) flags |= 8;
        }
        output.writeInt(0x43564731); output.writeInt(flags); output.writeLong(++revision);
        output.writeLong(location == null ? 0 : location.getTime());
        output.writeLong(location == null ? 0 : location.getElapsedRealtimeNanos());
        output.writeDouble(location == null ? 0 : location.getLatitude());
        output.writeDouble(location == null ? 0 : location.getLongitude());
        output.writeDouble(location == null ? 0 : location.getAltitude());
        output.writeFloat(location == null ? 0 : location.getSpeed());
        output.writeFloat(location == null ? 0 : location.getBearing());
        output.writeFloat(location == null ? 0 : location.getAccuracy());
        output.writeInt(0); output.writeLong(0); // 80-byte versioned frame
        output.flush();
        if (input.readUnsignedByte() != 1) throw new IOException("GNSS 拒绝无效定位帧");
    }
    void clear() throws Exception {
        Exception failure = null;
        try { if (socket != null) send(null); } catch (Exception problem) { failure = problem; }
        finally {
            if (socket != null) { try { socket.close(); } catch (IOException ignored) {} }
            socket = null; output = null; input = null;
            if (listening) { manager.removeUpdates(listener); listening = false; }
        }
        if (failure != null) throw failure;
    }
}
