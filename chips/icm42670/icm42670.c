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

#define REG_PWR_MGMT0 0x1F
#define REG_ACCEL_CONFIG0 0x21
#define REG_GYRO_CONFIG0 0x20
#define REG_TEMP_DATA1 0x09    /* Big-endian: TEMP_DATA1 then TEMP_DATA0 */
#define REG_ACCEL_DATA_X1 0x0B /* AX1 AX0 AY1 AY0 AZ1 AZ0 */
#define REG_GYRO_DATA_X1 0x11  /* GX1 GX0 GY1 GY0 GZ1 GZ0 */
#define REG_WHO_AM_I 0x75

/* PWR_MGMT0 bits: GYRO_MODE[3:2], ACCEL_MODE[1:0].
 * 0 = off, 1 = standby, 2 = low-noise, 3 = low-power. */
#define PWR_GYRO_LN (0x3u << 2)
#define PWR_ACCEL_LN (0x3u << 0)

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

	uint8_t      id  = 0;
	alp_status_t s   = icm42670_read_id(dev, &id);
	if (s != ALP_OK) return s;
	if (id != ICM42670_WHO_AM_I_VAL) return ALP_ERR_IO;

	/* Bring both engines into low-noise mode so subsequent
     * read_accel / read_gyro return live data without waiting for
     * a separate "start sampling" call. */
	s = reg_write(dev, REG_PWR_MGMT0, PWR_GYRO_LN | PWR_ACCEL_LN);
	if (s != ALP_OK) return s;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t icm42670_read_id(icm42670_t *dev, uint8_t *id_out)
{
	if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
	return reg_read(dev, REG_WHO_AM_I, id_out, 1);
}

alp_status_t icm42670_set_accel(icm42670_t *dev, icm42670_odr_t odr, icm42670_accel_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	/* ACCEL_CONFIG0: ACCEL_UI_FS_SEL[6:5] | ACCEL_ODR[3:0]. */
	uint8_t      v = (uint8_t)((((uint8_t)fs & 0x03u) << 5) | ((uint8_t)odr & 0x0Fu));
	alp_status_t s = reg_write(dev, REG_ACCEL_CONFIG0, v);
	if (s == ALP_OK) dev->accel_fs = fs;
	return s;
}

alp_status_t icm42670_set_gyro(icm42670_t *dev, icm42670_odr_t odr, icm42670_gyro_fs_t fs)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
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

void icm42670_deinit(icm42670_t *dev)
{
	if (dev == NULL) return;
	/* Best-effort: park the engines in standby before we forget
     * the bus handle.  Errors here are intentionally ignored --
     * deinit must not block on I²C state. */
	if (dev->bus != NULL) {
		(void)reg_write(dev, REG_PWR_MGMT0, 0u);
	}
	dev->initialised = false;
	dev->bus         = NULL;
}
