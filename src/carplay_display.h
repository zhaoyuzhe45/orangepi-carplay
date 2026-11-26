#ifndef CARPLAY_DISPLAY_H
#define CARPLAY_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include "carplay_capabilities.h"

struct carplay_display_state {
    unsigned width;
    unsigned height;
    unsigned physical_width;
    unsigned physical_height;
    unsigned density;
    int size_override;
    int density_override;
};

struct carplay_display_result {
    unsigned prior_width;
    unsigned prior_height;
    unsigned prior_density;
    int prior_size_override;
    int prior_density_override;
    unsigned width;
    unsigned height;
    unsigned density;
    unsigned physical_width;
    unsigned physical_height;
    unsigned prior_physical_width;
    unsigned prior_physical_height;
    int changed;
    int native;
    char error[192];
};

int carplay_display_parse_size(const char *text,
                               struct carplay_display_state *state);
int carplay_display_parse_density(const char *text,
                                  struct carplay_display_state *state);
int carplay_display_format_size_arg(unsigned width, unsigned height,
                                    char *out, size_t capacity);
int carplay_display_format_density_arg(unsigned density,
                                       char *out, size_t capacity);
int carplay_display_format_physical_mode(unsigned width, unsigned height,
                                         unsigned fps, char *out,
                                         size_t capacity);
int carplay_display_mode_builtin(unsigned width, unsigned height);
int carplay_display_mode_supported(const char *text, unsigned width,
                                   unsigned height, unsigned fps);
int carplay_display_read(struct carplay_display_state *state);
/* Bounded `wm size` probe. Unlike carplay_display_read it does not touch
 * density and never waits on a wedged WindowManager for more than a second and
 * a half, so it is safe to call on the RTSP control thread. */
int carplay_display_probe(struct carplay_display_state *state);
/* 1 when WindowManager answers `wm size`, i.e. SurfaceFlinger and system_server
 * are alive. Screen capture is impossible while this returns 0. */
int carplay_display_framework_ready(void);
/* Compatibility query for an active runtime physical-mode transaction. */
int carplay_display_boot_provisioning_pending(void);
/* Call only after media/client sockets are retired. Does not wait for Android.
 * The returned request ID identifies the exact detached transaction. */
int carplay_display_start_preparation(const struct carplay_display_plan *plan,
                                      char *request_id, size_t capacity);
/* 1 matching verified completion; -1 matching failure; 0 not completed.
 * These functions read local text only and never call Android services. */
int carplay_display_parse_preparation_result(const char *text, const char *boot_id,
    const char *request_id, unsigned width, unsigned height, unsigned fps);
int carplay_display_preparation_result(const char *request_id, unsigned width,
                                       unsigned height, unsigned fps);
/* Asynchronously validate and cache a preset after a real keyframe was sent. */
int carplay_display_remember_mode(unsigned width, unsigned height, unsigned density);
/* Zero-initialize per stream. The encoder owner calls step only after sending
 * a real IDR. Completion is the helper's successful exit after cache sync.
 * Polling uses a nonblocking pipe, with at most three runtime-only attempts. */
struct carplay_display_memory {
    int fd, pending, complete;
    unsigned attempts;
    uint64_t next_ms, deadline_ms;
};
void carplay_display_remember_step(struct carplay_display_memory *memory,
    unsigned width, unsigned height, unsigned density, uint64_t now_ms);
/* Drop the completion reader without waiting for detached Android work. */
void carplay_display_remember_close(struct carplay_display_memory *memory);
/* Compatibility wrapper; new callers should retain the request ID. */
int carplay_display_request_mode(unsigned width, unsigned height, unsigned fps,
                                 unsigned density);
/* 1 another transaction active; -1 target failed this boot; 0 may proceed.
 * Zero dimensions query the active transaction only. No Android binder calls. */
int carplay_display_preparation_state(unsigned width, unsigned height, unsigned fps);
int carplay_display_apply(const struct carplay_display_plan *plan,
                          struct carplay_display_result *result);
/* Panel-mirror geometry: the largest rectangle with the canvas aspect that
 * still fits inside the panel - the canvas scaled UP to fill when it is
 * smaller, not the raw canvas (measured on this unit: SurfaceFlinger renders
 * a logical display smaller than the panel scaled up and centered, wm
 * 1280x600 composes at 0,90-1920,990 - so expressing the canvas directly
 * makes the composer rescale and the top-left SPS crop grab the wrong
 * region). A canvas larger than the panel has no such rectangle, so it maps
 * to the whole panel (the sink stretches what it receives). Even dimensions
 * throughout. screenrecord captures the PANEL at 1:1 under this policy and
 * the SPS frame-cropping rewrite removes the centered letterbox bars, so
 * nothing is ever rescaled in the encoder. */
int carplay_display_mirror_size(unsigned canvas_width, unsigned canvas_height,
                                unsigned panel_width, unsigned panel_height,
                                unsigned *mirror_width, unsigned *mirror_height);
/* Where the composer renders the logical display inside the panel: the
 * mirror rectangle from carplay_display_mirror_size, centered. Even origin.
 * This is PANEL space -- it describes what the local screen shows, not what
 * the capture contains; screenrecord samples the display frame itself and
 * puts it at the capture's top-left. */
int carplay_display_letterbox_origin(unsigned panel_width,
                                     unsigned panel_height,
                                     unsigned mirror_width,
                                     unsigned mirror_height, unsigned *x,
                                     unsigned *y);
/* The viewport rect in CAPTURE space: mirror_size at the capture origin.
 * Measured on this unit, a capture of a logical display smaller than the
 * panel starts at the frame's first row (constant +90 vs the panel, slope
 * 1.0), so the capture origin is 0,0 and the panel letterbox offset does not
 * apply. Cropping at the panel offset shifts the whole picture up. */
int carplay_display_viewport_rect(unsigned panel_width, unsigned panel_height,
                                  unsigned logical_width,
                                  unsigned logical_height, unsigned *x,
                                  unsigned *y, unsigned *width,
                                  unsigned *height);
/* Map a touch position from the coordinate space the sink SEES (the cropped
 * stream, whose top-left sits at stream_origin inside the panel) back to the
 * box's logical WindowManager space, whose viewport is composed at
 * viewport_origin with the given size. Touches above/left of the viewport
 * clamp to the origin. Returns 0, or -1 on a zero viewport. */
int carplay_display_input_coordinate(unsigned seen_x, unsigned seen_y,
                                     unsigned stream_origin_x,
                                     unsigned stream_origin_y,
                                     unsigned viewport_origin_x,
                                     unsigned viewport_origin_y,
                                     unsigned viewport_width,
                                     unsigned viewport_height,
                                     unsigned logical_width,
                                     unsigned logical_height, unsigned *x,
                                     unsigned *y);
/* Lift a point from the streamed viewport into the PHYSICAL panel frame, the
 * space the input stack actually maps this virtual touchscreen onto. Measured
 * on this unit (`dumpsys input`, wm 1920x900 on the 1920x1080 panel): the raw
 * 0..65535 axis maps to X 0..1919 and Y -90..989, i.e. the mapping target is
 * the panel expressed relative to the logical origin, not the 1920x900
 * logical display. Injecting logical pixels instead gets them scaled by
 * panel/viewport (1.2 here) and shifted by the letterbox, so the centre of
 * the screen still lands right while both edges drift away - "taps near the
 * top go too high, taps near the bottom go too low". Points outside the
 * viewport clamp to its last pixel. Returns 0, or -1 on a zero extent. */
int carplay_display_panel_point(unsigned stream_x, unsigned stream_y,
                                unsigned viewport_width,
                                unsigned viewport_height,
                                unsigned letterbox_x, unsigned letterbox_y,
                                unsigned panel_width, unsigned panel_height,
                                unsigned *x, unsigned *y);
/* Density that reproduces the canvas dp layout at the mirror size: the sink
 * plans its UI for canvas_height @ canvas_density, and dp is density
 * independent, so scaling by mirror_height / canvas_height keeps every layout
 * identical. Clamped to the 120..480 range `wm density` accepts. Returns 0
 * for a zero canvas. */
unsigned carplay_display_mirror_density(unsigned canvas_width,
                                        unsigned canvas_height,
                                        unsigned canvas_density,
                                        unsigned mirror_width,
                                        unsigned mirror_height);
/* Express the canvas as a WindowManager override no larger than the panel and
 * a compensated density; never touches the physical panel mode. result->width
 * /height carry the mirror size, physical_* the untouched panel, and native is
 * 1 only when the mirror equals the panel (no letterbox bars to crop). */
int carplay_display_apply_mirror(unsigned canvas_width, unsigned canvas_height,
                                 unsigned canvas_density,
                                 struct carplay_display_result *result);
int carplay_display_restore(const struct carplay_display_result *result);

#endif
