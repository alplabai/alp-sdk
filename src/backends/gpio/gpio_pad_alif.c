/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the Alif pinctrl driver) ======
 * Alif Ensemble pad input-buffer (REN) enable for portable GPIO / qenc inputs
 * (issue #2808).  Upstream gpio_dw and gpio-qdec never set the pad's REN bit, so
 * an input read through <alp/gpio.h> / <alp/qenc.h> is idle for ever.  This file
 * only BUILDS a pinctrl word and hands it to pinctrl_configure_pins(); no
 * register is touched here.  Retires if upstream gpio_dw learns pinctrl.
 *
 * Pad <-> pin: gpioN is pad port N, lpgpio is pad port 15 (pinctrl_soc.h), the
 * pin number is the same on both.  The port is read off the controller's DT
 * register base (gpioN @ 0x49000000 + N * 0x1000, lpgpio @ 0x42002000) so no
 * per-board data is needed.
 */

#define DT_DRV_COMPAT snps_designware_gpio

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>

#include "gpio_pad.h"

#define ALP_ALIF_GPIO_BASE   0x49000000u
#define ALP_ALIF_LPGPIO_BASE 0x42002000u
#define ALP_ALIF_LPGPIO_PORT 15u

/* DSC bias field, pinctrl_soc.h: 0 = high-Z, 1 = pull-up, 2 = pull-down. */
#define ALP_ALIF_DSC_PULL_UP   1u
#define ALP_ALIF_DSC_PULL_DOWN 2u

struct pad_port {
	const struct device *dev;
	uint8_t              port;
};

#define ALP_PAD_PORT(n) \
	((DT_INST_REG_ADDR(n) == ALP_ALIF_LPGPIO_BASE) \
	     ? ALP_ALIF_LPGPIO_PORT \
	     : ((DT_INST_REG_ADDR(n) - ALP_ALIF_GPIO_BASE) >> 12))

#define ALP_PAD_ENTRY(n) { DEVICE_DT_INST_GET(n), ALP_PAD_PORT(n) },

static const struct pad_port _ports[] = {
	DT_INST_FOREACH_STATUS_OKAY(ALP_PAD_ENTRY){ NULL,
	                                            0 }, /* sentinel; also keeps the array non-empty */
};

int alp_z_gpio_pad_input(const struct gpio_dt_spec *spec, alp_gpio_pull_t pull)
{
	/* The DW controllers are 8 pins wide; a larger pin is refused by the
	 * gpio call that follows, with a proper error. */
	if (spec->pin > 7u) return 0;

	for (size_t i = 0; _ports[i].dev != NULL; ++i) {
		if (_ports[i].dev != spec->port) continue;

		uint32_t dsc = (pull == ALP_GPIO_PULL_UP)     ? ALP_ALIF_DSC_PULL_UP
		               : (pull == ALP_GPIO_PULL_DOWN) ? ALP_ALIF_DSC_PULL_DOWN
		                                              : 0u;
		/* AF 0 = GPIO.  Opening the pad as a GPIO input is the caller's choice
		 * to own it, so taking the mux is correct. */
		pinctrl_soc_pin_t pad = ALIF_PINMUX(_ports[i].port, spec->pin, 0) | ALIF_PAD_CONF_REN(1) |
		                        ALIF_PAD_CONF_DSC(dsc);
		return pinctrl_configure_pins(&pad, 1, PINCTRL_REG_NONE);
	}
	return 0; /* not an Alif DW port (e.g. an emulated controller) */
}
