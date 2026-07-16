/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BMP581 barometric pressure sensor driver.  See header.
 *
 * Compared to BMP280 / BME280 the BMP581 returns *already-compensated*
 * pressure and temperature, so this driver doesn't carry calibration
 * coefficients.  bmp581_compensate is a thin scale conversion from
 * the chip's native units (1/64 Pa, 1/65536 °C) into Pa and
 * milli-degrees-C × 1000.
 */

#include <stddef.h>

#include "alp/chips/bmp581.h"

/* ------------------------------------------------------------------ */
/* Register map (BST-BMP581-DS004)                                    */
/* ------------------------------------------------------------------ */

#define REG_CHIP_ID    0x01
#define REG_CMD        0x7E
#define REG_OSR_CONF   0x36
#define REG_ODR_CONF   0x37
#define REG_TEMP_XLSB  0x1D /* T = [TEMP_MSB][TEMP_LSB][TEMP_XLSB] */
#define REG_PRESS_XLSB 0x20 /* P = [PRESS_MSB][PRESS_LSB][PRESS_XLSB] */

#define CMD_SOFT_RESET 0xB6

static alp_status_t reg_write(bmp581_t *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read(bmp581_t *dev, uint8_t reg, uint8_t *out, size_t len)
{
	return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, out, len);
}

/* Sign-extend a 24-bit little-endian field to int32_t.
 * Buffer layout per datasheet §6.6: XLSB, LSB, MSB (LE order). */
static int32_t s24_le(const uint8_t *p)
{
	uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
	/* Sign-extend bit 23. */
	if (u & (1u << 23)) u |= 0xFF000000u;
	return (int32_t)u;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t bmp581_init(bmp581_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->initialised = false;

	uint8_t      id = 0;
	alp_status_t s  = bmp581_read_id(dev, &id);
	if (s != ALP_OK) return s;
	if (id != BMP581_CHIP_ID) return ALP_ERR_IO;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t bmp581_read_id(bmp581_t *dev, uint8_t *id_out)
{
	if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
	return reg_read(dev, REG_CHIP_ID, id_out, 1);
}

alp_status_t bmp581_set_sampling(bmp581_t     *dev,
                                 bmp581_osr_t  press_osr,
                                 bmp581_osr_t  temp_osr,
                                 bmp581_odr_t  odr,
                                 bmp581_mode_t mode)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	/* OSR and mode are contiguous encodings spanning their whole field
     * width, so an upper-bound check is sufficient. */
	if ((unsigned)press_osr > BMP581_OSR_X128) return ALP_ERR_INVAL;
	if ((unsigned)temp_osr > BMP581_OSR_X128) return ALP_ERR_INVAL;
	if ((unsigned)mode > BMP581_MODE_CONTINUOUS) return ALP_ERR_INVAL;

	/* ODR is sparse -- BST-BMP581-DS004 defines all 32 codes of the
     * 5-bit field, but bmp581_odr_t only declares a curated subset
     * (0x00, 0x01, 0x07, 0x0E, 0x14, 0x17, 0x1C).  An upper-bound / mask
     * check would silently admit an undeclared-but-real ODR the API
     * doesn't expose, so switch-validate against the declared set. */
	switch (odr) {
	case BMP581_ODR_240_HZ:
	case BMP581_ODR_120_HZ:
	case BMP581_ODR_50_HZ:
	case BMP581_ODR_25_HZ:
	case BMP581_ODR_10_HZ:
	case BMP581_ODR_5_HZ:
	case BMP581_ODR_1_HZ:
		break;
	default:
		return ALP_ERR_INVAL;
	}

	/* OSR_CONFIG: PRESS_EN[6] | OSR_P[5:3] | OSR_T[2:0].
     * Always enable pressure -- v0.2 doesn't expose temperature-only
     * mode (the chip can do it but apps that need just temperature
     * usually don't pick a barometer). */
	uint8_t osr =
	    (uint8_t)((1u << 6) | (((uint8_t)press_osr & 0x07u) << 3) | ((uint8_t)temp_osr & 0x07u));
	alp_status_t s = reg_write(dev, REG_OSR_CONF, osr);
	if (s != ALP_OK) return s;

	/* ODR_CONFIG: ODR[6:2] | PWR_MODE[1:0]. */
	uint8_t conf = (uint8_t)((((uint8_t)odr & 0x1Fu) << 2) | ((uint8_t)mode & 0x03u));
	return reg_write(dev, REG_ODR_CONF, conf);
}

alp_status_t bmp581_read_raw(bmp581_t *dev, bmp581_raw_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	/* Burst-read T(3) + P(3) starting from REG_TEMP_XLSB.  The chip
     * arranges them in LSB-first order; pack each into a sign-
     * extended int32_t. */
	uint8_t      buf[6] = { 0 };
	alp_status_t s      = reg_read(dev, REG_TEMP_XLSB, buf, sizeof buf);
	if (s != ALP_OK) return s;
	out->temperature_raw = s24_le(&buf[0]);
	out->pressure_raw    = s24_le(&buf[3]);
	return ALP_OK;
}

alp_status_t bmp581_compensate(const bmp581_raw_t *raw, bmp581_compensated_t *out)
{
	if (raw == NULL || out == NULL) return ALP_ERR_INVAL;
	/* Pressure: chip emits 1/64 Pa.  Round-half-up to whole Pa. */
	out->pressure_pa = (raw->pressure_raw + 32) / 64;
	/* Temperature: chip emits 1/65536 °C.  Convert to °C × 1000.
     * raw / 65536 * 1000 = raw * 1000 / 65536 ≈ raw / 65.536.  Use
     * exact 64-bit math to avoid intermediate overflow. */
	int64_t t              = ((int64_t)raw->temperature_raw * 1000) / 65536;
	out->temperature_c1000 = (int32_t)t;
	return ALP_OK;
}

alp_status_t bmp581_soft_reset(bmp581_t *dev)
{
	if (dev == NULL || dev->bus == NULL) return ALP_ERR_INVAL;
	return reg_write(dev, REG_CMD, CMD_SOFT_RESET);
}

void bmp581_deinit(bmp581_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
