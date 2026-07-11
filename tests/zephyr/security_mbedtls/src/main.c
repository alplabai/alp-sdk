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
