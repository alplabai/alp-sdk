/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/security.h> under native_sim.  No MbedTLS
 * PSA stack -> the wrapper falls back to NOSUPPORT and we verify
 * the public-API contract for that path.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/security.h"

ZTEST_SUITE(alp_security, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_security, test_hash_open_no_backend_returns_null)
{
	alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
	zassert_is_null(h);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_security, test_hash_lifecycle_null_handle_safe)
{
	uint8_t buf[32] = { 0 };
	zassert_equal(alp_hash_update(NULL, buf, 4), ALP_ERR_NOT_READY);
	zassert_equal(alp_hash_finish(NULL, buf, sizeof(buf), NULL), ALP_ERR_NOT_READY);
	alp_hash_close(NULL);
}

ZTEST(alp_security, test_aead_open_null_key_invalid)
{
	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 0);
	zassert_is_null(a);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_security, test_aead_open_no_backend_returns_null)
{
	uint8_t     key[16] = { 0 };
	alp_aead_t *a       = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
	zassert_is_null(a);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_security, test_aead_lifecycle_null_handle_safe)
{
	uint8_t iv[12]  = { 0 };
	uint8_t out[16] = { 0 };
	uint8_t tag[16] = { 0 };
	zassert_equal(alp_aead_encrypt(NULL, iv, sizeof(iv), NULL, 0, NULL, 0, out, tag, sizeof(tag)),
	              ALP_ERR_NOT_READY);
	zassert_equal(alp_aead_decrypt(NULL, iv, sizeof(iv), NULL, 0, NULL, 0, tag, sizeof(tag), out),
	              ALP_ERR_NOT_READY);
	alp_aead_close(NULL);
}

ZTEST(alp_security, test_random_bytes_no_backend_errors)
{
	uint8_t buf[16] = { 0 };
	zassert_equal(alp_random_bytes(buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_security, test_random_bytes_null_invalid)
{
	zassert_equal(alp_random_bytes(NULL, 16), ALP_ERR_INVAL);
}
