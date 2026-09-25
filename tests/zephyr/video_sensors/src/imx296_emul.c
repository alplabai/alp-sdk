/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the IMX296 CCI register map used by imx296_test.c. Modeled on ov5647_emul.c
 * (itself modeled on ov9281_emul.c / upstream tests/drivers/sensor/ina237/src/ina237_emul.c),
 * adapted to the same 16-bit CCI register addressing (see ov9281_emul.c's header comment for the
 * transaction shape: a write is a single 3-byte transfer [addr_hi, addr_lo, data]; a read is a
 * 2-byte address write followed by a 1-byte read -- video_common.h's VIDEO_REG_ADDR16_* helpers
 * do one 8-bit-data transaction per register byte, even for IMX296's wider REG16/REG24
 * registers). UNLIKE ov5647.c's OV5647_REG16 (big-endian), imx296.c's IMX296_REG16/REG24 are
 * LITTLE-endian (VIDEO_REG_ADDR16_DATA16_LE / _DATA24_LE): video_write_cci_reg()/
 * video_read_cci_reg() put the LEAST significant byte at the register's base address and walk
 * UP for each more significant byte (see video_common.c) -- the opposite byte order from
 * ov5647_emul.c's chip-ID seeding comment.
 *
 * SENSOR_INFO (0x3148/0x3149, LE) is seeded to the bench-confirmed colour-IMX296LQR-C signature
 * 0x4A00 (issue #2287, bench run 229) at emulator init, so imx296_init()'s identity probe
 * succeeds without any test code involvement -- LE means the LOW byte (0x00) lives at 0x3148 and
 * the HIGH byte (0x4A) at 0x3149, i.e. 0x4A00 = regs[0x3149] << 8 | regs[0x3148].
 *
 * Like ov5647_emul.c (and unlike ov9281_emul.c), this emulator records every register WRITE in
 * issue order (see struct imx296_emul_write / imx296_emul_log_get() in imx296_emul.h):
 * imx296_test.c's standby/stream-sequence coverage needs to assert not just final register
 * values but that IMX296_REG_STANDBY is cancelled (temporarily) BEFORE SENSOR_INFO is read, and
 * re-armed BEFORE the VMAX/HMAX/INCKSEL/CSI-timing "S" registers are written -- a values-only
 * check cannot see write ORDER.
 */
#define DT_DRV_COMPAT sony_imx296

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

#include "imx296_emul.h"

LOG_MODULE_REGISTER(imx296_emul, CONFIG_I2C_LOG_LEVEL);

/* Sized to cover every register address the driver touches (highest is 0x418c); a flat array
 * keeps the emulator a direct address->value map with no sparse-lookup logic to get wrong. */
#define IMX296_EMUL_REG_MAP_SIZE 0x4200

/* imx296_init()'s boot sequence is short (no per-mode table, one fixed format): XMSTA + STANDBY
 * quiesce, a standby-cancel/SENSOR_INFO-read/standby-re-arm round trip, then VMAX (3 bytes) +
 * HMAX (2 bytes) + INCKSEL0..3 (4 bytes) + CSI_TIMING (1 byte) + CSI_LANE_HS (1 byte) +
 * BLKLEVEL (2 bytes) + ROI_ENABLE (1 byte) -- under 20 writes total. Sized generously (matching
 * ov5647_emul.c's rationale) so accumulated writes across a whole ZTEST suite run cannot silently
 * overflow and drop entries partway through. */
#define IMX296_EMUL_LOG_CAPACITY 256

struct imx296_emul_data {
	uint8_t                  regs[IMX296_EMUL_REG_MAP_SIZE];
	struct imx296_emul_write log[IMX296_EMUL_LOG_CAPACITY];
	size_t                   log_count;
};

int imx296_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value)
{
	struct imx296_emul_data *data = target->data;

	if (reg >= IMX296_EMUL_REG_MAP_SIZE || value == NULL) {
		return -EINVAL;
	}

	*value = data->regs[reg];

	return 0;
}

void imx296_emul_clear_log(const struct emul *target)
{
	struct imx296_emul_data *data = target->data;

	data->log_count = 0;
}

size_t imx296_emul_log_count(const struct emul *target)
{
	struct imx296_emul_data *data = target->data;

	return data->log_count;
}

int imx296_emul_log_get(const struct emul *target, size_t index, struct imx296_emul_write *out)
{
	struct imx296_emul_data *data = target->data;

	if (index >= data->log_count || out == NULL) {
		return -EINVAL;
	}

	*out = data->log[index];

	return 0;
}

static void imx296_emul_log_write(struct imx296_emul_data *data, uint16_t reg, uint8_t value)
{
	if (data->log_count >= IMX296_EMUL_LOG_CAPACITY) {
		LOG_WRN("Write log full (%d entries); dropping 0x%04x=0x%02x",
		        IMX296_EMUL_LOG_CAPACITY,
		        reg,
		        value);
		return;
	}

	data->log[data->log_count].reg   = reg;
	data->log[data->log_count].value = value;
	data->log_count++;
}

static int
imx296_emul_transfer_i2c(const struct emul *target, struct i2c_msg msgs[], int num_msgs, int addr)
{
	struct imx296_emul_data *data = target->data;
	uint16_t                 reg;

	ARG_UNUSED(addr);

	if (!msgs || num_msgs < 1 || num_msgs > 2 || (msgs[0].flags & I2C_MSG_READ)) {
		LOG_ERR("Unexpected transfer shape (num_msgs=%d)", num_msgs);
		return -EIO;
	}

	if (num_msgs == 1) {
		/* video_write_cci_reg(): one write of [addr_hi, addr_lo, data] */
		if (msgs[0].len != 3) {
			LOG_ERR("Expected a 3-byte register write, got %u", msgs[0].len);
			return -EIO;
		}
		reg = sys_get_be16(msgs[0].buf);
		if (reg >= IMX296_EMUL_REG_MAP_SIZE) {
			LOG_ERR("Register 0x%04x out of the emulated map", reg);
			return -EIO;
		}
		data->regs[reg] = msgs[0].buf[2];
		imx296_emul_log_write(data, reg, msgs[0].buf[2]);
		return 0;
	}

	/* video_read_cci_reg(): a 2-byte address write, then a 1-byte read */
	if (msgs[0].len != 2 || !(msgs[1].flags & I2C_MSG_READ) || msgs[1].len != 1) {
		LOG_ERR("Expected a 2-byte address write + 1-byte read");
		return -EIO;
	}
	reg = sys_get_be16(msgs[0].buf);
	if (reg >= IMX296_EMUL_REG_MAP_SIZE) {
		LOG_ERR("Register 0x%04x out of the emulated map", reg);
		return -EIO;
	}
	msgs[1].buf[0] = data->regs[reg];

	return 0;
}

/* Config data: whether this instance should seed a WRONG SENSOR_INFO, to exercise
 * imx296_init()'s identity-mismatch -ENODEV path from imx296_test.c. Set per-instance from the
 * node's own CCI address below -- see IMX296_EMUL(n) and app.overlay's imx296_bad_id_test node. */
struct imx296_emul_cfg {
	bool seed_bad_sensor_info;
};

static int imx296_emul_init(const struct emul *target, const struct device *parent)
{
	struct imx296_emul_data      *data = target->data;
	const struct imx296_emul_cfg *cfg  = target->cfg;

	ARG_UNUSED(parent);

	memset(data->regs, 0, sizeof(data->regs));
	data->log_count = 0;

	/* POR defaults (Sony IMX296 datasheet Register Map, Chip ID = 02h, page 34): STANDBY
	 * (0x3000) = 0x01 (standby), XMSTA (0x300a) = 0x01 (stopped) -- matches bench run 229's
	 * observed cold-boot reset state. */
	data->regs[0x3000] = 0x01;
	data->regs[0x300a] = 0x01;

	/* SENSOR_INFO (IMX296_REG_SENSOR_INFO = IMX296_REG16(0x3148), little-endian): bench run
	 * 229's confirmed colour-IMX296LQR-C signature 0x4a00 -- see this file's header comment
	 * for the byte order. imx296_bad_id_test seeds an arbitrary WRONG value instead, to prove
	 * imx296_init() rejects a signature that isn't this exact one. */
	if (cfg != NULL && cfg->seed_bad_sensor_info) {
		data->regs[0x3148] = 0x00;
		data->regs[0x3149] = 0x00;
	} else {
		data->regs[0x3148] = 0x00;
		data->regs[0x3149] = 0x4a;
	}

	return 0;
}

static const struct i2c_emul_api imx296_emul_api_i2c = {
	.transfer = imx296_emul_transfer_i2c,
};

#define IMX296_EMUL(n) \
	static struct imx296_emul_data      imx296_emul_data_##n; \
	static const struct imx296_emul_cfg imx296_emul_cfg_##n = { \
		.seed_bad_sensor_info = (DT_INST_REG_ADDR(n) != 0x1a), \
	}; \
	EMUL_DT_INST_DEFINE(n, \
	                    imx296_emul_init, \
	                    &imx296_emul_data_##n, \
	                    &imx296_emul_cfg_##n, \
	                    &imx296_emul_api_i2c, \
	                    NULL)

DT_INST_FOREACH_STATUS_OKAY(IMX296_EMUL)
