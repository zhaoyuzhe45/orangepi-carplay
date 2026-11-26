#define _GNU_SOURCE
#include "carplay_vehicle_state.h"
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
static atomic_int calls;
static void sleep_ms(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL); }
static int execute(int night,void*context) { (void)night;(void)context;return atomic_fetch_add(&calls,1)==0?-1:0; }
int main(void) {
 struct carplay_vehicle_state state;
 struct carplay_vehicle_snapshot snapshot;
 carplay_vehicle_state_init(&state); carplay_vehicle_state_begin_session(&state,7);
 carplay_vehicle_state_set_night_executor(&state,execute,NULL);
 uint64_t start=carplay_vehicle_monotonic_ms();
 if(carplay_vehicle_state_request_night(&state,7,1)<0)return 1;
 if(carplay_vehicle_monotonic_ms()-start>100)return 2;
 for(int i=0;i<180;i++){
  carplay_vehicle_state_snapshot(&state,carplay_vehicle_monotonic_ms(),&snapshot);
  if(snapshot.night_applied)break;
  sleep_ms(10);
 }
 if(!snapshot.night_applied || atomic_load(&calls)!=2){fprintf(stderr,"FAIL night worker did not retry automatically\n");return 3;}
 carplay_vehicle_state_request_night(&state,7,1);sleep_ms(200);
 if(atomic_load(&calls)!=2)return 4;
 carplay_vehicle_state_end_session(&state,7);
 if(carplay_vehicle_state_request_night(&state,7,0)>=0)return 5;
 carplay_vehicle_state_destroy(&state);puts("PASS automatic night retry, deduplication and generation");return 0;
}
