/*
 * airplay_crypto.c -- HAP SRP-6a client over Android's system BoringSSL.
 *
 * The CarPlay accessory advertises the RFC 5054 3072-bit group with SHA-512.
 * Android 8.1 exports the required BIGNUM and SHA-512 primitives from
 * libcrypto.so, so this keeps the daemon dynamically linked to the on-device
 * crypto implementation instead of embedding a second TLS stack.
 */
#include "airplay_crypto.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern void iap2_log(const char *fmt, ...);

typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;

static void *crypto_handle;
static int load_attempted;
static int loaded;

static BIGNUM *(*bn_new)(void);
static void (*bn_free)(BIGNUM *);
static BN_CTX *(*bn_ctx_new)(void);
static void (*bn_ctx_free)(BN_CTX *);
static BIGNUM *(*bn_ctx_get)(BN_CTX *);
static BIGNUM *(*bn_bin2bn)(const unsigned char *, int, BIGNUM *);
static int (*bn_bn2bin)(const BIGNUM *, unsigned char *);
static int (*bn_set_word)(BIGNUM *, unsigned long);
static int (*bn_add)(BIGNUM *, const BIGNUM *, const BIGNUM *);
static int (*bn_mod_add)(BIGNUM *, const BIGNUM *, const BIGNUM *,
			 const BIGNUM *, BN_CTX *);
static int (*bn_mod_sub)(BIGNUM *, const BIGNUM *, const BIGNUM *,
			 const BIGNUM *, BN_CTX *);
static int (*bn_mod_mul)(BIGNUM *, const BIGNUM *, const BIGNUM *,
			 const BIGNUM *, BN_CTX *);
static int (*bn_mod_exp)(BIGNUM *, const BIGNUM *, const BIGNUM *,
			 const BIGNUM *, BN_CTX *);
static int (*rand_bytes)(unsigned char *, int);
static unsigned char *(*sha512)(const unsigned char *, size_t,
				unsigned char *);

typedef struct evp_md_st EVP_MD;
typedef struct evp_aead_st EVP_AEAD;
typedef struct evp_aead_ctx_st EVP_AEAD_CTX;

static int (*hkdf)(unsigned char *, size_t, const EVP_MD *,
                   const unsigned char *, size_t,
                   const unsigned char *, size_t,
                   const unsigned char *, size_t);
static const EVP_MD *(*evp_sha512)(void);
static void (*ed25519_keypair)(unsigned char *, unsigned char *);
static int (*ed25519_sign)(unsigned char *, const unsigned char *, size_t,
                           const unsigned char *);
static int (*ed25519_verify)(const unsigned char *, size_t,
                             const unsigned char *, const unsigned char *);
static void (*x25519_keypair)(unsigned char *, unsigned char *);
static int (*x25519_shared_key)(unsigned char *, const unsigned char *,
                                const unsigned char *);
static const EVP_AEAD *(*evp_aead_chacha20_poly1305)(void);
static size_t (*evp_aead_key_length)(const EVP_AEAD *);
static size_t (*evp_aead_nonce_length)(const EVP_AEAD *);
static EVP_AEAD_CTX *(*evp_aead_ctx_new)(const EVP_AEAD *, const unsigned char *,
                                         size_t, size_t);
static void (*evp_aead_ctx_free)(EVP_AEAD_CTX *);
static int (*evp_aead_ctx_seal)(const EVP_AEAD_CTX *, unsigned char *,
                                size_t *, size_t, const unsigned char *, size_t,
                                const unsigned char *, size_t,
                                const unsigned char *, size_t);
static int (*evp_aead_ctx_open)(const EVP_AEAD_CTX *, unsigned char *,
                                size_t *, size_t, const unsigned char *, size_t,
                                const unsigned char *, size_t,
                                const unsigned char *, size_t);

static const char N_HEX[] =
	"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
	"020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
	"4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
	"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
	"98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
	"9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
	"E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
	"995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
	"85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
	"F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
	"7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
	"E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

static void *required_symbol(const char *name)
{
	void *symbol = dlsym(crypto_handle, name);

	if (!symbol)
		iap2_log("airplay crypto: libcrypto missing symbol %s", name);
	return symbol;
}

int airplay_crypto_init(void)
{
	if (loaded)
		return 0;
	if (load_attempted)
		return -1;
	load_attempted = 1;
	crypto_handle = dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
	if (!crypto_handle) {
		iap2_log("airplay crypto: dlopen libcrypto.so failed: %s", dlerror());
		return -1;
	}
	bn_new = required_symbol("BN_new");
	bn_free = required_symbol("BN_free");
	bn_ctx_new = required_symbol("BN_CTX_new");
	bn_ctx_free = required_symbol("BN_CTX_free");
	bn_ctx_get = required_symbol("BN_CTX_get");
	bn_bin2bn = required_symbol("BN_bin2bn");
	bn_bn2bin = required_symbol("BN_bn2bin");
	bn_set_word = required_symbol("BN_set_word");
	bn_add = required_symbol("BN_add");
	bn_mod_add = required_symbol("BN_mod_add");
	bn_mod_sub = required_symbol("BN_mod_sub");
	bn_mod_mul = required_symbol("BN_mod_mul");
	bn_mod_exp = required_symbol("BN_mod_exp");
	rand_bytes = required_symbol("RAND_bytes");
	sha512 = required_symbol("SHA512");

	hkdf = required_symbol("HKDF");
	evp_sha512 = required_symbol("EVP_sha512");
	ed25519_keypair = required_symbol("ED25519_keypair");
	ed25519_sign = required_symbol("ED25519_sign");
	ed25519_verify = required_symbol("ED25519_verify");
	x25519_keypair = required_symbol("X25519_keypair");
	x25519_shared_key = required_symbol("X25519");
	evp_aead_chacha20_poly1305 = required_symbol("EVP_aead_chacha20_poly1305");
	evp_aead_key_length = required_symbol("EVP_AEAD_key_length");
	evp_aead_nonce_length = required_symbol("EVP_AEAD_nonce_length");
	evp_aead_ctx_new = required_symbol("EVP_AEAD_CTX_new");
	evp_aead_ctx_free = required_symbol("EVP_AEAD_CTX_free");
	evp_aead_ctx_seal = required_symbol("EVP_AEAD_CTX_seal");
	evp_aead_ctx_open = required_symbol("EVP_AEAD_CTX_open");
	if (!bn_new || !bn_free || !bn_ctx_new || !bn_ctx_free || !bn_ctx_get ||
	    !bn_bin2bn || !bn_bn2bin || !bn_set_word || !bn_add ||
	    !bn_mod_add || !bn_mod_sub || !bn_mod_mul || !bn_mod_exp ||
	    !rand_bytes || !sha512 || !hkdf || !evp_sha512 ||
	    !ed25519_keypair || !ed25519_sign || !ed25519_verify ||
	    !x25519_keypair || !x25519_shared_key ||
	    !evp_aead_chacha20_poly1305 || !evp_aead_key_length ||
	    !evp_aead_nonce_length || !evp_aead_ctx_new ||
	    !evp_aead_ctx_free || !evp_aead_ctx_seal || !evp_aead_ctx_open)
		return -1;
	loaded = 1;
	iap2_log("airplay crypto: system libcrypto ready for HAP pairing and control crypto");
	return 0;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
	size_t i;

	if (strlen(hex) != out_len * 2u)
		return -1;
	for (i = 0; i < out_len; i++) {
		unsigned value;
		if (sscanf(hex + i * 2u, "%2x", &value) != 1)
			return -1;
		out[i] = (uint8_t)value;
	}
	return 0;
}

static int bn_to_padded(const BIGNUM *value, uint8_t *out, size_t out_len)
{
	uint8_t raw[AIRPLAY_SRP_PUBLIC_BYTES];
	int width;

	if (!value || !out || out_len > sizeof(raw))
		return -1;
	width = bn_bn2bin(value, raw);
	if (width <= 0 || (size_t)width > out_len)
		return -1;
	memset(out, 0, out_len);
	memcpy(out + out_len - (size_t)width, raw, (size_t)width);
	return 0;
}

static int hash_bytes(const uint8_t *data, size_t len, uint8_t out[64])
{
	return sha512(data, len, out) ? 0 : -1;
}

int airplay_srp_client(const uint8_t *salt, size_t salt_len,
		       const uint8_t *server_public, size_t server_public_len,
		       struct airplay_srp_client_result *result)
{
	BIGNUM *n = NULL, *g = NULL, *k = NULL, *x = NULL, *v = NULL;
	BIGNUM *b = NULL, *a = NULL, *u = NULL, *k_mul_v = NULL;
	BIGNUM *base = NULL, *ux = NULL, *exponent = NULL, *secret = NULL;
	BN_CTX *ctx = NULL;
	uint8_t n_bytes[AIRPLAY_SRP_PUBLIC_BYTES];
	uint8_t padded_g[AIRPLAY_SRP_PUBLIC_BYTES];
	uint8_t hash_input[2u * AIRPLAY_SRP_PUBLIC_BYTES];
	uint8_t private_bytes[32];
	uint8_t inner_hash[64], prefix[128];
	uint8_t hash_n[64], hash_g[64], hash_user[64], hash_xor[64];
	uint8_t secret_raw[AIRPLAY_SRP_PUBLIC_BYTES];
	uint8_t proof_input[1024];
	uint8_t server_public_padded[AIRPLAY_SRP_PUBLIC_BYTES];
	int rc = -1;

	if (!loaded || !salt || !server_public || !result ||
	    salt_len != 16 || server_public_len > AIRPLAY_SRP_PUBLIC_BYTES ||
	    server_public_len == 0)
		return -1;
	if (hex_to_bytes(N_HEX, n_bytes, sizeof(n_bytes)) < 0)
		return -1;
	memset(padded_g, 0, sizeof(padded_g));
	padded_g[sizeof(padded_g) - 1] = 5;
	memcpy(server_public_padded + sizeof(server_public_padded) -
	       server_public_len, server_public, server_public_len);
	memset(server_public_padded, 0,
	       sizeof(server_public_padded) - server_public_len);

	memcpy(hash_input, n_bytes, sizeof(n_bytes));
	memcpy(hash_input + sizeof(n_bytes), padded_g, sizeof(padded_g));
	if (hash_bytes(hash_input, sizeof(hash_input), inner_hash) < 0)
		goto out;

	ctx = bn_ctx_new();
	if (!ctx)
		goto out;
	n = bn_bin2bn(n_bytes, sizeof(n_bytes), NULL);
	g = bn_new();
	k = bn_bin2bn(inner_hash, sizeof(inner_hash), NULL);
	b = bn_bin2bn(server_public_padded, sizeof(server_public_padded), NULL);
	base = bn_new();
	k_mul_v = bn_new();
	ux = bn_new();
	exponent = bn_new();
	secret = bn_new();
	if (!n || !g || !k || !b || !base ||
	    !bn_set_word(g, 5) || !k_mul_v || !ux || !exponent || !secret)
		goto out;

	if (rand_bytes(private_bytes, sizeof(private_bytes)) <= 0)
		goto out;
	a = bn_bin2bn(private_bytes, sizeof(private_bytes), NULL);
	if (!a)
		goto out;

	/* H(username + ':' + password) */
	memcpy(prefix, "Pair-Setup:3939", 15);
	if (hash_bytes(prefix, 15, inner_hash) < 0)
		goto out;
	{
		uint8_t x_input[16 + 64];
		memcpy(x_input, salt, salt_len);
		memcpy(x_input + salt_len, inner_hash, 64);
		if (hash_bytes(x_input, sizeof(x_input), inner_hash) < 0)
			goto out;
	}
	x = bn_bin2bn(inner_hash, 64, NULL);
	v = bn_new();
	if (!x || !v || !bn_mod_exp(v, g, x, n, ctx))
		goto out;

	if (!bn_mod_mul(k_mul_v, k, v, n, ctx))
		goto out;
	if (!bn_mod_sub(base, b, k_mul_v, n, ctx))
		goto out;

	/* A = g^a mod N */
	if (!bn_ctx_get(ctx))
		goto out;
	{
		BIGNUM *public_a = bn_ctx_get(ctx);
		if (!public_a || !bn_mod_exp(public_a, g, a, n, ctx) ||
		    bn_to_padded(public_a, result->public_key,
				 sizeof(result->public_key)) < 0)
			goto out;
		memcpy(hash_input, result->public_key, sizeof(result->public_key));
		memcpy(hash_input + sizeof(result->public_key),
		       server_public_padded, sizeof(server_public_padded));
		if (hash_bytes(hash_input, sizeof(hash_input), inner_hash) < 0)
			goto out;
		u = bn_bin2bn(inner_hash, 64, NULL);
		if (!u)
			goto out;
		if (!bn_mod_mul(ux, u, x, n, ctx) ||
		    !bn_add(exponent, a, ux) ||
		    !bn_mod_exp(secret, base, exponent, n, ctx) ||
		    bn_to_padded(secret, secret_raw, sizeof(secret_raw)) < 0)
			goto out;
	}
	{
		int width = bn_bn2bin(secret, secret_raw);
		if (width <= 0 || (size_t)width > sizeof(secret_raw))
			goto out;
		if (hash_bytes(secret_raw + sizeof(secret_raw) - (size_t)width,
			       (size_t)width, result->session_key) < 0)
			goto out;
	}

	if (hash_bytes(n_bytes, sizeof(n_bytes), hash_n) < 0 ||
	    hash_bytes((const uint8_t *)"\x05", 1, hash_g) < 0 ||
	    hash_bytes((const uint8_t *)"Pair-Setup", 10, hash_user) < 0)
		goto out;
	for (size_t i = 0; i < sizeof(hash_xor); i++)
		hash_xor[i] = hash_n[i] ^ hash_g[i];
	{
		size_t off = 0;
		memcpy(proof_input + off, hash_xor, 64); off += 64;
		memcpy(proof_input + off, hash_user, 64); off += 64;
		memcpy(proof_input + off, salt, salt_len); off += salt_len;
		memcpy(proof_input + off, result->public_key, 384); off += 384;
		memcpy(proof_input + off, server_public_padded, 384); off += 384;
		memcpy(proof_input + off, result->session_key, 64); off += 64;
		if (hash_bytes(proof_input, off, result->client_proof) < 0)
			goto out;
	}
	memcpy(proof_input, result->public_key, 384);
	memcpy(proof_input + 384, result->client_proof, 64);
	memcpy(proof_input + 448, result->session_key, 64);
	if (hash_bytes(proof_input, 512, result->server_proof) < 0)
		goto out;
	rc = 0;
out:
	if (ctx) {
		if (n) bn_free(n);
		if (g) bn_free(g);
		if (k) bn_free(k);
		if (x) bn_free(x);
		if (v) bn_free(v);
		if (b) bn_free(b);
		if (a) bn_free(a);
		if (u) bn_free(u);
		if (k_mul_v) bn_free(k_mul_v);
		if (base) bn_free(base);
		if (ux) bn_free(ux);
		if (exponent) bn_free(exponent);
		if (secret) bn_free(secret);
		bn_ctx_free(ctx);
	}
	if (rc)
		iap2_log("airplay crypto: SRP calculation failed");
	return rc;
}

int airplay_ed25519_generate(struct airplay_ed25519_keypair *keypair)
{
	if (!keypair || airplay_crypto_init() < 0)
		return -1;
	memset(keypair, 0, sizeof(*keypair));
	ed25519_keypair(keypair->public_key, keypair->private_key);
	return 0;
}

int airplay_ed25519_sign(const uint8_t private_key[AIRPLAY_ED25519_PRIVATE_BYTES],
			 const uint8_t *message, size_t message_len,
			 uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES])
{
	if (!private_key || (!message && message_len) || !signature ||
	    airplay_crypto_init() < 0)
		return -1;
	return ed25519_sign(signature, message, message_len,
			    private_key) == 1 ? 0 : -1;
}

int airplay_ed25519_verify(
	const uint8_t public_key[AIRPLAY_ED25519_PUBLIC_BYTES],
	const uint8_t *message, size_t message_len,
	const uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES])
{
	if (!public_key || (!message && message_len) || !signature ||
	    airplay_crypto_init() < 0)
		return -1;
	return ed25519_verify(message, message_len, signature, public_key) == 1 ? 0 : -1;
}

int airplay_x25519_generate(struct airplay_x25519_keypair *keypair)
{
	if (!keypair || airplay_crypto_init() < 0)
		return -1;
	memset(keypair, 0, sizeof(*keypair));
	x25519_keypair(keypair->public_key, keypair->private_key);
	return 0;
}

int airplay_x25519_shared(
	const uint8_t private_key[AIRPLAY_X25519_PRIVATE_BYTES],
	const uint8_t peer_public_key[AIRPLAY_X25519_PUBLIC_BYTES],
	uint8_t shared_secret[AIRPLAY_X25519_PUBLIC_BYTES])
{
	if (!private_key || !peer_public_key || !shared_secret ||
	    airplay_crypto_init() < 0)
		return -1;
	memset(shared_secret, 0, AIRPLAY_X25519_PUBLIC_BYTES);
	return x25519_shared_key(shared_secret, private_key, peer_public_key) == 1 ?
		0 : -1;
}

int airplay_hkdf_sha512(const uint8_t *input_key_material,
			size_t input_key_material_len,
			const uint8_t *salt, size_t salt_len,
			const uint8_t *info, size_t info_len,
			uint8_t *output, size_t output_len)
{
	const EVP_MD *digest;

	if ((!input_key_material && input_key_material_len) ||
	    (!salt && salt_len) || (!info && info_len) ||
	    (!output && output_len) || airplay_crypto_init() < 0)
		return -1;
	digest = evp_sha512();
	if (!digest)
		return -1;
	return hkdf(output, output_len, digest,
		    input_key_material, input_key_material_len,
		    salt, salt_len, info, info_len) == 1 ? 0 : -1;
}

static int chacha20_poly1305_crypt(
	int seal,
	const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
	const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
	const uint8_t *input, size_t input_len,
	const uint8_t *aad, size_t aad_len,
	uint8_t *output, size_t output_capacity, size_t *output_len)
{
	const EVP_AEAD *aead;
	EVP_AEAD_CTX *ctx;
	size_t written = 0;
	int result;

	if (!key || !nonce || (!input && input_len) || (!aad && aad_len) ||
	    (!output && output_capacity) || !output_len ||
	    airplay_crypto_init() < 0)
		return -1;
	*output_len = 0;
	aead = evp_aead_chacha20_poly1305();
	if (!aead || evp_aead_key_length(aead) != AIRPLAY_CHACHA_KEY_BYTES ||
	    evp_aead_nonce_length(aead) != AIRPLAY_CHACHA_NONCE_BYTES)
		return -1;
	ctx = evp_aead_ctx_new(aead, key, AIRPLAY_CHACHA_KEY_BYTES,
			       AIRPLAY_CHACHA_TAG_BYTES);
	if (!ctx)
		return -1;
	if (seal) {
		if (input_len > SIZE_MAX - AIRPLAY_CHACHA_TAG_BYTES ||
		    output_capacity < input_len + AIRPLAY_CHACHA_TAG_BYTES) {
			evp_aead_ctx_free(ctx);
			return -1;
		}
		result = evp_aead_ctx_seal(
			ctx, output, &written, output_capacity, nonce,
			AIRPLAY_CHACHA_NONCE_BYTES, input, input_len, aad, aad_len);
	} else {
		if (input_len < AIRPLAY_CHACHA_TAG_BYTES ||
		    output_capacity < input_len - AIRPLAY_CHACHA_TAG_BYTES) {
			evp_aead_ctx_free(ctx);
			return -1;
		}
		result = evp_aead_ctx_open(
			ctx, output, &written, output_capacity, nonce,
			AIRPLAY_CHACHA_NONCE_BYTES, input, input_len, aad, aad_len);
	}
	evp_aead_ctx_free(ctx);
	if (result != 1)
		return -1;
	*output_len = written;
	return 0;
}

int airplay_chacha20_poly1305_seal(
	const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
	const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
	const uint8_t *plaintext, size_t plaintext_len,
	const uint8_t *aad, size_t aad_len,
	uint8_t *output, size_t output_capacity, size_t *output_len)
{
	return chacha20_poly1305_crypt(1, key, nonce, plaintext, plaintext_len,
				       aad, aad_len, output, output_capacity,
				       output_len);
}

int airplay_chacha20_poly1305_open(
	const uint8_t key[AIRPLAY_CHACHA_KEY_BYTES],
	const uint8_t nonce[AIRPLAY_CHACHA_NONCE_BYTES],
	const uint8_t *ciphertext_and_tag, size_t ciphertext_and_tag_len,
	const uint8_t *aad, size_t aad_len,
	uint8_t *output, size_t output_capacity, size_t *output_len)
{
	return chacha20_poly1305_crypt(0, key, nonce, ciphertext_and_tag,
				       ciphertext_and_tag_len, aad, aad_len,
				       output, output_capacity, output_len);
}
