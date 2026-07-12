/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E GPIO proxy host helpers (0x50..0x53).  See
 * <alp/chips/cc3501e/gpio.h> for the public API.
 *
 * These ops are synchronous + fast in the firmware (no worker, no
 * radio bring-up), so they take the caller's timeout with no down-
 * window floor.  poll_by_repeat still absorbs a transient ALP_ERR_IO
 * if a radio op happens to overlap (the bridge is briefly down then).
 * pad = the RAW CC3501E GPIO index; the firmware drives it 1:1 and
 * refuses its own SPI/UART pads, so the logical IO11.. -> raw map can
 * live entirely host-side in board metadata.
 */

#include <stdint.h>
#include <stdbool.h>

#include "cc3501e_internal.h"

alp_status_t cc3501e_gpio_configure(cc3501e_t                   *ctx,
                                    uint8_t                      pad,
                                    alp_cc3501e_gpio_direction_t dir,
                                    alp_cc3501e_gpio_pull_t      pull,
                                    uint32_t                     timeout_ms)
{
	alp_cc3501e_gpio_configure_t c = {
		.cc3501e_gpio = pad,
		.direction    = (uint8_t)dir,
		.pull         = (uint8_t)pull,
		.reserved     = 0u,
	};
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_GPIO_CONFIGURE,
	                      (const uint8_t *)&c,
	                      sizeof(c),
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}

alp_status_t cc3501e_gpio_write(cc3501e_t *ctx, uint8_t pad, bool level, uint32_t timeout_ms)
{
	alp_cc3501e_gpio_write_t w = {
		.cc3501e_gpio = pad,
		.level        = level ? 1u : 0u,
		.reserved     = { 0u, 0u },
	};
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_GPIO_WRITE, (const uint8_t *)&w, sizeof(w), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_gpio_read(cc3501e_t *ctx, uint8_t pad, bool *level_out, uint32_t timeout_ms)
{
	if (level_out == NULL) return ALP_ERR_INVAL;
	uint8_t      req      = pad;
	uint8_t      reply[1] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_GPIO_READ, &req, sizeof(req), reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < 1u) return ALP_ERR_IO;
	*level_out = (reply[0] != 0u);
	return ALP_OK;
}

alp_status_t cc3501e_gpio_set_interrupt(cc3501e_t              *ctx,
                                        uint8_t                 pad,
                                        alp_cc3501e_gpio_edge_t edge,
                                        bool                    enabled,
                                        uint32_t                timeout_ms)
{
	alp_cc3501e_gpio_set_interrupt_t s = {
		.cc3501e_gpio = pad,
		.edge         = (uint8_t)edge,
		.enabled      = enabled ? 1u : 0u,
		.reserved     = 0u,
	};
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_GPIO_SET_INTERRUPT,
	                      (const uint8_t *)&s,
	                      sizeof(s),
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}
