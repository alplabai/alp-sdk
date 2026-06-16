/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Trinamic TMC2209 silent stepper driver (UART side).
 * See <alp/chips/tmc2209.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tmc2209.h"

#define TMC2209_SYNC_BYTE 0x05u
#define TMC2209_READ_FLAG 0x00u
#define TMC2209_WRITE_FLAG 0x80u

/* Trinamic UART CRC-8/ATM (poly 0x07, init 0). */
static uint8_t tmc2209_crc(const uint8_t *buf, size_t len)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		uint8_t b = buf[i];
		for (uint8_t j = 0; j < 8; j++) {
			if (((crc >> 7) ^ (b & 0x01)) != 0)
				crc = (uint8_t)((crc << 1) ^ 0x07);
			else
				crc <<= 1;
			b >>= 1;
		}
	}
	return crc;
}

alp_status_t tmc2209_init(tmc2209_t *dev, alp_uart_t *port, uint8_t slave_addr)
{
	if (dev == NULL || port == NULL) return ALP_ERR_INVAL;
	if (slave_addr > 3) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->port        = port;
	dev->slave_addr  = slave_addr;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t tmc2209_read_reg(tmc2209_t *dev, uint8_t reg, uint32_t *val_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (val_out == NULL) return ALP_ERR_INVAL;

	uint8_t req[4] = { TMC2209_SYNC_BYTE, dev->slave_addr, (uint8_t)(reg | TMC2209_READ_FLAG), 0 };
	req[3]         = tmc2209_crc(req, 3);
	alp_status_t s = alp_uart_write(dev->port, req, sizeof(req));
	if (s != ALP_OK) return s;

	/* Reply: 8 bytes including sync/master-addr/reg/4-byte data/crc. */
	uint8_t reply[8] = { 0 };
	s                = alp_uart_read(dev->port, reply, sizeof(reply), 50u);
	if (s != ALP_OK) return s;
	if (reply[0] != TMC2209_SYNC_BYTE) return ALP_ERR_IO;
	if (tmc2209_crc(reply, 7) != reply[7]) return ALP_ERR_IO;

	*val_out = ((uint32_t)reply[3] << 24) | ((uint32_t)reply[4] << 16) | ((uint32_t)reply[5] << 8) |
	           (uint32_t)reply[6];
	return ALP_OK;
}

alp_status_t tmc2209_write_reg(tmc2209_t *dev, uint8_t reg, uint32_t val)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;

	uint8_t req[8] = { TMC2209_SYNC_BYTE,
		               dev->slave_addr,
		               (uint8_t)(reg | TMC2209_WRITE_FLAG),
		               (uint8_t)(val >> 24),
		               (uint8_t)(val >> 16),
		               (uint8_t)(val >> 8),
		               (uint8_t)val,
		               0 };
	req[7]         = tmc2209_crc(req, 7);
	return alp_uart_write(dev->port, req, sizeof(req));
}

void tmc2209_deinit(tmc2209_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->port        = NULL;
}
