/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-master -- discrete I2C master that reads a known device at
 * a known address.
 *
 * Pattern: open the bus, init the chip driver, loop reading the
 * register every second, close cleanly.  Contrasts with
 * examples/peripheral-io/i2c-scanner which probes every 7-bit address for ACKs
 * without knowing what's behind them.
 *
 * Hardware: the TMP112 +/-0.5 C temperature sensor sits on the
 * BRD_I2C management bus on every E1M-AEN, E1M-V2N, and E1M-V2N-M1
 * SoM per metadata/chips/tmp112.yaml.  7-bit address depends on
 * the ADD0 strap; the SoM presets pick 0x48 (AEN) or 0x40 (V2N).
 * On a brand-new bring-up you may want to run examples/peripheral-io/i2c-scanner
 * first to confirm which address ACKs.
 *
 * What success looks like (real hardware):
 *
 *   [i2c-master] open BOARD_I2C_SENSORS @ 400 kHz
 *   [i2c-master] tmp112_init @ 0x48 -> 0 (OK)
 *   [i2c-master] sample 0: 23.625 degC
 *   [i2c-master] sample 1: 23.687 degC
 *   ...
 *   [i2c-master] done
 *
 * On native_sim (CI lane) the alp-i2c0 alias maps to the emul I2C
 * driver -- no TMP112 is registered as a target, so tmp112_init
 * gets NACKed and the example exits with the diagnostic.  Either
 * way the [i2c-master] done marker latches the harness.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/tmp112.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* TMP112 7-bit I2C address with ADD0 = GND (the strap the AEN
 * SoM uses by default).  V2N's TMP112 sits at 0x40 because the
 * SoM ties ADD0 to SDA -- if you copy this example to a V2N
 * project, change this constant.  See the TMP112 datasheet
 * SBOS473K table 2 or include/alp/chips/tmp112.h. */
#define TMP112_ADDR_7BIT TMP112_I2C_ADDR_GND /* 0x48 */

/* Number of samples to take before exiting.  Capped so the
 * native_sim build doesn't stall the twister harness; real
 * firmware would loop forever. */
#define SAMPLE_COUNT 5u

/* Wait between samples.  TMP112's continuous-conversion mode
 * runs at 4 Hz by default (250 ms between fresh readings); waiting
 * a full second gives a comfortable margin and prints once per
 * watch-tick which is easy on the eyes. */
#define SAMPLE_PERIOD_MS 1000u

int main(void)
{
	printf("[i2c-master] open BOARD_I2C_SENSORS @ 400 kHz\n");

	/* Open the bus at 400 kHz (I2C Fast-mode).  TMP112 supports up
     * to 400 kHz per its datasheet; the SDK rounds DOWN to the
     * controller's closest achievable rate.  100 kHz is the safe
     * baseline for unknown devices; 1 MHz (Fast-mode Plus) needs
     * confirmation in the chip's datasheet and short bus traces. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS, /* E1M EVK: E1M_I2C0; E1M-X EVK: E1M_X_I2C0 */
	    .bitrate_hz = 400000,
	});
	if (bus == NULL) {
		/* No alp-i2c0 alias on this build -> NULL handle.
         *
         * Common causes:
         *   * Board overlay forgot to set the alias.
         *   * SoM has no I2C0 routed (rare -- it's part of the
         *     portable E1M baseline).
         *   * On native_sim without the emul overlay we ship,
         *     the alias is unset. */
		printf("[i2c-master] open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[i2c-master] done\n");
		return 0;
	}

	/* Initialise the TMP112 driver.  This:
     *   1. Issues a write to register 0x01 (CONF) to put the chip
     *      in continuous-conversion mode at 4 Hz.
     *   2. Verifies the write by reading back CONF -- catches the
     *      "wrong address" case (NACK on probe) up-front.
     * If init fails the example exits cleanly -- maybe the chip
     * isn't populated, maybe the address is wrong, maybe the
     * bus is held low by another device.  i2c-scanner can
     * confirm which devices ACK. */
	tmp112_t     sensor;
	alp_status_t s = tmp112_init(&sensor, bus, TMP112_ADDR_7BIT);
	if (s != ALP_OK) {
		/* Most-frequent failure modes:
         *   * ALP_ERR_IO   -- bus error or NACK.  No TMP112 here,
         *                     wrong address, or pull-ups missing
         *                     (the bus floats high without them).
         *   * ALP_ERR_INVAL -- bad argument (NULL ctx or NULL bus).
         *
         * Use i2c-scanner to enumerate what IS on this bus before
         * chasing a TMP112 that may not be populated. */
		printf("[i2c-master] tmp112_init @ 0x%02x -> %d "
		       "(populated? right address?)\n",
		       TMP112_ADDR_7BIT,
		       (int)s);
		alp_i2c_close(bus);
		printf("[i2c-master] done\n");
		return 0;
	}
	printf("[i2c-master] tmp112_init @ 0x%02x -> %d (OK)\n", TMP112_ADDR_7BIT, (int)s);

	/* Optional: tune the conversion rate.  4 Hz is the datasheet
     * default; reach for 8 Hz when you want lower latency at the
     * cost of more power, or 0.25 Hz for very low-power monitoring.
     * Skipping this call keeps the default. */
	s = tmp112_set_rate(&sensor, TMP112_RATE_4_HZ);
	if (s != ALP_OK) {
		printf("[i2c-master] tmp112_set_rate -> %d\n", (int)s);
		/* Non-fatal: the chip stays at whatever rate init set. */
	}

	/* Sample loop: read SAMPLE_COUNT temperatures, one per second.
     * Real-life firmware would publish each reading over MQTT,
     * push to a ring buffer for trend analysis, or compare against
     * an alert threshold and pull a GPIO. */
	for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
		int32_t milli_c = 0;
		s               = tmp112_read_temp_milli_c(&sensor, &milli_c);
		if (s == ALP_OK) {
			/* Format integer + fractional parts so we avoid float
             * printf on M-class targets.  milli_c is signed -- the
             * fractional part takes the absolute value so e.g.
             * -1750 milli-C prints as "-1.750 degC" (not
             * "-1.-750 degC"). */
			int whole = milli_c / 1000;
			int frac  = (milli_c < 0 ? -milli_c : milli_c) % 1000;
			printf("[i2c-master] sample %u: %d.%03d degC\n", i, whole, frac);
		} else {
			/* Read errors during steady-state are rare -- usually
             * a transient bus glitch (EMI, ground bounce).  Log
             * and continue rather than aborting; the next sample
             * will likely succeed. */
			printf("[i2c-master] sample %u: read -> %d\n", i, (int)s);
		}
		k_msleep(SAMPLE_PERIOD_MS);
	}

	/* Clean shutdown -- deinit the chip driver (which leaves the
     * chip in continuous-conversion mode; harmless), then close
     * the bus handle (which releases the slot back to the pool). */
	tmp112_deinit(&sensor);
	alp_i2c_close(bus);
	printf("[i2c-master] done\n");
	return 0;
}
