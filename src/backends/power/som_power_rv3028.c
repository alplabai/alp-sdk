/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * RTC power-domain hook through the RV-3028 chip driver (#2784, U5).
 *
 * The RV-3028 is the primary RTC and the primary timed-wake source: it is
 * never powered down (KEEP_ALIVE).  The only quiesce action is CLKOUT low,
 * through rv3028c7_route_clkout(); its endurance guard skips the EEPROM
 * commit once CLKOUT already reads low.  CONTROL_1.EERD (a RAM bit) is then set so
 * the chip's 24 h EEPROM -> mirror refresh cannot switch CLKOUT back on during
 * the sleep, and cleared again on restore.  Nothing else is restored: the alarm /
 * countdown-timer interrupt state belongs to the wake path, not here.
 */

#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

#define RV3028_REG_CONTROL_1 0x0Fu
#define RV3028_CTRL1_EERD    0x08u

static alp_status_t eerd(rv3028c7_t *ctx, bool pause)
{
	uint8_t      reg = RV3028_REG_CONTROL_1;
	uint8_t      v   = 0;
	alp_status_t s   = alp_i2c_write_read(ctx->bus, RV3028C7_I2C_ADDR, &reg, 1, &v, 1);

	if (s != ALP_OK) {
		return s;
	}
	v = pause ? (uint8_t)(v | RV3028_CTRL1_EERD) : (uint8_t)(v & ~RV3028_CTRL1_EERD);

	uint8_t buf[2] = { RV3028_REG_CONTROL_1, v };

	return alp_i2c_write(ctx->bus, RV3028C7_I2C_ADDR, buf, sizeof(buf));
}

static alp_status_t rtc_quiesce(void *ctx, bool rail_off)
{
	(void)rail_off;
	/* route_clkout() leaves EERD as it found it, so pause the refresh AFTER it. */
	alp_status_t s = rv3028c7_route_clkout((rv3028c7_t *)ctx, RV3028C7_CLKOUT_LOW);

	return (s == ALP_OK) ? eerd((rv3028c7_t *)ctx, true) : s;
}

static alp_status_t rtc_restore(void *ctx, bool rail_off, bool early)
{
	(void)rail_off;
	(void)early;
	return eerd((rv3028c7_t *)ctx, false);
}

static const alp_som_power_hooks_t _hooks = {
	.quiesce = rtc_quiesce,
	.restore = rtc_restore,
};

alp_status_t alp_som_power_bind_rv3028(rv3028c7_t *ctx)
{
	if (ctx == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_som_power_bind(ALP_POWER_DOMAIN_RTC, &_hooks, ctx);
}
