/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Semtech SX1262 LoRa transceiver driver (SPI).
 * See <alp/chips/semtech_sx1262.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/semtech_sx1262.h"

alp_status_t
semtech_sx1262_init(semtech_sx1262_t *dev, alp_spi_t *spi, alp_gpio_t *nreset, alp_gpio_t *busy)
{
	if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = spi;
	dev->nreset      = nreset;
	dev->busy        = busy;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t semtech_sx1262_hw_reset(semtech_sx1262_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->nreset == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t s = alp_gpio_write(dev->nreset, false);
	if (s != ALP_OK) return s;
	alp_delay_us(200); /* NRESET low: 100 us datasheet minimum, sub-ms, spin */
	s = alp_gpio_write(dev->nreset, true);
	if (s != ALP_OK) return s;
	/* Post-reset boot settle.  Sleeps: 10 ms of alp_delay_us would spin
	 * without yielding (include/alp/peripheral.h) and nothing here is bus
	 * timing -- overshooting the settle is harmless. */
	alp_delay_ms(10);
	return ALP_OK;
}

alp_status_t semtech_sx1262_wait_busy(semtech_sx1262_t *dev, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->busy == NULL) {
		/* No BUSY pin: blind 1 ms wait.  Sleeps rather than spins for the
		 * same reason as the poll step below. */
		alp_delay_ms(1);
		return ALP_OK;
	}
	/* The 1 ms poll step sleeps so a long BUSY (a full TX at a low datarate
	 * can hold it for seconds) does not monopolise the core.  waited_ms counts
	 * NOMINAL step time, and a sleep may overshoot to the OS tick boundary, so
	 * timeout_ms bounds the accounted wait, not wall clock. */
	uint32_t waited_ms = 0;
	while (waited_ms < timeout_ms) {
		bool         level = true;
		alp_status_t s     = alp_gpio_read(dev->busy, &level);
		if (s != ALP_OK) return s;
		if (!level) return ALP_OK;
		alp_delay_ms(1);
		waited_ms++;
	}
	return ALP_ERR_TIMEOUT;
}

alp_status_t semtech_sx1262_get_status(semtech_sx1262_t *dev, uint8_t *status_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (status_out == NULL) return ALP_ERR_INVAL;
	uint8_t      tx[2] = { SX1262_OPCODE_GET_STATUS, 0x00 };
	uint8_t      rx[2] = { 0 };
	alp_status_t s     = alp_spi_transceive(dev->bus, tx, rx, sizeof(tx));
	if (s != ALP_OK) return s;
	*status_out = rx[1];
	return ALP_OK;
}

void semtech_sx1262_deinit(semtech_sx1262_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
	dev->nreset      = NULL;
	dev->busy        = NULL;
}
