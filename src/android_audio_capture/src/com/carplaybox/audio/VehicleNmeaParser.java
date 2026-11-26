package com.carplaybox.audio;

import java.util.Calendar;
import java.util.GregorianCalendar;
import java.util.TimeZone;

/** Pure parser shared by the Android bridge and offline validation. */
final class VehicleNmeaParser {
    static final class Fix {
        boolean valid;
        double latitude, longitude;
        Double speedMps, bearingDeg, altitudeM, hdop;
        long utcMillis;
        long utcTimeOfDayMs = -1;
        float accuracyEstimateM() { return (float)Math.max(25.0, hdop == null ? 25.0 : hdop * 5.0); }
    }
    static Fix parse(String sentence) {
        if (sentence == null || sentence.length() > 255) return null;
        int end = sentence.length();
        while (end > 0 && (sentence.charAt(end - 1) == '\r' || sentence.charAt(end - 1) == '\n')) end--;
        String text = sentence.substring(0, end);
        int star = text.indexOf('*');
        if (text.length() < 10 || text.charAt(0) != '$' || star < 7 || star != text.length() - 3) return null;
        int checksum = 0;
        for (int i = 1; i < star; i++) {
            char c = text.charAt(i);
            if (c < 32 || c > 126) return null;
            checksum ^= c;
        }
        try {
            if (checksum != Integer.parseInt(text.substring(star + 1), 16)) return null;
            String[] f = text.substring(1, star).split(",", -1);
            if (f[0].length() != 5 || !f[0].substring(0, 2).matches("[A-Z]{2}")) return null;
            String kind = f[0].substring(2);
            Fix result = new Fix();
            if ("RMC".equals(kind)) {
                if (f.length < 10) return null;
                if ("V".equals(f[2]) || (f.length > 12 && "N".equals(f[12]))) return result;
                if (!"A".equals(f[2])) return null;
                result.latitude = degrees(f[3], f[4], true);
                result.longitude = degrees(f[5], f[6], false);
                result.speedMps = optional(f[7], 0, 2000);
                if (result.speedMps != null) result.speedMps *= 1852.0 / 3600.0;
                result.bearingDeg = optional(f[8], 0, Math.nextDown(360.0));
                result.utcMillis = utc(f[1], f[9]);
                result.utcTimeOfDayMs = result.utcMillis % 86400000L;
            } else if ("GGA".equals(kind)) {
                if (f.length < 13) return null;
                int quality = Integer.parseInt(f[6]);
                if (quality == 0) return result;
                if (quality < 1 || quality > 8) return null;
                int[] time = timeParts(f[1]);
                result.utcTimeOfDayMs = ((time[0] * 60L + time[1]) * 60 + time[2]) * 1000 + time[3];
                result.latitude = degrees(f[2], f[3], true);
                result.longitude = degrees(f[4], f[5], false);
                result.hdop = optional(f[8], 0.01, 999);
                if ("M".equals(f[10]) && "M".equals(f[12]) && !f[9].isEmpty() && !f[11].isEmpty()) {
                    result.altitudeM = number(f[9], -20000, 100000) + number(f[11], -1000, 1000);
                }
            } else return null;
            result.valid = true;
            return result;
        } catch (IllegalArgumentException error) { return null; }
    }

    private static double degrees(String value, String direction, boolean latitude) {
        int digits = latitude ? 4 : 5;
        if (!value.matches("[0-9]{" + digits + "}(\\.[0-9]+)?")) throw new IllegalArgumentException("coordinate");
        boolean negative;
        if ((latitude ? "N" : "E").equals(direction)) negative = false;
        else if ((latitude ? "S" : "W").equals(direction)) negative = true;
        else throw new IllegalArgumentException("hemisphere");
        int degreeDigits = digits - 2;
        double degrees = Integer.parseInt(value.substring(0, degreeDigits));
        double minutes = number(value.substring(degreeDigits), 0, Math.nextDown(60.0));
        double result = degrees + minutes / 60.0;
        if (result > (latitude ? 90 : 180)) throw new IllegalArgumentException("range");
        return negative ? -result : result;
    }

    private static double number(String value, double min, double max) {
        if (!value.matches("[+-]?[0-9]+(\\.[0-9]+)?")) throw new IllegalArgumentException("number");
        double result = Double.parseDouble(value);
        if (Double.isNaN(result) || Double.isInfinite(result) || result < min || result > max)
            throw new IllegalArgumentException("number range");
        return result;
    }
    private static Double optional(String value, double min, double max) {
        return value.isEmpty() ? null : number(value, min, max);
    }
    private static int[] timeParts(String value) {
        if (!value.matches("[0-9]{6}(\\.[0-9]+)?")) throw new IllegalArgumentException("time");
        int h = Integer.parseInt(value.substring(0, 2)), m = Integer.parseInt(value.substring(2, 4));
        double seconds = number(value.substring(4), 0, Math.nextDown(60.0));
        if (h > 23 || m > 59) throw new IllegalArgumentException("time range");
        return new int[] {h, m, (int)seconds, (int)Math.round((seconds - (int)seconds) * 1000)};
    }
    private static long utc(String clock, String date) {
        if (!date.matches("[0-9]{6}")) throw new IllegalArgumentException("date");
        int[] time = timeParts(clock);
        int year = Integer.parseInt(date.substring(4, 6));
        GregorianCalendar calendar = new GregorianCalendar(TimeZone.getTimeZone("UTC"));
        calendar.clear(); calendar.setLenient(false);
        calendar.set(year >= 80 ? year + 1900 : year + 2000, Integer.parseInt(date.substring(2, 4)) - 1,
                Integer.parseInt(date.substring(0, 2)), time[0], time[1], time[2]);
        calendar.set(Calendar.MILLISECOND, time[3]);
        return calendar.getTimeInMillis();
    }
}
