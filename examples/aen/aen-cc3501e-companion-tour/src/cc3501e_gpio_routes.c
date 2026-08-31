/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Auto-generated for aen-cc3501e-companion-tour by scripts/gen_cc3501e_gpio_routes.py
 * from metadata/e1m_modules/E1M-AEN801.yaml `pad_routes:` (resolved for
 * hw_rev=r2 -- see metadata/e1m_modules/aen/hw-revisions.yaml
 * `pad_route_overrides:` when board.yaml sets `som.hw_rev:`).
 * DO NOT EDIT BY HAND -- regenerate:
 *   python3 scripts/gen_cc3501e_gpio_routes.py
 *
 * Strong override of the WEAK cc3501e_gpio_routes[] /
 * cc3501e_gpio_route_count in
 * src/backends/gpio/cc3501e_proxy_routes_weak.c.  Maps the portable E1M
 * GPIO pin_id (alp_gpio_open(ALP_E1M_GPIO_IOxx)) to the RAW CC3501E GPIO
 * index the inter-chip bridge drives, so an alp_gpio_* call on a proxied
 * E1M IO is routed over the bridge while the Alif's own pins delegate to
 * the platform driver.
 *
 * Pads whose CC3501E target is bridge-reserved (the transport's own
 * SPI0 / console pads, or the bridge's own READY/host-IRQ + SPI0-CS
 * lines) are never emitted here -- the firmware's gpio_pad_reserved()
 * would refuse them at runtime, so the generator excludes them at
 * generation time instead (issue #1859).
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO8, 30u },  /* I2S0 74LVC157 /E (Alif side P7.1); drive low to enable mux. */
	{ ALP_E1M_GPIO_IO9, 12u },  /* Reset output to the PCIe IO expander. */
	{ ALP_E1M_GPIO_IO10, 35u }, /* Drive high to enable I2C mux to the PCIe slot. */
	{ ALP_E1M_GPIO_IO11, 2u },  /* USB2 TMUXHS221 select: 0 = USB connector, 1 = M.2 E-key USB. */
	{ ALP_E1M_GPIO_IO13, 13u }, /* I2S0 74LVC157 S; 0 = TAS2563 amps, 1 = M.2 E-key I2S. */
	{ ALP_E1M_GPIO_IO15, 14u }, /* BMI323 INT1 (data-ready / motion / FIFO); CC3501E GPIO14. */
	{ ALP_E1M_GPIO_IO18, 18u }, /* M.2 E-key SDIO-path wake (active-low). */
	{ ALP_E1M_GPIO_IO19, 19u }, /* M.2 E-key UART-path wake (active-low). */
	{ ALP_E1M_GPIO_IO20, 26u }, /* SDIO 74LVC157 /E; drive low to enable mux. */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
