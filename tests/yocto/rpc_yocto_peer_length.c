/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for #1645: src/backends/rpc/yocto_drv.c's
 * rpc_rx_main() called `read(ch->ept_fd, buf, sizeof buf)` into a
 * fixed ALP_RPC_TX_FRAME_MAX buffer and, whenever a peer frame was
 * bigger than that, let read() itself silently truncate it -- the
 * truncated bytes still parsed as a well-formed-looking (method,
 * payload) frame whenever the method name landed inside the first
 * ALP_RPC_TX_FRAME_MAX bytes, so the subscriber ran with a silently
 * shortened payload. The fix treats a full-buffer read as a protocol
 * error (the rpmsg endpoint's own message-at-a-time semantics make
 * "read exactly filled the buffer" the only truncation signal
 * available) instead of parsing it.
 *
 * Same technique as tests/yocto/rpc_yocto_self_close.c: #includes the
 * real src/backends/rpc/yocto_drv.c directly and drives rpc_rx_main()
 * over an AF_UNIX/SOCK_DGRAM socketpair standing in for the
 * /dev/rpmsgN chardev -- no OpenAMP/libmetal userspace library
 * needed (the real backend code here only ever calls POSIX
 * read()/write()/poll()/pthread), so unlike this fix's sibling (2a,
 * rpc_uio_peer_length.c) this test is NOT gated behind a
 * pkg_check_modules(open-amp libmetal) find and runs on every host.
 * AF_UNIX SOCK_DGRAM preserves the exact property this fix depends
 * on: a `write()` larger than the reader's buffer is truncated by
 * the kernel to fit that buffer in one `read()`, with the remainder
 * silently discarded -- the same "read() returns exactly `sizeof
 * buf`" signal a real rpmsg endpoint chardev produces for an
 * oversized peer frame.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_yocto_peer_length
 *   ctest --test-dir build -R alp_test_rpc_yocto_peer_length
 */

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#include "../../src/backends/rpc/yocto_drv.c"

#include <string.h>
#include <sys/socket.h>

#include "test_assert.h"

#define TEST_TIMEOUT_MS 5000

/* alp_rpc_close_finalize() is normally defined by src/rpc_dispatch.c;
 * this binary does not link alp::sdk (see the #include-the-.c-file
 * note above). Never actually invoked here -- neither test's
 * subscriber callback calls alp_rpc_close(), so rpc_rx_main()'s
 * DEFERRED/self-close epilogue path never runs -- this stub exists
 * only to satisfy the linker, same as rpc_yocto_self_close.c's own
 * copy. */
void alp_rpc_close_finalize(void *owner)
{
	(void)owner;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Bounded poll-wait, mirroring rpc_uio_self_close.c's own wait_until():
 * an inline `while (!flag) { sleep; ALP_ASSERT_TRUE(deadline); }` loop
 * does NOT stop looping when that assert fails (ALP_ASSERT_TRUE is
 * non-fatal, see test_assert.h), so a genuine regression that never
 * sets `flag` spins forever instead of failing the test -- the exact
 * shape a repro of #1645's finding 1 hit while this file's assertions
 * were still inline. Returns false (never true) once `timeout_ms`
 * elapses so every call site can fail cleanly via ALP_ASSERT_TRUE. */
static bool wait_until(atomic_int *flag, int timeout_ms)
{
	int waited_ms = 0;

	while (!atomic_load(flag)) {
		sleep_ms(1);
		if (++waited_ms >= timeout_ms) return false;
	}
	return true;
}

/* Mirrors rpc_yocto_self_close.c's make_test_channel(): `ept_fd`
 * stands in for the real /dev/rpmsgN chardev (a socketpair end). */
static struct rpc_be *make_test_channel(int ept_fd)
{
	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	ALP_ASSERT_TRUE(ch != NULL);
	if (ch == NULL) return NULL;

	strncpy(ch->name, "peerlen", sizeof(ch->name) - 1);
	pthread_mutex_init(&ch->tx_mutex, NULL);
	pthread_mutex_init(&ch->sub_mutex, NULL);
	pthread_mutex_init(&ch->call_mutex, NULL);
	pthread_cond_init(&ch->call_cond, NULL);
	ch->ept_fd  = ept_fd;
	ch->ctrl_fd = -1;
	ALP_ASSERT_EQ_INT(pipe(ch->rx_wake_pipe), 0);
	atomic_store(&ch->rx_run, 1);
	return ch;
}

struct rx_spawn_ctx {
	struct rpc_be *ch;
	atomic_int    *worker_done;
};

static void *rx_thread_wrapper(void *arg)
{
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)arg;
	void                *ret = rpc_rx_main(ctx->ch);
	atomic_store(ctx->worker_done, 1);
	free(ctx);
	return ret;
}

static atomic_int g_worker_done;

static void spawn_rx_thread(struct rpc_be *ch)
{
	atomic_store(&g_worker_done, 0);
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)malloc(sizeof(*ctx));
	ALP_ASSERT_TRUE(ctx != NULL);
	ctx->ch          = ch;
	ctx->worker_done = &g_worker_done;
	ALP_ASSERT_EQ_INT(pthread_create(&ch->rx_thread, NULL, rx_thread_wrapper, ctx), 0);
}

/* Stop the rx thread cleanly via the same rx_wake_pipe/rx_run
 * mechanism y_shutdown() uses on an external close, then join it and
 * free the channel -- this test never routes through the real
 * y_shutdown()/y_destroy() (no dispatcher state to wire up here), so
 * teardown is done by hand. */
static void stop_and_free_channel(struct rpc_be *ch)
{
	atomic_store(&ch->rx_run, 0);
	char poke = 0;
	ALP_ASSERT_TRUE(write(ch->rx_wake_pipe[1], &poke, 1) >= 0);
	ALP_ASSERT_TRUE(wait_until(&g_worker_done, TEST_TIMEOUT_MS));
	pthread_join(ch->rx_thread, NULL);
	close(ch->rx_wake_pipe[0]);
	close(ch->rx_wake_pipe[1]);
	pthread_mutex_destroy(&ch->tx_mutex);
	pthread_mutex_destroy(&ch->sub_mutex);
	pthread_mutex_destroy(&ch->call_mutex);
	pthread_cond_destroy(&ch->call_cond);
	free(ch);
}

static atomic_int g_cb_fired;
static size_t     g_cb_payload_len;

static void on_big_frame(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)user;
	g_cb_payload_len = len;
	atomic_fetch_add(&g_cb_fired, 1);
}

/* THE regression: a peer frame longer than ALP_RPC_TX_FRAME_MAX,
 * written as ONE datagram (so the kernel truncates it to exactly
 * `sizeof buf` on read(), the same signal a real rpmsg endpoint
 * chardev gives for an oversized frame), must be dropped -- not
 * parsed as a shortened-but-otherwise-valid message. */
static void test_oversized_frame_is_dropped_not_truncated(void)
{
	atomic_store(&g_cb_fired, 0);
	g_cb_payload_len = 0;

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be          *ch = make_test_channel(sv[0]);
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(y_subscribe(&st, "big_frame", on_big_frame, ch), ALP_OK);
	spawn_rx_thread(ch);

	/* method "big_frame" (8 + NUL) + a 1200-byte payload = 1209 bytes,
	 * past ALP_RPC_TX_FRAME_MAX (1024) -- the method name still lands
	 * inside the first 1024 bytes, the exact pre-fix trap. */
	static uint8_t oversized_payload[1200];
	memset(oversized_payload, 0x5A, sizeof(oversized_payload));

	uint8_t oversized_frame[sizeof(oversized_payload) + 16];
	int     built = frame_build(oversized_frame,
	                            sizeof(oversized_frame),
	                            "big_frame",
	                            oversized_payload,
	                            sizeof(oversized_payload));
	ALP_ASSERT_TRUE(built > (int)ALP_RPC_TX_FRAME_MAX);

	ssize_t sent = send(sv[1], oversized_frame, (size_t)built, 0);
	ALP_ASSERT_EQ_INT((int)sent, built);

	/* Bounded wait for the rx thread to have processed (or dropped)
     * the frame; a small settle time is enough since delivery is
     * local. */
	sleep_ms(100);

	ALP_ASSERT_EQ_INT(atomic_load(&g_cb_fired), 0);

	stop_and_free_channel(ch);
	close(sv[1]);
}

/* Companion positive-path check: a small, well-formed frame must
 * still dispatch normally -- proves the fix rejects only a genuinely
 * oversized frame, not everything. */
static void test_small_frame_still_dispatches(void)
{
	atomic_store(&g_cb_fired, 0);
	g_cb_payload_len = 0;

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be          *ch = make_test_channel(sv[0]);
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(y_subscribe(&st, "m", on_big_frame, ch), ALP_OK);
	spawn_rx_thread(ch);

	static const uint8_t payload[32] = { 0 };
	uint8_t              frame[64];
	int                  built = frame_build(frame, sizeof(frame), "m", payload, sizeof(payload));
	ALP_ASSERT_TRUE(built > 0);

	ssize_t sent = send(sv[1], frame, (size_t)built, 0);
	ALP_ASSERT_EQ_INT((int)sent, built);

	ALP_ASSERT_TRUE(wait_until(&g_cb_fired, TEST_TIMEOUT_MS));
	ALP_ASSERT_EQ_INT(atomic_load(&g_cb_fired), 1);
	ALP_ASSERT_EQ_INT((int)g_cb_payload_len, (int)sizeof(payload));

	stop_and_free_channel(ch);
	close(sv[1]);
}

/* THE #1645 blocker (review finding 1): a frame of EXACTLY
 * ALP_RPC_TX_FRAME_MAX bytes is LEGAL -- alp_rpc_frame_size()
 * (rpc_ops.h) allows total == cap, not just total < cap -- so it must
 * still dispatch. rpc_rx_main()'s original fix used `n == sizeof buf`
 * as the oversized signal, which drops exactly this frame; the real
 * buffer is now ALP_RPC_TX_FRAME_MAX + 1 bytes so a legal max-size
 * frame reads as `n == ALP_RPC_TX_FRAME_MAX`, distinct from an
 * oversized one. Mirrors rpc_uio_peer_length.c's
 * test_frame_at_exact_max_still_dispatches(). */
static void test_frame_at_exact_max_still_dispatches(void)
{
	atomic_store(&g_cb_fired, 0);
	g_cb_payload_len = 0;

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be          *ch = make_test_channel(sv[0]);
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(y_subscribe(&st, "m", on_big_frame, ch), ALP_OK);
	spawn_rx_thread(ch);

	/* "m" + NUL = 2 bytes of header; pad the payload so the whole
	 * frame lands exactly on ALP_RPC_TX_FRAME_MAX. */
	static uint8_t payload[ALP_RPC_TX_FRAME_MAX - 2];
	memset(payload, 0x11, sizeof(payload));

	uint8_t frame[ALP_RPC_TX_FRAME_MAX];
	int     built = frame_build(frame, sizeof(frame), "m", payload, sizeof(payload));
	ALP_ASSERT_EQ_INT(built, (int)ALP_RPC_TX_FRAME_MAX);

	ssize_t sent = send(sv[1], frame, (size_t)built, 0);
	ALP_ASSERT_EQ_INT((int)sent, built);

	ALP_ASSERT_TRUE(wait_until(&g_cb_fired, TEST_TIMEOUT_MS));
	ALP_ASSERT_EQ_INT(atomic_load(&g_cb_fired), 1);
	ALP_ASSERT_EQ_INT((int)g_cb_payload_len, (int)sizeof(payload));

	stop_and_free_channel(ch);
	close(sv[1]);
}

int main(void)
{
	test_oversized_frame_is_dropped_not_truncated();
	test_small_frame_still_dispatches();
	test_frame_at_exact_max_still_dispatches();
	ALP_TEST_SUMMARY();
}
