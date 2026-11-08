/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2979: the Secure Enclave reads and writes caller buffers behind the
 * CPU D-cache.  This test compiles the REAL se_cryptocell.c with its wire
 * paths on, intercepts sys_cache_*() and se_service_send_request(), and
 * checks the maintenance order: inputs cleaned, outputs clean+invalidated,
 * then (after the SE ran) outputs invalidated -- whole lines only, with the
 * partial edge lines clean+invalidated so a neighbour's dirty bytes survive.
 */

#define CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL           1
#define CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM 1

#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/ztest.h>

enum ev_kind { EV_FLUSH, EV_FLUSH_INVD, EV_INVD, EV_SEND };

struct ev {
	enum ev_kind kind;
	uintptr_t    addr;
	size_t       len;
};

#define LINE 32u

static struct ev g_ev[64];
static size_t    g_nev;

static int rec(enum ev_kind k, void *a, size_t n)
{
	zassert_true(g_nev < ARRAY_SIZE(g_ev), "event log full");
	g_ev[g_nev++] = (struct ev){ k, (uintptr_t)a, n };
	return 0;
}

/* Defined AFTER <zephyr/cache.h> so the real header's declarations are
 * untouched and only se_cryptocell.c's calls are redirected. */
#define sys_cache_data_flush_range(a, n)          rec(EV_FLUSH, (a), (n))
#define sys_cache_data_flush_and_invd_range(a, n) rec(EV_FLUSH_INVD, (a), (n))
#define sys_cache_data_invd_range(a, n)           rec(EV_INVD, (a), (n))
#define sys_cache_data_line_size_get()            ((size_t)LINE)

#include "../../../../src/backends/security/se_cryptocell.c"

int se_service_get_rnd_num(uint8_t *out, uint16_t len)
{
	(void)out;
	(void)len;
	return -1;
}

int se_service_send_request(uint32_t *packet, uint32_t size)
{
	(void)packet;
	(void)size;
	rec(EV_SEND, NULL, 0u);
	return 0;
}

/* The fake SE reports success; resp_error_code is the last packet field and
 * stays 0 from the backend's memset. */

static void reset_state(void *fixture)
{
	(void)fixture;
	memset(g_se_hash_pool, 0, sizeof(g_se_hash_pool));
	memset(g_se_aead_pool, 0, sizeof(g_se_aead_pool));
	g_nev = 0u;
}

ZTEST_SUITE(se_cryptocell_cache, NULL, NULL, reset_state, NULL, NULL);

static size_t send_index(void)
{
	for (size_t i = 0; i < g_nev; i++) {
		if (g_ev[i].kind == EV_SEND) {
			return i;
		}
	}
	zassert_unreachable("SE request never sent");
	return 0;
}

/* Does some event of `kind` in [from, to) fully cover [a, a+n)? */
static bool covered(enum ev_kind kind, size_t from, size_t to, const void *a, size_t n)
{
	for (size_t i = from; i < to; i++) {
		if (g_ev[i].kind == kind && g_ev[i].addr <= (uintptr_t)a &&
		    g_ev[i].addr + g_ev[i].len >= (uintptr_t)a + n) {
			return true;
		}
	}
	return false;
}

/* Output post-op: the invalidated union is the whole buffer, a plain INVD
 * only ever spans whole lines, and edge lines are clean+invalidated. */
static void check_output_post(size_t snd, const uint8_t *out, size_t n)
{
	uintptr_t covered_bytes = 0;

	for (size_t i = snd + 1; i < g_nev; i++) {
		zassert_not_equal(g_ev[i].kind, EV_FLUSH, "no clean after the SE ran");
		if (g_ev[i].addr >= (uintptr_t)out && g_ev[i].addr < (uintptr_t)out + n) {
			covered_bytes += g_ev[i].len;
			if (g_ev[i].kind == EV_INVD) {
				zassert_equal(g_ev[i].addr % LINE, 0u, "INVD start line-aligned");
				zassert_equal(g_ev[i].len % LINE, 0u, "INVD whole lines only");
			}
		}
	}
	zassert_equal(covered_bytes, n, "output fully invalidated after the SE wrote it");
}

ZTEST(se_cryptocell_cache, test_hash_finish_cache_order)
{
	struct se_hash_be *be = se_hash_acquire();
	zassert_not_null(be);
	memset(be->buf, 0xA5, 70);
	be->used                       = 70u;
	alp_hash_backend_state_t state = { .alg = ALP_HASH_SHA256, .be_data = be };
	/* Unaligned 32-byte digest destination spanning a line boundary. */
	static uint8_t __aligned(64) region[128];
	uint8_t       *digest = &region[8];
	uint8_t       *input  = be->buf;
	size_t         dlen;

	zassert_equal(_ops.hash_finish(&state, digest, 32u, &dlen), ALP_OK);

	size_t snd = send_index();
	zassert_true(covered(EV_FLUSH, 0, snd, input, 70), "input cleaned before the SE reads");
	zassert_true(covered(EV_FLUSH_INVD, 0, snd, digest, 32), "output clean+invd before");
	check_output_post(snd, digest, 32);
}

ZTEST(se_cryptocell_cache, test_gcm_encrypt_and_decrypt_cache_order)
{
	static uint8_t     __aligned(64) out[192];
	static uint8_t     __aligned(64) tag[64];
	uint8_t            iv[12] = { 1 }, aad[7] = { 2 }, plain[100] = { 3 };
	struct se_aead_be *be = se_aead_acquire();
	zassert_not_null(be);
	be->key_len                     = 16u;
	alp_aead_backend_state_t state  = { .alg = ALP_AEAD_AES_128_GCM, .be_data = be };
	uint8_t                 *cipher = &out[4]; /* both edges partial: 4..104 */
	uint8_t                 *tagp   = &tag[3];

	zassert_equal(_ops.aead_encrypt(&state, iv, 12, aad, 7, plain, 100, cipher, tagp, 16), ALP_OK);
	size_t snd = send_index();
	zassert_true(covered(EV_FLUSH, 0, snd, be->key, 16), "key cleaned");
	zassert_true(covered(EV_FLUSH, 0, snd, iv, 12), "iv cleaned");
	zassert_true(covered(EV_FLUSH, 0, snd, aad, 7), "aad cleaned");
	zassert_true(covered(EV_FLUSH, 0, snd, plain, 100), "plaintext cleaned");
	zassert_true(covered(EV_FLUSH_INVD, 0, snd, cipher, 100), "ciphertext clean+invd before");
	zassert_true(covered(EV_FLUSH_INVD, 0, snd, tagp, 16), "tag clean+invd before");
	check_output_post(snd, cipher, 100);
	check_output_post(snd, tagp, 16);

	g_nev        = 0u;
	uint8_t *dec = &out[100];
	zassert_equal(_ops.aead_decrypt(&state, iv, 12, aad, 7, plain, 100, tagp, 16, dec), ALP_OK);
	snd = send_index();
	zassert_true(covered(EV_FLUSH, 0, snd, plain, 100), "ciphertext cleaned");
	zassert_true(covered(EV_FLUSH, 0, snd, tagp, 16), "tag cleaned (input on decrypt)");
	zassert_true(covered(EV_FLUSH_INVD, 0, snd, dec, 100), "plaintext clean+invd before");
	check_output_post(snd, dec, 100);
}
