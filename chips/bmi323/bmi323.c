/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BMI323 6-axis IMU driver.  See header.
 *
 * The BMI323 differs from the typical IMU register protocol on two
 * axes:
 *
 *   1. 16-bit register values.  Each register holds a 16-bit value
 *      (LSB first); the register index itself is a single 8-bit
 *      byte -- NOT 16-bit addressing.
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

#define REG_CHIP_ID 0x00
#define REG_ERR_REG 0x01 /* fatal_err bit0, acc_conf_err bit5 (Rev 1.7 p.61 Table 36) */
/* fatal_err (bit0): "an unrecoverable error occurred" (Rev 1.7 p.61 Table 36)
 * -- no field-specific caveat, unlike acc_conf_err (bit5), which flags a bad
 * ACC_CONF encoding and is meaningless at init() time: this driver hasn't
 * written ACC_CONF yet (bmi323_set_accel() does that later).  Acting on
 * acc_conf_err here would mean asserting what makes it fire before ACC_CONF
 * has ever been touched, which this driver isn't in a position to claim. */
#define BMI323_ERR_REG_FATAL_ERR 0x0001u
#define REG_STATUS \
	0x02 /* por_detected bit0 / drdy_gyr bit6 / drdy_acc bit7, all R/C (Rev 1.7 p.66) */
#define REG_ACC_CONF    0x20
#define REG_GYR_CONF    0x21
#define REG_TEMP_DATA   0x09 /* 16-bit signed; LSB first on this reg */
#define REG_ACC_DATA_X  0x03 /* X, Y, Z = 3 × int16, LSB first */
#define REG_GYR_DATA_X  0x06
#define REG_CMD         0x7E /* Command register. */
#define REG_IO_INT_CTRL 0x38 /* INT1/INT2 level+drive+output_en, packed (Rev 1.7 p.102) */
#define REG_INT_CONF    0x39 /* int_latch, bit0 only (Rev 1.7 p.103) */
#define REG_INT_MAP2    0x3B /* acc_drdy_int[11:10] / gyr_drdy_int[9:8] (Rev 1.7 pp.106-107) */

#define BMI323_CMD_SOFT_RESET 0xDEAFu /* Soft-reset command (BST-BMI323-DS000). */
#define BMI323_SOFT_RESET_MS  3u      /* >= t_soft_reset (~1.5 ms) before CHIP_ID is valid. */

/* STATUS bit0: set by a real POR/soft-reset event, nothing else; clear-on-read
 * (Rev 1.7 p.66).  Used by bmi323_init()'s device-initialisation status test. */
#define BMI323_STATUS_POR_DETECTED 0x0001u

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

/* Unsigned counterpart of le16(), used for register bitfields (int_latch,
 * IO_INT_CTRL, INT_MAP2) rather than signed sensor data. */
static alp_status_t reg_read_u16(bmi323_t *dev, uint8_t reg, uint16_t *out)
{
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = reg_read16(dev, reg, buf, 1);
	if (s != ALP_OK) return s;
	*out = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t bmi323_init(bmi323_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;

	dev->bus          = bus;
	dev->addr         = i2c_addr;
	dev->accel_fs     = BMI323_ACCEL_FS_2G;
	dev->gyro_fs      = BMI323_GYRO_FS_2000_DPS;
	dev->initialised  = false;
	dev->por_detected = false; /* Overwritten below once STATUS is actually read. */

	/* Power-up / I2C-interface bring-up.  The soft reset (CMD <- 0xDEAF) is the
	 * first I2C transaction, which also selects the I2C interface (the part
	 * auto-detects SPI vs I2C from the first access).  Wait t_soft_reset before
	 * reading the ID.
	 *
	 * DO NOT treat a correct CHIP_ID as evidence that this reset -- or any later
	 * write -- landed.  CHIP_ID's RESET VALUE is already 0x0043 (BST-BMI323-DS000-13
	 * Rev 1.7, Table 36 p.61) and is readable straight out of POR with no reset at
	 * all (Figure 1 p.14 reads it as the very first transaction).  An earlier
	 * comment here claimed POR returns 0x00 until a soft reset; that was wrong, and
	 * it came from a raw register sweep that did not strip the two dummy bytes the
	 * BMI323 read protocol prepends (Table 53 p.204).  The false claim made a dead
	 * data path look like a live one for a whole bench session: a part that ACKs
	 * every write without applying any of them reads back exactly the same
	 * CHIP_ID.  To prove a write landed, read the register back. */
	alp_status_t s = reg_write(dev, REG_CMD, BMI323_CMD_SOFT_RESET);
	if (s != ALP_OK) return s;
	alp_delay_ms(BMI323_SOFT_RESET_MS);

	/*
	 * Bosch's device-initialisation status test (BST-BMI323-DS000-13 Rev
	 * 1.7, Figure 2 pp.15-16): read ERR_REG, then STATUS, before ever
	 * trusting CHIP_ID.  ERR_REG.fatal_err (bit0) is acted on below --
	 * see BMI323_ERR_REG_FATAL_ERR's comment for why acc_conf_err
	 * (bit5) is not.  STATUS.por_detected (bit0, clear-on-read) is
	 * load-bearing: it is set ONLY by a real POR/soft-reset event,
	 * whereas CHIP_ID's reset value is 0x0043 and reads back correctly
	 * whether or not the soft-reset write just above actually landed
	 * (see the big comment above this function).  A part that ACKs the
	 * write without applying it, or that never left its own prior POR
	 * state, reads por_detected as 0 while CHIP_ID still matches --
	 * catch that here instead of reporting a false ALP_OK.
	 */
	uint16_t err_reg = 0;
	s                = reg_read_u16(dev, REG_ERR_REG, &err_reg);
	if (s != ALP_OK) return s;
	if ((err_reg & BMI323_ERR_REG_FATAL_ERR) != 0) return ALP_ERR_IO;

	uint16_t status = 0;
	s               = reg_read_u16(dev, REG_STATUS, &status);
	if (s != ALP_OK) return s;
	/* Store before deciding whether to fail on it -- see
	 * bmi323_was_por_detected(): once this read happens the bit is
	 * gone from the device (R/C), so this is the only chance to keep
	 * it anywhere a later caller, including one downstream of the
	 * `return` right below, can still see it. */
	dev->por_detected = (status & BMI323_STATUS_POR_DETECTED) != 0;
	/* Distinct from the ALP_ERR_IO below (CHIP_ID mismatch/read
	 * failure) and from the ALP_ERR_IO above (ERR_REG.fatal_err): this
	 * is neither a bus fault nor a bad chip, it's "the reset this
	 * function just issued was never confirmed", which is a
	 * not-ready-yet condition, not an IO or identity error (#2035). */
	if (!dev->por_detected) return ALP_ERR_NOT_READY;

	uint8_t id = 0;
	s          = bmi323_read_id(dev, &id);
	if (s != ALP_OK) return s;
	if (id != BMI323_CHIP_ID) return ALP_ERR_IO;

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t bmi323_was_por_detected(const bmi323_t *dev, bool *out)
{
	/* No dev->initialised gate, unlike this driver's other accessors --
	 * see the doc comment in the header.  The failing bmi323_init()
	 * paths (POR gate rejected, or a later CHIP_ID mismatch/read
	 * error) are exactly what this exists to report. */
	if (dev == NULL || out == NULL) return ALP_ERR_INVAL;
	*out = dev->por_detected;
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
	/* ACC_CONF[6:4] = FS, [3:0] = ODR.  This is a full 16-bit write, not
     * a read-modify-write, so it always forces acc_bw=0 and acc_avg_num=0
     * regardless of any prior configuration -- it does not "preserve the
     * reset value", it happens to land on the same bit pattern.  That's
     * harmless today: 0b100 (normal mode, forced below) is the only
     * acc_mode this driver uses, and BST-BMI323-DS000-13 Rev 1.7 p.22
     * states acc_avg_num has no effect in normal/high-performance mode,
     * while acc_bw=0 (ODR/2) is itself the documented reset default.  If
     * v0.3 exposes acc_bw or low-power mode (where acc_avg_num does
     * matter), this write must become read-modify-write so it stops
     * silently clobbering it. */
	uint16_t v = (uint16_t)((((uint16_t)fs & 0x07u) << 4) | ((uint16_t)odr & 0x0Fu));
	/* ACC_CONF[14:12] = acc_mode, a 3-bit field (BST-BMI323-DS000-13
     * Rev 1.7, pp.21-22, 61, 88-91): 0b000 disabled, 0b011 duty-cycled,
     * 0b100 normal (continuous, reduced current), 0b111 high
     * performance.  0b100 is used here.  The previous `v |= (1u << 12)`
     * produced 0b001, an encoding not in the table, which left the
     * accelerometer disabled -- this is why the bench read -32768 on
     * all three axes. */
	v              = (uint16_t)((v & (uint16_t)~(0x7u << 12)) | (uint16_t)(0x4u << 12));
	alp_status_t s = reg_write(dev, REG_ACC_CONF, v);
	if (s != ALP_OK) return s;
	dev->accel_fs = fs;
	/* tA,SU (BST-BMI323-DS000-13 Rev 1.7, Table 2 p.9): accelerometer
     * start-up from suspend is 2 ms typ.  Wait here so a caller that
     * reads immediately after configuring never races an unready
     * conversion. */
	alp_delay_ms(2);
	return ALP_OK;
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
	/* GYR_CONF[14:12] = gyr_mode, a 3-bit field (BST-BMI323-DS000-13
     * Rev 1.7, pp.21-22, 61, 88-91): 0b000 disabled, 0b001 disabled
     * with drive kept enabled, 0b011 duty-cycled, 0b100 normal,
     * 0b111 high performance (0b010/0b101/0b110 reserved).  0b100 is
     * used here; see bmi323_set_accel for the same field-width bug
     * this replaces. */
	v              = (uint16_t)((v & (uint16_t)~(0x7u << 12)) | (uint16_t)(0x4u << 12));
	alp_status_t s = reg_write(dev, REG_GYR_CONF, v);
	if (s != ALP_OK) return s;
	dev->gyro_fs = fs;
	/* tG,SU (BST-BMI323-DS000-13 Rev 1.7, Table 5 p.10): gyroscope
     * start-up from suspend to high-performance mode, including filter
     * settling, is 30 ms typ -- 15x the accelerometer's tA,SU (2 ms).
     * Wait here so a caller that reads immediately after configuring
     * never races the 0x8000 "no valid sample" reset value. */
	alp_delay_ms(30);
	return ALP_OK;
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

/* ------------------------------------------------------------------ */
/* Interrupt configuration                                             */
/* ------------------------------------------------------------------ */

alp_status_t
bmi323_configure_int_pin(bmi323_t *dev, bmi323_int_pin_t pin, const bmi323_int_pin_config_t *cfg)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (cfg == NULL) return ALP_ERR_INVAL;
	if (pin != BMI323_INT_PIN1 && pin != BMI323_INT_PIN2) return ALP_ERR_INVAL;

	/* IO_INT_CTRL packs INT1 into bits[2:0] (lvl, od, output_en from
	 * LSB up) and INT2 into bits[10:8], same field order
	 * (BST-BMI323-DS000-13 Rev 1.7, p.102).  Read-modify-write so
	 * configuring one pin never disturbs the other. */
	uint16_t     cur = 0;
	alp_status_t s   = reg_read_u16(dev, REG_IO_INT_CTRL, &cur);
	if (s != ALP_OK) return s;

	uint16_t shift = (pin == BMI323_INT_PIN1) ? 0u : 8u;
	uint16_t mask  = (uint16_t)(0x7u << shift);
	uint16_t bits  = (uint16_t)((((uint16_t)cfg->output_enable << 2) | ((uint16_t)cfg->drive << 1) |
	                             (uint16_t)cfg->level)
	                            << shift);
	uint16_t v     = (uint16_t)((cur & (uint16_t)~mask) | bits);
	return reg_write(dev, REG_IO_INT_CTRL, v);
}

alp_status_t bmi323_set_int_latch(bmi323_t *dev, bmi323_int_latch_t mode)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return reg_write(dev, REG_INT_CONF, (uint16_t)mode & 0x1u);
}

alp_status_t bmi323_route_data_ready_int(bmi323_t          *dev,
                                         bmi323_int_route_t accel_route,
                                         bmi323_int_route_t gyro_route)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	/* INT_MAP2 also carries temp_drdy_int, fifo_watermark_int,
	 * fifo_full_int, tap_out, i3c_out and err_status routing this
	 * driver does not manage -- read-modify-write so routing
	 * accel/gyro DRDY never disturbs those. */
	uint16_t     cur = 0;
	alp_status_t s   = reg_read_u16(dev, REG_INT_MAP2, &cur);
	if (s != ALP_OK) return s;

	uint16_t v = cur;
	v          = (uint16_t)((v & (uint16_t)~(0x3u << 10)) | (((uint16_t)accel_route & 0x3u) << 10));
	v          = (uint16_t)((v & (uint16_t)~(0x3u << 8)) | (((uint16_t)gyro_route & 0x3u) << 8));
	return reg_write(dev, REG_INT_MAP2, v);
}

alp_status_t bmi323_data_ready(bmi323_t *dev, bmi323_data_ready_t *out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      buf[2] = { 0 };
	alp_status_t s      = reg_read16(dev, REG_STATUS, buf, 1);
	if (s != ALP_OK) return s;
	out->accel = (buf[0] & (1u << 7)) != 0;
	out->gyro  = (buf[0] & (1u << 6)) != 0;
	return ALP_OK;
}

void bmi323_deinit(bmi323_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
