/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/security.h>.  Measures the rejection
 * cost of hash + AEAD + random-bytes wrappers when no backend is
 * wired (native_sim path).  Real-stack benches (AES-GCM round-
 * trip, SHA-256 throughput, hardware TRNG drain rate) live in the
 * per-(SoM, OS) baselines under E1M-<MPN>-<os>.yaml once HiL goes
 * online.
 */

#include <stddef.h>
#include <stdint.h>

#include "bench.h"

#include "alp/security.h"

static const uint8_t k_key16[16] = { 0 };
static const uint8_t k_iv12[12]  = { 0 };
static const uint8_t k_tag16[16] = { 0 };

void                 bench_security_main(void)
{
	/* alp_hash_open with no backend wired -- the wrapper hands off
     * to the active hash implementation which returns NULL on the
     * native_sim path.  Cheap; representative of apps that probe
     * for a SHA-256 backend at boot. */
	BENCH_RUN("alp_hash_open(SHA256)", 1000000, { (void)alp_hash_open(ALP_HASH_SHA256); });

	/* alp_hash_update on a NULL handle -- the wrapper's NULL guard.
     * Same shape as every accessor in the suite. */
	BENCH_RUN("alp_hash_update(NULL)", 1000000, { (void)alp_hash_update(NULL, NULL, 0u); });

	BENCH_RUN("alp_hash_finish(NULL)", 1000000, {
		uint8_t digest[32];
		size_t  got = 0u;
		(void)alp_hash_finish(NULL, digest, sizeof digest, &got);
	});

	/* AEAD round-trip on the stub backend.  open + encrypt + decrypt
     * each NOSUPPORT or NOT_READY -- collectively the price a caller
     * pays per "try AEAD, fall back to plaintext" decision when no
     * backend is wired. */
	BENCH_RUN("alp_aead_open(AES_128_GCM)", 1000000,
	          { (void)alp_aead_open(ALP_AEAD_AES_128_GCM, k_key16, sizeof k_key16); });

	BENCH_RUN("alp_aead_encrypt(NULL handle)", 1000000, {
		uint8_t ct[32]  = { 0 };
		uint8_t tag[16] = { 0 };
		(void)alp_aead_encrypt(NULL, k_iv12, sizeof k_iv12, NULL, 0u, NULL, 0u, ct, tag,
		                       sizeof tag);
	});

	BENCH_RUN("alp_aead_decrypt(NULL handle)", 1000000, {
		uint8_t pt[32] = { 0 };
		(void)alp_aead_decrypt(NULL, k_iv12, sizeof k_iv12, NULL, 0u, NULL, 0u, k_tag16,
		                       sizeof k_tag16, pt);
	});

	/* alp_random_bytes with NULL out -- INVAL pre-check before any
     * entropy work.  Validates the rejection-cost claim in the AEN
     * feature audit. */
	BENCH_RUN("alp_random_bytes(NULL out)", 1000000, { (void)alp_random_bytes(NULL, 16u); });
}
