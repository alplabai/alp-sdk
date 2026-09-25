/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-temp-sensor -- read the on-module TMP112 temperature sensor
 * once per second and print the value.  Classic V2N starter app.
 *
 * RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive in a55_boot mode
 * (metadata/e1m_modules/v2n/core-ownership.yaml) -- the CM33 masters it
 * only transiently during its own cm33_boot rail sequence
 * (examples/v2n/v2n-cm33-deepx-rail), never here.  This is a Linux/Yocto user-space app on the
 * V2N Cortex-A55, following the same `alp_i2c_*` + chip-driver
 * pattern as examples/v2n/v2n-power-monitor.
 *
 * On V2N + V2N-M1 the TMP112 sits on BRD_I2C (Renesas RIIC8) at
 * 7-bit address 0x40 (ADD0 strapped to GND on the fitted
 * TMP112DIDPWR X2SON-5 package -- maintainer-confirmed 2026-09-24,
 * see metadata/chips/tmp112.yaml; TMP112_I2C_ADDR_ADDRVAR_GND, NOT
 * the naive-datasheet TMP112_I2C_ADDR_GND=0x48).  Linux numbers
 * BRD_I2C as adapter 8: meta-alp-sdk's e1m-v2n-som.dtsi aliases
 * `i2c8 = &i2c8;`, so `alp_i2c_open(.bus_id = 8)` here opens
 * `/dev/i2c-8`.
 */

#include <stdio.h>
#include <unistd.h>

#include "alp/peripheral.h"
#include "alp/chips/tmp112.h"

/* BRD_I2C = Linux /dev/i2c-8 -- see the file header for the DT-alias
 * citation.  A literal here (not a board-header macro) because BRD_I2C
 * is a SoM-level bus, not one of the E1M-X-EVK carrier's own routed
 * pins (those get generated ALP_E1M_X_I2Cn macros; this doesn't). */
#define V2N_BRD_I2C_BUS_ID 8u

int main(void)
{
	printf("[temp] v2n-temp-sensor\n");

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = V2N_BRD_I2C_BUS_ID,
	    .bitrate_hz = 400000u,
	});
	if (bus == NULL) {
		printf("[temp] alp_i2c_open failed: err=%d\n", (int)alp_last_error());
		return 1;
	}

	tmp112_t     sensor;
	alp_status_t s = tmp112_init(&sensor, bus, TMP112_I2C_ADDR_ADDRVAR_GND);
	if (s != ALP_OK) {
		/* Bus reachable but the TMP112 isn't ACKing -- either the
		 * chip isn't populated, the address is wrong, or the bus
		 * has another device colliding.  i2cdetect(8) on target
		 * can confirm which devices ACK. */
		printf("[temp] tmp112_init -> %d "
		       "(populated? right address?)\n",
		       (int)s);
		alp_i2c_close(bus);
		return 1;
	}

	/* Read + print 10 samples one second apart.  Real production
	 * firmware would post the value to a logging subsystem or
	 * publish over MQTT. */
	for (int i = 0; i < 10; ++i) {
		int32_t milli_c = 0;
		s               = tmp112_read_temp_milli_c(&sensor, &milli_c);
		if (s == ALP_OK) {
			/* Divide/mod the magnitude, not milli_c itself: integer
			 * division truncates toward zero, so -500/1000 == 0 and a
			 * value between -999 and 0 would silently lose its sign
			 * (-0.5 degC would print "0.500"). Prepend "-" by hand
			 * instead -- same fix as v2n-brd-i2c-bringup's
			 * probe_tmp112(). */
			int32_t abs_mc = milli_c < 0 ? -milli_c : milli_c;
			int     whole  = (int)(abs_mc / 1000);
			int     frac   = (int)(abs_mc % 1000);
			printf("[temp] sample %d: %s%d.%03d degC\n", i, milli_c < 0 ? "-" : "", whole, frac);
		} else {
			printf("[temp] sample %d: read failed (status=%d)\n", i, (int)s);
		}
		sleep(1);
	}

	tmp112_deinit(&sensor);
	alp_i2c_close(bus);
	printf("[temp] done\n");
	return 0;
}
