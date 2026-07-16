/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TAS2563 smart Class-D speaker amp driver -- v0.3 thin
 * scope (init / probe / mode / hw_enable).  See <alp/chips/tas2563.h>
 * for the public API and v0.3.x roadmap.
 */

#include <string.h>

#include "alp/chips/tas2563.h"

/* TAS2563 register map (datasheet Table 7-50, BOOK 0 / PAGE 0). */
#define TAS2563_REG_PAGE      0x00u /* Page select.            */
#define TAS2563_REG_MODE_CTRL 0x02u /* Operating-mode control. */
#define TAS2563_REG_REVID     0x7Du /* Revision ID (RO).       */

/* MODE_CTRL field encoding -- bits 0..1.  Datasheet table 7-58. */
#define TAS2563_MODE_CTRL_MASK 0x07u

static alp_status_t reg_read(tas2563_t *ctx, uint8_t reg, uint8_t *val_out)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val_out, 1);
}

static alp_status_t reg_write(tas2563_t *ctx, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

static alp_status_t select_page(tas2563_t *ctx, uint8_t page)
{
	return reg_write(ctx, TAS2563_REG_PAGE, page);
}

/* Table 7-3: only five 7-bit addresses are wired -- the four
 * AD0/SPICLK strap options plus the global broadcast address.
 * Anything else (including an out-of-range/8-bit-encoded value)
 * cannot correspond to a real strap and is rejected before any bus
 * access. */
static bool addr_is_valid(uint8_t addr)
{
	return addr == TAS2563_I2C_ADDR_BROADCAST ||
	       (addr >= TAS2563_I2C_ADDR_GND_DIRECT && addr <= TAS2563_I2C_ADDR_VDD_DIRECT);
}

alp_status_t tas2563_init(tas2563_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, alp_gpio_t *sd_n)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (!addr_is_valid(addr_7bit)) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = addr_7bit;
	ctx->sd_n = sd_n;

	/* If we own the SD_N pin, drive it high to leave HW shutdown.
     * If sd_n == NULL the caller is managing that line elsewhere
     * (or it's tied permanently asserted on the board). */
	if (sd_n != NULL) {
		alp_status_t s = alp_gpio_configure(sd_n, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
		if (s != ALP_OK) return s;
		s = alp_gpio_write(sd_n, true); /* AMP.ENABLE high -> chip out of HW shutdown */
		if (s != ALP_OK) return s;
	}

	/* I2C connectivity probe via REVID register on PAGE 0. */
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;
	uint8_t rev = 0;
	s           = reg_read(ctx, TAS2563_REG_REVID, &rev);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t tas2563_read_revision(tas2563_t *ctx, uint8_t *rev_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (rev_out == NULL) return ALP_ERR_INVAL;
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	return reg_read(ctx, TAS2563_REG_REVID, rev_out);
}

alp_status_t tas2563_set_mode(tas2563_t *ctx, tas2563_mode_t mode)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	uint8_t cur = 0;
	s           = reg_read(ctx, TAS2563_REG_MODE_CTRL, &cur);
	if (s != ALP_OK) return s;
	cur = (uint8_t)((cur & ~TAS2563_MODE_CTRL_MASK) | ((uint8_t)mode & TAS2563_MODE_CTRL_MASK));
	return reg_write(ctx, TAS2563_REG_MODE_CTRL, cur);
}

alp_status_t tas2563_set_hw_enable(tas2563_t *ctx, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->sd_n == NULL) return ALP_ERR_NOSUPPORT;
	return alp_gpio_write(ctx->sd_n, enable);
}

void tas2563_deinit(tas2563_t *ctx)
{
	if (ctx == NULL) return;
	if (ctx->initialised && ctx->sd_n != NULL) {
		(void)alp_gpio_write(ctx->sd_n, false); /* HW shutdown on close */
	}
	ctx->initialised = false;
	ctx->bus         = NULL;
}
