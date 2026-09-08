/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-evk-demo -- phased full-board demo for the E1M-AEN801 (Alif Ensemble
 * E8, M55-HE) on the E1M EVK carrier, bench RAM-run via J-Link
 * (e1m-aen-evk-03, E1M-AEN803 serial 2026W36-0002, EVK rev 2626-R2).
 *
 * WHY A PHASE FRAMEWORK, NOT A FLAT LIST OF CHECKS
 * -------------------------------------------------
 * i2c-device-hub used to print `RESULT PASS: 13/13` while one of its
 * sensors was still returning its power-on-reset sentinel -- the tally
 * counted a successful ID *read* as a pass, not a successful *sample*.
 * That bug is the reason this app is shaped the way it is: every phase
 * below returns exactly one of three verdicts, and "the hardware wasn't
 * there" is a DIFFERENT verdict from "the hardware answered garbage":
 *
 *   PASS    -- the phase exercised its hardware and the data was valid.
 *   SKIPPED -- the hardware it needs is absent, or no operator is present
 *              to do the physical part (turn a knob, listen for a tone).
 *              A phase that cannot exercise its hardware reports SKIPPED,
 *              NEVER PASS -- inventing a pass here would be the exact
 *              class of lie i2c-device-hub's bug already cost a bench run.
 *   FAIL    -- the hardware IS present and it misbehaved.
 *
 * A run full of SKIPPED phases (no SD card in the slot, nobody at the
 * bench to turn the encoder) is not a failed run -- the summary table and
 * the final RESULT line both report all three counts so that distinction
 * stays legible at a glance instead of collapsing into one pass/fail bit.
 *
 * SCOPE OF THIS SLICE
 * --------------------
 * Fourteen phases are registered below, run in a fixed order. Seven have
 * real, bench-proven drivers behind them and are fully implemented: the
 * first six, plus phase 13 (JPEG encode on the Hantro VC9000E). The
 * remaining seven (encoder, CC3501E, SD card, Ethernet, sound, screen,
 * NPU) are stubs that always report SKIPPED with a phase-specific reason
 * -- see the "STUBS" section below for why each one differs (an
 * attended-run requirement is not the same kind of gap as hardware
 * genuinely out of scope, a larger unit of work deferred to the next
 * slice, or -- for NPU -- an image that would have to change BOOT FLOW to
 * hold the model). Attempting all eleven-plus phases of the full design in
 * one drop would have meant shipping several of them unverified against
 * real silicon; a working core that others can extend safely is worth more
 * than an unverifiable giant one.
 *
 * Neither implemented phase 13 nor the NPU stub is camera-gated: the JPEG
 * phase encodes a synthetic gradient it builds itself and the NPU model
 * carries its own input, so no camera module is required by, or in scope
 * for, either one.
 *
 * BUSES
 * -----
 * Two physically separate I2C buses are opened once in main() and shared
 * across phases via `demo_ctx_t`:
 *
 *   brd_bus     -- SoC I2C0, portable bus index 2 (alias alp-i2c2). The
 *                  on-module housekeeping bus: RV-3028-C7 RTC @0x52,
 *                  TMP112 temperature sensor @0x40. Already enabled by the
 *                  board layer (metadata/e1m_modules/aen/
 *                  on-module-links.yaml `brd_i2c` entry) -- this app's own
 *                  overlay adds nothing for it.
 *   carrier_bus -- SoC I2C2, portable bus index 0 / ALP_E1M_I2C0 (alias
 *                  alp-i2c0, pads P5_6 SCL / P5_7 SDA). The E1M EVK
 *                  carrier's sensor/power/expander/EEPROM bus: BMI323
 *                  @0x68, ICM-42670 @0x69, BMP581 @0x47, six INA236, the
 *                  TCAL9538 I/O expander @0x73, and the SoM's own 24C128
 *                  manifest EEPROM @0x50 (bridge/DNP-selected onto this
 *                  SAME physical bus). Also already board-layer-enabled.
 *
 * Confusing these two buses wastes a bench run (EVK-BRIEFING.md) -- every
 * phase below states which one it opens and why.
 *
 * CONSOLE
 * -------
 * This bench's app UART emits nothing under the Flow C RAM-run this app
 * targets and exports no SE-UART, so prj.conf documents the RAM-console
 * toggle every sibling AEN bench app uses -- see its comment.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h> /* ARRAY_SIZE, BIT */

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/jpeg.h"
#include "alp/hw_info.h" /* alp_hw_info_eeprom_t, ALP_HW_INFO_MAGIC -- manifest layout only */
#include "alp/boards/alp_e1m_evk.h"

#include "alp/chips/rv3028c7.h"
#include "alp/chips/tmp112.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/ina236.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/eeprom_24c128.h"

/* ==================================================================== */
/* Phase framework                                                       */
/* ==================================================================== */

typedef enum {
	PHASE_PASS = 0,
	PHASE_SKIPPED,
	PHASE_FAIL,
} phase_verdict_t;

static const char *verdict_str(phase_verdict_t v)
{
	switch (v) {
	case PHASE_PASS:
		return "PASS";
	case PHASE_SKIPPED:
		return "SKIPPED";
	case PHASE_FAIL:
	default:
		return "FAIL";
	}
}

/** Shared state every phase function may read. Opened once in main() so a
 *  phase that only needs the carrier bus doesn't pay to open it again, and
 *  so a bus-open failure is reported once instead of once per phase. */
typedef struct {
	alp_i2c_t *brd_bus; /**< SoC I2C0 / BRD_I2C, portable bus 2. NULL if open failed. */
	alp_i2c_t
	    *carrier_bus; /**< SoC I2C2 / EVK_I2C_BUS_SENSORS, portable bus 0. NULL if open failed. */
} demo_ctx_t;

typedef phase_verdict_t (*phase_fn_t)(demo_ctx_t *ctx);

typedef struct {
	const char *name;
	phase_fn_t  run;
} phase_t;

/* ==================================================================== */
/* Phase 1 -- RTC + temperature (BRD_I2C)                                */
/* ==================================================================== */

/*
 * RV-3028-C7 @0x52: init (clears PORF + forces 24h mode -- both plain,
 * non-EEPROM register writes), was_cold_start (reports what PORF was
 * BEFORE init cleared it), get_time twice 1 s apart to prove seconds
 * actually advance, not just that a read succeeds.
 *
 * HARD CONSTRAINT this phase must never violate: it never calls
 * rv3028c7_set_int_enable() or rv3028c7_route_clkout() -- both are
 * EEPROM-backed writes, and the part's datasheet-documented endurance is
 * only 100 cycles minimum at the hot corner (Application Manual Rev. 1.4
 * p.98). Every register this phase touches is either read-only from this
 * app's perspective, or one of the two plain registers rv3028c7_init()
 * itself already writes internally.
 *
 * TMP112 @0x40 (TMP112_I2C_ADDR_ADDRVAR_GND -- the X2SON-5 address-variant
 * strap this EVK's U20 uses, SBOS473L Table 7-4 p.16; NOT the 0x48..0x4B
 * alert-variant range): one reading, checked against the same plausible
 * indoor band examples/aen/aen-temp-sensor uses. That band is a
 * plausibility heuristic, not an accuracy claim -- see the macro comment.
 */
#define TEMP_PLAUSIBLE_LO_MILLI_C 15000
#define TEMP_PLAUSIBLE_HI_MILLI_C 35000

static phase_verdict_t phase_rtc_temp(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: RTC + temperature (BRD_I2C) --\n");
	if (ctx->brd_bus == NULL) {
		printf("[evkdemo] RTC+TEMP: BRD_I2C bus not open\n");
		return PHASE_FAIL;
	}

	bool rtc_ok = false;
	{
		rv3028c7_t   rtc;
		alp_status_t rc = rv3028c7_init(&rtc, ctx->brd_bus);
		if (rc != ALP_OK) {
			printf("[evkdemo] RTC: rv3028c7_init -> %d\n", (int)rc);
		} else {
			bool cold = false;
			(void)rv3028c7_was_cold_start(&rtc, &cold);

			rv3028c7_time_t t0 = { 0 }, t1 = { 0 };
			alp_status_t    rc0 = rv3028c7_get_time(&rtc, &t0);
			/* A full second, plus margin, so a same-second race (read
			 * lands right before a rollover) can't read as "not
			 * advancing" -- 1100 ms guarantees at least one seconds
			 * tick has elapsed by the second read. */
			k_msleep(1100);
			alp_status_t rc1 = rv3028c7_get_time(&rtc, &t1);

			/* Handle the 59 -> 00 rollover: "advancing" means the
			 * absolute second-of-minute changed, in either direction
			 * a wrap can present it. */
			bool advanced = (rc0 == ALP_OK) && (rc1 == ALP_OK) && (t0.second != t1.second);

			printf("[evkdemo] RTC: init ok, cold_start=%s, t0=%02u:%02u:%02u "
			       "t1=%02u:%02u:%02u advanced=%s\n",
			       cold ? "true" : "false",
			       t0.hour,
			       t0.minute,
			       t0.second,
			       t1.hour,
			       t1.minute,
			       t1.second,
			       advanced ? "yes" : "no");

			rtc_ok = (rc0 == ALP_OK) && (rc1 == ALP_OK) && advanced;
			rv3028c7_deinit(&rtc);
		}
	}

	bool tmp_ok = false;
	{
		tmp112_t     tmp;
		alp_status_t rc = tmp112_init(&tmp, ctx->brd_bus, TMP112_I2C_ADDR_ADDRVAR_GND);
		if (rc != ALP_OK) {
			printf("[evkdemo] TMP112: tmp112_init -> %d\n", (int)rc);
		} else {
			int32_t      milli_c   = 0;
			alp_status_t rs        = tmp112_read_temp_milli_c(&tmp, &milli_c);
			bool         plausible = (rs == ALP_OK) && (milli_c >= TEMP_PLAUSIBLE_LO_MILLI_C) &&
			                         (milli_c <= TEMP_PLAUSIBLE_HI_MILLI_C);
			printf("[evkdemo] TMP112: %d milli-degC, rs=%d %s\n",
			       milli_c,
			       (int)rs,
			       plausible ? "(plausible indoor reading)"
			                 : "(outside plausible band -- plausibility check only)");
			tmp_ok = plausible;
			tmp112_deinit(&tmp);
		}
	}

	return (rtc_ok && tmp_ok) ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 2 -- Sensors: BMI323 + ICM-42670 + BMP581 (carrier bus)         */
/* ==================================================================== */

/*
 * Copies the pattern examples/peripheral-io/i2c-device-hub now uses (fixed
 * post-#1978): a documented startup-time FLOOR, then a BOUNDED POLL of the
 * chip's own data-ready flag, then a read -- never a single guessed sleep.
 * A bench read-back on real E1M-AEN803 silicon found the BMI323's config
 * write ACKs at ~17 ms while the chip doesn't actually produce a sample
 * until ~100 ms for a 10 ms configured ODR period; a fixed sleep shorter
 * than that reads the reset sentinel and reports a live chip as dead. The
 * chip's own invalid/reset sentinel (0x8000 on every accel axis, BMP581's
 * 0x7f7f7f raw pair) is kept as a backstop even after drdy asserts, in
 * case the flag itself lied.
 */
static bool imu_axes_invalid(int16_t x, int16_t y, int16_t z)
{
	return x == INT16_MIN && y == INT16_MIN && z == INT16_MIN;
}

static bool bmp581_raw_invalid(const bmp581_raw_t *raw)
{
	return raw->pressure_raw == 0x7F7F7F && raw->temperature_raw == 0x7F7F7F;
}

static phase_verdict_t phase_sensors(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: sensors (BMI323 + ICM-42670 + BMP581) --\n");
	if (ctx->carrier_bus == NULL) {
		printf("[evkdemo] SENSORS: carrier bus not open\n");
		return PHASE_FAIL;
	}

	int attempted = 0, answered = 0;

	/* --- BMI323 @0x68 ------------------------------------------------ */
	attempted++;
	{
		bmi323_t     bmi;
		alp_status_t irc = bmi323_init(&bmi, ctx->carrier_bus, EVK_I2C_ADDR_BMI323);
		if (irc != ALP_OK) {
			printf("[evkdemo] BMI323 @0x%02x: init -> %d\n", EVK_I2C_ADDR_BMI323, (int)irc);
		} else {
			uint8_t id = 0;
			(void)bmi323_read_id(&bmi, &id);
			alp_status_t cfg_rc = bmi323_set_accel(&bmi, BMI323_ODR_100_HZ, BMI323_ACCEL_FS_2G);

			/* tA,SU = 2 ms typ FLOOR (BST-BMI323-DS000-13 Rev 1.7, Table 2
			 * p.9), then poll STATUS.drdy_acc -- bench-measured still 0 at
			 * ~17 ms, set by ~100 ms against a 10 ms configured ODR
			 * period. 30x the ODR period (300 ms) is generous without
			 * being unbounded; poll every half period (5 ms). */
			k_msleep(2);
			const uint32_t      poll_step_ms = 5, timeout_ms = 300;
			bmi323_data_ready_t drdy  = { 0 };
			bool                ready = false;
			for (uint32_t w = 0; w < timeout_ms; w += poll_step_ms) {
				if (bmi323_data_ready(&bmi, &drdy) == ALP_OK && drdy.accel) {
					ready = true;
					break;
				}
				k_msleep(poll_step_ms);
			}

			bmi323_axes_t a  = { 0 };
			alp_status_t  rs = bmi323_read_accel(&bmi, &a);
			bool          valid =
			    (cfg_rc == ALP_OK) && (rs == ALP_OK) && ready && !imu_axes_invalid(a.x, a.y, a.z);
			printf("[evkdemo] BMI323 @0x%02x: id=0x%02x accel{%d,%d,%d} cfg_rc=%d rs=%d "
			       "ready=%s %s\n",
			       EVK_I2C_ADDR_BMI323,
			       id,
			       a.x,
			       a.y,
			       a.z,
			       (int)cfg_rc,
			       (int)rs,
			       ready ? "yes" : "TIMEOUT",
			       valid ? "ok" : "INVALID");
			if (valid) answered++;
		}
	}

	/* --- ICM-42670 @0x69 ---------------------------------------------- */
	attempted++;
	{
		icm42670_t   imu;
		alp_status_t irc = icm42670_init(&imu, ctx->carrier_bus, EVK_I2C_ADDR_ICM42670);
		if (irc != ALP_OK) {
			printf("[evkdemo] ICM42670 @0x%02x: init -> %d\n", EVK_I2C_ADDR_ICM42670, (int)irc);
		} else {
			uint8_t id = 0;
			(void)icm42670_read_id(&imu, &id);
			alp_status_t cfg_rc =
			    icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_2G);

			/* 10 ms typ sleep-to-valid-sample FLOOR (TDK DS-000451 Rev
			 * 1.0 p.11), then poll DATA_RDY_INT (p.67) the same
			 * 5 ms / 300 ms shape as BMI323 above. */
			k_msleep(10);
			const uint32_t poll_step_ms = 5, timeout_ms = 300;
			bool           ready = false;
			for (uint32_t w = 0; w < timeout_ms; w += poll_step_ms) {
				if (icm42670_data_ready(&imu, &ready) == ALP_OK && ready) break;
				ready = false;
				k_msleep(poll_step_ms);
			}

			icm42670_axes_t a  = { 0 };
			alp_status_t    rs = icm42670_read_accel(&imu, &a);
			bool            valid =
			    (cfg_rc == ALP_OK) && (rs == ALP_OK) && ready && !imu_axes_invalid(a.x, a.y, a.z);
			printf("[evkdemo] ICM42670 @0x%02x: id=0x%02x accel{%d,%d,%d} cfg_rc=%d rs=%d "
			       "ready=%s %s\n",
			       EVK_I2C_ADDR_ICM42670,
			       id,
			       a.x,
			       a.y,
			       a.z,
			       (int)cfg_rc,
			       (int)rs,
			       ready ? "yes" : "TIMEOUT",
			       valid ? "ok" : "INVALID");
			if (valid) answered++;
		}
	}

	/* --- BMP581 @0x47 --------------------------------------------------- */
	attempted++;
	{
		bmp581_t     baro;
		alp_status_t irc = bmp581_init(&baro, ctx->carrier_bus, EVK_I2C_ADDR_BMP581);
		if (irc != ALP_OK) {
			printf("[evkdemo] BMP581 @0x%02x: init -> %d\n", EVK_I2C_ADDR_BMP581, (int)irc);
		} else {
			uint8_t id = 0;
			(void)bmp581_read_id(&baro, &id);
			/* FORCED: one conversion then back to standby, fitting a
			 * single-read phase; init() alone leaves the part in
			 * STANDBY with pressure OFF (BST-BMP581-DS004-13 Rev 1.13
			 * pp.50,58). */
			alp_status_t cfg_rc = bmp581_set_sampling(
			    &baro, BMP581_OSR_X1, BMP581_OSR_X1, BMP581_ODR_50_HZ, BMP581_MODE_FORCED);

			/* ~2 ms typ P+T conversion at OSR x1 (p.12); poll
			 * drdy_data_reg (p.58, clear-on-read) up to 10x that. */
			const uint32_t poll_step_ms = 2, timeout_ms = 20;
			bool           ready = false;
			for (uint32_t w = 0; w < timeout_ms; w += poll_step_ms) {
				if (bmp581_data_ready(&baro, &ready) == ALP_OK && ready) break;
				ready = false;
				k_msleep(poll_step_ms);
			}

			bmp581_raw_t raw = { 0 };
			alp_status_t rs  = bmp581_read_raw(&baro, &raw);
			bool valid = (cfg_rc == ALP_OK) && (rs == ALP_OK) && ready && !bmp581_raw_invalid(&raw);
			printf("[evkdemo] BMP581 @0x%02x: id=0x%02x p_raw=%d t_raw=%d cfg_rc=%d rs=%d "
			       "ready=%s %s\n",
			       EVK_I2C_ADDR_BMP581,
			       id,
			       raw.pressure_raw,
			       raw.temperature_raw,
			       (int)cfg_rc,
			       (int)rs,
			       ready ? "yes" : "TIMEOUT",
			       valid ? "ok" : "INVALID");
			if (valid) answered++;
		}
	}

	printf("[evkdemo] SENSORS: %d/%d answered\n", answered, attempted);
	return (answered == attempted) ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 3 -- Power rails: six INA236 (carrier bus)                      */
/* ==================================================================== */

/*
 * Bus voltage, SHUNT MICROVOLTS, and current per rail. CURRENT (and the
 * shunt reading it derives from) is only meaningful once the chip's own
 * CVRF conversion-ready flag has been observed set -- ina236_init()'s
 * final CALIBRATION write clears CVRF, and a full bus+shunt cycle at the
 * reset default conversion times is ~2.2 ms (TI SBOSA81D pp.19-24). This
 * phase POLLS ina236_conversion_ready() rather than trusting a fixed
 * sleep, with a generous bound.
 *
 * Two things this phase must NOT do:
 *   - Fail on +VCAM0 / +VCAM1 reading 0 mV. Both rails are genuinely
 *     unpowered with no camera fitted on this bench -- that is correct
 *     reporting, not a fault (EVK-BRIEFING.md).
 *   - Fail on +1V8 (U31, EVK_I2C_ADDR_INA236_1V8) reading exactly 0 uV
 *     shunt while the rail is live. That is a real, currently-open
 *     question about this specific rail's reading, not something this
 *     demo can resolve -- it is reported, not gated on.
 * There is no documented invalid/reset encoding for INA236's voltage or
 * current registers (an idle rail legitimately reads 0), so -- as with
 * i2c-device-hub -- the return codes plus the conversion-ready predicate
 * are the whole check here, not a value-range heuristic.
 */
static const struct {
	const char *name;
	uint8_t     addr;
	float       shunt_ohms;
	float       max_a;
} INA_RAILS[] = {
	{ "+3V3", EVK_I2C_ADDR_INA236_3V3, EVK_INA236_SHUNT_3V3_OHMS, EVK_INA236_MAX_3V3_A },
	{ "+1V8", EVK_I2C_ADDR_INA236_1V8, EVK_INA236_SHUNT_1V8_OHMS, EVK_INA236_MAX_1V8_A },
	{ "+VIO", EVK_I2C_ADDR_INA236_VIO, EVK_INA236_SHUNT_VIO_OHMS, EVK_INA236_MAX_VIO_A },
	{ "+VCAM0", EVK_I2C_ADDR_INA236_VCAM0, EVK_INA236_SHUNT_VCAM0_OHMS, EVK_INA236_MAX_VCAM0_A },
	{ "+VCAM1", EVK_I2C_ADDR_INA236_VCAM1, EVK_INA236_SHUNT_VCAM1_OHMS, EVK_INA236_MAX_VCAM1_A },
	{ "+5V", EVK_I2C_ADDR_INA236_5V, EVK_INA236_SHUNT_5V_OHMS, EVK_INA236_MAX_5V_A },
};

static phase_verdict_t phase_power_rails(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: power rails (6x INA236) --\n");
	if (ctx->carrier_bus == NULL) {
		printf("[evkdemo] POWER: carrier bus not open\n");
		return PHASE_FAIL;
	}

	int ok_count = 0;
	for (size_t i = 0; i < ARRAY_SIZE(INA_RAILS); i++) {
		ina236_t     mon;
		alp_status_t rc = ina236_init(&mon,
		                              ctx->carrier_bus,
		                              INA_RAILS[i].addr,
		                              INA_RAILS[i].shunt_ohms,
		                              INA_RAILS[i].max_a,
		                              INA236_ADCRANGE_81MV);
		if (rc != ALP_OK) {
			printf("[evkdemo] INA236 %-6s @0x%02x: init -> %d (all six are hard-populated on "
			       "this EVK -- a miss is a real fault)\n",
			       INA_RAILS[i].name,
			       INA_RAILS[i].addr,
			       (int)rc);
			continue;
		}

		/* Poll CVRF instead of a fixed sleep. NOTE the clear-on-read
		 * hazard documented on ina236_conversion_ready(): only this one
		 * caller polls it per rail, so that hazard doesn't apply here. */
		const uint32_t poll_step_ms = 1, timeout_ms = 10;
		bool           ready = false;
		for (uint32_t w = 0; w < timeout_ms; w += poll_step_ms) {
			if (ina236_conversion_ready(&mon, &ready) == ALP_OK && ready) break;
			ready = false;
			k_msleep(poll_step_ms);
		}

		int32_t      mv = 0, uv = 0, ua = 0;
		alp_status_t mv_rc = ina236_read_bus_mv(&mon, &mv);
		alp_status_t uv_rc = ina236_read_shunt_uv(&mon, &uv);
		alp_status_t ua_rc = ina236_read_current_ua(&mon, &ua);
		bool         valid = ready && (mv_rc == ALP_OK) && (uv_rc == ALP_OK) && (ua_rc == ALP_OK);

		printf("[evkdemo] INA236 %-6s @0x%02x: %ld mV  %ld uV(shunt)  %ld uA  ready=%s "
		       "mv_rc=%d uv_rc=%d ua_rc=%d %s\n",
		       INA_RAILS[i].name,
		       INA_RAILS[i].addr,
		       (long)mv,
		       (long)uv,
		       (long)ua,
		       ready ? "yes" : "TIMEOUT",
		       (int)mv_rc,
		       (int)uv_rc,
		       (int)ua_rc,
		       valid ? "ok" : "READ FAIL");
		if (valid) ok_count++;
	}

	printf("[evkdemo] POWER: %d/%zu rails answered\n", ok_count, ARRAY_SIZE(INA_RAILS));
	return (ok_count == (int)ARRAY_SIZE(INA_RAILS)) ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 4 -- I/O expander: TCAL9538 @0x73 (carrier bus)                 */
/* ==================================================================== */

/*
 * Reads the configuration register (0x03) and the input port (0x00). Also
 * reads the interrupt-status register (0x46) once, READ-ONLY: nothing in
 * this phase unmasks any pin in 0x45 (all pins mask their own contribution
 * at power-up, SCPS280B p.26), so every bit of 0x46 is guaranteed 0
 * regardless of the physical pin state -- there is nothing pending to
 * acknowledge. This is deliberate: routing a real interrupt through this
 * expander belongs to a consumer that owns the source (see
 * examples/aen/aen-sensor-int-probe), not to a phase that's only proving
 * the Agile-IO block itself answers. The fact worth remembering if a
 * future phase DOES unmask 0x45: register 0x46 is NOT clear-on-read --
 * only a read of the input port (0x00) clears a latched condition
 * (SCPS280B p.26, Table 7-13).
 */
static phase_verdict_t phase_io_expander(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: I/O expander (TCAL9538 @0x%02x) --\n", EVK_I2C_ADDR_TCAL9538_MAIN);
	if (ctx->carrier_bus == NULL) {
		printf("[evkdemo] IOEXP: carrier bus not open\n");
		return PHASE_FAIL;
	}

	tcal9538_t   io;
	alp_status_t rc = tcal9538_init(&io, ctx->carrier_bus, EVK_I2C_ADDR_TCAL9538_MAIN);
	if (rc != ALP_OK) {
		printf("[evkdemo] IOEXP @0x%02x: init -> %d (populated on every 2626-R2 board -- a miss "
		       "is a real fault, not an absent-part skip)\n",
		       EVK_I2C_ADDR_TCAL9538_MAIN,
		       (int)rc);
		return PHASE_FAIL;
	}

	/* Both reads must ACK AND land a byte other than the 0xee sentinel
	 * these locals start at -- an ALP_OK transfer that never actually
	 * wrote the output byte is exactly the class of silent success
	 * i2c-device-hub's bug taught us to distrust. */
	uint8_t      cfg = 0xee, in0 = 0xee;
	uint8_t      reg_cfg = 0x03, reg_in0 = 0x00;
	alp_status_t cfg_rc =
	    alp_i2c_write_read(ctx->carrier_bus, EVK_I2C_ADDR_TCAL9538_MAIN, &reg_cfg, 1, &cfg, 1);
	alp_status_t in0_rc =
	    alp_i2c_write_read(ctx->carrier_bus, EVK_I2C_ADDR_TCAL9538_MAIN, &reg_in0, 1, &in0, 1);

	uint8_t      irq_status = 0xee;
	alp_status_t irq_rc     = tcal9538_get_interrupt_status(&io, &irq_status);

	bool valid = (cfg_rc == ALP_OK) && (in0_rc == ALP_OK) && (cfg != 0xee) && (in0 != 0xee);
	printf("[evkdemo] IOEXP @0x%02x: config(0x03)=0x%02x input(0x00)=0x%02x irqstatus(0x46)=0x%02x "
	       "cfg_rc=%d in0_rc=%d irq_rc=%d %s\n",
	       EVK_I2C_ADDR_TCAL9538_MAIN,
	       cfg,
	       in0,
	       irq_status,
	       (int)cfg_rc,
	       (int)in0_rc,
	       (int)irq_rc,
	       valid ? "ok" : "READ FAIL");

	tcal9538_deinit(&io);
	return valid ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 5 -- EEPROM identity: 24C128 @0x50, READ-ONLY (carrier bus)     */
/* ==================================================================== */

/*
 * Reads the first 128 bytes and checks the two facts this phase's brief
 * asks for: the "ALPH" magic at offset 0 and the family string "aen" --
 * both fields of the fixed manifest layout <alp/hw_info.h> declares
 * (alp_hw_info_eeprom_t), the same layout examples/aen/aen-eeprom-manifest
 * decodes in full. This phase deliberately calls eeprom_24c128_read() only
 * -- no write path is exercised, matching the task's read-only scope.
 */
static phase_verdict_t phase_eeprom_identity(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: EEPROM identity (24C128 @0x%02x, read-only) --\n",
	       EEPROM_24C128_I2C_ADDR_LOW);
	if (ctx->carrier_bus == NULL) {
		printf("[evkdemo] EEPROM: carrier bus not open\n");
		return PHASE_FAIL;
	}

	eeprom_24c128_t ee;
	alp_status_t    rc = eeprom_24c128_init(&ee, ctx->carrier_bus, EEPROM_24C128_I2C_ADDR_LOW);
	if (rc != ALP_OK) {
		printf("[evkdemo] EEPROM @0x%02x: init -> %d\n", EEPROM_24C128_I2C_ADDR_LOW, (int)rc);
		return PHASE_FAIL;
	}

	uint8_t raw[128];
	rc = eeprom_24c128_read(&ee, /* offset */ 0x0000u, raw, sizeof(raw));
	eeprom_24c128_deinit(&ee);
	if (rc != ALP_OK) {
		printf("[evkdemo] EEPROM @0x%02x: read -> %d\n", EEPROM_24C128_I2C_ADDR_LOW, (int)rc);
		return PHASE_FAIL;
	}

	/* Reinterpret rather than field-copy: alp_hw_info_eeprom_t is the
	 * fixed, packed on-wire layout scripts/program_eeprom.py writes. */
	const alp_hw_info_eeprom_t *m         = (const alp_hw_info_eeprom_t *)raw;
	bool                        magic_ok  = (m->magic == ALP_HW_INFO_MAGIC);
	bool                        family_ok = (strncmp(m->family, "aen", 3) == 0);

	printf("[evkdemo] EEPROM: magic=0x%08x (%s) family=\"%.*s\" (%s) sku=\"%.*s\" hw_rev=\"%.*s\" "
	       "serial=\"%.*s\"\n",
	       m->magic,
	       magic_ok ? "OK -- ASCII 'ALPH'" : "FAIL -- not programmed?",
	       ALP_HW_INFO_FAMILY_LEN,
	       m->family,
	       family_ok ? "OK" : "FAIL -- expected \"aen\"",
	       ALP_HW_INFO_SKU_LEN,
	       m->sku,
	       ALP_HW_INFO_HW_REV_LEN,
	       m->hw_rev,
	       ALP_HW_INFO_SERIAL_LEN,
	       m->serial);

	return (magic_ok && family_ok) ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 6 -- RGB LED: PWM0 (red) / PWM3 (green) / PWM1 (blue)           */
/* ==================================================================== */

/*
 * The netlist net labels for this LED are WRONG; the mapping below is the
 * BENCH-MEASURED one (metadata/boards/e1m-evk.yaml's `pwm:` block, already
 * folded into <alp/boards/alp_e1m_evk_routes.h>'s EVK_PWM_LED_* macros --
 * this phase consumes those macros rather than re-deriving the mapping).
 *
 * Driven through the portable <alp/pwm.h> surface only (alp_pwm_open /
 * alp_pwm_set_duty / alp_pwm_close) -- no chip driver or vendor header.
 * With no scope on this bench and no operator present, "the LED lit" is
 * not a claim software can make; this phase instead ASSERTS THE REGISTER
 * the driver programmed, the same register-readback approach
 * examples/aen/aen-pwm-utimer-pwmleds uses for the same INTERIM /
 * BENCH-UNVERIFIED pwm_alif_utimer.c driver (see that driver's file
 * header). The readback reads the raw UTIMER registers directly via their
 * devicetree-derived addresses -- proving structural correctness (the
 * driver + compare-enable bits are set, the timer's clock gate and RUN bit
 * are set, and a real fractional duty was programmed) rather than an
 * exact cycle count, which would require re-deriving Zephyr's internal
 * ns-to-cycles rounding here.
 *
 * Each channel is restored to 0 % duty and closed before the phase
 * returns, leaving the LED idle.
 */

/* UTIMER register offsets, transcribed from modules/hal/alif
 * drivers/utimer/include/utimer.h -- the same citation
 * examples/aen/aen-pwm-utimer-pwmleds uses for the identical readback. */
#define RGB_OFF_CNTR_PTR         0x0A4U /* reload value == period */
#define RGB_OFF_COMPARE_A        0x0D0U /* driver A compare / duty */
#define RGB_OFF_COMPARE_B        0x0E0U /* driver B compare / duty */
#define RGB_OFF_COMPARE_CTRL_A   0x08CU /* driver A output config + enables */
#define RGB_OFF_COMPARE_CTRL_B   0x090U /* driver B output config + enables */
#define RGB_OFF_GLB_CNTR_RUNNING 0x00CU /* global: bit timer_id set while running */
#define RGB_OFF_GLB_CLOCK_ENABLE 0x020U /* global: bit timer_id == this timer's clock gate */

/* Expected COMPARE_CTRL_{A,B} for NORMAL polarity, identical bit layout on
 * either driver (zephyr/drivers/pwm/pwm_alif_utimer.c pwm_alif_set_cycles):
 * START_VAL_HIGH(0x10) | LOW_AT_COMP_MATCH(0x01) | HIGH_AT_CYCLE_END(0x08)
 * | DRIVER_EN(0x100) | COMPARE_EN(0x800) = 0x919. */
#define RGB_EXP_COMPARE_CTRL 0x919U

#define RGB_PERIOD_NS 1000000U             /* 1 kHz -- comfortably visible if anyone does look. */
#define RGB_PULSE_NS  (RGB_PERIOD_NS / 4U) /* 25 % duty. */

/* How long each colour is held lit. The register read-back below proves the
 * controller and the pad are programmed; it cannot prove the LED lights, and
 * it cannot confirm which colour sits on which channel -- the netlist labels
 * for these pads are known wrong, so the mapping in EVK_PWM_LED_* came from a
 * bench measurement. Only a human eye settles that, and a 1 ms pulse is not
 * something a human eye can see. Hold each colour long enough to name it. */
#define RGB_VISIBLE_HOLD_MS 1500U

#define RGB_RED_NODE   DT_NODELABEL(evk_rgb_red)
#define RGB_BLUE_NODE  DT_NODELABEL(evk_rgb_blue)
#define RGB_GREEN_NODE DT_NODELABEL(evk_rgb_green)

/* RED (driver B) and BLUE (driver A) share the SAME utimer11/pwm11
 * controller -- both consumer nodes' `pwms` phandle resolves to the
 * identical parent, so the register bases are pulled once. */
#define RGB_UT11_NODE        DT_PARENT(DT_PWMS_CTLR_BY_IDX(RGB_RED_NODE, 0))
#define RGB_UT11_TIMER_BASE  ((uint32_t)DT_REG_ADDR_BY_IDX(RGB_UT11_NODE, 0))
#define RGB_UT11_GLOBAL_BASE ((uint32_t)DT_REG_ADDR_BY_IDX(RGB_UT11_NODE, 1))
#define RGB_UT11_TIMER_ID    ((uint32_t)DT_PROP(RGB_UT11_NODE, timer_id))

#define RGB_UT10_NODE        DT_PARENT(DT_PWMS_CTLR_BY_IDX(RGB_GREEN_NODE, 0))
#define RGB_UT10_TIMER_BASE  ((uint32_t)DT_REG_ADDR_BY_IDX(RGB_UT10_NODE, 0))
#define RGB_UT10_GLOBAL_BASE ((uint32_t)DT_REG_ADDR_BY_IDX(RGB_UT10_NODE, 1))
#define RGB_UT10_TIMER_ID    ((uint32_t)DT_PROP(RGB_UT10_NODE, timer_id))

typedef struct {
	const char *name;
	uint32_t    e1m_pwm_id; /**< ALP_E1M_PWM0/1/3 -- the alp_pwm_open() channel_id. */
	uint32_t    timer_base;
	uint32_t    global_base;
	uint32_t    timer_id;
	uint32_t    driver; /**< 0 = driver A (COMPARE_A/COMPARE_CTRL_A), 1 = driver B. */
} rgb_channel_t;

static const rgb_channel_t RGB_CHANNELS[] = {
	{ "RED", EVK_PWM_LED_RED, RGB_UT11_TIMER_BASE, RGB_UT11_GLOBAL_BASE, RGB_UT11_TIMER_ID, 1u },
	{ "BLUE", EVK_PWM_LED_BLUE, RGB_UT11_TIMER_BASE, RGB_UT11_GLOBAL_BASE, RGB_UT11_TIMER_ID, 0u },
	{ "GREEN",
	  EVK_PWM_LED_GREEN,
	  RGB_UT10_TIMER_BASE,
	  RGB_UT10_GLOBAL_BASE,
	  RGB_UT10_TIMER_ID,
	  0u },
};

static inline uint32_t rgb_reg_read(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static phase_verdict_t phase_rgb_led(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx); /* PWM is not on either I2C bus. */
	printf("[evkdemo] -- Phase: RGB LED (PWM0 red / PWM3 green / PWM1 blue) --\n");
	printf("[evkdemo] RGB: watch the LED -- each colour is held lit for %u ms, in the\n"
	       "[evkdemo]      order RED, BLUE, GREEN. If the colour named does not match\n"
	       "[evkdemo]      what lights, the EVK_PWM_LED_* mapping is wrong.\n",
	       RGB_VISIBLE_HOLD_MS);

	int ok_count = 0;
	for (size_t i = 0; i < ARRAY_SIZE(RGB_CHANNELS); i++) {
		const rgb_channel_t *ch = &RGB_CHANNELS[i];

		alp_pwm_t *pwm = alp_pwm_open(&(alp_pwm_config_t){
		    .channel_id = ch->e1m_pwm_id,
		    .period_ns  = RGB_PERIOD_NS,
		    .polarity   = ALP_PWM_POLARITY_NORMAL,
		});
		if (pwm == NULL) {
			printf("[evkdemo] RGB %-5s: alp_pwm_open -> NULL, err=%d\n",
			       ch->name,
			       (int)alp_last_error());
			continue;
		}

		alp_status_t duty_rc = alp_pwm_set_duty(pwm, RGB_PULSE_NS);
		/* Latch time: the driver's compare/period write is buffered until
		 * the next cycle boundary. 1 ms comfortably covers the 1 kHz
		 * period this phase configures. */
		k_busy_wait(1000);

		uint32_t reload  = rgb_reg_read(ch->timer_base, RGB_OFF_CNTR_PTR);
		uint32_t compare = rgb_reg_read(ch->timer_base,
		                                (ch->driver == 0u) ? RGB_OFF_COMPARE_A : RGB_OFF_COMPARE_B);
		uint32_t ctrl    = rgb_reg_read(
		    ch->timer_base, (ch->driver == 0u) ? RGB_OFF_COMPARE_CTRL_A : RGB_OFF_COMPARE_CTRL_B);
		uint32_t clk_en  = rgb_reg_read(ch->global_base, RGB_OFF_GLB_CLOCK_ENABLE);
		uint32_t running = rgb_reg_read(ch->global_base, RGB_OFF_GLB_CNTR_RUNNING);
		bool     clk_bit = (clk_en & BIT(ch->timer_id)) != 0u;
		bool     run_bit = (running & BIT(ch->timer_id)) != 0u;

		/* Structural assertion, not a bit-exact cycle match: the driver
		 * + compare-enable bits are programmed, the timer's clock gate
		 * and RUN bit are set, and the compare value sits strictly
		 * between 0 and the reload -- a real fractional duty, not
		 * full-on or full-off. */
		bool valid = (duty_rc == ALP_OK) && (ctrl == RGB_EXP_COMPARE_CTRL) && clk_bit && run_bit &&
		             (reload > 0u) && (compare > 0u) && (compare < reload);

		printf("[evkdemo] RGB %-5s (E1M_PWM%u, driver %c): duty_rc=%d CNTR_PTR=0x%08x "
		       "COMPARE_%c=0x%08x COMPARE_CTRL_%c=0x%08x (exp 0x%08x) clk_en=%s run=%s %s\n",
		       ch->name,
		       (unsigned)ch->e1m_pwm_id,
		       (ch->driver == 0u) ? 'A' : 'B',
		       (int)duty_rc,
		       reload,
		       (ch->driver == 0u) ? 'A' : 'B',
		       compare,
		       (ch->driver == 0u) ? 'A' : 'B',
		       ctrl,
		       RGB_EXP_COMPARE_CTRL,
		       clk_bit ? "yes" : "no",
		       run_bit ? "yes" : "no",
		       valid ? "ok" : "READ FAIL");

		if (valid) ok_count++;

		/* Hold the colour lit long enough for an operator to name it. This
		 * is the only part of the phase a human can check, and it is the
		 * only evidence that would catch a wrong EVK_PWM_LED_* mapping --
		 * every channel programs identically, so the register read-back
		 * above looks the same whether the mapping is right or wrong. */
		if (valid) {
			printf("[evkdemo] RGB %-5s: lit now for %u ms -- expect %s\n",
			       ch->name,
			       RGB_VISIBLE_HOLD_MS,
			       ch->name);
			k_msleep(RGB_VISIBLE_HOLD_MS);
		}

		/* Restore to idle before this phase returns. */
		(void)alp_pwm_set_duty(pwm, 0u);
		alp_pwm_close(pwm);
	}

	printf("[evkdemo] RGB LED: %d/%zu channels verified\n", ok_count, ARRAY_SIZE(RGB_CHANNELS));
	return (ok_count == (int)ARRAY_SIZE(RGB_CHANNELS)) ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 13 -- JPEG encode: Alif Hantro VC9000E via <alp/jpeg.h>         */
/* ==================================================================== */

/*
 * Grouped here with the other IMPLEMENTED phases even though it runs
 * thirteenth -- the file's organising principle is "real phases first,
 * stubs after", and this one has a real, bench-proven driver behind it.
 *
 * NO CAMERA. This phase encodes a synthetic gradient it builds itself, so
 * nothing about it depends on a camera module being attached (none is on
 * this bench, and one is out of scope for this slice). The gradient is
 * real, varying luma the encoder cannot collapse into a single DCT
 * coefficient -- the identical source examples/aen/aen-jpeg-regcheck
 * encodes to produce its silicon-proven 935-byte result on this SoC.
 *
 * THREE TRAPS this phase deliberately walks around. Each is a defect a
 * real AEN801 bench run exposed, and each is recorded in full in
 * examples/aen/aen-jpeg-regcheck/src/main.c's file header:
 *
 *  1. BACKEND SELECTION. Without CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8 the build
 *     resolves ALP_SOC_REF_STR="unknown", the selector (src/backend.c)
 *     filters out alif_hantro (silicon_ref="alif:ensemble:e8", priority
 *     100), and the portable software fallback (src/backends/jpeg/
 *     sw_baseline.c, "*", priority 50) silently wins. The app then still
 *     prints a perfectly valid JPEG while proving NOTHING about the
 *     silicon -- the exact shape of lie this whole demo exists to avoid.
 *     prj.conf sets the symbol; this phase prints caps.hw_accelerated so
 *     the log itself names which backend ran, and a software win is a
 *     FAIL here: on this board the hardware encoder IS the phase.
 *  2. DMA PLACEMENT. The Hantro block is an AXI bus master -- it fetches
 *     the source and writes the output through its OWN master, not via
 *     the M55, so both buffers must sit at a GLOBAL address. This board's
 *     default RAM (`zephyr,sram = &dtcm`) puts .bss in the M55's private
 *     DTCM at 0x20000000, which that master cannot reach; the backend
 *     detects the TCM window and returns ALP_ERR_NOSUPPORT rather than
 *     encode garbage. Both buffers below are therefore tagged into the
 *     "SRAM0" linker region (global on-chip SRAM0 @0x02000000), the same
 *     fix aen-dma-regcheck needed for the PL330's AXI master.
 *  3. SOURCE LAYOUT. The two backends want genuinely different layouts
 *     (Hantro latches one raw pointer over a semi-planar NV12 buffer; the
 *     software encoder wants three separate Y/U/V planes). aen-jpeg-
 *     regcheck explicitly REMOVED the #ifdef-on-build-target approach
 *     that encodes this at compile time: it needs a new arm for every
 *     future backend, while asking the WON backend at runtime what it
 *     advertises (alp_jpeg_caps_t::pixfmt_mask) never does. This phase
 *     asks, and builds whichever layout comes back.
 *
 * WHAT THE PORTABLE API DOES NOT EXPOSE: the driver's jpeg_hw_init()
 * reads JPEG_SWREG0 and compares it against JPEG_HW_ID (0x90001000) --
 * the one read that proves the block is alive and mapped at 0x49044000
 * independent of any encode. <alp/jpeg.h> has no accessor for it, and
 * this example will NOT hand-roll a register poke to get at it. It is
 * still observable two ways, both of which land in this app's log: an ID
 * mismatch makes jpeg_hw_init() return -ENODEV, which fails the device's
 * init, which makes hantro_open()'s device_is_ready() false, which
 * surfaces here as alp_jpeg_open() == NULL with alp_last_error() ==
 * ALP_ERR_NOT_READY (-14) -- and the driver's own
 * "JPEG hardware not found (ID: 0x%08x)" LOG_ERR carries the actual
 * register value, visible because this app builds with CONFIG_LOG=y.
 */

#define JPEG_FRAME_W 64
#define JPEG_FRAME_H 64
#define JPEG_OUT_CAP 8192u

/* Bytes of NV12 source for one frame: full-resolution Y, then one
 * interleaved U,V pair per 2x2 luma block (half as many bytes again). */
#define JPEG_SRC_LEN ((JPEG_FRAME_W * JPEG_FRAME_H) + (JPEG_FRAME_W * JPEG_FRAME_H) / 2)

/*
 * "Not suspiciously tiny" floor for the encoded length. A baseline JPEG
 * pays for its markers before it encodes a single pixel -- SOI, JFIF
 * APP0, two quantization tables, SOF0, four Huffman tables, SOS -- which
 * is already several hundred bytes, so anything under this floor is a
 * marker skeleton with no image in it, not a small picture. 256 sits
 * comfortably below the 935 bytes aen-jpeg-regcheck measured for this
 * exact 64x64 frame on real AEN801 silicon, and comfortably above an
 * empty stream. That 935 is an ORDER-OF-MAGNITUDE reference, not an
 * expected value: quantization tables are quality- and backend-dependent,
 * so pinning the exact figure would turn a legitimate encoder change into
 * a bench failure.
 */
#define JPEG_MIN_PLAUSIBLE_LEN 256u

/*
 * Hantro AXI INPUT and OUTPUT -- both must be globally addressable (trap 2
 * above). "SRAM0" is the linker region the board's dts maps onto the
 * global on-chip SRAM0 bank at 0x02000000, which every AXI master on this
 * SoC can see. Uninitialised on purpose: an INITIALISED array in a custom
 * section is not init-copied by Zephyr, so these are filled at runtime.
 */
static uint8_t jpeg_src[JPEG_SRC_LEN] __attribute__((section("SRAM0")));
static uint8_t jpeg_out[JPEG_OUT_CAP] __attribute__((section("SRAM0")));

/* NV12: one contiguous buffer, Y plane then the interleaved UV plane at
 * offset W*H -- what alif_hantro.c latches as a single raw pointer. */
static void jpeg_build_nv12(alp_jpeg_encode_req_t *req)
{
	uint8_t *y  = jpeg_src;
	uint8_t *uv = jpeg_src + (JPEG_FRAME_W * JPEG_FRAME_H);

	for (int r = 0; r < JPEG_FRAME_H; ++r) {
		for (int c = 0; c < JPEG_FRAME_W; ++c) {
			y[r * JPEG_FRAME_W + c] = (uint8_t)((r + c) * 2);
		}
	}
	/* Neutral grey chroma: the checks below are all structural (markers,
	 * length), so chroma CONTENT is irrelevant -- but it still has to be
	 * present and correctly sized or the encoder reads past the frame. */
	memset(uv, 128, (JPEG_FRAME_W * JPEG_FRAME_H) / 2);

	req->format   = ALP_PIXFMT_NV12;
	req->y_plane  = y;
	req->y_stride = JPEG_FRAME_W;
	req->u_plane  = NULL; /* NV12: the UV plane lives inside y_plane. */
	req->v_plane  = NULL;
}

static phase_verdict_t phase_jpeg_encode(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx); /* The JPEG block is on neither I2C bus. */
	printf("[evkdemo] -- Phase: JPEG encode (Hantro VC9000E, %dx%d synthetic frame) --\n",
	       JPEG_FRAME_W,
	       JPEG_FRAME_H);

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	if (h == NULL) {
		/* ALP_ERR_NOT_READY here is the closest this app gets to the
		 * JPEG_SWREG0 hardware-ID readback -- see the file-section note
		 * above; the driver's LOG_ERR line carries the actual value. */
		printf("[evkdemo] JPEG: alp_jpeg_open -> NULL, err=%d (NOT_READY here means the driver's "
		       "jpeg_hw_init() rejected JPEG_SWREG0 vs JPEG_HW_ID 0x90001000 -- its LOG_ERR "
		       "line above carries the value it read)\n",
		       (int)alp_last_error());
		return PHASE_FAIL;
	}

	alp_jpeg_caps_t caps;
	alp_status_t    caps_rc = alp_jpeg_capabilities(h, &caps);
	printf("[evkdemo] JPEG: caps rc=%d hw_accelerated=%d mjpeg=%d max=%ux%u subsample_mask=0x%x "
	       "pixfmt_mask=0x%x backend=%s\n",
	       (int)caps_rc,
	       (int)caps.hw_accelerated,
	       (int)caps.mjpeg_supported,
	       caps.max_width,
	       caps.max_height,
	       caps.subsample_mask,
	       caps.pixfmt_mask,
	       caps.hw_accelerated ? "alif_hantro (HW)" : "sw_baseline (SW) -- WRONG ON THIS BOARD");

	alp_jpeg_encode_req_t req = {
		.width     = JPEG_FRAME_W,
		.height    = JPEG_FRAME_H,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
	};

	/* Trap 3: build the layout the WON backend advertises, not the one
	 * this build's CONFIG_* implies. Only NV12 is built here -- that is
	 * what the hardware backend on this board asks for, and a backend
	 * asking for anything else on THIS board is already the failure the
	 * hw_accelerated check below reports, so there is no second layout
	 * worth carrying. */
	if ((caps.pixfmt_mask & (1u << ALP_PIXFMT_NV12)) == 0u) {
		printf("[evkdemo] JPEG: won backend does not accept ALP_PIXFMT_NV12 (pixfmt_mask=0x%x) -- "
		       "the Hantro backend advertises exactly that bit, so this is not it\n",
		       caps.pixfmt_mask);
		alp_jpeg_close(h);
		return PHASE_FAIL;
	}
	jpeg_build_nv12(&req);

	size_t       out_len = 0;
	alp_status_t rc      = alp_jpeg_encode(h, &req, jpeg_out, sizeof(jpeg_out), &out_len);
	alp_jpeg_close(h);

	/*
	 * rc == ALP_OK IS NOT THE VERDICT. i2c-device-hub counted a
	 * successful chip-ID read as a pass while its sensors returned reset
	 * sentinels; "the call returned 0" is the same claim. What is
	 * asserted instead: the bytes that came back really are a JPEG --
	 * they open with the SOI marker (FF D8, always followed by the FF of
	 * the next marker), they close with EOI (FF D9), and the length is
	 * plausible for a real image of this size.
	 */
	bool    len_ok = (out_len >= JPEG_MIN_PLAUSIBLE_LEN) && (out_len < (size_t)JPEG_SRC_LEN) &&
	                 (out_len <= sizeof(jpeg_out));
	uint8_t soi[3] = { 0, 0, 0 };
	uint8_t eoi[2] = { 0, 0 };
	bool    soi_ok = false, eoi_ok = false;
	if (out_len >= 3u) {
		soi[0] = jpeg_out[0];
		soi[1] = jpeg_out[1];
		soi[2] = jpeg_out[2];
		soi_ok = (soi[0] == 0xFFu) && (soi[1] == 0xD8u) && (soi[2] == 0xFFu);
	}
	/* Deliberately NOT gated on len_ok: a stream that misses the plausible-
	 * length band still has real trailing bytes, and printing them is how a
	 * reader tells "the encoder produced a short but well-formed JPEG" from
	 * "the encoder produced garbage". Gating this on len_ok would print
	 * last2=0000 eoi=BAD for both, and the log is the entire diagnostic --
	 * re-running it costs a bench reservation. len_ok is ANDed into the
	 * verdict separately below, so the pass criterion is unchanged. */
	if ((out_len >= 2u) && (out_len <= sizeof(jpeg_out))) {
		eoi[0] = jpeg_out[out_len - 2u];
		eoi[1] = jpeg_out[out_len - 1u];
		eoi_ok = (eoi[0] == 0xFFu) && (eoi[1] == 0xD9u);
	}

	printf("[evkdemo] JPEG: encode rc=%d out_len=%u first3=%02x%02x%02x (expect ffd8ff SOI) "
	       "last2=%02x%02x (expect ffd9 EOI)\n",
	       (int)rc,
	       (unsigned)out_len,
	       soi[0],
	       soi[1],
	       soi[2],
	       eoi[0],
	       eoi[1]);
	printf("[evkdemo] JPEG: soi=%s eoi=%s len=%s (floor %u B, source %u B; aen-jpeg-regcheck "
	       "measured 935 B for this frame on real AEN801 -- magnitude reference, not an "
	       "expected value) hw=%s\n",
	       soi_ok ? "ok" : "BAD",
	       eoi_ok ? "ok" : "BAD",
	       len_ok ? "ok" : "BAD",
	       JPEG_MIN_PLAUSIBLE_LEN,
	       (unsigned)JPEG_SRC_LEN,
	       caps.hw_accelerated ? "ok" : "BAD -- software fallback won, this proves nothing");

	return ((caps_rc == ALP_OK) && caps.hw_accelerated && (rc == ALP_OK) && len_ok && soi_ok &&
	        eoi_ok)
	           ? PHASE_PASS
	           : PHASE_FAIL;
}

/* ==================================================================== */
/* STUBS -- phases not in this slice.  Each reason differs; see below.  */
/* ==================================================================== */

/* Rotary encoder: wiring is netlist-proven (UTIMER channel 12) but count
 * still reads 0 on the bench, and the maintainer's own open question is
 * whether the driver can tell "nobody is turning the knob" apart from
 * "not counting at all" -- an unattended bench run cannot supply the
 * physical input either check needs. Implementing this phase without a
 * human at the bench would only ever print SKIPPED or a FAIL this app
 * cannot distinguish from "the knob wasn't touched" -- worse than being
 * honest about the gap. Needs an attended run; out of THIS slice. */
static phase_verdict_t phase_encoder_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: rotary encoder -- SKIPPED (needs an attended run; "
	       "see EVK-BRIEFING.md's open question) --\n");
	return PHASE_SKIPPED;
}

/* CC3501E Wi-Fi/BLE: the coprocessor is ALREADY ACTIVATED on every SoM and
 * radio ops are expected -- but the bridge protocol dispatch (4-phase SPI
 * exchange, hardware SS0 + READY gating) is a larger unit of work than
 * fits this slice alongside six other phases, and a partial/rushed
 * implementation risks tripping the hard constraint against ever
 * re-activating or re-flashing the bridge. Deferred to the next slice,
 * not because the hardware is missing. */
static phase_verdict_t phase_cc3501e_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: CC3501E Wi-Fi/BLE -- SKIPPED (bridge protocol dispatch "
	       "deferred to the next slice, not a hardware gap) --\n");
	return PHASE_SKIPPED;
}

/* SD card: per DEMO-DESIGN.md's order constraint, the SDIO mux (EN/SEL on
 * CC35 GPIO_26/GPIO_30) is not reachable until the CC3501E bridge phase
 * above is implemented and running -- so this phase is blocked on that
 * one, not on whether a card is in the slot. Deferred alongside it. */
static phase_verdict_t phase_sdcard_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: SD card -- SKIPPED (the SDIO mux is behind the CC3501E "
	       "bridge, deferred alongside it) --\n");
	return PHASE_SKIPPED;
}

/* Ethernet: not implemented this slice; deferred alongside the other
 * SoC-peripheral phases below to keep this drop to a working core. */
static phase_verdict_t phase_ethernet_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: Ethernet -- SKIPPED (deferred to the next slice) --\n");
	return PHASE_SKIPPED;
}

/* Sound out -> PDM-in loopback: the maintainer has confirmed speakers ARE
 * connected to both TAS2563 amps, so a real tone-out + mic-capture
 * end-to-end check is possible in a future slice -- but the I2S bring-up
 * this needs isn't in this slice, and the amp is a ~15 W class-D part: a
 * rushed first attempt at the volume ramp is exactly the kind of thing
 * that should not be the first time this code runs. Deferred, not
 * skipped for lack of hardware. */
static phase_verdict_t phase_sound_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: sound out -> PDM in -- SKIPPED (I2S bring-up + the "
	       "low-volume ramp policy deferred to the next slice) --\n");
	return PHASE_SKIPPED;
}

/* Screen (DSI): no panel is attached on this bench and a clean DSI init
 * would not prove one is -- deferred to the next slice regardless. */
static phase_verdict_t phase_screen_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: screen (DSI) -- SKIPPED (no panel on this bench; "
	       "deferred to the next slice) --\n");
	return PHASE_SKIPPED;
}

/* NPU inference: blocked by a BOOT-FLOW constraint, not by missing code
 * and not by a camera. examples/aen/aen-npu-inference-alp is the
 * silicon-proven Ethos-U85 path through <alp/inference.h> -- but its
 * Vela-compiled person_detect_u85 model is ~263 KiB, which is precisely
 * why THAT app links into MRAM slot0 and boots via Flow D instead of
 * RAM-running. This demo is a Flow C ITCM RAM-run: ITCM is 256 KB total
 * and the demo already occupies 108552 B (41.41%) of it -- it was 95008 B
 * (36.24%) before phase 13, so the headroom is shrinking, not growing. A
 * ~263 KiB model does not fit in what is left, by a wide margin and not
 * by a trimmable one. Adding NPU inference here therefore means relinking the whole
 * demo into MRAM slot0 and switching its boot flow -- a different unit of
 * work from adding a phase, and out of scope for this slice. Shrinking
 * the model to fit would trade the proven artefact for an unproven one,
 * which is the opposite of what this app is for. */
static phase_verdict_t phase_npu_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: NPU inference -- SKIPPED (needs a boot-flow change, not a "
	       "phase: aen-npu-inference-alp's person_detect_u85 model is ~263 KiB and this "
	       "demo is a Flow C ITCM RAM-run -- 256 KB ITCM total, 108552 B (41.41%%) already "
	       "used, so the model would have to move the whole image to MRAM slot0 / Flow D) "
	       "--\n");
	return PHASE_SKIPPED;
}

/* ==================================================================== */
/* Phase table + main                                                    */
/* ==================================================================== */

static const phase_t PHASES[] = {
	{ "RTC + temperature (BRD_I2C)", phase_rtc_temp },
	{ "Sensors (BMI323/ICM42670/BMP581)", phase_sensors },
	{ "Power rails (6x INA236)", phase_power_rails },
	{ "I/O expander answers (TCAL9538, read-only)", phase_io_expander },
	{ "EEPROM identity (24C128)", phase_eeprom_identity },
	{ "RGB LED (PWM0/1/3)", phase_rgb_led },
	{ "Rotary encoder", phase_encoder_stub },
	{ "CC3501E Wi-Fi/BLE", phase_cc3501e_stub },
	{ "SD card", phase_sdcard_stub },
	{ "Ethernet", phase_ethernet_stub },
	{ "Sound out -> PDM in", phase_sound_stub },
	{ "Screen (DSI)", phase_screen_stub },
	{ "JPEG encode (Hantro VC9000E)", phase_jpeg_encode },
	{ "NPU inference", phase_npu_stub },
};

int main(void)
{
	printf("\n=== aen-evk-demo: E1M-AEN801 / E1M EVK phased demo ===\n");

	(void)alp_init();

	demo_ctx_t ctx = { 0 };

	/* BRD_I2C -- SoC I2C0, portable bus index 2 (0 and 1 are the E1M EDGE
	 * buses; BRD_I2C has no edge route of its own, #1848). Board-layer
	 * enabled; no per-app overlay needed. */
	ctx.brd_bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 2u,
	    .bitrate_hz = 100000u,
	});
	if (ctx.brd_bus == NULL) {
		printf("[evkdemo] alp_i2c_open(BRD_I2C) -> NULL, err=%d\n", (int)alp_last_error());
	}

	/* Carrier bus -- SoC I2C2, EVK_I2C_BUS_SENSORS / ALP_E1M_I2C0. Also
	 * board-layer enabled. */
	ctx.carrier_bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	if (ctx.carrier_bus == NULL) {
		printf("[evkdemo] alp_i2c_open(carrier bus) -> NULL, err=%d\n", (int)alp_last_error());
	}

	phase_verdict_t results[ARRAY_SIZE(PHASES)];
	int             n_pass = 0, n_skipped = 0, n_fail = 0;

	for (size_t i = 0; i < ARRAY_SIZE(PHASES); i++) {
		results[i] = PHASES[i].run(&ctx);
		printf("[evkdemo] phase %2zu/%2zu: %-34s %s\n",
		       i + 1,
		       ARRAY_SIZE(PHASES),
		       PHASES[i].name,
		       verdict_str(results[i]));
		switch (results[i]) {
		case PHASE_PASS:
			n_pass++;
			break;
		case PHASE_SKIPPED:
			n_skipped++;
			break;
		case PHASE_FAIL:
		default:
			n_fail++;
			break;
		}
	}

	if (ctx.carrier_bus != NULL) alp_i2c_close(ctx.carrier_bus);
	if (ctx.brd_bus != NULL) alp_i2c_close(ctx.brd_bus);

	printf("\n[evkdemo] ==================== SUMMARY ====================\n");
	for (size_t i = 0; i < ARRAY_SIZE(PHASES); i++) {
		printf("[evkdemo]   %-38s %s\n", PHASES[i].name, verdict_str(results[i]));
	}
	printf("[evkdemo] ====================================================\n");

	/* A run full of SKIPPED phases is not a failed run -- all three counts
	 * are reported together so that distinction stays legible at a
	 * glance, exactly the property i2c-device-hub's old tally lacked. */
	printf("[evkdemo] RESULT: %d PASS, %d SKIPPED, %d FAIL\n", n_pass, n_skipped, n_fail);
	printf("[evkdemo] done\n");
	return 0;
}
