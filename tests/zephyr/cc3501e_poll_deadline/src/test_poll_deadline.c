/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deadline regression test for issue #1953: `poll_by_repeat()`
 * (chips/cc3501e/cc3501e_core.c) used to charge only its own back-off
 * sleeps against `timeout_ms`, leaving the time spent inside
 * `cc3501e_request_locked()` on every attempt -- up to four SPI transfers,
 * plus whatever the transport waits -- entirely free. `timeout_ms` was
 * therefore a FLOOR on wall time, not a bound, and the skew grew with the
 * number of attempts made.
 *
 * Drives the REAL host driver (reached via the argless
 * `cc3501e_ble_adv_stop()` wrapper, which forwards straight to
 * `poll_by_repeat()` with no payload) against a minimal software slave
 * that ALWAYS answers `RESP_ERR_BUSY` -- a companion that never publishes
 * a result, exactly the scenario issue #1953 names.
 *
 * The clock under test is a fake, not a real one: `alp_uptime_ms()` and
 * `alp_delay_ms()` below share one process-wide millisecond counter that
 * advances only on demand --
 *
 *   - `alp_delay_ms(ms)` advances it by `ms` (models a real scheduler
 *     sleep, same accounting a real caller's wall clock would see).
 *   - `alp_spi_transceive()` advances it by `CC3501E_ATTEMPT_COST_MS` on
 *     every call, modelling the "four SPI transfers ... is free" cost
 *     issue #1953 describes -- exactly the cost the old sleep-only budget
 *     never charged.
 *
 * No real time passes, so the test is fast and deterministic: it proves
 * the deadline MATH, not a timing-sensitive race against a real clock.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- fake clock: the "stub that advances on demand" ------------------- */

static uint64_t g_fake_now_ms;

uint64_t alp_uptime_ms(void)
{
	return g_fake_now_ms;
}

void alp_delay_ms(uint32_t ms)
{
	g_fake_now_ms += ms;
}

void alp_delay_us(uint32_t us)
{
	/* cc3501e_reply_gate()'s microsecond settles are not under test here;
	 * inert, like every other host-driver test's alp_delay_us seam. */
	(void)us;
}

alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	return NULL;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

/* ---- software model of a companion that never resolves ----------------
 *
 * Handles exactly the argless request/reply shape (no request payload
 * phase): request header -> reply header -> reply payload.  Every reply
 * is a bare RESP_ERR_BUSY -- the firmware worker "still running",
 * forever -- which is what makes poll_by_repeat() retry until its
 * deadline, the behaviour under test.
 */

#define CC3501E_ATTEMPT_COST_MS 10u /* simulated per-alp_spi_transceive() cost */

enum slave_phase { PH_REQ_HDR = 0, PH_REPLY_HDR, PH_REPLY_PL };

static enum slave_phase g_phase;
static uint8_t          g_last_cmd;
static uint32_t         g_transceive_calls;

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	/* The "free" attempt cost issue #1953 describes: real SPI transfer
	 * time no sleep-only budget ever charged. */
	g_fake_now_ms += CC3501E_ATTEMPT_COST_MS;
	g_transceive_calls++;
	if (len == 0u) {
		return ALP_OK;
	}
	switch (g_phase) {
	case PH_REQ_HDR:
		g_last_cmd = tx[0];
		memset(rx, ALP_CC3501E_SYNC_IDLE, len); /* "armed" marker */
		g_phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]   = g_last_cmd; /* reply header echoes the request cmd */
		rx[1]   = 0x00u;      /* solicited */
		rx[2]   = 1u;         /* reply payload length = 1 (bare status) */
		rx[3]   = 0u;
		g_phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		rx[0]   = ALP_CC3501E_RESP_ERR_BUSY; /* worker never finishes */
		g_phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

/* ---- fixture ------------------------------------------------------------ */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw; /* opaque, non-NULL; the stub ignores it */

static void reset_before(void *fixture)
{
	(void)fixture;
	g_phase            = PH_REQ_HDR;
	g_last_cmd         = 0u;
	g_transceive_calls = 0u;
	g_fake_now_ms      = 0u;
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
}

/* ================================ TESTS =================================== */

ZTEST(cc3501e_poll_deadline, test_timeout_bounds_wall_time_1953)
{
	const uint32_t timeout_ms = 200u;
	const uint64_t start_ms   = g_fake_now_ms;

	alp_status_t s = cc3501e_ble_adv_stop(&fw, timeout_ms);

	zassert_equal(
	    s, ALP_ERR_TIMEOUT, "a companion that never publishes a result must time out (got %d)", s);

	const uint64_t elapsed_ms = g_fake_now_ms - start_ms;
	/* Pre-fix (sleep-only budget): the full CC3501E_ATTEMPT_COST_MS * 3
	 * transceive calls of EVERY attempt is free, on top of the full
	 * timeout_ms of back-off sleeps -- roughly 10 attempts at this
	 * timeout/backoff shape, i.e. ~500 ms against a 200 ms budget.
	 * Post-fix (deadline loop): the only unavoidable slack is ONE
	 * attempt's own cost (3 * 10 ms = 30 ms) straddling the deadline.
	 * 60 ms of headroom cleanly separates the two. */
	zassert_true(elapsed_ms <= (uint64_t)timeout_ms + 60u,
	             "poll_by_repeat must bound wall time to ~timeout_ms plus one attempt's cost, "
	             "not timeout_ms plus the accumulated cost of every attempt made "
	             "(got %llu ms against a %u ms budget, %u attempts)",
	             (unsigned long long)elapsed_ms,
	             timeout_ms,
	             g_transceive_calls / 3u);
}

ZTEST(cc3501e_poll_deadline, test_zero_timeout_still_makes_one_attempt_1953)
{
	alp_status_t s = cc3501e_ble_adv_stop(&fw, 0u);

	zassert_equal(s, ALP_ERR_TIMEOUT, "zero budget must still resolve, not hang (got %d)", s);
	zassert_true(g_transceive_calls >= 3u,
	             "zero timeout_ms must still make at least one full attempt (got %u transceive "
	             "calls)",
	             g_transceive_calls);
}

ZTEST_SUITE(cc3501e_poll_deadline, NULL, NULL, reset_before, NULL, NULL);
