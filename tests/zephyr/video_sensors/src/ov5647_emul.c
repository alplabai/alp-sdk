/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the OV5647 CCI register map used by ov5647_test.c. Modeled on ov9281_emul.c
 * (itself modeled on upstream tests/drivers/sensor/ina237/src/ina237_emul.c), adapted to the same
 * 16-bit CCI register addressing (see ov9281_emul.c's header comment for the transaction shape: a
 * write is a single 3-byte transfer [addr_hi, addr_lo, data]; a read is a 2-byte address write
 * followed by a 1-byte read -- video_common.h's VIDEO_REG_ADDR16_* helpers do one 8-bit-data
 * transaction per register byte, even for OV5647's wider REG16/REG24 registers).
 *
 * Chip ID (0x300a/0x300b = 0x56/0x47, combining to OV5647_CHIP_ID = 0x5647 big-endian) is seeded
 * at emulator init so ov5647_init()'s probe succeeds without any test code involvement.
 *
 * Unlike ov9281_emul.c, this emulator also records every register WRITE in issue order (see
 * struct ov5647_emul_write / ov5647_emul_log_get() in ov5647_emul.h): ov5647_test.c's lane-park
 * regression coverage (issue #2248) needs to assert not just the final parked register VALUES but
 * that 0x0100 (MODE_SELECT) is written running (0x01) BEFORE the three park registers -- parking
 * while still in software standby writes the identical final values but leaves the CSI-2 lanes
 * out of LP-11 (Stop state), a bug a values-only check cannot see.
 */
#define DT_DRV_COMPAT ovti_ov5647

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

#include "ov5647_emul.h"

LOG_MODULE_REGISTER(ov5647_emul, CONFIG_I2C_LOG_LEVEL);

/* Sized to cover every register address the driver touches (highest is 0x503d); a flat array
 * keeps the emulator a direct address->value map with no sparse-lookup logic to get wrong. */
#define OV5647_EMUL_REG_MAP_SIZE 0x6000

/* ov5647_init()'s full boot sequence writes the reset, the (now ~50-entry, since AUTHORIZED
 * LOCAL DIVERGENCE #3 added the common analog/BLC/AEC block) init-regs table, the initial
 * full-frame set_fmt() (window + 1:1 subsample/binning/analog + frmival), and two lane_park()
 * calls -- around 80 writes total; tests that clear the log before acting add only a handful
 * more each. 160 leaves headroom without the log ever being the limiting factor. */
#define OV5647_EMUL_LOG_CAPACITY 160

struct ov5647_emul_data {
	uint8_t                  regs[OV5647_EMUL_REG_MAP_SIZE];
	struct ov5647_emul_write log[OV5647_EMUL_LOG_CAPACITY];
	size_t                   log_count;
};

int ov5647_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value)
{
	struct ov5647_emul_data *data = target->data;

	if (reg >= OV5647_EMUL_REG_MAP_SIZE || value == NULL) {
		return -EINVAL;
	}

	*value = data->regs[reg];

	return 0;
}

void ov5647_emul_clear_log(const struct emul *target)
{
	struct ov5647_emul_data *data = target->data;

	data->log_count = 0;
}

size_t ov5647_emul_log_count(const struct emul *target)
{
	struct ov5647_emul_data *data = target->data;

	return data->log_count;
}

int ov5647_emul_log_get(const struct emul *target, size_t index, struct ov5647_emul_write *out)
{
	struct ov5647_emul_data *data = target->data;

	if (index >= data->log_count || out == NULL) {
		return -EINVAL;
	}

	*out = data->log[index];

	return 0;
}

static void ov5647_emul_log_write(struct ov5647_emul_data *data, uint16_t reg, uint8_t value)
{
	if (data->log_count >= OV5647_EMUL_LOG_CAPACITY) {
		LOG_WRN("Write log full (%d entries); dropping 0x%04x=0x%02x",
		        OV5647_EMUL_LOG_CAPACITY,
		        reg,
		        value);
		return;
	}

	data->log[data->log_count].reg   = reg;
	data->log[data->log_count].value = value;
	data->log_count++;
}

static int
ov5647_emul_transfer_i2c(const struct emul *target, struct i2c_msg msgs[], int num_msgs, int addr)
{
	struct ov5647_emul_data *data = target->data;
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
		if (reg >= OV5647_EMUL_REG_MAP_SIZE) {
			LOG_ERR("Register 0x%04x out of the emulated map", reg);
			return -EIO;
		}
		data->regs[reg] = msgs[0].buf[2];
		ov5647_emul_log_write(data, reg, msgs[0].buf[2]);
		return 0;
	}

	/* video_read_cci_reg(): a 2-byte address write, then a 1-byte read */
	if (msgs[0].len != 2 || !(msgs[1].flags & I2C_MSG_READ) || msgs[1].len != 1) {
		LOG_ERR("Expected a 2-byte address write + 1-byte read");
		return -EIO;
	}
	reg = sys_get_be16(msgs[0].buf);
	if (reg >= OV5647_EMUL_REG_MAP_SIZE) {
		LOG_ERR("Register 0x%04x out of the emulated map", reg);
		return -EIO;
	}
	msgs[1].buf[0] = data->regs[reg];

	return 0;
}

static int ov5647_emul_init(const struct emul *target, const struct device *parent)
{
	struct ov5647_emul_data *data = target->data;

	ARG_UNUSED(parent);

	memset(data->regs, 0, sizeof(data->regs));
	data->log_count = 0;

	/* OV5647_CHIP_ID_REG = OV5647_REG16(0x300a): a 2-byte big-endian read starting at 0x300a,
	 * byte-by-byte per video_read_cci_reg() -- so 0x300a holds the high byte. */
	data->regs[0x300a] = 0x56;
	data->regs[0x300b] = 0x47;

	return 0;
}

static const struct i2c_emul_api ov5647_emul_api_i2c = {
	.transfer = ov5647_emul_transfer_i2c,
};

#define OV5647_EMUL(n) \
	static struct ov5647_emul_data ov5647_emul_data_##n; \
	EMUL_DT_INST_DEFINE( \
	    n, ov5647_emul_init, &ov5647_emul_data_##n, NULL, &ov5647_emul_api_i2c, NULL)

DT_INST_FOREACH_STATUS_OKAY(OV5647_EMUL)
