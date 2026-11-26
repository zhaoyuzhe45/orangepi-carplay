#define _GNU_SOURCE
#include "carplay_vehicle_state.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
int main(void)
{
    struct carplay_vehicle_state state;
    unsigned char wire[128];
    char out[128];
    size_t n, written = 0;
    int known = 0, night = 0;
    struct carplay_vehicle_snapshot snapshot;
    n = carplay_vehicle_state_encode_location_start(wire, sizeof(wire), 1000);
    CHECK(n > 6 && (((size_t)wire[2] << 8) | wire[3]) == n);
    CHECK(wire[4] == 0xff && wire[5] == 0xfa);
    n = carplay_vehicle_state_encode_location_stop(wire, sizeof(wire));
    CHECK(n == 6 && wire[2] == 0 && wire[3] == 6 && wire[4] == 0xff && wire[5] == 0xfc);
    carplay_vehicle_state_init(&state);
    carplay_vehicle_state_begin_session(&state, 1);
    CHECK(carplay_vehicle_state_set_nmea(&state, 1, "$OLD", 4) == 0);
    CHECK(carplay_vehicle_state_set_night(&state, 1, 1, 1) == 1);
    carplay_vehicle_state_begin_session(&state, 2);
    CHECK(carplay_vehicle_state_get_nmea(&state, out, sizeof(out), &written) == 0 && written == 0);
    CHECK(carplay_vehicle_state_set_nmea(&state, 1, "$LATE", 5) < 0);
    CHECK(carplay_vehicle_state_set_night(&state, 1, 1, 0) < 0);
    CHECK(carplay_vehicle_state_get_night(&state, &known, &night) == 0 && !known);
    CHECK(carplay_vehicle_state_set_night(&state, 2, 1, 1) == 1);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
    CHECK(snapshot.night_known && !snapshot.night_applied);
    CHECK(carplay_vehicle_state_night_applied(&state, 2, 1, "failure") == 0);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
    CHECK(!snapshot.night_applied && !strcmp(snapshot.night_error, "failure"));
    CHECK(carplay_vehicle_state_night_applied(&state, 2, 1, "") == 0);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
    CHECK(snapshot.night_applied);
    CHECK(carplay_vehicle_state_set_nmea(&state, 2, "$GPRMC,first", 12) == 0);
    CHECK(carplay_vehicle_state_set_nmea(&state, 2, "$GPGGA,second", 13) == 0);
    CHECK(carplay_vehicle_state_set_nmea(&state, 2, "$PASCD,last", 11) == 0);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
    CHECK(snapshot.received && !strcmp(snapshot.rmc, "$GPRMC,first") && !strcmp(snapshot.gga, "$GPGGA,second"));
    CHECK(snapshot.rmc_sequence < snapshot.gga_sequence);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms() + 5000, &snapshot);
    CHECK(snapshot.received && !strcmp(snapshot.rmc, "$GPRMC,first") &&
          !strcmp(snapshot.gga, "$GPGGA,second"));
    CHECK(carplay_vehicle_state_decode_location_information(&state, 2, NULL, 0, NULL, 0) == 0);
    carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
    CHECK(!snapshot.received && !snapshot.rmc[0] && !snapshot.gga[0]);
    CHECK(carplay_vehicle_state_set_nmea(&state, 2, "$GPRMC,x\0hidden", 15) < 0);
    carplay_vehicle_state_end_session(&state, 2);
    CHECK(carplay_vehicle_state_set_night(&state, 2, 1, 1) < 0);
    CHECK(carplay_vehicle_state_set_nmea(&state, 2, "$LATE", 5) < 0);
    carplay_vehicle_state_destroy(&state);
    if (!failures) puts("PASS vehicle framing and generation");
    return failures != 0;
}
