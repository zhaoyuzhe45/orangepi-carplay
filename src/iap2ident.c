/*
 * iap2ident.c -- builds the iAP2 identification parameter group.
 *
 * Kept free of USB, socket and logging dependencies so the byte layout can be
 * unit tested on any target.
 */
#include "iap2ident.h"

#include <string.h>

#define PARAM_HEADER_BYTES 4u
#define PARAM_MAX_VALUE    (0xFFFFu - PARAM_HEADER_BYTES)

static void wr16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void write_param(uint8_t *dst, size_t *off, uint16_t id,
			const uint8_t *value, size_t len)
{
	wr16(dst + *off, (uint16_t)(PARAM_HEADER_BYTES + len));
	wr16(dst + *off + 2, id);
	if (len)
		memcpy(dst + *off + PARAM_HEADER_BYTES, value, len);
	*off += PARAM_HEADER_BYTES + len;
}

/* Adds one NUL-terminated text parameter when the field is present. Returns
 * IAP2_IDENT_ERR_CAPACITY when the caller's buffer is too small. */
static int add_text(uint8_t *dst, size_t capacity, size_t *off, uint16_t id,
		    const char *value)
{
	size_t len;

	if (!value)
		return IAP2_IDENT_OK;
	len = strlen(value) + 1u;
	if (len > PARAM_MAX_VALUE)
		return IAP2_IDENT_ERR_ARGUMENT;
	if (dst) {
		if (*off + PARAM_HEADER_BYTES + len > capacity)
			return IAP2_IDENT_ERR_CAPACITY;
		write_param(dst, off, id, (const uint8_t *)value, len);
	} else {
		*off += PARAM_HEADER_BYTES + len;
	}
	return IAP2_IDENT_OK;
}

static int add_ea_protocols(uint8_t *dst, size_t capacity, size_t *off,
			    const struct iap2_ident_info *info)
{
	size_t len = 1u;	/* the terminating empty entry */
	size_t i;
	size_t pos;

	if (!info->ea_protocols || info->ea_protocol_count == 0)
		return IAP2_IDENT_OK;
	for (i = 0; i < info->ea_protocol_count; i++) {
		const char *entry = info->ea_protocols[i];

		if (!entry || !entry[0])
			return IAP2_IDENT_ERR_ARGUMENT;
		len += strlen(entry) + 1u;
	}
	if (len > PARAM_MAX_VALUE)
		return IAP2_IDENT_ERR_ARGUMENT;
	if (!dst) {
		*off += PARAM_HEADER_BYTES + len;
		return IAP2_IDENT_OK;
	}
	if (*off + PARAM_HEADER_BYTES + len > capacity)
		return IAP2_IDENT_ERR_CAPACITY;
	wr16(dst + *off, (uint16_t)(PARAM_HEADER_BYTES + len));
	wr16(dst + *off + 2, IAP2_IDENT_PARAM_EA_PROTOCOLS);
	pos = *off + PARAM_HEADER_BYTES;
	for (i = 0; i < info->ea_protocol_count; i++) {
		size_t entry_len = strlen(info->ea_protocols[i]) + 1u;

		memcpy(dst + pos, info->ea_protocols[i], entry_len);
		pos += entry_len;
	}
	dst[pos] = 0;		/* extra NUL closes the list */
	*off += PARAM_HEADER_BYTES + len;
	return IAP2_IDENT_OK;
}

static int build_group(const struct iap2_ident_info *info, uint8_t *dst,
		       size_t capacity, size_t *off)
{
	int rc;

	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_NAME, info->name);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_MODEL, info->model);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_MANUFACTURER,
		      info->manufacturer);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_SERIAL,
		      info->serial);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_HARDWARE,
		      info->hardware_version);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_FIRMWARE,
		      info->firmware_version);
	if (rc)
		return rc;
	rc = add_text(dst, capacity, off, IAP2_IDENT_PARAM_LANGUAGE,
		      info->current_language);
	if (rc)
		return rc;
	return add_ea_protocols(dst, capacity, off, info);
}

int iap2_ident_measure(const struct iap2_ident_info *info, size_t *size)
{
	size_t off = 0;
	int rc;

	if (!info || !size)
		return IAP2_IDENT_ERR_ARGUMENT;
	rc = build_group(info, NULL, 0, &off);
	if (rc)
		return rc;
	*size = off;
	return IAP2_IDENT_OK;
}

int iap2_ident_build(const struct iap2_ident_info *info, uint8_t *out,
		     size_t capacity, size_t *written)
{
	size_t needed = 0;
	size_t off = 0;
	int rc;

	if (!info || (!out && capacity))
		return IAP2_IDENT_ERR_ARGUMENT;
	/* Measure first: a short buffer must be reported without leaving a
	 * half-written group behind. */
	rc = build_group(info, NULL, 0, &needed);
	if (rc)
		return rc;
	if (needed > capacity)
		return IAP2_IDENT_ERR_CAPACITY;
	rc = build_group(info, out, capacity, &off);
	if (rc)
		return rc;
	if (written)
		*written = off;
	return IAP2_IDENT_OK;
}

/* Identity advertised to the accessory. The box is the device side, so this
 * describes an iPhone-like endpoint rather than a car head unit. Keep the
 * strings printable ASCII: the session layer also logs them as text. */
void iap2_ident_defaults(struct iap2_ident_info *info)
{
	static const char *const ea_protocols[] = {
		"com.apple.carplay",
	};

	if (!info)
		return;
	memset(info, 0, sizeof(*info));
	info->name = "OPi4 CarPlay";
	info->model = "OPi4-CarPlayBridge";
	info->manufacturer = "Shenbang";
	info->serial = "OPI4CP00000001";
	info->hardware_version = "1.0";
	info->firmware_version = "1.0";
	info->current_language = "en";
	info->ea_protocols = ea_protocols;
	info->ea_protocol_count = sizeof(ea_protocols) / sizeof(ea_protocols[0]);
}
