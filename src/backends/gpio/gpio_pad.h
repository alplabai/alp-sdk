/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private cross-backend seam (gpio + qenc): make the PAD behind a gpio_dt_spec
 * readable as an input before the pin is used as one.  NOT a public header.
 *
 * Why it exists (issue #2808).  On the Alif Ensemble the pad input buffer (REN)
 * is a pad-config bit that only pinctrl writes; the DesignWare gpio_dw
 * controller sets direction and never touches it, and neither does Zephyr's
 * gpio-qdec.  A pin opened as an input through the portable API therefore reads
 * 0 for ever regardless of the wire.  Upstream gpio_dw takes no pinctrl, so the
 * fix is this small hook, not a driver fork.
 *
 * ADR 0017 Tier-1.5: thin glue over the existing Alif pinctrl driver
 * (pinctrl_configure_pins); implementation in gpio_pad_alif.c.  Where
 * CONFIG_ALP_GPIO_PAD_INPUT is off (every non-Alif SoC) the call is a no-op.
 */

#ifndef ALP_BACKENDS_GPIO_PAD_H
#define ALP_BACKENDS_GPIO_PAD_H

#include <alp/peripheral.h>

struct gpio_dt_spec;

#ifdef CONFIG_ALP_GPIO_PAD_INPUT
/* Mux the pad to GPIO, enable its input buffer and apply `pull` as the pad bias
 * (the controller has no pull of its own).  Returns 0, or a negative errno.
 * A controller this hook does not know is left alone and returns 0. */
int alp_z_gpio_pad_input(const struct gpio_dt_spec *spec, alp_gpio_pull_t pull);
#else
static inline int alp_z_gpio_pad_input(const struct gpio_dt_spec *spec, alp_gpio_pull_t pull)
{
	(void)spec;
	(void)pull;
	return 0;
}
#endif

#endif /* ALP_BACKENDS_GPIO_PAD_H */
