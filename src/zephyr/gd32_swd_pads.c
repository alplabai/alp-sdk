/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr pad seam for chips/gd32_swd: opens the reserved GD32 pads
 * (GD32G553_PAD_ID_*) through the GPIO dispatcher's INTERNAL opener.  The
 * portable alp_gpio_open() refuses those ids, so application code and the
 * console can never drive SWDIO / SWCLK (P71 = the bridge ATTN pin) or the shared
 * open-drain NRST net; only the SWD driver reaches them, here.
 */

#include "alp/chips/gd32_swd.h"
#include "../backends/gpio/gpio_ops.h"

static alp_gpio_t *pad_open(uint32_t pad_id)
{
	return alp_z_gpio_open_internal(pad_id);
}

static const gd32_swd_platform_t g_pads = {
	.open_pad             = pad_open,
	.configure_output_low = alp_z_gpio_configure_output_low,
	.close_pad            = alp_gpio_close,
};

const gd32_swd_platform_t *gd32_swd_platform(void)
{
	return &g_pads;
}
