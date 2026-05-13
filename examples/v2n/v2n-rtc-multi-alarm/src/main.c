/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-rtc-multi-alarm -- exercise the rv3028c7 multi-source event
 * dispatcher.  Registers per-source handlers, enables a subset of
 * the seven RV-3028-C7 interrupt sources (PORF / EXT_EVENT /
 * ALARM / COUNTDOWN / PERIODIC / BSF / CLKF), and demonstrates the
 * dispatch-from-IRQ pattern.
 *
 * The real production usage is:
 *   - Carrier-side ISR fires when the INT pin falls.
 *   - The ISR posts a work item that calls rv3028c7_dispatch_irq().
 *   - dispatch_irq reads STATUS, fans out to the per-source
 *     handlers we registered below, and write-0-to-clears every
 *     fired flag.
 *
 * This example doesn't wire a real INT-pin GPIO -- it calls
 * dispatch_irq directly from main() to show the flow.  On real
 * hardware, replace the manual call with an IRQ-driven path
 * (e.g. alp_gpio_irq_enable on the RTC_ALARM pin).
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/rv3028c7.h"

/* Per-source handler.  Production code would route to whatever
 * work-queue / state machine the application uses.  Here we just
 * print so the example's output makes the dispatch flow obvious. */
static void on_alarm(rv3028c7_t *ctx, rv3028c7_src_t src, void *user) {
    (void)ctx; (void)user;
    printf("[rtc] dispatched src=%d (ALARM)\n", (int)src);
}

static void on_periodic(rv3028c7_t *ctx, rv3028c7_src_t src, void *user) {
    (void)ctx; (void)user;
    printf("[rtc] dispatched src=%d (PERIODIC)\n", (int)src);
}

static void on_porf(rv3028c7_t *ctx, rv3028c7_src_t src, void *user) {
    (void)ctx; (void)user;
    printf("[rtc] dispatched src=%d (PORF -- chip just came out of POR)\n",
           (int)src);
}

static void on_backup_switch(rv3028c7_t *ctx, rv3028c7_src_t src, void *user) {
    (void)ctx; (void)user;
    printf("[rtc] dispatched src=%d (BSF -- power switched VDD <-> VBAT)\n",
           (int)src);
}

int main(void) {
    printf("[rtc] v2n-rtc-multi-alarm\n");

    /* Open the RTC's I2C bus -- on V2N this is BRD_I2C; on AEN
     * the RTC sits on Alif LPI2C.  Both reach the RV-3028-C7 at
     * 7-bit address 0x52. */
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,
        .bitrate_hz = 400000u,
    });
    if (bus == NULL) {
        printf("[rtc] alp_i2c_open failed: err=%d\n",
               (int)alp_last_error());
        return 0;
    }

    rv3028c7_t rtc;
    alp_status_t s = rv3028c7_init(&rtc, bus);
    if (s != ALP_OK) {
        printf("[rtc] rv3028c7_init -> %d\n", (int)s);
        alp_i2c_close(bus);
        return 0;
    }

    /* Register per-source handlers.  We don't need to register
     * every source -- unregistered sources are silently ignored
     * by the dispatcher.  Typical app: register the few it cares
     * about (alarm + power-switch are the common ones). */
    rv3028c7_register_handler(&rtc, RV3028C7_SRC_PORF,     on_porf,           NULL);
    rv3028c7_register_handler(&rtc, RV3028C7_SRC_ALARM,    on_alarm,          NULL);
    rv3028c7_register_handler(&rtc, RV3028C7_SRC_PERIODIC, on_periodic,       NULL);
    rv3028c7_register_handler(&rtc, RV3028C7_SRC_BSF,      on_backup_switch,  NULL);

    /* Mask the corresponding INT-enable bits.  Without this the
     * source's flag still latches in STATUS (and dispatch_irq still
     * fans it out), but the chip's physical INT pin doesn't fall. */
    rv3028c7_set_int_enable(&rtc, RV3028C7_SRC_ALARM,    true);
    rv3028c7_set_int_enable(&rtc, RV3028C7_SRC_PERIODIC, true);
    rv3028c7_set_int_enable(&rtc, RV3028C7_SRC_BSF,      true);

    /* Configure a 30-second-from-now alarm.  In a production app
     * you'd compute the absolute time and the match mask the alarm
     * should fire on; for the example we just set a recent time. */
    rv3028c7_time_t now = {
        .year = 2026, .month = 5, .day = 12,
        .hour = 10, .minute = 0, .second = 0,
        .weekday = 2, /* Tuesday */
    };
    rv3028c7_set_time(&rtc, &now);

    rv3028c7_time_t alarm = now;
    alarm.minute = 0;  /* fire when minutes hits 00 -- recurring once an hour */
    rv3028c7_alarm_match_t match = {
        .match_minute        = true,
        .match_hour          = false,
        .match_day_or_weekday= false,
    };
    rv3028c7_set_alarm(&rtc, &alarm, &match);
    rv3028c7_alarm_int_enable(&rtc, true);

    /* On real hardware the INT pin would now route to a Renesas
     * GPIO + ISR; the ISR posts a work item that calls
     * rv3028c7_dispatch_irq.  Without that wiring, simulate by
     * calling dispatch_irq directly. */
    printf("[rtc] dispatching once to show the flow...\n");
    uint8_t status_seen = 0u;
    s = rv3028c7_dispatch_irq(&rtc, &status_seen);
    printf("[rtc] dispatch_irq -> status=%d  status_byte=0x%02x\n",
           (int)s, (unsigned)status_seen);

    /* Optional: route the CLKOUT pin to a different source so the
     * board has a second physical interrupt line for the alarm.
     * Carriers that wire CLKOUT as a programmable IRQ use this
     * to route alarm separately from the generic INT pin. */
    s = rv3028c7_route_clkout(&rtc, RV3028C7_SRC_ALARM);
    printf("[rtc] route_clkout(ALARM) -> status=%d\n", (int)s);

    rv3028c7_alarm_int_enable(&rtc, false);
    rv3028c7_set_int_enable(&rtc, RV3028C7_SRC_PERIODIC, false);
    rv3028c7_set_int_enable(&rtc, RV3028C7_SRC_BSF,      false);
    alp_i2c_close(bus);
    printf("[rtc] done\n");
    return 0;
}
