/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * timer-periodic-interrupt -- the canonical "periodic ISR plus
 * main-thread coordination" example.
 *
 * Pattern:
 *
 *   1. Open a free-running counter + a GPIO output pin for the LED.
 *   2. Schedule a one-shot alarm 100 ms in the future.
 *   3. When the alarm fires (in IRQ context), the callback:
 *        a. Flips a `volatile bool` flag.
 *        b. Re-arms itself for the NEXT 100 ms tick.  (The
 *           counter API is one-shot per call; periodic timing
 *           is built by re-arming inside the callback.)
 *   4. The main thread polls the flag, clears it, toggles the
 *      LED, and waits for the next tick.
 *
 * Why this dance instead of just calling alp_gpio_write() from
 * the ISR?
 *
 *   * **alp_gpio_write IS safe from ISR context.**  This
 *     particular call is short + doesn't block.  But more
 *     complicated ISRs would block on alp_i2c_write, allocate
 *     memory, or take a mutex -- all forbidden from IRQ.
 *   * **printf / printk + LOG_* are NOT safe.**  printf uses
 *     standard I/O internally which takes a mutex; LOG_INF
 *     allocates from a log buffer; printk is safe but slow.
 *     None of them belong in an ISR's hot path.
 *
 * The "flag + main-thread drain" pattern keeps the ISR minimal
 * and lets the main thread do all the I/O.  It's the
 * recommended idiom for any periodic work that needs to log,
 * publish to a network, or trigger anything beyond a single
 * register write.
 *
 * The LED: the E1M-EVK has no dedicated GPIO LED, so we drive the
 * red RGB pad (default function PWM3) as a plain digital GPIO via
 * the parallel `EVK_PIN_LED_RED` index -- the e1m-spec "GPIO
 * secondary" capability (see e1m_pinout.h "Pin-as-GPIO fallback").
 *
 * What success looks like:
 *
 *   [timer] open counter=0
 *   [timer] start -> 0
 *   [timer] 100ms = N ticks (status=0)
 *   [timer] open LED on EVK_PIN_LED_RED
 *   [timer] arming first alarm
 *   [timer] tick 0 fired @ N+0 ticks, LED -> 1
 *   [timer] tick 1 fired @ N+1 ticks, LED -> 0
 *   ...
 *   [timer] done
 *
 * On the EVK the RGB-red LED blinks at 5 Hz (200 ms period, 50%
 * duty since we toggle every 100 ms).
 *
 * On the V2N supervisor backend the counter alarm callback
 * returns ALP_ERR_NOSUPPORT (the GD32 IO MCU has no interrupt
 * line back to the Renesas host) -- the example prints the
 * diagnostic and exits.  Run this example on AEN / native_sim
 * for the working ISR path.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/counter.h"
#include "alp/boards/alp_e1m_evk_routes.h"

/* Alarm period.  100 ms gives a visible LED blink (5 Hz toggle)
 * that's easy to count by eye without being so slow that the
 * example takes minutes to run. */
#define ALARM_PERIOD_US 100000u

/* Number of ticks to run before exiting.  Capped so the
 * native_sim build doesn't stall the twister harness; real
 * firmware would loop forever. */
#define TICK_COUNT 5u

/* ------------------------------------------------------------------
 * Shared state between ISR + main thread.
 *
 * The `volatile` qualifier matters here: without it the compiler
 * is free to cache the flag in a register inside the main loop
 * and never notice the ISR has updated memory.  `volatile`
 * forces a fresh load on every read.
 *
 * For multi-byte / multi-word state across an ISR boundary on
 * multi-core systems you'd also need atomic_* primitives or a
 * lock; here a single bool fits in one byte so the read+clear
 * dance is naturally atomic on M-class targets.
 * ------------------------------------------------------------------ */

static volatile bool     g_tick_fired = false;
static volatile uint32_t g_tick_count = 0u;

/* Saved tick value at the moment the most-recent alarm fired.
 * Updated in ISR; read once by main thread per tick. */
static volatile uint32_t g_last_tick_value = 0u;

/* Saved counter handle so the ISR can re-arm itself.  The
 * counter pointer is set once during setup, then read by the
 * ISR -- no synchronisation needed.  100 ms ticks per call so
 * we accumulate the right number for periodicity. */
static alp_counter_t *g_counter      = NULL;
static uint32_t       g_period_ticks = 0u;

/* ------------------------------------------------------------------
 * Alarm callback.  Runs in IRQ context on M-class targets.
 *
 * Rules:
 *   * NO printf / printk / LOG_* (use a flag + main thread).
 *   * NO blocking calls (alp_i2c_*, alp_spi_*, k_sleep, ...).
 *   * NO memory allocation (k_malloc, etc.).
 *   * Keep the body short -- microseconds, not milliseconds.
 *
 * What we do: stash the tick value, flip a flag, re-arm.
 * ------------------------------------------------------------------ */
static void on_periodic_alarm(alp_counter_t *c, uint32_t ticks, void *user)
{
    (void)user;
    g_last_tick_value = ticks;
    g_tick_count++;
    g_tick_fired = true;

    /* Re-arm for the next period.  The counter API is one-shot
     * per call -- this is how you build "periodic" out of "one
     * shot".  The set_alarm call IS safe from ISR (it just
     * writes a compare register on the counter peripheral). */
    (void)alp_counter_set_alarm(c, g_period_ticks, on_periodic_alarm, NULL);
}

int main(void)
{
    printf("[timer] open counter=%u\n", E1M_COUNTER0);

    /* Open counter 0.  Channel choice matters less for this
     * example than for hardware-PWM-tied timer usage; counter 0
     * is conventionally the "general purpose" timer in
     * E1M-conformant SoMs. */
    alp_counter_t *c = alp_counter_open(&(alp_counter_config_t){
        .counter_id = E1M_COUNTER0,
    });
    if (c == NULL) {
        /* Likely causes:
         *   * No alp-counter0 alias on this build.
         *   * Counter pool exhausted (rare).
         *   * On native_sim there's no counter device by
         *     default -- alp_counter_open returns NULL with
         *     ALP_ERR_NOT_READY. */
        printf("[timer] open counter failed: alp_last_error=%d\n", (int)alp_last_error());
        printf("[timer] done\n");
        return 0;
    }
    g_counter = c;

    /* Start the counter ticking.  Without this set_alarm fails
     * with NOT_READY (the counter has to be running for the
     * compare unit to fire). */
    alp_status_t s = alp_counter_start(c);
    printf("[timer] start -> %d\n", (int)s);

    /* Convert wall-clock microseconds to native counter ticks.
     * Tick rate is hardware-specific; the SDK hides the
     * conversion so app code stays portable across SoMs. */
    s = alp_counter_us_to_ticks(c, ALARM_PERIOD_US, &g_period_ticks);
    printf("[timer] %u us = %u ticks (status=%d)\n", ALARM_PERIOD_US, g_period_ticks, (int)s);
    if (s != ALP_OK) {
        /* On the V2N supervisor backend this returns NOSUPPORT
         * because the bridge can't report the GD32 timer's tick
         * frequency to the host without a protocol-v0.3 opcode.
         * Bail cleanly. */
        printf("[timer] us_to_ticks not supported on this backend; "
               "this example is AEN / native_sim today\n");
        alp_counter_close(c);
        printf("[timer] done\n");
        return 0;
    }

    /* Open the user LED.  The EVK has no plain GPIO LED, so the
     * indicator is the RGB-red pad (default function PWM3) claimed
     * as a digital GPIO via EVK_PIN_LED_RED; on a board with a real
     * GPIO LED, swap the index for whatever your LED maps to (or
     * comment the GPIO out if you only need the timer half). */
    printf("[timer] open LED on EVK_PIN_LED_RED\n");
    alp_gpio_t *led = alp_gpio_open(EVK_PIN_LED_RED);
    if (led != NULL) {
        s = alp_gpio_configure(led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
        if (s != ALP_OK) {
            printf("[timer] gpio_configure -> %d\n", (int)s);
            alp_gpio_close(led);
            led = NULL;
        }
    }
    /* If led is NULL after this point the example still works --
     * we just won't have a physical LED to wiggle; the printf
     * trace shows the ISR is firing correctly. */

    /* Arm the first alarm.  The callback re-arms itself for
     * subsequent periods -- this gives us "periodic" out of the
     * "one-shot" counter API.  On a backend that can't dispatch
     * the callback (V2N supervisor) this returns NOSUPPORT. */
    printf("[timer] arming first alarm\n");
    s = alp_counter_set_alarm(c, g_period_ticks, on_periodic_alarm, NULL);
    if (s != ALP_OK) {
        printf("[timer] set_alarm -> %d (NOSUPPORT on V2N supervisor)\n", (int)s);
        if (led != NULL) alp_gpio_close(led);
        alp_counter_close(c);
        printf("[timer] done\n");
        return 0;
    }

    /* ------------------------------------------------------------------
     * Main loop -- drain the flag, toggle the LED, log.
     *
     * Polling a flag in the main loop is the simplest form of
     * ISR -> main-thread coordination.  For higher fan-out
     * (multiple ISRs each contributing events) you'd use a
     * k_event or k_msgq instead so the main thread can wait()
     * efficiently rather than busy-checking.
     * ------------------------------------------------------------------ */
    bool led_state = false;
    while (g_tick_count < TICK_COUNT) {
        /* Spin until the ISR sets the flag.  k_msleep yields the
         * CPU so the kernel idle thread / lower-priority work
         * can run during the wait.  10 ms is conservative -- on
         * a 100 ms alarm cycle that's at most a 10% latency hit
         * between alarm fire and LED update. */
        if (!g_tick_fired) {
            k_msleep(10);
            continue;
        }

        /* Drain the flag FIRST, then read the data the ISR
         * wrote.  This ordering matters when the ISR could fire
         * multiple times between drains: if we read the data
         * first, then cleared the flag, a fresh ISR between
         * those two steps would update the data + leave the
         * flag set -- and we'd toggle once for two alarms.
         *
         * On a single-core M-class target with simple
         * volatile flag access this race is microsecond-narrow
         * but still exists.  For multi-core systems use
         * atomic_set / atomic_clear instead. */
        g_tick_fired        = false;
        uint32_t tick_no    = g_tick_count;
        uint32_t tick_value = g_last_tick_value;

        /* Toggle the LED.  alp_gpio_write IS safe from any
         * context; we run it from main thread for symmetry with
         * the printf below (which is NOT safe from ISR). */
        led_state = !led_state;
        if (led != NULL) {
            (void)alp_gpio_write(led, led_state);
        }

        /* Now safe to log -- main thread context, printf is
         * fine.  Including the tick number, the counter value
         * at the moment the alarm fired, and the new LED state
         * gives a complete event trace. */
        printf("[timer] tick %u fired @ %u ticks, LED -> %d\n", tick_no, tick_value,
               (int)led_state);
    }

    /* Clean teardown -- cancel any pending alarm, close the GPIO
     * handle, close the counter handle.  Counter handle's close
     * also stops the counter; nothing left in flight. */
    (void)alp_counter_cancel_alarm(c);
    if (led != NULL) alp_gpio_close(led);
    alp_counter_close(c);
    printf("[timer] done\n");
    return 0;
}
