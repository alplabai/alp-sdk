/* tests/host/test_camera_watchdog.c -- src/vision/camera_watchdog.h: the
 * counter behind main.c's TR_INPUT_NPU camera-liveness watchdogs
 * (calib_ticks, paused_ticks). Fix round 7 (silicon finding on 2c45a9c):
 * before the fix, a healthy pipeline with an empty booth still tripped the
 * watchdog at TR_CALIB_TIMEOUT_TICKS, because the counter was fed "got a
 * calibrated player" instead of "is the HP pipeline alive". These two cases
 * are exactly what this file pins. */
#include <assert.h>
#include <stdio.h>

#include "../../src/vision/camera_watchdog.h"

#define TR_CALIB_TIMEOUT_TICKS 450u /* mirrors main.c's own constant */

int main(void)
{
	/* 1. A fresh (ok=true) signal every tick never accumulates, however
	 * long the run: the empty-booth case that used to false-trip. */
	uint32_t ticks = 0u;

	for (int i = 0; i < 10000; i++) {
		ticks = tr_watchdog_tick(ticks, true);
		assert(ticks == 0u);
	}

	/* 2. A stale (ok=false) signal accumulates, and crosses the timeout
	 * at exactly the tick count main.c's fall_back() acts on -- the dead-
	 * pipeline case this watchdog exists to catch, still caught. */
	ticks = 0u;
	for (uint32_t i = 1; i <= TR_CALIB_TIMEOUT_TICKS; i++) {
		ticks = tr_watchdog_tick(ticks, false);
		assert(ticks == i);
	}
	assert(ticks >= TR_CALIB_TIMEOUT_TICKS);

	/* 3. Interleaved: a signal that flickers healthy at least once every
	 * TR_CALIB_TIMEOUT_TICKS-1 ticks never reaches the timeout, matching
	 * "CONSECUTIVE ticks of unresponsive" (main.c's own comment), not
	 * cumulative unresponsive time over the whole run. */
	ticks = 0u;
	for (int i = 0; i < 5000; i++) {
		bool ok = (i % (TR_CALIB_TIMEOUT_TICKS - 1u)) == 0u; /* healthy just often enough */

		ticks = tr_watchdog_tick(ticks, ok);
		assert(ticks < TR_CALIB_TIMEOUT_TICKS);
	}

	/* 4. A single stale ticks mid-run, surrounded by healthy ticks, never
	 * survives to the next tick's reset -- no partial accumulation leaks
	 * across a healthy tick. */
	ticks = tr_watchdog_tick(0u, true);
	assert(ticks == 0u);
	ticks = tr_watchdog_tick(ticks, false);
	assert(ticks == 1u);
	ticks = tr_watchdog_tick(ticks, true);
	assert(ticks == 0u);

	printf("PASS: tests/host/test_camera_watchdog.c\n");
	return 0;
}
