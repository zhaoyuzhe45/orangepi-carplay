/*
 * usbmux.h -- Apple USBMUX framing plus the minimal TCP relay CarPlay uses.
 *
 * Frame header, 16 bytes big-endian:
 *   u32 protocol   0 = VERSION, 2 = SETUP, 6 = TCP
 *   u32 length     total frame length including this header
 *   u32 word8      0xfeedface on frames we send; the peer only checks length
 *   u16 sequence
 *   u16 acknowledgement
 *
 * Handshake (from DiPlay's Iap2UsbMuxHost):
 *   host -> device : VERSION, 20 bytes, word8 = 2
 *   device -> host : the same 20-byte frame
 *   host -> device : SETUP, payload 0x07
 *   host -> device : TCP SYN to port 62078; the iAP2 byte stream then travels
 *                    inside TCP payloads (20-byte header, flags 0x18 = ACK|PSH).
 */
#ifndef USBMUX_H
#define USBMUX_H

#include <stddef.h>
#include <stdint.h>

#define USBMUX_PROTO_VERSION 0
#define USBMUX_PROTO_SETUP   2
#define USBMUX_PROTO_TCP     6

#define USBMUX_VERSION       2u
#define USBMUX_MAGIC         0xfeedfaceu
#define USBMUX_HEADER_BYTES  16
#define USBMUX_VERSION_BYTES 20
#define USBMUX_LOCAL_PORT    62078
#define USBMUX_RX_BYTES      65536u

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_HEADER_BYTES 20

enum usbmux_state {
	USBMUX_WAIT_VERSION = 0,
	USBMUX_WAIT_SETUP,
	USBMUX_READY,
	USBMUX_TCP_UP,
};

struct usbmux {
	int state;
	uint32_t our_seq;      /* next sequence number we will send */
	uint32_t peer_seq;     /* next sequence number we expect from the peer */
	uint16_t peer_port;       /* the host's source port for the TCP relay */
	uint16_t destination_port; /* service port selected by the host */
	uint32_t connection_id;  /* increments for every SYN */

	uint8_t tx[16384];
	size_t tx_len;

	uint8_t rx[USBMUX_RX_BYTES];
	size_t rx_len;

	uint8_t in[8192];      /* iAP2 stream carried by the TCP payloads */
	size_t in_len;

	uint32_t frames_in;
	uint32_t frames_out;
	uint32_t log_segments_in;
};

void usbmux_init(struct usbmux *m);
void usbmux_endpoint_reconfigured(struct usbmux *m);
void usbmux_feed(struct usbmux *m, const uint8_t *buf, size_t len);
size_t usbmux_take_in(struct usbmux *m, uint8_t *dst, size_t cap);
int usbmux_connected(const struct usbmux *m);
void usbmux_send(struct usbmux *m, const uint8_t *data, size_t len);
size_t usbmux_take(struct usbmux *m, uint8_t *dst, size_t cap);

#endif /* USBMUX_H */


