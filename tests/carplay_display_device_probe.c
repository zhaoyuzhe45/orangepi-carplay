#define _POSIX_C_SOURCE 200809L
#include "carplay_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
void iap2_log(const char *fmt, ...) { (void)fmt; }
int main(int argc, char **argv)
{
    if (argc != 4) return 64;
    struct carplay_display_plan plan = {0};
    plan.physical_width = (unsigned)atoi(argv[1]);
    plan.physical_height = (unsigned)atoi(argv[2]);
    plan.physical_fps = 60;
    plan.density = (unsigned)atoi(argv[3]);
    char request[64];
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int result = carplay_display_start_preparation(&plan, request, sizeof(request));
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (result < 0) { puts("launch_failed"); return 1; }
    long long us = (end.tv_sec-start.tv_sec)*1000000LL + (end.tv_nsec-start.tv_nsec)/1000;
    printf("request_id=%s\nlaunch_us=%lld\n", request, us);
    return 0;
}
