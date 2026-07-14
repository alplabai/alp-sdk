/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #737 (fix-round follow-up): boundary + mutation-discriminating
 * tests for the CryptoCell hash-update staging-buffer guard in
 * src/backends/security/se_cryptocell.c's se_hash_update().
 *
 * The first round of this fix shipped a suite that locally reimplemented
 * BOTH the pre-#737 guard and the post-#737 guard and asserted they
 * disagreed on a wraparound input -- a tautology about C's `size_t`
 * arithmetic, not a claim about the shipped backend. Reverting the
 * production fix left that suite green.
 *
 * This suite instead compiles the REAL src/backends/security/
 * se_cryptocell.c translation unit (via the #include below -- the same
 * technique tests/unit/rpc_zephyr_backend/src/test_rpc_zephyr_backend.c
 * and tests/yocto/rpc_yocto_self_close.c use) and calls the SHIPPED
 * se_hash_update() through the backend's own vtable (`_ops`, file-scope
 * `static` in se_cryptocell.c and therefore visible here after the
 * #include, exactly like `struct se_hash_be` / `se_hash_acquire()` /
 * `g_se_hash_pool` below).
 *
 * se_cryptocell.c is gated behind CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL,
 * which `depends on` the AEN801/E8-only hal_alif Kconfig symbol
 * HAS_ALIF_SE_SERVICES and so can never be selected on native_sim
 * through the real Kconfig/zephyr_library path -- this test instead
 * fakes the macro directly at the preprocessor level (mirrors
 * test_rpc_zephyr_backend.c's CONFIG_ALP_SDK_RPC fake). The sibling
 * CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM stays UNDEFINED: it
 * gates the AES/SHA/CMAC/CCM/GCM/ChaCha wire paths, which need further
 * hal_alif packet-struct headers this test does not fake, and
 * se_hash_update() itself does not need it -- only se_hash_open() (which
 * this test never calls) and se_hash_finish() do.
 *
 * With the seam off, se_hash_open() declines every algorithm
 * unconditionally, so a handle is acquired directly via the also
 * file-scope se_hash_acquire() instead of through open().
 *
 * A fake, lowest-priority "security" backend is registered below to give
 * se_hash_delegate()'s se_hash_fallback_ops() (se_cryptocell.c) a
 * delegation target -- mirrors the registration pattern in
 * tests/zephyr/security_fallback/src/main.c. Its hash_update()
 * deliberately never dereferences `data`, only counts `len`: see
 * test_wrap_defeats_naive_check_delegates_without_crash for why that
 * matters.
 */

/* Faked purely at the preprocessor level -- no real
 * ALP_SDK_SECURITY_SE_CRYPTOCELL Kconfig symbol exists in this image
 * (see this directory's CMakeLists.txt for why CONFIG_ALP_SDK is
 * deliberately never set here). Toggles ONLY se_cryptocell.c's own
 * top-of-file `#if defined(...)` body guard; CONFIG_ALP_SDK_SECURITY_
 * SE_CRYPTOCELL_SEND_SEAM is intentionally left undefined (see header
 * comment above). */
#define CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL 1

#include "../../../../src/backends/security/se_cryptocell.c"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

/* ------------------------------------------------------------------ */
/* Fake PSA-shaped fallback backend -- se_hash_delegate()'s target      */
/* ------------------------------------------------------------------ */

static bool   g_fake_open_called;
static size_t g_fake_update_calls; /* # of hash_update() invocations since reset_state() */
static size_t g_fake_last_len;     /* `len` of the MOST RECENT call; data never read */

static alp_status_t
fake_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	g_fake_open_called = true;
	state->alg         = alg;
	state->be_data     = NULL;
	return ALP_OK;
}

/* Deliberately does NOT touch `data`: se_hash_update() forwards its full,
 * possibly attacker-controlled `len` here unchanged once it decides to
 * delegate, and a real PSA/mbedtls implementation would read that many
 * bytes from `data`. This fake only RECORDS `len` (never sums it -- a
 * running sum can itself wrap, which would just relocate the problem this
 * suite is testing), so a delegation with an enormous len is observable
 * (g_fake_last_len) WITHOUT this test needing an actual len-sized buffer --
 * exactly what lets test_wrap_defeats_naive_check_delegates_without_crash
 * use a near-SIZE_MAX length safely on the FIXED code path while still
 * crashing loudly if the guard regresses to the pre-#737 expression.
 *
 * se_hash_delegate() (se_cryptocell.c) calls this TWICE when be->used != 0
 * at delegation time: once to replay the already-buffered bytes, once more
 * (from se_hash_update() itself, after delegate() returns) to forward the
 * triggering call's own (data, len) -- see g_fake_update_calls below. */
static alp_status_t
fake_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
	(void)state;
	(void)data;
	g_fake_update_calls++;
	g_fake_last_len = len;
	return ALP_OK;
}

static void fake_hash_close(alp_hash_backend_state_t *state)
{
	(void)state;
}

static const alp_security_ops_t _fake_ops = {
	.hash_open   = fake_hash_open,
	.hash_update = fake_hash_update,
	.hash_close  = fake_hash_close,
};

ALP_BACKEND_REGISTER(security,
                     test_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "test_fallback",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = &_fake_ops,
                         .probe       = NULL,
                     });

/* Linked against se_random_bytes()'s unconditional call (se_cryptocell.c) --
 * the whole-archive `used`+`retain` section keeps se_random_bytes() (and
 * hence this symbol reference) alive even though no test below invokes it. */
int se_service_get_rnd_num(uint8_t *out, uint16_t len)
{
	(void)out;
	(void)len;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static void reset_state(void *fixture)
{
	(void)fixture;
	memset(g_se_hash_pool, 0, sizeof(g_se_hash_pool));
	g_fake_open_called  = false;
	g_fake_update_calls = 0u;
	g_fake_last_len     = 0u;
}

ZTEST_SUITE(se_cryptocell_hash_bounds, NULL, NULL, reset_state, NULL, NULL);

/* Mutation-discriminating case (the reason this suite exists): the
 * staging buffer already carries 10 legitimately-buffered bytes
 * (be->used == 10, well under capacity) when a NEW update() call
 * supplies a length close to SIZE_MAX -- attacker/caller controlled,
 * e.g. a corrupted or malicious length field upstream of this backend.
 *
 * FIXED code (alp_size_range_valid(), never computes used + len): sees
 * len does not fit the remaining `capacity - used` headroom, delegates
 * to the fake fallback registered above, which forwards `len` WITHOUT
 * touching `data` -- se_hash_update() returns ALP_OK and this test
 * observes the full length was forwarded. No crash: the fixed guard
 * never reaches the memcpy() below with this input.
 *
 * PRE-#737 code (`be->used + len > sizeof(be->buf)`): 10 + (SIZE_MAX - 5)
 * wraps size_t to 4, which is NOT greater than the buffer's capacity, so
 * the naive guard wrongly concludes the input fits and falls into
 * `memcpy(be->buf + be->used, data, len)` -- copying (SIZE_MAX - 5)
 * bytes starting from a real 16-byte stack array. That read runs off the
 * end of `data` almost immediately and crashes the test binary.
 * Reverting ONLY the se_hash_update() guard back to the pre-#737
 * expression (keeping this test) crashes this test with no further
 * ztest output -- twister's last "START -" log line names it. */
ZTEST(se_cryptocell_hash_bounds, test_wrap_defeats_naive_check_delegates_without_crash)
{
	struct se_hash_be *be = se_hash_acquire();
	zassert_not_null(be, "pool has a free slot");
	be->used = 10u; /* bytes already legitimately buffered by an earlier update() */

	alp_hash_backend_state_t state = {
		.alg     = ALP_HASH_SHA256,
		.be_data = be,
	};

	uint8_t      data[16] = { 0 };
	const size_t len      = SIZE_MAX - 5u; /* used(10) + len wraps size_t to 4 */

	const alp_status_t rc = _ops.hash_update(&state, data, len);

	zassert_equal(rc, ALP_OK, "delegation must succeed, not hard-fail");
	zassert_true(g_fake_open_called, "must have delegated to the fallback backend");
	/* Two calls: se_hash_delegate()'s replay of the 10 pre-buffered bytes,
	 * then se_hash_update()'s own forward of THIS call's (data, len). The
	 * last one recorded is the forward -- and it must carry the exact,
	 * un-truncated, un-wrapped `len` this test passed in. */
	zassert_equal(g_fake_update_calls, 2u, "replay (buffered bytes) + forward (this call)");
	zassert_equal(g_fake_last_len, len, "the full (unwrapped) length must reach the fallback");
	zassert_true(be->delegated, "handle must be marked delegated after the migration");

	_ops.hash_close(&state);
}

/* Sanity / boundary check -- does NOT discriminate the #737 bug: the
 * pre- and post-fix expressions agree an input this large must delegate
 * since there is no wraparound at this size. A single update() exactly
 * one byte over the staging buffer's capacity must still delegate, not
 * error out -- issue #737's "delegation preserved for ordinary oversized
 * input" requirement. */
ZTEST(se_cryptocell_hash_bounds, test_ordinary_oversized_input_still_delegates)
{
	struct se_hash_be *be = se_hash_acquire();
	zassert_not_null(be);

	alp_hash_backend_state_t state = {
		.alg     = ALP_HASH_SHA256,
		.be_data = be,
	};

	const size_t capacity = sizeof(be->buf);
	uint8_t     *data     = malloc(capacity + 1u);
	zassert_not_null(data);
	memset(data, 0x42, capacity + 1u);

	const alp_status_t rc = _ops.hash_update(&state, data, capacity + 1u);

	zassert_equal(rc, ALP_OK);
	zassert_true(g_fake_open_called, "one byte over capacity must delegate");
	/* be->used starts at 0 here, so se_hash_delegate() skips its replay
	 * call -- only the forward call happens. */
	zassert_equal(g_fake_update_calls, 1u);
	zassert_equal(g_fake_last_len, capacity + 1u);

	free(data);
	_ops.hash_close(&state);
}

/* Sanity / boundary check -- also non-discriminating, same reasoning as
 * above: input that exactly fills the remaining headroom must be
 * buffered IN PLACE, never delegated -- proves the guard is not
 * off-by-one in the conservative direction either. */
ZTEST(se_cryptocell_hash_bounds, test_exact_capacity_buffers_in_place)
{
	struct se_hash_be *be = se_hash_acquire();
	zassert_not_null(be);

	alp_hash_backend_state_t state = {
		.alg     = ALP_HASH_SHA256,
		.be_data = be,
	};

	const size_t capacity = sizeof(be->buf);
	uint8_t     *data     = malloc(capacity);
	zassert_not_null(data);
	memset(data, 0x5a, capacity);

	const alp_status_t rc = _ops.hash_update(&state, data, capacity);

	zassert_equal(rc, ALP_OK);
	zassert_false(g_fake_open_called, "input that fits exactly must not delegate");
	zassert_equal(g_fake_update_calls, 0u, "fallback must never be touched");
	zassert_equal(be->used, capacity);
	zassert_mem_equal(be->buf, data, capacity, "buffered bytes must match the input");

	free(data);
	_ops.hash_close(&state);
}

/* Sanity / boundary check -- non-discriminating: a corrupted `used`
 * already past the buffer end must delegate even for a zero-length
 * update -- issue #737's second requirement ("defensively verify
 * be->used is not already greater than sizeof(be->buf)"). */
ZTEST(se_cryptocell_hash_bounds, test_used_already_past_capacity_delegates)
{
	struct se_hash_be *be = se_hash_acquire();
	zassert_not_null(be);
	be->used = sizeof(be->buf) + 1u;

	alp_hash_backend_state_t state = {
		.alg     = ALP_HASH_SHA256,
		.be_data = be,
	};

	const alp_status_t rc = _ops.hash_update(&state, NULL, 0u);

	zassert_equal(rc, ALP_OK);
	zassert_true(g_fake_open_called, "used > capacity must delegate even for len == 0");
	/* be->used (corrupted to capacity + 1) is nonzero, so se_hash_delegate()
	 * replays it first; the forward call then carries this call's own
	 * len == 0. */
	zassert_equal(g_fake_update_calls, 2u);
	zassert_equal(g_fake_last_len, 0u);

	_ops.hash_close(&state);
}
