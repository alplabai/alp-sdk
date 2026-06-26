/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Quectel BG77 driver (UART AT shell).  See <alp/chips/quectel_bg77.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/quectel_bg77.h"

alp_status_t
quectel_bg77_init(quectel_bg77_t *dev, alp_uart_t *port, alp_gpio_t *pwrkey, alp_gpio_t *reset)
{
	if (dev == NULL || port == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->port        = port;
	dev->pwrkey      = pwrkey;
	dev->reset       = reset;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t quectel_bg77_power_on(quectel_bg77_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->pwrkey == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t s = alp_gpio_write(dev->pwrkey, true);
	if (s != ALP_OK) return s;
	alp_delay_us(500000);
	return alp_gpio_write(dev->pwrkey, false);
}

alp_status_t quectel_bg77_send_cmd(quectel_bg77_t *dev, const char *at_cmd)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (at_cmd == NULL) return ALP_ERR_INVAL;
	const size_t len = strlen(at_cmd);
	alp_status_t s   = alp_uart_write(dev->port, (const uint8_t *)at_cmd, len);
	if (s != ALP_OK) return s;
	const bool needs_crlf = len < 2 || at_cmd[len - 1] != '\n';
	if (needs_crlf) {
		static const uint8_t crlf[2] = { '\r', '\n' };
		s                            = alp_uart_write(dev->port, crlf, sizeof(crlf));
	}
	return s;
}

alp_status_t quectel_bg77_read_response(
    quectel_bg77_t *dev, uint8_t *buf, size_t max, size_t *received_out, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (buf == NULL || max == 0) return ALP_ERR_INVAL;
	alp_status_t s = alp_uart_read(dev->port, buf, max, timeout_ms);
	if (s == ALP_OK && received_out != NULL) *received_out = max;
	return s;
}

void quectel_bg77_deinit(quectel_bg77_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->port        = NULL;
	dev->pwrkey      = NULL;
	dev->reset       = NULL;
}
