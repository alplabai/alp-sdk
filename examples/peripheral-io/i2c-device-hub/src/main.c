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
 *   TCA6408A   I/O expander 0x20   config + input port (TCAL9538 @0x72 alt)
 *   24C128     EEPROM       0x50   first 16 bytes
 *
 * Each device is independent: a missing / DNP part is reported and skipped,
 * never fatal.  The final RESULT line states how many of the attempted
 * devices answered.
 */

#include <stdio.h>

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
		icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_2G);
		icm42670_axes_t a  = { 0 };
		alp_status_t    rs = icm42670_read_accel(&imu, &a);
		printf("[devhub] ICM42670 @0x%02x id=0x%02x accel{%d,%d,%d} rs=%d\n",
		       EVK_I2C_ADDR_ICM42670,
		       id,
		       a.x,
		       a.y,
		       a.z,
		       (int)rs);
		answered++;
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
		bmi323_set_accel(&bmi, BMI323_ODR_100_HZ, BMI323_ACCEL_FS_2G);
		bmi323_axes_t a  = { 0 };
		alp_status_t  rs = bmi323_read_accel(&bmi, &a);
		printf("[devhub] BMI323   @0x%02x id=0x%02x accel{%d,%d,%d} rs=%d\n",
		       EVK_I2C_ADDR_BMI323,
		       id,
		       a.x,
		       a.y,
		       a.z,
		       (int)rs);
		answered++;
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
		bmp581_raw_t raw = { 0 };
		alp_status_t rs  = bmp581_read_raw(&baro, &raw);
		printf("[devhub] BMP581   @0x%02x id=0x%02x p_raw=%d t_raw=%d rs=%d\n",
		       EVK_I2C_ADDR_BMP581,
		       id,
		       raw.pressure_raw,
		       raw.temperature_raw,
		       (int)rs);
		answered++;
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
			int32_t mv = 0, ua = 0;
			ina236_read_bus_mv(&mon, &mv);
			alp_status_t rs = ina236_read_current_ua(&mon, &ua);
			printf("[devhub] INA236 %-6s @0x%02x  %ld mV  %ld uA  rs=%d\n",
			       INA_RAILS[i].name,
			       INA_RAILS[i].addr,
			       (long)mv,
			       (long)ua,
			       (int)rs);
			answered++;
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
			uint8_t rev = 0;
			tas2563_read_revision(&amp, &rev);
			alp_status_t cs   = tas2563_set_mode(&amp, TAS2563_MODE_ACTIVE);
			uint8_t      mreg = 0x02u, mode = 0xeeu;
			alp_i2c_write_read(bus, tas_addrs[i], &mreg, 1, &mode, 1);
			printf("[devhub] TAS2563  @0x%02x rev=0x%02x set_active=%d MODE_CTRL=0x%02x\n",
			       tas_addrs[i],
			       rev,
			       (int)cs,
			       mode);
			answered++;
		} else {
			printf("[devhub] TAS2563  @0x%02x absent\n", tas_addrs[i]);
		}
	}

	/* --- I/O expander (U35): the EVK fits either a TCAL9538 @0x72 or, when the
	 * TCA6408A alternative is populated (R112 fitted / R145 DNP), a TCA6408A @0x20.
	 * Both are PCA9538-register-compatible, so the tcal9538 driver drives either --
	 * probe both addresses. Read the config reg + input port P0 to prove I2C R/W. */
	{
		attempted++;
		const uint8_t ioexp_addrs[] = { EVK_I2C_ADDR_TCA6408A_MAIN, EVK_I2C_ADDR_TCAL9538_MAIN };
		bool          ioexp_ok      = false;
		for (size_t i = 0; i < ARRAY_SIZE(ioexp_addrs) && !ioexp_ok; i++) {
			tcal9538_t io;
			if (tcal9538_init(&io, bus, ioexp_addrs[i]) != ALP_OK) continue;
			uint8_t cfg = 0xee, in0 = 0xee;
			alp_i2c_write_read(bus, ioexp_addrs[i], (uint8_t[]){ 0x03 }, 1, &cfg, 1); /* config */
			alp_i2c_write_read(
			    bus, ioexp_addrs[i], (uint8_t[]){ 0x00 }, 1, &in0, 1); /* input port */
			printf("[devhub] IOEXP    @0x%02x ok (PCA9538-class) config=0x%02x input=0x%02x\n",
			       ioexp_addrs[i],
			       cfg,
			       in0);
			answered++;
			ioexp_ok = true;
		}
		if (!ioexp_ok)
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
		printf("\n");
		answered++;
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
