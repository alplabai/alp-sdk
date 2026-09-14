/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Strong override of the WEAK cc3501e_gpio_routes[] / cc3501e_gpio_route_count
 * in src/backends/gpio/cc3501e_proxy_routes_weak.c (empty by default). This
 * app needs exactly ONE proxied pin -- the SDIO mux ENABLE -- so it carries
 * one entry rather than the full generated table
 * (examples/aen/aen-cc3501e-gpio/src/cc3501e_gpio_routes.c and siblings) --
 * see that file's own header for the generator + the other AEN801 routes.
 *
 * ALP_E1M_GPIO_IO20 -> raw CC3501E GPIO_26 -- the SDIO 74LVC157 (now
 * 74LV3257 on the reworked bench board) mux ENABLE, active low.
 * metadata/e1m_modules/E1M-AEN801.yaml:356.
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO20, 26u }, /* SDIO mux /E; drive low to enable mux. */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
