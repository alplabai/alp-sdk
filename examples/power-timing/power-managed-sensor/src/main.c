/*
 * Copyright 2026 Alp Lab AB
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
 * What this source actually does
 * ==============================
 *
 * This file is a *framing* demo, NOT a working sensor driver.  It
 * prints the duty-cycle narrative above so you can see the wake-
 * source / sleep-policy shape on any host, including native_sim --
 * but it does NOT open the BME280, push a host channel, or enter a
 * real PM sleep state.  Those are real-silicon PM-subsystem +
 * peripheral operations; wiring them here would not run on
 * native_sim (no I2C sensor, no deep-sleep transition).
 *
 * The teaching point is the DECLARATIVE side: the `power:` block in
 * board.yaml drives the generated CONFIG_PM_* set (see README), and
 * the sample cadence is an app concern -- represented here by the
 * RTC_TICK_S macro below.  A production node replaces the printf
 * bodies with the real alp_* sensor + PM calls; the policy stays in
 * board.yaml unchanged.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

/* Steady-state RTC wake cadence, in seconds.  This is the app-level
 * sample period: board.yaml declares "RTC is a wake source," and the
 * app decides how often to fire.  Edit this to reship a different
 * cadence (a production node also programs the platform counter's
 * alarm channel to match -- not done in this framing demo). */
#define RTC_TICK_S 60

int main(void)
{
	printf("[pm] power-managed-sensor (AEN301 / M55-HE)\n");
	printf("[pm] wake sources: rtc(%ds) | gpio_int(IMU/user) | "
	       "uart(console)\n",
	       RTC_TICK_S);
	printf("[pm] sleep policy: deep -- see board.yaml "
	       "cores.m55_he.power:\n");

	/* Three wake-stage announcements.  These are printf framing
     * only -- the "sample acquired / push" lines mark where a real
     * node would call its alp_* sensor + host-channel APIs.  On real
     * silicon the loop repeats forever; this framing demo runs the
     * stages once and exits. */
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
