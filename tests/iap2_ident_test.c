/*
 * iap2_ident_test.c -- legacy serializer checks, NOT a 0x1D02 wire contract.
 *
 * The group is what a real host reads to recognise the box, so the layout is
 * asserted exactly: parameter header width, big-endian fields, NUL terminated
 * strings and the double-NUL that closes a multi-string parameter.
 */
#include "iap2ident.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_PARAMS 16

struct parsed_param {
	uint16_t id;
	uint16_t total;
	size_t value_offset;
	size_t value_len;
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static size_t parse_group(const uint8_t *buf, size_t len,
			  struct parsed_param *out, size_t cap)
{
	size_t off = 0;
	size_t count = 0;

	while (off + 4 <= len && count < cap) {
		uint16_t total = rd16(buf + off);
		uint16_t id = rd16(buf + off + 2);

		assert(total >= 4);
		assert((size_t)total <= len - off);
		out[count].id = id;
		out[count].total = total;
		out[count].value_offset = off + 4;
		out[count].value_len = (size_t)total - 4u;
		count++;
		off += total;
	}
	assert(off == len);
	return count;
}

static const struct parsed_param *find_param(const struct parsed_param *p,
					     size_t count, uint16_t id)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (p[i].id == id)
			return &p[i];
	return NULL;
}

static void assert_text(const uint8_t *buf, const struct parsed_param *p,
			uint16_t id, const char *expected)
{
	size_t expected_len = strlen(expected) + 1u;

	assert(p != NULL);
	assert(p->value_len == expected_len);
	assert(memcmp(buf + p->value_offset, expected, expected_len) == 0);
	assert(buf[p->value_offset + p->value_len - 1] == 0);
	(void)id;
}

static void test_defaults_layout(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	struct parsed_param params[MAX_PARAMS];
	size_t written = 0;
	size_t count;
	size_t measured = 0;
	const struct parsed_param *p;

	iap2_ident_defaults(&info);
	assert(strcmp(info.name, "OPi4 CarPlay") == 0);
	assert(strcmp(info.model, "OPi4-CarPlayBridge") == 0);
	assert(strcmp(info.manufacturer, "Shenbang") == 0);
	assert(info.serial[0] != 0);
	assert(info.hardware_version[0] != 0);
	assert(info.firmware_version[0] != 0);
	assert(info.current_language[0] != 0);
	assert(info.ea_protocol_count == 1);
	assert(strcmp(info.ea_protocols[0], "com.apple.carplay") == 0);

	assert(iap2_ident_measure(&info, &measured) == IAP2_IDENT_OK);
	memset(buf, 0xAA, sizeof(buf));
	assert(iap2_ident_build(&info, buf, sizeof(buf), &written) ==
	       IAP2_IDENT_OK);
	assert(written == measured);
	assert(written < sizeof(buf));

	count = parse_group(buf, written, params, MAX_PARAMS);
	assert(count == 8);

	p = find_param(params, count, IAP2_IDENT_PARAM_NAME);
	assert_text(buf, p, IAP2_IDENT_PARAM_NAME, "OPi4 CarPlay");
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_MODEL),
		    IAP2_IDENT_PARAM_MODEL, "OPi4-CarPlayBridge");
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_MANUFACTURER),
		    IAP2_IDENT_PARAM_MANUFACTURER, "Shenbang");
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_SERIAL),
		    IAP2_IDENT_PARAM_SERIAL, info.serial);
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_HARDWARE),
		    IAP2_IDENT_PARAM_HARDWARE, info.hardware_version);
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_FIRMWARE),
		    IAP2_IDENT_PARAM_FIRMWARE, info.firmware_version);
	assert_text(buf, find_param(params, count, IAP2_IDENT_PARAM_LANGUAGE),
		    IAP2_IDENT_PARAM_LANGUAGE, info.current_language);

	/* Parameters keep the documented order so the body stays diffable. */
	assert(params[0].id == IAP2_IDENT_PARAM_NAME);
	assert(params[1].id == IAP2_IDENT_PARAM_MODEL);
	assert(params[2].id == IAP2_IDENT_PARAM_MANUFACTURER);
	assert(params[3].id == IAP2_IDENT_PARAM_SERIAL);
	assert(params[4].id == IAP2_IDENT_PARAM_HARDWARE);
	assert(params[5].id == IAP2_IDENT_PARAM_FIRMWARE);
	assert(params[6].id == IAP2_IDENT_PARAM_LANGUAGE);
	assert(params[7].id == IAP2_IDENT_PARAM_EA_PROTOCOLS);

	/* Multi-string: "com.apple.carplay\0\0". */
	p = &params[7];
	assert(p->value_len == strlen("com.apple.carplay") + 2);
	assert(memcmp(buf + p->value_offset, "com.apple.carplay",
		      strlen("com.apple.carplay") + 1) == 0);
	assert(buf[p->value_offset + p->value_len - 1] == 0);
	assert(buf[p->value_offset + p->value_len - 2] == 0);
}

static void test_length_field_includes_header(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	size_t written = 0;

	iap2_ident_defaults(&info);
	assert(iap2_ident_build(&info, buf, sizeof(buf), &written) ==
	       IAP2_IDENT_OK);
	/* total = 4-byte header + value length, header included. */
	assert(rd16(buf) == 4u + strlen(info.name) + 1u);
	assert(rd16(buf + 2) == IAP2_IDENT_PARAM_NAME);
}

static void test_optional_fields_skipped(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	size_t full = 0;
	size_t partial = 0;

	iap2_ident_defaults(&info);
	assert(iap2_ident_measure(&info, &full) == IAP2_IDENT_OK);
	info.hardware_version = NULL;
	info.current_language = NULL;
	info.ea_protocols = NULL;
	info.ea_protocol_count = 0;
	assert(iap2_ident_measure(&info, &partial) == IAP2_IDENT_OK);
	assert(partial < full);
	assert(full - partial ==
	       (4u + strlen("1.0") + 1u) +		/* hardware */
	       (4u + strlen("en") + 1u) +		/* language */
	       (4u + strlen("com.apple.carplay") + 2u));	/* EA list */
	assert(iap2_ident_build(&info, buf, sizeof(buf), NULL) == IAP2_IDENT_OK);
}

static void test_empty_string_is_encoded(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	struct parsed_param params[MAX_PARAMS];
	size_t written = 0;
	size_t count;
	const struct parsed_param *p;

	iap2_ident_defaults(&info);
	info.current_language = "";
	assert(iap2_ident_build(&info, buf, sizeof(buf), &written) ==
	       IAP2_IDENT_OK);
	count = parse_group(buf, written, params, MAX_PARAMS);
	p = find_param(params, count, IAP2_IDENT_PARAM_LANGUAGE);
	assert(p != NULL);
	assert(p->value_len == 1);
	assert(buf[p->value_offset] == 0);
}

static void test_short_buffer_rejected_without_writing(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	size_t measured = 0;
	size_t written = 123;
	size_t i;

	iap2_ident_defaults(&info);
	assert(iap2_ident_measure(&info, &measured) == IAP2_IDENT_OK);
	memset(buf, 0x5A, sizeof(buf));
	assert(iap2_ident_build(&info, buf, measured - 1, &written) ==
	       IAP2_IDENT_ERR_CAPACITY);
	assert(written == 123);
	for (i = 0; i < measured; i++)
		assert(buf[i] == 0x5A);
	/* The exact size is enough. */
	assert(iap2_ident_build(&info, buf, measured, &written) ==
	       IAP2_IDENT_OK);
	assert(written == measured);
}

static void test_argument_checks(void)
{
	struct iap2_ident_info info;
	uint8_t buf[IAP2_IDENT_MAX_BYTES];
	size_t size = 0;
	static const char *const with_blank[] = { "com.apple.carplay", "" };

	assert(iap2_ident_measure(NULL, &size) == IAP2_IDENT_ERR_ARGUMENT);
	assert(iap2_ident_build(NULL, buf, sizeof(buf), NULL) ==
	       IAP2_IDENT_ERR_ARGUMENT);

	iap2_ident_defaults(&info);
	assert(iap2_ident_measure(&info, NULL) == IAP2_IDENT_ERR_ARGUMENT);
	info.ea_protocols = with_blank;
	info.ea_protocol_count = 2;
	assert(iap2_ident_measure(&info, &size) == IAP2_IDENT_ERR_ARGUMENT);
	info.ea_protocols = NULL;
	info.ea_protocol_count = 0;
	assert(iap2_ident_measure(&info, &size) == IAP2_IDENT_OK);
}

static void test_no_parameters_at_all(void)
{
	struct iap2_ident_info info;
	uint8_t buf[8];
	size_t written = 99;

	memset(&info, 0, sizeof(info));
	assert(iap2_ident_build(&info, buf, sizeof(buf), &written) ==
	       IAP2_IDENT_OK);
	assert(written == 0);
}

int main(void)
{
	test_defaults_layout();
	test_length_field_includes_header();
	test_optional_fields_skipped();
	test_empty_string_is_encoded();
	test_short_buffer_rejected_without_writing();
	test_argument_checks();
	test_no_parameters_at_all();
	printf("iap2 identification tests passed\n");
	return 0;
}
