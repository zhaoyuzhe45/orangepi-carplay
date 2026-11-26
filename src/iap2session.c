/*
 * iap2session.c -- control-session exchange expected by DiPlay after LSP NORMAL.
 *
 * The box impersonates the iPhone/device side:
 *   1D00 > 1D01 > 1D02, AA00 > AA01 > AA02 > AA03 > AA05, then 4300.
 * The accessory answers 4300 with a 4301 CarPlayStartSession endpoint.
 *
 * 0x1D01 carries the accessory's IdentificationInformation. The phone replies
 * with 0x1D02 IdentificationAccepted, with no parameters. USB host/device role
 * switching does not reverse these iAP2 phone/accessory roles.
 *
 * Identification runs as a stage machine (see enum iap2_session_stage).
 * Non-reliable peers need application timers because iap2link marks control
 * frames complete immediately when maxRetransmissions=0. Reliable peers own
 * data retransmission, and a one-packet window requires waiting for 0x1D02's
 * ACK before queueing 0xAA00.
 *
 * This stage logs the MFi signature rather than validating an Apple trust
 * chain. It establishes DiPlay control without claiming FairPlay support.
 */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif

#include "iap2session.h"
#include "carplay_log_sampling.h"
#include "carplay_time_sync.h"
#include "carplay_vehicle_state.h"
#include "ncm_bridge.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define SERVICE_LIBRARY_INFO 1u
#define SERVICE_LIBRARY_UPDATE 2u
#define SERVICE_BLUETOOTH 4u
#define SERVICE_DEVICE_NAME 8u
#define SERVICE_DEVICE_LANGUAGE 16u
#define SERVICE_LOCATION_START 32u
#define SERVICE_POST_AUTH (SERVICE_DEVICE_NAME | SERVICE_DEVICE_LANGUAGE)
#define NOTIFY_NAME 1u
#define NOTIFY_LANGUAGE 2u

static const char empty_library_name[] = "Box iAP2";
static const char empty_library_uid[] = "box-empty-v1";

static void send_location_start(struct iap2_session *s, struct iap2_link *l,
				uint64_t now_ms);

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int ascii_contains_ci(const uint8_t *text, size_t len,
			     const char *needle)
{
	size_t needle_len = strlen(needle);

	if (!text || !needle_len || needle_len > len)
		return 0;
	for (size_t off = 0; off + needle_len <= len; off++) {
		size_t i;

		for (i = 0; i < needle_len; i++) {
			unsigned char a = text[off + i];
			unsigned char b = (unsigned char)needle[i];

			if (a >= 'a' && a <= 'z')
				a = (unsigned char)(a - 'a' + 'A');
			if (b >= 'a' && b <= 'z')
				b = (unsigned char)(b - 'a' + 'A');
			if (a != b)
				break;
		}
		if (i == needle_len)
			return 1;
	}
	return 0;
}

static uint64_t rd64(const uint8_t *p)
{
	uint64_t value = 0;
	int i;

	for (i = 0; i < 8; i++)
		value = (value << 8) | p[i];
	return value;
}

static void wr16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static size_t add_param(uint8_t *dst, size_t cap, uint16_t id,
			const uint8_t *payload, size_t len)
{
	size_t total = IAP2_PARAM_HEADER_BYTES + len;

	if (total > 0xFFFFu || total > cap)
		return 0;
	wr16(dst, (uint16_t)total);
	wr16(dst + 2, id);
	if (len)
		memcpy(dst + IAP2_PARAM_HEADER_BYTES, payload, len);
	return total;
}

static int first_param(const uint8_t *body, size_t body_len, uint16_t wanted,
		       const uint8_t **payload, size_t *payload_len)
{
	size_t off = 0;
	int found = 0;

	while (off + IAP2_PARAM_HEADER_BYTES <= body_len) {
		uint16_t total = rd16(body + off);
		uint16_t id = rd16(body + off + 2);
		if (total < IAP2_PARAM_HEADER_BYTES || total > body_len - off) {
			iap2_log("csm: malformed parameter length %u at %zu",
				 total, off);
			return -1;
		}
		if (id == wanted) {
			if (found)
				return -1;
			*payload = body + off + IAP2_PARAM_HEADER_BYTES;
			*payload_len = total - IAP2_PARAM_HEADER_BYTES;
			found = 1;
		}
		off += total;
	}
	return off == body_len ? (found ? 0 : 1) : -1;
}

/* Validate the accessory's TLV envelope before accepting it. Log capabilities
 * in bounded chunks so a later car test can distinguish 4300/4301 support from
 * a receiver that needs Bonjour discovery. Absence is not an empty allow-list. */
static int inspect_accessory_identification(struct iap2_session *s,
					   const uint8_t *body, size_t len)
{
	size_t off = 0;
	unsigned parameters = 0;
	int message_lists = 0;
	unsigned lists_seen = 0;

	while (off < len) {
		uint16_t total, id;
		if (len - off < IAP2_PARAM_HEADER_BYTES)
			return -1;
		total = rd16(body + off);
		id = rd16(body + off + 2);
		if (total < IAP2_PARAM_HEADER_BYTES || total > len - off ||
		    ((id == 6 || id == 7) && (total & 1u)))
			return -1;
		if (id == 6 || id == 7) {
			unsigned bit = id == 6 ? 1u : 2u;
			if (lists_seen & bit)
				return -1; /* Lists are not repeatable: avoid ambiguous union. */
			lists_seen |= bit;
		}
		off += total;
	}
	memset(s->accessory_sends, 0, sizeof(s->accessory_sends));
	memset(s->accessory_receives, 0, sizeof(s->accessory_receives));
	memset(&s->route_components,0,sizeof(s->route_components));
	if(carplay_route_identify(body,len,&s->route_components)<0)
		iap2_log("csm: invalid route display components; navigation disabled");
	s->received_route_subscription=0;
	memset(s->route_sent,0,sizeof(s->route_sent));
	iap2_log("csm: route display components=%u",s->route_components.count);
	s->message_lists_present = lists_seen;
	for (off = 0; off < len;) {
		uint16_t total = rd16(body + off);
		uint16_t id = rd16(body + off + 2);
		const uint8_t *value = body + off + IAP2_PARAM_HEADER_BYTES;
		size_t value_len = total - IAP2_PARAM_HEADER_BYTES;
		if (id == 6 || id == 7) {
			uint8_t *bits = id == 6 ? s->accessory_sends : s->accessory_receives;
			message_lists |= id == 6 ? 1 : 2;
			for (size_t i = 0; i < value_len; i += 2) {
				uint16_t message = rd16(value + i);
				bits[message >> 3] |= (uint8_t)(1u << (message & 7));
			}
		}
		if (++parameters <= 32) {
			iap2_log("csm: identity param=0x%04x bytes=%zu", id, value_len);
			if (id <= 2) {
				char text[96];
				size_t n = 0;
				while (n < value_len && n < sizeof(text) - 1 && value[n]) {
					text[n] = value[n] >= 0x20 && value[n] <= 0x7e
						? (char)value[n] : '?';
					n++;
				}
				text[n] = 0;
				iap2_log("csm: identity %s=%s", id == 0 ? "name" :
					 id == 1 ? "model" : "manufacturer", text);
				/* Audi MIB3/MMI head units reject a 32-byte
				 * MFi challenge without generating AA03. The
				 * real-device capture for this vehicle accepts
				 * the 20-byte challenge used by iPhones. Keep
				 * the protocol default for every other peer. */
				if (ascii_contains_ci(value, value_len, "AUDI") ||
				    ascii_contains_ci(value, value_len, "CLU8P") ||
				    ascii_contains_ci(value, value_len, "MIB3"))
					s->mfi_challenge_length = 20;
			}
			if (id == 6 || id == 7) {
				size_t count = value_len / 2;
				size_t logged = count < 128 ? count : 128;
				message_lists |= id == 6 ? 1 : 2;
				for (size_t i = 0; i < logged || i == 0; i += 16) {
					char ids[128] = "";
					size_t used = 0;
					for (size_t j = i; j < logged && j < i + 16; j++)
						used += (size_t)snprintf(ids + used, sizeof(ids) - used,
							"%s0x%04x", j == i ? "" : " ", rd16(value + j * 2));
					iap2_log("csm: identity Messages%s count=%zu offset=%zu: %s",
						 id == 6 ? "SentByAccessory" : "ReceivedFromDevice",
						 count, i, count ? ids : "<empty>");
				}
				if (count > logged)
					iap2_log("csm: identity message list truncated after %zu IDs", logged);
			}
		}
		off += total;
	}
	if (parameters > 32)
		iap2_log("csm: identity parameter log truncated after 32 entries");
	if (message_lists != 3)
		iap2_log("csm: identity message lists not fully reported (mask=%d); "
			 "do not infer 4300/4301 support", message_lists);
	iap2_log("csm: capability 4300 receive=%d 4301 send=%d (-1 unknown, 0 excluded, 1 declared)",
		iap2_session_accessory_receives(s, 0x4300),
		iap2_session_accessory_sends(s, 0x4301));
	return 0;
}

enum iap2_capability iap2_session_accessory_receives(
	const struct iap2_session *s, uint16_t id)
{
	if (!s || !(s->message_lists_present & 2u))
		return IAP2_CAP_UNKNOWN;
	return (s->accessory_receives[id >> 3] & (1u << (id & 7)))
		? IAP2_CAP_DECLARED : IAP2_CAP_EXCLUDED;
}

enum iap2_capability iap2_session_accessory_sends(
	const struct iap2_session *s, uint16_t id)
{
	if (!s || !(s->message_lists_present & 1u))
		return IAP2_CAP_UNKNOWN;
	return (s->accessory_sends[id >> 3] & (1u << (id & 7)))
		? IAP2_CAP_DECLARED : IAP2_CAP_EXCLUDED;
}

static int valid_params(const uint8_t *body, size_t len)
{
	size_t off = 0;
	while (off < len) {
		if (len - off < IAP2_PARAM_HEADER_BYTES)
			return 0;
		size_t total = rd16(body + off);
		if (total < IAP2_PARAM_HEADER_BYTES || total > len - off)
			return 0;
		off += total;
	}
	return 1;
}

static void log_string_param(const uint8_t *body, size_t body_len,
			     uint16_t id, const char *name)
{
	const uint8_t *payload = NULL;
	size_t payload_len = 0;
	size_t i;

	if (first_param(body, body_len, id, &payload, &payload_len) != 0) {
		iap2_log("csm: 0x4301 missing %s", name);
		return;
	}
	for (i = 0; i < payload_len; i++) {
		if (payload[i] == 0)
			break;
		if (payload[i] < 0x20 || payload[i] > 0x7E) {
			iap2_log("csm: 0x4301 %s contains non-text byte", name);
			return;
		}
	}
	if (i == payload_len) {
		iap2_log("csm: 0x4301 %s is not NUL terminated", name);
		return;
	}
	iap2_log("csm: 0x4301 %s=%.*s", name, (int)i, payload);
}

/* Copies a NUL-terminated text parameter into caller storage. Returns 0 when
 * the value was present, NUL terminated and fit. */
static int copy_string_param(const uint8_t *body, size_t body_len, uint16_t id,
			     char *out, size_t out_size)
{
	const uint8_t *payload = NULL;
	size_t payload_len = 0;
	size_t i;

	if (!out || out_size == 0)
		return -1;
	if (first_param(body, body_len, id, &payload, &payload_len) != 0)
		return -1;
	for (i = 0; i < payload_len; i++) {
		if (payload[i] == 0)
			break;
		if (payload[i] < 0x20 || payload[i] > 0x7E)
			return -1;
	}
	if (i == payload_len || i >= out_size)
		return -1;
	memcpy(out, payload, i);
	out[i] = '\0';
	return 0;
}

static int valid_ip_literal(const char *address)
{
	uint8_t binary[16];
	char literal[128];
	char *zone;
	size_t n = strlen(address);

	if (n == 0 || n >= sizeof(literal))
		return 0;
	memcpy(literal, address, n + 1);
	zone = strchr(literal, '%');
	if (zone) {
		/* The NCM connector accepts an IPv6 scope suffix. */
		if (!zone[1])
			return 0;
		*zone = '\0';
		return inet_pton(AF_INET6, literal, binary) == 1;
	}
	return inet_pton(AF_INET, literal, binary) == 1 ||
	       inet_pton(AF_INET6, literal, binary) == 1;
}

static size_t collect_wired_addresses(const uint8_t *body, size_t body_len,
					      char addresses[16][128],
					      char *pointers[16])
{
	const uint8_t *group = NULL;
	size_t group_len = 0;
	size_t off = 0;
	size_t count = 0;

	if (first_param(body, body_len, 0, &group, &group_len) != 0) {
		iap2_log("csm: 0x4301 missing wired group");
		return 0;
	}
	if (!valid_params(group, group_len))
		return 0;
	while (off + IAP2_PARAM_HEADER_BYTES <= group_len && count < 16) {
		uint16_t total = rd16(group + off);
		uint16_t id = rd16(group + off + 2);
		const uint8_t *value = group + off + IAP2_PARAM_HEADER_BYTES;
		size_t value_len;

		if (total < IAP2_PARAM_HEADER_BYTES || total > group_len - off)
			break;
		value_len = total - IAP2_PARAM_HEADER_BYTES;
		if (id == 0 && value_len > 1 && value[value_len - 1] == 0) {
			size_t copy_len = value_len - 1;

			if (copy_len < sizeof(addresses[count])) {
				memcpy(addresses[count], value, copy_len);
				addresses[count][copy_len] = '\0';
				if (valid_ip_literal(addresses[count])) {
					pointers[count] = addresses[count];
					iap2_log("csm: 0x4301 wiredIP[%zu]=%s",
						 count, addresses[count]);
					count++;
				}
			}
		}
		off += total;
	}
	return count;
}

static int connect_carplay_start(const uint8_t *body, size_t body_len)
{
	const uint8_t *port = NULL;
	size_t port_len = 0;
	char addresses[16][128];
	char *pointers[16] = { 0 };
	size_t address_count;
	uint32_t airplay_port = 0;

	address_count = collect_wired_addresses(body, body_len, addresses,
						 pointers);
	if (first_param(body, body_len, 2, &port, &port_len) == 0 &&
	    port_len == 4) {
		airplay_port = ((uint32_t)port[0] << 24) |
			       ((uint32_t)port[1] << 16) |
			       ((uint32_t)port[2] << 8) | port[3];
		iap2_log("csm: 0x4301 airPlayPort=%u", airplay_port);
	}
	log_string_param(body, body_len, 3, "deviceIdentifier");
	log_string_param(body, body_len, 4, "publicKey");
	log_string_param(body, body_len, 5, "sourceVersion");
	/* deviceIdentifier is the host's real hardware address. Prefer it over
	 * anything learned from received frames, which may still be the NCM
	 * default placeholder 02:00:00:00:00:02. */
	{
		char identifier[32];

		if (copy_string_param(body, body_len, 3, identifier,
				      sizeof(identifier)) == 0) {
			if (ncm_bridge_set_peer_mac(identifier) != 0)
				iap2_log("csm: 0x4301 deviceIdentifier '%s' is "
					 "not a usable peer MAC", identifier);
		}
	}
	if (!address_count || !airplay_port || airplay_port > UINT16_MAX) {
		iap2_log("csm: 0x4301 has no usable wired endpoint");
		return -1;
	}
	if (ncm_bridge_connect_endpoint((const char *const *)pointers,
				       address_count, (uint16_t)airplay_port) != 0) {
		iap2_log("csm: failed to queue AirPlay endpoint connect");
		return -1;
	}
	return 0;
}
static int send_simple(struct iap2_session *s, struct iap2_link *l,
		       uint16_t message_id, uint64_t now_ms)
{
	uint8_t csm[IAP2_CSM_HEADER_BYTES];

	wr16(csm, 0x4040);
	wr16(csm + 2, sizeof(csm));
	wr16(csm + 4, message_id);
	if (iap2_link_send_control(l, csm, sizeof(csm), now_ms) < 0)
		return -1;
	s->tx_frames++;
	iap2_log("csm TX message=0x%04x body=0", message_id);
	return 0;
}

static void apply_vehicle_time(struct iap2_session *s, int64_t epoch_seconds,
			       const char *source)
{
	char detail[128];
	char message[192];
	int rc;

	if (!s)
		return;
	rc = carplay_time_sync_apply_epoch(epoch_seconds, detail,
					   sizeof(detail));
	snprintf(message, sizeof(message), "%s: %s", source, detail);
	if (rc != s->time_sync_last_result ||
	    strcmp(message, s->time_sync_detail) != 0) {
		if (rc < 0)
			iap2_log("csm: car time sync failed %s", message);
		else
			iap2_log("csm: car time sync %s", message);
		s->time_sync_last_result = rc;
		snprintf(s->time_sync_detail, sizeof(s->time_sync_detail), "%s",
			 message);
	}
}
static int send_param(struct iap2_session *s, struct iap2_link *l,
		      uint16_t message_id, uint16_t parameter_id,
		      const uint8_t *payload, size_t payload_len,
		      uint64_t now_ms)
{
	uint8_t csm[IAP2_PARAM_HEADER_BYTES + 0xFFFFu];
	size_t param_len;
	size_t csm_len;

	param_len = add_param(csm + IAP2_CSM_HEADER_BYTES,
			      sizeof(csm) - IAP2_CSM_HEADER_BYTES,
			      parameter_id, payload, payload_len);
	if (!param_len)
		return -1;
	csm_len = IAP2_CSM_HEADER_BYTES + param_len;
	wr16(csm, 0x4040);
	wr16(csm + 2, (uint16_t)csm_len);
	wr16(csm + 4, message_id);
	if (iap2_link_send_control(l, csm, csm_len, now_ms) < 0)
		return -1;
	s->tx_frames++;
	iap2_log("csm TX message=0x%04x param=%u bytes=%zu",
		 message_id, parameter_id, payload_len);
	return 0;
}

static int send_body(struct iap2_session *s, struct iap2_link *l,
	uint16_t id, const uint8_t *body, size_t len, uint64_t now_ms)
{
	uint8_t frame[CARPLAY_NOW_MAX + IAP2_CSM_HEADER_BYTES];
	if (len > sizeof(frame) - IAP2_CSM_HEADER_BYTES)
		return -1;
	wr16(frame, 0x4040);
	wr16(frame + 2, (uint16_t)(len + IAP2_CSM_HEADER_BYTES));
	wr16(frame + 4, id);
	memcpy(frame + IAP2_CSM_HEADER_BYTES, body, len);
	if (iap2_link_send_control(l, frame, len + IAP2_CSM_HEADER_BYTES, now_ms) < 0)
		return -1;
	s->tx_frames++;
	iap2_log("csm TX message=0x%04x body=%zu", id, len);
	return 0;
}

static unsigned power_update_field_mask(unsigned step)
{
	switch (step) {
	case 0: return 1u << 0;
	case 1: return 1u << 1;
	case 2: return 1u << 4;
	case 3: return 1u << 5;
	case 4: return 1u << 6;
	default: return 0;
	}
}

static int send_power_update(struct iap2_session *s, struct iap2_link *l,
			     uint64_t now_ms)
{
	static const uint8_t current_ma[] = {0x03, 0xE8};
	static const uint8_t charge_level[] = {0x00, 0x62};
	static const uint8_t should_charge = 1;
	static const uint8_t external_charger = 1;
	static const uint8_t charging_state = 1;
	unsigned step;
	unsigned field;
	uint16_t id = 0;
	const uint8_t *value = NULL;
	size_t value_len = 0;
	int remaining = 0;

	if (!s->power_update_pending)
		return 0;
	if (!s->authenticated || !s->authentication_reported)
		return -1;
	/* Match the vendor transmitter: one PowerUpdate parameter per AE01 and
	 * wait for the one-packet reliable window between fields. This car requests
	 * fields 0,1,5,6; field 4 is part of the same reference sequence when asked.
	 * The values are compatibility data, not measured Android battery telemetry. */
	step = s->power_update_step;
	field = power_update_field_mask(step);
	if (!field) {
		s->power_update_pending = 0;
		s->power_update_next_ms = 0;
		return 0;
	}
	if (!(s->power_update_fields & field)) {
		s->power_update_step = step + 1;
		return send_power_update(s, l, now_ms);
	}
	switch (step) {
	case 0:
		id = 0;
		value = current_ma;
		value_len = sizeof(current_ma);
		break;
	case 1:
		id = 1;
		value = &should_charge;
		value_len = sizeof(should_charge);
		break;
	case 2:
		id = 4;
		value = &external_charger;
		value_len = sizeof(external_charger);
		break;
	case 3:
		id = 5;
		value = &charging_state;
		value_len = sizeof(charging_state);
		break;
	case 4:
		id = 6;
		value = charge_level;
		value_len = sizeof(charge_level);
		break;
	}
	if (send_param(s, l, 0xAE01, id, value, value_len, now_ms) < 0)
		return -1;
	s->power_update_step = step + 1;
	for (unsigned next = s->power_update_step; next < 5; next++) {
		if (s->power_update_fields & power_update_field_mask(next)) {
			remaining = 1;
			break;
		}
	}
	if (remaining) {
		s->power_update_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
		iap2_log("csm: PowerUpdate sent field=%u step=%u/5 mono_ms=%llu",
			 id, s->power_update_step, (unsigned long long)now_ms);
	} else {
		s->power_update_pending = 0;
		s->power_update_next_ms = 0;
		iap2_log("csm: PowerUpdate ready fields=0x%x profile=reference "
			 "current_limit=1000mA will_charge=1 external_charger=1 "
			 "state=charging level=98%% (not measured)",
			 s->power_update_fields);
	}
	return 0;
}
/* Only state owned by this daemon is represented: its empty local library
 * and no Bluetooth profiles established by this wired iAP2 implementation.
 * Android playback, phone calls and battery state have no provider here. */
static void flush_services(struct iap2_session *s, struct iap2_link *l,
			   uint64_t now_ms)
{
	uint8_t body[128], component[2];
	size_t len;
	uint32_t tx_before = s->tx_frames;
	int window_blocked = 0;
	static const uint8_t local = 0;
	if (!s->authenticated || !s->authentication_reported)
		return;
	if (s->service_pending & SERVICE_LIBRARY_INFO) {
		len = add_param(body, sizeof(body), 0,
			(const uint8_t *)empty_library_name, sizeof(empty_library_name));
		len += add_param(body + len, sizeof(body) - len, 1,
			(const uint8_t *)empty_library_uid, sizeof(empty_library_uid));
		len += add_param(body + len, sizeof(body) - len, 2, &local, 1);
		if (send_param(s, l, 0x4C01, 0, body, len, now_ms) == 0)
			s->service_pending &= ~SERVICE_LIBRARY_INFO;
	}
	if (s->service_pending & SERVICE_LIBRARY_UPDATE) {
		len = add_param(body, sizeof(body), 0,
			(const uint8_t *)empty_library_uid, sizeof(empty_library_uid));
		len += add_param(body + len, sizeof(body) - len, 1,
			(const uint8_t *)"1", 2);
		if (send_body(s, l, 0x4C04, body, len, now_ms) == 0)
			s->service_pending &= ~SERVICE_LIBRARY_UPDATE;
	}
	if (s->service_pending & SERVICE_BLUETOOTH) {
		wr16(component, s->bluetooth_component);
		len = add_param(body, sizeof(body), 0, component, sizeof(component));
		len += add_param(body + len, sizeof(body) - len, 1, NULL, 0);
		if (send_body(s, l, 0x4E04, body, len, now_ms) == 0)
			s->service_pending &= ~SERVICE_BLUETOOTH;
	}
	if (s->service_pending & SERVICE_DEVICE_NAME) {
		if (send_param(s, l, 0x4E09, 0, (const uint8_t *)s->device_name,
			strlen(s->device_name) + 1, now_ms) == 0) {
			s->service_pending &= ~SERVICE_DEVICE_NAME;
			s->notifications_sent |= NOTIFY_NAME;
		} else if (errno == EAGAIN) {
			window_blocked = 1;
		}
	}
	if (s->service_pending & SERVICE_DEVICE_LANGUAGE) {
		if (send_param(s, l, 0x4E0A, 0, (const uint8_t *)s->device_language,
			strlen(s->device_language) + 1, now_ms) == 0) {
			s->service_pending &= ~SERVICE_DEVICE_LANGUAGE;
			s->notifications_sent |= NOTIFY_LANGUAGE;
		} else if (errno == EAGAIN) {
			window_blocked = 1;
		}
	}
	if (s->service_pending & SERVICE_LOCATION_START) {
		if (!s->power_update_pending &&
		    !(l->peer_max_retransmissions > 0 && l->pending_count) &&
		    (s->power_exchange_seen || s->received_carplay_start_session ||
		     iap2_session_accessory_sends(s, 0xAE00) != IAP2_CAP_DECLARED)) {
			errno = 0;
			send_location_start(s, l, now_ms);
			if (s->location_subscription_started || !s->vehicle_state)
				s->service_pending &= ~SERVICE_LOCATION_START;
			else if (errno == EAGAIN)
				window_blocked = 1;
		} else {
			window_blocked = 1;
		}
	}
	if (s->service_pending) {
		if (window_blocked ||
		    ((s->service_pending & (SERVICE_POST_AUTH | SERVICE_LOCATION_START)) &&
		     s->tx_frames != tx_before)) {
			/* A one-packet reliable window accepts one post-auth service per
			 * ACK. Poll promptly instead of charging a full retry interval
			 * after every productive partial flush. Subscription replies keep
			 * their established retry cadence. */
			s->service_attempts = 0;
			s->service_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
		} else {
			if (s->service_attempts < IAP2_SESSION_RETRY_LIMIT)
				s->service_attempts++;
			s->service_next_ms = now_ms +
				(s->service_attempts < IAP2_SESSION_RETRY_LIMIT
				 ? IAP2_SESSION_RETRY_MS : IAP2_SESSION_SLOW_RETRY_MS);
		}
	} else {
		s->service_next_ms = 0;
		s->service_attempts = 0;
	}
}

/* Retain a blocked Cancel and stop all old data until it reaches the link. */
static int cancel_artwork(struct iap2_session *s,struct iap2_link *l,uint64_t now_ms)
{
    s->art_loading=s->art_announce=0;
    if(s->art_transfer.phase && l->peer_file_session && s->authenticated){
        uint8_t cancel[]={s->art_transfer.id,2};s->art_cancel=1;
        if(iap2_link_send_file(l,cancel,2,now_ms)<0)return -1;
    }
    s->art_transfer.phase=s->art_cancel=0;return 0;
}

static void flush_artwork(struct iap2_session *s,struct iap2_link *l,uint64_t now_ms)
{
    uint8_t reply[2];
    while(iap2_link_take_file_control(l,reply))carplay_file_control(&s->art_transfer,reply,2,now_ms);
    if(!s->authenticated || !s->authentication_reported || !s->received_now_playing_subscription ||
        !(s->now_subscription.media&(1u<<26)) || !l->peer_file_session ||
        s->now_provider_status!=1 || iap2_session_accessory_receives(s,0x5001)==IAP2_CAP_EXCLUDED){
        s->art_generation=0;(void)cancel_artwork(s,l,now_ms);return;
    }
    if(s->art_cancel){(void)cancel_artwork(s,l,now_ms);return;}
    if(s->art_loading){
        if(now_ms>=s->art_deadline){s->art_loading=0;iap2_log("csm: artwork source deadline; discarded");return;}
        if(now_ms<s->art_retry_ms)return;
        if(s->art_loaded<s->art_size){
            size_t n=0;
            if(carplay_art_read(s->art_target_generation,s->art_size,(uint32_t)s->art_loaded,
                s->art_transfer.data+s->art_loaded,CARPLAY_ART_MAX-s->art_loaded,&n)<0 ||
                !n || n>s->art_size-s->art_loaded){
                s->art_attempts++;s->art_retry_ms=now_ms+1000;
                if(s->art_attempts>=3)s->art_loading=0;
                iap2_log("csm: artwork source unavailable attempt=%u%s",s->art_attempts,
                    s->art_loading?"; bounded retry":"; discarded");return;
            }
            s->art_loaded+=n;return; /* Bound each tick to one private IPC request. */
        }
        s->art_loading=0;s->art_announce=1;
    }
    if(s->art_announce){
        if(s->art_next_id<128 || s->art_next_id==255)s->art_next_id=128;
        uint8_t body[]={0,9,0,0,0,5,0,26,s->art_next_id};
        if(send_body(s,l,0x5001,body,sizeof(body),now_ms)<0)return;
        s->art_announce=0;
        (void)carplay_file_begin(&s->art_transfer,s->art_next_id++,s->art_transfer.data,s->art_size,2,now_ms);
        iap2_log("csm: artwork file=%u bytes=%u generation=%u",s->art_transfer.id,s->art_size,s->art_target_generation);
    }
    uint8_t packet[1026];
    const size_t overhead = IAP2_HEADER_BYTES + IAP2_CHECKSUM_BYTES;
    size_t cap = sizeof(packet), len = 0;
    if (l->peer_max_length <= (int)overhead) return;
    size_t max = (size_t)l->peer_max_length - overhead;
    if (cap > max) cap = max;
    /* Keep artwork transfers short on native USB host. The car can stop
     * draining its GPS stream after a full-size bulk OUT packet. */
    if (l->usb_out_max_packet > overhead + 10 &&
        cap >= l->usb_out_max_packet - overhead)
        cap = l->usb_out_max_packet - overhead - 1;
    int rc=carplay_file_next(&s->art_transfer,packet,cap,&len,now_ms);
    if(rc<0)iap2_log("csm: artwork transfer timeout or packet limit");
    else if(rc==1 && iap2_link_send_file(l,packet,len,now_ms)==0)carplay_file_commit(&s->art_transfer,len,now_ms);
}

static void flush_now_playing(struct iap2_session *s, struct iap2_link *l,
                              uint64_t now_ms)
{
    struct carplay_now_playing state;
    uint8_t body[CARPLAY_NOW_MAX];size_t len=0;
    if(!s->received_now_playing_subscription || !s->authenticated ||
       !s->authentication_reported || now_ms<s->now_next_ms ||
       iap2_session_accessory_receives(s,0x5001)==IAP2_CAP_EXCLUDED)return;
    s->now_next_ms=now_ms+1000;
    int available=carplay_now_read(now_ms,&state)==0;
    if(s->now_provider_status!=(available?1:-1)) {
        s->now_provider_status=available?1:-1;
        iap2_log("csm: NowPlaying provider %s",available?"available":"unavailable (no fabricated update)");
    }
    size_t capacity=sizeof(body);
    if(l->peer_max_length>0) {
        size_t overhead=IAP2_HEADER_BYTES+IAP2_CHECKSUM_BYTES+IAP2_CSM_HEADER_BYTES;
        if((size_t)l->peer_max_length<=overhead)return;
        if(capacity>(size_t)l->peer_max_length-overhead)capacity=(size_t)l->peer_max_length-overhead;
    }
    if(!available || carplay_now_encode(&s->now_subscription,&state,body,capacity,&len)<0)return;
    if(len && (len!=s->now_sent_len || memcmp(s->now_sent,body,len))) {
        if(send_body(s,l,0x5001,body,len,now_ms)<0)return;
        memcpy(s->now_sent,body,len);s->now_sent_len=len;
    }
    if((s->now_subscription.media&(1u<<26)) && l->peer_file_session && state.media_generation &&
        state.media_generation!=s->art_generation){
        if(cancel_artwork(s,l,now_ms)<0)return;
        s->art_transfer.phase=0;s->art_announce=0;s->art_generation=state.media_generation;
        s->art_target_generation=state.media_generation;s->art_size=state.art_size;s->art_loaded=0;
        s->art_loading=1;s->art_deadline=now_ms+5000;
        s->art_attempts=0;s->art_retry_ms=0;
    }
}

static int service_allowed(struct iap2_session *s, uint16_t response)
{
	if (!s->authenticated || !s->authentication_reported) {
		iap2_log("csm: service 0x%04x ignored before authentication", response);
		return 0;
	}
	if (iap2_session_accessory_receives(s, response) == IAP2_CAP_EXCLUDED) {
		iap2_log("csm: service 0x%04x excluded by accessory declaration", response);
		return 0;
	}
	return 1;
}

static void flush_route(struct iap2_session *s,struct iap2_link *l,uint64_t now_ms)
{
    if(!s->received_route_subscription || !s->authenticated || !s->authentication_reported ||
        now_ms<s->route_next_ms || iap2_session_accessory_receives(s,0x5201)==IAP2_CAP_EXCLUDED)return;
    s->route_next_ms=now_ms+500;
    struct carplay_route_snapshot p={0};
    int available=carplay_route_read(now_ms,&p)==0 && p.state!=255;
    if(!available){memset(&p,0,sizeof(p));p.state=p.maneuver=255;p.index=65535;}
    if(s->route_provider_status!=(available?1:-1)){
        s->route_provider_status=available?1:-1;
        iap2_log("csm: route provider %s",available?"available":"unavailable; clearing maneuver selection");
    }
    size_t capacity=CARPLAY_ROUTE_WIRE_MAX;
    if(l->peer_max_length>0){
        size_t overhead=IAP2_HEADER_BYTES+IAP2_CHECKSUM_BYTES+IAP2_CSM_HEADER_BYTES;
        if((size_t)l->peer_max_length<=overhead)return;
        if(capacity>(size_t)l->peer_max_length-overhead)capacity=(size_t)l->peer_max_length-overhead;
    }
    if(iap2_session_accessory_receives(s,0x5202)==IAP2_CAP_EXCLUDED)p.maneuver=255;
    for(unsigned i=0;i<s->route_components.count;i++){
        const struct carplay_route_component *c=&s->route_components.item[i];unsigned j;
        for(j=0;j<s->route_subscription.count;j++)if(s->route_subscription.ids[j]==c->id)break;
        if(j==s->route_subscription.count)continue;
        uint8_t body[CARPLAY_ROUTE_WIRE_MAX];size_t len=0;
        if(p.state==1 && p.index!=65535 && p.maneuver!=255){
            size_t mcap=capacity<sizeof(s->route_sent[i].maneuver)?capacity:sizeof(s->route_sent[i].maneuver);
            if(carplay_route_maneuver(c,&p,body,mcap,&len)<0)continue;
            if(len!=s->route_sent[i].maneuver_len || memcmp(body,s->route_sent[i].maneuver,len)){
                if(send_body(s,l,0x5202,body,len,now_ms)<0)return;
                memcpy(s->route_sent[i].maneuver,body,len);s->route_sent[i].maneuver_len=len;
            }
        } else s->route_sent[i].maneuver_len=0;
        if(carplay_route_update(&s->route_subscription,c,&p,body,capacity,&len)<0)continue;
        if(len==s->route_sent[i].update_len && !memcmp(body,s->route_sent[i].update,len))continue;
        if(send_body(s,l,0x5201,body,len,now_ms)<0)return;
        memcpy(s->route_sent[i].update,body,len);s->route_sent[i].update_len=len;
    }
}

static void flush_communications(struct iap2_session *s,struct iap2_link *l,uint64_t now_ms)
{
    struct carplay_communications state;uint8_t body[64];size_t len;
    if(!s->authenticated || !s->authentication_reported || !s->received_communications_subscription ||
       now_ms<s->communications_next_ms || iap2_session_accessory_receives(s,0x4158)==IAP2_CAP_EXCLUDED)return;
    s->communications_next_ms=now_ms+1000;
    int available=carplay_comm_read(now_ms,&state)==0;
    if(s->communications_provider_status!=(available?1:-1)) {
        s->communications_provider_status=available?1:-1;
        iap2_log("csm: Android communications provider %s",available?"available":"unavailable");
    }
    if(!available || carplay_comm_encode(s->communications_fields,&state,body,sizeof(body),&len)<0 || !len)return;
    if(len==s->communications_sent_len && !memcmp(body,s->communications_sent,len))return;
    if(send_body(s,l,0x4158,body,len,now_ms)==0) {
        memcpy(s->communications_sent,body,len);s->communications_sent_len=len;
    }
}

static void flush_calls(struct iap2_session *s,struct iap2_link *l,uint64_t now_ms)
{
    struct carplay_call_list current;
    uint8_t body[CARPLAY_CALL_WIRE_MAX];size_t capacity=sizeof(body),len;
    if(!s->authenticated || !s->authentication_reported || !s->received_call_subscription ||
       now_ms<s->call_next_ms || iap2_session_accessory_receives(s,0x4155)==IAP2_CAP_EXCLUDED)return;
    s->call_next_ms=now_ms+500;
    int available=carplay_calls_read(now_ms,&current)==0;
    if(s->call_provider_status!=(available?1:-1)) {
        s->call_provider_status=available?1:-1;
        iap2_log("csm: Bluetooth call provider %s",available?"available":"unavailable; retaining last observed calls");
    }
    if(!available)return;
    if(l->peer_max_length>0) {
        size_t overhead=IAP2_HEADER_BYTES+IAP2_CHECKSUM_BYTES+IAP2_CSM_HEADER_BYTES;
        if((size_t)l->peer_max_length<=overhead)return;
        if(capacity>(size_t)l->peer_max_length-overhead)capacity=(size_t)l->peer_max_length-overhead;
    }
    /* Only a successful, fresh phone query may establish that a call disappeared.
     * Failed queues retain each old call so its terminal update can be retried. */
    for(size_t i=0;i<s->call_sent_count;) {
        size_t j;
        for(j=0;j<current.count;j++)if(!strcmp(s->call_sent[i].call.uuid,current.calls[j].uuid))break;
        if(j<current.count) { i++;continue; }
        struct carplay_call ended=s->call_sent[i].call;ended.status=0;
        if(carplay_call_encode(s->call_fields,&ended,body,capacity,&len)<0 ||
           send_body(s,l,0x4155,body,len,now_ms)<0)return;
        s->call_sent_count--;
        memmove(&s->call_sent[i],&s->call_sent[i+1],(s->call_sent_count-i)*sizeof(s->call_sent[0]));
    }
    for(size_t i=0;i<current.count;i++) {
        size_t slot;
        for(slot=0;slot<s->call_sent_count;slot++)if(!strcmp(s->call_sent[slot].call.uuid,current.calls[i].uuid))break;
        if(slot>=CARPLAY_CALLS_MAX || carplay_call_encode(s->call_fields,&current.calls[i],body,capacity,&len)<0)return;
        if(slot<s->call_sent_count && len==s->call_sent[slot].len && !memcmp(s->call_sent[slot].body,body,len))continue;
        if(send_body(s,l,0x4155,body,len,now_ms)<0)return;
        if(slot==s->call_sent_count)s->call_sent_count++;
        s->call_sent[slot].call=current.calls[i];s->call_sent[slot].len=len;
        memcpy(s->call_sent[slot].body,body,len);
    }
}

static void schedule_device_notifications(struct iap2_session *s,
	struct iap2_link *l, uint64_t now_ms)
{
	/* These unsolicited notifications require an explicit receive declaration.
	 * Keep unknown-list DiPlay's existing AA05/4300 exchange unchanged. */
	if (s->device_name[0] && !(s->notifications_sent & NOTIFY_NAME) &&
	    iap2_session_accessory_receives(s, 0x4E09) == IAP2_CAP_DECLARED)
		s->service_pending |= SERVICE_DEVICE_NAME;
	if (s->device_language[0] && !(s->notifications_sent & NOTIFY_LANGUAGE) &&
	    iap2_session_accessory_receives(s, 0x4E0A) == IAP2_CAP_DECLARED)
		s->service_pending |= SERVICE_DEVICE_LANGUAGE;
	/* A discovery-only car never sends 4301; start its declared GPS stream
	 * after authentication while keeping unknown-list DiPlay's old trigger. */
	if (s->vehicle_state && !s->location_subscription_started && !s->location_subscription_stopped &&
	    iap2_session_accessory_receives(s, 0xFFFA) == IAP2_CAP_DECLARED &&
	    iap2_session_accessory_sends(s, 0xFFFB) == IAP2_CAP_DECLARED)
		s->service_pending |= SERVICE_LOCATION_START;
	if (s->service_pending && (!s->service_next_ms || now_ms >= s->service_next_ms)) {
		iap2_log("csm: post-auth service mask=0x%x due; GPS deferred during power exchange",
			 s->service_pending);
		flush_services(s, l, now_ms);
	}
}

static void make_challenge(uint8_t out[32], size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY);
	size_t got = 0;

	if (fd >= 0) {
		while (got < len) {
			ssize_t n = read(fd, out + got, len - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(fd);
	}	while (got < len) {
		out[got] = (uint8_t)(0xA5u ^ (got * 17u));
		got++;
	}
}

static int send_availability(struct iap2_session *s, struct iap2_link *l,
			     uint64_t now_ms)
{
	uint8_t inner[64];
	uint8_t csm[128];
	static const uint8_t available = 1;
	static const char transport[] = "USBHostTransport";
	size_t inner_len = 0;
	size_t group_len;
	size_t csm_len;

	inner_len += add_param(inner + inner_len, sizeof(inner) - inner_len,
			       0, &available, 1);
	inner_len += add_param(inner + inner_len, sizeof(inner) - inner_len,
			       1, (const uint8_t *)transport, sizeof(transport));
	group_len = add_param(csm + IAP2_CSM_HEADER_BYTES,
			      sizeof(csm) - IAP2_CSM_HEADER_BYTES,
			      0, inner, inner_len);
	csm_len = IAP2_CSM_HEADER_BYTES + group_len;
	wr16(csm, 0x4040);
	wr16(csm + 2, (uint16_t)csm_len);
	wr16(csm + 4, 0x4300);
	if (iap2_link_send_control(l, csm, csm_len, now_ms) < 0)
		return -1;
	s->tx_frames++;
	s->availability_sent = 1;
	iap2_log("csm TX 0x4300 CarPlayAvailability wired=1 transport=%s",
		 transport);
	return 0;
}

/* Sends every frame that belongs to the current stage. Retransmissions reuse
 * this, so a stage is always idempotent from the peer's point of view. */
static void session_stage_emit(struct iap2_session *s, struct iap2_link *l,
			       uint64_t now_ms)
{
	switch (s->stage) {
	case IAP2_SESSION_STAGE_IDENT_START:
		if (send_simple(s, l, 0x1D00, now_ms) == 0)
			s->identification_started = 1;
		break;
	case IAP2_SESSION_STAGE_IDENT_INFO:
		/* Acceptance is the parameterless device -> accessory message. It
		 * is queued exactly once; the reliable link owns its retransmission.
		 * A car may advertise a one-packet window, so do not let a timed
		 * phase retry evict the pending AA00 certificate request. */
		if (!s->identification_accept_queued) {
			if (send_simple(s, l, 0x1D02, now_ms) < 0)
				break;
			s->identification_accept_queued = 1;
		}
		if (s->mfi_requested && l->peer_max_retransmissions > 0)
			break;
		if (send_simple(s, l, 0xAA00, now_ms) == 0) {
			if (!s->mfi_requested) {
				s->mfi_requested = 1;
				iap2_log("csm: requesting accessory certificate");
			}
			s->identification_window_wait_reported = 0;
			/* Reliable links retransmit AA00 themselves. Keep the
			 * application retry timer for non-reliable peers. */
			s->stage_next_ms = l->peer_max_retransmissions > 0 ? 0 :
				now_ms + IAP2_SESSION_RETRY_MS;
		} else if (errno == EAGAIN || l->pending_count > 0) {
			if (!s->identification_window_wait_reported) {
				s->identification_window_wait_reported = 1;
				iap2_log("csm: AA00 waiting for reliable TX window "
					 "(pending=%u peer_max_outgoing=%d)",
					 l->pending_count, l->peer_max_outgoing);
			}
			s->stage_next_ms = now_ms +
				IAP2_SESSION_WINDOW_POLL_MS;
		}
		break;
	case IAP2_SESSION_STAGE_MFI_CHALLENGE:
		if (!s->mfi_challenge_generated) {
			make_challenge(s->challenge, s->mfi_challenge_length);
			s->mfi_challenge_generated = 1;
			iap2_log("csm: MFi challenge length=%u prefix=%02x%02x%02x%02x",
				 s->mfi_challenge_length,
				 s->challenge[0], s->challenge[1],
				 s->challenge[2], s->challenge[3]);
		}
		if (send_param(s, l, 0xAA02, 0, s->challenge,
			   s->mfi_challenge_length, now_ms) == 0)
			s->mfi_challenge_sent = 1;
		break;
	case IAP2_SESSION_STAGE_SESSION_WAIT:
		s->authenticated = 1;
		/* Reliable links own AA05 retransmission. Re-enqueueing it after
		 * each ACK starves 4300 forever when only one packet may be in flight. */
		if (!s->authentication_reported || l->peer_max_retransmissions <= 0) {
			if (send_simple(s, l, 0xAA05, now_ms) < 0) {
				s->stage_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
				break;
			}
		}
		if (!s->authentication_reported) {
			s->authentication_sequence = (uint8_t)l->sent_seq;
			s->authentication_reported = 1;
			s->authentication_completed_once = 1;
			iap2_log("csm: authentication accepted "
				 "(signature not policy-verified)");
		}
		if (iap2_session_accessory_receives(s, 0x4300) == IAP2_CAP_EXCLUDED) {
			/* AuthenticationCompleted was queued successfully. This peer
			 * uses discovery, so there is no outstanding 4301 transaction. */
			iap2_log("csm: accessory excludes 0x4300; authenticated, using NCM discovery");
			s->stage = IAP2_SESSION_STAGE_READY;
			s->stage_next_ms = 0;
		} else {
			if (l->peer_max_retransmissions > 0 && l->pending_count > 0) {
				if (!s->availability_sent)
					s->stage_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
			} else if (send_availability(s, l, now_ms) < 0) {
				s->stage_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
			} else {
				s->stage_next_ms = now_ms + IAP2_SESSION_RETRY_MS;
			}
		}
		schedule_device_notifications(s, l, now_ms);
		break;
	default:
		break;
	}
}

/* Enters stage, resets the retry bookkeeping and emits the stage frames. */
static void session_stage_enter(struct iap2_session *s, struct iap2_link *l,
				uint64_t now_ms, int stage)
{
	if (stage == IAP2_SESSION_STAGE_LINK) {
		/* The link fell out of NORMAL: identification must restart from
		 * the beginning when it comes back. */
		s->identification_started = 0;
		s->identification_accepted = 0;
		s->identification_accept_queued = 0;
		s->identification_window_wait_reported = 0;
		s->mfi_requested = 0;
		s->mfi_challenge_sent = 0;
		s->mfi_challenge_generated = 0;
		s->mfi_challenge_length = 32;
		s->authenticated = 0;
		s->authentication_reported = 0;
		s->authentication_sequence = 0;
		s->authentication_link_ready = 0;
		s->availability_sent = 0;
		s->received_carplay_start_session = 0;
		s->received_power = 0;
		s->power_exchange_seen = 0;
		s->power_update_pending = 0;
		s->power_update_step = 0;
		s->power_update_fields = 0;
		s->power_update_next_ms = 0;
		s->received_now_playing_subscription = 0;
		s->art_loading=s->art_announce=s->art_cancel=0;s->art_generation=0;s->art_transfer.phase=0;s->art_next_id=128;
		s->art_attempts=0;s->art_retry_ms=0;
		s->received_route_subscription=0;s->route_next_ms=0;s->route_provider_status=0;
		memset(&s->route_components,0,sizeof(s->route_components));
		memset(&s->route_subscription,0,sizeof(s->route_subscription));
		memset(s->route_sent,0,sizeof(s->route_sent));
		memset(&s->now_subscription,0,sizeof(s->now_subscription));
		s->now_next_ms=0;s->now_sent_len=0;s->now_provider_status=0;
		s->received_communications_subscription=0;s->communications_fields=0;
		s->communications_next_ms=0;s->communications_sent_len=0;s->communications_provider_status=0;
		s->received_call_subscription=0;s->call_fields=0;s->call_next_ms=0;
		s->call_sent_count=0;s->call_provider_status=0;memset(s->call_sent,0,sizeof(s->call_sent));
		s->location_subscription_started = 0;
		s->location_subscription_stopped = 0;
		s->location_received_ms = s->location_retry_ms = 0;
		carplay_vehicle_state_clear_location(s->vehicle_state, s->generation);
		s->message_lists_present = 0;
		memset(s->accessory_sends, 0, sizeof(s->accessory_sends));
		memset(s->accessory_receives, 0, sizeof(s->accessory_receives));
		s->notifications_sent = 0;
		s->service_pending = 0;
		s->service_attempts = 0;
		s->service_next_ms = 0;
		s->library_information_started = 0;
		s->library_updates_started = 0;
		s->bluetooth_updates_started = 0;
		s->rx_len = 0;
	}
	s->stage = stage;
	s->stage_attempts = 0;
	s->stage_backoff_reported = 0;
	if (stage == IAP2_SESSION_STAGE_LINK || stage == IAP2_SESSION_STAGE_READY)
		s->stage_next_ms = 0;
	else
		s->stage_next_ms = now_ms + IAP2_SESSION_RETRY_MS;
	session_stage_emit(s, l, now_ms);
}

static int transmit_location_start(struct iap2_session *s, struct iap2_link *l,
                                   uint64_t now_ms)
{
    uint8_t frame[128];
    size_t len;

    if (!s->vehicle_state || s->location_subscription_stopped)
        return -1;
    if (iap2_session_accessory_receives(s, 0xFFFA) == IAP2_CAP_EXCLUDED ||
        iap2_session_accessory_sends(s, 0xFFFB) == IAP2_CAP_EXCLUDED)
        return -1;
    len = carplay_vehicle_state_encode_location_start(frame, sizeof(frame),
                                                      1000);
    if (!len) {
        iap2_log("csm: could not encode car GPS subscription");
        return -1;
    }
    if (iap2_link_send_control(l, frame, len, now_ms) < 0) {
        /* A full reliable window is normal backpressure, not a GPS error. */
        if (errno != EAGAIN)
            iap2_log("csm: could not subscribe to car GPS: %s", strerror(errno));
        return -1;
    }
    s->tx_frames++;
    iap2_log("csm: car GPS subscription requested GGA/RMC/PASCD");
    return 0;
}

static int transmit_location_stop(struct iap2_session *s, struct iap2_link *l,
                                  uint64_t now_ms)
{
    uint8_t frame[16];
    size_t len;

    if (!s->vehicle_state)
        return -1;
    len = carplay_vehicle_state_encode_location_stop(frame, sizeof(frame));
    if (!len) {
        errno = EINVAL;
        return -1;
    }
    if (iap2_link_send_control(l, frame, len, now_ms) < 0)
        return -1;
    /* An automatic resubscription does not invalidate the last car fix. */
    s->location_subscription_started = 0;
    return 0;
}

static void send_location_start(struct iap2_session *s, struct iap2_link *l,
                               uint64_t now_ms)
{
    if (!s->location_subscription_started && transmit_location_start(s,l,now_ms)==0) {
        s->location_subscription_started = 1;
        s->location_received_ms = now_ms;
        s->location_retry_ms = now_ms + 15000;
    }
}

static void refresh_location_subscription(struct iap2_session *s,struct iap2_link *l,
                                          uint64_t now_ms)
{
    uint64_t stalled_ms;

    if (!s->location_subscription_started || s->location_subscription_stopped ||
        s->stage != IAP2_SESSION_STAGE_READY ||
        !iap2_session_authentication_ready(s,l) || s->service_pending ||
        s->power_update_pending || (l->peer_max_retransmissions>0 && l->pending_count) ||
        now_ms < s->location_retry_ms || now_ms < s->location_received_ms ||
        now_ms - s->location_received_ms < 15000)
        return;
    /* The accessory may leave a silently expired subscription active and
     * acknowledge a duplicate 0xFFFA without resuming 0xFFFB. Close the stale
     * subscription first, then reopen it after the stop reaches the reliable
     * window. */
    stalled_ms = now_ms - s->location_received_ms;
    s->location_retry_ms = now_ms + 30000;
    if (transmit_location_stop(s,l,now_ms)==0) {
        s->service_pending |= SERVICE_LOCATION_START;
        s->service_next_ms = now_ms;
        iap2_log("csm: car GPS stalled after %llu ms; reopening subscription",
                 (unsigned long long)stalled_ms);
    }
}

static void handle_message(struct iap2_session *s, struct iap2_link *l,
			   uint16_t message_id, const uint8_t *body,
			   size_t body_len, uint64_t now_ms)
{
	const uint8_t *payload;
	size_t payload_len;
	int found;

	s->rx_frames++;
	if (carplay_log_should_sample(s->rx_frames))
		iap2_log("csm RX message=0x%04x body=%zu", message_id, body_len);
	if (!valid_params(body, body_len)) {
		s->malformed_frames++;
		iap2_log("csm: malformed parameters in 0x%04x", message_id);
		return;
	}
	if (l->state != IAP2_NORMAL)
		return;
	switch (message_id) {
	case 0x1D01:
		if (s->stage != IAP2_SESSION_STAGE_IDENT_START || !s->identification_started)
			break;
		if (!s->identification_accepted) {
			if (inspect_accessory_identification(s, body, body_len) < 0) {
				s->malformed_frames++;
				iap2_log("csm: malformed 0x1D01 identity; waiting for a complete reply");
				break;
			}
			s->identification_accepted = 1;
			iap2_log("csm: identification information accepted");
		}
		if (s->stage <= IAP2_SESSION_STAGE_IDENT_START)
			session_stage_enter(s, l, now_ms,
					    IAP2_SESSION_STAGE_IDENT_INFO);
		break;
	case 0x1D02:
	case 0x1D03:
		iap2_log("csm: ignoring phone-side identification result 0x%04x "
			 "received from accessory; waiting for 0x1D01", message_id);
		break;
	case 0xAA01:
		if (s->stage != IAP2_SESSION_STAGE_IDENT_INFO ||
		    !s->identification_accepted || !s->mfi_requested) {
			iap2_log("csm: ignoring certificate without queued authentication request");
			break;
		}
		found = first_param(body, body_len, 0, &payload, &payload_len);
		if (found != 0 || payload_len == 0) {
			iap2_log("csm: 0xAA01 has no certificate parameter");
			break;
		}
		iap2_log("csm: certificate bytes=%zu prefix=%02x%02x%02x%02x",
			 payload_len, payload[0], payload_len > 1 ? payload[1] : 0,
			 payload_len > 2 ? payload[2] : 0,
			 payload_len > 3 ? payload[3] : 0);
		session_stage_enter(s, l, now_ms, IAP2_SESSION_STAGE_MFI_CHALLENGE);
		break;
	case 0xAA03:
		if (s->stage != IAP2_SESSION_STAGE_MFI_CHALLENGE || !s->mfi_challenge_sent) {
			iap2_log("csm: ignoring signature without queued challenge");
			break;
		}
		found = first_param(body, body_len, 0, &payload, &payload_len);
		if (found != 0 || payload_len == 0) {
			iap2_log("csm: 0xAA03 has no signature parameter");
			break;
		}
		iap2_log("csm: signature bytes=%zu prefix=%02x%02x%02x%02x%s",
			 payload_len, payload[0], payload_len > 1 ? payload[1] : 0,
			 payload_len > 2 ? payload[2] : 0,
			 payload_len > 3 ? payload[3] : 0,
			 payload_len == 64 ? " (raw P-256)" : "");
		session_stage_enter(s, l, now_ms, IAP2_SESSION_STAGE_SESSION_WAIT);
		break;
	case 0xAA04:
		iap2_log("csm: accessory reported authentication failure");
		break;
	case 0xAE03: {
		const uint8_t *value;
		size_t value_len;
		int current_ma = -1;
		int should_charge = -1;

		if (first_param(body, body_len, 0, &value, &value_len) == 0 &&
		    value_len == 2)
			current_ma = rd16(value);
		if (first_param(body, body_len, 1, &value, &value_len) == 0 &&
		    value_len == 1)
			should_charge = value[0] != 0;
		iap2_log("csm: PowerSourceUpdate car_current=%dmA should_charge=%d (-1 unknown)",
			 current_ma, should_charge);
		break;
	}
	case 0xAE00: {
		unsigned fields = 0;
		if (!service_allowed(s, 0xAE01))
			break;
		/* Start parameters are zero-length flags, not PowerUpdate values.
		 * An empty legacy request retains the reference two-field profile. */
		for (size_t off = 0; off < body_len;) {
			uint16_t param_len;
			uint16_t id;
			unsigned field_bit = 0;
			if (body_len - off < IAP2_PARAM_HEADER_BYTES) {
				s->malformed_frames++;
				iap2_log("csm: truncated PowerUpdate subscription flag");
				return;
			}
			param_len = rd16(body + off);
			id = rd16(body + off + 2);
			if (param_len != IAP2_PARAM_HEADER_BYTES) {
				s->malformed_frames++;
				iap2_log("csm: invalid PowerUpdate subscription flag=%u", id);
				return;
			}
			/* StartPowerUpdates contains zero-length parameter flags. The
			 * reference transmitter supplies fields 0, 1, 4, 5 and 6. This car
			 * requests 0, 1, 5 and 6; encode each requested field in one AE01. */
			if (id == 0 || id == 1)
				field_bit = 1u << id;
			else if (id == 4)
				field_bit = 1u << 4;
			else if (id == 5 || id == 6)
				field_bit = 1u << id;
			if (field_bit)
				fields |= field_bit;
			else
				iap2_log("csm: PowerUpdate requested field=%u unpublished; omitted", id);
			off += param_len;
		}
		if (!body_len)
			fields = 3;
		if (s->received_power && s->power_update_fields == fields)
			break;
		s->received_power = 1;
		s->power_exchange_seen = 1;
		s->power_update_fields = fields;
		s->power_update_step = 0;
		s->power_update_pending = fields != 0;
		s->power_update_next_ms = 0;
		iap2_log("csm: PowerUpdate subscription fields=0x%x mono_ms=%llu",
			 fields, (unsigned long long)now_ms);
		/* Preserve the reference name/language preference. This is not proof
		 * of Audi's disconnect cause. Optional GPS must never gate a requested
		 * power response; the pump ticks services after feeding this message. */
		if (!(s->service_pending & SERVICE_POST_AUTH) &&
		    s->power_update_pending &&
		    send_power_update(s, l, now_ms) < 0) {
			s->power_update_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
		}
		break;
	}
	case 0x5000:
		if(!service_allowed(s,0x5001))break;
		if(carplay_now_subscribe(&s->now_subscription,body,body_len)<0) {
			iap2_log("csm: malformed NowPlaying subscription, retaining prior fields");break;
		}
		s->received_now_playing_subscription=1;s->now_next_ms=0;s->now_sent_len=0;
		s->art_generation=0;
		iap2_log("csm: NowPlaying subscribed media=0x%08x playback=0x%08x",
		    s->now_subscription.media,s->now_subscription.playback);
		break;
	case 0x5003: {
		struct carplay_now_playing state;
		if(!service_allowed(s,0x5001) || !s->received_now_playing_subscription)break;
		/* Only the verified seek field is implemented; reject ambiguous payloads. */
		if(body_len!=8 || rd16(body)!=8 || rd16(body+2)!=0 ||
		   carplay_now_read(now_ms,&state)<0 || !state.can_seek) {
			iap2_log("csm: SetNowPlayingInformation unsupported or unavailable");break;
		}
		if(carplay_app_command(3,((uint32_t)rd16(body+4)<<16)|rd16(body+6))<0)
			iap2_log("csm: NowPlaying seek rejected by Android provider");
		break;
	}
	case 0x5200:
		if(!service_allowed(s,0x5201))break;
		if(carplay_route_subscribe(&s->route_components,body,body_len,&s->route_subscription)<0){
			iap2_log("csm: invalid route subscription or no identified display");break;
		}
		s->received_route_subscription=1;s->route_next_ms=0;
		memset(s->route_sent,0,sizeof(s->route_sent));
		iap2_log("csm: route subscribed displays=%u flags=%u",s->route_subscription.count,s->route_subscription.flags);
		break;
	case 0x4157: {
		struct carplay_now_subscription fields;
		uint8_t wrapped[CARPLAY_NOW_MAX];
		if(!service_allowed(s,0x4158))break;
		if(body_len>sizeof(wrapped)-4)break;
		wr16(wrapped,(uint16_t)(body_len+4));wr16(wrapped+2,0);
		memcpy(wrapped+4,body,body_len);
		if(carplay_now_subscribe(&fields,wrapped,body_len+4)<0) {
			iap2_log("csm: malformed communications subscription");break;
		}
		s->received_communications_subscription=1;s->communications_fields=fields.media;
		s->communications_next_ms=0;s->communications_sent_len=0;
		iap2_log("csm: communications subscribed fields=0x%08x",fields.media);
		break;
	}
	case 0x4154: {
		uint32_t fields;
		if(!service_allowed(s,0x4155))break;
		if(carplay_calls_subscribe(&fields,body,body_len)<0) {
			iap2_log("csm: malformed call-state subscription; retaining prior fields");break;
		}
		s->received_call_subscription=1;s->call_fields=fields;
		s->call_next_ms=0;s->call_sent_count=0;
		iap2_log("csm: call-state subscribed fields=0x%08x",fields);
		break;
	}
	case 0x4C00:
		if (!service_allowed(s, 0x4C01))
			break;
		s->library_information_started = 1;
		s->service_pending |= SERVICE_LIBRARY_INFO;
		flush_services(s, l, now_ms);
		break;
	case 0x4C03:
		if (!service_allowed(s, 0x4C04))
			break;
		if (!s->library_information_started ||
		    first_param(body, body_len, 0, &payload, &payload_len) != 0 ||
		    payload_len != sizeof(empty_library_uid) ||
		    memcmp(payload, empty_library_uid, sizeof(empty_library_uid)) != 0) {
			iap2_log("csm: unimplemented MediaLibraryUpdate for unknown/missing library identifier");
			break;
		}
		s->library_updates_started = 1;
		s->service_pending |= SERVICE_LIBRARY_UPDATE;
		flush_services(s, l, now_ms);
		break;
	case 0x4E03:
		if (!service_allowed(s, 0x4E04))
			break;
		if (first_param(body, body_len, 0, &payload, &payload_len) != 0 || payload_len != 2) {
			iap2_log("csm: invalid BluetoothConnectionUpdates component identifier");
			break;
		}
		s->bluetooth_component = rd16(payload);
		s->bluetooth_updates_started = 1;
		s->service_pending |= SERVICE_BLUETOOTH;
		flush_services(s, l, now_ms);
		break;
	case 0x4C02:
		s->library_information_started = 0;
		s->service_pending &= ~SERVICE_LIBRARY_INFO;
		break;
	case 0x4C05:
		s->library_updates_started = 0;
		s->service_pending &= ~SERVICE_LIBRARY_UPDATE;
		break;
	case 0x4E05:
		s->bluetooth_updates_started = 0;
		s->service_pending &= ~SERVICE_BLUETOOTH;
		break;
	case 0x4E01: /* Accessory component information needs no reply. */
		break;
	case 0xAE02:
		s->power_exchange_seen = 1;
		s->received_power = 0;
		s->power_update_pending = 0;
		s->power_update_step = 0;
		s->power_update_fields = 0;
		s->power_update_next_ms = 0;
		break;
	case 0x5002:
		s->received_now_playing_subscription=0;s->now_sent_len=0;s->now_next_ms=0;
		memset(&s->now_subscription,0,sizeof(s->now_subscription));break;
	case 0x5203: {
		struct carplay_route_subscription stop;
		if(!service_allowed(s,0x5201) || carplay_route_subscribe(&s->route_components,body,body_len,&stop)<0)break;
		for(unsigned i=0;i<s->route_subscription.count;){
			unsigned j;for(j=0;j<stop.count;j++)if(stop.ids[j]==s->route_subscription.ids[i])break;
			if(j<stop.count){s->route_subscription.count--;memmove(&s->route_subscription.ids[i],
				&s->route_subscription.ids[i+1],(s->route_subscription.count-i)*sizeof(uint16_t));}
			else i++;
		}
		s->received_route_subscription=s->route_subscription.count!=0;break;
	}
	case 0x4159:
		s->received_communications_subscription=0;s->communications_sent_len=0;
		s->communications_fields=0;break;
	case 0x4156:
		s->received_call_subscription=0;s->call_sent_count=0;s->call_next_ms=0;
		memset(s->call_sent,0,sizeof(s->call_sent));break;
	case 0x4301:
		if (!s->authenticated || !s->authentication_reported || !s->availability_sent ||
		    s->stage < IAP2_SESSION_STAGE_SESSION_WAIT ||
		    iap2_session_accessory_sends(s, 0x4301) == IAP2_CAP_EXCLUDED) {
			iap2_log("csm: ignoring CarPlayStartSession without authenticated availability or excluded by declaration");
			break;
		}
		if (connect_carplay_start(body, body_len) < 0)
			break;
		s->received_carplay_start_session = 1;
		iap2_log("csm: received 0x4301 CarPlayStartSession body=%zu",
			 body_len);
		if (s->vehicle_state && !s->location_subscription_started && !s->location_subscription_stopped &&
		    iap2_session_accessory_receives(s, 0xFFFA) != IAP2_CAP_EXCLUDED &&
		    iap2_session_accessory_sends(s, 0xFFFB) != IAP2_CAP_EXCLUDED) {
			s->service_pending |= SERVICE_LOCATION_START;
			flush_services(s, l, now_ms);
		}
		if (s->stage < IAP2_SESSION_STAGE_READY)
			session_stage_enter(s, l, now_ms,
					    IAP2_SESSION_STAGE_READY);
		break;
	case 0x4e0b: {
		const uint8_t *payload = NULL;
		size_t payload_len = 0;
		int64_t epoch_seconds;

		if (first_param(body, body_len, 0, &payload, &payload_len) != 0 ||
		    payload_len != 8u) {
			iap2_log("csm: invalid DeviceTimeUpdate body=%zu",
				 body_len);
			break;
		}
		if (carplay_time_sync_from_apple_epoch(rd64(payload),
						      &epoch_seconds) < 0) {
			iap2_log("csm: DeviceTimeUpdate rejected as implausible");
			break;
		}
		apply_vehicle_time(s, epoch_seconds, "DeviceTimeUpdate");
		break;
	}
	case 0xfffa:
		iap2_log("csm: car requested location updates");
		break;
	case 0xfffb: {
		char error[96];
		int location_rc = carplay_vehicle_state_decode_location_information(
			s->vehicle_state, s->generation, body, body_len,
			error, sizeof(error));
		if (location_rc < 0)
			iap2_log("csm: invalid car location data: %s", error);
		else if (location_rc == 0)
			iap2_log("csm: car location cleared");
		else {
			char nmea[CARPLAY_NMEA_CAPACITY];
			char detail[128];
			size_t nmea_len = 0;
			s->location_received_ms = now_ms;

			if (carplay_log_should_sample(s->rx_frames))
				iap2_log("csm: car location updated bytes=%zu", body_len);
			if (carplay_vehicle_state_get_nmea(
				    s->vehicle_state, nmea, sizeof(nmea),
				    &nmea_len) == 0) {
				int time_rc = carplay_time_sync_apply_nmea(
					nmea, nmea_len, detail,
					sizeof(detail));
				if (time_rc != s->time_sync_last_result ||
				    strcmp(detail,
				           s->time_sync_detail) != 0) {
					iap2_log("csm: car NMEA time sync %s",
						 detail);
					s->time_sync_last_result = time_rc;
					snprintf(s->time_sync_detail,
						 sizeof(s->time_sync_detail),
						 "%s", detail);
				}
			}
		}
		break;
	}
	case 0xfffc:
		carplay_vehicle_state_clear_location(s->vehicle_state, s->generation);
		s->location_subscription_started = 0;
		s->location_subscription_stopped = 1;
		s->service_pending &= ~SERVICE_LOCATION_START;
		iap2_log("csm: car stopped location updates");
		break;
	default:
		iap2_log("csm: unhandled message=0x%04x body=%zu",
			 message_id, body_len);
		break;
	}
}
static void parse_available(struct iap2_session *s, struct iap2_link *l,
			    uint64_t now_ms)
{
	while (s->rx_len >= IAP2_CSM_HEADER_BYTES) {
		uint8_t *p = s->rx;
		size_t frame_len;
		uint16_t message_id;

		if (p[0] != 0x40 || p[1] != 0x40) {
			memmove(p, p + 1, s->rx_len - 1);
			s->rx_len--;
			continue;
		}
		frame_len = rd16(p + 2);
		if (frame_len < IAP2_CSM_HEADER_BYTES) {
			memmove(p, p + 1, s->rx_len - 1);
			s->rx_len--;
			continue;
		}
		if (s->rx_len < frame_len)
			return;
		message_id = rd16(p + 4);
		handle_message(s, l, message_id, p + IAP2_CSM_HEADER_BYTES,
			       frame_len - IAP2_CSM_HEADER_BYTES, now_ms);
		memmove(p, p + frame_len, s->rx_len - frame_len);
		s->rx_len -= frame_len;
	}
}

void iap2_session_init(struct iap2_session *s)
{
	memset(s, 0, sizeof(*s));
	s->time_sync_last_result = 2;
	s->mfi_challenge_length = 32;
#ifdef __ANDROID__
	{
		char name[PROP_VALUE_MAX] = "", language[PROP_VALUE_MAX] = "";
		__system_property_get("ro.product.model", name);
		if (!__system_property_get("persist.sys.locale", language))
			__system_property_get("persist.sys.language", language);
		iap2_session_set_device_identity(s, name, language);
	}
#endif
}

void iap2_session_set_device_identity(struct iap2_session *s,
	const char *name, const char *language)
{
	if (!s)
		return;
	/* Metadata is configured before negotiation. Do not silently change an
	 * already active subscription or truncate a UTF-8 product model. */
	if (s->stage != IAP2_SESSION_STAGE_LINK &&
	    s->stage != IAP2_SESSION_STAGE_IDENT_START)
		return;
	s->device_name[0] = 0;
	s->device_language[0] = 0;
	if (name && strlen(name) < sizeof(s->device_name))
		memcpy(s->device_name, name, strlen(name) + 1);
	if (language && strlen(language) < sizeof(s->device_language))
		memcpy(s->device_language, language, strlen(language) + 1);
}

void iap2_session_set_vehicle_state(struct iap2_session *s,
                                    struct carplay_vehicle_state *state,
                                    uint64_t generation)
{
    if (!s)
        return;
    s->vehicle_state = state;
    s->generation = generation;
    s->location_subscription_started = 0;
    s->location_subscription_stopped = 0;
    s->location_received_ms = s->location_retry_ms = 0;
}

void iap2_session_send_location_stop(struct iap2_session *s,
                                     struct iap2_link *l,
                                     uint64_t now_ms)
{
    uint8_t frame[16];
    size_t len;

    if (!s || !l)
        return;
    s->location_subscription_stopped = 1;
    s->service_pending &= ~SERVICE_LOCATION_START;
    carplay_vehicle_state_clear_location(s->vehicle_state, s->generation);
    if (!s->location_subscription_started) return;
    len = carplay_vehicle_state_encode_location_stop(frame, sizeof(frame));
    if (len)
        (void)iap2_link_send_control(l, frame, len, now_ms);
    s->location_subscription_started = 0;
    iap2_log("csm: car GPS subscription stopped");
}

int iap2_session_authentication_ready(struct iap2_session *s,
                                    const struct iap2_link *l)
{
	if (l->state != IAP2_NORMAL || !s->authenticated ||
	    !s->authentication_reported)
		return 0;
	if (!s->authentication_link_ready) {
		if (l->peer_max_retransmissions > 0) {
			for (unsigned i = 0; i < l->pending_count; i++)
				if (l->pending[i].sequence == s->authentication_sequence)
					return 0;
		}
		s->authentication_link_ready = 1;
		iap2_log("csm: AirPlay authentication gate ready AA05_seq=%u delivery=%s",
			 s->authentication_sequence,
			 l->peer_max_retransmissions > 0 ? "link-acknowledged" : "untracked-peer");
	}
	return 1;
}

void iap2_session_tick(struct iap2_session *s, struct iap2_link *l,
		       uint64_t now_ms)
{
	if (s->stage == IAP2_SESSION_STAGE_LINK) {
		if (l->state == IAP2_NORMAL) {
			iap2_log("csm: link ready, starting identification");
			session_stage_enter(s, l, now_ms,
					    IAP2_SESSION_STAGE_IDENT_START);
		}
		return;
	}
	if (l->state != IAP2_NORMAL) {
		iap2_log("csm: link left NORMAL during identification "
			 "(state=%d), restarting", l->state);
		session_stage_enter(s, l, now_ms, IAP2_SESSION_STAGE_LINK);
		return;
	}
	/* Prefer declared identity notifications, then requested power updates.
	 * Optional GPS is best effort and cannot block the power exchange. */
	if ((s->service_pending & SERVICE_POST_AUTH) &&
	    (!s->service_next_ms || now_ms >= s->service_next_ms))
		flush_services(s, l, now_ms);
	if (!(s->service_pending & SERVICE_POST_AUTH) && s->power_update_pending &&
	    (!s->power_update_next_ms || now_ms >= s->power_update_next_ms)) {
		if (send_power_update(s, l, now_ms) < 0)
			s->power_update_next_ms = now_ms + IAP2_SESSION_WINDOW_POLL_MS;
	}
	if (!(s->service_pending & SERVICE_POST_AUTH) && s->service_pending &&
	    (!s->service_next_ms || now_ms >= s->service_next_ms))
		flush_services(s, l, now_ms);
	flush_now_playing(s,l,now_ms);
	flush_artwork(s,l,now_ms);
	flush_route(s,l,now_ms);
	flush_communications(s,l,now_ms);
	flush_calls(s,l,now_ms);
	refresh_location_subscription(s,l,now_ms);
	if (s->stage == IAP2_SESSION_STAGE_READY)
		return;
	if (s->stage == IAP2_SESSION_STAGE_SESSION_WAIT &&
	    (!s->authentication_reported || !s->availability_sent)) {
		if (s->stage_next_ms && now_ms >= s->stage_next_ms)
			session_stage_emit(s, l, now_ms);
		return;
	}
	/* AA00 cannot enter a full reliable window. Poll the link closely until
	 * acceptance is acknowledged; this is transport backpressure, not an
	 * application-level unanswered stage. */
	if (s->stage == IAP2_SESSION_STAGE_IDENT_INFO &&
	    s->identification_accept_queued && !s->mfi_requested) {
		if (s->stage_next_ms && now_ms >= s->stage_next_ms)
			session_stage_emit(s, l, now_ms);
		return;
	}
	/* Once AA00 is queued on a reliable link, link-layer retransmission is
	 * authoritative. A duplicate application send would consume another
	 * sequence and is especially harmful with MaxOutstandingPackets=1. */
	if (s->stage == IAP2_SESSION_STAGE_IDENT_INFO &&
	    s->mfi_requested && l->peer_max_retransmissions > 0) {
		s->stage_next_ms = 0;
		return;
	}
	/* The link layer owns same-sequence retransmission after the challenge is
	 * queued. A new application sequence for 0xAA02 breaks peers that retain
	 * one accessory transaction while waiting for the signature. */
	if (s->stage == IAP2_SESSION_STAGE_MFI_CHALLENGE &&
	    s->mfi_challenge_sent && l->peer_max_retransmissions > 0) {
		s->stage_next_ms = 0;
		return;
	}
	if (!s->stage_next_ms || now_ms < s->stage_next_ms)
		return;
	if (s->stage_attempts < IAP2_SESSION_RETRY_LIMIT) {
		s->stage_attempts++;
		s->stage_next_ms = now_ms + IAP2_SESSION_RETRY_MS;
		iap2_log("csm: stage %d unanswered, resending (attempt %u/%u)",
			 s->stage, s->stage_attempts,
			 IAP2_SESSION_RETRY_LIMIT);
	} else {
		s->stage_next_ms = now_ms + IAP2_SESSION_SLOW_RETRY_MS;
		if (!s->stage_backoff_reported) {
			s->stage_backoff_reported = 1;
			iap2_log("csm: stage %d still unanswered after %u "
				 "attempts, backing off to %u ms", s->stage,
				 s->stage_attempts,
				 IAP2_SESSION_SLOW_RETRY_MS);
		}
	}
	session_stage_emit(s, l, now_ms);
}

int iap2_session_stage(const struct iap2_session *s)
{
	return s ? s->stage : IAP2_SESSION_STAGE_LINK;
}

uint64_t iap2_session_stage_deadline(const struct iap2_session *s)
{
	return s ? s->stage_next_ms : 0;
}

void iap2_session_feed(struct iap2_session *s, struct iap2_link *l,
		       const uint8_t *data, size_t len, uint64_t now_ms)
{
	if (len > sizeof(s->rx) - s->rx_len) {
		iap2_log("csm: receive reassembly overflow (%zu + %zu)",
			 s->rx_len, len);
		s->rx_len = 0;
		s->malformed_frames++;
		return;
	}
	memcpy(s->rx + s->rx_len, data, len);
	s->rx_len += len;
	parse_available(s, l, now_ms);
}




