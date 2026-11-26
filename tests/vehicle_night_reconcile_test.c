#define _GNU_SOURCE
#include "carplay_vehicle_state.h"
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
static atomic_int calls, blocked, release_call, actual = -1;
static void pause_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}
static int execute(int night, void *unused) {
    (void)unused;
    int call = atomic_fetch_add(&calls, 1) + 1;
    if (call == 2) {
        atomic_store(&blocked, 1);
        while (!atomic_load(&release_call)) pause_ms(1);
    }
    atomic_store(&actual, night);
    return 0;
}
int main(int argc, char **argv) {
    struct carplay_vehicle_state state;
    struct carplay_vehicle_snapshot snapshot;
    (void)argv;
    carplay_vehicle_state_init(&state);
    carplay_vehicle_state_begin_session(&state, 1);
    carplay_vehicle_state_set_night_executor(&state, execute, NULL);
    carplay_vehicle_state_request_night(&state, 1, 0);
    for (int i=0; i<500; i++) {
        carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
        if (snapshot.night_applied) break;
        pause_ms(2);
    }
    if (!snapshot.night_applied) return 2;
    carplay_vehicle_state_request_night(&state, 1, 1);
    for (int i=0; i<500 && !atomic_load(&blocked); i++) pause_ms(2);
    if (!atomic_load(&blocked)) return 3;
    if (argc > 1) carplay_vehicle_state_begin_session(&state, 2);
    carplay_vehicle_state_request_night(&state, argc > 1 ? 2 : 1, 0);
    atomic_store(&release_call, 1);
    for (int i=0; i<1000; i++) {
        carplay_vehicle_state_snapshot(&state, carplay_vehicle_monotonic_ms(), &snapshot);
        if (snapshot.night_applied && atomic_load(&calls) >= 3) break;
        pause_ms(2);
    }
    int ok = snapshot.night_applied && snapshot.night_mode == atomic_load(&actual) && atomic_load(&calls) >= 3;
    printf("%s %s desired=%d applied=%d actual=%d calls=%d\n", ok ? "PASS" : "FAIL", argc>1?"generation":"ABA",
        snapshot.night_mode, snapshot.night_applied, atomic_load(&actual), atomic_load(&calls));
    carplay_vehicle_state_destroy(&state);
    return ok ? 0 : 1;
}
