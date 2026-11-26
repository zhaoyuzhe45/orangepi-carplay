/*
 * airplay_client.c -- minimal CarPlay/iPhone AirPlay control client.
 *
 * The first state sends GET /info, validates the RTSP response framing, then
 * starts the HAP pair-setup exchange. Pair-Setup state 2 is decoded so the SRP
 * client can be added without changing the transport layer.
 */
#include "airplay_client.h"
#include "airplay_crypto.h"
#include "airplay_legacy.h"
#include "carplay_modes.h"
#include "carplay_now_playing.h"
#include "carplay_identity.h"
#include "airplay_protocol.h"
#include "airplay_bplist.h"
#include "carplay_capabilities.h"
#include "carplay_display.h"
#include "carplay_audio_bridge.h"
#include "carplay_vehicle_state.h"
#include "airplay_video.h"
#include "airplay_timing.h"
#include "airplay_screen_fixture.h"
#include "airplay_h264.h"
#include "carplay_encoder.h"
#include "carplay_video_recovery.h"
#include "carplay_config.h"
#include <stdatomic.h>
#include "airplay_input.h"
#include "airplay_uinput.h"
#include "carplay_knob.h"
#include "carplay_keys.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void iap2_log(const char *fmt, ...);

#define AIRPLAY_RESPONSE_BUFFER 131072u
#define AIRPLAY_CONTROL_MAX_PAYLOAD 0x4000u
#define AIRPLAY_SEND_TIMEOUT_MS 3000u

/*
 * SurfaceFlinger only composites when something changes, so a still desktop
 * produces almost no H.264 output: measured on the OPI4 box at 1280x600, ten
 * seconds of a motionless launcher yielded ~23 KB (one access unit) while ten
 * seconds of continuous swiping yielded ~880 KB. Silence alone cannot tell a
 * still picture from a stalled capture path; encoder heartbeat and staged
 * keyframe/capture probes provide that distinction.
 *
 * The phone keeps the last decoded picture, so the pipeline simply refreshes the
 * most recent keyframe once a second instead of painting over the real desktop
 * with the diagnostic fixture. The fixture is only ever sent before the first
 * real access unit arrives, where it still proves the crypto/packet path works.
 */
#define STILL_FRAME_REFRESH_MS 1000u
#define PLACEHOLDER_FIRST_MS 500u

/* How long a buffered picture has to stop growing before it is pushed without
 * waiting for the next one. Slices of one picture arrive in the same read burst,
 * while a picture interval at 30 fps is 33 ms, so this can never split a picture
 * yet still releases the last picture of a scroll burst promptly. */
#define PICTURE_IDLE_FLUSH_MS 100u
/* A touch whose up report never arrived. When the next down shows up after
 * this long a gap, Android is still holding a phantom finger at the old
 * position; releasing it first is what keeps a tap from turning into a drag
 * from a stale location. */
#define TOUCH_LOST_UP_TIMEOUT_MS 2000u
/* A jump between consecutive move targets wider than this gets logged as an
 * anomaly. Normal finger movement never teleports this far inside one report,
 * so the log stays quiet during drags but still catches a misparsed report. */
#define TOUCH_JUMP_LOG_PX 96u

/* Defined near the video pipeline; the touch state machine needs it for the
 * lost-up watchdog. */
static uint64_t video_monotonic_ms(void);

/* A failing encoder must not spin the reader: real restarts cost an OMX
 * re-initialisation (1-2 s of black) so space them out. */
#define ENCODER_RESTART_MIN_MS 2000u

/* The phone asks for a fresh keyframe right after SETUP and again on rotation.
 * Coalesce bursts while keeping the running MediaCodec instance alive. */
#define FORCE_KEY_FRAME_MIN_MS 200u

static const uint8_t controller_identifier[] = "DiPlay-CarPlay-Board";

static uint64_t screen_sender_ntp(void);

struct airplay_client {
	int fd;
	enum airplay_profile profile;
	int wants_hap_fallback;
	uint64_t handshake_deadline_ms;
	unsigned pending_handshake_cseq;
	uint64_t last_feedback_ms;
	uint64_t feedback_deadline_ms;
	unsigned next_control_cseq;
	unsigned pending_feedback_cseq;
	unsigned post_handshake_refusals;
	unsigned pending_initial_modes_cseq;
	uint64_t initial_modes_deadline_ms;
	int initial_modes_accepted;
	int keepalive_fd;
	int keepalive_wake[2];
	pthread_t keepalive_thread;
	int keepalive_started;
	atomic_int keepalive_stop;
	int keepalive_stats_body;
	struct airplay_legacy_cipher *legacy_video;
	struct carplay_modes modes;
	atomic_int screen_suspended;
	/* A full borrow/resume can occur between video worker samples. */
	atomic_uint screen_resume_generation;
	atomic_int screen_needs_keyframe;
	char audio_host[128];
	int audio_setup_deferred;
	int audio_setup_pending;
	int audio_setup_failed;
	uint64_t audio_setup_deadline_ms;
	/* Traditional MainAudio transaction: teardown(1), modes(2), setup(3),
	 * modes accepted/waiting for ownership(4). Only control owner mutates it. */
	int legacy_audio_phase;
	unsigned legacy_audio_cseq;
	uint64_t legacy_audio_deadline;
	uint64_t legacy_audio_retry_ms;
	int legacy_audio_target;
	int legacy_audio_have_stream;
	int legacy_audio_refused_call;
	int legacy_audio_last_demand;
	unsigned legacy_audio_failures;
	/* Once announced, phoneCall may remain at the receiver until a clear is
	 * acknowledged; an optimistic local modes value cannot discharge it. */
	int legacy_audio_call_announced;
	unsigned legacy_audio_release_failures;
	unsigned legacy_nav_cseq;
	uint64_t legacy_nav_deadline;
	int legacy_nav_attempted;
	struct airplay_timing *timing;
	int event_fd;
	int info_requested;
	int info_complete;
	int pair_setup_requested;
	int pair_setup_m5_sent;
	int pair_setup_complete;
	int pairing_proven;
	int controller_key_ready;
	int pair_verify_requested;
	int pair_verify_m2_verified;
	int pair_verify_m3_sent;
	int pair_verify_complete;
	int control_channel_ready;
	int auth_setup_sent;
	int auth_setup_complete;
	int sap_certificate_len;
	int sap_signature_len;
	int setup_sent;
	int setup_complete;
	int screen_setup_sent;
	int screen_setup_complete;
	int record_sent;
	int record_accepted;
	/* Set when a handshake step was answered with a non-200 status.  The
	 * pipeline cannot make progress after that -- nothing else is queued and
	 * the RTSP thread only polls -- so the session has to be torn down
	 * instead of being left spinning.  See airplay_client_failed(). */
	int failed;
	char failure[96];
	int test_screen_sent;
	/* Timestamp of the last picture the phone received, real capture or
	 * placeholder. Drives the placeholder repeat so a silent capture path
	 * never leaves the phone on a frozen frame. */
	uint64_t last_screen_frame_ms;
	int live_screen_attempted;
	int live_screen_started;
	int display_override_width;
	int display_override_height;
	int display_density_override;
	int physical_override_width;
	int physical_override_height;
	int configured_fps;
	int bitrate;
	int actual_fps;
	int actual_density;
	int display_native;
	unsigned requested_width;
	unsigned requested_height;
	unsigned physical_width;
	unsigned physical_height;
	char display_error[192];
	uint8_t *video_batch;
	size_t video_batch_len;
	size_t video_batch_capacity;
	uint64_t video_batch_start_ms;
	uint64_t video_batch_last_append_ms;
	unsigned long video_batch_nals;
	unsigned long video_batch_packets;
	unsigned video_batch_slices;
	int video_batch_keyframe;
	unsigned long flushed_pictures;
	atomic_int live_screen_stop;
	atomic_int force_key_frame;
	/* Video worker reports failure; only the control owner aborts the session. */
	atomic_int video_failure;
	/* Retire the diagnostic fixture once a real access unit enters the batch.
	 * Cached keyframe refreshes preserve that picture during idle periods;
	 * the recovery policy separately checks for fresh frames on the wire. */
	int has_real_frame;
	uint8_t *last_keyframe;
	size_t last_keyframe_len;
	unsigned long last_keyframe_nals;
	uint64_t last_keyframe_ms;
	uint64_t last_capture_read_ms;
	/* 1 while the stored keyframe is still the newest picture put on the wire.
	 * Resending an older one rewinds the decoder to a state the following
	 * deltas do not reference, which is a self-inflicted mosaic. */
	int last_keyframe_current;
	uint64_t last_encoder_spawn_ms;
	uint64_t last_force_key_frame_ms;
	unsigned encoder_restarts;
	unsigned placeholder_frames;
	unsigned still_refreshes;
	int touch_active;
	int touch_direct_active;
	unsigned touch_start_x;
	unsigned touch_start_y;
	unsigned touch_last_x;
	unsigned touch_last_y;
	unsigned touch_last_logged_x;
	unsigned touch_last_logged_y;
	uint64_t touch_last_report_ms;
	uint8_t keyboard_report[8];
	int keyboard_report_present;
	unsigned input_width;
	unsigned input_height;
	/* Panel-mirror capture: the size screenrecord samples at - the panel's
	 * own mode, 1:1 - while input_* is the WindowManager mirror the canvas
	 * was expressed as. The difference is the letterbox the SPS crop
	 * removes before the sink sees the stream. */
	unsigned capture_width;
	unsigned capture_height;
	/* Where the composer actually renders the logical display inside the
	 * panel (scale-and-center), and where the streamed region starts - the
	 * two agree under the fill-mirror policy, but the touch mapping must
	 * stay correct even while they differ. */
	unsigned viewport_x;
	unsigned viewport_y;
	unsigned viewport_width;
	unsigned viewport_height;
	/* Where the viewport sits inside the physical panel. The input stack
	 * maps the virtual touchscreen onto the panel frame, so injection adds
	 * this back (see carplay_display_panel_point). */
	unsigned panel_origin_x;
	unsigned panel_origin_y;
	unsigned stream_crop_x;
	unsigned stream_crop_y;
	struct airplay_uinput *touch_input;
	struct carplay_keys *key_input;
	struct carplay_knob_layout knob_layouts[CARPLAY_MAX_HID_DEVICES];
	struct carplay_knob_state knob_states[CARPLAY_MAX_HID_DEVICES];
	struct carplay_display_plan pending_display_plan;
	int display_prepare_requested;
	int display_profile_eligible;
	struct carplay_display_memory display_memory;
	unsigned output_width, output_height;
	unsigned long knob_reports, knob_rejected;
	unsigned knob_buttons;
	uint8_t media_reports[CARPLAY_MAX_HID_DEVICES];
	uint8_t media_report;
	struct carplay_phone_hid_state phone_reports[CARPLAY_MAX_HID_DEVICES];
	signed char knob_x;
	signed char knob_y;
	signed char knob_wheel;
	struct carplay_encoder encoder;
	pthread_t encoder_thread;
	pthread_mutex_t video_send_mutex;
	pthread_mutex_t event_send_mutex;
	unsigned event_request_cseq;
	unsigned long live_config_count;
	uint8_t latest_screen_config[1024];
	size_t latest_screen_config_len;
	unsigned latest_screen_config_width;
	unsigned latest_screen_config_height;
	int screen_config_pending;
	unsigned long live_frame_count;
	uint16_t timing_port;
	uint16_t event_port;
	uint16_t keep_alive_port;
	uint16_t screen_data_port;
	int keep_alive_port_present;
	int screen_data_fd;
	uint64_t stream_connection_id;
	uint8_t video_key[AIRPLAY_CHACHA_KEY_BYTES];
	struct airplay_x25519_keypair sap_ephemeral;
	uint8_t sap_shared_secret[AIRPLAY_X25519_PUBLIC_BYTES];
	struct airplay_ed25519_keypair controller_key;
	struct airplay_x25519_keypair controller_ephemeral;
	struct carplay_session_capabilities capabilities;
	struct carplay_audio_bridge *audio_bridge;
	struct {
		int type;
		char audio_type[CARPLAY_UUID_CAPACITY];
		uint16_t data_port;
		uint16_t control_port;
	} audio_ports[CARPLAY_AUDIO_MAX_STREAMS];
	size_t audio_port_count;
	int enable_media_audio;
	int enable_microphone;
	struct carplay_vehicle_state *vehicle_state;
	uint64_t vehicle_generation;
	uint8_t pair_setup_session_key[AIRPLAY_SHA512_BYTES];
	uint8_t pair_setup_encryption_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t server_proof[AIRPLAY_SHA512_BYTES];
	uint8_t accessory_public_key[AIRPLAY_ED25519_PUBLIC_BYTES];
	uint8_t accessory_identifier[64];
	size_t accessory_identifier_len;
	uint8_t accessory_ephemeral_public_key[AIRPLAY_X25519_PUBLIC_BYTES];
	uint8_t pair_verify_shared_secret[AIRPLAY_X25519_PUBLIC_BYTES];
	uint8_t pair_verify_encryption_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t control_read_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t control_write_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t event_read_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t event_write_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint64_t control_read_counter;
	uint64_t control_write_counter;
	uint64_t event_read_counter;
	uint64_t event_write_counter;
	uint64_t video_write_counter;
	uint8_t response[AIRPLAY_RESPONSE_BUFFER];
	size_t response_len;
	uint8_t wire_response[AIRPLAY_RESPONSE_BUFFER];
	size_t wire_response_len;
	uint8_t event_response[AIRPLAY_RESPONSE_BUFFER];
	size_t event_response_len;
	uint8_t event_wire_response[AIRPLAY_RESPONSE_BUFFER];
	size_t event_wire_response_len;
};

/* Pass the same start time for every write of one logical message. */
static int send_all_since(int fd, const void *data, size_t len, uint64_t started)
{
	const uint8_t *p = data;
	size_t sent = 0;

	while (sent < len) {
		if (video_monotonic_ms() - started >= AIRPLAY_SEND_TIMEOUT_MS) {
			errno = ETIMEDOUT;
			break;
		}
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			uint64_t elapsed = video_monotonic_ms() - started;
			if (elapsed >= AIRPLAY_SEND_TIMEOUT_MS) {
				errno = ETIMEDOUT;
				break;
			}
			int ready = poll(&pfd, 1, (int)(AIRPLAY_SEND_TIMEOUT_MS - elapsed));
			if (ready > 0 && (pfd.revents & POLLOUT))
				continue;
			if (ready < 0 && errno == EINTR)
				continue;
			if (ready == 0) errno = ETIMEDOUT;
			else if (ready > 0) errno = EPIPE;
		}
		if (n == 0) errno = EPIPE;
		break;
	}
	if (sent == len) return 0;
	int saved = errno;
	/* A partially sent encrypted record cannot be followed by another record.
	 * Wake every user of this socket so the connector retires the session. */
	(void)shutdown(fd, SHUT_RDWR);
	iap2_log("airplay: send failed after %zu/%zu bytes: %s", sent, len, strerror(saved));
	errno = saved;
	return -1;
}

static int send_all(int fd, const void *data, size_t len)
{
	return send_all_since(fd, data, len, video_monotonic_ms());
}

static unsigned clamp_coordinate(unsigned value, unsigned limit)
{
	if (!limit)
		return 0;
	return value >= limit ? limit - 1 : value;
}

static int apply_native_display(struct airplay_client *client)
{
    struct carplay_display_plan plan, native_plan;
    struct carplay_display_result result;
    if (carplay_display_preparation_state(0, 0, 0) != 0) {
        snprintf(client->display_error, sizeof(client->display_error), "physical mode transaction active");
        return -1;
    }
    if (carplay_capabilities_display_plan(&client->capabilities, 800, 480,
            client->display_override_width, client->display_override_height,
            client->physical_override_width, client->physical_override_height,
            client->display_density_override, client->configured_fps, &plan) < 0) {
        snprintf(client->display_error, sizeof(client->display_error),
                 "display plan rejected: receiver=%dx%d, encoder limit=%ux%u",
                 client->capabilities.display.width_pixels,
                 client->capabilities.display.height_pixels,
                 CARPLAY_ENCODER_MAX_WIDTH, CARPLAY_ENCODER_MAX_HEIGHT);
        return -1;
    }
    client->output_width = plan.output_width;
    client->output_height = plan.output_height;
    client->requested_width = plan.width;
    client->requested_height = plan.height;
    iap2_log("display: preset=%ux%u canvas=%ux%u output=%ux%u fps=%u density=%u stretched=%d",
             plan.physical_width, plan.physical_height, plan.width, plan.height,
             plan.output_width, plan.output_height,
             plan.fps, plan.density, plan.stretched);
    native_plan = plan;
    native_plan.width = plan.physical_width;
    native_plan.height = plan.physical_height;
    int applied = carplay_display_apply(&native_plan, &result);
    if (applied != 0) {
        snprintf(client->display_error, sizeof(client->display_error), "%s",
                 result.error[0] ? result.error : "native display unavailable");
        if (applied == 1 && carplay_display_preparation_state(plan.physical_width,
                plan.physical_height, plan.physical_fps) == 0) {
            client->pending_display_plan = plan;
            client->display_prepare_requested = 1;
        }
        return -1;
    }
    client->display_native = !plan.stretched;
    client->physical_width = result.physical_width;
    client->physical_height = result.physical_height;
    client->actual_fps = plan.fps;
    client->actual_density = result.density;
    client->input_width = result.width;
    client->input_height = result.height;
    client->capture_width = result.physical_width;
    client->capture_height = result.physical_height;
    client->viewport_width = result.width;
    client->viewport_height = result.height;
    client->viewport_x = client->viewport_y = 0;
    client->stream_crop_x = client->stream_crop_y = 0;
    client->panel_origin_x = client->panel_origin_y = 0;
    snprintf(client->display_error, sizeof(client->display_error), "%s",
             plan.stretched ? "closest preset stretched to receiver canvas" : "ok");
    return 0;
}

static void prepare_knob_layouts(struct airplay_client *client, const uint8_t *data, size_t len)
{
    struct airplay_bplist_document doc;
    struct airplay_bplist_value root, devices, item, value;
    uint64_t count = 0;
    if (airplay_bplist_init(&doc, data, len) != 0 || airplay_bplist_root(&doc, &root) != 0 ||
        airplay_bplist_dict_get(&doc, &root, "hidDevices", &devices) != 1 ||
        airplay_bplist_array_count(&doc, &devices, &count) != 0) return;
    for (uint64_t i = 0; i < count && i < CARPLAY_MAX_HID_DEVICES; i++) {
        char uuid[CARPLAY_UUID_CAPACITY];
        size_t uuid_len = 0;
        if (airplay_bplist_array_get(&doc, &devices, i, &item) != 0 ||
            airplay_bplist_dict_get(&doc, &item, "uuid", &value) != 1 ||
            airplay_bplist_string_copy(&doc, &value, uuid, sizeof(uuid), &uuid_len) != 0) continue;
        const struct carplay_hid_device_capabilities *device =
            carplay_capabilities_find_hid(&client->capabilities, uuid);
        if (!device || device->kind != CARPLAY_HID_KNOB ||
            airplay_bplist_dict_get(&doc, &item, "hidDescriptor", &value) != 1 ||
            value.type != AIRPLAY_BPLIST_DATA || value.data_offset > len ||
            value.count > len - value.data_offset) continue;
        size_t index = (size_t)(device - client->capabilities.hid);
        int parsed = carplay_knob_parse(data + value.data_offset, (size_t)value.count,
                                       &client->knob_layouts[index]);
        iap2_log("airplay input: knob descriptor uuid=%s parsed=%d fields=%u", uuid,
                 parsed, client->knob_layouts[index].field_count);
    }
    if (client->capabilities.has_knob && !client->key_input &&
        carplay_keys_create(&client->key_input, NULL) != 0)
        iap2_log("airplay input: persistent key device unavailable errno=%d", errno);
}

static void prepare_input_environment(void)
{
	setenv("PATH",
	       "/vendor/xbin:/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin",
	       1);
	setenv("ANDROID_ROOT", "/system", 0);
	setenv("ANDROID_DATA", "/data", 0);
	setenv("ANDROID_ASSETS", "/system/app", 0);
	setenv("ANDROID_STORAGE", "/storage", 0);
}

static int wait_input_child(pid_t pid, const char *action)
{
	int status = 0;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		iap2_log("airplay input: waitpid(%s) failed: %s", action,
			 strerror(errno));
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		iap2_log("airplay input: %s failed status=0x%x", action, status);
		return -1;
	}
	return 0;
}

/* Use Android's native cmd input dispatcher here. The legacy shebangless
 * /system/bin/input wrapper starts a Java app_process for every event, adding
 * visible input latency on this image. */
static int run_input_keycode(int keycode)
{
	char value[16];
	pid_t pid;

	if (keycode <= 0)
		return 0;
	snprintf(value, sizeof(value), "%d", keycode);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", value,
		      (char *)NULL);
		_exit(127);
	}
	return wait_input_child(pid, "keyevent");
}

static int run_input_text(int character)
{
	char text[2];
	pid_t pid;

	if (character <= 0 || character > 0x7f)
		return 0;
	text[0] = (char)character;
	text[1] = '\0';
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl("/system/bin/sh", "sh", "/system/bin/input", "text", text,
		      (char *)NULL);
		_exit(127);
	}
	return wait_input_child(pid, "text");
}

static int run_input_touch(unsigned start_x, unsigned start_y,
			   unsigned end_x, unsigned end_y)
{
	char x1[16], y1[16], x2[16], y2[16];
	int horizontal = (int)end_x - (int)start_x;
	int vertical = (int)end_y - (int)start_y;
	int is_swipe = horizontal * horizontal + vertical * vertical > 400;
	pid_t pid;

	snprintf(x1, sizeof(x1), "%u", start_x);
	snprintf(y1, sizeof(y1), "%u", start_y);
	if (is_swipe) {
		snprintf(x2, sizeof(x2), "%u", end_x);
		snprintf(y2, sizeof(y2), "%u", end_y);
	} else {
		snprintf(x2, sizeof(x2), "%u", start_x);
		snprintf(y2, sizeof(y2), "%u", start_y);
	}
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (is_swipe) {
			execl("/system/bin/sh", "sh", "/system/bin/input", "swipe", x1,
			      y1, x2, y2,
			      "120", (char *)NULL);
		} else {
			execl("/system/bin/sh", "sh", "/system/bin/input", "tap", x1, y1,
			      (char *)NULL);
		}
		_exit(127);
	}
	return wait_input_child(pid, is_swipe ? "swipe" : "tap");
}

static unsigned scale_axis(unsigned value, unsigned source, unsigned target)
{
	return clamp_coordinate((unsigned)(((uint64_t)value * target) / source),
				target);
}

/* The two injection paths disagree about space: `input tap` takes logical
 * display pixels, while the uinput ABS axis is mapped by the input stack onto
 * the physical PANEL frame (measured: `dumpsys input` reports Y -90..989 for a
 * 1920x900 override on the 1920x1080 panel). Track the finger in logical space
 * - that is what the shell fallback needs - and lift it into panel space only
 * for the uinput write. Injecting logical pixels there gets them scaled by
 * panel/viewport and shifted by the letterbox, so the centre of the screen
 * lands right while both edges drift away. */
static void touch_panel_point(const struct airplay_client *client,
			      unsigned logical_x, unsigned logical_y,
			      unsigned *x, unsigned *y, unsigned *width,
			      unsigned *height)
{
	unsigned px = logical_x;
	unsigned py = logical_y;

	if (carplay_display_panel_point(logical_x, logical_y,
					client->viewport_width,
					client->viewport_height,
					client->panel_origin_x,
					client->panel_origin_y,
					client->capture_width,
					client->capture_height, &px, &py) < 0) {
		px = logical_x;
		py = logical_y;
	}
	*x = px;
	*y = py;
	*width = client->capture_width ? client->capture_width
				       : client->input_width;
	*height = client->capture_height ? client->capture_height
					 : client->input_height;
}

static void handle_touch_report(
	struct airplay_client *client,
	const struct carplay_hid_device_capabilities *touch_caps,
	const uint8_t *report, size_t report_len)
{
	struct airplay_input_touch touch;
	unsigned x;
	unsigned y;

	if (airplay_input_touch_point(report, report_len, &touch) != 1)
		return;
	unsigned source_width = client->capabilities.display.present ?
		(unsigned)client->capabilities.display.width_pixels : 1920u;
	unsigned source_height = client->capabilities.display.present ?
		(unsigned)client->capabilities.display.height_pixels : 1080u;
	if (touch_caps && touch_caps->touch_x_max)
		source_width = touch_caps->touch_x_max;
	if (touch_caps && touch_caps->touch_y_max)
		source_height = touch_caps->touch_y_max;
	x = scale_axis(touch.x, source_width, client->input_width);
	y = scale_axis(touch.y, source_height, client->input_height);
	/* The sink reports what it SEES; Android injects in the logical
	 * WindowManager space. Invert the composer's scale-and-center
	 * placement (identity while the stream presents the viewport 1:1,
	 * the fill correction otherwise). */
	if (client->viewport_width && client->viewport_height) {
		unsigned mapped_x = 0;
		unsigned mapped_y = 0;

		if (carplay_display_input_coordinate(
			    x, y, client->stream_crop_x, client->stream_crop_y,
			    client->viewport_x, client->viewport_y,
			    client->viewport_width, client->viewport_height,
			    client->input_width, client->input_height,
			    &mapped_x, &mapped_y) == 0) {
			x = mapped_x;
			y = mapped_y;
		}
	}
	if (touch.down) {
		uint64_t now_ms = video_monotonic_ms();

		if (client->touch_active &&
		    now_ms - client->touch_last_report_ms >
			    TOUCH_LOST_UP_TIMEOUT_MS) {
			/* The up report never arrived, yet Android is still
			 * holding the previous finger. Lift the phantom at its
			 * last known position before this down lands, or the
			 * new touch merges into a drag from a stale location. */
			iap2_log("airplay input: touch up was lost; releasing the phantom finger at %u,%u",
				 client->touch_last_x, client->touch_last_y);
			if (client->touch_direct_active) {
				unsigned ix;
				unsigned iy;
				unsigned iw;
				unsigned ih;

				touch_panel_point(client, client->touch_last_x,
						  client->touch_last_y, &ix, &iy,
						  &iw, &ih);
				(void)airplay_uinput_touch_up(
					client->touch_input, ix, iy, iw, ih);
			}
			client->touch_active = 0;
			client->touch_direct_active = 0;
		}
		client->touch_last_report_ms = now_ms;
		if (!client->touch_active) {
			unsigned ix = x;
			unsigned iy = y;
			unsigned iw = client->input_width;
			unsigned ih = client->input_height;
			int direct = 0;

			client->touch_start_x = x;
			client->touch_start_y = y;
			client->touch_last_x = x;
			client->touch_last_y = y;
			client->touch_active = 1;
			client->touch_direct_active = 0;
			if (client->touch_input) {
				touch_panel_point(client, x, y, &ix, &iy, &iw,
						  &ih);
				if (airplay_uinput_touch_down(
					    client->touch_input, ix, iy, iw,
					    ih) == 0) {
					direct = 1;
				} else {
					airplay_uinput_close(client->touch_input);
					client->touch_input = NULL;
				}
			}
			if (direct) {
				client->touch_direct_active = 1;
				iap2_log("airplay input: touch down raw=%u,%u target=%u,%u panel=%u,%u mode=uinput",
					 touch.x, touch.y, x, y, ix, iy);
			} else {
				iap2_log("airplay input: touch down raw=%u,%u target=%u,%u mode=fallback",
					 touch.x, touch.y, x, y);
			}
			return;
		}
		if (x != client->touch_last_x || y != client->touch_last_y) {
			client->touch_last_x = x;
			client->touch_last_y = y;
			if (client->touch_direct_active) {
				unsigned ix;
				unsigned iy;
				unsigned iw;
				unsigned ih;

				touch_panel_point(client, x, y, &ix, &iy, &iw,
						  &ih);
				if (airplay_uinput_touch_move(
					    client->touch_input, ix, iy, iw,
					    ih) < 0)
					iap2_log("airplay input: uinput move failed target=%u,%u",
						 x, y);
			}
		}
		/* Anomaly-only trace for the edge-sticking drag report: a move
		 * that teleports more than one workspace cell between
		 * consecutive events is what a misparsed report or a phantom
		 * finger looks like in the log. Ordinary drags stay silent. */
		if (x > client->touch_last_logged_x + TOUCH_JUMP_LOG_PX ||
		    x + TOUCH_JUMP_LOG_PX < client->touch_last_logged_x ||
		    y > client->touch_last_logged_y + TOUCH_JUMP_LOG_PX ||
		    y + TOUCH_JUMP_LOG_PX < client->touch_last_logged_y) {
			client->touch_last_logged_x = x;
			client->touch_last_logged_y = y;
			iap2_log("airplay input: touch move jump raw=%u,%u target=%u,%u report=%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x",
				 touch.x, touch.y, x, y,
				 report_len > 0 ? report[0] : 0,
				 report_len > 1 ? report[1] : 0,
				 report_len > 2 ? report[2] : 0,
				 report_len > 3 ? report[3] : 0,
				 report_len > 4 ? report[4] : 0,
				 report_len > 5 ? report[5] : 0,
				 report_len > 6 ? report[6] : 0,
				 report_len > 7 ? report[7] : 0,
				 report_len > 8 ? report[8] : 0,
				 report_len > 9 ? report[9] : 0,
				 report_len > 10 ? report[10] : 0,
				 report_len > 11 ? report[11] : 0);
		}
		return;
	}
	if (client->touch_active) {
		/* DiPlay answers with an all-zero contact when its contact
		 * list is already empty. The finger must lift where it was
		 * tracked last, not teleport to the top-left corner of the
		 * screen; the final position event before BTN_TOUCH=0 would
		 * otherwise drag whatever is under the pointer to (0,0). */
		if (!touch.x && !touch.y) {
			x = client->touch_last_x;
			y = client->touch_last_y;
		}
		iap2_log("airplay input: touch up raw=%u,%u target=%u,%u start=%u,%u mode=%s",
			 touch.x, touch.y, x, y, client->touch_start_x,
			 client->touch_start_y,
			 client->touch_direct_active ? "uinput" : "fallback");
		if (client->touch_direct_active) {
			unsigned ix;
			unsigned iy;
			unsigned iw;
			unsigned ih;

			client->touch_last_x = x;
			client->touch_last_y = y;
			touch_panel_point(client, x, y, &ix, &iy, &iw, &ih);
			(void)airplay_uinput_touch_up(client->touch_input, ix,
						      iy, iw, ih);
		} else {
			(void)run_input_touch(client->touch_start_x,
					      client->touch_start_y,
					      client->touch_last_x,
					      client->touch_last_y);
		}
		client->touch_active = 0;
		client->touch_direct_active = 0;
	}
}

static void handle_keyboard_report(
	struct airplay_client *client,
	const uint8_t *report, size_t report_len)
{
	static const uint8_t released[8] = { 0 };
	struct airplay_input_keyboard_change changes[8];
	const uint8_t *previous = client->keyboard_report_present ?
		client->keyboard_report : released;
	int count;
	int i;

	count = airplay_input_keyboard_changed(previous, 8, report, report_len,
					       changes, 8);
	if (count < 0) {
		iap2_log("airplay input: malformed keyboard report len=%zu",
			 report_len);
		return;
	}
	for (i = 0; i < count; i++) {
		if (changes[i].character) {
			iap2_log("airplay input: keyboard character=0x%02x",
				 (unsigned)changes[i].character);
			(void)run_input_text(changes[i].character);
		} else if (changes[i].keycode) {
			iap2_log("airplay input: keyboard keycode=%d",
				 changes[i].keycode);
			(void)run_input_keycode(changes[i].keycode);
		}
	}
	memcpy(client->keyboard_report, report, 8);
	client->keyboard_report_present = 1;
}

static void handle_hid_report(struct airplay_client *client,
			      const struct airplay_input_command *command)
{
	const struct carplay_hid_device_capabilities *device =
		carplay_capabilities_find_hid(&client->capabilities,
					      command->uuid);
	enum carplay_hid_kind kind = device ? device->kind :
		(strcmp(command->uuid, "2a2a2a2a") == 0 ? CARPLAY_HID_TOUCH :
		 strcmp(command->uuid, "2a2a2a2b") == 0 ? CARPLAY_HID_KNOB :
		 strcmp(command->uuid, "2a2a2a2c") == 0 ? CARPLAY_HID_MEDIA :
		 CARPLAY_HID_UNKNOWN);

	if(device && device->phone.valid) {
		size_t index=(size_t)(device-client->capabilities.hid);
		int phone_report=carplay_phone_hid_owns_report(&device->phone,command->report,command->report_len);
		int pressed=carplay_phone_hid_press(&device->phone,&client->phone_reports[index],
			command->report,command->report_len);
		/* Conflicting Hook+Drop never chooses an arbitrary phone action. */
		if(pressed==1 || pressed==2) {
			int result=carplay_app_command(6,(uint32_t)pressed);
			iap2_log("airplay input: telephone key=%d result=%d; never replaying",pressed,result);
		}
		/* A composite phone report cannot also be interpreted as a media byte
		 * or knob movement, including release and malformed payloads. */
		if(phone_report || kind==CARPLAY_HID_TELEPHONY)return;
	}
	/* Borrowing the screen suspends its inputs, not independent audio keys. */
	if (atomic_load(&client->screen_suspended) &&
	    kind != CARPLAY_HID_MEDIA && kind != CARPLAY_HID_TELEPHONY)return;
	if (kind == CARPLAY_HID_TOUCH) {
		handle_touch_report(client, device, command->report,
				    command->report_len);
		return;
	}
	if (kind == CARPLAY_HID_KEYBOARD) {
		handle_keyboard_report(client, command->report,
				       command->report_len);
		return;
	}
	if (kind == CARPLAY_HID_KNOB) {
        struct carplay_key_batch batch;
        client->knob_reports++;
        if (!device) { client->knob_rejected++; return; }
        size_t index = (size_t)(device - client->capabilities.hid);
        if (carplay_knob_decode(&client->knob_layouts[index], &client->knob_states[index],
                command->report, command->report_len, &batch) != 0) {
            client->knob_rejected++;
            return;
        }
        if (batch.count && carplay_keys_submit(client->key_input, &batch) != 0) {
            client->knob_rejected++;
            iap2_log("airplay input: knob batch rejected reports=%lu rejected=%lu",
                     client->knob_reports, client->knob_rejected);
        }
        return;
    }
	if (kind == CARPLAY_HID_MEDIA && command->report_len >= 1) {
		const int descriptor_backed = device && device->media.valid;
		uint8_t value = command->report[0];
		uint8_t *last = device ?
			&client->media_reports[device - client->capabilities.hid] :
			&client->media_report;
		int keycode;

		if ((descriptor_backed && command->report_len != 1) || *last == value)
			return;
		*last = value;
		keycode = descriptor_backed ? device->media.keycodes[value] :
			airplay_input_media_keycode(value);
		if (keycode || (descriptor_backed && value))
			iap2_log("airplay input: media report=%u keycode=%d uuid=%s mapping=%s",
				 value, keycode, command->uuid,
				 descriptor_backed ? "descriptor" : "legacy");
		if(keycode) {
			int result=carplay_app_command(1,(uint32_t)keycode);
			if(result==-1)(void)run_input_keycode(keycode);
			else if(result<0)iap2_log("airplay input: media command delivery uncertain; not replaying");
		}
	}
}

static void apply_car_night_mode(struct airplay_client *client, int night)
{
    if (!client->vehicle_state) return;
    if (carplay_vehicle_state_request_night(client->vehicle_state,
            client->vehicle_generation, night) < 0)
        iap2_log("airplay: could not queue car night mode for current session");
}

static void handle_control_command(struct airplay_client *client,
				   const uint8_t *body, size_t body_len)
{
	struct airplay_input_command command;

	if (airplay_input_parse(body, body_len, &command) != 1) {
		iap2_log("airplay input: malformed control command body=%zu", body_len);
		return;
	}
	switch (command.kind) {
	case AIRPLAY_INPUT_NIGHT_MODE:
		iap2_log("airplay input: setNightMode enabled=%d",
			 command.boolean_value);
		apply_car_night_mode(client, command.boolean_value);
		break;
	case AIRPLAY_INPUT_FORCE_KEY_FRAME:
		client->force_key_frame = 1;
		iap2_log("airplay input: forceKeyFrame requested");
		break;
	case AIRPLAY_INPUT_HID_REPORT:
		handle_hid_report(client, &command);
		break;
	case AIRPLAY_INPUT_SIRI:
		if(carplay_app_command(2,command.siri_action)<0)
			iap2_log("airplay input: Android voice bridge unavailable action=%u",command.siri_action);
		break;
	case AIRPLAY_INPUT_OTHER:
	default:
		iap2_log("airplay input: command type=%s", command.uuid);
		break;
	}
}

static void nonce64(uint64_t counter, uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES])
{
	size_t i;

	memset(nonce, 0, AIRPLAY_CHACHA_NONCE_BYTES);
	for (i = 4; i < AIRPLAY_CHACHA_NONCE_BYTES; i++) {
		nonce[i] = (uint8_t)counter;
		counter >>= 8;
	}
}

static int send_encrypted_frames(int fd,
				 const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
				 uint64_t *counter,
				 const uint8_t *plaintext, size_t len,
				 uint64_t started)
{
	uint8_t sealed[AIRPLAY_CONTROL_MAX_PAYLOAD + AIRPLAY_CHACHA_TAG_BYTES];
	size_t offset = 0;

	do {
		size_t chunk = len - offset;
		size_t sealed_len = 0;
		uint8_t header[2];
		uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];

		if (chunk > AIRPLAY_CONTROL_MAX_PAYLOAD)
			chunk = AIRPLAY_CONTROL_MAX_PAYLOAD;
		header[0] = (uint8_t)chunk;
		header[1] = (uint8_t)(chunk >> 8);
		nonce64(*counter, nonce);
		if (airplay_chacha20_poly1305_seal(
			    key, nonce, plaintext + offset, chunk,
			    header, sizeof(header), sealed, sizeof(sealed),
			    &sealed_len) < 0)
			return -1;
		if (send_all_since(fd, header, sizeof(header), started) < 0 ||
		    send_all_since(fd, sealed, sealed_len, started) < 0)
			return -1;
		(*counter)++;
		offset += chunk;
	} while (offset < len);
	return 0;
}

static int send_control_frames(struct airplay_client *client,
			       const void *data, size_t len, uint64_t started)
{
	if (!client->control_channel_ready)
		return send_all_since(client->fd, data, len, started);
	return send_encrypted_frames(client->fd, client->control_write_key,
				     &client->control_write_counter,
				     data, len, started);
}

static int send_event_frames(struct airplay_client *client, const void *data,
			     size_t len, uint64_t started)
{
	if (client->profile == AIRPLAY_PROFILE_LEGACY)
		return send_all_since(client->event_fd, data, len, started);
	return send_encrypted_frames(client->event_fd, client->event_write_key,
				     &client->event_write_counter, data, len, started);
}

static int send_request(struct airplay_client *client,
			const char *method, const char *path,
			const char *content_type, unsigned cseq,
			const uint8_t *body, size_t body_len)
{
	char head[512];
	const char *type_line = content_type && *content_type ?
		"Content-Type: " : "";
	const char *type_value = content_type ? content_type : "";
	const char *failed_part = NULL;

	if (!client || client->failed)
		return -1;
	uint64_t started = video_monotonic_ms();
	int head_len = snprintf(head, sizeof(head),
		"%s %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"User-Agent: AirPlay/" CARPLAY_SOURCE_VERSION "\r\n"
		"X-Apple-Device-ID: " CARPLAY_DEVICE_ID "\r\n"
		"%s%s%s"
		"Content-Length: %zu\r\n"
		"\r\n",
		method, path, cseq,
		type_line, type_value, type_value[0] ? "\r\n" : "",
		body_len);

	if (head_len < 0 || (size_t)head_len >= sizeof(head)) {
		iap2_log("airplay: request header overflow");
		return -1;
	}
	if (send_control_frames(client, head, (size_t)head_len, started) < 0)
		failed_part = "header";
	else if (body_len && send_control_frames(client, body, body_len, started) < 0)
		failed_part = "body";
	if (failed_part) {
		int saved = errno;
		iap2_log("airplay: send %s cseq=%u %s failed errno=%d",
			 method, cseq, failed_part, saved);
		errno = saved;
		return -1;
	}
	iap2_log("airplay: sent %s %s cseq=%u body=%zu encrypted=%d",
		 method, path, cseq, body_len, client->control_channel_ready);
	if (cseq <= 10) {
		client->pending_handshake_cseq = cseq;
		client->handshake_deadline_ms = video_monotonic_ms() + 15000u;
	}
	return 0;
}

static const uint8_t *find_header_end(const uint8_t *data, size_t len,
				      size_t *offset)
{
	size_t i;

	for (i = 0; i + 3 < len; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n') {
			*offset = i;
			return data + i + 4;
		}
	}
	return NULL;
}

static int header_uint(const uint8_t *head, size_t len, const char *name,
		       unsigned *value)
{
	size_t name_len = strlen(name);
	size_t pos = 0;

	while (pos < len) {
		size_t line_end = pos;
		size_t colon;
		size_t vpos;

		while (line_end + 1 < len &&
		       !(head[line_end] == '\r' && head[line_end + 1] == '\n'))
			line_end++;
		if (line_end + 1 >= len || line_end == pos)
			return 0;
		if (line_end > pos + name_len && head[pos + name_len] == ':') {
			int match = 1;
			size_t j;
			for (j = 0; j < name_len; j++) {
				if (strncasecmp((const char *)head + pos + j,
						name + j, 1) != 0) {
					match = 0;
					break;
				}
			}
			if (match) {
				colon = pos + name_len;
				vpos = colon + 1;
				while (vpos < line_end &&
				       (head[vpos] == ' ' || head[vpos] == '\t'))
					vpos++;
				if (vpos == line_end || head[vpos] < '0' ||
				    head[vpos] > '9')
					return -1;
				*value = 0;
				while (vpos < line_end && head[vpos] >= '0' &&
				       head[vpos] <= '9') {
					unsigned digit = (unsigned)(head[vpos] - '0');
					if (*value > (UINT_MAX - digit) / 10u)
						return -1;
					*value = *value * 10u + digit;
					vpos++;
				}
				while (vpos < line_end &&
				       (head[vpos] == ' ' || head[vpos] == '\t'))
					vpos++;
				if (vpos != line_end)
					return -1;
				return 1;
			}
		}
		pos = line_end + 2;
	}
	return 0;
}

static int parse_state_two(struct airplay_client *client,
			   const uint8_t *body, size_t len)
{
	uint8_t salt[64];
	uint8_t server_public[512];
	size_t salt_len = 0, public_len = 0;
	unsigned state = 0;
	size_t off = 0;

	while (off + 2 <= len) {
		unsigned type = body[off++];
		unsigned item_len = body[off++];
		if (off + item_len > len) {
			iap2_log("airplay: truncated pair-setup TLV type=0x%02x len=%u",
				 type, item_len);
			return -1;
		}
		iap2_log("airplay: pair-setup state-2 TLV type=0x%02x len=%u",
			 type, item_len);
		if (type == 0x06 && item_len)
			state = body[off];
		if (type == 0x02 && salt_len + item_len <= sizeof(salt)) {
			memcpy(salt + salt_len, body + off, item_len);
			salt_len += item_len;
		}
		if (type == 0x03 && public_len + item_len <= sizeof(server_public)) {
			memcpy(server_public + public_len, body + off, item_len);
			public_len += item_len;
		}
		off += item_len;
	}
	if (state != 2 || salt_len != 16 || public_len == 0 ||
	    public_len > AIRPLAY_SRP_PUBLIC_BYTES) {
		iap2_log("airplay: invalid pair-setup state-2 state=%u salt=%zu public=%zu",
			 state, salt_len, public_len);
		return -1;
	}
	if (airplay_crypto_init() < 0)
		return -1;
	{
		struct airplay_srp_client_result srp;
		uint8_t request[3u + 2u + 255u + 2u + 129u + 2u + 64u];
		size_t pos = 0;

		if (airplay_srp_client(salt, salt_len, server_public,
				       public_len, &srp) < 0)
			return -1;
		memcpy(client->server_proof, srp.server_proof,
		       sizeof(client->server_proof));
		request[pos++] = 0x06;
		request[pos++] = 0x01;
		request[pos++] = 0x03;
		request[pos++] = 0x03;
		request[pos++] = 0xff;
		memcpy(request + pos, srp.public_key, 255);
		pos += 255;
		request[pos++] = 0x03;
		request[pos++] = 0x81;
		memcpy(request + pos, srp.public_key + 255, 129);
		pos += 129;
		request[pos++] = 0x04;
		request[pos++] = 0x40;
		memcpy(request + pos, srp.client_proof, sizeof(srp.client_proof));
		pos += sizeof(srp.client_proof);
		memcpy(client->pair_setup_session_key, srp.session_key,
		       sizeof(client->pair_setup_session_key));
		iap2_log("airplay: SRP client proof ready A=%zu M1=%zu",
			 sizeof(srp.public_key), sizeof(srp.client_proof));
		return send_request(client, "POST", "/pair-setup",
				    "application/pairing+tlv8", 3,
				    request, pos);
	}
}

static void nonce_label(const char *label, uint8_t nonce[12])
{
	size_t label_len = strlen(label);

	memset(nonce, 0, 12);
	if (label_len > 8)
		label_len = 8;
	memcpy(nonce + 4, label, label_len);
}

static int derive_key(const uint8_t input[AIRPLAY_SHA512_BYTES],
		      const char *salt, const char *info,
		      uint8_t output[AIRPLAY_CHACHA_KEY_BYTES])
{
	return airplay_hkdf_sha512(
		input, AIRPLAY_SHA512_BYTES,
		(const uint8_t *)salt, strlen(salt),
		(const uint8_t *)info, strlen(info),
		output, AIRPLAY_CHACHA_KEY_BYTES);
}

static int send_pair_verify_m1(struct airplay_client *client);

static int send_pair_setup_m5(struct airplay_client *client)
{
	uint8_t controller_sign_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t signed_data[32 + sizeof(controller_identifier) - 1 + 32];
	uint8_t inner[256];
	uint8_t sealed[sizeof(inner) + AIRPLAY_CHACHA_TAG_BYTES];
	uint8_t request[sizeof(sealed) + 32];
	uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];
	size_t inner_len = 0;
	size_t sealed_len = 0;
	size_t request_len = 0;
	size_t signed_len = 0;

	if (!client->controller_key_ready) {
		if (airplay_ed25519_generate(&client->controller_key) < 0)
			return -1;
		client->controller_key_ready = 1;
	}
	if (airplay_hkdf_sha512(
		    client->pair_setup_session_key, AIRPLAY_SHA512_BYTES,
		    (const uint8_t *)"Pair-Setup-Encrypt-Salt",
		    strlen("Pair-Setup-Encrypt-Salt"),
		    (const uint8_t *)"Pair-Setup-Encrypt-Info",
		    strlen("Pair-Setup-Encrypt-Info"),
		    client->pair_setup_encryption_key,
		    sizeof(client->pair_setup_encryption_key)) < 0)
		return -1;
	if (derive_key(client->pair_setup_session_key,
		       "Pair-Setup-Controller-Sign-Salt",
		       "Pair-Setup-Controller-Sign-Info",
		       controller_sign_key) < 0)
		return -1;
	memcpy(signed_data, controller_sign_key, 32);
	signed_len = 32;
	memcpy(signed_data + signed_len, controller_identifier,
	       sizeof(controller_identifier) - 1);
	signed_len += sizeof(controller_identifier) - 1;
	memcpy(signed_data + signed_len, client->controller_key.public_key, 32);
	signed_len += 32;

	if (airplay_tlv_append(inner, sizeof(inner), &inner_len, 0x01,
			       controller_identifier,
			       sizeof(controller_identifier) - 1) < 0 ||
	    airplay_tlv_append(inner, sizeof(inner), &inner_len, 0x03,
			       client->controller_key.public_key, 32) < 0)
		return -1;
	{
		uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
		if (airplay_ed25519_sign(client->controller_key.private_key,
					signed_data, signed_len, signature) < 0 ||
		    airplay_tlv_append(inner, sizeof(inner), &inner_len, 0x0a,
				       signature, sizeof(signature)) < 0)
			return -1;
	}
	nonce_label("PS-Msg05", nonce);
	if (airplay_chacha20_poly1305_seal(
		    client->pair_setup_encryption_key, nonce,
		    inner, inner_len, NULL, 0,
		    sealed, sizeof(sealed), &sealed_len) < 0)
		return -1;
	{
		static const uint8_t state_five = 5;
		if (airplay_tlv_append(request, sizeof(request), &request_len, 0x06,
				       &state_five, 1) < 0 ||
		    airplay_tlv_append(request, sizeof(request), &request_len, 0x05,
				       sealed, sealed_len) < 0)
			return -1;
	}
	if (send_request(client, "POST", "/pair-setup",
			 "application/pairing+tlv8", 4, request, request_len) < 0)
		return -1;
	client->pair_setup_m5_sent = 1;
	iap2_log("airplay: sent pair-setup M5 encrypted identity and signature");
	return 0;
}

static int complete_pair_setup_state_six(struct airplay_client *client,
					 const uint8_t *body, size_t len)
{
	uint8_t state_buf[1];
	uint8_t encrypted[256];
	uint8_t inner[256];
	uint8_t accessory_sign_key[AIRPLAY_CHACHA_KEY_BYTES];
	uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
	uint8_t signed_data[32 + 64 + AIRPLAY_ED25519_PUBLIC_BYTES];
	uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];
	size_t state_len = 0;
	size_t encrypted_len = 0;
	size_t inner_len = 0;
	size_t identifier_len = 0;
	size_t public_key_len = 0;
	size_t signature_len = 0;
	size_t signed_len = 0;

	if (airplay_tlv_get(body, len, 0x06, state_buf, sizeof(state_buf),
			    &state_len) < 0 || state_len != 1 || state_buf[0] != 6 ||
	    airplay_tlv_get(body, len, 0x05, encrypted, sizeof(encrypted),
			    &encrypted_len) < 0)
		return -1;
	nonce_label("PS-Msg06", nonce);
	if (airplay_chacha20_poly1305_open(
		    client->pair_setup_encryption_key, nonce,
		    encrypted, encrypted_len, NULL, 0,
		    inner, sizeof(inner), &inner_len) < 0)
		return -1;
	if (airplay_tlv_get(inner, inner_len, 0x01,
			    client->accessory_identifier,
			    sizeof(client->accessory_identifier),
			    &identifier_len) < 0 ||
	    airplay_tlv_get(inner, inner_len, 0x03,
			    client->accessory_public_key,
			    sizeof(client->accessory_public_key),
			    &public_key_len) < 0 ||
	    airplay_tlv_get(inner, inner_len, 0x0a,
			    signature, sizeof(signature), &signature_len) < 0 ||
	    identifier_len == 0 || public_key_len != AIRPLAY_ED25519_PUBLIC_BYTES ||
	    signature_len != AIRPLAY_ED25519_SIGNATURE_BYTES)
		return -1;
	client->accessory_identifier_len = identifier_len;
	if (derive_key(client->pair_setup_session_key,
		       "Pair-Setup-Accessory-Sign-Salt",
		       "Pair-Setup-Accessory-Sign-Info",
		       accessory_sign_key) < 0)
		return -1;
	memcpy(signed_data, accessory_sign_key, 32);
	signed_len = 32;
	memcpy(signed_data + signed_len, client->accessory_identifier,
	       client->accessory_identifier_len);
	signed_len += client->accessory_identifier_len;
	memcpy(signed_data + signed_len, client->accessory_public_key,
	       AIRPLAY_ED25519_PUBLIC_BYTES);
	signed_len += AIRPLAY_ED25519_PUBLIC_BYTES;
	if (airplay_ed25519_verify(client->accessory_public_key,
				  signed_data, signed_len, signature) < 0) {
		iap2_log("airplay: accessory pair-setup signature mismatch");
		return -1;
	}
	client->pair_setup_complete = 1;
	iap2_log("airplay: pair-setup complete accessory_id=%.*s",
		 (int)client->accessory_identifier_len,
		 (const char *)client->accessory_identifier);
	return send_pair_verify_m1(client);
}

static int send_pair_verify_m1(struct airplay_client *client)
{
	uint8_t request[3 + 2 + AIRPLAY_X25519_PUBLIC_BYTES];
	size_t request_len = 0;
	static const uint8_t state_one = 1;

	if (airplay_x25519_generate(&client->controller_ephemeral) < 0)
		return -1;
	if (airplay_tlv_append(request, sizeof(request), &request_len, 0x06,
			       &state_one, 1) < 0 ||
	    airplay_tlv_append(request, sizeof(request), &request_len, 0x03,
			       client->controller_ephemeral.public_key,
			       AIRPLAY_X25519_PUBLIC_BYTES) < 0)
		return -1;
	if (send_request(client, "POST", "/pair-verify",
			 "application/pairing+tlv8", 5, request, request_len) < 0)
		return -1;
	client->pair_verify_requested = 1;
	iap2_log("airplay: sent pair-verify M1 cseq=5");
	return 0;
}

static int complete_pair_verify_state_two(struct airplay_client *client,
					  const uint8_t *body, size_t len)
{
	uint8_t state_buf[1];
	uint8_t accessory_public_key[AIRPLAY_X25519_PUBLIC_BYTES];
	uint8_t encrypted[256];
	uint8_t inner[256];
	uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
	uint8_t signed_data[AIRPLAY_X25519_PUBLIC_BYTES + 64 +
			   AIRPLAY_X25519_PUBLIC_BYTES];
	uint8_t response_inner[256];
	uint8_t response_sealed[sizeof(response_inner) + AIRPLAY_CHACHA_TAG_BYTES];
	uint8_t request[sizeof(response_sealed) + 32];
	uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];
	uint8_t accessory_id[64];
	size_t state_len = 0;
	size_t accessory_public_key_len = 0;
	size_t encrypted_len = 0;
	size_t inner_len = 0;
	size_t signature_len = 0;
	size_t accessory_id_len = 0;
	size_t signed_len = 0;
	size_t response_inner_len = 0;
	size_t response_sealed_len = 0;
	size_t request_len = 0;
	static const uint8_t state_three = 3;

	if (airplay_tlv_get(body, len, 0x06, state_buf, sizeof(state_buf),
			    &state_len) < 0 || state_len != 1 || state_buf[0] != 2 ||
	    airplay_tlv_get(body, len, 0x03, accessory_public_key,
			    sizeof(accessory_public_key),
			    &accessory_public_key_len) < 0 ||
	    accessory_public_key_len != AIRPLAY_X25519_PUBLIC_BYTES ||
	    airplay_tlv_get(body, len, 0x05, encrypted, sizeof(encrypted),
			    &encrypted_len) < 0)
		return -1;
	memcpy(client->accessory_ephemeral_public_key, accessory_public_key,
	       sizeof(accessory_public_key));
	if (airplay_x25519_shared(client->controller_ephemeral.private_key,
				 accessory_public_key,
				 client->pair_verify_shared_secret) < 0)
		return -1;
	if (airplay_hkdf_sha512(
		    client->pair_verify_shared_secret,
		    sizeof(client->pair_verify_shared_secret),
		    (const uint8_t *)"Pair-Verify-Encrypt-Salt",
		    strlen("Pair-Verify-Encrypt-Salt"),
		    (const uint8_t *)"Pair-Verify-Encrypt-Info",
		    strlen("Pair-Verify-Encrypt-Info"),
		    client->pair_verify_encryption_key,
		    sizeof(client->pair_verify_encryption_key)) < 0)
		return -1;
	nonce_label("PV-Msg02", nonce);
	if (airplay_chacha20_poly1305_open(
		    client->pair_verify_encryption_key, nonce,
		    encrypted, encrypted_len, NULL, 0,
		    inner, sizeof(inner), &inner_len) < 0 ||
	    airplay_tlv_get(inner, inner_len, 0x01, accessory_id,
			    sizeof(accessory_id), &accessory_id_len) < 0 ||
	    airplay_tlv_get(inner, inner_len, 0x0a, signature,
			    sizeof(signature), &signature_len) < 0 ||
	    accessory_id_len == 0 ||
	    signature_len != AIRPLAY_ED25519_SIGNATURE_BYTES)
		return -1;
	memcpy(signed_data, accessory_public_key,
	       AIRPLAY_X25519_PUBLIC_BYTES);
	signed_len = AIRPLAY_X25519_PUBLIC_BYTES;
	memcpy(signed_data + signed_len, accessory_id, accessory_id_len);
	signed_len += accessory_id_len;
	memcpy(signed_data + signed_len,
	       client->controller_ephemeral.public_key,
	       AIRPLAY_X25519_PUBLIC_BYTES);
	signed_len += AIRPLAY_X25519_PUBLIC_BYTES;
	if (airplay_ed25519_verify(client->accessory_public_key,
				  signed_data, signed_len, signature) < 0) {
		iap2_log("airplay: pair-verify accessory signature mismatch");
		return -1;
	}
	client->pair_verify_m2_verified = 1;

	memcpy(signed_data, client->controller_ephemeral.public_key,
	       AIRPLAY_X25519_PUBLIC_BYTES);
	signed_len = AIRPLAY_X25519_PUBLIC_BYTES;
	memcpy(signed_data + signed_len, controller_identifier,
	       sizeof(controller_identifier) - 1);
	signed_len += sizeof(controller_identifier) - 1;
	memcpy(signed_data + signed_len, accessory_public_key,
	       AIRPLAY_X25519_PUBLIC_BYTES);
	signed_len += AIRPLAY_X25519_PUBLIC_BYTES;
	if (airplay_ed25519_sign(client->controller_key.private_key,
				 signed_data, signed_len, signature) < 0 ||
	    airplay_tlv_append(response_inner, sizeof(response_inner),
			       &response_inner_len, 0x01,
			       controller_identifier,
			       sizeof(controller_identifier) - 1) < 0 ||
	    airplay_tlv_append(response_inner, sizeof(response_inner),
			       &response_inner_len, 0x0a, signature,
			       sizeof(signature)) < 0)
		return -1;
	nonce_label("PV-Msg03", nonce);
	if (airplay_chacha20_poly1305_seal(
		    client->pair_verify_encryption_key, nonce,
		    response_inner, response_inner_len, NULL, 0,
		    response_sealed, sizeof(response_sealed), &response_sealed_len) < 0 ||
	    airplay_tlv_append(request, sizeof(request), &request_len, 0x06,
			       &state_three, 1) < 0 ||
	    airplay_tlv_append(request, sizeof(request), &request_len, 0x05,
			       response_sealed, response_sealed_len) < 0)
		return -1;
	if (send_request(client, "POST", "/pair-verify",
			 "application/pairing+tlv8", 6, request, request_len) < 0)
		return -1;
	client->pair_verify_m3_sent = 1;
	iap2_log("airplay: pair-verify M2 verified and M3 sent cseq=6");
	return 0;
}

static uint32_t read_u32_be(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

static int send_auth_setup(struct airplay_client *client)
{
	uint8_t body[1 + AIRPLAY_X25519_PUBLIC_BYTES];

	if (airplay_x25519_generate(&client->sap_ephemeral) < 0)
		return -1;
	body[0] = 1;
	memcpy(body + 1, client->sap_ephemeral.public_key,
	       AIRPLAY_X25519_PUBLIC_BYTES);
	if (send_request(client, "POST", "/auth-setup",
			 "application/octet-stream", 7, body, sizeof(body)) < 0)
		return -1;
	client->auth_setup_sent = 1;
	iap2_log("airplay: sent SAP auth-setup public key cseq=7");
	return 0;
}

static void prepare_audio_bridge(struct airplay_client *client)
{
    struct carplay_audio_bridge *bridge = NULL;

    if (!client)
        return;
    carplay_audio_bridge_destroy(&client->audio_bridge);
    if (!client->enable_media_audio && !client->enable_microphone)
        return;
    iap2_log("airplay: audio capabilities entries=%zu advertised_output=%d advertised_input=%d enabled_media=%d enabled_microphone=%d",
             client->capabilities.audio.format_count, client->capabilities.audio.media_output,
             client->capabilities.audio.microphone_input, client->enable_media_audio, client->enable_microphone);
    for (size_t i = 0; i < client->capabilities.audio.format_count; i++) {
        const struct carplay_audio_format_capabilities *format = &client->capabilities.audio.formats[i];
        iap2_log("airplay: audio capability[%zu] present=%d type=%d audioType=%s input=0x%08x output=0x%08x",
                 i, format->present, format->type, format->audio_type,
                 format->input_formats, format->output_formats);
    }
    int result = client->profile == AIRPLAY_PROFILE_LEGACY ?
        carplay_audio_bridge_init_legacy(&bridge, client->enable_media_audio, client->enable_microphone) :
        carplay_audio_bridge_init(&bridge, client->pair_verify_shared_secret,
                                  client->enable_media_audio, client->enable_microphone);
    if (result < 0) {
        iap2_log("airplay: audio bridge initialization failed");
        return;
    }
    if (carplay_audio_bridge_prepare(bridge, &client->capabilities) < 0) {
        iap2_log("airplay: audio stream preparation failed: %s",
                 carplay_audio_bridge_last_error(bridge));
        carplay_audio_bridge_destroy(&bridge);
        return;
    }
    client->audio_bridge = bridge;
    struct carplay_audio_status audio_status;
    carplay_audio_bridge_get_status(bridge, &audio_status);
	carplay_modes_init(&client->modes, audio_status.media_available || audio_status.microphone_available);
    iap2_log("airplay: prepared %zu LPCM audio stream(s) media_available=%d microphone_available=%d",
             carplay_audio_bridge_setup_count(bridge), audio_status.media_available, audio_status.microphone_available);
    if (!audio_status.media_available && !audio_status.microphone_available)
        iap2_log("airplay: optional audio unavailable; no usable enabled PCM formats in the parsed capability entries");
}

static int send_setup(struct airplay_client *client);
static void fail_session(struct airplay_client *client, const char *reason);
static int send_modes_changed(struct airplay_client *client);
static int legacy_audio_drive(struct airplay_client *client);
static int legacy_audio_response(struct airplay_client *client, unsigned status,
                                 unsigned cseq, const uint8_t *body, size_t len);
static int start_keepalive_worker(struct airplay_client *client);
static void stop_keepalive_worker(struct airplay_client *client);
static int send_deferred_audio_setup(struct airplay_client *client);

static int complete_auth_setup(struct airplay_client *client,
			       const uint8_t *body, size_t len)
{
	uint32_t certificate_len;
	uint32_t signature_len;
	size_t offset;

	if (len < 32 + 4)
		return -1;
	memcpy(client->sap_shared_secret, body, 32);
	if (airplay_x25519_shared(client->sap_ephemeral.private_key, body,
				 client->sap_shared_secret) < 0) {
		iap2_log("airplay: SAP X25519 shared secret failed");
		return -1;
	}
	certificate_len = read_u32_be(body + 32);
	offset = 36;
	if (certificate_len > len - offset)
		return -1;
	offset += certificate_len;
	if (len - offset < 4)
		return -1;
	signature_len = read_u32_be(body + offset);
	offset += 4;
	if (signature_len > len - offset)
		return -1;
	client->sap_certificate_len = (int)certificate_len;
	client->sap_signature_len = (int)signature_len;
	client->auth_setup_complete = 1;
	iap2_log("airplay: SAP auth-setup response parsed certificate=%u signature=%u",
		 certificate_len, signature_len);
	prepare_audio_bridge(client);
	return send_setup(client);
}

static int send_setup(struct airplay_client *client)
{
	uint8_t body[1024];
	size_t body_len = 0;
	uint16_t local_timing_port;

	if (!client->timing && airplay_timing_start(&client->timing, client->fd) < 0) {
		fail_session(client, "could not start AirPlay timing responder");
		return -1;
	}
	local_timing_port = airplay_timing_port(client->timing);
	if (!local_timing_port || airplay_timing_error(client->timing)) {
		fail_session(client, "AirPlay timing responder unavailable");
		return -1;
	}

	/* Paired session: pair-verify already produced the ChaCha20 keys, so the
	 * plist must not carry `et` (see airplay_bplist_encode_setup()). */
	if (airplay_bplist_encode_setup(body, sizeof(body), &body_len,
					local_timing_port, 1,
					client->profile == AIRPLAY_PROFILE_HAP) < 0) {
		iap2_log("airplay: failed to encode SETUP plist");
		return -1;
	}
	if (send_request(client, "SETUP",
			 client->profile == AIRPLAY_PROFILE_LEGACY ? "/setup" : "/stream",
			 "application/x-apple-binary-plist", 8,
			 body, body_len) < 0)
		return -1;
	client->setup_sent = 1;
	iap2_log("airplay: sent session SETUP cseq=8 profile=%s body=%zu",
		 client->profile == AIRPLAY_PROFILE_LEGACY ? "legacy" : "HAP", body_len);
	return 0;
}

static int bplist_port(const struct airplay_bplist_document *document,
		       const struct airplay_bplist_value *dictionary,
		       const char *key, uint16_t *port, int required)
{
	struct airplay_bplist_value value;
	uint64_t number = 0;
	int found = airplay_bplist_dict_get(document, dictionary, key, &value);

	if (found == 0 && !required)
		return 0;
	if (found != 1 || airplay_bplist_get_uint64(document, &value, &number) < 0 ||
	    number == 0 || number > UINT16_MAX) {
		iap2_log("airplay: SETUP response has invalid %s", key);
		return -1;
	}
	*port = (uint16_t)number;
	return 1;
}

static int complete_stream_setup(struct airplay_client *client,
				 const uint8_t *body, size_t len, int audio_only)
{
	struct airplay_bplist_document document;
	struct airplay_bplist_value root;
	struct airplay_bplist_value streams;
	uint64_t stream_count = 0;
	uint64_t i;
	int screen_complete = 0;

	client->audio_port_count = 0;
	if (airplay_bplist_init(&document, body, len) < 0 ||
	    airplay_bplist_root(&document, &root) < 0 ||
	    airplay_bplist_dict_get(&document, &root, "streams",
				    &streams) != 1 ||
	    airplay_bplist_array_count(&document, &streams,
				       &stream_count) < 0 ||
	    stream_count == 0) {
		iap2_log("airplay: invalid stream SETUP response");
		return -1;
	}
	for (i = 0; i < stream_count; i++) {
		struct airplay_bplist_value stream;
		struct airplay_bplist_value type;
		struct airplay_bplist_value data_port;
		struct airplay_bplist_value control_port;
		struct airplay_bplist_value connection_id;
		struct airplay_bplist_value audio_type;
		uint64_t type_number = 0;
		uint64_t data_number = 0;
		uint64_t control_number = 0;
		uint64_t connection_number = 0;
		size_t audio_type_len = 0;

		if (airplay_bplist_array_get(&document, &streams, (size_t)i,
					     &stream) < 0 ||
		    airplay_bplist_dict_get(&document, &stream, "type",
					    &type) != 1 ||
		    airplay_bplist_get_uint64(&document, &type,
					      &type_number) < 0)
			continue;
		if (airplay_bplist_dict_get(&document, &stream, "dataPort",
					    &data_port) == 1 &&
		    airplay_bplist_get_uint64(&document, &data_port,
					      &data_number) == 0 &&
		    data_number > UINT16_MAX)
			data_number = 0;
		if (airplay_bplist_dict_get(&document, &stream, "controlPort",
					    &control_port) == 1 &&
		    airplay_bplist_get_uint64(&document, &control_port,
					      &control_number) == 0 &&
		    control_number > UINT16_MAX)
			control_number = 0;
		if (airplay_bplist_dict_get(&document, &stream,
					    "streamConnectionID", &connection_id) == 1)
			(void)airplay_bplist_get_uint64(
				&document, &connection_id, &connection_number);
		if (type_number == 110) {
			char salt[96];
			int salt_len;
			if (audio_only)
				continue;
			if (!data_number) {
				iap2_log("airplay: screen SETUP omitted dataPort");
				continue;
			}
			if (client->profile == AIRPLAY_PROFILE_LEGACY) {
				uint8_t zero[16] = {0}, key[16], iv[16];
				if (airplay_legacy_keys(zero, client->stream_connection_id, key, iv) < 0)
					return -1;
				airplay_legacy_free(client->legacy_video);
				client->legacy_video = airplay_legacy_new(key, iv);
				if (!client->legacy_video)
					return -1;
			} else {
			salt_len = snprintf(
				salt, sizeof(salt), "DataStream-Salt%llu",
				(unsigned long long)client->stream_connection_id);
			if (salt_len <= 0 || (size_t)salt_len >= sizeof(salt) ||
			    airplay_hkdf_sha512(
				client->pair_verify_shared_secret,
				sizeof(client->pair_verify_shared_secret),
				(const uint8_t *)salt, (size_t)salt_len,
				(const uint8_t *)"DataStream-Output-Encryption-Key",
				strlen("DataStream-Output-Encryption-Key"),
				client->video_key, sizeof(client->video_key)) < 0)
				return -1;
			}
			client->screen_data_port = (uint16_t)data_number;
			screen_complete = 1;
			iap2_log("airplay: screen SETUP complete dataPort=%u videoKey=ready",
				 client->screen_data_port);
			continue;
		}
		if (type_number != 100 && type_number != 101 &&
		    type_number != 102)
			continue;
		if (client->profile == AIRPLAY_PROFILE_LEGACY &&
		    (type_number != 100 || connection_number != 0))
			continue;
		if (!data_number) {
			iap2_log("airplay: audio SETUP omitted dataPort type=%llu",
				 (unsigned long long)type_number);
			continue;
		}
		if (client->audio_port_count >= CARPLAY_AUDIO_MAX_STREAMS)
			continue;
		if (airplay_bplist_dict_get(&document, &stream, "audioType",
					    &audio_type) == 1) {
			if (airplay_bplist_string_copy(
				    &document, &audio_type,
				    client->audio_ports[client->audio_port_count].audio_type,
				    CARPLAY_UUID_CAPACITY, &audio_type_len) < 0 ||
			    audio_type_len == 0)
				continue;
		} else if (!client->audio_bridge ||
			   carplay_audio_bridge_resolve_response(
				   client->audio_bridge, (int)type_number,
				   connection_number,
				   client->audio_ports[client->audio_port_count].audio_type,
				   CARPLAY_UUID_CAPACITY, &audio_type_len) < 0) {
			iap2_log("airplay: audio SETUP response could not resolve type=%llu connection=%llu",
				 (unsigned long long)type_number,
				 (unsigned long long)connection_number);
			continue;
		}
		client->audio_ports[client->audio_port_count].type =
			(int)type_number;
		if (client->profile == AIRPLAY_PROFILE_LEGACY && client->audio_bridge) {
			char expected[CARPLAY_UUID_CAPACITY]; size_t expected_len;
			if (client->audio_port_count != 0 ||
			    carplay_audio_bridge_resolve_response(client->audio_bridge, 100, 0,
					expected, sizeof(expected), &expected_len) < 0 ||
			    strcmp(expected, client->audio_ports[client->audio_port_count].audio_type))
				continue;
		}
		client->audio_ports[client->audio_port_count].data_port =
			(uint16_t)data_number;
		client->audio_ports[client->audio_port_count].control_port =
			(uint16_t)control_number;
		iap2_log("airplay: audio SETUP port type=%llu audioType=%s dataPort=%llu controlPort=%llu",
			 (unsigned long long)type_number,
			 client->audio_ports[client->audio_port_count].audio_type,
			 (unsigned long long)data_number,
			 (unsigned long long)control_number);
		client->audio_port_count++;
	}
	if (audio_only)
		return client->audio_port_count ? 0 : -1;
	if (!screen_complete) {
		iap2_log("airplay: stream SETUP response did not contain the screen stream");
		return -1;
	}
	client->screen_setup_complete = 1;
	iap2_log("airplay: stream SETUP complete streams=%llu audioStreams=%zu",
		 (unsigned long long)stream_count, client->audio_port_count);
	return 0;
}

static int complete_screen_setup(struct airplay_client *client,
				 const uint8_t *body, size_t len)
{
	return complete_stream_setup(client, body, len, 0);
}

static int complete_setup(struct airplay_client *client,
			  const uint8_t *body, size_t len)
{
	struct airplay_bplist_document document;
	struct airplay_bplist_value root;
	struct airplay_bplist_value features;
	struct airplay_bplist_value feature;
	uint64_t feature_count = 0;
	uint64_t i;
	int found_iap_channel = 0;
	int found_view_areas = 0;
	int result;

	if (airplay_bplist_init(&document, body, len) < 0 ||
	    airplay_bplist_root(&document, &root) < 0 ||
	    root.type != AIRPLAY_BPLIST_DICTIONARY) {
		iap2_log("airplay: invalid SETUP binary plist response");
		return -1;
	}
	if (client->profile == AIRPLAY_PROFILE_LEGACY) {
		struct airplay_bplist_value value;
		uint64_t port = 0;
		if (bplist_port(&document, &root, "eventPort", &client->event_port, 1) != 1)
			return -1;
		/* Legacy peers legitimately advertise timingPort=0 (no probes). */
		result = airplay_bplist_dict_get(&document, &root, "timingPort", &value);
		if (result < 0 || (result == 1 &&
		    (airplay_bplist_get_uint64(&document, &value, &port) < 0 || port > UINT16_MAX)))
			return -1;
		client->timing_port = (uint16_t)port;
		port = 0;
		result = airplay_bplist_dict_get(&document, &root, "keepAlivePort", &value);
		if (result < 0 || (result == 1 &&
		    (airplay_bplist_get_uint64(&document, &value, &port) < 0 || port > UINT16_MAX)))
			return -1;
		client->keep_alive_port = (uint16_t)port;
		client->keep_alive_port_present = port != 0;
		if (start_keepalive_worker(client) < 0)
			return -1;
		client->setup_complete = 1;
		client->info_requested = 1;
		iap2_log("airplay: legacy SETUP complete timingPort=%u eventPort=%u keepAlivePort=%u",
			 client->timing_port, client->event_port, client->keep_alive_port);
		return send_request(client, "GET", "/info", NULL, 1, NULL, 0);
	}
	if (bplist_port(&document, &root, "timingPort",
			&client->timing_port, 1) != 1 ||
	    bplist_port(&document, &root, "eventPort",
			&client->event_port, 1) != 1)
		return -1;
	result = bplist_port(&document, &root, "keepAlivePort",
			     &client->keep_alive_port, 0);
	if (result < 0)
		return -1;
	client->keep_alive_port_present = result == 1;
	if (airplay_bplist_dict_get(&document, &root, "enabledFeatures",
				    &features) != 1 ||
	    airplay_bplist_array_count(&document, &features,
				       &feature_count) < 0) {
		iap2_log("airplay: SETUP response has no enabledFeatures array");
		return -1;
	}
	for (i = 0; i < feature_count; i++) {
		if (airplay_bplist_array_get(&document, &features,
					     (size_t)i, &feature) < 0)
			return -1;
		if (airplay_bplist_string_equals(&document, &feature,
						 "iAPChannel"))
			found_iap_channel = 1;
		if (airplay_bplist_string_equals(&document, &feature,
						 "viewAreas"))
			found_view_areas = 1;
	}
	if (!found_iap_channel) {
		iap2_log("airplay: receiver did not negotiate iAPChannel");
		return -1;
	}
	client->setup_complete = 1;
	iap2_log("airplay: SETUP complete timingPort=%u eventPort=%u keepAlivePort=%u enabledFeatures=%llu iapChannel=1 viewAreas=%d",
		 client->timing_port, client->event_port,
		 client->keep_alive_port_present ? client->keep_alive_port : 0,
		 (unsigned long long)feature_count, found_view_areas);
	return 0;
}
static int complete_pair_verify_state_four(struct airplay_client *client,
					   const uint8_t *body, size_t len)
{
	uint8_t state_buf[1];
	size_t state_len = 0;

	if (airplay_tlv_get(body, len, 0x06, state_buf, sizeof(state_buf),
			    &state_len) < 0 || state_len != 1 || state_buf[0] != 4 ||
	    airplay_hkdf_sha512(
		    client->pair_verify_shared_secret,
		    sizeof(client->pair_verify_shared_secret),
		    (const uint8_t *)"Control-Salt", strlen("Control-Salt"),
		    (const uint8_t *)"Control-Read-Encryption-Key",
		    strlen("Control-Read-Encryption-Key"),
		    client->control_read_key, sizeof(client->control_read_key)) < 0 ||
	    airplay_hkdf_sha512(
		    client->pair_verify_shared_secret,
		    sizeof(client->pair_verify_shared_secret),
		    (const uint8_t *)"Control-Salt", strlen("Control-Salt"),
		    (const uint8_t *)"Control-Write-Encryption-Key",
		    strlen("Control-Write-Encryption-Key"),
		    client->control_write_key, sizeof(client->control_write_key)) < 0 ||
	    airplay_hkdf_sha512(
		    client->pair_verify_shared_secret,
		    sizeof(client->pair_verify_shared_secret),
		    (const uint8_t *)"Events-Salt", strlen("Events-Salt"),
		    (const uint8_t *)"Events-Write-Encryption-Key",
		    strlen("Events-Write-Encryption-Key"),
		    client->event_read_key, sizeof(client->event_read_key)) < 0 ||
	    airplay_hkdf_sha512(
		    client->pair_verify_shared_secret,
		    sizeof(client->pair_verify_shared_secret),
		    (const uint8_t *)"Events-Salt", strlen("Events-Salt"),
		    (const uint8_t *)"Events-Read-Encryption-Key",
		    strlen("Events-Read-Encryption-Key"),
		    client->event_write_key, sizeof(client->event_write_key)) < 0)
		return -1;
	client->pair_verify_complete = 1;
	client->control_channel_ready = 1;
	client->control_read_counter = 0;
	client->control_write_counter = 0;
	client->event_read_counter = 0;
	client->event_write_counter = 0;
	iap2_log("airplay: pair-verify complete; control channel encryption enabled");
	return send_auth_setup(client);
}

static int verify_state_four(struct airplay_client *client,
			     const uint8_t *body, size_t len)
{
	int proof_present = 0;
	int result = airplay_pair_setup_check_state_four(
		body, len, client->server_proof, &proof_present);

	if (result < 0) {
		iap2_log("airplay: invalid pair-setup state-4 response");
		return -1;
	}
	client->pairing_proven = 1;
	if (proof_present)
		iap2_log("airplay: pair-setup SRP proof verified");
	else
		iap2_log("airplay: pair-setup state 4 accepted without optional server proof");
	return send_pair_setup_m5(client);
}
/*
 * Name the step that a CSeq belongs to.  The handshake is a fixed sequence
 * (see process_response()), so a bare "cseq=4 failed status=470" makes the
 * reader reconstruct the ladder by hand; the step name is the diagnosis.
 */
static const char *rtsp_step_name(unsigned cseq)
{
	switch (cseq) {
	case 1: return "GET /info";
	case 2: return "pair-setup M1";
	case 3: return "pair-setup M3";
	case 4: return "pair-setup M5";
	case 5: return "pair-verify M1";
	case 6: return "pair-verify M3";
	case 7: return "auth-setup (FairPlay SAP)";
	case 8: return "SETUP (session)";
	case 9: return "SETUP (screen stream)";
	case 10: return "RECORD";
	default: return "post-handshake request";
	}
}

static int legacy_nav_ports(const uint8_t *body, size_t len, uint16_t *data, uint16_t *control)
{
    struct airplay_bplist_document doc;
    struct airplay_bplist_value root, streams, stream, value;
    uint64_t count;
    if (airplay_bplist_init(&doc, body, len) < 0 || airplay_bplist_root(&doc, &root) < 0 ||
        airplay_bplist_dict_get(&doc, &root, "streams", &streams) != 1 ||
        airplay_bplist_array_count(&doc, &streams, &count) < 0 || count != 1 ||
        airplay_bplist_array_get(&doc, &streams, 0, &stream) < 0)
        return -1;
    uint64_t number = 0;
    if (airplay_bplist_dict_get(&doc, &stream, "type", &value) != 1 ||
        airplay_bplist_get_uint64(&doc, &value, &number) < 0 || number != 101)
        return -1;
    int found = airplay_bplist_dict_get(&doc, &stream, "streamConnectionID", &value);
    if (found < 0 || (found == 1 &&
        (airplay_bplist_get_uint64(&doc, &value, &number) < 0 || number != 0)))
        return -1;
    found = airplay_bplist_dict_get(&doc, &stream, "audioType", &value);
    if (found < 0) return -1;
    if (found == 1) {
        char name[16]; size_t size;
        if (airplay_bplist_string_copy(&doc, &value, name, sizeof(name), &size) < 0 ||
            size != sizeof("default") - 1 || strcmp(name, "default"))
            return -1;
    }
    found = airplay_bplist_dict_get(&doc, &stream, "controlPort", &value);
    number = 0;
    if (found < 0 || (found == 1 &&
        (airplay_bplist_get_uint64(&doc, &value, &number) < 0 || number > UINT16_MAX)))
        return -1;
    *control = (uint16_t)number;
    return bplist_port(&doc, &stream, "dataPort", data, 1) == 1 ? 0 : -1;
}

static int legacy_nav_response(struct airplay_client *c, unsigned status,
                               unsigned cseq, const uint8_t *body, size_t len)
{
    uint16_t data = 0, control = 0;
    if (!cseq || cseq != c->legacy_nav_cseq) return 0;
    c->legacy_nav_cseq = 0;
    c->legacy_nav_deadline = 0;
    if (status != 200 || legacy_nav_ports(body, len, &data, &control) < 0 ||
        (c->audio_port_count && data == c->audio_ports[0].data_port) ||
        carplay_audio_bridge_start_stream(c->audio_bridge, 101, "default", data,
                                          control, c->audio_host) < 0)
        iap2_log("airplay: navigation 101 SETUP unavailable status=%u; retaining local mix", status);
    return 0;
}

static int process_response(struct airplay_client *client,
			    unsigned status, unsigned cseq,
			    const uint8_t *body, size_t body_len)
{
	if (client->profile == AIRPLAY_PROFILE_LEGACY && cseq && cseq == client->legacy_nav_cseq)
		return legacy_nav_response(client, status, cseq, body, body_len);
	if (client->profile == AIRPLAY_PROFILE_LEGACY && cseq &&
	    cseq == client->legacy_audio_cseq)
		return legacy_audio_response(client, status, cseq, body, body_len);
	if (cseq && cseq == client->pending_initial_modes_cseq) {
		struct airplay_bplist_document doc;
		struct airplay_bplist_value root, value;
		uint64_t result = 0;
		int found = 0;
		client->pending_initial_modes_cseq = 0;
		client->initial_modes_deadline_ms = 0;
		if (body_len) {
			if (airplay_bplist_init(&doc, body, body_len) < 0 ||
			    airplay_bplist_root(&doc, &root) < 0 ||
			    root.type != AIRPLAY_BPLIST_DICTIONARY)
				found = -1;
			else {
				found = airplay_bplist_dict_get(&doc, &root, "status", &value);
				if (found == 1 && airplay_bplist_get_uint64(&doc, &value, &result) < 0)
					found = -1;
			}
		}
		if (status != 200 || found < 0 || result != 0) {
			fail_session(client, "initial modesChanged refused or malformed");
			return -1;
		}
		client->initial_modes_accepted = 1;
		return 0;
	}
	if (cseq == 11) {
		if (!client->audio_setup_pending)
			return 0;
		client->audio_setup_pending = 0;
		client->audio_setup_deadline_ms = 0;
		if (status != 200 || complete_stream_setup(client, body, body_len, 1) < 0) {
			client->audio_setup_failed = 1;
			iap2_log("airplay: deferred audio SETUP unavailable status=%u; video continues", status);
			return 0;
		}
		if (client->audio_host[0] &&
		    airplay_client_start_audio(client, client->audio_host) < 0 && client->failed)
			return -1;
		return 0;
	}
	if (cseq == client->pending_handshake_cseq) {
		client->handshake_deadline_ms = 0;
		client->pending_handshake_cseq = 0;
	}
	if (cseq == client->pending_feedback_cseq) {
		client->pending_feedback_cseq = 0;
		client->feedback_deadline_ms = 0;
	}
	if (status != 200) {
		/*
		 * A non-200 on the *handshake* is terminal: the next request in the
		 * ladder is only ever queued from inside the successful arm of the
		 * previous one, so the session would otherwise sit in the poll loop
		 * forever with a fresh socket and no work to do.  Post-handshake
		 * requests (feedback, keepalive) are answered with a status we do
		 * not care about, so those stay non-fatal.
		 */
		if (cseq >= 1 && cseq <= 10) {
			if (client->profile == AIRPLAY_PROFILE_LEGACY && cseq == 8 &&
			    client->setup_sent && !client->setup_complete && status >= 400 && status < 500)
				client->wants_hap_fallback = 1;
			snprintf(client->failure, sizeof(client->failure),
				 "%s refused (status=%u)", rtsp_step_name(cseq),
				 status);
			client->failed = 1;
			iap2_log("airplay: session aborted at %s: status=%u cseq=%u body=%zu",
				 rtsp_step_name(cseq), status, cseq, body_len);
			return -1;
		}
		if (++client->post_handshake_refusals == 1 ||
		    client->post_handshake_refusals % 60u == 0)
			iap2_log("airplay: %s answered status=%u cseq=%u refusals=%u; continuing",
				 rtsp_step_name(cseq), status, cseq, client->post_handshake_refusals);
		return 0;
	}
	if (cseq == 1 && !client->info_complete) {
		if (carplay_capabilities_parse(body, body_len,
					       &client->capabilities) != 0) {
			iap2_log("airplay: /info capability description rejected body=%zu",
				 body_len);
			return -1;
		}
		client->info_complete = 1;
		{
			struct airplay_bplist_document doc;
			struct airplay_bplist_value root, value;
			int enabled = 0;
			if (airplay_bplist_init(&doc, body, body_len) == 0 &&
			    airplay_bplist_root(&doc, &root) == 0 &&
			    airplay_bplist_dict_get(&doc, &root, "keepAliveSendStatsAsBody", &value) == 1 &&
			    airplay_bplist_get_bool(&doc, &value, &enabled) == 0)
				client->keepalive_stats_body = enabled;
		}
		if (client->capabilities.display.present) {
			client->requested_width =
				(unsigned)client->capabilities.display.width_pixels;
			client->requested_height =
				(unsigned)client->capabilities.display.height_pixels;
		}
		if (client->capabilities.appearance_known)
			apply_car_night_mode(client,
				client->capabilities.night_mode);
		if (client->capabilities.display.present) {
			iap2_log("airplay: negotiated display=%dx%d uuid=%s fps=%d input=%d touch=%d keyboard=%d knob=%d",
				 client->capabilities.display.width_pixels,
				 client->capabilities.display.height_pixels,
				 client->capabilities.display.uuid,
				 client->capabilities.display.max_fps,
				 client->capabilities.display.primary_input_device,
				 client->capabilities.has_touch,
				 client->capabilities.has_keyboard,
				 client->capabilities.has_knob);
		} else {
			iap2_log("airplay: /info did not provide a valid display; local size will be a logged fallback");
		}
		if (apply_native_display(client) != 0) {
            snprintf(client->failure, sizeof(client->failure), "display preparation: %.170s", client->display_error);
            client->failed = 1;
            return -1;
        }
        prepare_knob_layouts(client, body, body_len);
		iap2_log("airplay: /info capabilities audio_output=%d microphone_input=%d hid_devices=%zu",
			 client->capabilities.audio.media_output,
			 client->capabilities.audio.microphone_input,
			 client->capabilities.hid_count);
		if (client->profile == AIRPLAY_PROFILE_LEGACY) {
			prepare_audio_bridge(client);
			iap2_log("airplay: legacy capabilities ready; audio negotiates after screen");
			return 0;
		}
		{
			static const uint8_t state_one[] = { 0x06, 0x01, 0x01 };
			if (send_request(client, "POST", "/pair-setup",
					 "application/pairing+tlv8", 2,
					 state_one, sizeof(state_one)) < 0)
				return -1;
			client->pair_setup_requested = 1;
		}
		return 0;
	}
	if (cseq == 2) {
		iap2_log("airplay: pair-setup state 2 received");
		return parse_state_two(client, body, body_len);
	}
	if (cseq == 3 && !client->pairing_proven)
		return verify_state_four(client, body, body_len);
	if (cseq == 4 && client->pair_setup_m5_sent &&
	    !client->pair_setup_complete)
		return complete_pair_setup_state_six(client, body, body_len);
	if (cseq == 5 && client->pair_verify_requested &&
	    !client->pair_verify_m2_verified)
		return complete_pair_verify_state_two(client, body, body_len);
	if (cseq == 6 && client->pair_verify_m3_sent &&
	    !client->pair_verify_complete)
		return complete_pair_verify_state_four(client, body, body_len);
	if (cseq == 7 && client->auth_setup_sent &&
	    !client->auth_setup_complete)
		return complete_auth_setup(client, body, body_len);
	if (cseq == 8 && client->setup_sent && !client->setup_complete)
		return complete_setup(client, body, body_len);
	if (cseq == 9 && client->screen_setup_sent &&
	    !client->screen_setup_complete)
		return complete_screen_setup(client, body, body_len);
	if (cseq == 10 && client->record_sent) {
		if (client->record_accepted)
			return 0;
		client->record_accepted = 1;
		iap2_log("airplay: RECORD accepted; streaming session is active");
		if (client->profile == AIRPLAY_PROFILE_LEGACY) {
			client->pending_initial_modes_cseq = client->next_control_cseq;
			client->initial_modes_deadline_ms = video_monotonic_ms() + 5000u;
			return send_modes_changed(client);
		}
		return 0;
	}
	/* Feedback and mode notifications use our monotonic post-handshake CSeq
	 * range. Their normal ACKs need no next handshake step or error log. */
	if (cseq >= 100 && cseq < client->next_control_cseq)
		return 0;
	iap2_log("airplay: unexpected RTSP response cseq=%u status=%u body=%zu",
		 cseq, status, body_len);
	return 0;
}

struct airplay_client *airplay_client_new_with_audio(int fd, int enable_media,
                                                    int enable_microphone)
{
	struct airplay_client *client = calloc(1, sizeof(*client));

	if (!client)
		return NULL;
	client->fd = fd;
	client->keepalive_fd = -1;
	client->keepalive_wake[0] = client->keepalive_wake[1] = -1;
	atomic_init(&client->keepalive_stop, 0);
	client->next_control_cseq = 100;
	carplay_modes_init(&client->modes, 0);
	client->enable_media_audio = !!enable_media;
	client->enable_microphone = !!enable_microphone;
	client->event_fd = -1;
	client->configured_fps = 30;
	client->bitrate = CARPLAY_VIDEO_BITRATE_MAX;
	snprintf(client->display_error, sizeof(client->display_error), "%s",
		 "waiting for negotiated display");
	client->screen_data_fd = -1;
	carplay_encoder_init(&client->encoder);
	atomic_init(&client->live_screen_stop, 0);
	atomic_init(&client->force_key_frame, 0);
	atomic_init(&client->video_failure, 0);
	atomic_init(&client->screen_suspended, 0);
	atomic_init(&client->screen_resume_generation, 0);
	atomic_init(&client->screen_needs_keyframe, 0);
	client->touch_input = airplay_uinput_open();
	pthread_mutex_init(&client->video_send_mutex, NULL);
	pthread_mutex_init(&client->event_send_mutex, NULL);
	prepare_input_environment();
	/* Temporary geometry only: /info performs the authoritative size+density
	 * read before media or HID becomes usable. Avoid a redundant binder probe. */
	client->input_width = 800;
	client->input_height = 480;
	return client;
}

struct airplay_client *airplay_client_new(int fd)
{
	return airplay_client_new_with_audio(fd, 1, 1);
}

void airplay_client_set_profile(struct airplay_client *client, enum airplay_profile profile)
{
	if (!client || client->info_requested || client->setup_sent)
		return;
	client->profile = profile == AIRPLAY_PROFILE_LEGACY ? profile : AIRPLAY_PROFILE_HAP;
}

enum airplay_profile airplay_client_profile(const struct airplay_client *client)
{
	return client ? client->profile : AIRPLAY_PROFILE_HAP;
}

int airplay_client_wants_hap_fallback(const struct airplay_client *client)
{
	return client && client->wants_hap_fallback;
}

void airplay_client_set_video_options(struct airplay_client *client,
					     int display_width, int display_height,
					     int physical_width, int physical_height,
					     int density, int configured_fps,
					     int bitrate)
{
	if (!client)
		return;
	client->display_override_width = display_width;
	client->display_override_height = display_height;
	client->physical_override_width = physical_width;
	client->physical_override_height = physical_height;
	client->display_density_override = density;
	client->configured_fps = configured_fps > 0 ? configured_fps : 30;
	client->bitrate = bitrate > 0 ?
		(bitrate > CARPLAY_VIDEO_BITRATE_MAX ? CARPLAY_VIDEO_BITRATE_MAX : bitrate) :
		CARPLAY_VIDEO_BITRATE_MAX;
}

void airplay_client_get_runtime_status(
	const struct airplay_client *client,
	struct airplay_runtime_status *status)
{
	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	if (!client)
		return;
	status->display_width = (int)client->input_width;
	status->display_height = (int)client->input_height;
	status->requested_width = (int)client->requested_width;
	status->requested_height = (int)client->requested_height;
	status->physical_width = (int)client->physical_width;
	status->physical_height = (int)client->physical_height;
	status->display_density = client->actual_density;
	status->requested_fps = client->configured_fps;
	status->actual_fps = client->actual_fps;
	status->native_size = client->display_native;
	snprintf(status->display_error, sizeof(status->display_error), "%s",
		 client->display_error);
}

void airplay_client_set_vehicle_state(struct airplay_client *client,
                                    struct carplay_vehicle_state *state,
                                    uint64_t generation)
{
    if (!client)
        return;
    client->vehicle_state = state;
    client->vehicle_generation = generation;
}

const struct carplay_session_capabilities *airplay_client_capabilities(
	const struct airplay_client *client)
{
	return client ? &client->capabilities : NULL;
}

int airplay_client_get_display_preparation(const struct airplay_client *client,
                                          struct carplay_display_plan *plan)
{
    if (!client || !plan || !client->display_prepare_requested) return 0;
    *plan = client->pending_display_plan;
    return 1;
}

void airplay_client_free(struct airplay_client *client)
{
	if (!client)
		return;
	airplay_timing_stop(&client->timing);
	stop_keepalive_worker(client);
	iap2_log("airplay: free begin encoder thread=%d", client->live_screen_started);
	client->live_screen_stop = 1;
	/* The video worker can be blocked on receiver backpressure. The bridge
	 * owns close(), but shutdown must precede join to wake that send. */
	if (client->screen_data_fd >= 0)
		shutdown(client->screen_data_fd, SHUT_RDWR);
	if (client->live_screen_started) {
		pthread_join(client->encoder_thread, NULL);
		client->live_screen_started = 0;
	}
	(void)carplay_encoder_stop(&client->encoder);
	carplay_display_remember_close(&client->display_memory);
	pthread_mutex_destroy(&client->video_send_mutex);
	pthread_mutex_destroy(&client->event_send_mutex);
	carplay_audio_bridge_destroy(&client->audio_bridge);
	if (client->key_input) {
        struct carplay_keys_stats stats;
        carplay_keys_get_stats(client->key_input, &stats);
        iap2_log("airplay input: knob reports=%lu rejected=%lu keys accepted=%llu emitted=%llu cancelled=%llu failures=%llu",
                 client->knob_reports, client->knob_rejected,
                 (unsigned long long)stats.accepted, (unsigned long long)stats.emitted,
                 (unsigned long long)stats.cancelled, (unsigned long long)stats.failures);
        carplay_keys_destroy(client->key_input);
        client->key_input = NULL;
    }
    airplay_uinput_close(client->touch_input);
	client->touch_input = NULL;
	free(client->last_keyframe);
	client->last_keyframe = NULL;
	client->last_keyframe_len = 0;
	free(client->video_batch);
	airplay_legacy_free(client->legacy_video);
    free(client);
}

int airplay_client_pairing_proven(const struct airplay_client *client)
{
	return client ? client->pairing_proven : 0;
}

int airplay_client_failed(const struct airplay_client *client)
{
	return client ? client->failed || airplay_timing_error(client->timing) != 0 : 0;
}

const char *airplay_client_failure(const struct airplay_client *client)
{
	if (!client)
		return "";
	if (client->failed)
		return client->failure;
	return airplay_timing_error(client->timing) ? "AirPlay timing responder failed" : "";
}

int airplay_client_setup_complete(const struct airplay_client *client)
{
	return client ? client->setup_complete && client->info_complete : 0;
}

uint16_t airplay_client_event_port(const struct airplay_client *client)
{
	return client ? client->event_port : 0;
}

int airplay_client_attach_event(struct airplay_client *client, int fd)
{
	if (!client || !client->setup_complete || fd < 0 ||
	    client->event_fd >= 0)
		return -1;
	client->event_fd = fd;
	iap2_log("airplay: event channel attached port=%u encrypted=%d",
		 client->event_port, client->control_channel_ready);
	return 0;
}

int airplay_client_car_ui_available(const struct airplay_client *client)
{
	return client && client->event_fd >= 0 &&
	       client->capabilities.request_car_ui_supported ? 1 : 0;
}

int airplay_client_request_car_ui(struct airplay_client *client)
{
    /* requestUI is sent only over the live encrypted event channel. Keeping
     * the call self-contained means this control message never tears down the
     * screen, audio, microphone or USB transport. */
    uint8_t command[128];
    size_t command_len = 0;
    char header[256];
    int header_len;
    int result = 0;

    if (!client)
        return AIRPLAY_CAR_UI_UNSUPPORTED;
    if (!client->capabilities.request_car_ui_supported)
        return AIRPLAY_CAR_UI_UNSUPPORTED;
    if (client->event_fd < 0)
        return AIRPLAY_CAR_UI_NOT_READY;
    if (airplay_bplist_encode_car_ui_request(command, sizeof(command),
                                             &command_len) < 0)
        return -1;

    pthread_mutex_lock(&client->event_send_mutex);
    uint64_t started = video_monotonic_ms();
    client->event_request_cseq++;
    header_len = snprintf(header, sizeof(header),
        "POST /command RTSP/1.0\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n"
        "Content-Length: %zu\r\n"
        "CSeq: %u\r\n\r\n",
        command_len, client->event_request_cseq);
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        send_event_frames(client, header, (size_t)header_len, started) < 0 ||
        send_event_frames(client, command, command_len, started) < 0)
        result = -1;
    int saved = errno;
    pthread_mutex_unlock(&client->event_send_mutex);
    if (result == 0) {
        iap2_log("airplay: sent requestUI; CarPlay streams remain active");
        return AIRPLAY_CAR_UI_SENT;
    }
    iap2_log("airplay: requestUI send failed: %s", strerror(saved));
    errno = saved;
    return result;
}

int airplay_client_send_screen_setup(struct airplay_client *client)
{
	struct airplay_stream_setup setups[1 + CARPLAY_AUDIO_MAX_STREAMS];
	uint8_t body[4096];
	size_t body_len = 0;
	size_t stream_count = 1;
	size_t audio_count = 0;

	if (!client || !client->setup_complete || client->screen_setup_sent)
		return -1;
	if (atomic_load(&client->screen_suspended))
		return 1;
	if (client->profile == AIRPLAY_PROFILE_LEGACY && !client->record_accepted)
		return -1;
	if (client->profile == AIRPLAY_PROFILE_LEGACY && !client->initial_modes_accepted)
		return 1;
	client->stream_connection_id = UINT64_C(0x0a0b0c0d0e0f0001);
	memset(setups, 0, sizeof(setups));
	setups[0].type = 110;
	setups[0].stream_id = 1;
	setups[0].connection_id = client->stream_connection_id;
	if (client->profile == AIRPLAY_PROFILE_LEGACY) {
		if (airplay_bplist_encode_legacy_screen_setup(body, sizeof(body), &body_len,
				client->stream_connection_id, client->capabilities.display.uuid) < 0 ||
		    send_request(client, "SETUP", "/", "application/x-apple-binary-plist",
				 9, body, body_len) < 0)
			return -1;
		client->screen_setup_sent = 1;
		return 0;
	}
	if (client->audio_bridge && carplay_modes_audio_active(&client->modes)) {
		audio_count = carplay_audio_bridge_setup_count(client->audio_bridge);
		if (audio_count > CARPLAY_AUDIO_MAX_STREAMS ||
		    carplay_audio_bridge_fill_setups(
			client->audio_bridge, setups + 1,
			CARPLAY_AUDIO_MAX_STREAMS) < 0) {
			iap2_log("airplay: could not collect audio SETUP streams");
			audio_count = 0;
		} else {
			stream_count += audio_count;
		}
	}
	client->audio_setup_deferred = client->audio_bridge && !audio_count &&
		carplay_audio_bridge_setup_count(client->audio_bridge) != 0;
	if (airplay_bplist_encode_stream_setup(
		    body, sizeof(body), &body_len, setups, stream_count) < 0) {
		iap2_log("airplay: failed to encode stream SETUP streams=%zu audio=%zu",
			 stream_count, audio_count);
		return -1;
	}
	if (send_request(client, "SETUP", "/stream",
			 "application/x-apple-binary-plist", 9,
			 body, body_len) < 0) {
		iap2_log("airplay: failed to send stream SETUP streams=%zu audio=%zu body=%zu",
			 stream_count, audio_count, body_len);
		return -1;
	}
	client->screen_setup_sent = 1;
	iap2_log("airplay: sent stream SETUP cseq=9 streams=%zu audio=%zu body=%zu",
		 stream_count, audio_count, body_len);
	return 0;
}

int airplay_client_start_audio(struct airplay_client *client, const char *host)
{
	size_t i;
	size_t started = 0;
	if (!client || !host || client->failed)
		return -1;
	if (strlen(host) >= sizeof(client->audio_host))
		return -1;
	if (host != client->audio_host)
		snprintf(client->audio_host, sizeof(client->audio_host), "%s", host);
	if (client->profile == AIRPLAY_PROFILE_LEGACY)
		return legacy_audio_drive(client);
	if (!carplay_modes_audio_active(&client->modes))
		return 0;
	if (client->audio_setup_deferred && client->screen_setup_complete &&
	    !client->audio_port_count)
		return send_deferred_audio_setup(client);
	if (!client->audio_bridge || !client->audio_port_count) {
		iap2_log("airplay: no negotiated audio streams are available");
		return 0;
	}
	for (i = 0; i < client->audio_port_count; i++) {
		if (carplay_audio_bridge_start_stream(
			client->audio_bridge,
			client->audio_ports[i].type,
			client->audio_ports[i].audio_type,
			client->audio_ports[i].data_port,
			client->audio_ports[i].control_port, host) == 0)
			started++;
		else
			iap2_log("airplay: audio stream start failed type=%d audioType=%s: %s",
				 client->audio_ports[i].type,
				 client->audio_ports[i].audio_type,
				 carplay_audio_bridge_last_error(
					client->audio_bridge));
	}
	iap2_log("airplay: audio streams active=%zu/%zu",
		 started, client->audio_port_count);
	return started == client->audio_port_count ? 0 : -1;
}

static int send_deferred_audio_setup(struct airplay_client *client)
{
	struct airplay_stream_setup setups[CARPLAY_AUDIO_MAX_STREAMS];
	uint8_t body[4096];
	size_t len, count;
	if (!client || client->failed)
		return -1;
	if (client->profile != AIRPLAY_PROFILE_HAP || !client->audio_bridge ||
	    client->audio_setup_pending || client->audio_setup_failed ||
	    !client->audio_setup_deferred || !carplay_modes_audio_active(&client->modes))
		return 0;
	count = carplay_audio_bridge_setup_count(client->audio_bridge);
	if (!count || count > CARPLAY_AUDIO_MAX_STREAMS ||
	    carplay_audio_bridge_fill_setups(client->audio_bridge, setups, count) < 0 ||
	    airplay_bplist_encode_stream_setup(body, sizeof(body), &len, setups, count) < 0) {
		client->audio_setup_failed = 1;
		iap2_log("airplay: could not negotiate deferred audio; video continues");
		return 0;
	}
	if (send_request(client, "SETUP", "/stream", "application/x-apple-binary-plist",
			 11, body, len) < 0) {
		/* A partial encrypted request leaves control framing/nonce state unsafe. */
		client->audio_setup_failed = 1;
		fail_session(client, "deferred audio SETUP control send failed");
		return -1;
	}
	client->audio_setup_pending = 1;
	client->audio_setup_deadline_ms = video_monotonic_ms() + 5000u;
	client->audio_setup_deferred = 0;
	return 0;
}

void airplay_client_get_audio_status(const struct airplay_client *client,
                                     struct carplay_audio_status *status)
{
    carplay_audio_bridge_get_status(client ? client->audio_bridge : NULL, status);
}

int airplay_client_screen_setup_complete(const struct airplay_client *client)
{
	return client ? client->screen_setup_complete : 0;
}

uint16_t airplay_client_screen_data_port(const struct airplay_client *client)
{
	return client ? client->screen_data_port : 0;
}

int airplay_client_attach_screen_data(struct airplay_client *client, int fd)
{
	if (!client || !client->screen_setup_complete || fd < 0 ||
	    client->screen_data_fd >= 0)
		return -1;
	client->screen_data_fd = fd;
	iap2_log("airplay: encrypted screen data channel attached port=%u",
		 client->screen_data_port);
	return 0;
}

int airplay_client_send_record(struct airplay_client *client)
{
	if (!client || !client->setup_complete || client->event_fd < 0 || client->record_sent)
		return -1;
	if (client->profile == AIRPLAY_PROFILE_HAP &&
	    (!client->screen_setup_complete || client->screen_data_fd < 0))
		return -1;
	if (send_request(client, "RECORD", client->profile == AIRPLAY_PROFILE_LEGACY ? "/" : "/stream",
			 NULL, 10, NULL, 0) < 0)
		return -1;
	client->record_sent = 1;
	iap2_log("airplay: sent RECORD cseq=10");
	return 0;
}

static int send_modes_changed(struct airplay_client *client)
{
	uint8_t body[2048];
	size_t len;
	if (airplay_bplist_encode_modes_changed(body, sizeof(body), &len, &client->modes) < 0)
		return -1;
	iap2_log("airplay: modesChanged screen=%d/%d audio=%d/%d",
		 client->modes.screen_entity, client->modes.screen_permanent_entity,
		 client->modes.audio_entity, client->modes.audio_permanent_entity);
	return send_request(client, "POST", "/command", "application/x-apple-binary-plist",
			    client->next_control_cseq++, body, len);
}

static int legacy_audio_command_ok(unsigned status, const uint8_t *body, size_t len)
{
    struct airplay_bplist_document d; struct airplay_bplist_value root, value;
    uint64_t result = 0; int found;
    if (status != 200) return 0;
    if (!len) return 1;
    if (airplay_bplist_init(&d, body, len) < 0 || airplay_bplist_root(&d, &root) < 0 ||
        root.type != AIRPLAY_BPLIST_DICTIONARY) return 0;
    found = airplay_bplist_dict_get(&d, &root, "status", &value);
    return found == 0 || (found == 1 && airplay_bplist_get_uint64(&d, &value, &result) == 0 && result == 0);
}

static void legacy_audio_backoff(struct airplay_client *c)
{
    unsigned failures = c->legacy_audio_failures;
    if (c->legacy_audio_target > 0) c->legacy_audio_refused_call = 1;
    else if (c->legacy_audio_target < 0) {
        /* A refused release must keep reconciling, without becoming a
         * refused call attempt or disabling the session's audio support. */
        if (c->legacy_audio_release_failures < 2) c->legacy_audio_release_failures++;
        failures = c->legacy_audio_release_failures;
    } else {
        failures = ++c->legacy_audio_failures;
        if (failures >= 3) c->audio_setup_failed = 1;
    }
    c->legacy_audio_retry_ms = video_monotonic_ms() +
        (1000u << (failures > 2 ? 2 : failures));
    c->legacy_audio_phase = 0; c->legacy_audio_cseq = 0; c->legacy_audio_deadline = 0;
    iap2_log("airplay: optional legacy audio unavailable; retry_ms=%llu disabled=%d",
             (unsigned long long)c->legacy_audio_retry_ms, c->audio_setup_failed);
}

static int legacy_audio_setup(struct airplay_client *c)
{
    struct airplay_stream_setup setup = {0}; uint8_t body[2048]; size_t len;
    if (carplay_audio_bridge_fill_setups(c->audio_bridge, &setup, 1) < 0 ||
        airplay_bplist_encode_stream_setup(body, sizeof(body), &len, &setup, 1) < 0) {
        legacy_audio_backoff(c); return 0;
    }
    c->legacy_audio_cseq = c->next_control_cseq++;
    if (send_request(c, "SETUP", "/", "application/x-apple-binary-plist",
                     c->legacy_audio_cseq, body, len) < 0) {
        fail_session(c, "legacy audio SETUP control send failed"); return -1;
    }
    c->legacy_audio_phase = 3;
    c->legacy_audio_deadline = video_monotonic_ms() + 10000u;
    return 0;
}

static int legacy_audio_response(struct airplay_client *c, unsigned status,
                                 unsigned cseq, const uint8_t *body, size_t len)
{
    int phase = c->legacy_audio_phase;
    if (!cseq || cseq != c->legacy_audio_cseq || phase == 0 || phase == 4) return 0;
    c->legacy_audio_cseq = 0; c->legacy_audio_deadline = 0;
    if (phase == 1) {
        c->legacy_audio_have_stream = 0; c->legacy_audio_phase = 0;
        if (status != 200) {
            /* Unknown receiver stream lifetime: never overlap another type100. */
            c->audio_setup_failed = 1;
            if (c->modes.phone_call || c->legacy_audio_call_announced) {
                c->legacy_audio_call_announced = 1;
                c->modes.phone_call = 0;
                c->legacy_audio_target = -1;
                c->legacy_audio_cseq = c->next_control_cseq;
                if (send_modes_changed(c) < 0) {
                    fail_session(c, "legacy call release control send failed"); return -1;
                }
                c->legacy_audio_phase = 2;
                c->legacy_audio_deadline = video_monotonic_ms() + 1500u;
            }
            iap2_log("airplay: audio TEARDOWN unconfirmed; optional audio disabled");
        }
        return 0;
    }
    if (phase == 2) {
        if (!legacy_audio_command_ok(status, body, len)) {
            legacy_audio_backoff(c); return 0;
        }
        if (c->legacy_audio_target <= 0) {
            c->legacy_audio_call_announced = 0;
            c->legacy_audio_release_failures = 0;
        }
        c->legacy_audio_phase = c->legacy_audio_target < 0 ? 0 : 4;
        return 0;
    }
    c->legacy_audio_phase = 0;
    if (status != 200 || complete_stream_setup(c, body, len, 1) < 0) {
        /* A successful but malformed response may already own a stream. */
        c->legacy_audio_have_stream = status == 200 || status == 408;
        c->audio_port_count = 0;
        carplay_audio_bridge_legacy_stop_stream(c->audio_bridge);
        legacy_audio_backoff(c); return 0;
    }
    c->legacy_audio_have_stream = 1; c->legacy_audio_failures = 0;
    if (carplay_modes_audio_active(&c->modes) &&
        c->legacy_audio_target == (carplay_audio_bridge_microphone_demand(c->audio_bridge) &&
                                   !c->legacy_audio_refused_call)) {
        if (carplay_audio_bridge_start_stream(c->audio_bridge, 100,
                c->audio_ports[0].audio_type, c->audio_ports[0].data_port,
                c->audio_ports[0].control_port, c->audio_host) < 0) {
            carplay_audio_bridge_legacy_stop_stream(c->audio_bridge);
            legacy_audio_backoff(c);
        }
    }
    return 0;
}

static int legacy_nav_drive(struct airplay_client *c, uint64_t now)
{
    if (c->legacy_nav_attempted) return 0;
    struct airplay_stream_setup setup;
    uint8_t body[512]; size_t len;
    int ready = carplay_audio_bridge_legacy_nav_setup(c->audio_bridge, &setup);
    if (!ready) return 0;
    c->legacy_nav_attempted = 1;
    if (ready < 0 || airplay_bplist_encode_stream_setup(body, sizeof(body), &len, &setup, 1) < 0) {
        iap2_log("airplay: cannot prepare navigation 101; retaining local mix");
        return 0;
    }
    c->legacy_nav_cseq = c->next_control_cseq++;
    c->legacy_nav_deadline = now + 1500u;
    if (send_request(c, "SETUP", "/", "application/x-apple-binary-plist",
                     c->legacy_nav_cseq, body, len) < 0) {
        fail_session(c, "navigation 101 SETUP control send failed");
        return -1;
    }
    iap2_log("airplay: requested navigation SETUP type=101 format=0x10");
    return 0;
}

static int legacy_audio_drive(struct airplay_client *c)
{
    uint64_t now = video_monotonic_ms(); int desired;
    if (c->failed) return -1;
    if (!c->audio_bridge || !c->audio_host[0] || !c->screen_setup_complete ||
        !c->record_accepted || !c->initial_modes_accepted ||
        (c->audio_setup_failed && !c->legacy_audio_call_announced))
        return 0;
    if (c->legacy_audio_deadline && now >= c->legacy_audio_deadline &&
        legacy_audio_response(c, 408, c->legacy_audio_cseq, NULL, 0) < 0)
        return -1;
    if (c->legacy_nav_deadline && now >= c->legacy_nav_deadline)
        (void)legacy_nav_response(c, 408, c->legacy_nav_cseq, NULL, 0);
    if ((c->audio_setup_failed && !c->legacy_audio_call_announced) ||
        (c->legacy_audio_phase && c->legacy_audio_phase != 4)) return 0;
    desired = carplay_audio_bridge_microphone_demand(c->audio_bridge);
    if (!desired && c->legacy_audio_last_demand) c->legacy_audio_refused_call = 0;
    c->legacy_audio_last_demand = desired;
    if (c->legacy_audio_refused_call) desired = 0;
    struct carplay_audio_status audio_status;
    carplay_audio_bridge_get_status(c->audio_bridge, &audio_status);
    if (!desired && !audio_status.media_available) desired = -1;
    if (c->audio_setup_failed) desired = -1;
    if (!carplay_modes_audio_active(&c->modes) || now < c->legacy_audio_retry_ms) return 0;
    if (c->legacy_audio_phase == 4) {
        if (desired == c->legacy_audio_target && desired >= 0) return legacy_audio_setup(c);
        c->legacy_audio_phase = 0;
    }
    if (c->legacy_audio_have_stream) {
        if (desired == c->legacy_audio_target && c->audio_port_count) {
            if (!carplay_audio_bridge_active_count(c->audio_bridge) &&
                carplay_audio_bridge_start_stream(c->audio_bridge, 100,
                    c->audio_ports[0].audio_type, c->audio_ports[0].data_port,
                    c->audio_ports[0].control_port, c->audio_host) < 0) {
                c->audio_port_count = 0; legacy_audio_backoff(c);
            }
            return desired == 0 ? legacy_nav_drive(c, now) : 0;
        }
        uint8_t body[128]; size_t len;
        struct airplay_stream_setup setup = {.type = 100, .legacy_audio = 1};
        /* A minimal stream entry is tolerated without audioType for teardown. */
        if (airplay_bplist_encode_stream_setup(body, sizeof(body), &len, &setup, 1) < 0) {
            c->audio_setup_failed = 1; return 0;
        }
        carplay_audio_bridge_legacy_stop_stream(c->audio_bridge); c->audio_port_count = 0;
        c->legacy_audio_cseq = c->next_control_cseq++;
        if (send_request(c, "TEARDOWN", "/", "application/x-apple-binary-plist",
                         c->legacy_audio_cseq, body, len) < 0) {
            fail_session(c, "legacy audio TEARDOWN control send failed"); return -1;
        }
        c->legacy_audio_phase = 1; c->legacy_audio_deadline = now + 1500u;
        return 0;
    }
    if (desired < 0 && !c->modes.phone_call && !c->legacy_audio_call_announced) return 0;
    c->legacy_audio_target = desired;
    if (desired >= 0 && carplay_audio_bridge_legacy_select(c->audio_bridge, desired) < 0) {
        legacy_audio_backoff(c); return 0;
    }
    c->modes.phone_call = desired > 0;
    if (c->modes.phone_call) c->legacy_audio_call_announced = 1;
    c->legacy_audio_cseq = c->next_control_cseq;
    if (send_modes_changed(c) < 0) {
        fail_session(c, "legacy audio modesChanged control send failed"); return -1;
    }
    c->legacy_audio_phase = 2; c->legacy_audio_deadline = now + 1500u;
    return 0;
}

static int open_keepalive_socket(struct airplay_client *client)
{
	struct sockaddr_storage local, peer;
	socklen_t local_len = sizeof(local), peer_len = sizeof(peer);
	char interface[IFNAMSIZ] = {0};
	socklen_t interface_len = sizeof(interface);
	int fd;
	if (getsockname(client->fd, (struct sockaddr *)&local, &local_len) < 0 ||
	    getpeername(client->fd, (struct sockaddr *)&peer, &peer_len) < 0)
		return -1;
	if (peer.ss_family == AF_INET) {
		((struct sockaddr_in *)&local)->sin_port = 0;
		((struct sockaddr_in *)&peer)->sin_port = htons(client->keep_alive_port);
	} else if (peer.ss_family == AF_INET6) {
		((struct sockaddr_in6 *)&local)->sin6_port = 0;
		((struct sockaddr_in6 *)&peer)->sin6_port = htons(client->keep_alive_port);
	} else return -1;
	fd = socket(peer.ss_family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	if (getsockopt(client->fd, SOL_SOCKET, SO_BINDTODEVICE, interface, &interface_len) == 0 &&
	    interface[0] && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface, interface_len) < 0)
		goto fail;
	if (bind(fd, (struct sockaddr *)&local, local_len) < 0 ||
	    connect(fd, (struct sockaddr *)&peer, peer_len) < 0)
		goto fail;
	client->keepalive_fd = fd;
	return 0;
fail:
	close(fd);
	return -1;
}

static void send_udp_keepalive(struct airplay_client *client)
{
	static const uint8_t ping[4] = {0};
	if (send(client->keepalive_fd, ping, sizeof(ping), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
	    errno != EAGAIN && errno != EWOULDBLOCK)
		iap2_log("airplay: UDP keepalive send failed errno=%d", errno);
}

static void *keepalive_main(void *opaque)
{
	struct airplay_client *client = opaque;
	while (!atomic_load(&client->keepalive_stop)) {
		struct pollfd wake = {client->keepalive_wake[0], POLLIN, 0};
		int result = poll(&wake, 1, 1000);
		if (atomic_load(&client->keepalive_stop) || result > 0)
			break;
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		send_udp_keepalive(client);
	}
	return NULL;
}

static int start_keepalive_worker(struct airplay_client *client)
{
	if (client->profile != AIRPLAY_PROFILE_LEGACY ||
	    !client->keep_alive_port_present || client->keepalive_started)
		return 0;
	if (open_keepalive_socket(client) < 0)
		return -1;
	if (pipe(client->keepalive_wake) < 0)
		goto fail;
	(void)fcntl(client->keepalive_wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(client->keepalive_wake[1], F_SETFD, FD_CLOEXEC);
	atomic_store(&client->keepalive_stop, 0);
	/* Before requesting /info: that path can block in display/event setup. */
	send_udp_keepalive(client);
	if (pthread_create(&client->keepalive_thread, NULL, keepalive_main, client) != 0) {
		close(client->keepalive_wake[0]);
		close(client->keepalive_wake[1]);
		client->keepalive_wake[0] = client->keepalive_wake[1] = -1;
		goto fail;
	}
	client->keepalive_started = 1;
	return 0;
fail:
	close(client->keepalive_fd);
	client->keepalive_fd = -1;
	return -1;
}

static void stop_keepalive_worker(struct airplay_client *client)
{
	if (client->keepalive_started) {
		const uint8_t wake = 1;
		atomic_store(&client->keepalive_stop, 1);
		(void)write(client->keepalive_wake[1], &wake, 1);
		pthread_join(client->keepalive_thread, NULL);
		client->keepalive_started = 0;
		close(client->keepalive_wake[0]);
		close(client->keepalive_wake[1]);
		client->keepalive_wake[0] = client->keepalive_wake[1] = -1;
	}
	if (client->keepalive_fd >= 0) {
		close(client->keepalive_fd);
		client->keepalive_fd = -1;
	}
}

int airplay_client_tick(struct airplay_client *client)
{
	uint64_t now;
	uint8_t message[AIRPLAY_SCREEN_HEADER_BYTES + 42];
	size_t len;
	if (!client || client->failed) return -1;
	if (atomic_load(&client->video_failure)) {
		fail_session(client, "video recovery exhausted; reconnect required");
		return -1;
	}
	now = video_monotonic_ms();
	if (client->initial_modes_deadline_ms && now >= client->initial_modes_deadline_ms) {
		fail_session(client, "initial modesChanged response timeout");
		return -1;
	}
	if (client->profile == AIRPLAY_PROFILE_HAP && client->audio_setup_pending && now >= client->audio_setup_deadline_ms) {
		client->audio_setup_pending = 0;
		client->audio_setup_deadline_ms = 0;
		client->audio_setup_failed = 1;
		iap2_log("airplay: deferred audio SETUP timed out; video continues");
	}
	if (client->profile == AIRPLAY_PROFILE_LEGACY && legacy_audio_drive(client) < 0)
		return -1;
	if ((client->handshake_deadline_ms && now >= client->handshake_deadline_ms) ||
	    (client->feedback_deadline_ms && now >= client->feedback_deadline_ms)) {
		char reason[96];
		snprintf(reason, sizeof(reason), "RTSP response timeout cseq=%u monotonic_ms=%llu",
			 client->pending_handshake_cseq ? client->pending_handshake_cseq : client->pending_feedback_cseq,
			 (unsigned long long)now);
		fail_session(client, reason);
		return -1;
	}
	/* Preserve the established HAP liveness behavior until both endpoints can
	 * be bench-tested. Legacy needs all three receiver activity signals. */
	if (client->profile != AIRPLAY_PROFILE_LEGACY || !client->record_accepted ||
	    (client->last_feedback_ms && now - client->last_feedback_ms < 1000u))
		return 0;
	client->last_feedback_ms = now;
	if (!client->pending_feedback_cseq) {
		client->pending_feedback_cseq = client->next_control_cseq++;
		if (send_request(client, "POST", "/feedback", NULL,
				 client->pending_feedback_cseq, NULL, 0) < 0)
			goto transport_error;
		client->feedback_deadline_ms = now + 15000u;
	}
	if (client->screen_data_fd >= 0) {
		int result = 0;
		if (airplay_screen_encode_keepalive(message, sizeof(message), &len,
				client->keepalive_stats_body) < 0)
			goto transport_error;
		/* Liveness checks must keep running while video waits on receiver
		 * backpressure. Its writer owns a separate bounded send deadline. */
		int locked = pthread_mutex_trylock(&client->video_send_mutex);
		if (locked == EBUSY) return 0;
		if (locked != 0) goto transport_error;
		if (!atomic_load(&client->screen_suspended))
			result = send_all(client->screen_data_fd, message, len);
		pthread_mutex_unlock(&client->video_send_mutex);
		if (result < 0) goto transport_error;
	}
	return 0;
transport_error:
	fail_session(client, "legacy session liveness send failed");
	return -1;
}

static int encode_video_frame(struct airplay_client *client, uint8_t *output,
	size_t capacity, size_t *len, const uint8_t *frame, size_t frame_len,
	uint64_t ntp, int keyframe)
{
	if (client->profile == AIRPLAY_PROFILE_LEGACY)
		return airplay_screen_encode_legacy_frame(output, capacity, len,
			client->legacy_video, keyframe, frame, frame_len, ntp);
	return airplay_screen_encode_frame(output, capacity, len, client->video_key,
		client->video_write_counter++, keyframe, frame, frame_len, ntp);
}

static int encode_video_config(struct airplay_client *client, uint8_t *output,
	size_t capacity, size_t *len, const uint8_t *config, size_t config_len, uint64_t ntp,
	unsigned content_width, unsigned content_height,
	unsigned display_width, unsigned display_height)
{
	if (client->profile == AIRPLAY_PROFILE_LEGACY)
		return airplay_screen_encode_legacy_config(output, capacity, len,
			config, config_len, ntp, content_width, content_height,
			display_width, display_height);
	return airplay_screen_encode_config(output, capacity, len, config, config_len, ntp);
}


struct h264_send_context {
	struct airplay_client *client;
};

/* video_send_mutex is held by the caller. Configuration is plaintext in the
 * legacy framing and must never consume the continuous AES-CTR stream. */
static void screen_output_size(const struct airplay_client *client,
			       unsigned *width, unsigned *height);

static int send_cached_screen_config_locked(struct airplay_client *client)
{
	uint8_t message[AIRPLAY_SCREEN_HEADER_BYTES + 1024];
	size_t len = 0;
	unsigned content_width = client->latest_screen_config_width;
	unsigned content_height = client->latest_screen_config_height;
	unsigned display_width = client->input_width;
	unsigned display_height = client->input_height;
	if (!client->latest_screen_config_len)
		return 1;
	screen_output_size(client, &content_width, &content_height);
	if (carplay_capabilities_output_size(&client->capabilities,
			client->input_width, client->input_height,
			&display_width, &display_height) < 0) {
		display_width = client->input_width;
		display_height = client->input_height;
	}
	if (encode_video_config(client, message, sizeof(message), &len,
		client->latest_screen_config, client->latest_screen_config_len,
		screen_sender_ntp(), content_width, content_height,
		display_width, display_height) < 0 ||
	    send_all(client->screen_data_fd, message, len) < 0)
		return -1;
	client->screen_config_pending = 0;
	client->live_config_count++;
	return 0;
}

static int send_screen_config(struct airplay_client *client,
			      const uint8_t *config, size_t config_len)
{
	int result;

	if (!client || !config || !config_len ||
	    config_len > sizeof(client->latest_screen_config))
		return -1;
	pthread_mutex_lock(&client->video_send_mutex);
	memcpy(client->latest_screen_config, config, config_len);
	client->latest_screen_config_len = config_len;
	screen_output_size(client, &client->latest_screen_config_width,
		&client->latest_screen_config_height);
	client->screen_config_pending = 1;
	result = atomic_load(&client->screen_suspended) ||
		atomic_load(&client->screen_needs_keyframe) ? 0 :
		send_cached_screen_config_locked(client);
	pthread_mutex_unlock(&client->video_send_mutex);
	return result;
}

static int send_h264_config(void *opaque, const uint8_t *config,
			    size_t config_len)
{
	struct h264_send_context *context = opaque;

	return send_screen_config(context->client, config, config_len);
}

static uint64_t video_monotonic_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000u +
	       (uint64_t)now.tv_nsec / 1000000u;
}

static int video_batch_reserve(struct airplay_client *client, size_t extra)
{
	size_t needed;
	size_t capacity;
	uint8_t *grown;

	if (!client || extra > SIZE_MAX - client->video_batch_len)
		return -1;
	needed = client->video_batch_len + extra;
	if (needed <= client->video_batch_capacity)
		return 0;
	capacity = client->video_batch_capacity ? client->video_batch_capacity : 65536u;
	while (capacity < needed && capacity <= SIZE_MAX / 2u)
		capacity *= 2u;
	if (capacity < needed)
		capacity = needed;
	grown = realloc(client->video_batch, capacity);
	if (!grown)
		return -1;
	client->video_batch = grown;
	client->video_batch_capacity = capacity;
	return 0;
}

static int video_batch_append(struct airplay_client *client,
			      const uint8_t *data, size_t len)
{
	if (!client || (!data && len) ||
	    video_batch_reserve(client, len) < 0)
		return -1;
	if (len) {
		memcpy(client->video_batch + client->video_batch_len, data, len);
		client->video_batch_len += len;
	}
	return 0;
}

static int video_batch_append_size(struct airplay_client *client, size_t len)
{
	uint8_t bytes[4];
	if (len > UINT32_MAX)
		return -1;
	bytes[0] = (uint8_t)(len >> 24);
	bytes[1] = (uint8_t)(len >> 16);
	bytes[2] = (uint8_t)(len >> 8);
	bytes[3] = (uint8_t)len;
	return video_batch_append(client, bytes, sizeof(bytes));
}

static int send_screen_payload(struct airplay_client *client,
			       const uint8_t *payload, size_t payload_len,
			       unsigned long nal_count, int fresh_keyframe)
{
	uint8_t *message;
	size_t message_len = 0;
	int result;

	if (!client || !payload || !payload_len)
		return 0;
	if (!client->screen_data_fd || client->screen_data_fd < 0)
		return -1;
	if (payload_len > SIZE_MAX - AIRPLAY_SCREEN_HEADER_BYTES -
			  AIRPLAY_CHACHA_TAG_BYTES)
		return -1;
	message = malloc(AIRPLAY_SCREEN_HEADER_BYTES + payload_len +
			 AIRPLAY_CHACHA_TAG_BYTES);
	if (!message)
		return -1;
	pthread_mutex_lock(&client->video_send_mutex);
	if (atomic_load(&client->screen_suspended) ||
	    (atomic_load(&client->screen_needs_keyframe) && !fresh_keyframe)) {
		pthread_mutex_unlock(&client->video_send_mutex);
		free(message);
		return 0;
	}
	if (fresh_keyframe && (client->screen_config_pending ||
		atomic_load(&client->screen_needs_keyframe))) {
		result = send_cached_screen_config_locked(client);
		if (result != 0) {
			pthread_mutex_unlock(&client->video_send_mutex);
			free(message);
			return result < 0 ? -1 : 0;
		}
	}
	result = encode_video_frame(client,
		message, AIRPLAY_SCREEN_HEADER_BYTES + payload_len +
			 AIRPLAY_CHACHA_TAG_BYTES,
		&message_len,
		payload, payload_len, screen_sender_ntp(), fresh_keyframe);
	if (result == 0)
		result = send_all(client->screen_data_fd, message, message_len);
	if (result == 0 && fresh_keyframe)
		atomic_store(&client->screen_needs_keyframe, 0);
	pthread_mutex_unlock(&client->video_send_mutex);
	free(message);
	if (result == 0) {
		if (client->live_frame_count++ == 0)
			iap2_log("airplay: live screen first packet sent body=%zu nals=%lu",
				 payload_len, nal_count);
		client->video_batch_packets++;
        /* Only a sent real IDR enables background persistence. The encoder
         * loop polls completion and retries; admission is not a saved profile. */
        if (fresh_keyframe) client->display_profile_eligible = 1;
	}
	return result;
}

static int save_last_keyframe(struct airplay_client *client);

static int flush_video_batch(struct airplay_client *client)
{
	int result = 0;

	if (!client || !client->video_batch_len)
		return 0;
	if (atomic_load(&client->screen_suspended) ||
	    (atomic_load(&client->screen_needs_keyframe) && !client->video_batch_keyframe))
		goto discard;
	/*
	 * Snapshot the keyframe here, once the whole picture is in the buffer. It
	 * used to be snapshotted as soon as a type-5 slice arrived, which keeps only
	 * the *first* slice of a multi-slice IDR: resending that later repaints one
	 * band of the screen and leaves the rest at whatever it was, which looks
	 * exactly like the partial mosaic the user sees.
	 */
	if (client->video_batch_keyframe) {
		(void)save_last_keyframe(client);
		client->last_keyframe_current = 1;
	} else {
		client->last_keyframe_current = 0;
	}
	result = send_screen_payload(client, client->video_batch,
				     client->video_batch_len,
				     client->video_batch_nals, client->video_batch_keyframe);
	if (client->flushed_pictures < 5u)
		iap2_log("airplay: picture #%lu slices=%u bytes=%zu keyframe=%d",
			 client->flushed_pictures, client->video_batch_slices,
			 client->video_batch_len, client->video_batch_keyframe);
	client->flushed_pictures++;
discard:
	client->video_batch_len = 0;
	client->video_batch_nals = 0;
	client->video_batch_slices = 0;
	client->video_batch_start_ms = 0;
	client->video_batch_keyframe = 0;
	return result;
}

/* Keep the newest keyframe (avcC-style: [len][SPS][len][PPS][len][IDR]) so a
 * still desktop can be refreshed without re-encoding. Called before the batch is
 * flushed, while the bytes are still buffered. */
static int save_last_keyframe(struct airplay_client *client)
{
	uint8_t *copy;

	if (!client || !client->video_batch_len)
		return -1;
	copy = malloc(client->video_batch_len);
	if (!copy)
		return -1;
	memcpy(copy, client->video_batch, client->video_batch_len);
	free(client->last_keyframe);
	client->last_keyframe = copy;
	client->last_keyframe_len = client->video_batch_len;
	client->last_keyframe_nals = client->video_batch_nals;
	client->last_keyframe_ms = video_monotonic_ms();
	return 0;
}

/* Re-encrypt and resend the last keyframe. The phone repaints the same picture,
 * which keeps the decoder and the TCP path warm while the desktop is idle.
 *
 * Only ever resend a keyframe that is still the newest picture on the wire.
 * Anything older rewinds the decoder to a reference the deltas that follow do
 * not build on, so the sink paints garbage until the encoder's next IDR - the
 * same partial mosaic from the other direction. */
static int resend_last_keyframe(struct airplay_client *client)
{
	int result;

	if (!client || !client->last_keyframe || !client->last_keyframe_len)
		return 0;
	if (!client->last_keyframe_current)
		return 0;
	result = send_screen_payload(client, client->last_keyframe,
				     client->last_keyframe_len,
				     client->last_keyframe_nals, 1);
	if (result == 0) {
		client->last_screen_frame_ms = video_monotonic_ms();
		if (client->still_refreshes++ == 0)
			iap2_log("airplay: screen is still; refreshing the last keyframe at 1 fps bytes=%zu",
				 client->last_keyframe_len);
	}
	return result;
}

static int send_h264_frame(void *opaque, const uint8_t *nal, size_t nal_len,
			   int keyframe)
{
	struct h264_send_context *context = opaque;
	struct airplay_client *client = context ? context->client : NULL;
	int picture_start;

	if (!client || !nal || !nal_len)
		return -1;
	if (client->live_screen_stop)
		return 0;
	/* Picture, not slice, is the unit the sink decodes. The Android encoder
	 * codes most frames as a single slice but switches to several slices when a
	 * picture is expensive - scrolling a full app grid is the classic case - and
	 * two packets for one picture makes the sink render it half-decoded. That is
	 * the mosaic that clears only at the next IDR. */
	picture_start = airplay_h264_slice_starts_picture(nal, nal_len);
	if (picture_start && client->video_batch_len &&
	    flush_video_batch(client) < 0)
		return -1;
	if (!client->video_batch_len) {
		client->video_batch_start_ms = video_monotonic_ms();
		client->video_batch_keyframe = keyframe && picture_start;
		client->video_batch_slices = 0;
		/* SPS/PPS travel in the separate cleartext VideoConfig packet. The
		 * encrypted access unit must contain picture data only; embedding the
		 * parameter sets again makes some car receivers reconfigure mid-IDR. */
	}
	if (video_batch_append_size(client, nal_len) < 0 ||
	    video_batch_append(client, nal, nal_len) < 0)
		return -1;
	client->video_batch_nals++;
	client->video_batch_slices++;
	client->video_batch_last_append_ms = video_monotonic_ms();
	client->last_screen_frame_ms = client->video_batch_last_append_ms;
	if (!client->has_real_frame) {
		client->has_real_frame = 1;
		iap2_log("airplay: first real capture frame arrived; placeholder retired");
	}
	/* A picture this large cannot be the one being streamed: at the configured
	 * bitrate a picture is a couple of hundred kilobytes at most. Flushing here
	 * only bounds memory if the encoder ever misbehaves. */
	if (client->video_batch_len >= 4u * 1024u * 1024u)
		return flush_video_batch(client);
	return 0;
}

static void screen_output_size(const struct airplay_client *client,
                               unsigned *width, unsigned *height)
{
    if (!client || !width || !height) return;
    *width = client->output_width ? client->output_width : 800;
    *height = client->output_height ? client->output_height : 480;
}

static uint64_t screen_sender_ntp(void);
static int send_placeholder_frame(struct airplay_client *client,
				  uint64_t sender_ntp);

static int consume_encoder_packet(struct airplay_client *client,
                                  struct airplay_h264_parser *parser,
                                  const struct carplay_encoder_packet *packet)
{
    if (packet->flags & CARPLAY_ENCODER_EOS)
        return -1;
    if (airplay_h264_feed(parser, packet->data, packet->size) < 0 ||
        airplay_h264_flush(parser) < 0)
        return -1;
    /* MediaCodec provides a whole access unit, including all its slices.
     * Release the last NAL and picture now, without waiting for another frame. */
    return flush_video_batch(client);
}

static void reset_encoder_picture(struct airplay_client *client)
{
    pthread_mutex_lock(&client->video_send_mutex);
    client->latest_screen_config_len = 0;
    client->screen_config_pending = 0;
    atomic_store(&client->screen_needs_keyframe, 1);
    pthread_mutex_unlock(&client->video_send_mutex);
    client->video_batch_len = 0;
    client->video_batch_nals = 0;
    client->video_batch_slices = 0;
    client->video_batch_keyframe = 0;
    free(client->last_keyframe);
    client->last_keyframe = NULL;
    client->last_keyframe_len = 0;
    client->last_keyframe_current = 0;
}

static int start_encoder(struct airplay_client *client)
{
    unsigned width, height;
    unsigned fps = client->actual_fps > 0 ? (unsigned)client->actual_fps : 30u;
    screen_output_size(client, &width, &height);
    client->last_encoder_spawn_ms = video_monotonic_ms();
    if (carplay_encoder_start_scaled(&client->encoder, width, height,
                              client->bitrate, fps, client->capture_width, client->capture_height) < 0) {
        iap2_log("airplay: MediaCodec launch failed errno=%d; stderr=/data/carplay/logs/encoder.err", errno);
        return -1;
    }
    client->encoder_restarts++;
    iap2_log("airplay: MediaCodec started pid=%d size=%ux%u fps=%u bitrate=%u native=%d",
             (int)client->encoder.pid, width, height, fps, client->bitrate,
             client->display_native);
    return 0;
}

static void *encoder_main(void *opaque)
{
    struct airplay_client *client = opaque;
    struct airplay_h264_parser parser;
    struct h264_send_context context = { client };
    struct carplay_video_restart_budget restart_budget = {0};
    uint8_t *nal_buffer = malloc(CARPLAY_ENCODER_MAX_PACKET);
    if (!nal_buffer) {
        atomic_store(&client->video_failure, 1);
        return NULL;
    }
    while (!atomic_load(&client->live_screen_stop)) {
        if (client->display_profile_eligible)
            carplay_display_remember_step(&client->display_memory, client->physical_width,
                client->physical_height, (unsigned)client->actual_density, video_monotonic_ms());
        if (client->encoder.fd < 0) {
            uint64_t now = video_monotonic_ms();
            if (client->last_encoder_spawn_ms &&
                now - client->last_encoder_spawn_ms < ENCODER_RESTART_MIN_MS) {
                usleep(50000);
                continue;
            }
            if (start_encoder(client) < 0) {
                if (!carplay_video_restart_allowed(&restart_budget, now)) {
                    atomic_store(&client->video_failure, 1);
                    break;
                }
                continue;
            }
        }
        reset_encoder_picture(client);
        airplay_h264_parser_init(&parser, nal_buffer, CARPLAY_ENCODER_MAX_PACKET,
                                 send_h264_config, send_h264_frame, &context);
        /* Java projects the full preset into this visible rectangle within
         * the aligned encoder surface. Crop only allocation padding. */
        airplay_h264_parser_set_stream_crop(&parser, client->output_width,
                                            client->output_height);
        iap2_log("airplay: preset capture source=%ux%u output=%ux%u; codec padding only",
                 client->capture_width, client->capture_height,
                 client->output_width, client->output_height);
        struct carplay_video_recovery recovery;
        carplay_video_recovery_init(&recovery, video_monotonic_ms());
        while (!atomic_load(&client->live_screen_stop)) {
            uint64_t now = video_monotonic_ms();
            if (client->display_profile_eligible)
                carplay_display_remember_step(&client->display_memory, client->physical_width,
                    client->physical_height, (unsigned)client->actual_density, now);
            int suspended = atomic_load(&client->screen_suspended);
            unsigned resume_generation = atomic_load(&client->screen_resume_generation);
            enum carplay_video_recovery_action action = carplay_video_recovery_tick(
                &recovery, now, suspended, resume_generation);
            if (action == CARPLAY_VIDEO_RESTART) {
                iap2_log("airplay: encoder recovery timeout initial=%d phase=%u frame_idle_ms=%llu heartbeat_idle_ms=%llu",
                         !recovery.has_frame, recovery.phase,
                         (unsigned long long)(now-recovery.last_frame_ms),
                         (unsigned long long)(now-recovery.last_heartbeat_ms));
                break;
            }
            if (action == CARPLAY_VIDEO_KEYFRAME) {
                atomic_store(&client->force_key_frame, 1);
                iap2_log("airplay: video idle; probing encoder pid=%d", (int)client->encoder.pid);
            } else if (action == CARPLAY_VIDEO_REFRESH) {
                iap2_log("airplay: encoder responsive without fresh frames; refreshing capture pid=%d",
                         (int)client->encoder.pid);
                if (carplay_encoder_request_refresh(&client->encoder) < 0) break;
            }
            if (atomic_load(&client->force_key_frame) &&
                now-client->last_force_key_frame_ms >= FORCE_KEY_FRAME_MIN_MS) {
                atomic_exchange(&client->force_key_frame, 0);
                if (carplay_encoder_request_keyframe(&client->encoder) < 0) {
                    if (errno==EAGAIN || errno==EWOULDBLOCK)
                        atomic_store(&client->force_key_frame, 1);
                    else break;
                } else {
                    client->last_force_key_frame_ms=now;
                    iap2_log("airplay: requested MediaCodec sync frame pid=%d", (int)client->encoder.pid);
                }
            }
            struct carplay_encoder_packet packet;
            int result = carplay_encoder_reader_read(&client->encoder.reader,
                            client->encoder.fd, &packet, now);
            if (result < 0) {
                iap2_log("airplay: MediaCodec stream ended errno=%d pid=%d; stderr=/data/carplay/logs/encoder.err",
                         errno, (int)client->encoder.pid);
                break;
            }
            if (result > 0) {
                if (packet.flags == CARPLAY_ENCODER_HEARTBEAT) {
                    carplay_video_recovery_heartbeat(&recovery, now);
                    continue;
                }
                unsigned long frames_before = client->live_frame_count;
                if (consume_encoder_packet(client, &parser, &packet) < 0) break;
                if (!(packet.flags & CARPLAY_ENCODER_CONFIG) &&
                    client->live_frame_count != frames_before) {
                    unsigned recovered = carplay_video_recovery_frame(
                        &recovery, video_monotonic_ms(), packet.pts_us);
                    if (recovered)
                        iap2_log("airplay: fresh video resumed after probe phase=%u pid=%d",
                                 recovered, (int)client->encoder.pid);
                }
                client->last_capture_read_ms=now;
                continue;
            }
            if (client->has_real_frame) {
                if (now-client->last_screen_frame_ms >= STILL_FRAME_REFRESH_MS &&
                    resend_last_keyframe(client) < 0) break;
            } else if (now-client->last_screen_frame_ms >= PLACEHOLDER_FIRST_MS) {
                client->placeholder_frames++;
                if (send_placeholder_frame(client, screen_sender_ntp()) < 0) break;
            }
            struct pollfd pfd = {client->encoder.fd, POLLIN, 0};
            int ready = poll(&pfd,1,50);
            if (ready<0 && errno!=EINTR) break;
        }
        /* Partial access units never become frames during stop/recovery. */
        (void)carplay_encoder_stop(&client->encoder);
        reset_encoder_picture(client);
        if (atomic_load(&client->live_screen_stop)) break;
        if (!carplay_video_restart_allowed(&restart_budget, video_monotonic_ms())) {
            iap2_log("airplay: repeated encoder failures; escalating to session reconnect");
            atomic_store(&client->video_failure, 1);
            break;
        }
        iap2_log("airplay: restarting encoder locally attempt=%u/2", restart_budget.restarts);
    }
    free(client->video_batch);
    client->video_batch=NULL;
    client->video_batch_capacity=0;
    free(nal_buffer);
    return NULL;
}

int airplay_client_start_live_screen(struct airplay_client *client)
{
    if (!client) return -1;
    if (client->live_screen_started) return 0;
    if (!client->record_accepted || client->screen_data_fd<0) return -1;
    client->live_screen_attempted=1;
    atomic_store(&client->live_screen_stop,0);
    atomic_store(&client->video_failure,0);
    client->has_real_frame=0;
    client->placeholder_frames=0;
    client->still_refreshes=0;
    reset_encoder_picture(client);
    /* Negotiation already verified the display. Encoder startup reports and
     * retries a service failure; a logging-only WM query delays every stream. */
    int error = pthread_create(&client->encoder_thread, NULL, encoder_main, client);
    if (error != 0) {
        client->live_screen_attempted=0;
        atomic_store(&client->video_failure, 1);
        iap2_log("airplay: unable to start live video worker: %s", strerror(error));
        return -1;
    }
    client->live_screen_started=1;
    iap2_log("airplay: live MediaCodec capture enabled");
    return 0;
}

int airplay_client_record_accepted(const struct airplay_client *client)
{
	return client ? client->record_accepted : 0;
}

int airplay_client_audio_stream_count(const struct airplay_client *client)
{
	return client && client->audio_bridge ?
		carplay_audio_bridge_active_count(client->audio_bridge) : 0;
}

static uint64_t screen_sender_ntp(void)
{
	uint64_t ntp = 0;
	(void)airplay_timing_now(&ntp);
	return ntp;
}

/*
 * Push one placeholder picture. The fixture is far larger than the old 290-byte
 * flat frame, so the message buffer is sized from the fixture instead of a fixed
 * 4 KiB array. Returns <0 only on a real transport failure.
 */
static int send_placeholder_frame(struct airplay_client *client,
				  uint64_t sender_ntp)
{
	size_t capacity;
	size_t message_len = 0;
	uint8_t *message;
	int rc;

	if (!client || client->screen_data_fd < 0 || !client->test_screen_sent)
		return -1;
	capacity = AIRPLAY_SCREEN_HEADER_BYTES + airplay_screen_test_frame_len +
		   AIRPLAY_CHACHA_TAG_BYTES;
	message = malloc(capacity);
	if (!message)
		return -1;
	pthread_mutex_lock(&client->video_send_mutex);
	if (atomic_load(&client->screen_suspended) || atomic_load(&client->screen_needs_keyframe)) {
		pthread_mutex_unlock(&client->video_send_mutex);
		free(message);
		return 0;
	}
	/* The fixture is an IDR picture. Its header must say so even though the
	 * synthetic path is short-lived and the real MediaCodec stream follows. */
	rc = encode_video_frame(client,
		message, capacity, &message_len,
		airplay_screen_test_frame, airplay_screen_test_frame_len,
		sender_ntp, 1);
	if (rc == 0)
		rc = send_all(client->screen_data_fd, message, message_len);
	pthread_mutex_unlock(&client->video_send_mutex);
	free(message);
	if (rc == 0)
		client->last_screen_frame_ms = video_monotonic_ms();
	return rc;
}

int airplay_client_send_test_screen(struct airplay_client *client)
{
	size_t capacity;
	uint8_t *message;
	uint64_t sender_ntp;
	size_t message_len = 0;
	int rc = 0;

	if (!client)
		return -1;
	if (client->test_screen_sent)
		return 0;
	if (!client->record_accepted || client->screen_data_fd < 0)
		return -1;
	capacity = AIRPLAY_SCREEN_HEADER_BYTES + airplay_screen_test_frame_len +
		   AIRPLAY_CHACHA_TAG_BYTES;
	if (airplay_screen_test_config_len > airplay_screen_test_frame_len)
		capacity = AIRPLAY_SCREEN_HEADER_BYTES +
			   airplay_screen_test_config_len;
	message = malloc(capacity);
	if (!message)
		return -1;
	sender_ntp = screen_sender_ntp();
	unsigned display_width = client->input_width;
	unsigned display_height = client->input_height;
	(void)carplay_capabilities_output_size(&client->capabilities,
		client->input_width, client->input_height,
		&display_width, &display_height);
	pthread_mutex_lock(&client->video_send_mutex);
	if (atomic_load(&client->screen_suspended) ||
	    atomic_load(&client->screen_needs_keyframe)) {
		pthread_mutex_unlock(&client->video_send_mutex);
		free(message);
		return 1;
	}
	if (encode_video_config(client,
		    message, capacity, &message_len,
		    airplay_screen_test_config, airplay_screen_test_config_len,
		    sender_ntp, 800, 480, display_width,
		    display_height) < 0 ||
	    send_all(client->screen_data_fd, message, message_len) < 0) {
		pthread_mutex_unlock(&client->video_send_mutex);
		free(message);
		return -1;
	}
	pthread_mutex_unlock(&client->video_send_mutex);
	free(message);
	usleep(150000);
	client->test_screen_sent = 1;
	for (unsigned frame = 0; frame < 3; frame++) {
		uint64_t frame_ntp = sender_ntp +
			((uint64_t)(frame + 1) << 32) / UINT64_C(10);
		if (send_placeholder_frame(client, frame_ntp) < 0) {
			rc = -1;
			break;
		}
		if (frame + 1 < 3)
			usleep(100000);
	}
	iap2_log("airplay: sent test H.264 config=%zu frame=%zu frames=3 encrypted=1 result=%d",
		 airplay_screen_test_config_len, airplay_screen_test_frame_len, rc);
	return rc;
}

/* Poll-thread resource transition; video sender shares the video mutex. */
static int apply_mode_state(struct airplay_client *client,
	const struct carplay_modes *next)
{
	int was_screen = carplay_modes_screen_active(&client->modes);
	int screen_active = carplay_modes_screen_active(next);
	pthread_mutex_lock(&client->video_send_mutex);
	if (!was_screen && screen_active) {
		client->screen_config_pending = 1;
		atomic_store(&client->screen_needs_keyframe, 1);
		atomic_store(&client->force_key_frame, 1);
		/* Publish the reset before clearing suspension. The worker samples
		 * the suspension level first, then this persistent generation. */
		atomic_fetch_add(&client->screen_resume_generation, 1);
	}
	atomic_store(&client->screen_suspended, !screen_active);
	pthread_mutex_unlock(&client->video_send_mutex);
	if (was_screen && !screen_active) {
		if (client->touch_direct_active)
			(void)airplay_uinput_touch_up(client->touch_input, client->touch_last_x,
				client->touch_last_y, client->input_width, client->input_height);
		client->touch_active = client->touch_direct_active = 0;
	}
	carplay_keys_suspend(client->key_input, !screen_active);
	carplay_audio_bridge_set_paused(client->audio_bridge, !carplay_modes_audio_active(next));
	client->modes = *next;
	/* Existing negotiated ports can start without a control-channel request.
	 * A deferred audio-only SETUP is sent after the event response below. */
	if (carplay_modes_audio_active(next) && client->audio_host[0] &&
	    client->audio_port_count &&
	    airplay_client_start_audio(client, client->audio_host) < 0 && client->failed)
		return -1;
	return 0;
}

/* Called on the poll thread, which is also the only resource-state writer. */
static int apply_modes_command(struct airplay_client *client, const uint8_t *body,
	size_t len, uint8_t *reply, size_t capacity, size_t *reply_len, int *accepted)
{
	struct airplay_bplist_document doc;
	struct airplay_bplist_value root, type;
	struct carplay_modes next = client->modes;
	int result;
	const char *rejection = NULL;
	*accepted = 0;
	if (airplay_bplist_init(&doc, body, len) < 0 ||
	    airplay_bplist_root(&doc, &root) < 0 ||
	    airplay_bplist_dict_get(&doc, &root, "type", &type) != 1 ||
	    !airplay_bplist_string_equals(&doc, &type, "changeModes"))
		return 0;
	result = carplay_modes_apply_with_reason(&next, body, len, &rejection);
	if (airplay_bplist_encode_modes_response(reply, capacity, reply_len,
			&next, result == 0 ? 0 : 1) < 0)
		return -1;
	if (result < 0) {
		iap2_log("airplay: changeModes rejected reason=%s bytes=%zu screen=%d/%d borrowed=%d audio=%d/%d borrowed=%d; resource state unchanged",
			rejection ? rejection : "unspecified", len,
			client->modes.screen_entity, client->modes.screen_permanent_entity,
			client->modes.screen_borrowed, client->modes.audio_entity,
			client->modes.audio_permanent_entity, client->modes.audio_borrowed);
		return 1;
	}
	if (apply_mode_state(client, &next) < 0)
		return -1;
	*accepted = 1;
	return 1;
}

static void log_event_buffer_preview(const uint8_t *data, size_t len)
{
	char hex[3 * 96 + 1];
	char ascii[96 + 1];
	size_t shown = len < 96 ? len : 96;
	size_t pos = 0;

	if (!data || !len)
		return;
	for (size_t i = 0; i < shown; i++) {
		int written = snprintf(hex + pos, sizeof(hex) - pos, "%02x", data[i]);
		if (written < 0 || (size_t)written >= sizeof(hex) - pos)
			break;
		pos += (size_t)written;
		ascii[i] = data[i] >= 0x20 && data[i] <= 0x7e ? (char)data[i] : '.';
	}
	ascii[shown] = '\0';
	iap2_log("airplay: event buffer preview bytes=%zu shown=%zu hex=%s ascii=%s",
		 len, shown, hex, ascii);
}

static int process_event_plaintext(
	struct airplay_client *client, char *failure, size_t failure_capacity)
{
	for (;;) {
		struct airplay_event_request request;
		char parse_error[AIRPLAY_EVENT_ERROR_CAPACITY] = { 0 };
		uint8_t *body;
		size_t total = 0;
		unsigned cseq = 0;
		unsigned content_length = 0;
		char response[256];
		uint8_t reply[2048];
		size_t reply_len = 0;
		int mode_command = 0, mode_accepted = 0;
		int response_len;
		int parsed;

		parsed = airplay_event_parse_request(
			client->event_response, client->event_response_len,
			&request, &total, parse_error, sizeof(parse_error));
		if (parsed == AIRPLAY_EVENT_PARSE_INCOMPLETE)
			return 0;
		if (parsed == AIRPLAY_EVENT_PARSE_INVALID) {
			if (failure && failure_capacity)
				snprintf(failure, failure_capacity,
					 "legacy event parse failed: %s", parse_error);
			iap2_log("airplay: event parse failed reason=%s buffered=%zu",
				 parse_error[0] ? parse_error : "invalid request",
				 client->event_response_len);
			log_event_buffer_preview(client->event_response,
						 client->event_response_len);
			return -1;
		}

		body = client->event_response + request.body_offset;
		content_length = (unsigned)request.content_length;
		if (request.cseq_present) {
			cseq = request.cseq;
			iap2_log("airplay: event rx %s %s cseq=%u body=%u",
				 request.method, request.path, cseq, content_length);
		} else {
			iap2_log("airplay: event rx %s %s cseq=absent body=%u",
				 request.method, request.path, content_length);
		}
		if (strncmp(request.method, "RTSP/", strlen("RTSP/")) == 0 ||
		    strncmp(request.method, "HTTP/", strlen("HTTP/")) == 0) {
			/* Receiver-side responses need no acknowledgement. */
		} else {
			mode_command = apply_modes_command(client, body, content_length,
				reply, sizeof(reply), &reply_len, &mode_accepted);
			if (mode_command < 0) {
				if (failure && failure_capacity)
					snprintf(failure, failure_capacity,
						 "event changeModes processing failed");
				return -1;
			}
			if (!mode_command)
				handle_control_command(client, body, content_length);
			if (request.cseq_present) {
				response_len = snprintf(response, sizeof(response),
					"RTSP/1.0 200 OK\r\n"
					"CSeq: %u\r\n"
					"Content-Type: application/x-apple-binary-plist\r\n"
					"Content-Length: %zu\r\n"
					"\r\n", cseq, reply_len);
			} else {
				response_len = snprintf(response, sizeof(response),
					"RTSP/1.0 200 OK\r\n"
					"Content-Type: application/x-apple-binary-plist\r\n"
					"Content-Length: %zu\r\n"
					"\r\n", reply_len);
			}
			pthread_mutex_lock(&client->event_send_mutex);
			uint64_t started = video_monotonic_ms();
			response_len = response_len < 0 ||
			    (size_t)response_len >= sizeof(response) ? -1 :
			    send_event_frames(client,
				    response, (size_t)response_len, started);
			if (response_len == 0 && reply_len)
				response_len = send_event_frames(client, reply, reply_len, started);
			pthread_mutex_unlock(&client->event_send_mutex);
			if (response_len < 0) {
				if (failure && failure_capacity)
					snprintf(failure, failure_capacity,
						 "event response send failed");
				return -1;
			}
			/* Only announce after the matching changeModes response. */
			if (mode_accepted && send_modes_changed(client) < 0) {
				if (failure && failure_capacity)
					snprintf(failure, failure_capacity,
						 "event modesChanged send failed");
				return -1;
			}
			/* Untake reports None first. Our local NiceToHave policy then
			 * acquires only unowned resources, never an accessory owner. */
			if (mode_accepted) {
				struct carplay_modes reclaimed = client->modes;
				int changed = carplay_modes_reclaim_unowned(&reclaimed);
				if (changed < 0)
					return -1;
				if (changed) {
					iap2_log("airplay: reclaim unowned resources mask=%d screen=%d->%d audio=%d->%d",
						changed, client->modes.screen_entity, reclaimed.screen_entity,
						client->modes.audio_entity, reclaimed.audio_entity);
					if (apply_mode_state(client, &reclaimed) < 0 ||
					    send_modes_changed(client) < 0)
						return -1;
				}
			}
			if (mode_accepted && client->audio_host[0] &&
			    carplay_modes_audio_active(&client->modes) &&
			    airplay_client_start_audio(client, client->audio_host) < 0 && client->failed)
				return -1;
		}
		if (client->event_response_len > total) {
			memmove(client->event_response,
				client->event_response + total,
				client->event_response_len - total);
			client->event_response_len -= total;
		} else {
			client->event_response_len = 0;
			return 0;
		}
	}
}
void airplay_client_feed_event(struct airplay_client *client,
			       const uint8_t *data, size_t len)
{
	if (!client || !data || !len || client->event_fd < 0)
		return;
	if (client->profile == AIRPLAY_PROFILE_LEGACY) {
		char failure[192] = "legacy event command processing failed";

		if (len > sizeof(client->event_response) - client->event_response_len) {
			fail_session(client, "legacy event receive buffer overflow");
			return;
		}
		memcpy(client->event_response + client->event_response_len, data, len);
		client->event_response_len += len;
		if (process_event_plaintext(client, failure, sizeof(failure)) < 0)
			fail_session(client, failure);
		return;
	}
	if (len > sizeof(client->event_wire_response) -
			  client->event_wire_response_len) {
		iap2_log("airplay: event wire buffer overflow");
		fail_session(client, "event wire buffer overflow");
		client->event_wire_response_len = 0;
		return;
	}
	memcpy(client->event_wire_response + client->event_wire_response_len,
	       data, len);
	client->event_wire_response_len += len;

	for (;;) {
		size_t payload_len;
		size_t frame_len;
		size_t plaintext_len = 0;
		uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];

		if (client->event_wire_response_len < 2)
			return;
		payload_len =
			(size_t)client->event_wire_response[0] |
			((size_t)client->event_wire_response[1] << 8);
		if (payload_len > AIRPLAY_CONTROL_MAX_PAYLOAD) {
			iap2_log("airplay: event frame payload=%zu exceeds limit",
				 payload_len);
			fail_session(client, "invalid encrypted event frame length");
			client->event_wire_response_len = 0;
			return;
		}
		frame_len = 2 + payload_len + AIRPLAY_CHACHA_TAG_BYTES;
		if (client->event_wire_response_len < frame_len)
			return;
		if (payload_len > sizeof(client->event_response) -
				  client->event_response_len) {
			iap2_log("airplay: event plaintext buffer overflow");
			fail_session(client, "event plaintext buffer overflow");
			client->event_response_len = 0;
			client->event_wire_response_len = 0;
			return;
		}
		nonce64(client->event_read_counter, nonce);
		if (airplay_chacha20_poly1305_open(
			    client->event_read_key, nonce,
			    client->event_wire_response + 2,
			    payload_len + AIRPLAY_CHACHA_TAG_BYTES,
			    client->event_wire_response, 2,
			    client->event_response + client->event_response_len,
			    sizeof(client->event_response) -
				    client->event_response_len,
			    &plaintext_len) < 0 ||
		    plaintext_len != payload_len) {
			iap2_log("airplay: event frame decrypt failed counter=%llu",
				 (unsigned long long)client->event_read_counter);
			fail_session(client, "event frame authentication failed");
			client->event_response_len = 0;
			client->event_wire_response_len = 0;
			return;
		}
		client->event_response_len += plaintext_len;
		client->event_read_counter++;
		memmove(client->event_wire_response,
			client->event_wire_response + frame_len,
			client->event_wire_response_len - frame_len);
		client->event_wire_response_len -= frame_len;
		char failure[192] = "event plaintext processing failed";
		if (process_event_plaintext(client, failure, sizeof(failure)) < 0) {
			iap2_log("airplay: event plaintext processing failed: %s", failure);
			fail_session(client, failure);
			client->event_response_len = 0;
			client->event_wire_response_len = 0;
			return;
		}
	}
}

int airplay_client_start(struct airplay_client *client)
{
	if (!client || client->info_requested || client->setup_sent)
		return -1;
	if (client->profile == AIRPLAY_PROFILE_LEGACY)
		return send_setup(client);
	client->info_requested = 1;
	return send_request(client, "GET", "/info", NULL, 1, NULL, 0);
}

static int append_response_plaintext(struct airplay_client *client,
					      const uint8_t *data, size_t len)
{
	if (len > sizeof(client->response) - client->response_len)
		return -1;
	memcpy(client->response + client->response_len, data, len);
	client->response_len += len;
	return 0;
}

static int decrypt_control_input(struct airplay_client *client,
				 const uint8_t *data, size_t len)
{
	if (len > sizeof(client->wire_response) - client->wire_response_len)
		return -1;
	memcpy(client->wire_response + client->wire_response_len, data, len);
	client->wire_response_len += len;

	for (;;) {
		size_t payload_len;
		size_t frame_len;
		size_t plaintext_len = 0;
		uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES];

		if (client->wire_response_len < 2)
			return 0;
		payload_len = (size_t)client->wire_response[0] |
			      ((size_t)client->wire_response[1] << 8);
		if (payload_len > AIRPLAY_CONTROL_MAX_PAYLOAD)
			return -1;
		frame_len = 2 + payload_len + AIRPLAY_CHACHA_TAG_BYTES;
		if (client->wire_response_len < frame_len)
			return 0;
		if (payload_len >
		    sizeof(client->response) - client->response_len)
			return -1;
		nonce64(client->control_read_counter, nonce);
		if (airplay_chacha20_poly1305_open(
			    client->control_read_key, nonce,
			    client->wire_response + 2,
			    payload_len + AIRPLAY_CHACHA_TAG_BYTES,
			    client->wire_response, 2,
			    client->response + client->response_len,
			    sizeof(client->response) - client->response_len,
			    &plaintext_len) < 0 ||
		    plaintext_len != payload_len)
			return -1;
		client->response_len += plaintext_len;
		client->control_read_counter++;
		memmove(client->wire_response,
			client->wire_response + frame_len,
			client->wire_response_len - frame_len);
		client->wire_response_len -= frame_len;
	}
}

static void fail_session(struct airplay_client *client, const char *reason)
{
	if (!client->failed) {
		snprintf(client->failure, sizeof(client->failure), "%s", reason);
		client->failed = 1;
		iap2_log("airplay: session aborted: %s", reason);
	}
}

void airplay_client_feed(struct airplay_client *client,
			 const uint8_t *data, size_t len)
{
	if (!client || !data || !len || client->failed)
		return;
	if (client->control_channel_ready) {
		if (decrypt_control_input(client, data, len) < 0) {
			fail_session(client, "control-channel decrypt/framing failed");
			client->wire_response_len = 0;
			return;
		}
	} else if (append_response_plaintext(client, data, len) < 0) {
		fail_session(client, "RTSP response buffer overflow");
		client->response_len = 0;
		return;
	}

	for (;;) {
		uint8_t *head;
		size_t header_end;
		size_t header_len;
		unsigned status = 0;
		unsigned cseq = 0;
		unsigned content_length = 0;
		size_t total;
		const uint8_t *body;

		head = (uint8_t *)find_header_end(client->response,
						 client->response_len,
						 &header_end);
		if (!head) {
			if (client->response_len == sizeof(client->response))
				fail_session(client, "RTSP response header exceeds buffer");
			return;
		}
		header_len = (size_t)(head - client->response);
		if (sscanf((const char *)client->response, "RTSP/%*u.%*u %u",
			   &status) != 1) {
			fail_session(client, "invalid RTSP response status line");
			client->response_len = 0;
			return;
		}
		if (header_uint(client->response, header_len,
				"Content-Length", &content_length) < 0 ||
		    header_uint(client->response, header_len, "CSeq", &cseq) != 1 ||
		    !cseq || content_length > sizeof(client->response) - header_len) {
			fail_session(client, "invalid RTSP response headers or body length");
			client->response_len = 0;
			return;
		}
		total = header_len + content_length;
		if (client->response_len < total)
			return;
		body = client->response + (size_t)(head - client->response);
		iap2_log("airplay: RTSP response status=%u cseq=%u body=%u",
			 status, cseq, content_length);
		if (process_response(client, status, cseq, body,
				     content_length) < 0) {
			char reason[128];
			snprintf(reason, sizeof(reason),
				 "RTSP response processing failed (status=%u cseq=%u)",
				 status, cseq);
			fail_session(client, reason);
			client->response_len = 0;
			return;
		}
		if (client->response_len > total) {
			memmove(client->response, client->response + total,
				client->response_len - total);
			client->response_len -= total;
		} else {
			client->response_len = 0;
			return;
		}
	}
}

