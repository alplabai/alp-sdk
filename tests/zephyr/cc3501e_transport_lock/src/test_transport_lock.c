/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Concurrency regression test for issue #1116: cc3501e_request() must
 * serialise the whole 4-phase SPI exchange (and the shared tx_scratch /
 * rx_scratch it touches) across every caller of a shared cc3501e_t --
 * before this fix, two threads racing a single ctx could interleave their
 * transactions and read back each other's replies (or a corrupted,
 * desynced frame).
 *
 * Rather than hoping a natural scheduling race trips the bug (flaky both
 * ways -- see the race-safe skill), this test forces ONE deterministic
 * interleave: two threads share one cc3501e_t and issue two DIFFERENT
 * commands (PING, GET_VERSION) -- a "concurrent mixed-operation" scenario
 * matching issue #1116's own acceptance criterion.  Thread VER is released
 * first and starts its GET_VERSION request; the moment its request header
 * has been clocked (but before it reads the reply), thread PING is
 * released and races its OWN complete PING request/reply cycle through in
 * between -- on a real CS-less link this is exactly two callers of an
 * unlocked cc3501e_request() colliding.  The rendezvous lives entirely in
 * this file's own alp_spi_transceive() stub (a test seam, not a
 * production hook) and uses a BOUNDED wait, so it degrades to a harmless
 * no-op (times out and continues) once the fix makes thread PING unable
 * to reach alp_spi_transceive() at all during that window -- it is
 * blocked in cc3501e_request()'s OWN lock acquire instead.
 *
 * Pre-fix: thread VER's request desyncs (thread PING's complete cycle
 * scrambles the single unlocked slave model mid-way through VER's own),
 * so VER's cc3501e_get_version() returns something other than ALP_OK with
 * the correct version -- reply cross-talk / transport desync, exactly as
 * issue #1116 describes.
 * Post-fix: thread PING cannot run until VER's cc3501e_request() releases
 * ctx's transport lock, so nothing interleaves and both calls succeed with
 * correct data.
 *
 * Only cc3501e_request() (via cc3501e_ping / cc3501e_get_version) is
 * exercised against the REAL driver (chips/cc3501e/cc3501e_core.c); every
 * other alp_* seam is a minimal test double, mirroring
 * tests/zephyr/cc3501e_host_driver's fixture shape.
 */

#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- software model of the (single, physical, CS-less) firmware slave ----- */

enum slave_phase {
	PH_REQ_HDR = 0,
	PH_REPLY_HDR,
	PH_REPLY_PL,
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint8_t  reply_pl[8]; /* status(1) + up to 7 data bytes -- plenty for PING/GET_VERSION */
	uint16_t reply_len;
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase = PH_REQ_HDR;
}

/* Stage the reply for whatever slave.cmd was just captured.  Both commands
 * under test are header-only requests (no request payload), matching the
 * majority of cc3501e ops (PING / GET_VERSION / WIFI_GET_RSSI / ...). */
static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_PING:
		slave.reply_pl[0] = ALP_CC3501E_RESP_OK;
		slave.reply_len   = 1u;
		break;
	case ALP_CC3501E_CMD_GET_VERSION:
		slave.reply_pl[0] = ALP_CC3501E_RESP_OK;
		slave.reply_pl[1] = 0xABu; /* version LE lo -- the value the test asserts on */
		slave.reply_pl[2] = 0xCDu; /* version LE hi */
		slave.reply_len   = 3u;
		break;
	default:
		/* A cmd byte this model never issued (the corrupted-parse case
		 * below) -- stage SOME terminal reply so a caller that reads it
		 * gets a real (if meaningless) status, not stale/zero memory. */
		slave.reply_pl[0] = ALP_CC3501E_RESP_ERR_INVALID;
		slave.reply_len   = 1u;
		break;
	}
}

/* ---- the forced deterministic interleave ----------------------------------- */

static struct k_sem race_ready; /* given once thread VER's header phase lands */
static struct k_sem race_go;    /* thread VER waits on this, bounded, before continuing */
static atomic_t     race_armed;

#define RACE_WINDOW_MS 50

/* Fires exactly once: the FIRST time any caller's request header is
 * processed.  By construction that is always thread VER (thread PING is
 * blocked on race_ready and cannot call cc3501e_request() at all until
 * this hook releases it), so this reliably captures "thread PING races in
 * mid-way through thread VER's transaction" without depending on the
 * scheduler's natural timing. */
static void maybe_force_interleave(enum slave_phase processed_phase)
{
	if (processed_phase != PH_REQ_HDR) {
		return;
	}
	bool was_armed = atomic_cas(&race_armed, 0, 1);
	if (!was_armed) {
		return;
	}
	k_sem_give(&race_ready);
	/* Bounded: with the fix applied, thread PING never reaches
	 * alp_spi_transceive() during this window (it is spin-waiting on
	 * ctx's own transport lock instead), so nobody ever gives race_go
	 * and this simply times out -- proving the fix rather than
	 * deadlocking the test. */
	(void)k_sem_take(&race_go, K_MSEC(RACE_WINDOW_MS));
}

/* ---- test doubles for the alp_* seams the host driver links against ------- */

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	ARG_UNUSED(bus);
	if (len == 0u) {
		return ALP_OK;
	}
	enum slave_phase processed_phase = slave.phase;

	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd = tx[0];
		/* Every request in this test is header-only (tx[2..3] == 0), so
		 * always dispatch immediately and move straight to the reply
		 * header phase -- no PH_REQ_PL needed. */
		slave_dispatch();
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]       = slave.cmd; /* reply header echoes the cmd */
		rx[1]       = 0x00u;
		rx[2]       = (uint8_t)(slave.reply_len & 0xFFu);
		rx[3]       = (uint8_t)((slave.reply_len >> 8) & 0xFFu);
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		memcpy(rx, slave.reply_pl, MIN(len, sizeof(slave.reply_pl)));
		slave.phase = PH_REQ_HDR;
		break;
	}

	/* Injected AFTER this call's own state-machine processing (so a
	 * paused caller has already half-committed its own transaction to
	 * `slave`) and BEFORE returning to cc3501e_request() -- this is what
	 * lets the racing thread's complete cycle land in the middle of the
	 * paused thread's, exactly reproducing issue #1116's unlocked
	 * cross-talk. */
	maybe_force_interleave(processed_phase);
	return ALP_OK;
}

/* alp_delay_us stays a no-op (matches tests/zephyr/cc3501e_host_driver --
 * cc3501e_request's own fixed settle gaps carry no meaning against this
 * mock).  alp_delay_ms MUST genuinely yield (k_msleep, not a no-op): it
 * backs cc3501e_lock_acquire()'s bounded retry loop, and this test relies
 * on that yield to let the contending thread actually run. */
void alp_delay_us(uint32_t us)
{
	ARG_UNUSED(us);
}
void alp_delay_ms(uint32_t ms)
{
	k_msleep((int32_t)ms);
}
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	ARG_UNUSED(pin_id);
	return NULL;
}
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	ARG_UNUSED(pin);
	ARG_UNUSED(level);
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	ARG_UNUSED(pin);
	ARG_UNUSED(level);
	return ALP_ERR_NOSUPPORT;
}

/* ---- fixture ---------------------------------------------------------------- */

static cc3501e_t  shared_ctx;
static alp_spi_t *fake_bus = (alp_spi_t *)&shared_ctx; /* opaque, non-NULL; the stub ignores it */

#define WORKER_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(ping_stack, WORKER_STACK_SIZE);
static struct k_thread ping_thread_h;

static alp_status_t g_ping_status;

static void ping_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Cannot reach cc3501e_request() -- and therefore alp_spi_transceive()
	 * -- until thread VER's mock call releases this.  Pre-fix that
	 * release happens mid-VER-transaction (genuine interleave); post-fix
	 * it happens with VER already holding ctx's transport lock, so this
	 * thread instead blocks inside cc3501e_lock_acquire() until VER is
	 * fully done. */
	k_sem_take(&race_ready, K_FOREVER);
	g_ping_status = cc3501e_ping(&shared_ctx);
}

ZTEST(cc3501e_transport_lock, test_concurrent_ping_and_get_version_do_not_cross_talk)
{
	k_sem_init(&race_ready, 0, 1);
	k_sem_init(&race_go, 0, 1);
	atomic_clear(&race_armed);
	g_ping_status = ALP_ERR_TIMEOUT; /* sentinel -- overwritten iff the thread ran */

	k_tid_t ping_tid = k_thread_create(&ping_thread_h,
	                                   ping_stack,
	                                   K_THREAD_STACK_SIZEOF(ping_stack),
	                                   ping_thread_entry,
	                                   NULL,
	                                   NULL,
	                                   NULL,
	                                   K_PRIO_PREEMPT(5),
	                                   0,
	                                   K_NO_WAIT);

	uint16_t     version    = 0u;
	alp_status_t ver_status = cc3501e_get_version(&shared_ctx, &version);

	/* Bounded join: the ping thread must finish shortly after its own
	 * cc3501e_ping() unblocks (either via the forced race window or via
	 * the fixed lock's own bounded timeout) -- never hang the suite. */
	zassert_equal(k_thread_join(ping_tid, K_MSEC(2000)), 0, "ping thread never finished");

	/*
	 * PROOF (see the file header + issue #1116's acceptance criterion):
	 *   - Pre-fix, thread PING's complete cycle scrambles the shared slave
	 *     model mid-way through thread VER's own transaction, so VER's
	 *     request desyncs: ver_status != ALP_OK (this repo's cc3501e_request
	 *     maps a reply-header mismatch to ALP_ERR_IO) and/or the decoded
	 *     version is wrong. That is reply cross-talk / transport
	 *     desynchronisation exactly as the issue describes.
	 *   - Post-fix, thread PING cannot touch the transport until thread
	 *     VER's cc3501e_request() releases ctx's internal lock, so nothing
	 *     interleaves: both calls succeed with correct data.
	 */
	zassert_equal(ver_status, ALP_OK, "GET_VERSION desynced under a concurrent PING (issue #1116)");
	zassert_equal(version, 0xCDABu, "GET_VERSION returned the wrong (cross-talked) data");
	zassert_equal(g_ping_status, ALP_OK, "concurrent PING failed");
}

static void reset_before(void *fixture)
{
	ARG_UNUSED(fixture);
	slave_reset();
	zassert_equal(cc3501e_init(&shared_ctx, fake_bus), ALP_OK, "init binds the (fake) bus");
}

ZTEST_SUITE(cc3501e_transport_lock, NULL, NULL, reset_before, NULL, NULL);
