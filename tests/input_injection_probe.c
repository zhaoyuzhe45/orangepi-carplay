#include "airplay_uinput.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
void iap2_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc(10, stderr);
}

int main(int argc, char **argv)
{
    struct airplay_uinput *device;
    unsigned x;
    unsigned y;
    if (argc != 3) {
        fprintf(stderr, "usage: input_injection_probe X Y\n");
        return 2;
    }
    x = (unsigned)strtoul(argv[1], NULL, 0);
    y = (unsigned)strtoul(argv[2], NULL, 0);
    device = airplay_uinput_open();
    if (!device || airplay_uinput_touch_down(device, x, y, 1871, 918) < 0 ||
        airplay_uinput_touch_up(device, x, y, 1871, 918) < 0) {
        fprintf(stderr, "uinput touch injection failed\n");
        airplay_uinput_close(device);
        return 1;
    }
    airplay_uinput_close(device);
    return 0;
}



