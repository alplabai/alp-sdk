/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * counter-alarm — schedule a one-shot alarm 100 ms in the future.
 *
 * The counter API is the lowest-level "wake me up later" primitive
 * the SDK exposes.  Apps that need millisecond-resolution
 * scheduling should usually reach for k_timer (Zephyr's higher-
 * level timer API) -- but counter is the right choice when you
 * need:
 *   - Microsecond resolution (for tight motor / signal generation)
 *   - Free-running tick reads as a high-precision timestamp source
 *   - A path that bypasses the kernel scheduler (interrupt context)
 *
 * The alarm callback runs in interrupt context on M-class targets;
 * keep the body short and avoid blocking calls.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/counter.h"

/* `volatile` because the callback runs from interrupt context on
 * the same hardware bus; the main thread polls the flag and the
 * compiler must not assume it doesn't change between iterations. */
static volatile bool fired = false;

/* Alarm callback.  The SDK invokes this from the counter's IRQ on
 * M-class targets.  Do *not* call printf/printk in production --
 * we use printk here only because it's safe from any context in
 * Zephyr; printf is not interrupt-safe. */
static void on_alarm(alp_counter_t *c, uint32_t ticks, void *user) {
    (void)c; (void)user;
    fired = true;
    /* Print the ticks value so callers can see when the alarm
     * actually fired (vs. when they expected it to). */
    printk("[counter] alarm fired @ ticks=%u\n", ticks);
}

int main(void) {
    printf("[counter] open id=0\n");

    alp_counter_t *c = alp_counter_open(&(alp_counter_config_t){
        /* counter_id 0 maps to the alp-counter0 DT alias.  Boards
         * with multiple general-purpose timers expose 1, 2, 3 too. */
        .counter_id = 0,
    });
    if (c == NULL) {
        printf("[counter] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[counter] done\n");
        return 0;
    }

    /* Start the counter -- it begins ticking from 0.  Without this
     * call set_alarm fails with NOT_READY. */
    alp_status_t s = alp_counter_start(c);
    printf("[counter] start -> %d\n", (int)s);

    /* Convert a wall-clock time (100 ms = 100,000 µs) into the
     * counter's native tick units.  Tick rate is hardware-specific
     * -- the SDK hides the conversion so app code stays portable. */
    uint32_t ticks_100ms = 0;
    s = alp_counter_us_to_ticks(c, 100000, &ticks_100ms);
    printf("[counter] 100ms = %u ticks (status=%d)\n",
           ticks_100ms, (int)s);

    /* Schedule the alarm `ticks_100ms` ticks from now.  At most one
     * alarm per handle; calling again replaces the prior schedule.
     * Pass NULL for `user` if the callback doesn't need state, or
     * stash a pointer to per-alarm context. */
    s = alp_counter_set_alarm(c, ticks_100ms, on_alarm, NULL);
    printf("[counter] set_alarm -> %d\n", (int)s);

    /* Spin-poll for the callback with a 250 ms ceiling.  This is
     * a smoke-test pattern; production code would either await an
     * event flag (k_event_wait) or do useful work between polls. */
    for (int i = 0; i < 25 && !fired; i++) k_msleep(10);
    printf("[counter] fired=%d\n", (int)fired);

    /* Close stops the counter and releases the handle.  Any pending
     * alarm is cancelled. */
    alp_counter_close(c);
    printf("[counter] done\n");
    return 0;
}
