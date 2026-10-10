/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Emulated register-file I2C device standing in for the TMP112 / RV-3028 on the
 * BRD_I2C emulator: the DT-addressed default path of the SoM power layer talks
 * to it.  Shared by both power_som_* suites.
 */

#define DT_DRV_COMPAT vnd_som_fake

#include <string.h>

#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

/* ---- Emulated I2C register file -------------------------------------------- */

struct fake_data {
	uint8_t regs[256];
	uint8_t ptr;
};

static int fake_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	struct fake_data *d = target->data;

	(void)addr;
	for (int m = 0; m < num_msgs; ++m) {
		if ((msgs[m].flags & I2C_MSG_READ) != 0) {
			for (uint32_t i = 0; i < msgs[m].len; ++i) {
				msgs[m].buf[i] = d->regs[d->ptr++];
			}
		} else if (msgs[m].len >= 1u) {
			d->ptr = msgs[m].buf[0];
			for (uint32_t i = 1; i < msgs[m].len; ++i) {
				d->regs[d->ptr++] = msgs[m].buf[i];
			}
		}
	}
	return 0;
}

static const struct i2c_emul_api fake_api = { .transfer = fake_transfer };

static int fake_init(const struct emul *target, const struct device *parent)
{
	(void)target;
	(void)parent;
	return 0;
}

/* The emulator references the device of the node it emulates, so each fake
 * node needs a (do-nothing) device of its own. */
#define FAKE_DEFINE(n) \
	static struct fake_data fake_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 99, NULL); \
	EMUL_DT_INST_DEFINE(n, fake_init, &fake_data_##n, NULL, &fake_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(FAKE_DEFINE)

uint8_t *fake_regs(const struct emul *e)
{
	return ((struct fake_data *)e->data)->regs;
}
