/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deterministic virtual clock backing <alp/testing/clock.h>.  Never
 * sleeps or blocks: "now" only moves inside
 * alp_testing_clock_advance_ms(), which fires every event due at or
 * before the new now, in ascending timestamp order, synchronously on
 * the calling thread.
 *
 * The pending-event set is a small fixed-capacity array (no heap --
 * this compiles into ZTEST images that may run without one).  Ties in
 * timestamp fire in scheduling order (stable, since we always pick
 * the lowest-indexed minimum among candidates).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/testing/clock.h>

#include "virtual_clock.h"

#ifndef ALP_TESTING_CLOCK_MAX_EVENTS
#define ALP_TESTING_CLOCK_MAX_EVENTS 32
#endif

typedef struct {
	uint64_t                   at_ms;
	alp_testing_clock_event_fn fn;
	void                      *ctx;
	bool                       pending;
} alp_testing_clock_event_t;

static uint64_t                  g_now_ms;
static alp_testing_clock_event_t g_events[ALP_TESTING_CLOCK_MAX_EVENTS];

void alp_testing_clock_reset(void)
{
	g_now_ms = 0;
	for (size_t i = 0; i < ALP_TESTING_CLOCK_MAX_EVENTS; ++i) {
		g_events[i].pending = false;
	}
}

uint64_t alp_testing_clock_now_ms(void)
{
	return g_now_ms;
}

alp_status_t alp_testing_clock_schedule(uint64_t at_ms, alp_testing_clock_event_fn fn, void *ctx)
{
	for (size_t i = 0; i < ALP_TESTING_CLOCK_MAX_EVENTS; ++i) {
		if (!g_events[i].pending) {
			g_events[i].at_ms   = at_ms;
			g_events[i].fn      = fn;
			g_events[i].ctx     = ctx;
			g_events[i].pending = true;
			return ALP_OK;
		}
	}
	return ALP_ERR_NOMEM;
}

alp_status_t alp_testing_clock_advance_ms(uint64_t ms)
{
	uint64_t new_now = g_now_ms + ms;
	if (new_now < g_now_ms) { /* overflow */
		return ALP_ERR_INVAL;
	}

	/*
	 * Repeatedly fire the earliest still-pending due event, then
	 * re-scan from the top -- a fired callback may itself schedule
	 * a new event (e.g. a chained edge_at), and that new event must
	 * still be considered against new_now before this call returns.
	 */
	for (;;) {
		size_t   best    = ALP_TESTING_CLOCK_MAX_EVENTS;
		uint64_t best_at = 0;
		for (size_t i = 0; i < ALP_TESTING_CLOCK_MAX_EVENTS; ++i) {
			if (!g_events[i].pending || g_events[i].at_ms > new_now) continue;
			if (best == ALP_TESTING_CLOCK_MAX_EVENTS || g_events[i].at_ms < best_at) {
				best    = i;
				best_at = g_events[i].at_ms;
			}
		}
		if (best == ALP_TESTING_CLOCK_MAX_EVENTS) break;

		alp_testing_clock_event_fn fn  = g_events[best].fn;
		void                      *ctx = g_events[best].ctx;
		g_events[best].pending         = false;
		if (fn != NULL) fn(ctx);
	}

	g_now_ms = new_now;
	return ALP_OK;
}
