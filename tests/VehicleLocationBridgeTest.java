package com.carplaybox.audio;

public final class VehicleLocationBridgeTest {
    static final class Provider implements VehicleLocationBridge.Provider {
        int applies, clears;
        boolean fail;
        long elapsed, wall;
        VehicleNmeaParser.Fix last;
        public void apply(VehicleNmeaParser.Fix fix, long elapsedMillis, long wallMillis) throws Exception {
            applies++; elapsed = elapsedMillis; wall = wallMillis; last = fix;
            if (fail) throw new Exception("permission denied");
        }
        public void clear() { clears++; }
    }
    static void check(boolean yes, String label) { if (!yes) throw new AssertionError(label); }
    public static void main(String[] args) {
        Provider provider = new Provider();
        VehicleLocationBridge bridge = new VehicleLocationBridge(provider);
        VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
        input.enabled = input.active = true;
        input.instance = input.generation = input.rmcSequence = 1;
        input.rmc = VehicleNmeaParserTest.nmea("GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A");
        input.rmcAgeMs = 500;
        bridge.update(input, 10000, 1789992000500L);
        check(provider.applies == 1 && bridge.hasFix && bridge.applied && provider.elapsed == 10000,
                "successful car fix is applied at the current poll time");
        check(bridge.lastContentAgeMs == 500, "reports NMEA content age separately from arrival age");
        int clearsBeforeTimeout = provider.clears;
        bridge.pollFailed("HTTP timeout");
        bridge.expire(90000);
        check(bridge.hasFix && bridge.applied && provider.clears == clearsBeforeTimeout && provider.applies == 1,
                "a missed poll must neither clear nor republish a previously received fix");
        check(bridge.lastApplyDurationMs >= 0, "reports provider apply duration");
        bridge.update(input, 10200, 1789992000700L);
        check(provider.applies == 2, "each successful poll replaces the Android location");
        bridge.expire(14500);
        check(bridge.applied, "time alone never removes a successful car fix");
        input.rmcAgeMs = 5000;
        bridge.update(input, 15000, 1789992005500L);
        check(bridge.hasFix && bridge.applied && provider.applies == 3,
                "aged NMEA is still the car's latest snapshot");
        input.rmcAgeMs = 0; input.rmcSequence = 2; provider.fail = true;
        bridge.update(input, 16000, 1789992006000L);
        check(bridge.hasFix && !bridge.applied && bridge.status.contains("permission denied"),
                "received fix is not a successful application");
        bridge.expire(22000);
        check(bridge.hasFix && !bridge.applied && bridge.status.contains("permission denied"),
                "provider failure status is retained without a time expiry decision");
        provider.fail = false;
        bridge.update(input, 16200, 1789992006200L);
        check(bridge.applied && provider.applies == 5, "same snapshot retries after provider failure");
        input.ggaSequence = 3;
        input.gga = VehicleNmeaParserTest.nmea("GPGGA,120000,,,,,0,00,,,,,,,");
        bridge.update(input, 16300, 1789992006300L);
        check(!bridge.hasFix && !bridge.applied, "newer no-fix wins over older valid sentence");
        input.active = false;
        bridge.update(input, 16400, 1789992006400L);
        check(!bridge.applied, "disconnect clears provider");
        input.active = true; input.enabled = false;
        bridge.update(input, 16500, 1789992006500L);
        check(!bridge.applied, "GPS disabled");
        bridge.stop("HTTP unavailable");
        check(!bridge.hasFix && bridge.status.contains("HTTP unavailable"), "transport loss clears provider");
        testPairedSentences();
        testLocationTimeUsesArrivalClock();
        System.out.println("PASS vehicle provider lifecycle");
    }

    private static void testPairedSentences() {
        Provider provider = new Provider();
        VehicleLocationBridge bridge = new VehicleLocationBridge(provider);
        VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
        input.enabled = input.active = true;
        input.instance = input.generation = 1;
        input.rmcSequence = 10; input.ggaSequence = 11;
        input.rmcAgeMs = 600; input.ggaAgeMs = 500;
        input.rmc = VehicleNmeaParserTest.nmea("GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,210926,,,A");
        input.gga = VehicleNmeaParserTest.nmea("GPGGA,120000,4851.402,N,00221.102,E,1,08,0.8,35.0,M,44.8,M,,");
        bridge.update(input, 10000, 1789992000600L);
        check(bridge.applied && provider.last.speedMps != null &&
                Math.abs(provider.last.speedMps - 21.208 * 1852 / 3600) < 1e-7,
                "newer same-epoch GGA must retain RMC speed for launcher hasSpeed gate");
        check(provider.last.bearingDeg == 218.18 && provider.last.altitudeM == 79.8 &&
                provider.last.utcMillis == 1789992000000L,
                "paired observation contains direction, ellipsoid altitude and UTC date");
        bridge.expire(14400);
        check(bridge.applied, "paired fix remains active regardless of NMEA age");

        input.ggaSequence++; input.ggaAgeMs = 0;
        input.gga = VehicleNmeaParserTest.nmea("GPGGA,120003,4851.402,N,00221.102,E,1,08,0.8,35.0,M,44.8,M,,");
        bridge.update(input, 10000, 1789992000600L);
        check(provider.last.speedMps != null,
                "same position keeps RMC movement fields without an NMEA time gate");
        input.gga = VehicleNmeaParserTest.nmea("GPGGA,120000,4951.402,N,00221.102,E,1,08,0.8,35.0,M,44.8,M,,");
        input.ggaSequence++;
        bridge.update(input, 10000, 1789992000600L);
        check(provider.last.speedMps == null, "inconsistent positions must not merge fields");
        input.rmc = VehicleNmeaParserTest.nmea("GPRMC,120000,V,,,,,,,210926,,,N");
        input.rmcSequence = ++input.ggaSequence + 1;
        bridge.update(input, 10000, 1789992000600L);
        check(!bridge.hasFix && !bridge.applied, "latest invalid RMC must still revoke the paired fix");
    }

    private static void testLocationTimeUsesArrivalClock() {
        Provider provider = new Provider();
        VehicleLocationBridge bridge = new VehicleLocationBridge(provider);
        VehicleLocationBridge.Input input = new VehicleLocationBridge.Input();
        input.enabled = input.active = true;
        input.instance = input.generation = input.rmcSequence = 1;
        input.rmcAgeMs = 500;
        input.rmc = VehicleNmeaParserTest.nmea(
                "GPRMC,120000,A,4851.402,N,00221.102,E,21.208,218.18,281126,,,A");
        bridge.update(input, 10000, 1789992000500L);
        check(provider.wall == 1789992000500L && provider.elapsed == 10000,
                "car clock errors are ignored and the poll time stamps the replacement");
    }
}
