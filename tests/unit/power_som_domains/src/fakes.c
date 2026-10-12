/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fakes for the SoM power-domain test: an emulated register-file I2C device
 * for the DT-addressed TMP112 / RV-3028 default path, the CC3501E driver
 * entry points the adapter calls, and the alp_gpio / alp_i2c / alp_delay_ms
 * calls the adapters and the TMP112 chip driver make.
 */

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>

#include <alp/chips/cc3501e.h>
#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>

#include "fakes.h"

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

/* ---- SoM power layer seams ------------------------------------------------------- */

unsigned int g_pads_calls;
alp_status_t g_pads_rc = ALP_OK;
uint32_t     g_stop_mode;

alp_status_t alp_som_power_pads_apply(void)
{
	g_pads_calls++;
	return g_pads_rc;
}

/* Strong definition: the emulator keeps a driven output apart from its input side,
 * so read the output latch the way a real pad with an input buffer reads back.
 * g_pad_stuck_pin >= 0 makes that one pin read the OPPOSITE of what was driven. */
int g_pad_stuck_pin = -1;

int alp_som_power_pad_read(const struct gpio_dt_spec *s)
{
	int phys = gpio_emul_output_get(s->port, s->pin);

	if (phys < 0) {
		return phys;
	}
	int logical = ((s->dt_flags & GPIO_ACTIVE_LOW) != 0U) ? !phys : phys;

	return (g_pad_stuck_pin == (int)s->pin) ? !logical : logical;
}

/* Strong definition: replaces the weak register read in som_power.c. */
uint32_t alp_som_power_stop_mode_read(void)
{
	return g_stop_mode;
}

alp_status_t cc3501e_ping(cc3501e_t *ctx)
{
	g_cc.ping_calls++;
	if (!ctx->initialised || g_cc.ping_always_fail) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

void fakes_reset(void)
{
	g_pad_stuck_pin = -1;
	g_pads_calls    = 0;
	g_pads_rc       = ALP_OK;
	g_stop_mode     = 0x10u;
	memset(&g_cc, 0, sizeof(g_cc));
	g_clkout_calls = 0;
	g_clkout_src   = -1;
	memset(&g_fw, 0, sizeof(g_fw));
	g_fw.reset_pin   = g_nrst_pin;
	g_fw.enable_pin  = g_en_pin;
	g_fw.initialised = true;
	memset(g_chip_regs, 0, sizeof(g_chip_regs));
}
