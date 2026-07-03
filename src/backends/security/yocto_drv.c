/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto security driver-class backend.  Lifts the body of
 * src/yocto/security_yocto.c (the legacy v0.4 direct-impl OpenSSL
 * wrapper) into a registry-shaped backend behind the alp_security
 * dispatcher's ops vtable (#33) -- one vtable covering all three
 * primitives (hash, AEAD, random_bytes), mirroring the PSA-backed
 * zephyr_drv sibling.  Registered at priority 100 with vendor
 * "linux"; the sw_fallback backend (priority 0) still wins on builds
 * where OpenSSL is absent (this TU is only compiled when CMake's
 * pkg_check_modules finds `libssl libcrypto`).
 *
 * Selected on any silicon (silicon_ref "*") because OpenSSL is pure
 * userspace -- kernel crypto offload (if any) is transparent.
 *
 * Why OpenSSL and not MbedTLS on the Yocto side
 * ---------------------------------------------
 * libmosquitto links against OpenSSL by default on a Yocto image,
 * so picking OpenSSL here keeps the two paths sharing the same TLS
 * runtime (smaller image, one CA bundle, one entropy source).  The
 * Zephyr backend uses MbedTLS PSA because that is what Zephyr's MQTT
 * + BLE host stacks link against on-target.  Apps don't see the
 * difference -- both backends sit behind `<alp/security.h>`.
 *
 * AEAD algorithm mapping
 * ----------------------
 *   ALP_AEAD_AES_128_GCM        -> EVP_aes_128_gcm()
 *   ALP_AEAD_AES_256_GCM        -> EVP_aes_256_gcm()
 *   ALP_AEAD_CHACHA20_POLY1305  -> EVP_chacha20_poly1305()
 *
 * Hash algorithm mapping
 * ----------------------
 *   ALP_HASH_SHA256             -> EVP_sha256()
 *   ALP_HASH_SHA384             -> EVP_sha384()
 *   ALP_HASH_SHA512             -> EVP_sha512()
 *
 * TRNG path
 * ---------
 * random_bytes wraps OpenSSL's RAND_bytes.  OpenSSL's default RAND
 * backend is a DRBG seeded from /dev/urandom + getrandom(2);
 * suitable for keys, IVs, and nonces per the <alp/security.h>
 * contract.  No hardware-RNG ioctl path -- on Yocto images that
 * have an HW TRNG, the rng-tools daemon feeds /dev/random which
 * OpenSSL consumes transparently.
 *
 * Semantics parity with the zephyr_drv (PSA) backend:
 *   - tag mismatch on decrypt   -> ALP_ERR_IO (integrity failure)
 *   - bad alg / key length      -> ALP_ERR_INVAL at open
 *   - hash_finish releases the backend blob on BOTH success and
 *     failure (the dispatcher frees its handle slot after finish
 *     unconditionally, so holding the blob would leak it).
 */

#if defined(__linux__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

#include "security_ops.h"

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state (heap-boxed behind state->be_data)   */
/* ------------------------------------------------------------------ */

struct hash_be {
	EVP_MD_CTX *ctx;
	size_t      digest_len;
};

struct aead_be {
	uint8_t key[32]; /* longest supported key (AES-256 / ChaCha20) */
	size_t  key_len;
};

static const EVP_MD *hash_alg_md(alp_hash_alg_t alg, size_t *digest_len)
{
	switch (alg) {
	case ALP_HASH_SHA256:
		if (digest_len) *digest_len = 32;
		return EVP_sha256();
	case ALP_HASH_SHA384:
		if (digest_len) *digest_len = 48;
		return EVP_sha384();
	case ALP_HASH_SHA512:
		if (digest_len) *digest_len = 64;
		return EVP_sha512();
	default:
		return NULL;
	}
}

static const EVP_CIPHER *aead_cipher(alp_aead_alg_t alg, size_t *required_key_len)
{
	switch (alg) {
	case ALP_AEAD_AES_128_GCM:
		if (required_key_len) *required_key_len = 16;
		return EVP_aes_128_gcm();
	case ALP_AEAD_AES_256_GCM:
		if (required_key_len) *required_key_len = 32;
		return EVP_aes_256_gcm();
	case ALP_AEAD_CHACHA20_POLY1305:
		if (required_key_len) *required_key_len = 32;
		return EVP_chacha20_poly1305();
	default:
		return NULL;
	}
}

/* ================================================================== */
/* Hash ops                                                            */
/* ================================================================== */

static alp_status_t
y_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	size_t        dlen = 0;
	const EVP_MD *md   = hash_alg_md(alg, &dlen);
	if (md == NULL) return ALP_ERR_INVAL;

	struct hash_be *be = (struct hash_be *)calloc(1, sizeof(*be));
	if (be == NULL) return ALP_ERR_NOMEM;

	be->ctx = EVP_MD_CTX_new();
	if (be->ctx == NULL) {
		free(be);
		return ALP_ERR_NOMEM;
	}
	if (EVP_DigestInit_ex(be->ctx, md, NULL) != 1) {
		EVP_MD_CTX_free(be->ctx);
		free(be);
		return ALP_ERR_IO;
	}
	be->digest_len = dlen;
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
}

static alp_status_t y_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL || be->ctx == NULL) return ALP_ERR_NOT_READY;
	if (len == 0) return ALP_OK;
	if (EVP_DigestUpdate(be->ctx, data, len) != 1) return ALP_ERR_IO;
	return ALP_OK;
}

static void hash_be_free(alp_hash_backend_state_t *state, struct hash_be *be)
{
	EVP_MD_CTX_free(be->ctx);
	be->ctx = NULL;
	free(be);
	state->be_data = NULL;
}

static alp_status_t y_hash_finish(alp_hash_backend_state_t *state,
                                  uint8_t                  *digest_out,
                                  size_t                    digest_cap,
                                  size_t                   *digest_len)
{
	if (digest_len != NULL) *digest_len = 0;
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL || be->ctx == NULL) return ALP_ERR_NOT_READY;
	if (digest_cap < be->digest_len) {
		/* The dispatcher frees its handle slot after finish either
		 * way -- release the blob here too so it doesn't leak. */
		hash_be_free(state, be);
		return ALP_ERR_INVAL;
	}

	unsigned int n  = 0;
	int          rc = EVP_DigestFinal_ex(be->ctx, digest_out, &n);
	hash_be_free(state, be);
	if (rc != 1) return ALP_ERR_IO;
	if (digest_len != NULL) *digest_len = (size_t)n;
	return ALP_OK;
}

static void y_hash_close(alp_hash_backend_state_t *state)
{
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL) return;
	hash_be_free(state, be);
}

/* ================================================================== */
/* AEAD ops                                                            */
/* ================================================================== */

static alp_status_t y_aead_open(alp_aead_alg_t            alg,
                                const uint8_t            *key,
                                size_t                    key_len,
                                alp_aead_backend_state_t *state,
                                alp_capabilities_t       *caps_out)
{
	(void)caps_out;
	size_t            required = 0;
	const EVP_CIPHER *c        = aead_cipher(alg, &required);
	if (c == NULL) return ALP_ERR_INVAL;
	if (key == NULL || key_len != required) return ALP_ERR_INVAL;

	struct aead_be *be = (struct aead_be *)calloc(1, sizeof(*be));
	if (be == NULL) return ALP_ERR_NOMEM;

	be->key_len = key_len;
	memcpy(be->key, key, key_len);
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
}

static alp_status_t y_aead_encrypt(alp_aead_backend_state_t *state,
                                   const uint8_t            *iv,
                                   size_t                    iv_len,
                                   const uint8_t            *aad,
                                   size_t                    aad_len,
                                   const uint8_t            *plain,
                                   size_t                    plain_len,
                                   uint8_t                  *cipher_out,
                                   uint8_t                  *tag_out,
                                   size_t                    tag_len)
{
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || iv_len == 0) return ALP_ERR_INVAL;
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (plain == NULL && plain_len > 0) return ALP_ERR_INVAL;
	if (tag_out == NULL || tag_len < 16) return ALP_ERR_INVAL;

	const EVP_CIPHER *c = aead_cipher(state->alg, NULL);
	if (c == NULL) return ALP_ERR_NOSUPPORT;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return ALP_ERR_NOMEM;

	alp_status_t status = ALP_ERR_IO;

	if (EVP_EncryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv_len, NULL) != 1) goto out;
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, be->key, iv) != 1) goto out;

	int outlen = 0;
	if (aad_len > 0) {
		if (EVP_EncryptUpdate(ctx, NULL, &outlen, aad, (int)aad_len) != 1) goto out;
	}
	if (plain_len > 0) {
		if (EVP_EncryptUpdate(ctx, cipher_out, &outlen, plain, (int)plain_len) != 1) goto out;
	}
	int finallen = 0;
	if (EVP_EncryptFinal_ex(ctx, cipher_out + outlen, &finallen) != 1) goto out;

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, (int)tag_len, tag_out) != 1) goto out;

	status = ALP_OK;

out:
	EVP_CIPHER_CTX_free(ctx);
	return status;
}

static alp_status_t y_aead_decrypt(alp_aead_backend_state_t *state,
                                   const uint8_t            *iv,
                                   size_t                    iv_len,
                                   const uint8_t            *aad,
                                   size_t                    aad_len,
                                   const uint8_t            *cipher,
                                   size_t                    cipher_len,
                                   const uint8_t            *tag,
                                   size_t                    tag_len,
                                   uint8_t                  *plain_out)
{
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || iv_len == 0) return ALP_ERR_INVAL;
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (cipher == NULL && cipher_len > 0) return ALP_ERR_INVAL;
	if (plain_out == NULL && cipher_len > 0) return ALP_ERR_INVAL;
	if (tag == NULL || tag_len < 16) return ALP_ERR_INVAL;

	const EVP_CIPHER *c = aead_cipher(state->alg, NULL);
	if (c == NULL) return ALP_ERR_NOSUPPORT;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return ALP_ERR_NOMEM;

	alp_status_t status = ALP_ERR_IO;

	if (EVP_DecryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv_len, NULL) != 1) goto out;
	if (EVP_DecryptInit_ex(ctx, NULL, NULL, be->key, iv) != 1) goto out;

	int outlen = 0;
	if (aad_len > 0) {
		if (EVP_DecryptUpdate(ctx, NULL, &outlen, aad, (int)aad_len) != 1) goto out;
	}
	if (cipher_len > 0) {
		if (EVP_DecryptUpdate(ctx, plain_out, &outlen, cipher, (int)cipher_len) != 1) goto out;
	}

	/* Set expected tag *before* final.  EVP_DecryptFinal_ex returns
	 * 1 on tag match, 0 on mismatch (no error queue entry on a
	 * clean tag mismatch). */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, (int)tag_len, (void *)(uintptr_t)tag) != 1)
		goto out;

	int finallen = 0;
	int rc       = EVP_DecryptFinal_ex(ctx, plain_out + outlen, &finallen);
	if (rc != 1) {
		/* Tag mismatch is the "tampered ciphertext" case per the
		 * header docs -- ALP_ERR_IO so callers can handle it as an
		 * integrity failure (matches the PSA backend's mapping of
		 * PSA_ERROR_INVALID_SIGNATURE). */
		status = ALP_ERR_IO;
		goto out;
	}

	status = ALP_OK;

out:
	EVP_CIPHER_CTX_free(ctx);
	return status;
}

static void y_aead_close(alp_aead_backend_state_t *state)
{
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL) return;
	/* Wipe key material before freeing -- header contract. */
	OPENSSL_cleanse(be->key, sizeof(be->key));
	free(be);
	state->be_data = NULL;
}

/* ================================================================== */
/* Random (stateless)                                                  */
/* ================================================================== */

static alp_status_t y_random_bytes(uint8_t *out, size_t len)
{
	if (len == 0) return ALP_OK;
	/* RAND_bytes returns 1 on success, 0 or -1 on failure.
	 * Failures here mean the entropy source itself is broken; map
	 * to IO so callers can distinguish from arg validation. */
	if (RAND_bytes(out, (int)len) != 1) return ALP_ERR_IO;
	return ALP_OK;
}

/* ---------- Registration ---------- */

static const alp_security_ops_t _ops = {
	.hash_open    = y_hash_open,
	.hash_update  = y_hash_update,
	.hash_finish  = y_hash_finish,
	.hash_close   = y_hash_close,
	.aead_open    = y_aead_open,
	.aead_encrypt = y_aead_encrypt,
	.aead_decrypt = y_aead_decrypt,
	.aead_close   = y_aead_close,
	.random_bytes = y_random_bytes,
};

ALP_BACKEND_REGISTER(security,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
