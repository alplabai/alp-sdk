/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real-backend AEAD coverage for <alp/security.h> -- GHSA-7xh2-9pcg-r824.
 *
 * tests/zephyr/security/ builds with no MbedTLS, so alp_aead_open()
 * there always returns NULL (ALP_ERR_NOSUPPORT) and the encrypt/decrypt
 * bodies in src/backends/security/zephyr_drv.c never run.  This suite
 * closes that gap: MbedTLS's PSA Crypto is wired in (see prj.conf), so
 * alp_aead_open() binds the real priority-100 zephyr_drv backend and
 * every call below drives the actual PSA multipart AEAD path.
 *
 * The suite (like tests/zephyr/security/) runs on CONFIG_MAIN_STACK_SIZE
 * = 4096 -- the exact figure the advisory calls out as a common SDK
 * default.  Before the fix, the backend's own 4,112-byte on-stack
 * scratch buffer overflowed a thread this size on every real AEAD call;
 * simply reaching a passing assertion below is the regression proof.
 * All test-owned buffers up to 8 KiB live in .bss (not on the stack),
 * so the stack budget stays reserved for the code path under test.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/security.h"

#define MAX_LEN 8192u

static uint8_t g_plain[MAX_LEN];
static uint8_t g_cipher[MAX_LEN];
static uint8_t g_decoded[MAX_LEN];

ZTEST_SUITE(alp_security_mbedtls_aead, NULL, NULL, NULL, NULL, NULL);

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; ++i) {
		buf[i] = (uint8_t)(seed + i);
	}
}

/*
 * AES-128-GCM round-trip through a REAL opened handle for one plaintext
 * length.  Covers the Acceptance-Criteria size matrix: empty, small,
 * exactly 4,096 (the old ceiling), and greater than 4,096 (the ceiling
 * this fix removes -- previously a hard ALP_ERR_NOSUPPORT).
 */
static void roundtrip_gcm128(size_t len)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x11u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a, "real PSA backend must open a real AEAD handle");

	uint8_t iv[12];
	uint8_t aad[8];

	fill_pattern(iv, sizeof(iv), 0x22u);
	fill_pattern(aad, sizeof(aad), 0x33u);
	fill_pattern(g_plain, len, 0x44u);
	memset(g_cipher, 0, len);
	memset(g_decoded, 0, len);

	uint8_t tag[16] = { 0 };

	zassert_equal(
	    alp_aead_encrypt(
	        a, iv, sizeof(iv), aad, sizeof(aad), g_plain, len, g_cipher, tag, sizeof(tag)),
	    ALP_OK,
	    "encrypt len=%zu",
	    len);

	zassert_equal(
	    alp_aead_decrypt(
	        a, iv, sizeof(iv), aad, sizeof(aad), g_cipher, len, tag, sizeof(tag), g_decoded),
	    ALP_OK,
	    "decrypt len=%zu",
	    len);

	if (len > 0) {
		zassert_mem_equal(g_decoded, g_plain, len, "round-trip len=%zu", len);
	}

	alp_aead_close(a);
}

ZTEST(alp_security_mbedtls_aead, test_roundtrip_empty)
{
	roundtrip_gcm128(0);
}

ZTEST(alp_security_mbedtls_aead, test_roundtrip_small)
{
	roundtrip_gcm128(37);
}

ZTEST(alp_security_mbedtls_aead, test_roundtrip_exactly_4096)
{
	roundtrip_gcm128(4096);
}

/* The old code rejected this with an undocumented ALP_ERR_NOSUPPORT
 * ceiling.  Multipart PSA AEAD has no such limit -- this must succeed. */
ZTEST(alp_security_mbedtls_aead, test_roundtrip_over_4096_ceiling_lifted)
{
	roundtrip_gcm128(8192);
}

/* Second algorithm the backend declares in aead_alg_meta(). */
ZTEST(alp_security_mbedtls_aead, test_roundtrip_chacha20_poly1305)
{
	uint8_t key[32];

	fill_pattern(key, sizeof(key), 0x88u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_CHACHA20_POLY1305, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t plain[128];
	uint8_t cipher[128]  = { 0 };
	uint8_t decoded[128] = { 0 };
	uint8_t tag[16]      = { 0 };

	fill_pattern(iv, sizeof(iv), 0x99u);
	fill_pattern(plain, sizeof(plain), 0xAAu);

	zassert_equal(alp_aead_encrypt(
	                  a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, sizeof(tag)),
	              ALP_OK);
	zassert_equal(
	    alp_aead_decrypt(
	        a, iv, sizeof(iv), NULL, 0, cipher, sizeof(cipher), tag, sizeof(tag), decoded),
	    ALP_OK);
	zassert_mem_equal(decoded, plain, sizeof(plain));

	alp_aead_close(a);
}

/* Acceptance Criteria: "Tampered tags return the documented failure
 * without exposing plaintext."  The multipart rewrite streams plaintext
 * into plain_out during psa_aead_update(), before psa_aead_verify() has
 * checked the tag -- z_aead_decrypt() must wipe that partial output on
 * every failure path, not just report the error. */
ZTEST(alp_security_mbedtls_aead, test_tamper_tag_rejected_no_plaintext_exposed)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x55u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t plain[64];
	uint8_t cipher[64] = { 0 };
	uint8_t tag[16]    = { 0 };

	fill_pattern(iv, sizeof(iv), 0x66u);
	fill_pattern(plain, sizeof(plain), 0x77u);

	zassert_equal(alp_aead_encrypt(
	                  a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, sizeof(tag)),
	              ALP_OK);

	tag[0] ^= 0xFFu; /* tamper */

	uint8_t decoded[64];

	memset(decoded, 0xCCu, sizeof(decoded)); /* sentinel */
	zassert_equal(
	    alp_aead_decrypt(
	        a, iv, sizeof(iv), NULL, 0, cipher, sizeof(cipher), tag, sizeof(tag), decoded),
	    ALP_ERR_IO,
	    "tampered tag must be rejected, undefined output discarded");

	uint8_t zero[64] = { 0 };

	zassert_mem_equal(
	    decoded, zero, sizeof(decoded), "plain_out must be wiped on tag mismatch, never leaked");

	alp_aead_close(a);
}

/*
 * Adversarial-review defect #2 (tag_len divergence): all three security
 * backends must agree that tag_len must be exactly 16 B -- reject any
 * other value with ALP_ERR_INVAL, on both encrypt and decrypt, before any
 * PSA setup runs (i.e. even a bogus tag_len must not leave cipher_out
 * touched).
 */
ZTEST(alp_security_mbedtls_aead, test_encrypt_rejects_non_16_tag_len)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x12u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t plain[32];
	uint8_t cipher[32] = { 0 };
	uint8_t tag[24]    = { 0 }; /* oversized -- only 16 round-trips */

	fill_pattern(iv, sizeof(iv), 0x13u);
	fill_pattern(plain, sizeof(plain), 0x14u);

	zassert_equal(
	    alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, 24),
	    ALP_ERR_INVAL,
	    "tag_len != 16 must be rejected on encrypt");
	zassert_equal(
	    alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, 8),
	    ALP_ERR_INVAL,
	    "short tag_len must be rejected on encrypt");

	alp_aead_close(a);
}

ZTEST(alp_security_mbedtls_aead, test_decrypt_rejects_non_16_tag_len)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x15u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t plain[32];
	uint8_t cipher[32]  = { 0 };
	uint8_t decoded[32] = { 0 };
	uint8_t tag[24]     = { 0 };

	fill_pattern(iv, sizeof(iv), 0x16u);
	fill_pattern(plain, sizeof(plain), 0x17u);

	zassert_equal(alp_aead_encrypt(
	                  a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, sizeof(tag)),
	              ALP_ERR_INVAL,
	              "setup: tag_len 24 already rejected on encrypt");

	/* Encrypt with a real 16 B tag so decrypt has a valid tag buffer to
	 * probe the tag_len argument against. */
	zassert_equal(
	    alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, 16),
	    ALP_OK);

	zassert_equal(
	    alp_aead_decrypt(a, iv, sizeof(iv), NULL, 0, cipher, sizeof(cipher), tag, 24, decoded),
	    ALP_ERR_INVAL,
	    "tag_len != 16 must be rejected on decrypt");
	zassert_equal(
	    alp_aead_decrypt(a, iv, sizeof(iv), NULL, 0, cipher, sizeof(cipher), tag, 8, decoded),
	    ALP_ERR_INVAL,
	    "short tag_len must be rejected on decrypt");

	alp_aead_close(a);
}

/*
 * Adversarial-review defect #3 (NULL buffer guard): NULL plain/cipher with
 * a non-zero length must be rejected at the dispatcher, before it can ever
 * reach a backend's psa_aead_update() / EVP_*Update() and NULL-deref.
 */
ZTEST(alp_security_mbedtls_aead, test_encrypt_rejects_null_plain_with_nonzero_len)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x18u);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t cipher[32] = { 0 };
	uint8_t tag[16]    = { 0 };

	fill_pattern(iv, sizeof(iv), 0x19u);

	zassert_equal(alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, NULL, 32, cipher, tag, sizeof(tag)),
	              ALP_ERR_INVAL,
	              "NULL plain with plain_len > 0 must be rejected");

	/* NULL plain with plain_len == 0 stays legitimate (empty payload). */
	zassert_equal(alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, NULL, 0, cipher, tag, sizeof(tag)),
	              ALP_OK,
	              "NULL plain with plain_len == 0 must still succeed");

	alp_aead_close(a);
}

ZTEST(alp_security_mbedtls_aead, test_decrypt_rejects_null_cipher_with_nonzero_len)
{
	uint8_t key[16];

	fill_pattern(key, sizeof(key), 0x1Au);
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));

	zassert_not_null(a);

	uint8_t iv[12];
	uint8_t tag[16]     = { 0 };
	uint8_t cipher_dump = 0; /* cipher_out is still mandatory on encrypt */
	uint8_t decoded[32] = { 0 };

	fill_pattern(iv, sizeof(iv), 0x1Bu);

	zassert_equal(alp_aead_decrypt(a, iv, sizeof(iv), NULL, 0, NULL, 32, tag, sizeof(tag), decoded),
	              ALP_ERR_INVAL,
	              "NULL cipher with cipher_len > 0 must be rejected");

	/* NULL cipher with cipher_len == 0 is the legitimate AAD-only case;
	 * exercise it with a real AAD-only tag from an empty-payload encrypt.
	 * cipher_out itself is still a mandatory (non-optional) buffer on
	 * encrypt -- only the ciphertext INPUT to decrypt is optional when
	 * cipher_len == 0. */
	zassert_equal(
	    alp_aead_encrypt(a, iv, sizeof(iv), NULL, 0, NULL, 0, &cipher_dump, tag, sizeof(tag)),
	    ALP_OK);
	zassert_equal(alp_aead_decrypt(a, iv, sizeof(iv), NULL, 0, NULL, 0, tag, sizeof(tag), decoded),
	              ALP_OK,
	              "NULL cipher with cipher_len == 0 must still succeed");

	alp_aead_close(a);
}
