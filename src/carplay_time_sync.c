#define _GNU_SOURCE
#include "carplay_time_sync.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CARPLAY_TIME_NMEA_MAX 160u
#define CARPLAY_TIME_APPLE_REFERENCE 978307200LL
#define CARPLAY_TIME_MIN_EPOCH 946684800LL  /* 2000-01-01T00:00:00Z */
#define CARPLAY_TIME_LIMIT_EPOCH 4102444800LL /* 2100-01-01T00:00:00Z */

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity)
        snprintf(error, capacity, "%s", message);
}

static int is_digit(char value)
{
    return value >= '0' && value <= '9';
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

static int field_at(const char *body, size_t body_len, unsigned wanted,
                    const char **value, size_t *value_len)
{
    size_t start = 0;
    size_t index = 0;

    while (start <= body_len) {
        size_t end = start;
        while (end < body_len && body[end] != ',')
            end++;
        if (index == wanted) {
            *value = body + start;
            *value_len = end - start;
            return 1;
        }
        if (end == body_len)
            break;
        start = end + 1u;
        index++;
    }
    return 0;
}

static int parse_digits(const char *value, size_t len, int *out)
{
    int number = 0;
    size_t i;

    if (!len || len > 4u)
        return -1;
    for (i = 0; i < len; i++) {
        if (!is_digit(value[i]))
            return -1;
        number = number * 10 + value[i] - '0';
    }
    *out = number;
    return 0;
}

static int parse_hms(const char *value, size_t len, int *hour, int *minute,
                     int *second)
{
    size_t i;

    if (len < 6u || !is_digit(value[0]) || !is_digit(value[1]) ||
        !is_digit(value[2]) || !is_digit(value[3]) ||
        !is_digit(value[4]) || !is_digit(value[5]))
        return -1;
    if (len > 6u) {
        if (value[6] != '.')
            return -1;
        for (i = 7u; i < len; i++) {
            if (!is_digit(value[i]))
                return -1;
        }
    }
    *hour = (value[0] - '0') * 10 + value[1] - '0';
    *minute = (value[2] - '0') * 10 + value[3] - '0';
    *second = (value[4] - '0') * 10 + value[5] - '0';
    return *hour <= 23 && *minute <= 59 && *second <= 60 ? 0 : -1;
}

static int leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int valid_date(int year, int month, int day)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31 };
    int limit;

    if (year < 2000 || year >= 2100 || month < 1 || month > 12)
        return 0;
    limit = days[month - 1];
    if (month == 2 && leap_year(year))
        limit = 29;
    return day >= 1 && day <= limit;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    int era;
    int adjusted_month;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned day_of_era;

    if (month <= 2u)
        year--;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (unsigned)(year - era * 400);
    adjusted_month = (int)(month > 2u ? month - 3u : month + 9u);
    day_of_year = (unsigned)(153 * adjusted_month + 2) / 5u + day - 1u;
    day_of_era = year_of_era * 365u + year_of_era / 4u -
                 year_of_era / 100u + day_of_year;
    return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static int make_epoch(int year, int month, int day, int hour, int minute,
                      int second, int64_t *epoch)
{
    int64_t value;

    if (!valid_date(year, month, day))
        return -1;
    value = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400LL +
            (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second;
    if (value < CARPLAY_TIME_MIN_EPOCH || value >= CARPLAY_TIME_LIMIT_EPOCH)
        return -1;
    *epoch = value;
    return 0;
}

int carplay_time_sync_jump_allowed(int64_t current_epoch_seconds,
                                    int64_t candidate_epoch_seconds)
{
    /* Data validity is checked by the parser. Once the car supplies the
     * candidate time, do not compare it with the current RTC or impose an
     * expiry/drift limit. */
    (void)current_epoch_seconds;
    return candidate_epoch_seconds >= CARPLAY_TIME_MIN_EPOCH &&
           candidate_epoch_seconds < CARPLAY_TIME_LIMIT_EPOCH;
}

static int parse_rmc(const char *body, size_t body_len,
                     struct carplay_nmea_time *result, char *error,
                     size_t error_capacity)
{
    const char *time_value;
    const char *status;
    const char *date_value;
    size_t time_len;
    size_t status_len;
    size_t date_len;
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;
    int short_year;

    if (!field_at(body, body_len, 1u, &time_value, &time_len) ||
        !field_at(body, body_len, 2u, &status, &status_len) ||
        !field_at(body, body_len, 9u, &date_value, &date_len)) {
        set_error(error, error_capacity, "RMC sentence has missing fields");
        return -1;
    }
    /* A void fix can carry a stale time; require the receiver's active flag. */
    if (status_len != 1u || status[0] != 'A')
        return 0;
    if (parse_hms(time_value, time_len, &hour, &minute, &second) < 0) {
        set_error(error, error_capacity, "RMC UTC time is invalid");
        return -1;
    }
    if (date_len != 6u || parse_digits(date_value, 2u, &day) < 0 ||
        parse_digits(date_value + 2u, 2u, &month) < 0 ||
        parse_digits(date_value + 4u, 2u, &short_year) < 0) {
        set_error(error, error_capacity, "RMC date is invalid");
        return -1;
    }
    /* This receiver emits the Unix epoch at startup even with status A.
     * Only the exact unset timestamp is unusable for setting the clock. */
    if (short_year == 70 && month == 1 && day == 1 && !hour && !minute && !second) {
        size_t i;
        for (i = 6; i < time_len; ++i)
            if (time_value[i] != '.' && time_value[i] != '0') break;
        if (i == time_len) return 0;
    }
    year = short_year >= 80 ? 1900 + short_year : 2000 + short_year;
    if (make_epoch(year, month, day, hour, minute, second,
                   &result->epoch_seconds) < 0) {
        set_error(error, error_capacity, "RMC date is outside 2000-2099");
        return -1;
    }
    snprintf(result->sentence, sizeof(result->sentence), "RMC");
    return 1;
}

static int parse_zda(const char *body, size_t body_len,
                     struct carplay_nmea_time *result, char *error,
                     size_t error_capacity)
{
    const char *time_value;
    const char *day_value;
    const char *month_value;
    const char *year_value;
    size_t time_len;
    size_t day_len;
    size_t month_len;
    size_t year_len;
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;

    if (!field_at(body, body_len, 1u, &time_value, &time_len) ||
        !field_at(body, body_len, 2u, &day_value, &day_len) ||
        !field_at(body, body_len, 3u, &month_value, &month_len) ||
        !field_at(body, body_len, 4u, &year_value, &year_len)) {
        set_error(error, error_capacity, "ZDA sentence has missing fields");
        return -1;
    }
    if (parse_hms(time_value, time_len, &hour, &minute, &second) < 0 ||
        parse_digits(day_value, day_len, &day) < 0 ||
        parse_digits(month_value, month_len, &month) < 0 ||
        year_len != 4u || parse_digits(year_value, year_len, &year) < 0) {
        set_error(error, error_capacity, "ZDA UTC value is invalid");
        return -1;
    }
    if (make_epoch(year, month, day, hour, minute, second,
                   &result->epoch_seconds) < 0) {
        set_error(error, error_capacity, "ZDA date is outside 2000-2099");
        return -1;
    }
    snprintf(result->sentence, sizeof(result->sentence), "ZDA");
    return 1;
}

static int parse_sentence(const char *body, size_t body_len,
                          struct carplay_nmea_time *result, char *error,
                          size_t error_capacity)
{
    if (body_len >= 5u && body[2] == 'R' && body[3] == 'M' &&
        body[4] == 'C')
        return parse_rmc(body, body_len, result, error, error_capacity);
    if (body_len >= 5u && body[2] == 'Z' && body[3] == 'D' &&
        body[4] == 'A')
        return parse_zda(body, body_len, result, error, error_capacity);
    return 0;
}

static int parse_candidate(const char *start, size_t available,
                           struct carplay_nmea_time *result, char *error,
                           size_t error_capacity, int *time_sentence)
{
    const char *body = start + 1;
    size_t body_len = 0;
    size_t checksum_at = available;
    int expected;
    int actual = 0;
    size_t i;

    while (body_len < available && body_len < CARPLAY_TIME_NMEA_MAX &&
           body[body_len] != '*' && body[body_len] != '\r' &&
           body[body_len] != '\n' && body[body_len] != '\0') {
        body_len++;
    }
    if (body_len >= 5u && body[2] == 'R' && body[3] == 'M' &&
        body[4] == 'C')
        *time_sentence = 1;
    else if (body_len >= 5u && body[2] == 'Z' && body[3] == 'D' &&
             body[4] == 'A')
        *time_sentence = 1;
    else
        return 0;

    if (body_len >= available || body[body_len] != '*') {
        set_error(error, error_capacity, "time sentence has no checksum");
        return -1;
    }
    checksum_at = body_len;
    if (checksum_at + 2u >= available ||
        hex_value(body[checksum_at + 1u]) < 0 ||
        hex_value(body[checksum_at + 2u]) < 0) {
        set_error(error, error_capacity, "time sentence checksum is truncated");
        return -1;
    }
    expected = hex_value(body[checksum_at + 1u]) * 16 +
               hex_value(body[checksum_at + 2u]);
    for (i = 0; i < body_len; i++)
        actual ^= (unsigned char)body[i];
    if ((actual & 0xff) != expected) {
        set_error(error, error_capacity, "time sentence checksum mismatch");
        return -1;
    }
    return parse_sentence(body, body_len, result, error, error_capacity);
}

int carplay_time_sync_parse_nmea(const char *nmea, size_t len,
                                 struct carplay_nmea_time *result,
                                 char *error, size_t error_capacity)
{
    size_t i;
    int saw_time_sentence = 0;
    char last_error[128] = "";

    if (!nmea || !result || (!len && !nmea)) {
        set_error(error, error_capacity, "invalid NMEA input");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (i = 0; i + 1u < len; i++) {
        int time_sentence = 0;
        int parsed;
        char sentence_error[128] = "";

        if (nmea[i] != '$')
            continue;
        parsed = parse_candidate(nmea + i, len - i, result,
                                 sentence_error, sizeof(sentence_error),
                                 &time_sentence);
        if (!time_sentence)
            continue;
        saw_time_sentence = 1;
        if (parsed == 1)
            return 1;
        if (parsed < 0)
            snprintf(last_error, sizeof(last_error), "%s", sentence_error);
    }
    if (saw_time_sentence && last_error[0]) {
        set_error(error, error_capacity, last_error);
        return -1;
    }
    set_error(error, error_capacity, "NMEA has no absolute UTC time");
    return 0;
}

int carplay_time_sync_from_apple_epoch(uint64_t seconds,
                                       int64_t *utc_epoch_seconds)
{
    int64_t converted;

    if (!utc_epoch_seconds ||
        seconds > (uint64_t)(INT64_MAX - CARPLAY_TIME_APPLE_REFERENCE))
        return -1;
    converted = CARPLAY_TIME_APPLE_REFERENCE + (int64_t)seconds;
    if (converted < CARPLAY_TIME_MIN_EPOCH ||
        converted >= CARPLAY_TIME_LIMIT_EPOCH)
        return -1;
    *utc_epoch_seconds = converted;
    return 0;
}

int carplay_time_sync_apply_epoch(int64_t utc_epoch_seconds,
                                  char *detail, size_t detail_capacity)
{
    struct timespec now;
    struct timespec target;
    int64_t delta;

    if (utc_epoch_seconds < CARPLAY_TIME_MIN_EPOCH ||
        utc_epoch_seconds >= CARPLAY_TIME_LIMIT_EPOCH) {
        set_error(detail, detail_capacity, "rejected implausible UTC time");
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        set_error(detail, detail_capacity, "could not read system time");
        return -1;
    }
    if (!carplay_time_sync_jump_allowed((int64_t)now.tv_sec,
                                        utc_epoch_seconds)) {
        set_error(detail, detail_capacity,
                  "rejected discontinuous UTC time");
        return -1;
    }
    delta = utc_epoch_seconds - (int64_t)now.tv_sec;
    if (delta > -2 && delta < 2) {
        set_error(detail, detail_capacity, "system time already aligned");
        return 0;
    }
    target.tv_sec = (time_t)utc_epoch_seconds;
    target.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &target) < 0) {
        snprintf(detail, detail_capacity,
                 "clock_settime failed errno=%d", errno);
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 ||
        (int64_t)now.tv_sec < utc_epoch_seconds - 2 ||
        (int64_t)now.tv_sec > utc_epoch_seconds + 2) {
        set_error(detail, detail_capacity, "system time verification failed");
        return -1;
    }
    set_error(detail, detail_capacity, "system time updated");
    return 1;
}

int carplay_time_sync_apply_nmea(const char *nmea, size_t len,
                                 char *detail, size_t detail_capacity)
{
    struct carplay_nmea_time parsed;
    char error[128];
    int rc = carplay_time_sync_parse_nmea(nmea, len, &parsed,
                                          error, sizeof(error));

    if (rc <= 0) {
        snprintf(detail, detail_capacity, "%s", error);
        return rc;
    }
    rc = carplay_time_sync_apply_epoch(parsed.epoch_seconds,
                                       detail, detail_capacity);
    if (rc >= 0 && detail && detail_capacity) {
        size_t used = strnlen(detail, detail_capacity);
        if (used < detail_capacity - 1u)
            snprintf(detail + used, detail_capacity - used,
                     " (%s)", parsed.sentence);
    }
    return rc;
}
