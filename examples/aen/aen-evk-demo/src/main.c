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
 * Fourteen phases are registered below, run in a fixed order. Nine have
 * real, bench-proven drivers behind them and are fully implemented: the
 * first six, plus phase 8 (the CC3501E Wi-Fi 6 / BLE 5.4 coprocessor over
 * the inter-chip SPI bridge), phase 10 (RMII Ethernet through the GMAC and
 * the on-module DP83825 PHY) and phase 13 (JPEG encode on the Hantro
 * VC9000E). The remaining five (encoder, SD card, sound, screen, NPU) are
 * stubs that always report SKIPPED with a phase-specific reason
 * -- see the "STUBS" section below for why each one differs (an
 * attended-run requirement is not the same kind of gap as hardware
 * genuinely out of scope, a larger unit of work deferred to the next
 * slice, or -- for NPU -- an image that would have to change BOOT FLOW to
 * hold the model). Attempting all eleven-plus phases of the full design in
 * one drop would have meant shipping several of them unverified against
 * real silicon; a working core that others can extend safely is worth more
 * than an unverifiable giant one.
 *
 * MEMORY: phase 10 moved the whole image's SYSTEM RAM out of the M55 DTCM
 * into the global on-chip SRAM0 bank, because the GMAC DMA cannot reach
 * DTCM and the descriptor rings and net_buf pool are driver-owned statics
 * no application can place. That is an image-wide change affecting every
 * phase, and the reasoning -- including why the naive form of it silently
 * corrupts phase 13, and what it costs the other phases -- is written out
 * in full at the top of this app's board overlay. Read that before
 * changing anything about memory here.
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
 * A third bus exists but is NOT shared through demo_ctx_t: SPI1, the
 * SoM-internal Alif <-> CC3501E inter-chip link. Phase 8 owns it end to
 * end -- it has to power the coprocessor before the bus has anything on
 * the other end of it, and no other phase touches it -- so opening it in
 * main() alongside the two I2C buses would only move a phase-local concern
 * somewhere it does not belong.
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
#include <errno.h>
#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h> /* ARRAY_SIZE, BIT */

/*
 * Phase 10 only. Two Zephyr headers this app would otherwise never reach for,
 * and both are deliberate rather than an oversight of the portable API:
 *
 *  - <zephyr/init.h> + <zephyr/drivers/gpio.h> + <zephyr/drivers/pinctrl.h>:
 *    the PHY's power and reset lines have to be driven from a SYS_INIT hook,
 *    BEFORE main() and before the Ethernet driver's own init (see
 *    eth_phy_power_init() for why the ordering is the whole trick).
 *    <alp/gpio.h> cannot serve that: its backend is not up that early, and
 *    these two nets are board-control lines with no E1M instance ID to open
 *    anyway -- the same reason src/cc3501e_bridge.c reaches for raw pad
 *    control.
 *  - the four zephyr/net headers: the portable API publishes no Ethernet or
 *    IP peripheral class at all. Its Wi-Fi surface, <alp/iot.h>, is a
 *    different peripheral on a different chip -- phase 8's coprocessor --
 *    so there is nothing here to route through it.
 */
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>

/*
 * Phase 9 only, and deliberate for the same reason the Ethernet headers above
 * are: the portable <alp/...> API publishes no block-device or filesystem
 * peripheral class at all, so there is nothing here to route through it. The
 * ONE thing phase 9 does reach for portably is the mux ENABLE pin, and that
 * IS opened through <alp/peripheral.h>'s alp_gpio_* on a portable E1M pin id
 * (ALP_E1M_GPIO_IO20) -- the CC3501E proxy backend turns it into a bridge
 * transaction without this file naming a raw coprocessor GPIO index.
 *
 * <ff.h> is the ELM FatFs work-area type (FATFS), needed because fs_mount()
 * takes a caller-owned one; it is not otherwise called into.
 */
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/disk.h> /* DISK_STATUS_NOMEDIA -- the no-card fork */
#include <zephyr/fs/fs.h>
#include <ff.h>

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
#include "alp/chips/cc3501e.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

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

	/**
	 * Optional one-word QUALIFIER a phase may set on its own verdict, carried
	 * into the summary table (NULL = none; main() clears it between phases).
	 *
	 * Three verdicts are the right number to gate on, but they are not always
	 * the whole truth, and the summary table is what most readers actually
	 * read. Phase 8's empty Wi-Fi scan is the case that forced this: it is a
	 * genuine PASS -- the scan round-tripped -- but nothing corroborated it,
	 * and a reader of the table alone could not tell that run from one that
	 * saw a dozen networks. A qualifier is NOT a fourth verdict: it never
	 * changes what the phase counts as.
	 */
	const char *note;
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
/* Phase 8 -- CC3501E Wi-Fi 6 / BLE 5.4 coprocessor (inter-chip SPI)     */
/* ==================================================================== */

/*
 * Grouped with the other IMPLEMENTED phases even though it runs eighth --
 * the file's organising principle is "real phases first, stubs after".
 *
 * WHAT THIS PHASE TALKS TO. The CC3501E is a second microcontroller on the
 * SoM (module U4 = BDE-BW35N) running Alp Lab's own bridge firmware. It is
 * NOT a register-level peripheral: every operation below is a request/reply
 * transaction over an inter-chip SPI link, four SS0-framed phases each, with
 * the host gating every reply phase on the slave's READY line. That framing
 * lives entirely in chips/cc3501e/ -- this phase calls the driver and never
 * hand-rolls a frame.
 *
 * ITS SUPPLY IS HOST-GATED. The coprocessor has NO POWER until the Alif
 * drives WIFI_EN (P15_5) high; a J-Link cannot even attach to it before
 * that (VTref reads 0 V). So nothing here can answer until this phase runs
 * its power + reset sequence, which is the first thing it does. A "the part
 * didn't answer" result therefore has to be read as "power/reset/link", not
 * "the radio is broken".
 *
 * HARD CONSTRAINT -- READ BEFORE EXTENDING THIS PHASE. There is NO path
 * here that activates, re-activates, provisions or re-flashes the CC3501E's
 * firmware, and there must never be one. The parts ship ALREADY ACTIVATED
 * from SoM provisioning, no SDK opcode can activate one, and the fuses
 * involved are OR-only -- a botched activation permanently bricks a unit's
 * secure boot (one bench unit was lost that way). The OTA opcodes
 * (cc3501e_ota_*) exist in the driver and are deliberately not called from
 * this app at all. Radio operations -- scan, BLE enable -- are ordinary
 * runtime commands and are exactly what this phase is for.
 *
 * NO NETWORK IS JOINED. The phase runs WIFI_SCAN_START and stops there: it
 * never calls cc3501e_wifi_connect(), and there are no credentials anywhere
 * in this file or its build. A scan is passive listening; associating would
 * put a bench board on somebody's network and would need a secret to do it.
 *
 * WHAT IT LEAVES BEHIND. WIFI_EN stays HIGH, `cc35_fw` stays bound, and the
 * BLE controller stays enabled when it came up -- deliberately, because the
 * SD-card phase's SDIO mux (EN/SEL on CC35 GPIO_26 / GPIO_30) is reachable
 * only through this coprocessor, so powering it back down here would make
 * that phase impossible to add later. The cost is that phases 9-14 run with
 * both radios up; on a bench-diagnostic image that is the right trade.
 */

/* Bounded retry for the first PING. cc3501e_reset() has already waited out
 * the boot budget, so attempt 1 usually lands; this only absorbs residual
 * ramp/boot jitter. 25 x 200 ms = 5 s, which is a bound, not a hope: a part
 * that has not answered in five seconds is not late, it is not there. */
#define CC35_PING_RETRIES 25u
#define CC35_PING_GAP_MS  200u

/* Poll-by-repeat budgets. GET_MAC, WIFI_SCAN_START and BLE_ENABLE are all
 * worker-routed on the firmware side: it answers BUSY while its worker runs
 * and the host driver re-issues until OK or the budget expires. These are
 * the same budgets aen-cc3501e-bringup uses on real silicon. */
#define CC35_MAC_TIMEOUT_MS  2000u
#define CC35_SCAN_TIMEOUT_MS 8000u
#define CC35_BLE_TIMEOUT_MS  10000u

/* Scan records collected. 16 is plenty for a bench desk and bounds the
 * static buffer at ~700 B; the firmware simply stops filling past `cap`. */
#define CC35_SCAN_MAX_RECORDS 16u

/*
 * FILE-STATIC, not a local, for two independent reasons -- both bench-proven
 * on this silicon:
 *
 *  1. sizeof(cc3501e_t) is ~32 KB (the driver keeps its tx/rx scratch, scan
 *     and socket buffers INSIDE the handle). As a stack local the function
 *     prologue crosses PSPLIM and the M55 raises STKOF -> UsageFault before
 *     a single line is printed. aen-cc3501e-bringup pays for its own local
 *     with CONFIG_MAIN_STACK_SIZE=32768; a static handle is cheaper and this
 *     app has fourteen other phases to fund.
 *  2. The SD-card phase needs this same bound handle to reach the SDIO mux
 *     on CC35 GPIO_26/GPIO_30. A handle scoped to phase 8's frame would be
 *     gone by the time phase 9 ran.
 */
static cc3501e_t cc35_fw;

/*
 * Is this a real, factory-programmed station MAC?
 *
 * GET_MAC returning ALP_OK is NOT the check -- that is precisely the
 * "the call succeeded, therefore the hardware works" reasoning that let
 * i2c-device-hub report a pass over sensors stuck at their reset sentinel.
 * A radio that has not read its identity out yet answers with a structurally
 * impossible address, and these are the shapes that takes:
 *
 *   00:00:00:00:00:00  -- nothing was read; the field is still zeroed.
 *   ff:ff:ff:ff:ff:ff  -- the broadcast address; an erased/undriven read.
 *   xx with bit 0 of the first octet SET -- the IEEE 802 group bit. A
 *     GROUP address can never be a station's own source address, so any
 *     value with it set is garbage however random it looks.
 *   a ZERO OUI (first three octets all 00) -- IEEE never assigns one, so
 *     no station can own such an address. This is the check that catches
 *     the failure THIS LINK actually produces: the documented READY re-arm
 *     race hands back a reply shifted one byte, with a leading 0x00, so a
 *     good 44:3e:8a:10:b6:9e reads as 00:44:3e:8a:10:b6 -- group bit clear,
 *     neither constant pattern, and it would otherwise pass. It also
 *     catches a half-populated reply that only filled the tail.
 *
 * WHAT THIS DELIBERATELY DOES NOT REJECT, and why it would be wrong to:
 * the address the E1M-AEN SoMs actually carry is a TI FACTORY MAC in the
 * 44:3E:8A MA-L block. Alp Lab holds no IEEE OUI and needs none -- the MAC
 * eFuse is an OVERRIDE that reads 0 on a good part, and the TI-assigned
 * address IS the valid one. Rejecting "a TI-looking MAC" would fail every
 * correctly provisioned module in the fleet. The OUI is printed below so a
 * reader can see which block the address came from and judge for themselves.
 *
 * WHY STRUCTURE ALONE IS NOT ENOUGH, and what the caller adds on top:
 * a single flipped bit produces an address that is still structurally
 * perfect. Measured, sweeping the master's RX sample delay on E1M-AEN803
 * serial 2026W36-0002: one run came back with byte 0 reading 0x46 instead
 * of 0x44 -- group bit clear, non-zero OUI, neither constant pattern -- and
 * this function returned true, so the phase printed PASS over a link that
 * was actively corrupting data. No structural test can tell a bit-flipped
 * MAC from a legitimate one, because both are legitimate-looking addresses;
 * the check cannot hard-code the right value either, since it is per-part.
 * So the caller issues GET_MAC TWICE and requires the two replies to be
 * identical. That is a genuine second wire round-trip, not a re-read of a
 * cached reply: cc3501e_wifi_get_mac() runs the full poll_by_repeat of
 * CMD_GET_MAC (0x03) every call, and the driver keeps no MAC state.
 *
 * What the repeat catches: TRANSIENT corruption -- a bit flip, a byte
 * shift, a half-filled reply -- on either read. What it does NOT catch: a
 * STABLE misread, where the radio or the link returns the same wrong
 * address both times. Nothing available to this app catches that one.
 */
static bool cc35_mac_plausible(const uint8_t mac[CC3501E_MAC_LEN])
{
	bool all_zero = true, all_ff = true;

	for (size_t i = 0; i < CC3501E_MAC_LEN; i++) {
		if (mac[i] != 0x00u) all_zero = false;
		if (mac[i] != 0xFFu) all_ff = false;
	}
	bool zero_oui = ((mac[0] | mac[1] | mac[2]) == 0x00u);

	return !all_zero && !all_ff && !zero_oui && ((mac[0] & 0x01u) == 0u);
}

/*
 * Is one scan record a real access-point report, or filler?
 *
 * Same argument as the MAC check, applied to the scan payload: a worker
 * that answered OK with a zeroed record array would otherwise be counted as
 * "networks seen". A genuine report has a non-zero BSSID and an RSSI that
 * is actually negative -- received power at an antenna is below 1 mW by a
 * wide margin, so 0 dBm is not a weak signal, it is an unwritten field.
 * -110 dBm is below the noise floor of any 802.11 receiver, so anything at
 * or under it is equally unwritten.
 */
static bool cc35_scan_record_plausible(const cc3501e_scan_record_t *r)
{
	bool bssid_zero = true;

	for (size_t i = 0; i < sizeof(r->bssid); i++) {
		if (r->bssid[i] != 0x00u) bssid_zero = false;
	}
	return !bssid_zero && (r->rssi_dbm < 0) && (r->rssi_dbm > -110);
}

static phase_verdict_t phase_cc3501e(demo_ctx_t *ctx)
{
	/* The coprocessor is on SPI1, not on either I2C bus -- ctx is used only
	 * to hang the empty-scan qualifier off, at the end. */
	printf("[evkdemo] -- Phase: CC3501E Wi-Fi 6 / BLE 5.4 (inter-chip SPI1 bridge) --\n");

	/* --- 1. Power + reset + open the link ---------------------------- */
	/* One call: opens SPI1 (hardware SS0, ALP_SPI_NO_CS) and the WIFI_EN /
	 * nRESET / READY pins, turns the LP pads' output drivers on (pinctrl
	 * does not reach the LP island), binds them, then runs the power-up and
	 * reset sequence -- including the Puya-flash double-boot workaround: a
	 * cold power-on mis-reads the PY25Q64LB on the FIRST boot, so the part
	 * is re-booted once with the rails already up. Blocks ~900 ms. */
	alp_status_t rc = cc3501e_bridge_bringup(&cc35_fw);
	printf("[evkdemo] CC3501E: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);
	if (rc == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) {
		/* The backend authority itself says the bus/pins are absent --
		 * i.e. this image's overlay does not declare the bridge. That is
		 * a build fault, not a coprocessor fault, and the coprocessor is
		 * fitted on every E1M-AEN SoM, so it is NOT a "hardware absent"
		 * skip: nothing about this board justifies the phase not running. */
		printf("[evkdemo] CC3501E: SPI bus %u / WIFI_EN+nRESET not in the devicetree (err=%d) -- "
		       "this app's overlay must declare them; the part is fitted on every E1M-AEN SoM, so "
		       "this is a build fault, not an absent part\n",
		       (unsigned)CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		return PHASE_FAIL;
	}
	if (rc == ALP_ERR_VERSION) {
		/* cc3501e_reset() refused on a MAJOR protocol skew and CLEARED
		 * `initialised`, so every call below would return ALP_ERR_NOT_READY
		 * the instant it was made -- 25 PINGs of nothing, five seconds of
		 * sleeps, and then a cause list that does not contain the cause.
		 * Short-circuit and print what the driver deliberately recorded on
		 * exactly this path so a caller could report it. */
		printf(
		    "[evkdemo] CC3501E: firmware speaks protocol v%u.%u, this host is built for v%u.%u -- "
		    "MAJOR skew, so the driver refused the link and marked the handle down. %s\n",
		    cc35_fw.fw_proto_major,
		    cc35_fw.fw_proto_minor,
		    (unsigned)ALP_CC3501E_PROTOCOL_MAJOR,
		    (unsigned)ALP_CC3501E_PROTOCOL_MINOR,
		    (cc35_fw.fw_proto_major == 0u)
		        ? "MAJOR 0 means the firmware predates the versioning scheme entirely (it answered "
		          "a raw v1..v9 integer) -- reflash the bridge"
		        : "The two binaries disagree about the frame layout; talking anyway would "
		          "misread every reply");
		return PHASE_FAIL;
	}

	/* --- 2. PING until it answers, bounded ---------------------------- */
	/* META opcode 0x00. A serviced PING proves the firmware parsed a frame
	 * and staged a reply across the SS0-framed, READY-gated link -- the
	 * whole transport, both ends. It proves NOTHING about either radio,
	 * which is why it is only the FIRST of five things gated on below. */
	alp_status_t ping_rc  = ALP_ERR_TIMEOUT;
	unsigned     attempts = 0u;
	for (; attempts < CC35_PING_RETRIES; ++attempts) {
		ping_rc = cc3501e_ping(&cc35_fw);
		if (ping_rc == ALP_OK) {
			attempts++; /* count the one that succeeded, not the ones before it */
			break;
		}
		k_msleep(CC35_PING_GAP_MS);
	}
	printf("[evkdemo] CC3501E: PING (0x00) -> %d after %u attempt(s) of %u (%u ms apart)\n",
	       (int)ping_rc,
	       attempts,
	       CC35_PING_RETRIES,
	       CC35_PING_GAP_MS);
	if (ping_rc != ALP_OK) {
		/* Every later call would time out against a dead link and add
		 * seconds of nothing to a run that costs a bench reservation.
		 * Stop here -- with the codes, not silently.
		 *
		 * The figure below is the SLEEP budget only. Each attempt also
		 * spends its own transport timeout inside cc3501e_ping(), so real
		 * elapsed time is longer -- stated rather than quietly understated,
		 * because a reader timing the run against this line would otherwise
		 * conclude something else was hanging. */
		printf("[evkdemo] CC3501E: no answer after %u ms of retry gaps (plus each attempt's own "
		       "transport timeout, so longer in wall-clock) -- check WIFI_EN (P15_5) actually went "
		       "high, the SPI1 pinmux (P14_4/5/6/7), the READY line (P2_6), and that the "
		       "coprocessor is running its bridge firmware. NOT a skip: the part is fitted and "
		       "powered by this phase, so silence is a failure\n",
		       (unsigned)(CC35_PING_RETRIES * CC35_PING_GAP_MS));
		return PHASE_FAIL;
	}

	/* --- 3. Identity: VERSION, MAC, CAPABILITIES ---------------------- */

	/*
	 * GET_VERSION (0x01) -- the PROTOCOL version, not the firmware build.
	 * cc3501e_get_version() deliberately does not compare it (its callers
	 * include liveness soaks), so the comparison is made here.
	 *
	 * MAJOR ONLY, per ADR 0033 and the driver's own policy
	 * (chips/cc3501e/cc3501e_core.c). MAJOR means "an unchanged host would be
	 * MISREAD" -- reused reserved bytes, changed struct layout, changed
	 * framing -- so it is the only difference that makes the replies below
	 * untrustworthy. MINOR is defined as purely ADDITIVE, and connecting
	 * across it is safe precisely because of that: this host never sends an
	 * opcode it does not know, and the firmware never spontaneously emits an
	 * event nobody armed. Demanding an exact match would fail a firmware ADR
	 * 0033 declares compatible, which is a false FAIL on a good board.
	 *
	 * A minor delta is INFORMATION, printed and not gated: lower than this
	 * host's minor means the firmware lacks newer features (GET_CAPABILITIES
	 * below is the better question about that anyway), higher means it has
	 * features this phase does not use.
	 *
	 * Note this line is not where a MAJOR skew is normally caught -- the
	 * bring-up above already refused and returned. It stays gated as a
	 * backstop for a firmware that somehow changes its answer afterwards.
	 */
	uint16_t     version  = 0u;
	alp_status_t ver_rc   = cc3501e_get_version(&cc35_fw, &version);
	unsigned     fw_major = ALP_CC3501E_PROTOCOL_VERSION_MAJOR(version);
	unsigned     fw_minor = ALP_CC3501E_PROTOCOL_VERSION_MINOR(version);
	bool         ver_ok = (ver_rc == ALP_OK) && (fw_major == (unsigned)ALP_CC3501E_PROTOCOL_MAJOR);
	printf(
	    "[evkdemo] CC3501E: GET_VERSION (0x01) -> %d protocol v%u.%u (host built for v%u.%u) %s\n",
	    (int)ver_rc,
	    fw_major,
	    fw_minor,
	    (unsigned)ALP_CC3501E_PROTOCOL_MAJOR,
	    (unsigned)ALP_CC3501E_PROTOCOL_MINOR,
	    !ver_ok ? "MAJOR MISMATCH -- replies below are parsed against the host's layout"
	    : (fw_minor == (unsigned)ALP_CC3501E_PROTOCOL_MINOR)
	        ? "match"
	        : "major match, minor differs -- additive by definition (ADR 0033), so the link is "
	          "compatible; not gated on");

	/* GET_MAC (0x03) -- poll-by-repeat, so an OK here also proves the
	 * firmware's worker seam (submit -> worker -> reply), not just META
	 * dispatch off the SPI ISR. The address itself is then checked for
	 * structural validity; see cc35_mac_plausible().
	 *
	 * Issued TWICE, and the two replies must match byte for byte. Structure
	 * cannot catch a single flipped bit -- a corrupted address still looks
	 * like an address -- and the correct value is per-part, so it cannot be
	 * hard-coded either. Each call is a full CMD_GET_MAC round-trip (nothing
	 * is cached host-side), so a repeat that agrees is two independent reads
	 * agreeing. Transient corruption is caught; a stable misread is not.
	 * See cc35_mac_plausible()'s header for the run that motivated this. */
	uint8_t      mac[CC3501E_MAC_LEN]  = { 0 };
	uint8_t      mac2[CC3501E_MAC_LEN] = { 0 };
	alp_status_t mac_rc                = cc3501e_wifi_get_mac(&cc35_fw, mac, CC35_MAC_TIMEOUT_MS);
	alp_status_t mac_rc2 =
	    (mac_rc == ALP_OK) ? cc3501e_wifi_get_mac(&cc35_fw, mac2, CC35_MAC_TIMEOUT_MS) : mac_rc;
	bool mac_repeatable = (mac_rc2 == ALP_OK) && (memcmp(mac, mac2, CC3501E_MAC_LEN) == 0);
	bool mac_ok         = (mac_rc == ALP_OK) && cc35_mac_plausible(mac) && mac_repeatable;
	printf("[evkdemo] CC3501E: GET_MAC (0x03) -> %d  %02x:%02x:%02x:%02x:%02x:%02x  "
	       "OUI=%02x:%02x:%02x  %s\n",
	       (int)mac_rc,
	       mac[0],
	       mac[1],
	       mac[2],
	       mac[3],
	       mac[4],
	       mac[5],
	       mac[0],
	       mac[1],
	       mac[2],
	       mac_ok ? "plausible station MAC, and a second read agreed"
	              : "INVALID (all-zero, broadcast, the IEEE group bit set -- not an address a "
	                "station can own -- or a second read disagreed)");
	/* Print the disagreement in full when it happens: which bytes moved is
	 * the whole diagnostic, and a verdict line that only said "INVALID"
	 * would throw it away. */
	if (mac_rc == ALP_OK && !mac_repeatable) {
		printf("[evkdemo] CC3501E: GET_MAC re-read (0x03) -> %d  "
		       "%02x:%02x:%02x:%02x:%02x:%02x -- DISAGREES with the first read, so the link "
		       "corrupted at least one of them; the address is not trustworthy either way\n",
		       (int)mac_rc2,
		       mac2[0],
		       mac2[1],
		       mac2[2],
		       mac2[3],
		       mac2[4],
		       mac2[5]);
	}

	/* GET_CAPABILITIES (0x06) -- ASK what the firmware implements rather
	 * than infer it from the version number. A build without Wi-Fi or
	 * without BLE reports the SAME wire version as a full one while its
	 * radio opcodes are NOTIMPL stubs, so the bitmap is the only honest
	 * source. The two bits this phase then goes on to exercise are named in
	 * the log; both are expected present on every shipped SoM. */
	uint32_t     caps     = 0u;
	alp_status_t caps_rc  = cc3501e_get_capabilities(&cc35_fw, &caps);
	bool         cap_wifi = (caps & ALP_CC3501E_CAP_WIFI_STA) != 0u;
	bool         cap_ble  = (caps & ALP_CC3501E_CAP_BLE) != 0u;
	/* ALP_ERR_INVAL means "firmware predates this opcode", i.e. no
	 * capability INFORMATION -- not "no capabilities". Said plainly in the
	 * log so nobody reads a 0x00000000 bitmap as a stripped firmware. */
	printf("[evkdemo] CC3501E: GET_CAPABILITIES (0x06) -> %d caps=0x%08x wifi_sta=%s ble=%s%s\n",
	       (int)caps_rc,
	       (unsigned)caps,
	       cap_wifi ? "yes" : "no",
	       cap_ble ? "yes" : "no",
	       (caps_rc == ALP_ERR_INVAL)
	           ? " (INVAL = firmware predates the opcode: no capability information, which is NOT "
	             "the same as no capabilities)"
	           : "");

	/* --- 4. Wi-Fi scan -- listen only, never associate ----------------- */
	/* WIFI_SCAN_START (0x10), poll-by-repeat: the firmware answers BUSY
	 * while the scan runs and the driver re-issues until the records come
	 * back as the reply payload. Passive: no probe of any network's
	 * security, no association, no credentials. */
	static cc3501e_scan_record_t scan[CC35_SCAN_MAX_RECORDS];
	size_t                       n_scan = 0u;
	alp_status_t                 scan_rc =
	    cc3501e_wifi_scan(&cc35_fw, scan, CC35_SCAN_MAX_RECORDS, &n_scan, CC35_SCAN_TIMEOUT_MS);

	size_t n_plausible = 0u;
	for (size_t i = 0; i < n_scan; i++) {
		if (cc35_scan_record_plausible(&scan[i])) n_plausible++;
		printf("[evkdemo] CC3501E:   scan[%u] \"%s\" ch%u %d dBm %s bssid=%02x:%02x:%02x:%02x:%02x:"
		       "%02x %s\n",
		       (unsigned)i,
		       scan[i].ssid,
		       (unsigned)scan[i].channel,
		       (int)scan[i].rssi_dbm,
		       cc3501e_wifi_sec_name(scan[i].security_info),
		       scan[i].bssid[0],
		       scan[i].bssid[1],
		       scan[i].bssid[2],
		       scan[i].bssid[3],
		       scan[i].bssid[4],
		       scan[i].bssid[5],
		       cc35_scan_record_plausible(&scan[i]) ? "ok" : "FILLER");
	}

	/*
	 * HOW AN EMPTY SCAN IS JUDGED, and why it is judged this way.
	 *
	 * Zero networks does NOT fail this phase. A shielded room, a screened
	 * bench, or simply a quiet band are all real, and a demo that turned an
	 * honest empty result into a red line would be lying in the other
	 * direction -- it would be asserting something about the RF environment
	 * that this board cannot know.
	 *
	 * What IS gated is the thing the phase can actually establish: that the
	 * scan round-tripped. WIFI_SCAN_START is poll-by-repeat, so ALP_OK means
	 * the firmware accepted the request, ran its scan worker to completion,
	 * and returned a well-formed reply payload -- radio submitted, radio
	 * finished, answer parsed. That is a materially stronger claim than "a
	 * register read returned a value", which is the claim this whole app
	 * exists to stop counting as a pass.
	 *
	 * And when records DO come back, they are checked rather than tallied:
	 * a reply full of zeroed records would otherwise be reported as
	 * "networks seen". If any record arrived, at least one must look like a
	 * real AP report or the payload is filler and the phase fails.
	 *
	 * The consequence is stated in the log every run: an empty scan is
	 * PASS-but-UNCORROBORATED, and the reader is told so in words rather
	 * than left to infer it from a 0.
	 */
	bool scan_ok = (scan_rc == ALP_OK) && ((n_scan == 0u) || (n_plausible > 0u));
	printf(
	    "[evkdemo] CC3501E: WIFI_SCAN_START (0x10) -> %d  %u network(s) seen, %u plausible  %s\n",
	    (int)scan_rc,
	    (unsigned)n_scan,
	    (unsigned)n_plausible,
	    (scan_rc != ALP_OK) ? "SCAN FAILED"
	    : (n_scan == 0u)
	        ? "UNCORROBORATED -- the scan completed and the reply parsed, which is what is gated "
	          "on; zero networks is a statement about the RF environment, not about this board, "
	          "so it does NOT fail the phase"
	    : (n_plausible == 0u) ? "FILLER -- records came back but none is a real AP report "
	                            "(zero BSSID / impossible RSSI)"
	                          : "ok");
	/* No cc3501e_wifi_connect() call anywhere in this phase, and no
	 * credentials in this file -- see the section header. */

	if (scan_rc != ALP_OK) {
		/* A timed-out scan may still be RUNNING on the coprocessor -- the
		 * poll-by-repeat budget expiring says the host gave up, not that the
		 * worker did. BLE_ENABLE below brings the Wi-Fi stack up first over
		 * the shared HIF, so leaving a scan in flight would make BLE fail
		 * too and misattribute one fault as two. Tear it down first; a stop
		 * against a scan that already finished is harmless. */
		alp_status_t stop_rc = cc3501e_wifi_scan_stop(&cc35_fw);
		printf("[evkdemo] CC3501E: WIFI_SCAN_STOP (0x11) -> %d (scan did not complete; torn down "
		       "before BLE_ENABLE so a still-running scan cannot fail the shared HIF too)\n",
		       (int)stop_rc);
	}

	/* --- 5. BLE ------------------------------------------------------- */
	/* BLE_ENABLE (0x30) brings up the BLE controller AND the NimBLE host on
	 * the coprocessor. The firmware worker-routes it off the SPI ISR and
	 * brings the Wi-Fi stack up first (shared HIF), so the bridge is briefly
	 * down mid-op and the driver re-issues -- another real worker-seam
	 * exercise, not a flag write. ALP_ERR_NOT_READY specifically means BLE
	 * is not built into this firmware; that is a genuine deviation from what
	 * every shipped SoM carries, so it fails rather than skips. */
	alp_status_t ble_rc = cc3501e_ble_enable(&cc35_fw, CC35_BLE_TIMEOUT_MS);
	printf(
	    "[evkdemo] CC3501E: BLE_ENABLE (0x30) -> %d %s\n",
	    (int)ble_rc,
	    (ble_rc == ALP_OK) ? "(controller + NimBLE host up; left ENABLED for later phases)"
	    : (ble_rc == ALP_ERR_NOT_READY)
	        ? "(NOT_READY = BLE is not built into this firmware -- every shipped SoM carries it, "
	          "so this is a real deviation, not an absent feature)"
	        : "");

	/* --- Verdict ------------------------------------------------------ */
	/*
	 * A PING is NOT enough. It proves the transport and nothing else, and
	 * "the link answered" is the same shape of claim as "the chip ID read
	 * back" -- which is the bug this app was built around. PASS additionally
	 * requires ALL of:
	 *
	 *   ver_ok   -- the firmware agrees on the MAJOR wire contract, so every
	 *               other reply below was parsed against the right layout. A
	 *               MINOR delta is additive and deliberately not gated.
	 *   mac_ok   -- the radio produced its own identity, that identity is
	 *               structurally a station MAC rather than a zeroed field,
	 *               and a second GET_MAC round-trip returned the same bytes.
	 *               The repeat is what keeps a corrupting link from passing
	 *               on a structurally perfect but bit-flipped address.
	 *   caps_rc  -- the firmware could state what it implements.
	 *   scan_ok  -- a real radio operation ran to completion on the Wi-Fi
	 *               side and its reply parsed (see the empty-scan note).
	 *   ble_rc   -- the other radio came up.
	 *
	 * Four of those five are radio-side, and three of them (MAC, scan, BLE)
	 * are worker-routed rather than answered from the SPI ISR, so they
	 * cannot be satisfied by a coprocessor that is merely running its
	 * dispatch loop with dead radios.
	 */
	bool pass = ver_ok && mac_ok && (caps_rc == ALP_OK) && scan_ok && (ble_rc == ALP_OK);
	/* ping is always "ok" on this line -- a failed PING returned above -- and
	 * it is printed anyway so the gate list in the log is the complete one. */
	printf("[evkdemo] CC3501E: ping=ok version=%s mac=%s caps=%s scan=%s ble=%s -> %s\n",
	       ver_ok ? "ok" : "BAD",
	       mac_ok ? "ok" : "BAD",
	       (caps_rc == ALP_OK) ? "ok" : "BAD",
	       scan_ok ? "ok" : "BAD",
	       (ble_rc == ALP_OK) ? "ok" : "BAD",
	       pass ? "PASS" : "FAIL");

	/* Carry the empty-scan qualifier into the SUMMARY TABLE, not just this
	 * line. The table is what most readers read, and a bare PASS there cannot
	 * be told apart from a run that actually saw networks. */
	if (pass && (n_scan == 0u)) {
		ctx->note = "scan UNCORROBORATED -- 0 networks seen";
	}
	return pass ? PHASE_PASS : PHASE_FAIL;
}

/* ==================================================================== */
/* Phase 9 -- SD card (74LVC157 SDIO mux -> DWC SDHC -> FAT round trip) */
/* ==================================================================== */

/*
 * WHAT HAS TO BE TRUE BEFORE THIS PHASE CAN DO ANYTHING
 * ------------------------------------------------------
 * The EVK microSD is not wired straight to the SoC. It sits behind a pair of
 * 74LVC157 muxes with two controls, and BOTH have to be in the right state
 * before the SD lines reach the card at all:
 *
 *   ENABLE -- E1M IO20 -> CC3501E GPIO_26 on BOTH module revisions, so it is
 *             drivable in software, over the coprocessor's GPIO proxy. The
 *             part is ACTIVE LOW (`/E`): drive it LOW to enable the mux.
 *             That is what step 1 below does.
 *   SELECT -- E1M IO21. NOT drivable in software on this bench. On r1 it
 *             reached CC3501E GPIO_30; on r2 GPIO_30 was re-routed to IO8 and
 *             IO21 was left OPEN on the module -- it reaches neither chip
 *             (metadata/e1m_modules/aen/hw-revisions.yaml
 *             `pad_route_overrides:`). The bench module is hw_rev 2626-r2.
 *
 * The SELECT is set BY HAND, on the carrier, and this phase cannot read it
 * back. From the 2626-R2 EVK netlist: header P18 pin 1 is +3V3, pin 2 is
 * `NetP18_2`, which reaches `MUX_SEL.SDIO` through R198 while R27 pulls that
 * net to 0V when the header is open. So a FITTED jumper pulls the select
 * high, an open header lets it sit low, and no firmware is involved either
 * way. The same net drives both mux select inputs (U38 pin 1 `S`, U39 pin 1
 * `S`) and also lands on E2 `L3` = IO21. The jumper is fitted on the bench
 * this phase was written against (maintainer, 2026-09-09).
 *
 * CONTENTION WARNING, and it matters on r1 BOARDS ONLY. Because
 * `MUX_SEL.SDIO` reaches BOTH the P18 header and E2 IO21, an r1 module that
 * drives IO21 from firmware while a jumper is fitted on P18 puts a driven pin
 * against the header rail. FIT THE JUMPER OR DRIVE THE PIN, NEVER BOTH. On r2
 * the module end is open, so the header is the only driver and there is
 * nothing to contend with -- which is why this phase never touches IO21, on
 * any revision: there is no revision on which driving it is both useful and
 * safe.
 *
 * PHASE 8 MUST HAVE RUN, and the phase table already guarantees that. The
 * ENABLE rides the CC3501E GPIO proxy, and the proxy only routes a pin once
 * cc3501e_bridge_bringup() has powered the coprocessor, reset it, and called
 * alp_gpio_cc3501e_attach() -- which phase 8 does, and deliberately does NOT
 * undo (see its "WHAT IT LEAVES BEHIND" note). This phase therefore re-uses
 * the live bridge rather than re-initialising it; a second bring-up would
 * re-run the ~900 ms power/reset sequence and drop the link phase 8 proved.
 * With no attached bridge the proxy DELEGATES IO20 to the platform GPIO
 * driver instead of refusing it -- so a phase 8 that failed does not produce
 * a clean error here, it produces a write to an Alif pad that goes nowhere
 * and then a card that never enumerates. The log below prints the ENABLE
 * result on its own line so that case is at least visible.
 *
 * WHAT COUNTS AS A PASS
 * ----------------------
 * A full write -> read -> VERIFY round trip, and nothing weaker. This app
 * exists because an earlier example counted a successful ID read as a pass;
 * `disk_access_init` returning 0, or geometry reading back, is that same
 * claim wearing an SD-shaped hat. Data has to move, come back, and compare
 * equal. Geometry is printed because it is useful, not because it is gated.
 *
 * WHAT THIS PHASE IS NOT ALLOWED TO DO -- A CARD IN THE SLOT IS SOMEONE'S
 * -----------------------------------------------------------------------
 * It writes exactly ONE file it owns, /ALPDEMO.TXT, and touches nothing else:
 * no partition table, no other file, and NO FORMATTING, ever. That is
 * enforced twice over, at two different layers, because one layer's mistake
 * would silently destroy a stranger's card:
 *
 *   - FS_MOUNT_FLAG_NO_FORMAT on the mount below, so fs_mount() reports a
 *     missing filesystem instead of creating one;
 *   - CONFIG_FS_FATFS_MOUNT_MKFS=n in prj.conf, which leaves the mkfs code
 *     OUT OF THE IMAGE entirely -- there is no format path to reach even by
 *     mistake, from this phase or any other.
 *
 * A card with no filesystem is a SKIPPED with that reason. It is not an
 * invitation to make one.
 *
 * THE FIVE OUTCOMES, KEPT APART ON PURPOSE
 * -----------------------------------------
 * Collapsing these into one line would send the next person to the wrong
 * place, so each gets its own verdict and its own printed reason:
 *
 *   mux ENABLE could not be driven   -> FAIL. IO20 is routed on BOTH module
 *        revisions and phase 8 left the bridge up, so nothing about this
 *        board justifies the ENABLE failing. Points at the bridge or the
 *        proxy route table, not at the card.
 *   no card detected                 -> SKIPPED. disk_access_init() answers
 *        DISK_STATUS_NOMEDIA, straight from the controller's PSTATE
 *        CARD_INSRT bit. An empty slot is not a fault -- but note the mux
 *        sits between the card and that bit, so a wrong P18 jumper position
 *        also lands here. The log says so.
 *   controller failed to init        -> FAIL. Any other non-zero from
 *        disk_access_init(): the card is detected and the SD handshake still
 *        did not complete. Our controller, pinmux or clocking.
 *   card present but no filesystem   -> SKIPPED. fs_mount() answers -ENODEV,
 *        which subsys/fs/fat_fs.c maps from THREE distinct ff.h causes alike
 *        -- FR_INVALID_DRIVE, FR_NOT_ENABLED and FR_NO_FILESYSTEM -- so this
 *        SKIP is also where this app's own disk-name/mount-point wiring
 *        drifting would land, indistinguishably. See the no-format rule
 *        above.
 *   write or verify mismatch         -> FAIL. The card enumerated and the
 *        filesystem mounted, and the data still did not survive the trip.
 *
 * HOW A STALE FILE IS MADE IMPOSSIBLE TO MISTAKE FOR A FRESH WRITE
 * -----------------------------------------------------------------
 * /ALPDEMO.TXT is left on the card after the run, so the NEXT run opens a
 * file that already has a plausible-looking payload in it. If the payload
 * were fixed text, a write that silently did nothing would still read back
 * byte-identical and pass -- the file from last time would be indistinguish-
 * able from a fresh one, which is exactly the "it looked right" failure this
 * app refuses everywhere else.
 *
 * So the payload carries a PER-RUN NONCE: k_cycle_get_32() sampled at write
 * time, printed in the log and embedded in the line. The verify compares the
 * read-back bytes against the buffer THIS run built in RAM, so last run's
 * file fails the compare on the nonce alone. A cycle counter sampled this
 * deep into a run that has already spent DHCP and scan timeouts is not
 * something a previous run reproduces. The file is also truncated to the
 * bytes just written, so a longer leftover cannot leave a matching prefix
 * with stale tail bytes hiding behind it.
 */

#define SD_DISK_NAME   "SD"
#define SD_MOUNT_POINT "/SD:"
/* 8.3 name on purpose: CONFIG_FS_FATFS_LFN is off (it costs image for
 * nothing here), so a long name would be rejected by the filesystem. */
#define SD_DEMO_FILE SD_MOUNT_POINT "/ALPDEMO.TXT"

/* The mux is a 74LVC157 -- combinational, ns-scale. This is settle time for
 * the CC3501E driving its pad and the card seeing its lines, not for the mux
 * itself; it costs 10 ms once and removes a whole class of "the first
 * enumeration attempt raced the mux" run.
 *
 * Guarded, not a bare define: MUX_EN (E1M IO20 -> CC3501E GPIO_26) has no
 * pull and is not observable from the SoC side at all, so the only way to
 * confirm the pad is actually low is a multimeter on U38 pin 15 / U39 pin 15
 * against 0V -- and 10 ms cannot be caught by hand. A bench build overrides
 * this with -DSD_MUX_SETTLE_MS=600000 to hold /E asserted long enough to
 * meter it; the default here is unchanged for a normal run. */
#ifndef SD_MUX_SETTLE_MS
#define SD_MUX_SETTLE_MS 10u
#endif

/* FATFS work area. FILE-STATIC, not a local: fs_mount() keeps the pointer for
 * as long as the volume is mounted, so a stack-local would be dangling the
 * moment this function returned -- and sizeof(FATFS) is a few hundred bytes
 * against a 4096-byte main stack. */
static FATFS sd_fat_fs;

static struct fs_mount_t sd_mnt = {
	.type      = FS_FATFS,
	.fs_data   = &sd_fat_fs,
	.mnt_point = SD_MOUNT_POINT,
	/* Half of the two-layer no-format guarantee; the other half is
	 * CONFIG_FS_FATFS_MOUNT_MKFS=n. See the header above. */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

static phase_verdict_t phase_sdcard(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: SD card (74LVC157 SDIO mux -> DWC SDHC -> FAT) --\n");

	/* --- 1. Enable the SDIO mux over the CC3501E GPIO proxy ------------ */
	/* alp_gpio_open() on a PORTABLE E1M pin id. The proxy backend looks
	 * IO20 up in this app's cc3501e_gpio_routes[] table, finds raw CC3501E
	 * GPIO_26, and sends the configure/write over the bridge phase 8 left
	 * bound. Nothing here names GPIO_26 -- the raw index belongs in the
	 * route table, which is derived from the SoM pad map, not in app code. */
	alp_gpio_t *mux_en = alp_gpio_open(ALP_E1M_GPIO_IO20);
	if (mux_en == NULL) {
		printf("[evkdemo] SD: alp_gpio_open(E1M IO20 = SDIO mux /E) -> NULL, err=%d -- the mux "
		       "cannot be enabled, so the card is electrically disconnected. IO20 reaches "
		       "CC3501E GPIO_26 on BOTH module revisions and phase 8 leaves the bridge up, so "
		       "this is a build/bridge fault, not an absent card\n",
		       (int)alp_last_error());
		ctx->note = "mux ENABLE not drivable";
		return PHASE_FAIL;
	}
	alp_status_t cfg_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	/* ACTIVE LOW: `false` asserts /E and connects the card to the SoC. */
	alp_status_t en_rc = (cfg_rc == ALP_OK) ? alp_gpio_write(mux_en, false) : cfg_rc;
	/* Read the pin back rather than trusting the write return code alone --
	 * the GPIO proxy's read path is reachable over the same bridge phase 8
	 * left up (route-table detail, not named here -- see the open() comment
	 * above). A LOW read-back is only CORROBORATION that the pad is
	 * asserted, not proof: depending on bridge firmware this may report
	 * the far-side output register rather than the pad itself. It does not
	 * gate anything below -- disagreement is printed and the run falls
	 * through to disk_access_init() either way, so the log still shows
	 * what the controller sees. */
	bool         mux_level = true;
	alp_status_t rd_rc     = alp_gpio_read(mux_en, &mux_level);
	/* level=? rather than a fabricated sample on a failed read: mux_level
	 * is seeded true and alp_gpio_read()/cc3501e_gpio_read() leaves the
	 * output untouched on every error path, so printing it unconditionally
	 * would make a failed read print "level=HIGH" -- indistinguishable
	 * from a genuine high reading, in the one field whose entire purpose
	 * is answering whether the pad actually moved. */
	const char *level_str = (rd_rc == ALP_OK) ? (mux_level ? "HIGH" : "LOW") : "?";
	printf("[evkdemo] SD: mux ENABLE via GPIO proxy (E1M IO20 -> CC3501E GPIO_26, /E active "
	       "low, driven LOW): configure -> %d, write -> %d, read-back -> %d (level=%s, "
	       "corroboration only -- may reflect the bridge's output register rather than the "
	       "pad, not proof the line moved), settle=%u ms\n",
	       (int)cfg_rc,
	       (int)en_rc,
	       (int)rd_rc,
	       level_str,
	       (unsigned)SD_MUX_SETTLE_MS);
	if (en_rc != ALP_OK) {
		printf("[evkdemo] SD: the mux ENABLE could not be driven -- every step below would run "
		       "against a card that is not connected to the SoC. Check that phase 8 passed "
		       "(the proxy needs its bridge attached) and that this build carries the IO20 "
		       "route\n");
		/* Restore idle before closing, same as the single exit below --
		 * cfg_rc == ALP_OK means the pin WAS configured as an output, so
		 * skipping this would leave GPIO_26 in whatever state the failed
		 * write left it at, for the rest of the run. Ignoring the return
		 * matches the main exit: best-effort restore on an already-
		 * failing path, not a second gate. */
		(void)alp_gpio_write(mux_en, true);
		alp_gpio_close(mux_en);
		ctx->note = "mux ENABLE not drivable";
		return PHASE_FAIL;
	}
	k_msleep(SD_MUX_SETTLE_MS);

	/* From here on the mux stays ENABLED (GPIO_26 driven low) until this
	 * function's single exit below, which de-asserts it and only then
	 * closes the handle -- the same restore-to-idle-before-close idiom
	 * phase 6 (RGB LED) uses for its PWM channels. Every step from here on
	 * reports through `verdict` / `ctx->note` instead of returning
	 * directly, so GPIO_26 never gets left driving the mux past this
	 * function on ANY of the five outcomes -- PASS, FAIL or SKIPPED. */
	phase_verdict_t verdict = PHASE_FAIL;

	/* --- 2. Enumerate the card ---------------------------------------- */
	/* disk_access_init() runs the whole SD initialisation on the vendored
	 * snps,dwc-sdhc controller: card-present, power/clock ramp, CMD0/CMD8,
	 * ACMD41, CID/CSD, bus width. Its return value is the fork between two
	 * of the five outcomes and is therefore read, not just printed. */
	int drc = disk_access_init(SD_DISK_NAME);
	printf("[evkdemo] SD: disk_access_init(\"%s\") -> %d\n", SD_DISK_NAME, drc);
	if (drc == DISK_STATUS_NOMEDIA) {
		/* Straight from the controller's PSTATE CARD_INSRT bit (no cd-gpios
		 * on this overlay). The mux sits between the card and that bit, so
		 * say both causes -- an operator who only reads "no card" will not
		 * think to check a jumper. */
		printf("[evkdemo] SD: NOMEDIA -- the controller sees no card. Either the slot is empty, "
		       "or the 74LVC157 mux is not passing the card through: the SELECT is header P18 "
		       "(jumper = MUX_SEL.SDIO pulled high through R198, open = R27 pulls it to 0V) and "
		       "is NOT software-drivable on this module -- E1M IO21 is unrouted on r2. An empty "
		       "slot is not a fault, so this is a SKIP\n");
		ctx->note = "no card detected (or mux SELECT on P18 wrong)";
		verdict   = PHASE_SKIPPED;
	} else if (drc != 0) {
		printf("[evkdemo] SD: the card IS detected and the SD handshake still failed (rc=%d) -- "
		       "that is the controller, the pinmux (CLK P4_1 / CMD P4_2 / D0..D3 P6_0..P6_3) or "
		       "the clock ramp, NOT a missing card. Not a skip\n",
		       drc);
		ctx->note = "SDHC init failed with a card present";
		verdict   = PHASE_FAIL;
	} else {
		/* Geometry, printed VERBATIM -- reported because it is the first
		 * real data the card ever hands back and it identifies which card
		 * is in the slot, but deliberately NOT part of the verdict. "The
		 * geometry read back" is a chip-ID read by another name. */
		uint32_t sectors = 0u, ssize = 0u;
		int      sc_rc = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
		int      ss_rc = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &ssize);
		printf("[evkdemo] SD: geometry: %u sectors x %u B = %llu MiB (ioctl rc %d / %d)\n",
		       (unsigned)sectors,
		       (unsigned)ssize,
		       (unsigned long long)(((uint64_t)sectors * ssize) / (1024u * 1024u)),
		       sc_rc,
		       ss_rc);

		/* --- 3. Mount, read-only-until-proven -------------------------- */
		int mrc = fs_mount(&sd_mnt);
		printf("[evkdemo] SD: fs_mount(FAT, \"%s\", NO_FORMAT) -> %d\n", SD_MOUNT_POINT, mrc);
		if (mrc == -ENODEV) {
			/* subsys/fs/fat_fs.c maps THREE distinct ff.h causes onto
			 * -ENODEV alike: FR_INVALID_DRIVE, FR_NOT_ENABLED and
			 * FR_NO_FILESYSTEM. disk_access_init(SD_DISK_NAME) already
			 * returned 0 above, which proves the "SD" disk itself is
			 * registered -- so SD_DISK_NAME is cleared, not an open
			 * suspect. What -ENODEV still cannot separate is "no FAT
			 * volume on the card" from SD_MOUNT_POINT not matching this
			 * build's FF_VOLUME_STRS entry, so the log below states only
			 * what -ENODEV actually proves. */
			printf("[evkdemo] SD: fs_mount -> -ENODEV -- subsys/fs/fat_fs.c maps three FatFs "
			       "causes (FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM) onto the same "
			       "code: either no FAT filesystem on this card, or SD_MOUNT_POINT=\"%s\" does "
			       "not match this build's FF_VOLUME_STRS entry. disk_access_init(\"%s\") "
			       "already returned 0 above, which rules out the disk name -- only the "
			       "mount-point volume string is still an open question. NOT formatted here "
			       "regardless, on purpose -- the card belongs to whoever put it in the slot. "
			       "FS_MOUNT_FLAG_NO_FORMAT is set AND CONFIG_FS_FATFS_MOUNT_MKFS=n keeps mkfs "
			       "out of the image entirely, so there is no format path to take even by "
			       "mistake\n",
			       SD_MOUNT_POINT,
			       SD_DISK_NAME);
			ctx->note = "no filesystem, or a mount-point/FF_VOLUME_STRS mismatch -- see log";
			verdict   = PHASE_SKIPPED;
		} else if (mrc != 0) {
			printf("[evkdemo] SD: mount failed with a card that enumerated (rc=%d) -- a read of "
			       "the boot sector did not complete, so this is the data path, not a missing "
			       "volume\n",
			       mrc);
			ctx->note = "mount failed on an enumerated card";
			verdict   = PHASE_FAIL;
		} else {
			/* --- 4. Write -> read -> VERIFY, in one file this demo owns */
			/* The nonce. k_cycle_get_32() at this point in the run -- after
			 * the bridge bring-up, a Wi-Fi scan and a BLE enable, all of
			 * which take wall-clock time that varies -- is not a value a
			 * previous run reproduces, which is exactly the property
			 * needed to tell a fresh write from last run's leftover file. */
			uint32_t nonce = k_cycle_get_32();
			char     payload[96];
			int      len = snprintk(payload,
			                        sizeof(payload),
			                        "aen-evk-demo phase 9 nonce=%08x uptime=%lldms\n",
			                        (unsigned)nonce,
			                        (long long)k_uptime_get());
			printf("[evkdemo] SD: writing %d B to %s, nonce=%08x (per-run, so a stale file from "
			       "an earlier run cannot compare equal)\n",
			       len,
			       SD_DEMO_FILE,
			       (unsigned)nonce);

			struct fs_file_t f;
			fs_file_t_init(&f);
			const char *fail_why = NULL;

			/* FS_O_CREATE|FS_O_RDWR, NOT a create-exclusive open: this
			 * file is the demo's own and every run rewrites it in place. */
			int frc = fs_open(&f, SD_DEMO_FILE, FS_O_CREATE | FS_O_RDWR);
			if (frc != 0) {
				printf("[evkdemo] SD: fs_open(\"%s\", CREATE|RDWR) -> %d\n", SD_DEMO_FILE, frc);
				fail_why = "cannot open the demo file";
			} else {
				ssize_t wrote = fs_write(&f, payload, (size_t)len);
				/* Truncate ONLY when fs_write() actually landed bytes
				 * (wrote > 0) -- never on a hard error (wrote < 0). A
				 * hard write error can leave the file in an
				 * indeterminate state; truncating anyway still frees
				 * the FAT chain and rewrites the dirent, on exactly the
				 * path that just failed a write -- destroying the one
				 * artifact (the previous run's leftover file) that
				 * would show what happened. A SHORT write (0 < wrote <
				 * len) still truncates, to wherever it actually landed:
				 * that is the case this call exists for, shedding an
				 * earlier run's longer tail rather than leaving a
				 * partial payload followed by stale bytes. */
				bool  did_truncate = (wrote > 0);
				off_t trunc_len    = did_truncate ? (off_t)wrote : 0;
				int   trc          = did_truncate ? fs_truncate(&f, trunc_len) : 0;
				/* SYNC BEFORE READ-BACK, and this is load-bearing. Without
				 * it the read below could be served out of the FATFS
				 * cache and would "verify" data that never reached the
				 * card -- the exact shape of false pass this app exists
				 * to refuse. Only run once the write was full AND the
				 * truncate actually ran and returned 0 -- a skipped or
				 * failed truncate is not a clean state to sync from
				 * either. */
				bool did_sync = (wrote == (ssize_t)len && did_truncate && trc == 0);
				int  syrc     = did_sync ? fs_sync(&f) : 0;

				/* A skipped call must not print the same as a successful
				 * one -- "-> 0" either way would be indistinguishable
				 * from a real pass in a phase whose whole thesis is that
				 * every return code is printed. */
				char trc_field[16];
				char syrc_field[16];
				if (did_truncate) {
					snprintk(trc_field, sizeof(trc_field), "%d", trc);
				} else {
					snprintk(trc_field, sizeof(trc_field), "SKIPPED");
				}
				if (did_sync) {
					snprintk(syrc_field, sizeof(syrc_field), "%d", syrc);
				} else {
					snprintk(syrc_field, sizeof(syrc_field), "SKIPPED");
				}
				printf("[evkdemo] SD: fs_write -> %d of %d B, fs_truncate(%ld) -> %s, "
				       "fs_sync -> %s\n",
				       (int)wrote,
				       len,
				       (long)trunc_len,
				       trc_field,
				       syrc_field);

				char    readback[sizeof(payload)] = { 0 };
				ssize_t got                       = -1;
				int     seek_rc                   = -1;
				if (wrote == (ssize_t)len && trc == 0 && syrc == 0) {
					seek_rc = fs_seek(&f, 0, FS_SEEK_SET);
					if (seek_rc == 0) {
						got = fs_read(&f, readback, sizeof(readback));
					}
				}
				int crc = fs_close(&f);
				printf("[evkdemo] SD: fs_close -> %d\n", crc);

				if (wrote != (ssize_t)len) {
					fail_why = "short write";
				} else if (trc != 0 || syrc != 0) {
					fail_why = "truncate/sync failed";
				} else if (seek_rc != 0 || got != (ssize_t)len) {
					printf("[evkdemo] SD: fs_seek -> %d, fs_read -> %d (expected %d B)\n",
					       seek_rc,
					       (int)got,
					       len);
					fail_why = "short read-back";
				} else if (memcmp(payload, readback, (size_t)len) != 0) {
					/* Print both, not a verdict word: WHICH bytes differ
					 * is the whole diagnostic and a bare "mismatch"
					 * throws it away. */
					printf("[evkdemo] SD: VERIFY MISMATCH\n[evkdemo] SD:   wrote: %.*s"
					       "[evkdemo] SD:   read : %.*s",
					       len,
					       payload,
					       len,
					       readback);
					fail_why = "read-back differs from what was written";
				} else {
					printf("[evkdemo] SD: read back %d B and they COMPARE EQUAL: %.*s",
					       (int)got,
					       len,
					       readback);
					verdict = PHASE_PASS;
				}
			}

			/* Unmount either way -- a mounted volume with dirty FAT cache
			 * left behind by a failing phase is how a card gets corrupted
			 * for the next person, and phases 10-14 run after this one. */
			int urc = fs_unmount(&sd_mnt);
			printf("[evkdemo] SD: fs_unmount -> %d\n", urc);

			if (verdict == PHASE_PASS && urc != 0) {
				/* The write/read/verify round trip proved the data path,
				 * but a failed unmount leaves this volume mounted in
				 * FATFS's own view while the mux de-assert below
				 * electrically disconnects the card out from under it --
				 * that is not a PASS. */
				fail_why = "fs_unmount failed after a passing verify";
				verdict  = PHASE_FAIL;
			}

			if (verdict != PHASE_PASS) {
				printf("[evkdemo] SD: RESULT FAIL -- %s. The card enumerated and the "
				       "filesystem mounted, so this is the data path, not the slot and not "
				       "the mux\n",
				       fail_why);
				ctx->note = fail_why;
				verdict   = PHASE_FAIL;
			} else {
				printf("[evkdemo] SD: mux enabled, card enumerated, FAT mounted, %d B "
				       "written -> read -> compared equal -> PASS\n",
				       len);
			}
		}
	}

	/* --- 5. Restore the mux to idle before returning --------------------
	 * De-assert ENABLE (drive it back HIGH) and only then close the
	 * handle, on every one of the five outcomes above -- matching phase
	 * 6's restore-to-idle-before-close idiom. Without this, GPIO_26 stays
	 * driven low through phases 10-14 and past the end of the run. */
	(void)alp_gpio_write(mux_en, true);
	alp_gpio_close(mux_en);
	return verdict;
}

/* ==================================================================== */
/* Phase 10 -- Ethernet (RMII GMAC + on-module TI DP83825 PHY)          */
/* ==================================================================== */

/*
 * Grouped with the other IMPLEMENTED phases though it runs tenth, same as
 * phase 13 above -- this file's organising principle is "real phases first,
 * stubs after".
 *
 * WHAT THIS PHASE WILL AND WILL NOT COUNT AS A PASS
 * --------------------------------------------------
 * It gates on a DHCPv4 LEASE, and on nothing weaker. That choice is the whole
 * point of the phase. The obvious cheaper gates are all lies of the exact
 * shape this app exists to refuse:
 *
 *   - "the MAC initialised" is the chip-ID read all over again;
 *   - net_if_is_carrier_ok() is worse, because it LOOKS like a link check. The
 *     PHY here is unmanaged (a fixed-link DT child, no Zephyr MDIO bus), so
 *     carrier is SYNTHETIC: it reports what devicetree hard-codes, not what is
 *     on the cable. It reads true with the cable in your hand.
 *
 * A lease is unforgeable by comparison: DISCOVER -> OFFER -> REQUEST -> ACK
 * completes only over a genuinely bidirectional link with a server on it. It
 * is also what caught the bug this app's overlay is shaped around -- with the
 * DMA buffers in DTCM the wire link came up and NOTHING moved.
 *
 * NO CABLE IS NOT A FAILURE, AND THAT DISTINCTION IS THE HARD PART
 * ----------------------------------------------------------------
 * A lease needs a switch with a DHCP server on the far end, which no bench run
 * can be assumed to have. So the phase separates the cases by asking the PHY
 * itself, over MDIO, what the wire is doing -- and then by asking the
 * interface's own byte counters what actually moved:
 *
 *   PHY silent on MDIO (no ID at any of the 32 addresses)  -> FAIL
 *         The DP83825 is fitted on every E1M-AEN SoM. If it does not answer
 *         its management interface it is unpowered, unclocked or unreset --
 *         our hardware, not the operator's cable.
 *   PHY answers, auto-neg never completes                  -> SKIPPED
 *         Nobody on the other end. This is "is a cable plugged in?", the
 *         normal state of an unattended bench, and it is NOT a failure.
 *   Link up but the MAC transmitted ZERO bytes             -> FAIL
 *         We know DHCP queued DISCOVERs, so a zero TX count means no frame
 *         ever left the part regardless of what is out there. Unambiguous.
 *   Link up, TX moved, no lease                            -> SKIPPED
 *         Either no DHCP server on this segment or a dead RX path; the two
 *         are told apart by the printed rx_bytes, and the verdict carries a
 *         qualifier into the summary table so a reader of the table alone
 *         still sees which. Left as SKIPPED rather than FAIL because a live
 *         switch port with no other talkers legitimately sends us nothing --
 *         failing on that would be the mirror-image lie.
 *   Link up + lease                                        -> PASS
 *
 * The reference for every register value and every ordering constraint below
 * is examples/aen/aen-ethernet-link, which is BENCH-VERIFIED on this silicon
 * (a real lease off the bench switch, server-side reachable). Read its header
 * before changing anything here.
 */

#define PHY_RESET_PIN    6 /* E_PHY_RESET  = P11_6 (gpio11), SoM TSV */
#define PHY_PWRDWN_PIN   4 /* E_PHY_PWRDWN = P15_4 (lpgpio), SoM TSV */
#define ETH_LINK_POLL_MS 250
#define ETH_LINK_POLLS   32 /* 32 x 250 ms = 8 s for auto-negotiation */
#define ETH_DHCP_POLL_MS 500
/* 30 x 500 ms = 15 s of WALL CLOCK, which is not the same as 15 s of DHCP: the
 * client first waits a random RFC 2131 4.4.1 interval capped by
 * CONFIG_NET_DHCPV4_INITIAL_DELAY_MAX. prj.conf pins that cap to its Kconfig
 * minimum of 2 s for exactly this reason -- at the Zephyr default of 10 s the
 * real DISCOVER..ACK budget would be 5 s, and a good segment could report "no
 * DHCP server" purely on timing. */
#define ETH_DHCP_POLLS 30

/* Pad-mux states for the two PHY control lines. gpio_dw applies no mux of its
 * own, so the GPIO/LPGPIO function (function 0) has to be selected through the
 * Alif pinctrl driver before the pins will do anything. */
static const pinctrl_soc_pin_t eth_phy_reset_mux[]  = { PIN_P11_6__GPIO };
static const pinctrl_soc_pin_t eth_phy_pwrdwn_mux[] = { PIN_P15_4__LPGPIO };

/*
 * THE PHY IS POWERED HERE, BEFORE main(), AND THE ORDERING IS THE WHOLE TRICK.
 *
 * The Ethernet glue's RMII reference-clock AUTO probe runs inside the eth
 * driver's init and picks between the on-module external 50 MHz oscillator and
 * an internal PLL fallback -- by looking for the external clock AT THAT
 * MOMENT. The oscillator is downstream of this power enable. Power the PHY
 * late (from the phase function, in main()) and the probe has already fallen
 * back before the phase runs: bench-observed as ETH_CTRL bit4 = 1.
 *
 * So this is a SYS_INIT at POST_KERNEL priority 50, which lands in the only
 * window that works. Both neighbours were read out of the produced artefacts,
 * not assumed: gpio_dw initialises at PRE_KERNEL_1 priority 40 -- an EARLIER
 * LEVEL, not merely a lower number, so the controllers are up with margin --
 * and eth_dwmac at POST_KERNEL CONFIG_ETH_INIT_PRIORITY = 60, after us, so its
 * ref-clock probe sees a PHY that is already powered.
 *
 * It runs on every boot, including runs where phase 10 is never reached --
 * unavoidable, and harmless: it touches two pads (P15_4, P11_6) that nothing
 * else in this app or its board layer claims.
 */
static int eth_phy_power_init(void)
{
	const struct device *gpio11 = DEVICE_DT_GET(DT_NODELABEL(gpio11));
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	if (!device_is_ready(gpio11) || !device_is_ready(lpgpio)) {
		return -ENODEV;
	}

	/*
	 * EVERY rc below is propagated, and that is worth a line of explanation.
	 * The phase's own failure message for a dead PHY points the operator at
	 * hardware ("check E_PHY_PWRDWN drove high, E_PHY_RESET released, the
	 * MDC/MDIO pinmux"). If a mux or a pin configure had quietly failed here,
	 * that message would be aiming a bench session at a board fault that does
	 * not exist. A non-zero return from a SYS_INIT hook is reported by the
	 * kernel with this function's name, so the boot log names the real cause.
	 */
	int rc = pinctrl_configure_pins(eth_phy_reset_mux, ARRAY_SIZE(eth_phy_reset_mux), 0U);
	if (rc != 0) {
		return rc;
	}
	rc = pinctrl_configure_pins(eth_phy_pwrdwn_mux, ARRAY_SIZE(eth_phy_pwrdwn_mux), 0U);
	if (rc != 0) {
		return rc;
	}

	/* E_PHY_PWRDWN gates a board power switch rather than the PHY's own
	 * power-down input on this module -- the "needs a power enable" the bench
	 * called out IS this pin -- so it is driven HIGH to turn the supply on.
	 * The SoM TSV gives the pad, not the polarity; both were bench-tried and
	 * this is the one that produced a clocked PHY. */
	rc = gpio_pin_configure(lpgpio, PHY_PWRDWN_PIN, GPIO_OUTPUT_ACTIVE);
	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_set(lpgpio, PHY_PWRDWN_PIN, 1);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(50000); /* let the supply and its reference clock settle */

	/* Conventional DP83825 RST_N (active-low): LOW asserts, HIGH releases.
	 * Long assert plus a long post-reset settle so the reference clock is
	 * stable ACROSS the reset -- a clock that is not stable at deassert leaves
	 * the analog front-end uninitialised, which presents as a PHY that answers
	 * MDIO but never links. */
	rc = gpio_pin_configure(gpio11, PHY_RESET_PIN, GPIO_OUTPUT_ACTIVE);
	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_set(gpio11, PHY_RESET_PIN, 0);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(50000);
	rc = gpio_pin_set(gpio11, PHY_RESET_PIN, 1);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(100000); /* DP83825 post-reset settle, >= 50 ms */
	return 0;
}
SYS_INIT(eth_phy_power_init, POST_KERNEL, 50);

/*
 * Raw MDIO through the DWMAC MAC_MDIO registers. The fixed-link configuration
 * performs no MDIO of its own, so this is how the phase reads what the WIRE is
 * doing rather than what devicetree claims -- see the carrier_ok note above.
 *
 * MAC_MDIO_ADDRESS = GMAC base + 0x200: PA[25:21] = PHY address,
 * RDA[20:16] = register, CR[11:8] = MDC divider, GOC = bits 3..2 (read = both
 * set, write = bit 2 only), GB = bit 0 = busy. MAC_MDIO_DATA = base + 0x204,
 * data in [15:0]. CR = 4 selects a slow, safe MDC. Every value here is
 * transcribed from aen-ethernet-link, not invented.
 *
 * This is the same "hand-rolled register poke" phase 13 above declines to do
 * for the Hantro hardware ID, and the difference is not a double standard: for
 * the JPEG block the portable API's own error path already surfaces the fact
 * (a bad ID makes alp_jpeg_open() return NULL with ALP_ERR_NOT_READY), so the
 * poke would add nothing. Here there is NO other source of the answer at all
 * -- the only alternative reading, carrier_ok, is the synthetic one.
 *
 * The BASE comes from devicetree rather than a literal, so this app holds one
 * copy of the GMAC address, not a second one that can drift from the node it
 * is talking to. (The ETH_CTRL register read further down still IS a literal:
 * the driver's ALIF_ETH_CTRL_REG is a private #define in its .c with no header
 * to include, so there is nothing to reuse. Noted rather than papered over --
 * that one is genuinely a second copy.)
 */
#define GMAC_BASE      DT_REG_ADDR(DT_NODELABEL(ethernet))
#define GMAC_MDIO_ADDR (GMAC_BASE + 0x200U)
#define GMAC_MDIO_DATA (GMAC_BASE + 0x204U)

/* Bounded spin on MAC_MDIO_ADDRESS.GB (bit 0 = busy). Returns false if the bit
 * never cleared.
 *
 * BOUNDED ON PURPOSE, and this is the one place it differs from the standalone
 * reference: aen-ethernet-link spins on GB unbounded before each transaction,
 * which is survivable in an app that does nothing else. Here it is phase 10 of
 * 14 -- a stuck GB bit would eat phases 11 to 14, the summary table and the
 * RESULT line, so a hardware fault would present as a demo that prints nothing
 * more, which is the least diagnosable outcome available. */
static bool eth_mdio_wait_idle(void)
{
	for (int i = 0; i < 100000; i++) {
		if ((sys_read32(GMAC_MDIO_ADDR) & BIT(0)) == 0U) {
			return true;
		}
		k_busy_wait(1);
	}
	printf("[evkdemo] ETH: MDIO busy bit stuck -- the MAC is not answering its management "
	       "interface\n");
	return false;
}

/* Returns the register value, or 0xFFFF if the bus never went idle -- which
 * eth_phy_find() already treats as "no device here", so a wedged bus degrades
 * into a clean "no PHY answered" FAIL instead of a hang. */
static uint16_t eth_mdio_read(uint8_t phy, uint8_t reg)
{
	if (!eth_mdio_wait_idle()) {
		return 0xFFFFU;
	}
	uint32_t a =
	    ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(3) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)); i++) {
		k_busy_wait(1);
	}
	return (uint16_t)(sys_read32(GMAC_MDIO_DATA) & 0xFFFFU);
}

static void eth_mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
	if (!eth_mdio_wait_idle()) {
		return;
	}
	sys_write32(val, GMAC_MDIO_DATA);
	uint32_t a = ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)); i++) {
		k_busy_wait(1);
	}
}

/* Scan all 32 MDIO addresses for a PHY. Returns its address, or -1 if nothing
 * answered. PHYIDR1 (reg 2) reading 0x0000 or 0xffff is "no device driving the
 * bus"; the DP83825's identity is 0x2000a140, which pins the die/OUI but does
 * NOT distinguish the part's grade or package suffix, so it is printed for the
 * reader rather than compared against. */
static int eth_phy_find(void)
{
	for (uint8_t phy = 0; phy < 32; phy++) {
		uint16_t id1 = eth_mdio_read(phy, 2);
		if (id1 != 0xFFFF && id1 != 0x0000) {
			printf("[evkdemo] ETH: MDIO PHY@%u id=%04x%04x (DP83825 = 2000a140)\n",
			       phy,
			       id1,
			       eth_mdio_read(phy, 3));
			return phy;
		}
	}
	return -1;
}

/* Put the PHY in 50 MHz-reference RMII mode, restart auto-negotiation and wait
 * for the wire link. Returns true once BMSR reports both link-up (bit 2) and
 * auto-neg-complete (bit 5). */
static bool eth_phy_wait_link(int phy)
{
	/*
	 * RCSR (0x17) bit 7 REF_CLK_SEL = 1 puts the PHY in 50 MHz-reference RMII
	 * mode, i.e. makes it the RMII SLAVE clocked from the module's external
	 * oscillator. Bench-confirmed on this silicon: with bit 7 set the PHY
	 * forms a media link (BMSR 0x786d), with it clear it does not (0x7849).
	 * This is a clock-domain setting only -- the separate data-plane stall
	 * that looked like the same fault turned out to be the DTCM buffer
	 * placement the app overlay now fixes.
	 */
	uint16_t rcsr = eth_mdio_read(phy, 0x17);
	eth_mdio_write(phy, 0x17, rcsr | BIT(7));
	printf("[evkdemo] ETH: RCSR 0x%04x -> 0x%04x (REF_CLK_SEL = 50 MHz reference)\n",
	       rcsr,
	       eth_mdio_read(phy, 0x17));

	eth_mdio_write(phy, 0, BIT(12) | BIT(9)); /* BMCR: auto-neg enable + restart */
	for (int i = 0; i < ETH_LINK_POLLS; i++) {
		k_msleep(ETH_LINK_POLL_MS);
		uint16_t bmsr = eth_mdio_read(phy, 1);
		if ((bmsr & BIT(2)) && (bmsr & BIT(5))) {
			printf("[evkdemo] ETH: wire link UP after %d ms (BMSR=%04x ANLPAR=%04x)\n",
			       (i + 1) * ETH_LINK_POLL_MS,
			       bmsr,
			       eth_mdio_read(phy, 0x05));
			return true;
		}
	}
	/* ANLPAR (the partner's advertisement) reading 0 is the tell that nothing
	 * is on the other end at all, as opposed to a partner that cannot agree. */
	printf("[evkdemo] ETH: wire link DOWN after %d ms (BMSR=%04x ANLPAR=%04x)\n",
	       ETH_LINK_POLLS * ETH_LINK_POLL_MS,
	       eth_mdio_read(phy, 1),
	       eth_mdio_read(phy, 0x05));
	return false;
}

static phase_verdict_t phase_ethernet(demo_ctx_t *ctx)
{
	printf("[evkdemo] -- Phase: Ethernet (RMII GMAC + on-module DP83825 PHY) --\n");

	struct net_if *iface = net_if_get_default();
	if (iface == NULL) {
		printf("[evkdemo] ETH: no default network interface -- the alif,ethernet node did "
		       "not bind (check the overlay's &ethernet status and CONFIG_ETH_DWMAC_ALIF)\n");
		ctx->note = "no network interface -- the MAC did not bind";
		return PHASE_FAIL;
	}

	struct net_linkaddr *la = net_if_get_link_addr(iface);
	printf("[evkdemo] ETH: MAC %02x:%02x:%02x:%02x:%02x:%02x (per-boot random "
	       "locally-administered, from the SoC dtsi's zephyr,random-mac-address)\n",
	       la->addr[0],
	       la->addr[1],
	       la->addr[2],
	       la->addr[3],
	       la->addr[4],
	       la->addr[5]);

	/*
	 * Which reference clock the driver's AUTO probe settled on, read back from
	 * ETH_CTRL bit 4 (0 = the module's external 50 MHz oscillator, 1 = the
	 * internal-PLL fallback). EXTERNAL is the bench-verified path and is the
	 * proof that eth_phy_power_init() ran early enough: the oscillator is
	 * behind the power enable, so the probe can only have found it if the PHY
	 * was already powered when the eth driver initialised.
	 *
	 * ADDRESS PROVENANCE, stated because it matters if this is ever reused:
	 * 0x4903F080 bit 4 was BENCH-OBSERVED over SWD during the E8 Ethernet
	 * bring-up, not transcribed from a public TRM. It is not a FAIL gate here
	 * -- the internal PLL is a real code path -- it is a diagnostic.
	 */
	bool refclk_external = (sys_read32(0x4903F080U) & BIT(4)) == 0U;
	printf("[evkdemo] ETH: RMII refclk = %s (ETH_CTRL bit4=%d)\n",
	       refclk_external ? "EXTERNAL oscillator -- the PHY was powered before the probe"
	                       : "internal PLL -- the external oscillator was NOT seen at probe time",
	       refclk_external ? 0 : 1);

	int rc = net_if_up(iface);
	printf("[evkdemo] ETH: net_if_up -> %d%s\n", rc, (rc == -EALREADY) ? " (already up)" : "");
	if (rc != 0 && rc != -EALREADY) {
		printf("[evkdemo] ETH: the interface refused to come up\n");
		ctx->note = "net_if_up refused";
		return PHASE_FAIL;
	}

	int phy = eth_phy_find();
	if (phy < 0) {
		printf("[evkdemo] ETH: NO PHY answered on any of the 32 MDIO addresses. The DP83825 "
		       "is fitted on every E1M-AEN SoM, so this is on-module: check E_PHY_PWRDWN "
		       "(P15_4) drove high, E_PHY_RESET (P11_6) released, and the MDC/MDIO pinmux "
		       "(P11_2/P11_1)\n");
		ctx->note = "no PHY answered on MDIO";
		return PHASE_FAIL;
	}

	/* ANAR = what we advertise, ANLPAR = what the partner advertises (0 means
	 * we are hearing nothing at all), PHYSTS = link/speed, RCSR = RMII mode. */
	printf("[evkdemo] ETH: PHY regs ANAR=%04x ANLPAR=%04x PHYSTS=%04x RCSR=%04x\n",
	       eth_mdio_read(phy, 0x04),
	       eth_mdio_read(phy, 0x05),
	       eth_mdio_read(phy, 0x10),
	       eth_mdio_read(phy, 0x17));

	if (!eth_phy_wait_link(phy)) {
		/* The normal unattended-bench outcome. The PHY is alive and clocked
		 * (it answered MDIO above); there is simply nobody on the wire. */
		printf("[evkdemo] ETH: no carrier -- is a cable plugged into a live switch port?\n");
		ctx->note = "no carrier -- cable?";
		return PHASE_SKIPPED;
	}

	/* The real test. A lease requires DISCOVER out and OFFER/ACK back, so it
	 * cannot complete unless both DMA directions genuinely work. */
	bool bound = false;
	net_dhcpv4_start(iface);
	for (int i = 0; i < ETH_DHCP_POLLS; i++) {
		if (iface->config.dhcpv4.state == NET_DHCPV4_BOUND) {
			bound = true;
			break;
		}
		k_msleep(ETH_DHCP_POLL_MS);
	}

	uint64_t tx = iface->stats.bytes.sent;
	uint64_t rx = iface->stats.bytes.received;
	printf("[evkdemo] ETH: admin_up=%d carrier_ok=%d(SYNTHETIC, not a link proof) "
	       "tx_bytes=%" PRIu64 " rx_bytes=%" PRIu64 " dhcp_bound=%d\n",
	       net_if_is_admin_up(iface),
	       net_if_is_carrier_ok(iface),
	       tx,
	       rx,
	       bound);

	if (bound) {
		char                ip[NET_IPV4_ADDR_LEN] = { 0 };
		struct net_if_addr *ua                    = &iface->config.ip.ipv4->unicast[0].ipv4;
		net_addr_ntop(AF_INET, &ua->address.in_addr, ip, sizeof(ip));
		printf("[evkdemo] ETH: DHCP lease = %s -- wire link UP and both DMA directions "
		       "proven end to end\n",
		       ip);
		return PHASE_PASS;
	}

	if (tx == 0u) {
		/* Unambiguous: we know the DHCP client queued DISCOVERs and the link
		 * is up, so a zero TX count means no frame left the MAC whatever is
		 * on the far end. This is the TX half of the DMA-placement failure
		 * the app overlay's memory block describes. */
		printf("[evkdemo] ETH: link is UP but the MAC transmitted ZERO bytes -- no frame "
		       "left the part. Suspect the descriptor rings / net_buf pool are not in "
		       "DMA-reachable memory; see the placement block in this app's overlay\n");
		ctx->note = "link UP but the MAC transmitted ZERO bytes";
		return PHASE_FAIL;
	}

	/* Frames left, no lease came back. Two very different worlds, and rx_bytes
	 * is what separates them -- but neither is provably a board fault, so
	 * neither is a FAIL: a live switch port with no other talkers and no DHCP
	 * server legitimately sends us nothing at all. */
	if (rx == 0u) {
		printf("[evkdemo] ETH: TX moved but NOTHING was received. Either this segment is "
		       "silent (no DHCP server, no other talkers) or the RX path is dead -- the "
		       "counters alone cannot tell those apart. Retry against a switch with a DHCP "
		       "server to settle it\n");
		ctx->note = "link UP, TX ok, RX silent -- no DHCP server, or dead RX";
	} else {
		printf("[evkdemo] ETH: the segment is live (we received frames) but no DHCP server "
		       "answered in %d s\n",
		       (ETH_DHCP_POLLS * ETH_DHCP_POLL_MS) / 1000);
		ctx->note = "link UP, traffic seen, no DHCP server";
	}
	return PHASE_SKIPPED;
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

/* NPU inference: the CODE does not fit in ITCM. Not the model, not a
 * boot flow, and not a camera.
 *
 * MEASURED, and it corrects what this comment used to say. Building this
 * demo with the NPU Kconfig set from examples/aen/aen-npu-inference-alp
 * (TFLM + ETHOS_U + ETHOS_U85_256 + CPP/STD_CPP17/REQUIRES_FULL_LIBCPP +
 * the alp inference dispatch) and NO MODEL AT ALL gives:
 *
 *     ld.bfd: region `FLASH' overflowed by 10664 bytes
 *
 * So the software stack alone overruns the ITCM left after the other nine
 * phases. That is the whole constraint.
 *
 * WHY THE OLD REASON WAS WRONG, since it pointed at the wrong fix. This
 * comment previously said the ~263 KiB Vela person_detect_u85 model is
 * what does not fit, and that adding NPU means relinking into MRAM slot0.
 * The model never needed ITCM: the Ethos-U is a DMA master that reads its
 * model and arena over the SRAM AXI port, so they must live in global
 * SRAM0 (@0x02000000) whatever the image does -- which is exactly why
 * aen-npu-inference-alp memcpy's the model out of rodata into SRAM0 at
 * boot. Reading the old text, the obvious next move was "put the model in
 * SRAM0", which was already mandatory and buys nothing.
 *
 * WHAT WOULD ACTUALLY WORK, for whoever picks this up. Nothing here needs
 * MRAM. Phase 10 already carved the bank -- 64 KiB SRAM0 at 0x02000000 for
 * phase 13's JPEG buffers, 512 KiB system RAM at 0x02010000 -- leaving
 * 0x02090000..0x02400000 free, ample for a model plus a 256 KiB arena. The
 * blob can be side-loaded straight into that window at Flow C time
 * (scripts/bench/aen/ram-run.sh takes a preload J-Link file that runs
 * after halt and before the image loadbin; Zephyr's init zeroes only its
 * own .bss, so the blob survives). The phase would then open it with an
 * explicit SRAM0 arena and PASS on the proven app's own criterion --
 * alp_inference_invoke() == ALP_OK and a non-zero output tensor -- and
 * report SKIPPED when no blob is present, the same shape as "no SD card
 * fitted".
 *
 * The blocker for all of that is the 10664 bytes above. Closing it means
 * dropping code the other nine phases need, and nine trustworthy phases
 * are worth more than ten with one of them thinned to fit. Shrinking the
 * MODEL would not help either -- the footprint above has no model in it,
 * and a toy network would prove dispatch rather than inference. */
static phase_verdict_t phase_npu_stub(demo_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	printf("[evkdemo] -- Phase: NPU inference -- SKIPPED (the CODE does not fit, not the "
	       "model: building this demo with the NPU Kconfig set and no model at all "
	       "overflows ITCM by 10664 bytes. The model belongs in SRAM0 either way -- the "
	       "Ethos-U reads it over the SRAM AXI port -- so relinking to MRAM would not "
	       "help. See the comment above this function for what would) --\n");
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
	{ "CC3501E Wi-Fi/BLE", phase_cc3501e },
	{ "SD card", phase_sdcard },
	{ "Ethernet", phase_ethernet },
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
	const char     *notes[ARRAY_SIZE(PHASES)] = { NULL };
	int             n_pass = 0, n_skipped = 0, n_fail = 0;

	for (size_t i = 0; i < ARRAY_SIZE(PHASES); i++) {
		ctx.note   = NULL; /* cleared per phase: a note never leaks forward. */
		results[i] = PHASES[i].run(&ctx);
		notes[i]   = ctx.note;
		printf("[evkdemo] phase %2zu/%2zu: %-34s %s%s%s\n",
		       i + 1,
		       ARRAY_SIZE(PHASES),
		       PHASES[i].name,
		       verdict_str(results[i]),
		       (notes[i] != NULL) ? " -- " : "",
		       (notes[i] != NULL) ? notes[i] : "");
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
		/* A qualifier is printed alongside the verdict, never instead of it:
		 * it explains a verdict, it is not a fourth one. */
		printf("[evkdemo]   %-38s %-7s %s\n",
		       PHASES[i].name,
		       verdict_str(results[i]),
		       (notes[i] != NULL) ? notes[i] : "");
	}
	printf("[evkdemo] ====================================================\n");

	/* A run full of SKIPPED phases is not a failed run -- all three counts
	 * are reported together so that distinction stays legible at a
	 * glance, exactly the property i2c-device-hub's old tally lacked. */
	printf("[evkdemo] RESULT: %d PASS, %d SKIPPED, %d FAIL\n", n_pass, n_skipped, n_fail);
	printf("[evkdemo] done\n");
	return 0;
}
