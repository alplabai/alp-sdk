/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for #1645: src/backends/rpc/yocto_uio_drv.c's
 * uio_ept_cb() clipped a peer rpmsg frame longer than
 * ALP_RPC_TX_FRAME_MAX to that size and dispatched it as if it were
 * the complete message -- if the method name (plus its NUL) landed
 * inside the first ALP_RPC_TX_FRAME_MAX bytes, frame_parse() still
 * produced a well-formed-looking (method, payload) pair, so the
 * subscriber callback ran with a silently-shortened payload and
 * ALP_OK-shaped success everywhere. The fix rejects a frame longer
 * than the buffer instead of truncating it.
 *
 * This file #includes the real src/backends/rpc/yocto_uio_drv.c
 * (same technique as tests/yocto/rpc_uio_self_close.c) so
 * uio_ept_cb() runs unmodified. Only `uio_ept_cb(NULL, data, len, 0,
 * ch)` is exercised directly -- it never touches `ch->ept`/
 * `ch->rproc`, so this test's channel is a bare calloc()'d
 * `struct rpc_be` with just the mutexes initialised, none of
 * rpc_uio_self_close.c's remoteproc/libmetal transport doubles.
 *
 * @par Build gate (this test's own honesty note)
 * yocto_uio_drv.c only compiles under ALP_SDK_HAVE_OPENAMP_USERLAND,
 * which needs real open-amp + libmetal headers on the include path
 * -- this test is wired into tests/yocto/CMakeLists.txt behind the
 * SAME `pkg_check_modules(... open-amp libmetal)` gate as
 * rpc_uio_self_close.c, and is skipped (not built, not run) exactly
 * like that sibling test whenever those packages are absent -- true
 * on this repo's current dev host (no libmetal-dev package exists in
 * the Debian/Ubuntu archive) and on this repo's CI today (neither
 * workflow nor test-all.sh installs open-amp/libmetal). That is a
 * pre-existing gap in this whole test class, not introduced here --
 * see the #1645 dispatch report for how this fix's control flow was
 * instead verified end-to-end: it is byte-for-byte the same
 * reject-over-clip shape as yocto_drv.c's rpc_rx_main() fix (2b,
 * tests/yocto/rpc_yocto_peer_length.c), which needs no libmetal and
 * DOES run here, red then green.
 *
 * Build (when open-amp/libmetal ARE available):
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_uio_peer_length
 *   ctest --test-dir build -R alp_test_rpc_uio_peer_length
 */

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#include "../../src/backends/rpc/yocto_uio_drv.c"

#include <string.h>

#include "test_assert.h"

/* Builds a `struct rpc_be` with just the state uio_ept_cb() touches:
 * call_mutex/sub_mutex + the subs table. uio_ept_cb() never reaches
 * ch->ept/ch->rproc, so unlike rpc_uio_self_close.c's
 * make_test_channel() this needs no remoteproc_init() and no fake
 * transport -- this test never calls y_destroy() either, so
 * rpc_be_teardown()'s unconditional remoteproc_shutdown()/_remove()
 * (which DOES need a real ch->rproc) never runs. */
static struct rpc_be *make_minimal_channel(void)
{
	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	ALP_ASSERT_TRUE(ch != NULL);
	if (ch == NULL) return NULL;

	strncpy(ch->name, "peerlen", sizeof(ch->name) - 1);
	pthread_mutex_init(&ch->call_mutex, NULL);
	pthread_mutex_init(&ch->sub_mutex, NULL);
	pthread_cond_init(&ch->call_cond, NULL);
	return ch;
}

static void free_minimal_channel(struct rpc_be *ch)
{
	pthread_mutex_destroy(&ch->call_mutex);
	pthread_mutex_destroy(&ch->sub_mutex);
	pthread_cond_destroy(&ch->call_cond);
	free(ch);
}

static int    g_cb_fired;
static size_t g_cb_payload_len;

static void on_big_frame(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)user;
	g_cb_fired++;
	g_cb_payload_len = len;
}

/* THE regression: a peer frame longer than ALP_RPC_TX_FRAME_MAX must be
 * dropped, not clipped-and-dispatched with a silently-shortened
 * payload. */
static void test_oversized_frame_is_dropped_not_clipped(void)
{
	g_cb_fired       = 0;
	g_cb_payload_len = 0;

	struct rpc_be          *ch = make_minimal_channel();
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(y_subscribe(&st, "big_frame", on_big_frame, ch), ALP_OK);

	/* method "big_frame" (8 + NUL) + a 1200-byte payload = 1209 bytes,
	 * well past ALP_RPC_TX_FRAME_MAX (1024) -- but the method name
	 * still lands inside the first 1024 bytes, which is exactly the
	 * pre-fix trap: frame_parse() on the clipped 1024-byte prefix
	 * still finds a valid-looking (method, payload) pair. */
	static uint8_t oversized_payload[1200];
	memset(oversized_payload, 0x5A, sizeof(oversized_payload));

	uint8_t oversized_frame[sizeof(oversized_payload) + 16];
	int     built = frame_build(oversized_frame,
	                            sizeof(oversized_frame),
	                            "big_frame",
	                            oversized_payload,
	                            sizeof(oversized_payload));
	ALP_ASSERT_TRUE(built > (int)ALP_RPC_TX_FRAME_MAX);

	(void)uio_ept_cb(NULL, oversized_frame, (size_t)built, 0, ch);

	ALP_ASSERT_EQ_INT(g_cb_fired, 0);

	free_minimal_channel(ch);
}

/* Companion positive-path check: a frame AT the boundary (exactly
 * ALP_RPC_TX_FRAME_MAX) is not oversized and must still dispatch --
 * proves the #1645 fix rejects only what is genuinely too big, not an
 * off-by-one over everything. */
static void test_frame_at_exact_max_still_dispatches(void)
{
	g_cb_fired       = 0;
	g_cb_payload_len = 0;

	struct rpc_be          *ch = make_minimal_channel();
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(y_subscribe(&st, "m", on_big_frame, ch), ALP_OK);

	/* "m" + NUL = 2 bytes of header; pad the payload so the whole
	 * frame lands exactly on ALP_RPC_TX_FRAME_MAX. */
	static uint8_t payload[ALP_RPC_TX_FRAME_MAX - 2];
	memset(payload, 0x11, sizeof(payload));

	uint8_t frame[ALP_RPC_TX_FRAME_MAX];
	int     built = frame_build(frame, sizeof(frame), "m", payload, sizeof(payload));
	ALP_ASSERT_EQ_INT(built, (int)ALP_RPC_TX_FRAME_MAX);

	(void)uio_ept_cb(NULL, frame, (size_t)built, 0, ch);

	ALP_ASSERT_EQ_INT(g_cb_fired, 1);
	ALP_ASSERT_EQ_INT((int)g_cb_payload_len, (int)sizeof(payload));

	free_minimal_channel(ch);
}

int main(void)
{
	test_oversized_frame_is_dropped_not_clipped();
	test_frame_at_exact_max_still_dispatches();
	ALP_TEST_SUMMARY();
}
