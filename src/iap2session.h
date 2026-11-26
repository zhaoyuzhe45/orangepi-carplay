#ifndef IAP2SESSION_H
#define IAP2SESSION_H

#include "iap2link.h"
#include "carplay_now_playing.h"
#include "carplay_calls.h"
#include "carplay_route.h"
#include "carplay_file.h"

#include <stddef.h>
#include <stdint.h>

#define IAP2_CSM_MAX_FRAME 65535u
#define IAP2_CSM_HEADER_BYTES 6u
#define IAP2_PARAM_HEADER_BYTES 4u
#define IAP2_MESSAGE_BITMAP_BYTES 8192u

/* An absent list is UNKNOWN; a present zero-length list explicitly excludes
 * every message. Exact 16-bit-ID bitmaps have a fixed 16 KiB storage cost. */
enum iap2_capability { IAP2_CAP_UNKNOWN = -1, IAP2_CAP_EXCLUDED = 0,
	IAP2_CAP_DECLARED = 1 };

/* Identification progress. For maxRetransmissions=0 peers, the application
 * timers re-send unanswered stage frames. Reliable peers own their link-layer
 * retransmission; identification acceptance (0x1D02) is still hand-sent only
 * once so a MaxOutstandingPackets=1 window can drain for 0xAA00. */
enum iap2_session_stage {
	IAP2_SESSION_STAGE_LINK = 0,	   /* waiting for link NORMAL */
	IAP2_SESSION_STAGE_IDENT_START,	   /* 0x1D00 sent, waiting 0x1D01 */
	IAP2_SESSION_STAGE_IDENT_INFO,	   /* 0x1D02 accepted, waiting 0xAA01 */
	IAP2_SESSION_STAGE_MFI_CHALLENGE,  /* 0xAA02 sent, waiting 0xAA03 */
	IAP2_SESSION_STAGE_SESSION_WAIT,   /* AA05, then 4300 if allowed */
	IAP2_SESSION_STAGE_READY,	   /* 4301 accepted or peer uses discovery */
};

#define IAP2_SESSION_RETRY_MS       2000u
#define IAP2_SESSION_RETRY_LIMIT    3u
#define IAP2_SESSION_SLOW_RETRY_MS 30000u
#define IAP2_SESSION_WINDOW_POLL_MS   20u

struct carplay_vehicle_state;

struct iap2_session {
	uint8_t rx[IAP2_CSM_MAX_FRAME];
	size_t rx_len;

	/* stage machine */
	int stage;
	unsigned stage_attempts;
	uint64_t stage_next_ms;
	int stage_backoff_reported;

	int identification_started;
	int identification_accepted;
	int identification_accept_queued;
	int identification_window_wait_reported;
	int mfi_requested;
	int mfi_challenge_sent;
	int mfi_challenge_generated;
	uint8_t mfi_challenge_length;
	int authenticated;
	int authentication_reported;
	uint8_t authentication_sequence;
	int authentication_link_ready;
	/* Round evidence survives LINK reset; init clears it for the next USB peer. */
	int authentication_completed_once;
	int availability_sent;
	int received_power;
	int power_exchange_seen;
	int power_update_pending;
	unsigned power_update_step;
	unsigned power_update_fields;
	uint64_t power_update_next_ms;
	int received_now_playing_subscription;
	struct carplay_now_subscription now_subscription;
	uint64_t now_next_ms;
	size_t now_sent_len;
	uint8_t now_sent[CARPLAY_NOW_MAX];
	int now_provider_status;
	struct carplay_file art_transfer;
	uint32_t art_generation,art_target_generation,art_size;
	size_t art_loaded;
	int art_loading,art_announce,art_cancel;
	unsigned art_attempts;
	uint8_t art_next_id;
	uint64_t art_deadline,art_retry_ms;
	int received_route_subscription;
	struct carplay_route_components route_components;
	struct carplay_route_subscription route_subscription;
	uint64_t route_next_ms;
	int route_provider_status;
	struct { size_t update_len,maneuver_len;uint8_t update[1200],maneuver[320]; }
	    route_sent[CARPLAY_ROUTE_COMPONENTS];
	int received_communications_subscription;
	uint32_t communications_fields;
	uint64_t communications_next_ms;
	size_t communications_sent_len;
	uint8_t communications_sent[64];
	int communications_provider_status;
	int received_call_subscription;
	uint32_t call_fields;
	uint64_t call_next_ms;
	int call_provider_status;
	size_t call_sent_count;
	struct {
	    struct carplay_call call;
	    size_t len;
	    uint8_t body[CARPLAY_CALL_WIRE_MAX];
	} call_sent[CARPLAY_CALLS_MAX];
	int received_carplay_start_session;
	int location_subscription_started;
	int location_subscription_stopped;
	uint64_t location_received_ms, location_retry_ms;
	unsigned message_lists_present;
	uint8_t accessory_sends[IAP2_MESSAGE_BITMAP_BYTES];
	uint8_t accessory_receives[IAP2_MESSAGE_BITMAP_BYTES];
	char device_name[96];
	char device_language[32];
	unsigned notifications_sent;
	unsigned service_pending;
	unsigned service_attempts;
	uint64_t service_next_ms;
	uint16_t bluetooth_component;
	int library_information_started;
	int library_updates_started;
	int bluetooth_updates_started;
	int time_sync_last_result;
	char time_sync_detail[128];
	uint64_t generation;
	struct carplay_vehicle_state *vehicle_state;
	uint8_t challenge[32];
	uint32_t rx_frames;
	uint32_t tx_frames;
	uint32_t malformed_frames;
};

void iap2_session_init(struct iap2_session *s);
void iap2_session_set_vehicle_state(struct iap2_session *s,
                                    struct carplay_vehicle_state *state,
                                    uint64_t generation);
void iap2_session_send_location_stop(struct iap2_session *s,
                                     struct iap2_link *l,
                                     uint64_t now_ms);
void iap2_session_tick(struct iap2_session *s, struct iap2_link *l,
		       uint64_t now_ms);
/* Current-session AA05 has left the reliable window (or peer disabled reliable
 * tracking). This is link delivery, not proof of application-level acceptance. */
int iap2_session_authentication_ready(struct iap2_session *s,
                                    const struct iap2_link *l);
void iap2_session_feed(struct iap2_session *s, struct iap2_link *l,
		       const uint8_t *data, size_t len, uint64_t now_ms);

/* Current identification stage and its next retransmission deadline (0 when no
 * retransmission is scheduled). Exposed for tests and diagnostics. */
int iap2_session_stage(const struct iap2_session *s);
uint64_t iap2_session_stage_deadline(const struct iap2_session *s);
enum iap2_capability iap2_session_accessory_receives(
	const struct iap2_session *s, uint16_t message_id);
enum iap2_capability iap2_session_accessory_sends(
	const struct iap2_session *s, uint16_t message_id);

/* Optional genuine device metadata supplied before identification completes.
 * init also reads Android product model and configured language properties.
 * NULL/empty means unavailable and never produces a fabricated notification. */
void iap2_session_set_device_identity(struct iap2_session *s,
	const char *name, const char *language);

#endif /* IAP2SESSION_H */
