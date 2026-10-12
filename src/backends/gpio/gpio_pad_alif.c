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
 * The pad word (which bits an input gets) is in gpio_pad_alif_word.h.
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
#include "gpio_pad_alif_word.h"

#define ALP_ALIF_GPIO_BASE   0x49000000u
#define ALP_ALIF_LPGPIO_BASE 0x42002000u
#define ALP_ALIF_LPGPIO_PORT 15u

/* The word layout lives in gpio_pad_alif_word.h (testable on the host); pin it to
 * the real pinctrl_soc.h here so it cannot drift. */
BUILD_ASSERT(ALP_PAD_REN == ALIF_PAD_CONF_REN(1));
BUILD_ASSERT(ALP_PAD_ODS_4MA == ALIF_PAD_CONF_ODS(1));
BUILD_ASSERT((ALP_PAD_DSC_UP << ALP_PAD_DSC_SHIFT) == ALIF_PAD_CONF_DSC(1));
BUILD_ASSERT((ALP_PAD_DSC_DOWN << ALP_PAD_DSC_SHIFT) == ALIF_PAD_CONF_DSC(2));
BUILD_ASSERT(ALIF_PINMUX(3, 5, 0) == ((3u << ALP_PAD_PORT_SHIFT) | (5u << ALP_PAD_PIN_SHIFT)));

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

		/* Opening the pad as a GPIO input is the caller's choice to own it, so taking
		 * the mux is correct.  Which pad bits an input gets: see the header. */
		pinctrl_soc_pin_t pad = alp_alif_pad_input_word(_ports[i].port, spec->pin, pull);
		return pinctrl_configure_pins(&pad, 1, PINCTRL_REG_NONE);
	}
	return 0; /* not an Alif DW port (e.g. an emulated controller) */
}
