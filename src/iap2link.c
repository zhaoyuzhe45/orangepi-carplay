/*
 * iap2link.c -- iAP2 link layer (LSP), wire-compatible with DiPlay's
 *               Iap2LinkEngine.kt.
 *
 * Sequence rules used by the reference implementation:
 * - sequence 99 is the initial control sequence on both sides;
 * - SYNCHRONIZE and control ACKs retain sent_seq;
 * - session data advances sent_seq only when the frame is queued;
 * - acknowledgement carries the last sequence received in order.
 */
#include "iap2link.h"
#include "carplay_log_sampling.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const uint8_t IAP2_MARKER[6] = { 0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10 };

static uint8_t cs(const uint8_t *p, size_t n)
{
	uint8_t sum = 0;
	size_t i;

	for (i = 0; i < n; i++)
		sum = (uint8_t)(sum + p[i]);
	return (uint8_t)(0u - sum);
}

static int next_sequence(int sequence)
{
	return (sequence + 1) & 0xFF;
}

static int emit_frame(struct iap2_link *l, uint8_t control, int seq, int ack,
		      int session, const uint8_t *payload, size_t plen)
{
	uint8_t hdr[IAP2_HEADER_BYTES];
	uint8_t tail;
	size_t total = IAP2_HEADER_BYTES +
		       (payload ? plen + IAP2_CHECKSUM_BYTES : 0);

	if (total > 0xFFFFu || total > sizeof(l->out) - l->out_len) {
		iap2_log("link: refusing to queue %zu-byte frame (pending=%zu)",
			 total, l->out_len);
		errno = total > 0xFFFFu ? EMSGSIZE : EAGAIN;
		return -1;
	}

	hdr[0] = 0xFF;
	hdr[1] = 0x5A;
	hdr[2] = (uint8_t)((total >> 8) & 0xFF);
	hdr[3] = (uint8_t)(total & 0xFF);
	hdr[4] = control;
	hdr[5] = (uint8_t)(seq & 0xFF);
	hdr[6] = (uint8_t)(ack & 0xFF);
	hdr[7] = (uint8_t)(session & 0xFF);
	hdr[8] = cs(hdr, 8);

	memcpy(l->out + l->out_len, hdr, sizeof(hdr));
	l->out_len += sizeof(hdr);
	if (payload && plen) {
		memcpy(l->out + l->out_len, payload, plen);
		l->out_len += plen;
		tail = cs(payload, plen);
		l->out[l->out_len++] = tail;
	}
	l->tx_frames++;

	if (carplay_log_should_sample(l->tx_frames))
		iap2_log("link TX ctl=0x%02x seq=%d ack=%d sess=%d len=%zu%s queued=1",
			 control, seq & 0xFF, ack & 0xFF, session & 0xFF, total,
			 payload ? " +payload" : "");
	return 0;
}

static size_t build_sync_response(uint8_t *p, const uint8_t *request, size_t len)
{
	if (!request || len < 10 || len > IAP2_SYNC_PAYLOAD_BYTES)
		return 0;
	memcpy(p, request, len);
	/* These two fields describe what this endpoint can receive. Preserve the
	 * peer's timing and session table, including the session IDs themselves. */
	p[1] = 127;
	p[2] = 0xFF;
	p[3] = 0xFF;
	return len;
}

static int emit_sync_ack(struct iap2_link *l)
{
	return emit_frame(l, IAP2_CTRL_SYNCHRONIZE | IAP2_CTRL_ACK,
			  l->sent_seq, l->last_recv, 0, l->peer_sync_payload,
			  l->peer_sync_payload_len);
}

static void receive_ack_queued(struct iap2_link *l)
{
	l->last_sent_ack = l->last_recv;
	l->ack_pending = 0;
	l->pending_ack_count = 0;
	l->next_ack_ms = 0;
}

static void emit_ack(struct iap2_link *l)
{
	if (emit_frame(l, IAP2_CTRL_ACK, l->sent_seq, l->last_recv,
		       0, NULL, 0) < 0) {
		/* Zero deadline means retry on the next tick after output drains. */
		l->ack_pending = 1;
		l->next_ack_ms = 0;
		return;
	}
	receive_ack_queued(l);
}

static void link_dead(struct iap2_link *l)
{
	l->state = IAP2_DEAD;
	l->next_sync_ms = 0;
	l->next_ack_ms = 0;
	l->ack_pending = 0;
	l->pending_ack_count = 0;
	l->pending_count = 0;
	l->pending_bytes = 0;
	l->out_len = 0;
	l->peer_file_session=0;l->file_controls_len=0;
}

static uint64_t retransmission_delay(const struct iap2_link *l)
{
	/* A zero timeout must not turn one timer tick into an unbounded loop. */
	return l->peer_retransmission_timeout_ms > 0 ?
		(uint64_t)l->peer_retransmission_timeout_ms : 1;
}

static void accept_acknowledgement(struct iap2_link *l, int ack)
{
	unsigned distance;
	unsigned removed;
	unsigned i;
	size_t bytes;

	if (!l->pending_count)
		return;
	/* Pending sequences are contiguous, including 255 -> 0. Only an ACK
	 * inside this live (<128 packet) range can retire bytes. Older, duplicate
	 * and future acknowledgements cannot move the window. */
	distance = (unsigned)((ack - l->pending[0].sequence) & 0xFF);
	if (distance >= l->pending_count)
		return;
	removed = distance + 1;
	bytes = l->pending[distance].offset + l->pending[distance].length;
	memmove(l->pending_payload, l->pending_payload + bytes,
		l->pending_bytes - bytes);
	l->pending_bytes -= bytes;
	l->pending_count -= removed;
	memmove(l->pending, l->pending + removed,
		l->pending_count * sizeof(l->pending[0]));
	for (i = 0; i < l->pending_count; i++)
		l->pending[i].offset -= (uint32_t)bytes;
}

static void retransmit_due(struct iap2_link *l, uint64_t now_ms)
{
	unsigned i;

	if (l->state != IAP2_NORMAL || l->peer_max_retransmissions <= 0)
		return;
	for (i = 0; i < l->pending_count; i++) {
		struct iap2_pending_packet *packet = &l->pending[i];

		if (now_ms < packet->deadline_ms)
			continue;
		if (packet->retransmissions >= (unsigned)l->peer_max_retransmissions) {
			iap2_log("link: data seq=%u unacknowledged after %u retries",
				 packet->sequence, packet->retransmissions);
			link_dead(l);
			return;
		}
		if (emit_frame(l, IAP2_CTRL_ACK, packet->sequence, l->last_recv,
			       packet->session, l->pending_payload + packet->offset,
			       packet->length) < 0)
			return; /* Keep payload, due time and retry budget intact. */
		packet->retransmissions++;
		packet->deadline_ms = now_ms + retransmission_delay(l);
		receive_ack_queued(l);
	}
}

static void handle_extended_ack(struct iap2_link *l, const uint8_t *missing,
				size_t count, uint64_t now_ms)
{
	unsigned i;
	size_t j;

	/* DiPlay encodes the missing sequence numbers as a byte list. Ignore
	 * numbers outside the pending range and replay each matching packet once. */
	for (i = 0; i < l->pending_count; i++) {
		for (j = 0; j < count; j++) {
			if (l->pending[i].sequence == missing[j]) {
				l->pending[i].deadline_ms = now_ms;
				break;
			}
		}
	}
	retransmit_due(l, now_ms);
}

void iap2_link_init(struct iap2_link *l, uint64_t now_ms)
{
	(void)now_ms;
	memset(l, 0, sizeof(*l));
	l->state = IAP2_IDLE;
	l->sent_seq = IAP2_INITIAL_SEQUENCE;
	l->peer_control_session = IAP2_SESSION_CONTROL;
	l->last_sent_ack = 0;
	l->last_recv = 0;
}

void iap2_link_start(struct iap2_link *l, uint64_t now_ms)
{
	if (l->state != IAP2_IDLE)
		return;

	/* The head unit is the iAP2 link initiator here.  Wait for its detect
	 * marker and SYN; sending our own SYN first makes the car respond as if we
	 * were the accessory side and leaves both endpoints waiting for the other
	 * to complete a different handshake. */
	l->state = IAP2_DETECTING;
	l->next_sync_ms = 0;
	iap2_log("link: waiting for peer marker/SYN (sent_seq=%d)", l->sent_seq);
	(void)now_ms;
}

void iap2_link_tick(struct iap2_link *l, uint64_t now_ms)
{
	if (l->state == IAP2_NEGOTIATING && l->peer_sync_received &&
	    (!l->next_sync_ms || now_ms >= l->next_sync_ms)) {
		emit_sync_ack(l);
		l->next_sync_ms = now_ms + IAP2_SYNC_RESEND_MS;
		iap2_log("link: SYNCHRONIZE|ACK retransmitted");
	}

	if ((l->state == IAP2_NORMAL || l->state == IAP2_NEGOTIATING) &&
	    l->ack_pending && (!l->next_ack_ms || now_ms >= l->next_ack_ms))
		emit_ack(l);
	retransmit_due(l, now_ms);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int append_control(struct iap2_link *l, const uint8_t *payload,
			  size_t len)
{
	if (len == 0)
		return 0;
	if (len > sizeof(l->control_rx) - l->control_rx_len) {
		l->control_dropped++;
		iap2_log("link: CSM receive buffer full, dropping %zu bytes",
			 len);
		return -1;
	}
	memcpy(l->control_rx + l->control_rx_len, payload, len);
	l->control_rx_len += len;
	return 0;
}

static void handle_synchronization(struct iap2_link *l,
				   const uint8_t *payload, size_t len, int seq,
				   uint64_t now_ms)
{
	size_t off;

	if (l->state != IAP2_DETECTING && l->state != IAP2_NEGOTIATING)
		return;
	if (len < 10 || payload[0] != 1 ||
	    rd16(payload + 2) <= IAP2_HEADER_BYTES + IAP2_CHECKSUM_BYTES) {
		iap2_log("link: invalid SYNCHRONIZE version or length");
		return;
	}

	l->peer_max_outgoing = payload[1];
	l->peer_max_length = rd16(payload + 2);
	l->peer_retransmission_timeout_ms = rd16(payload + 4);
	l->peer_acknowledgement_timeout_ms = rd16(payload + 6);
	l->peer_max_retransmissions = payload[8];
	l->peer_max_acknowledgements = payload[9];
	l->peer_sync_received = 1;
	l->last_recv = seq;
	if (len > sizeof(l->peer_sync_payload)) {
		iap2_log("link: SYNCHRONIZE payload too large (%zu bytes)", len);
		return;
	}
	if (!build_sync_response(l->peer_sync_payload, payload, len)) {
		iap2_log("link: cannot build SYNCHRONIZE response");
		return;
	}
	l->peer_sync_payload_len = (uint16_t)len;

	iap2_log("link: peer SYNCHRONIZE maxOutgoing=%d maxLength=%d "
		 "retrans=%dms ackTimeout=%dms maxRetrans=%d maxAcks=%d",
		 l->peer_max_outgoing, l->peer_max_length,
		 l->peer_retransmission_timeout_ms,
		 l->peer_acknowledgement_timeout_ms,
		 l->peer_max_retransmissions, l->peer_max_acknowledgements);

	unsigned control_session = 0, ea_session = 0;
	unsigned file_count = 0;
	uint8_t seen[256] = {0};
	int duplicate = 0, invalid = 0;
	for (off = 10; off + 3 <= len; off += 3) {
		iap2_log("link:   peer session id=%u kind=%u version=%u",
			 payload[off], payload[off + 1], payload[off + 2]);
		if (seen[payload[off]]++)
			duplicate = 1;
		if (payload[off + 1] == 0)
			control_session = payload[off];
		else if (payload[off + 1] == 1) {
			file_count++;
			l->peer_file_session = payload[off];
			if (!payload[off] || payload[off + 2] != 1)
				invalid = 1;
		}
		else if (payload[off + 1] == 2)
			ea_session = payload[off];
	}
	if (off != len || duplicate || invalid || !control_session || file_count > 1) {
		l->peer_file_session = 0;
		iap2_log("link: invalid SYNCHRONIZE session table");
	}
	else {
		l->peer_control_session = (int)control_session;
		l->peer_file_session = file_count == 1 ? l->peer_file_session : 0;
		l->peer_ea_session = (uint8_t)ea_session;
	}

	/* Device-side negotiation: the car originates SYN and this endpoint
	 * answers with SYN|ACK.  Use its session IDs for every later CSM frame. */
	l->state = IAP2_NEGOTIATING;
	l->next_ack_ms = 0;
	l->ack_pending = 0;
	if (emit_sync_ack(l) < 0) {
		l->next_sync_ms = now_ms;
		return;
	}
	l->next_sync_ms = now_ms + IAP2_SYNC_RESEND_MS;
	iap2_log("link: sent SYNCHRONIZE|ACK seq=%d ack=%d", l->sent_seq, l->last_recv);
}

static void dump_hex(const char *tag, const uint8_t *p, size_t n)
{
	char line[3 * 32 + 1];
	size_t i;
	int off = 0;

	for (i = 0; i < n && i < 32; i++)
		off += snprintf(line + off, sizeof(line) - (size_t)off, "%02x ",
				p[i]);
	iap2_log("%s %zu bytes: %s", tag, n, line);
}

static void process_frame(struct iap2_link *l, const uint8_t *frame,
			  size_t len, uint64_t now_ms)
{
	uint8_t control = frame[4];
	int seq = frame[5];
	int ack = frame[6];
	int session = frame[7];
	const uint8_t *payload = NULL;
	size_t plen = 0;

	l->rx_frames++;
	if (len > IAP2_HEADER_BYTES) {
		payload = frame + IAP2_HEADER_BYTES;
		plen = len - IAP2_HEADER_BYTES - IAP2_CHECKSUM_BYTES;
	}

	if (carplay_log_should_sample(l->rx_frames))
		iap2_log("link RX ctl=0x%02x seq=%d ack=%d sess=%d payload=%zu",
			 control, seq, ack, session, plen);

	if (control & IAP2_CTRL_RESET) {
		iap2_log("link: peer sent RESET, link dead");
		link_dead(l);
		return;
	}

	if (control & IAP2_CTRL_SYNCHRONIZE)
		handle_synchronization(l, payload, plen, seq, now_ms);

	if (control & IAP2_CTRL_ACK) {
		/* The completing ACK must acknowledge our SYN|ACK sequence. */
		if (l->state == IAP2_NEGOTIATING && l->peer_sync_received) {
			if (ack != l->sent_seq) {
				iap2_log("link: completing ACK has wrong ack=%d expected=%d",
					 ack, l->sent_seq);
			} else {
				l->last_recv = seq;
				l->state = IAP2_NORMAL;
				l->next_sync_ms = 0;
				iap2_log("link: peer acknowledged, state NORMAL");
			}
		}
		if (l->state == IAP2_NORMAL)
			accept_acknowledgement(l, ack);

		/* SYN/extended-ACK payloads are link metadata, never session data. */
		if (control == IAP2_CTRL_ACK && payload && plen) {
			int distance = (seq - l->last_recv) & 0xFF;
			if (distance == 1) {
				if (session == l->peer_control_session) {
					if (carplay_log_should_sample(l->rx_frames))
						dump_hex("link: CSM payload", payload, plen);
					if (append_control(l, payload, plen) < 0) {
						/* Do not acknowledge bytes we could not deliver. */
						emit_ack(l);
						return;
					}
				} else if(l->peer_file_session && session==l->peer_file_session && plen==2) {
					if(l->file_controls_len+2>sizeof(l->file_controls)){emit_ack(l);return;}
					memcpy(l->file_controls+l->file_controls_len,payload,2);l->file_controls_len+=2;
				} else if (carplay_log_should_sample(l->rx_frames)) {
					dump_hex("link: session payload", payload,
						 plen);
				}
				l->last_recv = seq;
				l->pending_ack_count++;
			} else if (distance != 0) {
				iap2_log("link: out-of-order data seq=%d expected=%d",
					 seq, next_sequence(l->last_recv));
			}

			/* Diplay advertises a zero receiver ACK window, but still retains
			 * outbound packets when the accessory advertises retransmission.
			 * ACK completion explicitly so the zero-window peer cannot spin
			 * through the same CSM sequence until its retransmit limit. */
			if (l->peer_max_acknowledgements <= 0 || distance != 1 ||
			    l->peer_acknowledgement_timeout_ms <= 0 ||
			    l->pending_ack_count >= (unsigned)l->peer_max_acknowledgements ||
			    (l->peer_max_outgoing > 0 &&
			     l->pending_ack_count >= (unsigned)l->peer_max_outgoing)) {
				emit_ack(l);
			} else if (!l->ack_pending) {
				/* Bound the wait from the first unacknowledged packet. */
				l->ack_pending = 1;
				l->next_ack_ms = now_ms +
					(uint64_t)l->peer_acknowledgement_timeout_ms;
			}
		}
	}

	if ((control & IAP2_CTRL_EXT_ACK) && payload && plen)
		handle_extended_ack(l, payload, plen, now_ms);
}

void iap2_link_feed(struct iap2_link *l, const uint8_t *buf, size_t len,
		    uint64_t now_ms)
{
	if (l->state == IAP2_DEAD)
		return;
	if (len > sizeof(l->rx) - l->rx_len) {
		iap2_log("link: rx overflow, discarding %zu bytes", len);
		l->rx_len = 0;
		return;
	}
	memcpy(l->rx + l->rx_len, buf, len);
	l->rx_len += len;

	while (l->rx_len > 0) {
		size_t i = 0;

		if (l->state == IAP2_DETECTING) {
			if (l->rx_len >= 2 && l->rx[0] == 0xFF && l->rx[1] == 0x5A)
				; /* Some peers send SYN without the six-byte detect marker. */
			else {
				if (l->rx_len < sizeof(IAP2_MARKER))
					return;
				if (memcmp(l->rx, IAP2_MARKER, sizeof(IAP2_MARKER)) != 0) {
					memmove(l->rx, l->rx + 1, l->rx_len - 1);
					l->rx_len--;
					continue;
				}
				memmove(l->rx, l->rx + sizeof(IAP2_MARKER),
					l->rx_len - sizeof(IAP2_MARKER));
				l->rx_len -= sizeof(IAP2_MARKER);
				if (l->out_len + sizeof(IAP2_MARKER) > sizeof(l->out)) {
					iap2_log("link: output buffer full, cannot echo marker");
					return;
				}
				memcpy(l->out + l->out_len, IAP2_MARKER, sizeof(IAP2_MARKER));
				l->out_len += sizeof(IAP2_MARKER);
				iap2_log("link: peer marker received and echoed");
				continue;
			}
		}

		while (i + 1 < l->rx_len &&
		       !(l->rx[i] == 0xFF && l->rx[i + 1] == 0x5A))
			i++;
		if (i > 0) {
			memmove(l->rx, l->rx + i, l->rx_len - i);
			l->rx_len -= i;
		}
		if (l->rx_len < IAP2_HEADER_BYTES)
			return;

		{
			size_t total = rd16(l->rx + 2);
			/* A corrupted length must not hold later frames hostage while
			 * we wait for its (untrusted) body. */
			if (total < IAP2_HEADER_BYTES ||
			    cs(l->rx, IAP2_HEADER_BYTES) != 0) {
				memmove(l->rx, l->rx + 1, l->rx_len - 1);
				l->rx_len--;
				continue;
			}
			if (l->rx_len < total)
				return;
			if (total > IAP2_HEADER_BYTES &&
			     cs(l->rx + IAP2_HEADER_BYTES,
				total - IAP2_HEADER_BYTES) != 0) {
				iap2_log("link: bad frame checksum, resyncing");
				memmove(l->rx, l->rx + 1, l->rx_len - 1);
				l->rx_len--;
				continue;
			}
			process_frame(l, l->rx, total, now_ms);
			if (l->state == IAP2_DEAD) {
				l->rx_len = 0;
				return;
			}
			memmove(l->rx, l->rx + total, l->rx_len - total);
			l->rx_len -= total;
		}
	}
}

size_t iap2_link_take(struct iap2_link *l, uint8_t *dst, size_t cap)
{
	size_t n = l->out_len < cap ? l->out_len : cap;

	if (n == 0)
		return 0;
	memcpy(dst, l->out, n);
	if (n < l->out_len)
		memmove(l->out, l->out + n, l->out_len - n);
	l->out_len -= n;
	return n;
}

size_t iap2_link_take_control(struct iap2_link *l, uint8_t *dst, size_t cap)
{
	size_t n = l->control_rx_len < cap ? l->control_rx_len : cap;

	if (n == 0)
		return 0;
	memcpy(dst, l->control_rx, n);
	if (n < l->control_rx_len)
		memmove(l->control_rx, l->control_rx + n,
			l->control_rx_len - n);
	l->control_rx_len -= n;
	return n;
}

static int send_session(struct iap2_link *l, const uint8_t *csm,
			   size_t len, uint64_t now_ms,uint8_t session)
{
	if (l->state != IAP2_NORMAL) {
		iap2_log("link: cannot send CSM before NORMAL (state=%d)",
			 l->state);
		errno = ENOTCONN;
		return -1;
	}
	if (!csm || len == 0 ||
	    l->peer_max_length <= IAP2_HEADER_BYTES + IAP2_CHECKSUM_BYTES ||
	    len > 0xFFFFu - IAP2_HEADER_BYTES - IAP2_CHECKSUM_BYTES ||
	    len > (size_t)l->peer_max_length -
	    IAP2_HEADER_BYTES - IAP2_CHECKSUM_BYTES) {
		iap2_log("link: invalid CSM payload length %zu", len);
		errno = EINVAL;
		return -1;
	}
	if (l->peer_max_retransmissions > 0 &&
	    (l->peer_max_outgoing <= 0 ||
	     l->pending_count >= (unsigned)l->peer_max_outgoing ||
	     l->pending_count >= IAP2_MAX_PENDING_PACKETS ||
	     len > sizeof(l->pending_payload) - l->pending_bytes)) {
		errno = EAGAIN;
		return -1;
	}
	if (emit_frame(l, IAP2_CTRL_ACK, next_sequence(l->sent_seq), l->last_recv,
		       session, csm, len) < 0)
		return -1;
	l->sent_seq = next_sequence(l->sent_seq);
	if (l->peer_max_retransmissions > 0) {
		struct iap2_pending_packet *packet = &l->pending[l->pending_count++];

		packet->sequence = (uint8_t)l->sent_seq;
		packet->session = session;
		packet->offset = (uint32_t)l->pending_bytes;
		packet->length = (uint16_t)len;
		packet->retransmissions = 0;
		packet->deadline_ms = now_ms + retransmission_delay(l);
		memcpy(l->pending_payload + l->pending_bytes, csm, len);
		l->pending_bytes += len;
	}
	/* Successfully queued data piggybacks the receive acknowledgement. */
	receive_ack_queued(l);
	return 0;
}

int iap2_link_send_control(struct iap2_link *l,const uint8_t *data,size_t len,uint64_t now_ms){
    return send_session(l,data,len,now_ms,(uint8_t)l->peer_control_session);
}
int iap2_link_send_file(struct iap2_link *l,const uint8_t *data,size_t len,uint64_t now_ms){
    if(!l || !l->peer_file_session){errno=ENOTSUP;return -1;}
    return send_session(l,data,len,now_ms,l->peer_file_session);
}
int iap2_link_take_file_control(struct iap2_link *l,uint8_t out[2]){
    if(!l || !out || l->file_controls_len<2)return 0;
    memcpy(out,l->file_controls,2);l->file_controls_len-=2;
    memmove(l->file_controls,l->file_controls+2,l->file_controls_len);return 1;
}
