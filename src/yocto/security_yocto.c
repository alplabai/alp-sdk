/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace cryptography backend for <alp/security.h>'s
 * alp_hash_* / alp_aead_* / alp_random_bytes surface.
 *
 * Binds against OpenSSL's EVP_* API.  OpenSSL ships on every
 * canonical Yocto image (oe-core's `openssl` recipe) and on every
 * Debian/Ubuntu host (`libssl-dev`); the SDK's CMake gates this
 * file behind `pkg_check_modules(libssl libcrypto)` so workspaces
 * without OpenSSL on the sysroot fall back to the NOSUPPORT stubs
 * via the ALP_VENDOR_OVERRIDES_SECURITY macro.
 *
 * Why OpenSSL and not MbedTLS on the Yocto side
 * ---------------------------------------------
 * libmosquitto links against OpenSSL by default on a Yocto image,
 * so picking OpenSSL here keeps the two paths sharing the same TLS
 * runtime (smaller image, one CA bundle, one entropy source).  The
 * Zephyr backend uses MbedTLS because MbedTLS is what Zephyr's MQTT
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
 * alp_random_bytes wraps OpenSSL's RAND_bytes.  OpenSSL's default
 * RAND backend is a DRBG seeded from /dev/urandom + getrandom(2);
 * suitable for keys, IVs, and nonces per the <alp/security.h>
 * contract.  No hardware-RNG ioctl path -- on Yocto images that
 * have an HW TRNG, the rng-tools daemon feeds /dev/random which
 * OpenSSL consumes transparently.
 */

#if !defined(__linux__)
#error "security_yocto.c (yocto backend) requires a Linux target"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "alp/security.h"
#include "alp/peripheral.h"
#include "alp_internal.h"

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

struct alp_hash {
	EVP_MD_CTX    *ctx;
	alp_hash_alg_t alg;
	size_t         digest_len;
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

alp_hash_t *alp_hash_open(alp_hash_alg_t alg)
{
	size_t        dlen = 0;
	const EVP_MD *md   = hash_alg_md(alg, &dlen);
	if (md == NULL) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	struct alp_hash *h = calloc(1, sizeof(*h));
	if (h == NULL) {
		alp_internal_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->ctx = EVP_MD_CTX_new();
	if (h->ctx == NULL) {
		free(h);
		alp_internal_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	if (EVP_DigestInit_ex(h->ctx, md, NULL) != 1) {
		EVP_MD_CTX_free(h->ctx);
		free(h);
		alp_internal_set_last_error(ALP_ERR_IO);
		return NULL;
	}
	h->alg        = alg;
	h->digest_len = dlen;
	return h;
}

alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len)
{
	if (h == NULL || h->ctx == NULL) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (len == 0) return ALP_OK;
	if (EVP_DigestUpdate(h->ctx, data, len) != 1) return ALP_ERR_IO;
	return ALP_OK;
}

alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap,
                             size_t *digest_len)
{
	if (digest_len != NULL) *digest_len = 0;
	if (h == NULL || h->ctx == NULL) return ALP_ERR_NOT_READY;
	if (digest_out == NULL) return ALP_ERR_INVAL;
	if (digest_cap < h->digest_len) return ALP_ERR_INVAL;

	unsigned int n = 0;
	if (EVP_DigestFinal_ex(h->ctx, digest_out, &n) != 1) {
		return ALP_ERR_IO;
	}
	if (digest_len != NULL) *digest_len = (size_t)n;
	/* Implicit close on success per the header contract. */
	EVP_MD_CTX_free(h->ctx);
	h->ctx = NULL;
	free(h);
	return ALP_OK;
}

void alp_hash_close(alp_hash_t *h)
{
	if (h == NULL) return;
	if (h->ctx != NULL) {
		EVP_MD_CTX_free(h->ctx);
		h->ctx = NULL;
	}
	free(h);
}

/* ------------------------------------------------------------------ */
/* AEAD                                                                */
/* ------------------------------------------------------------------ */

struct alp_aead {
	alp_aead_alg_t alg;
	uint8_t        key[32]; /* longest supported key (AES-256 / ChaCha20) */
	size_t         key_len;
};

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

alp_aead_t *alp_aead_open(alp_aead_alg_t alg, const uint8_t *key, size_t key_len)
{
	size_t            required = 0;
	const EVP_CIPHER *c        = aead_cipher(alg, &required);
	if (c == NULL) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	if (key == NULL || key_len != required) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	struct alp_aead *a = calloc(1, sizeof(*a));
	if (a == NULL) {
		alp_internal_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	a->alg     = alg;
	a->key_len = key_len;
	memcpy(a->key, key, key_len);
	return a;
}

alp_status_t alp_aead_encrypt(alp_aead_t *a, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                              size_t aad_len, const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher_out, uint8_t *tag_out, size_t tag_len)
{
	if (a == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || iv_len == 0) return ALP_ERR_INVAL;
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (plain == NULL && plain_len > 0) return ALP_ERR_INVAL;
	if (cipher_out == NULL && plain_len > 0) return ALP_ERR_INVAL;
	if (tag_out == NULL || tag_len < 16) return ALP_ERR_INVAL;

	const EVP_CIPHER *c = aead_cipher(a->alg, NULL);
	if (c == NULL) return ALP_ERR_NOSUPPORT;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return ALP_ERR_NOMEM;

	alp_status_t status = ALP_ERR_IO;

	if (EVP_EncryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv_len, NULL) != 1) goto out;
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, a->key, iv) != 1) goto out;

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

alp_status_t alp_aead_decrypt(alp_aead_t *a, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                              size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len, uint8_t *plain_out)
{
	if (a == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || iv_len == 0) return ALP_ERR_INVAL;
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (cipher == NULL && cipher_len > 0) return ALP_ERR_INVAL;
	if (plain_out == NULL && cipher_len > 0) return ALP_ERR_INVAL;
	if (tag == NULL || tag_len < 16) return ALP_ERR_INVAL;

	const EVP_CIPHER *c = aead_cipher(a->alg, NULL);
	if (c == NULL) return ALP_ERR_NOSUPPORT;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return ALP_ERR_NOMEM;

	alp_status_t status = ALP_ERR_IO;

	if (EVP_DecryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv_len, NULL) != 1) goto out;
	if (EVP_DecryptInit_ex(ctx, NULL, NULL, a->key, iv) != 1) goto out;

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
         * header docs -- distinguish from generic IO so callers can
         * handle it as an integrity failure. */
		status = ALP_ERR_IO;
		goto out;
	}

	status = ALP_OK;

out:
	EVP_CIPHER_CTX_free(ctx);
	return status;
}

void alp_aead_close(alp_aead_t *a)
{
	if (a == NULL) return;
	/* Wipe key material before freeing -- header contract. */
	OPENSSL_cleanse(a->key, sizeof(a->key));
	free(a);
}

/* ------------------------------------------------------------------ */
/* TRNG                                                                */
/* ------------------------------------------------------------------ */

alp_status_t alp_random_bytes(uint8_t *out, size_t len)
{
	if (out == NULL && len > 0) return ALP_ERR_INVAL;
	if (len == 0) return ALP_OK;
	/* RAND_bytes returns 1 on success, 0 or -1 on failure.
     * Failures here mean the entropy source itself is broken; map
     * to IO so callers can distinguish from arg validation. */
	if (RAND_bytes(out, (int)len) != 1) return ALP_ERR_IO;
	return ALP_OK;
}
