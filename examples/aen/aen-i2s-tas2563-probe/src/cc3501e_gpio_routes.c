/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Strong override of the WEAK cc3501e_gpio_routes[] / cc3501e_gpio_route_count
 * in src/backends/gpio/cc3501e_proxy_routes_weak.c (empty by default). This
 * app needs exactly TWO proxied pins -- the I2S0 mux SELECT and ENABLE -- so
 * it carries two entries rather than the full generated table
 * (examples/aen/aen-cc3501e-gpio/src/cc3501e_gpio_routes.c and siblings) --
 * see that file's own header for the generator + the other AEN801 routes.
 *
 * ALP_E1M_GPIO_IO13 -> raw CC3501E GPIO_13 -- the I2S0 74LVC157 (now 74LV3257
 * on the reworked bench board) mux SELECT; 0 = TAS2563 amps.
 * ALP_E1M_GPIO_IO8  -> raw CC3501E GPIO_30 -- the same mux's ENABLE, active
 * low.
 * metadata/e1m_modules/E1M-AEN801.yaml:336-345.
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO13, 13u }, /* I2S0 mux SELECT; drive low to pick the amps. */
	{ ALP_E1M_GPIO_IO8, 30u },  /* I2S0 mux /E; drive low to enable mux. */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
