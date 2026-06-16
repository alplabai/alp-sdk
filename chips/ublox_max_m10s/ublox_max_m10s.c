/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * u-blox MAX-M10S GNSS driver (UART NMEA).
 * See <alp/chips/ublox_max_m10s.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ublox_max_m10s.h"

alp_status_t ublox_max_m10s_init(ublox_max_m10s_t *dev, alp_uart_t *port)
{
	if (dev == NULL || port == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->port        = port;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t ublox_max_m10s_read_nmea_line(ublox_max_m10s_t *dev, uint8_t *line_buf,
                                           size_t line_max, size_t *len_out, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (line_buf == NULL || line_max < 2) return ALP_ERR_INVAL;
	size_t pos = 0;
	while (pos + 1 < line_max) {
		uint8_t      b = 0;
		alp_status_t s = alp_uart_read(dev->port, &b, 1, timeout_ms);
		if (s != ALP_OK) return s;
		line_buf[pos++] = b;
		if (b == '\n') break;
	}
	line_buf[pos] = '\0';
	if (len_out != NULL) *len_out = pos;
	return ALP_OK;
}

void ublox_max_m10s_deinit(ublox_max_m10s_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->port        = NULL;
}
