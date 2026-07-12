/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: GPIO-proxy command-family handlers
 * (0x50..0x5F).  Split out of protocol.c (issue #461); protocol_dispatch()
 * in protocol.c still owns the single command-family switch that routes
 * here.
 */

#include "protocol_internal.h"

/* GPIO_CONFIGURE (0x50): set direction + pull on a proxied CC3501E pad. */
alp_cc3501e_resp_t handle_gpio_configure(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_configure_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_configure_t *c = (const alp_cc3501e_gpio_configure_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_configure(c->cc3501e_gpio, c->direction, c->pull));
}

/* GPIO_WRITE (0x51): drive a proxied pad (open-drain semantics in the HAL). */
alp_cc3501e_resp_t handle_gpio_write(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_write_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_write_t *w = (const alp_cc3501e_gpio_write_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_write(w->cc3501e_gpio, w->level));
}

/* GPIO_READ (0x52): request payload = 1 byte (cc3501e_gpio); reply data =
 * the sampled level (1 byte). */
alp_cc3501e_resp_t handle_gpio_read(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 1u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t            level = 0u;
	alp_cc3501e_resp_t st    = hw_to_resp(cc3501e_hw_gpio_read(req[0], &level));
	if (st == ALP_CC3501E_RESP_OK) {
		reply_data[0]   = level;
		*reply_data_len = 1u;
	}
	return st;
}

/* GPIO_SET_INTERRUPT (0x53): arm/disable an edge IRQ on a proxied pad.  The
 * async EVT_GPIO_INTERRUPT delivery needs the next-rev host-IRQ line; this
 * rev accepts the config (event delivery lands with r2 -- see DESIGN.md). */
alp_cc3501e_resp_t handle_gpio_set_interrupt(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_set_interrupt_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_set_interrupt_t *s = (const alp_cc3501e_gpio_set_interrupt_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_set_interrupt(s->cc3501e_gpio, s->edge, s->enabled));
}
