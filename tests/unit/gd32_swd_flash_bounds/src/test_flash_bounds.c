/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression tests for the gd32_swd flash addr/size wrap guard (#471,
 * tracked by #563): `addr + size` (erase) and `addr + offset` (write /
 * verify) are caller-supplied and were only checked against the lower
 * bound (`addr >= GD32_SWD_FMC_FLASH_BASE`) -- a near-UINT32_MAX size/len
 * let the additive bound wrap past the flash-capacity check entirely.
 *
 * These tests construct the driver context directly (bypassing
 * gd32_swd_init/gd32_swd_connect) and drive only the rejection path: the
 * fixed guard returns ALP_ERR_INVAL before any GPIO/SWD transaction runs,
 * so the alp_gpio_* stubs below exist only to satisfy the link, never to
 * be called by these cases.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/gd32_swd.h"

/* ---- alp_gpio_* link seam: satisfies gd32_swd.c's other (unreached by
 * these tests) code paths.  Never expected to run here -- ztest fails
 * loudly if it does. */
alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	ARG_UNUSED(pin);
	ARG_UNUSED(dir);
	ARG_UNUSED(pull);
	zassert_unreachable("bound-rejected call must not reach GPIO configure");
	return ALP_OK;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	ARG_UNUSED(pin);
	ARG_UNUSED(level);
	zassert_unreachable("bound-rejected call must not reach GPIO write");
	return ALP_OK;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	ARG_UNUSED(pin);
	*level = false;
	zassert_unreachable("bound-rejected call must not reach GPIO read");
	return ALP_OK;
}

static gd32_swd_t open_ctx(void)
{
	gd32_swd_t ctx;
	memset(&ctx, 0, sizeof ctx);
	ctx.initialised = true;
	ctx.connected   = true;
	return ctx;
}

ZTEST(gd32_swd_flash_bounds, test_erase_rejects_wrapped_size)
{
	gd32_swd_t ctx = open_ctx();

	/* addr near UINT32_MAX, size small: addr + size wraps to a tiny
	 * value that the old (upper-bound-free) check let straight
	 * through. */
	const alp_status_t s = gd32_swd_flash_erase(&ctx, 0xFFFFFFF0u, 0x20u);
	zassert_equal(s, ALP_ERR_INVAL, "wrapped erase range must be rejected");
}

ZTEST(gd32_swd_flash_bounds, test_erase_rejects_oversized_length)
{
	gd32_swd_t ctx = open_ctx();

	const alp_status_t s = gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 0xFFFFFFFFu);
	zassert_equal(s, ALP_ERR_INVAL, "size beyond flash capacity must be rejected");
}

ZTEST(gd32_swd_flash_bounds, test_write_rejects_wrapped_len)
{
	gd32_swd_t ctx     = open_ctx();
	uint8_t    data[8] = { 0 };

	/* addr doubleword-aligned, near UINT32_MAX; len = 8: addr + len
	 * wraps to 0 in uint32_t arithmetic. */
	const alp_status_t s = gd32_swd_flash_write(&ctx, 0xFFFFFFF8u, data, sizeof data);
	zassert_equal(s, ALP_ERR_INVAL, "wrapped write range must be rejected");
}

ZTEST(gd32_swd_flash_bounds, test_write_rejects_oversized_len)
{
	gd32_swd_t ctx     = open_ctx();
	uint8_t    data[8] = { 0 };

	const alp_status_t s =
	    gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, data, (size_t)0xFFFFFFFFu);
	zassert_equal(s, ALP_ERR_INVAL, "len beyond flash capacity must be rejected");
}

ZTEST(gd32_swd_flash_bounds, test_verify_rejects_wrapped_len)
{
	gd32_swd_t ctx     = open_ctx();
	uint8_t    data[4] = { 0 };

	/* addr word-aligned, near UINT32_MAX; len = 4: addr + len wraps. */
	const alp_status_t s = gd32_swd_flash_verify(&ctx, 0xFFFFFFFCu, data, sizeof data);
	zassert_equal(s, ALP_ERR_INVAL, "wrapped verify range must be rejected");
}

ZTEST_SUITE(gd32_swd_flash_bounds, NULL, NULL, NULL, NULL, NULL);
