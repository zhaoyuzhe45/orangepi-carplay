/*
 * iap2ident.h -- iAP2 identification-information parameter group.
 *
 * Legacy experimental serializer, no longer used by the phone-side session.
 * It must NOT be attached to 0x1D02: that message is IdentificationAccepted
 * with an empty body. The accessory supplies identity in 0x1D01. This older
 * emulator-specific schema is not a verified accessory implementation either.
 *
 * Wire layout of every parameter (big endian):
 *   u16 totalLength   (>= 4, includes this header)
 *   u16 parameterId
 *   u8  value[totalLength - 4]
 *
 * Encoding rules:
 *   - text parameters are NUL terminated (the terminator is part of the value);
 *   - multi-string parameters repeat NUL-terminated entries and finish with an
 *     extra NUL, so the value always ends with two zero bytes.
 *
 * Retained for historical experiments and serializer tests only; see
 * docs/evidence/orangecar-review-2026-11-11.md for the protocol correction.
 */
#ifndef IAP2IDENT_H
#define IAP2IDENT_H

#include <stddef.h>
#include <stdint.h>

#define IAP2_IDENT_PARAM_NAME          0u
#define IAP2_IDENT_PARAM_MODEL         1u
#define IAP2_IDENT_PARAM_MANUFACTURER  2u
#define IAP2_IDENT_PARAM_SERIAL        3u
#define IAP2_IDENT_PARAM_HARDWARE      4u
#define IAP2_IDENT_PARAM_FIRMWARE      5u
#define IAP2_IDENT_PARAM_LANGUAGE      6u
#define IAP2_IDENT_PARAM_EA_PROTOCOLS  7u

#define IAP2_IDENT_MAX_BYTES 1024u

enum iap2_ident_result {
	IAP2_IDENT_OK = 0,
	IAP2_IDENT_ERR_ARGUMENT = -1,
	IAP2_IDENT_ERR_CAPACITY = -2
};

/* Any text field may be NULL, which omits that parameter. A non-NULL but empty
 * string is encoded as a one-byte NUL value. */
struct iap2_ident_info {
	const char *name;
	const char *model;
	const char *manufacturer;
	const char *serial;
	const char *hardware_version;
	const char *firmware_version;
	const char *current_language;
	const char *const *ea_protocols;
	size_t ea_protocol_count;
};

/* Fills the identity advertised by this box. Strings point at static storage. */
void iap2_ident_defaults(struct iap2_ident_info *info);

/*
 * Encodes the parameter group into out. Nothing is written on failure, so a
 * short buffer is reported instead of producing a truncated group.
 * written may be NULL.
 */
int iap2_ident_build(const struct iap2_ident_info *info, uint8_t *out,
		     size_t capacity, size_t *written);

/* Total encoded size for a valid group, without writing anything. Returns
 * IAP2_IDENT_ERR_ARGUMENT for NULL info. */
int iap2_ident_measure(const struct iap2_ident_info *info, size_t *size);

#endif /* IAP2IDENT_H */
