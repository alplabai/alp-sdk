/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the OV9281 CCI register map used by ov9281_test.c. Modeled on upstream
 * tests/drivers/sensor/ina237/src/ina237_emul.c, adapted to the OV9281's 16-bit CCI register
 * addressing (video_common.h's VIDEO_REG_ADDR16_* helpers do one 8-bit-data transaction per
 * register byte -- see video_write_cci_reg()/video_read_cci_reg() in
 * zephyr/drivers/video/video_common.c): a write is a single 3-byte transfer
 * [addr_hi, addr_lo, data]; a read is a 2-byte address write followed by a 1-byte read.
 *
 * Chip ID (0x300a/0x300b = 0x92/0x81, combining to OV9281_CHIP_ID = 0x9281 big-endian) is
 * seeded at emulator init so ov9281_init()'s probe succeeds without any test code involvement.
 */
#define DT_DRV_COMPAT ovti_ov9281

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

#include "ov9281_emul.h"

LOG_MODULE_REGISTER(ov9281_emul, CONFIG_I2C_LOG_LEVEL);

/* Sized to cover every register address the driver touches (highest is 0x5e00); a flat array
 * keeps the emulator a direct address->value map with no sparse-lookup logic to get wrong. */
#define OV9281_EMUL_REG_MAP_SIZE 0x6000

struct ov9281_emul_data {
	uint8_t regs[OV9281_EMUL_REG_MAP_SIZE];
};

int ov9281_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value)
{
	struct ov9281_emul_data *data = target->data;

	if (reg >= OV9281_EMUL_REG_MAP_SIZE || value == NULL) {
		return -EINVAL;
	}

	*value = data->regs[reg];

	return 0;
}

static int ov9281_emul_transfer_i2c(const struct emul *target, struct i2c_msg msgs[],
				    int num_msgs, int addr)
{
	struct ov9281_emul_data *data = target->data;
	uint16_t reg;

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
		if (reg >= OV9281_EMUL_REG_MAP_SIZE) {
			LOG_ERR("Register 0x%04x out of the emulated map", reg);
			return -EIO;
		}
		data->regs[reg] = msgs[0].buf[2];
		return 0;
	}

	/* video_read_cci_reg(): a 2-byte address write, then a 1-byte read */
	if (msgs[0].len != 2 || !(msgs[1].flags & I2C_MSG_READ) || msgs[1].len != 1) {
		LOG_ERR("Expected a 2-byte address write + 1-byte read");
		return -EIO;
	}
	reg = sys_get_be16(msgs[0].buf);
	if (reg >= OV9281_EMUL_REG_MAP_SIZE) {
		LOG_ERR("Register 0x%04x out of the emulated map", reg);
		return -EIO;
	}
	msgs[1].buf[0] = data->regs[reg];

	return 0;
}

static int ov9281_emul_init(const struct emul *target, const struct device *parent)
{
	struct ov9281_emul_data *data = target->data;

	ARG_UNUSED(parent);

	memset(data->regs, 0, sizeof(data->regs));

	/* OV9281_REG_CHIP_ID = OV9281_REG16(0x300a): a 2-byte big-endian read starting at
	 * 0x300a, byte-by-byte per video_read_cci_reg() -- so 0x300a holds the high byte. */
	data->regs[0x300a] = 0x92;
	data->regs[0x300b] = 0x81;

	return 0;
}

static const struct i2c_emul_api ov9281_emul_api_i2c = {
	.transfer = ov9281_emul_transfer_i2c,
};

#define OV9281_EMUL(n)                                                                          \
	static struct ov9281_emul_data ov9281_emul_data_##n;                                     \
	EMUL_DT_INST_DEFINE(n, ov9281_emul_init, &ov9281_emul_data_##n, NULL,                    \
			    &ov9281_emul_api_i2c, NULL)

DT_INST_FOREACH_STATUS_OKAY(OV9281_EMUL)
