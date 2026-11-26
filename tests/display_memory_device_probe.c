#define _POSIX_C_SOURCE 200809L
#include "carplay_display.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
void iap2_log(const char *format,...)
{ va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n'); }
static uint64_t now_ms(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
int main(void)
{
    struct carplay_display_state state;
    if(carplay_display_read(&state)<0 || state.size_override ||
       !carplay_display_mode_builtin(state.physical_width,state.physical_height)) return 2;
    struct carplay_display_memory m={0};uint64_t start=now_ms();
    while(!m.complete && now_ms()-start<15000) {
        carplay_display_remember_step(&m,state.physical_width,state.physical_height,state.density,now_ms());
        struct timespec tick={0,50000000};nanosleep(&tick,NULL);
    }
    carplay_display_remember_close(&m);
    printf("current profile %ux%u density=%u completion=%d attempts=%u elapsed_ms=%llu\n",
        state.physical_width,state.physical_height,state.density,m.complete,m.attempts,
        (unsigned long long)(now_ms()-start));
    return m.complete?0:1;
}
