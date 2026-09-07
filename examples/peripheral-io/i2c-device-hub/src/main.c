/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-device-hub -- functional read-out of every populated device on the
 * EVK sensor/power I2C bus, each through its real chip driver (not a raw
 * register poke).  This is the "prove the whole board is usable" demo:
 * one bus, many ICs, each brought up + read for a real value.
 *
 * Devices exercised (E1M-EVK populated set, addresses from
 * <alp/boards/alp_e1m_evk.h>):
 *   ICM-42670  IMU         0x69   WHO_AM_I + a live accel sample
 *   BMI323     IMU         0x68   CHIP_ID + a live accel sample
 *   BMP581     barometer   0x47   CHIP_ID + a raw pressure/temperature sample
 *   INA236 x6  rail monitors      bus voltage (mV) + current (uA) per rail
 *   TAS2563 x2 I2S amps     0x4d/0x4e  revision + ACTIVE-mode configuration
 *   TCA6408A   I/O expander 0x20   config + input port (NOT ASSEMBLED; TCAL9538 @0x73 alt)
 *   24C128     EEPROM       0x50   first 16 bytes
 *
 * Each device is independent: a missing / DNP part is reported and skipped,
 * never fatal.  The final RESULT line states how many of the attempted
 * devices answered.
 */

#include <stdbool.h>
#include <stdint.h> /* INT16_MIN -- the IMU "no valid sample yet" sentinel */
#include <stdio.h>

#include <zephyr/kernel.h>   /* k_msleep -- conversion / startup waits below */
#include <zephyr/sys/util.h> /* ARRAY_SIZE */

#include "alp/peripheral.h"
#include "alp/board.h"
#include "alp/boards/alp_e1m_evk.h" /* EVK_I2C_ADDR_* */

#include "alp/chips/icm42670.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/ina236.h"
#include "alp/chips/tas2563.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/eeprom_24c128.h"

/* The six INA236 rail monitors.  Address AND per-rail shunt / full-scale
 * current all come from <alp/boards/alp_e1m_evk.h> -- the board header is
 * the single source of these hardware facts (it mirrors the EVK
 * schematic), so a board respin that changes a shunt updates every app
 * through the EVK_INA236_SHUNT_* / EVK_INA236_MAX_* macros; hardcoded
 * copies here would silently drift (#246). */
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

/* Both IMU datasheets document the "no valid sample yet" register state as
 * every axis reading -32768 (0x8000) at once -- verbatim from the BMI323
 * datasheet (BST-BMI323-DS000-13 Rev 1.7, Table 2 p.9): "The default value
 * for each axis is invalid value with 0x8000."  ICM-42670 shares the same
 * reset/pre-first-sample encoding.  A bench that reads this triple has
 * caught the chip mid-startup, not measured "stationary" -- the whole point
 * of gating on it instead of just tallying init() success. */
static bool imu_axes_invalid(int16_t x, int16_t y, int16_t z)
{
	return x == INT16_MIN && y == INT16_MIN && z == INT16_MIN;
}

/* TEMP_DATA_XLSB (0x1D) through PRESS_DATA_MSB (0x22) power-on-reset to 0x7F
 * each (BST-BMP581-DS004-13 Rev 1.13, pp.63-65), and stay there until the
 * chip has actually run a conversion -- so the sign-extended 24-bit
 * pattern 0x7F7F7F on BOTH pressure and temperature means "never sampled",
 * not "reading zero". */
static bool bmp581_raw_invalid(const bmp581_raw_t *raw)
{
	return raw->pressure_raw == 0x7F7F7F && raw->temperature_raw == 0x7F7F7F;
}

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	/* Tallied across every device probed below; the final RESULT line
	 * reports answered/attempted instead of failing hard on the first
	 * absent or DNP part. */
	int attempted = 0;
	int answered  = 0;

	/* 100 kHz standard-mode: the one rate every device on this shared bus
	 * is guaranteed to support, even though most of them can also run
	 * fast-mode (400 kHz). One alp_i2c_open() call serves all seven chip
	 * drivers below, so we pick the slowest common denominator. */
	printf("[devhub] open BOARD_I2C_SENSORS @ 100 kHz\n");
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 100000,
	});
	if (bus == NULL) {
		printf("[devhub] bus open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[devhub] RESULT FAIL: no bus\n");
		printf("[devhub] done\n");
		return 0;
	}

	/* --- ICM-42670 IMU (U12 @0x69): WHO_AM_I (0x67) verified in init + a sample. --- */
	attempted++;
	icm42670_t   imu;
	alp_status_t irc = icm42670_init(&imu, bus, EVK_I2C_ADDR_ICM42670);
	if (irc == ALP_OK) {
		uint8_t id = 0;
		icm42670_read_id(&imu, &id);
		alp_status_t cfg_rc = icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_2G);

		/* Accel startup is 10 ms typ from sleep to a valid sample (TDK
		 * DS-000451 Rev 1.0 p.11); reading right after set_accel() would
		 * just catch the chip mid-startup and report its 0x8000 sentinel
		 * -- which is exactly the bug this fix closes. Add one ODR period
		 * (100 Hz -> 10 ms) so the FIRST real sample has landed in the
		 * output register by the time we read it. p.55 also bars register
		 * WRITES for 200 us after a PWR_MGMT0 change; our next op is a
		 * read, so that guard is moot here, but this wait clears it too. */
		k_msleep(20);

		icm42670_axes_t a  = { 0 };
		alp_status_t    rs = icm42670_read_accel(&imu, &a);
		bool valid = (cfg_rc == ALP_OK) && (rs == ALP_OK) && !imu_axes_invalid(a.x, a.y, a.z);
		printf("[devhub] ICM42670 @0x%02x id=0x%02x accel{%d,%d,%d} cfg_rc=%d rs=%d %s\n",
		       EVK_I2C_ADDR_ICM42670,
		       id,
		       a.x,
		       a.y,
		       a.z,
		       (int)cfg_rc,
		       (int)rs,
		       valid ? "ok" : "STALE/RESET DATA");
		if (valid) answered++;
	} else {
		/* Pre-respin batch: U12 + U13 both strap to 0x69 and collide (garbage). */
		printf("[devhub] ICM42670 @0x%02x init fail (rc=%d; pre-respin collides w/ BMI323 @0x69)\n",
		       EVK_I2C_ADDR_ICM42670,
		       (int)irc);
	}

	/* --- BMI323 IMU (U13): soft-reset bring-up + CHIP_ID (0x43) + a sample. Addressed
	 * at 0x68 post-respin; the pre-respin batch mis-straps it to 0x69 (collides w/ U12). --- */
	attempted++;
	bmi323_t bmi;
	irc = bmi323_init(&bmi, bus, EVK_I2C_ADDR_BMI323);
	if (irc == ALP_OK) {
		uint8_t id = 0;
		bmi323_read_id(&bmi, &id);
		alp_status_t cfg_rc = bmi323_set_accel(&bmi, BMI323_ODR_100_HZ, BMI323_ACCEL_FS_2G);

		/* tA,SU = 2 ms typ (BST-BMI323-DS000-13 Rev 1.7, Table 2 p.9) plus
		 * one ODR period (100 Hz -> 10 ms) before the first real sample is
		 * guaranteed in the output register -- same reasoning as the
		 * ICM-42670 above, this part just has a shorter startup. */
		k_msleep(15);

		bmi323_axes_t a  = { 0 };
		alp_status_t  rs = bmi323_read_accel(&bmi, &a);
		bool valid       = (cfg_rc == ALP_OK) && (rs == ALP_OK) && !imu_axes_invalid(a.x, a.y, a.z);
		printf("[devhub] BMI323   @0x%02x id=0x%02x accel{%d,%d,%d} cfg_rc=%d rs=%d %s\n",
		       EVK_I2C_ADDR_BMI323,
		       id,
		       a.x,
		       a.y,
		       a.z,
		       (int)cfg_rc,
		       (int)rs,
		       valid ? "ok" : "STALE/RESET DATA");
		if (valid) answered++;
	} else {
		printf("[devhub] BMI323   @0x%02x init fail (rc=%d; pre-respin it's at 0x69)\n",
		       EVK_I2C_ADDR_BMI323,
		       (int)irc);
	}

	/* --- BMP581 barometer (0x47): CHIP_ID + a raw sample ---------------- */
	attempted++;
	bmp581_t baro;
	if (bmp581_init(&baro, bus, EVK_I2C_ADDR_BMP581) == ALP_OK) {
		uint8_t id = 0;
		bmp581_read_id(&baro, &id);

		/* init() only verifies CHIP_ID -- by the driver's own contract
		 * (<alp/chips/bmp581.h>) it "does not start sampling". At POR,
		 * OSR_CONFIG's press_en (reg 0x36 bit 6) and ODR_CONFIG's
		 * pwr_mode (reg 0x37 bits[1:0]) are both 0 -- pressure OFF,
		 * mode STANDBY (BST-BMP581-DS004-13 Rev 1.13 pp.50,58) -- so
		 * reading raw data without this call just replays the reset
		 * value forever, which is the exact bug the bench caught.
		 * FORCED mode runs one conversion and returns to standby by
		 * itself, which fits a "read once" example; the odr argument
		 * is a don't-care outside NORMAL/CONTINUOUS mode. */
		alp_status_t cfg_rc = bmp581_set_sampling(
		    &baro, BMP581_OSR_X1, BMP581_OSR_X1, BMP581_ODR_50_HZ, BMP581_MODE_FORCED);

		/* Conversion is 1.0 ms typ at OSR x1 (same datasheet, p.12); 5 ms
		 * is a ~5x margin so the example doesn't need to poll INT_STATUS
		 * (reg 0x27 bit 0, drdy_data_reg, clear-on-read) itself. */
		k_msleep(5);

		bmp581_raw_t raw   = { 0 };
		alp_status_t rs    = bmp581_read_raw(&baro, &raw);
		bool         valid = (cfg_rc == ALP_OK) && (rs == ALP_OK) && !bmp581_raw_invalid(&raw);
		printf("[devhub] BMP581   @0x%02x id=0x%02x p_raw=%d t_raw=%d cfg_rc=%d rs=%d %s\n",
		       EVK_I2C_ADDR_BMP581,
		       id,
		       raw.pressure_raw,
		       raw.temperature_raw,
		       (int)cfg_rc,
		       (int)rs,
		       valid ? "ok" : "STALE/RESET DATA");
		if (valid) answered++;
	} else {
		printf("[devhub] BMP581   @0x%02x absent (err=%d)\n",
		       EVK_I2C_ADDR_BMP581,
		       (int)alp_last_error());
	}

	/* --- INA236 x6 rail monitors: bus voltage + current per rail -------- */
	for (size_t i = 0; i < ARRAY_SIZE(INA_RAILS); i++) {
		attempted++;
		ina236_t     mon;
		alp_status_t rc = ina236_init(&mon,
		                              bus,
		                              INA_RAILS[i].addr,
		                              INA_RAILS[i].shunt_ohms,
		                              INA_RAILS[i].max_a,
		                              INA236_ADCRANGE_81MV);
		if (rc == ALP_OK) {
			/* init() programs CALIBRATION (reg 05h) as its last step, and
			 * writing CONFIG (reg 00h) along the way clears the CVRF
			 * conversion-ready flag (TI SBOSA81D pp.19-24) -- CURRENT only
			 * becomes meaningful after the NEXT shunt conversion completes,
			 * not the moment CAL is written. At the reset defaults
			 * VBUSCT=VSHCT=1100 us, one full bus+shunt cycle is 2.2 ms;
			 * wait a safe margin (here, >2x) rather than polling
			 * MASK_ENABLE (06h) bit 3 (CVRF) ourselves. */
			k_msleep(5);

			int32_t      mv = 0, uv = 0, ua = 0;
			alp_status_t mv_rc = ina236_read_bus_mv(&mon, &mv);
			/* shunt_uv is printed alongside current on purpose: a non-zero
			 * shunt reading with a reported 0 uA current is exactly what
			 * distinguishes "this rail is genuinely idle" from "this
			 * register is stale/never converted" -- the open question the
			 * bench run couldn't answer because current was the only
			 * number on screen. */
			alp_status_t uv_rc = ina236_read_shunt_uv(&mon, &uv);
			alp_status_t rs    = ina236_read_current_ua(&mon, &ua);
			bool         valid = (mv_rc == ALP_OK) && (uv_rc == ALP_OK) && (rs == ALP_OK);
			printf("[devhub] INA236 %-6s @0x%02x  %ld mV  %ld uV(shunt)  %ld uA  rs=%d %s\n",
			       INA_RAILS[i].name,
			       INA_RAILS[i].addr,
			       (long)mv,
			       (long)uv,
			       (long)ua,
			       (int)rs,
			       valid ? "ok" : "READ FAIL");
			if (valid) answered++;
		} else {
			printf("[devhub] INA236 %-6s @0x%02x absent (rc=%d)\n",
			       INA_RAILS[i].name,
			       INA_RAILS[i].addr,
			       (int)rc);
		}
	}

	/* --- TAS2563 x2 I2S smart-amps (0x4d/0x4e): identify + configure --------
	 * Read the revision, then bring the amp out of shutdown into ACTIVE mode and
	 * read MODE_CTRL (reg 0x02) back to confirm the config write took. (Real audio
	 * out also needs an I2S BCLK/WCLK/data stream into the amp -- that path needs
	 * the Alif I2S peripheral driver vendored; see the README.) */
	const uint8_t tas_addrs[] = { EVK_I2C_ADDR_TAS2563_LOW, EVK_I2C_ADDR_TAS2563_HIGH };
	for (size_t i = 0; i < ARRAY_SIZE(tas_addrs); i++) {
		attempted++;
		tas2563_t amp;
		if (tas2563_init(&amp, bus, tas_addrs[i], NULL) == ALP_OK) {
			/* REVID (reg 0x7D) has no fixed value documented for this part --
			 * tas2563_init() already used it as a bare connectivity probe (an
			 * ACK, nothing more), and reading it again here proves only that
			 * the chip is still answering, not that ACTIVE mode took. It is
			 * printed for visibility and deliberately left OUT of the `valid`
			 * gate below. */
			uint8_t rev = 0;
			tas2563_read_revision(&amp, &rev);

			alp_status_t cs = tas2563_set_mode(&amp, TAS2563_MODE_ACTIVE);

			/* MODE_CTRL's low 3 bits are the operating-mode field the driver
			 * writes (TAS2563_MODE_CTRL_MASK in chips/tas2563/tas2563.c,
			 * datasheet Table 7-58); set_mode() preserves the rest via
			 * read-modify-write, so a full-byte match against
			 * TAS2563_MODE_ACTIVE isn't safe -- only the field is. `mode`
			 * starts at the 0xee sentinel (not a valid MODE_CTRL encoding),
			 * so it staying there catches a readback that silently no-op'd
			 * even if the transfer reports ALP_OK -- the same trap the
			 * bench run found: every device answering init while the
			 * config write never actually landed. */
			const uint8_t mode_ctrl_field_mask = 0x07u;
			uint8_t       mreg = 0x02u, mode = 0xeeu;
			alp_status_t  rr    = alp_i2c_write_read(bus, tas_addrs[i], &mreg, 1, &mode, 1);
			bool          valid = (cs == ALP_OK) && (rr == ALP_OK) &&
			                      ((mode & mode_ctrl_field_mask) == TAS2563_MODE_ACTIVE);
			printf("[devhub] TAS2563  @0x%02x rev=0x%02x cs=%d rr=%d MODE_CTRL=0x%02x %s\n",
			       tas_addrs[i],
			       rev,
			       (int)cs,
			       (int)rr,
			       mode,
			       valid ? "ok" : "READ FAIL");
			if (valid) answered++;
		} else {
			printf("[devhub] TAS2563  @0x%02x absent\n", tas_addrs[i]);
		}
	}

	/* --- I/O expander (U35): the EVK fits a TCAL9538 @0x73 by default; an
	 * earlier/other revision instead populates the TCA6408A alternative
	 * (R112 fitted / R145 DNP) @0x20 -- NOT ASSEMBLED on this revision
	 * (neither 2026-09-05 bench board answers there, alp-sdk#1974), so its
	 * macro carries the generator's `_NOT_ASSEMBLED` suffix (#1980). Both
	 * parts are PCA9538-register-compatible, so the tcal9538 driver drives
	 * either -- probe both addresses. Read the config reg + input port P0
	 * to prove I2C R/W. */
	{
		attempted++;
		const uint8_t ioexp_addrs[] = { EVK_I2C_ADDR_TCA6408A_MAIN_NOT_ASSEMBLED,
			                            EVK_I2C_ADDR_TCAL9538_MAIN };
		/* Present vs answered are tracked separately on purpose: the two
		 * addresses are alternate populations of the SAME part (only one is
		 * ever fitted), not two devices to try in sequence. Once init()
		 * finds the real one, we stop probing addresses -- but a present,
		 * initialised part that then returns garbage must still fail, not
		 * fall through to "try the other address" and get silently skipped. */
		bool ioexp_found = false;
		for (size_t i = 0; i < ARRAY_SIZE(ioexp_addrs) && !ioexp_found; i++) {
			tcal9538_t io;
			if (tcal9538_init(&io, bus, ioexp_addrs[i]) != ALP_OK) continue;
			ioexp_found = true;

			/* Both reads must ACK AND land a byte other than the 0xee
			 * sentinel these locals start at -- gating on the return code
			 * alone was exactly the bug the review caught: a transfer that
			 * reports ALP_OK without actually filling `cfg`/`in0` still
			 * counted as "answered". 0xee isn't excluded by the register
			 * map (config's POR default is 0xFF, all inputs -- a real
			 * 0xee is merely unlikely, not unreachable), so this is the
			 * same class of best-effort sentinel the IMU/BMP581 blocks
			 * above use, not a value the datasheet declares invalid. */
			uint8_t      cfg = 0xee, in0 = 0xee;
			alp_status_t cfg_rc =
			    alp_i2c_write_read(bus, ioexp_addrs[i], (uint8_t[]){ 0x03 }, 1, &cfg, 1);
			alp_status_t in0_rc =
			    alp_i2c_write_read(bus, ioexp_addrs[i], (uint8_t[]){ 0x00 }, 1, &in0, 1);
			bool valid = (cfg_rc == ALP_OK) && (in0_rc == ALP_OK) && (cfg != 0xee) && (in0 != 0xee);
			printf("[devhub] IOEXP    @0x%02x %s (PCA9538-class) config=0x%02x input=0x%02x "
			       "cfg_rc=%d in0_rc=%d\n",
			       ioexp_addrs[i],
			       valid ? "ok" : "READ FAIL",
			       cfg,
			       in0,
			       (int)cfg_rc,
			       (int)in0_rc);
			if (valid) answered++;
		}
		if (!ioexp_found)
			printf("[devhub] IOEXP    absent (@0x20 / @0x%02x)\n", EVK_I2C_ADDR_TCAL9538_MAIN);
	}

	/* --- 24C128 EEPROM (0x50): first 16 bytes --------------------------- */
	attempted++;
	eeprom_24c128_t eep;
	if (eeprom_24c128_init(&eep, bus, EEPROM_24C128_I2C_ADDR_LOW) == ALP_OK) {
		uint8_t      b[16] = { 0 };
		alp_status_t rs    = eeprom_24c128_read(&eep, 0, b, sizeof b);
		printf("[devhub] EEPROM   @0x%02x rs=%d bytes:", EEPROM_24C128_I2C_ADDR_LOW, (int)rs);
		for (size_t i = 0; i < sizeof b; i++)
			printf(" %02x", b[i]);
		printf("%s\n", (rs == ALP_OK) ? "" : " READ FAIL");
		/* Unlike the sensor blocks above, EEPROM content has no reset/
		 * power-on-default that's invalid to read -- an erased array
		 * legitimately reads 0xFF, and any other stored byte pattern is
		 * equally legitimate. There's no sentinel to gate on here; the
		 * return code IS the whole check -- but it must actually be
		 * checked (it wasn't, before this fix). */
		if (rs == ALP_OK) answered++;
	} else {
		printf("[devhub] EEPROM   @0x%02x absent (err=%d)\n",
		       EEPROM_24C128_I2C_ADDR_LOW,
		       (int)alp_last_error());
	}

	printf("[devhub] RESULT %s: %d/%d devices answered\n",
	       (answered == attempted) ? "PASS" : "PARTIAL",
	       answered,
	       attempted);
	alp_i2c_close(bus);
	printf("[devhub] done\n");
	return 0;
}
