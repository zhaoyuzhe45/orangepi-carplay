#ifndef AIRPLAY_LEGACY_H
#define AIRPLAY_LEGACY_H
#include <stddef.h>
#include <stdint.h>
struct airplay_legacy_cipher;
int airplay_legacy_keys(const uint8_t session_key[16], uint64_t connection_id,
                        uint8_t key[16], uint8_t iv[16]);
struct airplay_legacy_cipher *airplay_legacy_new(const uint8_t key[16], const uint8_t iv[16]);
int airplay_legacy_crypt(struct airplay_legacy_cipher *cipher, const uint8_t *input,
                         uint8_t *output, size_t length);
void airplay_legacy_free(struct airplay_legacy_cipher *cipher);
#endif
