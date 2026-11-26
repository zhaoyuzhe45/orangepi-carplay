/*
 * iap2link.h -- iAP2 link layer (LSP) for the CarPlay box side.
 *
 * Wire format, taken from DiPlay's Iap2LinkEngine.kt:
 *   marker : FF 55 02 00 EE 10
 *   frame  : FF 5A | length(u16 BE) | control | sequence | acknowledgement
 *            | sessionId | checksum(header) | payload | checksum(payload)
 * The control session ID comes from the car's SYN payload; field-car sessions
 * are not assumed to be 10/11/12. It carries CSM chunks assembled by
 * iap2session.c.
 */
#ifndef IAP2LINK_H
#define IAP2LINK_H

#include <stddef.h>
#include <stdint.h>

#define IAP2_CTRL_SYNCHRONIZE 0x80
#define IAP2_CTRL_ACK         0x40
#define IAP2_CTRL_EXT_ACK     0x20
#define IAP2_CTRL_RESET       0x10

#define IAP2_HEADER_BYTES      9
#define IAP2_CHECKSUM_BYTES    1
#define IAP2_SESSION_CONTROL   10
#define IAP2_SESSION_EA        11
#define IAP2_SESSION_FILE      12

#define IAP2_INITIAL_SEQUENCE   99
#define IAP2_SYNC_RESEND_MS      500
#define IAP2_LINK_BUFFER_BYTES 65536u
#define IAP2_MAX_PENDING_PACKETS 64u
#define IAP2_PENDING_PAYLOAD_BYTES 65536u
#define IAP2_SYNC_PAYLOAD_BYTES 1024u

/* Keep the live sequence window below half the 8-bit sequence space. Payloads
 * share one fixed pool rather than allocating a maximum-size buffer per slot. */
struct iap2_pending_packet {
	uint64_t deadline_ms;
	uint32_t offset;
	uint16_t length;
	uint16_t retransmissions;
	uint8_t sequence;
	uint8_t session;
};

enum iap2_link_state {
	IAP2_IDLE = 0,
	IAP2_DETECTING,
	IAP2_NEGOTIATING,
	IAP2_NORMAL,
	IAP2_DEAD,
};

struct iap2_link {
	int state;
	int sent_seq;
	int last_recv;
	int last_sent_ack;
	int peer_sync_received;
	int peer_control_session;
	uint8_t peer_file_session;
	uint8_t peer_ea_session;
	uint8_t peer_sync_payload[IAP2_SYNC_PAYLOAD_BYTES];
	uint16_t peer_sync_payload_len;
	uint8_t file_controls[64];
	size_t file_controls_len;
	int peer_max_outgoing;
	int peer_max_length;
	unsigned int usb_out_max_packet; /* Zero outside native USB host mode. */
	int peer_retransmission_timeout_ms;
	int peer_acknowledgement_timeout_ms;
	int peer_max_retransmissions;
	int peer_max_acknowledgements;
	int ack_pending;
	unsigned pending_ack_count;

	uint64_t next_sync_ms;
	uint64_t next_ack_ms;

	/* Reliable sends retain their original sequence and payload until a valid
	 * cumulative ACK. New sends fail with EAGAIN when either bound is full. */
	struct iap2_pending_packet pending[IAP2_MAX_PENDING_PACKETS];
	unsigned pending_count;
	size_t pending_bytes;
	uint8_t pending_payload[IAP2_PENDING_PAYLOAD_BYTES];

	/* pending wire bytes to push to the USB IN endpoint */
	uint8_t out[IAP2_LINK_BUFFER_BYTES];
	size_t out_len;

	/* inbound LSP reassembly */
	uint8_t rx[IAP2_LINK_BUFFER_BYTES];
	size_t rx_len;

	/* complete control-session payloads waiting for the CSM framer */
	uint8_t control_rx[IAP2_LINK_BUFFER_BYTES];
	size_t control_rx_len;

	uint32_t tx_frames;
	uint32_t rx_frames;
	uint32_t control_dropped;
};

extern void iap2_log(const char *fmt, ...);

void iap2_link_init(struct iap2_link *l, uint64_t now_ms);
void iap2_link_start(struct iap2_link *l, uint64_t now_ms);
void iap2_link_tick(struct iap2_link *l, uint64_t now_ms);
void iap2_link_feed(struct iap2_link *l, const uint8_t *buf, size_t len,
		    uint64_t now_ms);
size_t iap2_link_take(struct iap2_link *l, uint8_t *dst, size_t cap);
size_t iap2_link_take_control(struct iap2_link *l, uint8_t *dst, size_t cap);
/* Returns 0 after queueing, or -1 without consuming a sequence. EAGAIN means
 * the output buffer or reliable-send window/payload pool must drain first.
 * Reliable packets are retried at most peer_max_retransmissions times; the
 * following unanswered deadline marks the link DEAD. Zero disables tracking. */
int iap2_link_send_control(struct iap2_link *l, const uint8_t *csm,
			   size_t len, uint64_t now_ms);
int iap2_link_send_file(struct iap2_link *l,const uint8_t *data,size_t len,uint64_t now_ms);
int iap2_link_take_file_control(struct iap2_link *l,uint8_t out[2]);

#endif /* IAP2LINK_H */
