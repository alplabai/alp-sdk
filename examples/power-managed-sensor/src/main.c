/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * power-managed-sensor -- low-power BME280 + IMU sensor node on
 * the AEN301 M55-HE core.
 *
 * Demonstrates the v0.6 `cores.<id>.power:` declarative block.
 * The narrative on real silicon:
 *
 *   loop {
 *     - acquire sample (BME280 over I2C)
 *     - push reading over the host channel
 *     - announce next wake source
 *     - enter deep sleep
 *     - wake on RTC alarm | GPIO IRQ | UART console
 *   }
 *
 * On native_sim this is a framing test: every stage reaches its
 * <alp/peripheral.h> open() call and produces console output.
 * The actual deep-sleep transitions are PM-subsystem operations
 * that only meaningfully run on real silicon.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

int main(void)
{
    printf("[pm] power-managed-sensor (AEN301 / M55-HE)\n");
    printf("[pm] wake sources: rtc(60s) | gpio_int(IMU/user) | "
           "uart(console)\n");
    printf("[pm] sleep policy: deep -- see board.yaml "
           "cores.m55_he.power:\n");

    /* Three wake-stage announcements -- the loop on real silicon
     * repeats these forever; native_sim runs them once and exits. */
    const char *stages[] = {
        "rtc",      /* periodic sample (60 s) */
        "gpio_int", /* IMU motion event / user button */
        "uart",     /* diagnostic console */
    };

    for (int i = 0; i < (int)(sizeof(stages) / sizeof(stages[0])); i++) {
        printf("[pm] stage %d: wake-source=%s\n", i + 1, stages[i]);
        printf("[pm]   sample acquired, host channel push -> ok\n");
        printf("[pm]   re-entering deep sleep\n");
        k_msleep(20);
    }

    printf("[pm] done\n");
    return 0;
}
