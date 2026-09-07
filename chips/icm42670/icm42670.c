/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TDK InvenSense ICM-42670-P 6-axis IMU driver.
 *
 * Same OS-agnostic shape as chips/lsm6dso.c: talks to the chip
 * exclusively through <alp/peripheral.h>'s I²C surface, which routes
 * to the right backend (Zephyr i2c_*, vendor HAL on bare-metal,
 * /dev/i2c-N on Yocto).
 */

#include <stddef.h>

#include "alp/chips/icm42670.h"

/* ------------------------------------------------------------------ */
/* Register map (DS-000451)                                            */
/* ------------------------------------------------------------------ */

#define REG_PWR_MGMT0       0x1F
#define REG_ACCEL_CONFIG0   0x21
#define REG_GYRO_CONFIG0    0x20
#define REG_TEMP_DATA1      0x09 /* Big-endian: TEMP_DATA1 then TEMP_DATA0 */
#define REG_ACCEL_DATA_X1   0x0B /* AX1 AX0 AY1 AY0 AZ1 AZ0 */
#define REG_GYRO_DATA_X1    0x11 /* GX1 GX0 GY1 GY0 GZ1 GZ0 */
#define REG_WHO_AM_I        0x75
#define REG_INT_CONFIG      0x06 /* INT1/INT2 mode+drive+polarity, packed together (p.50) */
#define REG_INT_SOURCE0     0x2B /* Sources routed to INT1 (p.63) */
#define REG_INT_SOURCE3     0x2D /* Sources routed to INT2 (p.64) */
#define REG_INT_STATUS_DRDY 0x39 /* DATA_RDY_INT, R/C (p.67) */

/* PWR_MGMT0 bits: GYRO_MODE[3:2], ACCEL_MODE[1:0] (TDK DS-000451 Rev 1.0,
 * p.55).  The two fields do NOT share one encoding -- 01 is Standby for
 * the gyro but a second "off" code for the accelerometer:
 *   GYRO_MODE:  00 off, 01 Standby, 10 Reserved,   11 Low Noise.
 *   ACCEL_MODE: 00 off, 01 off,     10 Low Power,  11 Low Noise.
 * PWR_GYRO_LN / PWR_ACCEL_LN below both select code 11 (Low Noise). */
#define PWR_GYRO_LN  (0x3u << 2)
#define PWR_ACCEL_LN (0x3u << 0)

/* TDK DS-000451 Rev 1.0, p.55 PWR_MGMT0 / GYRO_MODE cell, two distinct
 * floors on the gyro's LN-mode dwell time (see icm42670_init() and
 * icm42670_deinit() for how each is honoured):
 *   - GYRO_STARTUP_MS: Table 1 p.10 "Gyroscope Start-Up Time, from gyro
 *     enable to gyro drive ready" -- a *data-validity* floor.
 *   - GYRO_MIN_ON_MS: p.55 "Gyroscope needs to be kept ON for a minimum
 *     of 45ms" -- a *minimum-on* floor, separate from the one above. */
#define GYRO_STARTUP_MS 30u
#define GYRO_MIN_ON_MS  45u

static alp_status_t reg_write(icm42670_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read(icm42670_t *dev, uint8_t reg, uint8_t *out, size_t len)
{
	return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, out, len);
}

/* ICM-42670 register data is big-endian (HIGH byte first). */
static int16_t be16(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t icm42670_init(icm42670_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->accel_fs    = ICM42670_ACCEL_FS_2G;
	dev->gyro_fs     = ICM42670_GYRO_FS_250_DPS;
	dev->initialised = false;

	uint8_t      id = 0;
	alp_status_t s  = icm42670_read_id(dev, &id);
	if (s != ALP_OK) return s;
	if (id != ICM42670_WHO_AM_I_VAL) return ALP_ERR_IO;

	/* Bring both engines into low-noise mode so subsequent
     * read_accel / read_gyro return live data without waiting for
     * a separate "start sampling" call. */
	s = reg_write(dev, REG_PWR_MGMT0, PWR_GYRO_LN | PWR_ACCEL_LN);
	if (s != ALP_OK) return s;

	/* TDK DS-000451 Rev 1.0, p.55, GYRO_MODE cell (quoted in full):
     * "Gyroscope needs to be kept ON for a minimum of 45ms. When
     * transitioning from OFF to any of the other modes, do not issue
     * any register writes for 200 us." -- this write just moved both
     * engines OFF -> LN, and the caller's next call is normally
     * set_accel()/set_gyro() (ACCEL_CONFIG0 / GYRO_CONFIG0 writes).
     * The two engines then need different amounts of time before
     * their data is valid: accelerometer "Accelerometer Startup
     * Time, from sleep mode to valid data" is 10 ms typ (Table 2
     * p.11), gyroscope "Gyroscope Start-Up Time, from gyro enable to
     * gyro drive ready" is 30 ms typ (Table 1 p.10) -- 3x longer.
     * This write enables both engines at once, so wait for the
     * slower one (GYRO_STARTUP_MS) here; anything shorter still lets
     * a caller that reads gyro data immediately after init() race
     * the 0x8000 "no valid sample" reset value.  GYRO_STARTUP_MS is
     * a *data-validity* floor, separate from the 45 ms *minimum-on*
     * floor quoted above -- icm42670_deinit() waits out the
     * remainder of that second floor before turning the gyro back
     * off, since init() always leaves at least GYRO_STARTUP_MS of
     * the 45 ms already elapsed. */
	alp_delay_ms(GYRO_STARTUP_MS);

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t icm42670_read_id(icm42670_t *dev, uint8_t *id_out)
{
	if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
	return reg_read(dev, REG_WHO_AM_I, id_out, 1);
}

/* icm42670_odr_t is sparse and shared by both ODR fields, but the two
 * fields do NOT share one legal set (TDK DS-000451 Rev 1.0):
 *
 *   - ACCEL_CONFIG0[3:0] (p.57): 0x0 reserved, 0x1..0x4 reserved,
 *     0x5..0xF all valid (LN or LP mode depending on the code).
 *   - GYRO_CONFIG0[3:0]  (p.56): 0x0 reserved, 0x1..0x4 reserved,
 *     0x5..0xC valid, 0xD/0xE/0xF ALSO reserved (the gyro has no
 *     low-power sub-1.6 kHz rates the accelerometer does).
 *
 * ICM42670_ODR_OFF (0x0) is reserved on both fields -- ODR is not a
 * power control, PWR_MGMT0 is -- so it is rejected by both setters
 * below rather than ever being written as a "valid" ODR code.
 *
 * No range check can reject any of this; only an explicit member test
 * does, and it must be one test per field. */
static bool accel_odr_is_valid(icm42670_odr_t odr)
{
	switch (odr) {
	case ICM42670_ODR_1_5625_HZ:
	case ICM42670_ODR_3_125_HZ:
	case ICM42670_ODR_6_25_HZ:
	case ICM42670_ODR_12_5_HZ:
	case ICM42670_ODR_25_HZ:
	case ICM42670_ODR_50_HZ:
	case ICM42670_ODR_100_HZ:
	case ICM42670_ODR_200_HZ:
	case ICM42670_ODR_400_HZ:
	case ICM42670_ODR_800_HZ:
	case ICM42670_ODR_1600_HZ:
		return true;
	default:
		return false;
	}
}

static bool gyro_odr_is_valid(icm42670_odr_t odr)
{
	switch (odr) {
	case ICM42670_ODR_12_5_HZ:
	case ICM42670_ODR_25_HZ:
	case ICM42670_ODR_50_HZ:
	case ICM42670_ODR_100_HZ:
	case ICM42670_ODR_200_HZ:
	case ICM42670_ODR_400_HZ:
	case ICM42670_ODR_800_HZ:
	case ICM42670_ODR_1600_HZ:
		return true;
	default:
		return false;
	}
}

alp_status_t icm42670_set_accel(icm42670_t *dev, icm42670_odr_t odr, icm42670_accel_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (!accel_odr_is_valid(odr)) return ALP_ERR_INVAL;
	/* ACCEL_CONFIG0: ACCEL_UI_FS_SEL[6:5] | ACCEL_ODR[3:0]. */
	uint8_t      v = (uint8_t)((((uint8_t)fs & 0x03u) << 5) | ((uint8_t)odr & 0x0Fu));
	alp_status_t s = reg_write(dev, REG_ACCEL_CONFIG0, v);
	if (s == ALP_OK) dev->accel_fs = fs;
	return s;
}

alp_status_t icm42670_set_gyro(icm42670_t *dev, icm42670_odr_t odr, icm42670_gyro_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (!gyro_odr_is_valid(odr)) return ALP_ERR_INVAL;
	/* GYRO_CONFIG0: GYRO_UI_FS_SEL[6:5] | GYRO_ODR[3:0]. */
	uint8_t      v = (uint8_t)((((uint8_t)fs & 0x03u) << 5) | ((uint8_t)odr & 0x0Fu));
	alp_status_t s = reg_write(dev, REG_GYRO_CONFIG0, v);
	if (s == ALP_OK) dev->gyro_fs = fs;
	return s;
}

alp_status_t icm42670_read_accel(icm42670_t *dev, icm42670_axes_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[6] = { 0 };
	alp_status_t s      = reg_read(dev, REG_ACCEL_DATA_X1, buf, sizeof buf);
	if (s != ALP_OK) return s;
	out->x = be16(&buf[0]);
	out->y = be16(&buf[2]);
	out->z = be16(&buf[4]);
	return ALP_OK;
}

alp_status_t icm42670_read_gyro(icm42670_t *dev, icm42670_axes_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[6] = { 0 };
	alp_status_t s      = reg_read(dev, REG_GYRO_DATA_X1, buf, sizeof buf);
	if (s != ALP_OK) return s;
	out->x = be16(&buf[0]);
	out->y = be16(&buf[2]);
	out->z = be16(&buf[4]);
	return ALP_OK;
}

alp_status_t icm42670_read_temp(icm42670_t *dev, int16_t *temp_raw)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (temp_raw == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = reg_read(dev, REG_TEMP_DATA1, buf, sizeof buf);
	if (s != ALP_OK) return s;
	*temp_raw = be16(buf);
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Interrupt configuration                                             */
/* ------------------------------------------------------------------ */

alp_status_t icm42670_configure_int_pin(icm42670_t                      *dev,
                                        icm42670_int_pin_t               pin,
                                        const icm42670_int_pin_config_t *cfg)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (cfg == NULL) return ALP_ERR_INVAL;
	if (pin != ICM42670_INT_PIN1 && pin != ICM42670_INT_PIN2) return ALP_ERR_INVAL;

	/* INT_CONFIG packs INT1 into bits[2:0] (mode, drive, polarity from
	 * MSB to LSB) and INT2 into bits[5:3], same field order (TDK
	 * DS-000451 Rev 1.0, p.50).  Read-modify-write so configuring one
	 * pin never disturbs the other. */
	uint8_t      cur = 0;
	alp_status_t s   = reg_read(dev, REG_INT_CONFIG, &cur, 1);
	if (s != ALP_OK) return s;

	uint8_t shift = (pin == ICM42670_INT_PIN1) ? 0u : 3u;
	uint8_t mask  = (uint8_t)(0x7u << shift);
	uint8_t bits =
	    (uint8_t)((((uint8_t)cfg->mode << 2) | ((uint8_t)cfg->drive << 1) | (uint8_t)cfg->polarity)
	              << shift);
	uint8_t v = (uint8_t)((cur & (uint8_t)~mask) | bits);
	return reg_write(dev, REG_INT_CONFIG, v);
}

alp_status_t icm42670_route_int(icm42670_t *dev, icm42670_int_pin_t pin, uint8_t source_mask)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (pin != ICM42670_INT_PIN1 && pin != ICM42670_INT_PIN2) return ALP_ERR_INVAL;
	uint8_t reg = (pin == ICM42670_INT_PIN1) ? REG_INT_SOURCE0 : REG_INT_SOURCE3;
	return reg_write(dev, reg, source_mask);
}

alp_status_t icm42670_data_ready(icm42670_t *dev, bool *ready)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (ready == NULL) return ALP_ERR_INVAL;
	uint8_t      v = 0;
	alp_status_t s = reg_read(dev, REG_INT_STATUS_DRDY, &v, 1);
	if (s != ALP_OK) return s;
	*ready = (v & 0x01u) != 0;
	return ALP_OK;
}

void icm42670_deinit(icm42670_t *dev)
{
	if (dev == NULL) return;
	/* Best-effort: park the engines in standby before we forget
     * the bus handle.  Errors here are intentionally ignored --
     * deinit must not block on I²C state. */
	if (dev->bus != NULL) {
		/* icm42670_init() always waits GYRO_STARTUP_MS after
     * enabling the gyro before it can return, so by the time any
     * caller reaches deinit() at least that much of the 45 ms
     * GYRO_MIN_ON_MS floor (p.55, quoted in full at the call
     * site above) has already elapsed -- wait out the remainder
     * here so init(); deinit(); never turns the gyro off early,
     * regardless of how much (if any) work the caller did in
     * between. */
		alp_delay_ms(GYRO_MIN_ON_MS - GYRO_STARTUP_MS);
		(void)reg_write(dev, REG_PWR_MGMT0, 0u);
	}
	dev->initialised = false;
	dev->bus         = NULL;
}
