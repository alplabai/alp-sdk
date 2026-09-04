/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GoodDisplay GDEW0154T8 1.54" e-paper driver (SPI).
 * See <alp/chips/gdew0154t8.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/gdew0154t8.h"
#include "alp/peripheral.h"

alp_status_t gdew0154t8_write_cmd(gdew0154t8_t *dev, uint8_t cmd)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = alp_gpio_write(dev->dc, false);
	if (s != ALP_OK) return s;
	return alp_spi_write(dev->bus, &cmd, 1);
}

alp_status_t gdew0154t8_write_data(gdew0154t8_t *dev, const uint8_t *data, size_t len)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	alp_status_t s = alp_gpio_write(dev->dc, true);
	if (s != ALP_OK) return s;
	return alp_spi_write(dev->bus, data, len);
}

alp_status_t gdew0154t8_wait_idle(gdew0154t8_t *dev, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->busy == NULL) {
		/* No busy pin: fall back to a fixed timing wait.  Sleeps, because a
		 * 100 ms non-yielding spin (alp_delay_us) would hold the core through
		 * a refresh nothing else is waiting on. */
		alp_delay_ms(100);
		return ALP_OK;
	}
	/* The 10 ms poll step sleeps so this releases the core for the seconds a
	 * full e-paper refresh can take.  waited_ms counts NOMINAL step time, and a
	 * sleep may overshoot to the OS tick boundary, so timeout_ms bounds the
	 * accounted wait, not wall clock. */
	uint32_t waited_ms = 0;
	while (waited_ms < timeout_ms) {
		bool         level = false;
		alp_status_t s     = alp_gpio_read(dev->busy, &level);
		if (s != ALP_OK) return s;
		if (!level) return ALP_OK;
		alp_delay_ms(10);
		waited_ms += 10;
	}
	return ALP_ERR_TIMEOUT;
}

alp_status_t gdew0154t8_hw_reset(gdew0154t8_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->reset == NULL) return ALP_ERR_NOSUPPORT;
	/* 10 ms low + 10 ms post-release settle.  Both sleep: alp_delay_us does
	 * not yield (include/alp/peripheral.h) and neither hold is bus timing
	 * that a scheduling gap could break. */
	alp_status_t s = alp_gpio_write(dev->reset, false);
	if (s != ALP_OK) return s;
	alp_delay_ms(10);
	s = alp_gpio_write(dev->reset, true);
	if (s != ALP_OK) return s;
	alp_delay_ms(10);
	return ALP_OK;
}

alp_status_t gdew0154t8_init(gdew0154t8_t *dev,
                             alp_spi_t    *spi,
                             alp_gpio_t   *dc,
                             alp_gpio_t   *reset,
                             alp_gpio_t   *busy)
{
	if (dev == NULL || spi == NULL || dc == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = spi;
	dev->dc          = dc;
	dev->reset       = reset;
	dev->busy        = busy;
	dev->initialised = true;

	if (reset != NULL) {
		alp_status_t s = gdew0154t8_hw_reset(dev);
		if (s != ALP_OK) {
			dev->initialised = false;
			return s;
		}
	}
	return ALP_OK;
}

void gdew0154t8_deinit(gdew0154t8_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
	dev->dc          = NULL;
	dev->reset       = NULL;
	dev->busy        = NULL;
}
