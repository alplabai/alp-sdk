/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fakes for the SoM power-domain test: an emulated register-file I2C device
 * for the DT-addressed TMP112 / RV-3028 default path, the CC3501E driver
 * entry points the adapter calls, and the alp_gpio / alp_i2c / alp_delay_ms
 * calls the adapters and the TMP112 chip driver make.
 */

#define DT_DRV_COMPAT vnd_som_fake

#include <string.h>

#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>

#include <alp/chips/cc3501e.h>
#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>

#include "fakes.h"

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

/* ---- CC3501E driver fakes --------------------------------------------------- */

struct fake_cc g_cc;
cc3501e_t      g_fw;

static int        g_nrst_obj, g_en_obj;
alp_gpio_t *const g_nrst_pin = (alp_gpio_t *)&g_nrst_obj;
alp_gpio_t *const g_en_pin   = (alp_gpio_t *)&g_en_obj;

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	if (pin == g_nrst_pin) {
		g_cc.nrst_level   = level;
		g_cc.nrst_written = true;
	} else if (pin == g_en_pin) {
		g_cc.en_level   = level;
		g_cc.en_written = true;
	}
	return ALP_OK;
}

alp_status_t cc3501e_hard_reset(cc3501e_t *ctx)
{
	(void)ctx;
	g_cc.hard_reset_calls++;
	g_cc.nrst_level = true; /* the real one pulses nRESET and leaves it released */
	return ALP_OK;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
	(void)ctx;
	g_cc.reset_calls++;
	return ALP_OK;
}

alp_status_t cc3501e_power_off(cc3501e_t *ctx)
{
	g_cc.power_off_calls++;
	ctx->initialised = false;
	return ALP_OK;
}

/* ---- RV-3028 driver fake ------------------------------------------------------ */

unsigned int g_clkout_calls;
int          g_clkout_src = -1;

alp_status_t rv3028c7_route_clkout(rv3028c7_t *ctx, rv3028c7_clkout_src_t src)
{
	(void)ctx;
	g_clkout_calls++;
	g_clkout_src = (int)src;
	return ALP_OK;
}

void alp_delay_ms(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

/* ---- alp_i2c fake for the TMP112 chip driver --------------------------------- */

uint8_t g_chip_regs[256];

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)bus;
	(void)addr;
	for (size_t i = 1; i < len; ++i) {
		g_chip_regs[(uint8_t)(data[0] + i - 1)] = data[i];
	}
	return ALP_OK;
}

alp_status_t alp_i2c_write_read(alp_i2c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *wdata,
                                size_t         wlen,
                                uint8_t       *rdata,
                                size_t         rlen)
{
	(void)bus;
	(void)addr;
	(void)wlen;
	for (size_t i = 0; i < rlen; ++i) {
		rdata[i] = g_chip_regs[(uint8_t)(wdata[0] + i)];
	}
	return ALP_OK;
}

void fakes_reset(void)
{
	memset(&g_cc, 0, sizeof(g_cc));
	g_clkout_calls = 0;
	g_clkout_src   = -1;
	memset(&g_fw, 0, sizeof(g_fw));
	g_fw.reset_pin   = g_nrst_pin;
	g_fw.enable_pin  = g_en_pin;
	g_fw.initialised = true;
	memset(g_chip_regs, 0, sizeof(g_chip_regs));
}
