package com.carplaybox.audio;

public final class VehicleLocationPollScheduleTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        check(VehicleLocationPollSchedule.delayAfter(1000, 1100) == 150,
                "poll cycle waits only for the remaining quarter-second interval");
        check(VehicleLocationPollSchedule.delayAfter(1000, 1250) == 0,
                "slow API calls do not add another full polling interval");
        check(VehicleLocationPollSchedule.delayAfter(1000, 1000) == 250,
                "poll interval is 250 ms when the request returns immediately");
        System.out.println("PASS vehicle location poll schedule");
    }
}
