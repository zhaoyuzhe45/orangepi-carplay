/*
 * usbmux.c -- USBMUX framing and the TCP relay that carries CarPlay's iAP2
 *             stream between the box (device) and the head unit (host).
 */
#include "usbmux.h"
#include "carplay_log_sampling.h"

#include <stdio.h>
#include <string.h>

extern void iap2_log(const char *fmt, ...);

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void tx_append(struct usbmux *m, const uint8_t *p, size_t n)
{
	if (m->tx_len + n > sizeof(m->tx)) {
		iap2_log("usbmux: tx buffer full, dropping %zu bytes", n);
		return;
	}
	memcpy(m->tx + m->tx_len, p, n);
	m->tx_len += n;
}

/* seq/ack are the mux-layer counters; the peer only trusts the length field. */
static void send_frame_word8(struct usbmux *m, uint32_t protocol, uint32_t word8,
			     const uint8_t *payload, size_t len)
{
	uint8_t hdr[USBMUX_HEADER_BYTES];
	size_t total = USBMUX_HEADER_BYTES + len;

	put32(hdr, protocol);
	put32(hdr + 4, (uint32_t)total);
	put32(hdr + 8, word8);
	put16(hdr + 12, (uint16_t)(m->frames_out & 0xffff));
	put16(hdr + 14, (uint16_t)(m->frames_in & 0xffff));
	tx_append(m, hdr, sizeof(hdr));
	if (payload && len)
		tx_append(m, payload, len);
	m->frames_out++;
}

/* Build one TCP segment carrying payload (may be empty for a pure ACK). */
static void send_tcp(struct usbmux *m, uint8_t flags, const uint8_t *payload,
		     size_t len)
{
	uint8_t seg[TCP_HEADER_BYTES + 4096];

	if (len > sizeof(seg) - TCP_HEADER_BYTES) {
		iap2_log("usbmux: tcp payload %zu too large", len);
		return;
	}
	memset(seg, 0, TCP_HEADER_BYTES);
	put16(seg, USBMUX_LOCAL_PORT);
	put16(seg + 2, m->peer_port);
	put32(seg + 4, m->our_seq);
	put32(seg + 8, m->peer_seq);
	seg[12] = (uint8_t)((TCP_HEADER_BYTES / 4) << 4);
	seg[13] = flags;
	put16(seg + 14, 512);
	if (payload && len)
		memcpy(seg + TCP_HEADER_BYTES, payload, len);

	send_frame_word8(m, USBMUX_PROTO_TCP, USBMUX_MAGIC, seg,
			TCP_HEADER_BYTES + len);
	m->our_seq += (uint32_t)len;
	if (flags & TCP_FLAG_SYN)
		m->our_seq += 1;
	if (flags & TCP_FLAG_FIN)
		m->our_seq += 1;
}

void usbmux_init(struct usbmux *m)
{
	memset(m, 0, sizeof(*m));
	m->state = USBMUX_WAIT_VERSION;
	m->our_seq = 1000;
	m->peer_seq = 0;
	m->peer_port = 0;
}

int usbmux_connected(const struct usbmux *m)
{
	return m->state == USBMUX_TCP_UP;
}

/* FunctionFS can close and re-enable endpoint requests when the host reselects
 * its configuration. Bytes left from the old endpoint are incomplete, while
 * the logical usbmux/TCP handshake remains valid across that endpoint reset. */
void usbmux_endpoint_reconfigured(struct usbmux *m)
{
	if (!m)
		return;
	m->rx_len = 0;
}

static void handle_version(struct usbmux *m, const uint8_t *frame, size_t len)
{
	/* The reply is a 20-byte frame in which the header's own word8 field is
	 * the protocol version: total 16 (header) + 4 (zero payload). */
	uint8_t payload[4];

	(void)frame;
	memset(payload, 0, sizeof(payload));
	send_frame_word8(m, USBMUX_PROTO_VERSION, USBMUX_VERSION, payload,
			 sizeof(payload));
	iap2_log("usbmux: VERSION received (%zu bytes), replied 20-byte "
		 "version %u frame", len, USBMUX_VERSION);
	m->state = USBMUX_WAIT_SETUP;
}

static void handle_setup(struct usbmux *m, const uint8_t *payload, size_t len)
{
	iap2_log("usbmux: SETUP received, %zu payload byte(s): %02x", len,
		 len ? payload[0] : 0);
	m->state = USBMUX_READY;
}

static void handle_tcp(struct usbmux *m, const uint8_t *seg, size_t len)
{
	uint16_t src, dst;
	uint32_t seq, ack;
	uint8_t flags;
	size_t hdr_len, data_len;
	const uint8_t *data;

	if (len < TCP_HEADER_BYTES) {
		iap2_log("usbmux: short TCP segment (%zu)", len);
		return;
	}
	src = be16(seg);
	dst = be16(seg + 2);
	seq = be32(seg + 4);
	ack = be32(seg + 8);
	flags = seg[13];
	hdr_len = (size_t)(seg[12] >> 4) * 4;
	if (hdr_len < TCP_HEADER_BYTES || hdr_len > len)
		hdr_len = TCP_HEADER_BYTES;
	data = seg + hdr_len;
	data_len = len - hdr_len;

	int sample = carplay_log_should_sample(++m->log_segments_in);
	if (sample)
		iap2_log("usbmux TCP src=%u dst=%u seq=%u ack=%u flags=0x%02x data=%zu",
			 src, dst, seq, ack, flags, data_len);

	if (flags & TCP_FLAG_RST) {
		iap2_log("usbmux: peer reset the TCP relay");
		m->state = USBMUX_READY;
		return;
	}

	if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
		m->peer_port = src;
		m->destination_port = dst;
		m->our_seq = 1000;
		m->in_len = 0;
		m->connection_id++;
		m->peer_seq = seq + 1;
		iap2_log("usbmux: SYN from port %u, replying SYN|ACK (our_seq=%u)",
			 src, m->our_seq);
		send_tcp(m, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
		m->state = USBMUX_TCP_UP;
		return;
	}

	if (m->state != USBMUX_TCP_UP) {
		iap2_log("usbmux: TCP data before the relay was established");
		return;
	}

	if (data_len) {
		char hex[3 * 48 + 4];
		size_t k;
		int off = 0;

		for (k = 0; k < data_len && k < 48; k++)
			off += snprintf(hex + off, sizeof(hex) - (size_t)off,
					"%02x ", data[k]);
		if (sample)
			iap2_log("usbmux: %zu bytes of TCP payload: %s", data_len, hex);

		if (m->in_len + data_len > sizeof(m->in)) {
			iap2_log("usbmux: link input overflow, dropping %zu", data_len);
		} else {
			memcpy(m->in + m->in_len, data, data_len);
			m->in_len += data_len;
		}
		m->peer_seq = seq + (uint32_t)data_len;
		send_tcp(m, TCP_FLAG_ACK, NULL, 0);
	}

	if (flags & TCP_FLAG_FIN) {
		m->peer_seq = seq + (uint32_t)data_len + 1;
		send_tcp(m, TCP_FLAG_ACK, NULL, 0);
		iap2_log("usbmux: peer closed the TCP relay");
		m->state = USBMUX_READY;
	}
}

void usbmux_feed(struct usbmux *m, const uint8_t *buf, size_t len)
{
	if (len > sizeof(m->rx) - m->rx_len) {
		iap2_log("usbmux: rx overflow, dropping %zu bytes", len);
		m->rx_len = 0;
		return;
	}
	memcpy(m->rx + m->rx_len, buf, len);
	m->rx_len += len;

	while (m->rx_len >= USBMUX_HEADER_BYTES) {
		uint32_t protocol = be32(m->rx);
		uint32_t total = be32(m->rx + 4);

		if (total < USBMUX_HEADER_BYTES || total > 65536) {
			iap2_log("usbmux: bad frame length %u, resyncing", total);
			memmove(m->rx, m->rx + 1, m->rx_len - 1);
			m->rx_len--;
			continue;
		}
		if (m->rx_len < total)
			return;

		switch (protocol) {
		case USBMUX_PROTO_VERSION:
			handle_version(m, m->rx, total);
			break;
		case USBMUX_PROTO_SETUP:
			handle_setup(m, m->rx + USBMUX_HEADER_BYTES,
				     total - USBMUX_HEADER_BYTES);
			break;
		case USBMUX_PROTO_TCP:
			handle_tcp(m, m->rx + USBMUX_HEADER_BYTES,
				   total - USBMUX_HEADER_BYTES);
			break;
		default:
			iap2_log("usbmux: unknown protocol %u (%u bytes)",
				 protocol, total);
			break;
		}

		m->frames_in++;
		memmove(m->rx, m->rx + total, m->rx_len - total);
		m->rx_len -= total;
	}
}

size_t usbmux_take_in(struct usbmux *m, uint8_t *dst, size_t cap)
{
	size_t n = m->in_len < cap ? m->in_len : cap;

	if (n == 0)
		return 0;
	memcpy(dst, m->in, n);
	if (n < m->in_len)
		memmove(m->in, m->in + n, m->in_len - n);
	m->in_len -= n;
	return n;
}

void usbmux_send(struct usbmux *m, const uint8_t *data, size_t len)
{
	if (m->state != USBMUX_TCP_UP) {
		iap2_log("usbmux: not connected, dropping %zu bytes", len);
		return;
	}
	while (len) {
		size_t chunk = len > 4096 ? 4096 : len;

		send_tcp(m, TCP_FLAG_ACK | TCP_FLAG_PSH, data, chunk);
		data += chunk;
		len -= chunk;
	}
}

size_t usbmux_take(struct usbmux *m, uint8_t *dst, size_t cap)
{
	size_t n = m->tx_len < cap ? m->tx_len : cap;

	if (n == 0)
		return 0;
	memcpy(dst, m->tx, n);
	if (n < m->tx_len)
		memmove(m->tx, m->tx + n, m->tx_len - n);
	m->tx_len -= n;
	return n;
}

