/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * v2n-power-monitor -- print a live per-rail power table from the
 * E1M-X EVK's on-board INA236 current/voltage monitors.
 *
 * What this demonstrates
 * ----------------------
 *   - Opening an Alp SDK I2C bus from a Linux/Yocto user-space app
 *     on the V2N Cortex-A55 (the portable `alp_i2c_*` surface maps
 *     onto `/dev/i2c-N` here, the same source compiles on Zephyr).
 *   - Driving several instances of a portable chip driver
 *     (`ina236_*`) over one shared bus, each calibrated for its
 *     rail's shunt resistor.
 *   - Reading bus voltage + current + power in one transaction via
 *     `ina236_read_all()` and converting the raw fixed-point fields
 *     to volts / milliamps / milliwatts.
 *
 * Scope
 * -----
 * The INA236 monitors are an *EVK-only* instrumentation feature --
 * production E1M-X SoMs do not carry them -- so this is a bring-up
 * / demo utility, not a production telemetry path.  Rail map +
 * shunt values come from <alp/boards/alp_e1m_x_evk.h>.
 *
 * The monitors sit on the on-board sensor I2C bus
 * (XEVK_I2C_BUS_SENSORS = E1M_X_I2C0, i.e. Linux /dev/i2c-0).
 *
 * Build (Yocto SDK):  see this example's README.md.
 * Run on target:      ./v2n-power-monitor   (Ctrl-C to stop)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "alp/peripheral.h"
#include "alp/chips/ina236.h"
#include "alp/boards/alp_e1m_x_evk.h"        /* INA236 addresses + shunt calibration */
#include "alp/boards/alp_e1m_x_evk_routes.h" /* XEVK_I2C_BUS_SENSORS                 */

/*
 * The five EVK rails.  Address + shunt + max-current come straight
 * from the board header so the calibration constants live in one
 * place (the board definition), not scattered through app code.
 */
static const struct rail_def {
	const char *name;
	uint8_t     addr;
	float       shunt_ohms;
	float       max_a;
} k_rails[] = {
	{ "3V3", XEVK_I2C_ADDR_INA236_3V3, XEVK_INA236_SHUNT_3V3_OHMS, XEVK_INA236_MAX_3V3_A },
	{ "1V8", XEVK_I2C_ADDR_INA236_1V8, XEVK_INA236_SHUNT_1V8_OHMS, XEVK_INA236_MAX_1V8_A },
	{ "VCAM2", XEVK_I2C_ADDR_INA236_VCAM2, XEVK_INA236_SHUNT_VCAM2_OHMS, XEVK_INA236_MAX_VCAM2_A },
	{ "VCAM3", XEVK_I2C_ADDR_INA236_VCAM3, XEVK_INA236_SHUNT_VCAM3_OHMS, XEVK_INA236_MAX_VCAM3_A },
	{ "5V", XEVK_I2C_ADDR_INA236_5V, XEVK_INA236_SHUNT_5V_OHMS, XEVK_INA236_MAX_5V_A },
};

#define N_RAILS (sizeof(k_rails) / sizeof(k_rails[0]))

int main(void)
{
	/*
	 * One bus handle shared by all five monitors.  400 kHz
	 * fast-mode is comfortable for the INA236 (it tolerates up to
	 * ~2.94 MHz).
	 */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = XEVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 400000,
	});
	if (bus == NULL) {
		fprintf(stderr, "alp_i2c_open(sensor bus) failed\n");
		return 1;
	}

	/*
	 * Calibrate each monitor for its rail.  A failed init (rail
	 * powered down, or the monitor not populated on this board
	 * revision) is non-fatal -- we keep the others and mark the
	 * dead one in the table.
	 */
	ina236_t mon[N_RAILS];
	bool     live[N_RAILS];
	for (size_t i = 0; i < N_RAILS; i++) {
		live[i] = (ina236_init(&mon[i],
		                       bus,
		                       k_rails[i].addr,
		                       k_rails[i].shunt_ohms,
		                       k_rails[i].max_a,
		                       INA236_ADCRANGE_81MV) == ALP_OK);
		if (!live[i]) {
			fprintf(stderr,
			        "INA236 %-5s @0x%02x: init failed (rail off / not populated?)\n",
			        k_rails[i].name,
			        k_rails[i].addr);
		}
	}

	/* Poll + print until interrupted. */
	for (;;) {
		printf("rail     bus_V     I_mA       P_mW\n");
		for (size_t i = 0; i < N_RAILS; i++) {
			ina236_sample_t s;
			if (!live[i] || ina236_read_all(&mon[i], &s) != ALP_OK) {
				printf("  %-5s    --        --         --\n", k_rails[i].name);
				continue;
			}
			printf("  %-5s %7.3f  %9.2f %10.1f\n",
			       k_rails[i].name,
			       s.bus_mv / 1000.0,
			       s.current_ua / 1000.0,
			       s.power_uw / 1000.0);
		}
		printf("\n");
		sleep(1);
	}

	/* Not reached (Ctrl-C exits); shown for lifecycle completeness. */
	alp_i2c_close(bus);
	return 0;
}
