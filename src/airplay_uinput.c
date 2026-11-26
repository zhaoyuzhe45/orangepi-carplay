#define _GNU_SOURCE
#include "airplay_uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern void iap2_log(const char *fmt, ...);

#define AIRPLAY_UINPUT_MAX 65535u

struct airplay_uinput {
    int fd;
    int touch_active;
    unsigned parity;
};

static int uinput_ioctl(int fd, unsigned long request, int value)
{
    if (ioctl(fd, request, value) < 0) {
        iap2_log("airplay input: uinput ioctl 0x%lx failed: %s",
                 request, strerror(errno));
        return -1;
    }
    return 0;
}

static int uinput_setup_single_bits(int fd)
{
    if (uinput_ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        uinput_ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        uinput_ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        uinput_ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        uinput_ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0 ||
        uinput_ioctl(fd, UI_SET_ABSBIT, ABS_Y) < 0)
        return -1;
    return 0;
}

static int uinput_setup_multitouch_bits(int fd)
{
    if (uinput_ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0 ||
        uinput_ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0 ||
        uinput_ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0 ||
        uinput_ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0)
        return -1;
    return 0;
}

struct airplay_uinput *airplay_uinput_open(void)
{
    struct airplay_uinput *device;
    struct uinput_user_dev setup;
    int fd;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        iap2_log("airplay input: /dev/uinput unavailable: %s", strerror(errno));
        return NULL;
    }
    if (uinput_setup_single_bits(fd) < 0 ||
        uinput_setup_multitouch_bits(fd) < 0 ||
        uinput_ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        close(fd);
        return NULL;
    }

    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "CarPlay virtual touchscreen");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0104;
    setup.id.version = 1;
    setup.absmin[ABS_X] = 0;
    setup.absmax[ABS_X] = AIRPLAY_UINPUT_MAX;
    setup.absmin[ABS_Y] = 0;
    setup.absmax[ABS_Y] = AIRPLAY_UINPUT_MAX;
    /* A zero-width ABS_MT_SLOT made InputReader report "Slot: unknown range"
     * (seen in `dumpsys input` on this unit). Declaring ten slots is what a
     * real panel does and binds the frame sequence to protocol B. */
    setup.absmin[ABS_MT_SLOT] = 0;
    setup.absmax[ABS_MT_SLOT] = 9;
    setup.absmin[ABS_MT_POSITION_X] = 0;
    setup.absmax[ABS_MT_POSITION_X] = AIRPLAY_UINPUT_MAX;
    setup.absmin[ABS_MT_POSITION_Y] = 0;
    setup.absmax[ABS_MT_POSITION_Y] = AIRPLAY_UINPUT_MAX;
    if (write(fd, &setup, sizeof(setup)) != (ssize_t)sizeof(setup)) {
        iap2_log("airplay input: uinput setup write failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        iap2_log("airplay input: uinput create failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    device = calloc(1, sizeof(*device));
    if (!device) {
        (void)ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return NULL;
    }
    device->fd = fd;
    /* InputReader discovers devices asynchronously. A session normally lasts
     * long enough before touch starts, but this short wait also protects the
     * device acceptance probe on fast local reconnects. */
    usleep(250000);
    iap2_log("airplay input: uinput touchscreen ready; direct low-latency input enabled");
    return device;
}

void airplay_uinput_close(struct airplay_uinput *device)
{
    if (!device)
        return;
    if (device->fd >= 0) {
        (void)ioctl(device->fd, UI_DEV_DESTROY);
        close(device->fd);
    }
    free(device);
}

static int write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        iap2_log("airplay input: uinput write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int scale_coordinate(unsigned value, unsigned extent)
{
    uint64_t scaled;

    if (extent < 2)
        return -1;
    if (value >= extent)
        value = extent - 1;
    scaled = (uint64_t)value * AIRPLAY_UINPUT_MAX;
    return (int)(scaled / (extent - 1));
}

static int with_parity(int scaled, unsigned parity)
{
    int value = scaled + (int)(parity & 1u);

    /* The exact maximum has no room above it, so step down instead: the frame
     * still changes and stays inside the declared range. */
    if (value > (int)AIRPLAY_UINPUT_MAX)
        value -= 2;
    return value;
}

int airplay_uinput_axis_values(unsigned x, unsigned y, unsigned width,
                               unsigned height, unsigned parity,
                               struct airplay_uinput_axis_values *out)
{
    int scaled_x;
    int scaled_y;

    if (!out)
        return -1;
    scaled_x = scale_coordinate(x, width);
    scaled_y = scale_coordinate(y, height);
    if (scaled_x < 0 || scaled_y < 0)
        return -1;
    out->x = with_parity(scaled_x, parity);
    out->y = with_parity(scaled_y, parity);
    return 0;
}

static int write_axis_values(struct airplay_uinput *device,
                             const struct airplay_uinput_axis_values *values)
{
    if (write_event(device->fd, EV_ABS, ABS_MT_POSITION_X, values->x) < 0 ||
        write_event(device->fd, EV_ABS, ABS_MT_POSITION_Y, values->y) < 0 ||
        write_event(device->fd, EV_ABS, ABS_X, values->x) < 0 ||
        write_event(device->fd, EV_ABS, ABS_Y, values->y) < 0)
        return -1;
    return 0;
}

/* Resolves the next frame - the parity advances on every frame so both axes
 * always differ from what the kernel already holds - and writes it. */
static int write_next_frame_axes(struct airplay_uinput *device,
                                 unsigned x, unsigned y,
                                 unsigned width, unsigned height)
{
    struct airplay_uinput_axis_values values;
    unsigned parity = (device->parity + 1u) & 1u;

    if (airplay_uinput_axis_values(x, y, width, height, parity, &values) < 0)
        return -1;
    device->parity = parity;
    return write_axis_values(device, &values);
}

static int send_position(struct airplay_uinput *device,
                         unsigned x, unsigned y,
                         unsigned width, unsigned height)
{
    if (!device || device->fd < 0)
        return -1;
    if (write_event(device->fd, EV_ABS, ABS_MT_SLOT, 0) < 0 ||
        write_next_frame_axes(device, x, y, width, height) < 0 ||
        write_event(device->fd, EV_SYN, SYN_REPORT, 0) < 0)
        return -1;
    return 0;
}

int airplay_uinput_touch_down(struct airplay_uinput *device,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height)
{
    if (!device || device->fd < 0 || device->touch_active)
        return -1;
    if (write_event(device->fd, EV_ABS, ABS_MT_SLOT, 0) < 0 ||
        write_event(device->fd, EV_ABS, ABS_MT_TRACKING_ID, 1) < 0 ||
        write_next_frame_axes(device, x, y, width, height) < 0 ||
        write_event(device->fd, EV_KEY, BTN_TOUCH, 1) < 0 ||
        write_event(device->fd, EV_SYN, SYN_REPORT, 0) < 0)
        return -1;
    device->touch_active = 1;
    return 0;
}

int airplay_uinput_touch_move(struct airplay_uinput *device,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height)
{
    if (!device || device->fd < 0 || !device->touch_active)
        return -1;
    return send_position(device, x, y, width, height);
}

int airplay_uinput_touch_up(struct airplay_uinput *device,
                            unsigned x, unsigned y,
                            unsigned width, unsigned height)
{
    if (!device || device->fd < 0 || !device->touch_active)
        return 0;
    if (send_position(device, x, y, width, height) < 0 ||
        write_event(device->fd, EV_ABS, ABS_MT_TRACKING_ID, -1) < 0 ||
        write_event(device->fd, EV_KEY, BTN_TOUCH, 0) < 0 ||
        write_event(device->fd, EV_SYN, SYN_REPORT, 0) < 0)
        return -1;
    device->touch_active = 0;
    return 0;
}


