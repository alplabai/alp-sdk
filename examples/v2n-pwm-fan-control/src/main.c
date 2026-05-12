/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-pwm-fan-control -- ramp a GD32-side PWM channel along a
 * fan-speed curve.  Demonstrates the GD32 bridge's PWM_SET opcode
 * driving a small-duty PWM signal at a fixed period, then walking
 * the duty cycle through a five-stop fan curve.
 *
 * Why GD32-side PWM on V2N?  Per the 2026-05-12 schematic decision,
 * all eight E1M PWM channels (PWM0..PWM7) are wired to the GD32 IO
 * MCU; the Renesas side drives no PWMs directly.  Driving any PWM
 * happens via the bridge: the host calls gd32g553_pwm_set, the GD32
 * firmware programs its own timer + GTIOC, and the carrier sees a
 * clean PWM waveform.  See docs/gd32-bridge-protocol.md §3.2 for the
 * channel mapping convention.
 *
 * The example treats PWM channel 0 as the fan-control output.  A
 * production firmware would read the carrier's thermistor /
 * temp-sensor + adjust duty in a control loop -- this example uses
 * a fixed five-stop ramp so the wave shape is observable on a scope
 * without needing live sensor data.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/gd32g553.h"

/* Fan-curve setpoints: each row is (duty_percent_x10, dwell_ms).
 * The x10 scaling gives one decimal of precision without floats. */
typedef struct {
    uint16_t duty_pct_x10;  /**< 0..1000  -- 0 = stopped, 1000 = full speed */
    uint16_t dwell_ms;
} fan_curve_step_t;

static const fan_curve_step_t fan_curve[] = {
    {   0u, 500u },  /* fan off: cold thermistor case */
    { 300u, 750u },  /* low:    e.g. CPU at 50 C */
    { 600u, 750u },  /* medium: e.g. CPU at 65 C */
    { 850u, 750u },  /* high:   e.g. CPU at 75 C */
    {1000u, 1000u }, /* max:    e.g. CPU at 80 C+ */
};

/* The fan controller's PWM channel on the GD32 firmware's
 * logical pwm_channel_map[] table.  The mapping is firmware-defined
 * (the host MUST NOT assume channel n == GD32 timer/GTIOC m) -- this
 * example assumes channel 0 is the fan output, matching the V2N
 * gd32-bridge default. */
#define FAN_PWM_CHANNEL          0u

/* PWM period.  25 kHz keeps the carrier above the audible range so
 * a 4-wire fan's tach line stays clean.  40 us = 25 kHz, 4000 ticks
 * of resolution per period at the firmware's hardware-achievable
 * timer base. */
#define FAN_PWM_PERIOD_NS    40000u

int main(void) {
    printf("[fan] v2n-pwm-fan-control\n");

    /* Open the SPI fast path to the GD32 bridge.  PWM_SET payloads
     * are small (10 bytes) so even the I2C management path would
     * work; SPI is preferred because fan-control updates can be
     * frequent enough that I2C contention with PMICs becomes
     * annoying on a busy bus.  bus_id 0 is the studio's resolved
     * SPI bus for V2N's RSPI master per the board.yaml above. */
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = 0u,
        .freq_hz       = 10000000u,    /* 10 MHz comfortable for the bridge */
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        .cs_pin_id     = 0u,
    });
    if (spi == NULL) {
        printf("[fan] alp_spi_open failed: %d\n", (int)alp_last_error());
        return 0;
    }

    /* Init the GD32 driver with the SPI handle.  Passing NULL for
     * the I2C handle is fine -- the driver routes every command over
     * SPI by default when both transports are present, and over
     * whichever one is open when only one is. */
    gd32g553_t gd32;
    alp_status_t s = gd32g553_init(&gd32, spi, NULL, 0u);
    if (s != ALP_OK) {
        printf("[fan] gd32g553_init -> %d\n", (int)s);
        alp_spi_close(spi);
        return 0;
    }

    /* Walk the fan curve forever.  Real firmware substitutes a
     * thermistor reading + closed-loop control -- the curve below
     * shows the wave shape a customer would observe on a scope. */
    for (;;) {
        for (size_t i = 0u; i < ARRAY_SIZE(fan_curve); ++i) {
            const fan_curve_step_t *step = &fan_curve[i];
            /* duty_ns = period_ns * duty_pct_x10 / 1000.  Integer
             * math: period_ns * duty_pct_x10 fits in 32 bits for
             * any duty up to 100% at 40 us period. */
            const uint32_t duty_ns =
                (uint32_t)((FAN_PWM_PERIOD_NS * step->duty_pct_x10) / 1000u);
            s = gd32g553_pwm_set(&gd32, FAN_PWM_CHANNEL,
                                 FAN_PWM_PERIOD_NS, duty_ns);
            if (s != ALP_OK) {
                printf("[fan] pwm_set -> %d (duty=%u/1000)\n",
                       (int)s, (unsigned)step->duty_pct_x10);
                /* Don't bail -- the bridge may be transiently busy
                 * (STATUS_BUSY -> ALP_ERR_BUSY); just keep walking
                 * the curve and let the next setpoint succeed.   */
            } else {
                printf("[fan] duty=%u/1000  period=%u ns  duty_ns=%u\n",
                       (unsigned)step->duty_pct_x10,
                       (unsigned)FAN_PWM_PERIOD_NS,
                       (unsigned)duty_ns);
            }
            k_msleep(step->dwell_ms);
        }
    }

    /* Unreachable in the demo -- but for completeness, this is the
     * tear-down a real app does when the controller shuts down: drop
     * the PWM (duty = 0), close the bus handle, close the driver. */
    gd32g553_pwm_set(&gd32, FAN_PWM_CHANNEL, FAN_PWM_PERIOD_NS, 0u);
    gd32g553_deinit(&gd32);
    alp_spi_close(spi);
    return 0;
}
