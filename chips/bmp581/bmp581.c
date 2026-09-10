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
/* Register map (BST-BMP581-DS004-13 Rev 1.13)                        */
/* ------------------------------------------------------------------ */

#define REG_CHIP_ID    0x01
#define REG_CMD        0x7E
#define REG_OSR_CONF   0x36
#define REG_ODR_CONF   0x37
#define REG_TEMP_XLSB  0x1D /* T = [TEMP_MSB][TEMP_LSB][TEMP_XLSB] */
#define REG_PRESS_XLSB 0x20 /* P = [PRESS_MSB][PRESS_LSB][PRESS_XLSB] */

/* REG_ODR_CONF.deep_dis (bit 7): 1 disables re-entry into DEEP STANDBY.
 * BST-BMP581-DS004-13 §4.3.2 (p.16) lists the conditions under which the
 * part re-enters DEEP STANDBY on its own; setting this bit is the only
 * way to keep a configured STANDBY/NORMAL/FORCED/CONTINUOUS mode sticky. */
#define ODR_CONFIG_DEEP_DIS (1u << 7)

#define REG_INT_CONFIG \
	0x14 /* int_mode[0] | int_pol[1] | int_od[2] | int_en[3] | pad_int_drv[7:4] */
#define REG_INT_SOURCE \
	0x15 /* drdy_data_reg_en[0] | fifo_full_en[1] | fifo_ths_en[2] | oor_p_en[3] */
#define REG_INT_STATUS 0x27 /* drdy_data_reg[0] | ... -- clear-on-read (whole register). */
/* REG_STATUS (0x28) -- not read by this driver (see the "do not gate on
 * core_rdy" note below); documented here only to head off a re-derivation
 * mistake. BST-BMP581-DS004-13's own register-map summary table (p.50)
 * labels bits 0/4/7 of 0x28 "reserved_0" / "reserved_4" / "reserved_7",
 * which contradicts §7.22 (p.58) -- the per-register section, with explicit
 * bit offsets, prose per field, and a reset value (0x02) that reproduces
 * the map's own reset column -- which names those same bits
 * status_core_rdy, status_boot_err_corrected, and st_crack_pass. Trust
 * §7.22, not the p.50 summary table. 0x02 is 0x28's documented reset value;
 * Bosch's own init treats it as healthy, so a part that reads 0x02 forever
 * is a correctly-behaving part sitting in DEEP STANDBY, not a broken one --
 * do NOT add a readiness gate on core_rdy: Bosch's BMP5_SensorAPI and
 * upstream Zephyr's driver never wait on it, and the datasheet defines it
 * in exactly one line and never uses it in any procedure. */

#define CMD_SOFT_RESET 0xB6

#define INT_SOURCE_MASK_ALL 0x0Fu /* Only bits[3:0] are defined; [7:4] reserved. */

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
     * (0x00, 0x08, 0x0F, 0x14, 0x17, 0x18, 0x1C).  An upper-bound / mask
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

	/* #2035: after startup/soft-reset the part is in DEEP STANDBY, and
     * BST-BMP581-DS004-13 §4.3 (p.16) requires entering STANDBY first --
     * "Transitions from one mode to another are only possible by entering
     * Standby mode first" -- while §4.3.8 (p.18) says writes to OSR_CONFIG
     * / ODR_CONFIG "in a mode other than STANDBY are lost". The old code
     * wrote both registers straight out of DEEP STANDBY (power-on default:
     * deep_dis=0, odr=1 Hz, no FIFO, no IIR -- exactly the §4.3.2
     * conditions for staying in DEEP STANDBY), so the writes were silently
     * discarded. Force STANDBY with deep_dis=1 (bit 7) before writing
     * either config register, matching Bosch's own bmp5_set_power_mode()
     * (BMP5_SensorAPI) and upstream Zephyr's set_power_mode()
     * (drivers/sensor/bosch/bmp581/bmp581.c). deep_dis=1 is set on every
     * ODR_CONFIG write from here on so the part doesn't fall back into
     * DEEP STANDBY on its own once configured. */
	uint8_t      standby_conf = (uint8_t)(ODR_CONFIG_DEEP_DIS | (((uint8_t)odr & 0x1Fu) << 2) |
	                                      (uint8_t)BMP581_MODE_STANDBY);
	alp_status_t s            = reg_write(dev, REG_ODR_CONF, standby_conf);
	if (s != ALP_OK) return s;
	/* tstandby = 2.5 ms typ (BST-BMP581-DS004-13 p.11); alp_delay_ms is
     * ms-granular and "at least", so round up to 3 ms rather than
     * truncate to 2. */
	alp_delay_ms(3);

	/* OSR_CONFIG: PRESS_EN[6] | OSR_P[5:3] | OSR_T[2:0].
     * Always enable pressure -- v0.2 doesn't expose temperature-only
     * mode (the chip can do it but apps that need just temperature
     * usually don't pick a barometer). Now in STANDBY, so this write
     * actually lands (see #2035 note above). */
	uint8_t osr =
	    (uint8_t)((1u << 6) | (((uint8_t)press_osr & 0x07u) << 3) | ((uint8_t)temp_osr & 0x07u));
	s = reg_write(dev, REG_OSR_CONF, osr);
	if (s != ALP_OK) return s;

	/* ODR_CONFIG: DEEP_DIS[7] | ODR[6:2] | PWR_MODE[1:0]. */
	uint8_t conf =
	    (uint8_t)(ODR_CONFIG_DEEP_DIS | (((uint8_t)odr & 0x1Fu) << 2) | ((uint8_t)mode & 0x03u));
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

alp_status_t bmp581_data_ready(bmp581_t *dev, bool *ready_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (ready_out == NULL) return ALP_ERR_INVAL;

	uint8_t      status = 0;
	alp_status_t s      = reg_read(dev, REG_INT_STATUS, &status, 1);
	if (s != ALP_OK) return s;
	/* NOTE: this read just cleared every asserted bit in INT_STATUS,
     * not only drdy_data_reg -- see the Doxygen warning on this
     * function in the header. */
	*ready_out = (status & 0x01u) != 0;
	return ALP_OK;
}

alp_status_t bmp581_configure_int_pin(bmp581_t             *dev,
                                      bool                  enable,
                                      bmp581_int_drive_t    drive,
                                      bmp581_int_polarity_t polarity,
                                      bmp581_int_mode_t     mode)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	/* INT_CONFIG: pad_int_drv[7:4] | int_en[3] | int_od[2] | int_pol[1] | int_mode[0].
     * pad_int_drv is left at its reset value (0x3) -- this driver only
     * exposes the fields a board-level consumer needs to match its
     * wiring; drive strength tuning can be added if a board needs it. */
	uint8_t conf =
	    (uint8_t)((3u << 4) | ((enable ? 1u : 0u) << 3) | (((uint8_t)drive & 0x01u) << 2) |
	              (((uint8_t)polarity & 0x01u) << 1) | ((uint8_t)mode & 0x01u));
	return reg_write(dev, REG_INT_CONFIG, conf);
}

alp_status_t bmp581_set_int_sources(bmp581_t *dev, uint8_t source_mask)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if ((source_mask & ~INT_SOURCE_MASK_ALL) != 0) return ALP_ERR_INVAL;
	return reg_write(dev, REG_INT_SOURCE, source_mask);
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
