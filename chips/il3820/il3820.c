/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Solomon IL3820 4.2" tri-colour e-paper driver (SPI).
 * See <alp/chips/il3820.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/il3820.h"
#include "alp/peripheral.h"

alp_status_t il3820_write_cmd(il3820_t *dev, uint8_t cmd)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = alp_gpio_write(dev->dc, false); /* command */
	if (s != ALP_OK) return s;
	return alp_spi_write(dev->bus, &cmd, 1);
}

alp_status_t il3820_write_data(il3820_t *dev, const uint8_t *data, size_t len)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	alp_status_t s = alp_gpio_write(dev->dc, true); /* data */
	if (s != ALP_OK) return s;
	return alp_spi_write(dev->bus, data, len);
}

alp_status_t il3820_wait_idle(il3820_t *dev, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->busy == NULL) {
		/* No busy pin — caller must use timing-based waits. */
		alp_delay_us(100000);
		return ALP_OK;
	}
	/* Poll BUSY (active high on most IL3820 modules). */
	uint32_t waited_ms = 0;
	while (waited_ms < timeout_ms) {
		bool         level = false;
		alp_status_t s     = alp_gpio_read(dev->busy, &level);
		if (s != ALP_OK) return s;
		if (!level) return ALP_OK;
		alp_delay_us(10000);
		waited_ms += 10;
	}
	return ALP_ERR_TIMEOUT;
}

alp_status_t il3820_hw_reset(il3820_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->reset == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t s = alp_gpio_write(dev->reset, false);
	if (s != ALP_OK) return s;
	alp_delay_us(10000);
	s = alp_gpio_write(dev->reset, true);
	if (s != ALP_OK) return s;
	alp_delay_us(10000);
	return ALP_OK;
}

alp_status_t
il3820_init(il3820_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset, alp_gpio_t *busy)
{
	if (dev == NULL || spi == NULL || dc == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = spi;
	dev->dc          = dc;
	dev->reset       = reset;
	dev->busy        = busy;
	dev->initialised = true;

	if (reset != NULL) {
		alp_status_t s = il3820_hw_reset(dev);
		if (s != ALP_OK) {
			dev->initialised = false;
			return s;
		}
	}
	return ALP_OK;
}

void il3820_deinit(il3820_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
	dev->dc          = NULL;
	dev->reset       = NULL;
	dev->busy        = NULL;
}
