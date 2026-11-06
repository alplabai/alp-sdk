/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * White-box tests for the LPRTC counter -> calendar shim (#2930).
 *
 * The real src/backends/rtc/lprtc_calendar_shim.c is #included below so the
 * static state, the rebase and the 6 h timer callback are exercised as built.
 * The counter driver, device readiness and the spinlock are replaced by fakes:
 *   - the counter is a plain uint32_t the test steps across several 2^32 wraps;
 *   - the spinlock wrapper counts the lock depth, so a counter read taken
 *     outside the state lock is observable (g_unlocked_reads) and can be
 *     preempted by a competing set_time (g_interpose) -- the stale-counter race
 *     that made (now - tick_snapshot) underflow and jump the clock ~36.4 h.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/soc_caps.h>

/* native_sim has no LPRTC alias / RTC count: stand in for both. */
#undef ALP_SOC_RTC_COUNT
#define ALP_SOC_RTC_COUNT 1

#define FAKE_FREQ_HZ 32768u
#define TICKS_PER_S  FAKE_FREQ_HZ
#define TICKS_PER_H  (3600u * TICKS_PER_S)

static struct device fake_dev;
static uint32_t      g_counter;
static int           g_lock_depth;
static unsigned      g_unlocked_reads;
static void (*g_interpose)(void);

static int fake_counter_get_value(const struct device *dev, uint32_t *ticks)
{
	ARG_UNUSED(dev);
	uint32_t value = g_counter;
	*ticks         = value;
	if (g_lock_depth == 0) {
		g_unlocked_reads++;
		if (g_interpose != NULL) {
			void (*fn)(void) = g_interpose;
			g_interpose      = NULL;
			fn(); /* another context runs between the read and the lock */
		}
	}
	return 0;
}

static int fake_counter_start(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -EALREADY;
}

static uint32_t fake_counter_get_frequency(const struct device *dev)
{
	ARG_UNUSED(dev);
	return FAKE_FREQ_HZ;
}

static bool fake_device_is_ready(const struct device *dev)
{
	ARG_UNUSED(dev);
	return true;
}

static k_spinlock_key_t counting_lock(struct k_spinlock *l)
{
	g_lock_depth++;
	return (k_spin_lock)(l);
}

static void counting_unlock(struct k_spinlock *l, k_spinlock_key_t key)
{
	g_lock_depth--;
	(k_spin_unlock)(l, key);
}

#undef counter_get_value
#undef counter_start
#undef counter_get_frequency
#undef device_is_ready
#undef k_spin_lock
#undef k_spin_unlock
#undef DT_NODE_HAS_STATUS
#undef DT_ALIAS
#undef DEVICE_DT_GET
#define counter_get_value                fake_counter_get_value
#define counter_start                    fake_counter_start
#define counter_get_frequency            fake_counter_get_frequency
#define device_is_ready                  fake_device_is_ready
#define k_spin_lock                      counting_lock
#define k_spin_unlock                    counting_unlock
#define DT_NODE_HAS_STATUS(node, status) 1
#define DT_ALIAS(name)                   0
#define DEVICE_DT_GET(node)              (&fake_dev)

#include "../../../../src/backends/rtc/lprtc_calendar_shim.c"

static alp_rtc_backend_state_t g_st;

static alp_rtc_time_t make_time(uint16_t year, uint8_t month, uint8_t day, uint8_t hour)
{
	alp_rtc_time_t t = { 0 };
	t.year           = year;
	t.month          = month;
	t.day            = day;
	t.hour           = hour;
	return t;
}

static int64_t read_unix(alp_rtc_backend_state_t *st)
{
	alp_rtc_time_t t = { 0 };
	zassert_equal(shim_get_time(st, &t), ALP_OK);
	return _calendar_to_unix(&t);
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&_state, 0, sizeof(_state));
	memset(&g_st, 0, sizeof(g_st));
	g_lock_depth     = 0;
	g_unlocked_reads = 0;
	g_interpose      = NULL;
}

ZTEST_SUITE(lprtc_calendar_shim, NULL, NULL, before, NULL, NULL);

/* 120 h of hourly reads spans three 2^32-tick (36.4 h) wraps: every read must be
 * exactly one hour after the previous one -- monotonic and correct. */
ZTEST(lprtc_calendar_shim, test_get_time_monotonic_across_multiple_wraps)
{
	g_counter         = 0xFFFFF000u; /* first wrap lands almost immediately */
	alp_rtc_time_t t0 = make_time(2024, 1, 1, 0);
	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t0), ALP_OK);
	const int64_t base = _calendar_to_unix(&t0);

	int64_t prev = base;
	for (int h = 1; h <= 120; h++) {
		g_counter += TICKS_PER_H;
		int64_t now = read_unix(&g_st);
		zassert_true(now >= prev, "clock went backwards at h=%d", h);
		zassert_equal(now, base + (int64_t)h * 3600, "wrong time at h=%d", h);
		prev = now;
	}
}

/* No reader at all for 120 h: only the 6 h timer folds elapsed time in, which is
 * what keeps a long idle stretch from losing wraps. */
ZTEST(lprtc_calendar_shim, test_rebase_timer_alone_carries_multiple_wraps)
{
	g_counter         = 0xFFFFF000u;
	alp_rtc_time_t t0 = make_time(2024, 1, 1, 0);
	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t0), ALP_OK);
	const int64_t base = _calendar_to_unix(&t0);

	for (int i = 0; i < 20; i++) {
		g_counter += LPRTC_REBASE_PERIOD_H * TICKS_PER_H;
		_rebase_timer_fn(NULL);
	}
	zassert_equal(read_unix(&g_st), base + 120 * 3600);
}

/* open, set, open, get: a later open (any handle) must keep the set time. */
ZTEST(lprtc_calendar_shim, test_reopen_keeps_set_time)
{
	g_counter                      = 1234u;
	alp_rtc_time_t          t0     = make_time(2030, 6, 15, 12);
	alp_rtc_backend_state_t second = { 0 };

	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t0), ALP_OK);
	g_counter += 10u * TICKS_PER_S;
	zassert_equal(shim_open(0, &second, NULL), ALP_OK);

	zassert_equal(read_unix(&second), _calendar_to_unix(&t0) + 10);
	zassert_equal(read_unix(&g_st), _calendar_to_unix(&t0) + 10);
}

/* Every counter read must happen under the state lock; an unlocked read is the
 * stale-snapshot window. */
ZTEST(lprtc_calendar_shim, test_counter_is_read_under_the_state_lock)
{
	g_counter         = 5u;
	alp_rtc_time_t t0 = make_time(2024, 1, 1, 0);
	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t0), ALP_OK);
	g_counter += TICKS_PER_H;
	(void)read_unix(&g_st);
	_rebase_timer_fn(NULL);
	zassert_equal(g_unlocked_reads, 0u, "counter read outside _state_lock");
}

static alp_rtc_time_t g_preempt_time;

static void preempting_set_time(void)
{
	g_counter += 100u * TICKS_PER_S;
	(void)shim_set_time(&g_st, &g_preempt_time);
}

/* A set_time that lands between get_time's counter read and its lock must not
 * leave tick_snapshot ahead of a stale `now` (the ~36.4 h forward jump). */
ZTEST(lprtc_calendar_shim, test_get_time_survives_preempting_set_time)
{
	g_counter             = 0x80000000u;
	alp_rtc_time_t t1     = make_time(2024, 1, 1, 0);
	g_preempt_time        = make_time(2024, 1, 1, 0);
	g_preempt_time.second = 40;
	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t1), ALP_OK);

	g_counter += 60u * TICKS_PER_S;
	g_interpose   = preempting_set_time;
	int64_t first = read_unix(&g_st);
	g_interpose   = NULL;
	int64_t later = read_unix(&g_st);

	const int64_t lo = _calendar_to_unix(&t1);
	zassert_true(first >= lo && first <= lo + 3600, "first read jumped: %lld", (long long)first);
	zassert_true(later >= lo && later <= lo + 3600, "later read jumped: %lld", (long long)later);
}

/* Same window in the 6 h timer callback. */
ZTEST(lprtc_calendar_shim, test_rebase_timer_survives_preempting_set_time)
{
	g_counter             = 0x80000000u;
	alp_rtc_time_t t1     = make_time(2024, 1, 1, 0);
	g_preempt_time        = make_time(2024, 1, 1, 0);
	g_preempt_time.second = 40;
	zassert_equal(shim_open(0, &g_st, NULL), ALP_OK);
	zassert_equal(shim_set_time(&g_st, &t1), ALP_OK);

	g_counter += 60u * TICKS_PER_S;
	g_interpose = preempting_set_time;
	_rebase_timer_fn(NULL);
	g_interpose = NULL;

	const int64_t lo    = _calendar_to_unix(&t1);
	int64_t       later = read_unix(&g_st);
	zassert_true(later >= lo && later <= lo + 3600, "read jumped: %lld", (long long)later);
}
