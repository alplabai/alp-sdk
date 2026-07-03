/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dispatcher fall-through tests for <alp/security.h> (issue #239).
 *
 * The SE-CryptoCell backend can decline an open() with
 * ALP_ERR_NOSUPPORT (SHA-384/512, or every alg with the send seam
 * compiled out).  Before the fix the dispatcher bound ONE backend at
 * open and never re-selected, so the decline surfaced to the app as
 * a hard failure even though the PSA backend could serve the request.
 *
 * The SE backend itself is silicon-gated, so this suite exercises the
 * dispatcher logic with two FAKE backends registered into the real
 * `security` class section:
 *
 *   - fake_hw  (priority 200)  declines every open with NOSUPPORT,
 *                              except SHA-512 where it fails ALP_ERR_IO
 *                              (to prove hard errors are NOT masked).
 *   - fake_sw  (priority 150)  accepts and implements trivial,
 *                              deterministic hash/AEAD ops.
 *
 * Both outrank the production zephyr_drv ("*", 100) entry, so the
 * dispatcher's ranked walk under test is fake_hw -> fake_sw.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/backend.h"
#include "alp/peripheral.h"
#include "alp/security.h"
#include "alp/soc_caps.h"

#include "security_ops.h" /* internal dispatcher<->backend ABI */

/* ------------------------------------------------------------------ */
/* Call accounting, reset before every test                            */
/* ------------------------------------------------------------------ */

static int g_hw_hash_opens;
static int g_hw_aead_opens;
static int g_sw_hash_opens;
static int g_sw_aead_opens;
static int g_sw_encrypts;

/* ------------------------------------------------------------------ */
/* fake_hw -- top-ranked backend that declines everything at open      */
/* ------------------------------------------------------------------ */

static alp_status_t
hw_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)state;
	(void)caps_out;
	g_hw_hash_opens++;
	/* SHA-512 stands in for a REAL failure (bus fault, SE dead...):
	 * the dispatcher must surface it, not fall through past it. */
	if (alg == ALP_HASH_SHA512) {
		return ALP_ERR_IO;
	}
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t hw_aead_open(alp_aead_alg_t            alg,
                                 const uint8_t            *key,
                                 size_t                    key_len,
                                 alp_aead_backend_state_t *state,
                                 alp_capabilities_t       *caps_out)
{
	(void)alg;
	(void)key;
	(void)key_len;
	(void)state;
	(void)caps_out;
	g_hw_aead_opens++;
	return ALP_ERR_NOSUPPORT;
}

static const alp_security_ops_t _hw_ops = {
	.hash_open = hw_hash_open,
	.aead_open = hw_aead_open,
};

ALP_BACKEND_REGISTER(security,
                     fake_hw,
                     {
                         .silicon_ref = "*",
                         .vendor      = "test_hw",
                         .base_caps   = 0u,
                         .priority    = 200,
                         .ops         = &_hw_ops,
                         .probe       = NULL,
                     });

/* ------------------------------------------------------------------ */
/* fake_sw -- next-ranked backend with trivial deterministic crypto    */
/* ------------------------------------------------------------------ */

/* Single-context fakes: each test drives at most one live handle. */
static bool   g_sw_hash_in_use;
static size_t g_sw_hash_fed;
static bool   g_sw_aead_in_use;

static alp_status_t
sw_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	g_sw_hash_opens++;
	if (g_sw_hash_in_use) {
		return ALP_ERR_NOMEM;
	}
	g_sw_hash_in_use = true;
	g_sw_hash_fed    = 0u;
	state->alg       = alg;
	state->be_data   = &g_sw_hash_fed;
	return ALP_OK;
}

static alp_status_t sw_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
	(void)data;
	if (state->be_data == NULL || !g_sw_hash_in_use) {
		return ALP_ERR_NOT_READY;
	}
	g_sw_hash_fed += len;
	return ALP_OK;
}

/* "Digest" = 32 bytes of the total fed byte count (mod 256): enough to
 * prove update+finish routed through THIS backend with the right data. */
static alp_status_t sw_hash_finish(alp_hash_backend_state_t *state,
                                   uint8_t                  *digest_out,
                                   size_t                    digest_cap,
                                   size_t                   *digest_len)
{
	if (state->be_data == NULL || !g_sw_hash_in_use) {
		return ALP_ERR_NOT_READY;
	}
	if (digest_out == NULL || digest_cap < 32u) {
		return ALP_ERR_INVAL;
	}
	memset(digest_out, (int)(g_sw_hash_fed & 0xFFu), 32u);
	if (digest_len != NULL) {
		*digest_len = 32u;
	}
	g_sw_hash_in_use = false;
	state->be_data   = NULL;
	return ALP_OK;
}

static void sw_hash_close(alp_hash_backend_state_t *state)
{
	g_sw_hash_in_use = false;
	state->be_data   = NULL;
}

static alp_status_t sw_aead_open(alp_aead_alg_t            alg,
                                 const uint8_t            *key,
                                 size_t                    key_len,
                                 alp_aead_backend_state_t *state,
                                 alp_capabilities_t       *caps_out)
{
	(void)caps_out;
	g_sw_aead_opens++;
	if (key == NULL || key_len == 0u || g_sw_aead_in_use) {
		return ALP_ERR_INVAL;
	}
	g_sw_aead_in_use = true;
	state->alg       = alg;
	state->be_data   = &g_sw_aead_in_use;
	return ALP_OK;
}

/* "Cipher" = plaintext XOR 0x5A; "tag" = 0xA5 fill.  Deterministic and
 * trivially invertible so the round-trip proves the routing. */
static alp_status_t sw_aead_encrypt(alp_aead_backend_state_t *state,
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
	(void)iv;
	(void)iv_len;
	(void)aad;
	(void)aad_len;
	if (state->be_data == NULL || !g_sw_aead_in_use) {
		return ALP_ERR_NOT_READY;
	}
	g_sw_encrypts++;
	for (size_t i = 0; i < plain_len; ++i) {
		cipher_out[i] = plain[i] ^ 0x5Au;
	}
	memset(tag_out, 0xA5, tag_len);
	return ALP_OK;
}

static alp_status_t sw_aead_decrypt(alp_aead_backend_state_t *state,
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
	(void)iv;
	(void)iv_len;
	(void)aad;
	(void)aad_len;
	if (state->be_data == NULL || !g_sw_aead_in_use) {
		return ALP_ERR_NOT_READY;
	}
	for (size_t i = 0; i < tag_len; ++i) {
		if (tag[i] != 0xA5u) {
			return ALP_ERR_IO; /* tamper signal, mirrors the contract */
		}
	}
	for (size_t i = 0; i < cipher_len; ++i) {
		plain_out[i] = cipher[i] ^ 0x5Au;
	}
	return ALP_OK;
}

static void sw_aead_close(alp_aead_backend_state_t *state)
{
	g_sw_aead_in_use = false;
	state->be_data   = NULL;
}

static const alp_security_ops_t _sw_ops = {
	.hash_open    = sw_hash_open,
	.hash_update  = sw_hash_update,
	.hash_finish  = sw_hash_finish,
	.hash_close   = sw_hash_close,
	.aead_open    = sw_aead_open,
	.aead_encrypt = sw_aead_encrypt,
	.aead_decrypt = sw_aead_decrypt,
	.aead_close   = sw_aead_close,
};

ALP_BACKEND_REGISTER(security,
                     fake_sw,
                     {
                         .silicon_ref = "*",
                         .vendor      = "test_sw",
                         .base_caps   = 0u,
                         .priority    = 150,
                         .ops         = &_sw_ops,
                         .probe       = NULL,
                     });

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static void reset_fakes(void *fixture)
{
	(void)fixture;
	g_hw_hash_opens  = 0;
	g_hw_aead_opens  = 0;
	g_sw_hash_opens  = 0;
	g_sw_aead_opens  = 0;
	g_sw_encrypts    = 0;
	g_sw_hash_in_use = false;
	g_sw_hash_fed    = 0u;
	g_sw_aead_in_use = false;
}

ZTEST_SUITE(alp_security_fallback, NULL, NULL, reset_fakes, NULL, NULL);

/* NOSUPPORT at hash_open falls through to the next-ranked backend
 * and the whole stream then routes through it. */
ZTEST(alp_security_fallback, test_hash_open_falls_through_nosupport)
{
	alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
	zassert_not_null(h, "open must fall through fake_hw's NOSUPPORT");
	zassert_equal(g_hw_hash_opens, 1, "top-ranked backend tried first");
	zassert_equal(g_sw_hash_opens, 1, "next-ranked backend bound");

	const uint8_t msg[3] = { 'a', 'b', 'c' };
	zassert_equal(alp_hash_update(h, msg, sizeof(msg)), ALP_OK);

	uint8_t digest[32] = { 0 };
	size_t  dlen       = 0;
	zassert_equal(alp_hash_finish(h, digest, sizeof(digest), &dlen), ALP_OK);
	zassert_equal(dlen, 32u);
	for (size_t i = 0; i < sizeof(digest); ++i) {
		zassert_equal(digest[i], 3u, "fake_sw served update+finish");
	}
}

/* A non-NOSUPPORT open error is a REAL failure: the dispatcher must
 * surface it, never mask it with a lower-ranked backend. */
ZTEST(alp_security_fallback, test_hash_open_hard_error_not_masked)
{
	alp_hash_t *h = alp_hash_open(ALP_HASH_SHA512);
	zassert_is_null(h);
	zassert_equal(alp_last_error(), ALP_ERR_IO, "fake_hw's IO error surfaced");
	zassert_equal(g_hw_hash_opens, 1);
	zassert_equal(g_sw_hash_opens, 0, "no fall-through past a hard error");
}

/* Same fall-through contract on the AEAD surface. */
ZTEST(alp_security_fallback, test_aead_open_falls_through_nosupport)
{
	const uint8_t key[16] = { 0x42 };
	alp_aead_t   *a       = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
	zassert_not_null(a, "open must fall through fake_hw's NOSUPPORT");
	zassert_equal(g_hw_aead_opens, 1);
	zassert_equal(g_sw_aead_opens, 1);

	const uint8_t iv[12]     = { 1, 2, 3 };
	const uint8_t plain[8]   = "seceight";
	uint8_t       cipher[8]  = { 0 };
	uint8_t       tag[16]    = { 0 };
	uint8_t       decoded[8] = { 0 };

	zassert_equal(alp_aead_encrypt(
	                  a, iv, sizeof(iv), NULL, 0, plain, sizeof(plain), cipher, tag, sizeof(tag)),
	              ALP_OK,
	              "NULL aad with aad_len 0 is legitimate");
	zassert_equal(
	    alp_aead_decrypt(
	        a, iv, sizeof(iv), NULL, 0, cipher, sizeof(cipher), tag, sizeof(tag), decoded),
	    ALP_OK);
	zassert_mem_equal(decoded, plain, sizeof(plain), "round-trip through fake_sw");
	alp_aead_close(a);
}

/* aad == NULL with aad_len > 0 is contradictory: rejected in the
 * dispatcher before any backend can translate the NULL (issue #245). */
ZTEST(alp_security_fallback, test_aead_null_aad_with_len_rejected)
{
	const uint8_t key[16] = { 0x42 };
	alp_aead_t   *a       = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
	zassert_not_null(a);

	const uint8_t iv[12]    = { 0 };
	const uint8_t plain[4]  = { 0 };
	uint8_t       cipher[4] = { 0 };
	uint8_t       out[4]    = { 0 };
	uint8_t       tag[16]   = { 0 };

	zassert_equal(
	    alp_aead_encrypt(a, iv, sizeof(iv), NULL, 5, plain, sizeof(plain), cipher, tag, 16),
	    ALP_ERR_INVAL);
	zassert_equal(g_sw_encrypts, 0, "backend never reached");
	zassert_equal(
	    alp_aead_decrypt(a, iv, sizeof(iv), NULL, 5, cipher, sizeof(cipher), tag, 16, out),
	    ALP_ERR_INVAL);
	alp_aead_close(a);
}

/* alp_backend_select_next walks the ranked candidates: strictly
 * descending, no repeats, terminating in NULL. */
ZTEST(alp_security_fallback, test_select_next_ranked_walk)
{
	const alp_backend_t *b = alp_backend_select("security", ALP_SOC_REF_STR);
	zassert_not_null(b);
	zassert_equal(b->priority, 200, "fake_hw ranks first");

	const alp_backend_t *n = alp_backend_select_next("security", ALP_SOC_REF_STR, b);
	zassert_not_null(n);
	zassert_equal(n->priority, 150, "fake_sw ranks second");

	/* Walk to exhaustion: priorities must strictly decrease (all the
	 * registered security backends carry distinct priorities), and the
	 * walk must terminate.  Bound the loop at 16 steps -- far beyond
	 * any realistic backend count -- so a cycle fails loudly. */
	int steps = 0;
	while (n != NULL && steps < 16) {
		const alp_backend_t *next = alp_backend_select_next("security", ALP_SOC_REF_STR, n);
		if (next != NULL) {
			zassert_true(next->priority < n->priority, "ranked walk descends");
		}
		n = next;
		steps++;
	}
	zassert_is_null(n, "walk terminates");

	/* prev == NULL degenerates to plain selection. */
	zassert_equal_ptr(alp_backend_select_next("security", ALP_SOC_REF_STR, NULL), b);
}
