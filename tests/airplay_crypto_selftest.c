#include "airplay_crypto.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void iap2_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	putchar('\n');
	va_end(args);
}

static int hex_equal(const uint8_t *actual, const char *expected, size_t len)
{
	char text[129];
	size_t i;
	if (len * 2 + 1 > sizeof(text))
		return 0;
	for (i = 0; i < len; i++)
		sprintf(text + i * 2, "%02x", actual[i]);
	return strcmp(text, expected) == 0;
}

int main(void)
{
	static const char expected_hkdf[] =
		"832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb";
	uint8_t ikm[22], salt[13], info[10], hkdf_out[42];
	struct airplay_ed25519_keypair ed;
	struct airplay_x25519_keypair a, b;
	uint8_t sig[64], shared_a[32], shared_b[32];
	uint8_t key[32], nonce[12], plain[18], sealed[34], opened[18];
	size_t sealed_len = 0, opened_len = 0;
	int ok = 1;

	memset(ikm, 0x0b, sizeof(ikm));
	for (size_t i = 0; i < sizeof(salt); i++) salt[i] = (uint8_t)i;
	for (size_t i = 0; i < sizeof(info); i++) info[i] = (uint8_t)(0xf0 + i);
	ok &= airplay_crypto_init() == 0;
	ok &= airplay_hkdf_sha512(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info),
	                           hkdf_out, sizeof(hkdf_out)) == 0;
	ok &= hex_equal(hkdf_out, expected_hkdf, sizeof(hkdf_out));
	printf("hkdf=%s\n", hex_equal(hkdf_out, expected_hkdf, sizeof(hkdf_out)) ? "PASS" : "FAIL");

	ok &= airplay_ed25519_generate(&ed) == 0;
	ok &= airplay_ed25519_sign(ed.private_key,
	                           (const uint8_t *)"selftest", 8, sig) == 0;
	ok &= airplay_ed25519_verify(ed.public_key,
	                             (const uint8_t *)"selftest", 8, sig) == 0;
	printf("ed25519=%s\n", airplay_ed25519_verify(ed.public_key,
	                                      (const uint8_t *)"selftest", 8, sig) == 0 ? "PASS" : "FAIL");

	ok &= airplay_x25519_generate(&a) == 0;
	ok &= airplay_x25519_generate(&b) == 0;
	ok &= airplay_x25519_shared(a.private_key, b.public_key, shared_a) == 0;
	ok &= airplay_x25519_shared(b.private_key, a.public_key, shared_b) == 0;
	printf("x25519=%s\n", memcmp(shared_a, shared_b, 32) == 0 ? "PASS" : "FAIL");

	memset(key, 0x42, sizeof(key));
	memset(nonce, 0x24, sizeof(nonce));
	memcpy(plain, "control-channel", 15);
	ok &= airplay_chacha20_poly1305_seal(key, nonce, plain, 15, NULL, 0,
	                                    sealed, sizeof(sealed), &sealed_len) == 0;
	ok &= airplay_chacha20_poly1305_open(key, nonce, sealed, sealed_len, NULL, 0,
	                                    opened, sizeof(opened), &opened_len) == 0;
	printf("chacha=%s\n", opened_len == 15 && memcmp(plain, opened, 15) == 0 ? "PASS" : "FAIL");
	return ok && memcmp(shared_a, shared_b, 32) == 0 ? 0 : 1;
}
