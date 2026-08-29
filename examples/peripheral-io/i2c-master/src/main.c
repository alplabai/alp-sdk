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
 * Hardware: the BMP581 barometer sits on BOARD_I2C_SENSORS on every
 * E1M and E1M-X EVK, strapped to 7-bit address 0x47 (SDO -> VDDIO)
 * on both -- see metadata/boards/e1m-evk.yaml and
 * metadata/boards/e1m-x-evk.yaml `i2c_devices:`.  On a brand-new
 * bring-up you may want to build and run the alp-sdk repo's
 * examples/peripheral-io/i2c-scanner example first to confirm which
 * address ACKs -- it isn't part of this scaffolded project.
 *
 * (#1269: this example used to target the TMP112 temperature
 * sensor, but TMP112 lives on BRD_I2C, not on BOARD_I2C_SENSORS --
 * opening BOARD_I2C_SENSORS and probing for TMP112 NACKs on real
 * hardware.  On the E1M-AEN family BRD_I2C is additionally the
 * slave-only Alif LPI2C0 -- ADR 0017 -- so the SoC can't master it
 * at all; see examples/v2n/v2n-temp-sensor for the V2N-only
 * BRD_I2C/TMP112 pattern.)
 *
 * What success looks like (real hardware):
 *
 *   [i2c-master] open BOARD_I2C_SENSORS @ 400 kHz
 *   [i2c-master] bmp581_init @ 0x47 -> 0 (OK)
 *   [i2c-master] sample 0: 96234 Pa, 23.625 degC
 *   [i2c-master] sample 1: 96231 Pa, 23.687 degC
 *   ...
 *   [i2c-master] done
 *
 * On native_sim (CI lane) the alp-i2c0 alias maps to the emul I2C
 * driver -- no BMP581 is registered as a target, so bmp581_init
 * gets NACKed and the example exits with the diagnostic.  Either
 * way the [i2c-master] done marker latches the harness.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/chips/bmp581.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> ALP_E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> ALP_E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* BMP581 7-bit I2C address with SDO -> VDDIO -- the on-board strap
 * on both the E1M EVK (U14) and E1M-X EVK (see
 * metadata/boards/e1m-evk.yaml / e1m-x-evk.yaml `i2c_devices:`).
 * SDO -> GND gives 0x46 instead; see the BMP581 datasheet
 * BST-BMP581-DS004 s5.6 or include/alp/chips/bmp581.h if your
 * board straps differently. */
#define BMP581_ADDR_7BIT BMP581_I2C_ADDR_HIGH /* 0x47 */

/* Number of samples to take before exiting.  Capped so the
 * native_sim build doesn't stall the twister harness; real
 * firmware would loop forever. */
#define SAMPLE_COUNT 5u

/* Wait between samples.  50 Hz ODR (below) means a fresh reading
 * every 20 ms; waiting a full second gives a comfortable margin
 * and prints once per watch-tick which is easy on the eyes. */
#define SAMPLE_PERIOD_MS 1000u

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[i2c-master] open BOARD_I2C_SENSORS @ 400 kHz\n");

	/* Open the bus at 400 kHz (I2C Fast-mode).  BMP581 supports up
     * to 1 MHz (Fast-mode Plus) per its datasheet, so 400 kHz is
     * comfortably inside spec; the SDK rounds DOWN to the
     * controller's closest achievable rate.  100 kHz is the safe
     * baseline for unknown devices. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS, /* E1M EVK: ALP_E1M_I2C0; E1M-X EVK: ALP_E1M_X_I2C0 */
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

	/* Initialise the BMP581 driver.  This reads CHIP_ID and verifies
     * it matches BMP581_CHIP_ID -- catches the "wrong address" case
     * (NACK on probe) up-front.  If init fails the example exits
     * cleanly -- maybe the chip isn't populated, maybe the address
     * is wrong, maybe the bus is held low by another device.
     * examples/peripheral-io/i2c-scanner can confirm which devices ACK. */
	bmp581_t     sensor;
	alp_status_t s = bmp581_init(&sensor, bus, BMP581_ADDR_7BIT);
	if (s != ALP_OK) {
		/* Most-frequent failure modes:
         *   * ALP_ERR_IO   -- bus error or NACK.  No BMP581 here,
         *                     wrong address, or pull-ups missing
         *                     (the bus floats high without them).
         *   * ALP_ERR_INVAL -- bad argument (NULL ctx or NULL bus).
         *
         * Use the alp-sdk repo's examples/peripheral-io/i2c-scanner
         * example to enumerate what IS on this bus before chasing a
         * BMP581 that may not be populated. */
		printf("[i2c-master] bmp581_init @ 0x%02x -> %d "
		       "(populated? right address?)\n",
		       BMP581_ADDR_7BIT,
		       (int)s);
		alp_i2c_close(bus);
		printf("[i2c-master] done\n");
		return 0;
	}
	printf("[i2c-master] bmp581_init @ 0x%02x -> %d (OK)\n", BMP581_ADDR_7BIT, (int)s);

	/* Configure oversampling + ODR + power mode in one call.  x8
     * oversampling at 50 Hz normal mode is a reasonable general-
     * purpose default; reach for higher OSR when you want lower
     * noise at the cost of more power/latency. */
	s = bmp581_set_sampling(
	    &sensor, BMP581_OSR_X8, BMP581_OSR_X8, BMP581_ODR_50_HZ, BMP581_MODE_NORMAL);
	if (s != ALP_OK) {
		printf("[i2c-master] bmp581_set_sampling -> %d\n", (int)s);
		/* Non-fatal: the chip stays at whatever mode init left it in. */
	}

	/* Sample loop: read SAMPLE_COUNT pressure+temperature pairs, one
     * per second.  Real-life firmware would publish each reading
     * over MQTT, push to a ring buffer for trend analysis, or
     * compare against an alert threshold and pull a GPIO. */
	for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
		bmp581_raw_t         raw  = { 0 };
		bmp581_compensated_t comp = { 0 };
		s                         = bmp581_read_raw(&sensor, &raw);
		if (s == ALP_OK) {
			s = bmp581_compensate(&raw, &comp);
		}
		if (s == ALP_OK) {
			/* Format integer + fractional parts so we avoid float
             * printf on M-class targets.  temperature_c1000 is
             * signed -- the fractional part takes the absolute
             * value so e.g. -1750 milli-C prints as "-1.750 degC"
             * (not "-1.-750 degC"). */
			int whole = comp.temperature_c1000 / 1000;
			int frac =
			    (comp.temperature_c1000 < 0 ? -comp.temperature_c1000 : comp.temperature_c1000) %
			    1000;
			printf(
			    "[i2c-master] sample %u: %d Pa, %d.%03d degC\n", i, comp.pressure_pa, whole, frac);
		} else {
			/* Read/compensate errors during steady-state are rare --
             * usually a transient bus glitch (EMI, ground bounce).
             * Log and continue rather than aborting; the next
             * sample will likely succeed. */
			printf("[i2c-master] sample %u: read -> %d\n", i, (int)s);
		}
		alp_delay_ms(SAMPLE_PERIOD_MS);
	}

	/* Clean shutdown -- deinit the chip driver, then close the bus
     * handle (which releases the slot back to the pool). */
	bmp581_deinit(&sensor);
	alp_i2c_close(bus);
	printf("[i2c-master] done\n");
	return 0;
}
