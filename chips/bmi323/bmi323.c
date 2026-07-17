/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BMI323 6-axis IMU driver.  See header.
 *
 * The BMI323 differs from the typical IMU register protocol on two
 * axes:
 *
 *   1. 16-bit register addressing.  Each transaction writes a
 *      2-byte address pointer (LSB first) before the data phase.
 *   2. Read responses are prefixed with 2 dummy bytes.  The wrapper
 *      reads `2 + len` and discards the first two.  This is a
 *      Bosch consistency choice for SPI compatibility and applies
 *      on I²C too.
 */

#include <stddef.h>
#include <string.h>

#include "alp/chips/bmi323.h"

/* ------------------------------------------------------------------ */
/* Register map (BST-BMI323-DS000)                                     */
/* ------------------------------------------------------------------ */

#define REG_CHIP_ID    0x00
#define REG_ACC_CONF   0x20
#define REG_GYR_CONF   0x21
#define REG_TEMP_DATA  0x09 /* 16-bit signed; LSB first on this reg */
#define REG_ACC_DATA_X 0x03 /* X, Y, Z = 3 × int16, LSB first */
#define REG_GYR_DATA_X 0x06
#define REG_CMD        0x7E /* Command register. */

#define BMI323_CMD_SOFT_RESET 0xDEAFu /* Soft-reset command (BST-BMI323-DS000). */
#define BMI323_SOFT_RESET_MS  3u      /* >= t_soft_reset (~1.5 ms) before CHIP_ID is valid. */

#define BMI323_DUMMY_BYTES 2 /* Read responses include 2 dummy bytes. */

static alp_status_t reg_write(bmi323_t *dev, uint8_t reg, uint16_t val)
{
	/* 16-bit write: [reg, dummy?, val_lo, val_hi].
     * Bosch's SPI mode prefixes a "address-write" stage but on I²C
     * the wire format is reg + LE-16 data byte. */
	uint8_t buf[3] = { reg, (uint8_t)(val & 0xFFu), (uint8_t)(val >> 8) };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read16(bmi323_t *dev, uint8_t reg, uint8_t *out, size_t words)
{
	/* Read returns dummy prefix + words×2 bytes; total = 2 + words*2. */
	if (words == 0) return ALP_ERR_INVAL;
	const size_t total = BMI323_DUMMY_BYTES + words * 2u;
	if (total > 32) return ALP_ERR_INVAL; /* sanity bound */
	uint8_t      scratch[32];
	alp_status_t s = alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, scratch, total);
	if (s != ALP_OK) return s;
	/* Skip the dummy prefix; copy the rest into the caller's buffer. */
	memcpy(out, scratch + BMI323_DUMMY_BYTES, words * 2u);
	return ALP_OK;
}

static int16_t le16(const uint8_t *p)
{
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t bmi323_init(bmi323_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->accel_fs    = BMI323_ACCEL_FS_2G;
	dev->gyro_fs     = BMI323_GYRO_FS_2000_DPS;
	dev->initialised = false;

	/* Power-up / I2C-interface bring-up.  After POR the BMI323 returns 0x00 for
	 * CHIP_ID until it is soft-reset (CMD <- 0xDEAF); the reset write is also the
	 * first I2C transaction, which selects the I2C interface (the part auto-detects
	 * SPI vs I2C from the first access).  Wait t_soft_reset before reading the ID. */
	alp_status_t s = reg_write(dev, REG_CMD, BMI323_CMD_SOFT_RESET);
	if (s != ALP_OK) return s;
	alp_delay_ms(BMI323_SOFT_RESET_MS);

	uint8_t id = 0;
	s          = bmi323_read_id(dev, &id);
	if (s != ALP_OK) return s;
	if (id != BMI323_CHIP_ID) return ALP_ERR_IO;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t bmi323_read_id(bmi323_t *dev, uint8_t *id_out)
{
	if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
	/* CHIP_ID is at register 0x00; the high byte is don't-care. */
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = reg_read16(dev, REG_CHIP_ID, buf, 1);
	if (s != ALP_OK) return s;
	*id_out = buf[0];
	return ALP_OK;
}

alp_status_t bmi323_set_accel(bmi323_t *dev, bmi323_odr_t odr, bmi323_accel_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	/* The ODR field is 4 bits and the FS field 3, but bmi323_odr_t only
     * declares 0x1..0xE and bmi323_accel_fs_t only 0x0..0x3.  Masking
     * alone would write a reserved encoding and still report success. */
	if ((int)odr < (int)BMI323_ODR_0_78125_HZ || (int)odr > (int)BMI323_ODR_6400_HZ) {
		return ALP_ERR_INVAL;
	}
	if ((int)fs < (int)BMI323_ACCEL_FS_2G || (int)fs > (int)BMI323_ACCEL_FS_16G) {
		return ALP_ERR_INVAL;
	}
	/* ACC_CONF[6:4] = FS, [3:0] = ODR.  Bosch reset value enables
     * normal mode bandwidth + averaging which is fine for v0.2. */
	uint16_t v = (uint16_t)((((uint16_t)fs & 0x07u) << 4) | ((uint16_t)odr & 0x0Fu));
	/* Bit 12 selects normal-power mode (vs low-power). */
	v |= (1u << 12);
	alp_status_t s = reg_write(dev, REG_ACC_CONF, v);
	if (s == ALP_OK) dev->accel_fs = fs;
	return s;
}

alp_status_t bmi323_set_gyro(bmi323_t *dev, bmi323_odr_t odr, bmi323_gyro_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	/* Same reserved-encoding trap as bmi323_set_accel; bmi323_gyro_fs_t
     * declares 0x0..0x4 against a 3-bit field. */
	if ((int)odr < (int)BMI323_ODR_0_78125_HZ || (int)odr > (int)BMI323_ODR_6400_HZ) {
		return ALP_ERR_INVAL;
	}
	if ((int)fs < (int)BMI323_GYRO_FS_125_DPS || (int)fs > (int)BMI323_GYRO_FS_2000_DPS) {
		return ALP_ERR_INVAL;
	}
	uint16_t v = (uint16_t)((((uint16_t)fs & 0x07u) << 4) | ((uint16_t)odr & 0x0Fu));
	v |= (1u << 12);
	alp_status_t s = reg_write(dev, REG_GYR_CONF, v);
	if (s == ALP_OK) dev->gyro_fs = fs;
	return s;
}

alp_status_t bmi323_read_accel(bmi323_t *dev, bmi323_axes_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[6] = { 0 };
	alp_status_t s      = reg_read16(dev, REG_ACC_DATA_X, buf, 3);
	if (s != ALP_OK) return s;
	out->x = le16(&buf[0]);
	out->y = le16(&buf[2]);
	out->z = le16(&buf[4]);
	return ALP_OK;
}

alp_status_t bmi323_read_gyro(bmi323_t *dev, bmi323_axes_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[6] = { 0 };
	alp_status_t s      = reg_read16(dev, REG_GYR_DATA_X, buf, 3);
	if (s != ALP_OK) return s;
	out->x = le16(&buf[0]);
	out->y = le16(&buf[2]);
	out->z = le16(&buf[4]);
	return ALP_OK;
}

alp_status_t bmi323_read_temp(bmi323_t *dev, int16_t *temp_raw)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (temp_raw == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = reg_read16(dev, REG_TEMP_DATA, buf, 1);
	if (s != ALP_OK) return s;
	*temp_raw = le16(buf);
	return ALP_OK;
}

void bmi323_deinit(bmi323_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
