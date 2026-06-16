/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Avia Semi HX711 24-bit load-cell ADC driver (bit-banged).
 * See <alp/chips/hx711.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/hx711.h"

alp_status_t hx711_init(hx711_t *dev, alp_gpio_t *sck, alp_gpio_t *dout, hx711_gain_t gain)
{
	if (dev == NULL || sck == NULL || dout == NULL) return ALP_ERR_INVAL;
	if (gain != HX711_GAIN_128 && gain != HX711_GAIN_32 && gain != HX711_GAIN_64)
		return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->sck         = sck;
	dev->dout        = dout;
	dev->gain        = gain;
	dev->initialised = true;
	/* Bring SCK low; chip starts in power-down if SCK is high >60us. */
	(void)alp_gpio_write(sck, false);
	return ALP_OK;
}

alp_status_t hx711_wait_ready(hx711_t *dev, uint32_t timeout_ms)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	uint32_t waited_ms = 0;
	while (waited_ms < timeout_ms) {
		bool         level = true;
		alp_status_t s     = alp_gpio_read(dev->dout, &level);
		if (s != ALP_OK) return s;
		if (!level) return ALP_OK; /* DOUT low = chip ready. */
		alp_delay_us(1000);
		waited_ms++;
	}
	return ALP_ERR_TIMEOUT;
}

alp_status_t hx711_read_raw(hx711_t *dev, int32_t *value_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (value_out == NULL) return ALP_ERR_INVAL;

	uint32_t raw = 0;
	/* Shift in 24 MSB-first data bits. */
	for (uint8_t i = 0; i < 24; i++) {
		alp_status_t s = alp_gpio_write(dev->sck, true);
		if (s != ALP_OK) return s;
		alp_delay_us(1);
		bool level = false;
		s          = alp_gpio_read(dev->dout, &level);
		if (s != ALP_OK) return s;
		s = alp_gpio_write(dev->sck, false);
		if (s != ALP_OK) return s;
		alp_delay_us(1);
		raw = (raw << 1) | (level ? 1u : 0u);
	}
	/* Trailing pulses to latch the next-conversion gain (1, 2, or 3). */
	const uint8_t extra = (uint8_t)((uint8_t)dev->gain - 24u);
	for (uint8_t i = 0; i < extra; i++) {
		(void)alp_gpio_write(dev->sck, true);
		alp_delay_us(1);
		(void)alp_gpio_write(dev->sck, false);
		alp_delay_us(1);
	}
	/* Sign-extend the 24-bit value. */
	int32_t s24 = (int32_t)raw;
	if ((raw & 0x800000u) != 0) s24 |= (int32_t)0xFF000000u;
	*value_out = s24;
	return ALP_OK;
}

void hx711_deinit(hx711_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->sck         = NULL;
	dev->dout        = NULL;
}
