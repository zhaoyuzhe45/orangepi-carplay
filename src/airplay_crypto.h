#ifndef AIRPLAY_CRYPTO_H
#define AIRPLAY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_SRP_PUBLIC_BYTES 384
#define AIRPLAY_SHA512_BYTES 64
#define AIRPLAY_ED25519_PUBLIC_BYTES 32u
#define AIRPLAY_ED25519_PRIVATE_BYTES 64u
#define AIRPLAY_ED25519_SIGNATURE_BYTES 64u
#define AIRPLAY_X25519_PUBLIC_BYTES 32u
#define AIRPLAY_X25519_PRIVATE_BYTES 32u
#define AIRPLAY_CHACHA_KEY_BYTES 32u
#define AIRPLAY_CHACHA_NONCE_BYTES 12u
#define AIRPLAY_CHACHA_TAG_BYTES 16u

struct airplay_srp_client_result {
	uint8_t public_key[AIRPLAY_SRP_PUBLIC_BYTES];
	uint8_t client_proof[AIRPLAY_SHA512_BYTES];
	uint8_t server_proof[AIRPLAY_SHA512_BYTES];
	uint8_t session_key[AIRPLAY_SHA512_BYTES];
};

struct airplay_ed25519_keypair {
    uint8_t public_key[AIRPLAY_ED25519_PUBLIC_BYTES];
    uint8_t private_key[AIRPLAY_ED25519_PRIVATE_BYTES];
};

struct airplay_x25519_keypair {
    uint8_t public_key[AIRPLAY_X25519_PUBLIC_BYTES];
    uint8_t private_key[AIRPLAY_X25519_PRIVATE_BYTES];
};

int airplay_ed25519_generate(struct airplay_ed25519_keypair *keypair);
int airplay_ed25519_sign(const uint8_t private_key[AIRPLAY_ED25519_PRIVATE_BYTES],
                         const uint8_t *message, size_t message_len,
                         uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES]);
int airplay_ed25519_verify(
    const uint8_t public_key[AIRPLAY_ED25519_PUBLIC_BYTES],
    const uint8_t *message, size_t message_len,
    const uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES]);
int airplay_x25519_generate(struct airplay_x25519_keypair *keypair);
int airplay_x25519_shared(
    const uint8_t private_key[AIRPLAY_X25519_PRIVATE_BYTES],
    const uint8_t peer_public_key[AIRPLAY_X25519_PUBLIC_BYTES],
    uint8_t shared_secret[AIRPLAY_X25519_PUBLIC_BYTES]);
int airplay_hkdf_sha512(const uint8_t *input_key_material,
                        size_t input_key_material_len,
                        const uint8_t *salt, size_t salt_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t *output, size_t output_len);
int airplay_chacha20_poly1305_seal(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
    const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    uint8_t *output, size_t output_capacity, size_t *output_len);
int airplay_chacha20_poly1305_open(
    const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
    const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
    const uint8_t *ciphertext_and_tag, size_t ciphertext_and_tag_len,
    const uint8_t *aad, size_t aad_len,
    uint8_t *output, size_t output_capacity, size_t *output_len);
int airplay_crypto_init(void);
int airplay_srp_client(const uint8_t *salt, size_t salt_len,
		       const uint8_t *server_public, size_t server_public_len,
		       struct airplay_srp_client_result *result);

#endif