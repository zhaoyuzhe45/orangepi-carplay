package com.carplaybox.audio;

public final class VehicleNmeaParserTest {
    static String nmea(String body) {
        int checksum = 0;
        for (int i = 0; i < body.length(); i++) checksum ^= body.charAt(i);
        return "$" + body + "*" + String.format("%02X", checksum);
    }
    static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
    }
    public static void main(String[] args) {
        String body = "GNRMC,120000.50,A,4851.402000,N,00221.102000,E,21.208,218.18,210926,,,A";
        VehicleNmeaParser.Fix fix = VehicleNmeaParser.parse(nmea(body));
        check(fix != null && fix.valid, "checksum-valid RMC yields fix");
        check(Math.abs(fix.latitude - 48.8567) < 1e-7 && Math.abs(fix.longitude - 2.3517) < 1e-7, "coordinates");
        check(Math.abs(fix.speedMps - 21.208 * 1852 / 3600) < 1e-7, "knots to meters per second");
        check(fix.utcMillis == 1789992000500L, "UTC date/time and fractions");
        check(fix.altitudeM == null && fix.accuracyEstimateM() == 25, "unknown altitude and explicit conservative accuracy estimate");
        fix = VehicleNmeaParser.parse(nmea("GPGGA,120000,3456.000,S,05830.000,W,1,08,0.8,35.0,M,44.8,M,,"));
        check(fix != null && fix.valid && Math.abs(fix.latitude + 34.9333333333) < 1e-8 && fix.longitude == -58.5, "south and west");
        check(Math.abs(fix.altitudeM - 79.8) < 1e-7 && fix.utcMillis == 0, "ellipsoid altitude adds geoid separation; no invented GGA date");
        fix = VehicleNmeaParser.parse(nmea("GPGGA,120000,,,,,0,00,,,,,,,"));
        check(fix != null && !fix.valid, "GGA quality zero invalidates prior fix");
        fix = VehicleNmeaParser.parse(nmea("GPRMC,120000,V,,,,,,,210926,,,N"));
        check(fix != null && !fix.valid, "RMC void invalidates prior fix");
        check(VehicleNmeaParser.parse(nmea(body).replace("21.208", "21.209")) == null, "checksum failure");
        check(VehicleNmeaParser.parse(nmea(body) + "\0") == null, "embedded NUL rejected");
        check(VehicleNmeaParser.parse(nmea(body.replace("4851.402000", "4899.4"))) == null, "minutes out of range");
        check(VehicleNmeaParser.parse(nmea(body.replace("00221.102000", "18121.0"))) == null, "longitude out of range");
        check(VehicleNmeaParser.parse(nmea(body.replace(",N,", ",Q,"))) == null, "invalid hemisphere");
        check(VehicleNmeaParser.parse(nmea(body.replace("21.208", "NaN"))) == null, "nonfinite speed");
        check(VehicleNmeaParser.parse(nmea(body.replace("210926", "310226"))) == null, "invalid date");
        check(VehicleNmeaParser.parse(nmea(body.replace("120000.50", "246000"))) == null, "invalid UTC clock");
        check(VehicleNmeaParser.parse(nmea("PASCD,43200.000,C,D,1,1,0.00,10.556")) == null, "PASCD does not invent position");
        System.out.println("PASS NMEA parser");
    }
}
