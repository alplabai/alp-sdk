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
 *   - Treating an optional part as optional: the +5V input monitor
 *     (an INA228, driven by `ina228_*`) answers only on carriers that
 *     have the bus-pin rework, so "not present" is a normal result that
 *     prints one line and leaves the rest of the table running.
 *
 * Scope
 * -----
 * The INA236 monitors are an *EVK-only* instrumentation feature --
 * production E1M-X SoMs do not carry them -- so this is a bring-up
 * / demo utility, not a production telemetry path.  Rail map +
 * shunt values come from <alp/boards/alp_e1m_x_evk.h>.
 *
 * The monitors sit on the on-board sensor I2C bus
 * (XEVK_I2C_BUS_SENSORS = ALP_E1M_X_I2C0, i.e. Linux /dev/i2c-0).
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
#include "alp/chips/ina228.h"
#include "alp/chips/ina236.h"
#include "alp/boards/alp_e1m_x_evk.h"        /* INA236 addresses + shunt calibration */
#include "alp/boards/alp_e1m_x_evk_routes.h" /* XEVK_I2C_BUS_SENSORS                 */

/*
 * The three INA236 rails.  Address + shunt + max-current come straight
 * from the board header so the calibration constants live in one
 * place (the board definition), not scattered through app code.  The
 * +5V input monitor (U30) is an INA228, a different register map, so
 * it is not in this table.
 */
static const struct rail_def {
	const char *name;
	uint8_t     addr;
	float       shunt_ohms;
	float       max_a;
} k_rails[] = {
	{ "3V3", XEVK_I2C_ADDR_INA236_3V3, XEVK_INA236_SHUNT_3V3_OHMS, XEVK_INA236_MAX_3V3_A },
	{ "1V8", XEVK_I2C_ADDR_INA236_1V8, XEVK_INA236_SHUNT_1V8_OHMS, XEVK_INA236_MAX_1V8_A },
	{ "VCAM3", XEVK_I2C_ADDR_INA236_VCAM3, XEVK_INA236_SHUNT_VCAM3_OHMS, XEVK_INA236_MAX_VCAM3_A },
};

#define N_RAILS (sizeof(k_rails) / sizeof(k_rails[0]))

/*
 * The +5V input monitor is an INA228 (20-bit registers, so a different
 * driver).  The board header gives the shunt in ohms and a max current in
 * amps; ina228_init() takes micro-ohms and micro-amps, so convert (+0.5f
 * rounds the float product instead of truncating it).  The max current is
 * the ADCRANGE = 0 shunt full scale (163.84 mV / 100 mOhm = 1.6384 A),
 * derived from the shunt, NOT a limit of the rail.
 */
#define RAIL5V_SHUNT_UOHM ((uint32_t)(XEVK_INA228_SHUNT_5V_OHMS * 1000000.0f + 0.5f))
#define RAIL5V_MAX_UA     ((uint32_t)(XEVK_INA228_MAX_5V_A * 1000000.0f + 0.5f))

int main(void)
{
	/*
	 * One bus handle shared by all four monitors.  400 kHz
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

	/*
	 * The INA228.  INA228_ERR_NOT_PRESENT means nothing acknowledged at its
	 * address -- the normal result on a carrier without the bus-pin rework --
	 * so say so once and carry on (the poll loop below still runs).  Any other
	 * failure -- BUSY, TIMEOUT, ... -- is a real bus fault and is printed with
	 * its actual status, never as "not present".
	 */
	ina228_t     mon5v;
	alp_status_t s5    = ina228_init(&mon5v,
	                                 bus,
	                                 XEVK_I2C_ADDR_INA228_5V,
	                                 RAIL5V_SHUNT_UOHM,
	                                 RAIL5V_MAX_UA,
	                                 INA228_ADCRANGE_163MV);
	bool         live5 = (s5 == ALP_OK);
	if (s5 == INA228_ERR_NOT_PRESENT) {
		fprintf(stderr,
		        "INA228 5V    @0x%02x: not present (carrier without the bus-pin rework?); "
		        "continuing without it\n",
		        XEVK_I2C_ADDR_INA228_5V);
	} else if (!live5) {
		fprintf(stderr,
		        "INA228 5V    @0x%02x: init failed: %s (%d)\n",
		        XEVK_I2C_ADDR_INA228_5V,
		        alp_status_name(s5),
		        (int)s5);
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
		int32_t  bus_uv = 0, cur_ua = 0;
		uint64_t pwr_uw = 0;
		if (!live5 || ina228_read_bus_uv(&mon5v, &bus_uv) != ALP_OK ||
		    ina228_read_current_ua(&mon5v, &cur_ua) != ALP_OK ||
		    ina228_read_power_uw(&mon5v, &pwr_uw) != ALP_OK) {
			printf("  %-5s    --        --         --\n", "5V");
		} else {
			printf("  %-5s %7.3f  %9.2f %10.1f\n",
			       "5V",
			       bus_uv / 1000000.0,
			       cur_ua / 1000.0,
			       (double)pwr_uw / 1000.0);
		}
		printf("\n");
		sleep(1);
	}

	/* Not reached (Ctrl-C exits); shown for lifecycle completeness. */
	alp_i2c_close(bus);
	return 0;
}
